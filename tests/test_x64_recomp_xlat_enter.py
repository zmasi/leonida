"""Tests for xlat/xlatb (0xD7) and enter (0xC8) emitters in tools/x64_recomp.py.

Covers:
- Decoding & classification of xlat (D7, 67 D7) and enter (C8 iw ib)
- ARM64 32-bit vs 64-bit addressing for 67 D7 vs D7, and large imm16 >= 4096
  (including unsigned 0x8000) in enter
- Hardware execution of xlat across 0..255 indices (including 0x80/0xFF proving
  unsigned zero-extension of AL) while preserving upper 56 bits of RAX
- Hardware execution of enter + leave across nesting 0, 1, 2, 3 (and 35 % 32 == 3)
  plus large frame allocation (>= 4096), verifying exact lexical display pointers
- Preservation of all flags (CF/ZF/SF/OF/PF) across both ARM NZCV and ctx.eflags

Static checks run everywhere; execution tests run natively on Apple Silicon when
CAN_RUN_MACHO is available, or under QEMU.

    python -I tests/test_x64_recomp_xlat_enter.py
"""
import os
import shutil
import struct
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")
if TOOLS not in sys.path:
    sys.path.insert(0, TOOLS)

import x64_recomp as xr  # noqa: E402

GUEST_VA = 0x46000000


def _runner():
    if getattr(xr, "CAN_RUN_MACHO", False) and hasattr(xr, "run_result") and not os.environ.get("QEMU_AARCH64"):
        return lambda code: xr.run_result(code, GUEST_VA)
    qemu = os.environ.get("QEMU_AARCH64") or shutil.which("qemu-system-aarch64")
    if qemu and os.path.exists(qemu):
        return lambda code: xr.run_kernel(code, GUEST_VA)
    return None


RUNNER = _runner()


def _imm64(reg, val):
    rex = 0x48 | (1 if reg >= 8 else 0)
    return bytes([rex, 0xB8 + (reg & 7)]) + struct.pack("<Q", val & 0xFFFFFFFFFFFFFFFF)


def _pack_flags_to_edi():
    """Set edi = ZF | (CF<<1) | (SF<<2) | (OF<<3) | (PF<<4) before ALU ops alter PF."""
    return bytes.fromhex(
        "410f94c3"      # setz r11b
        "410f92c4"      # setc r12b
        "410f98c5"      # sets r13b
        "410f90c6"      # seto r14b
        "410f9ac7"      # setp r15b
        "410fb6fb"      # movzx edi, r11b
        "450fb6e4"      # movzx r12d, r12b
        "41d1e4"        # shl r12d, 1
        "4409e7"        # or edi, r12d
        "450fb6ed"      # movzx r13d, r13b
        "41c1e502"      # shl r13d, 2
        "4409ef"        # or edi, r13d
        "450fb6f6"      # movzx r14d, r14b
        "41c1e603"      # shl r14d, 3
        "4409f7"        # or edi, r14d
        "450fb6ff"      # movzx r15d, r15b
        "41c1e704"      # shl r15d, 4
        "4409ff"        # or edi, r15d
    )


class XlatEnterStaticTests(unittest.TestCase):
    def test_decode_and_translate_xlat_and_enter(self):
        # xlat (D7) and 67 xlat (67 D7)
        for hx, size, addrsize, exp_sf in (("d7", 1, 0, 1), ("67d7", 2, 1, 0)):
            with self.subTest(xlat=hx):
                raw = bytes.fromhex(hx)
                ins = xr.decode_one(raw, 0, len(raw), GUEST_VA)
                self.assertFalse(ins.unknown)
                self.assertEqual(ins.mnem, "xlat")
                self.assertEqual(ins.kind, "misc")
                self.assertEqual(ins.size, size)
                self.assertEqual(ins.addrsize, addrsize)
                self.assertEqual(ins.flags_w, "")
                self.assertEqual(ins.flags_r, "")
                ft = xr.FunctionTranslator(xr.disasm(raw + b"\xc3", GUEST_VA))
                ft.translate()
                self.assertEqual(ft.stats["unhandled"], [])
                # Verify add x16, x22, x0 (sf=1) vs add w16, w22, w0 (sf=0)
                a_ref = xr.Asm()
                a_ref.add_r(xr.ADDR_T, xr.GMAP[3], xr.T0, sf=exp_sf)
                self.assertIn(a_ref.words[0], ft.asm.words)

        # enter imm16, imm8 across nesting 0, 1, 3, 35 and unsigned imm16 0x8000
        for hx in ("c8000000", "c8200000", "c8008000", "c8100001", "c8100003", "c8100023"):
            with self.subTest(enter=hx):
                raw = bytes.fromhex(hx)
                ins = xr.decode_one(raw, 0, len(raw), GUEST_VA)
                self.assertFalse(ins.unknown)
                self.assertEqual(ins.mnem, "enter")
                self.assertEqual(ins.kind, "misc")
                self.assertEqual(ins.size, 4)
                self.assertEqual(ins.flags_w, "")
                self.assertEqual(ins.flags_r, "")
                ft = xr.FunctionTranslator(xr.disasm(raw + b"\xc9\xc3", GUEST_VA))
                ft.translate()
                self.assertEqual(ft.stats["unhandled"], [])


