"""tools/cvt_diff.py: the rounding and normalizing conversion reference against the words measured on RDNA2 hardware
and the semantic tests' references, the generator's encoding rules, the sweep and edge grid, and short seeded
differential runs. Standard library only; no GPU, no game files.

The family is v_cvt_flr_i32_f32, v_cvt_rpi_i32_f32, v_cvt_norm_i16_f16, v_cvt_norm_u16_f16 (VOP1) and
v_cvt_pknorm_i16_f32, v_cvt_pknorm_u16_f32, v_cvt_pknorm_i16_f16, v_cvt_pknorm_u16_f16 (VOP3).
"""
import contextlib
import io
import itertools
import os
import random
import re
import sys
import tempfile
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import cvt_diff as c  # noqa: E402  (puts tests/semantic on sys.path)
import test_alu_cvt_flr_rpi as fr  # noqa: E402
import test_alu_cvt_norm_f16 as nf16  # noqa: E402
import test_alu_cvt_pknorm_f16 as pk16  # noqa: E402
import test_alu_cvt_pknorm_f32 as pk32  # noqa: E402  (the gfx1036-measured v_cvt_pknorm_*_f32 words)

M32 = 0xFFFFFFFF


def run(*body):
    """The reference's four exported words for a program."""
    return c.reference(list(body))


def is_denormal32(w):
    return w & 0x7F800000 == 0 and w & 0x007FFFFF


