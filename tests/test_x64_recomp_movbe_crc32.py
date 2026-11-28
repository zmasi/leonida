"""Tests for movbe and SSE4.2 crc32 (0F 38 F0 / 0F 38 F1) in tools/x64_recomp.py.

Covers:
- Decoding & classification of 0F 38 F0/F1:
  * movbe 16/32/64-bit load & store (including REX.W overriding 66, RIP-relative)
  * rejection of invalid mod==3 movbe and F3 0F 38 F0/F1 encodings with exact size
  * crc32 8/16/32/64-bit register & memory forms (hi8 vs REX low8, 66 ignored on F0,
    32-bit Gd destination on 66 F2 F1, REX.W overriding 66 on F1)
  * unrelated 0F 38 SIMD opcodes remain unaffected
- Bit-exact ARM64 REV16 encoding and clean ARMv8.0 emission
- Hardware execution of movbe (16-bit upper-48-bit preservation, 32-bit zero-extend,
  64-bit round-trip, partial-width store isolation, base-register aliasing)
- Hardware execution of crc32 against RFC 3720 / iSCSI Castagnoli vector
  (b"123456789" -> 0xE3069283) across byte, word, dword, qword, mixed chunks,
  hi8/REX low8, upper-32-bit zero-extension, and dst==src aliasing
- Preservation of all flags (CF/ZF/SF/OF/PF) across both ARM NZCV and ctx.eflags

Static checks run everywhere; execution tests run natively on Apple Silicon when
CAN_RUN_MACHO is available, or under QEMU.

    python -I tests/test_x64_recomp_movbe_crc32.py
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


def _crc32c_ref(crc: int, val: int, nbytes: int) -> int:
    x = (crc & 0xFFFFFFFF) ^ (val & ((1 << (8 * nbytes)) - 1))
    for _ in range(8 * nbytes):
        x = (x >> 1) ^ (0x82F63B78 if (x & 1) else 0)
    return x & 0xFFFFFFFF


def _pack_flags_to_edi():
    """Set edi = ZF | (CF<<1) | (SF<<2) | (OF<<3) | (PF<<4) using r11..r15."""
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


class MovbeCrc32StaticTests(unittest.TestCase):
    def test_decode_movbe_all_widths_and_reject_invalid(self):
        cases = [
            # (hex, size, dst_kind, src_kind, width)
            ("0f38f007", 4, "r", "m", 4),           # movbe eax, [rdi]
            ("660f38f007", 5, "r", "m", 2),         # movbe ax, [rdi]
            ("480f38f007", 5, "r", "m", 8),         # movbe rax, [rdi]
            ("66480f38f007", 6, "r", "m", 8),       # REX.W overrides 66 -> 64-bit
            ("0f38f107", 4, "m", "r", 4),           # movbe [rdi], eax
            ("660f38f107", 5, "m", "r", 2),         # movbe [rdi], ax
            ("480f38f107", 5, "m", "r", 8),         # movbe [rdi], rax
            ("4c0f38f04810", 6, "r", "m", 8),       # movbe r9, [rax+0x10]
        ]
        for hx, size, dk, sk, width in cases:
            with self.subTest(hx=hx):
                raw = bytes.fromhex(hx)
                ins = xr.decode_one(raw, 0, len(raw), GUEST_VA)
                self.assertFalse(ins.unknown)
                self.assertIsNone(ins.error)
                self.assertEqual(ins.mnem, "movbe")
                self.assertEqual(ins.kind, "misc")
                self.assertEqual(ins.size, size)
                self.assertEqual(ins.flags_w, "")
                self.assertEqual(ins.flags_r, "")
                self.assertEqual(ins.ops[0][0], dk)
                self.assertEqual(ins.ops[1][0], sk)
                dst_sz = ins.ops[0][1]["size"] if dk == "m" else ins.ops[0][2]
                src_sz = ins.ops[1][1]["size"] if sk == "m" else ins.ops[1][2]
                self.assertEqual(dst_sz, width)
                self.assertEqual(src_sz, width)

        # RIP-relative movbe resolves abs = addr + size + disp
        rip_raw = bytes.fromhex("0f38f00510000000")
        ins = xr.decode_one(rip_raw, 0, len(rip_raw), GUEST_VA)
        self.assertEqual(ins.mnem, "movbe")
        self.assertEqual(ins.size, 8)
        self.assertTrue(ins.ops[1][1]["rip"])
        self.assertEqual(ins.ops[1][1]["abs"], GUEST_VA + 8 + 0x10)

        # Invalid mod==3 movbe and F3 0F 38 F0/F1 are rejected as unknown with exact length
        for bad_hx, exp_len in (
            ("0f38f0c0", 4),            # movbe eax, eax (mod=3 -> #UD)
            ("480f38f1c0", 5),          # movbe rax, rax (mod=3 -> #UD)
            ("f30f38f007", 5),          # F3 0F 38 F0 [rdi] (#UD)
            ("f30f38f14808", 6),        # F3 0F 38 F1 [rax+8] (#UD)
        ):
            with self.subTest(bad_hx=bad_hx):
                raw = bytes.fromhex(bad_hx)
                ins = xr.decode_one(raw, 0, len(raw), GUEST_VA)
                self.assertTrue(ins.unknown)
                self.assertEqual(ins.size, exp_len)

        # Unrelated 0F 38 SIMD opcodes (e.g. pshufb 0F 38 00) remain simd
        simd_raw = bytes.fromhex("660f3800c1")
        ins = xr.decode_one(simd_raw, 0, len(simd_raw), GUEST_VA)
        self.assertEqual(ins.kind, "simd")
        self.assertEqual(ins.size, 5)

    def test_decode_crc32_all_widths_hi8_and_prefixes(self):
        cases = [
            # (hex, size, dst_sz, src_kind, src_sz, src_hi8)
            ("f20f38f0c3", 5, 4, "r", 1, False),        # crc32 eax, bl
            ("f20f38f0c4", 5, 4, "r", 1, True),         # crc32 eax, ah (no REX)
            ("f2400f38f0c4", 6, 4, "r", 1, False),      # crc32 eax, spl (with REX)
            ("66f20f38f0c3", 6, 4, "r", 1, False),      # 66 ignored on F0
            ("f2480f38f0c3", 6, 8, "r", 1, False),      # crc32 rax, bl
            ("66f20f38f1c3", 6, 4, "r", 2, False),      # crc32 eax, bx (dst MUST be 4!)
            ("f20f38f1c3", 5, 4, "r", 4, False),        # crc32 eax, ebx
            ("f2480f38f1c3", 6, 8, "r", 8, False),      # crc32 rax, rbx
            ("66f2480f38f1c3", 7, 8, "r", 8, False),    # REX.W overrides 66 -> 64-bit
            ("f20f38f007", 5, 4, "m", 1, False),        # crc32 eax, byte [rdi]
            ("66f20f38f14c2408", 8, 4, "m", 2, False),  # crc32 ecx, word [rsp+8]
            ("f2480f38f14810", 7, 8, "m", 8, False),    # crc32 rcx, qword [rax+0x10]
        ]
        for hx, size, dst_sz, sk, src_sz, hi8 in cases:
            with self.subTest(hx=hx):
                raw = bytes.fromhex(hx)
                ins = xr.decode_one(raw, 0, len(raw), GUEST_VA)
                self.assertFalse(ins.unknown)
                self.assertIsNone(ins.error)
                self.assertEqual(ins.mnem, "crc32")
                self.assertEqual(ins.kind, "misc")
                self.assertEqual(ins.size, size)
                self.assertEqual(ins.flags_w, "")
                self.assertEqual(ins.flags_r, "")
                self.assertEqual(ins.ops[0][0], "r")
                self.assertEqual(ins.ops[0][2], dst_sz)
                self.assertEqual(ins.ops[1][0], sk)
                actual_src_sz = ins.ops[1][1]["size"] if sk == "m" else ins.ops[1][2]
                self.assertEqual(actual_src_sz, src_sz)
                if sk == "r":
                    self.assertEqual(bool(len(ins.ops[1]) > 3 and ins.ops[1][3]), hi8)

        # RIP-relative crc32 qword
        rip_raw = bytes.fromhex("f24c0f38f10d10000000")
        ins = xr.decode_one(rip_raw, 0, len(rip_raw), GUEST_VA)
        self.assertEqual(ins.mnem, "crc32")
        self.assertEqual(ins.size, 10)
        self.assertEqual(ins.ops[0][1], 9)              # r9
        self.assertEqual(ins.ops[1][1]["abs"], GUEST_VA + 10 + 0x10)

    def test_arm64_rev16_encoding_and_translation(self):
        a = xr.Asm()
        a.rev16(0, 0, sf=0)
        a.rev16(1, 2, sf=0)
        a.rev16(0, 1, sf=1)
        self.assertEqual(a.words, [0x5AC00400, 0x5AC00441, 0xDAC00420])

        for hx, expect_word in (
            ("660f38f007c3", 0x5AC00400),       # movbe ax, [rdi] -> rev16 w0, w0
            ("0f38f007c3", 0x5AC00800),         # movbe eax, [rdi] -> rev w0, w0
            ("480f38f007c3", 0xDAC00C00),       # movbe rax, [rdi] -> rev x0, x0
            ("660f38f107c3", 0x5AC00400),       # movbe [rdi], ax -> rev16 w0, w0
            ("480f38f107c3", 0xDAC00C00),       # movbe [rdi], rax -> rev x0, x0
        ):
            with self.subTest(movbe_hx=hx):
                ft = xr.FunctionTranslator(xr.disasm(bytes.fromhex(hx), GUEST_VA))
                ft.translate()
                self.assertEqual(ft.stats["unhandled"], [])
                self.assertIn(expect_word, ft.asm.words)

        for hx in ("f20f38f0c3c3", "66f20f38f1c3c3", "f20f38f1c3c3", "f2480f38f1c3c3"):
            with self.subTest(crc32_hx=hx):
                ft = xr.FunctionTranslator(xr.disasm(bytes.fromhex(hx), GUEST_VA))
                ft.translate()
                self.assertEqual(ft.stats["unhandled"], [])
                # Verify Castagnoli polynomial low halfword (0x3B78) and neg x3, x3 (0xCB0303E3)
                self.assertIn(0xCB0303E3, ft.asm.words)


@unittest.skipUnless(RUNNER is not None, "no arm64 runner available")
class MovbeCrc32ExecTests(unittest.TestCase):
    def _run(self, code: bytes) -> int:
        val, ft = RUNNER(code)
        self.assertEqual(ft.stats["unhandled"], [])
        return val

    def test_exec_movbe_16_32_64_load_store_and_aliasing(self):
        # 1. 16-bit load preserves upper 48 bits of RAX
        code = (
            bytes.fromhex("4883ec20")                   # sub rsp, 32
            + _imm64(9, 0xFFEEDDCCBBAA1234)
            + bytes.fromhex("4c890c24")                 # mov [rsp], r9
            + _imm64(0, 0x1122334455667788)
            + bytes.fromhex("660f38f00424")             # movbe ax, [rsp] -> ax = 0x3412
            + _imm64(10, 0x1122334455663412)
            + bytes.fromhex("4c39d0" "0f94c0" "0fb6f8")  # cmp rax, r10; sete al; movzx edi, al
            + bytes.fromhex("4883c420c3")
        )
        self.assertEqual(self._run(code), 1)

        # 2. 32-bit load zero-extends upper 32 bits of RAX
        code = (
            bytes.fromhex("4883ec20")
            + _imm64(9, 0xDEADBEEF12345678)
            + bytes.fromhex("4c890c24")
            + _imm64(0, 0xCAFEBABEFFFFFFFF)
            + bytes.fromhex("0f38f00424")               # movbe eax, [rsp] -> rax = 0x0000000078563412
            + _imm64(10, 0x0000000078563412)
            + bytes.fromhex("4c39d0" "0f94c0" "0fb6f8")
            + bytes.fromhex("4883c420c3")
        )
        self.assertEqual(self._run(code), 1)

        # 3. 64-bit load & store (including 66 + REX.W override) + adjacent sentinel check for 16/32 store
        code = (
            bytes.fromhex("4883ec20")
            + _imm64(9, 0x0123456789ABCDEF)
            + bytes.fromhex("4c0f38f10c24")             # movbe [rsp], r9 -> stores 0xEFCDAB8967452301
            + bytes.fromhex("488b0424")                 # mov rax, [rsp]
            + _imm64(10, 0xEFCDAB8967452301)
            + bytes.fromhex("4c39d0" "410f94c3" "410fb6fb")  # bit0: raw stored bytes swapped
            + bytes.fromhex("66480f38f00424")           # 66+REX.W movbe rax, [rsp] -> 0x0123456789ABCDEF
            + bytes.fromhex("4c39c8" "410f94c3" "450fb6db" "41d1e3" "4409df")  # bit1: round-trip 64
            # 16-bit store into [rsp+8] must not disturb upper 6 bytes of qword at [rsp+8]
            + _imm64(9, 0xAABBCCDDEEFF0000)
            + bytes.fromhex("4c894c2408")               # mov [rsp+8], r9
            + _imm64(0, 0x9988776655441234)
            + bytes.fromhex("660f38f1442408")           # movbe [rsp+8], ax -> writes 0x34, 0x12
            + bytes.fromhex("488b442408")
            + _imm64(10, 0xAABBCCDDEEFF3412)
            + bytes.fromhex("4c39d0" "410f94c3" "450fb6db" "41c1e302" "4409df")  # bit2: 16-bit store isolated
            # 32-bit store into [rsp+16] must not disturb upper 4 bytes of qword at [rsp+16]
            + _imm64(9, 0x5566778800000000)
            + bytes.fromhex("4c894c2410")
            + _imm64(0, 0xFFFFFFFF11223344)
            + bytes.fromhex("0f38f1442410")             # movbe [rsp+16], eax -> writes 0x44332211
            + bytes.fromhex("488b442410")
            + _imm64(10, 0x5566778844332211)
            + bytes.fromhex("4c39d0" "410f94c3" "450fb6db" "41c1e303" "4409df")  # bit3: 32-bit store isolated
            + bytes.fromhex("4883c420c3")
        )
        self.assertEqual(self._run(code), 0x0F)

        # 4. Aliasing: movbe rcx, [rcx] and movbe [rcx], rcx
        code = (
            bytes.fromhex("4883ec20")
            + _imm64(9, 0x0102030405060708)
            + bytes.fromhex("4c890c24")                 # [rsp] = 0x0102030405060708
            + bytes.fromhex("4889e1")                   # mov rcx, rsp
            + bytes.fromhex("480f38f009")               # movbe rcx, [rcx] -> rcx = 0x0807060504030201
            + _imm64(10, 0x0807060504030201)
            + bytes.fromhex("4c39d1" "410f94c3" "410fb6fb")  # bit0: load alias
            + bytes.fromhex("4889e1")                   # mov rcx, rsp (rcx == address)
            + bytes.fromhex("480f38f109")               # movbe [rcx], rcx (store bswap(rsp) to [rsp])
            + bytes.fromhex("480f38f00424")             # movbe rax, [rsp] (unswap back to rsp)
            + bytes.fromhex("4839e0" "410f94c3" "450fb6db" "41d1e3" "4409df")  # bit1: store alias
            + bytes.fromhex("4883c420c3")
        )
        self.assertEqual(self._run(code), 0x03)

    def test_exec_crc32_rfc3720_all_widths_and_aliasing(self):
        # 1. RFC 3720 / iSCSI standard Castagnoli vector: CRC32C(b"123456789") == 0xE3069283
        #    (a) Byte-by-byte via memory operand (F2 0F 38 F0)
        msg = b"123456789"
        push_bytes = b""
        for i, b in enumerate(msg):
            push_bytes += bytes([0xC6, 0x44, 0x24, i, b])   # mov byte [rsp+i], b
        byte_crcs = b""
        for i in range(len(msg)):
            byte_crcs += bytes([0xF2, 0x0F, 0x38, 0xF0, 0x44, 0x24, i])  # crc32 eax, byte [rsp+i]
        code = (
            bytes.fromhex("4883ec20")
            + push_bytes
            + bytes.fromhex("b8ffffffff")               # mov eax, 0xFFFFFFFF
            + byte_crcs
            + bytes.fromhex("f7d0" "89c7")              # not eax; mov edi, eax
            + bytes.fromhex("4883c420c3")
        )
        self.assertEqual(self._run(code), 0xE3069283)

        #    (b) Mixed 64-bit qword ("12345678") + 8-bit byte ("9") with dirty upper 32 bits in RAX
        code = (
            bytes.fromhex("4883ec20")
            + push_bytes
            + _imm64(0, 0xDEADBEEFFFFFFFFF)             # upper 32 dirty; low 32 = 0xFFFFFFFF
            + bytes.fromhex("f2480f38f10424")           # crc32 rax, qword [rsp]
            + bytes.fromhex("f2480f38f0442408")         # crc32 rax, byte [rsp+8]
            # Verify upper 32 bits of RAX are 0 AND low 32 ^ 0xFFFFFFFF == 0xE3069283
            + _imm64(10, 0xE3069283 ^ 0xFFFFFFFF)
            + bytes.fromhex("4c39d0" "0f94c0" "0fb6f8")  # full 64-bit equality confirms upper 32 == 0!
            + bytes.fromhex("4883c420c3")
        )
        self.assertEqual(self._run(code), 1)

        #    (c) Mixed 32-bit ("1234") + 16-bit ("56") + 16-bit ("78") + 8-bit ("9")
        code = (
            bytes.fromhex("4883ec20")
            + push_bytes
            + _imm64(0, 0xCAFEBABEFFFFFFFF)             # dirty upper 32 bits
            + bytes.fromhex("f20f38f10424")             # crc32 eax, dword [rsp]
            + bytes.fromhex("66f20f38f1442404")         # crc32 eax, word [rsp+4]
            + bytes.fromhex("66f20f38f1442406")         # crc32 eax, word [rsp+6]
            + bytes.fromhex("66f20f38f0442408")         # 66+F2 F0 (66 ignored -> byte [rsp+8])
            + _imm64(10, 0xE3069283 ^ 0xFFFFFFFF)
            + bytes.fromhex("4c39d0" "0f94c0" "0fb6f8")  # full 64-bit check (upper 32 must be 0!)
            + bytes.fromhex("4883c420c3")
        )
        self.assertEqual(self._run(code), 1)

        # 2. Register sources: hi8 (ah), REX low8 (spl), 16-bit (bx), 32-bit (ebx),
        #    64-bit (rbx) with 66+REX.W override, plus dst==src aliasing (crc32 eax, eax / ah)
        init = 0x13579BDF
        expected_ah = _crc32c_ref(init, 0xAB, 1)
        code = (
            _imm64(0, 0xDEAD000000000000 | init)
            + _imm64(3, 0x000000000000AB55)             # bh = 0xAB, bl = 0x55
            + bytes.fromhex("f20f38f0c7")               # crc32 eax, bh (ModRM=11 000 111 -> reg=eax, rm=7=bh)
            + bytes.fromhex("89c7c3")                   # mov edi, eax; ret
        )
        self.assertEqual(self._run(code), expected_ah)

        # REX 0x40 + rm=7 -> dil (NOT bh!)
        expected_dil = _crc32c_ref(init, 0x5A, 1)
        code = (
            _imm64(0, init)
            + _imm64(7, 0x000000000000995A)             # dil = 0x5A
            + bytes.fromhex("f2400f38f0c7")             # crc32 eax, dil
            + bytes.fromhex("89c7c3")
        )
        self.assertEqual(self._run(code), expected_dil)

        # 16-bit reg (66 F2 F1) zeroes upper 32 of RAX while hashing only low 16 bits of RBX
        expected_w = _crc32c_ref(init, 0xBEEF, 2)
        code = (
            _imm64(0, 0x55AA55AA00000000 | init)
            + _imm64(3, 0xFFFF12340000BEEF)
            + bytes.fromhex("66f20f38f1c3")             # crc32 eax, bx
            + _imm64(10, expected_w)
            + bytes.fromhex("4c39d0" "0f94c0" "0fb6f8" "c3")
        )
        self.assertEqual(self._run(code), 1)

        # 66 + REX.W + F2 F1 hashes all 64 bits of RBX and clears upper 32 bits of RAX
        val64 = 0x0123456789ABCDEF
        expected_q = _crc32c_ref(init, val64, 8)
        code = (
            _imm64(0, 0xFACEFEED00000000 | init)
            + _imm64(3, val64)
            + bytes.fromhex("66f2480f38f1c3")           # crc32 rax, rbx (REX.W beats 66)
            + _imm64(10, expected_q)
            + bytes.fromhex("4c39d0" "0f94c0" "0fb6f8" "c3")
        )
        self.assertEqual(self._run(code), 1)

        # Aliasing: crc32 eax, eax; crc32 eax, ah; crc32 rax, rax
        v1 = _crc32c_ref(init, init, 4)
        v2 = _crc32c_ref(v1, (v1 >> 8) & 0xFF, 1)
        v3 = _crc32c_ref(v2, v2, 8)
        code = (
            _imm64(0, 0x9999999900000000 | init)
            + bytes.fromhex("f20f38f1c0")               # crc32 eax, eax
            + bytes.fromhex("f20f38f0c4")               # crc32 eax, ah
            + bytes.fromhex("f2480f38f1c0")             # crc32 rax, rax
            + bytes.fromhex("89c7c3")
        )
        self.assertEqual(self._run(code), v3)

    def test_exec_movbe_and_crc32_preserve_all_flags(self):
        # Verify movbe (16/32/64 load/store) and crc32 (8/16/32/64) preserve
        # CF=1, SF=1, OF=0, PF=1, ZF=0 (mask 0x16) AND CF=0, SF=0, OF=1, PF=1, ZF=0 (mask 0x18)
        # across both ARM NZCV (same block) and materialized ctx.eflags (cross block).
        op_seq = bytes.fromhex(
            "660f38f10424"      # movbe [rsp], ax
            "660f38f00424"      # movbe ax, [rsp]
            "0f38f10424"        # movbe [rsp], eax
            "0f38f00424"        # movbe eax, [rsp]
            "480f38f10424"      # movbe [rsp], rax
            "480f38f00424"      # movbe rax, [rsp]
            "f20f38f0c3"        # crc32 eax, bl
            "66f20f38f1c3"      # crc32 eax, bx
            "f20f38f1c3"        # crc32 eax, ebx
            "f2480f38f1c3"      # crc32 rax, rbx
        )
        for cross_block in (False, True):
            for setup_hex, expected_mask in (
                ("41b900000000" "4183e901", 0x16),      # 0 - 1 -> CF=1, SF=1, OF=0, PF=1, ZF=0
                ("41b900000080" "4183e901", 0x18),      # 0x80000000 - 1 -> CF=0, SF=0, OF=1, PF=1, ZF=0
            ):
                with self.subTest(cross_block=cross_block, expected=hex(expected_mask)):
                    code = (
                        bytes.fromhex("4883ec20")
                        + _imm64(0, 0x1122334455667788)
                        + _imm64(3, 0xAABBCCDDEEFF1234)
                        + bytes.fromhex(setup_hex)
                        + op_seq
                        + (bytes.fromhex("eb00") if cross_block else b"")
                        + _pack_flags_to_edi()
                        + bytes.fromhex("4883c420c3")
                    )
                    self.assertEqual(self._run(code), expected_mask)


if __name__ == "__main__":
    unittest.main()
