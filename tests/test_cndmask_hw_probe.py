"""CPU-only unit tests for tools/cndmask_hw_probe.py (TOOL-CNDMASK-HW-PROBE). No GPU and no game files.

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
PROBE = os.path.join(TOOLS, "cndmask_hw_probe.py")
if TOOLS not in sys.path:
    sys.path.insert(0, TOOLS)

import cndmask_hw_probe as probe  # noqa: E402


class _FakeUnavailable(Exception):
    pass


def _fake_hw_oracle(open_side_effect=None, family="gfx11", mode_skip=None):
    def fam(arch):
        if family is None:
            raise ValueError(arch)
        return family
    asm_mod = types.SimpleNamespace(family=fam, Kernel=lambda *a, **k: (a, k),
                                    mode_skip_reason=lambda mode, arch: mode_skip)
    open_oracle = mock.Mock(side_effect=open_side_effect or _FakeUnavailable("no gpu"))
    runner_mod = types.SimpleNamespace(Unavailable=_FakeUnavailable, open_oracle=open_oracle)
    pkg = types.SimpleNamespace(asm=asm_mod, runner=runner_mod)
    return {"hw_oracle": pkg, "hw_oracle.asm": asm_mod, "hw_oracle.runner": runner_mod}, open_oracle


def _words(ieee, denorm32, quiet_snan=False, flush_denormals=False, control_off=None):
    """The words a GPU stores for one kernel: sign-bit moves, optionally quieting or flushing them, and the
    controls of the given MODE; control_off=<index> breaks one control."""
    out = []
    for w, _, mod, _ in probe.runs():
        g = probe.bit_move(w, mod)
        exp, frac = (g >> 23) & 0xFF, g & 0x7FFFFF
        if quiet_snan and exp == 0xFF and frac and not frac & 0x400000:
            g |= 0x400000
        if flush_denormals and exp == 0 and frac:
            g &= 0x80000000
        out.append(g)
    ctl = [probe.control_expected(name, ieee, denorm32) for name, _, _ in probe.CONTROLS]
    if control_off is not None:
        ctl[control_off] ^= 1
    return out + ctl


class Model(unittest.TestCase):
    def test_self_test_passes(self):
        with mock.patch.object(sys, "stdout", io.StringIO()):
            probe.self_test()

    def test_bit_move_is_abs_then_neg(self):
        self.assertEqual(probe.bit_move(0xFF800001, "-|v|"), 0xFF800001)
        self.assertEqual(probe.bit_move(0x7F800001, "-|v|"), 0xFF800001)
        self.assertEqual(probe.bit_move(0xFF800001, "-v"), 0x7F800001)
        self.assertEqual(probe.bit_move(0x80000001, "|v|"), 0x00000001)
        self.assertEqual(probe.bit_move(0x12345678, "v"), 0x12345678)

    def test_classify_names_each_float_effect(self):
        self.assertEqual(probe.classify(0x7FA00000, "v", 0x7FE00000), "quieted")
        self.assertEqual(probe.classify(0x007FFFFF, "-v", 0x80000000), "flushed")
        self.assertEqual(probe.classify(0x7FC00000, "v", 0x7FC00000), "bit_move")
        # a quiet NaN cannot be "quieted", and a wrong sign is not a flush
        self.assertEqual(probe.classify(0xFFC00001, "|v|", 0x7FC00001), "bit_move")
        self.assertEqual(probe.classify(0x00000001, "-v", 0x00000000), "other")

    def test_runs_cover_every_word_mod_and_side_once(self):
        runs = probe.runs()
        self.assertEqual(len(runs), len(set((w, m, s) for w, _, m, s in runs)))
        self.assertEqual({w for w, _, _, _ in runs}, {w for w, _ in probe.WORDS})
        self.assertEqual({m for _, _, m, _ in runs}, {m for m, _ in probe.MODS})
        self.assertEqual({s for _, _, _, s in runs}, set(probe.SIDES))

    def test_kernel_lines_store_each_result_at_its_own_offset(self):
        for wave in (32, 64):
            with self.subTest(wave=wave):
                lines = probe.kernel_lines(wave)
                offsets = [int(ln.rsplit("offset:", 1)[1]) for ln in lines if ln.startswith("global_store_b32")]
                self.assertEqual(offsets, [4 * i for i in range(probe.words_per_kernel())])
                cond = "s4" if wave == 32 else "s[4:5]"
                cnd = [ln for ln in lines if ln.startswith("v_cndmask_b32_e64")]
                self.assertEqual(len(cnd), len(probe.runs()))
                self.assertTrue(all(ln.endswith(", " + cond) for ln in cnd))
                self.assertEqual(any(ln.startswith("s_mov_b32 s5") for ln in lines), wave == 64)
        with self.assertRaises(ValueError):
            probe.kernel_lines(16)

    def test_the_selected_source_is_the_modified_one(self):
        lines = probe.kernel_lines(32)
        for k, (w, _, mod, side) in enumerate(probe.runs()):
            cnd = [ln for ln in lines if ln.startswith("v_cndmask_b32_e64")][k]
            src0, src1 = [t.strip() for t in cnd.split(",")[1:3]]
            with self.subTest(k=k):
                self.assertEqual("v1" in src0, side == "src0")
                self.assertEqual("v1" in src1, side == "src1")

    def test_controls_tell_every_mode_apart(self):
        seen = {tuple(probe.control_expected(n, i, d) for n, _, _ in probe.CONTROLS) for i, d in probe.MODES}
        self.assertEqual(len(seen), len(probe.MODES))
        with self.assertRaises(ValueError):
            probe.control_expected("other", 0, 0)


@unittest.skipUnless(os.environ.get("LEONIDA_AMD_CLANG") and shutil.which(
    os.path.join(os.path.dirname(os.environ.get("LEONIDA_AMD_CLANG", "")), "llvm-mc")),
    "needs LEONIDA_AMD_CLANG with llvm-mc beside it")
class LlvmMcAssembles(unittest.TestCase):
    """Every kernel body assembles for gfx1030 (in GFX10 spelling) and gfx1101, in both wave sizes."""

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
                    self.assertEqual(r.stdout.count("v_cndmask_b32_e64"), len(probe.runs()))


class Judging(unittest.TestCase):
    def test_sign_bit_moves_with_their_mode_pass(self):
        for ieee, den in probe.MODES:
            with self.subTest(ieee=ieee, denorm32=den):
                entry = probe.judge(32, ieee, den, _words(ieee, den))
                self.assertEqual(entry["counts"]["bit_move"], len(probe.runs()))
                self.assertTrue(entry["controls_ok"])
                self.assertTrue(probe.report_ok([entry]))

    def test_float_behaviour_is_counted_and_fails(self):
        entry = probe.judge(64, 1, 0, _words(1, 0, quiet_snan=True, flush_denormals=True))
        self.assertEqual(entry["counts"]["quieted"], 3 * len(probe.MODS) * len(probe.SIDES))
        self.assertEqual(entry["counts"]["flushed"], 3 * len(probe.MODS) * len(probe.SIDES))
        self.assertFalse(probe.report_ok([entry]))
        self.assertIn("quieted", probe.describe(entry))

    def test_a_control_that_does_not_show_its_mode_fails(self):
        for i in range(len(probe.CONTROLS)):
            with self.subTest(control=i):
                entry = probe.judge(32, 0, 3, _words(0, 3, control_off=i))
                self.assertFalse(entry["controls_ok"])
                self.assertFalse(probe.report_ok([entry]))
                self.assertIn("MODE NOT SHOWN", probe.describe(entry))

    def test_a_short_result_is_a_harness_error(self):
        with self.assertRaises(RuntimeError):
            probe.judge(32, 0, 0, _words(0, 0)[:-1])


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

    def test_gpus_that_cannot_set_the_mode_skip_with_77_and_close_the_oracle(self):
        for family, mode_skip in (("gfx12", None), (None, None), ("gfx11", "ieee cannot be set on gfx1170")):
            oracle = types.SimpleNamespace(arch="gfx1201", close=mock.Mock())
            modules, _ = _fake_hw_oracle(open_side_effect=lambda: oracle, family=family, mode_skip=mode_skip)
            with self.subTest(family=family, mode_skip=mode_skip):
                rc, _, err = self.run_main([], modules)
                self.assertEqual(rc, 77)
                self.assertIn("SKIP", err)
                oracle.close.assert_called_once()

    def test_run_writes_the_report_and_exit_code_follows_it(self):
        def oracle_for(**kw):
            return types.SimpleNamespace(
                arch="gfx1101", close=mock.Mock(),
                run=lambda ks: [_words(k[1]["mode"]["ieee"], k[1]["mode"]["denorm32"], **kw) for k in ks])
        good = oracle_for()
        modules, _ = _fake_hw_oracle(open_side_effect=lambda: good)
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, "sub", "report.json")
            rc, stdout, _ = self.run_main(["--waves", "32", "--json", out], modules)
            self.assertEqual(rc, 0, stdout)
            self.assertIn("every source modifier is a sign-bit move", stdout)
            self.assertEqual(stdout.count("120/120 sign-bit moves"), len(probe.MODES))
            with open(out, "rb") as f:
                raw = f.read()
            self.assertNotIn(b"\r\n", raw)
            data = json.loads(raw)
            self.assertEqual((data["arch"], data["ok"], len(data["kernels"])), ("gfx1101", True, len(probe.MODES)))
        good.close.assert_called_once()
        bad = oracle_for(quiet_snan=True)
        modules, _ = _fake_hw_oracle(open_side_effect=lambda: bad)
        rc, stdout, _ = self.run_main(["--waves", "64"], modules)
        self.assertEqual(rc, 1)
        self.assertIn("NOT every result", stdout)

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
