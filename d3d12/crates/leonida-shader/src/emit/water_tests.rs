//! The water's tile programs (`CS_TransmissionVolumetrics`: 8 x 8 groups of one console wave64 with
//! 512 bytes of LDS) on a host of 32-lane waves, as hand-written fixtures of the constructs that
//! made them untranslatable or wrong: per-lane early returns around LDS that is written and read
//! (the program is taken wave-wide, [`super::widen`]), and the tile's starting minimum and
//! maximum stored by thread (0, 0) after those returns (stored by every thread at the start
//! instead, [`super::lds_init`]). Lowered, validated by `dxil.dll` when present, and run on the
//! host GPU when it takes waves of 32. The expected values are what the console computes.

use super::host::Host;
use super::wave64_tests::{native_of, whole_uav};
use super::{HostShader, lds_init, lower, validator};
use crate::native::{ComputeIo, NativeProgram, StageIo};
use crate::{dxbc, io, ir, resource, structure};

const fn vop1(op: u32, vdst: u32, src0: u32) -> u32 {
    0x7E00_0000 | vdst << 17 | op << 9 | src0
}

const fn vop2(op: u32, vdst: u32, src0: u32, vsrc1: u32) -> u32 {
    op << 25 | vdst << 17 | vsrc1 << 9 | src0
}

const fn vopc(op: u32, src0: u32, vsrc1: u32) -> u32 {
    0x7C00_0000 | op << 17 | vsrc1 << 9 | src0
}

/// DS `op` with its data and address VGPRs (`vdst` for a read), the two byte offsets.
const fn ds(op: u32, vdst: u32, addr: u32, data0: u32, data1: u32, offset0: u32, offset1: u32) -> [u32; 2] {
    [0x36 << 26 | op << 18 | offset1 << 8 | offset0, vdst << 24 | data1 << 16 | data0 << 8 | addr]
}

const DS_WRITE_B32: u32 = 0x0D;
const DS_WRITE2_B32: u32 = 0x0E;
const DS_MIN_U32: u32 = 0x07;
const DS_MAX_U32: u32 = 0x08;
const DS_READ2_B32: u32 = 0x37;

const V: u32 = 256;
const fn k(n: u32) -> u32 {
    128 + n
}
const K_M1: u32 = 193;
const S_WAITCNT_LGKM0: u32 = 0xBF8C_C07F;
const S_AND_SAVEEXEC_VCC: u32 = 0xBE8C_246A; // s_and_saveexec_b64 s[12:13], vcc
const S_RESTORE_EXEC: u32 = 0xBEFE_040C; // s_mov_b64 exec, s[12:13]
const SAVE_EXEC_14: u32 = 0xBE8E_047E; // s_mov_b64 s[14:15], exec
const RESTORE_EXEC_14: u32 = 0xBEFE_040E; // s_mov_b64 exec, s[14:15]

/// `buffer_store_dword v<data>, v<addr>, s[8:11], 0 offen offset:<offset>`.
const fn store(data: u32, addr: u32, offset: u32) -> [u32; 2] {
    [0x38 << 26 | 0x1C << 18 | 1 << 12 | offset, 0x80 << 24 | 2 << 16 | data << 8 | addr]
}

