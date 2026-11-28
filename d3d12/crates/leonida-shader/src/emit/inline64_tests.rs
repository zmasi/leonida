//! End-to-end D3D12 hardware and WARP tests for GFX10 inline constants in
//! 64-bit operand positions: the float codes 240..=248 stand for their f64
//! bit patterns there (LLVM's AMDGPU disassembler, `getInlineImmVal64` for
//! `OPERAND_REG_IMM_INT64` / `OPERAND_REG_INLINE_C_INT64` operands), integer
//! and bitwise instructions included; 32-bit operands of the same
//! instructions (shift amounts) keep the f32 bits.

use super::host::{Dispatch, Host, Root, View};
use super::valu_tests::ONE_AT_A_TIME;
use super::{HostShader, lower, validator};
use crate::native::{
    ComputeIo, HwStage, NativeProgram, ResourceType, RootParam, RootType, StageIo, UsedResource,
};
use crate::{io, ir, resource, structure};

const S_ENDPGM: u32 = 0xBF81_0000;

/// The 64-bit value of each float inline constant, independent of the
/// translator: LLVM `AMDGPUDisassembler.cpp` `getInlineImmVal64` and
/// `AMDGPUBaseInfo.cpp` `isInlinableLiteral64` (1/(2*pi) is `...C882`).
const F64_CODES: [(u8, u64); 9] = [
    (240, 0x3FE0_0000_0000_0000),
    (241, 0xBFE0_0000_0000_0000),
    (242, 0x3FF0_0000_0000_0000),
    (243, 0xBFF0_0000_0000_0000),
    (244, 0x4000_0000_0000_0000),
    (245, 0xC000_0000_0000_0000),
    (246, 0x4010_0000_0000_0000),
    (247, 0xC010_0000_0000_0000),
    (248, 0x3FC4_5F30_6DC9_C882),
];

/// Integer inline constants in a 64-bit operand: sign-extended.
const INT_CODES: [(u8, u64); 4] = [
    (128, 0),
    (192, 64),
    (193, u64::MAX),
    (208, (-16i64) as u64),
];

const fn sop1(op: u8, sdst: u8, src0: u8) -> u32 {
    0xBE80_0000 | (sdst as u32) << 16 | (op as u32) << 8 | src0 as u32
}

const fn sop2(op: u8, sdst: u8, src0: u8, src1: u8) -> u32 {
    0x8000_0000 | (op as u32 & 0x7F) << 23 | (sdst as u32 & 0x7F) << 16 | (src1 as u32) << 8 | src0 as u32
}

const fn sopc(op: u8, src0: u8, src1: u8) -> u32 {
    0xBF00_0000 | (op as u32 & 0x7F) << 16 | (src1 as u32) << 8 | src0 as u32
}

const fn vop1_mov(vdst: u8, src0: u16) -> u32 {
    0x7E00_0000 | (vdst as u32) << 17 | 0x01 << 9 | (src0 as u32 & 0x1FF)
}

const fn vop3(op: u16, vdst: u8, sdst: u8, src: [u16; 3]) -> [u32; 2] {
    [
        0xD400_0000 | (op as u32 & 0x3FF) << 16 | (sdst as u32) << 8 | vdst as u32,
        (src[0] as u32 & 0x1FF) | (src[1] as u32 & 0x1FF) << 9 | (src[2] as u32 & 0x1FF) << 18,
    ]
}

const fn s_load_dwordx4(dst: u8, base: u8) -> [u32; 2] {
    [
        0x3D << 26 | 0x02 << 18 | (dst as u32) << 6 | (base as u32 >> 1),
        125 << 25,
    ]
}

const fn buffer_store_dwordx2(vdata: u8, srsrc: u8) -> [u32; 2] {
    [
        0x38 << 26 | 0x1D << 18,
        0x80 << 24 | ((srsrc as u32 >> 2) & 0x1F) << 16 | (vdata as u32) << 8,
    ]
}

/// SALU tail: v0 = s6, v1 = s7.
const SALU_OUT: [u32; 2] = [vop1_mov(0, 6), vop1_mov(1, 7)];