class Reference(unittest.TestCase):
    def test_measured_gfx1036_pknorm_f32_words(self):
        # tests/semantic/test_alu_cvt_pknorm_f32.py: near ties on both sides of k + 1/2, measured on gfx1036
        for op, a, b, want in pk32.MEASURED:
            with self.subTest(op=op, a=hex(a), b=hex(b)):
                self.assertEqual(run(f"v_mov_b32 v9, 0x{a:x}", f"v_mov_b32 v10, 0x{b:x}", f"{op} v8, v9, v10")[0],
                                 want)

    def test_f16_normalize_matches_the_semantic_reference_for_every_half(self):
        for snorm in (True, False):
            bad = [h for h in range(1 << 16) if c.normalize(c.f16_value(h), snorm) != nf16.ref_norm16(h, snorm)]
            self.assertEqual(bad, [], f"snorm={snorm}")
        for h in sorted(set(pk16.EDGE_F16) | set(c.F16_EDGES) | set(c.F16_TIES)):
            for snorm in (True, False):
                self.assertEqual(c.normalize(c.f16_value(h), snorm), pk16.ref_norm16_f16(h, snorm), hex(h))

    def test_f32_normalize_matches_the_semantic_reference(self):
        for w in pk32.VALUES + list(c.F32_EDGES + c.F32_TIES):
            for snorm in (True, False):
                self.assertEqual(c.normalize(c.f32_value(w), snorm), pk32.norm16(w, snorm), hex(w))

    def test_flr_rpi_match_the_semantic_reference_off_the_denormals(self):
        for w in fr.VALUES + [w for w in c.F32_EDGES + c.F32_TIES if not is_denormal32(w)]:
            for op, half_up in (("v_cvt_flr_i32_f32", False), ("v_cvt_rpi_i32_f32", True)):
                self.assertEqual(c.to_i32(w, half_up), fr.ref(op, w), f"{op} {w:#x}")

    def test_f32_denormal_reads_as_zero(self):
        # the translator's MODE (and gfx1036's flr(-1e-45) = 0); the semantic ref() keeps them, so it is not used here
        for w in (0x00000001, 0x80000001, 0x007FFFFF, 0x807FFFFF):
            self.assertEqual((c.to_i32(w, False), c.to_i32(w, True)), (0, 0), hex(w))

    def test_saturation_follows_the_sign_bit(self):
        for w, want in ((0x4F000000, 0x7FFFFFFF), (0xCF000000, 0x80000000), (0xCEFFFFFF, 0x80000080),
                        (0x4EFFFFFF, 0x7FFFFF80),
                        (0x7F800000, 0x7FFFFFFF), (0xFF800000, 0x80000000), (0x7FC00000, 0x7FFFFFFF),
                        (0xFFC00000, 0x80000000), (0x7F800001, 0x7FFFFFFF), (0xFF800001, 0x80000000)):
            self.assertEqual(c.to_i32(w, False), want, hex(w))
            self.assertEqual(c.to_i32(w, True), want, hex(w))

    def test_rpi_adds_one_half_exactly(self):
        for w, want in ((0x3EFFFFFF, 0), (0x3F000000, 1), (0xBF000000, 0), (0xBFC00000, M32), (0x40200000, 3),
                        (0x4AFFFFFF, 0x00800000), (0x4B000001, 0x00800001), (0x4B7FFFFF, 0x00FFFFFF)):
            self.assertEqual(c.to_i32(w, True), want, hex(w))

    def test_normalize_rounds_once_and_ties_to_even(self):
        self.assertEqual([c.normalize(c.f32_value(w), True) for w in (0x3F000000, 0xBF000000)], [0x4000, 0xC000])
        self.assertEqual(c.normalize(c.f32_value(0x3F000000), False), 0x8000)
        # 0.5 + 2^-11 and 0.5 + 2^-10: the binary32 product lands on k + 1/2 and a second rounding would go up
        self.assertEqual([c.normalize(c.f16_value(h), True) for h in (0x3801, 0x3802)], [0x400F, 0x401F])
        self.assertEqual([c.normalize(c.f16_value(h), False) for h in (0x3801, 0x3802, 0x3803, 0x3804)],
                         [0x801F, 0x803F, 0x805F, 0x807F])
        self.assertEqual(c.normalize(c.f16_value(0x03FF), True), 2)                 # f16 denormals are values
        self.assertEqual([c.normalize(c.f16_value(h), True) for h in (0x7C00, 0xFC00, 0x7E00, 0xFE00)],
                         [0x7FFF, 0x8001, 0, 0])
        self.assertEqual([c.normalize(c.f16_value(h), False) for h in (0x7C00, 0xFC00, 0xBC00)], [0xFFFF, 0, 0])

    def test_destination_halves(self):
        self.assertEqual(run("v_mov_b32 v8, 0x12340000", "v_mov_b32 v9, 0x3c00", "v_cvt_norm_u16_f16_e32 v8, v9")[0],
                         0x1234FFFF)                                                 # norm keeps D[31:16]
        self.assertEqual(run("v_mov_b32 v9, 0x3f800000", "v_mov_b32 v10, 0xbf800000",
                             "v_cvt_pknorm_i16_f32 v8, v9, v10")[0], 0x80017FFF)      # S0 low, S1 high
        self.assertEqual(run("v_mov_b32 v9, 0xbc003c00", "v_cvt_pknorm_i16_f16 v8, v9, v9 op_sel:[1,0,0]")[0],
                         0x7FFF8001)
        self.assertEqual(run("v_mov_b32 v9, 0xbc003c00", "v_cvt_pknorm_u16_f16 v8, v9, v9 op_sel:[0,1,0]")[0],
                         0x0000FFFF)
        # op_sel[2] of the f16 forms: S0's result in D[31:16], D[15:0] = 0, S1's dropped; clamp is a no-op. The
        # words of tests/semantic/test_cvt_pknorm_f16_mods.py, measured on gfx1036.
        pk = ("v_mov_b32 v8, 0xdeadbeef", "v_mov_b32 v9, 0x3c003400", "v_mov_b32 v10, 0xbc003801")
        self.assertEqual(run(*pk, "v_cvt_pknorm_i16_f16 v8, v9, v10")[0], 0x400F2000)
        self.assertEqual(run(*pk, "v_cvt_pknorm_i16_f16 v8, v9, v10 clamp")[0], 0x400F2000)
        self.assertEqual(run(*pk, "v_cvt_pknorm_i16_f16 v8, v9, v10 op_sel:[0,0,1]")[0], 0x20000000)
        self.assertEqual(run(*pk, "v_cvt_pknorm_u16_f16 v8, v9, v10 op_sel:[1,0,1] clamp")[0], 0xFFFF0000)

    def test_source_modifiers(self):
        self.assertEqual(c.modify(0x3F800000, "-", 32), 0xBF800000)
        self.assertEqual(c.modify(0xBC00, "|", 16), 0x3C00)
        self.assertEqual(c.modify(0x3C00, "-|", 16), 0xBC00)                       # abs first, then neg
        self.assertEqual(run("v_mov_b32 v9, 0xbfc00000", "v_cvt_flr_i32_f32_e64 v8, |v9|",
                             "v_cvt_flr_i32_f32_e64 v10, -|v9|", "v_cvt_rpi_i32_f32_e64 v11, -v9"),
                         [1, 0xBFC00000, 0xFFFFFFFE, 2])                   # v9 is exported as is
        self.assertEqual(run("v_mov_b32 v9, 0xbc00", "v_cvt_norm_u16_f16_e64 v8, |v9|")[0], 0xFFFF)
        self.assertEqual(run("s_mov_b32 s4, 0x3c00bc00", "v_cvt_pknorm_u16_f16 v8, -s4, -|s4| op_sel:[0,1,0]")[0],
                         0x0000FFFF)

    def test_constants(self):
        self.assertEqual(c.source("0.15915494", {}, {}, True)[0], 0x3118)          # f16 value in a 16-bit operand
        self.assertEqual(c.source("0.15915494", {}, {}, False)[0], 0x3E22F983)
        self.assertEqual(c.source("-16", {}, {}, False)[0], 0xFFFFFFF0)
        self.assertEqual(run("v_cvt_norm_i16_f16_e32 v8, 0.5")[0], 0x4000)          # 16383.5 -> even
        self.assertEqual(run("v_cvt_flr_i32_f32_e32 v8, -2.0")[0], 0xFFFFFFFE)
        self.assertEqual(run("v_cvt_flr_i32_f32_e64 v8, -3")[0], 0x80000000)         # 0xfffffffd is a negative NaN
        self.assertEqual(run("v_cvt_flr_i32_f32_e64 v8, 23")[0], 0)                  # a denormal word
        self.assertEqual(run("v_cvt_norm_u16_f16_e32 v8, 0x3c00")[0], 0xFFFF)
        self.assertEqual(run("v_cvt_pknorm_u16_f16 v8, 1.0, 0.5")[0], 0x8000FFFF)

    def test_line_parsing(self):
        self.assertEqual(c.split_line("v_cvt_pknorm_i16_f16 v8, -|v9|, s4 op_sel:[1,0,0]"),
                         ("v_cvt_pknorm_i16_f16", "v_cvt_pknorm_i16_f16", ["v8", "-|v9|", "s4"], ["op_sel:[1,0,0]"]))
        self.assertEqual(c.split_line("v_cvt_norm_u16_f16_e64 v9, |v10| clamp mul:2"),
                         ("v_cvt_norm_u16_f16", "v_cvt_norm_u16_f16_e64", ["v9", "|v10|"], ["clamp", "mul:2"]))

    def test_unmodelled_forms_raise(self):
        for bad in ("v_cvt_pknorm_i16_f32 v8, v9, v10 op_sel:[1,0,0]",
                    "v_cvt_pknorm_i16_f32 v8, v9, v10 op_sel:[0,0,1]",
                    "v_cvt_pknorm_u16_f16 v8, 1.0, v10 op_sel:[1,0,0]",
                    "v_cvt_flr_i32_f32_e64 v8, -|v9",
                    "v_cvt_rpi_i32_f32_e64 s8, v9",
                    "v_cvt_f32_i32 v8, v9"):
            with self.subTest(line=bad), self.assertRaises(c.HarnessError):
                run(bad)


