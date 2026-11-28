"""CPU-only unit tests for tools/dpp_hw_probe.py (TOOL-DPP-HW-PROBE). No GPU and no game files.

The encoding test runs AMD clang's llvm-mc when LEONIDA_AMD_CLANG points at a clang.exe with llvm-mc beside it, and
is skipped otherwise; the same words are pinned in the probe's --self-test.
"""
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(REPO, "tools")
PROBE = os.path.join(TOOLS, "dpp_hw_probe.py")
if TOOLS not in sys.path:
    sys.path.insert(0, TOOLS)

import dpp_hw_probe as probe  # noqa: E402


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


def _neg_words(wave, model, flip=None):
    """The words a GPU following `model` stores for group neg; flip=(case index, lane) breaks one word."""
    n = len(probe.NEG_CASES)
    words = [0] * (wave * n)
    for j, case in enumerate(probe.NEG_CASES):
        for lane in range(wave):
            words[lane * n + j] = probe.neg_model(case, lane, model)
    if flip:
        j, lane = flip
        words[lane * n + j] ^= 0x1
    return words


def _vopc_words(wave, model):
    runs = probe.vopc_runs()
    per = []
    for run in runs:
        mask = sum(probe.vopc_model(run, lane, model) << lane for lane in range(wave))
        per += [mask & 0xFFFFFFFF, mask >> 32]
    return per * wave


class Models(unittest.TestCase):
    def test_self_test(self):
        proc = subprocess.run([sys.executable, "-I", PROBE, "--self-test"], capture_output=True, text=True,
                              check=False)
        self.assertEqual(proc.returncode, 0, proc.stderr or proc.stdout)
        self.assertIn("self-test OK", proc.stdout)

    def test_neg_models_differ_only_on_negated_invalid_lanes(self):
        for case in probe.NEG_CASES:
            for wave in (32, 64):
                with self.subTest(case=case[0], wave=wave):
                    diff = [l for l in range(wave) if probe.neg_model(case, l, "mods_after_zero")
                            != probe.neg_model(case, l, "mods_before_zero")]
                    invalid = [l for l in range(wave) if probe.lane_active(l, case[4])
                               and not probe.source_valid(l, case[3], case[4])]
                    self.assertEqual(diff, invalid if case[2] == "neg" else [])
                    for l in diff:
                        self.assertEqual(probe.neg_model(case, l, "mods_after_zero"), 0x80000000)
                        self.assertEqual(probe.neg_model(case, l, "mods_before_zero"), 0)

    def test_each_vopc_model_is_singled_out_by_some_run(self):
        """For every pair of models some run gives a lane a different bit, so a GPU matches at most one."""
        for wave in (32, 64):
            for a in probe.VOPC_MODELS:
                for b in probe.VOPC_MODELS:
                    if a < b:
                        with self.subTest(wave=wave, models=(a, b)):
                            self.assertTrue(any(probe.vopc_model(r, l, a) != probe.vopc_model(r, l, b)
                                                for r in probe.vopc_runs() for l in range(wave)))

    def test_lane_categories(self):
        row_mask = probe.Ctrl("quad_perm", (0, 1, 2, 3), row_mask=0x5, bound_ctrl=True)
        bank_mask = probe.Ctrl("quad_perm", (0, 1, 2, 3), bank_mask=0x5, bound_ctrl=True)
        swap = probe.Ctrl("quad_perm", (1, 0, 3, 2))
        self.assertEqual([probe.lane_category(l, row_mask, "all") for l in (0, 15, 16, 31, 32, 48)],
                         ["valid", "valid", "masked", "masked", "valid", "masked"])
        self.assertEqual([probe.lane_category(l, bank_mask, "all") for l in (0, 3, 4, 7, 8, 12)],
                         ["valid", "valid", "masked", "masked", "valid", "masked"])
        self.assertEqual([probe.lane_category(l, swap, "even") for l in (0, 1, 2)],
                         ["unwritten", "inactive", "unwritten"])
        shr = probe.Ctrl("row_shr", 1, bound_ctrl=True)
        self.assertEqual([probe.lane_category(l, shr, "all") for l in (0, 1, 16, 17)],
                         ["zero_src", "valid", "zero_src", "valid"])


