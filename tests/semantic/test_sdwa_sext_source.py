"""An SDWA source under sext() is sign-extended with shifts, not OpBitFieldSExtract, and the conversion is unsigned.

``v_cvt_f32_u32_sdwa v4, sext(v1) src0_sel:BYTE_2`` sign-extends the selected field to 32 bits and converts that
32-bit pattern as unsigned: a negative field gives a value near 2**32, not a small negative number. A translated
OpBitFieldSExtract feeding OpConvertUToF came back converted as signed on an NVIDIA driver (RTX 4050 Laptop, driver
621.01: ``v1 = 0x4fbdc08a`` gave -67.0 where the model has 0x4f800000), in 7 of 400 programs of
``tools/sdwa_diff.py --ops v_cvt_f32_u32`` judged by ``tools/vk_model_fuzz.py``. Two shifts give the same bits and the
same 400 programs then match. The CPU interpreter cannot show the driver bug, so this test pins the two things it can:
the SPIR-V has no OpBitFieldSExtract for an SDWA source, and every selector gives the value the model gives.

    python -I tests/semantic/test_sdwa_sext_source.py
"""
import os
import struct
import sys
import tempfile
import unittest
import warnings

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.dont_write_bytecode = True
sys.path.insert(0, os.environ.get("LEONIDA_TOOLS_DIR") or os.path.join(ROOT, "tools"))
sys.path.insert(0, HERE)

import rdna2spirv  # noqa: E402
import spirv_mini_interp as si  # noqa: E402

STUB_DXBC = b"DXBC" + bytes(24) + struct.pack("<I", 0) + bytes(4)
BASE = 0x1000
SELECTS = {"BYTE_0": (0, 8), "BYTE_1": (8, 8), "BYTE_2": (16, 8), "BYTE_3": (24, 8),
           "WORD_0": (0, 16), "WORD_1": (16, 16)}
VALUES = (0x00000000, 0x7F7F7F7F, 0x80808080, 0xFFFFFFFF, 0x4FBDC08A, 0xBFCF9CFC, 0x80FF7F01, 0x7F80FF00, 0x12348765)


def sext_field(value, select):
    off, width = SELECTS[select]
    field = (value >> off) & ((1 << width) - 1)
    if field >> (width - 1):
        field -= 1 << width
    return field & 0xFFFFFFFF


def unsigned_to_f32_bits(word):
    return struct.unpack("<I", struct.pack("<f", float(word)))[0]


def translate(rows):
    """The SPIR-V text of one pixel shader that runs every row and stores v4 at BASE + 4k."""
    body = [f"s_mov_b32 s0, {BASE:#x}", "s_mov_b32 s1, 0", "s_mov_b32 s2, 0x1000", "s_mov_b32 s3, 0x31016fac"]
    for k, (line, v1) in enumerate(rows):
        body += [f"v_mov_b32 v1, {v1:#x}", "v_mov_b32 v4, 0xdeadbeef", line,
                 f"buffer_store_dword v4, off, s[0:3], 0 offset:{4 * k}"]
    body += ["exp mrt0 v4, v4, v4, v4 done vm", "s_endpgm"]
    with tempfile.TemporaryDirectory() as tmp:
        os.mkdir(os.path.join(tmp, "disasm"))
        src = os.path.join(tmp, "disasm", "ps_sdwa_sext.s")
        with open(os.path.join(tmp, "ps_sdwa_sext.dxbc"), "wb") as f:
            f.write(STUB_DXBC)
        with open(src, "w", encoding="utf-8") as f:
            f.write("; test_sdwa_sext_source\n" + "".join(f"\t{x}\n" for x in body))
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", ResourceWarning)
            return rdna2spirv.translate(src)


class SdwaSextSource(unittest.TestCase):
    ROWS = [(f"v_cvt_f32_u32_sdwa v4, sext(v1) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:{sel}", v1, sel)
            for sel in SELECTS for v1 in VALUES]

    @classmethod
    def setUpClass(cls):
        cls.spv = translate([(line, v1) for line, v1, _ in cls.ROWS])

    def test_no_bitfield_sext_for_an_sdwa_source(self):
        self.assertNotIn("OpBitFieldSExtract", self.spv)

    def test_unsigned_convert_of_the_sign_extended_field(self):
        memory = {}
        si.run_fragment(self.spv, memory=memory)
        for k, (line, v1, sel) in enumerate(self.ROWS):
            with self.subTest(sel=sel, v1=f"{v1:#010x}"):
                self.assertEqual(memory.get(BASE + 4 * k), unsigned_to_f32_bits(sext_field(v1, sel)))

    def test_the_case_the_nvidia_driver_got_wrong(self):
        # v1 = 0x4fbdc08a, BYTE_2 = 0xbd: sign-extended 0xffffffbd converts, unsigned, to 2**32 (0x4f800000), not -67.0.
        self.assertEqual(sext_field(0x4FBDC08A, "BYTE_2"), 0xFFFFFFBD)
        self.assertEqual(unsigned_to_f32_bits(0xFFFFFFBD), 0x4F800000)

    def test_zero_extended_sources_still_use_the_unsigned_extract(self):
        spv = translate([("v_cvt_f32_u32_sdwa v4, v1 dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2", 0x4FBDC08A)])
        self.assertIn("OpBitFieldUExtract", spv)
        memory = {}
        si.run_fragment(spv, memory=memory)
        self.assertEqual(memory.get(BASE), unsigned_to_f32_bits(0xBD))


if __name__ == "__main__":
    unittest.main()
