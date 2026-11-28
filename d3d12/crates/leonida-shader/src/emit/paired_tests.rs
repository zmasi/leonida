//! The paired form (`paired.rs`, [`super::paired`]) of console wave64 compute programs on a host
//! of 32-lane waves: hand-written fixtures of what the form must keep exact, each lowered in the
//! paired form, validated by `dxil.dll` when present, and run on the host GPU when it takes waves
//! of 32 (NVIDIA). The expected values are what the console computes. Most fixtures are from
//! TaynanGT/Vice-Tay 5e588e8 `wave64_after_loop.rs` (GPL-3.0; the loop after darkworldhm 08fdf0b,
//! the encoders after fxceless/victory-fork #993), the others this tree's own wave64 and water
//! fixtures, which the other forms could not run on a 32-lane host.

use super::host::{Dispatch, Host, Root, View};
use super::wave64_tests::native_of;
use super::{HostShader, PairedLanes, lower, validator};
use crate::native::{NativeProgram, RootType, StageIo};
use crate::{dxbc, io, resource, structure};

const EXEC: u32 = 126;
const LIT: u32 = 255;

const fn v(n: u32) -> u32 {
    256 + n
}

const fn k(n: u32) -> u32 {
    128 + n
}

const fn sop1(op: u32, sdst: u32, src0: u32) -> u32 {
    0xBE80_0000 | sdst << 16 | op << 8 | src0
}

const fn sop2(op: u32, sdst: u32, src0: u32, src1: u32) -> u32 {
    0x8000_0000 | op << 23 | sdst << 16 | src1 << 8 | src0
}

/// A branch by `imm` dwords from the next instruction.
const fn sopp(op: u32, imm: i16) -> u32 {
    0xBF80_0000 | op << 16 | imm as u16 as u32
}

const fn vop1(op: u32, vdst: u32, src0: u32) -> u32 {
    0x7E00_0000 | vdst << 17 | op << 9 | src0
}

const fn vop2(op: u32, vdst: u32, src0: u32, vsrc1: u32) -> u32 {
    op << 25 | vdst << 17 | vsrc1 << 9 | src0
}

const fn vopc(op: u32, src0: u32, vsrc1: u32) -> u32 {
    0x7C00_0000 | op << 17 | vsrc1 << 9 | src0
}

const fn vop3(op: u32, dst: u32, src0: u32, src1: u32) -> [u32; 2] {
    [0xD400_0000 | op << 16 | dst, src0 | src1 << 9]
}

/// `buffer_store_dword v<data>, v<addr>, s[8:11], 0 offen offset:<offset>`
const fn store(data: u32, addr: u32, offset: u32) -> [u32; 2] {
    [0xE070_1000 | offset, 0x8002_0000 | data << 8 | addr]
}

/// `v<regs>` stored as each thread's four dwords of the UAV, from the thread's index (v0) times 16.
fn stores(regs: [u32; 4]) -> Vec<u32> {
    let mut w = vec![vop2(0x1A, 15, k(4), 0)]; // v_lshlrev_b32 v15, 4, v0
    for (n, r) in regs.into_iter().enumerate() {
        w.extend(store(r, 15, 4 * n as u32));
    }
    w
}

/// A console wave64 program of `threads` threads in one dimension (the thread index in v0), with
/// 512 bytes of LDS when `lds`.
fn program(body: &[u32], threads: u32, lds: bool) -> NativeProgram {
    let mut native = native_of(body, true, threads);
    if lds && let StageIo::Cs(cs) = &mut native.io {
        cs.rsrc2 |= 1 << 15;
    }
    native
}

/// `native` in the paired form for a host of 32-lane waves, validated when `dxil.dll` is there.
fn paired(native: &NativeProgram) -> HostShader {
    let p = crate::spill::build(native).expect("build");
    let bytes = dxbc::build(&[]);
    let c = dxbc::Container::parse(&bytes).expect("container");
    let was = lower::TEST_HOST_WAVE.replace(Some(32));
    let shader = super::paired(&c, native, &p, false);
    lower::TEST_HOST_WAVE.set(was);
    let (shader, wave) = shader.expect("the paired form takes the program");
    assert_eq!(wave, Some(32));
    if let Ok(mut v) = validator::Validator::load(validator::Validator::default_path().as_deref()) {
        let mut copy = shader.dxil.clone();
        v.validate(&mut copy).expect("DXIL validation failed");
    }
    shader
}

