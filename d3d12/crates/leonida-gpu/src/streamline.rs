//! NVIDIA Streamline (SL) 2.14.16 FFI: DLSS Frame Generation (DLSS-G), Reflex and PC Latency (PCL).
//!
//! Hand-written mirrors of the SDK headers (`sl.h`, `sl_struct.h`, `sl_core_types.h`,
//! `sl_core_api.h`, `sl_consts.h`, `sl_dlss_g.h`, `sl_reflex.h`, `sl_pcl.h`, `sl_version.h`,
//! `sl_result.h`, `sl_appidentity.h`, `sl_device_wrappers.h`) plus a runtime loader for
//! `sl.interposer.dll`. Nothing links against `sl.interposer.lib`; every core entry point is
//! resolved with `GetProcAddress`, every feature entry point with `slGetFeatureFunction`.
//! Layouts are x64 MSVC and are checked by the `const` asserts at the bottom of the file.
//!
//! The headers mirrored here are NVIDIA's, published under the MIT licence
//! (github.com/NVIDIA-RTX/Streamline, `license.txt`), whose notice follows:
//!
//! ```text
//! Copyright (c) 2023 NVIDIA CORPORATION. All rights reserved
//!
//! Permission is hereby granted, free of charge, to any person obtaining a copy
//! of this software and associated documentation files (the "Software"), to deal
//! in the Software without restriction, including without limitation the rights
//! to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//! copies of the Software, and to permit persons to whom the Software is
//! furnished to do so, subject to the following conditions:
//!
//! The above copyright notice and this permission notice shall be included in all
//! copies or substantial portions of the Software.
//!
//! THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//! IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//! FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//! AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//! LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//! OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
//! SOFTWARE.
//! ```
//!
//! # Call sequence for a D3D12 app using MANUAL HOOKING with DLSS-G
//!
//! (from ProgrammingGuide.md, ProgrammingGuideManualHooking.md, ProgrammingGuideDLSS_G.md,
//! ProgrammingGuideReflex.md, ProgrammingGuidePCL.md, cross-checked against the interposer source)
//!
//! ## Start-up
//!
//! 1. `Sl::load(dir)` (LoadLibraryW of `<dir>\sl.interposer.dll` by full path).
//! 2. `slInit` before any hooked DXGI call can happen (before swap-chain creation). Not from
//!    `DllMain`. With manual hooking the D3D12 device may be created before or after `slInit`.
//!    * `flags = eDisableCLStateTracking | eUseManualHooking | eUseFrameBasedResourceTagging`
//!      (`PreferenceFlags::MANUAL_HOOKING_FRAME_TAGS`). `eUseFrameBasedResourceTagging` is
//!      required to use `slSetTagForFrame`. The C++ default also has `eAllowOTA |
//!      eLoadDownloadedPlugins` (internet updates of SL/NGX); add them back if wanted.
//!    * `featuresToLoad = [kFeatureDLSS_G, kFeatureReflex]` (`FG_FEATURES`). sl.dlss_g's manifest
//!      lists `required_plugins: ["sl.common", "sl.reflex"]`. `sl.common` and `sl.pcl` are always
//!      added by the plugin manager (`kFeatureCommon`, `kFeaturePCL`), they need not be listed.
//!      Add `kFeatureDLSS` only if DLSS-SR is also driven through SL.
//!    * `renderAPI = eD3D12`, `engine = eCustom`.
//!    * App identity: without an NVIDIA application id, `applicationId = 0` makes SL use
//!      `kTemporaryAppId` (100721531, `source/core/sl.api/internal.h`). In the signed
//!      (SL_PRODUCTION) build sl.common then REFUSES to start NGX ("Please provide correct
//!      application id ... NGX based features will be disabled") unless BOTH `engineVersion` and
//!      `projectId` are non-empty, in which case it uses `NVSDK_NGX_D3D12_Init_with_ProjectID`.
//!      DLSS-G is NGX-based, so set `engine = eCustom`, `engineVersion = c"..."` and
//!      `projectId = c"<a GUID string>"` (see `commonEntry.cpp`, `hasProjectId`).
//!    * `pathsToPlugins`: by default plugins are searched next to `sl.interposer.dll` itself
//!      (`file::getModulePath()`), not next to the exe. Give an absolute path to override.
//!    * Pass `sdkVersion = kSDKVersion`.
//! 3. Create the native `ID3D12Device`, then `slSetD3DDevice(nativeDevice)` "IMMEDIATELY after the
//!    main device is created" (with manual hooking this is what initialises the plugins).
//! 4. `slIsFeatureSupported(kFeatureDLSS_G / kFeatureReflex / kFeaturePCL, AdapterInfo{LUID})`,
//!    optionally `slGetFeatureRequirements` (OS/driver, `eHardwareSchedulingRequired`, ...).
//! 5. `Sl::resolve_feature_functions()` - `slGetFeatureFunction` only works AFTER the device is set.
//! 6. Proxies (sl_hooks.h list; everything NOT in that list must use native interfaces):
//!    * `slUpgradeInterface(&device)` -> proxy `ID3D12Device`. Create the PRESENT queue through
//!      the proxy (`eID3D12Device_CreateCommandQueue` is "Mandatory" and sl.dlss_g's embedded
//!      manifest hooks `ID3D12Device::CreateCommandQueue`); the result is a proxy queue. The
//!      interposer's factory proxy also accepts a native queue but logs "Detected base interface
//!      'ID3D12CommandQueue' while expecting SL proxy" when a plugin wants the proxy.
//!      `ExecuteCommandLists` etc. may go to the native queue (`slGetNativeInterface`).
//!    * `CreateDXGIFactory2` (native) then `slUpgradeInterface(&factory)` BEFORE
//!      `CreateSwapChainForHwnd`; call `CreateSwapChainForHwnd(proxyQueue, ...)` on the PROXY
//!      factory. The returned `IDXGISwapChain1` is the SL proxy (needs IDXGISwapChain3 support).
//!    * Route `Present`, `Present1`, `GetBuffer`, `GetDesc`, `ResizeBuffers`, `ResizeBuffers1`,
//!      `GetCurrentBackBufferIndex`, `SetFullscreenState` through the proxy swap chain; other
//!      swap-chain calls may use `slGetNativeInterface(proxySwapChain)`. Calling
//!      `GetCurrentBackBufferIndex` on the proxy is mandatory (`eFailGetCurrentBackBufferIndexNotCalled`).
//!      With DLSS-G loaded the proxy's back buffers are off-screen copies owned by SL.
//!    * Ref counts: `slUpgradeInterface` replaces the pointer in place with a NEW proxy object
//!      (ref count 1) that AddRefs the base; the caller still owns its original native ref.
//!      `slGetNativeInterface` returns an AddRef'd native pointer.
//!    * Use native interfaces for NVAPI / 3rd party SDKs.
//!    * Presenting through the proxy is also what makes sl.common's `presentCommon()` run once per
//!      frame (garbage collection; memory grows across resizes if it does not run).
//! 7. `slReflexSetOptions(ReflexOptions{ mode: eLowLatency, .. })` at least once (even if Reflex is
//!    "off"). DLSS-G reports `eFailReflexNotDetectedAtRuntime` when Reflex is not ACTIVE while
//!    DLSS-G is on, so keep mode `eLowLatency`/`eLowLatencyWithBoost` while FG is on.
//! 8. `slDLSSGSetOptions(viewport, DLSSGOptions{ mode: eOn, numFramesToGenerate: 1, .. })`;
//!    takes effect at the next `Present`; call it on the presenting thread. Loading the plugin
//!    and tagging does NOT enable interpolation by itself. `numFramesToGenerate` (1 = 2x) must
//!    be <= `DLSSGState::numFramesToGenerateMax` (from `slDLSSGGetState`). Recommended flag:
//!    `eRetainResourcesWhenOff` (no realloc stutter when toggling off/on).
//!
//! ## Per frame (one `FrameToken`, one viewport; no `slEvaluateFeature` for DLSS-G - Present is the marker)
//!
//! 1. `slGetNewFrameToken(&token, Some(frameIndex))` where the simulation of the frame starts.
//! 2. `slReflexSleep(token)` (always, regardless of Reflex mode), then PCL markers with the same
//!    token: `eSimulationStart` .. `eSimulationEnd`, `eRenderSubmitStart` .. `eRenderSubmitEnd`,
//!    `ePresentStart` / proxy `Present` / `ePresentEnd`. The index on the present markers must be
//!    the index of the frame being presented and match the one used for `slSetConstants`
//!    (otherwise "common constants cannot be found for frame N"). Optional: `ePCLatencyPing` when
//!    the `PCLState::statsWindowMessage` window message arrives, `eTriggerFlash` on left click.
//! 3. `slSetConstants(consts, token, viewport)` ONCE per frame, as early as possible. All
//!    non-optional fields must be valid (defaults are `INVALID_FLOAT` / `Boolean::eInvalid`):
//!    `cameraViewToClip`, `clipToCameraView`, `clipToPrevClip`, `prevClipToClip` (row-major, NO
//!    jitter), `jitterOffset` (pixels), `mvecScale` (scales MV to [-1,1]; `1/renderWidth,
//!    1/renderHeight` for pixel-space MVs), `cameraPos/Up/Right/Fwd` (Up/Right/Fwd must be
//!    orthonormal or auto scene-change detection disables itself), `cameraNear/Far/FOV(radians)/
//!    AspectRatio`, `depthInverted`, `cameraMotionIncluded`, `motionVectors3D`, `reset`.
//!    Optional: `clipToLensClip`, `cameraPinholeOffset`, `motionVectorsInvalidValue` (only if
//!    camera motion is not included). Invalid constants -> `eFailCommonConstantsInvalid`.
//! 4. `slSetTagForFrame(token, viewport, tags, cmdList)`:
//!    * Required: `kBufferTypeDepth` (or `kBufferTypeLinearDepth`) and `kBufferTypeMotionVectors`
//!      (dense: camera + object motion). The final colour comes from the proxy swap chain.
//!    * Strongly recommended for UI quality: `kBufferTypeHUDLessColor` (same size / colour space /
//!      post-processing as the back buffer, no UI) plus `kBufferTypeUIAlpha` (preferred) or
//!      `kBufferTypeUIColorAndAlpha` (premultiplied; alpha precision > 2 bits). Sizes must
//!      exactly match the back buffer (or its extent).
//!    * `kBufferTypeBackbuffer` only to pass a sub-rect (resource pointer may be null).
//!    * Lifecycle: tagged buffers are consumed during `Present`. If the resource is untouched
//!      until Present use `eValidUntilPresent` (cmdList may be null; `Resource::state` must be
//!      the state at Present time; SL keeps a reference until a null tag is set). If it will be
//!      overwritten/reused before Present (typical when tagging on a command list right after
//!      rendering a transient target) use `eOnlyValidNow` with that command list: SL records a
//!      copy on it, `state` must be the state the resource is in ON THAT LIST at that point, and
//!      the producing GPU work must already be on that list or on one executed before it.
//!      NVIDIA advises trying `eValidUntilPresent` first (copies cost VRAM).
//!    * Manual hooking: `Resource::state` (D3D12_RESOURCE_STATES) is mandatory - SL cannot track
//!      it. No transitions are needed; SL restores states.
//!    * Loading/menus/cut-scenes: set the tags to null (`resource = null`) and/or turn DLSS-G off.
//! 5. Viewport: an app-chosen integer (`ViewportHandle::new(0)`; the checklist says "Tag buffers
//!    with unique id 0"); the SAME handle for constants, tags, `slDLSSGSetOptions/GetState`.
//! 6. Optionally `slDLSSGGetState(viewport, null options)` after Present: `status` must be `eOk`
//!    (else turn FG off; an invalid FG draws a pink overlay), `numFramesActuallyPresented`,
//!    `minWidthOrHeight` (FG must be off below it). Never pass `eRequestVRAMEstimate` per frame.
//!
//! ## Swap chain, resize, auto-disable
//!
//! * Docs do not prescribe a swap effect / buffer count / flags beyond D3D12's flip model and the
//!   IDXGISwapChain3 requirement of the interposer proxy. `DLSSGOptions::numBackBuffers` is optional.
//! * Frame latency: SL's internal swap chain always uses
//!   `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT`. If the app does NOT request that flag
//!   (recommended) it must not call `GetFrameLatencyWaitableObject` / `SetMaximumFrameLatency`
//!   (applies whenever sl.dlss_g is loaded, even with FG off); pace with `slReflexSleep` /
//!   `ReflexOptions::frameLimitUs`. If it requests the flag it owns the waitable.
//! * HDR: only UINT10/RGB10 + HDR10/BT.2100. FP16 (R16G16B16A16_FLOAT) back buffers / scRGB are
//!   NOT supported (`eFailHDRFormatNotSupported`).
//! * VSync: `SyncInterval` 0 or 1 only (D3D12), check `DLSSGState::bIsVsyncSupportAvailable`.
//! * Turn FG off (`mode = eOff`) before resizing, minimise/maximise, full-screen transitions or any
//!   operation that could deadlock `Present` (FG presents on its own thread), and in pause/full
//!   screen menus. Recreate the swap chain whenever FG is switched on/off (and use
//!   `slSetFeatureLoaded(kFeatureDLSS_G, false)` + recreate to remove the off-screen copy and
//!   queue sync overhead while off). `slSetFeatureLoaded` = unload/unhook, not on/off.
//! * Auto-off: `eAuto` mode disables FG when it would lower FPS; `eEnableFullscreenMenuDetection`
//!   (needs `kBufferTypeUIColorAndAlpha`) turns it off in full-screen menus; FG is unusable when
//!   a `DLSSGStatus` failure bit is set (resolution too low, Reflex not active, HDR format,
//!   invalid constants, GetCurrentBackBufferIndex not called).
//! * OS: Windows 10 20H1 (build 19041+) and Hardware-accelerated GPU Scheduling ON.
//! * Multiple swap chains are not supported by DLSS-G; with manual hooking only the upgraded one
//!   is attached.
//! * `slShutdown()` before releasing the DXGI/D3D12 objects.
//!
//! ## DLLs (all from `sdk\bin\x64`, same directory)
//!
//! `sl.interposer.dll`, `sl.common.dll` (mandatory), `sl.dlss_g.dll` + `nvngx_dlssg.dll`,
//! `sl.reflex.dll`, `sl.pcl.dll`. (`sl.dlss.dll` + `nvngx_dlss.dll` only for DLSS-SR,
//! `NvLowLatencyVk.dll` only for Vulkan.) `sdk\bin\x64\development` holds unsigned dev builds.
//!
//! ## Signatures
//!
//! The production `sl.interposer.dll` loads every plugin through `sl::security::loadLibrary`,
//! which under `SL_PRODUCTION` calls `verifyEmbeddedSignature` (WinVerifyTrust + NVIDIA's second
//! signature) on the plugin's full path and silently refuses unsigned modules. There is no
//! location check: NVIDIA-signed plugins load from any folder (next to the interposer or
//! `pathsToPlugins`). Mixing in the unsigned `development` plugins with the production
//! interposer will make them fail to load. The docs ask the HOST to verify the signature of
//! `sl.interposer.dll` before loading it; this module does not (that needs WinTrust bindings).
//! OTA-downloaded plugins are only loaded by production builds with `eLoadDownloadedPlugins`.

