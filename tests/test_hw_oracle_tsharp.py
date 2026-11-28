"""Standalone unit tests for tools/hw_oracle/tsharp.py.

Tests GFX10 / GFX11 8-dword T# descriptor packing, bitfield placement,
pitch and layout helpers, kernel_lines generation, boundary values,
validation errors, and negative controls. Pure Python stdlib-only; no GPU needed.
"""
import sys

sys.dont_write_bytecode = True

from pathlib import Path
import unittest

TOOLS = Path(__file__).resolve().parents[1] / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from hw_oracle import tsharp  # noqa: E402


def _decode_tsharp_words(words):
    """Unpack the 8-dword T# descriptor fields produced by tsharp.image_words."""
    w0, w1, w2, w3, w4, w5, w6, w7 = words
    width = ((w1 >> 30) & 0x3) | (((w2 & 0xFFF) << 2))
    height = (w2 >> 14) & 0x3FFF
    return {
        "base_lo": w0,
        "base_hi": w1 & 0xFF,
        "format": (w1 >> 20) & 0xFF,
        "width_m1": width,
        "height_m1": height,
        "resource_level": (w2 >> 31) & 1,
        "dst_sel": tuple((w3 >> (3 * i)) & 0x7 for i in range(4)),
        "type": (w3 >> 28) & 0xF,
        "pitch_m1": w4 & 0x3FFF,
        "perf_mod": (w5 >> 20) & 0x7,
        "w6": w6,
        "w7": w7,
    }


class TestArchAndPitch(unittest.TestCase):
    def test_check_arch_supported_and_unsupported(self):
        self.assertEqual(tsharp.check_arch("gfx1010"), "gfx10")
        self.assertEqual(tsharp.check_arch("gfx1030"), "gfx10")
        self.assertEqual(tsharp.check_arch("gfx1036"), "gfx10")
        self.assertEqual(tsharp.check_arch("gfx1100"), "gfx11")
        self.assertEqual(tsharp.check_arch("gfx1150"), "gfx11")

        for bad in ("gfx1200", "gfx1201", "gfx90a", "invalid"):
            with self.assertRaises(ValueError, msg=bad):
                tsharp.check_arch(bad)

    def test_has_custom_pitch(self):
        self.assertFalse(tsharp.has_custom_pitch("gfx1010"))
        self.assertFalse(tsharp.has_custom_pitch("gfx1013"))
        self.assertTrue(tsharp.has_custom_pitch("gfx1030"))
        self.assertTrue(tsharp.has_custom_pitch("gfx1036"))
        self.assertTrue(tsharp.has_custom_pitch("gfx103a"))
        self.assertTrue(tsharp.has_custom_pitch("gfx1100"))
        self.assertTrue(tsharp.has_custom_pitch("gfx1150"))

        with self.assertRaises(ValueError):
            tsharp.has_custom_pitch("gfx1201")

    def test_pitch_texels_256_byte_alignment(self):
        self.assertEqual(tsharp.pitch_texels(1, 4), 64)
        self.assertEqual(tsharp.pitch_texels(64, 4), 64)
        self.assertEqual(tsharp.pitch_texels(65, 4), 128)
        self.assertEqual(tsharp.pitch_texels(1, 16), 16)
        self.assertEqual(tsharp.pitch_texels(16, 16), 16)
        self.assertEqual(tsharp.pitch_texels(17, 16), 32)
        self.assertEqual(tsharp.pitch_texels(16384, 4), 16384)


