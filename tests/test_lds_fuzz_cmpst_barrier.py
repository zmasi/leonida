"""Unit tests for tools/lds_fuzz.py cmpst setup-read barrier epoch separation."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import lds_fuzz as lf


class CmpstBarrierTests(unittest.TestCase):
    def test_cmpst_cmp_hit_emits_barrier_at_full_exec_before_atomic(self):
        for wave, mask in ((32, None), (32, 0x0000FFFF), (64, None), (64, 0x000000FF_FFFF00FF)):
            with self.subTest(wave=wave, mask=mask):
                p = lf.Program(1, 0, wave)
                op = {
                    "kind": "cmpst",
                    "addr": (4, 16, 8),
                    "src": 20,
                    "src2": 21,
                    "dst": 22,
                    "exec": mask,
                    "cmp_hit": True,
                }
                full = "s_mov_b64 exec, -1" if wave == 64 else "s_mov_b32 exec_lo, -1"
                lines = p.asm_op(op, wave == 64, full)
                read_ln = f"ds_read_b32 v20, v{lf.ADDR} offset:8"
                cmpst_ln = f"ds_cmpst_rtn_b32 v22, v{lf.ADDR}, v20, v21 offset:8"
                self.assertIn(read_ln, lines)
                self.assertIn(cmpst_ln, lines)
                i_read = lines.index(read_ln)
                i_cmpst = lines.index(cmpst_ln)
                self.assertEqual(
                    lines[i_read + 1 : i_read + 3],
                    ["s_waitcnt lgkmcnt(0)", "s_barrier"],
                )
                self.assertLess(i_read + 2, i_cmpst)
                if mask is not None:
                    exec_Idx = next(i for i, ln in enumerate(lines) if ln.startswith("s_mov_b32 exec_lo,"))
                    self.assertGreater(
                        exec_Idx,
                        i_read + 2,
                        "s_barrier must run at uniform full EXEC before asm_exec narrows EXEC",
                    )
                    self.assertLess(exec_Idx, i_cmpst)
                    self.assertEqual(lines[-1], full)

    def test_cmpst_without_cmp_hit_omits_setup_read_and_inner_barrier(self):
        p = lf.Program(1, 0, 32)
        op = {
            "kind": "cmpst",
            "addr": (4, 0, 0),
            "src": 20,
            "src2": 21,
            "dst": 22,
            "exec": 0x55555555,
            "cmp_hit": False,
        }
        lines = p.asm_op(op, False, "s_mov_b32 exec_lo, -1")
        self.assertFalse(any(ln.startswith("ds_read_b32") for ln in lines))
        self.assertNotIn("s_barrier", lines)
        self.assertIn(f"ds_cmpst_rtn_b32 v22, v{lf.ADDR}, v20, v21 offset:0", lines)

    def test_hw_lines_lowers_cmpst_cmp_hit_barrier_across_gfx10_gfx11_gfx12(self):
        p = lf.Program(1, 0, 32)
        op = {
            "kind": "cmpst",
            "addr": (4, 0, 4),
            "src": 20,
            "src2": 21,
            "dst": 22,
            "exec": 0x00FF00FF,
            "cmp_hit": True,
        }
        p.ops = [op]

        g10 = p.hw_lines("gfx1030")
        i10_rd = g10.index(f"ds_read_b32 v20, v{lf.ADDR} offset:4")
        i10_cs = g10.index(f"ds_cmpst_rtn_b32 v22, v{lf.ADDR}, v20, v21 offset:4")
        self.assertEqual(g10[i10_rd + 1 : i10_rd + 3], ["s_waitcnt lgkmcnt(0)", "s_barrier"])
        self.assertLess(i10_rd + 2, i10_cs)

        g11 = p.hw_lines("gfx1150")
        i11_rd = g11.index(f"ds_load_b32 v20, v{lf.ADDR} offset:4")
        i11_cs = g11.index(f"ds_cmpstore_rtn_b32 v22, v{lf.ADDR}, v21, v20 offset:4")
        self.assertEqual(g11[i11_rd + 1 : i11_rd + 3], ["s_waitcnt lgkmcnt(0)", "s_barrier"])
        self.assertLess(i11_rd + 2, i11_cs)

        g12 = p.hw_lines("gfx1201")
        i12_rd = g12.index(f"ds_load_b32 v20, v{lf.ADDR} offset:4")
        i12_cs = g12.index(f"ds_cmpstore_rtn_b32 v22, v{lf.ADDR}, v21, v20 offset:4")
        self.assertEqual(
            g12[i12_rd + 1 : i12_rd + 4],
            ["s_wait_dscnt 0", "s_barrier_signal -1", "s_barrier_wait -1"],
        )
        self.assertLess(i12_rd + 3, i12_cs)

    def test_seed_20261130_repro_programs_separate_every_cmpst_setup_read(self):
        for idx in (1, 12, 44):
            p = lf.Program(20261130, idx, 32, threads="mix", permute=True, swizzle_exec=True)
            hit_ops = [op for op in p.ops if op["kind"] == "cmpst" and op["cmp_hit"]]
            self.assertTrue(hit_ops, f"expected cmpst with cmp_hit in index {idx}")
            asm_lines = p.asm().splitlines()
            for i, ln in enumerate(asm_lines):
                if ln.startswith("ds_cmpst_rtn_b32 "):
                    # Walk backwards to the preceding barrier; no ds_read_b32 may appear after that barrier.
                    prev_barrier = max(j for j in range(i) if asm_lines[j] == "s_barrier")
                    between = asm_lines[prev_barrier + 1 : i]
                    self.assertFalse(
                        any(b.startswith("ds_read_b32 ") for b in between),
                        f"program {p.name}: ds_read_b32 and ds_cmpst_rtn_b32 share a barrier epoch: {between}",
                    )


if __name__ == "__main__":
    unittest.main()