/// Runs `shader` in one group with the root constants `s0`, `s1` and gives the first `dwords`
/// dwords of the UAV; None (and a line) without a device that takes it.
fn on_the_card(shader: &HostShader, constants: [u32; 2], dwords: u32) -> Option<Vec<u32>> {
    let Ok(host) = Host::new(false) else {
        eprintln!("no SM 6.6 hardware device; not run");
        return None;
    };
    let (root, pso) = match host.compute_pipeline(shader) {
        Ok(x) => x,
        Err(e) => {
            eprintln!("the hardware device does not take this program: {e}; not run");
            return None;
        }
    };
    let mut ud = vec![0u32; shader.user_data_dwords().max(1) as usize];
    for u in &shader.user_data {
        if let (RootType::Constants, 0, d @ 0..=1) = (u.root, u.slot, u.dword) {
            ud[u.index as usize] = constants[d as usize];
        }
    }
    let out = host
        .dispatch(
            &root,
            &pso,
            &Dispatch {
                roots: vec![(0, Root::Constants(ud))],
                views: &[View::RawUav(dwords)],
                samplers: &[],
                groups: [1, 1, 1],
            },
        )
        .unwrap_or_else(|e| panic!("dispatch failed: {e}"));
    Some(out[0][..dwords as usize].to_vec())
}

/// `native` in the paired form, run with each case's root constants and compared with what the
/// console stores (four dwords a thread); whether it ran.
fn runs_as_the_console(
    native: &NativeProgram,
    cases: &[(u32, u32)],
    console: impl Fn(u32, u32) -> Vec<u32>,
) -> bool {
    let shader = paired(native);
    let StageIo::Cs(cs) = native.io else {
        unreachable!()
    };
    let threads = cs.threads.iter().product::<u32>();
    let _one = super::valu_tests::ONE_AT_A_TIME
        .lock()
        .unwrap_or_else(|e| e.into_inner());
    for &(s0, s1) in cases {
        // (twice: a group of a later dispatch finds what the first left in the LDS)
        for _ in 0..2 {
            let Some(out) = on_the_card(&shader, [s0, s1], 4 * threads) else {
                return false;
            };
            let want = console(s0, s1);
            if out != want {
                let at = out.iter().zip(&want).position(|(a, b)| a != b).unwrap_or(0);
                panic!(
                    "s0 {s0}, s1 {s1}: thread {} dword {} is {:#x}, the console's {:#x}",
                    at / 4,
                    at % 4,
                    out[at],
                    want[at]
                );
            }
        }
    }
    true
}

#[test]
fn the_switch_reads_its_modes() {
    assert_eq!(super::parse_paired_lanes(None), PairedLanes::BeforeWide);
    assert_eq!(
        super::parse_paired_lanes(Some("1")),
        PairedLanes::BeforeWide
    );
    assert_eq!(super::parse_paired_lanes(Some("0")), PairedLanes::Off);
    assert_eq!(
        super::parse_paired_lanes(Some(" first ")),
        PairedLanes::First
    );
    assert_eq!(super::parse_paired_lanes(Some("exact")), PairedLanes::Exact);
    // (each mode is another translation: the shader cache keys on its number)
    assert_eq!(PairedLanes::Exact as u8, 3);
}

#[test]
fn the_lds_operations_the_paired_form_takes() {
    assert!(lower::supports_paired_lds("ds_add_u32"));
    assert!(lower::supports_paired_lds("ds_or_b32"));
    assert!(lower::supports_paired_lds("ds_read2_b32"));
    assert!(!lower::supports_paired_lds("ds_add_rtn_u32"));
    assert!(!lower::supports_paired_lds("ds_add_f32"));
    assert!(!lower::supports_paired_lds("ds_read_b64"));
}

