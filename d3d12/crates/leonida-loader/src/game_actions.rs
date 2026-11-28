//! Where the title's own code for the trainer menu's game actions is, and the hook that runs them once a frame
//! on the title's own thread (`docs/mod-menu.md`, "Game actions"). Off unless `LEONIDA_GAME_ACTIONS=1`.
//!
//! The title has a cheat system of its own: a function reads the pad once a frame (a step of the title's update
//! graph), and a typed cheat is run through a table of sixteen handlers that take no argument. That function is
//! where our work is done: its one call site becomes a call of ours, which calls it and then hands the title's own
//! deferred-call queue (the one its cheats go through) the work the menu asked for. So an action runs where the
//! title runs its own cheats, never on a thread of ours.
//!
//! Every place is named by a short pattern of its bytes (`??`: any byte, for what another build changes) that
//! must be in the image's code exactly once, never by an address. What a place gives (a function, a global a
//! displacement in it points at, a number in it) is read from the mapped image. Nothing of the title is here but
//! those patterns.
#![cfg_attr(not(windows), allow(dead_code))]

use crate::vipatch::Pattern;

/// The environment variable that turns the game actions on (`=1`).
pub const ENV: &str = "LEONIDA_GAME_ACTIONS";

/// Whether this run asks for the game actions.
pub fn wanted() -> bool {
    std::env::var_os(ENV).is_some_and(|v| v == "1")
}

/// What the actions need of the title, each an offset in its image (or a number read from its code).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(usize)]
pub enum Site {
    /// The `call` (its first byte) of the cheat input function, in the update step that wraps it.
    HookCall,
    /// The cheat input function: reads the pad, once a frame.
    CheatsUpdate,
    /// The title's deferred call: `(queue, closure)`; runs the closure now, or at the queue's next point of
    /// rest when the thread is inside a parallel update.
    Defer,
    /// Runs cheat `*index`: `(index: *const u32)`, through the table.
    CheatDispatch,
    /// The table of the sixteen cheat handlers (functions without arguments).
    CheatTable,
    /// The global that holds the player's ped.
    PlayerPed,
    /// Sets a ped's wanted level: `(ped, level, bool, bool, bool)`; level 0 clears it.
    WantedSet,
    /// The wanted state of an entity: `(entity id) -> pointer or null`.
    WantedGet,
    /// Number: where the level is in the wanted state.
    WantedLevelAt,
    /// Sets the time of day: `(hour, minute, second)`.
    ClockSet,
    /// The clock's time: hour, minute, second, millisecond (four `i32`).
    ClockTime,
    /// The byte that stops the clock.
    ClockPaused,
    /// Forces a weather type now: `(type index, flags)`.
    WeatherForce,
    /// Lets the weather run again: `()`.
    WeatherClear,
    /// The weather state; its first `u32` is the number of types.
    Weather,
    /// The weather's previous, next and forced type index (`u32` each; forced is -1 when none).
    WeatherPrev,
    WeatherNext,
    WeatherForced,
    /// The global that points at the weather types ([`WEATHER_TYPE_STRIDE`] bytes each, the name's hash first).
    WeatherTypes,
}

/// How many [`Site`]s there are.
pub const SITES: usize = Site::WeatherTypes as usize + 1;

/// The queue the title's own cheats are deferred to (`mov ecx, 4` in the pattern of [`Site::Defer`], whose
/// stores are the closure's four words).
pub const CHEAT_QUEUE: u32 = 4;
/// The cheat table's entries: 0 to 15 (the dispatcher does not check).
pub const CHEATS: u32 = 16;
/// Where an entity keeps its id (`mov ebx, [rsi + 9Ch]` in the pattern of [`Site::WantedSet`]).
pub const ENTITY_ID_AT: usize = 0x9c;
/// Where an entity keeps its position (`vmovaps xmm0, [rax + 70h]` in the pattern of [`Site::PlayerPed`]): the
/// fourth row of a 4x4 matrix at 40h, whose second row (50h) is the forward axis the title's vehicle cheats use.
pub const ENTITY_POSITION_AT: usize = 0x70;
pub const ENTITY_FORWARD_AT: usize = 0x50;
/// The size of a weather type and where its name's hash is (`imul rax, rax, 38h; mov eax, [rcx + rax]` in the
/// pattern of [`Site::WeatherTypes`]).
pub const WEATHER_TYPE_STRIDE: usize = 0x38;

