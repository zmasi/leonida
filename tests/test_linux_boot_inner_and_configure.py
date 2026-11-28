"""Unit tests for scripts/linux-boot/boot-inner.sh and configure-worktree.sh.

    python -I tests/test_linux_boot_inner_and_configure.py
"""
from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BOOT_INNER = ROOT / "scripts" / "linux-boot" / "boot-inner.sh"
CONFIG_WT = ROOT / "scripts" / "linux-boot" / "configure-worktree.sh"


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


class LinuxBootInnerAndConfigureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.bash = _find_bash()

    def _require_bash(self) -> str:
        if not self.bash:
            self.skipTest("bash not available on host")
        return self.bash

    def _run_bash(
        self,
        script: Path,
        args: list[str] | None = None,
        extra_env: dict[str, str] | None = None,
        strip_vars: tuple[str, ...] = (),
    ) -> subprocess.CompletedProcess[str]:
        bash = self._require_bash()
        env = os.environ.copy()
        for key in strip_vars:
            for existing in [k for k in env if k.upper() == key.upper()]:
                env.pop(existing, None)
        if extra_env:
            env.update(extra_env)
        return subprocess.run(
            [bash, _to_bash_path(script), *(args or [])],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            env=env,
            timeout=30,
        )

    def test_lf_line_endings_and_bash_syntax(self) -> None:
        for script in (BOOT_INNER, CONFIG_WT):
            with self.subTest(script=script.name):
                self.assertNotIn(b"\r\n", script.read_bytes())
                bash = self._require_bash()
                res = subprocess.run(
                    [bash, "-n", _to_bash_path(script)],
                    capture_output=True,
                    text=True,
                    encoding="utf-8",
                    timeout=15,
                )
                self.assertEqual(res.returncode, 0, res.stderr)

    def test_boot_inner_rejects_missing_or_invalid_env_vars_before_writing(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            out_dir = root / "opt"
            base = {
                "LEONIDA_BOOT_ROOT": _to_bash_path(out_dir),
                "BOOT_AGENT": "agentA",
                "BOOT_N": "1",
                "BOOT_SECS": "5",
                "GTA6RT": "/fake/gta6rt.exe",
            }
            for missing in ("BOOT_AGENT", "BOOT_N", "BOOT_SECS", "GTA6RT"):
                with self.subTest(missing=missing):
                    env = dict(base)
                    env.pop(missing)
                    res = self._run_bash(
                        BOOT_INNER,
                        extra_env=env,
                        strip_vars=("BOOT_AGENT", "BOOT_N", "BOOT_SECS", "GTA6RT"),
                    )
                    self.assertEqual(res.returncode, 2, res.stderr)
                    self.assertIn("ERROR:", res.stderr)
            for bad_key, bad_val in (("BOOT_AGENT", "../escape"), ("BOOT_SECS", "notanint")):
                with self.subTest(bad_key=bad_key, bad_val=bad_val):
                    env = dict(base)
                    env[bad_key] = bad_val
                    res = self._run_bash(BOOT_INNER, extra_env=env)
                    self.assertEqual(res.returncode, 2, res.stderr)
                    self.assertFalse((out_dir / "work" / "agentA" / "boot-1.meta").exists())

    def test_boot_inner_handles_cgroup_max_and_timeout_exit_124(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            out_dir = root / "opt"
            meminfo = root / "proc" / "meminfo"
            self_cgroup = root / "proc" / "self_cgroup"
            cg_dir = root / "sys_cgroup" / "user.slice" / "unit.scope"
            cg_dir.mkdir(parents=True)
            _write_lf(meminfo, "MemAvailable:    16777216 kB\nSwapTotal:        4194304 kB\nSwapFree:         4194304 kB\n")
            _write_lf(self_cgroup, "12:freezer:/\n0::/user.slice/unit.scope\n")
            _write_lf(cg_dir / "memory.current", "2097152\n")
            _write_lf(cg_dir / "memory.swap.current", "max\n")

            nvidia_smi = root / "bin" / "nvidia-smi"
            _write_lf(nvidia_smi, "#!/bin/bash\nprintf '128, 35\\n'\n", executable=True)

            res_file = out_dir / "work" / "agentA" / "boot-1.resources.tsv"
            boot_sh = out_dir / "boot.sh"
            _write_lf(
                boot_sh,
                "#!/bin/bash\n"
                "i=0\n"
                "while [ \"$i\" -lt 100 ]; do\n"
                "  if [ -f \"$TEST_RES\" ] && [ \"$(wc -l < \"$TEST_RES\")\" -ge 2 ]; then break; fi\n"
                "  i=$((i + 1))\n"
                "  sleep 0.05\n"
                "done\n"
                "echo 'stub boot invoked'\n"
                "exit \"${STUB_BOOT_EXIT:-124}\"\n",
                executable=True,
            )

            env = {
                "LEONIDA_BOOT_ROOT": _to_bash_path(out_dir),
                "LEONIDA_MEMINFO_PATH": _to_bash_path(meminfo),
                "LEONIDA_SELF_CGROUP_PATH": _to_bash_path(self_cgroup),
                "LEONIDA_CGROUP_ROOT": _to_bash_path(root / "sys_cgroup"),
                "NVIDIA_SMI": _to_bash_path(nvidia_smi),
                "BOOT_SH": _to_bash_path(boot_sh),
                "LEONIDA_WAIT_SLEEP_SECS": "0",
                "LEONIDA_SAMPLE_SLEEP_SECS": "1",
                "LEONIDA_WAIT_ITERS": "1",
                "BOOT_AGENT": "agentA",
                "BOOT_N": "1",
                "BOOT_SECS": "3",
                "GTA6RT": "/fake/gta6rt.exe",
                "TEST_RES": _to_bash_path(res_file),
                "STUB_BOOT_EXIT": "124",
            }
            res = self._run_bash(BOOT_INNER, extra_env=env)
            self.assertEqual(res.returncode, 0, res.stderr)

            meta = (out_dir / "work" / "agentA" / "boot-1.meta").read_text(encoding="utf-8")
            self.assertIn("unit=leonida-boot-agentA-1", meta)
            self.assertIn("boot_epoch=", meta)
            log = (out_dir / "work" / "agentA" / "boot-1.log").read_text(encoding="utf-8")
            self.assertIn("stub boot invoked", log)
            self.assertIn("time limit reached", log)
            tsv_lines = res_file.read_text(encoding="utf-8").splitlines()
            self.assertGreaterEqual(len(tsv_lines), 2)
            self.assertTrue(tsv_lines[1].endswith("\t2\t0\t128\t35"), tsv_lines)

            env["BOOT_N"] = "2"
            env["TEST_RES"] = _to_bash_path(out_dir / "work" / "agentA" / "boot-2.resources.tsv")
            env["STUB_BOOT_EXIT"] = "9"
            res_fail = self._run_bash(BOOT_INNER, extra_env=env)
            self.assertEqual(res_fail.returncode, 9, res_fail.stderr)

    def test_configure_worktree_help_validation_and_spaced_paths(self) -> None:
        help_res = self._run_bash(CONFIG_WT, ["--help"])
        self.assertEqual(help_res.returncode, 0, help_res.stderr)
        self.assertIn("usage: configure-worktree.sh", help_res.stdout)

        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            cmake_log = root / "cmake.args"
            fake_cmake = root / "fake-cmake"
            _write_lf(
                fake_cmake,
                "#!/bin/bash\n"
                ": > \"$CMAKE_ARGS_OUT\"\n"
                "for arg in \"$@\"; do printf '%s\\n' \"$arg\" >> \"$CMAKE_ARGS_OUT\"; done\n",
                executable=True,
            )

            no_wt = root / "empty dir"
            no_wt.mkdir()
            for args in ([], [""], [str(root / "missing")], [_to_bash_path(no_wt)]):
                with self.subTest(args=args):
                    bad = self._run_bash(
                        CONFIG_WT,
                        args=args,
                        extra_env={"CMAKE": _to_bash_path(fake_cmake), "CMAKE_ARGS_OUT": _to_bash_path(cmake_log)},
                    )
                    self.assertEqual(bad.returncode, 2, bad.stderr)
                    self.assertFalse(cmake_log.exists())

            wt = root / "my worktree"
            (wt / "runtime" / "cmake").mkdir(parents=True)
            _write_lf(wt / "runtime" / "CMakeLists.txt", "# dummy\n")
            _write_lf(wt / "runtime" / "cmake" / "clang-cl-xwin.cmake", "# dummy\n")
            sdl3 = root / "sdl3 source"
            sdl3.mkdir()

            ok = self._run_bash(
                CONFIG_WT,
                args=[_to_bash_path(wt)],
                extra_env={
                    "CMAKE": _to_bash_path(fake_cmake),
                    "CMAKE_ARGS_OUT": _to_bash_path(cmake_log),
                    "SDL3_SRC": _to_bash_path(sdl3),
                    "XWIN_DIR": "/opt/xwin",
                },
            )
            self.assertEqual(ok.returncode, 0, ok.stderr)
            recorded = cmake_log.read_text(encoding="utf-8").splitlines()
            wt_bash = _to_bash_path(wt)
            sdl_bash = _to_bash_path(sdl3)
            self.assertIn(f"{wt_bash}/runtime", recorded)
            self.assertIn(f"{wt_bash}/runtime/build/boot", recorded)
            self.assertIn(f"-DCMAKE_TOOLCHAIN_FILE={wt_bash}/runtime/cmake/clang-cl-xwin.cmake", recorded)
            self.assertIn(f"-DFETCHCONTENT_SOURCE_DIR_SDL3={sdl_bash}", recorded)


if __name__ == "__main__":
    unittest.main()
