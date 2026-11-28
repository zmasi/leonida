//! End-to-end D3D12 hardware + WARP + `dxil.dll` regression tests for RDNA2
//! VOP3 and SDWA `omod` (`mul:2`, `mul:4`, `div:2`) and `clamp` on `f16`
//! destinations across console `MODE` (`denorm_mode_16_64 = 3`, `IEEE = 0`,
//! `DX10_CLAMP = 1`) and `FSPX` `COMPUTE_PGM_RSRC1` (`FLOAT_MODE` / `IEEE`).
//!
//! Expected outputs match the native RDNA2 (`gfx1036`) hardware measurements
//! recorded in `docs/design.md` (*`v_fmac_f16`, `v_interp_p2/mov_f32`, and f16
//! output modifiers*), `docs/shader_validation.md` (`SHADER-135`),
//! `tools/f16misc_diff.py`, and `tests/semantic/test_f16_omod_rdna2.py`:
//! - Under console default `FLOAT_MODE = 0xC0` (`denorm_mode_16_64 = 3`,
//!   `IEEE = 0`, including `FSSM` stages without `PGM_RSRC1` and all 29 shipped
//!   `FSPX` shaders in `docs/fspx.md`), or whenever `IEEE = 1`, `f16`-destination
//!   instructions ignore `omod` while keeping `[0, 1]` `clamp`, whereas
//!   `f32`-destination instructions (`v_cvt_f32_f16`, `v_add_f32`) still scale
//!   with `omod` under `denorm_mode_32 = 0`.
//! - When `COMPUTE_PGM_RSRC1` sets `denorm_mode_16_64 = 0` (`FLOAT_MODE[7:6] = 0`)
//!   and `IEEE = 0` (bit 23 = 0), `f16` `omod` (`mul:2`, `mul:4`, `div:2`) scales
//!   the result before `[0, 1]` `clamp`.
//!
//! Notes:
//! - SDWA `f16` `omod` follows the VOP3 gate by design (`SHADER-135` measured
//!   VOP3 `f16` encodings; `tools/sdwa_diff.py` does not generate `omod`).
//! - `v_cvt_f16_f32` follows the same `PGM_RSRC1` gate (whereas bare-assembly
//!   `tools/rdna2spirv.py`, which has no container `PGM_RSRC1`, unconditionally
//!   scales `v_cvt_f16_f32` via `f16_omod` while dropping `omod` on the other
//!   `f16` classes).

use super::host::{Dispatch, Host, Root, View};
use super::valu_tests::ONE_AT_A_TIME;
use super::{FourCC, HostShader, lower, validator};
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

const fn v_mov_lit(vdst: u8, lit: u32) -> [u32; 2] {
    [vop1(0x001, vdst, 0xFF), lit]
}

const fn vop2(op: u16, vdst: u8, src0: u16, vsrc1: u8) -> u32 {
    ((op as u32 & 0x3F) << 25)
        | (vdst as u32) << 17
        | (vsrc1 as u32) << 9
        | (src0 as u32 & 0x1FF)
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
    omod: u8,
    clamp: bool,
) -> [u32; 2] {
    [
        0x35 << 26
            | (op as u32 & 0x3FF) << 16
            | (clamp as u32) << 15
            | (opsel as u32 & 0xF) << 11
            | vdst as u32,
        (omod as u32 & 3) << 27
            | (src0 as u32 & 0x1FF)
            | (src1 as u32 & 0x1FF) << 9
            | (src2 as u32 & 0x1FF) << 18,
    ]
}

/// VOP2 SDWA (`src0 = 0xF9`) reading WORD_0 (`sel = 4`) of `vsrc0`, `vsrc1`
/// and writing WORD_0 (`dst_sel = 4`, `UNUSED_PRESERVE = 2`) with `omod`/`clamp`.
const fn vop2_sdwa_word0(
    op: u16,
    vdst: u8,
    vsrc0: u8,
    vsrc1: u8,
    omod: u8,
    clamp: bool,
) -> [u32; 2] {
    [
        vop2(op, vdst, 0xF9, vsrc1),
        (4u32 << 24)
            | (4u32 << 16)
            | ((omod as u32 & 3) << 14)
            | ((clamp as u32) << 13)
            | (2u32 << 11)
            | (4u32 << 8)
            | vsrc0 as u32,
    ]
}

