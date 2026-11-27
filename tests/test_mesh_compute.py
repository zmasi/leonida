"""rdna2spirv.py --mesh-compute and tools/mesh_compute_vs.py: mesh shaders without VK_EXT_mesh_shader.

The inputs are the hand-written GPU tests runtime/test/shaders/ms_*.asm with conftest's synthetic PSV0/OSG1
containers (no game data). On the GPU, run_gpu_tests.py with LEONIDA_MESH_COMPUTE=1 dispatches each one as compute
and draws its slots with the generated vertex shader (vk_harness VKH_MESH_COMPUTE). Here:
* the slot layout arithmetic;
* the module is a GLCompute entry point with the mesh workgroup size and none of the mesh shader's capability,
  extension, execution modes, outputs or SetMeshOutputs; it reads the output address from push-constant members
  5/6 (bytes 144/148), and stores the header, vertices and primitives through it; it passes spirv-val, also with
  --wave64-emulate; the metadata records the layout and 152 push-constant bytes;
  the 152-byte producer also declares the GPU-driven indirect-tile contract: one exact OpModuleProcessed string in
  the module-level debug section, and a prologue that fetches a tagged TileMeta once inside its own structured
  selection (a direct launch never dereferences it), used by the epilogue for the local slot while firstGroup only
  offsets the original shader's v5; the --pc-tail-ubo variant and the mesh shader have neither;
* without the flag the module is the mesh shader it was;
* the generated vertex shader matches the layout and passes spirv-val;
* check.py's mesh_env picks the compute variables only when the flag, the layout and the vertex shader are there.
"""
import json
import os
import re
import subprocess
import sys

import pytest

from conftest import HAVE_SPIRV, ROOT, SAMPLE_DXBC, requires_tools

sys.path.insert(0, os.path.join(ROOT, "tools"))
import rdna2spirv  # noqa: E402
import mesh_compute_vs  # noqa: E402

sys.path.insert(0, os.path.join(ROOT, "runtime", "test", "shaders"))
import check  # noqa: E402

MESH_TESTS = ("ms_tri", "ms_clip", "ms_v5_workgroup", "ms_w64x_ballot", "ms_wave_info_s3")


def test_layout():
    lay = rdna2spirv.mesh_compute_layout(128, 128, 5, 0, [3, 0])
    # position, two vec4s of clip distances (5 padded to 8), param 0, param 3.
    assert lay["vertex_stride"] == 16 * (1 + 2 + 2)
    assert lay["clip_offset"] == 16 and lay["cull_offset"] == 48
    assert lay["params"] == {0: 48, 3: 64}
    assert lay["vertices"] == 16
    assert lay["primitives"] == 16 + 128 * lay["vertex_stride"]
    assert lay["slot_bytes"] == lay["primitives"] + 128 * 16
    assert lay["push_constant_bytes"] == 152 and lay["address_dwords"] == [36, 37]
    small = rdna2spirv.mesh_compute_layout(3, 1, 0, 0, [0])
    assert small["slot_bytes"] == 16 + 3 * 32 + 16


def _translate(make_shader, name, compute, emulate=False):
    with open(os.path.join(ROOT, "runtime", "test", "shaders", name + ".asm"), encoding="utf-8") as f:
        shader = make_shader(f.read(), name, dxbc=SAMPLE_DXBC[name])
    old = rdna2spirv.MESH_COMPUTE, rdna2spirv.WAVE64_EMULATE
    rdna2spirv.MESH_COMPUTE, rdna2spirv.WAVE64_EMULATE = compute, emulate
    try:
        spvasm = shader.translate()
        meta = rdna2spirv.shader_meta(rdna2spirv.translate.last_gen, spvasm)
    finally:
        rdna2spirv.MESH_COMPUTE, rdna2spirv.WAVE64_EMULATE = old
    return shader, spvasm, meta