class Generator(unittest.TestCase):
    def test_generation_is_deterministic(self):
        self.assertEqual(c.program(random.Random(5), 6), c.program(random.Random(5), 6))
        self.assertNotEqual(c.program(random.Random(5), 6), c.program(random.Random(6), 6))

    def test_ops_subset_is_respected(self):
        rng = random.Random(1)
        ops = ["v_cvt_rpi_i32_f32", "v_cvt_pknorm_u16_f16"]
        for _ in range(30):
            _, used = c.program(rng, 4, ops)
            self.assertTrue(set(used) <= set(ops))
        self.assertEqual({op for op, *_ in c.sweep_ops(ops)}, set(ops))

    def test_sweep_size(self):
        # flr/rpi: 5 _e32 kinds + 11 _e64 (4 modifiers on the 2 register kinds, 3 constant kinds);
        # norm adds the 6 output modifier settings per kind (30); pk32: 120 encodable kind/modifier pairs + clamp on
        # 24 kind pairs; pk16: ((2 register kinds x 4 modifiers x 2 halves + 3)^2 - 1 (two literals)) x 2 destination
        # halves, + clamp on the 24 kind pairs with each destination half
        self.assertEqual(len(c.sweep_ops()), 2 * 16 + 2 * 46 + 2 * 144 + 2 * (2 * 360 + 2 * 24))

    def test_edge_grid(self):
        self.assertEqual(len(c.F32_TIES), 2 * 24 * 3)
        for h in (0x3801, 0x3802, 0x3803, 0x3804, 0xB801, 0xB802):               # the f16 double-rounding traps
            self.assertIn(h, c.F16_TIES)
        cases = c.grid_cases()
        self.assertEqual(len(cases),
                         4 * (len(c.F32_EDGES) + len(c.F32_TIES)) + 4 * (len(c.F16_EDGES) + len(c.F16_TIES)))
        rng = random.Random(0)
        by_op = {}
        for case in cases:
            by_op.setdefault(case[0], []).append(case)
        for op, op_cases in by_op.items():
            kind, snorm = c.FAMILY[op]
            chunk = op_cases[:4]
            words = run(*c.grid_program(chunk, rng))
            for i, ((_, a, b), got) in enumerate(zip(chunk, words)):
                if kind in ("flr", "rpi"):
                    want = c.to_i32(a, kind == "rpi")
                elif kind == "norm":
                    want = 0xA5A50000 | c.normalize(c.f16_value(a), snorm)
                elif kind == "pk32":
                    want = c.normalize(c.f32_value(a), snorm) | c.normalize(c.f32_value(b), snorm) << 16
                else:
                    want = c.normalize(c.f16_value(a), snorm) | c.normalize(c.f16_value(b), snorm) << 16
                self.assertEqual(got, want, f"{op} {a:#x} {b:#x}")

    def test_exhaustive_programs_cover_every_half(self):
        seen = {}
        for op, lines in c.exhaustive_programs():
            seen.setdefault(op, []).append(lines)
        self.assertEqual({op: len(p) for op, p in seen.items()},
                         {"v_cvt_norm_i16_f16": 16384, "v_cvt_norm_u16_f16": 16384,
                          "v_cvt_pknorm_i16_f16": 8192, "v_cvt_pknorm_u16_f16": 8192})
        halves = set()
        for lines in seen["v_cvt_pknorm_u16_f16"]:
            for line in lines:
                if line.startswith("v_mov_b32_e32 v1"):
                    w = int(line.split()[-1], 16)
                    halves |= {w & 0xFFFF, w >> 16}
        self.assertEqual(len(halves), 1 << 16)

    def test_programs_are_encodable(self):
        rng = random.Random(2)
        for _ in range(300):
            lines, _ = c.program(rng, 6)
            for line in lines:
                op, raw, args, mods = c.split_line(line)
                if op not in c.FAMILY:
                    continue
                kind = c.FAMILY[op][0]
                self.assertTrue(8 <= c._dst_index(args[0]) <= 11, line)
                srcs = args[1:]
                lits = [s for s in srcs if s.startswith("0x")]
                consts = [s for s in srcs if not re.fullmatch(r"-?\|?[sv]\d+\|?", s)]
                for s in consts:                                                   # no modifiers on constants
                    self.assertNotIn("|", s, line)
                if raw.endswith("_e32"):
                    self.assertIn(op, c.VOP1_OPS, line)
                    self.assertEqual(mods, [], line)
                    self.assertNotRegex(srcs[0], r"[-|]v|[-|]s", line)
                    continue
                self.assertLessEqual(len(lits), 1, line)
                self.assertLessEqual(len(lits) + sum("s" in s and not s.startswith("0x") for s in srcs), 2, line)
                for m in mods:
                    if m.startswith("op_sel"):
                        self.assertEqual(kind, "pk16", line)
                        self.assertRegex(m, r"^op_sel:\[[01],[01],[01]\]$", line)
                        sel = c._opsel(mods)
                        for s, bit in zip(srcs, sel):
                            self.assertTrue(not bit or s not in consts, line)
                    elif m == "clamp":
                        self.assertIn(op, c.CLAMP_OPS, line)
                    else:
                        self.assertIn(op, c.OMOD_OPS, line)
                if lits:
                    self.assertGreater(int(lits[0], 16), 64, line)                 # never an inline constant


