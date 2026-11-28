//! Which processors our own threads run on, where a processor has two kinds
//! of cores. `cpu_sets` and `keep_on_fast_cores` are fxceless/victory-fork's
//! (`framelog.rs`, commit 0b0074e0, GPL-3.0-or-later, as this).
//! Where our threads go beside the title's pinned ones (`title_affinity`,
//! `LEONIDA_PLACE_APART`, `LEONIDA_PLACE_LANE`): ported from
//! Biskwit/VIctory-reloaded (`crates/victory-gpu/src/cores.rs`, commit d92322077d,
//! through Keel62155/VIctoryUnified 74f4035fd9, GPL-3.0-or-later).

use std::cell::RefCell;
use std::sync::atomic::{AtomicU8, AtomicU64, Ordering::Relaxed};
use std::sync::{Mutex, OnceLock};

/// The system's processor sets of group 0: each one's id, its efficiency
/// class (higher is faster) and its logical processor's number.
fn cpu_sets() -> Vec<(u32, u8, u8)> {
    use windows_sys::Win32::System::SystemInformation::{
        CpuSetInformation, GetSystemCpuSetInformation, SYSTEM_CPU_SET_INFORMATION,
    };
    use windows_sys::Win32::System::Threading::GetCurrentProcess;
    let mut bytes = 0u32;
    // SAFETY: a size query, then the call into a buffer of that size.
    unsafe { GetSystemCpuSetInformation(std::ptr::null_mut(), 0, &mut bytes, GetCurrentProcess(), 0) };
    if bytes == 0 {
        return Vec::new();
    }
    let mut buffer = vec![0u64; (bytes as usize).div_ceil(8)];
    let cap = (buffer.len() * 8) as u32;
    // SAFETY: as above; `buffer` holds `cap` bytes (>= `bytes`), 8-byte aligned.
    let ok = unsafe { GetSystemCpuSetInformation(buffer.as_mut_ptr().cast(), cap, &mut bytes, GetCurrentProcess(), 0) };
    let used = (bytes as usize).min(buffer.len() * 8);
    let mut sets = Vec::new();
    let mut at = 0usize;
    while ok != 0 && at + size_of::<SYSTEM_CPU_SET_INFORMATION>() <= used {
        // SAFETY: `at + size_of::<SYSTEM_CPU_SET_INFORMATION>() <= used` lies within `buffer`;
        // `read_unaligned` avoids assuming variable-sized steps preserve alignment.
        let info = unsafe {
            buffer
                .as_ptr()
                .cast::<u8>()
                .add(at)
                .cast::<SYSTEM_CPU_SET_INFORMATION>()
                .read_unaligned()
        };
        let step = info.Size as usize;
        if step < size_of::<SYSTEM_CPU_SET_INFORMATION>() || step > used - at {
            break;
        }
        if info.Type == CpuSetInformation {
            // SAFETY: `info.Type == CpuSetInformation` selects the `CpuSet` union arm.
            let set = unsafe { &info.Anonymous.CpuSet };
            if set.Group == 0 {
                sets.push((set.Id, set.EfficiencyClass, set.LogicalProcessorIndex));
            }
        }
        at += step;
    }
    sets
}

/// The sets of the fastest class, if there are slower ones too.
///
/// Homogeneous processors (for instance multi-CCD AMD Ryzen 9 5950X or
/// single-CCD Ryzen 7 9800X3D, where every core shares one `EfficiencyClass`)
/// return an empty list so thread placement stays untouched.
fn fast_of(sets: &[(u32, u8, u8)]) -> Vec<u32> {
    let best = sets.iter().map(|c| c.1).max().unwrap_or(0);
    let fast: Vec<u32> = sets.iter().filter(|c| c.1 == best).map(|c| c.0).collect();
    if fast.len() == sets.len() { Vec::new() } else { fast }
}

