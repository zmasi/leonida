//! `xg_xs.dll`: the console's texture layout library. Titles ask it which
//! swizzle mode a texture should use, how large it is, where each mip and
//! slice lies, and to copy linear data into tiled memory and back. Served
//! from `leonida_formats::texture`.
//!
//! The title imports `XGCreateTextureComputer` and
//! `XGSuggestSwizzleMode` and calls these computer methods: Release (0x08),
//! the layout (0x10), copies into (0x40) and out of (0x48) a subresource,
//! and the 64 KiB tiling (0x50). The other slots log and fail.
#![allow(non_snake_case)]
// Title code passes pointers the real library would trust.
#![allow(clippy::not_unsafe_ptr_arg_deref)]

use std::sync::atomic::{AtomicU32, Ordering};

use leonida_formats::texture::{Dimension, Error, Layout, Swizzle, Texture, TextureDesc};

use super::xasync::{E_INVALIDARG, E_NOTIMPL};
use super::xgameruntime::{HRESULT, S_OK};
use crate::{HleModule, hle_export};

const E_FAIL: HRESULT = 0x8000_4005_u32 as i32;

/// `D3D12_RESOURCE_DESC`, whose layout field carries `0x100 | swizzle
/// mode` on the console (1 row major, 0 "pick one").
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct ResourceDesc {
    pub dimension: u32,
    pub alignment: u64,
    pub width: u64,
    pub height: u32,
    pub depth_or_array: u16,
    pub mips: u16,
    pub format: u32,
    pub samples: u32,
    pub quality: u32,
    pub layout: u32,
    pub flags: u32,
}
const _: () = assert!(size_of::<ResourceDesc>() == 0x38);

const SWIZZLED: u32 = 0x100;
const ROW_MAJOR: u32 = 1;
const ALLOW_RENDER_TARGET: u32 = 1;
const ALLOW_DEPTH_STENCIL: u32 = 2;
const TEXTURE_1D: u32 = 2;
const TEXTURE_3D: u32 = 4;

/// The mode the console's library picks for a description: linear for 1D,
/// the XOR render and depth modes for targets, else 4 KiB standard
/// (`.ttpf` textures without a mode of their own use it).
///
/// shortcut: the rule of the library's older optimal-mode function (any
/// size, 2D, 3D and cubes alike); `XGSuggestSwizzleMode` itself is newer
/// than every copy we could check. Revisit if streamed textures come out
/// scrambled.
fn suggested(desc: &ResourceDesc) -> Swizzle {
    if desc.dimension == TEXTURE_1D {
        Swizzle::LINEAR
    } else if desc.flags & ALLOW_DEPTH_STENCIL != 0 {
        Swizzle::SW_64KB_Z_X
    } else if desc.flags & ALLOW_RENDER_TARGET != 0 {
        Swizzle::SW_64KB_R_X
    } else {
        Swizzle::SW_4KB_S
    }
}

/// `(desc, flags, out layout)`; the title passes flags 0 and puts the
/// result in the description's layout field.
pub extern "system" fn XGSuggestSwizzleMode(
    desc: *const ResourceDesc,
    _flags: u32,
    out: *mut u32,
) -> HRESULT {
    if desc.is_null() || out.is_null() {
        return E_INVALIDARG;
    }
    // SAFETY: checked non-null; read `*desc` before writing `out` because the
    // title passes `&desc.layout` as `out`.
    let d = unsafe { desc.read_unaligned() };
    unsafe { out.write_unaligned(SWIZZLED | suggested(&d).0) };
    S_OK
}

#[repr(C)]
pub struct Computer {
    vtable: &'static Vtable,
    refs: AtomicU32,
    desc: ResourceDesc,
    mips: u32,
    texture: Texture,
}

type This = *mut Computer;

fn computer<'a>(this: This) -> Option<&'a Computer> {
    // SAFETY: called from computer methods; null `this` is rejected.
    unsafe { this.as_ref() }
}

pub extern "system" fn XGCreateTextureComputer(
    desc: *const ResourceDesc,
    out: *mut This,
) -> HRESULT {
    if desc.is_null() || out.is_null() {
        return E_INVALIDARG;
    }
    // SAFETY: checked non-null; copied before writing `out`.
    let desc = unsafe { desc.read_unaligned() };
    let Some(texture) = texture(&desc) else {
        tracing::warn!(?desc, "no texture layout");
        return E_INVALIDARG;
    };
    tracing::debug!(
        width = desc.width,
        height = desc.height,
        format = desc.format,
        layout = format_args!("{:#x}", desc.layout),
        size = texture.size,
        "texture computer"
    );
    let mips = texture.planes[0].layout.mips.len() as u32;
    let computer = Box::new(Computer {
        vtable: &VTABLE,
        refs: AtomicU32::new(1),
        desc,
        mips,
        texture,
    });
    // SAFETY: checked non-null.
    unsafe { out.write(Box::into_raw(computer)) };
    S_OK
}

fn texture(desc: &ResourceDesc) -> Option<Texture> {
    let dimension = match desc.dimension {
        TEXTURE_1D => Dimension::Tex1d,
        3 => Dimension::Tex2d,
        TEXTURE_3D => Dimension::Tex3d,
        _ => return None,
    };
    let swizzle = match desc.layout {
        0 => suggested(desc),
        ROW_MAJOR => Swizzle::LINEAR,
        l if l & !0xff == SWIZZLED => Swizzle(l & 0xff),
        _ => return None,
    };
    let texture = Texture::new(&TextureDesc {
        dimension,
        width: u32::try_from(desc.width).ok()?,
        height: desc.height,
        depth_or_array: u32::from(desc.depth_or_array),
        mips: u32::from(desc.mips),
        format: desc.format,
        samples: desc.samples.max(1),
        swizzle,
    })
    .ok()?;
    // The layout struct has room for 15 mips.
    (texture.planes[0].layout.mips.len() <= MIPS).then_some(texture)
}

