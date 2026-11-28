"""Tests for tools/conformance/native/scripts/negative-control.ps1 (Windows PowerShell 5.1)."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "conformance" / "native" / "scripts" / "negative-control.ps1"
SYS32 = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32"
WHERE_EXE = SYS32 / "where.exe"
HOSTNAME_EXE = SYS32 / "hostname.exe"

RENDERER_STUB = (
    "description.DepthStencilState.DepthEnable = TRUE;\n"
    "description.BlendState.RenderTarget[0].BlendEnable = TRUE;\n"
)
PLAN_STUB = (
    "plan.scissor = {conform::kScissorLeft, conform::kScissorTop, conform::kScissorRight,\n"
    "conform::kScissorBottom};\n"
)


@unittest.skipUnless(
    os.name == "nt" and shutil.which("powershell.exe") and WHERE_EXE.is_file() and HOSTNAME_EXE.is_file(),
    "requires Windows PowerShell 5.1",
)
class NegativeControlScriptTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.base = Path(tmp.name)
        self.native = self.base / "native"
        self.bin_dir = self.base / "bin"
        (self.native / "scripts").mkdir(parents=True)
        (self.native / "src").mkdir(parents=True)
        self.bin_dir.mkdir(parents=True)
        shutil.copy(SCRIPT, self.native / "scripts" / "negative-control.ps1")
        (self.native / "CMakeLists.txt").write_text("# stub\n", encoding="utf-8", newline="\n")
        (self.native / "src" / "renderer.cpp").write_text(RENDERER_STUB, encoding="utf-8", newline="\n")
        (self.native / "src" / "scene_plan.cpp").write_text(PLAN_STUB, encoding="utf-8", newline="\n")
        self.ps1 = self.native / "scripts" / "negative-control.ps1"
        self.scratch = self.native / "negative-control"

    def run_ps1(self, cmake_body: str):
        cmake_cmd = self.bin_dir / "cmake.cmd"
        cmake_cmd.write_bytes(cmake_body.replace("\n", "\r\n").encode("ascii"))
        env = {**os.environ, "PATH": str(self.bin_dir) + os.pathsep + os.environ["PATH"]}
        return subprocess.run(
            ["powershell.exe", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", str(self.ps1)],
            env=env, capture_output=True, text=True, timeout=60,
        )

    def test_configure_failure_stops_before_build_and_cleans_scratch(self):
        sentinel = self.base / "build_was_called.txt"
        cmake_body = (
            "@echo off\n"
            'if "%1"=="-S" exit /b 1\n'
            'if "%1"=="--build" (\n'
            f'  echo called > "{sentinel}"\n'
            "  exit /b 0\n"
            ")\n"
        )
        res = self.run_ps1(cmake_body)
        self.assertEqual(res.returncode, 1, res.stdout + res.stderr)
        self.assertIn("CONFIGURE FAILED 1", res.stdout)
        self.assertFalse(sentinel.exists(), "--build ran after cmake configure failed")
        self.assertFalse(self.scratch.exists(), "scratch folder was not cleaned up")

    def test_stderr_and_zero_failed_scenes_report_clean_failure_and_clean_scratch(self):
        cmake_body = (
            "@echo off\n"
            'if "%1"=="-S" (\n'
            '  if not exist "%4\\Release" mkdir "%4\\Release"\n'
            f'  copy /y "{WHERE_EXE}" "%4\\Release\\conform_native.exe" >nul\n'
            "  exit /b 0\n"
            ")\n"
            'if "%1"=="--build" exit /b 0\n'
        )
        res = self.run_ps1(cmake_body)
        self.assertEqual(res.returncode, 1, res.stdout + res.stderr)
        self.assertIn("NEGATIVE CONTROL FAILED", res.stdout)
        self.assertNotIn("NativeCommandError", res.stderr)
        self.assertNotIn("Compare-Object", res.stderr)
        self.assertFalse(self.scratch.exists(), "scratch folder was not cleaned up")

    def test_zero_exit_exe_reports_clean_failure_and_clean_scratch(self):
        cmake_body = (
            "@echo off\n"
            'if "%1"=="-S" (\n'
            '  if not exist "%4\\Release" mkdir "%4\\Release"\n'
            f'  copy /y "{HOSTNAME_EXE}" "%4\\Release\\conform_native.exe" >nul\n'
            "  exit /b 0\n"
            ")\n"
            'if "%1"=="--build" exit /b 0\n'
        )
        res = self.run_ps1(cmake_body)
        self.assertEqual(res.returncode, 1, res.stdout + res.stderr)
        self.assertIn("NEGATIVE CONTROL FAILED", res.stdout)
        self.assertFalse(self.scratch.exists(), "scratch folder was not cleaned up")

    def test_missing_source_anchor_throws_and_cleans_scratch(self):
        (self.native / "src" / "renderer.cpp").write_text("// missing anchors\n", encoding="utf-8", newline="\n")
        res = self.run_ps1("@echo off\nexit /b 0\n")
        self.assertNotEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn("expected exactly one occurrence", res.stderr)
        self.assertFalse(self.scratch.exists(), "scratch folder was not cleaned up")


if __name__ == "__main__":
    unittest.main()
