"""Unit tests for tools/vk_model_fuzz.py: the Vulkan run of a diff fuzzer judged against its own reference model.

The GPU is stood in for by a ``judge`` function, so the test needs no Vulkan device, llvm-mc or game files.

    python -I tests/test_vk_model_fuzz.py
"""
import contextlib
import io
import os
import random
import sys
import tempfile
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS_DIR = os.path.join(ROOT, "tools")
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

import pk16_diff  # noqa: E402
import vk_model_fuzz as vmf  # noqa: E402


def agreeing_judge(seen):
    """A GPU that returns exactly the reference words: every program matches."""
    def judge(progs, want, verdict, **kwargs):
        seen.update(progs=progs, want=want, kwargs=kwargs)
        counts, bad = {}, []
        for i, (lines, words) in enumerate(zip(progs, want)):
            if isinstance(words, str):
                counts["skip: " + words] = counts.get("skip: " + words, 0) + 1
                continue
            klass, detail = verdict(lines, words, list(words))
            counts[klass] = counts.get(klass, 0) + 1
            if klass == "mismatch":
                bad.append((i, detail, lines))
        return counts, bad
    return judge


def one_wrong_word_judge(progs, want, verdict, **kwargs):
    """A GPU that flips one bit of the first judged program's first word."""
    counts, bad = {}, []
    flipped = False
    for i, (lines, words) in enumerate(zip(progs, want)):
        if isinstance(words, str):
            counts["skip: " + words] = counts.get("skip: " + words, 0) + 1
            continue
        got = list(words)
        if not flipped:
            got[0] ^= 1
            flipped = True
        klass, detail = verdict(lines, words, got)
        counts[klass] = counts.get(klass, 0) + 1
        if klass == "mismatch":
            bad.append((i, detail, lines))
    return counts, bad


class RunTest(unittest.TestCase):
    def test_programs_are_the_ones_the_fuzzer_generates_for_the_seed(self):
        seen = {}
        vmf.run("pk16_diff", 25, 7, 4, 32, None, None, judge=agreeing_judge(seen))
        rng = random.Random(7)
        expected = [pk16_diff.generate(rng, rng.randrange(1, 5), list(pk16_diff.ALL_OPS)) for _ in range(25)]
        self.assertEqual(seen["progs"], expected)

    def test_the_model_words_are_what_the_gpu_is_judged_against(self):
        seen = {}
        counts, bad = vmf.run("pk16_diff", 40, 3, 6, 32, None, None, judge=agreeing_judge(seen))
        self.assertEqual(bad, [])
        self.assertEqual(sum(counts.values()), 40)
        for lines, words in zip(seen["progs"], seen["want"]):
            self.assertEqual(words, [w & 0xFFFFFFFF for w in pk16_diff.reference(lines)])

    def test_sdwa_programs_are_not_assumed_uniform_across_lanes(self):
        seen = {}
        vmf.run("sdwa_diff", 10, 2, 3, 32, None, None, judge=agreeing_judge(seen))
        self.assertFalse(seen["kwargs"]["uniform"])
        seen.clear()
        vmf.run("vint_diff", 10, 2, 3, 32, None, None, judge=agreeing_judge(seen))
        self.assertTrue(seen["kwargs"]["uniform"])

    def test_a_program_the_model_declines_is_a_skip_with_a_reason(self):
        with mock.patch.object(pk16_diff, "reference", side_effect=pk16_diff.Skip("not modelled")):
            self.assertEqual(vmf.reference_words(pk16_diff, ["v_nop"]), "not modelled")
        with mock.patch.object(pk16_diff, "reference", side_effect=pk16_diff.RefError("bad program")):
            self.assertEqual(vmf.reference_words(pk16_diff, ["v_nop"]), "bad program")


    def test_declined_programs_never_reach_the_gpu_and_mismatch_indexes_stay_original(self):
        real = pk16_diff.reference
        calls = []

        def declining(lines):
            calls.append(lines)
            if len(calls) % 2 == 0:
                raise pk16_diff.Skip("every other program")
            return real(lines)

        seen = {}
        with mock.patch.object(pk16_diff, "reference", side_effect=declining):
            counts, bad = vmf.run("pk16_diff", 10, 5, 4, 32, None, None, judge=agreeing_judge(seen))
        self.assertEqual(len(seen["progs"]), 5)
        self.assertEqual(counts.get("skip: model declines: every other program"), 5)
        self.assertEqual(counts.get("match"), 5)
        self.assertEqual(bad, [])

        calls.clear()
        with mock.patch.object(pk16_diff, "reference", side_effect=declining):
            counts, bad = vmf.run("pk16_diff", 10, 5, 4, 32, None, None, judge=one_wrong_word_judge)
        self.assertEqual(len(bad), 1)
        rng = random.Random(5)
        progs = [pk16_diff.generate(rng, rng.randrange(1, 5), list(pk16_diff.ALL_OPS)) for _ in range(10)]
        self.assertEqual(bad[0][2], progs[bad[0][0]])
        self.assertEqual(bad[0][0] % 2, 0)   # the model declined the odd-numbered calls, so judged ones are even


class MainTest(unittest.TestCase):
    def call(self, *argv, judge=None):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            if judge is None:
                code = vmf.main(list(argv))
            else:
                real = vmf.run

                def with_judge(*a, **k):
                    return real(*a, judge=judge, **k)
                with mock.patch.object(vmf, "run", side_effect=with_judge):
                    code = vmf.main(list(argv))
        return code, out.getvalue(), err.getvalue()

    def test_all_programs_matching_exits_zero(self):
        code, out, _ = self.call("pk16_diff", "--programs", "20", "--seed", "4", judge=agreeing_judge({}))
        self.assertEqual(code, 0)
        self.assertIn("pk16_diff seed 4, 20 programs, wave 32:", out)
        self.assertNotIn("MISMATCH", out)

    def test_a_wrong_word_exits_one_and_prints_the_program(self):
        code, out, _ = self.call("vint_diff", "--programs", "20", "--seed", "4", judge=one_wrong_word_judge)
        self.assertEqual(code, 1)
        self.assertIn("MISMATCH program", out)
        self.assertIn("\n    ", out)

    def test_nothing_judged_exits_77(self):
        with mock.patch.object(vmf, "run", return_value=({"skip: hardware: x": 3}, [])):
            code, out, _ = self.call("pk16_diff", "--programs", "3")
        self.assertEqual(code, 77)

    def test_a_folder_without_vk_compute_test_is_one_error_line_and_exit_two(self):
        with tempfile.TemporaryDirectory() as folder:
            code, out, err = self.call("pk16_diff", "--programs", "2", "--build", folder)
        self.assertEqual(code, 2)
        self.assertTrue(err.startswith("ERROR: "), err)
        self.assertEqual(len(err.strip().splitlines()), 1, err)
        self.assertNotIn("Traceback", err)

    def test_bad_arguments_are_a_usage_error(self):
        for argv in (["pk16_diff", "--programs", "0"], ["pk16_diff", "--ops", "a,,b"], ["nope"]):
            with self.subTest(argv=argv), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as caught:
                    vmf.main(argv)
                self.assertEqual(caught.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