const MIPS: usize = 15;

/// One mip in the layout the title reads (offsets 0x00, 0x08, 0x10 and
/// 0x1c are read by the title; the rest follow the library's
/// older public shape).
#[repr(C)]
#[derive(Debug, Default, Clone, Copy)]
pub struct MipOut {
    pub size: u64,
    pub offset: u64,
    pub slice_size: u64,
    pub pitch_texels: u32,
    pub pitch_bytes: u32,
    pub alignment: u32,
    /// Width, height in elements, depth or array size.
    pub padded: [u32; 3],
    pub elements: [u32; 2],
    pub texels: [u32; 2],
    pub depth_or_array: u32,
    pub samples: u32,
    pub swizzle: u32,
    pub bank_rotation: [u64; 2],
    pub slice_depth: u32,
    pub in_tail: u32,
    /// Where a tail mip starts in elements: x bits 0-10, y 11-21.
    pub tail_position: u32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct PlaneOut {
    /// 1 data, 7 depth, 8 stencil (metadata planes are not made).
    pub usage: u32,
    pub size: u64,
    pub slice_size: u64,
    pub offset: u64,
    pub alignment: u64,
    pub element_bytes: u32,
    pub standard_mips: u32,
    pub tail_mips: u32,
    pub mips: [MipOut; MIPS],
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct LayoutOut {
    pub size: u64,
    pub alignment: u64,
    pub mips: u32,
    pub planes: u32,
    pub plane: [PlaneOut; 4],
    pub dimension: u32,
}
const _: () = assert!(size_of::<MipOut>() == 0x70);
const _: () = assert!(size_of::<PlaneOut>() == 0x6c8);
const _: () = assert!(size_of::<LayoutOut>() == 0x1b40);

/// The mode a tail mip is reported in: the 256-byte one of its kind (3 for
/// the render kind, which GFX10 lacks), the depth mode itself.
fn tail_swizzle(swizzle: Swizzle) -> u32 {
    match swizzle.0 {
        2 | 6 | 10 | 18 | 22 | 26 => Swizzle::SW_256B_D.0,
        23 | 27 => 3,
        24 => swizzle.0,
        _ => Swizzle::SW_256B_S.0,
    }
}

fn mip_out(desc: &ResourceDesc, l: &Layout, i: usize) -> MipOut {
    let m = &l.mips[i];
    let bpe = u64::from(l.desc.element_bytes);
    let block = l.desc.block;
    let three_d = desc.dimension == TEXTURE_3D;
    let array = u32::from(desc.depth_or_array).max(1);
    let mut out = MipOut {
        pitch_texels: m.pitch * block,
        pitch_bytes: m.pitch * l.desc.element_bytes,
        padded: [m.pitch, m.rows, if three_d { m.depth_pitch } else { array }],
        elements: [m.width, m.height],
        texels: [((desc.width >> i) as u32).max(1), (desc.height >> i).max(1)],
        depth_or_array: if three_d { m.depth } else { array },
        samples: l.desc.samples,
        slice_depth: l.block[2],
        ..MipOut::default()
    };
    let slice = u64::from(m.pitch) * u64::from(m.rows) * bpe;
    let depth = u64::from(if three_d { m.depth_pitch } else { 1 });
    match m.tail {
        None => {
            out.offset = m.offset;
            out.slice_size = slice * u64::from(l.desc.samples);
            out.size = out.slice_size * depth;
            out.alignment = l.alignment as u32;
            out.swizzle = l.desc.swizzle.0;
        }
        Some(t) => {
            out.in_tail = 1;
            out.alignment = 256;
            out.swizzle = tail_swizzle(l.desc.swizzle);
            if l.thick {
                // A 3D tail mip is placed by position, not by offset.
                out.slice_size = l.alignment / u64::from(l.block[2]);
                out.tail_position = t.x | t.y << 11;
            } else {
                out.offset = u64::from(t.offset);
                out.slice_size = slice;
            }
            out.size = out.slice_size * depth;
        }
    }
    out
}

extern "system" fn add_ref(this: This) -> u32 {
    let Some(c) = computer(this) else {
        return 0;
    };
    c.refs.fetch_add(1, Ordering::Relaxed) + 1
}

extern "system" fn release(this: This) -> u32 {
    if this.is_null() {
        return 0;
    }
    // SAFETY: checked non-null; access `refs` via raw pointer so no `&Computer`
    // borrow spans `Box::from_raw` on the final release.
    let left = unsafe { (*this).refs.fetch_sub(1, Ordering::AcqRel) } - 1;
    if left == 0 {
        // SAFETY: made by `Box::into_raw` in `XGCreateTextureComputer`; the
        // last reference is gone.
        drop(unsafe { Box::from_raw(this) });
    }
    left
}

extern "system" fn layout(this: This, out: *mut LayoutOut) -> HRESULT {
    let (Some(c), false) = (computer(this), out.is_null()) else {
        return E_INVALIDARG;
    };
    // SAFETY: every field of `LayoutOut` is plain integer data, so zeroes are
    // valid; written with `write_unaligned` so `out` need not be aligned.
    let mut built: LayoutOut = unsafe { std::mem::zeroed() };
    built.size = c.texture.size;
    built.alignment = c.texture.alignment;
    built.mips = c.mips;
    built.planes = c.texture.planes.len() as u32;
    built.dimension = c.desc.dimension;
    // The library calls a depth plane "depth" only beside stencil or
    // without mips.
    let depth =
        c.desc.flags & ALLOW_DEPTH_STENCIL != 0 && (c.texture.planes.len() > 1 || c.mips == 1);
    for (plane, p) in built.plane.iter_mut().zip(&c.texture.planes) {
        let l = &p.layout;
        plane.usage = match (p.stencil, depth) {
            (true, _) => 8,
            (false, true) => 7,
            (false, false) => 1,
        };
        plane.size = l.size;
        plane.slice_size = l.slice_size;
        plane.offset = p.offset;
        plane.alignment = l.alignment;
        plane.element_bytes = l.desc.element_bytes;
        plane.standard_mips = l.first_tail_mip;
        plane.tail_mips = c.mips - l.first_tail_mip;
        for (i, mip) in plane.mips.iter_mut().take(l.mips.len()).enumerate() {
            *mip = mip_out(&c.desc, l, i);
        }
    }
    unsafe { out.write_unaligned(built) };
    S_OK
}

/// The plane, its mip and array slice for a subresource index, and the
/// tiled bytes and the linear bytes the copy touches.
struct CopyArgs<'a> {
    layout: &'a Layout,
    plane_offset: usize,
    mip: u32,
    slice: u32,
    tiled: usize,
    linear: usize,
}

fn copy_args(
    c: &Computer,
    plane: u32,
    subresource: u32,
    row: u32,
    depth: u32,
) -> Result<CopyArgs<'_>, HRESULT> {
    if c.mips == 0 {
        return Err(E_INVALIDARG);
    }
    let p = c.texture.planes.get(plane as usize).ok_or(E_INVALIDARG)?;
    let l = &p.layout;
    let (mip, slice) = (subresource % c.mips, subresource / c.mips);
    let m = l.mips.get(mip as usize).ok_or(E_INVALIDARG)?;
    let max_slices = if l.desc.dimension == Dimension::Tex3d {
        1
    } else {
        l.slices
    };
    if slice >= max_slices {
        return Err(E_INVALIDARG);
    }
    if !l.addressable() {
        return Err(E_NOTIMPL);
    }
    let bytes = (m.width as usize)
        .checked_mul(l.desc.element_bytes as usize)
        .ok_or(E_INVALIDARG)?;
    if (row as usize) < bytes {
        return Err(E_INVALIDARG);
    }
    let slice_bytes = (m.height.saturating_sub(1) as usize)
        .checked_mul(row as usize)
        .and_then(|offset| offset.checked_add(bytes))
        .ok_or(E_INVALIDARG)?;
    let depths = l.subresource_depth(mip) as usize;
    if depths > 1 && (depth as usize) < slice_bytes {
        return Err(E_INVALIDARG);
    }
    let linear = depths
        .saturating_sub(1)
        .checked_mul(depth as usize)
        .and_then(|offset| offset.checked_add(slice_bytes))
        .ok_or(E_INVALIDARG)?;
    let plane_offset = usize::try_from(p.offset).map_err(|_| E_INVALIDARG)?;
    let tiled = usize::try_from(l.bytes_from(mip)).map_err(|_| E_INVALIDARG)?;
    if tiled > isize::MAX as usize || linear > isize::MAX as usize {
        return Err(E_INVALIDARG);
    }
    Ok(CopyArgs {
        layout: l,
        plane_offset,
        mip,
        slice,
        tiled,
        linear,
    })
}

