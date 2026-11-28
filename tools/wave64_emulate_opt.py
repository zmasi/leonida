"""Exact CFG/dataflow optimizations for --wave64-emulate (Issue #884).

Two conservative, bit-exact analyses over a compute/mesh shader's basic blocks:

1. `scc_live_outs(blocks, succ)` -> set of `id(ins)` whose SCC write can reach
   any SCC read before being unconditionally overwritten. Under --wave64-emulate,
   mask ops (`s_and_saveexec_b64`, `s_andn2_b64`, `s_or_b64`, `s_not_b64`,
   `s_wqm_b64`, ...) otherwise pay a full cross-lane `x_round` + `x_split`
   (workgroup barrier + OpSwitch case) to compute SCC even when SCC is dead.

2. `find_local_exec_folds(blocks, raw_edges, succ, lane_masks, scc_live)` ->
   `(folds, fold_chain, absorbed)`: identifies single-entry, single-exit
   regions guarded by `s_cbranch_execz` / `s_cbranch_execnz` whose body has:
     - no cross-lane operations, no `s_barrier`, no live-SCC mask votes,
     - no memory or LDS writes/atomics and no unmasked side effects,
     - no EXEC-widening writes (an inactive lane stays inactive across the region),
     - no scalar (`SGPR`, `VCC`, `SCC`, `M0`) written in the region that is live
       at the join block (`skip`),
     - reducible internal control flow (`cfg_structure.structure` succeeds).

   Each such region can be emitted inline as a structured `if (%active) { R }`
   inside its guarding block with zero `x_round` / `x_split` / `OpControlBarrier`,
   keeping all 64 lanes of the wave in the exact same scheduler round on arrival
   at `skip`. When `skip` has no predecessors outside `{B} | R`, `skip` is also
   chained inline after the fold, collapsing chains of thousands of unrolled
   `s_and_saveexec_b64; s_cbranch_execz` diamonds into a single scheduler block.

Standard library only.
"""
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.realpath(__file__)))
import cfg_structure  # noqa: E402
import lane_masks as _lane_masks  # noqa: E402

BRANCHES = frozenset({
    "s_branch", "s_cbranch_scc0", "s_cbranch_scc1",
    "s_cbranch_vccz", "s_cbranch_vccnz",
    "s_cbranch_execz", "s_cbranch_execnz",
})

# Instructions that implicitly read SCC (in addition to explicit `scc` operands).
_SCC_IMPLICIT_READS = frozenset({
    "s_cbranch_scc0", "s_cbranch_scc1",
    "s_cselect_b32", "s_cselect_b64",
    "s_cmov_b32", "s_cmov_b64", "s_cmovk_i32",
    "s_addc_u32", "s_subb_u32",
})

# Instructions verified in tools/rdna2spirv.py to unconditionally overwrite %scc.
# Conservatively omitting an op only keeps SCC live across it (never unsafe).
_SAVEEXEC_KINDS = ("and", "or", "xor", "andn1", "andn2", "orn1", "orn2", "nand", "nor", "xnor")
_ALL_SAVEEXEC_WREXEC = frozenset(
    [f"s_{k}_saveexec_b{w}" for k in _SAVEEXEC_KINDS for w in (32, 64)]
    + [f"s_{k}_wrexec_b{w}" for k in ("andn1", "andn2") for w in (32, 64)]
)

_SCC_UNCONDITIONAL_WRITES = _ALL_SAVEEXEC_WREXEC | frozenset({
    "s_and_b64", "s_or_b64", "s_xor_b64", "s_andn2_b64", "s_orn2_b64",
    "s_nand_b64", "s_nor_b64", "s_xnor_b64", "s_not_b64",
    "s_and_b32", "s_or_b32", "s_xor_b32", "s_andn2_b32", "s_orn2_b32",
    "s_nand_b32", "s_nor_b32", "s_xnor_b32", "s_not_b32",
    "s_and_b32_mask", "s_or_b32_mask",
    "s_add_u32", "s_addc_u32", "s_sub_u32", "s_subb_u32",
    "s_add_i32", "s_sub_i32", "s_addk_i32",
    "s_lshl1_add_u32", "s_lshl2_add_u32", "s_lshl3_add_u32", "s_lshl4_add_u32",
    "s_min_u32", "s_max_u32", "s_min_i32", "s_max_i32",
    "s_abs_i32", "s_absdiff_i32",
    "s_lshl_b32", "s_lshr_b32", "s_ashr_i32", "s_lshl_b64",
    "s_bfe_u32", "s_bfe_i32", "s_bfe_u64", "s_bfe_i64",
    "s_bcnt0_i32_b32", "s_bcnt1_i32_b32", "s_bcnt0_i32_b64", "s_bcnt1_i32_b64",
    "s_wqm_b32", "s_wqm_b64", "s_quadmask_b32", "s_quadmask_b64",
    "s_cmp_eq_u32", "s_cmp_lg_u32", "s_cmp_gt_u32", "s_cmp_ge_u32", "s_cmp_lt_u32", "s_cmp_le_u32",
    "s_cmp_eq_i32", "s_cmp_lg_i32", "s_cmp_gt_i32", "s_cmp_ge_i32", "s_cmp_lt_i32", "s_cmp_le_i32",
    "s_cmp_eq_u64", "s_cmp_lg_u64",
    "s_bitcmp0_b32", "s_bitcmp1_b32", "s_bitcmp0_b64", "s_bitcmp1_b64",
})

# Scalar ops that reconstruct a whole mask word when their source holds a lane mask.
_MASK_WORD_READERS = frozenset({
    "s_bcnt0_i32_b32", "s_bcnt1_i32_b32", "s_bcnt0_i32_b64", "s_bcnt1_i32_b64",
    "s_ff0_i32_b32", "s_ff1_i32_b32", "s_ff0_i32_b64", "s_ff1_i32_b64",
    "s_flbit_i32_b32", "s_flbit_i32_b64", "s_flbit_i32",
})

# Mask ops that emit a cross-lane vote in --wave64-emulate when SCC is live.
_MASK_SCC_VOTE_OPS = _ALL_SAVEEXEC_WREXEC | frozenset({
    "s_and_b64", "s_or_b64", "s_xor_b64", "s_andn2_b64", "s_orn2_b64",
    "s_nand_b64", "s_nor_b64", "s_xnor_b64", "s_not_b64",
    "s_and_b32_mask", "s_or_b32_mask",
    "s_wqm_b32", "s_wqm_b64",
})

# Scalar register bit positions for fast integer-bitmask backward liveness.
_BIT_VCC = 128
_BIT_SCC = 129
_BIT_M0 = 130
_ALL_SGPRS = (1 << 128) - 1

_IMPLICIT_VCC_READS = frozenset({
    "v_cndmask_b32", "v_addc_co_u32", "v_subb_co_u32", "v_subbrev_co_u32",
    "v_add_co_ci_u32", "v_sub_co_ci_u32", "v_subrev_co_ci_u32",
    "v_addc_u32", "v_subb_u32", "v_div_fmas_f32", "v_div_fmas_f64",
    "s_cbranch_vccz", "s_cbranch_vccnz",
})

_NO_DEST_PREFIXES = (
    "s_cmp", "s_bitcmp", "v_cmpx", "exp",
    "buffer_store", "global_store", "flat_store", "scratch_store", "tbuffer_store", "image_store",
    "ds_write", "ds_store", "s_store", "s_buffer_store",
    "s_cbranch", "s_branch", "s_waitcnt", "s_nop", "s_endpgm", "s_barrier", "s_sendmsg",
    "s_setprio", "s_inst_prefetch", "s_clause", "s_trap", "s_icache", "s_dcache", "s_sleep",
    "s_code_end", "s_denorm_mode", "s_round_mode", "s_ttracedata", "v_nop", "v_clrexcp",
    "v_pipeflush", "s_set", "s_sethalt", "s_setkill", "s_wakeup", "s_version",
    "buffer_gl", "buffer_wbinvl", "ds_gws", "ds_nop",
)

_SOP2_B32_LOGIC = frozenset({
    "s_and_b32", "s_or_b32", "s_xor_b32", "s_andn2_b32", "s_orn2_b32",
    "s_nand_b32", "s_nor_b32", "s_xnor_b32", "s_not_b32",
})

_FORBIDDEN_FOLD_OPS = frozenset({
    "s_barrier", "s_sendmsg", "s_setpc_b64", "s_swappc_b64", "s_trap",
    "s_sethalt", "s_setkill", "s_sleep", "s_setprio",
    "s_round_mode", "s_denorm_mode",
    "v_readlane_b32", "v_readfirstlane_b32", "v_writelane_b32",
    "v_permlane16_b32", "v_permlanex16_b32",
    "ds_bpermute_b32", "ds_permute_b32", "ds_swizzle_b32",
    "ds_gws_init", "ds_gws_sema_v", "ds_gws_sema_br", "ds_gws_sema_p", "ds_gws_barrier",
    "ds_consume", "ds_append", "ds_ordered_count",
    "s_wqm_b32", "s_wqm_b64", "s_quadmask_b32", "s_quadmask_b64",
})

_ALWAYS_BARRIER_OPS = frozenset({
    "s_barrier", "v_readlane_b32", "v_readfirstlane_b32",
    "v_permlane16_b32", "v_permlanex16_b32",
    "ds_bpermute_b32", "ds_permute_b32", "ds_swizzle_b32",
    "ds_append", "ds_consume",
    "s_wqm_b32", "s_wqm_b64",
})

_VOTED_BRANCHES = frozenset({
    "s_cbranch_execz", "s_cbranch_execnz", "s_cbranch_vccz", "s_cbranch_vccnz",
})

_SCALAR_NO_SCC_PREFIXES = (
    "s_mov", "s_cselect", "s_load", "s_buffer_load", "s_getpc",
    "s_nop", "s_waitcnt", "s_clause", "s_inst_prefetch", "s_pack",
    "s_mul_i32", "s_mul_hi", "s_mulk", "s_ff", "s_flbit", "s_bfm",
    "s_get_waveid",
)

# Bounded caches for opcode normalization and static per-opcode classification.
_OP_CACHE_MAX = 4096
_BASE_OP_CACHE = {}
_OP_META_CACHE = {}

