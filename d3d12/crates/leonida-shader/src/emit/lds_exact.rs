//! Lane-branch regions whose LDS accesses the two host waves of a console wave need not order.
//!
//! The console runs a wave64 as one wave, where every lane's LDS read sees every write that came
//! before it in program order. A host of 32-lane waves runs the wave as two, which meet at group
//! barriers on the edges of each lane-branch region (`lower.rs` `lds_meetings`), never inside
//! it: a barrier must be run by every thread. So a region that both reads and writes LDS is
//! refused unless it only raises a flag ([`super::lds_flags`]). Two more kinds of region need no
//! order between the halves, and [`exact_region`] proves them:
//!
//! - **One thread runs the region.** Every way into its blocks is the edge of a branch on
//!   `v_cmp_eq(thread id, k)` (a group of one dimension: thread `k` of the group and no other),
//!   or from a block that thread alone runs. Its reads and writes are one thread's, in its own
//!   order, and the other threads leave the region's blocks alone: the thread that runs a
//!   `s_and_saveexec` on `tid == 0` around a read-modify-write of a counter, say. The edges of
//!   the region keep their group barriers, so what the region writes reaches the code after it
//!   as before.
//! - **What it writes it does not read.** Per LDS access the byte range it can touch is bounded
//!   from how its address is made (a constant, a masked or shifted value, such a value plus a
//!   constant, the access's own offsets). If no range read in the region meets a range written
//!   in it, and two atomics that meet are the same operation, a read cannot see a write of the
//!   region at all, from either half, so the order of the halves cannot show. The atomics are
//!   commutative; one that returns the old value (`ds_add_rtn_u32`: a wave's reservation of
//!   slots) gives each half the value it saw first, which only the allocation idiom of
//!   [`super::lower::with_local_waves`] tolerates, so only that form takes them.
//!
//! Two writes in one region are taken not to hit the same dword from both halves (the same
//! shortcut as `lds_meetings`). `LEONIDA_LDS_EXACT=0` turns this off.

use std::collections::{HashMap, HashSet};

use crate::gfx10::{Fields, Operand};
use crate::ir::{Inst, Program, Reg, Value};
use crate::structure::{Atom, Bool, LaneExit, Structured};

type Why = &'static str;

/// Whether these regions may be taken at all (`LEONIDA_LDS_EXACT`, on unless `0`).
pub(super) fn switch() -> bool {
    #[cfg(test)]
    if let Some(on) = TEST_EXACT.get() {
        return on;
    }
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| std::env::var_os("LEONIDA_LDS_EXACT").is_none_or(|v| v != "0"))
}

thread_local! {
    /// [`with_exact`] is running.
    static ACTIVE: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
}

/// Whether the regions are taken now: inside [`with_exact`], the last forms `emit` tries.
pub(super) fn on() -> bool {
    switch() && ACTIVE.get()
}

/// Runs `f` with these regions taken, and the allocation idiom knowing the reservation by an LDS
/// atomic. `emit` runs the forms of a program that translates without them as it always has;
/// only a program nothing else took gets here.
pub(super) fn with_exact<R>(f: impl FnOnce() -> R) -> R {
    let was = ACTIVE.replace(true);
    let r = f();
    ACTIVE.set(was);
    r
}

#[cfg(test)]
thread_local! {
    /// A test's choice for [`switch`], for its thread only.
    pub(super) static TEST_EXACT: std::cell::Cell<Option<bool>> = const { std::cell::Cell::new(None) };
}

/// Where each value is written: (block, instruction).
fn definitions(p: &Program) -> HashMap<Value, (usize, usize)> {
    let mut def = HashMap::new();
    for (b, block) in p.blocks.iter().enumerate() {
        for (i, inst) in block.insts.iter().enumerate() {
            for &(_, v) in &inst.writes {
                def.insert(v, (b, i));
            }
        }
    }
    def
}

