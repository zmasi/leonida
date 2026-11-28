//! Game patches after ZiZc3's VI-Patch (<https://github.com/ZiZc3/VI-Patch>), a patch set for the PS5 build of
//! the title: the same changes, made to the Xbox Series executable. `docs/vi_patch.md` lists them all, with the
//! ones that are not here and why.
//!
//! VI-Patch has no licence, so nothing of it is copied. Its patches are addresses and bytes of the PS5
//! executable, which are of no use here anyway: taken is what a patch does (its README says), and each place
//! below was found in the Xbox executable by what the code there does. The bytes are this file's own.
//!
//! A place is named by a short pattern of its bytes (`??`: any byte, for the displacements another build of the
//! title changes) and is found by it: at the offset known for the 1.0.2.1 executable if the pattern is there, or
//! by a search of the image's code that must find it exactly once. Nothing of a patch is written unless every
//! place of it is found. Only patterns and offsets are here, nothing of the title itself.
//!
//! The patterns, the table and the search are plain code for every host; writing into the mapped image is
//! `hooks::game_patches`, on Windows.
#![cfg_attr(not(windows), allow(dead_code))]

use std::fmt;

/// `IMAGE_SCN_MEM_EXECUTE`: a section of code.
pub(crate) const SCN_MEM_EXECUTE: u32 = 0x2000_0000;

/// A run of bytes to look for; `None` is any byte.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct Pattern(Vec<Option<u8>>);

impl Pattern {
    /// From text like `"8B 86 ?? ?? 00 00"`: two hexadecimal digits a byte, `??` any byte. A pattern with no
    /// byte to look for is none.
    pub(crate) fn parse(text: &str) -> Option<Pattern> {
        let bytes = text
            .split_ascii_whitespace()
            .map(|token| match token {
                "??" => Some(None),
                t if t.len() == 2 && t.bytes().all(|c| c.is_ascii_hexdigit()) => {
                    u8::from_str_radix(t, 16).ok().map(Some)
                }
                _ => None,
            })
            .collect::<Option<Vec<_>>>()?;
        bytes.iter().any(Option::is_some).then_some(Pattern(bytes))
    }

    /// Whether `bytes` begins with the pattern.
    pub(crate) fn matches(&self, bytes: &[u8]) -> bool {
        bytes.len() >= self.0.len()
            && self
                .0
                .iter()
                .zip(bytes)
                .all(|(want, have)| want.is_none_or(|w| w == *have))
    }

    /// The offsets in `code` the pattern is at, in order, `limit` of them at most.
    pub(crate) fn find(&self, code: &[u8], limit: usize) -> Vec<usize> {
        let mut found = Vec::new();
        // The first byte that is not `??` leads the search.
        let Some((lead, byte)) = self
            .0
            .iter()
            .enumerate()
            .find_map(|(i, b)| b.map(|b| (i, b)))
        else {
            return found;
        };
        let mut from = lead;
        while found.len() < limit && from < code.len() {
            let Some(step) = code[from..].iter().position(|&b| b == byte) else {
                break;
            };
            let start = from + step - lead;
            if self.matches(&code[start..]) {
                found.push(start);
            }
            from += step + 1;
        }
        found
    }

    /// The `len` bytes of the pattern from `at`, if they are in it and none of them is `??`.
    pub(crate) fn fixed(&self, at: usize, len: usize) -> Option<Vec<u8>> {
        self.0
            .get(at..at.checked_add(len)?)?
            .iter()
            .copied()
            .collect()
    }
}

/// One place a patch changes.
pub(crate) struct Site {
    /// What the change is, for the log.
    pub(crate) what: &'static str,
    /// The bytes there before the change ([`Pattern::parse`]).
    pub(crate) pattern: &'static str,
    /// Where in the pattern the changed bytes begin.
    pub(crate) at: usize,
    /// The bytes written, over as many of the pattern's (which are the bytes expected there).
    pub(crate) new: &'static [u8],
    /// The pattern's offset in the image of the 1.0.2.1 executable.
    pub(crate) rva: usize,
}

