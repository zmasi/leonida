//! Console groups of 32 threads run several to a host group (`LEONIDA_GROUP_PACK`,
//! `lower::group_pack`): hand-written console programs translated unpacked and packed, run on the
//! host GPU over the same console grids, and compared with what the console would store.
//!
//! The programs store, for every console thread, a record of four dwords - its thread id (x, y, z)
//! and the number its group id makes (`x | y << 10 | z << 20`) - at the place its group number and
//! thread index give. A thread that runs for a group the grid does not have, or twice, or with
//! another group's id or memory shows in the buffer.

use std::collections::HashSet;

use super::host::{Dispatch, Host, Root, View};
use super::{HostShader, lower, pack_grid, pack_group, validator};
use crate::native::{
    ComputeIo, HwStage, NativeProgram, ResourceType, RootParam, RootType, StageIo, UsedResource,
};
use crate::{ShaderError, io, ir, resource, structure};

const S_ENDPGM: u32 = 0xBF81_0000;

fn sop2(op: u32, dst: u32, src1: u32, src0: u32) -> u32 {
    0x8000_0000 | op << 23 | dst << 16 | src1 << 8 | src0
}

fn vop2(op: u32, dst: u32, src1: u32, src0: u32) -> u32 {
    op << 25 | dst << 17 | src1 << 9 | src0
}

/// A constant 0 to 64 as an operand.
fn inline(n: u32) -> u32 {
    0x80 + n
}

fn vgpr(n: u32) -> u32 {
    0x100 + n
}

/// A program of one console wave of 32 lanes with the group `shape`, the grid's X and Y counts
/// in s0 and s1, a raw UAV in the table, group ids in s6 to s8 and thread ids in v0 to v2.
/// `lds`: dwords of group-shared memory, whose use `middle` makes (it runs before the store, with
/// the group-id number in s18 and the thread id's x in v0, and leaves the third dword in v2).
fn program(shape: [u32; 3], lds: u32, middle: &[u32]) -> NativeProgram {
    let lg = |n: u32| n.trailing_zeros();
    let mut words = vec![
        0xBE85_0380, // s_mov_b32 s5, 0
        0x3D << 26 | 0x02 << 18 | 12 << 6 | 2,
        125 << 25,                     // s_load_dwordx4 s[12:15], s[4:5], 0
        sop2(0x26, 16, 1, 8),          // s_mul_i32 s16, s8, s1 (z * Y)
        sop2(0, 16, 7, 16),            // s_add_u32 s16, s16, s7
        sop2(0x26, 16, 0, 16),         // s_mul_i32 s16, s16, s0
        sop2(0, 16, 6, 16),            // s_add_u32 s16, s16, s6: the group's number
        sop2(0x1E, 17, inline(9), 16), // s_lshl_b32 s17, s16, 9: its 512 bytes of records
        sop2(0x1E, 19, inline(10), 7), // s_lshl_b32 s19, s7, 10
        sop2(0x1E, 20, inline(20), 8), // s_lshl_b32 s20, s8, 20
        sop2(0x10, 18, 19, 6),         // s_or_b32 s18, s6, s19
        sop2(0x10, 18, 20, 18),        // s_or_b32 s18, s18, s20: the group id's number
        0x7E00_0200 | 3 << 17 | 18,    // v_mov_b32 v3, s18
    ];
    // (the record's place, before the middle changes the thread ids' registers)
    words.extend_from_slice(&[
        vop2(0x1A, 6, 0, inline(4)),                // v_lshlrev_b32 v6, 4, v0
        vop2(0x1A, 7, 1, inline(4 + lg(shape[0]))), // v_lshlrev_b32 v7, 4 + lg x, v1
        vop2(0x25, 6, 7, vgpr(6)),                  // v_add_nc_u32 v6, v6, v7
        vop2(0x1A, 7, 2, inline(4 + lg(shape[0] * shape[1]))), // v_lshlrev_b32 v7, ., v2
        vop2(0x25, 6, 7, vgpr(6)),                  // v_add_nc_u32 v6, v6, v7
    ]);
    words.extend_from_slice(middle);
    words.extend_from_slice(&[
        0x38 << 26 | 0x1E << 18 | 1 << 12,
        17 << 24 | 3 << 16 | 6, // buffer_store_dwordx4 v[0:3], v6, s[12:15], s17 offen
        S_ENDPGM,
    ]);
    let granules = (lds * 4).div_ceil(512);
    NativeProgram {
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
            // six user SGPRs (s0 to s5), the three group ids after them, three thread ids, LDS
            rsrc2: 6 << 1 | 7 << 7 | 2 << 11 | granules << 15,
            threads: shape,
        }),
        code: words.iter().flat_map(|w| w.to_le_bytes()).collect(),
    }
}

