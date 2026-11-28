"""tools/dot_diff.py: the packed integer dot reference against the words measured on RDNA2 hardware and hand-pinned
ISA words, the generator's encoding rules, the source-kind sweep, and a short seeded differential run. Standard
library only; no GPU, no game files.

The family is v_dot2_i32_i16, v_dot2_u32_u16, v_dot4_i32_i8, v_dot4_u32_u8, v_dot8_i32_i4, v_dot8_u32_u4 (VOP3P) and
the accumulating v_dot4c_i32_i8, v_dot2c_i32_i16, v_dot2c_u32_u16, v_dot4c_u32_u8, v_dot8c_i32_i4, v_dot8c_u32_u4.
"""
import contextlib
import io
import os
import random
import re
import sys
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import dot_diff as d  # noqa: E402  (puts tests/semantic on sys.path)
import test_alu_dot4_i8 as hw  # noqa: E402  (the gfx1036-measured v_dot4_i32_i8 / v_dot4c_i32_i8 words)


def run(*body):
    """The reference's four exported words for a program."""
    return d.reference(list(body))


class Reference(unittest.TestCase):
    def test_measured_gfx1036_words(self):
        # tests/semantic/test_alu_dot4_i8.py: v_dot4_i32_i8 v8, v1, v2, v3 and v_dot4c_i32_i8 on gfx1036
        for a, b, acc, want in hw.MEASURED:
            with self.subTest(a=hex(a), b=hex(b), acc=hex(acc)):
                self.assertEqual(d.dot("v_dot4_i32_i8", a, b, acc), want)
                self.assertEqual(run(f"v_mov_b32 v9, {a:#x}", f"v_mov_b32 v10, {b:#x}", f"v_mov_b32 v11, {acc:#x}",
                                     "v_dot4_i32_i8 v8, v9, v10, v11")[0], want)
                self.assertEqual(run(f"v_mov_b32 v9, {a:#x}", f"v_mov_b32 v10, {b:#x}", f"v_mov_b32 v8, {acc:#x}",
                                     "v_dot4c_i32_i8_e32 v8, v9, v10")[0], want)
                self.assertEqual(run(f"v_mov_b32 v9, {a:#x}", f"v_mov_b32 v10, {b:#x}", f"v_mov_b32 v8, {acc:#x}",
                                     "v_dot4c_i32_i8_e32 v8, v10, v9")[0], want)

    def test_measured_gfx1036_op_sel_words(self):
        # the same hardware with op_sel 2 / op_sel_hi 5: S1's 16-bit halves exchanged
        for a, b, acc, want in hw.MEASURED_OP_SEL:
            with self.subTest(a=hex(a), b=hex(b), acc=hex(acc)):
                self.assertEqual(run(f"v_mov_b32 v9, {a:#x}", f"v_mov_b32 v10, {b:#x}", f"v_mov_b32 v11, {acc:#x}",
                                     "v_dot4_i32_i8 v8, v9, v10, v11 op_sel:[0,1,0] op_sel_hi:[1,0,1]")[0], want)

    def test_clamp_saturates_the_exact_sum(self):
        M = d.M32
        # (-32768)^2 + (-32768)^2 = 2^31: wraps, saturates, and cancels against INT32_MIN without early saturation
        self.assertEqual(d.dot("v_dot2_i32_i16", 0x80008000, 0x80008000, 0), 0x80000000)
        self.assertEqual(d.dot("v_dot2_i32_i16", 0x80008000, 0x80008000, 0, clamp=True), 0x7FFFFFFF)
        self.assertEqual(d.dot("v_dot2_i32_i16", 0x80008000, 0x80008000, 0x80000000, clamp=True), 0)
        self.assertEqual(d.dot("v_dot2_i32_i16", 0x80008000, 0x80008000, 0x80000000), 0)
        # the signed accumulator: -1 + 2^31 is in range, INT32_MIN - 448 is not
        self.assertEqual(d.dot("v_dot2_i32_i16", 0x80008000, 0x80008000, M, clamp=True), 0x7FFFFFFF)
        self.assertEqual(d.dot("v_dot8_i32_i4", 0x77777777, 0x88888888, 0x80000000, clamp=True), 0x80000000)
        self.assertEqual(d.dot("v_dot8_i32_i4", 0x77777777, 0x88888888, 0x80000000), 0x7FFFFE40)
        # unsigned: 2 * 65535^2 wraps to 0xfffc0002, saturates to UINT32_MAX; an unsigned accumulator never goes below 0
        self.assertEqual(d.dot("v_dot2_u32_u16", M, M, 0), 0xFFFC0002)
        self.assertEqual(d.dot("v_dot2_u32_u16", M, M, 0, clamp=True), M)
        self.assertEqual(d.dot("v_dot4_u32_u8", 0, 0, 0x80000000, clamp=True), 0x80000000)
        self.assertEqual(d.dot("v_dot8_u32_u4", M, M, M - 1799, clamp=True), M)
        self.assertEqual(d.dot("v_dot8_u32_u4", M, M, M - 1800, clamp=True), M)
        self.assertEqual(d.dot("v_dot8_u32_u4", M, M, M - 1801, clamp=True), M - 1)

    def test_element_widths_and_signedness(self):
        self.assertEqual(d.elements(0x80017FFF, 16, True), [32767, -32767])
        self.assertEqual(d.elements(0x80017FFF, 16, False), [32767, 32769])
        self.assertEqual(d.elements(0x807F01FF, 8, True), [-1, 1, 127, -128])
        self.assertEqual(d.elements(0x89ABCDEF, 4, True), [-1, -2, -3, -4, -5, -6, -7, -8])
        self.assertEqual(d.elements(0x89ABCDEF, 4, False), [15, 14, 13, 12, 11, 10, 9, 8])

    def test_constants_read_as_32_bit_dwords(self):
        # an inline constant is the sign-extended dword, a literal the whole dword
        self.assertEqual(run("v_mov_b32 v9, 0x00010001", "v_dot2_i32_i16 v8, v9, -2, 0")[0], 0xFFFFFFFD)  # (-2) + (-1)
        self.assertEqual(run("v_mov_b32 v9, 0x00010001", "v_dot2_i32_i16 v8, v9, 64, 0")[0], 64)          # 64 + 0
        self.assertEqual(run("v_mov_b32 v9, 0x00010001", "v_dot2_i32_i16 v8, v9, 0x00030004, 0")[0], 7)

    def test_line_parsing(self):
        self.assertEqual(d.split_line("v_dot4_i32_i8 v8, v9, s4, 0x10 op_sel:[1,0,0] op_sel_hi:[1,1,1] clamp"),
                         ("v_dot4_i32_i8", ["v8", "v9", "s4", "0x10"],
                          {"clamp": True, "op_sel": (1, 0, 0), "op_sel_hi": (1, 1, 1)}))
        self.assertEqual(d.split_line("v_dot8c_i32_i4_e64 v8, -3, v9 clamp"),
                         ("v_dot8c_i32_i4", ["v8", "-3", "v9"], {"clamp": True}))
        with self.assertRaises(d.HarnessError):
            run("v_dot2_i32_i16 v8, v9, v10, v11 op_sel:[1,0,0] op_sel_hi:[1,1,1]")   # not a modelled form


