"""Standalone unit tests for RDNA2 ISA opcode tables (tools/rdna2_isa_tables.py).

Validates structural integrity, opcode bounds, mnemonic/operand schemas,
cross-table invariants (e.g. VOP3/VOP3B disjointness and VOP3 promoted parity),
cross-checks representative opcodes against tools/rdna2_decode.py across all
major RDNA2 encodings, and includes negative controls on mutated tables and
unmapped opcodes. Python 3 stdlib-only, no game files.
"""
from __future__ import annotations

import os
import re
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TOOLS = os.path.join(ROOT, "tools")

sys.dont_write_bytecode = True
if TOOLS not in sys.path:
    sys.path.insert(0, TOOLS)

import rdna2_decode  # noqa: E402
import rdna2_isa_tables as T  # noqa: E402

MNEMONIC_RE = re.compile(r"^[a-z][a-z0-9_]*$")
ROLE_RE = re.compile(r"^[A-Z][A-Z0-9_]*$")
VALID_WIDTHS = frozenset({-1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 16})
VALID_DTYPES = frozenset({"i", "f16", "f32", "f64"})

# Expected tables and inclusive (min_opcode, max_opcode) hardware field bounds.
TABLE_BOUNDS = {
    "DS": (0x0, 0xFF),
    "FLAT": (0x0, 0x7F),
    "GLOBAL": (0x0, 0x7F),
    "MIMG": (0x0, 0xFF),
    "MTBUF": (0x0, 0xF),
    "MUBUF": (0x0, 0xFF),
    "SCRATCH": (0x0, 0x7F),
    "SMEM": (0x0, 0xFF),
    "SOP1": (0x0, 0x7F),
    "SOP2": (0x0, 0x7F),
    "SOPC": (0x0, 0x7F),
    "SOPK": (0x0, 0x1F),
    "SOPP": (0x0, 0x7F),
    "VINTRP": (0x0, 0x3),
    "VOP1": (0x0, 0xFF),
    "VOP2": (0x0, 0x3F),
    "VOP3": (0x0, 0x3FF),
    "VOP3B": (0x0, 0x3FF),
    "VOP3P": (0x0, 0x7F),
    "VOPC": (0x0, 0xFF),
}


def words(*ws: int) -> bytes:
    """Pack 32-bit little-endian instruction words."""
    return b"".join(struct.pack("<I", w & 0xFFFFFFFF) for w in ws)


def validate_table(name: str, table: dict, bounds: tuple[int, int]) -> None:
    """Validate structure, opcode bounds, mnemonics, and operand specs of one ISA table."""
    if not isinstance(table, dict) or not table:
        raise AssertionError(f"{name}: expected non-empty dict, got {type(table).__name__}")
    lo, hi = bounds
    seen_mnemonics: dict[str, int] = {}
    for opc, entry in table.items():
        if not isinstance(opc, int) or isinstance(opc, bool) or not (lo <= opc <= hi):
            raise AssertionError(f"{name}: opcode {opc!r} out of bounds [{lo:#x}, {hi:#x}]")
        if not isinstance(entry, tuple) or len(entry) != 2:
            raise AssertionError(f"{name}[{opc:#x}]: expected 2-tuple, got {entry!r}")
        mnem, spec = entry
        if not isinstance(mnem, str) or not MNEMONIC_RE.match(mnem):
            raise AssertionError(f"{name}[{opc:#x}]: invalid mnemonic {mnem!r}")
        if mnem in seen_mnemonics:
            raise AssertionError(
                f"{name}[{opc:#x}]: duplicate mnemonic {mnem!r} (first at {seen_mnemonics[mnem]:#x})"
            )
        seen_mnemonics[mnem] = opc
        if not isinstance(spec, tuple):
            raise AssertionError(f"{name}[{opc:#x}] ({mnem}): spec must be tuple, got {type(spec).__name__}")
        for idx, op_spec in enumerate(spec):
            if not isinstance(op_spec, tuple) or len(op_spec) != 3:
                raise AssertionError(f"{name}[{opc:#x}] ({mnem}) arg {idx}: invalid spec {op_spec!r}")
            role, width, dtype = op_spec
            if not isinstance(role, str) or not ROLE_RE.match(role):
                raise AssertionError(f"{name}[{opc:#x}] ({mnem}) arg {idx}: invalid role {role!r}")
            if not isinstance(width, int) or isinstance(width, bool) or width not in VALID_WIDTHS:
                raise AssertionError(f"{name}[{opc:#x}] ({mnem}) arg {idx}: invalid width {width!r}")
            if dtype not in VALID_DTYPES:
                raise AssertionError(f"{name}[{opc:#x}] ({mnem}) arg {idx}: invalid dtype {dtype!r}")


