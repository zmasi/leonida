//! Diagnostics only (`LEONIDA_NATIVE_SCAN`): looks, at run time, for the tables
//! the title's script engine calls its commands through.
//!
//! A RAGE-style engine resolves each script's native table from hashes to
//! handler addresses once the script is loaded, so the handlers (all inside
//! the title's executable section) sit in arrays in memory the title builds
//! itself, even though no such table exists in the file (`docs/mod-menu.md`).
//! This only *observes* data the title has written to its own memory: it reads
//! committed memory (through `ReadProcessMemory` on the own process, so a page
//! that goes away under the scan fails the read instead of the process) and
//! writes nothing to the title.
//!
//! `LEONIDA_NATIVE_SCAN=<s>[,<s>...]` scans that many seconds after the title
//! starts (once per number). Tuning, all optional:
//!
//! - `LEONIDA_NATIVE_MIN_RUN` (default 64): fewest array elements in a run.
//! - `LEONIDA_NATIVE_MIN_DISTINCT` (default 200): fewest distinct handlers for
//!   a run to be listed and to enter the union.
//! - `LEONIDA_NATIVE_ALL=1`: also scan read-only and image-backed memory (the
//!   default is private/mapped memory a program can write to).
//! - `LEONIDA_NATIVE_LOOSE=1`: a pointer into code counts even when the target
//!   is not a function start (the default needs a `.pdata` function start, or
//!   a 16-aligned byte after `int3` padding).
//! - `LEONIDA_NATIVE_U32=1`: also looks for arrays of 32-bit offsets that are
//!   handler addresses relative to some base `B` (see `OffsetArray`).
//!
//! Output goes under `%TEMP%\leonida-natives\scan-<s>s\` (addresses of the
//! title: never commit them): `summary.txt`, `candidates.txt`, `targets.txt`
//! (handler RVAs per candidate) and `union.txt` (one handler RVA per line).

use std::collections::HashMap;
use std::fmt::Write as _;
use std::path::PathBuf;
use std::sync::OnceLock;
use std::time::Duration;

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
    fn GetCurrentProcess() -> isize;
    fn ReadProcessMemory(
        process: isize,
        address: usize,
        buffer: *mut u8,
        size: usize,
        read: *mut usize,
    ) -> i32;
}

const MEM_COMMIT: u32 = 0x1000;
const MEM_RESERVE: u32 = 0x2000;
const MEM_IMAGE: u32 = 0x100_0000;
const PAGE_READWRITE: u32 = 0x04;
const PAGE_READONLY: u32 = 0x02;
const PAGE_WRITECOPY: u32 = 0x08;
const PAGE_EXECUTE_READ: u32 = 0x20;
const PAGE_EXECUTE_READWRITE: u32 = 0x40;
const PAGE_EXECUTE_WRITECOPY: u32 = 0x80;
const PAGE_GUARD: u32 = 0x100;
const CHUNK: usize = 4 << 20;
const MAX_STRIDE: usize = 8;
/// Candidates listed in full (the best by distinct handlers).
const LIST_MAX: usize = 200;

/// Starts the scan thread when `LEONIDA_NATIVE_SCAN` is set.
pub fn start(base: usize, info: &leonida_pe::ImageInfo) {
    let Ok(list) = std::env::var("LEONIDA_NATIVE_SCAN") else {
        return;
    };
    let delays: Vec<u64> = list
        .split(',')
        .filter_map(|s| s.trim().parse().ok())
        .collect();
    if delays.is_empty() {
        eprintln!("Leonida: LEONIDA_NATIVE_SCAN={list}: expected seconds, e.g. 240 or 120,300");
        return;
    }
    let env_num = |name: &str, default: usize| {
        std::env::var(name)
            .ok()
            .and_then(|v| v.trim().parse().ok())
            .unwrap_or(default)
    };
    let flag = |name: &str| std::env::var(name).is_ok_and(|v| v == "1");
    let config = Config {
        base,
        size: info.size_of_image as usize,
        exec: info
            .sections
            .iter()
            .filter(|s| s.characteristics & 0x2000_0000 != 0)
            .map(|s| (s.rva as usize, s.rva as usize + s.virtual_size as usize))
            .collect(),
        writable_image: info
            .sections
            .iter()
            .filter(|s| s.characteristics & 0x8000_0000 != 0)
            .map(|s| (s.rva as usize, s.rva as usize + s.virtual_size as usize))
            .collect(),
        min_run: env_num("LEONIDA_NATIVE_MIN_RUN", 32).max(8),
        min_distinct: env_num("LEONIDA_NATIVE_MIN_DISTINCT", 64),
        all: flag("LEONIDA_NATIVE_ALL"),
        loose: flag("LEONIDA_NATIVE_LOOSE"),
        u32_mode: flag("LEONIDA_NATIVE_U32"),
        sections: info
            .sections
            .iter()
            .map(|s| (s.name.clone(), s.rva as usize, s.virtual_size as usize))
            .collect(),
        starts: OnceLock::new(),
    };
    eprintln!(
        "Leonida: native scan at {delays:?} s; executable ranges (rva) {:x?}; output {}",
        config.exec,
        out_root().display()
    );
    std::thread::Builder::new()
        .name("leonida-native-scan".into())
        .spawn(move || {
            let mut waited = 0;
            for delay in delays {
                std::thread::sleep(Duration::from_secs(delay.saturating_sub(waited)));
                waited = delay;
                let started = std::time::Instant::now();
                let dir = out_root().join(format!("scan-{delay}s"));
                match scan(&config, &dir) {
                    Ok(headline) => eprintln!(
                        "Leonida: native scan {delay}s done in {:.1}s: {headline}; details in {}",
                        started.elapsed().as_secs_f32(),
                        dir.display()
                    ),
                    Err(e) => eprintln!("Leonida: native scan {delay}s failed: {e}"),
                }
            }
        })
        .ok();
}

