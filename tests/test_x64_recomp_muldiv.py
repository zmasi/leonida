"""Tests for x86-64 mul/imul/div/idiv emitters (8/16/32/64-bit, CF/OF, 128-bit div) in tools/x64_recomp.py.

Translation and ARM64-code-shape tests run everywhere (no game files, no QEMU needed);
execution tests run natively on Apple Silicon when run_darwin is available, or under QEMU.

    python -I tests/test_x64_recomp_muldiv.py
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
    """mov r64, imm64 (REX.W + B8+r)."""
    rex = 0x48 | (1 if reg >= 8 else 0)
    return bytes([rex, 0xB8 + (reg & 7)]) + struct.pack("<Q", val & 0xFFFFFFFFFFFFFFFF)


def _mov_edi_eax():
    return bytes.fromhex("89c7")


def _ret():
    return b"\xc3"


def _pack_flags_to_edi():
    """Set edi = (CF ? 1 : 0) | (OF ? 2 : 0) without touching rax/rdx."""
    # setc cl (0f 92 c1); seto r10b (41 0f 90 c2); movzx edi, cl (0f b6 f9);
    # movzx r10d, r10b (45 0f b6 d2); shl r10d, 1 (41 d1 e2); or edi, r10d (44 09 d7)
    return bytes.fromhex("0f92c1" "410f90c2" "0fb6f9" "450fb6d2" "41d1e2" "4409d7")


class MulDivTranslateTests(unittest.TestCase):
    """Static translation & code-shape checks (always run without QEMU/Darwin runner)."""

    def translate(self, code):
        insns = list(xr.disasm(code, 0x1000))
        self.assertFalse(any(i.unknown or i.error for i in insns), insns)
        tr = xr.FunctionTranslator(insns)
        tr.translate()
        self.assertEqual(tr.stats["unhandled"], [], f"unhandled: {tr.stats['unhandled']}")
        return tr

    def test_all_widths_mul_imul_div_idiv_translate_cleanly(self):
        # 8/16/32/64-bit 1-op mul/imul/div/idiv + 16/32/64-bit 2/3-op imul (incl. imm=-1)
        code = bytes.fromhex(
            "f6e3" "f6eb" "f6f3" "f6fb"             # mul/imul/div/idiv bl
            "f6e4"                                  # mul ah (hi8)
            "66f7e3" "66f7eb" "66f7f3" "66f7fb"     # mul/imul/div/idiv bx
            "f7e3" "f7eb" "f7f3" "f7fb"             # mul/imul/div/idiv ebx
            "48f7e3" "48f7eb" "48f7f3" "48f7fb"     # mul/imul/div/idiv rbx
            "660fafc3" "0fafc3" "480fafc3"          # imul ax/eax/rax, bx/ebx/rbx
            "666bc3fd" "6bc3fd" "486bc3ff"          # imul ax/eax/rax, bx/ebx/rbx, -3/-3/-1
            "c3"
        )
        self.translate(code)

    def test_cross_block_mul_and_imul_materialize_flags(self):
        # Jump joins into a shared jo consumer -> both mul and imul land in tr.mat and must bl rt_mkflags.
        # 0x1000: test edi, edi (85 ff)
        # 0x1002: je +4 -> 0x1008 (74 04)
        # 0x1004: mul ebx (f7 e3)
        # 0x1006: jmp +3 -> 0x100b (eb 03)
        # 0x1008: imul eax, ebx (0f af c3)
        # 0x100b: seto al (0f 90 c0)
        # 0x100e: movzx edi, al (0f b6 f8)
        # 0x1011: ret (c3)
        code = bytes.fromhex("85ff" "7404" "f7e3" "eb03" "0fafc3" "0f90c0" "0fb6f8" "c3")
        tr = self.translate(code)
        self.assertIn(0x1004, tr.mat)
        self.assertIn(0x1008, tr.mat)
        mkflag_calls = [lbl for _, kind, lbl in tr.asm.fixups if kind == "b26" and lbl == "rt_mkflags"]
        self.assertGreaterEqual(len(mkflag_calls), 2, "both mul and imul in tr.mat must call rt_mkflags")

    def test_two_op_imul_does_not_clobber_source_before_smulh(self):
        # imul rax, rbx; seto al; ret -> must NOT compute mul T0, T1, T0 followed by smulh *, T1, T0
        code = bytes.fromhex("480fafc3" "0f90c0" "c3")
        tr = self.translate(code)
        # Find smulh (0x9B407C00 mask 0xFFE0FC00) and any preceding mul (0x9B007C00 mask 0xFFE0FC00)
        smulhs = [w for w in tr.asm.words if (w & 0xFFE0FC00) == 0x9B407C00]
        muls = [w for w in tr.asm.words if (w & 0xFFE0FC00) == 0x9B007C00]
        self.assertEqual(len(smulhs), 1)
        self.assertEqual(len(muls), 1)
        mul_rd = muls[0] & 0x1F
        smulh_rn = (smulhs[0] >> 5) & 0x1F
        smulh_rm = (smulhs[0] >> 16) & 0x1F
        self.assertNotIn(mul_rd, (smulh_rn, smulh_rm), "mul destination must not clobber smulh inputs")

    def test_one_op_imul_checks_sign_extension_not_zero(self):
        # imul rbx; seto al; ret -> must emit ASR x*, x_rax, #63 (SBFM 0x9340FC00 | (63<<16))
        code = bytes.fromhex("48f7eb" "0f90c0" "c3")
        tr = self.translate(code)
        asr63 = [w for w in tr.asm.words if (w & 0xFFFFFC00) == (0x9340FC00 | (63 << 16))]
        self.assertGreaterEqual(len(asr63), 1, "1-op imul r64 must compare rdx against sign-extension of rax")


@unittest.skipIf(RUNNER is None, "needs Darwin arm64 Mach-O runner or qemu-system-aarch64")
class MulDivExecTests(unittest.TestCase):
    """End-to-end execution tests on real arm64 hardware (or QEMU)."""

    def run_code(self, code):
        res, tr = RUNNER(code)
        self.assertEqual(tr.stats["unhandled"], [], f"unhandled: {tr.stats['unhandled']}")
        return res

    def test_two_and_three_op_imul_cf_of_all_widths(self):
        # Regression: 100000 * 10000 = 10^9 fits in signed 32-bit (CF=OF=0); old code clobbered T0 -> CF=OF=1!
        cases_32 = [
            (100_000, 10_000, 1_000_000_000, 0),
            (-100_000, 10_000, (-1_000_000_000) & 0xFFFFFFFF, 0),
            (0x7FFFFFFF, 2, 0xFFFFFFFE, 3),
            (-0x80000000, -1, 0x80000000, 3),
            (-0x40000000, 2, 0x80000000, 0),
        ]
        for a, b, want_lo, want_flags in cases_32:
            with self.subTest(width=32, a=a, b=b):
                # mov eax, a; mov ebx, b; imul eax, ebx; pack flags -> edi; check eax == want_lo
                code = (
                    _imm64(0, a) + _imm64(3, b)
                    + bytes.fromhex("0fafc3")       # imul eax, ebx
                    + _pack_flags_to_edi()
                    + bytes.fromhex("c1e708")       # shl edi, 8
                    + bytes.fromhex("25ff000000")   # and eax, 0xff (low byte spot check) -> better: xor
                )
                # Pack (want_flags << 16) | (result == want_lo)
                code = (
                    _imm64(0, a) + _imm64(3, b)
                    + bytes.fromhex("0fafc3")
                    + _pack_flags_to_edi()
                    + _imm64(2, want_lo)
                    + bytes.fromhex("39d0")         # cmp eax, edx
                    + bytes.fromhex("0f94c0")       # sete al
                    + bytes.fromhex("0fb6c0")       # movzx eax, al
                    + bytes.fromhex("c1e704")       # shl edi, 4
                    + bytes.fromhex("09c7")         # or edi, eax
                    + _ret()
                )
                self.assertEqual(self.run_code(code), (want_flags << 4) | 1)

        # 64-bit 2-op imul: 0x100000000 * 0x10000 fits in signed 64-bit (CF=OF=0); old clobber gave CF=OF=1!
        cases_64 = [
            (0x1_0000_0000, 0x1_0000, 0x1_0000_0000_0000, 0),
            (-0x1_0000_0000, 0x1_0000, (-0x1_0000_0000_0000) & 0xFFFFFFFFFFFFFFFF, 0),
            (0x4000_0000_0000_0000, 2, 0x8000_0000_0000_0000, 3),
            (-(1 << 62), 2, (-1 << 63) & 0xFFFFFFFFFFFFFFFF, 0),
        ]
        for a, b, want_lo, want_flags in cases_64:
            with self.subTest(width=64, a=a, b=b):
                code = (
                    _imm64(0, a) + _imm64(3, b)
                    + bytes.fromhex("480fafc3")     # imul rax, rbx
                    + _pack_flags_to_edi()
                    + _imm64(2, want_lo)
                    + bytes.fromhex("4839d0")       # cmp rax, rdx
                    + bytes.fromhex("0f94c0")       # sete al
                    + bytes.fromhex("0fb6c0")       # movzx eax, al
                    + bytes.fromhex("c1e704")       # shl edi, 4
                    + bytes.fromhex("09c7")         # or edi, eax
                    + _ret()
                )
                self.assertEqual(self.run_code(code), (want_flags << 4) | 1)

        # 64-bit 3-op imul with imm=-1 (exercises safe -1 load and overflow on INT64_MIN * -1)
        for a, want_lo, want_flags in [
            (42, (-42) & 0xFFFFFFFFFFFFFFFF, 0),
            (1 << 63, 1 << 63, 3),
        ]:
            with self.subTest(width="64_imm_neg1", a=a):
                code = (
                    _imm64(3, a)
                    + bytes.fromhex("486bc3ff")     # imul rax, rbx, -1
                    + _pack_flags_to_edi()
                    + _imm64(2, want_lo)
                    + bytes.fromhex("4839d0")
                    + bytes.fromhex("0f94c0") + bytes.fromhex("0fb6c0")
                    + bytes.fromhex("c1e704") + bytes.fromhex("09c7") + _ret()
                )
                self.assertEqual(self.run_code(code), (want_flags << 4) | 1)

        # 16-bit 2-op imul preserves upper 48 bits of rax and computes exact 16-bit signed overflow
        for a16, b16, want_ax, want_flags in [
            (100, 200, 20000, 0),
            (-100, 200, (-20000) & 0xFFFF, 0),
            (200, 200, 40000 & 0xFFFF, 3),
            (-32768, -1, 0x8000, 3),
        ]:
            with self.subTest(width=16, a16=a16, b16=b16):
                upper = 0xDEAD_BEEF_CAFE_0000
                code = (
                    _imm64(0, upper | (a16 & 0xFFFF))
                    + _imm64(3, 0x1111_2222_3333_0000 | (b16 & 0xFFFF))
                    + bytes.fromhex("660fafc3")     # imul ax, bx
                    + _pack_flags_to_edi()
                    + _imm64(2, upper | want_ax)
                    + bytes.fromhex("4839d0")
                    + bytes.fromhex("0f94c0") + bytes.fromhex("0fb6c0")
                    + bytes.fromhex("c1e704") + bytes.fromhex("09c7") + _ret()
                )
                self.assertEqual(self.run_code(code), (want_flags << 4) | 1)

    def test_one_op_mul_imul_8_16_32_64_and_flags(self):
        # 8-bit mul/imul (preserves upper 48 bits of rax)
        upper = 0x1234_5678_9ABC_0000
        for op_hex, al, bl, want_ax, want_flags in [
            ("f6e3", 10, 20, 200, 0),                       # mul: ah == 0 -> CF=OF=0
            ("f6e3", 20, 20, 400, 3),                       # mul: ah == 1 -> CF=OF=1
            ("f6eb", (-3) & 0xFF, 4, (-12) & 0xFFFF, 0),    # imul: -12 fits in s8 -> CF=OF=0
            ("f6eb", (-16) & 0xFF, 16, (-256) & 0xFFFF, 3), # imul: -256 overflows s8 -> CF=OF=1
        ]:
            with self.subTest(width=8, op=op_hex, al=al, bl=bl):
                code = (
                    _imm64(0, upper | al) + _imm64(3, bl)
                    + bytes.fromhex(op_hex)
                    + _pack_flags_to_edi()
                    + _imm64(2, upper | want_ax)
                    + bytes.fromhex("4839d0")
                    + bytes.fromhex("0f94c0") + bytes.fromhex("0fb6c0")
                    + bytes.fromhex("c1e704") + bytes.fromhex("09c7") + _ret()
                )
                self.assertEqual(self.run_code(code), (want_flags << 4) | 1)

        # 16-bit mul/imul (preserves upper 48 bits of rax AND rdx)
        up_a, up_d = 0xAAAA_BBBB_CCCC_0000, 0x5555_6666_7777_0000
        for op_hex, ax, bx, want_ax, want_dx, want_flags in [
            ("66f7e3", 200, 200, 40000, 0, 0),
            ("66f7e3", 1000, 100, (100000) & 0xFFFF, (100000 >> 16) & 0xFFFF, 3),
            ("66f7eb", (-100) & 0xFFFF, 200, (-20000) & 0xFFFF, 0xFFFF, 0),  # negative non-overflow!
            ("66f7eb", 200, 200, 40000 & 0xFFFF, 0, 3),                      # +40000 overflows s16!
        ]:
            with self.subTest(width=16, op=op_hex, ax=ax, bx=bx):
                code = (
                    _imm64(0, up_a | ax) + _imm64(2, up_d) + _imm64(3, bx)
                    + bytes.fromhex(op_hex)
                    + _pack_flags_to_edi()
                    + _imm64(6, up_a | want_ax) + _imm64(8, up_d | want_dx)
                    + bytes.fromhex("4839f0")       # cmp rax, rsi
                    + bytes.fromhex("0f94c0")       # sete al
                    + bytes.fromhex("4c39c2")       # cmp rdx, r8
                    + bytes.fromhex("0f94c2")       # sete dl
                    + bytes.fromhex("20d0")         # and al, dl
                    + bytes.fromhex("0fb6c0")       # movzx eax, al
                    + bytes.fromhex("c1e704") + bytes.fromhex("09c7") + _ret()
                )
                self.assertEqual(self.run_code(code), (want_flags << 4) | 1)

        # 32-bit & 64-bit 1-op imul negative non-overflow regression (old code set CF=OF=1 because rdx != 0!)
        for width, op_hex, a, b, want_lo, want_hi, want_flags in [
            (32, "f7eb", (-2) & 0xFFFFFFFF, 3, (-6) & 0xFFFFFFFF, 0xFFFFFFFF, 0),
            (32, "f7eb", (-0x40000000) & 0xFFFFFFFF, 4, 0, 0xFFFFFFFF, 3),
            (32, "f7e3", 0x80000000, 2, 0, 1, 3),
            (64, "48f7eb", (-7) & 0xFFFFFFFFFFFFFFFF, 9, (-63) & 0xFFFFFFFFFFFFFFFF, 0xFFFFFFFFFFFFFFFF, 0),
            (64, "48f7eb", (-(1 << 62)) & 0xFFFFFFFFFFFFFFFF, 4, 0, 0xFFFFFFFFFFFFFFFF, 3),
            (64, "48f7e3", 1 << 63, 2, 0, 1, 3),
        ]:
            with self.subTest(width=width, op=op_hex, a=a, b=b):
                code = (
                    _imm64(0, a) + _imm64(3, b)
                    + bytes.fromhex(op_hex)
                    + _pack_flags_to_edi()
                    + _imm64(6, want_lo) + _imm64(8, want_hi)
                    + bytes.fromhex("4839f0") + bytes.fromhex("0f94c0")
                    + bytes.fromhex("4c39c2") + bytes.fromhex("0f94c2")
                    + bytes.fromhex("20d0") + bytes.fromhex("0fb6c0")
                    + bytes.fromhex("c1e704") + bytes.fromhex("09c7") + _ret()
                )
                self.assertEqual(self.run_code(code), (want_flags << 4) | 1)

    def test_cross_block_imul_flags_execution(self):
        # Forces imul into tr.mat via a branch join before seto/setc.
        for b, want_flags in [(3, 0), (0x40000000, 3)]:
            with self.subTest(b=b):
                # mov eax, -4; mov ebx, b; test ebx, ebx; jz +3; imul eax, ebx; jmp +2; mul ebx; pack_flags; ret
                # Simpler: two-way join where both arms do imul eax, ebx
                code = (
                    _imm64(0, (-4) & 0xFFFFFFFF) + _imm64(3, b)
                    + bytes.fromhex("85db")         # test ebx, ebx
                    + bytes.fromhex("7405")         # je +5 -> second imul
                    + bytes.fromhex("0fafc3")       # imul eax, ebx
                    + bytes.fromhex("eb03")         # jmp +3 -> join
                    + bytes.fromhex("0fafc3")       # imul eax, ebx
                    + _pack_flags_to_edi() + _ret()
                )
                self.assertEqual(self.run_code(code), want_flags)

    def test_div_and_idiv_8_16_32_bit(self):
        # 8-bit div & idiv (preserves upper 48 bits of rax; al=quot, ah=rem)
        upper = 0xABCD_1234_5678_0000
        for op_hex, ax, bl, want_al, want_ah in [
            ("f6f3", 1000, 7, 142, 6),                          # 1000 / 7 = 142 r 6
            ("f6fb", (-100) & 0xFFFF, 7, (-14) & 0xFF, (-2) & 0xFF),
            ("f6fb", (-100) & 0xFFFF, (-7) & 0xFF, 14, (-2) & 0xFF),
            ("f6fb", 100, (-7) & 0xFF, (-14) & 0xFF, 2),
        ]:
            with self.subTest(width=8, op=op_hex, ax=ax, bl=bl):
                want_rax = upper | (want_ah << 8) | want_al
                code = (
                    _imm64(0, upper | ax) + _imm64(3, bl)
                    + bytes.fromhex(op_hex)
                    + _imm64(2, want_rax)
                    + bytes.fromhex("4839d0") + bytes.fromhex("0f94c0") + bytes.fromhex("0fb6f8") + _ret()
                )
                self.assertEqual(self.run_code(code), 1)

        # 16-bit div & idiv (preserves upper 48 bits of rax & rdx)
        up_a, up_d = 0x1122_3344_5566_0000, 0x7788_99AA_BBCC_0000
        for op_hex, dx, ax, bx, want_ax, want_dx in [
            ("66f7f3", 0x0001, 0x0005, 0x100, 0x0100, 0x0005),  # 0x10005 / 0x100 = 0x100 r 5
            ("66f7fb", 0xFFFF, (-1000) & 0xFFFF, 33, (-30) & 0xFFFF, (-10) & 0xFFFF),
            ("66f7fb", 0xFFFE, 0x0000, (-32768) & 0xFFFF, 4, 0), # -131072 / -32768 = 4 r 0
        ]:
            with self.subTest(width=16, op=op_hex, dx=dx, ax=ax, bx=bx):
                code = (
                    _imm64(0, up_a | ax) + _imm64(2, up_d | dx) + _imm64(3, bx)
                    + bytes.fromhex(op_hex)
                    + _imm64(6, up_a | want_ax) + _imm64(8, up_d | want_dx)
                    + bytes.fromhex("4839f0") + bytes.fromhex("0f94c0")
                    + bytes.fromhex("4c39c2") + bytes.fromhex("0f94c2")
                    + bytes.fromhex("20d0") + bytes.fromhex("0fb6f8") + _ret()
                )
                self.assertEqual(self.run_code(code), 1)

        # 32-bit div & idiv with non-zero edx
        for op_hex, edx, eax, ebx, want_eax, want_edx in [
            ("f7f3", 3, 0x12345678, 7, ((3 << 32 | 0x12345678) // 7), ((3 << 32 | 0x12345678) % 7)),
            ("f7fb", 0xFFFFFFFF, (-100000) & 0xFFFFFFFF, 17, (-5882) & 0xFFFFFFFF, (-6) & 0xFFFFFFFF),
        ]:
            with self.subTest(width=32, op=op_hex):
                code = (
                    _imm64(0, eax) + _imm64(2, edx) + _imm64(3, ebx)
                    + bytes.fromhex(op_hex)
                    + _imm64(6, want_eax) + _imm64(8, want_edx)
                    + bytes.fromhex("4839f0") + bytes.fromhex("0f94c0")
                    + bytes.fromhex("4c39c2") + bytes.fromhex("0f94c2")
                    + bytes.fromhex("20d0") + bytes.fromhex("0fb6f8") + _ret()
                )
                self.assertEqual(self.run_code(code), 1)

    def test_div_64_fast_and_128bit_slow_paths(self):
        M64 = (1 << 64) - 1
        cases = [
            # Fast path (rdx == 0)
            (0, 1_000_000_000_007, 97),
            (0, M64, 1),
            # Full 128-by-64 unsigned slow path
            (5, 0x1234_5678_9ABC_DEF0, 0x0000_0001_0000_0007),
            (0x7FFF_FFFF_FFFF_FFFF, 0xDEAD_BEEF_CAFE_BABE, 0x8000_0000_0000_0001),
            # Near-max divisor & dividend: exercises 65th-bit carry_out path every iteration!
            (M64 - 1, M64, M64),
            (1, 0, 2),
        ]
        for rdx, rax, rbx in cases:
            dividend = (rdx << 64) | rax
            want_q, want_r = divmod(dividend, rbx)
            with self.subTest(rdx=hex(rdx), rax=hex(rax), rbx=hex(rbx)):
                code = (
                    _imm64(0, rax) + _imm64(2, rdx) + _imm64(3, rbx)
                    + bytes.fromhex("48f7f3")       # div rbx
                    + _imm64(6, want_q) + _imm64(8, want_r)
                    + bytes.fromhex("4839f0") + bytes.fromhex("0f94c0")
                    + bytes.fromhex("4c39c2") + bytes.fromhex("0f94c2")
                    + bytes.fromhex("20d0") + bytes.fromhex("0fb6f8") + _ret()
                )
                self.assertEqual(self.run_code(code), 1)

    def test_idiv_64_fast_and_128bit_slow_paths(self):
        M64 = (1 << 64) - 1
        # Exact truncated-toward-zero signed division oracle (matching x86 idiv)
        def sdivmod(a, b):
            q = abs(a) // abs(b)
            if (a < 0) ^ (b < 0):
                q = -q
            r = a - q * b
            return q & M64, r & M64

        d_max_pos = (1 << 63) - 1
        cases = [
            # Fast path (sign-extended 64-bit dividend, divisor != -1)
            (1_000_000_000_007, 97),
            (-1_000_000_000_007, 97),
            (1_000_000_000_007, -97),
            (-1_000_000_000_007, -97),
            (-(1 << 63), 1),
            # Slow path triggered by divisor == -1 (non-overflowing)
            (42, -1),
            (-42, -1),
            # Full 128-bit signed dividends across all 4 sign combinations
            ((5 << 64) + 0x1234_5678_9ABC_DEF0, 0x0000_0001_0000_0007),
            ((5 << 64) + 0x1234_5678_9ABC_DEF0, -0x0000_0001_0000_0007),
            (-((5 << 64) + 0x1234_5678_9ABC_DEF0), 0x0000_0001_0000_0007),
            (-((5 << 64) + 0x1234_5678_9ABC_DEF0), -0x0000_0001_0000_0007),
            # Boundary: (+2^63) / (-1) == -2^63 (exact INT64_MIN, must NOT trap!)
            (1 << 63, -1),
            # Boundary: (-2^63 * d_max_pos - 12345) / d_max_pos == -2^63 r -12345
            (-(1 << 63) * d_max_pos - 12345, d_max_pos),
            # Boundary: (+(2^63 - 1) * d_max_pos + 12345) / d_max_pos == +(2^63 - 1) r +12345
            (d_max_pos * d_max_pos + 12345, d_max_pos),
            # Divisor == INT64_MIN (-2^63)
            (-((1 << 64) + 12345), -(1 << 63)),
        ]
        for dividend, divisor in cases:
            rdx = (dividend >> 64) & M64
            rax = dividend & M64
            want_q, want_r = sdivmod(dividend, divisor)
            with self.subTest(dividend=hex(dividend), divisor=hex(divisor)):
                code = (
                    _imm64(0, rax) + _imm64(2, rdx) + _imm64(3, divisor & M64)
                    + bytes.fromhex("48f7fb")       # idiv rbx
                    + _imm64(6, want_q) + _imm64(8, want_r)
                    + bytes.fromhex("4839f0") + bytes.fromhex("0f94c0")
                    + bytes.fromhex("4c39c2") + bytes.fromhex("0f94c2")
                    + bytes.fromhex("20d0") + bytes.fromhex("0fb6f8") + _ret()
                )
                self.assertEqual(self.run_code(code), 1)


if __name__ == "__main__":
    unittest.main()
