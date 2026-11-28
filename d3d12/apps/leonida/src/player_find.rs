//! Diagnostics only (`LEONIDA_FIND_PLAYER=<start s>,<seconds>`): finds, at run
//! time, where the title keeps the local player's position, and how to reach it
//! from the title's image, so that a teleport can write there (`player.rs`).
//!
//! 1. At `<start>` seconds it lists every candidate entity transform in the
//!    title's writable memory: three unit axes at right angles (within 1e-3)
//!    and a finite position row (|x|, |y| < 20000, -500 < z < 3000), as a 4x4
//!    row-major matrix (`R4`), a 3x4 one whose position is the fourth column
//!    (`T34`) or a packed 4x3 (`R43`). Then, for `<seconds>`, it reads every
//!    candidate's position every `LEONIDA_PLAYER_INTERVAL_MS` (250) together
//!    with the keys held at that moment (the file `LEONIDA_KEYS_FILE`
//!    names). A candidate dies when it moves while no key has been held for a
//!    second (more than `LEONIDA_PLAYER_IDLE_TOL` = 0.02 m, `LEONIDA_PLAYER_IDLE_MAX`
//!    = 2 times) or stops looking like a transform; the survivors that moved
//!    are ranked by how their displacement follows the keys (speed while a
//!    movement key is held, direction along one of their own axes, the opposite
//!    sign for S).
//! 2. The best transform and the ones that move together with it (the physics
//!    copy) are searched for in memory as pointers: qwords equal to the address
//!    of the struct they sit in (a start 0 to 0x200 bytes before the matrix, 16
//!    byte steps), then qwords that point to a block holding such a pointer less
//!    than 0x1000 bytes in, up to `LEONIDA_PLAYER_LEVELS` (3) levels. The paths
//!    that start in a writable section of the title's image are written to
//!    `paths.txt`, which `player.rs` follows in later runs.
//!
//! `LEONIDA_FIND_PLAYER` alone is `90,240`; one number is the start. Besides the
//! transforms it also tracks position vectors: 16-byte aligned float4s whose xyz lie in
//! the map and whose w is not a float in range (`V4`, `LEONIDA_PLAYER_VEC_W=flag|plain|any|off`)
//! and triples of doubles in the map (`D3`, `LEONIDA_PLAYER_F64=0` to skip). The scan runs
//! on `LEONIDA_PLAYER_THREADS` threads (default 8 at most). `LEONIDA_PLAYER_FILTER=loose` turns the
//! strict filters off (strict: a 4x4's w column exactly 0 and corner 1, a position that is not a
//! whole number of metres on every axis, axes that are not exactly the world's); `summary.txt`
//! counts what survives each. A candidate that never moves once movement keys have been held
//! for `LEONIDA_PLAYER_PRUNE_STABLE` (6) samples, or that jumps more than 25 m/s, is dropped.
//!
//! After sampling, a **write probe** finds what the derived position follows: the
//! best candidate is usually a copy the game rewrites (a camera target, a history
//! entry), so every f32 triple (4-byte aligned) and f64 triple (8-byte aligned) within
//! 0.5 m of it is tried in turn (transform positions first): +3 m in z is written to that one
//! triple every 5 ms for 400 ms, and if the derived position rises by 2.5 to 3.5 m the triple
//! drives it; the others that rose with it join it as the teleport group (`probe.txt`,
//! `probe-candidates.txt`). The player must stand still (hold no keys) when it starts, which is
//! waited for; `LEONIDA_PLAYER_PROBE=<s>` starts it that many seconds after the title started,
//! `LEONIDA_PLAYER_PROBE_MAX` (600) bounds the tries. The pointer search then looks only at that struct
//! (`LEONIDA_PLAYER_LEVELS`, 4 by default, pointers up to 0x2000 bytes into a block). A
//! `LEONIDA_TELEPORT` in the same run waits for the probe.
//!
//! Tuning, all optional: `LEONIDA_PLAYER_ALIGN` (4; 16 is faster),
//! `LEONIDA_PLAYER_MAX_CANDIDATES` (6000000), `LEONIDA_PLAYER_MAX_TRACKS`
//! (2000000), `LEONIDA_PLAYER_TOP` (100), `LEONIDA_PLAYER_GROUPS` (6),
//! `LEONIDA_PLAYER_POINTERS=0` (no pointer search).
//!
//! Output goes under `%TEMP%\leonida-player\` (addresses of the title: never
//! commit them): `summary.txt`, `samples.txt` (time, wall clock, keys),
//! `candidates.txt` (the ranking), `tracks.csv`, `comovers.txt`, `best.txt`
//! (where the best one lies), `pointers.txt` and `paths.txt`.
//!
//! Only reads the title's memory (`ReadProcessMemory` on the own process: a page
//! that goes away fails the read). Addresses the scan keeps itself have a high
//! bit set so that its own tables are not found as pointers.

use std::fmt::Write as _;
use std::path::Path;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::time::{Duration, Instant};

use crate::player::{self, Group, Known, Layout, Mat, mem};

#[repr(C)]
struct MemoryBasicInformation {
    base_address: usize,
    allocation_base: usize,
    allocation_protect: u32,
    partition_id: u16,
    region_size: usize,
    state: u32,
    protect: u32,
    kind: u32,
}

unsafe extern "system" {
    fn VirtualQuery(address: usize, buffer: *mut MemoryBasicInformation, len: usize) -> usize;
    fn VirtualAlloc(address: usize, size: usize, kind: u32, protect: u32) -> *mut u8;
}

const MEM_COMMIT: u32 = 0x1000;
const MEM_RESERVE: u32 = 0x2000;
const MEM_IMAGE: u32 = 0x100_0000;
const PAGE_READWRITE: u32 = 0x04;
const PAGE_WRITECOPY: u32 = 0x08;
const PAGE_EXECUTE_READWRITE: u32 = 0x40;
const PAGE_EXECUTE_WRITECOPY: u32 = 0x80;
const PAGE_GUARD: u32 = 0x100;
const CHUNK: usize = 4 << 20;
/// A matrix (at most 64 bytes) that starts inside a chunk is read whole.
const OVERLAP: usize = 64;
/// Added to every address the scan stores itself.
const ENC: usize = 1 << 62;

const W: u8 = 1;
const A: u8 = 2;
const S: u8 = 4;
const D: u8 = 8;
const SHIFT: u8 = 16;
const SPACE: u8 = 32;
const OTHER: u8 = 64;
const WASD: u8 = W | A | S | D;

fn enc(address: usize) -> usize {
    address | ENC
}

fn dec(address: usize) -> usize {
    address & !ENC
}

struct Config {
    base: usize,
    size: usize,
    /// (name, rva, size) of every section; the writable ones are the roots.
    sections: Vec<(String, usize, usize)>,
    writable_image: Vec<(usize, usize)>,
    seconds: f64,
    align: usize,
    interval: Duration,
    max_candidates: usize,
    max_tracks: usize,
    idle_tol: f32,
    idle_max: u8,
    top: usize,
    groups: usize,
    levels: usize,
    pointers: bool,
    /// Drop the forms and values static props have (see `admit`).
    strict: bool,
    vec_w: VecW,
    f64s: bool,
    threads: usize,
    /// Stable held-key samples after which a candidate that never moved is dropped.
    prune_after: u32,
    /// Seconds after the title started at which the write probe begins (default: after sampling).
    probe_at: Option<f64>,
    /// The most seconds the write probe may take.
    probe_seconds: u64,
    started: Instant,
}

/// Which fourth floats a position vector (`V4`) may have.
#[derive(Clone, Copy, PartialEq, Eq)]
enum VecW {
    Off,
    /// Not an ordinary float: a flag word (a denormal, NaN, infinity or above 2^31).
    Flag,
    /// 0.0 or 1.0.
    Plain,
    Any,
}

/// Starts the finder thread when `LEONIDA_FIND_PLAYER` is set.
pub fn start(base: usize, info: &leonida_pe::ImageInfo) {
    let Ok(spec) = std::env::var("LEONIDA_FIND_PLAYER") else {
        return;
    };
    // Defaults for one run: the scan starts at 90 s and sampling lasts 240 s.
    let numbers: Vec<f64> = spec.split(',').filter_map(|s| s.trim().parse().ok()).collect();
    let (delay, seconds) = match numbers[..] {
        [] => (90.0, 240.0),
        [delay] => (delay, 240.0),
        [delay, seconds, ..] => (delay, seconds),
    };
    let num = |name: &str, default: usize| {
        std::env::var(name)
            .ok()
            .and_then(|v| v.trim().parse().ok())
            .unwrap_or(default)
    };
    let writable = |s: &&leonida_pe::SectionInfo| s.characteristics & 0x8000_0000 != 0;
    let config = Config {
        base,
        size: info.size_of_image as usize,
        sections: info
            .sections
            .iter()
            .map(|s| (s.name.clone(), s.rva as usize, s.virtual_size as usize))
            .collect(),
        writable_image: info
            .sections
            .iter()
            .filter(writable)
            .map(|s| (s.rva as usize, s.rva as usize + s.virtual_size as usize))
            .collect(),
        seconds,
        align: if num("LEONIDA_PLAYER_ALIGN", 4) >= 16 { 16 } else { 4 },
        interval: Duration::from_millis(num("LEONIDA_PLAYER_INTERVAL_MS", 250).max(20) as u64),
        max_candidates: num("LEONIDA_PLAYER_MAX_CANDIDATES", 6_000_000),
        max_tracks: num("LEONIDA_PLAYER_MAX_TRACKS", 2_000_000),
        idle_tol: std::env::var("LEONIDA_PLAYER_IDLE_TOL")
            .ok()
            .and_then(|v| v.trim().parse().ok())
            .unwrap_or(0.02),
        idle_max: num("LEONIDA_PLAYER_IDLE_MAX", 2) as u8,
        top: num("LEONIDA_PLAYER_TOP", 100),
        groups: num("LEONIDA_PLAYER_GROUPS", 6).clamp(1, 16),
        levels: num("LEONIDA_PLAYER_LEVELS", 4).clamp(1, 5),
        pointers: std::env::var("LEONIDA_PLAYER_POINTERS").map_or(true, |v| v != "0"),
        strict: std::env::var("LEONIDA_PLAYER_FILTER").map_or(true, |v| v != "loose"),
        vec_w: match std::env::var("LEONIDA_PLAYER_VEC_W").as_deref() {
            Ok("off") => VecW::Off,
            Ok("plain") => VecW::Plain,
            Ok("any") => VecW::Any,
            _ => VecW::Flag,
        },
        f64s: std::env::var("LEONIDA_PLAYER_F64").map_or(true, |v| v != "0"),
        threads: num(
            "LEONIDA_PLAYER_THREADS",
            std::thread::available_parallelism().map_or(4, |n| n.get()).min(8),
        )
        .max(1),
        prune_after: num("LEONIDA_PLAYER_PRUNE_STABLE", 6) as u32,
        probe_at: std::env::var("LEONIDA_PLAYER_PROBE").ok().and_then(|v| v.trim().parse().ok()),
        probe_seconds: num("LEONIDA_PLAYER_PROBE_SECONDS", 900) as u64,
        started: Instant::now(),
    };
    player::find_started();
    eprintln!(
        "Leonida: find player at {delay} s for {seconds} s (keys file {:?}); output {}",
        std::env::var("LEONIDA_KEYS_FILE").ok(),
        player::out_dir().display()
    );
    std::thread::Builder::new()
        .name("leonida-find-player".into())
        .spawn(move || {
            // However this ends, a teleport waiting for it goes on.
            struct Done;
            impl Drop for Done {
                fn drop(&mut self) {
                    player::find_finished();
                }
            }
            let _done = Done;
            std::thread::sleep(Duration::from_secs_f64(delay.max(0.0)));
            match run(&config) {
                Ok(headline) => eprintln!("Leonida: find player done: {headline}"),
                Err(e) => eprintln!("Leonida: find player failed: {e}"),
            }
            player::find_finished();
        })
        .ok();
}

