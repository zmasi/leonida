//! End-to-end D3D12 hardware and WARP tests for RDNA2 SALU `SCC` carry and signed overflow:
//! `s_lshl1_add_u32`..`s_lshl4_add_u32`, `s_add_i32`, `s_sub_i32`, and `s_addk_i32`.

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

const fn sop2(op: u8, sdst: u8, ssrc0: u8, ssrc1: u8) -> u32 {
    0x8000_0000
        | (op as u32 & 0x7F) << 23
        | (sdst as u32 & 0x7F) << 16
        | (ssrc1 as u32) << 8
        | ssrc0 as u32
}

const fn sopk(op: u8, sdst: u8, simm16: u16) -> u32 {
    0xB000_0000 | (op as u32 & 0x1F) << 23 | (sdst as u32 & 0x7F) << 16 | simm16 as u32
}

const fn s_cbranch_scc1(simm16: i16) -> u32 {
    0xBF85_0000 | (simm16 as u16 as u32)
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

const fn buffer_store_dwordx2(vdata: u8, srsrc: u8) -> [u32; 2] {
    [
        0x38 << 26 | 0x1D << 18,
        0x80 << 24 | ((srsrc as u32 >> 2) & 0x1F) << 16 | (vdata as u32) << 8,
    ]
}

/// Builds a compute shader taking root constants `s0, s1`, running `body` to
/// populate `s6` (result) and `s7` (observed `SCC`), and storing `[s6, s7]`.
fn build_salu_pair_cs(body: &[u32]) -> HostShader {
    let mut words = Vec::new();
    words.push(s_mov_b32(5, 0x80));
    words.extend_from_slice(&s_load_dwordx4(8, 4));
    words.extend_from_slice(body);
    words.push(vop1(0x001, 0, 6)); // v_mov_b32 v0, s6
    words.push(vop1(0x001, 1, 7)); // v_mov_b32 v1, s7
    words.extend_from_slice(&buffer_store_dwordx2(0, 8));
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

    if let Ok(mut v) = validator::Validator::load(validator::Validator::default_path().as_deref()) {
        let mut copy = shader.dxil.clone();
        let ours = copy[4..20].to_vec();
        v.validate(&mut copy).expect("DXIL validation failed");
        assert_eq!(copy[4..20], ours[..], "container hash mismatch");
    }

    shader
}

fn run_pairs(
    shader: &HostShader,
    pairs: &[(u32, u32)],
    mut check: impl FnMut(&str, usize, (u32, u32), [u32; 2]),
) {
    let _one = ONE_AT_A_TIME.lock().unwrap_or_else(|e| e.into_inner());
    let mut ran = 0;
    for (label, warp) in [("hardware", false), ("warp", true)] {
        let Ok(host) = Host::new(warp) else {
            continue;
        };
        let (root, pso) = host
            .compute_pipeline(shader)
            .unwrap_or_else(|e| panic!("{label}: compute_pipeline failed: {e}"));
        for (i, &(a, b)) in pairs.iter().enumerate() {
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
                .unwrap_or_else(|e| panic!("{label}: dispatch failed: {e}"));
            check(label, i, (a, b), [out[0][0], out[0][1]]);
        }
        ran += 1;
    }
    assert!(ran > 0, "no SM 6.6 device available");
}

#[test]
fn s_lshl1_to_4_add_u32_set_scc_to_unsigned_carry() {
    let mut pairs = vec![
        (0u32, 0u32),
        (1, 2),                       // non-zero sum, zero carry -> SCC = 0
        (0x8000_0000, 0),             // top bit shifted out on k>=1, zero sum on k=1 -> SCC = 1
        (0x4000_0000, 0x8000_0000),   // (a << 1) + b == 2^32 -> sum 0, SCC = 1
        (0x4000_0000, 0x7FFF_FFFF),   // (a << 1) + b == 2^32 - 1 -> SCC = 0 on k=1, 1 on k>=2
        (0x2000_0000, 0),             // SCC = 0 on k=1,2; SCC = 1 on k=3,4
        (0x1000_0000, 0),             // SCC = 0 on k=1..3; SCC = 1 on k=4
        (0x0FFF_FFFF, 0x0000_0010),   // wraps 32 bits on k=4 -> sum 0, SCC = 1
        (0x0FFF_FFFF, 0x0000_000F),   // 0xFFFF_FFFF on k=4 -> SCC = 0
        (u32::MAX, u32::MAX),
    ];
    let mut state = 0x5A1C_2026u32;
    for _ in 0..32 {
        state = state.wrapping_mul(1664525).wrapping_add(1013904223);
        let a = state;
        state = state.wrapping_mul(1664525).wrapping_add(1013904223);
        pairs.push((a, state));
    }

    for (k, op) in [(1u32, 0x2eu8), (2, 0x2f), (3, 0x30), (4, 0x31)] {
        let shader = build_salu_pair_cs(&[
            sop2(op, 6, 0, 1),          // s_lshl{k}_add_u32 s6, s0, s1
            sop2(0x0a, 7, 129, 128),    // s_cselect_b32 s7, 1, 0
        ]);
        run_pairs(&shader, &pairs, |backend, idx, (a, b), got| {
            let full = (u64::from(a) << k) + u64::from(b);
            let exp_r = full as u32;
            let exp_scc = u32::from(full >= (1u64 << 32));
            assert_eq!(
                got,
                [exp_r, exp_scc],
                "{backend} s_lshl{k}_add_u32 case {idx} a={a:#010x} b={b:#010x}: got {got:#x?}, expected [{exp_r:#010x}, {exp_scc}]"
            );
        });
    }
}

#[test]
fn s_add_i32_s_sub_i32_and_s_addk_i32_set_scc_to_signed_overflow() {
    let pairs = [
        (0u32, 0u32),
        (1, 2),
        (0x7FFF_FFFF, 1),             // +MAX + 1 -> overflow
        (0x7FFF_FFFF, 0),             // +MAX + 0 -> no overflow
        (0x7FFF_FFFF, u32::MAX),      // +MAX + (-1) -> no overflow
        (0x8000_0000, u32::MAX),      // MIN + (-1) -> overflow
        (0x8000_0000, 1),             // MIN + 1 -> no overflow (add), MIN - 1 -> overflow (sub)
        (0x8000_0000, 0x8000_0000),   // MIN + MIN -> overflow (add), MIN - MIN -> no overflow (sub)
        (0, 0x8000_0000),             // 0 + MIN -> no overflow (add), 0 - MIN -> overflow (sub)
        (0x4000_0000, 0x4000_0000),   // +2^30 + +2^30 -> overflow (add)
        (0xC000_0000, 0xC000_0000),   // -2^30 + -2^30 -> no overflow (-2^31 fits!)
        (0xC000_0000, 0xBFFF_FFFF),   // -2^30 + (-2^30 - 1) -> overflow
    ];

    for (name, op, is_sub) in [
        ("s_add_i32", 0x02u8, false),
        ("s_sub_i32", 0x03u8, true),
    ] {
        // Observe SCC via both s_cselect_b32 and s_cbranch_scc1.
        let shader = build_salu_pair_cs(&[
            sop2(op, 6, 0, 1),          // s_add_i32 / s_sub_i32 s6, s0, s1
            s_mov_b32(7, 128),          // s_mov_b32 s7, 0 (preserves SCC)
            s_cbranch_scc1(1),          // if SCC == 1, skip clearing and set bit
            s_mov_b32(7, 128),          // fallthrough keeps 0
            sop2(0x0a, 7, 129, 7),      // s_cselect_b32 s7, 1, s7
        ]);
        run_pairs(&shader, &pairs, |backend, idx, (a, b), got| {
            let sa = i64::from(a as i32);
            let sb = i64::from(b as i32);
            let full = if is_sub { sa - sb } else { sa + sb };
            let exp_r = if is_sub {
                a.wrapping_sub(b)
            } else {
                a.wrapping_add(b)
            };
            let exp_scc = u32::from(!(i64::from(i32::MIN)..=i64::from(i32::MAX)).contains(&full));
            assert_eq!(
                got,
                [exp_r, exp_scc],
                "{backend} {name} case {idx} a={a:#010x} b={b:#010x}: got {got:#x?}, expected [{exp_r:#010x}, {exp_scc}]"
            );
        });
    }

    for simm16 in [0x0001u16, 0x7FFF, 0x8000, 0xFFFF] {
        let sx = i32::from(simm16 as i16);
        let shader = build_salu_pair_cs(&[
            s_mov_b32(6, 0),            // s_mov_b32 s6, s0
            sopk(0x0f, 6, simm16),      // s_addk_i32 s6, simm16
            sop2(0x0a, 7, 129, 128),    // s_cselect_b32 s7, 1, 0
        ]);
        let inputs: Vec<(u32, u32)> = [
            0u32,
            0x7FFF_FFFF,
            0x7FFF_8000,
            0x7FFF_8001,
            0x8000_0000,
            0x8000_7FFF,
            0x8000_8000,
            u32::MAX,
        ]
        .into_iter()
        .map(|a| (a, 0))
        .collect();
        run_pairs(&shader, &inputs, |backend, idx, (a, _), got| {
            let full = i64::from(a as i32) + i64::from(sx);
            let exp_r = a.wrapping_add(sx as u32);
            let exp_scc = u32::from(!(i64::from(i32::MIN)..=i64::from(i32::MAX)).contains(&full));
            assert_eq!(
                got,
                [exp_r, exp_scc],
                "{backend} s_addk_i32 simm={simm16:#06x} case {idx} a={a:#010x}: got {got:#x?}, expected [{exp_r:#010x}, {exp_scc}]"
            );
        });
    }
}
