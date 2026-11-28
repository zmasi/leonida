//! LOCAL (11/26): DLSS Frame Generation through NVIDIA Streamline (manual hooking).
//!
//! `LEONIDA_FRAME_GEN=2|3|4` (frames shown per rendered one; unset or 1: off) with the SDK's
//! signed DLLs in `LEONIDA_STREAMLINE_DIR` (sl.interposer, sl.common, sl.dlss_g, sl.reflex,
//! sl.pcl, nvngx_dlssg). NVIDIA's licence: for own use only, never pushed.
//!
//! - [`start`] (from the presenter, before its swap chain): `slInit`, `slSetD3DDevice`, Reflex
//!   low latency, and the factory upgraded to Streamline's proxy, so that the swap chain made
//!   from it is the proxy that generates frames at `Present`.
//! - [`tag`] (from the DLSS upscale, `crate::dlss::record`, or [`tag_plain`] from a stretched one):
//!   the frame's constants and its depth and motion vectors (handed over at the present,
//!   `eValidUntilPresent`). Both come from the title's FidelityFX dispatch, so frames are generated
//!   where the title upscales through it: Series S, and Series X with `LEONIDA_FSR_UPSCALER=1`
//!   (leonida-loader's `hooks.rs`). The title's own temporal upscaler hands nothing over: with it
//!   no frame is tagged and none is generated.
//! - [`present_start`] / [`present_end`] around the proxy's `Present`.
//!
//! The title gives no camera matrices: they are made from a 60 degree field of view, the
//! upscale's near/far and the render size, the camera standing still (its motion is in the
//! motion vectors). Frame indices are the presents so far: the upscale of a frame is recorded
//! before its present.

use std::ffi::CString;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::{Mutex, OnceLock};

use windows::Win32::Graphics::Direct3D12::*;
use windows::Win32::Graphics::Dxgi::IDXGIFactory2;
use windows::core::Interface;

use crate::streamline as sl;

/// Frames to generate per rendered frame (1 = 2x .. 5 = 6x), 0: off, as `LEONIDA_FRAME_GEN`
/// says now (the F1 menu changes it while running).
fn wanted_now() -> u32 {
    std::env::var("LEONIDA_FRAME_GEN").ok().and_then(|v| v.trim().parse::<u32>().ok()).map_or(0, |n| n.clamp(1, 6) - 1)
}

/// [`wanted_now`] at the start: whether Streamline is started at all (its proxy swap chain is
/// made once; with it, the amount and off/on change while running).
pub(crate) fn wanted() -> u32 {
    static N: OnceLock<u32> = OnceLock::new();
    *N.get_or_init(|| {
        let n = wanted_now();
        // Not together with the ray-tracing structure builder: each runs at about 15 frames a
        // second alone and the two at 0.1 (11/26 r_rtE/F/G: from the moment Streamline has its
        // first constants in the world, recording takes seconds a frame; Reflex off and no
        // generated frames change nothing). `LEONIDA_FG_WITH_RT=1` starts it anyway.
        let rt = std::env::var_os("LEONIDA_BVH_BUILDER").is_some_and(|v| v == "1");
        let forced = std::env::var_os("LEONIDA_FG_WITH_RT").is_some_and(|v| v == "1");
        if n > 0 && rt && !forced {
            tracing::warn!("frame generation: not started with ray tracing on (together they run at 0.1 frames a second); LEONIDA_FG_WITH_RT=1 starts it anyway");
            return 0;
        }
        n
    })
}

/// `LEONIDA_FG_REFLEX=0`: Reflex left off and its sleep not taken (to tell its cost from the rest).
fn reflex_on() -> bool {
    static ON: OnceLock<bool> = OnceLock::new();
    *ON.get_or_init(|| std::env::var_os("LEONIDA_FG_REFLEX").is_none_or(|v| v != "0"))
}

struct State {
    sl: sl::Sl,
    /// Frames to generate as last given to DLSS-G (0: turned off); none before the first present.
    set: Option<u32>,
    /// The most frames this card generates per rendered one, as DLSS-G says; 0: not asked yet.
    most: u32,
    told: u32,
}

static STATE: OnceLock<Option<Mutex<State>>> = OnceLock::new();
/// Presents so far: the index of the frame being made.
static FRAME: AtomicU32 = AtomicU32::new(1);
/// A frame was tagged since the last present.
static TAGGED: AtomicBool = AtomicBool::new(false);

const VIEWPORT: sl::ViewportHandle = sl::ViewportHandle::new(0);

