"""Semantic end-to-end tests for negative LLVM 23 lit(-V) operands in shader_ir + rdna2spirv.

Verifies that lit(-1) and lit(-0x10) keep their negative integer value (0xFFFFFFFF / 0xFFFFFFF0)
rather than being rewritten to VOP3 neg(1) / neg(0x10) (sign-bit flip 0x80000001 / 0x80000010),
while -lit(...) still applies VOP3 FP negation.
"""
from __future__ import annotations

import gc
import os
import struct
import sys
import tempfile
import unittest
import warnings

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TOOLS_DIR = os.environ.get("LEONIDA_TOOLS_DIR") or os.path.join(ROOT, "tools")

sys.dont_write_bytecode = True
for _p in (HERE, TOOLS_DIR):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import rdna2spirv  # noqa: E402
import spirv_mini_interp as si  # noqa: E402

STUB_DXBC = b"DXBC" + bytes(24) + struct.pack("<I", 0) + bytes(4)
VSHARP = [0, 0, 0xFFFFFFFF, 0x31016FAC]


def translate_and_run_cs(name: str, lines: list[str]) -> dict[int, int]:
    with tempfile.TemporaryDirectory() as tmp:
        os.mkdir(os.path.join(tmp, "disasm"))
        os.mkdir(os.path.join(tmp, "fssm"))
        src = os.path.join(tmp, "disasm", f"cs_{name}.s")
        with open(src, "w", encoding="utf-8", newline="\n") as f:
            f.write(f"; cs_{name}\n")
            for ln in lines:
                f.write(f"\t{ln}\n")
        with open(os.path.join(tmp, f"cs_{name}.dxbc"), "wb") as f:
            f.write(STUB_DXBC)
        with open(os.path.join(tmp, "fssm", f"cs_{name}.fssm"), "wb") as f:
            hdr = bytearray(0x138)
            hdr[0:4] = b"FSSM"
            struct.pack_into("<I", hdr, 0xD4, 0x20)
            f.write(bytes(hdr) + struct.pack("<4I", 0x8090, 32, 1, 1))
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", ResourceWarning)
            spv = rdna2spirv.translate(src)
            gc.collect()
    return si.run_workgroup(
        spv, 32, 32,
        inputs_for=lambda i: {"%wg_id": (0, 0, 0), "%local_id": (i, 0, 0), "%sg_lane": i},
        push_constants=VSHARP + [0] * 28, memory={},
    )


class ShaderIrLitNegSemanticTests(unittest.TestCase):
    def test_negative_lit_integer_and_float_operands(self):
        asm = [
            "s_mov_b32 s4, lit(-1)",
            "v_mov_b32_e32 v1, s4",
            "v_add_nc_u32_e32 v2, lit(-0x10), v0",
            "v_mov_b32_e32 v3, lit(-1.5)",
            "v_add_f32_e64 v4, 0.5, -lit(-1.5)",
            "v_lshlrev_b32_e32 v6, 2, v0",
            "buffer_store_dword v1, v6, s[0:3], 0 offen",
            "buffer_store_dword v2, v6, s[0:3], 0 offen offset:128",
            "buffer_store_dword v3, v6, s[0:3], 0 offen offset:256",
            "buffer_store_dword v4, v6, s[0:3], 0 offen offset:384",
            "s_endpgm",
        ]
        mem = translate_and_run_cs("lit_neg", asm)
        f_neg15 = struct.unpack("<I", struct.pack("<f", -1.5))[0]
        f_20 = struct.unpack("<I", struct.pack("<f", 2.0))[0]
        self.assertEqual([mem[4 * i] for i in range(4)], [0xFFFFFFFF] * 4)
        self.assertEqual([mem[128 + 4 * i] for i in range(4)], [(i - 16) & 0xFFFFFFFF for i in range(4)])
        self.assertEqual([mem[256 + 4 * i] for i in range(4)], [f_neg15] * 4)
        self.assertEqual([mem[384 + 4 * i] for i in range(4)], [f_20] * 4)


if __name__ == "__main__":
    unittest.main()
