"""Cross-platform safety tests for scripts/run_game_wine.sh.

    python -I tests/test_run_game_wine_safety.py
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "run_game_wine.sh"


def _find_bash() -> str | None:
    if os.name == "nt":
        for root in (
            os.environ.get("ProgramFiles", r"C:\Program Files"),
            os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
        ):
            if not root:
                continue
            for rel in (r"Git\bin\bash.exe", r"Git\usr\bin\bash.exe"):
                cand = Path(root) / rel
                if cand.is_file():
                    return str(cand)
        return None
    return shutil.which("bash")


def _to_bash_path(path: Path) -> str:
    s = str(path.resolve())
    if os.name == "nt" and len(s) >= 2 and s[1] == ":":
        return "/" + s[0].lower() + s[2:].replace("\\", "/")
    return s.replace("\\", "/")


def _write_lf(path: Path, text: str, executable: bool = False) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(text.encode("utf-8"))
    if executable:
        path.chmod(0o755)


class RunGameWineSafetyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.bash = _find_bash()

    def _require_bash(self) -> str:
        if not self.bash:
            self.skipTest("bash not available on host")
        return self.bash

    def _run_bash(
        self,
        args: list[str],
        extra_env: dict[str, str] | None = None,
        path_prefix: Path | None = None,
    ) -> subprocess.CompletedProcess[str]:
        bash = self._require_bash()
        env = os.environ.copy()
        if extra_env:
            env.update(extra_env)
        if path_prefix is not None:
            prefix = _to_bash_path(path_prefix) if os.name == "nt" else str(path_prefix)
            bash_dir = str(Path(bash).parent)
            if os.name == "nt":
                env["PATH"] = str(path_prefix) + os.pathsep + bash_dir + os.pathsep + env.get("PATH", "")
                env["LEONIDA_TEST_PREPEND_PATH"] = prefix
            else:
                env["PATH"] = str(path_prefix) + os.pathsep + env.get("PATH", "")
        return subprocess.run(
            [bash, _to_bash_path(SCRIPT), *args],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            env=env,
            timeout=30,
        )

    def test_script_is_lf_and_passes_bash_syntax(self) -> None:
        raw = SCRIPT.read_bytes()
        self.assertNotIn(b"\r\n", raw)
        self.assertTrue(raw.startswith(b"#!/usr/bin/env bash\n"))
        bash = self._require_bash()
        res = subprocess.run(
            [bash, "-n", _to_bash_path(SCRIPT)],
            capture_output=True,
            text=True,
            encoding="utf-8",
            timeout=15,
        )
        self.assertEqual(res.returncode, 0, res.stderr)

    def test_validation_rejects_bad_inner_args_leading_zeros_and_missing_build(self) -> None:
        r_inner = self._run_bash(["--inner", "/tmp/only_one"])
        self.assertEqual(r_inner.returncode, 2)
        self.assertIn("--inner requires", r_inner.stderr)

        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            exe = root / "game.exe"
            exe.write_bytes(b"")
            game = root / "pkg"
            game.mkdir()
            build = root / "build"
            build.mkdir()
            (build / "gta6rt.exe").write_bytes(b"")
            for bad_shot in ("00", "05", "abc"):
                with self.subTest(bad_shot=bad_shot):
                    r = self._run_bash([
                        "--build", _to_bash_path(build),
                        "--exe", _to_bash_path(exe),
                        "--game-dir", _to_bash_path(game),
                        "--shaders", "none",
                        "--out", _to_bash_path(root / "out"),
                        "--screenshot-every", bad_shot,
                    ])
                    self.assertEqual(r.returncode, 2)
                    self.assertIn("--screenshot-every must be a whole number", r.stderr)

            r_missing_src = self._run_bash([
                "--build", _to_bash_path(root / "no_such_build"),
                "--exe", _to_bash_path(exe),
                "--game-dir", _to_bash_path(game),
                "--shaders", "none",
                "--out", _to_bash_path(root / "out"),
            ])
            self.assertEqual(r_missing_src.returncode, 2)
            self.assertIn("no build path at", r_missing_src.stderr)

    def test_uppercase_device_inner_bash_lock_parent_and_summary_exit_propagation(self) -> None:
        bash = self._require_bash()
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            sbin = root / "bin"
            sbin.mkdir()
            _write_lf(sbin / "wine", "#!/usr/bin/env bash\nexit 0\n", executable=True)
            _write_lf(sbin / "wineserver", "#!/usr/bin/env bash\nexit 0\n", executable=True)
            _write_lf(sbin / "setsid", "#!/usr/bin/env bash\nexec \"$@\"\n", executable=True)
            _write_lf(sbin / "flock", "#!/usr/bin/env bash\nexit 0\n", executable=True)
            _write_lf(sbin / "pkill", "#!/usr/bin/env bash\nexit 0\n", executable=True)
            _write_lf(sbin / "timeout", "#!/usr/bin/env bash\nshift 3\n\"$@\"\n", executable=True)
            _write_lf(
                sbin / "python3",
                "#!/usr/bin/env bash\n"
                "echo \"summary ok\"\n"
                "exit \"${STUB_SUMMARY_EXIT:-0}\"\n",
                executable=True,
            )

            build = root / "build"
            build.mkdir()
            (build / "gta6rt.exe").write_bytes(b"")
            (build / "gta6rt_tls_host.dll").write_bytes(b"")
            exe = root / "title.exe"
            exe.write_bytes(b"")
            game = root / "pkg"
            game.mkdir()
            sysfs = root / "sysfs"
            dev_dir = sysfs / "renderD129" / "device"
            dev_dir.mkdir(parents=True)
            _write_lf(dev_dir / "vendor", "0x1002\n")
            _write_lf(dev_dir / "device", "0x744c\n")

            lock = root / "nested_locks" / "sub" / "run.lock"
            out1 = root / "run1"
            base_args = [
                "--build", _to_bash_path(build),
                "--exe", _to_bash_path(exe),
                "--game-dir", _to_bash_path(game),
                "--shaders", "none",
                "--visible",
                "--device", "1002:744C",
                "--seconds", "5",
                "--lock", _to_bash_path(lock),
            ]
            # Wrap via bash -c so our stub bin sits before /usr/bin on Git Bash too.
            sbin_bash = _to_bash_path(sbin)
            wrapper_args = [
                "-c",
                f'export PATH="{sbin_bash}:$PATH"; exec "{_to_bash_path(SCRIPT)}" "$@"',
                "run_game_wine_wrapper",
                *base_args,
                "--out",
                _to_bash_path(out1),
            ]
            env = os.environ.copy()
            env.update({
                "DISPLAY": ":99",
                "HOME": _to_bash_path(root),
                "LEONIDA_DRM_SYSFS_DIR": _to_bash_path(sysfs),
                "STUB_SUMMARY_EXIT": "0",
            })
            r_ok = subprocess.run(
                [bash, *wrapper_args],
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                env=env,
                timeout=30,
            )
            self.assertEqual(r_ok.returncode, 0, r_ok.stdout + r_ok.stderr)
            self.assertTrue(lock.parent.is_dir())
            env_txt = (out1 / "env.txt").read_text(encoding="utf-8")
            self.assertIn("KWIN_DRM_DEVICES=/dev/dri/renderD129", env_txt)
            self.assertIn("MESA_VK_DEVICE_SELECT=1002:744C!", env_txt)
            inner_sh = (out1 / "inner.sh").read_text(encoding="utf-8")
            self.assertTrue(inner_sh.startswith("#!/usr/bin/env bash\n"))

            out2 = root / "run2"
            env["STUB_SUMMARY_EXIT"] = "4"
            wrapper_args[-1] = _to_bash_path(out2)
            r_fail = subprocess.run(
                [bash, *wrapper_args],
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                env=env,
                timeout=30,
            )
            self.assertEqual(r_fail.returncode, 4, r_fail.stdout + r_fail.stderr)


if __name__ == "__main__":
    unittest.main()