/// What a place gives.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Take {
    /// The address this many bytes into the pattern.
    At(usize),
    /// The address this many bytes before the pattern: the start of the function the pattern lies in, which
    /// must be at a multiple of 16 as the title's functions are.
    Back(usize),
    /// What a 32-bit displacement at `at` points to, counted from `next` (offsets in the pattern).
    Rel { at: usize, next: usize },
    /// The 32-bit number at this offset in the pattern.
    Number(usize),
}

/// A place in the title's code and what is read there.
struct Place {
    what: &'static str,
    pattern: &'static str,
    takes: &'static [(Site, Take)],
}

/// The places. The comments read the patterns, which are no longer than they must be to be in the code once
/// and to hold what is taken.
const PLACES: &[Place] = &[
    Place {
        what: "the update step that reads the cheat buttons",
        // sub rsp, 28h; call +7 (the function right after); mov al, 1; add rsp, 28h; ret;
        // push rsi; push rdi; push rbx; sub rsp, 30h; mov esi, [...
        pattern: "48 83 EC 28 E8 07 00 00 00 B0 01 48 83 C4 28 C3 56 57 53 48 83 EC 30 8B 35",
        takes: &[(Site::HookCall, Take::At(4)), (Site::CheatsUpdate, Take::At(16))],
    },
    Place {
        what: "the deferred call of a typed cheat",
        // mov ecx, 4; lea r9, [copy]; mov byte [x], 1; mov [rdx], rax; mov [rdx + 8], r8; lea r8, [nothing];
        // mov [rdx + 10h], r9; mov dword [rax], 0; mov [rdx + 18h], r8; call defer
        pattern: "B9 04 00 00 00 4C 8D 0D ?? ?? ?? ?? C6 05 ?? ?? ?? ?? 01 48 89 02 4C 89 42 08 4C 8D 05 ?? ?? ?? ?? \
                  4C 89 4A 10 C7 00 00 00 00 00 4C 89 42 18 E8 ?? ?? ?? ??",
        takes: &[(Site::Defer, Take::Rel { at: 48, next: 52 })],
    },
    Place {
        what: "the cheat dispatcher",
        // mov eax, [rcx]; lea rcx, [rax - 10h]; cmp rcx, 3; jae +0Ch; lea rcx, [toggles]; xor byte [rax + rcx], 1;
        // ret; lea rcx, [table]; jmp [rcx + rax * 8]
        pattern: "8B 01 48 8D 48 F0 48 83 F9 03 73 0C 48 8D 0D ?? ?? ?? ?? 80 34 08 01 C3 48 8D 0D ?? ?? ?? ?? 48 FF 24 C1",
        takes: &[(Site::CheatDispatch, Take::At(0)), (Site::CheatTable, Take::Rel { at: 27, next: 31 })],
    },
    Place {
        what: "the function that gives the player's position",
        // sub rsp, 38h; mov rax, [player]; mov rdx, rcx; test rax, rax; je +1Bh; vmovaps xmm0, [rax + 70h]
        pattern: "48 83 EC 38 48 8B 05 ?? ?? ?? ?? 48 89 CA 48 85 C0 74 1B C5 F8 28 40 70",
        takes: &[(Site::PlayerPed, Take::Rel { at: 7, next: 11 })],
    },
    Place {
        what: "the function that sets a ped's wanted level",
        // 39 bytes into it: mov ebx, [rsi + 9Ch]; mov bpl, [rsp + n]; mov edi, edx; mov r15d, r8d; mov r13d, r9d;
        // mov ecx, ebx; call (wanted state); test rax, rax; je; mov r12d, [rax + level]
        pattern: "8B 9E 9C 00 00 00 40 8A AC 24 ?? ?? ?? ?? 89 D7 45 89 C7 45 89 CD 89 D9 E8 ?? ?? ?? ?? 48 85 C0 \
                  0F 84 ?? ?? ?? ?? 44 8B A0 ?? ?? 00 00",
        takes: &[
            (Site::WantedSet, Take::Back(39)),
            (Site::WantedGet, Take::Rel { at: 25, next: 29 }),
            (Site::WantedLevelAt, Take::Number(41)),
        ],
    },
    Place {
        what: "the function that sets the time of day",
        // push rsi, rdi, rbx; sub rsp, 50h; vmovups ymm0, [clock]; mov r9d, r8d; mov r8d, edx; mov edx, ecx;
        // lea rsi, [rsp + 30h]; lea rcx, [clock's time]; lea rdi, [...
        pattern: "56 57 53 48 83 EC 50 C5 FC 10 05 ?? ?? ?? ?? 45 89 C1 41 89 D0 89 CA 48 8D 74 24 30 48 8D 0D ?? ?? ?? ?? 48 8D 3D",
        takes: &[(Site::ClockSet, Take::At(0)), (Site::ClockTime, Take::Rel { at: 31, next: 35 })],
    },
    Place {
        what: "the function that gives the length of a game minute",
        // cmp byte [paused], 0; mov eax, [ms a minute]; jne; vmovss xmm0, [x]; vxorps xmm1, xmm1, xmm1;
        // xor ecx, ecx; vmulss
        pattern: "80 3D ?? ?? ?? ?? 00 8B 05 ?? ?? ?? ?? 75 ?? C5 FA 10 05 ?? ?? ?? ?? C5 F0 57 C9 31 C9 C5 FA 59",
        takes: &[(Site::ClockPaused, Take::Rel { at: 2, next: 7 })],
    },
    Place {
        what: "the function that forces a weather type",
        // push rsi, rdi; sub rsp, 28h; cmp [types], ecx; jbe; mov esi, edx; mov edi, ecx; call; mov edx, esi;
        // mov ecx, edi; mov dword [x], 0; and edx, 2; shr edx, 1; call; (12 bytes); mov [previous], edi;
        // mov [next], edi; mov [forced], edi
        pattern: "56 57 48 83 EC 28 39 0D ?? ?? ?? ?? 76 ?? 89 D6 89 CF E8 ?? ?? ?? ?? 89 F2 89 F9 C7 05 ?? ?? ?? ?? \
                  00 00 00 00 83 E2 02 D1 EA E8 ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? \
                  89 3D ?? ?? ?? ?? 89 3D ?? ?? ?? ?? 89 3D ?? ?? ?? ??",
        takes: &[
            (Site::WeatherForce, Take::At(0)),
            (Site::Weather, Take::Rel { at: 8, next: 12 }),
            (Site::WeatherPrev, Take::Rel { at: 61, next: 65 }),
            (Site::WeatherNext, Take::Rel { at: 67, next: 71 }),
            (Site::WeatherForced, Take::Rel { at: 73, next: 77 }),
        ],
    },
    Place {
        what: "the function that lets the weather run again",
        // sub rsp, 28h; vxorps xmm1, xmm1, xmm1; mov ecx, -1; mov r8b, 1; mov dword [forced], -1; call
        pattern: "48 83 EC 28 C5 F0 57 C9 B9 FF FF FF FF 41 B0 01 C7 05 ?? ?? ?? ?? FF FF FF FF E8",
        takes: &[(Site::WeatherClear, Take::At(0))],
    },
    Place {
        what: "a read of the current weather type's name",
        // mov rcx, [types]; lea rdx, [rsp + n]; mov r8w, 8000h; vucomiss xmm0, [half]; seta al; add rsi, imm;
        // mov eax, [rdi + rax * 4 + n]; imul rax, rax, 38h; mov eax, [rcx + rax]
        pattern: "48 8B 0D ?? ?? ?? ?? 48 8D 54 24 ?? 66 41 B8 00 80 C5 F8 2E 05 ?? ?? ?? ?? 0F 97 C0 48 81 C6 ?? ?? ?? ?? \
                  8B 84 87 ?? ?? 00 00 48 6B C0 38 8B 04 01",
        takes: &[(Site::WeatherTypes, Take::Rel { at: 3, next: 7 })],
    },
];

