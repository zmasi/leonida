//! Opt-in compatibility hooks for known title builds.
//!
//! LOCAL: copied from vitutu1/leonida-perf crates/leonida-loader/src/hooks.rs, which ported it from
//! ZigZagBoi1/ScarlettPort (`runtime/victory`, GPL-3.0-or-later).
//! Only per-build offsets and short byte signatures live here (asset policy:
//! nothing of the title itself). Every hook checks the build's hash and the
//! exact bytes it replaces, and does nothing otherwise.

use windows_sys::Win32::System::Diagnostics::Debug::FlushInstructionCache;
use windows_sys::Win32::System::Memory::{PAGE_EXECUTE_READWRITE, VirtualProtect};
use windows_sys::Win32::System::Threading::GetCurrentProcess;

/// First 16 hex digits of the BLAKE3 of the Series X 1.0.2.1 executable.
const BUILD_1_0_2_1: &str = "b99f876b74d4a77f";

/// Whether `hash` is the 1.0.2.1 executable's.
pub(crate) fn is_build_1_0_2_1(hash: &blake3::Hash) -> bool {
    hash.to_hex().starts_with(BUILD_1_0_2_1)
}

/// Environment variable that turns the watchdog hook on.
pub(crate) const NO_WATCHDOG_ENV: &str = "LEONIDA_NO_WATCHDOG";

/// One code patch: `rva` must hold exactly `expect` before `new` is written.
pub(crate) struct Patch<'a> {
    pub(crate) what: &'static str,
    pub(crate) rva: usize,
    pub(crate) expect: &'a [u8],
    pub(crate) new: &'a [u8],
}

/// The title's watchdog thread ends the process on purpose when its main
/// thread has not advanced for about 30 one-second ticks. Two places do it:
/// the handler it calls (`mov dword ptr [0], imm32`) and, when the handler
/// returns or is unset, a fallback right after the call (`mov dword ptr [0],
/// 0; int3`). Jumping over the whole block returns the loop to its normal
/// path; the first patch makes the handler harmless if something else calls it.
const WATCHDOG: &[Patch<'static>] = &[
    Patch {
        what: "watchdog: skip the call and the fallback fault",
        rva: 0x2215c27,
        // mov rax, [rip + disp32] (the handler pointer)
        expect: &[0x48, 0x8B, 0x05, 0x1A, 0x07, 0xFA, 0x04],
        // jmp +0x18 (to the loop's normal continuation), then NOPs
        new: &[0xEB, 0x18, 0x90, 0x90, 0x90, 0x90, 0x90],
    },
    Patch {
        what: "watchdog: handler's deliberate fault",
        rva: 0x23cf683,
        expect: &[0xC7, 0x04, 0x25, 0, 0, 0, 0, 0x21, 0xD4, 0x8B, 0x4D],
        // an eleven-byte NOP
        new: &[0x66, 0x66, 0x2E, 0x0F, 0x1F, 0x84, 0, 0, 0, 0, 0],
    },
];

/// Applies the watchdog patches whose bytes match. Returns how many applied.
///
/// # Safety
/// `base` is the mapped image whose original BLAKE3 is `hash`, and no title
/// code is running yet.
pub(crate) unsafe fn disarm_watchdog(base: usize, hash: &blake3::Hash) -> usize {
    if !hash.to_hex().starts_with(BUILD_1_0_2_1) {
        return 0;
    }
    WATCHDOG
        .iter()
        // SAFETY: forwarded from this function's contract.
        .filter(|p| unsafe { apply(base, p) })
        .count()
}

/// Environment variable that pins the title's internal render scale
/// (`0.2..=1.0` of the output size). Testing lever, off by default.
pub(crate) const RENDER_SCALE_ENV: &str = "LEONIDA_RENDER_SCALE";
/// Environment variable that makes the title use its own temporal upscaler
/// on every console profile (Series S otherwise picks FSR).
pub(crate) const OWN_UPSCALER_ENV: &str = "LEONIDA_OWN_UPSCALER";