fn state() -> Option<&'static Mutex<State>> {
    STATE.get().and_then(|s| s.as_ref())
}

fn init(device: &ID3D12Device) -> anyhow::Result<State> {
    let dir = std::env::var_os("LEONIDA_STREAMLINE_DIR")
        .ok_or_else(|| anyhow::anyhow!("LEONIDA_STREAMLINE_DIR is not set"))?;
    // SAFETY: NVIDIA's signed interposer from the SDK folder the launcher names.
    let mut s = unsafe { sl::Sl::load(std::path::Path::new(&dir)) }?;
    let features = sl::FG_FEATURES;
    static ENGINE: OnceLock<(CString, CString)> = OnceLock::new();
    let (version, project) = ENGINE.get_or_init(|| {
        (CString::new("1.0").unwrap_or_default(), CString::new("5e2f9a3c-1b7d-4c8e-9a6f-0d3b2c1e4f57").unwrap_or_default())
    });
    let logs: Vec<u16> = std::env::temp_dir().join("leonida-sl").as_os_str().encode_wide_z();
    let _ = std::fs::create_dir_all(std::env::temp_dir().join("leonida-sl"));
    // Streamline starts NGX for the process, and NGX looks for every feature in the folders it
    // was started with: with the plugins' folder alone, DLSS started afterwards
    // (`crate::dlss`, from its own folder) is "feature not found" (0xBAD00004; probe of
    // 2026-11-27, `dlss::tests`). So the folder of nvngx_dlss.dll is named here as well.
    let folders: Vec<Vec<u16>> = [Some(dir.clone()), std::env::var_os("LEONIDA_DLSS_DIR")]
        .into_iter()
        .flatten()
        .map(|d| d.as_os_str().encode_wide_z())
        .collect();
    let folder_pointers: Vec<*const u16> = folders.iter().map(|f| f.as_ptr()).collect();
    let pref = sl::Preferences {
        logLevel: sl::LogLevel::eDefault,
        pathsToPlugins: folder_pointers.as_ptr(),
        numPathsToPlugins: folder_pointers.len() as u32,
        pathToLogsAndData: logs.as_ptr(),
        logMessageCallback: Some(sl::log_to_tracing),
        flags: sl::PreferenceFlags::MANUAL_HOOKING_FRAME_TAGS,
        featuresToLoad: features.as_ptr(),
        numFeaturesToLoad: features.len() as u32,
        engine: sl::EngineType::eCustom,
        engineVersion: version.as_ptr(),
        projectId: project.as_ptr(),
        renderAPI: sl::RenderAPI::eD3D12,
        ..Default::default()
    };
    // SAFETY: every pointer in `pref` lives through the call (SL copies them); the log
    // callback is a static function.
    unsafe { s.init(&pref) }.ok().map_err(|e| anyhow::anyhow!("slInit: {e}"))?;
    // SAFETY: the host's live native device.
    unsafe { s.set_d3d_device(device.as_raw()) }.ok().map_err(|e| anyhow::anyhow!("slSetD3DDevice: {e}"))?;
    // SAFETY: a plain query of a live device.
    let luid = unsafe { device.GetAdapterLuid() };
    let mut bytes = [0u8; 8];
    bytes[..4].copy_from_slice(&luid.LowPart.to_le_bytes());
    bytes[4..].copy_from_slice(&luid.HighPart.to_le_bytes());
    let supported = s.is_feature_supported(sl::kFeatureDLSS_G, Some(&bytes));
    if !supported.is_ok() {
        anyhow::bail!("DLSS-G is not supported here: {supported}");
    }
    s.resolve_feature_functions()?;
    let reflex = sl::ReflexOptions {
        mode: if reflex_on() { sl::ReflexMode::eLowLatency } else { sl::ReflexMode::eOff },
        ..Default::default()
    };
    let r = s.reflex_set_options(&reflex);
    if !r.is_ok() {
        tracing::warn!(result = %r, "frame generation: Reflex not set");
    }
    Ok(State { sl: s, set: None, most: 0, told: 0 })
}

trait WideZ {
    fn encode_wide_z(&self) -> Vec<u16>;
}
impl WideZ for std::ffi::OsStr {
    fn encode_wide_z(&self) -> Vec<u16> {
        use std::os::windows::ffi::OsStrExt;
        self.encode_wide().chain([0]).collect()
    }
}

