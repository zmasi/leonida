"""Synthetic conversion, subnormal, and clamp tests for RDNA2 binary16 VALU instructions."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import support
import spirv_asm
import spirv_mini_interp as si


HALF_TO_FLOAT = si.half_to_float


def conversion_fragment(op, cases):
    """Each case supplies a VGPR word and an operand string with {src} naming that VGPR."""
    lines = []
    for i, (word, operands) in enumerate(cases):
        src = f"v{i + 8}"
        lines.extend([f"v_mov_b32 {src}, {word:#x}", f"v_mov_b32 v{i}, 0xaaaa5555",
                      f"{op} v{i}, {operands.format(src=src)}"])
    lines += ["exp mrt0 v0, v1, v2, v3 done vm", "s_endpgm"]
    return support.translate_lines("ps_cvt_f16_denorm_mods", lines)


def flushed_unpack(word):
    """Model a driver's legal UnpackHalf2x16 choice of flushing binary16 subnormals."""
    half = word & 0xFFFF
    if 0 < (half & 0x7FFF) < 0x0400:
        half &= 0x8000
    return HALF_TO_FLOAT(half)

SDWA_WIDEN = "dst_sel:DWORD dst_unused:UNUSED_PAD"
SDWA_CASES = [
    (0xDEAD0001, "{src} " + SDWA_WIDEN + " src0_sel:BYTE_0"),
    (0xDEAD0100, "{src} " + SDWA_WIDEN + " src0_sel:BYTE_1"),
    (0x80013C00, "|{src}| " + SDWA_WIDEN + " src0_sel:WORD_1"),
    (0, "2.0 " + SDWA_WIDEN + " src0_sel:BYTE_1"),
]
NARROW_CLAMP_CASES = [
    (0x33800000, "{src} clamp"),       # smallest positive subnormal
    (0x387FC000, "{src} clamp"),       # largest positive subnormal
    (0x80000000, "{src} clamp"),       # -0 saturates to +0
    (0x7FA02000, "{src} clamp"),       # DX10 clamp maps NaN to +0
]
DPP_LINES = [
    "v_mbcnt_lo_u32_b32 v0, -1, 0",
    "v_xor_b32 v1, 1, v0",
    "v_mov_b32 v2, 0xdeadbeef",
    "v_mov_b32 v3, 0xaaaa5555",
    "s_mov_b32 exec_lo, 13",          # lanes 0, 2, 3
    "v_cvt_f32_f16_dpp v2, -v1 quad_perm:[2,3,0,1] row_mask:0xf bank_mask:0xf bound_ctrl:1",
    "v_cvt_f16_f32_e64 v3, 0x33800000 clamp",
    "s_mov_b32 exec_lo, -1",
    "v_mov_b32 v4, 0",
    "exp pos0 v4, v4, v4, v4 done",
    "exp param0 v2, v3, v4, v4",
    "s_endpgm",
]