/// The LDS use of the second program: every thread writes the group's number plus its own index
/// to its dword and reads the next thread's (the console's wave runs in step, so it needs no
/// barrier), which the record holds as its third dword.
fn neighbours() -> Vec<u32> {
    vec![
        vop2(0x1A, 9, 0, inline(2)), // v_lshlrev_b32 v9, 2, v0
        vop2(0x25, 4, 0, vgpr(3)),   // v_add_nc_u32 v4, v3, v0
        0x36 << 26 | 0x0D << 18,
        4 << 8 | 9,                     // ds_write_b32 v9, v4
        vop2(0x25, 10, 0, inline(1)),   // v_add_nc_u32 v10, 1, v0
        vop2(0x1B, 10, 10, inline(31)), // v_and_b32 v10, 31, v10
        vop2(0x1A, 10, 10, inline(2)),  // v_lshlrev_b32 v10, 2, v10
        0xBF8C_C07F,                    // s_waitcnt lgkmcnt(0)
        0x36 << 26 | 0x36 << 18,
        8 << 24 | 10,                    // ds_read_b32 v8, v10
        0xBF8C_C07F,                     // s_waitcnt lgkmcnt(0)
        0x7E00_0200 | 2 << 17 | vgpr(8), // v_mov_b32 v2, v8
    ]
}

/// `native` translated with `k` console groups to a host group (0: as the console runs them), on a
/// host of waves of 32.
fn translate(native: &NativeProgram, k: u32) -> Result<HostShader, ShaderError> {
    let p = ir::build(&native.code, native.wave64, native.vgprs).expect("ir::build");
    let st = structure::structure(&p).expect("structure");
    let res =
        resource::track(&p, &native.layout, native.stage, Some(&st)).expect("resource::track");
    let io = io::raise(&p, native).expect("io::raise");
    let was = lower::TEST_HOST_WAVE.replace(Some(32));
    let packing = lower::TEST_GROUP_PACK.replace(Some(k));
    let lowered = lower::lower(&lower::Parts {
        native,
        p: &p,
        st: &st,
        io: &io,
        res: &res,
        inputs: Vec::new(),
        outputs: Vec::new(),
        prims: Vec::new(),
        mesh_stride: 0,
    });
    lower::TEST_GROUP_PACK.set(packing);
    lower::TEST_HOST_WAVE.set(was);
    let lowered = lowered?;
    let shader = HostShader {
        stage: HwStage::Cs,
        dxil: lowered.dxil,
        user_data: lowered.user_data,
        samplers: lowered.samplers,
        static_samplers: Vec::new(),
        root_cbvs: lowered.root_cbvs,
        root_srvs: lowered.root_srvs,
        root_uavs: lowered.root_uavs,
        root_indexed_views: res.root_indexed_views.clone(),
        half_views: lowered.half_views,
    };
    if let Ok(mut v) = validator::Validator::load(validator::Validator::default_path().as_deref()) {
        v.validate(&mut shader.dxil.clone())
            .unwrap_or_else(|e| panic!("DXIL validation (pack {k}): {e}"));
    }
    Ok(shader)
}

