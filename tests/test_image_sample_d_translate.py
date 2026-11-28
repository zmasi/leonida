"""Translation and SPIR-V structure tests for image_sample_d and image_sample_d_cl in tools/rdna2spirv.py.

Tests call the real rdna2spirv.translate() on hand-written RDNA2 listings using the Python standard library only:
    python -I tests/test_image_sample_d_translate.py -v
"""
import gc
import os
import re
import struct
import sys
import tempfile
import unittest
import warnings

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(ROOT, "tools"))
import rdna2spirv  # noqa: E402

# Minimal valid DXBC container with zero parts (no declared PS inputs).
STUB_DXBC = b"DXBC" + bytes(24) + struct.pack("<I", 0) + bytes(4)


def translate_lines(name, lines, dxbc=STUB_DXBC):
    """Translate a hand-written RDNA2 listing via rdna2spirv.translate()."""
    text = f"; {name}  hand-written test program\n" + "".join(f"\t{ln.strip()}\n" for ln in lines)
    with tempfile.TemporaryDirectory() as tmp:
        os.mkdir(os.path.join(tmp, "disasm"))
        s_path = os.path.join(tmp, "disasm", f"{name}.s")
        with open(s_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        with open(os.path.join(tmp, f"{name}.dxbc"), "wb") as f:
            f.write(dxbc)
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", ResourceWarning)
            result = rdna2spirv.translate(s_path)
            gc.collect()
        return result


def vgpr_stores(spvasm):
    """Return [(vgpr_index, stored_id), ...] in emission order for stores into %vgpr."""
    ptr_to_idx = {}
    const_u = {}
    stores = []
    for line in spvasm.splitlines():
        m = re.match(r"\s*(%\w+) = OpConstant %uint (\d+)", line)
        if m:
            const_u[m.group(1)] = int(m.group(2))
            continue
        m = re.match(r"\s*(%\w+) = OpAccessChain %_ptr_Function_uint %vgpr (%\w+)", line)
        if m and m.group(2) in const_u:
            ptr_to_idx[m.group(1)] = const_u[m.group(2)]
            continue
        m = re.match(r"\s*OpStore (%\w+) (%\w+)", line)
        if m and m.group(1) in ptr_to_idx:
            stores.append((ptr_to_idx[m.group(1)], m.group(2)))
    return stores


def parse_defs(spvasm):
    """Map %id -> (opcode, operands_list) for SSA instructions in SPIR-V assembly."""
    defs = {}
    for line in spvasm.splitlines():
        m = re.match(r"\s*(%\w+) = (Op\w+)(?: (.*))?$", line)
        if m:
            rest = m.group(3).split() if m.group(3) else []
            defs[m.group(1)] = (m.group(2), rest)
    return defs


def unwrap_dst_value(defs, sid):
    """Unwrap EXEC-masked OpSelect %uint <act> <new_val> <old_val> emitted by Gen.dst()."""
    op, args = defs[sid]
    if op == "OpSelect":
        return args[2]
    return sid


def find_sample_explicit_lod(defs):
    """Return [(res_id, args)] for all OpImageSampleExplicitLod instructions."""
    return [(rid, args) for rid, (op, args) in defs.items() if op == "OpImageSampleExplicitLod"]


class ImageSampleDTranslateTests(unittest.TestCase):
    def test_1d_emits_scalar_grad_and_sampled1d(self):
        # 1D: dx (v0), dy (v1), u (v2) -> 3 address VGPRs.
        spv = translate_lines("ps_sample_d_1d", [
            "v_mov_b32 v0, 0.125",
            "v_mov_b32 v1, 0.25",
            "v_mov_b32 v2, 0.5",
            "image_sample_d v[8:11], v[0:2], s[8:15], s[16:19] dmask:0xf dim:SQ_RSRC_IMG_1D",
            "exp mrt0 v8, v9, v10, v11 done vm",
            "s_endpgm",
        ])
        self.assertIn("OpCapability Sampled1D", spv)
        self.assertNotIn("OpCapability MinLod", spv)
        self.assertIn("OpAccessChain %_ptr_UC_img1 %images_1d ", spv)
        self.assertIn("OpLoad %img1d ", spv)
        self.assertIn("OpSampledImage %simg1d ", spv)

        defs = parse_defs(spv)
        samples = find_sample_explicit_lod(defs)
        self.assertEqual(len(samples), 1)
        _, args = samples[0]
        # args: [%v4float, sampled_img, uv, "Grad", dx, dy]
        self.assertEqual(args[0], "%v4float")
        self.assertEqual(args[3], "Grad")
        self.assertEqual(len(args), 6)
        # 1D uses scalar %float for uv, dx, dy (not OpCompositeConstruct).
        self.assertEqual(defs[args[2]][0], "OpBitcast")
        self.assertEqual(defs[args[4]][0], "OpBitcast")
        self.assertEqual(defs[args[5]][0], "OpBitcast")

    def test_2d_emits_v2float_grad_and_v2float_uv(self):
        # 2D: dx=(v0,v1), dy=(v2,v3), uv=(v4,v5) -> 6 address VGPRs.
        spv = translate_lines("ps_sample_d_2d", [
            "v_mov_b32 v0, 0.1",
            "v_mov_b32 v1, 0.2",
            "v_mov_b32 v2, 0.3",
            "v_mov_b32 v3, 0.4",
            "v_mov_b32 v4, 0.5",
            "v_mov_b32 v5, 0.6",
            "image_sample_d v[8:11], v[0:5], s[8:15], s[16:19] dmask:0xf dim:SQ_RSRC_IMG_2D",
            "exp mrt0 v8, v9, v10, v11 done vm",
            "s_endpgm",
        ])
        self.assertNotIn("OpCapability MinLod", spv)
        self.assertIn("OpAccessChain %_ptr_UC_img %images ", spv)
        self.assertIn("OpLoad %img2d ", spv)
        self.assertIn("OpSampledImage %simg2d ", spv)

        defs = parse_defs(spv)
        samples = find_sample_explicit_lod(defs)
        self.assertEqual(len(samples), 1)
        _, args = samples[0]
        self.assertEqual(args[3], "Grad")
        self.assertEqual(len(args), 6)
        uv_id, dx_id, dy_id = args[2], args[4], args[5]
        for vid in (uv_id, dx_id, dy_id):
            self.assertEqual(defs[vid][0], "OpCompositeConstruct")
            self.assertEqual(defs[vid][1][0], "%v2float")
            self.assertEqual(len(defs[vid][1]) - 1, 2)

    def test_2d_array_emits_v2float_grad_and_v3float_uv(self):
        # 2D_ARRAY: dx=(v0,v1), dy=(v2,v3), uv_layer=(v4,v5,v6) -> 7 address VGPRs.
        spv = translate_lines("ps_sample_d_2da", [
            "v_mov_b32 v0, 0.1",
            "v_mov_b32 v1, 0.2",
            "v_mov_b32 v2, 0.3",
            "v_mov_b32 v3, 0.4",
            "v_mov_b32 v4, 0.5",
            "v_mov_b32 v5, 0.6",
            "v_mov_b32 v6, 1.0",
            "image_sample_d v[8:11], v[0:6], s[8:15], s[16:19] dmask:0xf dim:SQ_RSRC_IMG_2D_ARRAY",
            "exp mrt0 v8, v9, v10, v11 done vm",
            "s_endpgm",
        ])
        self.assertIn("OpAccessChain %_ptr_UC_imga %images_arr ", spv)
        self.assertIn("OpLoad %img2da ", spv)
        self.assertIn("OpSampledImage %simg2da ", spv)

        defs = parse_defs(spv)
        samples = find_sample_explicit_lod(defs)
        self.assertEqual(len(samples), 1)
        _, args = samples[0]
        self.assertEqual(args[3], "Grad")
        uv_id, dx_id, dy_id = args[2], args[4], args[5]
        self.assertEqual(defs[uv_id][:2], ("OpCompositeConstruct", ["%v3float"] + defs[uv_id][1][1:]))
        self.assertEqual(len(defs[uv_id][1]) - 1, 3)
        self.assertEqual(defs[dx_id][1][0], "%v2float")
        self.assertEqual(defs[dy_id][1][0], "%v2float")

    def test_cube_emits_v2float_grad_and_v3float_cube_coords(self):
        # CUBE: dx=(v0,v1), dy=(v2,v3), cube=(s, t, face)=(v4,v5,v6) -> 7 address VGPRs.
        spv = translate_lines("ps_sample_d_cube", [
            "v_mov_b32 v0, 0.1",
            "v_mov_b32 v1, 0.2",
            "v_mov_b32 v2, 0.3",
            "v_mov_b32 v3, 0.4",
            "v_mov_b32 v4, 1.5",
            "v_mov_b32 v5, 1.5",
            "v_mov_b32 v6, 2.0",
            "image_sample_d v[8:11], v[0:6], s[8:15], s[16:19] dmask:0xf dim:SQ_RSRC_IMG_CUBE",
            "exp mrt0 v8, v9, v10, v11 done vm",
            "s_endpgm",
        ])
        self.assertIn("OpAccessChain %_ptr_UC_imga %images_arr ", spv)
        self.assertIn("OpLoad %img2da ", spv)
        self.assertIn("OpSampledImage %simg2da ", spv)

        defs = parse_defs(spv)
        samples = find_sample_explicit_lod(defs)
        self.assertEqual(len(samples), 1)
        _, args = samples[0]
        self.assertEqual(args[3], "Grad")
        uv_id, dx_id, dy_id = args[2], args[4], args[5]
        self.assertEqual(defs[uv_id][0], "OpCompositeConstruct")
        self.assertEqual(defs[uv_id][1][0], "%v3float")
        # cube_coords subtracts 1.0 from s and t.
        s_comp = defs[uv_id][1][1]
        self.assertEqual(defs[s_comp][0], "OpFSub")
        self.assertEqual(defs[dx_id][1][0], "%v2float")
        self.assertEqual(defs[dy_id][1][0], "%v2float")

    def test_3d_emits_v3float_grad_and_v3float_uv(self):
        # 3D: dx=(v0,v1,v2), dy=(v3,v4,v5), uvw=(v6,v7,v8) -> 9 address VGPRs.
        spv = translate_lines("ps_sample_d_3d", [
            "v_mov_b32 v0, 0.1",
            "v_mov_b32 v1, 0.2",
            "v_mov_b32 v2, 0.3",
            "v_mov_b32 v3, 0.4",
            "v_mov_b32 v4, 0.5",
            "v_mov_b32 v5, 0.6",
            "v_mov_b32 v6, 0.25",
            "v_mov_b32 v7, 0.5",
            "v_mov_b32 v8, 0.75",
            "image_sample_d v[9:12], v[0:8], s[8:15], s[16:19] dmask:0xf dim:SQ_RSRC_IMG_3D",
            "exp mrt0 v9, v10, v11, v12 done vm",
            "s_endpgm",
        ])
        self.assertIn("OpAccessChain %_ptr_UC_img3 %images_3d ", spv)
        self.assertIn("OpLoad %img3d ", spv)
        self.assertIn("OpSampledImage %simg3d ", spv)

        defs = parse_defs(spv)
        samples = find_sample_explicit_lod(defs)
        self.assertEqual(len(samples), 1)
        _, args = samples[0]
        self.assertEqual(args[3], "Grad")
        for vid in (args[2], args[4], args[5]):
            self.assertEqual(defs[vid][0], "OpCompositeConstruct")
            self.assertEqual(defs[vid][1][0], "%v3float")
            self.assertEqual(len(defs[vid][1]) - 1, 3)

    def test_image_sample_d_cl_emits_grad_minlod_and_capability(self):
        # 2D _d_cl: dx=(v0,v1), dy=(v2,v3), uv=(v4,v5), minlod=v6 -> 7 address VGPRs.
        spv = translate_lines("ps_sample_d_cl_2d", [
            "v_mov_b32 v0, 0.1",
            "v_mov_b32 v1, 0.2",
            "v_mov_b32 v2, 0.3",
            "v_mov_b32 v3, 0.4",
            "v_mov_b32 v4, 0.5",
            "v_mov_b32 v5, 0.6",
            "v_mov_b32 v6, 1.5",
            "image_sample_d_cl v[8:11], v[0:6], s[8:15], s[16:19] dmask:0xf dim:SQ_RSRC_IMG_2D",
            "exp mrt0 v8, v9, v10, v11 done vm",
            "s_endpgm",
        ])
        self.assertIn("OpCapability MinLod", spv)
        defs = parse_defs(spv)
        samples = find_sample_explicit_lod(defs)
        self.assertEqual(len(samples), 1)
        _, args = samples[0]
        # args: [%v4float, sampled_img, uv, "Grad|MinLod", dx, dy, minlod]
        self.assertEqual(args[3], "Grad|MinLod")
        self.assertEqual(len(args), 7)
        self.assertEqual(defs[args[4]][1][0], "%v2float")
        self.assertEqual(defs[args[5]][1][0], "%v2float")
        # minlod comes from v6 bitcast to %float.
        self.assertEqual(defs[args[6]][0], "OpBitcast")
        self.assertEqual(defs[args[6]][1][0], "%float")

    def test_a16_keeps_gradients_32bit_and_unpacks_coords_and_clamp(self):
        # 2D _d_cl a16: dx=(v0,v1), dy=(v2,v3) as f32; v4=(u,v) as f16x2; v5=(minlod,pad) as f16x2.
        spv = translate_lines("ps_sample_d_cl_a16", [
            "v_mov_b32 v0, 0.1",
            "v_mov_b32 v1, 0.2",
            "v_mov_b32 v2, 0.3",
            "v_mov_b32 v3, 0.4",
            "v_mov_b32 v4, 0x38003400",
            "v_mov_b32 v5, 0x00003c00",
            "image_sample_d_cl v[8:11], v[0:5], s[8:15], s[16:19] dmask:0xf dim:SQ_RSRC_IMG_2D a16",
            "exp mrt0 v8, v9, v10, v11 done vm",
            "s_endpgm",
        ])
        self.assertIn("OpCapability MinLod", spv)
        self.assertEqual(spv.count("UnpackHalf2x16"), 2)
        defs = parse_defs(spv)
        samples = find_sample_explicit_lod(defs)
        self.assertEqual(len(samples), 1)
        _, args = samples[0]
        self.assertEqual(args[3], "Grad|MinLod")
        # Gradients are built from 32-bit float bitcasts, not UnpackHalf2x16.
        dx_c0 = defs[args[4]][1][1]
        self.assertEqual(defs[dx_c0][0], "OpBitcast")
        # Coordinates and minlod come from UnpackHalf2x16 -> OpCompositeExtract.
        uv_c0 = defs[args[2]][1][1]
        self.assertEqual(defs[uv_c0][0], "OpCompositeExtract")
        self.assertEqual(defs[defs[uv_c0][1][1]][0], "OpExtInst")
        minlod_id = args[6]
        self.assertEqual(defs[minlod_id][0], "OpCompositeExtract")
        self.assertEqual(defs[defs[minlod_id][1][1]][0], "OpExtInst")

    def test_partial_dmask_compacts_written_vgprs(self):
        # dmask:0x5 (channels 0 and 2 -> R and B) compacts into v8 and v9 only.
        spv = translate_lines("ps_sample_d_dmask5", [
            "v_mov_b32 v0, 0.1",
            "v_mov_b32 v1, 0.2",
            "v_mov_b32 v2, 0.3",
            "v_mov_b32 v3, 0.4",
            "v_mov_b32 v4, 0.5",
            "v_mov_b32 v5, 0.6",
            "image_sample_d v[8:9], v[0:5], s[8:15], s[16:19] dmask:0x5 dim:SQ_RSRC_IMG_2D",
            "exp mrt0 v8, v9, v8, v9 done vm",
            "s_endpgm",
        ])
        defs = parse_defs(spv)
        written = [(idx, unwrap_dst_value(defs, sid)) for idx, sid in vgpr_stores(spv) if idx >= 8]
        self.assertEqual([idx for idx, _ in written], [8, 9])
        extracted = [defs[defs[sid][1][1]][1][2] for _, sid in written]
        self.assertEqual(extracted, ["0", "2"])

    def test_non_ps_stage_supports_image_sample_d(self):
        spv = translate_lines("vs_sample_d", [
            "v_mov_b32 v0, 0.1",
            "v_mov_b32 v1, 0.2",
            "v_mov_b32 v2, 0.3",
            "v_mov_b32 v3, 0.4",
            "v_mov_b32 v4, 0.5",
            "v_mov_b32 v5, 0.6",
            "image_sample_d v[8:11], v[0:5], s[8:15], s[16:19] dmask:0xf dim:SQ_RSRC_IMG_2D",
            "s_endpgm",
        ])
        defs = parse_defs(spv)
        samples = find_sample_explicit_lod(defs)
        self.assertEqual(len(samples), 1)
        self.assertEqual(samples[0][1][3], "Grad")

    def test_unsupported_variants_dims_and_short_addrs_raise(self):
        # _o and _c_d variants are not in rdna2spirv's dispatch table and must raise Unsupported.
        for op in ("image_sample_d_o", "image_sample_d_cl_o", "image_sample_c_d", "image_sample_c_d_cl"):
            with self.subTest(op=op):
                with self.assertRaises(rdna2spirv.Unsupported):
                    translate_lines("ps_sample_d_unsup_op", [
                        "v_mov_b32 v0, 0",
                        f"{op} v[8:11], v[0:7], s[8:15], s[16:19] dmask:0xf dim:SQ_RSRC_IMG_2D",
                        "exp mrt0 v8, v9, v10, v11 done vm",
                        "s_endpgm",
                    ])
        # Unsupported dimension.
        with self.assertRaises(rdna2spirv.Unsupported):
            translate_lines("ps_sample_d_bad_dim", [
                "v_mov_b32 v0, 0",
                "image_sample_d v[8:11], v[0:5], s[8:15], s[16:19] dmask:0xf dim:SQ_RSRC_IMG_1D_ARRAY",
                "exp mrt0 v8, v9, v10, v11 done vm",
                "s_endpgm",
            ])
        # 3D gradient with tfe/lwe status flags is explicitly rejected.
        for flag in ("tfe", "lwe"):
            with self.subTest(flag=flag):
                with self.assertRaises(rdna2spirv.Unsupported):
                    translate_lines("ps_sample_d_3d_status", [
                        "v_mov_b32 v0, 0",
                        f"image_sample_d v[9:13], v[0:8], s[8:15], s[16:19] dmask:0xf dim:SQ_RSRC_IMG_3D {flag}",
                        "exp mrt0 v9, v10, v11, v12 done vm",
                        "s_endpgm",
                    ])
        # Too few address registers for 2D _d (needs 6) or _d_cl (needs 7).
        with self.assertRaises(rdna2spirv.Unsupported):
            translate_lines("ps_sample_d_short", [
                "v_mov_b32 v0, 0",
                "image_sample_d v[8:11], v[0:4], s[8:15], s[16:19] dmask:0xf dim:SQ_RSRC_IMG_2D",
                "exp mrt0 v8, v9, v10, v11 done vm",
                "s_endpgm",
            ])
        with self.assertRaises(rdna2spirv.Unsupported):
            translate_lines("ps_sample_d_cl_short", [
                "v_mov_b32 v0, 0",
                "image_sample_d_cl v[8:11], v[0:5], s[8:15], s[16:19] dmask:0xf dim:SQ_RSRC_IMG_2D",
                "exp mrt0 v8, v9, v10, v11 done vm",
                "s_endpgm",
            ])


class ImageSampleDNegativeControlTests(unittest.TestCase):
    def test_image_sample_without_d_does_not_emit_grad(self):
        # 2D_ARRAY implicit sample emits OpImageSampleImplicitLod without Grad.
        spv_imp = translate_lines("ps_sample_2da", [
            "v_mov_b32 v0, 0.5",
            "v_mov_b32 v1, 0.5",
            "v_mov_b32 v2, 1.0",
            "image_sample v[8:11], v[0:2], s[8:15], s[16:19] dmask:0xf dim:SQ_RSRC_IMG_2D_ARRAY",
            "exp mrt0 v8, v9, v10, v11 done vm",
            "s_endpgm",
        ])
        self.assertIn("OpImageSampleImplicitLod", spv_imp)
        self.assertNotIn("Grad", spv_imp)
        self.assertNotIn("MinLod", spv_imp)

        # image_sample_l emits OpImageSampleExplicitLod with Lod, without Grad.
        spv_lod = translate_lines("ps_sample_l_2da", [
            "v_mov_b32 v0, 0.5",
            "v_mov_b32 v1, 0.5",
            "v_mov_b32 v2, 1.0",
            "v_mov_b32 v3, 2.0",
            "image_sample_l v[8:11], v[0:3], s[8:15], s[16:19] dmask:0xf dim:SQ_RSRC_IMG_2D_ARRAY",
            "exp mrt0 v8, v9, v10, v11 done vm",
            "s_endpgm",
        ])
        self.assertIn("OpImageSampleExplicitLod", spv_lod)
        self.assertIn(" Lod ", spv_lod)
        self.assertNotIn("Grad", spv_lod)

    def test_changing_dmask_changes_written_vgprs_and_channels(self):
        spv_a = translate_lines("ps_sample_d_dmask_a", [
            "v_mov_b32 v0, 0.1",
            "v_mov_b32 v1, 0.2",
            "v_mov_b32 v2, 0.3",
            "v_mov_b32 v3, 0.4",
            "v_mov_b32 v4, 0.5",
            "v_mov_b32 v5, 0.6",
            "image_sample_d v[8:9], v[0:5], s[8:15], s[16:19] dmask:0xa dim:SQ_RSRC_IMG_2D",
            "exp mrt0 v8, v9, v8, v9 done vm",
            "s_endpgm",
        ])
        defs_a = parse_defs(spv_a)
        written_a = [(idx, unwrap_dst_value(defs_a, sid)) for idx, sid in vgpr_stores(spv_a) if idx >= 8]
        self.assertEqual([idx for idx, _ in written_a], [8, 9])
        self.assertEqual([defs_a[defs_a[sid][1][1]][1][2] for _, sid in written_a], ["1", "3"])

        spv_1 = translate_lines("ps_sample_d_dmask_1", [
            "v_mov_b32 v0, 0.1",
            "v_mov_b32 v1, 0.2",
            "v_mov_b32 v2, 0.3",
            "v_mov_b32 v3, 0.4",
            "v_mov_b32 v4, 0.5",
            "v_mov_b32 v5, 0.6",
            "image_sample_d v8, v[0:5], s[8:15], s[16:19] dmask:0x1 dim:SQ_RSRC_IMG_2D",
            "exp mrt0 v8, v8, v8, v8 done vm",
            "s_endpgm",
        ])
        defs_1 = parse_defs(spv_1)
        written_1 = [(idx, unwrap_dst_value(defs_1, sid)) for idx, sid in vgpr_stores(spv_1) if idx >= 8]
        self.assertEqual([idx for idx, _ in written_1], [8])
        self.assertEqual([defs_1[defs_1[sid][1][1]][1][2] for _, sid in written_1], ["0"])


if __name__ == "__main__":
    unittest.main()