/// One 8 x 8 console wave64 (thread ids in v0 and v1) with 512 bytes of LDS. Every thread but the
/// one whose number is s0 goes on (the others return early, `s_cbranch_execz` past the rest); of
/// those the one at (0, 0), if it is among them, stores the tile's starting maximum (0, dword 0)
/// and minimum (-1, dword 1), then every thread `ds_max_u32`s and `ds_min_u32`s its key (its
/// number + 10) in and reads both back, and stores them at `out[2 * number]` (max) and
/// `out[2 * number + 1]` (min). No barrier: the console runs one wave.
///
/// `max_start` replaces the 0; `stray`: a write at an address that is not a constant;
/// `atomics: false` leaves them out.
pub(super) fn tile_program(max_start: u32, stray: bool, atomics: bool) -> NativeProgram {
    let mut after = vec![
        vop2(0x1C, 3, V + 1, 0), // v_or_b32 v3, v1, v0
        SAVE_EXEC_14,
        vopc(0xD2, k(0), 3), // v_cmpx_eq_u32 exec, 0, v3
        vop1(1, 4, k(0)),    // v_mov_b32 v4, 0
        vop1(1, 5, K_M1),    // v_mov_b32 v5, -1
    ];
    let start = if max_start != 0 {
        after.extend([vop1(1, 10, 0xFF), max_start]); // v_mov_b32 v10, lit
        10
    } else {
        4
    };
    after.extend(ds(DS_WRITE2_B32, 0, 4, start, 5, 0, 1)); // ds_write2_b32 v4, v<start>, v5 offset1:1
    after.push(RESTORE_EXEC_14);
    after.extend([
        vop2(0x25, 6, k(10), 2), // v_add_nc_u32 v6, 10, v2
        vop1(1, 4, k(0)),        // v_mov_b32 v4, 0
    ]);
    if stray {
        after.extend(ds(DS_WRITE_B32, 0, 2, 6, 0, 0, 0)); // ds_write_b32 v2, v6: any address
    }
    if atomics {
        after.extend(ds(DS_MIN_U32, 0, 4, 6, 0, 4, 0)); // ds_min_u32 v4, v6 offset:4
        after.extend(ds(DS_MAX_U32, 0, 4, 6, 0, 0, 0)); // ds_max_u32 v4, v6
    }
    after.extend([S_WAITCNT_LGKM0]);
    after.extend(ds(DS_READ2_B32, 7, 4, 0, 0, 0, 1)); // ds_read2_b32 v[7:8], v4 offset1:1
    after.extend([
        S_WAITCNT_LGKM0,
        vop2(0x1A, 9, k(3), 2), // v_lshlrev_b32 v9, 3, v2
    ]);
    after.extend(store(7, 9, 0));
    after.extend(store(8, 9, 4));
    let mut body = vec![
        vop2(0x1A, 2, k(3), 1),  // v_lshlrev_b32 v2, 3, v1
        vop2(0x25, 2, V + 2, 0), // v_add_nc_u32 v2, v2, v0: the thread's number
        vopc(0xC5, 0, 2),        // v_cmp_ne_u32 vcc, s0, v2
        S_AND_SAVEEXEC_VCC,
        0xBF88_0000 | after.len() as u32, // s_cbranch_execz to the restore of EXEC
    ];
    body.extend(after);
    body.push(S_RESTORE_EXEC);
    let mut native = native_of(&body, true, 64);
    native.io = StageIo::Cs(ComputeIo {
        rsrc2: 5 << 1 | 1 << 11 | 1 << 15, // 5 user SGPRs, thread ids x and y, 512 bytes of LDS
        threads: [8, 8, 1],
    });
    native
}

fn hoists(native: &NativeProgram) -> Vec<(u32, u32)> {
    let p = crate::spill::build(native).expect("build");
    lds_init::init_hoists(&p, 128)
}

#[test]
fn accumulator_starts_are_hoisted() {
    assert_eq!(hoists(&tile_program(0, false, true)), [(0, 0), (1, u32::MAX)]);
    // a start that is not the maximum's identity stays where it is
    assert_eq!(hoists(&tile_program(5, false, true)), [(1, u32::MAX)]);
    // plain stores (a flag, data) are not accumulators
    assert_eq!(hoists(&tile_program(0, false, false)), []);
    // a write that could land anywhere: nothing is known
    assert_eq!(hoists(&tile_program(0, true, true)), []);
}

