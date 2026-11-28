//! Two presented planes as one image: the console's display puts the second
//! (the interface) over the first (the scene), each scaled to the screen.
//! Grand Theft Auto VI presents so on Series S.

use windows::Win32::Graphics::Direct3D::Fxc::D3DCompile;
use windows::Win32::Graphics::Direct3D::ID3DBlob;
use windows::Win32::Graphics::Direct3D12::*;
use windows::Win32::Graphics::Dxgi::Common::*;
use windows::core::{PCSTR, s};

use crate::host::Host;

const SOURCE: &str = r#"
#define RS "DescriptorTable(SRV(t0, numDescriptors=2), visibility=SHADER_VISIBILITY_PIXEL), StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_LINEAR, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP)"
Texture2D<float4> scene : register(t0);
Texture2D<float4> over : register(t1);
SamplerState smooth_ : register(s0);
struct V { float4 at : SV_Position; float2 uv : TEXCOORD0; };
[RootSignature(RS)]
V vs(uint id : SV_VertexID) {
    V v;
    v.uv = float2((id << 1) & 2, id & 2);
    v.at = float4(v.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return v;
}
// Leonida: the scene sharpened before the interface goes over it (SHARPEN, 0 to 1, set when the pass is made from
// LEONIDA_SHARPEN). Contrast-adaptive, after the idea of AMD FidelityFX CAS (MIT): each pixel is pushed away from
// its four neighbours by an amount that shrinks where the neighbourhood already spans much of the range, so soft
// areas gain detail and hard edges get no halo; the result stays inside the neighbourhood's range.
float3 at(float2 uv) { return scene.SampleLevel(smooth_, uv, 0).rgb; }
float3 sharpened(float2 uv) {
    float3 g = at(uv);
    if (SHARPEN <= 0.0) return g;
    float w, h;
    scene.GetDimensions(w, h);
    float2 t = 1.0 / float2(w, h);
    float3 l = at(uv - float2(t.x, 0)), r = at(uv + float2(t.x, 0));
    float3 u = at(uv - float2(0, t.y)), d = at(uv + float2(0, t.y));
    float3 lo = min(g, min(min(l, r), min(u, d)));
    float3 hi = max(g, max(max(l, r), max(u, d)));
    float3 amp = sqrt(saturate(min(lo, 1.0 - hi) / max(hi, 1e-4)));
    float3 k = -amp * lerp(0.125, 0.2, saturate(SHARPEN));
    float3 c = (g + (l + r + u + d) * k) / (1.0 + 4.0 * k);
    return clamp(c, lo, hi);
}
// A clamp to [0,1] that is safe for NaN and infinity (CLEAN, set when the pass is made from LEONIDA_COMPOSE_NAN):
// saturate() makes a NaN 0, which showed as black specks and a black sun where the upscaler or frame generation
// handed one over (sun glints on water). Here a NaN becomes 1, +infinity 1, a negative value and -infinity 0. The
// test is on the bits: the compiler may fold x != x or isnan() away.
// From aki-gta6/aki-runtime 33c21b9 (Aki-Developer), GPL-3.0.
float clean1(float x) {
#if CLEAN
    return (asuint(x) & 0x7FFFFFFFu) > 0x7F800000u ? 1.0 : saturate(x);
#else
    return saturate(x);
#endif
}
float3 clean(float3 x) {
    return float3(clean1(x.r), clean1(x.g), clean1(x.b));
}
[RootSignature(RS)]
float4 ps(V v) : SV_Target {
    float3 s = sharpened(v.uv);
    float4 o = over.SampleLevel(smooth_, v.uv, 0);
    return float4(clean(o.rgb + s * (1 - clean1(o.a))), 1);
}
"#;

/// Whether the composer's clamp keeps a NaN from coming out black (`clean1` in [`SOURCE`]). On by default;
/// `LEONIDA_COMPOSE_NAN=0`: plain `saturate`, as before.
fn clean() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| std::env::var_os("LEONIDA_COMPOSE_NAN").is_none_or(|v| v != "0"))
}

/// [`SOURCE`] with its two settings in front.
fn source(sharpen: f32, clean: bool) -> String {
    format!("#define SHARPEN {sharpen:.3}\n#define CLEAN {}\n{SOURCE}", u8::from(clean))
}

