"""Tests for parity/ctx_jcc NZCV preservation and rt_mkflags logic-OF / 64-bit eflags clearing.

Covers:
- rt_mkflags(kind=2, LOGIC_KIND) clearing EF_OF (bit 11) instead of leaking stale ARM NZCV.V
- rt_mkflags writing all 64 bits of CTX_EFLAGS so stale upper 32 bits from popfq do not persist
- _parity_jcc reading CTX_EFLAGS bit 2 (EF_PF) for popf/sahf/materialized/cross-block producers
  instead of stale CTX_RES
- _parity_jcc and _ctx_jcc_reg using tbz/tbnz instead of tst_i+bcond so live ARM NZCV stays
  intact for subsequent same-block flag consumers (setcc/cmov/jcc)

    python -I tests/test_x64_recomp_parity_logic_eflags.py
"""

import os
import shutil
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
import x64_recomp as xr  # noqa: E402

GUEST_VA = 0x46000000


def _find_runner():
    if getattr(xr, "CAN_RUN_MACHO", False) and hasattr(xr, "run_result") and not os.environ.get("QEMU_AARCH64"):
        return lambda code, guest_va=GUEST_VA: xr.run_result(code, guest_va)
    qemu = os.environ.get("QEMU_AARCH64") or shutil.which("qemu-system-aarch64")
    if qemu and os.path.exists(qemu):
        return lambda code, guest_va=GUEST_VA: xr.run_kernel(code, guest_va)
    return None


RUNNER = _find_runner()


class ParityLogicEflagsStaticTests(unittest.TestCase):
    def test_rt_mkflags_clears_of_for_logic_and_stores_64bit_eflags(self):
        a = xr.Asm()
        xr.emit_rt(a, 0x47000000, bare_metal=False)
        # CSEL W3, WZR, W3, EQ (sf=0, rm=3, cond=EQ=0, rn=31, rd=3): 0x1A8303E3
        csel_w3_wzr_w3_eq = 0x1A800000 | (3 << 16) | (xr.COND["eq"] << 12) | (31 << 5) | 3
        # Must appear twice in rt_mkflags: once for OF (kind==2) and once for CF (kind==2).
        self.assertGreaterEqual(a.words.count(csel_w3_wzr_w3_eq), 2)
        # 64-bit STR X2, [CTX_REG, #0]: 0xF9000000 | (CTX_REG << 5) | 2
        str_x2_eflags = 0xF9000000 | (xr.CTX_REG << 5) | 2
        str_w2_eflags = 0xB9000000 | (xr.CTX_REG << 5) | 2
        self.assertIn(str_x2_eflags, a.words)
        self.assertNotIn(str_w2_eflags, a.words)

    def test_parity_jcc_uses_eflags_after_popf_and_never_clobbers_nzcv(self):
        # popfq; setp al; ret -> must read CTX_EFLAGS into T2 (not T0, so cmov src stays intact)
        # and branch via tbnz (bit 2) with no TST clobbering NZCV.
        code = bytes.fromhex("9d0f9ac0c3")
        insns = list(xr.disasm(code, base=GUEST_VA))
        ft = xr.FunctionTranslator(insns, GUEST_VA)
        ft.translate()
        self.assertEqual(ft.stats["unhandled"], [])
        ldr_eflags = 0xB9400000 | (xr.CTX_REG << 5) | xr.T2
        self.assertIn(ldr_eflags, ft.asm.words)
        # No ANDS/TST immediate on T2 or T0
        tst_words = [w for w in ft.asm.words if (w & 0xFF80001F) == 0x7200001F]
        self.assertEqual(tst_words, [])
        # Must use TBNZ T2, #2 (opcode 0x37000000 | (2 << 19) | T2)
        tbnz_bit2 = [w for w in ft.asm.words if (w & 0xFFF8001F) == (0x37000000 | (2 << 19) | xr.T2)]
        self.assertEqual(len(tbnz_bit2), 1)

        # Same-block cmp eax, ebx; setp dl; sete cl; ret -> must use TBZ T2, #0 (no TST clobbering NZCV)
        code2 = bytes.fromhex("39d80f9ac20f94c1c3")
        ft2 = xr.FunctionTranslator(list(xr.disasm(code2, base=GUEST_VA)), GUEST_VA)
        ft2.translate()
        self.assertEqual(ft2.stats["unhandled"], [])
        tst_words2 = [w for w in ft2.asm.words if (w & 0xFF80001F) == 0x7200001F]
        self.assertEqual(tst_words2, [])
        tbz_bit0 = [w for w in ft2.asm.words if (w & 0xFFF8001F) == (0x36000000 | xr.T2)]
        self.assertEqual(len(tbz_bit0), 1)


