//! Copy-back elision (`LEONIDA_COPYBACK_ELIDE`): a big host copy in the card's
//! memory that only the card ever changes is not copied back into title memory.
//!
//! Every submission that declares a copy's ranges as written by shaders
//! ([`Shadow::gpu_writes`]) has them compared on the card, fetched and put
//! into title memory where they differ. For a copy whose bytes the title never
//! reads on the CPU this is all for nothing: 22-26 MB a frame, 31-38 ms on the
//! submitting thread's chain, in a static scene on the Series X profile.
//!
//! Ported from maren-halv/VIctory-DXR dd83629 (`VICTORY_GPU_ONLY_COPYBACK`:
//! `GpuOnlyTracker`, `gpu_only_note_upload`, `gpu_only_note_fetched`,
//! `note_cpu_read`, `demote_overlapping_gpu_only`, `has_overlapping_shadow`),
//! GPL-3.0, as Leonida code: the decision logic is [`Tracker`], the rest
//! hooks it to Leonida's declarations, uploads, fetches and the list steps that
//! read title memory.
//!
//! This is a heuristic, not a proof, and so it is off by default. What is
//! known is what the host sees: nobody uploaded to the copy, shaders of
//! submissions that were landed wrote it for a while ([`WARMUP_FRAMES`]), no
//! other host copy of the same bytes exists, and no step of a list reads title
//! memory there. What cannot be seen is the title's own threads reading title
//! memory on the CPU (the reason the copy-back exists); a copy so read goes
//! stale from the day it is promoted. Hence the minimum size ([`min_bytes`]:
//! small read-back and query buffers are never skipped) and the sticky demotion
//! at the first sign of a reader.
//!
//! A promoted copy is [`Shadow::keep_stores`]: what shaders store there is not
//! declared, so from then on only the dwords the title changes go up, as for
//! a copy by address ([`super::by_address`]). A demoted copy is declared again
//! and compared as before. Its card-side hashes were not updated while it was
//! promoted, so the first compare of a range finds what the card wrote since
//! and lands it (the fork marks the hashes stale instead, and loses those bytes;
//! that Leonida's compare heals is read from `Host::card_before`, not measured).

use super::{Arc, CARDED, EXACT, LOOSE, Shadow, WINDOW, buffers_read};
use std::sync::atomic::{AtomicU64, Ordering};

/// The state of a copy ([`Shadow::elide`]).
pub(super) const OBSERVING: u8 = 0;
pub(super) const PROMOTED: u8 = 1;
/// Sticky.
pub(super) const DEMOTED: u8 = 2;

/// Frames from the first fetch seen to the promotion, at least.
pub(super) const WARMUP_FRAMES: u64 = 60;
/// Pieces fetched in that time, at least.
pub(super) const MIN_FETCHED_PIECES: u64 = 16;
/// Times the window starts over (an upload came in) before the copy is demoted for good.
pub(super) const MAX_RESETS: u32 = 4;

const SWITCH: &str = "LEONIDA_COPYBACK_ELIDE";

/// Whether the elision watches copies (and promotes or demotes them): the switch is on, or `LEONIDA_AB` names it.
/// Watching goes on in the phases it is off, so that a copy promoted in one phase is not skipped
/// in the next after an upload that was not seen.
pub(super) fn enabled() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| {
        #[cfg(feature = "diagnostics")]
        if crate::ab::bit(SWITCH).is_some() {
            return true;
        }
        crate::switch::on(SWITCH)
    })
}

/// Whether a promoted copy is skipped now. Off unless asked for (`LEONIDA_COPYBACK_ELIDE=1`, or `2`
/// for copies the CPU only uploads to apart from what shaders write): `crate::toggled!` is on unless
/// its variable is `0`, which is what this is not. `LEONIDA_AB=<s>,LEONIDA_COPYBACK_ELIDE` turns it on
/// and off while the title runs.
pub(super) fn on() -> bool {
    #[cfg(feature = "diagnostics")]
    if let Some(on) = crate::ab::pick(SWITCH) {
        return on;
    }
    enabled()
}

/// `1`: a copy is promoted when nothing was uploaded to it while it was watched. `2`: also one whose
/// uploaded pieces and fetched pieces are apart (`LEONIDA_COPYBACK_ELIDE=2`).
fn mode() -> u8 {
    static MODE: std::sync::OnceLock<u8> = std::sync::OnceLock::new();
    *MODE.get_or_init(|| match std::env::var(SWITCH).as_deref().map(str::trim) {
        Ok("2") => 2,
        _ => 1,
    })
}

