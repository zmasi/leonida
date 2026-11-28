"""Tests for RDNA2 v_div_fixup_f32 through the real translator (TEST-545).

The file this replaces built a hand-written Python model of the fixup and its own wave interpreter, and asserted
that model against itself: it never ran tools/rdna2spirv.py, so it passed whatever the translator did, and its
rules had drifted from RDNA2 silicon on six points (NaN priority, the sign of 0/0 and inf/inf, inf(S2), the
copysign of the pass-through, NaN(S0), and the exponent-difference cutoff). These tests run the translator on
hand-written listings instead:

  * the emitted SPIR-V of the unmodified form is pure integer, and its op order, op kinds and classification
    constants are the measured ones (fixtures/div_fixup_f32.asm);
  * the decision order is the measured one - NaN(S2) above NaN(S1) above 0/0 - asserted on the dataflow, not on
    the text, because the two NaN selects are textually identical apart from their operands;
  * every measured rule produces its measured word through spirv_mini_interp, with the expected word derived
    from the rule below, not from the translator.

Measured rules (docs/design.md "v_div_fixup_f32" row, native gfx1036, wave32 and wave64, MODE IEEE=0
denorm32=0 DX10_CLAMP=1), S0=v1 quotient, S1=v2 denominator, S2=v3 numerator, |X| is the bit pattern with the
sign cleared and sign_out = sign(S1) ^ sign(S2), highest priority first:

  1. NaN(S2) -> S2 unchanged.
  2. NaN(S1) -> S1 unchanged.
  3. zero(S1) and zero(S2), or inf(S1) and inf(S2) -> 0xFFC00000 (-qNaN, not +qNaN).
  4. zero(S2) or inf(S1) -> sign_out (+-0.0).
  5. zero(S1) or inf(S2) -> sign_out | 0x7F800000 (+-inf).
  6. exp(S2) - exp(S1) <= -151 -> sign_out (beats NaN(S0); biased exponents, no overflow check).
  7. NaN(S0) -> sign_out | 0x7F800000.
  8. otherwise copysign(S0, sign_out).

zero(X) means |X| < 0x00800000 (subnormals flush, denorm32=0), except for S0, whose subnormals are preserved.
S1 and S2 NaNs pass through unchanged (IEEE=0). Output modifiers apply to the result before clamp, and clamp
maps a NaN to +0.0 (DX10_CLAMP=1) before FClamp to [0, 1].

The f16 and f64 forms are not lowered yet; they arrive with #2477, whose own translator tests replace the f16
half of the model this file replaces. Standard library only; hand-written listings only.
"""
from __future__ import annotations

import gc
import os
import re
import struct
import sys
import tempfile
import unittest
import warnings

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TOOLS_DIR = os.environ.get("LEONIDA_TOOLS_DIR") or os.path.join(ROOT, "tools")

sys.dont_write_bytecode = True
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import rdna2spirv  # noqa: E402
import spirv_mini_interp as si  # noqa: E402

STUB_DXBC = b"DXBC" + bytes(24) + struct.pack("<I", 0) + bytes(4)
BASE = 0x1000
BUF_HEADER = [
    f"s_mov_b32 s0, {BASE:#x}",
    "s_mov_b32 s1, 0",
    "s_mov_b32 s2, 0x1000",
    "s_mov_b32 s3, 0x31016fac",
]
TAIL = ["exp mrt0 v4, v4, v4, v4 done vm", "s_endpgm"]
FIXUP = "v_div_fixup_f32 v4, v1, v2, v3"
OP_SEL = "OpSelect"

# Every op the fixup lowering emits, so the chain can be cut out of the module by op kind alone.
FIXUP_OPS = frozenset({
    "OpBitwiseXor", "OpBitwiseAnd", "OpBitwiseOr", "OpUGreaterThan", "OpUGreaterThanEqual", "OpIEqual",
    "OpULessThan", "OpShiftRightLogical", "OpISub", "OpLogicalAnd", "OpLogicalOr", OP_SEL,
})
INSN = re.compile(r"^\s*(%\w+) = (\w+) (.+)$")
CONST = re.compile(r"%c[0-9a-fA-F]+")

