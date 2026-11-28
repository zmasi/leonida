"""Regression test for --game-dir validation in tools/pe_inspect.py."""

from __future__ import annotations

import contextlib
import io
import sys
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

import pe_inspect  # noqa: E402


class TestPeInspectGameDir(unittest.TestCase):
    def test_missing_game_dir_returns_error_before_scanning_images(self) -> None:
        with TemporaryDirectory() as tmp:
            img = Path(tmp) / "img.exe"
            img.write_bytes(b"not-a-pe")
            missing = Path(tmp) / "no_such_game_dir"
            out_dir = Path(tmp) / "out"
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                rc = pe_inspect.main([str(img), "--game-dir", str(missing), "--out", str(out_dir)])
            self.assertEqual(rc, 2)
            self.assertIn("--game-dir", err.getvalue())
            self.assertFalse(out_dir.exists())

    def test_file_as_game_dir_is_rejected(self) -> None:
        with TemporaryDirectory() as tmp:
            img = Path(tmp) / "img.exe"
            img.write_bytes(b"not-a-pe")
            not_a_dir = Path(tmp) / "not_a_dir.txt"
            not_a_dir.write_text("x", encoding="utf-8", newline="\n")
            out_dir = Path(tmp) / "out"
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                rc = pe_inspect.main([str(img), "--game-dir", str(not_a_dir), "--out", str(out_dir)])
            self.assertEqual(rc, 2)
            self.assertIn("not a directory", err.getvalue())
            self.assertFalse(out_dir.exists())


if __name__ == "__main__":
    unittest.main()