#![allow(
    dead_code,
    non_snake_case,
    non_camel_case_types,
    non_upper_case_globals,
    clippy::missing_safety_doc,
    clippy::too_many_arguments
)]

use core::ffi::{CStr, c_char, c_void};
use core::ptr::{self, NonNull};
use std::os::windows::ffi::OsStrExt;
use std::path::{Path, PathBuf};

use anyhow::{Context, anyhow, bail};
use windows::Win32::Foundation::HMODULE;
use windows::Win32::System::LibraryLoader::{GetProcAddress, LoadLibraryW};
use windows::core::{PCSTR, PCWSTR};

// ---------------------------------------------------------------------------------------------
// Versions (sl_version.h, sl_struct.h)
// ---------------------------------------------------------------------------------------------

pub const SL_VERSION_MAJOR: u64 = 2;
pub const SL_VERSION_MINOR: u64 = 14;
pub const SL_VERSION_PATCH: u64 = 16;
pub const kSDKVersionMagic: u64 = 0xfedc;
/// `sl::kSDKVersion`, the second argument of `slInit`.
pub const kSDKVersion: u64 =
    (SL_VERSION_MAJOR << 48) | (SL_VERSION_MINOR << 32) | (SL_VERSION_PATCH << 16) | kSDKVersionMagic;

pub const kStructVersion1: u32 = 1;
pub const kStructVersion2: u32 = 2;
pub const kStructVersion3: u32 = 3;
pub const kStructVersion4: u32 = 4;
pub const kStructVersion5: u32 = 5;
pub const kStructVersion6: u32 = 6;
pub const kStructVersion7: u32 = 7;

/// What SL substitutes when `Preferences::applicationId == 0`
/// (`source/core/sl.api/internal.h`, `pluginManager.cpp`). Not in the public headers. Leaving
/// `applicationId` at 0 is equivalent to passing this; see the module docs for why
/// `engineVersion` + `projectId` must then be set for NGX features (DLSS-G) in production builds.
pub const kTemporaryAppId: u32 = 100721531;

/// `sl::INVALID_FLOAT` (== FLT_MAX), the "not provided" value of the float defaults.
pub const INVALID_FLOAT: f32 = f32::MAX;
pub const INVALID_UINT: u32 = 0xffff_ffff;
pub const MAX_FRAMES_IN_FLIGHT: u32 = 6;

pub const INTERPOSER_DLL: &str = "sl.interposer.dll";

/// DLLs that must sit next to `sl.interposer.dll` (or in `pathsToPlugins`) for DLSS-G + Reflex.
pub const FG_DLLS: [&str; 6] = [
    "sl.interposer.dll",
    "sl.common.dll",
    "sl.dlss_g.dll",
    "nvngx_dlssg.dll",
    "sl.reflex.dll",
    "sl.pcl.dll",
];

// ---------------------------------------------------------------------------------------------
// StructType / BaseStructure (sl_struct.h)
// ---------------------------------------------------------------------------------------------

/// `sl::StructType` - a GUID.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct StructType {
    pub data1: u32,
    pub data2: u16,
    pub data3: u16,
    pub data4: [u8; 8],
}

impl StructType {
    pub const fn new(data1: u32, data2: u16, data3: u16, data4: [u8; 8]) -> Self {
        Self { data1, data2, data3, data4 }
    }
}

/// `sl::BaseStructure`: `BaseStructure* next{}; StructType structType{}; size_t structVersion;`
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct BaseStructure {
    pub next: *mut BaseStructure,
    pub structType: StructType,
    pub structVersion: usize,
}

impl BaseStructure {
    pub const fn new(structType: StructType, structVersion: u32) -> Self {
        Self { next: ptr::null_mut(), structType, structVersion: structVersion as usize }
    }
}

/// `SL_STRUCT_BEGIN(name, guid, version)`: the GUID / version constants plus helpers. Every
/// struct using it has `base: BaseStructure` as its first field.
macro_rules! sl_struct {
    ($name:ident, $guid:expr, $version:expr) => {
        impl $name {
            pub const STRUCT_TYPE: StructType = $guid;
            pub const STRUCT_VERSION: u32 = $version;
            /// Header exactly as the C++ constructor fills it (next = null).
            #[inline]
            pub const fn header() -> BaseStructure {
                BaseStructure::new(Self::STRUCT_TYPE, Self::STRUCT_VERSION)
            }
            /// For `next` chains and `slEvaluateFeature` inputs.
            #[inline]
            pub fn as_base(&self) -> *const BaseStructure {
                (self as *const Self).cast()
            }
            #[inline]
            pub fn as_base_mut(&mut self) -> *mut BaseStructure {
                (self as *mut Self).cast()
            }
        }
    };
}

/// A C/C++ enum as a transparent integer (an FFI-returned value outside the known set must not
/// be UB, so these are newtypes rather than Rust enums).
macro_rules! c_enum {
    ($(#[$m:meta])* $name:ident : $repr:ty { $($(#[$vm:meta])* $variant:ident = $value:expr),* $(,)? }) => {
        $(#[$m])*
        #[repr(transparent)]
        #[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
        pub struct $name(pub $repr);
        impl $name {
            $($(#[$vm])* pub const $variant: Self = Self($value);)*
        }
    };
}

/// A `c_enum!` that is used as a bit set (`SL_ENUM_OPERATORS_32/64`).
macro_rules! c_flags {
    ($name:ident : $repr:ty) => {
        impl $name {
            #[inline]
            pub const fn bits(self) -> $repr {
                self.0
            }
            #[inline]
            pub const fn contains(self, other: Self) -> bool {
                (self.0 & other.0) == other.0
            }
            #[inline]
            pub const fn intersects(self, other: Self) -> bool {
                (self.0 & other.0) != 0
            }
            #[inline]
            pub const fn union(self, other: Self) -> Self {
                Self(self.0 | other.0)
            }
        }
        impl core::ops::BitOr for $name {
            type Output = Self;
            fn bitor(self, rhs: Self) -> Self {
                Self(self.0 | rhs.0)
            }
        }
        impl core::ops::BitOrAssign for $name {
            fn bitor_assign(&mut self, rhs: Self) {
                self.0 |= rhs.0;
            }
        }
        impl core::ops::BitAnd for $name {
            type Output = Self;
            fn bitand(self, rhs: Self) -> Self {
                Self(self.0 & rhs.0)
            }
        }
        impl core::ops::Not for $name {
            type Output = Self;
            fn not(self) -> Self {
                Self(!self.0)
            }
        }
    };
}

// ---------------------------------------------------------------------------------------------
// Result (sl_result.h) - `enum class Result` (no underlying type => int)
// ---------------------------------------------------------------------------------------------

/// `sl::Result`.
#[repr(transparent)]
#[must_use]
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub struct SlResult(pub i32);

const RESULT_NAMES: [&str; 40] = [
    "eOk",
    "eErrorIO",
    "eErrorDriverOutOfDate",
    "eErrorOSOutOfDate",
    "eErrorOSDisabledHWS",
    "eErrorDeviceNotCreated",
    "eErrorNoSupportedAdapterFound",
    "eErrorAdapterNotSupported",
    "eErrorNoPlugins",
    "eErrorVulkanAPI",
    "eErrorDXGIAPI",
    "eErrorD3DAPI",
    "eErrorNRDAPI",
    "eErrorNVAPI",
    "eErrorReflexAPI",
    "eErrorNGXFailed",
    "eErrorJSONParsing",
    "eErrorMissingProxy",
    "eErrorMissingResourceState",
    "eErrorInvalidIntegration",
    "eErrorMissingInputParameter",
    "eErrorNotInitialized",
    "eErrorComputeFailed",
    "eErrorInitNotCalled",
    "eErrorExceptionHandler",
    "eErrorInvalidParameter",
    "eErrorMissingConstants",
    "eErrorDuplicatedConstants",
    "eErrorMissingOrInvalidAPI",
    "eErrorCommonConstantsMissing",
    "eErrorUnsupportedInterface",
    "eErrorFeatureMissing",
    "eErrorFeatureNotSupported",
    "eErrorFeatureMissingHooks",
    "eErrorFeatureFailedToLoad",
    "eErrorFeatureWrongPriority",
    "eErrorFeatureMissingDependency",
    "eErrorFeatureManagerInvalidState",
    "eErrorInvalidState",
    "eWarnOutOfVRAM",
];

impl SlResult {
    pub const eOk: Self = Self(0);
    pub const eErrorIO: Self = Self(1);
    pub const eErrorDriverOutOfDate: Self = Self(2);
    pub const eErrorOSOutOfDate: Self = Self(3);
    pub const eErrorOSDisabledHWS: Self = Self(4);
    pub const eErrorDeviceNotCreated: Self = Self(5);
    pub const eErrorNoSupportedAdapterFound: Self = Self(6);
    pub const eErrorAdapterNotSupported: Self = Self(7);
    pub const eErrorNoPlugins: Self = Self(8);
    pub const eErrorVulkanAPI: Self = Self(9);
    pub const eErrorDXGIAPI: Self = Self(10);
    pub const eErrorD3DAPI: Self = Self(11);
    pub const eErrorNRDAPI: Self = Self(12);
    pub const eErrorNVAPI: Self = Self(13);
    pub const eErrorReflexAPI: Self = Self(14);
    pub const eErrorNGXFailed: Self = Self(15);
    pub const eErrorJSONParsing: Self = Self(16);
    pub const eErrorMissingProxy: Self = Self(17);
    pub const eErrorMissingResourceState: Self = Self(18);
    pub const eErrorInvalidIntegration: Self = Self(19);
    pub const eErrorMissingInputParameter: Self = Self(20);
    pub const eErrorNotInitialized: Self = Self(21);
    pub const eErrorComputeFailed: Self = Self(22);
    pub const eErrorInitNotCalled: Self = Self(23);
    pub const eErrorExceptionHandler: Self = Self(24);
    pub const eErrorInvalidParameter: Self = Self(25);
    pub const eErrorMissingConstants: Self = Self(26);
    pub const eErrorDuplicatedConstants: Self = Self(27);
    pub const eErrorMissingOrInvalidAPI: Self = Self(28);
    pub const eErrorCommonConstantsMissing: Self = Self(29);
    pub const eErrorUnsupportedInterface: Self = Self(30);
    pub const eErrorFeatureMissing: Self = Self(31);
    pub const eErrorFeatureNotSupported: Self = Self(32);
    pub const eErrorFeatureMissingHooks: Self = Self(33);
    pub const eErrorFeatureFailedToLoad: Self = Self(34);
    pub const eErrorFeatureWrongPriority: Self = Self(35);
    pub const eErrorFeatureMissingDependency: Self = Self(36);
    pub const eErrorFeatureManagerInvalidState: Self = Self(37);
    pub const eErrorInvalidState: Self = Self(38);
    pub const eWarnOutOfVRAM: Self = Self(39);

    #[inline]
    pub const fn is_ok(self) -> bool {
        self.0 == 0
    }
    #[inline]
    pub fn ok(self) -> Result<(), SlResult> {
        if self.is_ok() { Ok(()) } else { Err(self) }
    }
    pub fn name(self) -> &'static str {
        usize::try_from(self.0)
            .ok()
            .and_then(|i| RESULT_NAMES.get(i).copied())
            .unwrap_or("<unknown sl::Result>")
    }
}

impl core::fmt::Debug for SlResult {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "{}({})", self.name(), self.0)
    }
}

impl core::fmt::Display for SlResult {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "sl::Result::{} ({})", self.name(), self.0)
    }
}

