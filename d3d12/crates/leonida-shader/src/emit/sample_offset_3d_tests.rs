//! End-to-end D3D12 hardware and WARP tests for 3D `_o` signed Z texel offsets
//! and `image_get_lod` `dmask` `Z`/`W` zero-fill (including `d16` packing).

use super::bitcode::{Attr, Term};
use super::container::{self, SigElement};
use super::host::{Dispatch, Draw, Host, Root, View};
use super::intersect_tests::build_compute_shader;
use super::shader::Shader;
use super::valu_tests::ONE_AT_A_TIME;
use super::{HostShader, emit, validator};
use crate::dxbc::{Container, FourCC};
use crate::native::{
    ComputeIo, HwStage, NativeProgram, ResourceType, RootParam, RootType, StageIo, UsedResource,
};
use crate::{io, ir, resource, structure};
use windows::Win32::Graphics::Direct3D12::*;

fn pack_offset_3d(ox: i32, oy: i32, oz: i32) -> u32 {
    let f = |v: i32| (v as u32) & 0x3F;
    f(ox) | (f(oy) << 8) | (f(oz) << 16)
}

#[test]
fn volume_sample_lz_o_applies_signed_z_texel_offset() {
    let _one = ONE_AT_A_TIME.lock().unwrap_or_else(|e| e.into_inner());
    const DIM: usize = 4;
    let mut words = vec![
        0xBE81_0380,
        0xBE83_0380, // s1, s3 = 0
        0xF40C_0400,
        0xFA00_0000, // volume SRV -> s[16:23] from table 0 + 0
        0xF408_0300,
        0xFA00_0020, // raw output UAV -> s[12:15] from table 0 + 32
        0xF408_0601,
        0xFA00_0000, // sampler -> s[24:27] from table 1 + 0
        0x7E1E_0280, // v15 = 0
    ];

    // Base coordinate at center of texel (1, 2, 2) in a 4x4x4 volume.
    let base = [1.5f32 / 4.0, 2.5 / 4.0, 2.5 / 4.0];
    let cases = [
        (0, 0, 0, [1.0f32, 2.0, 2.0, 221.0]),
        (-1, 1, -2, [0.0, 3.0, 0.0, 30.0]),
        (2, -2, 1, [3.0, 0.0, 3.0, 303.0]),
    ];

    for (i, &(ox, oy, oz, _)) in cases.iter().enumerate() {
        let off = pack_offset_3d(ox, oy, oz);
        let addr_words = [off, base[0].to_bits(), base[1].to_bits(), base[2].to_bits()];
        for (reg, val) in addr_words.into_iter().enumerate() {
            words.extend([0x7E00_02FF | ((reg as u32) << 17), val]);
        }
        // image_sample_lz_o (0x37), dim=2 (3D: 2 << 3 = 0x10), dmask=0xF, vaddr=v0, vdata=v8, srsrc=s16, ssamp=s24
        words.extend([0xF000_0F10 | (0x37 << 18), 0x00C4_0800]);
        for ch in 0..4u32 {
            let byte_off = (i as u32 * 4 + ch) * 4;
            words.extend([
                0xE070_1000 | byte_off,
                0x8003_000F | ((8 + ch) << 8),
            ]);
        }
    }
    words.push(0xBF81_0000);

    let table = |slot, user_data| RootParam {
        root: RootType::Table,
        table_offset_dwords: 0,
        sgprs: 1,
        base_register: 0,
        user_data,
        slot,
    };
    let range = |kind, at, dwords, slot, user_data| UsedResource {
        kind,
        root: RootType::Table,
        table_offset_dwords: at,
        lower_bound: 0,
        stride: 0,
        dwords,
        user_data,
        slot,
    };
    let native = NativeProgram {
        stage: HwStage::Cs,
        wave64: false,
        vgprs: 16,
        layout: vec![table(0, 0), table(1, 2)],
        used: vec![
            range(ResourceType::Srv, 0, 8, 0, 0),
            range(ResourceType::Uav, 8, 4, 0, 0),
            range(ResourceType::Sampler, 0, 4, 1, 2),
        ],
        io: StageIo::Cs(ComputeIo {
            rsrc2: 3 << 1,
            threads: [1, 1, 1],
        }),
        code: words.into_iter().flat_map(u32::to_le_bytes).collect(),
    };
    let (shader, _) = build_compute_shader(&native).expect("build_compute_shader");
    if let Ok(mut v) = validator::Validator::load(validator::Validator::default_path().as_deref()) {
        let mut copy = shader.dxil.clone();
        v.validate(&mut copy).expect("DXIL validation failed");
    }

    let mut texels = Vec::with_capacity(DIM * DIM * DIM);
    for z in 0..DIM {
        for y in 0..DIM {
            for x in 0..DIM {
                texels.push([
                    x as f32,
                    y as f32,
                    z as f32,
                    (100 * z + 10 * y + x) as f32,
                ]);
            }
        }
    }
    let sampler = D3D12_SAMPLER_DESC {
        Filter: D3D12_FILTER_MIN_MAG_MIP_POINT,
        AddressU: D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        AddressV: D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        AddressW: D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        MaxLOD: f32::MAX,
        MaxAnisotropy: 1,
        ..Default::default()
    };

    let mut ran = 0;
    for (backend, warp) in [("hardware", false), ("warp", true)] {
        let Ok(host) = Host::new(warp) else {
            continue;
        };
        let (root, pso) = host.compute_pipeline(&shader).expect("compute_pipeline");
        let out = host
            .dispatch(
                &root,
                &pso,
                &Dispatch {
                    roots: vec![(
                        0,
                        Root::Constants(vec![0; shader.user_data_dwords().max(1) as usize]),
                    )],
                    views: &[
                        View::Volume(DIM as u32, DIM as u32, DIM as u32, &texels),
                        View::RawUav((cases.len() * 4) as u32),
                    ],
                    samplers: &[sampler],
                    groups: [1, 1, 1],
                },
            )
            .expect("dispatch");
        let floats: Vec<f32> = out[0].iter().copied().map(f32::from_bits).collect();
        for (i, &(ox, oy, oz, exp)) in cases.iter().enumerate() {
            let got = &floats[i * 4..(i + 1) * 4];
            assert_eq!(
                got,
                &exp,
                "{backend} case {i} offset=({ox},{oy},{oz}): got {got:?}, expected {exp:?}"
            );
        }
        ran += 1;
    }
    assert!(ran > 0, "no SM 6.6 device available");
}

