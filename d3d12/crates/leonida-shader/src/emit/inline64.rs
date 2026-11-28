//! GFX10 inline constants in 64-bit operand positions.
//!
//! An inline constant fills the whole operand. The integer codes (128..=208)
//! sign-extend to 64 bits. The float codes (240..=248) stand for their f64
//! bit patterns in any 64-bit operand, integer and bitwise instructions
//! included (`s_mov_b64`, `s_and_b64`, `s_cmp_eq_u64`, `v_lshlrev_b64`'s
//! value, `v_cmp_*_u64`...), not for the f32 bits in the low dword: LLVM's
//! AMDGPU disassembler decodes them with `getInlineImmVal64` for
//! `OPERAND_REG_IMM_INT64` and `OPERAND_REG_INLINE_C_INT64` operands as for
//! the f64 ones. 1/(2*pi) is the hardware's `0x3FC45F306DC9C882`
//! (`isInlinableLiteral64`), one unit in the last place below the rounded
//! quotient `1.0 / TAU`. 32-bit operands of the same instructions (shift
//! amounts, `s_bfe_*64` controls) keep the 32-bit values.

/// The 64-bit value of inline constant `code` in a 64-bit operand; `None`
/// for codes that are no inline constant (registers, literals, specials).
pub(crate) const fn value(code: u8) -> Option<u64> {
    Some(match code {
        128..=192 => (code - 128) as u64,
        193..=208 => (192 - code as i64) as u64,
        240 => 0x3FE0_0000_0000_0000,  // 0.5
        241 => 0xBFE0_0000_0000_0000,  // -0.5
        242 => 0x3FF0_0000_0000_0000,  // 1.0
        243 => 0xBFF0_0000_0000_0000,  // -1.0
        244 => 0x4000_0000_0000_0000,  // 2.0
        245 => 0xC000_0000_0000_0000,  // -2.0
        246 => 0x4010_0000_0000_0000,  // 4.0
        247 => 0xC010_0000_0000_0000,  // -4.0
        248 => 0x3FC4_5F30_6DC9_C882,  // 1/(2*pi)
        _ => return None,
    })
}

/// Dword `half` (0 low, 1 high) of [`value`].
pub(crate) const fn dword(code: u8, half: u8) -> Option<u32> {
    match value(code) {
        Some(x) => Some((x >> (32 * (half as u32 & 1))) as u32),
        None => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn integers_sign_extend_and_floats_are_f64_bits() {
        assert_eq!(value(128), Some(0));
        assert_eq!(value(192), Some(64));
        assert_eq!(value(193), Some(u64::MAX));
        assert_eq!(value(208), Some((-16i64) as u64));
        for (code, f) in [(240, 0.5), (241, -0.5), (242, 1.0), (243, -1.0)] {
            assert_eq!(value(code), Some(f64::to_bits(f)));
        }
        for (code, f) in [(244, 2.0), (245, -2.0), (246, 4.0), (247, -4.0)] {
            assert_eq!(value(code), Some(f64::to_bits(f)));
        }
        // The hardware's 1/(2*pi) is one ulp below the rounded quotient.
        assert_eq!(value(248), Some((1.0 / std::f64::consts::TAU).to_bits() - 1));
        assert_eq!(dword(248, 0), Some(0x6DC9_C882));
        assert_eq!(dword(248, 1), Some(0x3FC4_5F30));
        for code in [0, 106, 127, 209, 239, 249, 253, 255] {
            assert_eq!(value(code), None, "{code}");
        }
    }

    #[test]
    fn the_low_dword_of_an_integer_matches_the_32_bit_inline_value() {
        for code in 128..=208u8 {
            assert_eq!(dword(code, 0), crate::resource::inline(code), "{code}");
        }
    }
}
