"""Unit and bash CLI safety tests for scripts/linux-boot/{boot-digest.sh,run-boot.sh}.

    python -I tests/test_linux_boot_scripts.py

Standard library only; no game data.
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BOOT_DIR = os.path.join(ROOT, "scripts", "linux-boot")
BOOT_DIGEST = os.path.join(BOOT_DIR, "boot-digest.sh")
RUN_BOOT = os.path.join(BOOT_DIR, "run-boot.sh")


def _find_bash() -> str | None:
    if sys.platform == "win32":
        git_bash = r"C:\Program Files\Git\bin\bash.exe"
        if os.path.isfile(git_bash):
            return git_bash
    return shutil.which("bash")


def _bash_path(path: str) -> str:
    return path.replace("\\", "/")


class LinuxBootScriptsTests(unittest.TestCase):
    def test_scripts_have_lf_line_endings_and_valid_syntax(self):
        for path in (BOOT_DIGEST, RUN_BOOT):
            with open(path, "rb") as f:
                raw = f.read()
            self.assertNotIn(b"\r\n", raw, path)
        bash = _find_bash()
        if not bash:
            self.skipTest("bash not available")
        for path in (BOOT_DIGEST, RUN_BOOT):
            r = subprocess.run([bash, "-n", _bash_path(path)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, f"{path}: {r.stderr}")

    def test_boot_digest_literal_dedup_empty_inputs_and_arg_guards(self):
        bash = _find_bash()
        if not bash:
            self.skipTest("bash not available")
        with tempfile.TemporaryDirectory() as tmp:
            env = dict(os.environ, LEONIDA_BOOT_ROOT=_bash_path(tmp), SERVICE_RESULT="success")

            # Invalid agent/boot inputs must be rejected upfront with exit 2.
            for bad_args in (["../evil", "1"], [".", "1"], ["agent", "abc"], ["agent", "-1"]):
                r = subprocess.run([bash, _bash_path(BOOT_DIGEST), *bad_args],
                                   env=env, capture_output=True, text=True)
                self.assertEqual(r.returncode, 2, (bad_args, r.stdout, r.stderr))

            # Header-only resources.tsv + 0-byte capture index.txt must emit clean placeholders.
            work_axb = os.path.join(tmp, "work", "axb")
            cap_axb = os.path.join(tmp, "capture", "axb-1")
            os.makedirs(work_axb)
            os.makedirs(cap_axb)
            with open(os.path.join(work_axb, "boot-1.resources.tsv"), "w", encoding="utf-8", newline="\n") as f:
                f.write("ts\tavail\tswap\trss\tswap_used\tvram\n")
            with open(os.path.join(cap_axb, "index.txt"), "wb") as f:
                f.write(b"")

            r1 = subprocess.run([bash, _bash_path(BOOT_DIGEST), "axb", "1"],
                                env=env, capture_output=True, text=True)
            self.assertEqual(r1.returncode, 0, r1.stderr)
            self.assertEqual(r1.stderr, "")
            with open(os.path.join(work_axb, "boot-1.digest.txt"), encoding="utf-8") as f:
                digest = f.read()
            self.assertIn("no capture index", digest)
            self.assertIn("no samples", digest)

            # Literal dedup: agent "a.b" must NOT collide with "axb" in boots.tsv, and repeat ("a.b", 1) skips.
            work_dot = os.path.join(tmp, "work", "a.b")
            cap_dot = os.path.join(tmp, "capture", "a.b-1")
            os.makedirs(work_dot)
            os.makedirs(cap_dot)
            with open(os.path.join(work_dot, "boot-1.resources.tsv"), "w", encoding="utf-8", newline="\n") as f:
                f.write("ts\tavail\tswap\trss\tswap_used\tvram\n1\t8000\t0\t1200\t100\t512\n")
            with open(os.path.join(cap_dot, "index.txt"), "w", encoding="utf-8", newline="\n") as f:
                f.write("0\t10\t20\t30\n1\t0\t0\t0\n")

            r2 = subprocess.run([bash, _bash_path(BOOT_DIGEST), "a.b", "1"],
                                env=env, capture_output=True, text=True)
            self.assertEqual(r2.returncode, 0, r2.stderr)
            r3 = subprocess.run([bash, _bash_path(BOOT_DIGEST), "a.b", "1"],
                                env=env, capture_output=True, text=True)
            self.assertEqual(r3.returncode, 0, r3.stderr)

            with open(os.path.join(tmp, "work", "boots.tsv"), encoding="utf-8") as f:
                rows = [ln.rstrip("\n").split("\t") for ln in f]
            self.assertEqual(len(rows), 3, rows)
            self.assertEqual(rows[1][1:3], ["axb", "1"])
            self.assertEqual(rows[1][8:10], ["-", "-"])
            self.assertEqual(rows[2][1:3], ["a.b", "1"])
            self.assertEqual(rows[2][8:10], ["1/2", "1200"])

    def test_run_boot_validates_args_upfront(self):
        bash = _find_bash()
        if not bash:
            self.skipTest("bash not available")
        with tempfile.TemporaryDirectory() as tmp:
            exe = os.path.join(tmp, "gta6rt.exe")
            dll = os.path.join(tmp, "gta6rt_tls_host.dll")
            for p in (exe, dll):
                with open(p, "wb") as f:
                    f.write(b"stub")
            env = dict(os.environ, LEONIDA_BOOT_ROOT=_bash_path(tmp))
            bad_invocations = (
                ["../bad", "1", _bash_path(exe)],
                ["agent", "nonum", _bash_path(exe)],
                ["agent", "1", _bash_path(exe), "0"],
                ["agent", "1", _bash_path(exe), "abc"],
                ["agent", "1", _bash_path(exe), "10", "BADARG"],
                ["agent", "1", _bash_path(exe), "10", "=val"],
                ["agent", "1", _bash_path(exe), "10", "1BAD=val"],
            )
            for args in bad_invocations:
                r = subprocess.run([bash, _bash_path(RUN_BOOT), *args],
                                   env=env, capture_output=True, text=True)
                self.assertEqual(r.returncode, 2, (args, r.stdout, r.stderr))
                self.assertIn("run-boot:", r.stderr)


if __name__ == "__main__":
    unittest.main()