/// The program translated for a host of 32-lane waves by `emit`, which takes its early returns
/// wave-wide (the other forms refuse LDS that is read and written where not every lane runs).
fn translated(native: &NativeProgram, hoist: bool) -> HostShader {
    let p = crate::spill::build(native).expect("build");
    let st = structure::structure(&p).expect("structure");
    let res = resource::track(&p, &native.layout, native.stage, Some(&st)).expect("resource::track");
    let io = io::raise(&p, native).expect("io::raise");
    let bytes = dxbc::build(&[]);
    let c = dxbc::Container::parse(&bytes).expect("container");
    let was = lower::TEST_HOST_WAVE.replace(Some(32));
    let was_hoist = lds_init::TEST_HOIST.replace(Some(hoist));
    let shader = super::emit(&c, native, &p, &st, &io, &res);
    lds_init::TEST_HOIST.set(was_hoist);
    lower::TEST_HOST_WAVE.set(was);
    let shader = shader.expect("the tile program translates for 32-lane waves");
    if let Ok(mut v) = validator::Validator::load(validator::Validator::default_path().as_deref()) {
        let mut copy = shader.dxil.clone();
        v.validate(&mut copy).expect("DXIL validation failed");
    }
    shader
}

/// What the console leaves in the UAV: `(max, min)` of the keys of the threads that went on, at
/// every one of them; nothing for the thread that returned.
fn expected(returned: u32) -> Vec<u32> {
    let keys: Vec<u32> = (0..64).filter(|&t| t != returned).map(|t| t + 10).collect();
    let (max, min) = (*keys.iter().max().unwrap(), *keys.iter().min().unwrap());
    (0..64u32).flat_map(|t| if t == returned { [0, 0] } else { [max, min] }).collect()
}

#[test]
fn a_tile_whose_corner_thread_returned_early_still_starts_its_minimum_and_maximum() {
    let native = tile_program(0, false, true);
    let shader = translated(&native, true);
    let _one = super::valu_tests::ONE_AT_A_TIME.lock().unwrap_or_else(|e| e.into_inner());
    if Host::new(false).is_err() {
        eprintln!("no SM 6.6 hardware device; not run");
        return;
    }
    // 0: the corner returns (the minimum is the next thread's key); 5 and 63: another does (the
    // maximum drops for 63); 64: none
    for returned in [0u32, 5, 32, 63, 64] {
        // (twice: the second group of a card finds what the first left in the LDS)
        for _ in 0..2 {
            let Some(out) = whole_uav(&shader, [returned, 0], 128) else {
                return;
            };
            assert_eq!(out, expected(returned), "thread {returned} returned early");
        }
    }
    eprintln!("ran on the hardware device: the tile's minimum and maximum for every early return");
}

#[test]
fn the_tile_program_keeps_working_with_the_hoist_off_when_its_corner_stays() {
    // what the hoist changes is only the tile whose corner returned: with the corner in, the
    // program's own store does the job
    let native = tile_program(0, false, true);
    let shader = translated(&native, false);
    let _one = super::valu_tests::ONE_AT_A_TIME.lock().unwrap_or_else(|e| e.into_inner());
    if Host::new(false).is_err() {
        eprintln!("no SM 6.6 hardware device; not run");
        return;
    }
    for returned in [5u32, 63, 64] {
        let Some(out) = whole_uav(&shader, [returned, 0], 128) else {
            return;
        };
        assert_eq!(out, expected(returned), "thread {returned} returned early");
    }
}

// ---- the culling program of 1,024 threads (6b82bc7d22016cb5): LDS in lane-branch regions ----

const fn sop1(op: u32, sdst: u32, ssrc0: u32) -> u32 {
    0xBE80_0000 | sdst << 16 | op << 8 | ssrc0
}

const fn sop2(op: u32, sdst: u32, ssrc0: u32, ssrc1: u32) -> u32 {
    0x8000_0000 | op << 23 | sdst << 16 | ssrc1 << 8 | ssrc0
}

