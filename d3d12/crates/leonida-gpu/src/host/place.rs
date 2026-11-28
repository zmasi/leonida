//! The title's buffer copies, done on the card at their place in the list
//! (`LEONIDA_COPIES_IN_PLACE`, on by default; `0`: all at submission, on the
//! CPU, as before).
//!
//! A buffer copy was done in title memory when its list was submitted, before
//! any draw of the submission ran, and the host copies refreshed after it.
//! That is another order than the list's: draws ahead of the copy read what
//! it put; of two copies to the same bytes with a draw behind each, both
//! draws read the second; a source that a dispatch of the same submission
//! writes first was read as it was before it; and a destination the
//! submissions in the air still read changed under them (where its host copy
//! has one side only, `crate::shadow::card_all`).
//!
//! Now the list copies, in the destination's host copy: out of the source's
//! host copy where shaders write that one, else out of upload memory of the
//! list that takes the source's bytes when the list is submitted (a source
//! shaders never wrote is the title's CPU's, or a copy's put there earlier in
//! the submission). What is written there is a shader's write from then on:
//! the bytes are declared, and what the card made of them is put into title
//! memory before the title is told of the submission.
//!
//! Title memory takes the copy at submission as well, as it did: fills of
//! upload memory, textures and host copies that lie over the same bytes read
//! it there. The reference of the destination's host copy takes the same
//! bytes ([`Shadow::placed_copy`]), so what the landing finds is what the card
//! made of them afterwards (a counter copied to zero and counted up; a source
//! that a shader wrote in between), and nothing is sent up over it. Where
//! flights in the air may write the source or the destination this is left to
//! the landing: what they wrote is not in title memory yet, and what they
//! will put there must not stand over this copy's bytes. If a step of the
//! submission reads the destination on the CPU, it waits for them instead
//! ([`Placed::waits`]).
//!
//! Copies to memory that has no host copy yet are done as before: nothing on
//! the card has read or written it. So are those of more than 8 MiB out of
//! memory shaders do not write (streaming).

use super::*;
use crate::shadow::Shadow;

pub(super) fn on() -> bool {
    static ON: OnceLock<bool> = OnceLock::new();
    *ON.get_or_init(|| std::env::var_os("LEONIDA_COPIES_IN_PLACE").is_none_or(|v| v != "0"))
}

/// The most bytes copied out of memory shaders do not write.
const MOST: usize = 8 << 20;

/// Since the last frame-rate line: copies at their place, their bytes, those
/// out of a host copy, and texture copies out of one.
pub(super) static STATS: [std::sync::atomic::AtomicU64; 4] = [const { std::sync::atomic::AtomicU64::new(0) }; 4];

/// What the CPU does at submission for a copy at its place in a list.
pub(super) struct Placed {
    pub(super) dst: usize,
    pub(super) src: usize,
    pub(super) len: usize,
    /// The destination's host copy.
    pub(super) shadow: std::sync::Arc<Shadow>,
    /// Upload memory the list copies from, to be given the source's bytes.
    from: Option<usize>,
}

impl Placed {
    /// Whether the copy's bytes are the title's own although flights in the air (`written`) declare them
    /// (`super::placed_is_titles`): the destination by what has landed there, and the source either not
    /// declared or the title's by the rule for fills (`super::fill_is_titles`).
    pub(super) fn titles(&self, written: &[(usize, usize)]) -> bool {
        super::placed_is_titles(self.dst, self.len, touches(written, self.dst, self.dst + self.len))
            && (!touches(written, self.src, self.src + self.len) || super::fill_is_titles(self.src, self.len))
    }

