//! Additional 16-bit integer and half-float VALU instructions (`v_add_nc_i16`,
//! `v_sub_nc_i16`, `v_mad_u16`, `v_mad_i16`, `v_fmamk_f16`, `v_fmaak_f16`,
//! `v_frexp_mant_f16`, `v_frexp_exp_i16_f16`, and `v_ldexp_f16`), kept in a
//! child module of `lower.rs` so they share [`Lower`] state cleanly.

use super::{Attr, Bin, Fields, Inst, Lower, V, Why, f16_nearest_on, pred};
use crate::gfx10::Encoding;

impl<'a> Lower<'a> {
    /// Extracts source `k`'s selected 16-bit half (`op_sel[k]`) in bits `[15:0]`
    /// with bits `[31:16]` zeroed. `f16_const` selects `f16` inline float
    /// constants (`0x3800`, `0x3C00`, ...) vs raw 32-bit bus values on integer
    /// operands (`docs/design.md`).
    fn ext_half_src(&mut self, inst: &Inst, k: usize, f16_const: bool) -> Result<V, Why> {
        let Fields::Valu(v) = &inst.fields else {
            unreachable!()
        };
        let op = *v.src.get(k).ok_or("a missing operand")?;
        let x = if f16_const {
            self.f16_operand(inst, &op)?
        } else {
            self.operand(inst, &op)?
        };
        let c = self.cur;
        let x = if v.opsel >> k & 1 != 0 {
            let k16 = self.s.m.i32(16);
            self.s.m.bin(c, Bin::LShr, x, k16)
        } else {
            x
        };
        let mask = self.s.m.i32(0xFFFF);
        Ok(self.s.m.bin(c, Bin::And, x, mask))
    }

    fn sext16(&mut self, x: V) -> V {
        let c = self.cur;
        let k16 = self.s.m.i32(16);
        let sh = self.s.m.bin(c, Bin::Shl, x, k16);
        self.s.m.bin(c, Bin::AShr, sh, k16)
    }

    /// Saturates a 32-bit signed intermediate `x` (`|x| <= 0x4000_7FFF`) to
    /// `[-32768, 32767]` in bits `[15:0]` with bits `[31:16] == 0`.
    ///
    /// Computed in `[0, 0xFFFF]` biased space (`clamp(x + 0x8000, 0, 0xFFFF) ^ 0x8000`)
    /// because NVIDIA's D3D12 driver JIT folds signed `clamp(x, -32768, 32767) & 0xFFFF`
    /// to a sign-extending 16-bit saturating op and drops `& 0xFFFF`, leaking `0xFFFF`
    /// into the preserved upper half of `vdst` whenever bit 15 is set.
    fn clamp_s16(&mut self, x: V) -> V {
        let c = self.cur;
        let zero = self.s.m.i32(0);
        let bias = self.s.m.i32(0x8000);
        let max_u16 = self.s.m.i32(0xFFFF);
        let u = self.s.m.bin(c, Bin::Add, x, bias);
        let neg = self.s.m.cmp(c, pred::SLT, u, zero);
        let u = self.s.m.select(c, neg, zero, u);
        let ov = self.s.m.cmp(c, pred::IUGT, u, max_u16);
        let u = self.s.m.select(c, ov, max_u16, u);
        self.s.m.bin(c, Bin::Xor, u, bias)
    }

    pub(super) fn valu16_ext(
        &mut self,
        inst: &Inst,
        name: &str,
        guard: Option<V>,
    ) -> Option<Result<(), Why>> {
        match name {
            "v_add_nc_i16" | "v_sub_nc_i16" => Some(self.vop3_add_sub_i16(inst, name, guard)),
            "v_mad_u16" | "v_mad_i16" => Some(self.vop3_mad_int16(inst, name == "v_mad_i16", guard)),
            "v_fmamk_f16" | "v_fmaak_f16" => Some(self.vop2_fmamk_fmaak_f16(inst, guard)),
            "v_frexp_mant_f16" | "v_frexp_exp_i16_f16" | "v_ldexp_f16" => {
                Some(self.frexp_ldexp_f16(inst, name, guard))
            }
            _ => None,
        }
    }

