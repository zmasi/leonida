//! What a host list was last given for its root parameters and its vertex and
//! index buffers, so that a draw that sets the same value again records
//! nothing (`LEONIDA_STATE_CACHE=0`: every call is recorded, as before).
//!
//! A title draws 6,000 to 10,000 times a frame and each draw set every
//! stage's root constants, root views, one-view tables and vertex and index
//! buffers again, most of them to the value they had; the driver copies each
//! call into the list (a CPU profile: a quarter of the recording threads'
//! running time, in `memmove` inside the driver).
//!
//! The cache is part of the title list's [`crate::draw::State`], so it has
//! the lifetime of one host list's recording: a new `State` (the title's
//! reset) and `State::replay` (a new host list after a cut or a pass of ours
//! that set what it wanted) start it empty. What the cache must be told:
//!
//! * a root signature set on a side (graphics or compute) drops the root
//!   arguments of that side ([`Cache::side_changed`]);
//! * descriptor heaps set drop the tables, and as `State::bind` sets the
//!   root signatures again then, both sides ([`Cache::heaps_changed`]);
//! * our own passes that set a compute root signature on the list
//!   (`State::clamped`) drop the compute side;
//! * a root constant set by itself ([`Cache::constant_set`]) and a vertex
//!   buffer set by itself ([`Cache::vertex_set`]) drop what they overwrote.
//!
//! The switch can change while a list records (`LEONIDA_AB`): the cache is
//! cleared whenever it is seen to differ from the last call's.

use std::sync::atomic::{AtomicU64, Ordering::Relaxed};

/// The root parameters a signature can have (it has 64 dwords, a parameter
/// costs at least one); one a cache does not hold is always recorded.
const PARAMS: usize = 64;

/// What the calls are counted by.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Kind {
    /// `Set*Root32BitConstants`.
    Constants,
    /// `Set*RootConstantBufferView`.
    Cbv,
    /// `Set*RootShaderResourceView`.
    Srv,
    /// `Set*RootUnorderedAccessView`.
    Uav,
    /// `Set*RootDescriptorTable`.
    Table,
    /// `IASetVertexBuffers`.
    Vertex,
    /// `IASetIndexBuffer`.
    Index,
}

/// The kinds, as counted.
pub(crate) const KINDS: [&str; 7] = [
    "root constants",
    "root CBVs",
    "root SRVs",
    "root UAVs",
    "tables",
    "vertex buffers",
    "index buffers",
];

/// Calls recorded and calls left out, by [`Kind`], of the lists that have
/// been let go (a cache gives its counts here when it is dropped).
static ISSUED: [AtomicU64; 7] = [const { AtomicU64::new(0) }; 7];
static SKIPPED: [AtomicU64; 7] = [const { AtomicU64::new(0) }; 7];

/// The counts since the last call: recorded, left out.
#[cfg_attr(not(feature = "diagnostics"), allow(dead_code))]
pub(crate) fn take() -> ([u64; 7], [u64; 7]) {
    (
        std::array::from_fn(|i| ISSUED[i].swap(0, Relaxed)),
        std::array::from_fn(|i| SKIPPED[i].swap(0, Relaxed)),
    )
}

/// Whether the cache is on (`LEONIDA_STATE_CACHE=0` turns it off).
pub(crate) fn on() -> bool {
    crate::toggled!("LEONIDA_STATE_CACHE")
}

/// What a root parameter that is not constants was last given: the kind of
/// call, and the GPU address or descriptor handle.
#[derive(Clone, Copy, Default, PartialEq, Eq)]
struct Given {
    /// 0: nothing known.
    kind: u8,
    value: u64,
}

/// One side of the list: graphics or compute.
struct Side {
    given: [Given; PARAMS],
    /// Root constants by parameter, the dwords last set; `known` has a bit
    /// for each parameter whose entry is current.
    constants: Vec<Vec<u32>>,
    known: u64,
}

impl Default for Side {
    fn default() -> Self {
        Side { given: [Given::default(); PARAMS], constants: Vec::new(), known: 0 }
    }
}

