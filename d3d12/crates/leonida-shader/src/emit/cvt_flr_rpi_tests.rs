//! End-to-end GPU and WARP tests for RDNA2 `v_cvt_flr_i32_f32` and `v_cvt_rpi_i32_f32`.

use super::host::{Dispatch, Host, Root, View};
use super::valu_tests::ONE_AT_A_TIME;
use super::{HostShader, lower, validator};
use crate::native::{
    ComputeIo, HwStage, NativeProgram, ResourceType, RootParam, RootType, StageIo, UsedResource,
};
use crate::{io, ir, resource, structure};

const S_ENDPGM: u32 = 0xBF81_0000;

const fn s_mov_b32(sdst: u8, src0: u8) -> u32 {
    0xBE80_0000 | (sdst as u32) << 16 | 0x03 << 8 | src0 as u32
}

const fn s_load_dwordx4(dst: u8, base: u8) -> [u32; 2] {
    [
        0x3D << 26 | 0x02 << 18 | (dst as u32) << 6 | (base as u32 >> 1),
        125 << 25,
    ]
}

const fn vop1(op: u16, vdst: u8, src0: u16) -> u32 {
    0x7E00_0000 | (vdst as u32) << 17 | (op as u32) << 9 | (src0 as u32 & 0x1FF)
}

const fn vop3_1(op: u16, vdst: u8, src0: u16, abs: u8, neg: u8) -> [u32; 2] {
    [
        0x35 << 26 | (op as u32 & 0x3FF) << 16 | (abs as u32 & 7) << 8 | vdst as u32,
        (neg as u32 & 7) << 29 | (src0 as u32 & 0x1FF),
    ]
}

const fn vop1_sdwa(
    op: u16,
    vdst: u8,
    vsrc0: u8,
    dst: (u8, u8),
    src0_sel: u8,
    mods: (bool, bool),
) -> [u32; 2] {
    let (dst_sel, dst_unused) = dst;
    let (abs, neg) = mods;
    [
        vop1(op, vdst, 0xF9),
        (vsrc0 as u32)
            | (dst_sel as u32 & 7) << 8
            | (dst_unused as u32 & 3) << 11
            | (src0_sel as u32 & 7) << 16
            | (neg as u32) << 20
            | (abs as u32) << 21,
    ]
}

const fn buffer_store_dword(vdata: u8, srsrc: u8, byte_off: u16) -> [u32; 2] {
    [
        0x38 << 26 | 0x1C << 18 | (byte_off as u32 & 0xFFF),
        0x80 << 24 | ((srsrc as u32 >> 2) & 0x1F) << 16 | (vdata as u32) << 8,
    ]
}

fn build_cvt_cs(body: &[u32]) -> HostShader {
    build_cvt_cs_threads(body, 1)
}

