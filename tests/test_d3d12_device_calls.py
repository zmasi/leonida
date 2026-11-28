"""Unit tests for tools/d3d12_device_calls.py on an x86-64 image assembled here byte by byte.

The image imports D3D12XboxCreateDevice from d3d12_xs.dll and has ten .pdata functions: two creator calls (through
the import thunk with the out-pointer a global, and `call [rip+IAT]` with it a stack slot), calls through the device
global and a copy of it (`call [vtable+disp]`, `call reg`, a CFG-style dispatch with the method in rax and the
device in rdx, a guard-check stub that ends in `jmp rax`, a tail jump), a method in rax handed to a helper that only
returns (not a slot: the review of old upstream PR 7998), an unrelated object's vtable call, the device passed to
another function, and an indexed vtable call no slot can be read from. No game files.

    python -I tests/test_d3d12_device_calls.py
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

import d3d12_device_calls as ddc  # noqa: E402
import pe_inspect  # noqa: E402

IMAGE_BASE = 0x140000000
TEXT_RVA = 0x1000
IDATA_RVA = 0x2000
DATA_RVA = 0x3000
PDATA_RVA = 0x4000
SIZE = 0x5000

IAT = IDATA_RVA + 0x60          # D3D12XboxCreateDevice's address-table slot
DEVICE = DATA_RVA + 0x00        # the creator's out-pointer
COPY = DATA_RVA + 0x08          # a global the device is copied to
OTHER = DATA_RVA + 0x10         # an unrelated object
GUARD = DATA_RVA + 0x18         # the dispatch routine's pointer (to a `jmp rax` stub)
EXTRA = TEXT_RVA + 0xC00        # free .text space for the code a test adds


class Asm:
    """Appends instructions at known RVAs; `rip` and `rel` encode a RIP-relative or rel32 operand."""

    def __init__(self, rva):
        self.rva = rva
        self.code = bytearray()

    @property
    def here(self):
        return self.rva + len(self.code)

    def raw(self, b):
        self.code += b

    def rip(self, prefix, target):
        end = self.here + len(prefix) + 4
        self.code += prefix + struct.pack("<i", target - end)

    def rel(self, opcode, target):
        self.rip(opcode, target)


def build_image():
    """(image bytes, {function name: rva})."""
    a = Asm(TEXT_RVA)
    funcs = {}

    def begin(name):
        while len(a.code) % 16:
            a.raw(b"\xcc")
        funcs[name] = [a.here, None]

    def end(name):
        funcs[name][1] = a.here

    thunk = a.here
    a.rip(b"\xff\x25", IAT)                              # jmp [rip+IAT]
    funcs_extra = {}
    while len(a.code) % 16:
        a.raw(b"\xcc")
    funcs_extra["jmp_rax"] = a.here                      # a CFG no-op dispatch routine
    a.raw(b"\xff\xe0")                                   # jmp rax
    while len(a.code) % 16:
        a.raw(b"\xcc")
    funcs_extra["guard_check"] = a.here                  # a guard check, then the dispatch
    a.raw(b"\x49\x89\xc3")                               # mov r11,rax
    a.raw(b"\x49\xc1\xeb\x09")                           # shr r11,9
    a.raw(b"\xa8\x0f")                                   # test al,0xf
    a.raw(b"\x75\x02")                                   # jne +2
    a.raw(b"\xff\xe0")                                   # jmp rax
    while len(a.code) % 16:
        a.raw(b"\xcc")
    funcs_extra["ret_helper"] = a.here                   # an ordinary helper that only returns
    a.raw(b"\xc3")

    begin("create")
    a.rip(b"\x4c\x8d\x0d", DEVICE)                       # lea r9,[rip+DEVICE]
    a.rel(b"\xe8", thunk)                                # call thunk
    a.raw(b"\xc3")
    end("create")

    begin("copy")                                        # slot 14 through call [rax+disp8]
    a.rip(b"\x48\x8b\x0d", DEVICE)                       # mov rcx,[rip+DEVICE]
    a.rip(b"\x48\x89\x0d", COPY)                         # mov [rip+COPY],rcx
    a.raw(b"\x48\x8b\x01")                               # mov rax,[rcx]
    a.raw(b"\x31\xd2")                                   # xor edx,edx
    a.raw(b"\x41\xb8\x01\x00\x00\x00")                   # mov r8d,1
    a.raw(b"\xff\x50\x70")                               # call [rax+0x70]
    a.raw(b"\xc3")
    end("copy")

    begin("method")                                      # slot 48 through the copy, call rax
    a.rip(b"\x48\x8b\x1d", COPY)                         # mov rbx,[rip+COPY]
    a.raw(b"\x48\x8b\x03")                               # mov rax,[rbx]
    a.raw(b"\x48\x8b\x80\x80\x01\x00\x00")               # mov rax,[rax+0x180]
    a.raw(b"\x48\x89\xd9")                               # mov rcx,rbx
    a.raw(b"\xff\xd0")                                   # call rax
    a.raw(b"\xc3")
    end("method")

    begin("dispatch")                                    # slot 26, device in rdx, dispatch routine
    a.rip(b"\x48\x8b\x15", DEVICE)                       # mov rdx,[rip+DEVICE]
    a.raw(b"\x48\x8d\x4c\x24\x20")                       # lea rcx,[rsp+0x20]
    a.raw(b"\x48\x8b\x02")                               # mov rax,[rdx]
    a.raw(b"\x48\x8b\x80\xd0\x00\x00\x00")               # mov rax,[rax+0xd0]
    a.rip(b"\xff\x15", GUARD)                            # call [rip+GUARD]
    a.raw(b"\xc3")
    end("dispatch")

    begin("stack")                                       # creator into a stack slot, then a tail jump at slot 119
    a.raw(b"\x48\x83\xec\x48")                           # sub rsp,0x48
    a.raw(b"\x4c\x8d\x4c\x24\x30")                       # lea r9,[rsp+0x30]
    a.rip(b"\xff\x15", IAT)                              # call [rip+IAT]
    a.raw(b"\x48\x8b\x4c\x24\x30")                       # mov rcx,[rsp+0x30]
    a.raw(b"\x48\x83\xc4\x48")                           # add rsp,0x48
    a.raw(b"\x48\x8b\x01")                               # mov rax,[rcx]
    a.raw(b"\xff\xa0\xb8\x03\x00\x00")                   # jmp [rax+0x3b8]
    end("stack")

    begin("unrelated")                                   # another object's vtable call: not counted
    a.rip(b"\x48\x8b\x0d", OTHER)                        # mov rcx,[rip+OTHER]
    a.raw(b"\x48\x8b\x01")                               # mov rax,[rcx]
    a.raw(b"\xff\x50\x18")                               # call [rax+0x18]
    a.raw(b"\xc3")
    end("unrelated")

    begin("passed")                                      # the device handed to another function
    a.rip(b"\x48\x8b\x0d", DEVICE)                       # mov rcx,[rip+DEVICE]
    a.rel(b"\xe8", funcs["unrelated"][0])                # call unrelated
    a.raw(b"\xc3")
    end("passed")

    begin("guarded")                                     # slot 60 through the guard-check stub, call rel32
    a.rip(b"\x48\x8b\x0d", DEVICE)                       # mov rcx,[rip+DEVICE]
    a.raw(b"\x48\x8b\x01")                               # mov rax,[rcx]
    a.raw(b"\x48\x8b\x80\xe0\x01\x00\x00")               # mov rax,[rax+0x1e0]
    a.rel(b"\xe8", funcs_extra["guard_check"])           # call guard_check
    a.raw(b"\xc3")
    end("guarded")

    begin("helper")                                      # method in rax, call to a helper that only returns
    a.rip(b"\x48\x8b\x0d", DEVICE)                       # mov rcx,[rip+DEVICE]
    a.raw(b"\x48\x8b\x01")                               # mov rax,[rcx]
    a.raw(b"\x48\x8b\x80\x08\x01\x00\x00")               # mov rax,[rax+0x108]
    a.rel(b"\xe8", funcs_extra["ret_helper"])            # call ret_helper
    a.raw(b"\xc3")
    end("helper")

    begin("indexed")                                     # call [rax+rdx*8]: no slot
    a.rip(b"\x48\x8b\x0d", DEVICE)                       # mov rcx,[rip+DEVICE]
    a.raw(b"\x48\x8b\x01")                               # mov rax,[rcx]
    a.raw(b"\xff\x14\xd0")                               # call [rax+rdx*8]
    a.raw(b"\xc3")
    end("indexed")

    assert len(a.code) < 0x1000
    buf = bytearray(SIZE)
    buf[0:2] = b"MZ"
    struct.pack_into("<I", buf, 0x3C, 0x40)
    buf[0x40:0x44] = b"PE\0\0"
    sections = ((b".text", TEXT_RVA, 0x60000020), (b".idata", IDATA_RVA, 0xC0000040),
                (b".data", DATA_RVA, 0xC0000040), (b".pdata", PDATA_RVA, 0x40000040))
    struct.pack_into("<HHIIIHH", buf, 0x44, 0x8664, len(sections), 0, 0, 0, 0xF0, 0x22)
    opt = 0x58
    struct.pack_into("<H", buf, opt, 0x20B)
    struct.pack_into("<Q", buf, opt + 24, IMAGE_BASE)
    struct.pack_into("<II", buf, opt + 32, 0x1000, 0x1000)       # section and file alignment
    struct.pack_into("<II", buf, opt + 56, SIZE, 0x1000)         # SizeOfImage, SizeOfHeaders
    struct.pack_into("<I", buf, opt + 108, 16)
    struct.pack_into("<II", buf, opt + 112 + 8, IDATA_RVA, 40)   # import directory
    struct.pack_into("<II", buf, opt + 112 + 3 * 8, PDATA_RVA, 12 * len(funcs))
    for i, (name, rva, chars) in enumerate(sections):
        at = opt + 0xF0 + i * 40
        buf[at:at + len(name)] = name
        struct.pack_into("<IIII", buf, at + 8, 0x1000, rva, 0x1000, rva)
        struct.pack_into("<I", buf, at + 36, chars)

    lookup = IDATA_RVA + 0x40
    hint = IDATA_RVA + 0xA0
    struct.pack_into("<IIIII", buf, IDATA_RVA, lookup, 0, 0, IDATA_RVA + 0xD0, IAT)
    struct.pack_into("<Q", buf, lookup, hint)
    struct.pack_into("<Q", buf, IAT, hint)
    buf[hint + 2:hint + 2 + 22] = b"D3D12XboxCreateDevice\0"
    buf[IDATA_RVA + 0xD0:IDATA_RVA + 0xD0 + 13] = b"d3d12_xs.dll\0"

    buf[TEXT_RVA:TEXT_RVA + len(a.code)] = a.code
    struct.pack_into("<Q", buf, GUARD, IMAGE_BASE + funcs_extra["jmp_rax"])
    for i, (begin_rva, end_rva) in enumerate(sorted(funcs.values())):
        struct.pack_into("<III", buf, PDATA_RVA + 12 * i, begin_rva, end_rva, 0)
    return bytes(buf), {**{k: v[0] for k, v in funcs.items()}, **funcs_extra}


class DeviceCallsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data, cls.funcs = build_image()
        cls.r = ddc.scan(cls.data)

    def slots(self):
        return {s["slot"]: s for s in self.r["slots"]}

    def test_known_sites_are_counted_exactly(self):
        slots = self.slots()
        self.assertEqual(sorted(slots), [14, 26, 48, 60, 119])
        self.assertEqual(self.r["sites"], 5)
        for slot in slots.values():
            self.assertEqual(slot["sites"], 1)
        self.assertEqual((slots[14]["calls"], slots[14]["this_rcx"]), (1, 1))
        self.assertEqual((slots[48]["calls"], slots[48]["this_rcx"]), (1, 1))
        self.assertEqual((slots[26]["calls"], slots[26]["this_rdx"]), (1, 1))
        self.assertEqual((slots[119]["tail_jumps"], slots[119]["calls"], slots[119]["this_rcx"]), (1, 0, 1))
        self.assertEqual((slots[60]["calls"], slots[60]["this_rcx"]), (1, 1))   # through the guard-check stub

    def test_a_method_in_rax_needs_a_verified_dispatch_routine(self):
        self.assertNotIn(33, self.slots())                  # the helper that only returns
        self.assertEqual(self.r["unresolved"]["dispatch_calls_to_unverified_targets"], 1)
        # shdann's case on old upstream PR 7998: the dispatch site of slot 26 retargeted to the helper that only
        # returns.
        data = bytearray(self.data)
        start, stop = self.funcs["dispatch"], self.funcs["stack"]
        site = data.index(b"\xff\x15", start, stop)
        patch = Asm(site)
        patch.rel(b"\xe8", self.funcs["ret_helper"])
        patch.raw(b"\x90")
        data[site:site + 6] = patch.code
        r = ddc.scan(bytes(data))
        self.assertEqual([s["slot"] for s in r["slots"]], [14, 48, 60, 119])
        self.assertEqual(r["unresolved"]["dispatch_calls_to_unverified_targets"], 2)
        # A pointer to a routine that writes rax before its jmp rax isn't a dispatch routine either.
        data = bytearray(self.data)
        struct.pack_into("<Q", data, GUARD, IMAGE_BASE + self.funcs["ret_helper"])
        self.assertNotIn(26, {s["slot"] for s in ddc.scan(bytes(data))["slots"]})

    def test_the_unrelated_vtable_call_is_not_counted(self):
        self.assertNotIn(3, self.slots())
        rvas = {int(s["rva"], 16) for s in self.r["site_rvas"]}
        start, stop = self.funcs["unrelated"], self.funcs["passed"]
        self.assertFalse(any(start <= rva < stop for rva in rvas))

    def test_a_jump_within_the_function_is_a_branch_not_a_tail_call(self):
        # pr-sorter's repro on old upstream PR 7998: `xor edx,edx; mov r8d,1` in `copy` replaced by
        # `jmp rel32 +3; nop x3`.
        data = bytearray(self.data)
        start, stop = self.funcs["copy"], self.funcs["method"]
        at = data.index(b"\x31\xd2\x41\xb8\x01\x00\x00\x00", start, stop)
        data[at:at + 8] = b"\xe9\x03\x00\x00\x00\x90\x90\x90"
        r = ddc.scan(bytes(data))
        self.assertIn(14, {s["slot"] for s in r["slots"]})
        self.assertEqual(r["sites"], self.r["sites"])
        self.assertEqual(r["unresolved"]["device_passed_to_other_calls"], 1)
        # The same with a short jump (EB +3), and a backward jump in range: neither is a tail call.
        data[at:at + 8] = b"\xeb\x03\x90\x90\x90\x90\x90\x90"
        self.assertIn(14, {s["slot"] for s in ddc.scan(bytes(data))["slots"]})
        data[at:at + 8] = b"\x90\x90\x90\xe9" + struct.pack("<i", start - (at + 8))
        r = ddc.scan(bytes(data))
        self.assertEqual(r["unresolved"]["device_passed_to_other_calls"], 1)
        self.assertNotIn(14, {s["slot"] for s in r["slots"]})   # past a backward jump nothing is known

    def with_code(self, emit, pdata=True):
        """The fixture with one more stretch of code at a free .text address, as a .pdata function or without an
        entry (walked as a byte window from its device reference). Returns the scan."""
        a = Asm(EXTRA)
        emit(a)
        data = bytearray(self.data)
        data[EXTRA:EXTRA + len(a.code)] = a.code
        if pdata:
            size = struct.unpack_from("<I", data, 0x58 + 112 + 3 * 8 + 4)[0]
            struct.pack_into("<III", data, PDATA_RVA + size, EXTRA, a.here, 0)
            struct.pack_into("<I", data, 0x58 + 112 + 3 * 8 + 4, size + 12)
        return ddc.scan(bytes(data))

    @staticmethod
    def early_return(a, overwrite=False):
        a.rip(b"\x48\x8b\x0d", DEVICE)                   # mov rcx,[rip+DEVICE]
        a.raw(b"\x80\x79\x18\x00")                       # cmp byte [rcx+0x18],0
        if overwrite:
            a.raw(b"\x74\x03")                           # je +3
            a.raw(b"\x48\x89\xd1")                       # mov rcx,rdx: the fall-through loses the device
        else:
            a.raw(b"\x75\x01")                           # jne +1
            a.raw(b"\xc3")                               # ret
        a.raw(b"\x48\x8b\x01")                           # mov rax,[rcx]
        a.raw(b"\xff\x90\xc0\x00\x00\x00")               # call [rax+0xc0]: slot 24
        a.raw(b"\xc3")

    def test_a_conditional_branch_carries_the_device_past_an_early_return(self):
        for pdata in (True, False):                      # a .pdata function, and a byte window without an entry
            r = self.with_code(self.early_return, pdata)
            self.assertIn(24, {s["slot"] for s in r["slots"]}, pdata)
            self.assertEqual(r["sites"], self.r["sites"] + 1)

    def test_a_join_keeps_only_what_both_paths_agree_on(self):
        r = self.with_code(lambda a: self.early_return(a, overwrite=True))
        self.assertNotIn(24, {s["slot"] for s in r["slots"]})
        self.assertEqual(r["sites"], self.r["sites"])

    def test_an_object_member_out_pointer_is_followed_until_the_next_call(self):
        def emit(a):
            a.raw(b"\x4c\x8d\x4b\x40")                   # lea r9,[rbx+0x40]
            a.rip(b"\xff\x15", IAT)                      # call [rip+IAT]
            a.raw(b"\x48\x8b\x4b\x40")                   # mov rcx,[rbx+0x40]
            a.raw(b"\x48\x8b\x01")                       # mov rax,[rcx]
            a.raw(b"\xff\x90\x48\x01\x00\x00")           # call [rax+0x148]: slot 41
            a.raw(b"\x48\x8b\x4b\x40")                   # mov rcx,[rbx+0x40]: after a call, not known
            a.raw(b"\x48\x8b\x01")                       # mov rax,[rcx]
            a.raw(b"\xff\x90\x50\x01\x00\x00")           # call [rax+0x150]: slot 42, not counted
            a.raw(b"\xc3")
        r = self.with_code(emit)
        slots = {s["slot"] for s in r["slots"]}
        self.assertIn(41, slots)
        self.assertNotIn(42, slots)
        self.assertEqual(r["creators"][0]["out"],
                         {"global": 1, "stack slot": 1, "object member (followed until the next call)": 1})

    def test_creators_and_globals(self):
        self.assertEqual(self.r["creators"], [{"import": "d3d12_xs.dll!D3D12XboxCreateDevice", "call_sites": 2,
                                               "out": {"global": 1, "stack slot": 1}}])
        self.assertEqual(self.r["device_globals"], 2)

    def test_calls_tail_jumps_and_dispatch_sites_are_counted_apart(self):
        self.assertEqual((self.r["calls"], self.r["tail_jumps"], self.r["dispatch_sites"]), (4, 1, 2))

    def test_unresolved_sites_are_reported_not_guessed(self):
        self.assertEqual(self.r["unresolved"], {
            "vtable_loads_without_matched_call": 1,      # the indexed call
            "dispatch_calls_to_unverified_targets": 1,   # the helper that only returns
            "device_passed_to_other_calls": 1,
            "device_stored_to_non_stack_memory": 0,
            "creator_calls_out_not_followed": 0,
        })

    def test_names_and_argument_estimate(self):
        slots = self.slots()
        self.assertEqual(slots[14]["name"], "CreateDescriptorHeap")
        self.assertEqual(slots[14]["name_source"], "PC order, console measured")
        self.assertEqual(slots[48]["name"], "CreateCommandQueueX")
        self.assertIsNone(slots[119]["name"])
        self.assertEqual(slots[14]["args_max"], 3)          # this, rdx, r8
        self.assertEqual(ddc.argument_count({2, 8}, {4, 5, 9}), 6)

    def test_no_import_means_nothing_followed(self):
        data = bytearray(self.data)
        data[IDATA_RVA + 0xA2:IDATA_RVA + 0xA2 + 21] = b"SomethingElseEntirely"
        r = ddc.scan(bytes(data))
        self.assertEqual((r["creators"], r["slots"], r["sites"]), ([], [], 0))

    def test_a_32_bit_image_is_refused(self):
        data = bytearray(self.data)
        struct.pack_into("<H", data, 0x58, 0x10B)
        with self.assertRaises(pe_inspect.PeFormatError):
            ddc.scan(bytes(data))

    def test_cli_needs_out_and_writes_stable_reports_without_addresses(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "image.exe")
            with open(path, "wb") as f:
                f.write(self.data)
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as e:
                ddc.main([path])
            self.assertEqual(e.exception.code, 2)
            outputs = []
            for run in ("a", "b"):
                out = os.path.join(tmp, run)
                printed = io.StringIO()
                with contextlib.redirect_stdout(printed):
                    self.assertEqual(ddc.main([path, "--out", out]), 0)
                with open(os.path.join(out, "image.exe.device_calls.txt"), encoding="utf-8") as f:
                    text = f.read()
                with open(os.path.join(out, "image.exe.device_calls.json"), encoding="utf-8") as f:
                    raw = f.read()
                self.assertEqual(printed.getvalue(), text)
                outputs.append((text, raw))
            self.assertEqual(outputs[0], outputs[1])
            text, raw = outputs[0]
            self.assertNotIn("0x", text)
            self.assertIn("5 site(s) across 5 slot(s): 4 indirect vtable call(s) and 1 indirect vtable tail jump(s), "
                          "2 of them through a verified dispatch routine", text)
            self.assertEqual(json.loads(raw)["sites"], 5)

    def test_cli_refuses_a_missing_image(self):
        with tempfile.TemporaryDirectory() as tmp, contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(ddc.main([os.path.join(tmp, "none.exe"), "--out", tmp]), 1)


class DeviceCallsCliTests(unittest.TestCase):
    def test_invalid_out_fails_before_opening_or_scanning_image(self):
        import io
        import os
        import tempfile
        from contextlib import redirect_stderr, redirect_stdout
        from unittest import mock

        with tempfile.TemporaryDirectory() as tmp:
            blocker = os.path.join(tmp, "blocker")
            with open(blocker, "wb") as f:
                f.write(b"keep-me")
            img = os.path.join(tmp, "sample.exe")
            data, _ = build_image()
            with open(img, "wb") as f:
                f.write(data)
            for bad_out in (blocker, os.path.join(blocker, "nested"), ""):
                err, out = io.StringIO(), io.StringIO()
                with mock.patch.object(ddc, "scan", side_effect=AssertionError("scan should not run")), \
                        redirect_stderr(err), redirect_stdout(out):
                    rc = ddc.main([img, "--out", bad_out])
                self.assertEqual(rc, 1)
                self.assertEqual(out.getvalue(), "")
                self.assertIn("cannot write the report:", err.getvalue())
            # Even with a missing image, invalid --out is reported upfront before opening the image.
            err = io.StringIO()
            with redirect_stderr(err):
                rc = ddc.main([os.path.join(tmp, "missing.exe"), "--out", blocker])
            self.assertEqual(rc, 1)
            self.assertIn("cannot write the report:", err.getvalue())
            with open(blocker, "rb") as f:
                self.assertEqual(f.read(), b"keep-me")

    def test_self_overwrite_and_dir_target_rejected_before_scan_without_creating_out(self):
        import io
        import os
        import tempfile
        from contextlib import redirect_stderr
        from unittest import mock

        with tempfile.TemporaryDirectory() as tmp:
            out_dir = os.path.join(tmp, "reports")
            os.makedirs(out_dir)
            img = os.path.join(tmp, "app.exe")
            with open(img, "wb") as f:
                f.write(b"MZ")
            colliding = os.path.join(out_dir, "app.exe.device_calls.json")
            os.link(img, colliding)
            err = io.StringIO()
            with mock.patch.object(ddc, "scan", side_effect=AssertionError("scan should not run")), \
                    redirect_stderr(err):
                rc = ddc.main([img, "--out", out_dir])
            self.assertEqual(rc, 1)
            self.assertIn("would overwrite the input", err.getvalue())
            with open(img, "rb") as f:
                self.assertEqual(f.read(), b"MZ")

            dir_img = os.path.join(tmp, "dirapp.exe")
            dir_target = os.path.join(out_dir, "dirapp.exe.device_calls.txt")
            os.makedirs(dir_target)
            err = io.StringIO()
            with mock.patch.object(ddc, "scan", side_effect=AssertionError("scan should not run")), \
                    redirect_stderr(err):
                rc = ddc.main([dir_img, "--out", out_dir])
            self.assertEqual(rc, 1)
            self.assertIn("is a directory", err.getvalue())

    def test_bad_image_does_not_create_empty_out_dir(self):
        import io
        import os
        import tempfile
        from contextlib import redirect_stderr

        with tempfile.TemporaryDirectory() as tmp:
            out_dir = os.path.join(tmp, "unused_out")
            bad_img = os.path.join(tmp, "bad.exe")
            with open(bad_img, "wb") as f:
                f.write(b"not-a-pe")
            err = io.StringIO()
            with redirect_stderr(err):
                rc = ddc.main([bad_img, "--out", out_dir])
            self.assertEqual(rc, 1)
            self.assertIn("not a readable x86-64 PE image", err.getvalue())
            self.assertFalse(os.path.exists(out_dir))

    def test_atomic_write_preserves_existing_reports_on_staging_failure(self):
        import io
        import os
        import tempfile
        from contextlib import redirect_stderr, redirect_stdout
        from unittest import mock

        with tempfile.TemporaryDirectory() as tmp:
            out_dir = os.path.join(tmp, "out")
            os.makedirs(out_dir)
            img = os.path.join(tmp, "game.exe")
            data, _ = build_image()
            with open(img, "wb") as f:
                f.write(data)
            json_dest = os.path.join(out_dir, "game.exe.device_calls.json")
            txt_dest = os.path.join(out_dir, "game.exe.device_calls.txt")
            with open(json_dest, "wb") as f:
                f.write(b"old-json\n")
            with open(txt_dest, "wb") as f:
                f.write(b"old-txt\n")

            orig_mode = ddc.published_mode

            def fail_txt_mode(path: str) -> int:
                if path.endswith(".txt"):
                    raise PermissionError("simulated permission error")
                return orig_mode(path)

            err, out = io.StringIO(), io.StringIO()
            with mock.patch.object(ddc, "published_mode", side_effect=fail_txt_mode), \
                    redirect_stderr(err), redirect_stdout(out):
                rc = ddc.main([img, "--out", out_dir])
            self.assertEqual(rc, 1)
            self.assertEqual(out.getvalue(), "")
            self.assertIn("cannot write the report:", err.getvalue())
            with open(json_dest, "rb") as f:
                self.assertEqual(f.read(), b"old-json\n")
            with open(txt_dest, "rb") as f:
                self.assertEqual(f.read(), b"old-txt\n")
            self.assertEqual(sorted(os.listdir(out_dir)), [
                "game.exe.device_calls.json",
                "game.exe.device_calls.txt",
            ])

            out_ok, err_ok = io.StringIO(), io.StringIO()
            with redirect_stdout(out_ok), redirect_stderr(err_ok):
                rc = ddc.main([img, "--out", out_dir])
            self.assertEqual(rc, 0)
            self.assertEqual(err_ok.getvalue(), "")
            self.assertIn("site(s) across", out_ok.getvalue())
            with open(json_dest, "rb") as f:
                raw_json = f.read()
            with open(txt_dest, "rb") as f:
                raw_txt = f.read()
            self.assertNotIn(b"\r\n", raw_json)
            self.assertNotIn(b"\r\n", raw_txt)
            self.assertEqual(sorted(os.listdir(out_dir)), [
                "game.exe.device_calls.json",
                "game.exe.device_calls.txt",
            ])


if __name__ == "__main__":
    unittest.main()
