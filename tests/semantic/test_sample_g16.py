"""image_sample_d_g16 and image_sample_d_cl_g16: explicit-gradient samples with 16-bit gradients (SHADER-SAMPLE-G16).

Standard library only (spirv-val is used when it is on PATH); no GPU, no LLVM, no game files.

Every expected position is written by hand from the GFX10 G16 layout that two independent compilers emit:
* LLVM `SIISelLowering.cpp`: `lowerImage` decides IsG16 from the gradient type and IsA16 from the coordinate type,
  independently on a G16 target. `packImage16bitOpsToDwords` packs dP/dx and dP/dy two halves per VGPR, low half
  first; its comment gives 1D as (dx/dh, -) (dx/dv, -), 2D as (dx/dh, dy/dh) (dx/dv, dy/dv), and 3D as
  (dx/dh, dy/dh) (dz/dh, -) (dx/dv, dy/dv) (dz/dv, -).
* Mesa ACO `aco_select_nir.cpp`: a16 and g16 come from the bit sizes of the coordinate and of ddx. ddx and ddy are
  packed by one `emit_pack_v1` call each (`aco_isel_helpers.cpp`: halves paired low first, an odd one padded), and
  the `_g16` opcode is used on GFX10+.
So the gradients take ceil(n/2) VGPRs per axis, and the coordinates (and the clamp) follow, packed only under a16.
The tests follow each SPIR-V sample operand back to the VGPR and the half it is read from. Not run on a GPU.
"""
import os
import re
import shutil
import struct
import subprocess
import tempfile
import unittest

import support
import rdna2spirv

DESC = "s[8:15], s[16:19]"
CS_FSSM = bytearray(0x148)
struct.pack_into("<4I", CS_FSSM, 0x138, 0x8088, 32, 1, 1)
CS_FSSM = bytes(CS_FSSM)
V = 30              # the address VGPRs start at v30


def translate(name, line):
    fssm = CS_FSSM if name.startswith("cs_") else None
    body = [line, "exp mrt0 v20, v21, v22, v23 done vm", "s_endpgm"] if name.startswith("ps_") else [line, "s_endpgm"]
    return support.translate_lines(name, body, fssm=fssm)


def the_sample(spv):
    (line,) = [ln.strip() for ln in spv.splitlines() if "= OpImageSample" in ln]
    return line


def defs(spv):
    return {m.group(1): (m.group(2), m.group(3).split()) for m in re.finditer(r"^\s*(%\w+) = (\w+) (.*)$", spv, re.M)}


def vgprs_behind(spv, value):
    """VGPRs a value is built from, in order: k for a whole VGPR, (k, half) for a half widened by UnpackHalf2x16."""
    d = defs(spv)
    c = {k: int(v) for k, v in re.findall(r"^\s*(%\w+) = OpConstant %uint (\d+)", spv, re.M)}

    def walk(v):
        op, args = d[v]
        if op == "OpLoad":
            chain_op, chain = d[args[1]]
            assert chain_op == "OpAccessChain" and chain[1] == "%vgpr", (chain_op, chain)
            return [c[chain[2]]]
        if op == "OpBitcast":
            return walk(args[1])
        if op == "OpCompositeConstruct":
            return [x for a in args[1:] for x in walk(a)]
        if op == "OpCompositeExtract":
            inner = d[args[1]]
            if inner[0] == "OpExtInst" and inner[1][2] == "UnpackHalf2x16":
                (k,) = walk(inner[1][3])
                return [(k, int(args[2]))]
            return [walk(args[1])[int(args[2])]]
        raise AssertionError(f"{v}: {op} {args}")
    return walk(value)


def operands(line):
    """(coordinate, {Grad: [dx, dy], MinLod: [clamp]}) of an OpImageSampleExplicitLod line."""
    toks = line.split()             # %r = OpImageSampleExplicitLod %type %sampled %coord Masks values...
    coord, masks, vals = toks[5], toks[6].split("|"), toks[7:]
    out = {}
    for m in masks:
        n = 2 if m == "Grad" else 1
        out[m], vals = vals[:n], vals[n:]
    assert not vals, line
    return coord, out


