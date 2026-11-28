"""Unit tests for tools/div_scale_probe.py (case matrix, candidate readings, kernels, simulated oracle, CLI).

Standard library only; no GPU and no game files. The probe has not been run on hardware: these tests check what can
be checked without it. The case matrix and its boundaries must be what the docstring says. The generated kernels must
assemble (with llvm-mc where it is installed). The candidate references must reproduce the contradictions they were
written to tell apart. And the words of a run must land in the right rows (a simulated oracle that executes the
kernels' v_div_scale lines with one candidate, and hand-placed words).
"""
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from fractions import Fraction
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
sys.dont_write_bytecode = True
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import div_scale_probe as pr  # noqa: E402
from hw_oracle import asm, runner  # noqa: E402

F32, F64 = pr.FMTS["f32"], pr.FMTS["f64"]
_REG = re.compile(r"^(-?)v(?:(\d+)|\[(\d+):(\d+)\])$")


def regs_of(token):
    """(negated, [register numbers]) of a VGPR operand such as v4, -v[2:3]."""
    m = _REG.match(token)
    if not m:
        raise ValueError(token)
    if m.group(2):
        return bool(m.group(1)), [int(m.group(2))]
    return bool(m.group(1)), list(range(int(m.group(3)), int(m.group(4)) + 1))