    /// `v_add_nc_i16` / `v_sub_nc_i16` (VOP3-only): signed 16-bit add/sub on
    /// `op_sel`-selected halves; wraps to 16 bits without `clamp`, saturates to
    /// `[-32768, 32767]` with `clamp`, and preserves the unwritten destination half.
    fn vop3_add_sub_i16(
        &mut self,
        inst: &Inst,
        name: &str,
        guard: Option<V>,
    ) -> Result<(), Why> {
        let Fields::Valu(v) = &inst.fields else {
            unreachable!()
        };
        if inst.machine.encoding != Encoding::Vop3
            || v.sdwa.is_some()
            || v.dpp.is_some()
            || v.omod != 0
            || v.abs != 0
            || v.neg != 0
        {
            return Err("modifiers on a 16-bit integer operation");
        }
        let a = self.ext_half_src(inst, 0, false)?;
        let b = self.ext_half_src(inst, 1, false)?;
        let (a, b) = (self.sext16(a), self.sext16(b));
        let op = if name == "v_add_nc_i16" {
            Bin::Add
        } else {
            Bin::Sub
        };
        let mut r = self.s.m.bin(self.cur, op, a, b);
        if v.clamp {
            r = self.clamp_s16(r);
        }
        self.write_half(inst, r, guard)
    }

    /// `v_mad_u16` / `v_mad_i16` (VOP3-only, `docs/design.md` *16-bit integer
    /// ops*): `S0 * S1 + S2` evaluated in 32 bits (cannot overflow `u32` or
    /// `i32`); with `clamp`, saturates to `[0, 0xFFFF]` or `[-32768, 32767]`.
    fn vop3_mad_int16(
        &mut self,
        inst: &Inst,
        signed: bool,
        guard: Option<V>,
    ) -> Result<(), Why> {
        let Fields::Valu(v) = &inst.fields else {
            unreachable!()
        };
        if inst.machine.encoding != Encoding::Vop3
            || v.sdwa.is_some()
            || v.dpp.is_some()
            || v.omod != 0
            || v.abs != 0
            || v.neg != 0
        {
            return Err("modifiers on a 16-bit integer operation");
        }
        let mut xs = [self.s.m.i32(0); 3];
        for (k, slot) in xs.iter_mut().enumerate() {
            let h = self.ext_half_src(inst, k, false)?;
            *slot = if signed { self.sext16(h) } else { h };
        }
        let c = self.cur;
        let prod = self.s.m.bin(c, Bin::Mul, xs[0], xs[1]);
        let mut tot = self.s.m.bin(c, Bin::Add, prod, xs[2]);
        if v.clamp {
            tot = if signed {
                self.clamp_s16(tot)
            } else {
                let max_u16 = self.s.m.i32(0xFFFF);
                let ov = self.s.m.cmp(c, pred::IUGT, tot, max_u16);
                self.s.m.select(c, ov, max_u16, tot)
            };
        }
        self.write_half(inst, tot, guard)
    }

    /// `v_fmamk_f16` (`D = S0 * K + S1`) and `v_fmaak_f16` (`D = S0 * S1 + K`),
    /// VOP2-only with a mandatory 32-bit literal `K` whose low 16 bits hold the
    /// `f16` constant (`gfx10/fields.rs` orders `v.src` so `src[0] * src[1] + src[2]`
    /// holds for both). Writes the low 16 bits and keeps the high half (GFX10).
    fn vop2_fmamk_fmaak_f16(&mut self, inst: &Inst, guard: Option<V>) -> Result<(), Why> {
        let Fields::Valu(v) = &inst.fields else {
            unreachable!()
        };
        if inst.machine.encoding != Encoding::Vop2
            || v.sdwa.is_some()
            || v.dpp.is_some()
            || v.src.len() != 3
            || v.abs != 0
            || v.neg != 0
            || v.clamp
            || v.omod != 0
            || v.opsel != 0
        {
            return Err("an invalid v_fmamk_f16 or v_fmaak_f16 form");
        }
        let f32t = self.s.t.f32;
        let i32t = self.s.t.i32;
        let mut fs = [self.s.m.f32(0); 3];
        for (k, slot) in fs.iter_mut().enumerate() {
            let h = self.ext_half_src(inst, k, true)?;
            *slot = self
                .s
                .op(self.cur, "legacyF16ToF32", Attr::ReadNone, f32t, 131, &[h]);
        }
        // shortcut: fused f16 multiply-adds as FMad in f32, matching `v_fma_f16`.
        let f = self.fmad(fs[0], fs[1], fs[2]);
        let h = if f16_nearest_on() {
            self.f16_nearest(f)
        } else {
            self.s
                .op(self.cur, "legacyF32ToF16", Attr::ReadNone, i32t, 130, &[f])
        };
        self.write_half(inst, h, guard)
    }

