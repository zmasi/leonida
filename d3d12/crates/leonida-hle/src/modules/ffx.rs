//! `amd_fidelityfx_loader_xbox.dll`: the upscale dispatch.
//!
//! The title's Series S mode renders below its output size and calls the
//! console's FidelityFX upscaler to make the output. The loader and the
//! upscaler are the title's own DLLs and run as they are, but the
//! upscaler's passes are compute pipelines in a container we do not
//! translate: its output stayed black. `ffxDispatch` alone is taken here:
//! an upscale dispatch becomes a plain stretch of the colour input
//! (`leonida_gpu::list::upscale_stretch`); anything else, and an upscale we
//! could not stretch, goes to the title's DLL.
//!
//! The structures are the public FidelityFX API's (`ffx_api.h`,
//! `ffx_upscale.h`): a header of a type and a next pointer, and resources of
//! a pointer, a 32-byte description and a state.

use std::ffi::c_void;
use std::sync::atomic::{AtomicU32, AtomicUsize, Ordering};


/// The DLL whose `ffxDispatch` is taken.
pub const LOADER: &str = "amd_fidelityfx_loader_xbox.dll";

static REAL: AtomicUsize = AtomicUsize::new(0);

/// The title's own `ffxDispatch`, for what is not stretched here.
pub fn set_real_dispatch(address: usize) {
    REAL.store(address, Ordering::Relaxed);
}

#[repr(C)]
struct Header {
    kind: u64,
    next: *const Header,
}

#[repr(C)]
struct Resource {
    resource: *const c_void,
    description: [u32; 8],
    state: u32,
}

#[repr(C)]
struct Upscale {
    header: Header,
    list: *const c_void,
    colour: Resource,
    depth: Resource,
    motion: Resource,
    exposure: Resource,
    reactive: Resource,
    composition: Resource,
    output: Resource,
    jitter: [f32; 2],
    motion_scale: [f32; 2],
    render: [u32; 2],
    upscale: [u32; 2],
    // The rest of `ffxDispatchDescUpscale` (`ffx_upscale.h`), read by the
    // probe only.
    enable_sharpening: u8,
    sharpness: f32,
    frame_time_delta: f32,
    pre_exposure: f32,
    reset: u8,
    camera_near: f32,
    camera_far: f32,
    camera_fov: f32,
    view_to_meters: f32,
    flags: u32,
}

/// `LEONIDA_UPSCALE_PROBE=1`: logs what the title gives each upscale (the
/// inputs a temporal upscaler such as DLSS or FSR 2 needs), now and then.
fn probe() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| std::env::var_os("LEONIDA_UPSCALE_PROBE").is_some_and(|v| v == "1"))
}

const _: () = assert!(size_of::<Resource>() == 48);
const _: () = assert!(std::mem::offset_of!(Upscale, output) == 0x138);

/// `FFX_API_RETURN_ERROR`: what a dispatch nothing could answer returns.
const ERROR: u32 = 1;

/// `FFX_API_DISPATCH_DESC_TYPE_UPSCALE`.
const UPSCALE: u64 = 0x0001_0001;

type Dispatch = extern "system" fn(*mut *mut c_void, *const Header) -> u32;