class FakeOracle:
    """Simulated hw_oracle backend: executes the probe kernels' seeds, presets, v_div_scale, copies and stores.

    v_div_scale follows the candidate `reading`, with neg source modifiers applied and clamp/omod ignored, and writes
    the flag to bit 0 of the mask with every other bit cleared. v_div_fixup writes the correctly rounded quotient of
    its denominator and numerator registers. The remaining arithmetic of the divide is not executed."""

    arch = "gfx1036"
    device = "fake gfx1036"

    def __init__(self, reading="isa_recip_width", arch=None, wave64_flips=False, sgpr_flips=False, fail=None,
                 skip_d_write=False):
        self.reading, self.wave64_flips, self.sgpr_flips, self.fail = reading, wave64_flips, sgpr_flips, fail
        self.skip_d_write = skip_d_write
        self.arch = arch or self.arch
        self.closed = False

    def run(self, kernels):
        if self.fail:
            return [self.fail for _ in kernels]
        return [self.execute(k) for k in kernels]

    def execute(self, kernel):
        v, s, words = {}, {}, [0xDEAD0000] * kernel.words
        pairs = {"vcc": ("vcc_lo", "vcc_hi"), "s[10:11]": ("s10", "s11"), "s[12:13]": ("s12", "s13")}
        for line in kernel.lines:
            op, _, rest = line.partition(" ")
            args = [a.strip() for a in rest.split(",")]
            if op == "v_mov_b32_e32":
                src = args[1]
                v[int(args[0][1:])] = s.get(src, 0) if not src[0].isdigit() and not src.startswith("-") \
                    else int(src, 0) & 0xFFFFFFFF
            elif op in ("s_mov_b32", "s_mov_b64"):
                for name in pairs.get(args[0], (args[0],)):
                    s[name] = 0xFFFFFFFF
            elif op.startswith("v_div_scale_"):
                f = pr.FMTS[op[-3:]]
                last = args[4].split()
                args[4] = last[0]
                ops = []
                for tok in args[2:5]:
                    negated, rr = regs_of(tok)
                    bits = sum(v.get(r, 0) << (32 * i) for i, r in enumerate(rr))
                    ops.append(pr.neg(f, bits) if negated else bits)
                d, flag = pr.READINGS[self.reading](f, *ops)
                if d == pr.NAN:
                    d = (f.emask << f.mant) | (1 << (f.mant - 1))
                if self.wave64_flips and kernel.wave == 64:
                    flag ^= 1
                if self.sgpr_flips and args[1].startswith("s1"):
                    flag ^= 1
                for i, r in enumerate(regs_of(args[0])[1]):
                    if not self.skip_d_write:
                        v[r] = (d >> (32 * i)) & 0xFFFFFFFF
                names = pairs.get(args[1], (args[1],))
                for i, name in enumerate(names):
                    s[name] = flag if i == 0 else 0
            elif op.startswith("v_div_fixup_"):
                f = pr.FMTS[op[-3:]]
                den = sum(v.get(r, 0) << (32 * i) for i, r in enumerate(regs_of(args[2])[1]))
                num = sum(v.get(r, 0) << (32 * i) for i, r in enumerate(regs_of(args[3])[1]))
                q = pr.divide(f, num, den)
                if q == pr.NAN:
                    q = (f.emask << f.mant) | (1 << (f.mant - 1))
                for i, r in enumerate(regs_of(args[0])[1]):
                    v[r] = (q >> (32 * i)) & 0xFFFFFFFF
            elif op == "global_store_dword":
                words[v[20] // 4] = v[int(args[1][1:])]
        return words

    def close(self):
        self.closed = True


def run_main(argv, oracle):
    out, err = io.StringIO(), io.StringIO()
    with mock.patch.object(runner, "open_oracle", return_value=oracle):
        with redirect_stdout(out), redirect_stderr(err):
            code = pr.main(argv)
    return code, out.getvalue(), err.getvalue()


class CaseMatrixTests(unittest.TestCase):
    def test_counts_are_the_docstring_counts(self):
        doc = pr.__doc__
        for f in (F32, F64):
            self.assertEqual(len(pr.pairs(f)), 49)
            self.assertEqual(len(pr.same_values(f)), 10)
            self.assertEqual(len(pr.scale_rows(f)), 108)
            self.assertEqual(len(pr.mod_rows(f)), 30)
            self.assertEqual(len({name for name, _, _ in pr.pairs(f)}), 49, "case names must be unique")
        for text in ("That is 49 pairs", "10 cases with\nS0 = S1 = S2", "108 rows per width", "30 rows per width",
                     "11 special pairs", "6 denormal pairs", "6 exponent-gap pairs", "8 reciprocal pairs",
                     "5 quotient pairs", "6 tiny-numerator pairs", "7 ordinary pairs"):
            self.assertIn(text, doc)
        groups = [11, 6, 6, 8, 5, 6, 7]
        self.assertEqual(sum(groups), 49)

    def test_exponent_gap_boundaries(self):
        for f, want in ((F32, [95, 96, 97, 95, 96, 97]), (F64, [767, 768, 769, 767, 768, 769])):
            gaps = [pr.exp_field(f, n) - pr.exp_field(f, d) for name, n, d in pr.pairs(f) if name.startswith("gap ")]
            self.assertEqual(gaps, want)

    def test_tiny_numerator_boundaries(self):
        for f, want in ((F32, [22, 22, 23, 23, 24, 24]), (F64, [52, 52, 53, 53, 54, 54])):
            fields = [pr.exp_field(f, n) for name, n, _ in pr.pairs(f) if name.startswith("field ")]
            self.assertEqual(fields, want)
            for name, n, _ in pr.pairs(f):
                if name.startswith("field "):
                    self.assertEqual(pr.value(f, n), Fraction(2) ** (pr.exp_field(f, n) - f.bias))

    def test_reciprocal_boundaries(self):
        for f, k in ((F32, 126), (F64, 1022)):
            dens = [d for name, n, d in pr.pairs(f) if name.startswith("1 / ") and ("2^" in name)
                    and not name.startswith("1 / -")]
            edge = Fraction(2) ** k
            self.assertEqual([pr.value(f, d) for d in dens][1], edge)
            below, at, above, top = (pr.value(f, d) for d in dens)
            self.assertLess(below, edge)
            self.assertGreater(above, edge)
            self.assertEqual(top, edge * 2)
            # neighbours, one unit in the last place away
            self.assertEqual(edge - below, edge * Fraction(1, 2 ** (f.mant + 1)))
            self.assertEqual(above - edge, edge * Fraction(1, 2 ** f.mant))
            # 1 / 2^k is the smallest normal; just above 2^k the reciprocal is denormal
            self.assertFalse(pr.is_denorm(f, pr.divide(f, pr.pow2(f, 0), dens[1])))
            self.assertTrue(pr.is_denorm(f, pr.divide(f, pr.pow2(f, 0), dens[2])))

    def test_quotient_boundaries(self):
        for f, k in ((F32, -126), (F64, -1022)):
            named = {name: (n, d) for name, n, d in pr.pairs(f)}
            mn = Fraction(2) ** k
            q = {name: pr.value(f, n) / pr.value(f, d) for name, (n, d) in named.items()
                 if pr.value(f, d) != 0 and not pr.is_nan(f, n) and not pr.is_inf(f, n)
                 and not pr.is_nan(f, d) and not pr.is_inf(f, d)}
            self.assertLess(q[f"below 2^{k + 1} / 2"], mn)
            self.assertEqual(q[f"2^{k + 1} / 2"], mn)
            self.assertGreater(q[f"above 2^{k + 1} / 2"], mn)
            self.assertEqual(q[f"2^{k} / 1"], mn)
            self.assertEqual(q[f"2^{k} / 2^30"], mn / 2 ** 30)

    def test_round_to_and_value_agree_with_struct(self):
        for x in (1.0, -2.5, 3e-5, 1e10, 2.0 ** -149, 2.0 ** -130, 3.4e38):
            self.assertEqual(pr.round_to(F32, Fraction(x)), pr.from_float(F32, x))
        for x in (1.0, -2.5, 3e-5, 1e300, 2.0 ** -1074, 2.0 ** -1050):
            self.assertEqual(pr.round_to(F64, Fraction(x)), pr.from_float(F64, x))
            self.assertEqual(pr.value(F64, pr.from_float(F64, x)), Fraction(x))
        self.assertEqual(pr.round_to(F32, Fraction(2) ** 200), 0x7F800000)


class CandidateTests(unittest.TestCase):
    """The contradictions the docs/shader_validation.md section and the coordination finding name, as the three
    references give them."""

    def test_zero_operand_flag(self):
        one = pr.pow2(F32, 0)
        for f in (F32, F64):
            o = pr.pow2(f, 0)
            self.assertEqual(pr.isa_newer(f, o, 0, o), (pr.NAN, 0))
            self.assertEqual(pr.isa_recip_width(f, o, 0, o), (pr.NAN, 0))
            self.assertEqual(pr.rdna3_emu(f, o, 0, o), (pr.NAN, 1))
        self.assertEqual(pr.isa_newer(F32, 0, one, 0), (pr.NAN, 0))

    def test_smallest_normal_over_one(self):
        f, n, one = F32, pr.pow2(F32, -126), pr.pow2(F32, 0)
        self.assertEqual(pr.isa_newer(f, n, one, n), (pr.pow2(f, -62), 0))
        self.assertEqual(pr.isa_recip_width(f, n, one, n), (pr.pow2(f, -62), 0))
        self.assertEqual(pr.rdna3_emu(f, n, one, n), (pr.pow2(f, -62), 1))
        # The denominator call: the text scales 1.0 too (both scaled, flag clear).
        self.assertEqual(pr.isa_newer(f, one, one, n), (pr.pow2(f, 64), 0))

    def test_one_over_2_127(self):
        f, one, d = F32, pr.pow2(F32, 0), pr.pow2(F32, 127)
        self.assertEqual(pr.isa_newer(f, one, d, one), (pr.pow2(f, 64), 1))
        self.assertEqual(pr.isa_recip_width(f, one, d, one), (one, 1))
        self.assertEqual(pr.rdna3_emu(f, one, d, one), (one, 1))
        # Denominator call under the f32-width reading: 2^127 * 2^64 overflows.
        self.assertEqual(pr.isa_recip_width(f, d, d, one), (0x7F800000, 1))
        self.assertEqual(pr.isa_newer(f, d, d, one), (d, 1))

    def test_one_over_2_127_divide_is_inf_under_the_f32_width_reading(self):
        """The LLVM f32 divide fed each reading's v_div_scale results, through the translator's v_div_fmas_f32 and
        v_div_fixup_f32 (gfx1036-measured rules) on the CPU interpreter: +inf for the f32-width reading, 0 for the
        newer wording (2^-127 flushed under the game MODE)."""
        sys.path.insert(0, str(ROOT / "tests" / "semantic"))
        try:
            import support
        finally:
            sys.path.remove(str(ROOT / "tests" / "semantic"))
        f, one, d = F32, pr.pow2(F32, 0), pr.pow2(F32, 127)
        results = {}
        for c in ("isa_newer", "isa_recip_width"):
            den_s, _ = pr.READINGS[c](f, d, d, one)
            num_s, flag = pr.READINGS[c](f, one, d, one)
            lines = [f"v_mov_b32 v2, {d:#x}", f"v_mov_b32 v4, {one:#x}", f"v_mov_b32 v6, {den_s:#x}",
                     f"v_mov_b32 v7, {num_s:#x}", f"s_mov_b32 vcc_lo, {flag}"]
            lines += [ln for ln in pr.sequence_lines(f, 32) if not ln.startswith("v_div_scale")]
            lines += ["v_mov_b32 v0, v8", "exp mrt0 v0, v1, v2, v3 done vm", "s_endpgm"]
            results[c] = support.mrt0_words(support.translate_lines("ps_div_scale_seq", lines))[0]
        self.assertEqual(results["isa_recip_width"], 0x7F800000)
        self.assertEqual(results["isa_newer"], 0x00000000)

    def test_denormal_denominator(self):
        f, one, dmin = F32, pr.pow2(F32, 0), 1
        self.assertEqual(pr.rdna3_emu(f, dmin, dmin, one), (pr.NAN, 1))
        self.assertEqual(pr.isa_newer(f, dmin, dmin, one), (pr.ldexp(f, dmin, 64), 1))

    def test_rdna3_emu_f64_tiny_numerator_keeps_the_text(self):
        f, one = F64, pr.pow2(F64, 0)
        n = pr.pow2(f, 53 - 1023)
        self.assertEqual(pr.rdna3_emu(f, n, one, n), (pr.ldexp(f, n, 128), 0))
        n32 = pr.pow2(F32, 23 - 127)
        self.assertEqual(pr.rdna3_emu(F32, n32, pr.pow2(F32, 0), n32)[1], 1)

    def test_ordinary_operands_pass_through(self):
        for f in (F32, F64):
            for c in pr.CANDIDATES:
                for n, d in ((3, 7), (1, 4), (-5.5, 1.25)):
                    nb, db = pr.round_to(f, Fraction(n)), pr.round_to(f, Fraction(d))
                    self.assertEqual(pr.READINGS[c](f, db, db, nb), (db, 0), (f.name, c, n, d))
                    self.assertEqual(pr.READINGS[c](f, nb, db, nb), (nb, 0), (f.name, c, n, d))

    def test_self_test(self):
        out = io.StringIO()
        with redirect_stdout(out):
            self.assertEqual(pr.main(["--self-test"]), 0)
        self.assertIn("self-test ok", out.getvalue())


class KernelTests(unittest.TestCase):
    def test_kernel_shapes(self):
        kernels, meta = pr.build_kernels(asm, list(pr.MODES), [32, 64], list(pr.WIDTHS))
        self.assertEqual(len(kernels), 2 * 5 * 2 * 3)
        self.assertEqual(meta[0], (32, "game_i0_d3", "f32", "scale"))
        for k, (wave, mode, width, section) in zip(kernels, meta):
            self.assertTrue(k.raw)
            self.assertEqual(k.threads, 1)
            self.assertEqual(k.mode, pr.MODES[mode])
            self.assertEqual(k.lines[-2:], ["s_waitcnt vmcnt(0)", "s_endpgm"])
            stores = sum(1 for ln in k.lines if ln.startswith("global_store_dword"))
            self.assertEqual(stores, k.words)
            f = pr.FMTS[width]
            per_row = f.words + (1 if wave == 32 else 2)
            want = {"scale": 2 * 108 * per_row, "mods": 30 * per_row, "seq": 49 * f.words}[section]
            self.assertEqual(k.words, want)
            self.assertLessEqual(k.words, 16384)

    def test_mask_destinations(self):
        lines, _ = pr.scale_kernel_lines(F32, 64)
        text = "\n".join(lines)
        self.assertIn("v_div_scale_f32 v8, vcc, v2, v2, v4", text)
        self.assertIn("v_div_scale_f32 v8, s[10:11], v4, v2, v4", text)
        self.assertIn("v_mov_b32_e32 v21, vcc_hi", text)
        lines, _ = pr.scale_kernel_lines(F64, 32)
        text = "\n".join(lines)
        self.assertIn("v_div_scale_f64 v[8:9], vcc_lo, v[2:3], v[2:3], v[4:5]", text)
        self.assertIn("v_div_scale_f64 v[8:9], s10, v[4:5], v[2:3], v[4:5]", text)
        self.assertNotIn("|", text)          # VOP3B has no abs

    def test_kernels_assemble_when_llvm_mc_available(self):
        llvm_mc = os.environ.get("LLVM_MC") or shutil.which("llvm-mc")
        if not llvm_mc or not os.path.isfile(llvm_mc):
            self.skipTest("llvm-mc not found")
        for wave in (32, 64):
            kernels, _ = pr.build_kernels(asm, list(pr.MODES), [wave], list(pr.WIDTHS))
            text, _, _ = asm.module_text(kernels, "gfx1036", wave=wave)
            flags = ["-mattr=+wavefrontsize64"] if wave == 64 else []
            res = subprocess.run(
                [llvm_mc, "-triple=amdgcn-amd-amdhsa", "-mcpu=gfx1036", *flags, "-filetype=obj", "-o", os.devnull],
                input=text, capture_output=True, text=True)
            self.assertEqual(res.returncode, 0, res.stderr[:800])


class ParsingTests(unittest.TestCase):
    def test_hand_placed_words_land_in_their_rows(self):
        meta = [(32, "game_i0_d3", "f32", "scale"), (64, "game_i0_d3", "f64", "scale"),
                (32, "game_i0_d3", "f32", "seq")]
        f32_words = [0] * (2 * 108 * 2)
        f32_words[0:2] = [0x12345678, 0xFFFFFFFE]        # row 0: VCC, "1 / +0", den call; flag 0, other lanes set
        f32_words[108 * 2:108 * 2 + 2] = [0x0BADF00D, 1]  # first SGPR row
        f64_words = [0] * (2 * 108 * 4)
        f64_words[4:8] = [0x11111111, 0x22222222, 1, 0x80000000]   # row 1: "1 / +0", num call, wave64
        seq_words = [0] * 49
        seq_words[48] = 0xBF800000
        rows, seq_rows, summary = pr.evaluate(meta, [f32_words, f64_words, {"words": seq_words}])
        r0 = rows[0]
        self.assertEqual((r0["case"], r0["call"], r0["dst"], r0["d"], r0["mask"]),
                         ("1 / +0", "den", "vcc", 0x12345678, [0xFFFFFFFE]))
        r_sgpr = rows[108]
        self.assertEqual((r_sgpr["dst"], r_sgpr["case"], r_sgpr["d"], r_sgpr["mask"]),
                         ("sgpr", "1 / +0", 0x0BADF00D, [1]))
        r64 = [r for r in rows if r["width"] == "f64"][1]
        self.assertEqual((r64["call"], r64["d"], r64["mask"]), ("num", 0x2222222211111111, [1, 0x80000000]))
        self.assertEqual((seq_rows[-1]["case"], seq_rows[-1]["q"]), (pr.pairs(F32)[-1][0], 0xBF800000))
        # 1 / -2^127 correctly rounded is -2^-127, a denormal: -1.0 is no reading's answer.
        self.assertEqual(seq_rows[-1]["verdict"], "other")
        self.assertIn("game_i0_d3 f32", summary["candidates"])

    def test_harness_string_result_is_an_error(self):
        with self.assertRaises(RuntimeError):
            pr.evaluate([(32, "game_i0_d3", "f32", "seq")], ["assembler rejected: x"])


class ProbeCliTests(unittest.TestCase):
    def test_device_following_one_reading(self):
        with tempfile.TemporaryDirectory() as work:
            report = os.path.join(work, "nested", "probe.json")
            oracle = FakeOracle(reading="isa_recip_width")
            code, out, err = run_main(["--json", report], oracle)
            self.assertEqual((code, err), (0, ""), out)
            self.assertTrue(oracle.closed)
            self.assertIn("wave_diffs=0, dst_diffs=0", out)
            for mode in pr.MODES:
                for width in pr.WIDTHS:
                    line = next(ln for ln in out.splitlines() if ln.startswith(f"{mode} {width}: "))
                    m = re.search(r"isa_recip_width (\d+)/(\d+)", line)
                    self.assertEqual(m.group(1), m.group(2), line)
            # the f32 readings part ways; for f64 the two text readings are the same rule
            f32_line = next(ln for ln in out.splitlines() if ln.startswith("game_i0_d3 f32: "))
            m = re.search(r"isa_newer (\d+)/(\d+)", f32_line)
            self.assertLess(int(m.group(1)), int(m.group(2)))
            f64_line = next(ln for ln in out.splitlines() if ln.startswith("game_i0_d3 f64: "))
            m = re.search(r"isa_newer (\d+)/(\d+)", f64_line)
            self.assertEqual(m.group(1), m.group(2))
            self.assertIn("isa_newer MISMATCH game_i0_d3 f32", out)
            with open(report, "rb") as fh:
                raw = fh.read()
            self.assertNotIn(b"\r\n", raw)
            data = json.loads(raw.decode("utf-8"))
            self.assertEqual(data["arch"], "gfx1036")
            self.assertEqual(data["modes"], list(pr.MODES))
            self.assertEqual(data["candidates"], list(pr.CANDIDATES))
            self.assertEqual(len(data["rows"]), 2 * 5 * 2 * (2 * 108 + 30))
            self.assertEqual(len(data["sequence"]), 2 * 5 * 2 * 49)
            row = data["rows"][0]
            self.assertNotIn("mask", row)
            self.assertEqual((row["mask_lo"], row["mask_hi"], row["d_unwritten"]), ("00000000", None, False))
            wave64 = next(r for r in data["rows"] if r["wave"] == 64)
            self.assertEqual((wave64["mask_lo"], wave64["mask_hi"]), ("00000000", "00000000"))
            self.assertEqual(set(row["candidates"]), set(pr.CANDIDATES))
            self.assertTrue(row["candidates"]["isa_recip_width"]["match"])
            seq = data["summary"]["sequence"]["game_i0_d3 f32"]
            self.assertEqual(seq["other"], 0)

    def test_table_prints_every_row(self):
        code, out, _ = run_main(["--table", "--waves", "32", "--modes", "game_i0_d3", "--widths", "f32"],
                                FakeOracle())
        self.assertEqual(code, 0)
        self.assertEqual(sum(1 for ln in out.splitlines() if ln.startswith("w32 game_i0_d3 f32 ")), 2 * 108 + 30)

    def test_wave_and_destination_differences_exit_1(self):
        code, out, _ = run_main(["--modes", "game_i0_d3"], FakeOracle(wave64_flips=True))
        self.assertEqual(code, 1)
        self.assertIn("WAVE DIFF", out)
        code, out, _ = run_main(["--modes", "game_i0_d3", "--waves", "32"], FakeOracle(sgpr_flips=True))
        self.assertEqual(code, 1)
        self.assertIn("DST DIFF", out)

    def test_harness_error_exits_2(self):
        code, out, _ = run_main(["--modes", "game_i0_d3"], FakeOracle(fail="assembler rejected: bad"))
        self.assertEqual(code, 2)
        self.assertIn("harness error", out)

    def test_unavailable_or_non_gfx10_oracle_exits_77(self):
        with mock.patch.object(runner, "open_oracle", side_effect=runner.Unavailable("no device")):
            out = io.StringIO()
            with redirect_stdout(out):
                self.assertEqual(pr.main([]), 77)
            self.assertIn("hardware oracle unavailable", out.getvalue())
        code, out, _ = run_main([], FakeOracle(arch="gfx1100"))
        self.assertEqual((code, out.strip()), (77, "hardware oracle unavailable: gfx1100 is not GFX10"))

    def test_unknown_entries_are_cli_errors(self):
        for argv in (["--modes", "game_i0_d3,nope"], ["--widths", "f16"], ["--waves", "16"]):
            with self.assertRaises(SystemExit) as caught:
                with redirect_stderr(io.StringIO()):
                    pr.main(argv)
            self.assertEqual(caught.exception.code, 2)


# The tables below are written by hand from LLVM's SIISelLowering.cpp, LowerFDIV32 and LowerFDIV64 (the node names are
# LLVM's), not read from the tool: the order of the instructions and the order of their operands are what a wrong
# kernel would get wrong. {junk} is the SGPR(s) of the denominator call's unused flag, {vcc} the numerator call's.
LLVM_F32 = [
    "v_div_scale_f32 v6, {junk}, v2, v2, v4",     # DenominatorScaled = DIV_SCALE {RHS, RHS, LHS}
    "v_div_scale_f32 v7, {vcc}, v4, v2, v4",      # NumeratorScaled   = DIV_SCALE {LHS, RHS, LHS}; its flag feeds DIV_FMAS
    "v_rcp_f32 v10, v6",                          # ApproxRcp = RCP DenominatorScaled
    "v_fma_f32 v11, -v6, v10, 1.0",               # Fma0 = fma(NegDivScale0, ApproxRcp, 1.0)
    "v_fmac_f32 v10, v11, v10",                   # Fma1 = fma(Fma0, ApproxRcp, ApproxRcp)  (v10 = v11 * v10 + v10)
    "v_mul_f32 v11, v7, v10",                     # Mul  = NumeratorScaled * Fma1
    "v_fma_f32 v12, -v6, v11, v7",                # Fma2 = fma(NegDivScale0, Mul, NumeratorScaled)
    "v_fmac_f32 v11, v12, v10",                   # Fma3 = fma(Fma2, Fma1, Mul)  (v11 = v12 * v10 + v11)
    "v_fma_f32 v12, -v6, v11, v7",                # Fma4 = fma(NegDivScale0, Fma3, NumeratorScaled)
    "v_div_fmas_f32 v12, v12, v10, v11",          # Fmas = DIV_FMAS {Fma4, Fma1, Fma3, Scale}
    "v_div_fixup_f32 v8, v12, v2, v4",            # DIV_FIXUP {Fmas, RHS, LHS}
]
LLVM_F64 = [
    "v_div_scale_f64 v[6:7], {junk}, v[2:3], v[2:3], v[4:5]",   # DivScale0 = DIV_SCALE {Y, Y, X}
    "v_rcp_f64 v[10:11], v[6:7]",                                # Rcp = RCP DivScale0.0
    "v_fma_f64 v[12:13], -v[6:7], v[10:11], 1.0",                # Fma0 = fma(NegDivScale0, Rcp, One)
    "v_fma_f64 v[10:11], v[10:11], v[12:13], v[10:11]",          # Fma1 = fma(Rcp, Fma0, Rcp)
    "v_fma_f64 v[12:13], -v[6:7], v[10:11], 1.0",                # Fma2 = fma(NegDivScale0, Fma1, One)
    "v_div_scale_f64 v[14:15], {vcc}, v[4:5], v[2:3], v[4:5]",   # DivScale1 = DIV_SCALE {X, Y, X}; its flag feeds DIV_FMAS
    "v_fma_f64 v[10:11], v[10:11], v[12:13], v[10:11]",          # Fma3 = fma(Fma1, Fma2, Fma1)
    "v_mul_f64 v[12:13], v[14:15], v[10:11]",                    # Mul = DivScale1.0 * Fma3
    "v_fma_f64 v[16:17], -v[6:7], v[12:13], v[14:15]",           # Fma4 = fma(NegDivScale0, Mul, DivScale1.0)
    "v_div_fmas_f64 v[16:17], v[16:17], v[10:11], v[12:13]",     # Fmas = DIV_FMAS {Fma4, Fma3, Mul, Scale}
    "v_div_fixup_f64 v[8:9], v[16:17], v[2:3], v[4:5]",          # DIV_FIXUP {Fmas, Y, X}
]


class SequenceTests(unittest.TestCase):
    def test_sequence_is_llvm_s_instruction_for_instruction(self):
        for f, table in ((F32, LLVM_F32), (F64, LLVM_F64)):
            for wave, junk, vcc in ((32, "s12", "vcc_lo"), (64, "s[12:13]", "vcc")):
                with self.subTest(width=f.name, wave=wave):
                    want = [ln.format(junk=junk, vcc=vcc) for ln in table]
                    self.assertEqual(pr.sequence_lines(f, wave), want)

    def test_the_sequence_divides_ordinary_operands(self):
        """Run the sequence on exact arithmetic with the ordinary-operand rule of v_div_scale (D = S0, flag clear) and
        check that it computes the correctly rounded quotient: a wrong operand order in the FMAs or in v_div_fmas /
        v_div_fixup would not."""
        import struct

        def rnd(f, x):
            x = float(x)
            if f.bits == 32:
                x = struct.unpack("<f", struct.pack("<f", x))[0]
            return Fraction(x)

        def reg(token):
            neg = token.startswith("-")
            token = token.lstrip("-")
            if token == "1.0":
                return None, neg
            return int(re.match(r"v\[?(\d+)", token).group(1)), neg

        for f, table in ((F32, LLVM_F32), (F64, LLVM_F64)):
            lines = [ln.format(junk="s12", vcc="vcc_lo") for ln in table]
            for n, d in ((3, 7), (1, 3), (-11, 8), (10, 3), (355, 113), (1, 10)):
                regs = {2: Fraction(d), 4: Fraction(n)}

                def val(token):
                    base, neg = reg(token)
                    x = Fraction(1) if base is None else regs[base]
                    return -x if neg else x
                for ln in lines:
                    op, _, rest = ln.partition(" ")
                    a = [t.strip() for t in rest.split(",")]
                    dst = reg(a[0])[0]
                    if op.startswith("v_div_scale"):
                        regs[dst] = val(a[2])
                    elif op.startswith("v_rcp"):
                        regs[dst] = rnd(f, 1 / val(a[1]))
                    elif op.startswith(("v_fma_", "v_div_fmas")):
                        regs[dst] = rnd(f, val(a[1]) * val(a[2]) + val(a[3]))
                    elif op.startswith("v_fmac"):
                        regs[dst] = rnd(f, val(a[1]) * val(a[2]) + regs[dst])
                    elif op.startswith("v_mul"):
                        regs[dst] = rnd(f, val(a[1]) * val(a[2]))
                    elif op.startswith("v_div_fixup"):
                        regs[dst] = val(a[1])
                    else:
                        self.fail(ln)
                with self.subTest(width=f.name, n=n, d=d):
                    self.assertEqual(regs[8], rnd(f, Fraction(n, d)))


class SentinelAndMatrixTests(unittest.TestCase):
    def test_sentinel_values(self):
        self.assertEqual(pr.SENTINEL, {"f32": 0x5EADBEEF, "f64": 0x5EADBEEFCAFEF00D})

    def test_the_sentinel_is_no_operand_and_no_prediction(self):
        for f in (F32, F64):
            self.assertEqual(pr.sentinel_conflicts(f), [])
        # the two words of the f64 sentinel differ, so a half-written D shows
        self.assertNotEqual(pr.SENTINEL["f64"] & 0xFFFFFFFF, pr.SENTINEL["f64"] >> 32)

    def test_every_div_scale_follows_the_mask_preset_and_the_d_sentinel(self):
        want = {"f32": ["v_mov_b32_e32 v8, 0x5eadbeef"],
                "f64": ["v_mov_b32_e32 v8, 0xcafef00d", "v_mov_b32_e32 v9, 0x5eadbeef"]}
        seen = 0
        for f in (F32, F64):
            for wave in (32, 64):
                for make in (pr.scale_kernel_lines, pr.mod_kernel_lines):
                    lines, _ = make(f, wave)
                    for i, ln in enumerate(lines):
                        if not ln.startswith("v_div_scale_"):
                            continue
                        sdst = ln.split(",")[1].strip()
                        preset = f"s_mov_b32 {sdst}, -1" if wave == 32 else f"s_mov_b64 {sdst}, -1"
                        self.assertEqual(lines[i - 1], preset, ln)
                        k = len(want[f.name])
                        self.assertEqual(lines[i - 1 - k:i - 1], want[f.name], ln)
                        seen += 1
        # 2 widths x 2 waves x (2 x 108 + 30) rows
        self.assertEqual(seen, 2 * 2 * (2 * 108 + 30))

    def test_the_pair_that_isolates_the_denormal_denominator_branch(self):
        for f, mid_exp in ((F32, -60), (F64, -284)):
            name, n, d = next(p for p in pr.pairs(f) if p[0].startswith("2^") and p[0].endswith("/ min denorm"))
            self.assertEqual(name, f"2^{mid_exp} / min denorm")
            self.assertEqual((n, d), (pr.pow2(f, mid_exp), 1))
            self.assertLess(pr.exp_field(f, n) - pr.exp_field(f, d), f.gap)       # below 96 [768]: no gap branch
            self.assertGreater(pr.exp_field(f, n), f.tiny + 1)                    # not the tiny-numerator branch
            # the text scales S0 and clears the flag; the emulator rule gives a NaN with the flag clear
            for c in ("isa_newer", "isa_recip_width"):
                self.assertEqual(pr.READINGS[c](f, n, d, n), (pr.ldexp(f, n, f.scale), 0))
                self.assertEqual(pr.READINGS[c](f, d, d, n), (pr.ldexp(f, d, f.scale), 0))
            self.assertEqual(pr.rdna3_emu(f, n, d, n), (pr.NAN, 0))
            self.assertEqual(pr.rdna3_emu(f, d, d, n), (pr.NAN, 0))


class ReportTests(unittest.TestCase):
    def test_an_unwritten_d_is_reported_not_taken_for_a_value(self):
        with tempfile.TemporaryDirectory() as work:
            report = os.path.join(work, "p.json")
            argv = ["--json", report, "--modes", "game_i0_d3", "--waves", "32", "--widths", "f32,f64"]
            code, out, _ = run_main(argv, FakeOracle(skip_d_write=True))
            self.assertEqual(code, 0, out)
            self.assertIn(f"d_unwritten={2 * (2 * 108 + 30)} ", out)
            self.assertIn("D UNWRITTEN w32 game_i0_d3 f32 vcc 1 / +0 (den)", out)
            with open(report, encoding="utf-8") as fh:
                data = json.load(fh)
            self.assertTrue(all(r["d_unwritten"] for r in data["rows"]))
            self.assertEqual({r["d"] for r in data["rows"] if r["width"] == "f32"}, {"5eadbeef"})
            self.assertEqual({r["d"] for r in data["rows"] if r["width"] == "f64"}, {"5eadbeefcafef00d"})
            self.assertEqual(data["summary"]["d_unwritten"], len(data["rows"]))
            # no candidate predicts the sentinel
            self.assertFalse(any(c["match"] for r in data["rows"] if "candidates" in r for c in r["candidates"].values()))
        code, out, _ = run_main(["--modes", "game_i0_d3", "--waves", "32"], FakeOracle())
        self.assertIn("d_unwritten=0 ", out)
        self.assertNotIn("D UNWRITTEN", out)

    def test_the_summary_says_when_it_is_not_the_measurement(self):
        note = "this summary is not the measurement. Re-run with --json <file> and post that file."
        code, out, _ = run_main(["--modes", "game_i0_d3", "--waves", "32"], FakeOracle())
        self.assertIn(note, out)
        with tempfile.TemporaryDirectory() as work:
            code, out, _ = run_main(["--json", os.path.join(work, "p.json"), "--modes", "game_i0_d3", "--waves", "32"],
                                    FakeOracle())
        self.assertNotIn(note, out)

    def test_the_f32_sequence_tallies_are_labeled_outside_the_denormal_keeping_mode(self):
        code, out, _ = run_main(["--waves", "32"], FakeOracle())
        lines = {ln.split(" divide sequence")[0]: ln for ln in out.splitlines() if " divide sequence:" in ln}
        for group in ("game_i0_d3 f32", "i0_d0 f32", "i1_d3 f32", "i1_d0 f32"):
            self.assertIn("not comparable with LLVM's lowering", lines[group], group)
        for group in ("i0_d3_f32keep f32", "game_i0_d3 f64", "i0_d0 f64", "i0_d3_f32keep f64"):
            self.assertNotIn("not comparable", lines[group], group)
        self.assertIn("comparable with LLVM's lowering only in i0_d3_f32keep", out)
        rows, seq_rows, summary = pr.evaluate([(32, "i0_d3_f32keep", "f32", "seq"), (32, "i1_d0", "f32", "seq"),
                                               (32, "i1_d0", "f64", "seq")], [[0] * 49, [0] * 49, [0] * 98])
        self.assertEqual({k: t["llvm_comparable"] for k, t in summary["sequence"].items()},
                         {"i0_d3_f32keep f32": True, "i1_d0 f32": False, "i1_d0 f64": True})

    def test_the_table_has_a_legend_and_the_json_names_its_mask_words(self):
        code, out, _ = run_main(["--table", "--waves", "32", "--modes", "game_i0_d3", "--widths", "f32"], FakeOracle())
        first = out.splitlines()[1]                   # line 0 is the oracle banner
        self.assertTrue(first.startswith("# columns: wave mode width mask-destination call modifier case D="), first)
        self.assertIn("without 0x, high word first", first)
        self.assertIn("mask_lo", first)

    def test_a_flushed_divide_is_classified_cr_ftz(self):
        names = [p[0] for p in pr.pairs(F32)]
        below, last, inf_row, nan_row = (names.index(n) for n in ("1 / 2^127", "1 / -2^127", "1 / +0", "qnan / 1"))
        words = [0] * 49
        words[below] = 0x00000000            # correctly rounded 2^-127 = 0x00400000, a denormal: flushed to +0
        words[last] = 0x80000000             # correctly rounded -2^-127 (denormal): flushed to -0
        words[inf_row] = 0x7F800000          # +inf, as is
        words[nan_row] = 0x7FC00000
        rows, seq_rows, summary = pr.evaluate([(32, "i0_d0", "f32", "seq")], [words])
        verdict = {r["case"]: r["verdict"] for r in seq_rows}
        self.assertEqual((verdict["1 / 2^127"], verdict["1 / -2^127"], verdict["1 / +0"], verdict["qnan / 1"]),
                         ("cr_ftz", "cr_ftz", "cr", "nan"))
        tally = summary["sequence"]["i0_d0 f32"]
        self.assertGreaterEqual(tally["cr_ftz"], 2)    # the other zero words happen to be right for other rows
        self.assertEqual(tally["cr"] + tally["cr_ftz"] + tally["nan"] + tally["other"], 49)
        # an unflushed denormal is cr, and the wrong sign of zero is neither
        words[below], words[last] = 0x00400000, 0x00000000
        _, seq_rows, _ = pr.evaluate([(32, "i0_d0", "f32", "seq")], [words])
        verdict = {r["case"]: r["verdict"] for r in seq_rows}
        self.assertEqual((verdict["1 / 2^127"], verdict["1 / -2^127"]), ("cr", "other"))


if __name__ == "__main__":
    unittest.main()
