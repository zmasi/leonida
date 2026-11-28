//! Which Direct3D 12 runtime `leonida.exe` asks Windows for.
//!
//! Windows 10's own Direct3D 12 stops at shader model 6.5 and refuses root
//! signatures that index the descriptor heaps directly, which every
//! translated pipeline uses (shader model 6.6): there each pipeline is
//! refused and the title closes within seconds. Microsoft's D3D12 Agility SDK
//! fixes that: an executable that exports `D3D12SDKVersion` and
//! `D3D12SDKPath` gets the newer `D3D12Core.dll` from that folder, where its
//! version is newer than Windows' own (`scripts\get-d3d12.ps1` fetches it).
//!
//! The version asked for has to match the file: a `D3D12Core.dll` of another
//! version than the one asked for makes Windows refuse Direct3D 12 altogether
//! ("the D3D12 SDK version configuration of the host exe is invalid",
//! 0x887E0003), and asking for one where the folder is missing does the same
//! on a Windows whose own runtime is older. So [`match_d3d12_core`] reads
//! the version of the file beside the program, or Windows' own without one,
//! first thing in `main`. Windows reads `D3D12SDKVersion` when `d3d12.dll` is
//! loaded, so build.rs delay-loads it: imported the usual way it is loaded
//! before `main`, and the value set here would come too late.
//!
//! Ported from trustno1-coder/VIctory-NVIDIA (GPL-3.0-or-later: `main.rs`,
//! `build.rs` and `scripts/get-d3d12.ps1` of v2026.11.27.9, commits 10862a6,
//! e69e39c, f3534e6 and 0f0ada1 there), where it was measured on Windows 10
//! 22H2 and on Windows 11 22H2 and 25H2.

/// The Agility SDK version Windows reads, set by [`match_d3d12_core`]
/// before Direct3D 12 is first used (an atomic: the same bytes as a `u32`,
/// in memory that can be written). 614 until then.
#[cfg(windows)]
#[unsafe(no_mangle)]
#[used]
#[allow(non_upper_case_globals)]
pub static D3D12SDKVersion: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(614);

/// `D3D12SDKPath`'s value: a pointer to a C string.
#[cfg(windows)]
#[repr(transparent)]
pub struct SdkPath(*const std::ffi::c_char);

// SAFETY: it points at a string literal, never written.
#[cfg(windows)]
unsafe impl Sync for SdkPath {}

/// Where `D3D12Core.dll` is, from `leonida.exe`'s folder.
#[cfg(windows)]
#[unsafe(no_mangle)]
#[used]
#[allow(non_upper_case_globals)]
pub static D3D12SDKPath: SdkPath = SdkPath(c".\\D3D12\\".as_ptr());

/// The highest version asked for where no `D3D12\D3D12Core.dll` is beside
/// the program: the one the fetch script gets.
#[cfg_attr(not(windows), allow(dead_code))]
const TESTED: u32 = 614;

/// The Agility SDK version of a `D3D12Core.dll` (its file version is
/// `1.<sdk>.<patch>.0`, e.g. 1.614.1.0), from the file's VS_FIXEDFILEINFO.
#[cfg_attr(not(windows), allow(dead_code))]
pub(crate) fn agility_version(dll: &[u8]) -> Option<u32> {
    let signature = 0xFEEF_04BD_u32.to_le_bytes();
    let at = dll.windows(4).position(|w| w == signature)?;
    let most = u32::from_le_bytes(dll.get(at + 8..at + 12)?.try_into().ok()?);
    let (major, minor) = (most >> 16, most & 0xffff);
    (major == 1 && minor >= 600).then_some(minor)
}