// ---------------------------------------------------------------- memory

struct Region {
    start: usize,
    size: usize,
}

/// Committed writable memory (the title's image and private or mapped memory,
/// not other libraries' images), minus the scan's own buffer.
fn regions(config: &Config, skips: &[(usize, usize)]) -> Vec<Region> {
    let mut out = Vec::new();
    let mut at = 0x1_0000usize;
    loop {
        let mut mbi = MemoryBasicInformation {
            base_address: 0,
            allocation_base: 0,
            allocation_protect: 0,
            partition_id: 0,
            region_size: 0,
            state: 0,
            protect: 0,
            kind: 0,
        };
        // SAFETY: a plain query into a struct of the documented layout.
        let got = unsafe { VirtualQuery(at, &mut mbi, std::mem::size_of_val(&mbi)) };
        if got == 0 {
            break;
        }
        let (start, size) = (mbi.base_address, mbi.region_size);
        at = start.saturating_add(size.max(0x1000));
        let writable = mbi.protect
            & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)
            != 0;
        if mbi.state != MEM_COMMIT || mbi.protect & PAGE_GUARD != 0 || !writable {
            continue;
        }
        let in_title = start >= config.base && start < config.base + config.size;
        if mbi.kind == MEM_IMAGE && !in_title {
            continue;
        }
        let mut parts = vec![(start, start + size)];
        for &(lo, hi) in skips {
            parts = parts
                .into_iter()
                .flat_map(|(a, b)| {
                    if lo < b && hi > a {
                        [(a, lo.max(a)), (hi.min(b), b)]
                    } else {
                        [(a, b), (0, 0)]
                    }
                })
                .filter(|&(a, b)| b > a)
                .collect();
        }
        out.extend(parts.into_iter().map(|(a, b)| Region { start: a, size: b - a }));
    }
    out
}

/// Reads `want` bytes at `address` into `out`; pages that cannot be read are
/// left zero (and say so by the returned count).
fn read_robust(address: usize, out: &mut [u8]) -> usize {
    if mem::read(address, out) {
        return 0;
    }
    let mut lost = 0;
    for (i, page) in out.chunks_mut(0x1000).enumerate() {
        if !mem::read(address + i * 0x1000, page) {
            page.fill(0);
            lost += page.len();
        }
    }
    lost
}

fn alloc_buffer() -> Result<&'static mut [u8], String> {
    // SAFETY: a fresh private allocation that is never freed.
    let buffer = unsafe { VirtualAlloc(0, CHUNK, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) };
    if buffer.is_null() {
        return Err("no buffer".into());
    }
    // SAFETY: CHUNK bytes of read-write memory owned by nobody else.
    Ok(unsafe { std::slice::from_raw_parts_mut(buffer, CHUNK) })
}

// ------------------------------------------------------------ candidates

/// One candidate transform and what is known of it so far.
struct Slot {
    /// `enc`oded address.
    addr: usize,
    layout: Layout,
    w: f32,
    alive: bool,
    /// Moves while no key was held.
    violations: u8,
    prev: [f32; 3],
    track: Option<Box<Track>>,
}

/// What happened to a candidate that moved.
#[derive(Default)]
struct Track {
    /// Sample index of `pos[0]`; `pos` stays empty until the candidate has also
    /// stayed put while no key was held (`quiet`), so that the many things that only
    /// look like the player do not each hold a path.
    first: usize,
    pos: Vec<[f32; 3]>,
    /// Idle samples (no key for a second) at which it stayed put.
    quiet: u8,
    /// Samples with the same movement keys held as the one before.
    steady: u32,
    /// Of those, the ones that moved at a walking to sprinting speed.
    follow: u32,
    held_dist: f32,
    speeds_w: Vec<f32>,
    speeds_shift: Vec<f32>,
    /// Sum of cosines between the movement and each axis, for W, S, D, A alone.
    cos_sum: [[f32; 3]; 4],
    cos_n: [u32; 4],
    /// Sum of the horizontal movement directions, for W, S, D, A alone.
    dir_sum: [[f32; 2]; 4],
}

fn pos_ok(x: f32, y: f32, z: f32) -> bool {
    x.abs() < 20000.0 && y.abs() < 20000.0 && z > -500.0 && z < 3000.0 && x.abs() + y.abs() + z.abs() > 1.0e-3
}

/// A position vector inside the world (the map's rectangle and heights with a margin).
fn world_ok(x: f32, y: f32, z: f32) -> bool {
    x > -14000.0 && x < 6000.0 && y > -10500.0 && y < 12000.0 && z > -150.0 && z < 1000.0 && x.abs() + y.abs() > 1.0
}

#[inline]
fn frac(v: f32) -> bool {
    (v - v.round()).abs() > 1.0e-3
}

/// At least two of the three are not a whole number of metres.
fn fractional(p: [f32; 3]) -> bool {
    p.iter().filter(|&&v| frac(v)).count() >= 2
}

#[inline]
fn unit(a: f32, b: f32, c: f32) -> bool {
    let n = a * a + b * b + c * c;
    (n - 1.0).abs() < 2.0e-3
}

#[inline]
fn dot(f: &[f32], i: usize, j: usize) -> f32 {
    f[i] * f[j] + f[i + 1] * f[j + 1] + f[i + 2] * f[j + 2]
}

/// What the scan counted: each stage of the filters, per layout (R4, T34, R43).
#[derive(Default, Clone)]
struct Counts {
    raw: [usize; 3],
    form: [usize; 3],
    not_integer: [usize; 3],
    kept: [usize; 3],
    v4_raw: usize,
    v4_kept: usize,
    d3_raw: usize,
    d3_kept: usize,
    unreadable: usize,
}

impl Counts {
    fn add(&mut self, o: &Counts) {
        for k in 0..3 {
            self.raw[k] += o.raw[k];
            self.form[k] += o.form[k];
            self.not_integer[k] += o.not_integer[k];
            self.kept[k] += o.kept[k];
        }
        self.v4_raw += o.v4_raw;
        self.v4_kept += o.v4_kept;
        self.d3_raw += o.d3_raw;
        self.d3_kept += o.d3_kept;
        self.unreadable += o.unreadable;
    }
}

/// The filters a transform passes to be kept: a strict form (the unused
/// fourth floats exactly 0 and the corner exactly 1), a position that is not a
/// whole number of metres on every axis, and axes that are not exactly the
/// world's (identity-like props).
fn admit(strict: bool, c: &mut Counts, k: usize, form_ok: bool, axes: [f32; 9], pos: [f32; 3]) -> bool {
    c.raw[k] += 1;
    if strict && !form_ok {
        return false;
    }
    c.form[k] += 1;
    if strict && pos.iter().all(|&v| !frac(v)) {
        return false;
    }
    c.not_integer[k] += 1;
    if strict && axes.iter().all(|&v| v == 0.0 || v == 1.0 || v == -1.0) {
        return false;
    }
    c.kept[k] += 1;
    true
}

fn w_ok(mode: VecW, w: f32) -> bool {
    let bits = w.to_bits();
    let exp = (bits >> 23) & 0xFF;
    match mode {
        VecW::Off => false,
        VecW::Any => true,
        VecW::Plain => w == 0.0 || w == 1.0,
        VecW::Flag => (exp == 0 && bits & 0x7F_FFFF != 0) || exp == 0xFF || exp >= 0x9E,
    }
}

