"""tools/cf_gpu_run.py: the FUZZ-8 programs of tools/cf_diff.py run on a GPU and compared with cf_ref, lane by lane.
Standard library only; no GPU, no llvm-mc, no game data. The GPU side is replaced by what the test wants it to return.

    python -I tests/test_cf_gpu_run.py
"""
import contextlib
import io
import json
import sys
import tempfile
import unittest
import unittest.mock as mock
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
sys.dont_write_bytecode = True

import cf_diff  # noqa: E402
import cf_gpu_run as G  # noqa: E402

# A divergent if (lanes below 16), then a probe every lane runs.
BODY = [("if", ("lt", "u32", 16, 0), True, [("probe", 5)]), ("probe", 6)]
WRONG = [("if", ("ge", "u32", 16, 0), True, [("probe", 5)]), ("probe", 6)]


def flipped(rows):
    rows = [list(lane) for lane in rows]
    rows[3][3] ^= 1
    return rows


def run_main(argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        status = G.main(argv)
    return status, out.getvalue(), err.getvalue()


class Listing(unittest.TestCase):
    def test_the_export_becomes_four_per_lane_stores_and_the_branches_are_untouched(self):
        for wave in (32, 64):
            with self.subTest(wave=wave):
                original = cf_diff.build(BODY, wave)
                text = G.compute_listing(BODY, wave, "cs_x").splitlines()
                self.assertEqual(text[:3], ["; cs_x", f"; group: {wave},1,1", "; rsrc2: 0x8088"])
                self.assertEqual("; mattr: +wavefrontsize64,-wavefrontsize32" in text, wave == 64)
                instructions = [ln.split(";")[0].strip() for ln in text if ln and not ln.startswith(";")]
                self.assertEqual(instructions[0], "s_mov_b64 exec, -1" if wave == 64 else "s_mov_b32 exec_lo, -1")
                # everything cf_diff generated before its export is kept, in order, between the prologue and the stores
                kept = [ln for ln, _size in original[:-2]]
                self.assertEqual(instructions[1:1 + len(kept)], kept)
                self.assertEqual(instructions[-1], "s_endpgm")
                stores = [ln for ln in instructions if ln.startswith("buffer_store_dword")]
                self.assertEqual([ln.split(",")[0].split()[-1] for ln in stores], ["v8", "v9", "v10", "v11"])
                self.assertEqual([("offset:" in ln) for ln in stores], [False, True, True, True])
                self.assertFalse(any(ln.startswith("exp ") for ln in instructions))

    def test_a_program_without_an_export_is_refused(self):
        with mock.patch.object(cf_diff, "build", return_value=[("s_nop 0", 4), ("s_endpgm", 4)]):
            with self.assertRaisesRegex(ValueError, "exp"):
                G.compute_listing(BODY, 32, "cs_x")

    def test_shapes(self):
        self.assertEqual(G.shapes(BODY), {"if"})
        self.assertEqual(G.shapes([("probe", 1), ("op", "x")]), {"plain"})
        nested = [("ifelse", ("lt", "u32", 4, 0), [("if", ("lt", "u32", 2, 0), True, [("probe", 1)])], [("probe", 2)])]
        self.assertEqual(G.shapes(nested), {"ifelse", "if"})


class Comparison(unittest.TestCase):
    def test_first_difference_names_the_lane_register_and_words(self):
        want = [[1, 2, 3, 4], [5, 6, 7, 8]]
        self.assertIsNone(G.first_difference(want, [list(lane) for lane in want]))
        got = [[1, 2, 3, 4], [5, 6, 9, 8]]
        self.assertEqual(G.first_difference(want, got),
                         {"lane": 1, "register": "v10", "expected": "0x00000007", "actual": "0x00000009"})

    def test_reference_is_cf_refs_export_per_lane(self):
        with tempfile.TemporaryDirectory() as tmp:
            rows = G.reference(BODY, 32, "ps_x", tmp)
            wrong = G.reference(WRONG, 32, "ps_w", tmp)
        self.assertEqual(len(rows), 32)
        self.assertTrue(all(len(lane) == 4 and G.FILL not in lane for lane in rows))   # every lane exported
        self.assertNotEqual(rows, wrong)           # the branch sense changes what the lanes record (the control)
        self.assertEqual(rows, G.reference(BODY, 32, "ps_y", tempfile.gettempdir()))   # and it is deterministic

    def test_the_gpu_is_asked_for_encoded_per_lane_words(self):
        seen = {}

        def fake(texts, **kwargs):
            seen.update(kwargs, texts=texts)
            return [[[0] * 4] * kwargs["wave"] for _ in texts]
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(G.vk_fuzz_run, "run_programs", side_effect=fake):
            G.gpu_batch([(0, 2, 0, BODY, [], {"if"})], 64, "build", tmp)
        self.assertEqual((seen["words"], seen["wave"], seen["per_lane"], seen["encoded"]), (4, 64, True, True))
        self.assertEqual(seen["build"], "build")
        self.assertEqual(seen["texts"], [G.compute_listing(BODY, 64, "cs_vkfuzz_0")])


class Control(unittest.TestCase):
    def test_a_gpu_that_agrees_with_the_model_passes_and_the_flipped_branch_is_caught(self):
        with tempfile.TemporaryDirectory() as tmp:
            good = G.reference(BODY, 32, "ps_x", tmp)
            with mock.patch.object(G, "gpu_batch", return_value=[good]):
                result = G.control("build", tmp)
        self.assertTrue(result["ok"])
        self.assertTrue(result["correct_reference_matches"] and result["wrong_branch_caught"])
        self.assertEqual(result["first_wrong_branch_difference"]["register"], "v11")

    def test_a_gpu_that_follows_the_wrong_model_fails_the_control(self):
        with tempfile.TemporaryDirectory() as tmp:
            bad = G.reference(WRONG, 32, "ps_x", tmp)
            with mock.patch.object(G, "gpu_batch", return_value=[bad]):
                result = G.control("build", tmp)
        self.assertFalse(result["ok"])
        self.assertFalse(result["correct_reference_matches"])

    def test_no_gpu_result_fails_the_control_with_the_reason(self):
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(G, "gpu_batch", return_value=["no device"]):
            self.assertEqual(G.control("build", tmp), {"ok": False, "error": "no device"})


class Campaign(unittest.TestCase):
    ARGS = ["--programs", "6", "--seeds", "1", "--depths", "2", "--wave", "32", "--no-minimize"]

    def run_campaign(self, gpu, control_ok=True, extra=()):
        with tempfile.TemporaryDirectory() as tmp:
            control = {"ok": control_ok, "correct_reference_matches": control_ok, "wrong_branch_caught": control_ok}
            with mock.patch.object(G, "control", return_value=control), \
                 mock.patch.object(G, "gpu_batch", side_effect=gpu):
                status, out, err = run_main(self.ARGS + ["--logs", tmp] + list(extra))
            summary_path = Path(tmp) / "summary.json"
            summary = json.loads(summary_path.read_text(encoding="utf-8")) if summary_path.exists() else None
        return status, out, err, summary

    def test_a_gpu_that_matches_the_model_exits_zero(self):
        status, out, _err, summary = self.run_campaign(lambda cases, *_a: [list(c[4]) for c in cases])
        self.assertEqual(status, 0)
        self.assertEqual((summary["mismatched"], summary["generated"]), (0, 6))
        self.assertGreater(summary["ran"], 0)
        self.assertEqual(summary["ran"] + sum(summary["skipped"].values()), 6)
        self.assertIn("mismatched=0", out)
        self.assertIn("shape", out)

    def test_one_wrong_word_exits_one_and_keeps_the_program(self):
        status, out, _err, summary = self.run_campaign(lambda cases, *_a: [flipped(c[4]) for c in cases])
        self.assertEqual(status, 1)
        self.assertEqual(summary["mismatched"], summary["ran"])
        self.assertIn("MISMATCH", out)
        first = summary["mismatches"][0]
        self.assertEqual((first["wave"], first["difference"]["lane"], first["difference"]["register"]),
                         (32, 3, "v11"))
        self.assertIn("buffer_store_dword v11", first["minimal_program"])
        self.assertIn("cpu_interpreter", first)

    def test_a_skipped_program_is_counted_and_never_a_pass(self):
        status, out, _err, summary = self.run_campaign(lambda cases, *_a: ["vk_compute_test: unsupported"] * len(cases))
        self.assertEqual(status, 2)
        self.assertEqual(summary["ran"], 0)
        self.assertEqual(summary["skipped"], {"vk_compute_test: unsupported": 6})
        self.assertIn("skipped x6: vk_compute_test: unsupported", out)

    def test_a_failed_control_stops_before_any_program_runs(self):
        calls = []
        status, _out, err, summary = self.run_campaign(lambda cases, *_a: calls.append(cases), control_ok=False)
        self.assertEqual(status, 3)
        self.assertEqual(calls, [])
        self.assertIsNone(summary)
        self.assertIn("could not see a mismatch", err)

    def test_control_only_runs_nothing_else(self):
        calls = []
        status, _out, _err, summary = self.run_campaign(lambda cases, *_a: calls.append(cases),
                                                        extra=["--control-only"])
        self.assertEqual((status, calls, summary), (0, [], None))

    def test_both_waves_are_run(self):
        waves = []

        def gpu(cases, wave, *_a):
            waves.append(wave)
            return [list(c[4]) for c in cases]
        _status, _out, _err, summary = self.run_campaign(gpu, extra=["--wave", "both"])
        self.assertEqual(set(waves), {32, 64})
        self.assertEqual(summary["generated"], 12)

    def test_bad_arguments_are_usage_errors(self):
        for argv in (["--programs", "0"], ["--depths", "0"], ["--seeds", "x"], ["--batch", "0"], ["--start", "-1"]):
            with self.subTest(argv=argv), self.assertRaises(SystemExit) as stop, \
                 contextlib.redirect_stderr(io.StringIO()):
                G.main(argv)
            self.assertEqual(stop.exception.code, 2)

    def test_summary_is_written_with_lf_line_endings(self):
        with tempfile.TemporaryDirectory() as tmp:
            G.write_summary({"b": 1, "a": {"x": 2}}, tmp)
            data = (Path(tmp) / "summary.json").read_bytes()
        self.assertNotIn(b"\r", data)
        self.assertTrue(data.endswith(b"}\n"))
        self.assertLess(data.index(b'"a"'), data.index(b'"b"'))   # sorted keys: the same bytes on every run


if __name__ == "__main__":
    unittest.main()