fn valid_disjoint_ranges(
    tiled_base: *const u8,
    plane_offset: usize,
    tiled_len: usize,
    linear_base: *const u8,
    linear_len: usize,
) -> bool {
    if tiled_base.is_null() || linear_base.is_null() {
        return false;
    }
    let Some(tiled_start) = (tiled_base as usize).checked_add(plane_offset) else {
        return false;
    };
    let Some(tiled_end) = tiled_start.checked_add(tiled_len) else {
        return false;
    };
    let linear_start = linear_base as usize;
    let Some(linear_end) = linear_start.checked_add(linear_len) else {
        return false;
    };
    !(tiled_len > 0 && linear_len > 0 && tiled_start < linear_end && linear_start < tiled_end)
}

fn result(r: Result<(), Error>) -> HRESULT {
    match r {
        Ok(()) => S_OK,
        Err(Error::Unsupported) => E_NOTIMPL,
        Err(_) => E_INVALIDARG,
    }
}

/// `(tiled base, plane, subresource, linear, row pitch, depth pitch)`.
extern "system" fn copy_into(
    this: This,
    tiled: *mut u8,
    plane: u32,
    subresource: u32,
    linear: *const u8,
    row: u32,
    depth: u32,
) -> HRESULT {
    let Some(c) = computer(this) else {
        return E_INVALIDARG;
    };
    if tiled.is_null() || linear.is_null() {
        return E_INVALIDARG;
    }
    let a = match copy_args(c, plane, subresource, row, depth) {
        Ok(a) => a,
        Err(hr) => {
            tracing::warn!(hr, desc = ?c.desc, subresource, "copy into a texture failed");
            return hr;
        }
    };
    if !valid_disjoint_ranges(tiled.cast_const(), a.plane_offset, a.tiled, linear, a.linear) {
        return E_INVALIDARG;
    }
    // SAFETY: bounds, address wraparound, and non-overlap were checked above.
    let (tiled, linear) = unsafe {
        (
            std::slice::from_raw_parts_mut(tiled.add(a.plane_offset), a.tiled),
            std::slice::from_raw_parts(linear, a.linear),
        )
    };
    let r = a
        .layout
        .tile(tiled, a.mip, a.slice, linear, row as usize, depth as usize);
    if r.is_err() {
        tracing::warn!(?r, desc = ?c.desc, subresource, "copy into a texture failed");
    }
    result(r)
}