const fn buffer_store_dword(vdata: u8, srsrc: u8, byte_off: u16) -> [u32; 2] {
    [
        0x38 << 26 | 0x1C << 18 | (byte_off as u32 & 0xFFF),
        0x80 << 24 | ((srsrc as u32 >> 2) & 0x1F) << 16 | (vdata as u32) << 8,
    ]
}

fn validate_dxil(shader: &HostShader) {
    if let Ok(mut v) = validator::Validator::load(validator::Validator::default_path().as_deref()) {
        let mut copy = shader.dxil.clone();
        let ours = copy[4..20].to_vec();
        v.validate(&mut copy).expect("DXIL validation failed");
        assert_eq!(copy[4..20], ours[..], "container hash mismatch");
    }
}

fn build_cs(body: &[u32]) -> HostShader {
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
    validate_dxil(&shader);
    shader
}

/// Builds a synthetic `DXBC` container carrying an `ACCC` (`FSPX`) compute
/// payload with the given `COMPUTE_PGM_RSRC1` (`rsrc1`), translates it through
/// the public `crate::translate` pipeline, and validates the resulting DXIL.
fn build_fspx_cs(rsrc1: u32, body: &[u32]) -> HostShader {
    let mut words = Vec::new();
    words.push(s_mov_b32(1, 0x80)); // s1 = 0 (table base s0 in user_data 0)
    words.extend_from_slice(&s_load_dwordx4(4, 0)); // s[4:7] = UAV V# (u0, slot 0)
    words.extend_from_slice(body);
    words.push(S_ENDPGM);

    let mut packet = [0u8; 0x1c];
    packet[0x0c..0x12].copy_from_slice(&[1, 0, 1, 0, 1, 0]); // threads = [1, 1, 1]
    packet[0x12..0x14].copy_from_slice(&(1u16 << 4).to_le_bytes()); // wave32
    let rsrc1_with_vgprs = (rsrc1 & !0x3F) | 1; // 2 granules * 8 = 16 VGPRs
    packet[0x14..0x18].copy_from_slice(&rsrc1_with_vgprs.to_le_bytes());
    packet[0x18..0x1c].copy_from_slice(&(1u32 << 1).to_le_bytes()); // 1 user SGPR

    let mut uav_rec = [0u8; 32];
    uav_rec[0..4].copy_from_slice(&0x41u32.to_le_bytes());
    uav_rec[4] = 3; // ResourceType::Uav
    uav_rec[5] = 3; // RootType::Table
    uav_rec[16..20].copy_from_slice(&1u32.to_le_bytes()); // buffer view
    uav_rec[20] = 4; // 4 descriptor dwords
    uav_rec[21] = 0; // user_data = 0
    uav_rec[22] = 0; // root slot = 0
    uav_rec[24..28].copy_from_slice(&u32::MAX.to_le_bytes());

    let mut end_rec = [0u8; 32];
    end_rec[0..4].copy_from_slice(&0x43u32.to_le_bytes());

    let mut meta = vec![0u8; 0x18];
    meta[0..4].copy_from_slice(&0x403u32.to_le_bytes());
    meta[8..12].copy_from_slice(&(words.len() as u32).to_le_bytes());
    meta[20] = 0x40;
    meta[22] = 1;
    meta.extend_from_slice(&uav_rec);
    meta.extend_from_slice(&end_rec);
    let meta_len_minus_14 = (meta.len() - 0x14) as u32;
    meta[4..8].copy_from_slice(&meta_len_minus_14.to_le_bytes());

    let dir_end = 0x10 + 16 * 3;
    let code_off = dir_end + packet.len();
    let code_len = words.len() * 4;
    let meta_off = code_off + code_len;
    let total = meta_off + meta.len();

    let mut fspx = Vec::with_capacity(total);
    fspx.extend_from_slice(b"FSPX");
    fspx.extend_from_slice(&1u32.to_le_bytes());
    fspx.extend_from_slice(&3u32.to_le_bytes());
    fspx.extend_from_slice(&(total as u32).to_le_bytes());
    for (kind, off, len) in [
        (0u32, dir_end, packet.len()),
        (6u32, code_off, code_len),
        (7u32, meta_off, meta.len()),
    ] {
        fspx.extend_from_slice(&kind.to_le_bytes());
        fspx.extend_from_slice(&(off as u32).to_le_bytes());
        fspx.extend_from_slice(&(len as u32).to_le_bytes());
        fspx.extend_from_slice(&0u32.to_le_bytes());
    }
    fspx.extend_from_slice(&packet);
    for w in &words {
        fspx.extend_from_slice(&w.to_le_bytes());
    }
    fspx.extend_from_slice(&meta);

    let dxbc = crate::dxbc::build(&[(FourCC::ACCC, &fspx)]);
    let shader = super::translate(&dxbc).expect("translate FSPX container");
    validate_dxil(&shader);
    shader
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

fn run_fspx_dwords(shader: &HostShader, count: usize, mut check: impl FnMut(&str, &[u32])) {
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
        let ud = vec![0u32; shader.user_data_dwords().max(1) as usize];
        let out = host
            .dispatch(
                &root,
                &pso,
                &Dispatch {
                    roots: vec![(0, Root::Constants(ud))],
                    views: &[View::RawUav(count as u32)],
                    samplers: &[],
                    groups: [1, 1, 1],
                },
            )
            .unwrap_or_else(|e| panic!("{label}: dispatch failed: {e}"));
        check(label, &out[0][..count]);
        ran += 1;
    }
    assert!(ran > 0, "no SM 6.6 device available");
}