/// Of the sets of the logical processors the title asked none of its threads
/// to run on (`asked`, a bit each): the fast ones (`Some(true)`), the slow
/// ones (`Some(false)`) or all (`None`). Empty if the title asked for nothing
/// yet, or if a fast/slow split is requested on a processor whose cores are
/// all of one class.
fn left_of(sets: &[(u32, u8, u8)], asked: u64, fast: Option<bool>) -> Vec<u32> {
    if asked == 0 {
        return Vec::new();
    }
    let (Some(least), Some(best)) = (sets.iter().map(|c| c.1).min(), sets.iter().map(|c| c.1).max()) else {
        return Vec::new();
    };
    if fast.is_some() && least == best {
        return Vec::new();
    }
    sets.iter()
        .filter(|c| (c.2 >= 64 || (asked >> c.2) & 1 == 0) && fast.is_none_or(|f| (c.1 == best) == f))
        .map(|c| c.0)
        .collect()
}

/// The group-0 processor bits pinned by `(mask, group)`, or `None` when the
/// call targets another group, names no known group-0 processor, or covers
/// every known group-0 processor (which pins nothing).
fn effective_title_mask(sets: &[(u32, u8, u8)], mask: u64, group: u16) -> Option<u64> {
    if group != 0 || mask == 0 {
        return None;
    }
    let all = sets.iter().filter(|c| c.2 < 64).fold(0u64, |acc, c| acc | (1u64 << c.2));
    let effective = mask & all;
    (effective != 0 && effective != all).then_some(effective)
}

/// The logical processors of group 0 the title's threads were asked to run
/// on, all of its asks together ([`title_affinity`]).
static TITLE_ASKED: AtomicU64 = AtomicU64::new(0);

/// The title asked the system to run one of its threads on the logical
/// processors `mask` of group `group` (`leonida-hle`'s
/// `SetThreadGroupAffinity`). It does so as on the console: its busiest
/// workers on the first eight, the rest on the six after them; on an
/// i9-12900K those are the first seven fast cores with both threads of each.
pub fn title_affinity(mask: usize, group: u16) {
    if group != 0 || mask == 0 {
        return;
    }
    // Each mask once in the log, with how often it was asked so far.
    static SEEN: Mutex<Vec<(u64, u32)>> = Mutex::new(Vec::new());
    {
        let mut seen = SEEN.lock().unwrap_or_else(|e| e.into_inner());
        match seen.iter_mut().find(|s| s.0 == mask as u64) {
            Some(s) => s.1 += 1,
            None => {
                seen.push((mask as u64, 1));
                tracing::info!(mask = format_args!("{mask:#x}"), "the title pins a thread to these processors, the first time");
            }
        }
    }
    let (all_sets, fast) = sets();
    let Some(effective) = effective_title_mask(all_sets, mask as u64, group) else {
        return;
    };
    let was = TITLE_ASKED.fetch_or(effective, Relaxed);
    // Our threads that are kept off them move with what it asks, on hybrid processors only.
    if !fast.is_empty() && (was | effective) != was && (apart() || lane_apart()) {
        place_again();
    }
}

/// The switches of where our threads run, each read from its variable (on
/// unless `0`) or changed while the title runs (`crate::ab`).
const SWITCHES: [&str; 2] = ["LEONIDA_PLACE_APART", "LEONIDA_PLACE_LANE"];

/// Each of [`SWITCHES`] as changed while the title runs: 0 as its variable
/// says, 1 on, 2 off.
static CHANGED: [AtomicU8; 2] = [const { AtomicU8::new(0) }; 2];

/// Whether switch `i` of [`SWITCHES`] is on.
fn switched(i: usize) -> bool {
    match CHANGED[i].load(Relaxed) {
        1 => return true,
        2 => return false,
        _ => {}
    }
    // Named in `LEONIDA_AB`: off until its first phase on.
    #[cfg(feature = "diagnostics")]
    if let Some(on) = crate::ab::pick(SWITCHES[i]) {
        return on;
    }
    static ON: [OnceLock<bool>; 2] = [const { OnceLock::new() }; 2];
    *ON[i].get_or_init(|| std::env::var_os(SWITCHES[i]).is_none_or(|v| v != "0"))
}

/// Whether the threads that translate lists run on the slow cores the
/// title leaves (on by default; `LEONIDA_PLACE_APART=0`: where they were).
///
/// Why: the title's threads run where it asks, as on the console. On an
/// i9-12900K its eight busy workers are on logical processors 0-7 (four
/// fast cores with both their threads), its main thread and ten others on
/// 8-12, one on 13. Ours were on the fast cores too, and the slow ones
/// stood idle. The eight threads that translate lists are the most work of
/// ours, and nothing waits for one of them alone.
///
/// Not all of ours: kept off every processor the title asks for (the line's
/// thread on 14-15, every other one on the slow cores), the frame was 5 ms
/// longer (split-pl3-apart). The helpers the line's thread and the sender
/// wait for (the heaps' records, refreshes spread over threads) were
/// slower there.
fn apart() -> bool {
    switched(0)
}

