//! Why a draw, a dispatch or an indirect execution did not run on the host:
//! every one counted by its reason, so the frame line names them.
//!
//! The frame line's "dropped N without a host pipeline" was one number for
//! all of them: stages of the ray-tracing structure builder that is off and
//! programs on a skip list (left out on purpose, every frame), next to
//! pipelines still being made, pipelines the host refused, descriptor tables
//! that found no slots and binds that failed (not meant, and each a hole in
//! the picture). The diagnostics lines by pipeline name only the first kind
//! and the pipelines; a draw whose pipeline is made and whose table was
//! refused had no line at all.
//!
//! Plain data, so the counting is tested without a card.
//!
//! From aki-gta6/aki-runtime d9d1341 (Aki-Developer), GPL-3.0. Here indirect
//! executions are a row of their own (the frame line counts them apart).

use std::sync::atomic::{AtomicU64, Ordering};

/// Why a draw or dispatch did not run on the host. The first two are
/// intended (the title's work is left out on purpose); the others are not.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) enum Why {
    /// Left out by a list: `LEONIDA_SKIP_PIPELINES`, the built-in ones
    /// (`CS_RaytracingResolve` and the ray activation without the builder),
    /// mesh draws switched off for a test.
    SkipList,
    /// A stage of the ray-tracing structure builder, which is off.
    BuilderOff,
    /// Its host pipeline is not made yet (queued or compiling), or the one
    /// made again for the formats bound is not.
    NotMade,
    /// The host refused its pipeline, or a shader does not translate.
    Refused,
    /// A descriptor table found fewer dynamic slots free than it needs.
    TableFull,
    /// A table found enough free slots, but no run of its length.
    TableApart,
    /// A table was refused without a search: one as long had failed since
    /// slots were last given back.
    TableShortcut,
    /// A table refused for another cause (longer than the ring).
    TableOther,
    /// Binding failed for another cause (a root view, the heaps, memory).
    BindFailed,
    /// Anything else (no index buffer, rectangles, another kind of pipeline,
    /// indirect arguments outside title buffers, ...).
    Other,
}

/// The number of reasons.
pub(crate) const WHYS: usize = 10;

/// What did not run: a row of the counters each.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) enum Kind {
    /// A draw or a mesh dispatch.
    Draw,
    /// A compute dispatch.
    Dispatch,
    /// An indirect execution of any kind.
    Indirect,
}

/// The number of kinds.
pub(crate) const KINDS: usize = 3;

impl Why {
    pub(crate) const ALL: [Why; WHYS] = [
        Why::SkipList,
        Why::BuilderOff,
        Why::NotMade,
        Why::Refused,
        Why::TableFull,
        Why::TableApart,
        Why::TableShortcut,
        Why::TableOther,
        Why::BindFailed,
        Why::Other,
    ];

    pub(crate) fn name(self) -> &'static str {
        match self {
            Why::SkipList => "skip list",
            Why::BuilderOff => "structure builder off",
            Why::NotMade => "pipeline not made yet",
            Why::Refused => "pipeline refused",
            Why::TableFull => "table: ring full",
            Why::TableApart => "table: free slots apart",
            Why::TableShortcut => "table: shortcut",
            Why::TableOther => "table: other",
            Why::BindFailed => "bind failed",
            Why::Other => "other",
        }
    }

    /// Left out on purpose.
    pub(crate) fn by_design(self) -> bool {
        matches!(self, Why::SkipList | Why::BuilderOff)
    }
}

/// Why a table asked of a ring of dynamic slots found none, from what the
/// ring said when it was asked (`HostHeap::refusal`): `free` slots of
/// `ring` were free, the table needs `n`, and no table of `too_long` slots
/// or more had found a run since slots were last given back.
pub(crate) fn table_refusal(n: u32, ring: u32, free: u32, too_long: u32) -> Why {
    if n == 0 || n > ring {
        Why::TableOther
    } else if free < n {
        Why::TableFull
    } else if n >= too_long {
        Why::TableShortcut
    } else {
        Why::TableApart
    }
}

/// The counters: by kind and reason, since the start. Only ever added to.
pub(crate) struct Tally([[AtomicU64; WHYS]; KINDS]);

impl Tally {
    pub(crate) const fn new() -> Tally {
        Tally([const { [const { AtomicU64::new(0) }; WHYS] }; KINDS])
    }

    pub(crate) fn count(&self, kind: Kind, why: Why) {
        self.0[kind as usize][why as usize].fetch_add(1, Ordering::Relaxed);
    }

    /// By reason, over the kinds, in [`Why::ALL`]'s order.
    pub(crate) fn reasons(&self) -> [u64; WHYS] {
        std::array::from_fn(|i| self.0.iter().map(|row| row[i].load(Ordering::Relaxed)).sum())
    }