#[test]
fn f16_binary_and_ternary_alu_ignore_omod_and_keep_clamp() {
    // Mirrors Class 1 of `tests/semantic/test_f16_omod_rdna2.py` (`gfx1036`):
    // 0.25h=0x3400, 0.5h=0x3800, 0.75h=0x3A00, 1.0h=0x3C00, 1.5h=0x3E00, 2.0h=0x4000, -0.5h=0xB800
    let seed = vop1(0x001, 0, vgpr(4)); // v0 = s3 (0xAAAA_0000 high-half sentinel)
    let checks: &[(&str, [u32; 2], [u32; 4], u32)] = &[
        // v_add_f16 v0, v1(0.5), v2(0.25) mul:2 -> 0.75h (0x3A00, mul:2 ignored), high half 0xAAAA kept
        ("v_add_f16 mul:2", vop3(0x132, 0, vgpr(1), vgpr(2), 0, 0, 1, false), [0x3800, 0x3400, 0, 0xAAAA_5555], 0xAAAA_3A00),
        // v_sub_f16 v0, v1(1.0), v2(0.25) mul:4 -> 0.75h (0x3A00, mul:4 ignored)
        ("v_sub_f16 mul:4", vop3(0x133, 0, vgpr(1), vgpr(2), 0, 0, 2, false), [0x3C00, 0x3400, 0, 0xAAAA_5555], 0xAAAA_3A00),
        // v_mul_f16 v0, v1(1.5), v2(0.5) div:2 -> 0.75h (0x3A00, div:2 ignored)
        ("v_mul_f16 div:2", vop3(0x135, 0, vgpr(1), vgpr(2), 0, 0, 3, false), [0x3E00, 0x3800, 0, 0xAAAA_5555], 0xAAAA_3A00),
        // v_fma_f16 v0, v1(1.0), v2(0.5), v3(0.25) mul:2 -> 0.75h (0x3A00, mul:2 ignored)
        ("v_fma_f16 mul:2", vop3(0x34b, 0, vgpr(1), vgpr(2), vgpr(3), 0, 1, false), [0x3C00, 0x3800, 0x3400, 0xAAAA_5555], 0xAAAA_3A00),
        // v_fmac_f16 v0, v1(0.75), v2(1.0) mul:4 (acc v0.lo = -0.0h = 0x8000) -> 0.75h (0x3A00, SHADER-135)
        ("v_fmac_f16 mul:4", vop3(0x136, 0, vgpr(1), vgpr(2), 0, 0, 2, false), [0x3A00, 0x3C00, 0, 0xAAAA_8000], 0xAAAA_3A00),
        // v_min3_f16 / v_max3_f16 / v_med3_f16 with mul:2 / div:2 / mul:4
        ("v_min3_f16 mul:2", vop3(0x351, 0, vgpr(1), vgpr(2), vgpr(3), 0, 1, false), [0x3A00, 0x3C00, 0x4000, 0xAAAA_5555], 0xAAAA_3A00),
        ("v_max3_f16 div:2", vop3(0x354, 0, vgpr(1), vgpr(2), vgpr(3), 0, 3, false), [0x3A00, 0x3800, 0x3400, 0xAAAA_5555], 0xAAAA_3A00),
        ("v_med3_f16 mul:4", vop3(0x357, 0, vgpr(1), vgpr(2), vgpr(3), 0, 2, false), [0x3800, 0x3A00, 0x3C00, 0xAAAA_5555], 0xAAAA_3A00),
        // Clamp still saturates [0, 1]: (0.75 + 0.75) div:2 clamp -> 1.5 clamped to 1.0h (0x3C00), NOT 0.75h!
        ("v_add_f16 div:2 clamp", vop3(0x132, 0, vgpr(1), vgpr(2), 0, 0, 3, true), [0x3A00, 0x3A00, 0, 0xAAAA_5555], 0xAAAA_3C00),
        // Negative sum clamps to +0.0h (0x0000), NaN clamps to +0.0h (0x0000) under DX10_CLAMP=1.
        ("v_add_f16 neg mul:2 clamp", vop3(0x132, 0, vgpr(1), vgpr(2), 0, 0, 1, true), [0xB800, 0xB800, 0, 0xAAAA_5555], 0xAAAA_0000),
        ("v_add_f16 nan mul:2 clamp", vop3(0x132, 0, vgpr(1), vgpr(2), 0, 0, 1, true), [0x7E00, 0x3800, 0, 0xAAAA_5555], 0xAAAA_0000),
    ];

    for &(desc, inst, input, want) in checks {
        let mut body = vec![seed];
        body.extend_from_slice(&inst);
        let shader = build_cs(&body);
        run_cases(&shader, &[input], |backend, _, _, got| {
            assert_eq!(got, want, "{backend} {desc}: got {got:#010x}, want {want:#010x}");
        });
    }
}

