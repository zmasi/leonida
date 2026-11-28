"""scripts/bench_translate.py runs, reports the documented numbers, and fails a slowdown check.

    python -I tests/test_bench_translate.py

The measurement itself is only asserted for shape: a positive, finite per-instruction time and the same fields in
--json. The absolute numbers depend on the machine, which is why the script records them for a same-machine
comparison instead of testing them.
"""
import json
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT = os.path.join(ROOT, "scripts", "bench_translate.py")


def run(*args):
    return subprocess.run([sys.executable, "-I", SCRIPT, *args], capture_output=True, text=True, timeout=600)


class BenchTranslateTest(unittest.TestCase):
    def test_json_reports_every_section(self):
        result = run("--json", "--runs", "1", "--size", "40")
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report["runs"], 1)
        self.assertEqual(report["synthetic"]["size"], 40)
        # Two synthetic programs, and the fixtures that translate without an FSSM header.
        self.assertEqual([row["name"] for row in report["synthetic"]["programs"]], ["alu", "nop"])
        self.assertGreater(report["corpus"]["summary"]["programs"], 0)
        for row in report["synthetic"]["programs"] + report["corpus"]["programs"]:
            self.assertGreater(row["instructions"], 0, row["name"])
            self.assertGreater(row["seconds"], 0.0, row["name"])
            self.assertGreater(row["us_per_instruction"], 0.0, row["name"])
        for name in ("corpus_us_per_instruction", "alu_us_per_instruction", "nop_us_per_instruction"):
            self.assertIn(name, report["measured"])
            self.assertIsNotNone(report["slowdown"][name])
        # A compute fixture needs the FSSM header the semantic tests pass, so it is reported, never silently counted.
        self.assertTrue(all("FSSM" in line for line in report["corpus"]["skipped"]), report["corpus"]["skipped"])

    def test_the_limit_is_honoured_in_both_output_formats(self):
        # A job that collects --json must not read a success code for a run whose numbers are over the limit.
        for extra in ((), ("--json",)):
            with self.subTest(extra=extra):
                result = run("--runs", "1", "--size", "20", "--max-slowdown", "0.01", *extra)
                self.assertEqual(result.returncode, 1, result.stderr)
                if extra:
                    self.assertTrue(json.loads(result.stdout)["over_limit"])

    def test_compare_honours_the_limit_and_exits_one(self):
        # Reported on the PR: --compare returned 0 whatever --max-slowdown said, while the help promised the limit in
        # every output format. Two identical trees give ratios at 1.00, so a 0.01 limit has to fail.
        tools = os.path.join(ROOT, "tools")
        result = run("--compare", tools, tools, "--rounds", "1", "--runs", "1", "--size", "20",
                     "--max-slowdown", "0.01")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("slower than the reference", result.stderr)

    def test_a_partial_baseline_does_not_abort_the_text_output(self):
        # Reported on the PR: a baseline holding only the corpus figure raised KeyError in the per-program lines.
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "partial.json")
            with open(path, "w", encoding="utf-8", newline="\n") as handle:
                json.dump({"corpus_us_per_instruction": 1.0}, handle)
            result = run("--runs", "1", "--size", "20", "--baseline", path)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("no baseline for this measurement", result.stdout)

    def test_flags_that_do_not_apply_to_compare_are_refused(self):
        # Reported on the PR: --compare ignored --json, --write-baseline and --baseline, so a run looked like it had
        # done something it had not.
        tools = os.path.join(ROOT, "tools")
        for extra in (("--json",), ("--write-baseline", "out.json"), ("--baseline", "ref.json")):
            with self.subTest(extra=extra):
                result = run("--compare", tools, tools, "--rounds", "1", "--runs", "1", "--size", "20", *extra)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn("does not apply to --compare", result.stderr)

    def test_a_baseline_and_a_write_baseline_may_not_name_one_file(self):
        # One file for both: a run that fails the limit would replace the reference it failed against, and the same
        # workload would pass on the retry.
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "same.json")
            result = run("--runs", "1", "--size", "20", "--baseline", path, "--write-baseline", path)
            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertIn("name the same file", result.stderr)

    def test_a_baseline_that_does_not_exist_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            missing = os.path.join(tmp, "nope.json")
            result = run("--runs", "1", "--size", "20", "--baseline", missing)
            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertIn("does not exist", result.stderr)

    def test_a_local_baseline_replaces_the_committed_one(self):
        # The committed numbers come from one machine; on another they would fail a run with no regression in it.
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "baseline.json")
            written = run("--runs", "1", "--size", "20", "--write-baseline", path)
            self.assertEqual(written.returncode, 0, written.stderr)
            self.assertTrue(os.path.isfile(path))
            result = run("--runs", "1", "--size", "20", "--baseline", path, "--max-slowdown", "5", "--json")
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads(result.stdout)
            self.assertEqual(report["baseline_source"], os.path.abspath(path))
            self.assertEqual(sorted(report["baseline"]), sorted(report["measured"]))
            self.assertEqual(report["over_limit"], [])

    def test_compare_runs_two_trees_and_prints_a_ratio(self):
        tools = os.path.join(ROOT, "tools")
        result = run("--compare", tools, tools, "--rounds", "1", "--runs", "1", "--size", "20")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("ratio", result.stdout)
        self.assertIn(tools, result.stdout)
        self.assertIn("us_per_instruction", result.stdout)

    def test_bad_arguments_are_rejected(self):
        for args in (("--runs", "0"), ("--size", "0")):
            result = run(*args)
            self.assertEqual(result.returncode, 2, result.stderr)


if __name__ == "__main__":
    unittest.main()