/// `(linear, plane, subresource, sample, tiled base, row pitch, depth
/// pitch)`.
#[allow(clippy::too_many_arguments)]
extern "system" fn copy_from(
    this: This,
    linear: *mut u8,
    plane: u32,
    subresource: u32,
    sample: u32,
    tiled: *const u8,
    row: u32,
    depth: u32,
) -> HRESULT {
    let Some(c) = computer(this) else {
        return E_INVALIDARG;
    };
    if tiled.is_null() || linear.is_null() {
        return E_INVALIDARG;
    }
    let a = match copy_args(c, plane, subresource, row, depth) {
        Ok(a) => a,
        Err(hr) => {
            tracing::warn!(hr, desc = ?c.desc, subresource, "copy out of a texture failed");
            return hr;
        }
    };
    if sample >= a.layout.desc.samples
        || !valid_disjoint_ranges(tiled, a.plane_offset, a.tiled, linear.cast_const(), a.linear)
    {
        return E_INVALIDARG;
    }
    // SAFETY: bounds, address wraparound, and non-overlap were checked above.
    let (tiled, linear) = unsafe {
        (
            std::slice::from_raw_parts(tiled.add(a.plane_offset), a.tiled),
            std::slice::from_raw_parts_mut(linear, a.linear),
        )
    };
    let r = a
        .layout
        .detile(tiled, a.mip, a.slice, linear, row as usize, depth as usize);
    if r.is_err() {
        tracing::warn!(?r, desc = ?c.desc, subresource, "copy out of a texture failed");
    }
    result(r)
}

#[repr(C)]
pub struct PackedMips {
    pub standard: u8,
    pub packed: u8,
    pub tiles: u32,
    pub start: u32,
}

#[repr(C)]
pub struct SubresourceTiling {
    pub width: u32,
    pub height: u16,
    pub depth: u16,
    pub start: u32,
}

const TILE: u64 = 64 << 10;

/// `(total tiles, packed mips, tile shape in texels, in: room / out:
/// count, first, subresource tilings)`, in 64 KiB tiles of plane 0, for 2D
/// 64 KiB modes only (others fail, as on the console). The mip tail is the
/// packed part, tile 0 of each slice; tilings list the standard mips of
/// every slice in turn. With a tail the console's library lists two slices
/// at most and zeroes all but the first; so do we (the title reads slice 0
/// only).
extern "system" fn tiling(
    this: This,
    tiles: *mut u32,
    packed: *mut PackedMips,
    shape: *mut [u32; 3],
    count: *mut u32,
    first: u32,
    out: *mut SubresourceTiling,
) -> HRESULT {
    let Some(c) = computer(this) else {
        return E_INVALIDARG;
    };
    let l = &c.texture.planes[0].layout;
    if l.alignment != TILE {
        return E_FAIL;
    }
    if l.thick {
        return E_NOTIMPL;
    }
    let standard = l.first_tail_mip;
    let tail = standard < c.mips;
    let tail_tiles = match standard {
        0 => l.slice_size / TILE,
        s if s < c.mips => l.mips[s as usize - 1].offset / TILE,
        _ => 0,
    };
    let block = l.desc.block;
    // SAFETY: each output pointer is checked non-null before writing; when
    // `out` is non-null it has room for `*count` entries.
    unsafe {
        if !tiles.is_null() {
            tiles.write_unaligned((l.size / TILE) as u32);
        }
        if !packed.is_null() {
            packed.write_unaligned(PackedMips {
                standard: standard as u8,
                packed: (c.mips - standard) as u8,
                tiles: tail_tiles as u32,
                start: 0,
            });
        }
        if !shape.is_null() {
            shape.write_unaligned([l.block[0] * block, l.block[1] * block, l.block[2]]);
        }
        if !count.is_null() {
            let requested = count.read_unaligned();
            let slices = if tail { l.slices.min(2) } else { l.slices };
            let total = u64::from(standard) * u64::from(slices);
            let wanted = total
                .saturating_sub(u64::from(first))
                .min(u64::from(requested)) as u32;
            if out.is_null() {
                count.write_unaligned(0);
            } else {
                for i in 0..wanted {
                    let sub = first + i;
                    let (slice, mip) = (sub / standard, sub % standard);
                    let m = &l.mips[mip as usize];
                    let entry = if tail && slice > 0 {
                        SubresourceTiling {
                            width: 0,
                            height: 0,
                            depth: 0,
                            start: 0,
                        }
                    } else {
                        SubresourceTiling {
                            width: m.pitch / l.block[0],
                            height: (m.rows / l.block[1]) as u16,
                            depth: 1,
                            start: ((u64::from(slice) * l.slice_size + m.offset) / TILE) as u32,
                        }
                    };
                    out.add(i as usize).write_unaligned(entry);
                }
                count.write_unaligned(wanted);
            }
        }
    }
    S_OK
}