/// One console wave of 64 threads. Lane t goes round a loop `(t & s0) + 1` times, adding
/// `t & s1` to its sum each round; the loop is the compiler's usual one for lanes that leave
/// apart (a mask of the lanes still going round, the wave's branch back while some lane is
/// left). After it, over the lanes whose `t & s1` is not 0: their count (`s_bcnt1_i32_b64`),
/// the first of them (`v_readfirstlane_b32` of the thread index) and each one's number among
/// them (`v_mbcnt_lo/hi`). Every thread stores its sum, its number (0x55 where it has none),
/// the count and the first.
fn after_loop_program(threads: u32) -> NativeProgram {
    let mut w = vec![
        vop2(0x1B, 4, 0, 0), // v_and_b32 v4, s0, v0: the lane's last round
        vop2(0x1B, 5, 1, 0), // v_and_b32 v5, s1, v0: what the lane adds a round
        vop1(0x01, 7, k(0)), // v_mov_b32 v7, 0
        vop1(0x01, 8, LIT),  // v_mov_b32 v8, 0x55
        0x55,
        sop1(0x03, 6, k(0)),      // s_mov_b32 s6, 0: the round
        sop1(0x04, 2, EXEC),      // s_mov_b64 s[2:3], exec
        sop1(0x04, 12, EXEC),     // s_mov_b64 s[12:13], exec: the lanes still going round
        sop1(0x04, 14, EXEC),     // s_mov_b64 s[14:15], exec
        sop2(0x0F, EXEC, 14, 12), // loop: s_and_b64 exec, s[14:15], s[12:13]
        vop2(0x25, 7, v(5), 7),   // v_add_nc_u32 v7, v5, v7
        sop1(0x04, EXEC, 14),     // s_mov_b64 exec, s[14:15]
        sop2(0x02, 6, 6, k(1)),   // s_add_i32 s6, s6, 1
    ];
    w.extend(vop3(0xC4, 14, 6, v(4))); // v_cmp_gt_u32 s[14:15], s6, v4: the lanes that are done
    w.extend([
        sop1(0x24, 14, 14),       // s_and_saveexec_b64 s[14:15], s[14:15]
        sop2(0x15, 12, 12, EXEC), // s_andn2_b64 s[12:13], s[12:13], exec
        sopp(0x05, -9),           // s_cbranch_scc1 loop
        sop1(0x04, EXEC, 2),      // s_mov_b64 exec, s[2:3]
        vopc(0xD5, k(0), 5),      // v_cmpx_ne_u32 exec, 0, v5
        sop1(0x10, 7, EXEC),      // s_bcnt1_i32_b64 s7, exec
        vop1(0x02, 16, v(0)),     // v_readfirstlane_b32 s16, v0
    ]);
    w.extend(vop3(0x365, 8, EXEC, k(0))); // v_mbcnt_lo_u32_b32 v8, exec_lo, 0
    w.extend(vop3(0x366, 8, EXEC + 1, v(8))); // v_mbcnt_hi_u32_b32 v8, exec_hi, v8
    w.extend([
        sop1(0x04, EXEC, 2), // s_mov_b64 exec, s[2:3]
        vop1(0x01, 9, 7),    // v_mov_b32 v9, s7
        vop1(0x01, 10, 16),  // v_mov_b32 v10, s16
    ]);
    w.extend(stores([7, 8, 9, 10]));
    program(&w, threads, false)
}

/// What the console's lanes of [`after_loop_program`] store with the root constants `s0`, `s1`.
fn after_loop_console(threads: u32, s0: u32, s1: u32) -> Vec<u32> {
    (0..threads)
        .flat_map(|t| {
            let first = t / 64 * 64;
            let counted: Vec<u32> = (first..first + 64).filter(|t| t & s1 != 0).collect();
            let number = if t & s1 != 0 {
                counted.iter().filter(|&&u| u < t).count() as u32
            } else {
                0x55
            };
            [
                (t & s1) * ((t & s0) + 1),
                number,
                counted.len() as u32,
                counted[0],
            ]
        })
        .collect()
}

#[test]
fn lane_counts_first_lanes_and_ranks_after_a_loop_lanes_leave_apart() {
    let cases = [(0, 1), (3, 1), (7, 5), (24, 63), (7, 32), (7, 16), (63, 9)];
    if runs_as_the_console(&after_loop_program(64), &cases, |s0, s1| {
        after_loop_console(64, s0, s1)
    }) {
        eprintln!(
            "ran on the hardware device: counts, first lanes and ranks over both halves after the loop"
        );
    }
}