/// VOP3 `v_mbcnt_lo_u32_b32` (`hi: false`) or `_hi_` of the two sources (operand codes).
const fn mbcnt(hi: bool, vdst: u32, src0: u32, src1: u32) -> [u32; 2] {
    [0xD765_0000 + ((hi as u32) << 16) | vdst, src0 | src1 << 9]
}

const VCC: u32 = 106;
const EXEC: u32 = 126;
const S_BARRIER: u32 = 0xBF8A_0000;
const DS_READ_B32: u32 = 0x36;
const DS_ADD_RTN_U32: u32 = 0x20;

/// Two console waves (128 threads in one dimension, 1 KiB of LDS): `head`, `cmp` (a VOPC into
/// vcc against v0), `s_and_saveexec_b64 s[12:13], vcc`, `region` up to the restore of EXEC, then
/// `tail`.
fn two_waves(head: &[u32], cmp: u32, region: &[u32], tail: &[u32]) -> NativeProgram {
    let mut body: Vec<u32> = head.to_vec();
    body.extend([cmp, S_AND_SAVEEXEC_VCC, 0xBF88_0000 | region.len() as u32]);
    body.extend(region);
    body.push(S_RESTORE_EXEC);
    body.extend(tail);
    let mut native = native_of(&body, true, 128);
    native.io = StageIo::Cs(ComputeIo { rsrc2: 5 << 1 | 2 << 15, threads: [128, 1, 1] });
    native
}

/// `exact`: the test's choice for [`super::lds_exact`] (`None`: as it is).
fn translated_for_32(native: &NativeProgram, exact: Option<bool>) -> Result<HostShader, crate::ShaderError> {
    let p = crate::spill::build(native).expect("build");
    let st = structure::structure(&p).expect("structure");
    let res = resource::track(&p, &native.layout, native.stage, Some(&st)).expect("resource::track");
    let io = io::raise(&p, native).expect("io::raise");
    let bytes = dxbc::build(&[]);
    let c = dxbc::Container::parse(&bytes).expect("container");
    let was = lower::TEST_HOST_WAVE.replace(Some(32));
    let was_exact = super::lds_exact::TEST_EXACT.replace(exact);
    let shader = super::emit(&c, native, &p, &st, &io, &res);
    super::lds_exact::TEST_EXACT.set(was_exact);
    lower::TEST_HOST_WAVE.set(was);
    if let Ok(shader) = &shader
        && let Ok(mut v) = validator::Validator::load(validator::Validator::default_path().as_deref())
    {
        let mut copy = shader.dxil.clone();
        v.validate(&mut copy).expect("DXIL validation failed");
    }
    shader
}

/// The threads under `count` (or thread 0 alone: `None`) write LDS dword 2, then read it, add
/// one and write it back three times; after a barrier every thread reads it and stores it with
/// its lane (`v_mbcnt`: what makes the program one of console lanes).
fn read_modify_write_program(count: Option<u32>) -> NativeProgram {
    let mut region = vec![vop1(1, 4, k(0)), vop1(1, 5, 0xFF), 41]; // v4 = 0, v5 = 41
    region.extend(ds(DS_WRITE_B32, 0, 4, 5, 0, 8, 0));
    for _ in 0..3 {
        region.push(S_WAITCNT_LGKM0);
        region.extend(ds(DS_READ_B32, 6, 4, 0, 0, 8, 0));
        region.extend([S_WAITCNT_LGKM0, vop2(0x25, 6, k(1), 6)]);
        region.extend(ds(DS_WRITE_B32, 0, 4, 6, 0, 8, 0));
    }
    let mut tail = vec![S_WAITCNT_LGKM0, S_BARRIER, vop1(1, 4, k(0))];
    tail.extend(ds(DS_READ_B32, 7, 4, 0, 0, 8, 0));
    tail.push(S_WAITCNT_LGKM0);
    tail.extend(mbcnt(false, 9, K_M1, k(0)));
    tail.extend(mbcnt(true, 10, K_M1, V + 9));
    tail.push(vop2(0x1A, 11, k(3), 0)); // v11 = thread * 8
    tail.extend(store(7, 11, 0));
    tail.extend(store(10, 11, 4));
    let cmp = match count {
        None => vopc(0xC2, k(0), 0),    // v_cmp_eq_u32 vcc, 0, v0
        Some(n) => vopc(0xC4, k(n), 0), // v_cmp_gt_u32 vcc, n, v0
    };
    two_waves(&[], cmp, &region, &tail)
}