/// The smallest copy that is watched (`LEONIDA_COPYBACK_ELIDE_MIN_MB`, 4: a small
/// buffer is the likely read back or query result).
fn min_bytes() -> u64 {
    static MIN: std::sync::OnceLock<u64> = std::sync::OnceLock::new();
    *MIN.get_or_init(|| std::env::var("LEONIDA_COPYBACK_ELIDE_MIN_MB").ok().and_then(|v| v.trim().parse::<u64>().ok()).unwrap_or(4) << 20)
}

/// The lowest start and the highest end of copies that are watched (`[0, 0]`: none), to let the
/// readers that are not near one pass without a lock.
static BOUNDS: [AtomicU64; 2] = [const { AtomicU64::new(0) }; 2];

/// Promoted copies, their bytes, demotions so far, and the declarations and bytes skipped since the last line.
static STATS: [AtomicU64; 5] = [const { AtomicU64::new(0) }; 5];

/// What a copy has seen since it was watched: the pieces (of 64 KiB) uploaded to it and fetched from it.
#[derive(Default)]
pub(super) struct Tracker {
    window_start: u64,
    fetched: u64,
    up_bits: Vec<u64>,
    fetched_bits: Vec<u64>,
    resets: u32,
}

impl Tracker {
    fn set(bits: &mut Vec<u64>, piece: usize) {
        let w = piece / 64;
        if w >= bits.len() {
            bits.resize(w + 1, 0);
        }
        bits[w] |= 1u64 << (piece % 64);
    }

    fn has(bits: &[u64], piece: usize) -> bool {
        bits.get(piece / 64).is_some_and(|&w| (w >> (piece % 64)) & 1 != 0)
    }

    /// The window starts over; whether the copy has had enough of that.
    fn reset(&mut self) -> bool {
        self.resets += 1;
        // The next window begins at the first fetch after this.
        self.window_start = 0;
        self.fetched = 0;
        self.up_bits.iter_mut().for_each(|w| *w = 0);
        self.fetched_bits.iter_mut().for_each(|w| *w = 0);
        self.resets > MAX_RESETS
    }

    /// Pieces `[first, last)` were uploaded to the copy. Whether it is demoted for good.
    pub(super) fn upload(&mut self, mode: u8, promoted: bool, first: usize, last: usize) -> bool {
        if first >= last {
            return false;
        }
        if mode == 1 {
            // Any upload: out of a promotion, and the watching starts over otherwise.
            return promoted || self.reset();
        }
        let mut overlaps = false;
        for p in first..last {
            if Self::has(&self.fetched_bits, p) {
                overlaps = true;
                break;
            }
            if !promoted {
                Self::set(&mut self.up_bits, p);
            }
        }
        if !overlaps {
            return false;
        }
        promoted || self.reset()
    }

    /// Pieces were fetched from the copy in `frame` (shaders wrote them). `(promote, demote)`.
    pub(super) fn fetch(&mut self, mode: u8, frame: u64, pieces: &[usize]) -> (bool, bool) {
        if pieces.is_empty() {
            return (false, false);
        }
        if self.window_start == 0 {
            self.window_start = frame;
        }
        for &p in pieces {
            if mode == 2 && Self::has(&self.up_bits, p) {
                return (false, self.reset());
            }
            Self::set(&mut self.fetched_bits, p);
        }
        self.fetched += pieces.len() as u64;
        (self.fetched >= MIN_FETCHED_PIECES && frame.saturating_sub(self.window_start) >= WARMUP_FRAMES, false)
    }
}

/// Whether another copy of any kind (a buffer's, a window's, a view's) holds some of `[start, start + len)`.
fn has_overlap(start: u64, len: u64, own: u64) -> bool {
    let Some(end) = start.checked_add(len) else {
        return true;
    };
    {
        let map = buffers_read();
        for (_, entry) in map.range(..=(start, usize::MAX)).rev().take(64) {
            if entry.end > start && entry.shadow.as_ref().is_some_and(|s| s.id != own) {
                return true;
            }
        }
        for (_, entry) in map.range((start.saturating_add(1), 0)..(end, 0)) {
            if entry.shadow.as_ref().is_some_and(|s| s.id != own) {
                return true;
            }
        }
    }
    {
        let exact = EXACT.lock().unwrap_or_else(|e| e.into_inner());
        const EXACT_MOST: u64 = 64 << 20;
        for (&(at, n), (s, _)) in exact.1.range((start.saturating_sub(EXACT_MOST), 0)..(end, 0)) {
            if at.saturating_add(n) > start && s.id != own {
                return true;
            }
        }
    }
    {
        let loose = LOOSE.read().unwrap_or_else(|e| e.into_inner());
        for (&(at, n), s) in loose.range((start.saturating_sub(WINDOW), 0)..(end, 0)) {
            if at.saturating_add(n) > start && s.id != own {
                return true;
            }
        }
    }
    false
}

