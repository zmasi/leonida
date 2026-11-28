//! Lane-branch regions that only raise an LDS flag.
//!
//! A console wave64 program with LDS in a group over 32 threads runs on a 32-lane host as two
//! host waves that meet at group barriers on the edges of each lane-branch region
//! (`lower.rs` `lds_meetings`). The console runs such a region as one wave, where a lane's read
//! sees every write before it; split in two, a read inside the region may miss the other half's
//! write, or see one the console's wave makes only later. So a region that both reads and
//! writes LDS is refused, unless that changes nothing the region leaves behind. [`flag_region`]
//! proves that for regions of this shape:
//!
//! - every LDS access in it is a `ds_read_b32` or `ds_write_b32` at a constant address, and
//!   every write stores the same constant;
//! - one address at most is both read and written in it (the flag), and every write a lane can
//!   reach after one of its reads goes to that flag;
//! - it has no other effect (no store, atomic, export, message or barrier);
//! - no register it writes is live, lane by lane, where a lane leaves it.
//!
//! What a lane reads of the flag is then the value from before the region or the constant, and
//! all it can change is whether that lane goes on to write the constant to the flag. The flag
//! ends up raised exactly when some lane raises it, on the console and here: the first write
//! (in the console's order, or in time here) is by a lane that had only read the old value, so
//! it writes in both. Every other address is read only or written only by lanes that had read
//! nothing, and no register carries what was read out of the region.
//! `LEONIDA_LDS_FLAGS=0` turns this off.
//!
//! After FrancescoGelosoEterea's 8bc48bf7 as carried by Itsbatt/victory-unified ad0f9dd
//! (GPL-3.0).

use std::collections::{HashMap, HashSet};

use crate::gfx10::{Fields, Operand};
use crate::ir::{EXEC, Program, Reg, Value};
use crate::structure::{LaneExit, Structured};

type Why = &'static str;

/// Whether flag regions are taken (`LEONIDA_LDS_FLAGS`, on unless `0`).
pub(super) fn on() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| std::env::var_os("LEONIDA_LDS_FLAGS").is_none_or(|v| v != "0"))
}