    /// Diagnostics: why [`Placed::titles`] did not take the copy for the title's own, for the wait's name.
    #[cfg(feature = "diagnostics")]
    pub(super) fn why_waits(&self, written: &[(usize, usize)]) -> &'static str {
        let source = touches(written, self.src, self.src + self.len);
        let declared = touches(written, self.dst, self.dst + self.len);
        let dst = super::placed_is_titles(self.dst, self.len, declared);
        let src = !source || super::fill_is_titles(self.src, self.len);
        match (dst, src, declared) {
            (false, false, _) => "(a shader's write went into its destination lately, and into its source)",
            (false, true, true) => "(a shader's write went into its destination, which flights declare, lately)",
            (false, true, false) => "(a shader's write went into its destination, which no flight declares, lately)",
            (true, false, _) => "(a shader's write went into its source lately, and the title did not write that since)",
            (true, true, _) => "(taken for the title's own by now)",
        }
    }

    /// Whether flights that write `written` are waited for.
    pub(super) fn waits(&self, written: &[(usize, usize)]) -> Waits {
        if self.titles(written) {
            return Waits::No;
        }
        let source = touches(written, self.src, self.src + self.len);
        if source && self.from.is_some() {
            return Waits::Yes;
        }
        match source || touches(written, self.dst, self.dst + self.len) {
            true => Waits::ForReaders,
            false => Waits::No,
        }
    }

    /// At submission, in the order of the steps: the list's upload memory
    /// takes the source's bytes and, if `free` (no flight in the air may write
    /// source or destination) or `titles` ([`Placed::titles`]), title memory at
    /// the destination does. Gives the bytes if it took them and the
    /// destination's reference is to take them too (`free` alone: with flights
    /// in the air the reference is left to this submission's landing, so that
    /// a flight ahead of it does not find the bytes from before the copy to
    /// differ from it and put them back).
    pub(super) fn prepare(&self, free: bool, titles: bool) -> Option<Vec<u8>> {
        if !free && !titles && self.from.is_none() {
            return None;
        }
        let mut bytes = vec![0u8; self.len];
        // (What is not there reads as zeros, as any copy's source.)
        copy_mapped(bytes.as_mut_ptr(), self.src as *const u8, self.len, false);
        if let Some(to) = self.from {
            // SAFETY: the list's own upload memory of that many bytes.
            unsafe { std::ptr::copy_nonoverlapping(bytes.as_ptr(), to as *mut u8, self.len) };
        }
        if !free && !titles {
            return None;
        }
        copy_mapped(self.dst as *mut u8, bytes.as_ptr(), self.len, true);
        crate::watch::wrote(self.dst, self.len);
        if !free {
            return None;
        }
        // Title memory that is not all there was left alone.
        let mut there = vec![0u8; self.len];
        (read_into(self.dst as *const u8, &mut there) && there == bytes).then_some(bytes)
    }
}

/// Whether a copy waits for flights before its submission is prepared: not;
/// yes, as it takes a source on the CPU that they may write; or only if
/// another step of the submission reads its destination on the CPU.
pub(super) enum Waits {
    No,
    ForReaders,
    Yes,
}

/// Whether only the steps behind a copy at its place in a list count as its readers (on unless
/// `LEONIDA_PLACED_READERS=0`: every step of the submission that reads its destination, wherever it stands, as
/// before; `LEONIDA_AB` can name it).
///
/// A copy whose bytes wait for flights ([`Waits::ForReaders`]) is left to the card: title memory does not hold
/// what it writes until its submission has landed, so a step of the submission that reads its destination on the
/// CPU stands until the flights have landed. That was asked of every such step. One ahead of the copy wants the
/// bytes from before it, which title memory holds, and the steps are done in their order: it has nothing to
/// wait for on the copy's account. (What flights declare of its own bytes stops it as before: `read_blocked`.)
///
/// From avatarmanz/VIctory-7900XTX 11511b81b ("... order-aware readers of copies at their place ...", author
/// recorded as "local", 2026-11-25; GPL-3.0-or-later), where the switch is `VICTORY_PLACED_READERS` and the rule
/// has been the default since; Belobug/VIctory-Belobug-AMD and TaynanGT/Vice-Tay have it from there. Not as in
/// the source: the copies that wait are told apart by where they stand in their lists, not asked a second time
/// whether they wait, and a diagnostics build counts the submissions the rule spared ([`AHEAD`]).
pub(super) fn readers_in_order() -> bool {
    crate::toggled!("LEONIDA_PLACED_READERS")
}

/// Diagnostics, since the last frame-rate line: submissions that were not held for flights because every step
/// that reads a waiting copy's destination on the CPU stands ahead of the copy ([`readers_in_order`]).
#[cfg(feature = "diagnostics")]
pub(super) static AHEAD: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);

/// The destinations of the copies at their place that wait for their readers ([`Waits::ForReaders`]), while
/// the steps of a submission are walked in their order.
pub(super) struct Waiting {
    /// Those that count for the step the walk is at, in order, joined.
    ranges: Vec<(usize, usize)>,
    in_order: bool,
}

impl Waiting {
    /// `all`: every such copy's destination. In order ([`readers_in_order`]) none counts before the walk has
    /// come to its copy; else all do from the first step on.
    pub(super) fn new(all: impl IntoIterator<Item = (usize, usize)>, in_order: bool) -> Self {
        let mut ranges = Vec::new();
        if !in_order {
            join_ranges(&mut ranges, all);
        }
        Self { ranges, in_order }
    }

