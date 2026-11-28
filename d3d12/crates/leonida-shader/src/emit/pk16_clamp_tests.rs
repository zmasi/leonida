//! End-to-end D3D12 hardware + WARP + `dxil.dll` regression tests for VOP3P
//! packed 16-bit integer `clamp` (`v_pk_add_{u16,i16}`, `v_pk_sub_{u16,i16}`,
//! `v_pk_mad_{u16,i16}`, `v_pk_mul_lo_u16`, `v_pk_{max,min}_{u16,i16}`) and
//! VOP3 16-bit unsigned `clamp` (`v_add_nc_u16`, `v_sub_nc_u16`).
//!
//! Expected outputs match the native RDNA2 (`gfx1036`) hardware measurements
//! recorded in `docs/design.md`, `tools/pk16_diff.py`, and
//! `tools/pk_int16_clamp_probe.py`; these tests execute the translated DXIL on
//! the host D3D12 GPU and WARP.

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

const fn sgpr(n: u8) -> u16 {
    n as u16
}

const fn vgpr(n: u8) -> u16 {
    256 + n as u16
}

/// VOP3P `op vdst, src0, src1[, src2]` (`0x33 << 26`).
/// `opsel` bits `[2:0]` select the high half for the low 16-bit result;
/// `opsel_hi` bits `[2:0]` select the high half for the high 16-bit result.
#[allow(clippy::too_many_arguments)]
const fn vop3p(
    op: u16,
    vdst: u8,
    src0: u16,
    src1: u16,
    src2: u16,
    opsel: u8,
    opsel_hi: u8,
    neg: u8,
    neg_hi: u8,
    clamp: bool,
) -> [u32; 2] {
    [
        0x33 << 26
            | (op as u32 & 0x7F) << 16
            | (clamp as u32) << 15
            | ((opsel_hi as u32 >> 2) & 1) << 14
            | (opsel as u32 & 7) << 11
            | (neg_hi as u32 & 7) << 8
            | vdst as u32,
        (neg as u32 & 7) << 29
            | (opsel_hi as u32 & 3) << 27
            | (src0 as u32 & 0x1FF)
            | (src1 as u32 & 0x1FF) << 9
            | (src2 as u32 & 0x1FF) << 18,
    ]
}

/// VOP3 `op vdst, src0, src1, src2` (`0x35 << 26`) with `opsel`, `abs`, `neg`,
/// `omod`, and `clamp`.
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
    try_build_cs(body).expect("shader should lower and validate")
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

const fn pack16(lo: u16, hi: u16) -> u32 {
    (lo as u32) | ((hi as u32) << 16)
}

const fn half_u(w: u32, hi: bool) -> u32 {
    if hi { w >> 16 } else { w & 0xFFFF }
}

const fn half_i(w: u32, hi: bool) -> i32 {
    (half_u(w, hi) as u16 as i16) as i32
}

const fn sat_u16(x: i64) -> u16 {
    if x < 0 {
        0
    } else if x > 0xFFFF {
        0xFFFF
    } else {
        x as u16
    }
}

const fn sat_i16(x: i64) -> u16 {
    let c = if x < -32768 {
        -32768
    } else if x > 32767 {
        32767
    } else {
        x as i32
    };
    (c as i16) as u16
}

