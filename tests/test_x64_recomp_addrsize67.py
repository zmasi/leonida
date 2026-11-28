"""Tests for 0x67 address-size override handling in tools/x64_recomp.py.

Verifies 32-bit effective address calculation (modulo 2^32, zero-extended to
64 bits) across ModRM/SIB base/index/disp forms, [disp32], EIP-relative
addressing, and 4-byte moffs (A0..A3) decoding/codegen.
"""

import os
import shutil
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
import x64_recomp as xr  # noqa: E402


def _find_runner():
    if getattr(xr, "CAN_RUN_MACHO", False) and hasattr(xr, "run_result") and not os.environ.get("QEMU_AARCH64"):
        return lambda code, guest_va=0x46000000: xr.run_result(code, guest_va)
    qemu = os.environ.get("QEMU_AARCH64") or shutil.which("qemu-system-aarch64")
    if qemu and os.path.exists(qemu):
        return lambda code, guest_va=0x46000000: xr.run_kernel(code, guest_va)
    return None


RUNNER = _find_runner()


def _mov_rax_imm64(val):
    return b"\x48\xb8" + struct.pack("<Q", val & 0xFFFFFFFFFFFFFFFF)


def _mov_rcx_imm64(val):
    return b"\x48\xb9" + struct.pack("<Q", val & 0xFFFFFFFFFFFFFFFF)


def _mov_r8_imm64(val):
    return b"\x49\xb8" + struct.pack("<Q", val & 0xFFFFFFFFFFFFFFFF)


def _mov_r9_imm64(val):
    return b"\x49\xb9" + struct.pack("<Q", val & 0xFFFFFFFFFFFFFFFF)


class Addrsize67StaticTests(unittest.TestCase):
    def test_decode_moffs_with_and_without_67(self):
        # 67 A1/A0/A2/A3 use a 4-byte moffs32 (not 8-byte moffs64), so the trailing C3 (ret)
        # must decode as a separate instruction instead of being consumed or raising truncated imm.
        for seq, exp_sz, exp_mnem in [
            (b"\x67\xa0\x78\x56\x34\x12\xc3", 6, "mov"),
            (b"\x67\xa1\x78\x56\x34\x12\xc3", 6, "mov"),
            (b"\x67\x48\xa1\x78\x56\x34\x12\xc3", 7, "mov"),
            (b"\x67\xa2\x78\x56\x34\x12\xc3", 6, "mov"),
            (b"\x67\xa3\x78\x56\x34\x12\xc3", 6, "mov"),
        ]:
            insns = list(xr.disasm(seq, base=0x46000000))
            self.assertEqual(len(insns), 2, f"seq={seq.hex()} insns={insns}")
            self.assertEqual(insns[0].mnem, exp_mnem)
            self.assertEqual(insns[0].size, exp_sz)
            self.assertEqual(insns[0].addrsize, 1)
            mem_ops = [o[1] for o in insns[0].ops if o[0] == "m"]
            self.assertEqual(len(mem_ops), 1)
            self.assertEqual(mem_ops[0]["abs"], 0x12345678)
            self.assertEqual(mem_ops[0].get("addrsize"), 4)
            self.assertEqual(insns[1].mnem, "ret")

        # Without 67, moffs is 8 bytes.
        seq64 = b"\xa1" + struct.pack("<Q", 0x0000000112345678) + b"\xc3"
        insns64 = list(xr.disasm(seq64, base=0x46000000))
        self.assertEqual(len(insns64), 2)
        self.assertEqual(insns64[0].size, 9)
        self.assertEqual(insns64[0].ops[1][1]["abs"], 0x0000000112345678)
        self.assertNotIn("addrsize", insns64[0].ops[1][1])

    def test_decode_eip_relative_and_group_mem_addrsize(self):
        base = 0x146000000
        # 67 48 8D 05 10 00 00 00: lea rax, [eip + 0x10] (size 8) -> (base + 8 + 0x10) & 0xFFFFFFFF
        ins67 = list(xr.disasm(b"\x67\x48\x8d\x05\x10\x00\x00\x00", base=base))[0]
        self.assertEqual(ins67.ops[1][1].get("addrsize"), 4)
        self.assertEqual(ins67.ops[1][1]["abs"], 0x46000018)

        # Without 67: 64-bit RIP-relative keeps upper 32 bits.
        ins64 = list(xr.disasm(b"\x48\x8d\x05\x10\x00\x00\x00", base=base))[0]
        self.assertEqual(ins64.ops[1][1]["abs"], 0x146000017)

        # Group instruction (_finish_group): 67 FF 05 10 00 00 00 -> inc dword ptr [eip + 0x10]
        grp67 = list(xr.disasm(b"\x67\xff\x05\x10\x00\x00\x00", base=base))[0]
        self.assertEqual(grp67.mnem, "inc")
        self.assertEqual(grp67.ops[0][1].get("addrsize"), 4)
        self.assertEqual(grp67.ops[0][1]["abs"], 0x46000017)

        # Group1 SIB with 67: 67 83 44 48 08 05 -> add dword ptr [eax + ecx*2 + 8], 5
        grp1_67 = list(xr.disasm(b"\x67\x83\x44\x48\x08\x05", base=base))[0]
        self.assertEqual(grp1_67.mnem, "add")
        self.assertEqual(grp1_67.ops[0][1].get("addrsize"), 4)

    def test_codegen_emits_32bit_arm_address_ops_for_67(self):
        # Verify 67 mov eax, [eax + 4] emits 32-bit ADD W_addr, W_rax, #4 (sf=0, bit31=0),
        # while mov eax, [rax + 4] emits 64-bit ADD X_addr, X_rax, #4 (sf=1, bit31=1).
        def _words(code):
            insns = list(xr.disasm(code, base=0x46000000))
            ft = xr.FunctionTranslator(insns, 0x46000000)
            ft.translate()
            self.assertEqual(ft.stats["unhandled"], [])
            return ft.asm.words

        rd = xr.ADDR_T
        rn = xr.GMAP[0]
        add_w = 0x11000000 | (4 << 10) | (rn << 5) | rd
        add_x = 0x91000000 | (4 << 10) | (rn << 5) | rd
        w67 = _words(b"\x67\x8b\x40\x04\xc3")
        w64 = _words(b"\x8b\x40\x04\xc3")
        self.assertIn(add_w, w67)
        self.assertNotIn(add_x, w67)
        self.assertIn(add_x, w64)

        # Verify 67 group1 SIB emits 32-bit ADD (shifted reg) with sf=0: 0x0B000000 | rm<<16 | imm6<<10 | rn<<5 | rd
        rm = xr.GMAP[1]
        add_sib_w = 0x0B000000 | (rm << 16) | (1 << 10) | (rn << 5) | rd
        add_sib_x = 0x8B000000 | (rm << 16) | (1 << 10) | (rn << 5) | rd
        w_sib67 = _words(b"\x67\x83\x44\x48\x08\x05\xc3")
        self.assertIn(add_sib_w, w_sib67)
        self.assertNotIn(add_sib_x, w_sib67)