/// Copies watched, other than `skip`, that hold some of `[start, start + len)`, sticky demoted.
fn demote_overlapping(start: u64, len: u64, skip: u64, why: &'static str) {
    let (lo, hi) = (BOUNDS[0].load(Ordering::Relaxed), BOUNDS[1].load(Ordering::Relaxed));
    let end = start.saturating_add(len);
    if hi == 0 || start >= hi || end <= lo || len == 0 {
        return;
    }
    let copies: Vec<Arc<Shadow>> = {
        let carded = CARDED.lock().unwrap_or_else(|e| e.into_inner());
        carded
            .values()
            .filter_map(std::sync::Weak::upgrade)
            .filter(|s| s.id != skip && s.start < end && s.start.saturating_add(s.len) > start)
            .collect()
    };
    for shadow in copies {
        shadow.elide_demote(why);
    }
}

/// A step of a list reads title memory `[at, at + len)` on the CPU (a buffer copy's source, a fill's,
/// a texture's, a plane's): a copy there is demoted. What the title's own threads read is not seen.
pub(crate) fn cpu_read(at: usize, len: usize) {
    if enabled() && len > 0 {
        demote_overlapping(at as u64, len as u64, 0, "a list step reads it on the CPU");
    }
}

/// A host copy was made, of `shadow`'s memory: copies of the same bytes that are watched are demoted
/// (the new one takes its bytes from title memory, which does not hold what shaders wrote there).
/// Not under the tables' locks.
pub(super) fn made(shadow: &Shadow) {
    if enabled() {
        demote_overlapping(shadow.start, shadow.len, shadow.id, "another host copy of the same memory was made");
    }
}

/// Pieces that were fetched from copies, by copy (consecutive ones together): for the watching.
pub(crate) fn fetched<'a>(pieces: impl Iterator<Item = (&'a Arc<Shadow>, usize, usize)>) {
    if !enabled() {
        return;
    }
    let mut run: Option<(&Arc<Shadow>, Vec<(usize, usize)>)> = None;
    for (shadow, lo, hi) in pieces {
        match &mut run {
            Some((last, ranges)) if Arc::ptr_eq(last, shadow) => ranges.push((lo, hi)),
            _ => {
                if let Some((last, ranges)) = run.take() {
                    last.elide_fetched(&ranges);
                }
                run = Some((shadow, vec![(lo, hi)]));
            }
        }
    }
    if let Some((last, ranges)) = run {
        last.elide_fetched(&ranges);
    }
}

/// A line for the frame lines, when watching.
#[cfg(feature = "diagnostics")]
pub(crate) fn log(frames: f64) {
    if !enabled() {
        return;
    }
    let skipped = STATS[3].swap(0, Ordering::Relaxed) as f64 / frames.max(1.0);
    let bytes = STATS[4].swap(0, Ordering::Relaxed) as f64 / 1e6 / frames.max(1.0);
    tracing::info!(
        "copy-back elision (LEONIDA_COPYBACK_ELIDE={}, on {}, from {} MB): {} copies promoted ({} MB), {} demoted; {skipped:.1} declarations a frame skipped ({bytes:.1} MB)",
        mode(),
        on(),
        min_bytes() >> 20,
        STATS[0].load(Ordering::Relaxed),
        STATS[1].load(Ordering::Relaxed) >> 20,
        STATS[2].load(Ordering::Relaxed),
    );
}

impl Shadow {
    /// A new copy (a title buffer's, in the card's memory): the bytes of the ones that are watched are noted.
    pub(super) fn elide_note_bounds(&self) {
        if !enabled() || self.window || self.card.is_none() || self.len < min_bytes() {
            return;
        }
        let end = self.start.saturating_add(self.len);
        let _ = BOUNDS[0].fetch_update(Ordering::Relaxed, Ordering::Relaxed, |cur| Some(if cur == 0 { self.start } else { cur.min(self.start) }));
        BOUNDS[1].fetch_max(end, Ordering::Relaxed);
    }