/// Leonida: how much the scene is sharpened before the interface goes over it (`LEONIDA_SHARPEN`, 0 to 1;
/// default 0.6; 0: off). The title renders 1280x720 on Series S and DLSS takes it to 1920x1080; the DLSS
/// release the launcher fetches ignores its own sharpness setting, so without this nothing sharpens the picture.
fn sharpen() -> f32 {
    static SET: std::sync::OnceLock<f32> = std::sync::OnceLock::new();
    *SET.get_or_init(|| {
        std::env::var("LEONIDA_SHARPEN")
            .ok()
            .and_then(|v| v.trim().parse::<f32>().ok())
            .filter(|v| v.is_finite())
            .map_or(0.6, |v| v.clamp(0.0, 1.0))
    })
}

pub(crate) struct Composer {
    root: ID3D12RootSignature,
    pso: ID3D12PipelineState,
    views: Vec<ID3D12DescriptorHeap>,
    step: u32,
}
// SAFETY: D3D12 objects are free-threaded.
unsafe impl Send for Composer {}
// SAFETY: as above.
unsafe impl Sync for Composer {}

fn compile(entry: PCSTR, target: PCSTR) -> windows::core::Result<ID3DBlob> {
    compile_source(&source(sharpen(), clean()), entry, target)
}

fn compile_source(source: &str, entry: PCSTR, target: PCSTR) -> windows::core::Result<ID3DBlob> {
    let mut code: Option<ID3DBlob> = None;
    // SAFETY: the source and names live through the call; the out pointer is
    // ours.
    unsafe {
        D3DCompile(
            source.as_ptr().cast(),
            source.len(),
            s!("compose"),
            None,
            None,
            entry,
            target,
            0,
            0,
            &mut code,
            None,
        )?;
    }
    code.ok_or_else(windows::core::Error::empty)
}