    /// `v_frexp_mant_f16`, `v_frexp_exp_i16_f16`, and `v_ldexp_f16` (VOP1/VOP2
    /// and VOP3), lowered bit-exactly on the 16-bit word in integers matching
    /// RDNA2 (`gfx1036`) hardware measurements (`docs/design.md`,
    /// `tools/frexp_ldexp_f16_mods_probe.py`).
    // shortcut: models console `MODE` (`denorm_mode_16_64 = 3`, `IEEE = 0`,
    // `DX10_CLAMP = 1`): `f16` subnormals are normalized/rounded to nearest even,
    // `omod` is ignored on `v_frexp_mant_f16`/`v_ldexp_f16` while `[0, 1]` `clamp`
    // applies, and both `clamp` and `omod` are no-ops on `v_frexp_exp_i16_f16`.
    fn frexp_ldexp_f16(
        &mut self,
        inst: &Inst,
        name: &str,
        guard: Option<V>,
    ) -> Result<(), Why> {
        let Fields::Valu(v) = &inst.fields else {
            unreachable!()
        };
        if v.sdwa.is_some() || v.dpp.is_some() {
            return Err("SDWA or DPP on a 16-bit frexp or ldexp operation");
        }
        if name == "v_ldexp_f16" && (v.abs >> 1 & 1 != 0 || v.neg >> 1 & 1 != 0) {
            return Err("integer operand modifiers");
        }
        let mut x = self.ext_half_src(inst, 0, true)?;
        let c = self.cur;
        let k = |l: &mut Self, n: u32| l.s.m.i32(n);
        if v.abs & 1 != 0 {
            let m = k(self, 0x7FFF);
            x = self.s.m.bin(c, Bin::And, x, m);
        }
        if v.neg & 1 != 0 {
            let m = k(self, 0x8000);
            x = self.s.m.bin(c, Bin::Xor, x, m);
        }

        let (k0, k1, k10, k31) = (k(self, 0), k(self, 1), k(self, 10), k(self, 0x1F));
        let e_sh = self.s.m.bin(c, Bin::LShr, x, k10);
        let e = self.s.m.bin(c, Bin::And, e_sh, k31);
        let em1 = self.s.m.bin(c, Bin::Sub, e, k1);
        let k30 = k(self, 30);
        let isnorm = self.s.m.cmp(c, pred::IULT, em1, k30); // exponent field 1..=30
        let isden = self.s.m.cmp(c, pred::EQ, e, k0);
        let sign_mask = k(self, 0x8000);
        let sgn = self.s.m.bin(c, Bin::And, x, sign_mask);
        let other = self.s.m.select(c, isden, sgn, x); // +-0 -> +-0, +-inf/NaN -> x

        let frac_mask = k(self, 0x3FF);
        let frac = self.s.m.bin(c, Bin::And, x, frac_mask);
        let nz = self.s.m.cmp(c, pred::NE, frac, k0);
        let sub = self.s.m.bin(c, Bin::And, isden, nz);
        let fin = self.s.m.bin(c, Bin::Or, isnorm, sub);

        let lead = k(self, 0x400);
        let wide = self.s.m.bin(c, Bin::Or, frac, lead);
        let sig = self.s.m.select(c, isnorm, wide, frac);
        // DXIL `FirstbitHi` (`unaryBits.i32` op 33): leading zeros from bit 31
        // (`21` when bit 10 is the top set bit, `22..=31` for subnormals, `-1` for 0).
        let clz = self
            .s
            .op(c, "unaryBits.i32", Attr::ReadNone, self.s.t.i32, 33, &[sig]);
        let k21 = k(self, 21);
        let lsh = self.s.m.bin(c, Bin::Sub, clz, k21);
        let lt10 = self.s.m.cmp(c, pred::IULT, lsh, k10);
        let lshc = self.s.m.select(c, lt10, lsh, k10);
        let sig11 = self.s.m.bin(c, Bin::Shl, sig, lshc);
        let ef0 = self.s.m.select(c, isnorm, e, k1);
        let ef = self.s.m.bin(c, Bin::Sub, ef0, lshc);

        let r = match name {
            "v_frexp_exp_i16_f16" => {
                let k14 = k(self, 14);
                let ex = self.s.m.bin(c, Bin::Sub, ef, k14);
                self.s.m.select(c, fin, ex, k0)
            }
            "v_frexp_mant_f16" => {
                let lo10 = self.s.m.bin(c, Bin::And, sig11, frac_mask);
                let keep = self.s.m.bin(c, Bin::Or, sgn, lo10);
                let bias14 = k(self, 0x3800);
                let m = self.s.m.bin(c, Bin::Or, keep, bias14);
                self.s.m.select(c, fin, m, other)
            }
            _ => {
                // `v_ldexp_f16`: S1 signed 16-bit integer clamped to [-64, 64].
                let n_raw = self.ext_half_src(inst, 1, false)?;
                let nu = self.sext16(n_raw);
                let k64 = k(self, 64);
                let km64 = k(self, (-64i32) as u32);
                let k129 = k(self, 129);
                let nb = self.s.m.bin(c, Bin::Add, nu, k64);
                let small = self.s.m.cmp(c, pred::IULT, nb, k129);
                let n_neg = self.s.m.cmp(c, pred::SLT, nu, k0);
                let lim = self.s.m.select(c, n_neg, km64, k64);
                let nc = self.s.m.select(c, small, nu, lim);

                let e2 = self.s.m.bin(c, Bin::Add, ef, nc);
                let e2m1 = self.s.m.bin(c, Bin::Sub, e2, k1);
                let valid = self.s.m.cmp(c, pred::IULT, e2m1, k30); // 1..=30
                let over = self.s.m.cmp(c, pred::SGT, e2, k0); // > 0 (so >= 31 when !valid)

                let sh = self.s.m.bin(c, Bin::Shl, e2, k10);
                let lo10 = self.s.m.bin(c, Bin::And, sig11, frac_mask);
                let keep = self.s.m.bin(c, Bin::Or, sgn, lo10);
                let norm = self.s.m.bin(c, Bin::Or, keep, sh);
                let k_inf = k(self, 0x7C00);
                let inf = self.s.m.bin(c, Bin::Or, sgn, k_inf);

                // Subnormal / underflow (`e2 <= 0`): shift `sig11` right by
                // `clamp(1 - e2, 1, 12)` with round-to-nearest-even.
                let rs = self.s.m.bin(c, Bin::Sub, k1, e2);
                let k12 = k(self, 12);
                let lt1 = self.s.m.cmp(c, pred::SLT, rs, k1);
                let rsc = self.s.m.select(c, lt1, k1, rs);
                let gt12 = self.s.m.cmp(c, pred::SGT, rsc, k12);
                let rsu = self.s.m.select(c, gt12, k12, rsc);

                let q = self.s.m.bin(c, Bin::LShr, sig11, rsu);
                let hm = self.s.m.bin(c, Bin::Shl, k1, rsu);
                let half = self.s.m.bin(c, Bin::LShr, hm, k1);
                let mask = self.s.m.bin(c, Bin::Sub, hm, k1);
                let rem = self.s.m.bin(c, Bin::And, sig11, mask);
                let gt = self.s.m.cmp(c, pred::IUGT, rem, half);
                let eq = self.s.m.cmp(c, pred::EQ, rem, half);
                let q_odd = self.s.m.bin(c, Bin::And, q, k1);
                let oddb = self.s.m.cmp(c, pred::NE, q_odd, k0);
                let tie = self.s.m.bin(c, Bin::And, eq, oddb);
                let up = self.s.m.bin(c, Bin::Or, gt, tie);
                let inc = self.s.m.select(c, up, k1, k0);
                let qd = self.s.m.bin(c, Bin::Add, q, inc);
                let den = self.s.m.bin(c, Bin::Or, sgn, qd);

                let ov = self.s.m.select(c, over, inf, den);
                let scaled = self.s.m.select(c, valid, norm, ov);
                self.s.m.select(c, fin, scaled, other)
            }
        };

        let mask16 = k(self, 0xFFFF);
        let mut lo = self.s.m.bin(c, Bin::And, r, mask16);
        if v.clamp && name != "v_frexp_exp_i16_f16" {
            // Bit-exact `DX10_CLAMP` `[0.0, 1.0]` on a 16-bit half word `lo`:
            // `lo > 0x7C00` covers all `+NaN` (`0x7C01..0x7FFF`) and all negative
            // encodings (`0x8000..0xFFFF`, including `-0.0`, `-inf`, `-NaN`) -> `+0.0`;
            // remaining values `> 0x3C00` (including `+inf = 0x7C00`) -> `1.0h` (`0x3C00`).
            let one_h = k(self, 0x3C00);
            let inf_h = k(self, 0x7C00);
            let gt_one = self.s.m.cmp(c, pred::IUGT, lo, one_h);
            let c1 = self.s.m.select(c, gt_one, one_h, lo);
            let zero_out = self.s.m.cmp(c, pred::IUGT, lo, inf_h);
            lo = self.s.m.select(c, zero_out, k0, c1);
        }
        self.write_half(inst, lo, guard)
    }
}