class Kernels(unittest.TestCase):
    def test_vopc_compares_are_long_words_with_no_wait_state_between_them(self):
        for fam in probe.FAMILIES:
            for wave in (32, 64):
                with self.subTest(fam=fam, wave=wave):
                    lines = probe.vopc_lines(wave, fam)
                    longs = [ln for ln in lines if ln.startswith(".long")]
                    self.assertEqual(len(longs), len(probe.vopc_runs()))
                    for ln, (_, op, ctrl, _, _) in zip(longs, probe.vopc_runs()):
                        words = tuple(int(w, 16) for w in re.findall(r"0x([0-9a-f]{8})", ln.split(";")[0]))
                        self.assertEqual(words, probe.vopc_dpp_words(fam, op, 1, 2, ctrl))
                    # no compare mnemonic outside the .long comments: gfx10 cannot assemble one
                    code = [ln.split(";")[0] for ln in lines]
                    self.assertFalse([c for c in code if re.match(r"\s*v_cmpx?_", c)])

    def test_kernels_restore_exec_and_end(self):
        for wave in (32, 64):
            for lines in (probe.neg_lines(wave), probe.vopc_lines(wave, "gfx11")):
                with self.subTest(wave=wave):
                    self.assertEqual(lines[-1], "s_endpgm")
                    stores = [i for i, ln in enumerate(lines) if ln.startswith("global_store_b32")]
                    self.assertTrue(stores)
                    last_exec = max(i for i, ln in enumerate(lines[:stores[0]]) if "exec_lo" in ln and
                                    ln.startswith("s_mov_b32 exec_lo"))
                    self.assertEqual(lines[last_exec], "s_mov_b32 exec_lo, -1")

    def test_neg_kernel_keeps_the_multiplier_register(self):
        lines = probe.neg_lines(32)
        dsts = {int(m) for ln in lines for m in re.findall(r"^v_(?:add|mul)_f32_dpp v(\d+)", ln)}
        self.assertEqual(len(dsts), len(probe.NEG_CASES))
        self.assertNotIn(7, dsts)
        self.assertNotIn(1, dsts)
        self.assertNotIn(2, dsts)

    def test_store_offsets_fit_gfx10(self):
        # GFX10 global instructions take a 12-bit signed offset
        for wave in (32, 64):
            for lines in (probe.neg_lines(wave), probe.vopc_lines(wave, "gfx10")):
                offs = [int(m) for ln in lines for m in re.findall(r"offset:(\d+)", ln)]
                self.assertLessEqual(max(offs), 2047)

    def test_encoding_rejects_bad_fields(self):
        with self.assertRaises(ValueError):
            probe.vopc_dpp_words("gfx12", "v_cmp_gt_f32", 1, 2, probe.Ctrl("row_shr", 1))
        with self.assertRaises(ValueError):
            probe.Ctrl("row_shr", 0)
        with self.assertRaises(ValueError):
            probe.Ctrl("quad_perm", (0, 1, 2, 4))
        with self.assertRaises(ValueError):
            probe.Ctrl("row_ror", 1)
        with self.assertRaises(ValueError):
            probe.dpp_word(256, probe.Ctrl("row_shr", 1))


@unittest.skipUnless(os.environ.get("LEONIDA_AMD_CLANG") and shutil.which(
    os.path.join(os.path.dirname(os.environ.get("LEONIDA_AMD_CLANG", "")), "llvm-mc")),
    "needs LEONIDA_AMD_CLANG with llvm-mc beside it")
class LlvmMcEncodings(unittest.TestCase):
    """The gfx11 words the probe emits are the ones AMD clang's llvm-mc encodes for the same text."""

    def test_gfx11_vopc_dpp_words(self):
        mc = shutil.which(os.path.join(os.path.dirname(os.environ["LEONIDA_AMD_CLANG"]), "llvm-mc"))
        for op in probe.VOPC_OPS:
            for _, ctrl, _ in probe.VOPC_CTRLS:
                dst = "" if op.startswith("v_cmpx_") else "vcc_lo, "
                text = f"{op}_dpp {dst}v1, v2 {ctrl.text()}\n"
                with self.subTest(text=text.strip()):
                    r = subprocess.run([mc, "-arch=amdgcn", "-mcpu=gfx1101", "--show-encoding"], input=text,
                                       capture_output=True, text=True, check=False)
                    self.assertEqual(r.returncode, 0, r.stderr)
                    enc = re.search(r"encoding: \[([^\]]+)\]", r.stdout)
                    self.assertIsNotNone(enc, r.stdout)
                    b = bytes(int(x, 16) for x in enc.group(1).split(","))
                    got = (int.from_bytes(b[0:4], "little"), int.from_bytes(b[4:8], "little"))
                    self.assertEqual(got, probe.vopc_dpp_words("gfx11", op, 1, 2, ctrl))


