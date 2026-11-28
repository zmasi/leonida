"""image_gather4_c_lz and image_gather4_c_lz_o (SHADER-GATHER4-C-LZ): the depth-compare gather at the base level.

Each form is translated to one OpImageDrefGather. The tests trace every operand of that instruction back to the VGPR (and
half) it was read from, and compare it with the position written out by hand below, which comes from two places:

* LLVM AMDGPU (`AMDGPUSampleVariant` in IntrinsicsAMDGPU.td: the address is "{offset} {bias} {z-compare}", then the
  coordinates s, t and for an array or cube the slice, then a LOD or clamp that _lz does not have; gather4 exists for 2D,
  Cube and 2DArray), and
* the dword counts `llvm-mc -mcpu=gfx1030` (LLVM 18) accepts, which are what the register ranges below are:
  2D 3 dwords (z, s, t), array and cube 4 (z, s, t, slice), the offset form one more (the offset dword first),
  under a16 the coordinates pack in halves (2D: z, st; array: z, st, slice) and the z-compare keeps a whole dword.

Mesa's ACO `visit_tex` agrees: a shadow tg4 at level zero is image_gather4_c_lz / _c_lz_o, the address is pushed as
offset, compare, coordinates (the compare stays a whole dword under a16), and dmask is 1 for every shadow gather
(docs/design.md). Nothing here ran on a GPU.
Standard library only (spirv-as / spirv-val are used when they are on PATH); no game files.
"""
import os
import re
import shutil
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import support
import rdna2spirv

DESC = "s[8:15], s[16:19]"
CS_FSSM = bytearray(0x148)
struct.pack_into("<4I", CS_FSSM, 0x138, 0x8088, 32, 1, 1)
CS_FSSM = bytes(CS_FSSM)

DIMS = {"2D": "dim:SQ_RSRC_IMG_2D", "ARR": "dim:SQ_RSRC_IMG_2D_ARRAY", "CUBE": "dim:SQ_RSRC_IMG_CUBE"}


def translate(op, regs, dim="dim:SQ_RSRC_IMG_2D", extra="", stage="ps", data="v[8:11]", dmask="dmask:0x1"):
    line = f"{op} {data}, {regs}, {DESC} {dmask} {dim}{' ' + extra if extra else ''}"
    body = [f"v_mov_b32 v{k}, 0" for k in range(6)] + [line]
    if stage == "ps":
        return support.translate_lines("ps_g4clz", body + ["exp mrt0 v8, v9, v10, v11 done vm", "s_endpgm"])
    if stage == "cs":
        return support.translate_lines("cs_g4clz", body + ["s_endpgm"], fssm=CS_FSSM)
    return support.translate_lines("vs_g4clz", body + ["exp pos0 v8, v9, v10, v11 done", "s_endpgm"])


class Spirv:
    """Definitions of one translated module, and the VGPR (and half) an id was read from."""

    def __init__(self, text):
        self.text = text
        self.defs = dict(re.findall(r"^\s*(%\w+) = (.+)$", text, re.M))

    def gathers(self):
        return re.findall(r"^\s*(%\w+) = OpImageDrefGather %v4float (%\w+) (%\w+) (%\w+)( Offset (%\w+))?$", self.text, re.M)

    def origin(self, value):
        """('v', k) for a whole VGPR, ('half', k, lane) for a lane of UnpackHalf2x16 of VGPR k."""
        for _ in range(8):
            e = self.defs[value].split()
            op = e[0]
            if op in ("OpBitcast", "OpCopyObject"):
                value = e[2]
            elif op == "OpLoad":
                m = re.fullmatch(r"%reg_v(\d+)", e[2])
                return ("v", int(m.group(1)))
            elif op == "OpCompositeExtract":
                inner = self.defs[e[2]].split()
                if inner[0] == "OpExtInst" and inner[3] == "UnpackHalf2x16":
                    return ("half", self.origin(inner[4])[1], int(e[3]))
                raise AssertionError(f"unexpected extract source {self.defs[e[2]]}")
            else:
                raise AssertionError(f"cannot trace {value} = {self.defs[value]}")
        raise AssertionError("trace too long")

    def components(self, vector):
        e = self.defs[vector].split()
        assert e[0] == "OpCompositeConstruct", self.defs[vector]
        return [self.origin(c) for c in e[2:]]

    def offset_fields(self, offset):
        """The (register, bit offset, width) of the two fields an Offset operand is built from."""
        e = self.defs[offset].split()
        assert e[0] == "OpCompositeConstruct" and e[1] == "%v2int", self.defs[offset]
        fields = []
        for part in e[2:]:
            cast = self.defs[part].split()
            assert cast[0] == "OpBitcast", self.defs[part]
            ext = self.defs[cast[2]].split()
            assert ext[:2] == ["OpBitFieldSExtract", "%uint"], self.defs[cast[2]]
            consts = {k: int(v) for k, v in re.findall(r"^\s*(%\w+) = OpConstant %uint (\d+)", self.text, re.M)}
            fields.append((self.origin(ext[2]), consts[ext[3]], consts[ext[4]]))
        return fields

    def stores(self):
        """{vgpr: id} of the OpStore %reg_vN <id> lines."""
        return {int(k): v for k, v in re.findall(r"^\s*OpStore %reg_v(\d+) (%\w+)$", self.text, re.M)}


