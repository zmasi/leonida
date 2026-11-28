"""Tests for tools/conformance/native/scripts/verify.ps1 and failure-log.cmd.

    python -I tests/test_conformance_verify_scripts.py
"""
from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VERIFY_PS1 = ROOT / "tools" / "conformance" / "native" / "scripts" / "verify.ps1"
FAILURE_CMD = ROOT / "tools" / "conformance" / "native" / "scripts" / "failure-log.cmd"

STUB_CS = r"""
using System;
using System.IO;
using System.Text;

public class ConformStub {
    public static int Main(string[] args) {
        if (args.Length == 0) {
            Console.Error.WriteLine("usage: conform_native <output-directory> [options]");
            return 2;
        }
        string outDir = args[0];
        string shaders = null;
        int adapter = -1;
        for (int i = 1; i < args.Length; i++) {
            if (args[i] == "--shaders" && i + 1 < args.Length) {
                shaders = args[++i];
            } else if (args[i] == "--adapter" && i + 1 < args.Length) {
                adapter = int.Parse(args[++i]);
            } else if (args[i].StartsWith("--")) {
                return 2;
            }
        }
        if (File.Exists(outDir)) {
            Console.Error.WriteLine("create_directories failed: " + outDir);
            return 1;
        }
        if (shaders != null && !File.Exists(Path.Combine(shaders, "fullscreen_color.vs.cso"))) {
            Console.Error.WriteLine("missing fullscreen_color.vs.cso (build conform_shaders)");
            return 1;
        }
        if (Directory.Exists(Path.Combine(outDir, "clear.png"))) {
            Console.Error.WriteLine("cannot write clear.png\nRESULT: FAIL (run did not complete)");
            return 1;
        }
        Directory.CreateDirectory(outDir);
        if (adapter == 0) {
            Console.WriteLine("adapter: AMD Radeon Test");
            Console.WriteLine("readback checks: 18 passed, 0 failed");
            Console.WriteLine("png checks:      18 passed, 0 failed");
            return 0;
        }
        string[] scenes = {"clear", "tint", "scissor", "vtx", "depth", "blend"};
        foreach (string s in scenes) {
            File.WriteAllText(Path.Combine(outDir, s + ".png"), "PNG:" + s, Encoding.ASCII);
        }
        if (Path.GetFileName(outDir) == "b" && Environment.GetEnvironmentVariable("STUB_EXTRA_B") == "1") {
            File.WriteAllText(Path.Combine(outDir, "extra.png"), "EXTRA", Encoding.ASCII);
        }
        StringBuilder checks = new StringBuilder();
        for (int i = 0; i < 6; i++) {
            if (i > 0) checks.Append(",");
            checks.Append("{\"checks\": [1, 2, 3]}");
        }
        File.WriteAllText(
            Path.Combine(outDir, "manifest.json"),
            "{\"renderer\":\"native\",\"adapter\":\"RTX 5070 Ti Stub\",\"scenes\":[" + checks + "]}",
            Encoding.ASCII
        );
        return 0;
    }
}
"""


class ConformanceVerifyScriptsTests(unittest.TestCase):
    def test_static_literal_path_and_cmd_quoting(self) -> None:
        ps_text = VERIFY_PS1.read_text(encoding="utf-8")
        self.assertIn("-LiteralPath", ps_text)
        self.assertIn("Test-Path -LiteralPath $manifestPath -PathType Leaf", ps_text)
        self.assertIn("$namesB", ps_text)
        cmd_text = FAILURE_CMD.read_text(encoding="utf-8")
        self.assertIn('set "EXE=', cmd_text)
        self.assertIn('if not exist "%EXE%"', cmd_text)

    @unittest.skipUnless(os.name == "nt" and shutil.which("powershell.exe"), "requires Windows PowerShell and cmd.exe")
    def test_verify_and_failure_log_with_brackets_and_ampersand_paths(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            base = Path(td) / "native [1] & test"
            scripts = base / "scripts"
            exe_dir = base / "build" / "Release"
            scripts.mkdir(parents=True)
            exe_dir.mkdir(parents=True)
            shutil.copy2(VERIFY_PS1, scripts / "verify.ps1")
            shutil.copy2(FAILURE_CMD, scripts / "failure-log.cmd")
            exe_path = exe_dir / "conform_native.exe"

            keep = base / "out" / "failures" / "keep.txt"
            keep.parent.mkdir(parents=True)
            keep.write_text("keep", encoding="utf-8", newline="\n")

            plain_stub = Path(td) / "conform_stub.exe"
            ps_base = ["powershell.exe", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass"]
            env = {**os.environ, "STUB_EXE": str(plain_stub), "STUB_CS": STUB_CS}
            env["PSModulePath"] = str(Path(os.environ["SystemRoot"]) / "System32/WindowsPowerShell/v1.0/Modules")

            # Missing exe fails cleanly without wiping existing out/failures.
            miss_ps = subprocess.run([*ps_base, "-File", str(scripts / "verify.ps1")], env=env, capture_output=True, text=True, timeout=30)
            self.assertEqual(miss_ps.returncode, 1, miss_ps.stdout + miss_ps.stderr)
            miss_cmd = subprocess.run(["cmd.exe", "/c", "call", str(scripts / "failure-log.cmd")], env=env, capture_output=True, text=True, timeout=30)
            self.assertEqual(miss_cmd.returncode, 1, miss_cmd.stdout + miss_cmd.stderr)
            self.assertTrue(keep.exists())

            compile_cmd = (
                "$ErrorActionPreference = 'Stop'; "
                "Add-Type -OutputAssembly $env:STUB_EXE -OutputType ConsoleApplication -TypeDefinition $env:STUB_CS"
            )
            subprocess.run([*ps_base, "-Command", compile_cmd], env=env, check=True, timeout=60)
            shutil.move(str(plain_stub), str(exe_path))

            pass_ps = subprocess.run([*ps_base, "-File", str(scripts / "verify.ps1")], env=env, capture_output=True, text=True, timeout=60)
            self.assertEqual(pass_ps.returncode, 0, pass_ps.stdout + pass_ps.stderr)
            self.assertIn("VERIFY: PASS", pass_ps.stdout)

            # Extra file in run B must make verification fail cleanly.
            fail_ps = subprocess.run(
                [*ps_base, "-File", str(scripts / "verify.ps1")],
                env={**env, "STUB_EXTRA_B": "1"},
                capture_output=True,
                text=True,
                timeout=60,
            )
            self.assertEqual(fail_ps.returncode, 1, fail_ps.stdout + fail_ps.stderr)
            self.assertIn("VERIFY: FAIL", fail_ps.stdout)

            ok_cmd = subprocess.run(["cmd.exe", "/c", "call", str(scripts / "failure-log.cmd")], env=env, capture_output=True, text=True, timeout=30)
            self.assertEqual(ok_cmd.returncode, 0, ok_cmd.stdout + ok_cmd.stderr)
            log_text = (base / "out" / "failures.log").read_text(encoding="utf-8", errors="replace")
            self.assertIn("fullscreen_color.vs.cso", log_text)


if __name__ == "__main__":
    unittest.main()
