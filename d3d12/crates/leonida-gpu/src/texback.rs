//! Textures shaders write that the title's own code then reads on the CPU.
//! On the console the CPU and the GPU share memory, so what a compute pass
//! stores into a texture is in title memory for the CPU to read. Here it stays
//! in the texture's host copy. The title's ocean is one: its wave passes write
//! a 256x256 height map (R32_FLOAT) and a displacement map (R32G32_FLOAT), and
//! its CPU code samples both, which is what boats float on; it read memory
//! nothing had written (run rw1, `LEONIDA_READ_WATCH=1`: about 30 places in
//! the title read those two kinds), and boats sank.
//!
//! For textures of the kinds in `LEONIDA_TEXTURE_BACK` (comma-separated
//! `<width>x<height>f<format>`; unset or `1`: the four water formats found by
//! the upstream trace; `0`: none), a list in which shaders write one copies it
//! into a read-back buffer of ours when it closes, and once the list has run
//! the texels are tiled into the texture's title memory in the console's
//! layout, as the console's GPU would have left them.
//!
//! shortcut: by kind, not by texture: textures of those kinds the title never
//! reads are copied back too (about a megabyte per such list). The copy shows
//! what the list's last write left; a title read in between sees the frame
//! before, as it would before the console's GPU has run.

use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex, OnceLock, Weak};

use windows::Win32::Graphics::Direct3D12::*;

use crate::object::This;

/// The kinds copied back: (width, height, format).
fn kinds() -> &'static [(u64, u32, u32)] {
    static KINDS: OnceLock<Vec<(u64, u32, u32)>> = OnceLock::new();
    KINDS.get_or_init(|| {
        const DEFAULT: &str = "256x256f41,256x256f16,64x64f16,128x128f36";
        let list = match std::env::var("LEONIDA_TEXTURE_BACK").as_deref() {
            Ok("1") | Err(_) => DEFAULT.to_owned(),
            Ok(spec) => spec.to_owned(),
        };
        let kinds: Vec<(u64, u32, u32)> = list
            .split(',')
            .filter_map(|k| {
                let (size, format) = k.trim().split_once('f')?;
                let (w, h) = size.split_once('x')?;
                Some((w.parse().ok()?, h.parse().ok()?, format.parse().ok()?))
            })
            .collect();
        if !kinds.is_empty() {
            tracing::info!(?kinds, "textures of these kinds that shaders write are copied back into title memory (LEONIDA_TEXTURE_BACK)");
        }
        kinds
    })
}

/// Whether title texture `resource` is of a kind copied back.
pub(crate) fn wanted(resource: This) -> bool {
    let kinds = kinds();
    !kinds.is_empty()
        && crate::resource::texture_kind(resource).is_some_and(|k| kinds.contains(&k))
}

/// An outstanding write to a specific allocation. Mapping and resource
/// changes invalidate only the writes that could touch the replaced memory.
struct Ticket {
    owner: usize,
    at: usize,
    end: usize,
    valid: AtomicBool,
}

static TICKETS: Mutex<Vec<Weak<Ticket>>> = Mutex::new(Vec::new());
static TRACKING: AtomicBool = AtomicBool::new(false);

fn track(owner: usize, at: usize, end: usize) -> Arc<Ticket> {
    let ticket = Arc::new(Ticket { owner, at, end, valid: AtomicBool::new(true) });
    let mut tickets = TICKETS.lock().unwrap_or_else(|e| e.into_inner());
    tickets.retain(|weak| weak.strong_count() != 0);
    tickets.push(Arc::downgrade(&ticket));
    TRACKING.store(true, Ordering::Relaxed);
    ticket
}

fn invalidate(matches: impl Fn(&Ticket) -> bool) {
    if !TRACKING.load(Ordering::Relaxed) {
        return;
    }
    TICKETS.lock().unwrap_or_else(|e| e.into_inner()).retain(|weak| {
        let Some(ticket) = weak.upgrade() else { return false };
        if matches(&ticket) {
            ticket.valid.store(false, Ordering::Release);
        }
        true
    });
}

pub(crate) fn released(owner: usize) {
    invalidate(|ticket| ticket.owner == owner);
}

