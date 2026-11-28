"""Wave-wide reductions and lane-spilled masks under --wave64-emulate, run on all 64 lanes of one workgroup.

Two sequences a compiler emits for a wave64, which under emulation only come out right when the two 32-lane halves
agree on every scalar:

1. A reduction over the wave whose result one lane commits to LDS. EXEC is widened (`s_or_saveexec_b64 ..., -1`),
   lanes that were off get the identity (`v_cndmask_b32`), four DPP steps reduce each 16-lane row (`quad_perm`
   [1,0,3,2] and [2,3,0,1], `row_half_mirror`, `row_mirror`), `v_permlanex16_b32` joins the two rows of each half,
   `v_readlane_b32` of lane 0 is folded into every lane, and `v_readlane_b32` of lane 63 is then the wave's result.
   EXEC is narrowed to the first lane that was on (`s_ff1_i32_b64`, `s_lshl_b64 ..., 1, sN`, `s_and_b64 exec` or
   `s_and_saveexec_b64`), and that one lane does `ds_or_b32` (an OR reduction) or `ds_min_u32` (an unsigned MIN).
   The low half cannot see lane 63 and the high half cannot see lane 0 or the first active lane without an exchange.
2. A 64-bit lane mask kept across a region in two lanes of a VGPR (an SGPR spill): `v_writelane_b32` of each half
   of the pair, the SGPRs reused for other masks and numbers, `v_readlane_b32` back, then `s_mov_b64 exec` and
   `v_cndmask_b32` on the reloaded pair.

Each program is translated twice, without and with --wave64-emulate, and run: natively by
`spirv_mini_interp.run_workgroup` as one 64-wide subgroup, emulated by
`test_wave64_emulate_opt.run_emulated_workgroup` (the Workgroup exchange buffer, strict uniform barriers). Both
must give every lane the words of a plain-Python reference. A counter in LDS that `ds_add_u32` raises next to each
commit shows that exactly one lane ran it, which an idempotent OR or MIN would hide.

`run_emulated_workgroup` executed only the OR, add, AND, XOR and exchange atomics, so a program with `ds_min_u32`
could not be run under emulation at all (`NotImplementedError: unsupported op OpAtomicUMin`). It now runs
`OpAtomicUMin`, `UMax`, `SMin`, `SMax` and `ISub`; `EmulatedAtomicKinds` runs the five LDS instructions behind them
on all 64 lanes against the native result.

Hand-written, with made-up values: no game data.

Run from the repository root:  python -I tests/semantic/test_wave64_emulate_wave_reduce.py
"""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import spirv_mini_interp as si  # noqa: E402
import test_wave64_emulate_opt as emulate_opt_tests  # noqa: E402

run_emulated_workgroup = emulate_opt_tests.run_emulated_workgroup
_translate_encoded = emulate_opt_tests._translate_encoded
VSHARP = emulate_opt_tests.VSHARP
MASK32 = emulate_opt_tests.MASK32

LANES = 64
ROW = " row_mask:0xf bank_mask:0xf fi:1"
DPP_STEPS = ("quad_perm:[1,0,3,2]", "quad_perm:[2,3,0,1]", "row_half_mirror", "row_mirror")


def run_both(name, insns):
    """{mode: memory} for the program translated natively and with --wave64-emulate, each run on 64 lanes."""
    out = {}
    for mode, emulate in (("native", False), ("emulated", True)):
        spv, _ = _translate_encoded(f"{name}_{mode[0]}", insns, (LANES, 1, 1), emulate=emulate)
        if emulate:
            memory, _ = run_emulated_workgroup(spv, (LANES, 1, 1), push_constants=VSHARP + [0] * 28)
        else:
            memory = {}
            si.run_workgroup(spv, LANES, LANES,
                             inputs_for=lambda i: {"%wg_id": (0, 0, 0), "%local_id": (i, 0, 0), "%sg_lane": i},
                             push_constants=VSHARP + [0] * 28, memory=memory)
        out[mode] = memory
    return out