/// A compute shader taking root constants `s0, s1`, running `body` (which
/// leaves its result in `v[0:1]`) and storing `v[0:1]`.
fn build_cs(body: &[u32]) -> HostShader {
    let mut words = vec![sop1(0x03, 5, 0x80)]; // s_mov_b32 s5, 0
    words.extend_from_slice(&s_load_dwordx4(8, 4));
    words.extend_from_slice(body);
    words.extend_from_slice(&buffer_store_dwordx2(0, 8));
    words.push(S_ENDPGM);

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
        code: words.iter().flat_map(|w| w.to_le_bytes()).collect(),
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

/// Runs `shader` for each `(s0, s1)` on the hardware device and WARP and
/// checks the stored pair against `expect`.
fn run(shader: &HostShader, label: &str, inputs: &[(u32, u32)], expect: impl Fn(u32, u32) -> u64) {
    let _one = ONE_AT_A_TIME.lock().unwrap_or_else(|e| e.into_inner());
    let mut ran = 0;
    for (backend, warp) in [("hardware", false), ("warp", true)] {
        let Ok(host) = Host::new(warp) else {
            continue;
        };
        let (root, pso) = host
            .compute_pipeline(shader)
            .unwrap_or_else(|e| panic!("{backend} {label}: compute_pipeline failed: {e}"));
        for &(a, b) in inputs {
            let mut ud = vec![0u32; shader.user_data_dwords().max(1) as usize];
            for u in &shader.user_data {
                ud[u.index as usize] = match (u.root, u.slot, u.dword) {
                    (RootType::Constants, 0, 0) => a,
                    (RootType::Constants, 0, 1) => b,
                    (RootType::Constants, 0, _) | (RootType::Table, 1, _) => 0,
                    other => panic!("unexpected user data {other:?}"),
                };
            }
            let out = host
                .dispatch(
                    &root,
                    &pso,
                    &Dispatch {
                        roots: vec![(0, Root::Constants(ud))],
                        views: &[View::RawUav(2)],
                        samplers: &[],
                        groups: [1, 1, 1],
                    },
                )
                .unwrap_or_else(|e| panic!("{backend} {label}: dispatch failed: {e}"));
            let got = u64::from(out[0][0]) | u64::from(out[0][1]) << 32;
            let want = expect(a, b);
            assert_eq!(
                got, want,
                "{backend} {label} s0={a:#010x} s1={b:#010x}: got {got:#018x}, expected {want:#018x}"
            );
        }
        ran += 1;
    }
    assert!(ran > 0, "no SM 6.6 device available");
}

fn split(x: u64) -> (u32, u32) {
    (x as u32, (x >> 32) as u32)
}

#[test]
fn s_mov_b64_and_bitwise_ops_read_inline_floats_as_f64_bits() {
    for (code, bits) in F64_CODES.into_iter().chain(INT_CODES) {
        // s_mov_b64 s[6:7], <code>
        let mut body = vec![sop1(0x04, 6, code)];
        body.extend_from_slice(&SALU_OUT);
        run(&build_cs(&body), &format!("s_mov_b64 {code}"), &[(0, 0)], |_, _| bits);
    }
    for (code, bits) in [F64_CODES[2], F64_CODES[5], F64_CODES[8], INT_CODES[2]] {
        // s_xor_b64 s[6:7], s[0:1], <code>
        let mut body = vec![sop2(0x13, 6, 0, code)];
        body.extend_from_slice(&SALU_OUT);
        let inputs = [(0, 0), (0x1234_5678, 0x9ABC_DEF0), (u32::MAX, u32::MAX)];
        run(&build_cs(&body), &format!("s_xor_b64 {code}"), &inputs, |a, b| {
            (u64::from(a) | u64::from(b) << 32) ^ bits
        });
    }
    for (code, bits) in [F64_CODES[0], F64_CODES[8]] {
        // s_cmp_eq_u32 s2, 0 (SCC = 1); s_cselect_b64 s[6:7], <code>, s[0:1]
        let mut body = vec![sopc(0x06, 2, 128), sop2(0x0b, 6, code, 0)];
        body.extend_from_slice(&SALU_OUT);
        run(&build_cs(&body), &format!("s_cselect_b64 {code}"), &[(7, 9)], |_, _| bits);
    }
}

#[test]
fn s_lshl_b64_and_s_cmp_u64_read_inline_floats_as_f64_bits() {
    for (code, bits) in [F64_CODES[4], F64_CODES[8], F64_CODES[1]] {
        // s_lshl_b64 s[6:7], <code>, s0 (the 32-bit shift amount from s0)
        let mut body = vec![sop2(0x1f, 6, code, 0)];
        body.extend_from_slice(&SALU_OUT);
        let inputs = [(0, 0), (1, 0), (4, 0), (33, 0), (64 + 3, 0)];
        run(&build_cs(&body), &format!("s_lshl_b64 {code}"), &inputs, |a, _| bits << (a & 63));
    }
    for (code, bits) in F64_CODES {
        // s_cmp_eq_u64 s[0:1], <code>; s_mov_b32 s6, 0; s_cselect_b32 s7, 1, 0
        let mut body = vec![sopc(0x12, 0, code), sop1(0x03, 6, 128), sop2(0x0a, 7, 129, 128)];
        body.extend_from_slice(&SALU_OUT);
        let (lo, hi) = split(bits);
        let f32_bits = crate::resource::inline(code).expect("an inline float");
        let inputs = [(lo, hi), (f32_bits, 0), (f32_bits, if code % 2 == 1 { u32::MAX } else { 0 })];
        run(&build_cs(&body), &format!("s_cmp_eq_u64 {code}"), &inputs, |a, b| {
            u64::from((u64::from(a) | u64::from(b) << 32) == bits) << 32
        });
    }
}

#[test]
fn valu_64_bit_integer_sources_read_inline_floats_as_f64_bits() {
    for (code, bits) in [F64_CODES[2], F64_CODES[7], F64_CODES[8], INT_CODES[2]] {
        // v_lshlrev_b64 v[0:1], s0, <code>
        let body = vop3(0x2ff, 0, 0, [0, code.into(), 0]);
        let inputs = [(0, 0), (1, 0), (12, 0), (40, 0)];
        run(&build_cs(&body), &format!("v_lshlrev_b64 {code}"), &inputs, |a, _| bits << (a & 63));
    }
    for (code, bits) in [F64_CODES[0], F64_CODES[8], INT_CODES[2]] {
        // v_mad_u64_u32 v[0:1], null, s0, s1, <code>
        let body = vop3(0x176, 0, 125, [0, 1, code.into()]);
        let inputs = [(0, 0), (3, 5), (0xFFFF_FFFF, 0xFFFF_FFFF)];
        run(&build_cs(&body), &format!("v_mad_u64_u32 {code}"), &inputs, |a, b| {
            (u64::from(a) * u64::from(b)).wrapping_add(bits)
        });
    }
    for (code, bits) in [F64_CODES[3], F64_CODES[8]] {
        // v_mov_b32 v2, s0; v_mov_b32 v3, s1; v_mov_b32 v4, 1; v_mov_b32 v0, 0
        // v_cmp_eq_u64 vcc, <code>, v[2:3]; v_cndmask_b32 v1, 0, v4, vcc
        let body = [
            vop1_mov(2, 0),
            vop1_mov(3, 1),
            vop1_mov(4, 129),
            vop1_mov(0, 128),
            0x7C00_0000 | 0xe2 << 17 | 2 << 9 | u32::from(code),
            0x0200_0000 | 1 << 17 | 4 << 9 | 128,
        ];
        let (lo, hi) = split(bits);
        let f32_bits = crate::resource::inline(code).expect("an inline float");
        let inputs = [(lo, hi), (f32_bits, 0), (lo, hi ^ 1)];
        run(&build_cs(&body), &format!("v_cmp_eq_u64 {code}"), &inputs, |a, b| {
            u64::from((u64::from(a) | u64::from(b) << 32) == bits) << 32
        });
    }
}

/// Integer inline constants in an f64 operand are the raw 64-bit pattern,
/// sign-extended (LLVM's `decodeIntImmed`): 1..=64 are f64 subnormals (an f32
/// zero), -1..=-16 NaNs whose top mantissa bits are all set (the f32 NaN
/// `0xFFFFFFFF`).
#[test]
fn v_cvt_f32_f64_reads_integer_inline_constants_as_raw_f64_bits() {
    let mut cases: Vec<(u8, u8, u8, u32)> = Vec::new(); // (code, abs, neg, f32 bits)
    for code in [128u8, 129, 130, 150, 192] {
        cases.push((code, 0, 0, 0));
    }
    for code in [193u8, 194, 200, 208] {
        cases.push((code, 0, 0, 0xFFFF_FFFF));
    }
    cases.push((129, 0, 1, 0x8000_0000)); // -(subnormal): -0.0
    cases.push((193, 1, 0, 0x7FFF_FFFF)); // |NaN|
    cases.push((193, 1, 1, 0xFFFF_FFFF)); // -|NaN|
    for (code, f32_bits) in [(240u8, 0x3F00_0000u32), (243, 0xBF80_0000), (248, 0x3E22_F983)] {
        cases.push((code, 0, 0, f32_bits));
    }
    for (code, abs, neg, want) in cases {
        // v_cvt_f32_f64 v0, <code> (VOP3, abs/neg on src0); v_mov_b32 v1, 0
        let [w0, w1] = vop3(0x18f, 0, 0, [code.into(), 0, 0]);
        let body = [w0 | u32::from(abs) << 8, w1 | u32::from(neg) << 29, vop1_mov(1, 128)];
        let label = format!("v_cvt_f32_f64 {code} abs={abs} neg={neg}");
        run(&build_cs(&body), &label, &[(0, 0)], |_, _| u64::from(want));
    }
}
