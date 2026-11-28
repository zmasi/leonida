"""Unit tests for the one-command synthetic source-only smoke suite (DX-07)."""
from __future__ import annotations

import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SMOKE_SCRIPT = ROOT / "scripts" / "smoke.py"
DEV_SCRIPT = ROOT / "scripts" / "dev.py"


class SmokeSuiteTests(unittest.TestCase):
    def invoke(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, "-I", "-B", str(SMOKE_SCRIPT), *args],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=30,
        )

    def invoke_dev(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, "-I", "-B", str(DEV_SCRIPT), "smoke", *args],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=30,
        )

    def test_help_exits_cleanly(self):
        proc = self.invoke("--help")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("DX-07", proc.stdout)
        self.assertIn("--quick", proc.stdout)
        self.assertIn("--prereqs-only", proc.stdout)
        self.assertIn("--json", proc.stdout)

    def test_prereqs_only_human_readable(self):
        proc = self.invoke("--prereqs-only")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("Environment Discovery", proc.stdout)
        self.assertIn("python:", proc.stdout)
        self.assertIn("cmake:", proc.stdout)

    def test_prereqs_only_json_schema(self):
        proc = self.invoke("--prereqs-only", "--json")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        data = json.loads(proc.stdout)
        self.assertEqual(data["schema_version"], 1)
        self.assertEqual(data["verdict"], "PASS")
        self.assertIn("python", data["prerequisites"])
        self.assertTrue(data["prerequisites"]["python"]["satisfies"])
        self.assertIn("toolchains", data)
        self.assertIn("cmake", data["toolchains"])

    def test_quick_smoke_json_report(self):
        proc = self.invoke("--quick", "--json")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        data = json.loads(proc.stdout)
        self.assertEqual(data["schema_version"], 1)
        self.assertEqual(data["verdict"], "PASS")
        self.assertIn("checks", data)
        check_names = {c["name"] for c in data["checks"]}
        self.assertIn("python_compileall", check_names)
        self.assertIn("clean_room_no_game_data", check_names)
        self.assertIn("cli_help_make_game.py", check_names)
        for c in data["checks"]:
            self.assertIn(c["status"], ("PASS", "SKIP"))
            self.assertIsInstance(c["duration_ms"], int)

    def test_dev_smoke_forwarding(self):
        proc = self.invoke_dev("--quick")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("Source-Only Validation", proc.stdout)
        self.assertIn("Verdict: PASS", proc.stdout)

    def test_missing_required_toolchain_fails_with_code_2(self):
        proc = self.invoke("--require", "nonexistent_mock_toolchain_xyz")
        self.assertEqual(proc.returncode, 2)
        self.assertIn("nonexistent_mock_toolchain_xyz", proc.stderr)

    def test_missing_required_toolchain_json_output(self):
        proc = self.invoke("--require", "nonexistent_mock_toolchain_xyz", "--json")
        self.assertEqual(proc.returncode, 2)
        data = json.loads(proc.stdout)
        self.assertEqual(data["verdict"], "ERROR")
        self.assertIn("error", data)

    def test_compile_failure_keeps_json_stdout_clean(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_root = Path(tmp)
            scripts_dir = tmp_root / "scripts"
            tools_dir = tmp_root / "tools"
            scripts_dir.mkdir()
            tools_dir.mkdir()
            shutil.copy2(SMOKE_SCRIPT, scripts_dir / "smoke.py")
            shutil.copy2(DEV_SCRIPT, scripts_dir / "dev.py")
            (tmp_root / "make_game.py").write_text("def broken(:\n", encoding="utf-8", newline="\n")
            for args in (
                [str(scripts_dir / "smoke.py"), "--quick", "--json"],
                [str(scripts_dir / "dev.py"), "smoke", "--quick", "--json"],
            ):
                with self.subTest(entry=args[0]):
                    proc = subprocess.run(
                        [sys.executable, "-I", "-B", *args],
                        cwd=tmp_root,
                        capture_output=True,
                        text=True,
                        timeout=30,
                    )
                    self.assertEqual(proc.returncode, 1, proc.stderr)
                    data = json.loads(proc.stdout)
                    self.assertEqual(data["verdict"], "FAIL")
                    compile_chk = next(c for c in data["checks"] if c["name"] == "python_compileall")
                    self.assertEqual(compile_chk["status"], "FAIL")
                    self.assertTrue(proc.stderr.strip())


if __name__ == "__main__":
    unittest.main()