pub(crate) fn replaced(at: usize) {
    invalidate(|ticket| ticket.at == at);
}

pub(crate) fn remapped(at: usize, len: usize) {
    let end = at.saturating_add(len);
    invalidate(|ticket| at < ticket.end && ticket.at < end);
}

pub(crate) struct Target {
    pub(crate) copy: Arc<crate::resource::HostTexture>,
    layout: leonida_formats::texture::Layout,
    ticket: Arc<Ticket>,
}

impl Target {
    pub(crate) fn new(resource: This) -> Option<Self> {
        // Keep the host copy independently of the title's resource lifetime.
        let copy = crate::resource::host_copy(resource)?;
        let _mapping = crate::watch::title_memory_reading();
        let (layout, at, size) = crate::resource::title_texture(resource)?;
        let end = at.checked_add(size).filter(|&end| at != 0 && end > at)?;
        let ticket = track(resource as usize, at, end);
        Some(Self { copy, layout, ticket })
    }

    pub(crate) fn owner(&self) -> usize { self.ticket.owner }

    /// Whether the ticket is still valid: the texture's memory has not been
    /// reused or remapped since it was registered.
    pub(crate) fn fresh(&self) -> bool { self.ticket.valid.load(Ordering::Acquire) }
}

/// What a list does with a texture its shaders write again
/// ([`crate::host::HostList::texture_written`]), given whether it has a
/// read-back registered for the texture and whether that one's ticket is
/// still valid ([`Target::fresh`]).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) enum Again {
    /// Registered and valid: nothing to do.
    Keep,
    /// Registered, but its ticket went stale (the title reused or remapped
    /// the memory the texture tiles into): registered anew, or the
    /// read-back would be dropped as one of memory the texture no longer
    /// holds, and the title would read what was there before (the sea's
    /// height under a boat).
    Renew,
    /// Not registered yet.
    Add,
}

/// [`Again`] for a texture with a registered read-back (`Some(fresh)`) or
/// none. `LEONIDA_BACK_RENEW=0`: a stale one is kept, as before.
///
/// From aki-gta6/aki-runtime d9d1341 (Aki-Developer; their port of
/// fxceless/victory-fork 86d446b, pull request 1376), GPL-3.0.
pub(crate) fn again(registered: Option<bool>, renew: bool) -> Again {
    match registered {
        None => Again::Add,
        Some(false) if renew => Again::Renew,
        Some(_) => Again::Keep,
    }
}

/// The readback resources and the allocation the pixels belong to.
pub(crate) struct Back {
    buffer: ID3D12Resource,
    footprints: Vec<D3D12_PLACED_SUBRESOURCE_FOOTPRINT>,
    rows: Vec<u32>,
    mips: u32,
    layout: leonida_formats::texture::Layout,
    ticket: Arc<Ticket>,
}

impl Back {
    /// Its read-back buffer, for the list that held it to give back when it
    /// is reset (`crate::host`, `read_back_pool`).
    pub(crate) fn into_buffer(self) -> ID3D12Resource {
        self.buffer
    }
}

impl Back {
    pub(crate) fn range(&self) -> (usize, usize) {
        (self.ticket.at, self.ticket.end)
    }
}

