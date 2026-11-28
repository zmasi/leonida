//! Static TLS of an executable mapped as a DLL.
//!
//! An exe always gets TLS index 0, so title code reaches its thread-local
//! block as `mov r64, gs:[0x58]` (the thread's TLS array) followed by
//! `[r64]`, without reading `_tls_index`. Mapped as a DLL, the image gets
//! another index. Every such load is turned into a call to a thunk that
//! yields `&array[index]` instead, and the image's `_tls_index` (which CRT
//! code does read, and adds to the array pointer) is set to 0.

use std::ffi::c_void;

use anyhow::{Result, bail};
use leonida_pe::ImageInfo;
use windows_sys::Win32::System::Diagnostics::Debug::FlushInstructionCache;
use windows_sys::Win32::System::Memory::{
    MEM_COMMIT, MEM_RESERVE, PAGE_EXECUTE_READ, PAGE_EXECUTE_READWRITE, PAGE_READWRITE,
    VirtualAlloc, VirtualProtect,
};
use windows_sys::Win32::System::Threading::GetCurrentProcess;

const SCN_MEM_EXECUTE: u32 = 0x2000_0000;
const TLS64_ADDRESS_OF_INDEX: usize = 16;
/// `mov r64, gs:[0x58]` is `65 REX 8B ModRM 25 58 00 00 00`.
const LOAD_LEN: usize = 9;
const THUNK_SIZE: usize = 32;

/// Redirects the image's TLS-array loads if the OS gave it an index other
/// than 0. Returns the number of loads patched.
///
/// # Safety
/// `base` is the image `info` describes, mapped and not yet running.
pub(crate) unsafe fn redirect(base: usize, info: &ImageInfo) -> Result<usize> {
    let Some(tls) = &info.tls else { return Ok(0) };
    if base == 0 {
        bail!("cannot redirect TLS at null image base");
    }
    let image_size = info.size_of_image as usize;
    let image_end = base
        .checked_add(image_size)
        .ok_or_else(|| anyhow::anyhow!("image range overflows address space"))?;
    let index_ptr_end = (tls.directory_rva as usize)
        .checked_add(TLS64_ADDRESS_OF_INDEX + size_of::<usize>())
        .filter(|&end| end <= image_size)
        .ok_or_else(|| anyhow::anyhow!("TLS directory AddressOfIndex lies outside image"))?;
    let index_ptr_slot = base + index_ptr_end - size_of::<usize>();
    // SAFETY: `index_ptr_slot..index_ptr_slot+8` lies within `[base, image_end)`.
    let index_addr = unsafe { (index_ptr_slot as *const usize).read_unaligned() };
    if index_addr < base
        || index_addr
            .checked_add(size_of::<u32>())
            .is_none_or(|end| end > image_end)
    {
        bail!("TLS AddressOfIndex {index_addr:#x} lies outside image [{base:#x}, {image_end:#x})");
    }
    let index_at = index_addr as *mut u32;
    // SAFETY: `index_at..index_at+4` lies within `[base, image_end)`.
    let index = unsafe { index_at.read_unaligned() };
    if index == 0 {
        return Ok(0);
    }
    for section in info
        .sections
        .iter()
        .filter(|s| s.characteristics & SCN_MEM_EXECUTE != 0)
    {
        let end = (section.rva as usize)
            .checked_add(section.virtual_size as usize)
            .ok_or_else(|| anyhow::anyhow!("executable section RVA overflows"))?;
        if end > image_size {
            bail!(
                "executable section {:?} ({:#x}..{end:#x}) exceeds image size {image_size:#x}",
                section.name,
                section.rva
            );
        }
    }
    let thunks = thunks(near(base, image_size)?, index)?;
    let mut patched = 0;
    for section in info
        .sections
        .iter()
        .filter(|s| s.characteristics & SCN_MEM_EXECUTE != 0)
    {
        let len = section.virtual_size as usize;
        if len == 0 {
            continue;
        }
        let start = base + section.rva as usize;
        let mut old = 0;
        // SAFETY: `[start, start + len)` was verified inside `[base, image_end)` above.
        if unsafe { VirtualProtect(start as _, len, PAGE_EXECUTE_READWRITE, &mut old) } == 0 {
            bail!(
                "VirtualProtect(code) failed: {}",
                std::io::Error::last_os_error()
            );
        }
        // SAFETY: writable now, and no title code runs yet.
        let code = unsafe { std::slice::from_raw_parts_mut(start as *mut u8, len) };
        let res = try_patch(code, start, thunks);
        // SAFETY: restoring the protection we changed; flushing our own code.
        unsafe {
            VirtualProtect(start as _, len, old, &mut old);
            FlushInstructionCache(GetCurrentProcess(), start as _, len);
        }
        patched += res?;
    }
    // SAFETY: `index_at` lies in writable mapped image data verified above.
    unsafe { index_at.write_unaligned(0) };
    tracing::info!(index, patched, "static TLS redirected");
    Ok(patched)
}

