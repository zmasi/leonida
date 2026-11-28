"""Unit tests for tools/cpu_feature_scan.py on hand-written dumpbin-style listings and a small synthetic PE.

The listings are written here line by line and the PE carries only headers and a .pdata table, so the test needs
no game files, no dumpbin and no third-party packages.

    python -I tests/test_cpu_feature_scan.py
"""
import io
import json
import os
import struct
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS_DIR = os.path.join(ROOT, "tools")
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

import cpu_feature_scan as scan  # noqa: E402

BASE = 0x140000000


def listing(*rows):
    """Rows are (rva, 'mnemonic operands'); written as `dumpbin /disasm:nobytes` prints them."""
    out = ["Dump of file synthetic.exe\n", "\n", "File Type: EXECUTABLE IMAGE\n", "\n"]
    for rva, text in rows:
        mnem, _, ops = text.partition(" ")
        out.append(f"  {BASE + rva:016X}: {mnem:<11} {ops}".rstrip() + "\n")
    return out


def pe_with_pdata(ranges, image_base=BASE, size_override=None):
    """An x64 PE whose only content is a .pdata section holding `ranges` as RUNTIME_FUNCTION entries."""
    pdata = b"".join(struct.pack("<III", b, e, 0) for b, e in ranges)
    raw = pdata + b"\0" * (-len(pdata) % 0x200 or 0x200)
    buf = bytearray(0x200 + len(raw))
    buf[0:2] = b"MZ"
    struct.pack_into("<I", buf, 0x3C, 0x40)
    buf[0x40:0x44] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", buf, 0x44, 0x8664, 1, 0, 0, 0, 240, 0x22)
    opt = 0x58
    struct.pack_into("<H", buf, opt, 0x20B)
    struct.pack_into("<Q", buf, opt + 24, image_base)
    struct.pack_into("<II", buf, opt + 56, 0x1000 + len(raw), 0x200)
    struct.pack_into("<H", buf, opt + 68, 3)
    struct.pack_into("<I", buf, opt + 108, 16)
    struct.pack_into("<II", buf, opt + 112 + 3 * 8, 0x1000, len(pdata) if size_override is None else size_override)
    sec = opt + 240
    buf[sec:sec + 8] = b".pdata\0\0"
    struct.pack_into("<IIII", buf, sec + 8, len(raw), 0x1000, len(raw), 0x200)
    struct.pack_into("<I", buf, sec + 36, 0x40000040)
    buf[0x200:0x200 + len(raw)] = raw
    return bytes(buf)


