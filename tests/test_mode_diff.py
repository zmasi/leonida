"""tools/mode_diff.py: the MODE.FP_ROUND reference against hand-worked f16 words, the generator, a short seeded run
of each stage, and planted translator bugs that the run must catch. Standard library only; no GPU, no game files.
"""
import contextlib
import io
import os
import random
import sys
import unittest
from collections import Counter
from fractions import Fraction
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
import mode_diff as d  # noqa: E402

RTZ, RNE = d.SETREG.format(3), d.SETREG.format(0)
ZEROED = [f"v_mov_b32 v{r}, 0" for r in d.EXPORT_REGS]


def lo_words(*body):
    """The low halves of the reference's four exported words."""
    words, _ = d.reference(ZEROED + list(body))
    return [w & 0xFFFF for w in words]


def num(x):
    x = Fraction(x)
    return ("num", int(x < 0), abs(x))


class Rounding(unittest.TestCase):
    def test_round16_both_modes(self):
        r = d.round16
        self.assertEqual((r(num(65519), False), r(num(65520), False)), (0x7BFF, 0x7C00))
        self.assertEqual((r(num(65535), True), r(num(-10 ** 6), True)), (0x7BFF, 0xFBFF))
        one_and = [num(1 + Fraction(k, 1 << 12)) for k in (2, 6, 3)]       # 1 + half an ulp, 1.5 ulps, 0.75 ulp
        self.assertEqual([r(v, False) for v in one_and], [0x3C00, 0x3C02, 0x3C01])
        self.assertEqual([r(v, True) for v in one_and], [0x3C00, 0x3C01, 0x3C00])
        self.assertEqual(r(num(-1 - Fraction(3, 1 << 12)), True), 0xBC00)  # toward zero, not down
        self.assertEqual((r(num(Fraction(1, 1 << 25)), False), r(num(Fraction(3, 1 << 25)), False)), (0, 2))
        self.assertEqual(r(num(Fraction(3, 1 << 25)), True), 1)

    def test_mode_writes_switch_and_persist(self):
        # 1 + 0.75 ulp: 0x3c01 to nearest even, 0x3c00 toward zero; the field holds until the next write
        add = "v_add_f16_e32 v{}, v12, v13"
        prog = ["v_mov_b32 v12, 0x3c00", "v_mov_b32 v13, 0x1200", RTZ, add.format(8), add.format(9), RNE,
                add.format(10), "s_round_mode 0xc", add.format(11)]
        self.assertEqual(lo_words(*prog), [0x3C00, 0x3C00, 0x3C01, 0x3C00])
        self.assertEqual(lo_words(*prog[:2], add.format(8)), [0x3C01, 0, 0, 0])     # nearest even at the start

    def test_overflow_gives_65504_toward_zero(self):
        prog = ["v_mov_b32 v12, 0x7bff", "v_mov_b32 v13, 0xc000", "v_mul_f16_e32 v8, v12, v13", RTZ,
                "v_mul_f16_e32 v9, v12, v13", "v_mov_b32 v14, 1", "v_ldexp_f16_e32 v10, v12, v14"]
        self.assertEqual(lo_words(*prog)[:3], [0xFC00, 0xFBFF, 0x7BFF])
        omod = ["v_mov_b32 v12, 0x47000000", "v_cvt_f16_f32_e64 v8, v12 mul:2", RTZ, "v_cvt_f16_f32_e64 v9, v12 mul:2"]
        self.assertEqual(lo_words(*omod)[:2], [0x7C00, 0x7BFF])     # 32768 -> 0x7800, then mul:2 overflows

    def test_sum_rounding_back_to_an_f16(self):
        # 2 - 2^-24: f32 rounds it to 2.0, but the exact sum truncates to 0x3fff
        prog = ["v_mov_b32 v12, 0x4000", "v_mov_b32 v13, 0x8001", "v_add_f16_e32 v8, v12, v13", RTZ,
                "v_add_f16_e32 v9, v12, v13"]
        self.assertEqual(lo_words(*prog)[:2], [0x4000, 0x3FFF])

    def test_ldexp_and_fract(self):
        prog = ["v_mov_b32 v12, 0x3e00", "v_mov_b32 v13, 0xffffffe7", "v_ldexp_f16_e32 v8, v12, v13", RTZ,
                "v_ldexp_f16_e32 v9, v12, v13"]
        self.assertEqual(lo_words(*prog)[:2], [0x0001, 0x0000])     # 0.75 * 2^-24
        prog = ["v_mov_b32 v12, 0x9401", "v_fract_f16_e32 v8, v12", RTZ, "v_fract_f16_e32 v9, v12"]
        self.assertEqual(lo_words(*prog)[:2], [0x3BFE, 0x3BFD])
        prog = ["v_mov_b32 v12, 0x8001", "v_fract_f16_e32 v8, v12", RTZ, "v_fract_f16_e32 v9, v12"]
        self.assertEqual(lo_words(*prog)[:2], [0x3BFF, 0x3BFF])     # 1 - 2^-24 rounds to 1.0 -> 0x3bff

    def test_nan_results_match_any_nan(self):
        want, nans = d.reference(ZEROED + ["v_mov_b32 v12, 0x7c00", "v_fract_f16_e32 v8, v12"])
        self.assertIsNone(d.compare(want, nans, [0x7E01] + want[1:]))
        self.assertIn("v8.lo", d.compare(want, nans, [0x3C00] + want[1:]))
        with self.assertRaises(d.NotJudged):
            d.reference(ZEROED + ["v_mov_b32 v12, 0x7e00", "v_add_f16_e64 v8, v12, v12 clamp"])