/// Slot 0x30 `(plane, mip, x, y, z or slice, sample, bit position)`: an
/// element's byte offset from the texture's start. The title reads
/// texels of a 128x4 `4KB_S` texture on the CPU through it (title+0x5151c90);
/// an error returned as an offset made it read wild memory. 0 if outside.
extern "system" fn element_offset(
    this: This,
    plane: u32,
    mip: u32,
    x: u64,
    y: u32,
    z: u32,
    _sample: u32,
    bit: *mut u32,
) -> u64 {
    if !bit.is_null() {
        // SAFETY: the caller passes room for the bit position or null.
        unsafe { bit.write_unaligned(0) };
    }
    let Some(c) = computer(this) else {
        return 0;
    };
    let Some(p) = c.texture.planes.get(plane as usize) else {
        return 0;
    };
    let x = u32::try_from(x).unwrap_or(u32::MAX);
    match p.layout.offset(mip, x, y, z) {
        Ok(at) => p.offset + at,
        Err(e) => {
            tracing::warn!(?e, mip, x, y, z, "texel outside its texture");
            0
        }
    }
}

/// Slots the title does not call: size, alignment, offsets, coordinates,
/// view descriptor, tiled-resource query.
extern "system" fn unused(this: This) -> HRESULT {
    if let Some(c) = computer(this) {
        tracing::warn!(desc = ?c.desc, "unimplemented texture computer method");
    }
    E_NOTIMPL
}

#[repr(C)]
struct Vtable([*const (); 13]);
// SAFETY: function addresses only.
unsafe impl Sync for Vtable {}

static VTABLE: Vtable = Vtable([
    add_ref as *const (),   // 0x00
    release as *const (),   // 0x08
    layout as *const (),    // 0x10
    unused as *const (),    // 0x18 size
    unused as *const (),    // 0x20 alignment
    unused as *const (),    // 0x28 mip offset
    element_offset as *const (), // 0x30
    unused as *const (),    // 0x38 element coordinate
    copy_into as *const (), // 0x40
    copy_from as *const (), // 0x48
    tiling as *const (),    // 0x50
    unused as *const (),    // 0x58 view descriptor
    unused as *const (),    // 0x60 tiled resource?
]);

