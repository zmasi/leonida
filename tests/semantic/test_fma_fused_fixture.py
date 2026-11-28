"""runtime/test/shaders/cs_fma_fused.asm: its eight words, and no multiply-add a compiler can fold
(TEST-FMA-FUSED-F16-RUNTIME).

The fixture takes the operands of its f32 multiply-adds from s4..s7, so that a driver has to compute them at run
time. The three operands of its v_fma_f16 were literals. That made the whole f16 multiply-add a compile-time
constant, and MoltenVK 1.4.2 gave the wrong one: word 7 read 0x0000 on an Apple M5 (0x3c00 in an earlier report)
instead of 0x7351, the cs_fma_fused row of docs/testing.md, while the same translation with any one of the three
operands taken at run time gave 0x7351. The triple is now s4 XOR a literal each.

Checked here, without a GPU: the translated fixture gives the eight words of its header in the interpreter (so the
new operands are the triple the header derives 0x7351 from), and every register a multiply-add reads comes from
s4..s7, so a literal operand cannot come back unnoticed. What a driver does with the module is the GPU test's
business (runtime/test/shaders/check.py, scripts/run_gpu_tests.py).

Standard library only; no GPU, no LLVM, no SPIRV-Tools, no game files.
"""
import os
import re
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import support  # noqa: E402  (puts tools/ on the path)
import spirv_mini_interp as si  # noqa: E402

FIXTURE = os.path.join(HERE, "..", "..", "runtime", "test", "shaders", "cs_fma_fused.asm")
VSHARP = [0, 0, 0x1000, 0x31016FAC]             # s[0:3], the output buffer
REG = re.compile(r"(?<![\w.])([vs])(\d+)\b")


def read_fixture():
    """(instruction lines, {header field: text}) of the fixture."""
    with open(FIXTURE, encoding="utf-8") as f:
        text = f.read()
    header = dict(re.findall(r"^; (group|rsrc2|Run|out\[0\.\.7\]):? =? ?(.+)$", text, re.M))
    lines = [ln.strip() for ln in text.splitlines() if ln.strip() and not ln.lstrip().startswith(";")]
    return lines, header


def run_time_registers(lines):
    """{line index: the VGPR sources of a multiply-add that are not run-time values}, by a pass over the listing: a
    VGPR is a run-time value when the instruction that wrote it read s4..s7 or another run-time VGPR."""
    run_time, literal_only = set(), {}
    for i, line in enumerate(lines):
        op, _, rest = line.partition(" ")
        regs = REG.findall(rest)
        if not op.startswith("v_") or not regs or regs[0][0] != "v":
            continue                                    # stores and the like write no VGPR
        dst, sources = int(regs[0][1]), regs[1:]
        if op.startswith(("v_fma", "v_fmac")):
            if op.startswith("v_fmac"):
                sources = regs                          # d += a * b reads its destination as well
            literal_only[i] = [f"v{n}" for kind, n in sources if kind == "v" and int(n) not in run_time]
        fed = any(kind == "s" and 4 <= int(n) <= 7 or kind == "v" and int(n) in run_time for kind, n in sources)
        (run_time.add if fed else run_time.discard)(dst)
    return literal_only


class FmaFusedFixture(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lines, cls.header = read_fixture()

    def test_the_eight_words_of_the_header(self):
        operands = [int(w, 16) for w in re.search(r"VKC_SGPRS=([0-9a-fx,]+)", self.header["Run"]).group(1).split(",")]
        want = [int(w, 16) for w in self.header["out[0..7]"].split("(")[0].split()]
        self.assertEqual((len(operands), len(want), want[7]), (4, 8, 0x7351))
        group = [int(v) for v in self.header["group"].split(",")]
        fssm = bytearray(0x148)
        struct.pack_into("<4I", fssm, 0x138, int(self.header["rsrc2"], 0), *group)
        spv = support.translate_lines("cs_fma_fused", self.lines, fssm=bytes(fssm))
        memory = si.run_workgroup(
            spv, 1, 32, inputs_for=lambda i: {"%wg_id": (0, 0, 0), "%local_id": (i, 0, 0), "%sg_lane": i},
            push_constants=VSHARP + operands + [0] * 24, memory={})
        self.assertEqual([memory.get(4 * k) for k in range(8)], want)

    def test_every_multiply_add_reads_run_time_registers_only(self):
        literal_only = run_time_registers(self.lines)
        self.assertEqual(len(literal_only), 8)          # seven f32 forms and the f16 one
        for i, regs in literal_only.items():
            self.assertEqual(regs, [], f"`{self.lines[i]}` reads {regs}, which only literals fed: a compiler can "
                                       "fold it, and MoltenVK 1.4.2 folds the f16 form to the wrong word")

    def test_the_pass_sees_a_literal_operand(self):
        # The f16 lines as they were: three literal moves in front of the multiply-add.
        old = ["v_mov_b32_e32 v0, s4", "v_mov_b32_e32 v15, 0x6ce1", "v_mov_b32_e32 v16, 0x4200",
               "v_mov_b32_e32 v17, 0x8004", "v_fma_f16 v18, v15, v16, v17", "v_fma_f32 v4, v0, v0, -1.0",
               "v_mov_b32_e32 v0, 0", "v_fmac_f32_e32 v4, v0, v0"]
        self.assertEqual(run_time_registers(old), {4: ["v15", "v16", "v17"], 5: [], 7: ["v0", "v0"]})


if __name__ == "__main__":
    unittest.main()