# _scalar_rw dispatch modes
_RW_MOVREL = 0
_RW_TERM = 1
_RW_NO_DEST = 2
_RW_VCMP = 3
_RW_ATOMIC = 4
_RW_DS_RET = 5
_RW_DS_NORET = 6
_RW_SOPK_RW = 7
_RW_DEFAULT = 8
_RW_VOP3B = 9

# Bit flags in meta[8]
_FLAG_CMOV_RD_DEST = 1
_FLAG_NO_KILL_DEST = 2
_FLAG_FORBIDDEN_FOLD = 4
_FLAG_ALWAYS_BARRIER = 8
_FLAG_WRITES_EXEC_OP = 16
_FLAG_SCC_VOTE_ALWAYS_MASK = 32
_FLAG_SOP2_B32_LOGIC = 64
_FLAG_VOTED_BRANCH = 128

# Mask check categories in meta[11]
_MASK_CHK_NONE = 0
_MASK_CHK_WORD_READER = 1
_MASK_CHK_MBCNT = 2
_MASK_CHK_SCMP = 3
_MASK_CHK_SCC_VOTE = 4

# 4-bit dirty-state bitmask: bit0=("mem","r"), bit1=("mem","w"), bit2=("lds","r"), bit3=("lds","w")
_DIRTY_MEM_R = 1
_DIRTY_MEM_W = 2
_DIRTY_LDS_R = 4
_DIRTY_LDS_W = 8
_DIRTY_MASK_TO_FROZENSET = tuple(
    frozenset(
        tok
        for bit, tok in (
            (_DIRTY_MEM_R, ("mem", "r")),
            (_DIRTY_MEM_W, ("mem", "w")),
            (_DIRTY_LDS_R, ("lds", "r")),
            (_DIRTY_LDS_W, ("lds", "w")),
        )
        if m & bit
    )
    for m in range(16)
)

_EMPTY_MEM_STATS = (0, False, False, 0, False, False)


def base_op(op):
    res = _BASE_OP_CACHE.get(op)
    if res is not None:
        return res
    res = op
    for suffix in ("_e32", "_e64", "_sdwa", "_dpp"):
        if op.endswith(suffix):
            res = op[:-len(suffix)]
            break
    if len(_BASE_OP_CACHE) >= _OP_CACHE_MAX:
        _BASE_OP_CACHE.clear()
    _BASE_OP_CACHE[op] = res
    return res


def _get_op_meta(raw_op):
    """Return cached static metadata tuple for raw opcode `raw_op`."""
    meta = _OP_META_CACHE.get(raw_op)
    if meta is not None:
        return meta

    op = base_op(raw_op)
    is_term = op == "s_endpgm" or op in BRANCHES
    scc_imp_read = op in _SCC_IMPLICIT_READS or op.startswith(("s_getreg", "s_setreg", "s_save_status"))
    scc_uncond_write = op in _SCC_UNCONDITIONAL_WRITES

    if op.startswith("s_movrel"):
        rw_mode = _RW_MOVREL
    elif is_term:
        rw_mode = _RW_TERM
    elif op.startswith(("s_cmp", "s_bitcmp")) or op.startswith("v_cmpx") or op.startswith(_NO_DEST_PREFIXES):
        rw_mode = _RW_NO_DEST
    elif op.startswith("v_cmp"):
        rw_mode = _RW_VCMP
    elif op.startswith(("buffer_atomic", "global_atomic", "flat_atomic", "image_atomic")):
        rw_mode = _RW_ATOMIC
    elif op.startswith("ds_"):
        returns = (op.startswith(("ds_read", "ds_bpermute", "ds_permute", "ds_swizzle", "ds_consume", "ds_append"))
                   or "_rtn" in op)
        rw_mode = _RW_DS_RET if returns else _RW_DS_NORET
    elif op in ("s_addk_i32", "s_mulk_i32", "s_cmovk_i32"):
        rw_mode = _RW_SOPK_RW
    elif op == "s_movk_i32":
        rw_mode = _RW_DEFAULT
    elif op.startswith("v_") and (
            "_co_" in op or op in ("v_mad_u64_u32", "v_mad_i64_i32", "v_div_scale_f32", "v_div_scale_f64")):
        rw_mode = _RW_VOP3B
    else:
        rw_mode = _RW_DEFAULT

    base_rd = 0
    if op in _IMPLICIT_VCC_READS:
        base_rd |= 1 << _BIT_VCC
    if scc_imp_read:
        base_rd |= 1 << _BIT_SCC
    if op.startswith(("ds_", "s_sendmsg", "v_interp", "v_movrel", "v_swaprel")):
        base_rd |= 1 << _BIT_M0

    base_mw = 0
    base_mk = 0
    if scc_uncond_write:
        base_mw |= 1 << _BIT_SCC
        base_mk |= 1 << _BIT_SCC
    elif op.startswith("s_") and not op.startswith(_SCALAR_NO_SCC_PREFIXES):
        base_mw |= 1 << _BIT_SCC

    x_mem_acc = _x_memory_order_access(op)
    x_mem_dirty = 0
    scratch_acc = None
    if x_mem_acc is not None:
        kind, r, w = x_mem_acc
        if kind == "lds":
            if r:
                x_mem_dirty |= _DIRTY_LDS_R
            if w:
                x_mem_dirty |= _DIRTY_LDS_W
        else:
            if r:
                x_mem_dirty |= _DIRTY_MEM_R
            if w:
                x_mem_dirty |= _DIRTY_MEM_W
    elif op.startswith("scratch_"):
        if "store" in op or "atomic" in op:
            scratch_acc = ("atomic" in op, True)
        else:
            scratch_acc = (True, False)

    flags = 0
    if op == "s_cmov_b32" or op == "s_cmov_b64":
        flags |= _FLAG_CMOV_RD_DEST | _FLAG_NO_KILL_DEST
    elif op == "s_cmovk_i32":
        flags |= _FLAG_NO_KILL_DEST

    is_dpp = raw_op.endswith(("_dpp", "_dpp8"))
    if (is_dpp
            or op in _FORBIDDEN_FOLD_OPS
            or op.startswith(("s_movrel", "s_setreg", "s_getreg", "s_store", "s_buffer_store", "s_atomic",
                              "ds_gws", "exp"))
            or (x_mem_acc is None and scratch_acc is None
                and not op.startswith(("image_sample", "image_gather", "image_bvh"))
                and ("store" in op or "atomic" in op))):
        flags |= _FLAG_FORBIDDEN_FOLD

    if is_dpp or op in _ALWAYS_BARRIER_OPS or op.startswith("s_movrel"):
        flags |= _FLAG_ALWAYS_BARRIER
    if op.startswith("v_cmpx") or "saveexec" in op or "wrexec" in op:
        flags |= _FLAG_WRITES_EXEC_OP
    if op in _MASK_SCC_VOTE_OPS and ("saveexec" in op or "wrexec" in op or op.endswith("_mask")):
        flags |= _FLAG_SCC_VOTE_ALWAYS_MASK
    if op in _SOP2_B32_LOGIC:
        flags |= _FLAG_SOP2_B32_LOGIC
    if op in _VOTED_BRANCHES:
        flags |= _FLAG_VOTED_BRANCH

    if op in _MASK_WORD_READERS:
        mask_chk = _MASK_CHK_WORD_READER
    elif op in ("v_mbcnt_lo_u32_b32", "v_mbcnt_hi_u32_b32"):
        mask_chk = _MASK_CHK_MBCNT
    elif op.startswith(("s_cmp", "s_bitcmp")):
        mask_chk = _MASK_CHK_SCMP
    elif op in _MASK_SCC_VOTE_OPS:
        mask_chk = _MASK_CHK_SCC_VOTE
    else:
        mask_chk = _MASK_CHK_NONE

    meta = (
        op,                # 0
        is_term,           # 1
        scc_imp_read,      # 2
        scc_uncond_write,  # 3
        rw_mode,           # 4
        base_rd,           # 5
        base_mw,           # 6
        base_mk,           # 7
        flags,             # 8
        x_mem_acc,         # 9
        scratch_acc,       # 10
        mask_chk,          # 11
        x_mem_dirty,       # 12
    )
    if len(_OP_META_CACHE) >= _OP_CACHE_MAX:
        _OP_META_CACHE.clear()
    _OP_META_CACHE[raw_op] = meta
    return meta


def _active_insns(blk):
    """Instructions of a basic block up to and including its terminator (branch or s_endpgm)."""
    meta_get = _OP_META_CACHE.get
    n = len(blk)
    for i, ins in enumerate(blk):
        raw_op = ins["op"]
        m = meta_get(raw_op)
        if m is None:
            m = _get_op_meta(raw_op)
        if m[1]:
            return blk if i == n - 1 else blk[:i + 1]
    return blk


def _has_vcc_or_exec(x):
    k = x.get("k")
    if k == "special":
        return x.get("t", "").startswith(("exec", "vcc"))
    if k == "slist":
        return any(isinstance(item, str) and item.startswith("vcc") for item in x.get("r", ()))
    return False


def _reads_scc_with_meta(ins, meta):
    if meta[2]:
        return True
    for x in ins["args"]:
        if x.get("k") == "special" and x.get("t") in ("scc", "src_scc"):
            return True
    return False


def reads_scc(ins):
    return _reads_scc_with_meta(ins, _get_op_meta(ins["op"]))


def writes_scc(ins):
    return _get_op_meta(ins["op"])[3]


