//! Turns a title's image into something the Windows loader maps but does
//! not run. Pure byte manipulation, so it is testable on any host.

use leonida_pe::layout::{self, dir, dll_flags, file_flags};
use leonida_pe::{ImageInfo, PeError};

#[derive(Debug, thiserror::Error)]
pub enum PrepareError {
    #[error(transparent)]
    Pe(#[from] PeError),
    #[error("cannot patch image: {0}")]
    Patch(&'static str),
}

/// A patched copy of an image plus everything we need from the original.
#[derive(Debug, Clone)]
pub struct PreparedImage {
    /// Parsed from the *original* bytes: imports, TLS callbacks and entry
    /// point as the title expects them.
    pub info: ImageInfo,
    /// The patched bytes to hand to the Windows loader.
    pub bytes: Vec<u8>,
    /// BLAKE3 of the original bytes: identifies the exact build.
    pub original_hash: blake3::Hash,
}

impl PreparedImage {
    /// File name for the patched copy, unique per original build.
    pub fn file_name(&self) -> String {
        format!("leonida-image-{}.dll", &self.original_hash.to_hex()[..16])
    }
}

/// Produces the patched copy. The changes are:
///
/// * `IMAGE_FILE_DLL` set, entry point cleared: the OS maps it like a DLL
///   and calls nothing;
/// * import directory points to its empty terminator and the bound-import
///   directory is cleared: we bind the original slots using `info.imports`;
/// * TLS callback pointer cleared: the OS still allocates static TLS for
///   the image, but callbacks only run when we call them (after binding);
///   its base relocation is made a no-op so the 0 survives a rebase;
/// * `FORCE_INTEGRITY` cleared, checksum zeroed: the copy is unsigned.
pub fn prepare(original: &[u8]) -> Result<PreparedImage, PrepareError> {
    let info = ImageInfo::parse(original)?;
    let mut bytes = original.to_vec();

    let fh = layout::file_header_offset(&bytes).ok_or(PrepareError::Patch("no file header"))?;
    let opt =
        layout::optional_header_offset(&bytes).ok_or(PrepareError::Patch("no optional header"))?;

    let characteristics = info.characteristics | file_flags::DLL;
    write_u16(
        &mut bytes,
        fh + layout::FILE_HEADER_CHARACTERISTICS,
        characteristics,
    )?;
    write_u32(&mut bytes, opt + layout::OPT_ADDRESS_OF_ENTRY_POINT, 0)?;
    write_u32(&mut bytes, opt + layout::OPT_CHECKSUM, 0)?;
    let dll_characteristics = info.dll_characteristics & !dll_flags::FORCE_INTEGRITY;
    write_u16(
        &mut bytes,
        opt + layout::OPT_DLL_CHARACTERISTICS,
        dll_characteristics,
    )?;

    // Windows 10 assigns static TLS to the prepared DLL when its empty
    // import table is present. The parser has already walked one descriptor
    // per module up to the null terminator, regardless of the recorded Size.
    if let Some(at) = layout::data_directory_offset(&bytes, dir::IMPORT) {
        let rva = layout::read_u32(&bytes, at).ok_or(PrepareError::Patch("no import RVA"))?;
        if rva != 0 {
            let terminator = u32::try_from(info.imports.len())
                .ok()
                .and_then(|count| count.checked_mul(IMPORT_DESCRIPTOR_SIZE))
                .and_then(|offset| rva.checked_add(offset))
                .ok_or(PrepareError::Patch("import terminator RVA overflow"))?;
            write_u32(&mut bytes, at, terminator)?;
            write_u32(&mut bytes, at + 4, IMPORT_DESCRIPTOR_SIZE)?;
        }
    }
    if let Some(at) = layout::data_directory_offset(&bytes, dir::BOUND_IMPORT) {
        write(&mut bytes, at, &[0; layout::DATA_DIRECTORY_SIZE])?;
    }

    if let Some(tls) = &info.tls
        && tls.callbacks_va != 0
    {
        let at = file_span(&info, tls.directory_rva, TLS64_CALLBACKS_END)
            .ok_or(PrepareError::Patch("TLS callback field is not file-backed"))?;
        let field_rva = tls
            .directory_rva
            .checked_add(layout::TLS64_ADDRESS_OF_CALLBACKS as u32)
            .ok_or(PrepareError::Patch("TLS callback field RVA overflow"))?;
        write_u64(&mut bytes, at + layout::TLS64_ADDRESS_OF_CALLBACKS, 0)?;
        // The field's base relocation would turn the 0 into `delta` whenever
        // the loader maps the copy away from its preferred base (Wine always
        // does for DYNAMIC_BASE images), and the loader then reads callbacks
        // from there. Make that entry a no-op.
        for (entry, target) in dir64_relocations(&bytes, &info) {
            if target == field_rva {
                write_u16(&mut bytes, entry, 0)?; // IMAGE_REL_BASED_ABSOLUTE
            }
        }
    }

    Ok(PreparedImage {
        info,
        bytes,
        original_hash: blake3::hash(original),
    })
}

/// Size of an IMAGE_IMPORT_DESCRIPTOR in the PE file.
const IMPORT_DESCRIPTOR_SIZE: u32 = 20;
/// End offset of `AddressOfCallBacks` (`+24..+32`) inside `IMAGE_TLS_DIRECTORY64`
/// (trailing `SizeOfZeroFill` and `Characteristics` at `+32..+40` may sit in
/// virtual zero-padding when `VirtualSize > SizeOfRawData`).
const TLS64_CALLBACKS_END: u32 = layout::TLS64_ADDRESS_OF_CALLBACKS as u32 + 8;

/// File offset of `[rva, rva + len)` when one section backs all `len` bytes.
fn file_span(info: &ImageInfo, rva: u32, len: u32) -> Option<usize> {
    info.sections.iter().find_map(|s| {
        let delta = rva.checked_sub(s.rva)?;
        let backed = match s.virtual_size {
            0 => s.file_size,
            vs => vs.min(s.file_size),
        };
        let end = delta.checked_add(len)?;
        (end <= backed)
            .then(|| s.file_offset.checked_add(delta))?
            .map(|at| at as usize)
    })
}

/// Every `IMAGE_REL_BASED_DIR64` base relocation: (file offset of its entry,
/// RVA it patches). Empty if the image has no readable relocation table.
fn dir64_relocations(bytes: &[u8], info: &ImageInfo) -> Vec<(usize, u32)> {
    const DIR64: u16 = 10;
    let mut out = Vec::new();
    let Some(d) = layout::data_directory_offset(bytes, dir::BASERELOC) else {
        return out;
    };
    let (Some(mut rva), Some(mut left)) =
        (layout::read_u32(bytes, d), layout::read_u32(bytes, d + 4))
    else {
        return out;
    };
    while left >= 8 {
        let Some(hdr_at) = file_span(info, rva, 8) else {
            break;
        };
        let (Some(page), Some(block)) = (
            layout::read_u32(bytes, hdr_at),
            layout::read_u32(bytes, hdr_at + 4),
        ) else {
            break;
        };
        if block < 8 || block > left {
            break;
        }
        let Some(at) = file_span(info, rva, block) else {
            break;
        };
        let Some(entries) = bytes.get(at + 8..at + block as usize) else {
            break;
        };
        for (i, pair) in entries.chunks_exact(2).enumerate() {
            let e = u16::from_le_bytes([pair[0], pair[1]]);
            if e >> 12 == DIR64
                && let Some(target) = page.checked_add(u32::from(e & 0xFFF))
            {
                out.push((at + 8 + i * 2, target));
            }
        }
        let Some(next) = rva.checked_add(block) else {
            break;
        };
        rva = next;
        left -= block;
    }
    if rva != 0 && left > 0 {
        tracing::warn!(
            rva = format_args!("{rva:#x}"),
            left,
            "base relocation walk stopped before the end of the directory"
        );
    }
    out
}

fn write_u16(bytes: &mut [u8], at: usize, v: u16) -> Result<(), PrepareError> {
    write(bytes, at, &v.to_le_bytes())
}
fn write_u32(bytes: &mut [u8], at: usize, v: u32) -> Result<(), PrepareError> {
    write(bytes, at, &v.to_le_bytes())
}
fn write_u64(bytes: &mut [u8], at: usize, v: u64) -> Result<(), PrepareError> {
    write(bytes, at, &v.to_le_bytes())
}
fn write(bytes: &mut [u8], at: usize, v: &[u8]) -> Result<(), PrepareError> {
    bytes
        .get_mut(at..at + v.len())
        .ok_or(PrepareError::Patch("header field outside of file"))?
        .copy_from_slice(v);
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn patches_own_test_binary() {
        let original = std::fs::read(std::env::current_exe().unwrap()).unwrap();
        let prepared = prepare(&original).unwrap();
        assert!(!prepared.info.is_dll(), "info describes the original");
        assert!(prepared.info.import_count() > 0);

        let patched = ImageInfo::parse(&prepared.bytes).unwrap();
        assert!(patched.is_dll());
        assert_eq!(patched.entry_rva, 0);
        assert!(patched.imports.is_empty());
        assert_eq!(patched.dll_characteristics & dll_flags::FORCE_INTEGRITY, 0);
        if let Some(tls) = &patched.tls {
            assert_eq!(tls.callbacks_va, 0);
            let field = tls.directory_rva + layout::TLS64_ADDRESS_OF_CALLBACKS as u32;
            assert!(
                dir64_relocations(&prepared.bytes, &patched)
                    .iter()
                    .all(|&(_, target)| target != field),
                "the cleared callback pointer must not be relocated"
            );
        }
        // Nothing else moved: same sections, same size.
        assert_eq!(patched.sections.len(), prepared.info.sections.len());
        assert_eq!(prepared.bytes.len(), original.len());
    }

    #[test]
    fn keeps_iat_slots_when_import_size_excludes_the_terminator() {
        let mut original = std::fs::read(std::env::current_exe().unwrap()).unwrap();
        let info = ImageInfo::parse(&original).unwrap();
        let directory = layout::data_directory_offset(&original, dir::IMPORT).unwrap();
        // This linker layout counts the descriptors for real modules only.
        let descriptor_bytes = u32::try_from(info.imports.len()).unwrap() * IMPORT_DESCRIPTOR_SIZE;
        write_u32(&mut original, directory + 4, descriptor_bytes).unwrap();
        let prepared = prepare(&original).unwrap();
        let empty = layout::read_u32(&prepared.bytes, directory).unwrap();
        assert_eq!(
            empty,
            layout::read_u32(&original, directory).unwrap() + descriptor_bytes
        );
        assert_eq!(
            layout::read_u32(&prepared.bytes, directory + 4),
            Some(IMPORT_DESCRIPTOR_SIZE)
        );
        let at = info.rva_to_file_offset(empty).unwrap() as usize;
        assert_eq!(
            &prepared.bytes[at..at + IMPORT_DESCRIPTOR_SIZE as usize],
            &[0; 20]
        );
        let iat = layout::data_directory_offset(&original, dir::IAT).unwrap();
        assert_eq!(
            &prepared.bytes[iat..iat + layout::DATA_DIRECTORY_SIZE],
            &original[iat..iat + layout::DATA_DIRECTORY_SIZE]
        );
        assert_eq!(prepared.info.import_count(), info.import_count());
        for module in &prepared.info.imports {
            for entry in &module.entries {
                let at = info.rva_to_file_offset(entry.iat_slot_rva).unwrap() as usize;
                assert_eq!(&prepared.bytes[at..at + 8], &original[at..at + 8]);
            }
        }
    }

    #[cfg(windows)]
    #[test]
    fn windows_assigns_static_tls_to_a_prepared_dll() {
        // Model a title DLL using our own linked image. Loading a DLL leaves
        // its assigned index intact; the executable-specific redirect resets it.
        let mut original = std::fs::read(std::env::current_exe().unwrap()).unwrap();
        let flags =
            layout::file_header_offset(&original).unwrap() + layout::FILE_HEADER_CHARACTERISTICS;
        let characteristics =
            ImageInfo::parse(&original).unwrap().characteristics | file_flags::DLL;
        write_u16(&mut original, flags, characteristics).unwrap();
        let prepared = prepare(&original).unwrap();
        let info = &prepared.info;
        let tls = info.tls.as_ref().expect("the test binary uses static TLS");
        let directory = info.rva_to_file_offset(tls.directory_rva).unwrap() as usize;
        let index_va = layout::read_u64(&original, directory + 16).unwrap();
        let index_rva = index_va.checked_sub(info.image_base).unwrap();
        assert!(index_rva + 4 <= u64::from(info.size_of_image));

        let folder = tempfile::tempdir().unwrap();
        let image = crate::load(&prepared, folder.path()).unwrap();
        // SAFETY: the mapped test image contains the checked DWORD above.
        let assigned =
            unsafe { ((image.base() + index_rva as usize) as *const u32).read_unaligned() };
        // SAFETY: the prepared DLL has never been attached or given to a caller.
        unsafe { image.unload() };
        assert_ne!(
            assigned, 0,
            "the process executable already owns TLS slot zero"
        );
    }

    #[test]
    fn file_name_is_stable_per_build() {
        let original = std::fs::read(std::env::current_exe().unwrap()).unwrap();
        let a = prepare(&original).unwrap();
        let b = prepare(&original).unwrap();
        assert_eq!(a.file_name(), b.file_name());
    }

    /// Builds a minimal PE32+ AMD64 image with the given `(name, rva, virtual_size,
    /// file_offset, file_size)` sections and `(index, rva, size)` data directories.
    fn synthetic_pe64(
        file_len: usize,
        size_of_image: u32,
        sections: &[(&[u8; 8], u32, u32, u32, u32)],
        dirs: &[(usize, u32, u32)],
    ) -> Vec<u8> {
        let mut buf = vec![0u8; file_len];
        buf[..2].copy_from_slice(b"MZ");
        let lfanew = 0x40usize;
        buf[layout::DOS_E_LFANEW..layout::DOS_E_LFANEW + 4]
            .copy_from_slice(&(lfanew as u32).to_le_bytes());
        buf[lfanew..lfanew + 4].copy_from_slice(b"PE\0\0");

        let fh = lfanew + layout::NT_SIGNATURE_SIZE;
        buf[fh..fh + 2].copy_from_slice(&0x8664u16.to_le_bytes());
        buf[fh + 2..fh + 4].copy_from_slice(&(sections.len() as u16).to_le_bytes());
        let opt_size = 240u16;
        buf[fh + 16..fh + 18].copy_from_slice(&opt_size.to_le_bytes());
        buf[fh + 18..fh + 20].copy_from_slice(&0x0022u16.to_le_bytes());

        let opt = fh + layout::FILE_HEADER_SIZE;
        buf[opt..opt + 2].copy_from_slice(&0x020Bu16.to_le_bytes());
        buf[opt + 24..opt + 32].copy_from_slice(&0x0000_0001_4000_0000u64.to_le_bytes());
        buf[opt + 32..opt + 36].copy_from_slice(&0x1000u32.to_le_bytes());
        buf[opt + 36..opt + 40].copy_from_slice(&0x0200u32.to_le_bytes());
        buf[opt + 56..opt + 60].copy_from_slice(&size_of_image.to_le_bytes());
        buf[opt + 60..opt + 64].copy_from_slice(&0x0200u32.to_le_bytes());
        buf[opt + 68..opt + 70].copy_from_slice(&3u16.to_le_bytes());
        buf[opt + layout::OPT_NUMBER_OF_RVA_AND_SIZES
            ..opt + layout::OPT_NUMBER_OF_RVA_AND_SIZES + 4]
            .copy_from_slice(&16u32.to_le_bytes());

        for &(idx, rva, size) in dirs {
            let at = opt + layout::OPT_DATA_DIRECTORY + idx * layout::DATA_DIRECTORY_SIZE;
            buf[at..at + 4].copy_from_slice(&rva.to_le_bytes());
            buf[at + 4..at + 8].copy_from_slice(&size.to_le_bytes());
        }

        let sec_base = opt + usize::from(opt_size);
        for (i, &(name, rva, vsize, foff, fsize)) in sections.iter().enumerate() {
            let s = sec_base + i * 40;
            buf[s..s + 8].copy_from_slice(name);
            buf[s + 8..s + 12].copy_from_slice(&vsize.to_le_bytes());
            buf[s + 12..s + 16].copy_from_slice(&rva.to_le_bytes());
            buf[s + 16..s + 20].copy_from_slice(&fsize.to_le_bytes());
            buf[s + 20..s + 24].copy_from_slice(&foff.to_le_bytes());
            buf[s + 36..s + 40].copy_from_slice(&0x4000_0040u32.to_le_bytes());
        }
        buf
    }

    #[test]
    fn tls_callbacks_field_must_be_file_backed_while_trailing_zero_fill_is_allowed() {
        const TLS_DIR: usize = 9;
        // Case 1: `.rdata` has `VirtualSize = 28, SizeOfRawData = 0x200`, so
        // `AddressOfCallBacks` (`+24..+32`) extends 4 bytes past `VirtualSize`.
        let mut short_virtual = synthetic_pe64(
            0x600,
            0x3000,
            &[
                (b".rdata\0\0", 0x1000, 28, 0x200, 0x200),
                (b".data\0\0\0", 0x2000, 0x40, 0x400, 0x200),
            ],
            &[(TLS_DIR, 0x1000, 40)],
        );
        write_u64(
            &mut short_virtual,
            0x200 + layout::TLS64_ADDRESS_OF_CALLBACKS,
            0x0000_0001_4000_2000,
        )
        .unwrap();
        let err = prepare(&short_virtual)
            .expect_err("TLS callbacks field truncated at 28 bytes must fail");
        assert!(
            matches!(err, PrepareError::Pe(_) | PrepareError::Patch(_)),
            "unexpected error: {err:?}"
        );

        // Case 2: `.rdata` has `VirtualSize = 40, SizeOfRawData = 32`, so
        // `+0..+32` (including `AddressOfCallBacks` at `+24..+32`) is file-backed
        // and `+32..+40` (`SizeOfZeroFill`, `Characteristics`) is zero-padded by
        // the Windows PE loader.
        let mut zero_tailed = synthetic_pe64(
            0x600,
            0x3000,
            &[
                (b".rdata\0\0", 0x1000, 40, 0x200, 32),
                (b".data\0\0\0", 0x2000, 0x40, 0x400, 0x200),
            ],
            &[(TLS_DIR, 0x1000, 40)],
        );
        write_u64(
            &mut zero_tailed,
            0x200 + layout::TLS64_ADDRESS_OF_CALLBACKS,
            0x0000_0001_4000_2000,
        )
        .unwrap();
        let prepared = prepare(&zero_tailed).expect("32-byte file-backed TLS prefix must succeed");
        assert_eq!(
            layout::read_u64(&prepared.bytes, 0x200 + layout::TLS64_ADDRESS_OF_CALLBACKS),
            Some(0)
        );
    }

    #[test]
    fn relocation_scan_walks_multiple_blocks_and_respects_section_bounds() {
        const TLS_DIR: usize = 9;
        let dir64_tls_callbacks = (10u16 << 12) | (layout::TLS64_ADDRESS_OF_CALLBACKS as u16);
        let dir64_other = (10u16 << 12) | 0x0008;

        // Positive multi-block case: Block 1 (16 bytes for page `0x3000`) is
        // followed by Block 2 (12 bytes for page `0x1000`) carrying the TLS
        // `AddressOfCallBacks` relocation alongside a neighboring relocation.
        let mut multi = synthetic_pe64(
            0x440,
            0x4000,
            &[
                (b".rdata\0\0", 0x1000, 0x100, 0x200, 0x200),
                (b".reloc\0\0", 0x2000, 0x20, 0x400, 0x20),
                (b".data\0\0\0", 0x3000, 0x20, 0x420, 0x20),
            ],
            &[(TLS_DIR, 0x1000, 40), (dir::BASERELOC, 0x2000, 28)],
        );
        write_u64(
            &mut multi,
            0x200 + layout::TLS64_ADDRESS_OF_CALLBACKS,
            0x0000_0001_4000_1040,
        )
        .unwrap();
        // Block 1 at `0x400..0x410` (`PageRVA = 0x3000`, `SizeOfBlock = 16`).
        write_u32(&mut multi, 0x400, 0x3000).unwrap();
        write_u32(&mut multi, 0x404, 16).unwrap();
        write_u16(&mut multi, 0x408, dir64_other).unwrap();
        // Block 2 at `0x410..0x41c` (`PageRVA = 0x1000`, `SizeOfBlock = 12`).
        write_u32(&mut multi, 0x410, 0x1000).unwrap();
        write_u32(&mut multi, 0x414, 12).unwrap();
        write_u16(&mut multi, 0x418, dir64_other).unwrap();
        write_u16(&mut multi, 0x41a, dir64_tls_callbacks).unwrap();

        let prepared_multi = prepare(&multi).unwrap();
        let u16_at = |b: &[u8], at: usize| u16::from_le_bytes([b[at], b[at + 1]]);
        assert_eq!(u16_at(&prepared_multi.bytes, 0x408), dir64_other);
        assert_eq!(u16_at(&prepared_multi.bytes, 0x418), dir64_other);
        assert_eq!(
            u16_at(&prepared_multi.bytes, 0x41a),
            0,
            "second relocation block's TLS callback entry must be cleared to ABSOLUTE"
        );

        // Case 1: `.reloc` is 0x10 bytes at file offset `0x400..0x410`, followed
        // immediately in the file by `.data` at `0x410..0x430`, while
        // `DataDirectory[BASERELOC].Size` is oversized (`0x1c`).
        let mut pe = synthetic_pe64(
            0x440,
            0x4000,
            &[
                (b".rdata\0\0", 0x1000, 0x100, 0x200, 0x200),
                (b".reloc\0\0", 0x2000, 0x10, 0x400, 0x10),
                (b".data\0\0\0", 0x3000, 0x20, 0x410, 0x20),
            ],
            &[(TLS_DIR, 0x1000, 40), (dir::BASERELOC, 0x2000, 0x1c)],
        );
        write_u64(
            &mut pe,
            0x200 + layout::TLS64_ADDRESS_OF_CALLBACKS,
            0x0000_0001_4000_1040,
        )
        .unwrap();
        // Valid 16-byte harmless block filling `.reloc` (`0x400..0x410`).
        write_u32(&mut pe, 0x400, 0x3000).unwrap();
        write_u32(&mut pe, 0x404, 16).unwrap();
        // `.data` starts at `0x410` with bytes resembling a relocation block
        // targeting the TLS `AddressOfCallBacks` RVA (`0x1018`).
        write_u32(&mut pe, 0x410, 0x1000).unwrap();
        write_u32(&mut pe, 0x414, 12).unwrap();
        write_u16(&mut pe, 0x418, dir64_tls_callbacks).unwrap();
        write_u16(&mut pe, 0x41a, 0xbeef).unwrap();

        let prepared = prepare(&pe).unwrap();
        assert_eq!(
            &prepared.bytes[0x410..0x430],
            &pe[0x410..0x430],
            "oversized BASERELOC directory must not patch across into .data"
        );

        // Case 2: `.reloc` block with odd `SizeOfBlock = 9` where bytes `+8..+10`
        // straddle 1 byte past `SizeOfBlock`.
        let mut odd = synthetic_pe64(
            0x440,
            0x3000,
            &[
                (b".rdata\0\0", 0x1000, 0x100, 0x200, 0x200),
                (b".reloc\0\0", 0x2000, 0x10, 0x400, 0x10),
            ],
            &[(TLS_DIR, 0x1000, 40), (dir::BASERELOC, 0x2000, 9)],
        );
        write_u64(
            &mut odd,
            0x200 + layout::TLS64_ADDRESS_OF_CALLBACKS,
            0x0000_0001_4000_1040,
        )
        .unwrap();
        write_u32(&mut odd, 0x400, 0x1000).unwrap();
        write_u32(&mut odd, 0x404, 9).unwrap();
        write_u16(&mut odd, 0x408, dir64_tls_callbacks).unwrap();

        let prepared_odd = prepare(&odd).unwrap();
        assert_eq!(
            &prepared_odd.bytes[0x408..0x40a],
            &odd[0x408..0x40a],
            "odd SizeOfBlock must not read or patch past block end"
        );
    }
}
