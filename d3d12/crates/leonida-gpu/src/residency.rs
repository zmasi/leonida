//! Host-memory admission for speculative texture work and reusable upload storage.
use std::sync::OnceLock;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use windows_sys::Win32::System::SystemInformation::{GlobalMemoryStatusEx, MEMORYSTATUSEX};

const MIB: u64 = 1 << 20;
const GIB: u64 = 1 << 30;
static PRESSURED: AtomicBool = AtomicBool::new(false);
static HEADROOM: AtomicU64 = AtomicU64::new(u64::MAX);

fn memory_status() -> Option<MEMORYSTATUSEX> {
    // SAFETY: the API initializes the fields of a correctly sized structure.
    let mut status: MEMORYSTATUSEX = unsafe { std::mem::zeroed() };
    status.dwLength = std::mem::size_of::<MEMORYSTATUSEX>() as u32;
    (unsafe { GlobalMemoryStatusEx(&mut status) } != 0).then_some(status)
}

// The texture sweep may run for seconds inside a single submitted frame.
static NEXT_COMMIT_SAMPLE: AtomicU64 = AtomicU64::new(0);
static AVAILABLE_COMMIT: AtomicU64 = AtomicU64::new(u64::MAX);
static RETRY_UPLOAD_AT: AtomicU64 = AtomicU64::new(0);

pub(crate) fn allocation_error(error: &windows::core::Error) {
    if error.code() == windows::Win32::Foundation::E_OUTOFMEMORY {
        count(SAID_FAILED);
        RETRY_UPLOAD_AT.fetch_max(crate::host::coarse_ms().saturating_add(500), Ordering::Relaxed);
    }
}

thread_local! {
    static STAGING_DEFERRED: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
}

pub(crate) fn note_staging_deferred() {
    STAGING_DEFERRED.set(true);
}

pub(crate) fn take_staging_deferred() -> bool {
    STAGING_DEFERRED.replace(false)
}

pub(crate) fn upload_refused() -> bool {
    crate::host::coarse_ms() < RETRY_UPLOAD_AT.load(Ordering::Relaxed)
}

fn sample_commit(now: u64) -> u64 {
    let next = NEXT_COMMIT_SAMPLE.load(Ordering::Relaxed);
    if now >= next
        && NEXT_COMMIT_SAMPLE
            .compare_exchange(next, now.saturating_add(32), Ordering::Relaxed, Ordering::Relaxed)
            .is_ok()
        && let Some(status) = memory_status()
    {
        AVAILABLE_COMMIT.store(status.ullAvailPageFile, Ordering::Relaxed);
    }
    AVAILABLE_COMMIT.load(Ordering::Relaxed)
}

fn has_upload_reserve(commit: u64, bake: bool) -> bool {
    // Keep a protective floor against Rust/game OOM aborts without starving
    // bindless world textures or one-shot ped decoration bakes when steady-state
    // commit headroom sits in the 1-4 GiB range on 32 GiB hosts.
    commit >= if bake { 256 * MIB } else { 512 * MIB }
}

fn has_growth_reserve(commit: u64, bake: bool) -> bool {
    // Growing a fresh 64 MiB upload block for speculative bindless prefetch
    // requires more headroom than reusing slices already committed inside the
    // staging pool.
    commit >= if bake { 256 * MIB } else { 1024 * MIB }
}

/// Leave commit for game allocations and submissions already in flight while
/// still letting bindless textures and one-shot material bakes upload.
pub(crate) fn admit_texture_pass(bake: bool) -> bool {
    let now = crate::host::coarse_ms();
    count(SAID_PASSES);
    if now < RETRY_UPLOAD_AT.load(Ordering::Relaxed) {
        count(SAID_PASSES_REFUSED_FAILURE);
        return false;
    }
    let admitted = has_upload_reserve(sample_commit(now), bake);
    if !admitted {
        count(SAID_PASSES_REFUSED_RESERVE);
    }
    admitted
}