def scc_live_outs(blocks, succ):
    """Return the set of `id(ins)` whose SCC result may be read before being unconditionally overwritten."""
    n = len(blocks)
    if not n:
        return set()
    preds = [[] for _ in range(n)]
    for b in range(n):
        for s in succ[b]:
            preds[s].append(b)
    use = [False] * n
    kill = [False] * n
    active = [_active_insns(blk) for blk in blocks]
    meta_get = _OP_META_CACHE.get
    for b, insns in enumerate(active):
        k = False
        u = False
        for ins in insns:
            raw_op = ins["op"]
            m = meta_get(raw_op)
            if m is None:
                m = _get_op_meta(raw_op)
            if m[2]:
                u = True
                break
            rd_explicit = False
            for x in ins["args"]:
                if x.get("k") == "special" and x.get("t") in ("scc", "src_scc"):
                    rd_explicit = True
                    break
            if rd_explicit:
                u = True
                break
            if m[3]:
                k = True
                break
        use[b] = u
        kill[b] = k
    live_in = list(use)
    live_out = [False] * n
    work = [b for b in range(n) if live_in[b]]
    while work:
        b = work.pop()
        for p in preds[b]:
            if not live_out[p]:
                live_out[p] = True
                if not kill[p] and not live_in[p]:
                    live_in[p] = True
                    work.append(p)
    out = set()
    for b, insns in enumerate(active):
        live = live_out[b]
        for ins in reversed(insns):
            if live:
                out.add(id(ins))
            raw_op = ins["op"]
            m = meta_get(raw_op)
            if m is None:
                m = _get_op_meta(raw_op)
            if m[2]:
                live = True
            else:
                if live and m[3]:
                    live = False
                if not live:
                    for x in ins["args"]:
                        if x.get("k") == "special" and x.get("t") in ("scc", "src_scc"):
                            live = True
                            break
    return out


def _operand_scalar_mask(x):
    """Bitmask of scalar registers (s0..s127, vcc, scc, m0) named by operand `x`."""
    k = x.get("k")
    if k == "sgpr":
        r = x["r"]
        cnt = x.get("n", 1)
        if cnt == 1:
            return (1 << r) if 0 <= r < 128 else 0
        lo = 0 if r < 0 else r
        hi = r + cnt
        if hi > 128:
            hi = 128
        return (((1 << (hi - lo)) - 1) << lo) if hi > lo else 0
    if k == "special":
        t = x.get("t", "")
        if t.startswith("vcc"):
            return 1 << _BIT_VCC
        if t in ("scc", "src_scc"):
            return 1 << _BIT_SCC
        if t == "m0":
            return 1 << _BIT_M0
        return 0
    if k == "slist":
        m = 0
        for item in x.get("r", ()):
            if isinstance(item, int):
                if 0 <= item < 128:
                    m |= 1 << item
            elif isinstance(item, str) and item.startswith("vcc"):
                m |= 1 << _BIT_VCC
        return m
    return 0


def _scalar_rw_with_meta(ins, meta):
    """Return `(read_mask, may_write_mask, must_kill_mask)` over scalar bits 0..130 for `ins`."""
    rw_mode = meta[4]
    if rw_mode == _RW_MOVREL:
        return _ALL_SGPRS | (1 << _BIT_M0), _ALL_SGPRS, 0
    if rw_mode == _RW_TERM:
        return meta[5], 0, 0

    a = ins["args"]
    len_a = len(a)
    vcc_imp_w = 0
    if rw_mode == _RW_DEFAULT:
        dest_n = 1 if len_a else 0
        src_start = 1
    elif rw_mode == _RW_NO_DEST or rw_mode == _RW_DS_NORET:
        dest_n = 0
        src_start = 0
    elif rw_mode == _RW_VCMP:
        if len_a == 2:
            dest_n = 0
            src_start = 0
            vcc_imp_w = 1 << _BIT_VCC
        else:
            dest_n = 1 if len_a else 0
            src_start = 1
    elif rw_mode == _RW_VOP3B:
        if len_a > 2 and a[1].get("k") in ("sgpr", "special", "slist"):
            dest_n = 2
            src_start = 2
        else:
            dest_n = 1 if len_a else 0
            src_start = 1
    elif rw_mode == _RW_DS_RET:
        dest_n = 1 if len_a else 0
        src_start = 1
    elif rw_mode == _RW_SOPK_RW:
        dest_n = 1 if len_a else 0
        src_start = 0
    else:  # _RW_ATOMIC
        mods = ins.get("mods")
        dest_n = 1 if (len_a and mods and "glc" in mods) else 0
        src_start = 0

    rd = meta[5]
    may_w = meta[6] | vcc_imp_w
    flags = meta[8]
    no_kill_dest = flags & _FLAG_NO_KILL_DEST
    must_kill = meta[7] | (0 if no_kill_dest else vcc_imp_w)
    cmov_rd_dest = flags & _FLAG_CMOV_RD_DEST

    for idx in range(len_a):
        x = a[idx]
        k = x.get("k")
        if k == "vgpr" or k == "imm":
            continue
        if k == "sgpr":
            r = x["r"]
            cnt = x.get("n", 1)
            if cnt == 1:
                m = (1 << r) if 0 <= r < 128 else 0
            else:
                lo = 0 if r < 0 else r
                hi = r + cnt
                if hi > 128:
                    hi = 128
                m = (((1 << (hi - lo)) - 1) << lo) if hi > lo else 0
            if idx >= src_start:
                rd |= m
            if idx < dest_n:
                if cmov_rd_dest:
                    rd |= m
                may_w |= m
                if not no_kill_dest:
                    must_kill |= m
        elif k == "special":
            t = x.get("t", "")
            if t.startswith("vcc"):
                m = 1 << _BIT_VCC
            elif t in ("scc", "src_scc"):
                m = 1 << _BIT_SCC
                rd |= m  # reads_scc scans all args
            elif t == "m0":
                m = 1 << _BIT_M0
            else:
                continue
            if idx >= src_start:
                rd |= m
            if idx < dest_n:
                if cmov_rd_dest:
                    rd |= m
                may_w |= m
                if not no_kill_dest and t not in ("vcc_lo", "vcc_hi"):
                    must_kill |= m
        elif k == "slist":
            regs = x.get("r", ())
            m = 0
            for item in regs:
                if isinstance(item, int):
                    if 0 <= item < 128:
                        m |= 1 << item
                elif isinstance(item, str) and item.startswith("vcc"):
                    m |= 1 << _BIT_VCC
            if idx >= src_start:
                rd |= m
            if idx < dest_n:
                if cmov_rd_dest:
                    rd |= m
                may_w |= m
                if not no_kill_dest:
                    has_lo = False
                    has_hi = False
                    for item in regs:
                        if isinstance(item, int):
                            if 0 <= item < 128:
                                must_kill |= 1 << item
                        elif item == "vcc_lo":
                            has_lo = True
                        elif item == "vcc_hi":
                            has_hi = True
                    if has_lo and has_hi:
                        must_kill |= 1 << _BIT_VCC

    return rd, may_w, must_kill


def _scalar_rw(ins):
    """Return `(read_mask, may_write_mask, must_kill_mask)` over scalar bits 0..130 for `ins`."""
    return _scalar_rw_with_meta(ins, _get_op_meta(ins["op"]))


def _solve_scalar_liveness(use, kill, preds):
    """Run backward scalar liveness worklist given per-block `(use, kill)` bitmasks and `preds`."""
    n = len(use)
    live_in = list(use)
    live_out = [0] * n
    in_work = [bool(u) for u in live_in]
    work = collections.deque(b for b in range(n) if in_work[b])
    while work:
        b = work.popleft()
        in_work[b] = False
        bi = live_in[b]
        for p in preds[b]:
            new_out = live_out[p] | bi
            if new_out != live_out[p]:
                live_out[p] = new_out
                new_in = use[p] | (new_out & ~kill[p])
                if new_in != live_in[p]:
                    live_in[p] = new_in
                    if not in_work[p]:
                        in_work[p] = True
                        work.append(p)
    return live_in


def _scalar_liveness(active_blocks, succ):
    """Exact conservative backward scalar liveness `live_in[b]` (int bitmask) for each block."""
    n = len(active_blocks)
    preds = [[] for _ in range(n)]
    for b in range(n):
        for s in succ[b]:
            preds[s].append(b)
    use = [0] * n
    kill = [0] * n
    meta_get = _OP_META_CACHE.get
    for b, insns in enumerate(active_blocks):
        u = 0
        k = 0
        for ins in insns:
            raw_op = ins["op"]
            m = meta_get(raw_op)
            if m is None:
                m = _get_op_meta(raw_op)
            rd, _, mk = _scalar_rw_with_meta(ins, m)
            u |= rd & ~k
            k |= mk
        use[b] = u
        kill[b] = k
    return _solve_scalar_liveness(use, kill, preds)


def mark_vcc_data_moves(blocks, succ):
    """Keep both words of scalar VCC copies used as data. Leave predicate copies in lane-mask form.

    Look between full definitions. A predicate read, an uncertain partial write, or a live-out
    at a block boundary prevents changing the producer. A wide SMEM load already writes both
    physical words, so an ensuing scalar copy out of that tuple must also read both.
    """
    if not any(i["op"] == "s_mov_b64" and _lane_masks.wave32_vcc_pair_op(i["op"], i["args"])
               for block in blocks for i in block):
        return
    vcc = 1 << _BIT_VCC
    live = _scalar_liveness(blocks, succ)
    for b, block in enumerate(blocks):
        numeric_use = 0
        unsafe = 3 if any(live[n] & vcc for n in succ[b]) else 0
        for ins in reversed(block):
            op, args = _lane_masks.base_op(ins["op"]), ins["args"]
            rd, may_write, kill = _scalar_rw(ins)
            if not (rd | may_write) & vcc:
                continue
            pair = op == "s_mov_b64" and _lane_masks.wave32_vcc_pair_op(op, args)
            if pair and args[0].get("t") == "vcc" and numeric_use and not unsafe:
                ins["vcc_data_pair"] = True
            if kill & vcc:
                unsafe = numeric_use = 0
            elif may_write & vcc:
                half = {"vcc_lo": 1, "vcc_hi": 2}.get(args[0].get("t")) if args else None
                if half and op in ("s_mov_b32", "s_add_u32"):
                    unsafe &= ~half
                    numeric_use &= ~half
                else:
                    unsafe |= 3
            if rd & vcc:
                # The general liveness pass conservatively counts implicit VCC even with an
                # explicit e64 predicate in another SGPR pair.
                if op == "v_cndmask_b32" and len(args) == 4 and args[-1].get("t") != "vcc":
                    if all(a.get("t") not in ("vcc_lo", "vcc_hi", "vcc") for a in args[1:]):
                        continue
                tuple_read = any(a["k"] == "slist" and "vcc_lo" in a["r"] and "vcc_hi" in a["r"]
                                 for a in args[1:])
                if tuple_read or (pair and args[1].get("t") == "vcc"):
                    numeric_use |= 3
                else:
                    halves = {a.get("t") for a in args[1:]}
                    words = (1 if "vcc_lo" in halves else 0) | (2 if "vcc_hi" in halves else 0)
                    scalar_copy = op == "s_mov_b32" and args[0]["k"] == "sgpr"
                    if words and (scalar_copy or op.startswith(("v_dot", "s_add_u32", "v_min_f32"))):
                        numeric_use |= words
                    else:
                        unsafe |= 3

        raw = False
        for ins in block:
            op, args = _lane_masks.base_op(ins["op"]), ins["args"]
            _, may_write, kill = _scalar_rw(ins)
            if raw and op == "s_mov_b64" and args[1].get("t") == "vcc" and args[0]["k"] == "sgpr":
                ins["vcc_data_pair"] = True
            if may_write & vcc:
                raw = bool(kill & vcc and (
                    ins.get("vcc_data_pair") or (op.startswith(("s_load_", "s_buffer_load_"))
                                                 and (args[0]["k"] == "slist" or args[0].get("t") == "vcc"))))


