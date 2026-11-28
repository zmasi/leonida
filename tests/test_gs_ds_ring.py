"""Synthetic translation and rasterization checks for legacy GS ring accesses."""
import os
from pathlib import Path
import struct
import subprocess

import pytest

from conftest import make_dxbc, requires_tools, signature_chunk


GS_DXBC = make_dxbc([signature_chunk(b"ISG1", [(0, 1, 0xF)]),
                     signature_chunk(b"OSG1", [(0, 1, 0xF), (15, 0, 0xF)])])
GS_REFERENCE = (Path(__file__).parent / "semantic/fixtures/gs_ds_ring.asm").read_text(encoding="utf-8")
WIDE_AND_STRIDED = [
    ("ds_read_b32 v20, v11\nds_read_b32 v21, v10", "ds_read2st64_b32 v[20:21], v10 offset0:1"),
    ("ds_write_b32 v12, v30 offset:4\nds_write_b64 v12, v[31:32] offset:8",
     "ds_write_b96 v12, v[30:32] offset:4"),
    ("ds_write_b64 v12, v[40:41] offset:512\nds_write_b64 v12, v[42:43] offset:520",
     "ds_write_b128 v12, v[40:43] offset:512"),
    ("ds_write2_b32 v12, v20, v21 offset1:64", "ds_write2st64_b32 v12, v20, v21 offset1:1"),
]


@requires_tools
@pytest.mark.parametrize("original,replacement", WIDE_AND_STRIDED, ids=[
    "read2st64", "write96", "write128", "write2st64",
])
def test_geometry_lds_translation(make_shader, original, replacement):
    shader = make_shader(GS_REFERENCE.replace(original, replacement), name="gs_ring", dxbc=GS_DXBC)
    spvasm = shader.translate()
    assert "OpEntryPoint Geometry" in spvasm
    shader.validate(spvasm)


@requires_tools
def test_geometry_lds_rasterization(make_shader):
    harness = os.environ.get("LEONIDA_VK_HARNESS")
    if not harness:
        pytest.skip("set LEONIDA_VK_HARNESS to the built vk_harness")

    # Input vertex 1 has x=3 and vertex 2 has x=1. The GS needs both
    # values, in reverse order, to form its full-screen triangle.
    vs = make_shader("""
v_lshlrev_b32 v1, 1, v0
v_and_b32 v1, 2, v1
v_cvt_f32_u32 v1, v1
v_add_f32 v1, 1.0, v1
v_mov_b32 v2, 0
v_mov_b32 v3, 1.0
exp pos0 v1, v2, v2, v3 done
s_endpgm
""", name="vs_ring")
    ps = make_shader("v_mov_b32 v0, 1.0\nexp mrt0 v0, v0, v0, v0 done\ns_endpgm", name="ps_ring")
    for shader in (vs, ps):
        shader.validate(shader.translate())

    combined = GS_REFERENCE
    for original, replacement in WIDE_AND_STRIDED:
        combined = combined.replace(original, replacement)
    # With no stride, the second write hits vertex 0's y instead of
    # vertex 1's x. All three x coordinates are then -1.
    unstrided = GS_REFERENCE.replace("offset1:64", "offset1:1")
    env = {key: value for key, value in os.environ.items() if not key.startswith("VKH_")}
    for name, asm, expected in [("gs_control", unstrided, -1), ("gs_reference", GS_REFERENCE, 1),
                                ("gs_wide_strided", combined, 1)]:
        gs = make_shader(asm, name=name, dxbc=GS_DXBC)
        gs.validate(gs.translate())
        out = gs.workdir / f"{name}.raw"
        run = subprocess.run(
            [harness, gs.workdir / "vs_ring.spv", gs.workdir / "ps_ring.spv", out, "8", "8"],
            env=dict(env, VKH_GS=str(gs.workdir / f"{name}.spv")), capture_output=True, text=True, timeout=30,
        )
        if run.returncode == 77:
            pytest.skip(run.stderr)
        assert run.returncode == 0, run.stderr
        assert out.read_bytes() == struct.pack("<4f", *([expected] * 4)) * 64, run.stderr