/// For `dlss::tests`: starts Streamline as [`start`] does (and leaves it started).
#[cfg(test)]
pub(crate) fn started_for_test(device: &ID3D12Device) -> bool {
    match init(device) {
        Ok(state) => {
            std::mem::forget(state);
            true
        }
        Err(e) => {
            println!("Streamline did not start: {e}");
            false
        }
    }
}

/// Before the presenter's swap chain is made: starts Streamline (once) and gives back the
/// factory to make it from, the proxy if frame generation is wanted and could start.
pub(crate) fn start(device: &ID3D12Device, factory: IDXGIFactory2) -> IDXGIFactory2 {
    if wanted() == 0 {
        return factory;
    }
    let state = STATE.get_or_init(|| match init(device) {
        Ok(s) => {
            tracing::info!(frames = wanted() + 1, "frame generation: Streamline started");
            Some(Mutex::new(s))
        }
        Err(e) => {
            tracing::warn!(error = %e, "frame generation: Streamline did not start; presenting without it");
            None
        }
    });
    let Some(state) = state else { return factory };
    let s = state.lock().unwrap_or_else(|e| e.into_inner());
    let mut raw = factory.as_raw();
    // SAFETY: a live factory; the call puts a new proxy (with its own reference) in `raw` and
    // leaves ours to the native one.
    let r = unsafe { s.sl.upgrade_interface(&mut raw) };
    if !r.is_ok() || raw.is_null() {
        tracing::warn!(result = %r, "frame generation: the factory was not upgraded");
        return factory;
    }
    // SAFETY: SL's proxy, an IDXGIFactory2 with one reference, now ours.
    unsafe { IDXGIFactory2::from_raw(raw) }
}

/// What the last DLSS upscale read: the frame's numbers and its depth and motion vectors (title
/// resources, whose host copies and states are looked up at the present).
struct Latest {
    depth: usize,
    motion: usize,
    render: [u32; 2],
    jitter: [f32; 2],
    mv_pixels: [f32; 2],
    near: f32,
    far: f32,
    reset: bool,
}

static LATEST: Mutex<Option<Latest>> = Mutex::new(None);

/// With frame generation on: the numbers of the DLSS upscale just recorded ([`crate::dlss::record`]).
/// The upscale of a frame is recorded after its present has been asked for (the lists are recorded
/// at submission), so everything is handed to Streamline at the next present, whose index is the
/// one the constants and tags carry.
#[allow(clippy::too_many_arguments)]
pub(crate) fn tag(render: [u32; 2], jitter: [f32; 2], mv_pixels: [f32; 2], near: f32, far: f32, reset: bool) {
    if state().is_none() {
        return;
    }
    let mut latest = LATEST.lock().unwrap_or_else(|e| e.into_inner());
    if latest.is_none() {
        tracing::info!(?render, ?jitter, ?mv_pixels, near, far, "frame generation: the first upscale's numbers (its depth and motion vectors are tagged from now on)");
    }
    // The first upscale after the G-buffer's were taken: its own resources come with `note_titles`.
    let first = !UPSCALE_TAGS.swap(true, Ordering::Relaxed);
    let (depth, motion) = latest.as_ref().filter(|_| !first).map_or((0, 0), |l| (l.depth, l.motion));
    *latest = Some(Latest { depth, motion, render, jitter, mv_pixels, near, far, reset });
    TAGGED.store(true, Ordering::Relaxed);
}

/// [`tag`] and [`note_titles`] for an upscale that was stretched, not made by DLSS (`crate::stretch`): the render
/// size is the title's, or the colour texture's (`colour`) when it gives none.
pub(crate) fn tag_plain(up: &crate::list::Upscale, colour: [u32; 2]) {
    let side = |asked: u32, held: u32| if asked == 0 { held } else { asked.min(held) };
    let scale = |s: f32| if s == 0.0 { 1.0 } else { s };
    tag(
        [side(up.render[0], colour[0]), side(up.render[1], colour[1])],
        up.jitter,
        [scale(up.motion_scale[0]), scale(up.motion_scale[1])],
        up.near,
        up.far,
        up.reset,
    );
    note_titles(up.depth, up.motion);
}

/// The title's depth and motion vectors the DLSS upscale read (`crate::list::upscale_now`).
pub(crate) fn note_titles(depth: u64, motion: u64) {
    if state().is_none() {
        return;
    }
    let mut latest = LATEST.lock().unwrap_or_else(|e| e.into_inner());
    if let Some(l) = latest.as_mut() {
        (l.depth, l.motion) = (depth as usize, motion as usize);
    }
}

