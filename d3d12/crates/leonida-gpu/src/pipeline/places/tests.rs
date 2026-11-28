//! The list without a card, then what the driver does and what is done
//! about it on the card: each of those tests runs processes of its own
//! (this test program again, [`a_run`]) that share a cache folder, as runs
//! of the title do.
//!
//!     cargo test -p leonida-gpu --features diagnostics --lib pipeline::places -- --ignored --nocapture --test-threads=1
//!
//! (One after another: they print what was measured, and the card is the
//! title's too.) On a card that is not NVIDIA's they print what they read
//! and hold nothing against it.

use std::path::Path;
use std::sync::atomic::Ordering;

use windows::Win32::Foundation::RECT;
use windows::Win32::Graphics::Direct3D::D3D_FEATURE_LEVEL_12_0;

use super::super::host::{Sub, root_signature, stream_pipeline};
use super::super::library;
use super::*;

/// The sampler `text` names: its bias in eighths of a level, then `p` for
/// point filters (levels stay blended), `w` for wrapped addresses (else
/// clamped) and `v` for another register and space and the pixel stage alone. For
/// the list alone (the test pipelines read the bias back and do not
/// compare): `x` the bias below zero, `a` anisotropic, `c` comparing, `n`
/// the least of its texels, `b` a white border, `m` mirrored, `l` levels
/// 1.5 to 3.25 only.
fn sampler(text: &str, register: u32, pixel: bool) -> D3D12_STATIC_SAMPLER_DESC {
    let digits = text.trim_end_matches(|c: char| !c.is_ascii_digit());
    let has = |c| text[digits.len()..].contains(c);
    let address = match () {
        () if has('w') => D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        () if has('b') => D3D12_TEXTURE_ADDRESS_MODE_BORDER,
        () if has('m') => D3D12_TEXTURE_ADDRESS_MODE_MIRROR,
        () => D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
    };
    let bias = digits.parse::<u32>().expect("eighths of a level") as f32 / 8.0;
    D3D12_STATIC_SAMPLER_DESC {
        Filter: match () {
            () if has('a') => D3D12_FILTER_ANISOTROPIC,
            () if has('c') && has('p') => D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT,
            () if has('c') => D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT,
            () if has('n') => D3D12_FILTER_MINIMUM_MIN_MAG_MIP_LINEAR,
            () if has('p') => D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR,
            () => D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        },
        AddressU: address,
        AddressV: address,
        AddressW: address,
        MipLODBias: if has('x') { -bias } else { bias },
        MaxAnisotropy: if has('a') { 16 } else { 1 },
        // As the title's immediate samplers have it when they do not compare.
        ComparisonFunc: if has('c') && has('x') {
            D3D12_COMPARISON_FUNC_GREATER
        } else {
            D3D12_COMPARISON_FUNC_LESS_EQUAL
        },
        BorderColor: if has('b') {
            D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE
        } else {
            D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK
        },
        MinLOD: if has('l') { 1.5 } else { 0.0 },
        MaxLOD: if has('l') { 3.25 } else { 1000.0 },
        ShaderRegister: if has('v') { 3 } else { register },
        RegisterSpace: if has('v') { 7 } else { 0 },
        ShaderVisibility: if pixel || has('v') {
            D3D12_SHADER_VISIBILITY_PIXEL
        } else {
            D3D12_SHADER_VISIBILITY_ALL
        },
    }
}

/// `1,2p,7-9`: the samplers named, `a-b` for each from a to b.
fn samplers(text: &str) -> Vec<String> {
    let mut names = Vec::new();
    for part in text.split(',').filter(|p| !p.is_empty()) {
        match part.split_once('-') {
            Some((a, b)) => {
                let (a, b): (u32, u32) = (a.parse().unwrap(), b.parse().unwrap());
                assert!(a <= b, "{part}");
                names.extend((a..=b).map(|k| k.to_string()));
            }
            None => names.push(part.to_owned()),
        }
    }
    names
}

fn listed(text: &str) -> Vec<Sampler> {
    samplers(text)
        .iter()
        .map(|s| Sampler::of(&sampler(s, 0, false)))
        .collect()
}

#[test]
fn a_line_is_a_sampler_only_when_whole() {
    let s = Sampler::of(&sampler("5p", 2, true));
    let line = s.line();
    assert_eq!(line.len(), 10 * 8 + 9);
    assert_eq!(Sampler::parse(&line), Some(s));
    // Register, space and visibility are not the sampler's.
    assert_eq!(s, Sampler::of(&sampler("5p", 7, false)));
    assert_ne!(s, Sampler::of(&sampler("5", 2, true)));
    assert_ne!(s, Sampler::of(&sampler("5pw", 2, true)));
    // A run that ended while writing left a part of a line.
    for cut in 0..line.len() {
        assert_eq!(
            Sampler::parse(&line[..cut]),
            None,
            "{cut} characters of a line"
        );
    }
    assert_eq!(Sampler::parse(&format!("{line} 00000000")), None);
    assert_eq!(Sampler::parse(&line.replace('0', "g")), None);
    // The description made from it is the one it was made from.
    assert_eq!(Sampler::of(&s.desc()), s);
    assert!(!s.compares());
    let mut compares = sampler("5p", 0, false);
    compares.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    assert!(Sampler::of(&compares).compares());
    compares.Filter = D3D12_FILTER_MINIMUM_MIN_MAG_MIP_LINEAR;
    assert!(!Sampler::of(&compares).compares());
}

fn file(id: &str, samplers: &[Sampler]) -> String {
    let mut text = format!("{HEAD}{id}\n");
    for s in samplers {
        text.push_str(&s.line());
        text.push('\n');
    }
    text
}