@unittest.skipIf(RUNNER is None, "no arm64 runner available")
class Addrsize67ExecTests(unittest.TestCase):
    def _run_rax64(self, body, guest_va=0x46000000):
        # run_result / run_kernel reports guest edi (32-bit); read low and high 32-bit halves of rax.
        lo, ft_lo = RUNNER(body + b"\x89\xc7\xc3", guest_va=guest_va)
        self.assertEqual(ft_lo.stats["unhandled"], [])
        hi, ft_hi = RUNNER(body + b"\x48\xc1\xe8\x20\x89\xc7\xc3", guest_va=guest_va)
        self.assertEqual(ft_hi.stats["unhandled"], [])
        return ((hi & 0xFFFFFFFF) << 32) | (lo & 0xFFFFFFFF)

    def test_exec_lea_base_only_wrap_and_zero_extend(self):
        cases = [
            # 67 48 8D 00: lea rax, [eax] (disp == 0, dst == base -> must zero-extend)
            ("base_disp0", _mov_rax_imm64(0xDEADBEEF12345678) + b"\x67\x48\x8d\x00", 0x12345678),
            # 67 48 8D 40 01: lea rax, [eax + 1] wraps 0xFFFFFFFF -> 0
            ("base_plus1_wrap", _mov_rax_imm64(0xDEADBEEFFFFFFFFF) + b"\x67\x48\x8d\x40\x01", 0),
            # 67 48 8D 40 FF: lea rax, [eax - 1] wraps 0 -> 0xFFFFFFFF
            ("base_minus1_wrap", _mov_rax_imm64(0xDEADBEEF00000000) + b"\x67\x48\x8d\x40\xff", 0xFFFFFFFF),
            # 67 48 8D 80 00 02 00 00: disp = +0x200 (256..4095)
            ("base_plus512_wrap", _mov_rax_imm64(0x11112222FFFFFE05) + b"\x67\x48\x8d\x80\x00\x02\x00\x00", 5),
            # 67 48 8D 80 00 FE FF FF: disp = -0x200 (-4095..-257)
            ("base_minus512_wrap", _mov_rax_imm64(0x1111222200000100) + b"\x67\x48\x8d\x80\x00\xfe\xff\xff", 0xFFFFFF00),
            # 67 48 8D 80 00 00 01 00: disp = +0x10000 (>= 4096)
            ("base_large_pos_wrap", _mov_rax_imm64(0x11112222FFFF0007) + b"\x67\x48\x8d\x80\x00\x00\x01\x00", 7),
            # 67 48 8D 80 00 00 FF FF: disp = -0x10000 (<= -4096)
            ("base_large_neg_wrap", _mov_rax_imm64(0x1111222200000005) + b"\x67\x48\x8d\x80\x00\x00\xff\xff", 0xFFFF0005),
            # 67 49 8D 40 01: lea rax, [r8d + 1] (REX.B)
            ("base_r8d_wrap", _mov_r8_imm64(0xCAFEBABEFFFFFFFF) + b"\x67\x49\x8d\x40\x01", 0),
        ]
        for name, code, expected in cases:
            with self.subTest(name=name):
                self.assertEqual(self._run_rax64(code), expected)

    def test_exec_lea_sib_base_index_scale_and_index_only(self):
        cases = [
            # 67 48 8D 44 88 10: lea rax, [eax + ecx*4 + 0x10]
            (
                "sib_base_idx_small_disp",
                _mov_rax_imm64(0xAAAABBBBC0000000)
                + _mov_rcx_imm64(0xCCCCDDDD10000001)
                + b"\x67\x48\x8d\x44\x88\x10",
                0x14,
            ),
            # 67 48 8D 84 C8 00 00 02 00: lea rax, [eax + ecx*8 + 0x20000]
            (
                "sib_base_idx_large_pos_disp",
                _mov_rax_imm64(0x12345678FFFE0000)
                + _mov_rcx_imm64(0x9ABCDEF020000001)
                + b"\x67\x48\x8d\x84\xc8\x00\x00\x02\x00",
                0x8,
            ),
            # 67 48 8D 84 C8 00 00 FE FF: lea rax, [eax + ecx*8 - 0x20000]
            (
                "sib_base_idx_large_neg_disp",
                _mov_rax_imm64(0x1234567800010000)
                + _mov_rcx_imm64(0x9ABCDEF000000001)
                + b"\x67\x48\x8d\x84\xc8\x00\x00\xfe\xff",
                0xFFFF0008,
            ),
            # 67 4B 8D 44 88 FF: lea rax, [r8d + r9d*4 - 1]
            (
                "sib_r8d_r9d_wrap",
                _mov_r8_imm64(0xAAAA000080000000)
                + _mov_r9_imm64(0xBBBB000020000000)
                + b"\x67\x4b\x8d\x44\x88\xff",
                0xFFFFFFFF,
            ),
            # 67 48 8D 04 4D E0 FF FF FF: lea rax, [ecx*2 - 0x20] (index-only, scale=1, neg disp)
            (
                "sib_idx_only_scale2_neg_disp",
                _mov_rcx_imm64(0x1234567880000005)
                + b"\x67\x48\x8d\x04\x4d\xe0\xff\xff\xff",
                0xFFFFFFEA,
            ),
            # 67 48 8D 04 0D 00 00 01 00: lea rax, [ecx*1 + 0x10000] (index-only, scale=0, large disp)
            (
                "sib_idx_only_scale1_large_disp",
                _mov_rcx_imm64(0x12345678FFFF0003)
                + b"\x67\x48\x8d\x04\x0d\x00\x00\x01\x00",
                0x3,
            ),
            # 67 48 8D 04 8D 00 00 00 00: lea rax, [ecx*4] (index-only, scale=2, disp=0)
            (
                "sib_idx_only_scale4_disp0",
                _mov_rcx_imm64(0x9999999940000002)
                + b"\x67\x48\x8d\x04\x8d\x00\x00\x00\x00",
                0x8,
            ),
        ]
        for name, code, expected in cases:
            with self.subTest(name=name):
                self.assertEqual(self._run_rax64(code), expected)

    def test_exec_lea_disp32_only_eip_relative_and_opsize_16_32_64(self):
        # 67 48 8D 04 25 F0 FF FF FF -> [0xFFFFFFF0] zero-extended to 64 bits
        self.assertEqual(
            self._run_rax64(b"\x67\x48\x8d\x04\x25\xf0\xff\xff\xff"),
            0x00000000FFFFFFF0,
        )
        # Contrast without 67 -> sign-extended to 64 bits
        self.assertEqual(
            self._run_rax64(b"\x48\x8d\x04\x25\xf0\xff\xff\xff"),
            0xFFFFFFFFFFFFFFF0,
        )
        # 67 EIP-relative above 4 GiB truncates to 32 bits; 64-bit RIP-relative preserves high bits
        self.assertEqual(
            self._run_rax64(b"\x67\x48\x8d\x05\x10\x00\x00\x00", guest_va=0x146000000),
            0x0000000046000018,
        )
        self.assertEqual(
            self._run_rax64(b"\x48\x8d\x05\x10\x00\x00\x00", guest_va=0x146000000),
            0x0000000146000017,
        )
        # 16-bit operand size with 67: lea ax, [eax + 1] preserves upper 48 bits of rax
        self.assertEqual(
            self._run_rax64(_mov_rax_imm64(0x112233445566FFFF) + b"\x67\x66\x8d\x40\x01"),
            0x1122334455660000,
        )
        # 32-bit operand size with 67: lea eax, [eax + 1] wraps and zero-extends to 64 bits
        self.assertEqual(
            self._run_rax64(_mov_rax_imm64(0x11223344FFFFFFFF) + b"\x67\x8d\x40\x01"),
            0,
        )


if __name__ == "__main__":
    unittest.main()