extern "system" fn dispatch(context: *mut *mut c_void, desc: *const Header) -> u32 {
    // SAFETY: the title passes a dispatch description, whose header says
    // what follows it.
    if let Some(header) = unsafe { desc.as_ref() } {
        static SEEN: AtomicU32 = AtomicU32::new(0);
        let first = SEEN.fetch_add(1, Ordering::Relaxed) < 4;
        if header.kind == UPSCALE {
            // SAFETY: an upscale description, by its type.
            let d = unsafe { &*desc.cast::<Upscale>() };
            if first {
                tracing::info!(
                    list = ?d.list,
                    colour = ?d.colour.resource,
                    colour_size = ?(d.colour.description[2], d.colour.description[3]),
                    output = ?d.output.resource,
                    output_size = ?(d.output.description[2], d.output.description[3]),
                    render = ?d.render,
                    upscale = ?d.upscale,
                    "upscale dispatch"
                );
            }
            #[cfg(windows)]
            if probe() {
                static N: AtomicU32 = AtomicU32::new(0);
                let n = N.fetch_add(1, Ordering::Relaxed);
                // the first 12 upscales, then one in 300
                if n < 12 || n.is_multiple_of(300) {
                    let inputs = leonida_gpu::list::upscale_inputs(&[
                        d.colour.resource,
                        d.depth.resource,
                        d.motion.resource,
                        d.exposure.resource,
                        d.reactive.resource,
                        d.composition.resource,
                        d.output.resource,
                    ]);
                    tracing::info!(
                        n,
                        colour = %inputs[0],
                        depth = %inputs[1],
                        motion = %inputs[2],
                        exposure = %inputs[3],
                        reactive = %inputs[4],
                        composition = %inputs[5],
                        output = %inputs[6],
                        jitter = ?d.jitter,
                        motion_scale = ?d.motion_scale,
                        render = ?d.render,
                        upscale = ?d.upscale,
                        sharpening = d.enable_sharpening,
                        sharpness = d.sharpness,
                        frame_time_delta = d.frame_time_delta,
                        pre_exposure = d.pre_exposure,
                        reset = d.reset,
                        near = d.camera_near,
                        far = d.camera_far,
                        fov = d.camera_fov,
                        meters = d.view_to_meters,
                        flags = format_args!("{:#x}", d.flags),
                        "upscale probe"
                    );
                }
            }
            #[cfg(windows)]
            {
                let up = leonida_gpu::list::Upscale {
                    colour: d.colour.resource as u64,
                    depth: d.depth.resource as u64,
                    motion: d.motion.resource as u64,
                    exposure: d.exposure.resource as u64,
                    output: d.output.resource as u64,
                    render: d.render,
                    jitter: d.jitter,
                    motion_scale: d.motion_scale,
                    reset: d.reset != 0,
                    pre_exposure: d.pre_exposure,
                    near: d.camera_near,
                    far: d.camera_far,
                    frame_ms: d.frame_time_delta,
                    dlss: std::env::var_os("LEONIDA_UPSCALE").is_some_and(|v| v == "dlss"),
                };
                if leonida_gpu::list::upscale(d.list, &up) {
                    return 0;
                }
            }
            if first {
                tracing::warn!("upscale dispatch not stretched: left to the title's upscaler");
            }
        } else if first {
            tracing::info!(
                kind = format_args!("{:#x}", header.kind),
                "another FidelityFX dispatch"
            );
        }
    }
    match REAL.load(Ordering::Relaxed) {
        // Neither stretched nor passed on: the output was not written, and
        // the title is told so instead of presenting it as it is.
        0 => {
            static TOLD: std::sync::Once = std::sync::Once::new();
            TOLD.call_once(|| {
                tracing::error!(
                    "a FidelityFX dispatch was not answered: no stretch was recorded and the title's own ffxDispatch was not found"
                );
            });
            ERROR
        }
        // SAFETY: the title's own `ffxDispatch`, with its own arguments.
        real => unsafe { std::mem::transmute::<usize, Dispatch>(real)(context, desc) },
    }
}

static REAL_CREATE: AtomicUsize = AtomicUsize::new(0);

/// The title's own `ffxCreateContext`, which [`create`] calls.
pub fn set_real_create(address: usize) {
    REAL_CREATE.store(address, Ordering::Relaxed);
}

/// `ffxCreateContextDescUpscale` (`ffx_upscale.h`), up to its sizes.
#[repr(C)]
struct CreateUpscale {
    header: Header,
    flags: u32,
    max_render: [u32; 2],
    max_upscale: [u32; 2],
}

/// `FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE`.
const CREATE_UPSCALE: u64 = 0x0001_0000;

/// The one pair of sizes (render, upscale) the console's upscaler makes a context for: it
/// ships the tables of its fixed 1.5x mode only, and says so of any other ("Shipping FSR4
/// configuration mismatch: maxUpscaleSize must be 1920x1080; received 3840x2160", run of
/// 2026-11-27 with `LEONIDA_FSR_UPSCALER=1` on the Series X profile).
const SHIPPED: ([u32; 2], [u32; 2]) = ([1280, 720], [1920, 1080]);

type Create = extern "system" fn(*mut *mut c_void, *mut Header, *const c_void) -> u32;

/// The sizes a context is made with, where they are not [`SHIPPED`]: `None` for those the
/// DLL takes as they are, and for what does not read as two sizes.
fn sizes_to_stand_in(render: [u32; 2], upscale: [u32; 2]) -> Option<([u32; 2], [u32; 2])> {
    let size = |s: [u32; 2]| (1..=16384).contains(&s[0]) && (1..=16384).contains(&s[1]);
    (size(render) && size(upscale) && upscale != SHIPPED.1).then_some(SHIPPED)
}