fn build_get_lod_ps(d16: bool) -> HostShader {
    let mut words = vec![
        0xBE81_0380,
        0xBE83_0380, // s1, s3 = 0
        0xF40C_0400,
        0xFA00_0000, // 2D SRV -> s[16:23] from table 0
        0xF408_0601,
        0xFA00_0000, // sampler -> s[24:27] from table 1
    ];
    // image_get_lod (0x60), dim=1 (2D: 1 << 3 = 0x08), dmask=0xF, vaddr=v0, vdata=v4, srsrc=s16, ssamp=s24
    let w1 = 0x00C4_0400 | if d16 { 1 << 31 } else { 0 };
    words.extend([0xF000_0F08 | (0x60 << 18), w1]);
    if d16 {
        // Unpack [v4.lo, v4.hi, v5.lo, v5.hi] -> v8, v9, v10, v11 via v_cvt_f32_f16 (VOP1 0x00b) and SDWA WORD_1.
        words.push(0x7E10_0000 | (0x00b << 9) | 260); // v_cvt_f32_f16 v8, v4
        words.extend([0x7E12_00F9 | (0x00b << 9), 4 | 6 << 8 | 5 << 16]); // v_cvt_f32_f16_sdwa v9, v4 src0_sel:WORD_1
        words.push(0x7E14_0000 | (0x00b << 9) | 261); // v_cvt_f32_f16 v10, v5
        words.extend([0x7E16_00F9 | (0x00b << 9), 5 | 6 << 8 | 5 << 16]); // v_cvt_f32_f16_sdwa v11, v5 src0_sel:WORD_1
        words.extend([0xF800_180F, 8 | 9 << 8 | 10 << 16 | 11 << 24]); // exp mrt0, v8, v9, v10, v11 done vm
    } else {
        words.extend([0xF800_180F, 4 | 5 << 8 | 6 << 16 | 7 << 24]); // exp mrt0, v4, v5, v6, v7 done vm
    }
    words.push(0xBF81_0000);

    let table = |slot, user_data| RootParam {
        root: RootType::Table,
        table_offset_dwords: 0,
        sgprs: 1,
        base_register: 0,
        user_data,
        slot,
    };
    let range = |kind, at, dwords, slot, user_data| UsedResource {
        kind,
        root: RootType::Table,
        table_offset_dwords: at,
        lower_bound: 0,
        stride: 0,
        dwords,
        user_data,
        slot,
    };
    let native = NativeProgram {
        stage: HwStage::Ps,
        wave64: false,
        vgprs: 16,
        layout: vec![table(0, 0), table(1, 2)],
        used: vec![
            range(ResourceType::Srv, 0, 8, 0, 0),
            range(ResourceType::Sampler, 0, 4, 1, 2),
        ],
        io: StageIo::Ps {
            inputs: Vec::new(),
            input_addr: (1 << 8) | (1 << 9), // POS_X_FLOAT (v0), POS_Y_FLOAT (v1)
            db_shader_control: 0,
            patches: Vec::new(),
        },
        code: words.into_iter().flat_map(u32::to_le_bytes).collect(),
    };
    let pos = SigElement {
        name: "SV_Position".into(),
        index: 0,
        kind: 3,
        comp: 9,
        interp: 4,
        row: Some(0),
        col: 0,
        cols: 4,
        usage: 0,
        half: false,
    };
    let target = SigElement {
        name: "SV_Target".into(),
        index: 0,
        kind: 16,
        comp: 9,
        interp: 0,
        row: Some(0),
        col: 0,
        cols: 4,
        usage: 15,
        half: false,
    };
    let isg1 = container::signature(&[pos], true);
    let osg1 = container::signature(&[target], false);
    let bytes = crate::dxbc::build(&[(FourCC::ISG1, &isg1), (FourCC::OSG1, &osg1)]);
    let c = Container::parse(&bytes).expect("Container::parse");
    let p = ir::build(&native.patched_code(), native.wave64, native.vgprs).expect("ir::build");
    let st = structure::structure(&p).expect("structure");
    let res =
        resource::track(&p, &native.layout, native.stage, Some(&st)).expect("resource::track");
    let io = io::raise(&p, &native).expect("io::raise");
    let shader = emit(&c, &native, &p, &st, &io, &res).expect("emit");

    if let Ok(mut v) = validator::Validator::load(validator::Validator::default_path().as_deref()) {
        let mut copy = shader.dxil.clone();
        v.validate(&mut copy).expect("DXIL validation failed");
    }
    shader
}