/// Every lane writes its thread index + 1 to LDS dword `lane`, then reads the dword of lane `t ^ 32`
/// (the other half of its console wave) with no barrier between (the console's wave is in step),
/// and its rank (`v_mbcnt` of all lanes). In a group of two console waves (128 threads) each
/// wave reads its own half's partner.
fn lds_between_halves(threads: u32) -> NativeProgram {
    let mut body = vec![
        vop2(0x1A, 1, k(2), 0), // v_lshlrev_b32 v1, 2, v0: this lane's byte address
        vop2(0x25, 2, k(1), 0), // v_add_nc_u32 v2, 1, v0
        0xD834_0000,
        0x0000_0201,             // ds_write_b32 v1, v2
        vop2(0x1D, 3, k(32), 0), // v_xor_b32 v3, 32, v0: the partner
        vop2(0x1A, 3, k(2), 3),
        0xD8D8_0000,
        0x0400_0003, // ds_read_b32 v4, v3
    ];
    body.extend(vop3(0x365, 5, 193, k(0))); // v_mbcnt_lo_u32_b32 v5, -1, 0
    body.extend(vop3(0x366, 5, 193, v(5))); // v_mbcnt_hi_u32_b32 v5, -1, v5
    body.extend(stores([4, 5, 2, 0]));
    program(&body, threads, true)
}

#[test]
fn lds_written_by_one_half_is_read_by_the_other_without_a_barrier() {
    for threads in [64u32, 128] {
        let ran = runs_as_the_console(&lds_between_halves(threads), &[(0, 0)], |_, _| {
            (0..threads)
                .flat_map(|t| [(t ^ 32) + 1, t % 64, t + 1, t])
                .collect()
        });
        if !ran {
            return;
        }
    }
    eprintln!(
        "ran on the hardware device: each lane read what the other half of its console wave wrote"
    );
}

/// `ds_write2_b32` of two dwords a lane, `ds_read2_b32` of the partner's (`t ^ 32`).
#[test]
fn lds_pairs_written_and_read_across_the_halves() {
    let mut body = vec![
        vop2(0x1A, 1, k(3), 0),  // v_lshlrev_b32 v1, 3, v0
        vop2(0x25, 2, k(1), 0),  // v2 = t + 1
        vop2(0x25, 3, k(64), 0), // v3 = t + 64
        0xD838_0100,
        0x0003_0201,             // ds_write2_b32 v1, v2, v3 offset1:1
        vop2(0x1D, 4, k(32), 0), // v_xor_b32 v4, 32, v0
        vop2(0x1A, 4, k(3), 4),
        0xD8DC_0001,
        0x0700_0004, // ds_read2_b32 v[7:8], v4 offset0:1
    ];
    body.extend(stores([7, 8, 2, 3]));
    let ran = runs_as_the_console(&program(&body, 64, true), &[(0, 0)], |_, _| {
        (0..64)
            .flat_map(|t| [(t ^ 32) + 64, (t ^ 32) + 1, t + 1, t + 64])
            .collect()
    });
    if ran {
        eprintln!("ran on the hardware device: pairs of dwords across the halves");
    }
}

/// The water's tiles: lane 0 stores a starting maximum (0) and minimum (-1); the lanes whose
/// `t ^ s1` is below s0 go on (the others skip, `s_cbranch_execz`) and `ds_max_u32` and `ds_min_u32`
/// (`add`: `ds_add_u32`) their key `t + 1` in; every lane reads both back.
fn masked_atomics(add: bool) -> NativeProgram {
    let mut body = vec![
        vop2(0x1A, 1, k(3), 0), // v_lshlrev_b32 v1, 3, v0
        vop1(0x01, 2, k(0)),    // v_mov_b32 v2, 0
        vop1(0x01, 3, 193),     // v_mov_b32 v3, -1
        0xD838_0100,
        0x0003_0201, // ds_write2_b32 v1, v2, v3 offset1:1 (lane 0's are the tile's)
        vop1(0x01, 4, k(0)), // v_mov_b32 v4, 0
        vop2(0x25, 5, k(1), 0), // v5 = t + 1
        vop2(0x1D, 6, 1, 0), // v_xor_b32 v6, s1, v0
        sop1(0x04, 2, EXEC), // s_mov_b64 s[2:3], exec
        vopc(0xD4, 0, 6), // v_cmpx_gt_u32 exec, s0, v6
        sopp(0x08, 4), // s_cbranch_execz past the atomics
        if add { 0xD800_0004 } else { 0xD81C_0004 },
        0x0000_0504, // ds_add_u32 / ds_min_u32 v4, v5 offset:4
        0xD820_0000,
        0x0000_0504,         // ds_max_u32 v4, v5
        sop1(0x04, EXEC, 2), // s_mov_b64 exec, s[2:3]
        0xD8DC_0100,
        0x0700_0004, // ds_read2_b32 v[7:8], v4 offset1:1
    ];
    body.extend(stores([7, 8, 0, 5]));
    program(&body, 64, true)
}