/// Leonida: environment variable that tops the title's output modes at 2560x1440 (Series X: the 3840x2160 row
/// of its mode table becomes 2560x1440, so it never finds a 4K mode and presents 1440p): [`OUTPUT_ENV`] with
/// `2560x1440`.
pub(crate) const OUTPUT_1440_ENV: &str = "LEONIDA_OUTPUT_1440";

/// Leonida: environment variable that sets the size of the title's largest output mode, `WIDTHxHEIGHT` (Series X:
/// `2560x1440`, or a wide one such as `5040x2160` or `3440x1440`). EXPERIMENTAL, not run yet: three places change
/// together ([`output_patches`], after VIctory's native 1440p profile as `runtime/src/display_profile.h` records
/// it). Alone, the mode row made the title stop at its own check at rva 0x247af2d a second after its window came
/// up (2026-11-27 runs, render scale 0.75 and 1.0 alike): what is left of the memory reserved for its scanout
/// buffers has to be under 2 MiB once they are made (`int3` when over), and the reservation was still the one for
/// 3840x2160.
pub(crate) const OUTPUT_ENV: &str = "LEONIDA_OUTPUT";

/// The title's table of output modes in `.rdata` (width, height, refresh, 1 per row: 720p, 936p, 1080p, 1440p,
/// 2160p, all 60 Hz): its last row, 3840x2160. The code that builds the mode list copies the rows, and the two
/// checks for a 4K output look 3840x2160 up in that list.
const OUTPUT_ROW: usize = 0x615c100;
/// `mov ecx, 0x4000000`: the bytes reserved for the scanout buffers off Series S (Series S takes another
/// immediate, by the `cmovne` after it).
const SCANOUT_RESERVATION: usize = 0x247ad4a;
/// `vmovsd xmm1, [rip + disp32]`: the Settings screen's viewport, read from a 3840x2160 pair of its own in `.data`.
const SETTINGS_VIEWPORT: usize = 0x58048d4;

/// The size [`OUTPUT_ENV`] asks for, if it is one the mode table can hold: even, 1280x720 to 7680x4320.
fn parse_output(text: &str) -> Option<(u32, u32)> {
    let (w, h) = text.trim().split_once(['x', 'X'])?;
    let (w, h): (u32, u32) = (w.trim().parse().ok()?, h.trim().parse().ok()?);
    ((1280..=7680).contains(&w) && (720..=4320).contains(&h) && w % 2 == 0 && h % 2 == 0).then_some((w, h))
}

/// The bytes the title reserves for its two scanout buffers of `width` x `height`: RGBA8 in the console's 64 KiB
/// tiling (128x128 texels a tile), together rounded up to 2 MiB. 0x4000000 for 3840x2160, the title's own figure.
fn scanout_pool_bytes(width: u32, height: u32) -> u32 {
    const TWO_MIB: u64 = 2 << 20;
    let tiles = u64::from(width.div_ceil(128)) * u64::from(height.div_ceil(128));
    ((tiles * 65536 * 2).div_ceil(TWO_MIB) * TWO_MIB) as u32
}

/// The patches for an output of `width` x `height` in place of 3840x2160: the mode row, the scanout
/// reservation that fits it, and the Settings viewport pointed at the mode row (it reads a pair of its own
/// otherwise). All three or none ([`apply_all`]).
fn output_patches(width: u32, height: u32) -> Vec<OwnedPatch> {
    let pair = |w: u32, h: u32| [w.to_le_bytes(), h.to_le_bytes()].concat();
    let reservation = |bytes: u32| [&[0xB9][..], &bytes.to_le_bytes()].concat();
    let viewport = (OUTPUT_ROW as i64 - (SETTINGS_VIEWPORT as i64 + 8)) as i32;
    vec![
        OwnedPatch {
            what: "output: the 3840x2160 mode row",
            rva: OUTPUT_ROW,
            expect: pair(3840, 2160),
            new: pair(width, height),
        },
        OwnedPatch {
            what: "output: the scanout buffers' reservation",
            rva: SCANOUT_RESERVATION,
            expect: reservation(scanout_pool_bytes(3840, 2160)),
            new: reservation(scanout_pool_bytes(width, height)),
        },
        OwnedPatch {
            what: "output: the Settings screen's viewport",
            rva: SETTINGS_VIEWPORT,
            expect: vec![0xC5, 0xFB, 0x10, 0x0D, 0x10, 0xF0, 0x13, 0x05],
            new: [&[0xC5, 0xFB, 0x10, 0x0D][..], &viewport.to_le_bytes()].concat(),
        },
    ]
}