#[test]
fn a_region_one_thread_runs_may_read_and_write_its_own_lds() {
    let native = read_modify_write_program(None);
    let shader = translated_for_32(&native, None).expect("a one-thread region translates for 32-lane waves");
    let _one = super::valu_tests::ONE_AT_A_TIME.lock().unwrap_or_else(|e| e.into_inner());
    if Host::new(false).is_err() {
        eprintln!("no SM 6.6 hardware device; not run");
        return;
    }
    let Some(out) = whole_uav(&shader, [0, 0], 256) else {
        return;
    };
    for t in 0..128usize {
        assert_eq!(out[2 * t], 44, "thread {t}");
        assert_eq!(out[2 * t + 1], (t % 64) as u32, "thread {t}'s lane");
    }
    eprintln!("ran on the hardware device: 128 threads read the 44 thread 0 made of 41");
}

#[test]
fn a_region_two_lanes_run_gets_the_consoles_order_from_a_barrier_per_access() {
    // threads 0 and 1 are two lanes of one console wave, which reads what both wrote: no
    // one-thread region, but the form without the skip makes the region's blocks ones every
    // lane runs, and every LDS access in them gets its own group barrier
    let native = read_modify_write_program(Some(2));
    let shader = translated_for_32(&native, None).expect("translates for 32-lane waves");
    let _one = super::valu_tests::ONE_AT_A_TIME.lock().unwrap_or_else(|e| e.into_inner());
    if Host::new(false).is_err() {
        eprintln!("no SM 6.6 hardware device; not run");
        return;
    }
    let Some(out) = whole_uav(&shader, [0, 0], 256) else {
        return;
    };
    for t in 0..128usize {
        assert_eq!(out[2 * t], 44, "thread {t}");
    }
}

