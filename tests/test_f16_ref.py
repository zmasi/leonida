"""Unit tests for tests/semantic/f16_ref.py negative-zero handling and 16-bit masking.

    python -I tests/test_f16_ref.py
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SEMANTIC_DIR = ROOT / "tests" / "semantic"
if str(SEMANTIC_DIR) not in sys.path:
    sys.path.insert(0, str(SEMANTIC_DIR))

import f16_ref  # noqa: E402


class F16RefTests(unittest.TestCase):
    def test_to_f16_preserves_signed_zero_and_negative_underflow(self) -> None:
        self.assertEqual(f16_ref.to_f16(0.0), 0x0000)
        self.assertEqual(f16_ref.to_f16(-0.0), 0x8000)
        self.assertEqual(f16_ref.to_f16(-1e-12), 0x8000)
        self.assertEqual(f16_ref.to_f16(1e-12), 0x0000)

    def test_scale_signed_zero_underflow_and_nan(self) -> None:
        self.assertEqual(f16_ref.scale(0x0000, -1.0), 0x8000)
        self.assertEqual(f16_ref.scale(0x8000, 1.0), 0x8000)
        self.assertEqual(f16_ref.scale(0x8000, -1.0), 0x0000)
        self.assertEqual(f16_ref.scale(0x0001, -0.25), 0x8000)
        self.assertEqual(f16_ref.scale(0x17E01, 2.0), 0x7E01)

    def test_upper_bits_are_masked_to_16_bits(self) -> None:
        hi = 0x10000
        self.assertEqual(f16_ref.f16_value(hi | 0x3C00), 1.0)
        self.assertTrue(f16_ref.is_nan(hi | 0x7E00))
        self.assertTrue(f16_ref.same_f16(hi | 0x3C00, 0x3C00))
        self.assertEqual(f16_ref.ref_add(hi | 0x8000, hi | 0x8000), 0x8000)
        self.assertEqual(f16_ref.ref_sub(hi | 0x8000, hi | 0x0000), 0x8000)
        self.assertEqual(f16_ref.ref_mul(hi | 0x8000, hi | 0x3C00), 0x8000)
        self.assertEqual(f16_ref.ref_fma(hi | 0x8000, hi | 0x3C00, hi | 0x8000), 0x8000)
        self.assertEqual(f16_ref.neg(hi | 0x3C00), 0xBC00)
        self.assertEqual(f16_ref.fabs(hi | 0xBC00), 0x3C00)
        self.assertEqual(f16_ref.clamp01(hi | 0x8000), 0x0000)
        self.assertEqual(f16_ref.clamp01(hi | 0x3800), 0x3800)


if __name__ == "__main__":
    unittest.main()