/// A patch whose bytes are computed at load time.
struct OwnedPatch {
    what: &'static str,
    rva: usize,
    expect: Vec<u8>,
    new: Vec<u8>,
}

impl OwnedPatch {
    fn borrowed(&self) -> Patch<'_> {
        Patch { what: self.what, rva: self.rva, expect: &self.expect, new: &self.new }
    }
}

/// The upscaler choice: `mov eax, 6` (own upscaler) then `cmovne eax, ecx`
/// (FSR when the profile asks for it). NOPing the cmovne keeps 6.
const OWN_UPSCALER: Patch<'static> = Patch {
    what: "upscaler: always the title's own",
    rva: 0x7e9426,
    expect: &[0x0F, 0x45, 0xC1],
    new: &[0x0F, 0x1F, 0x00],
};

/// Leonida: environment variable that makes the title use its FidelityFX upscaler on every console profile
/// (Series X otherwise uses its own). Its upscale is then one `ffxDispatch` call, which DLSS or the stretch take
/// (`LEONIDA_UPSCALE`, `leonida-hle`'s `ffx.rs`) with the depth, motion vectors and jitter that frame generation
/// needs as well. [`OWN_UPSCALER_ENV`] wins over it.
pub(crate) const FSR_UPSCALER_ENV: &str = "LEONIDA_FSR_UPSCALER";

/// The upscaler choice again ([`OWN_UPSCALER`]), from two instructions earlier: `test al, al` asks whether the
/// console is a Series S (the device type, 6), and only then does the `cmovne` take ecx, which is 7 (FidelityFX)
/// when the title has that upscaler and 6 without it. `or al, 1` gives every console the Series S answer. Off
/// Series S the FidelityFX upscaler renders at scale 1.0 (its other load, [`FSR_LOAD_OTHER`]).
const FSR_UPSCALER: Patch<'static> = Patch {
    what: "upscaler: FidelityFX on every console",
    rva: 0x7e941f,
    expect: &[0x84, 0xC0, 0xB8, 0x06, 0, 0, 0, 0x0F, 0x45, 0xC1],
    new: &[0x0C, 0x01, 0xB8, 0x06, 0, 0, 0, 0x0F, 0x45, 0xC1],
};

/// The scale the FidelityFX upscaler loads on Series S, and where the constant is.
const FSR_SCALE_S: (f32, usize) = (2.0 / 3.0, 0x615d95c);
/// The scale it loads on the other consoles, and where the constant is.
const FSR_SCALE_OTHER: (f32, usize) = (1.0, 0x5fc6ed8);
/// The two loads, `vmovss xmm0, [rip + disp32]` (the next instruction at +8): Series S, the other consoles.
const FSR_LOAD_S: usize = 0x28ef6b2;
const FSR_LOAD_OTHER: usize = 0x28ef6a8;

/// Read-only float constants of this build usable as the FSR scale:
/// value, RVA. The two its loads read untouched are among them.
const FSR_SCALES: [(f32, usize); 8] = [
    (0.5, 0x5fc6edc),
    (0.4, 0x5fc6f90),
    (1.0 / 3.0, 0x615a240),
    (0.75, 0x6159b04),
    FSR_SCALE_S,
    FSR_SCALE_OTHER,
    (0.6, 0x615a220),
    (0.58, 0x61671d4),
];

