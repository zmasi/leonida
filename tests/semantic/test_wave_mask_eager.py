"""The whole-wave mask gather runs in every lane, also after SPIRV-Cross inlines expressions (SHADER-WAVEMASK-EAGER).

A register holds only this lane's bit of a lane mask. The instructions that read a mask as a whole (v_mbcnt, s_bcnt1,
s_ff1, ...) gather the other lanes' bits with a subgroup operation (Gen.wave_mask). v_mbcnt writes a VGPR, so its
result is an OpSelect on the lane's EXEC, and SPIRV-Cross (MoltenVK) inlines a result that is used once into the
expression that uses it. The gather ended up in the taken arm of a ternary,

    v6 = active ? popcount(simd_or(mine ? bit : 0) & below) + acc : v6;

where a lane that is off in EXEC never runs it, so its mask bit is missing from what every other lane counts.
tools/crosslane_fuzz.py found it on an Apple M5 (MoltenVK 1.4.2): 4 of 100 wave32 programs of the default seed
mismatched, each a v_mbcnt of a compare mask read after EXEC was narrowed. Gen.wave_mask now stores the gather to a
Function variable (Gen.eager, as SHADER-529 did for DPP's ballot and shuffle), and a store is a statement every lane
runs.

The check is test_dpp_eager_subgroup.py's model of SPIRV-Cross's single-use forwarding. The values are checked too,
in tests/semantic/spirv_wave_interp.py; it runs every lane at every instruction, so it shows that the stored gather
still counts the right lanes, not what Metal did.

Standard library only; no GPU, no LLVM, no SPIRV-Tools, no game files.
"""
import os
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import support  # noqa: E402  (puts tools/ on the path)
import spirv_wave_interp as wi  # noqa: E402
from test_dpp_eager_subgroup import conditional_subgroup_ops  # noqa: E402

READERS = ("v_mbcnt", "s_bcnt1", "s_ff1")
MASKS = ("s16", "vcc_lo", "exec_lo")
FLOOR, LIMIT = 8, 20        # EXEC is narrowed to the lanes above FLOOR; the compare mask is the lanes below LIMIT


def body(reader, mask):
    """v0 = the lane. `mask` gets the lanes below LIMIT (exec_lo is read as it is), EXEC is narrowed to the lanes
    above FLOOR, and `reader` reads the mask there: the lanes at or below FLOOR are off, and every one of them below
    LIMIT has its bit set in the mask. v6 = 100 + the result in the lanes still on, and stays 100 in the others."""
    lines = ["v_mbcnt_lo_u32_b32 v0, -1, 0", "v_mov_b32 v6, 100", "v_mov_b32 v7, 100", "s_mov_b32 s10, exec_lo"]
    if mask != "exec_lo":
        lines.append(f"v_cmp_gt_u32_e64 {mask}, {LIMIT}, v0")
    lines.append(f"v_cmpx_lt_u32_e32 {FLOOR}, v0")
    if reader == "v_mbcnt":
        lines.append(f"v_mbcnt_lo_u32_b32 v6, {mask}, v7")
    else:
        lines += [f"{reader}_i32_b32 s5, {mask}", "v_add_nc_u32 v6, s5, v7"]
    return lines + ["s_mov_b32 exec_lo, s10"]


def vertex(reader, mask):
    return support.translate_lines("vs_wave_mask_eager", body(reader, mask) + [
        "v_mov_b32 v5, 0", "exp pos0 v5, v5, v5, v5 done", "exp param0 v6, v5, v5, v5", "s_endpgm"])


def pixel(reader, mask):
    return support.translate_lines("ps_wave_mask_eager", body(reader, mask) + [
        "exp mrt0 v6, v6, v6, v6 done vm", "s_endpgm"])


def compute(reader, mask, threads=32):
    header = bytearray(0x148)
    struct.pack_into("<4I", header, 0x138, 0x8088, threads, 1, 1)
    return support.translate_lines("cs_wave_mask_eager", body(reader, mask) + [
        "v_lshlrev_b32_e32 v22, 2, v0", "buffer_store_dword v6, v22, s[0:3], 0 offen", "s_endpgm"], fssm=bytes(header))