#[test]
fn the_list_names_places_by_what_comes_before() {
    let all = listed("1,2p,3w,4,5p");
    let list = List::parse(&file("00aa", &all)).expect("a list");
    assert_eq!(list.samplers, all);
    let key = |list: &List, of: &str| list.key(listed(of).into_iter());
    // The last of a pipeline's samplers names it, in whatever order it has them.
    assert_eq!(key(&list, "3w,1"), key(&list, "1,3w"));
    assert_eq!(key(&list, "3w,1"), key(&list, "3w"));
    assert_ne!(key(&list, "3w"), key(&list, "4"));
    assert_ne!(key(&list, "1"), key(&list, "2p"));
    // No sampler at all: what stands before the first, in an empty list too.
    assert!(List::new("00aa".into()).key(std::iter::empty()).is_ok());
    assert_eq!(
        key(&list, ""),
        List::new("00aa".into()).key(std::iter::empty())
    );
    assert_ne!(key(&list, ""), key(&list, "1"));
    // A sampler that is not in the list: no name, and which one.
    assert_eq!(key(&list, "1,9,5p,8w"), Err(listed("9,8w")));
    // A list that grew names the earlier ones as before.
    let grown = List::parse(&file("00aa", &listed("1,2p,3w,4,5p,9,8w"))).unwrap();
    for of in ["1", "2p,1", "5p,3w"] {
        assert_eq!(key(&list, of), key(&grown, of));
    }
    assert!(key(&grown, "9,1").is_ok());
    // Another order before a sampler, or another list: another name.
    let other = List::parse(&file("00aa", &listed("2p,1,3w,4,5p"))).unwrap();
    assert_ne!(key(&list, "1"), key(&other, "1"));
    assert_ne!(key(&list, "3w"), key(&other, "3w"));
    let anew = List::parse(&file("00ab", &all)).unwrap();
    assert_ne!(key(&list, "1"), key(&anew, "1"));
}

#[test]
fn a_list_is_read_as_far_as_it_is_one() {
    let all = listed("1,2p,3w");
    // Not ours, or of another form: none.
    assert!(List::parse("").is_none());
    assert!(List::parse(&all[0].line()).is_none());
    assert!(List::parse("leonida static samplers 2 00aa\n").is_none());
    assert!(List::parse(&format!("{HEAD}\n")).is_none());
    assert!(List::parse(&format!("{HEAD}zz\n")).is_none());
    // Lines twice, cut short or of something else are passed over; the
    // others keep their order.
    let mut text = file("00aa", &all[..2]);
    text.push_str(&all[0].line());
    text.push_str("\nsomething else\n");
    text.push_str(&all[1].line()[..40]);
    text.push('\n');
    text.push_str(&all[2].line());
    let list = List::parse(&text).unwrap();
    assert_eq!(list.samplers, all);
    assert_eq!(
        list.key(all.iter().copied()),
        List::parse(&file("00aa", &all))
            .unwrap()
            .key(all.iter().copied())
    );
    // No more than the table has room for.
    let many: Vec<Sampler> = (0..MOST as u32 + 50)
        .map(|k| {
            let mut d = sampler("0", 0, false);
            d.MinLOD = k as f32 / 256.0;
            Sampler::of(&d)
        })
        .collect();
    assert_eq!(
        List::parse(&file("00aa", &many)).unwrap().samplers.len(),
        MOST
    );
}

#[test]
fn what_uses_the_library_is_named_by_its_use() {
    assert_eq!(Use::Plain.name("abc").as_deref(), Some("abc"));
    assert_eq!(Use::Skip.name("abc"), None);
    let placed = Use::Placed("k1".into()).name("abc").expect("a name");
    assert_ne!(placed, "abc");
    assert_eq!(Some(&placed), Use::Placed("k1".into()).name("abc").as_ref());
    assert_ne!(Some(&placed), Use::Placed("k2".into()).name("abc").as_ref());
    assert_ne!(Some(&placed), Use::Placed("k1".into()).name("abd").as_ref());
}

/// What a test pipeline writes through each of its `samplers`: the level
/// read where level 0 is asked for (the sampler's bias), a point between
/// texels of the noisy volume (its filter), a point beyond its side (its
/// address mode), and the code's `number`.
fn source(samplers: usize, number: u32, pixel: bool) -> String {
    let mut text = format!(
        "cbuffer At : register(b0) {{ uint base; float lod; uint spare0; uint spare1; }};
        Texture2D<float> levels : register(t0);
        Texture3D<float4> pattern : register(t2);
        float4 through(SamplerState s) {{
            return float4(levels.SampleLevel(s, float2(0.5, 0.5), 0.0),
                pattern.SampleLevel(s, float3(0.37, 0.61, 0.45), -9.0).x,
                pattern.SampleLevel(s, float3(-0.3, 0.4, 0.4), -9.0).x, {number}.0);
        }}\n"
    );
    for n in 0..samplers {
        text.push_str(&format!("SamplerState s{n} : register(s{n});\n"));
    }
    if pixel {
        text.push_str(
            "float4 main(float4 at : SV_Position) : SV_Target {\n uint n = (uint)at.x;\n",
        );
        for n in 0..samplers {
            text.push_str(&format!("if (n == {n}) return through(s{n});\n"));
        }
        text.push_str("return float4(0.0, 0.0, 0.0, 0.0);\n}\n");
    } else {
        text.push_str("RWStructuredBuffer<float4> wrote : register(u0);\n[numthreads(1, 1, 1)] void main() {\n");
        for n in 0..samplers {
            text.push_str(&format!("wrote[base + {n}] = through(s{n});\n"));
        }
        text.push_str("}\n");
    }
    text
}