def _postdominators(succ):
    """Immediate post-dominator `ipdom[b]` (Cooper-Harvey-Kennedy on reversed CFG with virtual exit `n`)."""
    n = len(succ)
    exit_ = n
    rev = [[] for _ in range(n + 1)]
    for b in range(n):
        if succ[b]:
            for s in succ[b]:
                rev[s].append(b)
        else:
            rev[exit_].append(b)
    # Iterative RPO from exit_ on reversed CFG.
    state = {exit_: 1}
    order = []
    stack = [(exit_, iter(rev[exit_]))]
    while stack:
        v, it = stack[-1]
        for w in it:
            if w not in state:
                state[w] = 1
                stack.append((w, iter(rev[w])))
                break
        else:
            state[v] = 2
            order.append(v)
            stack.pop()
    rpo = order[::-1]
    pos = {v: i for i, v in enumerate(rpo)}
    ipdom = {exit_: exit_}
    # Original successors (with [] -> [exit_]) are the predecessors in the reversed CFG.
    orig_succ = [list(s) if s else [exit_] for s in succ] + [[]]
    changed = True
    while changed:
        changed = False
        for v in rpo[1:]:
            new = None
            for p in orig_succ[v]:
                if p not in ipdom:
                    continue
                if new is None:
                    new = p
                    continue
                a, b = p, new
                while a != b:
                    while pos[a] > pos[b]:
                        a = ipdom[a]
                    while pos[b] > pos[a]:
                        b = ipdom[b]
                new = a
            if ipdom.get(v) != new:
                ipdom[v] = new
                changed = True
    return ipdom


def _holds_lane_mask(ins, x, lm_map):
    k = x.get("k")
    if k == "slist":
        regs = x.get("r", ())
        return bool(regs) and any(
            (item in lm_map) if isinstance(item, int)
            else (isinstance(item, str) and item.startswith("vcc") and "vcc" in lm_map)
            for item in regs
        )
    if k == "special":
        t = x.get("t", "")
        return t.startswith("exec") or (t.startswith("vcc") and "vcc" in lm_map)
    if k == "sgpr":
        n = x.get("n", 1)
        if n == 2:
            # Gen.holds_lane_mask takes a pair as a mask when one word is a mask and the other a constant 0 / -1
            # (s_mov_b32 s5, 0 after a v_cmp into s[4:5]). `lm_map` has no constants, so one mask word is enough
            # here; a pair with a data word too is then over-approximated, which only costs a fold (old #4480).
            return x["r"] in lm_map or x["r"] + 1 in lm_map
        return all(x["r"] + i in lm_map for i in range(n))
    return False


def _has_any_lane_mask_word(x, lm_map):
    """True if `x` is an SGPR operand with at least one register holding a lane mask."""
    return x.get("k") == "sgpr" and any(x["r"] + i in lm_map for i in range(x.get("n", 1)))


def _is_mask_op(ins, x, lm_map):
    """Matches Gen.is_mask_op in tools/rdna2spirv.py (special exec/vcc operands always take the mask path)."""
    if _holds_lane_mask(ins, x, lm_map):
        return True
    return _has_vcc_or_exec(x)


def _is_exec_op_arg(x):
    return x.get("k") == "special" and x.get("t", "") == "exec"


def _sgpr_pair(x):
    if x.get("k") == "sgpr" and x.get("n", 1) == 2:
        return x["r"]
    return None


def _x_memory_order_access(op):
    """Exact mirror of Gen.x_memory_order(op) in tools/rdna2spirv.py -> (kind, reads, writes) or None."""
    if op in ("ds_swizzle_b32", "ds_bpermute_b32", "ds_permute_b32", "ds_nop"):
        return None
    if op.startswith("ds_"):
        kind = "lds"
    elif op.startswith(("buffer_", "tbuffer_", "global_", "flat_", "image_load", "image_store", "image_atomic",
                        "s_load_", "s_buffer_load_")):
        kind = "mem"
    else:
        return None
    store = "store" in op or op.startswith("ds_write")
    reads = not store
    writes = store or "atomic" in op or (kind == "lds" and not op.startswith("ds_read"))
    return kind, reads, writes


def _vcc_pair_collects_mask(ins, masks):
    return ins.get("vcc_data_pair") and any(
        _holds_lane_mask(ins, x, masks.get(id(ins), {})) for x in ins["args"][1:])


def _classify_block_body(insns, lane_masks, scc_live, mask_writes):
    """Inspect non-terminator instructions of a block for lane-local region safety.

    Returns `(clean, mem_stats, may_write_scalars, exec_writes, sgpr_write_ops)` where:
      - `clean`: True if no instruction does a cross-lane op, barrier, or unmasked side effect.
      - `mem_stats`: `(mem_ops, mem_r, mem_w, lds_ops, lds_r, lds_w)` counting EXEC-masked memory/LDS ops.
      - `may_write_scalars`: int bitmask of scalar regs possibly written.
      - `exec_writes`: list of `(op, args)` that write EXEC.
      - `sgpr_write_ops`: list of `(sgpr_bitmask, op, args)` for every instruction writing any SGPR.
    """
    mem_ops = lds_ops = 0
    mem_r = mem_w = lds_r = lds_w = False
    may_w = 0
    exec_writes = []
    sgpr_write_ops = []
    meta_get = _OP_META_CACHE.get
    has_mask_writes = bool(mask_writes)
    has_scc_live = bool(scc_live)

    for ins in insns:
        raw_op = ins["op"]
        m = meta_get(raw_op)
        if m is None:
            m = _get_op_meta(raw_op)
        if m[1]:
            break
        if has_mask_writes and id(ins) in mask_writes:
            # write_all_lanes VGPR store (parked lane mask): runs even when EXEC == 0, so must not be skipped.
            return False, _EMPTY_MEM_STATS, 0, (), ()
        flags = m[8]
        # 1. Strictly forbidden opcodes inside a lane-local folded region (cross-lane, barrier, unmasked writes).
        if flags & _FLAG_FORBIDDEN_FOLD or _vcc_pair_collects_mask(ins, lane_masks):
            return False, _EMPTY_MEM_STATS, 0, (), ()

        acc = m[9]
        if acc is not None:
            kind, r, w = acc
            if kind == "lds":
                lds_ops += 1
                if r:
                    lds_r = True
                if w:
                    lds_w = True
            else:
                mem_ops += 1
                if r:
                    mem_r = True
                if w:
                    mem_w = True
        else:
            sacc = m[10]
            if sacc is not None:
                mem_ops += 1
                if sacc[0]:
                    mem_r = True
                if sacc[1]:
                    mem_w = True

        # 2. Cross-lane mask readers (`wave_mask`, `wave_equal`, live-SCC `scc_from_mask`).
        a = ins["args"]
        mask_chk = m[11]
        if mask_chk:
            if mask_chk == _MASK_CHK_SCC_VOTE:
                if has_scc_live and id(ins) in scc_live:
                    if (flags & _FLAG_SCC_VOTE_ALWAYS_MASK) or (bool(a) and _has_vcc_or_exec(a[0])):
                        return False, _EMPTY_MEM_STATS, 0, (), ()
                    lm = lane_masks.get(id(ins), {})
                    if any(_is_mask_op(ins, x, lm) for x in a[1:]):
                        return False, _EMPTY_MEM_STATS, 0, (), ()
            elif mask_chk == _MASK_CHK_WORD_READER:
                if len(a) > 1 and _holds_lane_mask(ins, a[1], lane_masks.get(id(ins), {})):
                    return False, _EMPTY_MEM_STATS, 0, (), ()
            elif mask_chk == _MASK_CHK_MBCNT:
                if len(a) > 1 and a[1].get("k") != "imm" and _holds_lane_mask(ins, a[1], lane_masks.get(id(ins), {})):
                    return False, _EMPTY_MEM_STATS, 0, (), ()
            else:  # _MASK_CHK_SCMP
                lm = lane_masks.get(id(ins), {})
                if any(_holds_lane_mask(ins, x, lm) or
                       (m[0] in ("s_cmp_eq_u64", "s_cmp_lg_u64") and _has_any_lane_mask_word(x, lm))
                       for x in a):
                    return False, _EMPTY_MEM_STATS, 0, (), ()

        # 3. Track scalar writes and EXEC writes.
        _, ins_mw, _ = _scalar_rw_with_meta(ins, m)
        may_w |= ins_mw
        s_bits = ins_mw & _ALL_SGPRS
        if s_bits:
            sgpr_write_ops.append((s_bits, m[0], a))
        if (flags & _FLAG_WRITES_EXEC_OP) or (bool(a) and a[0].get("k") == "special" and a[0].get("t", "").startswith("exec")):
            exec_writes.append((m[0], a))

    return True, (mem_ops, mem_r, mem_w, lds_ops, lds_r, lds_w), may_w, exec_writes, sgpr_write_ops


def _is_zero_val(x, zero_pairs):
    if _is_exec_op_arg(x):
        return True
    if x.get("k") == "imm" and x.get("v", 1) == 0:
        return True
    return _sgpr_pair(x) in zero_pairs


