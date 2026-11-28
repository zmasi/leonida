//! What the host device holds of memory, by what it is and where it lies
//! (diagnostics).
//!
//! The memory lines count what the layer holds: a host copy, a window or a
//! host texture leaves them when its owner lets it go. The device lets go
//! later, or not at all: a descriptor slot keeps what it shows
//! (`HostHeap::kept`), a list what its commands name until it is reset and
//! its own scratch buffers for as long as it lives, a spare table what it may
//! hand out again. In the play session of 2026-11-27 (RTX 4060 Ti, 16 GB) the
//! card's memory in use stood 5 GB above everything those lines add up to,
//! and rose by what [`crate::host::MADE`] counted as card buffers made, MB
//! for MB.
//!
//! Here a buffer, a texture or a block of memory is counted when it is made
//! and counted out when the device destroys it: each holds a small object of
//! ours (`SetPrivateDataInterface`), which the device releases with it, as
//! `crate::pool` and `crate::book` have theirs. Bytes are what the device
//! says the allocation takes (a buffer of 256 bytes takes 64 KiB), the place
//! is what the heap's properties say. Counted by what it is and by the line
//! of the source that made it ([`made`] and the others take their caller's
//! place; the buffers of `Host::upload_buffer` and its like, their caller's).
//!
//! Not counted: the swap chain's buffers, descriptor heaps, pipelines, what
//! the driver and an upscaler make for themselves, and the menu's, the
//! panel's and the trainer's own.

use std::collections::{BTreeMap, HashMap};
use std::ffi::c_void;
use std::panic::Location;
use std::sync::Mutex;
use std::sync::atomic::{AtomicU32, AtomicU64, Ordering::Relaxed};

use windows::Win32::Graphics::Direct3D12::*;
use windows::core::{GUID, HRESULT, IUnknown, Interface};

/// Where memory lies: in the card's own memory; there, and mapped by the
/// CPU; in the PC's memory that the card shares (upload, read-back and
/// write-back heaps on a card with memory of its own); inside a block or
/// title memory counted elsewhere; nowhere (a texture made without memory).
const CARD: usize = 0;
const MAPPED: usize = 1;
const SHARED: usize = 2;
const INSIDE: usize = 3;
const NONE: usize = 4;

const LIES: [&str; 5] = [
    "the card's own",
    "the card's that the CPU maps",
    "the PC's shared with the card",
    "inside blocks or title memory (no memory of their own)",
    "made without memory",
];

/// What one line of the source made of one kind in one place: alive now
/// (objects, bytes) and made since the start.
struct Tally {
    what: &'static str,
    file: &'static str,
    line: u32,
    lies: usize,
    alive: [AtomicU64; 2],
    made: [AtomicU64; 2],
}

type Key = (&'static str, &'static str, u32, usize);

/// Every tally, by what, file, line and place. Never more than the source
/// has lines that make memory.
static TALLIES: Mutex<Option<HashMap<Key, &'static Tally>>> = Mutex::new(None);

fn tally(what: &'static str, at: &'static Location<'static>, lies: usize) -> &'static Tally {
    let mut tallies = TALLIES.lock().unwrap_or_else(|e| e.into_inner());
    let found: &mut &'static Tally = tallies.get_or_insert_with(HashMap::new).entry((what, at.file(), at.line(), lies)).or_insert_with(|| {
        Box::leak(Box::new(Tally {
            what,
            file: at.file(),
            line: at.line(),
            lies,
            alive: [const { AtomicU64::new(0) }; 2],
            made: [const { AtomicU64::new(0) }; 2],
        }))
    });
    *found
}

/// The object a counted thing holds: when the device releases it, the thing
/// is gone.
#[repr(C)]
struct Token {
    methods: &'static [*const (); 3],
    references: AtomicU32,
    tally: &'static Tally,
    bytes: u64,
}

