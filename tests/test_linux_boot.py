"""Unit tests for scripts/linux-boot/boot.sh using stubbed commands (no root, Wine, or game data).

    python -I tests/test_linux_boot.py
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BOOT_SH = ROOT / "scripts" / "linux-boot" / "boot.sh"


def _find_bash() -> str | None:
    if os.name == "nt":
        for candidate in (
            r"C:\Program Files\Git\bin\bash.exe",
            r"C:\Program Files (x86)\Git\bin\bash.exe",
        ):
            if os.path.isfile(candidate):
                return candidate
        git = shutil.which("git")
        if git:
            root = Path(git).resolve().parent.parent
            for rel in ("bin/bash.exe", "usr/bin/bash.exe"):
                p = root / rel
                if p.is_file():
                    return str(p)
        return None
    return shutil.which("bash")


def _to_bash_path(path: Path) -> str:
    text = str(path).replace("\\", "/")
    if os.name == "nt":
        text = re.sub(r"^([A-Za-z]):", lambda m: "/" + m.group(1).lower(), text)
    return text


def _write_exec(path: Path, body: str) -> None:
    path.write_text(body, encoding="utf-8", newline="\n")
    path.chmod(0o755)


class LinuxBootScriptTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.bash = _find_bash()
        if not cls.bash:
            raise unittest.SkipTest("Git Bash / POSIX bash unavailable")

    def test_lf_endings_and_bash_syntax_ok(self) -> None:
        raw = BOOT_SH.read_bytes()
        self.assertNotIn(b"\r\n", raw)
        proc = subprocess.run([self.bash, "-n", _to_bash_path(BOOT_SH)], capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_help_and_invalid_arguments_fail_before_creating_dirs(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            out_dir = Path(td) / "out"
            env = dict(os.environ, LEONIDA_OUT=_to_bash_path(out_dir))

            help_run = subprocess.run(
                [self.bash, _to_bash_path(BOOT_SH), "--help"],
                capture_output=True, text=True, env=env, timeout=15,
            )
            self.assertEqual(help_run.returncode, 0, help_run.stderr)
            self.assertIn("dry-run|init|run", help_run.stdout)
            self.assertFalse(out_dir.exists())

            for argv in ([], ["bogus"], ["run", "0"], ["run", "-5"], ["run", "abc"], ["dry-run", "5", "extra"]):
                with self.subTest(argv=argv):
                    bad = subprocess.run(
                        [self.bash, _to_bash_path(BOOT_SH), *argv],
                        capture_output=True, text=True, env=env, timeout=15,
                    )
                    self.assertEqual(bad.returncode, 2)
                    self.assertFalse(out_dir.exists())

    def test_missing_gta6rt_fails_before_creating_dirs(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            out_dir = root / "out"
            repo_dir = root / "repo"
            repo_dir.mkdir()
            env = dict(
                os.environ,
                LEONIDA_OUT=_to_bash_path(out_dir),
                LEONIDA_REPO=_to_bash_path(repo_dir),
                GTA6RT=_to_bash_path(root / "no_such_gta6rt.exe"),
            )
            p = subprocess.run(
                [self.bash, _to_bash_path(BOOT_SH), "dry-run"],
                capture_output=True, text=True, env=env, timeout=15,
            )
            self.assertEqual(p.returncode, 2)
            self.assertIn("missing gta6rt", p.stderr)
            self.assertFalse(out_dir.exists())

    def test_dry_run_and_run_forward_args_timeout_and_exit_code(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            out_dir = root / "out"
            repo_dir = root / "repo"
            (repo_dir / "tools").mkdir(parents=True)
            _write_exec(repo_dir / "tools" / "boot_trace.py", "# stub\n")
            gta6rt = root / "gta6rt.exe"
            gta6rt.write_bytes(b"")
            bin_dir = root / "bin"
            bin_dir.mkdir()
            calls = root / "calls.log"
            py_calls = root / "py.log"

            _write_exec(bin_dir / "timeout", f'#!/usr/bin/env bash\nprintf "timeout:%s\\n" "$*" >> "{_to_bash_path(calls)}"\nwhile [[ "$1" == -* ]]; do shift; done\nshift\nexec "$@"\n')
            _write_exec(bin_dir / "unshare", f'#!/usr/bin/env bash\nprintf "unshare:%s\\n" "$*" >> "{_to_bash_path(calls)}"\nif [ "$1" = "-rn" ]; then shift; fi\nexec "$@"\n')
            _write_exec(bin_dir / "wine", f'#!/usr/bin/env bash\nprintf "wine:%s\\n" "$*" >> "{_to_bash_path(calls)}"\nexit "${{MOCK_RC:-0}}"\n')
            _write_exec(bin_dir / "python3", f'#!/usr/bin/env bash\nprintf "py:%s\\n" "$*" >> "{_to_bash_path(py_calls)}"\n')

            env = dict(
                os.environ,
                PATH=f"{bin_dir}{os.pathsep}{os.environ.get('PATH', '')}",
                LEONIDA_OUT=_to_bash_path(out_dir),
                LEONIDA_REPO=_to_bash_path(repo_dir),
                LEONIDA_GAME_DIR="/mock/game",
                GTA6RT=_to_bash_path(gta6rt),
                WINE=_to_bash_path(bin_dir / "wine"),
                PYTHON3=_to_bash_path(bin_dir / "python3"),
                UNSHARE=_to_bash_path(bin_dir / "unshare"),
                TIMEOUT=_to_bash_path(bin_dir / "timeout"),
            )

            dry = subprocess.run(
                [self.bash, _to_bash_path(BOOT_SH), "dry-run"],
                capture_output=True, text=True, env=env, timeout=15,
            )
            self.assertEqual(dry.returncode, 0, dry.stdout + dry.stderr)
            dry_calls = calls.read_text(encoding="utf-8")
            self.assertNotIn("timeout:", dry_calls)
            self.assertNotIn("--trace-d3d12", dry_calls)
            self.assertIn("--dry-run /mock/game/game_scarlett_master.exe", dry_calls)

            calls.unlink()
            env_run = dict(env, MOCK_RC="7")
            run = subprocess.run(
                [self.bash, _to_bash_path(BOOT_SH), "run", "42"],
                capture_output=True, text=True, env=env_run, timeout=15,
            )
            self.assertEqual(run.returncode, 7)
            run_calls = calls.read_text(encoding="utf-8")
            self.assertIn("timeout:--signal=INT --kill-after=20 42 ", run_calls)
            self.assertIn("--trace-d3d12", run_calls)
            self.assertIn("-I", py_calls.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
