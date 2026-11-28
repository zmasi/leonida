"""Direct unit tests for tests/semantic/f16_unary_cases.py reference functions and validation."""
from fractions import Fraction
import os
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests", "semantic"))

import f16_unary_cases  # noqa: E402


class F16UnaryCasesTests(unittest.TestCase):
    def test_reference_ops_special_and_finite_values(self):
        # ref_rcp
        self.assertEqual(f16_unary_cases.ref_rcp(0x0000), 0x7C00)
        self.assertEqual(f16_unary_cases.ref_rcp(0x8000), 0xFC00)
        self.assertEqual(f16_unary_cases.ref_rcp(0x7C00), 0x0000)
        self.assertEqual(f16_unary_cases.ref_rcp(0xFC00), 0x8000)
        self.assertEqual(f16_unary_cases.ref_rcp(0x7D01), 0x7F01)
        self.assertEqual(f16_unary_cases.ref_rcp(0x4000), 0x3800)  # 1 / 2 = 0.5
        self.assertEqual(f16_unary_cases.ref_rcp(0x0400), 0x7400)  # 1 / 2^-14 = 2^14
        self.assertEqual(f16_unary_cases.ref_rcp(0x0001), 0x7C00)  # 1 / 2^-24 overflows f16

        # ref_sqrt
        self.assertEqual(f16_unary_cases.ref_sqrt(0x0000), 0x0000)
        self.assertEqual(f16_unary_cases.ref_sqrt(0x8000), 0x8000)
        self.assertEqual(f16_unary_cases.ref_sqrt(0x7C00), 0x7C00)
        self.assertEqual(f16_unary_cases.ref_sqrt(0xFC00), 0xFE00)
        self.assertEqual(f16_unary_cases.ref_sqrt(0xBC00), 0xFE00)
        self.assertEqual(f16_unary_cases.ref_sqrt(0x7D01), 0x7F01)
        self.assertEqual(f16_unary_cases.ref_sqrt(0x4400), 0x4000)  # sqrt(4) = 2
        self.assertEqual(f16_unary_cases.ref_sqrt(0x0001), 0x0C00)  # sqrt(2^-24) = 2^-12

        # ref_rsq
        self.assertEqual(f16_unary_cases.ref_rsq(0x0000), 0x7C00)
        self.assertEqual(f16_unary_cases.ref_rsq(0x8000), 0xFC00)
        self.assertEqual(f16_unary_cases.ref_rsq(0x7C00), 0x0000)
        self.assertEqual(f16_unary_cases.ref_rsq(0xFC00), 0xFE00)
        self.assertEqual(f16_unary_cases.ref_rsq(0xBC00), 0xFE00)
        self.assertEqual(f16_unary_cases.ref_rsq(0x7D01), 0x7F01)
        self.assertEqual(f16_unary_cases.ref_rsq(0x4400), 0x3800)  # rsq(4) = 0.5
        self.assertEqual(f16_unary_cases.ref_rsq(0x0001), 0x6C00)  # rsq(2^-24) = 2^12

        # ref_fract
        self.assertEqual(f16_unary_cases.ref_fract(0x0000), 0x0000)
        self.assertEqual(f16_unary_cases.ref_fract(0x8000), 0x0000)
        self.assertEqual(f16_unary_cases.ref_fract(0x7C00), 0xFE00)
        self.assertEqual(f16_unary_cases.ref_fract(0xFC00), 0xFE00)
        self.assertEqual(f16_unary_cases.ref_fract(0x7D01), 0x7F01)
        self.assertEqual(f16_unary_cases.ref_fract(0x3E00), 0x3800)  # fract(1.5) = 0.5
        self.assertEqual(f16_unary_cases.ref_fract(0x8001), 0x3BFF)  # tiny negative -> largest < 1.0

    def test_reference_ops_mask_upper_bits_above_16_bits(self):
        words = (0x17C00, 0x18000, 0x17E01, 0x13C00, 0xFFFFFC00)
        funcs = (
            f16_unary_cases.ref_rcp,
            f16_unary_cases.ref_sqrt,
            f16_unary_cases.ref_rsq,
            f16_unary_cases.ref_fract,
        )
        for fn in funcs:
            for w in words:
                with self.subTest(fn=fn.__name__, word=hex(w)):
                    got = fn(w)
                    self.assertEqual(got, fn(w & 0xFFFF))
                    self.assertTrue(0 <= got <= 0xFFFF)

    def test_exact_sqrt_ratio_domain_and_values(self):
        self.assertEqual(f16_unary_cases.exact_sqrt_ratio(0, 1), Fraction(0))
        self.assertEqual(f16_unary_cases.exact_sqrt_ratio(9, 4), Fraction(3, 2))
        self.assertEqual(f16_unary_cases.exact_sqrt_ratio(1, 16), Fraction(1, 4))
        approx = f16_unary_cases.exact_sqrt_ratio(2, 1)
        self.assertLess(Fraction(2) - approx * approx, Fraction(1, 1 << 63))

        invalid_args = (
            (-1, 1, 64),
            (0, 0, 64),
            (1, 0, 64),
            (1, -2, 64),
            (1, 1, -1),
            (True, 1, 64),
            (1, True, 64),
            (1, 1, False),
        )
        for args in invalid_args:
            with self.subTest(args=args):
                with self.assertRaises(ValueError):
                    f16_unary_cases.exact_sqrt_ratio(*args)


if __name__ == "__main__":
    unittest.main()
