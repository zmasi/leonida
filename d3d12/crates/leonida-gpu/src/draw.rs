//! Draws on the host GPU (decision 0007, step 3). A list keeps the state
//! the title sets for its draws (pipeline, descriptor heaps, root
//! arguments, vertex and index buffers, targets); each draw then brings the
//! host list up to date: the host pipeline and our root signature, the
//! user data the translated shaders read, the host copies of the buffers
//! and descriptors they read, and the states of the textures involved.
//! State the host keeps across draws (viewports, scissors, blend factor,
//! stencil reference, topology, targets) is set on the host list as the
//! title sets it.

use std::sync::atomic::{AtomicU64, Ordering};

use leonida_shader::resource::RootIndexedView;
use leonida_shader::{ResourceType, RootType, UsedResource};

use crate::descriptor::View;
use crate::drops::{Kind, Why};
use windows::Win32::Foundation::RECT;
use windows::Win32::Graphics::Direct3D::D3D_PRIMITIVE_TOPOLOGY;
use windows::Win32::Graphics::Direct3D12::*;
use windows::Win32::Graphics::Dxgi::Common::{
    DXGI_FORMAT, DXGI_FORMAT_X24_TYPELESS_G8_UINT, DXGI_FORMAT_X32_TYPELESS_G8X24_UINT,
};
use windows::core::Interface;

use crate::host::HostList;
use crate::object::This;
use crate::pipeline::host::HostPipeline;

/// Whether a shader view description reads a texture's depth plane (the
/// ambient resolve's stand-in binds one such view and no other).
fn is_depth_srv(desc: &[u32; 10]) -> bool {
    let format = DXGI_FORMAT(crate::resource::pc_format(desc[0]) as i32);
    // In a Texture2D SRV, the union starts at word 4 and PlaneSlice is word 6.
    desc[1] == D3D12_SRV_DIMENSION_TEXTURE2D.0 as u32
        && desc[6] == 0
        && !matches!(format, DXGI_FORMAT_X32_TYPELESS_G8X24_UINT | DXGI_FORMAT_X24_TYPELESS_G8_UINT)
}

/// Shader interface metadata shared by every pipeline variant and draw.
pub(crate) struct TablePlan {
    groups: Vec<TableGroup>,
    // Direct entries can interleave slots. Preserve their declaration order
    // when recording resource reads, writes, and transitions.
    direct: Vec<(usize, usize)>,
}

struct TableGroup {
    slot: u8,
    has_cbv: bool,
    has_uav: bool,
    max_dword: u32,
    entries: Vec<UsedResource>,
    indexed: Vec<RootIndexedView>,
    // Descriptor number -> first declaration, matching the bind-time search.
    by_descriptor: Vec<(u32, usize)>,
}

impl TablePlan {
    pub(crate) fn new(used: &[UsedResource], indexed: &[RootIndexedView]) -> Self {
        let mut slots: Vec<_> = used
            .iter()
            .filter(|u| u.root == RootType::Table)
            .map(|u| u.slot)
            .collect();
        slots.sort_unstable();
        slots.dedup();
        let mut groups: Vec<_> = slots
            .iter()
            .map(|&slot| TableGroup {
                slot,
                has_cbv: false,
                has_uav: false,
                max_dword: 0,
                entries: Vec::new(),
                by_descriptor: Vec::new(),
                indexed: indexed.iter().filter(|v| v.table_slot == slot).copied().collect(),
            })
            .collect();
        let mut direct = Vec::new();
        for &u in used.iter().filter(|u| u.root == RootType::Table) {
            let i = slots
                .binary_search(&u.slot)
                .expect("table slot collected from these declarations");
            let group = &mut groups[i];
            group.has_cbv |= u.kind == ResourceType::Cbv;
            group.has_uav |= u.kind == ResourceType::Uav;
            group.max_dword = group.max_dword.max(u.table_offset_dwords);
            direct.push((i, group.entries.len()));
            group
                .by_descriptor
                .push((u.table_offset_dwords / 8, group.entries.len()));
            group.entries.push(u);
        }
        for group in &mut groups {
            group.by_descriptor.sort_unstable();
            group.by_descriptor.dedup_by_key(|item| item.0);
        }
        Self { groups, direct }
    }
}

impl TableGroup {
    fn entry(&self, descriptor: u32) -> Option<&UsedResource> {
        let i = self
            .by_descriptor
            .binary_search_by_key(&descriptor, |item| item.0)
            .ok()?;
        Some(&self.entries[self.by_descriptor[i].1])
    }

    fn needs_snapshot(&self, heap: &crate::descriptor::Heap, host_count: u32, table: u64) -> bool {
        let index_at = |dword: u32| {
            table
                .checked_add(u64::from(dword) * 4)
                .and_then(|end| heap.index(end))
        };
        match index_at(self.max_dword) {
            Some(i) => i >= host_count,
            None => self
                .entries
                .iter()
                .any(|u| index_at(u.table_offset_dwords).is_some_and(|i| i >= host_count)),
        }
    }
}

/// A root argument as the title set it.
#[derive(Debug, Clone, Default)]
enum Root {
    #[default]
    None,
    /// A descriptor table's first descriptor (an address in a heap).
    Table(u64),
    Constants(Vec<u32>),
    /// A root view's GPU address.
    View(u64),
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub(crate) struct VertexView {
    address: u64,
    size: u32,
    stride: u32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub(crate) struct IndexView {
    address: u64,
    size: u32,
    format: u32,
}

/// Host states of a depth target, and of a texture shaders read.
const DEPTH_WRITE: u32 = 0x10;
const DEPTH_READ: u32 = 0x20;
const SHADER_RESOURCE: u32 = 0x40 | 0x80;

thread_local! {
    /// While a draw is prepared (`State::bind`, then its targets' states):
    /// the depth target the draw only tests (0: none, or one it writes),
    /// and whether a view its shaders read is of that same texture.
    static TESTED_DEPTH: std::cell::Cell<(usize, bool)> =
        const { std::cell::Cell::new((0, false)) };
    /// While a stage's tables are walked (`State::tables`): whether that
    /// stage's shader can store to buffers (`Translated::stores`).
    static STORES: std::cell::Cell<bool> = const { std::cell::Cell::new(true) };
    /// While `State::early_textures` runs the texture pass: the
    /// milliseconds and the bytes that run may take, in place of the
    /// pass's own per frame.
    static PASS_BUDGET: std::cell::Cell<Option<(u64, u64, bool)>> = const { std::cell::Cell::new(None) };
}

/// The milliseconds of every frame the pass that uploads textures picked
/// by index takes before a shader that picks by index has asked for it
/// (`LEONIDA_EARLY_TEXTURES_MS`, 12; 0: never, as before), and the
/// milliseconds it may take just before a draw of the title's decoration
/// bake (`LEONIDA_BAKE_TEXTURES_MS`, 1500; 0: nothing special).
///
/// The pass ran only from the first world frame on, where it met some
/// 24,000 slots at once and needed 60-80 frames for them. The title bakes
/// its characters' skin and decorations into textures of its own while it
/// loads, 159-1,165 frames before that (runs 156-174), with a pixel shader
/// that picks its sources by index: whatever was not on the card then was
/// baked as nothing, once, for the run. On the console a texture is there
/// when its bytes are.
fn early_budgets() -> (u64, u64) {
    static MS: std::sync::OnceLock<(u64, u64)> = std::sync::OnceLock::new();
    *MS.get_or_init(|| {
        let ms = |name: &str, default: u64| {
            std::env::var(name).ok().and_then(|v| v.trim().parse::<u64>().ok()).unwrap_or(default)
        };
        (ms("LEONIDA_EARLY_TEXTURES_MS", 12), ms("LEONIDA_BAKE_TEXTURES_MS", 1500))
    })
}

/// Whether `frame` is new to `seen`, which then holds it. With `peek` the
/// shared word is read first and written only when the frame changed
/// (`LEONIDA_FRAME_PEEK=0`: swapped at every ask, as before).
///
/// `State::early_textures` asks at every draw's bind, on every thread that
/// translates lists: the swap wrote the word's cache line from all of them,
/// some 4,000 times a frame, and each write takes the line from the other
/// threads. The first ask of a frame still finds it first (the swap decides
/// among threads that read the old frame together); the others only read.
/// After DoubleGhosty/cool_fox ca7e76a (Esteban Viera; GPL-3.0-or-later).
fn first_of_frame(seen: &AtomicU64, frame: u64, peek: bool) -> bool {
    (!peek || seen.load(Ordering::Relaxed) != frame) && seen.swap(frame, Ordering::Relaxed) != frame
}

/// [`first_of_frame`]'s `peek` (on; `LEONIDA_FRAME_PEEK=0`: off).
fn frame_peek() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| std::env::var_os("LEONIDA_FRAME_PEEK").is_none_or(|v| v != "0"))
}

/// Whether a draw of the title's bake finds the textures it picks by index
/// as title memory has them at that moment (`LEONIDA_BAKE_EXACT=0`: as the
/// pass leaves them, as before). The pass looks at a slot whose memory was
/// written or mapped a frame or two later, by when the rest of it has
/// landed, and at one uploaded whole a few frames later again, for a title
/// that fills memory unnoticed: a bake in between took the bytes from before
/// (another texture's), and what it bakes stays: coloured specks all over
/// the waitress's face for the whole diner scene (run n01), none in other
/// runs. So the pass ahead of a bake takes those slots too, and its uploads
/// wait for title memory that is being remapped (`crate::texture` `BAKING`).
fn bake_exact() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| std::env::var_os("LEONIDA_BAKE_EXACT").is_none_or(|v| v != "0"))
}

/// Whether a texture found changed with nothing to tell of it is looked at
/// again in the next frame (`LEONIDA_RECHECK_SOON=0`: after four, as
/// before).
fn recheck_soon() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| std::env::var_os("LEONIDA_RECHECK_SOON").is_none_or(|v| v != "0"))
}

/// Whether the once-a-frame passes over the heap's views (the buffers and the
/// textures shaders pick by an index they compute) are recorded into a list of
/// ours that is sent ahead of the frame's first submission
/// (`LEONIDA_PASSES_AHEAD`: 1 or 0, or 2 for [`passes_ahead_where_sent`];
/// unset: with `LEONIDA_THIN=1`). Only with the line of submissions
/// (`crate::queue::line_on`), whose thread sends that list
/// (`crate::queue::ahead_of_frame`).
///
/// From PastiiJay/klbf-lab a5f5a75 by way of Fayberr/Victory-United b963101f
/// (GPL-3.0, as this). The passes were recorded into the list whose draw asked
/// first. With lists translated by threads of ours that list is any of the
/// frame's, often one submitted late: the lists submitted before it read last
/// frame's buffers through the heap (there, run ve167: hair and whole people
/// drawn from old bones, 44 dark patches in 25 s; one translating thread: 2).
///
/// What differs here: the list is sent by the thread of the line of
/// submissions, the only place lists are sent from with thin lists; a list
/// keeps its own passes, as before, where the lists ahead do not have them
/// ([`done_ahead`]); and the buffers' pass is in them only once a shader has
/// asked for it.
pub(crate) fn passes_ahead() -> bool {
    // Diagnostics: on and off while the title runs (`crate::ab`). A list
    // recorded ahead and not sent when it goes off is sent when it is on again,
    // and the lists keep their own passes until one has had them again.
    #[cfg(feature = "diagnostics")]
    if let Some(on) = crate::ab::pick("LEONIDA_PASSES_AHEAD") {
        return on && crate::queue::line_on();
    }
    static ASKED: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ASKED.get_or_init(|| match std::env::var("LEONIDA_PASSES_AHEAD") {
        Ok(v) => v.trim() != "0",
        Err(_) => crate::list::thin::wanted(),
    }) && crate::queue::line_on()
}

/// Whether the list ahead of a frame is recorded where it is sent, by the
/// line's thread at the frame's first submission (`LEONIDA_PASSES_AHEAD=2`),
/// not by a thread of its own from the present before on.
///
/// Begun at the present, the list has the views the title had written into
/// its heap by then: with the title a frame ahead of the line that is the
/// frame's, with the line caught up (the title has not begun the frame when
/// the one before is presented) a view written for the frame gets its host
/// slot and its bytes a frame late. Recorded where it is sent, it has what
/// the title wrote up to the frame's first submission, and the line's thread
/// stands for the whole pass where it otherwise waits for what is left of it
/// (the source: 12.4 ms a frame against 10, its runs ve170 and ve172). Not
/// measured here.
pub(crate) fn passes_ahead_where_sent() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| std::env::var("LEONIDA_PASSES_AHEAD").is_ok_and(|v| v.trim() == "2"))
}

/// The title's view heap whose passes the lists ahead of frames record
/// ([`passes_ahead`]; 0: none yet), and its slots: the biggest a draw has had
/// set, as elsewhere (`crate::descriptor::lent_heap`).
static AHEAD_HEAP: std::sync::Mutex<(usize, u32)> = std::sync::Mutex::new((0, 0));
/// Held while a list ahead of a frame is recorded: a heap that is let go waits
/// for it ([`view_heap_gone`]).
static AHEAD_RECORDING: std::sync::Mutex<()> = std::sync::Mutex::new(());
/// A shader has picked a buffer from [`AHEAD_HEAP`] by index: the lists ahead
/// of frames have the buffers' pass from now on.
static AHEAD_BUFFERS: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
/// The frame, plus one, of the last list ahead of a frame that had the
/// buffers' pass, and of the last that had the textures' (0: none yet).
static AHEAD_DID: [AtomicU64; 2] = [const { AtomicU64::new(0) }; 2];
/// Frames a pass in a list ahead of a frame counts for ([`done_ahead`]): the
/// title's count of frames and the line's presents are not in step (a list
/// translated late in a frame the title recorded a frame ahead of the line is
/// three of them from the last such pass), and a list that makes the pass for
/// itself beside those makes it twice. Past these the lists ahead have
/// stopped coming: nothing is presented.
const AHEAD_FRAMES: u64 = 8;
/// Since the last frame line: lists recorded ahead of a frame and the
/// microseconds that took; passes lists made for themselves all the same
/// (buffers, textures).
pub(crate) static AHEAD_STATS: [AtomicU64; 4] = [const { AtomicU64::new(0) }; 4];

/// Whether the lists ahead of frames make pass `which` (0: the buffers', 1:
/// the textures') over the heap at `at` ([`passes_ahead`]); if not, the list
/// that asks makes it for itself, as before: the switch is off, another heap
/// is theirs, or none of them has had that pass for [`AHEAD_FRAMES`] frames
/// (the first frames, a heap just taken, frames that are not presented).
fn done_ahead(which: usize, at: usize, host: &crate::descriptor::host::HostHeap) -> bool {
    if !passes_ahead() {
        return false;
    }
    let theirs = {
        let mut theirs = AHEAD_HEAP.lock().unwrap_or_else(|e| e.into_inner());
        let taken = ahead_takes(&mut theirs, at, host.count());
        if taken == Some(true) {
            // What was made for the heap before it counts for nothing.
            AHEAD_BUFFERS.store(false, Ordering::Relaxed);
            AHEAD_DID.iter().for_each(|did| did.store(0, Ordering::Relaxed));
        }
        if taken.is_some() && which == 0 {
            AHEAD_BUFFERS.store(true, Ordering::Relaxed);
        }
        taken.is_some()
    };
    let done = theirs && ahead_counts(AHEAD_DID[which].load(Ordering::Relaxed), crate::frame::current());
    if !done {
        AHEAD_STATS[2 + which].fetch_add(1, Ordering::Relaxed);
    }
    done
}

/// Whether the heap at `at`, of `count` slots, is the one the lists ahead of
/// frames record for (`theirs`: that heap and its slots; 0: none), which it
/// becomes if there is none or it is bigger (`Some(true)`). A heap that is let
/// go is taken out ([`view_heap_gone`]).
fn ahead_takes(theirs: &mut (usize, u32), at: usize, count: u32) -> Option<bool> {
    if theirs.0 == at {
        return Some(false);
    }
    if theirs.0 != 0 && count <= theirs.1 {
        return None;
    }
    *theirs = (at, count);
    Some(true)
}

/// Whether a pass a list ahead of a frame made in frame `did - 1` (0: none
/// did) still counts in `frame`.
fn ahead_counts(did: u64, frame: u64) -> bool {
    did != 0 && (frame + 1).saturating_sub(did) <= AHEAD_FRAMES
}

/// The title lets go of the descriptor heap at `at`: the lists ahead of frames
/// do not look at it any more, and a recording under way, which may be at it
/// (or at one a bigger heap has taken the place of since), has ended when this
/// returns. Called before the heap leaves `crate::descriptor`'s list, with
/// none of its locks held.
pub(crate) fn view_heap_gone(at: usize) {
    {
        let mut theirs = AHEAD_HEAP.lock().unwrap_or_else(|e| e.into_inner());
        if theirs.0 == at {
            *theirs = (0, 0);
        }
    }
    drop(AHEAD_RECORDING.lock().unwrap_or_else(|e| e.into_inner()));
}

/// Records into `list`, a list of ours that is sent ahead of a frame's first
/// submission, what the frame's lists then find done ([`passes_ahead`]): the
/// heap's buffer views brought up to date, if a shader has picked one, and the
/// textures picked by index uploaded. `false`: nothing was recorded (no draw
/// has had a view heap set yet, or that heap is gone).
pub(crate) fn record_passes_ahead(list: &HostList) -> bool {
    let began = std::time::Instant::now();
    let _recording = AHEAD_RECORDING.lock().unwrap_or_else(|e| e.into_inner());
    let at = AHEAD_HEAP.lock().unwrap_or_else(|e| e.into_inner()).0;
    // (It stays from here on: `view_heap_gone` waits for this recording.)
    if at == 0 || crate::descriptor::with_heap(at, |_| ()).is_none() {
        return false;
    }
    let heap = crate::descriptor::heap(at as This);
    let Some(host) = heap.host() else {
        return false;
    };
    let frame = crate::frame::current();
    let buffers = AHEAD_BUFFERS.load(Ordering::Relaxed) && host.strides_gpu().is_some();
    let mut stage = crate::timeline::span("ahead of a frame: the heap's buffer views");
    if buffers {
        for (shadow, from, to) in heap_buffer_pass(heap, host, frame) {
            shadow.refresh(list, from, to - from);
        }
        HEAP_BUFFER_PASSES.fetch_add(1, Ordering::Relaxed);
    }
    #[cfg(feature = "diagnostics")]
    let refreshed = std::time::Instant::now();
    stage.next("ahead of a frame: textures picked by index");
    // With the budget the pass has where it is: a frame's own behind the
    // buffers' pass, the early one's before any shader picks a buffer.
    let (early_ms, _) = early_budgets();
    let textures = buffers || early_ms > 0;
    if textures {
        if !buffers {
            EARLY_TEXTURE_PASSES[0].fetch_add(1, Ordering::Relaxed);
            PASS_BUDGET.set(Some((early_ms, 64 << 20, false)));
        }
        let _ = State::default().heap_textures(list, heap, host, None);
        PASS_BUDGET.set(None);
    }
    stage.end();
    #[cfg(feature = "diagnostics")]
    if buffers {
        HEAP_SPLIT[2].fetch_add(refreshed.elapsed().as_nanos() as u64, Ordering::Relaxed);
    }
    if buffers {
        HEAP_BUFFER_MICROS.fetch_add(began.elapsed().as_micros() as u64, Ordering::Relaxed);
    }
    // What the lists that ask may now leave to these: unless the heap is
    // another's by now.
    if AHEAD_HEAP.lock().unwrap_or_else(|e| e.into_inner()).0 == at {
        for (did, made) in AHEAD_DID.iter().zip([buffers, textures]) {
            if made {
                did.store(frame + 1, Ordering::Relaxed);
            }
        }
    }
    AHEAD_STATS[0].fetch_add(1, Ordering::Relaxed);
    AHEAD_STATS[1].fetch_add(began.elapsed().as_micros() as u64, Ordering::Relaxed);
    true
}

/// For a test: [`record_passes_ahead`] into `list` as if a draw had had the
/// heap at `at`, of `count` slots, set (`None`: whatever heap is theirs now);
/// and the heap that is theirs afterwards.
#[cfg(test)]
pub(crate) fn test_record_passes_ahead(heap: Option<(usize, u32)>, list: &HostList) -> (bool, usize) {
    if let Some(heap) = heap {
        *AHEAD_HEAP.lock().unwrap_or_else(|e| e.into_inner()) = heap;
    }
    let recorded = record_passes_ahead(list);
    (recorded, AHEAD_HEAP.lock().unwrap_or_else(|e| e.into_inner()).0)
}

/// Frames ahead the pass before a bake takes slots from that are due only
/// then ([`bake_exact`]): those of notices (1 and 2), and the first looks
/// again ([`RECHECK_FRAMES`]), these at the frame's first bake alone (a
/// look reads 48 KiB, and a frame bakes up to sixteen times).
const BAKE_AHEAD: [u64; 2] = [12, 2];

/// What [`crate::descriptor::host::HostHeap::texture_news`] said when the
/// pass before a bake last served all there was, plus one (0: it did not):
/// until something is new, the next bakes of the frame need no pass.
static BAKE_SERVED: AtomicU64 = AtomicU64::new(0);

/// Free-threaded wrapper around a reusable upload-only [`HostList`] protected
/// by [`BAKE_UPLOAD`]. D3D12 command allocators, lists, and retained COM
/// resources are free-threaded when externally synchronized.
struct BakeList(HostList);
// SAFETY: accessed exclusively under `BAKE_UPLOAD`'s mutex.
unsafe impl Send for BakeList {}

/// Serializes synchronous one-shot ped/decoration bake uploads across
/// concurrent `LEONIDA_THIN=1` recording threads and owns the reusable
/// upload-only host command list.
static BAKE_UPLOAD: std::sync::Mutex<Option<BakeList>> = std::sync::Mutex::new(None);

/// Pending descriptor publications for a synchronous bake upload pass: slots
/// stay on `neutral()` (or their previous compatible view) until the dedicated
/// upload list has finished executing on the host GPU.
struct BakeSync {
    publish: Vec<(u32, u64, u64)>,
    keep: Vec<std::sync::Arc<crate::resource::HostTexture>>,
}

/// The pixel shaders of the title's decoration bake (the title:
/// `PS_BakeDecorations_*`), by the first 16 hex digits of their hash as the
/// frame listings print it. They draw into a 256x256 corner of 2048x2048
/// targets and name no texture in a table.
pub(crate) const BAKE_PIXEL_SHADERS: [&str; 7] = [
    "c0a7fe77b1862e10",
    "0d1575a396dfe513",
    "b7cfd8dda91ebb6b",
    "280967ad158cee0b",
    "fa455c8a3c32577b",
    "d126bcae5d5d8d26",
    "fcee612943c008e0",
];

/// Geometry stages shared by every ped texture compositor / decoration bake
/// permutation (`vertex 089d6271be6650ef` fullscreen-triangle ped baker and
/// `mesh d4111a6e6ea49d91` decoration projection mesh), catching pixel-shader
/// permutations beyond `BAKE_PIXEL_SHADERS`.
const BAKE_GEOMETRY_SHADERS: [&str; 2] = ["089d6271be6650ef", "d4111a6e6ea49d91"];

/// Since the start: early runs of the texture pass, and those of them
/// made for a bake draw.
pub(crate) static EARLY_TEXTURE_PASSES: [AtomicU64; 2] = [AtomicU64::new(0), AtomicU64::new(0)];

/// Whether a buffer range a shader names through an unordered access view
/// is given to shaders as written (snapshot before the submission, compare
/// and write back after) only if that shader's code can store to buffers.
/// The title binds buffers its shaders only read through such views, and
/// every range declared costs a snapshot and a compare of its bytes: 3.97
/// GB a frame in 31 submissions, 0.49 s of snapshots and 0.28 s of
/// comparing (run 28). On by default; `LEONIDA_WRITE_SKIP=0` declares
/// every such range, as before.
fn write_skip() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| std::env::var_os("LEONIDA_WRITE_SKIP").is_none_or(|v| v != "0"))
}

/// A run of indirect records clamped in one pass ([`windows`]): the host
/// copy they came from (its number), their kind and stride, the offset of
/// the first in that copy, how many, and the list's scratch buffer that
/// holds them clamped, record `i` at `i * step`.
#[derive(Clone)]
struct Window {
    copy: u64,
    kind: u32,
    stride: u32,
    /// How far apart the records are in the copy, and in the scratch
    /// buffer: what the title walks them by ([`windows`]).
    step: u32,
    base: u64,
    records: u32,
    scratch: ID3D12Resource,
}

/// The most records clamped in one pass ([`windows`]).
const WINDOW_MOST: u64 = 64;

/// Whether indirect draws that read record after record of one argument
/// buffer are clamped a window at a time (`LEONIDA_INDIRECT_WINDOW=1`; off
/// by default).
///
/// The title issues its mesh draws one record per call, each from the next
/// slot of the same argument buffer: 3,460 a frame in gameplay, and before
/// each our clamp ran as a compute dispatch of its own between two waits,
/// so the card went from drawing to compute and back for every record. In
/// the timing run of the lit scene (run 79) a shadow draw of next to
/// nothing cost 14-16 microseconds, 4,126 records a frame.
///
/// With the switch, when a draw asks for a record a short step after the
/// one drawn before, the clamp takes that record and the ones that step
/// apart after it (16, then twice as many each time, [`WINDOW_MOST`] at
/// most, never past the buffer) into
/// the list's scratch buffer in one pass, and the draws that follow read
/// their record there with no pass of ours and no barrier (the scratch
/// buffer stays readable as arguments until the next clamp), until one of
/// these: a compute
/// dispatch, a direct draw, an indirect call with a count buffer or more
/// than one record, a draw whose shaders can store to buffers, a record
/// outside the window, another host list.
///
/// The step is the one the title is seen to take, not the stride its
/// command signature names: it draws one record a call, for which the
/// stride means nothing, and the title names 12 bytes (the three
/// words of a mesh dispatch) for records it keeps 32 bytes apart (2,173 of
/// a frame's 3,349 mesh records in run 87's listed frame; 283 are 16
/// apart, which ends a window). Walked by the named stride the window
/// never opened: run 87, with the switch, still clamped 5,068 records a
/// frame one at a time.
///
/// What it costs the guard against garbage records: nothing against a
/// hang (every record is bounded as before and charged to the list's
/// budget as before), but the budget is charged when a window is clamped,
/// so also for records of the window's tail that are never drawn. Those
/// hold garbage in this title (slots of all ones: run 93 spent lists'
/// budgets on them and emptied the draws after), so in a window a record
/// that asks for more than can run is left empty and charges nothing
/// (`crate::indirect::bound_ahead`); a tail record that looks real is
/// charged what it asks, a few thousand of 2^27. And arguments the title's CPU side changes are seen as
/// before, at the next submission.
fn windows() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| std::env::var_os("LEONIDA_INDIRECT_WINDOW").is_some_and(|v| v == "1"))
}

/// Whether indirect executions whose arguments or count lie in title memory
/// the card reads in place (renderer port 1, `LEONIDA_PORT_CONSTANTS=1`) are
/// clamped alone, right before they run (`LEONIDA_PORT_CONSTANTS_ORDER`; `=0`:
/// in batches and windows as any other; it matters only with port 1 on).
///
/// A batch or a window clamps records ahead, in the pass recorded where it
/// was opened, before the commands between it and the execution that reads
/// the record. For a host copy that is fine, its bytes are the title's of
/// the submission. Where the card reads the title's memory in place, the
/// records the title's culling passes write between the two are read before
/// they are written: the execution draws the records of an earlier frame,
/// or none (palms and trees missing for runs of frames while the camera
/// moves).
///
/// Ported from Belobug/VIctory-Belobug-AMD 631400f (Renderer port 1 ordering).
fn in_place_order() -> bool {
    crate::toggled!("LEONIDA_PORT_CONSTANTS_ORDER")
}

/// Whether indirect executions of one record are clamped in batches
/// (`crate::indirect::batch`; `LEONIDA_INDIRECT_BATCH=0`: each on its own,
/// or in windows ([`windows`])).
///
/// A frame of gameplay has 4,809 indirect executions (a capture: 3,523 mesh
/// draws, 948 dispatches, 338 draws), and with the windows still 2,355
/// clamps: each a dispatch between two full waits, with the scratch buffer
/// moved twice. The replayed frame's 84.6 ms are 75.3 ms with the clamps in
/// 133 passes, and 77.7 ms in the 980 of this.
///
/// A batch is opened by the first execution after a point where arguments
/// may have changed, and all executions up to the next such point read
/// theirs from it, each exactly the record it asks for (windows guess which
/// are next, and clamp what they do not know). Arguments in buffers change
/// by dispatches and by draws whose shaders store to buffers; those end the
/// batch, so the record an execution reads is clamped after all that was
/// recorded before it, and the pass waits for all of it, exactly as the
/// clamp of its own did. Between two executions of a batch is no wait of
/// ours: there is nothing between them to wait for but draws that store to
/// no buffer.
///
/// `LEONIDA_INDIRECT_BATCH=2`: a dispatch does not end the batch before the
/// title waits for it (a barrier, or a pipeline sync) or draws: what a
/// dispatch writes is not there for a later command without a wait, and
/// dispatches the title runs side by side (its tile classes, up to 92 at a
/// time) are not held up by waits of ours. Off: whether the title has ways
/// to wait that do not pass through its lists' methods is not known.
fn batches() -> u8 {
    static ON: std::sync::OnceLock<u8> = std::sync::OnceLock::new();
    *ON.get_or_init(|| match std::env::var("LEONIDA_INDIRECT_BATCH").as_deref() {
        Ok("0") => 0,
        Ok("2") => 2,
        _ => 1,
    })
}

/// The records clamped by the pass opened last ([`batches`]).
struct Batch {
    table: crate::indirect::batch::Table,
    scratch: ID3D12Resource,
    /// Dispatches since, which may have written arguments
    /// (`LEONIDA_INDIRECT_BATCH=2`).
    wrote: bool,
}

/// The host copies arguments were read from last: what a batch opens with,
/// as the executions after the first may read theirs from another one.
static ARGUMENT_COPIES: std::sync::Mutex<Vec<std::sync::Weak<crate::shadow::Shadow>>> = std::sync::Mutex::new(Vec::new());

/// [`ARGUMENT_COPIES`] that live, and `first` put in front of them.
fn argument_copies(first: &std::sync::Arc<crate::shadow::Shadow>) -> Vec<(u64, ID3D12Resource)> {
    let mut copies = ARGUMENT_COPIES.lock().unwrap_or_else(|e| e.into_inner());
    let live: Vec<_> = copies.iter().filter_map(std::sync::Weak::upgrade).filter(|s| s.id() != first.id()).collect();
    *copies = std::iter::once(first)
        .chain(&live)
        .take(crate::indirect::batch::SOURCES)
        .map(std::sync::Arc::downgrade)
        .collect();
    live.iter().map(|s| (s.id(), s.buffer.clone())).collect()
}

/// Whether a wait for every earlier shader write that the title asks for (a
/// barrier on a buffer, its barrier groups, a pipeline sync) is left out
/// where its host list has had such a wait and no command since that can
/// write through unordered access (`LEONIDA_BARRIER_MERGE=1`; off by default;
/// `LEONIDA_AB` can name it).
///
/// The title's buffers have host copies that no barrier of the host's can
/// name, so each of those calls is a barrier on every resource, which the
/// card may take for a full wait: between passes that only draw into their
/// targets (shadow cascades, the geometry passes, thousands of mesh draws)
/// there is nothing it orders. A wait is still recorded after anything of the
/// title's that binds a compute pipeline, or a pipeline with a stage whose
/// code stores to a buffer or a texture or is not known
/// (`crate::pipeline::Translated::writes`), after a clamp of ours, after an
/// upscale, and first thing in every host list ([`State::wait_for_writes`]).
/// Those are all that write through unordered access in a title's host list
/// today (the light stand-in runs behind a dispatch, which binds a compute
/// pipeline); a pass of ours added to one has to say so (`waited`).
///
/// After felicienradio/cold-project-vi 1141d35 and 5044b98 (vitutu, from
/// ZigZagBoi1/ScarlettPort; GPL-3.0-or-later): `VICTORY_BARRIER_MERGE`, "skip
/// redundant global UAV barriers on a host list when no unordered-access
/// write has been recorded since the previous" one. That tree counts them
/// in its frame line and gives no frame time for this change alone (its
/// whole pack: 10-14 fps on a Ryzen 7 9800X3D and RTX 4070 SUPER), so it is
/// off here until an A/B says what it is worth.
fn barrier_merge() -> bool {
    #[cfg(feature = "diagnostics")]
    if let Some(on) = crate::ab::pick("LEONIDA_BARRIER_MERGE") {
        return on;
    }
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| crate::switch::on("LEONIDA_BARRIER_MERGE"))
}

/// Since the last frame-rate line: waits for every shader write the title
/// asked for that were recorded, that [`barrier_merge`] left out, and that it
/// would leave out were it on.
static WAITS: [AtomicU64; 3] = [AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0)];

/// For the frame-rate line: what became of the title's waits for every
/// shader write.
pub(crate) fn waits_line(frames: f64) {
    let [recorded, left_out, could] = [0, 1, 2].map(|i| WAITS[i].swap(0, Ordering::Relaxed) as f64 / frames);
    if recorded + left_out > 0.0 {
        tracing::info!(
            "the title's waits for every shader write per frame (LEONIDA_BARRIER_MERGE={}): {recorded:.0} recorded, {left_out:.0} left \
             out with nothing written through unordered access since the last; {could:.0} of those recorded could be left out",
            u8::from(barrier_merge())
        );
    }
}

