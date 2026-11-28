"""analyze_4240_full: a rejected 42 40 00 00 block is counted and stepped over, not parsed for ever.

    python -I tests/test_analyze_4240_full.py

Everything is synthetic: an RPF9-like header, zlib streams and hand-built block headers. No game data is used.
"""
import contextlib
import io
import json
import os
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOL = os.path.join(ROOT, "tools", "analyze_4240_full.py")
sys.path.insert(0, os.path.join(ROOT, "tools"))

import analyze_4240_full as A  # noqa: E402

BLOCK_6330 = struct.pack("<I", 0x3063) + bytes(0x60 - 4)


def block_4240(words):
    """A 0x60-byte 42 40 00 00 block: word 0 is the magic, words 1..22 are slot words."""
    header = bytearray(0x60)
    struct.pack_into("<24I", header, 0, *words)
    return bytes(header)


def make_archive(path, *bodies):
    """RPF9-like header (stream start word at 4) followed by one zlib stream per body."""
    header = bytearray(0x780)
    header[0:4] = b"9FPR"
    struct.pack_into("<I", header, 4, 0x780)
    with open(path, "wb") as f:
        f.write(bytes(header) + b"".join(zlib.compress(b, 9) for b in bodies) + bytes(64))


class Analyze4240Test(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = self.tmp.name

    def tearDown(self):
        self.tmp.cleanup()

    def run_tool(self, *args, timeout=30):
        return subprocess.run([sys.executable, "-I", TOOL, *args], cwd=ROOT,
                              capture_output=True, text=True, errors="replace", timeout=timeout)

    def truncated_archive(self):
        path = os.path.join(self.root, "truncated.rpf")
        # A 63 30 block the walker steps over, then the 42 40 magic with only 8 bytes behind it.
        make_archive(path, BLOCK_6330 + struct.pack("<I", 0x4042) + bytes(8))
        return path

    def test_a_truncated_block_returns_instead_of_looping(self):
        # Before: the except branch did not advance pos, so the same word was parsed and rejected for ever
        # (measured: still running after 12 s under timeout).
        path = self.truncated_archive()
        p = self.run_tool(path, timeout=30)
        self.assertEqual(p.returncode, 0, p.stderr)
        stats = A.process_archive(path)
        self.assertEqual(stats["blocks_4240"], 0)
        self.assertEqual(stats["errors"], 1, "a rejected block must be counted")
        self.assertIn("first_error", stats)

    def test_a_valid_block_is_measured(self):
        path = os.path.join(self.root, "valid.rpf")
        slots = [0x4042, (0x30 << 8) | 0x01, (0x40 << 8) | 0x01] + [0] * 21
        make_archive(path, BLOCK_6330 + block_4240(slots))
        stats = A.process_archive(path)
        self.assertEqual(stats["errors"], 0)
        self.assertEqual(stats["blocks_6330"], 1)
        self.assertEqual(stats["blocks_4240"], 1)
        self.assertEqual(stats["slot_tag_counts"], {(1, 1): 1, (0, 0x42): 1, (2, 1): 1})
        # Slot 1 sits at 0x60 and slot 0 at 0x80; slot 2 shares 0x80, so it adds no stride.
        self.assertEqual(stats["strides"], {(1, 1, 0, 0x42, 0x20): 1})

    def test_stream_starting_with_4240_block_is_measured(self) -> None:
        path = os.path.join(self.root, "direct-4240.rpf")
        # Four 8-byte positions from 0x80, then four 4-byte colors from 0xA0.
        slots = [0x4042, (0x50 << 8) | 0x26] + [0] * 22
        page = block_4240(slots) + bytes(0xA0 - 0x60) + bytes((10, 20, 30, 255)) * 4
        make_archive(path, page)

        stats = A.process_archive(path)

        self.assertEqual((stats["streams"], stats["blocks_4240"], stats["blocks_6330"]), (1, 1, 0))
        self.assertEqual(stats["slot_tag_counts"], {(0, 0x42): 1, (1, 0x26): 1})
        self.assertEqual(stats["strides"], {(0, 0x42, 1, 0x26, 32): 1})
        self.assertEqual(stats["errors"], 0)

    def test_missing_input_is_one_error_line(self):
        missing = os.path.join(self.root, "missing.rpf")
        p = self.run_tool(missing)
        self.assertEqual(p.returncode, 2, p.stderr)
        self.assertIn("does not exist", p.stderr)
        self.assertIn("missing.rpf", p.stderr)
        self.assertNotIn("Traceback", p.stderr)
        self.assertNotIn("Processing", p.stdout)

    def test_a_folder_input_is_one_line_and_the_run_continues(self):
        # Before: exists() let a folder through, open() raised PermissionError, and the archives after it were lost.
        folder = os.path.join(self.root, "carve")
        os.makedirs(folder)
        good = self.truncated_archive()
        p = self.run_tool(folder, good)
        self.assertEqual(p.returncode, 1, p.stderr)
        self.assertIn("is not a file", p.stderr)
        self.assertIn("carve", p.stderr)
        self.assertNotIn("Traceback", p.stderr)
        self.assertIn("Processing " + good, p.stdout, "the next archive is still read")

    def test_unreadable_archive_is_reported_and_later_archives_are_measured(self):
        blocked = os.path.join(self.root, "locked.rpf")
        make_archive(blocked, BLOCK_6330)
        good = self.truncated_archive()
        out = os.path.join(self.root, "stats.json")
        stderr = io.StringIO()

        def open_archive(path, *args, **kwargs):
            if path == blocked:
                raise PermissionError(13, "input is locked", path)
            return open(path, *args, **kwargs)

        with (mock.patch.object(A, "open", create=True, side_effect=open_archive),
              mock.patch.object(sys, "argv", [TOOL, blocked, good, "--json", out]),
              contextlib.redirect_stdout(io.StringIO()),
              contextlib.redirect_stderr(stderr)):
            self.assertEqual(A.main(), 1)
        self.assertEqual(stderr.getvalue().splitlines(),
                         [f"ERROR: cannot read {blocked}: input is locked"])
        with open(out, encoding="utf-8") as fh:
            doc = json.load(fh)
        self.assertEqual(len(doc), 1)
        self.assertEqual(doc[0]["errors"], 1)

    def test_limit_below_one_is_refused(self):
        path = self.truncated_archive()
        for bad in ("0", "-1"):
            with self.subTest(limit=bad):
                p = self.run_tool(path, "--limit", bad)
                self.assertEqual(p.returncode, 2, p.stderr)
                self.assertIn("at least 1", p.stderr)
                self.assertNotIn("Traceback", p.stderr)

    def test_json_in_a_missing_folder_is_created(self):
        path = self.truncated_archive()
        out = os.path.join(self.root, "no", "such", "stats.json")
        p = self.run_tool(path, "--json", out)
        self.assertEqual(p.returncode, 0, p.stderr)
        with open(out, encoding="utf-8") as f:
            doc = json.load(f)
        self.assertEqual(len(doc), 1)
        self.assertEqual(doc[0]["errors"], 1)

    def test_json_naming_an_input_archive_is_refused_and_the_archive_kept(self):
        # Before: the archive was read, then replaced with the JSON report.
        path = self.truncated_archive()
        with open(path, "rb") as f:
            before = f.read()
        for spelling in (path, os.path.join(self.root, ".", os.path.basename(path))):
            with self.subTest(json=spelling):
                p = self.run_tool(path, "--json", spelling)
                self.assertEqual(p.returncode, 2, p.stderr)
                self.assertIn("would overwrite the input archive", p.stderr)
                self.assertNotIn("Traceback", p.stderr)
                self.assertNotIn("Processing", p.stdout, "refused before the scan")
                with open(path, "rb") as f:
                    self.assertEqual(f.read(), before)

    def test_json_naming_a_later_input_is_refused(self):
        first, second = self.truncated_archive(), os.path.join(self.root, "second.rpf")
        make_archive(second, BLOCK_6330)
        p = self.run_tool(first, second, "--json", second)
        self.assertEqual(p.returncode, 2, p.stderr)
        self.assertIn("second.rpf", p.stderr)
        self.assertTrue(open(second, "rb").read().startswith(b"9FPR"))

    def test_json_through_a_link_to_an_input_is_refused(self):
        path = self.truncated_archive()
        link = os.path.join(self.root, "link.json")
        try:
            os.link(path, link)              # a hard link: another name for the archive itself
        except OSError as e:
            self.skipTest(f"hard links unavailable: {e}")
        p = self.run_tool(path, "--json", link)
        self.assertEqual(p.returncode, 2, p.stderr)
        self.assertIn("would overwrite the input archive", p.stderr)
        self.assertTrue(open(path, "rb").read().startswith(b"9FPR"))

    def test_json_folder_is_refused_before_the_scan(self):
        path = self.truncated_archive()
        p = self.run_tool(path, "--json", self.root)
        self.assertEqual(p.returncode, 2, p.stderr)
        self.assertIn("is a folder", p.stderr)
        self.assertNotIn("Traceback", p.stderr)
        self.assertNotIn("Processing", p.stdout)

    def test_a_failed_json_write_is_one_error_line(self):
        path = self.truncated_archive()
        blocker = os.path.join(self.root, "plain-file")
        with open(blocker, "w", encoding="utf-8") as f:
            f.write("x")
        out = os.path.join(blocker, "stats.json")     # its parent is a file, so the folder can't be made
        p = self.run_tool(path, "--json", out)
        self.assertEqual(p.returncode, 2, p.stderr)
        self.assertIn("cannot write", p.stderr)
        self.assertNotIn("Traceback", p.stderr)

    def bad_offset_archive(self):
        path = os.path.join(self.root, "bad-offset.rpf")
        header = bytearray(0x780)
        header[0:4] = b"9FPR"
        struct.pack_into("<I", header, 4, 0x100000)   # data start past EOF
        with open(path, "wb") as f:
            f.write(bytes(header) + zlib.compress(BLOCK_6330, 9) + bytes(64))
        return path

    def test_offset_past_eof_is_rejected(self):
        with self.assertRaises(ValueError) as cm:
            A.process_archive(self.bad_offset_archive())
        self.assertIn("past the end", str(cm.exception))

    def test_bad_offset_is_one_error_line_and_left_out_of_the_report(self):
        bad, good = self.bad_offset_archive(), self.truncated_archive()
        report = os.path.join(self.root, "stats.json")
        p = self.run_tool(bad, good, "--json", report)
        self.assertEqual(p.returncode, 1, p.stderr)
        self.assertIn("ERROR: ", p.stderr)
        self.assertIn("bad-offset.rpf", p.stderr)
        self.assertIn("past the end", p.stderr)
        self.assertNotIn("Traceback", p.stderr)
        self.assertNotIn("Streams: 0", p.stdout)
        self.assertIn("Processing " + good, p.stdout, "the next archive is still read")
        with open(report, encoding="utf-8") as f:
            self.assertEqual(len(json.load(f)), 1)

    def test_limit_reports_exactly_that_many_streams(self):
        path = os.path.join(self.root, "five.rpf")
        make_archive(path, *([BLOCK_6330] * 5))
        for n in (1, 3, 5):
            with self.subTest(limit=n):
                stats = A.process_archive(path, limit=n)
                self.assertEqual(stats["streams"], n)
                self.assertEqual(stats["blocks_6330"], n)


    def test_trailing_high_bit_word_does_not_crash_or_fake_group(self):
        path = os.path.join(self.root, "trailing-hi.rpf")
        slots = [0x4042, (0x30 << 8) | 0x01] + [0] * 22
        # Valid 4240 header (0x60 bytes) + 8 trailing bytes starting with a high-bit word (e.g. -0.0f).
        page = block_4240(slots) + struct.pack("<II", 0x80000000, 0x12345678)
        make_archive(path, page)
        stats = A.process_archive(path)
        self.assertEqual(stats["streams"], 1)
        self.assertEqual(stats["blocks_4240"], 1)
        self.assertEqual(stats["groups"], 0)
        self.assertEqual(stats["errors"], 0)

        # Also verify a high-bit vertex payload word whose counts exceed the stream is not counted as a group.
        path2 = os.path.join(self.root, "false-group.rpf")
        page2 = block_4240(slots) + struct.pack("<4I", 0x80001000, 0x00100000, 0x00100000, 0)
        make_archive(path2, page2)
        stats2 = A.process_archive(path2)
        self.assertEqual(stats2["blocks_4240"], 1)
        self.assertEqual(stats2["groups"], 0)

    def test_oversized_zlib_stream_is_skipped_and_process_archive_rejects_bad_limit(self):
        path = os.path.join(self.root, "bomb-and-good.rpf")
        huge = BLOCK_6330 + bytes(8192)
        make_archive(path, huge, BLOCK_6330)
        stats = A.process_archive(path, max_inflate=4096)
        self.assertEqual(stats["streams"], 1)
        self.assertEqual(stats["blocks_6330"], 1)

        for bad in (0, -1, True, False):
            with self.subTest(limit=bad), self.assertRaisesRegex(ValueError, "at least 1"):
                A.process_archive(path, limit=bad)

    def test_write_json_is_atomic_trailing_lf_and_preserves_mode(self):
        out = os.path.join(self.root, "report.json")
        with open(out, "w", encoding="utf-8") as f:
            f.write("previous\n")
        if os.name != "nt":
            os.chmod(out, 0o640)
        with mock.patch.object(A, "replace_retry", side_effect=OSError("busy")):
            with self.assertRaises(OSError):
                A.write_json(out, [{"streams": 1}])
        with open(out, encoding="utf-8") as f:
            self.assertEqual(f.read(), "previous\n")
        self.assertEqual([n for n in os.listdir(self.root) if n.startswith(".analyze_4240_full_")], [])

        A.write_json(out, [{"streams": 1}])
        with open(out, "rb") as f:
            raw = f.read()
        self.assertTrue(raw.endswith(b"\n"))
        self.assertEqual(json.loads(raw.decode("utf-8")), [{"streams": 1}])
        if os.name != "nt":
            self.assertEqual(stat.S_IMODE(os.stat(out).st_mode), 0o640)


if __name__ == "__main__":
    unittest.main(verbosity=2)