    /// Of `kind`: those left out by design, and those that were not meant
    /// to be.
    #[cfg_attr(not(feature = "diagnostics"), allow(dead_code))]
    pub(crate) fn totals(&self, kind: Kind) -> (u64, u64) {
        let row = &self.0[kind as usize];
        Why::ALL.iter().fold((0, 0), |(design, unintended), why| {
            let n = row[*why as usize].load(Ordering::Relaxed);
            if why.by_design() { (design + n, unintended) } else { (design, unintended + n) }
        })
    }
}

static TALLY: Tally = Tally::new();

thread_local! {
    /// The reason the draw or dispatch being recorded on this thread is
    /// about to be dropped for, set where it is known. (Plain data: nothing
    /// runs when the thread ends.)
    static WHY: std::cell::Cell<Option<Why>> = const { std::cell::Cell::new(None) };
}

/// A draw or dispatch begins: no reason is noted for it yet.
pub(crate) fn begin() {
    WHY.set(None);
}

/// Notes the reason (the first one noted for a draw stays).
pub(crate) fn note(why: Why) {
    if WHY.get().is_none() {
        WHY.set(Some(why));
    }
}

/// Counts one of `kind` that did not run, for `why`.
pub(crate) fn count(kind: Kind, why: Why) {
    WHY.set(None);
    TALLY.count(kind, why);
}

/// Counts one of `kind` that did not run, for the reason noted (else
/// `fallback`).
pub(crate) fn count_noted(kind: Kind, fallback: Why) {
    let why = WHY.take().unwrap_or(fallback);
    TALLY.count(kind, why);
}

/// Of draws, dispatches and indirect executions together, since the start:
/// those left out by design, and those not meant to be (the F3 panel's).
#[cfg_attr(not(feature = "diagnostics"), allow(dead_code))]
pub(crate) fn totals() -> (u64, u64) {
    [Kind::Draw, Kind::Dispatch, Kind::Indirect].iter().fold((0, 0), |(design, unintended), kind| {
        let (d, u) = TALLY.totals(*kind);
        (design + d, unintended + u)
    })
}

/// Tables that found no dynamic slots at first, since the start: those that
/// got them once the slots of flights the card had run were given back
/// (nothing waited for), and those the flights in the air were waited for
/// (`crate::host::table_slots`; a wait holds up the thread that records).
static TABLES_HELPED: [AtomicU64; 2] = [const { AtomicU64::new(0) }; 2];

/// A table got its slots from flights the card had run (`waited` false), or
/// the flights in the air were waited for on its behalf.
pub(crate) fn table_helped(waited: bool) {
    TABLES_HELPED[usize::from(waited)].fetch_add(1, Ordering::Relaxed);
}

/// What the frame line says of tables helped: `reaped` and `waited` per
/// frame; nothing when there were none.
pub(crate) fn tables_text(reaped: f64, waited: f64) -> String {
    if reaped <= 0.0 && waited <= 0.0 {
        return String::new();
    }
    format!(
        "; tables without slots at first: {reaped:.1} got them from flights the card had run, {waited:.1} waited for the card (LEONIDA_SLOT_SETTLE=0: refused instead)"
    )
}

/// What the frame line says of the last `frames` frames: the reasons that
/// grew from `was` to `now`, per frame, those by design first.
/// `"none"` when nothing was dropped.
pub(crate) fn text(was: &[u64; WHYS], now: &[u64; WHYS], frames: f64) -> String {
    let frames = frames.max(1.0);
    let per_frame = |i: usize| now[i].saturating_sub(was[i]) as f64 / frames;
    let list = |design: bool| -> (f64, Vec<String>) {
        let mut sum = 0.0;
        let mut names = Vec::new();
        for (i, why) in Why::ALL.iter().enumerate() {
            let n = per_frame(i);
            if why.by_design() == design && n > 0.0 {
                sum += n;
                names.push(format!("{} {n:.1}", why.name()));
            }
        }
        (sum, names)
    };
    let (design, design_names) = list(true);
    let (unintended, unintended_names) = list(false);
    if design_names.is_empty() && unintended_names.is_empty() {
        return "none".to_owned();
    }
    let part = |sum: f64, names: Vec<String>| {
        if names.is_empty() { format!("{sum:.1}") } else { format!("{sum:.1} ({})", names.join(", ")) }
    };
    format!("left out on purpose {}, not meant {}", part(design, design_names), part(unintended, unintended_names))
}

