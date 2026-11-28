"""Floating-point edge-case conformance suite translated via rdna2spirv (SHADER-13).

Translates RDNA2 floating-point edge-case instructions (`v_min_f32`, `v_max_f32`,
`v_fract_f32`, `v_cvt_pkrtz_f16_f32`) through `tools/rdna2spirv.py` and evaluates
them in `spirv_mini_interp.py`: signed zero ordering (-0.0 < +0.0), single/dual NaN
operand selection, signaling/quiet NaN pass-through in `v_fract_f32` (game IEEE=0
mode), `±inf` handling, and RTZ finite saturation in `v_cvt_pkrtz_f16_f32`.
Standard library only; no game data, no GPU.
"""
from __future__ import annotations

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.dont_write_bytecode = True
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import support  # noqa: E402

POS_ZERO_F32 = 0x00000000
NEG_ZERO_F32 = 0x80000000
POS_INF_F32 = 0x7F800000
NEG_INF_F32 = 0xFF800000
IND_NAN_F32 = 0xFFC00000
QNAN_A = 0x7FC10001
QNAN_B = 0xFFC20002
SNAN_A = 0x7F800001
FINITE_3_5 = 0x40600000
F32_65520 = 0x477FF000
F32_NEG_65520 = 0xC77FF000


def _run_ps(lines: list[str]) -> tuple[list[int], str]:
    """Translate instruction lines as a pixel shader exporting v4..v7 and return (words, spvasm)."""
    spvasm = support.translate_lines(
        "ps_fp_edge",
        lines + [
            "exp mrt0 v4, v5, v6, v7 done vm",
            "s_endpgm",
        ],
    )
    return support.mrt0_words(spvasm), spvasm


def _alu1(op: str, a: int) -> int:
    words, _ = _run_ps([
        f"v_mov_b32 v0, {a:#x}",
        f"{op} v4, v0",
        "v_mov_b32 v5, 0",
        "v_mov_b32 v6, 0",
        "v_mov_b32 v7, 0",
    ])
    return words[0]


def _alu2(op: str, a: int, b: int) -> int:
    words, _ = _run_ps([
        f"v_mov_b32 v0, {a:#x}",
        f"v_mov_b32 v1, {b:#x}",
        f"{op} v4, v0, v1",
        "v_mov_b32 v5, 0",
        "v_mov_b32 v6, 0",
        "v_mov_b32 v7, 0",
    ])
    return words[0]


class FloatingPointEdgeConformanceTests(unittest.TestCase):
    """Translated RDNA2 floating-point edge-case verification (SHADER-13)."""

    def test_signed_zero_min_max_ordering(self):
        """RDNA2 v_min_f32 / v_max_f32 treat -0.0 < +0.0 regardless of operand order."""
        for a, b in ((NEG_ZERO_F32, POS_ZERO_F32), (POS_ZERO_F32, NEG_ZERO_F32)):
            with self.subTest(op="v_min_f32", a=hex(a), b=hex(b)):
                self.assertEqual(_alu2("v_min_f32", a, b), NEG_ZERO_F32)
            with self.subTest(op="v_max_f32", a=hex(a), b=hex(b)):
                self.assertEqual(_alu2("v_max_f32", a, b), POS_ZERO_F32)

    def test_nan_minmax_operand_selection(self):
        """One NaN returns the other operand; two NaNs preserve src0's exact bits."""
        for nan in (QNAN_A, SNAN_A):
            for op in ("v_min_f32", "v_max_f32"):
                with self.subTest(op=op, nan=hex(nan), order="nan_first"):
                    self.assertEqual(_alu2(op, nan, FINITE_3_5), FINITE_3_5)
                with self.subTest(op=op, nan=hex(nan), order="nan_second"):
                    self.assertEqual(_alu2(op, FINITE_3_5, nan), FINITE_3_5)

        for op in ("v_min_f32", "v_max_f32"):
            with self.subTest(op=op, pair="a_b"):
                self.assertEqual(_alu2(op, QNAN_A, QNAN_B), QNAN_A)
            with self.subTest(op=op, pair="b_a"):
                self.assertEqual(_alu2(op, QNAN_B, QNAN_A), QNAN_B)
            with self.subTest(op=op, pair="snan_qnan"):
                self.assertEqual(_alu2(op, SNAN_A, QNAN_B), SNAN_A)

    def test_fract_signed_zero_infinities_and_nan_passthrough(self):
        """v_fract_f32: -0 -> +0, +-inf -> 0xffc00000, qNaN and sNaN pass through unchanged (IEEE=0)."""
        self.assertEqual(_alu1("v_fract_f32", NEG_ZERO_F32), POS_ZERO_F32)
        self.assertEqual(_alu1("v_fract_f32", POS_INF_F32), IND_NAN_F32)
        self.assertEqual(_alu1("v_fract_f32", NEG_INF_F32), IND_NAN_F32)
        self.assertEqual(_alu1("v_fract_f32", QNAN_A), QNAN_A)
        self.assertEqual(_alu1("v_fract_f32", SNAN_A), SNAN_A)

    def test_cvt_pkrtz_f16_saturates_finite_overflow(self):
        """v_cvt_pkrtz_f16_f32 clamps finite overflow to +-65504 (+-0x7BFF/0xFBFF), not +-inf."""
        packed = _alu2("v_cvt_pkrtz_f16_f32", F32_65520, F32_NEG_65520)
        self.assertEqual(packed & 0xFFFF, 0x7BFF)
        self.assertEqual((packed >> 16) & 0xFFFF, 0xFBFF)

    def test_emits_signed_zero_inf_nan_preserve_mode(self):
        """Translated shaders declare Vulkan SignedZeroInfNanPreserve 32."""
        _, spvasm = _run_ps([
            f"v_mov_b32 v0, {FINITE_3_5:#x}",
            "v_fract_f32 v4, v0",
            "v_mov_b32 v5, 0",
            "v_mov_b32 v6, 0",
            "v_mov_b32 v7, 0",
        ])
        self.assertIn("SignedZeroInfNanPreserve", spvasm)


if __name__ == "__main__":
    unittest.main()
