"""scripts/run_game_wine.sh, the Linux/Wine launcher for one game run, checked without the game, Wine or a GPU.

    python -I tests/test_run_game_wine.py

The script must parse (`bash -n`), print its inputs with --help, and stop with a clear message (exit 2) for a missing
or wrong argument, a missing host tool (headless without kwin_wayland names it and says why Xvfb is no substitute) or
a held lock. A whole --visible run is then made against stubs: `wine` and `wineserver` are shell scripts that record
their arguments and environment, `setsid` just runs its command, and the build, executable, package and shader
folders are empty files and folders made by the test. The run must reach the summary with the sound off
(Wine's audio drivers disabled, LEONIDA_AUDIO=silent), the device pinned or a warning printed, the traces on and the
extra gta6rt arguments passed. PATH holds only the stubs and the few real commands the script uses, so the machine's
own Wine and KWin are never started. POSIX only; no game files.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "run_game_wine.sh"
# Real commands the launcher (and its inner half) runs, besides bash builtins and the stubs.
REAL = ("bash", "dirname", "readlink", "mkdir", "rm", "cat", "env", "grep", "sort", "chmod", "date", "sed", "tr", "sleep",
        "pkill", "flock", "timeout", "tee")

WINE_STUB = """#!/bin/sh
if [ "$1" = wineboot ]; then exit 0; fi
for a in "$@"; do printf '%s\\n' "$a"; done > "$STUB_LOG/wine.args"
env | grep -E '^(LEONIDA_|MESA_VK|WINEDLL|WINEPREFIX|PULSE_SERVER|PIPEWIRE_REMOTE|DISPLAY)' | sort > "$STUB_LOG/wine.env"
pwd > "$STUB_LOG/wine.cwd"
echo "[rt] audio: serving the silent output endpoint (LEONIDA_AUDIO=silent): 48 kHz float 7.1" >&2
echo "[d3d12-trace] #1 XboxID3D12CommandQueue::PresentX ok" >&2
exit 0
"""
WINESERVER_STUB = "#!/bin/sh\nexit 0\n"
SETSID_STUB = '#!/bin/sh\nexec "$@"\n'


@unittest.skipIf(os.name == "nt" or not shutil.which("bash"), "a POSIX bash launcher")
class RunGameWineTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)
        self.bin = self.tmp / "bin"
        self.bin.mkdir()
        for name in REAL:
            found = shutil.which(name)
            if not found:
                self.skipTest(f"no {name}")
            (self.bin / name).symlink_to(found)
        (self.bin / "python3").symlink_to(sys.executable)
        self.stub("wine", WINE_STUB)
        self.stub("wineserver", WINESERVER_STUB)
        self.stub("setsid", SETSID_STUB)
        self.log = self.tmp / "log"
        self.log.mkdir()
        # The inputs: empty stand-ins, nothing of the game.
        self.build = self.tmp / "build"
        self.build.mkdir()
        (self.build / "gta6rt.exe").write_bytes(b"")
        (self.build / "gta6rt_tls_host.dll").write_bytes(b"")
        self.exe = self.tmp / "title.exe"
        self.exe.write_bytes(b"")
        self.game = self.tmp / "package"
        self.game.mkdir()
        self.shaders = self.tmp / "spirv"
        self.shaders.mkdir()
        self.out = self.tmp / "run"
        self.lock = self.tmp / "run.lock"

    def stub(self, name, text):
        path = self.bin / name
        path.write_text(text, encoding="utf-8", newline="\n")
        path.chmod(0o755)

    def inputs(self, **drop):
        args = {"--build": self.build, "--exe": self.exe, "--game-dir": self.game, "--shaders": self.shaders,
                "--out": self.out, "--lock": self.lock}
        flat = []
        for flag, value in args.items():
            if flag not in drop.values():
                flat += [flag, str(value)]
        return flat

    def run_script(self, *args, display=":99"):
        env = {"PATH": str(self.bin), "HOME": str(self.tmp), "STUB_LOG": str(self.log)}
        if display:
            env["DISPLAY"] = display
        return subprocess.run([str(self.bin / "bash"), str(SCRIPT), *args], cwd=self.tmp, env=env, capture_output=True, text=True,
                              timeout=120)

    def test_the_script_parses_is_executable_and_lf(self):
        p = subprocess.run(["bash", "-n", str(SCRIPT)], capture_output=True, text=True, timeout=60)
        self.assertEqual(p.returncode, 0, p.stderr)
        data = SCRIPT.read_bytes()
        self.assertTrue(os.access(SCRIPT, os.X_OK), "scripts/run_game_wine.sh lost its executable bit")
        self.assertNotIn(b"\r", data)
        self.assertTrue(data.startswith(b"#!/usr/bin/env bash\n"))

    def test_help_lists_the_inputs(self):
        p = self.run_script("--help")
        self.assertEqual(p.returncode, 0, p.stderr)
        for word in ("--build", "--exe", "--game-dir", "--shaders", "--out", "--seconds", "--visible", "--device",
                     "--screenshot-every", "LEONIDA_SHADERS", "LEONIDA_GAME_EXE", "LEONIDA_AUDIO=silent",
                     "kwin_wayland", "DRI3"):
            self.assertIn(word, p.stdout)

    def test_a_missing_input_is_named(self):
        for flag, word in (("--build", "--build is required"), ("--exe", "--exe (or LEONIDA_GAME_EXE)"),
                           ("--game-dir", "--game-dir (or LEONIDA_GAME_DIR)"), ("--out", "--out is required"),
                           ("--shaders", "--shaders (or LEONIDA_SHADERS)")):
            with self.subTest(flag=flag):
                p = self.run_script(*self.inputs(drop=flag), "--visible")
                self.assertEqual(p.returncode, 2, p.stderr)
                self.assertIn(word, p.stderr)

    def test_wrong_arguments_are_refused(self):
        cases = (
            (["--seconds", "ten"], "--seconds must be a whole number"),
            (["--seconds", "0"], "--seconds must be a whole number"),
            (["--seconds", "08"], "--seconds must be a whole number"),
            (["--device", "radeon"], "--device takes VID:DID"),
            (["--screenshot-every", "x"], "--screenshot-every must be"),
            (["--exe", str(self.tmp / "nothing.exe")], "no game executable at"),
            (["--game-dir", str(self.tmp / "nothing")], "no package folder at"),
            (["--shaders", str(self.tmp / "nothing")], "no shader library at"),
            (["--build", str(self.game)], "neither a build folder with gta6rt.exe nor a checkout"),
            (["--bogus"], "unknown argument: --bogus"),
            (["--out"], "--out needs a value"),
        )
        for extra, word in cases:
            with self.subTest(extra=extra):
                p = self.run_script(*self.inputs(), "--visible", *extra)
                self.assertEqual(p.returncode, 2, p.stderr)
                self.assertIn(word, p.stderr)
        self.assertFalse((self.log / "wine.args").exists())

    def test_headless_without_kwin_names_what_is_missing(self):
        p = self.run_script(*self.inputs())
        self.assertEqual(p.returncode, 2, p.stderr)
        self.assertIn("missing: kwin_wayland dbus-run-session Xwayland", p.stderr)
        self.assertIn("Xvfb has no DRI3", p.stderr)

    def test_visible_needs_a_display(self):
        p = self.run_script(*self.inputs(), "--visible", display=None)
        self.assertEqual(p.returncode, 2, p.stderr)
        self.assertIn("--visible needs DISPLAY", p.stderr)

    def test_a_held_lock_stops_a_second_run(self):
        # POSIX only, like the launcher: the class-level skip above keeps this off Windows, where `fcntl` does not exist
        # and a module-level import of it made the file fail to load before the skip could run.
        import fcntl

        with open(self.lock, "w", encoding="utf-8", newline="\n") as held:
            fcntl.flock(held, fcntl.LOCK_EX)
            p = self.run_script(*self.inputs(), "--visible")
        self.assertEqual(p.returncode, 2, p.stderr)
        self.assertIn("another run holds", p.stderr)
        self.assertFalse((self.log / "wine.args").exists())

    def test_a_visible_run_against_stubs(self):
        p = self.run_script(*self.inputs(), "--visible", "--device", "1002:744C", "--seconds", "30", "--",
                            "--extra", "a b")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertNotIn("Mesa picks", p.stderr)
        args = (self.log / "wine.args").read_text(encoding="utf-8").splitlines()
        self.assertEqual(args[0], str(self.build / "gta6rt.exe"))
        self.assertEqual(args[1:5], ["--run", f"Z:{self.exe}", "--game-dir", f"Z:{self.game}"])
        self.assertIn("--trace-d3d12", args)
        self.assertEqual(args[-2:], ["--extra", "a b"])
        env = dict(line.split("=", 1) for line in (self.log / "wine.env").read_text(encoding="utf-8").splitlines())
        self.assertEqual(env["LEONIDA_AUDIO"], "silent")
        self.assertEqual(env["MESA_VK_DEVICE_SELECT"], "1002:744C!")
        self.assertEqual(env["LEONIDA_SHADERS"], f"Z:{self.shaders}")
        self.assertEqual(env["LEONIDA_D3D12"], "vulkan")
        self.assertEqual(env["PULSE_SERVER"], "unix:/nonexistent")
        self.assertEqual(env["WINEPREFIX"], str(self.out / "prefix"))
        self.assertEqual(env["DISPLAY"], ":99")
        for driver in ("winepulse.drv=d", "winealsa.drv=d", "wineoss.drv=d", "winecoreaudio.drv=d"):
            self.assertIn(driver, env["WINEDLLOVERRIDES"])
        self.assertNotIn("LEONIDA_GAME_DIR", env)
        self.assertEqual((self.log / "wine.cwd").read_text(encoding="utf-8").strip(), str(self.game))
        self.assertIn("end: process exit 0", p.stdout)
        self.assertIn("silent audio endpoint: served (1 lines)", p.stdout)
        self.assertIn("presents: 1", p.stdout)
        self.assertTrue((self.out / "summary.txt").is_file())
        self.assertFalse((self.out / "xdg").exists(), "the private XDG_RUNTIME_DIR outlived the run")
        # The same folder again is refused: the trace would not be overwritten.
        p = self.run_script(*self.inputs(), "--visible")
        self.assertEqual(p.returncode, 2)
        self.assertIn("already holds a run", p.stderr)

    def test_without_a_device_it_warns(self):
        p = self.run_script(*self.inputs(), "--visible", "--seconds", "30")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn("warning: no --device: Mesa picks the Vulkan device", p.stderr)
        env = (self.log / "wine.env").read_text(encoding="utf-8")
        self.assertNotIn("MESA_VK_DEVICE_SELECT", env)


if __name__ == "__main__":
    unittest.main()
