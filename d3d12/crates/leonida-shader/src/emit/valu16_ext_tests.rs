//! End-to-end D3D12 hardware + WARP + `dxil.dll` regression tests for the
//! 16-bit integer and half-float VALU instructions in `valu16_ext.rs`:
//! - `v_add_nc_i16` (`0x30d`), `v_sub_nc_i16` (`0x30e`)
//! - `v_mad_u16` (`0x340`), `v_mad_i16` (`0x35e`)
//! - `v_fmamk_f16` (`0x037`), `v_fmaak_f16` (`0x038`)
//! - `v_frexp_mant_f16` (`0x059` / `0x1d9`), `v_frexp_exp_i16_f16` (`0x05a` / `0x1da`),
//!   `v_ldexp_f16` (`0x03b` / `0x13b`)

use super::host::{Dispatch, Host, Root, View};
use super::valu_tests::ONE_AT_A_TIME;
use super::{HostShader, lower, validator};
use crate::ShaderError;
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

const fn vop2(op: u16, vdst: u8, src0: u16, vsrc1: u8) -> u32 {
    ((op as u32 & 0x3F) << 25)
        | (vdst as u32) << 17
        | (vsrc1 as u32) << 9
        | (src0 as u32 & 0x1FF)
}

const fn vop2_lit(op: u16, vdst: u8, src0: u16, vsrc1: u8, lit: u32) -> [u32; 2] {
    [vop2(op, vdst, src0, vsrc1), lit]
}

const fn sgpr(n: u8) -> u16 {
    n as u16
}

const fn vgpr(n: u8) -> u16 {
    256 + n as u16
}

#[allow(clippy::too_many_arguments)]
const fn vop3(
    op: u16,
    vdst: u8,
    src0: u16,
    src1: u16,
    src2: u16,
    opsel: u8,
    abs: u8,
    neg: u8,
    omod: u8,
    clamp: bool,
) -> [u32; 2] {
    [
        0x35 << 26
            | (op as u32 & 0x3FF) << 16
            | (clamp as u32) << 15
            | (opsel as u32 & 0xF) << 11
            | (abs as u32 & 7) << 8
            | vdst as u32,
        (neg as u32 & 7) << 29
            | (omod as u32 & 3) << 27
            | (src0 as u32 & 0x1FF)
            | (src1 as u32 & 0x1FF) << 9
            | (src2 as u32 & 0x1FF) << 18,
    ]
}

const fn buffer_store_dword(vdata: u8, srsrc: u8, byte_off: u16) -> [u32; 2] {
    [
        0x38 << 26 | 0x1C << 18 | (byte_off as u32 & 0xFFF),
        0x80 << 24 | ((srsrc as u32 >> 2) & 0x1F) << 16 | (vdata as u32) << 8,
    ]
}

fn try_build_cs(body: &[u32]) -> Result<HostShader, ShaderError> {
    let mut words = Vec::new();
    words.push(s_mov_b32(5, 0x80));
    words.extend_from_slice(&s_load_dwordx4(8, 4));
    for i in 0..4u8 {
        words.push(vop1(0x001, i + 1, sgpr(i))); // v1..v4 = s0..s3
    }
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
            threads: [1, 1, 1],
        }),
        code,
    };

    let p = ir::build(&native.code, native.wave64, native.vgprs)?;
    let st = structure::structure(&p)?;
    let res = resource::track(&p, &native.layout, native.stage, Some(&st))?;
    let io = io::raise(&p, &native)?;
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
    })?;

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

    Ok(shader)
}

fn build_cs(body: &[u32]) -> HostShader {
    try_build_cs(body).expect("build_cs failed")
}