fn build_cvt_cs_threads(body: &[u32], threads_x: u32) -> HostShader {
    let mut words = Vec::new();
    words.push(s_mov_b32(5, 0x80));
    words.extend_from_slice(&s_load_dwordx4(8, 4));
    words.push(vop1(0x001, 1, 0)); // v_mov_b32 v1, s0
    words.extend_from_slice(body);
    words.extend_from_slice(&buffer_store_dword(0, 8, 0));
    words.push(S_ENDPGM);

    let code: Vec<u8> = words.iter().flat_map(|w| w.to_le_bytes()).collect();
    let native = NativeProgram {
        stage: HwStage::Cs,
        wave64: false,
        vgprs: 8,
        layout: vec![
            RootParam {
                root: RootType::Constants,
                table_offset_dwords: 0,
                sgprs: 4,
                base_register: 0,
                user_data: 0,
                slot: 0,
            },
            RootParam {
                root: RootType::Table,
                table_offset_dwords: 0,
                sgprs: 1,
                base_register: 0,
                user_data: 4,
                slot: 1,
            },
        ],
        used: vec![UsedResource {
            kind: ResourceType::Uav,
            root: RootType::Table,
            table_offset_dwords: 0,
            lower_bound: 0,
            stride: 0,
            dwords: 4,
            user_data: 4,
            slot: 1,
        }],
        io: StageIo::Cs(ComputeIo {
            rsrc2: 5 << 1,
            threads: [threads_x, 1, 1],
        }),
        code,
    };

    let p = ir::build(&native.code, native.wave64, native.vgprs).expect("ir::build");
    let st = structure::structure(&p).expect("structure");
    let res =
        resource::track(&p, &native.layout, native.stage, Some(&st)).expect("resource::track");
    let io = io::raise(&p, &native).expect("io::raise");
    let lowered = lower::lower(&lower::Parts {
        native: &native,
        p: &p,
        st: &st,
        io: &io,
        res: &res,
        inputs: Vec::new(),
        outputs: Vec::new(),
        prims: Vec::new(),
        mesh_stride: 0,
    })
    .expect("lower");

    let shader = HostShader {
        stage: HwStage::Cs,
        dxil: lowered.dxil,
        user_data: lowered.user_data,
        samplers: lowered.samplers,
        static_samplers: Vec::new(),
        root_cbvs: lowered.root_cbvs,
        root_srvs: lowered.root_srvs,
        root_uavs: lowered.root_uavs,
        root_indexed_views: res.root_indexed_views,
        half_views: lowered.half_views,
    };

    if let Ok(mut v) = validator::Validator::load(validator::Validator::default_path().as_deref()) {
        let mut copy = shader.dxil.clone();
        let ours = copy[4..20].to_vec();
        v.validate(&mut copy).expect("DXIL validation failed");
        assert_eq!(copy[4..20], ours[..], "container hash mismatch");
    }

    shader
}

fn run_words_min_wave(
    shader: &HostShader,
    min_wave: u32,
    words: &[u32],
    mut check: impl FnMut(&str, usize, u32, u32),
) {
    let _one = ONE_AT_A_TIME.lock().unwrap_or_else(|e| e.into_inner());
    let mut ran = 0;
    for (label, warp) in [("hardware", false), ("warp", true)] {
        let Ok(host) = Host::new(warp) else {
            continue;
        };
        if min_wave > 1 {
            let mut options =
                windows::Win32::Graphics::Direct3D12::D3D12_FEATURE_DATA_D3D12_OPTIONS1::default();
            // SAFETY: options is the correctly sized output struct for OPTIONS1.
            unsafe {
                host.device
                    .CheckFeatureSupport(
                        windows::Win32::Graphics::Direct3D12::D3D12_FEATURE_D3D12_OPTIONS1,
                        (&raw mut options).cast(),
                        size_of_val(&options) as u32,
                    )
                    .expect("wave capabilities");
            }
            if options.WaveLaneCountMax < min_wave || options.WaveLaneCountMin > min_wave {
                continue;
            }
        }
        let (root, pso) = host
            .compute_pipeline(shader)
            .unwrap_or_else(|e| panic!("{label}: compute_pipeline failed: {e}"));
        for (i, &w) in words.iter().enumerate() {
            let mut ud = vec![0u32; shader.user_data_dwords().max(1) as usize];
            for u in &shader.user_data {
                ud[u.index as usize] = match (u.root, u.slot) {
                    (RootType::Constants, 0) if u.dword == 0 => w,
                    (RootType::Constants, 0) => 0,
                    (RootType::Table, 1) => 0,
                    other => panic!("unexpected user data {other:?}"),
                };
            }
            let out = host
                .dispatch(
                    &root,
                    &pso,
                    &Dispatch {
                        roots: vec![(0, Root::Constants(ud))],
                        views: &[View::RawUav(1)],
                        samplers: &[],
                        groups: [1, 1, 1],
                    },
                )
                .unwrap_or_else(|e| panic!("{label}: dispatch failed: {e}"));
            check(label, i, w, out[0][0]);
        }
        ran += 1;
    }
    assert!(ran > 0, "no compatible SM 6.6 device available");
}

fn run_words(shader: &HostShader, words: &[u32], check: impl FnMut(&str, usize, u32, u32)) {
    run_words_min_wave(shader, 1, words, check);
}