/// Each odd thread whose bit of a word of LDS is set takes a slot in a list by the allocation
/// idiom: its wave's first lane adds the wave's count to the counter in LDS dword 1
/// (`ds_add_rtn_u32`) and the lanes take the base it returned plus their rank among the odd
/// lanes. The region also reads the bitset word (bytes 16 and up) at an address of its own
/// whose range does not meet the counter's. Every thread stores its number at `out[slot]`.
fn allocation_program() -> NativeProgram {
    let mut head = vec![
        vop2(0x1A, 2, k(2), 0), // v_lshlrev_b32 v2, 2, v0
        vop2(0x1B, 2, 0xFF, 2), // v_and_b32 v2, lit, v2
        0x1FC,
        vop1(1, 3, K_M1), // v_mov_b32 v3, -1
    ];
    head.extend(ds(DS_WRITE_B32, 0, 2, 3, 0, 16, 0)); // the bitset: every bit set
    head.push(vop1(1, 4, k(0)));
    head.extend(ds(DS_WRITE_B32, 0, 4, 4, 0, 4, 0)); // the counter: 0
    head.extend([
        S_WAITCNT_LGKM0,
        S_BARRIER,
        vop2(0x1B, 5, k(1), 0), // v_and_b32 v5, 1, v0: odd threads
    ]);
    head.extend(ds(DS_READ_B32, 6, 2, 0, 0, 16, 0)); // their bitset word (all lanes)
    head.extend([
        S_WAITCNT_LGKM0,
        vop2(0x1B, 7, V + 6, 5), // v_and_b32 v7, v6, v5
    ]);
    let cmp = vopc(0xC5, k(0), 7); // v_cmp_ne_u32 vcc, 0, v7
    let mut region = vec![
        sop1(0x14, 16, EXEC), // s_ff1_i32_b64 s16, exec
        sop1(4, 18, EXEC),    // s_mov_b64 s[18:19], exec
    ];
    region.extend(ds(DS_READ_B32, 8, 2, 0, 0, 16, 0)); // (in the region: the bitset word again)
    region.push(S_WAITCNT_LGKM0);
    region.extend(mbcnt(false, 9, K_M1, k(0)));
    region.push(sop2(0x0F, 20, VCC, EXEC)); // s_and_b64 s[20:21], vcc, exec
    region.extend(mbcnt(true, 10, K_M1, V + 9));
    region.extend(mbcnt(false, 11, 20, k(0)));
    region.extend(mbcnt(true, 12, 21, V + 11));
    region.extend([
        vop1(1, 13, k(0)),  // v_mov_b32 v13, 0
        vopc(0xD2, 16, 10), // v_cmpx_eq_u32 exec, s16, v10: the first lane
        sop1(0x10, 22, 20), // s_bcnt1_i32_b64 s22, s[20:21]
        vop1(1, 13, k(0)),  // v_mov_b32 v13, 0
        vop1(1, 14, 22),    // v_mov_b32 v14, s22
    ]);
    region.extend(ds(DS_ADD_RTN_U32, 13, 13, 14, 0, 4, 0)); // ds_add_rtn_u32 v13, v13, v14 offset:4
    region.extend([
        sop1(4, EXEC, 18), // s_mov_b64 exec, s[18:19]
        S_WAITCNT_LGKM0,
        vop1(2, 23, V + 13),      // v_readfirstlane_b32 s23, v13
        vop2(0x25, 15, 23, 12),   // v_add_nc_u32 v15, s23, v12: the slot
        vop2(0x1A, 16, k(2), 15), // v_lshlrev_b32 v16, 2, v15
    ]);
    region.extend(store(0, 16, 0)); // out[slot] = the thread's number
    two_waves(&head, cmp, &region, &[])
}

#[test]
fn a_wave_allocation_through_an_lds_counter_gives_every_thread_a_slot_of_its_own() {
    let native = allocation_program();
    let shader = translated_for_32(&native, None).expect("the allocation translates for 32-lane waves");
    let _one = super::valu_tests::ONE_AT_A_TIME.lock().unwrap_or_else(|e| e.into_inner());
    if Host::new(false).is_err() {
        eprintln!("no SM 6.6 hardware device; not run");
        return;
    }
    // (the order of the slots between waves is the order the waves' atomics came in: any)
    for _ in 0..8 {
        let Some(out) = whole_uav(&shader, [0, 0], 128) else {
            return;
        };
        let mut got: Vec<u32> = out[..64].to_vec();
        got.sort_unstable();
        let want: Vec<u32> = (0..128).filter(|t| t % 2 == 1).collect();
        assert_eq!(got, want, "the odd threads' numbers, each once, in slots 0..64");
        assert!(out[64..].iter().all(|&x| x == 0), "nothing past the 64 slots");
    }
    eprintln!("ran on the hardware device: 64 odd threads, 64 slots, each once");
}

