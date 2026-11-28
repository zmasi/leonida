//! NVIDIA DLSS in place of the title's FidelityFX upscale
//! (`LEONIDA_UPSCALE=dlss`, builds with the `dlss` feature): on Series S, where
//! the title upscales with FidelityFX by itself, and on Series X when the loader
//! makes it do so (`LEONIDA_FSR_UPSCALER=1`, leonida-loader's `hooks.rs`; there
//! the title's own temporal upscaler runs otherwise, as passes of its own that
//! hand nothing over).
//!
//! The title renders below its output size and asks its upscaler to make the
//! output from the colour, depth and motion vectors it rendered, with the
//! camera's jitter: what DLSS takes as well. The title's own FidelityFX passes
//! are slow translated for 32-lane cards (about 150 ms of card time a frame on
//! an RTX 3070 Ti); DLSS runs on the card's tensor cores in a millisecond or
//! two.
//!
//! The SDK (NVIDIA/DLSS on GitHub: `include`, `lib/Windows_x86_64`) is not part
//! of this repository: the build links `nvsdk_ngx_d.lib` from
//! `LEONIDA_DLSS_SDK`, and `nvngx_dlss.dll` is looked for in `LEONIDA_DLSS_DIR`
//! (else beside the executable). Any failure leaves the caller to stretch.
//!
//! Switches: `LEONIDA_DLSS_QUALITY` (0 performance, 1 balanced, 2 quality, 3
//! ultra performance, 5 DLAA; unset: by the sizes, `crate::list::dlss_mode`,
//! which the render scale decides: `LEONIDA_RENDER_SCALE`),
//! `LEONIDA_DLSS_MV_SIGN=-1` (motion vectors the other way),
//! `LEONIDA_DLSS_JITTER_SIGN=-1`, `LEONIDA_DLSS_HDR=0|1` (default: by the colour
//! format), `LEONIDA_DLSS_DEPTH_INVERTED=0|1` (default: by near > far),
//! `LEONIDA_DLSS_SHARPNESS=<0..1>`.

use std::ffi::{CString, c_char, c_int, c_uint, c_void};
use std::sync::Mutex;

use windows::Win32::Graphics::Direct3D12::*;
use windows::core::Interface;

type Result = c_int;
const SUCCESS: Result = 1;
const FEATURE_SUPER_SAMPLING: c_int = 1;
const ENGINE_CUSTOM: c_int = 0;
const VERSION_API: c_int = 0x15;

const FLAG_HDR: c_int = 1 << 0;
const FLAG_MV_LOW_RES: c_int = 1 << 1;
const FLAG_MV_JITTERED: c_int = 1 << 2;
const FLAG_DEPTH_INVERTED: c_int = 1 << 3;
const FLAG_AUTO_EXPOSURE: c_int = 1 << 6;

#[repr(C)]
struct PathList {
    path: *const *const u16,
    length: c_uint,
}

#[repr(C)]
struct Logging {
    callback: *const c_void,
    minimum: c_int,
    disable_other_sinks: bool,
}

#[repr(C)]
struct CommonInfo {
    paths: PathList,
    internal: *const c_void,
    logging: Logging,
}

#[link(name = "nvsdk_ngx_d", kind = "static")]
unsafe extern "C" {
    fn NVSDK_NGX_D3D12_Init_with_ProjectID(
        project: *const c_char,
        engine: c_int,
        engine_version: *const c_char,
        data_path: *const u16,
        device: *mut c_void,
        info: *const CommonInfo,
        version: c_int,
    ) -> Result;
    fn NVSDK_NGX_D3D12_GetCapabilityParameters(out: *mut *mut c_void) -> Result;
    fn NVSDK_NGX_D3D12_CreateFeature(list: *mut c_void, feature: c_int, params: *mut c_void, out: *mut *mut c_void) -> Result;
    fn NVSDK_NGX_D3D12_ReleaseFeature(handle: *mut c_void) -> Result;
    fn NVSDK_NGX_D3D12_EvaluateFeature_C(list: *mut c_void, handle: *const c_void, params: *const c_void, callback: *const c_void) -> Result;
    fn NVSDK_NGX_Parameter_SetUI(p: *mut c_void, name: *const c_char, value: c_uint);
    fn NVSDK_NGX_Parameter_SetI(p: *mut c_void, name: *const c_char, value: c_int);
    fn NVSDK_NGX_Parameter_SetF(p: *mut c_void, name: *const c_char, value: f32);
    fn NVSDK_NGX_Parameter_SetD3d12Resource(p: *mut c_void, name: *const c_char, value: *mut c_void);
    fn NVSDK_NGX_Parameter_GetI(p: *mut c_void, name: *const c_char, out: *mut c_int) -> Result;
}

