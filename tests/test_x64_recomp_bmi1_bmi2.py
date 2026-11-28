"""Tests for VEX-encoded BMI1 and BMI2 emitters in tools/x64_recomp.py.

Covers all 13 VEX.LZ BMI1/BMI2 instructions across 32-bit (VEX.W=0) and 64-bit (VEX.W=1):
- BMI1: andn, blsr, blsmsk, blsi, bextr (tzcnt is tested separately)
- BMI2: bzhi, shlx, sarx, shrx, rorx, mulx, pdep, pext

Verifies:
- Decoding & operand extraction (including extended r8..r15 via VEX.R/X/B,
  memory operands, RIP-relative rorx resolving abs past imm8, VEX.L==1 non-BMI,
  and invalid 0F38 F3 /reg extensions)
- Exact flag semantics for all 6 flag-writers (andn, blsr, blsmsk, blsi, bextr, bzhi)
  across both ARM NZCV and cross-block materialized ctx.eflags
- Full edge cases: 32-bit zero-extension, count masking, start/index >= width,
  mulx hi == lo (hi wins) and rdx aliasing, pdep/pext Morton interleave & sparse 64-bit masks
- Zero flag clobbering across all 7 flag-preserving BMI2 ops (shlx/sarx/shrx/rorx/mulx/pdep/pext)

Static checks run everywhere; execution tests run natively on Apple Silicon when
CAN_RUN_MACHO is available, or under QEMU.

    python -I tests/test_x64_recomp_bmi1_bmi2.py
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


def _vex3(omap, w, vvvv, pp, op, reg, rm, *, mod=3, l=0, sib=None, disp=b"", imm=b""):
    r_bit = 0 if (reg & 8) else 1
    x_bit = 0 if (sib is not None and (sib[1] & 8)) else 1
    b_bit = 0 if ((sib[2] if sib is not None else rm) & 8) else 1
    b1 = (r_bit << 7) | (x_bit << 6) | (b_bit << 5) | (omap & 0x1F)
    b2 = ((w & 1) << 7) | (((~vvvv) & 0xF) << 3) | ((l & 1) << 2) | (pp & 3)
    modrm = ((mod & 3) << 6) | ((reg & 7) << 3) | ((4 if sib is not None else rm) & 7)
    sib_b = bytes([((sib[0] & 3) << 6) | ((sib[1] & 7) << 3) | (sib[2] & 7)]) if sib is not None else b""
    return bytes([0xC4, b1, b2, op, modrm]) + sib_b + disp + imm


def _pack_nzcv_to_edi():
    """Set edi = ZF | (CF<<1) | (SF<<2) | (OF<<3) using r11..r14."""
    return bytes.fromhex(
        "410f94c3"      # setz r11b
        "410f92c4"      # setc r12b
        "410f98c5"      # sets r13b
        "410f90c6"      # seto r14b
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
    )


def _pack_flags_with_pf_to_edi():
    """Set edi = ZF | (CF<<1) | (SF<<2) | (OF<<3) | (PF<<4) before any ALU op alters PF."""
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


class Bmi1Bmi2StaticTests(unittest.TestCase):
    def test_decode_all_13_bmi1_bmi2_and_extended_regs(self):
        # Exact bytes cross-checked with `clang -arch x86_64 -mbmi -mbmi2`
        cases = [
            ("c4e260f2c1", "andn", 5, 4, "NZCV"),              # andn eax, ebx, ecx
            ("c442b0f24210", "andn", 6, 8, "NZCV"),            # andn r8, r9, [r10+16]
            ("c4e278f3c9", "blsr", 5, 4, "NZCV"),              # blsr eax, ecx
            ("c4c2b8f3d1", "blsmsk", 5, 8, "NZCV"),            # blsmsk r8, r9
            ("c4e2f8f35f08", "blsi", 6, 8, "NZCV"),            # blsi rax, [rdi+8]
            ("c4e268f7c1", "bextr", 5, 4, "NZCV"),             # bextr eax, ecx, edx
            ("c442a8f74108", "bextr", 6, 8, "NZCV"),           # bextr r8, [r9+8], r10
            ("c4e268f5c1", "bzhi", 5, 4, "NZCV"),              # bzhi eax, ecx, edx
            ("c442a8f501", "bzhi", 5, 8, "NZCV"),              # bzhi r8, [r9], r10
            ("c4e269f7c1", "shlx", 5, 4, ""),                  # shlx eax, ecx, edx
            ("c442aaf7c1", "sarx", 5, 8, ""),                  # sarx r8, r9, r10
            ("c4e2f3f707", "shrx", 5, 8, ""),                  # shrx rax, [rdi], rcx
            ("c4e37bf0c107", "rorx", 6, 4, ""),                # rorx eax, ecx, 7
            ("c443fbf041080d", "rorx", 7, 8, ""),              # rorx r8, [r9+8], 13
            ("c4e263f6c1", "mulx", 5, 4, ""),                  # mulx eax, ebx, ecx
            ("c442b3f602", "mulx", 5, 8, ""),                  # mulx r8, r9, [r10]
            ("c4e263f5c1", "pdep", 5, 4, ""),                  # pdep eax, ebx, ecx
            ("c442b2f54208", "pext", 6, 8, ""),                # pext r8, r9, [r10+8]
        ]
        for hx, mnem, size, width, fw in cases:
            with self.subTest(hx=hx, mnem=mnem):
                raw = bytes.fromhex(hx)
                ins = xr.decode_one(raw, 0, len(raw), GUEST_VA)
                self.assertFalse(ins.unknown)
                self.assertIsNone(ins.error)
                self.assertEqual(ins.mnem, mnem)
                self.assertEqual(ins.kind, "bmi")
                self.assertEqual(ins.size, size)
                self.assertEqual(ins.flags_w, fw)
                self.assertEqual(ins.flags_r, "")
                self.assertEqual(ins.ops[0][2], width)

        # Verify extended register numbers on `andn r8, r9, [r10+16]`
        ins = xr.decode_one(bytes.fromhex("c442b0f24210"), 0, 6, GUEST_VA)
        self.assertEqual(ins.ops[0], ("r", 8, 8))
        self.assertEqual(ins.ops[1], ("r", 9, 8))
        self.assertEqual(ins.ops[2][0], "m")
        self.assertEqual(ins.ops[2][1]["base"], 10)
        self.assertEqual(ins.ops[2][1]["disp"], 16)

        # Verify RIP-relative rorx resolves abs = addr + size + disp (including imm8 byte!)
        rip_rorx = _vex3(3, 1, 0, 3, 0xF0, 0, 5, mod=0, disp=struct.pack("<i", 0x20), imm=b"\x07")
        ins = xr.decode_one(rip_rorx, 0, len(rip_rorx), GUEST_VA)
        self.assertEqual(ins.mnem, "rorx")
        self.assertEqual(ins.size, 10)
        self.assertTrue(ins.ops[1][1]["rip"])
        self.assertEqual(ins.ops[1][1]["abs"], GUEST_VA + 10 + 0x20)
        self.assertEqual(ins.ops[2], ("i", 7, 1))

        # VEX.L == 1 is NOT BMI (falls through to simd)
        l1 = _vex3(2, 0, 3, 0, 0xF2, 0, 1, l=1)
        ins = xr.decode_one(l1, 0, len(l1), GUEST_VA)
        self.assertNotEqual(ins.kind, "bmi")

        # Invalid 0F38 F3 /0 extension is marked unknown with exact length
        bad_f3 = _vex3(2, 0, 0, 0, 0xF3, 0, 1)
        ins = xr.decode_one(bad_f3, 0, len(bad_f3), GUEST_VA)
        self.assertTrue(ins.unknown)
        self.assertEqual(ins.size, 5)

    def test_flag_writers_and_codegen_all_13(self):
        for m in ("andn", "blsr", "blsmsk", "blsi", "bextr", "bzhi"):
            self.assertEqual(xr.FLAG_WRITERS[m], "NZCV")
        for m in ("shlx", "sarx", "shrx", "rorx", "mulx", "pdep", "pext"):
            self.assertNotIn(m, xr.FLAG_WRITERS)

        # Translate all 13 instructions in both 32-bit and 64-bit forms -> 0 unhandled
        seq = (
            bytes.fromhex("c4e260f2c1") + bytes.fromhex("c442b0f24210")
            + bytes.fromhex("c4e278f3c9") + bytes.fromhex("c4c2b8f3d1")
            + bytes.fromhex("c4e2f8f35f08") + bytes.fromhex("c4e268f7c1")
            + bytes.fromhex("c442a8f74108") + bytes.fromhex("c4e268f5c1")
            + bytes.fromhex("c442a8f501") + bytes.fromhex("c4e269f7c1")
            + bytes.fromhex("c442aaf7c1") + bytes.fromhex("c4e2f3f707")
            + bytes.fromhex("c4e37bf0c107") + bytes.fromhex("c443fbf041080d")
            + bytes.fromhex("c4e263f6c1") + bytes.fromhex("c442b3f602")
            + bytes.fromhex("c4e263f5c1") + bytes.fromhex("c442b2f54208")
            + b"\xc3"
        )
        ft = xr.FunctionTranslator(xr.disasm(seq, GUEST_VA))
        ft.translate()
        self.assertEqual(ft.stats["unhandled"], [])
        self.assertEqual(ft.stats["insns"], 19)


@unittest.skipUnless(RUNNER is not None, "no arm64 runner available")
class Bmi1Bmi2ExecTests(unittest.TestCase):
    def _run(self, code: bytes) -> int:
        val, ft = RUNNER(code)
        self.assertEqual(ft.stats["unhandled"], [])
        return val

    def test_exec_bmi1_andn_blsr_blsmsk_blsi_bextr(self):
        # 1. andn 32-bit (zero-extends upper 32!) + 64-bit + flags (ARM & cross-block ctx)
        for cross in (False, True):
            with self.subTest(op="andn32", cross=cross):
                # ~0x0F0F0F0F & 0xFF00AA55 = 0xF000A050 -> SF=1, ZF=0, CF=0, OF=0 (mask 0x04)
                code = (
                    _imm64(0, 0xDEAD000000000000)       # dirty upper 32 of RAX
                    + _imm64(3, 0x0F0F0F0F)
                    + _imm64(1, 0xFF00AA55)
                    + _vex3(2, 0, 3, 0, 0xF2, 0, 1)     # andn eax, ebx, ecx
                    + (b"\xeb\x00" if cross else b"")
                    + _pack_nzcv_to_edi()               # edi = 4 (SF=1)
                    + _imm64(10, 0x00000000F000A050)    # upper 32 of RAX MUST be 0!
                    + bytes.fromhex("4c39d0" "410f94c3" "450fb6db" "41c1e304" "4409df" "c3")
                )
                self.assertEqual(self._run(code), 0x14)

        # andn 64-bit zero result -> ZF=1, SF=0, CF=0, OF=0 (mask 0x01)
        code = (
            _imm64(9, 0xFFFFFFFFFFFFFFFF)
            + _imm64(10, 0x123456789ABCDEF0)
            + _vex3(2, 1, 9, 0, 0xF2, 8, 10)            # andn r8, r9, r10 -> 0
            + _pack_nzcv_to_edi()                       # 1 (ZF=1)
            + bytes.fromhex("4d85c0" "410f94c3" "450fb6db" "41c1e304" "4409df" "c3")
        )
        self.assertEqual(self._run(code), 0x11)

        # 2. blsr / blsmsk / blsi across 32 & 64-bit, testing exact ZF/SF/CF/OF on both ARM & ctx
        #    flags bitmask: ZF=1, CF=2, SF=4, OF=8
        f3_cases = [
            # (reg_ext, w, src, exp_res, exp_flags)
            (1, 0, 0, 0, 0x03),                                     # blsr32(0) = 0, ZF=1, CF=1
            (1, 0, 0x80000000, 0, 0x01),                            # blsr32(pow2) = 0, ZF=1, CF=0
            (1, 0, 0x8000000A, 0x80000008, 0x04),                   # blsr32 = 0x80000008, SF=1, CF=0
            (1, 1, 0x8000000000000003, 0x8000000000000002, 0x04),   # blsr64
            (2, 0, 0, 0xFFFFFFFF, 0x06),                            # blsmsk32(0) = -1, SF=1, CF=1, ZF=0
            (2, 0, 0x00000010, 0x0000001F, 0x00),                   # blsmsk32(0x10) = 0x1F, all 0
            (2, 1, 0, 0xFFFFFFFFFFFFFFFF, 0x06),                    # blsmsk64(0) = -1, SF=1, CF=1
            (3, 0, 0, 0, 0x01),                                     # blsi32(0) = 0, ZF=1, CF=0!
            (3, 0, 0x80000000, 0x80000000, 0x06),                   # blsi32(0x80000000) -> SF=1, CF=1!
            (3, 0, 0x00000014, 0x00000004, 0x02),                   # blsi32(0x14) = 4, CF=1
            (3, 1, 0x8000000000000000, 0x8000000000000000, 0x06),   # blsi64(1<<63) -> SF=1, CF=1
        ]
        for reg_ext, w, src_val, exp_res, exp_flags in f3_cases:
            for cross in (False, True):
                with self.subTest(reg_ext=reg_ext, w=w, src=hex(src_val), cross=cross):
                    code = (
                        _imm64(0, 0xCAFE000000000000)   # dirty upper 32 of RAX
                        + _imm64(1, src_val)
                        + _vex3(2, w, 0, 0, 0xF3, reg_ext, 1)  # blsr/blsmsk/blsi eax/rax, ecx/rcx
                        + (b"\xeb\x00" if cross else b"")
                        + _pack_nzcv_to_edi()
                        + _imm64(10, exp_res)
                        + bytes.fromhex("4c39d0" "410f94c3" "450fb6db" "41c1e304" "4409df" "c3")
                    )
                    self.assertEqual(self._run(code), 0x10 | exp_flags)

        # 3. bextr 32/64-bit: normal slice, start>=W, len==0, len>=W, start+len>W, aliasing
        bextr_cases = [
            # (w, src, start, length, exp_res, exp_zf)
            (0, 0x12345678, 8, 12, 0x456, 0),
            (0, 0x12345678, 32, 8, 0, 1),               # start >= 32 -> 0, ZF=1
            (0, 0x12345678, 4, 0, 0, 1),                # len == 0 -> 0, ZF=1
            (0, 0x12345678, 0, 255, 0x12345678, 0),     # len >= 32 -> full src
            (0, 0xF2345678, 28, 10, 0xF, 0),            # start + len > 32
            (1, 0xFEDCBA9876543210, 20, 24, 0xA98765, 0),
            (1, 0xFEDCBA9876543210, 64, 8, 0, 1),       # start >= 64 -> 0, ZF=1
            (1, 0xFEDCBA9876543210, 60, 10, 0xF, 0),    # top 4 bits
            (1, 0xFEDCBA9876543210, 0, 64, 0xFEDCBA9876543210, 0),
        ]
        for w, src_val, start_b, len_b, exp_res, exp_zf in bextr_cases:
            with self.subTest(op="bextr", w=w, start=start_b, length=len_b):
                ctrl = (len_b << 8) | start_b
                code = (
                    _imm64(0, 0xDEAD000000000000)
                    + _imm64(1, src_val)
                    + _imm64(2, ctrl)
                    + bytes.fromhex("f9")               # stc (CF=1 before bextr; bextr MUST clear CF=0!)
                    + _vex3(2, w, 2, 0, 0xF7, 0, 1)     # bextr eax/rax, ecx/rcx, edx/rdx
                    + _pack_nzcv_to_edi()               # expect ZF=exp_zf, CF=0, OF=0
                    + bytes.fromhex("83e70b")           # mask to ZF|CF|OF (SF is undefined for bextr)
                    + _imm64(10, exp_res)
                    + bytes.fromhex("4c39d0" "410f94c3" "450fb6db" "41c1e304" "4409df" "c3")
                )
                self.assertEqual(self._run(code), 0x10 | exp_zf)

    def test_exec_bmi2_bzhi_shifts_rorx_mulx(self):
        # 1. bzhi 32/64-bit across boundary indices (0, mid, W-1, W, 255, and >255 wrapping low byte!)
        bzhi_cases = [
            # (w, src, idx, exp_res, exp_flags: ZF|CF<<1|SF<<2|OF<<3)
            (0, 0xFFFFFFFF, 0, 0, 0x01),                        # idx=0 -> 0, ZF=1, CF=0
            (0, 0xFFFFFFFF, 12, 0xFFF, 0x00),                   # idx=12 -> 0xFFF, CF=0
            (0, 0xFFFFFFFF, 31, 0x7FFFFFFF, 0x00),              # idx=31 -> CF=0, SF=0
            (0, 0xFFFFFFFF, 32, 0xFFFFFFFF, 0x06),              # idx=32 -> full src, CF=1, SF=1
            (0, 0x80000001, 255, 0x80000001, 0x06),             # idx=255 -> full src, CF=1, SF=1
            (0, 0xFFFFFFFF, 0x104, 0xF, 0x00),                  # only low 8 bits (4) used -> CF=0!
            (1, 0xFFFFFFFFFFFFFFFF, 63, 0x7FFFFFFFFFFFFFFF, 0x00),
            (1, 0xFFFFFFFFFFFFFFFF, 64, 0xFFFFFFFFFFFFFFFF, 0x06),
            (1, 0x0000000000000000, 64, 0, 0x03),               # src=0, idx=64 -> ZF=1, CF=1
        ]
        for w, src_val, idx_val, exp_res, exp_flags in bzhi_cases:
            for cross in (False, True):
                with self.subTest(op="bzhi", w=w, idx=hex(idx_val), cross=cross):
                    code = (
                        _imm64(0, 0xBEEF000000000000)
                        + _imm64(1, src_val)
                        + _imm64(2, idx_val)
                        + _vex3(2, w, 2, 0, 0xF5, 0, 1)         # bzhi eax/rax, ecx/rcx, edx/rdx
                        + (b"\xeb\x00" if cross else b"")
                        + _pack_nzcv_to_edi()
                        + _imm64(10, exp_res)
                        + bytes.fromhex("4c39d0" "410f94c3" "450fb6db" "41c1e304" "4409df" "c3")
                    )
                    self.assertEqual(self._run(code), 0x10 | exp_flags)

        # 2. shlx / sarx / shrx 32 & 64-bit (including count masking mod 32/64 & sign extension)
        shift_cases = [
            # (pp, w, src, cnt, exp_res)
            (1, 0, 0x12345678, 36, (0x12345678 << 4) & 0xFFFFFFFF),     # shlx32 cnt=36 -> shift 4
            (1, 1, 0x0000000012345678, 36, (0x12345678 << 36) & ((1 << 64) - 1)),  # shlx64 cnt=36
            (2, 0, 0x80000040, 4, 0xF8000004),                          # sarx32 negative sign-ext
            (2, 1, 0x8000000000000040, 68, 0xF800000000000004),         # sarx64 cnt=68 -> shift 4
            (3, 0, 0x80000040, 4, 0x08000004),                          # shrx32 logical
            (3, 1, 0x8000000000000040, 4, 0x0800000000000004),          # shrx64 logical
        ]
        for pp, w, src_val, cnt_val, exp_res in shift_cases:
            with self.subTest(op=("shlx", "sarx", "shrx")[pp - 1], w=w, cnt=cnt_val):
                code = (
                    _imm64(0, 0xAAAA000000000000)
                    + _imm64(1, src_val)
                    + _imm64(2, cnt_val)
                    + _vex3(2, w, 2, pp, 0xF7, 0, 1)            # shlx/sarx/shrx eax/rax, ecx/rcx, edx/rdx
                    + _imm64(10, exp_res)
                    + bytes.fromhex("4c39d0" "0f94c0" "0fb6f8" "c3")
                )
                self.assertEqual(self._run(code), 1)

        # 3. rorx 32 & 64-bit (imm=0, normal, imm=37 -> 5 in 32-bit vs 37 in 64-bit)
        for w, src_val, imm, exp_res in (
            (0, 0x12345678, 0, 0x12345678),
            (0, 0x12345678, 8, 0x78123456),
            (0, 0x12345678, 36, 0x81234567),                    # 36 & 31 == 4
            (1, 0x0123456789ABCDEF, 16, 0xCDEF0123456789AB),
            (1, 0x0123456789ABCDEF, 36, 0x789ABCDEF0123456),
        ):
            with self.subTest(op="rorx", w=w, imm=imm):
                code = (
                    _imm64(0, 0xCCCC000000000000)
                    + _imm64(1, src_val)
                    + _vex3(3, w, 0, 3, 0xF0, 0, 1, imm=bytes([imm]))
                    + _imm64(10, exp_res)
                    + bytes.fromhex("4c39d0" "0f94c0" "0fb6f8" "c3")
                )
                self.assertEqual(self._run(code), 1)

        # 4. mulx 32 & 64-bit, including hi == lo (hi wins!) and hi/lo == rdx aliasing
        #    32-bit: 0xFFFFFFFF * 0xFFFFFFFF = 0xFFFFFFFE_00000001
        code = (
            _imm64(0, 0xAAAA000000000000)
            + _imm64(3, 0xBBBB000000000000)
            + _imm64(2, 0xDEAD0000FFFFFFFF)                     # edx = 0xFFFFFFFF
            + _imm64(1, 0xBEEF0000FFFFFFFF)                     # ecx = 0xFFFFFFFF
            + _vex3(2, 0, 3, 3, 0xF6, 0, 1)                     # mulx eax(hi), ebx(lo), ecx
            + _imm64(10, 0x00000000FFFFFFFE)
            + bytes.fromhex("4c39d0" "410f94c3" "410fb6fb")     # bit0: hi == 0xFFFFFFFE (zero-ext)
            + _imm64(10, 0x0000000000000001)
            + bytes.fromhex("4c39d3" "410f94c3" "450fb6db" "41d1e3" "4409df" "c3")  # bit1: lo == 1
        )
        self.assertEqual(self._run(code), 0x03)

        #    64-bit: 0xFFFFFFFFFFFFFFFF * 0xFFFFFFFFFFFFFFFF = 0xFFFFFFFFFFFFFFFE_0000000000000001
        code = (
            _imm64(2, 0xFFFFFFFFFFFFFFFF)
            + _imm64(1, 0xFFFFFFFFFFFFFFFF)
            + _vex3(2, 1, 9, 3, 0xF6, 8, 1)                     # mulx r8(hi), r9(lo), rcx
            + _imm64(10, 0xFFFFFFFFFFFFFFFE)
            + bytes.fromhex("4d39d0" "410f94c3" "410fb6fb")     # bit0: hi
            + _imm64(10, 0x0000000000000001)
            + bytes.fromhex("4d39d1" "410f94c3" "450fb6db" "41d1e3" "4409df")  # bit1: lo
            # hi == lo == rdx aliasing: mulx rdx, rdx, rcx -> rdx gets HI half (0xFFFFFFFFFFFFFFFE)
            + _vex3(2, 1, 2, 3, 0xF6, 2, 1)                     # mulx rdx, rdx, rcx
            + _imm64(10, 0xFFFFFFFFFFFFFFFE)
            + bytes.fromhex("4c39d2" "410f94c3" "450fb6db" "41c1e302" "4409df" "c3")  # bit2: hi wins!
        )
        self.assertEqual(self._run(code), 0x07)

    def test_exec_bmi2_pdep_pext_and_flag_preservation(self):
        # 1. pdep & pext 32-bit Morton interleave + zero/all-ones masks + upper-32 zero-extend
        #    x = 0x0000A55A -> pdep(x, 0x55555555) puts bits at even positions;
        #    pext(that, 0x55555555) recovers 0x0000A55A!
        def pdep_ref(src, mask, bits):
            res = 0
            k = 0
            for i in range(bits):
                if (mask >> i) & 1:
                    if (src >> k) & 1:
                        res |= 1 << i
                    k += 1
            return res

        def pext_ref(src, mask, bits):
            res = 0
            k = 0
            for i in range(bits):
                if (mask >> i) & 1:
                    if (src >> i) & 1:
                        res |= 1 << k
                    k += 1
            return res

        for w, src_val, mask_val in (
            (0, 0x12345678, 0x00000000),
            (0, 0x12345678, 0xFFFFFFFF),
            (0, 0x0000A55A, 0x55555555),
            (0, 0xDEADBEEF, 0x80F00F01),
            (1, 0x0123456789ABCDEF, 0x0000000000000000),
            (1, 0x0123456789ABCDEF, 0xFFFFFFFFFFFFFFFF),
            (1, 0xFEDCBA9876543210, 0x8000F000000F0001),
            (1, 0x00000000A55A3C3C, 0x5555555555555555),
        ):
            bits = 64 if w else 32
            exp_dep = pdep_ref(src_val, mask_val, bits)
            exp_ext = pext_ref(src_val, mask_val, bits)
            with self.subTest(op="pdep_pext", w=w, mask=hex(mask_val)):
                code = (
                    _imm64(0, 0xDEAD000000000000)       # dirty upper 32 of RAX
                    + _imm64(8, 0xBEEF000000000000)     # dirty upper 32 of R8
                    + _imm64(3, src_val)
                    + _imm64(1, mask_val)
                    + _vex3(2, w, 3, 3, 0xF5, 0, 1)     # pdep eax/rax, ebx/rbx, ecx/rcx
                    + _vex3(2, w, 3, 2, 0xF5, 8, 1)     # pext r8d/r8, ebx/rbx, ecx/rcx
                    + _imm64(10, exp_dep)
                    + bytes.fromhex("4c39d0" "410f94c3" "410fb6fb")  # bit0: pdep exact (incl zero-ext)
                    + _imm64(10, exp_ext)
                    + bytes.fromhex("4d39d0" "410f94c3" "450fb6db" "41d1e3" "4409df" "c3")  # bit1: pext
                )
                self.assertEqual(self._run(code), 0x03)

        # 2. All 7 flag-preserving BMI2 ops (shlx/sarx/shrx/rorx/mulx/pdep/pext)
        #    MUST preserve CF/ZF/SF/OF/PF across both ARM NZCV and cross-block ctx.eflags!
        bmi2_chain = (
            _vex3(2, 1, 1, 1, 0xF7, 0, 3)               # shlx rax, rbx, rcx
            + _vex3(2, 1, 1, 2, 0xF7, 0, 0)             # sarx rax, rax, rcx
            + _vex3(2, 1, 1, 3, 0xF7, 0, 0)             # shrx rax, rax, rcx
            + _vex3(3, 1, 0, 3, 0xF0, 0, 0, imm=b"\x0b")  # rorx rax, rax, 11
            + _vex3(2, 1, 8, 3, 0xF6, 0, 3)             # mulx rax, r8, rbx
            + _vex3(2, 1, 0, 3, 0xF5, 0, 3)             # pdep rax, rax, rbx
            + _vex3(2, 1, 0, 2, 0xF5, 0, 3)             # pext rax, rax, rbx
        )
        for cross in (False, True):
            for setup_hex, expected_mask in (
                ("41b900000000" "4183e901", 0x16),      # 0 - 1 -> CF=1, SF=1, OF=0, PF=1, ZF=0
                ("41b900000080" "4183e901", 0x18),      # 0x80000000 - 1 -> CF=0, SF=0, OF=1, PF=1, ZF=0
            ):
                with self.subTest(flags_preserve=True, cross=cross, expected=hex(expected_mask)):
                    code = (
                        _imm64(3, 0x80F00F0F55AA3311)
                        + _imm64(2, 0x123456789ABCDEF1)
                        + _imm64(1, 5)
                        + bytes.fromhex(setup_hex)
                        + bmi2_chain
                        + (b"\xeb\x00" if cross else b"")
                        + _pack_flags_with_pf_to_edi()
                        + b"\xc3"
                    )
                    self.assertEqual(self._run(code), expected_mask)


if __name__ == "__main__":
    unittest.main()
