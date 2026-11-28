"""Tests for tools/conformance/native/scripts/run.ps1 exit propagation and path safety.

    python -I tests/test_conformance_run_exit.py
"""
from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools" / "conformance" / "native" / "scripts" / "run.ps1"


class ConformanceRunExitTests(unittest.TestCase):
    def test_static_exit_propagation_and_guards(self) -> None:
        text = SCRIPT.read_text(encoding="utf-8")
        self.assertIn("IsNullOrWhiteSpace", text)
        self.assertIn("is the native conformance folder", text)
        self.assertIn("exit $rc", text)

    @unittest.skipUnless(os.name == "nt" and shutil.which("powershell.exe"), "requires Windows PowerShell")
    def test_propagates_renderer_exit_code_and_refuses_blank_or_root_output(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            base = Path(td)
            native = base / "tools" / "conformance" / "native"
            scripts = native / "scripts"
            exe_dir = native / "build" / "Release"
            scripts.mkdir(parents=True)
            exe_dir.mkdir(parents=True)
            shutil.copy2(SCRIPT, scripts / "run.ps1")
            exe_path = exe_dir / "conform_native.exe"

            ps_exe = "powershell.exe"
            ps_base = [ps_exe, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass"]
            compile_cmd = (
                "$ErrorActionPreference = 'Stop'; "
                "Add-Type -OutputAssembly $env:STUB_EXE -OutputType ConsoleApplication -TypeDefinition @'\n"
                "using System;\n"
                "public class Program {\n"
                "    public static int Main(string[] args) {\n"
                "        string v = Environment.GetEnvironmentVariable(\"LEONIDA_TEST_EXIT\");\n"
                "        return string.IsNullOrEmpty(v) ? 0 : int.Parse(v);\n"
                "    }\n"
                "}\n'@"
            )
            env = {**os.environ, "STUB_EXE": str(exe_path)}
            env["PSModulePath"] = str(Path(os.environ["SystemRoot"]) / "System32/WindowsPowerShell/v1.0/Modules")
            subprocess.run([*ps_base, "-Command", compile_cmd], env=env, check=True, timeout=60)

            out_dir = base / "out_ok"
            for code in (0, 7):
                with self.subTest(code=code):
                    res = subprocess.run(
                        [*ps_base, "-File", str(scripts / "run.ps1"), "-Output", str(out_dir)],
                        env={**env, "LEONIDA_TEST_EXIT": str(code)},
                        capture_output=True,
                        text=True,
                        timeout=30,
                    )
                    self.assertEqual(res.returncode, code, res.stdout + res.stderr)
                    self.assertIn(f"EXITCODE {code}", res.stdout)

            blank = subprocess.run(
                [*ps_base, "-File", str(scripts / "run.ps1"), "-Output", "   "],
                env={**env, "LEONIDA_TEST_EXIT": "0"},
                capture_output=True,
                text=True,
                timeout=30,
            )
            self.assertEqual(blank.returncode, 2, blank.stdout + blank.stderr)
            self.assertIn("is blank", blank.stdout)

            root_res = subprocess.run(
                [*ps_base, "-File", str(scripts / "run.ps1"), "-Output", str(native)],
                env={**env, "LEONIDA_TEST_EXIT": "0"},
                capture_output=True,
                text=True,
                timeout=30,
            )
            self.assertEqual(root_res.returncode, 2, root_res.stdout + root_res.stderr)
            self.assertIn("is the native conformance folder", root_res.stdout)


if __name__ == "__main__":
    unittest.main()
