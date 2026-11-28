"""tools/wint_diff.py: the whole-wave reference against hand-worked values, a short seeded fuzz run, and canaries.
Standard library only; no GPU."""
import contextlib
import io
import os
from pathlib import Path
import re
import subprocess
import sys
import unittest
import unittest.mock

TOOL = Path(__file__).resolve().parents[1] / "tools" / "wint_diff.py"
sys.path.insert(0, str(TOOL.parent))
import wint_diff as d  # noqa: E402


def program(body, wave=32, obs=(1, 2, 3, 4)):
    """Every data VGPR starts as the lane number, s<N> = N, s20/s[20:21] = lanes < 4, s21/s[22:23] = lanes < 2,
    VCC = no lane."""
    init = ["v_mbcnt_lo_u32_b32 v0, -1, 0"] + (["v_mbcnt_hi_u32_b32 v0, -1, v0"] if wave == 64 else [])
    init += [f"v_mov_b32_e32 v{v}, v0" for v in d.DREGS] + [f"s_mov_b32 s{s}, {s}" for s in d.SREGS]
    m1, m2, vcc = d.masks(wave)
    init += [f"v_cmp_lt_u32_e64 {m1}, v0, 4", f"v_cmp_lt_u32_e64 {m2}, v0, 2", f"v_cmp_ne_u32_e64 {vcc}, 0, 0"]
    return d.Program(init, list(body), list(obs), wave)


def run(body, wave=32, obs=(1, 2, 3, 4)):
    p = program(body, wave, obs)
    return d.reference(p.lines(), wave)