class TestImageWords(unittest.TestCase):
    def test_gfx10_vs_gfx11_descriptor_fields(self):
        swz = (tsharp.SEL_W, tsharp.SEL_Z, tsharp.SEL_Y, tsharp.SEL_X)
        w10 = tsharp.image_words("gfx1030", "R8G8B8A8_UNORM", 300, 129, 4, swizzle=swz)
        w11 = tsharp.image_words("gfx1150", "R8G8B8A8_UNORM", 300, 129, 4, swizzle=swz)

        d10 = _decode_tsharp_words(w10)
        d11 = _decode_tsharp_words(w11)

        self.assertEqual(d10["format"], tsharp.IMG_FORMATS["gfx10"]["R8G8B8A8_UNORM"])
        self.assertEqual(d11["format"], tsharp.IMG_FORMATS["gfx11"]["R8G8B8A8_UNORM"])
        self.assertNotEqual(d10["format"], d11["format"])

        self.assertEqual(d10["resource_level"], 1)
        self.assertEqual(d11["resource_level"], 0)

        for d in (d10, d11):
            self.assertEqual(d["base_lo"], 0)
            self.assertEqual(d["base_hi"], 0)
            self.assertEqual(d["width_m1"], 299)
            self.assertEqual(d["height_m1"], 128)
            self.assertEqual(d["dst_sel"], swz)
            self.assertEqual(d["type"], tsharp.SQ_RSRC_IMG_2D)
            self.assertEqual(d["pitch_m1"], tsharp.pitch_texels(300, 4) - 1)
            self.assertEqual(d["perf_mod"], tsharp.PERF_MOD)
            self.assertEqual(d["w6"], 0)
            self.assertEqual(d["w7"], 0)

    def test_gfx1010_has_zero_word4_custom_pitch(self):
        words = tsharp.image_words("gfx1010", "R32_SFLOAT", 17, 5, 4)
        self.assertEqual(words[4], 0)
        d = _decode_tsharp_words(words)
        self.assertEqual(d["resource_level"], 1)
        self.assertEqual(d["width_m1"], 16)
        self.assertEqual(d["height_m1"], 4)

    def test_boundary_1x1_and_16384x16384_pitch_msb(self):
        min_w = tsharp.image_words("gfx1150", "R32_SFLOAT", 1, 1, 4)
        d_min = _decode_tsharp_words(min_w)
        self.assertEqual(d_min["width_m1"], 0)
        self.assertEqual(d_min["height_m1"], 0)
        self.assertEqual(d_min["pitch_m1"], 63)

        max_w = tsharp.image_words("gfx1036", "R32G32B32A32_SFLOAT", 16384, 16384, 16)
        d_max = _decode_tsharp_words(max_w)
        self.assertEqual(d_max["width_m1"], 16383)
        self.assertEqual(d_max["height_m1"], 16383)
        # 16384 - 1 = 0x3FFF: DEPTH [12:0] = 0x1FFF and PITCH_MSB bit 13 = 1
        self.assertEqual(max_w[4], 0x3FFF)
        self.assertEqual((max_w[4] >> 13) & 1, 1)
        self.assertEqual(d_max["pitch_m1"], 16383)

    def test_all_formats_encode_distinct_codes_per_family(self):
        for fam in ("gfx10", "gfx11"):
            arch = "gfx1030" if fam == "gfx10" else "gfx1150"
            for fmt, expected_code in tsharp.IMG_FORMATS[fam].items():
                words = tsharp.image_words(arch, fmt, 4, 4, 4)
                self.assertEqual(_decode_tsharp_words(words)["format"], expected_code, msg=f"{arch} {fmt}")

    def test_validation_errors(self):
        for bad_w, bad_h in ((0, 1), (1, 0), (16385, 1), (1, 16385), (-1, 4)):
            with self.assertRaises(ValueError, msg=f"{bad_w}x{bad_h}"):
                tsharp.image_words("gfx1150", "R8G8B8A8_UNORM", bad_w, bad_h, 4)

        with self.assertRaises(KeyError):
            tsharp.image_words("gfx1150", "NO_SUCH_FORMAT", 4, 4, 4)

        with self.assertRaises(ValueError):
            tsharp.image_words("gfx1201", "R8G8B8A8_UNORM", 4, 4, 4)