/// An upscale has handed its numbers over ([`tag`] from the upscale's side): the G-buffer's are then left alone.
static UPSCALE_TAGS: AtomicBool = AtomicBool::new(false);

/// Whether the G-buffer pass's depth and velocity targets are frame generation's inputs where no upscale
/// hands any over (`LEONIDA_FG_GBUFFER=0`: not, as before).
fn gbuffer_on() -> bool {
    static ON: OnceLock<bool> = OnceLock::new();
    *ON.get_or_init(|| std::env::var_os("LEONIDA_FG_GBUFFER").is_none_or(|v| v != "0"))
}

/// With frame generation on and the title upscaling by itself (Series X: its own upscaler's passes hand
/// nothing over, and its FidelityFX path made to run there lights nothing: the scene colour stays black, dump
/// of 2026-11-27): the depth and the velocity target of the pass that fills the G-buffer
/// (`crate::draw::State::set_targets` finds it: four or more targets, one of them two 16-bit floats, with a
/// depth target of the same size). They are what the FidelityFX upscale is given on that path (the probe of
/// the same run: depth 2560x1440 format 19, motion vectors 2560x1440 format 34, in pixels, depth from 10000
/// down to 0.15), so its numbers are used: no jitter is known here, and none is told.
pub(crate) fn gbuffer(depth: u64, motion: u64, size: [u32; 2]) {
    if state().is_none() || !gbuffer_on() || UPSCALE_TAGS.load(Ordering::Relaxed) {
        return;
    }
    let mut latest = LATEST.lock().unwrap_or_else(|e| e.into_inner());
    if latest.is_none() {
        tracing::info!(
            ?size,
            depth = format_args!("{depth:#x}"),
            motion = format_args!("{motion:#x}"),
            "frame generation: depth and motion vectors are the G-buffer pass's from now on (LEONIDA_FG_GBUFFER=0: only an upscale's)"
        );
    }
    *latest = Some(Latest {
        depth: depth as usize,
        motion: motion as usize,
        render: size,
        jitter: [0.0; 2],
        mv_pixels: [1.0; 2],
        near: 10000.0,
        far: 0.15,
        reset: false,
    });
    TAGGED.store(true, Ordering::Relaxed);
}

/// Where the title's picture is in the swap chain's buffer when bars go around it (`crate::aspect`): left, top,
/// width, height. None: it is the whole buffer.
static PICTURE: Mutex<Option<[u32; 4]>> = Mutex::new(None);
/// A box was given at some time: from then on every present says where the picture is, the whole buffer too.
static BOXED: AtomicBool = AtomicBool::new(false);

/// From the presenter, before [`present_start`]: where in the buffer about to be presented the picture is.
pub(crate) fn picture_box(place: Option<[u32; 4]>) {
    if place.is_none() && !BOXED.load(Ordering::Relaxed) {
        return;
    }
    BOXED.store(true, Ordering::Relaxed);
    *PICTURE.lock().unwrap_or_else(|e| e.into_inner()) = place;
}

fn constants(l: &Latest) -> sl::Constants {
    let (w, h) = (l.render[0].max(1) as f32, l.render[1].max(1) as f32);
    let inverted = l.near > l.far;
    let (n, f) = (l.near.min(l.far).max(1e-3), l.near.max(l.far).min(1e6).max(l.near.min(l.far) + 1.0));
    let fov = 60f32.to_radians();
    let ys = 1.0 / (fov / 2.0).tan();
    let xs = ys / (w / h);
    let (c, d) = if inverted { (n / (n - f), -n * f / (n - f)) } else { (f / (f - n), -n * f / (f - n)) };
    sl::Constants {
        cameraViewToClip: sl::float4x4::from_rows([[xs, 0.0, 0.0, 0.0], [0.0, ys, 0.0, 0.0], [0.0, 0.0, c, 1.0], [0.0, 0.0, d, 0.0]]),
        clipToCameraView: sl::float4x4::from_rows([
            [1.0 / xs, 0.0, 0.0, 0.0],
            [0.0, 1.0 / ys, 0.0, 0.0],
            [0.0, 0.0, 0.0, 1.0 / d],
            [0.0, 0.0, 1.0, -c / d],
        ]),
        clipToPrevClip: sl::float4x4::identity(),
        prevClipToClip: sl::float4x4::identity(),
        jitterOffset: sl::float2::new(l.jitter[0], l.jitter[1]),
        mvecScale: sl::float2::new(l.mv_pixels[0] / w, l.mv_pixels[1] / h),
        cameraPinholeOffset: sl::float2::new(0.0, 0.0),
        cameraPos: sl::float3::new(0.0, 0.0, 0.0),
        cameraUp: sl::float3::new(0.0, 1.0, 0.0),
        cameraRight: sl::float3::new(1.0, 0.0, 0.0),
        cameraFwd: sl::float3::new(0.0, 0.0, 1.0),
        cameraNear: n,
        cameraFar: f,
        cameraFOV: fov,
        cameraAspectRatio: w / h,
        depthInverted: inverted.into(),
        cameraMotionIncluded: sl::Boolean::eTrue,
        motionVectors3D: sl::Boolean::eFalse,
        reset: l.reset.into(),
        motionVectorsJittered: sl::Boolean::eTrue,
        ..Default::default()
    }
}

