"""Synthetic checks for durable, isolated shader-index publication.

    python -I tests/test_rdna2spirv_index_write.py
"""
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import rdna2spirv as R  # noqa: E402


class WriteIndexTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.out = Path(self.tmp.name)
        self.path = self.out / "index.part0.json"
        self.previous = b'{"previous": {}}\n'
        self.path.write_bytes(self.previous)
        self.index = {"z": {"synthetic": True}, "a": {}}

    def assert_previous_only(self):
        self.assertEqual(self.path.read_bytes(), self.previous)
        self.assertEqual(list(self.out.iterdir()), [self.path])

    def test_existing_tmp_file_is_not_overwritten_or_removed(self):
        neighbor = Path(str(self.path) + ".tmp")
        neighbor.write_bytes(b"unrelated input")
        R.write_index(str(self.path), self.index)
        self.assertEqual(neighbor.read_bytes(), b"unrelated input")
        self.assertEqual(self.path.read_bytes(),
                         json.dumps(self.index, indent=0, sort_keys=True).encode("utf-8"))
        self.assertEqual(set(self.out.iterdir()), {self.path, neighbor})

    def test_stale_tmp_directory_does_not_block_publication(self):
        neighbor = Path(str(self.path) + ".tmp")
        neighbor.mkdir()
        R.write_index(str(self.path), self.index)
        self.assertEqual(json.loads(self.path.read_text(encoding="utf-8")), self.index)
        self.assertTrue(neighbor.is_dir())
        self.assertEqual(set(self.out.iterdir()), {self.path, neighbor})

    def test_serialization_error_preserves_index_and_removes_staging(self):
        with self.assertRaises(TypeError):
            R.write_index(str(self.path), {"not_json": object()})
        self.assert_previous_only()

    def test_sync_error_preserves_index_and_removes_staging(self):
        with patch.object(R.os, "fsync", side_effect=OSError("sync failed")), \
                patch.object(R, "replace_retry") as publish:
            with self.assertRaisesRegex(OSError, "sync failed"):
                R.write_index(str(self.path), self.index)
            publish.assert_not_called()
        self.assert_previous_only()

    def test_replace_error_or_interruption_removes_staging(self):
        for error in (PermissionError("replace failed"), KeyboardInterrupt()):
            with self.subTest(error=type(error).__name__):
                with patch.object(R, "replace_retry", side_effect=error):
                    with self.assertRaises(type(error)):
                        R.write_index(str(self.path), self.index)
                self.assert_previous_only()

    def test_overlapping_writers_stage_separately_outside_merge_glob(self):
        real_replace = R.replace_retry
        staged = []
        inner = {"other": {}}

        def interleave(source, destination):
            staged.append(source)
            self.assertEqual(list(self.out.glob("index.part*.json")), [self.path])
            self.assertFalse(str(source).endswith(".json"))
            if len(staged) == 1:
                self.assertEqual(self.path.read_bytes(), self.previous)
                R.write_index(str(self.path), inner)
                self.assertEqual(json.loads(self.path.read_text(encoding="utf-8")), inner)
            real_replace(source, destination)

        with patch.object(R, "replace_retry", side_effect=interleave):
            R.write_index(str(self.path), self.index)
        self.assertEqual(len(set(staged)), 2)
        self.assertEqual(json.loads(self.path.read_text(encoding="utf-8")), self.index)
        self.assertEqual(list(self.out.iterdir()), [self.path])

    def test_posix_mode_is_applied_before_publication(self):
        real_replace = R.replace_retry
        def publish(source, destination):
            mode.assert_called_once_with(str(self.path))
            chmod.assert_called_once_with(source, 0o640)
            real_replace(source, destination)

        with patch.object(R.os, "name", "posix"), \
                patch.object(R, "published_mode", return_value=0o640, create=True) as mode, \
                patch.object(R.os, "chmod") as chmod, \
                patch.object(R, "replace_retry", side_effect=publish):
            R.write_index(str(self.path), self.index)
        self.assertEqual(json.loads(self.path.read_text(encoding="utf-8")), self.index)

    def test_mode_error_preserves_index_and_removes_staging(self):
        for operation in ("mode", "chmod"):
            with self.subTest(operation=operation):
                with patch.object(R.os, "name", "posix"), \
                        patch.object(R, "published_mode", return_value=0o640, create=True) as mode, \
                        patch.object(R.os, "chmod") as chmod:
                    (mode if operation == "mode" else chmod).side_effect = OSError("mode failed")
                    with self.assertRaisesRegex(OSError, "mode failed"):
                        R.write_index(str(self.path), self.index)
                self.assert_previous_only()

    @unittest.skipUnless(os.name == "posix", "POSIX permission bits")
    def test_permissions_of_existing_and_new_indexes(self):
        os.chmod(self.path, 0o640)
        R.write_index(str(self.path), self.index)
        self.assertEqual(self.path.stat().st_mode & 0o777, 0o640)
        self.path.unlink()
        probe = self.out / "ordinary-file"
        probe.write_bytes(b"")
        R.write_index(str(self.path), self.index)
        self.assertEqual(self.path.stat().st_mode & 0o777, probe.stat().st_mode & 0o777)

    def test_publish_cleans_up_staged_file_when_replace_fails_even_if_readonly(self):
        os.chmod(self.path, 0o444)
        real_unlink = os.unlink

        def windows_strict_unlink(p):
            mode = os.stat(p).st_mode
            if not (mode & 0o200):
                raise PermissionError(13, f"Access is denied: {p}")
            return real_unlink(p)

        with patch.object(R, "replace_retry", side_effect=OSError(13, "Permission denied")), \
             patch("os.unlink", side_effect=windows_strict_unlink):
            with self.assertRaisesRegex(OSError, "Permission denied"):
                R.write_index(str(self.path), self.index)

        leftover = [f for f in os.listdir(self.out) if f.startswith(".index-")]
        self.assertEqual(leftover, [])

    def test_publish_applies_published_mode_on_windows(self):
        chmod_calls = []
        real_chmod = os.chmod

        def track_chmod(p, mode):
            chmod_calls.append((str(p), mode))
            real_chmod(p, mode)

        with patch.object(R.os, "name", "nt"), patch.object(R.os, "chmod", side_effect=track_chmod):
            R.write_index(str(self.path), self.index)
        self.assertTrue(any(".index-" in p and mode == R.published_mode(str(self.path)) for p, mode in chmod_calls), chmod_calls)


if __name__ == "__main__":
    unittest.main()