fn out_root() -> PathBuf {
    std::env::temp_dir().join("leonida-natives")
}

struct Config {
    base: usize,
    size: usize,
    /// Executable sections, as RVA ranges.
    exec: Vec<(usize, usize)>,
    /// Writable sections of the title's image, as RVA ranges.
    writable_image: Vec<(usize, usize)>,
    min_run: usize,
    min_distinct: usize,
    all: bool,
    /// A pointer into code counts even when it is not a function start.
    loose: bool,
    /// Also look for arrays of 32-bit offsets from a base.
    u32_mode: bool,
    /// (name, rva, size) of every section.
    sections: Vec<(String, usize, usize)>,
    /// Function starts, built when the first scan runs.
    starts: OnceLock<Starts>,
}

/// The function starts of the title's code: a bit per RVA, and the list.
struct Starts {
    bits: Vec<u64>,
    list: Vec<u32>,
    from_pdata: usize,
    from_padding: usize,
}

impl Starts {
    #[inline]
    fn has(&self, rva: u32) -> bool {
        self.bits
            .get(rva as usize >> 6)
            .is_some_and(|w| w >> (rva & 63) & 1 != 0)
    }
}

/// Reads `len` bytes of the own process; `None` when any part is not readable.
fn read_mem(address: usize, len: usize) -> Option<Vec<u8>> {
    let mut data = vec![0u8; len];
    let mut read = 0usize;
    // SAFETY: a read of the own process into an owned buffer; failure is a return value.
    let ok = unsafe { ReadProcessMemory(GetCurrentProcess(), address, data.as_mut_ptr(), len, &mut read) };
    (ok != 0 && read == len).then_some(data)
}

/// `.pdata`'s function begins, plus 16-aligned bytes after `int3` padding (a
/// leaf function has no unwind entry).
fn build_starts(config: &Config) -> Starts {
    let mut bits = vec![0u64; (config.size >> 6) + 1];
    let mut list = Vec::new();
    let set = |rva: u32, bits: &mut Vec<u64>, list: &mut Vec<u32>| -> bool {
        let w = &mut bits[rva as usize >> 6];
        let bit = 1u64 << (rva & 63);
        let fresh = *w & bit == 0;
        *w |= bit;
        if fresh {
            list.push(rva);
        }
        fresh
    };
    let mut from_pdata = 0;
    for (name, rva, size) in &config.sections {
        if name != ".pdata" {
            continue;
        }
        let mut at = 0;
        while at < *size {
            let n = (4 << 20).min(*size - at) / 12 * 12;
            let Some(data) = (n > 0).then(|| read_mem(config.base + rva + at, n)).flatten() else {
                break;
            };
            for entry in data.chunks_exact(12) {
                let begin = u32::from_le_bytes(entry[..4].try_into().unwrap());
                if begin != 0 && (begin as usize) < config.size && set(begin, &mut bits, &mut list) {
                    from_pdata += 1;
                }
            }
            at += n;
        }
    }
    let mut from_padding = 0;
    for &(lo, hi) in &config.exec {
        let mut at = lo;
        let mut previous = 0u8;
        while at < hi {
            let n = (8 << 20).min(hi - at);
            let Some(code) = read_mem(config.base + at, n) else {
                break;
            };
            for (k, &b) in code.iter().enumerate() {
                let rva = at + k;
                if rva % 16 == 0 && previous == 0xCC && b != 0xCC && set(rva as u32, &mut bits, &mut list) {
                    from_padding += 1;
                }
                previous = b;
            }
            at += n;
        }
    }
    list.sort_unstable();
    Starts {
        bits,
        list,
        from_pdata,
        from_padding,
    }
}

