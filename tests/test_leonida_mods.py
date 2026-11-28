"""tools/leonida_mods.py: local mod manifests, folders and ZIP packages. No game data.

    python -I tests/test_leonida_mods.py -v
"""
import contextlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import leonida_mods  # noqa: E402

MANDY = {
    "name": "Mandy in the prison intro",
    "enabled": True,
    "game_version": "1.0.2.1",
    "archetype_swaps": [
        {"from": "player_story000_00", "to": "cs_mandy", "when": "always"}
    ],
}


def write_json(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, indent=2)
        f.write("\n")


def read_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def cli(*argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = leonida_mods.main(list(argv))
    return code, out.getvalue(), err.getvalue()


class ManifestTests(unittest.TestCase):
    def test_hashes_match_the_runtime_contract(self):
        self.assertEqual(leonida_mods.joaat("player_story000_00"), 0xE472B687)
        self.assertEqual(leonida_mods.joaat("cs_mandy"), 0xD7C0CDFF)
        self.assertEqual(leonida_mods.parse_model_id("0xD7C0CDFF"), 0xD7C0CDFF)
        self.assertEqual(leonida_mods.parse_model_id("  player_story000_00 \t"), 0xE472B687)
        self.assertEqual(leonida_mods.parse_model_id("\n0xD7C0CDFF "), 0xD7C0CDFF)
        with self.assertRaises(ValueError):
            leonida_mods.parse_model_id(" \t\n ")
        with self.assertRaises(ValueError):
            leonida_mods.parse_model_id("0x")
        with self.assertRaises(ValueError):
            leonida_mods.parse_model_id("0x123456789")

    def test_manifest_validation_matches_runtime_rules(self):
        mod = leonida_mods.validate_manifest(MANDY, "mandy_prison_intro")
        self.assertEqual(mod.name, "Mandy in the prison intro")
        self.assertEqual(mod.swaps[0].from_id, 0xE472B687)
        self.assertEqual(mod.swaps[0].to_id, 0xD7C0CDFF)

        padded = dict(MANDY)
        padded["archetype_swaps"] = [{"from": " player_story000_00 ", "to": " cs_mandy\t"}]
        padded_mod = leonida_mods.validate_manifest(padded, "padded_names")
        self.assertEqual(padded_mod.swaps[0].from_id, 0xE472B687)
        self.assertEqual(padded_mod.swaps[0].to_id, 0xD7C0CDFF)

        def bad(data, needle):
            with self.assertRaises(leonida_mods.ModsError) as cm:
                leonida_mods.validate_manifest(data, "bad")
            self.assertIn(needle, str(cm.exception))

        bad({"enabled": "yes"}, '"enabled" must be true or false')
        bad({"archetype_swaps": [{"from": "a", "to": "b", "when": "mission:intro"}]},
            '"when" can only be "always"')
        bad({"archetype_swaps": [{"from": "cs_mandy", "to": "0xD7C0CDFF"}]}, "with itself")
        bad({"archetype_swaps": "all"}, '"archetype_swaps" must be a list')
        bad({"archetype_swaps": None}, '"archetype_swaps" must be a list')
        bad([1, 2], "not a JSON object")

    def test_camera_section_matches_runtime_rules(self):
        # The cases of runtime/test/mods_test.cpp, "camera": the manager refuses what parse_mod refuses.
        mod = leonida_mods.validate_manifest(
            {"camera": {"view_mode": "first_person", "first_person_value": 4, "contexts": [0]}}, "fp")
        self.assertEqual((mod.camera.view_mode, mod.camera.first_person_value, mod.camera.contexts),
                         ("first_person", 4, [0]))
        self.assertEqual(mod.warnings, [])     # a camera mod changes something without any swap
        mod = leonida_mods.validate_manifest({"camera": {"view_mode": "default"}}, "d")
        self.assertEqual((mod.camera.view_mode, mod.camera.first_person_value, mod.camera.contexts),
                         ("default", 4, []))
        mod = leonida_mods.validate_manifest(
            {"camera": {"view_mode": "first_person", "first_person_value": 255.0,
                        "contexts": [2147483647, 2147483648, 4294967295]}}, "wide")
        self.assertEqual(mod.camera.first_person_value, 255)
        self.assertEqual(mod.camera.contexts, [2147483647, 2147483648, 4294967295])
        self.assertIsNone(leonida_mods.validate_manifest(MANDY, "mandy_prison_intro").camera)
        self.assertEqual(leonida_mods.validate_manifest({}, "empty").warnings,
                         ["no archetype_swaps; this mod currently changes nothing"])
        self.assertEqual(leonida_mods.validate_manifest({"archetype_swaps": []}, "empty").warnings,
                         ["archetype_swaps is empty; this mod currently changes nothing"])

        def bad(camera, needle):
            with self.assertRaises(leonida_mods.ModsError) as cm:
                leonida_mods.validate_manifest({"archetype_swaps": MANDY["archetype_swaps"], "camera": camera},
                                               "bad")
            self.assertIn(needle, str(cm.exception))

        bad("first_person", '"camera" must be an object')
        bad(None, '"camera" must be an object')
        bad({}, '"view_mode" must be "first_person" or "default"')
        bad({"view_mode": "top_down"}, '"view_mode" must be "first_person" or "default"')
        for value in (4.5, -1, 256, "4", True, None):
            bad({"view_mode": "first_person", "first_person_value": value},
                '"first_person_value" must be a whole number 0-255')
        bad({"view_mode": "first_person", "contexts": "all"}, '"contexts" must be a list of numbers')
        for value in (-1, 0.5, 4294967296, "0", False, None):
            bad({"view_mode": "first_person", "contexts": [0, value]},
                '"contexts" must be a list of whole numbers from 0 to 4294967295')


class ManagerTests(unittest.TestCase):
    def test_new_list_enable_disable_and_uninstall(self):
        with tempfile.TemporaryDirectory() as tmp:
            mods = os.path.join(tmp, "mods")
            code, out, err = cli("--dir", mods, "new", "lucia_to_mandy", "--name", "Mandy",
                                 "--game-version", "1.0.2.1",
                                 "--swap", "player_story000_00=cs_mandy")
            self.assertEqual((code, err), (0, ""))
            self.assertIn("created lucia_to_mandy", out)
            manifest = Path(mods, "lucia_to_mandy", "mod.json")
            self.assertEqual(read_json(manifest)["archetype_swaps"][0]["to"], "cs_mandy")

            code, out, err = cli("--dir", mods, "disable", "lucia_to_mandy")
            self.assertEqual((code, err), (0, ""))
            self.assertFalse(read_json(manifest)["enabled"])

            code, out, err = cli("--dir", mods, "list", "--json")
            self.assertEqual((code, err), (0, ""))
            report = json.loads(out)
            self.assertEqual(report["mods"][0]["folder"], "lucia_to_mandy")
            self.assertFalse(report["mods"][0]["enabled"])

            code, out, err = cli("--dir", mods, "uninstall", "lucia_to_mandy", "--yes")
            self.assertEqual((code, err), (0, ""))
            self.assertFalse(manifest.exists())

    def test_install_folder_package_and_pack_round_trip(self):
        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp, "package", "mandy_prison_intro")
            write_json(package / "mod.json", MANDY)
            mods = os.path.join(tmp, "mods")
            code, out, err = cli("--dir", mods, "install", str(Path(tmp, "package")))
            self.assertEqual((code, err), (0, ""))
            self.assertIn("installed mandy_prison_intro", out)
            installed = Path(mods, "mandy_prison_intro", "mod.json")
            self.assertEqual(read_json(installed), MANDY)

            packed = Path(tmp, "mandy.leonidamod")
            code, out, err = cli("--dir", mods, "pack", "mandy_prison_intro", "--out", str(packed))
            self.assertEqual((code, err), (0, ""))
            self.assertTrue(packed.is_file())
            with zipfile.ZipFile(packed) as archive:
                self.assertEqual(archive.namelist(), ["mandy_prison_intro/mod.json"])

            other_mods = os.path.join(tmp, "other-mods")
            code, out, err = cli("--dir", other_mods, "install", str(packed), "--disable")
            self.assertEqual((code, err), (0, ""))
            installed_copy = Path(other_mods, "mandy_prison_intro", "mod.json")
            self.assertFalse(read_json(installed_copy)["enabled"])

    def test_zip_install_rejects_path_traversal(self):
        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp, "bad.zip")
            with zipfile.ZipFile(package, "w") as archive:
                archive.writestr("mod.json", json.dumps(MANDY))
                archive.writestr("../escape.txt", "nope")
            code, out, err = cli("--dir", os.path.join(tmp, "mods"), "install", str(package))
            self.assertEqual(code, 2)
            self.assertEqual(out, "")
            self.assertIn("unsafe ZIP member", err)
            self.assertFalse(Path(tmp, "escape.txt").exists())

    def test_check_reports_broken_manifest(self):
        with tempfile.TemporaryDirectory() as tmp:
            broken = Path(tmp, "mods", "broken")
            broken.mkdir(parents=True)
            broken.joinpath("mod.json").write_text("{not json", encoding="utf-8")
            code, out, err = cli("--dir", os.path.join(tmp, "mods"), "check")
            self.assertEqual(code, 1)
            self.assertIn("skipped broken:", out)
            self.assertEqual(err, "")

    def test_manifest_with_a_byte_order_mark_is_read_as_the_runtime_reads_it(self):
        # docs/modding.md: a UTF-8 byte-order mark in front is accepted, a UTF-16 file is named as such.
        with tempfile.TemporaryDirectory() as tmp:
            mods = os.path.join(tmp, "mods")
            manifest = Path(mods, "marked", "mod.json")
            manifest.parent.mkdir(parents=True)
            manifest.write_bytes(b"\xef\xbb\xbf" + json.dumps(MANDY).encode("utf-8"))
            code, out, err = cli("--dir", mods, "check")
            self.assertEqual((code, err), (0, ""))
            self.assertIn("marked: Mandy in the prison intro [enabled; 1 swap", out)

            code, out, err = cli("--dir", mods, "disable", "marked")
            self.assertEqual((code, err), (0, ""))
            self.assertEqual(read_json(manifest), {**MANDY, "enabled": False})

            source = Path(tmp, "source", "from_notepad", "mod.json")
            source.parent.mkdir(parents=True)
            source.write_bytes(b"\xef\xbb\xbf" + json.dumps(MANDY).encode("utf-8"))
            code, out, err = cli("--dir", mods, "install", str(source.parent))
            self.assertEqual((code, err), (0, ""))
            self.assertIn("installed from_notepad", out)

            for name, encoding in (("le", "utf-16"), ("bare", "utf-16-le")):
                Path(mods, name).mkdir()
                Path(mods, name, "mod.json").write_bytes(json.dumps(MANDY).encode(encoding))
            code, out, err = cli("--dir", mods, "check")
            self.assertEqual((code, err), (1, ""))
            self.assertIn("skipped le: mod.json is UTF-16; save it as UTF-8", out)
            self.assertIn("skipped bare: mod.json is UTF-16; save it as UTF-8", out)

    def test_list_and_check_report_the_camera_section(self):
        with tempfile.TemporaryDirectory() as tmp:
            mods = Path(tmp, "mods")
            write_json(mods / "a_first" / "mod.json",
                       {"camera": {"view_mode": "first_person", "first_person_value": 4, "contexts": [0, 2]}})
            write_json(mods / "b_default" / "mod.json", {"camera": {"view_mode": "default"}})
            code, out, err = cli("--dir", str(mods), "list")
            self.assertEqual((code, err), (0, ""))
            self.assertIn("a_first: a_first [enabled; 0 swaps, camera]", out)
            self.assertIn("  camera first_person (value 4; contexts 0, 2)\n", out)
            self.assertIn("  camera default (the game's own view)  (overrides a_first)\n", out)
            self.assertNotIn("changes nothing", out)

            code, out, err = cli("--dir", str(mods), "list", "--json")
            self.assertEqual((code, err), (0, ""))
            report = {m["folder"]: m for m in json.loads(out)["mods"]}
            self.assertEqual(report["a_first"]["camera"],
                             {"view_mode": "first_person", "first_person_value": 4, "contexts": [0, 2]})
            self.assertEqual(report["a_first"]["warnings"], [])

            # A section the runtime skips the mod for is an error here too, not a mod that "checks" clean.
            write_json(mods / "c_broken" / "mod.json", {"camera": {"view_mode": "sideways"}})
            code, out, err = cli("--dir", str(mods), "check")
            self.assertEqual((code, err), (1, ""))
            self.assertIn('skipped c_broken: "camera": "view_mode" must be "first_person" or "default"', out)

    def test_swap_conflicts_appear_in_json_text_and_strict_check(self):
        with tempfile.TemporaryDirectory() as tmp:
            mods = Path(tmp, "mods")
            write_json(mods / "a_first" / "mod.json", MANDY)
            write_json(mods / "b_disabled" / "mod.json", {**MANDY, "enabled": False})
            write_json(mods / "c_second" / "mod.json", {
                "name": "Second swap",
                "enabled": True,
                "game_version": "1.0.2.1",
                "archetype_swaps": [
                    {"from": "0xE472B687", "to": "cs_monalisa", "when": "always"},
                ],
            })

            code, out, err = cli("--dir", str(mods), "check")
            self.assertEqual((code, err), (0, ""))
            self.assertIn("(overrides a_first)", out)
            self.assertNotIn("overrides b_disabled", out)

            code, out, err = cli("--dir", str(mods), "list", "--json")
            self.assertEqual((code, err), (0, ""))
            report = json.loads(out)
            self.assertEqual(report["conflicts"], [{
                "from_hash": "0xE472B687",
                "from": "0xE472B687",
                "winner": "c_second",
                "overrides": "a_first",
            }])
            by_folder = {m["folder"]: m for m in report["mods"]}
            self.assertNotIn("overrides", by_folder["a_first"]["archetype_swaps"][0])
            self.assertNotIn("overrides", by_folder["b_disabled"]["archetype_swaps"][0])
            self.assertEqual(by_folder["c_second"]["archetype_swaps"][0]["overrides"], "a_first")

            code, out, err = cli("--dir", str(mods), "check", "--strict")
            self.assertEqual((code, err), (1, ""))

    def test_strict_check_fails_on_manifest_warnings(self):
        with tempfile.TemporaryDirectory() as tmp:
            mods = Path(tmp, "mods")
            write_json(mods / "empty_mod" / "mod.json", {
                "name": "Empty",
                "enabled": True,
                "game_version": "1.0.2.1",
                "archetype_swaps": [],
            })
            code, _, err = cli("--dir", str(mods), "check")
            self.assertEqual((code, err), (0, ""))
            code, _, err = cli("--dir", str(mods), "check", "--strict")
            self.assertEqual((code, err), (1, ""))

    def test_run_print_builds_gta6rt_command(self):
        with tempfile.TemporaryDirectory() as tmp:
            port = Path(tmp, "port")
            game = Path(tmp, "game")
            port.mkdir()
            game.mkdir()
            # the launcher runtime_executable() looks for here: Windows takes only gta6rt.exe
            launcher = "gta6rt.exe" if os.name == "nt" else "gta6rt.sh"
            port.joinpath(launcher).write_bytes(b"MZ" if os.name == "nt" else b"#!/bin/sh\n")
            port.joinpath("game_path.txt").write_text(str(game) + "\n", encoding="utf-8")
            image = game / leonida_mods.EXPECTED_EXE
            image.write_bytes(b"MZ")
            code, out, err = cli("--port", str(port), "run", "--dry-run", "--print",
                                 "--archetype-lookup", "1A2B3C0", "--", "--windowed")
            self.assertEqual((code, err), (0, ""))
            self.assertIn(" --dry-run ", " " + out)
            self.assertIn("--mods", out)
            self.assertIn(os.path.join(str(port), "mods"), out)
            self.assertIn("--archetype-lookup 1A2B3C0", out)
            self.assertTrue(out.rstrip().endswith("--windowed"))


