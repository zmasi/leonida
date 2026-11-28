"""tools/leonida_saves.py on fake save trees built here (no game data, nothing from a real save).

    python -I tests/test_leonida_saves.py -v
"""
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import leonida_saves as S  # noqa: E402


def make_container(root, name, blobs, version=1, display=None, user="0009000000000001", config="cfg"):
    """A container the way the runtime commits one: <Name>~<hash>/v<N>/{0,1,manifest}."""
    vdir = Path(root, user, config, S.container_dir_name(name), f"v{version}")
    vdir.mkdir(parents=True)
    for i, (_n, data) in enumerate(blobs):
        (vdir / str(i)).write_bytes(data)
    (vdir / "manifest").write_bytes(S.build_manifest(name, display or name, [n for n, _ in blobs]))
    return vdir.parent


def asav(kind, mode, body):
    head = bytearray(0x170)
    head[0:4] = b"ASAV"
    struct.pack_into("<7I", head, 4, 2, kind, mode, 0, 0x170, 0, 0x170 + len(body))
    struct.pack_into("<Q", head, 0x20, 1700000000)
    head[0x28:0x28 + 10] = "Hello".encode("utf-16-le")
    head[0x128:0x12C] = b"slot"
    return bytes(head) + body


class NamesTests(unittest.TestCase):
    def test_fnv_known_values(self):
        self.assertEqual(S.fnv1a64(""), 0xCBF29CE484222325)
        self.assertEqual(S.fnv1a64("a"), 0xAF63DC4C8601EC8C)
        self.assertEqual(S.fnv1a64("foobar"), 0x85944171F73967E8)

    def test_folder_names(self):
        self.assertEqual(S.container_dir_name("Slot_1"), f"Slot_1~{S.fnv1a64('Slot_1'):016x}")
        self.assertTrue(S.container_dir_name("a b/c").startswith("a%20b%2Fc~"))
        self.assertEqual(len(S.container_dir_name("x" * 300).split("~")[0]), 100)
        self.assertNotEqual(S.container_dir_name("Slot"), S.container_dir_name("slot"))

    def test_valid_names(self):
        for good in ("Player", "a/b/File.name-1", "SARC0015"):
            self.assertTrue(S.valid_container_name(good), good)
        for bad in ("", "a b", "a..b", "x.", "a-b/c", "x" * 257, "é"):
            self.assertFalse(S.valid_container_name(bad), bad)

    def test_manifest_round_trip(self):
        data = S.build_manifest("N", "Disp", ["Save Game Data", "Save Game Header"])
        self.assertEqual(S.parse_manifest(data), ["N", "Disp", "Save Game Data", "Save Game Header"])
        self.assertIsNone(S.parse_manifest(data[:-1]))
        self.assertIsNone(S.parse_manifest(b"\x01\x00\x00\x00x"))


class TreeTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.data = self.tmp.name
        self.root = S.saves_root(self.data)
        self.backups = S.backups_dir(self.data)
        make_container(self.root, "Player", [("Save Game Data", b"one"), ("Save Game Header", b"hdr")])
        make_container(self.root, "Slot_2", [("Save Game Data", b"old")], version=1)
        make_container(self.root, "Slot_2", [("Save Game Data", b"newer!")], version=3)

    def by_name(self, name):
        return next(c for c in S.scan(self.root) if c.name == name)

    def test_scan_shows_newest_version(self):
        found = {c.name: c for c in S.scan(self.root)}
        self.assertEqual(set(found), {"Player", "Slot_2"})
        slot = found["Slot_2"]
        self.assertEqual((slot.version, slot.highest, slot.blobs[0].size), (3, 3, 6))
        self.assertEqual([b.name for b in found["Player"].blobs], ["Save Game Data", "Save Game Header"])
        self.assertEqual(S.scan(os.path.join(self.data, "nothing")), [])

    def test_unreadable_newest_falls_back(self):
        slot = self.by_name("Slot_2")
        bad = Path(slot.path, "v4")
        bad.mkdir()
        (bad / "manifest").write_bytes(b"\x09\x00\x00\x00x")
        again = self.by_name("Slot_2")
        self.assertEqual((again.version, again.highest), (3, 4))

    def test_backup_and_restore(self):
        path = S.backup(self.root, self.backups)
        with zipfile.ZipFile(path) as z:
            self.assertTrue(all(n.count("/") >= 4 for n in z.namelist()))
        player = self.by_name("Player")
        Path(player.blobs[0].path).write_bytes(b"changed")
        S.delete(self.by_name("Slot_2"), self.backups)
        before = S.restore(self.root, self.backups, path)
        self.assertTrue(before and os.path.isfile(before))  # the state before the restore was saved
        self.assertEqual(Path(self.by_name("Player").blobs[0].path).read_bytes(), b"one")
        self.assertEqual(self.by_name("Slot_2").version, 3)
        self.assertFalse(os.path.exists(os.path.join(os.path.dirname(self.root), "restore.new")))

    def test_restore_refuses_other_zips(self):
        evil = os.path.join(self.data, "evil.zip")
        with zipfile.ZipFile(evil, "w") as z:
            z.writestr("../../x/y/z/w", b"1")
        with self.assertRaises(S.SaveError):
            S.restore(self.root, self.backups, evil)
        empty = os.path.join(self.data, "empty.zip")
        zipfile.ZipFile(empty, "w").close()
        with self.assertRaises(S.SaveError):
            S.zip_containers(empty)
        junk = os.path.join(self.data, "junk.zip")
        Path(junk).write_bytes(b"not a zip")
        with self.assertRaises(S.SaveError):
            S.zip_containers(junk)

    def test_duplicate_gets_its_own_hash_and_manifest(self):
        player = self.by_name("Player")
        target = S.duplicate(self.root, player, "Copy_1")
        self.assertEqual(os.path.basename(target), S.container_dir_name("Copy_1"))
        copy = self.by_name("Copy_1")
        self.assertEqual((copy.display, copy.version), ("Copy_1", 1))
        self.assertEqual(Path(copy.blobs[0].path).read_bytes(), b"one")
        self.assertEqual(self.by_name("Player").name, "Player")
        with self.assertRaises(S.SaveError):
            S.duplicate(self.root, player, "Copy_1")
        with self.assertRaises(S.SaveError):
            S.duplicate(self.root, player, "bad name")
        self.assertEqual([p for p in os.listdir(os.path.dirname(target)) if p.endswith(".new")], [])

    def test_delete_moves_instead_of_erasing(self):
        slot = self.by_name("Slot_2")
        moved = S.delete(slot, self.backups)
        self.assertFalse(os.path.exists(slot.path))
        self.assertTrue(os.path.isfile(os.path.join(moved, "v3", "manifest")))
        self.assertEqual([c.name for c in S.scan(self.root)], ["Player"])

    def test_export_and_import_write_a_new_version_after_a_backup(self):
        slot = self.by_name("Slot_2")
        out = os.path.join(self.data, "blob.bin")
        S.export_blob(slot.blobs[0], out)
        self.assertEqual(Path(out).read_bytes(), b"newer!")
        Path(out).write_bytes(b"edited")
        number, saved = S.import_blob(self.root, self.backups, slot, slot.blobs[0], out)
        self.assertEqual(number, 4)
        self.assertTrue(os.path.isfile(saved))
        now = self.by_name("Slot_2")
        self.assertEqual((now.version, Path(now.blobs[0].path).read_bytes()), (4, b"edited"))
        self.assertEqual(Path(slot.path, "v3", "0").read_bytes(), b"newer!")  # the old copy stays
        self.assertEqual([p for p in os.listdir(slot.path) if p.endswith(".new")], [])
        empty = os.path.join(self.data, "empty.bin")
        Path(empty).write_bytes(b"")
        with self.assertRaises(S.SaveError):
            S.import_blob(self.root, self.backups, now, now.blobs[0], empty)

    def test_backup_names_do_not_collide(self):
        a, b = S.backup(self.root, self.backups), S.backup(self.root, self.backups)
        self.assertNotEqual(a, b)
        self.assertEqual(len(S.list_backups(self.backups)), 2)


class BytesTests(unittest.TestCase):
    def test_entropy(self):
        self.assertEqual(S.entropy(b""), 0.0)
        self.assertEqual(S.entropy(b"aaaa"), 0.0)
        self.assertAlmostEqual(S.entropy(bytes(range(256)) * 4), 8.0)

    def test_describe_plain_and_packed(self):
        plain = S.describe(asav(2, 1, b'{"a":1}\r\n' * 40))
        self.assertEqual((plain["kind"], plain["packed"], plain["editable"]), ("asav", False, False))
        self.assertEqual((plain["title"], plain["slot"], plain["asav_kind"]), ("Hello", "slot", 2))
        packed = S.describe(asav(1, 3, bytes((i * 167 + i // 7) % 256 for i in range(8192))))
        self.assertTrue(packed["packed"])
        self.assertIn("Editing is not supported", S.explain(packed))
        self.assertIn("checksum", S.explain(plain))
        self.assertEqual(S.describe(b"hello world\n")["kind"], "text")
        self.assertEqual(S.describe(b"")["kind"], "unknown")

    def test_viewer_helpers(self):
        dump = S.hexdump(b"ABC\x00" * 10)
        self.assertTrue(dump.startswith("00000000  41 42 43 00"))
        self.assertIn("ABC.ABC.", dump)
        self.assertIn("more bytes", S.hexdump(bytes(100), limit=32))
        found = S.strings(b"\x00\x01hello world\x01\x01" + "wide text".encode("utf-16-le"))
        self.assertIn("hello world", found)
        self.assertIn("wide text", found)


if __name__ == "__main__":
    unittest.main()