#[test]
fn f16_transcendental_cvt_and_sdwa_ignore_omod_and_keep_clamp() {
    // Mirrors Classes 2 & 3 of `tests/semantic/test_f16_omod_rdna2.py` + `SHADER-135`
    // (`v_cvt_f16_f32 0.75 clamp mul:2 -> 0x3A00`), plus SDWA `f16` `omod` (which
    // follows the VOP3 gate by design; `SHADER-135` measured VOP3 encodings).
    let seed = vop1(0x001, 0, vgpr(4));
    let checks: &[(&str, [u32; 2], [u32; 4], u32)] = &[
        // Class 2: unary / transcendental f16
        ("v_rcp_f16 2.0 mul:2", vop3(0x1d4, 0, vgpr(1), 0, 0, 0, 1, false), [0x4000, 0, 0, 0xBBBB_0000], 0xBBBB_3800),
        ("v_sqrt_f16 4.0 mul:2", vop3(0x1d5, 0, vgpr(1), 0, 0, 0, 1, false), [0x4400, 0, 0, 0xBBBB_0000], 0xBBBB_4000),
        ("v_fract_f16 1.75 mul:4", vop3(0x1df, 0, vgpr(1), 0, 0, 0, 2, false), [0x3F00, 0, 0, 0xBBBB_0000], 0xBBBB_3A00),
        ("v_sqrt_f16 4.0 mul:2 clamp", vop3(0x1d5, 0, vgpr(1), 0, 0, 0, 1, true), [0x4400, 0, 0, 0xBBBB_0000], 0xBBBB_3C00),
        ("v_exp_f16 0.0 mul:2", vop3(0x1d8, 0, vgpr(1), 0, 0, 0, 1, false), [0x0000, 0, 0, 0xBBBB_0000], 0xBBBB_3C00),
        // Class 3: conversions to f16 (`v_cvt_f16_f32`, `v_cvt_f16_u16`, `v_cvt_f16_i16`)
        ("v_cvt_f16_f32 0.75f clamp mul:2", vop3(0x18a, 0, vgpr(1), 0, 0, 0, 1, true), [0.75f32.to_bits(), 0, 0, 0xBBBB_0000], 0xBBBB_3A00),
        ("v_cvt_f16_f32 2.5f div:2 clamp", vop3(0x18a, 0, vgpr(1), 0, 0, 0, 3, true), [2.5f32.to_bits(), 0, 0, 0xBBBB_0000], 0xBBBB_3C00),
        ("v_cvt_f16_u16 3 mul:2", vop3(0x1d0, 0, vgpr(1), 0, 0, 0, 1, false), [3, 0, 0, 0xBBBB_0000], 0xBBBB_4200),
        ("v_cvt_f16_i16 3 div:2", vop3(0x1d1, 0, vgpr(1), 0, 0, 0, 3, false), [3, 0, 0, 0xBBBB_0000], 0xBBBB_4200),
        ("v_cvt_f16_u16 5 mul:4 clamp", vop3(0x1d0, 0, vgpr(1), 0, 0, 0, 2, true), [5, 0, 0, 0xBBBB_0000], 0xBBBB_3C00),
        ("v_cvt_f16_i16 -1 mul:2 clamp", vop3(0x1d1, 0, vgpr(1), 0, 0, 0, 1, true), [0xFFFF, 0, 0, 0xBBBB_0000], 0xBBBB_0000),
        // SDWA f16 `omod` follows the VOP3 gate by design (`clamp` still applies)
        ("v_add_f16_sdwa 0.5+0.25 mul:2", vop2_sdwa_word0(0x032, 0, 1, 2, 1, false), [0x3800, 0x3400, 0, 0xBBBB_0000], 0xBBBB_3A00),
        ("v_add_f16_sdwa 0.75+0.75 div:2 clamp", vop2_sdwa_word0(0x032, 0, 1, 2, 3, true), [0x3A00, 0x3A00, 0, 0xBBBB_0000], 0xBBBB_3C00),
    ];

    for &(desc, inst, input, want) in checks {
        let mut body = vec![seed];
        body.extend_from_slice(&inst);
        let shader = build_cs(&body);
        run_cases(&shader, &[input], |backend, _, _, got| {
            assert_eq!(got, want, "{backend} {desc}: got {got:#010x}, want {want:#010x}");
        });
    }
}