impl std::error::Error for SlResult {}

/// Returned by the wrappers when an optional / feature function pointer was not resolved.
pub const MISSING_FUNCTION: SlResult = SlResult::eErrorMissingOrInvalidAPI;

// ---------------------------------------------------------------------------------------------
// Ids and enums (sl_core_types.h, sl_consts.h, sl_appidentity.h, sl_device_wrappers.h)
// ---------------------------------------------------------------------------------------------

/// `using Feature = uint32_t;`
pub type Feature = u32;
pub const kFeatureDLSS: Feature = 0;
pub const kFeatureNRD_INVALID: Feature = 1;
pub const kFeatureNIS: Feature = 2;
pub const kFeatureReflex: Feature = 3;
pub const kFeaturePCL: Feature = 4;
pub const kFeatureDeepDVC: Feature = 5;
pub const kFeatureLatewarp: Feature = 6;
pub const kFeatureDLSS_G: Feature = 1000;
pub const kFeatureDLSS_RR: Feature = 1001;
pub const kFeatureNvPerf: Feature = 1002;
pub const kFeatureDirectSR: Feature = 1003;
pub const kFeatureDLSS_NR: Feature = 1004;
pub const kFeatureImGUI: Feature = 9999;
/// sl.common (UINT_MAX), "NOT intended to be used directly"; always loaded.
pub const kFeatureCommon: Feature = u32::MAX;

/// `featuresToLoad` for frame generation. sl.dlss_g requires sl.common + sl.reflex; sl.common and
/// sl.pcl are added by SL itself.
pub const FG_FEATURES: [Feature; 2] = [kFeatureDLSS_G, kFeatureReflex];

/// `using BufferType = uint32_t;` (subset; the header has 0..=72)
pub type BufferType = u32;
pub const kBufferTypeDepth: BufferType = 0;
pub const kBufferTypeMotionVectors: BufferType = 1;
pub const kBufferTypeHUDLessColor: BufferType = 2;
pub const kBufferTypeScalingInputColor: BufferType = 3;
pub const kBufferTypeScalingOutputColor: BufferType = 4;
pub const kBufferTypeExposure: BufferType = 13;
pub const kBufferTypeUIColorAndAlpha: BufferType = 23;
pub const kBufferTypeHiResDepth: BufferType = 48;
pub const kBufferTypeLinearDepth: BufferType = 49;
pub const kBufferTypeBidirectionalDistortionField: BufferType = 50;
pub const kBufferTypeBackbuffer: BufferType = 53;
pub const kBufferTypeNoWarpMask: BufferType = 54;
pub const kBufferTypeUIAlpha: BufferType = 69;

c_enum! {
    /// `enum class LogLevel : uint32_t`
    LogLevel: u32 { eOff = 0, eDefault = 1, eVerbose = 2, eCount = 3 }
}

c_enum! {
    /// `enum class LogType : uint32_t`
    LogType: u32 { eInfo = 0, eWarn = 1, eError = 2, eCount = 3 }
}

c_enum! {
    /// `enum class ResourceType : char`
    ResourceType: u8 {
        eTex2d = 0,
        eBuffer = 1,
        eCommandQueue = 2,
        eCommandBuffer = 3,
        eCommandPool = 4,
        eFence = 5,
        eSwapchain = 6,
        eHostFence = 7,
        eUnknown = 8,
        eCount = 9,
    }
}

c_enum! {
    /// `enum ResourceLifecycle` (unscoped, no fixed type => int)
    ResourceLifecycle: i32 { eOnlyValidNow = 0, eValidUntilPresent = 1, eValidUntilEvaluate = 2 }
}

c_enum! {
    /// `enum Boolean : char`
    Boolean: u8 { eFalse = 0, eTrue = 1, eInvalid = 2 }
}

impl From<bool> for Boolean {
    fn from(b: bool) -> Self {
        if b { Self::eTrue } else { Self::eFalse }
    }
}

c_enum! {
    /// `enum class PreferenceFlags : uint64_t`
    PreferenceFlags: u64 {
        eDisableCLStateTracking = 1 << 0,
        eDisableDebugText = 1 << 1,
        eUseManualHooking = 1 << 2,
        eAllowOTA = 1 << 3,
        eBypassOSVersionCheck = 1 << 4,
        eUseDXGIFactoryProxy = 1 << 5,
        eLoadDownloadedPlugins = 1 << 6,
        eUseFrameBasedResourceTagging = 1 << 7,
        eAll = 0xff,
    }
}
c_flags!(PreferenceFlags: u64);

impl PreferenceFlags {
    /// The C++ default member initializer of `Preferences::flags`.
    pub const DEFAULT: Self = Self(
        Self::eDisableCLStateTracking.0 | Self::eAllowOTA.0 | Self::eLoadDownloadedPlugins.0,
    );
    /// Manual hooking + `slSetTagForFrame` (no OTA).
    pub const MANUAL_HOOKING_FRAME_TAGS: Self = Self(
        Self::eDisableCLStateTracking.0
            | Self::eUseManualHooking.0
            | Self::eUseFrameBasedResourceTagging.0,
    );
}

c_enum! {
    /// `enum class EngineType : uint32_t`
    EngineType: u32 { eCustom = 0, eUnreal = 1, eUnity = 2, eCount = 3 }
}

c_enum! {
    /// `enum class RenderAPI : uint32_t`
    RenderAPI: u32 { eD3D11 = 0, eD3D12 = 1, eVulkan = 2, eCount = 3 }
}

c_enum! {
    /// `enum class FeatureRequirementFlags : uint32_t`
    FeatureRequirementFlags: u32 {
        eD3D11Supported = 1 << 0,
        eD3D12Supported = 1 << 1,
        eVulkanSupported = 1 << 2,
        eVSyncOffRequired = 1 << 3,
        eHardwareSchedulingRequired = 1 << 4,
        eAll = 0x1f,
    }
}
c_flags!(FeatureRequirementFlags: u32);

// DLSS-G (sl_dlss_g.h)

c_enum! {
    /// `enum class DLSSGMode : uint32_t`
    DLSSGMode: u32 { eOff = 0, eOn = 1, eAuto = 2, eDynamic = 3, eCount = 4 }
}

c_enum! {
    /// `enum class DLSSGFlags : uint32_t`
    DLSSGFlags: u32 {
        eShowOnlyInterpolatedFrame = 1 << 0,
        eDynamicResolutionEnabled = 1 << 1,
        eRequestVRAMEstimate = 1 << 2,
        eRetainResourcesWhenOff = 1 << 3,
        eEnableFullscreenMenuDetection = 1 << 4,
        eAll = 0x1f,
    }
}
c_flags!(DLSSGFlags: u32);

c_enum! {
    /// `enum class DLSSGQueueParallelismMode : uint32_t` (eBlockNoClientQueues is Vulkan-only)
    DLSSGQueueParallelismMode: u32 { eBlockPresentingClientQueue = 0, eBlockNoClientQueues = 1, eCount = 2 }
}

c_enum! {
    /// `enum class DLSSGStatus : uint32_t`
    DLSSGStatus: u32 {
        eOk = 0,
        eFailResolutionTooLow = 1 << 0,
        eFailReflexNotDetectedAtRuntime = 1 << 1,
        eFailHDRFormatNotSupported = 1 << 2,
        eFailCommonConstantsInvalid = 1 << 3,
        eFailGetCurrentBackBufferIndexNotCalled = 1 << 4,
        eReserved5 = 1 << 5,
        eAll = 0x3f,
    }
}
c_flags!(DLSSGStatus: u32);

// Reflex (sl_reflex.h)

c_enum! {
    /// `enum ReflexMode` (unscoped, no fixed type => int)
    ReflexMode: i32 { eOff = 0, eLowLatency = 1, eLowLatencyWithBoost = 2, ReflexMode_eCount = 3 }
}

pub const kReflexFrameReportCount: usize = 64;

// PCL (sl_pcl.h)

c_enum! {
    /// `enum class PCLHotKey : int16_t`
    PCLHotKey: i16 { eUsePingMessage = 0, eVK_F13 = 0x7C, eVK_F14 = 0x7D, eVK_F15 = 0x7E }
}

c_enum! {
    /// `enum class PCLMarker : uint32_t`
    PCLMarker: u32 {
        eSimulationStart = 0,
        eSimulationEnd = 1,
        eRenderSubmitStart = 2,
        eRenderSubmitEnd = 3,
        ePresentStart = 4,
        ePresentEnd = 5,
        // 6 = eInputSample, deprecated/removed
        eTriggerFlash = 7,
        ePCLatencyPing = 8,
        eOutOfBandRenderSubmitStart = 9,
        eOutOfBandRenderSubmitEnd = 10,
        eOutOfBandPresentStart = 11,
        eOutOfBandPresentEnd = 12,
        eControllerInputSample = 13,
        eDeltaTCalculation = 14,
        eLateWarpPresentStart = 15,
        eLateWarpPresentEnd = 16,
        eCameraConstructed = 17,
        eLateWarpRenderSubmitStart = 18,
        eLateWarpRenderSubmitEnd = 19,
        eVendorInternalAsyncPresentStart = 20,
        eVendorInternalAsyncPresentEnd = 21,
        eNumPresentsInBatch = 22,
        eMaximum = 23,
    }
}