#[test]
fn packed_u16_clamp_saturates_per_half_and_respects_opsel() {
    let inputs: &[[u32; 4]] = &[
        // Asymmetric overflow/borrow, exact boundaries, and bit-31 u32 product+add:
        // case 0: 0xFFFF * 0xFFFF + 0xFFFF = 0xFFFF_0000 (bit 31 set in u32!).
        [pack16(0xFFFF, 0x1234), pack16(0xFFFF, 0x4321), pack16(0xFFFF, 0x0001), 0],
        // case 1: lo borrows on sub (0 - 1), hi does not (0x8000 - 0x7FFF = 1).
        [pack16(0x0000, 0x8000), pack16(0x0001, 0x7FFF), pack16(0x0000, 0x0001), 0],
        // case 2: exact 0xFFFF boundary vs +1 past 0xFFFF (0x0100 * 0x0100 = 0x10000).
        [pack16(0x7FFF, 0x0100), pack16(0x8000, 0x0100), pack16(0x0000, 0x0000), 0],
        // case 3: 0x8000 + 0x8000 overflow in lo, 300 * 200 + 5535 = 65535 exact in hi.
        [pack16(0x8000, 300), pack16(0x8000, 200), pack16(0x0001, 5535), 0],
        // case 4: 300 * 200 + 5536 = 65536 overflow in lo, borrow 0x1234 - 0x5678 in hi.
        [pack16(300, 0x1234), pack16(200, 0x5678), pack16(5536, 0x0000), 0],
    ];

    for &(opcode, name) in &[
        (0x00au16, "v_pk_add_u16"),
        (0x00bu16, "v_pk_sub_u16"),
        (0x009u16, "v_pk_mad_u16"),
    ] {
        for &(opsel, opsel_hi) in &[(0b000u8, 0b111u8), (0b101u8, 0b010u8)] {
            let clamped = build_cs(&vop3p(
                opcode,
                0,
                vgpr(1),
                vgpr(2),
                vgpr(3),
                opsel,
                opsel_hi,
                0,
                0,
                true,
            ));
            let plain = build_cs(&vop3p(
                opcode,
                0,
                vgpr(1),
                vgpr(2),
                vgpr(3),
                opsel,
                opsel_hi,
                0,
                0,
                false,
            ));

            let eval = |ins: [u32; 4], sat: bool| -> u32 {
                let mut out = [0u16; 2];
                for (h, &sel) in [opsel, opsel_hi].iter().enumerate() {
                    let a = half_u(ins[0], sel & 1 != 0) as i64;
                    let b = half_u(ins[1], sel & 2 != 0) as i64;
                    let c = half_u(ins[2], sel & 4 != 0) as i64;
                    let exact = match opcode {
                        0x00a => a + b,
                        0x00b => a - b,
                        0x009 => a * b + c,
                        _ => unreachable!(),
                    };
                    out[h] = if sat {
                        sat_u16(exact)
                    } else {
                        (exact as u64 & 0xFFFF) as u16
                    };
                }
                pack16(out[0], out[1])
            };

            run_cases(&clamped, inputs, |backend, i, ins, got| {
                let want = eval(ins, true);
                assert_eq!(
                    got, want,
                    "{backend} {name} clamp opsel={opsel:#b}/{opsel_hi:#b} case {i}: got {got:#010x}, want {want:#010x}"
                );
            });
            run_cases(&plain, inputs, |backend, i, ins, got| {
                let want = eval(ins, false);
                assert_eq!(
                    got, want,
                    "{backend} {name} plain opsel={opsel:#b}/{opsel_hi:#b} case {i}: got {got:#010x}, want {want:#010x}"
                );
            });
        }
    }
}

