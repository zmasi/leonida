"""Unit tests for tools/frexp_ldexp_f16_mods_probe.py (reference model, kernel assembly, FakeOracle verdicts, CLI).

Standard library only; no GPU, no LLVM required (the clang assembly test skips when AMD clang is unavailable),
and no game files.
"""
import io
import json
import os
import re
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.dont_write_bytecode = True
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import frexp_ldexp_f16_mods_probe as pr  # noqa: E402
from hw_oracle import asm, runner  # noqa: E402


class FakeOracle:
    """Simulated hw_oracle backend that parses raw GFX10 probe kernels and emits expected RDNA2 words."""

    arch = "gfx1036"
    device = "fake gfx1036"

    def __init__(
        self,
        *,
        bad_grid: bool = False,
        bad_flush: bool = False,
        bad_omod: bool = False,
        wave64_diff: bool = False,
        error_msg: str | None = None,
    ):
        self.bad_grid = bad_grid
        self.bad_flush = bad_flush
        self.bad_omod = bad_omod
        self.wave64_diff = wave64_diff
        self.error_msg = error_msg
        self.closed = False

    def run(self, kernels):
        out = []
        flush_map = {
            (c.op, c.h, c.n, c.mod): (c.init_dst & 0xFFFF0000) | c.want_lo
            for c in pr.FLUSH_CONTROL_CASES
        }
        for k in kernels:
            if self.error_msg is not None:
                out.append(self.error_msg)
                continue
            mode_val = pr.MODE_CONSOLE
            for ln in k.lines:
                m_mode = re.match(r"s_setreg_imm32_b32 hwreg\(HW_REG_MODE, 0, 10\), (0x[0-9a-fA-F]+)", ln)
                if m_mode:
                    mode_val = int(m_mode.group(1), 16)
                    break

            v1 = v2 = v8 = 0
            words = []
            for ln in k.lines:
                m_mov = re.match(r"v_mov_b32_e32 v([128]), (0x[0-9a-fA-F]+)", ln)
                if m_mov:
                    reg = int(m_mov.group(1))
                    val = int(m_mov.group(2), 16)
                    if reg == 1:
                        v1 = val
                    elif reg == 2:
                        v2 = val
                    else:
                        v8 = val
                    continue

                m_op = re.match(
                    r"(v_frexp_mant_f16_e64|v_frexp_exp_i16_f16_e64|v_ldexp_f16)\s+v8,\s+([^,\s]+)(?:,\s+v2)?(?:\s+(.*))?$",
                    ln,
                )
                if not m_op:
                    continue
                op = m_op.group(1).removesuffix("_e64")
                s0 = m_op.group(2)
                mod = (m_op.group(3) or "").strip()
                src0_neg = s0.startswith("-")
                src0_abs = "|" in s0
                h = v1 & 0xFFFF
                n = v2 & 0xFFFF
                if mode_val == pr.MODE_FLUSH16 and op != "v_frexp_exp_i16_f16":
                    w = flush_map[(op, h, n, mod)]
                    if self.bad_flush and mod == "mul:2":
                        w ^= 0x0400
                else:
                    w = pr.expected_word(
                        op, h, n, mod=mod, init_dst=v8, src0_abs=src0_abs, src0_neg=src0_neg
                    )
                    if self.bad_grid and mode_val == pr.MODE_CONSOLE and not words and op == "v_frexp_mant_f16":
                        w ^= 1
                    if self.bad_omod and mode_val == pr.MODE_CONSOLE and mod == "mul:2" and not words:
                        w ^= 0x0100
                    if self.wave64_diff and k.wave == 64 and not words and mode_val == pr.MODE_CONSOLE:
                        w ^= 1
                words.append(w)
            out.append(words)
        return out

    def close(self):
        self.closed = True


def _run_main(oracle, *args):
    buf = io.StringIO()
    err = io.StringIO()
    with mock.patch.object(runner, "open_oracle", return_value=oracle), redirect_stdout(buf), redirect_stderr(err):
        rc = pr.main(list(args))
    return rc, buf.getvalue(), err.getvalue()


