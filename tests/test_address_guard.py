"""Unit tests for tools/address_guard.py parity with runtime/src/address_guard.h.

Standard library only; no GPU, no LLVM, no game files.
"""
import contextlib
import io
import os
import struct
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")
if TOOLS not in sys.path:
    sys.path.insert(0, TOOLS)

import address_guard as ag  # noqa: E402

GIB = 1 << 30
LIMIT = ag.RESERVED_END


class AddressGuardOracleTest(unittest.TestCase):
    def test_header_layout_and_last_module(self):
        raw_default = ag.header(LIMIT, 0x100000)
        self.assertEqual(len(raw_default), 64)
        self.assertEqual(struct.unpack("<QQQIIQ", raw_default[:40]), (ag.MAGIC, LIMIT, 0x100000, 0, 0, 0))
        self.assertEqual(raw_default[40:], bytes(24))

        raw_full = ag.header(LIMIT, -0x2000, refused=7, last=0xA0000, last_module=0x12345678)
        self.assertEqual(
            struct.unpack("<QQQIIQ", raw_full[:40]),
            (ag.MAGIC, LIMIT, (-0x2000) & ((1 << 64) - 1), 7, 0x12345678, 0xA0000),
        )

    def test_empty_spans_produce_only_empty_and_full_blocks(self):
        words = ag.table_words([(0x1000, 0x1000), (0, 0), (-0x2000, (-0x2000) & ag.MASK48)])
        self.assertEqual(len(words), ag.L1 + 2 * ag.BLOCK_WORDS)
        self.assertFalse(ag.page_backed(words, 0x1000))
        self.assertFalse(ag.page_backed(words, -0x2000))

    def test_signed_and_unsigned_wraparound_spans_match_cpp(self):
        """Matches runtime/test/address_guard_test.cpp::negative_and_wide."""
        below = (-0x2000) & ag.MASK48
        wrap = (-0x1000) & ag.MASK48
        wide = 8 * GIB + 0x10000

        for spans in (
            [(below, below + 0x1000), (wrap, 0x1000), (wide, wide + 6 * GIB)],
            [(-0x2000, -0x1000), (-0x1000, 0x1000), (wide, wide + 6 * GIB)],
        ):
            with self.subTest(spans=spans):
                words = ag.table_words(spans)
                allocated = (len(words) - ag.L1) // ag.BLOCK_WORDS - 2
                self.assertEqual(allocated, 4)  # chunks 0, 2, 3, and 0xffff
                self.assertTrue(ag.passes(words, LIMIT, below, 4))
                self.assertTrue(ag.passes(words, LIMIT, -0x2000, 4))
                self.assertTrue(ag.passes(words, LIMIT, -4, 8))
                self.assertFalse(ag.passes(words, LIMIT, -0x3000, 4))
                self.assertTrue(ag.passes(words, LIMIT, wide, 4))
                self.assertTrue(ag.passes(words, LIMIT, wide + 4 * GIB, 4))
                self.assertTrue(ag.passes(words, LIMIT, wide + 6 * GIB - 4, 4))
                self.assertFalse(ag.passes(words, LIMIT, wide + 6 * GIB, 4))
                self.assertFalse(ag.passes(words, LIMIT, wide - 4, 8))

    def test_span_ending_at_zero_and_single_byte_below_zero(self):
        words = ag.table_words([((-0x2000) & ag.MASK48, 0)])
        self.assertTrue(ag.page_backed(words, -0x2000))
        self.assertTrue(ag.page_backed(words, -1))
        self.assertFalse(ag.page_backed(words, -0x3000))
        self.assertFalse(ag.page_backed(words, 0))

        one_byte = ag.table_words([(-1, 0)])
        self.assertTrue(ag.page_backed(one_byte, -0x1000))
        self.assertTrue(ag.page_backed(one_byte, -1))
        self.assertFalse(ag.page_backed(one_byte, -0x2000))
        self.assertFalse(ag.page_backed(one_byte, 0))

    def test_word_boundary_bit_marking(self):
        """Exercise partial low word, full interior words, and partial high word inside one chunk."""
        start = 0x1000 * 31       # page 31 (last bit of word 0)
        end = 0x1000 * (64 + 2)   # through page 65 (first 2 bits of word 2), covering all 32 bits of word 1
        words = ag.table_words([(start, end)])
        self.assertFalse(ag.page_backed(words, 0x1000 * 30))
        for page in range(31, 66):
            self.assertTrue(ag.page_backed(words, 0x1000 * page), f"page {page}")
        self.assertFalse(ag.page_backed(words, 0x1000 * 66))

    def test_max_blocks_overflow_gives_full_block(self):
        words = ag.table_words([(4 * GIB, 4 * GIB + 1), (8 * GIB, 8 * GIB + 1)], max_blocks=1)
        self.assertEqual(words[1], 2)
        self.assertEqual(words[2], ag.FULL_BLOCK)
        self.assertTrue(ag.passes(words, LIMIT, 4 * GIB, 4))
        self.assertFalse(ag.passes(words, LIMIT, 4 * GIB + 0x1000, 4))
        self.assertTrue(ag.passes(words, LIMIT, 8 * GIB + 0x5000, 4))

    def test_cli_reports_wrapped_and_unbacked_checks(self):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = ag.main([
                "0xfffffffff000:0x1000",
                "--check", "-4", "8",
                "--check", "0xffffffffe000", "4",
            ])
        self.assertEqual(rc, 0)
        out = buf.getvalue()
        self.assertIn("2 level-2 block(s)", out)
        self.assertIn("-0x4 +8: passes", out)
        self.assertIn("0xffffffffe000 +4: refused", out)

        buf2 = io.StringIO()
        with contextlib.redirect_stdout(buf2):
            rc2 = ag.main(["--check", "-4", "8", "--", "-0x1000:0x1000"])
        self.assertEqual(rc2, 0)
        self.assertIn("-0x4 +8: passes", buf2.getvalue())


if __name__ == "__main__":
    unittest.main()
