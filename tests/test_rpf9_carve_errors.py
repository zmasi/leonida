"""Regression tests for clean OSError handling when writing carved streams in tools/rpf9_carve.py.

    python -I tests/test_rpf9_carve_errors.py
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
TOOL = os.path.join(ROOT, "tools", "rpf9_carve.py")
sys.path.insert(0, os.path.join(ROOT, "tools"))
import rpf9_carve  # noqa: E402


class Rpf9CarveWriteErrorTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def _write_archive(self, name, payload, base=16):
        path = os.path.join(self.tmp.name, name)
        with open(path, "wb") as f:
            f.write(b"9FPR" + struct.pack("<I", base) + b"\x00" * (base - 8) + payload)
        return path

    def test_unwritable_bin_stream_exits_without_traceback(self):
        zdata = zlib.compress(b"A" * 128)
        rpf = self._write_archive("bin.rpf", zdata, base=16)
        out_dir = os.path.join(self.tmp.name, "out_bin")
        os.makedirs(os.path.join(out_dir, f"{16:012x}.bin"))
        p = subprocess.run([sys.executable, "-I", TOOL, rpf, out_dir], capture_output=True, text=True)
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("cannot write", p.stderr)
        self.assertIn(f"{16:012x}.bin", p.stderr)
        self.assertNotIn("Traceback", p.stderr)

    def test_unwritable_bk2_stream_exits_without_traceback(self):
        blen = 64
        bk2 = bytearray(blen)
        bk2[0:4] = b"KB2i"
        struct.pack_into("<II", bk2, 4, blen - 8, 10)
        struct.pack_into("<II", bk2, 0x14, 1280, 720)
        rpf = self._write_archive("bk2.rpf", bytes(bk2), base=16)
        out_dir = os.path.join(self.tmp.name, "out_bk2")
        os.makedirs(os.path.join(out_dir, f"{16:012x}.bk2"))
        p = subprocess.run([sys.executable, "-I", TOOL, rpf, out_dir], capture_output=True, text=True)
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("cannot write", p.stderr)
        self.assertIn(f"{16:012x}.bk2", p.stderr)
        self.assertNotIn("Traceback", p.stderr)

    def test_index_json_aliasing_input_archive_is_refused(self):
        out_dir = os.path.join(self.tmp.name, "out_alias")
        os.makedirs(out_dir)
        arch = os.path.join(out_dir, "index.json")
        original = b"9FPR" + struct.pack("<I", 8) + b"\x00" * 16
        with open(arch, "wb") as f:
            f.write(original)

        aliases = [arch]
        if os.name == "nt":
            aliases.append(os.path.join(out_dir, "INDEX.JSON"))
        for alias in aliases:
            with self.subTest(archive=os.path.basename(alias)):
                p = subprocess.run([sys.executable, "-I", TOOL, alias, out_dir, "--index-only"],
                                   capture_output=True, text=True)
                self.assertNotEqual(p.returncode, 0)
                self.assertIn("must not overwrite input archive", p.stderr)
                self.assertNotIn("Traceback", p.stderr)
                with open(arch, "rb") as f:
                    self.assertEqual(f.read(), original)

    def test_bin_output_aliasing_input_archive_is_refused(self):
        zdata = zlib.compress(b"A" * 128)
        out_dir = os.path.join(self.tmp.name, "out_bin_alias")
        os.makedirs(out_dir)
        arch = os.path.join(out_dir, f"{16:012x}.bin")
        with open(arch, "wb") as f:
            f.write(b"9FPR" + struct.pack("<I", 16) + b"\x00" * 8 + zdata)
        with open(arch, "rb") as f:
            original = f.read()
        p = subprocess.run([sys.executable, "-I", TOOL, arch, out_dir], capture_output=True, text=True)
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("must not overwrite input archive", p.stderr)
        self.assertNotIn("Traceback", p.stderr)
        with open(arch, "rb") as f:
            self.assertEqual(f.read(), original)

    def test_bk2_output_aliasing_input_archive_is_refused(self):
        blen = 64
        bk2 = bytearray(blen)
        bk2[0:4] = b"KB2i"
        struct.pack_into("<II", bk2, 4, blen - 8, 10)
        struct.pack_into("<II", bk2, 0x14, 1280, 720)
        out_dir = os.path.join(self.tmp.name, "out_bk2_alias")
        os.makedirs(out_dir)
        arch = os.path.join(out_dir, f"{16:012x}.bk2")
        with open(arch, "wb") as f:
            f.write(b"9FPR" + struct.pack("<I", 16) + b"\x00" * 8 + bytes(bk2))
        with open(arch, "rb") as f:
            original = f.read()
        p = subprocess.run([sys.executable, "-I", TOOL, arch, out_dir], capture_output=True, text=True)
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("must not overwrite input archive", p.stderr)
        self.assertNotIn("Traceback", p.stderr)
        with open(arch, "rb") as f:
            self.assertEqual(f.read(), original)

    def test_failed_index_replace_preserves_previous_and_cleans_staging(self):
        rpf = self._write_archive("ok.rpf", zlib.compress(b"B" * 128), base=16)
        out_dir = os.path.join(self.tmp.name, "out_ro")
        os.makedirs(out_dir)
        idx = os.path.join(out_dir, "index.json")
        previous = '{"previous": true}\n'
        with open(idx, "w", encoding="utf-8", newline="\n") as f:
            f.write(previous)
        with mock.patch.object(rpf9_carve.json, "dump", side_effect=OSError("disk full")):
            with self.assertRaises(SystemExit) as cm:
                rpf9_carve.main([rpf, out_dir, "--index-only"])
        self.assertIn("cannot write", str(cm.exception))
        with open(idx, encoding="utf-8") as f:
            self.assertEqual(f.read(), previous)
        self.assertEqual([n for n in os.listdir(out_dir) if n.startswith(".rpf9_carve-")], [])

        os.chmod(idx, 0o444)
        try:
            with mock.patch.object(os, "replace", side_effect=PermissionError("replace denied")):
                with self.assertRaises(SystemExit) as cm:
                    rpf9_carve.main([rpf, out_dir, "--index-only"])
            self.assertIn("cannot write", str(cm.exception))
            with open(idx, encoding="utf-8") as f:
                self.assertEqual(f.read(), previous)
            self.assertEqual([n for n in os.listdir(out_dir) if n.startswith(".rpf9_carve-")], [])
            if os.name == "nt":
                p = subprocess.run([sys.executable, "-I", TOOL, rpf, out_dir, "--index-only"],
                                   capture_output=True, text=True)
                self.assertNotEqual(p.returncode, 0)
                self.assertIn("cannot write", p.stderr)
                self.assertNotIn("Traceback", p.stderr)
                with open(idx, encoding="utf-8") as f:
                    self.assertEqual(f.read(), previous)
                self.assertEqual([n for n in os.listdir(out_dir) if n.startswith(".rpf9_carve-")], [])
        finally:
            for entry in os.listdir(out_dir):
                try:
                    os.chmod(os.path.join(out_dir, entry), 0o600)
                except OSError:
                    pass

    def test_index_json_ends_with_newline(self):
        rpf = self._write_archive("nl.rpf", zlib.compress(b"C" * 128), base=16)
        out_dir = os.path.join(self.tmp.name, "out_nl")
        rc = rpf9_carve.main([rpf, out_dir, "--index-only"])
        self.assertEqual(rc, 0)
        with open(os.path.join(out_dir, "index.json"), "rb") as f:
            data = f.read()
        self.assertTrue(data.endswith(b"\n"))


if __name__ == "__main__":
    unittest.main()