@unittest.skipIf(RUNNER is None, "no arm64 runner available")
class ParityLogicEflagsExecTests(unittest.TestCase):
    def _run(self, code):
        val, ft = RUNNER(code)
        self.assertEqual(ft.stats["unhandled"], [])
        return val

    def test_exec_logic_ops_clear_of_across_block_boundary(self):
        # Pack (jo<<0) | (jno<<1) | (jl<<2) | (jge<<3) | (jle<<4) | (jg<<5) into edi
        # after an unconditional jump forces materialization through rt_mkflags(2).
        def _run_logic_after_overflow(logic_bytes):
            return self._run(
                bytes.fromhex(
                    "b8ffffff7f"      # mov eax, 0x7fffffff
                    "83c001"          # add eax, 1  -> eax=0x80000000, ARM NZCV.V=1 (OF=1)
                )
                + logic_bytes
                + bytes.fromhex(
                    "eb00"            # jmp +0 -> block boundary forces rt_mkflags(2)
                    "410f90c0"        # seto  r8b
                    "410f91c1"        # setno r9b
                    "410f9cc2"        # setl  r10b
                    "410f9dc3"        # setge r11b
                    "410f9ec4"        # setle r12b
                    "410f9fc5"        # setg  r13b
                    "450fb6f8"        # movzx r15d, r8b
                    "450fb6f1"        # movzx r14d, r9b
                    "41d1e6"          # shl r14d, 1
                    "4509f7"          # or r15d, r14d
                    "450fb6f2"        # movzx r14d, r10b
                    "41c1e602"        # shl r14d, 2
                    "4509f7"          # or r15d, r14d
                    "450fb6f3"        # movzx r14d, r11b
                    "41c1e603"        # shl r14d, 3
                    "4509f7"          # or r15d, r14d
                    "450fb6f4"        # movzx r14d, r12b
                    "41c1e604"        # shl r14d, 4
                    "4509f7"          # or r15d, r14d
                    "450fb6f5"        # movzx r14d, r13b
                    "41c1e605"        # shl r14d, 5
                    "4509f7"          # or r15d, r14d
                    "4489ff"          # mov edi, r15d
                    "c3"              # ret
                )
            )

        # xor eax, eax -> SF=0, ZF=1, OF=0 => jo=0, jno=1, jl=0, jge=1, jle=1, jg=0 => 0b011010 = 0x1A
        self.assertEqual(_run_logic_after_overflow(bytes.fromhex("31c0")), 0x1A)
        # and eax, eax (eax=0x80000000) -> SF=1, ZF=0, OF=0 => jo=0, jno=1, jl=1, jge=0, jle=1, jg=0 => 0b010110 = 0x16
        self.assertEqual(_run_logic_after_overflow(bytes.fromhex("21c0")), 0x16)
        # or eax, eax (eax=0x80000000) -> SF=1, ZF=0, OF=0 => 0x16
        self.assertEqual(_run_logic_after_overflow(bytes.fromhex("09c0")), 0x16)
        # test eax, eax (eax=0x80000000) -> SF=1, ZF=0, OF=0 => 0x16
        self.assertEqual(_run_logic_after_overflow(bytes.fromhex("85c0")), 0x16)

    def test_exec_parity_after_popf_and_cross_block_reads_eflags(self):
        # Poison CTX_RES with odd parity (1), then popfq with PF=1 (0x04):
        # jp/setp/cmovp must all observe PF=1 (even), jnp/setnp/cmovnp must observe 0.
        code_pf1 = bytes.fromhex(
            "41b800000000"    # mov r8d, 0
            "41b900000000"    # mov r9d, 0
            "41ba00000000"    # mov r10d, 0
            "b900000000"      # mov ecx, 0
            "ba01000000"      # mov edx, 1
            "b801000000"      # mov eax, 1
            "83c000"          # add eax, 0     (CTX_RES := 1, odd parity!)
            "6a04"            # push 0x04      (EF_PF = 1)
            "9d"              # popfq
            "410f9ac0"        # setp  r8b      (1)
            "410f9bc1"        # setnp r9b      (0)
            "0f4aca"          # cmovp ecx, edx (1)
            "7a02"            # jp +2          (taken -> mov r10b, 1)
            "eb03"            # jmp +3         (skip mov r10b, 1)
            "41b201"          # mov r10b, 1
            "4489c7"          # mov edi, r8d
            "d1e1"            # shl ecx, 1
            "09cf"            # or edi, ecx
            "41c1e202"        # shl r10d, 2
            "4409d7"          # or edi, r10d
            "41c1e103"        # shl r9d, 3
            "4409cf"          # or edi, r9d
            "c3"
        )
        # Expected: setp=1 (bit0), cmovp=1 (bit1), jp=1 (bit2), setnp=0 (bit3) -> 0b0111 = 7
        self.assertEqual(self._run(code_pf1), 7)

        # Poison CTX_RES with even parity (0), then popfq with PF=0 (0x00) across a jump:
        code_pf0 = bytes.fromhex(
            "41b800000000"    # mov r8d, 0
            "41b900000000"    # mov r9d, 0
            "31c0"            # xor eax, eax   (CTX_RES := 0, even parity!)
            "6a00"            # push 0x00      (EF_PF = 0)
            "9d"              # popfq
            "eb00"            # jmp +0         (cross-block)
            "410f9ac0"        # setp  r8b      (0)
            "410f9bc1"        # setnp r9b      (1)
            "4489c7"          # mov edi, r8d
            "41d1e1"          # shl r9d, 1
            "4409cf"          # or edi, r9d
            "c3"
        )
        # Expected: setp=0 (bit0), setnp=1 (bit1) -> 2
        self.assertEqual(self._run(code_pf0), 2)

    def test_exec_parity_check_preserves_live_arm_nzcv_for_following_cc(self):
        # Single basic block: cmp 1, 2 -> res=-1 (0xFF even parity PF=1), SF=1, ZF=0, CF=1, OF=0.
        # setp runs FIRST; previously _parity_jcc's tst_i clobbered NZCV.N=0 and NZCV.Z=1,
        # corrupting the subsequent sete, sets, and setl in the same block.
        code_lt = bytes.fromhex(
            "b801000000"      # mov eax, 1
            "bb02000000"      # mov ebx, 2
            "39d8"            # cmp eax, ebx   (1 - 2 = -1: PF=1, SF=1, ZF=0, CF=1, OF=0)
            "410f9ac0"        # setp r8b       (1)
            "410f94c1"        # sete r9b       (0)
            "410f98c2"        # sets r10b      (1)
            "410f92c3"        # setb r11b      (1)
            "410f9cc4"        # setl r12b      (1)
            "450fb6f8"        # movzx r15d, r8b
            "450fb6f1"        # movzx r14d, r9b
            "41d1e6"          # shl r14d, 1
            "4509f7"          # or r15d, r14d
            "450fb6f2"        # movzx r14d, r10b
            "41c1e602"        # shl r14d, 2
            "4509f7"          # or r15d, r14d
            "450fb6f3"        # movzx r14d, r11b
            "41c1e603"        # shl r14d, 3
            "4509f7"          # or r15d, r14d
            "450fb6f4"        # movzx r14d, r12b
            "41c1e604"        # shl r14d, 4
            "4509f7"          # or r15d, r14d
            "4489ff"          # mov edi, r15d
            "c3"
        )
        # Expected: setp=1, sete=0, sets=1, setb=1, setl=1 -> 0b11101 = 0x1D
        self.assertEqual(self._run(code_lt), 0x1D)

        # Not-taken jp followed immediately by js in fall-through:
        # test 0x80000001, 0x80000001 -> low byte 0x01 (odd parity PF=0), SF=1, ZF=0.
        code_jp_js = bytes.fromhex(
            "b801000080"      # 0:  mov eax, 0x80000001
            "85c0"            # 5:  test eax, eax
            "7a08"            # 7:  jp 17  (9 + 8 = 17)
            "780c"            # 9:  js 23  (11 + 12 = 23)
            "bf00000000"      # 11: mov edi, 0
            "c3"              # 16: ret
            "bf02000000"      # 17: mov edi, 2 (bad)
            "c3"              # 22: ret
            "bf01000000"      # 23: mov edi, 1 (good)
            "c3"              # 28: ret
        )
        self.assertEqual(self._run(code_jp_js), 1)

    def test_exec_rt_mkflags_clears_upper_32_bits_of_ctx_eflags(self):
        # Seed upper 32 bits of CTX_EFLAGS via popfq, then materialize via rt_mkflags and inspect high 32 bits.
        code = (
            b"\x48\xb8" + struct.pack("<Q", 0x1234567800000002)  # mov rax, 0x1234567800000002
            + bytes.fromhex(
                "50"          # push rax
                "9d"          # popfq
                "31c0"        # xor eax, eax
                "eb00"        # jmp +0 -> forces rt_mkflags(2)
                "9c"          # pushfq
                "58"          # pop rax
                "48c1e820"    # shr rax, 32
                "89c7"        # mov edi, eax
                "c3"          # ret
            )
        )
        self.assertEqual(self._run(code), 0)

    def test_exec_cmov_via_ctx_and_parity_preserves_src_register(self):
        # Cross-block cmovl (reads CTX_EFLAGS via _ctx_jcc) previously overwrote T0 (holding src)
        # with ctx.eflags before moving T0 into dst.
        code_cross_cmov = bytes.fromhex(
            "b801000000"      # mov eax, 1
            "bb02000000"      # mov ebx, 2
            "bf11111111"      # mov edi, 0x11111111
            "baefbeadde"      # mov edx, 0xdeadbeef
            "39d8"            # cmp eax, ebx  (1 < 2 -> SF=1, OF=0 -> cmovl taken)
            "eb00"            # jmp +0        (block boundary -> _ctx_jcc)
            "0f4cfa"          # cmovl edi, edx
            "c3"
        )
        self.assertEqual(self._run(code_cross_cmov), 0xDEADBEEF)

        # Same-block cmovp (reads parity via _parity_jcc) previously overwrote T0 (holding src)
        # with folded CTX_RES before moving T0 into dst.
        code_cmovp = bytes.fromhex(
            "b803000000"      # mov eax, 3
            "85c0"            # test eax, eax (0x03 -> 2 bits set -> PF=1 even)
            "bf22222222"      # mov edi, 0x22222222
            "babebafeca"      # mov edx, 0xcafebabe
            "0f4afa"          # cmovp edi, edx
            "c3"
        )
        self.assertEqual(self._run(code_cmovp), 0xCAFEBABE)


if __name__ == "__main__":
    unittest.main()