// ---------------------------------------------------------------------------------------------
// Math types (sl_consts.h) - default-constructed to INVALID_FLOAT like the C++ constructors
// ---------------------------------------------------------------------------------------------

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct float2 {
    pub x: f32,
    pub y: f32,
}
impl float2 {
    pub const fn new(x: f32, y: f32) -> Self {
        Self { x, y }
    }
}
impl Default for float2 {
    fn default() -> Self {
        Self::new(INVALID_FLOAT, INVALID_FLOAT)
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct float3 {
    pub x: f32,
    pub y: f32,
    pub z: f32,
}
impl float3 {
    pub const fn new(x: f32, y: f32, z: f32) -> Self {
        Self { x, y, z }
    }
}
impl Default for float3 {
    fn default() -> Self {
        Self::new(INVALID_FLOAT, INVALID_FLOAT, INVALID_FLOAT)
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct float4 {
    pub x: f32,
    pub y: f32,
    pub z: f32,
    pub w: f32,
}
impl float4 {
    pub const fn new(x: f32, y: f32, z: f32, w: f32) -> Self {
        Self { x, y, z, w }
    }
}
impl Default for float4 {
    fn default() -> Self {
        Self::new(INVALID_FLOAT, INVALID_FLOAT, INVALID_FLOAT, INVALID_FLOAT)
    }
}

/// Row-major 4x4 (`float4 row[4]`).
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Default)]
pub struct float4x4 {
    pub row: [float4; 4],
}
impl float4x4 {
    pub const fn from_rows(r: [[f32; 4]; 4]) -> Self {
        Self {
            row: [
                float4::new(r[0][0], r[0][1], r[0][2], r[0][3]),
                float4::new(r[1][0], r[1][1], r[1][2], r[1][3]),
                float4::new(r[2][0], r[2][1], r[2][2], r[2][3]),
                float4::new(r[3][0], r[3][1], r[3][2], r[3][3]),
            ],
        }
    }
    pub const fn identity() -> Self {
        Self::from_rows([[1.0, 0.0, 0.0, 0.0], [0.0, 1.0, 0.0, 0.0], [0.0, 0.0, 1.0, 0.0], [0.0, 0.0, 0.0, 1.0]])
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct uint2 {
    pub x: u32,
    pub y: u32,
}

/// `sl::Extent { top, left, width, height }` (all zero = whole resource).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Extent {
    pub top: u32,
    pub left: u32,
    pub width: u32,
    pub height: u32,
}

/// `sl::Version { major, minor, build }`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Version {
    pub major: u32,
    pub minor: u32,
    pub build: u32,
}

/// `struct APIError { union { HRESULT hres; SL_VKResult vkRes; }; }` (both 32-bit).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct APIError {
    pub hres: i32,
}

// ---------------------------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------------------------

/// `Resource(const ResourceAllocationDesc* desc, void* device)`
pub type PFun_ResourceAllocateCallback =
    unsafe extern "C" fn(desc: *const ResourceAllocationDesc, device: *mut c_void) -> Resource;
/// `void(Resource* resource, void* device)`
pub type PFun_ResourceReleaseCallback = unsafe extern "C" fn(resource: *mut Resource, device: *mut c_void);
/// `void(LogType type, const char* msg)`
pub type PFun_LogMessageCallback = unsafe extern "C" fn(ty: LogType, msg: *const c_char);
/// `void(const APIError& lastError)` - called on the present thread, must return quickly.
pub type PFunOnAPIErrorCallback = unsafe extern "C" fn(lastError: *const APIError);

/// A ready-made `logMessageCallback` that forwards SL's log to `tracing`.
pub unsafe extern "C" fn log_to_tracing(ty: LogType, msg: *const c_char) {
    if msg.is_null() {
        return;
    }
    // SAFETY: SL passes a NUL-terminated string valid for the duration of the call.
    let text = unsafe { CStr::from_ptr(msg) }.to_string_lossy();
    let text = text.trim_end();
    match ty {
        LogType::eError => tracing::error!(target: "streamline", "{text}"),
        LogType::eWarn => tracing::warn!(target: "streamline", "{text}"),
        _ => tracing::info!(target: "streamline", "{text}"),
    }
}

// ---------------------------------------------------------------------------------------------
// Core structs (sl_core_types.h)
// ---------------------------------------------------------------------------------------------

/// `sl::Preferences` {1CA10965-BF8E-432B-8DA1-6716D879FB14}, kStructVersion1. All fields of the
/// 2.14.16 header, in order.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct Preferences {
    pub base: BaseStructure,
    pub showConsole: bool,
    pub logLevel: LogLevel,
    /// `const wchar_t** pathsToPlugins` - absolute paths, first has highest priority.
    pub pathsToPlugins: *const *const u16,
    pub numPathsToPlugins: u32,
    /// `const wchar_t*` - null disables file logging.
    pub pathToLogsAndData: *const u16,
    pub allocateCallback: Option<PFun_ResourceAllocateCallback>,
    pub releaseCallback: Option<PFun_ResourceReleaseCallback>,
    pub logMessageCallback: Option<PFun_LogMessageCallback>,
    pub flags: PreferenceFlags,
    pub featuresToLoad: *const Feature,
    pub numFeaturesToLoad: u32,
    pub applicationId: u32,
    pub engine: EngineType,
    pub engineVersion: *const c_char,
    pub projectId: *const c_char,
    pub renderAPI: RenderAPI,
}
sl_struct!(
    Preferences,
    StructType::new(0x1ca10965, 0xbf8e, 0x432b, [0x8d, 0xa1, 0x67, 0x16, 0xd8, 0x79, 0xfb, 0x14]),
    kStructVersion1
);
impl Default for Preferences {
    fn default() -> Self {
        Self {
            base: Self::header(),
            showConsole: false,
            logLevel: LogLevel::eDefault,
            pathsToPlugins: ptr::null(),
            numPathsToPlugins: 0,
            pathToLogsAndData: ptr::null(),
            allocateCallback: None,
            releaseCallback: None,
            logMessageCallback: None,
            flags: PreferenceFlags::DEFAULT,
            featuresToLoad: ptr::null(),
            numFeaturesToLoad: 0,
            applicationId: 0,
            engine: EngineType::eCustom,
            engineVersion: ptr::null(),
            projectId: ptr::null(),
            renderAPI: RenderAPI::eD3D12,
        }
    }
}

/// `sl::ResourceAllocationDesc` {00BB57E5-49A2-4C23-A519-AB9286E74014}, kStructVersion1.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ResourceAllocationDesc {
    pub base: BaseStructure,
    pub r#type: ResourceType,
    /// D3D12_RESOURCE_DESC*
    pub desc: *mut c_void,
    /// D3D12_RESOURCE_STATES
    pub state: u32,
    /// D3D12_HEAP_PROPERTIES* or null
    pub heap: *mut c_void,
}
sl_struct!(
    ResourceAllocationDesc,
    StructType::new(0x00bb57e5, 0x49a2, 0x4c23, [0xa5, 0x19, 0xab, 0x92, 0x86, 0xe7, 0x40, 0x14]),
    kStructVersion1
);
impl Default for ResourceAllocationDesc {
    fn default() -> Self {
        Self { base: Self::header(), r#type: ResourceType::eTex2d, desc: ptr::null_mut(), state: 0, heap: ptr::null_mut() }
    }
}

/// `sl::Resource` {3A9D70CF-2418-4B72-8391-13F8721C7261}, kStructVersion1.
///
/// For D3D12 only `type`, `native` and `state` are needed (state is MANDATORY with manual hooking).
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct Resource {
    pub base: BaseStructure,
    pub r#type: ResourceType,
    /// ID3D12Resource*
    pub native: *mut c_void,
    /// Vulkan only
    pub memory: *mut c_void,
    /// Vulkan only
    pub view: *mut c_void,
    /// D3D12_RESOURCE_STATES (UINT_MAX = not provided)
    pub state: u32,
    pub width: u32,
    pub height: u32,
    pub nativeFormat: u32,
    pub mipLevels: u32,
    pub arrayLayers: u32,
    pub gpuVirtualAddress: u64,
    /// VkImageCreateFlags (no initializer in C++; zeroed here)
    pub flags: u32,
    pub usage: u32,
    /// `enum InternalFlags : uint16_t` - do not modify
    pub internalFlags: u16,
    pub reserved: u16,
}
sl_struct!(
    Resource,
    StructType::new(0x3a9d70cf, 0x2418, 0x4b72, [0x83, 0x91, 0x13, 0xf8, 0x72, 0x1c, 0x72, 0x61]),
    kStructVersion1
);
impl Default for Resource {
    fn default() -> Self {
        Self {
            base: Self::header(),
            r#type: ResourceType::eTex2d,
            native: ptr::null_mut(),
            memory: ptr::null_mut(),
            view: ptr::null_mut(),
            state: u32::MAX,
            width: 0,
            height: 0,
            nativeFormat: 0,
            mipLevels: 0,
            arrayLayers: 0,
            gpuVirtualAddress: 0,
            flags: 0,
            usage: 0,
            internalFlags: 0,
            reserved: 0,
        }
    }
}
impl Resource {
    /// `Resource(ResourceType::eTex2d, native, state)` for an `ID3D12Resource*`.
    pub fn d3d12_tex2d(native: *mut c_void, state: u32) -> Self {
        Self { native, state, ..Self::default() }
    }
}

/// `sl::ResourceTag` {4C6A5AAD-B445-496C-87FF-1AF3845BE653}, kStructVersion1.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ResourceTag {
    pub base: BaseStructure,
    /// Null removes the tag (releases SL's reference for eValidUntilPresent).
    pub resource: *mut Resource,
    pub r#type: BufferType,
    pub lifecycle: ResourceLifecycle,
    /// All zero = whole resource.
    pub extent: Extent,
}
sl_struct!(
    ResourceTag,
    StructType::new(0x4c6a5aad, 0xb445, 0x496c, [0x87, 0xff, 0x1a, 0xf3, 0x84, 0x5b, 0xe6, 0x53]),
    kStructVersion1
);
impl Default for ResourceTag {
    fn default() -> Self {
        Self {
            base: Self::header(),
            resource: ptr::null_mut(),
            r#type: 0,
            lifecycle: ResourceLifecycle::eOnlyValidNow,
            extent: Extent::default(),
        }
    }
}
impl ResourceTag {
    /// `ResourceTag(Resource* r, BufferType t, ResourceLifecycle l, const Extent* e = nullptr)`
    pub fn new(resource: *mut Resource, ty: BufferType, lifecycle: ResourceLifecycle, extent: Option<Extent>) -> Self {
        Self { resource, r#type: ty, lifecycle, extent: extent.unwrap_or_default(), ..Self::default() }
    }
    /// A null tag for `ty` (removes it).
    pub fn null(ty: BufferType) -> Self {
        Self::new(ptr::null_mut(), ty, ResourceLifecycle::eValidUntilPresent, None)
    }
}

/// `sl::FrameToken` {830A0F35-DB84-4171-A804-59B206499B18}, kStructVersion1 - OPAQUE.
///
/// It is an abstract C++ class (`virtual operator uint32_t() const = 0`), so the object starts
/// with a vtable pointer and is only ever handled by pointer, as obtained from
/// `slGetNewFrameToken`. SL owns it (a small ring of tokens).
#[repr(C)]
pub struct FrameToken {
    _opaque: [u8; 0],
    _marker: core::marker::PhantomData<(*mut u8, core::marker::PhantomPinned)>,
}
impl FrameToken {
    pub const STRUCT_TYPE: StructType =
        StructType::new(0x830a0f35, 0xdb84, 0x4171, [0xa8, 0x04, 0x59, 0xb2, 0x06, 0x49, 0x9b, 0x18]);
    pub const STRUCT_VERSION: u32 = kStructVersion1;
}

/// A frame token handle returned by [`Sl::new_frame_token`]. Copyable; the docs allow handing a
/// copy to other threads ("Store a copy of the handle and use that in any async tasks").
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Frame(NonNull<FrameToken>);
// SAFETY: the token is SL-owned, immutable from our side and documented as usable from any thread.
unsafe impl Send for Frame {}
// SAFETY: as above.
unsafe impl Sync for Frame {}

impl Frame {
    #[inline]
    pub fn as_ptr(self) -> *const FrameToken {
        self.0.as_ptr()
    }

    /// The frame index SL associates with the token (the C++ `operator uint32_t()`), read through
    /// vtable slot 0 (MSVC x64: vfptr at offset 0, `this` in RCX, result in EAX).
    ///
    /// Prefer remembering the index you passed to `slGetNewFrameToken`.
    pub unsafe fn sl_index(self) -> u32 {
        type IndexFn = unsafe extern "C" fn(this: *const FrameToken) -> u32;
        let this = self.as_ptr();
        // SAFETY: caller guarantees the token is alive (before slShutdown); slot 0 of the
        // vtable of every FrameToken implementation is `operator uint32_t() const`.
        unsafe {
            let vtable = *(this as *const *const IndexFn);
            (*vtable)(this)
        }
    }
}

/// `sl::ViewportHandle` {171B6435-9B3C-4FC8-9994-FBE52569AAA4}, kStructVersion1.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ViewportHandle {
    pub base: BaseStructure,
    /// Private in C++; default UINT_MAX.
    pub value: u32,
}
sl_struct!(
    ViewportHandle,
    StructType::new(0x171b6435, 0x9b3c, 0x4fc8, [0x99, 0x94, 0xfb, 0xe5, 0x25, 0x69, 0xaa, 0xa4]),
    kStructVersion1
);
impl ViewportHandle {
    pub const fn new(value: u32) -> Self {
        Self { base: Self::header(), value }
    }
}
impl Default for ViewportHandle {
    fn default() -> Self {
        Self::new(u32::MAX)
    }
}

