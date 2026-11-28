"""Offline crash analysis using a hand-built x64 PE and minidump. No target runs."""
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

SCRIPTS = Path(__file__).resolve().parents[1] / "scripts" / "windows-boot"
sys.path.insert(0, str(SCRIPTS))
import mapped_stack

BASE = 0x140000000
RELOCATED = 0x251000000
PC_RVA = 0x104F
CALLER_RVA = 0x1110
SP = 0x300000


def pe_image():
    image = bytearray(0x800)
    image[:2] = b"MZ"
    struct.pack_into("<I", image, 0x3C, 0x80)
    struct.pack_into("<4sHHIIIHH", image, 0x80, b"PE\0\0", 0x8664, 3, 12345, 0, 0, 240, 0x22)
    opt = 0x98
    struct.pack_into("<H", image, opt, 0x20B)
    struct.pack_into("<IIQII", image, opt + 16, 0x1000, 0x1000, BASE, 0x1000, 0x200)
    struct.pack_into("<I", image, opt + 48, 6)
    struct.pack_into("<II", image, opt + 56, 0x4000, 0x200)
    struct.pack_into("<H", image, opt + 68, 3)
    struct.pack_into("<I", image, opt + 108, 16)
    struct.pack_into("<II", image, opt + 112 + 3 * 8, 0x2000, 12)
    for index, (name, flags) in enumerate([(b".text", 0x60000020), (b".pdata", 0x40000040),
                                            (b".xdata", 0x40000040)]):
        struct.pack_into("<8sIIII12xI", image, opt + 240 + 40 * index, name, 0x200, 0x1000 * (index + 1),
                         0x200, 0x200 * (index + 1), flags)
    # A non-leaf function reserves 40 bytes; its caller is a leaf. The body
    # uses different immediates so the dump carries a unique instruction window.
    image[0x200:0x204] = bytes.fromhex("48 83 ec 28")
    for i in range(40):
        struct.pack_into("<BI", image, 0x204 + i * 5, 0xB8, i)  # mov eax, i
    image[0x2CC:0x2D1] = bytes.fromhex("48 83 c4 28 c3")
    image[0x310] = 0xC3
    struct.pack_into("<III", image, 0x400, 0x1000, 0x10D1, 0x3000)
    image[0x600:0x608] = bytes((1, 4, 1, 0, 4, 0x42, 0, 0))  # UWOP_ALLOC_SMALL, 40
    return image


def minidump(image, base=BASE, *, memory64=False, exception=True, registered=False, headers=b"",
             module_name=None, capture_code=True):
    # Real MINIDUMP_HEADER / stream layouts. Only the fault window and thread
    # stack are captured. In particular, CDB must read .pdata from the disk PE.
    kinds = [7, 4, 3, 9 if memory64 else 5] + ([6] if exception else [])
    data = bytearray(32 + len(kinds) * 12)
    struct.pack_into("<4sIII", data, 0, b"MDMP", 0xA793, len(kinds), 32)

    def append(blob):
        data.extend(bytes(-len(data) % 4))
        offset = len(data)
        data.extend(blob)
        return offset

    def stream(kind, blob):
        struct.pack_into("<III", data, 32 + kinds.index(kind) * 12, kind, len(blob), append(blob))

    system = bytearray(56)
    struct.pack_into("<HHHBBIIII", system, 0, 9, 6, 0, 1, 1, 10, 0, 22621, 2)
    stream(7, system)
    modules = bytearray(4 + (108 if registered or module_name else 0))
    if registered or module_name:
        name = (module_name or "fixture.exe").encode("utf-16-le")
        name_rva = append(struct.pack("<I", len(name)) + name + bytes(2))
        struct.pack_into("<IQI", modules, 0, 1, base if registered else 0x7FF200000000, 0x4000)
        struct.pack_into("<I", modules, 24, name_rva)
    stream(4, modules)
    ctx = bytearray(1232)
    struct.pack_into("<I", ctx, 48, 0x10000B)
    struct.pack_into("<H", ctx, 56, 0x33)
    struct.pack_into("<Q", ctx, 152, SP)
    struct.pack_into("<Q", ctx, 248, base + PC_RVA)
    context = append(ctx)
    stack = bytearray(0x100)
    struct.pack_into("<Q", stack, 40, base + CALLER_RVA)
    stack_rva = append(stack)
    thread = bytearray(52)
    struct.pack_into("<II", thread, 0, 1, 0x51)
    struct.pack_into("<QIIII", thread, 28, SP, len(stack), stack_rva, len(ctx), context)
    stream(3, thread)
    code = image[0x220:0x2A0]
    code_rva = append(code)
    if memory64:
        stream(9, struct.pack("<QQQQ", 1, code_rva, base + 0x1020, len(code)))
    else:
        ranges = [(base + 0x1020, len(code), code_rva), (SP, len(stack), stack_rva)]
        if not capture_code:
            ranges = ranges[1:]
        if headers:
            ranges.append((base, len(headers), append(headers)))
        stream(5, struct.pack("<I", len(ranges)) + b"".join(struct.pack("<QII", *item) for item in ranges))
    if exception:
        exc = bytearray(168)
        struct.pack_into("<I", exc, 0, 0x51)
        struct.pack_into("<I", exc, 8, 0xC0000005)
        struct.pack_into("<Q", exc, 24, base + PC_RVA)
        struct.pack_into("<II", exc, 160, len(ctx), context)
        stream(6, exc)
    return data


