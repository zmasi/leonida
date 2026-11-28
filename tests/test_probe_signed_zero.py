"""Unit tests for runtime/test/shaders/probe_signed_zero.py (no Vulkan GPU or SPIR-V tools needed).

    python -I tests/test_probe_signed_zero.py
"""
from __future__ import annotations

import contextlib
import importlib.util
import io
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "probe_signed_zero",
    ROOT / "runtime" / "test" / "shaders" / "probe_signed_zero.py",
)
probe = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(probe)


class ProbeSignedZeroTests(unittest.TestCase):
    def test_rejects_empty_missing_and_directory_harness_before_subprocess(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            subdir = root / "subdir"
            subdir.mkdir()
            missing = root / "missing_harness.exe"

            with mock.patch.object(probe.subprocess, "run", side_effect=AssertionError("called subprocess")):
                for blank in ("", "   "):
                    with self.subTest(blank=blank):
                        err = io.StringIO()
                        with contextlib.redirect_stderr(err), self.assertRaises(SystemExit) as cm:
                            probe.main([blank])
                        self.assertEqual(cm.exception.code, 2)
                        self.assertIn("must not be empty", err.getvalue())

                for bad in (subdir, missing):
                    with self.subTest(bad=bad.name):
                        err = io.StringIO()
                        with contextlib.redirect_stderr(err):
                            rc = probe.main([str(bad)])
                        self.assertEqual(rc, 2)
                        self.assertIn("ERROR:", err.getvalue())

    def test_subprocess_failures_return_one_without_traceback(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            harness = Path(td) / "vk_store_test"
            harness.write_bytes(b"")
            failures = (
                FileNotFoundError("spirv-as not found"),
                subprocess.CalledProcessError(1, ["spirv-val"], stderr="bad spv"),
                subprocess.TimeoutExpired(["vk_store_test"], 60),
            )
            for exc in failures:
                with self.subTest(exc=type(exc).__name__):
                    err = io.StringIO()
                    with mock.patch.object(probe.subprocess, "run", side_effect=exc), \
                            contextlib.redirect_stdout(io.StringIO()), \
                            contextlib.redirect_stderr(err):
                        rc = probe.main([str(harness)])
                    self.assertEqual(rc, 1)
                    self.assertIn("ERROR:", err.getvalue())
                    self.assertIn("Unknown", err.getvalue())

    def test_aggregates_return_codes_and_passes_resolved_harness_paths(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            harness = Path(td) / "vk_store_test"
            harness.write_bytes(b"")

            for codes, expected in (
                ([0, 0, 0, 0, 0, 0], 0),
                ([0, 0, 77, 0, 0, 0], 77),
                ([0, 0, 0, 0, 0, 77], 77),
                ([0, 0, 3, 0, 0, 0], 1),
            ):
                with self.subTest(expected=expected):
                    it = iter(codes)
                    calls = []

                    def fake_run(cmd, **_kwargs):
                        calls.append(list(cmd))
                        return subprocess.CompletedProcess(cmd, next(it))

                    out = io.StringIO()
                    with mock.patch.object(probe.subprocess, "run", side_effect=fake_run), \
                            contextlib.redirect_stdout(out):
                        rc = probe.main([str(harness)])
                    self.assertEqual(rc, expected)
                    self.assertEqual(len(calls), 6)
                    self.assertEqual(calls[0][0], "spirv-as")
                    self.assertEqual(calls[1][0], "spirv-val")
                    self.assertEqual(calls[2][0], str(harness.resolve()))
                    self.assertEqual(calls[2][2], "signed-zero")


if __name__ == "__main__":
    unittest.main()