/// `sl::FeatureRequirements` {66714097-AC6D-4BC6-8915-1E0F55A6B61F}, kStructVersion2.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct FeatureRequirements {
    pub base: BaseStructure,
    pub flags: FeatureRequirementFlags,
    pub maxNumCPUThreads: u32,
    pub maxNumViewports: u32,
    pub numRequiredTags: u32,
    pub requiredTags: *const BufferType,
    pub osVersionDetected: Version,
    pub osVersionRequired: Version,
    pub driverVersionDetected: Version,
    pub driverVersionRequired: Version,
    pub vkNumComputeQueuesRequired: u32,
    pub vkNumGraphicsQueuesRequired: u32,
    pub vkNumDeviceExtensions: u32,
    pub vkDeviceExtensions: *const *const c_char,
    pub vkNumInstanceExtensions: u32,
    pub vkInstanceExtensions: *const *const c_char,
    pub vkNumFeatures12: u32,
    pub vkFeatures12: *const *const c_char,
    pub vkNumFeatures13: u32,
    pub vkFeatures13: *const *const c_char,
    pub vkNumOpticalFlowQueuesRequired: u32,
}
sl_struct!(
    FeatureRequirements,
    StructType::new(0x66714097, 0xac6d, 0x4bc6, [0x89, 0x15, 0x1e, 0x0f, 0x55, 0xa6, 0xb6, 0x1f]),
    kStructVersion2
);
impl Default for FeatureRequirements {
    fn default() -> Self {
        Self {
            base: Self::header(),
            flags: FeatureRequirementFlags(0),
            maxNumCPUThreads: 0,
            maxNumViewports: 0,
            numRequiredTags: 0,
            requiredTags: ptr::null(),
            osVersionDetected: Version::default(),
            osVersionRequired: Version::default(),
            driverVersionDetected: Version::default(),
            driverVersionRequired: Version::default(),
            vkNumComputeQueuesRequired: 0,
            vkNumGraphicsQueuesRequired: 0,
            vkNumDeviceExtensions: 0,
            vkDeviceExtensions: ptr::null(),
            vkNumInstanceExtensions: 0,
            vkInstanceExtensions: ptr::null(),
            vkNumFeatures12: 0,
            vkFeatures12: ptr::null(),
            vkNumFeatures13: 0,
            vkFeatures13: ptr::null(),
            vkNumOpticalFlowQueuesRequired: 0,
        }
    }
}
impl FeatureRequirements {
    /// The required buffer tags (pointer owned by SL).
    pub unsafe fn required_tags(&self) -> &[BufferType] {
        if self.requiredTags.is_null() || self.numRequiredTags == 0 {
            &[]
        } else {
            // SAFETY: SL guarantees `numRequiredTags` entries.
            unsafe { core::slice::from_raw_parts(self.requiredTags, self.numRequiredTags as usize) }
        }
    }
}

/// `sl::FeatureVersion` {6D5B51F0-076B-486D-9995-5A561043F5C1}, kStructVersion1.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct FeatureVersion {
    pub base: BaseStructure,
    pub versionSL: Version,
    pub versionNGX: Version,
}
sl_struct!(
    FeatureVersion,
    StructType::new(0x6d5b51f0, 0x076b, 0x486d, [0x99, 0x95, 0x5a, 0x56, 0x10, 0x43, 0xf5, 0xc1]),
    kStructVersion1
);
impl Default for FeatureVersion {
    fn default() -> Self {
        Self { base: Self::header(), versionSL: Version::default(), versionNGX: Version::default() }
    }
}

/// `sl::AdapterInfo` {0677315F-A746-4492-9F42-CB6142C9C3D4}, kStructVersion1.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct AdapterInfo {
    pub base: BaseStructure,
    /// Points at a `LUID` (DXGI_ADAPTER_DESC::AdapterLuid).
    pub deviceLUID: *mut u8,
    /// `sizeof(LUID)` = 8
    pub deviceLUIDSizeInBytes: u32,
    pub vkPhysicalDevice: *mut c_void,
}
sl_struct!(
    AdapterInfo,
    StructType::new(0x0677315f, 0xa746, 0x4492, [0x9f, 0x42, 0xcb, 0x61, 0x42, 0xc9, 0xc3, 0xd4]),
    kStructVersion1
);
impl Default for AdapterInfo {
    fn default() -> Self {
        Self { base: Self::header(), deviceLUID: ptr::null_mut(), deviceLUIDSizeInBytes: 0, vkPhysicalDevice: ptr::null_mut() }
    }
}

// ---------------------------------------------------------------------------------------------
// Constants (sl_consts.h)
// ---------------------------------------------------------------------------------------------

/// `sl::Constants` {DCD35AD7-4E4A-4BAD-A90C-E0C49EB23AFE}, kStructVersion2.
/// Defaults match C++: every matrix/vector element INVALID_FLOAT.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct Constants {
    pub base: BaseStructure,
    pub cameraViewToClip: float4x4,
    pub clipToCameraView: float4x4,
    /// Optional
    pub clipToLensClip: float4x4,
    pub clipToPrevClip: float4x4,
    pub prevClipToClip: float4x4,
    /// Pixel space
    pub jitterOffset: float2,
    pub mvecScale: float2,
    /// Optional
    pub cameraPinholeOffset: float2,
    pub cameraPos: float3,
    pub cameraUp: float3,
    pub cameraRight: float3,
    pub cameraFwd: float3,
    pub cameraNear: f32,
    pub cameraFar: f32,
    /// Radians
    pub cameraFOV: f32,
    pub cameraAspectRatio: f32,
    /// Only needed if `cameraMotionIncluded` is false
    pub motionVectorsInvalidValue: f32,
    pub depthInverted: Boolean,
    pub cameraMotionIncluded: Boolean,
    pub motionVectors3D: Boolean,
    pub reset: Boolean,
    pub orthographicProjection: Boolean,
    pub motionVectorsDilated: Boolean,
    pub motionVectorsJittered: Boolean,
    // Version 2
    pub minRelativeLinearDepthObjectSeparation: f32,
}
sl_struct!(
    Constants,
    StructType::new(0xdcd35ad7, 0x4e4a, 0x4bad, [0xa9, 0x0c, 0xe0, 0xc4, 0x9e, 0xb2, 0x3a, 0xfe]),
    kStructVersion2
);
impl Default for Constants {
    fn default() -> Self {
        Self {
            base: Self::header(),
            cameraViewToClip: float4x4::default(),
            clipToCameraView: float4x4::default(),
            clipToLensClip: float4x4::default(),
            clipToPrevClip: float4x4::default(),
            prevClipToClip: float4x4::default(),
            jitterOffset: float2::default(),
            mvecScale: float2::default(),
            cameraPinholeOffset: float2::default(),
            cameraPos: float3::default(),
            cameraUp: float3::default(),
            cameraRight: float3::default(),
            cameraFwd: float3::default(),
            cameraNear: INVALID_FLOAT,
            cameraFar: INVALID_FLOAT,
            cameraFOV: INVALID_FLOAT,
            cameraAspectRatio: INVALID_FLOAT,
            motionVectorsInvalidValue: INVALID_FLOAT,
            depthInverted: Boolean::eInvalid,
            cameraMotionIncluded: Boolean::eInvalid,
            motionVectors3D: Boolean::eInvalid,
            reset: Boolean::eInvalid,
            orthographicProjection: Boolean::eFalse,
            motionVectorsDilated: Boolean::eFalse,
            motionVectorsJittered: Boolean::eFalse,
            minRelativeLinearDepthObjectSeparation: 40.0,
        }
    }
}

// ---------------------------------------------------------------------------------------------
// DLSS-G (sl_dlss_g.h)
// ---------------------------------------------------------------------------------------------

/// `sl::DLSSGOptions` {FAC5F1CB-2DFD-4F36-A1E6-3A9E865256C5}, kStructVersion5.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct DLSSGOptions {
    pub base: BaseStructure,
    pub mode: DLSSGMode,
    /// 1 = 2x, 2 = 3x, 3 = 4x; <= DLSSGState::numFramesToGenerateMax
    pub numFramesToGenerate: u32,
    pub flags: DLSSGFlags,
    pub dynamicResWidth: u32,
    pub dynamicResHeight: u32,
    pub numBackBuffers: u32,
    pub mvecDepthWidth: u32,
    pub mvecDepthHeight: u32,
    pub colorWidth: u32,
    pub colorHeight: u32,
    /// DXGI_FORMAT values
    pub colorBufferFormat: u32,
    pub mvecBufferFormat: u32,
    pub depthBufferFormat: u32,
    pub hudLessBufferFormat: u32,
    pub uiBufferFormat: u32,
    pub onErrorCallback: Option<PFunOnAPIErrorCallback>,
    // kStructVersion2
    pub bReserved15: Boolean,
    // kStructVersion3
    pub queueParallelismMode: DLSSGQueueParallelismMode,
    // kStructVersion4
    pub enableUserInterfaceRecomposition: Boolean,
    // kStructVersion5
    /// 0 = display refresh rate (eDynamic only)
    pub dynamicTargetFrameRate: f32,
}
sl_struct!(
    DLSSGOptions,
    StructType::new(0xfac5f1cb, 0x2dfd, 0x4f36, [0xa1, 0xe6, 0x3a, 0x9e, 0x86, 0x52, 0x56, 0xc5]),
    kStructVersion5
);
impl Default for DLSSGOptions {
    fn default() -> Self {
        Self {
            base: Self::header(),
            mode: DLSSGMode::eOff,
            numFramesToGenerate: 1,
            flags: DLSSGFlags(0),
            dynamicResWidth: 0,
            dynamicResHeight: 0,
            numBackBuffers: 0,
            mvecDepthWidth: 0,
            mvecDepthHeight: 0,
            colorWidth: 0,
            colorHeight: 0,
            colorBufferFormat: 0,
            mvecBufferFormat: 0,
            depthBufferFormat: 0,
            hudLessBufferFormat: 0,
            uiBufferFormat: 0,
            onErrorCallback: None,
            bReserved15: Boolean::eInvalid,
            queueParallelismMode: DLSSGQueueParallelismMode::eBlockPresentingClientQueue,
            enableUserInterfaceRecomposition: Boolean::eFalse,
            dynamicTargetFrameRate: 0.0,
        }
    }
}

