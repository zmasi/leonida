"""CPU-only unit tests for tools/valu_literal64_probe.py (HW-VALU-LITERAL64-EXT). No GPU and no game files.

The assembler test runs AMD clang's llvm-mc when LEONIDA_AMD_CLANG points at a clang.exe with llvm-mc beside it, and
is skipped otherwise.
"""
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(REPO, "tools")
PROBE = os.path.join(TOOLS, "valu_literal64_probe.py")
if TOOLS not in sys.path:
    sys.path.insert(0, TOOLS)

import valu_literal64_probe as probe  # noqa: E402


class _FakeUnavailable(Exception):
    pass


def _fake_hw_oracle(open_side_effect=None, family="gfx11"):
    def fam(arch):
        if family is None:
            raise ValueError(arch)
        return family
    asm_mod = types.SimpleNamespace(family=fam, Kernel=lambda *a, **k: (a, k))
    open_oracle = mock.Mock(side_effect=open_side_effect or _FakeUnavailable("no gpu"))
    runner_mod = types.SimpleNamespace(Unavailable=_FakeUnavailable, open_oracle=open_oracle)
    pkg = types.SimpleNamespace(asm=asm_mod, runner=runner_mod)
    return {"hw_oracle": pkg, "hw_oracle.asm": asm_mod, "hw_oracle.runner": runner_mod}, open_oracle


def _words(model=None):
    """The words a GPU stores: each case under `model`, or under the row's rule for its operand when None."""
    out = []
    for case, lit in probe.runs():
        v = probe.expected(case, lit, model or case.rule)
        out += [v & 0xFFFFFFFF, v >> 32]
    return out


class Model(unittest.TestCase):
    def test_self_test_passes(self):
        with mock.patch.object(sys, "stdout", io.StringIO()):
            probe.self_test()

    def test_extend(self):
        self.assertEqual(probe.extend(0x80000000, "sext"), 0xFFFFFFFF80000000)
        self.assertEqual(probe.extend(0x80000000, "zext"), 0x80000000)
        self.assertEqual(probe.extend(0x00000001, "sext"), 1)
        with self.assertRaises(ValueError):
            probe.extend(1 << 32, "sext")
        with self.assertRaises(ValueError):
            probe.extend(1, "other")

    def test_the_row_examples(self):
        # docs/design.md: v_cmp_lt_i64 vcc_lo, 5, 0xffffff9c = 0; v_ashrrev_i64 4, 0xffffff9c = 0xFFFFFFFF_FFFFFFF9;
        # v_mad_i64_i32 1, 1, 0xffffff9c = -99
        named = {c.name: c for c in probe.CASES}
        self.assertEqual(named["v_cmp_lt_i64_e64 v, lit"].model(5, probe.extend(0xFFFFFF9C, "sext")), 0)
        self.assertEqual(probe.expected(named["v_ashrrev_i64 4, lit"], 0xFFFFFF9C, "sext"), 0xFFFFFFFFFFFFFFF9)
        self.assertEqual(probe.s64(probe.expected(named["v_mad_i64_i32 1, 1, lit"], 0xFFFFFF9C, "sext")), -99)

    def test_every_case_separates_the_models_except_the_control(self):
        for case in probe.CASES:
            for lit in probe.LITERALS:
                with self.subTest(case=case.name, lit=hex(lit)):
                    same = probe.expected(case, lit, "sext") == probe.expected(case, lit, "zext")
                    self.assertEqual(same, lit == 0x7FFFFFFF)

    def test_operand_kinds_follow_the_op_names(self):
        for case in probe.CASES:
            op = case.name.split()[0]
            with self.subTest(case=case.name):
                signed = "_i64" in op
                self.assertEqual(case.kind, "i64" if signed else "u64")
                self.assertEqual(case.rule, "sext" if signed else "zext")

    def test_kernel_lines_store_each_result_at_its_own_offset(self):
        for wave in (32, 64):
            with self.subTest(wave=wave):
                lines = probe.kernel_lines(wave)
                offsets = [int(ln.rsplit("offset:", 1)[1]) for ln in lines if ln.startswith("global_store_b32")]
                self.assertEqual(offsets, [4 * i for i in range(2 * len(probe.runs()))])
                self.assertEqual(lines[-1], "s_endpgm")
                self.assertEqual(any("exec_lo" in ln for ln in lines), wave == 32)
                self.assertEqual(any("s[4:5]" in ln for ln in lines), wave == 64)
        with self.assertRaises(ValueError):
            probe.kernel_lines(16)

    def test_every_literal_is_in_the_kernel_once_per_case(self):
        text = "\n".join(probe.kernel_lines(32))
        for lit in probe.LITERALS:
            self.assertEqual(text.count(f"0x{lit:08x}"), len(probe.CASES), hex(lit))