impl Side {
    fn clear(&mut self) {
        self.given = [Given::default(); PARAMS];
        self.known = 0;
    }
}

/// A vertex buffer view: address, size, stride.
pub(crate) type VertexView = (u64, u32, u32);

/// An index buffer view: address, size, format.
pub(crate) type IndexView = (u64, u32, i32);

#[derive(Default)]
pub(crate) struct Cache {
    /// The switch as the last call saw it.
    seen: bool,
    /// Graphics, compute.
    sides: [Side; 2],
    vertex: [VertexView; 32],
    /// The slots of `vertex` that are current.
    vertex_known: u32,
    index: Option<IndexView>,
    issued: [u64; 7],
    skipped: [u64; 7],
}

impl Drop for Cache {
    fn drop(&mut self) {
        for i in 0..KINDS.len() {
            if self.issued[i] != 0 {
                ISSUED[i].fetch_add(self.issued[i], Relaxed);
            }
            if self.skipped[i] != 0 {
                SKIPPED[i].fetch_add(self.skipped[i], Relaxed);
            }
        }
    }
}

impl Cache {
    /// Sees the switch, as it is now: a change clears the cache (while it
    /// was off nothing was kept, so what it holds is not what the list has).
    /// Whether the cache is on.
    pub(crate) fn sync(&mut self) -> bool {
        let now = on();
        if now != self.seen {
            self.clear();
            self.seen = now;
        }
        now
    }

    /// For tests of what holds a cache: on, whatever the environment says.
    #[cfg(test)]
    pub(crate) fn on_for_test(&mut self) {
        self.seen = true;
    }

    /// The host list has nothing set (a new list, or one a pass of ours
    /// used with no care for what was set).
    pub(crate) fn clear(&mut self) {
        for side in &mut self.sides {
            side.clear();
        }
        self.vertex_known = 0;
        self.index = None;
    }

    /// A root signature was set on a side: its arguments are gone.
    pub(crate) fn side_changed(&mut self, compute: bool) {
        self.sides[usize::from(compute)].clear();
    }

    /// Descriptor heaps were set: tables are not kept, and the root
    /// signatures are set again after, so the arguments are gone.
    pub(crate) fn heaps_changed(&mut self) {
        for side in &mut self.sides {
            side.clear();
        }
    }

    /// One root constant of `param` was set by itself.
    pub(crate) fn constant_set(&mut self, compute: bool, param: u32) {
        if let Some(bit) = 1u64.checked_shl(param).filter(|_| (param as usize) < PARAMS) {
            self.sides[usize::from(compute)].known &= !bit;
        }
    }

    /// A vertex buffer was set by itself, not through [`Cache::vertex`].
    pub(crate) fn vertex_set(&mut self, slot: u32) {
        self.vertex_known &= !(1u32.checked_shl(slot).unwrap_or(0));
    }

    fn count(&mut self, kind: Kind, issue: bool) -> bool {
        let at = kind as usize;
        if issue {
            self.issued[at] += 1;
        } else {
            self.skipped[at] += 1;
        }
        issue
    }

    /// Whether to record `Set*Root32BitConstants(param, values.len(), values, 0)`.
    pub(crate) fn constants(&mut self, compute: bool, param: u32, values: &[u32]) -> bool {
        if !self.seen || param as usize >= PARAMS {
            return true;
        }
        let side = &mut self.sides[usize::from(compute)];
        if side.constants.is_empty() {
            side.constants.resize_with(PARAMS, Vec::new);
        }
        let bit = 1u64 << param;
        let held = &mut side.constants[param as usize];
        if side.known & bit != 0 && held.as_slice() == values {
            return self.count(Kind::Constants, false);
        }
        held.clear();
        held.extend_from_slice(values);
        side.known |= bit;
        self.count(Kind::Constants, true)
    }

    /// Whether to record a root view or a table (`kind`) of `param` with
    /// `value` (a GPU address or a descriptor handle).
    pub(crate) fn descriptor(&mut self, compute: bool, kind: Kind, param: u32, value: u64) -> bool {
        if !self.seen || param as usize >= PARAMS {
            return true;
        }
        let given = Given { kind: kind as u8 + 1, value };
        let slot = &mut self.sides[usize::from(compute)].given[param as usize];
        if *slot == given {
            return self.count(kind, false);
        }
        *slot = given;
        self.count(kind, true)
    }

