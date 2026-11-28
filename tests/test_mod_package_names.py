"""tools/leonida_mods.py: the file names a mod package may carry. Synthetic files only; no game data.

    python -I tests/test_mod_package_names.py -v
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

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import leonida_mods as mods

MANIFEST = {"name": "Names", "archetype_swaps": [{"from": "player_story000_00", "to": "cs_mandy"}]}

# Ordinary names the member check used to call unsafe, because it compared each one with the tidy folder label
# safe_folder_name() would have made of it: two spaces in a row, an ideographic space, more than 80 characters.
ORDINARY = ("read  me.txt", "\u8aac\u660e\u3000\u66f8.txt", "n" * 90 + ".txt",
            "notes/version  2.txt")


def cli(*argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = mods.main(list(argv))
    return code, out.getvalue(), err.getvalue()


def make_mod(mods_dir, relative_names):
    folder = Path(mods_dir, "sample")
    folder.mkdir(parents=True)
    (folder / "mod.json").write_text(json.dumps(MANIFEST), encoding="utf-8", newline="\n")
    for index, relative in enumerate(relative_names):
        path = folder.joinpath(*relative.split("/"))
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"synthetic %d\n" % index)
    return folder


def tree(folder):
    return {path.relative_to(folder).as_posix(): path.read_bytes()
            for path in sorted(Path(folder).rglob("*")) if path.is_file()}


class MemberNameTest(unittest.TestCase):
    def test_ordinary_names_are_members(self):
        for name in ORDINARY + (" leading space.txt", "trailing\u00a0space\u00a0.txt", "console.txt", "com10.txt"):
            with self.subTest(name=name):
                self.assertEqual(mods.zip_parts("sample/" + name), ("sample", *name.split("/")))

    def test_names_windows_or_the_package_cannot_hold_are_refused(self):
        for name in ("../escape.txt", "a/../b.txt", "/absolute.txt", "back\\slash.txt", "drive:stream.txt",
                     "star*.txt", "what?.txt", "pipe|.txt", "quote\".txt", "<angle>.txt", "tab\there.txt",
                     "trailing dot.", "trailing space ", "folder ./a.txt", "con.txt", "NUL", "aux.tar.gz",
                     "COM1 .log", "folder/lpt9", ""):
            with self.subTest(name=name), self.assertRaisesRegex(mods.ModsError, "unsafe ZIP member name"):
                mods.zip_parts(name)


class PackInstallNamesTest(unittest.TestCase):
    def test_what_pack_writes_install_accepts(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = make_mod(Path(tmp, "mods"), ORDINARY)
            packed = Path(tmp, "sample.leonidamod")
            code, out, err = cli("--dir", str(Path(tmp, "mods")), "pack", "sample", "--out", str(packed))
            self.assertEqual((code, err), (0, ""))

            other = Path(tmp, "other-mods")
            code, out, err = cli("--dir", str(other), "install", str(packed))
            self.assertEqual((code, err), (0, ""))
            self.assertIn("installed sample", out)
            self.assertEqual(tree(other / "sample"), tree(folder))

    def test_a_hand_made_zip_with_ordinary_names_installs(self):
        # The same names from a ZIP made outside this tool, mod.json at the top level.
        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp, "by hand.zip")
            with zipfile.ZipFile(package, "w") as archive:
                archive.writestr("mod.json", json.dumps(MANIFEST))
                for name in ORDINARY:
                    archive.writestr(name, "synthetic")
            mods_dir = Path(tmp, "mods")
            code, out, err = cli("--dir", str(mods_dir), "install", str(package))
            self.assertEqual((code, err), (0, ""))
            self.assertEqual(sorted(tree(mods_dir / "by hand")), sorted(("mod.json",) + ORDINARY))

    @unittest.skipIf(os.name == "nt", "Windows cannot create these names to pack them")
    def test_pack_refuses_a_name_install_would_refuse(self):
        for name in ("aux.txt", "drive:stream.txt", "what?.txt", "trailing dot.", "nested/back\\slash.txt"):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as tmp:
                make_mod(Path(tmp, "mods"), (name,))
                packed = Path(tmp, "sample.zip")
                packed.write_bytes(b"an earlier package")
                code, out, err = cli("--dir", str(Path(tmp, "mods")), "pack", "sample", "--out", str(packed),
                                     "--force")
                self.assertEqual((code, out), (2, ""))
                self.assertIn("has a name a package cannot carry", err)
                self.assertIn(name.rsplit("/", 1)[-1], err)
                self.assertEqual(packed.read_bytes(), b"an earlier package")


if __name__ == "__main__":
    unittest.main()