/// The allocation of [`allocation_program`] in a loop the lanes leave one by one, as the
/// culling program's is (the lanes of the loop's mask walk its items; the wave goes on while
/// some lane has one): the odd lanes allocate once for each of `(thread & 3) + 1` rounds and
/// store `thread * 8 + round`. The loop keeps the region from being cut into blocks every lane
/// runs.
fn allocation_loop_program() -> NativeProgram {
    let mut head = vec![
        vop2(0x1A, 2, k(2), 0), // v_lshlrev_b32 v2, 2, v0
        vop2(0x1B, 2, 0xFF, 2), // v_and_b32 v2, lit, v2
        0x1FC,
        vop1(1, 3, K_M1), // v_mov_b32 v3, -1
    ];
    head.extend(ds(DS_WRITE_B32, 0, 2, 3, 0, 16, 0));
    head.push(vop1(1, 4, k(0)));
    head.extend(ds(DS_WRITE_B32, 0, 4, 4, 0, 4, 0));
    head.extend([S_WAITCNT_LGKM0, S_BARRIER, vop2(0x1B, 5, k(1), 0)]); // v5 = odd
    let cmp = vopc(0xC5, k(0), 5); // v_cmp_ne_u32 vcc, 0, v5
    let mut init = vec![
        sop1(4, 14, EXEC),      // s_mov_b64 s[14:15], exec: the lanes in the loop
        vop2(0x1B, 20, k(3), 0), // v_and_b32 v20, 3, v0
        vop2(0x25, 20, k(1), 20), // v_add_nc_u32 v20, 1, v20: its rounds
        vop1(1, 21, k(0)),      // v_mov_b32 v21, 0: its round
        0xBF82_0001,            // s_branch past the top of the loop
    ];
    let top = vec![sop2(0x0F, EXEC, 26, 14)]; // s_and_b64 exec, s[26:27], s[14:15]
    let mut body = vec![sop1(0x14, 16, EXEC), sop1(4, 18, EXEC)]; // s_ff1_i32_b64 s16, exec; s_mov_b64 s[18:19], exec
    body.extend(ds(DS_READ_B32, 8, 2, 0, 0, 16, 0));
    body.push(S_WAITCNT_LGKM0);
    body.extend(mbcnt(false, 9, K_M1, k(0)));
    body.push(sop1(4, 20 + 2, EXEC)); // s_mov_b64 s[22:23], exec: the lanes that allocate
    body.extend(mbcnt(true, 10, K_M1, V + 9));
    body.extend(mbcnt(false, 11, 22, k(0)));
    body.extend(mbcnt(true, 12, 23, V + 11));
    body.extend([
        vop1(1, 13, k(0)),
        vopc(0xD2, 16, 10),
        sop1(0x10, 24, 22), // s_bcnt1_i32_b64 s24, s[22:23]
        vop1(1, 13, k(0)),
        vop1(1, 14, 24),
    ]);
    body.extend(ds(DS_ADD_RTN_U32, 13, 13, 14, 0, 4, 0));
    body.extend([
        sop1(4, EXEC, 18),
        S_WAITCNT_LGKM0,
        vop1(2, 25, V + 13),      // v_readfirstlane_b32 s25, v13
        vop2(0x25, 15, 25, 12),   // v_add_nc_u32 v15, s25, v12: the slot
        vop2(0x1A, 16, k(2), 15), // v_lshlrev_b32 v16, 2, v15
        vop2(0x1A, 17, k(3), 0),  // v_lshlrev_b32 v17, 3, v0
        vop2(0x25, 17, V + 17, 21), // v_add_nc_u32 v17, v17, v21
    ]);
    body.extend(store(17, 16, 0));
    body.extend([
        vop2(0x25, 21, k(1), 21), // v_add_nc_u32 v21, 1, v21
        sop1(4, 26, EXEC),        // s_mov_b64 s[26:27], exec
        vopc(0xD6, V + 21, 20),   // v_cmpx_ge_u32 exec, v21, v20: the lanes that are through
        sop2(0x15, 14, 14, EXEC), // s_andn2_b64 s[14:15], s[14:15], exec
    ]);
    let mut region = std::mem::take(&mut init);
    region.extend(top);
    region.extend(body);
    // the branch's target is the top of the loop: its offset counts from the dword after it
    let top_at = 5; // the dwords of `init`
    region.push(0);
    let branch_at = region.len() - 1;
    let offset = (top_at as i32 - (branch_at as i32 + 1)) as i16 as u16 as u32;
    region[branch_at] = 0xBF85_0000 | offset; // s_cbranch_scc1 to the top
    two_waves(&head, cmp, &region, &[])
}