/// Rewrites each `mov r64, gs:[0x58]` in `code` (at address `at`) as
/// `call thunks[r]; nop4`, failing without modifying `code` if any target is
/// outside signed 32-bit `rel32` reach.
fn try_patch(code: &mut [u8], at: usize, thunks: usize) -> Result<usize> {
    let mut sites = Vec::new();
    let mut i = 0;
    while i + LOAD_LEN <= code.len() {
        let c = &code[i..i + LOAD_LEN];
        let rex = c[1];
        let reg = usize::from((c[3] >> 3) & 7) | if rex & 4 != 0 { 8 } else { 0 };
        // shortcut: a byte match, not a decode. In the title all
        // 53,246 hits are instruction starts and none is missed (checked
        // against a full disassembly); decode .text if a title shows one.
        if c[0] == 0x65
            && (rex == 0x48 || rex == 0x4C)
            && c[2] == 0x8B
            && c[3] & 0xC7 == 0x04
            && c[4..] == [0x25, 0x58, 0, 0, 0]
            && reg != 4
        {
            let Some(target) = thunks.checked_add(reg * THUNK_SIZE) else {
                bail!("TLS thunk target overflows address space");
            };
            let Some(next_ip) = at.checked_add(i).and_then(|p| p.checked_add(5)) else {
                bail!("TLS load site overflows address space");
            };
            let Ok(rel) = i32::try_from(i128::from(target as u64) - i128::from(next_ip as u64))
            else {
                bail!("TLS thunk {target:#x} is out of rel32 reach from {next_ip:#x}");
            };
            sites.push((i, rel));
            i += LOAD_LEN;
        } else {
            i += 1;
        }
    }
    let count = sites.len();
    for (offset, rel) in sites {
        code[offset] = 0xE8;
        code[offset + 1..offset + 5].copy_from_slice(&rel.to_le_bytes());
        code[offset + 5..offset + 9].copy_from_slice(&[0x0F, 0x1F, 0x40, 0x00]);
    }
    Ok(count)
}

#[cfg(test)]
fn patch(code: &mut [u8], at: usize, thunks: usize) -> usize {
    try_patch(code, at, thunks).unwrap_or(0)
}

/// Fills `block` with one thunk per register: `mov r, gs:[0x58];
/// lea r, [r + index * 8]; ret` (flags untouched, like the load it replaces).
fn thunks(block: *mut u8, index: u32) -> Result<usize> {
    if block.is_null() {
        bail!("null TLS thunk block");
    }
    let Ok(disp) = i32::try_from(u64::from(index) * 8) else {
        bail!("TLS slot index {index:#x} exceeds signed 32-bit displacement");
    };
    let disp = disp.to_le_bytes();
    for reg in 0..16u8 {
        let (r, low) = (reg >> 3, reg & 7);
        let mut code = vec![0x65, 0x48 | r << 2, 0x8B, low << 3 | 4, 0x25, 0x58, 0, 0, 0];
        code.extend([0x48 | r << 2 | r, 0x8D, 0x80 | low << 3 | low]);
        if low == 4 {
            code.push(0x24); // SIB: [r12 + disp32]
        }
        code.extend(disp);
        code.push(0xC3);
        // SAFETY: `block` holds 16 thunks and is still writable.
        unsafe {
            std::ptr::copy_nonoverlapping(
                code.as_ptr(),
                block.add(usize::from(reg) * THUNK_SIZE),
                code.len(),
            )
        };
    }
    let mut old = 0;
    // SAFETY: `block` is our allocation of one page.
    if unsafe { VirtualProtect(block as _, 4096, PAGE_EXECUTE_READ, &mut old) } == 0 {
        bail!(
            "VirtualProtect(TLS thunks) failed: {}",
            std::io::Error::last_os_error()
        );
    }
    // SAFETY: flushing our own thunk page.
    unsafe { FlushInstructionCache(GetCurrentProcess(), block as _, 4096) };
    Ok(block as usize)
}

