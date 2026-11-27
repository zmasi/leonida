"""buffer_load_format_* with a run-time FORMAT decodes only the components its registers take.

The decode is one shared function, %fmt_dec(format, the element's four dwords, component). A load calls it once per
register it writes, with the component that register's DST_SEL field picks (4..7 -> X..W): buffer_load_format_x
makes one call, _xy two, _xyzw four. Until 2026-11-27 every load called four per-component functions, whatever it
wrote. The values do not change: DST_SEL 1 is 1.0 (integer 1 for an integer format), 0 and the reserved codes are 0,
an out-of-bounds element decodes to 0, and a register the load does not write keeps its value.

The programs are the hand-written fixtures fixtures/typed_load_runtime_{x,xy,xyzw}.asm: the V# is s[0:3], read from
the push constants, so one translation serves every format and DST_SEL.
"""
import os
import random
import unittest

import support
from spirv_mini_interp import run_fragment
from test_format_decode_table import BASE, WORDS, function_body, reference

import rdna2spirv

HERE = os.path.dirname(os.path.abspath(__file__))
SENTINELS = (0xA5A5A5A5, 0x5A5A5A5A, 0x13579BDF, 0x2468ACE0)
WIDTHS = {"x": 1, "xy": 2, "xyzw": 4}
XYZW = 4 | 5 << 3 | 6 << 6 | 7 << 9
# 8_UNORM, 16_UNORM, 16_SNORM, 16_FLOAT, 32_UINT, 32_SINT, 32_FLOAT, 8_8_UNORM, 16_16_UINT, 16_16_FLOAT,
# 8_8_8_8_UNORM, 8_8_8_8_SINT, 32_32_FLOAT, 32_32_32_FLOAT, 32_32_32_32_UINT and a code outside FORMATS
CODES = (1, 7, 8, 13, 20, 21, 22, 14, 27, 29, 56, 61, 64, 74, 75, 127)
_SPV = {}


def spv_of(width):
    if width not in _SPV:
        with open(os.path.join(HERE, "fixtures", f"typed_load_runtime_{width}.asm"), encoding="utf-8") as f:
            lines = [ln.strip() for ln in f if ln.strip() and not ln.startswith(";")]
        _SPV[width] = support.translate_lines(f"ps_typed_load_runtime_{width}", lines)
    return _SPV[width]


def run(width, code, dst_sel, num_records=0x100):
    w3 = 0x30000000 | code << 12 | dst_sel
    memory = {BASE + 4 * i: w for i, w in enumerate(WORDS)}
    return run_fragment(spv_of(width), push_constants=[BASE, 0, num_records, w3] + [0] * 28,
                        memory=memory)["%out_mrt0"]


def is_integer(code):
    """DST_SEL 1 reads integer 1 for the integer formats and the codes outside FORMATS, 1.0 for the others."""
    return code not in rdna2spirv.Gen.FORMATS or rdna2spirv.Gen.FORMATS[code][1] in ("u32", "i32", "uint", "sint")


def expected(width, code, sels, in_bounds=True):
    ref = reference(code) if in_bounds else (0, 0, 0, 0)
    out = list(SENTINELS)
    for c in range(WIDTHS[width]):
        s = sels[c]
        out[c] = ref[s - 4] if s >= 4 else (1 if is_integer(code) else 0x3F800000) if s == 1 else 0
    return tuple(out)


def pack(sels):
    return sum(s << (3 * i) for i, s in enumerate(sels))


class DecodeCalls(unittest.TestCase):
    def test_one_call_per_written_register(self):
        for width, n in WIDTHS.items():
            with self.subTest(width=width):
                spv = spv_of(width)
                calls = [ln for ln in function_body(spv, "%main") if "OpFunctionCall %uint %fmt_dec " in ln]
                self.assertEqual(len(calls), n)
                # The component is the call's last argument, a run-time id (DST_SEL - 4), not a constant.
                for ln in calls:
                    self.assertRegex(ln, r"OpFunctionCall %uint %fmt_dec %t\d+ %t\d+ %t\d+$")
                self.assertEqual(spv.count("%fmt_dec = OpFunction %uint None %fn_fmt_dec"), 1)
                self.assertNotIn("%fmt_dec_c", spv)

    def test_fewer_registers_make_a_smaller_module(self):
        sizes = [len(spv_of(w).splitlines()) for w in ("x", "xy", "xyzw")]
        self.assertLess(sizes[0], sizes[1])
        self.assertLess(sizes[1], sizes[2])


class DecodeValues(unittest.TestCase):
    def test_formats_with_identity_selectors(self):
        for code in CODES:
            if reference(code) is None:
                continue
            for width in WIDTHS:
                with self.subTest(code=code, width=width):
                    self.assertEqual(run(width, code, XYZW), expected(width, code, (4, 5, 6, 7)))

    def test_formats_with_swizzled_and_constant_selectors(self):
        # Each register takes another component, or 0, 1, or a reserved code (2, 3), from the descriptor.
        for sels in ((7, 6, 5, 4), (5, 1, 0, 6), (6, 3, 2, 1), (1, 7, 7, 0), (0, 4, 1, 5)):
            for code in CODES:
                if reference(code) is None:
                    continue
                for width in WIDTHS:
                    with self.subTest(sels=sels, code=code, width=width):
                        self.assertEqual(run(width, code, pack(sels)), expected(width, code, sels))

    def test_out_of_bounds_reads_zero_and_keeps_the_constant_one(self):
        for code in (1, 20, 22, 56, 75):
            for width in WIDTHS:
                with self.subTest(code=code, width=width):
                    sels = (1, 4, 0, 7)
                    self.assertEqual(run(width, code, pack(sels), num_records=0),
                                     expected(width, code, sels, in_bounds=False))

    def test_narrow_loads_match_the_xyzw_load(self):
        # Every code, random selectors: the registers _x and _xy write are the first of what _xyzw writes.
        rnd = random.Random(1608)
        for code in range(128):
            sels = [rnd.randrange(8) for _ in range(4)]
            full = run("xyzw", code, pack(sels))
            for width, n in (("x", 1), ("xy", 2)):
                with self.subTest(code=code, sels=sels, width=width):
                    self.assertEqual(run(width, code, pack(sels)), full[:n] + SENTINELS[n:])


if __name__ == "__main__":
    unittest.main()
