"""CLI validation, global state isolation, and subprocess encoding tests for tools/cf_diff.py."""

import contextlib
import io
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

TOOLS_DIR = Path(__file__).resolve().parents[1] / "tools"
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import cf_diff  # noqa: E402


class CfDiffCliTests(unittest.TestCase):
    def setUp(self):
        self._orig_spirv_val = cf_diff.SPIRV_VAL
        cf_diff.SPIRV_VAL = False

    def tearDown(self):
        cf_diff.SPIRV_VAL = self._orig_spirv_val

    def test_rejects_minimize_without_program_and_negative_indices(self):
        cases = [
            (["--minimize", "--self-test"], "--minimize requires --program"),
            (["--program", "-1", "--self-test"], "--program must be nonnegative"),
            (["--depth", "-1", "--self-test"], "--depth must be nonnegative"),
        ]
        for argv, msg in cases:
            with self.subTest(argv=argv):
                err = io.StringIO()
                with contextlib.redirect_stderr(err):
                    with self.assertRaises(SystemExit) as cm:
                        cf_diff.main(argv)
                self.assertEqual(cm.exception.code, 2)
                self.assertIn(msg, err.getvalue())

    def test_missing_spirv_val_tools_prints_to_stderr_and_keeps_state(self):
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(cf_diff.shutil, "which", return_value=None):
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                code = cf_diff.main(["--spirv-val", "--self-test"])
        self.assertEqual(code, 2)
        self.assertEqual(out.getvalue(), "")
        self.assertIn("--spirv-val needs spirv-as and spirv-val on PATH", err.getvalue())
        self.assertFalse(cf_diff.SPIRV_VAL)

    def test_spirv_val_restores_global_on_normal_return_and_exception(self):
        observed = []

        def fake_self_test():
            observed.append(cf_diff.SPIRV_VAL)
            return True

        with mock.patch.object(cf_diff.shutil, "which", return_value="/usr/bin/tool"):
            with mock.patch.object(cf_diff, "self_test", side_effect=fake_self_test):
                code = cf_diff.main(["--spirv-val", "--self-test"])
        self.assertEqual(code, 0)
        self.assertEqual(observed, [True])
        self.assertFalse(cf_diff.SPIRV_VAL)

        observed.clear()

        def failing_self_test():
            observed.append(cf_diff.SPIRV_VAL)
            raise RuntimeError("boom")

        with mock.patch.object(cf_diff.shutil, "which", return_value="/usr/bin/tool"):
            with mock.patch.object(cf_diff, "self_test", side_effect=failing_self_test):
                with self.assertRaises(RuntimeError):
                    cf_diff.main(["--spirv-val", "--self-test"])
        self.assertEqual(observed, [True])
        self.assertFalse(cf_diff.SPIRV_VAL)

    def test_spirv_val_subprocess_uses_utf8_replace_encoding(self):
        seen_kwargs = []

        def fake_run(cmd, **kwargs):
            seen_kwargs.append(kwargs)
            return mock.Mock(returncode=0, stdout="", stderr="")

        with tempfile.TemporaryDirectory() as tmp:
            with mock.patch.object(cf_diff.subprocess, "run", side_effect=fake_run):
                err = cf_diff._spirv_val("OpCapability Shader\n", tmp)
            self.assertIsNone(err)
            self.assertTrue(os.path.isfile(os.path.join(tmp, "m.spvasm")))
        self.assertEqual(len(seen_kwargs), 2)
        for kwargs in seen_kwargs:
            self.assertEqual(kwargs.get("encoding"), "utf-8")
            self.assertEqual(kwargs.get("errors"), "replace")
            self.assertTrue(kwargs.get("capture_output"))
            self.assertTrue(kwargs.get("text"))


if __name__ == "__main__":
    unittest.main()
