"""Unit tests for tools/hw_oracle/runner.py."""

from __future__ import annotations

import os
import sys
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest import mock

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

from hw_oracle import runner  # noqa: E402


class TestRunnerToolDiscovery(unittest.TestCase):
    def test_is_msvc_recognizes_cl_variants(self) -> None:
        self.assertTrue(runner._is_msvc(r"C:\VS\cl.exe"))
        self.assertTrue(runner._is_msvc("clang-cl"))
        self.assertFalse(runner._is_msvc("/usr/bin/gcc"))

    def test_find_clang_env_missing_raises_unavailable(self) -> None:
        with mock.patch.dict(os.environ, {"LEONIDA_AMD_CLANG": "/nonexistent/clang"}, clear=False):
            with self.assertRaises(runner.Unavailable):
                runner.find_clang()

    def test_find_clang_env_existing_returns_path(self) -> None:
        with TemporaryDirectory() as tmp:
            fake_clang = Path(tmp) / "clang"
            fake_clang.write_text("", encoding="utf-8")
            with mock.patch.dict(os.environ, {"LEONIDA_AMD_CLANG": str(fake_clang)}, clear=False):
                self.assertEqual(runner.find_clang(), str(fake_clang))

    def test_find_hip_dll_env_missing_raises_unavailable(self) -> None:
        with mock.patch.dict(os.environ, {"LEONIDA_HIP_DLL": "/nonexistent/amdhip64_7.dll"}, clear=False):
            with self.assertRaises(runner.Unavailable):
                runner.find_hip_dll()

    def test_detect_arch_uses_env_override_and_strips_xnack(self) -> None:
        with mock.patch.dict(os.environ, {"LEONIDA_HW_ARCH": "gfx1100"}, clear=False):
            self.assertEqual(runner.detect_arch("/unused/clang"), "gfx1100")

        with TemporaryDirectory() as tmp:
            clang = Path(tmp) / runner._exe("clang")
            arch_tool = Path(tmp) / runner._exe("amdgpu-arch")
            clang.write_text("", encoding="utf-8")
            arch_tool.write_text("", encoding="utf-8")
            env = {k: v for k, v in os.environ.items() if k != "LEONIDA_HW_ARCH"}
            proc = mock.Mock(stdout="gfx1010:xnack-\n")
            with mock.patch.dict(os.environ, env, clear=True), mock.patch.object(
                runner.subprocess, "run", return_value=proc
            ):
                self.assertEqual(runner.detect_arch(str(clang)), "gfx1010")

    def test_build_command_gcc_and_msvc(self) -> None:
        self.assertEqual(
            runner.build_command("gcc", "gcc", "hw_oracle.c", "out", "/tmp"),
            ["gcc", "-O1", "hw_oracle.c", "-o", "out"],
        )
        msvc_cmd = runner.build_command("msvc", "cl.exe", "hw_oracle.c", "out.exe", "/tmp")
        self.assertEqual(msvc_cmd[:4], ["cl.exe", "/nologo", "/O1", "hw_oracle.c"])
        self.assertIn("/Feout.exe", msvc_cmd)

    def test_find_lld_missing_raises_unavailable(self) -> None:
        with TemporaryDirectory() as tmp:
            fake_clang = Path(tmp) / runner._exe("clang")
            fake_clang.write_text("", encoding="utf-8")
            with mock.patch.object(runner.shutil, "which", return_value=None):
                with self.assertRaises(runner.Unavailable):
                    runner._find_lld(str(fake_clang))


if __name__ == "__main__":
    unittest.main()