/// Whether a title's wait for every shader write is recorded: `waited`, its
/// list has had one and nothing since that can write through unordered
/// access; `merge`, [`barrier_merge`].
fn wait_needed(waited: bool, merge: bool) -> bool {
    !(waited && merge)
}

/// What one title list has set, and what its host list was last given.
#[derive(Default)]
pub(crate) struct State {
    pipeline: usize,
    /// View heap, sampler heap.
    heaps: [usize; 2],
    graphics: Vec<Root>,
    compute: Vec<Root>,
    vertex: [VertexView; 32],
    index: IndexView,
    /// Bound render target and depth resources, for their host states.
    targets: Vec<usize>,
    depth: usize,
    /// The host state the bound depth view needs ([`depth_state`]).
    depth_state: u32,
    /// The title's primitive topology (the host's may differ).
    topology: u32,
    /// What the title set that goes straight onto the host list, kept for
    /// the host list that continues this one after a cut ([`State::replay`]):
    /// whether a topology was set, the render target and depth handles, the
    /// viewports, the scissors, the blend factor and the stencil reference.
    topology_set: bool,
    target_handles: Vec<u64>,
    depth_handle: u64,
    targets_set: bool,
    /// LOCAL (fxceless a35c557393): the formats of the views
    /// [`State::set_targets`] bound (eight targets, then depth), for the
    /// draws' pipelines (`crate::pipeline::host_pipeline_for`).
    target_formats: crate::pipeline::host::TargetFormats,
    /// The render target views and the depth view the host list has, once
    /// it has any ([`State::set_targets`]): per view the host texture (0:
    /// a null view) and what its descriptor says.
    host_targets: Option<(Vec<TargetKey>, Option<TargetKey>)>,
    viewports: Vec<D3D12_VIEWPORT>,
    scissors: Vec<RECT>,
    blend: Option<[f32; 4]>,
    stencil: Option<u32>,
    depth_bounds: Option<[f32; 2]>,
    /// The indirect records clamped last, if later draws may still read
    /// them ([`windows`]).
    window: Option<Window>,
    /// The records clamped in a batch, if later executions may still read
    /// them ([`batches`]).
    batch: Option<Batch>,
    /// The host list has had a wait for every shader write, and nothing
    /// recorded since can write through unordered access
    /// ([`State::wait_for_writes`]).
    waited: bool,
    /// The user data bound last, of the last stage that has some (for the
    /// report of a removed device, `crate::host::indirects`).
    #[cfg(feature = "diagnostics")]
    user: Vec<u32>,
    /// The tables the last bind made afresh for a program the stand-in for
    /// the indirect light follows (`crate::ambient`): root parameter, bytes
    /// into the view heap.
    ambient_tables: Vec<(u8, u32)>,
    /// What the host list has: pipeline, root signature, heaps.
    host_pipeline: usize,
    /// Graphics, compute.
    host_roots: [usize; 2],
    host_heaps: [usize; 2],
    host_heaps_set: bool,
    /// The root arguments and the vertex and index buffers the host list
    /// has, so that equal ones are not recorded again (`crate::rootcache`).
    cache: crate::rootcache::Cache,
    /// Tables made afresh in this list: (table, slot count, contents) → first
    /// dynamic slot.
    fresh_tables: std::collections::HashMap<(u64, u32, u64), FreshTable, std::hash::BuildHasherDefault<crate::host::AddressHash>>,
    /// The dynamic slots the list holds: title heap, host heap number
    /// (`HostHeap::id`: a heap made at the same address is another),
    /// first, count; given back when the list has run, is reset or released.
    dynamic: Vec<(usize, u64, u32, u32)>,
    /// Root SRVs bound as one-view tables in this list: (title heap, title
    /// address; 0 for none) → dynamic slot, and the host copy the slot
    /// shows: later draws of the list bring that one up to date, whatever
    /// copy the address would find by then (`shadow.rs` `tails`).
    root_srv_slots: std::collections::HashMap<(usize, u64), RootSlot, std::hash::BuildHasherDefault<crate::host::AddressHash>>,
    /// Root UAVs bound as one-view tables in this list, likewise.
    root_uav_slots: std::collections::HashMap<(usize, u64), RootSlot, std::hash::BuildHasherDefault<crate::host::AddressHash>>,
    /// The list ran and its dynamic slots were given back.
    tables_gone: bool,
    /// This recording, for [`crate::descriptor::host::HostHeap::make_listed`]
    /// (0: it has none yet).
    recording: u64,
}

impl Drop for State {
    fn drop(&mut self) {
        crate::descriptor::release_dynamic_many(&self.dynamic);
    }
}

impl State {
    /// The list's submission has run (submissions are waited for): the
    /// dynamic slots its tables were made in are given back, not kept
    /// until the title resets the list. A frame of the opening cutscene
    /// asks for 12,500 tables; held until the resets they ran the 131,072
    /// slots out, and up to 4,900 draws a frame went without their tables.
    /// shortcut: a list the title runs a second time without a reset then
    /// reads slots others may have taken (logged once).
    pub(crate) fn tables_done(&mut self) {
        if self.tables_gone && self.dynamic.is_empty() {
            return;
        }
        if self.tables_gone {
            static ONCE: std::sync::Once = std::sync::Once::new();
            ONCE.call_once(|| tracing::warn!("a list ran again after its descriptor tables were given back"));
        }
        crate::descriptor::release_dynamic_many(&self.dynamic);
        self.dynamic.clear();
        self.fresh_tables.clear();
        self.recording = 0;
        self.root_srv_slots.clear();
        self.root_uav_slots.clear();
        self.tables_gone = true;
    }
}

/// An experiment on the title's scissor rectangles (`LEONIDA_SCISSOR_TEST=full`):
/// they are all widened to the whole target. The title's room at
/// the start of the prologue lost its walls in five runs of seven, and in
/// those runs the player's body and the world outside were cut by one
/// rectangle around the window (run 85, frame 2046): one run with this on
/// says whether the walls are drawn and cut away by such a rectangle, or
/// not drawn at all. Off by default: a rectangle the title sets is part of
/// its picture.
fn scissor_test() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| std::env::var("LEONIDA_SCISSOR_TEST").as_deref() == Ok("full"))
}

/// An experiment on mesh draws (`LEONIDA_MESH_TEST`): 1 for `skip` (none
/// is issued), 2 for `scissor` (each runs its shaders but rasterises one
/// pixel), 0 otherwise. It tells a card hung by a mesh shader from one hung
/// by what the shader's triangles cover.
fn mesh_test() -> u8 {
    static ON: std::sync::OnceLock<u8> = std::sync::OnceLock::new();
    *ON.get_or_init(|| match std::env::var("LEONIDA_MESH_TEST").as_deref() {
        Ok("skip") => 1,
        Ok("scissor") => 2,
        _ => 0,
    })
}

/// `LEONIDA_MESH_TEST=keep:<hex digits>[:<hex digits>]`: only the mesh
/// draws whose mesh shader's hash ends in one of the first digits (and
/// whose digit before it is one of the second, if given) are issued: a
/// search for the shaders that stop the card.
fn mesh_kept(pipeline: usize) -> bool {
    static KEEP: std::sync::OnceLock<Option<(String, String)>> = std::sync::OnceLock::new();
    let Some((last, before)) = KEEP.get_or_init(|| {
        let value = std::env::var("LEONIDA_MESH_TEST").ok()?;
        let mut parts = value.strip_prefix("keep:")?.split(':');
        Some((parts.next()?.to_owned(), parts.next().unwrap_or("").to_owned()))
    }) else {
        return true;
    };
    let Some(p) = crate::pipeline::host_pipeline(pipeline as This) else {
        return true;
    };
    let Some(at) = p.name.find("mesh ") else {
        return true;
    };
    let hash = p.name[at + 5..].as_bytes();
    if hash.len() < 16 {
        return true;
    }
    last.as_bytes().contains(&hash[15]) && (before.is_empty() || before.as_bytes().contains(&hash[14]))
}

/// Draws recorded on the host GPU since the last frame line.
/// What a list held for its descriptor tables ([`State::tables_taken`]):
/// dropping it gives the dynamic slots back and lets go of the host copies
/// its root views' slots showed.
pub(crate) struct TablesHeld {
    dynamic: Vec<(usize, u64, u32, u32)>,
    _root_srv_slots: std::collections::HashMap<(usize, u64), RootSlot, std::hash::BuildHasherDefault<crate::host::AddressHash>>,
    _root_uav_slots: std::collections::HashMap<(usize, u64), RootSlot, std::hash::BuildHasherDefault<crate::host::AddressHash>>,
}

impl Drop for TablesHeld {
    fn drop(&mut self) {
        crate::descriptor::release_dynamic_many(&self.dynamic);
    }
}

impl State {
    /// As [`State::tables_done`], with what the list held handed to the
    /// caller instead of let go of here: one that holds a lock takes it
    /// under the lock and drops it after (`crate::host::release_finished_tables`).
    /// `None` if the list holds nothing.
    pub(crate) fn tables_taken(&mut self) -> Option<TablesHeld> {
        if self.tables_gone && self.dynamic.is_empty() {
            return None;
        }
        let held = TablesHeld {
            dynamic: std::mem::take(&mut self.dynamic),
            _root_srv_slots: std::mem::take(&mut self.root_srv_slots),
            _root_uav_slots: std::mem::take(&mut self.root_uav_slots),
        };
        self.fresh_tables.clear();
        self.recording = 0;
        self.tables_gone = true;
        Some(held)
    }
}

static DRAWN: AtomicU64 = AtomicU64::new(0);

/// Draws run on the host so far.
pub(crate) fn drawn() -> u64 {
    DRAWN.load(Ordering::Relaxed)
}

/// The draws traced since the last present (diagnostics builds, with
/// debug logging for this module): logged when the frame comes out black.
#[cfg(feature = "diagnostics")]
static FRAME: std::sync::Mutex<Vec<String>> = std::sync::Mutex::new(Vec::new());

/// The draws traced since the last call.
#[cfg(feature = "diagnostics")]
pub(crate) fn take_frame() -> Vec<String> {
    std::mem::take(&mut *FRAME.lock().unwrap_or_else(|e| e.into_inner()))
}

/// Draws, mesh dispatches and dispatches recorded but not run on the host
/// since the last call, whatever the reason (`crate::drops` counts them by
/// reason: most are left out on purpose).
pub(crate) fn take_dropped() -> u64 {
    DROPPED.swap(0, Ordering::Relaxed)
}
/// A root SRV's dynamic slot in a list, and the host copy it shows.
type RootSlot = (u32, Option<std::sync::Arc<crate::shadow::Shadow>>);

/// Buffer views of the heap whose slot showed another host buffer than
/// the copy the pass brought up to date for them, since the start
/// (diagnostics; for the memory line). It stays 0 while the pass and the
/// slots agree; with `LEONIDA_VIEW_EXTENT=0` it counts the views that were
/// read stale.
#[cfg(feature = "diagnostics")]
pub(crate) static HEAP_VIEWS_ELSEWHERE: AtomicU64 = AtomicU64::new(0);
/// Passes over the view heap's buffer views for shaders that pick them by
/// index ([`State::heap_buffers`]), and the microseconds they took.
pub(crate) static HEAP_BUFFER_PASSES: AtomicU64 = AtomicU64::new(0);
pub(crate) static HEAP_BUFFER_MICROS: AtomicU64 = AtomicU64::new(0);
/// Of the passes, nanoseconds since the last frame line: looking at slots,
/// the spans and refreshes of their copies, the texture side.
#[cfg(feature = "diagnostics")]
pub(crate) static HEAP_SPLIT: [AtomicU64; 3] = [const { AtomicU64::new(0) }; 3];

/// Passes after which every buffer slot of the heap was looked at again.
const HEAP_ROUND: u64 = 16;

/// Whether the slots looked at again for no write are a part of them at
/// every pass, each slot at every sixteenth (`LEONIDA_HEAP_SPREAD=0`: all of
/// them at every sixteenth pass, as before).
///
/// All at once was some 115 ms on a thread that records the frame, every
/// sixteenth frame (72,000 slots; run record-1: 11 ms a pass in the lines
/// without such a pass, 22 in those of ten frames with one). A slot is
/// looked at as often as before.
fn heap_spread() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| std::env::var_os("LEONIDA_HEAP_SPREAD").is_none_or(|v| v != "0"))
}

/// The host copy and range the buffer view of each of a heap's slots reads
/// ([`State::heap_buffers`]), by slot.
#[derive(Default)]
struct HeapRanges<T>(Vec<Option<(T, u64, u64)>>);

/// Whether the pass keeps, for each of its sixteen parts (`slot %
/// HEAP_ROUND`), the span that each copy's views in the part cover, and
/// joins those for the spans of a pass (`LEONIDA_SWEEP_SPANS=0`: the spans
/// are made from every slot's range at every pass, as before, and a written
/// slot that is also of the pass's part is looked at twice).
///
/// A pass looks at a sixteenth of the slots and the written ones, and then
/// went through the ranges of all of them (72,000 slots) for the spans of
/// some 4,400 copies. A part's spans change only with its slots: one whose
/// slot shows another range inside the span it had, or shows something
/// where it showed nothing, is put right in place; any other change has
/// the part's spans made again from its slots, once, when they are asked
/// for. The spans are the same either way (`heap_range_tests`). What is
/// saved is the walk over the slots, not the making of the table: 0.5 ms a
/// pass from every slot, 0.28 ms from the parts and 0.1 ms more to set a
/// part's slots (`spans_of_a_pass`, an optimized build, 72,000 slots).
/// (After XaviFortes/VIctory-RDNA f8eb38d.)
fn sweep_spans() -> bool {
    crate::toggled!("LEONIDA_SWEEP_SPANS")
}

/// The parts of a round.
const PARTS: usize = HEAP_ROUND as usize;

/// A copy and the span of it that ranges cover.
type Span<T> = (std::sync::Arc<T>, u64, u64);

/// [`HeapRanges`] of copies, with the spans of each part's ranges kept
/// beside them ([`sweep_spans`]).
struct SweepRanges<T> {
    ranges: HeapRanges<std::sync::Arc<T>>,
    /// Per part and copy (by its address): the copy and the span its views
    /// in the part cover. Right for the parts that are not `stale`.
    spans: [crate::host::ByCopy<Span<T>>; PARTS],
    /// How many slots of each part show something.
    views: [u32; PARTS],
    /// The parts whose spans are to be made again from their slots, a bit
    /// each.
    stale: u16,
}

impl<T> Default for SweepRanges<T> {
    fn default() -> Self {
        Self {
            ranges: HeapRanges(Vec::new()),
            spans: std::array::from_fn(|_| Default::default()),
            views: [0; PARTS],
            stale: 0,
        }
    }
}

/// A copy's address, as [`crate::host::ByCopy`] wants it.
fn copy_key<T>(copy: &std::sync::Arc<T>) -> usize {
    std::sync::Arc::as_ptr(copy) as usize
}

/// Widens the span of `range`'s copy in `spans` by it.
fn join_span<T>(spans: &mut crate::host::ByCopy<Span<T>>, (copy, from, to): &Span<T>) {
    let span = spans.entry(copy_key(copy)).or_insert_with(|| (copy.clone(), *from, *to));
    span.1 = span.1.min(*from);
    span.2 = span.2.max(*to);
}

impl<T> SweepRanges<T> {
    fn clear(&mut self) {
        self.ranges.0.clear();
        for spans in &mut self.spans {
            spans.clear();
        }
        self.views = [0; PARTS];
        self.stale = 0;
    }

    /// Forgets the slots of one part (`HostHeap::buffer_slots_of`).
    fn forget_part(&mut self, part: u32) {
        self.ranges.forget_part(part, PARTS as u32);
        let p = part as usize % PARTS;
        self.spans[p].clear();
        self.views[p] = 0;
        self.stale &= !(1 << p);
    }

    fn set(&mut self, slot: u32, range: Option<Span<T>>) {
        let p = slot as usize % PARTS;
        let now = range.is_some();
        if self.stale >> p & 1 != 0 || !sweep_spans() {
            // Not kept, or made again anyway.
            let was = self.ranges.replace(slot, range).is_some();
            self.views[p] = (self.views[p] + u32::from(now)).saturating_sub(u32::from(was));
            self.stale |= 1 << p;
            return;
        }
        let old = self.ranges.replace(slot, range.clone());
        self.views[p] = (self.views[p] + u32::from(now)).saturating_sub(u32::from(old.is_some()));
        match (old, range) {
            (None, None) => {}
            // One more range of the copy's.
            (None, Some(new)) => join_span(&mut self.spans[p], &new),
            (Some(old), Some(new)) if copy_key(&old.0) == copy_key(&new.0) => {
                if (old.1, old.2) == (new.1, new.2) {
                    return;
                }
                // The span did not end where the old range did: without it
                // the span is as it was.
                match self.spans[p].get_mut(&copy_key(&old.0)) {
                    Some(span) if span.1 < old.1 && old.2 < span.2 => {
                        span.1 = span.1.min(new.1);
                        span.2 = span.2.max(new.2);
                    }
                    _ => self.stale |= 1 << p,
                }
            }
            // A range gone, or another copy's now: what the others of the
            // old copy cover is not known here.
            _ => self.stale |= 1 << p,
        }
    }

    /// Per copy (by its address), the span its ranges cover; and how many
    /// slots show something.
    fn spans(&mut self) -> (crate::host::ByCopy<Span<T>>, u32) {
        let mut all: crate::host::ByCopy<Span<T>> = Default::default();
        if !sweep_spans() {
            // From every slot, as before. (The parts set meanwhile are
            // stale, so their spans are made again once the switch is on.)
            let mut views = 0;
            for range in self.ranges.ranges() {
                views += 1;
                join_span(&mut all, range);
            }
            return (all, views);
        }
        for p in (0..PARTS).filter(|p| self.stale >> p & 1 != 0) {
            let part = &mut self.spans[p];
            part.clear();
            let mut views = 0;
            for range in self.ranges.part_ranges(p as u32, PARTS as u32) {
                views += 1;
                join_span(part, range);
            }
            self.views[p] = views;
        }
        self.stale = 0;
        // (As many as the part with the most, at least.)
        all.reserve(self.spans.iter().map(|part| part.len()).max().unwrap_or(0));
        for part in &self.spans {
            for span in part.values() {
                join_span(&mut all, span);
            }
        }
        (all, self.views.iter().sum())
    }
}

/// Descriptor-derived references belong to the heap that produced them.
#[derive(Default)]
pub(crate) struct BufferSweep {
    passes: u64,
    ranges: SweepRanges<crate::shadow::Shadow>,
    // Last refresh frame and covered interval for throttled, unwatched copies.
    unwatched: std::collections::HashMap<u64, (u64, u64, u64)>,
}

impl<T> HeapRanges<T> {
    #[cfg(test)]
    fn set(&mut self, slot: u32, range: Option<(T, u64, u64)>) {
        let _ = self.replace(slot, range);
    }

    /// What the slot showed before.
    fn replace(&mut self, slot: u32, range: Option<(T, u64, u64)>) -> Option<(T, u64, u64)> {
        let slot = slot as usize;
        if range.is_none() && slot >= self.0.len() {
            return None;
        }
        if slot >= self.0.len() {
            self.0.resize_with(slot + 1, || None);
        }
        std::mem::replace(&mut self.0[slot], range)
    }

    /// Forgets the slots of one of `parts` parts
    /// (`HostHeap::buffer_slots_of`).
    fn forget_part(&mut self, part: u32, parts: u32) {
        self.0.iter_mut().skip(part as usize).step_by(parts as usize).for_each(|r| *r = None);
    }

    fn part_ranges(&self, part: u32, parts: u32) -> impl Iterator<Item = &(T, u64, u64)> {
        self.0.iter().skip(part as usize).step_by(parts as usize).flatten()
    }

    fn ranges(&self) -> impl Iterator<Item = &(T, u64, u64)> {
        self.0.iter().flatten()
    }
}

#[cfg(test)]
mod heap_range_tests {
    use super::{HEAP_ROUND, HeapRanges, PARTS, SweepRanges};
    /// Round by round every slot is looked at again once, whatever was
    /// written between, and what a slot holds is what was found last.
    #[test]
    fn every_slot_is_looked_at_again_in_a_round() {
        const SLOTS: u32 = 1000;
        let mut ranges = HeapRanges(Vec::new());
        ranges.set(7, None);
        assert_eq!(ranges.ranges().count(), 0);
        for slot in (0..SLOTS).step_by(3) {
            ranges.set(slot, Some(((), 0, 1)));
        }
        let parts = HEAP_ROUND as u32;
        for pass in 1..=HEAP_ROUND {
            let part = (pass % HEAP_ROUND) as u32;
            ranges.forget_part(part, parts);
            for slot in (0..SLOTS).step_by(3).filter(|slot| slot % parts == part) {
                assert!(ranges.0[slot as usize].is_none(), "slot {slot} not forgotten");
                ranges.set(slot, Some(((), pass, pass)));
            }
            assert_eq!(ranges.ranges().count(), SLOTS.div_ceil(3) as usize);
        }
        assert!(ranges.ranges().all(|r| r.1 != 0), "a slot not looked at in a round");
        ranges.set(3, None);
        assert_eq!(ranges.ranges().count(), SLOTS.div_ceil(3) as usize - 1);
    }

    /// The spans kept part by part are those of all the slots' ranges,
    /// whatever was set, taken away and forgotten since they were last asked
    /// for.
    #[test]
    fn spans_kept_by_part_are_those_of_every_slot() {
        use std::sync::Arc;
        const SLOTS: usize = 400;
        let copies: Vec<Arc<u64>> = (0..24).map(Arc::new).collect();
        let mut kept = SweepRanges::<u64>::default();
        let mut seed = 0x9E37_79B9_7F4A_7C15u64;
        let mut random = |below: usize| {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            (seed >> 11) as usize % below
        };
        // Whether the kept spans were ever right without being made again.
        let mut in_place = 0;
        for round in 0..600 {
            match random(40) {
                0 => kept.clear(),
                1..=4 => kept.forget_part(random(PARTS) as u32),
                _ => {}
            }
            for _ in 0..random(30) {
                let slot = random(SLOTS) as u32;
                let range = match random(8) {
                    0 | 1 => None,
                    // The range the slot shows, or one of its copy's.
                    2 | 3 => kept.ranges.0.get(slot as usize).cloned().flatten().map(|(copy, from, to)| match random(3) {
                        0 => (copy, from, to),
                        1 => (copy, from + random(3) as u64, to + random(3) as u64),
                        _ => (copy, from.saturating_sub(random(50) as u64), to + random(50) as u64),
                    }),
                    _ => {
                        let from = random(1000) as u64;
                        Some((copies[random(copies.len())].clone(), from, from + 1 + random(200) as u64))
                    }
                };
                kept.set(slot, range);
            }
            in_place += u32::from(kept.stale == 0);
            let (spans, views) = kept.spans();
            assert_eq!(kept.stale, 0);
            let mut slow: std::collections::BTreeMap<usize, (u64, u64)> = Default::default();
            for (copy, from, to) in kept.ranges.ranges() {
                let span = slow.entry(Arc::as_ptr(copy) as usize).or_insert((*from, *to));
                *span = (span.0.min(*from), span.1.max(*to));
            }
            assert_eq!(views as usize, kept.ranges.ranges().count(), "round {round}");
            let fast: std::collections::BTreeMap<usize, (u64, u64)> = spans
                .iter()
                .map(|(&key, (copy, from, to))| {
                    assert_eq!(key, Arc::as_ptr(copy) as usize);
                    (key, (*from, *to))
                })
                .collect();
            assert_eq!(fast, slow, "round {round}");
            // (A part's own spans are those of its slots.)
            for p in 0..PARTS {
                assert_eq!(kept.views[p] as usize, kept.ranges.part_ranges(p as u32, PARTS as u32).count(), "round {round}, part {p}");
            }
        }
        assert!(in_place > 10, "the spans were made again every time: {in_place}");
        // Nothing of a copy is kept once no slot shows it.
        kept.clear();
        assert!(copies.iter().all(|copy| Arc::strong_count(copy) == 1));
    }

    /// The spans of 4,400 copies from the ranges of 72,000 slots, as a pass
    /// makes them, by slot and from the table they were in. Run with
    /// `--ignored --nocapture`.
    #[test]
    #[ignore]
    fn spans_of_a_pass() {
        use std::collections::HashMap;
        use std::sync::Arc;
        let copies: Vec<Arc<u64>> = (0..4400).map(Arc::new).collect();
        let (mut table, mut slots) = (HashMap::new(), HeapRanges(Vec::new()));
        for slot in 0..72_000u32 {
            let copy = &copies[slot as usize * 7919 % 4400];
            table.insert(slot, (copy.clone(), u64::from(slot), u64::from(slot) + 64));
            slots.set(slot, Some((copy.clone(), u64::from(slot), u64::from(slot) + 64)));
        }
        fn join<S: std::hash::BuildHasher + Default>(ranges: &mut dyn Iterator<Item = &(Arc<u64>, u64, u64)>) -> usize {
            let mut spans: HashMap<usize, (Arc<u64>, u64, u64), S> = HashMap::default();
            for (copy, from, to) in ranges {
                let span = spans.entry(Arc::as_ptr(copy) as usize).or_insert_with(|| (copy.clone(), *from, *to));
                span.1 = span.1.min(*from);
                span.2 = span.2.max(*to);
            }
            spans.len()
        }
        let best = |f: &mut dyn FnMut() -> usize| {
            (0..30)
                .map(|_| {
                    let t = std::time::Instant::now();
                    assert_eq!(f(), 4400);
                    t.elapsed().as_secs_f64() * 1e3
                })
                .fold(f64::MAX, f64::min)
        };
        println!(
            "{:.2} ms from the table, {:.2} ms by slot",
            best(&mut || join::<std::hash::RandomState>(&mut table.values())),
            best(&mut || join::<std::hash::BuildHasherDefault<crate::host::AddressHash>>(&mut slots.ranges())),
        );
        // Kept by part (`sweep_spans`), with a sixteenth of the slots set
        // again as a pass does: each copy's views in one part (as the slots
        // above lie: 4,400 is 16 times 275), and each copy's in many.
        for (what, spread) in [("in many parts", true), ("in one part", false)] {
            let mut kept = SweepRanges::<u64>::default();
            let copy_of = |slot: u32| &copies[slot as usize * 7919 % if spread { 4399 } else { 4400 }];
            for slot in 0..72_000u32 {
                kept.set(slot, Some((copy_of(slot).clone(), u64::from(slot), u64::from(slot) + 64)));
            }
            // A pass's part forgotten and set again (beside it, the same into
            // the slots alone, as before), then the spans.
            let mut plain = HeapRanges(Vec::new());
            for slot in 0..72_000u32 {
                plain.set(slot, Some((copy_of(slot).clone(), u64::from(slot), u64::from(slot) + 64)));
            }
            let mut ms = [f64::MAX; 3];
            for pass in 1..=30u32 {
                let part = pass % PARTS as u32;
                let t = std::time::Instant::now();
                plain.forget_part(part, PARTS as u32);
                for slot in (part..72_000).step_by(PARTS) {
                    plain.set(slot, Some((copy_of(slot).clone(), u64::from(slot), u64::from(slot) + 64)));
                }
                ms[0] = ms[0].min(t.elapsed().as_secs_f64() * 1e3);
                let t = std::time::Instant::now();
                kept.forget_part(part);
                for slot in (part..72_000).step_by(PARTS) {
                    kept.set(slot, Some((copy_of(slot).clone(), u64::from(slot), u64::from(slot) + 64)));
                }
                ms[1] = ms[1].min(t.elapsed().as_secs_f64() * 1e3);
                let t = std::time::Instant::now();
                assert_eq!(kept.spans().0.len(), if spread { 4399 } else { 4400 });
                ms[2] = ms[2].min(t.elapsed().as_secs_f64() * 1e3);
            }
            println!(
                "a copy's views {what} ({} spans in the parts): a part set again {:.2} ms (into the slots alone {:.2} ms),                  the spans from the parts {:.2} ms",
                kept.spans.iter().map(|part| part.len()).sum::<usize>(),
                ms[1],
                ms[0],
                ms[2]
            );
        }
    }
}

/// Textures brought up to date for shaders that pick them by index
/// ([`State::heap_textures`]), the bytes of their memory, and the slots
/// still waiting.
pub(crate) static HEAP_TEXTURES: AtomicU64 = AtomicU64::new(0);
pub(crate) static HEAP_TEXTURE_BYTES: AtomicU64 = AtomicU64::new(0);
pub(crate) static HEAP_TEXTURES_WAITING: AtomicU64 = AtomicU64::new(0);
/// Passes that made no texture because the card had no room
/// (`crate::resource::room`), for the memory line.
pub(crate) static HEAP_TEXTURES_FULL: AtomicU64 = AtomicU64::new(0);

/// Frames after a whole upload of a texture picked by index at which its
/// memory is looked at again, each counted from the look before.
const RECHECK_FRAMES: [u64; 5] = [4, 12, 40, 120, 400];

/// Leonida: frames between further looks at an uploaded texture picked by
/// index once [`RECHECK_FRAMES`] is through (`LEONIDA_HEAP_RECHECK_EVERY`,
/// 300; 0: never again, as before). Characters' skin and clothes are
/// streamed in and rewritten long after their views are written; a change
/// after the schedule's last look stayed on the card for the rest of the
/// run, which is the skin that turns black (or red) and stays so. The same
/// finding as fxceless/victory-fork (after RyanCase94 920423a9, GPL-3.0),
/// there off by default; here on, since a look reads only 48 KiB and only
/// a changed texture goes up again.
fn recheck_every() -> u64 {
    static EVERY: std::sync::OnceLock<u64> = std::sync::OnceLock::new();
    *EVERY.get_or_init(|| {
        std::env::var("LEONIDA_HEAP_RECHECK_EVERY").ok().and_then(|v| v.trim().parse().ok()).unwrap_or(300)
    })
}

/// Looks at uploaded textures picked by index that found the memory as it
/// was, and that found it changed (uploaded again), since the start.
pub(crate) static HEAP_TEXTURES_RECHECKED: [AtomicU64; 2] = [AtomicU64::new(0), AtomicU64::new(0)];

/// Uploads of textures already on the card made while it had no room for
/// new ones, since the start.
pub(crate) static HEAP_TEXTURES_REUPLOADED_FULL: AtomicU64 = AtomicU64::new(0);

/// Whether a pass over the textures picked by index on a full card holds
/// back the slots that need a new host copy (`LEONIDA_FULL_CARD_PASS=0`:
/// no, as before: they stay due, each pass looks at the same ones again
/// and stops at the 257th). Held back, such a slot is looked at again
/// after about four seconds or as soon as the card has room, and the pass
/// goes on to the slots behind it: the ones whose host copy exists and
/// only wants bringing up to date, and the ones whose memory went (letting
/// their host copies go is what makes room). What is uploaded while the
/// card has room is the same either way.
fn full_card_pass() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| std::env::var_os("LEONIDA_FULL_CARD_PASS").is_none_or(|v| v != "0"))
}

/// Whether a texture that has its host copy is still brought up to date
/// while the card has no room for new ones (`LEONIDA_REUPLOAD_WHEN_FULL=0`:
/// no).
fn reupload_when_full() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| std::env::var_os("LEONIDA_REUPLOAD_WHEN_FULL").is_none_or(|v| v != "0"))
}

/// Whether textures picked by index are looked at again after their upload
/// (`LEONIDA_HEAP_RECHECK=0`: no).
/// LOCAL (11/26): changes found by those later looks, since the start.
pub(crate) static HEAP_TEXTURES_LATE: AtomicU64 = AtomicU64::new(0);

/// Leonida: of the changes those looks found, the ones only the spread places
/// of the fingerprint saw (`crate::texture::spread_alone`), since the start.
pub(crate) static HEAP_TEXTURES_SPREAD: AtomicU64 = AtomicU64::new(0);

fn heap_recheck() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| std::env::var_os("LEONIDA_HEAP_RECHECK").is_none_or(|v| v != "0"))
}
/// Times a placed texture's slot was passed over because its memory is not
/// mapped.
pub(crate) static HEAP_TEXTURES_SAME: AtomicU64 = AtomicU64::new(0);
pub(crate) static HEAP_TEXTURES_AWAY: AtomicU64 = AtomicU64::new(0);
/// Slots the pass found up to date already: their texture is uploaded by
/// the draws that read it through a table (it has a staging buffer).
pub(crate) static HEAP_TEXTURES_TABLED: AtomicU64 = AtomicU64::new(0);
/// The queue of textures picked by index at the last pass
/// (`TextureQueue::census`): slots in it, due, waiting for their memory,
/// uploaded in part, and the frames the oldest due one has waited.
pub(crate) static HEAP_TEXTURES_QUEUE: [AtomicU64; 5] = [const { AtomicU64::new(0) }; 5];
/// Since the start: looks at a slot uploaded in part that found no more of
/// its memory mapped; uploads of textures not all mapped; and of those,
/// the ones whose first page was not there (asked by the first page alone,
/// as before, they counted as not there at all).
pub(crate) static HEAP_TEXTURES_PARTLY: [AtomicU64; 3] = [const { AtomicU64::new(0) }; 3];
static DROPPED: AtomicU64 = AtomicU64::new(0);

/// Marks the last traced draw as not run on the host.
#[cfg(feature = "diagnostics")]
fn dropped(pipeline: usize) {
    crate::pipeline::note_drop(pipeline as This);
    if let Some(last) = FRAME.lock().unwrap_or_else(|e| e.into_inner()).last_mut() {
        last.push_str(" DROPPED");
    }
}

/// Dispatches run on the host so far.
static DISPATCHED: AtomicU64 = AtomicU64::new(0);