const MASKED_CASES: [(u32, u32); 6] = [(0, 0), (1, 0), (31, 0), (32, 32), (33, 32), (64, 0)];

#[test]
fn the_tiles_minimum_and_maximum_of_the_lanes_that_go_on() {
    let ran = runs_as_the_console(&masked_atomics(false), &MASKED_CASES, |limit, xor| {
        let on = (0..64).filter(|t| (t ^ xor) < limit).map(|t| t + 1);
        let max = on.clone().max().unwrap_or(0);
        let min = on.min().unwrap_or(u32::MAX);
        (0..64).flat_map(|t| [max, min, t, t + 1]).collect()
    });
    if ran {
        eprintln!("ran on the hardware device: the tile's minimum and maximum over both halves");
    }
}

#[test]
fn a_count_added_by_the_lanes_that_go_on() {
    let ran = runs_as_the_console(&masked_atomics(true), &MASKED_CASES, |limit, xor| {
        let on = (0..64).filter(|t| (t ^ xor) < limit).map(|t| t + 1);
        let max = on.clone().max().unwrap_or(0);
        let sum = on.fold(u32::MAX, |s, x: u32| s.wrapping_add(x));
        (0..64).flat_map(|t| [max, sum, t, t + 1]).collect()
    });
    if ran {
        eprintln!("ran on the hardware device: the lanes' adds over both halves");
    }
}

/// A wave OR of `(t >> 5) + 1` (the lower half's bit 0, the upper half's bit 1: the compiler's
/// DPP tree, [`super::wave::WAVE_OR_V7_TO_S7`]) and `v_readlane_b32` of lane 37.
#[test]
fn a_wave_or_and_a_lane_read_over_both_halves() {
    let mut body = vec![vop2(0x16, 7, k(5), 0), vop2(0x25, 7, k(1), 7)]; // v7 = (t >> 5) + 1
    body.extend(super::wave::WAVE_OR_V7_TO_S7);
    body.extend(vop3(0x360, 16, v(0), k(37))); // v_readlane_b32 s16, v0, 37
    body.extend([vop1(0x01, 9, 7), vop1(0x01, 10, 16)]);
    body.extend(stores([9, 10, 0, 0]));
    if runs_as_the_console(&program(&body, 64, false), &[(0, 0)], |_, _| {
        (0..64).flat_map(|t| [3, 37, t, t]).collect()
    }) {
        eprintln!("ran on the hardware device: the wave's OR and lane 37");
    }
}

/// `ds_bpermute_b32` of `t + 1` from lane `(t ^ 33) + offset / 4` of the same half, with only the
/// lanes below s0 in EXEC: an inactive source gives 0, an inactive lane keeps its 17.
#[test]
fn a_backward_permute_stays_in_its_half_and_reads_exec_there() {
    for offset in [0u32, 4, 128] {
        let mut body = vec![
            sop1(0x04, 2, EXEC),     // s_mov_b64 s[2:3], exec
            vop2(0x25, 2, k(1), 0),  // v2 = t + 1
            vop2(0x1D, 3, k(33), 0), // v_xor_b32 v3, 33, v0
            vop2(0x1A, 3, k(2), 3),
            vop1(0x01, 4, k(17)), // v_mov_b32 v4, 17
            vopc(0xD4, 0, 0),     // v_cmpx_gt_u32 exec, s0, v0
            0xDACC_0000 | offset,
            0x0400_0203,         // ds_bpermute_b32 v4, v3, v2 offset:<offset>
            sop1(0x04, EXEC, 2), // s_mov_b64 exec, s[2:3]
        ];
        body.extend(stores([4, 0, 2, 0]));
        let cases = [(0, 0), (1, 0), (31, 0), (32, 0), (33, 0), (63, 0), (64, 0)];
        let ran = runs_as_the_console(&program(&body, 64, false), &cases, |limit, _| {
            (0..64)
                .flat_map(|t| {
                    let partner = (t & 32) | (((t ^ 33) + offset / 4) & 31);
                    let value = if t >= limit {
                        17
                    } else if partner >= limit {
                        0
                    } else {
                        partner + 1
                    };
                    [value, t, t + 1, t]
                })
                .collect()
        });
        if !ran {
            return;
        }
    }
    eprintln!("ran on the hardware device: permutes within each half, EXEC per half");
}