/// The compare results (every dword they write) that hold in the one thread of a group of one
/// dimension whose number is the other operand: `v_cmp_eq` of the thread id (the program's `v0`
/// on entry) with a constant or a scalar.
fn single_thread_compares(p: &Program, threads: [u32; 3]) -> HashSet<Value> {
    let mut out = HashSet::new();
    if threads[1] != 1 || threads[2] != 1 {
        return out;
    }
    let Some(&(_, tid)) = p.inputs.iter().find(|(r, _)| *r == Reg::V(0)) else {
        return out;
    };
    for inst in p.blocks.iter().flat_map(|b| &b.insts) {
        if !matches!(inst.name(), "v_cmp_eq_u32" | "v_cmp_eq_i32" | "v_cmpx_eq_u32" | "v_cmpx_eq_i32") {
            continue;
        }
        let Fields::Valu(v) = &inst.fields else { continue };
        if v.sdwa.is_some() || v.dpp.is_some() || v.src.len() != 2 {
            continue;
        }
        let is_tid = |o: &Operand| matches!(*o, Operand::Vector { reg, .. } if inst.read(Reg::V(reg)) == Some(tid));
        let uniform = |o: &Operand| match *o {
            Operand::Const(_) | Operand::Literal(_) => true,
            Operand::Scalar { code, .. } => code < crate::ir::PARKED,
            _ => false,
        };
        let (a, b) = (&v.src[0], &v.src[1]);
        if (is_tid(a) && uniform(b)) || (is_tid(b) && uniform(a)) {
            out.extend(inst.writes.iter().filter(|w| matches!(w.0, Reg::S(_))).map(|w| w.1));
        }
    }
    out
}

/// Whether at most one lane has `b` equal to `want`, given the atoms that hold in one lane at
/// most. A mask joined at a loop or a merge (`Atom::Phi`) holds in at most one lane when every
/// edge into the join brings nothing, the join itself, or the one same condition that does.
fn at_most_one(st: &Structured, singles: &HashSet<Value>, b: Bool, want: bool, seen: &mut Vec<Value>) -> bool {
    let none = if want { Bool::FALSE } else { Bool::TRUE };
    if b == none {
        return true;
    }
    let Some((atom, lo, hi)) = st.bools.node(b) else {
        return false;
    };
    // (lanes with the atom false take `lo`, the others `hi`)
    let single = match atom {
        Atom::Lane(v) => singles.contains(&v),
        Atom::Phi(id) => {
            if seen.contains(&id) {
                // (the edges that are not this one decide)
                true
            } else {
                seen.push(id);
                let own = |e: Bool| st.bools.node(e) == Some((Atom::Phi(id), Bool::FALSE, Bool::TRUE));
                let mut brought: Vec<Bool> = Vec::new();
                for &e in st.phi_masks.get(&id).map_or(&[][..], |v| &v[..]) {
                    if e != Bool::FALSE && !own(e) && !brought.contains(&e) {
                        brought.push(e);
                    }
                }
                let ok = st.phi_masks.contains_key(&id)
                    && match brought[..] {
                        [] => true,
                        [e] => at_most_one(st, singles, e, true, seen),
                        _ => false,
                    };
                seen.pop();
                ok
            }
        }
        _ => false,
    };
    if lo == none {
        // (the lanes with `want` are among those that have the atom: at most one when it holds in
        // one lane at most)
        single || at_most_one(st, singles, hi, want, seen)
    } else if hi == none {
        at_most_one(st, singles, lo, want, seen)
    } else {
        false
    }
}