# The lowering of the unmodified form, in emission order: opcode, result type, then no operand identity but the
# integer constants (a `$` is any SSA id). All 32 instructions, so an added, dropped or reordered op fails.
EXPECTED_CHAIN = (
    # sign_out = sign(S1) ^ sign(S2); |S0|, |S1|, |S2|
    "OpBitwiseXor %uint $ $",
    "OpBitwiseAnd %uint $ #80000000",
    "OpBitwiseAnd %uint $ #7fffffff",
    "OpBitwiseAnd %uint $ #7fffffff",
    "OpBitwiseAnd %uint $ #7fffffff",
    # NaN(.) is |X| > 0x7F800000, inf(.) is |X| == 0x7F800000, zero(.) is |X| < 0x00800000
    "OpUGreaterThan %bool $ #7f800000",
    "OpUGreaterThan %bool $ #7f800000",
    "OpUGreaterThan %bool $ #7f800000",
    "OpIEqual %bool $ #7f800000",
    "OpIEqual %bool $ #7f800000",
    "OpULessThan %bool $ #800000",
    "OpULessThan %bool $ #800000",
    # exp(S2) - exp(S1) <= -151 (23 is the exponent shift, 151 is 0x97) with no underflow past e1 < e2
    "OpShiftRightLogical %uint $ #17",
    "OpShiftRightLogical %uint $ #17",
    "OpUGreaterThanEqual %bool $ $",
    "OpISub %uint $ $",
    "OpUGreaterThanEqual %bool $ #97",
    "OpLogicalAnd %bool $ $",
    # sign_out | 0x7F800000 and sign_out | |S0|
    "OpBitwiseOr %uint $ #7f800000",
    "OpBitwiseOr %uint $ $",
    # the branch conditions, then the chain that gives them their priority
    "OpLogicalAnd %bool $ $",
    "OpLogicalAnd %bool $ $",
    "OpLogicalOr %bool $ $",
    "OpLogicalOr %bool $ $",
    "OpLogicalOr %bool $ $",
    "OpSelect %uint $ $ $",
    "OpSelect %uint $ $ $",
    "OpSelect %uint $ $ $",
    "OpSelect %uint $ $ $",
    "OpSelect %uint $ #ffc00000 $",
    "OpSelect %uint $ $ $",
    "OpSelect %uint $ $ $",
)

CONSTANTS = frozenset({"#80000000", "#7fffffff", "#7f800000", "#800000", "#17", "#97", "#ffc00000"})

_TMP = None
_DISASM = None


def setUpModule():
    global _TMP, _DISASM
    _TMP = tempfile.TemporaryDirectory()
    _DISASM = os.path.join(_TMP.name, "disasm")
    os.mkdir(_DISASM)


def tearDownModule():
    global _TMP, _DISASM
    if _TMP is not None:
        gc.collect()
        _TMP.cleanup()
        _TMP = None
        _DISASM = None


def translate(lines, name="ps_div_fixup_f32"):
    """Translate one hand-written listing (the translator skips its first line, so it gets a comment header)."""
    if _TMP is None:
        setUpModule()
    with open(os.path.join(_TMP.name, name + ".dxbc"), "wb") as f:
        f.write(STUB_DXBC)
    path = os.path.join(_DISASM, name + ".s")
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(f"; {name}\n" + "".join(f"\t{ln}\n" for ln in lines))
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", ResourceWarning)
        spv = rdna2spirv.translate(path)
    gc.collect()
    return spv


def fixture_lines(name="div_fixup_f32"):
    with open(os.path.join(HERE, "fixtures", name + ".asm"), encoding="utf-8") as f:
        return [ln.strip() for ln in f.read().splitlines() if ln.strip() and not ln.lstrip().startswith(";")]


def instructions(spv):
    """(result id, opcode, operands) for every result-producing instruction in emission order."""
    out = []
    for line in spv.splitlines():
        match = INSN.match(line)
        if match:
            out.append((match.group(1), match.group(2), match.group(3).split()))
    return out


