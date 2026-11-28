"""LLVM_MC is one executable path or a whole command line, in every tool that reads it.

Windows runs llvm-mc through WSL (the official Windows LLVM build has no AMDGPU target):

    set LLVM_MC=wsl -d Ubuntu -e /usr/lib/llvm-21/bin/llvm-mc

    python -I tests/test_mc_command.py
"""
import json
import os
import shlex
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import mc_command  # noqa: E402


class SplitCommandTest(unittest.TestCase):
    def test_bare_name(self):
        self.assertEqual(mc_command.split_command("llvm-mc"), ["llvm-mc"])

    def test_empty(self):
        self.assertEqual(mc_command.split_command(""), [])
        self.assertEqual(mc_command.split_command("   "), [])

    def test_command_line(self):
        self.assertEqual(mc_command.split_command("wsl -d Ubuntu -e /usr/lib/llvm-21/bin/llvm-mc"),
                         ["wsl", "-d", "Ubuntu", "-e", "/usr/lib/llvm-21/bin/llvm-mc"])

    def test_quoted_arguments(self):
        if os.name == "nt":
            raw = '"C:\\Program Files\\LLVM\\bin\\llvm-mc.exe" --version'
            want = ["C:\\Program Files\\LLVM\\bin\\llvm-mc.exe", "--version"]
        else:
            raw = '"/opt/llvm 21/bin/llvm-mc" --version'
            want = ["/opt/llvm 21/bin/llvm-mc", "--version"]
        self.assertEqual(mc_command.split_command(raw), want)

    def test_windows_backslashes_and_literal_quotes_round_trip(self):
        argv = [r"C:\Program Files\LLVM\bin\llvm-mc.exe", "", 'label="wide value"',
                "C:\\output folder\\", r"plain\path", 'a\\"b']
        with mock.patch.object(mc_command.os, "name", "nt"):
            self.assertEqual(mc_command.split_command(subprocess.list2cmdline(argv)), argv)
            self.assertEqual(mc_command.split_command('llvm-mc "a""b"'), ["llvm-mc", 'a"b'])

    def test_posix_keeps_quotes_that_are_part_of_an_argument(self):
        argv = ["llvm-mc", '"literal quotes"', "", '--label="wide value"']
        with mock.patch.object(mc_command.os, "name", "posix"):
            self.assertEqual(mc_command.split_command(shlex.join(argv)), argv)

    @unittest.skipUnless(os.name == "nt", "Windows command line and subprocess")
    def test_windows_subprocess_receives_literal_quotes_and_a_trailing_backslash(self):
        want = ['"keep these quotes"', "C:\\output folder\\", ""]
        command = [sys.executable, "-c", "import json, sys; print(json.dumps(sys.argv[1:]))", *want]
        args = mc_command.split_command(subprocess.list2cmdline(command))
        result = mc_command.run(args, capture_output=True, check=True, text=True)
        self.assertEqual(json.loads(result.stdout), want)

    def test_windows_quoted_option_value_keeps_spaces_and_backslashes(self) -> None:
        executable = r"C:\Program Files\LLVM\bin\llvm-mc.exe"
        output = r"C:\shader builds\listing.s"
        raw = f'"{executable}" -o="{output}" -show-encoding'
        with mock.patch.object(mc_command.os, "name", "nt"):
            self.assertEqual(mc_command.split_command(raw), [executable, f"-o={output}", "-show-encoding"])

    def test_path_with_spaces_is_kept_whole(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "llvm mc")
            with open(path, "w", encoding="utf-8") as f:
                f.write("")
            self.assertEqual(mc_command.split_command(path), [path])
            self.assertEqual(mc_command.split_command(f'"{path}"'), [path])

    def test_path_with_spaces_and_no_extension_is_kept_whole(self):
        # C:\Program Files\LLVM\bin\llvm-mc names llvm-mc.exe on Windows. It is not a file under that exact
        # name, so it was split into C:\Program and Files\LLVM\bin\llvm-mc.
        raw = "C:\\Program Files\\LLVM\\bin\\llvm-mc"
        with mock.patch.object(mc_command.os, "name", "nt"), \
                mock.patch.object(mc_command.shutil, "which", return_value=raw + ".EXE") as which:
            self.assertEqual(mc_command.split_command(raw), [raw])
            which.assert_called_once_with(raw)

    @unittest.skipUnless(os.name == "nt", "Windows resolves a missing .exe")
    def test_real_path_with_spaces_and_no_extension(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = os.path.join(tmp, "llvm 21")
            os.mkdir(folder)
            with open(os.path.join(folder, "llvm-mc.exe"), "w", encoding="utf-8") as f:
                f.write("")
            path = os.path.join(folder, "llvm-mc")
            self.assertEqual(mc_command.split_command(path), [path])

    def test_command_line_is_still_split_when_its_first_word_is_on_path(self):
        # Only a value with a directory part is tried as one path: "wsl -d Ubuntu ..." stays a command line
        # whatever is on PATH, and a bare name is not looked up at all.
        with mock.patch.object(mc_command.shutil, "which", return_value="/usr/bin/x") as which:
            self.assertEqual(mc_command.split_command("wsl -d Ubuntu -e llvm-mc"),
                             ["wsl", "-d", "Ubuntu", "-e", "llvm-mc"])
            self.assertEqual(mc_command.split_command("llvm-mc"), ["llvm-mc"])
            which.assert_not_called()

    def test_missing_path_with_spaces_is_split_as_before(self):
        with mock.patch.object(mc_command.os, "name", "nt"), \
                mock.patch.object(mc_command.shutil, "which", return_value=None):
            self.assertEqual(mc_command.split_command('"C:\\no such\\llvm-mc" --version'),
                             ["C:\\no such\\llvm-mc", "--version"])

    def test_unbalanced_quote_is_one_clear_error(self):
        raw = '"unterminated --version'
        for platform in ("nt", "posix"):
            with self.subTest(platform=platform), mock.patch.object(mc_command.os, "name", platform):
                with self.assertRaises(SystemExit) as ctx:
                    mc_command.split_command(raw)
                self.assertIn("LLVM_MC", str(ctx.exception))
                self.assertIn("unterminated", str(ctx.exception))

    def test_mc_command_reads_the_environment(self):
        with mock.patch.dict(os.environ, {"LLVM_MC": "wsl -d Ubuntu -e /opt/llvm-21/bin/llvm-mc"}):
            self.assertEqual(mc_command.mc_command(),
                             ["wsl", "-d", "Ubuntu", "-e", "/opt/llvm-21/bin/llvm-mc"])

    def test_mc_command_falls_back_to_the_path_name(self):
        with mock.patch.dict(os.environ, {"LLVM_MC": ""}), \
             mock.patch.object(mc_command.shutil, "which", return_value="/usr/bin/llvm-mc"):
            self.assertEqual(mc_command.mc_command(), ["llvm-mc"])

    def test_mc_command_without_any_llvm_mc_keeps_the_plain_name(self):
        with mock.patch.dict(os.environ, {"LLVM_MC": ""}), \
             mock.patch.object(mc_command.shutil, "which", return_value=None), \
             mock.patch.object(mc_command, "versioned_llvm_mc", return_value=None):
            self.assertEqual(mc_command.mc_command(), ["llvm-mc"])


def _executable(path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("#!/bin/sh\n")
    os.chmod(path, 0o755)
    return path


@unittest.skipIf(os.name == "nt", "versioned LLVM installs are a Linux distribution layout")
class VersionedLlvmMcTest(unittest.TestCase):
    """Debian and Ubuntu's llvm-<N> packages install llvm-mc-<N> and /usr/lib/llvm-<N>/bin/llvm-mc, not llvm-mc."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.bin = os.path.join(self.tmp.name, "bin")
        self.lib = os.path.join(self.tmp.name, "lib")
        os.makedirs(self.bin)
        os.makedirs(self.lib)

    def found(self):
        pattern = os.path.join(self.lib, "llvm-*", "bin", "llvm-mc")
        with mock.patch.dict(os.environ, {"PATH": self.bin, "LLVM_MC": ""}), \
             mock.patch.object(mc_command, "DEBIAN_LLVM_MC", pattern):
            return mc_command.versioned_llvm_mc(), mc_command.mc_command()

    def test_nothing_installed(self):
        self.assertEqual(self.found(), (None, ["llvm-mc"]))

    def test_the_highest_version_wins_by_number_not_by_name(self):
        _executable(os.path.join(self.bin, "llvm-mc-9"))
        want = _executable(os.path.join(self.bin, "llvm-mc-21"))
        _executable(os.path.join(self.lib, "llvm-19", "bin", "llvm-mc"))
        self.assertEqual(self.found(), (want, [want]))

    def test_the_debian_library_folder_counts(self):
        want = _executable(os.path.join(self.lib, "llvm-22", "bin", "llvm-mc"))
        _executable(os.path.join(self.bin, "llvm-mc-21"))
        self.assertEqual(self.found(), (want, [want]))

    def test_other_tools_and_non_executables_are_not_llvm_mc(self):
        _executable(os.path.join(self.bin, "llvm-mca-23"))        # the machine-code analyser, not llvm-mc
        _executable(os.path.join(self.bin, "llvm-mc-23.bak"))
        with open(os.path.join(self.bin, "llvm-mc-24"), "w", encoding="utf-8", newline="\n") as f:
            f.write("not executable\n")
        self.assertEqual(self.found(), (None, ["llvm-mc"]))

    def test_the_plain_name_on_path_is_kept(self):
        # A work folder records the command that disassembled it; one built with llvm-mc on PATH must keep that.
        _executable(os.path.join(self.bin, "llvm-mc"))
        _executable(os.path.join(self.bin, "llvm-mc-21"))
        self.assertEqual(self.found()[1], ["llvm-mc"])

    def test_llvm_mc_set_wins(self):
        _executable(os.path.join(self.bin, "llvm-mc-21"))
        with mock.patch.dict(os.environ, {"PATH": self.bin, "LLVM_MC": "/opt/llvm/bin/llvm-mc --flag"}):
            self.assertEqual(mc_command.mc_command(), ["/opt/llvm/bin/llvm-mc", "--flag"])

    def test_the_message_names_the_distribution_package_off_windows(self):
        with mock.patch.object(mc_command.sys, "platform", "linux"):
            message = mc_command.not_found("llvm-mc", FileNotFoundError(2, "No such file"))
        self.assertIn("apt install llvm-21", message)
        self.assertNotIn("build_llvm_mc.ps1", message)


    def test_no_tool_pins_the_plain_name_as_its_default(self):
        # run_gpu_tests.py once defaulted --llvm-mc to os.environ.get("LLVM_MC", "llvm-mc"), so with only
        # Ubuntu's llvm-21 installed it never reached versioned_llvm_mc() and failed with the very message that
        # says /usr/lib/llvm-21/bin/llvm-mc is found without further setup.
        import re
        pinned = re.compile(r"""get\(\s*["']LLVM_MC["']\s*,\s*["']llvm-mc["']\s*\)""")
        found = []
        for folder in ("scripts", "tools", os.path.join("runtime", "test", "shaders")):
            for name in sorted(os.listdir(os.path.join(ROOT, folder))):
                if name.endswith(".py"):
                    with open(os.path.join(ROOT, folder, name), encoding="utf-8") as f:
                        if pinned.search(f.read()):
                            found.append(f"{folder}/{name}")
        self.assertEqual(found, [], "use mc_command.mc_command() (or an empty LLVM_MC) instead of a literal llvm-mc")

class MissingLlvmMcTest(unittest.TestCase):
    """An llvm-mc that can't be started ends each tool with one line, not a FileNotFoundError traceback."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.missing = os.path.join(self.tmp.name, "no-llvm-mc.exe")

    def tearDown(self):
        self.tmp.cleanup()

    def test_run_names_the_command_and_how_to_set_it(self):
        with self.assertRaises(SystemExit) as cm:
            mc_command.run([self.missing, "-show-encoding"], input="", capture_output=True, text=True)
        message = str(cm.exception.code)
        self.assertIn(f"'{self.missing}' could not be started", message)
        self.assertIn("LLVM_MC", message)
        # The way to get one is the OS's own: the Windows build script there, a distribution package elsewhere.
        self.assertIn(mc_command.install_hint(), message)
        if os.name == "nt":
            self.assertIn("build_llvm_mc.ps1", message)
        else:
            self.assertNotIn("build_llvm_mc.ps1", message)
        self.assertNotIn("--llvm-mc", message)   # only a tool that has the option names it

    def test_windows_keeps_the_build_script_and_wsl_advice(self):
        with mock.patch.object(mc_command.os, "name", "nt"):
            hint = mc_command.install_hint()
        self.assertIn("build_llvm_mc.ps1", hint)
        self.assertIn("LLVM_MC=wsl -d Ubuntu -e /usr/lib/llvm-21/bin/llvm-mc", hint)

    def test_run_names_the_tools_own_option(self):
        with self.assertRaises(SystemExit) as cm:
            mc_command.run([self.missing], flag="--llvm-mc", input="", capture_output=True, text=True)
        self.assertIn("set LLVM_MC (or pass --llvm-mc)", str(cm.exception.code))

    def test_run_passes_a_command_that_starts_through(self):
        r = mc_command.run([sys.executable, "-c", "import sys; print(sys.stdin.read()); sys.exit(3)"],
                           input="hi", capture_output=True, text=True)
        self.assertEqual((r.returncode, r.stdout.strip()), (3, "hi"))

    def test_every_tool_that_assembles_gives_one_line(self):
        build = os.path.join(self.tmp.name, "build")
        os.mkdir(build)
        for name in ("vk_compute_test", "vk_store_test", "vk_harness"):   # run_gpu_tests.py only checks they exist
            open(os.path.join(build, name + (".exe" if os.name == "nt" else "")), "wb").close()
        tools = {
            "run_gpu_tests.py": [os.path.join(ROOT, "scripts", "run_gpu_tests.py"), "--build", build],
            "valu_fuzz.py": [os.path.join(ROOT, "tools", "valu_fuzz.py"), "--family", "f16", "--programs", "1",
                             "--build", build],
            "lds_fuzz.py": [os.path.join(ROOT, "tools", "lds_fuzz.py"), "--programs", "1", "--build", build],
            "crosslane_fuzz.py": [os.path.join(ROOT, "tools", "crosslane_fuzz.py"), "--programs", "1", "--exe",
                                  os.path.join(build, "vk_compute_test" + (".exe" if os.name == "nt" else ""))],
            "image_fuzz.py": [os.path.join(ROOT, "tools", "image_fuzz.py"), "--programs", "1", "--build", build],
            "build.py": [os.path.join(ROOT, "runtime", "test", "shaders", "build.py"),
                         os.path.join(self.tmp.name, "spirv"), "cs_store"],
        }
        env = dict(os.environ, LLVM_MC=self.missing)
        for name, argv in tools.items():
            with self.subTest(tool=name):
                p = subprocess.run([sys.executable, "-I", *argv], capture_output=True, text=True,
                                   errors="replace", env=env, cwd=self.tmp.name, timeout=300)
                self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
                self.assertNotIn("Traceback", p.stderr)
                self.assertIn("could not be started", p.stderr)
                # Only run_gpu_tests.py has an --llvm-mc option; the others read LLVM_MC alone.
                self.assertEqual("--llvm-mc" in p.stderr, name == "run_gpu_tests.py", p.stderr)


if __name__ == "__main__":
    unittest.main()
