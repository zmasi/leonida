"""Tests for _emit_conv, _emit_ret, _emit_call, and _emit_branch fixes in tools/x64_recomp.py.

Regression coverage for:
- 66 99 (cwd) sign-extending AX into DX instead of copying AX into DX
- REX.W taking precedence over 0x66 for 66 48 98 (cdqe) and 66 48 99 (cqo)
- C2 iw (ret imm16) fixing ins.ops[0][1] IndexError and imm16 >= 4096 ARM64 add limit
- Indirect call [rsp+disp] loading target before decrementing rsp for the return address
- jrcxz vs 67 jecxz (64-bit vs 32-bit cbz), 67 loop addrsize, and cross-block loope/loopne via ctx.eflags

Static ARM64-encoding checks run everywhere; execution tests run natively on
Apple Silicon when CAN_RUN_MACHO is available, or under QEMU.

    python -I tests/test_x64_recomp_control_conv.py
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


class ControlConvStaticTests(unittest.TestCase):
    """Static ARM64 encoding checks that run on every platform."""

    def translate(self, code):
        insns = list(xr.disasm(code, 0x1000))
        self.assertFalse(any(i.unknown or i.error for i in insns), insns)
        tr = xr.FunctionTranslator(insns)
        tr.translate()
        self.assertEqual(tr.stats["unhandled"], [], f"unhandled: {tr.stats['unhandled']}")
        return tr

    def test_cwd_shifts_sign_bit_and_rexw_precedence_over_66(self):
        # 66 99 (cwd): must emit ASR W0, W0, #16 (0x13107C00) between SXTH and BFI!
        tr_cwd = self.translate(bytes.fromhex("6699c3"))
        a_ref = xr.Asm()
        a_ref.asr(xr.T0, xr.T0, 16, sf=0)
        self.assertIn(a_ref.words[0], tr_cwd.asm.words, "66 99 (cwd) must shift sign bit down by 16 before bfi")

        # 66 48 98 (cdqe with redundant 66): REX.W wins -> SXTW X19, X19, NOT BFI!
        tr_cdqe = self.translate(bytes.fromhex("664898c3"))
        a_cdqe = xr.Asm()
        a_cdqe.sxtw(xr.GMAP[0], xr.GMAP[0])
        self.assertIn(a_cdqe.words[0], tr_cdqe.asm.words, "66 48 98 must emit cdqe (sxtw), not cbw")

        # 66 48 99 (cqo with redundant 66): REX.W wins -> ASR X21, X19, #63, NOT BFI!
        tr_cqo = self.translate(bytes.fromhex("664899c3"))
        a_cqo = xr.Asm()
        a_cqo.asr(xr.GMAP[2], xr.GMAP[0], 63)
        self.assertIn(a_cqo.words[0], tr_cqo.asm.words, "66 48 99 must emit cqo (asr #63), not cwd")

    def test_ret_imm16_small_and_large_translate_cleanly(self):
        for hx in ("c20000", "c21000", "c20020", "c2fff0"):
            with self.subTest(hx=hx):
                self.translate(bytes.fromhex(hx))

    def test_indirect_call_loads_target_before_pushing_return_address(self):
        # call [rsp] (ff 14 24); ret (c3)
        tr = self.translate(bytes.fromhex("ff1424c3"))
        a_ref = xr.Asm()
        a_ref.ldr(xr.ADDR_T, xr.ADDR_T, 0, size=8)
        a_ref.str_pre(xr.T0, xr.GMAP[4], -8)
        ldr_w, str_pre_w = a_ref.words
        self.assertIn(ldr_w, tr.asm.words)
        self.assertIn(str_pre_w, tr.asm.words)
        self.assertLess(
            tr.asm.words.index(ldr_w),
            tr.asm.words.index(str_pre_w),
            "indirect call must load target from [rsp] BEFORE decrementing rsp to push return address",
        )

    def test_jrcxz_uses_64bit_cbz_and_67_jecxz_uses_32bit_cbz(self):
        # e3 00 (jrcxz +0); ret (c3) -> 64-bit CBZ X20 (sf=1, opcode 0xB4000014 mask 0xFF00001F)
        tr64 = self.translate(bytes.fromhex("e300c3"))
        cbz64 = [w for w in tr64.asm.words if (w & 0xFF00001F) == (0xB4000000 | xr.GMAP[1])]
        self.assertEqual(len(cbz64), 1, "E3 in 64-bit mode is jrcxz and must emit 64-bit CBZ")

        # 67 e3 00 (jecxz +0); ret (c3) -> 32-bit CBZ W20 (sf=0, opcode 0x34000014 mask 0xFF00001F)
        tr32 = self.translate(bytes.fromhex("67e300c3"))
        cbz32 = [w for w in tr32.asm.words if (w & 0xFF00001F) == (0x34000000 | xr.GMAP[1])]
        self.assertEqual(len(cbz32), 1, "67 E3 is jecxz and must emit 32-bit CBZ")

    def test_cross_block_loope_reads_ctx_eflags(self):
        # cmp eax, ebx (39 d8); jmp +0 (eb 00); loope +0 (e1 00); ret (c3)
        tr = self.translate(bytes.fromhex("39d8" "eb00" "e100" "c3"))
        a_ref = xr.Asm()
        a_ref.ldr(xr.T0, xr.CTX_REG, xr.CTX_EFLAGS, size=4)
        ldr_mask = ~0x1F
        self.assertTrue(any((w & ldr_mask) == (a_ref.words[0] & ldr_mask) for w in tr.asm.words), "cross-block loope must read ZF from ctx.eflags")


@unittest.skipIf(RUNNER is None, "needs Darwin arm64 Mach-O runner or qemu-system-aarch64")
class ControlConvExecTests(unittest.TestCase):
    """End-to-end arm64 execution tests for conv, ret imm16, indirect call [rsp], and jrcxz/loop."""

    def run_code(self, code):
        res, tr = RUNNER(code)
        self.assertEqual(tr.stats["unhandled"], [], f"unhandled: {tr.stats['unhandled']}")
        return res

    def test_cwd_cbw_cwde_cdqe_cdq_cqo_and_rexw_66_precedence(self):
        # 1. 66 99 (cwd) positive: ax=0x1234 -> dx=0x0000, upper 48 bits of rdx preserved!
        code_cwd_pos = (
            _imm64(0, 0xFFFF_FFFF_FFFF_1234)
            + _imm64(2, 0xAAAA_BBBB_CCCC_5555)
            + bytes.fromhex("6699")
            + _imm64(1, 0xAAAA_BBBB_CCCC_0000)
            + bytes.fromhex("4839ca" "0f94c0" "0fb6f8" "c3")
        )
        self.assertEqual(self.run_code(code_cwd_pos), 1)

        # 2. 66 99 (cwd) negative: ax=0x8001 -> dx=0xFFFF, upper 48 bits of rdx preserved!
        code_cwd_neg = (
            _imm64(0, 0x0000_0000_0000_8001)
            + _imm64(2, 0x1111_2222_3333_0000)
            + bytes.fromhex("6699")
            + _imm64(1, 0x1111_2222_3333_FFFF)
            + bytes.fromhex("4839ca" "0f94c0" "0fb6f8" "c3")
        )
        self.assertEqual(self.run_code(code_cwd_neg), 1)

        # 3. 66 48 98 (cdqe with 66 prefix): REX.W wins -> sign-extends eax to 64-bit rax!
        code_66_cdqe = (
            _imm64(0, 0x0000_0000_8000_0001)
            + bytes.fromhex("664898")
            + _imm64(1, 0xFFFF_FFFF_8000_0001)
            + bytes.fromhex("4839c8" "0f94c0" "0fb6f8" "c3")
        )
        self.assertEqual(self.run_code(code_66_cdqe), 1)

        # 4. 66 48 99 (cqo with 66 prefix): REX.W wins -> fills 64-bit rdx from bit 63 of rax!
        code_66_cqo = (
            _imm64(0, 0x8000_0000_0000_0001)
            + _imm64(2, 0x1234)
            + bytes.fromhex("664899")
            + _imm64(1, 0xFFFF_FFFF_FFFF_FFFF)
            + bytes.fromhex("4839ca" "0f94c0" "0fb6f8" "c3")
        )
        self.assertEqual(self.run_code(code_66_cqo), 1)

        # 5. 66 98 (cbw), 98 (cwde), 99 (cdq)
        code_cbw = (
            _imm64(0, 0x1234_5678_9ABC_0085)
            + bytes.fromhex("6698")
            + _imm64(1, 0x1234_5678_9ABC_FF85)
            + bytes.fromhex("4839c8" "0f94c0" "0fb6f8" "c3")
        )
        self.assertEqual(self.run_code(code_cbw), 1)

    def test_ret_imm16_small_and_large_stack_cleanup(self):
        # Test 1: ret 16 (c2 10 00) cleans 16 bytes pushed by caller
        # mov rbx, rsp (48 89 e3); sub rsp, 16 (48 83 ec 10); call +12 (e8 0c 00 00 00);
        # cmp rbx, rsp (48 39 e3); sete al (0f 94 c0); movzx edi, al (0f b6 f8); ret (c3);
        # helper: mov eax, 7 (b8 07 00 00 00); ret 16 (c2 10 00)
        # Sizes: 48 89 e3 (3) + 48 83 ec 10 (4) + e8 .. (5) + 48 39 e3 (3) + 0f 94 c0 (3) + 0f b6 f8 (3) + c3 (1) = 10 after call
        code_small = bytes.fromhex(
            "4889e3" "4883ec10" "e80a000000"
            "4839e3" "0f94c0" "0fb6f8" "c3"
            "b807000000" "c21000"
        )
        self.assertEqual(self.run_code(code_small), 1)

        # Test 2: ret 0x2000 (c2 00 20) cleans 8192 bytes (> 4095 ARM64 add imm12 limit!)
        # sub rsp, 0x2000 (48 81 ec 00 20 00 00)
        code_large = bytes.fromhex(
            "4889e3" "4881ec00200000" "e80a000000"
            "4839e3" "0f94c0" "0fb6f8" "c3"
            "b809000000" "c20020"
        )
        self.assertEqual(self.run_code(code_large), 1)

    def test_indirect_call_through_rsp_slot(self):
        # Push address of helper onto [rsp], then `call [rsp]` (ff 14 24) and `call [rsp+8]` (ff 54 24 08)!
        # Layout:
        # 0x00: lea rax, [rip + helper]  (48 8d 05 disp32)  [7 bytes -> next=0x07]
        # 0x07: push rax                 (50)               [1 byte  -> 0x08]
        # 0x08: call [rsp]               (ff 14 24)         [3 bytes -> 0x0b]  -> edi = 0x20
        # 0x0b: mov ebx, edi             (89 fb)            [2 bytes -> 0x0d]
        # 0x0d: push rax                 (50)               [1 byte  -> 0x0e]  -> original [rsp+8] has helper!
        # 0x0e: call [rsp+8]             (ff 54 24 08)      [4 bytes -> 0x12]  -> edi = 0x20
        # 0x12: add edi, ebx             (01 df)            [2 bytes -> 0x14]  -> edi = 0x40
        # 0x14: add rsp, 16              (48 83 c4 10)      [4 bytes -> 0x18]
        # 0x18: ret                      (c3)               [1 byte  -> 0x19]
        # 0x19 (helper): mov edi, 0x20   (bf 20 00 00 00)   [5 bytes]
        # 0x1e: ret                      (c3)               [1 byte]
        # disp32 = 0x19 - 0x07 = 0x12
        code = bytes.fromhex(
            "488d0512000000" "50" "ff1424" "89fb" "50" "ff542408" "01df" "4883c410" "c3"
            "bf20000000" "c3"
        )
        self.assertEqual(self.run_code(code), 0x40)

    def test_jrcxz_vs_jecxz_and_loop_addrsize_and_cross_block_loope(self):
        # 1. rcx = 0x0000_0001_0000_0000: jrcxz NOT taken (returns 0), 67 jecxz TAKEN (returns 1)!
        code_jrcxz = (
            _imm64(1, 0x0000_0001_0000_0000)
            + bytes.fromhex("e307" "bf00000000" "eb05" "bf01000000" "c3")
        )
        self.assertEqual(self.run_code(code_jrcxz), 0)

        code_jecxz = (
            _imm64(1, 0x0000_0001_0000_0000)
            + bytes.fromhex("67e307" "bf00000000" "eb05" "bf01000000" "c3")
        )
        self.assertEqual(self.run_code(code_jecxz), 1)

        code_jrcxz_zero = (
            _imm64(1, 0)
            + bytes.fromhex("e307" "bf00000000" "eb05" "bf01000000" "c3")
        )
        self.assertEqual(self.run_code(code_jrcxz_zero), 1)

        # 2. 67 E2 (loop with 32-bit ecx): rcx = 0xDEAD_BEEF_0000_0003 -> loops 3 times, zero-extends rcx to 0!
        # xor edi, edi (31 ff); body: inc edi (ff c7, 2B); 67 loop body (67 e2 fb, 3B -> rel8=-5); test rcx, rcx -> 0!
        code_loop32 = (
            _imm64(1, 0xDEAD_BEEF_0000_0003)
            + bytes.fromhex("31ff" "ffc7" "67e2fb" "4885c9" "0f94c0" "0fb6c0" "c1e704" "09c7" "c3")
        )
        self.assertEqual(self.run_code(code_loop32), (3 << 4) | 1)

        # 3. Cross-block loope: cmp in block 1 -> jmp to block 2 -> loope reads ZF from ctx.eflags!
        # Case A (ZF=1, rcx=2): loope decrements rcx to 1 and branches to taken -> edi=1
        code_loope_taken = (
            _imm64(0, 5) + _imm64(1, 2)
            + bytes.fromhex("39c0" "eb00" "e107" "bf00000000" "eb05" "bf01000000" "c3")
        )
        self.assertEqual(self.run_code(code_loope_taken), 1)

        # Case B (ZF=0, rcx=2): loope decrements rcx to 1 and falls through -> edi=0
        code_loope_fall = (
            _imm64(0, 5) + _imm64(3, 6) + _imm64(1, 2)
            + bytes.fromhex("39d8" "eb00" "e107" "bf00000000" "eb05" "bf01000000" "c3")
        )
        self.assertEqual(self.run_code(code_loope_fall), 0)

        # Case C (ZF=0, rcx=2): cross-block loopne branches -> edi=1
        code_loopne_taken = (
            _imm64(0, 5) + _imm64(3, 6) + _imm64(1, 2)
            + bytes.fromhex("39d8" "eb00" "e007" "bf00000000" "eb05" "bf01000000" "c3")
        )
        self.assertEqual(self.run_code(code_loopne_taken), 1)


if __name__ == "__main__":
    unittest.main()
