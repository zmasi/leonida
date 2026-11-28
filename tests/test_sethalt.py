"""s_sethalt in tools/rdna2spirv.py.

s_sethalt sets the wave's HALT bit to SIMM16[0] (RDNA2 ISA, SOPP). A halt right before s_endpgm ends the wave where
the s_endpgm would; s_sethalt 0 changes nothing; a halt with more code after it is refused. Hand-written listings
only: no game files, no llvm-mc, no GPU.
"""
import struct

import pytest

import rdna2spirv
from listing import EMPTY_DXBC
from semantic.test_loop_ballot_barrier import VSHARP, _translate_cs, run_compute_workgroup


def translate_cs(tmp_path, body, name="cs_sethalt"):
    (tmp_path / "disasm").mkdir(exist_ok=True)
    (tmp_path / f"{name}.dxbc").write_bytes(EMPTY_DXBC)
    (tmp_path / "fssm").mkdir(exist_ok=True)
    hdr = bytearray(0x148)
    struct.pack_into("<4I", hdr, 0x138, 0x8c, 64, 1, 1)     # COMPUTE_PGM_RSRC2 (4 user SGPRs, TGID_X), 64x1x1 threads
    (tmp_path / "fssm" / f"{name}.fssm").write_bytes(bytes(hdr))
    lines = [ln.strip() for ln in body.splitlines() if ln.strip()]
    path = tmp_path / "disasm" / f"{name}.s"
    path.write_text(f"; {name} code 0x0..0x100\n" + "".join("\t" + ln + "\n" for ln in lines))
    return rdna2spirv.translate(str(path))


STORE = """
v_mov_b32 v0, s0
v_mov_b32 v1, s1
v_mov_b32 v2, 0x7fc00000
global_store_dword v[0:1], v2, off
"""


def test_halt_before_endpgm_keeps_the_store(tmp_path):
    plain = translate_cs(tmp_path, STORE + "s_endpgm", name="cs_plain")
    halted = translate_cs(tmp_path, STORE + "s_sethalt 1\ns_endpgm", name="cs_halted")
    assert halted == plain.replace("cs_plain", "cs_halted")


def test_resume_is_a_no_op(tmp_path):
    plain = translate_cs(tmp_path, STORE + "s_endpgm", name="cs_plain")
    resumed = translate_cs(tmp_path, "s_sethalt 0\n" + STORE + "s_endpgm", name="cs_resumed")
    assert resumed == plain.replace("cs_plain", "cs_resumed")


def test_halt_before_more_code_is_refused(tmp_path):
    with pytest.raises(rdna2spirv.Unsupported, match="s_sethalt 1 before more code"):
        translate_cs(tmp_path, "s_sethalt 1\n" + STORE + "s_endpgm")


@pytest.mark.parametrize("structured", [True, False])
@pytest.mark.parametrize("halt", [0, 1])
@pytest.mark.parametrize("target", [False, True])
def test_halt_keeps_branch_destination(structured, halt, target):
    condition = "s_cbranch_scc0 stopped\ns_branch ok" if target else "s_cbranch_scc1 ok"
    store = "v_lshlrev_b32_e32 v2, 2, v0\nv_mov_b32 v1, 0x5555\nbuffer_store_dword v1, v2, s[0:3], 0 offen\n"
    stopped = "s_endpgm\n" if halt else store.replace("0x5555", "0x7777") + "s_endpgm\n"
    program = f"s_cmp_eq_u32 s4, 0\n{condition}\nstopped:\ns_sethalt {halt}\n{stopped}ok:\n{store}s_endpgm"
    asm, _ = _translate_cs("cs_halt_branch", [line for line in program.splitlines() if line], structured=structured)

    def run(flag):
        return run_compute_workgroup(asm, 64, 64, push_constants=[*VSHARP[:4], flag])
    assert run(0) == {4 * lane: 0x5555 for lane in range(64)}
    assert run(1) == ({} if halt else {4 * lane: 0x7777 for lane in range(64)})