@unittest.skipUnless(os.environ.get("LEONIDA_AMD_CLANG") and shutil.which(
    os.path.join(os.path.dirname(os.environ.get("LEONIDA_AMD_CLANG", "")), "llvm-mc")),
    "needs LEONIDA_AMD_CLANG with llvm-mc beside it")
class LlvmMcAssembles(unittest.TestCase):
    """Every kernel body assembles for gfx1030 (in GFX10 spelling) and gfx1101, in both wave sizes, and every
    literal is encoded as the one dword the probe names."""

    def test_kernel_lines_assemble(self):
        from hw_oracle import asm
        mc = shutil.which(os.path.join(os.path.dirname(os.environ["LEONIDA_AMD_CLANG"]), "llvm-mc"))
        for cpu in ("gfx1030", "gfx1101"):
            for wave in (32, 64):
                lines = probe.kernel_lines(wave)
                if cpu == "gfx1030":
                    lines = [asm.gfx10_spelling(ln) for ln in lines]
                with self.subTest(cpu=cpu, wave=wave):
                    r = subprocess.run([mc, "-arch=amdgcn", f"-mcpu={cpu}", f"-mattr=+wavefrontsize{wave}",
                                        "--show-encoding"], input="\n".join(lines) + "\n", capture_output=True,
                                       text=True, check=False)
                    self.assertEqual(r.returncode, 0, r.stderr)
                    self.assertNotIn("error", r.stderr)
                    for lit in probe.LITERALS:
                        b = lit.to_bytes(4, "little")
                        tail = ",".join(f"0x{x:02x}" for x in b) + "]"
                        enc = [ln for ln in r.stdout.splitlines() if f"0x{lit:08x}" in ln.split(";")[0]]
                        self.assertEqual(len(enc), len(probe.CASES), hex(lit))
                        self.assertTrue(all(ln.rstrip().endswith(tail) for ln in enc), hex(lit))


class Judging(unittest.TestCase):
    def test_words_that_follow_the_rule_pass(self):
        entry = probe.judge(32, _words())
        self.assertTrue(entry["ok"])
        self.assertEqual(sum(r["control"] for r in entry["results"]), len(probe.CASES))
        self.assertIn("63/63 follow the rule", probe.describe(entry))

    def test_one_model_for_every_operand_fails(self):
        for model in probe.MODELS:
            with self.subTest(model=model):
                entry = probe.judge(64, _words(model))
                self.assertFalse(entry["ok"])
                wrong = [r for r in entry["results"] if not r["follows_rule"]]
                kind = "u64" if model == "sext" else "i64"
                self.assertTrue(wrong and all(r["operand"] == kind for r in wrong))
                self.assertIn("rule:", probe.describe(entry))

    def test_a_word_neither_model_gives_is_reported(self):
        words = _words()
        words[0] ^= 2
        entry = probe.judge(32, words)
        self.assertFalse(entry["ok"])
        self.assertEqual(entry["results"][0]["models"], [])
        self.assertIn("neither model", probe.describe(entry))

    def test_a_short_result_is_a_harness_error(self):
        with self.assertRaises(RuntimeError):
            probe.judge(32, _words()[:-1])


