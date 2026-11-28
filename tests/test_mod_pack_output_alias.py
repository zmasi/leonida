"""Synthetic pack-output alias checks; no runtime or external data required."""
import contextlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import leonida_mods as mods


class PackAliasTest(unittest.TestCase):
    def make_mod(self, root):
        folder = root / "mods" / "sample"
        folder.mkdir(parents=True)
        (folder / "mod.json").write_text(json.dumps({"archetype_swaps": []}), encoding="utf-8", newline="\n")
        (folder / "nested").mkdir()
        (folder / "nested" / "notes.txt").write_bytes(b"synthetic payload\n")
        return folder

    def test_alias_preflight_precedes_output_truncation(self):
        # Exercise the OS alias answer deterministically even where the sandbox
        # forbids creating hard links. Keep the real report-path comparison.
        for relative in ("mod.json", "nested/notes.txt"):
            with self.subTest(relative=relative), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                folder = self.make_mod(root)
                source = folder / relative
                before = source.read_bytes()
                output = root / "packed.zip"
                output.write_bytes(before)
                # same_report_path hands samefile the names realpath gave it, which are not the ones tempfile
                # spelled where the temporary folder is reached through a link (macOS: /var is /private/var).
                def resolved(path):
                    return os.path.normcase(os.path.realpath(path))
                aliases = {resolved(source), resolved(output)}
                def samefile(a, b):
                    return {resolved(a), resolved(b)} == aliases
                with mock.patch("report_paths.os.path.samefile", side_effect=samefile):
                    with self.assertRaisesRegex(mods.ModsError, "aliases a file"):
                        mods.pack_mod(str(root / "mods"), "sample", str(output), True)
                self.assertEqual(output.read_bytes(), before)
                self.assertEqual(source.read_bytes(), before)

    def test_force_refuses_hardlink_to_manifest_or_nested_file(self):
        for relative in ("mod.json", "nested/notes.txt"):
            with self.subTest(relative=relative), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                folder = self.make_mod(root)
                source = folder / relative
                before = source.read_bytes()
                output = root / "packed.zip"
                try:
                    os.link(source, output)
                except OSError as exc:
                    self.skipTest(f"hard links unavailable: {exc}")
                err = io.StringIO()
                with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
                    code = mods.main(["--dir", str(root / "mods"), "pack", "sample", "--out", str(output), "--force"])
                self.assertEqual(code, 2)
                self.assertIn("aliases a file", err.getvalue())
                self.assertEqual(source.read_bytes(), before)
                self.assertEqual(output.read_bytes(), before)

    def test_force_replaces_unrelated_output_with_complete_archive(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            folder = self.make_mod(root)
            output = root / "packed.zip"
            output.write_bytes(b"old output")
            mods.pack_mod(str(root / "mods"), "sample", str(output), True)
            with zipfile.ZipFile(output) as archive:
                self.assertEqual(archive.namelist(), ["sample/mod.json", "sample/nested/notes.txt"])
                for relative in ("mod.json", "nested/notes.txt"):
                    self.assertEqual(archive.read("sample/" + relative), (folder / relative).read_bytes())


if __name__ == "__main__":
    unittest.main()