@unittest.skipUnless(RUNNER is not None, "no arm64 runner available")
class XlatEnterExecTests(unittest.TestCase):
    def _run(self, code: bytes) -> int:
        val, ft = RUNNER(code)
        self.assertEqual(ft.stats["unhandled"], [])
        return val

    def test_exec_xlat_unsigned_indices_and_upper_rax_preserved(self):
        # Allocate 256-byte table at [rsp], populate table[i] = (i * 3 + 7) & 0xFF
        # Loop: ecx = 0..255; mov eax, ecx; imul eax, 3; add eax, 7; mov [rsp+rcx], al; inc ecx; cmp ecx, 256; jne
        init_loop = bytes.fromhex(
            "89c8"                  # mov eax, ecx
            "6bc003"                # imul eax, eax, 3
            "83c007"                # add eax, 7
            "88040c"                # mov [rsp+rcx], al
            "ffc1"                  # inc ecx
            "81f900010000"          # cmp ecx, 256
        )
        rel_back = (-(len(init_loop) + 2)) & 0xFF
        # Look up indices 0, 17, 0x80 (128: tests unsigned AL!), 0xFF (255),
        # each time with sentinel upper 56 bits 0x1122334455667700 in RAX!
        checks = b""
        for bit, idx in enumerate((0, 17, 0x80, 0xFF)):
            exp_val = 0x1122334455667700 | ((idx * 3 + 7) & 0xFF)
            checks += (
                _imm64(0, 0x1122334455667700 | idx)
                + b"\xd7"                               # xlatb
                + _imm64(10, exp_val)
                + bytes.fromhex("4c39d0" "410f94c3" "450fb6db")
                + (bytes([0x41, 0xC1, 0xE3, bit]) if bit else b"")
                + bytes.fromhex("4409df")               # or edi, r11d
            )
        code = (
            bytes.fromhex("4881ec00010000")             # sub rsp, 256
            + bytes.fromhex("31c9")                     # xor ecx, ecx
            + init_loop
            + bytes([0x75, rel_back])                   # jne init_loop
            + bytes.fromhex("4889e3" "31ff")            # mov rbx, rsp; xor edi, edi
            + checks
            + bytes.fromhex("4881c400010000c3")         # add rsp, 256; ret
        )
        self.assertEqual(self._run(code), 0x0F)

    def test_exec_enter_and_leave_nesting_0_1_2_3_and_large_alloc(self):
        # 1. enter 0x1200, 0 (alloc = 4608 >= 4096) + leave round-trip
        code = (
            _imm64(5, 0x123456789ABCDEF0)               # sentinel RBP
            + bytes.fromhex("4989e0")                   # mov r8, rsp (original RSP)
            + bytes.fromhex("c8001200")                 # enter 0x1200, 0
            + bytes.fromhex("4889e8" "4829e0")          # mov rax, rbp; sub rax, rsp -> must be 0x1200
            + _imm64(10, 0x1200)
            + bytes.fromhex("4c39d0" "410f94c3" "410fb6fb")  # bit0: frame alloc == 0x1200
            + bytes.fromhex("488b4500")                 # mov rax, [rbp] -> saved sentinel RBP
            + _imm64(10, 0x123456789ABCDEF0)
            + bytes.fromhex("4c39d0" "410f94c3" "450fb6db" "41d1e3" "4409df")  # bit1: [rbp] == old rbp
            + b"\xc9"                                   # leave
            + bytes.fromhex("4c39c4" "410f94c3" "450fb6db" "41c1e302" "4409df")  # bit2: rsp restored
            + bytes.fromhex("4c39d5" "410f94c3" "450fb6db" "41c1e303" "4409df")  # bit3: rbp restored
            + b"\xc3"
        )
        self.assertEqual(self._run(code), 0x0F)

        # 2. Nested lexical displays:
        #    enter 16, 1 (F1 -> r9) -> enter 16, 2 (F2 -> r10) -> enter 16, 35 (35&31==3, F3)
        #    Verify [F3-8]==F1, [F3-16]==F2, [F3-24]==F3, [F3]==F2, F3-rsp == 24+16 == 40,
        #    and 3x leave restores original RSP & RBP!
        code = (
            _imm64(5, 0xCAFEBABE11223344)               # sentinel RBP -> r12
            + bytes.fromhex("4989ec" "4989e0")          # mov r12, rbp; mov r8, rsp
            + bytes.fromhex("c8100001")                 # enter 16, 1
            + bytes.fromhex("4989e9")                   # mov r9, rbp (F1)
            + bytes.fromhex("c8100002")                 # enter 16, 2
            + bytes.fromhex("4989ea")                   # mov r10, rbp (F2)
            + bytes.fromhex("c8100023")                 # enter 16, 35 (nesting = 3)
            # bit0: [rbp-8] == F1 (r9)
            + bytes.fromhex("488b45f8" "4c39c8" "410f94c3" "410fb6fb")
            # bit1: [rbp-16] == F2 (r10)
            + bytes.fromhex("488b45f0" "4c39d0" "410f94c3" "450fb6db" "41d1e3" "4409df")
            # bit2: [rbp-24] == F3 (rbp)
            + bytes.fromhex("488b45e8" "4839e8" "410f94c3" "450fb6db" "41c1e302" "4409df")
            # bit3: rbp - rsp == 40 (3 display qwords + 16 alloc)
            + bytes.fromhex("4889e8" "4829e0" "4883f828" "410f94c3" "450fb6db" "41c1e303" "4409df")
            + bytes.fromhex("c9c9c9")                   # leave; leave; leave
            # bit4: rsp == r8 (original RSP)
            + bytes.fromhex("4c39c4" "410f94c3" "450fb6db" "41c1e304" "4409df")
            # bit5: rbp == r12 (original RBP)
            + bytes.fromhex("4c39e5" "410f94c3" "450fb6db" "41c1e305" "4409df")
            + b"\xc3"
        )
        self.assertEqual(self._run(code), 0x3F)

    def test_exec_xlat_and_enter_preserve_all_flags(self):
        for cross in (False, True):
            for setup_hex, expected_mask in (
                ("41b900000000" "4183e901", 0x16),      # 0 - 1 -> CF=1, SF=1, OF=0, PF=1, ZF=0
                ("41b900000080" "4183e901", 0x18),      # 0x80000000 - 1 -> CF=0, SF=0, OF=1, PF=1, ZF=0
            ):
                with self.subTest(cross=cross, expected=hex(expected_mask)):
                    code = (
                        bytes.fromhex("4883ec20" "4889e3" "c604245a")  # table[0] = 0x5A, rbx = rsp
                        + bytes.fromhex("31c0")                         # al = 0
                        + bytes.fromhex(setup_hex)
                        + b"\xd7"                                       # xlatb (must preserve flags!)
                        + bytes.fromhex("c8100001" "c8100002")          # enter 16, 1; enter 16, 2 (loop!)
                        + bytes.fromhex("c9c9")                         # leave; leave
                        + (b"\xeb\x00" if cross else b"")
                        + _pack_flags_to_edi()
                        + bytes.fromhex("4883c420c3")
                    )
                    self.assertEqual(self._run(code), expected_mask)


if __name__ == "__main__":
    unittest.main()