class Rdna2IsaTablesTests(unittest.TestCase):
    def test_all_expected_tables_valid(self):
        for name, bounds in TABLE_BOUNDS.items():
            with self.subTest(table=name):
                self.assertTrue(hasattr(T, name), f"Missing table {name}")
                validate_table(name, getattr(T, name), bounds)

    def test_vop3_and_vop3b_disjoint(self):
        overlap = set(T.VOP3) & set(T.VOP3B)
        self.assertEqual(overlap, set(), f"Overlapping VOP3/VOP3B opcodes: {sorted(overlap)}")

    def test_vop3_promoted_mnemonic_parity(self):
        v3_all = {**T.VOP3, **T.VOP3B}
        for opc, (mnem, _) in T.VOPC.items():
            if opc in v3_all:
                with self.subTest(encoding="VOPC", opc=hex(opc)):
                    self.assertEqual(v3_all[opc][0], mnem)
        for opc, (mnem, _) in T.VOP2.items():
            if (0x100 + opc) in v3_all:
                with self.subTest(encoding="VOP2", opc=hex(opc)):
                    self.assertEqual(v3_all[0x100 + opc][0], mnem)
        for opc, (mnem, _) in T.VOP1.items():
            if (0x180 + opc) in v3_all:
                with self.subTest(encoding="VOP1", opc=hex(opc)):
                    self.assertEqual(v3_all[0x180 + opc][0], mnem)

    def test_cross_check_with_rdna2_decode(self):
        # Representative encoded words across all major RDNA2 encodings:
        # (encoding_label, table_name_or_None, opcode_key_or_None, raw_bytes, expected_op)
        cases = [
            ("SOP2", "SOP2", 0x0, words(0x80020100), "s_add_u32"),
            ("SOPK", "SOPK", 0x0, words(0xB0020005), "s_movk_i32"),
            ("SOP1", "SOP1", 0x3, words(0xBE820301), "s_mov_b32"),
            ("SOPC", "SOPC", 0x0, words(0xBF000201), "s_cmp_eq_i32"),
            ("SOPP", "SOPP", 0x0, words(0xBF800000), "s_nop"),
            ("VOP2", "VOP2", 0x3, words(0x06020301), "v_add_f32_e32"),
            ("VOP1", "VOP1", 0x1, words(0x7E020301), "v_mov_b32_e32"),
            ("VOPC", "VOPC", 0x0, words(0x7C000301), "v_cmp_f_f32_e32"),
            ("VINTRP", "VINTRP", 0x0, words(0xC8000001), "v_interp_p1_f32"),
            ("VOP3", "VOP3", 0x103, words(0xD5030001, 0x00020302), "v_add_f32_e64"),
            ("VOP3B", "VOP3B", 0x128, words(0xD5280101, 0x00020302), "v_add_co_ci_u32_e64"),
            ("VOP3P", "VOP3P", 0x0, words(0xCC000001, 0x00020302), "v_pk_mad_i16"),
            ("DS", "DS", 0x36, words(0xD8D80000, 0x01000002), "ds_read_b32"),
            ("FLAT", "FLAT", 0xC, words(0xDC300000, 0x01000002), "flat_load_dword"),
            ("SCRATCH", "SCRATCH", 0xC, words(0xDC304000, 0x01000002), "scratch_load_dword"),
            ("GLOBAL", "GLOBAL", 0xC, words(0xDC308000, 0x01000002), "global_load_dword"),
            ("MUBUF", "MUBUF", 0x0, words(0xE0000000, 0x00000102), "buffer_load_format_x"),
            ("MTBUF", "MTBUF", 0x0, words(0xE8000000, 0x00000102), "tbuffer_load_format_x"),
            ("MIMG", "MIMG", 0x0, words(0xF0000108, 0x00000201), "image_load"),
            ("SMEM", "SMEM", 0x0, words(0xF4000040, 0xFA000000), "s_load_dword"),
            ("EXP", None, None, words(0xF800000F, 0x04030201), "exp"),
        ]
        for enc, tbl_name, opc, code, expected_op in cases:
            with self.subTest(encoding=enc):
                if tbl_name is not None:
                    tbl = getattr(T, tbl_name)
                    self.assertIn(opc, tbl)
                    base_mnem = tbl[opc][0]
                    self.assertIn(
                        expected_op,
                        (base_mnem, base_mnem + "_e32", base_mnem + "_e64"),
                        f"{enc}: expected_op {expected_op!r} incompatible with table {base_mnem!r}",
                    )
                res = rdna2_decode.decode_bytes(code, wave32=True)
                self.assertEqual(len(res["insns"]), 1, f"{enc}: decode failed: {res}")
                self.assertEqual(res["insns"][0]["op"], expected_op)

    def test_negative_controls_mutated_tables_and_unmapped_opcodes(self):
        # 1. Mutated table entries must fail validate_table.
        bad_mnem = {0x0: ("BAD_MNEM", ())}
        with self.assertRaises(AssertionError):
            validate_table("SOPP", bad_mnem, TABLE_BOUNDS["SOPP"])

        out_of_bounds = {0x80: ("s_fake", ())}
        with self.assertRaises(AssertionError):
            validate_table("SOPP", out_of_bounds, TABLE_BOUNDS["SOPP"])

        bad_spec = {0x0: ("s_fake", (("VDST", "not_int", "i"),))}
        with self.assertRaises(AssertionError):
            validate_table("SOPP", bad_spec, TABLE_BOUNDS["SOPP"])

        dup_mnem = {0x0: ("s_dup", ()), 0x1: ("s_dup", ())}
        with self.assertRaises(AssertionError):
            validate_table("SOPP", dup_mnem, TABLE_BOUNDS["SOPP"])

        # 2. Unmapped opcode in SOP1 (0x7F is not in T.SOP1) must not decode.
        self.assertNotIn(0x7F, T.SOP1)
        unmapped_sop1 = words(0xBE800000 | (0x7F << 8))
        res = rdna2_decode.decode_bytes(unmapped_sop1, wave32=True)
        self.assertEqual(res["insns"], [])


if __name__ == "__main__":
    unittest.main()
