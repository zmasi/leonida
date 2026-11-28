//! Regression tests for 16-bit VALU/VOPC operand decoding (`half_src`),
//! `v_cmp_class_f16` mask decoding, and SDWA sub-dword source/destination
//! selection (`write_half`) on D3D12 hardware, WARP, and `dxil.dll`.

use super::host::{Dispatch, Host, Root, View};
use super::valu_tests::ONE_AT_A_TIME;
use super::{HostShader, lower, validator};
use crate::ShaderError;
use crate::native::{
    ComputeIo, HwStage, NativeProgram, ResourceType, RootParam, RootType, StageIo, UsedResource,
};
use crate::{io, ir, resource, structure};

const S_ENDPGM: u32 = 0xBF81_0000;
const VCC_LO: u8 = 106;

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

const fn vopc(op: u16, src0: u16, vsrc1: u8) -> u32 {
    0x7C00_0000 | ((op as u32 & 0xFF) << 17) | (vsrc1 as u32) << 9 | (src0 as u32 & 0x1FF)
}

const fn vop3_opsel(
    op: u16,
    vdst: u8,
    src0: u16,
    src1: u16,
    src2: u16,
    opsel: u8,
    mods: (u8, u8),
) -> [u32; 2] {
    let (abs, neg) = mods;
    [
        0x35 << 26
            | (op as u32 & 0x3FF) << 16
            | (opsel as u32 & 0xF) << 11
            | (abs as u32 & 7) << 8
            | vdst as u32,
        (neg as u32 & 7) << 29
            | (src0 as u32 & 0x1FF)
            | (src1 as u32 & 0x1FF) << 9
            | (src2 as u32 & 0x1FF) << 18,
    ]
}

/// Encodes `(code, non_vgpr)` for an SDWA operand from a standard 9-bit `src`.
const fn sdwa_op(src: u16) -> (u8, bool) {
    if src >= 256 {
        ((src - 256) as u8, false)
    } else {
        (src as u8, true)
    }
}

/// VOP1 SDWA `op_sdwa vdst, src0 dst_sel:... dst_unused:... src0_sel:...`.
const fn vop1_sdwa(
    op: u16,
    vdst: u8,
    src0: u16,
    dst: (u8, u8),
    src0_sel: u8,
    sext0: bool,
    mods0: (bool, bool),
) -> [u32; 2] {
    let (s0, non_vgpr0) = sdwa_op(src0);
    let (dst_sel, dst_unused) = dst;
    let (abs0, neg0) = mods0;
    [
        vop1(op, vdst, 0xF9),
        (non_vgpr0 as u32) << 23
            | (abs0 as u32) << 21
            | (neg0 as u32) << 20
            | (sext0 as u32) << 19
            | (src0_sel as u32 & 7) << 16
            | (dst_unused as u32 & 3) << 11
            | (dst_sel as u32 & 7) << 8
            | s0 as u32,
    ]
}

/// VOP2 SDWA `op_sdwa vdst, src0, src1 dst_sel:... dst_unused:... src0_sel:... src1_sel:...`.
const fn vop2_sdwa(
    op: u16,
    vdst: u8,
    srcs: (u16, u16),
    dst: (u8, u8),
    src_sel: (u8, u8),
    sext: u8,
    mods: (u8, u8),
) -> [u32; 2] {
    let (s0, non_vgpr0) = sdwa_op(srcs.0);
    let (s1, non_vgpr1) = sdwa_op(srcs.1);
    let (dst_sel, dst_unused) = dst;
    let (sel0, sel1) = src_sel;
    let (abs, neg) = mods;
    [
        vop2(op, vdst, 0xF9, s1),
        (non_vgpr1 as u32) << 31
            | ((abs as u32 >> 1 & 1) << 29)
            | ((neg as u32 >> 1 & 1) << 28)
            | ((sext as u32 >> 1 & 1) << 27)
            | (sel1 as u32 & 7) << 24
            | (non_vgpr0 as u32) << 23
            | ((abs as u32 & 1) << 21)
            | ((neg as u32 & 1) << 20)
            | ((sext as u32 & 1) << 19)
            | (sel0 as u32 & 7) << 16
            | (dst_unused as u32 & 3) << 11
            | (dst_sel as u32 & 7) << 8
            | s0 as u32,
    ]
}