/// [`text`] of what came since the last call (the frame line's, once a
/// line), and the two sums per frame: by design, not meant.
pub(crate) fn line(frames: f64) -> (String, f64, f64) {
    static WAS: std::sync::Mutex<[u64; WHYS]> = std::sync::Mutex::new([0; WHYS]);
    let now = TALLY.reasons();
    let was = std::mem::replace(&mut *WAS.lock().unwrap_or_else(|e| e.into_inner()), now);
    let frames = frames.max(1.0);
    let sum = |design: bool| -> f64 {
        Why::ALL
            .iter()
            .enumerate()
            .filter(|(_, why)| why.by_design() == design)
            .map(|(i, _)| now[i].saturating_sub(was[i]) as f64 / frames)
            .sum()
    };
    static HELPED: [AtomicU64; 2] = [const { AtomicU64::new(0) }; 2];
    let [reaped, waited] = [0, 1].map(|i| {
        let now = TABLES_HELPED[i].load(Ordering::Relaxed);
        now.saturating_sub(HELPED[i].swap(now, Ordering::Relaxed)) as f64 / frames
    });
    (text(&was, &now, frames) + &tables_text(reaped, waited), sum(true), sum(false))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_reason_has_its_own_place_and_name() {
        for (i, why) in Why::ALL.iter().enumerate() {
            assert_eq!(*why as usize, i);
        }
        let mut names: Vec<&str> = Why::ALL.iter().map(|w| w.name()).collect();
        names.sort_unstable();
        names.dedup();
        assert_eq!(names.len(), WHYS);
        assert_eq!(Why::ALL.iter().filter(|w| w.by_design()).count(), 2);
    }

    #[test]
    fn a_tally_keeps_kinds_and_reasons_apart() {
        let t = Tally::new();
        for _ in 0..300 {
            t.count(Kind::Dispatch, Why::BuilderOff);
        }
        t.count(Kind::Dispatch, Why::SkipList);
        t.count(Kind::Draw, Why::TableFull);
        t.count(Kind::Draw, Why::NotMade);
        t.count(Kind::Indirect, Why::NotMade);
        assert_eq!(t.totals(Kind::Dispatch), (301, 0));
        assert_eq!(t.totals(Kind::Draw), (0, 2));
        assert_eq!(t.totals(Kind::Indirect), (0, 1));
        let reasons = t.reasons();
        assert_eq!(reasons[Why::BuilderOff as usize], 300);
        assert_eq!(reasons[Why::NotMade as usize], 2);
        assert_eq!(reasons.iter().sum::<u64>(), 304);
    }

    #[test]
    fn the_text_names_what_grew_per_frame() {
        let mut was = [0u64; WHYS];
        let mut now = [0u64; WHYS];
        assert_eq!(text(&was, &now, 10.0), "none");
        was[Why::BuilderOff as usize] = 1000;
        now[Why::BuilderOff as usize] = 4000;
        now[Why::SkipList as usize] = 70;
        assert_eq!(
            text(&was, &now, 10.0),
            "left out on purpose 307.0 (skip list 7.0, structure builder off 300.0), not meant 0.0"
        );
        now[Why::TableFull as usize] = 25;
        now[Why::Refused as usize] = 5;
        assert_eq!(
            text(&was, &now, 10.0),
            "left out on purpose 307.0 (skip list 7.0, structure builder off 300.0), not meant 3.0 (pipeline refused 0.5, table: ring full 2.5)"
        );
        // A counter never goes back; if it did, nothing is said of it.
        was[Why::Other as usize] = 9;
        assert!(!text(&was, &now, 10.0).contains("other"));
    }

    #[test]
    fn tables_helped_are_said_only_when_there_were_any() {
        assert_eq!(tables_text(0.0, 0.0), "");
        let text = tables_text(12.0, 0.5);
        assert!(text.contains("12.0 got them from flights the card had run"));
        assert!(text.contains("0.5 waited for the card"));
    }

    #[test]
    fn a_refused_table_is_named_by_what_the_ring_said() {
        // Fewer free than asked: the ring is full.
        assert_eq!(table_refusal(179, 262_144, 100, u32::MAX), Why::TableFull);
        // 3,404 free and no run of 179 (the measured case): they lie apart.
        assert_eq!(table_refusal(179, 262_144, 3_404, u32::MAX), Why::TableApart);
        // A table as long failed since the last release: not searched for.
        assert_eq!(table_refusal(179, 262_144, 3_404, 179), Why::TableShortcut);
        assert_eq!(table_refusal(200, 262_144, 3_404, 179), Why::TableShortcut);
        assert_eq!(table_refusal(100, 262_144, 3_404, 179), Why::TableApart);
        assert_eq!(table_refusal(0, 262_144, 3_404, u32::MAX), Why::TableOther);
        assert_eq!(table_refusal(300_000, 262_144, 262_144, u32::MAX), Why::TableOther);
    }

    #[test]
    fn the_first_reason_noted_for_a_draw_stays_and_is_taken_once() {
        begin();
        note(Why::TableFull);
        note(Why::BindFailed);
        assert_eq!(WHY.get(), Some(Why::TableFull));
        let before = TALLY.reasons();
        count_noted(Kind::Draw, Why::Other);
        count_noted(Kind::Draw, Why::Other);
        let after = TALLY.reasons();
        assert_eq!(after[Why::TableFull as usize] - before[Why::TableFull as usize], 1);
        assert!(after[Why::Other as usize] - before[Why::Other as usize] >= 1);
        assert_eq!(WHY.get(), None);
    }
}