/// A parameter name as the SDK spells it.
macro_rules! name {
    ($s:literal) => {
        concat!($s, "\0").as_ptr().cast::<c_char>()
    };
}

/// What the title gives an upscale, as the host's resources.
pub(crate) struct Inputs<'a> {
    pub colour: &'a ID3D12Resource,
    pub depth: Option<&'a ID3D12Resource>,
    pub motion: Option<&'a ID3D12Resource>,
    pub exposure: Option<&'a ID3D12Resource>,
    pub output: &'a ID3D12Resource,
    pub jitter: [f32; 2],
    pub motion_scale: [f32; 2],
    pub render: [u32; 2],
    pub reset: bool,
    pub pre_exposure: f32,
    pub near: f32,
    pub far: f32,
    pub frame_ms: f32,
}

struct Feature {
    handle: *mut c_void,
    /// render width and height the feature was made for, output size, flags
    key: [u32; 5],
}

struct Dlss {
    params: *mut c_void,
    feature: Option<Feature>,
}

// SAFETY: the SDK's objects are used under the mutex only.
unsafe impl Send for Dlss {}

/// `None` until tried; `Some(None)`: unavailable, never tried again.
static DLSS: Mutex<Option<Option<Dlss>>> = Mutex::new(None);

fn env_f(name: &str, default: f32) -> f32 {
    std::env::var(name).ok().and_then(|v| v.trim().parse().ok()).unwrap_or(default)
}

fn env_flag(name: &str) -> Option<bool> {
    std::env::var(name).ok().map(|v| v.trim() == "1")
}

fn wide(s: &std::ffi::OsStr) -> Vec<u16> {
    use std::os::windows::ffi::OsStrExt;
    s.encode_wide().chain([0]).collect()
}

