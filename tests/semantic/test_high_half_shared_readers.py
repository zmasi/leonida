"""Regression tests ensuring all 16-bit high-half readers use Gen.u16_half / Gen.i16_half.

AMD Windows driver 25.10.36 (gfx1036) miscompiles `OpBitField{S,U}Extract ... 16 16` by
taking the offset mod 16 (reading the low half instead of the high half; docs/design.md
"16-bit high-half reads", #1506 / SHADER-777). Routing all remaining 16-bit half readers
through `Gen.u16_half` / `Gen.i16_half` eliminates every remaining offset-16 width-16
field extract while keeping bit-exact semantics:
  1. `v_cmp_*_{u16,i16}` / `v_cmpx_*_{u16,i16}` (`op_sel` high half)
  2. `Gen.sdwa_src` `WORD_1` (unsigned and `sext()`)
  3. `v_pk_{add,sub,mul_lo,mad,max,min}_{u16,i16}`
  4. `v_qsad_pk_u16_u8` / `v_mqsad_pk_u16_u8`
  5. `v_sad_u16`
  6. `v_mad_u32_u16` / `v_mad_i32_i16`
  7. `image_store` with `d16`

    python -I tests/semantic/test_high_half_shared_readers.py
"""
import gc
import os
import re
import struct
import sys
import tempfile
import unittest
import warnings

SEMANTIC_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(SEMANTIC_DIR))
TOOLS_DIR = os.environ.get("LEONIDA_TOOLS_DIR") or os.path.join(ROOT, "tools")
for _p in (TOOLS_DIR, SEMANTIC_DIR):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import rdna2spirv  # noqa: E402
import spirv_mini_interp as si  # noqa: E402

STUB_DXBC = b"DXBC" + bytes(24) + struct.pack("<I", 0) + bytes(4)
BAD_EXTRACT_16_16 = re.compile(r"OpBitField[SU]Extract\s+%(?:uint|int)\s+%\w+\s+%c10\s+%c10\b")


def translate_ps(name: str, body_lines: list[str]) -> str:
    with tempfile.TemporaryDirectory() as td:
        os.mkdir(os.path.join(td, "disasm"))
        src = os.path.join(td, "disasm", f"ps_{name}.s")
        with open(src, "w", encoding="utf-8", newline="\n") as f:
            f.write(f"; ps_{name}\n")
            for ln in body_lines:
                f.write(f"\t{ln}\n")
            f.write("\ts_endpgm\n")
        with open(os.path.join(td, f"ps_{name}.dxbc"), "wb") as f:
            f.write(STUB_DXBC)
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", ResourceWarning)
            spv = rdna2spirv.translate(src)
            gc.collect()
        return spv


def translate_cs(name: str, body_lines: list[str]) -> str:
    with tempfile.TemporaryDirectory() as td:
        os.mkdir(os.path.join(td, "disasm"))
        os.mkdir(os.path.join(td, "fssm"))
        src = os.path.join(td, "disasm", f"cs_{name}.s")
        with open(src, "w", encoding="utf-8", newline="\n") as f:
            f.write(f"; cs_{name}\n")
            for ln in body_lines:
                f.write(f"\t{ln}\n")
            f.write("\ts_endpgm\n")
        with open(os.path.join(td, f"cs_{name}.dxbc"), "wb") as f:
            f.write(STUB_DXBC)
        fssm = bytearray(0x148)
        fssm[0:4] = b"FSSM"
        struct.pack_into("<I", fssm, 0xD4, 0x20)
        struct.pack_into("<4I", fssm, 0x138, 0x8090, 32, 1, 1)
        with open(os.path.join(td, "fssm", f"cs_{name}.fssm"), "wb") as f:
            f.write(bytes(fssm))
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", ResourceWarning)
            spv = rdna2spirv.translate(src)
            gc.collect()
        return spv


def run_ps_u32(name: str, body_lines: list[str], out_reg: str = "v0") -> tuple[str, int]:
    spv = translate_ps(name, body_lines + [f"exp mrt0 {out_reg}, {out_reg}, {out_reg}, {out_reg} done vm"])
    return spv, si.run_fragment(spv)["%out_mrt0"][0]


