"""tools/vrel_diff.py: the reference against hand-worked values, the generator's coverage, short seeded runs through
the translator, and canaries. Standard library only; no GPU."""
import contextlib
import io
from pathlib import Path
import sys
import unittest

TOOL = Path(__file__).resolve().parents[1] / "tools" / "vrel_diff.py"
sys.path.insert(0, str(TOOL.parent))
import vrel_diff as d  # noqa: E402


def program(body, wave=32):
    """v0 = the lane, v<r> = (lane << 16) + 0xa000 + r for r = 1..31, s4 = 0x1234, s5 = 3, M0 = 0."""
    init = ["v_mbcnt_lo_u32_b32 v0, -1, 0"] + (["v_mbcnt_hi_u32_b32 v0, -1, v0"] if wave == 64 else [])
    for r in range(1, d.WINDOW):
        init += [f"v_mul_lo_u32 v{r}, v0, 0x10000", f"v_add_nc_u32_e32 v{r}, 0x{0xA000 + r:x}, v{r}"]
    init += ["s_mov_b32 s4, 0x1234", "s_mov_b32 s5, 3", "s_mov_b32 m0, 0"]
    return d.Program(init, list(body), wave)


def start(lane, r):
    return lane if r == 0 else (lane << 16) + 0xA000 + r


def run(body, wave=32):
    return d.reference(program(body, wave).lines(), wave)


class Reference(unittest.TestCase):
    def test_movrelsd_2_uses_two_fields_and_ignores_the_rest(self):
        # RDNA2 ISA example: M0[25:16] = 20 and M0[9:0] = 10 give v25 = v17
        for m0 in ("0x14000a", "0xfc14fc0a"):
            out = run([f"s_mov_b32 m0, {m0}", "v_movrelsd_2_b32 v5, v7"])
            self.assertEqual(out[3][25], start(3, 17), m0)
            self.assertEqual(out[3][17], start(3, 17), m0)

    def test_whole_m0_forms(self):
        out = run(["s_mov_b32 m0, 3", "v_movrels_b32 v1, v20", "v_movrelsd_b32 v2, v10", "s_mov_b32 m0, s5",
                   "v_movreld_b32 v20, v0"])
        self.assertEqual(out[4][1], start(4, 23))        # v1 = v[20 + 3]
        self.assertEqual(out[4][5], start(4, 13))        # v[2 + 3] = v[10 + 3]
        self.assertEqual(out[4][23], 4)                  # v[20 + 3] = v0, the lane

    def test_movreld_takes_any_vop1_source(self):
        cases = (("s4", 0x1234), ("m0", 1), ("-3", 0xFFFFFFFD), ("64", 64), ("0.15915494", 0x3E22F983),
                 ("-4.0", 0xC0800000), ("0x12345678", 0x12345678))
        for src, want in cases:
            for form in ("v_movreld_b32", "v_movreld_b32_e64"):
                out = run(["s_mov_b32 m0, 1", f"{form} v20, {src}"])
                self.assertEqual([lane[21] for lane in out], [want] * 32, (form, src))

    def test_swaprel_and_swap(self):
        out = run(["s_mov_b32 m0, 0x20003", "v_swaprel_b32 v1, v4", "v_swap_b32 v9, v10"])
        self.assertEqual((out[2][3], out[2][7]), (start(2, 7), start(2, 3)))
        self.assertEqual((out[2][9], out[2][10]), (start(2, 10), start(2, 9)))
        out = run(["s_mov_b32 m0, 0x30001", "v_swaprel_b32 v1, v3"])     # v4 with itself
        self.assertEqual(out[2][4], start(2, 4))

    def test_inactive_lanes_keep_every_register(self):
        out = run(["s_mov_b32 exec_lo, 0x5", "s_mov_b32 m0, 0x20003", "v_swaprel_b32 v1, v4", "s_mov_b32 m0, 1",
                   "v_movreld_b32 v8, s4"])
        self.assertEqual((out[2][3], out[2][9]), (start(2, 7), 0x1234))
        self.assertEqual((out[1][3], out[1][9]), (start(1, 3), start(1, 9)))

    def test_wave64_exec_hi(self):
        out = run(["s_mov_b32 exec_hi, 0x80000000", "s_mov_b32 m0, 2", "v_movrels_b32 v1, v0"], wave=64)
        self.assertEqual(out[63][1], start(63, 2))
        self.assertEqual(out[62][1], start(62, 1))
        self.assertEqual(out[63][0], 63)

    def test_outside_the_model_is_refused(self):
        for body in (["s_mov_b32 m0, 2", "v_movreld_b32 v30, s4"],              # v32
                     ["s_mov_b32 m0, 1", "v_movrels_b32 v1, v31"],
                     ["s_mov_b32 m0, 0x200000", "v_movrelsd_2_b32 v0, v1"],     # M0[25:16] = 32
                     ["s_mov_b32 m0, 0x20", "v_swaprel_b32 v0, v1"],            # M0[9:0] = 32
                     ["v_movrels_b32 v1, s4"],
                     ["v_movreld_b32 v1, v2 row_shl:1"]):
            with self.subTest(body=body), self.assertRaises(d.RefError):
                run(body)