fn init(device: &ID3D12Device) -> Option<Dlss> {
    let dir = std::env::var_os("LEONIDA_DLSS_DIR")
        .map(std::path::PathBuf::from)
        .or_else(|| std::env::current_exe().ok()?.parent().map(|p| p.to_path_buf()))?;
    let data = std::env::temp_dir().join("leonida-ngx");
    let _ = std::fs::create_dir_all(&data);
    let dir_w = wide(dir.as_os_str());
    let data_w = wide(data.as_os_str());
    let paths = [dir_w.as_ptr()];
    let info = CommonInfo {
        paths: PathList { path: paths.as_ptr(), length: 1 },
        internal: std::ptr::null(),
        logging: Logging { callback: std::ptr::null(), minimum: 0, disable_other_sinks: false },
    };
    let project = CString::new("5e2f9a3c-1b7d-4c8e-9a6f-0d3b2c1e4f57").ok()?;
    let version = CString::new("1.0").ok()?;
    // SAFETY: live strings and device; the SDK copies what it keeps.
    let r = unsafe {
        NVSDK_NGX_D3D12_Init_with_ProjectID(
            project.as_ptr(),
            ENGINE_CUSTOM,
            version.as_ptr(),
            data_w.as_ptr(),
            device.as_raw(),
            &info,
            VERSION_API,
        )
    };
    if r != SUCCESS {
        tracing::warn!(result = format_args!("{r:#x}"), dir = %dir.display(), "DLSS: the SDK did not start; the upscale stays a stretch");
        return None;
    }
    let mut params = std::ptr::null_mut();
    // SAFETY: an out pointer.
    let r = unsafe { NVSDK_NGX_D3D12_GetCapabilityParameters(&mut params) };
    if r != SUCCESS || params.is_null() {
        tracing::warn!(result = format_args!("{r:#x}"), "DLSS: no capability parameters");
        return None;
    }
    let mut available = 0;
    // SAFETY: live parameters.
    unsafe { NVSDK_NGX_Parameter_GetI(params, name!("SuperSampling.Available"), &mut available) };
    if available == 0 {
        // Why, in the SDK's own words: the result of loading the feature, and the driver it asks for.
        let (mut result, mut driver, mut major, mut minor) = (0, 0, 0, 0);
        // SAFETY: live parameters.
        unsafe {
            NVSDK_NGX_Parameter_GetI(params, name!("SuperSampling.FeatureInitResult"), &mut result);
            NVSDK_NGX_Parameter_GetI(params, name!("SuperSampling.NeedsUpdatedDriver"), &mut driver);
            NVSDK_NGX_Parameter_GetI(params, name!("SuperSampling.MinDriverVersionMajor"), &mut major);
            NVSDK_NGX_Parameter_GetI(params, name!("SuperSampling.MinDriverVersionMinor"), &mut minor);
        }
        tracing::warn!(
            feature_result = format_args!("{:#x}", result as u32),
            needs_updated_driver = driver,
            min_driver = format_args!("{major}.{minor}"),
            dir = %dir.display(),
            has_dll = dir.join("nvngx_dlss.dll").exists(),
            "DLSS: super sampling is not available on this card or driver"
        );
        #[cfg(test)]
        println!(
            "DLSS not available: feature result {:#x}, needs updated driver {driver}, min driver {major}.{minor}, dir {} (dll there: {})",
            result as u32,
            dir.display(),
            dir.join("nvngx_dlss.dll").exists()
        );
        return None;
    }
    tracing::info!(dir = %dir.display(), "DLSS: started; the title's upscale is DLSS (LEONIDA_UPSCALE=dlss)");
    Some(Dlss { params, feature: None })
}