/// `sl::DLSSGState` {CC8AC8E1-A179-44F5-97FA-E74112F9BC61}, kStructVersion4.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct DLSSGState {
    pub base: BaseStructure,
    pub estimatedVRAMUsageInBytes: u64,
    pub status: DLSSGStatus,
    pub minWidthOrHeight: u32,
    pub numFramesActuallyPresented: u32,
    // kStructVersion2
    pub numFramesToGenerateMax: u32,
    pub bReserved4: Boolean,
    pub bIsVsyncSupportAvailable: Boolean,
    /// Internal fence (ID3D12Fence*) for eBlockNoClientQueues / cross-queue input reuse
    pub inputsProcessingCompletionFence: *mut c_void,
    pub lastPresentInputsProcessingCompletionFenceValue: u64,
    // kStructVersion4
    pub bIsDynamicMFGSupported: Boolean,
}
sl_struct!(
    DLSSGState,
    StructType::new(0xcc8ac8e1, 0xa179, 0x44f5, [0x97, 0xfa, 0xe7, 0x41, 0x12, 0xf9, 0xbc, 0x61]),
    kStructVersion4
);
impl Default for DLSSGState {
    fn default() -> Self {
        Self {
            base: Self::header(),
            estimatedVRAMUsageInBytes: 0,
            status: DLSSGStatus::eOk,
            minWidthOrHeight: 0,
            numFramesActuallyPresented: 0,
            numFramesToGenerateMax: 0,
            bReserved4: Boolean::eFalse,
            bIsVsyncSupportAvailable: Boolean::eFalse,
            inputsProcessingCompletionFence: ptr::null_mut(),
            lastPresentInputsProcessingCompletionFenceValue: 0,
            bIsDynamicMFGSupported: Boolean::eFalse,
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Reflex (sl_reflex.h)
// ---------------------------------------------------------------------------------------------

/// `sl::ReflexOptions` {F03AF81A-6D0B-4902-A651-C4965E215434}, kStructVersion1.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ReflexOptions {
    pub base: BaseStructure,
    pub mode: ReflexMode,
    /// 0 = no cap, else microseconds per frame
    pub frameLimitUs: u32,
    /// Leave false unless told otherwise by NVIDIA
    pub useMarkersToOptimize: bool,
    /// VK_F13/14/15 or 0
    pub virtualKey: u16,
    pub idThread: u32,
}
sl_struct!(
    ReflexOptions,
    StructType::new(0xf03af81a, 0x6d0b, 0x4902, [0xa6, 0x51, 0xc4, 0x96, 0x5e, 0x21, 0x54, 0x34]),
    kStructVersion1
);
impl Default for ReflexOptions {
    fn default() -> Self {
        Self { base: Self::header(), mode: ReflexMode::eOff, frameLimitUs: 0, useMarkersToOptimize: false, virtualKey: 0, idThread: 0 }
    }
}

/// `sl::ReflexReport` {0D569B37-A1C8-4453-BE4D-40F4DE57952B}, kStructVersion1.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ReflexReport {
    pub base: BaseStructure,
    pub frameID: u64,
    pub inputSampleTime: u64,
    pub simStartTime: u64,
    pub simEndTime: u64,
    pub renderSubmitStartTime: u64,
    pub renderSubmitEndTime: u64,
    pub presentStartTime: u64,
    pub presentEndTime: u64,
    pub driverStartTime: u64,
    pub driverEndTime: u64,
    pub osRenderQueueStartTime: u64,
    pub osRenderQueueEndTime: u64,
    pub gpuRenderStartTime: u64,
    pub gpuRenderEndTime: u64,
    pub gpuActiveRenderTimeUs: u32,
    pub gpuFrameTimeUs: u32,
}
sl_struct!(
    ReflexReport,
    StructType::new(0x0d569b37, 0xa1c8, 0x4453, [0xbe, 0x4d, 0x40, 0xf4, 0xde, 0x57, 0x95, 0x2b]),
    kStructVersion1
);
impl Default for ReflexReport {
    fn default() -> Self {
        Self {
            base: Self::header(),
            frameID: 0,
            inputSampleTime: 0,
            simStartTime: 0,
            simEndTime: 0,
            renderSubmitStartTime: 0,
            renderSubmitEndTime: 0,
            presentStartTime: 0,
            presentEndTime: 0,
            driverStartTime: 0,
            driverEndTime: 0,
            osRenderQueueStartTime: 0,
            osRenderQueueEndTime: 0,
            gpuRenderStartTime: 0,
            gpuRenderEndTime: 0,
            gpuActiveRenderTimeUs: 0,
            gpuFrameTimeUs: 0,
        }
    }
}

/// `sl::ReflexReport2` {68BB0632-5E1C-402B-899D-B49F633C56C2}, kStructVersion1.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ReflexReport2 {
    pub base: BaseStructure,
    pub cameraConstructedTime: u64,
    pub crossAdapterCopyTimeUs: u32,
}
sl_struct!(
    ReflexReport2,
    StructType::new(0x68bb0632, 0x5e1c, 0x402b, [0x89, 0x9d, 0xb4, 0x9f, 0x63, 0x3c, 0x56, 0xc2]),
    kStructVersion1
);
impl Default for ReflexReport2 {
    fn default() -> Self {
        Self { base: Self::header(), cameraConstructedTime: 0, crossAdapterCopyTimeUs: 0 }
    }
}

/// `sl::ReflexState` {F0BB5985-DAF9-4728-B2FD-AE80A2BD7989}, kStructVersion2 (12848 bytes).
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ReflexState {
    pub base: BaseStructure,
    /// Show the Reflex UI only if true; everything else is done regardless.
    pub lowLatencyAvailable: bool,
    pub latencyReportAvailable: bool,
    /// No initializer in C++; zeroed here
    pub statsWindowMessage: u32,
    pub frameReport: [ReflexReport; kReflexFrameReportCount],
    pub flashIndicatorDriverControlled: bool,
    // kStructVersion2
    pub frameReport2: [ReflexReport2; kReflexFrameReportCount],
}
sl_struct!(
    ReflexState,
    StructType::new(0xf0bb5985, 0xdaf9, 0x4728, [0xb2, 0xfd, 0xae, 0x80, 0xa2, 0xbd, 0x79, 0x89]),
    kStructVersion2
);
impl Default for ReflexState {
    fn default() -> Self {
        Self {
            base: Self::header(),
            lowLatencyAvailable: false,
            latencyReportAvailable: false,
            statsWindowMessage: 0,
            frameReport: [ReflexReport::default(); kReflexFrameReportCount],
            flashIndicatorDriverControlled: false,
            frameReport2: [ReflexReport2::default(); kReflexFrameReportCount],
        }
    }
}

// ---------------------------------------------------------------------------------------------
// PCL (sl_pcl.h)
// ---------------------------------------------------------------------------------------------

/// `sl::PCLOptions` {CFA32F9B-023C-420E-9056-6832B74F89B4}, kStructVersion1.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct PCLOptions {
    pub base: BaseStructure,
    pub virtualKey: PCLHotKey,
    pub idThread: u32,
}
sl_struct!(
    PCLOptions,
    StructType::new(0xcfa32f9b, 0x023c, 0x420e, [0x90, 0x56, 0x68, 0x32, 0xb7, 0x4f, 0x89, 0xb4]),
    kStructVersion1
);
impl Default for PCLOptions {
    fn default() -> Self {
        Self { base: Self::header(), virtualKey: PCLHotKey::eUsePingMessage, idThread: 0 }
    }
}

/// `sl::PCLState` {CFA32F9B-023C-420E-9056-6832B74F89B5}, kStructVersion1.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct PCLState {
    pub base: BaseStructure,
    /// Window message id of the PCL ping (send `ePCLatencyPing` when it arrives)
    pub statsWindowMessage: u32,
}
sl_struct!(
    PCLState,
    StructType::new(0xcfa32f9b, 0x023c, 0x420e, [0x90, 0x56, 0x68, 0x32, 0xb7, 0x4f, 0x89, 0xb5]),
    kStructVersion1
);
impl Default for PCLState {
    fn default() -> Self {
        Self { base: Self::header(), statsWindowMessage: 0 }
    }
}

// ---------------------------------------------------------------------------------------------
// Function pointer types (sl_core_api.h, sl_dlss_g.h, sl_reflex.h, sl_pcl.h)
// C++ references are passed as pointers; `bool` is the 1-byte C++ bool.
// ---------------------------------------------------------------------------------------------

pub type PFun_slInit = unsafe extern "C" fn(pref: *const Preferences, sdkVersion: u64) -> SlResult;
pub type PFun_slShutdown = unsafe extern "C" fn() -> SlResult;
pub type PFun_slIsFeatureSupported = unsafe extern "C" fn(feature: Feature, adapterInfo: *const AdapterInfo) -> SlResult;
pub type PFun_slIsFeatureLoaded = unsafe extern "C" fn(feature: Feature, loaded: *mut bool) -> SlResult;
pub type PFun_slSetFeatureLoaded = unsafe extern "C" fn(feature: Feature, loaded: bool) -> SlResult;
pub type PFun_slEvaluateFeature = unsafe extern "C" fn(
    feature: Feature,
    frame: *const FrameToken,
    inputs: *const *const BaseStructure,
    numInputs: u32,
    cmdBuffer: *mut c_void,
) -> SlResult;
pub type PFun_slAllocateResources =
    unsafe extern "C" fn(cmdBuffer: *mut c_void, feature: Feature, viewport: *const ViewportHandle) -> SlResult;
pub type PFun_slFreeResources = unsafe extern "C" fn(feature: Feature, viewport: *const ViewportHandle) -> SlResult;
/// Deprecated; cannot be mixed with slSetTagForFrame.
pub type PFun_slSetTag = unsafe extern "C" fn(
    viewport: *const ViewportHandle,
    tags: *const ResourceTag,
    numTags: u32,
    cmdBuffer: *mut c_void,
) -> SlResult;
pub type PFun_slSetTagForFrame = unsafe extern "C" fn(
    frame: *const FrameToken,
    viewport: *const ViewportHandle,
    tags: *const ResourceTag,
    numTags: u32,
    cmdBuffer: *mut c_void,
) -> SlResult;
pub type PFun_slGetFeatureRequirements =
    unsafe extern "C" fn(feature: Feature, requirements: *mut FeatureRequirements) -> SlResult;
pub type PFun_slGetFeatureVersion = unsafe extern "C" fn(feature: Feature, version: *mut FeatureVersion) -> SlResult;
pub type PFun_slUpgradeInterface = unsafe extern "C" fn(baseInterface: *mut *mut c_void) -> SlResult;
pub type PFun_slSetConstants = unsafe extern "C" fn(
    values: *const Constants,
    frame: *const FrameToken,
    viewport: *const ViewportHandle,
) -> SlResult;
pub type PFun_slGetNativeInterface =
    unsafe extern "C" fn(proxyInterface: *mut c_void, baseInterface: *mut *mut c_void) -> SlResult;
pub type PFun_slGetFeatureFunction =
    unsafe extern "C" fn(feature: Feature, functionName: *const c_char, function: *mut *mut c_void) -> SlResult;
pub type PFun_slGetNewFrameToken = unsafe extern "C" fn(token: *mut *mut FrameToken, frameIndex: *const u32) -> SlResult;
pub type PFun_slSetD3DDevice = unsafe extern "C" fn(d3dDevice: *mut c_void) -> SlResult;

pub type PFun_slDLSSGGetState = unsafe extern "C" fn(
    viewport: *const ViewportHandle,
    state: *mut DLSSGState,
    options: *const DLSSGOptions,
) -> SlResult;
pub type PFun_slDLSSGSetOptions =
    unsafe extern "C" fn(viewport: *const ViewportHandle, options: *const DLSSGOptions) -> SlResult;

pub type PFun_slReflexGetState = unsafe extern "C" fn(state: *mut ReflexState) -> SlResult;
pub type PFun_slReflexSleep = unsafe extern "C" fn(frame: *const FrameToken) -> SlResult;
pub type PFun_slReflexSetOptions = unsafe extern "C" fn(options: *const ReflexOptions) -> SlResult;

pub type PFun_slPCLGetState = unsafe extern "C" fn(state: *mut PCLState) -> SlResult;
pub type PFun_slPCLSetMarker = unsafe extern "C" fn(marker: PCLMarker, frame: *const FrameToken) -> SlResult;
pub type PFun_slPCLSetOptions = unsafe extern "C" fn(options: *const PCLOptions) -> SlResult;

// ---------------------------------------------------------------------------------------------
// Runtime loader
// ---------------------------------------------------------------------------------------------

/// `GetProcAddress(module, name)` typed as `$ty`, or `None`.
macro_rules! proc_address {
    ($module:expr, $name:literal, $ty:ty) => {{
        // SAFETY: `module` is a loaded module and the name is a NUL-terminated literal.
        let raw = unsafe { GetProcAddress($module, PCSTR(concat!($name, "\0").as_ptr())) };
        // SAFETY: the export has the C signature declared by `$ty` (sl_core_api.h).
        raw.map(|f| unsafe { core::mem::transmute::<unsafe extern "system" fn() -> isize, $ty>(f) })
    }};
}

/// `slGetFeatureFunction(feature, name, ptr)` typed as `$ty`; failures are pushed to `$missing`.
macro_rules! feature_function {
    ($get:expr, $feature:expr, $name:literal, $ty:ty, $missing:ident) => {{
        let mut p: *mut c_void = ptr::null_mut();
        // SAFETY: NUL-terminated literal name and a valid out pointer.
        let r = unsafe { ($get)($feature, concat!($name, "\0").as_ptr().cast::<c_char>(), &mut p) };
        if r.is_ok() && !p.is_null() {
            // SAFETY: SL hands out the function declared as `$ty` in the feature header.
            Some(unsafe { core::mem::transmute::<*mut c_void, $ty>(p) })
        } else {
            $missing.push(format!("{} ({})", $name, r));
            None
        }
    }};
}

/// Function table for `sl.interposer.dll`. Core functions are resolved by [`Sl::load`]; feature
/// functions by [`Sl::resolve_feature_functions`] (only after `slSetD3DDevice`).
///
/// The module is never unloaded (SL keeps hooks and threads alive until process exit).
pub struct Sl {
    pub module: HMODULE,
    /// Absolute directory `sl.interposer.dll` was loaded from.
    pub dir: PathBuf,