pub(crate) fn dispatched() -> u64 {
    DISPATCHED.load(Ordering::Relaxed)
}

/// Dispatches of packed programs run on the host so far (`LEONIDA_GROUP_PACK`): direct ones,
/// then indirect ones.
static PACKED: [AtomicU64; 2] = [AtomicU64::new(0), AtomicU64::new(0)];

pub(crate) fn packed() -> [u64; 2] {
    [PACKED[0].load(Ordering::Relaxed), PACKED[1].load(Ordering::Relaxed)]
}

/// What a render target or depth view of the host is made from: the host
/// texture, and [`crate::descriptor::RtvRecord::key`].
type TargetKey = (usize, (usize, [u32; 7]));

/// The host address of a root view: its buffer's host copy, or for a
/// constant buffer not 256-byte aligned there (the console needs 16) a
/// copy in upload memory.
/// shortcut: a root view has no size; constant buffers are taken as 64 KiB
/// (cut at their buffer's end), other views as all that is left of their
/// buffer. (They were taken as 1 MiB: 1,452 of the 2,021 mesh vertex
/// streams of a cutscene frame reach further, and what lay past the first
/// megabyte was never brought up to date - meshes without bodies, exploded
/// triangles. `LEONIDA_ROOT_VIEW_MB` sets a limit again.)
fn root_view(list: &HostList, address: u64, srv: bool) -> Option<u64> {
    static LIMIT: std::sync::OnceLock<u64> = std::sync::OnceLock::new();
    let limit = *LIMIT.get_or_init(|| {
        std::env::var("LEONIDA_ROOT_VIEW_MB")
            .ok()
            .and_then(|v| v.parse::<u64>().ok())
            .map_or(u64::MAX, |mb| mb << 20)
    });
    let shadow = crate::shadow::find(address, 1)?;
    let len = shadow
        .left(address)
        .min(if srv { limit } else { 1 << 16 });
    let gpu = shadow.gpu(address);
    if srv || gpu % 256 == 0 {
        shadow.refresh(list, address, len);
        #[cfg(feature = "diagnostics")]
        crate::book::root(gpu, len);
        return Some(gpu);
    }
    list.fill_scratch(address as *const u8, len)
}

macro_rules! once {
    ($($arg:tt)*) => {{
        static ONCE: std::sync::Once = std::sync::Once::new();
        ONCE.call_once(|| tracing::warn!($($arg)*));
    }};
}

fn set(roots: &mut Vec<Root>, slot: u32, root: Root) {
    let slot = slot as usize;
    if roots.len() <= slot {
        roots.resize(slot + 1, Root::None);
    }
    roots[slot] = root;
}

/// The pass over the buffer views of `heap` that shaders pick by index
/// ([`State::heap_buffers`]): their host slots are made, and the host copies
/// they show are to be refreshed.
fn heap_buffer_pass(heap: &crate::descriptor::Heap, host: &crate::descriptor::host::HostHeap, frame: u64) -> Vec<crate::host::aside::Refresh> {
    let started = std::time::Instant::now();
    // Per host heap: the host copy and range each slot's buffer view
    // reads, kept from pass to pass. Only the slots written since the
    // last pass are looked at again (83,000 slots a pass otherwise, 0.1 s
    // at best); all of them every 16 passes, because a view of a
    // released resource goes stale without a write, and a slot written
    // before its memory had a host copy is only made then.
    let mut kept = host.buffer_sweep.lock().unwrap_or_else(|e| e.into_inner());
    let changed = host.buffer_changes();
    let all = kept.passes == 0 || (kept.passes % HEAP_ROUND == 0 && !heap_spread());
    let slots = if all {
        kept.ranges.clear();
        host.buffer_slots()
    } else if heap_spread() {
        // The written ones, and this pass's part of the others (each once:
        // a written slot of the part was looked at twice).
        let (part, parts) = ((kept.passes % HEAP_ROUND) as u32, HEAP_ROUND as u32);
        kept.ranges.forget_part(part);
        let mut slots = host.buffer_slots_of(part, parts);
        if !changed.is_empty() {
            slots.extend(changed);
            if sweep_spans() {
                slots.sort_unstable();
                slots.dedup();
            }
        }
        slots
    } else {
        changed
    };
    kept.passes += 1;
    // Whether the device was there before the pass: a view that removes
    // it is named below.
    static GONE: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
    let alive = !GONE.load(Ordering::Relaxed) && crate::descriptor::host::device_removed().is_none();
    let mut unordered = Vec::new();
    /// The title's heap, for the threads that look.
    struct Shared<'a>(&'a crate::descriptor::Heap);
    // SAFETY: a heap is free-threaded (its methods are called by any thread of
    // the title's), and it lives until all of them have looked.
    unsafe impl Sync for Shared<'_> {}
    impl Shared<'_> {
        fn handle(&self, index: u32) -> Option<u64> {
            self.0.handle(index)
        }
    }
    let heap = Shared(heap);
    // What a slot of the title's shows: its copy and range, and whether it is
    // an unordered view.
    let look = |index: u32| {
        // A draw's table's: the draw brings what it reads up to date.
        if host.copied_tabled(index, frame) {
            return None;
        }
        let view = host.mirror_for_pass(index, heap.handle(index)?)?;
        // The copy the slot shows (`buffer_copy`), not the one the
        // view's range alone would find.
        let (shadow, address, len) = crate::descriptor::host::buffer_copy(&view)?;
        #[cfg(feature = "diagnostics")]
        if host.shows(index, &shadow.buffer) == Some(false) {
            HEAP_VIEWS_ELSEWHERE.fetch_add(1, Ordering::Relaxed);
        }
        let written = crate::picked::on() && view_kind(&view) == Some(ResourceType::Uav);
        Some(((shadow, address, address + len), written))
    };
    // Some slots at a time, on several threads.
    const AT_A_TIME: usize = 256;
    let parts: Vec<&[u32]> = slots.chunks(AT_A_TIME).collect();
    let found: Vec<std::sync::OnceLock<Vec<_>>> = parts.iter().map(|_| Default::default()).collect();
    crate::land::each_view(false, parts.len(), &|n| {
        let looked: Vec<_> = parts[n].iter().map(|&index| (index, look(index))).collect();
        // (After each piece while the device lives: a cheap query.)
        if alive
            && let Some(why) = crate::descriptor::host::device_removed()
            && !GONE.swap(true, Ordering::Relaxed)
        {
            let last: Vec<String> = parts[n]
                .iter()
                .filter_map(|&i| crate::descriptor::view(heap.handle(i)?).map(|v| format!("{i}: {v:?}")))
                .map(|s| s.chars().take(200).collect())
                .collect();
            tracing::error!(why, "the host device went while buffer views of the heap were made; perhaps one of: {last:#?}");
            crate::host::log_removal_context();
        }
        let _ = found[n].set(looked);
    });
    for (index, shows) in found.into_iter().filter_map(std::sync::OnceLock::into_inner).flatten() {
        let range = shows.map(|(range, written)| {
            if written {
                unordered.push(range.clone());
            }
            range
        });
        kept.ranges.set(index, range);
    }
    if crate::picked::on() {
        crate::picked::shown(unordered, all);
    }
    #[cfg(feature = "diagnostics")]
    let looked = std::time::Instant::now();
    // Per host copy, the span its picked views cover ([`sweep_spans`]).
    let (spans, views) = kept.ranges.spans();
    let bytes: u64 = spans.values().map(|s| s.2 - s.1).sum();
    // Memory that is not write-watched is copied whole by every refresh.
    // It was once refreshed only every 120 frames (unmapped pages read
    // one by one cost 14 GB a frame in run 11, before they were
    // remembered): streamed geometry then drew from stale buffers, and
    // the garbage ended runs (the card stopped answering 14-90 frames
    // into the opening cutscene; with every frame it ran on). The pass
    // costs some 30 ms a frame this way.
    static EVERY: std::sync::OnceLock<u64> = std::sync::OnceLock::new();
    let every = *EVERY.get_or_init(|| {
        std::env::var("LEONIDA_UNWATCHED_FRAMES").ok().and_then(|v| v.parse().ok()).unwrap_or(1)
    });
    let mut found = Vec::with_capacity(spans.len());
    let mut next_unwatched = std::collections::HashMap::new();
    for (shadow, from, to) in spans.values() {
        if every > 1 && shadow.unwatched() {
            let id = shadow.id();
            if let Some(&(last, low, high)) = kept.unwatched.get(&id)
                && frame >= last && frame - last < every && low <= *from && *to <= high
            {
                next_unwatched.insert(id, (last, low, high));
                continue;
            }
            next_unwatched.insert(id, (frame, *from, *to));
        }
        found.push((shadow.clone(), *from, (*to).min(*from + shadow.left(*from))));
    }
    kept.unwatched = next_unwatched;
    drop(kept);
    #[cfg(feature = "diagnostics")]
    let refreshed = std::time::Instant::now();
    crate::watch::look_at_holes();
    #[cfg(feature = "diagnostics")]
    for (sum, time) in HEAP_SPLIT.iter().zip([looked - started, refreshed - looked]) {
        sum.fetch_add(time.as_nanos() as u64, Ordering::Relaxed);
    }
    static LOGGED: AtomicU64 = AtomicU64::new(0);
    // The first pass and every growth of the set by half.
    let seen = LOGGED.load(Ordering::Relaxed);
    if u64::from(views) > seen + seen / 2 {
        LOGGED.store(u64::from(views), Ordering::Relaxed);
        tracing::info!(
            slots = slots.len(),
            views,
            copies = spans.len(),
            mb = bytes >> 20,
            ms = started.elapsed().as_millis(),
            "buffer views of the heap made ready for shaders that pick them by index"
        );
    }
    found
}

impl State {
    pub(crate) fn reset(&mut self, pipeline: This) {
        *self = State::default();
        self.pipeline = pipeline as usize;
    }