/// What this module said since the start, for the memory lines ([`line`]):
/// looks once a second ([`refresh`]), those under pressure, those of them
/// with commit above where pressure by commit begins (4 GiB: the pressure was
/// physical memory's), those with commit under [`critical_commit`]'s 1.5 GiB;
/// texture passes asked for and refused for a failed allocation and for the
/// reserve ([`admit_texture_pass`]); new staging buffers asked for and
/// refused likewise ([`allow_staging_growth`]); allocations that failed for
/// want of memory; questions whether commit is critical and how often it was.
static SAID: [AtomicU64; 13] = [const { AtomicU64::new(0) }; 13];
const SAID_LOOKS: usize = 0;
const SAID_PRESSURED: usize = 1;
const SAID_PRESSURED_PHYSICAL: usize = 2;
const SAID_LOOKS_CRITICAL: usize = 3;
const SAID_PASSES: usize = 4;
const SAID_PASSES_REFUSED_FAILURE: usize = 5;
const SAID_PASSES_REFUSED_RESERVE: usize = 6;
const SAID_GROWTH: usize = 7;
const SAID_GROWTH_REFUSED_FAILURE: usize = 8;
const SAID_GROWTH_REFUSED_RESERVE: usize = 9;
const SAID_FAILED: usize = 10;
const SAID_CRITICAL_ASKED: usize = 11;
const SAID_CRITICAL: usize = 12;
/// The least physical memory free and the least commit left that [`refresh`]
/// has seen.
static LEAST: [AtomicU64; 2] = [const { AtomicU64::new(u64::MAX) }; 2];

fn count(what: usize) {
    SAID[what].fetch_add(1, Ordering::Relaxed);
}

/// The memory lines' line of what this module said since the start
/// ([`SAID`]).
#[cfg(feature = "diagnostics")]
pub(crate) fn line() {
    let n: Vec<u64> = SAID.iter().map(|c| c.load(Ordering::Relaxed)).collect();
    let least = |i: usize| match LEAST[i].load(Ordering::Relaxed) {
        u64::MAX => 0,
        bytes => bytes >> 20,
    };
    tracing::info!(
        "memory, the PC's (residency), since the start: {} looks a second apart, {} under pressure ({} of them with more than 4 GiB left to commit: \
         by physical memory alone), {} with less than 1.5 GiB left to commit; the least seen: {} MB of physical memory free, {} MB left to commit; \
         texture passes asked for {}, refused {} after a failed allocation and {} for the reserve; new staging buffers ahead asked for {}, refused {} \
         and {}; allocations failed for want of memory {}; commit critical in {} of {} questions; now {} (LEONIDA_COMMIT_FLOOR: {})",
        n[SAID_LOOKS],
        n[SAID_PRESSURED],
        n[SAID_PRESSURED_PHYSICAL],
        n[SAID_LOOKS_CRITICAL],
        least(0),
        least(1),
        n[SAID_PASSES],
        n[SAID_PASSES_REFUSED_FAILURE],
        n[SAID_PASSES_REFUSED_RESERVE],
        n[SAID_GROWTH],
        n[SAID_GROWTH_REFUSED_FAILURE],
        n[SAID_GROWTH_REFUSED_RESERVE],
        n[SAID_FAILED],
        n[SAID_CRITICAL],
        n[SAID_CRITICAL_ASKED],
        if pressured() { "under pressure" } else { "not under pressure" },
        crate::resource::commit_floor_note(),
    );
}

/// The commit the system has left, sampled at most 32 ms ago.
pub(crate) fn available_commit() -> u64 {
    sample_commit(crate::host::coarse_ms())
}

/// Whether the staging allocator may commit a brand-new OS upload buffer
/// (reusing existing free ranges in already-committed pool blocks stays open
/// whenever `admit_texture_pass` is true).
pub(crate) fn allow_staging_growth(bake: bool) -> bool {
    let now = crate::host::coarse_ms();
    count(SAID_GROWTH);
    if now < RETRY_UPLOAD_AT.load(Ordering::Relaxed) {
        count(SAID_GROWTH_REFUSED_FAILURE);
        return false;
    }
    let allowed = has_growth_reserve(sample_commit(now), bake);
    if !allowed {
        count(SAID_GROWTH_REFUSED_RESERVE);
    }
    allowed
}

pub(crate) fn limited_host_ram() -> bool {
    static LIMITED: OnceLock<bool> = OnceLock::new();
    *LIMITED.get_or_init(|| memory_status().is_some_and(|s| s.ullTotalPhys <= 32 * GIB))
}

fn next_pressure(was: bool, physical: u64, commit: u64) -> bool {
    if was {
        physical < 5 * GIB || commit < 6 * GIB
    } else {
        physical <= 3 * GIB || commit <= 4 * GIB
    }
}