/// One writable page within `rel32` reach of `[base, base + size)`. Leaked:
/// title code calls into it for the life of the process.
fn near(base: usize, size: usize) -> Result<*mut u8> {
    const GRANULE: usize = 0x1_0000;
    // The search spans 1 GiB each side, so every call site stays in reach.
    if size >= 1 << 30 {
        bail!("image too large for TLS thunks within rel32 reach");
    }
    let Some(above) = base
        .checked_add(size)
        .and_then(|end| end.checked_next_multiple_of(GRANULE))
    else {
        bail!("image range overflows address space");
    };
    for k in 0..(1usize << 14) {
        for hint in [
            base.checked_sub((k + 1) * GRANULE),
            above.checked_add(k * GRANULE),
        ] {
            let Some(hint) = hint else { continue };
            // SAFETY: a fresh allocation at a free address, or null.
            let p = unsafe {
                VirtualAlloc(
                    hint as *const c_void,
                    4096,
                    MEM_COMMIT | MEM_RESERVE,
                    PAGE_READWRITE,
                )
            };
            if !p.is_null() {
                return Ok(p.cast());
            }
        }
    }
    bail!("no free page near the image for TLS thunks")
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Reads the thread's TLS array pointer the way title code does.
    fn tls_array() -> usize {
        let array: usize;
        // SAFETY: reads TEB.ThreadLocalStoragePointer.
        unsafe { std::arch::asm!("mov {}, gs:[0x58]", out(reg) array) };
        array
    }

    #[test]
    fn loads_become_calls_that_yield_the_title_slot() {
        let page = near(tls_array as *const () as usize, 0).unwrap();
        let thunks = thunks(near(page as usize, 4096).unwrap(), 3).unwrap();
        // (before the load, REX, ModRM, after the load): each case returns
        // the loaded register in rax; r12 is saved, being callee-saved.
        let cases: [(&[u8], u8, u8, &[u8]); 3] = [
            (&[], 0x48, 0x04, &[0xC3]),                   // rax
            (&[], 0x4C, 0x04, &[0x4C, 0x89, 0xC0, 0xC3]), // r8
            (
                &[0x41, 0x54],
                0x4C,
                0x24,
                &[0x4C, 0x89, 0xE0, 0x41, 0x5C, 0xC3],
            ), // r12
        ];
        for (before, rex, modrm, after) in cases {
            let mut code = before.to_vec();
            code.extend([0x65, rex, 0x8B, modrm, 0x25, 0x58, 0, 0, 0]);
            code.extend(after);
            // SAFETY: the page is ours and writable.
            let bytes = unsafe { std::slice::from_raw_parts_mut(page, code.len()) };
            bytes.copy_from_slice(&code);
            assert_eq!(patch(bytes, page as usize, thunks), 1);
            let mut old = 0;
            // SAFETY: our page, made executable to run the patched code.
            unsafe { VirtualProtect(page as _, 4096, PAGE_EXECUTE_READ, &mut old) };
            // SAFETY: the page holds the patched load and a return.
            let f: extern "system" fn() -> usize = unsafe { std::mem::transmute(page) };
            assert_eq!(f(), tls_array() + 3 * 8, "REX {rex:#x} ModRM {modrm:#x}");
            // SAFETY: our page again.
            unsafe { VirtualProtect(page as _, 4096, PAGE_READWRITE, &mut old) };
        }
    }

    #[test]
    fn redirect_and_thunks_validate_disp32_zero_sections_and_bounds() {
        use leonida_pe::{SectionInfo, TlsInfo};
        use windows_sys::Win32::System::Memory::{MEM_RELEASE, VirtualFree};

        let block = near(tls_array as *const () as usize, 0).unwrap();
        // `0x1000_0000 * 8 == 0x8000_0000`, which sign-extends negative in `lea r, [r + disp32]`.
        assert!(thunks(block, 0x1000_0000).is_err());
        assert!(thunks(block, 0x2000_0000).is_err());
        assert!(thunks(std::ptr::null_mut(), 1).is_err());
        // `0x0fff_ffff * 8 == 0x7fff_fff8 <= i32::MAX` is the largest valid slot.
        assert!(thunks(block, 0x0fff_ffff).is_ok());

        // `patch` must not truncate a >2 GiB displacement (`0x1_0000_0000` -> `0`) into `call $+5`.
        let orig_load: [u8; 9] = [0x65, 0x48, 0x8B, 0x04, 0x25, 0x58, 0, 0, 0];
        let mut far_code = orig_load;
        assert_eq!(patch(&mut far_code, 0x1_0000_0000, 0x2_0000_0005), 0);
        assert_eq!(far_code, orig_load);

        // Allocate 3 pages (12 KiB); expose only the first 2 pages (8 KiB) as `size_of_image`
        // so out-of-image pointers land in our owned 3rd canary page without UB.
        // SAFETY: fresh 3-page read-write allocation freed at the end of the test.
        let alloc = unsafe {
            VirtualAlloc(
                std::ptr::null(),
                3 * 4096,
                MEM_COMMIT | MEM_RESERVE,
                PAGE_READWRITE,
            )
        } as *mut u8;
        assert!(!alloc.is_null());
        let base = alloc as usize;

        let make_info = |sections: Vec<SectionInfo>, directory_rva: u32| ImageInfo {
            machine: 0x8664,
            characteristics: 0x2002,
            timestamp: 0,
            subsystem: 3,
            dll_characteristics: 0,
            image_base: base as u64,
            size_of_image: 8192,
            entry_rva: 0,
            stack_reserve: 0,
            stack_commit: 0,
            sections,
            imports: Vec::new(),
            delay_imports: Vec::new(),
            exports: Vec::new(),
            tls: Some(TlsInfo {
                directory_rva,
                callbacks_va: 0,
                callback_rvas: Vec::new(),
            }),
            has_relocations: false,
            has_exception_table: false,
            has_load_config: false,
            codeview: None,
        };

        // 1. Zero-length executable section (`virtual_size == 0`) alongside a real `.text`
        // section must be skipped instead of failing `VirtualProtect(..., 0, ...)`.
        // Page 0 (`0x0000..0x1000`): TLS directory at `+0x100`, `_tls_index` = 2 at `+0x200`.
        // Page 1 (`0x1000..0x2000`): `.text` containing one `mov rax, gs:[0x58]; ret`.
        // SAFETY: writing within our 3-page allocation.
        unsafe {
            let index_slot = alloc.add(0x200).cast::<u32>();
            index_slot.write(2);
            alloc
                .add(0x100 + TLS64_ADDRESS_OF_INDEX)
                .cast::<usize>()
                .write(index_slot as usize);
            let text_code: [u8; 10] = [0x65, 0x48, 0x8B, 0x04, 0x25, 0x58, 0, 0, 0, 0xC3];
            std::ptr::copy_nonoverlapping(text_code.as_ptr(), alloc.add(0x1000), text_code.len());
            let mut old = 0;
            assert_ne!(
                VirtualProtect(alloc.add(0x1000).cast(), 4096, PAGE_EXECUTE_READ, &mut old),
                0
            );
        }

        let valid_with_empty_exec = make_info(
            vec![
                SectionInfo {
                    name: ".empty".into(),
                    rva: 0x1000,
                    virtual_size: 0,
                    file_offset: 0,
                    file_size: 0,
                    characteristics: SCN_MEM_EXECUTE,
                },
                SectionInfo {
                    name: ".text".into(),
                    rva: 0x1000,
                    virtual_size: 16,
                    file_offset: 0,
                    file_size: 16,
                    characteristics: SCN_MEM_EXECUTE,
                },
            ],
            0x100,
        );
        // SAFETY: `alloc` holds the synthetic mapped image described by `valid_with_empty_exec`.
        assert_eq!(unsafe { redirect(base, &valid_with_empty_exec) }.unwrap(), 1);
        // SAFETY: `_tls_index` was zeroed by `redirect`.
        assert_eq!(unsafe { alloc.add(0x200).cast::<u32>().read() }, 0);

        // 2. `AddressOfIndex` pointing past `size_of_image` (into canary page 2) is rejected
        // without zeroing the canary word.
        // SAFETY: canary page `+0x2000` is within our 3-page allocation.
        unsafe {
            let canary = alloc.add(0x2000).cast::<u32>();
            canary.write(7);
            alloc
                .add(0x100 + TLS64_ADDRESS_OF_INDEX)
                .cast::<usize>()
                .write(canary as usize);
        }
        let oob_index = make_info(Vec::new(), 0x100);
        // SAFETY: `oob_index` points into our owned allocation.
        assert!(unsafe { redirect(base, &oob_index) }.is_err());
        // SAFETY: canary must remain untouched.
        assert_eq!(unsafe { alloc.add(0x2000).cast::<u32>().read() }, 7);

        // 3. Executable section extending past `size_of_image` is rejected before patching.
        // SAFETY: reset `_tls_index` inside image to 2.
        unsafe {
            let index_slot = alloc.add(0x200).cast::<u32>();
            index_slot.write(2);
            alloc
                .add(0x100 + TLS64_ADDRESS_OF_INDEX)
                .cast::<usize>()
                .write(index_slot as usize);
        }
        let oob_section = make_info(
            vec![SectionInfo {
                name: ".oob".into(),
                rva: 0x1000,
                virtual_size: 0x1800,
                file_offset: 0,
                file_size: 0x1800,
                characteristics: SCN_MEM_EXECUTE,
            }],
            0x100,
        );
        // SAFETY: `alloc` owns all 3 pages (`0..0x3000`).
        assert!(unsafe { redirect(base, &oob_section) }.is_err());
        // SAFETY: `_tls_index` must remain 2 when `redirect` fails.
        assert_eq!(unsafe { alloc.add(0x200).cast::<u32>().read() }, 2);

        // SAFETY: freeing our test allocation; no code in it is called again.
        unsafe { VirtualFree(alloc.cast(), 0, MEM_RELEASE) };
    }
}