    /// The title's topology; console-only ones become host lists (see
    /// [`crate::primitive`]).
    pub(crate) fn set_topology(&mut self, list: &HostList, topology: u32) {
        self.topology = topology;
        self.topology_set = true;
        let host = match topology {
            RECTANGLES => 4,
            TRIANGLE_FAN | LINE_LOOP | QUADS | QUAD_STRIP => {
                once!(topology, "console topology drawn as a triangle list");
                4
            }
            t => t,
        };
        // SAFETY: an open list.
        unsafe {
            list.list
                .IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY(host as i32))
        };
    }

    pub(crate) fn set_pipeline(&mut self, pipeline: This) {
        self.pipeline = pipeline as usize;
    }

    pub(crate) fn set_heaps(&mut self, heaps: &[This]) {
        self.heaps = [0; 2];
        for &h in heaps {
            if !h.is_null() {
                let samplers = crate::descriptor::heap(h).samplers();
                self.heaps[usize::from(samplers)] = h as usize;
            }
        }
    }

    fn roots(&mut self, compute: bool) -> &mut Vec<Root> {
        if compute {
            &mut self.compute
        } else {
            &mut self.graphics
        }
    }

    pub(crate) fn set_table(&mut self, compute: bool, slot: u32, handle: u64) {
        set(self.roots(compute), slot, Root::Table(handle));
    }

    pub(crate) fn set_view(&mut self, compute: bool, slot: u32, address: u64) {
        set(self.roots(compute), slot, Root::View(address));
    }

    pub(crate) fn set_constants(&mut self, compute: bool, slot: u32, values: &[u32], offset: u32) {
        let roots = self.roots(compute);
        let s = slot as usize;
        if !matches!(roots.get(s), Some(Root::Constants(_))) {
            set(roots, slot, Root::Constants(Vec::new()));
        }
        if let Root::Constants(c) = &mut roots[s] {
            let end = offset as usize + values.len();
            if c.len() < end {
                c.resize(end, 0);
            }
            c[offset as usize..end].copy_from_slice(values);
        }
    }

    pub(crate) fn set_vertex_buffers(&mut self, start: u32, views: &[VertexView]) {
        for (i, v) in views.iter().enumerate() {
            if let Some(slot) = self.vertex.get_mut(start as usize + i) {
                *slot = *v;
            }
        }
    }

    pub(crate) fn clear_vertex_buffers(&mut self, start: u32, count: u32) {
        for slot in self.vertex.iter_mut().skip(start as usize).take(count as usize) {
            *slot = VertexView::default();
        }
    }

    pub(crate) fn set_index_buffer(&mut self, view: Option<&IndexView>) {
        self.index = view.copied().unwrap_or_default();
    }

    /// Render targets and depth, set on the host list at once, unless it has
    /// the same ones already: the title sets the same targets again and
    /// again, each time a host view per target and a call of the driver
    /// (25,662 times in a frame of another fork). `LEONIDA_BIND_CACHE=0`
    /// sets them every time.
    pub(crate) fn set_targets(&mut self, list: &HostList, handles: &[u64], depth: u64) {
        let Some(host) = crate::host::host() else {
            return;
        };
        self.target_handles.clear();
        self.target_handles.extend_from_slice(handles);
        self.depth_handle = depth;
        self.targets_set = true;
        self.targets.clear();
        let mut views = Vec::with_capacity(handles.len());
        let mut keys = Vec::with_capacity(handles.len());
        // The title resource of each view in `views` (0: none), for frame generation below.
        let mut titles: Vec<usize> = Vec::with_capacity(handles.len());
        for &h in handles {
            let Some(record) = crate::descriptor::rtv_record(h) else {
                once!(
                    handle = format_args!("{h:#x}"),
                    "render target without a recorded view"
                );
                continue;
            };
            let Some(texture) = crate::resource::host_resource(record.resource) else {
                // A typed null view (or a target without a host copy) still
                // takes its slot: left out, the next live target would move
                // into it and the pixel shader's outputs reach the wrong
                // targets.
                // shortcut: a view without a description or a format has no
                // null view to stand for it and is left out, as before.
                let null = record
                    .desc()
                    .filter(|d| d.Format.0 != 0 && d.ViewDimension.0 != 0);
                if let Some(desc) = null {
                    views.push((None, Some(desc)));
                    keys.push((0, record.key()));
                    titles.push(0);
                }
                continue;
            };
            self.targets.push(record.resource as usize);
            titles.push(record.resource as usize);
            #[cfg(feature = "diagnostics")]
            crate::capture::note_target(record.resource);
            views.push((Some(texture), record.desc()));
            keys.push((texture.as_raw() as usize, record.key()));
        }
        self.depth = 0;
        self.depth_state = DEPTH_WRITE;
        let dsv = crate::descriptor::dsv_record(depth).and_then(|record| {
            let texture = crate::resource::host_resource(record.resource)?;
            self.depth = record.resource as usize;
            self.depth_state = depth_state(texture, record.depth_desc().as_ref());
            if crate::resource::depth_and_unordered(record.resource) {
                crate::resource::DEPTH_UNORDERED[1].fetch_add(1, Ordering::Relaxed);
            }
            #[cfg(feature = "diagnostics")]
            crate::capture::note_target(record.resource);
            Some((texture, record))
        });
        // The formats of the host views, for the draws' pipelines (before
        // the bind cache below, which can return early).
        let mut formats = [0u32; 9];
        for (i, (texture, desc)) in views.iter().take(8).enumerate() {
            formats[i] = bound_format(*texture, desc.as_ref().map(|d| d.Format));
        }
        if let Some((texture, record)) = dsv.as_ref() {
            formats[8] = bound_format(Some(*texture), record.depth_desc().as_ref().map(|d| d.Format));
        }
        self.target_formats = formats;
        // Frame generation where the title upscales by itself: the G-buffer pass's depth and velocity
        // (`crate::fg::gbuffer`). The pass is the one with four or more targets, one of them two 16-bit floats
        // (DXGI format 34), and a depth target of that size.
        const VELOCITY: u32 = 34;
        if crate::fg::active() && views.len() >= 4 && self.depth != 0 {
            let velocity = (0..views.len().min(8)).find(|&i| formats[i] == VELOCITY && titles[i] != 0);
            if let (Some(i), Some((depth, _))) = (velocity, dsv.as_ref())
                && let Some(motion) = views[i].0
            {
                // SAFETY: live host resources.
                let (m, d) = unsafe { (motion.GetDesc(), depth.GetDesc()) };
                if (m.Width, m.Height) == (d.Width, d.Height) {
                    crate::fg::gbuffer(self.depth as u64, titles[i] as u64, [m.Width as u32, m.Height]);
                }
            }
        }
        let now = (keys, dsv.as_ref().map(|(texture, record)| (texture.as_raw() as usize, record.key())));
        static CACHE: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
        if *CACHE.get_or_init(|| std::env::var("LEONIDA_BIND_CACHE").map_or(true, |v| v != "0"))
            && self.host_targets.as_ref() == Some(&now)
        {
            return;
        }
        self.host_targets = Some(now);
        let rtvs: Vec<_> = views.iter().map(|(texture, desc)| host.rtv(*texture, desc.as_ref())).collect();
        let dsv = dsv.map(|(texture, record)| host.dsv(texture, record.depth_desc().as_ref()));
        // SAFETY: an open list; the views were just made.
        unsafe {
            list.list.OMSetRenderTargets(
                rtvs.len() as u32,
                (!rtvs.is_empty()).then_some(rtvs.as_ptr()),
                false,
                dsv.as_ref().map(std::ptr::from_ref),
            )
        };
    }

    /// Viewports, scissors, the blend factor and the stencil reference: set
    /// on the host list at once, and kept for [`State::replay`].
    pub(crate) fn set_viewports(&mut self, list: &HostList, viewports: &[D3D12_VIEWPORT]) {
        #[cfg(feature = "diagnostics")]
        if let Some(v) = viewports.first() {
            crate::host::spans::viewport(v.Width as u32, v.Height as u32);
        }
        self.viewports.clear();
        self.viewports.extend_from_slice(viewports);
        crate::draw::viewports(list, viewports);
    }

    pub(crate) fn set_scissors(&mut self, list: &HostList, rects: &[RECT]) {
        self.scissors.clear();
        self.scissors.extend_from_slice(rects);
        if scissor_test() {
            // The experiment: every rectangle the title sets is widened to
            // the largest target there is, so nothing is cut by one. What
            // the listing shows (`op_text`) stays the title's own.
            let full = RECT {
                left: 0,
                top: 0,
                right: 16384,
                bottom: 16384,
            };
            crate::draw::scissors(list, &vec![full; rects.len()]);
            return;
        }
        crate::draw::scissors(list, rects);
    }

    pub(crate) fn set_blend_factor(&mut self, list: &HostList, factor: Option<&[f32; 4]>) {
        self.blend = factor.copied();
        crate::draw::blend_factor(list, factor);
    }

    pub(crate) fn set_stencil_ref(&mut self, list: &HostList, reference: u32) {
        self.stencil = Some(reference);
        crate::draw::stencil_ref(list, reference);
    }

    pub(crate) fn set_depth_bounds(&mut self, list: &HostList, min: f32, max: f32) {
        self.depth_bounds = Some([min, max]);
        crate::draw::depth_bounds(list, min, max);
    }

    /// `list` is a new host list that continues the title's list (it was
    /// cut at a wait for a memory value, `crate::list::cut`): a new host
    /// list has nothing set, so what the title set so far is set again.
    /// The pipeline, the root signatures and the heaps are set by the next
    /// draw or dispatch (`bind`, which is told here that the host list has
    /// none); root arguments, vertex and index buffers are set by every
    /// draw anyway. The dynamic descriptor slots and the tables made in
    /// them stay: the parts before the cut have not run yet, and they are
    /// given back when the title's list is reset.
    /// shortcut: the render target views are made again from the handles'
    /// descriptors as they are now, not as they were when the title set
    /// the targets (a descriptor rewritten in between names another view).
    pub(crate) fn replay(&mut self, list: &HostList) {
        // Another host list, or one a pass of ours wrote in (an upscale).
        self.waited = false;
        self.host_pipeline = 0;
        self.host_roots = [0; 2];
        self.host_heaps = [0; 2];
        self.host_heaps_set = false;
        self.cache.clear();
        if self.topology_set {
            self.set_topology(list, self.topology);
        }
        self.host_targets = None;
        if self.targets_set {
            let handles = std::mem::take(&mut self.target_handles);
            self.set_targets(list, &handles, self.depth_handle);
        }
        if !self.viewports.is_empty() {
            crate::draw::viewports(list, &self.viewports);
        }
        if !self.scissors.is_empty() {
            let rects = std::mem::take(&mut self.scissors);
            self.set_scissors(list, &rects);
        }
        if let Some(factor) = self.blend {
            crate::draw::blend_factor(list, Some(&factor));
        }
        if let Some(reference) = self.stencil {
            crate::draw::stencil_ref(list, reference);
        }
        if let Some([min, max]) = self.depth_bounds {
            crate::draw::depth_bounds(list, min, max);
        }
    }

    /// The user-data value of a root argument, as translated shaders read
    /// it: a table's byte offset in its heap, a constant's value.
    fn user_value(&self, compute: bool, slot: u8, root: RootType, dword: u32) -> u32 {
        match (self.roots_ref(compute).get(usize::from(slot)), root) {
            (Some(Root::Table(handle)), RootType::Table) => match self.heap_of(*handle) {
                Some(heap) => (handle - heap.start) as u32,
                None => {
                    once!(
                        handle = format_args!("{handle:#x}"),
                        "descriptor table outside the bound heaps"
                    );
                    0
                }
            },
            (Some(Root::Constants(c)), RootType::Constants) => {
                c.get(dword as usize).copied().unwrap_or(0)
            }
            // The host's own ([`Self::host_word`]): no root argument.
            (_, RootType::Other(_)) => 0,
            _ => {
                once!(
                    slot,
                    ?root,
                    "a shader reads a root argument the title did not set"
                );
                0
            }
        }
    }

    /// A user-data dword the host computes for a translated shader
    /// (`leonida_shader::emit::USER_MEMORY_BASE`, `USER_VIEW_ADDRESS`).
    fn host_word(
        &self,
        compute: bool,
        kind: u32,
        slot: u8,
        dword: u32,
        memory: Option<&crate::memory::Ready>,
    ) -> u32 {
        use leonida_shader::emit::{USER_MEMORY_BASE, USER_ROOT_ADDRESS, USER_VIEW_ADDRESS, USER_VIEW_BIAS};
        if kind == USER_MEMORY_BASE {
            return memory.map_or(0, |m| m.base);
        }
        // All of every group, unless the dispatch says otherwise.
        if kind == leonida_shader::emit::USER_DISPATCH_THREADS_X {
            return 0;
        }
        // The console grid of a packed program: the dispatch sets it (`pack_of`).
        if is_pack_word(kind) {
            return 0;
        }
        if kind == USER_ROOT_ADDRESS || kind == USER_ROOT_ADDRESS + 1 {
            return match self.roots_ref(compute).get(usize::from(slot)) {
                Some(&Root::View(address)) if kind == USER_ROOT_ADDRESS => address as u32,
                Some(&Root::View(address)) => (address >> 32) as u32,
                _ => 0,
            };
        }
        if kind == USER_VIEW_BIAS {
            // The view begins on 16 bytes of the copy the address finds
            // (`root_uav`, `root_srv_table`).
            // shortcut: found again here, not taken from the list's slot.
            return match self.roots_ref(compute).get(usize::from(slot)) {
                Some(&Root::View(address)) => crate::shadow::find(address, 1).map_or(0, |s| {
                    // SAFETY: a live buffer.
                    let base = unsafe { s.buffer.GetGPUVirtualAddress() };
                    ((s.gpu(address) - base) & 15) as u32
                }),
                _ => 0,
            };
        }
        if kind != USER_VIEW_ADDRESS && kind != USER_VIEW_ADDRESS + 1 {
            once!(kind, "a shader reads a host value this build does not know");
            return 0;
        }
        let address = self.view_address(compute, slot, dword);
        if address == 0 {
            // The first few say what lies there instead.
            static SAID: AtomicU64 = AtomicU64::new(0);
            if SAID.fetch_add(1, Ordering::Relaxed) < 8 {
                let view = match self.roots_ref(compute).get(usize::from(slot)) {
                    Some(&Root::Table(table)) => crate::descriptor::view(table + u64::from(dword) * 4),
                    _ => None,
                };
                tracing::warn!(slot, dword, ?view, "a shader takes the address of a buffer view that is not there: it reads 0");
            }
        }
        if kind == USER_VIEW_ADDRESS {
            address as u32
        } else {
            (address >> 32) as u32
        }
    }

    /// The title address the view at dword `dword` of the table in root
    /// slot `slot` starts at, 0 without one.
    fn view_address(&self, compute: bool, slot: u8, dword: u32) -> u64 {
        match self.roots_ref(compute).get(usize::from(slot)) {
            Some(&Root::Table(table)) => crate::descriptor::view(table + u64::from(dword) * 4)
                .and_then(|view| crate::descriptor::host::view_address(&view))
                .unwrap_or(0),
            _ => 0,
        }
    }

    /// For a shader that reads title memory by address
    /// ([`crate::memory`]): its directory, fault notes and view slots,
    /// with the pages made ready of the root views the list has set and of
    /// the views whose addresses it takes (`t`: the ray-tracing structures
    /// it walks).
    fn memory_ready(
        &self,
        list: &HostList,
        t: &crate::pipeline::Translated,
        compute: bool,
        use_: crate::memory::Use,
    ) -> Option<crate::memory::Ready> {
        let heap = match self.heaps[0] {
            0 => None,
            h => crate::descriptor::heap(h as This).host(),
        }?;
        let (mut addresses, mut constants) = (Vec::new(), Vec::new());
        // (What it may write: its root views of unordered access.)
        let written: Vec<u64> = t
            .used
            .iter()
            .filter(|u| u.kind == ResourceType::Uav && u.root == RootType::Descriptor)
            .filter_map(|u| match self.roots_ref(compute).get(usize::from(u.slot)) {
                Some(&Root::View(address)) => Some(address),
                _ => None,
            })
            .collect();
        for root in self.roots_ref(compute) {
            match root {
                // (Of a program that only walks structures, the structures'
                // views below: its root views are constants and tables.)
                Root::View(address) if use_ != crate::memory::Use::Reads => addresses.push(*address),
                Root::Constants(values) => constants.extend_from_slice(values),
                _ => {}
            }
        }
        for u in &t.host.user_data {
            if u.root == RootType::Other(leonida_shader::emit::USER_VIEW_ADDRESS) {
                addresses.push(self.view_address(compute, u.slot, u.dword));
            }
        }
        crate::memory::ready(list, heap, &addresses, &written, &constants, use_, t.threads)
    }

    /// The bound heap holding `handle`.
    fn heap_of(&self, handle: u64) -> Option<&'static crate::descriptor::Heap> {
        self.heaps
            .iter()
            .filter(|&&h| h != 0)
            .map(|&h| crate::descriptor::heap(h as This))
            .find(|h| h.index(handle).is_some())
    }

    /// Brings the host list up to date for a draw; false if the draw cannot
    /// run on the host.
    fn prepare(&mut self, list: &HostList, indexed: bool) -> bool {
        self.prepare_for(list, indexed, false)
    }

    /// As [`State::prepare`], for draws (`mesh` false) or mesh dispatches.
    fn prepare_for(&mut self, list: &HostList, indexed: bool, mesh: bool) -> bool {
        // Why it does not run, if it does not (`crate::drops`).
        crate::drops::begin();
        #[cfg(feature = "diagnostics")]
        if crate::pipeline::is_skipped(self.pipeline as This) {
            crate::drops::note(Why::SkipList);
            return false;
        }
        let Some(p) = crate::pipeline::host_pipeline_for(self.pipeline as This, &self.target_formats) else {
            crate::drops::note(crate::pipeline::why_gone(self.pipeline as This));
            return false;
        };
        if p.compute || p.mesh != mesh {
            once!(mesh, "a draw or mesh dispatch with another kind of pipeline");
            return false;
        }
        if p.rects && self.topology != RECTANGLES {
            // Its geometry stage would turn each triangle into a rectangle.
            once!("a rectangle pipeline drawn with another topology");
        }
        if !self.bind(list, p, false) {
            crate::drops::note(Why::BindFailed);
            return false;
        }
        let l = &list.list;
        // Vertex buffers the input layout reads.
        let mut views = [D3D12_VERTEX_BUFFER_VIEW::default(); 32];
        let mut count = 0;
        for (slot, (view, &v)) in views.iter_mut().zip(&self.vertex).enumerate() {
            if p.slots & 1 << slot == 0 {
                continue;
            }
            count = slot + 1;
            if v.address == 0 || v.size == 0 {
                continue;
            }
            let Some(shadow) = crate::shadow::find_any(v.address, u64::from(v.size)) else {
                continue;
            };
            shadow.refresh(list, v.address, u64::from(v.size));
            *view = D3D12_VERTEX_BUFFER_VIEW {
                BufferLocation: shadow.gpu(v.address),
                SizeInBytes: v.size.min(u32::try_from(shadow.left(v.address)).unwrap_or(u32::MAX)),
                StrideInBytes: v.stride,
            };
        }
        if count > 0 {
            let mut keys = [(0u64, 0u32, 0u32); 32];
            for (key, view) in keys.iter_mut().zip(&views[..count]) {
                *key = (view.BufferLocation, view.SizeInBytes, view.StrideInBytes);
            }
            if self.cache.vertex(&keys[..count]) {
                // SAFETY: an open list; the views name live host buffers.
                unsafe { l.IASetVertexBuffers(0, Some(&views[..count])) };
            }
        }
        if indexed {
            let v = self.index;
            let Some(shadow) = crate::shadow::find_any(v.address, u64::from(v.size)) else {
                return false;
            };
            shadow.refresh(list, v.address, u64::from(v.size));
            let view = D3D12_INDEX_BUFFER_VIEW {
                BufferLocation: shadow.gpu(v.address),
                SizeInBytes: v.size.min(u32::try_from(shadow.left(v.address)).unwrap_or(u32::MAX)),
                Format: DXGI_FORMAT(v.format as i32),
            };
            if self.cache.index((view.BufferLocation, view.SizeInBytes, view.Format.0)) {
                // SAFETY: an open list; the view names a live host buffer.
                unsafe { l.IASetIndexBuffer(Some(&view)) };
            }
        }
        const RENDER_TARGET: u32 = 0x4;
        for &t in &self.targets {
            crate::list::ensure_state(list, t as This, RENDER_TARGET);
        }
        if self.depth != 0 {
            // A depth texture shaders also write: what they wrote, first.
            list.twin_use(self.depth as This, false, true);
            // A view that writes neither plane keeps the texture readable:
            // the depth read state, together with the shader read states
            // if this draw's shaders read the same texture (the write state
            // put it out of the state those views need, and a move to it
            // and back for every such draw).
            let state = if self.depth_state == DEPTH_READ {
                let (tested, read) = TESTED_DEPTH.get();
                if read && tested == self.depth {
                    DEPTH_READ | SHADER_RESOURCE
                } else {
                    DEPTH_READ
                }
            } else {
                DEPTH_WRITE
            };
            crate::list::ensure_state(list, self.depth as This, state);
        }
        true
    }

    /// A descriptor a shader reads through a table: its host slot made if
    /// it changed, constant data copied, textures made readable.
    /// What draws and dispatches share: heaps, our root signature, the
    /// pipeline, and per stage the user data, root views and the
    /// descriptors its tables hold.
    /// False if the draw or dispatch must not run: a root SRV it reads
    /// could not be given a view.
    pub(crate) fn bind(&mut self, list: &HostList, p: &HostPipeline, compute: bool) -> bool {
        let l = &list.list;
        // What is drawn or dispatched with it may write through unordered
        // access: the title's next wait for shader writes is one
        // ([`State::wait_for_writes`]).
        if compute || p.stages.iter().any(|stage| stage.translated().writes) {
            self.waited = false;
        }
        // (Clears the cache if the switch changed since the last call.)
        self.cache.sync();
        // Textures picked by index, before anything asks for them.
        self.early_textures(list, p, compute);
        if crate::host::walls_trace() {
            self.walls_census(p, compute);
        }
        // A list without a view heap: one of the title's for the views the
        // host itself puts into heap slots.
        if self.heaps[0] == 0
            && p.stages.iter().any(|s| !s.root_uavs.is_empty() || !s.root_srvs.is_empty())
            && let Some(heap) = crate::descriptor::lent_heap()
        {
            once!("a dispatch without a view heap that needs one gets the title's");
            self.heaps[0] = heap;
        }
        // Heaps first: a root signature with directly indexed heaps must be
        // set after them, with both a view heap and a sampler heap bound.
        if !self.host_heaps_set || self.host_heaps != self.heaps {
            let mut view_heap: Option<ID3D12DescriptorHeap> = None;
            let mut sampler_heap: Option<ID3D12DescriptorHeap> = None;
            for &h in &self.heaps {
                if h == 0 {
                    continue;
                }
                let title_heap = crate::descriptor::heap(h as This);
                if let Some(host_heap) = title_heap.host() {
                    if title_heap.samplers() {
                        sampler_heap = Some(host_heap.heap.clone());
                    } else {
                        view_heap = Some(host_heap.heap.clone());
                    }
                }
            }
            if view_heap.is_none() {
                view_heap = crate::host::host().and_then(|h| h.fallback_view_heap().cloned());
            }
            if sampler_heap.is_none() {
                sampler_heap = crate::host::host().and_then(|h| h.fallback_sampler_heap().cloned());
            }
            let mut heaps: Vec<Option<ID3D12DescriptorHeap>> = Vec::with_capacity(2);
            if let Some(h) = view_heap {
                list.keep(&h);
                heaps.push(Some(h));
            }
            if let Some(h) = sampler_heap {
                list.keep(&h);
                heaps.push(Some(h));
            }
            if !heaps.is_empty() {
                // SAFETY: an open list and shader-visible heaps.
                unsafe { l.SetDescriptorHeaps(&heaps) };
            }
            self.host_heaps = self.heaps;
            self.host_heaps_set = true;
            self.host_roots = [0; 2];
            self.cache.heaps_changed();
        }
        // The depth target a draw only tests, for the views of it that the
        // draw's shaders read (`readable`).
        let tested = if !compute && self.depth_state == DEPTH_READ {
            self.depth
        } else {
            0
        };
        TESTED_DEPTH.set((tested, false));
        let root = p.root.as_raw() as usize;
        if self.host_roots[usize::from(compute)] != root {
            // SAFETY: an open list and a live root signature.
            unsafe {
                if compute {
                    l.SetComputeRootSignature(&p.root);
                } else {
                    l.SetGraphicsRootSignature(&p.root);
                }
            }
            self.host_roots[usize::from(compute)] = root;
            // A root signature set drops the arguments of its side.
            self.cache.side_changed(compute);
        }
        // By host object: a title pipeline released and another made at its
        // address is another host pipeline (the list keeps the old one).
        if self.host_pipeline != p.pso.as_raw() as usize {
            // SAFETY: an open list and a live pipeline.
            unsafe { l.SetPipelineState(&p.pso) };
            self.host_pipeline = p.pso.as_raw() as usize;
            // The title may release the pipeline before it submits the list.
            list.bind_pipeline(&p.name, &p.pso, &p.root);
        }
        for stage in &p.stages {
            let t = stage.translated();
            STORES.set(t.stores);
            if t.stores && crate::picked::on() && crate::picked::picks(&stage.shader, &t.picks) {
                crate::picked::runs_in(list);
            }
            let made = self.tables(list, &t.tables, &t.host.half_views, compute);
            STORES.set(true);
            if made.iter().any(|m| m.1 == u32::MAX) {
                once!("a draw whose descriptor table found no free slots is dropped");
                return false;
            }
            if stage.shader.ambient.is_some() {
                self.ambient_tables.clear();
                self.ambient_tables.extend(made.iter().copied());
            }
            // What the shader stores through root views and raw addresses is
            // copied back into title memory afterwards.
            let writes = (t.stores || !write_skip())
                && !crate::pipeline::builder_quarantined(self.pipeline as This);
            // Title memory by address: what the shader finds it by.
            let memory = if t.host.root_srvs.contains(&leonida_shader::emit::SLOT_MEMORY) {
                // Only through a store by address: what it stores through
                // views is declared with them.
                let use_ = if crate::pipeline::is_builder(self.pipeline as This) || writes && t.stores_by_address {
                    crate::memory::Use::Builds { back: writes }
                } else {
                    crate::memory::Use::Reads
                };
                let Some(ready) = self.memory_ready(list, &t, compute, use_) else {
                    once!("a shader that reads title memory by address without a view heap to hold its views: the draw is dropped");
                    return false;
                };
                Some(ready)
            } else {
                None
            };
            if let Some((param, dwords)) = stage.user_data {
                // (A root signature holds 64 dwords.)
                let mut room = [0u32; 64];
                let mut more = Vec::new();
                let values: &mut [u32] = match room.get_mut(..dwords as usize) {
                    Some(values) => values,
                    None => {
                        more.resize(dwords as usize, 0);
                        &mut more
                    }
                };
                for u in &t.host.user_data {
                    if let RootType::Other(kind) = u.root {
                        values[u.index as usize] =
                            self.host_word(compute, kind, u.slot, u.dword, memory.as_ref());
                        continue;
                    }
                    let fresh = made
                        .iter()
                        .find(|m| m.0 == u.slot && u.root == RootType::Table);
                    values[u.index as usize] = match fresh {
                        Some(&(_, offset)) => offset,
                        None => self.user_value(compute, u.slot, u.root, u.dword),
                    };
                }
                #[cfg(feature = "diagnostics")]
                {
                    self.user.clear();
                    self.user.extend_from_slice(values);
                }
                if self.cache.constants(compute, param, values) {
                    // SAFETY: an open list; `values` covers the parameter.
                    unsafe {
                        if compute {
                            l.SetComputeRoot32BitConstants(param, dwords, values.as_ptr().cast(), 0);
                        } else {
                            l.SetGraphicsRoot32BitConstants(param, dwords, values.as_ptr().cast(), 0);
                        }
                    }
                }
            }
            for (views, srv) in [(&stage.root_cbvs, false), (&stage.root_srvs, true)] {
                for (at, &(slot, param)) in views.iter().enumerate() {
                    if srv && slot == leonida_shader::emit::SLOT_MEMORY {
                        // Ours: the directory of title memory by address.
                        if let Some(m) = &memory {
                            // An open list; the directory lives as long as
                            // the process and the list holds it.
                            self.put_view(l, compute, crate::rootcache::Kind::Srv, param, m.directory);
                        }
                        continue;
                    }
                    if srv && slot == leonida_shader::emit::SLOT_STRIDES {
                        // Ours, not the title's: the shader picks buffers
                        // from the view heap by an index it computes.
                        match self.heap_buffers(list) {
                            // An open list; the table lives as long as the
                            // heap the list holds.
                            Some(gpu) => self.put_view(l, compute, crate::rootcache::Kind::Srv, param, gpu),
                            None => once!("a shader picks buffers from a view heap that is not set"),
                        }
                        continue;
                    }
                    if srv && slot == leonida_shader::emit::SLOT_TABLE_STRIDES {
                        // Ours: the shader inspects typed view kinds for its
                        // descriptor tables only (already mirrored by `tables`),
                        // so bind the strides buffer without sweeping the heap.
                        match self.heap_strides(list) {
                            // An open list; the table lives as long as the
                            // heap the list holds.
                            Some(gpu) => self.put_view(l, compute, crate::rootcache::Kind::Srv, param, gpu),
                            None => once!("a shader inspects table view kinds without a view heap set"),
                        }
                        continue;
                    }
                    let tabled = srv && crate::pipeline::host::bounded_root_srvs();
                    let Some(&Root::View(address)) = self.roots_ref(compute).get(usize::from(slot))
                    else {
                        once!(slot, "a shader reads a root view the title did not set");
                        // A table left unset is undefined: a null view.
                        if tabled && !self.root_srv_table(list, None, param, compute, false) {
                            return false;
                        }
                        continue;
                    };
                    // The shader adds the bytes a raw view begins early by
                    // (known with the pipeline, not asked of its user data
                    // at every draw).
                    let biased = srv && stage.root_srv_biased(at);
                    if tabled {
                        if !self.root_srv_table(list, Some(address), param, compute, biased) {
                            return false;
                        }
                        continue;
                    }
                    let Some(mut gpu) = root_view(list, address, srv) else {
                        continue;
                    };
                    if biased {
                        gpu -= u64::from(self.host_word(
                            compute,
                            leonida_shader::emit::USER_VIEW_BIAS,
                            slot,
                            0,
                            None,
                        ));
                    }
                    // An open list; the address is in live memory.
                    self.put_view(l, compute, if srv { crate::rootcache::Kind::Srv } else { crate::rootcache::Kind::Cbv }, param, gpu);
                }
            }
            for &(slot, param) in &stage.root_uavs {
                if slot == leonida_shader::emit::SLOT_FAULTS {
                    // Ours: where shaders note the pages they found empty.
                    if let Some(m) = &memory {
                        // An open list; the buffer lives as long as the
                        // process and the list holds it.
                        self.put_view(l, compute, crate::rootcache::Kind::Uav, param, m.faults);
                    }
                    continue;
                }
                let address = match self.roots_ref(compute).get(usize::from(slot)) {
                    Some(&Root::View(address)) => Some(address),
                    _ => {
                        once!(slot, "a shader writes through a root view the title did not set");
                        None
                    }
                };
                if !self.root_uav(list, address, param, compute, writes) {
                    return false;
                }
            }
        }
        // Depth textures it writes through unordered access: their colour
        // twins hold what they lack from here on.
        list.twins_written();
        true
    }

    /// The clamp of indirect records ran on the list (`crate::indirect`):
    /// it set a compute root signature and pipeline of its own, so the
    /// compute side has no root arguments the cache knows and the next
    /// bind sets its pipeline and root signature again. The graphics side
    /// stays as it was.
    fn clamped(&mut self) {
        self.waited = false;
        self.host_pipeline = 0;
        self.host_roots[1] = 0;
        self.cache.side_changed(true);
    }

    /// Sets a root view (`kind`: constant buffer, shader resource or
    /// unordered access), unless the host list has that address there.
    fn put_view(&mut self, l: &ID3D12GraphicsCommandList, compute: bool, kind: crate::rootcache::Kind, param: u32, gpu: u64) {
        use crate::rootcache::Kind;
        if !self.cache.descriptor(compute, kind, param, gpu) {
            return;
        }
        // SAFETY: an open list; the caller's address is in live memory.
        unsafe {
            match (compute, kind) {
                (false, Kind::Cbv) => l.SetGraphicsRootConstantBufferView(param, gpu),
                (false, Kind::Srv) => l.SetGraphicsRootShaderResourceView(param, gpu),
                (false, _) => l.SetGraphicsRootUnorderedAccessView(param, gpu),
                (true, Kind::Cbv) => l.SetComputeRootConstantBufferView(param, gpu),
                (true, Kind::Srv) => l.SetComputeRootShaderResourceView(param, gpu),
                (true, _) => l.SetComputeRootUnorderedAccessView(param, gpu),
            }
        }
    }

    /// Sets a root descriptor table, unless the host list has it there.
    fn put_table(&mut self, l: &ID3D12GraphicsCommandList, compute: bool, param: u32, handle: D3D12_GPU_DESCRIPTOR_HANDLE) {
        if !self.cache.descriptor(compute, crate::rootcache::Kind::Table, param, handle.ptr) {
            return;
        }
        // SAFETY: an open list; the handle is in the bound view heap.
        unsafe {
            if compute {
                l.SetComputeRootDescriptorTable(param, handle);
            } else {
                l.SetGraphicsRootDescriptorTable(param, handle);
            }
        }
    }

    /// Binds a root UAV: a raw unordered access view of the title buffer's
    /// host copy from `address` on, as a one-view table (a null view
    /// without an address, without a host copy shaders can write, or at an
    /// address not on 16 bytes), or as a root descriptor with
    /// `LEONIDA_ROOT_SRV_DIRECT=1` (the dispatch is dropped then if there
    /// is nothing to bind). With `writes`, what the shader stored is copied
    /// back into title memory when the list has run. False if the draw
    /// must be dropped.
    /// shortcut: a root view has no size: the view, the refresh and the
    /// copy back cover all that is left of the title's buffer
    /// (`LEONIDA_ROOT_UAV_MB` sets a limit for all three).
    fn root_uav(
        &mut self,
        list: &HostList,
        address: Option<u64>,
        param: u32,
        compute: bool,
        writes: bool,
    ) -> bool {
        const UNORDERED: i32 = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS.0;
        static LIMIT: std::sync::OnceLock<u64> = std::sync::OnceLock::new();
        let limit = *LIMIT.get_or_init(|| {
            std::env::var("LEONIDA_ROOT_UAV_MB")
                .ok()
                .and_then(|v| v.parse::<u64>().ok())
                .map_or(u64::MAX, |mb| mb << 20)
        });
        let find = |a: u64| {
            let s = crate::shadow::find(a, 1)?;
            // SAFETY: a live buffer.
            let flags = unsafe { s.buffer.GetDesc() }.Flags;
            if flags.0 & UNORDERED == 0 {
                once!("a root UAV of a buffer whose host copy shaders cannot write");
                return None;
            }
            Some(s)
        };
        let ready = |a: u64, s: &std::sync::Arc<crate::shadow::Shadow>| {
            let len = s.left(a).min(limit);
            s.refresh(list, a, len);
            if writes {
                #[cfg(feature = "diagnostics")]
                crate::host::wide_write(a, len, self.pipeline);
                s.gpu_writes(list, a, len);
            }
        };
        if !crate::pipeline::host::bounded_root_srvs() {
            let Some((a, s)) = address.and_then(|a| Some((a, find(a)?))) else {
                once!("a root UAV with nothing to bind: the draw is dropped");
                return false;
            };
            ready(a, &s);
            // From the 16 bytes below: the shader adds the rest.
            // SAFETY: a live buffer.
            let base = unsafe { s.buffer.GetGPUVirtualAddress() };
            let gpu = base + ((s.gpu(a) - base) & !15);
            // An open list; the address is in a live buffer.
            self.put_view(&list.list, compute, crate::rootcache::Kind::Uav, param, gpu);
            return true;
        }
        let heap = match self.heaps[0] {
            0 => None,
            h => Some(crate::descriptor::heap(h as This)),
        };
        let Some((heap, host)) = heap.and_then(|h| Some((h, h.host()?))) else {
            once!("a root UAV without a bound view heap: the draw is dropped");
            return false;
        };
        let key = (std::ptr::from_ref(heap) as usize, address.unwrap_or(0));
        let (index, shadow) = match self.root_uav_slots.get(&key) {
            Some((index, shadow)) => (*index, address.zip(shadow.clone())),
            None => {
                let shadow = address.and_then(|a| find(a).map(|s| (a, s)));
                let Some(index) = crate::host::table_slots(host, 1) else {
                    crate::drops::note(host.refusal(1));
                    return false;
                };
                self.dynamic.push((key.0, host.id(), index, 1));
                if let Some((a, s)) = &shadow {
                    // SAFETY: a live buffer.
                    let base = unsafe { s.buffer.GetGPUVirtualAddress() };
                    let offset = s.gpu(*a) - base;
                    // Raw views start on 16 bytes: from the 16 bytes
                    // below, and the shader adds the rest to its offsets
                    // (`leonida_shader::emit::USER_VIEW_BIAS`). (An address
                    // not on 16 bytes got a null view at first: the
                    // builder's counters are at such addresses.)
                    let bias = offset & 15;
                    host.raw_uav_at(index, &s.buffer, offset - bias, s.left(*a).min(limit) + bias);
                }
                if shadow.is_none() {
                    host.null_raw_uav_at(index);
                }
                self.root_uav_slots
                    .insert(key, (index, shadow.as_ref().map(|(_, s)| s.clone())));
                (index, shadow)
            }
        };
        if let Some((a, s)) = &shadow {
            ready(*a, s);
        }
        let handle = host.gpu_handle(index);
        // An open list; the slot is in the bound view heap.
        self.put_table(&list.list, compute, param, handle);
        true
    }

    /// Binds a root SRV as a one-view table
    /// ([`crate::pipeline::host::bounded_root_srvs`]): a raw view of the
    /// title buffer's host copy from `address` to the copy's end, or a null
    /// view (no address, no host copy, or an address not on 16 bytes). One
    /// dynamic slot per address per list, given back with the list's
    /// others. False if no slot could be had or no view heap is bound.
    fn root_srv_table(
        &mut self,
        list: &HostList,
        address: Option<u64>,
        param: u32,
        compute: bool,
        biased: bool,
    ) -> bool {
        let heap = match self.heaps[0] {
            0 => None,
            h => Some(crate::descriptor::heap(h as This)),
        };
        let Some((heap, host)) = heap.and_then(|h| Some((h, h.host()?))) else {
            once!("a root SRV without a bound view heap: the draw is dropped");
            return false;
        };
        // A view that begins early is another view than one that does not
        // (title addresses stay below bit 63).
        let key = (
            std::ptr::from_ref(heap) as usize,
            address.unwrap_or(0) | u64::from(biased) << 63,
        );
        let (index, shadow) = match self.root_srv_slots.get(&key) {
            Some((index, shadow)) => (*index, address.zip(shadow.clone())),
            None => {
                let shadow = address.and_then(|a| crate::shadow::find(a, 1).map(|s| (a, s)));
                let Some(index) = crate::host::table_slots(host, 1) else {
                    crate::drops::note(host.refusal(1));
                    return false;
                };
                self.dynamic.push((key.0, host.id(), index, 1));
                self.root_srv_slots
                    .insert(key, (index, shadow.as_ref().map(|(_, s)| s.clone())));
                match &shadow {
                    Some((a, s)) => {
                        // SAFETY: a live buffer.
                        let base = unsafe { s.buffer.GetGPUVirtualAddress() };
                        let offset = s.gpu(*a) - base;
                        if biased {
                            // From the 16 bytes below; the shader adds the
                            // rest (`leonida_shader::emit::USER_VIEW_BIAS`).
                            let bias = offset & 15;
                            host.raw_at(index, &s.buffer, offset - bias, s.left(*a) + bias);
                        } else if offset.is_multiple_of(16) {
                            host.raw_at(index, &s.buffer, offset, s.left(*a));
                        } else {
                            // Raw views start on 16 bytes; one from the
                            // aligned start would shift every read.
                            once!(
                                address = format_args!("{a:#x}"),
                                "a root SRV not on 16 bytes reads zeros"
                            );
                            host.null_raw_at(index);
                        }
                    }
                    None => host.null_raw_at(index),
                }
                (index, shadow)
            }
        };
        // As for root descriptors (`root_view`): all that is left of the
        // buffer is brought up to date. The first 1 MiB only left what lay
        // past it stale (most mesh vertex streams reach further).
        if let Some((a, s)) = &shadow {
            s.refresh(list, *a, s.left(*a));
        }
        let handle = host.gpu_handle(index);
        // An open list; the slot is in the bound view heap.
        self.put_table(&list.list, compute, param, handle);
        true
    }

    /// For a shader that picks buffers from the view heap by an index it
    /// computes (SM 6.6 dynamic resources): the GPU address of the heap's
    /// table of element strides, with every buffer view of the heap given
    /// its host slot and its memory brought up to date when `list` is
    /// submitted. Tables tell us which views a draw reads; nothing tells
    /// us which of the heap's a computed index names, so all of them are
    /// made ready, once per list.
    /// shortcut: the buffers are read, not written: what such a shader
    /// writes through a picked unordered access view stays in the host
    /// copy. A list refreshes them where it first runs such a shader;
    /// title writes recorded later in the same list are not seen.
    fn heap_buffers(&self, list: &HostList) -> Option<u64> {
        let heap = self
            .heaps
            .iter()
            .filter(|&&h| h != 0)
            .map(|&h| crate::descriptor::heap(h as This))
            .find(|h| !h.samplers())?;
        let host = heap.host()?;
        let gpu = host.strides_gpu()?;
        // Stride buffers have aligned GPU addresses. Tag the list key so it
        // is distinct from aligned guest resource pointers.
        if !list.first_use(gpu as usize | 1) {
            return Some(gpu);
        }
        // The list may outlive the title's heap object, which owns the table.
        if let Some(buffer) = host.strides_buffer() {
            list.keep(buffer);
        }
        // The pass is the list's that goes ahead of the frame, and the
        // textures' with it ([`passes_ahead`]).
        if done_ahead(0, std::ptr::from_ref(heap) as usize, host) {
            return Some(gpu);
        }
        // The pass itself once per frame: per list it ran up to 51 times a
        // frame over 81,000 views and 4,100 host copies, 18 s of every
        // frame (2026-11-24, run 11).
        // shortcut: what the title writes into such buffers between lists
        // of one frame is seen a frame late.
        let frame = crate::frame::current();
        if host.buffer_frame.swap(frame, Ordering::Relaxed) == frame {
            return Some(gpu);
        }
        let started = std::time::Instant::now();
        let mut stage = crate::timeline::span("the heap pass: begun, or done here");
        if crate::host::aside::on() {
            let at = std::ptr::from_ref(heap) as usize;
            list.refreshes_coming(move || {
                crate::descriptor::with_heap(at, |heap| {
                    // The list retains the original stride buffer. A newly
                    // allocated heap at the same CPU address has another one.
                    heap.host().filter(|h| h.strides_gpu() == Some(gpu)).map(|h| heap_buffer_pass(heap, h, frame))
                })
                    .flatten()
                    .unwrap_or_default()
            });
        } else {
            for (shadow, from, to) in heap_buffer_pass(heap, host, frame) {
                shadow.refresh(list, from, to - from);
            }
        }
        #[cfg(feature = "diagnostics")]
        let refreshed = std::time::Instant::now();
        stage.next("the heap pass: textures");
        let _ = self.heap_textures(list, heap, host, None);
        stage.end();
        #[cfg(feature = "diagnostics")]
        HEAP_SPLIT[2].fetch_add(refreshed.elapsed().as_nanos() as u64, Ordering::Relaxed);
        HEAP_BUFFER_PASSES.fetch_add(1, Ordering::Relaxed);
        HEAP_BUFFER_MICROS.fetch_add(started.elapsed().as_micros() as u64, Ordering::Relaxed);
        Some(gpu)
    }

    /// The GPU address of the bound view heap's stride/kind table without
    /// running the bindless heap buffer/texture sweep (for shaders that only
    /// inspect descriptors already mirrored via their descriptor tables).
    fn heap_strides(&self, list: &HostList) -> Option<u64> {
        let heap = self
            .heaps
            .iter()
            .filter(|&&h| h != 0)
            .map(|&h| crate::descriptor::heap(h as This))
            .find(|h| !h.samplers())?;
        let host = heap.host()?;
        let gpu = host.strides_gpu()?;
        // Tag bit 1 (distinct from `heap_buffers`'s `| 1` and aligned guest pointers).
        if list.first_use(gpu as usize | 2)
            && let Some(buffer) = host.strides_buffer()
        {
            list.keep(buffer);
        }
        Some(gpu)
    }

    /// Ensures textures claimed by background copy-queue uploads
    /// (`LEONIDA_UPLOAD_AHEAD=1`) or deferred behind the `neutral()` gray
    /// stand-in (`LEONIDA_SHOW_AFTER=1` / `shows_other`) have finished
    /// uploading and flipped to their real descriptors before a one-shot
    /// decoration bake draw samples them.
    fn flush_for_bake(
        &self,
        list: &HostList,
        host: &crate::descriptor::host::HostHeap,
        mut sync: Option<&mut BakeSync>,
    ) {
        static RESCUED: [AtomicU64; 4] = [const { AtomicU64::new(0) }; 4];
        let drained = crate::texture::ahead::drain();
        let mut settled = false;
        let mut flipped = 0usize;
        for _ in 0..4 {
            let need_settle = host.needs_gpu_settle_for_bake();
            if need_settle {
                settled = true;
                crate::host::settle_for_fence();
            }
            flipped += host.texture_views_remade();
            flipped += host.texture_slots_flip();
            if !need_settle || !host.needs_gpu_settle_for_bake() {
                break;
            }
        }
        let pending = host.take_pending_list_flips();
        let mut promoted = 0usize;
        if !pending.is_empty() {
            let this_list = (std::ptr::from_ref(list) as usize, list.epoch());
            let sampled = UsedResource {
                kind: ResourceType::Srv,
                root: RootType::Table,
                table_offset_dwords: 0,
                lower_bound: 0,
                stride: 0,
                dwords: 8,
                user_data: 0,
                slot: 0,
            };
            for item in pending {
                let (origin, index, handle, key) = (item.origin, item.index(), item.handle(), item.key());
                if !host.slot_still_at(handle, key) {
                    continue;
                }
                let Some(view) = crate::descriptor::view(handle) else {
                    continue;
                };
                if let View::Placed(_, resource, desc) = &view {
                    let address = u64::from(desc[10]) | u64::from(desc[11]) << 32;
                    let len = crate::resource::placed_bytes(resource).unwrap_or(1);
                    if crate::texture::mapped_bytes(address as *const u8, len as usize) == 0 {
                        host.restore_pending_list_flip(item);
                        continue;
                    }
                }
                if sync.is_some() || origin != this_list {
                    let placed = crate::descriptor::host::placed_texture(&view);
                    if let Some(&View::Shader(resource, _)) = placed.as_ref().or(Some(&view)) {
                        let resource = resource as This;
                        let texture = crate::resource::host_resource(resource);
                        let title_fills = texture.is_some_and(crate::texture::title_written);
                        let first = match (&placed, &view) {
                            (Some(_), View::Placed(_, _, desc)) => crate::resource::first_mip(desc),
                            _ => 0,
                        };
                        if let (Some(sync), Some(copy)) = (sync.as_deref_mut(), crate::resource::host_copy(resource)) {
                            sync.keep.push(copy);
                        }
                        if title_fills
                            && !crate::texture::ahead::there(resource)
                            && (sync.is_some() || !crate::texture::unchanged(resource, first))
                        {
                            let was_staged = crate::texture::is_staged(resource);
                            let _ = crate::residency::take_staging_deferred();
                            readable(list, sampled.kind, &view);
                            let failed = crate::residency::take_staging_deferred() || crate::residency::upload_refused();
                            if !was_staged {
                                crate::texture::unstage(resource);
                            }
                            if failed {
                                host.restore_pending_list_flip(item);
                                continue;
                            }
                        }
                    }
                }
                if let Some(sync) = sync.as_deref_mut() {
                    sync.publish.push((index, handle, key));
                } else {
                    let _ = host.mirror(index, handle, true);
                }
                promoted += 1;
            }
        }
        if drained > 0 || settled || flipped > 0 || promoted > 0 {
            let n = RESCUED[0].fetch_add(1, Ordering::Relaxed) + 1;
            let all_drained = RESCUED[1].fetch_add(drained as u64, Ordering::Relaxed) + drained as u64;
            let all_flipped = RESCUED[2].fetch_add(flipped as u64, Ordering::Relaxed) + flipped as u64;
            let all_promoted = RESCUED[3].fetch_add(promoted as u64, Ordering::Relaxed) + promoted as u64;
            if n.is_power_of_two() {
                tracing::info!(
                    frame = crate::frame::current(),
                    flushes = n,
                    drained_ahead = all_drained,
                    flipped = all_flipped,
                    promoted_from_lists = all_promoted,
                    settled,
                    "flushed deferred texture stand-ins before decoration bake"
                );
            }
        }
    }

    /// Whether a graphics draw is a one-shot texture compositor / decoration
    /// bake whose bindless texture inputs must be flushed and up to date
    /// before recording the draw. Matches known bake pixel/geometry shaders
    /// and structurally detects depth-less 4+ MRT square atlas bakes
    /// (`>= 512x512`) so unseen NPC/head/clothing shader permutations never
    /// bake `neutral()` or `null` stand-ins into permanent ped textures.
    fn is_bake_draw(&self, p: &HostPipeline) -> bool {
        if p.stages.iter().any(|s| {
            let h = s.shader.hash.as_str();
            BAKE_PIXEL_SHADERS.contains(&h) || BAKE_GEOMETRY_SHADERS.contains(&h)
        }) {
            return true;
        }
        self.depth == 0
            && self.targets.len() >= 4
            && self
                .targets
                .first()
                .and_then(|&t| crate::resource::texture_kind(t as This))
                .is_some_and(|(w, h, _)| w >= 512 && w == u64::from(h))
    }

    /// Runs the pass that uploads textures picked by index
    /// ([`State::heap_textures`]) where nothing has asked for it yet
    /// ([`early_budgets`]): once a frame for a few milliseconds, at the
    /// frame's first draw with a view heap set, and with a larger budget
    /// of its own before every draw whose pixel shader is one of the
    /// title's bake programs. For decoration bakes, uploads run synchronously
    /// on a dedicated internal command list and descriptors are published only
    /// after the GPU copy finishes so concurrent `LEONIDA_THIN=1` lists can
    /// never submit a bake draw ahead of its texture uploads.
    fn early_textures(&self, list: &HostList, p: &HostPipeline, compute: bool) {
        let (early_ms, bake_ms) = early_budgets();
        if compute || (early_ms == 0 && bake_ms == 0) {
            return;
        }
        let is_bake = bake_ms > 0 && self.is_bake_draw(p);
        static FRAME: AtomicU64 = AtomicU64::new(u64::MAX);
        // The milliseconds this frame's bake draws have taken so far: a
        // frame has several, and together they get `bake_ms`.
        static BAKED_MS: AtomicU64 = AtomicU64::new(0);
        let frame = crate::frame::current();
        let first = first_of_frame(&FRAME, frame, frame_peek());
        if first {
            BAKED_MS.store(0, Ordering::Relaxed);
        }
        if !is_bake && !(first && early_ms > 0) {
            return;
        }
        let Some(heap) = self
            .heaps
            .iter()
            .filter(|&&h| h != 0)
            .map(|&h| crate::descriptor::heap(h as This))
            .find(|h| !h.samplers())
        else {
            return;
        };
        let Some(host) = heap.host() else {
            return;
        };
        if is_bake {
            let began = std::time::Instant::now();
            let mut bake_slot = BAKE_UPLOAD.lock().unwrap_or_else(|e| e.into_inner());
            let gpu = crate::host::host();
            if bake_slot.is_none()
                && let Some(gpu) = gpu
            {
                match gpu.new_list() {
                    Ok(upload_list) => *bake_slot = Some(BakeList(upload_list)),
                    Err(e) => tracing::warn!(error = %e, "no host list for synchronous bake uploads"),
                }
            }
            let exact = bake_exact();
            if let (Some(gpu), Some(BakeList(bake_list))) = (gpu, bake_slot.as_ref()) {
                let mut sync = BakeSync { publish: Vec::new(), keep: Vec::new() };
                crate::texture::BAKING.set(exact);
                self.flush_for_bake(bake_list, host, Some(&mut sync));
                let left = bake_ms.saturating_sub(BAKED_MS.load(Ordering::Relaxed));
                let run_pass = left > 0
                    && (first
                        || !exact
                        || {
                            let (writes, noted) = host.texture_news();
                            noted || BAKE_SERVED.load(Ordering::Relaxed) != writes + 1
                        });
                let mut served = false;
                if run_pass {
                    let n = EARLY_TEXTURE_PASSES[1].fetch_add(1, Ordering::Relaxed) + 1;
                    if n.is_power_of_two() {
                        let [slots, due, ..] = HEAP_TEXTURES_QUEUE.each_ref().map(|c| c.load(Ordering::Relaxed));
                        tracing::info!(
                            frame,
                            bakes = n,
                            queued_at_the_last_pass = slots,
                            due_at_the_last_pass = due,
                            uploaded_so_far = HEAP_TEXTURES.load(Ordering::Relaxed),
                            "a draw of the title's decoration bake: textures picked by index are uploaded synchronously first (LEONIDA_BAKE_TEXTURES_MS=0: not)"
                        );
                    }
                    BAKE_SERVED.store(0, Ordering::Relaxed);
                    PASS_BUDGET.set(Some((left, 2 << 30, true)));
                    served = self.heap_textures(bake_list, heap, host, Some(&mut sync));
                    PASS_BUDGET.set(None);
                }
                crate::texture::BAKING.set(false);
                if bake_list.has_recorded_work() {
                    bake_list.close();
                    gpu.execute_sync_internal(bake_list);
                    bake_list.reset();
                } else if !sync.publish.is_empty() {
                    crate::host::settle_for_fence();
                }
                for (index, handle, key) in sync.publish {
                    let _ = host.publish_uploaded_slot(index, handle, key);
                }
                host.texture_views_remade();
                host.texture_slots_flip();
                drop(sync.keep);
                if run_pass && served && exact {
                    BAKE_SERVED.store(host.texture_news().0 + 1, Ordering::Relaxed);
                }
                BAKED_MS.fetch_add(began.elapsed().as_millis() as u64, Ordering::Relaxed);
                return;
            }
            // Fallback when no host GPU list can be created (e.g. headless tests).
            crate::texture::BAKING.set(exact);
            self.flush_for_bake(list, host, None);
            crate::texture::BAKING.set(false);
            let left = bake_ms.saturating_sub(BAKED_MS.load(Ordering::Relaxed));
            if left == 0 {
                return;
            }
            if !first && exact {
                let (writes, noted) = host.texture_news();
                if !noted && BAKE_SERVED.load(Ordering::Relaxed) == writes + 1 {
                    return;
                }
            }
            EARLY_TEXTURE_PASSES[1].fetch_add(1, Ordering::Relaxed);
            PASS_BUDGET.set(Some((left, 2 << 30, true)));
            crate::texture::BAKING.set(exact);
            let _ = self.heap_textures(list, heap, host, None);
            crate::texture::BAKING.set(false);
            PASS_BUDGET.set(None);
            BAKED_MS.fetch_add(began.elapsed().as_millis() as u64, Ordering::Relaxed);
            return;
        }
        let budget = if first && early_ms > 0 {
            (early_ms, 64 << 20)
        } else {
            return;
        };
        // The pass is the list's that goes ahead of the frame
        // ([`passes_ahead`]).
        if done_ahead(1, std::ptr::from_ref(heap) as usize, host) {
            return;
        }
        EARLY_TEXTURE_PASSES[0].fetch_add(1, Ordering::Relaxed);
        PASS_BUDGET.set(Some((budget.0, budget.1, false)));
        let _ = self.heap_textures(list, heap, host, None);
        PASS_BUDGET.set(None);
    }

    /// The queue's side of the card accounting line
    /// (`crate::resource::card_accounting`, which says what the line
    /// holds): the slots that are due, by what their texture has on the
    /// card. Off unless `LEONIDA_CARD_ACCOUNTING=1`; then at the first pass
    /// that finds the card without room, and every five seconds after it.
    /// Nothing is made, uploaded or counted as used here. For 18,000 due
    /// slots: 18,000 looks at a title slot and at the texture it views, a
    /// lock each, some milliseconds of the thread that records the frame.
    fn card_accounting(
        heap: &crate::descriptor::Heap,
        host: &crate::descriptor::host::HostHeap,
        frame: u64,
        room: bool,
    ) {
        static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
        if !*ON.get_or_init(|| std::env::var_os("LEONIDA_CARD_ACCOUNTING").is_some_and(|v| v == "1")) {
            return;
        }
        // When the next line is due (0: none yet, the card has had room).
        static NEXT: AtomicU64 = AtomicU64::new(0);
        let now = crate::host::now_ms().max(1);
        let next = NEXT.load(Ordering::Relaxed);
        // (With levels on demand the card may never be without room: the
        // lines then start with the first texture made that way.)
        let by_level = crate::tiles::STATS[4].load(Ordering::Relaxed) > 0;
        let due_now = if next == 0 { !room || by_level } else { now >= next };
        if !due_now || NEXT.compare_exchange(next, now + 5000, Ordering::Relaxed, Ordering::Relaxed).is_err() {
            return;
        }
        // Due slots; of them uploaded before; of them viewing nothing the
        // title fills; and the textures (each once) whose host copy is
        // alive, with their bytes on the card.
        let mut due = [0u64; 5];
        // Textures (each once) that never had a host copy, and ones whose
        // host copy was let go: where their memory is, and its bytes.
        let (mut never, mut gone) = (Vec::new(), Vec::new());
        let mut seen = std::collections::BTreeSet::new();
        for (index, uploaded) in host.texture_slots_all_due(frame) {
            due[0] += 1;
            due[1] += u64::from(uploaded);
            let state = match heap.handle(index).and_then(crate::descriptor::view) {
                Some(View::Placed(false, resource, desc)) if resource[0] != 1 => {
                    let address = u64::from(desc[10]) | u64::from(desc[11]) << 32;
                    match crate::resource::placed_known(address, &resource) {
                        Some(texture) => crate::resource::copy_state(texture),
                        // No title texture was made for it: the pass never
                        // got as far as making one.
                        None => Some((0, crate::resource::placed_bytes(&resource).unwrap_or(0), address)),
                    }
                }
                Some(View::Shader(object, _)) if crate::resource::title_fills(object as This) => {
                    crate::resource::copy_state(object as This)
                }
                _ => None,
            };
            let Some((state, bytes, memory)) = state else {
                due[2] += 1;
                continue;
            };
            if !seen.insert(memory) {
                continue;
            }
            match state {
                2 => {
                    due[3] += 1;
                    due[4] += bytes;
                }
                1 => gone.push((memory, bytes)),
                _ => never.push((memory, bytes)),
            }
        }
        crate::resource::card_accounting(room, due, never, gone);
    }

    /// The texture side of [`State::heap_buffers`]: textures the title fills
    /// itself (streamed pages, layout-library copies) whose views sit in the
    /// view heap are created (placed textures have no host texture until
    /// something reads them) and uploaded, as a table read would do, for
    /// shaders that pick them by a computed index. Within a budget of bytes
    /// per frame and in all; the rest waits for later lists and frames.
    /// The most recently written slots first (`TextureQueue::due`); slots
    /// draws read through tables are not in the queue.
    /// shortcut: uploaded once, and again only when the title rewrites the
    /// slot. A placed texture not all mapped yet is uploaded with what is
    /// there and looked at again (1, 2, 4, ... frames later, at most about
    /// a second): uploaded again when more of it is mapped. Pages mapped
    /// in place of others, and contents changed with no descriptor write,
    /// are missed (hook the page-mapping events for those).
    /// `LEONIDA_HEAP_TEXTURES=0` turns it off.
    fn heap_textures(
        &self,
        list: &HostList,
        heap: &crate::descriptor::Heap,
        host: &crate::descriptor::host::HostHeap,
        mut sync: Option<&mut BakeSync>,
    ) -> bool {
        static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
        if !*ON.get_or_init(|| std::env::var_os("LEONIDA_HEAP_TEXTURES").is_none_or(|v| v != "0")) {
            return true;
        }
        let memory = crate::host::host().and_then(|h| h.video_memory());
        // `LEONIDA_TEXTURE_STAGE_MB` sets it: a backlog of tens of thousands of
        // textures (a scene's first minutes) then drains in fewer frames.
        static PER_FRAME_MB: std::sync::OnceLock<Option<u64>> = std::sync::OnceLock::new();
        let asked = *PER_FRAME_MB.get_or_init(|| {
            std::env::var("LEONIDA_TEXTURE_STAGE_MB").ok().and_then(|v| v.trim().parse::<u64>().ok())
        });
        let per_frame = match asked {
            Some(mb) => mb << 20,
            None => {
                let base = staging_budget_per_frame(memory.map(|[budget, ..]| budget));
                if crate::residency::limited_host_ram() {
                    base.min(128 << 20)
                } else {
                    base
                }
            }
        };
        static FRAME: AtomicU64 = AtomicU64::new(u64::MAX);
        static SPENT: AtomicU64 = AtomicU64::new(0);
        let frame = crate::frame::current();
        if FRAME.swap(frame, Ordering::Relaxed) != frame {
            SPENT.store(0, Ordering::Relaxed);
        }
        // No more of them are made while the card has no room
        // (`resource::room`): by what is alive now, not by what was ever
        // staged (4 GB in all, after which later textures stayed empty for
        // the run). The ones nothing names any more are let go, which
        // makes room again: so the slots are still looked at (the pass
        // returned here, and a slot whose texture had been streamed out
        // kept its host copy for as long as the card was full).
        let mut room = crate::resource::room(memory);
        // Bytes uploaded by this pass since the card was last asked
        // whether it has room: asked again every 32 MiB (a pass may upload
        // 192 MB and more, and one asking a pass let the queue run some
        // 400 MB past its line while the world loaded: run 193).
        let mut since_asked = 0u64;
        if !room {
            HEAP_TEXTURES_FULL.fetch_add(1, Ordering::Relaxed);
            once!("textures picked by index: the card has no room for more; they wait");
        }
        let mut refused = 0;
        // Room again: the slots held back while the card was full are due
        // now (`full_card_pass`).
        if room {
            host.texture_slots_room(frame);
        }
        // Slots whose texture's memory the runtime mapped, unmapped or
        // wrote since the last pass are due again.
        host.texture_slots_remapped(frame);
        let baking = crate::texture::BAKING.get();
        if baking {
            host.texture_slots_bake_written(frame);
        }
        host.texture_slots_short(frame);
        // The slots of textures whose first level with memory (levels on
        // demand), or whose host copy for one with more levels, changed since
        // the last pass are made again from it; those whose new view's upload
        // has run are made from that.
        host.texture_views_remade();
        host.texture_slots_flip();
        // And with room, the textures whose upload left a held level
        // without the card's memory are uploaded again.
        if room {
            host.texture_slots_waiting(frame);
        }
        let required = PASS_BUDGET.get().is_some_and(|(_, _, bake)| bake);
        let (mut due, census) = host.texture_slots_due(frame, if baking { 1 << 16 } else { 4096 });
        // (Those that are due later after all that are due.)
        let on_time = due.len();
        if baking {
            let known: std::collections::HashSet<u32> = due.iter().map(|&(index, _)| index).collect();
            static AHEAD: AtomicU64 = AtomicU64::new(u64::MAX);
            let again = AHEAD.swap(frame, Ordering::Relaxed) == frame;
            let (ahead, _) = host.texture_slots_due(frame + BAKE_AHEAD[usize::from(again)], 1 << 16);
            due.extend(ahead.into_iter().filter(|(index, _)| !known.contains(index)));
        }
        HEAP_TEXTURES_WAITING.store(census[0], Ordering::Relaxed);
        for (count, now) in HEAP_TEXTURES_QUEUE.iter().zip(census) {
            count.store(now, Ordering::Relaxed);
        }
        // Where the card's memory is, and what the due slots would need
        // (`LEONIDA_CARD_ACCOUNTING=1`; a line when the card first has no
        // room, and every five seconds after it).
        Self::card_accounting(heap, host, frame, room);
        // The frames in about four seconds, between 16 and 256: how long a
        // slot with nothing to upload waits at most for its next look. In
        // frames alone it was 120: four minutes at the opening cutscene's
        // half a frame per second, while the title streams by the clock.
        // (About a second before the runtime told of the memory it maps:
        // the looks are now only for what that does not tell.)
        static PASSED: AtomicU64 = AtomicU64::new(0);
        let now = crate::host::now_ms();
        let took = now.saturating_sub(PASSED.swap(now, Ordering::Relaxed)).max(1);
        // `LEONIDA_RELOOK_MS` (20,000 unless said; 4,000 before): at 4,000 the
        // opening cutscene's 4 frames a second looked at its ~20,000 slots
        // waiting for their memory every 16 frames, thousands of queries
        // a second and most of the pass's time, while slots never looked
        // at waited. The runtime's mappings already make a slot due
        // (`texture_slots_remapped`); these looks are only for what that
        // does not tell.
        static RELOOK_MS: std::sync::OnceLock<u64> = std::sync::OnceLock::new();
        let relook = *RELOOK_MS.get_or_init(|| {
            std::env::var("LEONIDA_RELOOK_MS").ok().and_then(|v| v.trim().parse().ok()).unwrap_or(20_000)
        });
        let most = (relook / took).clamp(16, 1024);
        // What a table entry of a texture a shader samples says.
        let sampled = UsedResource {
            kind: ResourceType::Srv,
            root: RootType::Table,
            table_offset_dwords: 0,
            lower_bound: 0,
            stride: 0,
            dwords: 8,
            user_data: 0,
            slot: 0,
        };
        // Also within a time per pass: the pass runs on a thread that
        // records the frame, and making and staging 192 MB of textures took
        // 1.6-2.2 s of each of the world's first twenty frames (run 20).
        // The rest waits for the next frames, as it does past the bytes.
        static SPAN_MS: std::sync::OnceLock<u64> = std::sync::OnceLock::new();
        let span = std::time::Duration::from_millis(*SPAN_MS.get_or_init(|| {
            std::env::var("LEONIDA_TEXTURE_STAGE_MS").ok().and_then(|v| v.trim().parse().ok()).unwrap_or(250)
        }));
        // A backlog (a scene's first minutes: 22,000 slots due, 2-3 minutes of black
        // surfaces at 192 MB a frame) gets three times the bytes and the time until it
        // drains; `LEONIDA_TEXTURE_CATCHUP=0` keeps the plain budget.
        static CATCHUP: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
        let catchup = *CATCHUP.get_or_init(|| std::env::var_os("LEONIDA_TEXTURE_CATCHUP").is_none_or(|v| v != "0"));
        let (per_frame, span) = if catchup && on_time >= 1024 { (per_frame * 3, span * 3) } else { (per_frame, span) };
        // A run made early (`State::early_textures`) has a budget of its
        // own: its bytes on top of what the frame has spent so far.
        // (Not `most`: that is the longest wait between looks, used below.)
        let (span, bytes_at_most) = match PASS_BUDGET.get() {
            Some((ms, bytes, _)) => {
                (std::time::Duration::from_millis(ms), SPENT.load(Ordering::Relaxed).saturating_add(bytes))
            }
            None => (span, per_frame),
        };
        // Apply the shared frame ceiling after early-pass and catch-up budgets.
        // A bake samples its inputs immediately and cannot retry that draw.
        let bytes_at_most = if required || asked.is_some() {
            bytes_at_most
        } else {
            crate::residency::prefetch_limit(
                bytes_at_most,
                crate::residency::pressured(),
                crate::residency::critical_commit(),
                crate::residency::limited_host_ram(),
                crate::texture::live_staging_bytes(),
            )
        };

        let began = std::time::Instant::now();
        if sync.is_none() {
            BAKE_SERVED.store(0, Ordering::Relaxed);
        }
        let mut served = true;
        for (index, write) in due {
            if SPENT.load(Ordering::Relaxed) >= bytes_at_most || began.elapsed() > span {
                served = false;
                break;
            }
            // A placed texture whose memory is not mapped (not streamed in
            // yet, or streamed out again: the heap keeps tens of thousands
            // of such views) gets no host texture; looked at again soon (a
            // title writes a texture's view before its pages are mapped,
            // and the pass runs early in a frame). One of which some is
            // mapped (a texture streaming in: its smaller mips lie at the
            // start of its memory and arrive first) is uploaded with what
            // is there, and again when more is. (It was uploaded again
            // every 120 frames, changed or not; and asked by its first
            // page alone, one whose first page is the one missing counted
            // as not there.)
            // A slot uploaded whole is looked at again a few times
            // (`RECHECK_FRAMES`): the title writes a texture's view, and
            // may fill the texture's memory itself only afterwards, with
            // no descriptor write and nothing from the runtime to tell of
            // it. The host copy then kept what the memory held at the
            // first look for the rest of the run. The look reads 48 KiB;
            // only a changed texture is uploaded again.
            // `LEONIDA_HEAP_RECHECK=0`: uploaded once, as before.
            // One found changed by such a look is looked at in the next frame
            // again: the textures left that change unnoticed are ones the
            // title paints all the time through another view of their pages
            // (all their blocks are shown at other addresses too), and each
            // was shown four frames late, over and over.
            let mut again_in = RECHECK_FRAMES[0];
            if let Some((at, len, print, looks, uploaded)) = host.texture_slot_checked(index) {
                let now = crate::texture::fingerprint(at, len);
                // Leonida: a change the look's three windows did not see
                // (`LEONIDA_HEAP_PRINT_SPREAD`): counted, the first ones named.
                if crate::texture::spread_alone(now, print) {
                    let n = HEAP_TEXTURES_SPREAD.fetch_add(1, Ordering::Relaxed);
                    if n < 100 || n.is_multiple_of(1000) {
                        tracing::warn!(
                            slot = index,
                            address = format_args!("{at:#x}"),
                            bytes = len,
                            frames_after_upload = frame.saturating_sub(uploaded),
                            looks,
                            so_far = n + 1,
                            "a texture picked by index changed where only the spread places of the look read (LEONIDA_HEAP_PRINT_SPREAD): uploaded again"
                        );
                    }
                }
                if now == print {
                    HEAP_TEXTURES_RECHECKED[0].fetch_add(1, Ordering::Relaxed);
                    match (RECHECK_FRAMES.get(usize::from(looks) + 1), recheck_every()) {
                        (Some(wait), _) => {
                            host.texture_slot_recheck(index, write, frame + wait, (at, len), print, looks + 1, uploaded)
                        }
                        // Past the schedule: on a slow beat for as long as
                        // the slot shows this texture (`recheck_every`).
                        (None, 0) => host.texture_slot_done(index, write, None),
                        (None, every) => {
                            host.texture_slot_recheck(index, write, frame + every, (at, len), print, looks, uploaded)
                        }
                    }
                    continue;
                }
                if usize::from(looks) + 1 >= RECHECK_FRAMES.len() {
                    // A change after the last of the usual looks: one the build kept from
                    // the screen for the rest of the run before.
                    let n = HEAP_TEXTURES_LATE.fetch_add(1, Ordering::Relaxed);
                    if n < 300 {
                        tracing::warn!(
                            slot = index,
                            address = format_args!("{at:#x}"),
                            bytes = len,
                            frames_after_upload = frame.saturating_sub(uploaded),
                            late = n + 1,
                            "a texture picked by index changed after the last usual look (LEONIDA_HEAP_RECHECK_EVERY): uploaded again"
                        );
                    }
                }
                if recheck_soon() {
                    again_in = 1;
                }
                let n = HEAP_TEXTURES_RECHECKED[1].fetch_add(1, Ordering::Relaxed);
                if n < 200 || n.is_multiple_of(1000) {
                    // Whether its pages are the title's at other addresses
                    // too: what is written there is told of there alone.
                    let elsewhere = crate::inplace::shown_elsewhere(at as usize & !0xffff, (at + len) as usize);
                    let first = heap.handle(index).and_then(crate::descriptor::view).map_or(0, |view| match view {
                        View::Placed(false, _, desc) => crate::resource::first_mip(&desc),
                        _ => 0,
                    });
                    tracing::warn!(
                        first_level = first,
                        blocks_shown_elsewhere = elsewhere.len(),
                        slot = index,
                        address = format_args!("{at:#x}"),
                        bytes = len,
                        frames_after_upload = frame.saturating_sub(uploaded),
                        looks,
                        changed = n + 1,
                        "a texture picked by index changed after its upload with nothing to tell of it: uploaded again"
                    );
                }
            }
            let mut partly = None;
            let mut there = None;
            if let Some(View::Placed(false, resource, desc)) = heap.handle(index).and_then(crate::descriptor::view) {
                let address = u64::from(desc[10]) | u64::from(desc[11]) << 32;
                let first = crate::resource::first_mip(&desc);
                let len = crate::resource::placed_bytes_from(&resource, first).unwrap_or(1);
                let mapped = crate::texture::mapped_bytes(address as *const u8, len as usize);
                if mapped == 0 {
                    HEAP_TEXTURES_AWAY.fetch_add(1, Ordering::Relaxed);
                    // Streamed out: what the slot showed is let go.
                    host.texture_slot_gone(index);
                    host.texture_slot_later(index, write, frame, most);
                    continue;
                }
                host.texture_slot_here(index);
                there = Some(mapped);
                if mapped < len {
                    if mapped <= host.texture_slot_mapped(index) {
                        HEAP_TEXTURES_PARTLY[0].fetch_add(1, Ordering::Relaxed);
                        host.texture_slot_later(index, write, frame, most);
                        continue;
                    }
                    if !crate::texture::committed(address as *const u8, 1) {
                        HEAP_TEXTURES_PARTLY[2].fetch_add(1, Ordering::Relaxed);
                    }
                    partly = Some(mapped);
                }
            }
            // A texture object the host renders to or writes holds the
            // host's own contents: nothing to bring up to date, and this
            // pass is no use of it (its host copy is made when something
            // uses it, and its slots then: `descriptor::make_waiting`).
            if let Some(View::Shader(object, _)) = heap.handle(index).and_then(crate::descriptor::view)
                && crate::resource::buffer_bytes(object as This).is_none()
                && !crate::resource::title_fills(object as This)
            {
                host.texture_slot_done(index, write, None);
                continue;
            }
            // From here on a host texture is made: not without room. The
            // slot stays due.
            // Not so a slot already made from this very view: its host
            // texture exists, and uploading it again takes none of the
            // card's memory. It was refused all the same, so a texture the
            // title filled after its first upload kept its empty host copy
            // from the moment the card reached the line, which is the first
            // world frame in eight launches of nine: the same slots were
            // found changed again at every pass and never uploaded (run
            // 179: 33,000 such findings), and the main character's face
            // had its colour only in the launches that entered the world
            // under the line. `LEONIDA_REUPLOAD_WHEN_FULL=0`: refused, as
            // before.
            if !room {
                let has_its_texture = reupload_when_full()
                    && heap.handle(index).is_some_and(|handle| host.texture_slot_made_from(index, handle));
                if !has_its_texture {
                    refused += 1;
                    if full_card_pass() {
                        // Held back for the frames of about four seconds
                        // (`most`), or until the card has room; the pass
                        // goes on, within a count that bounds one pass.
                        host.texture_slot_held(index, write, frame + most);
                        if refused > 1024 {
                            break;
                        }
                    } else if refused > 256 {
                        break;
                    }
                    continue;
                }
                let n = HEAP_TEXTURES_REUPLOADED_FULL.fetch_add(1, Ordering::Relaxed) + 1;
                if n == 1 || n.is_multiple_of(500) {
                    tracing::info!(
                        so_far = n,
                        slot = index,
                        "a texture already on the card is brought up to date although the card has no room for new ones"
                    );
                }
            }
            // Gate new host texture creation / staging on commit reserve after
            // `mapped == 0` reclamation (`texture_slot_gone`) has run, and
            // before mirroring a slot to an unuploaded texture.
            if !crate::residency::admit_texture_pass(required) {
                served = false;
                break;
            }
            // A slot that shows the texture by an earlier view of it (one
            // from another level) goes on doing so until the upload of what
            // the new one shows has run.
            let sync_bake = sync.is_some();
            let shows_other = !baking
                && !sync_bake
                && crate::resource::held_only()
                && heap.handle(index).is_some_and(|handle| host.texture_slot_shows_other(index, handle));
            // A slot that shows nothing of it yet, or something else, shows it
            // when the upload has run too.
            let late = !shows_other
                && !baking
                && !sync_bake
                && crate::descriptor::host::show_after()
                && heap.handle(index).is_some_and(|handle| !host.texture_slot_made_from(index, handle));
            // The view and the record it came from are read once: the upload
            // and whatever shows the slot later refer to that record, not to
            // one the title wrote to the slot meanwhile.
            let (view, upload_key) = if sync_bake || shows_other || late {
                match heap.handle(index).and_then(|handle| host.prepare_texture_slot(index, handle)) {
                    Some((view, key)) => (Some(view), Some(key)),
                    None => (None, None),
                }
            } else {
                let view = heap.handle(index).and_then(|handle| host.mirror(index, handle, true));
                (view, None)
            };
            let at_once = || {
                if (late || (sync_bake && upload_key.is_some())) && let Some(handle) = heap.handle(index) {
                    let _ = host.mirror(index, handle, true);
                }
            };
            let placed = view.as_ref().and_then(crate::descriptor::host::placed_texture);
            if crate::residency::upload_refused() {
                served = false;
                break;
            }
            let resource = match placed.as_ref().or(view.as_ref()) {
                Some(&View::Shader(resource, _)) => resource as This,
                _ => {
                    at_once();
                    host.texture_slot_done(index, write, None);
                    continue;
                }
            };
            let texture = crate::resource::host_resource(resource);
            if crate::residency::upload_refused() {
                served = false;
                break;
            }
            // The card had no room for its host texture a moment ago
            // (`crate::resource::waits_for_room`): looked at again after 1,
            // 2, 4, ... frames, not done with as a texture the host writes
            // itself is. (Done with, nothing asked for it again until the
            // title wrote the slot anew, and it read black until then.)
            if texture.is_none() && crate::resource::waits_for_room(resource) {
                host.texture_slot_later(index, write, frame, most);
                continue;
            }
            let title_fills = texture.is_some_and(crate::texture::title_written);
            if !title_fills {
                at_once();
                // a render target, a depth or unordered access texture: the host's own
                host.texture_slot_done(index, write, None);
                continue;
            }
            // A texture that has a staging buffer now is one draws read
            // through a table: they upload it, and check it at every
            // submission that samples it.
            let tabled = crate::texture::is_staged(resource);
            // The host copy holds these very bytes: nothing to upload.
            let first = match (&placed, &view) {
                (Some(_), Some(View::Placed(_, _, desc))) => crate::resource::first_mip(desc),
                _ => 0,
            };
            let same = !tabled && crate::texture::unchanged(resource, first);
            if same {
                HEAP_TEXTURES_SAME.fetch_add(1, Ordering::Relaxed);
                if let (Some(sync), Some(key), Some(handle)) = (sync.as_deref_mut(), upload_key, heap.handle(index)) {
                    if let Some(copy) = crate::resource::host_copy(resource) {
                        sync.keep.push(copy);
                    }
                    sync.publish.push((index, handle, key));
                } else {
                    if shows_other && let Some(handle) = heap.handle(index) {
                        let _ = host.mirror(index, handle, true);
                    }
                    // Its bytes are in, or on the way there in a list before.
                    if late && texture.is_some_and(crate::pool::fresh) && !crate::texture::ahead::there(resource) {
                        if let (Some(handle), Some(key)) = (heap.handle(index), upload_key) {
                            host.texture_slot_show_after(list, index, handle, key);
                        }
                    } else {
                        at_once();
                    }
                }
                let again = partly.map(|mapped| (frame + 1, mapped));
                let memory = if again.is_none() && heap_recheck() { crate::texture::title_span(resource, first) } else { None };
                match memory {
                    Some((at, len)) => {
                        let print = crate::texture::fingerprint(at, len);
                        host.texture_slot_recheck(index, write, frame + again_in, (at, len), print, 0, frame);
                    }
                    None => host.texture_slot_done(index, write, again),
                }
                continue;
            }
            // The first upload of a texture no list has used: on a queue of
            // its own, and the slot shows it as soon as that has run.
            let ahead = if late && !tabled && texture.is_some_and(crate::pool::fresh) {
                crate::texture::ahead::take(resource, first, there.filter(|_| partly.is_none()).unwrap_or(u64::MAX))
            } else {
                None
            };
            if let Some(ahead) = &ahead {
                use crate::texture::ahead::Taken;
                let (Taken::New(job) | Taken::Known(job)) = ahead;
                if let (Some(handle), Some(key)) = (heap.handle(index), upload_key) {
                    host.texture_slot_show_ahead(job, index, handle, key);
                }
                if let Taken::New(job) = ahead {
                    crate::texture::ahead::send(job);
                }
            }
            if ahead.is_none() && crate::residency::upload_refused() {
                served = false;
                break;
            }
            let late = late && ahead.is_none();
            let _ = crate::residency::take_staging_deferred();
            if let Some(view) = &view
                && ahead.is_none()
            {
                // (What was found mapped of it a moment ago is not asked
                // again, unless the host copy may need more.)
                crate::texture::PICKING.set(Some(there.filter(|_| partly.is_none()).unwrap_or(u64::MAX)));
                readable(list, sampled.kind, view);
                crate::texture::PICKING.set(None);
            }
            // A failed or deferred staging allocation leaves this slot due.
            // Publishing it or marking it served would lose the only retry for
            // some materials.
            if ahead.is_none()
                && (crate::residency::take_staging_deferred() || crate::residency::upload_refused())
            {
                served = false;
                break;
            }
            if let (Some(sync), Some(key), Some(handle)) = (sync.as_deref_mut(), upload_key, heap.handle(index)) {
                if let Some(copy) = crate::resource::host_copy(resource) {
                    sync.keep.push(copy);
                }
                sync.publish.push((index, handle, key));
            }
            if shows_other && let (Some(handle), Some(key)) = (heap.handle(index), upload_key) {
                host.texture_slot_flip(list, index, handle, key);
            }
            if late && let (Some(handle), Some(key)) = (heap.handle(index), upload_key) {
                host.texture_slot_show_after(list, index, handle, key);
            }
            #[cfg(feature = "diagnostics")]
            if crate::book::on()
                && let Some(texture) = texture.filter(|t| crate::pool::fresh(t))
            {
                use windows::core::Interface;
                let raw = texture.as_raw() as usize;
                let asked = crate::host::submissions();
                list.defer_host(move || {
                    let (started, finished) = crate::host::submissions();
                    crate::book::note(raw, || {
                        format!(
                            "; upload for slot {index} recorded at submissions {asked:?}, sent as submission {started} (finished {finished}) at {:.3} s",
                            crate::host::now_ms() as f64 / 1e3
                        )
                    });
                });
            }
            // A texture view the host refuses removes the device: name it.
            static GONE: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
            if !GONE.load(Ordering::Relaxed)
                && let Some(why) = crate::descriptor::host::device_removed()
            {
                GONE.store(true, Ordering::Relaxed);
                let text: String = format!("{view:?}").chars().take(300).collect();
                tracing::error!(why, slot = index, "the host device went while textures picked by index were made ready; last: {text}");
                crate::host::log_removal_context();
                return false;
            }
            let (bytes, whole) = crate::texture::staged(resource, there.is_none()).unwrap_or((0, true));
            // Not all of it there: looked at again next frame, and from
            // then on as a slot with nothing new (a placed texture: until
            // more of it is mapped; a texture object at an address the
            // title mapped: again in 120 frames, as before).
            let again = match partly {
                Some(mapped) => Some((frame + 1, mapped)),
                None => (!whole).then_some((frame + 120, 0)),
            };
            if again.is_some() {
                HEAP_TEXTURES_PARTLY[1].fetch_add(1, Ordering::Relaxed);
            }
            if tabled {
                // Nothing was staged here, so nothing is counted or spent,
                // and the staging buffer stays: taking it away made the
                // next draw that reads the texture through a table stage,
                // detile and upload all of it once more.
                HEAP_TEXTURES_TABLED.fetch_add(1, Ordering::Relaxed);
                host.texture_slot_done(index, write, again);
                continue;
            }
            // Uploaded: the list holds the staging buffer until it has run;
            // a table read, or the next look at a texture not all mapped
            // yet, makes it anew. (Kept for those until now: every partly
            // streamed texture of the heap held a staging buffer of its full
            // size for good.)
            let memory = if again.is_none() && heap_recheck() {
                crate::texture::staged_memory(resource)
            } else {
                None
            };
            crate::texture::unstage(resource);
            SPENT.fetch_add(bytes, Ordering::Relaxed);
            since_asked += bytes;
            if room && since_asked >= 32 << 20 {
                since_asked = 0;
                room = crate::resource::room(crate::host::host().and_then(|h| h.video_memory()));
            }
            HEAP_TEXTURE_BYTES.fetch_add(bytes, Ordering::Relaxed);
            HEAP_TEXTURES.fetch_add(1, Ordering::Relaxed);
            match memory {
                // Whole and uploaded: looked at again (see above).
                Some((at, len)) => {
                    let print = crate::texture::fingerprint(at, len);
                    host.texture_slot_recheck(index, write, frame + again_in, (at, len), print, 0, frame);
                }
                None => host.texture_slot_done(index, write, again),
            }
        }
        if sync.is_none() && baking && served {
            BAKE_SERVED.store(host.texture_news().0 + 1, Ordering::Relaxed);
        }
        served
    }

    /// For the frame listing, after a dropped dispatch's line: its group
    /// counts and the compute root arguments it was recorded with (root
    /// constants as hex dwords, root views with the title buffer they
    /// point into). A program that does not translate has no bindings to
    /// list; these say which memory it would have read and written (the
    /// ray-tracing structure builder passes its buffers this way).
    #[cfg(feature = "diagnostics")]
    fn list_dropped_roots(&self, groups: [u32; 3]) {
        crate::capture::op(0, || {
            use std::fmt::Write;
            let [x, y, z] = groups;
            let mut text = format!("  its roots ({x}x{y}x{z}):");
            for (slot, root) in self.compute.iter().enumerate() {
                match root {
                    Root::None => {}
                    Root::Table(a) => {
                        let _ = write!(text, " {slot}=table {a:#x}");
                    }
                    Root::Constants(v) => {
                        let _ = write!(text, " {slot}=constants {:x?}", &v[..v.len().min(48)]);
                    }
                    Root::View(a) => {
                        let _ = write!(text, " {slot}=view {a:#x}");
                        match crate::shadow::extent(*a) {
                            Some((start, len)) => {
                                let _ = write!(text, " in buffer {start:#x}+{len:#x}");
                            }
                            None => text.push_str(" in no buffer"),
                        }
                    }
                }
            }
            text
        });
    }

    /// Runs a dispatch on the host.
    pub(crate) fn dispatch(&mut self, list: &HostList, x: u32, y: u32, z: u32, flags: u32) {
        // It may write the arguments a window holds clamped.
        self.window = None;
        #[cfg(feature = "diagnostics")]
        if crate::pipeline::is_skipped(self.pipeline as This) {
            DROPPED.fetch_add(1, Ordering::Relaxed);
            crate::drops::count(Kind::Dispatch, Why::SkipList);
            crate::pipeline::note_drop(self.pipeline as This);
            return;
        }
        // Timing experiment (`crate::indirect::batch::experiments`): the title's dispatches are left out.
        #[cfg(feature = "diagnostics")]
        if crate::indirect::batch::experiments() & 4 != 0 {
            return;
        }
        // The ray-tracing structure builder's stages run all or none.
        if crate::pipeline::is_builder(self.pipeline as This) && !crate::pipeline::builder_runs() {
            DROPPED.fetch_add(1, Ordering::Relaxed);
            crate::drops::count(Kind::Dispatch, Why::BuilderOff);
            #[cfg(feature = "diagnostics")]
            {
                crate::pipeline::note_why("a stage of the ray-tracing structure builder, which is off".to_owned());
                self.list_dropped_roots([x, y, z]);
            }
            return;
        }
        let Some(p) = crate::pipeline::host_pipeline(self.pipeline as This) else {
            once!(
                pipeline = format_args!("{:#x}", self.pipeline),
                "a dispatch without a host pipeline dropped"
            );
            DROPPED.fetch_add(1, Ordering::Relaxed);
            crate::drops::count(Kind::Dispatch, crate::pipeline::why_gone(self.pipeline as This));
            #[cfg(feature = "diagnostics")]
            {
                crate::pipeline::note_drop(self.pipeline as This);
                self.list_dropped_roots([x, y, z]);
            }
            return;
        };
        if !p.compute {
            once!("a dispatch with a graphics pipeline");
            return;
        }
        crate::drops::begin();
        if !self.bind(list, p, true) {
            DROPPED.fetch_add(1, Ordering::Relaxed);
            crate::drops::count_noted(Kind::Dispatch, Why::BindFailed);
            // A stage of the structure builder that cannot be bound is a
            // dropped stage: the others stop too.
            if crate::pipeline::is_builder(self.pipeline as This) {
                crate::pipeline::builder_rests("one of its dispatches could not be bound");
            }
            return;
        }
        #[cfg(feature = "diagnostics")]
        list.note_op(|| self.op_text(format!("dispatch {x}x{y}x{z}"), true));
        #[cfg(feature = "diagnostics")]
        if crate::pipeline::is_builder(self.pipeline as This) {
            crate::capture::op(list as *const _ as usize, || format!("builder {}", p.name));
            self.list_dropped_roots([x, y, z]);
        }
        // Counted in threads: whole groups of them, and the program is told
        // how many of the last one run, where it asks (its `bind` set that to
        // all of them).
        let size = (flags & THREAD_DIMENSIONS != 0 && thread_dimensions())
            .then(|| p.stages.first().and_then(|s| s.translated().threads))
            .flatten();
        let threads = [x, y, z];
        let [x, y, z] = size.map_or(threads, |size| dispatch_groups(threads, size));
        if pack_broken(p) {
            DROPPED.fetch_add(1, Ordering::Relaxed);
            crate::drops::count(Kind::Dispatch, Why::Other);
            return;
        }
        // A program translated to run several console groups to a host group (`LEONIDA_GROUP_PACK`):
        // the host runs the groups in fewer, told the console's grid.
        let [x, y, z] = match pack_of(p) {
            Some(pack) => {
                let (host, words) = leonida_shader::emit::pack_grid([x, y, z], pack.k);
                for (word, value) in pack.words.into_iter().zip(words) {
                    // SAFETY: an open list; the root signature has the word.
                    unsafe { list.list.SetComputeRoot32BitConstant(pack.param, value, word) };
                }
                // (Words of a parameter's constants: the cache has them whole.)
                self.cache.constant_set(true, pack.param);
                PACKED[0].fetch_add(1, Ordering::Relaxed);
                host
            }
            None => [x, y, z],
        };
        if size.is_some() {
            if !crate::pipeline::is_builder(self.pipeline as This) {
                once!(pipeline = p.name, "a dispatch counted in threads that is not the structure builder's");
            }
            if let Some((param, word)) = compute_word(p, leonida_shader::emit::USER_DISPATCH_THREADS_X) {
                // SAFETY: an open list; the root signature has the word.
                unsafe { list.list.SetComputeRoot32BitConstant(param, threads[0], word) };
                // (One word of a parameter's constants: the cache has them whole.)
                self.cache.constant_set(true, param);
            }
        }
        // SAFETY: an open list with everything set.
        unsafe { list.list.Dispatch(x, y, z) };
        self.dispatched(p);
        DISPATCHED.fetch_add(1, Ordering::Relaxed);
        if let Some(role) = p.stages.first().and_then(|s| s.shader.ambient)
            && crate::ambient::on()
        {
            let recorded = self.ambient(list, p, role);
            // Its passes set a compute root signature and pipeline of their own.
            self.cache.side_changed(true);
            if recorded {
                self.host_pipeline = 0;
                self.host_roots[1] = 0;
            }
        }
    }

    /// The stand-in for the indirect light after a dispatch of `p`
    /// (`crate::ambient`). Whether it recorded anything.
    fn ambient(&self, list: &HostList, p: &HostPipeline, role: crate::ambient::Role) -> bool {
        use crate::ambient::{AIR, DEPTH, LIGHT, RADIANCE, REFLECTIONS, Role, Views};
        let Some(t) = p.stages.first().map(|s| s.translated()) else {
            return false;
        };
        let Some(heap) = (self.heaps[0] != 0).then(|| crate::descriptor::heap(self.heaps[0] as This).host()).flatten()
        else {
            return false;
        };
        let entry = |kind: ResourceType, (slot, offset): (u8, u32)| {
            t.used.iter().find(|u| {
                u.kind == kind && u.root == RootType::Table && (u.slot, u.table_offset_dwords) == (slot, offset)
            })?;
            let Some(Root::Table(table)) = self.roots_ref(true).get(usize::from(slot)) else {
                return None;
            };
            let view = crate::descriptor::view(table + u64::from(offset) * 4)?;
            let view = crate::descriptor::host::placed_texture(&view).unwrap_or(view);
            let resource = match (kind, view) {
                (ResourceType::Srv, View::Shader(r, desc)) if desc.as_ref().is_none_or(is_depth_srv) => r,
                (ResourceType::Uav, View::Unordered(r, ..)) => r,
                _ => return None,
            };
            let start = match self.ambient_tables.iter().find(|m| m.0 == slot) {
                Some(m) => m.1,
                None => self.user_value(true, slot, RootType::Table, 0),
            };
            let host = crate::resource::host_resource(resource as This)?;
            // SAFETY: a live resource.
            let desc = unsafe { host.GetDesc() };
            Some((desc, heap.gpu_handle((start + offset * 4) / 32)))
        };
        let at = match role {
            Role::Resolve => LIGHT,
            Role::Radiance => RADIANCE,
            Role::Air => AIR,
            Role::Reflect => REFLECTIONS,
        };
        let Some((of, image)) = entry(ResourceType::Uav, at) else {
            once!(?role, "no stand-in for the indirect light: missing image UAV");
            return false;
        };
        let depth = match role {
            Role::Resolve => {
                let Some((seen, depth)) = entry(ResourceType::Srv, DEPTH) else {
                    once!(?role, "no stand-in for the indirect light: missing depth SRV");
                    return false;
                };
                if seen.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D
                    || seen.SampleDesc.Count != 1
                    || (seen.Width, seen.Height) != (of.Width, of.Height)
                {
                    once!(?role, "no stand-in for the indirect light: depth dimensions differ");
                    return false;
                }
                Some(depth)
            }
            Role::Radiance | Role::Air | Role::Reflect => None,
        };
        // (R32_UINT holding R9G9B9E5; R11G11B10_FLOAT.)
        let format = if matches!(role, Role::Resolve | Role::Reflect) { 42 } else { 26 };
        let volume = of.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        if of.Format.0 != format || volume != (role == Role::Air) {
            once!(?role, format = of.Format.0, "no stand-in for the indirect light: another image");
            return false;
        }
        crate::ambient::after(list, role, &Views {
            image,
            depth,
            size: [of.Width as u32, of.Height],
            slices: u32::from(of.DepthOrArraySize),
        })
    }

    /// What a draw or dispatch runs, for the device-removed report
    /// (diagnostics): the pipeline, then per stage the user data it gets
    /// (with the view at that heap index, if one is), the host addresses
    /// of its root views and the views its tables name.
    #[cfg(feature = "diagnostics")]
    fn op_text(&self, kind: String, compute: bool) -> String {
        use std::fmt::Write;
        let Some(p) = crate::pipeline::host_pipeline(self.pipeline as This) else {
            return kind;
        };
        let mut text = format!("{kind} {}", p.name);
        // What it draws into (a dispatch writes through its views below).
        if !compute {
            let described = |t: usize| {
                crate::resource::host_made(t as This).map(|r| {
                    // SAFETY: a live resource.
                    let d = unsafe { r.GetDesc() };
                    format!("{t:#x} {}x{} f{}", d.Width, d.Height, d.Format.0)
                })
            };
            let targets: Vec<String> = self.targets.iter().filter_map(|&t| described(t)).collect();
            let _ = write!(text, " targets {targets:?} depth {:?} state [{}]", described(self.depth), p.state);
            // The title's scissor and viewport at this draw (the first of
            // each): the title draws what it sees through a
            // portal (a window, from inside a room) under the portal's
            // rectangle, and the listing did not say which draws those are.
            if let Some(r) = self.scissors.first() {
                let _ = write!(text, " scissor [{} {} {} {}]", r.left, r.top, r.right, r.bottom);
            }
            if let Some(v) = self.viewports.first() {
                let _ = write!(
                    text,
                    " viewport [{} {} {} {} depth {}..{}]",
                    v.TopLeftX, v.TopLeftY, v.Width, v.Height, v.MinDepth, v.MaxDepth
                );
            }
        }
        let views = (self.heaps[0] != 0)
            .then(|| crate::descriptor::heap(self.heaps[0] as This).host())
            .flatten();
        for stage in &p.stages {
            let t = stage.translated();
            let _ = write!(text, " | {}:", stage.shader.hash);
            for u in &t.host.user_data {
                let v = match u.root {
                    RootType::Other(kind) => self.host_word(compute, kind, u.slot, u.dword, None),
                    _ => self.user_value(compute, u.slot, u.root, u.dword),
                };
                let _ = write!(text, " u{}={v:#x}", u.index);
                let index = if u.root == RootType::Table { v / 32 } else { v };
                if let Some(slot) = views.and_then(|h| h.describe_slot(index)) {
                    let _ = write!(text, " [heap {index}: {slot}]");
                }
            }
            for &(slot, _) in stage.root_cbvs.iter().chain(&stage.root_srvs).chain(&stage.root_uavs) {
                if let Some(Root::View(address)) = self.roots_ref(compute).get(usize::from(slot)) {
                    let host = crate::shadow::find(*address, 1)
                        .map(|s| (s.gpu(*address), s.left(*address)));
                    let _ = write!(text, " root{slot}={address:#x} host {host:#x?}");
                }
            }
        }
        let _ = write!(text, " tables {:?}", self.reads(compute));
        if !compute && !list_slots().is_empty() {
            text.push_str(&self.slots_text());
        }
        text
    }

    /// For the frame listing, after a draw whose shader `LEONIDA_LIST_SLOTS`
    /// names: the heap slots it will pick textures from by index, and what
    /// the title and the host hold there (one `~slot` line each), with the
    /// words they were read from (`~raw` lines, to decode offline what is
    /// not decoded here).
    ///
    /// Where the slots are, for the title's character shaders
    /// (`PS_DeferredPedTexturedNC`, from their native code): in the small
    /// buffers and constants of the draw's own table (every word of those
    /// that names a texture view gets a line); in the material record, at
    /// the byte offset the draw's constants (table 1, dword 56) hold at
    /// 0x1c, of the buffer at table 0, dword 160; and in the record at the
    /// byte offset the second root constant holds of the buffer at table 0,
    /// dword 688. A word that is a small number by chance and names a
    /// texture gets a line too.
    #[cfg(feature = "diagnostics")]
    fn slots_text(&self) -> String {
        use std::fmt::Write;
        const MATERIALS: u32 = 160;
        const GLOBAL: u32 = 688;
        const DRAW_CONSTANTS: u32 = 56;
        let wanted = list_slots();
        let Some(p) = crate::pipeline::host_pipeline(self.pipeline as This) else {
            return String::new();
        };
        let Some(stage) = p.stages.iter().find(|s| wanted.iter().any(|w| s.shader.hash.starts_with(w.as_str())))
        else {
            return String::new();
        };
        let Some(heap) = self
            .heaps
            .iter()
            .filter(|&&h| h != 0)
            .map(|&h| crate::descriptor::heap(h as This))
            .find(|h| !h.samplers())
        else {
            return String::new();
        };
        let t = stage.translated();
        let mut text = String::new();
        let entry = |slot: u8, dwords: u32| -> Option<View> {
            let Some(Root::Table(table)) = self.roots_ref(false).get(usize::from(slot)) else {
                return None;
            };
            crate::descriptor::view(table + u64::from(dwords) * 4)
        };
        let mut material = None;
        for u in t.used.iter().filter(|u| u.root == RootType::Table && u.kind != ResourceType::Sampler) {
            let label = format!("s{}+{}", u.slot, u.table_offset_dwords);
            let Some(view) = entry(u.slot, u.table_offset_dwords) else {
                let _ = write!(text, "\n    ~table {label} no view");
                continue;
            };
            let range = match &view {
                View::Constant(address, size) => Some((*address, u64::from(*size))),
                other => crate::descriptor::host::buffer_range(other),
            };
            match range {
                // A small buffer or constants: its words.
                Some((address, len)) if len <= 2048 => {
                    Self::slot_words(&mut text, heap, &label, address, len as usize);
                    if u.slot == 1 && u.table_offset_dwords == DRAW_CONSTANTS && len >= 0x20 {
                        let mut word = [0u8; 4];
                        if crate::host::read_into((address + 0x1c) as *const u8, &mut word) {
                            material = Some(u32::from_le_bytes(word));
                        }
                    }
                }
                Some(_) => {}
                // A texture read through the table: the title slot it sits in.
                None => {
                    let Some(Root::Table(table)) = self.roots_ref(false).get(usize::from(u.slot)) else {
                        continue;
                    };
                    match heap.index(table + u64::from(u.table_offset_dwords) * 4) {
                        Some(index) => text.push_str(&Self::slot_line(heap, &format!("table:{label}"), 0, index)),
                        None => {
                            let _ = write!(text, "\n    ~table {label} title=[{}]", view_state(Some(&view)));
                        }
                    }
                }
            }
        }
        let uses = |slot: u8, dwords: u32| {
            t.used.iter().any(|u| u.root == RootType::Table && u.slot == slot && u.table_offset_dwords == dwords)
        };
        if let Some(at) = material
            && uses(0, MATERIALS)
            && let Some((base, len)) = entry(0, MATERIALS).as_ref().and_then(crate::descriptor::host::buffer_range)
        {
            let at = u64::from(at);
            let _ = write!(text, "\n    ~material record at {at:#x} of the buffer at {base:#x} bytes {len}");
            if at + 64 <= len {
                Self::slot_words(&mut text, heap, "M", base + at, 64);
            }
            if at + 1936 + 512 <= len {
                Self::slot_words(&mut text, heap, "M+1936", base + at + 1936, 512);
            }
        }
        if uses(0, GLOBAL)
            && let Some(slot) = t.used.iter().find(|u| u.root == RootType::Constants).map(|u| u.slot)
            && let Some(Root::Constants(words)) = self.roots_ref(false).get(usize::from(slot))
            && let Some(&at) = words.get(1)
            && let Some((base, len)) = entry(0, GLOBAL).as_ref().and_then(crate::descriptor::host::buffer_range)
        {
            let at = u64::from(at);
            let _ = write!(text, "\n    ~root constants {words:08x?}: record at {at:#x} of the buffer at {base:#x} bytes {len}");
            if at + 64 <= len {
                Self::slot_words(&mut text, heap, "G", base + at, 64);
            }
        }
        text
    }

    /// Up to 512 bytes of title memory at `address` as hex words (a `~raw`
    /// line), and a `~slot` line for every word of them that names a
    /// texture view in `heap`.
    #[cfg(feature = "diagnostics")]
    fn slot_words(text: &mut String, heap: &crate::descriptor::Heap, label: &str, address: u64, len: usize) {
        use std::fmt::Write;
        let mut bytes = vec![0u8; len.min(512) & !3];
        if bytes.is_empty() || !crate::host::read_into(address as *const u8, &mut bytes) {
            let _ = write!(text, "\n    ~raw {label} at {address:#x} bytes {len}: unreadable");
            return;
        }
        let words: Vec<u32> =
            bytes.chunks_exact(4).map(|w| u32::from_le_bytes([w[0], w[1], w[2], w[3]])).collect();
        let _ = write!(text, "\n    ~raw {label} at {address:#x} bytes {len}:");
        for w in &words {
            let _ = write!(text, " {w:08x}");
        }
        for (i, &index) in words.iter().enumerate() {
            let names = index != 0
                && heap
                    .handle(index)
                    .and_then(crate::descriptor::view)
                    .as_ref()
                    .is_some_and(crate::descriptor::host::is_texture_view);
            if names {
                text.push_str(&Self::slot_line(heap, label, i * 4, index));
            }
        }
    }

    /// One `~slot` line: where the slot number was read (`from`, byte
    /// `at`), what the title's heap holds in slot `index` with the state
    /// of that texture ([`view_state`]), what the host slot was made from,
    /// and what the queue of textures picked by index knows of it.
    #[cfg(feature = "diagnostics")]
    fn slot_line(heap: &crate::descriptor::Heap, from: &str, at: usize, index: u32) -> String {
        let title = heap.handle(index).and_then(crate::descriptor::view);
        let host: String = heap
            .host()
            .and_then(|h| h.describe_slot(index))
            .unwrap_or_else(|| "none".into())
            .chars()
            .take(90)
            .collect();
        let queue = match heap.host() {
            Some(h) => h.texture_slot_state(index, crate::frame::current()),
            None => "no host heap".into(),
        };
        format!(
            "\n    ~slot from={from}+{at:#x} slot={index} title=[{}] host=[{host}] queue=[{queue}]",
            view_state(title.as_ref())
        )
    }

    /// A mesh draw under `LEONIDA_MESH_TEST=scissor` rasterises one pixel:
    /// the scissors before it (`on`), the title's after it.
    fn mesh_scissor(&self, list: &HostList, on: bool) {
        if mesh_test() != 2 {
            return;
        }
        if on {
            let one = RECT { left: 0, top: 0, right: 1, bottom: 1 };
            crate::draw::scissors(list, &vec![one; self.scissors.len().max(1)]);
        } else if !self.scissors.is_empty() {
            crate::draw::scissors(list, &self.scissors);
        }
    }

    /// Runs a mesh dispatch of `x` groups on the host (the title's mesh
    /// programs are one-dimensional).
    pub(crate) fn dispatch_mesh(&mut self, list: &HostList, x: u32) {
        self.window = None;
        #[cfg(feature = "diagnostics")]
        self.trace("mesh", x, 0);
        // (A test of the mesh programs leaves some out on purpose.)
        let held = mesh_test() == 1 || !mesh_kept(self.pipeline);
        if held {
            crate::drops::begin();
            crate::drops::note(Why::SkipList);
        }
        let ran = !held
            && self.prepare_for(list, false, true)
            && match list.list.cast::<ID3D12GraphicsCommandList6>() {
                Ok(l6) => {
                    #[cfg(feature = "diagnostics")]
                    list.note_op(|| self.op_text(format!("mesh {x}"), false));
                    self.mesh_scissor(list, true);
                    #[cfg(feature = "diagnostics")]
                    if let Some(p) = crate::pipeline::host_pipeline(self.pipeline as This) {
                        list.ran_indirect(p.pso.as_raw() as usize, &p.name, crate::host::indirects::DIRECT, x, &self.user);
                    }
                    // (The host takes 65,535 groups in a dimension: more is
                    // undefined there.)
                    if x > 65_535 {
                        once!(x, "a mesh dispatch of more groups than the host takes: cut");
                    }
                    // SAFETY: an open list with everything set.
                    unsafe { l6.DispatchMesh(x.min(65_535), 1, 1) };
                    self.drew(crate::pipeline::host_pipeline(self.pipeline as This));
                    self.mesh_scissor(list, false);
                    DRAWN.fetch_add(1, Ordering::Relaxed);
                    true
                }
                Err(_) => false,
            };
        if !ran {
            DROPPED.fetch_add(1, Ordering::Relaxed);
            crate::drops::count_noted(Kind::Draw, Why::Other);
            #[cfg(feature = "diagnostics")]
            dropped(self.pipeline);
        }
    }

    /// The walls trace (`crate::host::walls_trace`): one log line per
    /// frame with the draws of its main pass (four or more colour targets)
    /// by scissor rectangle, and how often each of the title's culling
    /// programs was dispatched. The title clips what it draws
    /// through a window or door to that opening's rectangle while it holds
    /// the camera to be in a room (run 182, frame 1529: 1,386 draws under
    /// `531 0 1789 1337`); in a launch without the room's walls the same
    /// shot has no such rectangle (run 187, frame 1414). The line shows
    /// the frame in which that changes.
    fn walls_census(&self, p: &HostPipeline, compute: bool) {
        const CULL: [(&str, &str); 9] = [
            ("55679a0547649d0b", "tree"),
            ("89d08e561897d4e5", "tree_imposter"),
            ("9209777e8face9dd", "grass"),
            ("17af15c4015cb4b6", "prop"),
            ("fdd56d97a806bcca", "prop_nocull"),
            ("42f717f1b4d18e94", "simple"),
            ("1c4b3c0547e3e6fa", "slod"),
            ("9146291b38ea9ce1", "meshlet_args"),
            ("42683eebec081520", "depth_pyramid_cpu_occlusion"),
        ];
        type Census = (u64, std::collections::BTreeMap<[i32; 4], u32>, [u32; 9]);
        static CENSUS: std::sync::Mutex<Census> =
            std::sync::Mutex::new((u64::MAX, std::collections::BTreeMap::new(), [0; 9]));
        let frame = crate::frame::current();
        let mut census = CENSUS.lock().unwrap_or_else(|e| e.into_inner());
        if census.0 != frame {
            let draws: u32 = census.1.values().sum();
            if draws > 0 {
                let mut rects: Vec<([i32; 4], u32)> = census.1.iter().map(|(r, n)| (*r, *n)).collect();
                rects.sort_unstable_by(|a, b| b.1.cmp(&a.1));
                let scissors: Vec<String> = rects
                    .iter()
                    .take(5)
                    .map(|(r, n)| format!("{} {} {} {} x{n}", r[0], r[1], r[2], r[3]))
                    .collect();
                let counts = census.2;
                let culling: Vec<String> = CULL
                    .iter()
                    .zip(counts)
                    .filter(|(_, n)| *n > 0)
                    .map(|(c, n)| format!("{}={n}", c.1))
                    .collect();
                tracing::info!(
                    frame = census.0,
                    main_pass_draws = draws,
                    rectangles = rects.len(),
                    scissors = ?scissors,
                    culling = ?culling,
                    "walls trace: the frame's main pass by scissor rectangle, and its culling dispatches"
                );
            }
            census.0 = frame;
            census.1.clear();
            census.2 = [0; 9];
        }
        if compute {
            for (i, c) in CULL.iter().enumerate() {
                if p.stages.iter().any(|s| s.shader.hash.as_str() == c.0) {
                    census.2[i] += 1;
                }
            }
        } else if self.targets.iter().filter(|&&t| t != 0).count() >= 4 {
            let r = self.scissors.first().map_or([0; 4], |r| [r.left, r.top, r.right, r.bottom]);
            *census.1.entry(r).or_insert(0) += 1;
        }
    }

    fn roots_ref(&self, compute: bool) -> &Vec<Root> {
        if compute {
            &self.compute
        } else {
            &self.graphics
        }
    }

    fn table_entry(&self, list: &HostList, u: &UsedResource, compute: bool) {
        let Some(Root::Table(table)) = self.roots_ref(compute).get(usize::from(u.slot)) else {
            return;
        };
        let handle = table + u64::from(u.table_offset_dwords) * 4;
        let Some(heap) = self.heap_of(handle) else {
            return;
        };
        let (Some(host), Some(index)) = (heap.host(), heap.index(handle)) else {
            return;
        };
        if let Some(view) = host.mirror(index, handle, true) {
            readable(list, u.kind, &view);
            // Up to date by this draw: not the texture pass's to make.
            if crate::descriptor::host::is_texture_view(&view) {
                host.texture_slot_tabled(index, true);
            } else {
                host.slot_tabled(index);
            }
        } else if u.kind == ResourceType::Sampler {
            // Leonida (diagnostics): a shader samples through a slot whose
            // record is no sampler of ours, so the slot holds the point
            // stand-in (`descriptor/host.rs` `null_sampler`). The first few
            // slots are named, with the record as the title has it: the
            // posterized picture of some runs is a table of colours read
            // without filtering, and this says through which slot.
            #[cfg(feature = "diagnostics")]
            {
                static SEEN: std::sync::Mutex<Vec<u32>> = std::sync::Mutex::new(Vec::new());
                let mut seen = SEEN.lock().unwrap_or_else(|e| e.into_inner());
                if seen.len() < 32 && !seen.contains(&index) {
                    seen.push(index);
                    // SAFETY: a descriptor slot in title memory (`mirror` just read it).
                    let record = unsafe { (handle as *const [u32; 8]).read_unaligned() };
                    tracing::warn!(
                        slot = index,
                        frame = crate::frame::current(),
                        pipeline = format_args!("{:#x}", self.pipeline),
                        compute,
                        record = ?record.map(|w| format!("{w:#x}")),
                        "a shader samples through a slot without a sampler of ours: it reads the point stand-in"
                    );
                }
            }
        }
    }

    /// The descriptors a shader reads through tables. Tables holding a
    /// constant buffer are made afresh for the draw in dynamic slots, with
    /// the constant data copied to upload memory when the list is
    /// submitted: the host wants constant buffers 256-byte aligned, the
    /// console only 16. Returns those tables' new byte offsets by root
    /// parameter; the other tables' slots are made from the title's.
    ///
    /// Within a list, the same table holding the same descriptors is made
    /// once: its constant data is copied at submission either way, and the
    /// big tables the world binds would otherwise take hundreds of slots per
    /// draw, wrapping the dynamic slots onto lists not yet submitted.
    ///
    /// `half`: the table descriptors the shader moves sixteen bits a
    /// channel as unsigned integers (`HostShader::half_views`). A table
    /// with a 16-bit float or signed buffer view at one of them is made
    /// afresh as well, with that view in the unsigned format of the same
    /// layout (`descriptor::host::half_view`): the title's own slot keeps
    /// the title's format for the shaders that read it as floats.
    pub(crate) fn tables(
        &mut self,
        list: &HostList,
        plan: &TablePlan,
        half: &[(u8, u32)],
        compute: bool,
    ) -> Few<(u8, u32), 16> {
        let mut fresh = [0u64; 4];
        let selected_slot = |mask: &[u64; 4], slot: u8| mask[usize::from(slot / 64)] & (1u64 << (slot % 64)) != 0;
        for group in &plan.groups {
            let slot = group.slot;
            let needs_snapshot = group.has_cbv
                || match self.roots_ref(compute).get(usize::from(slot)) {
                    Some(&Root::Table(table)) => self.heap_of(table).is_some_and(|heap| {
                        heap.host()
                            .is_some_and(|host| group.needs_snapshot(heap, host.count(), table))
                    }),
                    _ => false,
                };
            if needs_snapshot {
                fresh[usize::from(slot / 64)] |= 1u64 << (slot % 64);
            }
        }
        // LOCAL (faces): the view shown in place of the title's at dword `at`
        // of the table in root parameter `slot`, if the shader moves halves
        // there and the title's view is a 16-bit float or signed buffer view.
        let halved = |state: &Self, slot: u8, at: u32| -> Option<View> {
            if !half.contains(&(slot, at)) {
                return None;
            }
            let Some(&Root::Table(table)) = state.roots_ref(compute).get(usize::from(slot)) else {
                return None;
            };
            crate::descriptor::host::half_view(&crate::descriptor::view(table + u64::from(at) * 4)?)
        };
        for &(slot, at) in half {
            let Some(&Root::Table(table)) = self.roots_ref(compute).get(usize::from(slot)) else {
                continue;
            };
            let Some(view) = crate::descriptor::view(table + u64::from(at) * 4) else {
                continue;
            };
            if crate::descriptor::host::half_view(&view).is_some() {
                fresh[usize::from(slot / 64)] |= 1u64 << (slot % 64);
            } else if let Some(format) = crate::descriptor::host::half_inexact(&view) {
                once!(
                    format,
                    "a shader moves a buffer's 16-bit channels as integers through a view whose values the console converts: they are read and written unconverted"
                );
            }
        }
        for &(g, e) in &plan.direct {
            let group = &plan.groups[g];
            if !selected_slot(&fresh, group.slot) {
                self.table_entry(list, &group.entries[e], compute);
            }
        }
        let mut made = Few::default();
        for group in &plan.groups {
            let slot = group.slot;
            if !selected_slot(&fresh, slot) {
                continue;
            }
            let Some(&Root::Table(table)) = self.roots_ref(compute).get(usize::from(slot)) else {
                continue;
            };
            let entries = &group.entries;
            let Some(heap) = self.heap_of(table) else {
                continue;
            };
            if heap.samplers() || (table - heap.start) % 32 != 0 {
                continue;
            }
            let Some(host) = heap.host() else {
                continue;
            };
            let mut selected: Vec<u32> = group
                .indexed
                .iter()
                .map(|index| {
                    let constant = self.user_value(
                        compute,
                        index.constant_slot,
                        RootType::Constants,
                        index.constant_dword,
                    );
                    // The shader adds i32 byte offsets and shifts by 5.
                    index.byte_offset.wrapping_add(constant << index.shift) / 32
                })
                .collect();
            // The descriptors the shader moves halves through are made too,
            // and the table reaches them: they may lie past the range starts
            // the entries give (a range of several views names only its
            // first; review on #927).
            selected.extend(
                half.iter()
                    .filter(|&&(s, _)| s == slot)
                    .map(|&(_, at)| at / 8),
            );
            selected.sort_unstable();
            selected.dedup();
            let n = (group.max_dword / 8).max(selected.last().copied().unwrap_or(0)) + 1;
            let last = table.checked_add(u64::from(n - 1) * 32);
            if n > crate::descriptor::host::DYNAMIC
                || last.and_then(|handle| heap.index(handle)).is_none()
            {
                once!(slot, n, "fresh descriptor table exceeds the available span");
                continue;
            }
            // The table and what its entries hold (record words).
            let key = {
                let mut h = RecordHash::default();
                for k in 0..n {
                    let at = table + u64::from(k) * 32;
                    // SAFETY: a descriptor slot in title memory.
                    h.add(unsafe { (at as *const [u32; 3]).read_unaligned() });
                    // (Another table than the same slots made for a shader
                    // that reads them in the title's formats.)
                    if half.contains(&(slot, k * 8)) {
                        h.add([u32::MAX, slot.into(), k]);
                    }
                }
                (table, n, h.0)
            };
            // Who reads of it, and as what, as far as [`readable`] goes by it.
            let reader = (std::ptr::from_ref(plan) as usize, plan.direct.len(), STORES.get(), TESTED_DEPTH.get().0);
            if let Some(known) = self.fresh_tables.get_mut(&key) {
                let first = known.first;
                // This reader had it brought up to date, and nothing in the
                // list changed since: nothing to do again.
                let once = ready_once() && selected.is_empty() && !group.has_uav;
                // (A table with a view of an aliased texture is gone
                // through again: `list_alias_exact`.)
                let mut again = None;
                if once
                    && known.epoch == list.epoch()
                    && let Some((_, depth)) = known.ready.iter().find(|(r, _)| *r == reader)
                {
                    if !(known.aliased && list_alias_exact()) {
                        if *depth {
                            TESTED_DEPTH.set((reader.3, true));
                        }
                        made.push((slot, first * 32));
                        continue;
                    }
                    again = Some((list.epoch(), list.deferred_steps()));
                }
                let depth_before = TESTED_DEPTH.get();
                TESTED_DEPTH.set((depth_before.0, false));
                let mut aliased = false;
                for u in entries.iter().filter(|u| u.kind != ResourceType::Cbv) {
                    let handle = table + u64::from(u.table_offset_dwords) * 4;
                    if let Some(view) = crate::descriptor::view(handle) {
                        aliased |= readable(list, u.kind, &view);
                    }
                }
                for k in selected {
                    if group.entry(k).is_some() {
                        continue;
                    }
                    let handle = table + u64::from(k) * 32;
                    if let Some(view) = crate::descriptor::view(handle)
                        && let Some(kind) = view_kind(&view)
                    {
                        aliased |= readable(list, kind, &view);
                    }
                }
                let depth = TESTED_DEPTH.get().1;
                TESTED_DEPTH.set((depth_before.0, depth_before.1 || depth));
                if let Some(before) = again {
                    LIST_ALIAS[2].fetch_add(1, Ordering::Relaxed);
                    if before != (list.epoch(), list.deferred_steps()) {
                        LIST_ALIAS[3].fetch_add(1, Ordering::Relaxed);
                    }
                }
                if aliased && let Some(known) = self.fresh_tables.get_mut(&key) {
                    // (For every reader from here on: they read different
                    // entries of it, and one flag says it of them all.)
                    known.aliased = true;
                }
                if once && let Some(known) = self.fresh_tables.get_mut(&key) {
                    // (The epoch as it is now: what this changed itself it
                    // need not do again.)
                    let epoch = list.epoch();
                    if known.epoch != epoch {
                        known.epoch = epoch;
                        known.ready.clear();
                    }
                    if known.ready.len() < 16 {
                        known.ready.push((reader, depth));
                    }
                }
                made.push((slot, first * 32));
                continue;
            }
            let Some(first) = crate::host::table_slots(host, n) else {
                // No room: the draw must not run with the title's own
                // offsets (slots never made for it); `bind` drops it.
                crate::drops::note(host.refusal(n));
                made.push((slot, u32::MAX));
                continue;
            };
            self.fresh_tables.insert(key, FreshTable { first, epoch: 0, ready: Vec::new(), aliased: false });
            self.dynamic
                .push((std::ptr::from_ref(heap) as usize, host.id(), first, n));
            // The title's own slots of this table: what the draw reads of
            // them it brings up to date here, in the dynamic slots.
            let title_first = heap.index(table).filter(|first| first + n <= host.count());
            // They are such a table's, whatever they hold. (Of a big one,
            // which may lie over slots shaders pick by index, only the ones
            // the shader names.)
            if let Some(title) = title_first {
                for k in (0..n).filter(|&k| n <= 64 || group.entry(k).is_some()) {
                    host.slot_tabled(title + k);
                }
            }
            let list_views = crate::descriptor::host::list_views();
            if self.recording == 0 {
                static NEXT: AtomicU64 = AtomicU64::new(1);
                self.recording = NEXT.fetch_add(1, Ordering::Relaxed);
            }
            let recording = self.recording;
            // The whole range: a shader may index past the entries its
            // used-resource list names (descriptor arrays).
            for k in 0..n {
                let index = first + k;
                // A view this list has made before: its descriptor copied,
                // and nothing more if nothing changed in the list since.
                if list_views
                    && let Some(words) = crate::descriptor::record_at(table + u64::from(k) * 32)
                    && let Some(mut listed) = host.make_listed(recording, words, index)
                {
                    let kind = group
                        .entry(k)
                        .map(|u| u.kind)
                        .or_else(|| selected.binary_search(&k).ok().and(listed.kind));
                    let was = (listed.ready, listed.tabled, listed.aliased);
                    if let Some(kind) = kind {
                        let tested = TESTED_DEPTH.get();
                        let ready = kind == ResourceType::Srv && listed.ready.0 == list.epoch() && listed.ready.1 == tested.0;
                        // A view of an aliased texture is not known ready by
                        // the epoch: its copy follows its source's writes
                        // (`list_alias_exact`).
                        let again = ready && listed.aliased && list_alias_exact();
                        if ready && !again {
                            if listed.ready.2 {
                                TESTED_DEPTH.set((tested.0, true));
                            }
                        } else if let Some(view) = crate::descriptor::host::listed_view(recording, words) {
                            let before = (list.epoch(), list.deferred_steps());
                            let _ = crate::residency::take_staging_deferred();
                            TESTED_DEPTH.set((tested.0, false));
                            let aliased = readable(list, kind, &view);
                            let depth = TESTED_DEPTH.get().1;
                            TESTED_DEPTH.set((tested.0, tested.1 || depth));
                            if kind == ResourceType::Srv && !crate::residency::take_staging_deferred() {
                                listed.ready = (list.epoch(), tested.0, depth);
                                listed.aliased = aliased;
                            }
                            if again {
                                LIST_ALIAS[0].fetch_add(1, Ordering::Relaxed);
                                if before != (list.epoch(), list.deferred_steps()) {
                                    // Taken as ready, the draw read the copy as it was.
                                    LIST_ALIAS[1].fetch_add(1, Ordering::Relaxed);
                                }
                            }
                        }
                    }
                    if let Some(title) = title_first
                        && listed.texture
                        && listed.tabled != Some((title + k, kind.is_some()))
                    {
                        listed.tabled = Some((title + k, kind.is_some()));
                        host.texture_slot_tabled(title + k, kind.is_some());
                        if kind.is_some() {
                            host.mirror_after(list, title + k, table + u64::from(k) * 32);
                        }
                    }
                    if was != (listed.ready, listed.tabled, listed.aliased) {
                        crate::descriptor::host::note_listed(recording, words, listed);
                    }
                    continue;
                }
                let used = group.entry(k);
                match crate::descriptor::view(table + u64::from(k) * 32) {
                    Some(View::Constant(address, size)) => {
                        // Renderer port 5: title memory in place is the
                        // card's to read where it is; nothing is copied.
                        if crate::port_cbv::in_place_on() {
                            match crate::shadow::find(address, u64::from(size)).filter(|s| s.in_place()) {
                                Some(shadow) => {
                                    if host.constant_raw_at(index, &shadow.buffer, shadow.gpu(address), size) {
                                        crate::port_cbv::in_place(size);
                                        continue;
                                    }
                                    crate::port_cbv::copied(false);
                                }
                                None => crate::port_cbv::copied(true),
                            }
                        }
                        let Some((buffer, gpu, to)) = list.scratch_held(u64::from(size)) else {
                            host.make_null(index);
                            continue;
                        };
                        let slot = (std::ptr::from_ref(heap) as usize, index);
                        list.defer_constants(to, address as *const u8, size as usize, slot);
                        host.constant_at(index, &buffer, gpu, size);
                    }
                    Some(view) => {
                        match halved(self, slot, k * 8) {
                            Some(shown) => host.make_at(index, &shown),
                            None => host.make_at(index, &view),
                        }
                        let kind = used.map(|u| u.kind).or_else(|| {
                            selected.binary_search(&k).ok().and_then(|_| view_kind(&view))
                        });
                        if let Some(kind) = kind {
                            readable(list, kind, &view);
                        }
                        // The title's slot is a table's: out of the queue
                        // of textures picked by index if the draw read it
                        // (its texture is up to date now), and made in
                        // place as the texture pass would have made it,
                        // for a shader that picks it by index all the same.
                        if let Some(title) = title_first
                            && crate::descriptor::host::is_texture_view(&view)
                        {
                            host.texture_slot_tabled(title + k, kind.is_some());
                            if kind.is_some() {
                                host.mirror_after(list, title + k, table + u64::from(k) * 32);
                            }
                        }
                    }
                    None => {
                        host.make_null(index);
                        if used.is_some() {
                            once!(slot, "a table entry without a view");
                        }
                    }
                }
            }
            made.push((slot, first * 32));
        }
        made
    }

    /// The first draws, at debug level: what they bind and read
    /// (diagnostics builds).
    #[cfg(feature = "diagnostics")]
    fn trace(&self, kind: &str, count: u32, first: u32) {
        static TRACED: AtomicU64 = AtomicU64::new(0);
        if !tracing::enabled!(tracing::Level::DEBUG) {
            return;
        }
        let listed = crate::capture::listed(crate::queue::presented() + 1);
        let logged = listed || TRACED.fetch_add(1, Ordering::Relaxed) < 64;
        let targets: Vec<String> = self
            .targets
            .iter()
            .filter_map(|&t| crate::resource::host_made(t as This))
            .map(|t| {
                // SAFETY: a live resource.
                let d = unsafe { t.GetDesc() };
                format!("{}x{} f{}", d.Width, d.Height, d.Format.0)
            })
            .collect();
        let v = self.vertex[0];
        let line = format!(
            "{kind} {count} from {first} topology {} pipeline {:#x} targets {targets:?} vb {:#x}+{} stride {} reads {:?}",
            self.topology,
            self.pipeline,
            v.address,
            v.size,
            v.stride,
            self.reads(false)
        );
        if logged {
            tracing::debug!("draw {line}");
        }
        let mut frame = FRAME.lock().unwrap_or_else(|e| e.into_inner());
        if frame.len() < 256 {
            frame.push(line);
        }
    }

    /// What a draw's shaders read, one line per used resource (diagnostics
    /// builds).
    #[cfg(feature = "diagnostics")]
    fn reads(&self, compute: bool) -> Vec<String> {
        let Some(p) = crate::pipeline::host_pipeline(self.pipeline as This) else {
            return vec!["no host pipeline".into()];
        };
        let mut out = Vec::new();
        for stage in &p.stages {
            let t = stage.translated();
            for u in &t.used {
                let what = match (u.root, self.roots_ref(compute).get(usize::from(u.slot))) {
                    (RootType::Table, Some(Root::Table(table))) => {
                        let handle = table + u64::from(u.table_offset_dwords) * 4;
                        describe(crate::descriptor::view(handle).as_ref())
                    }
                    (root, _) => format!("{root:?}"),
                };
                out.push(format!(
                    "{} {:?} s{}+{}: {what}",
                    stage.shader.hash, u.kind, u.slot, u.table_offset_dwords
                ));
            }
        }
        out
    }

    pub(crate) fn draw(
        &mut self,
        list: &HostList,
        vertices: u32,
        instances: u32,
        first: u32,
        first_instance: u32,
    ) {
        self.window = None;
        #[cfg(feature = "diagnostics")]
        self.trace("draw", vertices, first);
        let ready = self.prepare(list, false);
        // A pipeline with the rectangle geometry stage draws the title's
        // vertices as they are; any other gets its rectangles expanded.
        let rects = self.topology == RECTANGLES && !self.completes_rects();
        let ran = ready && (!rects || self.rectangles(list, vertices, first));
        if ran {
            // Rectangles were expanded into their own vertex buffers.
            let (count, from) = if rects {
                (vertices / 3 * 6, 0)
            } else {
                (vertices, first)
            };
            #[cfg(feature = "diagnostics")]
            list.note_op(|| self.op_text(format!("draw {count}x{instances}"), false));
            // SAFETY: an open list with everything set.
            unsafe {
                list.list
                    .DrawInstanced(count, instances, from, first_instance)
            };
            self.drew(crate::pipeline::host_pipeline(self.pipeline as This));
            DRAWN.fetch_add(1, Ordering::Relaxed);
        }
        if !ran {
            DROPPED.fetch_add(1, Ordering::Relaxed);
            crate::drops::count_noted(Kind::Draw, Why::Other);
            #[cfg(feature = "diagnostics")]
            dropped(self.pipeline);
        }
    }

    /// Whether the bound pipeline completes rectangles itself, in the
    /// geometry stage rectangle pipelines get (`HostPipeline::rects`).
    fn completes_rects(&self) -> bool {
        crate::pipeline::host_pipeline(self.pipeline as This).is_some_and(|p| p.rects)
    }

    /// Binds vertex buffers holding the rectangles of vertices `first..first
    /// + count` as triangles, made when the list is submitted.
    fn rectangles(&mut self, list: &HostList, count: u32, first: u32) -> bool {
        let Some(p) = crate::pipeline::host_pipeline(self.pipeline as This) else {
            return false;
        };
        let rects = u64::from(count / 3);
        for slot in 0..32 {
            let elements: Vec<(u32, u32)> = p
                .inputs
                .iter()
                .filter(|e| e.slot == slot && !e.per_instance)
                .map(|e| (e.offset, e.format))
                .collect();
            if elements.is_empty() {
                continue;
            }
            let v = self.vertex[slot as usize];
            let stride = u64::from(v.stride);
            let from = v.address + u64::from(first) * stride;
            if v.address == 0
                || stride == 0
                || u64::from(first + count) * stride > u64::from(v.size)
                || crate::shadow::find(from, rects * 3 * stride).is_none()
            {
                once!(slot, "rectangle vertices outside their buffer");
                return false;
            }
            let Some((gpu, to)) = list.scratch(rects * 6 * stride) else {
                return false;
            };
            let (from, to, stride) = (from as usize, to as usize, stride as usize);
            let n = rects as usize;
            list.defer_host_read(from, n * 3 * stride, move || {
                // The vertices are read through the kernel, at submission; in
                // place only if that fails, with no change of mapping under
                // the read (see `crate::watch::title_memory_reading`): while
                // the world streams a change is under way most of the time,
                // and this is the thread that submits.
                let mut read = vec![0u8; n * 3 * stride];
                let held = (!crate::host::read_into(from as *const u8, &mut read)).then(crate::watch::title_memory_reading);
                // SAFETY: the vertices lie in a title buffer (checked when
                // recorded) and the scratch memory is the list's.
                let (src, dst) = unsafe {
                    (
                        if held.is_some() { std::slice::from_raw_parts(from as *const u8, n * 3 * stride) } else { &read[..] },
                        std::slice::from_raw_parts_mut(to as *mut u8, n * 6 * stride),
                    )
                };
                crate::primitive::expand_rects(src, dst, stride, &elements);
            });
            let view = D3D12_VERTEX_BUFFER_VIEW {
                BufferLocation: gpu,
                SizeInBytes: (rects * 6 * stride as u64) as u32,
                StrideInBytes: v.stride,
            };
            // (Not what the cache holds for the slot; the next draw sets its own.)
            self.cache.vertex_set(slot);
            // SAFETY: an open list; the view names live scratch memory.
            unsafe { list.list.IASetVertexBuffers(slot, Some(&[view])) };
        }
        true
    }

    pub(crate) fn draw_indexed(
        &mut self,
        list: &HostList,
        indices: u32,
        instances: u32,
        first: u32,
        base: i32,
        first_instance: u32,
    ) {
        self.window = None;
        #[cfg(feature = "diagnostics")]
        self.trace("indexed", indices, first);
        // Indexed rectangles are drawn by the rectangle geometry stage
        // alone (nothing expands them on the CPU).
        let lost = self.topology == RECTANGLES && !self.completes_rects();
        if lost {
            once!("indexed rectangles not drawn");
            crate::drops::begin();
        }
        let ran = !lost && self.prepare(list, true);
        if ran {
            #[cfg(feature = "diagnostics")]
            list.note_op(|| self.op_text(format!("indexed {indices}x{instances}"), false));
            // SAFETY: an open list with everything set.
            unsafe {
                list.list
                    .DrawIndexedInstanced(indices, instances, first, base, first_instance)
            };
            self.drew(crate::pipeline::host_pipeline(self.pipeline as This));
            DRAWN.fetch_add(1, Ordering::Relaxed);
        }
        if !ran {
            DROPPED.fetch_add(1, Ordering::Relaxed);
            crate::drops::count_noted(Kind::Draw, Why::Other);
            #[cfg(feature = "diagnostics")]
            dropped(self.pipeline);
        }
    }
}

