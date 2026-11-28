//! Renderer port #2: the title's writes of descriptor records into its
//! shader-visible heap say which slots they wrote, and the per-frame look at
//! every changed heap page goes (`LEONIDA_PORT_DESCRIPTORS=1`, off by
//! default; read in [`on`]).
//!
//! Ported from Belobug/VIctory-Belobug-AMD d3cf70dc (`crates/victory-gpu/src/port_descriptors.rs`, GPL-3.0;
//! introduced there in 9997d25, with the memcpy marks of 4cb90be and the disjoint parts of 4ad8527), mapped
//! onto this layer's `descriptor::sync_heaps`: its parts must be apart from one another and are made here by
//! [`plan_parts`], which also coalesces runs of marked slots into fewer, larger reads.
//!
//! What the title does (game_scarlett_master.exe 1.0.2.1; `leonida-loader`'s
//! `descriptor_sites` has the code): its placed shader-visible view heap is
//! the memory of its frame allocator (image +0x4d9f400), which hands out the
//! draws' and dispatches' descriptor tables and much else (constants, list
//! nodes). The tables are filled one 32-byte record at a time by six stores
//! in two functions (+0x4d9efd0, +0x4eec8e0); views made by our calls
//! (`Create*View`, `CopyDescriptors*`) are mirrored where they are made
//! (`descriptor::mirror`). What the layer did before: the write watch's
//! changed 64 KiB pages of the heap, every slot on them read and compared
//! with the one last seen (~290 pages, ~600,000 slots, 15-16 ms a frame of
//! the heap-sync thread, 5-7 of it asking the watch; the fork's runs port1-3).
//!
//! What is done here: the six stores are patched (`descriptor_sites`) to set
//! a byte per slot and per 128 slots in maps of ours ([`MARKS`]), and our own
//! mirroring marks too; `descriptor::sync_heaps` takes the marked slots
//! ([`take`]) in place of the changed pages, and compares and makes only
//! those, as before.
//!
//! Correctness first, the fallback: once in a few frames the write watch is
//! asked ([`ask_due`], every [`check_every`] frames) and a few of the pages it
//! says were written ([`check_budget`] a frame) are looked at whole, as
//! before; a slot found with a new view record that no mark told of is made
//! (so it is right from then on) and counted, with the place in the title's
//! code that last faulted on its 64 KiB block when the guards saw one
//! ([`faulted`]): a writer the patches miss shows in the log. Records that
//! are not views (the allocator's other data) are made as before and counted
//! apart.
//!
//! The title's `memcpy` and `memmove` imports are wrapped too ([`import`]) and
//! mark the view records they copy into the heap (the fork's fallback found
//! ~250 view records a frame the six stores had not told of, copied that way).
//!
//! Assumption: a slot whose view record the title overwrote with other data
//! (not a view) and no mark told of keeps showing its old view on the host
//! until the fallback looks at its page; nothing reads such a slot as a
//! descriptor (the title wrote no descriptor there).

use std::sync::atomic::{AtomicPtr, AtomicU8, AtomicU64, AtomicUsize, Ordering::{AcqRel, Acquire, Relaxed, Release}};

const SWITCH: &str = "LEONIDA_PORT_DESCRIPTORS";

/// Whether renderer port #2 is on. With `LEONIDA_AB` naming the switch it
/// changes while the title runs (the stores stay patched; only what
/// `sync_heaps` looks at changes).
pub fn on() -> bool {
    #[cfg(all(windows, feature = "diagnostics"))]
    if let Some(on) = crate::ab::pick(SWITCH) {
        return on;
    }
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    // Read once, here: the switch of renderer port #2.
    *ON.get_or_init(|| std::env::var(SWITCH).is_ok_and(|v| v.trim() == "1"))
}

/// Whether the stores are to be patched and the imports wrapped at the start:
/// the switch is set, or `LEONIDA_AB` will change it.
pub fn wanted() -> bool {
    #[cfg(all(windows, feature = "diagnostics"))]
    if crate::ab::bit(SWITCH).is_some() {
        return true;
    }
    on()
}

