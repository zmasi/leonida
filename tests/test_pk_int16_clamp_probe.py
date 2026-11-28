"""Unit and oracle-backed tests for tools/pk_int16_clamp_probe.py. Standard library only; no game files."""
from contextlib import redirect_stderr, redirect_stdout
import io
import json
import os
import sys
import tempfile
import unittest
import unittest.mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")
sys.dont_write_bytecode = True
for p in (TOOLS, os.path.join(TOOLS, "hw_oracle")):
    if p not in sys.path:
        sys.path.insert(0, p)

import pk_int16_clamp_probe as probe  # noqa: E402
from hw_oracle import runner as hw_runner  # noqa: E402


def _mock_oracle(results, *, arch="gfx1036", device="Mock GFX10"):
    oracle = unittest.mock.MagicMock()
    oracle.arch = arch
    oracle.device = device
    oracle.run.return_value = results
    return oracle


def _expected_results_for(ops, waves):
    out = []
    for _wave in waves:
        for op in ops:
            for clamp in probe.CLAMP_MODES:
                out.append([
                    probe.expected_pk_int16(op, c.src0, c.src1, sel=c.sel, selhi=c.selhi, clamp=clamp)
                    for c in probe.CASES
                ])
    return out


class PkInt16ClampProbeTests(unittest.TestCase):
    def test_self_test(self):
        out = io.StringIO()
        with redirect_stdout(out):
            rc = probe.main(["--self-test"])
        self.assertEqual(rc, 0)
        self.assertIn("self-test ok", out.getvalue())

    def test_model_distinguishes_wrap_from_saturation(self):
        # Every target op must agree between plain and clamp, including wrapping mul_lo overflows.
        overflow_seen = False
        for op in probe.TARGET_OPS:
            for c in probe.CASES:
                plain = probe.expected_pk_int16(op, c.src0, c.src1, sel=c.sel, selhi=c.selhi, clamp=False)
                clamped = probe.expected_pk_int16(op, c.src0, c.src1, sel=c.sel, selhi=c.selhi, clamp=True)
                self.assertEqual(plain, clamped, f"{op} {c.name}")
                if op == "v_pk_mul_lo_u16" and c.name == "pow2_overflow_wrap":
                    self.assertEqual(clamped, 0x00000000)
                    overflow_seen = True
        self.assertTrue(overflow_seen)

        # Control op (v_pk_add_u16) must differ on overflowing cases.
        ctrl_diffs = sum(
            probe.expected_pk_int16("v_pk_add_u16", c.src0, c.src1, sel=c.sel, selhi=c.selhi, clamp=False)
            != probe.expected_pk_int16("v_pk_add_u16", c.src0, c.src1, sel=c.sel, selhi=c.selhi, clamp=True)
            for c in probe.CASES
        )
        self.assertGreater(ctrl_diffs, 0)

    def test_kernel_lines_and_build_jobs(self):
        kernels, meta = probe.build_jobs(["v_pk_mul_lo_u16", "v_pk_max_i16"], [32, 64])
        self.assertEqual(len(kernels), 8)
        self.assertEqual(meta[0], (32, "v_pk_mul_lo_u16", False))
        self.assertEqual(meta[-1], (64, "v_pk_max_i16", True))
        lines = probe.kernel_lines("v_pk_mul_lo_u16", True)
        self.assertIn("v_pk_mul_lo_u16 v2, v0, 0x02000400 clamp", lines)
        self.assertIn("v_pk_mul_lo_u16 v2, -16, v1 op_sel:[1,0] op_sel_hi:[0,1] clamp", lines)
        self.assertIn("v_pk_mul_lo_u16 v2, s4, v1 op_sel:[0,1] op_sel_hi:[1,0] clamp", lines)
        self.assertIn("v_pk_mul_lo_u16 v2, v2, v1 clamp", lines)
        self.assertIn("v_pk_mul_lo_u16 v2, v0, v2 clamp", lines)
        self.assertEqual(lines[-2:], ["s_waitcnt vmcnt(0)", "s_endpgm"])

    def test_mocked_oracle_success_and_json_lf(self):
        results = _expected_results_for(probe.OPS, [32, 64])
        oracle = _mock_oracle(results)
        with tempfile.TemporaryDirectory() as td:
            jpath = os.path.join(td, "sub", "report.json")
            out = io.StringIO()
            with unittest.mock.patch.object(hw_runner, "open_oracle", return_value=oracle):
                with redirect_stdout(out):
                    rc = probe.main(["--json", jpath])
            self.assertEqual(rc, 0)
            self.assertIn("mismatches=0, wave_diffs=0, target_clamp_diffs=0", out.getvalue())
            oracle.close.assert_called_once()
            with open(jpath, "rb") as f:
                raw = f.read()
            self.assertNotIn(b"\r\n", raw)
            doc = json.loads(raw.decode("utf-8"))
            self.assertEqual(doc["total_cases"], 2 * len(probe.OPS) * 2 * len(probe.CASES))
            self.assertEqual(doc["mismatches"], 0)
            self.assertEqual(doc["target_clamp_diffs"], 0)
            self.assertGreater(doc["control_clamp_diffs"], 0)

    def test_mocked_oracle_mismatch_control_and_harness_error(self):
        # Flip one wave64 output to catch both model mismatch and wave_diffs.
        results = _expected_results_for(["v_pk_mul_lo_u16"], [32, 64])
        results[-1][0] ^= 0xFFFF
        oracle = _mock_oracle(results)
        with unittest.mock.patch.object(hw_runner, "open_oracle", return_value=oracle):
            with redirect_stdout(io.StringIO()):
                rc = probe.main(["--ops", "v_pk_mul_lo_u16"])
        self.assertEqual(rc, 1)

        # Control op returning plain words for clamp should fail control check.
        ctrl_results = _expected_results_for(["v_pk_add_u16"], [32])
        ctrl_results[1] = list(ctrl_results[0])
        oracle_ctrl = _mock_oracle(ctrl_results)
        with unittest.mock.patch.object(hw_runner, "open_oracle", return_value=oracle_ctrl):
            with redirect_stdout(io.StringIO()):
                rc_ctrl = probe.main(["--ops", "v_pk_add_u16", "--waves", "32"])
        self.assertEqual(rc_ctrl, 1)

        # Harness string error returns exit 2.
        oracle_err = _mock_oracle(["compile failed", [0] * len(probe.CASES)])
        with unittest.mock.patch.object(hw_runner, "open_oracle", return_value=oracle_err):
            with redirect_stdout(io.StringIO()):
                rc_err = probe.main(["--ops", "v_pk_mul_lo_u16", "--waves", "32"])
        self.assertEqual(rc_err, 2)

    def test_unavailable_non_gfx10_and_cli_errors(self):
        with unittest.mock.patch.object(hw_runner, "open_oracle", side_effect=hw_runner.Unavailable("no gpu")):
            with redirect_stdout(io.StringIO()):
                self.assertEqual(probe.main([]), 77)

        non_gfx10 = _mock_oracle([], arch="gfx1100", device="RDNA3")
        with unittest.mock.patch.object(hw_runner, "open_oracle", return_value=non_gfx10):
            with redirect_stdout(io.StringIO()):
                self.assertEqual(probe.main([]), 77)
        non_gfx10.close.assert_called_once()

        with tempfile.TemporaryDirectory() as td:
            with self.assertRaises(SystemExit) as cm:
                with unittest.mock.patch.object(sys, "stderr", io.StringIO()):
                    probe.main(["--json", td])
            self.assertEqual(cm.exception.code, 2)

        with self.assertRaises(SystemExit) as cm2:
            with unittest.mock.patch.object(sys, "stderr", io.StringIO()):
                probe.main(["--ops", "v_not_an_op"])
        self.assertEqual(cm2.exception.code, 2)

    def test_malformed_and_duplicate_ops_waves_and_empty_args(self):
        for bad_args in (
            ["--ops", "v_pk_mul_lo_u16,"],
            ["--ops", ",v_pk_mul_lo_u16"],
            ["--ops", "v_pk_mul_lo_u16,,v_pk_add_u16"],
            ["--waves", "32,"],
            ["--waves", "32,,64"],
            ["--hw-arch", ""],
            ["--hw-arch", "   "],
            ["--json", ""],
            ["--json", "   "],
        ):
            with self.subTest(args=bad_args):
                with unittest.mock.patch.object(hw_runner, "open_oracle") as open_mock, redirect_stderr(io.StringIO()):
                    with self.assertRaises(SystemExit) as cm:
                        probe.main(bad_args)
                self.assertEqual(cm.exception.code, 2)
                open_mock.assert_not_called()

        results = _expected_results_for(["v_pk_mul_lo_u16"], [32])
        oracle = _mock_oracle(results)
        with tempfile.TemporaryDirectory() as td:
            jpath = os.path.join(td, "dedup.json")
            with unittest.mock.patch.object(hw_runner, "open_oracle", return_value=oracle), redirect_stdout(io.StringIO()):
                rc = probe.main([
                    "--ops", "v_pk_mul_lo_u16,v_pk_mul_lo_u16",
                    "--waves", "32,32",
                    "--json", jpath,
                ])
            self.assertEqual(rc, 0)
            with open(jpath, encoding="utf-8") as f:
                doc = json.load(f)
            self.assertEqual(doc["ops"], ["v_pk_mul_lo_u16"])
            self.assertEqual(doc["waves"], [32])
            self.assertEqual(doc["total_cases"], 2 * len(probe.CASES))

    def test_json_blocker_trailing_sep_and_atomic_write_report(self):
        with tempfile.TemporaryDirectory() as td:
            blocker = os.path.join(td, "blocker")
            with open(blocker, "wb") as f:
                f.write(b"keep")
            with unittest.mock.patch.object(hw_runner, "open_oracle") as open_mock, redirect_stderr(io.StringIO()) as err:
                rc = probe.main(["--ops", "v_pk_mul_lo_u16", "--waves", "32", "--json", os.path.join(blocker, "r.json")])
            self.assertEqual(rc, 2)
            open_mock.assert_not_called()
            self.assertIn("ERROR: cannot write", err.getvalue())
            with open(blocker, "rb") as f:
                self.assertEqual(f.read(), b"keep")

            with unittest.mock.patch.object(hw_runner, "open_oracle") as open_mock, redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as cm:
                    probe.main(["--ops", "v_pk_mul_lo_u16", "--waves", "32", "--json", os.path.join(td, "sub") + os.sep])
            self.assertEqual(cm.exception.code, 2)
            open_mock.assert_not_called()

            out = os.path.join(td, "sub", "report.json")
            self.assertTrue(probe.write_report(out, {"ok": 1}))
            with open(out, "rb") as f:
                raw = f.read()
            self.assertTrue(raw.endswith(b"\n"))
            self.assertNotIn(b"\r\n", raw)

            with redirect_stderr(io.StringIO()):
                self.assertFalse(probe.write_report(out, {"bad": float("nan")}))
            with open(out, encoding="utf-8") as f:
                self.assertEqual(json.load(f), {"ok": 1})

            if hasattr(os, "symlink"):
                real = os.path.join(td, "real.json")
                link = os.path.join(td, "link.json")
                with open(real, "w", encoding="utf-8", newline="\n") as f:
                    f.write("old")
                try:
                    os.symlink(real, link)
                except OSError:
                    link = None
                if link is not None:
                    self.assertTrue(probe.write_report(link, {"via": "link"}))
                    self.assertTrue(os.path.islink(link))
                    with open(real, encoding="utf-8") as f:
                        self.assertEqual(json.load(f), {"via": "link"})

            if os.name == "posix" and os.path.exists("/dev/null"):
                self.assertTrue(probe.write_report("/dev/null", {"ok": 1}))

    def test_evaluate_results_count_mismatch_and_unknown_arch_family(self):
        _, meta = probe.build_jobs(["v_pk_mul_lo_u16"], [32])
        rep = probe.evaluate_results(["v_pk_mul_lo_u16"], [32], meta, [])
        self.assertEqual(len(rep["harness_errors"]), 1)
        self.assertIn("expected", rep["harness_errors"][0]["error"])

        bad_arch = _mock_oracle([], arch="unknown_arch_999", device="Unknown")
        with unittest.mock.patch.object(hw_runner, "open_oracle", return_value=bad_arch), redirect_stdout(io.StringIO()):
            self.assertEqual(probe.main(["--ops", "v_pk_mul_lo_u16", "--waves", "32"]), 77)
        bad_arch.close.assert_called_once()

    def test_live_hw_oracle_when_configured(self):
        try:
            hw_runner.find_clang()
        except hw_runner.Unavailable as e:
            self.skipTest(str(e))
        arch = "gfx1036" if os.environ.get("LEONIDA_AMD_CLANG") else None
        out = io.StringIO()
        argv = ["--waves", "32,64"]
        if arch:
            argv.extend(["--hw-arch", arch])
        with redirect_stdout(out):
            rc = probe.main(argv)
        if rc == 77:
            self.skipTest(out.getvalue().strip() or "GFX10 oracle unavailable")
        self.assertEqual(rc, 0, out.getvalue())


if __name__ == "__main__":
    unittest.main()
