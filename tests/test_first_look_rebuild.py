"""scripts/first_look.ps1 checks for a current and complete runtime before a dry run.

    python -I tests/test_first_look_rebuild.py

A gta6rt.exe from before a pull still runs, but its dry run reports the old shim table: the day-one import to-do list
would be wrong without any error. The script used to build the loader only when it was missing. No game files.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "first_look.ps1"
PE_INSPECT_STUB = (
    "from pathlib import Path\n"
    "import sys\n"
    "report = Path(sys.argv[-1]) / (Path(sys.argv[1]).name + '.json')\n"
    "report.write_text('{\"imports\":[],\"sections\":[]}', encoding='utf-8', newline='\\n')\n"
)
# The other report tools first_look.ps1 runs (TEST-1421): each records its call in tools_called.txt and writes the file
# its --out / --json names, so a missing tool's exit code doesn't become $LASTEXITCODE before the dry run.
REPORT_TOOL_STUB = (
    "from pathlib import Path\n"
    "import sys\n"
    "args = sys.argv[1:]\n"
    "with Path(__file__).resolve().parents[1].joinpath('tools_called.txt').open('a', encoding='utf-8', newline='\\n') as log:\n"
    "    log.write(Path(__file__).name + '\\n')\n"
    "for flag in ('--json', '--out'):\n"
    "    if flag in args:\n"
    "        target = Path(args[args.index(flag) + 1])\n"
    "        if target.suffix:\n"
    "            target.write_text('{}' if target.suffix == '.json' else '', encoding='utf-8', newline='\\n')\n"
)
REPORT_TOOLS = ("gdk_runtime_calls.py", "find_hash_lookup.py", "exe_checklist.py")
# The build script leaves a marker; the loader is replaced by an alias that echoes its arguments.
BUILD_STUB = r"""
Set-Content -LiteralPath (Join-Path $PSScriptRoot 'built.txt') -Value 'built'
if ($env:LEONIDA_TEST_INCOMPLETE_RUNTIME -ne '1') {
    $build = Join-Path (Split-Path $PSScriptRoot -Parent) 'runtime\build'
    foreach ($name in 'gta6rt.exe', 'gta6rt_tls_host.dll') {
        New-Item -ItemType File -Path (Join-Path $build $name) -Force | Out-Null
    }
}
"""
RUN = r"""
$ErrorActionPreference = 'Stop'
Set-Alias -Name (Join-Path $PWD 'runtime\build\gta6rt.exe') -Value (Join-Path $PWD 'loader.cmd')
& .\scripts\first_look.ps1 -Exe (Join-Path $PWD 'fixture.exe')
"""


@unittest.skipUnless(os.name == "nt", "Windows PowerShell diagnostics script")
class FirstLookRebuildTest(unittest.TestCase):
    def run_case(self, source_newer=False, missing=(), incomplete_build=False, cache=None):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for folder in ("scripts", "tools", "runtime/build", "runtime/src"):
                (root / folder).mkdir(parents=True)
            shutil.copyfile(SCRIPT, root / "scripts" / SCRIPT.name)
            (root / "scripts" / "build_runtime.ps1").write_text(BUILD_STUB, encoding="utf-8")
            (root / "tools" / "pe_inspect.py").write_text(PE_INSPECT_STUB, encoding="utf-8", newline="\n")
            for tool in REPORT_TOOLS:
                (root / "tools" / tool).write_text(REPORT_TOOL_STUB, encoding="utf-8", newline="\n")
            (root / "fixture.exe").write_bytes(b"MZ")
            (root / "loader.cmd").write_text("@echo off\necho %*\nexit /b 0\n", encoding="ascii")
            loader = root / "runtime" / "build" / "gta6rt.exe"
            if cache is not None:
                (loader.parent / "CMakeCache.txt").write_text(cache, encoding="utf-8")
            source = root / "runtime" / "src" / "pe_loader.cpp"
            tls = loader.with_name("gta6rt_tls_host.dll")
            loader.write_bytes(b"")
            tls.touch()
            source.write_text("// source\n", encoding="utf-8")
            now = time.time()
            older, newer = now - 3600, now
            os.utime(loader, (newer, newer) if not source_newer else (older, older))
            os.utime(source, (newer, newer) if source_newer else (older, older))
            for name in missing:
                loader.with_name(name).unlink()
            env = {k: v for k, v in os.environ.items() if k.upper() != "PSMODULEPATH"}
            env["PATH"] = str(Path(sys.executable).parent) + os.pathsep + env["PATH"]
            env["LEONIDA_TEST_INCOMPLETE_RUNTIME"] = "1" if incomplete_build else "0"
            run = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                                  "-Command", RUN], cwd=root, env=env, capture_output=True, text=True, timeout=60)
            dry_run = (root / "reports" / "exe" / "fixture" / "dryrun.txt").is_file()
            called = root / "tools_called.txt"
            self.assertEqual(called.read_text(encoding="utf-8").split() if called.is_file() else [], list(REPORT_TOOLS),
                             run.stdout + run.stderr)
            return (root / "scripts" / "built.txt").is_file(), dry_run, run.returncode, run.stdout + run.stderr

    def test_a_loader_older_than_the_sources_is_rebuilt(self):
        built, dry_run, code, out = self.run_case(source_newer=True)
        self.assertTrue(built, out)
        self.assertTrue(dry_run, out)
        self.assertEqual(code, 0, out)
        self.assertIn("rebuilding it first", out)

    def test_an_up_to_date_loader_is_used_as_it_is(self):
        built, dry_run, code, out = self.run_case()
        self.assertFalse(built, out)
        self.assertTrue(dry_run, out)
        self.assertEqual(code, 0, out)

    def test_a_missing_runtime_file_is_rebuilt(self):
        for name in ("gta6rt.exe", "gta6rt_tls_host.dll"):
            with self.subTest(name=name):
                built, dry_run, code, out = self.run_case(missing=(name,))
                self.assertTrue(built, out)
                self.assertTrue(dry_run, out)
                self.assertEqual(code, 0, out)

    def test_an_incomplete_build_stops_before_the_dry_run(self):
        built, dry_run, code, out = self.run_case(missing=("gta6rt_tls_host.dll",), incomplete_build=True)
        self.assertTrue(built, out)
        self.assertFalse(dry_run, out)
        self.assertNotEqual(code, 0, out)
        self.assertIn("runtime build did not produce", out)
        self.assertIn("gta6rt_tls_host.dll", out)

    def test_incomplete_visual_studio_cache_names_the_missing_command(self):
        generator = "CMAKE_GENERATOR:INTERNAL=Visual Studio 17 2022\n"
        for command in ("", "CMAKE_COMMAND:INTERNAL=\n"):
            with self.subTest(command=command):
                built, dry_run, code, out = self.run_case(source_newer=True, cache=generator + command)
                self.assertNotEqual(code, 0, out)
                self.assertFalse(built, out)
                self.assertIn("CMakeCache.txt", out)
                self.assertIn("Reconfigure", out)
                self.assertIn("CMAKE_COMMAND", out)
                self.assertFalse(dry_run, out)


if __name__ == "__main__":
    unittest.main()