/// Whether the lane-branch region `blocks` (one region of `lds_meetings`, which both reads and
/// writes LDS) only raises a flag, as the module's text says; else what it does besides.
pub(super) fn flag_region(p: &Program, st: &Structured, blocks: &[usize]) -> Result<(), Why> {
    let inside: HashSet<usize> = blocks.iter().copied().collect();
    // where each value is written: (block, instruction)
    let mut def: HashMap<Value, (usize, usize)> = HashMap::new();
    for (b, block) in p.blocks.iter().enumerate() {
        for (i, inst) in block.insts.iter().enumerate() {
            for &(_, v) in &inst.writes {
                def.insert(v, (b, i));
            }
        }
    }
    let inline = |op: &Operand| -> Option<u32> {
        match *op {
            Operand::Const(c @ 128..=192) => Some(u32::from(c) - 128),
            Operand::Const(c @ 193..=208) => Some((-(i32::from(c) - 192)) as u32),
            Operand::Literal(x) => Some(x),
            _ => None,
        }
    };
    let guard = |b: usize, i: usize| st.guards.get(b).and_then(|g| g.get(i)).copied().flatten();
    // a VGPR value a `v_mov_b32` of a constant wrote, unguarded or under the same guard as `at`
    let constant = |v: Option<Value>, at: (usize, usize)| -> Option<u32> {
        let (b, i) = *def.get(&v?)?;
        let inst = &p.blocks[b].insts[i];
        if inst.name() != "v_mov_b32" {
            return None;
        }
        let g = guard(b, i);
        let same = g.is_none() || (b == at.0 && g == guard(at.0, at.1));
        let Fields::Valu(valu) = &inst.fields else { return None };
        if !same || valu.dpp.is_some() || valu.sdwa.is_some() {
            return None;
        }
        inline(valu.src.first()?)
    };
    // every LDS access: (block, index, is_read, address)
    let mut access: Vec<(usize, usize, bool, u32)> = Vec::new();
    let mut stored: Option<u32> = None;
    for &b in blocks {
        for (i, inst) in p.blocks[b].insts.iter().enumerate() {
            let name = inst.name();
            if name.contains("store")
                || name.contains("atomic")
                || name.starts_with("exp")
                || name.starts_with("s_sendmsg")
                || name == "s_barrier"
            {
                return Err("another effect in the region");
            }
            let Fields::Ds { addr, data, offset0, offset1, gds, .. } = &inst.fields else {
                if name.starts_with("ds_") {
                    return Err("an LDS access of another kind");
                }
                continue;
            };
            if name.starts_with("ds_swizzle") || name.contains("permute") {
                continue;
            }
            let is_read = match name {
                "ds_read_b32" => true,
                "ds_write_b32" => false,
                _ => return Err("an LDS access of another kind"),
            };
            if *gds {
                return Err("an LDS access of another kind");
            }
            let Some(Operand::Vector { reg, .. }) = *addr else {
                return Err("an LDS access at an address that is no constant");
            };
            let Some(base) = constant(inst.read(Reg::V(reg)), (b, i)) else {
                return Err("an LDS access at an address that is no constant");
            };
            let at = base.wrapping_add(u32::from(*offset0) | u32::from(*offset1) << 8);
            if !is_read {
                let Some(&Operand::Vector { reg, .. }) = data.first() else {
                    return Err("an LDS write of a value that is no constant");
                };
                let Some(k) = constant(inst.read(Reg::V(reg)), (b, i)) else {
                    return Err("an LDS write of a value that is no constant");
                };
                if stored.is_some_and(|s| s != k) {
                    return Err("LDS writes of two constants");
                }
                stored = Some(k);
            }
            access.push((b, i, is_read, at));
        }
    }
    let read_at: HashSet<u32> = access.iter().filter(|a| a.2).map(|a| a.3).collect();
    let written_at: HashSet<u32> = access.iter().filter(|a| !a.2).map(|a| a.3).collect();
    let flags: Vec<u32> = read_at.intersection(&written_at).copied().collect();
    if flags.len() > 1 {
        return Err("two LDS addresses both read and written");
    }
    let succ = |b: usize| -> Option<Vec<usize>> {
        match st.exits[b] {
            LaneExit::Jump(t) => Some(vec![t]),
            LaneExit::Branch { taken, not_taken, .. } => Some(vec![taken, not_taken]),
            LaneExit::End => Some(Vec::new()),
            LaneExit::Indirect => None,
        }
    };
    let n = st.exits.len().min(p.blocks.len());
    if (0..n).any(|b| succ(b).is_none()) {
        return Err("an indirect exit");
    }
    if let Some(&flag) = flags.first() {
        for &(rb, ri, is_read, _) in &access {
            if !is_read {
                continue;
            }
            // the blocks of the region a lane can reach from the read along one edge or more
            let mut seen: HashSet<usize> = HashSet::new();
            let mut work: Vec<usize> =
                succ(rb).unwrap_or_default().into_iter().filter(|t| inside.contains(t)).collect();
            while let Some(b) = work.pop() {
                if seen.insert(b) {
                    work.extend(succ(b).unwrap_or_default().into_iter().filter(|t| inside.contains(t)));
                }
            }
            let after = access.iter().any(|&(wb, wi, w_read, at)| {
                !w_read && at != flag && ((wb == rb && wi > ri) || seen.contains(&wb))
            });
            if after {
                return Err("an LDS write elsewhere after a read");
            }
        }
    }
    // Nothing the region computes is used after it: no register it writes is live, lane by
    // lane, where a lane leaves it. A lane's own write kills a register for that lane (an
    // unguarded instruction that writes the whole dword); what an instruction reads of a VGPR
    // only for the lanes outside EXEC (the old value of the one it writes) is no use.
    let exec = |r: Reg| matches!(r, Reg::S(c) if c == EXEC || c == EXEC + 1);
    let covers = |ops: &[Operand], r: Reg| {
        let Reg::V(x) = r else { return false };
        ops.iter().any(|op| {
            matches!(*op, Operand::Vector { reg, dwords }
                if (u16::from(reg)..u16::from(reg) + u16::from(dwords.max(1))).contains(&u16::from(x)))
        })
    };
    // per instruction: the registers it reads for its lanes, and those it writes in full
    let effect = |b: usize, i: usize| -> (Vec<Reg>, Vec<Reg>) {
        let inst = &p.blocks[b].insts[i];
        let name = inst.name();
        let partial = name.contains("16")
            || name.contains("mac")
            || name.contains("lane")
            || matches!(&inst.fields, Fields::Valu(v) if v.dpp.is_some() || v.sdwa.is_some());
        let effectful = name.contains("store") || name.contains("atomic");
        // the operands it reads explicitly, where its old destination is not one of them
        let sources: Option<Vec<Operand>> = match &inst.fields {
            Fields::Valu(v) if !partial => Some(v.src.clone()),
            Fields::Ds { addr, data, .. } if !partial => {
                Some(addr.iter().copied().chain(data.iter().copied()).collect())
            }
            Fields::Buffer { addr, .. } if !partial && !effectful => Some(vec![*addr]),
            Fields::Image { addr, .. } if !partial && !effectful => Some(addr.clone()),
            _ => None,
        };
        let mut reads = Vec::new();
        for &(r, _) in &inst.reads {
            if exec(r) {
                continue;
            }
            let old_only = matches!(r, Reg::V(_))
                && inst.write(r).is_some()
                && sources.as_ref().is_some_and(|src| !covers(src, r));
            if !old_only {
                reads.push(r);
            }
        }
        let plain = !matches!(
            inst.fields,
            Fields::Valu(_) | Fields::Ds { .. } | Fields::Buffer { .. } | Fields::Image { .. }
        );
        let kills: Vec<Reg> = if guard(b, i).is_none() && (sources.is_some() || plain) {
            inst.writes
                .iter()
                .map(|w| w.0)
                .filter(|&r| !exec(r) && !(partial && matches!(r, Reg::V(_))))
                .collect()
        } else {
            Vec::new()
        };
        (reads, kills)
    };
    let mut live_in: Vec<HashSet<Reg>> = vec![HashSet::new(); n];
    loop {
        let mut changed = false;
        for b in (0..n).rev() {
            let mut live: HashSet<Reg> = HashSet::new();
            for t in succ(b).unwrap_or_default() {
                if let Some(l) = live_in.get(t) {
                    live.extend(l.iter().copied());
                }
            }
            for i in (0..p.blocks[b].insts.len()).rev() {
                let (reads, kills) = effect(b, i);
                for r in kills {
                    live.remove(&r);
                }
                live.extend(reads);
            }
            if live != live_in[b] {
                live_in[b] = live;
                changed = true;
            }
        }
        if !changed {
            break;
        }
    }
    let written: HashSet<Reg> = blocks
        .iter()
        .flat_map(|&b| p.blocks[b].insts.iter().flat_map(|i| i.writes.iter().map(|w| w.0)))
        .filter(|&r| !exec(r))
        .collect();
    for &b in blocks {
        for t in succ(b).unwrap_or_default() {
            if !inside.contains(&t) && live_in.get(t).is_some_and(|l| l.iter().any(|r| written.contains(r))) {
                return Err("a register the region writes is read after it");
            }
        }
    }
    Ok(())
}

/// For `LEONIDA_WAVE_WHY`: the LDS instructions and the other effects of the region `blocks`,
/// by name with their counts.
pub(super) fn describe(p: &Program, blocks: &[usize]) -> String {
    let mut count: std::collections::BTreeMap<&str, usize> = Default::default();
    for inst in blocks.iter().flat_map(|&b| &p.blocks[b].insts) {
        let name = inst.name();
        let lds = matches!(inst.fields, Fields::Ds { .. })
            && !(name.starts_with("ds_swizzle") || name.contains("permute"));
        let effect = name.contains("store")
            || name.contains("atomic")
            || name.starts_with("exp")
            || name.starts_with("s_sendmsg")
            || name == "s_barrier";
        if lds || effect {
            *count.entry(name).or_default() += 1;
        }
    }
    let parts: Vec<String> = count.iter().map(|(name, n)| format!("{n} {name}")).collect();
    parts.join(", ")
}