def _write_preserves_zero(op, a, zero_pairs, *, for_exec):
    """True iff the write produces 0 whenever EXEC == 0 and every pair in `zero_pairs` == 0."""
    if for_exec:
        if op.startswith("v_cmpx") or op in (
                "s_and_saveexec_b64", "s_and_saveexec_b32",
                "s_andn1_saveexec_b64", "s_andn1_saveexec_b32",
                "s_andn1_wrexec_b64", "s_andn1_wrexec_b32"):
            return True
        if op in ("s_andn2_saveexec_b64", "s_or_saveexec_b64", "s_xor_saveexec_b64", "s_andn2_wrexec_b64"):
            return len(a) >= 2 and _is_zero_val(a[1], zero_pairs)
        if not a or not _is_exec_op_arg(a[0]):
            return False  # partial exec_lo/exec_hi or unknown EXEC write
    else:
        if op.endswith("_saveexec_b64") or op == "s_andn1_wrexec_b64":
            return True  # saves old EXEC (or ~S0 & EXEC), both 0 when EXEC == 0
        if op == "s_andn2_wrexec_b64":
            return len(a) >= 2 and _is_zero_val(a[1], zero_pairs)
    if op == "s_and_b64" and len(a) == 3:
        return _is_zero_val(a[1], zero_pairs) or _is_zero_val(a[2], zero_pairs)
    if op == "s_andn2_b64" and len(a) == 3:
        return _is_zero_val(a[1], zero_pairs)
    if op in ("s_or_b64", "s_xor_b64") and len(a) == 3:
        return _is_zero_val(a[1], zero_pairs) and _is_zero_val(a[2], zero_pairs)
    if op == "s_mov_b64" and len(a) == 2:
        return _is_zero_val(a[1], zero_pairs)
    return False


def _exec_writes_stay_inactive(region_blocks, blk_info, run_live_in):
    """Return True iff every EXEC write in `region_blocks` preserves `active == False` for inactive lanes.

    Inductive proof over all execution steps in `region_blocks`: any SGPR pair `s[r:r+1]` not live-in at
    the region entry (`run_live_in`) cannot be read before being written in the region. If every write to
    `s[r:r+1]` and every write to `EXEC` produces 0 whenever `EXEC == 0` and all such pairs are 0, then
    an inactive lane stays inactive across arbitrary nested / reused saveexec if-else diamonds.
    """
    has_exec_write = False
    for u in region_blocks:
        if blk_info[u][3]:
            has_exec_write = True
            break
    if not has_exec_write:
        return True
    sgpr_writers = collections.defaultdict(list)
    for u in region_blocks:
        for item in blk_info[u][4]:
            s_bits = item[0]
            while s_bits:
                lsb = s_bits & -s_bits
                sgpr_writers[lsb.bit_length() - 1].append(item)
                s_bits ^= lsb
    zero_pairs = set()
    for r, w0 in sgpr_writers.items():
        if r >= 127 or ((3 << r) & run_live_in):
            continue  # entry value could be read before first write in R
        if w0 != sgpr_writers.get(r + 1):
            continue
        pair_mask = 3 << r
        if all(s_bits == pair_mask and bool(a) and _sgpr_pair(a[0]) == r for s_bits, _, a in w0):
            zero_pairs.add(r)
    changed = True
    while changed:
        changed = False
        for r in list(zero_pairs):
            if not all(_write_preserves_zero(op, a, zero_pairs, for_exec=False) for _, op, a in sgpr_writers[r]):
                zero_pairs.remove(r)
                changed = True
    for u in region_blocks:
        for op, a in blk_info[u][3]:
            if not _write_preserves_zero(op, a, zero_pairs, for_exec=True):
                return False
    return True


def _tree_has_loop(tree):
    """Iterative check whether a cfg_structure tree contains any loop node."""
    if not tree:
        return False
    stack = [tree]
    while stack:
        for it in stack.pop():
            if it[0] == "loop":
                return True
            if it[0] == "guard":
                stack.append(it[2])
    return False


def _blocks_in_tree(tree):
    """Iterative set of all block ids appearing in a cfg_structure tree (or single item)."""
    out = set()
    stack = [tree if isinstance(tree, list) else [tree]]
    while stack:
        for it in stack.pop():
            if it[0] == "block":
                out.add(it[1])
            else:
                stack.append(it[2])
    return out


def _map_tree_ids(tree, mapping):
    """Iterative copy of a cfg_structure tree replacing every compact id with `mapping[id]`."""
    out = []
    stack = [(tree, out)]
    while stack:
        src, dst = stack.pop()
        for it in src:
            if it[0] == "block":
                dst.append(("block", mapping[it[1]]))
            else:
                child = []
                dst.append((it[0], mapping[it[1]], child))
                stack.append((it[2], child))
    return out


def _drop_tree_block(tree, b):
    """Iterative copy of a cfg_structure tree without `("block", b)`, b being a sink with no successors: in
    `structure` its only node is `("guard", b, [("block", b)])`, which is dropped whole."""
    out = []
    stack = [(tree, out)]
    while stack:
        src, dst = stack.pop()
        for it in src:
            if it[0] == "block":
                if it[1] != b:
                    dst.append(it)
            elif it[0] == "guard" and it[2] == [("block", b)]:
                continue
            else:
                child = []
                dst.append((it[0], it[1], child))
                stack.append((it[2], child))
    return out


def _unwrap_known_pc_guards(tree, out_pcs, entry_pc=0):
    """Unwrap `("guard", g, body)` nodes whenever abstract interpretation proves `%pc == g` on all incoming paths."""
    return cfg_structure.unwrap_known_pc_guards(tree, out_pcs, entry_pc=entry_pc)


def _region_has_cycle(region, succ):
    """Iterative 3-color cycle check over the subgraph induced by `region`."""
    color = {}
    for start in region:
        if start in color:
            continue
        color[start] = 1
        stack = [(start, iter(succ[start]))]
        while stack:
            u, it = stack[-1]
            for v in it:
                if v not in region:
                    continue
                c = color.get(v, 0)
                if c == 1:
                    return True
                if c == 0:
                    color[v] = 1
                    stack.append((v, iter(succ[v])))
                    break
            else:
                color[u] = 2
                stack.pop()
    return False


_TID_EQ = frozenset(f"v_cmp{x}_eq_{t}32" for x in ("", "x") for t in ("i", "u"))
_VGPR_READ_ONLY = ("exp", "buffer_store", "tbuffer_store", "image_store", "global_store", "flat_store", "scratch_store",
                   "ds_write", "ds_store", "v_cmp", "v_readlane", "v_readfirstlane")


def _one_key(x, cmp_dest=False):
    """The mask an operand names for `_one_lane_in`: "exec", "vcc" or the first SGPR of a pair (else None)."""
    t = x.get("t") if x.get("k") == "special" else None
    if t == "exec" or t == "vcc" or (cmp_dest and t == "vcc_lo"):
        return "vcc" if t.startswith("vcc") else t
    return _sgpr_pair(x)


def _one_lane_step(ins, st):
    """`_one_lane_in` across one instruction: `st` = (VGPRs still the thread id, masks with at most one lane set)."""
    tid, one = st
    meta = _get_op_meta(ins["op"])
    op, a = meta[0], ins["args"]
    if meta[1]:
        return st
    if op.startswith(("v_movrel", "v_swaprel")):
        tid = frozenset()
    elif a and a[0].get("k") == "vgpr" and not op.startswith(_VGPR_READ_ONLY):
        # A memory result can be one register longer than its operand says (tfe); a swap writes both operands.
        vdst = a[:2] if op.startswith("v_swap") else a[:1]
        tid = tid - {x["r"] + i for x in vdst if x.get("k") == "vgpr"
                     for i in range(x.get("n", 1) + (not op.startswith("v_")))}
    may_w = _scalar_rw_with_meta(ins, meta)[1]
    writes_exec = bool(meta[8] & _FLAG_WRITES_EXEC_OP) or (
        bool(a) and a[0].get("k") == "special" and a[0].get("t", "").startswith("exec"))
    if not writes_exec and not may_w & (_ALL_SGPRS | 1 << _BIT_VCC):
        return tid, one

    def is_one(x):
        k = _one_key(x)
        return k is not None and k in one

    dest, val, exec_val = None, False, False
    if op.startswith("v_cmp"):
        # A compare writes 0 in every inactive lane, so its result is within EXEC; `== c` of the thread id is true
        # in at most one lane of the wave.
        val = "exec" in one
        if op in _TID_EQ and not ins.get("mods") and not ins["op"].endswith("_sdwa") and len(a) >= 2:
            x, y = a[-2:]
            if x.get("k") == "vgpr":
                x, y = y, x
            val = val or (x.get("k") == "imm" and isinstance(x.get("v"), int) and y.get("k") == "vgpr"
                          and y.get("n", 1) == 1 and y["r"] in tid)
        exec_val = val
        if not op.startswith("v_cmpx"):
            dest = "vcc" if len(a) == 2 else _one_key(a[0], cmp_dest=True)
    elif op in ("s_and_saveexec_b64", "s_andn2_saveexec_b64") and len(a) == 2:
        dest, val = _one_key(a[0]), "exec" in one            # the old EXEC
        exec_val = is_one(a[1]) or (op == "s_and_saveexec_b64" and "exec" in one)
    elif op in ("s_mov_b64", "s_and_b64", "s_andn2_b64") and len(a) >= 2:
        # The result is within the first source (and within the second for s_and_b64).
        val = is_one(a[1]) or (op == "s_and_b64" and len(a) > 2 and is_one(a[2]))
        dest = _one_key(a[0])
        exec_val = val
    if any(x.get("k") == "special" and x.get("t") in ("exec_lo", "exec_hi") for x in a[:2]):
        # One half of EXEC (a compare into exec_lo, s_mov_b32 exec_hi, a carry out): the other half keeps its lanes,
        # so EXEC stays at most one lane only if it already was and the half written is within it (a compare).
        exec_val = exec_val and "exec" in one
    out = {k for k in one if k != "exec" and not may_w & (1 << _BIT_VCC if k == "vcc" else 3 << k)}
    if not writes_exec and "exec" in one:
        out.add("exec")
    if writes_exec and exec_val:
        out.add("exec")
    if dest not in (None, "exec") and val:
        out.add(dest)
    return tid, frozenset(out)