def normalise(opcode, operands):
    """Opcode, result type, then `#<constant>` or `$` for every other operand."""
    return opcode + " " + operands[0] + " " + " ".join(
        "#" + o[2:].lower() if CONST.fullmatch(o) else ("$" if o.startswith("%") else o) for o in operands[1:])


def fixup_chain(spv):
    """The lowering as emitted for one v_div_fixup_f32, plus the ids that read v2 (S1) and v3 (S2)."""
    insns = instructions(spv)
    chains = {}
    reads = {}
    start = None
    for i, (result, opcode, operands) in enumerate(insns):
        if opcode == "OpAccessChain" and operands[:2] == ["%_ptr_Function_uint", "%vgpr"]:
            reg = re.fullmatch(r"%c(\d+)", operands[2])
            if reg:
                chains[result] = int(reg.group(1))
        elif opcode == "OpLoad" and operands[0] == "%uint" and operands[1] in chains:
            reads[chains[operands[1]]] = result
        elif opcode == "OpBitwiseXor" and operands == ["%uint", reads.get(2), reads.get(3)]:
            start = i
            break
    if start is None:
        raise AssertionError("the translator emitted no sign(S1) ^ sign(S2) for v_div_fixup_f32")
    chain = []
    for result, opcode, operands in insns[start:]:
        if opcode not in FIXUP_OPS:
            break
        chain.append((result, opcode, operands))
    return chain, {2: reads[2], 3: reads[3]}


def run_rows(rows):
    """Run one program of (instruction, S0, S1, S2, expected) rows; return the word each row stored."""
    body = []
    for k, (_, insn, s0, s1, s2, _) in enumerate(rows):
        body += [
            f"v_mov_b32 v1, {s0:#x}",
            f"v_mov_b32 v2, {s1:#x}",
            f"v_mov_b32 v3, {s2:#x}",
            "v_mov_b32 v4, 0xdeadbeef",
            insn,
            f"buffer_store_dword v4, off, s[0:3], 0 offset:{4 * k}",
        ]
    memory = {}
    si.run_fragment(translate(BUF_HEADER + body + TAIL), memory=memory)
    return [memory.get(BASE + 4 * k) for k in range(len(rows))]