/// The render-scale patches for `scale`, with the image at `base`:
/// - the own upscaler's minimum scale (an immediate, 2/3 by default) set to
///   `scale` and its maximum made equal to the minimum (`vmaxss` → `vmovss`),
///   so the scale is fixed;
/// - FSR's scale (a load of 2/3 on Series S, another of 1.0 on the other consoles) pointed at a constant
///   equal to `scale`, when this build has one, and its maximum size made the scaled
///   size (vtable slot 11 → slot 10's function).
fn render_scale_patches(base: usize, scale: f32, fsr: bool) -> Vec<OwnedPatch> {
    let mut patches = vec![
        OwnedPatch {
            what: "render scale: own upscaler minimum",
            rva: 0x28f1e7e,
            expect: vec![0xAB, 0xAA, 0x2A, 0x3F],
            new: scale.to_le_bytes().to_vec(),
        },
        OwnedPatch {
            what: "render scale: own upscaler maximum = minimum",
            rva: 0x28f1e87,
            expect: vec![0xC5, 0xFA, 0x5F, 0x86, 0x90, 0, 0, 0],
            new: vec![0xC5, 0xFA, 0x10, 0x86, 0x90, 0, 0, 0],
        },
    ];
    if !fsr {
        return patches;
    }
    match FSR_SCALES.iter().find(|(v, _)| (v - scale).abs() < 1e-3) {
        Some(&(_, constant)) => {
            let load = |at: usize, constant: usize| {
                let disp = (constant as i64 - (at as i64 + 8)) as i32;
                [&[0xC5, 0xFA, 0x10, 0x05][..], &disp.to_le_bytes()].concat()
            };
            for (what, at, stock) in [
                ("render scale: FSR scale", FSR_LOAD_S, FSR_SCALE_S.1),
                ("render scale: FSR scale off Series S", FSR_LOAD_OTHER, FSR_SCALE_OTHER.1),
            ] {
                // A load that reads this scale already stays as it is.
                if constant != stock {
                    patches.push(OwnedPatch { what, rva: at, expect: load(at, stock), new: load(at, constant) });
                }
            }
            patches.push(OwnedPatch {
                what: "render scale: FSR maximum = scaled size",
                rva: 0x6300ba8,
                expect: ((base + 0x28ef6f0) as u64).to_le_bytes().to_vec(),
                new: ((base + 0x28ef680) as u64).to_le_bytes().to_vec(),
            });
        }
        None => tracing::warn!(
            scale,
            "no FSR constant for this scale (1/3, 0.4, 0.5, 0.58, 0.6, 2/3, 0.75, 1); FSR keeps its own (use {OWN_UPSCALER_ENV}=1)"
        ),
    }
    patches
}

/// The scale `LEONIDA_RENDER_SCALE` asks for, if valid.
fn parse_scale(text: &str) -> Option<f32> {
    let v: f32 = text.trim().parse().ok()?;
    (0.2..=1.0).contains(&v).then_some(v)
}

/// Applies the resolution levers asked for in the environment
/// ([`RENDER_SCALE_ENV`], [`OWN_UPSCALER_ENV`], [`FSR_UPSCALER_ENV`], [`OUTPUT_ENV`]). Returns how many applied.
///
/// # Safety
/// As `disarm_watchdog`.
pub(crate) unsafe fn resolution_levers(base: usize, hash: &blake3::Hash) -> usize {
    let scale = std::env::var(RENDER_SCALE_ENV).ok();
    let own = std::env::var_os(OWN_UPSCALER_ENV).is_some_and(|v| v != "0");
    let fsr = std::env::var_os(FSR_UPSCALER_ENV).is_some_and(|v| v == "1");
    let output = std::env::var(OUTPUT_ENV).ok().filter(|v| !v.trim().is_empty()).or_else(|| {
        std::env::var_os(OUTPUT_1440_ENV).is_some_and(|v| v == "1").then(|| "2560x1440".to_owned())
    });
    if scale.is_none() && !own && !fsr && output.is_none() {
        return 0;
    }
    // Every image the loader maps comes here (the title's DLLs too); only
    // the 1.0.2.1 executable has the patched places.
    if !hash.to_hex().starts_with(BUILD_1_0_2_1) {
        tracing::debug!("resolution levers: not the 1.0.2.1 executable; nothing patched");
        return 0;
    }
    let mut applied = 0;
    if let Some(text) = output {
        match parse_output(&text) {
            // The title's own output.
            Some((3840, 2160)) => {}
            Some((width, height)) => {
                tracing::warn!(width, height, "{OUTPUT_ENV}: experimental, the title's largest output mode is changed");
                // SAFETY: forwarded from this function's contract.
                applied += unsafe { apply_all(base, &output_patches(width, height)) };
            }
            None => tracing::warn!(text, "{OUTPUT_ENV} wants WIDTHxHEIGHT, even, from 1280x720 to 7680x4320"),
        }
    }
    if own {
        if fsr {
            tracing::warn!("{OWN_UPSCALER_ENV} and {FSR_UPSCALER_ENV} are both set: the title's own upscaler it is");
        }
        // SAFETY: forwarded from this function's contract.
        applied += usize::from(unsafe { apply(base, &OWN_UPSCALER) });
    } else if fsr {
        // SAFETY: forwarded from this function's contract.
        applied += usize::from(unsafe { apply(base, &FSR_UPSCALER) });
    }
    if let Some(text) = scale {
        match parse_scale(&text) {
            Some(scale) => {
                // With the own upscaler forced, FSR is left as it is.
                for p in render_scale_patches(base, scale, !own) {
                    // SAFETY: forwarded from this function's contract.
                    applied += usize::from(unsafe { apply(base, &p.borrowed()) });
                }
            }
            None => tracing::warn!(text, "{RENDER_SCALE_ENV} wants a number in 0.2..=1.0"),
        }
    }
    applied
}

