"""buffer_load_format_d16_hi_x and buffer_store_format_d16_hi_x against their d16_x forms, on the CPU.

RDNA2 ISA (MUBUF): buffer_load_format_d16_hi_x decodes one component as buffer_load_format_d16_x does (to f16 for a
float format, to a saturated 16-bit integer for an integer one) but writes it to the high half of vdata, keeping the
low half; buffer_store_format_d16_hi_x stores the component from the high half of vdata where d16_x stores the low
half. For every format the translator decodes (Gen.FORMATS) each _hi form must give exactly what the d16_x form gives,
moved to / taken from the other half. Run through spirv_mini_interp; standard library only; no game files.
"""
import os
import struct
import sys
import tempfile
import unittest
import warnings

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.dont_write_bytecode = True
sys.path.insert(0, os.environ.get("LEONIDA_TOOLS_DIR") or os.path.join(ROOT, "tools"))
sys.path.insert(0, HERE)

import rdna2spirv  # noqa: E402
import spirv_mini_interp as si  # noqa: E402

STUB_DXBC = b"DXBC" + bytes(24) + struct.pack("<I", 0) + bytes(4)
FSSM = bytearray(0x148)
struct.pack_into("<4I", FSSM, 0x138, 0x88, 32, 1, 1)
FSSM = bytes(FSSM)
SRC, OUT = 0x100, 0x200
ELEMENT = [0xC0A08040, 0x3F800000, 0x12345678, 0x9ABCDEF0]     # bytes every format decodes from
KEEP = 0xBEEF1234


def translate(body):
    with tempfile.TemporaryDirectory() as tmp:
        for d in ("disasm", "fssm"):
            os.mkdir(os.path.join(tmp, d))
        with open(os.path.join(tmp, "cs_d16hi.dxbc"), "wb") as f:
            f.write(STUB_DXBC)
        with open(os.path.join(tmp, "fssm", "cs_d16hi.fssm"), "wb") as f:
            f.write(FSSM)
        src = os.path.join(tmp, "disasm", "cs_d16hi.s")
        with open(src, "w", encoding="utf-8") as f:
            f.write("; cs_d16hi\n" + "".join(f"\t{ln}\n" for ln in body) + "\ts_endpgm\n")
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", ResourceWarning)
            return rdna2spirv.translate(src)


def run(body, word3):
    vsharp = ["s_mov_b32 s8, 0", "s_mov_b32 s9, 0", "s_mov_b32 s10, 0x1000", f"s_mov_b32 s11, {word3:#x}"]
    mem = {SRC + 4 * k: w for k, w in enumerate(ELEMENT)}
    mem.update({OUT + 4 * k: 0xEEEEEEEE for k in range(4)})
    si.run_workgroup(translate(vsharp + body), 1, 32,
                     inputs_for=lambda i: {"%wg_id": (0, 0, 0), "%local_id": (i, 0, 0), "%sg_lane": i},
                     push_constants=[0, 0, 0x1000, 0x31016FAC] + [0] * 28, memory=mem)
    return mem


def word3(code):
    return (3 << 28) | (code << 12) | 0xFAC              # raw bounds, DST format `code`, selectors x y z w


def load(op, code):
    mem = run([f"v_mov_b32_e32 v1, {SRC:#x}", f"v_mov_b32_e32 v5, {KEEP:#x}", f"{op} v5, v1, s[8:11], 0 offen",
               "s_waitcnt vmcnt(0)", f"v_mov_b32_e32 v6, {OUT:#x}", "buffer_store_dword v5, v6, s[0:3], 0 offen"],
              word3(code))
    return mem[OUT]


def store(op, code, value):
    mem = run([f"v_mov_b32_e32 v1, {OUT:#x}", f"v_mov_b32_e32 v5, {value:#x}", f"{op} v5, v1, s[8:11], 0 offen"],
              word3(code))
    return [mem[OUT + 4 * k] for k in range(4)]


class BufferFormatD16Hi(unittest.TestCase):
    def test_load_writes_the_high_half(self):
        for code in sorted(rdna2spirv.Gen.FORMATS):
            with self.subTest(fmt=code):
                lo = load("buffer_load_format_d16_x", code)
                hi = load("buffer_load_format_d16_hi_x", code)
                self.assertEqual(lo >> 16, KEEP >> 16)                 # d16_x keeps the high half
                self.assertEqual(hi, (lo & 0xFFFF) << 16 | (KEEP & 0xFFFF))

    def test_store_takes_the_high_half(self):
        for code in sorted(rdna2spirv.Gen.FORMATS):
            for v in (0x3C00, 0xB800, 0x7BFF, 0x0001, 0x8000):
                with self.subTest(fmt=code, v=hex(v)):
                    want = store("buffer_store_format_d16_x", code, 0xABCD0000 | v)
                    self.assertEqual(store("buffer_store_format_d16_hi_x", code, v << 16 | 0xABCD), want)

    def test_first_read_analysis_counts_the_kept_half(self):
        from shader_ir import parse_line
        self.assertEqual(rdna2spirv.read_before_write_vgprs(
            [parse_line("\tbuffer_load_format_d16_hi_x v5, v1, s[8:11], 0 offen")]), {1, 5})


if __name__ == "__main__":
    unittest.main()