struct Methods([*const (); 3]);
// SAFETY: function addresses only.
unsafe impl Sync for Methods {}
static TOKEN: Methods = Methods([query as *const (), add_ref as *const (), release as *const ()]);

const KEY: GUID = GUID::from_u128(0x8a41c3e7_52d6_4f0b_b1a9_3c6e0d7f2e45);

unsafe extern "system" fn query(this: *mut c_void, iid: *const GUID, out: *mut *mut c_void) -> HRESULT {
    // SAFETY: COM's arguments.
    unsafe {
        if out.is_null() {
            return HRESULT(0x8000_4003_u32 as i32);
        }
        if iid.as_ref().is_some_and(|iid| *iid == IUnknown::IID) {
            add_ref(this);
            *out = this;
            return HRESULT(0);
        }
        *out = std::ptr::null_mut();
    }
    HRESULT(0x8000_4002_u32 as i32)
}

unsafe extern "system" fn add_ref(this: *mut c_void) -> u32 {
    // SAFETY: one of ours, alive while a reference to it is.
    unsafe { (*this.cast::<Token>()).references.fetch_add(1, Relaxed) + 1 }
}

unsafe extern "system" fn release(this: *mut c_void) -> u32 {
    // SAFETY: as above; the last reference frees the box it was made as.
    // (Counters only: the device may call this on any thread, under its own
    // locks.)
    unsafe {
        let left = (*this.cast::<Token>()).references.fetch_sub(1, std::sync::atomic::Ordering::AcqRel) - 1;
        if left == 0 {
            let gone = Box::from_raw(this.cast::<Token>());
            gone.tally.alive[0].fetch_sub(1, Relaxed);
            gone.tally.alive[1].fetch_sub(gone.bytes, Relaxed);
        }
        left
    }
}

/// A token of `bytes` for `tally`, with one reference, counted in.
fn token(tally: &'static Tally, bytes: u64) -> *mut c_void {
    tally.alive[0].fetch_add(1, Relaxed);
    tally.alive[1].fetch_add(bytes, Relaxed);
    tally.made[0].fetch_add(1, Relaxed);
    tally.made[1].fetch_add(bytes, Relaxed);
    Box::into_raw(Box::new(Token { methods: &TOKEN.0, references: AtomicU32::new(1), tally, bytes })).cast()
}

/// Counts `object` in `tally` until the device destroys it. Counted again
/// (another name for the same thing), the count before goes: the device
/// releases what the key held.
fn count(object: &ID3D12Object, tally: &'static Tally, bytes: u64) {
    // SAFETY: a COM object of ours with one reference, which this drops at
    // the end; the object takes its own. If it takes none, that drop is the
    // last and counts it out again.
    unsafe {
        let token = IUnknown::from_raw(token(tally, bytes));
        let _ = object.SetPrivateDataInterface(&KEY, &token);
    }
}

/// Where a heap of these properties lies. (On a card without memory of its
/// own everything is the PC's; the names are a discrete card's.)
fn lies_in(heap: &D3D12_HEAP_PROPERTIES) -> usize {
    match heap.Type {
        D3D12_HEAP_TYPE_DEFAULT => CARD,
        D3D12_HEAP_TYPE_GPU_UPLOAD => MAPPED,
        D3D12_HEAP_TYPE_CUSTOM if heap.MemoryPoolPreference == D3D12_MEMORY_POOL_L1 => {
            if heap.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE { CARD } else { MAPPED }
        }
        _ => SHARED,
    }
}

/// The bytes the device says `resource` takes: its description's allocation
/// (whole 64 KiB for a buffer), or a buffer's width if the device does not
/// say.
fn allocation(resource: &ID3D12Resource) -> u64 {
    // SAFETY: plain queries on a live resource and its device.
    unsafe {
        let desc = resource.GetDesc();
        let mut device: Option<ID3D12Device> = None;
        let size = match (resource.GetDevice(&mut device), device) {
            (Ok(()), Some(device)) => device.GetResourceAllocationInfo(0, &[desc]).SizeInBytes,
            _ => u64::MAX,
        };
        if size != u64::MAX {
            size
        } else if desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER {
            desc.Width
        } else {
            0
        }
    }
}