class ReferenceModelTests(unittest.TestCase):
    def test_self_test(self):
        with redirect_stdout(io.StringIO()):
            self.assertEqual(pr.self_test(), 0)

    def test_clamp_f16_rules(self):
        self.assertEqual(pr.ref_clamp_f16(0x0000), 0x0000)
        self.assertEqual(pr.ref_clamp_f16(0x8000), 0x0000)
        self.assertEqual(pr.ref_clamp_f16(0x0001), 0x0001)
        self.assertEqual(pr.ref_clamp_f16(0x8001), 0x0000)
        self.assertEqual(pr.ref_clamp_f16(0x3800), 0x3800)
        self.assertEqual(pr.ref_clamp_f16(0x3C00), 0x3C00)
        self.assertEqual(pr.ref_clamp_f16(0x3C01), 0x3C00)
        self.assertEqual(pr.ref_clamp_f16(0x7C00), 0x3C00)
        self.assertEqual(pr.ref_clamp_f16(0xFC00), 0x0000)
        self.assertEqual(pr.ref_clamp_f16(0x7C01), 0x0000)
        self.assertEqual(pr.ref_clamp_f16(0x7E00), 0x0000)
        self.assertEqual(pr.ref_clamp_f16(0xFE55), 0x0000)

    def test_expected_word_console_mode_and_high_half_preserved(self):
        # omod is a no-op under console MODE; clamp applies to mant and ldexp only.
        for mod in ("", "mul:2", "mul:4", "div:2"):
            self.assertEqual(pr.expected_word("v_frexp_mant_f16", 0xBC00, mod=mod, init_dst=0x12345678), 0x1234B800)
            self.assertEqual(pr.expected_word("v_ldexp_f16", 0x3C00, 1, mod=mod, init_dst=0x12345678), 0x12344000)
        for mod in ("clamp", "clamp mul:2", "clamp mul:4", "clamp div:2"):
            self.assertEqual(pr.expected_word("v_frexp_mant_f16", 0xBC00, mod=mod, init_dst=0x12345678), 0x12340000)
            self.assertEqual(pr.expected_word("v_frexp_mant_f16", 0x7C00, mod=mod, init_dst=0x12345678), 0x12343C00)
            self.assertEqual(pr.expected_word("v_ldexp_f16", 0x3C00, 1, mod=mod, init_dst=0x12345678), 0x12343C00)
            self.assertEqual(pr.expected_word("v_frexp_exp_i16_f16", 0x4800, mod=mod, init_dst=0x12345678), 0x12340004)
        # abs applied before neg
        self.assertEqual(
            pr.expected_word("v_frexp_mant_f16", 0xBC00, mod="clamp", src0_abs=True, src0_neg=True),
            0xDEAD0000,
        )
        self.assertEqual(
            pr.expected_word("v_frexp_mant_f16", 0xBC00, mod="clamp", src0_abs=True, src0_neg=False),
            0xDEAD3800,
        )


class KernelTests(unittest.TestCase):
    def test_kernel_structure(self):
        for op in pr.OPS:
            for mod in pr.MODS:
                lines, pairs = pr.mode_grid_kernel_lines(op, mod, pr.MODE_CONSOLE)
                self.assertEqual(lines[0], "s_setreg_imm32_b32 hwreg(HW_REG_MODE, 0, 10), 0x1c0")
                self.assertEqual(lines[-2:], ["s_waitcnt vmcnt(0)", "s_endpgm"])
                expected_n = len(pr.EDGE_F16) * len(pr.LDEXP_EXPS) if op == "v_ldexp_f16" else len(pr.EDGE_F16)
                self.assertEqual(len(pairs), expected_n)

    def test_build_jobs_deduplicates_selections_in_first_seen_order(self):
        ops = ["v_ldexp_f16", "v_frexp_mant_f16"]
        waves = [64, 32]
        unique_kernels, unique_meta = pr.build_jobs(ops, waves)
        kernels, meta = pr.build_jobs([*ops, ops[0]], [*waves, waves[0]])
        self.assertEqual(meta, unique_meta)
        self.assertEqual([(k.wave, k.lines) for k in kernels], [(k.wave, k.lines) for k in unique_kernels])
        self.assertEqual(
            [(wave, op) for kind, wave, op, mod, _ in meta if kind == "grid" and not mod],
            [(64, ops[0]), (64, ops[1]), (32, ops[0]), (32, ops[1])],
        )

    def test_kernels_assemble_when_amd_clang_available(self):
        try:
            clang = runner.find_clang()
        except runner.Unavailable as e:
            self.skipTest(str(e))
        for wave in (32, 64):
            kernels, _ = pr.build_jobs(["v_frexp_mant_f16"], [wave])
            text, _, _ = asm.module_text(kernels[:4], "gfx1036", wave=wave)
            with tempfile.TemporaryDirectory() as d:
                s_path = Path(d) / "k.s"
                o_path = Path(d) / "k.o"
                s_path.write_text(text, encoding="utf-8", newline="\n")
                flags = ["-mwavefrontsize64"] if wave == 64 else []
                res = subprocess.run(
                    [clang, "--target=amdgcn-amd-amdhsa", "-mcpu=gfx1036", *flags, "-c", str(s_path), "-o", str(o_path)],
                    capture_output=True,
                    text=True,
                )
            self.assertEqual(res.returncode, 0, res.stderr[:800])


