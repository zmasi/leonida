"""Verify v_mbcnt_lo_u32_b32 and v_mbcnt_hi_u32_b32 never emit out-of-range SPIR-V shifts.

In SPIR-V, OpShiftLeftLogical with Shift >= 32 produces an undefined value even on the discarded
side of OpSelect. In wave64, v_mbcnt_lo_u32_b32 has rel >= 32 on lanes 32..63; in both wave32
and wave64, v_mbcnt_hi_u32_b32 has rel = (lane - 32) >= 32 on lanes 0..31. This test checks that
the translated SPIR-V produces both exact per-lane outputs and zero _Poison values in
spirv_wave_interp._Wave across all lanes.

Standard library only; no GPU, no game files.
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
import spirv_wave_interp as sw  # noqa: E402

STUB_DXBC = b"DXBC" + bytes(24) + struct.pack("<I", 0) + bytes(4)


def translate_ps(name: str, lines: list[str], wave: int) -> str:
    with tempfile.TemporaryDirectory() as tmp:
        os.mkdir(os.path.join(tmp, "disasm"))
        os.mkdir(os.path.join(tmp, "fssm"))
        src = os.path.join(tmp, "disasm", f"ps_{name}.s")
        with open(src, "w", encoding="utf-8", newline="\n") as f:
            f.write(f"; ps_{name}\n")
            for ln in lines:
                f.write(f"\t{ln}\n")
        with open(os.path.join(tmp, f"ps_{name}.dxbc"), "wb") as f:
            f.write(STUB_DXBC)
        with open(os.path.join(tmp, "fssm", f"ps_{name}.fssm"), "wb") as f:
            hdr = bytearray(0x138)
            hdr[0:4] = b"FSSM"
            if wave == 32:
                struct.pack_into("<I", hdr, 0xD4, 0x20)
            f.write(bytes(hdr) + struct.pack("<4I", 0, 0, 0, 0))
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", ResourceWarning)
            spv = rdna2spirv.translate(src)
            gc.collect()
        return spv


def run_wave_checked(spv: str, wave: int) -> tuple[list[int], list[tuple[int, str, str]]]:
    w = sw._Wave(sw._Module(spv), wave, None, 2_000_000, None)
    outputs = w.run()
    poisons = [
        (i, ssa, val.why)
        for i, ln in enumerate(w.lanes)
        for ssa, val in ln.values.items()
        if isinstance(val, sw._Poison)
    ]
    mrt0_x = [outputs[i]["%out_mrt0"][0] for i in range(wave)]
    return mrt0_x, poisons


class MbcntShiftDefinedTests(unittest.TestCase):
    def test_v_mbcnt_lo_wave64_no_poison_and_exact(self):
        spv = translate_ps(
            "mbcnt_lo_w64",
            [
                "s_mov_b32 s4, 0x55555555",
                "v_mbcnt_lo_u32_b32 v0, s4, 0",
                "exp mrt0 v0, v0, v0, v0 done vm",
                "s_endpgm",
            ],
            wave=64,
        )
        got, poisons = run_wave_checked(spv, 64)
        self.assertEqual(poisons, [], f"v_mbcnt_lo_u32_b32 produced SPIR-V UB shifts: {poisons[:4]}")
        want = [((0x55555555 & ((1 << min(i, 32)) - 1)).bit_count()) for i in range(64)]
        self.assertEqual(got, want)

    def test_v_mbcnt_hi_wave32_and_wave64_subgroups_no_poison_and_exact(self):
        spv = translate_ps(
            "mbcnt_hi_w64",
            [
                "s_mov_b32 s4, 0xAAAAAAAA",
                "v_mbcnt_hi_u32_b32 v0, s4, 7",
                "exp mrt0 v0, v0, v0, v0 done vm",
                "s_endpgm",
            ],
            wave=64,
        )
        for subgroup in (32, 64):
            with self.subTest(subgroup=subgroup):
                got, poisons = run_wave_checked(spv, subgroup)
                self.assertEqual(
                    poisons, [],
                    f"v_mbcnt_hi_u32_b32 (subgroup {subgroup}) produced SPIR-V UB shifts: {poisons[:4]}",
                )
                want = [
                    7 if i < 32 else 7 + (0xAAAAAAAA & ((1 << (i - 32)) - 1)).bit_count()
                    for i in range(subgroup)
                ]
                self.assertEqual(got, want)

    def test_v_mbcnt_lo_hi_paired_rank_wave64_no_poison(self):
        spv = translate_ps(
            "mbcnt_rank_w64",
            [
                "v_mbcnt_lo_u32_b32 v0, -1, 0",
                "v_mbcnt_hi_u32_b32 v0, -1, v0",
                "exp mrt0 v0, v0, v0, v0 done vm",
                "s_endpgm",
            ],
            wave=64,
        )
        got, poisons = run_wave_checked(spv, 64)
        self.assertEqual(poisons, [], f"paired v_mbcnt wave64 produced SPIR-V UB shifts: {poisons[:4]}")
        self.assertEqual(got, list(range(64)))


if __name__ == "__main__":
    unittest.main()
