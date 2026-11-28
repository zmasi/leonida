"""Synthetic tests for tools/anim_skeleton_match.py (standard library only; no game data)."""
import json
import os
import sys
import tempfile
import unittest
import zlib
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import anim_skeleton_match as asm  # noqa: E402


class SkeletonMatchTests(unittest.TestCase):
    def test_extract_and_compare_case_insensitive_and_joaat32(self):
        skel_blob = b"hdr\x00SKEL_ROOT\x00SKEL_Head\x00SKEL_Pelvis\x00"
        anim_streams = [
            asm.extract_skel_tokens(b"\x00SKEL_ROOT\x00SKEL_HEAD\x00"),
            asm.extract_skel_tokens(b"\x00SKEL_Pelvis\x00SKEL_Unknown\x00"),
        ]
        summary = asm.compare_sets(asm.extract_skel_tokens(skel_blob), anim_streams)
        self.assertEqual(summary["distinct_anim_tokens"], 4)
        self.assertEqual(summary["distinct_skeleton_tokens"], 3)
        self.assertEqual(summary["matched_exact"], 2)
        self.assertEqual(summary["matched_case_insensitive"], 3)
        self.assertEqual(summary["matched_joaat32"], 3)
        self.assertEqual(summary["unmatched_case_insensitive"], 1)
        self.assertEqual(summary["streams_all_matched_exact"], 0)
        self.assertEqual(summary["streams_all_matched_ci"], 1)
        self.assertEqual(summary["streams_any_matched_ci"], 2)

    def test_cli_synthetic_archives_and_overwrite_guard(self):
        with tempfile.TemporaryDirectory() as tmp:
            packs_p = os.path.join(tmp, "packs.rpf")
            anim_p = os.path.join(tmp, "anim.rpf")
            out_p = os.path.join(tmp, "report.json")
            with open(packs_p, "wb") as f:
                f.write(b"RPF9" + bytes(12) + zlib.compress(b"SKEL_ROOT\x00SKEL_Spine0\x00" + b"A" * 128))
            with open(anim_p, "wb") as f:
                f.write(b"RPF9" + bytes(12) + zlib.compress(b"SKEL_ROOT\x00SKEL_Spine0\x00" + b"B" * 128))
            rc = asm.main([packs_p, anim_p, "--out", out_p])
            self.assertEqual(rc, 0)
            with open(out_p, encoding="utf-8") as f:
                data = json.load(f)
            self.assertEqual(data["matched_exact"], 2)
            self.assertEqual(data["unmatched_case_insensitive"], 0)
            with self.assertRaises(SystemExit) as cm:
                asm.main([packs_p, anim_p, "--out", packs_p])
            self.assertEqual(cm.exception.code, 2)
            with self.assertRaises(SystemExit) as cm:
                asm.main([packs_p, anim_p, "--out", os.path.join(tmp, "nodir", "rep.json")])
            self.assertEqual(cm.exception.code, 2)
            with self.assertRaises(SystemExit) as cm:
                asm.main([packs_p, anim_p, "--out", tmp])
            self.assertEqual(cm.exception.code, 2)
            empty_p = os.path.join(tmp, "empty.rpf")
            open(empty_p, "wb").close()
            for bad_in in (os.path.join(tmp, "missing.rpf"), empty_p, tmp):
                with self.subTest(bad_in=bad_in), self.assertRaises(SystemExit) as cm:
                    asm.main([bad_in, anim_p])
                self.assertEqual(cm.exception.code, 2)

    def test_json_write_failure_preserves_previous_report(self):
        with tempfile.TemporaryDirectory() as tmp:
            previous = '{"previous": true}\n'
            path = os.path.join(tmp, "keep.json")
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(previous)
            with mock.patch.object(asm, "replace_retry", side_effect=OSError(13, "Permission denied")):
                with self.assertRaises(OSError):
                    asm._write_json_report(path, '{"new": true}')
            with open(path, encoding="utf-8") as f:
                self.assertEqual(f.read(), previous)

    def test_publish_cleans_up_staged_file_when_replace_fails_even_if_readonly(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "report.json")
            real_unlink = os.unlink
            unlink_calls = []

            def windows_strict_unlink(p):
                mode = os.stat(p).st_mode
                if not (mode & 0o200):
                    raise PermissionError(13, f"Access is denied: {p}")
                unlink_calls.append(p)
                return real_unlink(p)

            replace_calls = []

            def fake_replace(src, dst):
                replace_calls.append((src, dst))
                raise OSError(13, "Permission denied")

            with mock.patch.object(asm, "published_mode", return_value=0o444), \
                 mock.patch.object(asm, "replace_retry", side_effect=fake_replace), \
                 mock.patch("os.unlink", side_effect=windows_strict_unlink):
                with self.assertRaises(OSError):
                    asm._write_json_report(path, '{"failed": true}')

            self.assertEqual(len(replace_calls), 1)
            self.assertEqual(len(unlink_calls), 1)
            leftover = [f for f in os.listdir(tmp) if f.startswith(".anim_skeleton_match-")]
            self.assertEqual(leftover, [])

    def test_publish_applies_published_mode_on_windows(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "report_win.json")
            chmod_calls = []
            real_chmod = os.chmod

            def track_chmod(p, mode):
                chmod_calls.append((p, mode))
                real_chmod(p, mode)

            with mock.patch("os.name", "nt"), mock.patch("os.chmod", side_effect=track_chmod):
                asm._write_json_report(path, '{"win": true}')
            self.assertTrue(any(p.endswith(".tmp") and mode == asm.published_mode(path) for p, mode in chmod_calls), chmod_calls)


if __name__ == "__main__":
    unittest.main()