impl Config {
    /// The RVA of `value` when it points into an executable section, at a
    /// function start (unless `loose`).
    #[inline]
    fn target(&self, value: u64) -> Option<u32> {
        let rva = (value as usize).wrapping_sub(self.base);
        if rva >= self.size {
            return None;
        }
        if !self.exec.iter().any(|&(lo, hi)| rva >= lo && rva < hi) {
            return None;
        }
        let rva = rva as u32;
        (self.loose || self.starts.get().is_none_or(|s| s.has(rva))).then_some(rva)
    }
}

/// One run of array elements that all point into code.
struct Candidate {
    /// First element's address.
    address: usize,
    /// Elements in the run.
    len: usize,
    /// Bytes between elements (8 * stride).
    step: usize,
    /// Sorted distinct handler RVAs.
    targets: Vec<u32>,
    /// What the memory is (see [`Region::tag`]).
    tag: &'static str,
}

#[derive(Clone, Copy, Default)]
struct Run {
    start: usize,
    len: usize,
}

struct Region {
    start: usize,
    size: usize,
    tag: &'static str,
}

/// Committed memory worth scanning, minus the scan's own buffer.
fn regions(config: &Config, skip: (usize, usize)) -> Vec<Region> {
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
        let readable = writable || mbi.protect & (PAGE_READONLY | PAGE_EXECUTE_READ) != 0;
        if mbi.state != MEM_COMMIT || mbi.protect & PAGE_GUARD != 0 || !readable {
            continue;
        }
        let in_title = start >= config.base && start < config.base + config.size;
        let image_backed = mbi.kind == MEM_IMAGE && !in_title;
        if !config.all && (!writable || image_backed) {
            continue;
        }
        let tag = match (in_title, image_backed, writable) {
            (true, _, true) => "title-image-rw",
            (true, _, false) => "title-image-ro",
            (_, true, _) => "dll-image",
            (_, _, true) => "heap-rw",
            _ => "heap-ro",
        };
        // Split around the scan's own buffer so it never finds itself.
        let end = start + size;
        if skip.0 < end && skip.1 > start {
            if skip.0 > start {
                out.push(Region {
                    start,
                    size: skip.0 - start,
                    tag,
                });
            }
            if skip.1 < end {
                out.push(Region {
                    start: skip.1,
                    size: end - skip.1,
                    tag,
                });
            }
        } else {
            out.push(Region { start, size, tag });
        }
    }
    out
}

