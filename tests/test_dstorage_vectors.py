"""Unit tests for runtime/test/dstorage_vectors.py CLI validation and atomic output write.

    python -I tests/test_dstorage_vectors.py
"""
from __future__ import annotations

import contextlib
import importlib.util
import io
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "dstorage_vectors",
    ROOT / "runtime" / "test" / "dstorage_vectors.py",
)
dv = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(dv)


class DstorageVectorsTests(unittest.TestCase):
    def test_help_and_invalid_output_paths_fail_before_write(self) -> None:
        out = io.StringIO()
        with contextlib.redirect_stdout(out), self.assertRaises(SystemExit) as cm:
            dv.main(["--help"])
        self.assertEqual(cm.exception.code, 0)
        self.assertIn("header file to write", out.getvalue())

        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            subdir = root / "dir"
            subdir.mkdir()
            blocker = root / "blocker"
            blocker.write_bytes(b"x")

            bad_cases = (
                [],
                ["a.h", "b.h"],
                [""],
                ["   "],
                [str(subdir)],
                [str(root / "out.h") + os.sep],
                [str(blocker / "out.h")],
                [str(blocker / "nested" / "out.h")],
            )
            with mock.patch.object(dv, "write", side_effect=AssertionError("called write")):
                for argv in bad_cases:
                    with self.subTest(argv=argv):
                        err = io.StringIO()
                        with contextlib.redirect_stderr(err), self.assertRaises(SystemExit) as bad_cm:
                            dv.main(argv)
                        self.assertEqual(bad_cm.exception.code, 2)

    def test_write_file_atomic_preserves_existing_on_failure_and_main_writes_lf(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            out_dir = root / "nested"
            out_file = out_dir / "vectors.h"
            out_dir.mkdir()
            out_file.write_bytes(b"// keep\n")

            with mock.patch.object(dv.os, "replace", side_effect=PermissionError("locked")):
                with self.assertRaises(PermissionError):
                    dv.write_file_atomic(out_file, "// new\n")
            self.assertEqual(out_file.read_bytes(), b"// keep\n")
            self.assertEqual([p.name for p in out_dir.iterdir() if p.name.startswith(".dstorage-vectors-")], [])

            def fake_write(handle):
                handle.write("static constexpr int ok = 1;\n")

            with mock.patch.object(dv, "write", side_effect=fake_write):
                rc = dv.main([str(out_file)])
            self.assertEqual(rc, 0)
            raw = out_file.read_bytes()
            self.assertEqual(raw, b"static constexpr int ok = 1;\n")
            self.assertNotIn(b"\r\n", raw)

            err = io.StringIO()
            with mock.patch.object(dv, "write", side_effect=fake_write), \
                    mock.patch.object(dv, "write_file_atomic", side_effect=OSError("disk full")), \
                    contextlib.redirect_stderr(err):
                rc_err = dv.main([str(out_file)])
            self.assertEqual(rc_err, 1)
            self.assertIn("ERROR:", err.getvalue())


if __name__ == "__main__":
    unittest.main()