    /// Whether the declaration of shaders' writes here is left out: the copy is promoted and the elision on.
    #[inline]
    pub(super) fn elide_skip(&self) -> bool {
        self.elide.load(Ordering::Acquire) == PROMOTED && on()
    }

    /// A declaration of `len` bytes was left out.
    pub(super) fn elide_skipped(&self, len: u64) {
        STATS[3].fetch_add(1, Ordering::Relaxed);
        STATS[4].fetch_add(len, Ordering::Relaxed);
    }

    /// Out of the elision for good; the copy is declared and compared as any.
    pub(super) fn elide_demote(&self, why: &'static str) {
        let was = self.elide.swap(DEMOTED, Ordering::AcqRel);
        if was == PROMOTED {
            STATS[0].fetch_sub(1, Ordering::Relaxed);
            STATS[1].fetch_sub(self.len, Ordering::Relaxed);
            STATS[2].fetch_add(1, Ordering::Relaxed);
            tracing::info!(start = format_args!("{:#x}", self.start), len = self.len, why, "copy no longer left out of the copy-back (LEONIDA_COPYBACK_ELIDE)");
        }
    }

    /// `ranges` (offsets in the copy) went up to the card's side: the CPU wrote them.
    pub(super) fn elide_uploaded(&self, ranges: &[(usize, usize)]) {
        if !enabled() || self.window || self.len < min_bytes() {
            return;
        }
        let state = self.elide.load(Ordering::Acquire);
        if state == DEMOTED || self.card.is_none() {
            return;
        }
        let demote = {
            let mut tracker = self.elide_track.lock().unwrap_or_else(|e| e.into_inner());
            ranges.iter().filter(|r| r.0 < r.1).any(|&(a, b)| {
                let pieces = self.pieces(a, b);
                tracker.upload(mode(), state == PROMOTED, pieces.start, pieces.end)
            })
        };
        if demote {
            self.elide_demote("the CPU wrote it");
        }
    }

    /// `ranges` (offsets in the copy) were fetched from the card's side: shaders wrote them. May promote.
    pub(super) fn elide_fetched(&self, ranges: &[(usize, usize)]) {
        if self.window || self.len < min_bytes() || ranges.is_empty() || self.elide.load(Ordering::Acquire) != OBSERVING || self.card.is_none() {
            return;
        }
        let mut pieces: Vec<usize> = Vec::with_capacity(ranges.len());
        for &(lo, hi) in ranges {
            if lo < hi {
                pieces.extend(self.pieces(lo, hi));
            }
        }
        let (promote, demote) = self.elide_track.lock().unwrap_or_else(|e| e.into_inner()).fetch(mode(), crate::frame::current(), &pieces);
        if demote {
            self.elide_demote("uploads and fetches over the same pieces");
            return;
        }
        if !promote {
            return;
        }
        if has_overlap(self.start, self.len, self.id) {
            self.elide_demote("another host copy of the same memory");
            return;
        }
        if self.elide.compare_exchange(OBSERVING, PROMOTED, Ordering::AcqRel, Ordering::Acquire).is_ok() {
            // What shaders store there is not declared: only the dwords the title changes go up.
            self.keep_stores();
            STATS[0].fetch_add(1, Ordering::Relaxed);
            STATS[1].fetch_add(self.len, Ordering::Relaxed);
            tracing::info!(
                start = format_args!("{:#x}", self.start),
                len = self.len,
                "copy left out of the copy-back: nothing but shaders wrote it for {WARMUP_FRAMES} frames (LEONIDA_COPYBACK_ELIDE)"
            );
        }
    }