class Generator(unittest.TestCase):
    def programs(self, n=400, wave=32, seed=1, ops=None):
        rng = d.random.Random(seed)
        return [d.generate(rng, rng.randint(1, 8), ops, wave) for _ in range(n)]

    def test_every_op_and_case_is_generated(self):
        ev = d.Counter()
        for wave in (32, 64):
            for p in self.programs(wave=wave):
                ev.update(d.events(p))
        want = set(d.OPS) | {"e64 form", "ignored M0 bits set", "one register on both sides", "partial EXEC"}
        want |= {f"v_movreld_b32 {k} source" for k in ("vgpr", "sgpr", "m0", "inline integer", "inline float",
                                                       "literal")}
        self.assertEqual(want - set(ev), set())

    def test_programs_stay_in_the_window(self):
        for wave in (32, 64):
            for p in self.programs(wave=wave, seed=3):
                d.reference(p.lines(), wave)                 # raises RefError on an index outside v0..v31

    def test_known_divergences_are_left_out(self):
        body = [line for p in self.programs(n=600, ops=["v_movreld_b32"]) for line in p.body]
        srcs = {line.split(",")[1].strip() for line in body if line.startswith("v_movreld_b32")}
        self.assertFalse(srcs & {"exec_lo", "exec_hi", "vcc_lo", "vcc_hi"})
        self.assertFalse([line for line in body if "dpp" in line or "row_" in line or "sdwa" in line])

    def test_ops_restricts_the_relative_moves(self):
        for p in self.programs(n=100, ops=["v_swaprel_b32"]):
            self.assertEqual({op for op in d.events(p) if op in d.OPS} - {"v_swaprel_b32"}, set())