fn scan(config: &Config, dir: &std::path::Path) -> Result<String, String> {
    std::fs::create_dir_all(dir).map_err(|e| format!("{}: {e}", dir.display()))?;
    // The copy buffer lives outside the allocator, so it can be told apart.
    // SAFETY: a fresh private allocation owned by this function.
    let buffer = unsafe { VirtualAlloc(0, CHUNK, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) };
    if buffer.is_null() {
        return Err("no buffer".into());
    }
    let skip = (buffer as usize, buffer as usize + CHUNK);
    let regions = regions(config, skip);
    // SAFETY: the buffer is CHUNK bytes of read-write memory.
    let chunk = unsafe { std::slice::from_raw_parts_mut(buffer, CHUNK) };

    // Function starts first: a pointer into code only counts at one.
    let starts_started = std::time::Instant::now();
    let starts = config.starts.get_or_init(|| build_starts(config));
    let starts_note = format!(
        "function starts: {} ({} from .pdata, {} after int3 padding), built in {:.1}s",
        starts.list.len(),
        starts.from_pdata,
        starts.from_padding,
        starts_started.elapsed().as_secs_f32()
    );
    let mut sink = Sink::default();
    let mut unreadable = 0usize;
    let mut scanned = 0usize;
    for region in &regions {
        let mut scanner = Scanner::new(config, region.tag);
        let mut offset = 0;
        while offset < region.size {
            let want = CHUNK.min(region.size - offset);
            let mut read = 0usize;
            // SAFETY: reads the own process into the buffer; a failure is a
            // return value, not a fault.
            let ok = unsafe {
                ReadProcessMemory(
                    GetCurrentProcess(),
                    region.start + offset,
                    chunk.as_mut_ptr(),
                    want,
                    &mut read,
                )
            };
            if ok == 0 || read == 0 {
                unreadable += want;
                scanner.finish_all(&mut sink);
                offset += want;
                continue;
            }
            scanned += read;
            scanner.feed(region.start + offset, &chunk[..read], &mut sink);
            offset += want;
        }
        scanner.finish_all(&mut sink);
    }

    let Sink {
        found,
        hist: by_distinct,
        offsets,
        runs_32,
        checked,
        ..
    } = sink;
    // A stride-1 run holds every longer-stride run inside it.
    let spans: Vec<(usize, usize, usize)> = found
        .iter()
        .map(|c| (c.address, c.address + c.len * c.step, c.step))
        .collect();
    let mut listed: Vec<usize> = (0..found.len())
        .filter(|&i| found[i].targets.len() >= config.min_distinct)
        .filter(|&i| {
            let (lo, hi, step) = spans[i];
            !spans
                .iter()
                .enumerate()
                .any(|(j, &(l, h, s))| j != i && s < step && l <= lo && h >= hi)
        })
        .collect();
    listed.sort_by_key(|&i| std::cmp::Reverse(found[i].targets.len()));
    listed.truncate(LIST_MAX);

    // The union of handlers over the listed candidates.
    let mut union: Vec<u32> = listed
        .iter()
        .flat_map(|&i| found[i].targets.iter().copied())
        .collect();
    union.sort_unstable();
    union.dedup();

    let mut candidates = String::new();
    let mut targets = String::new();
    for (n, &i) in listed.iter().enumerate() {
        let c = &found[i];
        let covered = intersect(&c.targets, &union);
        let _ = write!(
            candidates,
            "#{n} addr={:#x} (rva {:#x}) {} len={} step={} qword distinct={} cover_of_union={:.1}%",
            c.address,
            c.address.wrapping_sub(config.base),
            c.tag,
            c.len,
            c.step,
            c.targets.len(),
            100.0 * covered as f64 / union.len().max(1) as f64
        );
        let _ = write!(candidates, " | {}", slot_stats(c));
        candidates.push('\n');
        let _ = write!(targets, "#{n}:");
        for t in &c.targets {
            let _ = write!(targets, " {t:x}");
        }
        targets.push('\n');
    }
    // Overlaps between listed candidates (handler sets sharing at least half
    // of the smaller one).
    candidates.push_str("\noverlaps (shared handlers; pairs sharing at least half of the smaller set):\n");
    for (a, &i) in listed.iter().enumerate() {
        for (b, &j) in listed.iter().enumerate().skip(a + 1) {
            let shared = intersect(&found[i].targets, &found[j].targets);
            let smaller = found[i].targets.len().min(found[j].targets.len());
            if shared * 2 >= smaller && shared > 0 {
                let _ = writeln!(candidates, "#{a} & #{b}: {shared} shared of {smaller}");
            }
        }
    }

    // Where the union's handlers sit: 1 MiB buckets of RVA.
    let mut histogram: std::collections::BTreeMap<u32, usize> = Default::default();
    for &t in &union {
        *histogram.entry(t >> 20).or_default() += 1;
    }
    let mut summary = String::new();
    let _ = writeln!(summary, "title base {:#x}, image size {:#x}", config.base, config.size);
    let _ = writeln!(summary, "executable rva ranges: {:x?}", config.exec);
    let _ = writeln!(summary, "title writable section rva ranges: {:x?}", config.writable_image);
    let _ = writeln!(
        summary,
        "min run {}, min distinct {}, all memory {}",
        config.min_run, config.min_distinct, config.all
    );
    let _ = writeln!(
        summary,
        "regions {}, {} MiB scanned, {} MiB unreadable",
        regions.len(),
        scanned >> 20,
        unreadable >> 20
    );
    let mut by_tag: HashMap<&str, (usize, usize)> = HashMap::new();
    for r in &regions {
        let e = by_tag.entry(r.tag).or_default();
        e.0 += 1;
        e.1 += r.size;
    }
    for (tag, (n, bytes)) in &by_tag {
        let _ = writeln!(summary, "  {tag}: {n} regions, {} MiB", bytes >> 20);
    }
    let _ = writeln!(summary, "runs of >= {} handler pointers: {}", config.min_run, found.len());
    let labels = ["<16", "<64", "<100", "<200", "<500", "<1000", ">=1000"];
    for (label, n) in labels.iter().zip(by_distinct) {
        let _ = writeln!(summary, "  runs with {label} distinct handlers: {n}");
    }
    let _ = writeln!(
        summary,
        "listed candidates (>= {} distinct, not inside a denser run): {}",
        config.min_distinct,
        listed.len()
    );
    let _ = writeln!(summary, "union of their handlers: {} distinct", union.len());
    let _ = writeln!(summary, "union by 1 MiB of rva (bucket start: handlers):");
    for (bucket, n) in &histogram {
        let _ = writeln!(summary, "  {:#x}: {n}", bucket << 20);
    }
    let _ = writeln!(summary, "{starts_note}");
    let mut offsets_text = String::new();
    let mut offset_handlers = String::new();
    let mut offset_note = String::new();
    if config.u32_mode {
        // Per base: arrays, handlers (union), and where a qword of the title's
        // writable sections holds that base as an absolute address.
        let mut by_base: std::collections::BTreeMap<i64, Vec<usize>> = Default::default();
        for (i, a) in offsets.iter().enumerate() {
            by_base.entry(a.base_rva).or_default().push(i);
        }
        let mut bases: Vec<(i64, Vec<usize>, Vec<u32>)> = by_base
            .into_iter()
            .map(|(base, idx)| {
                let mut all: Vec<u32> = idx.iter().flat_map(|&i| offsets[i].handlers.iter().copied()).collect();
                all.sort_unstable();
                all.dedup();
                (base, idx, all)
            })
            .collect();
        bases.sort_by_key(|(_, _, all)| std::cmp::Reverse(all.len()));
        let top: Vec<u64> = bases
            .iter()
            .take(20)
            .map(|(base, _, _)| (config.base as i64 + base) as u64)
            .collect();
        let slots = find_slots(config, &top);
        let _ = writeln!(
            summary,
            "32-bit offset mode: {} equal-nibble runs of >= {OFFSET_MIN_RUN}, {} distinct checked against the function starts, {} arrays accepted, {} distinct bases",
            runs_32,
            checked,
            offsets.len(),
            bases.len()
        );
        let _ = writeln!(summary, "bases (rva of the base; arrays; distinct handlers; qword slots holding it, as rva of the slot):");
        for (base, idx, all) in bases.iter().take(40) {
            let abs = (config.base as i64 + base) as u64;
            let _ = writeln!(
                summary,
                "  base rva {base:#x} (abs {abs:#x}): {} arrays, {} handlers, slots {:x?}",
                idx.len(),
                all.len(),
                slots.get(&abs).map(Vec::as_slice).unwrap_or(&[])
            );
        }
        let mut sorted: Vec<&OffsetArray> = offsets.iter().collect();
        sorted.sort_by_key(|a| std::cmp::Reverse(a.handlers.len()));
        for a in sorted.iter().take(2000) {
            let _ = writeln!(
                offsets_text,
                "addr={:#x} (rva {:#x}) {} len={} base_rva={:#x} hits={} handlers={}",
                a.address,
                a.address.wrapping_sub(config.base),
                a.tag,
                a.len,
                a.base_rva,
                a.hits,
                a.handlers.len()
            );
        }
        for (base, _, all) in bases.iter().take(40) {
            let _ = write!(offset_handlers, "base {base:#x}:");
            for h in all {
                let _ = write!(offset_handlers, " {h:x}");
            }
            offset_handlers.push('\n');
        }
        let _ = write!(
            offset_note,
            ", {} offset arrays on {} bases",
            offsets.len(),
            bases.len()
        );
    }
    let headline = format!(
        "{} MiB scanned, {} runs, {} candidates with >= {} handlers, union {} handlers{offset_note}",
        scanned >> 20,
        found.len(),
        listed.len(),
        config.min_distinct,
        union.len()
    );
    let _ = writeln!(summary, "\n{headline}");

    let mut union_text = String::new();
    for t in &union {
        let _ = writeln!(union_text, "{t:x}");
    }
    for (name, text) in [
        ("summary.txt", &summary),
        ("candidates.txt", &candidates),
        ("targets.txt", &targets),
        ("union.txt", &union_text),
        ("offset-arrays.txt", &offsets_text),
        ("offset-handlers.txt", &offset_handlers),
    ] {
        if text.is_empty() && !matches!(name, "summary.txt" | "candidates.txt") {
            continue;
        }
        std::fs::write(dir.join(name), text).map_err(|e| format!("{name}: {e}"))?;
    }
    Ok(headline)
}