#[test]
fn fspx_pgm_rsrc1_gates_f16_omod_on_denorm_flush_and_ieee_off() {
    // End-to-end `ACCC` (`FSPX`) container translation through `crate::translate`:
    //   v0: v_add_f16 0.25h + 0.5h mul:2          -> 1.5h (0x3E00) iff enabled, else 0.75h (0x3A00)
    //   v1: v_cvt_f16_f32 0.375f mul:4            -> 1.5h (0x3E00) iff enabled, else 0.375h (0x3600)
    //   v2: v_add_f16 0.75h + 0.75h div:2 clamp   -> 0.75h (0x3A00) iff enabled, else 1.0h (0x3C00)
    //   v3: v_rcp_f16 2.0h mul:2                  -> 1.0h (0x3C00) iff enabled, else 0.5h (0x3800)
    //   v4: v_add_f16_sdwa 0.5h + 0.25h mul:2     -> 1.5h (0x3E00) iff enabled, else 0.75h (0x3A00)
    let mut body = Vec::new();
    body.extend_from_slice(&v_mov_lit(8, 0x3400)); // v8 = 0.25h
    body.extend_from_slice(&v_mov_lit(9, 0x3800)); // v9 = 0.5h
    body.extend_from_slice(&v_mov_lit(10, 0.375f32.to_bits())); // v10 = 0.375f
    body.extend_from_slice(&v_mov_lit(11, 0x3A00)); // v11 = 0.75h
    body.extend_from_slice(&v_mov_lit(12, 0x4000)); // v12 = 2.0h
    for dst in 0..5u8 {
        body.extend_from_slice(&v_mov_lit(dst, 0xCAFE_0000));
    }
    body.extend_from_slice(&vop3(0x132, 0, vgpr(8), vgpr(9), 0, 0, 1, false));
    body.extend_from_slice(&vop3(0x18a, 1, vgpr(10), 0, 0, 0, 2, false));
    body.extend_from_slice(&vop3(0x132, 2, vgpr(11), vgpr(11), 0, 0, 3, true));
    body.extend_from_slice(&vop3(0x1d4, 3, vgpr(12), 0, 0, 0, 1, false));
    body.extend_from_slice(&vop2_sdwa_word0(0x032, 4, 9, 8, 1, false));
    for dst in 0..5u8 {
        body.extend_from_slice(&buffer_store_dword(dst, 4, u16::from(dst) * 4));
    }

    let scaled = [
        0xCAFE_3E00,
        0xCAFE_3E00,
        0xCAFE_3A00,
        0xCAFE_3C00,
        0xCAFE_3E00,
    ];
    let ignored = [
        0xCAFE_3A00,
        0xCAFE_3600,
        0xCAFE_3C00,
        0xCAFE_3800,
        0xCAFE_3A00,
    ];

    let modes: &[(&str, u32, [u32; 5])] = &[
        // FLOAT_MODE = 0x00 (denorm_mode_16_64 = 0), DX10_CLAMP = 1, IEEE = 0 -> omod scales
        ("denorm16_64=0,ieee=0", 1 << 21, scaled),
        // Console / shipped FSPX FLOAT_MODE = 0xC0 (denorm_mode_16_64 = 3), IEEE = 0 -> omod ignored
        ("denorm16_64=3,ieee=0", (0xC0 << 12) | (1 << 21), ignored),
        // FLOAT_MODE = 0x00 (denorm_mode_16_64 = 0), IEEE = 1 (bit 23) -> omod ignored
        ("denorm16_64=0,ieee=1", (1 << 21) | (1 << 23), ignored),
    ];

    for &(desc, rsrc1, want) in modes {
        let shader = build_fspx_cs(rsrc1, &body);
        run_fspx_dwords(&shader, 5, |backend, got| {
            assert_eq!(got, &want[..], "{backend} FSPX {desc}: got {got:08x?}, want {want:08x?}");
        });
    }
}