class Translated(unittest.TestCase):
    def test_harness_canaries(self):
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertTrue(d.self_test())

    def test_short_seeded_run_matches(self):
        counts, failure = d.run_suite(12, 7, 6, None, (32, 64))
        self.assertIsNone(failure, failure and failure[3])
        self.assertEqual(counts["skip"], {})
        self.assertEqual(counts["pass"], 12)

    def test_every_movreld_source_translates(self):
        # VOP1 and VOP3 forms with an SGPR, M0, inline integer and float constants and a literal, under a partial
        # EXEC; rdna2spirv refused all but a VGPR before
        body = ["s_mov_b32 exec_lo, 0xfff0ff0f", "s_mov_b32 m0, 1", "v_movreld_b32 v1, s4",
                "v_movreld_b32_e64 v2, s5", "v_movreld_b32 v3, m0", "v_movreld_b32 v4, -3",
                "v_movreld_b32_e64 v5, 64", "v_movreld_b32 v6, 0.15915494", "v_movreld_b32_e64 v7, -0.5",
                "v_movreld_b32 v8, 0x12345678", "v_movreld_b32_e64 v9, 0xdeadbeef", "v_movreld_b32 v10, v0"]
        for wave in (32, 64):
            with self.subTest(wave=wave):
                self.assertIsNone(d.check(program(body, wave)))

    def test_two_field_cases_translate(self):
        body = ["s_mov_b32 exec_lo, 0x0000ff0f", "s_mov_b32 m0, 0xfc14fc0a", "v_movrelsd_2_b32 v5, v7",
                "v_swaprel_b32 v1, v3", "s_mov_b32 m0, 0x30001", "v_swaprel_b32 v1, v3", "v_movrelsd_2_b32 v2, v1"]
        self.assertIsNone(d.check(program(body)))

    def test_a_wrong_translation_is_caught(self):
        prog = program(["s_mov_b32 m0, 2", "v_movrels_b32 v1, v20"])
        want = d.reference(prog.lines(), 32)
        real = d.translated
        try:
            d.translated = lambda lines, wave: [w if lane != 9 else (w[0], w[1] ^ 4) + w[2:]
                                                for lane, w in enumerate(want)]
            self.assertIn("lane 9 v1", d.check(prog))
            d.translated = lambda lines, wave: want[:31] + [None]
            self.assertIn("lane 31", d.check(prog))
        finally:
            d.translated = real

    def test_shrinking_keeps_the_instruction_that_matters(self):
        prog = d.generate(d.random.Random(2), 6, ["v_swaprel_b32"])
        target = next(line for line in prog.body if line.startswith("v_swaprel_b32"))
        fake = lambda lines, wave: [tuple(w ^ (target in lines) for w in lane)    # noqa: E731
                                    for lane in d.reference(lines, wave)]
        small = d.shrink(prog, lambda p: d.check(p, fake))
        self.assertIn(target, small.body)
        self.assertLess(len(small.body), len(prog.body))

    def test_each_program_is_checked_before_the_next_is_made(self):
        # a run holds one program at a time, not the whole --programs list
        made, seen = [], []
        real = d.generate, d.check
        try:
            d.generate = lambda *a: made.append(1) or real[0](*a)
            d.check = lambda prog, run=None: seen.append(len(made))
            d.run_suite(6, 1)
        finally:
            d.generate, d.check = real
        self.assertEqual(seen, [1, 2, 3, 4, 5, 6])


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
        self.assertRegex(out, r"v_movrelsd_2_b32 +VGPR\[D \+ M0\[25:16\]\]")
        self.assertEqual(self.main("--ops", "v_nope")[0], 2)
        self.assertEqual(self.main("--wave", "16")[0], 2)
        for argv in (("--programs", "-1"), ("--programs", "0"), ("--max-ops", "0"), ("--max-ops", "-3")):
            rc, out = self.main(*argv)
            self.assertEqual(rc, 2, argv)
            self.assertNotIn("programs match", out)
        rc, out = self.main("--programs", "2", "--seed", "1", "--wave", "both")
        self.assertEqual(rc, 0, out)
        self.assertIn("2/2 programs match", out)
        real = d.translated
        try:
            d.translated = lambda lines, wave: [tuple(w ^ 1 for w in lane) for lane in d.reference(lines, wave)]
            rc, out = self.main("--programs", "2", "--seed", "1", "--ops", "v_swap_b32")
            self.assertEqual(rc, 1)
            self.assertIn("first mismatch", out)

            def skip(lines, wave):
                raise d.Skip("translator unsupported: test")
            d.translated = skip
            rc, out = self.main("--programs", "2", "--seed", "1")
            self.assertEqual(rc, 1)
            self.assertIn("2 skipped", out)
        finally:
            d.translated = real

    def test_empty_and_malformed_ops_tokens_rejected_and_duplicates_deduped(self):
        for bad in ("", "   ", ",", " , ", "v_swap_b32,", ",v_swap_b32", "v_swap_b32,,v_movrels_b32"):
            with self.subTest(ops=bad):
                rc, out = self.main("--programs", "1", "--ops", bad)
                self.assertEqual(rc, 2, (bad, out))
                self.assertNotIn("programs match", out)

        from unittest import mock
        seen_ops = []
        real_run_suite = d.run_suite

        def spy(programs, seed, max_ops=8, ops=None, waves=(32,), run=None):
            seen_ops.append(ops)
            return real_run_suite(programs, seed, max_ops, ops, waves, run)

        with mock.patch.object(d, "run_suite", spy):
            rc, out = self.main("--programs", "1", "--seed", "1", "--ops", "v_swap_b32,v_swap_b32")
        self.assertEqual(rc, 0, out)
        self.assertEqual(seen_ops, [["v_swap_b32"]])

    def test_compare_rejects_mismatched_lane_and_word_counts(self):
        want = [tuple(range(d.WINDOW)) for _ in range(32)]
        self.assertIn("lane count mismatch", d.compare(want, []))
        self.assertIn("lane count mismatch", d.compare(want, want[:31]))
        self.assertIn("expected 32 words", d.compare(want, [want[0][:4]] + want[1:]))

    def test_run_suite_handles_skip_on_final_shrunk_recheck(self):
        calls = {"n": 0}

        def flaky(lines, wave):
            calls["n"] += 1
            if calls["n"] == 1:
                return [tuple(w ^ 1 for w in lane) for lane in d.reference(lines, wave)]
            raise d.Skip("transient skip")

        counts, failure = d.run_suite(1, 1, 1, ["v_swap_b32"], (32,), run=flaky)
        self.assertEqual(counts["mismatch"], 1)
        self.assertIsNotNone(failure)
        self.assertIn("lane 0", failure[3])

    def test_reference_and_events_raise_ref_error_on_malformed_inputs(self):
        bad_bodies = (
            ["s_mov_b32 m0"],
            ["s_add_u32 m0, m0"],
            ["v_mov_b32 v1"],
            ["v_add_nc_u32 v1, v2"],
            ["v_writelane_b32 v1, s4, nope"],
            ["v_movrels_b32 v1"],
            ["v_movrels_b32 v1, "],
            ["v_movreld_b32 , v1"],
        )
        for body in bad_bodies:
            with self.subTest(body=body):
                with self.assertRaises(d.RefError):
                    run(body)

        # events() with uninitialized m0 on a two-field op raises RefError instead of TypeError.
        prog_no_m0 = d.Program([], ["v_movrelsd_2_b32 v5, v7"], 32)
        with self.assertRaises(d.RefError):
            d.events(prog_no_m0)


if __name__ == "__main__":
    unittest.main()
