"""tools/find_hash_lookup.py ranks the function a model hash is passed to (standard library only, no game data).

    python -I tests/test_find_hash_lookup.py

The images are synthetic PE files: one executable section holding `mov ecx, <hash>` followed by calls, with a
"lookup" called from many places and a decoy called once.
"""
import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOL = os.path.join(ROOT, "tools", "find_hash_lookup.py")
sys.path.insert(0, os.path.join(ROOT, "tools"))

import find_hash_lookup as fhl  # noqa: E402

TEXT_RVA, TEXT_RAW = 0x1000, 0x400


def pe(code, vsize=None, rsize=None, raw_offset=None):
    """A minimal PE32+ file whose only section, .text (executable), holds `code`.

    The optional fields let a test describe a truncated section (raw offset past EOF, zero vsize) while the file
    itself stays well-formed enough to parse its headers.
    """
    header = bytearray(TEXT_RAW)
    header[0:2] = b"MZ"
    struct.pack_into("<I", header, 0x3C, 0x40)
    header[0x40:0x44] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", header, 0x44, 0x8664, 1, 0, 0, 0, 0xF0, 0x22)
    first = 0x44 + 20 + 0xF0
    struct.pack_into("<8sIIIIIIHHI", header, first, b".text",
                     len(code) if vsize is None else vsize, TEXT_RVA,
                     len(code) if rsize is None else rsize,
                     TEXT_RAW if raw_offset is None else raw_offset,
                     0, 0, 0, 0, 0x60000020)
    return bytes(header) + code


def call(at, target):
    """E8 rel32 placed at offset `at` of .text, calling .text offset `target`."""
    return b"\xE8" + struct.pack("<i", target - (at + 5))


def image():
    lucia, mandy = fhl.joaat("player_story000_00"), fhl.joaat("cs_mandy")
    code = bytearray(b"\xCC" * 0x400)
    lookup, decoy = 0x300, 0x380
    code[lookup] = code[decoy] = 0xC3
    # mov ecx, lucia ; call lookup   (at 0x00)
    code[0x00:0x05] = b"\xB9" + struct.pack("<I", lucia)
    code[0x05:0x0A] = call(0x05, lookup)
    # mov ecx, mandy ; nop ; call lookup   (at 0x20)
    code[0x20:0x25] = b"\xB9" + struct.pack("<I", mandy)
    code[0x25] = 0x90
    code[0x26:0x2B] = call(0x26, lookup)
    # mov ecx, lucia ; call decoy   (at 0x40): led by one hash, called once
    code[0x40:0x45] = b"\xB9" + struct.pack("<I", lucia)
    code[0x45:0x4A] = call(0x45, decoy)
    for k in range(20):                          # other callers of the lookup
        at = 0x100 + 8 * k
        code[at:at + 5] = call(at, lookup)
    return pe(bytes(code)), TEXT_RVA + lookup, TEXT_RVA + decoy