impl State {
    /// Shaders that can write buffers were recorded: arguments may be others
    /// from here on.
    fn buffers_written(&mut self) {
        match &mut self.batch {
            Some(batch) if batches() == 2 => batch.wrote = true,
            batch => *batch = None,
        }
    }

    /// Leonida: a dispatch with the pipeline `p` was recorded: as
    /// [`State::buffers_written`] when its shader can store to buffers, the only
    /// memory indirect arguments are read from. A dispatch that writes textures
    /// alone (most of a frame's: lighting tiles, post passes) leaves every
    /// argument as it was, so the open batch of clamped records stays open past
    /// it, as it does past a draw that stores to no buffer ([`State::drew`]):
    /// fewer clamp passes, each a dispatch of ours between two full waits.
    /// `LEONIDA_BATCH_TEXTURE_DISPATCHES=0`: every dispatch ends the batch, as
    /// before (an A/B switch, `LEONIDA_AB`).
    fn dispatched(&mut self, p: &HostPipeline) {
        if !crate::toggled!("LEONIDA_BATCH_TEXTURE_DISPATCHES")
            || p.stages.iter().any(|stage| stage.translated().stores)
        {
            self.buffers_written();
        }
    }

    /// A draw with the pipeline `p` was recorded (as [`State::buffers_written`]
    /// if its shaders can store to buffers).
    fn drew(&mut self, p: Option<&HostPipeline>) {
        if p.is_none_or(|p| p.stages.iter().any(|stage| stage.translated().stores)) {
            self.batch = None;
        }
    }