def reduce_insns(lo, hi, count_commits):
    """Both reductions under EXEC = lanes lo <= lane < hi. Every lane then stores {OR, MIN, LDS[0], LDS[4]} at
    16 * lane and LDS[8], the number of lanes that ran the two commit regions, at 0x1000 + 4 * lane."""
    skip = 6 if count_commits else 4  # dwords the s_cbranch_execz jumps over
    count = [("ds_add_u32 v4, v7 offset:8", 8)] if count_commits else []
    wave_or = [(f"v_or_b32_dpp v53, v53, v53 {step}{ROW}", 8) for step in DPP_STEPS]
    wave_min = [(f"v_min_u32_dpp v53, v53, v53 {step}{ROW}", 8) for step in DPP_STEPS]
    return [
        ("v_mov_b32_e32 v1, 0", 4),
        ("v_mov_b32_e32 v7, 1", 4),
        ("v_mov_b32_e32 v16, 0", 4),
        ("v_mov_b32_e32 v17, -1", 4),
        ("ds_write_b64 v1, v[16:17]", 8),          # LDS[0] = 0, the OR identity; LDS[4] = ~0, the MIN identity
        ("ds_write_b32 v1, v16 offset:8", 8),      # LDS[8] = 0, the commit counter
        ("v_lshrrev_b32_e32 v9, 1, v0", 4),
        ("v_lshlrev_b32_e32 v8, v9, v7", 4),       # OR input: 1 << (lane >> 1)
        ("v_xor_b32_e32 v10, 42, v0", 4),
        ("v_add_nc_u32_e32 v11, 0x64, v10", 8),    # MIN input: (lane ^ 42) + 100
        (f"v_cmp_ge_u32_e64 s[6:7], v0, {lo}", 8),
        (f"v_cmp_lt_u32_e64 s[18:19], v0, {hi}", 8),
        ("s_and_b64 s[30:31], s[6:7], s[18:19]", 4),
        ("s_mov_b64 s[28:29], exec", 4),
        ("s_mov_b64 exec, s[30:31]", 4),           # the partial EXEC both reductions start from
        # the wave's OR
        ("s_or_saveexec_b64 s[26:27], -1", 4),
        ("v_cndmask_b32_e64 v53, 0, v8, s[26:27]", 8),
        *wave_or,
        ("v_permlanex16_b32 v54, v53, 0, 0", 8),
        ("v_or_b32_e32 v53, v53, v54", 4),
        ("v_readlane_b32 s24, v53, 0", 8),
        ("v_or_b32_e32 v53, s24, v53", 4),
        ("s_mov_b64 exec, s[26:27]", 4),
        ("s_ff1_i32_b64 s25, exec", 4),
        ("v_readlane_b32 s24, v53, 63", 8),
        ("s_mov_b32 s40, s24", 4),                 # s[24:25] is a mask again below
        ("s_lshl_b64 s[26:27], 1, s25", 4),
        ("s_and_b64 exec, s[26:27], exec", 4),     # the first active lane only
        (f"s_cbranch_execz {skip}", 4),
        ("v_mov_b32_e32 v2, s24", 4),
        ("v_mov_b32_e32 v4, 0", 4),
        ("ds_or_b32 v4, v2", 8),
        *count,
        ("s_mov_b64 exec, s[30:31]", 4),
        # the wave's unsigned MIN; the mask of the first active lane is made before EXEC is widened
        ("s_ff1_i32_b64 s18, exec", 4),
        ("s_lshl_b64 s[24:25], 1, s18", 4),
        ("s_or_saveexec_b64 s[48:49], -1", 4),
        ("v_cndmask_b32_e64 v53, -1, v11, s[48:49]", 8),
        *wave_min,
        ("v_permlanex16_b32 v54, v53, 0, 0", 8),
        ("v_min_u32_e32 v53, v53, v54", 4),
        ("v_readlane_b32 s27, v53, 0", 8),
        ("v_min_u32_e32 v53, s27, v53", 4),
        ("s_mov_b64 exec, s[48:49]", 4),
        ("s_and_saveexec_b64 s[24:25], s[24:25]", 4),
        ("v_readlane_b32 s27, v53, 63", 8),
        (f"s_cbranch_execz {skip}", 4),
        ("v_mov_b32_e32 v4, 0", 4),
        ("v_mov_b32_e32 v2, s27", 4),
        ("ds_min_u32 v4, v2 offset:4", 8),
        *count,
        ("s_mov_b64 exec, s[28:29]", 4),           # every lane again
        ("v_mov_b32_e32 v20, s40", 4),
        ("v_mov_b32_e32 v21, s27", 4),
        ("ds_read_b32 v22, v1", 8),
        ("ds_read_b32 v23, v1 offset:4", 8),
        ("ds_read_b32 v24, v1 offset:8", 8),
        ("v_lshlrev_b32_e32 v12, 4, v0", 4),
        ("buffer_store_dwordx4 v[20:23], v12, s[0:3], 0 offen", 8),
        ("v_lshlrev_b32_e32 v13, 2, v0", 4),
        ("v_add_nc_u32_e32 v13, 0x1000, v13", 8),
        ("buffer_store_dword v24, v13, s[0:3], 0 offen", 8),
        ("s_endpgm", 4),
    ]


