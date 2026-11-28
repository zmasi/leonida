"""tools/conformance/native/scripts/build.ps1 finds the CMake that Visual Studio includes and names no Visual Studio
version. Before, it took CMake from PATH or C:\\Program Files\\CMake only, and always asked for Visual Studio 2022.

Nothing is built: a copy of the script runs with stand-ins for cmake that record their arguments. Windows only."""
import base64
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "conformance" / "native" / "scripts" / "build.ps1"
BUNDLED = Path("Common7", "IDE", "CommonExtensions", "Microsoft", "CMake", "CMake", "bin", "cmake.exe")


@unittest.skipUnless(os.name == "nt" and shutil.which("powershell.exe"), "requires Windows PowerShell")
class BuildScriptTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.base = Path(tmp.name).resolve()
        scripts = self.base / "a b" / "native" / "scripts"
        scripts.mkdir(parents=True)
        self.script = Path(shutil.copy(SCRIPT, scripts))
        # No vswhere.exe and no CMake here, so a test sees only the stand-ins it adds.
        self.programs = self.base / "programs"
        self.programs.mkdir()

    def environment(self, path, **extra):
        env = {k: v for k, v in os.environ.items() if k.upper() not in ("PATH", "CMAKE_GENERATOR")}
        # Upper case, as os.environ gives them: another spelling would be a second variable of the same name. A 64-bit
        # process gets its ProgramFiles from ProgramW6432.
        for name in ("PROGRAMFILES", "PROGRAMFILES(X86)", "PROGRAMW6432"):
            env[name] = str(self.programs)
        env.update(extra)
        env["PATH"] = os.pathsep.join(map(str, path))
        return env

    def powershell(self, arguments, path, **extra):
        exe = shutil.which("powershell.exe")
        result = subprocess.run([exe, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", *arguments],
                                env=self.environment(path, **extra), capture_output=True, text=True,
                                errors="replace", timeout=60)
        return result.returncode, result.stdout + result.stderr

    def run_script(self, path, **extra):
        return self.powershell(["-File", str(self.script)], path, **extra)

    def cmake(self, folder, generators="  Visual Studio 18 2026"):
        """A cmake.cmd that answers --help, and records every other call and the CMAKE_GENERATOR it sees."""
        path = self.base / folder / "cmake.cmd"
        path.parent.mkdir()
        # The log goes next to the stand-in (%~dp0), so the batch file holds no path and stays ASCII.
        path.write_text("\r\n".join((
            "@echo off",
            f'if "%~1"=="--help" (echo {generators}& exit /b 0)',
            '>>"%~dp0calls.txt" echo [%CMAKE_GENERATOR%] %*',
            "exit /b 0", "")), encoding="ascii", newline="")
        return path

    def calls(self, cmake):
        """The recorded calls; cmd writes them in the console code page, so only their ASCII parts are compared."""
        return cmake.with_name("calls.txt").read_text(encoding="ascii", errors="replace").splitlines()

    def find(self, visual_studio, path):
        text = SCRIPT.read_text(encoding="ascii")
        function = re.search(r"^function Find-CMake.*?^\}", text, re.M | re.S).group(0)
        # Base64, because the console code page may not have every character of the path.
        code = function + ("\n$found = [Text.Encoding]::UTF8.GetBytes((Find-CMake $env:TEST_VS))"
                           "\nWrite-Output ('FOUND=' + [Convert]::ToBase64String($found))")
        status, output = self.powershell(["-Command", code], path, TEST_VS=str(visual_studio))
        self.assertEqual(status, 0, output)
        return base64.b64decode(re.search(r"FOUND=(\S*)", output).group(1)).decode("utf-8")

    def test_visual_studio_cmake_comes_first(self):
        on_path = self.cmake("path")
        bundled = self.base / "VS [1]" / BUNDLED
        bundled.parent.mkdir(parents=True)
        bundled.write_bytes(b"")
        self.assertEqual(self.find(self.base / "VS [1]", [on_path.parent]), str(bundled))
        self.assertEqual(self.find(self.base / "VS [1]", []), str(bundled))

    def test_path_cmake_without_visual_studio_generators_is_skipped(self):
        msys = self.cmake("msys", generators="  Unix Makefiles")
        native = self.cmake("path")
        self.assertEqual(self.find("", [msys.parent, native.parent]), str(native))
        self.assertEqual(self.find("", [msys.parent]), "")

    def test_default_install_folder_is_the_last_resort(self):
        installed = self.programs / "CMake" / "bin" / "cmake.exe"
        installed.parent.mkdir(parents=True)
        installed.write_bytes(b"")
        self.assertEqual(self.find("", []), str(installed))
        on_path = self.cmake("path")
        self.assertEqual(self.find("", [on_path.parent]), str(on_path))

    def test_no_cmake_is_one_message_and_exit_1(self):
        status, output = self.run_script([])
        self.assertEqual(status, 1, output)
        self.assertEqual(output.strip().splitlines(),
                         ["CMake was not found: install Visual Studio's C++ CMake tools or CMake"])

    def test_configure_names_no_visual_studio_version(self):
        cmake = self.cmake("path")
        status, output = self.run_script([cmake.parent], CMAKE_GENERATOR="Ninja")
        self.assertEqual(status, 0, output)
        self.assertIn("BUILD EXIT 0", output)
        configure, build = self.calls(cmake)
        self.assertRegex(configure, r'^\[\] -S "[^"]*\\a b\\native" -B "[^"]*\\a b\\native\\build" -A x64$')
        self.assertRegex(build, r'^\[Ninja\] --build "[^"]*\\a b\\native\\build" --config Release --parallel$')


if __name__ == "__main__":
    unittest.main()
