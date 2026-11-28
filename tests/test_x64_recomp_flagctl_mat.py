"""Tests for SUB_KIND flag materialization and clc/stc/cmc preservation in tools/x64_recomp.py.

Covers two bugs in flag materialization and carry-flag control:
1. `_maybe_mkflags` previously inverted ARM NZCV.C in-place for SUB_KIND producers
   before calling `rt_mkflags(0)` and never restored it, inverting every same-block
   carry consumer (`jb`, `jae`, `jbe`, `ja`, `setc`, `sbb`, `cmc`, ...) whenever a
   SUB_KIND instruction also materialized flags for a downstream block — including
   the ubiquitous 3-way unsigned compare `cmp a, b; jb less; je equal`.
2. `_emit_flagctl` (`clc`, `stc`, `cmc`) previously called `_maybe_mkflags`, which
   overwrote the entire `ctx.eflags` word instead of patching only `EF_CF` (bit 0).

Static checks run everywhere; execution tests run natively on Apple Silicon when
CAN_RUN_MACHO is available, or under QEMU.

    python -I tests/test_x64_recomp_flagctl_mat.py
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


class FlagctlMatStaticTests(unittest.TestCase):
    def test_maybe_mkflags_uses_kind1_without_clobbering_arm_nzcv(self):
        # cmp eax, ebx; jb +6; je +6; mov edi, 3; ret; mov edi, 1; ret
        code = bytes.fromhex(
            "39d8"              # cmp eax, ebx (materialized because je is in next block!)
            "7208"              # jb less      (same-block consumer via arm_path -> SUB_KIND b.cc)
            "740c"              # je equal
            "bf03000000c3"      # mov edi, 3; ret
            "bf01000000c3"      # mov edi, 1; ret
            "bf02000000c3"      # mov edi, 2; ret
        )
        ft = xr.FunctionTranslator(xr.disasm(code, GUEST_VA))
        ft.translate()
        self.assertEqual(ft.stats["unhandled"], [])
        self.assertIn(GUEST_VA, ft.mat)
        self.assertEqual(ft.arm_path[GUEST_VA + 2].addr, GUEST_VA)
        # Must call rt_mkflags with kind=1 (movz w0, #1 == 0x52800020) and MUST NOT
        # emit `eor x17, x17, #0x20000000` (which inverted ARM NZCV.C in-place).
        a_ref = xr.Asm()
        a_ref.eor_i(xr.GTMP, xr.GTMP, 0x20000000)
        self.assertNotIn(a_ref.words[0], ft.asm.words)
        self.assertIn(0x52800020, ft.asm.words)

    def test_flagctl_mat_patches_cf_without_calling_rt_mkflags(self):
        for hx, name in (
            ("83e801f8eb000f92c00f98c3c3", "clc"),
            ("83e801f9eb000f92c00f98c3c3", "stc"),
            ("83e801f5eb000f92c00f98c3c3", "cmc"),
        ):
            with self.subTest(op=name):
                ft = xr.FunctionTranslator(xr.disasm(bytes.fromhex(hx), GUEST_VA))
                ft.translate()
                self.assertEqual(ft.stats["unhandled"], [])
                # Flagctl instruction at GUEST_VA + 3 is materialized, yet rt_mkflags
                # must be called only ONCE (for `sub`), not a second time for clc/stc/cmc.
                self.assertIn(GUEST_VA + 3, ft.mat)
                mkflags_calls = [f for f in ft.asm.fixups if f[2] == "rt_mkflags"]
                self.assertEqual(len(mkflags_calls), 1)


@unittest.skipUnless(RUNNER is not None, "no arm64 runner available")
class FlagctlMatExecTests(unittest.TestCase):
    def _run(self, code: bytes) -> int:
        val, ft = RUNNER(code)
        self.assertEqual(ft.stats["unhandled"], [])
        return val

    def test_exec_three_way_unsigned_cmp_jb_je(self):
        # Classic compiler 3-way unsigned compare: cmp eax, ebx; jb -> 1; je -> 2; else -> 3
        for a_val, b_val, expected in (
            (3, 5, 1),
            (5, 5, 2),
            (9, 5, 3),
            (0, 1, 1),
            (0x7FFFFFFF, 0x80000000, 1),
            (0x80000000, 0x80000000, 2),
            (0x80000000, 0x7FFFFFFF, 3),
        ):
            with self.subTest(a=hex(a_val), b=hex(b_val)):
                code = (
                    b"\xb8" + struct.pack("<I", a_val)      # mov eax, a_val
                    + b"\xbb" + struct.pack("<I", b_val)    # mov ebx, b_val
                    + bytes.fromhex(
                        "39d8"              # cmp eax, ebx
                        "7208"              # jb less   (+8 -> mov edi, 1)
                        "740c"              # je equal  (+12 -> mov edi, 2)
                        "bf03000000c3"      # mov edi, 3; ret (6 bytes)
                        "bf01000000c3"      # mov edi, 1; ret (6 bytes)
                        "bf02000000c3"      # mov edi, 2; ret (6 bytes)
                    )
                )
                self.assertEqual(self._run(code), expected)

    def test_exec_materialized_sub_same_block_setc_sbb_cmc(self):
        # 1. Materialized `sub` followed by same-block `setc` AND cross-block `sets`
        for a_val, b_val, exp_cf, exp_sf in (
            (0, 1, 1, 1),       # 0 - 1 = 0xFFFFFFFF -> CF=1, SF=1
            (5, 3, 0, 0),       # 5 - 3 = 2 -> CF=0, SF=0
            (2, 10, 1, 1),      # 2 - 10 -> CF=1, SF=1
        ):
            with self.subTest(case="sub_setc_cross_sets", a=a_val, b=b_val):
                code = (
                    b"\xb8" + struct.pack("<I", a_val)
                    + b"\xbb" + struct.pack("<I", b_val)
                    + bytes.fromhex(
                        "29d8"              # sub eax, ebx (materialized for cross-block sets!)
                        "410f92c3"          # setc r11b (same-block SUB_KIND arm_path!)
                        "eb00"              # jmp +0
                        "410f98c4"          # sets r12b (cross-block ctx.eflags!)
                        "410fb6fb"          # movzx edi, r11b
                        "450fb6e4"          # movzx r12d, r12b
                        "41d1e4"            # shl r12d, 1
                        "4409e7"            # or edi, r12d
                        "c3"
                    )
                )
                self.assertEqual(self._run(code), exp_cf | (exp_sf << 1))

        # 2. Materialized `sub` followed by same-block `sbb` (borrow must NOT be inverted!)
        #    0 - 1 -> CF=1, SF=1; then same-block `sbb edx, 0` (10 - 0 - 1 == 9);
        #    then cross-block `sets` on the `sbb`? Force `sub` itself into self.mat by
        #    putting `jns` right after `sub`? Better: `sub` -> `sbb` -> `sbb` kills flags,
        #    so put `sub` -> `setc` + `adc`/`sbb`? Use `sub` -> `jb +2` -> `sbb`?
        #    Actually `sub` followed by `setz` + `cmc` + `jmp +0` + `sets` forces `sub` into self.mat
        #    (since `cmc` writes ONLY `C`, `N` from `sub` is live out of the block!).
        for a_val, b_val, exp_mask in (
            (0, 1, 0x04),       # sub: CF=1, SF=1, ZF=0, OF=0 -> cmc flips CF to 0 -> SF=4
            (5, 3, 0x02),       # sub: CF=0, SF=0, ZF=0, OF=0 -> cmc flips CF to 1 -> CF=2
            (5, 5, 0x03),       # sub: CF=0, SF=0, ZF=1, OF=0 -> cmc flips CF to 1 -> ZF=1|CF=2
        ):
            with self.subTest(case="sub_cmc_cross_block", a=a_val, b=b_val):
                code = (
                    b"\xb8" + struct.pack("<I", a_val)
                    + b"\xbb" + struct.pack("<I", b_val)
                    + bytes.fromhex("29d8" "f5" "eb00")     # sub eax, ebx; cmc; jmp +0
                    + _pack_nzcv_to_edi()
                    + b"\xc3"
                )
                self.assertEqual(self._run(code), exp_mask)

    def test_exec_flagctl_preserves_other_flags_across_blocks_and_popf(self):
        # 1. Cross-block clc/stc/cmc after `sub 0x80000000 - 1` (OF=1, SF=0, ZF=0, CF=0):
        #    Block 1: sub -> Block 2: stc (CF:=1, OF stays 1!) -> Block 3: read all NZCV -> 0x0A
        code = (
            bytes.fromhex("41b900000080" "4183e901")        # 0x80000000 - 1 -> OF=1, CF=0
            + bytes.fromhex("eb00" "f9" "eb00")             # jmp +0; stc; jmp +0
            + _pack_nzcv_to_edi()                           # CF=2 | OF=8 = 0x0A
            + b"\xc3"
        )
        self.assertEqual(self._run(code), 0x0A)

        # 2. Cross-block `popf` with OF=1, SF=1, ZF=1, CF=1 (0x08C1), then `clc` -> CF=0, OF/SF/ZF intact (0x0D)
        code = (
            bytes.fromhex("68c1080000" "9d")                # push 0x08C1; popf
            + bytes.fromhex("eb00" "f8" "eb00")             # jmp +0; clc; jmp +0
            + _pack_nzcv_to_edi()                           # ZF=1 | SF=4 | OF=8 = 0x0D
            + b"\xc3"
        )
        self.assertEqual(self._run(code), 0x0D)

        # 3. Cross-block `cmc` (prod is None) twice: toggles ONLY CF while preserving OF=1, SF=1, ZF=1
        #    Start 0x08C1 (CF=1) -> jmp +0 -> cmc (CF=0, mask 0x0D) -> jmp +0 -> cmc (CF=1, mask 0x0F)
        code = (
            bytes.fromhex("68c1080000" "9d")                # push 0x08C1; popf (ZF=1,CF=1,SF=1,OF=1)
            + bytes.fromhex("eb00" "f5" "eb00")             # jmp +0; cmc (CF:=0); jmp +0
            + _pack_nzcv_to_edi()                           # edi = 0x0D
            + bytes.fromhex("4189fa")                       # mov r10d, edi (save 0x0D)
            + bytes.fromhex("68c1080000" "9d")              # reload 0x08C1
            + bytes.fromhex("eb00" "f5" "eb00" "f5" "eb00") # cmc twice -> CF=1
            + _pack_nzcv_to_edi()                           # edi = 0x0F
            + bytes.fromhex("41c1e204" "4409d7" "c3")       # (0x0D << 4) | 0x0F = 0xDF
        )
        self.assertEqual(self._run(code), 0xDF)

        # 4. Same-block `xor` (LOGIC_KIND: ZF=1, CF=0) -> `cmc` (CF:=1) -> cross-block NZCV (ZF=1|CF=2 = 0x03)
        code = (
            bytes.fromhex("31c0" "f5" "eb00")               # xor eax, eax; cmc; jmp +0
            + _pack_nzcv_to_edi()
            + b"\xc3"
        )
        self.assertEqual(self._run(code), 0x03)


if __name__ == "__main__":
    unittest.main()