/// The `D3D12SDKVersion` a `D3D12Core.dll` exports: Windows' own, in
/// System32, says which version it is (616 on Windows 11 25H2).
#[cfg_attr(not(windows), allow(dead_code))]
pub(crate) fn exported_sdk_version(dll: &[u8]) -> Option<u32> {
    let u16_at = |at: usize| Some(u16::from_le_bytes(dll.get(at..at + 2)?.try_into().ok()?));
    let u32_at = |at: usize| Some(u32::from_le_bytes(dll.get(at..at + 4)?.try_into().ok()?));
    let pe = u32_at(0x3c)? as usize;
    (dll.get(pe..pe + 4)? == b"PE\0\0").then_some(())?;
    let sections = usize::from(u16_at(pe + 6)?);
    let optional = pe + 24;
    let table = optional + usize::from(u16_at(pe + 20)?);
    // PE32+ only: the export directory is the first data directory, at 112.
    (u16_at(optional)? == 0x20b).then_some(())?;
    let offset = |rva: u32| -> Option<usize> {
        (0..sections).find_map(|i| {
            let s = table + i * 40;
            let (size, va, raw_size, raw) = (
                u32_at(s + 8)?,
                u32_at(s + 12)?,
                u32_at(s + 16)?,
                u32_at(s + 20)?,
            );
            // Checked: a malformed section table (va + size or the file offset past
            // u32::MAX) is no section, not an overflow.
            let end = va.checked_add(size.max(raw_size))?;
            if va <= rva && rva < end {
                (rva - va).checked_add(raw).map(|at| at as usize)
            } else {
                None
            }
        })
    };
    let exports = offset(u32_at(optional + 112)?)?;
    // NumberOfNames, AddressOfFunctions, AddressOfNames, AddressOfNameOrdinals
    let count = u32_at(exports + 24)? as usize;
    let functions_at = offset(u32_at(exports + 28)?)?;
    let names_at = offset(u32_at(exports + 32)?)?;
    let ordinals_at = offset(u32_at(exports + 36)?)?;
    (0..count).find_map(|i| {
        let name = offset(u32_at(names_at + 4 * i)?)?;
        if dll.get(name..name + 16)? != b"D3D12SDKVersion\0" {
            return None;
        }
        let ordinal = usize::from(u16_at(ordinals_at + 2 * i)?);
        u32_at(offset(u32_at(functions_at + 4 * ordinal)?)?)
    })
}

/// The version asked for where no `D3D12\D3D12Core.dll` is beside this
/// program: Windows' own runtime's (System32's `D3D12Core.dll` exports it),
/// at most [`TESTED`], so Windows keeps its own. Where that file is missing
/// or unreadable (Windows older than 10 2004 reads no `D3D12SDKVersion`), 4,
/// the first Agility SDK's.
#[cfg(windows)]
fn os_runtime() -> u32 {
    let system = std::env::var_os("SystemRoot")
        .map(std::path::PathBuf::from)
        .unwrap_or_else(|| r"C:\Windows".into());
    std::fs::read(system.join("System32").join("D3D12Core.dll"))
        .ok()
        .and_then(|dll| exported_sdk_version(&dll))
        .map_or(4, |v| v.min(TESTED))
}

/// Windows' build number (19045 is Windows 10 22H2; 22000 and up, Windows
/// 11), from `RtlGetVersion`, which no compatibility shim changes.
/// `LEONIDA_WINDOWS_BUILD` stands in for it (to try Windows 10's path).
#[cfg(windows)]
pub(crate) fn windows_build() -> Option<u32> {
    if let Some(build) = std::env::var("LEONIDA_WINDOWS_BUILD")
        .ok()
        .and_then(|v| v.trim().parse().ok())
    {
        return Some(build);
    }
    #[repr(C)]
    struct OsVersionInfo {
        size: u32,
        major: u32,
        minor: u32,
        build: u32,
        platform: u32,
        service_pack: [u16; 128],
    }
    #[link(name = "ntdll")]
    unsafe extern "system" {
        fn RtlGetVersion(info: *mut OsVersionInfo) -> i32;
    }
    let mut info = OsVersionInfo {
        size: size_of::<OsVersionInfo>() as u32,
        major: 0,
        minor: 0,
        build: 0,
        platform: 0,
        service_pack: [0; 128],
    };
    // SAFETY: `info` is an OSVERSIONINFOW with its size set, as RtlGetVersion takes.
    (unsafe { RtlGetVersion(&mut info) } == 0).then_some(info.build)
}