/// What was found: each [`Site`]'s offset in the image (or its number), `None` for what this executable does
/// not have.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Sites {
    found: [Option<usize>; SITES],
    /// The places that were not found, or were found more than once, as text for the log.
    pub missing: Vec<String>,
}

impl Sites {
    /// The site's offset in the image, or its number.
    pub fn get(&self, site: Site) -> Option<usize> {
        self.found[site as usize]
    }

    /// How many of the [`SITES`] were found.
    pub fn count(&self) -> usize {
        self.found.iter().flatten().count()
    }

    /// Whether every one of `sites` was found.
    pub fn has(&self, sites: &[Site]) -> bool {
        sites.iter().all(|s| self.get(*s).is_some())
    }
}

/// Where the pattern is in `code` (offset in the image, bytes), if it is there exactly once.
fn once(pattern: &Pattern, code: &[(usize, &[u8])]) -> Result<(usize, usize), &'static str> {
    let mut found = None;
    for (section, &(start, bytes)) in code.iter().enumerate() {
        for offset in pattern.find(bytes, 2) {
            if found.is_some() {
                return Err("is there more than once");
            }
            found = Some((section, start + offset));
        }
    }
    found.ok_or("is not there")
}

/// The sites of an image whose executable sections are `code` (offset in the image, bytes) and whose size is
/// `image_size`. A place that is missing, there twice, or that points outside the image leaves its sites `None`
/// and a line in [`Sites::missing`].
pub fn resolve(code: &[(usize, &[u8])], image_size: usize) -> Sites {
    let mut sites = Sites::default();
    for place in PLACES {
        let Some(pattern) = Pattern::parse(place.pattern) else {
            sites.missing.push(format!("{}: its pattern does not read", place.what));
            continue;
        };
        let (section, at) = match once(&pattern, code) {
            Ok(found) => found,
            Err(why) => {
                sites.missing.push(format!("{}: its code {why} in this executable", place.what));
                continue;
            }
        };
        let (start, bytes) = code[section];
        let here = &bytes[at - start..];
        let u32_at = |offset: usize| -> Option<u32> {
            Some(u32::from_le_bytes(here.get(offset..offset + 4)?.try_into().ok()?))
        };
        let taken: Option<Vec<(Site, usize)>> = place
            .takes
            .iter()
            .map(|&(site, take)| {
                let value = match take {
                    Take::At(offset) => at + offset,
                    Take::Back(offset) => at.checked_sub(offset).filter(|start| start.is_multiple_of(16))?,
                    Take::Rel { at: disp, next } => {
                        let target = (at + next) as i64 + i64::from(u32_at(disp)? as i32);
                        usize::try_from(target).ok().filter(|t| *t > 0 && *t < image_size)?
                    }
                    Take::Number(offset) => u32_at(offset)? as usize,
                };
                Some((site, value))
            })
            .collect();
        match taken {
            Some(taken) => taken.into_iter().for_each(|(site, value)| sites.found[site as usize] = Some(value)),
            None => sites.missing.push(format!("{}: it does not lead where it should", place.what)),
        }
    }
    sites
}