#[test]
fn f32_destination_controls_still_apply_omod() {
    // Mirrors Class 4 of `tests/semantic/test_f16_omod_rdna2.py`: `v_cvt_f32_f16`
    // (`0x18b`) and `v_add_f32` (`0x103`) have `f32` destinations (`denorm_mode_32 = 0`),
    // so `omod` continues to scale by 2, 4, and 0.5 before optional `clamp`.
    let checks: &[(&str, [u32; 2], [u32; 4], u32)] = &[
        ("v_cvt_f32_f16 2.0h mul:2", vop3(0x18b, 0, vgpr(1), 0, 0, 0, 1, false), [0x4000, 0, 0, 0], 4.0f32.to_bits()),
        ("v_cvt_f32_f16 2.0h mul:4", vop3(0x18b, 0, vgpr(1), 0, 0, 0, 2, false), [0x4000, 0, 0, 0], 8.0f32.to_bits()),
        ("v_cvt_f32_f16 2.0h div:2", vop3(0x18b, 0, vgpr(1), 0, 0, 0, 3, false), [0x4000, 0, 0, 0], 1.0f32.to_bits()),
        ("v_cvt_f32_f16 2.0h clamp", vop3(0x18b, 0, vgpr(1), 0, 0, 0, 0, true), [0x4000, 0, 0, 0], 1.0f32.to_bits()),
        ("v_add_f32 1.5+1.5 mul:2", vop3(0x103, 0, vgpr(1), vgpr(2), 0, 0, 1, false), [1.5f32.to_bits(), 1.5f32.to_bits(), 0, 0], 6.0f32.to_bits()),
    ];

    for &(desc, inst, input, want) in checks {
        let shader = build_cs(&inst);
        run_cases(&shader, &[input], |backend, _, _, got| {
            assert_eq!(got, want, "{backend} {desc}: got {got:#010x}, want {want:#010x}");
        });
    }
}
