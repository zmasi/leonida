"""Tests for inc, dec, neg, and not width/CF/AF semantics in tools/x64_recomp.py.

Regression coverage for:
- 8-bit and 16-bit inc/dec adding/subtracting (1 << (32 - width)) instead of 1 after widening
- inc/dec preserving CF on dead-flag, live-flag (ubfx bit 29 before bfi), and materialized paths
- dec not inverting surviving CF via SUB_KIND materialization
- inc/dec/neg stashing CTX_A/CTX_B for AF, and not leaving all flags untouched

Static ARM64-encoding checks run everywhere; execution tests run natively on
Apple Silicon when CAN_RUN_MACHO is available, or under QEMU.

    python -I tests/test_x64_recomp_incdec.py
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


def _pack_cf_zf_sf_of_to_edi():
    """Set edi = CF | (ZF<<1) | (SF<<2) | (OF<<3) without clobbering rax..rdx."""
    return bytes.fromhex(
        "410f92c0" "410f94c1" "410f98c2" "410f90c3"
        "410fb6f8"
        "450fb6c9" "41d1e1" "4409cf"
        "450fb6d2" "41c1e202" "4409d7"
        "450fb6db" "41c1e303" "4409df"
    )


class IncDecStaticTests(unittest.TestCase):
    """Static ARM64 encoding checks that run on every platform."""

    def translate(self, code):
        insns = list(xr.disasm(code, 0x1000))
        self.assertFalse(any(i.unknown or i.error for i in insns), insns)
        tr = xr.FunctionTranslator(insns)
        tr.translate()
        self.assertEqual(tr.stats["unhandled"], [], f"unhandled: {tr.stats['unhandled']}")
        return tr

    def test_narrow_inc_dec_use_shifted_delta_not_imm1(self):
        # Flaggy `inc al; setz cl; ret` (fe c0 0f 94 c1 c3) must load 0x01000000 (MOVZ W1, #0x100, LSL #16 = 0x52A02001)
        tr8 = self.translate(bytes.fromhex("fec0" "0f94c1" "c3"))
        self.assertIn(0x52A02001, tr8.asm.words, "8-bit flaggy inc must add 0x01000000 after <<24")
        # Must NOT emit 32-bit ADDS W0, W0, #1 (0x31000400)
        self.assertNotIn(0x31000400, tr8.asm.words)

        # Flaggy `dec ax; setz cl; ret` (66 ff c8 0f 94 c1 c3) must load 0x00010000 (MOVZ W1, #1, LSL #16 = 0x52A00021)
        tr16 = self.translate(bytes.fromhex("66ffc8" "0f94c1" "c3"))
        self.assertIn(0x52A00021, tr16.asm.words, "16-bit flaggy dec must subtract 0x00010000 after <<16")
        self.assertNotIn(0x71000400, tr16.asm.words)

    def test_flaggy_inc_dec_extracts_bit29_before_bfi(self):
        # stc (f9); inc eax (ff c0); setz dl (0f 94 c2); setc cl (0f 92 c1); ret (c3)
        tr = self.translate(bytes.fromhex("f9" "ffc0" "0f94c2" "0f92c1" "c3"))
        # UBFX X17, X17, #29, #1 -> 0xD35D7631 (sf=1, N=1, immr=29, imms=29, rn=17, rd=17)
        a_ref = xr.Asm()
        a_ref.ubfx(xr.GTMP, xr.GTMP, 29, 1)
        a_ref.bfi(xr.T3, xr.GTMP, 29, 1)
        ubfx_w, bfi_w = a_ref.words
        self.assertIn(ubfx_w, tr.asm.words, "inc/dec must extract bit 29 of saved NZCV before BFI")
        self.assertIn(bfi_w, tr.asm.words)
        self.assertLess(tr.asm.words.index(ubfx_w), tr.asm.words.index(bfi_w))

    def test_dead_flags_inc_dec_does_not_touch_nzcv(self):
        # stc (f9); inc eax (ff c0); setc cl (0f 92 c1); ret (c3): inc flags are dead -> must emit ADD, not ADDS!
        tr = self.translate(bytes.fromhex("f9" "ffc0" "0f92c1" "c3"))
        # Must NOT emit 32-bit ADDS W0, W0, #1 (0x31000400)
        self.assertNotIn(0x31000400, tr.asm.words, "dead-flag inc must use plain ADD so NZCV is untouched")

    def test_materialized_dec_does_not_flip_arm_carry(self):
        # stc (f9); dec ecx (ff c9); jmp +0 (eb 00); setc dl (0f 92 c2); setz dh (0f 94 c6); ret (c3)
        # Neither stc nor dec should emit EOR X17, X17, #0x20000000 (which dec's SUB_KIND materialization used to do!).
        tr = self.translate(bytes.fromhex("f9" "ffc9" "eb00" "0f92c2" "0f94c6" "c3"))
        a_ref = xr.Asm()
        a_ref.eor_i(xr.GTMP, xr.GTMP, 0x20000000)
        self.assertNotIn(a_ref.words[0], tr.asm.words, "materialized dec must not invert surviving CF")

    def test_inc_dec_neg_stash_af_operands_when_materialized(self):
        a_ref = xr.Asm()
        a_ref.stur(xr.T0, xr.CTX_REG, xr.CTX_A, size=8)
        a_ref.stur(31, xr.CTX_REG, xr.CTX_A, size=8)
        stur_t0_a, stur_zr_a = a_ref.words

        tr_inc = self.translate(bytes.fromhex("ffc0" "9c" "c3"))  # inc eax; pushf; ret
        self.assertIn(stur_t0_a, tr_inc.asm.words, "inc must stash CTX_A when AF is needed")

        tr_dec = self.translate(bytes.fromhex("ffc8" "9c" "c3"))  # dec eax; pushf; ret
        self.assertIn(stur_t0_a, tr_dec.asm.words, "dec must stash CTX_A when AF is needed")

        tr_neg = self.translate(bytes.fromhex("f7d8" "9c" "c3"))  # neg eax; pushf; ret
        self.assertIn(stur_zr_a, tr_neg.asm.words, "neg must stash CTX_A=0 when AF is needed")


@unittest.skipIf(RUNNER is None, "needs Darwin arm64 Mach-O runner or qemu-system-aarch64")
class IncDecExecTests(unittest.TestCase):
    """End-to-end arm64 execution tests for inc, dec, neg, and not."""

    def run_code(self, code):
        res, tr = RUNNER(code)
        self.assertEqual(tr.stats["unhandled"], [], f"unhandled: {tr.stats['unhandled']}")
        return res

    def test_8bit_and_16bit_inc_dec_values_and_flags(self):
        upper8 = 0xDEAD_BEEF_CAFE_BA00
        # (label, init_rax, op_hex, want_rax, want_eflags_0x8D5_after_clc)
        # Note: clc before inc/dec sets CF=0; inc/dec must preserve CF=0 and produce exact SF/ZF/OF/PF/AF!
        cases8 = [
            ("inc al 0->1", upper8 | 0x00, "fec0", upper8 | 0x01, 0x000),
            ("inc al 0f->10 AF", upper8 | 0x0F, "fec0", upper8 | 0x10, 0x010),
            ("inc al 7f->80 OF+SF+AF", upper8 | 0x7F, "fec0", upper8 | 0x80, 0x890),
            ("inc al ff->00 ZF+PF+AF", upper8 | 0xFF, "fec0", upper8 | 0x00, 0x054),
            ("dec al 01->00 ZF+PF", upper8 | 0x01, "fec8", upper8 | 0x00, 0x044),
            ("dec al 10->0f AF+PF", upper8 | 0x10, "fec8", upper8 | 0x0F, 0x014),
            ("dec al 80->7f OF+AF", upper8 | 0x80, "fec8", upper8 | 0x7F, 0x810),
            ("dec al 00->ff SF+PF+AF", upper8 | 0x00, "fec8", upper8 | 0xFF, 0x094),
            ("inc ah ff->00", 0x1122_3344_5566_FF77, "fec4", 0x1122_3344_5566_0077, 0x054),
            ("dec ah 01->00", 0x1122_3344_5566_0177, "fecc", 0x1122_3344_5566_0077, 0x044),
        ]
        for label, init_rax, op_hex, want_rax, want_ef in cases8:
            with self.subTest(label=label):
                code = (
                    _imm64(0, init_rax)
                    + bytes.fromhex("f8")                           # clc (CF=0)
                    + bytes.fromhex(op_hex)                         # inc/dec al/ah
                    + bytes.fromhex("eb00" "9c" "5f" "81e7d5080000")  # jmp +0; pushf; pop rdi; and edi, 0x8D5
                    + _imm64(2, want_rax)
                    + bytes.fromhex("4839d0" "0f94c0" "0fb6c0" "c1e704" "09c7" "c3")
                )
                self.assertEqual(self.run_code(code), (want_ef << 4) | 1)

        # Also test dead-flags 8-bit `inc al` (must actually increment even when flags aren't read!)
        code_dead8 = (
            _imm64(0, upper8 | 0x41)
            + bytes.fromhex("fec0")                                 # inc al (flags unused)
            + _imm64(2, upper8 | 0x42)
            + bytes.fromhex("4839d0" "0f94c0" "0fb6f8" "c3")
        )
        self.assertEqual(self.run_code(code_dead8), 1)

        upper16 = 0x1234_5678_9ABC_0000
        cases16 = [
            ("inc ax 0->1", upper16 | 0x0000, "66ffc0", upper16 | 0x0001, 0x000),
            ("inc ax 7fff->8000 OF+SF+AF+PF", upper16 | 0x7FFF, "66ffc0", upper16 | 0x8000, 0x894),
            ("inc ax ffff->0000 ZF+PF+AF", upper16 | 0xFFFF, "66ffc0", upper16 | 0x0000, 0x054),
            ("dec ax 0001->0000 ZF+PF", upper16 | 0x0001, "66ffc8", upper16 | 0x0000, 0x044),
            ("dec ax 8000->7fff OF+AF+PF", upper16 | 0x8000, "66ffc8", upper16 | 0x7FFF, 0x814),
            ("dec ax 0000->ffff SF+PF+AF", upper16 | 0x0000, "66ffc8", upper16 | 0xFFFF, 0x094),
        ]
        for label, init_rax, op_hex, want_rax, want_ef in cases16:
            with self.subTest(label=label):
                code = (
                    _imm64(0, init_rax)
                    + bytes.fromhex("f8")
                    + bytes.fromhex(op_hex)
                    + bytes.fromhex("eb00" "9c" "5f" "81e7d5080000")
                    + _imm64(2, want_rax)
                    + bytes.fromhex("4839d0" "0f94c0" "0fb6c0" "c1e704" "09c7" "c3")
                )
                self.assertEqual(self.run_code(code), (want_ef << 4) | 1)

        # Memory 8-bit inc & 16-bit dec
        code_mem = (
            bytes.fromhex("4883ec10")
            + _imm64(6, 0x1122_3344_5566_01FF)
            + bytes.fromhex("48893424")                             # [rsp] = 0x...01FF
            + bytes.fromhex("fe0424")                               # inc byte [rsp] -> byte0 = 0x00 -> 0x...0100
            + bytes.fromhex("66ff0c24")                             # dec word [rsp] -> word0 = 0x00FF -> 0x...00FF
            + _imm64(2, 0x1122_3344_5566_00FF)
            + bytes.fromhex("483b1424" "0f94c0" "0fb6f8" "4883c410" "c3")
        )
        self.assertEqual(self.run_code(code_mem), 1)

    def test_inc_dec_preserve_cf_across_all_paths(self):
        # 1. Dead-flags path: stc -> inc eax -> setc (CF must remain 1!)
        code_dead_stc = (
            _imm64(0, 5) + bytes.fromhex("f9" "ffc0" "0f92c1" "0fb6f9" "c3")
        )
        self.assertEqual(self.run_code(code_dead_stc), 1)

        # 2. Live-flags same-block path: stc -> inc eax (0xFFFFFFFF -> 0, ZF=1) -> setz + setc (both 1!)
        code_live_stc = (
            _imm64(0, 0xFFFFFFFF)
            + bytes.fromhex("f9" "ffc0")
            + _pack_cf_zf_sf_of_to_edi()
            + bytes.fromhex("c3")
        )
        self.assertEqual(self.run_code(code_live_stc), 0b0011)  # CF=1, ZF=1

        # 3. Live-flags after SUB_KIND: cmp 5, 9 (CF=1) -> dec ecx (1 -> 0, ZF=1) -> setz + setc (both 1!)
        code_live_sub_cf1 = (
            _imm64(0, 5) + _imm64(3, 9) + _imm64(1, 1)
            + bytes.fromhex("39d8" "ffc9")                          # cmp eax, ebx (CF=1); dec ecx (ZF=1)
            + _pack_cf_zf_sf_of_to_edi()
            + bytes.fromhex("c3")
        )
        self.assertEqual(self.run_code(code_live_sub_cf1), 0b0011)  # CF=1, ZF=1

        # 4. Live-flags after SUB_KIND: cmp 9, 5 (CF=0) -> dec ecx (0 -> -1, SF=1) -> sets + setc (SF=1, CF=0!)
        code_live_sub_cf0 = (
            _imm64(0, 9) + _imm64(3, 5) + _imm64(1, 0)
            + bytes.fromhex("39d8" "ffc9")
            + _pack_cf_zf_sf_of_to_edi()
            + bytes.fromhex("c3")
        )
        self.assertEqual(self.run_code(code_live_sub_cf0), 0b0100)  # SF=1, CF=0

        # 5. Cross-block materialized dec after stc (CF=1) AND after clc (CF=0): dec must NOT invert CF!
        code_mat_dec_cf1 = (
            _imm64(1, 1)
            + bytes.fromhex("f9" "ffc9" "eb00" "9c" "5f" "81e7d5080000" "c3")
        )
        self.assertEqual(self.run_code(code_mat_dec_cf1), 0x045)    # CF=1 | PF=1 | ZF=1

        code_mat_dec_cf0 = (
            _imm64(1, 1)
            + bytes.fromhex("f8" "ffc9" "eb00" "9c" "5f" "81e7d5080000" "c3")
        )
        self.assertEqual(self.run_code(code_mat_dec_cf0), 0x044)    # CF=0 | PF=1 | ZF=1

        # 6. Cross-block materialized dec after cmp (SUB_KIND, CF=1): CF stays 1, AF=1, PF=1
        code_mat_cmp_dec = (
            _imm64(0, 5) + _imm64(3, 9) + _imm64(1, 0x10)
            + bytes.fromhex("39d8" "ffc9" "eb00" "9c" "5f" "81e7d5080000" "c3")
        )
        self.assertEqual(self.run_code(code_mat_cmp_dec), 0x015)    # CF=1 | PF=1 | AF=1

        # 7. Multi-word add chain with intervening inc: add (CF=1); inc ecx; adc edi, 0 -> 1!
        code_adc_chain = (
            _imm64(0, 0xFFFFFFFF) + _imm64(3, 1) + _imm64(1, 10) + _imm64(7, 0)
            + bytes.fromhex("01d8" "ffc1" "83d700" "c3")            # add eax, ebx; inc ecx; adc edi, 0; ret
        )
        self.assertEqual(self.run_code(code_adc_chain), 1)

    def test_neg_af_and_not_preserves_flags(self):
        # 1. neg al AF/CF/OF/ZF/SF/PF across edge values
        neg_cases = [
            ("neg 0", 0x00, 0x00, 0x044),       # ZF=1, PF=1, CF=0, AF=0
            ("neg 1", 0x01, 0xFF, 0x095),       # CF=1, PF=1, AF=1, SF=1
            ("neg 0x10", 0x10, 0xF0, 0x085),    # CF=1, PF=1, AF=0, SF=1
            ("neg 0x80", 0x80, 0x80, 0x881),    # CF=1, OF=1, SF=1, PF=0, AF=0
        ]
        for label, v8, want8, want_ef in neg_cases:
            with self.subTest(label=label):
                code = (
                    _imm64(0, 0xAABB_CCDD_EEFF_0000 | v8)
                    + bytes.fromhex("f6d8" "eb00" "9c" "5f" "81e7d5080000")
                    + _imm64(2, 0xAABB_CCDD_EEFF_0000 | want8)
                    + bytes.fromhex("4839d0" "0f94c0" "0fb6c0" "c1e704" "09c7" "c3")
                )
                self.assertEqual(self.run_code(code), (want_ef << 4) | 1)

        # 2. not preserves all flags (CF=1, ZF=0, SF=1, OF=0 from `cmp 5, 9`) and narrows properly
        code_not = (
            _imm64(0, 0x1122_3344_5566_770F)
            + _imm64(3, 5) + _imm64(1, 9)
            + bytes.fromhex("39cb" "f6d0")                          # cmp ebx, ecx (CF=1, SF=1); not al -> 0xF0
            + _pack_cf_zf_sf_of_to_edi()
            + _imm64(2, 0x1122_3344_5566_77F0)
            + bytes.fromhex("4839d0" "0f94c0" "0fb6c0" "c1e704" "09c7" "c3")
        )
        self.assertEqual(self.run_code(code_not), (0b0101 << 4) | 1)


if __name__ == "__main__":
    unittest.main()