class ClassifyTest(unittest.TestCase):
    def check(self, text, feature):
        mnem, _, ops = text.partition(" ")
        self.assertEqual(scan.classify(mnem, ops), feature, text)

    def test_amd_only(self):
        self.check("insertq xmm0,xmm1,10h,8", "SSE4A")
        self.check("extrq xmm2,8,4", "SSE4A")
        self.check("movntsd qword ptr [rax],xmm0", "SSE4A")
        self.check("clzero", "CLZERO")
        self.check("monitorx rax,rcx,rdx", "MONITORX")
        self.check("mwaitx", "MONITORX")
        self.check("rdpru", "RDPRU")
        self.check("vpperm xmm0,xmm1,xmm2,xmm3", "XOP")
        self.check("vfmaddps xmm0,xmm1,xmm2,xmm3", "FMA4")
        self.check("blcfill eax,ecx", "TBM")
        for feature in ("SSE4A", "CLZERO", "MONITORX", "RDPRU", "XOP", "FMA4", "TBM", "3DNOW"):
            self.assertEqual(scan.FEATURES[feature][0], "amd-only")

    def test_newer_intel(self):
        self.check("clwb byte ptr [rcx]", "CLWB")
        self.check("sha256rnds2 xmm1,xmm2,xmm0", "SHA")
        self.check("rdpid rax", "RDPID")

    def test_fma3_is_baseline_and_fma4_is_not(self):
        self.check("vfmadd213ps ymm0,ymm1,ymm2", "FMA")
        self.check("vfnmsub231sd xmm0,xmm1,xmm2", "FMA")
        self.check("vfmaddsub231ps ymm0,ymm1,ymm2", "FMA")
        self.check("vfmaddsubps ymm0,ymm1,ymm2,ymm3", "FMA4")

    def test_width_decides_vaes_and_vpclmulqdq(self):
        self.check("aesenc xmm0,xmm1", "AES")
        self.check("vaesenc xmm0,xmm1,xmm2", "AES")
        self.check("vaesenc ymm0,ymm1,ymm2", "VAES")
        self.check("vpclmulqdq xmm0,xmm1,xmm2,0", "PCLMULQDQ")
        self.check("vpclmulqdq ymm0,ymm1,ymm2,11h", "VPCLMULQDQ")

    def test_avx_levels(self):
        self.check("vaddps ymm0,ymm1,ymm2", "AVX")
        self.check("vmovups xmmword ptr [rax],xmm0", "AVX")
        self.check("vzeroupper", "AVX")
        self.check("vpaddd ymm0,ymm1,ymm2", "AVX2")
        self.check("vpaddd xmm0,xmm1,xmm2", "AVX")
        self.check("vpbroadcastd xmm0,xmm1", "AVX2")
        self.check("vaddps zmm0,zmm1,zmm2", "AVX512")
        self.check("vaddps ymm0{k1},ymm1,ymm2", "AVX512")
        self.check("kmovw k1,eax", "AVX512")

    def test_avx512vl_high_vector_registers_are_flagged(self):
        # EVEX xmm16..xmm31 / ymm16..ymm31 require AVX-512VL and #UD on Zen 2.
        for text in (
            "vaddps ymm16,ymm17,ymm18",
            "vpaddd ymm31,ymm0,ymm1",
            "vmovaps xmm16,xmm0",
            "vaesenc ymm20,ymm0,ymm1",
        ):
            self.check(text, "AVX512")
        # Registers 0..15 stay baseline AVX/AVX2.
        self.check("vaddps ymm15,ymm14,ymm13", "AVX")
        self.check("vpaddd ymm15,ymm14,ymm13", "AVX2")
        self.check("vmovaps xmm15,xmm0", "AVX")

    def test_privileged_and_kernel_calls(self):
        self.check("in al,dx", "PORT-IO")
        self.check("out dx,al", "PORT-IO")
        self.check("hlt", "SYSTEM")
        self.check("cli", "SYSTEM")
        self.check("rdmsr", "SYSTEM")
        self.check("mov rax,cr0", "SYSTEM")
        self.check("mov dr7,rax", "SYSTEM")
        self.check("xsetbv", "XSETBV")
        self.check("mwait", "MONITOR")
        self.check("syscall", "SYSCALL")
        self.check("int 2Eh", "INT")
        self.check("int 0B9h", "INT")

    def test_windows_served_interrupts_are_not_flagged(self):
        for vector in ("3", "29h", "2Ch", "2Dh", "2ch", "2dh", "29H", "0x3", "0x29", "0x2c", "0X2D"):
            self.check(f"int {vector}", None)
        for vector in ("2eh", "0x2e", "0x29h"):
            self.check(f"int {vector}", "INT")

    def test_host_queries(self):
        self.check("cpuid", "CPUID")
        self.check("xgetbv", "XGETBV")
        self.check("rdtscp", "RDTSC")

    def test_prefixes_are_skipped(self):
        self.check("lock cmpxchg16b xmmword ptr [r8]", "CX16")
        self.check("xacquire lock cmpxchg16b xmmword ptr [r8]", "CX16")
        self.check("lock add dword ptr [rax],1", None)
        self.check("rep stos byte ptr [rdi]", None)

    def test_prefixes_with_extra_whitespace_and_mixed_case(self):
        self.assertEqual(scan.classify("lock", "  cmpxchg16b xmmword ptr [r8]"), "CX16")
        self.assertEqual(scan.classify("lock", "\tcmpxchg16b xmmword ptr [r8]"), "CX16")
        self.assertEqual(scan.classify("xacquire", "  lock\t  cmpxchg16b xmmword ptr [r8]"), "CX16")
        self.assertEqual(scan.classify("rep", "  insb"), "PORT-IO")
        self.assertEqual(scan.classify("rep", "\tinsb"), "PORT-IO")
        self.assertEqual(scan.classify("LOCK", "CMPXCHG16B XMMWORD PTR [R8]"), "CX16")
        self.assertEqual(scan.classify("MOV", "RAX,CR0"), "SYSTEM")
        self.assertEqual(scan.classify("VADDPS", "YMM0,YMM1,YMM2"), "AVX")
        self.assertEqual(scan.classify("VMOVUPS", "XMMWORD PTR [RAX],XMM0"), "AVX")
        self.assertEqual(scan.classify("VADDPS", "ZMM0,ZMM1,ZMM2"), "AVX512")
        self.assertEqual(scan.classify("VADDPS", "YMM0{K1},YMM1,YMM2"), "AVX512")

    def test_plain_code_needs_nothing(self):
        for text in ("mov rax,qword ptr [rcx+18h]", "add rsp,28h", "ret", "movaps xmm0,xmmword ptr [rcx]",
                     "pshufb xmm0,xmm1", "mov rax,rcx", "lea rax,[rcx+8]", "nop"):
            self.check(text, None)

    def test_every_mnemonic_names_a_known_feature(self):
        for mnem, feature in scan.MNEMONICS.items():
            self.assertIn(feature, scan.FEATURES, mnem)
        for feature, (cls, _note) in scan.FEATURES.items():
            self.assertIn(cls, scan.CLASSES, feature)