def reduce_reference(lo, hi, count_commits):
    active = range(lo, hi)
    wave_or = 0
    for lane in active:
        wave_or |= 1 << (lane >> 1)
    wave_min = min((lane ^ 42) + 100 for lane in active)
    return (wave_or, wave_min, wave_or, wave_min, 2 if count_commits else 0)


class WaveReduceCommit(unittest.TestCase):
    # EXEC ranges: both halves, the whole wave, one half only (each), one lane in the high half, lane 0 alone,
    # and the two lanes either side of the half boundary.
    RANGES = ((5, 59), (0, 64), (40, 50), (3, 9), (33, 34), (0, 1), (31, 33))

    def test_every_lane_sees_the_reduction_and_one_lane_commits_it(self):
        for lo, hi in self.RANGES:
            for count_commits in (False, True):
                want = reduce_reference(lo, hi, count_commits)
                runs = run_both(f"cs_wred_{lo}_{hi}_{int(count_commits)}", reduce_insns(lo, hi, count_commits))
                for mode, memory in runs.items():
                    with self.subTest(exec_lanes=(lo, hi), counter=count_commits, mode=mode):
                        for lane in range(LANES):
                            got = tuple(memory.get(16 * lane + 4 * k) for k in range(4)) + \
                                (memory.get(0x1000 + 4 * lane),)
                            self.assertEqual(got, want, f"lane {lane}")

    def test_the_reference_is_not_the_identity(self):
        # A reduction that did nothing would leave 0 and ~0: no range may expect those.
        for lo, hi in self.RANGES:
            wave_or, wave_min = reduce_reference(lo, hi, False)[:2]
            self.assertNotEqual(wave_or, 0)
            self.assertNotEqual(wave_min, MASK32)
        # Lanes of the low half alone and of the high half alone give different results.
        self.assertNotEqual(reduce_reference(3, 9, False)[:2], reduce_reference(40, 50, False)[:2])


def spill_insns(a_hi, b_lo, other_lo):
    """Mask A = lanes < a_hi becomes EXEC and is spilled from s[80:81] to lanes 2 and 3 of v55; mask B = lanes >=
    b_lo is spilled from s[54:55] to lanes 0 and 1. In between EXEC is lanes >= other_lo and the four SGPRs hold
    another mask and two numbers. Every lane stores {1 if it ran under the reloaded A, 1 if the reloaded B selects
    it} at 8 * lane."""
    return [
        ("v_mov_b32_e32 v20, 0", 4),
        ("v_mov_b32_e32 v21, 0", 4),
        ("v_mov_b32_e32 v7, 1", 4),
        ("s_mov_b64 s[14:15], exec", 4),
        (f"v_cmp_gt_u32_e64 s[72:73], {a_hi}, v0", 8),
        (f"v_cmp_le_u32_e64 s[54:55], {b_lo}, v0", 8),
        ("s_and_b64 exec, s[72:73], exec", 4),
        ("s_mov_b64 s[80:81], exec", 4),
        ("v_writelane_b32 v55, s54, 0", 8),
        ("v_writelane_b32 v55, s55, 1", 8),
        ("v_writelane_b32 v55, s80, 2", 8),
        ("v_writelane_b32 v55, s81, 3", 8),
        (f"v_cmp_le_u32_e64 s[54:55], {other_lo}, v0", 8),
        ("s_mov_b64 exec, s[14:15]", 4),
        ("s_and_b64 exec, s[54:55], exec", 4),
        ("s_mov_b32 s80, 0x12345", 8),
        ("s_mov_b32 s81, 0x6789a", 8),
        ("v_add_nc_u32_e32 v22, s80, v0", 4),
        ("s_mov_b64 s[54:55], 0", 4),
        ("v_readlane_b32 s54, v55, 2", 8),
        ("v_readlane_b32 s55, v55, 3", 8),
        ("s_mov_b64 exec, s[54:55]", 4),           # EXEC = A again
        ("v_mov_b32_e32 v20, 1", 4),
        ("v_readlane_b32 s54, v55, 0", 8),
        ("v_readlane_b32 s55, v55, 1", 8),
        ("s_mov_b64 exec, s[14:15]", 4),
        ("v_cndmask_b32_e64 v21, 0, v7, s[54:55]", 8),  # B selects
        ("v_lshlrev_b32_e32 v12, 3, v0", 4),
        ("buffer_store_dwordx2 v[20:21], v12, s[0:3], 0 offen", 8),
        ("s_endpgm", 4),
    ]