    pub slInit: PFun_slInit,
    pub slShutdown: PFun_slShutdown,
    pub slSetD3DDevice: PFun_slSetD3DDevice,
    pub slIsFeatureSupported: PFun_slIsFeatureSupported,
    pub slIsFeatureLoaded: PFun_slIsFeatureLoaded,
    pub slSetFeatureLoaded: PFun_slSetFeatureLoaded,
    pub slGetNewFrameToken: PFun_slGetNewFrameToken,
    pub slSetConstants: PFun_slSetConstants,
    pub slSetTagForFrame: PFun_slSetTagForFrame,
    pub slEvaluateFeature: PFun_slEvaluateFeature,
    pub slUpgradeInterface: PFun_slUpgradeInterface,
    pub slGetNativeInterface: PFun_slGetNativeInterface,
    pub slGetFeatureRequirements: PFun_slGetFeatureRequirements,
    pub slGetFeatureFunction: PFun_slGetFeatureFunction,
    pub slAllocateResources: PFun_slAllocateResources,
    pub slFreeResources: PFun_slFreeResources,
    /// Optional exports.
    pub slGetFeatureVersion: Option<PFun_slGetFeatureVersion>,
    pub slSetTag: Option<PFun_slSetTag>,

    // Feature functions (slGetFeatureFunction)
    pub slDLSSGSetOptions: Option<PFun_slDLSSGSetOptions>,
    pub slDLSSGGetState: Option<PFun_slDLSSGGetState>,
    pub slReflexSetOptions: Option<PFun_slReflexSetOptions>,
    pub slReflexGetState: Option<PFun_slReflexGetState>,
    pub slReflexSleep: Option<PFun_slReflexSleep>,
    pub slPCLSetMarker: Option<PFun_slPCLSetMarker>,
    pub slPCLGetState: Option<PFun_slPCLGetState>,
    pub slPCLSetOptions: Option<PFun_slPCLSetOptions>,
}

// SAFETY: the table only holds immutable function pointers and a module handle; thread safety of
// each call is SL's (documented per function in sl_core_api.h / feature headers).
unsafe impl Send for Sl {}
// SAFETY: as above.
unsafe impl Sync for Sl {}

impl core::fmt::Debug for Sl {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_struct("Sl")
            .field("dir", &self.dir)
            .field("dlssg", &self.slDLSSGSetOptions.is_some())
            .field("reflex", &self.slReflexSetOptions.is_some())
            .field("pcl", &self.slPCLSetMarker.is_some())
            .finish_non_exhaustive()
    }
}

impl Sl {
    /// Loads `<dir>\sl.interposer.dll` (full path) and resolves the core exports.
    ///
    /// # Safety
    /// Loading the DLL runs its initialisation code. The caller should have verified the
    /// signature of the file if that matters (see module docs).
    pub unsafe fn load(dir: &Path) -> anyhow::Result<Self> {
        let dir = std::path::absolute(dir).with_context(|| format!("resolving {}", dir.display()))?;
        let dll = dir.join(INTERPOSER_DLL);
        if !dll.is_file() {
            bail!("{} not found", dll.display());
        }
        let wide: Vec<u16> = dll.as_os_str().encode_wide().chain(Some(0)).collect();
        // SAFETY: NUL-terminated full path; the caller accepts running the DLL's init code.
        let module = unsafe { LoadLibraryW(PCWSTR(wide.as_ptr())) }
            .with_context(|| format!("LoadLibraryW({})", dll.display()))?;

        macro_rules! need {
            ($name:literal, $ty:ty) => {
                proc_address!(module, $name, $ty)
                    .ok_or_else(|| anyhow!("{} does not export {}", dll.display(), $name))?
            };
        }

        Ok(Self {
            module,
            slInit: need!("slInit", PFun_slInit),
            slShutdown: need!("slShutdown", PFun_slShutdown),
            slSetD3DDevice: need!("slSetD3DDevice", PFun_slSetD3DDevice),
            slIsFeatureSupported: need!("slIsFeatureSupported", PFun_slIsFeatureSupported),
            slIsFeatureLoaded: need!("slIsFeatureLoaded", PFun_slIsFeatureLoaded),
            slSetFeatureLoaded: need!("slSetFeatureLoaded", PFun_slSetFeatureLoaded),
            slGetNewFrameToken: need!("slGetNewFrameToken", PFun_slGetNewFrameToken),
            slSetConstants: need!("slSetConstants", PFun_slSetConstants),
            slSetTagForFrame: need!("slSetTagForFrame", PFun_slSetTagForFrame),
            slEvaluateFeature: need!("slEvaluateFeature", PFun_slEvaluateFeature),
            slUpgradeInterface: need!("slUpgradeInterface", PFun_slUpgradeInterface),
            slGetNativeInterface: need!("slGetNativeInterface", PFun_slGetNativeInterface),
            slGetFeatureRequirements: need!("slGetFeatureRequirements", PFun_slGetFeatureRequirements),
            slGetFeatureFunction: need!("slGetFeatureFunction", PFun_slGetFeatureFunction),
            slAllocateResources: need!("slAllocateResources", PFun_slAllocateResources),
            slFreeResources: need!("slFreeResources", PFun_slFreeResources),
            slGetFeatureVersion: proc_address!(module, "slGetFeatureVersion", PFun_slGetFeatureVersion),
            slSetTag: proc_address!(module, "slSetTag", PFun_slSetTag),
            slDLSSGSetOptions: None,
            slDLSSGGetState: None,
            slReflexSetOptions: None,
            slReflexGetState: None,
            slReflexSleep: None,
            slPCLSetMarker: None,
            slPCLGetState: None,
            slPCLSetOptions: None,
            dir,
        })
    }

    /// Resolves the DLSS-G / Reflex / PCL functions with `slGetFeatureFunction`.
    /// Must be called AFTER `slSetD3DDevice`. Keeps whatever resolved; errors list the rest.
    pub fn resolve_feature_functions(&mut self) -> anyhow::Result<()> {
        let get = self.slGetFeatureFunction;
        let mut missing: Vec<String> = Vec::new();
        self.slDLSSGSetOptions =
            feature_function!(get, kFeatureDLSS_G, "slDLSSGSetOptions", PFun_slDLSSGSetOptions, missing);
        self.slDLSSGGetState = feature_function!(get, kFeatureDLSS_G, "slDLSSGGetState", PFun_slDLSSGGetState, missing);
        self.slReflexSetOptions =
            feature_function!(get, kFeatureReflex, "slReflexSetOptions", PFun_slReflexSetOptions, missing);
        self.slReflexGetState = feature_function!(get, kFeatureReflex, "slReflexGetState", PFun_slReflexGetState, missing);
        self.slReflexSleep = feature_function!(get, kFeatureReflex, "slReflexSleep", PFun_slReflexSleep, missing);
        self.slPCLSetMarker = feature_function!(get, kFeaturePCL, "slPCLSetMarker", PFun_slPCLSetMarker, missing);
        self.slPCLGetState = feature_function!(get, kFeaturePCL, "slPCLGetState", PFun_slPCLGetState, missing);
        self.slPCLSetOptions = feature_function!(get, kFeaturePCL, "slPCLSetOptions", PFun_slPCLSetOptions, missing);
        if missing.is_empty() {
            Ok(())
        } else {
            Err(anyhow!("Streamline feature functions not available: {}", missing.join(", ")))
        }
    }

    // ---- core wrappers ----------------------------------------------------------------------

    /// `slInit(pref, kSDKVersion)`.
    ///
    /// # Safety
    /// Every pointer in `pref` must be valid for the call (SL copies paths, features and strings).
    /// The log callback must stay valid until `slShutdown`.
    pub unsafe fn init(&self, pref: &Preferences) -> SlResult {
        // SAFETY: caller upholds the pointer requirements of `pref`.
        unsafe { (self.slInit)(pref, kSDKVersion) }
    }

    /// `slShutdown()` - before releasing DXGI/D3D objects.
    pub fn shutdown(&self) -> SlResult {
        // SAFETY: no arguments.
        unsafe { (self.slShutdown)() }
    }

    /// `slSetD3DDevice(nativeDevice)`.
    ///
    /// # Safety
    /// `native_device` must be a live NATIVE `ID3D12Device*` (not the SL proxy).
    pub unsafe fn set_d3d_device(&self, native_device: *mut c_void) -> SlResult {
        // SAFETY: caller guarantees a live device.
        unsafe { (self.slSetD3DDevice)(native_device) }
    }

    /// `slIsFeatureSupported(feature, AdapterInfo{LUID})`; `None` checks OS/driver only.
    pub fn is_feature_supported(&self, feature: Feature, adapter_luid: Option<&[u8; 8]>) -> SlResult {
        let mut luid = adapter_luid.copied().unwrap_or_default();
        let mut info = AdapterInfo::default();
        if adapter_luid.is_some() {
            info.deviceLUID = luid.as_mut_ptr();
            info.deviceLUIDSizeInBytes = 8;
        }
        // SAFETY: `info` and `luid` outlive the call.
        unsafe { (self.slIsFeatureSupported)(feature, &info) }
    }

    /// `slIsFeatureLoaded(feature, loaded)`.
    pub fn is_feature_loaded(&self, feature: Feature) -> Result<bool, SlResult> {
        let mut loaded = false;
        // SAFETY: valid out pointer.
        unsafe { (self.slIsFeatureLoaded)(feature, &mut loaded) }.ok()?;
        Ok(loaded)
    }

    /// `slSetFeatureLoaded(feature, loaded)` - load/unload (unhook) a feature. Flush the GPU and
    /// make no DXGI/D3D calls in parallel.
    pub fn set_feature_loaded(&self, feature: Feature, loaded: bool) -> SlResult {
        // SAFETY: plain values.
        unsafe { (self.slSetFeatureLoaded)(feature, loaded) }
    }

    /// `slGetNewFrameToken(token, frameIndex)`. Passing your own index bypasses SL's counter.
    pub fn new_frame_token(&self, frame_index: Option<u32>) -> Result<Frame, SlResult> {
        let mut token: *mut FrameToken = ptr::null_mut();
        let index = frame_index.unwrap_or(0);
        let index_ptr = if frame_index.is_some() { &index as *const u32 } else { ptr::null() };
        // SAFETY: valid out pointer; `index` outlives the call.
        unsafe { (self.slGetNewFrameToken)(&mut token, index_ptr) }.ok()?;
        NonNull::new(token).map(Frame).ok_or(SlResult::eErrorInvalidState)
    }

    /// `slSetConstants(values, frame, viewport)` - once per frame.
    pub fn set_constants(&self, values: &Constants, frame: Frame, viewport: &ViewportHandle) -> SlResult {
        // SAFETY: references are valid; the token came from SL.
        unsafe { (self.slSetConstants)(values, frame.as_ptr(), viewport) }
    }

    /// `slSetTagForFrame(frame, viewport, tags, numTags, cmdList)`.
    ///
    /// # Safety
    /// Every non-null `tags[i].resource` must point at a valid `Resource` whose `native` is a live
    /// `ID3D12Resource*`; `cmd_list` is a NATIVE open `ID3D12GraphicsCommandList*` or null (null
    /// only if all tags are null or `eValidUntilPresent`).
    pub unsafe fn set_tag_for_frame(
        &self,
        frame: Frame,
        viewport: &ViewportHandle,
        tags: &[ResourceTag],
        cmd_list: *mut c_void,
    ) -> SlResult {
        // SAFETY: caller guarantees the resource and command list pointers.
        unsafe { (self.slSetTagForFrame)(frame.as_ptr(), viewport, tags.as_ptr(), tags.len() as u32, cmd_list) }
    }

    /// Deprecated `slSetTag` (only without `eUseFrameBasedResourceTagging`).
    ///
    /// # Safety
    /// As [`Sl::set_tag_for_frame`].
    pub unsafe fn set_tag(&self, viewport: &ViewportHandle, tags: &[ResourceTag], cmd_list: *mut c_void) -> SlResult {
        match self.slSetTag {
            // SAFETY: caller guarantees the resource and command list pointers.
            Some(f) => unsafe { f(viewport, tags.as_ptr(), tags.len() as u32, cmd_list) },
            None => MISSING_FUNCTION,
        }
    }

    /// `slEvaluateFeature` (not needed for DLSS-G).
    ///
    /// # Safety
    /// `inputs` must point at live SL structs; `cmd_list` a native command list.
    pub unsafe fn evaluate_feature(
        &self,
        feature: Feature,
        frame: Frame,
        inputs: &[*const BaseStructure],
        cmd_list: *mut c_void,
    ) -> SlResult {
        // SAFETY: caller guarantees the inputs.
        unsafe { (self.slEvaluateFeature)(feature, frame.as_ptr(), inputs.as_ptr(), inputs.len() as u32, cmd_list) }
    }

