"""tools/leonida_launcher.py: settings, the command line and the mod list (no display, no game data).

    python -I tests/test_leonida_launcher.py -v
"""
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import leonida_launcher as launcher  # noqa: E402


def settings(**changes):
    value = dict(launcher.DEFAULTS, game_dir="D:/Games/GTA")
    value.update(changes)
    return value


class SettingsTests(unittest.TestCase):
    def test_round_trip_and_line_endings(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "sub", "launcher.json")
            launcher.save_settings(settings(dlss=False, resolution="1080", data_dir="E:/d"), path)
            with open(path, "rb") as f:
                raw = f.read()
            self.assertNotIn(b"\r", raw)
            self.assertTrue(raw.endswith(b"}\n"))
            loaded = launcher.load_settings(path)
            self.assertFalse(loaded["dlss"])
            self.assertEqual((loaded["resolution"], loaded["data_dir"]), ("1080", "E:/d"))

    def test_missing_or_broken_file_gives_defaults(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(launcher.load_settings(os.path.join(tmp, "none.json")), launcher.DEFAULTS)
            bad = os.path.join(tmp, "bad.json")
            Path(bad).write_text("{not json", encoding="utf-8")
            self.assertEqual(launcher.load_settings(bad), launcher.DEFAULTS)

    def test_wrong_types_and_unknown_keys_are_dropped(self):
        got = launcher.normalise({"dlss": "yes", "resolution": "999", "game_dir": 5, "extra": 1, "log": True})
        self.assertEqual(got["dlss"], True)
        self.assertEqual(got["resolution"], "")
        self.assertEqual(got["game_dir"], "")
        self.assertTrue(got["log"])
        self.assertNotIn("extra", got)
        self.assertEqual(launcher.normalise([1]), launcher.DEFAULTS)

    def test_game_folder_check(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertFalse(launcher.game_exe_present(tmp))
            Path(tmp, launcher.EXPECTED_EXE).write_bytes(b"")
            self.assertTrue(launcher.game_exe_present(tmp))
            self.assertFalse(launcher.game_exe_present(""))


class CommandTests(unittest.TestCase):
    def test_default_windows_command(self):
        command = launcher.play_command(settings(), repo="R", windows=True)
        self.assertEqual(command[:6], ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                                       os.path.join("R", "scripts", "play-d3d12.ps1")])
        self.assertEqual(command[6:], ["-Game", "D:/Games/GTA"])

    def test_every_option_maps_to_its_switch(self):
        command = launcher.play_command(
            settings(data_dir="E:/d", dlss=False, ray_tracing=True, smooth_motion=True, precompile=False,
                     series_x=True, quality=True, resolution="1440", capture=True, log=True),
            repo="R", windows=True)
        self.assertEqual(command[6:], ["-Game", "D:/Games/GTA", "-Data", "E:/d", "-NoDlss", "-RayTracing",
                                       "-SmoothMotion", "-NoPrecompile", "-SeriesX", "-Quality",
                                       "-Resolution", "1440", "-Capture", "-Log"])

    def test_switches_exist_in_the_script(self):
        text = (ROOT / "scripts" / "play-d3d12.ps1").read_text(encoding="utf-8")
        every = settings(dlss=False, ray_tracing=True, smooth_motion=True, precompile=False, series_x=True,
                         quality=True, resolution="720", capture=True, log=True)
        for arg in launcher.switch_args(every):
            if arg.startswith("-") and arg[1:].isalpha():
                self.assertIn("$" + arg[1:], text, arg)
        self.assertIn("$Game", text)
        self.assertIn("$Data", text)

    def test_linux_command(self):
        command = launcher.play_command(settings(game_dir="/g", series_x=True, dlss=False), repo="/r",
                                        windows=False)
        self.assertEqual(command, ["sh", os.path.join("/r", "scripts", "play-linux.sh"),
                                   os.path.join("/g", launcher.EXPECTED_EXE), "--series-x"])

    def test_no_game_folder_is_refused(self):
        with self.assertRaises(ValueError):
            launcher.play_command(settings(game_dir="  "), windows=True)

    def test_tools(self):
        self.assertEqual(launcher.tool_command("get_dlss", repo="R", windows=True)[:2], ["cmd", "/c"])
        self.assertTrue(launcher.tool_command("build", repo="R", windows=True)[-1].endswith("build-nvidia.ps1"))
        with self.assertRaises(ValueError):
            launcher.tool_command("build", windows=False)
        with self.assertRaises(ValueError):
            launcher.tool_command("nope", windows=True)

    def test_state_follows_the_scripts_messages(self):
        state = "Starting"
        for line, want in (("Series S, 720p. F1 in the game: settings.\n", "Starting"),
                           ("Precompiling shaders (minutes) ...\n", "Preparing shaders"),
                           ("  1000 shaders\n", "Preparing shaders"),
                           ("log: x.log\n", "Running")):
            state = launcher.state_for_line(state, line)
            self.assertEqual(state, want, line)
        self.assertEqual(launcher.state_for_line("Starting", "Building the layer (a few minutes) ...\n"),
                         "Building the runtime")


class ModsAndPicturesTests(unittest.TestCase):
    def test_list_and_toggle_mods(self):
        with tempfile.TemporaryDirectory() as tmp:
            mod = Path(tmp, "alpha")
            mod.mkdir()
            manifest = mod / "mod.json"
            manifest.write_text(json.dumps({"name": "Alpha", "archetype_swaps": [{"from": "a_b", "to": "c_d"}]}),
                                encoding="utf-8")
            Path(tmp, "broken").mkdir()
            Path(tmp, "broken", "mod.json").write_text("{", encoding="utf-8")
            Path(tmp, "notamod").mkdir()
            rows, errors = launcher.list_mods(tmp)
            self.assertEqual([(r["folder"], r["name"], r["enabled"], r["swaps"]) for r in rows],
                             [("alpha", "Alpha", True, 1)])
            self.assertEqual(len(errors), 1)
            launcher.set_mod_enabled(rows[0]["path"], False)
            self.assertFalse(json.loads(manifest.read_text(encoding="utf-8"))["enabled"])
            self.assertNotIn(b"\r", manifest.read_bytes())
            self.assertFalse(launcher.list_mods(tmp)[0][0]["enabled"])
            self.assertEqual(launcher.list_mods(os.path.join(tmp, "missing")), ([], []))

    def test_newest_picture(self):
        with tempfile.TemporaryDirectory() as tmp:
            old, new = Path(tmp, "a", "old.png"), Path(tmp, "b", "new.png")
            old.parent.mkdir()
            new.parent.mkdir()
            old.write_bytes(b"x")
            new.write_bytes(b"x")
            Path(tmp, "note.txt").write_bytes(b"x")
            os.utime(old, (1000, 1000))
            os.utime(new, (2000, 2000))
            self.assertEqual(launcher.newest_picture([tmp]), str(new))
            self.assertIsNone(launcher.newest_picture([os.path.join(tmp, "none")]))


if __name__ == "__main__":
    unittest.main()