fn fullscreen_vs() -> HostShader {
    let mut s = Shader::new(false);
    let b = s.entry;
    let (zero, iu) = (s.m.i32(0), s.m.undef(s.t.i32));
    for col in 0..4u8 {
        let c = s.m.i8(col);
        let v = s.op(b, "loadInput.f32", Attr::ReadNone, s.t.f32, 4, &[zero, zero, c, iu]);
        s.op(b, "storeOutput.f32", Attr::NoUnwind, s.t.void, 5, &[zero, zero, c, v]);
    }
    s.inputs = vec![SigElement {
        name: "POSITION".into(),
        index: 0,
        kind: 0,
        comp: 9,
        interp: 0,
        row: Some(0),
        col: 0,
        cols: 4,
        usage: 15,
        half: false,
    }];
    s.outputs = vec![SigElement {
        name: "SV_Position".into(),
        index: 0,
        kind: 3,
        comp: 9,
        interp: 4,
        row: Some(0),
        col: 0,
        cols: 4,
        usage: 15,
        half: false,
    }];
    s.m.terminate(b, Term::Ret);
    HostShader {
        stage: HwStage::Vs,
        dxil: s.finish(),
        user_data: Vec::new(),
        samplers: Vec::new(),
        static_samplers: Vec::new(),
        root_cbvs: Vec::new(),
        root_srvs: Vec::new(),
        root_uavs: Vec::new(),
        root_indexed_views: Vec::new(),
        half_views: Vec::new(),
    }
}

#[test]
fn image_get_lod_writes_zero_to_z_and_w_and_packs_d16() {
    let _one = ONE_AT_A_TIME.lock().unwrap_or_else(|e| e.into_inner());
    let texels = vec![[1.0f32; 4]; 8 * 8];
    let sampler = D3D12_SAMPLER_DESC {
        Filter: D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        AddressU: D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        AddressV: D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        AddressW: D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        MinLOD: 0.5,
        MaxLOD: 4.0,
        MaxAnisotropy: 1,
        ..Default::default()
    };
    let vertices = [-1.0f32, 3.0, 0.5, 1.0, 3.0, -1.0, 0.5, 1.0, -1.0, -1.0, 0.5, 1.0];
    let vs = fullscreen_vs();

    for d16 in [false, true] {
        let ps = build_get_lod_ps(d16);
        let mut ran = 0;
        for (backend, warp) in [("hardware", false), ("warp", true)] {
            let Ok(host) = Host::new(warp) else {
                continue;
            };
            let (root, pso) = host.pipeline(&vs, &ps).expect("pipeline");
            let out = host
                .draw(
                    &root,
                    &pso,
                    &Draw {
                        roots: vec![(
                            0,
                            Root::Constants(vec![0; ps.user_data_dwords().max(1) as usize]),
                        )],
                        textures: &[(8, 8, &texels)],
                        samplers: &[sampler],
                        vertices: &vertices,
                        stride: 16,
                        size: (4, 4),
                        mesh_groups: 0,
                    },
                )
                .expect("draw");
            // UV = SV_Position.xy -> ddx = 1, ddy = 1 -> 8x8 texel footprint = 8 -> unclamped LOD = 3.0;
            // clamped LOD clamps to [MinLOD=0.5, MaxLOD=4.0] (or resource mip 0 then MinLOD=0.5); Z = 0.0, W = 0.0.
            let px = out[0];
            assert!(
                (px[1] - 3.0).abs() < 1e-3,
                "{backend} d16={d16}: expected unclamped Y=3.0, got {px:?}"
            );
            assert!(
                px[0] >= 0.0 && px[0] <= 3.0,
                "{backend} d16={d16}: unexpected clamped X in {px:?}"
            );
            assert_eq!(
                [px[2].to_bits(), px[3].to_bits()],
                [0, 0],
                "{backend} d16={d16}: expected exact +0.0 in Z/W, got {px:?}"
            );
            ran += 1;
        }
        assert!(ran > 0, "no SM 6.6 device available");
    }
}