class FunctionsTest(unittest.TestCase):
    def test_ranges_merge_and_contain(self):
        f = scan.Functions([(0x2000, 0x2010), (0x1000, 0x1100), (0x1100, 0x1180), (0x3000, 0x3000)])
        self.assertEqual(f.count, 4)
        self.assertTrue(f.contains(0x1000))
        self.assertTrue(f.contains(0x117F))
        self.assertFalse(f.contains(0x1180))
        self.assertFalse(f.contains(0xFFF))
        self.assertTrue(f.contains(0x200F))
        self.assertFalse(f.contains(0x3000))  # a zero-length entry covers nothing

    def test_from_image_reads_pdata(self):
        img = scan.PeImage(pe_with_pdata([(0x1000, 0x1040), (0x1040, 0x1100)]))
        f = scan.Functions.from_image(img)
        self.assertEqual(f.count, 2)
        self.assertTrue(f.contains(0x10FF))
        self.assertFalse(f.contains(0x1100))

    def test_partial_trailing_entry_is_ignored(self):
        img = scan.PeImage(pe_with_pdata([(0x1000, 0x1040), (0x2000, 0x2040)], size_override=20))
        self.assertEqual(scan.Functions.from_image(img).count, 1)

    def test_image_without_pdata(self):
        img = scan.PeImage(pe_with_pdata([], size_override=0))
        self.assertEqual(scan.Functions.from_image(img).count, 0)


ROWS = (
    (0x1000, "push rbx"),
    (0x1001, "insertq xmm0,xmm1,10h,8"),
    (0x1006, "vfmadd213ps ymm0,ymm1,ymm2"),
    (0x100B, "cpuid"),
    (0x100D, "int 3"),
    (0x2000, "clzero"),        # a leaf routine with no .pdata entry
    (0x2003, "in al,dx"),       # data decoded as code
    (0x2004, "ret"),
)