/// A game patch: its places, and the switch it is behind.
pub(crate) struct GamePatch {
    pub(crate) name: &'static str,
    /// The environment variable that turns it on (`=1`); none: always on.
    pub(crate) env: Option<&'static str>,
    pub(crate) sites: &'static [Site],
}

impl GamePatch {
    /// Whether this run wants the patch.
    pub(crate) fn wanted(&self) -> bool {
        self.env
            .is_none_or(|var| std::env::var_os(var).is_some_and(|v| v == "1"))
    }
}

/// `mov eax, 60`, then a one-byte `nop`: over the six bytes of a `mov eax, [register + disp32]`.
const SIXTY: &[u8] = &[0xB8, 60, 0, 0, 0, 0x90];

/// 60 frames a second, always on.
///
/// The title's video settings hold a frame-rate limit. Two functions that work on those settings turn it into the
/// frame interval, with the same code: load the limit, bound it to 1000 (0, no limit, is 1000 too), store
/// 1 000 000 / limit as the interval in microseconds. From that interval come the frames a second the title asks for, its frame lock (output refresh
/// / rate, 4 at most), the length it gives `SetFrameIntervalX` (a table by output refresh and frame lock: 16 667
/// or 33 333 at 60 Hz) and the wait of its own limiter on the game side. Untouched it asks for 33 333, 30 frames
/// a second. Here the load of the limit becomes `mov eax, 60` in both places, so every one of those follows: 60
/// at a 60 Hz output (frame lock 1) and at a 120 Hz one (frame lock 2).
///
/// That code (the two bounds of 1000 and the division into 1 000 000) is in the executable twice, these two.
pub(crate) const FPS_60: GamePatch = GamePatch {
    name: "60 fps",
    env: None,
    sites: &[
        Site {
            what: "60 fps: the frame limit the video settings set (first of two places)",
            pattern: "8B 86 10 02 00 00 BA E8 03 00 00 C5 FA 10 0D ?? ?? ?? ?? B9 E8 03 00 00 3D E8 03 00 00",
            at: 0,
            new: SIXTY,
            rva: 0x224359a,
        },
        Site {
            what: "60 fps: the frame limit the video settings set (second of two places)",
            pattern: "8B 87 10 02 00 00 BA E8 03 00 00 C5 FA 10 0D ?? ?? ?? ?? B9 E8 03 00 00 3D E8 03 00 00",
            at: 0,
            new: SIXTY,
            rva: 0x1e0fc62,
        },
    ],
};

/// Environment variable of [`NO_VIGNETTE`].
pub(crate) const NO_VIGNETTE_ENV: &str = "LEONIDA_VIPATCH_NO_VIGNETTE";

/// No dark corners when aiming or driving fast.
///
/// The title blends exposure ("spot meter") profiles, looked up by the `joaat` of their names, in one function:
/// a profile for each state that is on, the default one last. VI-Patch's notes name the two that darken the
/// corners, `SPOTMETER_aiming` and `SPOTMETER_fast`; their hashes (of the lower-cased names: `0x1791bd90`,
/// `0xf2b6fe6c`) are each in the executable once, in that function, each after a conditional jump over its
/// lookup and blend. Both jumps (`jne`, `75`) become unconditional (`jmp`, `EB`), so the two profiles are never
/// blended in and the others stay.
pub(crate) const NO_VIGNETTE: GamePatch = GamePatch {
    name: "no vignette",
    env: Some(NO_VIGNETTE_ENV),
    sites: &[
        Site {
            what: "no vignette: the exposure profile for aiming is skipped",
            // cmp byte [aiming], 1; jne +27h; lea rcx, [profiles]; mov edx, joaat("spotmeter_aiming")
            pattern: "80 3D ?? ?? ?? ?? 01 75 27 48 8D 0D ?? ?? ?? ?? BA 90 BD 91 17",
            at: 7,
            new: &[0xEB],
            rva: 0x496aaf,
        },
        Site {
            what: "no vignette: the exposure profile for fast driving is skipped",
            // test cl, 1; jne +43h; mov ecx, hash; xor edx, edx; call; test al, al; mov eax, hash;
            // mov edx, joaat("spotmeter_fast")
            pattern: "F6 C1 01 75 43 B9 ?? ?? ?? ?? 31 D2 E8 ?? ?? ?? ?? 84 C0 B8 ?? ?? ?? ?? BA 6C FE B6 F2",
            at: 3,
            new: &[0xEB],
            rva: 0x496b4e,
        },
    ],
};