def _one_lane_in(active_blocks, succ, tid):
    """Per block, whether EXEC is certainly at most one lane of the wave on entry (forward must-analysis, merge =
    intersection). `tid`: VGPRs whose entry value no two lanes of a wave share (the thread id); one holds that
    value until it is written. `v_cmp(x)_eq_{i,u}32 <constant>, <that VGPR>` is true in at most one lane, and a
    mask stays at most one lane through a copy, an AND with anything, `s_andn2_b64` of it and a saveexec of it."""
    n = len(active_blocks)
    state_in = [None] * n
    state_in[0] = (frozenset(tid), frozenset())
    work = collections.deque([0])
    while work:
        b = work.popleft()
        st = state_in[b]
        for ins in active_blocks[b]:
            st = _one_lane_step(ins, st)
        for v in succ[b]:
            old = state_in[v]
            new = st if old is None else (old[0] & st[0], old[1] & st[1])
            if new != old:
                state_in[v] = new
                work.append(v)
    return [s is not None and "exec" in s[1] for s in state_in]


def find_local_exec_folds(blocks, raw_edges, succ, lane_masks, scc_live, mask_writes=frozenset(), tid=frozenset()):
    """Find safe lane-local `s_cbranch_execz`/`nz` regions to fold inline under --wave64-emulate.

    Allows read-only regions (including regions with internal scalar-counted loops) AND acyclic
    single-write-per-domain regions (at most one store/atomic touching `"mem"` or `"lds"` with no
    other access to that domain inside the region).

    Returns `(folds, fold_chain, absorbed)`:
      - `folds`: dict `head_block -> {"run": int, "skip": int, "region": frozenset[int],
                 "order": list[int], "sub_tree": list | None, "mem_reads": frozenset[str],
                 "mem_writes": frozenset[str], "mem_stats": tuple, "one_lane": bool}`
                 containing only the maximal (outermost) folded regions.
      - `fold_chain`: dict `head_block -> skip_block` when `skip_block` has no predecessors outside
                 `{head_block} | region` and can be emitted directly after the fold in the same block.
      - `absorbed`: set of all block indices emitted inline (inside a folded region or chained as `skip`).
    """
    n = len(blocks)
    if n < 2:
        return {}, {}, set()

    active_blocks = [_active_insns(blk) for blk in blocks]
    preds = [set() for _ in range(n)]
    for b in range(n):
        for s in succ[b]:
            preds[s].add(b)

    ipdom = _postdominators(succ)

    # Single fused pass over active_blocks: compute scalar liveness (use/kill),
    # block-body classification (blk_info), and block terminators (terms).
    use = [0] * n
    kill = [0] * n
    blk_info = [None] * n
    terms = [None] * n
    meta_get = _OP_META_CACHE.get
    has_mask_writes = bool(mask_writes)
    has_scc_live = bool(scc_live)

    for b, insns in enumerate(active_blocks):
        u_mask = 0
        k_mask = 0
        clean = True
        mem_ops = lds_ops = 0
        mem_r = mem_w = lds_r = lds_w = False
        may_w = 0
        exec_writes = []
        sgpr_write_ops = []
        first_lane = False
        term = ("fall", None)

        for ins in insns:
            raw_op = ins["op"]
            m = meta_get(raw_op)
            if m is None:
                m = _get_op_meta(raw_op)
            rd, ins_mw, mk = _scalar_rw_with_meta(ins, m)
            u_mask |= rd & ~k_mask
            k_mask |= mk
            if m[1]:
                op = m[0]
                term = ("end", None) if op == "s_endpgm" else ("br", op)
                break
            if not clean:
                continue
            if has_mask_writes and id(ins) in mask_writes:
                clean = False
                continue
            flags = m[8]
            # A v_readfirstlane_b32 only in a fold that at most one lane runs and that leaves EXEC alone (below).
            rfl = bool(tid) and m[0] == "v_readfirstlane_b32"
            if (flags & _FLAG_FORBIDDEN_FOLD and not rfl) or _vcc_pair_collects_mask(ins, lane_masks):
                clean = False
                continue
            first_lane = first_lane or rfl
            acc = m[9]
            if acc is not None:
                kind, r, w = acc
                if kind == "lds":
                    lds_ops += 1
                    if r:
                        lds_r = True
                    if w:
                        lds_w = True
                else:
                    mem_ops += 1
                    if r:
                        mem_r = True
                    if w:
                        mem_w = True
            else:
                sacc = m[10]
                if sacc is not None:
                    mem_ops += 1
                    if sacc[0]:
                        mem_r = True
                    if sacc[1]:
                        mem_w = True

            a = ins["args"]
            mask_chk = m[11]
            if mask_chk:
                if mask_chk == _MASK_CHK_SCC_VOTE:
                    if has_scc_live and id(ins) in scc_live:
                        if (flags & _FLAG_SCC_VOTE_ALWAYS_MASK) or (bool(a) and _has_vcc_or_exec(a[0])):
                            clean = False
                            continue
                        lm = lane_masks.get(id(ins), {})
                        if any(_is_mask_op(ins, x, lm) for x in a[1:]):
                            clean = False
                            continue
                elif mask_chk == _MASK_CHK_WORD_READER:
                    if len(a) > 1 and _holds_lane_mask(ins, a[1], lane_masks.get(id(ins), {})):
                        clean = False
                        continue
                elif mask_chk == _MASK_CHK_MBCNT:
                    if len(a) > 1 and a[1].get("k") != "imm" and _holds_lane_mask(ins, a[1], lane_masks.get(id(ins), {})):
                        clean = False
                        continue
                else:  # _MASK_CHK_SCMP
                    lm = lane_masks.get(id(ins), {})
                    if any(_holds_lane_mask(ins, x, lm) or
                           (m[0] in ("s_cmp_eq_u64", "s_cmp_lg_u64") and _has_any_lane_mask_word(x, lm))
                           for x in a):
                        clean = False
                        continue

            may_w |= ins_mw
            s_bits = ins_mw & _ALL_SGPRS
            if s_bits:
                sgpr_write_ops.append((s_bits, m[0], a))
            if (flags & _FLAG_WRITES_EXEC_OP) or (bool(a) and a[0].get("k") == "special" and a[0].get("t", "").startswith("exec")):
                exec_writes.append((m[0], a))

        use[b] = u_mask
        kill[b] = k_mask
        terms[b] = term
        if clean:
            blk_info[b] = (True, (mem_ops, mem_r, mem_w, lds_ops, lds_r, lds_w), may_w, exec_writes, sgpr_write_ops,
                           first_lane)
        else:
            blk_info[b] = (False, _EMPTY_MEM_STATS, 0, (), (), False)

    scalar_live_in = _solve_scalar_liveness(use, kill, preds)
    one_lane_in = None

    # Gather candidate s_cbranch_execz / s_cbranch_execnz blocks with single-entry/single-exit post-dominated regions.
    candidates = []
    for b in range(n):
        kind, op = terms[b]
        if kind != "br" or op not in ("s_cbranch_execz", "s_cbranch_execnz"):
            continue
        edges = raw_edges[b]
        if len(edges) < 2:
            continue
        tgt, fall = edges[0], edges[1]
        run, skip = (fall, tgt) if op == "s_cbranch_execz" else (tgt, fall)
        if run == skip or skip >= n or ipdom.get(b) != skip:
            continue
        # Collect region R reachable from `run` stopping at `skip`.
        region = set()
        work = [run]
        ok = True
        while work:
            u = work.pop()
            if u == skip or u in region:
                continue
            if (u == b or u == 0 or not succ[u] or not blk_info[u][0]
                    or (len(raw_edges[u]) == 1 and terms[u][0] == "br" and terms[u][1] != "s_branch")):
                ok = False
                break
            region.add(u)
            for s in succ[u]:
                if s != skip and s not in region:
                    work.append(s)
        if not ok or not region:
            continue
        # Single-entry check: no external edge into any block of R except B -> run.
        for u in region:
            allowed_ext = {b} if u == run else set()
            if not preds[u] <= (region | allowed_ext):
                ok = False
                break
        if not ok:
            continue
        # Memory-write safety: if R writes "mem" (or "lds"), R must be acyclic and touch that domain at most once.
        mem_ops = lds_ops = 0
        mem_r = mem_w = lds_r = lds_w = False
        for u in region:
            mo, mr, mw, lo, lr, lw = blk_info[u][1]
            mem_ops += mo
            mem_r = mem_r or mr
            mem_w = mem_w or mw
            lds_ops += lo
            lds_r = lds_r or lr
            lds_w = lds_w or lw
        # With at most one lane in EXEC (kept so by the EXEC check below) the lane's own order of its accesses is
        # the wave's, and v_readfirstlane_b32 reads the lane's own value as long as the region leaves EXEC alone.
        first_lane = any(blk_info[u][5] for u in region)
        one = (first_lane or (mem_w and mem_ops > 1) or (lds_w and lds_ops > 1)
               or ((mem_w or lds_w) and _region_has_cycle(region, succ)))
        if one:
            if one_lane_in is None:
                one_lane_in = _one_lane_in(active_blocks, succ, tid)
            if not one_lane_in[run] or (first_lane and any(blk_info[u][3] for u in region)):
                continue
        # No scalar written in R may be live-in at `skip`.
        region_may_w = 0
        for u in region:
            region_may_w |= blk_info[u][2]
        if region_may_w & scalar_live_in[skip]:
            continue
        # EXEC must stay inactive for any lane that skipped R.
        if not _exec_writes_stay_inactive(region, blk_info, scalar_live_in[run]):
            continue
        candidates.append((len(region), b, run, skip, frozenset(region),
                           (mem_ops, mem_r, mem_w, lds_ops, lds_r, lds_w), bool(one)))

    # Validate innermost regions first so nested s_cbranch_execz/nz inside an outer region are already verified.
    candidates.sort(key=lambda item: (item[0], item[1]))
    valid = {}
    for _, b, run, skip, region, mem_stats, one in candidates:
        ok = True
        for u in region:
            tkind, top = terms[u]
            if tkind == "end":
                ok = False
                break
            if tkind == "br":
                if top in ("s_cbranch_vccz", "s_cbranch_vccnz"):
                    ok = False
                    break
                if top in ("s_cbranch_execz", "s_cbranch_execnz"):
                    if u not in valid or valid[u]["skip"] not in (region | {skip}):
                        ok = False
                        break
        if not ok:
            continue
        # Build local sub-CFG and verify reducibility (multi-block regions and a one-block loop use
        # cfg_structure.structure; emitted straight, a block that branches back to itself would run once).
        if len(region) == 1 and run not in succ[run]:
            order = [run]
            sub_tree = None
        else:
            order = [run] + sorted(region - {run})
            idx_of = {u: i for i, u in enumerate(order)}
            sub_end = len(order)
            # An edge to `skip` (e.g. an s_cbranch_scc to the join) goes to an exit node `sub_end`, so the blocks
            # after it in the region get a `%pc` guard; dropping the edge would run them on the exit path too.
            sub_succ = [list(dict.fromkeys(idx_of.get(s, sub_end) for s in succ[u])) for u in order] + [[]]
            sub_tree = cfg_structure.structure(sub_succ)
            if sub_tree is None:
                continue
            sub_tree = _drop_tree_block(sub_tree, sub_end)
            sub_out_pcs = {}
            for i, u in enumerate(order):
                uedges = raw_edges[u]
                tkind, top = terms[u]
                if tkind in ("fall", "end"):
                    s = {idx_of.get(uedges[0], sub_end) if (tkind == "fall" and uedges) else sub_end}
                elif top == "s_branch":
                    s = {idx_of.get(uedges[0], sub_end)}
                else:
                    s = {idx_of.get(uedges[0], sub_end),
                         idx_of.get(uedges[1], sub_end) if len(uedges) > 1 else sub_end}
                sub_out_pcs[i] = frozenset(s)
            sub_tree = _unwrap_known_pc_guards(sub_tree, sub_out_pcs, 0)
        _, mem_r, mem_w, _, lds_r, lds_w = mem_stats
        mem_reads = frozenset(k for k, flag in (("mem", mem_r), ("lds", lds_r)) if flag)
        mem_writes = frozenset(k for k, flag in (("mem", mem_w), ("lds", lds_w)) if flag)
        valid[b] = {
            "run": run,
            "skip": skip,
            "region": region,
            "order": order,
            "sub_tree": sub_tree,
            "mem_reads": mem_reads,
            "mem_writes": mem_writes,
            "mem_stats": mem_stats,
            "one_lane": one,    # at most one lane runs it: no order between lanes, v_readfirstlane_b32 is a copy
        }

    # Select maximal (non-overlapping outermost) folded regions.
    selected = {}
    in_region = set()
    for _, b, _, _, _ in sorted(
            ((len(v["region"]), k, v["run"], v["skip"], v["region"]) for k, v in valid.items()),
            key=lambda x: (-x[0], x[1])):
        fd = valid[b]
        if b in in_region or (fd["region"] & in_region):
            continue
        selected[b] = fd
        in_region.update(fd["region"])

    # Chain single-predecessor `skip` continuations directly after their fold (cycle-free by construction).
    fold_chain = {}
    chained = set()
    parent_chain = {}

    def chain_root(x):
        while x in parent_chain:
            x = parent_chain[x]
        return x

    for b in sorted(selected):
        fd = selected[b]
        skip = fd["skip"]
        if skip == 0 or skip in in_region or skip in chained:
            continue
        if not preds[skip] <= ({b} | fd["region"]):
            continue
        if chain_root(skip) == chain_root(b):
            continue
        fold_chain[b] = skip
        parent_chain[skip] = b
        chained.add(skip)

    absorbed = in_region | chained
    return selected, fold_chain, absorbed