class ScanTest(unittest.TestCase):
    def test_hits_are_split_by_pdata(self):
        stats = {}
        functions = scan.Functions([(0x1000, 0x1010)])
        report = scan.summarize(scan.scan(listing(*ROWS), functions, BASE, stats), stats, functions)
        self.assertEqual(report["instructions"], 8)
        self.assertEqual(report["instructions_in_functions"], 5)
        feats = report["features"]
        self.assertEqual(list(feats), ["CLZERO", "SSE4A", "PORT-IO", "CPUID", "FMA"])
        self.assertEqual((feats["SSE4A"]["in_functions"], feats["SSE4A"]["elsewhere"]), (1, 0))
        self.assertEqual((feats["CLZERO"]["in_functions"], feats["CLZERO"]["elsewhere"]), (0, 1))
        self.assertEqual(feats["CLZERO"]["sites"], [{"rva": 0x2000, "in_function": False}])
        self.assertEqual(feats["CPUID"]["sites"], [])  # host queries are counted, not listed
        self.assertNotIn("INT", feats)

    def test_base_is_inferred_without_an_image(self):
        stats = {}
        hits = list(scan.scan(listing(*ROWS), None, None, stats))
        self.assertIn(("SSE4A", 0x1001, None), hits)


class CliTest(unittest.TestCase):
    def run_main(self, *argv):
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            code = scan.main(list(argv))
        return code, out.getvalue(), err.getvalue()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.listing = os.path.join(self.tmp.name, "listing.txt")
        with open(self.listing, "w", encoding="utf-8", newline="\n") as f:
            f.writelines(listing(*ROWS))
        self.image = os.path.join(self.tmp.name, "image.bin")
        with open(self.image, "wb") as f:
            f.write(pe_with_pdata([(0x1000, 0x1010)]))

    def test_text_report_with_image(self):
        code, out, _ = self.run_main(self.listing, "--image", self.image, "--sites")
        self.assertEqual(code, 0)
        self.assertIn("instructions scanned: 8", out)
        self.assertIn(".pdata entries: 1, instructions inside them: 5", out)
        self.assertIn("flagged features: 3 (CLZERO, SSE4A, PORT-IO)", out)
        self.assertIn("  SSE4A +0x1001 in .pdata", out)
        self.assertIn("  CLZERO +0x2000 outside .pdata", out)

    def test_json_report_without_image(self):
        code, out, _ = self.run_main(self.listing, "--json")
        self.assertEqual(code, 0)
        report = json.loads(out)
        self.assertIsNone(report["pdata_entries"])
        self.assertEqual(report["features"]["SSE4A"]["total"], 1)
        self.assertEqual(report["features"]["SSE4A"]["sites"], [{"rva": 0x1001, "in_function": None}])

    def test_listing_without_instructions_exits_2(self):
        empty = os.path.join(self.tmp.name, "empty.txt")
        with open(empty, "w", encoding="utf-8", newline="\n") as f:
            f.write("not a listing\n")
        code, out, err = self.run_main(empty)
        self.assertEqual(code, 2)
        self.assertEqual(out, "")
        self.assertIn("no instructions found", err)

    def test_missing_input_exits_2(self):
        code, _, err = self.run_main(os.path.join(self.tmp.name, "nope.txt"))
        self.assertEqual(code, 2)
        self.assertIn("not found", err)

    def test_bad_image_exits_2(self):
        bad = os.path.join(self.tmp.name, "bad.bin")
        with open(bad, "wb") as f:
            f.write(b"MZ" + b"\0" * 10)
        code, _, err = self.run_main(self.listing, "--image", bad)
        self.assertEqual(code, 2)
        self.assertIn("bad.bin", err)

    def test_negative_and_invalid_base_rejected(self):
        for bad_base in ("-0x1000", "not-hex"):
            with self.subTest(base=bad_base):
                out, err = io.StringIO(), io.StringIO()
                with redirect_stdout(out), redirect_stderr(err):
                    with self.assertRaises(SystemExit) as cm:
                        scan.main([self.listing, "--base", bad_base])
                self.assertEqual(cm.exception.code, 2)
                self.assertNotIn("<lambda>", err.getvalue())

    def test_unreadable_source_exits_2_without_traceback(self):
        with mock.patch.object(scan.inline_scan, "read_lines", side_effect=PermissionError("denied")):
            code, out, err = self.run_main(self.listing)
        self.assertEqual(code, 2)
        self.assertEqual(out, "")
        self.assertIn("denied", err)
        self.assertNotIn("Traceback", err)


if __name__ == "__main__":
    unittest.main()
