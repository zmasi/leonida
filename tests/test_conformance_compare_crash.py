"""compare.py's main() turns an exception it did not anticipate into exit 2, never 1; no renderer, GPU or game files.

Exit 1 means "the run completed and found a mismatch", so a crash that leaks out of main() as an uncaught exception
(which Python reports as exit 1) would read as a pixel mismatch to a calling script.
"""
import contextlib
import importlib.util
import io
from pathlib import Path
import subprocess
import sys
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "conformance" / "compare.py"
SPEC = importlib.util.spec_from_file_location("conformance_compare_crash", SCRIPT)
C = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = C
SPEC.loader.exec_module(C)


def crash(_arguments):
    raise KeyError("simulated bug inside a handler")


class UnexpectedExceptionTest(unittest.TestCase):
    def test_handler_crash_returns_exit_error_with_traceback(self):
        err = io.StringIO()
        with mock.patch.object(C, "command_inspect", crash), contextlib.redirect_stderr(err):
            code = C.main(["inspect", "--image", "unused.png"])
        self.assertEqual(code, C.EXIT_ERROR)
        self.assertNotEqual(code, 1, "a crash must not look like a pixel mismatch")
        text = err.getvalue()
        self.assertIn("Traceback (most recent call last)", text)
        self.assertIn("simulated bug inside a handler", text)
        self.assertIn("this is a bug in compare.py, not a pixel mismatch", text)
        self.assertNotIn("NameError", text)

    def test_handler_crash_exits_2_as_a_script(self):
        # The same path through the command line: the exit status a calling script sees.
        runner = (
            "import importlib.util, sys\n"
            "sys.argv = [sys.argv[1], 'inspect', '--image', 'unused.png']\n"
            "spec = importlib.util.spec_from_file_location('compare_cli', sys.argv[0])\n"
            "m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)\n"
            "def crash(_a): raise KeyError('simulated bug inside a handler')\n"
            "m.command_inspect = crash\n"
            "sys.exit(m.main(sys.argv[1:]))\n"
        )
        result = subprocess.run([sys.executable, "-I", "-c", runner, str(SCRIPT)],
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertNotIn("NameError", result.stderr)


if __name__ == "__main__":
    unittest.main()
