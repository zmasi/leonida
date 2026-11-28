"""CPU tests for tools/mad_denorm_probe.py (SHADER-340): RDNA1 mad/mac pre- and post-omod denormal flush.
Standard library only; no GPU, no LLVM, no game files.
"""
import contextlib
import io
import os
import sys
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(ROOT, "tools"))

import mad_denorm_probe as probe  # noqa: E402
from hw_oracle import runner as hw_runner  # noqa: E402


class TranslatorTests(unittest.TestCase):
    def test_translator_matches_rdna1_expected_words(self):
        got = probe.run_translator()
        self.assertEqual(len(got), len(probe.CASES))
        by_what = {c.what: w for c, w in zip(probe.CASES, got)}

        # Pre-omod sum flush: subnormal sum 2^-127 is flushed to +0 before mul:2 / mul:4.
        self.assertEqual(by_what["(1.5*2^-126 - 2^-126) mul:2"], probe.POS_ZERO)
        self.assertEqual(by_what["(1.5*2^-126 - 2^-126) mul:4"], probe.POS_ZERO)
        self.assertEqual(by_what["legacy (1.5*2^-126 - 2^-126) mul:2"], probe.POS_ZERO)

        # Post-omod flush: FLT_MIN div:2 and -FLT_MIN div:2 flush with sign kept.
        self.assertEqual(by_what["FLT_MIN div:2 -> +0"], probe.POS_ZERO)
        self.assertEqual(by_what["-FLT_MIN div:2 -> -0"], probe.NEG_ZERO)
        self.assertEqual(by_what["-FLT_MIN clamp div:2 -> +0"], probe.POS_ZERO)
        self.assertEqual(by_what["legacy FLT_MIN div:2 -> +0"], probe.POS_ZERO)

        # Subnormal product flush and legacy 0 * anything -> +0 after input flush.
        self.assertEqual(by_what["2^-126 * 0.5 + 2^-126 (prod flushed)"], probe.FLT_MIN)
        self.assertEqual(by_what["mac 2^-126 * 0.5 + 2^-126"], probe.FLT_MIN)
        self.assertEqual(by_what["subnorm * +inf + -1 -> -1"], probe.NEG_ONE)
        self.assertEqual(by_what["subnorm * qNaN + 1 -> 1"], probe.ONE)

    def test_mode_sanity_rejects_broken_denorm_control(self):
        class BrokenModeOracle:
            device = "fake"
            arch = "gfx1010"

            def run(self, kernels, words=1):
                return [[probe.POS_ZERO] for _ in kernels]

            def close(self):
                pass

        with mock.patch.object(hw_runner, "open_oracle", return_value=BrokenModeOracle()):
            with contextlib.redirect_stdout(io.StringIO()):
                rc = probe.main(["--oracle", "hw"])
        self.assertEqual(rc, 2)