@requires_tools
@pytest.mark.parametrize("emulate", [False, True], ids=["native", "wave64-emulate"])
@pytest.mark.parametrize("name", MESH_TESTS)
def test_module_is_compute(make_shader, name, emulate):
    shader, spvasm, meta = _translate(make_shader, name, True, emulate)
    shader.validate(spvasm)
    assert re.search(r'OpEntryPoint GLCompute %main "main"', spvasm)
    for gone in ("MeshShadingEXT", "SPV_EXT_mesh_shader", "OpSetMeshOutputsEXT", "OutputVertices",
                 "OutputPrimitivesEXT", "OutputTrianglesEXT", "PrimitiveTriangleIndicesEXT", "CullPrimitiveEXT",
                 "OpCapability ClipDistance", "BuiltIn ClipDistance", " Output\n"):
        assert gone not in spvasm, gone
    size = meta["local_size"]
    assert f"OpExecutionMode %main LocalSize {size[0]} {size[1]} {size[2]}" in spvasm
    assert "OpMemberDecorate %UserData 5 Offset 144" in spvasm
    assert "OpMemberDecorate %UserData 6 Offset 148" in spvasm
    lay = meta["mesh_compute"]
    assert meta["push_constant_bytes"] == 152
    assert lay["max_vertices"] == meta["max_vertices"] and lay["max_primitives"] == meta["max_primitives"]
    assert sorted(int(k) for k in lay["params"]) == meta["params"]
    # Header (2 words), position (4), each param (4), each clip/cull distance (1) and the primitive (4).
    stores = spvasm.count("OpConvertUToPtr %_ptr_PSB_uint %ma")
    assert stores == 2 + 4 + 4 * len(lay["params"]) + lay["clip_count"] + lay["cull_count"] + 4
    assert "%mso = OpIMul %ulong %mwl64" in spvasm          # 64-bit slot offset
    assert "mesh_compute" not in meta["features"]


@requires_tools
@pytest.mark.parametrize("name", ("ms_tri", "ms_clip"))
def test_default_is_still_a_mesh_shader(make_shader, name):
    shader, spvasm, meta = _translate(make_shader, name, False)
    assert 'OpEntryPoint MeshEXT %main "main"' in spvasm
    assert "OpSetMeshOutputsEXT" in spvasm and "OpCapability MeshShadingEXT" in spvasm
    assert "mesh_compute" not in meta and meta["push_constant_bytes"] == 144
    assert "%moa" not in spvasm


@requires_tools
def test_clip_distances_go_to_the_slot(make_shader):
    _, spvasm, meta = _translate(make_shader, "ms_clip", True)
    lay = meta["mesh_compute"]
    assert lay["clip_count"] >= 1 and lay["clip_offset"] == 16
    assert "OpAccessChain %_ptr_Function_float %ms_clip" in spvasm


def _module_ops(spvasm):
    """(opcode, operand words) of every instruction of the assembled module, in order."""
    import spirv_asm
    data = spirv_asm.assemble(spvasm, target_env="vulkan1.2")
    words = [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data), 4)]
    ops, pos = [], 5
    while pos < len(words):
        wc, op = words[pos] >> 16, words[pos] & 0xFFFF
        ops.append((op, words[pos + 1:pos + wc]))
        pos += wc
    return ops


