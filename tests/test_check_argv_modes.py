"""Unit tests for runtime/test/check_argv_modes.py (no real loader or MSVC binaries required).

    python -I tests/test_check_argv_modes.py
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
    "check_argv_modes",
    ROOT / "runtime" / "test" / "check_argv_modes.py",
)
cam = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(cam)


def _fake_windll():
    return types.SimpleNamespace(kernel32=types.SimpleNamespace(GetACP=lambda: 65001))


class CheckArgvModesTests(unittest.TestCase):
    def test_preflight_rejects_bad_loader_fixtures_workdir_and_report(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            loader = root / "gta6rt.exe"
            loader.write_bytes(b"")
            fixtures = root / "fixtures"
            fixtures.mkdir()
            work = root / "work"
            work.mkdir()
            blocker = root / "blocker"
            blocker.write_bytes(b"x")

            bad_cases = (
                ["--loader", str(root / "missing.exe"), "--fixtures", str(fixtures), "--work-dir", str(work)],
                ["--loader", str(loader), "--fixtures", str(blocker), "--work-dir", str(work)],
                ["--loader", str(loader), "--fixtures", str(fixtures), "--work-dir", str(root / "missing_work")],
                ["--loader", str(loader), "--fixtures", str(fixtures), "--work-dir", str(work), "--report", str(fixtures)],
                ["--loader", str(loader), "--fixtures", str(fixtures), "--work-dir", str(work), "--report", str(blocker / "rep.json")],
                ["--loader", str(loader), "--fixtures", str(fixtures), "--work-dir", str(work), "--report", "  "],
            )
            with mock.patch.object(cam, "_is_windows", return_value=True):
                for argv in bad_cases:
                    with self.subTest(argv=argv):
                        err = io.StringIO()
                        with contextlib.redirect_stderr(err), self.assertRaises(SystemExit) as cm:
                            cam.main(argv)
                        self.assertEqual(cm.exception.code, 2)

    def test_run_handles_timeout_and_oserror_without_traceback(self) -> None:
        for exc in (subprocess.TimeoutExpired(["x"], 15), OSError("cannot execute")):
            with self.subTest(exc=type(exc).__name__):
                with mock.patch.object(cam.subprocess, "run", side_effect=exc):
                    res = cam.run(Path("a.exe"), "a.exe", Path("."), {})
                self.assertIsNone(res["exit_code"])
                self.assertEqual(res["lines"], [])
                self.assertNotEqual(res["stderr"], "")

    def test_strip_env_keys_case_insensitive_on_windows_and_atomic_report(self) -> None:
        env = {"leonida_game_dir": "X", "Leonida_Argv_Sequence": "1", "KEEP": "yes"}
        with mock.patch.object(cam, "_is_windows", return_value=True):
            cam._strip_env_keys(env, ("LEONIDA_GAME_DIR", "LEONIDA_ARGV_SEQUENCE"))
        self.assertEqual(env, {"KEEP": "yes"})

        with tempfile.TemporaryDirectory() as td:
            rep_dir = Path(td) / "rep"
            rep_dir.mkdir()
            rep = rep_dir / "argv.json"
            rep.write_bytes(b'{"old": 1}\n')
            with mock.patch.object(cam.os, "replace", side_effect=PermissionError("denied")):
                with self.assertRaises(PermissionError):
                    cam._write_report_atomic(rep, {"new": 2})
            self.assertEqual(rep.read_bytes(), b'{"old": 1}\n')
            self.assertEqual([p.name for p in rep_dir.iterdir() if p.name.startswith(".report-")], [])

    def test_main_strips_windows_env_and_writes_report_atomically(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            loader = root / "gta6rt.exe"
            loader.write_bytes(b"")
            fixtures = root / "fixtures"
            fixtures.mkdir()
            for width in ("a", "w"):
                for mode in ("default", "expand", "noargs"):
                    (fixtures / f"tiny_argv_{mode}_{width}.exe").write_bytes(b"")
            work = root / "work"
            work.mkdir()
            report = root / "out" / "argv_modes.json"
            seen_envs = []

            def fake_run(cmd, executable, cwd, env, capture_output, timeout):
                seen_envs.append(dict(env))
                is_mapped = Path(executable).resolve() == loader.resolve()
                seq = (env.get("LEONIDA_ARGV_SEQUENCE") == "1")
                if "noargs" in str(cmd):
                    head = "STATE entry argc=0 A=null W=null\n"
                elif "expand" in str(cmd) and ('"space dir/*.txt"' in cmd or "data/*.txt" in cmd or '"data/*.txt"' in cmd):
                    head = "STATE entry argc=4 A=ok W=ok\n"
                else:
                    head = "STATE entry argc=3 A=ok W=ok\n"
                extra = ""
                if seq:
                    extra += "".join(f"NOOP {i} status=0 unchanged=1\n" for i in range(6))
                    extra += "".join(f"CONFIG {i} status=0\n" for i in range(4))
                if is_mapped:
                    extra += "game exited with code 0\n"
                return subprocess.CompletedProcess(cmd, 0, stdout=(head + extra).encode("utf-8"), stderr=b"")

            with mock.patch.object(cam, "_is_windows", return_value=True), \
                    mock.patch.object(cam.ctypes, "windll", _fake_windll(), create=True), \
                    mock.patch.dict(cam.os.environ, {"leonida_game_dir": "OLD", "Leonida_Argv_Sequence": "99"}, clear=False), \
                    mock.patch.object(cam.subprocess, "run", side_effect=fake_run), \
                    contextlib.redirect_stdout(io.StringIO()):
                rc = cam.main(["--loader", str(loader), "--fixtures", str(fixtures), "--work-dir", str(work), "--report", str(report)])
            self.assertEqual(rc, 0)
            self.assertTrue(all(not any(k.upper() == "LEONIDA_GAME_DIR" for k in e) for e in seen_envs))
            raw = report.read_bytes()
            self.assertTrue(raw.endswith(b"\n"))
            self.assertEqual(json.loads(raw.decode("utf-8"))["failures"], [])


if __name__ == "__main__":
    unittest.main()
