"""tools/d3d12_guid_sites.py and tools/d3d12_iid_scan.py report a bad header or an unreadable import table.

Hand-made inputs only: a few bytes stand in for a PE image, and the header is written by the test (no SDK, no game files).
"""
import os
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")

GOOD_HEADER = b'MIDL_INTERFACE("6102dee4-af59-4c3d-9c5b-7a1e4cb0d5b6")\nID3D12Fence\n'
BAD_HEADER = b'MIDL_INTERFACE("000000000000000000000000000000000000")\nID3D12Fence\n'


def pe_stub(bad):
    """A 0x300-byte PE32+ image with one d3d12_xs.dll import (ordinal 50), or, when bad, a second descriptor whose
    name is an unterminated string at the end of the file."""
    b = bytearray(0x300)
    b[0:2] = b"MZ"
    struct.pack_into("<I", b, 0x3C, 0x40)
    b[0x40:0x44] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", b, 0x44, 0x8664, 1, 0, 0, 0, 240, 0x22)
    struct.pack_into("<H", b, 0x58, 0x20B)
    struct.pack_into("<II", b, 0x58 + 112 + 8, 0x1000, 0x40)     # import directory: RVA, size
    b[0x148:0x148 + 8] = b".text\0\0\0"
    struct.pack_into("<IIII", b, 0x148 + 8, 0x200, 0x1000, 0x200, 0x200)  # VirtualSize, VA, SizeOfRawData, raw
    struct.pack_into("<IIIII", b, 0x200, 0x1040, 0, 0, 0x1030, 0x1040)   # descriptor 1: name RVA 0x1030
    b[0x230:0x23D] = b"d3d12_xs.dll\0"
    struct.pack_into("<Q", b, 0x240, (1 << 63) | 50)
    if bad:
        struct.pack_into("<IIIII", b, 0x214, 0x1040, 0, 0, 0x10F8, 0x1040)  # descriptor 2: name RVA 0x10F8
        b[0x2F8:0x300] = b"AAAAAAAA"                                       # no NUL before the file ends
    return bytes(b)


class ToolErrorReporting(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, name, data):
        path = os.path.join(self.tmp.name, name)
        with open(path, "wb") as f:
            f.write(data)
        return path

    def run_tool(self, tool, *args):
        return subprocess.run([sys.executable, "-I", os.path.join(TOOLS, tool + ".py"), *args],
                              capture_output=True, text=True, timeout=120, errors="replace")

    def test_missing_explicit_header_is_an_error_in_both_tools(self):
        image = self.write("tiny.exe", b"MZ")
        missing = os.path.join(self.tmp.name, "no_such_d3d12.h")
        for tool in ("d3d12_guid_sites", "d3d12_iid_scan"):
            with self.subTest(tool=tool):
                p = self.run_tool(tool, image, "--header", missing)
                self.assertEqual(p.returncode, 2, p.stderr)
                self.assertNotIn("Traceback", p.stderr)
                self.assertIn("ERROR: d3d12.h not found", p.stderr)
                self.assertEqual(p.stdout, "", "no GUID table may be printed for a missing header")

    def test_malformed_guid_in_header_is_an_error_in_both_tools(self):
        image = self.write("tiny.exe", b"MZ")
        header = self.write("bad.h", BAD_HEADER)
        for tool in ("d3d12_guid_sites", "d3d12_iid_scan"):
            with self.subTest(tool=tool):
                p = self.run_tool(tool, image, "--header", header)
                self.assertEqual(p.returncode, 2, p.stderr)
                self.assertNotIn("Traceback", p.stderr)
                self.assertTrue(p.stderr.startswith("ERROR: cannot read "), p.stderr)
                self.assertIn(header, p.stderr)
                self.assertIn("malformed", p.stderr)

    def test_unreadable_import_table_keeps_names_and_warns(self):
        header = self.write("ok.h", GOOD_HEADER)
        image = self.write("bad.exe", pe_stub(bad=True))
        p = self.run_tool("d3d12_iid_scan", image, "--header", header)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertNotIn("Traceback", p.stderr)
        self.assertIn("imports from d3d12_xs.dll: #50", p.stdout)
        self.assertIn("WARNING: bad.exe: the d3d12_xs.dll import table could not be read to the end", p.stderr)
        self.assertIn("no terminating NUL", p.stderr)

    def test_good_header_and_stub_behave_as_before(self):
        header = self.write("ok.h", GOOD_HEADER)
        image = self.write("good.exe", pe_stub(bad=False))
        p = self.run_tool("d3d12_iid_scan", image, "--header", header)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(p.stderr, "")
        self.assertIn("imports from d3d12_xs.dll: #50", p.stdout)
        self.assertIn("WARNING: these d3d12_xs.dll imports may be IID data", p.stdout)

        p = self.run_tool("d3d12_guid_sites", image, "--header", header)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("1 known GUIDs from", p.stdout)
        self.assertNotIn("Traceback", p.stderr)


if __name__ == "__main__":
    unittest.main()