/// Slots (RVAs) in the title's writable sections that hold one of `values` as a qword.
fn find_slots(config: &Config, values: &[u64]) -> HashMap<u64, Vec<u32>> {
    let mut out: HashMap<u64, Vec<u32>> = HashMap::new();
    for &(lo, hi) in &config.writable_image {
        let mut at = lo;
        while at < hi.min(config.size) {
            let n = (8 << 20).min(hi.min(config.size) - at);
            if let Some(data) = read_mem(config.base + at, n) {
                for (k, raw) in data.chunks_exact(8).enumerate() {
                    let v = u64::from_le_bytes(raw.try_into().unwrap());
                    if values.contains(&v) {
                        let slots = out.entry(v).or_default();
                        if slots.len() < 8 {
                            slots.push((at + k * 8) as u32);
                        }
                    }
                }
            }
            at += n;
        }
    }
    out
}

/// How many values the two sorted lists share.
fn intersect(a: &[u32], b: &[u32]) -> usize {
    let (mut i, mut j, mut n) = (0, 0, 0);
    while i < a.len() && j < b.len() {
        match a[i].cmp(&b[j]) {
            std::cmp::Ordering::Less => i += 1,
            std::cmp::Ordering::Greater => j += 1,
            std::cmp::Ordering::Equal => {
                n += 1;
                i += 1;
                j += 1;
            }
        }
    }
    n
}