/// Records a host copy from a texture already in COPY_SOURCE.
pub(crate) fn record(list: &ID3D12GraphicsCommandList, target: Target) -> Option<Back> {
    if !target.ticket.valid.load(Ordering::Acquire) {
        return None;
    }
    let host = crate::host::host()?;
    let texture = &target.copy.texture;
    // SAFETY: a live texture kept through the command list's state tracking.
    let desc = unsafe { texture.GetDesc() };
    let mips = u32::from(desc.MipLevels.max(1));
    let subresources = mips * u32::from(desc.DepthOrArraySize.max(1));
    let n = subresources as usize;
    let mut footprints = vec![D3D12_PLACED_SUBRESOURCE_FOOTPRINT::default(); n];
    let mut rows = vec![0u32; n];
    let mut total = 0u64;
    // SAFETY: out arrays of `n` entries.
    unsafe {
        host.device.GetCopyableFootprints(
            &desc,
            0,
            subresources,
            0,
            Some(footprints.as_mut_ptr()),
            Some(rows.as_mut_ptr()),
            None,
            Some(&mut total),
        )
    };
    // The last footprint's advertised extent can omit row-end padding.
    // The tiler reads a pitched slice, so the mapped buffer must include it.
    for (footprint, &rows) in footprints.iter().zip(&rows) {
        let format = footprint.Footprint;
        let pitched = u64::from(format.RowPitch)
            .checked_mul(u64::from(rows))?
            .checked_mul(u64::from(format.Depth))?;
        total = total.max(footprint.Offset.checked_add(pitched)?);
    }
    let buffer = host.read_back_buffer(total)?;
    for (i, footprint) in footprints.iter().enumerate() {
        let to = D3D12_TEXTURE_COPY_LOCATION {
            // SAFETY: a borrowed pointer the location never releases.
            pResource: unsafe { std::mem::transmute_copy(&buffer) },
            Type: D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
            Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 { PlacedFootprint: *footprint },
        };
        let from = D3D12_TEXTURE_COPY_LOCATION {
            // SAFETY: as above.
            pResource: unsafe { std::mem::transmute_copy(texture) },
            Type: D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
            Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 { SubresourceIndex: i as u32 },
        };
        // SAFETY: an open list; the texture is in the copy-source state.
        unsafe { list.CopyTextureRegion(&to, 0, 0, 0, &from, None) };
    }
    Some(Back { buffer, footprints, rows, mips, layout: target.layout, ticket: target.ticket })
}

/// Whether texture read-backs are written into title memory by a thread of
/// their own (`LEONIDA_TEXBACK_ASYNC`; on unless `0`).
///
/// Writing one takes the lock against mapping changes
/// ([`crate::watch::title_memory_reading`]), and while the title streams the
/// world it changes mappings all the time: on the thread that lands
/// submissions the wait for that lock stood 100-400 ms in some frames
/// (run 5 of the 16 GB machine, RTX 3060: "textures shaders wrote" in the
/// read-backs line, two to ten 350 KB water maps a frame), and with it the
/// landing of everything else. Here the landing thread only copies the
/// texels out of the read-back buffer; the waiting, tiling and writing go to
/// [`writer`]. Of several copies of one texture waiting there only the newest
/// is written: the title reads the frame before at most, which it may on the
/// console too.
fn asynchronous() -> bool {
    static ON: OnceLock<bool> = OnceLock::new();
    *ON.get_or_init(|| std::env::var_os("LEONIDA_TEXBACK_ASYNC").is_none_or(|v| v != "0"))
}

/// Whether read-backs go the light way (`LEONIDA_TEXBACK_LIGHT=0`: as
/// before): a read-back is tiled outside the tickets' lock, which is taken
/// for the write into title memory alone, into memory its thread keeps; and
/// of a list's read-backs of one texture only the last has its texels taken
/// out of its buffer ([`land_many`]).
///
/// The tickets' lock is what everyone waits for who lets a texture go,
/// replaces one or maps other memory where one lies ([`released`],
/// [`replaced`], [`remapped`]: the title's own threads) and who records a
/// read-back ([`Target::new`]); it was held while a texture was read out of
/// title memory and tiled, some hundred kilobytes each. A ticket made
/// invalid meanwhile is found so under the lock, before anything is
/// written, as before; mappings do not change while the texture is read
/// ([`crate::watch::title_memory_reading`]).
///
/// (After XaviFortes/VIctory-RDNA e4250eb. That commit also tiles a
/// flight's read-backs on several threads; here they have left the landing
/// thread already, [`asynchronous`].)
fn light() -> bool {
    crate::toggled!("LEONIDA_TEXBACK_LIGHT")
}

/// Of read-backs with these ranges, in order: those that are landed, the
/// last of each range ([`light`]). A read-back holds every subresource of
/// its texture, so a later one of the same texture leaves nothing of an
/// earlier one.
fn last_of_each(ranges: &[(usize, usize)]) -> Vec<usize> {
    (0..ranges.len()).filter(|&i| !ranges[i + 1..].contains(&ranges[i])).collect()
}