def one_gather(spv):
    found = spv.gathers()
    assert len(found) == 1, found
    return found[0]


class OperandOrder(unittest.TestCase):
    """Positions from LLVM's {offset} {z-compare} then coordinates, and the dword counts llvm-mc accepts."""

    def check(self, op, regs, dim, want_ref, want_uv, extra="", offset_reg=None):
        spv = Spirv(translate(op, regs, dim, extra))
        _, _sampled, uv, ref, _, off = one_gather(spv)
        self.assertEqual(spv.origin(ref), want_ref)
        self.assertEqual(spv.components(uv), want_uv)
        if offset_reg is None:
            self.assertEqual(off, "")
        else:
            # signed 6-bit x [5:0] and y [13:8] of the first address dword
            self.assertEqual(spv.offset_fields(off), [(("v", offset_reg), 0, 6), (("v", offset_reg), 8, 6)])
        return spv

    def test_2d_is_z_compare_then_s_t(self):
        self.check("image_gather4_c_lz", "v[0:2]", DIMS["2D"], ("v", 0), [("v", 1), ("v", 2)])

    def test_2d_array_adds_the_slice_after_t(self):
        self.check("image_gather4_c_lz", "v[0:3]", DIMS["ARR"], ("v", 0), [("v", 1), ("v", 2), ("v", 3)])

    def test_offset_form_has_the_offset_dword_first(self):
        self.check("image_gather4_c_lz_o", "v[0:3]", DIMS["2D"], ("v", 1), [("v", 2), ("v", 3)], offset_reg=0)
        self.check("image_gather4_c_lz_o", "v[0:4]", DIMS["ARR"], ("v", 1), [("v", 2), ("v", 3), ("v", 4)], offset_reg=0)

    def test_a16_packs_coordinates_but_not_the_z_compare(self):
        a16 = "a16"
        self.check("image_gather4_c_lz", "v[0:1]", DIMS["2D"], ("v", 0), [("half", 1, 0), ("half", 1, 1)], a16)
        self.check("image_gather4_c_lz", "v[0:2]", DIMS["ARR"], ("v", 0),
                   [("half", 1, 0), ("half", 1, 1), ("half", 2, 0)], a16)
        self.check("image_gather4_c_lz_o", "v[0:2]", DIMS["2D"], ("v", 1), [("half", 2, 0), ("half", 2, 1)], a16,
                   offset_reg=0)
        self.check("image_gather4_c_lz_o", "v[0:3]", DIMS["ARR"], ("v", 1),
                   [("half", 2, 0), ("half", 2, 1), ("half", 3, 0)], a16, offset_reg=0)

    def test_cube_reads_the_compare_first_and_gathers_the_six_layer_view(self):
        spv = Spirv(translate("image_gather4_c_lz", "v[0:3]", DIMS["CUBE"]))
        _, sampled, uv, ref, _, _ = one_gather(spv)
        self.assertEqual(spv.origin(ref), ("v", 0))
        self.assertIn("%simg2da", spv.defs[sampled])           # the cube is bound as a 2D-array view (cube_coords)
        self.assertEqual(spv.defs[uv].split()[1], "%v3float")


