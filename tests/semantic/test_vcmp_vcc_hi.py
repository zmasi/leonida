"""A wave32 VOP3 compare that writes its lane mask to vcc_hi.

In wave32 code vcc_hi is an ordinary SGPR, and the console compiler puts compare masks there (5,907 VOP3 compares
in the title's captured compute programs; 606 programs were refused with "v_cmp destination"). The translator stores
the mask word in %vcc_hi, which a later read as data or as a VOP3 lane mask takes back. Hand-written program, no game
data.

    python -I tests/semantic/test_vcmp_vcc_hi.py -v
"""
import os
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

from support import mrt0_words, translate_lines  # noqa: E402

ONE = struct.unpack("<I", struct.pack("<f", 1.0))[0]


def program(a, b):
    """v_cmp_lt_f32 into vcc_hi, then a select through vcc_hi as the lane mask, exported to mrt0."""
    return [
        f"v_mov_b32_e32 v1, {a}",
        f"v_mov_b32_e32 v2, {b}",
        "v_cmp_lt_f32_e64 vcc_hi, v1, v2",
        "v_cndmask_b32_e64 v3, 0, 1.0, vcc_hi",
        "v_mov_b32_e32 v4, 0",
        "exp mrt0 v3, v4, v4, v4 done vm",
        "s_endpgm",
    ]


class VccHiCompare(unittest.TestCase):
    def test_true_compare_sets_the_lane(self):
        words = mrt0_words(translate_lines("ps_vcmp_vcc_hi_true", program("1.0", "2.0")))
        self.assertEqual(words[0], ONE)

    def test_false_compare_clears_the_lane(self):
        words = mrt0_words(translate_lines("ps_vcmp_vcc_hi_false", program("2.0", "1.0")))
        self.assertEqual(words[0], 0)


if __name__ == "__main__":
    unittest.main()