# One row per measured rule, and the rule number in the label. Expected words are derived from the rules in the
# module docstring (e.g. `1.0 / -inf` is `zero(S2) or inf(S1)` -> sign_out = 1 -> -0.0), never from the translator.
ROWS = (
    # 1. NaN(S2) wins over NaN(S1), 0/0, NaN(S0), and passes through unchanged (sNaN included, IEEE=0)
    ("1_nan_s2", FIXUP, 0x3F800000, 0x3F800000, 0x7FC00456, 0x7FC00456),
    ("1_nan_s2_over_nan_s1", FIXUP, 0x7FC00011, 0x7FC00022, 0xFF800123, 0xFF800123),
    ("1_nan_s2_over_zero_div_zero", FIXUP, 0x3F800000, 0x00000000, 0x7FC00999, 0x7FC00999),
    # 2. NaN(S1) wins over NaN(S0) and over inf(S2), and passes through unchanged (sNaN included)
    ("2_nan_s1", FIXUP, 0x3F800000, 0x7FC00234, 0x3F800000, 0x7FC00234),
    ("2_nan_s1_snan_over_nan_s0", FIXUP, 0x7FC00077, 0xFF800002, 0x7F800000, 0xFF800002),
    # 3. zero/zero and inf/inf -> canonical -qNaN, whatever the signs and even when the zeros are subnormal
    ("3_zero_div_zero", FIXUP, 0x3F800000, 0x00000000, 0x00000000, 0xFFC00000),
    ("3_neg_zero_div_neg_zero", FIXUP, 0x3F800000, 0x80000000, 0x80000000, 0xFFC00000),
    ("3_inf_div_inf", FIXUP, 0x3F800000, 0xFF800000, 0x7F800000, 0xFFC00000),
    ("3_subnormal_div_subnormal", FIXUP, 0x3F800000, 0x00000001, 0x807FFFFF, 0xFFC00000),
    # 4. zero(S2) or inf(S1) -> sign_out zero
    ("4_zero_s2", FIXUP, 0x3F800000, 0xC0000000, 0x00000000, 0x80000000),
    ("4_inf_s1", FIXUP, 0x3F800000, 0xFF800000, 0xC0000000, 0x00000000),
    ("4_subnormal_s2_flushes", FIXUP, 0x12345678, 0x40000000, 0x80000001, 0x80000000),
    # 5. zero(S1) or inf(S2) -> sign_out inf
    ("5_zero_s1", FIXUP, 0x3F800000, 0x00000000, 0xC0000000, 0xFF800000),
    ("5_subnormal_s1_flushes", FIXUP, 0x3F800000, 0x807FFFFF, 0xC0400000, 0x7F800000),
    ("5_inf_s2", FIXUP, 0x3F800000, 0xC0000000, 0x7F800000, 0xFF800000),
    # 6. exp(S2) - exp(S1) <= -151 -> sign_out, on the boundary, and ahead of NaN(S0)
    ("6_exp_151", FIXUP, 0x3F800000, 0x4C000000, 0x00800000, 0x00000000),
    ("6_exp_150_passes_through", FIXUP, 0x3F800000, 0x4B800000, 0x00800000, 0x3F800000),
    ("6_exp_beats_nan_s0", FIXUP, 0x7FC00055, 0x60000000, 0x00800000, 0x00000000),
    ("6_exp_negative_sign_out", FIXUP, 0xFFC00001, 0xE4000000, 0x00800000, 0x80000000),
    ("6_exp_s2_above_s1", FIXUP, 0xBF400000, 0x00800000, 0x4C000000, 0x3F400000),
    # 7. NaN(S0) -> sign_out inf
    ("7_nan_s0", FIXUP, 0xFFC00001, 0x40000000, 0x3F800000, 0x7F800000),
    ("7_snan_s0_negative_sign_out", FIXUP, 0xFFC00002, 0xC0000000, 0x3F800000, 0xFF800000),
    # 8. otherwise copysign(S0, sign_out), S0 subnormals and zeros included
    ("8_copysign_clears_s0_sign", FIXUP, 0xBF400000, 0x40000000, 0x40400000, 0x3F400000),
    ("8_copysign_sets_s0_sign", FIXUP, 0x3F400000, 0xC0000000, 0x40400000, 0xBF400000),
    ("8_s0_subnormal_preserved", FIXUP, 0x00000001, 0xBF800000, 0x3F800000, 0x80000001),
    ("8_s0_negative_zero_is_cleared", FIXUP, 0x80000000, 0x3F800000, 0x40400000, 0x00000000),
    # VOP3 source modifiers are bitwise before classification (S0, S1 and S2)
    ("mod_neg_s1", "v_div_fixup_f32 v4, v1, -v2, v3", 0x3F800000, 0x40000000, 0x3F800000, 0xBF800000),
    ("mod_abs_s1", "v_div_fixup_f32 v4, v1, |v2|, v3", 0x3F800000, 0xC0000000, 0x3F800000, 0x3F800000),
    ("mod_neg_abs_s2", "v_div_fixup_f32 v4, v1, v2, -|v3|", 0x3F800000, 0x3F800000, 0x40400000, 0xBF800000),
    ("mod_abs_s0", "v_div_fixup_f32 v4, |v1|, v2, v3", 0xBF800000, 0xBF800000, 0x3F800000, 0xBF800000),
    ("mod_neg_s1_snan_unchanged", "v_div_fixup_f32 v4, v1, -v2, v3", 0x3F800000, 0x7F800003, 0x3F800000, 0xFF800003),
    # Output modifiers apply before clamp; clamp maps a NaN result to +0.0 (DX10_CLAMP=1)
    ("omod_mul2", "v_div_fixup_f32 v4, v1, v2, v3 mul:2", 0x3F000000, 0x3F800000, 0x3F800000, 0x3F800000),
    ("omod_div2", "v_div_fixup_f32 v4, v1, v2, v3 div:2", 0x40000000, 0x3F800000, 0x3F800000, 0x3F800000),
    ("omod_mul4", "v_div_fixup_f32 v4, v1, v2, v3 mul:4", 0x3F000000, 0x3F800000, 0x3F800000, 0x40000000),
    ("clamp_nan_to_zero", "v_div_fixup_f32 v4, v1, v2, v3 clamp", 0x3F800000, 0x00000000, 0x00000000, 0x00000000),
    ("clamp_negative", "v_div_fixup_f32 v4, v1, v2, v3 clamp", 0x3F000000, 0xBF800000, 0x3F800000, 0x00000000),
    ("clamp_above_one", "v_div_fixup_f32 v4, v1, v2, v3 clamp", 0x40000000, 0x3F800000, 0x3F800000, 0x3F800000),
    ("omod_mul2_clamp", "v_div_fixup_f32 v4, v1, v2, v3 mul:2 clamp", 0x3F400000, 0x3F800000, 0x3F800000, 0x3F800000),
    ("omod_div2_clamp", "v_div_fixup_f32 v4, v1, v2, v3 div:2 clamp", 0x3FC00000, 0x3F800000, 0x3F800000, 0x3F400000),
)