class Reference(unittest.TestCase):
    def test_carry_out_and_in_under_exec(self):
        out = run(["s_mov_b32 exec_lo, 0xf",                      # lanes 0..3
                   "v_add_co_u32_e64 v1, s20, v0, -2",            # lanes 2 and 3 carry: s20 = 0b1100
                   "v_add_co_ci_u32_e64 v2, s21, v0, 0, s20",     # v2 = lane + carry
                   "v_cndmask_b32_e64 v3, 7, 9, s20"])
        self.assertEqual(out[0], (0xFFFFFFFE, 0, 7, 0))
        self.assertEqual(out[3], (1, 4, 9, 3))
        self.assertEqual(out[5], (5, 5, 5, 5))                    # inactive: untouched

    def test_borrow(self):
        # 0 - 0 - borrow(1) = 0xffffffff with a borrow out; s20 holds lanes 0..3 as the borrow in
        out = run(["v_sub_co_ci_u32_e64 v1, s21, 0, 0, s20", "v_cndmask_b32_e64 v2, 0, 1, s21"])
        self.assertEqual(out[1][:2], (0xFFFFFFFF, 1))
        self.assertEqual(out[4][:2], (0, 0))

    def test_clamp_saturates_d_and_keeps_the_carry(self):
        # v1 = lane + 0xfffffffe saturates from lane 2 on; v2 = lane - 2 saturates to 0 in lanes 0 and 1;
        # both masks still get the real carry / borrow
        out = run(["v_add_co_u32_e64 v1, s20, v0, -2 clamp", "v_sub_co_u32_e64 v2, s21, v0, 2 clamp",
                   "v_cndmask_b32_e64 v3, 0, 1, s20", "v_cndmask_b32_e64 v4, 0, 1, s21"])
        self.assertEqual(out[0], (0xFFFFFFFE, 0, 0, 1))
        self.assertEqual(out[1], (0xFFFFFFFF, 0, 0, 1))
        self.assertEqual(out[2], (0xFFFFFFFF, 0, 1, 0))
        self.assertEqual(out[5], (0xFFFFFFFF, 3, 1, 0))

    def test_clamp_counts_the_carry_in(self):
        # carry in = lanes 0..3. Lane 0: -1 + 0 + 1 and 0 - 0 - 1 overflow only through the carry in
        out = run(["v_add_co_ci_u32_e64 v1, s21, -1, v0, s20 clamp", "v_cndmask_b32_e64 v2, 0, 1, s21",
                   "v_subrev_co_ci_u32_e64 v3, vcc_lo, v0, 0, s20 clamp", "v_cndmask_b32_e64 v4, 0, 1, vcc_lo"])
        self.assertEqual(out[0], (0xFFFFFFFF, 1, 0, 1))
        self.assertEqual(out[4], (0xFFFFFFFF, 1, 0, 1))
        for line in ("v_add_co_u32_e32 v1, vcc_lo, v0, v2 clamp", "v_xor_b32_e64 v1, v0, v2 clamp",
                     "v_add_nc_u16 v1, v0, v2 clamp", "s_mov_b32 s20, 1 clamp", "v_writelane_b32 v1, s20, 3 clamp",
                     "v_mbcnt_lo_u32_b32 v0, -1, 0 clamp", "v_xor_b32_e64 v1, v0, v2 op_sel:[1,0,0]",
                     "s_mov_b32 s20, 1 op_sel:[1,0]"):
            with self.subTest(line=line), self.assertRaises(d.RefError):
                run([line])
        # rejected before it runs: EXEC keeps its value (it used to become 1)
        st = d.State(32)
        before = st.exec
        with self.assertRaises(d.RefError):
            d.step(st, "s_mov_b32 exec_lo, 1 clamp")
        self.assertEqual(st.exec, before)

    def test_inactive_lanes_get_a_zero_mask_bit(self):
        # s20 starts as lanes 0..3; a compare that is true everywhere but runs on lanes 2..31 clears lanes 0 and 1
        out = run(["s_mov_b32 exec_lo, 0xfffffffc", "v_cmp_t_u32_e64 s20, v0, v0", "s_mov_b32 exec_lo, -1",
                   "v_cndmask_b32_e64 v1, 0, 1, s20"])
        self.assertEqual([out[lane][0] for lane in range(4)], [0, 0, 1, 1])

    def test_cmpx_writes_exec(self):
        out = run(["v_cmpx_gt_i32_e64 v0, 29", "v_mov_b32_e32 v1, 77"])
        self.assertEqual([out[lane][0] for lane in (28, 29, 30, 31)], [28, 29, 77, 77])

    def test_mad64_carry(self):
        # u64: (2^32 - 1)^2 + (2^64 - 1) = 2^65 - 2^33: low 64 bits 0xfffffffe_00000000, bit 64 set
        out = run(["v_mov_b32_e32 v4, -1", "v_mov_b32_e32 v5, -1",
                   "v_mad_u64_u32 v[2:3], s20, -1, -1, v[4:5]", "v_cndmask_b32_e64 v1, 0, 1, s20"],
                  obs=(2, 3, 1, 1))
        self.assertEqual(out[7], (0, 0xFFFFFFFE, 1, 1))
        # i64: -1 * lane + 0 is negative for lane > 0: the carry is the sign of the exact result
        out = run(["v_mov_b32_e32 v4, 0", "v_mov_b32_e32 v5, 0",
                   "v_mad_i64_i32 v[2:3], s20, -1, v0, v[4:5]", "v_cndmask_b32_e64 v1, 0, 1, s20"],
                  obs=(2, 3, 1, 1))
        self.assertEqual(out[0], (0, 0, 0, 0))
        self.assertEqual(out[6], (0xFFFFFFFA, 0xFFFFFFFF, 1, 1))

    def test_16bit_writes_one_half(self):
        out = run(["v_mov_b32_e32 v1, 0x12345678", "v_mov_b32_e32 v2, 0x00030004",
                   "v_add_nc_u16 v1, v2, v2 op_sel:[1,0,1]",          # 3 + 4 into the high half
                   "v_mov_b32_e32 v3, 0x12345678", "v_mul_lo_u16_e64 v3, v2, -1"])   # 4 * 0xffff: 0xfffc
        self.assertEqual(out[0][:3], (0x00075678, 0x00030004, 0x1234FFFC))

    def test_wave64_exec_hi_and_pairs(self):
        out = run(["s_mov_b32 exec_lo, 0", "s_mov_b32 exec_hi, 0x80000001",   # lanes 32 and 63
                   "v_cmp_ge_u32_e64 s[20:21], v0, 33", "v_add_nc_u32_e64 v1, v0, 100",
                   "s_mov_b32 exec_lo, -1", "s_mov_b32 exec_hi, -1",
                   "v_cndmask_b32_e64 v2, 0, 1, s[20:21]"], wave=64)
        self.assertEqual([out[lane][:2] for lane in (0, 32, 33, 63)], [(0, 0), (132, 0), (33, 0), (163, 1)])

    def test_data_in_a_mask_sgpr_is_read_bit_by_bit(self):
        out = run(["s_mov_b32 s20, 0xa", "v_cndmask_b32_e64 v1, 0, 1, s20"])
        self.assertEqual([out[lane][0] for lane in range(5)], [0, 1, 0, 1, 0])

    def test_mbcnt_and_writelane_semantics_respect_operands_exec_and_wrap(self):
        # v_mbcnt_lo/hi honor dst VGPR, mask, addend, and EXEC; v_writelane honors scalar/imm src and wraps lane mod wave
        body32 = [
            "s_mov_b32 exec_lo, 0x0000000f",
            "v_mbcnt_lo_u32_b32 v1, 0x55555555, 10",
            "s_mov_b32 exec_lo, -1",
            "v_writelane_b32 v2, 7, 32",          # wave32 wraps 32 -> lane 0
            "v_writelane_b32 v3, s4, s5",         # lane s5 (=5) gets s4 (=4)
        ]
        out32 = run(body32, wave=32, obs=(1, 2, 3, 0))
        self.assertEqual([out32[l][0] for l in range(6)], [10, 11, 11, 12, 4, 5])
        self.assertEqual(out32[0][1], 7)
        self.assertEqual(out32[5][2], 4)
        self.assertEqual(out32[0][3], 0)          # v0 was not clobbered by v_mbcnt into v1

        body64 = [
            "v_mbcnt_lo_u32_b32 v1, 0x00000003, 5",
            "v_mbcnt_hi_u32_b32 v2, 0x00000005, v1",
            "v_writelane_b32 v3, 9, 65",          # wave64 wraps 65 -> lane 1
        ]
        out64 = run(body64, wave=64, obs=(1, 2, 3, 0))
        self.assertEqual(out64[0][0], 5)
        self.assertEqual(out64[2][0], 7)
        self.assertEqual(out64[32][1], 7)
        self.assertEqual(out64[33][1], 8)
        self.assertEqual(out64[35][1], 9)
        self.assertEqual(out64[1][2], 9)

        # Cross-check with the SPIR-V translator on wave32
        self.assertIsNone(d.check(program(body32, wave=32, obs=(1, 2, 3, 0))))

    def test_operand_mask_pair_and_arity_validation_rejects_malformed_lines(self):
        bad32 = [
            "",
            "s_mov_b32 v1, 5",
            "s_mov_b32 s4, v1",
            "s_mov_b32 exec_hi, 1",
            "s_mov_b32 s4, 1, 2",
            "v_mbcnt_lo_u32_b32 s1, -1, 0",
            "v_mbcnt_lo_u32_b32 v1, -1",
            "v_writelane_b32 s1, s4, 0",
            "v_writelane_b32 v1, v2, 0",
            "v_writelane_b32 v1, s4, v0",
            "v_cmp_bad_u32_e64 s20, v0, v1",
            "v_cmp_eq_u32_e64 s[20:21], v0, v1",
            "v_cmp_eq_u32_e64 vcc, v0, v1",
            "v_cmp_eq_u32_e64 v1, v0, v1",
            "v_cmpx_eq_u32_e64 s20, v0, v1",
            "v_cndmask_b32_e64 s1, v0, v2, s20",
            "v_cndmask_b32_e64 v1, v0, v2",
            "v_add_co_ci_u32_e64 v1, s20, v0, 1",
            "v_add_co_u32_e64 v1, s20, v0, 1, s21",
            "v_add_co_u32_e64 s1, s20, v0, 1",
            "v_mad_u64_u32 v[2:4], s20, v0, v1, v[4:5]",
            "v_mad_u64_u32 v2, s20, v0, v1, v[4:5]",
            "v_mad_u64_u32 v[2:3], s20, v0, v1, v[4:6]",
            "v_add_nc_u16 v1, v2, v3 op_sel:[1,0]",
            "v_add_nc_u16 v1, v2, v3 op_sel:[1,0,1,0]",
            "v_add_nc_u32_e64 s1, v0, 1",
            "v_add_nc_u32_e64 v1, v0, 1, 2",
            "v_add_nc_u32_e64 v1, , 1",
            "v_add_nc_u32_e64 v1, badtok, 1",
        ]
        for line in bad32:
            with self.subTest(wave=32, line=line), self.assertRaises(d.RefError):
                run([line], wave=32)

        bad64 = [
            "v_cmp_eq_u32_e64 s20, v0, v1",
            "v_cmp_eq_u32_e64 vcc_lo, v0, v1",
            "v_cmp_eq_u32_e64 s[20:22], v0, v1",
            "v_cndmask_b32_e64 v1, v0, v2, s20",
        ]
        for line in bad64:
            with self.subTest(wave=64, line=line), self.assertRaises(d.RefError):
                run([line], wave=64)