/// [`land`] for a list's read-backs, in order.
pub(crate) fn land_many(backs: &[Back]) {
    if backs.len() < 2 || !light() {
        backs.iter().for_each(land);
        return;
    }
    let ranges: Vec<(usize, usize)> = backs.iter().map(Back::range).collect();
    for i in last_of_each(&ranges) {
        land(&backs[i]);
    }
}

/// A read-back's texels taken out of its buffer, to be tiled into title memory.
struct Landing {
    footprints: Vec<D3D12_PLACED_SUBRESOURCE_FOOTPRINT>,
    rows: Vec<u32>,
    mips: u32,
    layout: leonida_formats::texture::Layout,
    ticket: Arc<Ticket>,
    /// The read-back buffer's bytes, from 0 to the end of its last subresource.
    data: Vec<u8>,
}

/// Once the list has run, tile its texels back into the same title allocation
/// (on [`writer`]'s thread when [`asynchronous`]).
pub(crate) fn land(back: &Back) {
    if !back.ticket.valid.load(Ordering::Acquire) {
        stale();
        return;
    }
    let len = back
        .footprints
        .iter()
        .zip(&back.rows)
        .map(|(f, &rows)| f.Offset as usize + f.Footprint.RowPitch as usize * rows as usize * f.Footprint.Depth as usize)
        .max()
        .unwrap_or(0);
    let mut mapped = std::ptr::null_mut();
    // SAFETY: our read-back buffer, written by the list that has run; `len`
    // ends with its last subresource, which it holds.
    let data = unsafe {
        if back.buffer.Map(0, None, Some(&mut mapped)).is_err() || mapped.is_null() {
            return;
        }
        let data = std::slice::from_raw_parts(mapped.cast::<u8>().cast_const(), len).to_vec();
        back.buffer.Unmap(0, None);
        data
    };
    let landing = Landing {
        footprints: back.footprints.clone(),
        rows: back.rows.clone(),
        mips: back.mips,
        layout: back.layout.clone(),
        ticket: back.ticket.clone(),
        data,
    };
    match asynchronous().then(writer).flatten() {
        Some(writer) => writer.send(landing),
        None => write(&landing),
    }
}

fn stale() {
    static STALE: AtomicU64 = AtomicU64::new(0);
    let skipped = STALE.fetch_add(1, Ordering::Relaxed) + 1;
    if skipped.is_power_of_two() {
        tracing::info!(skipped, "skipping texture readbacks whose title allocation changed");
    }
}