/// What the patched stores read (`leonida_loader::descriptor_sites`,
/// `MARKS_LEN` bytes): the heap's first byte, its length (0: none), the map
/// of slots, the map of chunks of [`CHUNK`] slots.
#[repr(C)]
pub struct Marks {
    start: AtomicU64,
    len: AtomicU64,
    slots: AtomicPtr<AtomicU8>,
    chunks: AtomicPtr<AtomicU8>,
}

const _: () = assert!(size_of::<Marks>() == 32);

pub static MARKS: Marks = Marks {
    start: AtomicU64::new(0),
    len: AtomicU64::new(0),
    slots: AtomicPtr::new(std::ptr::null_mut()),
    chunks: AtomicPtr::new(std::ptr::null_mut()),
};

/// Slots a byte of the map of chunks stands for (4 KiB of records).
const CHUNK: usize = 128;

/// Where the stores' cells are, for `descriptor_sites::apply`.
pub fn marks_address() -> usize {
    std::ptr::from_ref(&MARKS) as usize
}

/// The title's image (base, size) and how many of its stores were patched.
static IMAGE: [AtomicUsize; 3] = [const { AtomicUsize::new(0) }; 3];

/// Told by the launcher: `patched` of the title's record stores in the image
/// at `base` (`size` bytes) mark their slots.
pub fn hooked(base: usize, size: usize, patched: usize) {
    IMAGE[0].store(base, Relaxed);
    IMAGE[1].store(size, Relaxed);
    IMAGE[2].store(patched, Relaxed);
}

/// The heap whose slots are marked: its slots, and the places in the title's
/// code that last faulted on each of its 64 KiB blocks.
struct Adopted {
    start: u64,
    count: usize,
    slots: &'static [AtomicU8],
    chunks: &'static [AtomicU8],
    writers: &'static [AtomicUsize],
}

static ADOPTED: std::sync::RwLock<Option<Adopted>> = std::sync::RwLock::new(None);

/// A placed shader-visible view heap of `count` records at `start`: the
/// first such is the one whose slots are marked. Whether it is. (Adopted
/// whenever the stores are patched, so that a switch changed live finds the
/// marks already there.)
pub(crate) fn adopt(start: u64, count: u32) -> bool {
    IMAGE[2].load(Relaxed) > 0 && adopt_now(start, count)
}

fn adopt_now(start: u64, count: u32) -> bool {
    let mut adopted = ADOPTED.write().unwrap_or_else(|e| e.into_inner());
    if adopted.is_some() {
        tracing::info!(start = format_args!("{start:#x}"), count, "renderer port 2: another placed view heap, looked at by its pages as before");
        return false;
    }
    let count = count as usize;
    let leak = |n: usize| -> &'static [AtomicU8] { Box::leak((0..n).map(|_| AtomicU8::new(0)).collect()) };
    // (Never freed: a store of the title's may be marking while the heap
    // goes; a few MB at most.)
    let slots = leak(count);
    let chunks = leak(count.div_ceil(CHUNK));
    let writers: &'static [AtomicUsize] = Box::leak((0..(count * 32).div_ceil(1 << 16)).map(|_| AtomicUsize::new(0)).collect());
    MARKS.slots.store(slots.as_ptr().cast_mut(), Release);
    MARKS.chunks.store(chunks.as_ptr().cast_mut(), Release);
    MARKS.start.store(start, Release);
    // Last: a store that sees a length sees the rest.
    MARKS.len.store(count as u64 * 32, Release);
    *adopted = Some(Adopted { start, count, slots, chunks, writers });
    tracing::info!(
        start = format_args!("{start:#x}"),
        count,
        stores = IMAGE[2].load(Relaxed),
        "renderer port 2: the title's view heap's slots are told by its record stores and our calls (LEONIDA_PORT_DESCRIPTORS)"
    );
    true
}