class Generator(unittest.TestCase):
    def lines(self, known=False, wave=32, n=3000, seed=1):
        real, d.KNOWN = d.KNOWN, known
        try:
            rng = d.random.Random(seed)
            return [line for _ in range(n) for line in d.Gen(rng, wave, d.ALL_OPS).op()]
        finally:
            d.KNOWN = real

    @staticmethod
    def opname(line):
        op = line.split()[0]
        if op == "s_mov_b32":
            dst = line.split()[1].rstrip(",")
            return "exec" if dst.startswith("exec") else "vcc" if dst.startswith("vcc") else "smask"
        return re.sub(r"_e(32|64)$", "", op)

    def test_empty_op_selection_raises_value_error(self):
        for wave in (32, 64):
            for ops in ([], ["v_nonexistent_op"]):
                with self.subTest(call="Gen", wave=wave, ops=ops):
                    with self.assertRaisesRegex(ValueError, r"^no ops selected$"):
                        d.Gen(d.random.Random(0), wave, ops)
                with self.subTest(call="generate", wave=wave, ops=ops):
                    with self.assertRaisesRegex(ValueError, r"^no ops selected$"):
                        d.generate(d.random.Random(0), 1, ops, wave)
                with self.subTest(call="run_suite", wave=wave, ops=ops):
                    with self.assertRaisesRegex(ValueError, r"^no ops selected$"):
                        d.run_suite(1, 0, 1, ops, (wave,))

    def test_every_op_is_generated(self):
        for wave in (32, 64):
            seen = {self.opname(line) for line in self.lines(wave=wave)}
            self.assertEqual(set(d.ALL_OPS) - seen, set(), wave)

    def test_clamp_only_on_the_e64_carry_ops(self):
        for wave in (32, 64):
            lines = self.lines(wave=wave)
            clamped = [line for line in lines if line.endswith(" clamp")]
            self.assertEqual({self.opname(line) for line in clamped}, set(d.CARRY), wave)
            self.assertTrue(all(line.split()[0].endswith("_e64") for line in clamped), wave)
            plain = [line for line in lines if line.split()[0].endswith("_e64") and self.opname(line) in d.CARRY
                     and not line.endswith(" clamp")]
            self.assertTrue(plain, wave)

    def test_known_divergences_are_left_out_by_default(self):
        def forms(lines):
            out = set()
            for line in lines:
                op, _, rest = line.partition(" ")
                rest = rest.removesuffix(" clamp")
                if op == "s_mov_b32" and rest.startswith("vcc"):
                    out.add("vcc-data-mask")
                if op.startswith("s_"):
                    continue
                srcs = [a.strip() for a in rest.split(",")]
                # drop the destination, the mask destinations and the mask operands (carry-in, cndmask selector)
                skip = 0 if op.startswith("v_cmpx_") else 2 if "_co_" in op or op in d.MAD64 else 1
                srcs = srcs[skip:-1] if op.startswith("v_cndmask") or "_co_ci_" in op else srcs[skip:]
                if any(re.fullmatch(r"s2[0-3]|vcc_lo|vcc_hi", s) for s in srcs):
                    out.add("mask-as-data")
            return out
        for wave in (32, 64):
            self.assertEqual(forms(self.lines(wave=wave)), set(), wave)
            self.assertEqual(forms(self.lines(known=True, wave=wave)), set(d.KNOWN_DIVERGENCES), wave)