/// Applies `patches` if every one of them finds its bytes, and none of them otherwise. Returns how many were
/// written.
///
/// # Safety
/// As `disarm_watchdog`.
unsafe fn apply_all(base: usize, patches: &[OwnedPatch]) -> usize {
    let there = |p: &OwnedPatch| {
        // SAFETY: the offset is inside the mapped image of this exact build; only read here.
        let now = unsafe { std::slice::from_raw_parts((base + p.rva) as *const u8, p.expect.len()) };
        now == p.expect && p.new.len() == p.expect.len()
    };
    if let Some(missing) = patches.iter().find(|p| !there(p)) {
        tracing::warn!(what = missing.what, "hooks skipped, all that go with it too: the bytes are not the expected ones");
        return 0;
    }
    patches
        .iter()
        // SAFETY: forwarded from this function's contract.
        .filter(|p| unsafe { apply(base, &p.borrowed()) })
        .count()
}

/// Leonida: applies the game patches of `vipatch.rs` (after VI-Patch, `docs/vi_patch.md`) to the title's
/// executable mapped at `base`: 60 frames a second always, the others by their switches. Each place is found by
/// its pattern in the image's code (at the offset known for 1.0.2.1, or by a search in another build) and a patch
/// is written only if all its places are found. Returns how many places were changed.
///
/// # Safety
/// As `disarm_watchdog`; `info` is the image's.
pub(crate) unsafe fn game_patches(base: usize, info: &leonida_pe::ImageInfo) -> usize {
    use crate::vipatch;
    let mut writes = Vec::new();
    {
        // The image's code as it is mapped. The slices end with this block, before anything is written.
        let code: Vec<(usize, &[u8])> = info
            .sections
            .iter()
            .filter(|s| s.characteristics & vipatch::SCN_MEM_EXECUTE != 0)
            .filter(|s| {
                u64::from(s.rva) + u64::from(s.virtual_size) <= u64::from(info.size_of_image)
            })
            .map(|s| {
                let (start, len) = (s.rva as usize, s.virtual_size as usize);
                // SAFETY: a section of the mapped image, readable, which nothing writes yet.
                let bytes = unsafe { std::slice::from_raw_parts((base + start) as *const u8, len) };
                (start, bytes)
            })
            .collect();
        for patch in vipatch::PATCHES.iter().filter(|p| p.wanted()) {
            match vipatch::plan(patch, &code) {
                Ok(places) => writes.extend(places),
                Err(miss) => tracing::warn!(patch = patch.name, "game patch not applied: {miss}"),
            }
        }
    }
    writes
        .iter()
        .filter(|w| {
            let p = Patch {
                what: w.what,
                rva: w.rva,
                expect: &w.expect,
                new: w.new,
            };
            // SAFETY: forwarded from this function's contract; the place is in the image's code.
            unsafe { apply(base, &p) }
        })
        .count()
}

