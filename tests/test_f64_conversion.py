"""Check the SPIR-V data flow for output modifiers on v_cvt_f32_f64."""

import re
import struct
import sys
import tempfile
import unittest
from pathlib import Path

TEST_DIR = Path(__file__).resolve().parent
sys.path[:0] = [str(TEST_DIR.parent / "tools"), str(TEST_DIR)]

from listing import translate_listing

FIXTURE = TEST_DIR / "fixtures" / "f64_conversion.asm"


class F64ConversionTest(unittest.TestCase):
    def test_output_modifiers(self):
        body = "\n".join(
            line for line in FIXTURE.read_text(encoding="utf-8").splitlines()
            if not line.startswith(";")
        )
        cases = (
            ("", None, False),
            ("mul:2", 0x40000000, False),
            ("mul:4", 0x40800000, False),
            ("div:2", 0x3F000000, False),
            ("clamp", None, True),
            ("clamp div:2", 0x3F000000, True),
        )
        with tempfile.TemporaryDirectory() as directory:
            for modifiers, scale_bits, clamp in cases:
                with self.subTest(modifiers=modifiers):
                    spv = translate_listing(Path(directory), f"{body} {modifiers}".rstrip())
                    self.check_conversion(spv, scale_bits, clamp)

    def check_conversion(self, spv, scale_bits, clamp):
        def result(instruction):
            match = re.search(rf"^ *(%\w+) = {instruction}$", spv, re.M)
            self.assertIsNotNone(match, f"Missing SPIR-V instruction: {instruction}")
            return match.group(1)

        value = result(r"OpFConvert %float %\w+")
        if scale_bits is not None:
            # The scale is one OpConstant %float named by its bits (Gen.fconst_bits), not a uint constant bitcast at
            # the use; its literal must give back exactly the 32 bits of the omod scale.
            scale = f"%fc{scale_bits:x}"
            match = re.search(rf"^ *{scale} = OpConstant %float (\S+)$", spv, re.M)
            self.assertIsNotNone(match, f"Missing SPIR-V instruction: {scale} = OpConstant %float")
            self.assertEqual(struct.unpack("<I", struct.pack("<f", float(match.group(1))))[0], scale_bits)
            value = result(rf"OpFMul %float {value} {scale}")
        if clamp:
            # DX10_CLAMP: a NaN is replaced by +0 before FClamp (Gen.clamp_mods).
            nan = result(rf"OpIsNan %bool {value}")
            value = result(rf"OpSelect %float {nan} %f0 {value}")
            value = result(rf"OpExtInst %float %glsl FClamp {value} %f0 %f1")
        word = result(rf"OpBitcast %uint {value}")
        dest = result(r"OpAccessChain %_ptr_Function_uint %vgpr %c3")
        # This straight-line fixture keeps EXEC full, so the result is stored directly.
        self.assertRegex(spv, rf"(?m)^ *OpStore {dest} {word}$")


if __name__ == "__main__":
    unittest.main()