class Generator(unittest.TestCase):
    def test_generation_is_deterministic(self):
        self.assertEqual(d.program(random.Random(5), 6), d.program(random.Random(5), 6))
        self.assertNotEqual(d.program(random.Random(5), 6), d.program(random.Random(6), 6))

    def test_ops_subset_is_respected(self):
        rng = random.Random(1)
        ops = ["v_dot2_u32_u16", "v_dot4c_i32_i8"]
        for _ in range(30):
            _, used = d.program(rng, 4, ops)
            self.assertTrue(set(used) <= set(ops))
        cases = d.sweep_ops(ops)
        self.assertEqual({op for op, *_ in cases}, set(ops))
        self.assertEqual(len(cases), 100 + 4)

    def test_sweep_size(self):
        # VOP3P: 50 encodable kind triples x clamp on/off; v_dot4_i32_i8 adds 16 op_sel settings x clamp on/off;
        # v_dot4c_i32_i8: 4 S0 kinds; each gfx908 accumulating op: 4 S0 kinds plus 3 (no literal) with clamp
        self.assertEqual(len(d.sweep_ops()), 6 * 100 + 32 + 4 + 5 * 7)

    def test_edge_grid(self):
        self.assertEqual(d.lane_edges(16), (0x80008000, 0x7FFF7FFF, 0xFFFFFFFF, 0x7FFF8000))
        self.assertEqual(d.lane_edges(8), (0x80808080, 0x7F7F7F7F, 0xFFFFFFFF, 0x7F807F80))
        self.assertEqual(d.lane_edges(4), (0x88888888, 0x77777777, 0xFFFFFFFF, 0x78787878))
        # 16 S0/S1 pairs x 8 accumulators, x2 where clamp is taken: 6 VOP3P + 5 gfx908 forms, v_dot4c_i32_i8 once
        cases = d.grid_cases()
        self.assertEqual(len(cases), 11 * 256 + 128)
        self.assertIn(("v_dot2_i32_i16", 0x80008000, 0x80008000, 0, True), cases)   # the product-only overflow
        for chunk in (cases[:4], cases[-3:]):
            words = run(*d.grid_program(chunk))
            for (op, x, y, acc, clamp), got in zip(chunk, words):
                self.assertEqual(got, d.dot(op, x, y, acc, clamp))

    def test_programs_are_encodable(self):
        rng = random.Random(2)
        for _ in range(200):
            lines, _ = d.program(rng, 6)
            for line in lines:
                op, args, mods = d.split_line(line)
                if op not in d.FAMILY:
                    continue
                self.assertTrue(8 <= d._dst_index(args[0]) <= 11, line)       # results land in exported regs
                srcs = args[1:]
                lits = [s for s in srcs if s.startswith("0x")]
                if op in d.VOP3P_OPS:
                    self.assertLessEqual(len(lits), 1, line)
                    self.assertLessEqual(len(lits) + sum(s.startswith("s") for s in srcs), 2, line)
                    if op not in d.OPSEL_OPS:
                        self.assertEqual((mods.get("op_sel", (0, 0, 0)), mods.get("op_sel_hi", (1, 1, 1))),
                                         ((0, 0, 0), (1, 1, 1)), line)
                else:
                    self.assertRegex(srcs[1], r"^v\d+$", line)                  # VOP2 S1
                    self.assertTrue(line.split()[0].endswith("_e64") == ("clamp" in mods), line)
                    if "clamp" in mods:
                        self.assertIn(op, d.CLAMP_ACC_OPS)
                        self.assertFalse(lits, line)                             # gfx908 VOP3: no literal

    def test_accumulating_op_reads_the_old_destination(self):
        self.assertEqual(run("v_mov_b32 v8, 0xFFFFFFF0", "v_mov_b32 v9, 0x01010101",
                             "v_dot4c_u32_u8_e32 v8, v9, v9")[0], 0xFFFFFFF4)
        self.assertEqual(run("v_mov_b32 v8, 0xFFFFFFF0", "v_mov_b32 v9, 0x01010101",
                             "v_dot4c_u32_u8_e64 v8, 16, v9 clamp")[0], 0xFFFFFFFF)


