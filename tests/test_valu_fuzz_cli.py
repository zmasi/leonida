"""Stdlib CLI, oracle-lifetime, and UTF-8 subprocess tests for tools/valu_fuzz.py.

    python -I tests/test_valu_fuzz_cli.py

Standard library only; no game data.
"""
from __future__ import annotations

import io
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

TOOLS_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools")
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

import valu_fuzz as vf  # noqa: E402


class ValuFuzzCliTests(unittest.TestCase):
    def test_invalid_show_rejected_before_open_oracle(self):
        from hw_oracle import runner

        for bad_show in ("bad", "1:-1", "1:0:16"):
            err = io.StringIO()
            with mock.patch.object(runner, "open_oracle", side_effect=AssertionError("open_oracle should not run")), \
                    redirect_stderr(err), self.assertRaises(SystemExit) as ctx:
                vf.main(["--family", "f16", "--oracle", "hw", "--show", bad_show])
            self.assertEqual(ctx.exception.code, 2)
            self.assertIn("--show expects", err.getvalue())

    def test_oracle_always_closed_when_show_raises(self):
        from hw_oracle import asm, runner

        closed = []
        fake_oracle = SimpleNamespace(
            arch="gfx1100",
            device="fake-gpu",
            close=lambda: closed.append(True),
        )
        out = io.StringIO()
        with mock.patch.object(runner, "open_oracle", return_value=fake_oracle), \
                mock.patch.object(asm, "family", return_value="gfx11"), \
                mock.patch.object(vf, "show", side_effect=RuntimeError("boom")), \
                redirect_stdout(out):
            with self.assertRaisesRegex(RuntimeError, "boom"):
                vf.main(["--family", "f16", "--oracle", "hw", "--show", "1:0:64"])
        self.assertEqual(closed, [True])

    def test_keep_and_negative_control_cli_validation(self):
        with tempfile.TemporaryDirectory() as tmp:
            blocker = os.path.join(tmp, "not_a_dir")
            with open(blocker, "wb") as f:
                f.write(b"x")
            bad_cases = (
                ["--family", "f16", "--keep", tmp],
                ["--family", "f16", "--oracle", "hw", "--model-only", "--keep", tmp],
                ["--family", "f16", "--oracle", "hw", "--model-only", "--negative-control"],
                ["--family", "f16", "--oracle", "hw", "--keep", ""],
                ["--family", "f16", "--oracle", "hw", "--keep", blocker],
            )
            for argv in bad_cases:
                err = io.StringIO()
                with redirect_stderr(err), self.assertRaises(SystemExit) as ctx:
                    vf.main(argv)
                self.assertEqual(ctx.exception.code, 2, argv)

    def test_translate_and_run_gpu_encoding_and_bom_index(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp) / "work"
            work.mkdir()
            progs = vf.programs("f16", 1, 1, (64,))
            seen_runs = []

            def fake_subproc(cmd, **kwargs):
                seen_runs.append(kwargs)
                spirv_dir = work / "spirv"
                spirv_dir.mkdir(parents=True, exist_ok=True)
                with open(spirv_dir / "index.json", "wb") as f:
                    f.write(b'\xef\xbb\xbf{"ok": true}\n')
                return SimpleNamespace(returncode=0, stdout="", stderr="")

            with mock.patch.object(vf.mc_command, "mc_command", return_value=["llvm-mc"]), \
                    mock.patch.object(vf.mc_command, "run", return_value=SimpleNamespace(returncode=0, stdout="", stderr="")), \
                    mock.patch.object(vf, "compute_header", return_value=b"\x00" * 16), \
                    mock.patch.object(vf.subprocess, "run", side_effect=fake_subproc):
                idx, log = vf.translate(progs, work, emulate=False)
            self.assertEqual(idx, {"ok": True})
            self.assertEqual(log, "")
            self.assertEqual(seen_runs[-1]["encoding"], "utf-8")
            self.assertEqual(seen_runs[-1]["errors"], "replace")

            fake_exe = Path(tmp) / ("vk_compute_test.exe" if os.name == "nt" else "vk_compute_test")
            fake_exe.write_bytes(b"")
            with mock.patch.object(vf.subprocess, "run", return_value=SimpleNamespace(returncode=0, stdout="ok", stderr="")) as mrun:
                rc, msg = vf.run_gpu(progs[0], Path(tmp) / "p.spv", tmp, {}, False, Path(tmp) / "d.bin")
            self.assertEqual((rc, msg), (0, "ok"))
            self.assertEqual(mrun.call_args.kwargs["encoding"], "utf-8")
            self.assertEqual(mrun.call_args.kwargs["errors"], "replace")

    def test_model_only_runs_cleanly(self):
        out = io.StringIO()
        with redirect_stdout(out):
            rc = vf.main(["--family", "f16", "--programs", "2", "--model-only"])
        self.assertEqual(rc, 0)
        self.assertIn("f16 programs generated and modelled", out.getvalue())


if __name__ == "__main__":
    unittest.main()