/// # Safety
/// See `disarm_watchdog`.
pub(crate) unsafe fn apply(base: usize, p: &Patch<'_>) -> bool {
    let at = (base + p.rva) as *mut u8;
    // SAFETY: the offset is inside the mapped image of this exact build (.text, or .rdata for the mode table).
    let now = unsafe { std::slice::from_raw_parts(at, p.expect.len()) };
    if now != p.expect || p.new.len() != p.expect.len() {
        tracing::warn!(what = p.what, "hook skipped: the bytes are not the expected ones");
        return false;
    }
    let mut old = 0;
    // SAFETY: changing the protection of those bytes only, restored below.
    if unsafe { VirtualProtect(at as _, p.new.len(), PAGE_EXECUTE_READWRITE, &mut old) } == 0 {
        return false;
    }
    // SAFETY: writable now; the bytes are the ones checked above.
    unsafe {
        std::ptr::copy_nonoverlapping(p.new.as_ptr(), at, p.new.len());
        VirtualProtect(at as _, p.new.len(), old, &mut old);
        FlushInstructionCache(GetCurrentProcess(), at as _, p.new.len());
    }
    tracing::info!(what = p.what, "hook applied");
    true
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_output_patches_change_the_mode_row_the_reservation_and_the_settings_viewport() {
        let pair = |b: &[u8]| {
            (u32::from_le_bytes(b[0..4].try_into().unwrap()), u32::from_le_bytes(b[4..8].try_into().unwrap()))
        };
        let p = output_patches(2560, 1440);
        assert_eq!(p.len(), 3);
        assert_eq!((p[0].rva, pair(&p[0].expect), pair(&p[0].new)), (0x615c100, (3840, 2160), (2560, 1440)));
        // mov ecx, imm32: the title's own 0x4000000 before, 240 tiles of 64 KiB twice, to 2 MiB, after.
        assert_eq!(p[1].expect, [0xB9, 0, 0, 0, 0x04]);
        assert_eq!(p[1].new, [0xB9, 0, 0, 0xE0, 0x01]);
        // The viewport's load reads the mode row: its displacement from the next instruction.
        assert_eq!(p[2].new, [0xC5, 0xFB, 0x10, 0x0D, 0x24, 0x78, 0x95, 0x00]);
        assert_eq!(0x58048d4 + 8 + 0x957824, 0x615c100);
        assert!(p.iter().all(|p| p.new.len() == p.expect.len() && p.new != p.expect));
    }

    #[test]
    fn the_scanout_reservation_is_two_tiled_buffers_to_two_mib() {
        assert_eq!(scanout_pool_bytes(3840, 2160), 0x400_0000);
        assert_eq!(scanout_pool_bytes(2560, 1440), 0x1E0_0000);
        // 21:9 and 32:9 at the title's height, and a 3440x1440 monitor.
        assert_eq!(scanout_pool_bytes(5040, 2160), 0x560_0000);
        assert_eq!(scanout_pool_bytes(7680, 2160), 0x800_0000);
        assert_eq!(scanout_pool_bytes(3440, 1440), 0x2A0_0000);
    }

    #[test]
    fn output_sizes_are_even_and_within_the_mode_table() {
        assert_eq!(parse_output("5040x2160"), Some((5040, 2160)));
        assert_eq!(parse_output(" 2560X1440 "), Some((2560, 1440)));
        for wrong in ["", "2560", "2561x1440", "640x360", "9000x2160", "21:9", "x", "2560x"] {
            assert_eq!(parse_output(wrong), None, "{wrong:?}");
        }
    }

    #[test]
    fn the_fidelityfx_patch_gives_every_console_the_series_s_answer() {
        // test al, al -> or al, 1; the mov eax, 6 and the cmovne eax, ecx after it stay.
        assert_eq!(FSR_UPSCALER.expect[..2], [0x84, 0xC0]);
        assert_eq!(FSR_UPSCALER.new[..2], [0x0C, 0x01]);
        assert_eq!(FSR_UPSCALER.expect[2..], FSR_UPSCALER.new[2..]);
        // The own-upscaler patch is the cmovne of the same choice: one or the other.
        assert_eq!(FSR_UPSCALER.rva + 7, OWN_UPSCALER.rva);
        assert_eq!(FSR_UPSCALER.expect[7..], *OWN_UPSCALER.expect);
    }

    #[test]
    fn a_scale_one_of_the_fidelityfx_loads_reads_already_leaves_that_load() {
        let base = 0x7ff0_0000_0000;
        // 2/3 is Series S' own: only the other consoles' load changes.
        let p = render_scale_patches(base, 0.6667, true);
        assert_eq!(p.iter().map(|p| p.rva).collect::<Vec<_>>(), [0x28f1e7e, 0x28f1e87, FSR_LOAD_OTHER, 0x6300ba8]);
        assert_eq!(p[2].expect, [0xC5, 0xFA, 0x10, 0x05, 0x28, 0x78, 0x6D, 0x03]);
        assert_eq!(FSR_LOAD_OTHER + 8 + 0x36d7828, FSR_SCALE_OTHER.1);
        let disp = i32::from_le_bytes(p[2].new[4..].try_into().unwrap());
        assert_eq!((FSR_LOAD_OTHER + 8).wrapping_add_signed(disp as isize), FSR_SCALE_S.1);
        // 1.0 is the other consoles' own: only Series S' load changes.
        let p = render_scale_patches(base, 1.0, true);
        assert_eq!(p.iter().map(|p| p.rva).collect::<Vec<_>>(), [0x28f1e7e, 0x28f1e87, FSR_LOAD_S, 0x6300ba8]);
    }

    /// With `LEONIDA_GAME_DIR` set to the folder of the Xbox package and the 1.0.2.1 executable in it: every
    /// place of the resolution levers holds the bytes expected, and every scale constant its value.
    #[test]
    fn the_resolution_places_are_in_the_retail_executable() {
        let Some(dir) = std::env::var_os("LEONIDA_GAME_DIR") else {
            eprintln!("SKIP: LEONIDA_GAME_DIR is not set");
            return;
        };
        let path = std::path::Path::new(&dir).join("game_scarlett_master.exe");
        let Ok(data) = std::fs::read(&path) else {
            eprintln!("SKIP: {} is not there", path.display());
            return;
        };
        if !is_build_1_0_2_1(&blake3::hash(&data)) {
            eprintln!("SKIP: not the 1.0.2.1 executable");
            return;
        }
        let info = leonida_pe::ImageInfo::parse(&data).expect("a PE image");
        let at = |rva: usize, len: usize| -> &[u8] {
            let s = info
                .sections
                .iter()
                .find(|s| (s.rva as usize..s.rva as usize + s.file_size as usize).contains(&rva))
                .unwrap_or_else(|| panic!("{rva:#x} is in no section's file bytes"));
            let offset = s.file_offset as usize + rva - s.rva as usize;
            &data[offset..offset + len]
        };
        for p in [&FSR_UPSCALER, &OWN_UPSCALER] {
            assert_eq!(at(p.rva, p.expect.len()), p.expect, "{}", p.what);
        }
        // The image's base does not matter to these: the vtable slot (a relocated pointer) is left out.
        let scales = render_scale_patches(0, 0.5, true);
        for p in output_patches(2560, 1440).iter().chain(scales.iter().filter(|p| p.rva != 0x6300ba8)) {
            assert_eq!(at(p.rva, p.expect.len()), p.expect, "{}", p.what);
        }
        for (value, rva) in FSR_SCALES {
            assert_eq!(at(rva, 4), value.to_le_bytes(), "{value} at {rva:#x}");
        }
    }

    #[test]
    fn scales_outside_the_floor_are_refused() {
        assert_eq!(parse_scale("0.5"), Some(0.5));
        assert_eq!(parse_scale(" 1 "), Some(1.0));
        assert_eq!(parse_scale("0.1"), None);
        assert_eq!(parse_scale("1.5"), None);
        assert_eq!(parse_scale("half"), None);
    }

    #[test]
    fn half_scale_patches_both_upscalers() {
        let base = 0x7ff0_0000_0000;
        let p = render_scale_patches(base, 0.5, true);
        assert_eq!(p.len(), 5);
        assert_eq!(p[0].new, 0.5f32.to_le_bytes());
        // The FSR loads now read 0.5 at 0x5fc6edc: disp from the next instruction. Series S' (2/3 before), then
        // the other consoles' (1.0 before).
        assert_eq!(p[2].expect, [0xC5, 0xFA, 0x10, 0x05, 0xA2, 0xE2, 0x86, 0x03]);
        assert_eq!(p[2].new, [0xC5, 0xFA, 0x10, 0x05, 0x22, 0x78, 0x6D, 0x03]);
        assert_eq!(p[3].expect, [0xC5, 0xFA, 0x10, 0x05, 0x28, 0x78, 0x6D, 0x03]);
        assert_eq!(p[3].new, [0xC5, 0xFA, 0x10, 0x05, 0x2C, 0x78, 0x6D, 0x03]);
        assert_eq!(p[4].new, ((base + 0x28ef680) as u64).to_le_bytes());
        assert!(p.iter().all(|p| p.new.len() == p.expect.len()));
    }

    #[test]
    fn the_game_patches_are_written_into_the_code_of_an_image() {
        use crate::vipatch::{PATCHES, SCN_MEM_EXECUTE};
        // A made-up image: one section of code with the bytes each pattern names (its `??` as zeros), away
        // from the offsets known for the title, so each place is found by the search.
        let mut image = vec![0xCCu8; 0x3000];
        let mut places = Vec::new();
        for (i, (patch, site)) in PATCHES
            .iter()
            .flat_map(|p| p.sites.iter().map(move |s| (p, s)))
            .enumerate()
        {
            let bytes: Vec<u8> = site
                .pattern
                .split_ascii_whitespace()
                .map(|t| u8::from_str_radix(t, 16).unwrap_or(0))
                .collect();
            let at = 0x1010 + i * 0x100;
            image[at..at + bytes.len()].copy_from_slice(&bytes);
            let old = bytes[site.at..site.at + site.new.len()].to_vec();
            places.push((patch.wanted(), at + site.at, old, site.new));
        }
        let info = leonida_pe::ImageInfo {
            machine: leonida_pe::MACHINE_AMD64,
            characteristics: 0,
            timestamp: 0,
            subsystem: 0,
            dll_characteristics: 0,
            image_base: 0,
            size_of_image: image.len() as u32,
            entry_rva: 0,
            stack_reserve: 0,
            stack_commit: 0,
            sections: vec![leonida_pe::SectionInfo {
                name: ".text".to_owned(),
                rva: 0x1000,
                virtual_size: 0x2000,
                file_offset: 0,
                file_size: 0,
                characteristics: SCN_MEM_EXECUTE,
            }],
            imports: Vec::new(),
            delay_imports: Vec::new(),
            exports: Vec::new(),
            tls: None,
            has_relocations: false,
            has_exception_table: false,
            has_load_config: false,
            codeview: None,
        };
        // SAFETY: `image` is memory of ours of the size `info` says, and nothing else uses it.
        let applied = unsafe { game_patches(image.as_mut_ptr() as usize, &info) };
        assert_eq!(applied, places.iter().filter(|p| p.0).count());
        // The frame-rate patch has no switch: its two places at least.
        assert!(applied >= 2);
        for (wanted, at, old, new) in places {
            let now = &image[at..at + new.len()];
            assert_eq!(now, if wanted { new } else { &old[..] }, "{at:#x}");
        }
        // A second pass finds nothing to change: the patterns are gone where they were written.
        // SAFETY: as above.
        let again = unsafe { game_patches(image.as_mut_ptr() as usize, &info) };
        assert_eq!(again, 0);
    }

    #[test]
    fn odd_scales_patch_only_the_own_upscaler() {
        assert_eq!(render_scale_patches(0, 0.55, true).len(), 2);
        assert_eq!(render_scale_patches(0, 0.5, false).len(), 2);
    }
}

