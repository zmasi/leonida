"""Unit tests for tools/import_call_sites.py: thunks and the direct calls to them, on an image built here byte by byte.

The image has two executable-code layouts the tool must tell apart: calls (E8) and tail jumps (E9) to the thunk of an
import, and an import nothing calls. It needs no game files and no third-party packages.

    python -I tests/test_import_call_sites.py
"""
import contextlib
import io
import json
import os
import struct
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS_DIR = os.path.join(ROOT, "tools")
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

import import_call_sites  # noqa: E402
import pe_inspect  # noqa: E402

IMAGE_BASE = 0x140000000
TEXT_RVA = 0x1000
IDATA_RVA = 0x2000
DIRECTORIES = 0x58 + 112

# .text layout (RVAs): three thunks, then the callers.
THUNK_FOO = TEXT_RVA + 0x00
THUNK_BAR = TEXT_RVA + 0x10
THUNK_ORD = TEXT_RVA + 0x20
# .idata layout: descriptor table, lookup table, address table, hint/name entries, module name.
IAT_FOO = IDATA_RVA + 0x60
IAT_BAR = IAT_FOO + 8
IAT_ORD = IAT_FOO + 16
OTHER_IAT_RVA = IDATA_RVA + 0xE0

DELAY_DIRECTORY = DIRECTORIES + 13 * 8
DELAY_DESC = IDATA_RVA + 0x100
DELAY_IAT = IDATA_RVA + 0x140
DELAY_INT = IDATA_RVA + 0x160
DELAY_DLL = IDATA_RVA + 0x180
DELAY_NAME = IDATA_RVA + 0x1A0
DELAY_THUNK = TEXT_RVA + 0x30
DELAY_ORD_THUNK = TEXT_RVA + 0x40
DELAY_CALLS = [TEXT_RVA + 0x110, TEXT_RVA + 0x120]
DELAY_JMP = TEXT_RVA + 0x130
DELAY_ORD_CALL = TEXT_RVA + 0x140
NORMAL_CALL = TEXT_RVA + 0x100


def rel32(from_end, target):
    return struct.pack("<i", target - from_end)


def build_image(callers, dll=b"xmem.dll"):
    """A PE32+ with one import descriptor (`dll`: XMemFoo, XMemBar, #7) and the given `.text` callers."""
    buf = bytearray(0x3000)
    buf[0:2] = b"MZ"
    struct.pack_into("<I", buf, 0x3C, 0x40)
    buf[0x40:0x44] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", buf, 0x44, 0x8664, 2, 0, 0, 0, 0xF0, 0x22)
    opt = 0x58
    struct.pack_into("<H", buf, opt, 0x20B)
    struct.pack_into("<Q", buf, opt + 24, IMAGE_BASE)
    struct.pack_into("<I", buf, opt + 32, 0x1000)  # SectionAlignment
    struct.pack_into("<I", buf, opt + 36, 0x1000)  # FileAlignment
    struct.pack_into("<I", buf, opt + 56, 0x3000)  # SizeOfImage
    struct.pack_into("<I", buf, opt + 60, 0x1000)  # SizeOfHeaders
    struct.pack_into("<I", buf, opt + 108, 16)  # NumberOfRvaAndSizes
    struct.pack_into("<II", buf, DIRECTORIES + 8, IDATA_RVA, 40)  # import directory
    table = opt + 0xF0
    for i, (name, rva, chars) in enumerate(((b".text", TEXT_RVA, 0x60000020), (b".idata", IDATA_RVA, 0xC0000040))):
        at = table + i * 40
        buf[at:at + len(name)] = name
        struct.pack_into("<IIII", buf, at + 8, 0x1000, rva, 0x1000, rva)
        struct.pack_into("<I", buf, at + 36, chars)

    # .idata: one descriptor, its terminator, the lookup and address tables (same entries), names, the module name.
    lookup = IDATA_RVA + 0x40
    names = IDATA_RVA + 0xA0
    struct.pack_into("<IIIII", buf, IDATA_RVA, lookup, 0, 0, IDATA_RVA + 0xD0, IAT_FOO)
    entries = (names, names + 0x10, (1 << 63) | 7)
    for i, value in enumerate(entries):
        struct.pack_into("<Q", buf, lookup + i * 8, value)
        struct.pack_into("<Q", buf, IAT_FOO + i * 8, value)
    buf[names + 2:names + 2 + 8] = b"XMemFoo\0"
    buf[names + 0x10 + 2:names + 0x10 + 2 + 8] = b"XMemBar\0"
    assert len(dll) < 0x10  # the module name sits in 0xD0..0xDF, before OTHER_IAT_RVA
    buf[IDATA_RVA + 0xD0:IDATA_RVA + 0xD0 + len(dll) + 1] = dll + b"\0"

    # .text: the thunks are `jmp qword [rip+disp32]` to the address-table slots.
    for thunk, slot in ((THUNK_FOO, IAT_FOO), (THUNK_BAR, IAT_BAR), (THUNK_ORD, IAT_ORD)):
        buf[thunk:thunk + 2] = b"\xff\x25"
        buf[thunk + 2:thunk + 6] = rel32(thunk + 6, slot)
    for at, opcode, target in callers:
        buf[at] = opcode
        buf[at + 1:at + 5] = rel32(at + 5, target)
    return bytes(buf)