    /// The walk's next step: `reads` are the bytes it reads on the CPU (address, length), `copy` what it writes
    /// if it is one of the waiting copies. Whether it reads what a waiting copy writes.
    pub(super) fn step(&mut self, reads: Option<(usize, usize)>, copy: Option<(usize, usize)>) -> bool {
        let read = reads.is_some_and(|(at, len)| touches(&self.ranges, at, at.saturating_add(len)));
        // (After it was looked at as a reader: a copy does not read what it writes.)
        if self.in_order
            && let Some(dst) = copy
        {
            join_ranges(&mut self.ranges, [dst]);
        }
        read
    }
}

/// The first step of `runs` that reads on the CPU what one of `placed` writes: what kind of step, and the bytes
/// it reads (address, length). `placed`: the copies at their place that wait for their readers, each by its
/// address in its list's steps and its destination (from, to). `in_order`: only steps behind the copy count
/// ([`readers_in_order`]). `taken`: copies and fills that the CPU leaves out (their list, their place in it:
/// `card_steps`).
pub(super) fn reader(
    runs: &[&HostList],
    placed: &[(usize, usize, usize)],
    in_order: bool,
    taken: impl Fn(&HostList, usize) -> bool,
) -> Option<(&'static str, usize, usize)> {
    let mut waiting = Waiting::new(placed.iter().map(|p| (p.1, p.2)), in_order);
    runs.iter().find_map(|list| {
        list.deferred().copies.iter().enumerate().find_map(|(i, step)| {
            let read = match step {
                &Step::Copy(_, src, len) if !taken(list, i) => {
                    Some(("a buffer copy's source, written by a copy at its place in a list", src, len))
                }
                &Step::Fill(_, src, len, _) if !taken(list, i) => {
                    Some(("a fill of upload memory from what a copy at its place in a list writes", src, len))
                }
                Step::Texture(staging) => {
                    let (at, len) = staging.memory();
                    Some(("a texture over what a copy at its place in a list writes", at, len))
                }
                &Step::Plane((at, len), _) => Some(("a movie plane over what a copy at its place in a list writes", at, len)),
                Step::Host(_, (from, to)) => {
                    Some(("a step of the host's that reads what a copy at its place in a list writes", *from, *to - *from))
                }
                Step::Placed(p) if p.from.is_some() => {
                    Some(("a copy at its place whose source another one at its place writes", p.src, p.len))
                }
                _ => None,
            };
            let copy = match step {
                Step::Placed(p) if placed.iter().any(|q| q.0 == std::ptr::from_ref::<Placed>(p) as usize) => {
                    Some((p.dst, p.dst + p.len))
                }
                _ => None,
            };
            let reads = waiting.step(read.map(|(_, at, len)| (at, len)), copy);
            read.filter(|_| reads)
        })
    })
}

/// Everything recorded so far is done, and seen, before what follows.
///
/// # Safety
/// An open list.
unsafe fn wait_for_all(host: &Host, list: &ID3D12GraphicsCommandList) {
    static ENHANCED: OnceLock<bool> = OnceLock::new();
    let enhanced = *ENHANCED.get_or_init(|| {
        let mut o = D3D12_FEATURE_DATA_D3D12_OPTIONS12::default();
        // SAFETY: a feature structure of its size.
        let known = unsafe {
            host.device.CheckFeatureSupport(
                D3D12_FEATURE_D3D12_OPTIONS12,
                (&raw mut o).cast(),
                size_of::<D3D12_FEATURE_DATA_D3D12_OPTIONS12>() as u32,
            )
        };
        known.is_ok() && o.EnhancedBarriersSupported.as_bool()
    });
    // SAFETY: the caller's.
    unsafe {
        if enhanced && let Ok(list) = list.cast::<ID3D12GraphicsCommandList7>() {
            let all = [D3D12_GLOBAL_BARRIER {
                SyncBefore: D3D12_BARRIER_SYNC_ALL,
                SyncAfter: D3D12_BARRIER_SYNC_ALL,
                AccessBefore: D3D12_BARRIER_ACCESS_COMMON,
                AccessAfter: D3D12_BARRIER_ACCESS_COMMON,
            }];
            list.Barrier(&[D3D12_BARRIER_GROUP {
                Type: D3D12_BARRIER_TYPE_GLOBAL,
                NumBarriers: 1,
                Anonymous: D3D12_BARRIER_GROUP_0 {
                    pGlobalBarriers: all.as_ptr(),
                },
            }]);
            return;
        }
        let alias = D3D12_RESOURCE_BARRIER {
            Type: D3D12_RESOURCE_BARRIER_TYPE_ALIASING,
            Flags: D3D12_RESOURCE_BARRIER_FLAG_NONE,
            Anonymous: D3D12_RESOURCE_BARRIER_0 {
                Aliasing: std::mem::ManuallyDrop::new(D3D12_RESOURCE_ALIASING_BARRIER {
                    pResourceBefore: std::mem::ManuallyDrop::new(None),
                    pResourceAfter: std::mem::ManuallyDrop::new(None),
                }),
            },
        };
        let uav = D3D12_RESOURCE_BARRIER {
            Type: D3D12_RESOURCE_BARRIER_TYPE_UAV,
            Flags: D3D12_RESOURCE_BARRIER_FLAG_NONE,
            Anonymous: D3D12_RESOURCE_BARRIER_0 {
                UAV: std::mem::ManuallyDrop::new(D3D12_RESOURCE_UAV_BARRIER {
                    pResource: std::mem::ManuallyDrop::new(None),
                }),
            },
        };
        list.ResourceBarrier(&[alias, uav]);
    }
}

