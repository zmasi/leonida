"""CPU test for tools/psocache_extract.py on synthetic RPF9 archives built by this test.

The cache inside is a tiny file that satisfies the layout `psocache_inspect` checks: a 0x80 header, one 0x70
record, one 0x18 record, two 8-byte index entries, a 32-byte meta entry and a 0x100-byte code entry. The cache
is split across two zlib streams in the archive, with zero padding, a stream before it and a stream after it.
"""

import os
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOL = os.path.join(ROOT, "tools", "psocache_extract.py")
sys.path.insert(0, os.path.join(ROOT, "tools"))
import psocache_extract as pe  # noqa: E402

HEADER = 0x80
GRAPHICS = 1
COMPUTE = 1
META_INDEX = 1
SHADER_INDEX = 1
META_SIZE = 32
CODE_SIZE = 0x100
CACHE_SIZE = (
    HEADER
    + GRAPHICS * 0x70
    + COMPUTE * 0x18
    + META_INDEX * 8
    + SHADER_INDEX * 8
    + META_SIZE
    + CODE_SIZE
)
SPLIT = 200
END_PGM = b"\x00\x00\x81\xbf"
CODE_END = b"\x00\x00\x9f\xbf"


def build_cache():
    data = bytearray(CACHE_SIZE)
    data[0:4] = b"PS\x00\x09"
    struct.pack_into("<4I", data, 0x50, GRAPHICS, COMPUTE, META_INDEX, SHADER_INDEX)
    struct.pack_into("<2I", data, 0x60, META_SIZE, CODE_SIZE)
    off = HEADER
    struct.pack_into("<Q", data, off, 0x1000)
    struct.pack_into("<2I", data, off + 8, 0, META_INDEX - 1)
    data[off + 0x60 : off + 0x70] = b"\xff" * 16
    off += 0x70
    struct.pack_into("<Q", data, off, 0x2000)
    struct.pack_into("<4I", data, off + 8, 0, 0, 0, 0)
    off += 0x18
    struct.pack_into("<2I", data, off, 0, META_SIZE)  # meta index
    struct.pack_into("<2I", data, off + 8, 0, CODE_SIZE)  # shader index
    meta_off = off + 16
    struct.pack_into("<8I", data, meta_off, 0x43, 1, 2, 3, 4, 0, 0, 0)
    code_off = meta_off + META_SIZE
    data[code_off + 0x20 : code_off + 0x24] = END_PGM
    for pos in range(code_off + 0x24, code_off + CODE_SIZE, 4):
        data[pos : pos + 4] = CODE_END
    return bytes(data)


def build_archive(cache, drop_tail=False, tail=True):
    body = bytearray()
    body += zlib.compress(b"noise" * 100)
    body += b"\x00"
    body += zlib.compress(cache[:SPLIT])
    body += b"\x00" * 5
    if not drop_tail:
        body += zlib.compress(cache[SPLIT:])
        if tail:
            body += zlib.compress(b"tail" * 100)
    data = bytearray(0x40)
    data[0:4] = b"9FPR"
    struct.pack_into("<I", data, 4, 0x40)
    data += body
    return bytes(data)


def run(*args, env=None):
    return subprocess.run(
        [sys.executable, "-I", TOOL, *args], capture_output=True, text=True, env=env
    )


class PsocacheExtractTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.cache = build_cache()
        self.archive = os.path.join(self.tmp.name, "input.rpf")
        with open(self.archive, "wb") as f:
            f.write(build_archive(self.cache))
        self.out = os.path.join(self.tmp.name, "cache.bin")

    def tearDown(self):
        self.tmp.cleanup()

    def test_extracts_the_contiguous_stream(self):
        p = run(self.archive, self.out)
        self.assertEqual(p.returncode, 0, p.stderr)
        with open(self.out, "rb") as f:
            self.assertEqual(f.read(), self.cache)
        self.assertIn("archive #1", p.stdout)
        self.assertIn("match: archive stream offset 0x", p.stdout)
        self.assertIn("declared %d bytes" % CACHE_SIZE, p.stdout)
        self.assertIn("verdict: model holds", p.stdout)
        self.assertNotIn("Traceback", p.stderr)

    def test_folder_finds_the_last_archive(self):
        folder = os.path.join(self.tmp.name, "archives")
        os.mkdir(folder)
        with open(os.path.join(folder, "01.bin"), "wb") as f:
            f.write(b"not an archive")
        empty = bytearray(0x40)
        empty[0:4] = b"9FPR"
        struct.pack_into("<I", empty, 4, 0x40)
        with open(os.path.join(folder, "02.rpf"), "wb") as f:
            f.write(empty)
        with open(os.path.join(folder, "03.rpf"), "wb") as f:
            f.write(build_archive(self.cache))
        p = run(folder, self.out)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("archive #3", p.stdout)
        self.assertNotIn("01.bin", p.stdout)

    def test_budget_stops_the_search(self):
        p = run(self.archive, self.out, "--budget", "4")
        self.assertEqual(p.returncode, 2)
        self.assertIn("no cache found", p.stderr)
        self.assertFalse(os.path.exists(self.out))

    def test_stream_limit_stops_the_search(self):
        p = run(self.archive, self.out, "--max-streams", "1")
        self.assertEqual(p.returncode, 2)
        self.assertIn("no cache found", p.stderr)

    def test_truncated_chain_is_reported(self):
        with open(self.archive, "wb") as f:
            f.write(build_archive(self.cache, drop_tail=True))
        p = run(self.archive, self.out)
        self.assertEqual(p.returncode, 2)
        self.assertIn("of %d declared bytes" % CACHE_SIZE, p.stderr)
        self.assertNotIn("Traceback", p.stderr)

    def test_output_outside_the_temp_directory_is_refused(self):
        outside = os.path.join(ROOT, "psocache_should_not_exist.bin")
        self.addCleanup(lambda: os.path.exists(outside) and os.unlink(outside))
        # Pin the tool's temporary directory to a fresh one: the check compares against the system temporary
        # directory, and a checkout that lives under /tmp (or %TEMP%) would make ROOT itself look "inside" it.
        tool_tmp = os.path.join(self.tmp.name, "tool-tmp")
        os.mkdir(tool_tmp)
        p = run(self.archive, outside, env=dict(os.environ, TMPDIR=tool_tmp))
        self.assertEqual(p.returncode, 2)
        self.assertIn("temporary directory", p.stderr)
        self.assertFalse(os.path.exists(outside))

    def test_missing_input_exits_two(self):
        p = run(os.path.join(self.tmp.name, "absent.rpf"), self.out)
        self.assertEqual(p.returncode, 2)
        self.assertNotIn("Traceback", p.stderr)

    def test_max_streams_stops_before_opening_later_archives(self):
        folder = os.path.join(self.tmp.name, "archives")
        os.mkdir(folder)
        with open(os.path.join(folder, "01.rpf"), "wb") as f:
            f.write(build_archive(self.cache))
        with open(os.path.join(folder, "02.rpf"), "wb") as f:
            f.write(build_archive(self.cache))
        p = run(folder, self.out, "--max-streams", "1")
        self.assertEqual(p.returncode, 2)
        self.assertIn("no cache found", p.stderr)
        self.assertNotIn("archive #2", p.stdout)

    def test_output_cannot_overwrite_source_or_be_dir_or_whitespace(self):
        with open(self.archive, "rb") as f:
            original = f.read()
        p = run(self.archive, self.archive)
        self.assertEqual(p.returncode, 2)
        self.assertIn("must not overwrite", p.stderr)
        with open(self.archive, "rb") as f:
            self.assertEqual(f.read(), original)

        folder = os.path.join(self.tmp.name, "dir_src")
        os.mkdir(folder)
        arc2 = os.path.join(folder, "01.rpf")
        with open(arc2, "wb") as f:
            f.write(original)
        p2 = run(folder, arc2)
        self.assertEqual(p2.returncode, 2)
        self.assertIn("must not overwrite", p2.stderr)
        with open(arc2, "rb") as f:
            self.assertEqual(f.read(), original)

        existing_dir = os.path.join(self.tmp.name, "out_dir")
        os.mkdir(existing_dir)
        p3 = run(self.archive, existing_dir)
        self.assertEqual(p3.returncode, 2)
        self.assertIn("is a directory", p3.stderr)

        missing_parent = os.path.join(self.tmp.name, "no_parent", "cache.bin")
        p4 = run(self.archive, missing_parent)
        self.assertEqual(p4.returncode, 2)
        self.assertIn("parent directory does not exist", p4.stderr)

        run_cwd = os.path.join(self.tmp.name, "cwd")
        os.mkdir(run_cwd)
        p5 = subprocess.run(
            [sys.executable, "-I", TOOL, self.archive, "   "],
            capture_output=True, text=True, cwd=run_cwd,
        )
        self.assertEqual(p5.returncode, 2)
        self.assertEqual(os.listdir(run_cwd), [])

    def test_inside_tempdir_handles_root_and_empty_paths(self):
        self.assertFalse(pe.inside_tempdir(""))
        self.assertFalse(pe.inside_tempdir("   "))
        with (mock.patch.object(pe.tempfile, "gettempdir", return_value="/"),
              mock.patch.object(pe.os.path, "realpath", side_effect=lambda p: p),
              mock.patch.object(pe.os.path, "abspath", side_effect=lambda p: p)):
            self.assertTrue(pe.inside_tempdir("/cache.bin"))
            self.assertFalse(pe.inside_tempdir("/"))

    def test_exact_max_gap_padding_accepted_and_over_gap_rejected(self):
        def archive_with_gap(gap_bytes):
            body = bytearray()
            body += zlib.compress(self.cache[:SPLIT])
            body += b"\x00" * gap_bytes
            body += zlib.compress(self.cache[SPLIT:])
            data = bytearray(0x40)
            data[0:4] = b"9FPR"
            struct.pack_into("<I", data, 4, 0x40)
            return bytes(data + body)

        with mock.patch.object(pe, "MAX_GAP", 16):
            ok_arc = os.path.join(self.tmp.name, "gap_exact.rpf")
            with open(ok_arc, "wb") as f:
                f.write(archive_with_gap(16))
            idx, report, _ = pe.extract(ok_arc, self.out, quiet=True)
            self.assertEqual(idx, 1)
            self.assertEqual(report["problems"], [])

            bad_arc = os.path.join(self.tmp.name, "gap_over.rpf")
            with open(bad_arc, "wb") as f:
                f.write(archive_with_gap(17))
            with self.assertRaisesRegex(ValueError, "contiguous streams"):
                pe.extract(bad_arc, self.out, quiet=True)

    def test_unreadable_source_folder_privacy_and_atomic_write(self):
        secret_dir = os.path.join(self.tmp.name, "secret_game_dir")
        os.mkdir(secret_dir)
        with mock.patch.object(pe.os, "listdir", side_effect=PermissionError(13, "denied", secret_dir)):
            with self.assertRaises(ValueError) as cm:
                pe.archives(secret_dir)
        self.assertIn("source folder cannot be listed", str(cm.exception))
        self.assertNotIn("secret_game_dir", str(cm.exception))

        out = os.path.join(self.tmp.name, "existing.bin")
        with open(out, "wb") as f:
            f.write(b"KEEP")
        with mock.patch.object(pe, "replace_file", side_effect=OSError("disk full")):
            with self.assertRaises(OSError):
                pe._write_bytes_atomic(out, b"NEW")
        with open(out, "rb") as f:
            self.assertEqual(f.read(), b"KEEP")
        self.assertEqual([n for n in os.listdir(self.tmp.name) if n.startswith(".psocache-")], [])


if __name__ == "__main__":
    unittest.main()