/// Per IR block: whether at most one thread of the group runs it (the module's first kind of
/// region). The greatest set in which every way into a block is the taken edge of a branch on
/// such a condition, or comes from a block of the set.
pub(super) fn single_thread_blocks(p: &Program, st: &Structured, threads: [u32; 3]) -> Vec<bool> {
    let n = st.exits.len().min(p.blocks.len());
    let singles = single_thread_compares(p, threads);
    let mut s = vec![false; n];
    if singles.is_empty() || n == 0 {
        return s;
    }
    // edges into each block: (from, the condition and its value that take a lane along the edge
    // when it is a branch's)
    let mut into: Vec<Vec<(usize, Option<(Bool, bool)>)>> = vec![Vec::new(); n];
    for (q, e) in st.exits.iter().enumerate().take(n) {
        match *e {
            LaneExit::Jump(t) if t < n => into[t].push((q, None)),
            LaneExit::Branch { when, taken, not_taken } => {
                if taken < n && taken != not_taken {
                    into[taken].push((q, Some((when, true))));
                }
                if not_taken < n && not_taken != taken {
                    into[not_taken].push((q, Some((when, false))));
                }
                if taken == not_taken && taken < n {
                    into[taken].push((q, None));
                }
            }
            _ => {}
        }
    }
    s.iter_mut().skip(1).for_each(|x| *x = true);
    // (the entry and what nothing reaches are in no set)
    loop {
        let mut changed = false;
        for b in 1..n {
            if !s[b] {
                continue;
            }
            let ok = !into[b].is_empty()
                && into[b].iter().all(|&(q, edge)| s[q] || edge.is_some_and(|(w, want)| at_most_one(st, &singles, w, want, &mut Vec::new())));
            if !ok {
                s[b] = false;
                changed = true;
            }
        }
        if !changed {
            break;
        }
    }
    s
}

/// A byte range of LDS, both ends included.
type Range = (u64, u64);

/// The values `v` can hold, as an inclusive range: what the instruction that made it and its
/// operands say; anything else the whole 32 bits.
fn bounds(p: &Program, def: &HashMap<Value, (usize, usize)>, v: Value, depth: u32) -> Range {
    const ALL: Range = (0, u32::MAX as u64);
    let Some(&(b, i)) = def.get(&v) else { return ALL };
    if depth == 0 {
        return ALL;
    }
    let inst = &p.blocks[b].insts[i];
    let Fields::Valu(f) = &inst.fields else { return ALL };
    if f.sdwa.is_some() || f.dpp.is_some() || f.abs != 0 || f.neg != 0 || f.clamp || f.omod != 0 {
        return ALL;
    }
    let of = |o: &Operand| -> Range {
        match *o {
            Operand::Const(c) => crate::resource::inline(c).map_or(ALL, |k| (u64::from(k), u64::from(k))),
            Operand::Literal(k) => (u64::from(k), u64::from(k)),
            Operand::Vector { reg, .. } => inst.read(Reg::V(reg)).map_or(ALL, |x| bounds(p, def, x, depth - 1)),
            _ => ALL,
        }
    };
    let sources: Vec<Range> = f.src.iter().map(of).collect();
    match (inst.name(), &sources[..]) {
        ("v_mov_b32", [x]) => *x,
        // anything and a constant: not above the constant
        ("v_and_b32", [a, b]) => (0, a.1.min(b.1)),
        ("v_lshrrev_b32", [s, x]) if s.0 == s.1 && s.0 < 32 => (x.0 >> s.0, x.1 >> s.0),
        ("v_lshlrev_b32", [s, x]) if s.0 == s.1 && s.0 < 32 && x.1 << s.0 <= u64::from(u32::MAX) => (x.0 << s.0, x.1 << s.0),
        ("v_add_nc_u32", [a, b]) if a.1 + b.1 <= u64::from(u32::MAX) => (a.0 + b.0, a.1 + b.1),
        _ => ALL,
    }
}

/// How an LDS access touches memory.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Kind {
    Read,
    Write,
    /// An atomic of this operation; whether it returns the old value.
    Atomic(&'static str, bool),
}

