"""Atomic --json write, path validation, and index-read tests for tools/gamedata_sps1.py.

    python -I tests/test_gamedata_sps1_atomic.py
"""
import contextlib
import io
import json
import os
import struct
import sys
import tempfile
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")
if TOOLS not in sys.path:
    sys.path.insert(0, TOOLS)

import gamedata_sps1  # noqa: E402


def _sps1_blob(entries: list[tuple[str, str, int]]) -> bytes:
    buf = bytearray(b"SPS1" + struct.pack("<I", len(entries)))
    for preset, effect, bucket in entries:
        p = preset.encode("ascii") + b"\x00"
        e = effect.encode("ascii") + b"\x00"
        buf += bytes([len(p)]) + p + bytes([len(e)]) + e + bytes([bucket])
    return bytes(buf)


class GamedataSps1AtomicTests(unittest.TestCase):
    def test_json_output_ends_with_trailing_lf_newline(self):
        with tempfile.TemporaryDirectory() as tmp:
            bin_path = os.path.join(tmp, "sample.bin")
            out_json = os.path.join(tmp, "out", "sps1.json")
            with open(bin_path, "wb") as f:
                f.write(_sps1_blob([("alpha.sps", "alpha", 0)]))
            with contextlib.redirect_stdout(io.StringIO()):
                rc = gamedata_sps1.main([bin_path, "--json", out_json])
            self.assertEqual(rc, 0)
            with open(out_json, "rb") as f:
                raw = f.read()
            self.assertTrue(raw.endswith(b"\n"))
            self.assertNotIn(b"\r\n", raw)

    def test_failed_json_write_and_replace_preserve_existing_report_and_clean_staging(self):
        with tempfile.TemporaryDirectory() as tmp:
            bin_path = os.path.join(tmp, "sample.bin")
            out_dir = os.path.join(tmp, "out")
            out_json = os.path.join(out_dir, "sps1.json")
            os.makedirs(out_dir)
            with open(bin_path, "wb") as f:
                f.write(_sps1_blob([("alpha.sps", "alpha", 0)]))
            before = b'{"keep": true}\n'
            with open(out_json, "wb") as f:
                f.write(before)

            def fail_dump(_data, handle, **_kwargs):
                handle.write("{")
                raise OSError("disk full")

            for target_obj, attr, side_effect in (
                (gamedata_sps1.json, "dump", fail_dump),
                (os, "replace", PermissionError("locked")),
            ):
                with self.subTest(mock_attr=attr):
                    err = io.StringIO()
                    with mock.patch.object(target_obj, attr, side_effect=side_effect), \
                            contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(err):
                        rc = gamedata_sps1.main([bin_path, "--json", out_json])
                    self.assertEqual(rc, 1)
                    self.assertIn("cannot write", err.getvalue())
                    with open(out_json, "rb") as f:
                        self.assertEqual(f.read(), before)
                    self.assertEqual(os.listdir(out_dir), ["sps1.json"])

    def test_empty_whitespace_and_trailing_sep_json_are_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            bin_path = os.path.join(tmp, "sample.bin")
            with open(bin_path, "wb") as f:
                f.write(_sps1_blob([("alpha.sps", "alpha", 0)]))
            for bad in ("", "   ", os.path.join(tmp, "reports") + os.sep):
                with self.subTest(bad=bad):
                    err = io.StringIO()
                    with contextlib.redirect_stderr(err), self.assertRaises(SystemExit) as cm:
                        gamedata_sps1.main([bin_path, "--json", bad])
                    self.assertEqual(cm.exception.code, 2)
                    self.assertIn("--json", err.getvalue())

    def test_directory_target_reads_index_json_once(self):
        with tempfile.TemporaryDirectory() as tmp:
            carve = os.path.join(tmp, "carve")
            os.makedirs(carve)
            blob = _sps1_blob([("alpha.sps", "alpha", 0)])
            with open(os.path.join(carve, "000000001000.bin"), "wb") as f:
                f.write(blob)
            with open(os.path.join(carve, "index.json"), "w", encoding="utf-8", newline="\n") as f:
                json.dump({"files": [{"off": 0x1000, "stored": len(blob), "size": len(blob), "head": blob[:16].hex()}]}, f)
            out_json = os.path.join(tmp, "report.json")
            orig = gamedata_sps1.gm._read_index_rows
            with mock.patch.object(gamedata_sps1.gm, "_read_index_rows", wraps=orig) as spy, \
                    contextlib.redirect_stdout(io.StringIO()):
                rc = gamedata_sps1.main([carve, "--json", out_json])
            self.assertEqual(rc, 0)
            self.assertEqual(spy.call_count, 1)

    def test_symlinked_json_updates_target_in_place(self):
        with tempfile.TemporaryDirectory() as tmp:
            bin_path = os.path.join(tmp, "sample.bin")
            real_json = os.path.join(tmp, "real.json")
            link_json = os.path.join(tmp, "link.json")
            with open(bin_path, "wb") as f:
                f.write(_sps1_blob([("alpha.sps", "alpha", 0)]))
            with open(real_json, "wb") as f:
                f.write(b"{}\n")
            try:
                os.symlink(real_json, link_json)
            except OSError as exc:
                self.skipTest(f"symlink unavailable: {exc}")
            with contextlib.redirect_stdout(io.StringIO()):
                rc = gamedata_sps1.main([bin_path, "--json", link_json])
            self.assertEqual(rc, 0)
            self.assertTrue(os.path.islink(link_json))
            with open(real_json, encoding="utf-8") as f:
                doc = json.load(f)
            self.assertEqual(doc["payloads"][0]["table"]["records"], 1)

    def test_non_dir_json_ancestor_refused_before_reading_target(self):
        with tempfile.TemporaryDirectory() as tmp:
            bin_path = os.path.join(tmp, "sample.bin")
            blocker = os.path.join(tmp, "file_blocker")
            with open(bin_path, "wb") as f:
                f.write(_sps1_blob([("alpha.sps", "alpha", 0)]))
            with open(blocker, "wb") as f:
                f.write(b"not a dir")
            for bad_out in (os.path.join(blocker, "out.json"), os.path.join(blocker, "nested", "out.json")):
                with self.subTest(bad_out=bad_out):
                    err = io.StringIO()
                    with mock.patch.object(gamedata_sps1, "read_payloads", side_effect=AssertionError("read target")), \
                            contextlib.redirect_stderr(err):
                        rc = gamedata_sps1.main([bin_path, "--json", bad_out])
                    self.assertEqual(rc, 1)
                    self.assertIn("error:", err.getvalue())

    def test_directory_shaders_and_special_target_fail_upfront_exit_2(self):
        with tempfile.TemporaryDirectory() as tmp:
            bin_path = os.path.join(tmp, "sample.bin")
            shader_dir = os.path.join(tmp, "shaders_dir")
            os.makedirs(shader_dir)
            with open(bin_path, "wb") as f:
                f.write(_sps1_blob([("alpha.sps", "alpha", 0)]))
            err = io.StringIO()
            with mock.patch.object(gamedata_sps1, "read_payloads", side_effect=AssertionError("read target")), \
                    contextlib.redirect_stderr(err):
                rc = gamedata_sps1.main([bin_path, "--shaders", shader_dir])
            self.assertEqual(rc, 2)
            self.assertIn("ERROR:", err.getvalue())

            err2 = io.StringIO()
            with mock.patch.object(gamedata_sps1.os.path, "exists", return_value=True), \
                    mock.patch.object(gamedata_sps1.os.path, "isfile", return_value=False), \
                    mock.patch.object(gamedata_sps1.os.path, "isdir", return_value=False), \
                    contextlib.redirect_stderr(err2):
                rc2 = gamedata_sps1.main([bin_path])
            self.assertEqual(rc2, 2)
            self.assertIn("ERROR:", err2.getvalue())


if __name__ == "__main__":
    unittest.main()