class Cli(unittest.TestCase):
    def run_main(self, argv, modules):
        stdout, stderr = io.StringIO(), io.StringIO()
        with mock.patch.dict(sys.modules, modules), mock.patch.object(sys, "stdout", stdout), \
                mock.patch.object(sys, "stderr", stderr):
            rc = probe.main(argv)
        return rc, stdout.getvalue(), stderr.getvalue()

    def test_missing_oracle_exits_77(self):
        env = {k: v for k, v in os.environ.items() if not k.startswith("LEONIDA_HW_") and k != "LEONIDA_AMD_CLANG"}
        env["LEONIDA_AMD_CLANG"] = os.path.join(REPO, "does_not_exist_clang")
        proc = subprocess.run([sys.executable, "-I", PROBE], env=env, capture_output=True, text=True, check=False)
        self.assertEqual(proc.returncode, 77, proc.stderr or proc.stdout)

    def test_self_test_flag(self):
        proc = subprocess.run([sys.executable, "-I", PROBE, "--self-test"], capture_output=True, text=True,
                              check=False)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("self-test OK", proc.stdout)

    def test_bad_arguments_are_rejected_before_the_oracle_opens(self):
        modules, open_oracle = _fake_hw_oracle()
        for argv in (["--waves", ""], ["--waves", "16"], ["--waves", "32,x"], ["--json", ""],
                     ["--json", "bad\x00.json"]):
            with self.subTest(argv=argv):
                rc, _, err = self.run_main(argv, modules)
                self.assertEqual(rc, 2)
                self.assertIn("error:", err)
                open_oracle.assert_not_called()

    def test_json_path_that_is_a_directory_is_rejected(self):
        modules, open_oracle = _fake_hw_oracle()
        with tempfile.TemporaryDirectory() as tmp:
            rc, _, _ = self.run_main(["--json", tmp], modules)
        self.assertEqual(rc, 2)
        open_oracle.assert_not_called()

    def test_other_families_skip_with_77_and_close_the_oracle(self):
        for family in ("gfx12", None):
            oracle = types.SimpleNamespace(arch="gfx1201", close=mock.Mock())
            modules, _ = _fake_hw_oracle(open_side_effect=lambda: oracle, family=family)
            with self.subTest(family=family):
                rc, _, err = self.run_main([], modules)
                self.assertEqual(rc, 77)
                self.assertIn("SKIP", err)
                oracle.close.assert_called_once()

    def test_run_writes_the_report_and_exit_code_follows_it(self):
        def oracle_for(model=None):
            return types.SimpleNamespace(arch="gfx1101", close=mock.Mock(),
                                         run=lambda ks: [_words(model) for _ in ks])
        good = oracle_for()
        modules, _ = _fake_hw_oracle(open_side_effect=lambda: good)
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, "sub", "report.json")
            rc, stdout, _ = self.run_main(["--json", out], modules)
            self.assertEqual(rc, 0, stdout)
            self.assertIn("every literal is extended as the row says", stdout)
            with open(out, "rb") as f:
                raw = f.read()
            self.assertNotIn(b"\r\n", raw)
            data = json.loads(raw)
            self.assertEqual((data["arch"], data["ok"], [k["wave"] for k in data["kernels"]]),
                             ("gfx1101", True, [32, 64]))
        good.close.assert_called_once()
        bad = oracle_for("zext")
        modules, _ = _fake_hw_oracle(open_side_effect=lambda: bad)
        rc, stdout, _ = self.run_main(["--waves", "32"], modules)
        self.assertEqual(rc, 1)
        self.assertIn("NOT every literal", stdout)

    def test_a_failed_kernel_is_a_harness_error(self):
        oracle = types.SimpleNamespace(arch="gfx1101", close=mock.Mock(),
                                       run=lambda kernels: ["assembler rejected: x"] * len(kernels))
        modules, _ = _fake_hw_oracle(open_side_effect=lambda: oracle)
        rc, _, err = self.run_main(["--waves", "32"], modules)
        self.assertEqual(rc, 2)
        self.assertIn("assembler rejected", err)
        oracle.close.assert_called_once()

    def test_write_json_report_error_is_one_line(self):
        with tempfile.TemporaryDirectory() as tmp:
            with mock.patch("builtins.open", side_effect=PermissionError(13, "denied")):
                err = probe.write_json_report(os.path.join(tmp, "out.json"), {})
        self.assertIn("cannot write --json", err)


if __name__ == "__main__":
    unittest.main()