class EvaluationTests(unittest.TestCase):
    def test_repeated_selections_do_not_count_a_measurement_twice(self):
        ops = ["v_frexp_mant_f16"]
        waves = [32]
        kernels, meta = pr.build_jobs(ops, waves)
        raw = FakeOracle().run(kernels)
        raw[0][0] ^= 1
        unique = pr.evaluate_results(ops, waves, meta, raw)
        self.assertEqual(unique["total_words"], 257)
        self.assertEqual(unique["mismatches"], 1)
        self.assertEqual(pr.evaluate_results(ops * 2, waves * 2, meta, raw), unique)

    def test_evaluator_rejects_duplicate_measurements(self):
        ops = ["v_frexp_mant_f16"]
        waves = [32]
        kernels, meta = pr.build_jobs(ops, waves)
        raw = FakeOracle().run(kernels)
        for job_kind in ("grid", "src_mods", "flush_ctrl", "ieee_ctrl"):
            with self.subTest(kind=job_kind):
                index = next(i for i, job in enumerate(meta) if job[0] == job_kind)
                kind, wave, op, mod, _ = meta[index]
                bad_raw = [words.copy() for words in raw]
                bad_raw[index][0] ^= 1
                baseline = pr.evaluate_results(ops, waves, meta, bad_raw)
                self.assertEqual(baseline["mismatches"], 1)
                # A later copy of the same job must keep the first mismatch visible and flag a harness error.
                result = pr.evaluate_results(ops, waves, [*meta, meta[index]], [*bad_raw, raw[index]])
                self.assertEqual(result["total_words"], baseline["total_words"])
                self.assertEqual(result["mismatches"], 1)
                self.assertEqual(result["first_mismatches"], baseline["first_mismatches"])
                self.assertEqual(result["harness_errors"], [
                    {"kind": kind, "wave": wave, "op": op, "mod": mod, "error": "duplicate job result"},
                ])