/// Whether the thread that prepares submissions runs on the fast cores the
/// title leaves (on by default; `LEONIDA_PLACE_LANE=0`: on all fast ones):
/// 14-15 on an i9-12900K, where nothing of the title's runs.
fn lane_apart() -> bool {
    switched(1)
}

/// One of [`SWITCHES`] is `on` from now on (diagnostics, `crate::ab`): every
/// thread of ours is placed again, and what each kind of ours ran since the
/// last change is logged.
#[cfg(feature = "diagnostics")]
pub(crate) fn placing(switch: &str, on: bool) {
    let Some(i) = SWITCHES.iter().position(|s| *s == switch) else {
        return;
    };
    CHANGED[i].store(if on { 1 } else { 2 }, Relaxed);
    ran_line();
    if !sets().1.is_empty() {
        place_again();
    }
}

/// Diagnostics: the processor time of our threads that asked, by the name
/// they asked with, per second since the last line.
#[cfg(feature = "diagnostics")]
fn ran_line() {
    use windows_sys::Win32::Foundation::FILETIME;
    use windows_sys::Win32::System::Threading::GetThreadTimes;
    static LAST: Mutex<Option<(std::time::Instant, std::collections::HashMap<usize, u64>)>> = Mutex::new(None);
    let ours = OURS.lock().unwrap_or_else(|e| e.into_inner());
    let mut now = std::collections::HashMap::new();
    for (handle, _, _) in ours.iter() {
        // SAFETY: zeroed FILETIMEs are valid; a live thread handle of ours,
        // held open until `ThreadRegistration::drop` removes it under `OURS`.
        let mut t: [FILETIME; 4] = unsafe { std::mem::zeroed() };
        let [a, b, c, d] = &mut t;
        unsafe { GetThreadTimes(*handle as *mut std::ffi::c_void, a, b, c, d) };
        let of = |f: &FILETIME| u64::from(f.dwHighDateTime) << 32 | u64::from(f.dwLowDateTime);
        now.insert(*handle, of(&t[2]) + of(&t[3]));
    }
    let mut last = LAST.lock().unwrap_or_else(|e| e.into_inner());
    // (Two switches changed at once: one line.)
    if last.as_ref().is_some_and(|(since, _)| since.elapsed() < std::time::Duration::from_secs(1)) {
        return;
    }
    if let Some((since, before)) = last.as_ref() {
        let seconds = since.elapsed().as_secs_f64().max(1e-3);
        let mut by: std::collections::BTreeMap<&str, (f64, u32)> = std::collections::BTreeMap::new();
        for (handle, _, what) in ours.iter() {
            let ran = now[handle].saturating_sub(before.get(handle).copied().unwrap_or(now[handle]));
            let e = by.entry(what.as_str()).or_default();
            *e = (e.0 + ran as f64 / 1e4 / seconds, e.1 + 1);
        }
        let text: Vec<String> = by.iter().map(|(what, (ms, n))| format!("{what} x{n} {ms:.0}")).collect();
        tracing::info!("our threads' processor time, ms a second, over {seconds:.1} s: {}", text.join(", "));
    }
    *last = Some((std::time::Instant::now(), now));
}

/// `LEONIDA_FAST_CORES`: 0: no thread asks; 1: the threads a frame waits for
/// one by one; 2 (the default): the threads they share their work with too;
/// 3: those that work beside the rest as well (lists translated, descriptors
/// made): more threads by then than the fast cores run at a time, while the
/// slow ones stood idle.
fn stage() -> u32 {
    static STAGE: OnceLock<u32> = OnceLock::new();
    *STAGE.get_or_init(|| std::env::var("LEONIDA_FAST_CORES").ok().and_then(|v| v.trim().parse().ok()).unwrap_or(2))
}

/// Asks the system to run the calling thread, which a frame waits for, on
/// the fast cores only, where the processor has two kinds. That fork's submit
/// thread was found on a slow one in 1,529 looks of 24,021. (A selection of
/// processors is a preference the scheduler keeps unless nothing of it is
/// free.) The title's threads are not asked for.
pub(crate) fn keep_on_fast_cores(what: &str) {
    ask(what, 1);
}

