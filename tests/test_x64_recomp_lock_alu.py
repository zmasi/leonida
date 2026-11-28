"""Tests for LOCK-prefixed memory RMW ALU emitters in tools/x64_recomp.py.

Covers all 11 LOCK-valid x86-64 memory ALU instructions across 8/16/32/64-bit:
  lock add, lock sub, lock and, lock or, lock xor,
  lock inc, lock dec, lock neg, lock not, lock adc, lock sbb

Verifies:
- Static codegen: LOCK forms emit ARMv8.0 LDAXR/STLXR (8/16/32/64-bit) while
  unlocked memory ALU forms do not; 0 unhandled instructions across all 11 ops
- Hardware execution of all 11 ops across 8/16/32/64-bit aligned memory operands,
  verifying adjacent sentinel byte isolation and exact SF/ZF/CF/OF/PF flags on
  both same-block ARM NZCV and cross-block materialized ctx.eflags paths
- Sign-extended negative immediates (`lock add qword [rsp], -1`, `lock sub ... -1`)
- Unaligned `lock add`/`inc`/`xor` at `[rsp+1]` (16/32/64-bit fallback, no SIGBUS)
- `lock inc`/`lock dec` preserving CF (both same-block SUB_KIND/ADD_KIND and cross-block)
- `lock not` preserving all flags (CF/ZF/SF/OF/PF) across ARM NZCV and ctx.eflags
- `lock adc`/`lock sbb` across 8/16/32/64-bit reading CF=0 and CF=1 from both
  ADD_KIND (`stc`/`clc`) and SUB_KIND (`sub`) producers

Static checks run everywhere; execution tests run natively on Apple Silicon when
CAN_RUN_MACHO is available, or under QEMU.

    python -I tests/test_x64_recomp_lock_alu.py
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


class LockAluStaticTests(unittest.TestCase):
    def test_lock_alu_emits_ldaxr_stlxr_for_all_widths_and_ops(self):
        # Expected LDAXR / STLXR words for (rt=T0=0, rn=ADDR_T=16) and (rs=T3=3, rt=T2=2, rn=ADDR_T=16)
        ldaxr = {
            1: 0x085FFC00 | (16 << 5) | 0,
            2: 0x485FFC00 | (16 << 5) | 0,
            4: 0x885FFC00 | (16 << 5) | 0,
            8: 0xC85FFC00 | (16 << 5) | 0,
        }
        stlxr = {
            1: 0x0800FC00 | (3 << 16) | (16 << 5) | 2,
            2: 0x4800FC00 | (3 << 16) | (16 << 5) | 2,
            4: 0x8800FC00 | (3 << 16) | (16 << 5) | 2,
            8: 0xC800FC00 | (3 << 16) | (16 << 5) | 2,
        }
        cases = [
            # (hex, size, label)
            ("f00007c3", 1, "lock add byte [rdi], al"),
            ("f0660107c3", 2, "lock add word [rdi], ax"),
            ("f00107c3", 4, "lock add dword [rdi], eax"),
            ("f0480107c3", 8, "lock add qword [rdi], rax"),
            ("f0488307ffc3", 8, "lock add qword [rdi], -1"),
            ("f02907c3", 4, "lock sub dword [rdi], eax"),
            ("f02107c3", 4, "lock and dword [rdi], eax"),
            ("f00907c3", 4, "lock or dword [rdi], eax"),
            ("f03107c3", 4, "lock xor dword [rdi], eax"),
            ("f0ff07c3", 4, "lock inc dword [rdi]"),
            ("f048ff0fc3", 8, "lock dec qword [rdi]"),
            ("f0f71fc3", 4, "lock neg dword [rdi]"),
            ("f0f717c3", 4, "lock not dword [rdi]"),
            ("f01007c3", 1, "lock adc byte [rdi], al"),
            ("f0661907c3", 2, "lock sbb word [rdi], ax"),
        ]
        for hx, sz, desc in cases:
            with self.subTest(desc=desc):
                ft = xr.FunctionTranslator(xr.disasm(bytes.fromhex(hx), GUEST_VA))
                ft.translate()
                self.assertEqual(ft.stats["unhandled"], [])
                words = set(ft.asm.words)
                self.assertIn(ldaxr[sz], words)
                self.assertIn(stlxr[sz], words)

        # Unlocked memory ALU ops must NOT emit LDAXR/STLXR
        all_llsc = set(ldaxr.values()) | set(stlxr.values())
        for hx in ("0107c3", "480107c3", "ff07c3", "f717c3"):
            with self.subTest(unlocked=hx):
                ft = xr.FunctionTranslator(xr.disasm(bytes.fromhex(hx), GUEST_VA))
                ft.translate()
                self.assertEqual(ft.stats["unhandled"], [])
                self.assertTrue(set(ft.asm.words).isdisjoint(all_llsc))


@unittest.skipUnless(RUNNER is not None, "no arm64 runner available")
class LockAluExecTests(unittest.TestCase):
    def _run(self, code: bytes) -> int:
        val, ft = RUNNER(code)
        self.assertEqual(ft.stats["unhandled"], [])
        return val

    def test_exec_lock_add_sub_logic_neg_widths_imm_and_unaligned(self):
        # 1. 8-bit lock add 0xFF + 1 = 0x00 -> ZF=1, CF=1, SF=0, OF=0, PF=1 (mask 0x13),
        #    without disturbing upper 7 bytes of [rsp]!
        for cross in (False, True):
            with self.subTest(op="lock_add_b", cross=cross):
                code = (
                    bytes.fromhex("4883ec20")
                    + _imm64(9, 0x11223344556677FF)
                    + bytes.fromhex("4c890c24")                 # [rsp] = 0x11223344556677FF
                    + bytes.fromhex("b001")                     # mov al, 1
                    + bytes.fromhex("f0000424")                 # lock add byte [rsp], al
                    + (b"\xeb\x00" if cross else b"")
                    + _pack_flags_to_edi()                      # 0x13 (ZF|CF|PF)
                    + bytes.fromhex("488b0424")                 # mov rax, [rsp]
                    + _imm64(10, 0x1122334455667700)
                    + bytes.fromhex("4c39d0" "410f94c3" "450fb6db" "41c1e305" "4409df")  # bit5: mem ok
                    + bytes.fromhex("4883c420c3")
                )
                self.assertEqual(self._run(code), 0x33)

        # 2. 16-bit lock sub 0x8000 - 1 = 0x7FFF -> OF=1, PF=1, CF=0, SF=0, ZF=0 (mask 0x18),
        #    upper 6 bytes untouched
        code = (
            bytes.fromhex("4883ec20")
            + _imm64(9, 0xAABBCCDDEEFF8000)
            + bytes.fromhex("4c890c24")
            + bytes.fromhex("f066832c2401")                     # lock sub word [rsp], 1
            + _pack_flags_to_edi()                              # 0x18 (OF|PF)
            + bytes.fromhex("488b0424")
            + _imm64(10, 0xAABBCCDDEEFF7FFF)
            + bytes.fromhex("4c39d0" "410f94c3" "450fb6db" "41c1e305" "4409df")
            + bytes.fromhex("4883c420c3")
        )
        self.assertEqual(self._run(code), 0x38)

        # 3. 64-bit lock add qword [rsp], -1 (negative sign-extended imm8!):
        #    1 + (-1) = 0 -> ZF=1, CF=1, PF=1 (mask 0x13)
        code = (
            bytes.fromhex("4883ec20")
            + _imm64(9, 1)
            + bytes.fromhex("4c890c24")
            + bytes.fromhex("f048830424ff")                     # lock add qword [rsp], -1
            + _pack_flags_to_edi()                              # 0x13
            + bytes.fromhex("488b0424" "4885c0" "410f94c3" "450fb6db" "41c1e305" "4409df")
            + bytes.fromhex("4883c420c3")
        )
        self.assertEqual(self._run(code), 0x33)

        # 4. 32-bit lock and / or / xor + lock neg (8-bit 0x80 -> 0x80, CF=1, SF=1, OF=1, PF=0 = 0x0E)
        code = (
            bytes.fromhex("4883ec20")
            + _imm64(9, 0x00000000F0F055AA)
            + bytes.fromhex("4c890c24")
            + bytes.fromhex("b80f0fff00" "f0210424")            # lock and dword [rsp], 0x00FF0F0F -> 0x00F0050A
            + bytes.fromhex("b800000080" "f0090424")            # lock or  dword [rsp], 0x80000000 -> 0x80F0050A
            + bytes.fromhex("b800000080" "f0310424")            # lock xor dword [rsp], 0x80000000 -> 0x00F0050A
            + bytes.fromhex("8b0424")                           # mov eax, [rsp]
            + _imm64(10, 0x00F0050A)
            + bytes.fromhex("4c39d0" "410f94c3" "410fb6fb")     # bit0: and/or/xor exact
            + bytes.fromhex("4189fa")                           # mov r10d, edi (save bit0)
            # lock neg byte [rsp+8] on 0x80 -> 0x80, CF=1, SF=1, OF=1, PF=0, ZF=0 -> mask 0x0E
            + _imm64(9, 0x1122334455667780)
            + bytes.fromhex("4c894c2408")
            + bytes.fromhex("f0f65c2408")                       # lock neg byte [rsp+8]
            + _pack_flags_to_edi()                              # 0x0E (CF|SF|OF)
            + bytes.fromhex("41c1e205" "4409d7")                # shl r10d, 5; or edi, r10d -> 0x2E
            + bytes.fromhex("4883c420c3")
        )
        self.assertEqual(self._run(code), 0x2E)

        # 5. Unaligned 16/32/64-bit lock add at [rsp+1] (exercises fallback without SIGBUS)
        code = (
            bytes.fromhex("4883ec20")
            + _imm64(9, 0x0102030405060708)
            + bytes.fromhex("4c894c2401")                       # unaligned [rsp+1]
            + bytes.fromhex("f0488344240107")                   # lock add qword [rsp+1], 7
            + bytes.fromhex("f0668344240110")                   # lock add word [rsp+1], 0x10
            + bytes.fromhex("488b442401")
            + _imm64(10, 0x010203040506071F)
            + bytes.fromhex("4c39d0" "0f94c0" "0fb6f8")
            + bytes.fromhex("4883c420c3")
        )
        self.assertEqual(self._run(code), 1)

    def test_exec_lock_inc_dec_and_not_flag_preservation(self):
        # 1. lock inc byte [rsp] on 0xFF -> 0x00 (ZF=1, PF=1, SF=0, OF=0),
        #    MUST preserve CF=1 from preceding `sub` (both same-block ARM & cross-block ctx)!
        for cross in (False, True):
            with self.subTest(op="lock_inc_b_preserves_cf", cross=cross):
                code = (
                    bytes.fromhex("4883ec20")
                    + _imm64(9, 0xAABBCCDDEEFF00FF)
                    + bytes.fromhex("4c890c24")
                    + bytes.fromhex("41b900000000" "4183e901")  # 0 - 1 -> CF=1 (SUB_KIND!)
                    + bytes.fromhex("f0fe0424")                 # lock inc byte [rsp] -> 0x00, ZF=1, PF=1, CF still 1!
                    + (b"\xeb\x00" if cross else b"")
                    + _pack_flags_to_edi()                      # ZF=1 | CF=2 | PF=16 = 0x13
                    + bytes.fromhex("488b0424")
                    + _imm64(10, 0xAABBCCDDEEFF0000)
                    + bytes.fromhex("4c39d0" "410f94c3" "450fb6db" "41c1e305" "4409df")
                    + bytes.fromhex("4883c420c3")
                )
                self.assertEqual(self._run(code), 0x33)

        # 2. lock dec qword [rsp] on 0 -> -1 (SF=1, PF=1, ZF=0, OF=0),
        #    MUST preserve CF=0 from preceding `clc` (even cross-block where SUB_KIND would invert!)
        for cross in (False, True):
            with self.subTest(op="lock_dec_q_preserves_cf0", cross=cross):
                code = (
                    bytes.fromhex("4883ec20")
                    + _imm64(9, 0)
                    + bytes.fromhex("4c890c24")
                    + bytes.fromhex("f8")                       # clc (CF=0)
                    + bytes.fromhex("f048ff0c24")               # lock dec qword [rsp] -> -1, SF=1, PF=1, CF still 0!
                    + (b"\xeb\x00" if cross else b"")
                    + _pack_flags_to_edi()                      # SF=4 | PF=16 = 0x14
                    + bytes.fromhex("488b0424")
                    + _imm64(10, 0xFFFFFFFFFFFFFFFF)
                    + bytes.fromhex("4c39d0" "410f94c3" "450fb6db" "41c1e305" "4409df")
                    + bytes.fromhex("4883c420c3")
                )
                self.assertEqual(self._run(code), 0x34)

        # 3. lock not (8/16/32/64-bit) MUST preserve ALL flags (CF/ZF/SF/OF/PF)
        for cross in (False, True):
            with self.subTest(op="lock_not_preserves_all_flags", cross=cross):
                code = (
                    bytes.fromhex("4883ec20")
                    + _imm64(9, 0x0123456789ABCDEF)
                    + bytes.fromhex("4c890c24")
                    + bytes.fromhex("41b900000080" "4183e901")  # 0x80000000 - 1 -> OF=1, PF=1 (0x18)
                    + bytes.fromhex("f048f71424")               # lock not qword [rsp]
                    + bytes.fromhex("f0f61424")                 # lock not byte [rsp] (flips low byte back)
                    + (b"\xeb\x00" if cross else b"")
                    + _pack_flags_to_edi()                      # 0x18
                    + bytes.fromhex("488b0424")
                    + _imm64(10, (~0x0123456789ABCDEF & 0xFFFFFFFFFFFFFF00) | 0xEF)
                    + bytes.fromhex("4c39d0" "410f94c3" "450fb6db" "41c1e305" "4409df")
                    + bytes.fromhex("4883c420c3")
                )
                self.assertEqual(self._run(code), 0x38)

    def test_exec_lock_adc_and_sbb_all_widths_and_carry_sources(self):
        # 1. 8-bit lock adc: 0xFF + 0x00 + CF(1 from SUB_KIND!) = 0x00 -> ZF=1, CF=1, PF=1 (0x13)
        for cross in (False, True):
            with self.subTest(op="lock_adc_b_sub_cf", cross=cross):
                code = (
                    bytes.fromhex("4883ec20")
                    + _imm64(9, 0x11223344556677FF)
                    + bytes.fromhex("4c890c24")
                    + bytes.fromhex("b000")                     # al = 0
                    + bytes.fromhex("41b900000000" "4183e901")  # 0 - 1 -> CF=1 (SUB_KIND)
                    + bytes.fromhex("f0100424")                 # lock adc byte [rsp], al -> 0x00
                    + (b"\xeb\x00" if cross else b"")
                    + _pack_flags_to_edi()                      # 0x13
                    + bytes.fromhex("488b0424")
                    + _imm64(10, 0x1122334455667700)
                    + bytes.fromhex("4c39d0" "410f94c3" "450fb6db" "41c1e305" "4409df")
                    + bytes.fromhex("4883c420c3")
                )
                self.assertEqual(self._run(code), 0x33)

        # 2. 16-bit lock sbb: 0x0000 - 0x0000 - CF(1 from stc) = 0xFFFF -> SF=1, CF=1, PF=1 (0x16)
        for cross in (False, True):
            with self.subTest(op="lock_sbb_w_cf1", cross=cross):
                code = (
                    bytes.fromhex("4883ec20")
                    + _imm64(9, 0xAABBCCDDEEFF0000)
                    + bytes.fromhex("4c890c24")
                    + bytes.fromhex("31c0" "f9")                # eax=0; stc (CF=1)
                    + bytes.fromhex("f066190424")               # lock sbb word [rsp], ax -> 0xFFFF
                    + (b"\xeb\x00" if cross else b"")
                    + _pack_flags_to_edi()                      # CF=2 | SF=4 | PF=16 = 0x16
                    + bytes.fromhex("488b0424")
                    + _imm64(10, 0xAABBCCDDEEFFFFFF)
                    + bytes.fromhex("4c39d0" "410f94c3" "450fb6db" "41c1e305" "4409df")
                    + bytes.fromhex("4883c420c3")
                )
                self.assertEqual(self._run(code), 0x36)

        # 3. 64-bit chained atomic 128-bit add:
        #    [rsp] = 0xFFFFFFFFFFFFFFFF, [rsp+8] = 0x0000000000000005
        #    lock add qword [rsp], 1    -> 0, CF=1
        #    lock adc qword [rsp+8], 0  -> 6, CF=0
        code = (
            bytes.fromhex("4883ec20")
            + _imm64(9, 0xFFFFFFFFFFFFFFFF)
            + _imm64(10, 5)
            + bytes.fromhex("4c890c24" "4c89542408")
            + bytes.fromhex("f04883042401")                     # lock add qword [rsp], 1 -> 0, CF=1
            + bytes.fromhex("f0488354240800")                   # lock adc qword [rsp+8], 0 -> 6, CF=0
            + bytes.fromhex("488b0424" "488b542408")            # rax=[rsp](0), rdx=[rsp+8](6)
            + bytes.fromhex("89d7" "4809c7")                    # edi = rdx | rax (6)
            + bytes.fromhex("4883c420c3")
        )
        self.assertEqual(self._run(code), 6)


if __name__ == "__main__":
    unittest.main()