class Judging(unittest.TestCase):
    def test_judge_neg_names_the_matching_model(self):
        for model in probe.NEG_MODELS:
            judged = probe.judge_neg(32, _neg_words(32, model))
            with self.subTest(model=model):
                self.assertTrue(all(model in c["models"] for c in judged.values()))
                self.assertEqual(probe.summary({"neg": {"32": judged}})["neg wave32"]["consistent_models"],
                                 [model])

    def test_judge_neg_reports_a_case_no_model_matches(self):
        judged = probe.judge_neg(32, _neg_words(32, "mods_after_zero", flip=(3, 5)))
        s = probe.summary({"neg": {"32": judged}})["neg wave32"]
        self.assertEqual(s["unmatched_cases"], [probe.NEG_CASES[3][0]])

    def test_judge_vopc_names_the_matching_model(self):
        for wave in (32, 64):
            for model in probe.VOPC_MODELS:
                with self.subTest(wave=wave, model=model):
                    judged = probe.judge_vopc(wave, _vopc_words(wave, model))
                    s = probe.summary({"vopc": {str(wave): judged}})[f"vopc wave{wave}"]
                    self.assertEqual(s["consistent_models"], [model])
                    self.assertEqual(s["unmatched_cases"], [])

    def test_judge_vopc_rejects_lanes_that_disagree(self):
        words = _vopc_words(32, "unwritten_gets_0")
        words[-1] ^= 1
        with self.assertRaises(RuntimeError):
            probe.judge_vopc(32, words)


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

    def test_bad_arguments_are_rejected_before_the_oracle_opens(self):
        modules, open_oracle = _fake_hw_oracle()
        for argv in (["--group", ""], ["--group", "neg,abs"], ["--waves", "16"], ["--waves", "32,x"],
                     ["--json", ""], ["--json", "bad\x00.json"]):
            with self.subTest(argv=argv):
                rc, _, err = self.run_main(argv, modules)
                self.assertEqual(rc, 2)
                self.assertIn("error:", err)
                open_oracle.assert_not_called()

    def test_json_path_that_is_a_directory_is_rejected(self):
        modules, open_oracle = _fake_hw_oracle()
        with tempfile.TemporaryDirectory() as tmp:
            rc, _, err = self.run_main(["--json", tmp], modules)
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

    def test_report_and_exit_code_follow_the_judged_cases(self):
        oracle = types.SimpleNamespace(arch="gfx1101", close=mock.Mock())
        modules, _ = _fake_hw_oracle(open_side_effect=lambda: oracle)
        good = {"neg": {"32": probe.judge_neg(32, _neg_words(32, "mods_after_zero"))}}
        bad = {"neg": {"32": probe.judge_neg(32, _neg_words(32, "mods_after_zero", flip=(0, 0)))}}
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, "sub", "report.json")
            with mock.patch.object(probe, "probe", return_value=good):
                rc, stdout, _ = self.run_main(["--group", "neg", "--waves", "32", "--json", out], modules)
            self.assertEqual(rc, 0)
            self.assertIn("every case matches mods_after_zero", stdout)
            with open(out, "rb") as f:
                raw = f.read()
            self.assertNotIn(b"\r\n", raw)
            self.assertEqual(json.loads(raw)["arch"], "gfx1101")
            with mock.patch.object(probe, "probe", return_value=bad):
                rc, stdout, _ = self.run_main(["--group", "neg", "--waves", "32"], modules)
            self.assertEqual(rc, 1)
            self.assertIn("no model matches", stdout)

    def test_a_failed_kernel_is_a_harness_error(self):
        oracle = types.SimpleNamespace(arch="gfx1101", close=mock.Mock(),
                                       run=lambda kernels: ["assembler rejected: x"] * len(kernels))
        modules, _ = _fake_hw_oracle(open_side_effect=lambda: oracle)
        rc, _, err = self.run_main(["--group", "vopc", "--waves", "32"], modules)
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
