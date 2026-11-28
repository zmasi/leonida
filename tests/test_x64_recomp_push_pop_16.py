"""Tests for 16-bit operand-size push/pop (66 prefix) in tools/x64_recomp.py.

Covers:
- 66 50..57 / 66 41 50..57 (push r16) decrementing RSP by 2 and storing 2 bytes
- 66 58..5F / 66 41 58..5F (pop r16) loading 2 bytes, incrementing RSP by 2, and preserving upper 48 bits
- 66 68 iw (push imm16) and 66 6A ib (push sign-extended imm8 to 16-bit)
- 66 FF /6 (push m16) and 66 8F /0 (pop m16, including pop word ptr [rsp] using post-increment RSP)
- 66 48 50 / 66 48 58 (REX.W overrides 66 -> 64-bit push/pop)

    python -I tests/test_x64_recomp_push_pop_16.py
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


class PushPop16StaticTests(unittest.TestCase):
    def _words(self, code):
        ft = xr.FunctionTranslator(list(xr.disasm(code, base=GUEST_VA)), GUEST_VA)
        ft.translate()
        self.assertEqual(ft.stats["unhandled"], [])
        return ft.asm.words

    def test_codegen_16bit_vs_64bit_str_pre_and_ldr_post(self):
        rsp = xr.GMAP[4]
        strh_pre_m2 = 0x78000C00 | ((-2 & 0x1FF) << 12) | (rsp << 5) | xr.T0
        ldrh_post_p2 = 0x78400400 | (2 << 12) | (rsp << 5) | xr.T0
        strx_pre_m8 = 0xF8000C00 | ((-8 & 0x1FF) << 12) | (rsp << 5) | xr.T0
        ldrx_post_p8 = 0xF8400400 | (8 << 12) | (rsp << 5) | xr.T0

        # 66 50 (push ax); 66 58 (pop ax); ret -> 16-bit pre/post indexed
        w16 = self._words(bytes.fromhex("66506658c3"))
        self.assertIn(strh_pre_m2, w16)
        self.assertIn(ldrh_post_p2, w16)
        self.assertNotIn(strx_pre_m8, w16)

        # 66 48 50 (REX.W push rax); 66 48 58 (REX.W pop rax); ret -> REX.W wins -> 64-bit
        w64 = self._words(bytes.fromhex("664850664858c3"))
        self.assertIn(strx_pre_m8, w64)
        self.assertIn(ldrx_post_p8, w64)
        self.assertNotIn(strh_pre_m2, w64)

        # 66 ff 30 (push word ptr [rax]); 66 8f 00 (pop word ptr [rax]); ret -> 16-bit mem load/store
        wm16 = self._words(bytes.fromhex("66ff30668f00c3"))
        ldrh_addr = 0x79400000 | (xr.ADDR_T << 5) | xr.T0
        strh_addr = 0x79000000 | (xr.ADDR_T << 5) | xr.T0
        self.assertIn(strh_pre_m2, wm16)
        self.assertIn(ldrh_post_p2, wm16)
        self.assertIn(ldrh_addr, wm16)
        self.assertIn(strh_addr, wm16)


@unittest.skipIf(RUNNER is None, "no arm64 runner available")
class PushPop16ExecTests(unittest.TestCase):
    def _run(self, code):
        val, ft = RUNNER(code)
        self.assertEqual(ft.stats["unhandled"], [])
        return val

    def _run_rax64(self, body):
        lo = self._run(body + b"\x89\xc7\xc3")
        hi = self._run(body + b"\x48\xc1\xe8\x20\x89\xc7\xc3")
        return ((hi & 0xFFFFFFFF) << 32) | (lo & 0xFFFFFFFF)

    def test_exec_push_pop_r16_rsp_delta_and_upper_48bits_preserved(self):
        # Push four 16-bit words (2 bytes each -> total 8 bytes), read them back as one 64-bit qword!
        # Stack grows downward: push 0x1111, push 0x2222, push 0x3333, pushw -2 (0xFFFE)
        # => [rsp..rsp+7] = FE FF 33 33 22 22 11 11 => qword 0x111122223333FFFE.
        body_pack = bytes.fromhex(
            "4889e3"              # mov rbx, rsp
            "66b81111"            # mov ax, 0x1111
            "6650"                # push ax
            "6641b82222"          # mov r8w, 0x2222
            "664150"              # push r8w
            "66683333"            # pushw 0x3333
            "666afe"              # pushw -2 -> 0xFFFE
            "4829e3"              # sub rbx, rsp       (must be 8)
            "58"                  # pop rax            (0x111122223333FFFE, restores rsp!)
            "4883fb08"            # cmp rbx, 8
            "7403"                # je +3
            "4831c0"              # xor rax, rax       (fail marker if rsp delta != 8)
        )
        self.assertEqual(self._run_rax64(body_pack), 0x111122223333FFFE)

        # pop r16 must preserve upper 48 bits of both rax and r8, and restore rsp by +2 each.
        body_pop_rax = (
            b"\x48\xb8" + struct.pack("<Q", 0xCAFEBABE12340000)
            + bytes.fromhex(
                "4889e3"          # mov rbx, rsp
                "6668cdab"        # pushw 0xABCD
                "6658"            # pop ax
                "4839e3"          # cmp rbx, rsp
                "7403"            # je +3
                "4831c0"          # xor rax, rax
            )
        )
        self.assertEqual(self._run_rax64(body_pop_rax), 0xCAFEBABE1234ABCD)

        body_pop_r8 = (
            b"\x49\xb8" + struct.pack("<Q", 0xDEADBEEF56780000)
            + bytes.fromhex(
                "4889e3"          # mov rbx, rsp
                "666aff"          # pushw -1 -> 0xFFFF
                "664158"          # pop r8w
                "4c89c0"          # mov rax, r8
                "4839e3"          # cmp rbx, rsp
                "7403"            # je +3
                "4831c0"          # xor rax, rax
            )
        )
        self.assertEqual(self._run_rax64(body_pop_r8), 0xDEADBEEF5678FFFF)

    def test_exec_push_pop_m16_and_rexw_override(self):
        # Allocate 8-byte sentinel slot on stack = 0x1122334455667788.
        # Push word [rsp] (0x7788), push 0xBEEF, pop word [rsp+4] -> overwrites ONLY bytes [2..3]
        # of the original qword (post-increment RSP+2 -> [orig_rsp + 2] = 0xBEEF),
        # leaving bytes [0..1] (0x7788) and [4..7] (0x11223344) completely untouched!
        body_mem = (
            b"\x48\xb8" + struct.pack("<Q", 0x1122334455667788)
            + bytes.fromhex(
                "50"              # push rax           ([rsp] = 11223344_55667788)
                "4889e1"          # mov rcx, rsp       (rcx -> sentinel qword)
                "66ff31"          # push word ptr [rcx] (pushes 0x7788; rsp = rcx - 2)
                "6668efbe"        # pushw 0xBEEF       (rsp = rcx - 4)
                "668f442404"      # pop word ptr [rsp+4] (post-inc rsp = rcx-2; [rsp+4] = rcx+2 := 0xBEEF!)
                "665a"            # pop dx             (pops 0x7788; rsp = rcx)
                "58"              # pop rax            (must be 0x11223344_BEEF_7788!)
                "6681fa8877"      # cmp dx, 0x7788
                "7403"            # je +3
                "4831c0"          # xor rax, rax
            )
        )
        self.assertEqual(self._run_rax64(body_mem), 0x11223344BEEF7788)

        # 66 48 50 / 66 48 58: REX.W overrides 66 -> full 64-bit push/pop (rsp delta == 8)
        body_rexw = (
            b"\x48\xb8" + struct.pack("<Q", 0x0123456789ABCDEF)
            + bytes.fromhex(
                "4889e3"          # mov rbx, rsp
                "664850"          # 66 REX.W push rax  (rsp -= 8)
                "4829e3"          # sub rbx, rsp       (must be 8)
                "4831c0"          # xor rax, rax
                "664858"          # 66 REX.W pop rax   (full 64-bit restored)
                "4883fb08"        # cmp rbx, 8
                "7403"            # je +3
                "4831c0"          # xor rax, rax
            )
        )
        self.assertEqual(self._run_rax64(body_rexw), 0x0123456789ABCDEF)


if __name__ == "__main__":
    unittest.main()
