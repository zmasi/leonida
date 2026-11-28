//! Exact RDNA2 lowering for `v_cvt_flr_i32_f32` and `v_cvt_rpi_i32_f32`.
//!
//! Measured against RDNA2 hardware (`gfx1035` and `gfx1036`, `tools/cvt_diff.py`):
//! - `v_cvt_flr_i32_f32` computes `sat_i32(floor(S0))`.
//! - `v_cvt_rpi_i32_f32` computes `sat_i32(floor(S0 + 0.5))` with exact `S0 + 0.5`
//!   (no intermediate binary32 rounding of `0.5 - 2^-25` or odd integers in `[2^23, 2^24]`).
//! - Input f32 denormals (`exp == 0`) flush to `0` under the default shader mode (`-denorm -> 0`).
//! - Unlike `v_cvt_i32_f32` (which returns `0` for NaN), both instructions saturate
//!   `|S0| >= 2^31`, `±inf`, and `±NaN` by the post-modifier sign bit (`0 -> 0x7FFF_FFFF`,
//!   `1 -> 0x8000_0000`).

use super::bitcode::{Bin, Module, V, pred};

/// Emits pure-integer 32-bit IEEE-754 conversion of `bits` (with VOP3/SDWA/DPP
/// `abs` and `neg` modifiers already applied to bit 31) for `v_cvt_flr_i32_f32`
/// (`rpi == false`) or `v_cvt_rpi_i32_f32` (`rpi == true`).
pub(super) fn emit(m: &mut Module, c: usize, bits: V, rpi: bool) -> V {
    let k0 = m.i32(0);
    let k1 = m.i32(1);
    let k7 = m.i32(7);
    let k23 = m.i32(23);
    let k31 = m.i32(31);
    let k126 = m.i32(126);
    let k150 = m.i32(150);

    let sign_bit = m.bin(c, Bin::LShr, bits, k31);
    let is_neg = m.cmp(c, pred::NE, sign_bit, k0);
    let abs_mask = m.i32(0x7FFF_FFFF);
    let mag = m.bin(c, Bin::And, bits, abs_mask);

    let kff = m.i32(0xFF);
    let exp_sh = m.bin(c, Bin::LShr, mag, k23);
    let exp = m.bin(c, Bin::And, exp_sh, kff);
    let frac_mask = m.i32(0x007F_FFFF);
    let frac = m.bin(c, Bin::And, mag, frac_mask);
    let hidden = m.i32(0x0080_0000);
    let mant = m.bin(c, Bin::Or, frac, hidden);

    // |x| >= 2^31 (0x4F00_0000), +-inf, and +-NaN saturate by sign bit.
    let lim = m.i32(0x4F00_0000);
    let is_sat = m.cmp(c, pred::IUGE, mag, lim);
    let i32_max = m.i32(0x7FFF_FFFF);
    let i32_min = m.i32(0x8000_0000);
    let sat = m.select(c, is_neg, i32_min, i32_max);

    // Zero and denormals (exp == 0) evaluate to 0.
    let is_zero = m.cmp(c, pred::EQ, exp, k0);

    // Exact integers without fractional bits: 150 <= exp <= 157 (2^23 <= |x| < 2^31).
    let raw_sh_l = m.bin(c, Bin::Sub, exp, k150);
    let lt150 = m.cmp(c, pred::IULT, exp, k150);
    let sh_l = m.select(c, lt150, k0, raw_sh_l);
    let gt7 = m.cmp(c, pred::IUGT, sh_l, k7);
    let sh_l = m.select(c, gt7, k7, sh_l);
    let exact_mag = m.bin(c, Bin::Shl, mant, sh_l);
    let neg_exact = m.bin(c, Bin::Sub, k0, exact_mag);
    let res_exact = m.select(c, is_neg, neg_exact, exact_mag);

    // Mixed integer + fraction range: 127 <= exp <= 149 (1.0 <= |x| < 2^23, fb in 1..=23).
    let raw_fb = m.bin(c, Bin::Sub, k150, exp);
    let fb_lt1 = m.cmp(c, pred::IULT, raw_fb, k1);
    let fb = m.select(c, fb_lt1, k1, raw_fb);
    let fb_gt23 = m.cmp(c, pred::IUGT, fb, k23);
    let fb = m.select(c, fb_gt23, k23, fb);

    let int_mag = m.bin(c, Bin::LShr, mant, fb);
    let neg_int = m.bin(c, Bin::Sub, k0, int_mag);
    let unit = m.bin(c, Bin::Shl, k1, fb);
    let rem_mask = m.bin(c, Bin::Sub, unit, k1);
    let rem = m.bin(c, Bin::And, mant, rem_mask);

    let neg_one = m.i32(0xFFFF_FFFF);
    let (res_mid, res_sub) = if rpi {
        let fb_m1 = m.bin(c, Bin::Sub, fb, k1);
        let half = m.bin(c, Bin::Shl, k1, fb_m1);
        let pos_ge_half = m.cmp(c, pred::IUGE, rem, half);
        let pos_inc = m.select(c, pos_ge_half, k1, k0);
        let pos_rpi = m.bin(c, Bin::Add, int_mag, pos_inc);

        let neg_gt_half = m.cmp(c, pred::IUGT, rem, half);
        let neg_dec = m.select(c, neg_gt_half, k1, k0);
        let neg_rpi = m.bin(c, Bin::Sub, neg_int, neg_dec);
        let mid = m.select(c, is_neg, neg_rpi, pos_rpi);

        // Sub-unit normals: 1 <= exp <= 126 (2^-126 <= |x| < 1.0).
        // +0.5 is mag == 0x3F00_0000: floor(+0.5 + 0.5) = 1, while floor(-0.5 + 0.5) = 0.
        let half_bits = m.i32(0x3F00_0000);
        let ge_half = m.cmp(c, pred::IUGE, mag, half_bits);
        let gt_half = m.cmp(c, pred::IUGT, mag, half_bits);
        let pos_sub = m.select(c, ge_half, k1, k0);
        let neg_sub = m.select(c, gt_half, neg_one, k0);
        let sub = m.select(c, is_neg, neg_sub, pos_sub);
        (mid, sub)
    } else {
        let rem_nz = m.cmp(c, pred::NE, rem, k0);
        let neg_dec = m.select(c, rem_nz, k1, k0);
        let neg_flr = m.bin(c, Bin::Sub, neg_int, neg_dec);
        let mid = m.select(c, is_neg, neg_flr, int_mag);
        let sub = m.select(c, is_neg, neg_one, k0);
        (mid, sub)
    };

    let is_sub = m.cmp(c, pred::IULE, exp, k126);
    let is_exact = m.cmp(c, pred::IUGE, exp, k150);
    let r = m.select(c, is_sub, res_sub, res_mid);
    let r = m.select(c, is_exact, res_exact, r);
    let r = m.select(c, is_zero, k0, r);
    m.select(c, is_sat, sat, r)
}