/// Called by the once-per-second submission sweep. A failed sample keeps the last good state.
pub(crate) fn refresh() {
    let Some(status) = memory_status() else { return };
    let (physical, commit) = (status.ullAvailPhys, status.ullAvailPageFile);
    HEADROOM.store(physical.min(commit), Ordering::Relaxed);
    let was = pressured();
    let pressure = next_pressure(was, physical, commit);
    count(SAID_LOOKS);
    if pressure {
        count(SAID_PRESSURED);
        if commit > 4 * GIB {
            count(SAID_PRESSURED_PHYSICAL);
        }
    }
    if commit < 1536 * MIB {
        count(SAID_LOOKS_CRITICAL);
    }
    LEAST[0].fetch_min(physical, Ordering::Relaxed);
    LEAST[1].fetch_min(commit, Ordering::Relaxed);
    if pressure != was {
        PRESSURED.store(pressure, Ordering::Relaxed);
        if pressure {
            tracing::warn!(
                pid = std::process::id(),
                frame = crate::frame::current(),
                physical_free_mib = physical >> 20,
                available_commit_mib = commit >> 20,
                buffer_mirror_span_mib = crate::shadow::live_span_bytes() >> 20,
                texture_staging_mib = crate::texture::live_staging_bytes() >> 20,
                spare_scratch_mib = crate::host::ALIVE[4].load(Ordering::Relaxed) >> 20,
                game_filled_gpu_texture_mib = crate::resource::KINDS[1].load(Ordering::Relaxed) >> 20,
                "host memory pressure: limiting texture prefetch and idle upload storage"
            );
        } else {
            tracing::info!(physical_free_mib = physical >> 20, available_commit_mib = commit >> 20, "host memory pressure cleared");
        }
    }
}

pub(crate) fn headroom() -> Option<u64> {
    let bytes = HEADROOM.load(Ordering::Relaxed);
    (bytes != u64::MAX).then_some(bytes)
}

pub(crate) fn pressured() -> bool {
    PRESSURED.load(Ordering::Relaxed)
}

/// True when OS commit headroom is critically low (< 1.5 GiB) or an allocation
/// just failed with `E_OUTOFMEMORY`, requiring emergency throttling.
pub(crate) fn critical_commit() -> bool {
    let now = crate::host::coarse_ms();
    let critical = now < RETRY_UPLOAD_AT.load(Ordering::Relaxed) || sample_commit(now) < 1536 * MIB;
    count(SAID_CRITICAL_ASKED);
    if critical {
        count(SAID_CRITICAL);
    }
    critical
}

/// A soft per-frame admission limit. A single texture can cross it. The
/// queue retains unserved slots so they can be retried on later frames.
pub(crate) fn prefetch_limit(bytes: u64, pressure: bool, critical: bool, limited: bool, staging: u64) -> u64 {
    if critical {
        bytes.min(16 << 20)
    } else if pressure || (limited && staging >= GIB) {
        bytes.min(64 << 20)
    } else {
        bytes
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bake_and_lookahead_leave_room_for_live_game_allocations() {
        for (commit, bake, lookahead, growth) in [
            (128 * MIB, false, false, false),
            (256 * MIB, true, false, false),
            (512 * MIB, true, true, false),
            (768 * MIB, true, true, false),
            (1024 * MIB, true, true, true),
        ] {
            assert_eq!(has_upload_reserve(commit, true), bake);
            assert_eq!(has_upload_reserve(commit, false), lookahead);
            assert_eq!(has_growth_reserve(commit, false), growth);
        }
    }

    #[test]
    fn either_constraint_enters_pressure_and_both_must_recover() {
        assert!(next_pressure(false, 12 * GIB, 4 * GIB));
        assert!(next_pressure(false, 3 * GIB, 16 * GIB));
        assert!(next_pressure(true, 4 * GIB, 7 * GIB));
        assert!(next_pressure(true, 6 * GIB, 5 * GIB));
        assert!(!next_pressure(true, 5 * GIB, 6 * GIB));
        assert!(!next_pressure(false, 4 * GIB, 5 * GIB));
    }

    #[test]
    fn prefetch_respects_existing_budgets_and_resumes_after_recovery() {
        assert_eq!(prefetch_limit(512 << 20, false, false, true, GIB), 64 << 20);
        assert_eq!(prefetch_limit(512 << 20, false, false, true, GIB - 1), 512 << 20);
        assert_eq!(prefetch_limit(512 << 20, false, false, false, 2 * GIB), 512 << 20);
        assert_eq!(prefetch_limit(0, true, true, false, 0), 0);
        assert_eq!(prefetch_limit(8 << 20, true, true, false, 0), 8 << 20);
        assert_eq!(prefetch_limit(512 << 20, true, false, false, 0), 64 << 20);
        assert_eq!(prefetch_limit(512 << 20, true, true, false, 0), 16 << 20);
        assert_eq!(prefetch_limit(192 << 20, false, false, false, 0), 192 << 20);
    }
}