def _is_uniform_scc_safe(active_blocks, lane_masks, scc_live):
    """True iff no instruction triggers wave64's per-lane 32-bit half-mask SCC write (rdna2spirv.py scc_logic)
    with live SCC. When True, %scc is guaranteed identical across all 64 lanes of each wave."""
    if not scc_live:
        return True
    meta_get = _OP_META_CACHE.get
    for insns in active_blocks:
        for ins in insns:
            ins_id = id(ins)
            if ins_id not in scc_live:
                continue
            raw_op = ins["op"]
            m = meta_get(raw_op)
            if m is None:
                m = _get_op_meta(raw_op)
            if not (m[8] & _FLAG_SOP2_B32_LOGIC):
                continue
            a = ins["args"]
            lm = lane_masks.get(ins_id, {})
            writes_exec = bool(a) and a[0].get("k") == "special" and a[0].get("t", "").startswith("exec")
            if writes_exec or any(_holds_lane_mask(ins, x, lm) for x in a[1:]):
                return False
    return True


def _insn_may_barrier_with_meta(ins, meta, lane_masks, scc_live):
    if meta[1]:
        return False
    flags = meta[8]
    if flags & _FLAG_ALWAYS_BARRIER or _vcc_pair_collects_mask(ins, lane_masks):
        return True
    mask_chk = meta[11]
    if not mask_chk:
        return False
    a = ins["args"]
    if mask_chk == _MASK_CHK_SCC_VOTE:
        if not scc_live or id(ins) not in scc_live:
            return False
        if (flags & _FLAG_SCC_VOTE_ALWAYS_MASK) or (bool(a) and _has_vcc_or_exec(a[0])):
            return True
        lm = lane_masks.get(id(ins), {})
        return any(_is_mask_op(ins, x, lm) for x in a[1:])
    if mask_chk == _MASK_CHK_WORD_READER:
        return len(a) > 1 and _holds_lane_mask(ins, a[1], lane_masks.get(id(ins), {}))
    if mask_chk == _MASK_CHK_MBCNT:
        return len(a) > 1 and a[1].get("k") != "imm" and _holds_lane_mask(ins, a[1], lane_masks.get(id(ins), {}))
    lm = lane_masks.get(id(ins), {})
    return any(_holds_lane_mask(ins, x, lm) or
               (meta[0] in ("s_cmp_eq_u64", "s_cmp_lg_u64") and _has_any_lane_mask_word(x, lm))
               for x in a)


def _insn_may_barrier(ins, lane_masks, scc_live):
    """Conservative (over-approximate) check: True if `ins` may emit x_round or OpControlBarrier in --wave64-emulate."""
    return _insn_may_barrier_with_meta(ins, _get_op_meta(ins["op"]), lane_masks, scc_live)


def _dirty_conflicts(dirty, reads, writes):
    return (any((k, "w") in dirty for k in reads)
            or any((k, "r") in dirty or (k, "w") in dirty for k in writes))