    /// A buffer was copied to: what was clamped of it ahead is clamped anew.
    pub(crate) fn copied(&mut self) {
        self.window = None;
        self.batch = None;
    }

    /// The title asks for a wait for every earlier shader write (a barrier on
    /// a buffer, its barrier groups, a pipeline sync): whether one is to be
    /// recorded into `list`. Not with [`barrier_merge`], if the list has had
    /// one and nothing recorded since can write through unordered access.
    pub(crate) fn wait_for_writes(&mut self, list: &HostList) -> bool {
        const WAIT_USE: usize = usize::MAX - 0x79;
        // Another host list (a new one after a cut, or this one after its
        // reset): what ran before it is not known here.
        if list.first_use(WAIT_USE) {
            self.waited = false;
        }
        let needed = wait_needed(self.waited, barrier_merge());
        if !needed {
            WAITS[1].fetch_add(1, Ordering::Relaxed);
        } else {
            WAITS[0].fetch_add(1, Ordering::Relaxed);
            if self.waited {
                WAITS[2].fetch_add(1, Ordering::Relaxed);
            }
        }
        self.waited = true;
        needed
    }

    /// The title waits for earlier work (a barrier or a pipeline sync).
    pub(crate) fn synced(&mut self) {
        if self.batch.as_ref().is_some_and(|b| b.wrote) {
            self.batch = None;
        }
    }

