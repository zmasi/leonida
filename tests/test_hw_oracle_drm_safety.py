"""Unit tests for tools/hw_oracle/drm.py ELF64 bounds validation, subprocess timeouts, and temp cleanup.

    python -I tests/test_hw_oracle_drm_safety.py
"""
from __future__ import annotations

import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from hw_oracle import drm, runner  # noqa: E402


def _make_elf64(
    sections: list[tuple[int, int, int, int, int, int, int]],
    symtab_bytes: bytes = b"",
    strtab_bytes: bytes = b"\0.text\0.symtab\0.strtab\0.shstrtab\0",
    payload: bytes = b"\0" * 128,
    shstrndx: int = 3,
    shentsize: int = 64,
    shoff: int | None = None,
) -> bytes:
    """Build a minimal little-endian ELF64 blob for malformed-bounds tests."""
    hdr = bytearray(64)
    hdr[0:6] = b"\x7fELF\x02\x01"
    data = bytearray(payload) + bytearray(symtab_bytes) + bytearray(strtab_bytes)
    actual_shoff = len(hdr) + len(data) if shoff is None else shoff
    sh_bytes = bytearray()
    for name_off, sh_type, addr, off, size, link, info in sections:
        sh_bytes += struct.pack("<IIQQQQIIQQ", name_off, sh_type, 0, addr, off, size, link, info, 8, 0)
    struct.pack_into("<Q", hdr, 0x28, actual_shoff)
    struct.pack_into("<HHH", hdr, 0x3A, shentsize, len(sections), shstrndx)
    return bytes(hdr + data + sh_bytes)


class HwOracleDrmSafetyTests(unittest.TestCase):
    def test_find_node_rejects_blank_value(self) -> None:
        with self.assertRaises(drm.Unavailable):
            drm.find_node({"LEONIDA_HW_DRM_NODE": "   "})

    def test_elf64_bounds_reject_malformed_headers_and_descriptors(self) -> None:
        with self.assertRaises(ValueError):
            drm.text_section(b"\x7fELF\x02\x01short")

        # shentsize < 64 / section table past EOF / out-of-bounds shstrndx
        bad_shent = _make_elf64([(1, 3, 0, 64, 32, 0, 0)], shentsize=32, shstrndx=0)
        with self.assertRaises(ValueError):
            drm._symbols(bad_shent)

        bad_shstrndx = _make_elf64([(1, 3, 0, 64, 32, 0, 0)], shstrndx=5)
        with self.assertRaises(ValueError):
            drm._symbols(bad_shstrndx)

        # Valid string/symtab sections, truncated .text section past EOF
        shstr = b"\0.text\0.symtab\0.strtab\0.shstrtab\0"
        base_str_off = 64
        sections = [
            (0, 0, 0, 0, 0, 0, 0),
            (shstr.index(b".text"), 1, 0x100, 64 + len(shstr), 99999, 0, 0),
            (shstr.index(b".strtab"), 3, 0, base_str_off, len(shstr), 0, 0),
            (shstr.index(b".shstrtab"), 3, 0, base_str_off, len(shstr), 0, 0),
        ]
        bad_text = _make_elf64(sections, payload=shstr, shstrndx=3)
        with self.assertRaises(ValueError):
            drm.text_section(bad_text)

        # Symtab entry whose value < section address (underflow guard)
        kd_str = b"\0k0.kd\0.text\0.symtab\0.strtab\0.shstrtab\0"
        str_off = 64
        text_off = 64 + len(kd_str)
        sym_off = text_off + 128
        sym_entry = struct.pack("<IBBHQQ", kd_str.index(b"k0.kd"), 0, 0, 1, 0x10, 64)
        kd_sections = [
            (0, 0, 0, 0, 0, 0, 0),
            (kd_str.index(b".text"), 1, 0x200, text_off, 128, 0, 0),
            (kd_str.index(b".symtab"), 2, 0, sym_off, len(sym_entry), 3, 0),
            (kd_str.index(b".strtab"), 3, 0, str_off, len(kd_str), 0, 0),
            (kd_str.index(b".shstrtab"), 3, 0, str_off, len(kd_str), 0, 0),
        ]
        underflow_elf = _make_elf64(
            kd_sections,
            symtab_bytes=sym_entry,
            strtab_bytes=b"",
            payload=kd_str + b"\0" * 128,
            shstrndx=4,
        )
        with self.assertRaises(ValueError):
            drm.kernel_registers(underflow_elf)

    def test_subprocess_timeouts_and_temp_binary_cleanup(self) -> None:
        o = object.__new__(drm.DrmOracle)
        with tempfile.TemporaryDirectory() as tmp, \
                mock.patch.object(drm.tempfile, "gettempdir", return_value=tmp), \
                mock.patch.object(drm.runner, "find_cc", return_value=("gcc", "gcc")), \
                mock.patch.object(drm.shutil, "which", return_value=None), \
                mock.patch.object(
                    drm.subprocess,
                    "run",
                    side_effect=subprocess.TimeoutExpired(["gcc"], drm.SUBPROCESS_TIMEOUT),
                ):
            with self.assertRaises(drm.Unavailable):
                o._build_launcher()
            leftovers = [p for p in Path(tmp).iterdir() if p.name.startswith(".leonida_hw_oracle_drm_")]
            self.assertEqual(leftovers, [])

        o.exe, o.node, o.clang, o.arch, o.workdir = "launcher", "/dev/dri/renderD129", "clang", "gfx1030", "."
        o._offsets, o._registers = {"k0": 0}, {"k0": (0, 0, 32)}
        with mock.patch.object(
            drm.subprocess, "run", side_effect=subprocess.TimeoutExpired(["launcher"], drm.SUBPROCESS_TIMEOUT)
        ):
            with self.assertRaises(drm.Unavailable):
                o._device_arch()
            rc, out, err = o._launch("k.bin", ["k0"])
            self.assertEqual((rc, out), (-1, ""))
            self.assertIn("timed out", err)

        with tempfile.TemporaryDirectory() as td:
            o.workdir = td
            with mock.patch.object(
                drm.subprocess, "run", side_effect=subprocess.TimeoutExpired(["clang"], drm.SUBPROCESS_TIMEOUT)
            ):
                code, msg = o._assemble("s_endpgm")
            self.assertIsNone(code)
            self.assertIn("timed out", msg)


if __name__ == "__main__":
    unittest.main()
