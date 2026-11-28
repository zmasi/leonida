"""tools/vint_diff.py: the integer VALU reference against hand-worked values, a short seeded fuzz run, and canaries.
Standard library only; no GPU."""
import contextlib
import io
import os
from pathlib import Path
import re
import subprocess
import sys
import unittest

TOOL = Path(__file__).resolve().parents[1] / "tools" / "vint_diff.py"
sys.path.insert(0, str(TOOL.parent))
import vint_diff as d  # noqa: E402


def prefix(v=None, s=None):
    return ([f"v_mov_b32_e32 v{r}, 0x{(v or {}).get(r, 0):x}" for r in d.VREGS]
            + [f"s_mov_b32 s{r}, 0x{(s or {}).get(r, 0):x}" for r in d.SREGS])


def run(*body, v=None, s=None, regs=(1, 2, 3, 4)):
    """The reference's four exported words; v / s map register -> seed value (others are 0)."""
    return d.reference(prefix(v, s) + list(body) + d.observe(list(regs)))


class Reference(unittest.TestCase):
    def test_vop3_only_ops(self):
        self.assertEqual(run("v_add3_u32 v1, v2, s4, 0x10", v={2: 0xFFFFFFF0}, s={4: 4})[0], 4)
        self.assertEqual(run("v_alignbit_b32 v1, v2, v3, 8", v={2: 0x12345678, 3: 0x9ABCDEF0})[0], 0x789ABCDE)
        self.assertEqual(run("v_bfm_b32 v1, 5, 4")[0], 0x1F0)
        self.assertEqual(run("v_bcnt_u32_b32 v1, v2, 3", v={2: 0xF0F0})[0], 11)
        self.assertEqual(run("v_med3_i32 v1, v2, v3, 5", v={2: 0x80000000, 3: 7})[0], 5)
        self.assertEqual(run("v_xad_u32 v1, v2, v3, 1", v={2: 0xFF, 3: 0x0F})[0], 0xF1)
        # saturating packs: (-40000, 40000) -> 0x8000 | 0x7fff << 16; unsigned 0x12345 -> 0xffff
        self.assertEqual(run("v_cvt_pk_i16_i32 v1, v2, v3", v={2: (-40000) & d.M32, 3: 40000})[0], 0x7FFF8000)
        self.assertEqual(run("v_cvt_pk_u16_u32 v1, v2, 7", v={2: 0x12345})[0], 0x0007FFFF)

    def test_bitfield_extract_stops_at_bit_31(self):
        # offset 28, width 8: only bits 28..31 exist, 0x8 sign-extends from bit 3 of the field
        self.assertEqual(run("v_bfe_i32 v1, v2, 28, 8", v={2: 0x80000000})[0], 0xFFFFFFF8)
        self.assertEqual(run("v_bfe_u32 v1, v2, 28, 8", v={2: 0x80000000})[0], 0x8)
        self.assertEqual(run("v_bfe_u32 v1, v2, 4, 0", v={2: M})[0], 0)

    def test_perm_selectors(self):
        # {src0, src1} = 0x80000000_00008000: 8 -> bit 15 (set), 9 -> bit 31, 10 -> bit 47, 11 -> bit 63 (set)
        self.assertEqual(run("v_perm_b32 v1, v2, v3, 0x0b0a0908", v={2: 0x80000000, 3: 0x8000})[0], 0xFF0000FF)
        # 12 -> 0x00, 13 and up -> 0xff, 0..7 -> bytes of src1 then src0
        self.assertEqual(run("v_perm_b32 v1, v2, v3, 0x070d0c00", v={2: 0xAB000000, 3: 0x11})[0], 0xABFF0011)

    def test_24_and_16_bit_multiplies(self):
        self.assertEqual(run("v_mul_hi_i32_i24_e64 v1, v2, 2", v={2: 0x800000})[0], M)       # -2^23 * 2 < 0
        self.assertEqual(run("v_mul_u32_u24_e32 v1, v2, v3", v={2: 0xFF000003, 3: 5})[0], 15)
        # op_sel takes the high halves of the two 16-bit sources: 5 * 2 + 1
        self.assertEqual(run("v_mad_u32_u16 v1, v2, v3, 1 op_sel:[1,0,0,0]", v={2: 0x00050003, 3: 0x00070002})[0],
                         11)
        self.assertEqual(run("v_mad_i32_i16 v1, v2, v3, 0", v={2: 0xFFFF, 3: 3})[0], M - 2)  # -1 * 3

    def test_find_first_bit(self):
        self.assertEqual(run("v_ffbh_i32_e64 v1, v2", v={2: 0xFFFFFFFE})[0], 31)
        self.assertEqual(run("v_ffbh_u32_e32 v1, v2", v={2: 0x00010000})[0], 15)
        self.assertEqual(run("v_ffbl_b32_e32 v1, 0")[0], M)

    def test_constant_sources(self):
        # a float inline constant in an integer op is its f32 bits; a literal goes in src0 of an e32 VOP2
        self.assertEqual(run("v_add_nc_u32_e64 v1, 1.0, 0")[0], 0x3F800000)
        self.assertEqual(run("v_or_b32_e64 v1, 0.15915494, 0")[0], 0x3E22F983)
        self.assertEqual(run("v_xor_b32_e32 v1, 0x12345678, v2", v={2: 0xFF})[0], 0x12345687)
        self.assertEqual(run("v_lshlrev_b32_e64 v1, s4, -16", s={4: 36})[0], 0xFFFFFF00)    # shift by 36 & 31

    def test_integer_clamp_saturates(self):
        # unsigned: to [0, 0xffffffff]; without clamp the same sums wrap
        self.assertEqual(run("v_add_nc_u32_e64 v1, v2, 0x20 clamp", v={2: 0xFFFFFFF0})[0], M)
        self.assertEqual(run("v_add_nc_u32_e64 v1, v2, 0x20", v={2: 0xFFFFFFF0})[0], 0x10)
        self.assertEqual(run("v_sub_nc_u32_e64 v1, 5, v2 clamp", v={2: 7})[0], 0)
        self.assertEqual(run("v_subrev_nc_u32_e64 v1, v2, 3 clamp", v={2: 7})[0], 0)       # 3 - 7
        self.assertEqual(run("v_subrev_nc_u32_e64 v1, 3, v2 clamp", v={2: 7})[0], 4)
        # signed: to [0x80000000, 0x7fffffff], the side x - y or x + y overflowed to
        self.assertEqual(run("v_add_nc_i32 v1, v2, 1 clamp", v={2: 0x7FFFFFFF})[0], 0x7FFFFFFF)
        self.assertEqual(run("v_add_nc_i32 v1, v2, 1", v={2: 0x7FFFFFFF})[0], 0x80000000)
        self.assertEqual(run("v_add_nc_i32 v1, v2, -1 clamp", v={2: 0x80000000})[0], 0x80000000)
        self.assertEqual(run("v_sub_nc_i32 v1, v2, 1 clamp", v={2: 0x80000000})[0], 0x80000000)
        self.assertEqual(run("v_sub_nc_i32 v1, 5, v2 clamp", v={2: 0x80000001})[0], 0x7FFFFFFF)
        self.assertEqual(run("v_sub_nc_i32 v1, v2, v3 clamp", v={2: 0xFFFFFFFF, 3: 0x7FFFFFFF})[0], 0x80000000)
        self.assertEqual(run("v_sub_nc_i32 v1, v2, v3 clamp", v={2: 0xFFFFFFFF, 3: 0x80000000})[0], 0x7FFFFFFF)
        for line in ("v_xor_b32_e64 v1, v2, v3 clamp", "v_add_nc_u32_e32 v1, v2, v3 clamp", "s_mov_b32 s4, 1 clamp",
                     "v_cmp_eq_u32_e64 s20, v2, v3 clamp", "v_lshlrev_b64 v[2:3], 1, v[4:5] clamp"):
            with self.assertRaises(d.RefError):
                run(line)

    def test_64bit_shifts(self):
        pair = {4: 0x89ABCDEF, 5: 0x01234567}
        self.assertEqual(run("v_lshlrev_b64 v[2:3], 4, v[4:5]", v=pair, regs=(2, 3, 4, 5))[:2],
                         [0x9ABCDEF0, 0x12345678])
        # S0 & 63: 100 shifts by 36; an SGPR pair as S1
        self.assertEqual(run("v_lshrrev_b64 v[2:3], v1, s[6:7]", v={1: 100}, s={7: 0x80000000}, regs=(2, 3, 1, 1))[:2],
                         [0x08000000, 0])
        self.assertEqual(run("v_lshrrev_b64 v[2:3], s4, v[4:5]", v=pair, s={4: 100}, regs=(2, 3, 1, 1))[:2],
                         [0x00123456, 0])
        self.assertEqual(run("v_ashrrev_i64 v[2:3], 36, s[6:7]", s={7: 0x80000000}, regs=(2, 3, 1, 1))[:2],
                         [0xF8000000, M])
        # an integer inline constant is sign-extended to 64 bits
        self.assertEqual(run("v_lshrrev_b64 v[2:3], 32, -2", regs=(2, 3, 1, 1))[:2], [M, 0])
        self.assertEqual(run("v_lshrrev_b64 v[2:3], 60, 64", regs=(2, 3, 1, 1))[:2], [0, 0])
        # an odd first register, and a destination that overlaps the source: S1 is read first
        self.assertEqual(run("v_lshlrev_b64 v[3:4], 32, v[4:5]", v=pair, regs=(3, 4, 5, 5))[:3],
                         [0, 0x89ABCDEF, 0x01234567])
        # the 64-bit shifts read one scalar value (GFX10): a second one does not assemble
        for line in ("v_lshlrev_b64 v[2:3], 1, 0x12345678", "v_lshlrev_b64 v[2:3], 1, 1.0",
                     "v_lshlrev_b64 v[2:3], 1, v[4:6]", "v_lshrrev_b64 v[2:3], 0x12345678, s[4:5]",
                     "v_lshrrev_b64 v[2:3], s4, s[6:7]", "v_ashrrev_i64 v[2:3], s9, s[4:5]"):
            with self.assertRaises(d.RefError):
                run(line)

    def test_compare_and_select(self):
        self.assertEqual(run("v_cmp_lt_i32_e64 s20, v2, v3", "v_cndmask_b32_e64 v1, 1, 2, s20",
                             v={2: 0x80000000, 3: 0})[0], 2)
        self.assertEqual(run("v_cmp_lt_u32_e64 vcc_lo, v2, v3", "v_cndmask_b32_e64 v1, 1, 2, vcc_lo",
                             v={2: 0x80000000, 3: 0})[0], 1)