#[test]
fn packed_i16_clamp_saturates_to_signed_bounds_per_half() {
    let inputs: &[[u32; 4]] = &[
        // case 0: +32767 + 1 -> +32767 in lo; -32768 + (-1) -> -32768 in hi.
        [pack16(0x7FFF, 0x8000), pack16(0x0001, 0xFFFF), pack16(0x0001, 0xFFFF), 0],
        // case 1: (-32768)*(-32768) + 0 = +1073741824 -> +32767 in lo;
        //         (-32768)*(+32767) + (-32768) = -1073741824 -> -32768 in hi.
        [pack16(0x8000, 0x8000), pack16(0x8000, 0x7FFF), pack16(0x0000, 0x8000), 0],
        // case 2: +256 * +128 + 0 = +32768 (+1 past max) in lo;
        //         -256 * +128 + (-1) = -32769 (-1 past min) in hi.
        [pack16(256, (-256i16) as u16), pack16(128, 128), pack16(0, 0xFFFF), 0],
        // case 3: exact signed boundaries (-32768 and +32767) without overflow.
        [pack16((-256i16) as u16, 256), pack16(128, 128), pack16(0, 0xFFFF), 0],
        // case 4: -32768 - (+32767) = -65535 -> -32768 in lo; +32767 - (-32768) = +65535 -> +32767 in hi.
        [pack16(0x8000, 0x7FFF), pack16(0x7FFF, 0x8000), pack16(0x0010, 0xFFEF), 0],
    ];

    for &(opcode, name) in &[
        (0x002u16, "v_pk_add_i16"),
        (0x003u16, "v_pk_sub_i16"),
        (0x000u16, "v_pk_mad_i16"),
    ] {
        for &(opsel, opsel_hi) in &[(0b000u8, 0b111u8), (0b011u8, 0b100u8)] {
            let clamped = build_cs(&vop3p(
                opcode,
                0,
                vgpr(1),
                vgpr(2),
                vgpr(3),
                opsel,
                opsel_hi,
                0,
                0,
                true,
            ));
            let eval = |ins: [u32; 4]| -> u32 {
                let mut out = [0u16; 2];
                for (h, &sel) in [opsel, opsel_hi].iter().enumerate() {
                    let a = half_i(ins[0], sel & 1 != 0) as i64;
                    let b = half_i(ins[1], sel & 2 != 0) as i64;
                    let c = half_i(ins[2], sel & 4 != 0) as i64;
                    let exact = match opcode {
                        0x002 => a + b,
                        0x003 => a - b,
                        0x000 => a * b + c,
                        _ => unreachable!(),
                    };
                    out[h] = sat_i16(exact);
                }
                pack16(out[0], out[1])
            };

            run_cases(&clamped, inputs, |backend, i, ins, got| {
                let want = eval(ins);
                assert_eq!(
                    got, want,
                    "{backend} {name} clamp opsel={opsel:#b}/{opsel_hi:#b} case {i}: got {got:#010x}, want {want:#010x}"
                );
            });
        }
    }
}

#[test]
fn packed_mul_lo_and_minmax_clamp_are_hardware_noops() {
    // Vectors from `tools/pk_int16_clamp_probe.py`: `v_pk_mul_lo_u16 clamp` wraps
    // modulo 2^16 on RDNA2 (`0xFFFF * 2 = 0xFFFE`, NOT `0xFFFF`), and `clamp` on
    // packed integer min/max leaves the bounded 16-bit result unchanged.
    let inputs: &[[u32; 4]] = &[
        [pack16(0xFFFF, 0xFFFF), pack16(0x0002, 0xFFFF), 0, 0],
        [pack16(0x0100, 300), pack16(0x0100, 300), 0, 0],
        [pack16(0x8000, 0x7FFF), pack16(0x7FFF, 0x8000), 0, 0],
        [pack16(0xFFFE, 0x8001), pack16(0x0001, 0x7FFE), 0, 0],
    ];

    for &(opcode, name) in &[
        (0x001u16, "v_pk_mul_lo_u16"),
        (0x007u16, "v_pk_max_i16"),
        (0x008u16, "v_pk_min_i16"),
        (0x00cu16, "v_pk_max_u16"),
        (0x00du16, "v_pk_min_u16"),
    ] {
        let clamped = build_cs(&vop3p(
            opcode,
            0,
            vgpr(1),
            vgpr(2),
            0,
            0b000,
            0b011,
            0,
            0,
            true,
        ));
        let plain = build_cs(&vop3p(
            opcode,
            0,
            vgpr(1),
            vgpr(2),
            0,
            0b000,
            0b011,
            0,
            0,
            false,
        ));

        let eval = |ins: [u32; 4]| -> u32 {
            let mut out = [0u16; 2];
            for (h, hi) in [false, true].into_iter().enumerate() {
                let au = half_u(ins[0], hi);
                let bu = half_u(ins[1], hi);
                let ai = half_i(ins[0], hi);
                let bi = half_i(ins[1], hi);
                out[h] = match opcode {
                    0x001 => ((au * bu) & 0xFFFF) as u16,
                    0x007 => (ai.max(bi) as i16) as u16,
                    0x008 => (ai.min(bi) as i16) as u16,
                    0x00c => au.max(bu) as u16,
                    0x00d => au.min(bu) as u16,
                    _ => unreachable!(),
                };
            }
            pack16(out[0], out[1])
        };

        run_cases(&clamped, inputs, |backend, i, ins, got| {
            let want = eval(ins);
            assert_eq!(
                got, want,
                "{backend} {name} clamp case {i}: got {got:#010x}, want {want:#010x}"
            );
        });
        run_cases(&plain, inputs, |backend, i, ins, got| {
            let want = eval(ins);
            assert_eq!(
                got, want,
                "{backend} {name} plain case {i}: got {got:#010x}, want {want:#010x}"
            );
        });
    }
}

