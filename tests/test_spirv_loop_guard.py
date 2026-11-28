"""Unit tests for tools/spirv_loop_guard.py (no GPU or game data needed)."""
import argparse
import os
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")
if TOOLS not in sys.path:
    sys.path.insert(0, TOOLS)

import spirv_loop_guard as lg  # noqa: E402


def _simple_loop(newline="\n", with_exit_phi=True):
    lines = [
        "%void = OpTypeVoid",
        "%fn_t = OpTypeFunction %void",
        "%uint = OpTypeInt 32 0",
        "%u0 = OpConstant %uint 0",
        "%main = OpFunction %void None %fn_t",
        "%entry = OpLabel",
        "OpBranch %hdr",
        "%hdr = OpLabel",
        "%iv = OpPhi %uint %u0 %entry %u0 %latch",
        "OpLoopMerge %exit %latch None",
        "OpBranch %body",
        "%body = OpLabel",
        "OpBranch %latch",
        "%latch = OpLabel",
        "OpBranch %hdr",
        "%exit = OpLabel",
    ]
    if with_exit_phi:
        lines.append("%out = OpPhi %uint %iv %hdr")
    lines.extend(["OpReturn", "OpFunctionEnd"])
    return newline.join(lines) + newline


class SpirvLoopGuardTests(unittest.TestCase):
    def test_parse_limit_accepts_bounds_and_rejects_bad_values(self):
        self.assertEqual(lg.parse_limit("0"), 0)
        self.assertEqual(lg.parse_limit("65536"), 65536)
        self.assertEqual(lg.parse_limit("4294967295"), 0xFFFFFFFF)
        for bad in ("-1", "4294967296", "abc", "", "1.5"):
            with self.subTest(bad=bad):
                with self.assertRaises(argparse.ArgumentTypeError):
                    lg.parse_limit(bad)

    def test_add_guards_rejects_booleans_non_ints_and_out_of_range(self):
        sample = _simple_loop()
        for bad in (True, False, -1, 1 << 32, 1.5, "64"):
            with self.subTest(limit=bad):
                with self.assertRaises(ValueError):
                    lg.add_guards(sample, limit=bad)

    def test_zero_limit_or_no_loop_returns_text_unchanged(self):
        sample = _simple_loop()
        self.assertEqual(lg.add_guards(sample, limit=0), sample)
        no_loop = "%void = OpTypeVoid\n%fn = OpTypeFunction %void\n"
        self.assertEqual(lg.add_guards(no_loop, limit=16), no_loop)

    def test_unconditional_back_edge_and_exit_phi_get_bounded(self):
        guarded = lg.add_guards(_simple_loop(), limit=8)
        self.assertIn("%loop_guard_bool = OpTypeBool", guarded)
        self.assertIn("%loop_guard_c1 = OpConstant %uint 1", guarded)
        self.assertIn("%loop_guard_c8 = OpConstant %uint 8", guarded)
        self.assertIn("%loop_guard_0_count = OpPhi %uint %loop_guard_c1 %entry %loop_guard_0_next %latch", guarded)
        self.assertIn("OpBranchConditional %loop_guard_0_room %hdr %exit", guarded)
        self.assertIn("%out = OpPhi %uint %iv %hdr %loop_guard_undef0 %latch", guarded)

    def test_conditional_back_edge_direct_exit_both_polarities(self):
        for true_lbl, false_lbl, expect_not in (("%hdr", "%exit", False), ("%exit", "%hdr", True)):
            with self.subTest(true_lbl=true_lbl, false_lbl=false_lbl):
                text = "\n".join([
                    "%void = OpTypeVoid",
                    "%bool = OpTypeBool",
                    "%cond = OpUndef %bool",
                    "%fn_t = OpTypeFunction %void",
                    "%main = OpFunction %void None %fn_t",
                    "%entry = OpLabel",
                    "OpBranch %hdr",
                    "%hdr = OpLabel",
                    "OpLoopMerge %exit %hdr None",
                    f"OpBranchConditional %cond {true_lbl} {false_lbl}",
                    "%exit = OpLabel",
                    "OpReturn",
                    "OpFunctionEnd",
                ]) + "\n"
                out = lg.add_guards(text, limit=4)
                if expect_not:
                    self.assertIn("OpLogicalNot", out)
                    self.assertIn("OpLogicalOr", out)
                else:
                    self.assertIn("OpLogicalAnd", out)
                    self.assertNotIn("OpLogicalNot", out)

    def test_conditional_back_edge_split_rewrites_latch_and_exit_phi(self):
        text = "\n".join([
            "%void = OpTypeVoid",
            "%bool = OpTypeBool",
            "%cond = OpUndef %bool",
            "%fn_t = OpTypeFunction %void",
            "%main = OpFunction %void None %fn_t",
            "%entry = OpLabel",
            "OpBranch %hdr",
            "%hdr = OpLabel",
            "%iv = OpPhi %bool %cond %entry %cond %hdr",
            "OpLoopMerge %exit %hdr None",
            "OpBranchConditional %cond %hdr %inner",
            "%inner = OpLabel",
            "OpBranch %exit",
            "%exit = OpLabel",
            "%done = OpPhi %bool %iv %inner",
            "OpReturn",
            "OpFunctionEnd",
        ]) + "\n"
        out = lg.add_guards(text, limit=5)
        self.assertIn("%loop_guard_0_latch = OpLabel", out)
        self.assertIn("%iv = OpPhi %bool %cond %entry %cond %loop_guard_0_latch", out)
        self.assertIn("OpBranchConditional %cond %loop_guard_0_latch %inner", out)
        self.assertIn("%done = OpPhi %bool %iv %inner %loop_guard_undef0 %loop_guard_0_latch", out)

    def test_crlf_input_keeps_opphi_on_one_line_without_interior_cr(self):
        out = lg.add_guards(_simple_loop(newline="\r\n", with_exit_phi=True), limit=8)
        for line in out.splitlines():
            self.assertNotIn("\r", line)
        opphi_lines = [line for line in out.splitlines() if "OpPhi" in line]
        self.assertEqual(len(opphi_lines), 3)
        self.assertTrue(any("%loop_guard_undef0 %latch" in line for line in opphi_lines))

    def test_malformed_opconstant_without_value_does_not_crash(self):
        text = "\n".join([
            "%void = OpTypeVoid",
            "%fn_t = OpTypeFunction %void",
            "%uint = OpTypeInt 32 0",
            "%bad = OpConstant %uint",
            "%main = OpFunction %void None %fn_t",
            "%entry = OpLabel",
            "OpBranch %hdr",
            "%hdr = OpLabel",
            "OpLoopMerge %exit %hdr None",
            "OpBranch %hdr",
            "%exit = OpLabel",
            "OpReturn",
            "OpFunctionEnd",
        ]) + "\n"
        out = lg.add_guards(text, limit=3)
        self.assertIn("%loop_guard_c3 = OpConstant %uint 3", out)

    def test_missing_opfunction_and_bad_cfg_raise_value_error(self):
        no_fn = "\n".join([
            "%entry = OpLabel",
            "OpBranch %hdr",
            "%hdr = OpLabel",
            "OpLoopMerge %exit %hdr None",
            "OpBranch %hdr",
            "%exit = OpLabel",
        ]) + "\n"
        with self.assertRaises(ValueError):
            lg.add_guards(no_fn, limit=4)

        switch_back = "\n".join([
            "%void = OpTypeVoid",
            "%uint = OpTypeInt 32 0",
            "%u0 = OpConstant %uint 0",
            "%fn_t = OpTypeFunction %void",
            "%main = OpFunction %void None %fn_t",
            "%entry = OpLabel",
            "OpBranch %hdr",
            "%hdr = OpLabel",
            "OpLoopMerge %exit %hdr None",
            "OpSwitch %u0 %exit 0 %hdr",
            "%exit = OpLabel",
            "OpReturn",
            "OpFunctionEnd",
        ]) + "\n"
        with self.assertRaises(ValueError):
            lg.add_guards(switch_back, limit=4)


if __name__ == "__main__":
    unittest.main()
