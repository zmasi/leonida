"""Regression tests for SHADER-LDS-BOUND-ACCESS (lds_bound enforcement on ds_add[_rtn]_f32,
ds_cmpst[_rtn]_f32, ds_read_addtid_b32, and ds_write_addtid_b32).

When a compute shader has a bounded RSRC2 LDS allocation (SHADER-309: `lds_bound` set, including
non-power-of-two allocations such as 3 * 512B = 1536B = 384 dwords), every LDS instruction must
route through `lds_access` (bounds-checked `idx < lds_bound`, returning 0 on OOB reads and dropping
OOB writes) rather than calling `lds_ptr` (which bit-masks `idx & (lds_dwords - 1)` and corrupts
both OOB targets and non-power-of-two in-bounds addresses like `128 & 383 == 0`).
"""

import os
import struct
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TOOLS_DIR = os.environ.get("LEONIDA_TOOLS_DIR") or os.path.join(ROOT, "tools")

sys.dont_write_bytecode = True
sys.path.insert(0, TOOLS_DIR)
sys.path.insert(0, HERE)

import rdna2spirv  # noqa: E402
import spirv_mini_interp as si  # noqa: E402


def _make_cs_fssm(rsrc2: int, group=(32, 1, 1)) -> bytes:
    hdr = bytearray(0x148)
    struct.pack_into("<4I", hdr, 0x138, rsrc2, *group)
    return bytes(hdr)


def _translate_cs(lines, rsrc2=0x18088, w64_emulate=False) -> str:
    # rsrc2 = 0x18088 -> LDS_SIZE = 3 (1536 bytes = 384 dwords, non-power-of-two)
    with tempfile.TemporaryDirectory() as td:
        ddir = os.path.join(td, "disasm")
        fdir = os.path.join(td, "fssm")
        os.mkdir(ddir)
        os.mkdir(fdir)
        sp = os.path.join(ddir, "cs_lds_bound.s")
        with open(sp, "w", encoding="utf-8") as f:
            f.write("; cs_lds_bound\n" + "".join(f"\t{ln}\n" for ln in lines) + "\ts_endpgm\n")
        with open(os.path.join(fdir, "cs_lds_bound.fssm"), "wb") as f:
            f.write(_make_cs_fssm(rsrc2))
        prev = rdna2spirv.WAVE64_EMULATE
        try:
            rdna2spirv.WAVE64_EMULATE = w64_emulate
            return rdna2spirv.translate(sp)
        finally:
            rdna2spirv.WAVE64_EMULATE = prev


def _check_cfg(spv: str):
    body = []
    in_body = False
    for raw in spv.splitlines():
        line = raw.strip()
        if not line:
            continue
        m = si._LINE.match(line)
        assert m, f"unparseable SPIR-V line: {line!r}"
        res, op, rest = m.groups()
        if in_body:
            body.append((res, op, rest.split(), line))
        elif op == "OpFunction":
            in_body = True
    assert body, "empty SPIR-V function body"
    si.check_structure(body)


class LdsBoundAccessTests(unittest.TestCase):
    def _assert_no_bitmask_wrap_and_uses_bound(self, op_lines):
        # Include an OOB constant offset:1536 so g.lds_bound = g.lds_dwords = 384 (0x180).
        spv = _translate_cs(op_lines)
        _check_cfg(spv)
        # Bounded LDS must compare against %c180 (384) via OpULessThan and must NOT wrap with
        # OpBitwiseAnd against %c17f (383).
        self.assertIn("OpULessThan %bool", spv)
        self.assertIn("%c180", spv)
        self.assertNotIn("OpBitwiseAnd %uint", "\n".join(
            ln for ln in spv.splitlines() if "%c17f" in ln
        ))

    def test_ds_add_f32_uses_lds_bound(self):
        self._assert_no_bitmask_wrap_and_uses_bound([
            "v_mov_b32 v1, 512",
            "v_mov_b32 v2, 0x41000000",
            "ds_add_f32 v1, v2",
            "ds_add_rtn_f32 v3, v1, v2 offset:1536",
            "s_waitcnt lgkmcnt(0)",
        ])

    def test_ds_cmpst_f32_uses_lds_bound(self):
        self._assert_no_bitmask_wrap_and_uses_bound([
            "v_mov_b32 v1, 512",
            "v_mov_b32 v2, 0x3f800000",
            "v_mov_b32 v3, 0x40000000",
            "ds_cmpst_f32 v1, v2, v3",
            "ds_cmpst_rtn_f32 v4, v1, v2, v3 offset:1536",
            "s_waitcnt lgkmcnt(0)",
        ])

    def test_ds_addtid_uses_lds_bound(self):
        self._assert_no_bitmask_wrap_and_uses_bound([
            "s_mov_b32 m0, 512",
            "v_mov_b32 v1, 0x11223344",
            "ds_write_addtid_b32 v1",
            "ds_read_addtid_b32 v2 offset:1536",
            "s_waitcnt lgkmcnt(0)",
        ])

    def test_wave64_emulate_structure_valid(self):
        spv = _translate_cs([
            "v_mov_b32 v1, 512",
            "v_mov_b32 v2, 0x3f800000",
            "v_mov_b32 v3, 0x40000000",
            "ds_add_rtn_f32 v4, v1, v2 offset:1536",
            "ds_cmpst_rtn_f32 v5, v1, v2, v3 offset:1536",
            "s_mov_b32 m0, 512",
            "ds_write_addtid_b32 v2",
            "ds_read_addtid_b32 v6 offset:1536",
            "s_waitcnt lgkmcnt(0)",
        ], w64_emulate=True)
        _check_cfg(spv)


if __name__ == "__main__":
    unittest.main()