def build_delay_image(attributes=1, *, dll=b"delay.dll", name=b"LateFoo"):
    """Add two delay-import slots and their callers to the ordinary-import fixture."""
    data = bytearray(build_image([
        (NORMAL_CALL, 0xE8, THUNK_FOO),
        (DELAY_CALLS[0], 0xE8, DELAY_THUNK),
        (DELAY_CALLS[1], 0xE8, DELAY_THUNK),
        (DELAY_JMP, 0xE9, DELAY_THUNK),
        (DELAY_ORD_CALL, 0xE8, DELAY_ORD_THUNK),
    ]))
    # Descriptor fields are DWORDs. A legacy descriptor needs a low image base to hold its VAs.
    base = IMAGE_BASE if attributes & 1 else 0x10000000
    field_base = 0 if attributes & 1 else base
    struct.pack_into("<Q", data, 0x58 + 24, base)
    struct.pack_into("<II", data, DELAY_DIRECTORY, DELAY_DESC, 64)
    struct.pack_into("<8I", data, DELAY_DESC, attributes, field_base + DELAY_DLL, 0,
                     field_base + DELAY_IAT, field_base + DELAY_INT, 0, 0, 0)
    struct.pack_into("<3Q", data, DELAY_INT, field_base + DELAY_NAME, (1 << 63) | 42, 0)
    # A delay IAT holds addresses of resolver code before binding. Import names come from the INT.
    struct.pack_into("<3Q", data, DELAY_IAT, base + TEXT_RVA + 0x60, base + TEXT_RVA + 0x70, 0)
    data[DELAY_DLL:DELAY_DLL + len(dll) + 1] = dll + b"\0"
    data[DELAY_NAME + 2:DELAY_NAME + len(name) + 3] = name + b"\0"
    for thunk, slot in ((DELAY_THUNK, DELAY_IAT), (DELAY_ORD_THUNK, DELAY_IAT + 8)):
        data[thunk:thunk + 6] = b"\xff\x25" + rel32(thunk + 6, slot)
    return data