impl Composer {
    pub(crate) fn new(host: &Host, format: DXGI_FORMAT) -> windows::core::Result<Self> {
        let (vs, ps) = (
            compile(s!("vs"), s!("vs_5_1"))?,
            compile(s!("ps"), s!("ps_5_1"))?,
        );
        // SAFETY: the blobs' own bytes, alive through the calls.
        unsafe {
            let bytes = |b: &ID3DBlob| {
                std::slice::from_raw_parts(b.GetBufferPointer().cast::<u8>(), b.GetBufferSize())
            };
            let device = &host.device;
            let root: ID3D12RootSignature = device.CreateRootSignature(0, bytes(&vs))?;
            let code = |b: &ID3DBlob| D3D12_SHADER_BYTECODE {
                pShaderBytecode: b.GetBufferPointer(),
                BytecodeLength: b.GetBufferSize(),
            };
            let mut desc = D3D12_GRAPHICS_PIPELINE_STATE_DESC {
                pRootSignature: std::mem::transmute_copy(&root),
                VS: code(&vs),
                PS: code(&ps),
                SampleMask: u32::MAX,
                RasterizerState: D3D12_RASTERIZER_DESC {
                    FillMode: D3D12_FILL_MODE_SOLID,
                    CullMode: D3D12_CULL_MODE_NONE,
                    ..Default::default()
                },
                PrimitiveTopologyType: D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
                NumRenderTargets: 1,
                SampleDesc: DXGI_SAMPLE_DESC {
                    Count: 1,
                    Quality: 0,
                },
                ..Default::default()
            };
            desc.RTVFormats[0] = format;
            desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
                D3D12_COLOR_WRITE_ENABLE_ALL.0 as u8;
            let pso = device.CreateGraphicsPipelineState(&desc)?;
            Ok(Composer {
                root,
                pso,
                views: Vec::new(),
                step: device
                    .GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV),
            })
        }
    }

    /// Draws the two planes over `width` x `height` of `back` from (`left`, `top`).
    /// Each presenter command-list slot owns its own pair of SRVs. The swap
    /// chain may have several outstanding presents; the slot is selected by
    /// the completion fence, and its descriptors are only overwritten on reuse.
    pub(crate) fn record(
        &mut self,
        host: &Host,
        slot: usize,
        list: &ID3D12GraphicsCommandList,
        planes: [(&ID3D12Resource, DXGI_FORMAT); 2],
        back: &ID3D12Resource,
        (left, top): (u32, u32),
        (width, height): (u32, u32),
    ) -> windows::core::Result<()> {
        while self.views.len() <= slot {
            let heap = unsafe {
                host.device
                    .CreateDescriptorHeap(&D3D12_DESCRIPTOR_HEAP_DESC {
                        Type: D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                        NumDescriptors: 2,
                        Flags: D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
                        NodeMask: 0,
                    })?
            };
            self.views.push(heap);
        }
        let views = &self.views[slot];
        // SAFETY: an open list; the views are made in slots no frame in flight
        // reads; live textures.
        unsafe {
            let (cpu, gpu) = (
                views.GetCPUDescriptorHandleForHeapStart(),
                views.GetGPUDescriptorHandleForHeapStart(),
            );
            for (i, plane) in planes.iter().enumerate() {
                let at = D3D12_CPU_DESCRIPTOR_HANDLE {
                    ptr: cpu.ptr + (i * self.step as usize),
                };
                let desc = D3D12_SHADER_RESOURCE_VIEW_DESC {
                    Format: plane.1,
                    ViewDimension: D3D12_SRV_DIMENSION_TEXTURE2D,
                    Shader4ComponentMapping: D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
                    Anonymous: D3D12_SHADER_RESOURCE_VIEW_DESC_0 {
                        Texture2D: D3D12_TEX2D_SRV {
                            MipLevels: 1,
                            ..Default::default()
                        },
                    },
                };
                host.device
                    .CreateShaderResourceView(Some(plane.0), Some(&desc), at);
            }
            let rtv = host.rtv(Some(back), None);
            list.OMSetRenderTargets(1, Some(&rtv), false, None);
            list.SetDescriptorHeaps(&[Some(views.clone())]);
            list.SetGraphicsRootSignature(&self.root);
            list.SetPipelineState(&self.pso);
            list.SetGraphicsRootDescriptorTable(0, gpu);
            list.IASetPrimitiveTopology(
                windows::Win32::Graphics::Direct3D::D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST,
            );
            // Leonida: the picture's place in `back`, which is larger when bars go around it (`crate::aspect`).
            list.RSSetViewports(&[D3D12_VIEWPORT {
                TopLeftX: left as f32,
                TopLeftY: top as f32,
                Width: width as f32,
                Height: height as f32,
                MaxDepth: 1.0,
                ..Default::default()
            }]);
            list.RSSetScissorRects(&[windows::Win32::Foundation::RECT {
                left: left as i32,
                top: top as i32,
                right: (left + width) as i32,
                bottom: (top + height) as i32,
            }]);
            list.DrawInstanced(3, 1, 0, 0);
        }
        Ok(())
    }
}

/// Select a legal float-sampled view of a display plane, preserving a typed
/// resource's format. Typeless resources use the corresponding display type.
pub(crate) fn view(d: &D3D12_RESOURCE_DESC) -> Option<DXGI_FORMAT> {
    if d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D
        || d.DepthOrArraySize != 1
        || d.SampleDesc.Count != 1
        || d.Flags.contains(D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)
    {
        return None;
    }
    match d.Format.0 {
        9 => Some(DXGI_FORMAT_R16G16B16A16_FLOAT),
        23 => Some(DXGI_FORMAT_R10G10B10A2_UNORM),
        27 => Some(DXGI_FORMAT_R8G8B8A8_UNORM),
        90 => Some(DXGI_FORMAT_B8G8R8A8_UNORM),
        10 | 11 | 13 | 24 | 26 | 28 | 29 | 31 | 67 | 87 | 91 => Some(d.Format),
        _ => None,
    }
}

/// No card needed: the compiler alone.
#[cfg(test)]
mod source_tests {
    use super::*;