/// Candidates in one block of memory of which `limit` bytes are this block's.
fn scan_block(config: &Config, address: usize, bytes: &[u8], limit: usize, out: &mut Vec<Slot>, c: &mut Counts, caps: &[AtomicUsize; 3]) {
    let limits = [config.max_candidates, config.max_candidates, config.max_candidates / 4];
    // SAFETY: the buffer is page aligned; `bytes` is a prefix of it.
    let f = unsafe { std::slice::from_raw_parts(bytes.as_ptr() as *const f32, bytes.len() / 4) };
    let count = f.len();
    let step = config.align / 4;
    let m0 = out.len();
    if caps[0].load(Ordering::Relaxed) < limits[0] {
        let mut j = 0;
        while j < limit / 4 {
            let here = address + j * 4;
            // Rows of four floats (R4, T34).
            if j + 12 <= count && unit(f[j], f[j + 1], f[j + 2]) && unit(f[j + 4], f[j + 5], f[j + 6]) && unit(f[j + 8], f[j + 9], f[j + 10]) {
                let ortho = dot(f, j, j + 4).abs() < 1.0e-3 && dot(f, j, j + 8).abs() < 1.0e-3 && dot(f, j + 4, j + 8).abs() < 1.0e-3;
                if ortho {
                    let axes = [f[j], f[j + 1], f[j + 2], f[j + 4], f[j + 5], f[j + 6], f[j + 8], f[j + 9], f[j + 10]];
                    if j + 16 <= count && pos_ok(f[j + 12], f[j + 13], f[j + 14]) {
                        let form = f[j + 3] == 0.0 && f[j + 7] == 0.0 && f[j + 11] == 0.0 && f[j + 15] == 1.0;
                        if admit(config.strict, c, 0, form, axes, [f[j + 12], f[j + 13], f[j + 14]]) {
                            out.push(slot(here, Layout::R4, f[j + 15]));
                        }
                    }
                    if pos_ok(f[j + 3], f[j + 7], f[j + 11]) {
                        let form = j + 16 <= count && f[j + 12] == 0.0 && f[j + 13] == 0.0 && f[j + 14] == 0.0 && f[j + 15] == 1.0;
                        if admit(config.strict, c, 1, form, axes, [f[j + 3], f[j + 7], f[j + 11]]) {
                            out.push(slot(here, Layout::T34, 0.0));
                        }
                    }
                }
            }
            // Packed rows of three floats (R43).
            if j + 12 <= count && unit(f[j], f[j + 1], f[j + 2]) && unit(f[j + 3], f[j + 4], f[j + 5]) && unit(f[j + 6], f[j + 7], f[j + 8]) {
                let ortho = dot(f, j, j + 3).abs() < 1.0e-3 && dot(f, j, j + 6).abs() < 1.0e-3 && dot(f, j + 3, j + 6).abs() < 1.0e-3;
                let axes = [f[j], f[j + 1], f[j + 2], f[j + 3], f[j + 4], f[j + 5], f[j + 6], f[j + 7], f[j + 8]];
                if ortho && pos_ok(f[j + 9], f[j + 10], f[j + 11]) && admit(config.strict, c, 2, true, axes, [f[j + 9], f[j + 10], f[j + 11]]) {
                    out.push(slot(here, Layout::R43, 0.0));
                }
            }
            j += step;
        }
    }
    caps[0].fetch_add(out.len() - m0, Ordering::Relaxed);
    // Position vectors: 16-byte aligned (x, y, z, w) with w not a position.
    if config.vec_w != VecW::Off && caps[1].load(Ordering::Relaxed) < limits[1] {
        let before = out.len();
        let mut j = 0;
        while j + 4 <= count && j < limit / 4 {
            let (x, y, z) = (f[j], f[j + 1], f[j + 2]);
            if world_ok(x, y, z) && w_ok(config.vec_w, f[j + 3]) {
                c.v4_raw += 1;
                if !config.strict || fractional([x, y, z]) {
                    c.v4_kept += 1;
                    out.push(slot(address + j * 4, Layout::V4, f[j + 3]));
                }
            }
            j += 4;
        }
        caps[1].fetch_add(out.len() - before, Ordering::Relaxed);
    }
    // Doubles (x, y, z), 8-byte aligned.
    if config.f64s && caps[2].load(Ordering::Relaxed) < limits[2] {
        let before = out.len();
        // SAFETY: as above; 8-byte aligned.
        let d = unsafe { std::slice::from_raw_parts(bytes.as_ptr() as *const f64, bytes.len() / 8) };
        let mut i = 0;
        while i + 3 <= d.len() && i < limit / 8 {
            // Cheap test first: most doubles are far outside the world.
            if d[i] > -14000.0 && d[i] < 6000.0 && d[i + 1] > -10500.0 && d[i + 1] < 12000.0 {
                let (x, y, z) = (d[i] as f32, d[i + 1] as f32, d[i + 2] as f32);
                if world_ok(x, y, z) {
                    c.d3_raw += 1;
                    if !config.strict || fractional([x, y, z]) {
                        c.d3_kept += 1;
                        out.push(slot(address + i * 8, Layout::D3, 0.0));
                    }
                }
            }
            i += 1;
        }
        caps[2].fetch_add(out.len() - before, Ordering::Relaxed);
    }
}

/// Scans every region with `buffers.len()` threads; the candidates, sorted by address.
fn scan_candidates(config: &Config, regions: &[Region], buffers: &mut [&'static mut [u8]]) -> (Vec<Slot>, Counts) {
    const SPAN: usize = 2 << 20;
    let mut tasks: Vec<(usize, usize)> = Vec::new();
    for r in regions {
        let mut at = r.start;
        while at < r.start + r.size {
            tasks.push((at, r.start + r.size));
            at += SPAN;
        }
    }
    let next = AtomicUsize::new(0);
    let caps = [AtomicUsize::new(0), AtomicUsize::new(0), AtomicUsize::new(0)];
    let results: Vec<(Vec<Slot>, Counts)> = std::thread::scope(|scope| {
        let handles: Vec<_> = buffers
            .iter_mut()
            .map(|buffer| {
                let (tasks, next, caps) = (&tasks, &next, &caps);
                scope.spawn(move || {
                    let mut out = Vec::new();
                    let mut c = Counts::default();
                    loop {
                        let i = next.fetch_add(1, Ordering::Relaxed);
                        let Some(&(at, end)) = tasks.get(i) else { break };
                        let limit = SPAN.min(end - at);
                        let want = (limit + OVERLAP).min(end - at);
                        c.unreadable += read_robust(at, &mut buffer[..want]);
                        scan_block(config, at, &buffer[..want], limit, &mut out, &mut c, caps);
                    }
                    (out, c)
                })
            })
            .collect();
        handles.into_iter().filter_map(|h| h.join().ok()).collect()
    });
    let mut counts = Counts::default();
    let mut all: Vec<Slot> = Vec::new();
    for (out, c) in results {
        counts.add(&c);
        all.extend(out);
    }
    all.sort_by_key(|s| s.addr);
    (all, counts)
}

fn slot(address: usize, layout: Layout, w: f32) -> Slot {
    Slot {
        addr: enc(address),
        layout,
        w,
        alive: true,
        violations: 0,
        prev: [0.0; 3],
        track: None,
    }
}

// -------------------------------------------------------------- sampling

/// The keys the test script holds now (`LEONIDA_KEYS_FILE`, as `gameinput`
/// reads it), as bits.
fn keys_now() -> u8 {
    let Some(path) = std::env::var_os("LEONIDA_KEYS_FILE") else {
        return 0;
    };
    let text = std::fs::read_to_string(path).unwrap_or_default();
    let mut keys = 0;
    for key in text.split(|c: char| c.is_whitespace() || c == ',' || c == '+').filter(|k| !k.is_empty()) {
        keys |= match key.to_ascii_uppercase().as_str() {
            "W" => W,
            "A" => A,
            "S" => S,
            "D" => D,
            "SHIFT" => SHIFT,
            "SPACE" => SPACE,
            _ => OTHER,
        };
    }
    keys
}

fn keys_text(keys: u8) -> String {
    if keys == 0 {
        return "-".into();
    }
    let names = [(W, "W"), (A, "A"), (S, "S"), (D, "D"), (SHIFT, "Shift"), (SPACE, "Space"), (OTHER, "other")];
    names.iter().filter(|(b, _)| keys & b != 0).map(|(_, n)| *n).collect::<Vec<_>>().join("+")
}

struct Sample {
    t: f64,
    unix_ms: u128,
    keys: u8,
}

struct Block {
    start: usize,
    len: usize,
    from: usize,
    to: usize,
}

/// Reads of the alive slots, grouped so that near ones share one read.
fn build_blocks(slots: &[Slot]) -> (Vec<u32>, Vec<Block>) {
    const GAP: usize = 256;
    const SPAN: usize = 1 << 16;
    let alive: Vec<u32> = (0..slots.len() as u32).filter(|&i| slots[i as usize].alive).collect();
    let mut blocks: Vec<Block> = Vec::new();
    for (n, &i) in alive.iter().enumerate() {
        let s = &slots[i as usize];
        let (a, end) = (dec(s.addr), dec(s.addr) + s.layout.size());
        match blocks.last_mut() {
            Some(b) if a <= b.start + b.len + GAP && end.max(b.start + b.len) - b.start <= SPAN => {
                b.len = end.max(b.start + b.len) - b.start;
                b.to = n + 1;
            }
            _ => blocks.push(Block { start: a, len: end - a, from: n, to: n + 1 }),
        }
    }
    (alive, blocks)
}

/// One sample's facts that every slot needs.
struct Tick {
    index: usize,
    dt: f32,
    keys: u8,
    prev_keys: u8,
    idle: bool,
    prev_idle: bool,
    /// Samples so far, over the whole run, at which the same movement keys were held as before.
    held_stable: u32,
}

fn observe(s: &mut Slot, m: &Mat, tick: &Tick, config: &Config, tracks: &mut usize) {
    let kill = |s: &mut Slot, tracks: &mut usize| {
        s.alive = false;
        if s.track.take().is_some() {
            *tracks -= 1;
        }
    };
    let finite = m.pos.iter().all(|v| v.is_finite() && v.abs() < 1.0e5);
    if !finite || !m.orthonormal(0.05) {
        kill(s, tracks);
        return;
    }
    let d = [m.pos[0] - s.prev[0], m.pos[1] - s.prev[1], m.pos[2] - s.prev[2]];
    let dist = (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]).sqrt();
    let speed3 = if tick.dt > 0.0 { dist / tick.dt } else { 0.0 };
    // A jump no one walks or runs is something else being written there.
    if speed3 > 25.0 {
        kill(s, tracks);
        return;
    }
    if tick.idle && tick.prev_idle && dist > config.idle_tol {
        s.violations += 1;
        if s.violations > config.idle_max {
            kill(s, tracks);
            return;
        }
    }
    // Never moved although movement keys have been held for a while: static.
    if s.track.is_none() && tick.held_stable >= config.prune_after && dist <= 0.02 {
        kill(s, tracks);
        return;
    }
    if s.track.is_none() && tick.keys & WASD != 0 && (0.15..=25.0).contains(&speed3) && *tracks < config.max_tracks {
        *tracks += 1;
        s.track = Some(Box::<Track>::default());
    }
    if let Some(t) = s.track.as_mut() {
        if tick.idle && tick.prev_idle && dist <= config.idle_tol {
            t.quiet = t.quiet.saturating_add(1);
            if t.pos.is_empty() && t.quiet >= 3 {
                t.first = tick.index - 1;
                t.pos.push(s.prev);
            }
        }
        if !t.pos.is_empty() {
            t.pos.push(m.pos);
        }
        let stable = tick.keys & WASD != 0 && tick.keys == tick.prev_keys;
        if stable && tick.dt > 0.0 {
            let dxy = (d[0] * d[0] + d[1] * d[1]).sqrt();
            let speed = dxy / tick.dt;
            t.steady += 1;
            t.held_dist += dxy;
            if (0.4..=12.0).contains(&speed) {
                t.follow += 1;
            }
            let only = |bit: u8| tick.keys & WASD == bit;
            let class = [only(W), only(S), only(D), only(A)].iter().position(|&c| c);
            if let Some(c) = class {
                if tick.keys & SHIFT != 0 {
                    t.speeds_shift.push(speed);
                } else {
                    t.speeds_w.push(speed);
                }
                if speed > 0.3 {
                    let dir = [d[0] / dxy, d[1] / dxy];
                    t.dir_sum[c][0] += dir[0];
                    t.dir_sum[c][1] += dir[1];
                    for (k, axis) in m.axes.iter().enumerate() {
                        let len = (axis[0] * axis[0] + axis[1] * axis[1]).sqrt();
                        if len > 0.1 {
                            t.cos_sum[c][k] += (dir[0] * axis[0] + dir[1] * axis[1]) / len;
                        }
                    }
                    t.cos_n[c] += 1;
                }
            }
        }
    }
    s.prev = m.pos;
}

