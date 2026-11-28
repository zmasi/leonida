//! Accumulator slots of group-shared memory that one thread initialises.
//!
//! The title's tile programs (the water's `CS_TransmissionVolumetrics` family:
//! `5dc3b25986622949`, `6f225b641c5b0708`, `926c46bab3b86bd6`) have one thread of the group
//! store the starting minimum and maximum of a tile (`if (all(tid == 0))`, at `0x230` and `0x244`
//! of the first two) after the per-pixel early returns, and every thread then `ds_min_u32` /
//! `ds_max_u32`s its key in, with no barrier between (one console wave per group). When that
//! thread has returned early (its pixel is outside the volume, or has nothing to light) it never
//! stores, and the atomics combine with whatever the memory held before: on the console another
//! group's values, on the host anything. The tile comes out wrong, a dark patch where its
//! neighbours are lit.
//!
//! [`init_hoists`] names the dwords for which that is the whole story: every write to the dword
//! is of one constant, and every atomic on it accumulates with that constant as its identity
//! (`min` with `0xFFFFFFFF`, `max` with 0, `add`, `or`, ... with 0). Every thread stores the
//! constant there at the start of the program, behind a group barrier (`Lower::hoist_lds_inits`).
//! That is exact: where the program's own store runs it stores the same value again before the
//! dword's first use in that order, and a read that would see the early store instead of a later
//! write of this group would have read memory this group never wrote (undefined). A program
//! with any LDS write or atomic whose address, or (for a write) data, is not a constant of its
//! own block has no hoists: what it could touch is not known.
//! `LEONIDA_LDS_INIT_HOIST=0` turns it off.
//!
//! Ported from fxceless/victory-fork a918163 (GPL-3.0), written again for this IR.

use std::collections::BTreeMap;

use crate::gfx10::{Fields, Operand};
use crate::ir::{EXEC, Inst, Program, Reg, Value};

/// Whether the hoists are made (`LEONIDA_LDS_INIT_HOIST`, on unless `0`). Part of the
/// translation: the shader cache keys on every `LEONIDA_*` switch the source reads.
pub(super) fn on() -> bool {
    #[cfg(test)]
    if let Some(on) = TEST_HOIST.get() {
        return on;
    }
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| std::env::var_os("LEONIDA_LDS_INIT_HOIST").is_none_or(|v| v != "0"))
}

#[cfg(test)]
thread_local! {
    /// A test's choice for [`on`], for its thread only.
    pub(super) static TEST_HOIST: std::cell::Cell<Option<bool>> = const { std::cell::Cell::new(None) };
}

/// The value an LDS dword must start with for the atomic `name` to accumulate into it (`min` of
/// everything with `u32::MAX`, `max` with 0, ...); `None` for an operation that is no such
/// accumulation.
fn identity(name: &str) -> Option<u32> {
    Some(match name.strip_suffix("_rtn").unwrap_or(name) {
        "ds_min_u32" | "ds_and_b32" => u32::MAX,
        "ds_max_u32" | "ds_add_u32" | "ds_or_b32" | "ds_xor_b32" => 0,
        "ds_min_i32" => 0x7FFF_FFFF,
        "ds_max_i32" => 0x8000_0000,
        _ => return None,
    })
}

/// The LDS dwords (below `dwords`, the program's own) that the program initialises for
/// accumulation, with their starting values, in rising order. See the module's text.
pub(super) fn init_hoists(p: &Program, dwords: u32) -> Vec<(u32, u32)> {
    // `x` at instruction `i` of `insts`: the constant a plain `v_mov_b32` before it in the
    // block gave it, with EXEC unchanged since (so in every lane that runs `i`)
    let constant = |insts: &[Inst], i: usize, x: Value| -> Option<u32> {
        let d = insts[..i].iter().rposition(|j| j.writes.iter().any(|w| w.1 == x))?;
        let Fields::Valu(f) = &insts[d].fields else {
            return None;
        };
        let plain = insts[d].name() == "v_mov_b32"
            && f.abs == 0
            && f.neg == 0
            && !f.clamp
            && f.omod == 0
            && f.opsel == 0
            && f.sdwa.is_none()
            && f.dpp.is_none()
            && insts[d + 1..i]
                .iter()
                .all(|j| j.write(Reg::S(EXEC)).is_none() && j.write(Reg::S(EXEC + 1)).is_none());
        if !plain {
            return None;
        }
        match f.src.first()? {
            Operand::Const(c) => crate::resource::inline(*c),
            Operand::Literal(k) => Some(*k),
            _ => None,
        }
    };
    let operand = |insts: &[Inst], i: usize, o: Option<&Operand>| -> Option<u32> {
        let &Operand::Vector { reg, .. } = o? else {
            return None;
        };
        constant(insts, i, insts[i].read(Reg::V(reg))?)
    };
    // per dword: the values stored, the identities of the atomics
    let mut stores: BTreeMap<u32, Vec<u32>> = BTreeMap::new();
    let mut atomics: BTreeMap<u32, Vec<u32>> = BTreeMap::new();
    for block in &p.blocks {
        let insts = &block.insts;
        for (i, inst) in insts.iter().enumerate() {
            let name = inst.name();
            match &inst.fields {
                // (a load through the LDS: its destination is unknown)
                Fields::Buffer { lds: true, .. } | Fields::Flat { lds: true, .. } => return Vec::new(),
                Fields::Ds { .. } => {}
                _ => continue,
            }
            let Fields::Ds { addr, data, offset0, offset1, gds, .. } = &inst.fields else {
                continue;
            };
            if *gds || name.starts_with("ds_swizzle") || name.contains("permute") || name.starts_with("ds_read") {
                continue;
            }
            let Some(base) = operand(insts, i, addr.as_ref()) else {
                return Vec::new();
            };
            let single = u32::from(*offset0) | u32::from(*offset1) << 8;
            let dword = |bytes: u32| base.wrapping_add(bytes) / 4;
            match name {
                "ds_write_b32" | "ds_write2_b32" => {
                    let at: &[u32] = if name == "ds_write_b32" {
                        &[single]
                    } else {
                        &[u32::from(*offset0) * 4, u32::from(*offset1) * 4]
                    };
                    for (n, &bytes) in at.iter().enumerate() {
                        let Some(k) = operand(insts, i, data.get(n)) else {
                            return Vec::new();
                        };
                        stores.entry(dword(bytes)).or_default().push(k);
                    }
                }
                _ => {
                    let Some(id) = identity(name) else {
                        return Vec::new();
                    };
                    atomics.entry(dword(single)).or_default().push(id);
                }
            }
        }
    }
    stores
        .into_iter()
        .filter_map(|(dword, values)| {
            let k = values[0];
            let ids = atomics.get(&dword)?;
            (dword < dwords && values.iter().all(|&v| v == k) && ids.iter().all(|&v| v == k)).then_some((dword, k))
        })
        .collect()
}