class HighHalfSharedReadersTests(unittest.TestCase):
    def assertNoOffset16Extract(self, spv: str, label: str):
        self.assertIsNone(
            BAD_EXTRACT_16_16.search(spv),
            f"{label} still emitted OpBitField{{S,U}}Extract at offset 16 width 16",
        )

    def test_vcmp_u16_and_i16_high_half(self):
        # v1.hi = 0x9E00 (unsigned 40448, signed -25088), v1.lo = 0x0001
        # v2.hi = 0x41B1 (unsigned 16817, signed +16817), v2.lo = 0xFFFF
        # Unsigned: v1.hi > v2.hi -> 1. Signed: v1.hi < v2.hi -> 1.
        spv_u, got_u = run_ps_u32("vcmp_u16_hi", [
            "v_mov_b32 v1, 0x9e000001",
            "v_mov_b32 v2, 0x41b1ffff",
            "v_cmp_gt_u16_e64 vcc_lo, v1, v2 op_sel:[1,1]",
            "v_cndmask_b32_e32 v0, 0, 1, vcc_lo",
        ])
        self.assertNoOffset16Extract(spv_u, "v_cmp_gt_u16")
        self.assertIn("OpShiftRightLogical %uint", spv_u)
        self.assertEqual(got_u, 1)

        spv_i, got_i = run_ps_u32("vcmp_i16_hi", [
            "v_mov_b32 v1, 0x9e000001",
            "v_mov_b32 v2, 0x41b1ffff",
            "v_cmp_lt_i16_e64 vcc_lo, v1, v2 op_sel:[1,1]",
            "v_cndmask_b32_e32 v0, 0, 1, vcc_lo",
        ])
        self.assertNoOffset16Extract(spv_i, "v_cmp_lt_i16")
        self.assertIn("OpShiftRightArithmetic %int", spv_i)
        self.assertEqual(got_i, 1)

    def test_sdwa_word1_unsigned_and_sext(self):
        # Unsigned WORD_1: 0x9E001234 -> 0x00009E00; sext(WORD_1): 0x9E001234 -> 0xFFFF9E00.
        spv_u, got_u = run_ps_u32("sdwa_w1_u", [
            "v_mov_b32 v1, 0x9e001234",
            "v_mov_b32_sdwa v0, v1 dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:WORD_1",
        ])
        self.assertNoOffset16Extract(spv_u, "sdwa WORD_1 unsigned")
        self.assertIn("OpShiftRightLogical %uint", spv_u)
        self.assertEqual(got_u, 0x00009E00)

        spv_s, got_s = run_ps_u32("sdwa_w1_s", [
            "v_mov_b32 v1, 0x9e001234",
            "v_mov_b32_sdwa v0, sext(v1) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:WORD_1",
        ])
        self.assertNoOffset16Extract(spv_s, "sdwa WORD_1 sext")
        self.assertIn("OpShiftRightArithmetic %int", spv_s)
        self.assertEqual(got_s, 0xFFFF9E00)

    def test_v_pk_u16_and_i16_ops(self):
        # v1 = {0x9E00 (-25088), 0x0010 (16)}, v2 = {0x41B1 (16817), 0x0005 (5)}, v3 = {0x0003, 0x0002}
        cases = [
            ("v_pk_add_u16", "v_pk_add_u16 v0, v1, v2",
             (((0x9E00 + 0x41B1) & 0xFFFF) << 16) | ((0x0010 + 0x0005) & 0xFFFF)),
            ("v_pk_sub_i16", "v_pk_sub_i16 v0, v1, v2 clamp",
             (0x8000 << 16) | (16 - 5)),
            ("v_pk_mul_lo_u16", "v_pk_mul_lo_u16 v0, v1, v2",
             (((0x9E00 * 0x41B1) & 0xFFFF) << 16) | ((0x0010 * 0x0005) & 0xFFFF)),
            ("v_pk_min_i16", "v_pk_min_i16 v0, v1, v2",
             (0x9E00 << 16) | 0x0005),
            ("v_pk_max_u16", "v_pk_max_u16 v0, v1, v2",
             (0x9E00 << 16) | 0x0010),
            ("v_pk_mad_i16", "v_pk_mad_i16 v0, v1, v3, v2",
             ((((-25088 * 3) + 16817) & 0xFFFF) << 16) | (((16 * 2) + 5) & 0xFFFF)),
        ]
        for name, asm, expected in cases:
            with self.subTest(op=name):
                spv, got = run_ps_u32(name, [
                    "v_mov_b32 v1, 0x9e000010",
                    "v_mov_b32 v2, 0x41b10005",
                    "v_mov_b32 v3, 0x00030002",
                    asm,
                ])
                self.assertNoOffset16Extract(spv, name)
                self.assertEqual(got, expected)

    def test_v_mad_u32_u16_and_i32_i16_high_half(self):
        # v1.hi = 0xFFFE (-2 signed, 65534 unsigned), v2.hi = 0x0003, v3.hi = 0x0005
        spv_u, got_u = run_ps_u32("vmad_u32_u16", [
            "v_mov_b32 v1, 0xfffe0001",
            "v_mov_b32 v2, 0x00030002",
            "v_mov_b32 v3, 0x00050004",
            "v_mad_u32_u16 v0, v1, v2, v3 op_sel:[1,1,1,0]",
        ])
        self.assertNoOffset16Extract(spv_u, "v_mad_u32_u16")
        self.assertEqual(got_u, 65534 * 3 + 5)

        spv_i, got_i = run_ps_u32("vmad_i32_i16", [
            "v_mov_b32 v1, 0xfffe0001",
            "v_mov_b32 v2, 0x00030002",
            "v_mov_b32 v3, 0x00050004",
            "v_mad_i32_i16 v0, v1, v2, v3 op_sel:[1,1,1,0]",
        ])
        self.assertNoOffset16Extract(spv_i, "v_mad_i32_i16")
        self.assertIn("OpShiftRightArithmetic %int", spv_i)
        self.assertEqual(got_i, (-2 * 3 + 5) & 0xFFFFFFFF)

    def test_v_sad_u16_and_qsad_pk_u16_u8(self):
        # v_sad_u16: |0x1000 - 0x0500| + |0x0020 - 0x0005| + 0x100 = 0x0B00 + 0x001B + 0x0100 = 0x0C1B
        spv_sad, got_sad = run_ps_u32("vsad_u16", [
            "v_mov_b32 v1, 0x10000020",
            "v_mov_b32 v2, 0x05000005",
            "v_mov_b32 v3, 0x00000100",
            "v_sad_u16 v0, v1, v2, v3",
        ])
        self.assertNoOffset16Extract(spv_sad, "v_sad_u16")
        self.assertEqual(got_sad, 0x0C1B)

        # v_qsad_pk_u16_u8 with zero bytes in src0/src1 simply forwards the 4 packed 16-bit accumulators.
        spv_qsad, got_qsad = run_ps_u32("vqsad_pk", [
            "v_mov_b32 v2, 0",
            "v_mov_b32 v3, 0",
            "v_mov_b32 v4, 0",
            "v_mov_b32 v6, 0x12345678",
            "v_mov_b32 v7, 0x9abcdef0",
            "v_qsad_pk_u16_u8 v[0:1], v[2:3], v4, v[6:7]",
        ], out_reg="v0")
        self.assertNoOffset16Extract(spv_qsad, "v_qsad_pk_u16_u8")
        self.assertEqual(got_qsad, 0x12345678)

    def test_image_store_d16_uses_shared_high_half_shifts(self):
        spv = translate_cs("img_store_d16_hi", [
            "v_mov_b32 v0, 0x9e004000",
            "v_mov_b32 v1, 0x80003c00",
            "v_mov_b32 v2, 0",
            "v_mov_b32 v3, 0",
            "image_store v[0:1], v[2:3], s[0:7] dmask:0xf dim:SQ_RSRC_IMG_2D unorm d16",
        ])
        self.assertNoOffset16Extract(spv, "image_store d16")
        self.assertIn("OpShiftRightLogical %uint", spv)
        self.assertIn("OpShiftRightArithmetic %int", spv)


if __name__ == "__main__":
    unittest.main()
