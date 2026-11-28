//! Leonida: the trainer menu (`leonida-menu`; F4, or RB with d-pad Left)
//! laid over the picture at present, beside the F1 settings menu
//! (`super::menu`) and the F3 numbers panel (`super::panel`), in every
//! build.
//!
//! The menu's thread paints it on the CPU when it changes; here the newest
//! picture is copied into a texture of ours and drawn onto the swap chain's
//! buffer in one quad, blended (premultiplied alpha), where the panel and
//! the F1 menu are copied onto it: after the title's picture (the two
//! planes laid over each other by `crate::compose`, or the one plane
//! copied) and before `Present`, so frame generation sees it as it sees
//! them. Inside the highlight band the quad shows the picture's "selected"
//! half, so the band slides between rows with no new picture. While the
//! panel is shown the menu stands below it.
//!
//! A present asks `leonida_menu::work()` once (one atomic load) and comes
//! here only while the menu is open or a screenshot is asked for. Saved
//! frames and screenshots are read back before the menu is drawn: they show
//! the title's picture.
//!
//! Also here: what this renderer lends the menu ([`provide`]): its
//! frame-rate cap (`crate::frame`), DLSS's sharpness, the panel's switch,
//! the frame recorder's `rec` file and screenshots (`crate::capture`).

use std::time::Instant;

use windows::Win32::Graphics::Direct3D::Fxc::D3DCompile;
use windows::Win32::Graphics::Direct3D::{D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, ID3DBlob};
use windows::Win32::Graphics::Direct3D12::*;
use windows::Win32::Graphics::Dxgi::Common::*;
use windows::core::{IUnknown, Interface};

use leonida_menu::knobs::{self, Cap};
use leonida_menu::paint::{LEFT, TOP};

use super::{Host, HostList, transition};