/// The packing a translated program carries (the `dword` of its grid words).
fn packed_by(shader: &HostShader) -> Option<u32> {
    let words = |kind| {
        shader
            .user_data
            .iter()
            .find(|u| u.root == RootType::Other(kind))
    };
    let k = words(super::USER_PACK_GROUPS)?.dword;
    assert!(words(super::USER_PACK_X).is_some_and(|u| u.dword == k));
    assert!(words(super::USER_PACK_Y).is_some_and(|u| u.dword == k));
    Some(k)
}

/// What the console stores for `grid` groups of `shape` threads: a record per thread, `extra`
/// records of zeros past them.
fn expected(shape: [u32; 3], grid: [u32; 3], extra: usize, neighbour: bool) -> Vec<u32> {
    let mut out = Vec::new();
    for z in 0..grid[2] {
        for y in 0..grid[1] {
            for x in 0..grid[0] {
                let sig = x | y << 10 | z << 20;
                // (the records of the group: by thread index along x, then y, then z)
                let mut group = vec![[0u32; 4]; 32];
                for t in 0..32u32 {
                    let (tx, ty, tz) = (
                        t % shape[0],
                        t / shape[0] % shape[1],
                        t / shape[0] / shape[1],
                    );
                    // the neighbour is by thread x alone (the LDS program has a 1-D group)
                    let third = if neighbour { sig + (tx + 1) % 32 } else { tz };
                    group[t as usize] = [tx, ty, third, sig];
                }
                out.extend(group.into_iter().flatten());
            }
        }
    }
    out.extend(std::iter::repeat_n(0, extra * 4));
    out
}

/// Runs `shader` over the console `grid` and returns the records (`extra` zeros' worth past the
/// grid's) it stored.
fn run(host: &Host, shader: &HostShader, grid: [u32; 3], extra: usize) -> Vec<u32> {
    let (root, pso) = host
        .compute_pipeline(shader)
        .expect("the host makes the pipeline");
    let mut constants = vec![0u32; shader.user_data_dwords().max(1) as usize];
    for u in &shader.user_data {
        if u.root == RootType::Constants {
            constants[u.index as usize] = [grid[0], grid[1], 0, 0][u.dword as usize];
        }
    }
    let groups = match packed_by(shader) {
        Some(k) => {
            let (host, words) = pack_grid(grid, k);
            for (kind, value) in [
                (super::USER_PACK_X, words[0]),
                (super::USER_PACK_Y, words[1]),
                (super::USER_PACK_GROUPS, words[2]),
            ] {
                let u = shader
                    .user_data
                    .iter()
                    .find(|u| u.root == RootType::Other(kind))
                    .unwrap();
                constants[u.index as usize] = value;
            }
            host
        }
        None => grid,
    };
    let total = (grid[0] * grid[1] * grid[2]) as usize * 32 + extra;
    let out = host
        .dispatch(
            &root,
            &pso,
            &Dispatch {
                roots: vec![(0, Root::Constants(constants))],
                views: &[View::RawUav(total as u32 * 4)],
                samplers: &[],
                groups,
            },
        )
        .unwrap_or_else(|e| panic!("dispatch of {groups:?} for {grid:?}: {e}"));
    out[0].clone()
}

#[test]
fn the_programs_are_the_instructions_they_are_meant_to_be() {
    // the words above, as the decoder reads them
    let names = |native: &NativeProgram| -> Vec<String> {
        crate::gfx10::decode(&native.code)
            .unwrap()
            .iter()
            .map(|i| i.name().unwrap_or("?").to_string())
            .collect()
    };
    assert_eq!(
        names(&program([32, 1, 1], 0, &[])),
        [
            "s_mov_b32",
            "s_load_dwordx4",
            "s_mul_i32",
            "s_add_u32",
            "s_mul_i32",
            "s_add_u32",
            "s_lshl_b32",
            "s_lshl_b32",
            "s_lshl_b32",
            "s_or_b32",
            "s_or_b32",
            "v_mov_b32",
            "v_lshlrev_b32",
            "v_lshlrev_b32",
            "v_add_nc_u32",
            "v_lshlrev_b32",
            "v_add_nc_u32",
            "buffer_store_dwordx4",
            "s_endpgm"
        ]
    );
    let middle = names(&program([32, 1, 1], 32, &neighbours()));
    assert!(
        middle.iter().any(|n| n == "ds_write_b32") && middle.iter().any(|n| n == "ds_read_b32"),
        "{middle:?}"
    );
    assert_eq!(
        middle.iter().filter(|n| n.as_str() == "v_and_b32").count(),
        1
    );
}