class MeasuredRuleWords(unittest.TestCase):
    def test_every_measured_rule_word(self):
        words = run_rows(ROWS)
        for (label, insn, s0, s1, s2, want), got in zip(ROWS, words):
            with self.subTest(rule=label, insn=insn, s0=hex(s0), s1=hex(s1), s2=hex(s2)):
                self.assertEqual(got, want)


class LoweringStructure(unittest.TestCase):
    def test_the_unmodified_form_is_pure_integer_with_the_measured_constants(self):
        spv = translate(fixture_lines())
        for op in ("OpFMul", "OpFAdd", "OpFSub", "OpFDiv", "OpFNegate", "OpIsNan", "OpIsInf", "FClamp",
                   "OpCapability Float64"):
            self.assertNotIn(op, spv, f"{op} in a v_div_fixup_f32 with no output modifier")
        chain, _ = fixup_chain(spv)
        self.assertEqual([normalise(opcode, operands) for _, opcode, operands in chain], list(EXPECTED_CHAIN))

    def test_classification_constants_are_exactly_the_measured_rule_constants(self):
        chain, _ = fixup_chain(translate(fixture_lines()))
        used = {"#" + o[2:].lower() for _, _, operands in chain for o in operands if CONST.fullmatch(o)}
        self.assertEqual(used, CONSTANTS)
        # 0/0 and inf/inf give -qNaN; a +qNaN anywhere would be the wrong rule.
        self.assertNotIn("#7fc00000", used)

    def test_nan_priority_is_s2_then_s1_on_the_dataflow(self):
        chain, reads = fixup_chain(translate(fixture_lines()))
        abs_of = {}                                # id of a NaN test -> the id whose magnitude it tested
        for result, opcode, operands in chain:
            if opcode == "OpUGreaterThan" and operands[0] == "%bool" and operands[2] == "%c7f800000":
                abs_of[result] = operands[1]
        magnitude = {}                             # |X| id -> the id X was taken from
        for result, opcode, operands in chain:
            if opcode == "OpBitwiseAnd" and operands[0] == "%uint" and operands[2] == "%c7fffffff":
                magnitude[result] = operands[1]
        selected = {}                              # id returned verbatim -> the selects returning it
        for result, opcode, operands in chain:
            if opcode == OP_SEL and operands[0] == "%uint":
                selected.setdefault(operands[2], []).append((result, operands))
        s1_raw, s2_raw = reads[2], reads[3]
        nan_s2 = selected[s2_raw]
        nan_s1 = selected[s1_raw]
        self.assertEqual(len(nan_s2), 1, "S2 is returned by exactly one select")
        self.assertEqual(len(nan_s1), 1, "S1 is returned by exactly one select")
        s2_result, s2_operands = nan_s2[0]
        _, s1_operands = nan_s1[0]
        for name, operands, raw in (("S1", s1_operands, s1_raw), ("S2", s2_operands, s2_raw)):
            self.assertIn(operands[1], abs_of, f"the {name} select is not conditioned on a NaN test")
            self.assertEqual(magnitude.get(abs_of[operands[1]]), raw,
                             f"the {name} select is not conditioned on |{name}| > 0x7F800000")
        self.assertEqual(s2_operands[3], nan_s1[0][0], "NaN(S1) must be applied below NaN(S2)")
        consumed = {operands[3] for _, opcode, operands in chain if opcode == OP_SEL}
        self.assertNotIn(s2_result, consumed, "the NaN(S2) select must be the last one")

    def test_clamp_replaces_a_nan_with_positive_zero_before_fclamp(self):
        spv = translate([
            "v_mov_b32 v1, 0x3f800000",
            "v_mov_b32 v2, 0",
            "v_mov_b32 v3, 0",
            FIXUP + " clamp",
        ] + TAIL)
        insns = instructions(spv)
        is_nan = [result for result, opcode, _ in insns if opcode == "OpIsNan"]
        self.assertEqual(len(is_nan), 1)
        guard = [(result, operands) for result, opcode, operands in insns
                 if opcode == OP_SEL and operands[0] == "%float" and operands[1] == is_nan[0]]
        self.assertEqual(len(guard), 1, "DX10_CLAMP=1: a NaN result must be replaced by +0.0")
        safe = guard[0][0]
        self.assertEqual(guard[0][1][2], "%f0")
        clamps = [operands for _, opcode, operands in insns if opcode == "OpExtInst" and "FClamp" in operands]
        self.assertEqual(len(clamps), 1)
        self.assertEqual([clamps[0][2], clamps[0][3], clamps[0][4]], ["FClamp", safe, "%f0"])
        self.assertLess([i for i, (_, opcode, _) in enumerate(insns) if opcode == "OpIsNan"][0],
                        [i for i, (_, opcode, _) in enumerate(insns) if opcode == "OpExtInst"][0])

    def test_output_modifier_scales_before_the_clamp(self):
        spv = translate([
            "v_mov_b32 v1, 0x3f400000",
            "v_mov_b32 v2, 0x3f800000",
            "v_mov_b32 v3, 0x3f800000",
            FIXUP + " mul:2 clamp",
        ] + TAIL)
        insns = instructions(spv)
        multiplies = [i for i, (_, opcode, _) in enumerate(insns) if opcode == "OpFMul"]
        nan_guards = [i for i, (_, opcode, _) in enumerate(insns) if opcode == "OpIsNan"]
        self.assertEqual(len(multiplies), 1)
        self.assertEqual(len(nan_guards), 1)
        self.assertLess(multiplies[0], nan_guards[0], "mul:2 applies before clamp")