class Generator(unittest.TestCase):
    def test_generation_is_deterministic(self):
        self.assertEqual(d.program(random.Random(5), 6), d.program(random.Random(5), 6))
        self.assertNotEqual(d.program(random.Random(5), 6), d.program(random.Random(6), 6))
        self.assertEqual(d.invariance_program(random.Random(5), 6), d.invariance_program(random.Random(5), 6))

    def test_sweep_covers_every_form_under_every_write(self):
        cases = d.sweep_cases()
        self.assertEqual(len(cases), sum(len(f) for f, _ in d.FAMILY.values()) * len(d.MODE_WRITES))
        self.assertEqual({op for op, _, _ in cases}, set(d.FAMILY))
        lines = d.sweep_program(cases[0], random.Random(1))
        self.assertIn(cases[0][2], lines)

    def test_ops_subset_and_destinations(self):
        rng = random.Random(2)
        ops = ["v_fma_mixlo_f16", "v_ldexp_f16"]
        for _ in range(40):
            lines, used = d.program(rng, 4, ops)
            self.assertTrue(set(used) <= set(ops))
            for line in lines:
                op, args, _ = d.split_line(line)
                if d.base_op(op) in d.FAMILY:
                    self.assertIn(int(args[0][1:]), d.DSTS)

    def test_invariance_pair_differs_only_in_mode_writes(self):
        rng = random.Random(3)
        for _ in range(20):
            with_mode, plain = d.invariance_program(rng, 5)
            self.assertIn(RTZ if RTZ in with_mode else "s_round_mode 0xc", with_mode)
            self.assertEqual([ln for ln in with_mode if ln not in d.MODE_WRITES], plain)
            self.assertFalse(set(plain) & set(d.MODE_WRITES))


class Translated(unittest.TestCase):
    def test_harness_canaries(self):
        self.assertTrue(d.self_test())

    def test_short_runs_match(self):
        ops = ["v_add_f16", "v_cvt_f16_f32", "v_ldexp_f16", "v_fma_mixhi_f16"]
        counts, failure = d.run_sweep(2, ops)
        self.assertEqual((failure, counts["skip"]), (None, {}))
        self.assertEqual(counts["pass"] + sum(counts["unjudged"].values()), len(d.sweep_cases(ops)))    # 7 forms x 4
        self.assertGreater(counts["pass"], 20)
        counts, failure = d.run_suite(20, 2)
        self.assertEqual((failure, counts["skip"]), (None, {}))
        counts, failure = d.run_invariance(10, 2)
        self.assertEqual((failure, counts["skip"]), (None, {}))