/// [`keep_on_fast_cores`] for one of the threads a submission's preparing or
/// a landing shares its work with.
pub(crate) fn keep_helper_on_fast_cores(what: &str) {
    ask(what, 2);
}

/// [`keep_on_fast_cores`] for one of many threads whose work is ahead of what
/// a frame waits for.
pub(crate) fn keep_many_on_fast_cores(what: &str) {
    ask(what, 3);
}

/// The sets, and the fast ones among them.
fn sets() -> &'static (Vec<(u32, u8, u8)>, Vec<u32>) {
    static SETS: OnceLock<(Vec<(u32, u8, u8)>, Vec<u32>)> = OnceLock::new();
    SETS.get_or_init(|| {
        let sets = cpu_sets();
        let fast = fast_of(&sets);
        (sets, fast)
    })
}

/// Pure placement rule for a thread named `what` at stage `from`.
///
/// Returns `(selected, exhausted_class)`:
/// - `selected`: `Some((ids, kind))` when the thread should be placed on `ids`,
///   or `None` when the OS scheduler should choose freely (including all
///   homogeneous non-hybrid processors, where `fast` is empty).
/// - `exhausted_class`: `Some((fast_ones, kind))` when `what` wanted unpinned
///   fast or slow cores on a hybrid processor, the title pinned threads, and
///   none of that class remained unpinned.
#[allow(clippy::too_many_arguments)]
fn choose_sets(
    sets: &[(u32, u8, u8)],
    fast: &[u32],
    asked: u64,
    stage: u32,
    apart: bool,
    lane_apart: bool,
    what: &str,
    from: u32,
) -> (Option<(Vec<u32>, &'static str)>, Option<(bool, &'static str)>) {
    if fast.is_empty() {
        return (None, None);
    }
    let kept = match what {
        "leonida-list" if apart => Some((false, "the slow cores the title leaves (LEONIDA_PLACE_APART)")),
        "leonida-submit" if lane_apart => Some((true, "the fast cores the title leaves (LEONIDA_PLACE_LANE)")),
        _ => None,
    };
    let mut exhausted = None;
    if let Some((fast_ones, kind)) = kept
        && asked != 0
    {
        let left = left_of(sets, asked, Some(fast_ones));
        if !left.is_empty() {
            return (Some((left, kind)), None);
        }
        exhausted = Some((fast_ones, kind));
    }
    let fallback = (stage >= from).then(|| (fast.to_vec(), "the fast cores (LEONIDA_FAST_CORES=0: not)"));
    (fallback, exhausted)
}

/// Where a thread that asked as `what` at stage `from` is to run, and what
/// that is called; `None`: wherever the system likes.
fn chosen(what: &str, from: u32) -> Option<(Vec<u32>, &'static str)> {
    let (sets, fast) = sets();
    let asked = TITLE_ASKED.load(Relaxed);
    let (picked, exhausted) = choose_sets(sets, fast, asked, stage(), apart(), lane_apart(), what, from);
    if let Some((fast_ones, kind)) = exhausted {
        static SAID: [std::sync::Once; 2] = [const { std::sync::Once::new() }; 2];
        SAID[usize::from(fast_ones)].call_once(|| {
            let cores = if fast_ones { "fast" } else { "slow" };
            tracing::info!(what, title = format_args!("{asked:#x}"), "the title leaves no {cores} cores: the thread stays where it was ({kind})");
        });
    }
    picked
}

/// Our live threads that asked: a handle each, the stage and the name they
/// asked with.
static OURS: Mutex<Vec<(usize, u32, String)>> = Mutex::new(Vec::new());

/// Removes a short-lived thread's entry from [`OURS`] before closing its
/// handle, so neither handles leak nor [`place_again`] / [`ran_line`] ever
/// touch a closed handle.
struct ThreadRegistration(usize);

impl Drop for ThreadRegistration {
    fn drop(&mut self) {
        use windows_sys::Win32::Foundation::CloseHandle;
        {
            let mut ours = OURS.lock().unwrap_or_else(|e| e.into_inner());
            ours.retain(|(handle, _, _)| *handle != self.0);
        }
        // SAFETY: opened in `ask` with `OpenThread` and removed from `OURS`
        // under its lock before closing.
        unsafe { CloseHandle(self.0 as *mut std::ffi::c_void) };
    }
}

