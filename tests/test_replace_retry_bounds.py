"""Tests for ERROR_LOCK_VIOLATION retry and seconds validation in tools/replace_retry.py."""
import errno
import os
import sys
import tempfile
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import replace_retry  # noqa: E402


def _win_perm_error(winerror):
    err = PermissionError(errno.EACCES, f"mock transient error {winerror}")
    err.winerror = winerror
    return err


class ReplaceRetryBoundsTests(unittest.TestCase):
    def test_retries_lock_violation_winerror_33(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, "src.tmp")
            dst = os.path.join(tmp, "dst.txt")
            with open(src, "w", encoding="utf-8", newline="\n") as f:
                f.write("updated")
            with open(dst, "w", encoding="utf-8", newline="\n") as f:
                f.write("original")

            real_replace = os.replace
            calls = []

            def fake_replace(s, d):
                calls.append((s, d))
                if len(calls) <= 2:
                    raise _win_perm_error(33)
                return real_replace(s, d)

            with mock.patch.object(replace_retry.os, "replace", fake_replace), \
                    mock.patch.object(replace_retry.time, "sleep") as sleep_mock:
                replace_retry.replace(src, dst)

            self.assertEqual(len(calls), 3)
            self.assertEqual(sleep_mock.call_count, 2)
            with open(dst, encoding="utf-8") as f:
                self.assertEqual(f.read(), "updated")

    def test_rejects_invalid_seconds_before_calling_replace(self):
        invalid_values = (
            float("nan"),
            float("inf"),
            float("-inf"),
            -0.5,
            10**1000,
            -10**1000,
            True,
            False,
            "1",
        )
        for bad in invalid_values:
            with self.subTest(seconds=bad):
                calls = []

                def guarded_replace(s, d):
                    calls.append((s, d))
                    if len(calls) > 1:
                        raise AssertionError("replace loop did not terminate")
                    return None

                with mock.patch.object(replace_retry.os, "replace", guarded_replace), \
                        mock.patch.object(replace_retry.time, "sleep") as sleep_mock:
                    with self.assertRaises(ValueError):
                        replace_retry.replace("a", "b", seconds=bad)

                self.assertEqual(calls, [])
                sleep_mock.assert_not_called()

    def test_zero_seconds_attempts_once_without_sleeping(self):
        calls = []
        err = _win_perm_error(32)

        def fail_once(s, d):
            calls.append((s, d))
            if len(calls) > 1:
                raise AssertionError("seconds=0 must attempt os.replace at most once")
            raise err

        with mock.patch.object(replace_retry.os, "replace", fail_once), \
                mock.patch.object(replace_retry.time, "sleep") as sleep_mock:
            with self.assertRaises(PermissionError) as caught:
                replace_retry.replace("a", "b", seconds=0)

        self.assertIs(caught.exception, err)
        self.assertEqual(len(calls), 1)
        sleep_mock.assert_not_called()


if __name__ == "__main__":
    unittest.main()