@requires_tools
@pytest.mark.parametrize("emulate", [False, True], ids=["native", "wave64-emulate"])
@pytest.mark.parametrize("name", MESH_TESTS)
def test_indirect_tile_marker_and_prologue(make_shader, name, emulate):
    shader, spvasm, meta = _translate(make_shader, name, True, emulate)
    shader.validate(spvasm)
    assert meta["push_constant_bytes"] == 152 and meta["mesh_compute"]["address_dwords"] == [36, 37]
    # The marker: exactly one OpModuleProcessed (opcode 330) with the exact literal, NUL-terminated and padded, in
    # the debug section: only capabilities, extensions, imports, the memory model, the entry point and execution
    # modes precede it.
    ops = _module_ops(spvasm)
    marks = [i for i, (op, _) in enumerate(ops) if op == 330]
    assert len(marks) == 1
    literal = b"".join(w.to_bytes(4, "little") for w in ops[marks[0]][1])
    assert literal == b"leonida.mesh_indirect_tile.v1" + b"\0" * (len(literal) - len(b"leonida.mesh_indirect_tile.v1"))
    assert 0 < len(literal) - len(b"leonida.mesh_indirect_tile.v1") <= 4
    assert {op for op, _ in ops[:marks[0]]} <= {10, 11, 14, 15, 16, 17}      # OpExtension .. OpExecutionMode
    assert rdna2spirv.MESH_INDIRECT_TILE_MARKER == "leonida.mesh_indirect_tile.v1"
    # Structure: %mt_head is the first block, the tag test is one uniform selection whose only memory access is
    # the 16-byte TileMeta fetch (8-byte aligned) inside it; the merge block is %entry.
    assert re.search(r"%main = OpFunction[^\n]*\n\s*%mt_head = OpLabel", spvasm)
    assert re.search(r"%mt_bit = OpBitwiseAnd %uint %mt_lo %c1\n\s*%mt_tg = OpINotEqual %bool %mt_bit %c0\n\s*"
                     r"OpSelectionMerge %entry None\n\s*OpBranchConditional %mt_tg %mt_do %entry", spvasm)
    start = spvasm.index("%mt_do = OpLabel")
    inside = spvasm[start:spvasm.index("OpBranch %entry", start)]
    assert len(re.findall(r"OpLoad %v4uint\w* %mt_mp Aligned 8", inside)) == 1
    assert len(re.findall(r"OpLoad %v4uint\w* %mt_mp Aligned 8", spvasm)) == 1
    assert len(re.findall(r"%mt_mp = OpConvertUToPtr %_ptr_PSB_v4\w* %mt_ma", spvasm)) == 1
    assert "%mt_mp" not in spvasm[:start] and "%mt_mv" not in spvasm[:start]
    assert spvasm.count("%mt_lo_p = OpAccessChain %_ptr_PC_uint %user_data %c5") == 1
    assert spvasm.count("%mt_hi_p = OpAccessChain %_ptr_PC_uint %user_data %c6") == 1
    # firstGroup only offsets the shader-visible group index (v5); the epilogue's output address is the resolved
    # one and its slot index is the physical group (workgroup id and count), as before.
    assert spvasm.count("%mt_fgv") == 2 and "%wlin = OpIAdd %uint %wlin0 %mt_fgv" in spvasm
    assert spvasm.count("OpLoad %uint %mt_first") == 1
    assert "%moa = OpLoad %ulong %mt_out" in spvasm and "%moa_lo_p" not in spvasm
    assert "%mwl0 = OpIAdd %uint %mwx %mwyx" in spvasm and "%mso = OpIMul %ulong %mwl64" in spvasm


@requires_tools
def test_pc_tail_and_mesh_shader_have_no_tile_contract(make_shader):
    _, mesh, _ = _translate(make_shader, "ms_tri", False)
    assert "OpModuleProcessed" not in mesh and "%mt_" not in mesh
    old = rdna2spirv.PC_TAIL_UBO
    rdna2spirv.PC_TAIL_UBO = True
    try:
        _, tail, _ = _translate(make_shader, "ms_tri", True)
    finally:
        rdna2spirv.PC_TAIL_UBO = old
    assert "OpModuleProcessed" not in tail and "%mt_" not in tail and "%entry = OpLabel" in tail
    assert "%moa_lo_p = OpAccessChain %_ptr_Uniform_uint_tail %pc_tail %c4" in tail


def _vs_layout(clip=0, cull=0, params=(0,)):
    lay = rdna2spirv.mesh_compute_layout(128, 128, clip, cull, list(params))
    lay["params"] = {str(k): v for k, v in lay["params"].items()}
    return lay


def test_vertex_shader_text():
    text = mesh_compute_vs.vs_spvasm(_vs_layout(clip=1, params=(0, 4)))
    assert 'OpEntryPoint Vertex %main "main" %vertex_index %pc %out_pos %out_clip %out0 %out4' in text
    assert "OpDecorate %out_clip BuiltIn ClipDistance" in text and "OpCapability ClipDistance" in text
    assert "OpDecorate %out0 Location 0" in text and "OpDecorate %out4 Location 4" in text
    assert "OpMemberDecorate %PC 5 Offset 144" in text and "OpMemberDecorate %PC 6 Offset 148" in text
    assert "%pos_out = OpSelect %v4float %valid %pos %outside" in text
    assert "%cl" + str(_vs_layout(clip=1, params=(0, 4))["slot_bytes"]) in text
    plain = mesh_compute_vs.vs_spvasm(_vs_layout())
    assert "ClipDistance" not in plain and "CullDistance" not in plain


