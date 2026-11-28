"""Tests for REX.W precedence over 0x66 operand-size override in tools/x64_recomp.py.

Per Intel SDM Vol. 2A §2.2.1.2, when both 0x66 and REX.W (0x48..0x4F) are present on
general-purpose instructions, REX.W takes precedence (64-bit operand size, 8-byte Iv for
movabs, 4-byte Iz immediates, 8-byte string size) while ins.opsize still records the 0x66
prefix for mandatory-prefix instructions.

    python -I tests/test_x64_recomp_rexw_66.py
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


class RexwOver66StaticTests(unittest.TestCase):
    def test_vsize_gives_rexw_precedence_over_opsize(self):
        self.assertEqual(xr._vsize(0, 0), 4)
        self.assertEqual(xr._vsize(1, 0), 2)
        self.assertEqual(xr._vsize(0, 1), 8)
        self.assertEqual(xr._vsize(1, 1), 8)

    def test_decode_66_rexw_lengths_and_operand_sizes(self):
        # 66 48 B8 <imm64>; ret -> 11-byte movabs rax, imm64 (not 5-byte mov ax, imm16!)
        seq_movabs = b"\x66\x48\xb8" + struct.pack("<Q", 0x1122334455667788) + b"\xc3"
        insns = list(xr.disasm(seq_movabs, base=GUEST_VA))
        self.assertEqual(len(insns), 2)
        self.assertEqual(insns[0].mnem, "mov")
        self.assertEqual(insns[0].size, 11)
        self.assertEqual(insns[0].ops, (("r", 0, 8, False), ("i", 0x1122334455667788, 8)))
        self.assertEqual(insns[0].opsize, 1)
        self.assertEqual(insns[0].rexw, 1)
        self.assertEqual(insns[1].mnem, "ret")

        # 66 48 81 C0 <imm32>; ret -> 8-byte add rax, imm32 (Iz must read 4 bytes, not 2!)
        seq_add = b"\x66\x48\x81\xc0" + struct.pack("<I", 0x12345678) + b"\xc3"
        insns = list(xr.disasm(seq_add, base=GUEST_VA))
        self.assertEqual(len(insns), 2)
        self.assertEqual(insns[0].mnem, "add")
        self.assertEqual(insns[0].size, 8)
        self.assertEqual(insns[0].ops[0], ("r", 0, 8))
        self.assertEqual(insns[0].ops[1], ("i", 0x12345678, 4))
        self.assertEqual(insns[1].mnem, "ret")

        # 66 48 68 <imm32>; ret -> 7-byte push imm32 (Iz must read 4 bytes, not 2!)
        seq_push = b"\x66\x48\x68" + struct.pack("<I", 0x12345678) + b"\xc3"
        insns = list(xr.disasm(seq_push, base=GUEST_VA))
        self.assertEqual(len(insns), 2)
        self.assertEqual(insns[0].mnem, "push")
        self.assertEqual(insns[0].size, 7)
        self.assertEqual(insns[0].ops[0], ("i", 0x12345678, 4))
        self.assertEqual(insns[1].mnem, "ret")

        # 66 48 01 C8 (add rax, rcx), 66 48 89 C8 (mov rax, rcx), 66 48 A5 (movsq), 66 48 0F C8 (bswap rax)
        ins_add = list(xr.disasm(bytes.fromhex("664801c8"), base=GUEST_VA))[0]
        self.assertEqual(ins_add.ops, (("r", 0, 8), ("r", 1, 8, False)))
        ins_mov = list(xr.disasm(bytes.fromhex("664889c8"), base=GUEST_VA))[0]
        self.assertEqual(ins_mov.ops, (("r", 0, 8), ("r", 1, 8, False)))
        ins_movs = list(xr.disasm(bytes.fromhex("6648a5"), base=GUEST_VA))[0]
        self.assertEqual(ins_movs.strsize, 8)
        ins_bswap = list(xr.disasm(bytes.fromhex("66480fc8"), base=GUEST_VA))[0]
        self.assertEqual(ins_bswap.ops, (("r", 0, 8, False),))

        # Without REX.W, 66 still selects 16-bit operands and 2-byte Iz immediates.
        ins_add16 = list(xr.disasm(bytes.fromhex("6681c03412c3"), base=GUEST_VA))
        self.assertEqual(len(ins_add16), 2)
        self.assertEqual(ins_add16[0].size, 5)
        self.assertEqual(ins_add16[0].ops, (("r", 0, 2), ("i", 0x1234, 2)))


@unittest.skipIf(RUNNER is None, "no arm64 runner available")
class RexwOver66ExecTests(unittest.TestCase):
    def _run(self, code):
        val, ft = RUNNER(code)
        self.assertEqual(ft.stats["unhandled"], [])
        return val

    def _run_rax64(self, body):
        lo = self._run(body + b"\x89\xc7\xc3")
        hi = self._run(body + b"\x48\xc1\xe8\x20\x89\xc7\xc3")
        return ((hi & 0xFFFFFFFF) << 32) | (lo & 0xFFFFFFFF)

    def test_exec_66_rexw_movabs_add_mov_push_bswap(self):
        # 66 48 B8 <imm64> + 66 48 81 C0 <imm32>: full 64-bit load and add with 4-byte imm32
        body = (
            b"\x66\x48\xb8" + struct.pack("<Q", 0x1122334400000001)
            + b"\x66\x48\x81\xc0" + struct.pack("<I", 0x12345678)
        )
        self.assertEqual(self._run_rax64(body), 0x1122334412345679)

        # 66 48 01 C8 (add rax, rcx) carries across bit 16 and bit 32!
        body_add = (
            b"\x48\xb8" + struct.pack("<Q", 0x00000001FFFFFFFF)
            + b"\x48\xb9" + struct.pack("<Q", 0x0000000200000002)
            + bytes.fromhex("664801c8")
        )
        self.assertEqual(self._run_rax64(body_add), 0x0000000400000001)

        # 66 48 0F C8 (bswap rax): reverses all 8 bytes (not 4 bytes or 2 bytes)
        body_bswap = (
            b"\x48\xb8" + struct.pack("<Q", 0x0102030405060708)
            + bytes.fromhex("66480fc8")
        )
        self.assertEqual(self._run_rax64(body_bswap), 0x0807060504030201)

        # 66 48 68 <imm32>; pop rax: pushes full 64-bit sign-extended value (rsp -= 8)
        body_push = (
            b"\x66\x48\x68" + struct.pack("<i", -0x123456)
            + b"\x58"
        )
        self.assertEqual(self._run_rax64(body_push), (-0x123456) & 0xFFFFFFFFFFFFFFFF)


if __name__ == "__main__":
    unittest.main()