#[test]
fn a_wave_allocation_in_a_loop_lanes_leave_one_by_one_gives_every_round_a_slot_of_its_own() {
    let native = allocation_loop_program();
    let shader = translated_for_32(&native, None).expect("the allocation loop translates for 32-lane waves");
    let _one = super::valu_tests::ONE_AT_A_TIME.lock().unwrap_or_else(|e| e.into_inner());
    if Host::new(false).is_err() {
        eprintln!("no SM 6.6 hardware device; not run");
        return;
    }
    for _ in 0..8 {
        let Some(out) = whole_uav(&shader, [0, 0], 256) else {
            return;
        };
        let mut got: Vec<u32> = out[..192].to_vec();
        got.sort_unstable();
        let mut want: Vec<u32> = (0..128u32)
            .filter(|t| t % 2 == 1)
            .flat_map(|t| (0..(t & 3) + 1).map(move |round| t * 8 + round))
            .collect();
        want.sort_unstable();
        assert_eq!(got, want, "every (thread, round) once in slots 0..192");
        assert!(out[192..].iter().all(|&x| x == 0), "nothing past the 192 slots");
    }
    eprintln!("ran on the hardware device: 192 allocations by 64 threads in loops of 2 and 4 rounds");
}

/// The blocks of `native` that hold an instruction called `name`.
fn blocks_with(p: &ir::Program, name: &str) -> Vec<usize> {
    (0..p.blocks.len()).filter(|&b| p.blocks[b].insts.iter().any(|i| i.name() == name)).collect()
}

#[test]
fn the_regions_that_need_no_order_between_the_halves_are_told_from_those_that_do() {
    use super::lds_exact::{single_thread_blocks, with_exact};
    let exact_region = |p: &ir::Program, region: &[usize], single: &[bool], rtn: bool| {
        with_exact(|| super::lds_exact::exact_region(p, region, single, rtn))
    };
    // thread 0 alone: its region is one thread's
    let native = read_modify_write_program(None);
    let p = crate::spill::build(&native).unwrap();
    let st = structure::structure(&p).unwrap();
    let single = single_thread_blocks(&p, &st, [128, 1, 1]);
    let region = blocks_with(&p, "ds_write_b32");
    assert!(!region.is_empty() && region.iter().all(|&b| single[b]), "{region:?} {single:?}");
    assert_eq!(exact_region(&p, &region, &single, false), Ok(()));
    // (not in a group of two dimensions, where `v0 == 0` is a row)
    assert!(single_thread_blocks(&p, &st, [8, 16, 1]).iter().all(|&s| !s));
    // the threads below 2: no one thread, and one dword read and written
    let native = read_modify_write_program(Some(2));
    let p = crate::spill::build(&native).unwrap();
    let st = structure::structure(&p).unwrap();
    let single = single_thread_blocks(&p, &st, [128, 1, 1]);
    let region = blocks_with(&p, "ds_write_b32");
    assert!(region.iter().all(|&b| !single[b]));
    assert_eq!(
        exact_region(&p, &region, &single, true),
        Err("an LDS range read and written in one region")
    );
    // the allocation: a read from 16 up and an atomic at 4 never meet; the atomic that returns
    // the old value only in the form that takes the idiom
    let native = allocation_program();
    let p = crate::spill::build(&native).unwrap();
    let st = structure::structure(&p).unwrap();
    let single = single_thread_blocks(&p, &st, [128, 1, 1]);
    let region = blocks_with(&p, "ds_add_rtn_u32");
    assert_eq!(region.len(), 1);
    assert_eq!(exact_region(&p, &region, &single, true), Ok(()));
    assert_eq!(exact_region(&p, &region, &single, false), Err("an atomic that returns the old value"));
}