thread_local! {
    static REGISTERED: RefCell<Option<ThreadRegistration>> = const { RefCell::new(None) };
}

fn ask(what: &str, from: u32) {
    use windows_sys::Win32::System::Threading::{
        GetCurrentThreadId, OpenThread, THREAD_QUERY_LIMITED_INFORMATION, THREAD_SET_LIMITED_INFORMATION,
    };
    let _ = REGISTERED.try_with(|slot| {
        let mut slot = slot.borrow_mut();
        if let Some(existing) = slot.as_ref() {
            let handle = existing.0;
            let mut ours = OURS.lock().unwrap_or_else(|e| e.into_inner());
            if let Some(entry) = ours.iter_mut().find(|(h, _, _)| *h == handle) {
                entry.1 = from;
                entry.2.clear();
                entry.2.push_str(what);
            }
            match chosen(what, from) {
                Some((sets, kind)) => select(handle, what, &sets, kind, true),
                None if !self::sets().1.is_empty() => select(handle, what, &[], "the processors the system chooses", false),
                None => {}
            }
            return;
        }
        // SAFETY: this thread of our own process; kept open until thread exit
        // via `ThreadRegistration`.
        let handle = unsafe {
            OpenThread(
                THREAD_SET_LIMITED_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION,
                0,
                GetCurrentThreadId(),
            )
        };
        if handle.is_null() {
            return;
        }
        let raw = handle as usize;
        {
            let mut ours = OURS.lock().unwrap_or_else(|e| e.into_inner());
            ours.push((raw, from, what.to_string()));
            // (Under the lock: a placing again does not come between. Nothing to
            // choose: as before, nothing asked.)
            if let Some((sets, kind)) = chosen(what, from) {
                select(raw, what, &sets, kind, true);
            }
        }
        *slot = Some(ThreadRegistration(raw));
    });
}

/// Asks for `sets` (none: the system's choice) for the thread `handle`.
fn select(handle: usize, what: &str, sets: &[u32], kind: &str, say: bool) {
    use windows_sys::Win32::System::Threading::SetThreadSelectedCpuSets;
    let (ids, count) = if sets.is_empty() { (std::ptr::null(), 0) } else { (sets.as_ptr(), sets.len() as u32) };
    // SAFETY: a thread of ours, opened for this; a list of the system's own
    // set ids, or none.
    let ok = unsafe { SetThreadSelectedCpuSets(handle as *mut std::ffi::c_void, ids, count) };
    if ok == 0 && say {
        tracing::warn!(what, error = %std::io::Error::last_os_error(), "the thread could not be kept on {kind}");
    } else if ok != 0 && say {
        let title = TITLE_ASKED.load(Relaxed);
        tracing::info!(what, sets = sets.len(), of = self::sets().0.len(), title = format_args!("{title:#x}"), "the thread is kept on {kind}");
    }
}

/// Every thread of ours that asked is placed again, as [`chosen`] says now.
fn place_again() {
    let ours = OURS.lock().unwrap_or_else(|e| e.into_inner());
    for (handle, from, what) in ours.iter() {
        match chosen(what, *from) {
            Some((sets, kind)) => select(*handle, what, &sets, kind, false),
            None => select(*handle, what, &[], "the processors the system chooses", false),
        }
    }
    tracing::info!(
        threads = ours.len(),
        apart = apart(),
        lane = lane_apart(),
        title = format_args!("{:#x}", TITLE_ASKED.load(Relaxed)),
        "our threads are placed again (LEONIDA_PLACE_*)"
    );
}

