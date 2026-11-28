"""The wave32 checkpoint report, using only synthetic cases and CPU interpreters."""

from contextlib import redirect_stderr, redirect_stdout
import io
import json
import os
from pathlib import Path
import random
import re
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import wave_trace as trace  # noqa: E402


class WaveTraceTests(unittest.TestCase):
    def test_case_uses_the_same_seed_stream_as_wave_diff(self):
        seed, index, max_ops = 17, 4, 8
        rng = random.Random(seed)
        for _ in range(index + 1):
            expected = trace.wave_diff.generate_program(rng, rng.randrange(1, max_ops + 1), 32)
        lines, start, end = trace.select_case(seed, index, max_ops)
        self.assertEqual(lines, expected)
        self.assertLess(start, end)
        self.assertEqual(trace.wave_diff.observed(lines[:end], 32), expected)

    def test_checkpoints_use_both_interpreters(self):
        seed, index, max_ops = 1, 0, 12
        report = trace.inspect_case(seed, index, max_ops)
        lines, start, end = trace.select_case(seed, index, max_ops)
        self.assertEqual([s["instruction"] for s in report["steps"]], [None, *lines[start:end]])
        self.assertIsNone(report["first_mismatch"])
        for step in report["steps"]:
            self.assertIsNone(step["unavailable"])
            self.assertEqual(step["reference"], step["translated"])
            self.assertEqual([len(row) for row in step["reference"]], [12] * 32)
        self.assertEqual(report["steps"][-1]["reference"], trace.wave_diff.run_model(lines, 32))

    def test_first_mismatch_can_disappear_before_the_full_case(self):
        run = trace.wave_diff.run_spirv
        calls = iter(range(100))

        def one_missing_word(spirv, wave):
            words = run(spirv, wave)
            if next(calls) == 1:
                words[17][2] = None
            return words

        with mock.patch.object(trace.wave_diff, "run_spirv", side_effect=one_missing_word):
            report = trace.inspect_case(1, 0, 12)
        self.assertEqual(report["first_mismatch"], 1)
        difference = report["steps"][1]["mismatch"]
        self.assertEqual((difference["lane"], difference["column"], difference["actual"]), (17, 2, None))
        self.assertEqual(difference["expected"], report["steps"][1]["reference"][17][2])
        self.assertIsNone(report["steps"][-1]["mismatch"])

    def test_unsupported_checkpoint_keeps_the_reference_and_reason(self):
        lines = trace.select_case(1, 0, 12)[0]
        with mock.patch.object(trace.wave_diff, "translate", side_effect=trace.wave_diff.rdna2spirv.Unsupported("fixture")):
            step = trace.inspect_checkpoint("cs_checkpoint", lines, None)
        self.assertEqual(step["unavailable"], "translator: fixture")
        self.assertEqual(len(step["reference"]), 32)
        self.assertIsNone(step["translated"])
        self.assertIsNone(step["mismatch"])

    def test_payload_cannot_end_the_data_script(self):
        report = {"text": "</script>"}
        html = trace.render_report(report)
        payload = re.search(r'<script id="wave-data" type="application/json">(.*?)</script>', html).group(1)
        self.assertNotIn("<", payload)
        self.assertEqual(json.loads(payload), report)

    def test_cli_writes_a_report_and_preserves_an_existing_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "wave.html"
            args = ["--max-ops", "1", "--out", str(path)]
            with redirect_stdout(io.StringIO()):
                self.assertEqual(trace.main(args), 0)
            original = path.read_bytes()
            self.assertIn(b'id="wave-data"', original)
            self.assertNotIn(b"WAVE_TRACE_DATA", original)
            with redirect_stderr(io.StringIO()):
                self.assertEqual(trace.main(args), 2)
            self.assertEqual(path.read_bytes(), original)

    def test_select_case_rejects_invalid_bounds_and_bools(self):
        for bad_case, bad_ops in [(-1, 4), (True, 4), (0, 0), (0, True)]:
            with self.subTest(case=bad_case, max_ops=bad_ops):
                with self.assertRaises(ValueError):
                    trace.select_case(1, bad_case, bad_ops)

    def test_render_report_escapes_gt_rejects_nan_and_checks_marker(self):
        report = {"text": "</script><!-- -->"}
        html = trace.render_report(report)
        payload = re.search(r'<script id="wave-data" type="application/json">(.*?)</script>', html).group(1)
        self.assertNotIn("<", payload)
        self.assertNotIn(">", payload)
        self.assertEqual(json.loads(payload), report)

        with self.assertRaises(ValueError):
            trace.render_report({"bad": float("nan")})

        with mock.patch.object(Path, "read_text", return_value="<html>no marker</html>"):
            with self.assertRaises(ValueError):
                trace.render_report(report)

    def test_cli_validates_out_before_running_inspect_case(self):
        with tempfile.TemporaryDirectory() as tmp, tempfile.TemporaryDirectory() as cwd:
            existing = Path(tmp) / "existing.html"
            existing.write_text("keep", encoding="utf-8", newline="\n")
            missing_parent = Path(tmp) / "no_such_dir" / "wave.html"
            trailing = str(Path(tmp) / "folder") + os.sep
            old_cwd = os.getcwd()
            try:
                os.chdir(cwd)
                with mock.patch.object(trace, "inspect_case", side_effect=AssertionError("should not run")):
                    for bad in ("", "   ", trailing):
                        with self.subTest(bad=bad), redirect_stderr(io.StringIO()):
                            with self.assertRaises(SystemExit) as cm:
                                trace.main(["--max-ops", "1", "--out", bad])
                            self.assertEqual(cm.exception.code, 2)
                    for target in (existing, missing_parent):
                        with self.subTest(target=str(target)), redirect_stderr(io.StringIO()):
                            self.assertEqual(trace.main(["--max-ops", "1", "--out", str(target)]), 2)
                self.assertEqual(os.listdir(cwd), [])
            finally:
                os.chdir(old_cwd)
            self.assertEqual(existing.read_text(encoding="utf-8"), "keep")

    def test_failed_write_unlinks_partial_output_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "partial.html"
            real_open = Path.open

            class FailingStream:
                def __init__(self, inner):
                    self._inner = inner

                def __enter__(self):
                    return self

                def __exit__(self, *exc):
                    self._inner.close()
                    return False

                def write(self, _text):
                    raise OSError("disk full")

                def close(self):
                    self._inner.close()

            def open_fail(self_path, mode="r", *args, **kwargs):
                inner = real_open(self_path, mode, *args, **kwargs)
                if mode == "x" and self_path == path:
                    return FailingStream(inner)
                return inner

            with mock.patch.object(trace, "inspect_case", return_value={"steps": [], "first_mismatch": None, "columns": []}), \
                    mock.patch.object(Path, "open", open_fail), \
                    redirect_stderr(io.StringIO()):
                code = trace.main(["--max-ops", "1", "--out", str(path)])
            self.assertEqual(code, 2)
            self.assertFalse(path.exists())


if __name__ == "__main__":
    unittest.main()