class ImportCallSitesTest(unittest.TestCase):
    def setUp(self):
        self.callers = [
            (TEXT_RVA + 0x100, 0xE8, THUNK_FOO),
            (TEXT_RVA + 0x110, 0xE8, THUNK_FOO),
            (TEXT_RVA + 0x120, 0xE9, THUNK_FOO),  # a tail jump
            (TEXT_RVA + 0x130, 0xE8, THUNK_ORD),
            (TEXT_RVA + 0x140, 0xE8, TEXT_RVA + 0x200),  # a call to something that is not a thunk
        ]
        self.data = build_image(self.callers)

    def test_counts_calls_and_jumps_per_import(self):
        out = import_call_sites.call_sites(self.data)["imports"]
        self.assertEqual(sorted(out), ["xmem.dll!#7", "xmem.dll!XMemBar", "xmem.dll!XMemFoo"])
        foo = out["xmem.dll!XMemFoo"]
        self.assertEqual(foo["thunks"], [THUNK_FOO])
        self.assertEqual(foo["call"], [TEXT_RVA + 0x100, TEXT_RVA + 0x110])
        self.assertEqual(foo["jmp"], [TEXT_RVA + 0x120])

    def test_an_import_nothing_calls_has_a_thunk_and_no_sites(self):
        bar = import_call_sites.call_sites(self.data)["imports"]["xmem.dll!XMemBar"]
        self.assertEqual((bar["thunks"], bar["call"], bar["jmp"]), ([THUNK_BAR], [], []))

    def test_an_ordinal_import_is_named_by_its_number(self):
        out = import_call_sites.call_sites(self.data, wanted={"xmem.dll!#7"})["imports"]
        self.assertEqual(list(out), ["xmem.dll!#7"])
        self.assertEqual(out["xmem.dll!#7"]["call"], [TEXT_RVA + 0x130])

    def test_wanted_names_and_dlls_select_imports(self):
        out = import_call_sites.call_sites(self.data, wanted={"XMEM.DLL!xmemfoo"})["imports"]
        self.assertEqual(list(out), ["xmem.dll!XMemFoo"])
        self.assertEqual(len(import_call_sites.call_sites(self.data, dlls={"XMem.dll"})["imports"]), 3)
        self.assertEqual(import_call_sites.call_sites(self.data, dlls={"other.dll"})["imports"], {})

    def test_a_dll_name_with_a_bang_is_selected_whole(self):
        # The dll and the function are kept apart, not re-split at the first '!' of "dll!name".
        data = build_image(self.callers, dll=b"x!em.dll")
        self.assertEqual(len(import_call_sites.call_sites(data, dlls={"x!em.dll"})["imports"]), 3)
        self.assertEqual(import_call_sites.call_sites(data, dlls={"x"})["imports"], {})
        out = import_call_sites.call_sites(data, wanted={"X!EM.DLL!xmemfoo"})["imports"]
        self.assertEqual(list(out), ["x!em.dll!XMemFoo"])
        self.assertEqual(out["x!em.dll!XMemFoo"]["call"], [TEXT_RVA + 0x100, TEXT_RVA + 0x110])

    def test_a_call_is_not_counted_for_a_different_thunk(self):
        data = build_image([(TEXT_RVA + 0x100, 0xE8, THUNK_BAR)])
        out = import_call_sites.call_sites(data)["imports"]
        self.assertEqual(out["xmem.dll!XMemFoo"]["call"], [])
        self.assertEqual(out["xmem.dll!XMemBar"]["call"], [TEXT_RVA + 0x100])

    def test_a_32_bit_image_is_refused(self):
        data = bytearray(self.data)
        struct.pack_into("<H", data, 0x58, 0x10B)
        with self.assertRaises(pe_inspect.PeFormatError):
            import_call_sites.call_sites(bytes(data))

    def test_cli_prints_counts_and_json_with_hex_rvas(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "image.exe")
            with open(path, "wb") as f:
                f.write(self.data)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(import_call_sites.main([path, "--import", "xmem.dll!XMemFoo"]), 0)
            self.assertIn("xmem.dll!XMemFoo", out.getvalue())
            self.assertIn("1 import(s), 3 call site(s)", out.getvalue())
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(import_call_sites.main([path, "--dll", "xmem.dll", "--json"]), 0)
            report = json.loads(out.getvalue())
            self.assertEqual(report["image_base"], hex(IMAGE_BASE))
            self.assertEqual(report["imports"]["xmem.dll!XMemFoo"]["call"], [hex(TEXT_RVA + 0x100), hex(TEXT_RVA + 0x110)])

    def test_cli_refuses_a_selector_that_selects_nothing(self):
        """--import is DLL!NAME and --dll is a DLL name. A bare DLL, a bare name or the two swapped select no import
        at all, and the run then printed "0 import(s), 0 call site(s)": a typo in half the name read as proof that
        the image never calls it (docs/xmem_exe_calls.md)."""
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "image.exe")
            with open(path, "wb") as f:
                f.write(self.data)
            for argv in (["--import", "xmem.dll"], ["--import", "XMemFoo"], ["--import", "xmem.dll!"],
                         ["--import", "!XMemFoo"], ["--dll", "xmem.dll!XMemFoo"], ["--dll", "!XMemFoo"]):
                err = io.StringIO()
                with self.assertRaises(SystemExit) as caught, contextlib.redirect_stderr(err):
                    import_call_sites.main([path, *argv])
                self.assertEqual(caught.exception.code, 2, argv)
                self.assertTrue(err.getvalue().strip(), argv)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):  # the two well-formed selectors still run
                self.assertEqual(import_call_sites.main([path, "--import", "xmem.dll!XMemFoo"]), 0)
                self.assertEqual(import_call_sites.main([path, "--dll", "xmem.dll"]), 0)

    def test_cli_refuses_a_well_formed_selector_this_image_does_not_have(self):
        """A misspelling inside a well-formed value names no import of this image, and the run printed
        "0 import(s), 0 call site(s)" for it: a proof of absence that was only a proof of a typo."""
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "image.exe")
            with open(path, "wb") as f:
                f.write(self.data)
            for argv in (["--import", "xmem.dll!XMemFooo"], ["--import", "xmem!XMemFoo"],
                         ["--dll", "xmem"], ["--dll", "other.dll"]):
                err = io.StringIO()
                with contextlib.redirect_stderr(err):
                    self.assertEqual(import_call_sites.main([path, *argv]), 2, argv)
                self.assertIn(argv[0], err.getvalue(), argv)
                self.assertIn(argv[1].lower(), err.getvalue().lower(), argv)
            # A matching name beside a misspelled one is still refused: no partial answer.
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(import_call_sites.main([path, "--import", "xmem.dll!XMemFoo",
                                                         "--import", "xmem.dll!XMemFooo"]), 2)

    def test_cli_reports_an_unreadable_or_non_pe_file_with_exit_2(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "not_a_pe.bin")
            with open(path, "wb") as f:
                f.write(b"not a pe")
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                self.assertEqual(import_call_sites.main([path]), 2)
            self.assertTrue(err.getvalue().strip())
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(import_call_sites.main([os.path.join(tmp, "missing.exe")]), 2)


