//! RDNA2 SALU `SCC` carry and signed-overflow helpers.
//!
//! Per the RDNA2 ISA pseudocode and `tools/salu_diff.py`:
//! - `s_lshl{1..4}_add_u32` sets `SCC = (((u64)S0 << k) + (u64)S1) >= 2^32`,
//!   which is `(S0 >> (32 - k)) != 0 || ((S0 << k) + S1) <u (S0 << k)`.
//! - `s_add_i32` and `s_addk_i32` set `SCC` to 32-bit signed overflow:
//!   `((S0 ^ D) & (S1 ^ D)) <s 0`.
//! - `s_sub_i32` sets `SCC` to 32-bit signed overflow:
//!   `((S0 ^ S1) & (S0 ^ D)) <s 0`.

use super::bitcode::{Bin, Module, V, pred};

pub(super) fn signed_overflow(m: &mut Module, c: usize, a: V, b: V, r: V, is_sub: bool) -> V {
    let lhs = if is_sub {
        m.bin(c, Bin::Xor, a, b)
    } else {
        m.bin(c, Bin::Xor, b, r)
    };
    let ar = m.bin(c, Bin::Xor, a, r);
    let ov = m.bin(c, Bin::And, lhs, ar);
    let z = m.i32(0);
    m.cmp(c, pred::SLT, ov, z)
}

pub(super) fn lshl_add_u32(m: &mut Module, c: usize, a: V, b: V, shift: u32) -> (V, V) {
    let k = m.i32(shift);
    let t = m.bin(c, Bin::Shl, a, k);
    let r = m.bin(c, Bin::Add, t, b);
    let hi_sh = m.i32(32 - shift);
    let hi = m.bin(c, Bin::LShr, a, hi_sh);
    let z = m.i32(0);
    let sh_out = m.cmp(c, pred::NE, hi, z);
    let add_wrap = m.cmp(c, pred::IULT, r, t);
    let carry = m.bin(c, Bin::Or, sh_out, add_wrap);
    (r, carry)
}
