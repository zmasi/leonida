"""Unit tests for tools/hw_oracle/asm.py GFX12 raw loads, per-kernel wave metadata, and Kernel validation.

    python -I tests/test_hw_oracle_asm_gfx12.py
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from hw_oracle import asm  # noqa: E402


class HwOracleAsmGfx12Tests(unittest.TestCase):
    def test_raw_kernel_uses_s_load_b64_on_gfx11_and_gfx12(self) -> None:
        self.assertIn("s_load_dwordx2 s[2:3], s[0:1], 0x0", asm.raw_kernel_text("k0", ["s_endpgm"], "gfx1030"))
        for arch in ("gfx1100", "gfx1150", "gfx1200", "gfx1201"):
            with self.subTest(arch=arch):
                text = asm.raw_kernel_text("k0", ["s_endpgm"], arch)
                self.assertIn("s_load_b64 s[2:3], s[0:1], 0x0", text)
                self.assertNotIn("s_load_dwordx2", text)

    def test_module_text_emits_per_kernel_wavefront_size_metadata(self) -> None:
        progs = [
            asm.Kernel(["s_endpgm"], wave=64, raw=True),
            asm.Kernel(["s_endpgm"], wave=32),
        ]
        text, names, _starts = asm.module_text(progs, "gfx1201")
        self.assertEqual(names, ["k0", "k1"])
        self.assertIn(".name: k0", text)
        self.assertIn(".name: k1", text)
        self.assertIn("    .wavefront_size: 64\n", text)
        self.assertIn("    .wavefront_size: 32\n", text)
        self.assertEqual(text.count(".amdhsa_wavefront_size32 0"), 1)
        self.assertEqual(text.count(".amdhsa_wavefront_size32 1"), 1)

    def test_kernel_rejects_invalid_and_boolean_counts(self) -> None:
        bad_kwargs = (
            {"wave": 16},
            {"wave": True},
            {"threads": 0},
            {"threads": False},
            {"words": 0},
            {"lds": True},
            {"vgprs": False},
            {"sgprs": True},
        )
        for kw in bad_kwargs:
            with self.subTest(kw=kw), self.assertRaises(ValueError):
                asm.Kernel(["s_endpgm"], **kw)


if __name__ == "__main__":
    unittest.main()