class ProbeCliTests(unittest.TestCase):
    def test_fake_oracle_pass_and_json_lf(self):
        oracle = FakeOracle()
        with tempfile.TemporaryDirectory() as d:
            report = os.path.join(d, "nested", "report.json")
            rc, out, err = _run_main(oracle, "--json", report)
            self.assertEqual(rc, 0, out + err)
            self.assertTrue(oracle.closed)
            self.assertIn("mismatches=0, wave_diffs=0, omod_diffs=0", out)
            with open(report, "rb") as f:
                raw = f.read()
            self.assertNotIn(b"\r\n", raw)
            doc = json.loads(raw.decode("utf-8"))
            self.assertEqual(doc["arch"], "gfx1036")
            self.assertEqual(doc["waves"], [32, 64])
            self.assertEqual(doc["ops"], list(pr.OPS))
            self.assertEqual(doc["mismatches"], 0)
            self.assertEqual(doc["wave_diffs"], 0)
            self.assertEqual(doc["omod_diffs"], 0)
            self.assertEqual(doc["total_words"], 3466)

    def test_duplicate_cli_selections_cannot_hide_first_measurement(self):
        op = "v_frexp_mant_f16"
        for ops, waves in ((op, "32,32"), (f"{op},{op}", "32")):
            with self.subTest(ops=ops, waves=waves):
                oracle = FakeOracle()
                run = oracle.run

                def flip_first_word(kernels):
                    raw = run(kernels)
                    raw[0][0] ^= 1
                    return raw

                with mock.patch.object(oracle, "run", side_effect=flip_first_word) as run_oracle:
                    rc, out, err = _run_main(oracle, "--ops", ops, "--waves", waves)
                self.assertEqual(rc, 1, out + err)
                self.assertIn("total_words=257", out)
                self.assertIn("mismatches=1", out)
                self.assertIn("MISMATCH grid wave32 v_frexp_mant_f16 '' h=0x0000,n=0: got 0xdead0001, want 0xdead0000", out)
                run_oracle.assert_called_once()
                self.assertEqual(len(run_oracle.call_args.args[0]), len(pr.MODS) + 3)

    def test_duplicate_cli_selections_match_unique_report_in_first_seen_order(self):
        ops = ["v_ldexp_f16", "v_frexp_mant_f16"]
        with tempfile.TemporaryDirectory() as d:
            report = os.path.join(d, "report.json")
            docs = []
            for op_spec, wave_spec in ((",".join(ops), "64,32"), (", ".join([*ops, ops[0]]), "64, 32, 64")):
                rc, out, err = _run_main(FakeOracle(), "--ops", op_spec, "--waves", wave_spec, "--json", report)
                self.assertEqual(rc, 0, out + err)
                with open(report, encoding="utf-8") as f:
                    docs.append(json.load(f))
            self.assertEqual(docs[0]["ops"], ops)
            self.assertEqual(docs[0]["waves"], [64, 32])
            self.assertEqual(docs[0]["total_words"], 2948)
            self.assertEqual(docs[1], docs[0])

    def test_fake_oracle_grid_mismatch_exits_1(self):
        rc, out, _ = _run_main(FakeOracle(bad_grid=True), "--ops", "v_frexp_mant_f16", "--waves", "32")
        self.assertEqual(rc, 1, out)
        self.assertIn("MISMATCH grid", out)

    def test_fake_oracle_flush_control_mismatch_exits_1(self):
        rc, out, _ = _run_main(FakeOracle(bad_flush=True), "--ops", "v_frexp_mant_f16", "--waves", "32")
        self.assertEqual(rc, 1, out)
        self.assertIn("MISMATCH flush_ctrl", out)

    def test_fake_oracle_omod_diff_exits_1(self):
        rc, out, _ = _run_main(FakeOracle(bad_omod=True), "--ops", "v_frexp_mant_f16", "--waves", "32")
        self.assertEqual(rc, 1, out)
        self.assertIn("OMOD DIFF", out)

    def test_fake_oracle_wave_diff_exits_1(self):
        rc, out, _ = _run_main(FakeOracle(wave64_diff=True), "--ops", "v_frexp_mant_f16", "--waves", "32,64")
        self.assertEqual(rc, 1, out)
        self.assertIn("WAVE DIFF", out)

    def test_fake_oracle_harness_error_exits_2(self):
        rc, out, _ = _run_main(FakeOracle(error_msg="assembler rejected: test"), "--ops", "v_frexp_mant_f16", "--waves", "32")
        self.assertEqual(rc, 2, out)
        self.assertIn("harness error", out)

    def test_bad_cli_and_json_paths(self):
        with tempfile.TemporaryDirectory() as d:
            with redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as ctx:
                    pr.main(["--ops", "v_bogus", "--waves", "32"])
                self.assertEqual(ctx.exception.code, 2)
                with self.assertRaises(SystemExit) as ctx2:
                    pr.main(["--waves", "16"])
                self.assertEqual(ctx2.exception.code, 2)
                with self.assertRaises(SystemExit) as ctx3:
                    _run_main(FakeOracle(), "--json", d)
                self.assertEqual(ctx3.exception.code, 2)

            blocker = os.path.join(d, "file_not_dir")
            with open(blocker, "wb") as f:
                f.write(b"keep")
            rc, _, err = _run_main(
                FakeOracle(), "--ops", "v_frexp_mant_f16", "--waves", "32", "--json", os.path.join(blocker, "r.json")
            )
            self.assertEqual(rc, 2)
            self.assertIn("ERROR: cannot write", err)
            self.assertNotIn("Traceback", err)

    def test_non_gfx10_arch_exits_77(self):
        oracle = FakeOracle()
        oracle.arch = "gfx1100"
        rc, out, _ = _run_main(oracle)
        self.assertEqual(rc, 77, out)
        self.assertTrue(oracle.closed)

    def test_unavailable_oracle_exits_77(self):
        buf = io.StringIO()
        with mock.patch.object(runner, "open_oracle", side_effect=runner.Unavailable("missing")), redirect_stdout(buf):
            rc = pr.main(["--ops", "v_frexp_mant_f16", "--waves", "32"])
        self.assertEqual(rc, 77)
        self.assertIn("unavailable", buf.getvalue())

    def test_malformed_comma_tokens_and_empty_args_rejected_before_oracle(self):
        for bad_args in (
            ["--ops", "v_frexp_mant_f16,"],
            ["--ops", ",v_frexp_mant_f16"],
            ["--ops", "v_frexp_mant_f16,,v_ldexp_f16"],
            ["--waves", "32,"],
            ["--waves", "32,,64"],
            ["--hw-arch", ""],
            ["--hw-arch", "   "],
            ["--json", ""],
            ["--json", "   "],
        ):
            with self.subTest(args=bad_args):
                with mock.patch.object(runner, "open_oracle") as open_mock, redirect_stderr(io.StringIO()):
                    with self.assertRaises(SystemExit) as cm:
                        pr.main(bad_args)
                self.assertEqual(cm.exception.code, 2)
                open_mock.assert_not_called()

    def test_json_blocker_trailing_sep_and_atomic_write_report(self):
        with tempfile.TemporaryDirectory() as d:
            blocker = os.path.join(d, "blocker")
            Path(blocker).write_bytes(b"keep")
            with mock.patch.object(runner, "open_oracle") as open_mock, redirect_stderr(io.StringIO()) as err:
                rc = pr.main(["--ops", "v_frexp_mant_f16", "--waves", "32", "--json", os.path.join(blocker, "r.json")])
            self.assertEqual(rc, 2)
            open_mock.assert_not_called()
            self.assertIn("ERROR: cannot write", err.getvalue())
            self.assertEqual(Path(blocker).read_bytes(), b"keep")

            with mock.patch.object(runner, "open_oracle") as open_mock, redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as cm:
                    pr.main(["--ops", "v_frexp_mant_f16", "--waves", "32", "--json", os.path.join(d, "sub") + os.sep])
            self.assertEqual(cm.exception.code, 2)
            open_mock.assert_not_called()

            out = os.path.join(d, "sub", "report.json")
            self.assertTrue(pr.write_report(out, {"ok": 1}))
            raw = Path(out).read_bytes()
            self.assertTrue(raw.endswith(b"\n"))
            self.assertNotIn(b"\r\n", raw)

            with redirect_stderr(io.StringIO()):
                self.assertFalse(pr.write_report(out, {"bad": float("nan")}))
            self.assertEqual(json.loads(Path(out).read_text(encoding="utf-8")), {"ok": 1})

            if hasattr(os, "symlink"):
                real = os.path.join(d, "real.json")
                link = os.path.join(d, "link.json")
                Path(real).write_text("old", encoding="utf-8")
                try:
                    os.symlink(real, link)
                except OSError:
                    link = None
                if link is not None:
                    self.assertTrue(pr.write_report(link, {"via": "link"}))
                    self.assertTrue(os.path.islink(link))
                    self.assertEqual(json.loads(Path(real).read_text(encoding="utf-8")), {"via": "link"})

            if os.name == "posix" and os.path.exists("/dev/null"):
                self.assertTrue(pr.write_report("/dev/null", {"ok": 1}))

    def test_evaluate_results_count_mismatch_and_unknown_arch_family(self):
        _, meta = pr.build_jobs(["v_frexp_mant_f16"], [32])
        rep = pr.evaluate_results(["v_frexp_mant_f16"], [32], meta, [])
        self.assertEqual(len(rep["harness_errors"]), 1)
        self.assertIn("expected", rep["harness_errors"][0]["error"])

        oracle = FakeOracle()
        oracle.arch = "unknown_arch_999"
        rc, out, _ = _run_main(oracle, "--ops", "v_frexp_mant_f16", "--waves", "32")
        self.assertEqual(rc, 77, out)
        self.assertTrue(oracle.closed)


if __name__ == "__main__":
    unittest.main()
