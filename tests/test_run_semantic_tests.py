"""scripts/run_semantic_tests.py on a throwaway suite: it finds what unittest discover finds, and a failing, crashing
or unimportable file always makes it exit 1 with that file named, in parallel and with -j 1.

    python -I tests/test_run_semantic_tests.py
"""
import contextlib
import io
import os
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import _thread

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))

import run_semantic_tests as r  # noqa: E402

PASS = "import unittest\nimport helper\nclass T(unittest.TestCase):\n    def test_a(self): self.assertEqual(helper.X, 1)\n" \
       "    def test_b(self): pass\n"
FAIL = "import unittest\nclass T(unittest.TestCase):\n    def test_a(self): self.assertEqual(1, 2, 'boom-msg')\n"
BROKEN = "import no_such_module_anywhere\n"
HANG = "import time\nprint('before-sleep-marker', flush=True)\ntime.sleep(60)\n"
RAN0 = ("import unittest\nclass T(unittest.TestCase):\n    def test_a(self):\n"
        "        print('Ran 0 tests in 0.000s', flush=True)\n")
RAN0_AT_EXIT = ("import atexit\nimport unittest\natexit.register(lambda: print('Ran 0 tests in 0.000s'))\n"
                "class T(unittest.TestCase):\n    def test_a(self):\n        pass\n")
# An atexit handler that writes to stderr after unittest's own summary: the count comes from unittest's result
# object, so the late line cannot change it.
ATEXIT_STDERR = ("import atexit\nimport sys\nimport unittest\n"
                 "atexit.register(lambda: unittest.TextTestRunner(stream=sys.stderr, verbosity=0)"
                 ".run(unittest.TestSuite()))\n"
                 "class T(unittest.TestCase):\n    def test_a(self):\n        pass\n")
LATE_SUMMARY = ("import atexit\nimport sys\nimport unittest\n"
                "atexit.register(lambda: print('Ran 7 tests in 0.000s', file=sys.stderr))\n"
                "class T(unittest.TestCase):\n    def test_a(self):\n        pass\n")
# A test that runs a nested unittest runner: that runner writes its own summary to stderr while the test runs.
NESTED = ("import sys\nimport unittest\nclass T(unittest.TestCase):\n    def test_a(self):\n"
          "        unittest.TextTestRunner(stream=sys.stderr, verbosity=0).run(unittest.TestSuite())\n")
# Dies on import the way a crashed interpreter does, printing nothing: a real SIGSEGV on POSIX, and on Windows the
# exit code python.exe gets from an access violation (0xC0000005), since Windows has no signal to raise for it.
CRASH = ("import os, signal, sys\n"
         "if sys.platform == 'win32':\n"
         "    os._exit(0xC0000005 - (1 << 32))\n"
         "os.kill(os.getpid(), signal.SIGSEGV)\n")


