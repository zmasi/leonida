"""Tests for 8/16/32/64-bit shift and rotate emitters (values, SF/ZF/CF) in tools/x64_recomp.py.

Regression coverage for:
- 64-bit shl/shr/sar/ror CF extraction using sf=1 (avoiding 32-bit UBFM/LSRV bit>=32 corruption)
- 8-bit and 16-bit sar sign-extension before arithmetic right shift
- 8-bit and 16-bit rol/ror narrow rotation (imm & CL, including count % width == 0)
- 8-bit and 16-bit shl/shr/sar narrow SF/ZF calculation and count == width CF
- rol/ror preserving N/Z flags instead of clobbering them with tst_r

Static ARM64-encoding checks run everywhere (no game files, no QEMU needed);
execution tests run natively on Apple Silicon when CAN_RUN_MACHO is available, or under QEMU.

    python -I tests/test_x64_recomp_shifts.py
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


def _pack_cf_zf_sf_to_edi():
    """Set edi = (CF ? 1 : 0) | (ZF ? 2 : 0) | (SF ? 4 : 0) without clobbering rax/rdx."""
    # setc r10b (41 0f 92 c2); setz r11b (41 0f 94 c3); sets r9b (41 0f 98 c1)
    # movzx edi, r10b (41 0f b6 fa); movzx r11d, r11b (45 0f b6 db); shl r11d, 1 (41 d1 e3);
    # or edi, r11d (44 09 df); movzx r9d, r9b (45 0f b6 c9); shl r9d, 2 (41 c1 e1 02); or edi, r9d (44 09 cf)
    return bytes.fromhex(
        "410f92c2" "410f94c3" "410f98c1"
        "410fb6fa" "450fb6db" "41d1e3" "4409df"
        "450fb6c9" "41c1e102" "4409cf"
    )


class ShiftStaticTests(unittest.TestCase):
    """Static ARM64 encoding checks that run on every platform."""

    def translate(self, code):
        insns = list(xr.disasm(code, 0x1000))
        self.assertFalse(any(i.unknown or i.error for i in insns), insns)
        tr = xr.FunctionTranslator(insns)
        tr.translate()
        self.assertEqual(tr.stats["unhandled"], [], f"unhandled: {tr.stats['unhandled']}")
        return tr

    def test_64bit_shift_cf_uses_64bit_ubfm_and_lsrv(self):
        # 32-bit UBFM (bit31=0) with immr>=32 or imms>=32 is UNDEFINED in ARM64!
        for label, hx in [
            ("shl rax, 1", "48d1e0" "0f92c0" "c3"),
            ("shr rax, 40", "48c1e828" "0f92c0" "c3"),
            ("sar rax, 40", "48c1f828" "0f92c0" "c3"),
            ("ror rax, 1", "48d1c8" "0f92c0" "c3"),
        ]:
            with self.subTest(label=label):
                tr = self.translate(bytes.fromhex(hx))
                # Check NO 32-bit UBFM (opcode 0x53000000, mask 0xFF800000) has immr>=32 or imms>=32
                for w in tr.asm.words:
                    if (w & 0xFF800000) == 0x53000000:
                        immr = (w >> 16) & 0x3F
                        imms = (w >> 10) & 0x3F
                        self.assertLess(immr, 32, f"illegal 32-bit UBFM immr={immr} in {label}")
                        self.assertLess(imms, 32, f"illegal 32-bit UBFM imms={imms} in {label}")
                # Must contain a 64-bit UBFM (0xD3400000, mask 0xFFC00000) extracting a bit >= 32
                ubfm64 = [w for w in tr.asm.words if (w & 0xFFC00000) == 0xD3400000 and ((w >> 16) & 0x3F) >= 32]
                self.assertGreaterEqual(len(ubfm64), 1, f"expected 64-bit UBFM for CF in {label}")

        # Variable 64-bit shl/shr/sar/ror CF must use 64-bit LSRV/ASRV/UBFM (bit31=1)
        for label, hx in [
            ("shl rax, cl", "48d3e0" "0f92c0" "c3"),
            ("shr rax, cl", "48d3e8" "0f92c0" "c3"),
            ("sar rax, cl", "48d3f8" "0f92c0" "c3"),
            ("ror rax, cl", "48d3c8" "0f92c0" "c3"),
        ]:
            with self.subTest(label=label):
                tr = self.translate(bytes.fromhex(hx))
                # Ensure no 32-bit LSRV (0x1AC02400 mask 0xFFE0FC00) is emitted for 64-bit CF
                lsrv32 = [w for w in tr.asm.words if (w & 0xFFE0FC00) == 0x1AC02400]
                self.assertEqual(lsrv32, [], f"unexpected 32-bit LSRV in 64-bit {label}")

    def test_narrow_sar_sign_extends_before_asr(self):
        # sar al, 1 (d0 f8) and sar ax, 1 (66 d1 f8) must emit 32-bit SXTB (0x13001C00) / SXTH (0x13003C00)
        tr8 = self.translate(bytes.fromhex("d0f8c3"))
        sxtb32 = [w for w in tr8.asm.words if (w & 0xFFFFFC00) == 0x13001C00]
        self.assertGreaterEqual(len(sxtb32), 1, "8-bit sar must sign-extend via SXTB before ASR")

        tr16 = self.translate(bytes.fromhex("66d1f8c3"))
        sxth32 = [w for w in tr16.asm.words if (w & 0xFFFFFC00) == 0x13003C00]
        self.assertGreaterEqual(len(sxth32), 1, "16-bit sar must sign-extend via SXTH before ASR")

    def test_rol_ror_do_not_clobber_nz_with_tst(self):
        # rol eax, 1; setc al; ret -> must NOT emit ANDS WZR (tst_r: 0x6A00001F mask 0x7F20001F)
        tr = self.translate(bytes.fromhex("d1c0" "0f92c0" "c3"))
        tsts = [w for w in tr.asm.words if (w & 0x7F20001F) == 0x6A00001F]
        self.assertEqual(tsts, [], "rol/ror must not emit tst_r and clobber N/Z")

        # Also check materialized rol/ror (CF from rol + ZF from xor both live across a block boundary)
        # does not call rt_mkflags or clobber N/Z/PF in ctx.eflags.
        # xor ebx, ebx (31 db); rol eax, 1 (d1 c0); jmp +0 (eb 00); jz +2 (74 02); jc +0 (72 00); ret (c3)
        tr_mat = self.translate(bytes.fromhex("31db" "d1c0" "eb00" "7402" "7200" "c3"))
        tsts_mat = [w for w in tr_mat.asm.words if (w & 0x7F20001F) == 0x6A00001F]
        # Only xor's tst_r and _ctx_jcc's tst_i (immediate, not tst_r) should exist; at most 1 tst_r (from xor)
        self.assertLessEqual(len(tsts_mat), 1, "materialized rol must not emit tst_r")
        # Only 1 BL (to rt_mkflags, from xor); rol itself must patch CF in ctx.eflags without calling rt_mkflags
        bls = [w for w in tr_mat.asm.words if (w & 0xFC000000) == 0x94000000]
        self.assertEqual(len(bls), 1, "rol must patch ctx.eflags CF bit directly instead of calling rt_mkflags")

    def test_narrow_rotate_by_multiple_of_width_has_no_illegal_extr(self):
        # rol al, 8 (c0 c0 08); ror ax, 16 (66 c1 c8 10)
        tr = self.translate(bytes.fromhex("c0c008" "66c1c810" "c3"))
        extrs = [w for w in tr.asm.words if (w & 0x7F800000) == 0x13800000]
        self.assertEqual(extrs, [], "rotate by multiple of width should be a no-op on value")


@unittest.skipIf(RUNNER is None, "needs Darwin arm64 Mach-O runner or qemu-system-aarch64")
class ShiftExecTests(unittest.TestCase):
    """End-to-end arm64 execution tests for narrow shifts/rotates and 64-bit CF."""

    def run_code(self, code):
        res, tr = RUNNER(code)
        self.assertEqual(tr.stats["unhandled"], [], f"unhandled: {tr.stats['unhandled']}")
        return res

    def test_64bit_shift_and_rotate_cf_high_bits(self):
        # 64-bit CF extraction for bit positions >= 32 (all corrupted by sf=0 on main!)
        cases = [
            # (desc, initial rax, cl_or_None, op_hex, want_cf)
            ("shl rax,1 top=1", 1 << 63, None, "48d1e0", 1),
            ("shl rax,1 top=0", 1 << 62, None, "48d1e0", 0),
            ("shl rax,20 bit44=1", 1 << 44, None, "48c1e014", 1),
            ("shr rax,40 bit39=1", 1 << 39, None, "48c1e828", 1),
            ("shr rax,40 bit39=0", 1 << 40, None, "48c1e828", 0),
            ("sar rax,40 bit39=1", (1 << 63) | (1 << 39), None, "48c1f828", 1),
            ("sar rax,40 bit39=0", (1 << 63) | (1 << 38), None, "48c1f828", 0),
            ("ror rax,1 bit0=1->bit63=1", 1, None, "48d1c8", 1),
            ("ror rax,1 bit0=0->bit63=0", 2, None, "48d1c8", 0),
            ("shl rax,cl(1) top=1", 1 << 63, 1, "48d3e0", 1),
            ("shl rax,cl(24) bit40=1", 1 << 40, 24, "48d3e0", 1),
            ("shr rax,cl(41) bit40=1", 1 << 40, 41, "48d3e8", 1),
            ("sar rax,cl(41) bit40=0", (1 << 63) | (1 << 39), 41, "48d3f8", 0),
            ("ror rax,cl(1) bit0=1", 1, 1, "48d3c8", 1),
            ("ror rax,cl(1) bit0=0", 2, 1, "48d3c8", 0),
        ]
        for desc, val, cl, hx, want_cf in cases:
            with self.subTest(desc=desc):
                code = _imm64(0, val)
                if cl is not None:
                    code += _imm64(1, cl)
                code += bytes.fromhex(hx) + bytes.fromhex("0f92c0" "0fb6f8" "c3")
                self.assertEqual(self.run_code(code), want_cf)

    def test_8bit_and_16bit_sar_preserves_sign_and_upper_bits(self):
        upper8 = 0xDEAD_BEEF_CAFE_BA00
        # (val8, cl_or_None, op_hex, want8, want_cf, want_zf, want_sf)
        cases8 = [
            (0x80, None, "d0f8", 0xC0, 0, 0, 1),          # sar al, 1 (-128 >> 1 = -64)
            (0x81, None, "d0f8", 0xC0, 1, 0, 1),          # sar al, 1 (-127 >> 1 = -64, CF=1)
            (0x80, None, "c0f807", 0xFF, 0, 0, 1),        # sar al, 7 = -1, CF=0
            (0x80, None, "c0f808", 0xFF, 1, 0, 1),        # sar al, 8 = -1, CF=1 (sign bit)
            (0x80, None, "c0f810", 0xFF, 1, 0, 1),        # sar al, 16 = -1, CF=1
            (0x7F, None, "c0f807", 0x00, 1, 1, 0),        # sar al, 7 = 0, ZF=1, CF=1
            (0x80, 1, "d2f8", 0xC0, 0, 0, 1),             # sar al, cl(1)
            (0x80, 9, "d2f8", 0xFF, 1, 0, 1),             # sar al, cl(9)
        ]
        for v8, cl, hx, want8, cf, zf, sf in cases8:
            with self.subTest(width=8, v8=hex(v8), cl=cl, hx=hx):
                want_flags = cf | (zf << 1) | (sf << 2)
                code = _imm64(0, upper8 | v8)
                if cl is not None:
                    code += _imm64(1, cl)
                code += (
                    bytes.fromhex(hx) + _pack_cf_zf_sf_to_edi()
                    + _imm64(2, upper8 | want8)
                    + bytes.fromhex("4839d0" "0f94c0" "0fb6c0" "c1e704" "09c7" "c3")
                )
                self.assertEqual(self.run_code(code), (want_flags << 4) | 1)

        upper16 = 0x1234_5678_9ABC_0000
        cases16 = [
            (0x8000, None, "66c1f804", 0xF800, 0, 0, 1),  # sar ax, 4
            (0x8000, None, "66c1f810", 0xFFFF, 1, 0, 1),  # sar ax, 16
            (0x8000, 4, "66d3f8", 0xF800, 0, 0, 1),       # sar ax, cl(4)
            (0x8000, 17, "66d3f8", 0xFFFF, 1, 0, 1),      # sar ax, cl(17)
        ]
        for v16, cl, hx, want16, cf, zf, sf in cases16:
            with self.subTest(width=16, v16=hex(v16), cl=cl, hx=hx):
                want_flags = cf | (zf << 1) | (sf << 2)
                code = _imm64(0, upper16 | v16)
                if cl is not None:
                    code += _imm64(1, cl)
                code += (
                    bytes.fromhex(hx) + _pack_cf_zf_sf_to_edi()
                    + _imm64(2, upper16 | want16)
                    + bytes.fromhex("4839d0" "0f94c0" "0fb6c0" "c1e704" "09c7" "c3")
                )
                self.assertEqual(self.run_code(code), (want_flags << 4) | 1)

    def test_8bit_and_16bit_shl_shr_flags_and_count_eq_width(self):
        upper8 = 0xAA55_BB66_CC77_DD00
        cases = [
            # (v8, cl, hx, want8, cf, zf, sf)
            (0x80, None, "d0e0", 0x00, 1, 1, 0),          # shl al, 1 -> 0, ZF=1, CF=1
            (0x40, None, "d0e0", 0x80, 0, 0, 1),          # shl al, 1 -> 0x80, SF=1, ZF=0, CF=0
            (0x01, None, "c0e008", 0x00, 1, 1, 0),        # shl al, 8 -> 0, ZF=1, CF=orig[0]=1
            (0xFF, None, "c0e009", 0x00, 0, 1, 0),        # shl al, 9 -> 0, ZF=1, CF=0
            (0x80, None, "c0e808", 0x00, 1, 1, 0),        # shr al, 8 -> 0, ZF=1, CF=orig[7]=1
            (0xFF, None, "c0e809", 0x00, 0, 1, 0),        # shr al, 9 -> 0, ZF=1, CF=0
            (0x80, 1, "d2e0", 0x00, 1, 1, 0),             # shl al, cl(1) -> 0, ZF=1, CF=1
            (0x40, 1, "d2e0", 0x80, 0, 0, 1),             # shl al, cl(1) -> 0x80, SF=1, ZF=0
            (0x01, 8, "d2e0", 0x00, 1, 1, 0),             # shl al, cl(8) -> 0, ZF=1, CF=1
            (0xFF, 9, "d2e0", 0x00, 0, 1, 0),             # shl al, cl(9) -> 0, ZF=1, CF=0
            (0x80, 8, "d2e8", 0x00, 1, 1, 0),             # shr al, cl(8) -> 0, ZF=1, CF=1
            (0xFF, 9, "d2e8", 0x00, 0, 1, 0),             # shr al, cl(9) -> 0, ZF=1, CF=0
        ]
        for v8, cl, hx, want8, cf, zf, sf in cases:
            with self.subTest(v8=hex(v8), cl=cl, hx=hx):
                want_flags = cf | (zf << 1) | (sf << 2)
                code = _imm64(0, upper8 | v8)
                if cl is not None:
                    code += _imm64(1, cl)
                code += (
                    bytes.fromhex(hx) + _pack_cf_zf_sf_to_edi()
                    + _imm64(2, upper8 | want8)
                    + bytes.fromhex("4839d0" "0f94c0" "0fb6c0" "c1e704" "09c7" "c3")
                )
                self.assertEqual(self.run_code(code), (want_flags << 4) | 1)

        upper16 = 0x1122_3344_5566_0000
        cases16 = [
            (0x8000, None, "66d1e0", 0x0000, 1, 1, 0),    # shl ax, 1 -> 0, ZF=1, CF=1
            (0x4000, None, "66d1e0", 0x8000, 0, 0, 1),    # shl ax, 1 -> 0x8000, SF=1, ZF=0
            (0x0001, None, "66c1e010", 0x0000, 1, 1, 0),  # shl ax, 16 -> 0, ZF=1, CF=orig[0]=1
            (0x8000, None, "66c1e810", 0x0000, 1, 1, 0),  # shr ax, 16 -> 0, ZF=1, CF=orig[15]=1
            (0x0001, 16, "66d3e0", 0x0000, 1, 1, 0),      # shl ax, cl(16)
            (0x8000, 16, "66d3e8", 0x0000, 1, 1, 0),      # shr ax, cl(16)
        ]
        for v16, cl, hx, want16, cf, zf, sf in cases16:
            with self.subTest(width=16, v16=hex(v16), cl=cl, hx=hx):
                want_flags = cf | (zf << 1) | (sf << 2)
                code = _imm64(0, upper16 | v16)
                if cl is not None:
                    code += _imm64(1, cl)
                code += (
                    bytes.fromhex(hx) + _pack_cf_zf_sf_to_edi()
                    + _imm64(2, upper16 | want16)
                    + bytes.fromhex("4839d0" "0f94c0" "0fb6c0" "c1e704" "09c7" "c3")
                )
                self.assertEqual(self.run_code(code), (want_flags << 4) | 1)

    def test_8bit_and_16bit_rol_ror_imm_and_cl_and_preserve_zf_sf(self):
        upper8 = 0xCAFE_BABE_1234_5600
        # Setup ZF=1, SF=0 before rol/ror via `xor ebx, ebx` (31 db) -> rol/ror MUST keep ZF=1, SF=0!
        cases8 = [
            # (v8, cl_or_None, hx, want8, want_cf)
            (0x81, None, "d0c0", 0x03, 1),          # rol al, 1
            (0x81, None, "d0c8", 0xC0, 1),          # ror al, 1
            (0x81, None, "c0c003", 0x0C, 0),        # rol al, 3
            (0x81, None, "c0c803", 0x30, 0),        # ror al, 3
            (0x81, None, "c0c008", 0x81, 1),        # rol al, 8 (value unchanged, CF = LSB = 1)
            (0x80, None, "c0c808", 0x80, 1),        # ror al, 8 (value unchanged, CF = MSB = 1)
            (0x81, 1, "d2c0", 0x03, 1),             # rol al, cl(1)
            (0x81, 1, "d2c8", 0xC0, 1),             # ror al, cl(1)
            (0x81, 8, "d2c0", 0x81, 1),             # rol al, cl(8)
            (0x81, 9, "d2c0", 0x03, 1),             # rol al, cl(9)
            (0x81, 17, "d2c8", 0xC0, 1),            # ror al, cl(17)
            (0x81, 0, "d2c0", 0x81, 0),             # rol al, cl(0) -> no-op, CF stays 0 from xor ebx, ebx
        ]
        for v8, cl, hx, want8, want_cf in cases8:
            with self.subTest(width=8, v8=hex(v8), cl=cl, hx=hx):
                # xor ebx, ebx sets CF=0, ZF=1, SF=0; rol/ror updates only CF (unless cl==0)
                want_flags = want_cf | (1 << 1)
                code = _imm64(0, upper8 | v8)
                if cl is not None:
                    code += _imm64(1, cl)
                code += (
                    bytes.fromhex("31db")           # xor ebx, ebx (CF=0, ZF=1, SF=0)
                    + bytes.fromhex(hx)
                    + _pack_cf_zf_sf_to_edi()
                    + _imm64(2, upper8 | want8)
                    + bytes.fromhex("4839d0" "0f94c0" "0fb6c0" "c1e704" "09c7" "c3")
                )
                self.assertEqual(self.run_code(code), (want_flags << 4) | 1)

        upper16 = 0x9988_7766_5544_0000
        cases16 = [
            (0x8001, None, "66d1c0", 0x0003, 1),    # rol ax, 1
            (0x8001, None, "66d1c8", 0xC000, 1),    # ror ax, 1
            (0x8001, None, "66c1c010", 0x8001, 1),  # rol ax, 16
            (0x8001, 5, "66d3c0", 0x0030, 0),       # rol ax, cl(5)
            (0x8001, 5, "66d3c8", 0x0C00, 0),       # ror ax, cl(5)
            (0x8001, 17, "66d3c0", 0x0003, 1),      # rol ax, cl(17)
        ]
        for v16, cl, hx, want16, want_cf in cases16:
            with self.subTest(width=16, v16=hex(v16), cl=cl, hx=hx):
                want_flags = want_cf | (1 << 1)
                code = _imm64(0, upper16 | v16)
                if cl is not None:
                    code += _imm64(1, cl)
                code += (
                    bytes.fromhex("31db")
                    + bytes.fromhex(hx)
                    + _pack_cf_zf_sf_to_edi()
                    + _imm64(2, upper16 | want16)
                    + bytes.fromhex("4839d0" "0f94c0" "0fb6c0" "c1e704" "09c7" "c3")
                )
                self.assertEqual(self.run_code(code), (want_flags << 4) | 1)

    def test_materialized_rol_ror_preserves_zf_and_pf(self):
        # Cross-block materialization: cmp sets ZF=1, rol updates only CF, jmp forces both into ctx.eflags;
        # next block reads BOTH ZF (setz) and CF (setc) from ctx.eflags.
        # Case 1: cmp ebx, ebx (ZF=1, CF=0) -> rol eax, 1 (eax=1 -> CF=0) -> jmp +0 -> pack CF/ZF/SF -> want ZF=1, CF=0, SF=0
        code_zf_only = (
            bytes.fromhex("b801000000" "bb05000000" "39db" "d1c0" "eb00")
            + _pack_cf_zf_sf_to_edi()
            + bytes.fromhex("c3")
        )
        self.assertEqual(self.run_code(code_zf_only), 0b010)

        # Case 2: cmp ebx, ebx (ZF=1, CF=0) -> rol eax, 1 (eax=0x80000000 -> CF=1) -> jmp +0 -> want ZF=1, CF=1, SF=0
        code_zf_and_cf = (
            bytes.fromhex("b800000080" "bb05000000" "39db" "d1c0" "eb00")
            + _pack_cf_zf_sf_to_edi()
            + bytes.fromhex("c3")
        )
        self.assertEqual(self.run_code(code_zf_and_cf), 0b011)

        # Case 3: cmp ebx, edx (5 - 3 -> ZF=0, CF=0) -> rol al, cl (al=0x80, cl=1 -> CF=1) -> jmp +0 -> want ZF=0, CF=1, SF=0
        code_var_cf = (
            bytes.fromhex("b880000000" "b901000000" "bb05000000" "ba03000000" "39d3" "d2c0" "eb00")
            + _pack_cf_zf_sf_to_edi()
            + bytes.fromhex("c3")
        )
        self.assertEqual(self.run_code(code_var_cf), 0b001)

        # Case 4: xor ebx, ebx (PF=1 since 0 has even parity) -> rol al, 1 (al=0x80 -> 0x01, odd parity!) -> jp taken!
        code_jp = bytes.fromhex(
            "b880000000" "31db" "d0c0"
            "7a07" "bf00000000" "eb05" "bf01000000" "c3"
        )
        self.assertEqual(self.run_code(code_jp), 1)


if __name__ == "__main__":
    unittest.main()