def reference(reader, mask, wave=32):
    """v6 per lane after body(reader, mask), from the ISA: a mask is one bit per lane, whatever EXEC is when it is
    read; v_mbcnt_lo counts its bits below the lane, s_bcnt1 all of them, s_ff1 gives the lowest."""
    on = [lane > FLOOR for lane in range(wave)]
    bits = on if mask == "exec_lo" else [lane < LIMIT for lane in range(wave)]
    out = []
    for lane in range(wave):
        if not on[lane]:
            out.append(100)
        elif reader == "v_mbcnt":
            out.append(100 + sum(bits[:lane]))
        elif reader == "s_bcnt1":
            out.append(100 + sum(bits))
        else:
            out.append(100 + bits.index(True))
    return out


class WaveMaskGatherRunsInEveryLane(unittest.TestCase):
    def test_no_gather_is_conditional(self):
        for stage, make in (("vertex", vertex), ("compute", compute), ("pixel", pixel)):
            for reader in READERS:
                for mask in MASKS:
                    with self.subTest(stage=stage, reader=reader, mask=mask):
                        spv = make(reader, mask)
                        gather = "OpGroupNonUniformBallot" if stage == "pixel" else "OpGroupNonUniformBitwiseOr"
                        self.assertIn(gather, spv)
                        self.assertEqual(conditional_subgroup_ops(spv), [])

    def test_the_gather_is_stored_before_v_mbcnt_selects_on_exec(self):
        # The shape itself, for the case the fuzzer hit: both halves of the gathered mask go to a variable.
        spv = compute("v_mbcnt", "s16")
        gathers = [ln.split()[0] for ln in spv.splitlines() if "OpGroupNonUniformBitwiseOr" in ln]
        self.assertEqual(len(gathers), 2, spv)
        for g in gathers:
            uses = [ln.split() for ln in spv.splitlines() if g in ln.split()[1:]]
            self.assertEqual([u[0] for u in uses], ["OpStore"], f"{g} is used by {uses}")

    def test_the_model_finds_the_old_shape(self):
        # What Gen.wave_mask gave v_mbcnt before: the gather's one use chain ends in an arm of the select on EXEC.
        old = ("%lo = OpGroupNonUniformBitwiseOr %uint %c3 Reduce %lo_in\n"
               "%below = OpBitwiseAnd %uint %lo %m1\n"
               "%cnt = OpBitCount %uint %below\n"
               "%r = OpIAdd %uint %cnt %acc\n"
               "%active = OpLoad %bool %active_var\n"
               "%sel = OpSelect %uint %active %r %old\n"
               "OpStore %reg_v6 %sel\n")
        self.assertEqual([(r, op) for r, op, _ in conditional_subgroup_ops(old)],
                         [("%lo", "OpGroupNonUniformBitwiseOr")])

    def test_values(self):
        for reader in READERS:
            for mask in MASKS:
                with self.subTest(reader=reader, mask=mask):
                    res = wi.run_wave(vertex(reader, mask), wave=32,
                                      builtins={"VertexIndex": lambda lane: lane, "InstanceIndex": lambda lane: 0})
                    got = [o["%out_param0"][0] for o in res["outputs"]]
                    self.assertEqual(got, reference(reader, mask))

    def test_reference_counts_the_lanes_that_are_off(self):
        # Lane 12 is on; lanes 0..8 are off and all in the compare mask, so v_mbcnt counts 12, not the 3 that are on.
        self.assertEqual(reference("v_mbcnt", "s16")[12], 112)
        self.assertEqual(reference("v_mbcnt", "exec_lo")[12], 103)       # EXEC itself: only lanes 9, 10, 11
        self.assertEqual(reference("s_bcnt1", "vcc_lo")[31], 100 + LIMIT)
        self.assertEqual(reference("s_ff1", "exec_lo")[9], 100 + FLOOR + 1)
        self.assertEqual(reference("s_ff1", "s16")[FLOOR], 100)          # off: keeps its 100


if __name__ == "__main__":
    unittest.main()