class HardwareSkipTests(unittest.TestCase):
    """--oracle hw on a GPU without the GFX10.1 mad opcodes is a skip (77), not a traceback."""

    def _main_with_arch(self, arch):
        from hw_oracle import runner

        class Stub:
            device = "stub"
            closed = False

            def __init__(self):
                self.arch = arch

            def run(self, programs, words=4):
                raise AssertionError("no kernel may run on " + arch)

            def close(self):
                Stub.closed = True

        saved = runner.open_oracle
        runner.open_oracle = lambda **kw: Stub()
        try:
            with contextlib.redirect_stdout(io.StringIO()) as out:
                rc = probe.main(["--oracle", "translator,hw"])
        finally:
            runner.open_oracle = saved
        return rc, out.getvalue(), Stub.closed

    def test_rdna2_and_gfx11_skip(self):
        for arch in ("gfx1030", "gfx1036", "gfx1100"):
            rc, out, closed = self._main_with_arch(arch)
            self.assertEqual(rc, 77, arch)
            self.assertIn("hardware oracle unavailable: v_mad_f32", out)
            self.assertTrue(closed, arch)

    def test_empty_and_malformed_cli_flags_are_rejected(self):
        for argv in (
            ["--oracle", ""],
            ["--oracle", "   "],
            ["--oracle", "hw,"],
            ["--oracle", ",translator"],
            ["--oracle", "gpu"],
            ["--hw-arch", ""],
            ["--hw-arch", "   "],
        ):
            with self.subTest(argv=argv), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as cm:
                    probe.main(argv)
                self.assertEqual(cm.exception.code, 2)

    def test_malformed_oracle_results_unavailable_and_runtime_error(self):
        class StubOracle:
            device = "fake"
            arch = "gfx1010"

            def __init__(self, run_fn):
                self._run_fn = run_fn
                self.closed = False
                self.calls = 0

            def run(self, kernels, words=1):
                self.calls += 1
                return self._run_fn(self.calls, kernels)

            def close(self):
                self.closed = True

        fake_want = [0] * len(probe.CASES)

        # Malformed sanity responses (too short, empty, None, empty word list, bool word).
        for label, bad_sanity in (
            ("short_sanity", [[probe.POS_ZERO]]),
            ("empty_sanity", []),
            ("none_sanity", None),
            ("empty_words_sanity", [[], [probe.SUBNORM_HALF]]),
            ("bool_word_sanity", [[True], [probe.SUBNORM_HALF]]),
        ):
            with self.subTest(sanity=label):
                oracle = StubOracle(lambda _c, _ks, s=bad_sanity: s)
                out = io.StringIO()
                with mock.patch.object(hw_runner, "open_oracle", return_value=oracle), \
                        mock.patch.object(probe, "run_translator", return_value=fake_want), \
                        contextlib.redirect_stdout(out):
                    rc = probe.main(["--oracle", "hw"])
                self.assertEqual(rc, 2, out.getvalue())
                self.assertIn("harness error:", out.getvalue())
                self.assertTrue(oracle.closed)

        # Malformed sweep responses (must NOT false-pass on empty/short list!).
        for label, bad_sweep in (
            ("empty_sweep", []),
            ("short_sweep", [[0]] * (len(probe.CASES) - 1)),
            ("long_sweep", [[0]] * (len(probe.CASES) + 1)),
            ("empty_words_sweep", [[] for _ in probe.CASES]),
            ("bool_word_sweep", [[True] for _ in probe.CASES]),
        ):
            with self.subTest(sweep=label):
                oracle = StubOracle(
                    lambda call, _ks, s=bad_sweep: [[probe.POS_ZERO], [probe.SUBNORM_HALF]] if call == 1 else s
                )
                out = io.StringIO()
                with mock.patch.object(hw_runner, "open_oracle", return_value=oracle), \
                        mock.patch.object(probe, "run_translator", return_value=fake_want), \
                        contextlib.redirect_stdout(out):
                    rc = probe.main(["--oracle", "hw"])
                self.assertEqual(rc, 2, out.getvalue())
                self.assertIn("harness error:", out.getvalue())
                self.assertTrue(oracle.closed)

        # runner.Unavailable during oracle.run() exits 77 cleanly and closes oracle.
        def raise_unavail(_call, _ks):
            raise hw_runner.Unavailable("mode unsupported on device")

        oracle = StubOracle(raise_unavail)
        out = io.StringIO()
        with mock.patch.object(hw_runner, "open_oracle", return_value=oracle), \
                mock.patch.object(probe, "run_translator", return_value=fake_want), \
                contextlib.redirect_stdout(out):
            rc = probe.main(["--oracle", "hw"])
        self.assertEqual(rc, 77, out.getvalue())
        self.assertIn("hardware oracle unavailable: mode unsupported on device", out.getvalue())
        self.assertTrue(oracle.closed)

        # RuntimeError during oracle.run() exits 2 cleanly and closes oracle.
        def raise_runtime(_call, _ks):
            raise RuntimeError("gpu hung")

        oracle = StubOracle(raise_runtime)
        out = io.StringIO()
        with mock.patch.object(hw_runner, "open_oracle", return_value=oracle), \
                mock.patch.object(probe, "run_translator", return_value=fake_want), \
                contextlib.redirect_stdout(out):
            rc = probe.main(["--oracle", "hw"])
        self.assertEqual(rc, 2, out.getvalue())
        self.assertIn("harness error: gpu hung", out.getvalue())
        self.assertTrue(oracle.closed)


if __name__ == "__main__":
    unittest.main()