/// `resource` was made with memory of its own (a committed resource), or
/// with none (a reserved one, which counts no bytes): `what` it is, in the
/// plural, as the line says it. Counted for the caller's place in the source.
#[track_caller]
pub(crate) fn made(resource: &ID3D12Resource, what: &'static str) {
    made_at(resource, what, Location::caller());
}

/// [`made`] for a place given by the caller (a maker of buffers gives its
/// own caller's).
pub(crate) fn made_at(resource: &ID3D12Resource, what: &'static str, at: &'static Location<'static>) {
    let Ok(object) = resource.cast::<ID3D12Object>() else { return };
    let mut heap = D3D12_HEAP_PROPERTIES::default();
    // SAFETY: a plain query on a live resource; a reserved one has no heap
    // and says so.
    let lies = if unsafe { resource.GetHeapProperties(Some(&mut heap), None) }.is_ok() { lies_in(&heap) } else { NONE };
    let bytes = if lies == NONE { 0 } else { allocation(resource) };
    count(&object, tally(what, at, lies), bytes);
}

/// `resource` was placed in a block ([`block`] counts that) or in title
/// memory opened as a heap: counted with the bytes it takes there, apart from
/// what has memory of its own.
#[track_caller]
pub(crate) fn placed(resource: &ID3D12Resource, what: &'static str) {
    let Ok(object) = resource.cast::<ID3D12Object>() else { return };
    count(&object, tally(what, Location::caller(), INSIDE), allocation(resource));
}

/// A block of memory (a heap) was made, for what is placed in it.
#[track_caller]
pub(crate) fn block(heap: &ID3D12Heap, what: &'static str) {
    let Ok(object) = heap.cast::<ID3D12Object>() else { return };
    // SAFETY: a plain query on a live heap.
    let desc = unsafe { heap.GetDesc() };
    count(&object, tally(what, Location::caller(), lies_in(&desc.Properties)), desc.SizeInBytes);
}

/// What `resource` is counted as, and its bytes; `None` if it is not counted.
fn counted(resource: &ID3D12Resource) -> Option<(&'static str, u64)> {
    let mut held: *mut c_void = std::ptr::null_mut();
    let mut size = size_of::<*mut c_void>() as u32;
    // SAFETY: the key holds an interface of ours or nothing; what comes back
    // comes with a reference, given back once read.
    unsafe {
        resource.GetPrivateData(&KEY, &mut size, Some((&raw mut held).cast())).ok()?;
        if held.is_null() {
            return None;
        }
        let token = &*held.cast::<Token>();
        let found = (token.tally.what, token.bytes);
        release(held);
        Some(found)
    }
}

/// A file of the source as the lines name it: from `src` on, with `/`.
fn short(file: &str) -> String {
    let file = file.replace('\\', "/");
    match file.rfind("/src/") {
        Some(at) => file[at + 5..].to_owned(),
        None => file,
    }
}

fn mb(bytes: u64) -> u64 {
    (bytes + (1 << 19)) >> 20
}

/// One tally as it stands: what, file, line, place, alive (objects, bytes),
/// made (objects, bytes).
type Stand = (&'static str, &'static str, u32, usize, [u64; 2], [u64; 2]);

fn stands() -> Vec<Stand> {
    let tallies = TALLIES.lock().unwrap_or_else(|e| e.into_inner());
    let Some(tallies) = tallies.as_ref() else { return Vec::new() };
    tallies
        .values()
        .map(|t| (t.what, t.file, t.line, t.lies, [0, 1].map(|i| t.alive[i].load(Relaxed)), [0, 1].map(|i| t.made[i].load(Relaxed))))
        .collect()
}

