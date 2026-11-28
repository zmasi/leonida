"""ds_permute_b32's shuffles run in every lane, also after SPIRV-Cross inlines expressions (SHADER-PERMUTE-EAGER).

Vulkan has no subgroup scatter, so Gen.ds_permute lets every destination lane scan the 32 source lanes of its half:
per source lane one shuffle of the published target and one of the data, and an OpSelect that keeps the data when
the target is this lane. Each shuffle was used once and so was every link of that chain, and SPIRV-Cross (MoltenVK)
inlines a result that is used once into the expression that uses it. The 32 steps became one nested ternary inside
the EXEC ternary of the register write,

    v10 = active ? (hit31 ? data31 : (hit30 ? data30 : ... )) : v10;

where a lane that takes an outer arm, or is off in EXEC, runs none of the shuffles inside it, and a lane that reads
from it gets 0 or a stale word. tools/lds_fuzz.py found it on an Apple M5 (MoltenVK 1.4.2):
`--wave 32 --permute --programs 100 --seed 31002` had 17 programs differ, every one in a ds_permute_b32 result, and
none with both shuffles stored (storing the data shuffle alone left 13). Gen.ds_permute now stores both to Function
variables (Gen.eager, as SHADER-529 did for DPP's ballot and shuffle); a store is a statement every lane runs.

The check is test_dpp_eager_subgroup.py's model of SPIRV-Cross's single-use forwarding, on the three native forms:
a wave32 compute shader, a wave64 one (two wave32 halves) and a pixel shader. The values are test_ds_permute.py's
business and are run here once more with EXEC narrowed, sources that are off and a collision, to show that the
stored shuffles still deliver the same words. --wave64-emulate has no subgroup operation in this rule.

Standard library only; no GPU, no LLVM, no SPIRV-Tools, no game files.
"""
import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import support  # noqa: E402  (puts tools/ on the path)
import test_ds_permute as dp  # noqa: E402  (the programs, the ISA model and the lockstep runner)
from test_dpp_eager_subgroup import conditional_subgroup_ops  # noqa: E402

SHUFFLE = re.compile(r"^\s*(%\w+) = OpGroupNonUniformShuffle\b", re.M)


def shaders():
    """{label: SPIR-V text} of one ds_permute_b32 after EXEC was narrowed, in each native form."""
    insns = dp.CASES["rotate_by_one"][0]
    return {
        "wave32 compute": dp.translate_text("cs_perm_eager_w32", dp.program(insns, 0x55555555), 32),
        "wave64 compute": dp.translate_text("cs_perm_eager_w64", dp.program(insns, 0x5555555555555555, wave=64), 64),
        "pixel": support.translate_lines("ps_perm_eager", [
            "v_mov_b32_e32 v0, 0", "v_mov_b32_e32 v1, 42", "v_mov_b32_e32 v2, 7", "v_cmpx_lt_f32_e32 v0, v8",
            "ds_permute_b32 v2, v0, v1", "exp mrt0 v2, v2, v2, v2 done vm", "s_endpgm"]),
    }


def uses_of(spvasm, result):
    """The instructions that read `result`, as stripped lines."""
    word = re.compile(re.escape(result) + r"(?!\w)")
    return [ln.strip() for ln in spvasm.splitlines()
            if word.search(ln) and not ln.strip().startswith(result + " =")]


class PermuteShufflesRunInEveryLane(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.shaders = shaders()

    def test_no_shuffle_is_left_in_a_conditional_position(self):
        for label, spv in self.shaders.items():
            with self.subTest(shader=label):
                self.assertEqual(conditional_subgroup_ops(spv), [])

    def test_each_shuffle_is_stored_before_anything_reads_it(self):
        for label, spv in self.shaders.items():
            with self.subTest(shader=label):
                shuffles = SHUFFLE.findall(spv)
                self.assertEqual(len(shuffles), 64)             # target and data of 32 source lanes
                stored_to = set()
                for result in shuffles:
                    readers = uses_of(spv, result)
                    self.assertEqual(len(readers), 1, f"{result} is read by {readers}")
                    store = re.fullmatch(r"OpStore (%eager\d+) " + re.escape(result), readers[0])
                    self.assertIsNotNone(store, f"{result} is read by `{readers[0]}`, not stored")
                    stored_to.add(store.group(1))
                self.assertEqual(len(stored_to), 64)            # a variable of its own each: no store hides another

    def test_the_model_of_spirv_cross_sees_an_unstored_shuffle(self):
        # The same module with the stores taken out again is what main produced: the check above must not pass it.
        spv = self.shaders["wave32 compute"]
        for result in SHUFFLE.findall(spv):
            store = re.search(r"^\s*OpStore (%eager\d+) " + re.escape(result)
                              + r"\s*\n\s*(%\w+) = OpLoad %uint \1\s*\n", spv, re.M)
            self.assertIsNotNone(store, result)
            loaded = store.group(2)
            spv = spv.replace(store.group(0), "")
            spv = re.sub(re.escape(loaded) + r"(?!\w)", result, spv)
        found = conditional_subgroup_ops(spv)
        self.assertEqual(len(found), 64)
        self.assertEqual({(op, where.split(" %")[0]) for _, op, where in found},
                         {("OpGroupNonUniformShuffle", "arm of OpSelect")})


class StoredShufflesDeliverTheSameWords(unittest.TestCase):
    """EXEC narrowed, sources that are off and several sources to one lane, against the ISA model."""
    CASES = (("rotate_by_one", 0x0000FFFF), ("inactive_source_does_not_win", 0x55555555),
             ("unwritten_lanes_are_zero", 0x00FF00FF), ("all_to_one_lane", 0xFFFF0000))

    def test_wave32(self):
        for name, mask in self.CASES:
            with self.subTest(case=name):
                insns, addr, off, _ = dp.CASES[name]
                got = dp.run(dp.translate_text("cs_" + name, dp.program(insns, mask, off), 32), 32, 32)
                want = dp.model([addr(lane) for lane in range(32)], mask, off)
                for lane in range(32):
                    self.assertIn(got[lane], want[lane], f"lane {lane} got {got[lane]}, ISA allows {want[lane]}")

    def test_wave64_on_one_and_on_two_subgroups(self):
        for subgroup in (64, 32):
            for name, mask in self.CASES:
                with self.subTest(case=name, subgroup=subgroup):
                    insns, addr, off, _ = dp.CASES[name]
                    mask64 = mask | (mask ^ 0xFFFFFFFF) << 32       # the upper half gets the other lanes
                    spv = dp.translate_text("cs_h_" + name, dp.program(insns, mask64, off, wave=64), 64)
                    got = dp.run(spv, 64, subgroup)
                    want = dp.model([addr(lane) for lane in range(64)], mask64, off, lanes=64)
                    for lane in range(64):
                        self.assertIn(got[lane], want[lane], f"lane {lane} got {got[lane]}, ISA allows {want[lane]}")


if __name__ == "__main__":
    unittest.main()