/// The heap at `start` goes: nothing more is marked for it.
pub(crate) fn let_go(start: u64) {
    let mut adopted = ADOPTED.write().unwrap_or_else(|e| e.into_inner());
    if adopted.as_ref().is_some_and(|a| a.start == start) {
        MARKS.len.store(0, Release);
        *adopted = None;
        tracing::info!(start = format_args!("{start:#x}"), "renderer port 2: the marked view heap went");
    }
}

/// Whether the heap at `start` is the one whose slots are marked and the
/// port is on (the page route is taken otherwise).
pub(crate) fn adopted(start: u64) -> bool {
    on() && ADOPTED.read().unwrap_or_else(|e| e.into_inner()).as_ref().is_some_and(|a| a.start == start)
}

/// Marks the slot at `handle` (a record our own calls wrote), if it is in the
/// marked heap.
#[inline]
pub(crate) fn mark(handle: u64) {
    let len = MARKS.len.load(Acquire);
    if len == 0 {
        return;
    }
    let at = handle.wrapping_sub(MARKS.start.load(Relaxed));
    if at >= len {
        return;
    }
    mark_slot((at / 32) as usize);
    MARKED_BY_US.fetch_add(1, Relaxed);
}

/// Marks slot `slot` of the marked heap (in it: checked by the caller).
#[inline]
fn mark_slot(slot: usize) {
    // SAFETY: the maps are leaked, as long as the length says.
    unsafe {
        (*MARKS.slots.load(Relaxed).add(slot)).store(1, Release);
        (*MARKS.chunks.load(Relaxed).add(slot / CHUNK)).store(1, Release);
    }
}

/// Whether slot `index`'s mark is set (told, not yet taken).
fn marked(a: &Adopted, index: u32) -> bool {
    a.slots.get(index as usize).is_some_and(|s| s.load(Acquire) != 0)
}

/// The marked slots of the heap at `start`, each mark cleared, as runs of
/// slots `(first, count)` in order; `None` if it is not the marked heap.
pub(crate) fn take(start: u64) -> Option<Vec<(u32, u32)>> {
    let began = std::time::Instant::now();
    let adopted = ADOPTED.read().unwrap_or_else(|e| e.into_inner());
    let a = adopted.as_ref().filter(|a| a.start == start)?;
    let mut runs: Vec<(u32, u32)> = Vec::new();
    let mut n = 0u64;
    for (c, chunk) in a.chunks.iter().enumerate() {
        if chunk.load(Relaxed) == 0 {
            continue;
        }
        // Cleared before its slots are read: a mark set meanwhile sets it again.
        chunk.swap(0, AcqRel);
        let first = c * CHUNK;
        for (i, slot) in a.slots[first..(first + CHUNK).min(a.count)].iter().enumerate() {
            if slot.load(Relaxed) == 0 || slot.swap(0, AcqRel) == 0 {
                continue;
            }
            let index = (first + i) as u32;
            n += 1;
            match runs.last_mut() {
                Some((at, len)) if *at + *len == index => *len += 1,
                _ => runs.push((index, 1)),
            }
        }
    }
    TAKEN[0].fetch_add(n, Relaxed);
    TAKEN[1].fetch_add(1, Relaxed);
    TAKEN[2].fetch_add(began.elapsed().as_nanos() as u64, Relaxed);
    Some(runs)
}

/// Gaps of at most this many slots between marked runs are read with them: a
/// read of a few kilobytes costs about what one of a record does, and the
/// compare with the records last seen leaves the slots between as they are.
pub(crate) const COALESCE_SLOTS: u64 = 64;