pub(crate) fn module() -> HleModule {
    HleModule {
        name: "xg",
        dlls: &["xg_xs.dll"],
        exports: vec![
            hle_export!(XGCreateTextureComputer),
            hle_export!(XGSuggestSwizzleMode),
        ],
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn desc(width: u64, height: u32, mips: u16, format: u32, layout: u32) -> ResourceDesc {
        ResourceDesc {
            dimension: 3,
            alignment: 0,
            width,
            height,
            depth_or_array: 1,
            mips,
            format,
            samples: 1,
            quality: 0,
            layout,
            flags: 0,
        }
    }

    /// Method `index` of a computer, as the title calls it.
    fn slot<T: Copy>(c: This, index: usize) -> T {
        // SAFETY: the computer is live; T is the slot's function type.
        unsafe { std::mem::transmute_copy(&(*c).vtable.0[index]) }
    }

    fn create(d: &ResourceDesc) -> This {
        let mut c = std::ptr::null_mut();
        assert_eq!(XGCreateTextureComputer(d, &mut c), S_OK);
        c
    }

    /// Element offsets: linear rows of 1 KiB (128 elements), 0 outside.
    #[test]
    fn element_offsets_follow_the_layout() {
        type Offset = extern "system" fn(This, u32, u32, u64, u32, u32, u32, *mut u32) -> u64;
        let c = create(&desc(128, 4, 1, 11, 0x100));
        let offset: Offset = slot(c, 6);
        let mut bit = 7;
        assert_eq!(offset(c, 0, 0, 3, 1, 0, 0, &mut bit), (128 + 3) * 8);
        assert_eq!(bit, 0);
        assert_eq!(offset(c, 0, 0, 128, 0, 0, 0, std::ptr::null_mut()), 0);
        slot::<extern "system" fn(This) -> u32>(c, 1)(c);
    }

    #[test]
    fn suggestions_follow_the_use() {
        let mut out = 0;
        let mut d = desc(256, 256, 1, 98, 0);
        assert_eq!(XGSuggestSwizzleMode(&d, 0, &mut out), S_OK);
        assert_eq!(out, 0x105);
        d.flags = ALLOW_RENDER_TARGET;
        XGSuggestSwizzleMode(&d, 0, &mut out);
        assert_eq!(out, 0x11b);
        d.flags = ALLOW_DEPTH_STENCIL;
        XGSuggestSwizzleMode(&d, 0, &mut out);
        assert_eq!(out, 0x118);
        // A 1D texture has no tiled layout, whatever its use.
        d.dimension = TEXTURE_1D;
        XGSuggestSwizzleMode(&d, 0, &mut out);
        assert_eq!(out, 0x100);
    }

    /// The title's calls: layout, then a copy in and out of each mip.
    #[test]
    fn layout_and_copies_through_the_object() {
        // 256x256 BC7, full chain, 64 KiB display mode.
        let c = create(&desc(256, 256, 0, 98, 0x10a));
        // SAFETY: plain data, filled by the call.
        let mut l: Box<LayoutOut> = Box::new(unsafe { std::mem::zeroed() });
        let get: extern "system" fn(This, *mut LayoutOut) -> HRESULT = slot(c, 2);
        assert_eq!(get(c, &mut *l), S_OK);
        assert_eq!(
            (l.size, l.alignment, l.mips, l.planes),
            (131072, 65536, 9, 1)
        );
        let p = &l.plane[0];
        assert_eq!((p.standard_mips, p.tail_mips, p.element_bytes), (1, 8, 16));
        assert_eq!((p.mips[0].offset, p.mips[0].pitch_bytes), (65536, 1024));
        assert_eq!((p.mips[1].offset, p.mips[1].in_tail), (32768, 1));

        type Into = extern "system" fn(This, *mut u8, u32, u32, *const u8, u32, u32) -> HRESULT;
        type From =
            extern "system" fn(This, *mut u8, u32, u32, u32, *const u8, u32, u32) -> HRESULT;
        let (into, from): (Into, From) = (slot(c, 8), slot(c, 9));
        let mut tiled = vec![0u8; l.size as usize];
        for mip in 0..9u32 {
            let m = &p.mips[mip as usize];
            let row = m.elements[0] * 16;
            let rows: Vec<u8> = (0..row * m.elements[1]).map(|i| (i ^ mip) as u8).collect();
            assert_eq!(
                into(c, tiled.as_mut_ptr(), 0, mip, rows.as_ptr(), row, 0),
                S_OK
            );
            let mut back = vec![0u8; rows.len()];
            assert_eq!(
                from(c, back.as_mut_ptr(), 0, mip, 0, tiled.as_ptr(), row, 0),
                S_OK
            );
            assert_eq!(back, rows, "mip {mip}");
        }
        // 0x50: one tile for the tail, one for mip 0.
        type Tiling = extern "system" fn(
            This,
            *mut u32,
            *mut PackedMips,
            *mut [u32; 3],
            *mut u32,
            u32,
            *mut SubresourceTiling,
        ) -> HRESULT;
        let tiling: Tiling = slot(c, 10);
        let (mut tiles, mut shape, mut n) = (0, [0; 3], 4);
        let mut packed = PackedMips {
            standard: 0,
            packed: 0,
            tiles: 0,
            start: 0,
        };
        let mut subs: Vec<SubresourceTiling> = (0..4)
            .map(|_| SubresourceTiling {
                width: 0,
                height: 0,
                depth: 0,
                start: 0,
            })
            .collect();
        assert_eq!(
            tiling(
                c,
                &mut tiles,
                &mut packed,
                &mut shape,
                &mut n,
                0,
                subs.as_mut_ptr()
            ),
            S_OK
        );
        assert_eq!(
            (tiles, packed.standard, packed.packed, packed.tiles),
            (2, 1, 8, 1)
        );
        assert_eq!((shape, n, subs[0].start), ([256, 256, 1], 1, 1));
        assert_eq!(release(c), 0);

        // 4 KiB modes have no 64 KiB tiling.
        let null = std::ptr::null_mut::<u8>();
        let c = create(&desc(256, 256, 0, 98, 0x105));
        assert_eq!(
            tiling(
                c,
                &mut tiles,
                null.cast(),
                null.cast(),
                null.cast(),
                0,
                null.cast()
            ),
            E_FAIL
        );
        assert_eq!(release(c), 0);
    }

    #[test]
    fn bad_descriptions_fail() {
        let mut c = std::ptr::null_mut();
        let bad = desc(256, 256, 1, 0, 0x10a);
        assert_eq!(XGCreateTextureComputer(&bad, &mut c), E_INVALIDARG);
        let bad = desc(256, 256, 1, 28, 0x103);
        assert_eq!(XGCreateTextureComputer(&bad, &mut c), E_INVALIDARG);
        assert!(module().exports.len() == 2);
    }

    #[test]
    fn tiling_handles_null_output_and_out_of_range_first() {
        type Tiling = extern "system" fn(
            This,
            *mut u32,
            *mut PackedMips,
            *mut [u32; 3],
            *mut u32,
            u32,
            *mut SubresourceTiling,
        ) -> HRESULT;
        let c = create(&desc(256, 256, 0, 98, 0x10a));
        let get_tiling: Tiling = slot(c, 10);

        let (mut tiles, mut shape, mut n) = (0, [0; 3], 4);
        let mut packed = PackedMips {
            standard: 0,
            packed: 0,
            tiles: 0,
            start: 99,
        };
        assert_eq!(
            get_tiling(
                c,
                &mut tiles,
                &mut packed,
                &mut shape,
                &mut n,
                0,
                std::ptr::null_mut(),
            ),
            S_OK
        );
        assert_eq!(
            (tiles, packed.standard, packed.packed, packed.tiles, packed.start, shape, n),
            (2, 1, 8, 1, 0, [256, 256, 1], 0)
        );

        let mut subs = [SubresourceTiling {
            width: 0,
            height: 0,
            depth: 0,
            start: 0xdead_beef,
        }];
        n = 1;
        assert_eq!(
            get_tiling(
                c,
                &mut tiles,
                &mut packed,
                &mut shape,
                &mut n,
                u32::MAX,
                subs.as_mut_ptr(),
            ),
            S_OK
        );
        assert_eq!((n, subs[0].start), (0, 0xdead_beef));
        assert_eq!(
            tiling(
                std::ptr::null_mut(),
                &mut tiles,
                &mut packed,
                &mut shape,
                &mut n,
                0,
                subs.as_mut_ptr(),
            ),
            E_INVALIDARG
        );
        assert_eq!(release(c), 0);
    }

    #[test]
    fn copy_rejects_invalid_subresources_pitches_overlap_and_bad_sample() {
        type Into = extern "system" fn(This, *mut u8, u32, u32, *const u8, u32, u32) -> HRESULT;
        type From =
            extern "system" fn(This, *mut u8, u32, u32, u32, *const u8, u32, u32) -> HRESULT;

        let mut aliased = desc(64, 64, 1, 28, 0);
        assert_eq!(
            XGSuggestSwizzleMode(&raw const aliased, 0, &raw mut aliased.layout),
            S_OK
        );
        assert_eq!(aliased.layout, 0x105);

        let c2d = create(&aliased);
        let (into, from): (Into, From) = (slot(c2d, 8), slot(c2d, 9));
        let mut tiled = vec![0u8; 16384];
        let mut linear = vec![0u8; 16384];
        let tiny = [0u8; 1];

        // Non-zero sample index on a single-sampled texture must fail.
        assert_eq!(
            from(c2d, linear.as_mut_ptr(), 0, 0, 1, tiled.as_ptr(), 256, 0),
            E_INVALIDARG
        );

        // Out-of-range subresource and short row pitch must fail before slicing `tiny`.
        assert_eq!(
            into(c2d, tiled.as_mut_ptr(), 0, 1, tiny.as_ptr(), 256, 0),
            E_INVALIDARG
        );
        assert_eq!(
            from(c2d, linear.as_mut_ptr(), 0, 1, 0, tiny.as_ptr(), 256, 0),
            E_INVALIDARG
        );
        assert_eq!(
            into(c2d, tiled.as_mut_ptr(), 0, 0, tiny.as_ptr(), 128, 0),
            E_INVALIDARG
        );
        assert_eq!(
            into(
                c2d,
                tiled.as_mut_ptr(),
                0,
                0,
                (usize::MAX - 7) as *const u8,
                256,
                0,
            ),
            E_INVALIDARG
        );

        // Overlapping tiled and linear ranges must fail instead of creating aliasing slices.
        assert_eq!(
            into(c2d, tiled.as_mut_ptr(), 0, 0, tiled.as_ptr(), 256, 0),
            E_INVALIDARG
        );
        assert_eq!(
            from(c2d, tiled.as_mut_ptr(), 0, 0, 0, tiled.as_ptr(), 256, 0),
            E_INVALIDARG
        );
        assert_eq!(
            into(
                c2d,
                tiled.as_mut_ptr(),
                0,
                0,
                unsafe { tiled.as_ptr().add(128) },
                256,
                0,
            ),
            E_INVALIDARG
        );

        // 3D textures only have slice 0 (subresource < mips) and require depth_pitch >= slice_bytes.
        let mut d3d = desc(32, 32, 1, 28, 0x100);
        d3d.dimension = TEXTURE_3D;
        d3d.depth_or_array = 4;
        let c3d = create(&d3d);
        let into3d: Into = slot(c3d, 8);
        let mut tiled3d = vec![0u8; 32768];
        assert_eq!(
            into3d(c3d, tiled3d.as_mut_ptr(), 0, 1, tiny.as_ptr(), 128, 4096),
            E_INVALIDARG
        );
        assert_eq!(
            into3d(c3d, tiled3d.as_mut_ptr(), 0, 0, tiny.as_ptr(), 128, 2048),
            E_INVALIDARG
        );
        assert_eq!(release(c3d), 0);

        // XOR render-target swizzle modes remain E_NOTIMPL without slicing `tiny`.
        let mut rt = desc(64, 64, 1, 28, 0x11b);
        rt.flags = ALLOW_RENDER_TARGET;
        let crt = create(&rt);
        let into_rt: Into = slot(crt, 8);
        assert_eq!(
            into_rt(crt, tiled.as_mut_ptr(), 0, 0, tiny.as_ptr(), 256, 0),
            E_NOTIMPL
        );
        assert_eq!(release(crt), 0);

        // Null `this` guards.
        assert_eq!(add_ref(std::ptr::null_mut()), 0);
        assert_eq!(release(std::ptr::null_mut()), 0);
        assert_eq!(layout(std::ptr::null_mut(), std::ptr::null_mut()), E_INVALIDARG);
        assert_eq!(
            copy_into(std::ptr::null_mut(), tiled.as_mut_ptr(), 0, 0, linear.as_ptr(), 256, 0),
            E_INVALIDARG
        );
        assert_eq!(
            copy_from(std::ptr::null_mut(), linear.as_mut_ptr(), 0, 0, 0, tiled.as_ptr(), 256, 0),
            E_INVALIDARG
        );
        let mut bit = 5;
        assert_eq!(
            element_offset(std::ptr::null_mut(), 0, 0, 0, 0, 0, 0, &mut bit),
            0
        );
        assert_eq!(bit, 0);

        assert_eq!(release(c2d), 0);
    }

    #[test]
    fn copy_into_and_from_coarse_mips_use_prefix_footprint_across_modes() {
        type Into = extern "system" fn(This, *mut u8, u32, u32, *const u8, u32, u32) -> HRESULT;
        type From =
            extern "system" fn(This, *mut u8, u32, u32, u32, *const u8, u32, u32) -> HRESULT;

        // 256x256 BC7 SW_64KB_D: full texture is 128 KiB, while mips 1..=8 live in the first 64 KiB tile.
        let c = create(&desc(256, 256, 0, 98, 0x10a));
        let (into, from): (Into, From) = (slot(c, 8), slot(c, 9));

        // Unaligned `layout()` output pointer must succeed without UB.
        let mut unaligned_buf = vec![0u64; (size_of::<LayoutOut>() + 16).div_ceil(8)];
        let unaligned_out = unsafe { unaligned_buf.as_mut_ptr().cast::<u8>().add(1) }.cast::<LayoutOut>();
        let get: extern "system" fn(This, *mut LayoutOut) -> HRESULT = slot(c, 2);
        assert_eq!(get(c, unaligned_out), S_OK);
        let read_layout = unsafe { unaligned_out.read_unaligned() };
        assert_eq!(read_layout.size, 131072);

        let mip1_row = 32 * 16;
        let mip1_src: Vec<u8> = (0..mip1_row * 32).map(|i| (i * 5 + 11) as u8).collect();

        // Place the linear source/destination immediately after the 64 KiB coarse-mip prefix in one arena.
        let prefix = 65536usize;
        let mut arena = vec![0u8; prefix + 65536];
        arena[prefix..prefix + mip1_src.len()].copy_from_slice(&mip1_src);
        let linear_after = unsafe { arena.as_mut_ptr().add(prefix) };

        assert_eq!(
            into(c, arena.as_mut_ptr(), 0, 1, linear_after.cast_const(), mip1_row as u32, 0),
            S_OK
        );
        arena[prefix..prefix + mip1_src.len()].fill(0);
        assert_eq!(
            from(c, linear_after, 0, 1, 0, arena.as_ptr(), mip1_row as u32, 0),
            S_OK
        );
        assert_eq!(&arena[prefix..prefix + mip1_src.len()], mip1_src.as_slice());

        // Mip 0 needs all 128 KiB of tiled memory, which overlaps `linear_after` at +64 KiB and must fail.
        assert_eq!(
            into(c, arena.as_mut_ptr(), 0, 0, linear_after.cast_const(), 1024, 0),
            E_INVALIDARG
        );
        assert_eq!(
            from(c, linear_after, 0, 0, 0, arena.as_ptr(), 1024, 0),
            E_INVALIDARG
        );
        assert_eq!(release(c), 0);

        // Sweep mips, formats, dimensions, and addressable swizzle modes to prove
        // `Layout::bytes_from(mip)` never underestimates any `copy_into` or `copy_from` span.
        let cases = [
            (TEXTURE_1D, 131u64, 1u32, 1u16, 28u32, 0x100u32),
            (3, 259, 175, 1, 61, 0x101),  // R8, SW_256B_S
            (3, 109, 71, 1, 71, 0x102),   // BC1, SW_256B_D
            (3, 300, 190, 1, 28, 0x105),  // RGBA8, SW_4KB_S
            (3, 250, 175, 1, 10, 0x106),  // RGBA16F, SW_4KB_D
            (3, 512, 300, 1, 98, 0x109),  // BC7, SW_64KB_S
            (3, 333, 211, 1, 2, 0x10a),   // RGBA32F, SW_64KB_D
            (3, 128, 96, 3, 98, 0x105),   // 2D array
            (TEXTURE_3D, 45, 29, 19, 28, 0x105), // 3D thick
        ];
        for (dim, w, h, d_or_a, fmt, swz) in cases {
            let mut d = desc(w, h, 0, fmt, swz);
            d.dimension = dim;
            d.depth_or_array = d_or_a;
            let comp = create(&d);
            let (into, from): (Into, From) = (slot(comp, 8), slot(comp, 9));
            let plane = unsafe { &(&*comp).texture.planes[0].layout };
            let slices = if dim == TEXTURE_3D { 1 } else { u32::from(d_or_a) };
            for slice in 0..slices {
                for mip in 0..plane.mips.len() as u32 {
                    let sub = slice * plane.mips.len() as u32 + mip;
                    let m = &plane.mips[mip as usize];
                    let row = m.width * plane.desc.element_bytes + 8;
                    let depth_pitch = row * m.height + 16;
                    let lin_len = (depth_pitch * m.depth) as usize;
                    let til_len = plane.bytes_from(mip) as usize;
                    let src: Vec<u8> = (0..lin_len).map(|i| (i * 13 + sub as usize) as u8).collect();
                    let mut arena = vec![0u8; til_len + lin_len];
                    arena[til_len..].copy_from_slice(&src);
                    let lin_ptr = unsafe { arena.as_mut_ptr().add(til_len) };
                    assert_eq!(
                        into(comp, arena.as_mut_ptr(), 0, sub, lin_ptr.cast_const(), row, depth_pitch),
                        S_OK,
                        "into dim={dim} fmt={fmt} swz={swz:#x} sub={sub}"
                    );
                    arena[til_len..].fill(0xcd);
                    assert_eq!(
                        from(comp, lin_ptr, 0, sub, 0, arena.as_ptr(), row, depth_pitch),
                        S_OK,
                        "from dim={dim} fmt={fmt} swz={swz:#x} sub={sub}"
                    );
                    let active = (m.width * plane.desc.element_bytes) as usize;
                    for z in 0..m.depth as usize {
                        for y in 0..m.height as usize {
                            let off = z * depth_pitch as usize + y * row as usize;
                            assert_eq!(
                                &arena[til_len + off..til_len + off + active],
                                &src[off..off + active]
                            );
                        }
                    }
                }
            }
            assert_eq!(release(comp), 0);
        }
    }
}