/// Right before the proxy's `Present`: generation on (once), and for a frame the upscale ran in,
/// its constants and its depth and motion vectors (valid until this present: the next frame's
/// lists come after it on the queue).
pub(crate) fn present_start() {
    let Some(state) = state() else { return };
    let mut s = state.lock().unwrap_or_else(|e| e.into_inner());
    let index = FRAME.load(Ordering::Relaxed);
    // The amount, at the first present and whenever the menu changed it (looked at twice a second
    // or so): no restart, the proxy swap chain stays.
    if s.set.is_none() || index % 8 == 0 {
        let asked = wanted_now();
        // No more than the card and driver generate (DLSS 4.5 gives up to 5 here: 6x).
        // Asked once here (each question restarts Streamline's count of presented frames).
        if s.most == 0 {
            s.most = match s.sl.dlssg_get_state(&VIEWPORT, None) {
                Ok(st) if st.numFramesToGenerateMax > 0 => st.numFramesToGenerateMax,
                _ => 1,
            };
        }
        let want = asked.min(s.most);
        if s.set != Some(want) {
            if want != asked {
                tracing::warn!(asked = asked + 1, given = want + 1, "frame generation: more than this card generates; its most is used");
            }
            let options = sl::DLSSGOptions {
                mode: if want == 0 { sl::DLSSGMode::eOff } else { sl::DLSSGMode::eOn },
                numFramesToGenerate: want.max(1),
                flags: sl::DLSSGFlags::eRetainResourcesWhenOff,
                ..Default::default()
            };
            let r = s.sl.dlssg_set_options(&VIEWPORT, &options);
            tracing::info!(result = %r, frames = want + 1, "frame generation: amount set (1 = off)");
            s.set = Some(want);
            if want == 0 {
                SHOWN.store(1, Ordering::Relaxed);
            }
        }
    }
    let Ok(token) = s.sl.new_frame_token(Some(index)) else { return };
    for marker in [
        sl::PCLMarker::eSimulationStart,
        sl::PCLMarker::eSimulationEnd,
        sl::PCLMarker::eRenderSubmitStart,
        sl::PCLMarker::eRenderSubmitEnd,
    ] {
        let _ = s.sl.pcl_set_marker(marker, token);
    }
    // Every present carries the last upscale's numbers and buffers, a present with no new
    // upscale since the last one too (DLSS-G drops a frame that has none).
    let fresh = TAGGED.swap(false, Ordering::Relaxed);
    {
        let latest = LATEST.lock().unwrap_or_else(|e| e.into_inner());
        // Said once: generation is on and nothing has come from an upscale yet.
        if latest.is_none() && index == 600 {
            tracing::warn!(
                "frame generation: neither an upscale nor a G-buffer pass has handed over depth and motion vectors in 600 presents, and no frame is generated until one does (menus and loading screens have neither)"
            );
        }
        let found = latest.as_ref().and_then(|l| {
            // Only a title resource that is still one, and only its copy as it is: the handles
            // are a frame old at a present with no new upscale, and the title may have let the
            // resource go since (with ray tracing on it makes and frees buffers all the time);
            // `host_copy` would make a copy of whatever lies there now.
            let look = |title: usize| {
                let this = title as crate::object::This;
                if this.is_null() || !crate::resource::is_resource(this) {
                    return None;
                }
                let copy = crate::resource::made_copy(this)?;
                let state = crate::host::legal_state(&copy.texture, copy.state.load(Ordering::Relaxed));
                Some((copy, state))
            };
            Some((l, look(l.depth)?, look(l.motion)?))
        });
        if let Some((l, (depth, depth_state), (motion, motion_state))) = found {
            let r = s.sl.set_constants(&constants(l), token, &VIEWPORT);
            let mut depth_res = sl::Resource::d3d12_tex2d(depth.texture.as_raw(), depth_state);
            let mut motion_res = sl::Resource::d3d12_tex2d(motion.texture.as_raw(), motion_state);
            let extent = Some(sl::Extent { top: 0, left: 0, width: l.render[0], height: l.render[1] });
            let mut tags = vec![
                sl::ResourceTag::new(&mut depth_res, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, extent),
                sl::ResourceTag::new(&mut motion_res, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, extent),
            ];
            // With bars around the picture: the part of the back buffer the depth and motion vectors are of
            // (no resource, only the extent; all zero is the whole buffer again).
            if BOXED.load(Ordering::Relaxed) {
                let place = PICTURE.lock().unwrap_or_else(|e| e.into_inner()).map(|[left, top, width, height]| sl::Extent { top, left, width, height });
                tags.push(sl::ResourceTag::new(std::ptr::null_mut(), sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent, place));
            }
            // SAFETY: live resources (their host copies are held through the call), in the
            // states they will be in at the present; no list needed for eValidUntilPresent.
            let t = unsafe { s.sl.set_tag_for_frame(token, &VIEWPORT, &tags, std::ptr::null_mut()) };
            if (!r.is_ok() || !t.is_ok()) && s.told < 5 {
                s.told += 1;
                tracing::warn!(constants = %r, tags = %t, index, "frame generation: constants or tags refused");
            }
            PRESENT_TAGGED.store(fresh, Ordering::Relaxed);
        }
    }
    let _ = s.sl.pcl_set_marker(sl::PCLMarker::ePresentStart, token);
}