/// Whether the threads the submitting thread waits for run at its priority
/// (`LEONIDA_HELPER_PRIORITY`, on unless `0`; `LEONIDA_AB` can flip it while
/// the title runs): above normal, ahead of the title's threads and of the
/// threads that translate lists. They are the threads that spread its work
/// (`shadow::wide`, the pools of `land`), the one that makes the heaps'
/// records and the one that passes over the heap's views beside the lists.
///
/// Ported from trustno1-coder/VIctory-NVIDIA 80c70b7 (Indy2l, pull request
/// #22, after readmet3xt-lab's 002cf4a; GPL-3.0, as this). On a processor
/// with six cores (i5-12400F, 12 threads; this PC's 7600X has six too) the
/// title's threads and eight translating lists kept every core busy, and the
/// submitting thread (above normal since `queue::behind`) stood waiting for
/// helpers at normal priority that were not running. Measured there with the
/// A/B switch in one run (run10, Series S at 540p, RTX 3060, medians of
/// 5-second windows in the world, off / on): 8.6 / 9.6 fps, refreshes 23.1 /
/// 17.6 ms a frame, waiting for the heaps' records 6.2 / 2.5 ms, the card
/// idle behind the submitting thread 24.7 / 13.2 ms. Lowering the translating
/// threads below normal as well made each list call about 5 % slower, with no
/// clear gain of its own, so they are left as they are. Nothing the card is
/// given changes: only when the work runs.
fn helpers_raised() -> bool {
    crate::toggled!("LEONIDA_HELPER_PRIORITY")
}

/// A helper's priority ([`helpers_raised`]). [`Self::keep`] is called before
/// each piece of work; it asks the system only when the switch has changed.
pub(crate) struct Priority {
    applied: Option<bool>,
}

impl Priority {
    pub(crate) fn new() -> Self {
        let mut priority = Self { applied: None };
        priority.keep();
        priority
    }