/// The parts of the heap `sync_heaps` looks at, `(from, to, fallback)` in
/// bytes, in order and apart from one another (parts that read the same slots
/// at once would race on the records last seen): the slot runs `runs`
/// (`(first, count)`, in order) with gaps of up to [`COALESCE_SLOTS`] closed,
/// and the fallback's `pages` (each `page_size` bytes from its address),
/// whole. A part that holds a fallback page is one of the fallback's.
/// Slots past `mirrored_count` and bytes outside `[start, end)` are left out.
pub(crate) fn plan_parts(start: u64, end: u64, step: u64, mirrored_count: u32, runs: &[(u32, u32)], pages: &[usize], page_size: usize) -> Vec<(usize, usize, bool)> {
    let mirrored = u64::from(mirrored_count);
    // (first slot, past the last slot, a fallback page)
    let mut slots: Vec<(u64, u64, bool)> = Vec::with_capacity(runs.len() + pages.len());
    for &(first, n) in runs {
        let (first, last) = (u64::from(first), (u64::from(first) + u64::from(n)).min(mirrored));
        if first < last {
            slots.push((first, last, false));
        }
    }
    for &page in pages {
        let (from, to) = ((page as u64).max(start), (page as u64).saturating_add(page_size as u64).min(end));
        if from >= to {
            continue;
        }
        // The slots that begin in the page: one that straddles its end is the next page's.
        let (first, last) = ((from - start).div_ceil(step), ((to - start).div_ceil(step)).min(mirrored));
        if first < last {
            slots.push((first, last, true));
        }
    }
    slots.sort_unstable_by_key(|&(first, last, _)| (first, last));
    let mut merged: Vec<(u64, u64, bool)> = Vec::with_capacity(slots.len());
    for (first, last, check) in slots {
        match merged.last_mut() {
            Some((_, to, was)) if first <= *to + if *was || check { 0 } else { COALESCE_SLOTS } => {
                *to = (*to).max(last);
                *was |= check;
            }
            _ => merged.push((first, last, check)),
        }
    }
    merged
        .into_iter()
        .map(|(first, last, check)| (start as usize + (first * step) as usize, start as usize + (last * step) as usize, check))
        .collect()
}

/// Whether slot `index` is in one of `taken` (runs of slots, in order, apart).
pub(crate) fn was_taken(taken: &[(u32, u32)], index: u32) -> bool {
    let at = taken.partition_point(|&(first, _)| first <= index);
    at > 0 && {
        let (first, n) = taken[at - 1];
        index - first < n
    }
}

/// A write fault of the title's at `at` (from its instruction at `rip`),
/// if in the marked heap: who wrote its block last, for [`missed`].
pub(crate) fn faulted(at: usize, rip: usize) {
    if MARKS.len.load(Relaxed) == 0 {
        return;
    }
    let Ok(adopted) = ADOPTED.try_read() else { return };
    if let Some(a) = adopted.as_ref() {
        let off = (at as u64).wrapping_sub(a.start);
        if let Some(w) = a.writers.get((off >> 16) as usize) {
            w.store(rip, Relaxed);
        }
    }
}

/// Frames between askings of the write watch for the fallback
/// (`LEONIDA_PORT_DESCRIPTORS_EVERY=<frames>`, 8 if not given).
fn check_every() -> u64 {
    static N: std::sync::OnceLock<u64> = std::sync::OnceLock::new();
    *N.get_or_init(|| std::env::var("LEONIDA_PORT_DESCRIPTORS_EVERY").ok().and_then(|v| v.trim().parse().ok()).unwrap_or(8u64).max(1))
}

/// Pages the fallback looks at a frame (`LEONIDA_PORT_DESCRIPTORS_CHECK=<pages>`,
/// 16 if not given; 0: none, and the write watch is not asked at all).
fn check_budget() -> usize {
    static N: std::sync::OnceLock<usize> = std::sync::OnceLock::new();
    *N.get_or_init(|| std::env::var("LEONIDA_PORT_DESCRIPTORS_CHECK").ok().and_then(|v| v.trim().parse().ok()).unwrap_or(16usize))
}

/// The fallback's pages: those the watch said were written, not looked at yet;
/// the frame it last asked in, and the frame it last looked in.
struct Fallback {
    pages: std::collections::BTreeSet<usize>,
    asked: u64,
    looked: u64,
    /// The page it goes on from (round the heap).
    next: usize,
}

