"""The run-time FORMAT decode of buffer_load_format (Gen.fmt_dec_component) is table-driven and branch-free.

Every code goes through the translator and the mini interpreter, which runs the shared decode function, and is
compared with a reference decoded here from the raw bits with exact quotients. A code outside FORMATS gives zeros.

The decode has no branch: the format's kind, channel count and channel widths come from a table, and the kind picks
the conversion with selects. The module carries one copy of the decode, one function whose last parameter is the output
component, instead of one copy per format: before, the shared function held every format's decode and a binary search
over the codes.
"""
import struct
import unittest
from fractions import Fraction

import support
from spirv_mini_interp import half_to_float, run_fragment

import rdna2spirv

BASE = 0x1000
WORDS = (0x8123C4F7, 0x3E9A7F01, 0xC0490FDB, 0x5AA5FF80)


def f32_bits(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


def reference(code):
    if code not in rdna2spirv.Gen.FORMATS:
        return (0, 0, 0, 0)
    size, kind, widths = rdna2spirv.Gen.FORMATS[code]
    raw, bit, out = sum(w << (32 * i) for i, w in enumerate(WORDS)), 0, []
    for w in widths:
        v = raw >> bit & ((1 << w) - 1)
        bit += w
        signed = v - (1 << w) if v >> (w - 1) else v
        if kind in ("f32", "u32", "i32", "uint"):
            out.append(v)
        elif kind == "sint":
            out.append(signed & 0xFFFFFFFF)
        elif kind == "f16":
            # A NaN half widens as sign | 0x7F800000 | payload << 13, quiet bit as stored: measured on gfx1036 for
            # formats 13, 29 and 71 (#4623, tools/f16_widen_probe.py). Not struct's '<e', whose NaN payload
            # depends on the Python version (CPython gh-130317).
            if v & 0x7FFF > 0x7C00:
                out.append((v & 0x8000) << 16 | 0x7F800000 | (v & 0x3FF) << 13)
            else:
                out.append(f32_bits(half_to_float(v)))
        elif kind == "unorm":
            out.append(f32_bits(float(Fraction(v, (1 << w) - 1))))
        elif kind == "snorm":
            out.append(f32_bits(float(max(Fraction(-1), Fraction(signed, (1 << (w - 1)) - 1)))))
        else:
            return None                 # a kind this reference doesn't model: covered by its own tests
    return tuple(out[c % len(out)] for c in range(4))


def load(code):
    lines = [f"s_mov_b32 s0, {BASE:#x}", "s_mov_b32 s1, 0", "s_mov_b32 s2, 0x100",
             f"s_mov_b32 s3, {code << 12 | 0x30000FAC:#x}", "v_mov_b32 v1, 0",
             "buffer_load_format_xyzw v[2:5], v1, s[0:3], 0 offen", "exp mrt0 v2, v3, v4, v5 done vm", "s_endpgm"]
    spv = support.translate_lines("ps_fmt_table", lines)
    return tuple(run_fragment(spv, memory={BASE + 4 * i: w for i, w in enumerate(WORDS)})["%out_mrt0"])


def module_of(code=1):
    lines = [f"s_mov_b32 s0, {BASE:#x}", "s_mov_b32 s1, 0", "s_mov_b32 s2, 0x100",
             f"s_mov_b32 s3, {code << 12 | 0x30000FAC:#x}", "v_mov_b32 v1, 0",
             "buffer_load_format_xyzw v[2:5], v1, s[0:3], 0 offen", "exp mrt0 v2, v3, v4, v5 done vm", "s_endpgm"]
    return support.translate_lines("ps_fmt_table", lines)


def function_body(spv, name):
    """The lines of the function `name`, without its header and its end."""
    body, inside = [], False
    for line in spv.splitlines():
        text = line.strip()
        if text.startswith(f"{name} = OpFunction"):
            inside = True
            continue
        if inside:
            if text == "OpFunctionEnd":
                break
            body.append(text)
    return body


BRANCHES = ("OpBranchConditional", "OpSelectionMerge", "OpSwitch", "OpBranch", "OpLoopMerge")


class FormatDecodeTableTests(unittest.TestCase):
    def test_every_format_and_the_codes_between(self):
        for code in range(max(rdna2spirv.Gen.FORMATS) + 2):
            want = reference(code)
            if want is not None:
                self.assertEqual(load(code), want, f"format {code}")

    def test_the_decode_has_no_branch(self):
        spv = module_of()
        body = function_body(spv, "%fmt_dec")
        self.assertTrue(body, "the decode is missing")
        for line in body:
            for op in BRANCHES:
                self.assertNotIn(op, line, f"the decode branches: {line}")

    def test_the_decode_is_shared(self):
        spv = module_of()
        self.assertIn("%fmt_dec = OpFunction %uint None %fn_fmt_dec", spv)
        self.assertIn("%fn_fmt_dec = OpTypeFunction %uint %uint %v4uint_fmt %uint", spv)
        self.assertNotIn("%fmt_dec_c", spv)
        # The xyzw load calls the one function once per register; the decode itself is not inlined there.
        main = function_body(spv, "%main")
        calls = [ln for ln in main if "OpFunctionCall %uint %fmt_dec " in ln]
        self.assertEqual(len(calls), 4)

    def test_the_decode_is_small(self):
        spv = module_of()
        count = len(function_body(spv, "%fmt_dec"))
        # Before, the shared function held every format's decode and a binary search over the codes: 3,498
        # instructions and 306 branches in the module of a shader with one dynamic-format load. One function per
        # component (565 instructions for the four) became one with the component as a parameter.
        self.assertLess(count, 250, f"the decode is {count} instructions")


if __name__ == "__main__":
    unittest.main()