/// The two lines: what is alive by place and kind, and by where it was made.
fn lines(stands: &[Stand]) -> [String; 2] {
    /// The places of the source the second line names.
    const MOST: usize = 16;
    let mut places = Vec::new();
    for (lies, name) in LIES.iter().enumerate() {
        let mut kinds: BTreeMap<&str, [u64; 2]> = BTreeMap::new();
        for &(what, _, _, _, alive, _) in stands.iter().filter(|s| s.3 == lies && s.4[0] > 0) {
            let kind = kinds.entry(what).or_default();
            kind[0] += alive[0];
            kind[1] += alive[1];
        }
        if kinds.is_empty() {
            continue;
        }
        let all = kinds.values().fold([0u64; 2], |sum, k| [sum[0] + k[0], sum[1] + k[1]]);
        let mut kinds: Vec<(&str, [u64; 2])> = kinds.into_iter().collect();
        kinds.sort_by_key(|k| std::cmp::Reverse(k.1[1]));
        let kinds: Vec<String> = kinds.iter().map(|(what, k)| format!("{what} {} MB in {}", mb(k[1]), k[0])).collect();
        places.push(format!("{name} {} MB in {} ({})", mb(all[1]), all[0], kinds.join(", ")));
    }
    let made = stands.iter().fold([0u64; 2], |sum, s| [sum[0] + s.5[0], sum[1] + s.5[1]]);
    let alive: u64 = stands.iter().map(|s| s.4[0]).sum();
    let first = format!(
        "memory the host device holds now, by where it lies and what it is (counted when it is made and when the device lets it go, \
         which is later than its owner does where a descriptor slot, a list or a spare table still holds it; the swap chain, \
         descriptor heaps and the driver's own are not in it): {}; made since the start: {} objects {} MB, of them the device \
         has let go {}",
        if places.is_empty() { "nothing yet".to_owned() } else { places.join("; ") },
        made[0],
        mb(made[1]),
        made[0].saturating_sub(alive),
    );
    let mut by_place: Vec<&Stand> = stands.iter().filter(|s| s.4[0] > 0).collect();
    by_place.sort_by_key(|s| std::cmp::Reverse(s.4[1]));
    let shown: Vec<String> = by_place
        .iter()
        .take(MOST)
        .map(|&&(what, file, line, _, alive, made)| {
            format!("{}:{line} {what} {} MB in {} (made there so far: {} of {} MB)", short(file), mb(alive[1]), alive[0], made[0], mb(made[1]))
        })
        .collect();
    let second = format!(
        "memory the host device holds now, by the line of the source that made it, the {} biggest of {} with something alive: {}",
        shown.len(),
        by_place.len(),
        if shown.is_empty() { "nothing yet".to_owned() } else { shown.join("; ") },
    );
    [first, second]
}

/// Logs the two lines, with the memory lines.
pub(crate) fn log() {
    for line in lines(&stands()) {
        tracing::info!("{line}");
    }
    let [waiting, handed] = crate::host::waiting_to_go();
    tracing::info!(
        "memory, what waits to be let go: {waiting} objects let go that wait for the submissions that may use them (LEONIDA_BURY_LAG), \
         {handed} handed to the thread that drops them and not dropped yet (LEONIDA_REAPER=0: dropped by the submitting thread)"
    );
}