#[test]
fn vop3_add_and_sub_nc_u16_clamp_saturate_and_preserve_other_half() {
    let inputs: &[[u32; 4]] = &[
        // v1 = src0, v2 = src1, v3 = initial v0 seed (to check other-half preservation).
        [pack16(0xFFFF, 0x8000), pack16(0x0001, 0x9000), 0xDEAD_BEEF, 0],
        [pack16(0x0000, 0x1000), pack16(0x0001, 0x2000), 0xCAFE_1234, 0],
        [pack16(0x1234, 0x5000), pack16(0x2345, 0x1234), 0xABCD_EF01, 0],
    ];

    for &(opcode, name, is_sub) in &[
        (0x303u16, "v_add_nc_u16", false),
        (0x304u16, "v_sub_nc_u16", true),
    ] {
        // opsel = 0b0000 (src0.lo, src1.lo -> dst.lo, preserving dst.hi)
        // opsel = 0b1011 (src0.hi, src1.hi -> dst.hi, preserving dst.lo; bit 3 is dst hi)
        for &opsel in &[0b0000u8, 0b1011u8] {
            let mut body = vec![vop1(0x001, 0, vgpr(3))]; // v0 = seed
            body.extend_from_slice(&vop3(
                opcode,
                0,
                vgpr(1),
                vgpr(2),
                0,
                opsel,
                0,
                0,
                0,
                true,
            ));
            let shader = build_cs(&body);

            run_cases(&shader, inputs, |backend, i, ins, got| {
                let a = half_u(ins[0], opsel & 1 != 0) as i64;
                let b = half_u(ins[1], opsel & 2 != 0) as i64;
                let res = sat_u16(if is_sub { a - b } else { a + b }) as u32;
                let want = if opsel & 8 != 0 {
                    (ins[2] & 0x0000_FFFF) | (res << 16)
                } else {
                    (ins[2] & 0xFFFF_0000) | res
                };
                assert_eq!(
                    got, want,
                    "{backend} {name} clamp opsel={opsel:#b} case {i}: got {got:#010x}, want {want:#010x}"
                );
            });
        }
    }
}

