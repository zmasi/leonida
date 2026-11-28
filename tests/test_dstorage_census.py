"""Unit tests for tools/dstorage_census.py: a wrong input is one ERROR line and exit 2, not a traceback.

The traces are built by hand, so the test needs no game files and no third-party packages.

    python -I tests/test_dstorage_census.py
"""
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS_DIR = os.path.join(ROOT, "tools")
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

import dstorage_census as dc  # noqa: E402

TOOL = os.path.join(TOOLS_DIR, "dstorage_census.py")
REQUEST = ("[rt] ds-request file=a.bin offset=0 dest=pages options=0 zlib=0 sw=0 src=16 dst=32 "
           "hr=0 cancelled=0 tick_ms=5")


def run_tool(*args):
    return subprocess.run([sys.executable, "-I", TOOL, *args], capture_output=True, text=True,
                          encoding="utf-8", errors="replace")


class CensusTest(unittest.TestCase):
    def test_one_good_request_is_counted(self):
        report = dc.census([REQUEST])
        self.assertEqual(report["totals"]["requests"], 1)
        self.assertEqual(report["totals"]["successful"], 1)
        self.assertEqual(report["totals"]["destination_bytes"], 32)

    def test_missing_field_names_the_line_and_the_field(self):
        with self.assertRaises(dc.TraceError) as caught:
            dc.census([REQUEST, "[rt] ds-request file=a.bin offset=0 options=0"])
        self.assertIn("line 2", str(caught.exception))
        self.assertIn("dest=", str(caught.exception))

    def test_malformed_number_names_the_line(self):
        with self.assertRaises(dc.TraceError) as caught:
            dc.census([REQUEST.replace("options=0", "options=zz")])
        self.assertIn("line 1", str(caught.exception))
        self.assertIn("malformed number", str(caught.exception))


class WrongInputTest(unittest.TestCase):
    def assert_one_error_line(self, result):
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertNotIn("Traceback", result.stderr)
        lines = result.stderr.strip().splitlines()
        self.assertEqual(len(lines), 1, result.stderr)
        self.assertTrue(lines[0].startswith("ERROR: "), result.stderr)
        self.assertEqual(result.stdout, "")

    def test_missing_trace(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assert_one_error_line(run_tool(os.path.join(tmp, "no_such_trace.log")))

    def test_folder_for_the_trace(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assert_one_error_line(run_tool(tmp))

    def test_truncated_request_line(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "trace.log")
            with open(path, "w", encoding="utf-8", newline="\n") as stream:
                stream.write(REQUEST + "\n[rt] ds-request file=a.bin offset=0\n")
            result = run_tool(path)
            self.assert_one_error_line(result)
            self.assertIn("line 2", result.stderr)

    def test_a_good_trace_still_prints_the_report_and_exits_zero(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "trace.log")
            with open(path, "w", encoding="utf-8", newline="\n") as stream:
                stream.write(REQUEST + "\n")
            result = run_tool(path)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('"requests": 1', result.stdout)


if __name__ == "__main__":
    unittest.main()