class Translated(unittest.TestCase):
    def test_harness_canaries(self):
        self.assertTrue(d.self_test())

    def test_short_seeded_run_matches(self):
        counts, failure = d.run_suite(20, 3, 5)
        self.assertEqual(failure, None)
        self.assertEqual(counts["skip"], {})
        self.assertEqual(counts["pass"], 20)

    def test_sweep_matches_for_the_signed_vop3p_ops(self):
        counts, failure = d.run_sweep(4, ["v_dot2_i32_i16", "v_dot4_i32_i8"])
        self.assertEqual((failure, counts["skip"]), (None, {}))
        self.assertEqual(counts["pass"], 100 + 132)

    def test_edge_grid_matches_for_v_dot2_i32_i16(self):
        counts, failure = d.run_grid(["v_dot2_i32_i16"])
        self.assertEqual((failure, counts["skip"], counts["pass"]), (None, {}, 64))
        self.assertGreater(counts["events"][("v_dot2_i32_i16", "saturated")], 0)

    def test_a_wrong_translation_is_caught(self):
        lines = ["v_mov_b32 v8, 0x80008000", "v_mov_b32 v9, 0x80008000", "v_mov_b32 v10, 0",
                 "v_dot2_i32_i16 v11, v8, v9, v10 clamp"]
        self.assertIsNone(d.check(lines))
        real = d.translate_listing
        try:
            d.translate_listing = lambda l, n, t: real(l, n, t)[:3] + [0x80000000]   # wrapped instead of clamped
            self.assertIn("expected 0x7fffffff, actual 0x80000000", d.check(lines))
        finally:
            d.translate_listing = real