class LaneSpilledMasks(unittest.TestCase):
    CASES = ((37, 9, 20), (64, 0, 0), (5, 60, 33), (33, 31, 1), (1, 63, 40), (32, 32, 16))

    def test_a_mask_pair_survives_two_lanes_of_a_vgpr(self):
        for a_hi, b_lo, other_lo in self.CASES:
            runs = run_both(f"cs_mspill_{a_hi}_{b_lo}", spill_insns(a_hi, b_lo, other_lo))
            for mode, memory in runs.items():
                with self.subTest(a_below=a_hi, b_from=b_lo, mode=mode):
                    for lane in range(LANES):
                        got = (memory.get(8 * lane), memory.get(8 * lane + 4))
                        self.assertEqual(got, (int(lane < a_hi), int(lane >= b_lo)), f"lane {lane}")


def lane_value(lane):
    """A different word in every lane, with both signs and no two equal."""
    return (lane * 0x9E3779B1 + 0x7F4A7C15) & MASK32


def signed(x):
    return x - (1 << 32) if x >> 31 else x


ATOMICS = {  # instruction: (start value, fold)
    "ds_min_u32": (MASK32, min),
    "ds_max_u32": (0, max),
    "ds_min_i32": (0x7FFFFFFF, lambda m, d: m if signed(m) <= signed(d) else d),
    "ds_max_i32": (0x80000000, lambda m, d: m if signed(m) >= signed(d) else d),
    "ds_sub_u32": (0x12345678, lambda m, d: (m - d) & MASK32),
}


def atomic_insns(op, start):
    return [
        ("s_mov_b64 exec, -1", 4),                 # a 64-bit EXEC write: the program is a wave64 (one wave of 64)
        ("v_mov_b32_e32 v1, 0", 4),
        (f"v_mov_b32_e32 v2, {start:#x}", 8),
        ("ds_write_b32 v1, v2", 8),
        ("v_mov_b32_e32 v3, 0x9e3779b1", 8),
        ("v_mul_lo_u32 v4, v0, v3", 8),
        ("v_add_nc_u32_e32 v4, 0x7f4a7c15, v4", 8),  # lane_value(lane)
        (f"{op} v1, v4", 8),
        ("ds_read_b32 v5, v1", 8),
        ("v_lshlrev_b32_e32 v12, 2, v0", 4),
        ("buffer_store_dword v5, v12, s[0:3], 0 offen", 8),
        ("s_endpgm", 4),
    ]


class EmulatedAtomicKinds(unittest.TestCase):
    def test_min_max_and_subtract_over_all_lanes(self):
        for op, (start, fold) in ATOMICS.items():
            want = start
            for lane in range(LANES):
                want = fold(want, lane_value(lane))
            runs = run_both("cs_wred_" + op, atomic_insns(op, start))
            for mode, memory in runs.items():
                with self.subTest(op=op, mode=mode):
                    self.assertEqual({memory.get(4 * lane) for lane in range(LANES)}, {want})

    def test_the_lane_values_tell_the_kinds_apart(self):
        values = [lane_value(lane) for lane in range(LANES)]
        self.assertEqual(len(set(values)), LANES)
        self.assertNotEqual(min(values), min(values, key=signed))
        self.assertNotEqual(max(values), max(values, key=signed))


if __name__ == "__main__":
    unittest.main()