/// The present now ending had constants and tags.
static PRESENT_TAGGED: AtomicBool = AtomicBool::new(false);

/// Right after the proxy's `Present`: the next frame's index, Reflex's sleep, and now and then
/// what the generator says of itself.
pub(crate) fn present_end() {
    let Some(state) = state() else { return };
    let s = state.lock().unwrap_or_else(|e| e.into_inner());
    let index = FRAME.load(Ordering::Relaxed);
    if let Ok(token) = s.sl.new_frame_token(Some(index)) {
        let _ = s.sl.pcl_set_marker(sl::PCLMarker::ePresentEnd, token);
    }
    let tagged = PRESENT_TAGGED.swap(false, Ordering::Relaxed);
    // For the frame-rate texts: frames shown per rendered one, as Streamline says of the last
    // present (r_cgbj41: 1 over 30 presents with the window not focused, so not a running count;
    // DLSS-G generates nothing while the window is not focused).
    const POLL: u32 = 10;
    if index % POLL == 0 {
        match s.sl.dlssg_get_state(&VIEWPORT, None) {
            Ok(st) => {
                let per = if st.status.0 == 0 { st.numFramesActuallyPresented.clamp(1, 6) } else { 1 };
                SHOWN.store(per, Ordering::Relaxed);
                if index % 600 == 0 || (index / POLL).is_power_of_two() {
                    tracing::info!(
                        index,
                        tagged,
                        status = st.status.0,
                        presented = st.numFramesActuallyPresented,
                        per,
                        max = st.numFramesToGenerateMax,
                        "frame generation: state (status 0 = working; 2 Reflex, 8 constants)"
                    );
                }
            }
            Err(e) => {
                SHOWN.store(1, Ordering::Relaxed);
                if index % 600 == 0 {
                    tracing::warn!(error = %e, "frame generation: no state");
                }
            }
        }
    }
    let next = index + 1;
    FRAME.store(next, Ordering::Relaxed);
    // Reflex paces the next frame here.
    if reflex_on()
        && let Ok(token) = s.sl.new_frame_token(Some(next))
    {
        let _ = s.sl.reflex_sleep(token);
    }
}
/// Frames shown per frame rendered (1 without generation working), as Streamline last said.
static SHOWN: AtomicU32 = AtomicU32::new(1);

/// For the frame-rate texts: frames shown per frame rendered; 1 when generation is off or failing.
pub(crate) fn shown_per_frame() -> u32 {
    if state().is_some() { SHOWN.load(Ordering::Relaxed).max(1) } else { 1 }
}

/// Whether Streamline is presenting (the presenter's swap chain is its proxy).
pub(crate) fn active() -> bool {
    state().is_some()
}