    pub(crate) fn keep(&mut self) {
        use windows_sys::Win32::System::Threading::{
            GetCurrentThread, SetThreadPriority, THREAD_PRIORITY_ABOVE_NORMAL, THREAD_PRIORITY_NORMAL,
        };
        let on = helpers_raised();
        if self.applied == Some(on) {
            return;
        }
        let priority = if on { THREAD_PRIORITY_ABOVE_NORMAL } else { THREAD_PRIORITY_NORMAL };
        // SAFETY: our own thread.
        unsafe { SetThreadPriority(GetCurrentThread(), priority) };
        self.applied = Some(on);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_fast_cores_are_those_of_the_best_class_if_there_is_a_worse_one() {
        assert_eq!(fast_of(&[(256, 1, 0), (257, 1, 1), (258, 0, 2)]), [256, 257]);
        // Three-tier hybrid topology (e.g. P + E + LP-E): only the top class is fast.
        assert_eq!(fast_of(&[(256, 2, 0), (257, 1, 1), (258, 0, 2)]), [256]);
        assert!(fast_of(&[(256, 0, 0), (257, 0, 1)]).is_empty());
        assert!(fast_of(&[(256, 1, 0), (257, 1, 1)]).is_empty());
        assert!(fast_of(&[]).is_empty());
        assert!(!cpu_sets().is_empty());
        std::thread::spawn(|| {
            keep_on_fast_cores("test-cleanup-1");
            keep_helper_on_fast_cores("test-cleanup-2");
            let ours = OURS.lock().unwrap_or_else(|e| e.into_inner());
            assert_eq!(
                ours.iter().filter(|(_, _, w)| w.starts_with("test-cleanup-")).count(),
                1,
                "repeated calls on the same thread update one entry"
            );
        })
        .join()
        .unwrap();
        let ours = OURS.lock().unwrap_or_else(|e| e.into_inner());
        assert!(
            !ours.iter().any(|(_, _, w)| w.starts_with("test-cleanup-")),
            "exited threads clean up their registered handle"
        );
    }

    /// The title on the first two fast processors of three: of the rest,
    /// the fast one, the slow one, or both.
    #[test]
    fn what_the_title_leaves_is_found_by_kind() {
        let four = [(256, 1, 0), (257, 1, 1), (258, 1, 2), (259, 0, 3)];
        assert_eq!(left_of(&four, 0b0011, Some(true)), [258]);
        assert_eq!(left_of(&four, 0b0011, Some(false)), [259]);
        assert_eq!(left_of(&four, 0b0011, None), [258, 259]);
        assert!(left_of(&four, 0, None).is_empty(), "the title asked for nothing yet");
        assert!(left_of(&four, 0b1111, None).is_empty(), "it left nothing");
        assert!(left_of(&four, 0b0111, Some(true)).is_empty(), "no fast one left");
        // Logical processor indices >= 64 never overflow shifts.
        let high = [(300, 1, 0), (301, 0, 64), (302, 1, 255)];
        assert_eq!(left_of(&high, 0b1, Some(false)), [301]);
        assert_eq!(left_of(&high, 0b1, Some(true)), [302]);
    }

    /// Homogeneous multi-CCD processors (such as a 16-core / 32-thread AMD
    /// Ryzen 9 5950X across two CCDs, or an 8-core / 16-thread Ryzen 7 9800X3D)
    /// report one uniform `EfficiencyClass` across every logical processor and
    /// must never be partitioned or degraded when the title pins threads.
    #[test]
    fn homogeneous_multi_ccd_processors_are_never_partitioned() {
        for uniform_class in [0u8, 1u8] {
            let ryzen_5950x: Vec<(u32, u8, u8)> =
                (0..32u32).map(|i| (256 + i, uniform_class, i as u8)).collect();
            let fast = fast_of(&ryzen_5950x);
            assert!(fast.is_empty());
            // Neither fast-only nor slow-only subsets exist when all cores share one class.
            assert!(left_of(&ryzen_5950x, 0x3fff, Some(true)).is_empty());
            assert!(left_of(&ryzen_5950x, 0x3fff, Some(false)).is_empty());
            assert_eq!(left_of(&ryzen_5950x, 0x3fff, None).len(), 18);

            for (what, from) in [("leonida-submit", 1), ("leonida-wide", 2), ("leonida-list", 3)] {
                let (picked, exhausted) =
                    choose_sets(&ryzen_5950x, &fast, 0x3fff, 2, true, true, what, from);
                assert_eq!((picked, exhausted), (None, None), "{what} class={uniform_class}");
            }
        }
    }

    /// Hybrid P/E placement (`i9-12900K`: 8 P-cores / 16 threads `0..15` +
    /// 8 E-cores `16..23`, and `i5-13400F`: 6 P-cores / 12 threads `0..11` +
    /// 4 E-cores `12..15`) places `leonida-list` on unpinned slow cores and
    /// `leonida-submit` on unpinned fast cores, falling back cleanly when a
    /// class has no unpinned cores left or when the switches are off.
    #[test]
    fn hybrid_placement_separates_list_and_submit_from_title_pins() {
        let i9_12900k: Vec<(u32, u8, u8)> = (0..24u32)
            .map(|i| (256 + i, u8::from(i < 16), i as u8))
            .collect();
        let fast = fast_of(&i9_12900k);
        assert_eq!(fast.len(), 16);

        // Before the title pins anything (`asked == 0`), submit and helpers use all P-cores;
        // list threads (`from = 3`, `stage = 2`) stay unpinned.
        assert_eq!(
            choose_sets(&i9_12900k, &fast, 0, 2, true, true, "leonida-submit", 1).0.unwrap().0,
            fast
        );
        assert!(choose_sets(&i9_12900k, &fast, 0, 2, true, true, "leonida-list", 3).0.is_none());

        // Console title pins logical processors 0..13 (`0x3fff`), leaving P-cores 14..15
        // and all eight E-cores 16..23.
        let (submit, ex_s) = choose_sets(&i9_12900k, &fast, 0x3fff, 2, true, true, "leonida-submit", 1);
        assert_eq!((submit.unwrap().0, ex_s), (vec![270, 271], None));

        let (lists, ex_l) = choose_sets(&i9_12900k, &fast, 0x3fff, 2, true, true, "leonida-list", 3);
        assert_eq!((lists.unwrap().0, ex_l), ((272..280).collect::<Vec<_>>(), None));

        let (wide, _) = choose_sets(&i9_12900k, &fast, 0x3fff, 2, true, true, "leonida-wide", 2);
        assert_eq!(wide.unwrap().0, fast);

        // Switches off restore the baseline placement.
        assert_eq!(
            choose_sets(&i9_12900k, &fast, 0x3fff, 2, false, false, "leonida-submit", 1).0.unwrap().0,
            fast
        );
        assert!(choose_sets(&i9_12900k, &fast, 0x3fff, 2, false, false, "leonida-list", 3).0.is_none());

        // 6P/12T + 4E/4T (`i5-13400F`): `0x3fff` covers every P-core (`0..11`), so
        // `leonida-submit` reports fast-core exhaustion and falls back to all P-cores.
        let i5_13400f: Vec<(u32, u8, u8)> = (0..16u32)
            .map(|i| (512 + i, u8::from(i < 12), i as u8))
            .collect();
        let i5_fast = fast_of(&i5_13400f);
        let (sub_fallback, exhausted) =
            choose_sets(&i5_13400f, &i5_fast, 0x3fff, 2, true, true, "leonida-submit", 1);
        assert_eq!(sub_fallback.unwrap().0, i5_fast);
        assert!(matches!(exhausted, Some((true, _))));
    }

    #[test]
    fn title_affinity_masks_ignore_other_groups_empty_and_all_processors() {
        let eight: Vec<(u32, u8, u8)> = (0..8u32).map(|i| (100 + i, u8::from(i < 4), i as u8)).collect();
        assert_eq!(effective_title_mask(&eight, 0x0f, 1), None, "non-zero processor group");
        assert_eq!(effective_title_mask(&eight, 0, 0), None, "empty mask");
        assert_eq!(effective_title_mask(&eight, 0xff, 0), None, "all group-0 processors");
        assert_eq!(effective_title_mask(&eight, u64::MAX, 0), None, "full mask pins nothing");
        assert_eq!(effective_title_mask(&eight, 0x0f00, 0), None, "bits outside known sets");
        assert_eq!(effective_title_mask(&eight, 0x0f03, 0), Some(0x03), "clamped to known group-0 sets");
        assert_eq!(effective_title_mask(&[], 0x03, 0), None, "no CPU sets reported");
    }

    #[test]
    fn selected_cpu_sets_apply_and_clear_on_a_live_thread() {
        use windows_sys::Win32::System::Threading::{
            GetCurrentThreadId, GetThreadSelectedCpuSets, OpenThread, THREAD_QUERY_LIMITED_INFORMATION,
            THREAD_SET_LIMITED_INFORMATION,
        };
        let host_sets = cpu_sets();
        let first_id = host_sets[0].0;
        std::thread::spawn(move || {
            // SAFETY: opens and closes our own test thread handle.
            let handle = unsafe {
                OpenThread(
                    THREAD_SET_LIMITED_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION,
                    0,
                    GetCurrentThreadId(),
                )
            };
            assert!(!handle.is_null());
            select(handle as usize, "test-select", &[first_id], "one set", false);
            let mut got = [0u32; 4];
            let mut required = 0u32;
            // SAFETY: queries `handle`'s selected CPU sets into `got`.
            let ok = unsafe { GetThreadSelectedCpuSets(handle, got.as_mut_ptr(), got.len() as u32, &mut required) };
            assert_ne!(ok, 0);
            assert_eq!((required, got[0]), (1, first_id));

            select(handle as usize, "test-select", &[], "system default", false);
            // SAFETY: as above.
            let ok = unsafe { GetThreadSelectedCpuSets(handle, got.as_mut_ptr(), got.len() as u32, &mut required) };
            assert_ne!(ok, 0);
            assert_eq!(required, 0);
            // SAFETY: closes the test handle.
            unsafe { windows_sys::Win32::Foundation::CloseHandle(handle) };
        })
        .join()
        .unwrap();
    }

    /// A helper's priority is asked for once, and again only when the switch changes.
    #[test]
    fn a_helper_priority_is_applied_once() {
        std::thread::spawn(|| {
            let mut priority = Priority::new();
            assert_eq!(priority.applied, Some(helpers_raised()));
            priority.keep();
            assert_eq!(priority.applied, Some(helpers_raised()));
        })
        .join()
        .unwrap();
    }

    #[cfg(feature = "diagnostics")]
    #[test]
    fn live_placing_flips_switches_and_ignores_other_names() {
        std::thread::spawn(|| {
            keep_many_on_fast_cores("leonida-list");
            placing("LEONIDA_PLACE_APART", false);
            assert!(!apart());
            placing("LEONIDA_PLACE_APART", true);
            assert!(apart());
            placing("LEONIDA_PLACE_LANE", false);
            assert!(!lane_apart());
            placing("LEONIDA_PLACE_LANE", true);
            assert!(lane_apart());
            placing("LEONIDA_UNRELATED", false);
            assert!(apart() && lane_apart());
            CHANGED[0].store(0, Relaxed);
            CHANGED[1].store(0, Relaxed);
        })
        .join()
        .unwrap();
    }
}
