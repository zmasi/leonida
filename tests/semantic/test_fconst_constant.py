"""Gen.fconst gives an OpConstant %float with the exact f32 bits, one per value, not an OpBitcast at every use.

Until 2026-11-27 fconst bitcast the uint constant of the bits at every use (4.3% of the instructions of a large
RaytracingResolve module). An OpConstant %float takes the 32-bit word literally, so the value is the same as long as
the literal spells exactly those bits: rdna2spirv.float_literal writes the shortest decimal of a finite value (-0.0
and the denormals too) and the SPIRV-Tools hex-float spelling of an infinity or a NaN, payload as stored. Checked here
through tools/spirv_asm.py, which reads float literals as spirv-as does, and the mini interpreter.
"""
import re
import struct
import sys
import unittest

import support
from spirv_mini_interp import f32_bits, f32_literal, run_fragment

import rdna2spirv

sys.path.insert(0, support.TOOLS_DIR)
import spirv_asm  # noqa: E402

# +-0, the smallest and largest denormals, the smallest normal, 1 and its neighbour, the largest finite values,
# +-infinity, quiet and signalling NaNs with payloads and both signs, and a few ordinary values.
TRICKY = (0x00000000, 0x80000000, 0x00000001, 0x80000001, 0x007FFFFF, 0x807FFFFF, 0x00800000, 0x3F800000,
          0x3F800001, 0x7F7FFFFF, 0xFF7FFFFF, 0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00000, 0x7F800001,
          0xFF800001, 0x7FBFFFFF, 0xFFFFFFFF, 0x7FC00001, 0x40C90FDB, 0x1F800000, 0x5F800000, 0xBF000000)


def bits_of(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


def value_of(bits):
    return struct.unpack("<f", struct.pack("<I", bits))[0]


class FloatLiteral(unittest.TestCase):
    def test_the_literal_assembles_to_the_same_bits(self):
        for bits in TRICKY:
            with self.subTest(bits=f"{bits:#010x}"):
                lit = rdna2spirv.float_literal(bits)
                self.assertEqual(spirv_asm._parse_float_bits(lit, 32, 1), bits, lit)

    def test_the_interpreter_reads_the_same_bits(self):
        for bits in TRICKY:
            with self.subTest(bits=f"{bits:#010x}"):
                self.assertEqual(f32_bits(f32_literal(rdna2spirv.float_literal(bits))), bits)

    def test_the_whole_module_keeps_the_bits(self):
        # Every TRICKY word as a module constant, through the assembler's binary.
        text = "".join(f"%k{i} = OpConstant %float {rdna2spirv.float_literal(b)}\n" for i, b in enumerate(TRICKY))
        words = spirv_asm.assemble("OpCapability Shader\nOpMemoryModel Logical GLSL450\n"
                                   "%float = OpTypeFloat 32\n" + text)
        words = struct.unpack(f"<{len(words) // 4}I", words)
        consts = [words[i + 3] for i in range(5, len(words)) if words[i] == (4 << 16 | 43)]     # OpConstant
        self.assertEqual(consts, list(TRICKY))


class Fconst(unittest.TestCase):
    def test_one_constant_per_value_and_no_instruction(self):
        g = rdna2spirv.Gen("ps")
        for x in (-0.0, 2.0 ** -149, -(2.0 ** -149), 2.0 ** -126 - 2.0 ** -149, 6.283185307179586,
                  3.4028234663852886e38, -1.0, 0.5, 2.0 ** -64):
            with self.subTest(x=x):
                bits = bits_of(x)
                name = g.fconst(x)
                self.assertEqual(name, f"%fc{bits:x}")
                self.assertIs(g.fconst(x), name)                 # cached
                self.assertEqual(g.tou(name), g.cu(bits))       # its bits are the uint constant, no bitcast
        self.assertEqual(g.fconst(0.0), "%f0")
        self.assertEqual(g.fconst(1.0), "%f1")
        self.assertNotEqual(g.fconst(-0.0), g.fconst(0.0))      # -0 keeps its sign bit
        self.assertEqual(g.body, [])

    def test_translated_module(self):
        # v_sin_f32 reduces in turns and scales by 2*pi (fconst 0.0, 6.2831855, 0.5); v_cvt_norm_i16_f16 clamps to
        # -1.0 and scales by 32767 / 32768.
        lines = ["v_mov_b32 v0, 0.5", "v_sin_f32 v1, v0", "v_mov_b32 v2, 0x3c00bc00",
                 "v_cvt_norm_i16_f16 v3, v2", "v_mov_b32 v4, 0x40400000", "v_sin_f32 v5, v4",
                 "exp mrt0 v1, v3, v5, v0 done vm", "s_endpgm"]
        spv = support.translate_lines("ps_fconst", lines)
        floats = dict(re.findall(r"^\s*(%\S+) = OpConstant %float (\S+)$", spv, re.M))
        for x in (6.283185307179586, 0.5, -1.0, 32767.0, 32768.0):
            bits = bits_of(x)
            with self.subTest(x=x):
                self.assertEqual(spirv_asm._parse_float_bits(floats[f"%fc{bits:x}"], 32, 1), bits)
                self.assertNotRegex(spv, rf"OpBitcast %float %c{bits:x}\b")
        self.assertEqual(len(set(floats.values())), len(floats), "a value is declared twice")
        spirv_asm.assemble(spv)
        out = run_fragment(spv)["%out_mrt0"]
        # sin of half a turn is an exact +0 (trig_exact_zero); -1.0 -> -32767; three turns -> +0.
        self.assertEqual(out[0], 0)
        self.assertEqual(out[1] & 0xFFFF, (-32767) & 0xFFFF)
        self.assertEqual(out[2], 0)
        self.assertEqual(out[3], bits_of(0.5))


if __name__ == "__main__":
    unittest.main()