class WavesAndDestinations(unittest.TestCase):
    def test_inactive_lane_keeps_the_destination(self):
        spv = translate(BUF_HEADER + [
            "v_mov_b32 v1, 0x3f800000",
            "v_mov_b32 v2, 0",
            "v_mov_b32 v3, 0",
            "v_mov_b32 v4, 0xdeadbeef",
            "s_mov_b32 exec_lo, 0",
            FIXUP,
            "s_mov_b32 exec_lo, 1",
            "buffer_store_dword v4, off, s[0:3], 0 offset:0",
        ] + TAIL)
        memory = {}
        si.run_fragment(spv, memory=memory)
        self.assertEqual(memory.get(BASE), 0xDEADBEEF)

    def test_null_destination_writes_nothing(self):
        memory = {}
        si.run_fragment(translate(BUF_HEADER + [
            "v_mov_b32 v1, 0x3f800000",
            "v_mov_b32 v2, 0",
            "v_mov_b32 v3, 0",
            "v_mov_b32 v4, 0x12345678",
            "v_div_fixup_f32 null, v1, v2, v3",
            "buffer_store_dword v4, off, s[0:3], 0 offset:0",
        ] + TAIL), memory=memory)
        self.assertEqual(memory.get(BASE), 0x12345678)


if __name__ == "__main__":
    unittest.main()