class TestLayoutLanesAndKernelLines(unittest.TestCase):
    def test_lay_out_pads_rows_with_0xcd_to_pitch(self):
        texels = b"\x01\x02\x03\x04\x05\x06\x07\x08"  # 2x1 @ 4 bpt -> 1 row of 8 bytes
        out1 = tsharp.lay_out(texels, 2, 1, 4)
        self.assertEqual(len(out1), 256)
        self.assertEqual(out1[:8], texels)
        self.assertEqual(out1[8:], b"\xcd" * 248)

        # 2x2 @ 4 bpt -> 2 rows, each 8 bytes texel + 248 bytes padding
        texels2 = b"\x11" * 8 + b"\x22" * 8
        out2 = tsharp.lay_out(texels2, 2, 2, 4)
        self.assertEqual(len(out2), 512)
        self.assertEqual(out2[:8], b"\x11" * 8)
        self.assertEqual(out2[8:256], b"\xcd" * 248)
        self.assertEqual(out2[256:264], b"\x22" * 8)
        self.assertEqual(out2[264:], b"\xcd" * 248)

        # Exact 256-byte row has zero padding
        exact = b"\xaa" * 256
        self.assertEqual(tsharp.lay_out(exact, 64, 1, 4), exact)

    def test_lanes(self):
        self.assertEqual(tsharp.lanes(1, 1), (1, 1))
        self.assertEqual(tsharp.lanes(16, 8), (128, 1))
        self.assertEqual(tsharp.lanes(32, 32), (1024, 1))
        self.assertEqual(tsharp.lanes(64, 32), (1024, 2))

    def test_longs_padding_and_line_wrapping(self):
        self.assertEqual(tsharp._longs(b""), [])
        self.assertEqual(tsharp._longs(b"\x01\x02\x03"), ["  .long 0x00030201"])
        nine_words = b"".join(i.to_bytes(4, "little") for i in range(9))
        lines = tsharp._longs(nine_words)
        self.assertEqual(len(lines), 2)
        self.assertEqual(lines[0].count("0x"), 8)
        self.assertEqual(lines[1], "  .long 0x00000008")

    def test_kernel_lines_structure_dmask_and_power_of_two(self):
        tex = tsharp.lay_out(b"\x01\x02\x03\x04" * 4, 2, 2, 4)
        lines = tsharp.kernel_lines("gfx1150", 7, "R8G8B8A8_UNORM", 2, 2, 4, 0xF, tex)
        joined = "\n".join(lines)

        self.assertIn(".Lpc7:", lines)
        self.assertIn("s_add_u32 s4, s4, .Ltex7-.Lpc7", lines)
        self.assertIn("image_load v[8:11], v[2:3], s[8:15] dmask:0xf dim:SQ_RSRC_IMG_2D unorm", lines)
        self.assertIn("s_endpgm", lines)
        self.assertIn(".p2align 8", lines)
        self.assertIn(".Ltex7:", lines)
        self.assertIn(f"0x{tsharp.POISON:08x}", joined)
        self.assertIn(f"0x{tsharp.SENTINEL_BITS[0]:08x}", joined)

        # Single-channel dmask uses scalar v8 instead of range v[8:8]
        lines_1ch = tsharp.kernel_lines("gfx1030", 3, "R32_SFLOAT", 2, 2, 4, 0x1, tex)
        self.assertIn("image_load v8, v[2:3], s[8:15] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm", lines_1ch)

        # Multiple iterations when width * height > 1024
        big_tex = b"\x00" * (64 * 32 * 4)
        lines_2it = tsharp.kernel_lines("gfx1150", 9, "R8G8B8A8_UNORM", 64, 32, 4, 0xF, big_tex)
        self.assertIn(".Lok9_0:", lines_2it)
        self.assertIn(".Lok9_1:", lines_2it)

        # Non-power-of-two width must fail
        with self.assertRaisesRegex(ValueError, "power of two"):
            tsharp.kernel_lines("gfx1150", 1, "R8G8B8A8_UNORM", 3, 2, 4, 0xF, b"\x00" * 512)


class TestNegativeControls(unittest.TestCase):
    def test_corrupted_bitfields_are_detected(self):
        words = tsharp.image_words("gfx1030", "R8G8B8A8_UNORM", 64, 32, 4)
        # Flipping RESOURCE_LEVEL bit 31 in word2 must be caught
        corrupted_w2 = list(words)
        corrupted_w2[2] ^= 1 << 31
        self.assertNotEqual(_decode_tsharp_words(corrupted_w2)["resource_level"], 1)

        # Corrupting PITCH_MSB bit 13 in word4 on 16384-wide image must be caught
        max_words = tsharp.image_words("gfx1150", "R8G8B8A8_UNORM", 16384, 1, 4)
        corrupted_w4 = list(max_words)
        corrupted_w4[4] &= ~(1 << 13)
        self.assertNotEqual(_decode_tsharp_words(corrupted_w4)["pitch_m1"], 16383)

        # Corrupting PERF_MOD in word5 must be caught
        corrupted_w5 = list(words)
        corrupted_w5[5] = 0
        self.assertNotEqual(_decode_tsharp_words(corrupted_w5)["perf_mod"], tsharp.PERF_MOD)


if __name__ == "__main__":
    unittest.main()