def regs(n):
    return f"v[{V}:{V + n - 1}]"


H = lambda k, half: (V + k, half)    # noqa: E731  a half of v(30 + k)
W = lambda k: V + k                  # noqa: E731  the whole v(30 + k)

# (dim, address words, a16, clamp?) -> expected (dx, dy, coordinate, clamp) written from the layout above.
CASES = {
    ("1D", False, False): (3, [H(0, 0)], [H(1, 0)], [W(2)], None),
    ("1D", False, True): (4, [H(0, 0)], [H(1, 0)], [W(2)], W(3)),
    ("2D", False, False): (4, [H(0, 0), H(0, 1)], [H(1, 0), H(1, 1)], [W(2), W(3)], None),
    ("2D", False, True): (5, [H(0, 0), H(0, 1)], [H(1, 0), H(1, 1)], [W(2), W(3)], W(4)),
    ("2D_ARRAY", False, True): (6, [H(0, 0), H(0, 1)], [H(1, 0), H(1, 1)], [W(2), W(3), W(4)], W(5)),
    ("3D", False, False): (7, [H(0, 0), H(0, 1), H(1, 0)], [H(2, 0), H(2, 1), H(3, 0)], [W(4), W(5), W(6)], None),
    ("3D", False, True): (8, [H(0, 0), H(0, 1), H(1, 0)], [H(2, 0), H(2, 1), H(3, 0)], [W(4), W(5), W(6)], W(7)),
    # a16 and g16 together: the coordinates and the clamp are packed halves too.
    ("1D", True, True): (3, [H(0, 0)], [H(1, 0)], [H(2, 0)], H(2, 1)),
    ("2D", True, False): (3, [H(0, 0), H(0, 1)], [H(1, 0), H(1, 1)], [H(2, 0), H(2, 1)], None),
    ("2D", True, True): (4, [H(0, 0), H(0, 1)], [H(1, 0), H(1, 1)], [H(2, 0), H(2, 1)], H(3, 0)),
    ("3D", True, True): (6, [H(0, 0), H(0, 1), H(1, 0)], [H(2, 0), H(2, 1), H(3, 0)], [H(4, 0), H(4, 1), H(5, 0)],
                         H(5, 1)),
}


class G16Layout(unittest.TestCase):
    def check(self, stage, dim, a16, clamp):
        n, dx, dy, uv, cl = CASES[(dim, a16, clamp)]
        op = "image_sample_d_cl_g16" if clamp else "image_sample_d_g16"
        spv = translate(stage, f"{op} v[20:23], {regs(n)}, {DESC} dmask:0xf dim:SQ_RSRC_IMG_{dim}"
                               f"{' a16' if a16 else ''}")
        coord, ops = operands(the_sample(spv))
        self.assertEqual(vgprs_behind(spv, ops["Grad"][0]), dx)    # 1D: a scalar gradient, one half
        self.assertEqual(vgprs_behind(spv, ops["Grad"][1]), dy)
        self.assertEqual(vgprs_behind(spv, coord), uv)
        if cl is None:
            self.assertNotIn("MinLod", ops)
        else:
            self.assertEqual(vgprs_behind(spv, ops["MinLod"][0]), [cl])

    def test_every_dim_and_a16_combination(self):
        for (dim, a16, clamp) in CASES:
            for stage in ("ps_g16", "cs_g16"):
                with self.subTest(dim=dim, a16=a16, clamp=clamp, stage=stage):
                    self.check(stage, dim, a16, clamp)

    def test_g16_and_the_f32_form_differ_only_in_the_gradients(self):
        # The same 2D sample with f32 gradients reads v30..v33 whole as gradients; the g16 form reads v30, v31 as
        # halves and its coordinates move up by two VGPRs.
        spv = translate("ps_d", f"image_sample_d v[20:23], {regs(6)}, {DESC} dmask:0xf dim:SQ_RSRC_IMG_2D")
        coord, ops = operands(the_sample(spv))
        self.assertEqual(vgprs_behind(spv, ops["Grad"][0]), [W(0), W(1)])
        self.assertEqual(vgprs_behind(spv, coord), [W(4), W(5)])

    def test_cube(self):
        # A cube samples its 2D-array view as the f32-gradient form does: two-component gradients.
        spv = translate("ps_cube", f"image_sample_d_g16 v[20:23], {regs(5)}, {DESC} dmask:0xf dim:SQ_RSRC_IMG_CUBE")
        _coord, ops = operands(the_sample(spv))
        self.assertEqual(vgprs_behind(spv, ops["Grad"][0]), [H(0, 0), H(0, 1)])
        self.assertEqual(vgprs_behind(spv, ops["Grad"][1]), [H(1, 0), H(1, 1)])

    def test_dmask(self):
        spv = translate("ps_dm", f"image_sample_d_g16 v[20:21], {regs(4)}, {DESC} dmask:0x9 dim:SQ_RSRC_IMG_2D")
        res = the_sample(spv).split()[0]
        self.assertEqual([int(k) for k in re.findall(rf"OpCompositeExtract %float {res} (\d)", spv)], [0, 3])