class Translated(unittest.TestCase):
    def test_harness_canaries(self):
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertTrue(d.self_test())

    def test_short_seeded_run_matches(self):
        counts, failures = d.run_suite(40, 7, 6, d.ALL_OPS, (32, 64))
        self.assertEqual(failures, [], failures[:1])
        self.assertEqual(counts["skip"], {})
        self.assertEqual(counts["pass"], 40)

    def test_hand_cases_match_the_translator(self):
        cases = [
            (32, ["s_mov_b32 exec_lo, 0x0000ff0f", "v_add_co_u32_e64 v1, s20, v0, -2",
                 "v_add_co_ci_u32_e64 v2, s21, v0, 0, s20", "v_cndmask_b32_e64 v3, 7, 9, s20",
                 "v_mad_i64_i32 v[4:5], s21, -1, v0, v[12:13]", "v_cmpx_gt_i32_e64 v0, 9",
                 "v_add_nc_u16 v12, v2, v2 op_sel:[1,0,1]"]),
            # a carry-out written under a narrow EXEC is 0 in the lanes that were off
            (32, ["s_mov_b32 exec_lo, 0xfffffffc", "v_add_co_u32_e64 v2, s20, v0, -1", "s_mov_b32 exec_lo, -1",
                  "v_cndmask_b32_e64 v1, 0, 1, s20", "v_mov_b32_e32 v3, 0", "v_mov_b32_e32 v12, 0"]),
            (64, ["s_mov_b32 exec_hi, 0x80000001", "v_cmp_ge_u32_e64 s[20:21], v0, 33",
                 "v_sub_co_ci_u32_e64 v1, s[22:23], 0, v0, s[20:21]", "s_mov_b32 s22, 0xa", "s_mov_b32 s23, 0x3",
                 "v_cndmask_b32_e64 v2, 0, 1, s[22:23]", "v_add_co_ci_u32_e32 v3, vcc, 5, v0, vcc"]),
        ]
        for wave, body in cases:
            with self.subTest(wave=wave):
                self.assertIsNone(d.check(program(body, wave, (1, 2, 3, 12 if wave == 32 else 4))))

    def test_clamp_cases_match_the_translator(self):
        cases = [
            # lane + 0xffffffff + carry in: the sum's low word is the lane again, and in lane 0 only the carry in
            # overflows
            (32, ["s_mov_b32 exec_lo, 0xfffffff3", "v_add_co_ci_u32_e64 v3, vcc_lo, v0, -1, s21 clamp",
                  "v_add_co_u32_e64 v1, s20, v0, -2 clamp", "v_sub_co_u32_e64 v2, s21, v0, 2 clamp",
                  "s_mov_b32 exec_lo, -1", "v_subrev_co_ci_u32_e64 v12, s20, v0, 3, s21 clamp"]),
            # lane - 0xffffffff - borrow in: with the borrow in, the low word is the lane again
            (64, ["s_mov_b32 exec_hi, 0xfffffff3", "v_add_co_u32_e64 v1, s[22:23], v0, -40 clamp",
                  "v_sub_co_ci_u32_e64 v2, vcc, v0, -1, s[22:23] clamp",
                  "v_subrev_co_u32_e64 v3, s[20:21], 40, v0 clamp", "v_cndmask_b32_e64 v4, 0, 1, vcc"]),
        ]
        for wave, body in cases:
            with self.subTest(wave=wave):
                self.assertIsNone(d.check(program(body, wave, (1, 2, 3, 12 if wave == 32 else 4))))

    def test_a_wrong_translation_is_caught(self):
        prog = program(["s_mov_b32 exec_lo, 0xf", "v_add_co_u32_e64 v1, s20, v0, -2",
                        "v_cndmask_b32_e64 v2, 7, 9, s20"])
        self.assertIsNone(d.check(prog))
        want = d.reference(prog.lines(), 32)
        real = d.translated
        try:
            # one lane's word off: a write that ignored EXEC on lane 9
            d.translated = lambda lines, wave: [w if lane != 9 else (want[9][0] ^ 4,) + w[1:]
                                                for lane, w in enumerate(want)]
            self.assertIn("lane 9 word 0 (v1)", d.check(prog))
            d.translated = lambda lines, wave: want[:31] + [None]
            self.assertIn("lane 31", d.check(prog))
        finally:
            d.translated = real

    def test_shrinking_keeps_the_instruction_that_matters(self):
        prog = d.generate(d.random.Random(2), 5, ["v_add_nc_u32", "v_xor_b32", "exec"], 32)
        target = next(line for line in prog.body if not line.startswith("s_"))
        real = d.translated
        try:
            d.translated = lambda lines, wave: [tuple(w ^ (target in lines) for w in lane)
                                                for lane in d.reference(lines, wave)]
            small = d.shrink(prog)
        finally:
            d.translated = real
        self.assertEqual(small.body, [target])