    /// Where the clamped record of `kind` at `arguments` of the copy `args`
    /// is, from the open batch or a new one ([`batches`]). `None` if it
    /// cannot be clamped in one.
    fn batched(
        &mut self,
        list: &HostList,
        args: &std::sync::Arc<crate::shadow::Shadow>,
        arguments: u64,
        kind: u32,
        pack: u32,
    ) -> Option<(ID3D12Resource, u64)> {
        const BATCH_USE: usize = usize::MAX - 0x78;
        const DISPATCH: u32 = 2;
        // Another host list, or this one after its reset ([`windows`]).
        if list.first_use(BATCH_USE) {
            self.batch = None;
        }
        // A draw after dispatches waits for them.
        if kind != DISPATCH {
            self.synced();
        }
        let (id, offset) = (args.id(), args.offset(arguments));
        if let Some(b) = &mut self.batch
            && let Some(at) = b.table.place_packed(id, offset, kind, pack)
        {
            return Some((b.scratch.clone(), at));
        }
        self.batch = None;
        let opened = crate::indirect::open_batch(list, (id, &args.buffer), &argument_copies(args));
        self.clamped();
        let (mut table, scratch) = opened?;
        let at = table.place_packed(id, offset, kind, pack)?;
        self.batch = Some(Batch { table, scratch: scratch.clone(), wrote: false });
        Some((scratch, at))
    }

    /// Indirect execution of a one-argument signature of `kind` (PC
    /// numbering: 0 draw, 1 indexed draw, 2 dispatch, 10 mesh dispatch) with
    /// records `stride` bytes apart at title address `arguments`, and the
    /// record count at `count` if the title gives one: the GPU reads them
    /// from the buffer's host copy, where earlier dispatches may have
    /// written them. False if it cannot run on the host.
    pub(crate) fn execute_indirect(
        &mut self,
        list: &HostList,
        (kind, stride, max): (u32, u32, u32),
        arguments: u64,
        count: Option<u64>,
    ) -> bool {
        // Why it does not run, if it does not: counted by the caller
        // (`crate::drops`, the row of indirect executions).
        crate::drops::begin();
        // Skip before the argument clamp can mutate the indirect buffer.
        #[cfg(feature = "diagnostics")]
        if crate::pipeline::is_skipped(self.pipeline as This) {
            crate::drops::note(Why::SkipList);
            crate::pipeline::note_drop(self.pipeline as This);
            return false;
        }
        if crate::pipeline::is_builder(self.pipeline as This) && !crate::pipeline::builder_runs() {
            crate::drops::note(Why::BuilderOff);
            return false;
        }
        const DRAW: u32 = 0;
        const INDEXED: u32 = 1;
        const DISPATCH: u32 = 2;
        const MESH: u32 = 10;
        #[cfg(feature = "diagnostics")]
        let why = |what: &str| crate::pipeline::note_why(format!("indirect kind {kind}: {what}"));
        #[cfg(not(feature = "diagnostics"))]
        let why = |_: &str| {};
        let Some(p) = crate::pipeline::host_pipeline(self.pipeline as This) else {
            crate::drops::note(crate::pipeline::why_gone(self.pipeline as This));
            #[cfg(feature = "diagnostics")]
            crate::pipeline::note_drop(self.pipeline as This);
            return false;
        };
        // Indirect rectangles (the title's tiled lighting, 256
        // indirect draws a frame, makes their corners from the vertex
        // number) are completed by the pipeline's geometry stage: the
        // arguments stay the title's, three vertices a rectangle. They
        // cannot be expanded on the CPU, which does not know their count.
        // shortcut: in a pipeline without that stage, a rectangle with no
        // per-vertex inputs is drawn as its first triangle (three vertices,
        // half of it), and one with vertex buffers is not drawn.
        let made_rects = !p.rects
            && kind == DRAW
            && self.topology == RECTANGLES
            && !p.inputs.iter().any(|e| !e.per_instance);
        if made_rects {
            once!("indirect rectangles without vertex inputs drawn as their first triangle");
        }
        let matches_kind = match kind {
            DRAW | INDEXED => {
                (self.topology != RECTANGLES || made_rects || p.rects) && !p.compute && !p.mesh
            }
            MESH => !p.compute && p.mesh,
            DISPATCH => p.compute,
            _ => false,
        };
        if !matches_kind {
            why(&format!(
                "pipeline mismatch (compute {}, mesh {}, topology {})",
                p.compute, p.mesh, self.topology
            ));
            return false;
        }
        // A program run several console groups to a host group takes the console's grid from
        // root constants the records carry (`crate::indirect::bound_packed`).
        if kind == DISPATCH && pack_broken(p) {
            why("a packed program whose grid words cannot be set");
            return false;
        }
        let pack = if kind == DISPATCH { pack_of(p) } else { None };
        let pack_k = pack.map_or(0, |pack| pack.k);
        let len = u64::from(stride) * u64::from(max.max(1));
        let Some(args) = crate::shadow::find_any(arguments, len) else {
            why("arguments outside title buffers");
            return false;
        };
        let counter = match count {
            Some(at) => match crate::shadow::find_any(at, 4) {
                Some(s) => Some((s, at)),
                None => {
                    why("count outside title buffers");
                    return false;
                }
            },
            None => None,
        };
        let Some(signature) = matches!(kind, DRAW | INDEXED | DISPATCH | MESH)
            .then(|| match pack {
                Some(pack) => pack_signature(&p.root, pack),
                None => host_signature(kind, stride),
            })
            .flatten()
        else {
            why("no host signature");
            return false;
        };
        if kind == MESH && (mesh_test() == 1 || !mesh_kept(self.pipeline)) {
            crate::drops::note(Why::SkipList);
            why("mesh draws are switched off (LEONIDA_MESH_TEST=skip)");
            return false;
        }
        // Renderer port 1: arguments (or their count) in the title's memory
        // read in place ([`in_place_order`]): clamped alone, right before their
        // execution, never ahead in a batch or window.
        // Ported from Belobug/VIctory-Belobug-AMD 631400f.
        let alone = (args.in_place() || counter.as_ref().is_some_and(|(s, _)| s.in_place())) && in_place_order();
        if alone {
            self.batch = None;
            self.window = None;
        }
        // One record without a count: clamped in a batch (`batches`).
        let batch = if !alone && batches() > 0 && max == 1 && count.is_none() {
            args.refresh(list, arguments, len);
            self.batched(list, &args, arguments, kind, pack_k)
        } else {
            None
        };
        if batch.is_some() {
            self.window = None;
        }
        // A window of records clamped in one pass (`windows`): for single
        // records without a count buffer, drawn by shaders that store to no
        // buffer. Anything else ends the window: it uses the scratch buffer
        // or may write the arguments.
        const WINDOW_USE: usize = usize::MAX - 0x77;
        let offset = args.offset(arguments);
        let windowed = windows()
            && !alone
            && kind != DISPATCH
            && max == 1
            && count.is_none()
            && !p.stages.iter().any(|stage| stage.translated().stores);
        // Another host list (a new one after a cut, or this one after its
        // reset) has another scratch buffer, or this one's with old bytes.
        if !windowed || list.first_use(WINDOW_USE) {
            self.window = None;
        }
        let same = |w: &Window| w.copy == args.id() && w.kind == kind && w.stride == stride;
        let hit = self
            .window
            .as_ref()
            .filter(|w| same(w) && offset >= w.base)
            .filter(|w| (offset - w.base) % u64::from(w.step) == 0 && (offset - w.base) / u64::from(w.step) < u64::from(w.records))
            .map(|w| (w.scratch.clone(), offset - w.base));
        let (bounded, at) = match batch.or(hit) {
            Some(hit) => hit,
            None => {
                // A record a short step after the last window's last one
                // (the same step as that window's, if it had one): the
                // title walks the buffer, so the ones that step apart
                // after it are clamped with it.
                let (step, records) = match &self.window {
                    Some(w) if windowed && same(w) => {
                        let last = w.base + u64::from(w.records - 1) * u64::from(w.step);
                        let step = offset.saturating_sub(last);
                        let walks = step >= u64::from(stride)
                            && step <= 256
                            && step % 4 == 0
                            && (w.records == 1 || step == u64::from(w.step));
                        if walks {
                            let fit = (args.left(arguments) / step).min(WINDOW_MOST) as u32;
                            (step as u32, (if w.records == 1 { 16 } else { w.records * 2 }).min(fit).max(1))
                        } else {
                            (stride, max)
                        }
                    }
                    _ => (stride, max),
                };
                args.refresh(list, arguments, len.max(u64::from(step) * u64::from(records)));
                if let Some((s, at)) = &counter {
                    s.refresh(list, *at, 4);
                }
                // Bounded first: the clamp binds a compute pipeline of its
                // own and copies clamped active records into a scratch
                // buffer at offset 0.
                let count_arg = counter.as_ref().map(|(s, at)| (&s.buffer, s.offset(*at)));
                // More than the record asked for: a window clamped ahead,
                // whose unfilled slots must not spend the list's budget.
                let bounded = if windowed && records > 1 {
                    crate::indirect::bound_ahead(list, (&args.buffer, offset), (kind, step, records))
                } else {
                    crate::indirect::bound_packed(list, (&args.buffer, offset), count_arg, (kind, step, records), pack_k)
                };
                // (A clamp that gave up may have set its root signature too.)
                self.clamped();
                let Some(bounded) = bounded else {
                    self.window = None;
                    why("not bounded (no clamp, or arguments not 4-byte aligned)");
                    return false;
                };
                self.window = windowed.then(|| Window {
                    copy: args.id(),
                    kind,
                    stride,
                    step,
                    base: offset,
                    records,
                    scratch: bounded.clone(),
                });
                (bounded, 0)
            }
        };
        let ran = match kind {
            DRAW | INDEXED => self.prepare(list, kind == INDEXED),
            MESH => self.prepare_for(list, false, true),
            DISPATCH => self.bind(list, p, true),
            _ => false,
        };
        if !ran {
            // (What `prepare` or a table noted stays.)
            crate::drops::note(Why::BindFailed);
            why("prepare failed");
            return false;
        }
        #[cfg(feature = "diagnostics")]
        list.note_op(|| {
            self.op_text(format!("indirect kind {kind} max {max} at {arguments:#x}"), kind == DISPATCH)
        });
        #[cfg(feature = "diagnostics")]
        list.ran_indirect(p.pso.as_raw() as usize, &p.name, kind, max, &self.user);
        if kind == MESH {
            self.mesh_scissor(list, true);
        }
        // SAFETY: an open list with everything set; the scratch buffer and
        // host copies are kept alive by the list, and the signature is cached.
        unsafe {
            list.list.ExecuteIndirect(
                &signature,
                max,
                &bounded,
                at,
                counter.as_ref().map(|(s, _)| &s.buffer),
                counter.as_ref().map_or(0, |(s, at)| s.offset(*at)),
            )
        };
        if kind == MESH {
            self.mesh_scissor(list, false);
        }
        if let Some(pack) = pack {
            // The records set three words of its user data.
            self.cache.constant_set(true, pack.param);
            PACKED[1].fetch_add(1, Ordering::Relaxed);
        }
        if kind == DISPATCH {
            self.dispatched(p);
        } else {
            self.drew(Some(p));
        }
        // The scratch buffer stays readable as arguments: the next draw of
        // the window reads it with no barrier, and the next clamp moves it
        // back (`crate::indirect`).
        if kind == DISPATCH {
            DISPATCHED.fetch_add(1, Ordering::Relaxed);
        } else {
            DRAWN.fetch_add(1, Ordering::Relaxed);
        }
        true
    }
}

/// `DispatchX`: the counts are threads (the console's dispatch initiator bit
/// "use thread dimensions"; DexNom/VIctory bc3cd4b0). Taken for groups, a
/// rehydration stage of the structure builder ran up to 64 times its threads
/// and stored over the structures after its own (runs light-1 to light-9).
const THREAD_DIMENSIONS: u32 = 0x20;

/// `LEONIDA_THREAD_DIMENSIONS=0`: such counts are taken for groups, as before.
fn thread_dimensions() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| std::env::var_os("LEONIDA_THREAD_DIMENSIONS").is_none_or(|v| v != "0"))
}

/// The groups of `size` threads that hold `threads`.
fn dispatch_groups(threads: [u32; 3], size: [u32; 3]) -> [u32; 3] {
    std::array::from_fn(|i| threads[i].div_ceil(size[i]))
}

/// Whether `kind` is one of the three dwords of a packed program's console grid.
fn is_pack_word(kind: u32) -> bool {
    use leonida_shader::emit::{USER_PACK_GROUPS, USER_PACK_X, USER_PACK_Y};
    kind == USER_PACK_X || kind == USER_PACK_Y || kind == USER_PACK_GROUPS
}

/// A compute program translated to run `k` console groups to a host group
/// (`leonida_shader::emit::USER_PACK_X`): where its console grid goes.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct PackWords {
    pub k: u32,
    /// The root parameter of the user data.
    pub param: u32,
    /// The dwords in it of the grid's X, Y and groups in all.
    pub words: [u32; 3],
}

/// The packing of the compute pipeline `p`'s program, if it has one.
pub(crate) fn pack_of(p: &HostPipeline) -> Option<PackWords> {
    let stage = p.stages.first().filter(|_| p.compute)?;
    let t = stage.translated();
    let (param, _) = stage.user_data?;
    let pack = t.pack?;
    if !t.pack_logged.swap(true, Ordering::Relaxed) {
        tracing::info!(pipeline = %p.name, k = pack.k, param, words = ?pack.words, "a packed program is bound (LEONIDA_GROUP_PACK)");
    }
    Some(PackWords { param, ..pack })
}

/// Whether the compute pipeline `p`'s program is packed (it carries the console grid's words) but its
/// dispatches cannot tell it the grid, so that every thread of it would leave at once: it is not run.
/// Logged once for each pipeline.
fn pack_broken(p: &HostPipeline) -> bool {
    let Some(stage) = p.stages.first().filter(|_| p.compute) else {
        return false;
    };
    let t = stage.translated();
    if !has_pack_words(&t.host.user_data) || pack_of(p).is_some() {
        return false;
    }
    if !t.pack_logged.swap(true, Ordering::Relaxed) {
        tracing::warn!(pipeline = %p.name, user_data = stage.user_data.is_some(), "a packed program whose grid words cannot be set: its dispatches are dropped (LEONIDA_GROUP_PACK)");
    }
    true
}

/// Whether the user data has any of a packed program's grid words.
pub(crate) fn has_pack_words(user_data: &[leonida_shader::emit::UserData]) -> bool {
    user_data.iter().any(|u| matches!(u.root, RootType::Other(kind) if is_pack_word(kind)))
}

/// The packing of a program with these user-data words, found once (its parameter is the
/// pipeline's: [`pack_of`]).
pub(crate) fn pack_in(user_data: &[leonida_shader::emit::UserData]) -> Option<PackWords> {
    use leonida_shader::emit::{USER_PACK_GROUPS, USER_PACK_X, USER_PACK_Y};
    pack_words(user_data, 0, [USER_PACK_X, USER_PACK_Y, USER_PACK_GROUPS])
}

/// [`pack_of`] for a program's user-data words (`kinds`: the three codes, in the order of
/// [`PackWords::words`]); `k` is the metadata `dword` all three carry. Pure.
fn pack_words(user_data: &[leonida_shader::emit::UserData], param: u32, kinds: [u32; 3]) -> Option<PackWords> {
    let find = |kind: u32| user_data.iter().find(|u| u.root == RootType::Other(kind));
    let [x, y, all] = kinds.map(find);
    let (x, y, all) = (x?, y?, all?);
    (all.dword >= 2).then_some(PackWords { k: all.dword, param, words: [x.index, y.index, all.index] })
}

/// Where a compute pipeline's user data has a word the host computes: the
/// root parameter and the dword in it.
fn compute_word(p: &HostPipeline, kind: u32) -> Option<(u32, u32)> {
    let stage = p.stages.first().filter(|_| p.compute)?;
    let (param, _) = stage.user_data?;
    let word = stage.translated().host.user_data.iter().find(|u| u.root == RootType::Other(kind))?;
    Some((param, word.index))
}

#[cfg(test)]
mod thread_tests {
    use super::*;

    #[test]
    fn threads_are_counted_in_the_programs_groups() {
        assert_eq!(dispatch_groups([8847, 1, 1], [64, 1, 1]), [139, 1, 1]);
        assert_eq!(dispatch_groups([74, 1, 1], [64, 1, 1]), [2, 1, 1]);
        assert_eq!(dispatch_groups([32, 1, 1], [32, 1, 1]), [1, 1, 1]);
        assert_eq!(dispatch_groups([0, 12, 9], [64, 4, 4]), [0, 3, 3]);
    }
}

#[cfg(test)]
mod pack_tests {
    use super::*;
    use leonida_shader::emit::{USER_PACK_GROUPS, USER_PACK_X, USER_PACK_Y, UserData, pack_grid};

    fn word(kind: u32, index: u32, k: u32) -> UserData {
        UserData { index, slot: 0, root: RootType::Other(kind), dword: k }
    }

    const KINDS: [u32; 3] = [USER_PACK_X, USER_PACK_Y, USER_PACK_GROUPS];

    #[test]
    fn a_packed_programs_grid_words_are_found_in_its_user_data() {
        let words = [
            UserData { index: 0, slot: 0, root: RootType::Constants, dword: 0 },
            word(USER_PACK_X, 5, 4),
            word(USER_PACK_Y, 6, 4),
            word(USER_PACK_GROUPS, 7, 4),
        ];
        assert_eq!(pack_words(&words, 3, KINDS), Some(PackWords { k: 4, param: 3, words: [5, 6, 7] }));
        // one of them missing, or no packing in it: not packed
        assert_eq!(pack_words(&words[..3], 3, KINDS), None);
        assert_eq!(pack_words(&words[..1], 3, KINDS), None);
        assert_eq!(pack_words(&[word(USER_PACK_X, 5, 1), word(USER_PACK_Y, 6, 1), word(USER_PACK_GROUPS, 7, 1)], 3, KINDS), None);
        // a program with some of the words and not all is told apart from one with none
        assert!(has_pack_words(&words[..2]));
        assert!(!has_pack_words(&words[..1]));
        assert_eq!(pack_in(&words[..2]), None);
        assert!(KINDS.into_iter().all(is_pack_word));
        assert!(!is_pack_word(leonida_shader::emit::USER_DISPATCH_THREADS_X));
    }

    #[test]
    fn a_direct_dispatch_runs_the_groups_divided_and_names_the_console_grid() {
        // what the direct dispatch sets and runs for a packed program
        assert_eq!(pack_grid([57_600, 1, 1], 4), ([14_400, 1, 1], [57_600, 1, 57_600]));
        assert_eq!(pack_grid([240, 135, 1], 2), ([16_200, 1, 1], [240, 135, 32_400]));
        assert_eq!(pack_grid([3, 1, 1], 4), ([1, 1, 1], [3, 1, 3]));
        assert_eq!(pack_grid([0, 1, 1], 4), ([0, 1, 1], [0, 1, 0]));
    }
}

#[cfg(test)]
mod cache_tests {
    use super::*;

    /// A state whose cache holds a constant and a root view on each side.
    fn held() -> State {
        let mut s = State::default();
        s.cache.on_for_test();
        for compute in [false, true] {
            assert!(s.cache.constants(compute, 0, &[1, 2]));
            assert!(s.cache.descriptor(compute, crate::rootcache::Kind::Srv, 1, 0x100));
        }
        s.host_pipeline = 7;
        s.host_roots = [8, 9];
        s
    }

    fn graphics_kept(s: &mut State) -> bool {
        !s.cache.constants(false, 0, &[1, 2]) && !s.cache.descriptor(false, crate::rootcache::Kind::Srv, 1, 0x100)
    }