/// `jmp [rip + 0]` and the address it jumps to: 14 bytes of code that reach anywhere.
fn far_jump(to: usize) -> [u8; 14] {
    let mut code = [0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
    code[6..].copy_from_slice(&(to as u64).to_le_bytes());
    code
}

/// The 32-bit distance of a `call` at `call` (its first byte) to `to`, if it reaches.
fn call_distance(call: usize, to: usize) -> Option<[u8; 4]> {
    i32::try_from(to as i64 - (call as i64 + 5)).ok().map(i32::to_le_bytes)
}

/// Finds the sites in the image mapped at `base` and makes the title's call of its cheat input function a call
/// of `hook` (the address of a function without arguments in the title's calling convention, which calls
/// [`Site::CheatsUpdate`] itself). `Err` says why nothing was changed; the sites are returned either way they
/// were found.
///
/// # Safety
/// `base` is the mapped image `info` describes, no title code runs yet, and `hook` stays valid for the process.
#[cfg(windows)]
pub unsafe fn install(base: usize, info: &leonida_pe::ImageInfo, hook: usize) -> (Sites, Result<(), String>) {
    let sites = {
        // The image's code as it is mapped; the slices end with this block, before anything is written.
        let code: Vec<(usize, &[u8])> = info
            .sections
            .iter()
            .filter(|s| s.characteristics & crate::vipatch::SCN_MEM_EXECUTE != 0)
            .filter(|s| u64::from(s.rva) + u64::from(s.virtual_size) <= u64::from(info.size_of_image))
            .map(|s| {
                let (start, len) = (s.rva as usize, s.virtual_size as usize);
                // SAFETY: a section of the mapped image, readable, which nothing writes now.
                (start, unsafe { std::slice::from_raw_parts((base + start) as *const u8, len) })
            })
            .collect();
        resolve(&code, info.size_of_image as usize)
    };
    let (Some(call), Some(_), Some(_)) =
        (sites.get(Site::HookCall), sites.get(Site::CheatsUpdate), sites.get(Site::Defer))
    else {
        return (sites, Err("the title's cheat update or its deferred call was not found".into()));
    };
    let Some(stub) = crate::console_insn::near(base + call) else {
        return (sites, Err("no free memory near the title's code for the hook".into()));
    };
    let Some(distance) = call_distance(base + call, stub) else {
        return (sites, Err("the hook's code is out of reach of the call".into()));
    };
    // SAFETY: `stub` is fresh writable memory of ours, 64 KiB of it.
    unsafe { std::ptr::copy_nonoverlapping(far_jump(hook).as_ptr(), stub as *mut u8, 14) };
    let patch = crate::hooks::Patch {
        what: "game actions: the cheat update's call goes through the runtime",
        rva: call + 1,
        // The pattern's own bytes: the function right after.
        expect: &[0x07, 0, 0, 0],
        new: &distance,
    };
    // SAFETY: forwarded from this function's contract; the place is in the image's code.
    if unsafe { crate::hooks::apply(base, &patch) } {
        (sites, Ok(()))
    } else {
        (sites, Err("the call could not be rewritten".into()))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A pattern's bytes with its `??` filled from `fill`, in order (zero when `fill` runs out).
    fn bytes_of(pattern: &str, fill: &[u8]) -> Vec<u8> {
        let mut fill = fill.iter().copied();
        pattern
            .split_ascii_whitespace()
            .map(|t| u8::from_str_radix(t, 16).unwrap_or_else(|_| fill.next().unwrap_or(0)))
            .collect()
    }

    /// Where place `i` is in [`image`]: 0x400 apart, each as far into its function as its pattern says.
    fn place_at(i: usize) -> usize {
        let back = PLACES[i].takes.iter().find_map(|t| match t.1 {
            Take::Back(offset) => Some(offset),
            _ => None,
        });
        i * 0x400 + 0x40 + back.unwrap_or(0)
    }

    /// The place that takes `site`.
    fn place_of(site: Site) -> usize {
        PLACES.iter().position(|p| p.takes.iter().any(|t| t.0 == site)).unwrap()
    }

    /// Code with every place in it, the wildcards zero.
    fn image() -> Vec<u8> {
        let mut code = vec![0xCCu8; 0x1000 + PLACES.len() * 0x400];
        for (i, place) in PLACES.iter().enumerate() {
            let bytes = bytes_of(place.pattern, &[]);
            code[place_at(i)..][..bytes.len()].copy_from_slice(&bytes);
        }
        code
    }

    #[test]
    fn every_place_reads_and_takes_what_its_pattern_holds() {
        let mut seen = Vec::new();
        for place in PLACES {
            let tokens: Vec<&str> = place.pattern.split_ascii_whitespace().collect();
            assert!(Pattern::parse(place.pattern).is_some(), "{}", place.what);
            // Enough fixed bytes to name a place, and no more than a short signature.
            let fixed = tokens.iter().filter(|t| **t != "??").count();
            assert!((16..=40).contains(&fixed), "{}: {fixed}", place.what);
            for &(site, take) in place.takes {
                assert!(!seen.contains(&site), "{site:?} twice");
                seen.push(site);
                match take {
                    Take::At(offset) => assert!(offset < tokens.len(), "{site:?}"),
                    Take::Back(offset) => assert!(offset > 0 && offset < 0x100, "{site:?}"),
                    // A displacement is four `??`, and the instruction ends at `next`, at most 5 bytes on.
                    Take::Rel { at, next } => {
                        assert!(tokens[at..at + 4].iter().all(|t| *t == "??"), "{site:?}");
                        assert!(next >= at + 4 && next <= at + 9 && next <= tokens.len(), "{site:?}");
                    }
                    Take::Number(at) => assert!(at + 4 <= tokens.len(), "{site:?}"),
                }
            }
        }
        assert_eq!(seen.len(), SITES, "every site is taken somewhere");
    }

    #[test]
    fn the_patterns_say_what_the_constants_say() {
        let has = |site: Site, bytes: &str| {
            let place = PLACES.iter().find(|p| p.takes.iter().any(|t| t.0 == site)).unwrap();
            let all: Vec<&str> = place.pattern.split_ascii_whitespace().collect();
            let want: Vec<&str> = bytes.split_ascii_whitespace().collect();
            assert!(all.windows(want.len()).any(|w| w == want), "{site:?}: {bytes}");
        };
        // mov ecx, 4: the queue.
        assert_eq!(CHEAT_QUEUE, 4);
        has(Site::Defer, "B9 04 00 00 00");
        // The closure's four words: [rdx], [rdx + 8], [rdx + 10h], [rdx + 18h].
        has(Site::Defer, "48 89 02 4C 89 42 08");
        has(Site::Defer, "4C 89 4A 10 C7 00 00 00 00 00 4C 89 42 18 E8");
        // The toggles are cheats 16 to 18; the table is below them.
        assert_eq!(CHEATS, 16);
        has(Site::CheatDispatch, "48 8D 48 F0 48 83 F9 03");
        assert_eq!(ENTITY_ID_AT, 0x9c);
        has(Site::WantedSet, "8B 9E 9C 00 00 00");
        assert_eq!(ENTITY_POSITION_AT, 0x70);
        has(Site::PlayerPed, "C5 F8 28 40 70");
        assert_eq!(WEATHER_TYPE_STRIDE, 0x38);
        has(Site::WeatherTypes, "48 6B C0 38 8B 04 01");
        // The call the hook takes is to the function 7 bytes on, which is 16 bytes into the pattern.
        has(Site::HookCall, "E8 07 00 00 00");
    }

    #[test]
    fn sites_are_found_by_their_patterns_wherever_they_lie() {
        let code = image();
        let sites = resolve(&[(0x1000, &code[..])], 0x10_0000);
        // A zero displacement points at the end of its instruction, inside the image.
        assert!(sites.missing.is_empty(), "{:?}", sites.missing);
        let at = |site: Site| 0x1000 + place_at(place_of(site));
        assert_eq!(sites.get(Site::HookCall), Some(at(Site::HookCall) + 4));
        assert_eq!(sites.get(Site::CheatsUpdate), Some(at(Site::HookCall) + 16));
        assert_eq!(sites.get(Site::CheatDispatch), Some(at(Site::CheatDispatch)));
        assert_eq!(sites.get(Site::Defer), Some(at(Site::Defer) + 52));
        // The function the wanted level's pattern lies in starts 39 bytes before it.
        assert_eq!(sites.get(Site::WantedSet), Some(at(Site::WantedSet) - 39));
        assert_eq!(sites.get(Site::WantedSet).unwrap() % 16, 0);
        assert_eq!(sites.get(Site::WantedLevelAt), Some(0));
        assert!(sites.has(&[Site::ClockSet, Site::WeatherForce, Site::WeatherTypes]));
        assert_eq!(sites.count(), SITES);
    }

    #[test]
    fn displacements_and_numbers_are_read_from_the_code() {
        let mut code = image();
        // The player's global: mov rax, [rip + 0x1234], 4 bytes into its place.
        let player = place_at(place_of(Site::PlayerPed));
        code[player + 7..player + 11].copy_from_slice(&0x1234i32.to_le_bytes());
        // The deferred call, backwards: call -0x800.
        let defer = place_at(place_of(Site::Defer));
        code[defer + 48..defer + 52].copy_from_slice(&(-0x800i32).to_le_bytes());
        // The wanted level's place: mov r12d, [rax + 0x1f2c].
        let wanted = place_at(place_of(Site::WantedSet));
        code[wanted + 41..wanted + 43].copy_from_slice(&0x1f2cu16.to_le_bytes());
        let sites = resolve(&[(0x1000, &code[..])], 0x10_0000);
        assert!(sites.missing.is_empty(), "{:?}", sites.missing);
        assert_eq!(sites.get(Site::PlayerPed), Some(0x1000 + player + 11 + 0x1234));
        assert_eq!(sites.get(Site::Defer), Some(0x1000 + defer + 52 - 0x800));
        assert_eq!(sites.get(Site::WantedLevelAt), Some(0x1f2c));
    }

    #[test]
    fn a_place_missing_twice_or_pointing_outside_is_left_out() {
        let mut code = image();
        // The clock's place gone: its sites are none, the others stay.
        let clock = place_at(place_of(Site::ClockSet));
        code[clock] ^= 0xFF;
        // The weather's place twice.
        let force = place_at(place_of(Site::WeatherForce));
        let copy = code[force..force + 0x60].to_vec();
        code[force + 0x100..force + 0x160].copy_from_slice(&copy);
        // The player's global far outside the image.
        let player = place_at(place_of(Site::PlayerPed));
        code[player + 7..player + 11].copy_from_slice(&0x7000_0000i32.to_le_bytes());
        let sites = resolve(&[(0x1000, &code[..])], 0x10_0000);
        assert_eq!(sites.missing.len(), 3, "{:?}", sites.missing);
        assert!(sites.get(Site::ClockSet).is_none() && sites.get(Site::ClockTime).is_none());
        assert!(sites.get(Site::WeatherForce).is_none() && sites.get(Site::Weather).is_none());
        assert!(sites.get(Site::PlayerPed).is_none());
        assert!(sites.has(&[Site::HookCall, Site::Defer, Site::CheatTable, Site::WantedSet]));
        // A negative displacement that leaves the image at its start.
        code[player + 7..player + 11].copy_from_slice(&(-0x10_0000i32).to_le_bytes());
        assert!(resolve(&[(0x1000, &code[..])], 0x10_0000).get(Site::PlayerPed).is_none());
        // The wanted level's pattern where no function of the title's could start 39 bytes before it.
        let mut code = image();
        let wanted = place_at(place_of(Site::WantedSet));
        let bytes = code[wanted..wanted + 0x30].to_vec();
        code[wanted] ^= 0xFF;
        code[wanted + 0x101..wanted + 0x131].copy_from_slice(&bytes);
        let sites = resolve(&[(0x1000, &code[..])], 0x10_0000);
        assert_eq!(sites.missing.len(), 1, "{:?}", sites.missing);
        assert!(sites.get(Site::WantedSet).is_none() && sites.get(Site::WantedGet).is_none());
        // Two sections: found in either, and once in each is twice.
        let code = image();
        let (a, b) = code.split_at(0x800);
        assert!(resolve(&[(0x1000, a), (0x1800, b)], 0x10_0000).missing.is_empty());
        assert!(!resolve(&[(0x1000, &code[..]), (0x9000, &code[..])], 0x10_0000).missing.is_empty());
    }

    #[test]
    fn the_hook_jump_and_the_call_reach() {
        assert_eq!(
            far_jump(0x1122_3344_5566_7788),
            [0xFF, 0x25, 0, 0, 0, 0, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11]
        );
        assert_eq!(call_distance(0x1000, 0x1005), Some([0, 0, 0, 0]));
        assert_eq!(call_distance(0x1000, 0x1000), Some((-5i32).to_le_bytes()));
        assert_eq!(call_distance(0x1000, 0x1_0000_1005), None);
    }

    /// The hook on made-up code that runs: the update step's `call` reaches a function of ours and the step
    /// returns as before.
    #[cfg(windows)]
    #[test]
    fn the_update_step_calls_the_hook() {
        use std::sync::atomic::{AtomicU32, Ordering};
        use windows_sys::Win32::System::Memory::{
            MEM_COMMIT, MEM_RESERVE, PAGE_EXECUTE_READWRITE, VirtualAlloc,
        };
        static CALLS: AtomicU32 = AtomicU32::new(0);
        extern "win64" fn hook() {
            CALLS.fetch_add(1, Ordering::Relaxed);
        }
        let code = image();
        // SAFETY: fresh memory of ours that may be run.
        let base = unsafe {
            VirtualAlloc(std::ptr::null(), 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)
        } as usize;
        assert_ne!(base, 0);
        // SAFETY: the allocation holds the code at 0x1000.
        unsafe { std::ptr::copy_nonoverlapping(code.as_ptr(), (base + 0x1000) as *mut u8, code.len()) };
        let info = leonida_pe::ImageInfo {
            machine: leonida_pe::MACHINE_AMD64,
            characteristics: 0,
            timestamp: 0,
            subsystem: 0,
            dll_characteristics: 0,
            image_base: 0,
            size_of_image: 0x10000,
            entry_rva: 0,
            stack_reserve: 0,
            stack_commit: 0,
            sections: vec![leonida_pe::SectionInfo {
                name: ".text".to_owned(),
                rva: 0x1000,
                virtual_size: code.len() as u32,
                file_offset: 0,
                file_size: 0,
                characteristics: crate::vipatch::SCN_MEM_EXECUTE,
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
        // SAFETY: the made-up image is mapped at `base`, nothing runs in it, and `hook` is a static function.
        let (sites, done) = unsafe { install(base, &info, hook as extern "win64" fn() as usize) };
        assert_eq!(done, Ok(()));
        let step = sites.get(Site::HookCall).unwrap() - 4;
        // SAFETY: the step is `sub rsp, 28h; call hook; mov al, 1; add rsp, 28h; ret`.
        let step: extern "win64" fn() -> u8 = unsafe { std::mem::transmute(base + step) };
        assert_eq!(step(), 1);
        assert_eq!(step(), 1);
        assert_eq!(CALLS.load(Ordering::Relaxed), 2);
        // A second install finds the call changed and leaves it.
        // SAFETY: as above.
        let (_, again) = unsafe { install(base, &info, hook as extern "win64" fn() as usize) };
        assert!(again.is_err());
    }

    /// With `LEONIDA_GAME_DIR` set to the folder of the Xbox package: every place is in the code of
    /// `game_scarlett_master.exe` exactly once and points inside the image.
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
        let info = leonida_pe::ImageInfo::parse(&data).expect("a PE image");
        let code: Vec<(usize, &[u8])> = info
            .sections
            .iter()
            .filter(|s| s.characteristics & crate::vipatch::SCN_MEM_EXECUTE != 0)
            .map(|s| {
                let len = s.file_size.min(s.virtual_size) as usize;
                (s.rva as usize, &data[s.file_offset as usize..s.file_offset as usize + len])
            })
            .collect();
        let sites = resolve(&code, info.size_of_image as usize);
        assert!(sites.missing.is_empty(), "{:?}", sites.missing);
        // Where each thing lies: functions in code, globals behind it.
        let in_code = |rva: usize| code.iter().any(|&(start, bytes)| (start..start + bytes.len()).contains(&rva));
        for site in [
            Site::CheatsUpdate,
            Site::Defer,
            Site::CheatDispatch,
            Site::WantedSet,
            Site::WantedGet,
            Site::ClockSet,
            Site::WeatherForce,
            Site::WeatherClear,
        ] {
            assert!(in_code(sites.get(site).unwrap()), "{site:?}");
        }
        for site in [
            Site::CheatTable,
            Site::PlayerPed,
            Site::ClockTime,
            Site::ClockPaused,
            Site::Weather,
            Site::WeatherPrev,
            Site::WeatherNext,
            Site::WeatherForced,
            Site::WeatherTypes,
        ] {
            assert!(!in_code(sites.get(site).unwrap()), "{site:?}");
        }
        // The weather's indices lie in the weather state, after its count.
        let weather = sites.get(Site::Weather).unwrap();
        for site in [Site::WeatherPrev, Site::WeatherNext, Site::WeatherForced, Site::WeatherTypes] {
            let at = sites.get(site).unwrap();
            assert!(at > weather && at < weather + 0x1000, "{site:?}");
        }
        assert!(sites.get(Site::WantedLevelAt).unwrap() < 0x10000);
    }
}