/// Independent RDNA2 reference (`gfx1035`/`gfx1036` hardware oracle, `tools/cvt_diff.py`):
/// exact `floor(x)` / `floor(x + 0.5)` in 64-bit fixed/integer math, denormals flushed
/// to `0`, and `|x| >= 2^31`, `±inf`, and `±NaN` saturated by the sign bit.
fn cvt_flr_rpi_ref(bits: u32, rpi: bool) -> u32 {
    let neg = bits >> 31 != 0;
    let mag = bits & 0x7FFF_FFFF;
    if mag >= 0x4F00_0000 {
        return if neg { 0x8000_0000 } else { 0x7FFF_FFFF };
    }
    let exp = (mag >> 23) & 0xFF;
    if exp == 0 {
        return 0;
    }
    let mant = i64::from((mag & 0x007F_FFFF) | 0x0080_0000);
    if exp >= 150 {
        let val = mant << (exp - 150);
        let signed = if neg { -val } else { val };
        return (signed as i32) as u32;
    }
    let fbits = 150 - exp;
    if fbits >= 26 {
        return if !rpi && neg { u32::MAX } else { 0 };
    }
    let mut scaled = if neg { -mant } else { mant };
    if rpi {
        scaled += 1i64 << (fbits - 1);
    }
    ((scaled >> fbits) as i32) as u32
}

#[test]
fn v_cvt_flr_and_rpi_i32_f32_match_rdna2() {
    let mut cases = vec![
        0x0000_0000, // +0.0
        0x8000_0000, // -0.0
        0x0000_0001, // +min subnormal (flushed -> 0)
        0x8000_0001, // -min subnormal (flushed -> 0, not -1)
        0x007F_FFFF, // +max subnormal
        0x807F_FFFF, // -max subnormal
        0x0080_0000, // +min normal
        0x8080_0000, // -min normal (flr -> -1, rpi -> 0)
        0xB380_0000, // -2^-24
        0x3EFF_FFFF, // 0.5 - 2^-25 (exact floor(x + 0.5) == 0, binary32 x + 0.5 == 1.0)
        0x3F00_0000, // +0.5 -> rpi = 1
        0xBEFF_FFFF, // -(0.5 - 2^-25) -> rpi = 0
        0xBF00_0000, // -0.5 -> rpi = 0
        0xBF00_0001, // -(0.5 + 2^-24) -> rpi = -1
        0x3F7F_FFFF, // 1.0 - 2^-24
        0xBF80_0000, // -1.0
        0xBF80_0001, // -1.0 - 2^-23
        0x3FC0_0000, // +1.5
        0xBFC0_0000, // -1.5
        0x4020_0000, // +2.5
        0xC020_0000, // -2.5
        0x4AFF_FFFF, // 8388607.5
        0xCAFF_FFFF, // -8388607.5
        0x4B00_0000, // 8388608.0 (2^23, even)
        0x4B00_0001, // 8388609.0 (2^23 + 1, odd: binary32 + 0.5 rounds up to 8388610)
        0x4B7F_FFFF, // 16777215.0 (2^24 - 1, odd: binary32 + 0.5 rounds up to 16777216)
        0xCB00_0000, // -8388608.0
        0xCB00_0001, // -8388609.0 (odd negative: binary32 + 0.5 rounds up to -8388608)
        0xCB7F_FFFF, // -16777215.0
        0x4EFF_FFFF, // 2^31 - 128 (largest representable < 2^31)
        0xCEFF_FFFF, // -(2^31 - 128)
        0x4F00_0000, // +2^31 -> 0x7FFF_FFFF
        0xCF00_0000, // -2^31 -> 0x8000_0000
        0xCF00_0001, // < -2^31 -> 0x8000_0000
        0x7F7F_FFFF, // +FLT_MAX -> 0x7FFF_FFFF
        0xFF7F_FFFF, // -FLT_MAX -> 0x8000_0000
        0x7F80_0000, // +inf -> 0x7FFF_FFFF
        0xFF80_0000, // -inf -> 0x8000_0000
        0x7FC0_0000, // +qNaN -> 0x7FFF_FFFF
        0xFFC0_0000, // -qNaN -> 0x8000_0000
        0x7F80_0001, // +sNaN -> 0x7FFF_FFFF
        0xFF80_0001, // -sNaN -> 0x8000_0000
    ];
    let mut state = 0x2026_1127u32;
    for _ in 0..128 {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        cases.push(state);
    }

    for (name, op, rpi) in [
        ("v_cvt_rpi_i32_f32", 0x00cu16, true),
        ("v_cvt_flr_i32_f32", 0x00du16, false),
    ] {
        let shader = build_cvt_cs(&[vop1(op, 0, 257)]);
        run_words(&shader, &cases, |backend, idx, input, got| {
            let expected = cvt_flr_rpi_ref(input, rpi);
            assert_eq!(
                got, expected,
                "{backend} {name} case {idx} input={input:#010x}: got {got:#010x}, expected {expected:#010x}"
            );
        });
    }
}