M = d.M32


class Generator(unittest.TestCase):
    def lines(self, known=False, literals=True, n=3000, seed=1):
        real = d.KNOWN, d.LITERALS
        d.KNOWN, d.LITERALS = known, literals
        try:
            rng = d.random.Random(seed)
            gen = d.Gen(rng)
            return [line for _ in range(n) for line in gen.op(d.ALL_OPS)]
        finally:
            d.KNOWN, d.LITERALS = real

    def test_every_op_is_generated_in_every_form(self):
        lines = self.lines()
        seen = {re.sub(r"_e(32|64)$", "", line.split()[0]) for line in lines}
        self.assertEqual(set(d.OPS) - seen, set())
        forms = {line.split()[0] for line in lines}
        for name, (enc, _, _) in d.OPS.items():
            if enc in ("vop1", "vop2"):
                self.assertTrue({name + "_e32", name + "_e64"} <= forms, name)
        self.assertTrue(any(line.startswith("v_cmp_") for line in lines))

    def test_clamp_only_where_it_is_modelled(self):
        lines = self.lines()
        clamped = {line.split()[0] for line in lines if line.endswith(" clamp")}
        self.assertEqual(clamped, {"v_add_nc_u32_e64", "v_sub_nc_u32_e64", "v_subrev_nc_u32_e64", "v_add_nc_i32",
                                   "v_sub_nc_i32"})
        self.assertFalse(any(" clamp" in line[:-len(" clamp")] for line in lines))

    def test_64bit_shifts_take_every_source_kind(self):
        lines = [line for line in self.lines() if line.split()[0] in d.OPS64]
        kinds = set()
        for line in lines:
            dst, sh, src = (a.strip() for a in line.partition(" ")[2].split(","))
            m = re.fullmatch(r"v\[(\d+):(\d+)\]", dst)
            self.assertTrue(m and int(m.group(2)) == int(m.group(1)) + 1, line)
            self.assertTrue({int(m.group(1)), int(m.group(2))} <= set(d.VREGS), line)
            if re.fullmatch(r"s\[(\d+):(\d+)\]", src):
                kinds.add("sgpr pair")
                self.assertEqual(int(src[2:src.index(":")]) % 2, 0, line)
            elif src.startswith("v["):
                kinds.add("odd vgpr pair" if int(src[2:src.index(":")]) % 2 else "even vgpr pair")
            else:
                kinds.add("negative constant" if int(src) < 0 else "constant")
            kinds.add("shift " + ("literal" if sh.startswith("0x") else "sgpr" if sh.startswith("s") else
                                  "vgpr" if sh.startswith("v") else "float" if sh in d.INLINE_F else "int"))
        self.assertEqual(kinds, {"sgpr pair", "odd vgpr pair", "even vgpr pair", "negative constant", "constant",
                                 "shift literal", "shift sgpr", "shift vgpr", "shift float", "shift int"})

    def test_sources_respect_the_encoding_rules(self):
        for line in self.lines():
            op, _, rest = line.partition(" ")
            srcs = [a.strip() for a in rest.split(" op_sel:")[0].removesuffix(" clamp").split(",")][1:]
            if op == "v_cndmask_b32_e64":
                srcs = srcs[:-1]
            lits = [x for x in srcs if x.startswith("0x")]
            scalars = set(x for x in srcs if x.startswith(("s", "vcc"))) | set(lits)
            if op.endswith("_e32"):
                self.assertTrue(len(srcs) == 1 or re.fullmatch(r"v\d+", srcs[1]), line)
            self.assertLessEqual(len(lits), 1, line)
            limit = 0 if op.startswith("v_cndmask") else 1 if op in d.OPS64 else 2
            self.assertLessEqual(len(scalars), limit, line)
        self.assertFalse(any("0x" in line.split(",", 1)[1] for line in self.lines(literals=False)))

    def test_16bit_sources_take_16bit_literals(self):
        for line in self.lines():
            if line.startswith(d.OP_SEL_OPS):
                srcs = [a.strip() for a in line.split(" op_sel:")[0].split(",")][1:3]
                self.assertTrue(all(int(x, 16) <= 0xFFFF for x in srcs if x.startswith("0x")), line)
        # measured on gfx1036: such a literal is zero-extended, a float constant is its f32 bits
        self.assertEqual(run("v_mad_u32_u16 v1, 0xffff, 1, 0 op_sel:[1,0,0,0]")[0], 0)
        self.assertEqual(run("v_mad_u32_u16 v1, 0.5, 1, 0 op_sel:[1,0,0,0]")[0], 0x3F00)

    def test_known_divergences_are_left_out_by_default(self):
        def mask_as_data(lines):
            for line in lines:
                op, _, rest = line.partition(" ")
                srcs = [a.strip() for a in rest.split(",")][1:]
                if op == "v_cndmask_b32_e64":
                    srcs = srcs[:-1]
                if any(x in d.MASKS for x in srcs):
                    return True
            return False
        self.assertFalse(mask_as_data(self.lines()))
        self.assertTrue(mask_as_data(self.lines(known=True)))