#[test]
fn a_packed_program_names_its_grid_and_the_group_size_the_console_has() {
    let native = program([8, 4, 1], 0, &[]);
    let shader = translate(&native, 4).expect("translated");
    assert_eq!(packed_by(&shader), Some(4));
    // 128 threads in a group, with a wave of 32 lanes each
    let unpacked = translate(&native, 0).expect("translated");
    assert_eq!(packed_by(&unpacked), None);
    assert_ne!(shader.dxil, unpacked.dxil);
    // the shapes the program is not made for are left as they are
    for (shape, why) in [
        ([64, 1, 1], "a group of other than 32 threads"),
        ([16, 1, 1], "a group of other than 32 threads"),
    ] {
        let mut native = program(shape, 0, &[]);
        let StageIo::Cs(cs) = &mut native.io else {
            unreachable!()
        };
        cs.threads = shape;
        let shader = translate(&native, 4).expect("translated");
        assert_eq!(packed_by(&shader), None);
        assert_eq!(lower::take_pack_why(), Some(why));
    }
    // a console wave of 64, and a program with a group barrier
    let mut wave64 = program([32, 1, 1], 0, &[]);
    wave64.wave64 = true;
    assert_eq!(packed_by(&translate(&wave64, 4).expect("translated")), None);
    assert_eq!(lower::take_pack_why(), Some("a console wave64 program"));
    let mut barrier = program([32, 1, 1], 0, &[0xBF8A_0000]); // s_barrier
    barrier.vgprs = 16;
    assert_eq!(
        packed_by(&translate(&barrier, 4).expect("translated")),
        None
    );
    assert_eq!(lower::take_pack_why(), Some("a group barrier"));
}

#[test]
fn a_packed_program_runs_every_console_group_once_with_its_own_ids() {
    let _one = super::valu_tests::ONE_AT_A_TIME
        .lock()
        .unwrap_or_else(|e| e.into_inner());
    let Ok(host) = Host::new(false) else { return };
    for shape in [[32, 1, 1], [8, 4, 1], [4, 8, 1], [4, 4, 2], [2, 2, 8]] {
        let native = program(shape, 0, &[]);
        let plain = translate(&native, 0).expect("translated");
        for k in [2, 4, 8] {
            let packed = translate(&native, k).expect("translated");
            assert_eq!(packed_by(&packed), Some(k));
            // grids that leave a tail of the last host group, and of several dimensions
            for grid in [
                [1, 1, 1],
                [2, 1, 1],
                [3, 1, 1],
                [7, 1, 1],
                [8, 1, 1],
                [33, 1, 1],
                [3, 5, 1],
                [3, 5, 2],
                [4, 3, 3],
            ] {
                let want = expected(shape, grid, 64, false);
                assert_eq!(
                    run(&host, &plain, grid, 64),
                    want,
                    "unpacked, shape {shape:?}, grid {grid:?}"
                );
                assert_eq!(
                    run(&host, &packed, grid, 64),
                    want,
                    "packed {k}, shape {shape:?}, grid {grid:?}"
                );
            }
        }
    }
    eprintln!(
        "ran on the hardware device: 5 shapes, packed 2, 4 and 8, 9 grids, as the console runs them"
    );
}