    fn compute_dropped(s: &mut State) -> bool {
        s.cache.constants(true, 0, &[1, 2]) && s.cache.descriptor(true, crate::rootcache::Kind::Srv, 1, 0x100)
    }

    #[test]
    fn the_indirect_clamp_drops_the_compute_side_and_leaves_graphics() {
        let mut s = held();
        s.clamped();
        assert!(graphics_kept(&mut s));
        assert!(compute_dropped(&mut s));
        assert_eq!((s.host_pipeline, s.host_roots), (0, [8, 0]));
    }

    #[test]
    fn a_pass_of_ours_with_its_own_root_signature_drops_that_side() {
        let mut s = held();
        s.cache.side_changed(true);
        assert!(graphics_kept(&mut s));
        assert!(compute_dropped(&mut s));
        let mut s = held();
        s.cache.side_changed(false);
        assert!(s.cache.constants(false, 0, &[1, 2]));
        assert!(!s.cache.constants(true, 0, &[1, 2]));
    }

    #[test]
    fn heaps_set_drop_both_sides() {
        let mut s = held();
        s.cache.heaps_changed();
        assert!(compute_dropped(&mut s));
        assert!(s.cache.constants(false, 0, &[1, 2]));
    }

    /// A host list that continues the title's (`replay`) has nothing set.
    #[test]
    fn a_new_host_list_after_a_cut_has_nothing_cached() {
        let Some(host) = crate::host::host() else {
            return; // no host GPU
        };
        let list = host.new_list().expect("a host list");
        let mut s = held();
        s.cache.vertex(&[(1, 2, 3)]);
        s.cache.index((1, 2, 3));
        s.replay(&list);
        assert!(compute_dropped(&mut s));
        assert!(s.cache.constants(false, 0, &[1, 2]));
        assert!(s.cache.descriptor(false, crate::rootcache::Kind::Srv, 1, 0x100));
        assert!(s.cache.vertex(&[(1, 2, 3)]));
        assert!(s.cache.index((1, 2, 3)));
        assert_eq!((s.host_pipeline, s.host_roots, s.host_heaps_set), (0, [0; 2], false));
    }

    #[test]
    fn a_state_reset_for_a_new_host_list_has_nothing_cached() {
        let mut s = held();
        s.reset(std::ptr::null());
        s.cache.on_for_test();
        assert!(s.cache.constants(false, 0, &[1, 2]));
        assert_eq!(s.host_roots, [0; 2]);
    }
}

/// The host command signature for one argument of `kind` with records
/// `stride` bytes apart (no root arguments, so no root signature), made
/// once.
fn host_signature(kind: u32, stride: u32) -> Option<ID3D12CommandSignature> {
    static MADE: std::sync::Mutex<Vec<((u32, u32), ID3D12CommandSignature)>> =
        std::sync::Mutex::new(Vec::new());
    let mut made = MADE.lock().unwrap_or_else(|e| e.into_inner());
    if let Some((_, s)) = made.iter().find(|(k, _)| *k == (kind, stride)) {
        return Some(s.clone());
    }
    let argument = D3D12_INDIRECT_ARGUMENT_DESC {
        Type: D3D12_INDIRECT_ARGUMENT_TYPE(kind as i32),
        ..Default::default()
    };
    let desc = D3D12_COMMAND_SIGNATURE_DESC {
        ByteStride: stride,
        NumArgumentDescs: 1,
        pArgumentDescs: &argument,
        NodeMask: 0,
    };
    let mut signature: Option<ID3D12CommandSignature> = None;
    // SAFETY: a description of one argument; no root arguments.
    let result = unsafe {
        crate::host::host()?.device.CreateCommandSignature(
            &desc,
            None::<&ID3D12RootSignature>,
            &mut signature,
        )
    };
    if let Err(e) = result {
        once!(error = %e, kind, stride, "host command signature refused");
        return None;
    }
    let signature = signature?;
    made.push(((kind, stride), signature.clone()));
    Some(signature)
}

/// The host command signature of an indirect dispatch of a program run `pack.k` console groups
/// to a host group: the three dwords of the console's grid set as root constants (`pack.words`,
/// consecutive in the program's user data: the translator makes them so), then the dispatch,
/// [`crate::indirect::PACKED`] bytes a record. One for each root signature and place of the
/// words, made once; `None` where they are not consecutive or the host refuses it.
fn pack_signature(root: &ID3D12RootSignature, pack: PackWords) -> Option<ID3D12CommandSignature> {
    static MADE: std::sync::Mutex<Vec<((usize, u32, u32), ID3D12RootSignature, ID3D12CommandSignature)>> =
        std::sync::Mutex::new(Vec::new());
    let [x, y, all] = pack.words;
    if y != x + 1 || all != y + 1 {
        once!(?pack, "the words of a packed program's console grid are not consecutive: its indirect dispatches are dropped");
        return None;
    }
    use windows::core::Interface;
    let key = (root.as_raw() as usize, pack.param, x);
    let mut made = MADE.lock().unwrap_or_else(|e| e.into_inner());
    if let Some((_, _, s)) = made.iter().find(|(k, ..)| *k == key) {
        return Some(s.clone());
    }
    let arguments = [
        D3D12_INDIRECT_ARGUMENT_DESC {
            Type: D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT,
            Anonymous: D3D12_INDIRECT_ARGUMENT_DESC_0 {
                Constant: D3D12_INDIRECT_ARGUMENT_DESC_0_1 {
                    RootParameterIndex: pack.param,
                    DestOffsetIn32BitValues: x,
                    Num32BitValuesToSet: 3,
                },
            },
        },
        D3D12_INDIRECT_ARGUMENT_DESC { Type: D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH, ..Default::default() },
    ];
    let desc = D3D12_COMMAND_SIGNATURE_DESC {
        ByteStride: crate::indirect::PACKED,
        NumArgumentDescs: arguments.len() as u32,
        pArgumentDescs: arguments.as_ptr(),
        NodeMask: 0,
    };
    let mut signature: Option<ID3D12CommandSignature> = None;
    // SAFETY: a description of two arguments that outlives the call; the root signature is the
    // pipeline's, which the root constants named are a parameter of.
    let result = unsafe { crate::host::host()?.device.CreateCommandSignature(&desc, root, &mut signature) };
    if let Err(e) = result {
        once!(error = %e, ?pack, "host command signature of a packed dispatch refused");
        return None;
    }
    let signature = signature?;
    // (the root signature is kept: the key is its address)
    made.push((key, root.clone(), signature.clone()));
    Some(signature)
}

/// Gets what a shader reads or writes through a view ready: buffer data
/// copied, textures moved to the state they need.
pub(crate) fn view_kind(view: &View) -> Option<ResourceType> {
    match view {
        View::Shader(..) | View::Placed(false, ..) => Some(ResourceType::Srv),
        View::Unordered(..) | View::Placed(true, ..) => Some(ResourceType::Uav),
        _ => None,
    }
}

/// LOCAL (fxceless a35c557393): the format of a host view of `texture`
/// whose description names `described`: that one, else the texture's own
/// (what the host gives a view without a format); 0 if neither is known.
fn bound_format(texture: Option<&ID3D12Resource>, described: Option<DXGI_FORMAT>) -> u32 {
    match (described, texture) {
        (Some(format), _) if format.0 != 0 => format.0 as u32,
        // SAFETY: a live texture.
        (_, Some(texture)) => unsafe { texture.GetDesc() }.Format.0 as u32,
        _ => 0,
    }
}

/// The host state a depth view needs of its texture: the read state if the
/// view writes no plane the format has (the title's read-only flags), else
/// the write state.
/// shortcut: whole textures have one state, so a view that writes one of
/// two planes counts as writing.
fn depth_state(texture: &ID3D12Resource, desc: Option<&D3D12_DEPTH_STENCIL_VIEW_DESC>) -> u32 {
    // D16_UNORM and D32_FLOAT: depth formats without a stencil plane.
    const ONE_PLANE: [i32; 2] = [55, 40];
    let Some(desc) = desc else {
        return DEPTH_WRITE;
    };
    let format = if desc.Format.0 == 0 {
        // SAFETY: a live texture; a view without a format has its own.
        unsafe { texture.GetDesc() }.Format.0
    } else {
        desc.Format.0
    };
    let mut needed = D3D12_DSV_FLAG_READ_ONLY_DEPTH.0;
    if !ONE_PLANE.contains(&format) {
        needed |= D3D12_DSV_FLAG_READ_ONLY_STENCIL.0;
    }
    if desc.Flags.0 & needed == needed {
        DEPTH_READ
    } else {
        DEPTH_WRITE
    }
}

/// A few things, up to `N` of them without memory of the heap: what a draw's
/// tables are gone through with, some six lists a stage.
pub(crate) struct Few<T, const N: usize> {
    few: [T; N],
    len: usize,
    /// All of them, if they are more.
    many: Vec<T>,
}

impl<T: Copy + Default, const N: usize> Default for Few<T, N> {
    fn default() -> Self {
        Few { few: [T::default(); N], len: 0, many: Vec::new() }
    }
}

impl<T: Copy, const N: usize> Few<T, N> {
    fn push(&mut self, what: T) {
        if self.many.is_empty() && self.len < N {
            self.few[self.len] = what;
            self.len += 1;
        } else {
            if self.many.is_empty() {
                self.many.extend_from_slice(&self.few);
            }
            self.many.push(what);
        }
    }
}

impl<T, const N: usize> std::ops::Deref for Few<T, N> {
    type Target = [T];
    fn deref(&self) -> &[T] {
        if self.many.is_empty() { &self.few[..self.len] } else { &self.many }
    }
}

impl<T, const N: usize> std::ops::DerefMut for Few<T, N> {
    fn deref_mut(&mut self) -> &mut [T] {
        if self.many.is_empty() { &mut self.few[..self.len] } else { &mut self.many }
    }
}

/// A table made afresh in dynamic slots, in a list ([`State::tables`]).
struct FreshTable {
    first: u32,
    /// The list's epoch ([`HostList::epoch`]) at which all its entries that
    /// these readers (a stage's list of what it reads, whether it stores, the
    /// tested depth target) read were brought up to date, and whether that read the
    /// tested depth target.
    epoch: u64,
    ready: Vec<((usize, usize, bool, usize), bool)>,
    /// Whether an entry a reader had brought up to date showed an aliased
    /// texture ([`list_alias_exact`]): such a table is never left as it is.
    aliased: bool,
}

/// Whether a table a list reads again, with nothing changed in the list
/// since, is left as it is (`LEONIDA_READY_ONCE=0`: every entry is gone
/// through again at every draw).
fn ready_once() -> bool {
    crate::toggled!("LEONIDA_READY_ONCE")
}

/// A hash of records' words: 64 bits, two multiplications a record. (The
/// standard library's took 3 % of the recording threads' time.)
#[derive(Default)]
struct RecordHash(u64);

impl RecordHash {
    fn add(&mut self, [a, b, c]: [u32; 3]) {
        let fold = |x: u64, y: u64| {
            let p = u128::from(x) * u128::from(y);
            p as u64 ^ (p >> 64) as u64
        };
        let words = u64::from(a) << 32 | u64::from(b);
        let first = fold(self.0 ^ words ^ 0x2d35_8dcc_aa6c_78a5, 0x8bb8_4b93_962e_acc9 ^ u64::from(c));
        self.0 = fold(first ^ 0x4b33_a62e_d433_d4a3, 0x4d5a_2da5_1de1_aa47 ^ self.0.rotate_left(23)) ^ first;
    }
}

/// Brings what `view` shows up to date in `list` for a read (or a write) as
/// `kind`. True if it shows an aliased texture
/// (`crate::resource::alias_of`): its copy follows what the list does to
/// its source, which no epoch of the list says ([`list_alias_exact`]).
fn readable(list: &HostList, kind: ResourceType, view: &View) -> bool {
    const UNORDERED_ACCESS: u32 = 0x8;
    let placed = crate::descriptor::host::placed_texture(view);
    let view = placed.as_ref().unwrap_or(view);
    let mut alias = false;
    // Buffer views read title memory: their range is copied at submission;
    // what shaders write through unordered access goes back to it after.
    if let Some((shadow, address, len)) = crate::descriptor::host::buffer_copy(view) {
        shadow.refresh(list, address, len);
        if kind == ResourceType::Uav {
            // A shader that cannot store to buffers leaves the range as
            // the title has it: nothing to find afterwards.
            if STORES.get() || !write_skip() {
                #[cfg(feature = "diagnostics")]
                crate::host::wide_write(address, len, 0);
                shadow.gpu_writes(list, address, len);
            } else {
                #[cfg(feature = "diagnostics")]
                {
                    crate::copystat::add(crate::copystat::C::GpuWritesLeftOut, 1);
                    crate::copystat::add(crate::copystat::C::GpuWritesLeftOutBytes, len);
                }
            }
        }
    }
    match (view, kind) {
        (View::Shader(resource, _), ResourceType::Srv) => {
            // A view the title placed, in another format, in the memory of
            // a texture the host writes: that texture's texels, copied on
            // the host (title memory does not have them).
            // (Held while the copy is recorded: the title may release the
            // source meanwhile. The list's states hold its host copy after.)
            let source = crate::resource::alias_of(*resource as This);
            alias = source.is_some();
            let aliased =
                source.as_ref().is_some_and(|source| crate::texture::alias(list, *resource as This, source.get()));
            drop(source);
            if !aliased
                && let Some(t) = crate::resource::host_copy(*resource as This)
                && t.flags() & 0x7 == 0
            {
                // (A texture the title writes: no target, depth or unordered
                // access, `crate::texture::title_written`.)
                // Of a texture the title placed, the levels the view shows.
                let first = match (&placed, view) {
                    (Some(_), View::Shader(_, Some(desc))) => crate::resource::first_mip(desc),
                    _ => 0,
                };
                crate::texture::upload_from(list, *resource as This, first);
            }
            // A depth texture shaders also write: what they wrote, first.
            list.twin_use(*resource as This, false, false);
            // The depth target the draw only tests is read in both ways at
            // once: both read states, which its preparation asks for too.
            let (tested, _) = TESTED_DEPTH.get();
            let state = if tested != 0 && tested == *resource {
                TESTED_DEPTH.set((tested, true));
                DEPTH_READ | SHADER_RESOURCE
            } else {
                SHADER_RESOURCE
            };
            crate::list::ensure_state(list, *resource as This, state);
            #[cfg(feature = "diagnostics")]
            if tracing::enabled!(tracing::Level::DEBUG)
                && let Some(t) = crate::resource::host_resource(*resource as This)
            {
                // SAFETY: a live resource.
                let d = unsafe { t.GetDesc() };
                if d.Flags.0 & 7 == 0 {
                    tracing::debug!(
                        resource = format_args!("{resource:#x}"),
                        ?d,
                        "a shader reads a texture the title fills"
                    );
                }
            }
        }
        (View::Unordered(resource, _, _), ResourceType::Uav) => {
            #[cfg(feature = "diagnostics")]
            crate::capture::note_target(*resource as This);
            // Some compute-written textures are also read by the title CPU.
            list.texture_written(*resource as This);
            // Unordered access to a depth texture is its colour twin's.
            if !list.twin_use(*resource as This, true, true) {
                crate::list::ensure_state(list, *resource as This, UNORDERED_ACCESS);
            }
        }
        _ => {}
    }
    alias
}

/// Whether a view of an aliased texture is made readable again each time a
/// list reads it, where the list would take it as ready: a view of a new
/// table the list made before ([`crate::descriptor::host::Listed::ready`]),
/// or a table the same reader reads again ([`FreshTable::ready`]). Both go
/// by the list's epoch, which changes when a texture changes state; but the
/// copy into an aliased texture ([`crate::texture::alias`]) leaves its
/// source in the state it was written in, so a draw that writes the source
/// again changes no state, and the next read through the view was taken as
/// ready and read the copy as it was (once a frame in the title's lighting
/// filter). On by default; `LEONIDA_LIST_ALIAS_EXACT=0`: as before.
fn list_alias_exact() -> bool {
    crate::toggled!("LEONIDA_LIST_ALIAS_EXACT")
}

/// Since the last frame line ([`list_alias_exact`]): views of aliased
/// textures taken as ready and made readable again, and those of them whose
/// copy was made again (a stale read without the switch); and the same for
/// tables read again that hold such a view.
static LIST_ALIAS: [AtomicU64; 4] = [const { AtomicU64::new(0) }; 4];

/// For the frame lines: what [`list_alias_exact`] did, if anything.
pub(crate) fn list_alias_line(frames: f64) {
    let [views, redone, tables, tables_redone] = [0, 1, 2, 3].map(|i| LIST_ALIAS[i].swap(0, Ordering::Relaxed));
    if views + tables == 0 {
        return;
    }
    let f = frames.max(1.0);
    tracing::info!(
        "aliased textures a list took as ready (LEONIDA_LIST_ALIAS_EXACT=0: left as they were), per frame: {:.1} views of new \
         tables made readable again, {:.1} of them with their copy made again (a stale read before); {:.1} tables read again, \
         {:.1} of them with a copy made again",
        views as f64 / f,
        redone as f64 / f,
        tables as f64 / f,
        tables_redone as f64 / f
    );
}

/// The pixel shaders whose draws get their slots listed
/// ([`State::slots_text`]): `LEONIDA_LIST_SLOTS`, hash prefixes separated
/// by commas. None by default.
#[cfg(feature = "diagnostics")]
fn list_slots() -> &'static [String] {
    static LIST: std::sync::OnceLock<Vec<String>> = std::sync::OnceLock::new();
    LIST.get_or_init(|| {
        std::env::var("LEONIDA_LIST_SLOTS")
            .unwrap_or_default()
            .split(',')
            .map(|s| s.trim().to_ascii_lowercase())
            .filter(|s| !s.is_empty())
            .collect()
    })
}

/// A fingerprint of the 4 KiB of title memory at `at` (FNV-1a, 64 bits; of
/// 4 KiB of zeros it is always the same value, which `slots.py` knows), or
/// "unreadable".
#[cfg(feature = "diagnostics")]
fn print_4k(at: u64) -> String {
    let mut bytes = [0u8; 4096];
    if at == 0 || !crate::host::read_into(at as *const u8, &mut bytes) {
        return "unreadable".into();
    }
    let mut hash = 0xcbf2_9ce4_8422_2325_u64;
    for b in bytes {
        hash = (hash ^ u64::from(b)).wrapping_mul(0x0000_0100_0000_01b3);
    }
    format!("{hash:016x}")
}

/// The `bytes` of title memory at `at`: how many are mapped, and the
/// fingerprints of the first and the last 4 KiB (a texture's smallest
/// levels lie first, its largest last).
#[cfg(feature = "diagnostics")]
fn memory_state(at: u64, bytes: u64) -> String {
    let mapped = crate::texture::mapped_bytes(at as *const u8, bytes as usize);
    let last = at + bytes.saturating_sub(4096);
    format!("memory={at:#x} mapped={mapped}/{bytes} first4k={} last4k={}", print_4k(at), print_4k(last))
}

/// Title texture `this` for the frame listing: its size, whether it has a
/// host copy (not made here), its staging buffer's state, whether the host
/// copied into it, and its memory ([`memory_state`]).
#[cfg(feature = "diagnostics")]
fn texture_state(this: This) -> String {
    let Some((at, desc)) = crate::resource::texture_memory(this) else {
        return "not a texture".into();
    };
    // SAFETY: 56 bytes in the description's layout.
    let words = unsafe { std::ptr::from_ref(&desc).cast::<[u32; 14]>().read_unaligned() };
    let bytes = crate::resource::placed_bytes(&words).unwrap_or(0);
    let staged = match crate::texture::staged_state(this) {
        Some((uploaded, dirty)) => format!("staged=yes uploaded={uploaded} dirty={dirty}"),
        None => "staged=no".into(),
    };
    format!(
        "{}x{} f{} levels={} host_copy={} {staged} host_written={} title_fills={} {}",
        u64::from(words[4]) | u64::from(words[5]) << 32,
        words[6],
        words[8],
        words[7] >> 16,
        crate::resource::host_made(this).is_some(),
        crate::texture::host_wrote(this as usize),
        crate::resource::title_fills(this),
        memory_state(at as u64, bytes)
    )
}

/// A view of the title's heap for the frame listing: a texture with its
/// state ([`texture_state`]; a placed one that has no resource yet with
/// its memory alone), anything else in a few words.
#[cfg(feature = "diagnostics")]
fn view_state(view: Option<&View>) -> String {
    match view {
        None => "no view".into(),
        Some(View::Placed(false, r, d)) if r[0] != 1 => {
            let address = u64::from(d[10]) | u64::from(d[11]) << 32;
            let state = match crate::resource::texture_at_made(address, r) {
                Some(this) => texture_state(this),
                None => format!(
                    "{}x{} f{} levels={} resource=none {}",
                    u64::from(r[4]) | u64::from(r[5]) << 32,
                    r[6],
                    r[8],
                    r[7] >> 16,
                    memory_state(address, crate::resource::placed_bytes(r).unwrap_or(0))
                ),
            };
            // (Last, so that readers of the older lines still find
            // theirs: the level the view starts at, and how many it shows.)
            format!("placed texture view_format={} {state} view_levels={}+{}", d[0], d[4], d[5] as i32)
        }
        Some(View::Shader(r, d)) if crate::resource::texture_memory(*r as This).is_some() => {
            format!(
                "texture object {r:#x} view_format={} {} view_levels={}",
                d.as_ref().map_or(0, |d| d[0]),
                texture_state(*r as This),
                d.as_ref().map_or("all".to_owned(), |d| format!("{}+{}", d[4], d[5] as i32))
            )
        }
        Some(v) => format!("{v:?}").chars().take(80).collect(),
    }
}

/// A view in a few words (diagnostics builds).
#[cfg(feature = "diagnostics")]
fn describe(view: Option<&View>) -> String {
    let texture = |r: This| {
        crate::resource::host_made(r).map(|t| {
            // SAFETY: a live resource.
            let d = unsafe { t.GetDesc() };
            format!("{}x{} f{} flags {:#x} of {r:?}", d.Width, d.Height, d.Format.0, d.Flags.0)
        })
    };
    match view {
        None => "no view".into(),
        Some(v @ View::Shader(r, _)) => texture(*r as This)
            .unwrap_or_else(|| format!("buffer {:?}", crate::descriptor::host::buffer_range(v))),
        Some(v @ View::Unordered(r, ..)) => match texture(*r as This) {
            Some(t) => format!("uav texture {t}"),
            None => format!(
                "uav resource {r:#x} buffer {:?}",
                crate::descriptor::host::buffer_range(v)
            ),
        },
        Some(View::Placed(u, r, d)) => format!(
            "placed uav={u} dim {} {}x{} f{} layout {:#x} at {:#x}",
            r[0],
            u64::from(r[4]) | u64::from(r[5]) << 32,
            r[6],
            r[8],
            r[11],
            u64::from(d[10]) | u64::from(d[11]) << 32
        ),
        // The first four values, as a colour rectangle's constants are.
        Some(View::Constant(address, size))
            if *size >= 16 && crate::texture::committed(*address as *const u8, 16) =>
        {
            // SAFETY: 16 committed bytes of title memory.
            let v = unsafe { (*address as *const [f32; 4]).read_unaligned() };
            format!("Constant({address:#x}, {size}) {v:?}")
        }
        Some(v) => format!("{v:?}").chars().take(60).collect(),
    }
}

/// The console's own primitive topologies.
const TRIANGLE_FAN: u32 = 6;
const RECTANGLES: u32 = 17;
const LINE_LOOP: u32 = 18;
const QUADS: u32 = 19;
const QUAD_STRIP: u32 = 20;

/// Fixed-function state, straight onto the host list.
pub(crate) fn viewports(list: &HostList, viewports: &[D3D12_VIEWPORT]) {
    // SAFETY: an open list.
    unsafe { list.list.RSSetViewports(viewports) };
}

pub(crate) fn scissors(list: &HostList, rects: &[RECT]) {
    // Timing experiment (`LEONIDA_AB=<s>,LEONIDA_EXP_NO_PIXELS`): every scissor is empty while it is on,
    // so the title's draws run their vertex and mesh programs and shade no pixel: a phase's frame time
    // against the next one's is what the pixels cost the card. For timing only.
    #[cfg(feature = "diagnostics")]
    if crate::ab::pick("LEONIDA_EXP_NO_PIXELS").unwrap_or(false) {
        let empty = vec![RECT::default(); rects.len().max(1)];
        // SAFETY: an open list.
        unsafe { list.list.RSSetScissorRects(&empty) };
        return;
    }
    // SAFETY: an open list.
    unsafe { list.list.RSSetScissorRects(rects) };
}

pub(crate) fn blend_factor(list: &HostList, factor: Option<&[f32; 4]>) {
    // SAFETY: an open list.
    unsafe { list.list.OMSetBlendFactor(factor) };
}

pub(crate) fn stencil_ref(list: &HostList, reference: u32) {
    #[cfg(feature = "diagnostics")]
    crate::capture::op(std::ptr::from_ref(list) as usize, || format!("STENCIL REF {reference:#x}"));
    // SAFETY: an open list.
    unsafe { list.list.OMSetStencilRef(reference) };
}

pub(crate) fn depth_bounds(list: &HostList, min: f32, max: f32) {
    #[cfg(feature = "diagnostics")]
    crate::capture::op(std::ptr::from_ref(list) as usize, || {
        format!("DEPTH BOUNDS {min}..{max}")
    });
    if crate::host::host().is_some_and(|h| h.depth_bounds())
        && let Ok(l1) = list.list.cast::<ID3D12GraphicsCommandList1>()
    {
        // SAFETY: an open list on a device that supports depth bounds.
        unsafe { l1.OMSetDepthBounds(min, max) };
    }
}

/// Per-frame texture staging byte budget scaled from the adapter's local VRAM
/// budget (`None` keeps the 192 MiB 12 GiB baseline).
fn staging_budget_per_frame(vram_budget: Option<u64>) -> u64 {
    const GIB: u64 = 1 << 30;
    const MIB: u64 = 1 << 20;
    match vram_budget {
        Some(budget) if budget <= 6 * GIB => 96 * MIB,
        Some(budget) if budget >= 20 * GIB => 384 * MIB,
        Some(budget) if budget > 12 * GIB => 256 * MIB,
        _ => 192 * MIB,
    }
}

#[cfg(test)]
mod tests {
    use super::{first_of_frame, is_depth_srv, staging_budget_per_frame};

    /// A frame is new once, with the look before the swap and without it; the look leaves
    /// the word as it is when the frame is the one it holds.
    #[test]
    fn a_frame_is_first_once_whether_the_word_is_read_first_or_not() {
        use std::sync::atomic::{AtomicU64, Ordering};
        for peek in [false, true] {
            let seen = AtomicU64::new(u64::MAX);
            let asks: Vec<bool> = [7, 7, 7, 8, 8, 7].iter().map(|&frame| first_of_frame(&seen, frame, peek)).collect();
            assert_eq!(asks, [true, false, false, true, false, true], "peek {peek}");
            assert_eq!(seen.load(Ordering::Relaxed), 7);
        }
    }

    #[test]
    fn depth_srv_checks_the_plane_not_the_number_of_levels() {
        use windows::Win32::Graphics::Direct3D12::{D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_SRV_DIMENSION_TEXTURE2DARRAY};
        use windows::Win32::Graphics::Dxgi::Common::{DXGI_FORMAT_R24_UNORM_X8_TYPELESS, DXGI_FORMAT_X24_TYPELESS_G8_UINT};

        let mut desc = [0; 10];
        desc[0] = DXGI_FORMAT_R24_UNORM_X8_TYPELESS.0 as u32;
        desc[1] = D3D12_SRV_DIMENSION_TEXTURE2D.0 as u32;
        desc[5] = 1;
        assert!(is_depth_srv(&desc));
        desc[6] = 1;
        assert!(!is_depth_srv(&desc));
        desc[6] = 0;
        desc[0] = DXGI_FORMAT_X24_TYPELESS_G8_UINT.0 as u32;
        assert!(!is_depth_srv(&desc));
        desc[0] = DXGI_FORMAT_R24_UNORM_X8_TYPELESS.0 as u32;
        desc[1] = D3D12_SRV_DIMENSION_TEXTURE2DARRAY.0 as u32;
        assert!(!is_depth_srv(&desc));
    }

    #[test]
    fn staging_budget_scales_with_vram_budget() {
        const GIB: u64 = 1 << 30;
        const MIB: u64 = 1 << 20;
        assert_eq!(staging_budget_per_frame(None), 192 * MIB);
        assert_eq!(staging_budget_per_frame(Some(4 * GIB)), 96 * MIB);
        assert_eq!(staging_budget_per_frame(Some(6 * GIB)), 96 * MIB);
        assert_eq!(staging_budget_per_frame(Some(8 * GIB)), 192 * MIB);
        assert_eq!(staging_budget_per_frame(Some(12 * GIB)), 192 * MIB);
        assert_eq!(staging_budget_per_frame(Some(13 * GIB)), 256 * MIB);
        assert_eq!(staging_budget_per_frame(Some(14 * GIB)), 256 * MIB);
        assert_eq!(staging_budget_per_frame(Some(16 * GIB)), 256 * MIB);
        assert_eq!(staging_budget_per_frame(Some(20 * GIB)), 384 * MIB);
        assert_eq!(staging_budget_per_frame(Some(24 * GIB)), 384 * MIB);
        assert_eq!(staging_budget_per_frame(Some(32 * GIB)), 384 * MIB);
    }
}

#[cfg(test)]
mod passes_ahead_tests {
    use super::{AHEAD_FRAMES, ahead_counts, ahead_takes};

    /// The lists ahead of frames record for the first view heap a draw has
    /// set, then for a bigger one; a list with another keeps its own passes.
    #[test]
    fn the_biggest_heap_asked_for_is_theirs() {
        let mut theirs = (0, 0);
        assert_eq!(ahead_takes(&mut theirs, 0x1000, 64), Some(true));
        assert_eq!(ahead_takes(&mut theirs, 0x1000, 64), Some(false));
        assert_eq!(ahead_takes(&mut theirs, 0x2000, 64), None, "one as big keeps its own passes");
        assert_eq!(ahead_takes(&mut theirs, 0x3000, 16), None);
        assert_eq!(theirs, (0x1000, 64));
        assert_eq!(ahead_takes(&mut theirs, 0x4000, 1 << 20), Some(true));
        assert_eq!(ahead_takes(&mut theirs, 0x1000, 64), None, "the one it was is another's now");
        // Let go of (`view_heap_gone`): the next that asks.
        theirs = (0, 0);
        assert_eq!(ahead_takes(&mut theirs, 0x3000, 16), Some(true));
        assert_eq!(theirs, (0x3000, 16));
    }

    /// A list leaves its pass to the lists ahead only while one of them had
    /// it a moment ago; else it makes it for itself, as before.
    #[test]
    fn a_pass_ahead_counts_for_a_few_frames() {
        assert!(!ahead_counts(0, 0), "none had it");
        assert!(!ahead_counts(0, 100));
        // Made in frame 7 (kept as 8).
        assert!(ahead_counts(8, 7));
        assert!(ahead_counts(8, 7 + AHEAD_FRAMES));
        assert!(!ahead_counts(8, 8 + AHEAD_FRAMES), "frames went by without one: not presented, or the line stood");
        // The title's count read a moment before the list ahead read it.
        assert!(ahead_counts(8, 6));
    }
}

#[cfg(test)]
mod record_hash_tests {
    use super::{Few, RecordHash};

    #[test]
    fn a_few_things_are_kept_in_order_however_many() {
        for n in 0..10u32 {
            let mut few: Few<u32, 4> = Few::default();
            (0..n).for_each(|i| few.push(n - i));
            assert_eq!(few.to_vec(), (0..n).map(|i| n - i).collect::<Vec<_>>());
            few.sort_unstable();
            assert_eq!(few.to_vec(), (1..=n).collect::<Vec<_>>());
        }
    }

    #[test]
    fn records_that_differ_hash_apart() {
        let of = |records: &[[u32; 3]]| {
            let mut h = RecordHash::default();
            records.iter().for_each(|r| h.add(*r));
            h.0
        };
        let mut seen = std::collections::HashSet::new();
        // One record, one word or two of it different; runs of zeros; order.
        for a in 0..64u32 {
            for b in 0..64u32 {
                for c in [0, 1, 0x8000_0000, u32::MAX] {
                    assert!(seen.insert(of(&[[a, b, c]])));
                    assert!(seen.insert(of(&[[1, 2, 3], [a, b, c]])));
                    assert!(seen.insert(of(&[[a, b, c], [1, 2, 3], [0, 0, 0]])));
                }
            }
        }
        assert!(seen.insert(of(&[])));
        // (One record of zeros is among the above.)
        let zeros: Vec<u64> = (2..100).map(|n| of(&vec![[0; 3]; n])).collect();
        assert!(zeros.iter().all(|h| seen.insert(*h)));
    }
}

#[cfg(test)]
mod wait_tests {
    use super::*;

    /// A title's wait for every shader write is left out only with the
    /// switch, and only when its list has had one with nothing since that
    /// can write through unordered access; a list starts with nothing waited
    /// for, and a clamp of ours, another host list and a reset are waited
    /// for again.
    #[test]
    fn a_wait_is_left_out_only_after_one_with_nothing_written_since() {
        assert!(wait_needed(false, false));
        assert!(wait_needed(false, true));
        assert!(wait_needed(true, false));
        assert!(!wait_needed(true, true));
        let mut state = State::default();
        assert!(!state.waited);
        state.waited = true;
        state.clamped();
        assert!(!state.waited);
        state.waited = true;
        state.reset(std::ptr::null());
        assert!(!state.waited);
    }
}