/// The water's tile program ([`super::water_tests::tile_program`]): another exact form takes it
/// (without its skips), so `LEONIDA_PAIRED_LANES` unset leaves it alone; with `first` `emit`
/// takes it in the paired form, which gives the console's minimum and maximum whichever thread
/// returned early.
#[test]
fn the_water_tile_program_in_the_paired_form_first() {
    let native = super::water_tests::tile_program(0, false, true);
    let p = crate::spill::build(&native).expect("build");
    let st = structure::structure(&p).expect("structure");
    let res =
        resource::track(&p, &native.layout, native.stage, Some(&st)).expect("resource::track");
    let io = io::raise(&p, &native).expect("io::raise");
    let bytes = dxbc::build(&[]);
    let c = dxbc::Container::parse(&bytes).expect("container");
    let was = lower::TEST_HOST_WAVE.replace(Some(32));
    let mut syncs = Vec::new();
    let mut shaders = Vec::new();
    for mode in [PairedLanes::First, PairedLanes::BeforeWide] {
        let was_mode = super::TEST_PAIRED_LANES.replace(Some(mode));
        let taken = super::TEST_PAIRED_TAKEN.get();
        lower::TEST_GROUP_SYNCS.set(0);
        let shader = super::emit(&c, &native, &p, &st, &io, &res).expect("emit");
        syncs.push(lower::TEST_GROUP_SYNCS.get());
        assert_eq!(
            super::TEST_PAIRED_TAKEN.get() - taken,
            u32::from(mode == PairedLanes::First),
            "{mode:?}"
        );
        super::TEST_PAIRED_LANES.set(was_mode);
        shaders.push(shader);
    }
    lower::TEST_HOST_WAVE.set(was);
    eprintln!(
        "group barriers made: {} in the paired form, {} in the form taken without it",
        syncs[0], syncs[1]
    );
    let shader = &shaders[0];
    if let Ok(mut v) = validator::Validator::load(validator::Validator::default_path().as_deref()) {
        let mut copy = shader.dxil.clone();
        v.validate(&mut copy).expect("DXIL validation failed");
    }
    let _one = super::valu_tests::ONE_AT_A_TIME
        .lock()
        .unwrap_or_else(|e| e.into_inner());
    for returned in [0u32, 5, 32, 63, 64] {
        let keys: Vec<u32> = (0..64).filter(|&t| t != returned).map(|t| t + 10).collect();
        let (max, min) = (*keys.iter().max().unwrap(), *keys.iter().min().unwrap());
        let want: Vec<u32> = (0..64u32)
            .flat_map(|t| if t == returned { [0, 0] } else { [max, min] })
            .collect();
        for _ in 0..2 {
            let Some(out) = on_the_card(shader, [returned, 0], 128) else {
                return;
            };
            assert_eq!(out, want, "thread {returned} returned early");
        }
    }
    eprintln!("ran on the hardware device: the paired tile program for every early return");
}