#[test]
fn a_packed_program_keeps_each_console_groups_memory_to_itself() {
    let _one = super::valu_tests::ONE_AT_A_TIME
        .lock()
        .unwrap_or_else(|e| e.into_inner());
    let Ok(host) = Host::new(false) else { return };
    // 128 bytes a group: 4 groups take 512 of the 32 KiB, 8 groups 1 KiB
    let native = program([32, 1, 1], 32, &neighbours());
    let plain = translate(&native, 0).expect("translated");
    for k in [2, 4, 8] {
        let packed = translate(&native, k).expect("translated");
        assert_eq!(packed_by(&packed), Some(k));
        for grid in [[1, 1, 1], [5, 1, 1], [4, 3, 1], [9, 1, 2]] {
            let want = expected([32, 1, 1], grid, 32, true);
            assert_eq!(
                run(&host, &plain, grid, 32),
                want,
                "unpacked, grid {grid:?}"
            );
            assert_eq!(
                run(&host, &packed, grid, 32),
                want,
                "packed {k}, grid {grid:?}"
            );
        }
    }
    // (the same program with memory 8 groups cannot hold packs fewer groups: 32 KiB)
    let big = program([32, 1, 1], 4096, &neighbours());
    let shader = translate(&big, 8).expect("translated");
    assert_eq!(
        packed_by(&shader),
        Some(2),
        "8192 dwords of the 32 KiB hold two groups of 4096"
    );
    let shader = translate(&program([32, 1, 1], 8192, &neighbours()), 8).expect("translated");
    assert_eq!(packed_by(&shader), None);
    eprintln!("ran on the hardware device: LDS in slices, 4 grids, packed 2, 4 and 8");
}

#[test]
fn a_grid_past_what_one_dimension_holds_is_folded_and_run_once() {
    let _one = super::valu_tests::ONE_AT_A_TIME
        .lock()
        .unwrap_or_else(|e| e.into_inner());
    let Ok(host) = Host::new(false) else { return };
    let native = program([32, 1, 1], 0, &[]);
    let packed = translate(&native, 2).expect("translated");
    // 131,075 groups: 65,538 host groups, which need a second row
    let grid = [131_075, 1, 1];
    let (host_grid, _) = pack_grid(grid, 2);
    assert_eq!(host_grid, [65_535, 2, 1]);
    let got = run(&host, &packed, grid, 64);
    let want = expected([32, 1, 1], grid, 64, false);
    assert_eq!(got.len(), want.len());
    let bad = got.iter().zip(&want).position(|(g, w)| g != w);
    assert_eq!(bad, None, "the first record that differs, in dwords");
}

/// A program that reads another lane of its console wave: the group's number plus the thread's
/// index, of lane 5, which every thread of the group stores as the third dword.
fn lane_five() -> Vec<u32> {
    vec![
        vop2(0x25, 4, 0, vgpr(3)), // v_add_nc_u32 v4, v3, v0
        0xD760_0015,
        vgpr(4) | inline(5) << 9, // v_readlane_b32 s21, v4, 5
        0x7E00_0200 | 2 << 17 | 21, // v_mov_b32 v2, s21
    ]
}

#[test]
fn a_packed_program_reads_the_lanes_of_its_own_console_group() {
    let _one = super::valu_tests::ONE_AT_A_TIME.lock().unwrap_or_else(|e| e.into_inner());
    let Ok(host) = Host::new(false) else { return };
    let native = program([32, 1, 1], 0, &lane_five());
    let names: Vec<String> = crate::gfx10::decode(&native.code)
        .unwrap()
        .iter()
        .map(|i| i.name().unwrap_or("?").to_string())
        .collect();
    assert!(names.iter().any(|n| n == "v_readlane_b32"), "{names:?}");
    let plain = translate(&native, 0).expect("translated");
    let want = |grid: [u32; 3]| {
        let mut out = expected([32, 1, 1], grid, 32, false);
        // (the third dword: the group's number of lane 5)
        for record in out.chunks_mut(4).take((grid[0] * grid[1] * grid[2]) as usize * 32) {
            record[2] = record[3] + 5;
        }
        out
    };
    for k in [2, 4, 8] {
        let packed = translate(&native, k).expect("translated");
        assert_eq!(packed_by(&packed), Some(k));
        for grid in [[1, 1, 1], [5, 1, 1], [4, 3, 1], [9, 1, 2]] {
            assert_eq!(run(&host, &plain, grid, 32), want(grid), "unpacked, grid {grid:?}");
            assert_eq!(run(&host, &packed, grid, 32), want(grid), "packed {k}, grid {grid:?}");
        }
    }
}