class CliGuards(unittest.TestCase):
    def _run_main(self, argv):
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(sys, "argv", ["dot_diff.py", *argv]), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            try:
                rc = d.main()
            except SystemExit as exc:
                rc = exc.code
        return rc, out.getvalue(), err.getvalue()

    def test_skipped_programs_exit_nonzero(self):
        with mock.patch.object(d, "run_one", return_value=("skip", "translator unsupported: synthetic")):
            rc, out, _ = self._run_main(["--no-sweep", "--programs", "2", "--ops", "v_dot2_i32_i16"])
        self.assertEqual(rc, 1)
        self.assertIn("SKIPPED 2 program(s)", out)

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

    def test_ops_validation_and_deduplication(self):
        for bad_ops in ("", "   ", "v_dot2_i32_i16,", ",v_dot2_i32_i16"):
            with self.subTest(ops=bad_ops):
                rc, _, err = self._run_main(["--no-sweep", "--programs", "1", "--ops", bad_ops])
                self.assertEqual(rc, 2)
                self.assertIn("--ops", err)

        seen_ops = []

        def fake_suite(programs, seed, max_ops=6, ops=None):
            seen_ops.append(ops)
            return {"pass": 1, "mismatch": 0, "skip": d.Counter(), "ops": d.Counter({"v_dot2_i32_i16": 1}),
                    "events": d.Counter()}, None

        with mock.patch.object(d, "run_suite", side_effect=fake_suite):
            rc, _, _ = self._run_main(["--no-sweep", "--programs", "1", "--ops", "v_dot2_i32_i16, v_dot2_i32_i16"])
        self.assertEqual(rc, 0)
        self.assertEqual(seen_ops, [["v_dot2_i32_i16"]])

        seen_ops.clear()
        with mock.patch.object(d, "run_suite", side_effect=fake_suite):
            rc, _, _ = self._run_main(["--no-sweep", "--programs", "1", "--ops", " gfx103x "])
        self.assertEqual(rc, 0)
        self.assertEqual(seen_ops, [list(d.GFX103X_OPS)])

    def test_run_suite_validates_bounds_and_main_accepts_argv(self):
        for bad_programs in (-1, True):
            with self.subTest(programs=bad_programs), self.assertRaises(ValueError):
                d.run_suite(bad_programs, 1)
        for bad_max_ops in (0, -1, True):
            with self.subTest(max_ops=bad_max_ops), self.assertRaises(ValueError):
                d.run_suite(1, 1, max_ops=bad_max_ops)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(d.main(["--list-ops"]), 0)
        self.assertIn("v_dot2_i32_i16", out.getvalue())


if __name__ == "__main__":
    unittest.main()
