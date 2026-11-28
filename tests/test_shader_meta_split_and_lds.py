"""Unit tests for tools/shader_meta.py operand splitting, LDS V# detection, SMEM atomics, and VGPR swap/rel vscale.

    python -I tests/test_shader_meta_split_and_lds.py
"""
import os
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS_DIR = os.environ.get("LEONIDA_TOOLS_DIR") or os.path.join(ROOT, "tools")
sys.path.insert(0, TOOLS_DIR)

import shader_ir  # noqa: E402
import shader_meta as M  # noqa: E402


def analyse(lines, user_sgpr_limit=16):
    insns = [shader_ir.parse_line("\t" + ln) for ln in lines]
    return M.analyse_instructions([insns], [[]], user_sgpr_limit=user_sgpr_limit)


class ShaderMetaSplitAndLdsTests(unittest.TestCase):
    def test_short_v_add_co_ci_u32_reads_src0_user_sgpr_instead_of_writing_it(self):
        # 3-operand short form: v_add_co_ci_u32 vD, src0, src1 (implicit VCC carry-out and carry-in).
        for op in ("v_add_co_ci_u32", "v_sub_co_ci_u32", "v_subrev_co_ci_u32"):
            with self.subTest(op=op):
                ins = shader_ir.parse_line(f"\t{op} v0, s4, v1")
                reads, writes = M._split_operands(ins)
                self.assertEqual(reads, [4])
                self.assertEqual(writes, [])
                res = analyse([f"{op} v0, s4, v1", "s_endpgm"])
                self.assertEqual(res["user_sgpr_reads"], [{"sgpr": 4, "kind": "const32"}])

        # 5-operand e64 form still treats a[1] as scalar carry-out dst and a[2:] as reads.
        ins5 = shader_ir.parse_line("\tv_add_co_ci_u32_e64 v0, s6, s4, v1, s8")
        reads, writes = M._split_operands(ins5)
        self.assertEqual(reads, [4, 8])
        self.assertEqual(writes, [6])

    def test_smem_atomics_read_data_sgpr_write_only_on_glc_and_classify_ptr_vsharp(self):
        # Without glc: a[0] is read-only data; a[1] is ptr64 (s_atomic_) or vsharp (s_buffer_atomic_).
        res_ptr = analyse([
            "s_atomic_add s0, s[2:3], 0",
            "s_endpgm",
        ])
        by_sgpr = {e["sgpr"]: e["kind"] for e in res_ptr["user_sgpr_reads"]}
        self.assertEqual(by_sgpr, {0: "const32", 2: "ptr64_lo", 3: "ptr64_hi"})

        # Without glc, s0 is NOT clobbered so a subsequent load using s[0:1] still sees user s0.
        ins_noglc = shader_ir.parse_line("\ts_atomic_add s0, s[2:3], 0")
        self.assertEqual(M._split_operands(ins_noglc), ([0, 2, 3], []))

        # With glc, a[0] is both read and written.
        ins_glc = shader_ir.parse_line("\ts_atomic_add s0, s[2:3], 0 glc")
        self.assertEqual(M._split_operands(ins_glc), ([0, 2, 3], [0]))

        # s_buffer_atomic_ classifies a[1] as vsharp and reads a[0].
        res_buf = analyse([
            "s_buffer_atomic_add s0, s[4:7], 0",
            "s_endpgm",
        ])
        by_sgpr = {e["sgpr"]: e["kind"] for e in res_buf["user_sgpr_reads"]}
        self.assertEqual(by_sgpr, {0: "const32", 4: "vsharp", 5: "vsharp", 6: "vsharp", 7: "vsharp"})

    def test_buffer_and_tbuffer_load_lds_reads_vsharp_from_a1(self):
        for line in (
            "buffer_load_dword v0, s[4:7], 0 offen lds",
            "buffer_load_dword off, s[4:7], 0 lds",
            "buffer_load_format_x v0, s[4:7], 0 idxen lds",
            "tbuffer_load_format_x v0, s[4:7], 0 format:[BUF_FMT_32_FLOAT] offen lds",
        ):
            with self.subTest(line=line):
                res = analyse([line, "s_endpgm"])
                by_sgpr = {e["sgpr"]: e["kind"] for e in res["user_sgpr_reads"]}
                self.assertEqual(by_sgpr, {4: "vsharp", 5: "vsharp", 6: "vsharp", 7: "vsharp"})

    def test_v_swap_and_v_movreld_update_vscale_tracking(self):
        # v_swap_b32 moves v0's scaled stride to v1 and clears stale stride on v0.
        res_swap = analyse([
            "v_lshlrev_b32_e32 v0, 5, v2",      # v0 scaled by 32
            "v_swap_b32 v0, v1",                 # now v1 has stride 32, v0 has none
            "v_readfirstlane_b32 s8, v0",
            "s_load_dwordx8 s[16:23], s[0:1], s8",
            "s_endpgm",
        ])
        self.assertEqual(res_swap["descriptor_strides"], [])

        res_swap_read = analyse([
            "v_lshlrev_b32_e32 v0, 5, v2",      # v0 scaled by 32
            "v_swap_b32 v0, v1",                 # v1 now holds stride 32
            "v_readfirstlane_b32 s8, v1",
            "s_load_dwordx8 s[16:23], s[0:1], s8",
            "s_endpgm",
        ])
        self.assertEqual(res_swap_read["descriptor_strides"], [32])

        # v_movreld_b32 v0, v3 can overwrite v1 (since 1 >= 0), clearing v1's stale vscale.
        res_rel = analyse([
            "v_lshlrev_b32_e32 v1, 5, v2",
            "v_movreld_b32 v0, v3",
            "v_readfirstlane_b32 s8, v1",
            "s_load_dwordx8 s[16:23], s[0:1], s8",
            "s_endpgm",
        ])
        self.assertEqual(res_rel["descriptor_strides"], [])


if __name__ == "__main__":
    unittest.main()