class SamplesAndGuiTests(unittest.TestCase):
    def test_bundled_basic_mods_are_valid_and_safe(self):
        mods, errors = leonida_mods.load_mods(str(ROOT / "mods"))
        self.assertEqual(errors, [])
        by_folder = {m.folder: m for m in mods}
        self.assertIn("mandy_prison_intro", by_folder)
        self.assertIn("monalisa_player_model", by_folder)
        self.assertIn("lucia_cutscene_model", by_folder)
        self.assertIn("mandy_to_monalisa", by_folder)
        self.assertTrue(by_folder["mandy_prison_intro"].enabled)
        self.assertFalse(by_folder["monalisa_player_model"].enabled)
        self.assertFalse(by_folder["lucia_cutscene_model"].enabled)
        self.assertFalse(by_folder["mandy_to_monalisa"].enabled)
        active = leonida_mods.active_swaps(mods)
        self.assertEqual(active, {0xE472B687: by_folder["mandy_prison_intro"].swaps[0]})
        # mods/first_person has a "camera" section and no swap: it is not a mod that "changes nothing".
        first_person = by_folder["first_person"]
        self.assertEqual((first_person.camera.view_mode, first_person.camera.first_person_value,
                          first_person.camera.contexts), ("first_person", 4, [0]))
        self.assertEqual(first_person.warnings, [])

    def test_gui_is_an_explicit_lazy_tkinter_entry_point(self):
        source = (ROOT / "tools" / "leonida_mods.py").read_text(encoding="utf-8")
        self.assertIn('commands.add_parser("gui"', source)
        self.assertIn("import tkinter as tk", source)
        self.assertLess(source.index("def run_gui"), source.index("import tkinter as tk"))
        args = leonida_mods.build_parser().parse_args(["--dir", "mods", "gui"])
        self.assertEqual(args.command, "gui")

    def test_windows_launcher_opens_the_gui(self):
        launcher = (ROOT / "Mods.bat").read_text(encoding="utf-8")
        self.assertIn("leonida_mods.py", launcher)
        self.assertIn(" gui ", launcher + " ")


if __name__ == "__main__":
    unittest.main()