/// For a strided run: per slot of the element, how many distinct values and
/// how many look like a 64-bit hash (both halves non-zero, not a small
/// number). The slot holding the handlers shows as `code`.
fn slot_stats(c: &Candidate) -> String {
    let stride = c.step / 8;
    if stride <= 1 {
        return "stride 1".into();
    }
    let span = (c.len * c.step).min(8 << 20);
    let mut data = vec![0u8; span];
    let mut read = 0usize;
    // SAFETY: the same own-process read as the scan.
    let ok = unsafe {
        ReadProcessMemory(GetCurrentProcess(), c.address, data.as_mut_ptr(), span, &mut read)
    };
    if ok == 0 {
        return "slots unreadable".into();
    }
    let qwords: Vec<u64> = data[..read]
        .chunks_exact(8)
        .map(|b| u64::from_le_bytes(b.try_into().unwrap()))
        .collect();
    let mut out = format!("stride {stride}:");
    for slot in 0..stride {
        let values: Vec<u64> = qwords.iter().skip(slot).step_by(stride).copied().collect();
        let mut distinct = values.clone();
        distinct.sort_unstable();
        distinct.dedup();
        let hashlike = values
            .iter()
            .filter(|&&v| (v >> 32) != 0 && (v & 0xFFFF_FFFF) != 0 && (v >> 47) == 0)
            .count();
        let _ = write!(
            out,
            " [slot {slot}: {} distinct of {}, {} hash-like]",
            distinct.len(),
            values.len(),
            hashlike
        );
    }
    out
}

/// Runs of code pointers, found over a stream of qwords (strides 1 to 8
/// elements, so (key, handler) pairs and wider buckets show) and of u32s.
struct Scanner<'a> {
    config: &'a Config,
    tag: &'static str,
    /// [stride - 1][phase]
    runs: [[Run; MAX_STRIDE]; MAX_STRIDE],
    rvas: Vec<Vec<u32>>,
    /// The open run of 32-bit values that share their low four bits.
    off_start: usize,
    off_nibble: u32,
    off_vals: Vec<u32>,
}

impl<'a> Scanner<'a> {
    fn new(config: &'a Config, tag: &'static str) -> Self {
        Scanner {
            config,
            tag,
            runs: [[Run::default(); MAX_STRIDE]; MAX_STRIDE],
            rvas: vec![Vec::new(); MAX_STRIDE * MAX_STRIDE],
            off_start: 0,
            off_nibble: 0,
            off_vals: Vec::new(),
        }
    }

    /// Consumes `bytes` at `address`; chunks of a region arrive in order and
    /// every chunk is a multiple of 8 bytes, so no qword straddles two.
    fn feed(&mut self, address: usize, bytes: &[u8], sink: &mut Sink) {
        let base_q = address / 8;
        for (k, raw) in bytes.chunks_exact(8).enumerate() {
            let value = u64::from_le_bytes(raw.try_into().unwrap());
            let q = base_q + k;
            let hit = self.config.target(value);
            for stride in 1..=MAX_STRIDE {
                let slot = (stride - 1) * MAX_STRIDE + q % stride;
                let run = &mut self.runs[stride - 1][q % stride];
                match hit {
                    Some(rva) => {
                        if run.len == 0 {
                            run.start = q;
                        }
                        run.len += 1;
                        self.rvas[slot].push(rva);
                    }
                    None if run.len > 0 => {
                        let done = *run;
                        *run = Run::default();
                        let rvas = std::mem::take(&mut self.rvas[slot]);
                        emit(self.config, self.tag, stride * 8, done, rvas, sink);
                    }
                    None => {}
                }
            }
            if self.config.u32_mode {
                for (half, word) in raw.chunks_exact(4).enumerate() {
                    let w = u32::from_le_bytes(word.try_into().unwrap());
                    self.offset_step(q * 2 + half, w, sink);
                }
            }
        }
    }

    /// Offsets to functions that are 16-aligned differ from the base in their
    /// low four bits alike, so an array of them is a run of non-zero values
    /// with the same low nibble. Every such run is then checked against the
    /// function starts ([`verify_offsets`]).
    fn offset_step(&mut self, at: usize, w: u32, sink: &mut Sink) {
        if w != 0 && (self.off_vals.is_empty() || w & 15 == self.off_nibble) {
            if self.off_vals.is_empty() {
                self.off_start = at;
                self.off_nibble = w & 15;
            }
            self.off_vals.push(w);
            return;
        }
        self.offset_end(sink);
        if w != 0 {
            self.off_start = at;
            self.off_nibble = w & 15;
            self.off_vals.push(w);
        }
    }