/// Tiles a read-back's texels into its title allocation.
fn write(landing: &Landing) {
    use crate::land::{count, spent, stat};
    static COPIED: AtomicU64 = AtomicU64::new(0);
    thread_local! {
        /// The texture as it is tiled, kept from one to the next ([`light`]).
        static TILED: std::cell::RefCell<Vec<u8>> = const { std::cell::RefCell::new(Vec::new()) };
    }
    /// Memory kept for the next up to this size.
    const KEPT: usize = 32 << 20;
    let asked = std::time::Instant::now();
    let light = light();
    let _mapping = crate::watch::title_memory_reading();
    let _ticket_guard = (!light).then(|| TICKETS.lock().unwrap_or_else(|e| e.into_inner()));
    let working = spent(stat::TB_WAIT, asked);
    if !landing.ticket.valid.load(Ordering::Acquire) {
        stale();
        return;
    }
    let (at, end) = (landing.ticket.at, landing.ticket.end);
    let size = end - at;
    // The texture as it will be, in `tiled` (`size` bytes); whether it is.
    let tile = |tiled: &mut [u8]| {
        // Keep title bytes the subresources do not cover.
        if !crate::host::read_into(at as *const u8, tiled) {
            return false;
        }
        for (i, (footprint, &rows)) in landing.footprints.iter().zip(&landing.rows).enumerate() {
            let (mip, slice) = (i as u32 % landing.mips, i as u32 / landing.mips);
            let f = footprint.Footprint;
            let pitch = f.RowPitch as usize;
            let from = footprint.Offset as usize;
            let linear = &landing.data[from..from + pitch * rows as usize * f.Depth as usize];
            if let Err(e) = landing.layout.tile(tiled, mip, slice, linear, pitch, pitch * rows as usize) {
                static ONCE: std::sync::Once = std::sync::Once::new();
                ONCE.call_once(|| tracing::warn!(?e, mip, slice, "a texture copied back could not be tiled"));
                return false;
            }
        }
        true
    };
    if light {
        let written = TILED.with_borrow_mut(|kept| {
            if kept.len() < size {
                kept.resize(size, 0);
            }
            let written = tile(&mut kept[..size]) && {
                // The lock for the write alone: who lets the allocation go
                // or maps other memory there waits for a copy, not for the
                // tiling, and what it made invalid meanwhile is not written.
                let _tickets = TICKETS.lock().unwrap_or_else(|e| e.into_inner());
                let valid = landing.ticket.valid.load(Ordering::Acquire);
                if valid {
                    crate::host::copy_mapped(at as *mut u8, kept.as_ptr(), size, true);
                } else {
                    stale();
                }
                valid
            };
            if kept.capacity() > KEPT {
                *kept = Vec::new();
            }
            written
        });
        if !written {
            return;
        }
    } else {
        let mut tiled = vec![0u8; size];
        if !tile(&mut tiled) {
            return;
        }
        crate::host::copy_mapped(at as *mut u8, tiled.as_ptr(), size, true);
    }
    spent(stat::TB_WORK, working);
    count(stat::TB_WRITTEN, 1);
    let copied = COPIED.fetch_add(1, Ordering::Relaxed) + 1;
    if copied == 1 || copied.is_multiple_of(128) {
        tracing::info!(copied, bytes = size, "texture readback landed");
    }
}

/// The thread that writes read-backs into title memory, and what waits for it.
struct Writer {
    /// By title address: the newest landing for that allocation.
    waiting: Mutex<std::collections::HashMap<usize, Landing>>,
    more: std::sync::Condvar,
}

impl Writer {
    fn send(&self, landing: Landing) {
        let mut waiting = self.waiting.lock().unwrap_or_else(|e| e.into_inner());
        if waiting.insert(landing.ticket.at, landing).is_some() {
            crate::land::count(crate::land::stat::TB_REPLACED, 1);
        }
        drop(waiting);
        self.more.notify_one();
    }
}

/// The writer, if its thread could be started (if not, landings are written
/// where they are made, as before).
fn writer() -> Option<&'static Writer> {
    static WRITER: OnceLock<Option<&'static Writer>> = OnceLock::new();
    *WRITER.get_or_init(|| {
        let writer: &'static Writer =
            Box::leak(Box::new(Writer { waiting: Mutex::new(std::collections::HashMap::new()), more: std::sync::Condvar::new() }));
        let started = std::thread::Builder::new().name("texture read-backs".into()).spawn(move || loop {
            let batch: Vec<Landing> = {
                let mut waiting = writer.waiting.lock().unwrap_or_else(|e| e.into_inner());
                while waiting.is_empty() {
                    waiting = writer.more.wait(waiting).unwrap_or_else(|e| e.into_inner());
                }
                waiting.drain().map(|(_, landing)| landing).collect()
            };
            for landing in &batch {
                write(landing);
            }
        });
        match started {
            Ok(_) => Some(writer),
            Err(e) => {
                tracing::warn!(error = %e, "no thread for texture read-backs: they are written where they land");
                None
            }
        }
    })
}


