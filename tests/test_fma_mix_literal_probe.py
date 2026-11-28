"""tools/fma_mix_literal_probe.py: the cases tell the two readings of a split v_fma_mix literal apart, the encodings
are the ones described, the translator side follows the halves reading, and a missing GPU is exit 77. Standard
library only; no GPU, no LLVM, no game files.

The expected values are worked out by hand (src1 is v2 = 2.0; f16 0x4000 = 2.0, 0x4200 = 3.0, 0xbc00 = -1.0; f32
0x40400000 = 3.0). No low half is an inline f16 constant, so llvm-mc prints each literal as the plain hex the listing
uses (an inline one, such as 0x3c00, prints as lit(0x...)):

  A  fma(lo 3.0, 2.0, hi 2.0) = 8.0    low reading: fma(3.0, 2.0, 0.0) = 6.0
  B  fma(hi 3.0, 2.0, hi 3.0) = 9.0    low reading: fma(0.0, 2.0, 0.0) = 0.0
  E  fma(2.0, 2.0, hi -1.0) = 3.0      low reading: fma(2.0, 2.0, 0.0) = 4.0
  C  fma(3.0, 2.0, 3.0) = 9.0 either way
  D  fma(3.0, 2.0, 2.0) = 8.0 either way
"""
import contextlib
import io
import os
import struct
import sys
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "tests", "semantic"))

import fma_mix_literal_probe as probe  # noqa: E402

BY_HAND = {"A": (8.0, 6.0), "B": (9.0, 0.0), "E": (3.0, 4.0), "C": (9.0, 9.0), "D": (8.0, 8.0)}


def f(word):
    return struct.unpack("<f", struct.pack("<I", word))[0]


class Cases(unittest.TestCase):
    def test_model_matches_hand_derivation(self):
        self.assertEqual(sorted(c.name for c in probe.CASES), sorted(BY_HAND))
        for case in probe.CASES:
            with self.subTest(case=case.name):
                self.assertEqual((f(case.expected(probe.HALVES)), f(case.expected(probe.LOW))), BY_HAND[case.name])

    def test_only_the_controls_agree(self):
        for case in probe.CASES:
            halves, low = BY_HAND[case.name]
            self.assertEqual(halves == low, case.what.startswith("control"), case.name)

    def test_printed_literal_is_the_low_half(self):
        # llvm-mc 23.1.5 re-encodes a v_fma_mix literal with its low 16 bits, also one read as f32 (D).
        by = {c.name: c for c in probe.CASES}
        self.assertEqual(by["A"].printed_literal(), 0x4200)
        self.assertEqual(by["D"].printed_literal(), 0x0000)


class Encoding(unittest.TestCase):
    def test_case_a_fields(self):
        a = next(c for c in probe.CASES if c.name == "A")
        w0, w1, lit = a.words(8, 2, a.literal)
        self.assertEqual(w0 >> 26, 0b110011)                       # VOP3P
        self.assertEqual((w0 >> 16) & 0x7F, 0x20)                  # v_fma_mix_f32
        self.assertEqual(w0 & 0xFF, 8)                             # vdst v8
        self.assertEqual([(w0 >> b) & 1 for b in (11, 12, 13)], [0, 0, 1])        # op_sel
        self.assertEqual([(w1 >> 27) & 1, (w1 >> 28) & 1, (w0 >> 14) & 1], [1, 0, 1])  # op_sel_hi
        self.assertEqual([(w1 >> (9 * i)) & 0x1FF for i in range(3)], [0xFF, 0x102, 0xFF])
        self.assertEqual(lit, 0x40004200)

    def test_hw_kernel_carries_the_exact_words(self):
        a = next(c for c in probe.CASES if c.name == "A")
        w0, w1, lit = a.words(8, 2, a.literal)
        self.assertIn(f".long 0x{w0:08x}, 0x{w1:08x}, 0x{lit:08x}", probe.hw_lines(a))

    def test_listing_prints_llvm_mc_text_and_the_blob_holds_the_literal(self):
        a = next(c for c in probe.CASES if c.name == "A")
        text, blob = probe.ps_program(a)
        self.assertIn("v_fma_mix_f32 v0, 0x4200, v2, 0x4200 op_sel:[0,0,1] op_sel_hi:[1,0,1]", text)
        self.assertIn(struct.pack("<I", 0x40004200), blob)
        self.assertNotIn(struct.pack("<I", 0x40004200), text.encode())


class Sides(unittest.TestCase):
    def run_main(self, *argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = probe.main(list(argv))
        return code, out.getvalue()

    def test_translator_side_follows_the_halves_reading(self):
        code, out = self.run_main()
        self.assertEqual(code, 0, out)
        self.assertIn("translator: follows the halves reading", out)

    def test_missing_hardware_is_77(self):
        from hw_oracle import runner
        with mock.patch.object(runner, "open_oracle", side_effect=runner.Unavailable("no AMD GPU")):
            code, out = self.run_main("--oracle", "hw")
        self.assertEqual(code, 77)
        self.assertIn("hardware oracle unavailable", out)

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

    def test_hw_oracle_malformed_results_unavailable_and_word_masking(self):
        from hw_oracle import runner

        class StubOracle:
            arch = "gfx1036"
            device = "fake gfx1036"

            def __init__(self, run_fn):
                self._run_fn = run_fn
                self.closed = False

            def run(self, kernels, words=1):
                return self._run_fn(kernels)

            def close(self):
                self.closed = True

        halves_words = [c.expected(probe.HALVES) for c in probe.CASES]
        for label, bad_fn in (
            ("short", lambda ks: [[0]] * (len(ks) - 1)),
            ("long", lambda ks: [[0]] * (len(ks) + 1)),
            ("empty_words", lambda ks: [[] for _ in ks]),
            ("none_entry", lambda ks: [None for _ in ks]),
            ("bool_word", lambda ks: [[True] for _ in ks]),
        ):
            with self.subTest(case=label):
                oracle = StubOracle(bad_fn)
                with mock.patch.object(runner, "open_oracle", return_value=oracle), \
                        mock.patch.object(probe, "run_translator", return_value=halves_words):
                    code, out = self.run_main("--oracle", "hw")
                self.assertEqual(code, 2, out)
                self.assertIn("harness error:", out)
                self.assertTrue(oracle.closed)

        # runner.Unavailable raised inside oracle.run() exits 77 cleanly and closes oracle.
        def raise_unavail(_ks):
            raise runner.Unavailable("mode unsupported")

        oracle = StubOracle(raise_unavail)
        with mock.patch.object(runner, "open_oracle", return_value=oracle), \
                mock.patch.object(probe, "run_translator", return_value=halves_words):
            code, out = self.run_main("--oracle", "hw")
        self.assertEqual(code, 77, out)
        self.assertIn("hardware oracle unavailable: mode unsupported", out)
        self.assertTrue(oracle.closed)

        # verdict masks >32-bit values before exact word comparison.
        reading, _ = probe.verdict([(c, c.expected(probe.HALVES) | (1 << 32)) for c in probe.CASES])
        self.assertEqual(reading, probe.HALVES)


if __name__ == "__main__":
    unittest.main()
