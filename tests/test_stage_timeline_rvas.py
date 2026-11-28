"""Exercise the boot timeline CLI with synthetic peek fields."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


TOOL = Path(__file__).resolve().parents[1] / "tools" / "stage_timeline.py"
GAME = "LEONIDA_GAME_STATE_RVA"
SESSION = "LEONIDA_SESSION_STATE_RVA"


class TimelineRvas(unittest.TestCase):
    def run_tool(self, log, *args, **settings):
        env = os.environ.copy()
        for key in (GAME, SESSION):
            env.pop(key, None)
        env.update(settings)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "boot.log"
            path.write_text(log, encoding="utf-8")
            return subprocess.run([sys.executable, "-I", str(TOOL), str(path), *args],
                                  capture_output=True, text=True, env=env)

    def test_settings_pair_press_and_ignore_unrelated_samples(self):
        log = ("[rt] peek t=1s: 0x001a=2 0x2b=1\n"
               "[rt] gameinput: scripted press of button 4 at 1.5 s\n"
               "[rt] peek t=2s: 0x99=5\n"
               "[rt] peek t=3s: 0x2B=100000003,dead 0x1A=3\n")
        run = self.run_tool(log, "--json", **{GAME: "0X1A", SESSION: "002b"})
        self.assertEqual(run.returncode, 0, run.stderr)
        data = json.loads(run.stdout)
        self.assertEqual(data["stages"], [{"t": 1, "stage": "init"}, {"t": 3, "stage": "landing"}])
        self.assertEqual(data["presses"], [{"t": 1.5, "button": 4, "next_change": data["stages"][1]}])

    def test_overrides_and_optional_loading_byte(self):
        log = ("[rt] peek t=1s: 0x10=3 0x20=0 0x30=102\n"
               "[rt] peek t=2s: 0x10=3 0x20=0 0x30=100\n")
        settings = {GAME: "aa", SESSION: "bb"}
        args = ("--game-state-rva", "10", "--session-state-rva", "0x20")
        for extra, ended in [((), False), (("--load-state-rva", "30"), True)]:
            with self.subTest(extra=extra):
                run = self.run_tool(log, *args, *extra, **settings)
                self.assertEqual(run.returncode, 0, run.stderr)
                self.assertIn("story load", run.stdout)
                self.assertEqual("loading screen ended" in run.stdout, ended)

    def test_unmatched_or_absent_peeks_report_an_error(self):
        for log in ("[rt] peek t=1s: 0xa=3 0xb=0\n", "no samples\n"):
            with self.subTest(log=log):
                run = self.run_tool(log, "--json", **{GAME: "10", SESSION: "20"})
                self.assertEqual(run.returncode, 1)
                self.assertEqual(run.stdout, "")
                self.assertIn("LEONIDA_PEEK", run.stderr)

    def test_unreadable_sample_is_still_a_match(self):
        run = self.run_tool("[rt] peek t=1s: 0x10=? 0x20=?\n", "--json", **{GAME: "10", SESSION: "20"})
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(json.loads(run.stdout)["stages"], [{"t": 1, "stage": "no title state yet"}])

    def test_missing_or_invalid_configuration(self):
        log = "[rt] peek t=1s: 0x10=2\n"
        missing = self.run_tool(log)
        self.assertEqual(missing.returncode, 2)
        self.assertIn(GAME, missing.stderr)
        invalid = self.run_tool(log, "--game-state-rva", "oops", **{SESSION: "20"})
        self.assertEqual(invalid.returncode, 2)
        self.assertIn("hex", invalid.stderr)


if __name__ == "__main__":
    unittest.main()