    /// `slUpgradeInterface(&iface)` - replaces `*iface` in place with a new SL proxy
    /// (`ID3D12Device`, `IDXGIFactory*`, `IDXGISwapChain*`). The proxy has its own reference
    /// (count 1) and AddRefs the base; the caller's reference to the native object is NOT consumed.
    ///
    /// # Safety
    /// `*iface` must be a live COM pointer of a supported interface.
    pub unsafe fn upgrade_interface(&self, iface: &mut *mut c_void) -> SlResult {
        // SAFETY: caller guarantees a live COM pointer.
        unsafe { (self.slUpgradeInterface)(iface) }
    }

    /// `slGetNativeInterface(proxy, &base)` - returns an AddRef'd native pointer.
    ///
    /// # Safety
    /// `proxy` must be a live COM pointer.
    pub unsafe fn get_native_interface(&self, proxy: *mut c_void) -> Result<*mut c_void, SlResult> {
        let mut base: *mut c_void = ptr::null_mut();
        // SAFETY: caller guarantees a live COM pointer; valid out pointer.
        unsafe { (self.slGetNativeInterface)(proxy, &mut base) }.ok()?;
        Ok(base)
    }

    /// `slGetFeatureRequirements(feature, requirements)`.
    pub fn get_feature_requirements(&self, feature: Feature) -> Result<FeatureRequirements, SlResult> {
        let mut req = FeatureRequirements::default();
        // SAFETY: valid out struct with a correct header.
        unsafe { (self.slGetFeatureRequirements)(feature, &mut req) }.ok()?;
        Ok(req)
    }

    /// `slGetFeatureVersion(feature, version)`.
    pub fn get_feature_version(&self, feature: Feature) -> Result<FeatureVersion, SlResult> {
        let f = self.slGetFeatureVersion.ok_or(MISSING_FUNCTION)?;
        let mut v = FeatureVersion::default();
        // SAFETY: valid out struct.
        unsafe { f(feature, &mut v) }.ok()?;
        Ok(v)
    }

    /// `slGetFeatureFunction(feature, name, &fn)` (raw).
    ///
    /// # Safety
    /// The caller must transmute the pointer to the exact signature of `name`.
    pub unsafe fn get_feature_function(&self, feature: Feature, name: &CStr) -> Result<*mut c_void, SlResult> {
        let mut p: *mut c_void = ptr::null_mut();
        // SAFETY: NUL-terminated name, valid out pointer.
        unsafe { (self.slGetFeatureFunction)(feature, name.as_ptr(), &mut p) }.ok()?;
        Ok(p)
    }

    /// `slAllocateResources(cmdList, feature, viewport)`.
    ///
    /// # Safety
    /// `cmd_list` is a native command list on the SL device, or null.
    pub unsafe fn allocate_resources(&self, cmd_list: *mut c_void, feature: Feature, viewport: &ViewportHandle) -> SlResult {
        // SAFETY: caller guarantees the command list.
        unsafe { (self.slAllocateResources)(cmd_list, feature, viewport) }
    }

    /// `slFreeResources(feature, viewport)` - flush pending work first.
    pub fn free_resources(&self, feature: Feature, viewport: &ViewportHandle) -> SlResult {
        // SAFETY: valid reference.
        unsafe { (self.slFreeResources)(feature, viewport) }
    }

    // ---- DLSS-G -----------------------------------------------------------------------------

    /// `slDLSSGSetOptions(viewport, options)` - applies at the next Present; call on the present thread.
    pub fn dlssg_set_options(&self, viewport: &ViewportHandle, options: &DLSSGOptions) -> SlResult {
        match self.slDLSSGSetOptions {
            // SAFETY: valid references; `onErrorCallback` (if any) is a valid fn pointer.
            Some(f) => unsafe { f(viewport, options) },
            None => MISSING_FUNCTION,
        }
    }

    /// `slDLSSGGetState(viewport, state, options)`. Pass `None` for the cheap per-frame query.
    pub fn dlssg_get_state(&self, viewport: &ViewportHandle, options: Option<&DLSSGOptions>) -> Result<DLSSGState, SlResult> {
        let f = self.slDLSSGGetState.ok_or(MISSING_FUNCTION)?;
        let mut state = DLSSGState::default();
        let opts = options.map_or(ptr::null(), |o| o as *const DLSSGOptions);
        // SAFETY: valid references / out struct.
        unsafe { f(viewport, &mut state, opts) }.ok()?;
        Ok(state)
    }

    // ---- Reflex -----------------------------------------------------------------------------

    /// `slReflexSetOptions(options)` - at least once, even with Reflex off.
    pub fn reflex_set_options(&self, options: &ReflexOptions) -> SlResult {
        match self.slReflexSetOptions {
            // SAFETY: valid reference.
            Some(f) => unsafe { f(options) },
            None => MISSING_FUNCTION,
        }
    }

    /// `slReflexGetState(state)` (boxed: 12.8 KB).
    pub fn reflex_get_state(&self) -> Result<Box<ReflexState>, SlResult> {
        let f = self.slReflexGetState.ok_or(MISSING_FUNCTION)?;
        let mut state = Box::<ReflexState>::default();
        // SAFETY: valid out struct.
        unsafe { f(&mut *state) }.ok()?;
        Ok(state)
    }

    /// `slReflexSleep(frame)` - every frame, regardless of Reflex mode.
    pub fn reflex_sleep(&self, frame: Frame) -> SlResult {
        match self.slReflexSleep {
            // SAFETY: token from SL.
            Some(f) => unsafe { f(frame.as_ptr()) },
            None => MISSING_FUNCTION,
        }
    }

    // ---- PCL --------------------------------------------------------------------------------

    /// `slPCLSetMarker(marker, frame)` (thread safe).
    pub fn pcl_set_marker(&self, marker: PCLMarker, frame: Frame) -> SlResult {
        match self.slPCLSetMarker {
            // SAFETY: token from SL.
            Some(f) => unsafe { f(marker, frame.as_ptr()) },
            None => MISSING_FUNCTION,
        }
    }

    /// `slPCLGetState(state)` - `statsWindowMessage` for the ping.
    pub fn pcl_get_state(&self) -> Result<PCLState, SlResult> {
        let f = self.slPCLGetState.ok_or(MISSING_FUNCTION)?;
        let mut state = PCLState::default();
        // SAFETY: valid out struct.
        unsafe { f(&mut state) }.ok()?;
        Ok(state)
    }

    /// `slPCLSetOptions(options)`.
    pub fn pcl_set_options(&self, options: &PCLOptions) -> SlResult {
        match self.slPCLSetOptions {
            // SAFETY: valid reference.
            Some(f) => unsafe { f(options) },
            None => MISSING_FUNCTION,
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Layout checks (x64 MSVC sizes/offsets computed from the 2.14.16 headers)
// ---------------------------------------------------------------------------------------------

#[cfg(target_pointer_width = "64")]
const _: () = {
    use core::mem::{align_of, offset_of, size_of};

    assert!(size_of::<StructType>() == 16);
    assert!(size_of::<BaseStructure>() == 32);
    assert!(offset_of!(BaseStructure, structType) == 8);
    assert!(offset_of!(BaseStructure, structVersion) == 24);

    assert!(size_of::<Preferences>() == 144);
    assert!(offset_of!(Preferences, showConsole) == 32);
    assert!(offset_of!(Preferences, logLevel) == 36);
    assert!(offset_of!(Preferences, pathsToPlugins) == 40);
    assert!(offset_of!(Preferences, numPathsToPlugins) == 48);
    assert!(offset_of!(Preferences, pathToLogsAndData) == 56);
    assert!(offset_of!(Preferences, logMessageCallback) == 80);
    assert!(offset_of!(Preferences, flags) == 88);
    assert!(offset_of!(Preferences, featuresToLoad) == 96);
    assert!(offset_of!(Preferences, numFeaturesToLoad) == 104);
    assert!(offset_of!(Preferences, applicationId) == 108);
    assert!(offset_of!(Preferences, engine) == 112);
    assert!(offset_of!(Preferences, engineVersion) == 120);
    assert!(offset_of!(Preferences, projectId) == 128);
    assert!(offset_of!(Preferences, renderAPI) == 136);

    assert!(size_of::<ResourceAllocationDesc>() == 64);

    assert!(size_of::<Resource>() == 112);
    assert!(offset_of!(Resource, r#type) == 32);
    assert!(offset_of!(Resource, native) == 40);
    assert!(offset_of!(Resource, state) == 64);
    assert!(offset_of!(Resource, gpuVirtualAddress) == 88);
    assert!(offset_of!(Resource, flags) == 96);
    assert!(offset_of!(Resource, internalFlags) == 104);

    assert!(size_of::<Extent>() == 16);
    assert!(size_of::<ResourceTag>() == 64);
    assert!(offset_of!(ResourceTag, resource) == 32);
    assert!(offset_of!(ResourceTag, r#type) == 40);
    assert!(offset_of!(ResourceTag, lifecycle) == 44);
    assert!(offset_of!(ResourceTag, extent) == 48);

    assert!(size_of::<ViewportHandle>() == 40);
    assert!(size_of::<AdapterInfo>() == 56);
    assert!(size_of::<Version>() == 12);
    assert!(size_of::<FeatureRequirements>() == 184);
    assert!(offset_of!(FeatureRequirements, osVersionDetected) == 56);
    assert!(offset_of!(FeatureRequirements, vkNumComputeQueuesRequired) == 104);
    assert!(offset_of!(FeatureRequirements, vkNumOpticalFlowQueuesRequired) == 176);
    assert!(size_of::<FeatureVersion>() == 56);

    assert!(size_of::<float4x4>() == 64);
    assert!(size_of::<Constants>() == 456);
    assert!(offset_of!(Constants, jitterOffset) == 352);
    assert!(offset_of!(Constants, mvecScale) == 360);
    assert!(offset_of!(Constants, cameraPos) == 376);
    assert!(offset_of!(Constants, cameraFwd) == 412);
    assert!(offset_of!(Constants, cameraNear) == 424);
    assert!(offset_of!(Constants, motionVectorsInvalidValue) == 440);
    assert!(offset_of!(Constants, depthInverted) == 444);
    assert!(offset_of!(Constants, motionVectorsJittered) == 450);
    assert!(offset_of!(Constants, minRelativeLinearDepthObjectSeparation) == 452);

    assert!(size_of::<DLSSGOptions>() == 120);
    assert!(offset_of!(DLSSGOptions, mode) == 32);
    assert!(offset_of!(DLSSGOptions, uiBufferFormat) == 88);
    assert!(offset_of!(DLSSGOptions, onErrorCallback) == 96);
    assert!(offset_of!(DLSSGOptions, bReserved15) == 104);
    assert!(offset_of!(DLSSGOptions, queueParallelismMode) == 108);
    assert!(offset_of!(DLSSGOptions, enableUserInterfaceRecomposition) == 112);
    assert!(offset_of!(DLSSGOptions, dynamicTargetFrameRate) == 116);

    assert!(size_of::<DLSSGState>() == 88);
    assert!(offset_of!(DLSSGState, status) == 40);
    assert!(offset_of!(DLSSGState, numFramesToGenerateMax) == 52);
    assert!(offset_of!(DLSSGState, bIsVsyncSupportAvailable) == 57);
    assert!(offset_of!(DLSSGState, inputsProcessingCompletionFence) == 64);
    assert!(offset_of!(DLSSGState, bIsDynamicMFGSupported) == 80);

    assert!(size_of::<ReflexOptions>() == 48);
    assert!(offset_of!(ReflexOptions, useMarkersToOptimize) == 40);
    assert!(offset_of!(ReflexOptions, virtualKey) == 42);
    assert!(offset_of!(ReflexOptions, idThread) == 44);
    assert!(size_of::<ReflexReport>() == 152);
    assert!(size_of::<ReflexReport2>() == 48);
    assert!(size_of::<ReflexState>() == 12848);
    assert!(offset_of!(ReflexState, frameReport) == 40);
    assert!(offset_of!(ReflexState, flashIndicatorDriverControlled) == 9768);
    assert!(offset_of!(ReflexState, frameReport2) == 9776);

    assert!(size_of::<PCLOptions>() == 40);
    assert!(offset_of!(PCLOptions, idThread) == 36);
    assert!(size_of::<PCLState>() == 40);

    assert!(size_of::<Boolean>() == 1 && size_of::<ResourceType>() == 1);
    assert!(size_of::<SlResult>() == 4 && align_of::<SlResult>() == 4);
    assert!(size_of::<Option<PFun_LogMessageCallback>>() == 8);
};