#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_readbacks_for_changed_allocations_are_invalidated() {
        let first = track(91, 0x1000, 0x2000);
        let next = track(92, 0x2000, 0x3000);
        let far = track(93, 0x4000, 0x5000);

        remapped(0x0f00, 0x200);
        assert!(!first.valid.load(Ordering::Acquire));
        assert!(next.valid.load(Ordering::Acquire));
        replaced(0x2000);
        assert!(!next.valid.load(Ordering::Acquire));
        assert!(far.valid.load(Ordering::Acquire));

        released(93);
        assert!(!far.valid.load(Ordering::Acquire));
        assert!(track(94, 0x2000, 0x3000).valid.load(Ordering::Acquire));
    }

    /// A list that writes a texture again registers its read-back anew only
    /// when the one it has went stale.
    #[test]
    fn a_stale_readback_is_registered_again() {
        assert_eq!(again(None, true), Again::Add);
        assert_eq!(again(Some(true), true), Again::Keep);
        assert_eq!(again(Some(false), true), Again::Renew);
        // The switch off: a stale one stays, as before.
        assert_eq!(again(Some(false), false), Again::Keep);
        assert_eq!(again(None, false), Again::Add);
        // What makes one stale: its memory mapped anew.
        let ticket = track(95, 0x9000, 0xa000);
        assert!(ticket.valid.load(Ordering::Acquire));
        remapped(0x9800, 0x100);
        assert!(!ticket.valid.load(Ordering::Acquire));
    }

    /// Of a list's read-backs of one texture the last is landed, the others
    /// each in their place.
    #[test]
    fn the_last_readback_of_each_texture_is_landed() {
        let (a, b, c) = ((0x1000, 0x2000), (0x2000, 0x3000), (0x1000, 0x1800));
        assert_eq!(last_of_each(&[]), [0usize; 0]);
        assert_eq!(last_of_each(&[a]), [0]);
        assert_eq!(last_of_each(&[a, b, a, c, b, b]), [2, 3, 5]);
        assert_eq!(last_of_each(&[a, c, b]), [0, 1, 2]);
    }

    /// A read-back's texels are tiled into the title's allocation, the bytes
    /// no subresource covers staying; one whose allocation went meanwhile is
    /// not written.
    #[test]
    fn a_readback_is_written_unless_its_allocation_went() {
        use leonida_formats::texture::{Desc, Dimension, Layout, Swizzle};
        const OWNER: usize = 0x7e57_bac0;
        let layout = Layout::new(&Desc {
            dimension: Dimension::Tex2d,
            width: 64,
            height: 16,
            depth: 1,
            mips: 1,
            element_bytes: 4,
            block: 1,
            samples: 1,
            swizzle: Swizzle::LINEAR,
        })
        .unwrap();
        // The title's texture, and some bytes behind it in the allocation.
        let size = layout.size as usize + 64;
        let mut title = vec![0xEEu8; size];
        let at = title.as_mut_ptr() as usize;
        // Rows of 256 bytes at a pitch of 320 in the read-back buffer.
        let (pitch, rows) = (320usize, 16u32);
        let texels = |seed: usize| -> Vec<u8> { (0..pitch * rows as usize).map(|i| (i * 7 + seed) as u8).collect() };
        let landing = |seed: usize| Landing {
            footprints: vec![D3D12_PLACED_SUBRESOURCE_FOOTPRINT {
                Offset: 0,
                Footprint: D3D12_SUBRESOURCE_FOOTPRINT { Width: 64, Height: 16, Depth: 1, RowPitch: pitch as u32, ..Default::default() },
            }],
            rows: vec![rows],
            mips: 1,
            layout: layout.clone(),
            ticket: track(OWNER, at, at + size),
            data: texels(seed),
        };
        let expected = |seed: usize, before: &[u8]| {
            let mut tiled = before.to_vec();
            layout.tile(&mut tiled, 0, 0, &texels(seed), pitch, pitch * rows as usize).unwrap();
            tiled
        };
        // (Written behind the vector's back: read the same way.)
        // SAFETY: the vector's bytes, which live to the end of the test.
        let now = || unsafe { std::slice::from_raw_parts(at as *const u8, size) }.to_vec();
        let before = now();
        let first = expected(1, &before);
        assert_ne!(first, before);
        assert_eq!(first[layout.size as usize..], [0xEE; 64], "the bytes behind the texture are no subresource's");
        write(&landing(1));
        assert_eq!(now(), first);
        // Again, into the memory the thread kept; then one whose texture was
        // let go before it is written.
        let second = expected(2, &first);
        assert_ne!(second, first);
        write(&landing(2));
        assert_eq!(now(), second);
        let late = landing(3);
        released(OWNER);
        write(&late);
        assert_eq!(now(), second, "a read-back was written into an allocation that went");
        drop(title);
    }
}