#[test]
fn v_cvt_flr_and_rpi_honor_vop3_abs_and_neg_modifiers_including_nan() {
    let inputs = [
        0x4020_0000u32, // +2.5
        0xC020_0000,    // -2.5
        0x3EFF_FFFF,    // +(0.5 - 2^-25)
        0x4B00_0001,    // +8388609.0
        0x7FC0_0000,    // +qNaN
        0xFFC0_0000,    // -qNaN
    ];
    for (op, rpi) in [(0x18cu16, true), (0x18du16, false)] {
        for (abs, neg) in [(0u8, 1u8), (1, 0), (1, 1)] {
            let shader = build_cvt_cs(&vop3_1(op, 0, 257, abs, neg));
            run_words(&shader, &inputs, |backend, idx, raw, got| {
                let mut modified = raw;
                if abs != 0 {
                    modified &= 0x7FFF_FFFF;
                }
                if neg != 0 {
                    modified ^= 0x8000_0000;
                }
                let expected = cvt_flr_rpi_ref(modified, rpi);
                assert_eq!(
                    got, expected,
                    "{backend} op={op:#x} abs={abs} neg={neg} case {idx} raw={raw:#010x} mod={modified:#010x}: got {got:#010x}, expected {expected:#010x}"
                );
            });
        }
    }
}

const fn vop1_dpp(op: u16, vdst: u8, vsrc0: u8, ctrl: u16, abs: bool, neg: bool) -> [u32; 2] {
    [
        vop1(op, vdst, 0xFA),
        (vsrc0 as u32)
            | (ctrl as u32 & 0x1FF) << 8
            | 1 << 19
            | (neg as u32) << 20
            | (abs as u32) << 21
            | 0xFF << 24,
    ]
}

#[test]
fn v_cvt_flr_and_rpi_sdwa_and_dpp_modifiers() {
    let inputs = [
        0x3EFF_FFFFu32, // +(0.5 - 2^-25)
        0x4B00_0001,    // +8388609.0
        0x7FC0_0000,    // +qNaN -> negated -> 0x8000_0000
        0xFFC0_0000,    // -qNaN -> negated -> 0x7FFF_FFFF
    ];
    for (op, rpi) in [(0x00cu16, true), (0x00du16, false)] {
        let mut sdwa_body = vec![vop1(0x001, 0, 128 + 42)]; // v_mov_b32 v0, 42
        sdwa_body.extend_from_slice(&vop1_sdwa(op, 0, 1, (5, 2), 6, (false, true))); // dst_sel:WORD_1 UNUSED_PRESERVE, -v1
        let sdwa_shader = build_cvt_cs(&sdwa_body);
        run_words(&sdwa_shader, &inputs, |backend, idx, raw, got| {
            let full = cvt_flr_rpi_ref(raw ^ 0x8000_0000, rpi);
            let expected = (full & 0xFFFF) << 16 | 42;
            assert_eq!(
                got, expected,
                "{backend} sdwa op={op:#x} case {idx} raw={raw:#010x}: got {got:#010x}, expected {expected:#010x}"
            );
        });

        let dpp_shader = build_cvt_cs_threads(&vop1_dpp(op, 0, 1, 0x00, false, true), 32); // quad_perm:[0,0,0,0] -v1
        run_words_min_wave(&dpp_shader, 32, &inputs, |backend, idx, raw, got| {
            let expected = cvt_flr_rpi_ref(raw ^ 0x8000_0000, rpi);
            assert_eq!(
                got, expected,
                "{backend} dpp op={op:#x} case {idx} raw={raw:#010x}: got {got:#010x}, expected {expected:#010x}"
            );
        });
    }
}