class DelayImportCallSitesTest(unittest.TestCase):
    def _assert_sites(self, entry, *, thunks, call, jmp):
        self.assertEqual((entry["thunks"], entry["call"], entry["jmp"]), (thunks, call, jmp))

    def test_rva_and_va_descriptors_report_named_and_ordinal_sites(self):
        for attributes in (1, 0):
            with self.subTest(attributes=attributes):
                data = build_delay_image(attributes)
                out = import_call_sites.call_sites(data)["imports"]
                self._assert_sites(out["delay.dll!LateFoo"],
                                   thunks=[DELAY_THUNK], call=DELAY_CALLS, jmp=[DELAY_JMP])
                self._assert_sites(out["delay.dll!#42"],
                                   thunks=[DELAY_ORD_THUNK], call=[DELAY_ORD_CALL], jmp=[])
                self._assert_sites(out["xmem.dll!XMemFoo"],
                                   thunks=[THUNK_FOO], call=[NORMAL_CALL], jmp=[])

    def test_a_delay_only_image_can_filter_by_ordinal(self):
        data = build_delay_image()
        struct.pack_into("<II", data, DIRECTORIES + 8, 0, 0)
        self.assertEqual(set(import_call_sites.call_sites(data)["imports"]), {"delay.dll!LateFoo", "delay.dll!#42"})
        out = import_call_sites.call_sites(data, wanted={"DELAY.DLL!#42"})["imports"]
        self.assertEqual(list(out), ["delay.dll!#42"])
        self._assert_sites(out["delay.dll!#42"], thunks=[DELAY_ORD_THUNK], call=[DELAY_ORD_CALL], jmp=[])

    def test_same_name_in_both_tables_keeps_both_slots(self):
        data = build_delay_image(dll=b"xmem.dll", name=b"XMemFoo")
        out = import_call_sites.call_sites(data, wanted={"xmem.dll!XMemFoo"})["imports"]
        self.assertEqual(list(out), ["xmem.dll!XMemFoo"])
        self._assert_sites(out["xmem.dll!XMemFoo"],
                           thunks=[THUNK_FOO, DELAY_THUNK], call=[NORMAL_CALL, *DELAY_CALLS], jmp=[DELAY_JMP])

    def test_overlapping_delay_slots_keep_the_ordinary_import_names(self):
        data = build_delay_image()
        struct.pack_into("<I", data, DELAY_DESC + 12, IAT_FOO)
        out = import_call_sites.call_sites(data)["imports"]
        self.assertNotIn("delay.dll!LateFoo", out)
        self.assertNotIn("delay.dll!#42", out)
        self.assertEqual(out["xmem.dll!XMemFoo"]["call"], [NORMAL_CALL])

    def test_cli_counts_and_lists_delay_import_sites(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "image.exe")
            with open(path, "wb") as f:
                f.write(build_delay_image())
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(import_call_sites.main([path, "--dll", "DELAY.DLL"]), 0)
            self.assertRegex(out.getvalue(), r"delay\.dll!LateFoo +1 +2 +1")
            self.assertIn("2 import(s), 4 call site(s)", out.getvalue())
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(import_call_sites.main([path, "--dll", "delay.dll", "--json"]), 0)
            self.assertEqual(json.loads(out.getvalue())["imports"]["delay.dll!LateFoo"]["call"],
                             [hex(rva) for rva in DELAY_CALLS])

    def test_absent_empty_and_truncated_directories_preserve_ordinary_sites(self):
        for rva, size in ((0, 64), (DELAY_DESC, 0), (DELAY_DESC, 31),
                          (DELAY_DESC + 32, 32), (0x2FF0, 64), (0x3000, 0xFFFFFFFF)):
            with self.subTest(rva=rva, size=size):
                data = build_delay_image()
                struct.pack_into("<II", data, DELAY_DIRECTORY, rva, size)
                self.assertEqual(import_call_sites.call_sites(data),
                                 import_call_sites.call_sites(build_image([(NORMAL_CALL, 0xE8, THUNK_FOO)])))

        data = build_delay_image()[:DELAY_DESC + 31]
        self.assertEqual(import_call_sites.call_sites(data)["imports"]["xmem.dll!XMemFoo"]["call"], [NORMAL_CALL])
        self.assertEqual(import_call_sites.call_sites(data, dlls={"delay.dll"})["imports"], {})

    def test_bad_descriptor_addresses_are_skipped(self):
        for field, value in ((4, 0x3000), (12, 0), (12, 0x3000), (16, 0), (16, 0x3000)):
            with self.subTest(field=field, value=value):
                data = build_delay_image()
                struct.pack_into("<I", data, DELAY_DESC + field, value)
                self.assertEqual(import_call_sites.call_sites(data, dlls={"delay.dll"})["imports"], {})
        data = build_delay_image()
        struct.pack_into("<I", data, DELAY_DESC, 0)  # The DWORD fields cannot address this high image base.
        self.assertEqual(import_call_sites.call_sites(data, dlls={"delay.dll"})["imports"], {})
        # Legacy VA descriptors: underflowed pINT or a DLL-name VA equal to ImageBase (RVA 0 / DOS header).
        data = build_delay_image(attributes=0)
        struct.pack_into("<I", data, DELAY_DESC + 16, 0x0FFF0000)
        self.assertEqual(import_call_sites.call_sites(data, dlls={"delay.dll"})["imports"], {})
        data = build_delay_image(attributes=0)
        struct.pack_into("<I", data, DELAY_DESC + 4, 0x10000000)
        self.assertNotIn("MZ!LateFoo", import_call_sites.call_sites(data)["imports"])

    def test_malformed_descriptor_does_not_hide_a_later_valid_descriptor(self):
        data = build_delay_image()
        struct.pack_into("<I", data, DELAY_DIRECTORY + 4, 96)
        data[DELAY_DESC + 32:DELAY_DESC + 64] = data[DELAY_DESC:DELAY_DESC + 32]
        struct.pack_into("<I", data, DELAY_DESC + 4, 0x3000)
        self.assertEqual(import_call_sites.call_sites(data)["imports"]["delay.dll!LateFoo"]["call"], DELAY_CALLS)

    def test_bad_lookup_name_does_not_shift_the_following_slot(self):
        data = build_delay_image()
        struct.pack_into("<Q", data, DELAY_INT, 0x3000)
        out = import_call_sites.call_sites(data, dlls={"delay.dll"})["imports"]
        self.assertEqual(out["delay.dll!?"]["thunks"], [DELAY_THUNK])
        self.assertEqual(out["delay.dll!#42"]["call"], [DELAY_ORD_CALL])

    def test_truncated_lookup_and_address_tables_keep_only_whole_slots(self):
        for field in (12, 16):  # IAT, then INT
            with self.subTest(field=field):
                data = build_delay_image()
                table = len(data) - 12  # One whole slot and half of the next one.
                struct.pack_into("<I", data, DELAY_DESC + field, table)
                if field == 12:
                    for thunk, slot in ((DELAY_THUNK, table), (DELAY_ORD_THUNK, table + 8)):
                        data[thunk + 2:thunk + 6] = rel32(thunk + 6, slot)
                else:
                    struct.pack_into("<Q", data, table, DELAY_NAME)
                out = import_call_sites.call_sites(data, dlls={"delay.dll"})["imports"]
                self.assertEqual(list(out), ["delay.dll!LateFoo"])
                self._assert_sites(out["delay.dll!LateFoo"],
                                   thunks=[DELAY_THUNK], call=DELAY_CALLS, jmp=[DELAY_JMP])