class Translated(unittest.TestCase):
    def test_harness_canaries(self):
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertTrue(d.self_test())

    def test_short_seeded_run_matches(self):
        counts, failures = d.run_suite(80, 5, 5, d.ALL_OPS)
        self.assertEqual(failures, [], failures[:1])
        self.assertEqual(counts["skip"], {})
        self.assertEqual(counts["pass"], 80)

    def test_hand_cases_match_the_translator(self):
        seeds = {2: 0x80000000, 3: 0x00008000, 4: 0x00050003, 5: 0x00070002, 6: 0xFFFFFFFE}
        cases = ["v_bfe_i32 v1, v2, 28, 8", "v_perm_b32 v1, v2, v3, 0x0b0a0908", "v_perm_b32 v1, v2, v3, 0x070d0c00",
                 "v_mad_u32_u16 v1, v4, v5, 1 op_sel:[1,0,0,0]", "v_ffbh_i32_e64 v1, v6",
                 "v_add_nc_u32_e64 v1, 1.0, s4", "v_xor_b32_e32 v1, 0x12345678, v2",
                 "v_lshlrev_b32_e64 v1, s5, -16", "v_cvt_pk_i16_i32 v1, v6, v4", "v_mul_hi_i32_i24_e32 v1, -2, v3",
                 "v_alignbit_b32 v1, v4, v6, 8", "v_mad_u32_u16 v1, 0xffff, 0.5, v3 op_sel:[1,1,0,0]"]
        for case in cases:
            with self.subTest(case=case):
                self.assertIsNone(d.check(prefix(seeds, {4: 7, 5: 36}) + [case] + d.observe([1, 2, 3, 4])))

    def test_clamp_and_64bit_shift_cases_match_the_translator(self):
        seeds = {2: 0x7FFFFFFF, 3: 0x80000000, 4: 0x89ABCDEF, 5: 0x01234567, 6: 1, 7: M}
        cases = ["v_add_nc_i32 v1, v2, v6 clamp", "v_add_nc_i32 v1, v2, v6", "v_sub_nc_i32 v1, v3, v6 clamp",
                 "v_sub_nc_i32 v1, v2, v7 clamp", "v_sub_nc_i32 v1, v7, s5 clamp", "v_add_nc_u32_e64 v1, v7, 2 clamp",
                 "v_sub_nc_u32_e64 v1, v6, v2 clamp", "v_subrev_nc_u32_e64 v1, v2, s4 clamp",
                 "v_lshlrev_b64 v[2:3], 4, v[4:5]", "v_lshrrev_b64 v[2:3], v4, s[6:7]",
                 "v_ashrrev_i64 v[2:3], 36, s[6:7]", "v_ashrrev_i64 v[1:2], v4, -16",
                 "v_lshlrev_b64 v[3:4], 0x21, v[4:5]", "v_lshrrev_b64 v[2:3], 1.0, v[4:5]",
                 "v_lshrrev_b64 v[2:3], s4, v[4:5]"]
        for case in cases:
            with self.subTest(case=case):
                lines = prefix(seeds, {4: 100, 5: 0x80000000, 7: 0x80000000}) + [case] + d.observe([1, 2, 3, 4])
                self.assertIsNone(d.check(lines))

    def test_a_wrong_translation_is_caught(self):
        lines = prefix({2: 5}) + ["v_add3_u32 v1, v2, 1, 2"] + d.observe([1, 2, 3, 4])
        self.assertIsNone(d.check(lines))
        want = d.reference(lines)
        real = d.translated
        try:
            d.translated = lambda _lines: [want[0] ^ 1] + want[1:]
            self.assertIn("word 0 (v1)", d.check(lines))
        finally:
            d.translated = real

    def test_shrinking_keeps_the_instruction_that_matters(self):
        lines = d.generate(d.random.Random(2), 5, ["v_add_nc_u32", "v_xor_b32", "cmp"])
        body = lines[d.N_SEED:-d.N_OBS]
        target = body[len(body) // 2]
        if target.startswith("v_cmp"):
            target = body[len(body) // 2 + 1]
        real = d.translated
        try:
            d.translated = lambda l: [w ^ (1 if target in l else 0) for w in d.reference(l)]
            small = d.shrink(lines)
        finally:
            d.translated = real
        kept = [line for line in small if not line.startswith(("v_mov_b32_e32", "s_mov_b32"))]
        self.assertIn(target, kept)
        self.assertLessEqual(len(kept), 2)                       # the target (and its compare, for a v_cndmask)


    def test_shrinking_keeps_the_seeds_of_a_pair(self):
        lines = prefix(s={6: 0x10, 7: 0x80000000}) + ["v_add_nc_u32_e32 v5, 1, v6", "v_ashrrev_i64 v[2:3], 4, s[6:7]"]
        lines += d.observe([2, 3, 5, 1])
        real = d.translated
        try:
            d.translated = lambda l: [w ^ (1 if any("_i64" in x for x in l) else 0) for w in d.reference(l)]
            small = d.shrink(lines)
        finally:
            d.translated = real
        self.assertIn("s_mov_b32 s6, 0x10", small)
        self.assertIn("s_mov_b32 s7, 0x80000000", small)
        self.assertNotIn("v_add_nc_u32_e32 v5, 1, v6", small)


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
        self.assertIn("v_perm_b32", out)
        self.assertRegex(out, r"v_ashrrev_i64 +VOP3 only, 64-bit pair")
        self.assertRegex(out, r"v_sub_nc_i32 +VOP3 only, 2 sources, clamp")
        self.assertEqual(self.main("--ops", "v_nope")[0], 2)
        self.assertEqual(self.main("--oracle", "gpu")[0], 2)
        rc, out = self.main("--programs", "5", "--seed", "1")
        self.assertEqual(rc, 0, out)
        real = d.translated
        try:
            d.translated = lambda l: [w ^ 1 for w in d.reference(l)]
            rc, out = self.main("--programs", "3", "--seed", "1", "--ops", "v_xor_b32")
        finally:
            d.translated = real
        self.assertEqual(rc, 1)
        self.assertIn("MISMATCH", out)

    def test_a_run_where_every_program_was_skipped_is_not_a_success(self):
        def refuse(lines, *args):
            raise d.Skip("unsupported form")
        real = d.translated
        try:
            d.translated = refuse
            rc, out = self.main("--programs", "3", "--seed", "1", "--ops", "v_xor_b32")
        finally:
            d.translated = real
        self.assertEqual(rc, 2, out)
        self.assertIn("no program ran: every one was skipped", out)

    def test_hardware_oracle_without_the_tools_exits_77(self):
        env = dict(os.environ, LEONIDA_AMD_CLANG=str(TOOL.parent / "no-such-clang.exe"))
        r = subprocess.run([sys.executable, "-I", str(TOOL), "--oracle", "hw", "--programs", "3"],
                           capture_output=True, text=True, env=env)
        self.assertEqual(r.returncode, 77, r.stdout + r.stderr)
        self.assertIn("unavailable", r.stdout)

    def test_main_restores_globals_and_rejects_empty_string_options(self):
        self.assertFalse(d.KNOWN)
        self.assertTrue(d.LITERALS)
        rc, _ = self.main("--known", "--no-literals", "--list-ops")
        self.assertEqual(rc, 0)
        self.assertFalse(d.KNOWN)
        self.assertTrue(d.LITERALS)

        rc, _ = self.main("--known", "--no-literals", "--programs", "2", "--seed", "1", "--ops", "v_xor_b32")
        self.assertEqual(rc, 0)
        self.assertFalse(d.KNOWN)
        self.assertTrue(d.LITERALS)

        for flag in ("--hw-arch", "--build", "--work"):
            for bad in ("", "   "):
                with self.subTest(flag=flag, bad=bad):
                    rc, _ = self.main(flag, bad, "--list-ops")
                    self.assertEqual(rc, 2)

    def test_hw_oracle_result_count_word_length_and_shrink_guards(self):
        from unittest import mock
        from hw_oracle import runner

        with self.assertRaisesRegex(RuntimeError, "hardware oracle required"):
            d.hw_run(None, [["v_mov_b32_e32 v1, 0"]])

        class StubOracle:
            device = "stub-rdna2"

            def __init__(self, responses):
                self._responses = list(responses)
                self.closed = False

            def run(self, kernels):
                return self._responses.pop(0)

            def close(self):
                self.closed = True

        stub_short = StubOracle([[[0, 0, 0, 0]]])
        with mock.patch.object(runner, "open_oracle", return_value=stub_short):
            rc, out = self.main("--oracle", "hw", "--programs", "2", "--ops", "v_xor_b32")
        self.assertEqual(rc, 2, out)
        self.assertIn("harness error", out)
        self.assertTrue(stub_short.closed)

        stub_words = StubOracle([[[0, 1]]])
        with mock.patch.object(runner, "open_oracle", return_value=stub_words):
            rc, out = self.main("--oracle", "hw", "--programs", "1", "--ops", "v_xor_b32")
        self.assertEqual(rc, 2, out)
        self.assertIn("expected 4 words", out)
        self.assertTrue(stub_words.closed)

        stub_shrink = StubOracle([[[0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF]], []])
        with mock.patch.object(runner, "open_oracle", return_value=stub_shrink):
            rc, out = self.main("--oracle", "hw", "--programs", "1", "--max-ops", "1", "--ops", "v_xor_b32")
        self.assertEqual(rc, 2, out)
        self.assertIn("harness error", out)
        self.assertTrue(stub_shrink.closed)

    def test_shrink_final_recheck_skip_falls_back_to_original_problem(self):
        from unittest import mock

        calls = {"n": 0}

        def flaky_translated(lines):
            calls["n"] += 1
            if calls["n"] == 1:
                return [w ^ 1 for w in d.reference(lines)]
            raise d.Skip("transient skip during shrink recheck")

        with mock.patch.object(d, "translated", flaky_translated):
            rc, out = self.main("--programs", "1", "--seed", "1", "--max-ops", "1", "--ops", "v_xor_b32")
        self.assertEqual(rc, 1, out)
        self.assertIn("MISMATCH program 0:", out)

    def test_reference_rejects_bad_operands_compares_64bit_dst_and_op_sel(self):
        bad_lines = (
            "v_add3_u32 v1, v2, v3, v4 op_sel:[1,0,0,0]",
            "v_mad_u32_u16 v1, v2, v3, v4 op_sel:[1,0]",
            "v_mad_u32_u16 v1, v2, v3, v4 op_sel:[0,0,1,0]",
            "v_cmp_bad_u32_e64 s20, v2, v3",
            "v_cmp_eq_u32_e32 s20, v2, v3",
            "v_cmp_eq_u32_e64 v1, v2, v3",
            "v_cndmask_b32_e64 s4, 1, 2, s20",
            "v_lshlrev_b64 v2, 1, v[4:5]",
            "v_lshlrev_b64 v[2:4], 1, v[4:5]",
            "v_lshlrev_b64 v[2:3], 1, v4",
            "v_add3_u32 v1, bogus, v3, v4",
            "v_add3_u32 s1, v2, v3, v4",
            "v_add3_u32 v1, v2, v3",
            "s_mov_b32 v1, 0",
        )
        for bad in bad_lines:
            with self.subTest(line=bad):
                with self.assertRaises(d.RefError):
                    run(bad)


if __name__ == "__main__":
    unittest.main()