    fn offset_end(&mut self, sink: &mut Sink) {
        if self.off_vals.len() >= OFFSET_MIN_RUN {
            check_offsets(self.config, self.tag, self.off_start * 4, &self.off_vals, sink);
        }
        self.off_vals.clear();
    }

    /// The region (or its readable part) ended: close every open run.
    fn finish_all(&mut self, sink: &mut Sink) {
        for stride in 1..=MAX_STRIDE {
            for phase in 0..stride {
                let run = std::mem::take(&mut self.runs[stride - 1][phase]);
                let rvas = std::mem::take(&mut self.rvas[(stride - 1) * MAX_STRIDE + phase]);
                if run.len > 0 {
                    emit(self.config, self.tag, stride * 8, run, rvas, sink);
                }
            }
        }
        self.offset_end(sink);
    }
}

/// Shortest run of equal-nibble 32-bit values that is checked.
const OFFSET_MIN_RUN: usize = 12;
/// Offset runs checked, at most (each costs a pass over the function starts).
const OFFSET_CHECK_MAX: usize = 150_000;

/// What the scan collects.
#[derive(Default)]
struct Sink {
    found: Vec<Candidate>,
    /// Qword runs by how many distinct handlers they hold, for context.
    hist: [usize; 7],
    offsets: Vec<OffsetArray>,
    /// Hashes of the offset runs already checked (copies of a table).
    seen: std::collections::HashSet<u64>,
    checked: usize,
    runs_32: usize,
}

/// An array of 32-bit values that, added to `base_rva` (an RVA of the title,
/// possibly negative), land on function starts.
struct OffsetArray {
    /// Address of the first element.
    address: usize,
    len: usize,
    /// The base the offsets are relative to, as an RVA of the title image.
    base_rva: i64,
    hits: usize,
    tag: &'static str,
    /// The handlers: `base_rva + offset`, sorted, distinct.
    handlers: Vec<u32>,
}

fn check_offsets(config: &Config, tag: &'static str, address: usize, vals: &[u32], sink: &mut Sink) {
    sink.runs_32 += 1;
    let Some(starts) = config.starts.get() else {
        return;
    };
    // The same table copied around is checked once.
    let mut hash = 0xcbf2_9ce4_8422_2325u64;
    for &v in vals.iter().take(64) {
        hash = (hash ^ u64::from(v)).wrapping_mul(0x100_0000_01b3);
    }
    hash ^= vals.len() as u64;
    if !sink.seen.insert(hash) || sink.checked >= OFFSET_CHECK_MAX {
        return;
    }
    sink.checked += 1;
    let Some((base_rva, hits)) = verify_offsets(config, starts, vals) else {
        return;
    };
    let mut handlers: Vec<u32> = vals
        .iter()
        .filter_map(|&v| {
            let rva = base_rva + i64::from(v);
            (0..config.size as i64).contains(&rva).then_some(rva as u32)
        })
        .filter(|&r| starts.has(r))
        .collect();
    handlers.sort_unstable();
    handlers.dedup();
    sink.offsets.push(OffsetArray {
        address,
        len: vals.len(),
        base_rva,
        hits,
        tag,
        handlers,
    });
}

/// The base that makes most of `vals` land on function starts, when at least
/// 90 % of them do: tried from every function start as the target of one of
/// the first two values.
fn verify_offsets(config: &Config, starts: &Starts, vals: &[u32]) -> Option<(i64, usize)> {
    let at = |base: i64, v: u32| {
        let rva = base + i64::from(v);
        (0..config.size as i64).contains(&rva) && starts.has(rva as u32)
    };
    let probe = [vals.len() / 4, vals.len() / 2, vals.len() - 1];
    for anchor in 0..2 {
        for &f in &starts.list {
            let base = i64::from(f) - i64::from(vals[anchor]);
            if !at(base, vals[probe[0]]) || !at(base, vals[probe[1]]) || !at(base, vals[probe[2]]) {
                continue;
            }
            let hits = vals.iter().filter(|&&v| at(base, v)).count();
            if hits * 10 >= vals.len() * 9 {
                return Some((base, hits));
            }
        }
    }
    None
}

