"""Build.sh and Mods.sh, the Linux and macOS counterparts of Build.bat and Mods.bat, start the right Python.

    python -I tests/test_shell_launchers.py

Each launcher is run with a PATH that holds only stub interpreters: shell scripts that answer the version check with
the version they are given and otherwise record their arguments. The launcher must pick the newest usable versioned
name or fall back to python3 or python, run the build or mod manager with -I and every argument unchanged,
pass the exit code back, and stop with a message when there is none. POSIX only (/bin/sh); no Python is started
for real, no compiler and no game files.
"""
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LAUNCHERS = {"Build.sh": ["make_game.py"], "Mods.sh": ["tools/leonida_mods.py", "gui"]}

STUB = """#!/bin/sh
if [ "$1" = -I ] && [ "$2" = -c ]; then
    exit {ok}
fi
for a in "$@"; do printf '%s\\n' "$a"; done > "$LEONIDA_STUB_LOG.{name}"
exit {status}
"""


@unittest.skipIf(os.name == "nt" or not os.path.exists("/bin/sh"), "POSIX shell launchers")
class ShellLauncherTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)
        self.bin = self.tmp / "python bin"
        self.bin.mkdir()
        self.log = self.tmp / "log"
        # PATH holds the stubs and the one external command the launchers run, dirname (cd, pwd, command -v and
        # printf are shell builtins), so the machine's own pythons in /usr/bin can't be picked instead.
        tools = self.tmp / "tools"
        tools.mkdir()
        dirname = shutil.which("dirname")
        if not dirname:
            self.skipTest("no dirname")
        (tools / "dirname").symlink_to(dirname)
        self.path = os.pathsep.join([str(self.bin), str(tools)])

    def stub(self, name, ok=True, status=0, directory=None):
        path = (directory or self.bin) / name
        path.write_text(STUB.format(ok=0 if ok else 1, name=name, status=status), encoding="utf-8", newline="\n")
        path.chmod(0o755)

    def run_launcher(self, launcher, *args, cwd=None):
        env = {"PATH": self.path, "LEONIDA_STUB_LOG": str(self.log), "HOME": str(self.tmp)}
        return subprocess.run(["/bin/sh", str(ROOT / launcher), *args], cwd=cwd or self.tmp, env=env,
                              capture_output=True, text=True, timeout=60)

    def recorded(self, name):
        return (self.tmp / f"log.{name}").read_text(encoding="utf-8").splitlines()

    def test_the_first_new_enough_python_runs_the_tool_with_every_argument(self):
        for launcher, tool in LAUNCHERS.items():
            with self.subTest(launcher=launcher):
                for name in ("python3.14", "python3.13", "python3.12", "python3", "python"):
                    (self.bin / name).unlink(missing_ok=True)
                self.stub("python3.13", ok=False)          # unusable: skipped
                self.stub("python3.12")
                self.stub("python3")                        # also new enough, but later in the order
                args = ["--out", "/mnt/a b/Leonida", "", "it's", "$HOME", "*"]
                p = self.run_launcher(launcher, *args)
                self.assertEqual(p.returncode, 0, p.stderr)
                want = ["-I", str(ROOT / tool[0]), *tool[1:], *args]
                self.assertEqual(self.recorded("python3.12"), want)
                self.assertFalse((self.tmp / "log.python3").exists())

    def test_discovers_newer_versioned_names_and_preserves_path_precedence(self):
        other = self.tmp / "second bin"
        other.mkdir()
        self.path += os.pathsep + str(other)
        self.stub("python3", ok=False)
        self.stub("python3.15")
        self.stub("python3.20")
        self.stub("python3.20", status=9, directory=other)
        self.stub("python3.99-config", status=8)
        for launcher, tool in LAUNCHERS.items():
            with self.subTest(launcher=launcher):
                p = self.run_launcher(launcher, "--help")
                self.assertEqual(p.returncode, 0, p.stderr)
                self.assertEqual(self.recorded("python3.20"), ["-I", str(ROOT / tool[0]), *tool[1:], "--help"])
                self.assertFalse((self.tmp / "log.python3.15").exists())

    def test_python3_is_used_when_there_is_no_versioned_name(self):
        self.stub("python3")
        p = self.run_launcher("Build.sh", "--check")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(self.recorded("python3"), ["-I", str(ROOT / "make_game.py"), "--check"])

    def test_the_exit_code_comes_back(self):
        self.stub("python3", status=7)
        self.assertEqual(self.run_launcher("Build.sh").returncode, 7)

    def test_started_from_another_folder(self):
        self.stub("python3")
        elsewhere = self.tmp / "elsewhere"
        elsewhere.mkdir()
        p = self.run_launcher("Build.sh", "--find", cwd=elsewhere)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(self.recorded("python3")[1], str(ROOT / "make_game.py"))

    def test_without_a_new_enough_python_it_says_so(self):
        self.stub("python3", ok=False)
        for launcher in LAUNCHERS:
            with self.subTest(launcher=launcher):
                p = self.run_launcher(launcher)
                self.assertEqual(p.returncode, 1)
                self.assertIn("Python 3.12 or newer is needed", p.stderr)
                self.assertFalse((self.tmp / "log.python3").exists())

    def test_the_launchers_are_executable_and_lf(self):
        for launcher in LAUNCHERS:
            with self.subTest(launcher=launcher):
                path = ROOT / launcher
                self.assertTrue(os.access(path, os.X_OK), f"{launcher} lost its executable bit")
                self.assertNotIn(b"\r", path.read_bytes())
                self.assertTrue(path.read_bytes().startswith(b"#!/bin/sh\n"))


if __name__ == "__main__":
    unittest.main()