fn median(v: &[f32]) -> f32 {
    if v.is_empty() {
        return 0.0;
    }
    let mut v = v.to_vec();
    v.sort_by(f32::total_cmp);
    v[v.len() / 2]
}

/// How well a track follows the keys, and the axis the player walks along.
struct Rank {
    score: f32,
    follow: f32,
    steady: u32,
    held_dist: f32,
    med_w: f32,
    med_shift: f32,
    /// Mean cosine of the W movement with each axis, and of the S and D movement.
    cos_w: [f32; 3],
    cos_s: [f32; 3],
    cos_d: [f32; 3],
    axis: usize,
    sign: f32,
}

fn rank(t: &Track, violations: u8, has_axes: bool) -> Rank {
    let mean = |c: usize| -> [f32; 3] {
        let n = t.cos_n[c].max(1) as f32;
        t.cos_sum[c].map(|v| v / n)
    };
    let (cos_w, cos_s, cos_d) = (mean(0), mean(1), mean(2));
    let axis = (0..3).max_by(|&a, &b| cos_w[a].abs().total_cmp(&cos_w[b].abs())).unwrap_or(0);
    let sign = if cos_w[axis] < 0.0 { -1.0 } else { 1.0 };
    let follow = t.follow as f32 / t.steady.max(1) as f32;
    let med_w = median(&t.speeds_w);
    let mut score = 0.0;
    if t.steady >= 3 && t.held_dist >= 1.0 {
        // W and S walk back and forth: the mean directions are opposite.
        let mean_dir = |c: usize| {
            let n = t.cos_n[c].max(1) as f32;
            [t.dir_sum[c][0] / n, t.dir_sum[c][1] / n]
        };
        let (dw, ds) = (mean_dir(0), mean_dir(1));
        let back_and_forth = t.cos_n[0] > 0 && t.cos_n[1] > 0 && dw[0] * ds[0] + dw[1] * ds[1] < -0.2;
        score = 2.0 * follow;
        if has_axes {
            score += cos_w[axis].abs();
        }
        if back_and_forth {
            score += 0.5;
        }
        if has_axes && t.cos_n[1] > 0 && cos_s[axis] * sign < -0.5 {
            score += 0.5;
        }
        if (0.8..=4.0).contains(&med_w) {
            score += 0.5;
        }
        if has_axes && t.cos_n[2] > 0 && (0..3).any(|j| j != axis && cos_d[j].abs() > 0.6) {
            score += 0.25;
        }
        score -= 0.25 * f32::from(violations);
    }
    Rank {
        score,
        follow,
        steady: t.steady,
        held_dist: t.held_dist,
        med_w,
        med_shift: median(&t.speeds_shift),
        cos_w,
        cos_s,
        cos_d,
        axis,
        sign,
    }
}

// ------------------------------------------------------------------ run

fn region_note(config: &Config, address: usize) -> String {
    let mut mbi = MemoryBasicInformation {
        base_address: 0,
        allocation_base: 0,
        allocation_protect: 0,
        partition_id: 0,
        region_size: 0,
        state: 0,
        protect: 0,
        kind: 0,
    };
    // SAFETY: a plain query into a struct of the documented layout.
    let got = unsafe { VirtualQuery(address, &mut mbi, std::mem::size_of_val(&mbi)) };
    if got == 0 {
        return "not mapped".into();
    }
    let mut s = format!(
        "region {:#x}..{:#x} (+{:#x} into it, {:#x} bytes) allocation base {:#x} (+{:#x}) protect {:#x} type {:#x}",
        mbi.base_address,
        mbi.base_address + mbi.region_size,
        address - mbi.base_address,
        mbi.region_size,
        mbi.allocation_base,
        address - mbi.allocation_base,
        mbi.protect,
        mbi.kind
    );
    if address >= config.base && address < config.base + config.size {
        let rva = address - config.base;
        let section = config.sections.iter().find(|(_, r, n)| rva >= *r && rva < r + n);
        let _ = write!(s, "; in the title's image at rva {rva:#x} {}", section.map_or("", |(n, _, _)| n.as_str()));
    }
    s
}