class DirectSlotCallsTest(unittest.TestCase):
    def test_direct_call_slot_counted_without_thunk(self):
        # Image where XMemBar's thunk is wiped, but an instruction calls its IAT slot directly via FF 15.
        data = bytearray(build_image([]))
        data[THUNK_BAR:THUNK_BAR + 6] = b"\x90" * 6  # Nop out the thunk
        call_rva = TEXT_RVA + 0x150
        data[call_rva:call_rva + 6] = b"\xff\x15" + rel32(call_rva + 6, IAT_BAR)

        out = import_call_sites.call_sites(bytes(data))["imports"]
        bar = out["xmem.dll!XMemBar"]
        self.assertEqual(bar["thunks"], [])
        self.assertEqual(bar["call"], [call_rva])
        self.assertEqual(bar["jmp"], [])

    def test_direct_call_and_thunk_call_combined_and_sorted(self):
        # XMemFoo called both through its thunk (at 0x160) and directly via FF 15 (at 0x120).
        data = bytearray(build_image([(TEXT_RVA + 0x160, 0xE8, THUNK_FOO)]))
        direct_rva = TEXT_RVA + 0x120
        data[direct_rva:direct_rva + 6] = b"\xff\x15" + rel32(direct_rva + 6, IAT_FOO)

        out = import_call_sites.call_sites(bytes(data))["imports"]
        foo = out["xmem.dll!XMemFoo"]
        self.assertEqual(foo["thunks"], [THUNK_FOO])
        self.assertEqual(foo["call"], [direct_rva, TEXT_RVA + 0x160])
        self.assertEqual(foo["jmp"], [])

    def test_direct_call_to_non_slot_is_ignored(self):
        data = bytearray(build_image([]))
        call_rva = TEXT_RVA + 0x180
        data[call_rva:call_rva + 6] = b"\xff\x15" + rel32(call_rva + 6, IDATA_RVA + 0x500)

        out = import_call_sites.call_sites(bytes(data))["imports"]
        for entry in out.values():
            self.assertNotIn(call_rva, entry["call"])

    def test_direct_call_truncated_at_section_end(self):
        data = bytearray(build_image([]))
        # Place FF 15 in the last 4 bytes of .text so s + 6 > len(blob)
        cut = TEXT_RVA + 0x1000 - 4
        data[cut:cut + 2] = b"\xff\x15"

        out = import_call_sites.call_sites(bytes(data))["imports"]
        self.assertIn("xmem.dll!XMemFoo", out)

    def test_cli_output_includes_direct_slot_calls(self):
        data = bytearray(build_image([]))
        call_rva = TEXT_RVA + 0x150
        data[call_rva:call_rva + 6] = b"\xff\x15" + rel32(call_rva + 6, IAT_BAR)

        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "image.exe")
            with open(path, "wb") as f:
                f.write(data)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                rc = import_call_sites.main([path, "--dll", "xmem.dll", "--json"])
            self.assertEqual(rc, 0)
            parsed = json.loads(out.getvalue())
            self.assertEqual(parsed["imports"]["xmem.dll!XMemBar"]["call"], [hex(call_rva)])


if __name__ == "__main__":
    unittest.main()