class Refusals(unittest.TestCase):
    def test_other_g16_forms(self):
        lines = [
            # The f32-gradient compare/offset forms are a separate change (SHADER-SAMPLE-GRAD-FORMS).
            f"image_sample_c_d_g16 v20, {regs(5)}, {DESC} dmask:0x1 dim:SQ_RSRC_IMG_2D",
            f"image_sample_d_o_g16 v[20:23], {regs(5)}, {DESC} dmask:0xf dim:SQ_RSRC_IMG_2D",
            # Coarse derivatives: no exact Vulkan form.
            f"image_sample_cd_g16 v[20:23], {regs(4)}, {DESC} dmask:0xf dim:SQ_RSRC_IMG_2D",
            f"image_sample_c_cd_cl_o_g16 v20, {regs(7)}, {DESC} dmask:0x1 dim:SQ_RSRC_IMG_2D",
        ]
        for line in lines:
            with self.subTest(line=line), self.assertRaises(rdna2spirv.Unsupported):
                translate("ps_no", line)

    def test_too_few_address_words(self):
        with self.assertRaises(rdna2spirv.Unsupported):
            translate("ps_short", f"image_sample_d_g16 v[20:23], v{V}, {DESC} dmask:0xf dim:SQ_RSRC_IMG_2D")


class Validation(unittest.TestCase):
    @unittest.skipUnless(shutil.which("spirv-as") and shutil.which("spirv-val"), "needs spirv-as and spirv-val on PATH")
    def test_modules_validate(self):
        for (dim, a16, clamp), (n, *_rest) in CASES.items():
            op = "image_sample_d_cl_g16" if clamp else "image_sample_d_g16"
            line = f"{op} v[20:23], {regs(n)}, {DESC} dmask:0xf dim:SQ_RSRC_IMG_{dim}{' a16' if a16 else ''}"
            for stage in ("ps_val", "cs_val"):
                with self.subTest(line=line, stage=stage):
                    spv = translate(stage, line)
                    with tempfile.TemporaryDirectory() as tmp:
                        a, b = os.path.join(tmp, "m.spvasm"), os.path.join(tmp, "m.spv")
                        with open(a, "w", encoding="utf-8", newline="\n") as f:
                            f.write(spv)
                        subprocess.run(["spirv-as", "--target-env", "vulkan1.2", a, "-o", b], check=True)
                        subprocess.run(["spirv-val", "--target-env", "vulkan1.2", b], check=True)


if __name__ == "__main__":
    unittest.main()