    /// The pixel program compiles with the NaN-safe clamp and without it, sharpened or not, and the two clamps
    /// are different code (the bit test is not folded away).
    #[test]
    fn the_composer_compiles_with_and_without_the_nan_clamp() {
        let bytes = |source: &str| {
            let blob = compile_source(source, s!("ps"), s!("ps_5_1")).expect("the composer's pixel program compiles");
            // SAFETY: the blob's own bytes, copied while it lives.
            unsafe { std::slice::from_raw_parts(blob.GetBufferPointer().cast::<u8>(), blob.GetBufferSize()).to_vec() }
        };
        for sharpen in [0.0, 0.6] {
            let (with, without) = (bytes(&source(sharpen, true)), bytes(&source(sharpen, false)));
            assert_ne!(with, without, "the clamp changed nothing at sharpen {sharpen}");
        }
        compile_source(&source(0.6, true), s!("vs"), s!("vs_5_1")).expect("the vertex program compiles");
        assert!(source(0.25, true).starts_with("#define SHARPEN 0.250\n#define CLEAN 1\n"));
        assert!(source(0.0, false).contains("#define CLEAN 0\n"));
    }
}

#[cfg(all(test, feature = "diagnostics"))]
mod tests {
    use super::*;

    #[test]
    fn composes_scaled_premultiplied_plane_on_gpu() {
        let Some(host) = crate::host::host() else {
            eprintln!("skipping display-plane readback: no D3D12 device");
            return;
        };
        let format = DXGI_FORMAT_R8G8B8A8_UNORM;
        let mut compositor = Composer::new(host, format).unwrap();
        let list = host.new_list().unwrap();
        let texture = |width: u64| -> ID3D12Resource {
            let mut resource = None;
            let desc = D3D12_RESOURCE_DESC {
                Dimension: D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                Width: width,
                Height: 1,
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
            unsafe {
                host.device
                    .CreateCommittedResource(
                        &D3D12_HEAP_PROPERTIES {
                            Type: D3D12_HEAP_TYPE_DEFAULT,
                            ..Default::default()
                        },
                        D3D12_HEAP_FLAG_NONE,
                        &desc,
                        D3D12_RESOURCE_STATE_RENDER_TARGET,
                        None,
                        &mut resource,
                    )
                    .unwrap()
            };
            resource.unwrap()
        };
        // A half-red base at one resolution. The overlay at output resolution
        // contains transparent, half-opaque green and fully opaque blue pixels.
        let base = texture(1);
        let overlay = texture(3);
        let output = texture(3);
        let target = D3D12_RESOURCE_STATE_RENDER_TARGET;
        let read = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        let cmds = &list.list;
        unsafe {
            cmds.ClearRenderTargetView(host.rtv(Some(&base), None), &[0.5, 0.0, 0.0, 1.0], None);
            let overlay_rtv = host.rtv(Some(&overlay), None);
            let colors = [
                [0.0, 0.0, 0.0, 0.0],
                [0.0, 0.5, 0.0, 0.5],
                [0.0, 0.0, 1.0, 1.0],
            ];
            for (i, color) in colors.iter().enumerate() {
                let rect = windows::Win32::Foundation::RECT {
                    left: i as i32,
                    right: i as i32 + 1,
                    top: 0,
                    bottom: 1,
                };
                cmds.ClearRenderTargetView(overlay_rtv, color, Some(&[rect]));
            }
            cmds.ResourceBarrier(&[
                crate::host::transition(&base, target, read),
                crate::host::transition(&overlay, target, read),
            ]);
            compositor
                .record(
                    host,
                    0,
                    cmds,
                    [(&base, format), (&overlay, format)],
                    &output,
                    (0, 0),
                    (3, 1),
                )
                .unwrap();
            cmds.ResourceBarrier(&[crate::host::transition(
                &output,
                target,
                D3D12_RESOURCE_STATE_COPY_SOURCE,
            )]);
            let (buffer, footprint) =
                crate::capture::read_back(&host.device, cmds, &output).unwrap();
            list.close();
            host.execute(&[&list]);
            host.device.GetDeviceRemovedReason().unwrap();
            let mut mapped = std::ptr::null_mut();
            buffer.Map(0, None, Some(&mut mapped)).unwrap();
            let start = mapped.cast::<u8>().add(footprint.Offset as usize);
            let pixels: [u8; 12] = std::slice::from_raw_parts(start, 12).try_into().unwrap();
            buffer.Unmap(0, None);
            for (actual, expected) in pixels
                .iter()
                .zip([128, 0, 0, 255, 64, 128, 0, 255, 0, 0, 255, 255])
            {
                assert!(
                    actual.abs_diff(expected) <= 1,
                    "unexpected composite {pixels:?}"
                );
            }
            eprintln!("D3D12 display-plane readback RGBA: {pixels:?}");
        }
    }
}