static FALLBACK: std::sync::Mutex<Fallback> = std::sync::Mutex::new(Fallback { pages: std::collections::BTreeSet::new(), asked: 0, looked: 0, next: 0 });

/// Whether to ask the write watch of the marked heap this frame.
pub(crate) fn ask_due(frame: u64) -> bool {
    let mut f = FALLBACK.lock().unwrap_or_else(|e| e.into_inner());
    if check_budget() == 0 || f.asked != 0 && frame < f.asked + check_every() {
        return false;
    }
    f.asked = frame.max(1);
    true
}

/// What the watch said was written (byte ranges), for later.
pub(crate) fn asked(ranges: &[(usize, usize)]) {
    let mut f = FALLBACK.lock().unwrap_or_else(|e| e.into_inner());
    for &(from, to) in ranges {
        let mut page = from & !(crate::watch::PAGE - 1);
        while page < to {
            f.pages.insert(page);
            page += crate::watch::PAGE;
        }
    }
}

/// The pages to look at whole this frame (once a frame; none after).
pub(crate) fn to_check(frame: u64) -> Vec<usize> {
    let mut f = FALLBACK.lock().unwrap_or_else(|e| e.into_inner());
    if f.looked == frame {
        return Vec::new();
    }
    f.looked = frame;
    let mut out = Vec::new();
    while out.len() < check_budget() {
        let from = f.next;
        let Some(&page) = f.pages.range(from..).next().or_else(|| f.pages.iter().next()) else {
            break;
        };
        f.pages.remove(&page);
        f.next = page + crate::watch::PAGE;
        out.push(page);
    }
    CHECKED[0].fetch_add(out.len() as u64, Relaxed);
    out
}

/// The fallback found slot `index` (at `handle`) with a new record: a view
/// record that no mark told of is counted and said.
pub(crate) fn missed(index: u32, handle: u64, view: bool, tabled: bool) {
    if !view {
        CHECKED[2].fetch_add(1, Relaxed);
        return;
    }
    if tabled {
        // A draw read it through a table lately: the draw makes it anyway.
        CHECKED[4].fetch_add(1, Relaxed);
    }
    let adopted = ADOPTED.read().unwrap_or_else(|e| e.into_inner());
    let Some(a) = adopted.as_ref() else { return };
    if marked(a, index) {
        // Told, not taken yet.
        CHECKED[3].fetch_add(1, Relaxed);
        return;
    }
    let n = CHECKED[1].fetch_add(1, Relaxed);
    MISSED_ALL.fetch_add(1, Relaxed);
    let rip = a.writers.get(((handle - a.start) >> 16) as usize).map_or(0, |w| w.load(Relaxed));
    let (base, size) = (IMAGE[0].load(Relaxed), IMAGE[1].load(Relaxed));
    let writer = if rip >= base && rip < base + size { format!("title+{:#x}", rip - base) } else if rip != 0 { format!("{rip:#x} (not the title's)") } else { "not known (no fault on its block)".into() };
    {
        let mut by = WRITERS.lock().unwrap_or_else(|e| e.into_inner());
        *by.entry(writer.clone()).or_default() += 1;
    }
    if n < 20 || n.is_power_of_two() {
        tracing::warn!(
            slot = index,
            handle = format_args!("{handle:#x}"),
            %writer,
            "renderer port 2: a view record in the title's heap that no store told of (found by the fallback; made now)"
        );
    }
}

/// The functions the title's `memcpy` and `memmove` imports were bound to
/// before ([`import`]).
static REAL: [AtomicUsize; 2] = [const { AtomicUsize::new(0) }; 2];

/// Since the last line: the title's copies into the marked heap, records they marked.
static COPIES: [AtomicU64; 2] = [const { AtomicU64::new(0) }; 2];

/// What the title's import of `name` (bound to `address`, which may be
/// `titlewrite`'s) is bound to: with the switch asked for, its `memcpy` and
/// `memmove` mark the view records they copy into the heap.
pub fn import(name: &str, address: usize) -> usize {
    let (which, ours) = match name {
        "memcpy" => (0, copy as *const () as usize),
        "memmove" => (1, shift as *const () as usize),
        _ => return address,
    };
    if !wanted() {
        return address;
    }
    REAL[which].store(address, Relaxed);
    ours
}