#[test]
fn the_grid_of_a_dispatch_is_divided_and_the_threads_find_their_groups() {
    assert_eq!(pack_grid([1, 1, 1], 4), ([1, 1, 1], [1, 1, 1]));
    assert_eq!(pack_grid([5, 1, 1], 4), ([2, 1, 1], [5, 1, 5]));
    assert_eq!(pack_grid([3, 5, 2], 2), ([15, 1, 1], [3, 5, 30]));
    assert_eq!(pack_grid([0, 7, 1], 4), ([0, 1, 1], [0, 7, 0]));
    assert_eq!(
        pack_grid([262_140, 1, 1], 4),
        ([65_535, 1, 1], [262_140, 1, 262_140])
    );
    assert_eq!(
        pack_grid([262_141, 1, 1], 4),
        ([65_535, 2, 1], [262_141, 1, 262_141])
    );
    assert_eq!(pack_grid([65_535, 65_535, 1], 8).0, [65_535, 8192, 1]);
    // every console group has one thread block, and none has two, whatever the grid
    for (grid, k) in [
        ([1, 1, 1], 2),
        ([7, 1, 1], 4),
        ([3, 5, 2], 8),
        ([131_075, 1, 1], 2),
        ([300, 400, 2], 4),
    ] {
        let (host, words) = pack_grid(grid, k);
        assert_eq!(words, [grid[0], grid[1], grid[0] * grid[1] * grid[2]]);
        let mut seen = HashSet::new();
        for hy in 0..host[1] {
            for hx in 0..host[0] {
                for slot in 0..k {
                    if let Some(g) = pack_group(grid, k, [hx, hy], slot * 32 + 5) {
                        assert!(
                            g[0] < grid[0] && g[1] < grid[1] && g[2] < grid[2],
                            "{g:?} of {grid:?}"
                        );
                        assert!(seen.insert(g), "{g:?} twice in {grid:?} packed {k}");
                        // (every thread of the slot is in the same group)
                        assert_eq!(pack_group(grid, k, [hx, hy], slot * 32 + 31), Some(g));
                    }
                }
            }
        }
        assert_eq!(
            seen.len() as u32,
            grid[0] * grid[1] * grid[2],
            "{grid:?} packed {k}"
        );
    }
    assert_eq!(pack_group([3, 5, 2], 4, [0, 0], 0), Some([0, 0, 0]));
    assert_eq!(pack_group([3, 5, 2], 4, [7, 0], 0), Some([1, 4, 1]));
    assert_eq!(pack_group([3, 5, 2], 4, [7, 0], 2 * 32), None);
    assert_eq!(pack_group([3, 5, 2], 4, [1, 0], 32), Some([2, 1, 0]));
}

#[test]
fn the_switch_names_the_largest_packing_it_covers() {
    for (value, k) in [
        (None, 0),
        (Some(""), 0),
        (Some("0"), 0),
        (Some("1"), 0),
        (Some("2"), 2),
        (Some("3"), 2),
        (Some("4"), 4),
        (Some(" 5 "), 4),
        (Some("8"), 8),
        (Some("64"), 8),
        (Some("many"), 0),
    ] {
        assert_eq!(lower::parse_group_pack(value), k, "{value:?}");
    }
}

#[test]
fn the_shader_cache_key_holds_the_switch() {
    // the build script lists every `LEONIDA_*` switch the translator reads, whose value joins
    // the key (`translation_fingerprint`)
    assert!(
        env!("LEONIDA_SHADER_SWITCHES")
            .split(',')
            .any(|n| n == "LEONIDA_GROUP_PACK")
    );
}