    /// Statistics when a copy goes.
    pub(super) fn elide_gone(&self) {
        if self.elide.load(Ordering::Relaxed) == PROMOTED {
            STATS[0].fetch_sub(1, Ordering::Relaxed);
            STATS[1].fetch_sub(self.len, Ordering::Relaxed);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn run(n: usize) -> Vec<usize> {
        (0..n).collect()
    }

    /// Mode 1: warmed up from the first fetch, enough pieces, and no upload since: promoted. An upload
    /// after that ends it; uploads before it start the window over, and too many end the watching.
    #[test]
    fn mode_one_promotes_after_warmup_and_demotes_on_upload() {
        let mut t = Tracker::default();
        // The boot upload starts the window over.
        assert!(!t.upload(1, false, 0, 16));
        assert_eq!(t.fetch(1, 2, &run(8)), (false, false));
        assert_eq!(t.fetch(1, 2 + WARMUP_FRAMES - 1, &run(MIN_FETCHED_PIECES as usize)), (false, false));
        assert_eq!(t.fetch(1, 2 + WARMUP_FRAMES, &[0]), (true, false));
        assert!(t.upload(1, true, 5, 6));
    }

    #[test]
    fn too_many_uploads_while_watching_end_the_watching() {
        let mut t = Tracker::default();
        for _ in 0..MAX_RESETS {
            assert!(!t.upload(1, false, 0, 1));
        }
        assert!(t.upload(1, false, 0, 1));
    }

    #[test]
    fn an_empty_range_is_nothing() {
        let mut t = Tracker::default();
        assert!(!t.upload(1, true, 3, 3));
        assert_eq!(t.fetch(1, 9, &[]), (false, false));
        // (and does not start the window)
        assert_eq!(t.window_start, 0);
    }

    /// Too few pieces fetched do not promote however long the wait; neither do enough in too few frames.
    #[test]
    fn it_takes_both_enough_pieces_and_enough_frames() {
        let mut t = Tracker::default();
        assert_eq!(t.fetch(1, 10, &run(MIN_FETCHED_PIECES as usize)), (false, false));
        assert_eq!(t.fetch(1, 10 + WARMUP_FRAMES - 1, &[1]), (false, false));
        let mut t = Tracker::default();
        assert_eq!(t.fetch(1, 10, &[0]), (false, false));
        assert_eq!(t.fetch(1, 10 + 10 * WARMUP_FRAMES, &[0]), (false, false));
    }

    /// Mode 2: uploads over other pieces than the ones fetched are fine, before and after promotion; one over a
    /// fetched piece ends it; before promotion it starts over.
    #[test]
    fn mode_two_allows_apart_uploads_and_demotes_on_overlap() {
        let mut t = Tracker::default();
        assert!(!t.upload(2, false, 0, 4));
        let gpu: Vec<usize> = (10..10 + MIN_FETCHED_PIECES as usize).collect();
        assert_eq!(t.fetch(2, 1, &gpu), (false, false));
        assert!(!t.upload(2, false, 0, 4));
        assert_eq!(t.fetch(2, 1 + WARMUP_FRAMES, &[10]), (true, false));
        assert!(!t.upload(2, true, 0, 4));
        assert!(t.upload(2, true, 10, 11));
    }

    /// Mode 2: a piece fetched that was uploaded to before starts the window over, with the pieces forgotten.
    #[test]
    fn mode_two_fetch_over_an_uploaded_piece_starts_over() {
        let mut t = Tracker::default();
        assert!(!t.upload(2, false, 4, 6));
        assert_eq!(t.fetch(2, 5, &[3, 4]), (false, false));
        assert_eq!(t.resets, 1);
        // Forgotten: the same piece is not found uploaded now.
        assert_eq!(t.fetch(2, 6, &[4]), (false, false));
        assert_eq!(t.resets, 1);
        // An overlapping upload before promotion starts over too, and the fetched piece is forgotten.
        assert!(!t.upload(2, false, 4, 5));
        assert_eq!(t.resets, 2);
        assert_eq!(t.fetch(2, 7, &[4]), (false, false));
    }

    /// Mode 2 ends the watching after as many starts over as mode 1.
    #[test]
    fn mode_two_gives_up_too() {
        let mut t = Tracker::default();
        for _ in 0..MAX_RESETS {
            assert!(!t.upload(2, false, 0, 1));
            assert_eq!(t.fetch(2, 1, &[0]), (false, false));
        }
        assert!(!t.upload(2, false, 0, 1));
        assert_eq!(t.fetch(2, 1, &[0]), (false, true));
    }

    #[test]
    fn bits_grow_to_the_piece() {
        let mut bits = Vec::new();
        assert!(!Tracker::has(&bits, 1000));
        Tracker::set(&mut bits, 1000);
        assert!(Tracker::has(&bits, 1000));
        assert!(!Tracker::has(&bits, 999));
        assert!(!Tracker::has(&bits, 100_000));
    }
}