/// Records DLSS over `inputs` into `list`; false if it could not (the caller
/// stretches instead). The list's descriptor heaps and root state are the
/// SDK's afterwards: the caller replays the title's.
pub(crate) fn record(device: &ID3D12Device, list: &ID3D12GraphicsCommandList, inputs: &Inputs<'_>) -> bool {
    let mut guard = DLSS.lock().unwrap_or_else(|e| e.into_inner());
    let dlss = match guard.get_or_insert_with(|| init(device)) {
        Some(d) => d,
        None => return false,
    };
    // SAFETY: live resources.
    let (out, colour) = unsafe { (inputs.output.GetDesc(), inputs.colour.GetDesc()) };
    let render = [
        if inputs.render[0] == 0 { colour.Width as u32 } else { inputs.render[0] },
        if inputs.render[1] == 0 { colour.Height } else { inputs.render[1] },
    ];
    // The feature's input size is the largest the colour texture holds, so
    // that a dynamic render size inside it needs no new feature.
    let (in_w, in_h) = (colour.Width as u32, colour.Height);
    let hdr = env_flag("LEONIDA_DLSS_HDR").unwrap_or(matches!(colour.Format.0, 2 | 10 | 26 | 24 | 9));
    let inverted = env_flag("LEONIDA_DLSS_DEPTH_INVERTED").unwrap_or(inputs.near > inputs.far);
    let mut flags = FLAG_MV_LOW_RES | FLAG_MV_JITTERED;
    if hdr {
        flags |= FLAG_HDR;
    }
    if inverted {
        flags |= FLAG_DEPTH_INVERTED;
    }
    if inputs.exposure.is_none() {
        flags |= FLAG_AUTO_EXPOSURE;
    }
    let key = [in_w, in_h, out.Width as u32, out.Height, flags as u32];
    let p = dlss.params;
    if dlss.feature.as_ref().is_none_or(|f| f.key != key) {
        if let Some(old) = dlss.feature.take() {
            // SAFETY: a handle the SDK made.
            unsafe { NVSDK_NGX_D3D12_ReleaseFeature(old.handle) };
        }
        // The mode: asked for, else the one whose ratio the sizes have (`crate::list::dlss_mode`).
        let quality = std::env::var("LEONIDA_DLSS_QUALITY")
            .ok()
            .and_then(|v| v.trim().parse::<f32>().ok())
            .map_or_else(|| crate::list::dlss_mode(render[1].min(in_h), out.Height), |q| q as c_int);
        let mut handle = std::ptr::null_mut();
        // SAFETY: live parameters and an open list.
        let r = unsafe {
            NVSDK_NGX_Parameter_SetUI(p, name!("CreationNodeMask"), 1);
            NVSDK_NGX_Parameter_SetUI(p, name!("VisibilityNodeMask"), 1);
            NVSDK_NGX_Parameter_SetUI(p, name!("Width"), in_w);
            NVSDK_NGX_Parameter_SetUI(p, name!("Height"), in_h);
            NVSDK_NGX_Parameter_SetUI(p, name!("OutWidth"), out.Width as u32);
            NVSDK_NGX_Parameter_SetUI(p, name!("OutHeight"), out.Height);
            NVSDK_NGX_Parameter_SetI(p, name!("PerfQualityValue"), quality);
            NVSDK_NGX_Parameter_SetI(p, name!("DLSS.Feature.Create.Flags"), flags);
            NVSDK_NGX_Parameter_SetI(p, name!("DLSS.Enable.Output.Subrects"), 0);
            // LOCAL (11/26): `LEONIDA_DLSS_PRESET=J|K|L|M`: the model, for every mode (unset:
            // the DLL's default; 310.9.16 uses K, the DLSS 4 transformer model, for Quality).
            let preset: c_uint = match std::env::var("LEONIDA_DLSS_PRESET").as_deref().map(str::trim) {
                Ok("J") => 10,
                Ok("K") => 11,
                Ok("L") => 12,
                Ok("M") => 13,
                _ => 0,
            };
            if preset != 0 {
                for mode in [
                    name!("DLSS.Hint.Render.Preset.DLAA"),
                    name!("DLSS.Hint.Render.Preset.Quality"),
                    name!("DLSS.Hint.Render.Preset.Balanced"),
                    name!("DLSS.Hint.Render.Preset.Performance"),
                    name!("DLSS.Hint.Render.Preset.UltraPerformance"),
                    name!("DLSS.Hint.Render.Preset.UltraQuality"),
                ] {
                    NVSDK_NGX_Parameter_SetUI(p, mode, preset);
                }
            }
            NVSDK_NGX_D3D12_CreateFeature(list.as_raw(), FEATURE_SUPER_SAMPLING, p, &mut handle)
        };
        if r != SUCCESS || handle.is_null() {
            tracing::warn!(result = format_args!("{r:#x}"), ?key, "DLSS: no feature for these sizes; the upscale stays a stretch");
            *guard = Some(None);
            return false;
        }
        tracing::info!(
            input = ?(in_w, in_h),
            output = ?(out.Width, out.Height),
            hdr,
            depth_inverted = inverted,
            auto_exposure = inputs.exposure.is_none(),
            quality,
            colour_format = colour.Format.0,
            "DLSS: feature made"
        );
        dlss.feature = Some(Feature { handle, key });
    }
    let Some(feature) = dlss.feature.as_ref() else { return false };
    let mv = env_f("LEONIDA_DLSS_MV_SIGN", 1.0);
    let jit = env_f("LEONIDA_DLSS_JITTER_SIGN", 1.0);
    let raw = |r: Option<&ID3D12Resource>| r.map_or(std::ptr::null_mut(), |r| r.as_raw());
    let scale = |s: f32| if s == 0.0 { 1.0 } else { s };
    // SAFETY: live parameters, resources and feature; an open list.
    let r = unsafe {
        NVSDK_NGX_Parameter_SetD3d12Resource(p, name!("Color"), inputs.colour.as_raw());
        NVSDK_NGX_Parameter_SetD3d12Resource(p, name!("Output"), inputs.output.as_raw());
        NVSDK_NGX_Parameter_SetD3d12Resource(p, name!("Depth"), raw(inputs.depth));
        NVSDK_NGX_Parameter_SetD3d12Resource(p, name!("MotionVectors"), raw(inputs.motion));
        NVSDK_NGX_Parameter_SetD3d12Resource(p, name!("ExposureTexture"), raw(inputs.exposure));
        NVSDK_NGX_Parameter_SetF(p, name!("Jitter.Offset.X"), jit * inputs.jitter[0]);
        NVSDK_NGX_Parameter_SetF(p, name!("Jitter.Offset.Y"), jit * inputs.jitter[1]);
        NVSDK_NGX_Parameter_SetF(p, name!("MV.Scale.X"), mv * scale(inputs.motion_scale[0]));
        NVSDK_NGX_Parameter_SetF(p, name!("MV.Scale.Y"), mv * scale(inputs.motion_scale[1]));
        NVSDK_NGX_Parameter_SetI(p, name!("Reset"), inputs.reset as c_int);
        NVSDK_NGX_Parameter_SetF(p, name!("Sharpness"), env_f("LEONIDA_DLSS_SHARPNESS", 0.0));
        NVSDK_NGX_Parameter_SetF(p, name!("DLSS.Pre.Exposure"), if inputs.pre_exposure > 0.0 { inputs.pre_exposure } else { 1.0 });
        NVSDK_NGX_Parameter_SetF(p, name!("FrameTimeDeltaInMsec"), inputs.frame_ms);
        NVSDK_NGX_Parameter_SetUI(p, name!("DLSS.Render.Subrect.Dimensions.Width"), render[0].min(in_w));
        NVSDK_NGX_Parameter_SetUI(p, name!("DLSS.Render.Subrect.Dimensions.Height"), render[1].min(in_h));
        NVSDK_NGX_D3D12_EvaluateFeature_C(list.as_raw(), feature.handle, p, std::ptr::null())
    };
    if r != SUCCESS {
        static TOLD: std::sync::Once = std::sync::Once::new();
        TOLD.call_once(|| tracing::warn!(result = format_args!("{r:#x}"), "DLSS: an evaluation failed; that upscale is a stretch"));
        return false;
    }
    // LOCAL (11/26): DLSS Frame Generation's inputs (`LEONIDA_FRAME_GEN`), in the state the
    // upscale read them in.
    if crate::fg::active() {
        crate::fg::tag(
            [render[0].min(in_w), render[1].min(in_h)],
            [jit * inputs.jitter[0], jit * inputs.jitter[1]],
            [mv * scale(inputs.motion_scale[0]), mv * scale(inputs.motion_scale[1])],
            inputs.near,
            inputs.far,
            inputs.reset,
        );
    }
    true
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Whether DLSS starts on this machine, alone or (with `LEONIDA_PROBE_STREAMLINE=1`,
    /// `LEONIDA_FRAME_GEN` and `LEONIDA_STREAMLINE_DIR`) after Streamline has started, as in the
    /// game with frame generation. Needs an NVIDIA card and `LEONIDA_DLSS_DIR`:
    /// `cargo test -p leonida-gpu --features dlss -- --ignored dlss_starts --nocapture`.
    #[test]
    #[ignore = "needs an NVIDIA card and the SDK files"]
    fn dlss_starts_on_this_card() {
        use windows::Win32::Graphics::Direct3D::D3D_FEATURE_LEVEL_12_0;
        use windows::Win32::Graphics::Direct3D12::D3D12CreateDevice;
        let mut device: Option<ID3D12Device> = None;
        // SAFETY: an out pointer for the default adapter's device.
        unsafe { D3D12CreateDevice(None, D3D_FEATURE_LEVEL_12_0, &mut device) }.expect("a D3D12 device");
        let device = device.expect("a device");
        if std::env::var_os("LEONIDA_PROBE_STREAMLINE").is_some() {
            println!("Streamline started first: {}", crate::fg::started_for_test(&device));
        }
        println!("DLSS available: {}", init(&device).is_some());
    }
}