@pytest.mark.skipif(not HAVE_SPIRV, reason="needs spirv-as and spirv-val")
@pytest.mark.parametrize("clip,cull,params", [(0, 0, ()), (0, 0, (0,)), (1, 0, (0, 1)), (5, 2, (0, 2, 7)),
                                              (0, 0, tuple(range(18)))])
def test_vertex_shader_validates(tmp_path, clip, cull, params):
    asm = tmp_path / "vs.spvasm"
    asm.write_text(mesh_compute_vs.vs_spvasm(_vs_layout(clip, cull, params)), encoding="utf-8")
    spv = tmp_path / "vs.spv"
    for cmd in (["spirv-as", "--target-env", "vulkan1.2", str(asm), "-o", str(spv)],
                ["spirv-val", "--target-env", "vulkan1.2", str(spv)]):
        r = subprocess.run(cmd, capture_output=True, text=True)
        assert r.returncode == 0, r.stdout + r.stderr


def test_index_drives_the_vertex_shaders(tmp_path):
    index = {"ms_a": {"stage": "ms", "mesh_compute": _vs_layout(params=(0, 1))},
             "ms_b": {"stage": "ms"}, "ps_c": {"stage": "ps"}}
    (tmp_path / "index.json").write_text(json.dumps(index), encoding="utf-8")
    written = mesh_compute_vs.write_for_index(str(tmp_path))
    assert [os.path.basename(p) for p in written] == ["ms_a.mesh_vs.spvasm"]
    assert open(written[0], encoding="utf-8").read() == mesh_compute_vs.vs_spvasm(index["ms_a"]["mesh_compute"])
    assert mesh_compute_vs.write_for_index(str(tmp_path / "missing")) == []


def test_mesh_env(tmp_path, monkeypatch):
    lay = _vs_layout()
    (tmp_path / "index.json").write_text(json.dumps({"ms_a": {"mesh_compute": lay}, "ms_b": {}}), encoding="utf-8")
    (tmp_path / "ms_a.mesh_vs.spv").write_bytes(b"\0" * 4)
    monkeypatch.setattr(check, "MESH_COMPUTE", False)
    assert check.mesh_env(str(tmp_path), "ms_a", {})["VKH_MESH"] == "1"
    monkeypatch.setattr(check, "MESH_COMPUTE", True)
    env = check.mesh_env(str(tmp_path), "ms_a", {"KEEP": "1"})
    assert env["VKH_MESH_COMPUTE"] == str(tmp_path / "ms_a.mesh_vs.spv")
    assert env["VKH_MESH_SLOT"] == str(lay["slot_bytes"]) and env["VKH_MESH_PRIMS"] == "128"
    assert env["KEEP"] == "1" and "VKH_MESH" not in env
    # No layout, or no vertex shader: the plain mesh path (which then skips without VK_EXT_mesh_shader).
    assert check.mesh_env(str(tmp_path), "ms_b", {}) == {"VKH_MESH": "1"}
    (tmp_path / "ms_a.mesh_vs.spv").unlink()
    assert check.mesh_env(str(tmp_path), "ms_a", {}) == {"VKH_MESH": "1"}


def test_pc_tail_translator_layout_has_no_companion():
    old = rdna2spirv.PC_TAIL_UBO
    rdna2spirv.PC_TAIL_UBO = True
    try:
        lay = rdna2spirv.mesh_compute_layout(64, 3, 0, 0, [0])
        lay["params"] = {str(k): v for k, v in lay["params"].items()}
    finally:
        rdna2spirv.PC_TAIL_UBO = old
    assert lay["push_constant_bytes"] == 128 and lay["address_dwords"] is None
    assert lay["pc_tail_address_dwords"] == [4, 5]
    assert mesh_compute_vs.is_pc_tail_layout(lay)
    with pytest.raises(ValueError, match="128-byte.*pc-tail"):
        mesh_compute_vs.vs_spvasm(lay)
    # The normal 152-byte contract still passes.
    mesh_compute_vs.check_push_contract(_vs_layout())