class Translated(unittest.TestCase):
    def test_harness_canaries(self):
        self.assertTrue(c.self_test())

    def test_short_seeded_run_matches(self):
        counts, failure = c.run_suite(20, 3, 5)
        self.assertEqual(failure, None)
        self.assertEqual(counts["skip"], {})
        self.assertEqual(counts["pass"], 20)

    def test_sweep_matches_for_rpi_and_pknorm_i16_f32(self):
        counts, failure = c.run_sweep(4, ["v_cvt_rpi_i32_f32", "v_cvt_pknorm_i16_f32"])
        self.assertEqual((failure, counts["skip"]), (None, {}))
        self.assertEqual(counts["pass"], 16 + 144)

    def test_edge_grid_matches_for_rpi(self):
        counts, failure = c.run_grid(["v_cvt_rpi_i32_f32"])
        self.assertEqual((failure, counts["skip"]), (None, {}))
        self.assertEqual(counts["pass"], -(-(len(c.F32_EDGES) + len(c.F32_TIES)) // c.GRID_PER_PROGRAM))
        for event in ("nan", "inf", "saturated", "tie"):
            self.assertGreater(counts["events"][("v_cvt_rpi_i32_f32", event)], 0, event)

    def test_every_half_around_one_half_matches_for_pknorm_u16_f16(self):
        # 0x3780..0x387f through the exhaustive stage's programs: the double-rounding traps and the 0.5 tie
        progs = [lines for op, lines in c.exhaustive_programs(["v_cvt_pknorm_u16_f16"])]
        for lines in progs[0x3780 // 8:0x3880 // 8]:
            self.assertIsNone(c.check(lines), lines[0])

    def test_a_wrong_translation_is_caught(self):
        lines = ["v_mov_b32 v9, 0x3803", "v_cvt_pknorm_u16_f16 v8, v9, v9"]
        self.assertIsNone(c.check(lines))
        real = c.translate_listing
        try:
            # 0.5 + 3 * 2^-11 times 65535 rounded in binary32 is 32863.5, which a second rounding takes to 32864
            c.translate_listing = lambda l, n, t: [0x80608060] + real(l, n, t)[1:]
            self.assertIn("expected 0x805f805f, actual 0x80608060", c.check(lines))
        finally:
            c.translate_listing = real

    def test_a_short_export_is_caught(self):
        # zip alone compared only the words both oracles have, so a translator exporting two of the four passed
        lines = ["v_mov_b32 v9, 0x3803", "v_cvt_pknorm_u16_f16 v8, v9, v9"]
        real = c.translate_listing
        try:
            c.translate_listing = lambda l, n, t: real(l, n, t)[:2]
            self.assertEqual(c.check(lines), "exported 2 words, want 4")
            with tempfile.TemporaryDirectory() as tmp:
                self.assertEqual(c.run_one(lines, "ps_cvt_short", tmp), ("mismatch", "exported 2 words, want 4"))
        finally:
            c.translate_listing = real


class CliGuards(unittest.TestCase):
    def _run_main(self, argv):
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(sys, "argv", ["cvt_diff.py", *argv]), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            try:
                rc = c.main()
            except SystemExit as exc:
                rc = exc.code
        return rc, out.getvalue(), err.getvalue()

    def test_invalid_counts_and_empty_runs_are_rejected(self):
        for argv, needle in (
            (["--no-sweep", "--programs", "-1"], "--programs"),
            (["--no-sweep", "--programs", "1", "--max-ops", "0"], "--max-ops"),
            (["--no-sweep", "--programs", "1", "--max-ops", "-2"], "--max-ops"),
            (["--no-sweep", "--programs", "0"], "nothing to run"),
        ):
            with self.subTest(argv=argv):
                rc, _, err = self._run_main(argv)
                self.assertEqual(rc, 2)
                self.assertIn(needle, err)

    def test_zero_program_exhaustive_f32_run_exits_two(self):
        rc, _, err = self._run_main(["--no-sweep", "--programs", "0", "--exhaustive-f16", "--ops", "v_cvt_flr_i32_f32"])
        self.assertEqual(rc, 2)
        self.assertIn("no program ran", err)

    def test_ops_validation_and_deduplication(self):
        for bad_ops in ("", "   ", "v_cvt_flr_i32_f32,", ",v_cvt_flr_i32_f32"):
            with self.subTest(ops=bad_ops):
                rc, _, err = self._run_main(["--no-sweep", "--programs", "1", "--ops", bad_ops])
                self.assertEqual(rc, 2)
                self.assertIn("--ops", err)

        seen_ops = []

        def fake_suite(programs, seed, max_ops=6, ops=None):
            seen_ops.append(ops)
            return {"pass": 1, "mismatch": 0, "skip": c.Counter(), "ops": c.Counter({"v_cvt_flr_i32_f32": 1}),
                    "events": c.Counter()}, None

        with mock.patch.object(c, "run_suite", side_effect=fake_suite):
            rc, _, _ = self._run_main(["--no-sweep", "--programs", "1", "--ops", "v_cvt_flr_i32_f32, v_cvt_flr_i32_f32"])
        self.assertEqual(rc, 0)
        self.assertEqual(seen_ops, [["v_cvt_flr_i32_f32"]])

    def test_run_suite_validates_bounds_and_main_accepts_argv(self):
        for bad_programs in (-1, True):
            with self.subTest(programs=bad_programs), self.assertRaises(ValueError):
                c.run_suite(bad_programs, 1)
        for bad_max_ops in (0, -1, True):
            with self.subTest(max_ops=bad_max_ops), self.assertRaises(ValueError):
                c.run_suite(1, 1, max_ops=bad_max_ops)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(c.main(["--list-ops"]), 0)
        self.assertIn("v_cvt_flr_i32_f32", out.getvalue())


if __name__ == "__main__":
    unittest.main()