class Cli(unittest.TestCase):
    def main(self, *argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            try:
                rc = d.main(list(argv))
            except SystemExit as e:
                rc = e.code
        return rc, out.getvalue()

    def test_exit_codes(self):
        rc, out = self.main("--list-ops")
        self.assertEqual(rc, 0)
        self.assertIn("v_mad_i64_i32", out)
        self.assertRegex(out, r"v_sub_co_ci_u32 +carry, clamp \(e64\)")
        self.assertEqual(self.main("--ops", "v_nope")[0], 2)
        self.assertEqual(self.main("--oracle", "gpu")[0], 2)
        self.assertEqual(self.main("--wave", "16")[0], 2)
        rc, out = self.main("--programs", "4", "--seed", "1", "--wave", "both")
        self.assertEqual(rc, 0, out)
        real = d.translated
        try:
            d.translated = lambda lines, wave: [tuple(w ^ 1 for w in lane) for lane in d.reference(lines, wave)]
            rc, out = self.main("--programs", "2", "--seed", "1", "--ops", "v_xor_b32")
        finally:
            d.translated = real
        self.assertEqual(rc, 1)
        self.assertIn("MISMATCH", out)

    def test_a_run_where_every_program_was_skipped_is_not_a_success(self):
        def refuse(lines, wave):
            raise d.Skip("unsupported form")
        real = d.translated
        try:
            d.translated = refuse
            rc, out = self.main("--programs", "3", "--seed", "1", "--ops", "v_xor_b32")
        finally:
            d.translated = real
        self.assertEqual(rc, 2, out)
        self.assertIn("no program ran: every one was skipped", out)
        self.assertIn("3 skipped", out)

    def test_hardware_oracle_without_the_tools_exits_77(self):
        env = dict(os.environ, LEONIDA_AMD_CLANG=str(TOOL.parent / "no-such-clang.exe"))
        r = subprocess.run([sys.executable, "-I", str(TOOL), "--oracle", "hw", "--programs", "3"],
                           capture_output=True, text=True, env=env)
        self.assertEqual(r.returncode, 77, r.stdout + r.stderr)
        self.assertIn("unavailable", r.stdout)

    def test_hardware_kernel_stores_every_lane(self):
        prog = program(["v_mov_b32_e32 v1, 5"], wave=64)
        k = d.hw_kernel(prog)
        self.assertEqual((k.wave, k.threads, k.words, k.raw), (64, 64, 256, True))
        self.assertEqual(k.lines[-3:], ["v_lshlrev_b32 v20, 4, v0", "global_store_b128 v20, v[8:11], s[2:3]",
                                        "s_endpgm"])
        words = list(range(256))
        self.assertEqual(d.hw_words(words, 64)[1], (4, 5, 6, 7))
        self.assertEqual(d.hw_words("assembler rejected: x", 64), "assembler rejected: x")


class OracleGuards(unittest.TestCase):
    def setUp(self):
        self.orig_known = d.KNOWN

    def tearDown(self):
        d.KNOWN = self.orig_known

    def test_hw_words_refuses_none_or_short_words(self):
        with self.assertRaises(RuntimeError) as ctx:
            d.hw_words(None, 32)
        self.assertIn("no result from the hardware runner", str(ctx.exception))

        with self.assertRaises(RuntimeError) as ctx:
            d.hw_words([0] * 50, 32)
        self.assertIn("the hardware runner returned 50 words (expected 128)", str(ctx.exception))

    def test_check_refuses_none_or_short_lanes(self):
        p = program(["v_mov_b32_e32 v1, 5"], wave=32)
        with self.assertRaises(RuntimeError) as ctx:
            d.check(p, backends=("hw",), hw=None)
        self.assertIn("no result from the hardware runner", str(ctx.exception))

        with self.assertRaises(RuntimeError) as ctx:
            d.check(p, backends=("hw",), hw=[(0, 0, 0, 0)] * 10)
        self.assertIn("the hardware runner returned 10 lanes (expected 32)", str(ctx.exception))

    def test_run_suite_refuses_short_or_none_oracle_results(self):
        mock_oracle = unittest.mock.MagicMock()
        mock_oracle.run.return_value = [[0] * 128] * 2
        with self.assertRaises(RuntimeError) as ctx:
            d.run_suite(programs=3, seed=1, max_ops=2, ops=["v_mov_b32"], backends=("hw",), oracle=mock_oracle)
        self.assertIn("the hardware runner returned 2 results for 3 programs", str(ctx.exception))

        mock_oracle.run.return_value = None
        with self.assertRaises(RuntimeError) as ctx:
            d.run_suite(programs=3, seed=1, max_ops=2, ops=["v_mov_b32"], backends=("hw",), oracle=mock_oracle)
        self.assertIn("the hardware runner returned 0 results for 3 programs", str(ctx.exception))

    def test_main_exits_2_on_oracle_count_mismatch(self):
        mock_oracle = unittest.mock.MagicMock()
        mock_oracle.device = "mock device"
        mock_oracle.run.return_value = [[0] * 128] * 2
        out, err = io.StringIO(), io.StringIO()
        with unittest.mock.patch("hw_oracle.runner.open_oracle", return_value=mock_oracle):
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                rc = d.main(["--oracle", "hw", "--programs", "3", "--seed", "1"])
        self.assertEqual(rc, 2)
        self.assertIn("harness error: the hardware runner returned 2 results for 3 programs", out.getvalue())
        mock_oracle.close.assert_called_once()

    def test_main_shrink_refuses_empty_oracle_result(self):
        mock_oracle = unittest.mock.MagicMock()
        mock_oracle.device = "mock device"
        mock_oracle.run.side_effect = [[[0x11111111] * 128], []]
        out, err = io.StringIO(), io.StringIO()
        with unittest.mock.patch("hw_oracle.runner.open_oracle", return_value=mock_oracle):
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                rc = d.main(["--oracle", "hw", "--programs", "1", "--seed", "1"])
        self.assertEqual(rc, 2)
        self.assertIn("harness error:", out.getvalue())
        mock_oracle.close.assert_called_once()


if __name__ == "__main__":
    unittest.main()