type Copy = unsafe extern "system-unwind" fn(*mut u8, *const u8, usize) -> *mut u8;

unsafe extern "system-unwind" fn copy(to: *mut u8, from: *const u8, n: usize) -> *mut u8 {
    // SAFETY: the title's copy, by the function it asked for.
    unsafe { copied(to, from, n, REAL[0].load(Relaxed)) }
}

unsafe extern "system-unwind" fn shift(to: *mut u8, from: *const u8, n: usize) -> *mut u8 {
    // SAFETY: as above.
    unsafe { copied(to, from, n, REAL[1].load(Relaxed)) }
}

/// # Safety
/// `real` copies as `memcpy` does; the rest are the title's.
#[inline]
unsafe fn copied(to: *mut u8, from: *const u8, n: usize, real: usize) -> *mut u8 {
    // SAFETY: the function noted by `import` before the title ran.
    let real: Copy = unsafe { std::mem::transmute(real) };
    // SAFETY: by the caller.
    let out = unsafe { real(to, from, n) };
    let len = MARKS.len.load(Acquire);
    if len != 0 && n >= 4 {
        let start = MARKS.start.load(Relaxed);
        let (lo, hi) = ((to as u64).max(start), (to as u64 + n as u64).min(start + len));
        if lo < hi {
            COPIES[0].fetch_add(1, Relaxed);
            // Each slot whose first word now lies in the copy and is a view's.
            let mut slot = (lo - start).div_ceil(32);
            let mut marked = 0;
            while start + slot * 32 + 4 <= hi {
                let at = start + slot * 32;
                // SAFETY: just written by the copy, inside the heap.
                if unsafe { (at as *const u32).read_unaligned() } == crate::descriptor::VIEW_TAG {
                    mark_slot(slot as usize);
                    marked += 1;
                }
                slot += 1;
            }
            COPIES[1].fetch_add(marked, Relaxed);
        }
    }
    out
}

/// Our own calls' marks since the last line.
static MARKED_BY_US: AtomicU64 = AtomicU64::new(0);
/// Since the last line: slots taken, takings, nanoseconds taking.
static TAKEN: [AtomicU64; 3] = [const { AtomicU64::new(0) }; 3];
/// Since the last line, of the fallback: pages looked at whole, view records
/// no mark told of, other records changed (not views), view records told and
/// not taken yet, and of the view records, those of draws' tables.
pub(crate) static CHECKED: [AtomicU64; 5] = [const { AtomicU64::new(0) }; 5];
/// Since the start: view records the fallback found that no mark told of.
static MISSED_ALL: AtomicU64 = AtomicU64::new(0);
/// Since the start: the missed records by the place that last faulted on their block.
static WRITERS: std::sync::Mutex<std::collections::BTreeMap<String, u64>> = std::sync::Mutex::new(std::collections::BTreeMap::new());
/// Since the start: slots taken, of them marked by our calls; pages checked.
static ALL: [AtomicU64; 3] = [const { AtomicU64::new(0) }; 3];