/// Records a finished run when it is long enough. `run.start` counts qwords
/// from address 0.
fn emit(config: &Config, tag: &'static str, step: usize, run: Run, mut rvas: Vec<u32>, sink: &mut Sink) {
    if run.len < config.min_run {
        return;
    }
    rvas.sort_unstable();
    rvas.dedup();
    let bucket = match rvas.len() {
        0..16 => 0,
        16..64 => 1,
        64..100 => 2,
        100..200 => 3,
        200..500 => 4,
        500..1000 => 5,
        _ => 6,
    };
    sink.hist[bucket] += 1;
    // Only runs that could be listed keep their handler sets.
    if rvas.len() < config.min_distinct {
        return;
    }
    sink.found.push(Candidate {
        address: run.start * 8,
        len: run.len,
        step,
        targets: rvas,
        tag,
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    fn config(min_run: usize) -> Config {
        Config {
            base: 0x1_4000_0000,
            size: 0x10_0000,
            exec: vec![(0x1000, 0x8_0000)],
            writable_image: vec![],
            min_run,
            min_distinct: 8,
            all: false,
            loose: true,
            u32_mode: true,
            sections: vec![],
            starts: OnceLock::new(),
        }
    }

    fn run(config: &Config, qwords: &[u64]) -> Vec<Candidate> {
        let bytes: Vec<u8> = qwords.iter().flat_map(|q| q.to_le_bytes()).collect();
        let mut sink = Sink::default();
        let mut scanner = Scanner::new(config, "test");
        scanner.feed(0x10_0000, &bytes, &mut sink);
        scanner.finish_all(&mut sink);
        sink.found
    }

    #[test]
    fn finds_a_dense_array_and_a_key_handler_table() {
        let c = config(16);
        let code = |i: u64| 0x1_4000_0000 + 0x1000 + i * 0x10;
        // 40 consecutive handlers, then a gap, then 40 (hash, handler) pairs.
        let mut q: Vec<u64> = (0..40).map(code).collect();
        q.push(7);
        for i in 0..40 {
            q.push(0x1234_5678_9ABC_DEF0 ^ i);
            q.push(code(100 + i));
        }
        let found = run(&c, &q);
        let dense: Vec<_> = found.iter().filter(|f| f.step == 8 && f.len == 40).collect();
        assert_eq!(dense.len(), 1);
        assert_eq!(dense[0].targets.len(), 40);
        assert_eq!(dense[0].address, 0x10_0000);
        let pairs: Vec<_> = found.iter().filter(|f| f.step == 16 && f.len == 40).collect();
        assert_eq!(pairs.len(), 1);
        assert_eq!(pairs[0].len, 40);
        assert_eq!(pairs[0].address, 0x10_0000 + 41 * 8 + 8);
    }

    #[test]
    fn ignores_short_runs_and_pointers_outside_code() {
        let c = config(16);
        let mut q: Vec<u64> = (0..8).map(|i| 0x1_4000_1000 + i * 8).collect();
        q.extend((0..64).map(|i| 0x1_4000_0000 + 0x9_0000 + i * 8));
        assert!(run(&c, &q).is_empty());
    }
}

#[cfg(test)]
mod start_tests {
    use super::*;

    /// A config whose function starts are every 0x40 from 0x1000 to 0x3000.
    fn with_starts() -> Config {
        let config = Config {
            base: 0x1_4000_0000,
            size: 0x10_0000,
            exec: vec![(0x1000, 0x8_0000)],
            writable_image: vec![],
            min_run: 8,
            min_distinct: 8,
            all: false,
            loose: false,
            u32_mode: true,
            sections: vec![],
            starts: OnceLock::new(),
        };
        let mut bits = vec![0u64; (config.size >> 6) + 1];
        let mut list = Vec::new();
        for rva in (0x1000u32..0x3000).step_by(0x40) {
            bits[rva as usize >> 6] |= 1 << (rva & 63);
            list.push(rva);
        }
        let _ = config.starts.set(Starts {
            bits,
            list,
            from_pdata: 0,
            from_padding: 0,
        });
        config
    }

    #[test]
    fn a_pointer_into_code_must_be_a_function_start() {
        let c = with_starts();
        assert_eq!(c.target(0x1_4000_1040), Some(0x1040));
        assert_eq!(c.target(0x1_4000_1044), None);
        assert_eq!(c.target(0x1_4000_9000), None);
    }

    #[test]
    fn finds_offsets_from_an_unknown_base() {
        let c = with_starts();
        // Offsets of 40 functions from a base that is not the image base.
        let base: i64 = 0x10_0000 - 0x8_0000 + 0x20;
        let _ = base;
        let b: i64 = -0x500 + 0x30;
        let vals: Vec<u32> = (0..40).map(|i| (0x1000 + 0x40 * (i * 3 % 40) - b) as u32).collect();
        let starts = c.starts.get().unwrap();
        let (found, hits) = verify_offsets(&c, starts, &vals).unwrap();
        assert_eq!(found, b);
        assert_eq!(hits, 40);
        // Offsets that are not function starts for any base.
        let noise: Vec<u32> = (0..40u32).map(|i| 0x10 + i * i * 0x30 + (i % 7) * 0x10).collect();
        assert!(verify_offsets(&c, starts, &noise).is_none());
    }
}