class PlantedBugs(unittest.TestCase):
    """Each change to tools/rdna2spirv.py below is a bug the fuzzer must report."""
    G = d.rdna2spirv.Gen

    def planted(self, attr, fn):
        real = getattr(self.G, attr)
        setattr(self.G, attr, fn)
        self.addCleanup(setattr, self.G, attr, real)
        return real

    def test_f16_results_ignore_mode(self):
        self.planted("f16_res", lambda g, f, exact_tiny=False: g.f16_bits(f, exact_tiny=exact_tiny))
        diff = d.check(ZEROED + ["v_mov_b32 v12, 0x3c00", "v_mov_b32 v13, 0x1200", RTZ, "v_add_f16_e32 v8, v12, v13"])
        self.assertEqual(diff, "v8.lo: want 0x3c00, got 0x3c01")

    def test_f32_sum_decides_the_toward_zero_result(self):
        self.planted("sum_toward_zero", lambda g, x, y, r: r)
        diff = d.check(ZEROED + ["v_mov_b32 v12, 0x4000", "v_mov_b32 v13, 0x8001", RTZ, "v_add_f16_e32 v8, v12, v13"])
        self.assertEqual(diff, "v8.lo: want 0x3fff, got 0x4000")

    def test_s_round_mode_ignored(self):
        real = self.planted("write_half_round_mode",
                            lambda g, op, mode: real(g, op, 0 if op == "s_round_mode" else mode))
        diff = d.check(ZEROED + ["v_mov_b32 v12, 0x3c00", "v_mov_b32 v13, 0x1200", "s_round_mode 0xc",
                                 "v_add_f16_e32 v8, v12, v13"])
        self.assertEqual(diff, "v8.lo: want 0x3c00, got 0x3c01")

    def test_transcendentals_follow_mode(self):
        real = self.planted("f16_out", lambda g, dst, r, hi=0, fract=False, mode=True, exact_tiny=False:
                            real(g, dst, r, hi, fract, True, exact_tiny))
        counts, failure = d.run_invariance(10, 1)
        self.assertGreater(counts["mismatch"], 0)
        self.assertIn("with the MODE writes", failure[2])


class CliGuards(unittest.TestCase):
    def _run_main(self, argv):
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(sys, "argv", ["mode_diff.py", *argv]), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            try:
                rc = d.main()
            except SystemExit as exc:
                rc = exc.code
        return rc, out.getvalue(), err.getvalue()

    def test_invalid_counts_and_empty_runs_are_rejected(self):
        for argv, needle in (
            (["--no-sweep", "--programs", "-1", "--invariance", "0"], "--programs"),
            (["--no-sweep", "--programs", "0", "--invariance", "-1"], "--invariance"),
            (["--no-sweep", "--programs", "1", "--invariance", "0", "--max-ops", "0"], "--max-ops"),
            (["--no-sweep", "--programs", "1", "--invariance", "0", "--max-ops", "-2"], "--max-ops"),
            (["--no-sweep", "--programs", "0", "--invariance", "0"], "nothing to run"),
        ):
            with self.subTest(argv=argv):
                rc, _, err = self._run_main(argv)
                self.assertEqual(rc, 2)
                self.assertIn(needle, err)

    def test_all_unjudged_run_exits_two(self):
        with mock.patch.object(d, "run_one", return_value=("unjudged", "synthetic unjudged")):
            rc, _, err = self._run_main(["--no-sweep", "--programs", "2", "--invariance", "0", "--ops", "v_add_f16"])
        self.assertEqual(rc, 2)
        self.assertIn("no judged program ran", err)

    def test_ops_validation_and_deduplication(self):
        for bad_ops in ("", "   ", ",", "v_add_f16,", ",v_add_f16"):
            with self.subTest(ops=bad_ops):
                rc, _, err = self._run_main(["--no-sweep", "--programs", "1", "--invariance", "0", "--ops", bad_ops])
                self.assertEqual(rc, 2)
                self.assertIn("--ops", err)

        seen_ops = []

        def fake_suite(programs, seed, max_ops=6, ops=None):
            seen_ops.append(ops)
            return {"pass": 1, "mismatch": 0, "skip": Counter(), "unjudged": Counter(),
                    "ops": Counter({"v_add_f16": 1}), "events": {}}, None

        with mock.patch.object(d, "run_suite", side_effect=fake_suite):
            rc, _, _ = self._run_main(["--no-sweep", "--programs", "1", "--invariance", "0",
                                       "--ops", "v_add_f16, v_add_f16"])
        self.assertEqual(rc, 0)
        self.assertEqual(seen_ops, [["v_add_f16"]])

    def test_suites_validate_bounds_and_main_accepts_argv(self):
        for fn in (d.run_suite, d.run_invariance):
            for bad_programs in (-1, True):
                with self.subTest(fn=fn.__name__, programs=bad_programs), self.assertRaises(ValueError):
                    fn(bad_programs, 1)
            for bad_max_ops in (0, -1, True):
                with self.subTest(fn=fn.__name__, max_ops=bad_max_ops), self.assertRaises(ValueError):
                    fn(1, 1, max_ops=bad_max_ops)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(d.main(["--list-ops"]), 0)
        self.assertIn("v_add_f16", out.getvalue())


if __name__ == "__main__":
    unittest.main()