class ResultsAndStages(unittest.TestCase):
    def test_the_four_comparisons_land_in_gather_order(self):
        spv = Spirv(translate("image_gather4_c_lz", "v[0:2]"))
        gather, *_ = one_gather(spv)
        stores = spv.stores()
        for k in range(4):
            # v8..v11 take components 0..3 of the gather result, unchanged and in that order
            comp = spv.defs[spv.defs[stores[8 + k]].split()[2]].split()
            self.assertEqual(comp[:2], ["OpCompositeExtract", "%float"])
            self.assertEqual(comp[2:], [gather, str(k)])

    def test_d16_packs_two_comparisons_per_register(self):
        spv = Spirv(translate("image_gather4_c_lz", "v[0:2]", extra="d16", data="v[8:9]"))
        stores = spv.stores()
        self.assertEqual(sorted(k for k in stores if 8 <= k <= 11), [8, 9])       # two registers, not four
        for k in (8, 9):
            # integer packing (low half | high half << 16), not a plain f32 bitcast of one comparison
            self.assertEqual(spv.defs[stores[k]].split()[0], "OpBitwiseOr")

    def test_it_is_a_dref_gather_at_the_base_level_with_no_query_or_fetch(self):
        text = translate("image_gather4_c_lz", "v[0:2]")
        self.assertIn("OpImageDrefGather", text)
        for forbidden in ("OpImageGather ", "OpImageFetch", "OpImageQuery", "OpImageSampleDref"):
            self.assertNotIn(forbidden, text)

    def test_offset_form_needs_the_gather_extended_capability(self):
        self.assertIn("OpCapability ImageGatherExtended", translate("image_gather4_c_lz_o", "v[0:3]"))
        self.assertNotIn("OpCapability ImageGatherExtended", translate("image_gather4_c_lz", "v[0:2]"))

    def test_every_stage_translates(self):
        # a gather has no implicit LOD, so there is no pixel-stage restriction
        for stage in ("ps", "vs", "cs"):
            for op, regs in (("image_gather4_c_lz", "v[0:2]"), ("image_gather4_c_lz_o", "v[0:3]")):
                with self.subTest(stage=stage, op=op):
                    spv = Spirv(translate(op, regs, stage=stage))
                    one_gather(spv)

    def test_texel_offset_fallback_does_not_change_a_gather_offset(self):
        # --texel-offset-fallback folds a sample's offset into its coordinates; a gather offset stays an Offset operand
        with patch.object(rdna2spirv, "TEXEL_OFFSET_FALLBACK", True):
            spv = Spirv(translate("image_gather4_c_lz_o", "v[0:3]"))
        _, _, _, _, _, off = one_gather(spv)
        self.assertNotEqual(off, "")


class Refusals(unittest.TestCase):
    def refused(self, text, *args, **kwargs):
        with self.assertRaises(rdna2spirv.Unsupported) as ctx:
            translate(*args, **kwargs)
        self.assertIn(text, str(ctx.exception))

    def test_dimensions_without_a_gather(self):
        # LLVM defines gather4 for 2D, Cube and 2DArray only
        self.refused("3D", "image_gather4_c_lz", "v[0:3]", "dim:SQ_RSRC_IMG_3D")
        self.refused("1D", "image_gather4_c_lz", "v[0:1]", "dim:SQ_RSRC_IMG_1D")

    def test_component_masks_other_than_the_depth_channel(self):
        for dmask in ("dmask:0x2", "dmask:0x4", "dmask:0x8", "dmask:0x3", "dmask:0xf"):
            with self.subTest(dmask=dmask):
                self.refused("dmask other than 0x1", "image_gather4_c_lz", "v[0:2]", dmask=dmask)

    def test_status_unnormalised_and_cache_modifiers(self):
        for extra, regs in (("tfe", "v[0:2]"), ("lwe", "v[0:2]"), ("unorm", "v[0:2]"), ("glc", "v[0:2]"), ("r128", "v[0:2]")):
            with self.subTest(extra=extra):
                with self.assertRaises(rdna2spirv.Unsupported):
                    translate("image_gather4_c_lz", regs, extra=extra, data="v[8:12]" if extra in ("tfe", "lwe") else "v[8:11]")

    def test_too_few_address_registers(self):
        self.refused("coords", "image_gather4_c_lz", "v[0:1]")
        self.refused("coords", "image_gather4_c_lz_o", "v[0:2]")


@unittest.skipUnless(shutil.which("spirv-as") and shutil.which("spirv-val"), "needs spirv-as and spirv-val on PATH")
class Validation(unittest.TestCase):
    def validate(self, text):
        with tempfile.TemporaryDirectory() as tmp:
            a, b = os.path.join(tmp, "m.spvasm"), os.path.join(tmp, "m.spv")
            with open(a, "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
            subprocess.run(["spirv-as", "--target-env", "vulkan1.2", a, "-o", b], check=True)
            subprocess.run(["spirv-val", "--target-env", "vulkan1.2", b], check=True)

    def test_every_form_validates(self):
        forms = []
        for op, plain, array in (("image_gather4_c_lz", ("v[0:2]", "v[0:1]"), ("v[0:3]", "v[0:2]")),
                                 ("image_gather4_c_lz_o", ("v[0:3]", "v[0:2]"), ("v[0:4]", "v[0:3]"))):
            for name, (full, packed) in (("2D", plain), ("ARR", array), ("CUBE", array)):
                forms.append((op, full, DIMS[name], ""))
                forms.append((op, packed, DIMS[name], "a16"))
                forms.append((op, full, DIMS[name], "d16"))
        for op, regs, dim, extra in forms:
            for stage in ("ps", "cs"):
                with self.subTest(op=op, regs=regs, dim=dim, extra=extra, stage=stage):
                    data = "v[8:9]" if extra == "d16" else "v[8:11]"
                    self.validate(translate(op, regs, dim, extra, stage=stage, data=data))


if __name__ == "__main__":
    unittest.main()