const SOURCE: &str = r#"
#define RS "RootConstants(num32BitConstants=8, b0), DescriptorTable(SRV(t0))"
cbuffer Place : register(b0) { float2 target; float2 origin; float2 size; float band_top; float band_bottom; };
Texture2D<float4> menu : register(t0);
struct V { float4 pos : SV_Position; };
[RootSignature(RS)]
V vs(uint id : SV_VertexID) {
    float2 corner = float2(id & 1, id >> 1);
    float2 px = origin + corner * size;
    V v;
    v.pos = float4(px / target * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return v;
}
[RootSignature(RS)]
float4 ps(V v) : SV_Target {
    float2 p = v.pos.xy - origin;
    int layer = (p.y >= band_top && p.y < band_bottom) ? 1 : 0;
    return menu.Load(int3(int(p.x) + layer * int(size.x), int(p.y), 0));
}
"#;

/// Uploads written in turn: the card reads one while the next is written
/// (a present sent ahead runs a frame or two after it was recorded).
const UPLOADS: usize = 4;

/// The gap between the panel and the menu below it, at a 1080-line picture.
const UNDER_PANEL: f32 = 8.0;

/// Not drawn for this long, the menu was closed in between: the highlight
/// band starts where it is, without sliding there.
const AWAY: f32 = 0.25;

fn compile(
    entry: windows::core::PCSTR,
    target: windows::core::PCSTR,
) -> windows::core::Result<ID3DBlob> {
    let mut code: Option<ID3DBlob> = None;
    let mut errors: Option<ID3DBlob> = None;
    // SAFETY: the source and names live through the call; out pointers are ours.
    let made = unsafe {
        D3DCompile(
            SOURCE.as_ptr().cast(),
            SOURCE.len(),
            windows::core::s!("leonida trainer menu"),
            None,
            None,
            entry,
            target,
            0,
            0,
            &mut code,
            Some(&mut errors),
        )
    };
    if let Err(e) = made {
        if let Some(errors) = errors {
            // SAFETY: the blob's own bytes.
            let text = unsafe {
                std::slice::from_raw_parts(
                    errors.GetBufferPointer().cast::<u8>(),
                    errors.GetBufferSize(),
                )
            };
            tracing::warn!(errors = %String::from_utf8_lossy(text), "trainer menu shader not compiled");
        }
        return Err(e);
    }
    code.ok_or_else(windows::core::Error::empty)
}

fn bytes(blob: &ID3DBlob) -> &[u8] {
    // SAFETY: the blob's own bytes, alive as long as it is.
    unsafe {
        std::slice::from_raw_parts(blob.GetBufferPointer().cast::<u8>(), blob.GetBufferSize())
    }
}

/// The pipeline for one target format.
struct Pipeline {
    root: ID3D12RootSignature,
    pso: ID3D12PipelineState,
    format: DXGI_FORMAT,
}

fn pipeline(device: &ID3D12Device, format: DXGI_FORMAT) -> windows::core::Result<Pipeline> {
    let vs = compile(windows::core::s!("vs"), windows::core::s!("vs_5_1"))?;
    let ps = compile(windows::core::s!("ps"), windows::core::s!("ps_5_1"))?;
    // SAFETY: the blobs' own bytes (the root signature is inside them);
    // plain object creation.
    unsafe {
        let root: ID3D12RootSignature = device.CreateRootSignature(0, bytes(&vs))?;
        let mut blend = D3D12_BLEND_DESC::default();
        blend.RenderTarget[0] = D3D12_RENDER_TARGET_BLEND_DESC {
            BlendEnable: true.into(),
            LogicOpEnable: false.into(),
            SrcBlend: D3D12_BLEND_ONE,
            DestBlend: D3D12_BLEND_INV_SRC_ALPHA,
            BlendOp: D3D12_BLEND_OP_ADD,
            SrcBlendAlpha: D3D12_BLEND_ONE,
            DestBlendAlpha: D3D12_BLEND_INV_SRC_ALPHA,
            BlendOpAlpha: D3D12_BLEND_OP_ADD,
            LogicOp: D3D12_LOGIC_OP_NOOP,
            RenderTargetWriteMask: D3D12_COLOR_WRITE_ENABLE_ALL.0 as u8,
        };
        let mut formats = [DXGI_FORMAT_UNKNOWN; 8];
        formats[0] = format;
        let desc = D3D12_GRAPHICS_PIPELINE_STATE_DESC {
            pRootSignature: std::mem::transmute_copy(&root),
            VS: D3D12_SHADER_BYTECODE {
                pShaderBytecode: vs.GetBufferPointer(),
                BytecodeLength: vs.GetBufferSize(),
            },
            PS: D3D12_SHADER_BYTECODE {
                pShaderBytecode: ps.GetBufferPointer(),
                BytecodeLength: ps.GetBufferSize(),
            },
            BlendState: blend,
            SampleMask: u32::MAX,
            RasterizerState: D3D12_RASTERIZER_DESC {
                FillMode: D3D12_FILL_MODE_SOLID,
                CullMode: D3D12_CULL_MODE_NONE,
                DepthClipEnable: true.into(),
                ..Default::default()
            },
            PrimitiveTopologyType: D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
            NumRenderTargets: 1,
            RTVFormats: formats,
            SampleDesc: DXGI_SAMPLE_DESC {
                Count: 1,
                Quality: 0,
            },
            ..Default::default()
        };
        let pso = device.CreateGraphicsPipelineState(&desc)?;
        Ok(Pipeline { root, pso, format })
    }
}

/// The menu's texture (both halves), its view and the uploads that fill it.
struct Picture {
    texture: ID3D12Resource,
    /// The one view of the texture, in a heap of its own: a present sent
    /// ahead keeps the heap, so a new picture's view never lands under a
    /// list that has not run.
    view: ID3D12DescriptorHeap,
    /// The texture is in the copy-destination state (else pixel-shader resource).
    writable: bool,
    width: u32,
    height: u32,
    pitch: u32,
    uploads: Vec<(ID3D12Resource, *mut u8)>,
    turn: usize,
}

fn upload(device: &ID3D12Device, size: u64) -> Option<(ID3D12Resource, *mut u8)> {
    let heap = D3D12_HEAP_PROPERTIES {
        Type: D3D12_HEAP_TYPE_UPLOAD,
        ..Default::default()
    };
    let desc = D3D12_RESOURCE_DESC {
        Dimension: D3D12_RESOURCE_DIMENSION_BUFFER,
        Width: size.max(1),
        Height: 1,
        DepthOrArraySize: 1,
        MipLevels: 1,
        SampleDesc: DXGI_SAMPLE_DESC {
            Count: 1,
            Quality: 0,
        },
        Layout: D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        ..Default::default()
    };
    let mut buffer: Option<ID3D12Resource> = None;
    let mut at = std::ptr::null_mut();
    // SAFETY: plain creation of an upload buffer, mapped for good.
    unsafe {
        device
            .CreateCommittedResource(
                &heap,
                D3D12_HEAP_FLAG_NONE,
                &desc,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                None,
                &mut buffer,
            )
            .ok()?;
        let buffer = buffer?;
        buffer.Map(0, None, Some(&mut at)).ok()?;
        Some((buffer, at.cast()))
    }
}

impl Picture {
    fn new(device: &ID3D12Device, width: u32, height: u32) -> Option<Picture> {
        let heap = D3D12_HEAP_PROPERTIES {
            Type: D3D12_HEAP_TYPE_DEFAULT,
            ..Default::default()
        };
        let desc = D3D12_RESOURCE_DESC {
            Dimension: D3D12_RESOURCE_DIMENSION_TEXTURE2D,
            Width: u64::from(2 * width),
            Height: height,
            DepthOrArraySize: 1,
            MipLevels: 1,
            Format: DXGI_FORMAT_R8G8B8A8_UNORM,
            SampleDesc: DXGI_SAMPLE_DESC {
                Count: 1,
                Quality: 0,
            },
            Layout: D3D12_TEXTURE_LAYOUT_UNKNOWN,
            ..Default::default()
        };
        let mut texture: Option<ID3D12Resource> = None;
        // SAFETY: plain creation of a device-local texture, a heap of one
        // view and that view.
        let (texture, view) = unsafe {
            device
                .CreateCommittedResource(
                    &heap,
                    D3D12_HEAP_FLAG_NONE,
                    &desc,
                    D3D12_RESOURCE_STATE_COPY_DEST,
                    None,
                    &mut texture,
                )
                .ok()?;
            let texture = texture?;
            let view: ID3D12DescriptorHeap = device
                .CreateDescriptorHeap(&D3D12_DESCRIPTOR_HEAP_DESC {
                    Type: D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                    NumDescriptors: 1,
                    Flags: D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
                    NodeMask: 0,
                })
                .ok()?;
            device.CreateShaderResourceView(
                &texture,
                None,
                view.GetCPUDescriptorHandleForHeapStart(),
            );
            (texture, view)
        };
        let pitch = (2 * width * 4).next_multiple_of(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
        let size = u64::from(pitch) * u64::from(height);
        let uploads = (0..UPLOADS)
            .map(|_| upload(device, size))
            .collect::<Option<Vec<_>>>()?;
        Some(Picture {
            texture,
            view,
            writable: true,
            width,
            height,
            pitch,
            uploads,
            turn: 0,
        })
    }
}

/// The trainer menu's overlay of a presenter.
pub(super) struct Overlay {
    pipeline: Option<Pipeline>,
    picture: Option<Picture>,
    /// The version of the menu's picture in the texture.
    shown: u64,
    /// Where the band is now, in the picture's pixels, and the page, scroll
    /// and depth it was on.
    band: f32,
    place: (usize, usize, usize),
    last: Option<Instant>,
    failed: bool,
}

// SAFETY: free-threaded D3D12 objects; mapped memory only written under the presenter's lock.
unsafe impl Send for Overlay {}

impl Overlay {
    /// The overlay of a presenter whose picture is `height` lines high. The
    /// menu's thread is started here (not with `LEONIDA_MENU=0`) and told
    /// what this renderer does for it, so that a present has nothing to
    /// look after while the menu is closed.
    pub(super) fn new(height: u32) -> Overlay {
        provide();
        leonida_menu::set_height(height);
        // The Lua mods first: the menu then has its Mods page from the start.
        leonida_lua::start();
        leonida_menu::start();
        Overlay::idle()
    }

    pub(super) fn idle() -> Overlay {
        Overlay {
            pipeline: None,
            picture: None,
            shown: 0,
            band: 0.0,
            place: (usize::MAX, 0, 0),
            last: None,
            failed: false,
        }
    }

    /// Records the menu onto `back` (a swap chain buffer of that size and
    /// format, in the copy-destination state, left in it). Called while
    /// `leonida_menu::work()` says the menu is open.
    pub(super) fn draw(
        &mut self,
        host: &Host,
        list: &HostList,
        back: &ID3D12Resource,
        (width, height): (u32, u32),
        format: DXGI_FORMAT,
    ) {
        if self.failed {
            return;
        }
        let Some(frame) = leonida_menu::frame(height) else {
            return;
        };
        self.draw_frame(host, list, back, (width, height), format, frame, true);
    }

    /// Records what the Lua mods draw (`leonida_menu::overlay`) onto `back`,
    /// as [`Overlay::draw`] does the menu, at the place the drawing says.
    pub(super) fn draw_lua(
        &mut self,
        host: &Host,
        list: &HostList,
        back: &ID3D12Resource,
        size: (u32, u32),
        format: DXGI_FORMAT,
    ) {
        if self.failed {
            return;
        }
        let Some(frame) = leonida_menu::overlay::frame() else {
            return;
        };
        self.draw_frame(host, list, back, size, format, frame, false);
    }

    #[allow(clippy::too_many_arguments)]
    fn draw_frame(
        &mut self,
        host: &Host,
        list: &HostList,
        back: &ID3D12Resource,
        (width, height): (u32, u32),
        format: DXGI_FORMAT,
        frame: leonida_menu::Frame,
        is_menu: bool,
    ) {
        // Not the title's: the frame recorder leaves it out.
        #[cfg(feature = "diagnostics")]
        let _ours = crate::replay::record::unrecorded();
        // Below the numbers panel while that is shown (the menu only).
        #[cfg(feature = "diagnostics")]
        let under = if is_menu { super::panel::lines_taken(height) } else { 0 };
        #[cfg(not(feature = "diagnostics"))]
        let under = {
            let _ = is_menu;
            0
        };
        let target = host.rtv(Some(back), None);
        let drawn = self.record(
            &host.device,
            &list.list,
            &|object| list.keep(object),
            (back, target),
            (width, height),
            format,
            under,
            &frame,
        );
        if let Err(e) = drawn {
            tracing::warn!(error = %e, "the trainer menu could not be drawn; it stays off");
            self.failed = true;
        }
    }

    /// Records the menu onto `back` through its view `target`, the menu's
    /// top no higher than `under` lines down; `keep` holds what the list
    /// uses until it has run.
    #[allow(clippy::too_many_arguments)]
    fn record(
        &mut self,
        device: &ID3D12Device,
        list: &ID3D12GraphicsCommandList,
        keep: &dyn Fn(&IUnknown),
        (back, target): (&ID3D12Resource, D3D12_CPU_DESCRIPTOR_HANDLE),
        (width, height): (u32, u32),
        format: DXGI_FORMAT,
        under: u32,
        frame: &leonida_menu::Frame,
    ) -> windows::core::Result<()> {
        let layers = &frame.layers;
        if self.pipeline.as_ref().is_none_or(|p| p.format != format) {
            self.pipeline = Some(pipeline(device, format)?);
        }
        let fits = |p: &Picture| p.width == layers.width && p.height == layers.height;
        if self.picture.as_ref().is_none_or(|p| !fits(p)) {
            // (Lists that drew the old one keep it until they have run.)
            self.picture = Picture::new(device, layers.width, layers.height);
            self.shown = 0;
        }
        let Some(picture) = self.picture.as_mut() else {
            return Err(windows::core::Error::empty());
        };
        let Some(pipeline) = self.pipeline.as_ref() else {
            return Err(windows::core::Error::empty());
        };
        keep(&picture.texture.cast()?);
        keep(&picture.view.cast()?);
        keep(&pipeline.root.cast()?);
        keep(&pipeline.pso.cast()?);

        // The band: snapped on a new page, scroll or a reopened menu, else sliding.
        let now = Instant::now();
        let to = layers.band_top() as f32;
        let place = (layers.page, layers.scroll, layers.depth);
        match self.last.map(|last| (now - last).as_secs_f32()) {
            Some(dt) if place == self.place && dt < AWAY => {
                self.band += (to - self.band) * (1.0 - (-dt.min(0.1) * 28.0).exp());
                if (to - self.band).abs() < 0.5 {
                    self.band = to;
                }
            }
            _ => self.band = to,
        }
        self.place = place;
        self.last = Some(now);

        // SAFETY: an open list; live resources; uploads written only here,
        // the oldest of `UPLOADS` (the card read it presents ago).
        unsafe {
            let mut before = vec![transition(
                back,
                D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_RENDER_TARGET,
            )];
            if self.shown != frame.version {
                picture.turn = (picture.turn + 1) % UPLOADS;
                let (buffer, at) = &picture.uploads[picture.turn];
                keep(&buffer.cast()?);
                let row = (2 * layers.width * 4) as usize;
                for y in 0..layers.height as usize {
                    std::ptr::copy_nonoverlapping(
                        layers.pixels.as_ptr().add(y * row),
                        at.add(y * picture.pitch as usize),
                        row,
                    );
                }
                if !picture.writable {
                    list.ResourceBarrier(&[transition(
                        &picture.texture,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_COPY_DEST,
                    )]);
                }
                let from = D3D12_TEXTURE_COPY_LOCATION {
                    pResource: std::mem::transmute_copy(buffer),
                    Type: D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                    Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 {
                        PlacedFootprint: D3D12_PLACED_SUBRESOURCE_FOOTPRINT {
                            Offset: 0,
                            Footprint: D3D12_SUBRESOURCE_FOOTPRINT {
                                Format: DXGI_FORMAT_R8G8B8A8_UNORM,
                                Width: 2 * layers.width,
                                Height: layers.height,
                                Depth: 1,
                                RowPitch: picture.pitch,
                            },
                        },
                    },
                };
                let into = D3D12_TEXTURE_COPY_LOCATION {
                    pResource: std::mem::transmute_copy(&picture.texture),
                    Type: D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                    Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 {
                        SubresourceIndex: 0,
                    },
                };
                list.CopyTextureRegion(&into, 0, 0, 0, &from, None);
                picture.writable = true;
                self.shown = frame.version;
            }
            if picture.writable {
                before.push(transition(
                    &picture.texture,
                    D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                ));
                picture.writable = false;
            }
            list.ResourceBarrier(&before);
            let top = (TOP * layers.scale).round();
            let top = if under == 0 {
                top
            } else {
                top.max(under as f32 + (UNDER_PANEL * layers.scale).round())
            };
            let origin = match frame.origin {
                // A drawing of the mods' stands where it says.
                Some((x, y)) => (x as f32, y as f32),
                None => ((LEFT * layers.scale).round(), top),
            };
            let band = if layers.rows == 0 {
                (-1.0, -1.0)
            } else {
                (self.band, self.band + layers.row_height as f32)
            };
            let constants: [f32; 8] = [
                width as f32,
                height as f32,
                origin.0,
                origin.1,
                layers.width as f32,
                layers.height as f32,
                band.0,
                band.1,
            ];
            list.SetGraphicsRootSignature(&pipeline.root);
            list.SetPipelineState(&pipeline.pso);
            list.SetDescriptorHeaps(&[Some(picture.view.clone())]);
            list.SetGraphicsRootDescriptorTable(
                1,
                picture.view.GetGPUDescriptorHandleForHeapStart(),
            );
            list.SetGraphicsRoot32BitConstants(0, 8, constants.as_ptr().cast(), 0);
            list.RSSetViewports(&[D3D12_VIEWPORT {
                TopLeftX: 0.0,
                TopLeftY: 0.0,
                Width: width as f32,
                Height: height as f32,
                MinDepth: 0.0,
                MaxDepth: 1.0,
            }]);
            list.RSSetScissorRects(&[windows::Win32::Foundation::RECT {
                left: 0,
                top: 0,
                right: width as i32,
                bottom: height as i32,
            }]);
            list.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            list.OMSetRenderTargets(1, Some(&target), false, None);
            list.DrawInstanced(4, 1, 0, 0);
            list.ResourceBarrier(&[transition(
                back,
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_COPY_DEST,
            )]);
        }
        Ok(())
    }
}

/// Sets or (empty) unsets a variable this build reads at each use.
fn set_env(var: &str, value: &str) {
    // SAFETY: as the F1 menu does (`super::menu`): the variables changed
    // here are read by this build with std::env::var at each use; Windows'
    // environment block is locked by the process heap's routines, and std
    // holds its own lock.
    unsafe {
        if value.is_empty() {
            std::env::remove_var(var);
        } else {
            std::env::set_var(var, value);
        }
    }
}

/// What `LEONIDA_FPS_CAP` says, read as `crate::frame` reads it.
fn cap_of(value: Option<&str>) -> Cap {
    match value.map(str::trim).and_then(|v| v.parse::<u32>().ok()) {
        None => Cap::Title,
        Some(0) => Cap::Unlimited,
        Some(n) => Cap::Fps(n),
    }
}

/// The value of `LEONIDA_FPS_CAP` for `cap` (empty: unset).
fn cap_text(cap: Cap) -> String {
    match cap {
        Cap::Title => String::new(),
        Cap::Unlimited => "0".to_owned(),
        Cap::Fps(n) => n.to_string(),
    }
}

fn cap() -> Cap {
    cap_of(std::env::var("LEONIDA_FPS_CAP").ok().as_deref())
}

/// The frame-rate cap is the frame clock's (`crate::frame`), changed as the
/// F1 menu changes it: the variable, then the clock.
fn set_cap(cap: Cap) -> Result<(), String> {
    let value = cap_text(cap);
    set_env("LEONIDA_FPS_CAP", &value);
    crate::frame::set_cap(&value);
    Ok(())
}

/// Whether the title's upscale is DLSS in this run, as far as the build and
/// the settings say (an SDK that did not start leaves a stretch).
fn dlss_asked() -> bool {
    cfg!(feature = "dlss") && std::env::var_os("LEONIDA_UPSCALE").is_some_and(|v| v == "dlss")
}

fn sharpness_of(value: Option<&str>) -> f32 {
    value
        .and_then(|v| v.trim().parse::<f32>().ok())
        .filter(|v| v.is_finite())
        .map_or(0.0, |v| v.clamp(0.0, 1.0))
}

/// `LEONIDA_DLSS_SHARPNESS` for `v`: two decimals at most, as the F1 menu
/// writes its values (`0.5`, `1`).
fn sharpness_text(v: f32) -> String {
    let text = format!("{:.2}", v.clamp(0.0, 1.0));
    text.trim_end_matches('0').trim_end_matches('.').to_owned()
}

/// DLSS's sharpness is the variable `crate::dlss` reads at every upscale.
fn sharpness() -> f32 {
    sharpness_of(std::env::var("LEONIDA_DLSS_SHARPNESS").ok().as_deref())
}

fn set_sharpness(v: f32) -> Result<(), String> {
    set_env("LEONIDA_DLSS_SHARPNESS", &sharpness_text(v));
    Ok(())
}

/// The frame recorder is on while a file named `rec` is in the capture
/// folder (`crate::capture::recording`): the menu's switch is that file.
#[cfg(feature = "diagnostics")]
fn recording() -> bool {
    crate::capture::folder().is_some_and(|folder| folder.join("rec").exists())
}

#[cfg(feature = "diagnostics")]
fn set_recording(on: bool) -> Result<(), String> {
    let Some(folder) = crate::capture::folder() else {
        return Err("Not available: LEONIDA_CAPTURE is not set.".into());
    };
    let rec = folder.join("rec");
    let done = if on {
        std::fs::create_dir_all(folder).and_then(|()| std::fs::write(&rec, b""))
    } else {
        match std::fs::remove_file(&rec) {
            Err(e) if e.kind() != std::io::ErrorKind::NotFound => Err(e),
            _ => Ok(()),
        }
    };
    done.map_err(|e| format!("Not done: {} ({e})", rec.display()))
}

/// Tells the menu what this renderer does for it (once): the settings of
/// ours it shows and changes, and who else reads the keyboard.
fn provide() {
    static ONCE: std::sync::Once = std::sync::Once::new();
    ONCE.call_once(|| {
        knobs::FPS_CAP.provide(cap, set_cap);
        // The Lua mods' checked reads and writes of the title's memory.
        leonida_lua::set_memory_host(leonida_lua::MemoryHost {
            read: super::lua_read,
            write: super::lua_write,
        });
        if dlss_asked() {
            knobs::SHARPNESS.provide(sharpness, set_sharpness);
        }
        #[cfg(feature = "diagnostics")]
        {
            use std::sync::atomic::Ordering::Relaxed;
            knobs::PANEL.provide(
                || super::panel::shown().load(Relaxed),
                |on| {
                    super::panel::shown().store(on, Relaxed);
                    Ok(())
                },
            );
            if crate::capture::folder().is_some() {
                knobs::RECORD.provide(recording, set_recording);
            }
            knobs::provide_screenshots();
            // The F1 settings menu takes the keyboard while it is shown.
            knobs::provide_keyboard_elsewhere(|| crate::SETTINGS_MENU_OPEN.load(Relaxed));
        }
    });
}

/// The path of the screenshot the menu asked for, if `work` (what
/// `leonida_menu::work()` said at this present) says one is: the present
/// then reads its frame back as it does for a saved frame.
#[cfg(feature = "diagnostics")]
pub(super) fn shot(work: u32) -> Option<std::path::PathBuf> {
    if work & leonida_menu::SHOT == 0 {
        return None;
    }
    knobs::take_screenshot()
}

/// One pixel of a read-back row as 8-bit RGB, by format; `None` for one
/// not read here.
#[cfg(any(feature = "diagnostics", test))]
fn rgb_of(format: DXGI_FORMAT) -> Option<(usize, fn(&[u8]) -> [u8; 3])> {
    fn half(h: u16) -> f32 {
        let (s, e, m) = ((h >> 15) as u32, ((h >> 10) & 31) as i32, (h & 1023) as f32);
        let v = if e == 0 {
            m / 1024.0 * 2f32.powi(-14)
        } else {
            (1.0 + m / 1024.0) * 2f32.powi(e - 15)
        };
        if s == 1 { -v } else { v }
    }
    Some(match format.0 {
        // R8G8B8A8 (typeless, unorm, sRGB).
        27..=29 => (4, |p| [p[0], p[1], p[2]]),
        // B8G8R8A8.
        87 | 90 | 91 => (4, |p| [p[2], p[1], p[0]]),
        // R10G10B10A2.
        23..=25 => (4, |p| {
            let v = u32::from_le_bytes([p[0], p[1], p[2], p[3]]);
            [0, 10, 20].map(|s| ((v >> s & 1023) >> 2) as u8)
        }),
        // R16G16B16A16 float: clamped to 0..1.
        9..=10 => (8, |p| {
            [0, 2, 4].map(|i| {
                (half(u16::from_le_bytes([p[i], p[i + 1]])).clamp(0.0, 1.0) * 255.0 + 0.5) as u8
            })
        }),
        _ => return None,
    })
}

/// Saves the frame a present read back (`crate::capture::read_back`, once
/// its submission has run) as the screenshot the menu asked for: at full
/// size, 8-bit, 10-bit and half-float pictures. The PNG
/// (`crate::capture::png`) is made on a thread of its own.
#[cfg(feature = "diagnostics")]
pub(super) fn save_shot(
    path: std::path::PathBuf,
    buffer: &ID3D12Resource,
    footprint: &D3D12_PLACED_SUBRESOURCE_FOOTPRINT,
) {
    let f = footprint.Footprint;
    if f.Width == 0 || f.Height == 0 {
        return;
    }
    let Some((size, rgb)) = rgb_of(f.Format) else {
        tracing::warn!(
            format = f.Format.0,
            "screenshot not saved: a format this does not read"
        );
        return;
    };
    let mut data = std::ptr::null_mut();
    // SAFETY: the copy finished (a present that reads back waits for its
    // submission); the mapping spans the footprint's rows.
    let rows = unsafe {
        if buffer.Map(0, None, Some(&mut data)).is_err() {
            return;
        }
        let rows = std::slice::from_raw_parts(
            data.cast::<u8>().add(footprint.Offset as usize),
            f.RowPitch as usize * (f.Height as usize - 1) + f.Width as usize * size,
        )
        .to_vec();
        buffer.Unmap(0, None);
        rows
    };
    let (w, h, pitch) = (f.Width, f.Height, f.RowPitch as usize);
    let _ = std::thread::Builder::new()
        .name("leonida-screenshot".into())
        .spawn(move || {
            let mut out = Vec::with_capacity(w as usize * h as usize * 3);
            for y in 0..h as usize {
                for x in 0..w as usize {
                    out.extend(rgb(&rows[y * pitch + x * size..][..size]));
                }
            }
            let png = crate::capture::png(w, h, &out);
            let written = path
                .parent()
                .map_or(Ok(()), std::fs::create_dir_all)
                .and_then(|()| std::fs::write(&path, png));
            match written {
                Ok(()) => tracing::info!(path = %path.display(), "screenshot saved"),
                Err(e) => {
                    tracing::warn!(error = %e, path = %path.display(), "screenshot not saved")
                }
            }
        });
}

#[cfg(test)]
mod tests {
    use super::*;
    use leonida_menu::model::Input;
    use windows::Win32::Graphics::Direct3D::D3D_FEATURE_LEVEL_11_0;
    use windows::Win32::System::Threading::{CreateEventW, INFINITE, WaitForSingleObject};

    type Card = (
        ID3D12Device,
        ID3D12CommandQueue,
        ID3D12CommandAllocator,
        ID3D12GraphicsCommandList,
    );

    /// A device, a queue and a list (with its allocator) of our own for a
    /// test: none of the host's, so nothing of a title is needed. `None`
    /// without a card.
    fn device() -> Option<Card> {
        let mut device: Option<ID3D12Device> = None;
        // SAFETY: plain device creation on the default adapter, then plain
        // object creation.
        unsafe {
            D3D12CreateDevice(None, D3D_FEATURE_LEVEL_11_0, &mut device).ok()?;
            let device = device?;
            let queue: ID3D12CommandQueue = device
                .CreateCommandQueue(&D3D12_COMMAND_QUEUE_DESC {
                    Type: D3D12_COMMAND_LIST_TYPE_DIRECT,
                    ..Default::default()
                })
                .ok()?;
            let allocator: ID3D12CommandAllocator = device
                .CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT)
                .ok()?;
            let list: ID3D12GraphicsCommandList = device
                .CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, &allocator, None)
                .ok()?;
            Some((device, queue, allocator, list))
        }
    }

    fn read_back_buffer(device: &ID3D12Device, size: u64) -> ID3D12Resource {
        let heap = D3D12_HEAP_PROPERTIES {
            Type: D3D12_HEAP_TYPE_READBACK,
            ..Default::default()
        };
        let desc = D3D12_RESOURCE_DESC {
            Dimension: D3D12_RESOURCE_DIMENSION_BUFFER,
            Width: size,
            Height: 1,
            DepthOrArraySize: 1,
            MipLevels: 1,
            SampleDesc: DXGI_SAMPLE_DESC {
                Count: 1,
                Quality: 0,
            },
            Layout: D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
            ..Default::default()
        };
        let mut buffer: Option<ID3D12Resource> = None;
        // SAFETY: plain creation.
        unsafe {
            device.CreateCommittedResource(
                &heap,
                D3D12_HEAP_FLAG_NONE,
                &desc,
                D3D12_RESOURCE_STATE_COPY_DEST,
                None,
                &mut buffer,
            )
        }
        .unwrap();
        buffer.unwrap()
    }

    /// Draws the menu with the overlay's own pipeline onto an offscreen
    /// texture over a made-up backdrop, twice (the second time below where
    /// the numbers panel would be), reads both back and saves the first as
    /// a PNG (`LEONIDA_MENU_PREVIEW=<path>`; else into the temporary
    /// folder). Skipped without a D3D12 device.
    #[test]
    fn the_overlay_draws_the_menu_offscreen() {
        let Some((device, queue, _allocator, list)) = device() else {
            eprintln!("SKIP: no D3D12 device");
            return;
        };
        let (width, height) = (1920u32, 1080u32);
        let format = DXGI_FORMAT_R8G8B8A8_UNORM;
        // The panel's box at this height, as `super::super::panel` sizes it
        // (87 dots of two pixels): the second picture has the menu below it.
        let under = 174u32;
        // SAFETY: plain object creation, recording and a wait on our own fence.
        unsafe {
            // The backdrop: a sky over grass, uploaded as rows.
            let mut backdrop = Vec::with_capacity((width * height * 4) as usize);
            for y in 0..height {
                for x in 0..width {
                    let (t, u) = (y as f32 / height as f32, x as f32 / width as f32);
                    backdrop.extend(if t < 0.55 {
                        [
                            (90.0 + 80.0 * t) as u8,
                            (150.0 + 60.0 * t) as u8,
                            (220.0 - 30.0 * u) as u8,
                            255,
                        ]
                    } else {
                        [(70.0 + 40.0 * u) as u8, (90.0 + 20.0 * t) as u8, 60, 255]
                    });
                }
            }
            let heap = D3D12_HEAP_PROPERTIES {
                Type: D3D12_HEAP_TYPE_DEFAULT,
                ..Default::default()
            };
            let desc = D3D12_RESOURCE_DESC {
                Dimension: D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                Width: u64::from(width),
                Height: height,
                DepthOrArraySize: 1,
                MipLevels: 1,
                Format: format,
                SampleDesc: DXGI_SAMPLE_DESC {
                    Count: 1,
                    Quality: 0,
                },
                Flags: D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                ..Default::default()
            };
            let pitch = width * 4;
            let (up, at) = upload(&device, u64::from(pitch * height)).unwrap();
            std::ptr::copy_nonoverlapping(backdrop.as_ptr(), at, backdrop.len());
            let footprint = D3D12_PLACED_SUBRESOURCE_FOOTPRINT {
                Offset: 0,
                Footprint: D3D12_SUBRESOURCE_FOOTPRINT {
                    Format: format,
                    Width: width,
                    Height: height,
                    Depth: 1,
                    RowPitch: pitch,
                },
            };
            let views: ID3D12DescriptorHeap = device
                .CreateDescriptorHeap(&D3D12_DESCRIPTOR_HEAP_DESC {
                    Type: D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
                    NumDescriptors: 1,
                    Flags: D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
                    NodeMask: 0,
                })
                .unwrap();
            let view = views.GetCPUDescriptorHandleForHeapStart();
            // The menu as the menu's thread paints it: the main page, the
            // second row selected.
            let layers = leonida_menu::preview(&[Input::Down], leonida_menu::scale_for(height));
            let frame = leonida_menu::Frame {
                version: 1,
                layers: std::sync::Arc::new(layers),
                origin: None,
            };
            let kept = std::cell::RefCell::new(Vec::new());
            let mut overlay = Overlay::idle();
            let mut pictures = Vec::new();
            for under in [0, under] {
                let mut target: Option<ID3D12Resource> = None;
                device
                    .CreateCommittedResource(
                        &heap,
                        D3D12_HEAP_FLAG_NONE,
                        &desc,
                        D3D12_RESOURCE_STATE_COPY_DEST,
                        None,
                        &mut target,
                    )
                    .unwrap();
                let target = target.unwrap();
                let whole = |resource: &ID3D12Resource| D3D12_TEXTURE_COPY_LOCATION {
                    pResource: std::mem::transmute_copy(resource),
                    Type: D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                    Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 {
                        SubresourceIndex: 0,
                    },
                };
                let rows = |buffer: &ID3D12Resource| D3D12_TEXTURE_COPY_LOCATION {
                    pResource: std::mem::transmute_copy(buffer),
                    Type: D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                    Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 {
                        PlacedFootprint: footprint,
                    },
                };
                list.CopyTextureRegion(&whole(&target), 0, 0, 0, &rows(&up), None);
                // (Views are read when recorded: one slot serves both.)
                device.CreateRenderTargetView(&target, None, view);
                overlay
                    .record(
                        &device,
                        &list,
                        &|object| kept.borrow_mut().push(object.clone()),
                        (&target, view),
                        (width, height),
                        format,
                        under,
                        &frame,
                    )
                    .unwrap();
                list.ResourceBarrier(&[transition(
                    &target,
                    D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_COPY_SOURCE,
                )]);
                let read = read_back_buffer(&device, u64::from(pitch * height));
                list.CopyTextureRegion(&rows(&read), 0, 0, 0, &whole(&target), None);
                pictures.push((target, read));
            }
            // The picture was uploaded once and both draws kept what they use.
            assert_eq!(overlay.shown, 1);
            assert_eq!(kept.borrow().len(), 4 + 1 + 4);
            list.Close().unwrap();
            queue.ExecuteCommandLists(&[Some(list.cast().unwrap())]);
            let fence: ID3D12Fence = device.CreateFence(0, D3D12_FENCE_FLAG_NONE).unwrap();
            queue.Signal(&fence, 1).unwrap();
            let event = CreateEventW(None, false, false, None).unwrap();
            fence.SetEventOnCompletion(1, event).unwrap();
            WaitForSingleObject(event, INFINITE);
            assert!(
                device.GetDeviceRemovedReason().is_ok(),
                "the device survived"
            );
            let l = &frame.layers;
            let (left, top) = (LEFT as u32, TOP as u32);
            let mut first = Vec::new();
            for (i, (_, read)) in pictures.iter().enumerate() {
                let mut data = std::ptr::null_mut();
                read.Map(0, None, Some(&mut data)).unwrap();
                let rows = std::slice::from_raw_parts(data.cast::<u8>(), (pitch * height) as usize)
                    .to_vec();
                read.Unmap(0, None);
                let pixel = |x: u32, y: u32| &rows[(y * pitch + x * 4) as usize..][..4];
                let behind = |x: u32, y: u32| &backdrop[((y * width + x) * 4) as usize..][..4];
                // The second picture's menu starts below the panel's box
                // and a gap; above it the backdrop is untouched.
                let top = if i == 0 {
                    top
                } else {
                    under + UNDER_PANEL as u32
                };
                if i == 1 {
                    assert_eq!(pixel(left + 300, top - 1), behind(left + 300, top - 1));
                }
                // The banner is blue, the selected row light, an unselected
                // one dark, and outside the menu the backdrop is untouched.
                let banner = pixel(left + 400, top + 100);
                assert!(banner[2] > banner[0] + 40, "{i}: {banner:?}");
                let selected = pixel(left + 300, top + l.band_top() + 4);
                assert!(selected[0] > 200 && selected[2] > 200, "{i}: {selected:?}");
                let other = pixel(left + 300, top + l.rows_top + 4);
                assert!(other[0] < 80, "{i}: {other:?}");
                assert_eq!(pixel(1500, 900), behind(1500, 900));
                if i == 0 {
                    first = rows;
                }
            }
            let rgb: Vec<u8> = first
                .chunks_exact(4)
                .flat_map(|p| [p[0], p[1], p[2]])
                .collect();
            let png = leonida_menu::png::encode(width, height, 3, width as usize * 3, &rgb);
            let path = std::env::var_os("LEONIDA_MENU_PREVIEW")
                .map(std::path::PathBuf::from)
                .unwrap_or_else(|| std::env::temp_dir().join("leonida-menu-preview.png"));
            std::fs::write(&path, png).unwrap();
            eprintln!("menu preview: {}", path.display());
        }
    }

    #[test]
    fn read_back_formats_turn_into_rgb() {
        let (n, f) = rgb_of(DXGI_FORMAT_B8G8R8A8_UNORM).unwrap();
        assert_eq!((n, f(&[1, 2, 3, 4])), (4, [3, 2, 1]));
        let (_, f) = rgb_of(DXGI_FORMAT_R10G10B10A2_UNORM).unwrap();
        assert_eq!(f(&(1023u32 | 512 << 10).to_le_bytes()), [255, 128, 0]);
        let (n, f) = rgb_of(DXGI_FORMAT_R16G16B16A16_FLOAT).unwrap();
        // 1.0, 0.5, 2.0 (clamped).
        assert_eq!(
            (n, f(&[0, 0x3C, 0, 0x38, 0, 0x40, 0, 0x3C])),
            (8, [255, 128, 255])
        );
        assert!(rgb_of(DXGI_FORMAT_R32_FLOAT).is_none());
    }

    #[test]
    fn the_cap_is_read_and_written_as_the_frame_clock_reads_it() {
        assert_eq!(cap_of(None), Cap::Title);
        assert_eq!(cap_of(Some("")), Cap::Title);
        assert_eq!(cap_of(Some("abc")), Cap::Title);
        assert_eq!(cap_of(Some("0")), Cap::Unlimited);
        assert_eq!(cap_of(Some(" 60 ")), Cap::Fps(60));
        for cap in [Cap::Title, Cap::Unlimited, Cap::Fps(40), Cap::Fps(144)] {
            assert_eq!(cap_of(Some(&cap_text(cap))), cap);
        }
    }

    #[test]
    fn sharpness_is_written_as_the_f1_menu_writes_it() {
        assert_eq!(sharpness_text(0.0), "0");
        assert_eq!(sharpness_text(0.25), "0.25");
        assert_eq!(sharpness_text(0.5), "0.5");
        assert_eq!(sharpness_text(0.05), "0.05");
        assert_eq!(sharpness_text(1.0), "1");
        assert_eq!(sharpness_text(7.0), "1");
        assert_eq!(sharpness_of(None), 0.0);
        assert_eq!(sharpness_of(Some("0.35")), 0.35);
        assert_eq!(sharpness_of(Some("nan")), 0.0);
        assert_eq!(sharpness_of(Some("3")), 1.0);
    }
}