    /// Whether to record `IASetVertexBuffers(0, views)`.
    pub(crate) fn vertex(&mut self, views: &[VertexView]) -> bool {
        if !self.seen || views.len() > self.vertex.len() {
            return true;
        }
        let all = if views.len() == 32 { u32::MAX } else { (1u32 << views.len()) - 1 };
        if self.vertex_known & all == all && self.vertex[..views.len()] == *views {
            return self.count(Kind::Vertex, false);
        }
        self.vertex[..views.len()].copy_from_slice(views);
        self.vertex_known |= all;
        self.count(Kind::Vertex, true)
    }

    /// Whether to record `IASetIndexBuffer(view)`.
    pub(crate) fn index(&mut self, view: IndexView) -> bool {
        if !self.seen {
            return true;
        }
        if self.index == Some(view) {
            return self.count(Kind::Index, false);
        }
        self.index = Some(view);
        self.count(Kind::Index, true)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A cache that is on, whatever the environment says.
    fn cache() -> Cache {
        let mut c = Cache::default();
        c.seen = true;
        c
    }

    #[test]
    fn constants_equal_are_skipped_and_changed_are_recorded() {
        let mut c = cache();
        assert!(c.constants(false, 3, &[1, 2, 3]));
        assert!(!c.constants(false, 3, &[1, 2, 3]));
        assert!(c.constants(false, 3, &[1, 2, 4]));
        assert!(!c.constants(false, 3, &[1, 2, 4]));
        // Another count of the same parameter is another call.
        assert!(c.constants(false, 3, &[1, 2]));
        // Another parameter, another side.
        assert!(c.constants(false, 4, &[1, 2]));
        assert!(c.constants(true, 3, &[1, 2]));
        assert!(!c.constants(true, 3, &[1, 2]));
        assert_eq!((c.issued[0], c.skipped[0]), (5, 3));
    }

    #[test]
    fn descriptors_are_told_apart_by_kind_and_value() {
        let mut c = cache();
        assert!(c.descriptor(false, Kind::Srv, 1, 0x1000));
        assert!(!c.descriptor(false, Kind::Srv, 1, 0x1000));
        assert!(c.descriptor(false, Kind::Srv, 1, 0x2000));
        assert!(c.descriptor(false, Kind::Table, 1, 0x2000));
        assert!(!c.descriptor(false, Kind::Table, 1, 0x2000));
        assert!(c.descriptor(true, Kind::Table, 1, 0x2000));
    }

    #[test]
    fn a_root_signature_drops_one_side() {
        let mut c = cache();
        c.constants(false, 0, &[7]);
        c.constants(true, 0, &[7]);
        c.descriptor(false, Kind::Srv, 2, 5);
        c.descriptor(true, Kind::Srv, 2, 5);
        c.side_changed(true);
        assert!(!c.constants(false, 0, &[7]));
        assert!(!c.descriptor(false, Kind::Srv, 2, 5));
        assert!(c.constants(true, 0, &[7]));
        assert!(c.descriptor(true, Kind::Srv, 2, 5));
    }

    #[test]
    fn heaps_drop_both_sides_but_not_the_input_assembler() {
        let mut c = cache();
        c.constants(false, 0, &[7]);
        c.constants(true, 0, &[7]);
        c.descriptor(true, Kind::Table, 1, 9);
        c.vertex(&[(1, 2, 3)]);
        c.index((1, 2, 3));
        c.heaps_changed();
        assert!(c.constants(false, 0, &[7]));
        assert!(c.constants(true, 0, &[7]));
        assert!(c.descriptor(true, Kind::Table, 1, 9));
        assert!(!c.vertex(&[(1, 2, 3)]));
        assert!(!c.index((1, 2, 3)));
    }

    #[test]
    fn a_reset_drops_everything() {
        let mut c = cache();
        c.constants(false, 0, &[7]);
        c.descriptor(true, Kind::Table, 1, 9);
        c.vertex(&[(1, 2, 3), (4, 5, 6)]);
        c.index((1, 2, 3));
        c.clear();
        assert!(c.constants(false, 0, &[7]));
        assert!(c.descriptor(true, Kind::Table, 1, 9));
        assert!(c.vertex(&[(1, 2, 3), (4, 5, 6)]));
        assert!(c.index((1, 2, 3)));
    }

    #[test]
    fn a_single_constant_drops_its_parameter() {
        let mut c = cache();
        c.constants(true, 2, &[1, 2, 3]);
        c.constants(true, 3, &[1]);
        c.constant_set(true, 2);
        assert!(c.constants(true, 2, &[1, 2, 3]));
        assert!(!c.constants(true, 3, &[1]));
    }

    #[test]
    fn vertex_buffers_compare_by_prefix_and_slot() {
        let mut c = cache();
        assert!(c.vertex(&[(1, 1, 1), (2, 2, 2)]));
        // A shorter list of the same first slots sets only those.
        assert!(!c.vertex(&[(1, 1, 1)]));
        // A longer list has a slot not known.
        assert!(c.vertex(&[(1, 1, 1), (2, 2, 2), (3, 3, 3)]));
        assert!(!c.vertex(&[(1, 1, 1), (2, 2, 2), (3, 3, 3)]));
        assert!(c.vertex(&[(1, 1, 1), (9, 9, 9), (3, 3, 3)]));
        // One slot set by itself is not known afterwards.
        c.vertex_set(1);
        assert!(c.vertex(&[(1, 1, 1), (9, 9, 9)]));
        assert!(!c.vertex(&[(1, 1, 1), (9, 9, 9)]));
        assert!(c.vertex(&[(0, 0, 0); 32]));
        assert!(!c.vertex(&[(0, 0, 0); 32]));
    }

    #[test]
    fn index_buffers_are_skipped_when_equal() {
        let mut c = cache();
        assert!(c.index((1, 2, 57)));
        assert!(!c.index((1, 2, 57)));
        assert!(c.index((1, 2, 42)));
    }

    #[test]
    fn parameters_past_the_table_are_always_recorded() {
        let mut c = cache();
        assert!(c.constants(false, 64, &[1]));
        assert!(c.constants(false, 64, &[1]));
        assert!(c.descriptor(false, Kind::Srv, 200, 1));
        assert!(c.descriptor(false, Kind::Srv, 200, 1));
        c.constant_set(false, 64);
        c.vertex_set(40);
    }

    #[test]
    fn an_off_cache_records_everything_and_a_switch_clears_it() {
        let mut c = Cache::default();
        assert!(!c.seen);
        assert!(c.constants(false, 0, &[1]));
        assert!(c.constants(false, 0, &[1]));
        assert!(c.vertex(&[(1, 1, 1)]));
        assert!(c.vertex(&[(1, 1, 1)]));
        assert!(c.index((1, 1, 1)));
        assert!(c.index((1, 1, 1)));
        assert_eq!(c.skipped, [0; 7]);
        // On: what it held while off is not held (it kept nothing); then it
        // keeps.
        c.seen = true;
        assert!(c.constants(false, 0, &[1]));
        assert!(!c.constants(false, 0, &[1]));
        // Off and on again with entries held: they are dropped at the change.
        c.seen = false;
        c.clear();
        c.seen = true;
        assert!(c.constants(false, 0, &[1]));
    }

    #[test]
    fn sync_clears_when_the_switch_changes() {
        let mut c = cache();
        c.constants(false, 0, &[1]);
        c.vertex(&[(1, 1, 1)]);
        assert_ne!(c.sides[0].known, 0);
        // The environment decides `on()`; a cache whose last seen value
        // differs is cleared by `sync`, whichever way it differs.
        c.seen = !on();
        let now = c.sync();
        assert_eq!(now, on());
        assert_eq!(c.vertex_known, 0);
        assert_eq!(c.sides[0].known, 0);
    }
}