/// VOPC SDWA `op_sdwa vcc_lo, src0, src1 src0_sel:... src1_sel:...`.
const fn vopc_sdwa(
    op: u16,
    srcs: (u16, u16),
    src_sel: (u8, u8),
    sext: u8,
    mods: (u8, u8),
) -> [u32; 2] {
    let (s0, non_vgpr0) = sdwa_op(srcs.0);
    let (s1, non_vgpr1) = sdwa_op(srcs.1);
    let (sel0, sel1) = src_sel;
    let (abs, neg) = mods;
    [
        vopc(op, 0xF9, s1),
        (non_vgpr1 as u32) << 31
            | ((abs as u32 >> 1 & 1) << 29)
            | ((neg as u32 >> 1 & 1) << 28)
            | ((sext as u32 >> 1 & 1) << 27)
            | (sel1 as u32 & 7) << 24
            | (non_vgpr0 as u32) << 23
            | ((abs as u32 & 1) << 21)
            | ((neg as u32 & 1) << 20)
            | ((sext as u32 & 1) << 19)
            | (sel0 as u32 & 7) << 16
            | s0 as u32,
    ]
}

const fn buffer_store_dword(vdata: u8, srsrc: u8, byte_off: u16) -> [u32; 2] {
    [
        0x38 << 26 | 0x1C << 18 | (byte_off as u32 & 0xFFF),
        0x80 << 24 | ((srsrc as u32 >> 2) & 0x1F) << 16 | (vdata as u32) << 8,
    ]
}

const fn sgpr(n: u8) -> u16 {
    n as u16
}

const fn vgpr(n: u8) -> u16 {
    256 + n as u16
}