/// Sets [`D3D12SDKVersion`] to the version of the `D3D12\D3D12Core.dll`
/// beside this program, or to Windows' own runtime's where there is none.
/// First thing in `main`: Windows reads it when Direct3D 12 is first used.
#[cfg(windows)]
pub(crate) fn match_d3d12_core() {
    use std::sync::atomic::Ordering::Relaxed;
    let Some(core) = std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(|d| d.join("D3D12").join("D3D12Core.dll")))
    else {
        return;
    };
    let Ok(bytes) = std::fs::read(&core) else {
        D3D12SDKVersion.store(os_runtime(), Relaxed);
        if windows_build().is_some_and(|build| build < 22000) {
            eprintln!(
                "Windows 10: Direct3D 12 here stops at shader model 6.5, and the translated shaders need 6.6. \
                 Without {} the game's pipelines are refused: d3d12\\Get-D3D12.cmd fetches Microsoft's runtime once.",
                core.display()
            );
        }
        return;
    };
    match agility_version(&bytes) {
        Some(v) => {
            if v != TESTED {
                eprintln!(
                    "{} is the D3D12 Agility SDK 1.{v}: asking Windows for that version.",
                    core.display()
                );
            }
            D3D12SDKVersion.store(v, Relaxed);
        }
        None => {
            D3D12SDKVersion.store(os_runtime(), Relaxed);
            eprintln!(
                "{} is not a D3D12 Agility SDK runtime Windows can use: Windows' own is used.",
                core.display()
            );
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The bytes of a VS_FIXEDFILEINFO for file version `major.minor.patch.0`,
    /// behind some padding, as a DLL's version resource holds it.
    fn version_resource(major: u16, minor: u16, patch: u16) -> Vec<u8> {
        let mut bytes = vec![0u8; 64];
        bytes.extend_from_slice(&0xFEEF_04BD_u32.to_le_bytes());
        bytes.extend_from_slice(&0x0001_0000_u32.to_le_bytes()); // structure version
        bytes.extend_from_slice(&((u32::from(major) << 16) | u32::from(minor)).to_le_bytes());
        bytes.extend_from_slice(&(u32::from(patch) << 16).to_le_bytes());
        bytes.extend_from_slice(&[0u8; 40]);
        bytes
    }

    #[test]
    fn agility_version_reads_the_minor_of_an_agility_runtime() {
        assert_eq!(agility_version(&version_resource(1, 614, 1)), Some(614));
        assert_eq!(agility_version(&version_resource(1, 620, 2)), Some(620));
    }

    #[test]
    fn agility_version_refuses_what_is_not_an_agility_runtime() {
        // Windows' own D3D12Core.dll carries the OS version (10.0.x).
        assert_eq!(agility_version(&version_resource(10, 0, 19041)), None);
        assert_eq!(agility_version(&version_resource(1, 4, 0)), None);
        assert_eq!(agility_version(b"no version resource"), None);
    }

    /// A minimal PE32+ image whose export directory names `exports` (each
    /// pointing at its `u32` value), in one section.
    fn pe_with_exports(exports: &[(&str, u32)]) -> Vec<u8> {
        const PE: usize = 0x40;
        const OPTIONAL: usize = PE + 24;
        const OPTIONAL_SIZE: usize = 240;
        const TABLE: usize = OPTIONAL + OPTIONAL_SIZE;
        const RAW: usize = 0x200;
        const VA: u32 = 0x1000;
        // The section: export directory (40), functions, names, ordinals, values, name strings.
        let n = exports.len();
        let functions = 40;
        let names = functions + 4 * n;
        let ordinals = names + 4 * n;
        let values = ordinals + 2 * n + (2 * n) % 4;
        let mut strings = values + 4 * n;
        let mut section = vec![0u8; strings];
        let put32 =
            |s: &mut Vec<u8>, at: usize, v: u32| s[at..at + 4].copy_from_slice(&v.to_le_bytes());
        put32(&mut section, 24, n as u32);
        put32(&mut section, 28, VA + functions as u32);
        put32(&mut section, 32, VA + names as u32);
        put32(&mut section, 36, VA + ordinals as u32);
        for (i, (name, value)) in exports.iter().enumerate() {
            put32(
                &mut section,
                functions + 4 * i,
                VA + (values + 4 * i) as u32,
            );
            put32(&mut section, names + 4 * i, VA + strings as u32);
            section[ordinals + 2 * i..ordinals + 2 * i + 2]
                .copy_from_slice(&(i as u16).to_le_bytes());
            put32(&mut section, values + 4 * i, *value);
            section.extend_from_slice(name.as_bytes());
            section.push(0);
            strings += name.len() + 1;
        }
        let mut image = vec![0u8; RAW];
        image[0x3c..0x40].copy_from_slice(&(PE as u32).to_le_bytes());
        image[PE..PE + 4].copy_from_slice(b"PE\0\0");
        image[PE + 6..PE + 8].copy_from_slice(&1u16.to_le_bytes());
        image[PE + 20..PE + 22].copy_from_slice(&(OPTIONAL_SIZE as u16).to_le_bytes());
        image[OPTIONAL..OPTIONAL + 2].copy_from_slice(&0x20bu16.to_le_bytes());
        image[OPTIONAL + 112..OPTIONAL + 116].copy_from_slice(&VA.to_le_bytes());
        let len = section.len() as u32;
        image[TABLE + 8..TABLE + 12].copy_from_slice(&len.to_le_bytes());
        image[TABLE + 12..TABLE + 16].copy_from_slice(&VA.to_le_bytes());
        image[TABLE + 16..TABLE + 20].copy_from_slice(&len.to_le_bytes());
        image[TABLE + 20..TABLE + 24].copy_from_slice(&(RAW as u32).to_le_bytes());
        image.extend_from_slice(&section);
        image
    }

    #[test]
    fn exported_sdk_version_finds_the_export_among_others() {
        let image = pe_with_exports(&[
            ("D3D12GetInterface", 7),
            ("D3D12SDKVersion", 616),
            ("D3D12SDKPath", 9),
        ]);
        assert_eq!(exported_sdk_version(&image), Some(616));
    }

    #[test]
    fn exported_sdk_version_is_none_without_the_export_or_a_pe_image() {
        assert_eq!(
            exported_sdk_version(&pe_with_exports(&[("D3D12GetInterface", 7)])),
            None
        );
        assert_eq!(exported_sdk_version(b"MZ and nothing else"), None);
    }

    /// A section table whose numbers run past `u32::MAX` (a malformed or
    /// truncated DLL) is no section: `None`, not an overflow panic.
    #[test]
    fn exported_sdk_version_is_none_for_a_section_past_u32_max() {
        // Offsets as in `pe_with_exports`: the section header follows a
        // 240-byte optional header at 0x40 + 24.
        const TABLE: usize = 0x40 + 24 + 240;
        let mut image = pe_with_exports(&[("D3D12SDKVersion", 616)]);
        // Virtual address near the top, so that va + size wraps.
        image[TABLE + 12..TABLE + 16].copy_from_slice(&0xFFFF_F000u32.to_le_bytes());
        image[TABLE + 8..TABLE + 12].copy_from_slice(&0x0010_0000u32.to_le_bytes());
        assert_eq!(exported_sdk_version(&image), None);
        // File offset near the top, so that rva - va + raw wraps: the section
        // starts 0x100 below the export directory (at 0x1000) and is made big
        // enough to hold it.
        let mut image = pe_with_exports(&[("D3D12SDKVersion", 616)]);
        image[TABLE + 8..TABLE + 12].copy_from_slice(&0x1000u32.to_le_bytes());
        image[TABLE + 12..TABLE + 16].copy_from_slice(&0x0F00u32.to_le_bytes());
        image[TABLE + 20..TABLE + 24].copy_from_slice(&0xFFFF_FFF0u32.to_le_bytes());
        assert_eq!(exported_sdk_version(&image), None);
    }

    /// Windows' own runtime, where there is one: it exports a version the
    /// parser reads (on Windows 10 2004 and later): 2 on Windows 10 22H2
    /// (19045), 616 on Windows 11 25H2.
    #[cfg(windows)]
    #[test]
    fn exported_sdk_version_reads_windows_own_runtime() {
        let system = std::env::var_os("SystemRoot")
            .map(std::path::PathBuf::from)
            .unwrap_or_else(|| r"C:\Windows".into());
        let Ok(dll) = std::fs::read(system.join("System32").join("D3D12Core.dll")) else {
            return;
        };
        let version =
            exported_sdk_version(&dll).expect("System32's D3D12Core.dll exports D3D12SDKVersion");
        assert!(version >= 1, "D3D12SDKVersion {version}");
    }
}