/// Logs what the slots of the title's view heap keep alive (`HostHeap::kept`:
/// each slot holds the buffer or texture it shows until it is written again),
/// and what of that nothing else holds: its owner let it go, and it stays in
/// the card's memory for as long as the title leaves those slots as they are.
/// Every slot is asked, under its lock: on a thread of its own, with the
/// memory map.
pub(crate) fn log_slots() {
    let Some(kept) = crate::descriptor::with_view_heap(|heap| heap.kept_now()) else {
        return;
    };
    let mut all = [0u64; 3];
    let mut alone: BTreeMap<&'static str, [u64; 2]> = BTreeMap::new();
    for (resource, slots) in &kept {
        let (what, bytes) = counted(resource).unwrap_or(("things the census does not count", 0));
        all[0] += 1;
        all[1] += bytes;
        all[2] += u64::from(*slots);
        // One reference is ours here, one each slot's that shows it.
        if crate::host::references(resource) == *slots + 1 {
            let kind = alone.entry(what).or_default();
            kind[0] += 1;
            kind[1] += bytes;
        }
    }
    let sum = alone.values().fold([0u64; 2], |sum, k| [sum[0] + k[0], sum[1] + k[1]]);
    let mut kinds: Vec<(&str, [u64; 2])> = alone.into_iter().collect();
    kinds.sort_by_key(|k| std::cmp::Reverse(k.1[1]));
    let kinds: Vec<String> = kinds.iter().map(|(what, k)| format!("{what} {} MB in {}", mb(k[1]), k[0])).collect();
    tracing::info!(
        "memory, what the view heap's slots keep alive: {} buffers and textures of {} MB shown by {} slots; {} of them, {} MB, are held \
         by slots alone (their owner let them go; they stay until the title writes those slots again){}{}",
        all[0],
        mb(all[1]),
        all[2],
        sum[0],
        mb(sum[1]),
        if kinds.is_empty() { "" } else { ": " },
        kinds.join(", "),
    );
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_thing_is_counted_until_its_last_reference_goes() {
        let tally = tally("things of a test", Location::caller(), CARD);
        let before = [0, 1].map(|i| tally.alive[i].load(Relaxed));
        let held = token(tally, 65_536);
        // What COM calls it by: its first word is its three methods.
        // SAFETY: a live token.
        assert_eq!(unsafe { *held.cast::<*const [*const (); 3]>() }, &raw const TOKEN.0);
        assert_eq!([0, 1].map(|i| tally.alive[i].load(Relaxed)), [before[0] + 1, before[1] + 65_536]);
        // The device takes its reference, we drop ours: still counted.
        // SAFETY: a live token.
        unsafe {
            assert_eq!(add_ref(held), 2);
            assert_eq!(release(held), 1);
        }
        assert_eq!(tally.alive[0].load(Relaxed), before[0] + 1);
        // The device destroys what held it.
        // SAFETY: the last reference.
        assert_eq!(unsafe { release(held) }, 0);
        assert_eq!([0, 1].map(|i| tally.alive[i].load(Relaxed)), before);
        assert!(tally.made[0].load(Relaxed) >= 1);
    }

    #[test]
    fn a_token_answers_for_the_plain_interface_alone() {
        let tally = tally("things of another test", Location::caller(), SHARED);
        let held = token(tally, 1);
        let mut out: *mut c_void = std::ptr::null_mut();
        // SAFETY: a live token; COM's arguments.
        unsafe {
            assert_eq!(query(held, &IUnknown::IID, &mut out), HRESULT(0));
            assert_eq!(out, held);
            assert_eq!(release(held), 1);
            assert_eq!(query(held, &ID3D12Resource::IID, &mut out), HRESULT(0x8000_4002_u32 as i32));
            assert!(out.is_null());
            assert_eq!(release(held), 0);
        }
    }

    #[test]
    fn heaps_lie_where_their_properties_say() {
        let of = |kind: D3D12_HEAP_TYPE| lies_in(&D3D12_HEAP_PROPERTIES { Type: kind, ..Default::default() });
        assert_eq!(of(D3D12_HEAP_TYPE_DEFAULT), CARD);
        assert_eq!(of(D3D12_HEAP_TYPE_GPU_UPLOAD), MAPPED);
        assert_eq!(of(D3D12_HEAP_TYPE_UPLOAD), SHARED);
        assert_eq!(of(D3D12_HEAP_TYPE_READBACK), SHARED);
        let custom = |pool: D3D12_MEMORY_POOL, page: D3D12_CPU_PAGE_PROPERTY| {
            lies_in(&D3D12_HEAP_PROPERTIES { Type: D3D12_HEAP_TYPE_CUSTOM, CPUPageProperty: page, MemoryPoolPreference: pool, ..Default::default() })
        };
        // The write-back heap of `Host::shared_buffer`.
        assert_eq!(custom(D3D12_MEMORY_POOL_L0, D3D12_CPU_PAGE_PROPERTY_WRITE_BACK), SHARED);
        assert_eq!(custom(D3D12_MEMORY_POOL_L1, D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE), CARD);
        assert_eq!(custom(D3D12_MEMORY_POOL_L1, D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE), MAPPED);
    }

    #[test]
    fn the_lines_sum_by_place_and_kind_and_name_the_biggest_places() {
        let stands: Vec<Stand> = vec![
            ("card buffers", "crates\\leonida-gpu\\src\\host.rs", 8313, CARD, [3, 3 << 16], [10, 10 << 16]),
            ("card buffers", "crates\\leonida-gpu\\src\\shadow.rs", 933, CARD, [2, 300 << 20], [2, 300 << 20]),
            ("blocks of textures", "crates/leonida-gpu/src/pool.rs", 585, CARD, [44, 44 * (64 << 20)], [45, 45 * (64 << 20)]),
            ("upload buffers", "crates\\leonida-gpu\\src\\host\\send.rs", 152, SHARED, [1, 32 << 20], [7, 200 << 20]),
            ("read-back buffers", "crates\\leonida-gpu\\src\\list.rs", 1620, SHARED, [0, 0], [5, 5 << 16]),
        ];
        let [first, second] = lines(&stands);
        assert!(first.contains("the card's own 3116 MB in 49 (blocks of textures 2816 MB in 44, card buffers 300 MB in 5)"), "{first}");
        assert!(first.contains("; the PC's shared with the card 32 MB in 1 (upload buffers 32 MB in 1)"), "{first}");
        assert!(first.ends_with("made since the start: 69 objects 3381 MB, of them the device has let go 19"), "{first}");
        assert!(!first.contains("read-back"), "{first}");
        assert!(second.contains("the 4 biggest of 4 with something alive: pool.rs:585 blocks of textures 2816 MB in 44 (made there so far: 45 of 2880 MB); shadow.rs:933 card buffers 300 MB in 2"), "{second}");
        assert!(second.contains("host/send.rs:152 upload buffers 32 MB in 1 (made there so far: 7 of 200 MB)"), "{second}");
        let [first, second] = lines(&[]);
        assert!(first.contains("nothing yet") && second.contains("nothing yet"));
    }

    /// On the card of this machine (or none: then nothing is asked): a buffer
    /// is counted, with the 64 KiB it takes, until the device destroys it.
    #[test]
    fn a_buffer_is_counted_while_the_device_holds_it() {
        let Some(host) = crate::host::host() else { return };
        let Some(buffer) = host.uav_buffer(256) else { return };
        let (what, bytes) = counted(&buffer).expect("counted when made");
        assert_eq!(what, "card buffers");
        assert_eq!(bytes, 65_536);
        let mine = |stands: Vec<Stand>| -> u64 {
            stands.iter().filter(|s| s.0 == "card buffers" && s.1.ends_with("census.rs")).map(|s| s.4[0]).sum()
        };
        let with = mine(stands());
        assert!(with >= 1);
        // Another holder, as a descriptor slot is: counted until that goes too.
        let slot = buffer.clone();
        drop(buffer);
        assert_eq!(mine(stands()), with);
        drop(slot);
        assert_eq!(mine(stands()), with - 1);
        eprintln!("a card buffer of 256 bytes: counted as {bytes} bytes of {what} while the device held it");
    }
}
