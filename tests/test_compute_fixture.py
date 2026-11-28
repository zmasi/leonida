"""The synthetic compute shader metadata reaches the translator's workgroup declaration."""

from pathlib import Path
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "runtime" / "test" / "shaders"))
sys.path.insert(0, str(ROOT / "tools"))

from compute_fixture import compute_header, shift_user_data, shifted_rsrc2  # noqa: E402
from rdna2spirv import translate  # noqa: E402


class ComputeFixtureTests(unittest.TestCase):
    def test_group_tuple_whitespace_reaches_spirv(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            disasm = root / "disasm"
            fssm = root / "fssm"
            disasm.mkdir()
            fssm.mkdir()
            for index, group in enumerate(("4,2,1", "4, 2, 1", "4 , 2,\t1")):
                with self.subTest(group=group):
                    shader = f"cs_group_{index}"
                    listing = disasm / f"{shader}.s"
                    listing.write_text(f"; group: {group}\n\ts_endpgm\n", encoding="utf-8", newline="\n")
                    (fssm / f"{shader}.fssm").write_bytes(compute_header(listing))
                    assembly = translate(str(listing))
                    local_size = next(line.strip() for line in assembly.splitlines()
                                      if line.lstrip().startswith("OpExecutionMode %main LocalSize "))
                    self.assertEqual(local_size, "OpExecutionMode %main LocalSize 4 2 1")

    def test_utf8_bom_asm_and_group_bounds(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            bom_asm = Path(tmp) / "bom.s"
            bom_asm.write_bytes(b"\xef\xbb\xbf; group: 8, 2, 1\n\ts_endpgm\n")
            hdr = compute_header(bom_asm)
            self.assertEqual(struct.unpack_from("<4I", hdr, 0x138), (0x8088, 8, 2, 1))

            for bad_group in ("0, 1, 1", "-1, 1, 1", f"{0x100000000}, 1, 1", "1, 0, 1", "1, 1, 0"):
                with self.subTest(bad_group=bad_group):
                    bad = Path(tmp) / "bad.s"
                    bad.write_text(f"; group: {bad_group}\n\ts_endpgm\n", encoding="utf-8", newline="\n")
                    with self.assertRaises(ValueError):
                        compute_header(bad)

    def test_rejects_negative_bool_shift_and_inverted_sgpr_ranges(self) -> None:
        for bad_shift in (-1, True, False):
            with self.subTest(bad_shift=bad_shift):
                with self.assertRaises(ValueError):
                    shift_user_data("s_mov_b32 s0, 0\n", bad_shift)
                with self.assertRaises(ValueError):
                    shifted_rsrc2(0x8088, bad_shift)

        for shift in (0, 4):
            with self.subTest(inverted_with_shift=shift):
                with self.assertRaises(ValueError):
                    shift_user_data("s_load_dwordx4 s[7:4], s[0:1], 0x0\n", shift)


if __name__ == "__main__":
    unittest.main()