class CvtF16DenormModsTests(unittest.TestCase):
    def test_widen_finite_values_and_signed_zero(self):
        spv = conversion_fragment("v_cvt_f32_f16_e64", [
            (0xAAAA3E00, "{src}"), (0xAAAA0000, "-{src}"),
            (0xAAAA8000, "{src}"), (0xAAAA8000, "|{src}|"),
        ])
        self.assertEqual(support.mrt0_words(spv), [0x3FC00000, 0x80000000, 0x80000000, 0])

    def test_widen_subnormals_even_if_unpack_flushes_them(self):
        spv = conversion_fragment("v_cvt_f32_f16_e64", [
            (0x0001, "{src}"), (0x03FF, "{src}"), (0x8001, "{src}"), (0x8001, "-|{src}|"),
        ])
        with patch.object(si, "half_to_float", flushed_unpack):
            self.assertEqual(support.mrt0_words(spv), [0x33800000, 0x387FC000, 0xB3800000, 0xB3800000])

    def test_widen_sdwa_before_modifiers_and_inline_float(self):
        spv = conversion_fragment("v_cvt_f32_f16_sdwa", SDWA_CASES)
        with patch.object(si, "half_to_float", flushed_unpack):
            self.assertEqual(support.mrt0_words(spv), [0x33800000, 0x33800000, 0x33800000, 0x36800000])

    def test_widen_nan_payload_sign_and_source_modifiers(self):
        spv = conversion_fragment("v_cvt_f32_f16_e64", [
            (0xFC01, "{src}"), (0xFE25, "-{src} mul:2"),
            (0xFE25, "|{src}|"), (0xFE25, "-|{src}|"),
        ])
        self.assertEqual(support.mrt0_words(spv), [0xFF802000, 0x7FC4A000, 0x7FC4A000, 0xFFC4A000])

    def test_widen_clamp_and_infinities(self):
        spv = conversion_fragment("v_cvt_f32_f16_e64", [
            (0x7C00, "{src}"), (0xFC00, "{src}"), (0x7E25, "{src} clamp"), (0xFC00, "{src} clamp"),
        ])
        self.assertEqual(support.mrt0_words(spv), [0x7F800000, 0xFF800000, 0, 0])

    def test_widen_omod_precedes_clamp(self):
        spv = conversion_fragment("v_cvt_f32_f16_e64", [
            (0x3A00, "{src} clamp mul:2"), (0, "0.5 mul:4"),
            (0x0001, "{src} div:2"), (0x8001, "-|{src}| clamp mul:4"),
        ])
        self.assertEqual(support.mrt0_words(spv), [0x3F800000, 0x40000000, 0x33000000, 0])

    def test_dpp_selects_before_modifiers_and_keeps_inactive_lanes(self):
        spv = support.translate_lines("vs_cvt_f16_dpp", DPP_LINES)
        _, outputs = si.run_workgroup(spv, 32, 32, inputs_for=lambda lane: {"%sg_lane": lane}, return_outputs=True)
        words = [tuple(out["%out_param0"][:2]) for out in outputs]
        self.assertEqual(words[:4], [(0xB4400000, 0xAAAA0001), (0xDEADBEEF, 0xAAAA5555),
                                     (0xB3800000, 0xAAAA0001), (0x80000000, 0xAAAA0001)])

    def test_narrow_rounds_to_even_and_keeps_high_half(self):
        spv = conversion_fragment("v_cvt_f16_f32_e64", [
            (0x3F801000, "{src}"), (0x3F801001, "{src}"),
            (0x33C00000, "{src}"), (0x477FF000, "{src}"),
        ])
        self.assertEqual(support.mrt0_words(spv), [0xAAAA3C00, 0xAAAA3C01, 0xAAAA0002, 0xAAAA7C00])

    def test_narrow_clamp_keeps_positive_subnormal_bits(self):
        spv = conversion_fragment("v_cvt_f16_f32_e64", NARROW_CLAMP_CASES)
        self.assertNotIn("UnpackHalf2x16", spv)
        self.assertEqual(support.mrt0_words(spv), [0xAAAA0001, 0xAAAA03FF, 0xAAAA0000, 0xAAAA0000])

    def test_narrow_omod_runs_on_the_half_before_clamp(self):
        spv = conversion_fragment("v_cvt_f16_f32_e64", [
            (0x3E800000, "{src} mul:2"), (0x3F400000, "{src} clamp mul:4"),
            (0x3F400000, "{src} clamp div:2"), (0x38800000, "{src} clamp div:2"),
        ])
        self.assertEqual(support.mrt0_words(spv), [0xAAAA3800, 0xAAAA3C00, 0xAAAA3600, 0xAAAA0000])

    def test_narrow_sdwa_clamps_before_merging_the_destination(self):
        keep = "clamp dst_sel:WORD_1 dst_unused:UNUSED_PRESERVE src0_sel:DWORD"
        spv = conversion_fragment("v_cvt_f16_f32_sdwa", [
            (0x33800000, "{src} " + keep), (0xC0000000, "{src} " + keep),
            (0x40000000, "{src} " + keep), (0x7FC00000, "{src} " + keep),
        ])
        self.assertEqual(support.mrt0_words(spv), [0x00015555, 0x00005555, 0x3C005555, 0x00005555])

    def test_div_fixup_clamps_the_half_before_writing_it(self):
        spv = support.translate_lines("ps_div_fixup_f16_bit_clamp", [
            "v_mov_b32 v0, 0xaaaa5555", "v_mov_b32 v1, v0", "v_mov_b32 v2, v0", "v_mov_b32 v3, v0",
            "v_mov_b32 v8, 0x03ff0001", "v_mov_b32 v9, 0x3c00", "v_mov_b32 v10, 0xbc00",
            "v_div_fixup_f16 v0, v8, v9, v9 clamp",
            "v_div_fixup_f16 v1, v8, v9, v9 op_sel:[1,0,0,1] clamp",
            "v_div_fixup_f16 v2, v8, v9, v10 clamp",
            "v_div_fixup_f16 v3, 0, v9, v10 clamp",
            "exp mrt0 v0, v1, v2, v3 done vm", "s_endpgm",
        ])
        self.assertNotIn("UnpackHalf2x16", spv)
        self.assertEqual(support.mrt0_words(spv), [0xAAAA0001, 0x03FF5555, 0xAAAA0000, 0xAAAA0000])

    @unittest.skipUnless(shutil.which("spirv-val"), "spirv-val not installed")
    def test_spirv_validation_with_both_assemblers(self):
        modules = {
            "sdwa": conversion_fragment("v_cvt_f32_f16_sdwa", SDWA_CASES),
            "clamp": conversion_fragment("v_cvt_f16_f32_e64", NARROW_CLAMP_CASES),
            "dpp": support.translate_lines("vs_cvt_f16_dpp", DPP_LINES),
        }
        with tempfile.TemporaryDirectory() as tmp:
            for name, spv in modules.items():
                binary = Path(tmp, name + ".spv")
                for assembler in ("python", "spirv-as"):
                    with self.subTest(module=name, assembler=assembler):
                        if assembler == "python":
                            binary.write_bytes(spirv_asm.assemble(spv))
                        elif shutil.which(assembler):
                            made = subprocess.run([assembler, "-", "--target-env", "vulkan1.2", "-o", str(binary)],
                                                  input=spv, capture_output=True, text=True)
                            self.assertEqual(made.returncode, 0, made.stderr)
                        else:
                            self.skipTest("spirv-as not installed")
                        checked = subprocess.run(["spirv-val", "--target-env", "vulkan1.2", str(binary)],
                                                 capture_output=True, text=True)
                        self.assertEqual(checked.returncode, 0, checked.stderr)


if __name__ == "__main__":
    unittest.main()