fn run(config: &Config) -> Result<String, String> {
    let dir = player::out_dir();
    std::fs::create_dir_all(&dir).map_err(|e| format!("{}: {e}", dir.display()))?;
    let buffer = alloc_buffer()?;
    let mut workers = Vec::new();
    for _ in 0..config.threads {
        workers.push(alloc_buffer()?);
    }
    let mut skips = vec![(buffer.as_ptr() as usize, buffer.as_ptr() as usize + CHUNK)];
    skips.extend(workers.iter().map(|b| (b.as_ptr() as usize, b.as_ptr() as usize + CHUNK)));
    let regions = regions(config, &skips);
    let total: usize = regions.iter().map(|r| r.size).sum();
    let started = Instant::now();

    let (mut slots, counts) = scan_candidates(config, &regions, &mut workers);
    let found_in = started.elapsed().as_secs_f32();
    let mut summary = String::new();
    let names = ["R4", "T34", "R43"];
    let _ = writeln!(
        summary,
        "memory scanned: {} regions, {} MiB, {} KiB unreadable, {found_in:.1} s on {} threads; filter {}",
        regions.len(),
        total >> 20,
        counts.unreadable >> 10,
        config.threads,
        if config.strict { "strict" } else { "loose" },
    );
    for k in 0..3 {
        let _ = writeln!(
            summary,
            "  {}: {} unit orthogonal axes with a position; {} with the exact form (w column 0, corner 1); {} with a fractional position; {} not axis-aligned = kept",
            names[k], counts.raw[k], counts.form[k], counts.not_integer[k], counts.kept[k]
        );
    }
    let _ = writeln!(
        summary,
        "  V4 (xyz in the world, w mode {}): {} found, {} kept; D3 (doubles): {} found, {} kept; candidates in all {}; R4 kept with w == 1: {}",
        match config.vec_w {
            VecW::Off => "off",
            VecW::Flag => "flag",
            VecW::Plain => "plain",
            VecW::Any => "any",
        },
        counts.v4_raw,
        counts.v4_kept,
        counts.d3_raw,
        counts.d3_kept,
        slots.len(),
        slots.iter().filter(|s| s.layout == Layout::R4 && (s.w - 1.0).abs() < 1.0e-4).count(),
    );
    eprintln!("Leonida: find player: {}", summary.trim_end());
    slots.shrink_to_fit();

    // Sampling.
    let unix = |t: std::time::SystemTime| t.duration_since(std::time::UNIX_EPOCH).unwrap_or_default().as_millis();
    let t0 = Instant::now();
    let wall0 = unix(std::time::SystemTime::now());
    let mut samples: Vec<Sample> = Vec::new();
    let mut tracks = 0usize;
    let (mut alive, mut blocks) = build_blocks(&slots);
    let mut alive_at_build = alive.len();
    let mut last_key_t = f64::NEG_INFINITY;
    let mut prev_idle = false;
    let mut read_buf = vec![0u8; 1 << 17];
    let mut next = t0;
    let mut held_stable = 0u32;
    eprintln!(
        "Leonida: find player: sampling starts now ({} candidates, {} s, every {} ms, keys file {:?}); unix ms at t=0: {wall0}",
        slots.len(),
        config.seconds,
        config.interval.as_millis(),
        std::env::var("LEONIDA_KEYS_FILE").ok()
    );
    loop {
        let now = Instant::now();
        let t = now.duration_since(t0).as_secs_f64();
        let keys = keys_now();
        if keys != 0 {
            last_key_t = t;
        }
        let idle = keys == 0 && t - last_key_t >= 1.0;
        let index = samples.len();
        let dt = index.checked_sub(1).map_or(0.0, |p| (t - samples[p].t) as f32);
        let prev_keys = samples.last().map_or(0, |s| s.keys);
        if keys & WASD != 0 && keys == prev_keys {
            held_stable += 1;
        }
        let tick = Tick {
            index,
            dt,
            keys,
            prev_keys,
            idle,
            prev_idle,
            held_stable,
        };
        for b in &blocks {
            let ok = b.len <= read_buf.len() && mem::read(b.start, &mut read_buf[..b.len]);
            for &i in &alive[b.from..b.to] {
                let s = &mut slots[i as usize];
                let (a, size) = (dec(s.addr), s.layout.size());
                let fresh;
                let bytes: Option<&[u8]> = if ok {
                    Some(&read_buf[a - b.start..a - b.start + size])
                } else {
                    fresh = mem::read_vec(a, size);
                    fresh.as_deref()
                };
                let Some(bytes) = bytes else {
                    s.alive = false;
                    if s.track.take().is_some() {
                        tracks -= 1;
                    }
                    continue;
                };
                let m = player::decode(bytes, s.layout);
                if index == 0 {
                    s.prev = m.pos;
                } else {
                    observe(s, &m, &tick, config, &mut tracks);
                }
            }
        }
        samples.push(Sample { t, unix_ms: wall0 + (t * 1000.0) as u128, keys });
        prev_idle = idle;
        let alive_now = slots.iter().filter(|s| s.alive).count();
        if alive_now * 10 < alive_at_build * 9 {
            (alive, blocks) = build_blocks(&slots);
            alive_at_build = alive.len();
        }
        if index % 20 == 0 {
            eprintln!(
                "Leonida: find player: sample {index} t={t:.1}s keys={} alive {alive_now} tracked {tracks}",
                keys_text(keys)
            );
        }
        if t >= config.seconds {
            break;
        }
        next += config.interval;
        if let Some(wait) = next.checked_duration_since(Instant::now()) {
            std::thread::sleep(wait);
        } else {
            next = Instant::now();
        }
    }
    let alive_end = slots.iter().filter(|s| s.alive).count();
    let _ = writeln!(
        summary,
        "sampling: {} samples over {:.1} s (wall clock of t=0: unix ms {wall0}), alive at the end {alive_end}, tracked {tracks}",
        samples.len(),
        samples.last().map_or(0.0, |s| s.t)
    );

    // Ranking.
    let mut ranked: Vec<(usize, Rank)> = slots
        .iter()
        .enumerate()
        .filter(|(_, s)| s.alive)
        .filter_map(|(i, s)| Some((i, rank(s.track.as_ref()?, s.violations, s.layout.has_axes()))))
        .filter(|(_, r)| r.score > 0.0)
        .collect();
    // Ones that also stayed put at the idle samples (their positions were kept) first.
    let verified = |i: usize| !slots[i].track.as_ref().is_none_or(|t| t.pos.is_empty());
    ranked.sort_by(|a, b| verified(b.0).cmp(&verified(a.0)).then(b.1.score.total_cmp(&a.1.score)));
    let _ = writeln!(summary, "ranked (moved with the keys): {}", ranked.len());

    let mut text = String::new();
    for (n, (i, r)) in ranked.iter().take(config.top).enumerate() {
        let s = &slots[*i];
        let _ = writeln!(
            text,
            "#{n} addr={:#x} {} w={:.2} score={:.2} follow={:.2} steady={} dist={:.1} m median speed W={:.2} W+Shift={:.2} m/s violations={} axis={} sign={:+} cosW={:.2?} cosS={:.2?} cosD={:.2?}",
            dec(s.addr),
            s.layout.name(),
            s.w,
            r.score,
            r.follow,
            r.steady,
            r.held_dist,
            r.med_w,
            r.med_shift,
            s.violations,
            r.axis,
            r.sign,
            r.cos_w,
            r.cos_s,
            r.cos_d
        );
    }
    write(&dir, "candidates.txt", &text)?;
    let mut text = String::from("sample,t,unix_ms,keys\n");
    for (n, s) in samples.iter().enumerate() {
        let _ = writeln!(text, "{n},{:.3},{},{}", s.t, s.unix_ms, keys_text(s.keys));
    }
    write(&dir, "samples.txt", &text)?;

    let Some(&(best_i, _)) = ranked.first() else {
        write(&dir, "summary.txt", &summary)?;
        return Err(format!("nothing moved with the keys; {}", summary.trim_end().replace('\n', "; ")));
    };
    let best = &slots[best_i];
    let best_track = best.track.as_ref().expect("ranked slots have tracks");
    let (best_addr, best_layout) = (dec(best.addr), best.layout);
    let best_rank = rank(best_track, best.violations, best_layout.has_axes());
    write(
        &dir,
        "best.txt",
        &format!(
            "best candidate {best_addr:#x} {} score {:.2}\n{}\nforward axis {} sign {:+} (cosines with W: {:.2?})\nstruct start candidates: see pointers.txt\n",
            best_layout.name(),
            best_rank.score,
            region_note(config, best_addr),
            best_rank.axis,
            best_rank.sign,
            best_rank.cos_w
        ),
    )?;

    // Transforms that move together with the best one (a constant offset).
    let mut movers: Vec<(usize, [f32; 3], f32)> = Vec::new();
    for (i, r) in &ranked {
        let (s, t) = (&slots[*i], slots[*i].track.as_ref().unwrap());
        let from = t.first.max(best_track.first);
        let end = (t.first + t.pos.len()).min(best_track.first + best_track.pos.len());
        let n = end.saturating_sub(from);
        if n < 5 || r.held_dist < 0.7 * best_rank.held_dist {
            continue;
        }
        let off = |k: usize| {
            let (a, b) = (t.pos[from - t.first + k], best_track.pos[from - best_track.first + k]);
            [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
        };
        let mut mean = [0.0f32; 3];
        for k in 0..n {
            for (m, v) in mean.iter_mut().zip(off(k)) {
                *m += v / n as f32;
            }
        }
        let dev = (0..n)
            .map(|k| {
                let o = off(k);
                ((o[0] - mean[0]).powi(2) + (o[1] - mean[1]).powi(2) + (o[2] - mean[2]).powi(2)).sqrt()
            })
            .fold(0.0, f32::max);
        if dev <= 0.10 {
            movers.push((*i, mean, dev));
        }
        let _ = s;
    }
    let mut text = String::new();
    for &(i, mean, dev) in &movers {
        let _ = writeln!(
            text,
            "addr={:#x} {} offset from the best ({:+.3}, {:+.3}, {:+.3}) max deviation {dev:.3} m; {:+#x} bytes from the best",
            dec(slots[i].addr),
            slots[i].layout.name(),
            mean[0],
            mean[1],
            mean[2],
            dec(slots[i].addr) as i64 - best_addr as i64
        );
    }
    write(&dir, "comovers.txt", &text)?;
    let _ = writeln!(summary, "move together with the best: {}", movers.len());

    // Tracks of the best ranked and of the co-movers.
    let mut csv = String::from("rank,addr,layout,sample,t,unix_ms,x,y,z,keys\n");
    let mut shown: Vec<usize> = ranked.iter().take(30).map(|&(i, _)| i).collect();
    shown.extend(movers.iter().map(|m| m.0).filter(|i| !shown.contains(i)).take(20).collect::<Vec<_>>());
    for i in shown {
        let (s, t) = (&slots[i], slots[i].track.as_ref().unwrap());
        let n = ranked.iter().position(|(j, _)| *j == i).unwrap_or(0);
        for (k, p) in t.pos.iter().enumerate() {
            let sample = &samples[t.first + k];
            let _ = writeln!(
                csv,
                "{n},{:#x},{},{},{:.3},{},{:.3},{:.3},{:.3},{}",
                dec(s.addr),
                s.layout.name(),
                t.first + k,
                sample.t,
                sample.unix_ms,
                p[0],
                p[1],
                p[2],
                keys_text(sample.keys)
            );
        }
    }
    write(&dir, "tracks.csv", &csv)?;

    // The groups: the best, then the co-movers that are not at nearly the same
    // address as one already taken.
    let mut picked: Vec<(usize, Layout)> = vec![(enc(best_addr), best_layout)];
    for &(i, _, _) in &movers {
        let (a, l) = (dec(slots[i].addr), slots[i].layout);
        if picked.len() < config.groups && picked.iter().all(|&(p, _)| dec(p).abs_diff(a) >= 0x40) {
            picked.push((enc(a), l));
        }
    }
    // The direction the player walks along its own axes was measured on the
    // first transform with axes (a position vector has none).
    let forward = picked.iter().find(|&&(_, l)| l.has_axes()).and_then(|&(a, _)| {
        let s = slots.iter().find(|s| s.addr == a)?;
        let r = rank(s.track.as_ref()?, s.violations, true);
        Some((r.axis, r.sign))
    });
    let _ = writeln!(summary, "groups for the teleport: {} (addresses in comovers.txt)", picked.len());
    let best_line = format!(
        "best {best_addr:#x} {} ({}); {} co-movers",
        best_layout.name(),
        region_note(config, best_addr),
        movers.len()
    );

    // Free what holds addresses (the candidates hold coordinates the probe would find).
    let derived = (best_addr, best_layout);
    drop(slots);
    drop(ranked);
    let mut groups: Vec<(usize, Layout)> = picked.iter().map(|&(a, l)| (dec(a), l)).collect();
    let mut probed = false;
    match probe(config, &regions, &mut workers, derived, &dir) {
        Ok(Some(found)) => {
            probed = found.drives;
            if !found.drives {
                let _ = writeln!(summary, "write probe: no triple moved the derived position; the group is triples that move other copies");
            }
            groups = found.group;
        }
        Ok(None) => {}
        Err(e) => eprintln!("Leonida: find player: probe failed: {e}"),
    }
    let _ = writeln!(summary, "write probe: {}", if probed { "found what drives the derived position (probe.txt)" } else { "found nothing (probe.txt)" });
    let mut known = Known {
        groups: groups
            .iter()
            .enumerate()
            .map(|(n, &(a, l))| Group {
                layout: l,
                primary: n == 0,
                paths: Vec::new(),
                fixed: Some(a),
            })
            .collect(),
        forward: if probed { None } else { forward },
    };
    // The teleport can go ahead now; the paths come later.
    player::set_known(known.clone());
    player::find_finished();
    write(&dir, "summary.txt", &summary)?;
    if config.pointers {
        // Only the struct that was written to (or the best candidate's, when nothing was).
        for g in &mut known.groups {
            g.fixed = g.fixed.map(enc);
        }
        let report = pointer_scan(config, &regions, &mut workers, &mut known, &[0], &dir)?;
        let _ = writeln!(summary, "{report}");
    }
    for g in &mut known.groups {
        // The address serves this run; the paths the next ones.
        g.fixed = g.fixed.map(dec);
    }
    let with_paths = known.groups.iter().filter(|g| !g.paths.is_empty()).count();
    if with_paths > 0 {
        let mut persisted = known.clone();
        for g in &mut persisted.groups {
            g.fixed = None;
        }
        // A group without paths cannot be found again.
        persisted.groups.retain(|g| !g.paths.is_empty());
        if !persisted.groups.iter().any(|g| g.primary) {
            persisted.groups[0].primary = true;
        }
        write(&dir, "paths.txt", &persisted.to_text(config.size))?;
    }
    player::set_known(known);
    write(&dir, "summary.txt", &summary)?;
    Ok(format!("{best_line}; {with_paths} group(s) with image paths; details in {}", dir.display()))
}

fn write(dir: &Path, name: &str, text: &str) -> Result<(), String> {
    std::fs::write(dir.join(name), text).map_err(|e| format!("{name}: {e}"))
}

// ----------------------------------------------------- parallel scans

/// Runs `f(address, bytes, limit, &mut T)` over every 2 MiB block of `regions` on
/// `buffers.len()` threads (positions below `limit` belong to the block; `overlap`
/// more bytes are readable after them). One `T` per thread, and the bytes that
/// could not be read.
fn scan_parallel<T: Default + Send>(
    regions: &[Region],
    buffers: &mut [&'static mut [u8]],
    overlap: usize,
    f: impl Fn(usize, &[u8], usize, &mut T) + Sync,
) -> (Vec<T>, usize) {
    const SPAN: usize = 2 << 20;
    let mut tasks: Vec<(usize, usize)> = Vec::new();
    for r in regions {
        let mut at = r.start;
        while at < r.start + r.size {
            tasks.push((at, r.start + r.size));
            at += SPAN;
        }
    }
    let next = AtomicUsize::new(0);
    let lost = AtomicUsize::new(0);
    let parts: Vec<T> = std::thread::scope(|scope| {
        let handles: Vec<_> = buffers
            .iter_mut()
            .map(|buffer| {
                let (tasks, next, lost, f) = (&tasks, &next, &lost, &f);
                scope.spawn(move || {
                    let mut out = T::default();
                    loop {
                        let i = next.fetch_add(1, Ordering::Relaxed);
                        let Some(&(at, end)) = tasks.get(i) else { break };
                        let limit = SPAN.min(end - at);
                        let want = (limit + overlap).min(end - at);
                        lost.fetch_add(read_robust(at, &mut buffer[..want]), Ordering::Relaxed);
                        f(at, &buffer[..want], limit, &mut out);
                    }
                    out
                })
            })
            .collect();
        handles.into_iter().filter_map(|h| h.join().ok()).collect()
    });
    (parts, lost.load(Ordering::Relaxed))
}

// ----------------------------------------------------------- write probe

/// A triple of numbers near the player's derived position.
#[derive(Clone)]
struct Near {
    addr: usize,
    layout: Layout,
    /// What it held when found.
    value: [f32; 3],
    /// Added to x and y to get the world position (a multiple of the cell size for
    /// a sector-relative value; zero otherwise).
    shift: [f32; 2],
    /// 0 for world coordinates, else the cell size the position is relative to.
    frame: u16,
}

/// Cell sizes of the sector-relative frames that are tried.
const CELLS: [f32; 4] = [512.0, 1024.0, 2048.0, 4096.0];

/// Every f32 triple (4-byte aligned) and f64 triple (8-byte aligned) with x and y
/// within `radius` metres of `p`, or of `p` minus a multiple of a cell size, and z
/// in a window: f32 from `p.z - 0.5` to `p.z + 2.5` (root, centre, head), f64 from
/// `p.z - 1` to `p.z + 3`.
fn scan_near(regions: &[Region], workers: &mut [&'static mut [u8]], p: [f32; 3], radius: f32) -> Vec<Near> {
    // Which frame an (x, y) pair of `p` is in: the shift to the world, and the cell (0 = world).
    fn frame_of(x: f64, y: f64, p: [f32; 3], radius: f64) -> Option<([f32; 2], u16)> {
        let (px, py) = (f64::from(p[0]), f64::from(p[1]));
        if (x - px).abs() < radius && (y - py).abs() < radius {
            return Some(([0.0, 0.0], 0));
        }
        for cell in CELLS {
            let c = f64::from(cell);
            if x.abs() > c || y.abs() > c {
                continue;
            }
            let (mx, my) = (((px - x) / c).round(), ((py - y) / c).round());
            if mx == 0.0 || my == 0.0 || (px - x - mx * c).abs() >= radius || (py - y - my * c).abs() >= radius {
                // Only one axis shifted is not a sector origin.
                continue;
            }
            return Some(([(mx * c) as f32, (my * c) as f32], cell as u16));
        }
        None
    }
    let radius64 = f64::from(radius);
    let (parts, _) = scan_parallel::<Vec<Near>>(regions, workers, 32, |address, bytes, limit, out| {
        // SAFETY: the buffer is page aligned; `bytes` is a prefix of it.
        let f = unsafe { std::slice::from_raw_parts(bytes.as_ptr() as *const f32, bytes.len() / 4) };
        let (lo, hi) = (p[2] - 0.5, p[2] + 2.5);
        for j in 0..(limit / 4).min(f.len().saturating_sub(2)) {
            let z = f[j + 2];
            if !(z >= lo && z <= hi) {
                continue;
            }
            if let Some((shift, frame)) = frame_of(f64::from(f[j]), f64::from(f[j + 1]), p, radius64) {
                out.push(Near { addr: address + j * 4, layout: Layout::V4, value: [f[j], f[j + 1], z], shift, frame });
            }
        }
        // SAFETY: as above; 8-byte aligned.
        let d = unsafe { std::slice::from_raw_parts(bytes.as_ptr() as *const f64, bytes.len() / 8) };
        let (lo, hi) = (f64::from(p[2]) - 1.0, f64::from(p[2]) + 3.0);
        for i in 0..(limit / 8).min(d.len().saturating_sub(2)) {
            let z = d[i + 2];
            if !(z >= lo && z <= hi) {
                continue;
            }
            if let Some((shift, frame)) = frame_of(d[i], d[i + 1], p, radius64) {
                out.push(Near { addr: address + i * 8, layout: Layout::D3, value: [d[i] as f32, d[i + 1] as f32, z as f32], shift, frame });
            }
        }
    });
    let mut all: Vec<Near> = parts.into_iter().flatten().collect();
    all.sort_by_key(|n| (n.addr, n.layout as u8));
    all
}

fn pos_of(address: usize, layout: Layout) -> Option<[f32; 3]> {
    let b = mem::read_vec(address, layout.size())?;
    let p = player::decode(&b, layout).pos;
    p.iter().all(|v| v.is_finite()).then_some(p)
}

/// The transform whose position row a triple is, when it follows a 4x4 or a packed 4x3.
fn matrix_before(address: usize) -> Option<(usize, Layout)> {
    for (back, layout) in [(48, Layout::R4), (36, Layout::R43)] {
        if let Some(a) = address.checked_sub(back)
            && player::read_mat(a, layout).is_some()
        {
            return Some((a, layout));
        }
    }
    None
}

/// What the probe found: the group to write to (the authoritative one first).
struct Probed {
    group: Vec<(usize, Layout)>,
    /// Whether a write drove the derived position (else: only candidates upstream of other copies).
    drives: bool,
}

fn wait_still(config: &Config, derived: (usize, Layout)) -> [f32; 3] {
    // The candidates are looked for around where the player is at the start of the
    // scan: wait until no key is held and the derived position has stopped.
    let deadline = Instant::now() + Duration::from_secs(120);
    let mut last: Option<([f32; 3], Instant)> = None;
    loop {
        let p = pos_of(derived.0, derived.1).unwrap_or([0.0; 3]);
        let moved = last.is_none_or(|(q, _)| (0..3).any(|i| (p[i] - q[i]).abs() > 0.02));
        if moved || keys_now() != 0 {
            last = Some((p, Instant::now()));
        }
        let still = last.is_some_and(|(_, at)| at.elapsed() >= Duration::from_secs(2));
        if still || Instant::now() > deadline {
            let _ = config;
            return p;
        }
        std::thread::sleep(Duration::from_millis(100));
    }
}

/// Batch size of the first level of the probe.
const BATCH: usize = 32;

/// What writing +3 m in z to some triples did.
#[derive(Default)]
struct Effect {
    /// The most the derived position rose.
    derived: f32,
    /// Triples that were not written and rose (index, rise).
    moved: Vec<(usize, f32)>,
    written: usize,
    kept: usize,
}

impl Effect {
    fn drove(&self) -> bool {
        (2.5..=3.5).contains(&self.derived)
    }
    fn upstream(&self) -> Vec<usize> {
        self.moved.iter().filter(|m| (2.0..=3.6).contains(&m.1)).map(|m| m.0).collect()
    }
    fn effective(&self) -> bool {
        self.drove() || !self.upstream().is_empty()
    }
}

/// The triple's position in the world (its frame's shift added).
fn world_of(h: &Near, v: [f32; 3]) -> [f32; 3] {
    [v[0] + h.shift[0], v[1] + h.shift[1], v[2]]
}

struct Cx<'a> {
    hits: &'a [Near],
    derived: (usize, Layout),
    log: String,
    trials: usize,
    deadline: Instant,
    /// Leaves that moved other triples but not the derived one (leaf, the ones it moved).
    upstream: Vec<(usize, Vec<usize>)>,
}

/// Writes the value + 3 m in z to every member that is still near the player, every
/// 5 ms for 300 ms, and watches the derived position and every other triple.
fn trial(cx: &mut Cx, members: &[usize]) -> Effect {
    let hits = cx.hits;
    let Some(b0) = pos_of(cx.derived.0, cx.derived.1) else { return Effect::default() };
    let mut live: Vec<(usize, [f32; 3])> = Vec::new();
    for &m in members {
        let h = &hits[m];
        let Some(c0) = pos_of(h.addr, h.layout) else { continue };
        let w = world_of(h, c0);
        // Still where the player is: x and y within 1 m, z from 1.5 m below to 3.5 m above.
        if (w[0] - b0[0]).abs() <= 1.0 && (w[1] - b0[1]).abs() <= 1.0 && w[2] >= b0[2] - 1.5 && w[2] <= b0[2] + 3.5 {
            live.push((m, c0));
        }
    }
    let mut e = Effect { written: live.len(), derived: f32::MIN, ..Effect::default() };
    if live.is_empty() {
        return e;
    }
    let mut is_member = vec![false; hits.len()];
    for &(m, _) in &live {
        is_member[m] = true;
    }
    let others: Vec<usize> = (0..hits.len()).filter(|&k| !is_member[k]).collect();
    let base: Vec<Option<f32>> = others.iter().map(|&k| pos_of(hits[k].addr, hits[k].layout).map(|v| v[2])).collect();
    let mut rise = vec![f32::MIN; others.len()];
    let poll_others = |rise: &mut Vec<f32>| {
        for (n, &k) in others.iter().enumerate() {
            if let (Some(b), Some(v)) = (base[n], pos_of(hits[k].addr, hits[k].layout)) {
                rise[n] = rise[n].max(v[2] - b);
            }
        }
    };
    let end = Instant::now() + Duration::from_millis(300);
    let mut round = 0u32;
    // The only writes the probe makes: these triples, +3 m in z, every 5 ms for 300 ms.
    while Instant::now() < end {
        for &(m, c0) in &live {
            player::write_pos(hits[m].addr, hits[m].layout, [c0[0], c0[1], c0[2] + 3.0]);
        }
        if let Some(b) = pos_of(cx.derived.0, cx.derived.1) {
            e.derived = e.derived.max(b[2] - b0[2]);
        }
        if round % 3 == 2 {
            poll_others(&mut rise);
        }
        round += 1;
        std::thread::sleep(Duration::from_millis(5));
    }
    // A short grace: copies may follow a frame later.
    let grace = Instant::now() + Duration::from_millis(100);
    while Instant::now() < grace {
        if let Some(b) = pos_of(cx.derived.0, cx.derived.1) {
            e.derived = e.derived.max(b[2] - b0[2]);
        }
        std::thread::sleep(Duration::from_millis(5));
    }
    poll_others(&mut rise);
    e.kept = live
        .iter()
        .filter(|&&(m, c0)| pos_of(hits[m].addr, hits[m].layout).is_some_and(|a| (a[2] - (c0[2] + 3.0)).abs() < 0.2))
        .count();
    e.moved = others.iter().zip(&rise).filter(|(_, r)| **r > 0.5).map(|(&k, &r)| (k, r)).collect();
    cx.trials += 1;
    e
}

struct Found {
    members: Vec<usize>,
    moved: Vec<usize>,
}

fn describe(hits: &[Near], members: &[usize]) -> String {
    let first = &hits[members[0]];
    format!("{} x {} (first {:#x} {})", members.len(), "triples", first.addr, first.layout.name())
}

/// Tests the halves of an effective set of triples; the first single triple (or
/// set that only works together) that drives the derived position is returned.
fn bisect(cx: &mut Cx, members: &[usize], path: &str, whole: &Effect) -> Option<Found> {
    if members.len() == 1 {
        let h = &cx.hits[members[0]];
        if whole.drove() {
            return Some(Found { members: members.to_vec(), moved: whole.upstream() });
        }
        let up = whole.upstream();
        let _ = writeln!(
            cx.log,
            "UPSTREAM {:#x} {} frame {}: writing it moved {} other triples by 2-3.6 m: {:x?}",
            h.addr,
            h.layout.name(),
            h.frame,
            up.len(),
            up.iter().take(12).map(|&k| cx.hits[k].addr).collect::<Vec<_>>()
        );
        cx.upstream.push((members[0], up));
        return None;
    }
    let (left, right) = members.split_at(members.len() / 2);
    let mut any = false;
    for (name, half) in [("L", left), ("R", right)] {
        if Instant::now() > cx.deadline {
            return None;
        }
        let e = trial(cx, half);
        let path = format!("{path} > {name}[{}]", half.len());
        let _ = writeln!(
            cx.log,
            "  {path}: {}; wrote {} kept {} derived rose {:.2} m, {} others moved 2-3.6 m",
            describe(cx.hits, half),
            e.written,
            e.kept,
            e.derived,
            e.upstream().len()
        );
        if e.effective() {
            any = true;
            if let Some(f) = bisect(cx, half, &path, &e) {
                return Some(f);
            }
        }
    }
    if !any && whole.drove() {
        let _ = writeln!(cx.log, "  {path}: neither half alone moves it, so the set works together");
        return Some(Found { members: members.to_vec(), moved: whole.upstream() });
    }
    None
}

fn probe(
    config: &Config,
    regions: &[Region],
    workers: &mut [&'static mut [u8]],
    derived: (usize, Layout),
    dir: &Path,
) -> Result<Option<Probed>, String> {
    if let Some(at) = config.probe_at {
        let wait = at - config.started.elapsed().as_secs_f64();
        if wait > 0.0 {
            eprintln!("Leonida: find player: probe waits {wait:.0} s (LEONIDA_PLAYER_PROBE={at})");
            std::thread::sleep(Duration::from_secs_f64(wait));
        }
    }
    eprintln!("Leonida: find player: probe: waiting for the player to stand still (hold no keys)");
    let mut p = wait_still(config, derived);
    let mut hits = Vec::new();
    for attempt in 0..3 {
        let t = Instant::now();
        hits = scan_near(regions, workers, p, 0.5);
        let now = pos_of(derived.0, derived.1).unwrap_or(p);
        let drift = (0..3).map(|i| (now[i] - p[i]).abs()).fold(0.0, f32::max);
        eprintln!(
            "Leonida: find player: probe scan {attempt}: {} triples near ({:.2}, {:.2}, {:.2}) in {:.1} s; the player moved {drift:.2} m meanwhile",
            hits.len(),
            p[0],
            p[1],
            p[2],
            t.elapsed().as_secs_f32()
        );
        if drift < 0.3 {
            break;
        }
        p = wait_still(config, derived);
    }
    // The derived one's own address is the reference, not a candidate.
    hits.retain(|h| !(h.addr >= derived.0 && h.addr < derived.0 + 64) && !(derived.0 >= h.addr && derived.0 < h.addr + 24));
    // How many of each kind.
    let mut by_kind: std::collections::BTreeMap<(u8, u16), usize> = std::collections::BTreeMap::new();
    for h in &hits {
        *by_kind.entry((h.layout as u8, h.frame)).or_default() += 1;
    }
    let mut kinds = String::new();
    for ((layout, frame), n) in &by_kind {
        let _ = write!(
            kinds,
            "{} {} {n}; ",
            if *layout == Layout::V4 as u8 { "f32" } else { "f64" },
            if *frame == 0 { "world".to_string() } else { format!("relative to {frame} m cells") }
        );
    }
    eprintln!("Leonida: find player: probe candidates: {kinds}");
    // Same triple at the same moment: one group.
    let key = |h: &Near| h.value.map(f32::to_bits);
    let mut sizes = std::collections::HashMap::new();
    for h in &hits {
        *sizes.entry(key(h)).or_insert(0usize) += 1;
    }
    // Transform positions first, then the larger groups, then by address.
    let mut order: Vec<(bool, usize, usize)> = hits
        .iter()
        .enumerate()
        .map(|(i, h)| (h.layout == Layout::V4 && h.frame == 0 && matrix_before(h.addr).is_some(), sizes[&key(h)], i))
        .collect();
    order.sort_by(|a, b| b.0.cmp(&a.0).then(b.1.cmp(&a.1)).then(a.2.cmp(&b.2)));
    let mut text = format!("# {kinds}\n");
    for h in &hits {
        let _ = writeln!(
            text,
            "{:#x} {} frame {} ({:.3}, {:.3}, {:.3}) group of {}; matrix before: {}; {}",
            h.addr,
            h.layout.name(),
            h.frame,
            h.value[0],
            h.value[1],
            h.value[2],
            sizes[&key(h)],
            matrix_before(h.addr).map_or("no".to_string(), |(a, l)| format!("{} at {a:#x}", l.name())),
            region_note(config, h.addr)
        );
    }
    write(dir, "probe-candidates.txt", &text)?;

    let ids: Vec<usize> = order.iter().map(|o| o.2).collect();
    let started = Instant::now();
    let mut cx = Cx {
        hits: &hits,
        derived,
        log: String::new(),
        trials: 0,
        deadline: started + Duration::from_secs(config.probe_seconds),
        upstream: Vec::new(),
    };
    let batches = ids.len().div_ceil(BATCH);
    let mut found: Option<Found> = None;
    for (n, batch) in ids.chunks(BATCH).enumerate() {
        if Instant::now() > cx.deadline {
            let _ = writeln!(cx.log, "stopped after {n} of {batches} batches: LEONIDA_PLAYER_PROBE_SECONDS reached");
            break;
        }
        let e = trial(&mut cx, batch);
        let path = format!("batch {n}");
        let _ = writeln!(
            cx.log,
            "{path}: {}; wrote {} kept {} derived rose {:.2} m, {} others moved 2-3.6 m",
            describe(&hits, batch),
            e.written,
            e.kept,
            e.derived,
            e.upstream().len()
        );
        if e.effective() {
            found = bisect(&mut cx, batch, &path, &e);
            if found.is_some() {
                break;
            }
        }
        if n % 10 == 0 {
            eprintln!("Leonida: find player: probe batch {n}/{batches} ({:.0} s), {} upstream so far", started.elapsed().as_secs_f32(), cx.upstream.len());
            let _ = write(dir, "probe.txt", &cx.log);
        }
    }
    let _ = writeln!(cx.log, "{} trials in {:.0} s", cx.trials, started.elapsed().as_secs_f32());
    write(dir, "probe.txt", &cx.log)?;

    // Turns a triple into what the teleport should write: the transform whose position row it is, if any.
    // Not a camera focus (a bare vector more than 1.5 m above the derived position; a full
    // transform is kept) and not all zeros.
    let dpos = pos_of(derived.0, derived.1);
    let add = |a: usize, l: Layout, group: &mut Vec<(usize, Layout)>| {
        if !l.has_axes() {
            match (pos_of(a, l), dpos) {
                (Some(p), _) if p == [0.0; 3] => return,
                (Some(p), Some(d)) if p[2] > d[2] + 1.5 => return,
                (None, _) => return,
                _ => {}
            }
        }
        if group.iter().all(|&(g, _)| g.abs_diff(a) >= 0x10) {
            group.push((a, l));
        }
    };
    let add_hit = |k: usize, group: &mut Vec<(usize, Layout)>| {
        let h = &hits[k];
        match matrix_before(h.addr).filter(|_| h.frame == 0) {
            Some((a, l)) => add(a, l, group),
            None => add(h.addr, h.layout, group),
        }
    };
    let mut group: Vec<(usize, Layout)> = Vec::new();
    if let Some(f) = &found {
        for &m in &f.members {
            add_hit(m, &mut group);
        }
        add(derived.0, derived.1, &mut group);
        for &k in &f.moved {
            add_hit(k, &mut group);
        }
        eprintln!(
            "Leonida: find player: probe: {} triple(s) drive the derived position ({:#x}...); {} transform(s)/vector(s) change together",
            f.members.len(),
            hits[f.members[0]].addr,
            group.len()
        );
        return Ok(Some(Probed { group, drives: true }));
    }
    if !cx.upstream.is_empty() {
        // Nothing moved the derived copy, but some triples move others: the most upstream first.
        let mut up = cx.upstream.clone();
        up.sort_by_key(|u| std::cmp::Reverse(u.1.len()));
        for (leaf, moved) in up.iter().take(4) {
            add_hit(*leaf, &mut group);
            for &k in moved.iter().take(6) {
                add_hit(k, &mut group);
            }
        }
        add(derived.0, derived.1, &mut group);
        eprintln!("Leonida: find player: probe: nothing drove the derived position; {} upstream triples move others (probe.txt); group of {}", cx.upstream.len(), group.len());
        return Ok(Some(Probed { group, drives: false }));
    }
    eprintln!("Leonida: find player: probe: none of {} triples drove the derived position or any other (probe.txt)", hits.len());
    Ok(None)
}

// ------------------------------------------------------------- pointers

/// A qword in memory that points, directly or through blocks, to a struct.
struct Node {
    /// `enc`oded location of the qword.
    loc: usize,
    /// The node one level closer to the struct.
    parent: u32,
    /// Added to the qword's value to get the parent's location.
    delta: u32,
    group: u16,
    /// The struct start is the matrix address minus 16 * k.
    k: u8,
}

const NO_PARENT: u32 = u32::MAX;
const NODE_CAP: usize = 400_000;
/// How far into a block a pointer may sit.
const WINDOW: usize = 0x2000;
const LOWER_BITS: u32 = 22;

fn bloom_slot(key: u64) -> (usize, u64) {
    let h = key.wrapping_mul(0x9E37_79B9_7F4A_7C15) >> (64 - LOWER_BITS);
    ((h >> 6) as usize, 1u64 << (h & 63))
}

fn pointer_scan(
    config: &Config,
    regions: &[Region],
    workers: &mut [&'static mut [u8]],
    known: &mut Known,
    only: &[usize],
    dir: &Path,
) -> Result<String, String> {
    let started = Instant::now();
    let mut report = String::new();
    // Struct start candidates of each group, up to 0x400 bytes before the position.
    let mut targets: Vec<(usize, u16, u8)> = Vec::new();
    for (g, group) in known.groups.iter().enumerate() {
        if !only.contains(&g) {
            continue;
        }
        let matrix = group.fixed.map_or(0, dec);
        for k in 0..=64u8 {
            if let Some(start) = matrix.checked_sub(16 * usize::from(k)) {
                targets.push((enc(start), g as u16, k));
            }
        }
    }
    if targets.is_empty() {
        return Ok(String::new());
    }
    targets.sort_unstable();
    let (min_t, max_t) = (dec(targets[0].0), dec(targets[targets.len() - 1].0));
    let mut bloom = vec![0u64; 1 << (LOWER_BITS - 6)];
    for t in &targets {
        let (w, b) = bloom_slot(dec(t.0) as u64 >> 4);
        bloom[w] |= b;
    }

    // Level 1: a qword equal to a struct candidate's address.
    let (parts, _) = scan_parallel::<Vec<(usize, usize)>>(regions, workers, 0, |address, bytes, limit, out| {
        // SAFETY: the buffer is page aligned; `bytes` is a prefix of it.
        let q = unsafe { std::slice::from_raw_parts(bytes.as_ptr() as *const u64, bytes.len() / 8) };
        for (j, &v) in q[..limit / 8].iter().enumerate() {
            let v = v as usize;
            if v < min_t || v > max_t {
                continue;
            }
            let (w, b) = bloom_slot(v as u64 >> 4);
            if bloom[w] & b == 0 {
                continue;
            }
            let e = enc(v);
            let mut at = targets.partition_point(|t| t.0 < e);
            while at < targets.len() && targets[at].0 == e {
                out.push((address + j * 8, at));
                at += 1;
            }
        }
    });
    let mut nodes: Vec<Node> = Vec::new();
    let mut capped = false;
    for (loc, t) in parts.into_iter().flatten() {
        if nodes.len() >= NODE_CAP {
            capped = true;
            break;
        }
        nodes.push(Node { loc: enc(loc), parent: NO_PARENT, delta: 0, group: targets[t].1, k: targets[t].2 });
    }
    let in_image = |loc: usize| {
        let rva = dec(loc).wrapping_sub(config.base);
        config.writable_image.iter().any(|&(lo, hi)| rva >= lo && rva < hi)
    };
    let _ = writeln!(report, "pointer level 1: {} qwords point at a struct candidate{}", nodes.len(), if capped { " (capped)" } else { "" });
    let mut frontier: Vec<u32> = (0..nodes.len() as u32).filter(|&i| !in_image(nodes[i as usize].loc)).collect();
    let mut level_ranges = vec![(0usize, nodes.len())];

    for level in 2..=config.levels as u8 {
        if frontier.is_empty() {
            break;
        }
        let mut locs: Vec<(usize, u32)> = frontier.iter().map(|&i| (nodes[i as usize].loc, i)).collect();
        locs.sort_unstable();
        let (min_l, max_l) = (dec(locs[0].0), dec(locs[locs.len() - 1].0));
        let mut pages = vec![0u64; 1 << (LOWER_BITS - 6)];
        for &(l, _) in &locs {
            let page = dec(l) >> 12;
            for p in [page, page.wrapping_sub(1), page.wrapping_sub(2)] {
                let (w, b) = bloom_slot(p as u64);
                pages[w] |= b;
            }
        }
        let (parts, _) = scan_parallel::<Vec<Node>>(regions, workers, 0, |address, bytes, limit, out| {
            // SAFETY: the buffer is page aligned; `bytes` is a prefix of it.
            let q = unsafe { std::slice::from_raw_parts(bytes.as_ptr() as *const u64, bytes.len() / 8) };
            for (j, &v) in q[..limit / 8].iter().enumerate() {
                let v = v as usize;
                if v.saturating_add(WINDOW) < min_l || v > max_l {
                    continue;
                }
                let (w, b) = bloom_slot(v as u64 >> 12);
                if pages[w] & b == 0 {
                    continue;
                }
                let e = enc(v);
                let mut at = locs.partition_point(|l| l.0 < e);
                let mut taken = 0;
                while at < locs.len() && locs[at].0 < e + WINDOW && taken < 4 {
                    let parent = &nodes[locs[at].1 as usize];
                    out.push(Node {
                        loc: enc(address + j * 8),
                        parent: locs[at].1,
                        delta: (dec(locs[at].0) - v) as u32,
                        group: parent.group,
                        k: parent.k,
                    });
                    at += 1;
                    taken += 1;
                }
            }
        });
        let from = nodes.len();
        let mut capped = false;
        for n in parts.into_iter().flatten() {
            if nodes.len() - from >= NODE_CAP {
                capped = true;
                break;
            }
            nodes.push(n);
        }
        level_ranges.push((from, nodes.len()));
        let roots = (from..nodes.len()).filter(|&i| in_image(nodes[i].loc)).count();
        let _ = writeln!(
            report,
            "pointer level {level}: {} qwords point into a block holding a level {} pointer less than {WINDOW:#x} bytes in{}; {roots} of them in the title's image ({:.0} s so far)",
            nodes.len() - from,
            level - 1,
            if capped { " (capped)" } else { "" },
            started.elapsed().as_secs_f32()
        );
        frontier = (from..nodes.len()).filter(|&i| !in_image(nodes[i].loc)).map(|i| i as u32).collect();
    }

    // Image-rooted paths, shortest first, per group.
    let mut text = String::new();
    let mut counts = vec![0usize; known.groups.len()];
    for (range_level, &(lo, hi)) in level_ranges.iter().enumerate() {
        for i in lo..hi {
            let n = &nodes[i];
            if !in_image(n.loc) {
                continue;
            }
            let g = usize::from(n.group);
            if counts[g] >= 60 {
                continue;
            }
            counts[g] += 1;
            let mut deltas = Vec::new();
            let mut at = n;
            while at.parent != NO_PARENT {
                deltas.push(at.delta as usize);
                at = &nodes[at.parent as usize];
            }
            let rva = dec(n.loc) - config.base;
            let section = config
                .sections
                .iter()
                .find(|(_, r, s)| rva >= *r && rva < r + s)
                .map_or("", |(name, _, _)| name.as_str());
            let _ = writeln!(
                text,
                "group {g} level {} rva={rva:#x} ({section}) deltas={:x?} struct start = matrix - {:#x}",
                range_level + 1,
                deltas,
                16 * usize::from(n.k)
            );
            known.groups[g].paths.push(player::Path { rva, deltas, adjust: 16 * usize::from(n.k) });
        }
    }
    // What the struct start's first qword looks like (a vtable pointer is in the image).
    for (g, group) in known.groups.iter().enumerate() {
        if !only.contains(&g) {
            continue;
        }
        let matrix = group.fixed.map_or(0, dec);
        let _ = writeln!(text, "group {g}: matrix {matrix:#x} {}", group.layout.name());
        for k in 0..=64usize {
            let Some(start) = matrix.checked_sub(16 * k) else { break };
            if let Some(first) = mem::read_u64(start) {
                let rva = (first as usize).wrapping_sub(config.base);
                if rva < config.size {
                    let section = config.sections.iter().find(|(_, r, s)| rva >= *r && rva < r + s).map_or("", |(n, _, _)| n.as_str());
                    let _ = writeln!(text, "  start candidate -{:#x}: first qword is title+{rva:#x} {section}", 16 * k);
                }
            }
        }
    }
    write(dir, "pointers.txt", &text)?;
    let _ = writeln!(report, "image-rooted paths per group: {counts:?} ({:.1} s)", started.elapsed().as_secs_f32());
    Ok(report)
}