fn try_build_cs(body: &[u32], out_dwords: u32) -> Result<HostShader, ShaderError> {
    let mut words = vec![s_mov_b32(5, 0x80)];
    words.extend_from_slice(&s_load_dwordx4(8, 4));
    for i in 0..4u8 {
        words.push(vop1(0x001, i + 1, sgpr(i))); // v_mov_b32 v(i+1), s(i)
    }
    words.extend_from_slice(body);
    for d in 0..out_dwords {
        words.extend_from_slice(&buffer_store_dword(d as u8, 8, (4 * d) as u16));
    }
    words.push(S_ENDPGM);

    let code: Vec<u8> = words.iter().flat_map(|w| w.to_le_bytes()).collect();
    let native = NativeProgram {
        stage: HwStage::Cs,
        wave64: false,
        vgprs: 16,
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

fn run_dwords(shader: &HostShader, inputs: [u32; 4], out_dwords: usize, mut check: impl FnMut(&str, &[u32])) {
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
        let mut ud = vec![0u32; shader.user_data_dwords().max(1) as usize];
        for u in &shader.user_data {
            ud[u.index as usize] = match u.root {
                RootType::Constants => inputs[u.dword as usize],
                _ => 0,
            };
        }
        let views = [View::RawUav((4 * out_dwords) as u32)];
        let got = host
            .dispatch(
                &root,
                &pso,
                &Dispatch {
                    roots: vec![(0, Root::Constants(ud))],
                    views: &views,
                    samplers: &[],
                    groups: [1, 1, 1],
                },
            )
            .unwrap_or_else(|e| panic!("{label}: dispatch failed: {e}"));
        check(label, &got[0][..out_dwords]);
        ran += 1;
    }
    assert!(ran > 0, "neither hardware nor WARP SM 6.6 device was available");
}

/// On RDNA2 (measured on `gfx1036`, `docs/design.md` "SDWA select on an f16 inline
/// constant" and `tools/sdwa_diff.py`), 16-bit integer instructions (`v_cvt_f16_{u16,i16}`,
/// `v_add/sub/mul_lo/shifts/min/max_*16`) read an inline float constant (`240..=248`) as its
/// 32-bit `f32` bit pattern split into halves, whereas 16-bit float ops read `0x0000_<f16>`.
#[test]
fn int16_valu_and_cvt_read_inline_float_constants_as_f32_halves() {
    let mut body = Vec::new();
    // v0: seed v1 (0xDEAD_BEEF), then v_cvt_f16_u16 v0, 1.0 (VOP1 0x050, inline 242 = 0x3F80_0000).
    // Low half is 0x0000 -> +0.0 (0x0000), high half 0xDEAD kept -> 0xDEAD_0000.
    body.push(vop1(0x001, 0, vgpr(1)));
    body.push(vop1(0x050, 0, 242));

    // v1: seed v1 (0xDEAD_BEEF), then v_cvt_f16_u16_e64 v1, 1.0 op_sel:[1,0] (VOP3 0x1D0).
    // High half of 0x3F80_0000 is 0x3F80 (16256 -> f16 0x73F0), high half 0xDEAD kept -> 0xDEAD_73F0.
    body.extend(vop3_opsel(0x1D0, 1, 242, 0, 0, 0b0001, (0, 0)));

    // v2: seed v2 (0xCAFE_BEEF), then v_cvt_f16_u16_sdwa v2, 1.0 dst_sel:WORD_0 UNUSED_PRESERVE src0_sel:WORD_1.
    // Exact `ps_sdwa_f16_const` hardware case -> 0xCAFE_73F0.
    body.extend(vop1_sdwa(0x050, 2, 242, (4, 2), 5, false, (false, false)));

    // v3: seed v3 (0x1234_BEEF), then v_cvt_f16_i16_e64 v3, -1.0 op_sel:[1,1] (VOP3 0x1D1).
    // High half of 0xBF80_0000 is 0xBF80 (-16512 -> f16 0xF408) written into high half -> 0xF408_BEEF.
    body.extend(vop3_opsel(0x1D1, 3, 243, 0, 0, 0b1001, (0, 0)));

    // v4: v_add_nc_u16 v4, 0.15915494, v6 (0x303, inline 248 = 0x3E22_F983, v6 = 5) in both halves:
    // low half: 0xF983 + 5 = 0xF988; high half (op_sel:[1,0,1]): 0x3E22 + 5 = 0x3E27 -> 0x3E27_F988.
    body.push(vop1(0x001, 6, vgpr(4)));
    body.extend(vop3_opsel(0x303, 4, 248, vgpr(6), 0, 0b0000, (0, 0)));
    body.extend(vop3_opsel(0x303, 4, 248, vgpr(6), 0, 0b1001, (0, 0)));

    // v5: positive control on f16 float ops: v_add_f16 v5, 1.0, 2.0 (low -> 3.0 = 0x4200),
    // then v_add_f16_e64 v5, 1.0, 2.0 op_sel:[1,0,1] (high half of f16 inline 1.0 is +0.0 -> 0.0 + 2.0 = 0x4000).
    body.push(vop1(0x001, 5, 128)); // 0
    body.extend(vop3_opsel(0x132, 5, 242, 244, 0, 0b0000, (0, 0)));
    body.extend(vop3_opsel(0x132, 5, 242, 244, 0, 0b1001, (0, 0)));

    // v6: v_ashrrev_i16 (0x308) of -2.0 high half (inline 245 = 0xC000_0000 -> hi = 0xC000 = -16384 >> 2 = -4096 = 0xF000)
    // in low half, and v_max_i16 (0x30A) of -2.0 high half (-16384) vs -1.0 high half (-16512) -> -16384 = 0xC000 in high half.
    body.extend(vop3_opsel(0x308, 6, 130, 245, 0, 0b0010, (0, 0)));
    body.extend(vop3_opsel(0x30A, 6, 245, 243, 0, 0b1011, (0, 0)));

    let shader = try_build_cs(&body, 7).expect("shader must lower");
    run_dwords(
        &shader,
        [0xDEAD_BEEF, 0xCAFE_BEEF, 0x1234_BEEF, 0x0000_0005],
        7,
        |backend, got| {
            assert_eq!(got[0], 0xDEAD_0000, "{backend}: v_cvt_f16_u16 1.0 low half");
            assert_eq!(got[1], 0xDEAD_73F0, "{backend}: v_cvt_f16_u16 1.0 op_sel:[1,0]");
            assert_eq!(got[2], 0xCAFE_73F0, "{backend}: v_cvt_f16_u16_sdwa 1.0 WORD_1");
            assert_eq!(got[3], 0xF408_BEEF, "{backend}: v_cvt_f16_i16 -1.0 op_sel:[1,1]");
            assert_eq!(got[4], 0x3E27_F988, "{backend}: v_add_nc_u16 1/(2*pi) halves");
            assert_eq!(got[5], 0x4000_4200, "{backend}: v_add_f16 inline float control");
            assert_eq!(got[6], 0xC000_F000, "{backend}: v_ashrrev_i16 and v_max_i16 inline floats");
        },
    );
}

/// 16-bit integer compares (`v_cmp_*_{u16,i16}`) read inline float constants as
/// 32-bit `f32` halves and support SDWA byte selects with `sext()`.
#[test]
fn int16_compares_read_inline_float_constants_as_f32_halves_and_sdwa_bytes() {
    let push_bit = |body: &mut Vec<u32>, bit_pos: u8, scond: u8| {
        // v6 = v_cndmask_b32 0, (1 << bit_pos), scond; v0 |= v6
        let mask_val = 1u32 << bit_pos;
        body.push(vop1(0x001, 7, 0xFF));
        body.push(mask_val);
        body.extend(vop3_opsel(0x101, 6, 128, vgpr(7), sgpr(scond), 0, (0, 0)));
        body.push(vop2(0x01C, 0, vgpr(6), 0)); // v_or_b32 v0, v6, v0
    };

    let mut body = vec![vop1(0x001, 0, 128)]; // v0 = 0
    // Bit 0: v_cmp_eq_u16 vcc_lo, 1.0, v1 (v1 = 0x0000 -> true = 1; old bug compared 0x3C00 -> 0)
    body.push(vopc(0x0AA, 242, 1));
    push_bit(&mut body, 0, VCC_LO);

    // Bit 1: v_cmp_eq_u16 vcc_lo, 1.0, v2 (v2 = 0x3C00 -> false = 0; old bug returned 1)
    body.push(vopc(0x0AA, 242, 2));
    push_bit(&mut body, 1, VCC_LO);

    // Bit 2: v_cmp_eq_u16_e64 s6, 1.0, v3 op_sel:[1,0] (v3 = 0x3F80 -> true = 1)
    body.extend(vop3_opsel(0x0AA, 6, 242, vgpr(3), 0, 0b0001, (0, 0)));
    push_bit(&mut body, 2, 6);

    // Bit 3: v_cmp_lt_i16_e64 s6, -1.0, v1 op_sel:[1,0] (-1.0 hi = 0xBF80 = -16512 < 0 -> true = 1)
    body.extend(vop3_opsel(0x089, 6, 243, vgpr(1), 0, 0b0001, (0, 0)));
    push_bit(&mut body, 3, 6);

    // Bit 4: v_cmp_lt_i16_sdwa vcc_lo, sext(v4), v1 src0_sel:BYTE_2 src1_sel:WORD_0
    // v4 = 0x0080_7FFF -> BYTE_2 sext = 0xFF80 = -128 < 0 -> true = 1.
    body.extend(vopc_sdwa(0x089, (vgpr(4), vgpr(1)), (2, 4), 1, (0, 0)));
    push_bit(&mut body, 4, VCC_LO);

    // Bit 5: same without sext -> BYTE_2 zero-extended = +128 < 0 -> false = 0.
    body.extend(vopc_sdwa(0x089, (vgpr(4), vgpr(1)), (2, 4), 0, (0, 0)));
    push_bit(&mut body, 5, VCC_LO);

    let shader = try_build_cs(&body, 1).expect("shader must lower");
    run_dwords(
        &shader,
        [0x0000_0000, 0x0000_3C00, 0x0000_3F80, 0x0080_7FFF],
        1,
        |backend, got| {
            assert_eq!(got[0], 0b01_1101, "{backend}: 16-bit integer compare bitmask");
        },
    );
}

/// `v_cmp_class_f16` decodes inline float constants in `src1` (the class mask)
/// as `f16` (`0x0000_<f16>`) before SDWA narrowing (`tools/sdwa_diff.py`).
#[test]
fn v_cmp_class_f16_reads_inline_float_mask_as_f16_and_narrows_sdwa() {
    let push_bit = |body: &mut Vec<u32>, bit_pos: u8| {
        body.push(vop1(0x001, 7, 0xFF));
        body.push(1u32 << bit_pos);
        body.extend(vop3_opsel(0x101, 6, 128, vgpr(7), sgpr(VCC_LO), 0, (0, 0)));
        body.push(vop2(0x01C, 0, vgpr(6), 0));
    };

    let mut body = vec![vop1(0x001, 0, 128)];
    // Inline 248 (`0.15915494`) as f16 is 0x3118 -> low 10 bits = 0x118 (bits 3, 4, 8:
    // -normal, -denormal, +normal). Under f32 bits (`0x3E22_F983`), low 10 bits were 0x183
    // (bits 0, 1, 7, 8: sNaN, qNaN, +denormal, +normal).
    // Bit 0: v1 = 0xBC00 (-1.0, class 3) -> 1
    body.extend(vop3_opsel(0x08F, VCC_LO, vgpr(1), 248, 0, 0, (0, 0)));
    push_bit(&mut body, 0);
    // Bit 1: v2 = 0x7E00 (qNaN, class 1) -> 0
    body.extend(vop3_opsel(0x08F, VCC_LO, vgpr(2), 248, 0, 0, (0, 0)));
    push_bit(&mut body, 1);
    // Bit 2: v3 = 0x0001 (+denorm, class 7) -> 0
    body.extend(vop3_opsel(0x08F, VCC_LO, vgpr(3), 248, 0, 0, (0, 0)));
    push_bit(&mut body, 2);
    // Bit 3: SDWA `v_cmp_class_f16_sdwa vcc_lo, v1, 0.5 src0_sel:WORD_0 src1_sel:BYTE_1`
    // 0.5 as f16 is 0x3800 -> BYTE_1 is 0x38 (bits 3, 4, 5), so -1.0 (class 3) -> 1.
    body.extend(vopc_sdwa(0x08F, (vgpr(1), 240), (4, 1), 0, (0, 0)));
    push_bit(&mut body, 3);
    // Bit 4: SDWA `v_cmp_class_f16_sdwa vcc_lo, v3, sext(v4) src0_sel:WORD_0 src1_sel:BYTE_0`
    // v4 = 0x80 -> sext BYTE_0 = 0xFF80 -> bits 7, 8, 9 set -> +denorm (class 7) -> 1.
    body.extend(vopc_sdwa(0x08F, (vgpr(3), vgpr(4)), (4, 0), 2, (0, 0)));
    push_bit(&mut body, 4);

    let shader = try_build_cs(&body, 1).expect("shader must lower");
    run_dwords(
        &shader,
        [0x0000_BC00, 0x0000_7E00, 0x0000_0001, 0x0000_0080],
        1,
        |backend, got| {
            assert_eq!(got[0], 0b1_1001, "{backend}: v_cmp_class_f16 mask bits");
        },
    );
}

/// SDWA 16-bit ops support `BYTE_0..3` source/destination selects, sign-extend
/// bit 15 under `dst_sel:WORD_0 dst_unused:UNUSED_SEXT`, and always zero the
/// upper half under `dst_sel:DWORD` (`docs/design.md`, `tools/sdwa_diff.py`).
#[test]
fn sdwa_16bit_byte_selects_and_word0_sext_destination_match_hardware() {
    let mut body = Vec::new();
    // v0: seed 0xDEAD_BEEF; `v_add_f16_sdwa v0, 2.0, 0 dst_sel:WORD_0 UNUSED_PRESERVE src0_sel:BYTE_1 src1_sel:WORD_0`
    // 2.0 as f16 is 0x4000 -> BYTE_1 is 0x0040 (`ps_sdwa_f16_const`) -> 0xDEAD_0040.
    body.push(vop1(0x001, 0, vgpr(1)));
    body.extend(vop2_sdwa(0x032, 0, (244, 128), (4, 2), (1, 4), 0, (0, 0)));

    // v1: `v_add_f16_sdwa v1, -1.0, 0 dst_sel:WORD_0 UNUSED_SEXT src0_sel:WORD_0 src1_sel:WORD_0`
    // -1.0 is 0xBC00 (bit 15 = 1) -> WORD_0 with UNUSED_SEXT sign-extends into [31:16] -> 0xFFFF_BC00.
    body.extend(vop2_sdwa(0x032, 1, (243, 128), (4, 1), (4, 4), 0, (0, 0)));

    // v2: seed 0x1122_3344; `v_cvt_u16_f16_sdwa v2, v3 dst_sel:BYTE_1 UNUSED_PRESERVE src0_sel:WORD_0`
    // v3 = 0x0000_5800 (128.0 in f16 -> 0x0080) -> placed in BYTE_1 preserving other bytes -> 0x1122_8044.
    body.extend(vop1_sdwa(0x052, 2, vgpr(3), (1, 2), 4, false, (false, false)));

    // v3: `v_cvt_u16_f16_sdwa v3, v3 dst_sel:BYTE_1 UNUSED_SEXT src0_sel:WORD_0`
    // 0x0080 in BYTE_1 with UNUSED_SEXT -> bits [7:0] = 0, [15:8] = 0x80, [31:16] = 0xFFFF -> 0xFFFF_8000.
    body.extend(vop1_sdwa(0x052, 3, vgpr(3), (1, 1), 4, false, (false, false)));

    // v4: seed 0xDEAD_BEEF; `v_add_f16_sdwa v4, -1.0, 0 dst_sel:DWORD UNUSED_SEXT`
    // On GFX10, dst_sel:DWORD on a 16-bit result always zeroes the high half -> 0x0000_BC00.
    body.push(vop1(0x001, 4, vgpr(1)));
    body.extend(vop2_sdwa(0x032, 4, (243, 128), (6, 1), (6, 6), 0, (0, 0)));

    let shader = try_build_cs(&body, 5).expect("shader must lower");
    run_dwords(
        &shader,
        [0xDEAD_BEEF, 0x1122_3344, 0x0000_5800, 0],
        5,
        |backend, got| {
            assert_eq!(got[0], 0xDEAD_0040, "{backend}: 2.0 under BYTE_1");
            assert_eq!(got[1], 0xFFFF_BC00, "{backend}: WORD_0 UNUSED_SEXT sign-extends bit 15");
            assert_eq!(got[2], 0x1122_8044, "{backend}: BYTE_1 UNUSED_PRESERVE");
            assert_eq!(got[3], 0xFFFF_8000, "{backend}: BYTE_1 UNUSED_SEXT");
            assert_eq!(got[4], 0x0000_BC00, "{backend}: DWORD zeroes high half regardless of UNUSED_SEXT");
        },
    );
}

/// Sweeps all 21 `(dst_sel 0..=6) x (dst_unused 0..=2)` 16-bit placements across
/// negative (`0xBC85`) and positive (`0x3C75`) results plus all 14 `(src_sel 0..=6) x (sext 0..=1)`
/// 16-bit integer source selections against `tools/sdwa_diff.py`'s reference rules.
#[test]
fn sdwa_16bit_all_placements_and_selects_match_sdwa_diff_reference() {
    fn ref_place16(old: u32, res16: u16, sel: u8, unused: u8) -> u32 {
        let res = u32::from(res16);
        if sel == 6 {
            return res;
        }
        let (off, width) = if sel < 4 {
            (8 * u32::from(sel), 8u32)
        } else {
            (16 * u32::from(sel - 4), 16)
        };
        let mask = (1u32 << width) - 1;
        let field = res & mask;
        match unused {
            2 => (old & !(mask << off)) | (field << off),
            1 => {
                let sx = if (field >> (width - 1)) & 1 != 0 {
                    field | !mask
                } else {
                    field
                };
                sx << off
            }
            _ => field << off,
        }
    }

    fn ref_select_i16(word: u32, sel: u8, sext: bool) -> i16 {
        let (off, width) = if sel < 4 {
            (8 * u32::from(sel), 8u32)
        } else if sel < 6 {
            (16 * u32::from(sel - 4), 16)
        } else {
            (0, 32)
        };
        let mask = if width == 32 { u32::MAX } else { (1u32 << width) - 1 };
        let mut v = (word >> off) & mask;
        if sext && width < 32 && (v >> (width - 1)) & 1 != 0 {
            v |= !mask;
        }
        (v & 0xFFFF) as u16 as i16
    }

    fn f32_to_f16_exact(f: f32) -> u16 {
        let b = f.to_bits();
        if b & 0x7FFF_FFFF == 0 {
            return ((b >> 16) & 0x8000) as u16;
        }
        let sign = ((b >> 16) & 0x8000) as u16;
        let exp = ((b >> 23) & 0xFF) as i32 - 127 + 15;
        let mant = ((b >> 13) & 0x3FF) as u16;
        sign | ((exp as u16) << 10) | mant
    }

    let mut body = Vec::new();
    let mut slot = 0u16;
    // 42 destination placements: v2 = 0xBC85, v3 = 0x3C75, old v1 = 0xA55A_5AA5.
    for src_reg in [2u8, 3u8] {
        for dst_sel in 0..=6u8 {
            for dst_unused in 0..=2u8 {
                body.push(vop1(0x001, 5, vgpr(1))); // v5 = old
                body.extend(vop2_sdwa(
                    0x032,
                    5,
                    (vgpr(src_reg), 128),
                    (dst_sel, dst_unused),
                    (4, 4),
                    0,
                    (0, 0),
                ));
                body.extend(buffer_store_dword(5, 8, 4 * slot));
                slot += 1;
            }
        }
    }
    // 14 integer source selections (`v_cvt_f16_i16_sdwa`, VOP1 0x051) on v4 = 0xC080_FE80.
    for sext in [false, true] {
        for src_sel in 0..=6u8 {
            body.push(vop1(0x001, 5, vgpr(1)));
            body.extend(vop1_sdwa(0x051, 5, vgpr(4), (6, 0), src_sel, sext, (false, false)));
            body.extend(buffer_store_dword(5, 8, 4 * slot));
            slot += 1;
        }
    }

    let total = usize::from(slot);
    let shader = try_build_cs(&body, 0).expect("grid shader must lower");
    let old = 0xA55A_5AA5u32;
    let src_word = 0xC080_FE80u32;
    run_dwords(&shader, [old, 0x0000_BC85, 0x0000_3C75, src_word], total, |backend, got| {
        let mut idx = 0;
        for res16 in [0xBC85u16, 0x3C75u16] {
            for dst_sel in 0..=6u8 {
                for dst_unused in 0..=2u8 {
                    let want = ref_place16(old, res16, dst_sel, dst_unused);
                    assert_eq!(
                        got[idx], want,
                        "{backend}: place res={res16:#06x} dst_sel={dst_sel} unused={dst_unused}"
                    );
                    idx += 1;
                }
            }
        }
        for sext in [false, true] {
            for src_sel in 0..=6u8 {
                let s = ref_select_i16(src_word, src_sel, sext);
                let want = u32::from(f32_to_f16_exact(f32::from(s)));
                assert_eq!(
                    got[idx], want,
                    "{backend}: select src_sel={src_sel} sext={sext} -> {s}"
                );
                idx += 1;
            }
        }
    });
}

#[test]
fn rejected_16bit_modifier_combinations_stay_unsupported() {
    let use_vcc = vop3_opsel(0x101, 0, 128, 129, sgpr(VCC_LO), 0, (0, 0));
    let cmp = |w: [u32; 2]| vec![w[0], w[1], use_vcc[0], use_vcc[1]];
    let cases: &[(&str, Vec<u32>)] = &[
        (
            "neg on v_cvt_f16_u16",
            vop3_opsel(0x1D0, 0, vgpr(1), 0, 0, 0, (0, 1)).to_vec(),
        ),
        (
            "abs on v_cmp_eq_u16",
            cmp(vop3_opsel(0x0AA, VCC_LO, vgpr(1), vgpr(2), 0, 0, (1, 0))),
        ),
        (
            "neg on v_cmp_class_f16 mask",
            cmp(vop3_opsel(0x08F, VCC_LO, vgpr(1), vgpr(2), 0, 0, (0, 2))),
        ),
        (
            "op_sel[1] on v_cmp_class_f16",
            cmp(vop3_opsel(0x08F, VCC_LO, vgpr(1), vgpr(2), 0, 0b0010, (0, 0))),
        ),
        (
            "clamp on v_cmp_class_f16",
            cmp([
                vop3_opsel(0x08F, VCC_LO, vgpr(1), vgpr(2), 0, 0, (0, 0))[0] | (1 << 15),
                vop3_opsel(0x08F, VCC_LO, vgpr(1), vgpr(2), 0, 0, (0, 0))[1],
            ]),
        ),
        (
            "sext on v_add_f16_sdwa",
            vop2_sdwa(0x032, 0, (vgpr(1), vgpr(2)), (4, 2), (0, 4), 1, (0, 0)).to_vec(),
        ),
        (
            "sext on v_cmp_class_f16_sdwa src0",
            cmp(vopc_sdwa(0x08F, (vgpr(1), vgpr(2)), (0, 4), 1, (0, 0))),
        ),
    ];

    for (label, body) in cases {
        let err = try_build_cs(body, 1).err();
        assert!(
            matches!(err, Some(ShaderError::Unsupported { .. })),
            "{label}: expected Unsupported, got {err:?}"
        );
    }
}