fn run_cases(
    shader: &HostShader,
    cases: &[[u32; 4]],
    mut check: impl FnMut(&str, usize, [u32; 4], u32),
) {
    let _one = ONE_AT_A_TIME.lock().unwrap_or_else(|e| e.into_inner());
    let mut ran = 0;
    for (label, warp) in [("hardware", false), ("warp", true)] {
        let host = match Host::new(warp) {
            Ok(host) => host,
            Err(e) => {
                eprintln!("no SM 6.6 {label} device ({e}); skipped");
                continue;
            }
        };
        let (root, pso) = host
            .compute_pipeline(shader)
            .unwrap_or_else(|e| panic!("{label}: compute_pipeline failed: {e}"));
        for (i, &inputs) in cases.iter().enumerate() {
            let mut ud = vec![0u32; shader.user_data_dwords().max(1) as usize];
            for u in &shader.user_data {
                ud[u.index as usize] = match (u.root, u.slot) {
                    (RootType::Constants, 0) => inputs[u.dword as usize],
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
            check(label, i, inputs, out[0][0]);
        }
        ran += 1;
    }
    assert!(ran > 0, "no SM 6.6 device available");
}

#[test]
fn vop3_add_sub_nc_i16_wrap_saturate_and_preserve_unwritten_half() {
    let seed = vop1(0x001, 0, vgpr(4)); // v0 = s3 sentinel
    let add_wrap = build_cs(&[seed, vop3(0x30d, 0, vgpr(1), vgpr(2), 0, 0, 0, 0, 0, false)[0], vop3(0x30d, 0, vgpr(1), vgpr(2), 0, 0, 0, 0, 0, false)[1]]);
    let add_clamp = build_cs(&[seed, vop3(0x30d, 0, vgpr(1), vgpr(2), 0, 0, 0, 0, 0, true)[0], vop3(0x30d, 0, vgpr(1), vgpr(2), 0, 0, 0, 0, 0, true)[1]]);
    let sub_wrap = build_cs(&[seed, vop3(0x30e, 0, vgpr(1), vgpr(2), 0, 0, 0, 0, 0, false)[0], vop3(0x30e, 0, vgpr(1), vgpr(2), 0, 0, 0, 0, 0, false)[1]]);
    // op_sel = 0b1010: src0.lo, src1.hi -> dst.hi (low half of v0 preserved!)
    let sub_clamp_hi = build_cs(&[seed, vop3(0x30e, 0, vgpr(1), vgpr(2), 0, 0b1010, 0, 0, 0, true)[0], vop3(0x30e, 0, vgpr(1), vgpr(2), 0, 0b1010, 0, 0, 0, true)[1]]);
    // Inline float 1.0 (242 = 0x3F80_0000 on 32-bit integer bus): op_sel[0]=1 reads 0x3F80!
    let add_fconst_hi = build_cs(&[seed, vop3(0x30d, 0, 242, vgpr(1), 0, 0b0001, 0, 0, 0, false)[0], vop3(0x30d, 0, 242, vgpr(1), 0, 0b0001, 0, 0, 0, false)[1]]);

    run_cases(
        &add_wrap,
        &[
            [0x7FFF, 1, 0, 0xDEAD_1111],      // 32767 + 1 wraps to 0x8000
            [0x8000, 0xFFFF, 0, 0xDEAD_1111], // -32768 + -1 wraps to 0x7FFF
        ],
        |backend, i, _, got| {
            let want = [0xDEAD_8000, 0xDEAD_7FFF][i];
            assert_eq!(got, want, "{backend}: add_wrap case {i}: got {got:#010x}, want {want:#010x}");
        },
    );
    run_cases(
        &add_clamp,
        &[
            [0x7FFF, 1, 0, 0xDEAD_1111],      // 32767 + 1 saturates to 0x7FFF
            [0x8000, 0xFFFF, 0, 0xDEAD_1111], // -32768 + -1 saturates to 0x8000 (upper 0xDEAD preserved!)
            [0x8000, 0, 0, 0xDEAD_1111],      // -32768 + 0 exact boundary 0x8000
            [0xFF9C, 25, 0, 0xDEAD_1111],     // -100 + 25 = -75 (0xFFB5)
        ],
        |backend, i, _, got| {
            let want = [0xDEAD_7FFF, 0xDEAD_8000, 0xDEAD_8000, 0xDEAD_FFB5][i];
            assert_eq!(got, want, "{backend}: add_clamp case {i}: got {got:#010x}, want {want:#010x}");
        },
    );
    run_cases(
        &sub_wrap,
        &[[0x8000, 1, 0, 0xBEEF_2222]], // -32768 - 1 wraps to 0x7FFF
        |backend, _, _, got| assert_eq!(got, 0xBEEF_7FFF, "{backend}: sub_wrap"),
    );
    run_cases(
        &sub_clamp_hi,
        &[
            [0x8000, 0x0001_9999, 0, 0x1111_BEEF], // -32768 - 1 -> 0x8000 in dst.hi, 0xBEEF kept in dst.lo
            [0x7FFF, 0xFFFF_9999, 0, 0x1111_BEEF], // 32767 - (-1) -> 0x7FFF in dst.hi
        ],
        |backend, i, _, got| {
            let want = [0x8000_BEEF, 0x7FFF_BEEF][i];
            assert_eq!(got, want, "{backend}: sub_clamp_hi case {i}: got {got:#010x}, want {want:#010x}");
        },
    );
    run_cases(
        &add_fconst_hi,
        &[[2, 0, 0, 0xCAFE_0000]], // 0x3F80 + 2 = 0x3F82
        |backend, _, _, got| assert_eq!(got, 0xCAFE_3F82, "{backend}: add_fconst_hi"),
    );

    // Invalid modifiers on integer 16-bit ops must be rejected.
    assert!(try_build_cs(&vop3(0x30d, 0, vgpr(1), vgpr(2), 0, 0, 1, 0, 0, false)).is_err());
    assert!(try_build_cs(&vop3(0x30e, 0, vgpr(1), vgpr(2), 0, 0, 0, 1, 0, false)).is_err());
    assert!(try_build_cs(&vop3(0x30d, 0, vgpr(1), vgpr(2), 0, 0, 0, 0, 1, false)).is_err());
}

#[test]
fn vop3_mad_u16_and_mad_i16_wrap_saturate_and_honor_opsel() {
    let seed = vop1(0x001, 0, vgpr(4));
    let mad_u16 = build_cs(&[seed, vop3(0x340, 0, vgpr(1), vgpr(2), vgpr(3), 0, 0, 0, 0, false)[0], vop3(0x340, 0, vgpr(1), vgpr(2), vgpr(3), 0, 0, 0, 0, false)[1]]);
    let mad_u16_clamp = build_cs(&[seed, vop3(0x340, 0, vgpr(1), vgpr(2), vgpr(3), 0, 0, 0, 0, true)[0], vop3(0x340, 0, vgpr(1), vgpr(2), vgpr(3), 0, 0, 0, 0, true)[1]]);
    let mad_i16 = build_cs(&[seed, vop3(0x35e, 0, vgpr(1), vgpr(2), vgpr(3), 0, 0, 0, 0, false)[0], vop3(0x35e, 0, vgpr(1), vgpr(2), vgpr(3), 0, 0, 0, 0, false)[1]]);
    let mad_i16_clamp_lo = build_cs(&[seed, vop3(0x35e, 0, vgpr(1), vgpr(2), vgpr(3), 0, 0, 0, 0, true)[0], vop3(0x35e, 0, vgpr(1), vgpr(2), vgpr(3), 0, 0, 0, 0, true)[1]]);
    // op_sel = 0b1101: src0.hi * src1.lo + src2.hi -> dst.hi, clamp = true
    let mad_i16_clamp_hi = build_cs(&[seed, vop3(0x35e, 0, vgpr(1), vgpr(2), vgpr(3), 0b1101, 0, 0, 0, true)[0], vop3(0x35e, 0, vgpr(1), vgpr(2), vgpr(3), 0b1101, 0, 0, 0, true)[1]]);

    run_cases(
        &mad_u16,
        &[
            [0xFFFF, 0xFFFF, 0xFFFF, 0xABCD_1234], // 65535*65535 + 65535 = 0xFFFF_0000 -> low 16 = 0x0000
            [300, 300, 500, 0xABCD_1234],          // 90500 = 0x1_6184 -> 0x6184
        ],
        |backend, i, _, got| {
            let want = [0xABCD_0000, 0xABCD_6184][i];
            assert_eq!(got, want, "{backend}: mad_u16 case {i}: got {got:#010x}, want {want:#010x}");
        },
    );
    run_cases(
        &mad_u16_clamp,
        &[
            [0xFFFF, 0xFFFF, 0xFFFF, 0xABCD_1234], // saturates to 0xFFFF
            [200, 300, 500, 0xABCD_1234],          // 60500 = 0xEC54 (in range)
        ],
        |backend, i, _, got| {
            let want = [0xABCD_FFFF, 0xABCD_EC54][i];
            assert_eq!(got, want, "{backend}: mad_u16_clamp case {i}: got {got:#010x}, want {want:#010x}");
        },
    );
    run_cases(
        &mad_i16,
        &[
            [0x8000, 0x8000, 0x7FFF, 0xABCD_1234], // (-32768)*(-32768) + 32767 = 0x4000_7FFF -> 0x7FFF
            [0xFF38, 200, 100, 0xABCD_1234],       // -200*200 + 100 = -39900 = 0xFFFF_6424 -> 0x6424
        ],
        |backend, i, _, got| {
            let want = [0xABCD_7FFF, 0xABCD_6424][i];
            assert_eq!(got, want, "{backend}: mad_i16 case {i}: got {got:#010x}, want {want:#010x}");
        },
    );
    run_cases(
        &mad_i16_clamp_lo,
        &[
            [0x8000, 0x7FFF, 0x8000, 0xABCD_1234], // (-32768)*32767 + (-32768) -> 0x8000 in dst.lo (0xABCD preserved!)
            [0xFFFB, 15, 0, 0xABCD_1234],          // (-5)*15 + 0 = -75 (0xFFB5) in dst.lo (0xABCD preserved!)
        ],
        |backend, i, _, got| {
            let want = [0xABCD_8000, 0xABCD_FFB5][i];
            assert_eq!(got, want, "{backend}: mad_i16_clamp_lo case {i}: got {got:#010x}, want {want:#010x}");
        },
    );
    run_cases(
        &mad_i16_clamp_hi,
        &[
            [0x8000_0000, 0x8000, 0x0000_9999, 0x9999_4321], // (-32768)*(-32768) + 0 -> 0x7FFF in dst.hi
            [0x8000_0000, 0x7FFF, 0x8000_9999, 0x9999_4321], // (-32768)*32767 + (-32768) -> 0x8000 in dst.hi
            [0xFF9C_0000, 0xFF9C, 0xF830_9999, 0x9999_4321], // (-100)*(-100) + (-2000) = 8000 (0x1F40) in dst.hi
        ],
        |backend, i, _, got| {
            let want = [0x7FFF_4321, 0x8000_4321, 0x1F40_4321][i];
            assert_eq!(got, want, "{backend}: mad_i16_clamp_hi case {i}: got {got:#010x}, want {want:#010x}");
        },
    );

    assert!(try_build_cs(&vop3(0x340, 0, vgpr(1), vgpr(2), vgpr(3), 0, 1, 0, 0, false)).is_err());
    assert!(try_build_cs(&vop3(0x35e, 0, vgpr(1), vgpr(2), vgpr(3), 0, 0, 0, 2, false)).is_err());
}

#[test]
fn vop2_fmamk_f16_and_fmaak_f16_apply_literal_k_and_preserve_high_half() {
    let seed = vop1(0x001, 0, vgpr(4));
    // v_fmamk_f16 v0, v1, K=1.5h(0x3E00, high junk 0x9999 ignored), v2: v1 * 1.5 + v2
    let [mk0, mk1] = vop2_lit(0x037, 0, vgpr(1), 2, 0x9999_3E00);
    let fmamk = build_cs(&[seed, mk0, mk1]);
    // v_fmaak_f16 v0, 241(-0.5h inline const), v1, K=3.25h(0x4280): (-0.5) * v1 + 3.25
    let [ak0, ak1] = vop2_lit(0x038, 0, 241, 1, 0x7777_4280);
    let fmaak = build_cs(&[seed, ak0, ak1]);

    run_cases(
        &fmamk,
        &[
            [0x4000, 0x3400, 0, 0xCAFE_1111], // 2.0 * 1.5 + 0.25 = 3.25h (0x4280)
            [0xBC00, 0x3800, 0, 0xCAFE_1111], // -1.0 * 1.5 + 0.5 = -1.0h (0xBC00)
        ],
        |backend, i, _, got| {
            let want = [0xCAFE_4280, 0xCAFE_BC00][i];
            assert_eq!(got, want, "{backend}: fmamk_f16 case {i}: got {got:#010x}, want {want:#010x}");
        },
    );
    run_cases(
        &fmaak,
        &[
            [0x4400, 0, 0, 0xF00D_2222], // (-0.5) * 4.0 + 3.25 = 1.25h (0x3D00)
            [0x0000, 0, 0, 0xF00D_2222], // (-0.5) * 0.0 + 3.25 = 3.25h (0x4280)
        ],
        |backend, i, _, got| {
            let want = [0xF00D_3D00, 0xF00D_4280][i];
            assert_eq!(got, want, "{backend}: fmaak_f16 case {i}: got {got:#010x}, want {want:#010x}");
        },
    );

    // VOP3 encodings of fmamk_f16 / fmaak_f16 (`0x137`, `0x138`) have no literal K and must fail.
    assert!(try_build_cs(&vop3(0x137, 0, vgpr(1), vgpr(2), 0, 0, 0, 0, 0, false)).is_err());
    assert!(try_build_cs(&vop3(0x138, 0, vgpr(1), vgpr(2), 0, 0, 0, 0, 0, false)).is_err());
}

#[test]
fn frexp_mant_exp_and_ldexp_f16_match_rdna2_across_subnormals_nans_and_mods() {
    let seed = vop1(0x001, 0, vgpr(4));
    // VOP1 forms
    let mant_e32 = build_cs(&[seed, vop1(0x059, 0, vgpr(1))]);
    let exp_e32 = build_cs(&[seed, vop1(0x05a, 0, vgpr(1))]);
    // VOP2 form of v_ldexp_f16
    let ldexp_e32 = build_cs(&[seed, vop2(0x03b, 0, vgpr(1), 2)]);

    run_cases(
        &mant_e32,
        &[
            [0x3C00, 0, 0, 0xAAAA_5555], // 1.0 -> 0.5h (0x3800)
            [0xC800, 0, 0, 0xAAAA_5555], // -8.0 -> -0.5h (0xB800)
            [0x0001, 0, 0, 0xAAAA_5555], // min subnormal 2^-24 -> 0.5h (0x3800)
            [0x83FF, 0, 0, 0xAAAA_5555], // -max subnormal -> -0x3BFE = 0xBBFE
            [0x8000, 0, 0, 0xAAAA_5555], // -0.0 -> -0.0 (0x8000)
            [0x7C00, 0, 0, 0xAAAA_5555], // +inf -> +inf (0x7C00)
            [0x7C01, 0, 0, 0xAAAA_5555], // sNaN -> unquieted 0x7C01 under IEEE=0
            [0xFE55, 0, 0, 0xAAAA_5555], // -qNaN payload -> 0xFE55
        ],
        |backend, i, _, got| {
            let want = [
                0xAAAA_3800,
                0xAAAA_B800,
                0xAAAA_3800,
                0xAAAA_BBFE,
                0xAAAA_8000,
                0xAAAA_7C00,
                0xAAAA_7C01,
                0xAAAA_FE55,
            ][i];
            assert_eq!(got, want, "{backend}: mant_e32 case {i}: got {got:#010x}, want {want:#010x}");
        },
    );

    run_cases(
        &exp_e32,
        &[
            [0x3C00, 0, 0, 0xBBBB_5555], // 1.0 = 0.5 * 2^1 -> 1
            [0xC800, 0, 0, 0xBBBB_5555], // -8.0 = -0.5 * 2^4 -> 4
            [0x0001, 0, 0, 0xBBBB_5555], // 2^-24 = 0.5 * 2^-23 -> -23 (0xFFE9)
            [0x83FF, 0, 0, 0xBBBB_5555], // -max subnormal -> -14 (0xFFF2)
            [0x8000, 0, 0, 0xBBBB_5555], // -0.0 -> 0
            [0xFC00, 0, 0, 0xBBBB_5555], // -inf -> 0
            [0x7C01, 0, 0, 0xBBBB_5555], // sNaN -> 0
        ],
        |backend, i, _, got| {
            let want = [
                0xBBBB_0001,
                0xBBBB_0004,
                0xBBBB_FFE9,
                0xBBBB_FFF2,
                0xBBBB_0000,
                0xBBBB_0000,
                0xBBBB_0000,
            ][i];
            assert_eq!(got, want, "{backend}: exp_e32 case {i}: got {got:#010x}, want {want:#010x}");
        },
    );

    run_cases(
        &ldexp_e32,
        &[
            [0x3800, 1, 0, 0xCCCC_5555],      // 0.5 * 2^1 = 1.0h (0x3C00)
            [0x0001, 14, 0, 0xCCCC_5555],     // 2^-24 * 2^14 = 2^-10 = 0x1400
            [0x0401, 0xFFFF, 0, 0xCCCC_5555], // (0x401 * 2^-24) * 2^-1 -> odd/even RNE tie -> 0x0200
            [0x0403, 0xFFFF, 0, 0xCCCC_5555], // (0x403 * 2^-24) * 2^-1 -> odd tie rounds up -> 0x0202
            [0x07FF, 0xFFFF, 0, 0xCCCC_5555], // carry into bit 10 -> smallest normal 0x0400
            [0x3C00, 0x7FFF, 0, 0xCCCC_5555], // +32767 clamped to +64 -> +inf (0x7C00)
            [0xBC00, 0x8000, 0, 0xCCCC_5555], // -32768 clamped to -64 -> -0.0h (0x8000)
            [0x7C01, 5, 0, 0xCCCC_5555],      // sNaN passes through unquieted
        ],
        |backend, i, _, got| {
            let want = [
                0xCCCC_3C00,
                0xCCCC_1400,
                0xCCCC_0200,
                0xCCCC_0202,
                0xCCCC_0400,
                0xCCCC_7C00,
                0xCCCC_8000,
                0xCCCC_7C01,
            ][i];
            assert_eq!(got, want, "{backend}: ldexp_e32 case {i}: got {got:#010x}, want {want:#010x}");
        },
    );

    // VOP3 modifiers under console MODE (`denorm_mode_16_64 = 3`):
    // - `v_frexp_mant_f16` & `v_ldexp_f16`: `omod` ignored, `clamp` saturates [0, 1] (`-0`/neg/`NaN` -> +0, >1 -> 1.0h)
    // - `v_frexp_exp_i16_f16`: both `clamp` and `omod` are hardware no-ops!
    let mant_mods = build_cs(&[
        seed,
        vop3(0x1d9, 0, vgpr(1), 0, 0, 0b1001, 0, 1, 2, true)[0], // -src0.hi, mul:4 ignored, clamp -> dst.hi
        vop3(0x1d9, 0, vgpr(1), 0, 0, 0b1001, 0, 1, 2, true)[1],
    ]);
    let exp_mods = build_cs(&[
        seed,
        vop3(0x1da, 0, vgpr(1), 0, 0, 0, 1, 0, 1, true)[0], // |src0|, mul:2 + clamp ignored on i16 result
        vop3(0x1da, 0, vgpr(1), 0, 0, 0, 1, 0, 1, true)[1],
    ]);
    let ldexp_mods = build_cs(&[
        seed,
        vop3(0x13b, 0, vgpr(1), vgpr(2), 0, 0, 1, 0, 1, true)[0], // |src0| * 2^src1, mul:2 ignored, clamp
        vop3(0x13b, 0, vgpr(1), vgpr(2), 0, 0, 1, 0, 1, true)[1],
    ]);

    run_cases(
        &mant_mods,
        &[
            [0xBC00_0000, 0, 0, 0x1111_5678], // -(-1.0h) = +1.0h -> mant 0.5h (0x3800, mul:4 ignored) in dst.hi
            [0x3C00_0000, 0, 0, 0x1111_5678], // -(1.0h) = -1.0h -> mant -0.5h clamped to +0.0h in dst.hi
            [0xFE55_0000, 0, 0, 0x1111_5678], // NaN clamped to +0.0h in dst.hi
        ],
        |backend, i, _, got| {
            let want = [0x3800_5678, 0x0000_5678, 0x0000_5678][i];
            assert_eq!(got, want, "{backend}: mant_mods case {i}: got {got:#010x}, want {want:#010x}");
        },
    );
    run_cases(
        &exp_mods,
        &[
            [0xC800, 0, 0, 0xDDDD_0000], // |-8.0h| -> exp 4 (NOT clamped to 1, NOT mul:2 to 8!)
            [0x0001, 0, 0, 0xDDDD_0000], // 2^-24 -> exp -23 (0xFFE9, NOT clamped to 0!)
        ],
        |backend, i, _, got| {
            let want = [0xDDDD_0004, 0xDDDD_FFE9][i];
            assert_eq!(got, want, "{backend}: exp_mods case {i}: got {got:#010x}, want {want:#010x}");
        },
    );
    run_cases(
        &ldexp_mods,
        &[
            [0xB800, 1, 0, 0xEEEE_0000],  // |-0.5h| * 2^1 = 1.0h (0x3C00, mul:2 ignored)
            [0xBC00, 3, 0, 0xEEEE_0000],  // |-1.0h| * 2^3 = 8.0h -> clamped to 1.0h (0x3C00)
            [0xFC00, 0, 0, 0xEEEE_0000],  // |-inf| = +inf -> clamped to 1.0h (0x3C00)
        ],
        |backend, i, _, got| {
            let want = [0xEEEE_3C00, 0xEEEE_3C00, 0xEEEE_3C00][i];
            assert_eq!(got, want, "{backend}: ldexp_mods case {i}: got {got:#010x}, want {want:#010x}");
        },
    );

    // abs/neg on S1 (integer exponent) of v_ldexp_f16 must be rejected.
    assert!(try_build_cs(&vop3(0x13b, 0, vgpr(1), vgpr(2), 0, 0, 0b010, 0, 0, false)).is_err());
    assert!(try_build_cs(&vop3(0x13b, 0, vgpr(1), vgpr(2), 0, 0, 0, 0b010, 0, false)).is_err());
}
