"""tools/victory_saves.py: VIctory's save layout converted to gta6rt's, on synthetic saves."""

import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
import victory_saves  # noqa: E402


def manifest(*names):
    return b"".join(struct.pack("<I", len(n.encode())) + n.encode() for n in names)


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


class VictorySavesTest(unittest.TestCase):
    def test_encode_name_matches_the_shim(self):
        self.assertEqual(victory_saves.encode_name("Save Game Data"), "Save%20Game%20Data")
        self.assertEqual(victory_saves.encode_name(".a.b"), "%2Ea.b")
        self.assertEqual(victory_saves.encode_name("A_z-9"), "A_z-9")
        self.assertEqual(victory_saves.encode_name("é"), "%C3%A9")

    def test_manifest_parse_refuses_truncation(self):
        self.assertEqual(victory_saves.parse_manifest(manifest("S", "D", "a")), ["S", "D", "a"])
        self.assertIsNone(victory_saves.parse_manifest(manifest("S", "D")[:-1]))
        self.assertIsNone(victory_saves.parse_manifest(manifest("S")))

    def test_newest_complete_version_is_converted(self):
        with tempfile.TemporaryDirectory() as tmp:
            title = os.path.join(tmp, "data", "7952987A")
            c = os.path.join(title, "saves", "0009", "scid", "Slot~abc")
            write(os.path.join(c, "v1", "manifest"), manifest("Slot", "Old", "Save Game Data"))
            write(os.path.join(c, "v1", "0"), b"old")
            write(os.path.join(c, "v2", "manifest"), manifest("Slot", "New", "Save Game Data", "Save Game Header"))
            write(os.path.join(c, "v2", "0"), b"data")
            write(os.path.join(c, "v2", "1"), b"head")
            # v3 is incomplete (a blob missing): the converter falls back to v2.
            write(os.path.join(c, "v3", "manifest"), manifest("Slot", "Broken", "Save Game Data"))
            write(os.path.join(title, "storage", "settings", "settings.xml"), b"<s/>")
            saves, overlay = os.path.join(tmp, "out"), os.path.join(tmp, "overlay")
            n = victory_saves.convert(title, saves, overlay, log=lambda *_: None)
            self.assertEqual(n, 1)
            out = os.path.join(saves, "local-user", "Slot")
            with open(os.path.join(out, "Save%20Game%20Data"), "rb") as f:
                self.assertEqual(f.read(), b"data")
            with open(os.path.join(out, "Save%20Game%20Header"), "rb") as f:
                self.assertEqual(f.read(), b"head")
            with open(os.path.join(out, ".display"), "rb") as f:
                self.assertEqual(f.read(), b"New")
            with open(os.path.join(overlay, ".gta6rt-persistent", "settings", "settings.xml"), "rb") as f:
                self.assertEqual(f.read(), b"<s/>")
            # A second run leaves what is there unless forced.
            self.assertEqual(victory_saves.convert(title, saves, overlay, log=lambda *_: None), 0)


if __name__ == "__main__":
    unittest.main()