class FindHashLookupTest(unittest.TestCase):
    def test_joaat_matches_the_measured_model_ids(self):
        self.assertEqual(fhl.joaat("player_story000_00"), 0xE472B687)
        self.assertEqual(fhl.joaat("CS_Mandy"), 0xD7C0CDFF)
        self.assertEqual(fhl.parse_hash("0xD7C0CDFF"), 0xD7C0CDFF)
        with self.assertRaises(ValueError):
            fhl.parse_hash("0x123456789")

    def test_the_lookup_ranks_first(self):
        data, lookup, decoy = image()
        report = fhl.rank(data, [("player_story000_00", fhl.joaat("player_story000_00")),
                                 ("cs_mandy", fhl.joaat("cs_mandy"))])
        top = report["candidates"][0]
        self.assertEqual(int(top["rva"], 16), lookup)
        self.assertEqual(top["led_by"], ["cs_mandy", "player_story000_00"])
        self.assertEqual(top["direct_callers"], 22)
        self.assertEqual(int(report["candidates"][1]["rva"], 16), decoy)
        self.assertEqual({h["name"]: h["occurrences"] for h in report["hashes"]},
                         {"player_story000_00": 2, "cs_mandy": 1})

    def test_an_encrypted_file_is_refused_clearly(self):
        with self.assertRaisesRegex(ValueError, "encrypted"):
            fhl.rank(b"\x6f\x1b\xbc\xae" * 64, [("x", 1)])

    def test_cli_writes_the_report_and_fails_cleanly(self):
        data, lookup, _ = image()
        with tempfile.TemporaryDirectory() as tmp:
            exe = os.path.join(tmp, "image.exe")
            with open(exe, "wb") as f:
                f.write(data)
            out = os.path.join(tmp, "report.json")
            r = subprocess.run([sys.executable, "-I", TOOL, exe, "--json", out], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn(f"0x{lookup:X}", r.stdout)
            with open(out, encoding="utf-8") as f:
                self.assertEqual(int(json.load(f)["candidates"][0]["rva"], 16), lookup)
            bad = os.path.join(tmp, "packed.exe")
            with open(bad, "wb") as f:
                f.write(b"\x00" * 64)
            r = subprocess.run([sys.executable, "-I", TOOL, bad], capture_output=True, text=True)
            self.assertEqual(r.returncode, 2)
            self.assertIn("ERROR", r.stderr)

    def test_json_writes_nothing_over_the_image_and_never_crashes(self):
        """--json naming the scanned image must be refused before it truncates the image (the game executable),
        and an unwritable path is one error line, not a traceback."""
        data, _, _ = image()
        with tempfile.TemporaryDirectory() as tmp:
            exe = os.path.join(tmp, "image.exe")
            with open(exe, "wb") as f:
                f.write(data)
            r = subprocess.run([sys.executable, "-I", TOOL, exe, "--json", exe], capture_output=True, text=True)
            self.assertEqual(r.returncode, 2, r.stderr)
            self.assertIn("would overwrite", r.stderr)
            self.assertNotIn("Traceback", r.stderr)
            with open(exe, "rb") as f:
                self.assertEqual(f.read(), data, "the scanned image must be untouched")
            r = subprocess.run([sys.executable, "-I", TOOL, exe, "--json", tmp], capture_output=True, text=True)
            self.assertEqual(r.returncode, 2, r.stderr)
            self.assertIn("is a folder", r.stderr)
            self.assertNotIn("Traceback", r.stderr)
            missing = os.path.join(tmp, "no-such-folder", "report.json")
            r = subprocess.run([sys.executable, "-I", TOOL, exe, "--json", missing], capture_output=True, text=True)
            self.assertEqual(r.returncode, 2, r.stderr)
            self.assertIn("ERROR: cannot write", r.stderr)
            self.assertNotIn("Traceback", r.stderr)

    def test_zero_call_target_produces_no_candidates(self):
        """A hash that appears in code but has no E8 in the following window yields an empty candidates list."""
        # Build a PE whose .text is just `mov ecx, <hash>; ret; <NOP padding>` -- no E8 anywhere.
        value = fhl.joaat("player_story000_00")
        code = b"\xB9" + struct.pack("<I", value) + b"\xC3" + b"\x90" * 59
        data = pe(code)
        report = fhl.rank(data, [("player_story000_00", value)])
        self.assertEqual(report["hashes"][0]["occurrences"], 1)
        self.assertEqual(report["candidates"], [],
                         "no E8 in window → candidates must be empty")

    def test_zero_window_produces_no_candidates(self):
        """window=0 means the search tail is empty; no candidates regardless of calls nearby."""
        data, _lookup, _decoy = image()
        hashes = [("player_story000_00", fhl.joaat("player_story000_00")),
                  ("cs_mandy", fhl.joaat("cs_mandy"))]
        report = fhl.rank(data, hashes, window=0)
        self.assertEqual(report["candidates"], [],
                         "window=0 must yield no candidates")

    def test_negative_window_raises(self):
        """rank() with window < 0 must raise ValueError naming its own parameter, not just the CLI flag."""
        data, _, _ = image()
        with self.assertRaises(ValueError) as raised:
            fhl.rank(data, [("player_story000_00", fhl.joaat("player_story000_00"))], window=-1)
        self.assertRegex(str(raised.exception), r"^window must be >= 0, got -1$")

    def test_non_positive_top_raises(self):
        """rank() with top <= 0 must raise ValueError naming its own parameter; top=-1 returns a wrong slice."""
        data, _, _ = image()
        hashes = [("player_story000_00", fhl.joaat("player_story000_00"))]
        with self.assertRaises(ValueError) as raised:
            fhl.rank(data, hashes, top=0)
        self.assertRegex(str(raised.exception), r"^top must be >= 1, got 0$")
        with self.assertRaises(ValueError) as raised:
            fhl.rank(data, hashes, top=-1)
        self.assertRegex(str(raised.exception), r"^top must be >= 1, got -1$")

    def test_truncated_pe_does_not_crash(self):
        """A truncated PE's section size is clamped to the bytes actually present in the file."""
        data, _, _ = image()
        truncated = data[:-100]                      # .text claims 0x400 bytes, only 0x39C remain
        sections = fhl.sections(truncated)
        self.assertEqual(len(sections), 1)
        _name, _rva, raw, size, _executable = sections[0]
        self.assertEqual(raw, TEXT_RAW)
        self.assertEqual(size, len(truncated) - raw)
        self.assertLessEqual(size, len(truncated) - raw)
        report = fhl.rank(truncated, [("player_story000_00", fhl.joaat("player_story000_00"))])
        self.assertIsInstance(report["candidates"], list)

    def test_section_raw_pointer_past_eof_clamps_to_zero(self):
        """A section whose raw pointer is beyond EOF must get size 0, never a negative size or a scan past EOF."""
        data = pe(b"\x90" * 16, raw_offset=0x10000)
        sections = fhl.sections(data)
        self.assertEqual(len(sections), 1)
        self.assertEqual(sections[0][2], 0x10000)
        self.assertEqual(sections[0][3], 0)
        self.assertLessEqual(sections[0][3], max(0, len(data) - sections[0][2]))

    def test_section_zero_vsize_uses_raw_size(self):
        """vsize == 0 means 'use the raw size', clamped by what the file actually holds."""
        data = pe(b"\x90" * 16, vsize=0, rsize=8)
        sections = fhl.sections(data)
        self.assertEqual(len(sections), 1)
        self.assertEqual(sections[0][3], 8)
        shorter = pe(b"\x90" * 4, vsize=0, rsize=8)
        self.assertEqual(fhl.sections(shorter)[0][3], 4)

    def test_cli_negative_window_exits_with_error(self):
        """--window -1 on the CLI must exit non-zero with an ERROR line, no traceback."""
        data, _, _ = image()
        with tempfile.TemporaryDirectory() as tmp:
            exe = os.path.join(tmp, "image.exe")
            with open(exe, "wb") as f:
                f.write(data)
            r = subprocess.run(
                [sys.executable, "-I", TOOL, exe, "--window", "-1"],
                capture_output=True, text=True,
            )
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("ERROR", r.stderr)
            self.assertIn("--window", r.stderr)      # CLI keeps the flag name, rank() uses its parameter name
            self.assertNotIn("Traceback", r.stderr)

    def test_cli_top_zero_exits_with_error(self):
        """--top 0 on the CLI must exit non-zero with an ERROR line, no traceback."""
        data, _, _ = image()
        with tempfile.TemporaryDirectory() as tmp:
            exe = os.path.join(tmp, "image.exe")
            with open(exe, "wb") as f:
                f.write(data)
            r = subprocess.run(
                [sys.executable, "-I", TOOL, exe, "--top", "0"],
                capture_output=True, text=True,
            )
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("ERROR", r.stderr)
            self.assertIn("--top", r.stderr)         # CLI keeps the flag name, rank() uses its parameter name
            self.assertNotIn("Traceback", r.stderr)


if __name__ == "__main__":
    unittest.main()
