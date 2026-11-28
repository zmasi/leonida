"""Synthetic regression tests for tools/stage_timeline.py. No game data required.

    python -I tests/test_stage_timeline.py
"""
import contextlib
import io
import json
import os
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")
TOOL = os.path.join(TOOLS, "stage_timeline.py")
if TOOLS not in sys.path:
    sys.path.insert(0, TOOLS)

import stage_timeline  # noqa: E402


RVA_ARGS = ("--game-state-rva", "1a", "--session-state-rva", "2b", "--load-state-rva", "3c")


class StageTimelineTests(unittest.TestCase):
    def test_parses_boot_timeline_and_pairs_presses_json(self):
        log = "\n".join([
            "[rt] peek t=1s: 0x1a=2,0 0x2b=1,0 0x3c=0,0",
            "[rt] peek t=3s: 0x1a=3,0 0x2b=1,0 0x3c=0,0",
            "[rt] gameinput: scripted press of button 1 at 4.0 s",
            "[rt] peek t=5s: 0x1a=3,0 0x2b=3,0 0x3c=0,0",
            "[rt] gameinput: scripted press of button 1 at 6.5 s",
            "[rt] peek t=8s: 0x1a=3,0 0x2b=0,0 0x3c=2,0",
            "[rt] peek t=10s: 0x1a=3,0 0x2b=0,0 0x3c=0,0",
        ]) + "\n"
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "boot.log")
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(log)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                rc = stage_timeline.main([path, "--json", *RVA_ARGS])
        self.assertEqual(rc, 0)
        doc = json.loads(out.getvalue())
        self.assertEqual(doc["stages"], [
            {"t": 1, "stage": "init"},
            {"t": 3, "stage": "boot screens"},
            {"t": 5, "stage": "landing"},
            {"t": 8, "stage": "story load"},
            {"t": 10, "stage": "story load (loading screen ended)"},
        ])
        self.assertEqual(doc["presses"][0]["next_change"], {"t": 5, "stage": "landing"})
        self.assertEqual(doc["presses"][1]["next_change"], {"t": 8, "stage": "story load"})

    def test_first_value_handles_empty_leading_comma_and_partial_unknowns(self):
        self.assertIsNone(stage_timeline.first_value(None))
        self.assertIsNone(stage_timeline.first_value(""))
        self.assertIsNone(stage_timeline.first_value(",1234"))
        self.assertIsNone(stage_timeline.first_value("?0"))
        self.assertIsNone(stage_timeline.first_value("1?2,0"))
        self.assertEqual(stage_timeline.first_value("a,b"), 0xA)

    def test_malformed_press_timestamp_does_not_crash_read(self):
        log = "\n".join([
            "[rt] peek t=2s: 0x1a=3,0 0x2b=1,0",
            "[rt] gameinput: scripted press of button 1 at . s",
            "[rt] gameinput: scripted press of button 1 at 1.2.3 s",
            "[rt] gameinput: scripted press of button 2 at 3 s",
        ]) + "\n"
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "boot.log")
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(log)
            peeks, presses = stage_timeline.read(path, 0x1a, 0x2b, 0x3c)
        self.assertEqual(len(peeks), 1)
        self.assertEqual(presses, [{"t": 3.0, "button": 2}])

    def test_ended_loading_screen_requires_running_game_state(self):
        peeks = [
            {"t": 1, "game": 3, "session": 0, "load": 2},
            {"t": 2, "game": 2, "session": 0, "load": 0},
            {"t": 3, "game": 3, "session": 0, "load": 0},
        ]
        events = stage_timeline.timeline(peeks, [])
        self.assertEqual(events, [
            {"t": 1, "stage": "story load"},
            {"t": 2, "stage": "init"},
            {"t": 3, "stage": "story load (loading screen ended)"},
        ])

    def test_utf8_bom_log_is_read(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "bom.log")
            with open(path, "w", encoding="utf-8-sig", newline="\n") as f:
                f.write("[rt] peek t=1s: 0x1a=2,0 0x2b=1,0\n")
            peeks, _ = stage_timeline.read(path, 0x1a, 0x2b, 0x3c)
        self.assertEqual(len(peeks), 1)
        self.assertEqual(peeks[0]["game"], 2)

    def test_missing_or_directory_log_returns_exit_2_without_traceback(self):
        with tempfile.TemporaryDirectory() as tmp:
            missing = os.path.join(tmp, "missing.log")
            for target in (missing, tmp):
                with self.subTest(target=target):
                    p = subprocess.run(
                        [sys.executable, "-I", TOOL, target, *RVA_ARGS],
                        capture_output=True, encoding="utf-8", errors="replace",
                    )
                    self.assertEqual(p.returncode, 2, p.stdout + p.stderr)
                    self.assertNotIn("Traceback", p.stderr)
                    self.assertIn(target, p.stderr)
                    err = io.StringIO()
                    with contextlib.redirect_stderr(err):
                        rc = stage_timeline.main([target, *RVA_ARGS])
                    self.assertEqual(rc, 2)
                    self.assertIn(target, err.getvalue())

    def test_main_block_guards_stdout_reconfigure(self):
        with open(TOOL, encoding="utf-8") as f:
            text = f.read()
        m = re.search(r"^if __name__ == ['\"]__main__['\"]:\n", text, re.M)
        self.assertIsNotNone(m)
        self.assertIn('sys.stdout.reconfigure(errors="replace")', text[m.end():m.end() + 400])


if __name__ == "__main__":
    unittest.main()