/// The console's 16-bit wave sum (`v_add_f16` in the compiler's tree, each addition rounded):
/// the paired form gives it bit for bit, in the whole wave and where the lanes at or past s0
/// have left the program (they add +0). The other forms run neither on a 32-lane host.
#[test]
fn a_sum_of_halves_is_the_consoles_tree_with_and_without_lanes_gone() {
    let terms = [
        0x340C_0085, // v_lshlrev_b32 v6, 5, v0
        0x340E_0082, // v_lshlrev_b32 v7, 2, v0
        0x4A0E_0F06, // v_add_nc_u32 v7, v6, v7
        0x4A0E_0F00, // v_add_nc_u32 v7, v0, v7
        0x4A0E_0EFF,
        0x0000_3C00, // v_add_nc_u32 v7, 0x3c00, v7
    ];
    let mut words = vec![
        0x7DA8_0000, // v_cmpx_gt_u32 exec, s0, v0: the lanes below s0 stay
        0,           // s_cbranch_execz to the end (filled in below)
    ];
    words.extend(terms);
    // (the wave OR's words with v_add_f16 for v_or_b32: opcode 50 for 28)
    words.extend(super::wave::WAVE_OR_V7_TO_S7.iter().map(|&w| {
        if w >> 25 == 28 {
            w & 0x01FF_FFFF | 50 << 25
        } else {
            w
        }
    }));
    words.extend([
        0x7E00_0207, // v_mov_b32 v0, s7
        0x38 << 26 | 0x1C << 18,
        0x80 << 24 | 2 << 16, // buffer_store_dword v0, off, s[8:11], 0
    ]);
    words[1] = 0xBF88_0000 | (words.len() as u32 - 2);
    let shader = paired(&program(&words, 64, false));
    let wide = |h: u16| {
        if h == 0 {
            0.0
        } else {
            f32::from_bits((u32::from(h >> 10) + 112) << 23 | u32::from(h & 0x3FF) << 13)
        }
    };
    let narrow = |f: f32| {
        if f == 0.0 {
            return 0;
        }
        let b = f.to_bits();
        let h = ((b >> 23) - 112) << 10 | (b >> 13 & 0x3FF);
        let rest = b & 0x1FFF;
        (h + u32::from(rest > 0x1000 || rest == 0x1000 && h & 1 == 1)) as u16
    };
    let add = |a: u16, b: u16| narrow(wide(a) + wide(b));
    let _one = super::valu_tests::ONE_AT_A_TIME
        .lock()
        .unwrap_or_else(|e| e.into_inner());
    for stay in [64u32, 63, 40, 33, 32, 17, 5, 1] {
        let mut tree: Vec<u16> = (0..64u32)
            .map(|t| {
                if t < stay {
                    (0x3C00 + 37 * t) as u16
                } else {
                    0
                }
            })
            .collect();
        while tree.len() > 1 {
            tree = tree.chunks(2).map(|c| add(c[0], c[1])).collect();
        }
        let Some(got) = on_the_card(&shader, [stay, 0], 1) else {
            return;
        };
        // (the high half is lane 0's term's: zero)
        assert_eq!(
            got[0],
            u32::from(tree[0]),
            "{stay} lanes stay: got {:#x}, the console's tree {:#x}",
            got[0],
            tree[0]
        );
    }
    eprintln!("ran on the hardware device: the console's 16-bit tree, lanes gone or not");
}

/// "Some lane is left" of a mask operation as a number (`v_mbcnt` ranks, `s_and_b64` with EXEC,
/// `s_cselect`, `s_orn2_b64`'s SCC), as the upscaler's wait loop takes it: the paired form asks
/// both halves.
#[test]
fn some_lane_of_a_mask_is_a_number_the_whole_wave_agrees_on() {
    let native = program(
        &[
            0xD765_0001,
            0x0001_00C1, // v_mbcnt_lo_u32_b32 v1, -1, 0
            0xD766_0001,
            0x0002_02C1, // v_mbcnt_hi_u32_b32 v1, -1, v1
            0x7D84_0200, // v_cmp_eq_u32 vcc, s0, v1
            0x8792_7E6A, // s_and_b64 s[18:19], vcc, exec
            0x8592_80C1, // s_cselect_b64 s[18:19], -1, 0
            0xBF09_8501, // s_cmp_ge_u32 s1, 5
            0x8594_80C1, // s_cselect_b64 s[20:21], -1, 0
            0x8B92_1214, // s_orn2_b64 s[18:19], s[20:21], s[18:19]
            0x8506_8081, // s_cselect_b32 s6, 1, 0
            0x7E04_0206, // v_mov_b32 v2, s6
            0xE070_0000,
            0x8002_0200, // buffer_store_dword v2, off, s[8:11], 0
        ],
        64,
        false,
    );
    let shader = paired(&native);
    let _one = super::valu_tests::ONE_AT_A_TIME
        .lock()
        .unwrap_or_else(|e| e.into_inner());
    // (lane s0 waits, if there is one; s1 rounds)
    for (wait, rounds, done) in [
        (0, 0, 0),
        (37, 4, 0),
        (63, 4, 0),
        (64, 0, 1),
        (37, 5, 1),
        (64, 9, 1),
    ] {
        let Some(out) = on_the_card(&shader, [wait, rounds], 1) else {
            return;
        };
        assert_eq!(out[0], done, "lane {wait} after {rounds} rounds");
    }
    eprintln!("ran on the hardware device: the wave's answer over both halves");
}

