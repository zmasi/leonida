"""Tests for sign-extended immediates in ALU/CMP/TEST/MOV, Asm.li(-1), and movnti in tools/x64_recomp.py.

Regression coverage for:
- 83 /r ib and 81 /r id / 05..3D / F7 /0 / A9 sign-extending negative immediates to destination width
  (e.g. `and rsp, -16` keeping bits 8..63 instead of truncating to 0xF0; `cmp rax, -1` comparing
  against 0xFFFFFFFFFFFFFFFF instead of 0xFF or 0xFFFFFFFF; `test rax, -1` testing upper 32 bits)
- C7 /0 (`mov qword ptr [mem], imm32`) sign-extending to 64 bits for memory as well as register dst
- Asm.li(rd, -1) handling 0xFFFFFFFFFFFFFFFF without IndexError (needed by `push -1`, `cmp rax, -1`, etc.)
- `movnti` (0F C3) emitting 32/64-bit memory stores instead of UnhandledInsn

Static checks run everywhere; execution tests run natively on Apple Silicon when
CAN_RUN_MACHO is available, or under QEMU.

    python -I tests/test_x64_recomp_signext_imm.py
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


class SignExtImmStaticTests(unittest.TestCase):
    """Static ARM64 encoding & translation checks that run on every platform."""

    def translate(self, code):
        insns = list(xr.disasm(code, 0x1000))
        self.assertFalse(any(i.unknown or i.error for i in insns), insns)
        tr = xr.FunctionTranslator(insns)
        tr.translate()
        self.assertEqual(tr.stats["unhandled"], [], f"unhandled: {tr.stats['unhandled']}")
        return tr

    def test_and_rsp_neg16_loads_64bit_mask_not_0xf0(self):
        # 48 83 e4 f0 (and rsp, -16): must load 0xFFFFFFFFFFFFFFF0 via MOVN X1, #0xF, NOT MOVZ X1, #0xF0!
        tr = self.translate(bytes.fromhex("4883e4f0c3"))
        a_good, a_bad = xr.Asm(), xr.Asm()
        a_good.movn(xr.T1, 0xF)
        a_bad.movz(xr.T1, 0xF0)
        self.assertIn(a_good.words[0], tr.asm.words, "and rsp, -16 must sign-extend -16 to 64 bits")
        self.assertNotIn(a_bad.words[0], tr.asm.words)

    def test_cmp_and_test_sign_extend_imm8_and_imm32(self):
        a_good, a_bad8 = xr.Asm(), xr.Asm()
        a_good.movn(xr.T1, 0)
        a_bad8.movz(xr.T1, 0xFF)
        for label, hx in [
            ("cmp rax, -1 (83)", "4883f8ffc3"),
            ("cmp rax, -1 (81)", "4881f8ffffffffc3"),
            ("cmp rax, -1 (3d)", "483dffffffffc3"),
            ("test rax, -1 (f7)", "48f7c0ffffffffc3"),
            ("test rax, -1 (a9)", "48a9ffffffffc3"),
        ]:
            with self.subTest(label=label):
                tr = self.translate(bytes.fromhex(hx))
                self.assertIn(a_good.words[0], tr.asm.words, f"{label} must materialize 64-bit -1 via MOVN")
                self.assertNotIn(a_bad8.words[0], tr.asm.words)

    def test_mov_mem64_neg_imm32_sign_extends_and_movnti_translates(self):
        # 48 c7 04 24 ff ff ff ff (mov qword ptr [rsp], -1): must load 64-bit -1 via MOVN X0, #0!
        tr = self.translate(bytes.fromhex("48c70424ffffffffc3"))
        a_good = xr.Asm()
        a_good.movn(xr.T0, 0)
        self.assertIn(a_good.words[0], tr.asm.words, "mov qword [rsp], -1 must sign-extend imm32 to 64 bits")

        # movnti [rsp], eax (0f c3 04 24) and movnti [rsp], rax (48 0f c3 04 24)
        self.translate(bytes.fromhex("0fc30424" "480fc30424" "c3"))

    def test_push_neg1_does_not_crash_asm_li(self):
        tr = self.translate(bytes.fromhex("6aff" "68ffffffff" "c3"))
        a_good = xr.Asm()
        a_good.movn(xr.T0, 0)
        self.assertGreaterEqual(tr.asm.words.count(a_good.words[0]), 2)


@unittest.skipIf(RUNNER is None, "needs Darwin arm64 Mach-O runner or qemu-system-aarch64")
class SignExtImmExecTests(unittest.TestCase):
    """End-to-end arm64 execution tests for sign-extended immediates and movnti."""

    def run_code(self, code):
        res, tr = RUNNER(code)
        self.assertEqual(tr.stats["unhandled"], [], f"unhandled: {tr.stats['unhandled']}")
        return res

    def test_and_rsp_neg16_aligns_pointer_without_truncating_high_bits(self):
        # 48 83 e0 f0 (and rax, -16): must preserve bits 8..63 and clear only low 4 bits!
        code = (
            _imm64(0, 0x1234_5678_9ABC_DEF7)
            + bytes.fromhex("4883e0f0")
            + _imm64(2, 0x1234_5678_9ABC_DEF0)
            + bytes.fromhex("4839d0" "0f94c0" "0fb6f8" "c3")
        )
        self.assertEqual(self.run_code(code), 1)

    def test_alu_negative_imm8_and_imm32_across_16_32_64(self):
        cases = [
            # (label, init_rax, op_hex, want_rax)
            ("add ax, -1 (66 83)", 0xAAAA_BBBB_CCCC_0005, "6683c0ff", 0xAAAA_BBBB_CCCC_0004),
            ("sub eax, -5 (83)", 0xDEAD_BEEF_0000_000A, "83e8fb", 0x0000_0000_0000_000F),
            ("add rax, -1 (48 83)", 0x0000_0001_0000_0000, "4883c0ff", 0x0000_0000_FFFF_FFFF),
            ("sub rax, -1 (48 81)", 0x0000_0000_FFFF_FFFF, "4881e8ffffffff", 0x0000_0001_0000_0000),
            ("xor rax, -1 (48 83)", 0x0123_4567_89AB_CDEF, "4883f0ff", 0xFEDC_BA98_7654_3210),
            ("or rax, -256 (48 81)", 0x0000_0000_0000_0042, "4881c800ffffff", 0xFFFF_FFFF_FFFF_FF42),
            ("add rax, -2 (48 05)", 0x0000_0001_0000_0000, "4805feffffff", 0x0000_0000_FFFF_FFFE),
        ]
        for label, init_rax, op_hex, want_rax in cases:
            with self.subTest(label=label):
                code = (
                    _imm64(0, init_rax)
                    + bytes.fromhex(op_hex)
                    + _imm64(2, want_rax)
                    + bytes.fromhex("4839d0" "0f94c0" "0fb6f8" "c3")
                )
                self.assertEqual(self.run_code(code), 1)

    def test_cmp_and_test_negative_imm8_and_imm32(self):
        # cmp rax, -1 (83 / 81 / 3d) must match 0xFFFFFFFFFFFFFFFF (ZF=1) and NOT 0xFF or 0xFFFFFFFF (ZF=0)!
        for label, val, hx, want_zf in [
            ("83 eq -1", 0xFFFFFFFFFFFFFFFF, "4883f8ff", 1),
            ("83 ne 0xff", 0x00000000000000FF, "4883f8ff", 0),
            ("81 eq -1", 0xFFFFFFFFFFFFFFFF, "4881f8ffffffff", 1),
            ("81 ne u32max", 0x00000000FFFFFFFF, "4881f8ffffffff", 0),
            ("3d eq -1", 0xFFFFFFFFFFFFFFFF, "483dffffffff", 1),
            ("3d ne u32max", 0x00000000FFFFFFFF, "483dffffffff", 0),
            ("66 83 eq 0xffff", 0x112233445566FFFF, "6683f8ff", 1),
            ("66 83 ne 0x00ff", 0x11223344556600FF, "6683f8ff", 0),
        ]:
            with self.subTest(label=label):
                code = _imm64(0, val) + bytes.fromhex(hx + "0f94c0" "0fb6f8" "c3")
                self.assertEqual(self.run_code(code), want_zf)

        # test rax, -1 (f7 /0 & a9) on high-bit-only value 1<<63 MUST yield ZF=0, SF=1!
        for label, hx in [("f7", "48f7c0ffffffff"), ("a9", "48a9ffffffff")]:
            with self.subTest(test=label):
                code = (
                    _imm64(0, 1 << 63)
                    + bytes.fromhex(hx + "0f95c1" "0f98c2" "0fb6f9" "0fb6d2" "d1e2" "09d7" "c3")
                )
                self.assertEqual(self.run_code(code), 0b11)  # NZ=1 (bit0), SF=1 (bit1)

    def test_mov_mem64_neg_imm32_push_neg1_and_movnti(self):
        # 1. mov qword ptr [rsp], -1 (48 c7 04 24 ff ff ff ff) stores full 64-bit -1!
        code_movm = (
            bytes.fromhex("4883ec10" "48c70424ffffffff")
            + _imm64(2, 0xFFFFFFFFFFFFFFFF)
            + bytes.fromhex("483b1424" "0f94c0" "0fb6f8" "4883c410" "c3")
        )
        self.assertEqual(self.run_code(code_movm), 1)

        # 2. push -1 (6a ff); pop rax -> full 64-bit -1!
        code_push = (
            bytes.fromhex("6aff" "58")
            + _imm64(2, 0xFFFFFFFFFFFFFFFF)
            + bytes.fromhex("4839d0" "0f94c0" "0fb6f8" "c3")
        )
        self.assertEqual(self.run_code(code_push), 1)

        # 3. movnti [rsp], rax (64-bit) & movnti [rsp+8], eax (32-bit)
        code_movnti = (
            bytes.fromhex("4883ec10")
            + _imm64(0, 0x0123_4567_89AB_CDEF)
            + bytes.fromhex("480fc30424" "0fc3442408")
            + bytes.fromhex("483b0424" "0f94c1" "0fb6c9" "3b442408" "0f94c2" "0fb6d2" "21d1" "89cf" "4883c410" "c3")
        )
        self.assertEqual(self.run_code(code_movnti), 1)


if __name__ == "__main__":
    unittest.main()
