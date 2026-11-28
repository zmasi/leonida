"""tools/native_pso_hashes.py on hand-built DXBC containers and indexes: no game data."""
import contextlib
import io
import json
import os
import struct
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import native_pso_hashes as tool  # noqa: E402
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import make_game  # noqa: E402


def container(digest: bytes, other=b"") -> bytes:
    """A DXBC container with a HASH part (flags 0, then the digest) and an optional second part."""
    parts = [b"HASH" + struct.pack("<I", 20) + struct.pack("<I", 0) + digest]
    if other:
        parts.insert(0, b"PRIV" + struct.pack("<I", len(other)) + other)
    head = 32 + 4 * len(parts)
    offsets, at = [], head
    for p in parts:
        offsets.append(at)
        at += len(p)
    body = b"DXBC" + bytes(16) + struct.pack("<II", 1, at) + struct.pack("<I", len(parts))
    return body + b"".join(struct.pack("<I", o) for o in offsets) + b"".join(parts)


class NativePsoHashesTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dxbc = os.path.join(self.tmp.name, "shaders", "dxbc")
        self.spirv = os.path.join(self.tmp.name, "shaders", "spirv")
        os.makedirs(self.dxbc)
        os.makedirs(self.spirv)

    def tearDown(self):
        self.tmp.cleanup()

    def put(self, name, data):
        with open(os.path.join(self.dxbc, name + ".dxbc"), "wb") as f:
            f.write(data)

    def index(self, entries):
        with open(os.path.join(self.spirv, "index.json"), "w", encoding="utf-8", newline="\n") as f:
            json.dump(entries, f)

    def read(self):
        with open(os.path.join(self.spirv, tool.OUT_NAME), encoding="utf-8") as f:
            return f.read().splitlines()

    def test_stage_builds_missing_catalog_and_refreshes_changed_translation(self):
        d = bytes(range(16))
        self.put("one", container(d))
        self.index({"one": {"stage": "vs", "fssm_sha256": "a" * 64}})
        with mock.patch.object(tool, "build", wraps=tool.build) as build:
            make_game.native_pso_hashes(self.dxbc, self.spirv)
            self.assertEqual(build.call_count, 1)
            self.assertIn(d.hex() + "\tvs\t../dxbc/one.dxbc", self.read())
            make_game.native_pso_hashes(self.dxbc, self.spirv)
            self.assertEqual(build.call_count, 1)
            self.index({})
            old = os.path.join(self.spirv, tool.OUT_NAME)
            os.utime(old, (1, 1))
            make_game.native_pso_hashes(self.dxbc, self.spirv)
            self.assertEqual(build.call_count, 2)
            self.assertEqual(self.read(), [tool.HEADER])

    def test_translation_index_with_utf8_bom_is_accepted(self):
        digest = bytes(range(16))
        self.put("one", container(digest))
        with open(os.path.join(self.spirv, "index.json"), "w", encoding="utf-8-sig", newline="\n") as f:
            json.dump({"one": {"stage": "vs", "fssm_sha256": "a" * 64}}, f)
        self.assertEqual(tool.build(self.dxbc, self.spirv),
                         {digest.hex(): ("vs", "../dxbc/one.dxbc")})

    def test_replacing_one_container_refreshes_the_digest(self):
        self.index({"one": {"stage": "ps", "fssm_sha256": "a" * 64}})
        self.put("one", container(bytes(16)))
        make_game.native_pso_hashes(self.dxbc, self.spirv)
        stamp = os.stat(os.path.join(self.spirv, tool.OUT_NAME)).st_mtime_ns
        changed = bytes([5]) * 16
        self.put("one", container(changed))
        os.utime(os.path.join(self.dxbc, "one.dxbc"), ns=(stamp, stamp + 1000000000))
        make_game.native_pso_hashes(self.dxbc, self.spirv)
        self.assertEqual(self.read()[1], changed.hex() + "\tps\t../dxbc/one.dxbc")

    def test_malformed_translator_identity(self):
        self.put("one", container(bytes(16)))
        for stage, fssm in (("ps\tvs", "a" * 64), ("vs", "short")):
            with self.subTest(stage=stage, fssm=fssm):
                self.index({"one": {"stage": stage, "fssm_sha256": fssm}})
                with self.assertRaisesRegex(ValueError, "invalid translated stage or FSSM hash"):
                    tool.build(self.dxbc, self.spirv)

    def test_digest_of_a_container(self):
        d = bytes(range(16))
        self.assertEqual(tool.hash_digest(container(d)), d)
        self.assertEqual(tool.hash_digest(container(d, b"x" * 9)), d)
        self.assertIsNone(tool.hash_digest(b"DXBC" + bytes(28)))
        self.assertIsNone(tool.hash_digest(container(d)[:-1]))
        self.assertIsNone(tool.hash_digest(b"FSSM" + bytes(60)))

    def test_map_lines(self):
        a, b, c = bytes([1] * 16), bytes([2] * 16), bytes([3] * 16)
        self.put("vs one", container(a))
        self.put(".ms_dot", container(b))
        self.put("same_a", container(a, b"other"))   # another container with digest a and the same program
        self.put("untranslated", container(c))
        self.put("not_dxbc", b"nothing")
        self.index({"vs one": {"stage": "vs", "fssm_sha256": "1" * 64}, ".ms_dot": {"stage": "ms", "fssm_sha256": "2" * 64},
                    "same_a": {"stage": "vs", "fssm_sha256": "1" * 64}, "not_dxbc": {"stage": "ps", "fssm_sha256": "3" * 64}})
        self.assertEqual(tool.main(["x", self.dxbc, self.spirv]), 0)
        lines = self.read()
        self.assertEqual(lines[0], tool.HEADER)
        self.assertEqual(lines[1:], [f"{a.hex()}\tvs\t../dxbc/same_a.dxbc", f"{b.hex()}\tms\t../dxbc/.ms_dot.dxbc"])
        with open(os.path.join(self.spirv, tool.OUT_NAME), "rb") as f:
            self.assertNotIn(b"\r", f.read())

    def test_a_digest_with_two_programs_does_not_publish_a_partial_map(self):
        a = bytes([7] * 16)
        self.put("one", container(a))
        self.put("two", container(a, b"x"))
        self.index({"one": {"stage": "ps", "fssm_sha256": "1" * 64}, "two": {"stage": "ps", "fssm_sha256": "2" * 64}})
        with open(os.path.join(self.spirv, tool.OUT_NAME), "w", encoding="utf-8", newline="\n") as f:
            f.write("stale\n")
        with self.assertRaises(tool.DigestClash):
            tool.build(self.dxbc, self.spirv)
        self.assertEqual(tool.main(["x", self.dxbc, self.spirv]), 1)
        self.assertEqual(self.read(), ["stale"])

    def test_no_index(self):
        self.assertEqual(tool.main(["x", self.dxbc, self.spirv]), 1)
        self.assertEqual(tool.main(["x"]), 2)

    def test_help_exits_zero(self):
        for flag in ("-h", "--help"):
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(tool.main(["x", flag]), 0)
            self.assertIn("native_hashes.txt", out.getvalue())

    def test_a_shared_digest_requires_the_same_stage(self):
        shared = container(bytes([9]) * 16)
        self.put("one", shared)
        self.put("two", shared)
        self.index({name: {"stage": stage, "fssm_sha256": "1" * 64}
                    for name, stage in (("one", "vs"), ("two", "ps"))})
        with self.assertRaises(tool.DigestClash):
            tool.build(self.dxbc, self.spirv)


if __name__ == "__main__":
    unittest.main()