/// With the frame line: what port #2 did per frame over `frames`.
pub(crate) fn line(frames: f64) {
    if !wanted() {
        return;
    }
    let [taken, takings, ns] = [0, 1, 2].map(|i| TAKEN[i].swap(0, Relaxed));
    let ours = MARKED_BY_US.swap(0, Relaxed);
    let [pages, missed, other, pending, tabled] = [0, 1, 2, 3, 4].map(|i| CHECKED[i].swap(0, Relaxed));
    let copies = [0, 1].map(|i| COPIES[i].swap(0, Relaxed));
    ALL[0].fetch_add(taken, Relaxed);
    ALL[1].fetch_add(ours, Relaxed);
    ALL[2].fetch_add(pages, Relaxed);
    if IMAGE[2].load(Relaxed) == 0 {
        tracing::info!("renderer port 2 (LEONIDA_PORT_DESCRIPTORS=1): not working, no store of the title's was patched");
        return;
    }
    if !on() {
        return;
    }
    let writers = {
        let by = WRITERS.lock().unwrap_or_else(|e| e.into_inner());
        let mut v: Vec<(&String, &u64)> = by.iter().collect();
        v.sort_by(|a, b| b.1.cmp(a.1));
        v.iter().take(6).map(|(w, n)| format!("{w} {n}")).collect::<Vec<_>>().join(", ")
    };
    tracing::info!(
        "renderer port 2 (LEONIDA_PORT_DESCRIPTORS=1) per frame: {:.0} slots told (of them {:.0} marked by our calls, the rest by the title's {} patched stores) in {:.1} takings, {:.3} ms taking; \
         of the title's memcpy/memmove calls into the heap {:.0}, {:.0} records they marked; fallback: {:.1} pages looked at whole, {:.2} view records no store told of ({:.2} of them of draws' tables), {:.0} other records changed, {:.1} told and not taken yet; \
         since the start: {} slots told ({} by our calls), {} pages looked at, {} view records missed; their writers: {}",
        taken as f64 / frames,
        ours as f64 / frames,
        IMAGE[2].load(Relaxed),
        takings as f64 / frames,
        ns as f64 / 1e6 / frames,
        copies[0] as f64 / frames,
        copies[1] as f64 / frames,
        pages as f64 / frames,
        missed as f64 / frames,
        tabled as f64 / frames,
        other as f64 / frames,
        pending as f64 / frames,
        ALL[0].load(Relaxed),
        ALL[1].load(Relaxed),
        ALL[2].load(Relaxed),
        MISSED_ALL.load(Relaxed),
        if writers.is_empty() { "none".into() } else { writers }
    );
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn marks_layout_is_what_the_stores_read() {
        assert_eq!(size_of::<Marks>(), 32);
        assert_eq!(std::mem::offset_of!(Marks, start), 0);
        assert_eq!(std::mem::offset_of!(Marks, len), 8);
        assert_eq!(std::mem::offset_of!(Marks, slots), 16);
        assert_eq!(std::mem::offset_of!(Marks, chunks), 24);
    }

    /// Slots marked as the stores mark them, by our calls and by a title
    /// `memcpy`, come out once each, as runs; outside the heap nothing is
    /// marked. (One test: the maps are statics.)
    #[test]
    fn marked_slots_are_taken_once_as_runs() {
        let start = 0x1000_0000u64;
        assert!(adopt_now(start, 1000));
        assert!(!adopt_now(start + 0x10_0000, 10), "the first heap only");
        let store = |slot: usize| {
            let a = ADOPTED.read().unwrap();
            let a = a.as_ref().unwrap();
            a.slots[slot].store(1, Release);
            a.chunks[slot / CHUNK].store(1, Release);
        };
        store(5);
        mark(start + 6 * 32 + 7);
        mark(start + 300 * 32);
        store(999);
        mark(start - 32);
        mark(start + 1000 * 32);
        assert!(marked(ADOPTED.read().unwrap().as_ref().unwrap(), 5));
        // `adopted` and `take` need the switch only for the page route's choice.
        assert_eq!(take(start).unwrap(), [(5, 2), (300, 1), (999, 1)]);
        assert_eq!(take(start).unwrap(), []);
        assert!(take(start + 32).is_none());
        // A title's copy of three records, one of them not a view, into slots 40-42 of the heap.
        let mut heap = vec![0u8; 1000 * 32];
        let base = heap.as_mut_ptr() as u64;
        // (The heap is a vector's memory here: the maps stand for it.)
        MARKS.start.store(base, Release);
        let mut records = [0u8; 96];
        for k in [0usize, 2] {
            records[k * 32..k * 32 + 4].copy_from_slice(&crate::descriptor::VIEW_TAG.to_le_bytes());
        }
        // SAFETY: `memcpy`'s own, into the vector.
        unsafe {
            REAL[0].store(libc_memcpy as usize, Relaxed);
            copy(heap.as_mut_ptr().add(40 * 32), records.as_ptr(), records.len());
            // A copy that begins inside a record leaves it to the copy that wrote its first word.
            copy(heap.as_mut_ptr().add(100 * 32 + 8), records.as_ptr(), 24);
        }
        assert_eq!(take(base).unwrap_or_default(), []);
        MARKS.start.store(start, Release);
        assert_eq!(&heap[40 * 32..40 * 32 + 4], &crate::descriptor::VIEW_TAG.to_le_bytes());
        // The copy marked slots 40 and 42, not 41 (no view) nor 100.
        let set: Vec<usize> = {
            let guard = ADOPTED.read().unwrap();
            let a = guard.as_ref().unwrap();
            (0..1000).filter(|&i| a.slots[i].load(Relaxed) != 0).collect()
        };
        assert_eq!(set, [40, 42]);
        // (Taken as the heap at `base` would have been.)
        for i in [40, 42] {
            ADOPTED.read().unwrap().as_ref().unwrap().slots[i].store(0, Relaxed);
        }
        ADOPTED.read().unwrap().as_ref().unwrap().chunks[0].store(0, Relaxed);
        let_go(start);
        mark(start + 5 * 32);
        assert!(take(start).is_none());
        assert_eq!(MARKS.len.load(Relaxed), 0);
    }

    unsafe extern "system-unwind" fn libc_memcpy(to: *mut u8, from: *const u8, n: usize) -> *mut u8 {
        // SAFETY: the test's own ranges.
        unsafe { std::ptr::copy(from, to, n) };
        to
    }

    #[test]
    fn parts_are_apart_and_the_gaps_closed() {
        let (start, end, step) = (0x10_0000u64, 0x10_0000 + 1000 * 32, 32u64);
        let at = |slot: u64| (start + slot * step) as usize;
        // Runs 5-6, 20, 300-301: the first two are 13 slots apart, joined; the third is far.
        let runs = [(5, 2), (20, 1), (300, 2)];
        let parts = plan_parts(start, end, step, 1000, &runs, &[], 0x10000);
        assert_eq!(parts, [(at(5), at(21), false), (at(300), at(302), false)]);
        // A run past the mirrored count is cut, a run wholly past is gone.
        let parts = plan_parts(start, end, step, 302, &[(300, 5), (400, 1)], &[], 0x10000);
        assert_eq!(parts, [(at(300), at(302), false)]);
        // A fallback page (here 4 KiB = 128 slots, from slot 256) holds the run inside it, trims nothing, and is one part.
        let page = at(256);
        let parts = plan_parts(start, end, step, 1000, &[(260, 2), (380, 10)], &[page], 4096);
        assert_eq!(parts, [(at(256), at(390), true)]);
        // Disjoint, ascending, in any case.
        let parts = plan_parts(start, end, step, 1000, &[(0, 3), (200, 100), (900, 50)], &[at(128), at(256)], 4096);
        assert!(parts.windows(2).all(|w| w[0].1 <= w[1].0), "{parts:?}");
        assert!(parts.iter().all(|&(from, to, _)| from >= at(0) && to <= end as usize && from < to));
        // A page before the heap's start or past its end is cut to it.
        let parts = plan_parts(start, end, step, 1000, &[], &[start as usize - 4096, end as usize - 64], 4096);
        assert_eq!(parts, [(at(998), at(1000), true)]);
        assert!(plan_parts(start, end, step, 0, &runs, &[at(0)], 4096).is_empty());
    }

    #[test]
    fn a_taken_slot_is_found_in_its_run() {
        let taken = [(5, 2), (300, 1), (999, 1)];
        for (index, yes) in [(4, false), (5, true), (6, true), (7, false), (300, true), (301, false), (998, false), (999, true), (1000, false)] {
            assert_eq!(was_taken(&taken, index), yes, "{index}");
        }
        assert!(!was_taken(&[], 0));
    }
}