def plan_structured_emulation(blocks, raw_edges, succ, folds, fold_chain, absorbed, lane_masks, scc_live, x_lanes):
    """Plan structured --wave64-emulate emission eliminating the giant global round-loop OpSwitch.

    - Single-wave workgroups (`x_lanes == 64`): every reducible CFG emits 100% inline structured SPIR-V
      with direct workgroup barriers (`0` OpSwitch cases, `0` x_spill variables, `0` scheduler atomics).
    - Multi-wave workgroups (`x_lanes > 64`): decomposes top-level post-dominating spine blocks and
      barrier-free guards/loops (including live-out scalars, single-write stores, EXEC-widening, and
      read-only loops) into inline structured SPIR-V, isolating any remaining wave-divergent barrier
      regions into tiny local mini-schedulers instead of one global K_{1,N} switch clique.

    Returns `None` when the shader already collapsed to 1 superblock, is irreducible, uses non-uniform
    32-bit half-mask SCC, or (multi-wave) has a divergent `s_barrier` or no decomposition gain.
    """
    n = len(blocks)
    eff_blocks = [b for b in range(n) if b not in absorbed]
    if len(eff_blocks) < 2:
        return None  # straight-line or fully folded to 1 superblock: existing x_scheduler already has <=1 case

    active_blocks = [_active_insns(blk) for blk in blocks]
    if not _is_uniform_scc_safe(active_blocks, lane_masks, scc_live):
        return None

    # Build superblock chains and effective post-fold CFG over `eff_blocks`.
    eff_idx = {b: i for i, b in enumerate(eff_blocks)}
    chain_of = {}
    eff_succ = {}
    eff_succ_compact = []
    for b in eff_blocks:
        chain = []
        cur = b
        while True:
            chain.append(cur)
            if cur in folds and cur in fold_chain:
                cur = fold_chain[cur]
                continue
            break
        chain_of[b] = chain
        tail = chain[-1]
        if tail in folds:
            nxt = [folds[tail]["skip"]]
        else:
            nxt = succ[tail]
        if any(s not in eff_idx for s in nxt):
            return None  # defensive: every effective successor must be a non-absorbed block
        eff_succ[b] = nxt
        eff_succ_compact.append([eff_idx[s] for s in nxt])

    out_pcs = {}
    for b in eff_blocks:
        tail = chain_of[b][-1]
        s = set(eff_succ[b])
        if not s:
            s.add(cfg_structure.END)
        elif tail not in folds and len(raw_edges[tail]) == 1:
            last = active_blocks[tail][-1] if active_blocks[tail] else None
            op = base_op(last["op"]) if last is not None else None
            if op in BRANCHES and op != "s_branch":
                s.add(cfg_structure.END)
        out_pcs[b] = frozenset(s)
    compact_out_pcs = {
        eff_idx[b]: frozenset(eff_idx[p] if p != cfg_structure.END else cfg_structure.END for p in pcs)
        for b, pcs in out_pcs.items()
    }
    compact_tree = cfg_structure.structure(eff_succ_compact, out_pcs=compact_out_pcs)
    if compact_tree is None:
        return None
    tree = _map_tree_ids(compact_tree, eff_blocks)
    tree = _unwrap_known_pc_guards(tree, out_pcs, eff_blocks[0])

    # Precompute per-superblock instruction summaries (for dirty transfer, sync-free check, s_barrier, s_endpgm).
    sb_has_sbarrier = {}
    sb_has_endpgm = {}
    sb_may_barrier = {}
    sb_mem_stats = {}
    sb_clears_dirty = {}
    sb_gen_dirty = {}
    meta_get = _OP_META_CACHE.get

    for b in eff_blocks:
        has_sb = False
        has_end = False
        may_bar = False
        mem_ops = lds_ops = 0
        mem_r = mem_w = lds_r = lds_w = False
        clears_dirty = False
        gen_dirty = 0
        chain = chain_of[b]
        last_c = len(chain) - 1
        for idx_c, cur in enumerate(chain):
            insns = active_blocks[cur]
            for ins in insns:
                raw_op = ins["op"]
                m = meta_get(raw_op)
                if m is None:
                    m = _get_op_meta(raw_op)
                op = m[0]
                if m[1]:
                    if op == "s_endpgm":
                        has_end = True
                    elif idx_c == last_c and cur not in folds and (m[8] & _FLAG_VOTED_BRANCH):
                        may_bar = True
                        clears_dirty = True
                        gen_dirty = 0
                    break
                if op == "s_barrier":
                    has_sb = True
                    may_bar = True
                    clears_dirty = True
                    gen_dirty = 0
                    continue
                if not may_bar and _insn_may_barrier_with_meta(ins, m, lane_masks, scc_live):
                    may_bar = True
                acc = m[9]
                if acc is not None:
                    kind, r, w = acc
                    if kind == "lds":
                        lds_ops += 1
                        if r:
                            lds_r = True
                        if w:
                            lds_w = True
                    else:
                        mem_ops += 1
                        if r:
                            mem_r = True
                        if w:
                            mem_w = True
                    gen_dirty |= m[12]
                else:
                    sacc = m[10]
                    if sacc is not None:
                        mem_ops += 1
                        if sacc[0]:
                            mem_r = True
                        if sacc[1]:
                            mem_w = True
            if cur in folds:
                fd = folds[cur]
                mo, mr, mw, lo, lr, lw = fd["mem_stats"]
                mem_ops += mo
                if mr:
                    mem_r = True
                if mw:
                    mem_w = True
                lds_ops += lo
                if lr:
                    lds_r = True
                if lw:
                    lds_w = True
                for k in fd["mem_reads"]:
                    gen_dirty |= _DIRTY_LDS_R if k == "lds" else _DIRTY_MEM_R
                for k in fd["mem_writes"]:
                    gen_dirty |= _DIRTY_LDS_W if k == "lds" else _DIRTY_MEM_W
        sb_has_sbarrier[b] = has_sb
        sb_has_endpgm[b] = has_end
        sb_may_barrier[b] = may_bar
        sb_mem_stats[b] = (mem_ops, mem_r, mem_w, lds_ops, lds_r, lds_w)
        sb_clears_dirty[b] = clears_dirty
        sb_gen_dirty[b] = gen_dirty

    # ---- Mode 1: Single-wave (`x_lanes == 64`) -> 100% inline structured SPIR-V, 0 OpSwitch ----
    if x_lanes == 64:
        dirty_in = {b: 0 for b in eff_blocks}
        work = collections.deque(eff_blocks)
        in_work = set(eff_blocks)
        while work:
            u = work.popleft()
            in_work.discard(u)
            d_out = sb_gen_dirty[u] if sb_clears_dirty[u] else (dirty_in[u] | sb_gen_dirty[u])
            if d_out:
                for v in eff_succ[u]:
                    old_v = dirty_in[v]
                    new_v = old_v | d_out
                    if new_v != old_v:
                        dirty_in[v] = new_v
                        if v not in in_work:
                            in_work.add(v)
                            work.append(v)
        return {
            "mode": "inline_all",
            "tree": tree,
            "dirty_in": {b: _DIRTY_MASK_TO_FROZENSET[d] for b, d in dirty_in.items()},
            "inline_blocks": set(eff_blocks),
            "sync_free_blocks": set(),
            "mini_of_block": {},
            "mini_blocks": {},
            "tail_blocks": set(),
        }

    # ---- Mode 2: Multi-wave (`x_lanes > 64`) -> top-level uniform inline + sync-free + local mini-schedulers ----
    ipdom_c = _postdominators(eff_succ_compact)
    m_eff = len(eff_blocks)
    # Require every effective block to reach exit (no infinite-loop sink SCCs bypassing post-dominators).
    entry_pdoms = set()
    if len(ipdom_c) == m_eff + 1 and 0 in ipdom_c:
        cur_c = 0
        while cur_c != m_eff:
            entry_pdoms.add(eff_blocks[cur_c])
            nxt_c = ipdom_c.get(cur_c)
            if nxt_c is None or nxt_c == cur_c:
                break
            cur_c = nxt_c

    eff_preds = {b: set() for b in eff_blocks}
    for u in eff_blocks:
        for v in eff_succ[u]:
            eff_preds[v].add(u)

    # Unwrap top-level guards whose entry strictly post-dominates entry 0 and whose non-back-edge
    # predecessors have all been emitted, exposing the full post-dominating spine at top level.
    flat_top = []
    q = collections.deque(tree)
    seen_before = set()
    saw_endpgm = False
    while q:
        it = q.popleft()
        if not saw_endpgm and it[0] == "guard":
            entry_b = it[1]
            sub = it[2]
            if entry_b in entry_pdoms and sub:
                first_kind = sub[0][0]
                if first_kind == "block" and sub[0][1] == entry_b:
                    need_preds = eff_preds[entry_b]
                elif first_kind == "loop" and sub[0][1] == entry_b:
                    loop_nodes = _blocks_in_tree(sub[0])
                    need_preds = eff_preds[entry_b] - loop_nodes
                else:
                    need_preds = None
                if need_preds is not None and need_preds <= seen_before:
                    for child in reversed(sub):
                        q.appendleft(child)
                    continue
        flat_top.append(it)
        it_blocks = _blocks_in_tree(it)
        seen_before.update(it_blocks)
        if any(sb_has_endpgm[u] for u in it_blocks):
            saw_endpgm = True

    def check_sync_free(it):
        sub_blocks = _blocks_in_tree(it)
        has_loop = _tree_has_loop([it])
        mem_ops = lds_ops = 0
        mem_r = mem_w = lds_r = lds_w = False
        for u in sub_blocks:
            if sb_may_barrier[u] or sb_has_endpgm[u] or sb_has_sbarrier[u]:
                return False, frozenset(), frozenset()
            mo, mr, mw, lo, lr, lw = sb_mem_stats[u]
            mem_ops += mo
            mem_r = mem_r or mr
            mem_w = mem_w or mw
            lds_ops += lo
            lds_r = lds_r or lr
            lds_w = lds_w or lw
        if has_loop:
            if mem_w or lds_w:
                return False, frozenset(), frozenset()
        else:
            if (mem_w and mem_ops > 1) or (lds_w and lds_ops > 1):
                return False, frozenset(), frozenset()
        reads = frozenset(k for k, f in (("mem", mem_r), ("lds", lds_r)) if f)
        writes = frozenset(k for k, f in (("mem", mem_w), ("lds", lds_w)) if f)
        return True, reads, writes

    steps = []
    dirty_in = {}
    inline_blocks = set()
    sync_free_blocks = set()
    mini_of_block = {}
    mini_blocks = {}
    tail_blocks = set()
    cur_dirty = 0
    mini_idx = 0

    for idx_it, it in enumerate(flat_top):
        it_blocks = _blocks_in_tree(it)
        has_end = any(sb_has_endpgm[u] for u in it_blocks)
        if it[0] == "block" and (not has_end or idx_it == len(flat_top) - 1):
            b = it[1]
            dirty_in[b] = _DIRTY_MASK_TO_FROZENSET[cur_dirty]
            inline_blocks.add(b)
            cur_dirty = sb_gen_dirty[b] if sb_clears_dirty[b] else (cur_dirty | sb_gen_dirty[b])
            steps.append(("inline_block", b))
            continue
        # An `s_barrier` under a guard or in a loop: waves on different paths meet at different sites
        # (cs_barrier_divergent), which only the round scheduler models. Every wave enters the tail in the same
        # round, and what ran before it has no `s_barrier` that is not reached by the whole workgroup.
        if has_end or (it[0] != "block" and any(sb_has_sbarrier[u] for u in it_blocks)):
            # Early s_endpgm inside a wave-divergent guard/loop (or non-final block): hand remaining tail to x_scheduler.
            rem = set()
            for rem_it in flat_top[idx_it:]:
                rem.update(_blocks_in_tree(rem_it))
            tail_blocks = rem
            steps.append(("tail_scheduler", rem, bool(cur_dirty)))
            cur_dirty = 0
            break
        ok_sf, sf_reads, sf_writes = check_sync_free(it)
        if ok_sf:
            pre_bar = _dirty_conflicts(_DIRTY_MASK_TO_FROZENSET[cur_dirty], sf_reads, sf_writes)
            if pre_bar:
                cur_dirty = 0
            for u in it_blocks:
                dirty_in[u] = frozenset()  # pre_bar cleared conflicts before entering sync-free region
                inline_blocks.add(u)
                sync_free_blocks.add(u)
            for k in sf_reads:
                cur_dirty |= _DIRTY_LDS_R if k == "lds" else _DIRTY_MEM_R
            for k in sf_writes:
                cur_dirty |= _DIRTY_LDS_W if k == "lds" else _DIRTY_MEM_W
            steps.append(("inline_sync_free", it, pre_bar))
        else:
            pre_bar = bool(cur_dirty)
            cur_dirty = 0  # mini-scheduler ends with OpControlBarrier %c2 %c2 %c948
            mini_blocks[mini_idx] = it_blocks
            for u in it_blocks:
                mini_of_block[u] = mini_idx
            steps.append(("mini_scheduler", mini_idx, it_blocks, pre_bar))
            mini_idx += 1

    # Only engage structured_hybrid when it actually eliminates global-switch blocks or splits into smaller regions.
    if len(steps) == 1 and steps[0][0] in ("tail_scheduler", "mini_scheduler"):
        return None
    return {
        "mode": "structured_hybrid",
        "steps": steps,
        "dirty_in": dirty_in,
        "inline_blocks": inline_blocks,
        "sync_free_blocks": sync_free_blocks,
        "mini_of_block": mini_of_block,
        "mini_blocks": mini_blocks,
        "tail_blocks": tail_blocks,
    }
