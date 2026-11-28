"""Failed replace of a read-only rehydrated BLAS leaves no staged file and keeps the real error.

Synthetic bytes only; no game data. Run: python3 -I tests/test_bvh_rehydrate_stage_cleanup.py
"""
import os
import sys
import tempfile
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import bvh_rehydrate as R  # noqa: E402


class PublishCleanupTest(unittest.TestCase):
    def test_failed_replace_makes_staged_file_writable_before_cleanup(self):
        with tempfile.TemporaryDirectory() as d:
            dest = os.path.join(d, "out.bin")
            with open(dest, "wb") as f:
                f.write(b"old")
            chmods = []
            real_chmod = os.chmod

            def chmod(path, mode, *args, **kwargs):
                chmods.append((str(path), mode))
                return real_chmod(path, mode, *args, **kwargs)

            with mock.patch.object(R.os, "replace", side_effect=OSError("replace denied")), \
                    mock.patch.object(R.os, "chmod", chmod):
                with self.assertRaises(OSError):
                    R._publish(dest, b"new")
            self.assertTrue(any(mode == 0o600 for _, mode in chmods), chmods)
            self.assertEqual([n for n in os.listdir(d) if n.startswith(".bvh_rehydrate-")], [])
            with open(dest, "rb") as f:
                self.assertEqual(f.read(), b"old")

    @unittest.skipUnless(os.name == "posix", "permission bits are POSIX")
    def test_replaced_output_keeps_0640(self):
        with tempfile.TemporaryDirectory() as d:
            dest = os.path.join(d, "out.bin")
            with open(dest, "wb") as f:
                f.write(b"old")
            os.chmod(dest, 0o640)
            R._publish(dest, b"new")
            self.assertEqual(os.stat(dest).st_mode & 0o777, 0o640)
            with open(dest, "rb") as f:
                self.assertEqual(f.read(), b"new")

    @unittest.skipUnless(os.name == "posix", "permission bits are POSIX")
    def test_read_only_destination_is_replaced_and_stays_read_only(self):
        with tempfile.TemporaryDirectory() as d:
            dest = os.path.join(d, "out.bin")
            with open(dest, "wb") as f:
                f.write(b"old")
            os.chmod(dest, 0o444)
            R._publish(dest, b"new")
            self.assertEqual(os.stat(dest).st_mode & 0o777, 0o444)
            with open(dest, "rb") as f:
                self.assertEqual(f.read(), b"new")

    @unittest.skipUnless(os.name == "posix", "permission bits are POSIX")
    def test_failed_replace_over_read_only_destination_keeps_real_error(self):
        with tempfile.TemporaryDirectory() as d:
            dest = os.path.join(d, "out.bin")
            with open(dest, "wb") as f:
                f.write(b"old")
            os.chmod(dest, 0o444)
            self.addCleanup(lambda: os.path.exists(dest) and os.chmod(dest, 0o600))
            with mock.patch.object(R.os, "replace", side_effect=OSError("replace denied")):
                with self.assertRaisesRegex(OSError, "replace denied"):
                    R._publish(dest, b"new")
            self.assertEqual([n for n in os.listdir(d) if n.startswith(".bvh_rehydrate-")], [])
            with open(dest, "rb") as f:
                self.assertEqual(f.read(), b"old")


if __name__ == "__main__":
    unittest.main()
