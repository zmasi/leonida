"""v_sat_pk_u8_i16: each signed i16 lane of SRC0 saturates into a u8, packed into D[15:0].

Semantics from the AMD machine-readable RDNA2 ISA specification: D.b16 = {SAT8(S0[31:16].i16),
SAT8(S0[15:0].i16)} with SAT8(n) = 0 for n <= 0, 255 for n >= 255 and n[7:0] otherwise. On GFX10 the
16-bit result keeps the destination's high half, the same partial write as v_cvt_f16_f32 (Mesa ACO
instr_is_16bit); under _sdwa the dst_sel/dst_unused rule decides instead. Hand-written pixel-shader
listings are translated by tools/rdna2spirv.py and run by spirv_mini_interp.py; expected words are
hand-computed and checked against a plain-Python reference.
"""

from pathlib import Path
import unittest

from support import HERE, mrt0_words, translate_lines, translate_listing  # noqa: F401
import rdna2spirv  # noqa: E402  (sys.path set by support above)


def ref_sat_pk_u8_i16(s0):
    def sat8(x):
        if x & 0x8000:
            x -= 0x10000
        return 0 if x <= 0 else (255 if x >= 255 else x)

    return sat8(s0 & 0xFFFF) | (sat8((s0 >> 16) & 0xFFFF) << 8)


class SatPkU8I16(unittest.TestCase):
    def test_fixture(self):
        fixture = Path(HERE) / "fixtures" / "sat_pk_u8_i16.asm"
        text = translate_listing("ps_sat_pk_u8_i16", fixture.read_bytes())
        self.assertEqual(mrt0_words(text), [0xCAFEFF00, 0xDEAD00FF, 0x00008000, 0xABCDFFFF])
        self.assertEqual(text.count("OpExtInst %int %glsl SClamp"), 8)

    def test_saturates_each_signed_lane_to_u8_and_keeps_high_half(self):
        def run(s0, seed):
            lines = [
                f"v_mov_b32 v1, 0x{s0:08x}",
                f"v_mov_b32 v2, 0x{seed:08x}",
                "v_sat_pk_u8_i16 v2, v1",
                "exp mrt0 v2, v2, v2, v2 done vm",
                "s_endpgm",
            ]
            return mrt0_words(translate_lines("ps_sat_pk", lines))[0]

        for s0 in (0x00000000, 0x00FF0080, 0x0100FE00, 0x80007FFF,
                   0x7FFF8000, 0x00FFFEFE, 0x12345678):
            for seed in (0x00000000, 0xDEADBEEF):
                self.assertEqual(run(s0, seed), (seed & 0xFFFF0000) | ref_sat_pk_u8_i16(s0),
                                 f"s0={s0:#010x} seed={seed:#010x}")

    def test_sgpr_and_literal_sources(self):
        text = translate_lines("ps_sat_pk_sgpr", [
            "s_mov_b32 s0, 0x8040ff00",
            "s_mov_b32 s8, m0",
            "v_mov_b32 v1, 0x11111111",
            "v_mov_b32 v2, 0x22222222",
            "v_sat_pk_u8_i16 v1, s0",
            "v_sat_pk_u8_i16 v2, 0x00ff0100",
            "v_sat_pk_u8_i16 v3, -1",
            "v_sat_pk_u8_i16 v4, 5",
            "exp mrt0 v1, v2, v3, v4 done vm",
            "s_endpgm",
        ])
        self.assertEqual(mrt0_words(text), [0x11110000, 0x2222FFFF, 0x00000000, 0x00000005])

    def test_sdwa_source_and_destination(self):
        text = translate_lines("ps_sat_pk_sdwa", [
            "v_mov_b32 v1, 0x1234fed4",
            "v_mov_b32 v2, 0x0008ff40",
            "v_mov_b32 v3, 0x12345678",
            "v_sat_pk_u8_i16_sdwa v4, v1 dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:WORD_1",
            "v_sat_pk_u8_i16_sdwa v3, v2 dst_sel:WORD_1 dst_unused:UNUSED_PRESERVE src0_sel:DWORD",
            "v_sat_pk_u8_i16_sdwa v5, v1 dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0",
            "exp mrt0 v4, v3, v5, v5 done vm",
            "s_endpgm",
        ])
        # WORD_1 zero-extends 0x1234: {sat(0x1234)=255, sat(0)=0} into a full DWORD write.
        # WORD_1 dst keeps v3's low half: {sat(8)=8, sat(-192)=0} -> 0x0800 into bits 31:16.
        # BYTE_0 zero-extends 0xd4: {sat(212)=212, sat(0)=0} into a full DWORD write.
        self.assertEqual(mrt0_words(text), [0x000000FF, 0x08005678, 0x000000D4, 0x000000D4])

    def test_modifiers_are_refused_not_guessed(self):
        for line in ("v_sat_pk_u8_i16_e64 v2, -v1",
                     "v_sat_pk_u8_i16_e64 v2, |v1|",
                     "v_sat_pk_u8_i16_e64 v2, v1 clamp",
                     "v_sat_pk_u8_i16_e64 v2, v1 mul:2",
                     "v_sat_pk_u8_i16_sdwa v2, v1 dst_sel:DWORD dst_unused:UNUSED_PAD "
                     "src0_sel:WORD_1 clamp"):
            with self.assertRaises(rdna2spirv.Unsupported, msg=line):
                translate_lines("ps_sat_pk_bad", [line, "exp mrt0 v2, v2, v2, v2 done vm", "s_endpgm"])


if __name__ == "__main__":
    unittest.main()