const VERTEX: &str = "float4 main(uint n : SV_VertexID) : SV_Position {
    float2 p = float2((n << 1) & 2, n & 2);
    return float4(p * 2.0 - 1.0, 0.5, 1.0);
}";

fn code(bytes: &[u8]) -> D3D12_SHADER_BYTECODE {
    D3D12_SHADER_BYTECODE {
        pShaderBytecode: bytes.as_ptr().cast(),
        BytecodeLength: bytes.len(),
    }
}

/// A test pipeline: `kind` is `c` (compute, by its description), `s`
/// (compute, by a stream) or `g` (graphics); `name` its library name, or
/// none to have the driver make it. With the milliseconds the library or
/// the driver took.
fn pipeline(
    device: &ID3D12Device,
    kind: &str,
    root: &ID3D12RootSignature,
    samplers: usize,
    number: u32,
    name: Option<&str>,
) -> (ID3D12PipelineState, f64) {
    let compile =
        |source: &str, target| crate::host::compile(source, s!("main"), target).expect("compiled");
    let (vs, ps, cs) = if kind == "g" {
        (
            compile(VERTEX, s!("vs_5_1")),
            compile(&source(samplers, number, true), s!("ps_5_1")),
            Vec::new(),
        )
    } else {
        (
            Vec::new(),
            Vec::new(),
            compile(&source(samplers, number, false), s!("cs_5_1")),
        )
    };
    let began = std::time::Instant::now();
    // SAFETY: the descriptions point at live data.
    let made = unsafe {
        match kind {
            "g" => {
                let mut desc = D3D12_GRAPHICS_PIPELINE_STATE_DESC {
                    pRootSignature: std::mem::transmute_copy(root),
                    VS: code(&vs),
                    PS: code(&ps),
                    SampleMask: u32::MAX,
                    RasterizerState: D3D12_RASTERIZER_DESC {
                        FillMode: D3D12_FILL_MODE_SOLID,
                        CullMode: D3D12_CULL_MODE_NONE,
                        DepthClipEnable: true.into(),
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
                desc.BlendState.RenderTarget[0].RenderTargetWriteMask = 0xF;
                desc.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
                library::pipeline(
                    name,
                    |library, name| library.LoadGraphicsPipeline(name, &desc),
                    || device.CreateGraphicsPipelineState(&desc),
                )
            }
            "s" => {
                #[repr(C)]
                struct Stream {
                    root: Sub<*mut std::ffi::c_void>,
                    cs: Sub<D3D12_SHADER_BYTECODE>,
                }
                let mut stream = Stream {
                    root: Sub(
                        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE,
                        root.as_raw(),
                    ),
                    cs: Sub(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS, code(&cs)),
                };
                stream_pipeline(device, &mut stream, name)
            }
            _ => {
                let desc = D3D12_COMPUTE_PIPELINE_STATE_DESC {
                    pRootSignature: std::mem::transmute_copy(root),
                    CS: code(&cs),
                    ..Default::default()
                };
                library::pipeline(
                    name,
                    |library, name| library.LoadComputePipeline(name, &desc),
                    || device.CreateComputePipelineState(&desc),
                )
            }
        }
    };
    (
        made.expect("a pipeline"),
        began.elapsed().as_secs_f64() * 1e3,
    )
}

/// What the graphics pipeline `pso` writes: a target 16 texels wide, one
/// for each sampler.
fn draw(probe: &Probe, root: &ID3D12RootSignature, pso: &ID3D12PipelineState) -> Vec<[u32; 4]> {
    // SAFETY: plain creations and an open list; all of it lives until the
    // list has run; the read-back buffer holds the 256 bytes read.
    unsafe {
        let mut target: Option<ID3D12Resource> = None;
        probe
            .device
            .CreateCommittedResource(
                &D3D12_HEAP_PROPERTIES {
                    Type: D3D12_HEAP_TYPE_DEFAULT,
                    ..Default::default()
                },
                D3D12_HEAP_FLAG_NONE,
                &D3D12_RESOURCE_DESC {
                    Dimension: D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                    Width: 16,
                    Height: 1,
                    DepthOrArraySize: 1,
                    MipLevels: 1,
                    Format: DXGI_FORMAT_R32G32B32A32_FLOAT,
                    SampleDesc: DXGI_SAMPLE_DESC {
                        Count: 1,
                        Quality: 0,
                    },
                    Flags: D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                    ..Default::default()
                },
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                None,
                &mut target,
            )
            .unwrap();
        let target = target.unwrap();
        let views: ID3D12DescriptorHeap = probe
            .device
            .CreateDescriptorHeap(&D3D12_DESCRIPTOR_HEAP_DESC {
                Type: D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
                NumDescriptors: 1,
                ..Default::default()
            })
            .unwrap();
        let view = views.GetCPUDescriptorHandleForHeapStart();
        probe.device.CreateRenderTargetView(&target, None, view);
        let back = probe
            .buffer(
                D3D12_HEAP_TYPE_READBACK,
                256,
                D3D12_RESOURCE_FLAG_NONE,
                D3D12_RESOURCE_STATE_COPY_DEST,
            )
            .unwrap();
        let (_allocator, list) = probe.list().unwrap();
        list.SetDescriptorHeaps(&[Some(probe.views.clone())]);
        list.OMSetRenderTargets(1, Some(&view), false, None);
        list.RSSetViewports(&[D3D12_VIEWPORT {
            Width: 16.0,
            Height: 1.0,
            MaxDepth: 1.0,
            ..Default::default()
        }]);
        list.RSSetScissorRects(&[RECT {
            left: 0,
            top: 0,
            right: 16,
            bottom: 1,
        }]);
        list.SetGraphicsRootSignature(root);
        list.SetGraphicsRoot32BitConstants(0, 4, [0u32; 4].as_ptr().cast(), 0);
        list.SetGraphicsRootDescriptorTable(1, probe.views.GetGPUDescriptorHandleForHeapStart());
        list.SetPipelineState(pso);
        list.IASetPrimitiveTopology(
            windows::Win32::Graphics::Direct3D::D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST,
        );
        list.DrawInstanced(3, 1, 0, 0);
        list.ResourceBarrier(&[crate::host::transition(
            &target,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_COPY_SOURCE,
        )]);
        let to = D3D12_TEXTURE_COPY_LOCATION {
            pResource: std::mem::transmute_copy(&back),
            Type: D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
            Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 {
                PlacedFootprint: D3D12_PLACED_SUBRESOURCE_FOOTPRINT {
                    Offset: 0,
                    Footprint: D3D12_SUBRESOURCE_FOOTPRINT {
                        Format: DXGI_FORMAT_R32G32B32A32_FLOAT,
                        Width: 16,
                        Height: 1,
                        Depth: 1,
                        RowPitch: 256,
                    },
                },
            },
        };
        let from = D3D12_TEXTURE_COPY_LOCATION {
            pResource: std::mem::transmute_copy(&target),
            Type: D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
            Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 {
                SubresourceIndex: 0,
            },
        };
        list.CopyTextureRegion(&to, 0, 0, 0, &from, None);
        probe.execute(&list).unwrap();
        let mut data = std::ptr::null_mut();
        back.Map(0, None, Some(&mut data)).unwrap();
        let read = std::slice::from_raw_parts(data.cast::<[u32; 4]>(), 16).to_vec();
        back.Unmap(0, None);
        read
    }
}

/// One run: a process of its own that does what `LEONIDA_PLACES_RUN` says
/// with the cache folder `LEONIDA_PLACES_DIR`, and prints what it found on
/// lines that begin with `PLACES`. The steps, in their order, apart by `;`:
///
/// - `make=<samplers>`: a root signature for each, kept. `drop=<samplers>`:
///   made and let go. `heap=<n>`: n samplers in a descriptor heap, kept.
///   Before `host` they are made on a device of the test's own: the card's
///   one device, before the host has it.
/// - `host`: the cache folder set and the host device made, as a run of
///   the title does.
/// - `list=<samplers>`, `many=<n>`: those samplers, or n different ones,
///   written to the list as an earlier run would have (before `host`).
/// - `number=<n>`: the code of the pipelines after it differs by it.
/// - `pipe=<kind>:<samplers>`: a pipeline of the kind ([`pipeline`]) with
///   those static samplers, from the library or made as a host pipeline of
///   the title is ([`library_use`], `library::pipeline`), then run. With
///   `!` after the kind it is kept in the library under its plain name
///   whatever its use: what a later run then loads tells whether the
///   samplers have their places of this run there.
/// - `save`: the library saved, as at the end of a run.
#[test]
#[ignore = "the processes of the tests beside it; alone it does nothing"]
fn a_run() {
    let (Ok(steps), Some(dir)) = (
        std::env::var("LEONIDA_PLACES_RUN"),
        std::env::var_os("LEONIDA_PLACES_DIR"),
    ) else {
        return;
    };
    let mut device: Option<ID3D12Device> = None;
    let mut host: Option<&'static crate::host::Host> = None;
    let mut probe: Option<Probe> = None;
    let mut kept: Vec<ID3D12RootSignature> = Vec::new();
    let mut heaps: Vec<ID3D12DescriptorHeap> = Vec::new();
    let mut number = 0u32;
    for step in steps.split(';').filter(|s| !s.is_empty()) {
        let (what, value) = step.split_once('=').unwrap_or((step, ""));
        if what == "host" {
            library::use_pipeline_cache(dir.clone().into());
            let Some(made) = crate::host::host() else {
                println!("PLACES none");
                return;
            };
            println!("PLACES vendor={:x}", made.adapter_vendor_id().unwrap_or(0));
            host = Some(made);
            device = Some(made.device.clone());
            continue;
        }
        if matches!(what, "list" | "many") {
            use std::io::Write;
            let names = match what {
                "list" => samplers(value),
                _ => (0..value.parse::<usize>().unwrap())
                    .map(|n| {
                        format!(
                            "{}{}",
                            n % 100,
                            ["", "p", "w", "pw", "b", "pb", "m", "pm"][n / 100]
                        )
                    })
                    .collect(),
            };
            std::fs::create_dir_all(&dir).unwrap();
            let path = Path::new(&dir).join(FILE);
            let mut file = std::fs::OpenOptions::new()
                .append(true)
                .create(true)
                .open(path)
                .unwrap();
            let mut lines = String::new();
            if file.metadata().unwrap().len() == 0 {
                lines.push_str(&format!("{HEAD}00aa\n"));
            }
            for name in names {
                lines.push_str(&Sampler::of(&sampler(&name, 0, false)).line());
                lines.push('\n');
            }
            file.write_all(lines.as_bytes()).unwrap();
            continue;
        }
        if device.is_none() {
            // SAFETY: a plain creation.
            if unsafe { D3D12CreateDevice(None, D3D_FEATURE_LEVEL_12_0, &mut device) }.is_err() {
                println!("PLACES none");
                return;
            }
        }
        let device = device.as_ref().unwrap();
        match what {
            "make" | "drop" => {
                for name in samplers(value) {
                    let made =
                        root(device, &[sampler(&name, 0, false)], true).expect("a root signature");
                    if what == "make" {
                        kept.push(made);
                    }
                }
            }
            "heap" => {
                let count: u32 = value.parse().unwrap();
                // SAFETY: a heap of `count` samplers, each written inside it.
                unsafe {
                    let heap: ID3D12DescriptorHeap = device
                        .CreateDescriptorHeap(&D3D12_DESCRIPTOR_HEAP_DESC {
                            Type: D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                            NumDescriptors: count,
                            Flags: D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
                            ..Default::default()
                        })
                        .unwrap();
                    let step = device
                        .GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
                        as usize;
                    let first = heap.GetCPUDescriptorHandleForHeapStart().ptr;
                    for n in 0..count {
                        let desc = D3D12_SAMPLER_DESC {
                            Filter: D3D12_FILTER_MIN_MAG_MIP_POINT,
                            AddressU: D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                            AddressV: D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                            AddressW: D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                            MipLODBias: (60 + n % 60) as f32 / 8.0,
                            MaxAnisotropy: 1,
                            MaxLOD: 1000.0,
                            ..Default::default()
                        };
                        device.CreateSampler(
                            &desc,
                            D3D12_CPU_DESCRIPTOR_HANDLE {
                                ptr: first + n as usize * step,
                            },
                        );
                    }
                    heaps.push(heap);
                }
            }
            "number" => number = value.parse().unwrap(),
            "save" => library::save(true),
            "pipe" => {
                let host = host.expect("host before pipe");
                let (kind, names) = value.split_once(':').unwrap();
                let (kind, plainly) = match kind.strip_suffix('!') {
                    Some(kind) => (kind, true),
                    None => (kind, false),
                };
                let names = samplers(names);
                assert!(names.len() <= SLOT);
                let descs: Vec<_> = names
                    .iter()
                    .enumerate()
                    .map(|(n, name)| sampler(name, n as u32, kind == "g"))
                    .collect();
                let range = D3D12_DESCRIPTOR_RANGE1 {
                    RangeType: D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
                    NumDescriptors: TEXTURES,
                    Flags: D3D12_DESCRIPTOR_RANGE_FLAG_NONE,
                    ..Default::default()
                };
                // Asked, and the root signature made, as for a host pipeline
                // of the title.
                let began = std::time::Instant::now();
                let usage = library_use(host, &descs);
                let asked = began.elapsed().as_secs_f64() * 1e3;
                let (root, root_blob) =
                    root_signature(device, &probe_parameters(&range), &descs, kind != "g")
                        .expect("a root signature");
                let plain =
                    library::name(&format!("places test {kind} {number}"), &root_blob, &[], "");
                let loaded = library::LOADED.load(Ordering::Relaxed);
                let name = if plainly {
                    Some(plain.clone())
                } else {
                    usage.name(&plain)
                };
                let (pso, ms) = pipeline(device, kind, &root, names.len(), number, name.as_deref());
                let loaded = library::LOADED.load(Ordering::Relaxed) > loaded;
                // The same from code that differs by a number, made now:
                // through the samplers as they are in this process.
                let (now, _) = pipeline(device, kind, &root, names.len(), number + 1_000_000, None);
                if probe.is_none() {
                    probe = Some(Probe::new(device).expect("a probe"));
                }
                let probe = probe.as_ref().unwrap();
                let (wrote, right) = if kind == "g" {
                    (draw(probe, &root, &pso), draw(probe, &root, &now))
                } else {
                    let run = |pso: ID3D12PipelineState| {
                        probe
                            .run(
                                &[Run {
                                    root: root.clone(),
                                    pso,
                                    constants: [0; 4],
                                }],
                                1,
                            )
                            .expect("run")
                    };
                    (run(pso), run(now))
                };
                let wrong = (0..names.len())
                    .filter(|&n| wrote[n][..3] != right[n][..3])
                    .count();
                let read: Vec<String> = (0..names.len())
                    .map(|n| ((f32::from_bits(wrote[n][0]) * 8.0).round() as i32).to_string())
                    .collect();
                println!(
                    "PLACES pipe {kind} {} {} {ms:.1} {wrong} {} {asked:.1}",
                    match usage {
                        Use::Plain => "plain",
                        Use::Skip => "skip",
                        Use::Placed(_) => "placed",
                    },
                    if loaded { "loaded" } else { "made" },
                    read.join(",")
                );
            }
            other => panic!("no step {other}"),
        }
    }
    let places = PLACES.get().and_then(Option::as_ref);
    println!(
        "PLACES places={}",
        match places.map(|p| p.same.get()) {
            None => "none",
            Some(None) => "unasked",
            Some(Some(true)) => "same",
            Some(Some(false)) => "differ",
        }
    );
    // With the debug layer (`LEONIDA_D3D12_DEBUG=1`): what it held against us.
    if let Some(error) = host.and_then(|host| host.log_messages()) {
        println!("PLACES debug {error}");
    }
    // The process ends here: what holds places is not released first.
    std::mem::forget((kept, heaps));
}

/// What a pipeline of a run did: its kind, how it used the library, whether
/// it was loaded, how long that took, through how many of its samplers it
/// read otherwise than the same pipeline made in that process, the eighths
/// of a level each sampler added, and how long asking for its use took
/// (the first that asks has the places checked).
#[derive(Debug)]
struct Pipe {
    kind: String,
    usage: String,
    loaded: bool,
    ms: f64,
    wrong: usize,
    read: Vec<i32>,
    asked: f64,
}

#[derive(Debug)]
struct Ran {
    nvidia: bool,
    /// Of the check: `none` (no places kept), `unasked`, `same`, `differ`.
    places: String,
    pipes: Vec<Pipe>,
}

/// Runs `steps` ([`a_run`]) in a process of its own with the cache folder
/// `dir` and `LEONIDA_LIBRARY_STATIC_SAMPLERS` as `mode` (unset for none).
/// `None`: no card here.
fn run(dir: &Path, mode: Option<&str>, steps: &str) -> Option<Ran> {
    let mut command = std::process::Command::new(std::env::current_exe().unwrap());
    command
        .args([
            "--exact",
            "pipeline::places::tests::a_run",
            "--ignored",
            "--nocapture",
        ])
        .env("LEONIDA_PLACES_RUN", steps)
        .env("LEONIDA_PLACES_DIR", dir)
        // The device of the steps before `host` and the host's are one: the
        // default card's.
        .env_remove("LEONIDA_ADAPTER")
        .env_remove("LEONIDA_D3D12_WARP");
    match mode {
        Some(mode) => command.env("LEONIDA_LIBRARY_STATIC_SAMPLERS", mode),
        None => command.env_remove("LEONIDA_LIBRARY_STATIC_SAMPLERS"),
    };
    let output = command.output().expect("the run");
    let text = String::from_utf8_lossy(&output.stdout);
    assert!(
        output.status.success(),
        "the run of {steps} failed:\n{text}\n{}",
        String::from_utf8_lossy(&output.stderr)
    );
    let mut ran = Ran {
        nvidia: false,
        places: String::new(),
        pipes: Vec::new(),
    };
    let mut card = false;
    for line in text.lines().filter_map(|l| l.strip_prefix("PLACES ")) {
        let words: Vec<&str> = line.split(' ').collect();
        match words[0] {
            "none" => return None,
            "pipe" => ran.pipes.push(Pipe {
                kind: words[1].to_owned(),
                usage: words[2].to_owned(),
                loaded: words[3] == "loaded",
                ms: words[4].parse().unwrap(),
                wrong: words[5].parse().unwrap(),
                read: words[6].split(',').map(|k| k.parse().unwrap()).collect(),
                asked: words[7].parse().unwrap(),
            }),
            "debug" => panic!("the debug layer, in the run of {steps}: {line}"),
            word => match word.split_once('=') {
                Some(("vendor", vendor)) => {
                    card = true;
                    ran.nvidia = vendor == "10de";
                }
                Some(("places", places)) => ran.places = places.to_owned(),
                _ => panic!("{line}"),
            },
        }
    }
    card.then_some(ran)
}

/// A number for code no run has compiled before.
fn fresh() -> u32 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_or(1, |d| d.as_micros() % 900_000) as u32
}

fn read(ran: &Ran) -> Vec<&[i32]> {
    ran.pipes.iter().map(|p| &p.read[..]).collect()
}

/// The fault. A run stores three pipelines with static samplers; the next
/// makes ten other static samplers first and loads them: each samples
/// through the samplers now at the places its own had in the first run.
#[test]
#[ignore = "on the card, in processes of its own; see the module's notes"]
fn a_stored_pipeline_reads_through_the_places_of_the_run_that_stored_it() {
    let dir = tempfile::tempdir().unwrap();
    let Some(first) = run(
        dir.path(),
        Some("1"),
        "host;pipe=c:1,2p,3w;pipe=g:4,5p;pipe=s:6w,7;save",
    ) else {
        return;
    };
    println!("stored: {first:?}");
    assert!(
        first
            .pipes
            .iter()
            .all(|p| p.usage == "plain" && !p.loaded && p.wrong == 0)
    );
    assert_eq!(read(&first), [&[1, 2, 3][..], &[4, 5], &[6, 7]]);
    let second = run(
        dir.path(),
        Some("1"),
        "host;make=40-49;pipe=s:6w,7;pipe=g:4,5p;pipe=c:1,2p,3w",
    )
    .expect("the card again");
    println!("loaded after ten other samplers: {second:?}");
    assert!(
        second.pipes.iter().all(|p| p.usage == "plain" && p.loaded),
        "loaded from the library"
    );
    if !second.nvidia {
        println!("not an NVIDIA card: nothing held against what it read");
        return;
    }
    // The first run's samplers had places 0 to 6 in the order they were
    // made (1, 2p, 3w; 4, 5p; 6w, 7); here the ten others have 0 to 9.
    assert_eq!(read(&second), [&[45, 46][..], &[43, 44], &[40, 41, 42]]);
    assert!(
        second.pipes.iter().all(|p| p.wrong == p.read.len()),
        "every sampler another one"
    );
    // As before this change (`=0`): made by the driver, and right.
    let skipped = run(
        dir.path(),
        Some("0"),
        "host;make=40-49;pipe=s:6w,7;pipe=g:4,5p;pipe=c:1,2p,3w",
    )
    .unwrap();
    println!("made by the driver: {skipped:?}");
    assert!(
        skipped
            .pipes
            .iter()
            .all(|p| p.usage == "skip" && !p.loaded && p.wrong == 0)
    );
    assert_eq!(skipped.places, "none");
}

/// How the driver gives out places, read through a stored pipeline whose
/// sixteen samplers had places 0 to 15: in a later run each reads whatever
/// is at its place now.
#[test]
#[ignore = "on the card, in processes of its own; see the module's notes"]
fn the_table_of_static_samplers() {
    let dir = tempfile::tempdir().unwrap();
    let Some(stored) = run(dir.path(), Some("1"), "host;pipe=c:0-15;save") else {
        return;
    };
    assert_eq!(stored.pipes[0].read, (0..16).collect::<Vec<_>>());
    let table = |steps: &str| {
        let ran =
            run(dir.path(), Some("1"), &format!("{steps};pipe=c:0-15")).expect("the card again");
        let table = ran.pipes.last().unwrap();
        assert!(table.loaded);
        assert_eq!(table.kind, "c");
        println!("{steps}: places 0 to 15 hold {:?}", table.read);
        (ran.nvidia, table.read.clone())
    };
    let after = |first: &[i32]| {
        first
            .iter()
            .copied()
            .chain(0..16)
            .take(16)
            .collect::<Vec<_>>()
    };
    let (nvidia, same) = table("host");
    assert_eq!(same, after(&[]), "the same order: the same places");
    if !nvidia {
        table("host;make=20,21,22");
        println!("not an NVIDIA card: nothing held against what it read");
        return;
    }
    // In the order they are first made, by root signatures alone.
    assert_eq!(table("host;make=22,20,21").1, after(&[22, 20, 21]));
    // By pipelines' root signatures too, in the order of their arrays.
    assert_eq!(
        table("host;pipe=c:31,30;pipe=g:33,32").1,
        after(&[31, 30, 33, 32])
    );
    // One place for a sampler, whatever its register, space and stage, however
    // often.
    assert_eq!(table("host;make=20,20v,21,20").1, after(&[20, 21]));
    // A place is free when its last root signature is gone, and the lowest
    // free place is given next.
    assert_eq!(table("host;drop=20,21;make=22").1, after(&[22]));
    // Samplers in descriptor heaps have no places.
    assert_eq!(table("host;heap=64;make=20").1, after(&[20]));
    // The table is the device's, and a process has one device for a card:
    // what was made with another handle before the host's is there.
    assert_eq!(table("make=20,21;host").1, after(&[20, 21]));
}

/// The driver's own cache on disk keeps a pipeline with the places of its
/// static samplers: at other places it is compiled again. A measurement:
/// the four times are printed.
#[test]
#[ignore = "a measurement on the card, in processes of its own; see the module's notes"]
fn the_drivers_own_cache_knows_the_places() {
    let dir = tempfile::tempdir().unwrap();
    let number = fresh();
    let made = |steps: &str| {
        let ran = run(
            dir.path(),
            Some("0"),
            &format!("{steps};number={number};pipe=c:1,2,3;pipe=g:4,5"),
        )?;
        assert!(
            ran.pipes.iter().all(|p| !p.loaded && p.wrong == 0),
            "{ran:?}"
        );
        Some((ran.pipes[0].ms, ran.pipes[1].ms))
    };
    let Some(first) = made("host") else { return };
    let again = made("host").unwrap();
    let moved = made("host;make=40").unwrap();
    let moved_again = made("host;make=40").unwrap();
    println!("made by the driver (compute, graphics; ms): first {first:?}, the next run {again:?}");
    println!("with one other sampler made first: {moved:?}, the next such run {moved_again:?}");
}

/// What is done about it: the samplers a run meets are listed, the next
/// run makes them first and stores its pipelines, and every run after that
/// loads them and samples right, whatever else it makes and in whatever
/// order; a run in which something was before the list is found out and
/// leaves the library alone.
#[test]
#[ignore = "on the card, in processes of its own; see the module's notes"]
fn pipelines_with_static_samplers_come_from_the_library() {
    let dir = tempfile::tempdir().unwrap();
    // Code no run has compiled before: the times tell what the driver's own
    // cache had.
    let three = format!("number={};pipe=c:1,2p,3w;pipe=g:4,5p;pipe=s:6w,7", fresh());
    let three = &three[..];
    let right = |ran: &Ran, steps: &str| {
        println!("{steps}: {ran:?}");
        assert!(
            ran.pipes.iter().all(|p| p.wrong == 0),
            "{steps}: a pipeline read through another sampler"
        );
    };
    // A first run: nothing is listed yet, so the driver makes them all.
    let Some(learns) = run(dir.path(), None, &format!("host;{three};save")) else {
        return;
    };
    right(&learns, "the first run");
    if !learns.nvidia {
        println!("not an NVIDIA card: its pipelines keep their plain names");
        assert!(learns.pipes.iter().all(|p| p.usage == "plain"));
        return;
    }
    assert!(learns.pipes.iter().all(|p| p.usage == "skip" && !p.loaded));
    let list = std::fs::read_to_string(dir.path().join(FILE)).expect("the list");
    assert_eq!(
        List::parse(&list).expect("a list").samplers,
        listed("1,2p,3w,4,5p,6w,7")
    );
    // The second: the list is made first, the pipelines are made and stored.
    let stores = run(dir.path(), None, &format!("host;{three};save")).unwrap();
    right(&stores, "the second run");
    assert!(
        stores
            .pipes
            .iter()
            .all(|p| p.usage == "placed" && !p.loaded)
    );
    assert_eq!(stores.places, "same");
    // The third makes other samplers and the pipelines in another order:
    // all loaded, and each through its own samplers.
    let number = three.split(';').next().unwrap();
    let steps = &format!(
        "host;make=40-49;heap=16;{number};pipe=s:6w,7;pipe=c:20,21;pipe=g:4,5p;pipe=c:1,2p,3w;save"
    );
    let loads = run(dir.path(), None, steps).unwrap();
    right(&loads, "the third run");
    assert_eq!(read(&loads), [&[6, 7][..], &[20, 21], &[4, 5], &[1, 2, 3]]);
    assert_eq!(loads.places, "same");
    let usage: Vec<_> = loads
        .pipes
        .iter()
        .map(|p| (p.usage.as_str(), p.loaded))
        .collect();
    // (20 and 21 were new to it: the driver's for this run, listed for the next.)
    assert_eq!(
        usage,
        [
            ("placed", true),
            ("skip", false),
            ("placed", true),
            ("placed", true)
        ]
    );
    // The next run stores the pipeline with the samplers that were new,
    // the one after loads it; the older ones stay loaded all along.
    let all = format!("host;{number};pipe=c:20,21;{three};save");
    let grown = run(dir.path(), None, &all).unwrap();
    right(&grown, "the fourth run");
    let usage: Vec<_> = grown
        .pipes
        .iter()
        .map(|p| (p.usage.as_str(), p.loaded))
        .collect();
    assert_eq!(
        usage,
        [
            ("placed", false),
            ("placed", true),
            ("placed", true),
            ("placed", true)
        ]
    );
    assert_eq!(grown.places, "same");
    let fifth = run(dir.path(), None, &all).unwrap();
    right(&fifth, "the fifth run");
    assert!(fifth.pipes.iter().all(|p| p.usage == "placed" && p.loaded));
    // Static samplers on the card's device before the host has it: the
    // places are not the library's, which the check finds, and the driver
    // makes the pipelines. Two samplers of nobody's, then one that is in
    // the list (only the places before its own are other ones then).
    for before in ["make=50,51", "make=3w"] {
        let steps = format!("{before};host;{three};pipe=c:20,21;save");
        let found = run(dir.path(), None, &steps).unwrap();
        right(&found, &steps);
        assert_eq!(found.places, "differ", "{steps}");
        assert!(
            found.pipes.iter().all(|p| p.usage == "skip" && !p.loaded),
            "{steps}"
        );
    }
    // Loaded with no care for the places (`=1`) such a run reads wrong
    // (the plain names are not in this library: stored now, loaded next).
    let plain = format!("host;{three};save");
    run(dir.path(), Some("1"), &plain).unwrap();
    let careless = run(dir.path(), Some("1"), &format!("make=50,51;{plain}")).unwrap();
    println!("with no care for the places: {careless:?}");
    assert!(
        careless
            .pipes
            .iter()
            .all(|p| p.usage == "plain" && p.loaded && p.wrong > 0)
    );
    // And a run like the others afterwards is as it was: all loaded, right.
    let last = run(dir.path(), None, &all).unwrap();
    right(&last, "the last run");
    assert!(last.pipes.iter().all(|p| p.usage == "placed" && p.loaded));
    assert_eq!(last.places, "same");
    // `=0`: as before this change.
    let off = run(dir.path(), Some("0"), &all).unwrap();
    right(&off, "switched off");
    assert!(off.pipes.iter().all(|p| p.usage == "skip" && !p.loaded));
    assert_eq!(off.places, "none");
}

/// A sampler a run meets first is made at the place it has in the next run,
/// when it is in the list: pipelines kept in the library by the first run
/// whatever their use (the test's doing, not the title's) read right when
/// the second loads them, with other static samplers made in between.
#[test]
#[ignore = "on the card, in processes of its own; see the module's notes"]
fn a_sampler_met_in_a_run_has_the_place_of_the_next() {
    let dir = tempfile::tempdir().unwrap();
    let steps = "host;make=40,41;pipe=c!:1,2p,3w;make=42;pipe=g!:4,5p;drop=43;pipe=s!:6w,7;save";
    let Some(met) = run(dir.path(), None, steps) else {
        return;
    };
    println!("{steps}: {met:?}");
    if !met.nvidia {
        println!("not an NVIDIA card: no places are kept");
        return;
    }
    assert!(
        met.pipes
            .iter()
            .all(|p| p.usage == "skip" && !p.loaded && p.wrong == 0)
    );
    let steps = "host;pipe=s!:6w,7;pipe=g!:4,5p;pipe=c!:1,2p,3w";
    let listed = run(dir.path(), None, steps).unwrap();
    println!("{steps}: {listed:?}");
    assert!(
        listed.pipes.iter().all(|p| p.usage == "placed" && p.loaded),
        "loaded under their plain names"
    );
    assert!(
        listed.pipes.iter().all(|p| p.wrong == 0),
        "a sampler had another place in the run that met it"
    );
    assert_eq!(read(&listed), [&[6, 7][..], &[4, 5], &[1, 2, 3]]);
}

/// The check holds nothing against a run like the one before, with every
/// kind of sampler in the list and many of them, and finds a run in which
/// one sampler was made before the list. A measurement too: how long the
/// first pipeline's question takes with 320 samplers listed (the run that
/// stores the check's pipelines, the first that loads them, a later one).
#[test]
#[ignore = "on the card, in processes of its own; see the module's notes"]
fn the_check_looks_at_every_kind_of_sampler() {
    let dir = tempfile::tempdir().unwrap();
    let kinds =
        "list=1a,2c,3n,4b,5m,6l,7x,8cb,9ap,10cw,11cx,12cp,13nw,14al,15xb,16pm,17ax,18lw,19cm,20bl";
    let Some(stores) = run(
        dir.path(),
        None,
        &format!("{kinds};many=300;host;pipe=c:1,2p;save"),
    ) else {
        return;
    };
    if !stores.nvidia {
        println!("not an NVIDIA card: no places are kept");
        return;
    }
    assert_eq!(stores.places, "same", "{stores:?}");
    let mut asked = vec![stores.pipes[0].asked];
    for _ in 0..2 {
        let again = run(dir.path(), None, "host;make=50;pipe=c:1,2p;save").unwrap();
        assert_eq!(again.places, "same", "{again:?}");
        assert!(
            again.pipes[0].loaded && again.pipes[0].wrong == 0,
            "{again:?}"
        );
        asked.push(again.pipes[0].asked);
    }
    println!(
        "320 places: stored in {:.0} ms, first held against in {:.0} ms, then in {:.0} ms",
        asked[0], asked[1], asked[2]
    );
    let before = run(dir.path(), None, "make=50;host;pipe=c:1,2p").unwrap();
    assert_eq!(before.places, "differ", "{before:?}");
    assert!(
        !before.pipes[0].loaded && before.pipes[0].wrong == 0,
        "{before:?}"
    );
    println!(
        "with a sampler before the list: found in {:.0} ms",
        before.pipes[0].asked
    );
}
