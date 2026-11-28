"""Unit tests for runtime/test/check_module_names.py using synthetic PE bytes and mocked runs.

    python -I tests/test_check_module_names.py
"""
from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import subprocess
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "check_module_names",
    ROOT / "runtime" / "test" / "check_module_names.py",
)
cmn = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(cmn)


def _make_fake_kernel32():
    return types.SimpleNamespace(kernel32=types.SimpleNamespace(GetACP=lambda: 65001))


class CheckModuleNamesTests(unittest.TestCase):
    def test_preflight_rejects_bad_inputs_work_dir_and_report_before_mkdir(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            ok = root / "ok.exe"
            ok.write_bytes(b"tiny_dll.dll\0")
            blocker = root / "blocker"
            blocker.write_bytes(b"x")
            subdir = root / "subdir"
            subdir.mkdir()

            base = [
                "--loader", str(ok),
                "--guest", str(ok),
                "--importer", str(ok),
                "--dependency", str(ok),
            ]
            bad_cases = (
                [*base, "--work-dir", str(blocker)],
                [*base, "--work-dir", str(blocker / "nested")],
                [*base, "--work-dir", str(root / "work"), "--report", str(subdir)],
                [*base, "--work-dir", str(root / "work"), "--report", str(blocker / "rep.json")],
                [*base, "--work-dir", str(root / "work"), "--report", "  "],
                ["--loader", str(root / "missing.exe"), "--guest", str(ok), "--importer", str(ok),
                 "--dependency", str(ok), "--work-dir", str(root / "work")],
            )
            with mock.patch.object(cmn, "_is_windows", return_value=True):
                for argv in bad_cases:
                    with self.subTest(argv=argv):
                        err = io.StringIO()
                        with contextlib.redirect_stderr(err), self.assertRaises(SystemExit) as cm:
                            cmn.main(argv)
                        self.assertEqual(cm.exception.code, 2)
                        self.assertFalse((root / "work").exists())

    def test_bom_trace_oserror_handling_and_atomic_report_write(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            loader = root / "gta6rt.exe"
            guest = root / "guest.exe"
            importer = root / "imp.exe"
            dep = root / "tiny_dll.dll"
            work = root / "work"
            report = root / "out" / "report.json"
            for p in (loader, guest, importer, dep):
                p.write_bytes(b"stub tiny_dll.dll\0stub")

            def fake_run(cmd, cwd, env, capture_output, timeout):
                if "--trace" in cmd:
                    trace = Path(cmd[cmd.index("--trace") + 1])
                    mods = [Path(cmd[2]).name]
                    if len(mods) == 1 and "Import-" in mods[0]:
                        mods.append("tiny_dll.dll")
                    lines = "".join(json.dumps({"kind": "map", "module": m}) + "\n" for m in mods)
                    trace.write_bytes(b"\xef\xbb\xbf" + lines.encode("utf-8"))
                out = b"NAME_PROBE entered=1 self_w=1 case_w=1 load_w=1 shim_dir_w=1 shim_dir_load_w=1\n"
                return subprocess.CompletedProcess(cmd, 0, stdout=out, stderr=b"")

            argv = [
                "--loader", str(loader),
                "--guest", str(guest),
                "--importer", str(importer),
                "--dependency", str(dep),
                "--work-dir", str(work),
                "--report", str(report),
            ]
            with mock.patch.object(cmn, "_is_windows", return_value=True), \
                    mock.patch.object(cmn.ctypes, "windll", _make_fake_kernel32(), create=True), \
                    mock.patch.object(cmn, "exact_acp", return_value=None), \
                    mock.patch.object(cmn.subprocess, "run", side_effect=fake_run), \
                    contextlib.redirect_stdout(io.StringIO()):
                rc = cmn.main(argv)
            self.assertEqual(rc, 0)
            self.assertIs(type(rc), int)
            raw = report.read_bytes()
            self.assertTrue(raw.endswith(b"\n"))
            self.assertNotIn(b"\r\n", raw)

            with mock.patch.object(cmn, "_is_windows", return_value=True), \
                    mock.patch.object(cmn.ctypes, "windll", _make_fake_kernel32(), create=True), \
                    mock.patch.object(cmn, "exact_acp", return_value=None), \
                    mock.patch.object(cmn.subprocess, "run", side_effect=OSError("missing binary")), \
                    contextlib.redirect_stdout(io.StringIO()):
                rc_err = cmn.main(argv)
            self.assertEqual(rc_err, 1)

    def test_write_report_atomic_preserves_existing_file_on_replace_failure(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            rep_dir = Path(td) / "rep"
            rep_dir.mkdir()
            rep = rep_dir / "r.json"
            rep.write_bytes(b'{"keep": true}\n')
            with mock.patch.object(cmn.os, "replace", side_effect=PermissionError("locked")):
                with self.assertRaises(PermissionError):
                    cmn._write_report_atomic(rep, {"new": 1})
            self.assertEqual(rep.read_bytes(), b'{"keep": true}\n')
            self.assertEqual([p.name for p in rep_dir.iterdir() if p.name.startswith(".report-")], [])


if __name__ == "__main__":
    unittest.main()