/// The bytes an LDS instruction can touch, per dword access of it, and how: `None` for one this
/// module does not know the reach of.
fn accesses(p: &Program, def: &HashMap<Value, (usize, usize)>, inst: &Inst) -> Option<Vec<(Range, Kind)>> {
    let Fields::Ds { addr, offset0, offset1, gds, .. } = &inst.fields else {
        return None;
    };
    if *gds {
        return None;
    }
    let name = inst.name();
    let base = match addr {
        Some(Operand::Vector { reg, .. }) => {
            inst.read(Reg::V(*reg)).map_or((0, u64::from(u32::MAX)), |v| bounds(p, def, v, 6))
        }
        _ => return None,
    };
    let (op, size) = match name.rsplit_once('_') {
        Some((op, "b32" | "u32" | "i32")) => (op, 4u64),
        Some((op, "b64" | "u64" | "i64")) => (op, 8),
        Some((op, "u8" | "i8" | "b8")) => (op, 1),
        Some((op, "u16" | "i16" | "b16")) => (op, 2),
        // (float atomics round: their order shows)
        Some((op, "f32")) if !op.contains("_add") && !op.contains("_sub") => (op, 4),
        _ => return None,
    };
    let (o0, o1) = (u64::from(*offset0), u64::from(*offset1));
    // (byte offsets, the size of each, the kind)
    let (offsets, kind): (Vec<u64>, Kind) = if op.starts_with("ds_read2st64") {
        (vec![o0 * 256 * size / 4, o1 * 256 * size / 4], Kind::Read)
    } else if op.starts_with("ds_read2") {
        (vec![o0 * size, o1 * size], Kind::Read)
    } else if op.starts_with("ds_write2st64") {
        (vec![o0 * 256 * size / 4, o1 * 256 * size / 4], Kind::Write)
    } else if op.starts_with("ds_write2") {
        (vec![o0 * size, o1 * size], Kind::Write)
    } else if op == "ds_read" {
        (vec![o0 | o1 << 8], Kind::Read)
    } else if op == "ds_write" {
        (vec![o0 | o1 << 8], Kind::Write)
    } else {
        let returns = op.contains("_rtn");
        let atomic = match op.replace("_rtn", "").as_str() {
            "ds_add" => "add",
            "ds_sub" => "sub",
            "ds_min" => "min",
            "ds_max" => "max",
            "ds_and" => "and",
            "ds_or" => "or",
            "ds_xor" => "xor",
            _ => return None,
        };
        (vec![o0 | o1 << 8], Kind::Atomic(atomic, returns))
    };
    offsets
        .into_iter()
        .map(|off| {
            let (lo, hi) = (base.0 + off, base.1 + off + size - 1);
            // (an address past the 32 bits wraps: no range then)
            (hi <= u64::from(u32::MAX)).then_some(((lo, hi), kind))
        })
        .collect()
}

/// Whether the lane-branch region `blocks` (of `lds_meetings`, which reads and writes LDS) needs
/// no order between the two host waves, as the module's text says; else why not. `single` is
/// [`single_thread_blocks`]; `rtn_ok`: the form takes atomics that return the old value.
pub(super) fn exact_region(
    p: &Program,
    blocks: &[usize],
    single: &[bool],
    rtn_ok: bool,
) -> Result<(), Why> {
    if !on() {
        return Err("exact regions are off");
    }
    if blocks.iter().all(|&b| single.get(b).copied().unwrap_or(false)) {
        return Ok(());
    }
    let def = definitions(p);
    let mut all: Vec<(Range, Kind)> = Vec::new();
    for &b in blocks {
        for inst in &p.blocks[b].insts {
            let name = inst.name();
            if !matches!(inst.fields, Fields::Ds { .. }) || name.starts_with("ds_swizzle") || name.contains("permute") {
                continue;
            }
            let Some(list) = accesses(p, &def, inst) else {
                return Err("an LDS access whose reach is not known");
            };
            all.extend(list);
        }
    }
    let meets = |a: &Range, b: &Range| a.0 <= b.1 && b.0 <= a.1;
    for (n, (ra, ka)) in all.iter().enumerate() {
        if let Kind::Atomic(_, true) = ka
            && !rtn_ok
        {
            return Err("an atomic that returns the old value");
        }
        for (rb, kb) in &all[n + 1..] {
            if !meets(ra, rb) {
                continue;
            }
            match (ka, kb) {
                (Kind::Read, Kind::Read) | (Kind::Write, Kind::Write) => {}
                (Kind::Atomic(x, _), Kind::Atomic(y, _)) if x == y => {}
                _ => return Err("an LDS range read and written in one region"),
            }
        }
    }
    Ok(())
}