#[test]
fn packed_int16_clamp_handles_sgpr_and_inline_operands() {
    // Exercises SGPR (`s0`, high half selected via `opsel_hi`) + inline constant
    // (`0x80 + 16` = 16, `0xC1` = -1 = 0xFFFF_FFFF) under `clamp`.
    let add_u16_sgpr = build_cs(&vop3p(
        0x00a,
        0,
        sgpr(0),
        0x80 + 16,
        0,
        0b000,
        0b001,
        0,
        0,
        true,
    ));
    let inputs_u: &[[u32; 4]] = &[
        [pack16(0xFFF5, 0x1000), 0, 0, 0], // lo: 0xFFF5 + 16 -> 0xFFFF; hi: 0x1000 + 16 -> 0x1010
        [pack16(0xFFEF, 0xFFFF), 0, 0, 0], // lo: 0xFFEF + 16 -> 0xFFFF; hi: 0xFFFF + 16 -> 0xFFFF
    ];
    run_cases(&add_u16_sgpr, inputs_u, |backend, i, ins, got| {
        let lo = sat_u16(half_u(ins[0], false) as i64 + 16);
        let hi = sat_u16(half_u(ins[0], true) as i64 + 16);
        let want = pack16(lo, hi);
        assert_eq!(got, want, "{backend} sgpr+inline u16 case {i}: got {got:#010x}, want {want:#010x}");
    });

    // 0xC1 is inline integer -1 (0xFFFF_FFFF, so both halves are -1 in i16).
    let add_i16_sgpr = build_cs(&vop3p(
        0x002,
        0,
        sgpr(0),
        0xC1,
        0,
        0b000,
        0b011,
        0,
        0,
        true,
    ));
    let inputs_i: &[[u32; 4]] = &[
        [pack16(0x8000, 0x0000), 0, 0, 0], // lo: -32768 + (-1) -> -32768 (0x8000); hi: 0 + (-1) -> 0xFFFF
    ];
    run_cases(&add_i16_sgpr, inputs_i, |backend, i, _, got| {
        let want = pack16(0x8000, 0xFFFF);
        assert_eq!(got, want, "{backend} sgpr+inline i16 case {i}: got {got:#010x}, want {want:#010x}");
    });
}

#[test]
fn unmeasured_int16_clamp_and_neg_omod_remain_unsupported() {
    // Packed shifts with `clamp` are unmeasured and rejected by `tools/rdna2spirv.py`.
    for opcode in [0x004u16, 0x005, 0x006] {
        let err = try_build_cs(&vop3p(opcode, 0, vgpr(1), vgpr(2), 0, 0, 0b011, 0, 0, true))
            .expect_err("packed shift with clamp must remain Unsupported");
        assert!(
            matches!(err, ShaderError::Unsupported { .. }),
            "expected Unsupported for packed shift {opcode:#x} clamp, got {err:?}"
        );
    }

    // `neg` / `neg_hi` on packed integer ops must still be rejected.
    for (neg, neg_hi) in [(1u8, 0u8), (0u8, 1u8)] {
        let err = try_build_cs(&vop3p(
            0x00a,
            0,
            vgpr(1),
            vgpr(2),
            0,
            0,
            0b011,
            neg,
            neg_hi,
            false,
        ))
        .expect_err("packed u16 op with neg/neg_hi must remain Unsupported");
        assert!(matches!(err, ShaderError::Unsupported { .. }));
    }

    // VOP3 16-bit non-add/sub integer ops (`v_mul_lo_u16`, shifts, min/max) with `clamp`
    // remain unmeasured and rejected, matching `tools/rdna2spirv.py`.
    for opcode in [0x305u16, 0x307, 0x308, 0x309, 0x30a, 0x30b, 0x30c, 0x314] {
        let err = try_build_cs(&vop3(opcode, 0, vgpr(1), vgpr(2), 0, 0, 0, 0, 0, true))
            .expect_err("unmeasured VOP3 int16 clamp must remain Unsupported");
        assert!(
            matches!(err, ShaderError::Unsupported { .. }),
            "expected Unsupported for VOP3 {opcode:#x} clamp, got {err:?}"
        );
    }

    // `omod`, `abs`, and `neg` on `v_add_nc_u16` must still be rejected.
    for (abs, neg, omod) in [(1u8, 0u8, 0u8), (0, 1, 0), (0, 0, 1)] {
        let err = try_build_cs(&vop3(0x303, 0, vgpr(1), vgpr(2), 0, 0, abs, neg, omod, false))
            .expect_err("v_add_nc_u16 with abs/neg/omod must remain Unsupported");
        assert!(matches!(err, ShaderError::Unsupported { .. }));
    }
}
