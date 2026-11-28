"""A scalar load whose 64-bit base address is held in the vcc pair as data.

The console compiler moves a pointer into vcc and loads through it (`s_load_dwordx4 s[..], vcc, ..`): 79 of the
title's captured compute programs and 8 console stages were refused with "s_load_dwordx4 operands sgpr vcc". The base
is read from vcc_lo and vcc_hi like any SGPR pair. Hand-written program, no game data.

    python -I tests/semantic/test_smem_vcc_base.py -v
"""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

from support import rdna2spirv, translate_lines  # noqa: E402

PROGRAM = [
    "s_mov_b32 vcc_lo, s8",
    "s_mov_b32 vcc_hi, s9",
    "s_load_dwordx4 s[12:15], vcc, 0x10",
    "s_waitcnt lgkmcnt(0)",
    "v_mov_b32_e32 v0, s12",
    "v_mov_b32_e32 v1, s13",
    "exp mrt0 v0, v1, v0, v1 done vm",
    "s_endpgm",
]


class SmemVccBase(unittest.TestCase):
    def test_vcc_pair_is_a_base_address(self):
        spvasm = translate_lines("ps_smem_vcc_base", PROGRAM)
        self.assertIn("%vcc_hi", spvasm)  # the high word of the address comes from vcc_hi

    def test_a_lone_exec_base_is_still_refused(self):
        with self.assertRaises(rdna2spirv.Unsupported):
            translate_lines("ps_smem_exec_base", [line.replace(", vcc,", ", exec_lo,") for line in PROGRAM])


if __name__ == "__main__":
    unittest.main()