/// `ffxCreateContext` while the dispatch is ours ([`dispatch`]: DLSS or the stretch).
///
/// On the Series X profile the title asks for a context of 2560x1440 to 3840x2160; the DLL
/// refuses it, the title goes on without one, its query of the jitter phase count answers
/// nothing, and its frame counter is divided by that zero right after the first upscale
/// (0xC0000094 at title+0x28f03e8). The context passes are never run here, only its queries
/// (the jitter sequence, which is arithmetic on the sizes each query names), so the DLL is
/// asked for the context it does make and the description of the title is put back.
extern "system" fn create(context: *mut *mut c_void, desc: *mut Header, memory: *const c_void) -> u32 {
    let real = match REAL_CREATE.load(Ordering::Relaxed) {
        0 => return ERROR,
        // SAFETY: the title's own `ffxCreateContext`, set from its DLL's export.
        real => unsafe { std::mem::transmute::<usize, Create>(real) },
    };
    // SAFETY: the title passes a creation description, whose header says what follows it.
    if let Some(header) = unsafe { desc.as_ref() }
        && header.kind == CREATE_UPSCALE
    {
        // SAFETY: an upscale context's description, by its type; the title's own memory,
        // which nothing else reads during its call.
        let d = unsafe { &mut *desc.cast::<CreateUpscale>() };
        let asked = (d.max_render, d.max_upscale);
        if let Some(stand_in) = sizes_to_stand_in(asked.0, asked.1) {
            (d.max_render, d.max_upscale) = stand_in;
            let result = real(context, desc, memory);
            (d.max_render, d.max_upscale) = asked;
            tracing::info!(
                render = ?asked.0,
                upscale = ?asked.1,
                made_for = ?stand_in,
                flags = format_args!("{:#x}", d.flags),
                result,
                "upscale context: the title's upscaler makes one of its own sizes; the upscale itself is ours"
            );
            return result;
        }
        tracing::info!(render = ?asked.0, upscale = ?asked.1, flags = format_args!("{:#x}", d.flags), "upscale context");
    }
    real(context, desc, memory)
}

/// What the title's import of `ffxCreateContext` is bound to while its dispatch is ours.
pub fn create_address() -> usize {
    create as *const () as usize
}

/// What the title's import of `ffxDispatch` is bound to. The DLL is not an
/// HLE module: the title's own must load, for its other entry points and
/// for the dispatches passed on.
pub fn dispatch_address() -> usize {
    dispatch as *const () as usize
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::mem::offset_of;

    /// The upscale description as the public API lays it out: what the
    /// stretch reads sits where the title wrote it.
    #[test]
    fn the_upscale_description_is_laid_out_as_the_public_api() {
        assert_eq!(size_of::<Header>(), 16);
        assert_eq!(offset_of!(Upscale, list), 0x10);
        assert_eq!(offset_of!(Upscale, colour), 0x18);
        assert_eq!(offset_of!(Upscale, depth), 0x48);
        assert_eq!(offset_of!(Upscale, motion), 0x78);
        assert_eq!(offset_of!(Upscale, output), 0x138);
        assert_eq!(offset_of!(Upscale, jitter), 0x168);
        assert_eq!(offset_of!(Upscale, motion_scale), 0x170);
        assert_eq!(offset_of!(Upscale, render), 0x178);
        assert_eq!(offset_of!(Upscale, upscale), 0x180);
        assert_eq!(offset_of!(Upscale, enable_sharpening), 0x188);
        assert_eq!(offset_of!(Upscale, sharpness), 0x18c);
        assert_eq!(offset_of!(Upscale, reset), 0x198);
        assert_eq!(offset_of!(Upscale, camera_near), 0x19c);
        assert_eq!(offset_of!(Upscale, flags), 0x1ac);
        assert_eq!(offset_of!(Resource, description), 8);
        assert_eq!(offset_of!(Resource, state), 40);
    }

    /// The sizes sit where the public API has them, and only a context the DLL would refuse
    /// is made with other sizes.
    #[test]
    fn a_context_of_other_sizes_is_made_with_the_shipped_ones() {
        assert_eq!(offset_of!(CreateUpscale, flags), 16);
        assert_eq!(offset_of!(CreateUpscale, max_render), 20);
        assert_eq!(offset_of!(CreateUpscale, max_upscale), 28);
        assert_eq!(sizes_to_stand_in([2560, 1440], [3840, 2160]), Some(SHIPPED));
        assert_eq!(sizes_to_stand_in([1280, 720], [1920, 1080]), None);
        assert_eq!(sizes_to_stand_in([960, 540], [1920, 1080]), None);
        // not sizes: left as it is
        assert_eq!(sizes_to_stand_in([0, 0], [3840, 2160]), None);
        assert_eq!(sizes_to_stand_in([2560, 1440], [0x7fff_0000, 2160]), None);
    }

    /// A dispatch that is not an upscale, with no DLL of the title's to
    /// pass it to, is an error and not a silent success.
    #[test]
    fn a_dispatch_nothing_answers_is_an_error() {
        let before = REAL.swap(0, Ordering::Relaxed);
        let other = Header {
            kind: 0x0002_0001,
            next: std::ptr::null(),
        };
        assert_eq!(dispatch(std::ptr::null_mut(), &other), ERROR);
        assert_eq!(dispatch(std::ptr::null_mut(), std::ptr::null()), ERROR);
        REAL.store(before, Ordering::Relaxed);
    }
}