/// Every patch, in the order they are applied.
pub(crate) const PATCHES: &[&GamePatch] = &[&FPS_60, &NO_VIGNETTE];

/// One change to make: `new` over `expect` at `rva` of the image.
#[derive(Debug, PartialEq, Eq)]
pub(crate) struct Write {
    pub(crate) what: &'static str,
    pub(crate) rva: usize,
    pub(crate) expect: Vec<u8>,
    pub(crate) new: &'static [u8],
}

/// Why a patch is not applied; each names the place.
#[derive(Debug, PartialEq, Eq)]
pub(crate) enum Miss {
    /// The table's entry is wrong (a pattern that does not read, changed bytes outside it or `??` among them).
    Entry(&'static str),
    /// The pattern is nowhere in the image's code: another build of the title.
    NotFound(&'static str),
    /// The pattern is there more than once, so it does not name a place.
    Several(&'static str),
}

impl fmt::Display for Miss {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Miss::Entry(what) => write!(f, "{what}: the patch table's entry is wrong"),
            Miss::NotFound(what) => write!(f, "{what}: its code is not in this executable"),
            Miss::Several(what) => {
                write!(f, "{what}: its code is in this executable more than once")
            }
        }
    }
}

/// Where `pattern` is in `code`, the image's executable sections (offset in the image, bytes): at `hint` if it
/// is there, else the only place a search finds.
fn locate(
    what: &'static str,
    pattern: &Pattern,
    hint: usize,
    code: &[(usize, &[u8])],
) -> Result<usize, Miss> {
    let at_hint = code.iter().any(|&(start, bytes)| {
        hint.checked_sub(start)
            .and_then(|offset| bytes.get(offset..))
            .is_some_and(|b| pattern.matches(b))
    });
    if at_hint {
        return Ok(hint);
    }
    let mut found = Vec::new();
    for &(start, bytes) in code {
        found.extend(
            pattern
                .find(bytes, 2 - found.len())
                .into_iter()
                .map(|offset| start + offset),
        );
        if found.len() > 1 {
            return Err(Miss::Several(what));
        }
    }
    found.first().copied().ok_or(Miss::NotFound(what))
}

/// The changes `patch` makes to an image whose executable sections are `code` (offset in the image, bytes):
/// one for each of its places, or the first place that cannot be changed.
pub(crate) fn plan(patch: &GamePatch, code: &[(usize, &[u8])]) -> Result<Vec<Write>, Miss> {
    patch
        .sites
        .iter()
        .map(|site| {
            let pattern = Pattern::parse(site.pattern).ok_or(Miss::Entry(site.what))?;
            let expect = pattern
                .fixed(site.at, site.new.len())
                .ok_or(Miss::Entry(site.what))?;
            let rva = locate(site.what, &pattern, site.rva, code)?;
            Ok(Write {
                what: site.what,
                rva: rva + site.at,
                expect,
                new: site.new,
            })
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn pattern(text: &str) -> Pattern {
        Pattern::parse(text).expect("a pattern")
    }

    #[test]
    fn patterns_read_bytes_and_wildcards() {
        assert_eq!(
            pattern("8B 86 ?? 0a").0,
            [Some(0x8B), Some(0x86), None, Some(0x0A)]
        );
        assert_eq!(pattern("  75\t27 ").0.len(), 2);
        for wrong in [
            "", "??", "?? ??", "8", "8B8", "8B 86 ?", "GG", "+1", "8B,86",
        ] {
            assert_eq!(Pattern::parse(wrong), None, "{wrong:?}");
        }
    }

    #[test]
    fn a_pattern_matches_at_the_start_only_and_needs_all_its_bytes() {
        let p = pattern("10 ?? 30");
        assert!(p.matches(&[0x10, 0x20, 0x30]));
        assert!(p.matches(&[0x10, 0xFF, 0x30, 0x40]));
        assert!(!p.matches(&[0x10, 0x20, 0x31]));
        assert!(!p.matches(&[0x00, 0x10, 0x20, 0x30]));
        assert!(!p.matches(&[0x10, 0x20]));
        assert!(!p.matches(&[]));
    }

    #[test]
    fn find_gives_every_place_in_order_up_to_the_limit() {
        let code = [0x10, 0x10, 0x20, 0x30, 0x00, 0x10, 0x55, 0x30, 0x10, 0x20];
        let p = pattern("10 ?? 30");
        assert_eq!(p.find(&code, usize::MAX), [1, 5]);
        assert_eq!(p.find(&code, 1), [1]);
        assert!(p.find(&code, 0).is_empty());
        // Overlapping places are all found, and one that would run past the end is none.
        assert_eq!(
            pattern("10 10").find(&[0x10, 0x10, 0x10], usize::MAX),
            [0, 1]
        );
        assert_eq!(pattern("10 20 ??").find(&code, usize::MAX), [1]);
        assert!(p.find(&[], usize::MAX).is_empty());
    }

    #[test]
    fn find_with_wildcards_first_starts_before_its_leading_byte() {
        let p = pattern("?? ?? 30 40");
        assert_eq!(
            p.find(&[0x30, 0x40, 0x30, 0x40, 0x30, 0x40], usize::MAX),
            [0, 2]
        );
        // The leading byte too near the start for the wildcards before it.
        assert!(p.find(&[0x00, 0x30, 0x40], usize::MAX).is_empty());
    }

    #[test]
    fn fixed_bytes_are_none_with_a_wildcard_or_past_the_end() {
        let p = pattern("10 ?? 30 40");
        assert_eq!(p.fixed(2, 2), Some(vec![0x30, 0x40]));
        assert_eq!(p.fixed(0, 2), None);
        assert_eq!(p.fixed(3, 2), None);
        assert_eq!(p.fixed(usize::MAX, 2), None);
    }

    const TWO: GamePatch = GamePatch {
        name: "test",
        env: None,
        sites: &[
            Site {
                what: "first",
                pattern: "A1 ?? A3 A4",
                at: 2,
                new: &[0xB3, 0xB4],
                rva: 0x1002,
            },
            Site {
                what: "second",
                pattern: "C1 C2 ??",
                at: 0,
                new: &[0xD1],
                rva: 0x9999,
            },
        ],
    };

    #[test]
    fn a_plan_takes_the_known_offset_or_the_only_place_found() {
        // "first" is at its known offset (and once more, which the known offset settles); "second" is not at
        // its own and is found by the search, in the other section.
        let text = [0, 0, 0xA1, 0x77, 0xA3, 0xA4, 0xA1, 0x00, 0xA3, 0xA4];
        let more = [0xC1, 0xC2, 0x00];
        let code = [(0x1000, &text[..]), (0x3000, &more[..])];
        let writes = plan(&TWO, &code).expect("both places");
        assert_eq!(
            writes,
            [
                Write {
                    what: "first",
                    rva: 0x1004,
                    expect: vec![0xA3, 0xA4],
                    new: &[0xB3, 0xB4]
                },
                Write {
                    what: "second",
                    rva: 0x3000,
                    expect: vec![0xC1],
                    new: &[0xD1]
                },
            ]
        );
    }

    #[test]
    fn a_plan_is_all_of_a_patch_or_nothing() {
        let text = [0, 0, 0xA1, 0x77, 0xA3, 0xA4];
        assert_eq!(
            plan(&TWO, &[(0x1000, &text[..])]),
            Err(Miss::NotFound("second"))
        );
        // Twice with no known offset to settle it: not a place. Across two sections too.
        let twice = [0xC1, 0xC2, 0x00, 0xC1, 0xC2, 0x01];
        assert_eq!(
            plan(&TWO, &[(0x1000, &text[..]), (0x3000, &twice[..])]),
            Err(Miss::Several("second"))
        );
        let once = [0xC1, 0xC2, 0x00];
        let code = [
            (0x1000, &text[..]),
            (0x3000, &once[..]),
            (0x5000, &once[..]),
        ];
        assert_eq!(plan(&TWO, &code), Err(Miss::Several("second")));
        assert_eq!(plan(&TWO, &[]), Err(Miss::NotFound("first")));
    }

    #[test]
    fn a_wrong_entry_is_refused() {
        const WILD: GamePatch = GamePatch {
            name: "test",
            env: None,
            sites: &[Site {
                what: "wild",
                pattern: "A1 ?? A3",
                at: 1,
                new: &[0],
                rva: 0,
            }],
        };
        const LONG: GamePatch = GamePatch {
            name: "test",
            env: None,
            sites: &[Site {
                what: "long",
                pattern: "A1 A2",
                at: 1,
                new: &[0, 0],
                rva: 0,
            }],
        };
        let text = [0xA1, 0xA2, 0xA3];
        assert_eq!(plan(&WILD, &[(0, &text[..])]), Err(Miss::Entry("wild")));
        assert_eq!(plan(&LONG, &[(0, &text[..])]), Err(Miss::Entry("long")));
    }

    #[test]
    fn every_entry_of_the_table_changes_bytes_its_pattern_names() {
        let mut switches = Vec::new();
        for patch in PATCHES {
            assert!(!patch.sites.is_empty(), "{}", patch.name);
            for site in patch.sites {
                let pattern = Pattern::parse(site.pattern)
                    .unwrap_or_else(|| panic!("{}: its pattern", site.what));
                // The bytes replaced are bytes of the pattern (none `??`), as many as the new ones, and differ.
                let expect = pattern
                    .fixed(site.at, site.new.len())
                    .unwrap_or_else(|| panic!("{}: its bytes", site.what));
                assert!(!site.new.is_empty(), "{}", site.what);
                assert_eq!(expect.len(), site.new.len(), "{}", site.what);
                assert_ne!(expect, site.new, "{}", site.what);
                // Enough fixed bytes to name a place.
                assert!(pattern.0.iter().flatten().count() >= 12, "{}", site.what);
                // A plan over the pattern's own bytes (the wildcards as zeros) finds it wherever it lies.
                let bytes: Vec<u8> = pattern.0.iter().map(|b| b.unwrap_or(0)).collect();
                let lone = GamePatch {
                    name: patch.name,
                    env: None,
                    sites: std::slice::from_ref(site),
                };
                let writes = plan(&lone, &[(0x4000, &bytes[..])]).expect("found");
                assert_eq!(writes[0].rva, 0x4000 + site.at);
                assert_eq!(writes[0].expect, expect);
            }
            if let Some(var) = patch.env {
                assert!(var.starts_with("LEONIDA_VIPATCH_"), "{var}");
                assert!(!switches.contains(&var), "{var} twice");
                switches.push(var);
            }
        }
        // The frame-rate patch has no switch.
        assert!(FPS_60.env.is_none() && FPS_60.wanted());
    }

    #[test]
    fn the_frame_rate_patch_loads_sixty() {
        for site in FPS_60.sites {
            // A six-byte `mov eax, [register + 210h]` becomes `mov eax, 60; nop`.
            let pattern = pattern(site.pattern);
            let old = pattern.fixed(0, 6).expect("the load");
            assert_eq!(
                (old[0], old[1] >> 6, old[1] >> 3 & 7),
                (0x8B, 0b10, 0),
                "{}",
                site.what
            );
            assert_eq!(site.new, [0xB8, 60, 0, 0, 0, 0x90]);
            assert_eq!(u32::from_le_bytes(site.new[1..5].try_into().unwrap()), 60);
        }
        assert_eq!(FPS_60.sites.len(), 2);
    }

    /// Jenkins one-at-a-time of the lower-cased name: the title's hash of names.
    fn joaat(name: &str) -> u32 {
        let mut h = 0u32;
        for c in name.to_ascii_lowercase().bytes() {
            h = h.wrapping_add(u32::from(c));
            h = h.wrapping_add(h << 10);
            h ^= h >> 6;
        }
        h = h.wrapping_add(h << 3);
        h ^= h >> 11;
        h.wrapping_add(h << 15)
    }

    #[test]
    fn the_vignette_patch_skips_the_two_named_profiles() {
        assert_eq!(joaat("a"), 0xca2e9442);
        for (site, name) in NO_VIGNETTE
            .sites
            .iter()
            .zip(["SPOTMETER_aiming", "SPOTMETER_fast"])
        {
            let pattern = pattern(site.pattern);
            // The pattern ends with `mov edx, <the profile's hash>`.
            let len = pattern.0.len();
            let tail = pattern.fixed(len - 5, 5).expect("the hash");
            assert_eq!(tail[0], 0xBA, "{}", site.what);
            assert_eq!(
                u32::from_le_bytes(tail[1..].try_into().unwrap()),
                joaat(name),
                "{name}"
            );
            // A short `jne` becomes a short `jmp` with the same distance, which lands at the pattern's end or after.
            assert_eq!(pattern.fixed(site.at, 1), Some(vec![0x75]));
            assert_eq!(site.new, [0xEB]);
            let distance = pattern.fixed(site.at + 1, 1).expect("the distance")[0] as usize;
            assert!(site.at + 2 + distance >= len, "{}", site.what);
        }
        assert_eq!(NO_VIGNETTE.env, Some("LEONIDA_VIPATCH_NO_VIGNETTE"));
    }

    /// With `LEONIDA_GAME_DIR` set to the folder of the Xbox package: every place's pattern is in the code of
    /// `game_scarlett_master.exe` exactly once, and for the 1.0.2.1 executable at the offset the table gives.
    #[test]
    fn every_place_is_in_the_retail_executable_once() {
        let Some(dir) = std::env::var_os("LEONIDA_GAME_DIR") else {
            eprintln!("SKIP: LEONIDA_GAME_DIR is not set");
            return;
        };
        let path = std::path::Path::new(&dir).join("game_scarlett_master.exe");
        let Ok(data) = std::fs::read(&path) else {
            eprintln!("SKIP: {} is not there", path.display());
            return;
        };
        // The 1.0.2.1 executable, by the hash `hooks.rs` knows it by.
        #[cfg(windows)]
        let known = crate::hooks::is_build_1_0_2_1(&blake3::hash(&data));
        #[cfg(not(windows))]
        let known = false;
        let info = leonida_pe::ImageInfo::parse(&data).expect("a PE image");
        let code: Vec<(usize, &[u8])> = info
            .sections
            .iter()
            .filter(|s| s.characteristics & SCN_MEM_EXECUTE != 0)
            .map(|s| {
                let len = s.file_size.min(s.virtual_size) as usize;
                (
                    s.rva as usize,
                    &data[s.file_offset as usize..s.file_offset as usize + len],
                )
            })
            .collect();
        for patch in PATCHES {
            for site in patch.sites {
                let pattern = pattern(site.pattern);
                let found: Vec<usize> = code
                    .iter()
                    .flat_map(|&(start, bytes)| {
                        pattern
                            .find(bytes, usize::MAX)
                            .into_iter()
                            .map(move |o| start + o)
                    })
                    .collect();
                eprintln!(
                    "{}: {} at {found:x?} (1.0.2.1 executable: {known})",
                    patch.name, site.what
                );
                assert_eq!(found.len(), 1, "{}", site.what);
                if known {
                    assert_eq!(found[0], site.rva, "{}", site.what);
                }
            }
            assert_eq!(plan(patch, &code).map(|w| w.len()), Ok(patch.sites.len()));
        }
    }
}