def suite(tmp, files):
    for rel, text in files.items():
        path = os.path.join(tmp, *rel.split("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)
    return tmp


def run(*argv):
    out = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
        code = r.main(list(argv))
    return code, out.getvalue()


def cli(*argv):
    return subprocess.run([sys.executable, "-I", os.path.join(ROOT, "scripts", "run_semantic_tests.py"), *argv],
                          capture_output=True, text=True, timeout=15)


class RunnerTest(unittest.TestCase):
    def test_parallel_timeout_reports_partial_output_and_keeps_healthy_modules(self):
        with tempfile.TemporaryDirectory() as tmp:
            suite(tmp, {"helper.py": "X = 1\n", "test_hang.py": HANG,
                        "test_pass_one.py": PASS, "test_pass_two.py": PASS})
            result = cli(tmp, "-j", "2", "--timeout", "1")
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("before-sleep-marker", result.stdout)
            self.assertIn("==== FAILED: test_hang", result.stdout)
            self.assertIn("TIMED OUT after 1 s: test_hang", result.stdout)
            self.assertIn("3 files, 4 tests, 1 files failed", result.stdout)
            self.assertNotIn("Traceback", result.stdout + result.stderr)

    def test_the_count_comes_from_unittest_not_from_a_test_line(self):
        # A test may print a line of the same shape as unittest's own summary. unittest writes its summary to stderr
        # and a test's own output to stdout, so the count comes from stderr: in the merged output it was whichever
        # line came first, which depended on stdout buffering.
        for source in (RAN0, RAN0_AT_EXIT):
            with self.subTest(source=source.split(chr(10))[0]), tempfile.TemporaryDirectory() as tmp:
                suite(tmp, {"test_counts.py": source})
                result = cli(tmp, "-j", "2")
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("1 files, 1 tests, 0 files failed", result.stdout)
                self.assertNotIn("no tests ran", result.stdout)

    def test_the_count_comes_from_unittest_not_from_a_nested_runner(self):
        # A test that runs a nested unittest runner writes that runner's summary ("Ran 0 tests in ...") to stderr
        # while the test runs, before unittest writes its own. The count must come from unittest's own summary.
        with tempfile.TemporaryDirectory() as tmp:
            suite(tmp, {"test_counts.py": NESTED})
            result = cli(tmp, "-j", "2")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("1 files, 1 tests, 0 files failed", result.stdout)
            self.assertNotIn("no tests ran", result.stdout)

    def test_the_count_comes_from_the_result_object_not_from_a_late_summary(self):
        # A summary written to stderr after unittest's own - an atexit handler that runs a nested runner, or one
        # that prints a line of that shape - must not change the count: it comes from unittest's result object.
        for source in (ATEXIT_STDERR, LATE_SUMMARY):
            with self.subTest(source=source.split(chr(10))[0]), tempfile.TemporaryDirectory() as tmp:
                suite(tmp, {"test_counts.py": source})
                result = cli(tmp, "-j", "2")
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("1 files, 1 tests, 0 files failed", result.stdout)
                self.assertNotIn("no tests ran", result.stdout)

    def test_a_successful_exit_without_a_unittest_result_fails_the_run(self):
        # A module aborts the child before unittest can run the suite or report a result.
        early_exit = "import os\nprint('before-exit', flush=True)\nos._exit(0)\n"
        for jobs in (1, 2):
            with self.subTest(jobs=jobs), tempfile.TemporaryDirectory() as tmp:
                suite(tmp, {"helper.py": "X = 1\n", "test_abort.py": early_exit, "test_ok.py": PASS})
                result = cli(tmp, "-j", str(jobs))
                out = result.stdout + result.stderr
                self.assertEqual(result.returncode, 1, out)
                self.assertIn("before-exit", out)
                self.assertIn("exited without a unittest result", out)
                if jobs == 2:
                    self.assertIn("==== FAILED: test_abort", out)
                    self.assertIn("2 files, 2 tests, 1 files failed", out)

    def test_parallel_worker_exit_mid_test_fails_even_when_another_module_passes(self):
        abort_mid_test = (
            "import os, unittest\n"
            "class AbruptExit(unittest.TestCase):\n"
            "    def test_exits_after_starting(self):\n"
            "        print('mid-test-exit', flush=True)\n"
            "        os._exit(0)\n"
        )
        with tempfile.TemporaryDirectory() as tmp:
            suite(tmp, {"helper.py": "X = 1\n", "test_abort.py": abort_mid_test, "test_ok.py": PASS})
            result = cli(tmp, "-j", "2")
            out = result.stdout + result.stderr
            self.assertEqual(result.returncode, 1, out)
            self.assertIn("mid-test-exit", out)
            self.assertIn("==== FAILED: test_abort", out)
            self.assertIn("exited without a unittest result", out)
            self.assertIn("2 files, 2 tests, 1 files failed", out)

    def test_successful_process_exit_does_not_hide_a_failed_unittest_result(self):
        # This module fails a test; its teardown of the process must not erase the result.
        late_exit = "import atexit, os\natexit.register(lambda: os._exit(0))\n" + FAIL
        for jobs in (1, 2):
            with self.subTest(jobs=jobs), tempfile.TemporaryDirectory() as tmp:
                suite(tmp, {"helper.py": "X = 1\n", "test_ok.py": PASS, "test_wrong.py": late_exit})
                result = cli(tmp, "-j", str(jobs))
                out = result.stdout + result.stderr
                self.assertEqual(result.returncode, 1, out)
                self.assertIn("boom-msg", out)
                self.assertIn("exited with status 0 but unittest reported 1", out)

    def test_serial_timeout_keeps_discover_output_and_names_the_suite(self):
        with tempfile.TemporaryDirectory() as tmp:
            suite(tmp, {"test_hang.py": HANG})
            result = cli(tmp, "-j", "1", "--timeout", "1")
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("before-sleep-marker", result.stdout)
            self.assertIn("TIMED OUT after 1 s: unittest discover in", result.stderr)
            self.assertIn(tmp, result.stderr)
            self.assertNotIn("Traceback", result.stdout + result.stderr)

    def test_timeout_requires_positive_finite_seconds(self):
        for value in ("0", "-1", "nan", "inf", "-inf"):
            with self.subTest(value=value):
                result = cli("unused-suite", "--timeout=" + value)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertIn("positive finite", result.stderr)
                self.assertNotIn("Traceback", result.stderr)

    def test_out_of_range_timeout_is_rejected_before_running_the_suite(self):
        for jobs in (1, 2):
            for value in ("86400.1", "1e100"):
                with self.subTest(jobs=jobs, value=value):
                    result = cli("unused-suite", "-j", str(jobs), "--timeout=" + value)
                    self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                    self.assertIn("no greater than 86400", result.stderr)
                    self.assertNotIn("Traceback", result.stderr)
                    self.assertNotIn("TIMED OUT", result.stdout + result.stderr)
                    self.assertNotIn("no test*.py files", result.stdout)

    def test_maximum_timeout_runs_a_passing_suite_in_both_modes(self):
        for jobs in (1, 2):
            with self.subTest(jobs=jobs), tempfile.TemporaryDirectory() as tmp:
                suite(tmp, {"helper.py": "X = 1\n", "test_pass.py": PASS})
                result = cli(tmp, "-j", str(jobs), "--timeout", "86400")
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_custom_patterns_find_only_python_modules_in_packages(self):
        with tempfile.TemporaryDirectory() as tmp:
            suite(tmp, {"case_one.py": "", "case_two.txt": "", "case_bad-name.py": "",
                        "pkg/__init__.py": "", "pkg/case_two.py": "", "plain/case_three.py": ""})
            self.assertEqual(r.test_modules(tmp, "case_*.py"), ["case_one", "pkg.__init__", "pkg.case_two"])
            self.assertEqual(r.test_modules(tmp, "*"), ["case_one", "pkg.__init__", "pkg.case_two"])

    def test_pattern_selects_files_without_importing_excluded_modules(self):
        for jobs in (1, 2):
            for flag in ("-p", "--pattern"):
                with self.subTest(jobs=jobs, flag=flag), tempfile.TemporaryDirectory() as tmp:
                    suite(tmp, {"helper.py": "X = 1\n", "case_one.py": PASS,
                                "pkg/__init__.py": "", "pkg/case_two.py": PASS,
                                "test_excluded.py": "raise AssertionError('excluded module imported')\n",
                                "plain/case_three.py": BROKEN})
                    result = cli(tmp, "-j", str(jobs), flag, "case_*.py")
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertNotIn("excluded module imported", result.stdout + result.stderr)
                    if jobs == 1:
                        self.assertIn("Ran 4 tests", result.stderr)
                    else:
                        self.assertIn("3 files, 4 tests, 0 files failed", result.stdout)

    def test_pattern_no_matches_is_an_error_in_both_modes(self):
        for jobs in (1, 2):
            with self.subTest(jobs=jobs), tempfile.TemporaryDirectory() as tmp:
                suite(tmp, {"test_excluded.py": BROKEN})
                result = cli(tmp, "-j", str(jobs), "--pattern", "case_*.py")
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn("no case_*.py files", result.stdout)
                self.assertNotIn("Traceback", result.stderr)

    def test_matching_failure_is_not_hidden_by_pattern_selection(self):
        for jobs in (1, 2):
            with self.subTest(jobs=jobs), tempfile.TemporaryDirectory() as tmp:
                suite(tmp, {"case_fail.py": FAIL, "test_excluded.py": BROKEN})
                result = cli(tmp, "-j", str(jobs), "--pattern", "case_*.py")
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn("boom-msg", result.stdout + result.stderr)
                self.assertNotIn("no_such_module_anywhere", result.stdout + result.stderr)

    def test_omitting_pattern_keeps_the_default_selection(self):
        for jobs in (1, 2):
            with self.subTest(jobs=jobs), tempfile.TemporaryDirectory() as tmp:
                suite(tmp, {"helper.py": "X = 1\n", "test_pass.py": PASS,
                            "case_excluded.py": "raise AssertionError('custom module imported')\n"})
                result = cli(tmp, "-j", str(jobs))
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertNotIn("custom module imported", result.stdout + result.stderr)

    def test_finds_what_discover_finds(self):
        with tempfile.TemporaryDirectory() as tmp:
            suite(tmp, {"test_one.py": "", "helper.py": "", "pkg/__init__.py": "", "pkg/test_two.py": "",
                        "plain_dir/test_three.py": "", "testx.py": ""})
            # discover enters packages only, and its pattern is test*.py
            self.assertEqual(r.test_modules(tmp), ["test_one", "testx", "pkg.__init__", "pkg.test_two"])

    def test_package_initializer_failure_is_seen_in_both_modes(self):
        init_fail = "import unittest\nclass T(unittest.TestCase):\n" \
                    "    def test_fail(self): self.assertEqual(1, 2, 'init-boom')\n"
        for jobs in (1, 2):
            with self.subTest(jobs=jobs), tempfile.TemporaryDirectory() as tmp:
                suite(tmp, {"helper.py": "X = 1\n", "pkg/__init__.py": init_fail, "pkg/test_child.py": PASS})
                result = cli(tmp, "-j", str(jobs))
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn("init-boom", result.stdout + result.stderr)
                if jobs == 1:
                    self.assertIn("Ran 3 tests", result.stderr)
                else:
                    self.assertIn("==== FAILED: pkg.__init__", result.stdout)
                    self.assertIn("2 files, 3 tests, 1 files failed", result.stdout)

    def test_package_initializer_runs_when_no_filename_matches_pattern(self):
        init_fail = "import unittest\nclass T(unittest.TestCase):\n" \
                    "    def test_fail(self): self.assertEqual(1, 2, 'init-boom')\n"
        for jobs in (1, 2):
            with self.subTest(jobs=jobs), tempfile.TemporaryDirectory() as tmp:
                suite(tmp, {"helper.py": "X = 1\n", "pkg/__init__.py": init_fail, "pkg/test_child.py": PASS})
                result = cli(tmp, "-j", str(jobs), "-p", "zzz*.py")
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn("init-boom", result.stdout + result.stderr)

    def test_package_initializer_runs_once_in_parallel(self):
        # `python -m unittest pkg.__init__` imports pkg and then runs the same file again as pkg.__init__, so this
        # registration happens twice; the runner hands unittest the package name instead.
        init = ("import unittest\n"
                "registrations = []\n"
                "registrations.append(__name__)\n"
                "class T(unittest.TestCase):\n"
                "    def test_once(self):\n"
                "        self.assertEqual(registrations, ['pkg'])\n")
        for jobs in (1, 2):
            with self.subTest(jobs=jobs), tempfile.TemporaryDirectory() as tmp:
                suite(tmp, {"pkg/__init__.py": init})
                result = cli(tmp, "-j", str(jobs))
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_empty_package_alone_is_not_a_false_green(self):
        # -j 1 forwards to discover, which exits 5 when nothing ran; a parallel run whose only job is an empty
        # initializer must not report success either.
        for jobs in (1, 2):
            with self.subTest(jobs=jobs), tempfile.TemporaryDirectory() as tmp:
                suite(tmp, {"pkg/__init__.py": ""})
                result = cli(tmp, "-j", str(jobs))
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                if jobs == 2:
                    self.assertIn("no tests ran", result.stdout)

    def test_all_pass(self):
        with tempfile.TemporaryDirectory() as tmp:
            suite(tmp, {"helper.py": "X = 1\n", "test_p1.py": PASS, "test_p2.py": PASS})
            code, out = run(tmp, "-j", "2")
            self.assertEqual(code, 0, out)
            self.assertIn("2 files, 4 tests, 0 files failed", out)

    def test_a_failure_or_import_error_is_reported_and_fails_the_run(self):
        with tempfile.TemporaryDirectory() as tmp:
            suite(tmp, {"helper.py": "X = 1\n", "test_p.py": PASS, "test_f.py": FAIL, "test_b.py": BROKEN})
            code, out = run(tmp, "-j", "3")
            self.assertEqual(code, 1, out)
            self.assertIn("==== FAILED: test_f", out)
            self.assertIn("boom-msg", out)                         # the failing file's own output is shown
            self.assertIn("==== FAILED: test_b", out)
            self.assertIn("2 files failed", out)
            self.assertTrue(out.rstrip().endswith("test_b, test_f"), out)

    def test_a_failure_header_names_the_exit_code(self):
        with tempfile.TemporaryDirectory() as tmp:
            suite(tmp, {"test_f.py": FAIL})
            code, out = run(tmp, "-j", "2")
            self.assertEqual(code, 1, out)
            self.assertIn("==== FAILED: test_f (exit code 1)\n", out)
            self.assertNotIn("no output", out)                      # it printed its traceback

    def test_a_crash_that_prints_nothing_is_named(self):
        # The case this is for: a crashed file used to leave a bare "==== FAILED: name" with nothing under it.
        with tempfile.TemporaryDirectory() as tmp:
            suite(tmp, {"helper.py": "X = 1\n", "test_p.py": PASS, "test_crash.py": CRASH})
            code, out = run(tmp, "-j", "2")
            self.assertEqual(code, 1, out)
            want = "0xC0000005, access violation" if sys.platform == "win32" else "killed by SIGSEGV (-11)"
            self.assertIn("==== FAILED: test_crash (", out)
            self.assertIn(want, out)
            self.assertIn("(no output: the process ended before printing anything)", out)
            self.assertIn("2 files, 2 tests, 1 files failed", out)

    def test_describe_exit(self):
        self.assertEqual(r.describe_exit(1), "exit code 1")
        self.assertEqual(r.describe_exit(-11), "killed by SIGSEGV (-11)")
        self.assertEqual(r.describe_exit(-1000), "killed by signal 1000 (-1000)")
        self.assertEqual(r.describe_exit(3221225477), "exit code 0xC0000005, access violation (the process crashed)")
        self.assertEqual(r.describe_exit(0xC00000FD), "exit code 0xC00000FD, stack overflow (the process crashed)")
        # Ctrl+C is a known status but not a crash.
        self.assertEqual(r.describe_exit(0xC000013A), "exit code 0xC000013A, interrupted with Ctrl+C")
        # A process can exit with any 32-bit value on purpose: an unknown one is only its value, not called a crash.
        self.assertEqual(r.describe_exit(0xC0001234), "exit code 0xC0001234")
        self.assertEqual(r.describe_exit(0xDEADBEEF), "exit code 0xDEADBEEF")
        self.assertEqual(r.describe_exit(255), "exit code 255")

    def test_one_job_runs_discover_and_keeps_its_exit_code(self):
        with tempfile.TemporaryDirectory() as tmp:
            suite(tmp, {"helper.py": "X = 1\n", "test_p.py": PASS, "test_f.py": FAIL})
            self.assertEqual(run(tmp, "-j", "1")[0], 1)
        with tempfile.TemporaryDirectory() as tmp:
            suite(tmp, {"helper.py": "X = 1\n", "test_p.py": PASS})
            self.assertEqual(run(tmp, "-j", "1")[0], 0)

    def test_an_empty_suite_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            code, out = run(tmp, "-j", "2")
            self.assertEqual(code, 1)
            self.assertIn("no test*.py files", out)

    def test_parallel_interrupt_stops_active_processes_and_cancels_queued_jobs(self):
        with tempfile.TemporaryDirectory() as tmp, tempfile.TemporaryDirectory() as marker_dir:
            mod_template = (
                "import os, time, unittest\n"
                "class T(unittest.TestCase):\n"
                "    def test_run(self):\n"
                "        with open(r'{marker}', 'w') as f: f.write('1')\n"
                "        time.sleep(10)\n"
            )
            files = {
                f"test_{i}.py": mod_template.format(marker=os.path.join(marker_dir, f"m_{i}.txt"))
                for i in range(4)
            }
            suite(tmp, files)

            def trigger():
                start = time.monotonic()
                while time.monotonic() - start < 8:
                    if len(os.listdir(marker_dir)) >= 2:
                        break
                    time.sleep(0.05)
                time.sleep(0.05)
                _thread.interrupt_main()

            threading.Thread(target=trigger, daemon=True).start()
            out, err = io.StringIO(), io.StringIO()
            t0 = time.monotonic()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                code = r.main([tmp, "-j", "2"])
            elapsed = time.monotonic() - t0

            self.assertEqual(code, 130)
            self.assertIn("interrupted", err.getvalue())
            self.assertNotIn("files failed", out.getvalue())
            self.assertNotIn("4 files,", out.getvalue())
            started = os.listdir(marker_dir)
            self.assertEqual(len(started), 2, f"Expected 2 started modules, got {started}")
            self.assertLess(elapsed, 5.0)

    def test_serial_interrupt_returns_130_and_prints_interrupted(self):
        with tempfile.TemporaryDirectory() as tmp, tempfile.TemporaryDirectory() as marker_dir:
            marker = os.path.join(marker_dir, "serial_marker.txt")
            content = (
                "import os, time, unittest\n"
                "class T(unittest.TestCase):\n"
                f"    def test_run(self):\n"
                f"        with open(r'{marker}', 'w') as f: f.write('1')\n"
                "        time.sleep(10)\n"
            )
            suite(tmp, {"test_serial_hang.py": content})

            def trigger():
                start = time.monotonic()
                while time.monotonic() - start < 8:
                    if os.path.exists(marker):
                        break
                    time.sleep(0.05)
                time.sleep(0.05)
                _thread.interrupt_main()

            threading.Thread(target=trigger, daemon=True).start()
            out, err = io.StringIO(), io.StringIO()
            t0 = time.monotonic()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                code = r.main([tmp, "-j", "1"])
            elapsed = time.monotonic() - t0

            self.assertEqual(code, 130)
            self.assertIn("interrupted", err.getvalue())
            self.assertLess(elapsed, 5.0)

    def test_terminate_active_processes_kills_registered_processes(self):
        cmd = [sys.executable, "-c", "import time; time.sleep(10)"]
        p1 = subprocess.Popen(cmd)
        p2 = subprocess.Popen(cmd)
        r._register_process(p1)
        r._register_process(p2)
        try:
            self.assertIsNone(p1.poll())
            self.assertIsNone(p2.poll())
            r._terminate_active_processes()
            self.assertIsNotNone(p1.poll())
            self.assertIsNotNone(p2.poll())
        finally:
            for p in (p1, p2):
                try:
                    p.kill()
                except OSError:
                    pass
                r._unregister_process(p)

    @unittest.skipUnless(os.name == "nt", "requires Windows taskkill")
    def test_windows_cancellation_stops_descendants(self):
        marker = "descendant completed"
        child = f"import time; time.sleep(3); print({marker!r}, flush=True)"
        parent = (
            "import subprocess, sys, time\n"
            f"subprocess.Popen([sys.executable, '-c', {child!r}])\n"
            "print('ready', flush=True)\n"
            "time.sleep(5)\n"
        )
        cmd = [sys.executable, "-c", parent]
        with subprocess.Popen(cmd, stdout=subprocess.PIPE, text=True) as proc:
            self.assertEqual(proc.stdout.readline().strip(), "ready")
            r._register_process(proc)
            try:
                r._terminate_active_processes()
                output, _ = proc.communicate(timeout=5)
            finally:
                r._unregister_process(proc)
            self.assertNotIn(marker, output)

    @unittest.skipUnless(os.name == "posix", "POSIX process-group SIGINT test")
    def test_posix_sigint_cancels_queued_modules_and_stops_runner(self):
        import signal
        with tempfile.TemporaryDirectory() as tmp, tempfile.TemporaryDirectory() as marker_dir:
            mod_template = (
                "import os, time, unittest\n"
                "class T(unittest.TestCase):\n"
                "    def test_run(self):\n"
                "        with open(r'{marker}', 'w') as f: f.write('1')\n"
                "        time.sleep(8)\n"
            )
            files = {
                f"test_{i}.py": mod_template.format(marker=os.path.join(marker_dir, f"m_{i}.txt"))
                for i in range(5)
            }
            suite(tmp, files)

            script = os.path.join(ROOT, "scripts", "run_semantic_tests.py")
            proc = subprocess.Popen([sys.executable, "-I", script, tmp, "-j", "2"],
                                    start_new_session=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    text=True)
            start = time.monotonic()
            while time.monotonic() - start < 5:
                if len(os.listdir(marker_dir)) >= 2:
                    break
                time.sleep(0.05)
            time.sleep(0.05)
            os.killpg(proc.pid, signal.SIGINT)
            stdout, stderr = proc.communicate(timeout=5)
            self.assertIn(proc.returncode, (130, -signal.SIGINT, 128 + signal.SIGINT))
            self.assertIn("interrupted", stderr)
            self.assertNotIn("5 files,", stdout)
            self.assertEqual(len(os.listdir(marker_dir)), 2)


if __name__ == "__main__":
    unittest.main()