/// The host copy of `[src, src + len)` if shaders write it: what it holds at
/// a place in a list is not what title memory holds at submission.
fn shaders_own(src: usize, len: usize) -> Option<std::sync::Arc<Shadow>> {
    crate::shadow::peek(src as u64, len as u64).filter(|s| s.written() && !s.in_place())
}

impl HostList {
    /// Copies `len` bytes of title memory at `src` to `dst`, at this place of
    /// the list (see the file). `false` if it is to be done at submission.
    pub(crate) fn place_copy(&self, dst: usize, src: usize, len: usize) -> bool {
        if !on() {
            return false;
        }
        let Some(host) = host() else {
            return false;
        };
        let Some(to) = crate::shadow::peek(dst as u64, len as u64).filter(|s| !s.in_place()) else {
            return false;
        };
        let own = shaders_own(src, len);
        let scratch = match &own {
            Some(_) => None,
            None if len <= MOST => match self.scratch_at(len as u64) {
                Some(scratch) => Some(scratch),
                None => return false,
            },
            None => return false,
        };
        let at = to.offset(dst as u64);
        // What the title's CPU wrote around and under it so far goes first.
        to.refresh(self, dst as u64, len as u64);
        // SAFETY (both): an open list; live buffers (the list holds the
        // copies and its scratch memory), ranges inside them.
        unsafe { wait_for_all(host, &self.list) };
        match (&own, &scratch) {
            (Some(from), _) => {
                from.refresh(self, src as u64, len as u64);
                let offset = from.offset(src as u64);
                if from.buffer.as_raw() == to.buffer.as_raw() {
                    let Some(between) = self.between(host, len as u64) else {
                        return false;
                    };
                    unsafe {
                        self.list.CopyBufferRegion(&between, 0, &from.buffer, offset, len as u64);
                        self.list.ResourceBarrier(&[transition(&between, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE)]);
                        wait_for_all(host, &self.list);
                        self.list.CopyBufferRegion(&to.buffer, at, &between, 0, len as u64);
                    }
                } else {
                    unsafe { self.list.CopyBufferRegion(&to.buffer, at, &from.buffer, offset, len as u64) };
                }
                STATS[2].fetch_add(1, Ordering::Relaxed);
            }
            (None, Some((from, offset, _))) => unsafe {
                self.list.CopyBufferRegion(&to.buffer, at, from, *offset, len as u64);
            },
            (None, None) => return false,
        }
        unsafe { wait_for_all(host, &self.list) };
        STATS[0].fetch_add(1, Ordering::Relaxed);
        STATS[1].fetch_add(len as u64, Ordering::Relaxed);
        to.gpu_writes(self, dst as u64, len as u64);
        let mut deferred = self.deferred();
        deferred.copies.push(Step::Placed(Placed {
            dst,
            src,
            len,
            shadow: to,
            from: scratch.map(|s| s.2 as usize),
        }));
        deferred.title_written();
        self.changed_something();
        true
    }

    /// For a copy of `[src, src + len)` into a texture: a buffer (readable by
    /// copies) that holds what shaders wrote there, at this place of the list.
    /// `None` where shaders do not write it.
    pub(crate) fn place_source(&self, src: usize, len: usize) -> Option<ID3D12Resource> {
        if !on() {
            return None;
        }
        let host = host()?;
        let from = shaders_own(src, len)?;
        let between = self.between(host, len as u64)?;
        from.refresh(self, src as u64, len as u64);
        STATS[3].fetch_add(1, Ordering::Relaxed);
        // SAFETY: an open list; live buffers, ranges inside them.
        unsafe {
            wait_for_all(host, &self.list);
            self.list.CopyBufferRegion(&between, 0, &from.buffer, from.offset(src as u64), len as u64);
            self.list.ResourceBarrier(&[transition(&between, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE)]);
            wait_for_all(host, &self.list);
        }
        Some(between)
    }