/// The console threads of an 8 x 8 group of one console wave: each host thread is two of them,
/// (x, y) of `t` and of `t + 32`. Thread (x, y) stores its rank (`v_mbcnt`) at dword 8y + x.
#[test]
fn the_console_threads_of_an_8_by_8_group_are_each_given_once() {
    let mut body = vec![
        vop2(0x1A, 2, k(3), 1), // v_lshlrev_b32 v2, 3, v1
        vop2(0x25, 2, v(2), 0), // v_add_nc_u32 v2, v2, v0: 8y + x
        vop2(0x1A, 2, k(2), 2), // v_lshlrev_b32 v2, 2, v2
    ];
    body.extend(vop3(0x365, 3, 193, k(0))); // v_mbcnt_lo_u32_b32 v3, -1, 0
    body.extend(vop3(0x366, 3, 193, v(3))); // v_mbcnt_hi_u32_b32 v3, -1, v3
    body.extend(store(3, 2, 0));
    let mut native = program(&body, 64, false);
    if let StageIo::Cs(cs) = &mut native.io {
        cs.threads = [8, 8, 1];
        cs.rsrc2 |= 1 << 11; // thread ids x and y
    }
    let shader = paired(&native);
    let _one = super::valu_tests::ONE_AT_A_TIME
        .lock()
        .unwrap_or_else(|e| e.into_inner());
    let Some(out) = on_the_card(&shader, [0, 0], 64) else {
        return;
    };
    // the console's lane of (x, y) is 8y + x
    assert_eq!(out, (0..64).collect::<Vec<u32>>());
    eprintln!(
        "ran on the hardware device: 64 of 64 console threads of the 8 x 8 group, each in its lane"
    );
}

/// `LEONIDA_PAIRED_LANES=exact` (after aki-gta6/aki-runtime fe9c736): a console wave's OR taken
/// inside a lane branch ([`super::wave64_tests::program`] with `s_cbranch_scc0`) is no form's
/// that is exact without a group barrier, so by default each host half-wave is given an OR of
/// its own (`local waves (APPROXIMATE)`); with `exact` the paired form takes it first, where the
/// OR is of the console wave's 64 lanes. With `0` and by default the paired form is not asked.
/// (No card: which form translates the program. What the paired form computes for it is
/// `a_wave_or_and_a_lane_read_over_both_halves`.)
#[test]
fn the_paired_form_before_the_approximate_ones_when_asked() {
    let native = super::wave64_tests::program(0xBF84_0000);
    let p = crate::spill::build(&native).expect("build");
    let st = structure::structure(&p).expect("structure");
    let res =
        resource::track(&p, &native.layout, native.stage, Some(&st)).expect("resource::track");
    let io = io::raise(&p, &native).expect("io::raise");
    let bytes = dxbc::build(&[]);
    let c = dxbc::Container::parse(&bytes).expect("container");
    let was = lower::TEST_HOST_WAVE.replace(Some(32));
    let mut shaders = Vec::new();
    for (mode, paired) in [
        (PairedLanes::BeforeWide, 0),
        (PairedLanes::Exact, 1),
        (PairedLanes::Off, 0),
    ] {
        let was_mode = super::TEST_PAIRED_LANES.replace(Some(mode));
        let taken = super::TEST_PAIRED_TAKEN.get();
        let shader = super::emit(&c, &native, &p, &st, &io, &res).expect("emit");
        assert_eq!(super::TEST_PAIRED_TAKEN.get() - taken, paired, "{mode:?}");
        super::TEST_PAIRED_LANES.set(was_mode);
        shaders.push(shader);
    }
    lower::TEST_HOST_WAVE.set(was);
    // the paired program is another one; without the form the translation is the default's
    assert_ne!(shaders[1].dxil, shaders[0].dxil);
    assert_eq!(shaders[2].dxil, shaders[0].dxil);
    if let Ok(mut v) = validator::Validator::load(validator::Validator::default_path().as_deref()) {
        let mut copy = shaders[1].dxil.clone();
        v.validate(&mut copy).expect("DXIL validation failed");
    }
}