class MappedStackTests(unittest.TestCase):
    def setUp(self):
        self.image = pe_image()

    def test_discovers_actual_base_from_each_memory_stream(self):
        for base, memory64 in [(BASE, False), (RELOCATED, True)]:
            with self.subTest(base=base):
                match = mapped_stack.identify(minidump(self.image, base, memory64=memory64), self.image)
                self.assertEqual((match.base, match.rip, match.thread), (base, base + PC_RVA, 0x51))

    def test_mismatch_no_exception_and_registered_image(self):
        dump = minidump(self.image)
        other = self.image.copy()
        other[0x250] ^= 0xFF
        for bad_dump, image in [(dump, other), (minidump(self.image, exception=False), self.image),
                                (minidump(self.image, registered=True), self.image), (dump[:100], self.image),
                                (minidump(self.image, capture_code=False), self.image)]:
            with self.subTest(), self.assertRaises(ValueError):
                mapped_stack.identify(bad_dump, image)

    def test_remote_module_path_and_debugger_alias_collision(self):
        for name in (r"\\host\volume\fixture.dll", "leonida_mapped.dll"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                mapped_stack.identify(minidump(self.image, module_name=name), self.image)

    def test_missing_or_repeated_instruction_bytes(self):
        for fill in (bytes(128), self.image[0x220:0x2A0]):
            image = self.image.copy()
            image[0x320:0x3A0] = fill
            if not any(fill):
                image[0x220:0x2A0] = fill
            with self.subTest(), self.assertRaises(ValueError):
                mapped_stack.identify(minidump(image), image)

    def test_captured_headers_corroborate_the_image(self):
        dump = minidump(self.image, headers=self.image[:0x200])
        self.assertEqual(mapped_stack.identify(dump, self.image).base, BASE)
        struct.pack_into("<I", self.image, 0x88, 12346)  # same code, different PE timestamp
        with self.assertRaisesRegex(ValueError, "metadata differs"):
            mapped_stack.identify(dump, self.image)

    def test_missing_and_unbacked_unwind_data(self):
        dump = minidump(self.image)
        for offset, value in [(0x400, 0x1090), (0x408, 0x4000), (0x98 + 140, 15)]:
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                image = self.image.copy()
                struct.pack_into("<I", image, offset, value)
                mapped_stack.identify(dump, image)

    def test_incomplete_wrong_context_and_speculative_debugger_output(self):
        match = mapped_stack.identify(minidump(self.image), self.image)
        context = f"{match.rip:x} {match.rsp:x}"
        first = "00 00000000`00300000 00000001`40001110 leonida_mapped+0x104f"
        second = "01 00000000`00300030 00000000`00000000 leonida_mapped+0x1110"
        for lines in [[context, first], ["0 0", first, second], [context, first.replace("leonida_mapped", "other"), second],
                      [context, first, "WARNING: Following frames may be wrong.", second]]:
            with self.subTest(lines=lines), self.assertRaises(ValueError):
                mapped_stack.stack_rows("\n".join([mapped_stack.BEGIN, *lines, mapped_stack.END]), match)

    @unittest.skipUnless(os.name == "nt" and (os.getenv("LEONIDA_TEST_CDB") or shutil.which("cdb")),
                         "set LEONIDA_TEST_CDB to the x64 Windows debugger to run the native test")
    def test_native_cdb_recovers_caller_across_unregistered_nonleaf(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            image = folder / "private ; q ; name.exe"
            dump = folder / "fault.dmp"
            report = folder / "mapped-stack.txt"
            image.write_bytes(self.image)
            dump.write_bytes(minidump(self.image, RELOCATED, module_name="fixture.exe"))
            args = [sys.executable, "-I", str(SCRIPTS / "mapped_stack.py"), str(dump), str(image),
                    "--cdb", os.getenv("LEONIDA_TEST_CDB") or shutil.which("cdb")]
            run = subprocess.run(args, capture_output=True, text=True, timeout=90)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertEqual(run.stdout.strip(), "Recovered 2 frames. Keep the stack report private.")
            frames = report.read_text(encoding="utf-8")
            self.assertIn("leonida_mapped+0x104f", frames)
            self.assertIn("leonida_mapped+0x1110", frames)
            self.assertNotIn(image.name, frames)
            # A second attempt must leave the original private report intact.
            again = subprocess.run(args, capture_output=True, text=True, timeout=90)
            self.assertNotEqual(again.returncode, 0)
            self.assertEqual(report.read_text(encoding="utf-8"), frames)


if __name__ == "__main__":
    unittest.main()