    /// A buffer on the card for this recording alone.
    fn between(&self, host: &Host, len: u64) -> Option<ID3D12Resource> {
        let buffer = host.uav_buffer(len.next_multiple_of(1 << 16))?;
        name(&buffer, || "bytes between copies".into());
        self.keep(&buffer);
        Some(buffer)
    }
}

#[cfg(test)]
mod tests {
    use super::Waiting;

    /// A step of a submission: what it reads on the CPU (address, length), and what it writes (from, to) if it
    /// is a copy at its place that waits for its readers.
    type Seen = (Option<(usize, usize)>, Option<(usize, usize)>);

    fn fill(at: usize, len: usize) -> Seen {
        (Some((at, len)), None)
    }

    /// Which of `steps` read what a waiting copy writes.
    fn readers(steps: &[Seen], in_order: bool) -> Vec<usize> {
        let mut waiting = Waiting::new(steps.iter().filter_map(|s| s.1), in_order);
        (0..steps.len()).filter(|&i| waiting.step(steps[i].0, steps[i].1)).collect()
    }

    /// A step ahead of a waiting copy does not read what the copy writes, one behind it does; with every step
    /// counted (`LEONIDA_PLACED_READERS=0`) both do, as before.
    #[test]
    fn waiting_copies_are_read_by_what_stands_behind_them() {
        let copy: Seen = (None, Some((2000, 2288)));
        // A fill of the copy's destination ahead of it, one behind it, and one of other bytes.
        let steps = [fill(2000, 64), copy, fill(2100, 64), fill(5000, 64)];
        assert_eq!(readers(&steps, true), [2]);
        assert_eq!(readers(&steps, false), [0, 2]);
        // Only ahead: nothing waits on the copy's account.
        assert!(readers(&steps[..2], true).is_empty());
        assert_eq!(readers(&steps[..2], false), [0]);
        // Ends that touch are apart, and a read of no bytes reads nothing.
        let steps = [copy, fill(1936, 64), fill(2288, 64), fill(2100, 0), fill(2287, 1)];
        assert_eq!(readers(&steps, true), [4]);
        assert_eq!(readers(&steps, false), [4]);
    }

    /// A waiting copy that takes its source on the CPU reads what a waiting copy ahead of it writes, not what
    /// it writes itself; and each copy counts from its own place on.
    #[test]
    fn a_waiting_copy_reads_its_source_not_its_destination() {
        let first: Seen = (None, Some((2000, 2064)));
        // Out of the first one's destination.
        let chained: Seen = (Some((2000, 64)), Some((3000, 3064)));
        let onto_itself: Seen = (Some((3000, 64)), Some((3000, 3064)));
        assert_eq!(readers(&[first, chained], true), [1]);
        assert!(readers(&[chained, first], true).is_empty());
        assert_eq!(readers(&[chained, first], false), [0]);
        assert!(readers(&[onto_itself], true).is_empty());
        // Two copies, a fill between them of the second one's destination and two behind both.
        let second: Seen = (None, Some((3000, 3064)));
        let steps = [first, fill(3000, 8), second, fill(3000, 8), fill(2000, 8)];
        assert_eq!(readers(&steps, true), [3, 4]);
        assert_eq!(readers(&steps, false), [1, 3, 4]);
    }

    /// What copies with two sides cost to make (ignored: a measurement for
    /// `LEONIDA_CARD_ALL`): buffers of 64 KiB on the card and in the PC's
    /// memory.
    #[test]
    #[ignore]
    fn small_buffers_cost() {
        let Some(host) = super::host() else {
            return;
        };
        const N: usize = 1000;
        let used = || host.video_memory().map_or(0, |m| m[1] >> 20);
        let before = used();
        let t = std::time::Instant::now();
        let card: Vec<_> = (0..N).map(|_| host.uav_buffer(1 << 16)).collect();
        let card_us = t.elapsed().as_micros() as f64 / N as f64;
        let after = used();
        let t = std::time::Instant::now();
        let pc: Vec<_> = (0..N).map(|_| host.shared_buffer(1 << 16)).collect();
        let pc_us = t.elapsed().as_micros() as f64 / N as f64;
        println!(
            "{N} buffers of 64 KiB: on the card {card_us:.0} us each, {} MB of its memory; in the PC's memory {pc_us:.0} us each",
            after.saturating_sub(before)
        );
        drop((card, pc));
    }
}
