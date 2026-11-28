"""CPU-only tests of the CI standalone-file runner."""
import contextlib
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
RUNNER = ROOT / "scripts" / "run_standalone_tests.py"
sys.path.insert(0, str(RUNNER.parent))
import run_standalone_tests as runner  # noqa: E402


class StandaloneRunnerTest(unittest.TestCase):
    def run_cli(self, cwd, paths, jobs):
        return subprocess.run([sys.executable, "-I", str(RUNNER), "-j", str(jobs)],
                              cwd=cwd, input="\n".join(paths) + "\n", text=True,
                              capture_output=True, timeout=30)

    def test_subprocesses_are_isolated_and_all_files_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            paths = ["test a.py", "test_b.py", "test_c.py"]
            for i, path in enumerate(paths):
                (root / path).write_text(
                    "import os, sys\n"
                    "assert sys.flags.isolated\n"
                    "assert os.path.isfile('test a.py')\n"
                    "assert sys.stdin.read() == ''\n"
                    f"print('stdout-{i}', flush=True)\n"
                    f"print('stderr-{i}', file=sys.stderr, flush=True)\n"
                    f"sys.exit({int(i != 1)})\n",
                    encoding="utf-8", newline="\n")
            for jobs in (1, 2):
                with self.subTest(jobs=jobs):
                    result = self.run_cli(tmp, paths, jobs)
                    self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                    for i, path in enumerate(paths):
                        self.assertIn(f"== {path}\nstdout-{i}\nstderr-{i}\n", result.stdout)
                    annotations = [line for line in result.stdout.splitlines() if line.startswith("::error")]
                    self.assertEqual(annotations, [f"::error file={p}::{p} failed" for p in (paths[0], paths[2])])
                    self.assertIn("3 files, 2 files failed", result.stdout)

            result = self.run_cli(tmp, [paths[1]], 2)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("1 files, 0 files failed", result.stdout)

    def test_redirected_annotations_preserve_utf8_paths(self):
        # Model redirected Windows stdout even when this test runs on a UTF-8 host.
        start = ("import runpy, sys; sys.stdout.reconfigure(encoding='cp1252'); "
                 "sys.argv = sys.argv[1:]; runpy.run_path(sys.argv[0], run_name='__main__')")
        name = "test_λ.py"
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / name).write_bytes(b"raise SystemExit(1)\n")
            result = subprocess.run([sys.executable, "-I", "-c", start, str(RUNNER), "-j", "2"],
                                    cwd=tmp, input=(name + "\n").encode(), capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn(f"== {name}".encode(), result.stdout.splitlines())
        self.assertIn(f"::error file={name}::{name} failed".encode(), result.stdout.splitlines())

    def test_bounds_concurrency_without_timing_assumptions(self):
        lock = threading.Lock()
        rendezvous = threading.Barrier(2)
        active = peak = 0
        seen = []

        def worker(path):
            nonlocal active, peak
            with lock:
                active += 1
                peak = max(peak, active)
                seen.append(path)
            rendezvous.wait(timeout=10)
            with lock:
                active -= 1
            return 0, ""

        paths = [f"test_{i}.py" for i in range(4)]
        with mock.patch.object(runner, "run_file", side_effect=worker), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(runner.run_tests(paths, 2), 0)
        self.assertEqual(peak, 2)
        self.assertCountEqual(seen, paths)

    def test_empty_selection_and_invalid_job_counts_fail(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = self.run_cli(tmp, [], 2)
            self.assertEqual(result.returncode, 1)
            self.assertIn("No standalone test files selected", result.stderr)
            for jobs in (0, -1, "nope"):
                with self.subTest(jobs=jobs):
                    result = self.run_cli(tmp, [], jobs)
                    self.assertEqual(result.returncode, 2)

    def test_launch_failure_does_not_hide_other_tests(self):
        with mock.patch.object(runner.subprocess, "run", side_effect=OSError("unavailable")):
            with contextlib.redirect_stdout(io.StringIO()) as out:
                self.assertEqual(runner.run_tests(["test_a.py", "test_b.py"], 2), 1)
        for path in ("test_a.py", "test_b.py"):
            self.assertIn(f"== {path}\nCould not start test: unavailable", out.getvalue())
            self.assertIn(f"::error file={path}::{path} failed", out.getvalue())


if __name__ == "__main__":
    unittest.main()
