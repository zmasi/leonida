r"""Differential fuzzer for the RDNA2 packed integer dot products (standard library only).

Covers twelve opcodes, one family (the "Packed integer dot products", "Signed-byte dot products" and
`v_dot2c_i32_i16`, `v_dot8c_i32_i4` rows of docs/design.md's contract table):

    v_dot2_i32_i16  v_dot2_u32_u16  v_dot4_i32_i8  v_dot4_u32_u8  v_dot8_i32_i4  v_dot8_u32_u4   VOP3P, acc = S2
    v_dot4c_i32_i8                                                                               VOP2,  acc = old D
    v_dot2c_i32_i16 v_dot2c_u32_u16 v_dot4c_u32_u8 v_dot8c_i32_i4 v_dot8c_u32_u4                 VOP2/VOP3, acc = old D

The first seven are the gfx103x encodings (docs/design.md, the GFX10.1/GFX10.3 opcode row). The last five are the
gfx908 `VOP2Instructions.td` forms the translator also accepts (not in the gfx1013/gfx103x feature sets); they run
by default because the translator ships them, and `--ops` can leave them out.

Seeded straight-line programs run through two independent oracles, and the four exported mrt0 words are compared
bit for bit (all integer, so a mismatch is never a rounding convention):

  * the reference below, written from the RDNA2 ISA with Python integers: D = sum(S0.elem[k] * S1.elem[k]) + acc,
    the exact sum taken modulo 2^32, or with `clamp` saturated to [-2^31, 2^31-1] (signed ops) or [0, 2^32-1]
    (unsigned ops). It shares no code with the translator. tests/test_dot_diff.py checks it against the words
    measured on RDNA2 hardware (gfx1036) in tests/semantic/test_alu_dot4_i8.py, and
  * tools/rdna2spirv.py, executing the SPIR-V with tests/semantic/spirv_mini_interp.py.

Program shape: seeds in v8..v15 and s4..s7, then the family ops, then `exp mrt0 v8, v9, v10, v11` so every result
is observed. Every op's destination is v8..v11, so later ops chain on earlier results, and an accumulating op reads
whatever an earlier op left in its destination.

Sources, in every position the encoding allows: VGPR v8..v15, SGPR s4..s7, a hex literal (32 bits) and an integer
inline constant (-16..64), read as the 32-bit dword (positive values zero-extend, negative values sign-extend: the
rule docs/design.md measured for VOP3P integer constants). VOP3P programs stay encodable on GFX10: at most one
literal and at most two constant-bus reads (SGPRs plus the literal). VOP2 S1 is a VGPR; the gfx908 VOP3 (`_e64 ...
clamp`) forms take no literal.

Modifiers: `clamp` on the VOP3P ops and on the gfx908 accumulating forms; `op_sel`/`op_sel_hi` on the S0 and S1
halves of v_dot4_i32_i8 (measured on gfx1036: op_sel[k] picks the 16-bit half of source k for bytes 0..1, op_sel_hi[k]
for bytes 2..3); and the explicit default `op_sel:[0,0,0] op_sel_hi:[1,1,1]` on the other VOP3P ops, which the
translator must take as the plain instruction.

Not generated, on purpose: `neg_lo`/`neg_hi` and source `neg`/`abs`, non-default op_sel outside v_dot4_i32_i8's S0/S1,
`clamp` on v_dot4c_i32_i8, and DPP/SDWA. The translator refuses each one; --self-test pins those rejections. Float
inline constants are left out too: no rule for them in a packed integer operand is recorded.

Three stages run one after the other:
  * a sweep: every op with every encodable source-kind combination, with and without clamp where the op takes it,
    and all 16 op_sel/op_sel_hi settings of v_dot4_i32_i8, values drawn from element-edge words;
  * an edge grid: every op with S0 and S1 from four words built of one lane width's extremes (all lanes the signed
    minimum, all the signed maximum, all ones, minimum/maximum alternating) and eight accumulators on and next to
    the saturation bounds, with and without clamp, four cases per program. It reaches the one point where the two
    products of v_dot2_i32_i16 alone overflow, (-32768)^2 + (-32768)^2 = 2^31, which random values almost never hit;
  * random programs (--programs), which widen the value coverage over the same space.

The report counts, per op, how many clamped results actually saturated and how many unclamped ones wrapped, so a
run shows that it reached the edges.

Usage:
    python -I tools/dot_diff.py [--programs N] [--seed S] [--max-ops N] [--no-sweep]
    python -I tools/dot_diff.py --ops v_dot4_i32_i8,v_dot4c_i32_i8
    python -I tools/dot_diff.py --list-ops
    python -I tools/dot_diff.py --self-test

Exit code 0 when every program matches, 1 on a mismatch (the first one is printed with its listing), 2 on a
harness error. Skips are counted with reasons and never silent. Nothing here reads game files.
"""
import argparse
import gc
import os
import random
import re
import struct
import sys
import tempfile
import warnings
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS_DIR = os.environ.get("LEONIDA_TOOLS_DIR") or HERE
sys.dont_write_bytecode = True
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)
SEM_DIR = os.path.join(os.path.dirname(HERE), "tests", "semantic")
if SEM_DIR not in sys.path:
    sys.path.insert(0, SEM_DIR)

import rdna2spirv  # noqa: E402
from spirv_mini_interp import UnsupportedConstruct, run_fragment  # noqa: E402

M32 = 0xFFFFFFFF
STUB_DXBC = b"DXBC" + bytes(24) + struct.pack("<I", 0) + bytes(4)

EXPORT = "exp mrt0 v8, v9, v10, v11 done vm"
EXPORT_REGS = (8, 9, 10, 11)

# op -> (element width, signed)
VOP3P_OPS = {
    "v_dot2_i32_i16": (16, True), "v_dot2_u32_u16": (16, False),
    "v_dot4_i32_i8": (8, True), "v_dot4_u32_u8": (8, False),
    "v_dot8_i32_i4": (4, True), "v_dot8_u32_u4": (4, False),
}
ACC_OPS = {
    "v_dot4c_i32_i8": (8, True),
    "v_dot2c_i32_i16": (16, True), "v_dot2c_u32_u16": (16, False), "v_dot4c_u32_u8": (8, False),
    "v_dot8c_i32_i4": (4, True), "v_dot8c_u32_u4": (4, False),
}
GFX103X_OPS = tuple(VOP3P_OPS) + ("v_dot4c_i32_i8",)
CLAMP_ACC_OPS = tuple(op for op in ACC_OPS if op != "v_dot4c_i32_i8")   # the gfx908 VOP3 `_e64 ... clamp` forms
OPSEL_OPS = ("v_dot4_i32_i8",)
FAMILY = {**VOP3P_OPS, **ACC_OPS}
OP_FORMS = {op: f"{op} d, s0, s1, s2 [op_sel] [clamp]" for op in VOP3P_OPS}
OP_FORMS.update({op: f"{op}_e32 d, s0, v1 (acc = d)" for op in ACC_OPS})
OP_FORMS.update({op: f"{op}_e32 d, s0, v1 | {op}_e64 d, s0, v1 clamp (acc = d)" for op in CLAMP_ACC_OPS})
OP_FORMS = {op: OP_FORMS[op] for op in FAMILY}

KINDS = ("vgpr", "sgpr", "lit", "inline")

# Element-edge words: the extremes of each lane width (i16 -32768/32767, i8 -128/127, i4 -8/7, their unsigned
# maxima) and accumulators next to the signed and unsigned saturation bounds.
EDGE = (0x00000000, 0x00000001, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000, 0x7FFFFFFE, 0x80000001, 0xFFFFFFFE,
        0x80008000, 0x7FFF7FFF, 0x80007FFF, 0xFFFF0001, 0x0000FFFF, 0x00010001, 0x7FFF8000, 0x80010000,
        0x80808080, 0x7F7F7F7F, 0x807F807F, 0x01010101, 0xFF00FF00, 0x00FF00FF,
        0x88888888, 0x77777777, 0x87878787, 0x11111111, 0xF0F0F0F0, 0x0F0F0F0F,
        0x55555555, 0xAAAAAAAA, 0x01234567, 0x89ABCDEF)
INLINE = tuple(range(0, 65)) + tuple(range(-16, 0))


class HarnessError(Exception):
    pass


# ---- the reference, from the RDNA2 ISA ----

def elements(word, width, signed):
    """The 32 // width lanes of a dword, lowest first, sign-extended for the signed ops."""
    out = []
    for off in range(0, 32, width):
        e = (word >> off) & ((1 << width) - 1)
        if signed and e >> (width - 1):
            e -= 1 << width
        out.append(e)
    return out


def exact_dot(op, x, y, acc):
    """The exact (unbounded) value of sum(S0.elem[k] * S1.elem[k]) + acc; acc is signed for the signed ops."""
    width, signed = FAMILY[op]
    total = sum(p * q for p, q in zip(elements(x & M32, width, signed), elements(y & M32, width, signed)))
    acc &= M32
    if signed and acc >> 31:
        acc -= 1 << 32
    return total + acc


def dot(op, x, y, acc, clamp=False):
    """The destination dword: the exact sum modulo 2^32, or saturated to the op's 32-bit range under clamp."""
    v = exact_dot(op, x, y, acc)
    if clamp:
        lo, hi = (-(1 << 31), (1 << 31) - 1) if FAMILY[op][1] else (0, M32)
        v = max(lo, min(hi, v))
    return v & M32


def select_halves(word, sel_lo, sel_hi):
    """op_sel/op_sel_hi on a v_dot4_i32_i8 source: bytes 0..1 come from the half sel_lo picks, bytes 2..3 from the
    half sel_hi picks."""
    half = lambda h: (word >> (16 * h)) & 0xFFFF  # noqa: E731
    return half(sel_lo) | (half(sel_hi) << 16)


_REG = re.compile(r"([sv])(\d+)$")
_MOD = re.compile(r"\s+(op_sel_hi:\[[01],[01],[01]\]|op_sel:\[[01],[01],[01]\]|clamp)$")


def _word(tok, vgpr, sgpr):
    """The 32-bit word a source token reads: a register, a hex literal, or an integer inline constant (two's
    complement at 32 bits)."""
    tok = tok.strip()
    m = _REG.fullmatch(tok)
    if m:
        regs = vgpr if m.group(1) == "v" else sgpr
        return regs.get(int(m.group(2)), 0)
    if tok.lower().startswith("0x"):
        return int(tok, 16) & M32
    if tok.lstrip("+-").isdigit():
        return int(tok, 10) & M32
    raise HarnessError(f"unknown source {tok!r}")


def _dst_index(dst):
    m = _REG.fullmatch(dst.strip())
    if not m or m.group(1) != "v":
        raise HarnessError(f"unknown destination {dst!r}")
    return int(m.group(2))


def split_line(line):
    """(opcode without _e32/_e64, operand tokens, modifier dict) for one generated line."""
    mods = {}
    body = line.rstrip()
    while True:
        m = _MOD.search(body)
        if not m:
            break
        tok = m.group(1)
        if tok == "clamp":
            mods["clamp"] = True
        else:
            name, bits = tok.split(":")
            mods[name] = tuple(int(b) for b in bits.strip("[]").split(","))
        body = body[:m.start()]
    op, _, rest = body.partition(" ")
    if op.endswith(("_e32", "_e64")):
        op = op[:-4]
    return op, [t.strip() for t in rest.split(",")], mods


def step(line, vgpr, sgpr, events=None):
    """Execute one generated listing line on the reference's registers. `events` (a Counter), when given, counts
    (op, "saturated") for a clamped result outside the 32-bit range and (op, "wrapped") for an unclamped one."""
    op, args, mods = split_line(line)
    if op == "v_mov_b32":
        vgpr[_dst_index(args[0])] = _word(args[1], vgpr, sgpr)
        return
    if op == "s_mov_b32":
        if not args[0].startswith("s"):
            raise HarnessError(line)
        sgpr[int(args[0][1:])] = _word(args[1], vgpr, sgpr)
        return
    if op not in FAMILY:
        raise HarnessError(line)
    d = _dst_index(args[0])
    if op in VOP3P_OPS:
        if len(args) != 4:
            raise HarnessError(line)
        x, y, acc = (_word(a, vgpr, sgpr) for a in args[1:])
        sel, selhi = mods.get("op_sel", (0, 0, 0)), mods.get("op_sel_hi", (1, 1, 1))
        if (sel, selhi) != ((0, 0, 0), (1, 1, 1)):
            if op not in OPSEL_OPS or sel[2] != 0 or selhi[2] != 1:
                raise HarnessError(f"op_sel outside the modelled forms: {line}")
            x, y = select_halves(x, sel[0], selhi[0]), select_halves(y, sel[1], selhi[1])
    else:
        if len(args) != 3:
            raise HarnessError(line)
        x, y = _word(args[1], vgpr, sgpr), _word(args[2], vgpr, sgpr)
        acc = vgpr.get(d, 0)
    clamp = mods.get("clamp", False)
    if events is not None:
        v = exact_dot(op, x, y, acc)
        lo, hi = (-(1 << 31), (1 << 31) - 1) if FAMILY[op][1] else (0, M32)
        if not lo <= v <= hi:
            events[(op, "saturated" if clamp else "wrapped")] += 1
    vgpr[d] = dot(op, x, y, acc, clamp)


def reference(lines, events=None):
    """The four exported mrt0 words the ISA gives for a program (seeds and ops; exp and s_endpgm are ignored)."""
    vgpr, sgpr = {}, {}
    for line in lines:
        first = line.split()[0]
        if first in ("exp", "s_endpgm"):
            continue
        step(line, vgpr, sgpr, events)
    return [vgpr.get(r, 0) for r in EXPORT_REGS]


# ---- program generation ----

def _near_bound(rng):
    """A word within a dot product's reach of a saturation bound (INT32_MAX, INT32_MIN, UINT32_MAX), so an
    accumulator or a chained result can cross it: a 4-bit dot spans less than 2^9, an 8-bit one 2^18."""
    delta = rng.randrange(1 << rng.choice((4, 9, 13, 18, 31)))
    bound = rng.choice((0x7FFFFFFF, 0x80000000, M32))
    return (bound + delta) & M32 if bound == 0x80000000 else (bound - delta) & M32


def _seed_word(rng):
    r = rng.random()
    if r < 0.45:
        return rng.choice(EDGE)
    return _near_bound(rng) if r < 0.7 else rng.getrandbits(32)


def _source(rng, kind):
    if kind == "vgpr":
        return f"v{rng.randrange(8, 16)}"
    if kind == "sgpr":
        return f"s{rng.randrange(4, 8)}"
    if kind == "lit":
        return f"0x{_seed_word(rng):08x}"
    return str(rng.choice(INLINE))


def encodable(op, kinds, clamp=False):
    """GFX10 VOP3P: at most one literal and two constant-bus reads. VOP2: S1 is a VGPR. The gfx908 VOP3 forms
    (clamp on an accumulating op) take no literal."""
    if op in VOP3P_OPS:
        return kinds.count("lit") <= 1 and kinds.count("lit") + kinds.count("sgpr") <= 2
    if kinds[1] != "vgpr":
        return False
    return not (clamp and kinds[0] == "lit")


def op_line(rng, op, kinds, clamp=False, sel=None):
    """One family line with the given source kinds; sel = (op_sel, op_sel_hi) tuples or None."""
    srcs = [_source(rng, k) for k in kinds]
    dst = f"v{rng.choice(EXPORT_REGS)}"
    if op in VOP3P_OPS:
        line = f"{op} {dst}, {', '.join(srcs)}"
        if sel is not None:
            line += " op_sel:[{},{},{}] op_sel_hi:[{},{},{}]".format(*sel[0], *sel[1])
        return line + (" clamp" if clamp else "")
    if clamp:
        return f"{op}_e64 {dst}, {srcs[0]}, {srcs[1]} clamp"
    return f"{op}_e32 {dst}, {srcs[0]}, {srcs[1]}"


def _random_op(rng, op):
    """(line) for one random use of op: encodable kinds, clamp where the op takes it, op_sel where measured."""
    clamp = (op in VOP3P_OPS or op in CLAMP_ACC_OPS) and rng.random() < 0.5
    n = 3 if op in VOP3P_OPS else 2
    while True:
        kinds = tuple(rng.choice(KINDS) for _ in range(n))
        if encodable(op, kinds, clamp):
            break
    sel = None
    if op in OPSEL_OPS and rng.random() < 0.4:
        sel = ((rng.randrange(2), rng.randrange(2), 0), (rng.randrange(2), rng.randrange(2), 1))
    elif op in VOP3P_OPS and rng.random() < 0.1:
        sel = ((0, 0, 0), (1, 1, 1))
    return op_line(rng, op, kinds, clamp, sel)


def seeds(rng):
    lines = [f"v_mov_b32 v{r}, 0x{_seed_word(rng):08x}" for r in range(8, 16)]
    lines += [f"s_mov_b32 s{r}, 0x{_seed_word(rng):08x}" for r in range(4, 8)]
    return lines


def program(rng, n_ops, ops=None):
    """One straight-line program: (lines, ops_used). Ops drawn from `ops` (default: the whole family)."""
    ops = list(ops) if ops else list(FAMILY)
    lines = seeds(rng)
    used = Counter()
    for _ in range(n_ops):
        op = rng.choice(ops)
        used[op] += 1
        lines.append(_random_op(rng, op))
    return lines, used


def sweep_ops(ops=None):
    """Every op with every encodable source-kind combination, with and without clamp where the op takes it, plus
    the 16 op_sel/op_sel_hi settings of v_dot4_i32_i8 (VGPR sources, with and without clamp). Returns a list of
    (op, kinds, clamp, sel)."""
    ops = list(ops) if ops else list(FAMILY)
    cases = []
    for op in ops:
        n = 3 if op in VOP3P_OPS else 2
        combos = [()]
        for _ in range(n):
            combos = [c + (k,) for c in combos for k in KINDS]
        clamps = (False, True) if (op in VOP3P_OPS or op in CLAMP_ACC_OPS) else (False,)
        for clamp in clamps:
            for kinds in combos:
                if encodable(op, kinds, clamp):
                    cases.append((op, kinds, clamp, None))
        if op in OPSEL_OPS:
            for bits in range(16):
                sel = ((bits & 1, (bits >> 1) & 1, 0), ((bits >> 2) & 1, (bits >> 3) & 1, 1))
                for clamp in (False, True):
                    cases.append((op, ("vgpr", "vgpr", "vgpr"), clamp, sel))
    return cases


def sweep_program(op, kinds, clamp, sel, rng):
    """One sweep program: seeds, then the op once."""
    return seeds(rng) + [op_line(rng, op, kinds, clamp, sel)]


def lane_edges(width):
    """Four dwords made of one lane width's extremes: every lane the signed minimum (0x8000, 0x80, 0x8), every lane
    the signed maximum, every lane all ones, and minimum/maximum alternating."""
    smin, smax = 1 << (width - 1), (1 << (width - 1)) - 1
    rep = lambda lane: sum(lane << off for off in range(0, 32, width))  # noqa: E731
    alt = sum((smin if (off // width) % 2 == 0 else smax) << off for off in range(0, 32, width))
    return (rep(smin), rep(smax), rep((1 << width) - 1), alt)


# Accumulators on and next to the saturation bounds; 256 is inside every lane width's reach (a 4-bit dot spans 512).
ACC_EDGES = (0, 1, M32, 0x7FFFFFFF, 0x80000000, 0x7FFFFF00, 0x80000100, 0xFFFFFF00)
GRID_PER_PROGRAM = 4


def grid_cases(ops=None):
    """The edge grid: every op with every pair of lane_edges words as S0/S1, every ACC_EDGES accumulator, with and
    without clamp where the op takes it. Returns a list of (op, x, y, acc, clamp)."""
    ops = list(ops) if ops else list(FAMILY)
    cases = []
    for op in ops:
        words = lane_edges(FAMILY[op][0])
        clamps = (False, True) if (op in VOP3P_OPS or op in CLAMP_ACC_OPS) else (False,)
        cases += [(op, x, y, acc, clamp) for clamp in clamps for x in words for y in words for acc in ACC_EDGES]
    return cases


def grid_program(chunk):
    """Up to four grid cases in one program: each loads its operands into v12..v14 (the accumulating forms load the
    accumulator into their destination) and writes its own exported register v8..v11."""
    lines = []
    for (op, x, y, acc, clamp), dst in zip(chunk, EXPORT_REGS):
        lines += [f"v_mov_b32 v12, 0x{x:08x}", f"v_mov_b32 v13, 0x{y:08x}"]
        if op in VOP3P_OPS:
            lines += [f"v_mov_b32 v14, 0x{acc:08x}", f"{op} v{dst}, v12, v13, v14" + (" clamp" if clamp else "")]
        else:
            suffix = "_e64" if clamp else "_e32"
            lines += [f"v_mov_b32 v{dst}, 0x{acc:08x}", f"{op}{suffix} v{dst}, v12, v13" + (" clamp" if clamp else "")]
    return lines


# ---- the translator oracle ----

def write_listing(path, lines, name):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(f"; {name}  code 0x0..0x1000\n")
        for line in lines:
            f.write("\t" + line + "\n")
        f.write("\t" + EXPORT + "\n")
        f.write("\ts_endpgm\n")


def translate_listing(lines, name, tmpdir):
    """The four mrt0 words the translator path produces. Raises rdna2spirv.Unsupported (and the mini interp's
    UnsupportedConstruct) for anything outside the oracles' model."""
    dis = os.path.join(tmpdir, "disasm")
    os.makedirs(dis, exist_ok=True)
    path = os.path.join(dis, name + ".s")
    write_listing(path, lines, name)
    with open(os.path.join(tmpdir, name + ".dxbc"), "wb") as f:
        f.write(STUB_DXBC)
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", ResourceWarning)
        spvasm = rdna2spirv.translate(path)
    gc.collect()
    out = run_fragment(spvasm)
    if list(out) != ["%out_mrt0"]:
        raise HarnessError(f"outputs {sorted(out)}, want %out_mrt0")
    return [w & M32 for w in out["%out_mrt0"]]


def check(lines, name="ps_dot_case"):
    """None when both oracles agree, else the first differing word with both values."""
    with tempfile.TemporaryDirectory(prefix="dot-diff-") as tmp:
        want = reference(lines)
        got = translate_listing(lines, name, tmp)
    for i, (w, g) in enumerate(zip(want, got)):
        if w != (g & M32):
            return f"word {i} (v{EXPORT_REGS[i]}): expected 0x{w:08x}, actual 0x{g:08x}"
    return None


def run_one(lines, name, tmpdir, events=None):
    """(status, detail) with status in {"match", "mismatch", "skip"}."""
    try:
        want = reference(lines, events)
    except HarnessError as e:
        return ("skip", f"reference cannot run the program: {e}")
    try:
        got = translate_listing(lines, name, tmpdir)
    except (rdna2spirv.Unsupported, UnsupportedConstruct) as e:
        return ("skip", f"{type(e).__name__}: {e}")
    except (IndexError, KeyError, ValueError) as e:
        return ("skip", f"translator error {type(e).__name__}: {e}")
    for i, (w, g) in enumerate(zip(want, got)):
        if w != (g & M32):
            return ("mismatch", f"word {i} (v{EXPORT_REGS[i]}): expected 0x{w:08x}, actual 0x{g:08x}")
    return ("match", "")


# ---- the suites ----

def _new_counts():
    return {"pass": 0, "mismatch": 0, "skip": Counter(), "ops": Counter(), "events": Counter()}


def _tally(counts, status, detail):
    if status == "match":
        counts["pass"] += 1
    elif status == "skip":
        counts["skip"][detail] += 1
    else:
        counts["mismatch"] += 1


def run_sweep(seed, ops=None):
    """Every case of sweep_ops, one program each. Returns (counts, failure)."""
    counts = _new_counts()
    failure = None
    with tempfile.TemporaryDirectory(prefix="dot-diff-sweep-") as tmp:
        for i, (op, kinds, clamp, sel) in enumerate(sweep_ops(ops)):
            tag = f"{op}:{'/'.join(kinds)}:{int(clamp)}:{sel}"
            lines = sweep_program(op, kinds, clamp, sel, random.Random(f"{seed}:{tag}"))
            counts["ops"][op] += 1
            status, detail = run_one(lines, f"ps_dot_sw{i:04d}", tmp, counts["events"])
            _tally(counts, status, detail)
            if status == "mismatch" and failure is None:
                failure = (f"sweep {tag} (case {i}, seed {seed})", lines, detail)
    return counts, failure


def run_grid(ops=None):
    """The edge grid, GRID_PER_PROGRAM cases per program. Returns (counts, failure); counts["ops"] counts cases."""
    counts = _new_counts()
    failure = None
    cases = grid_cases(ops)
    with tempfile.TemporaryDirectory(prefix="dot-diff-grid-") as tmp:
        for i in range(0, len(cases), GRID_PER_PROGRAM):
            chunk = cases[i:i + GRID_PER_PROGRAM]
            lines = grid_program(chunk)
            counts["ops"].update(op for op, *_ in chunk)
            status, detail = run_one(lines, f"ps_dot_gr{i // GRID_PER_PROGRAM:04d}", tmp, counts["events"])
            _tally(counts, status, detail)
            if status == "mismatch" and failure is None:
                failure = (f"edge grid program {i // GRID_PER_PROGRAM}", lines, detail)
    return counts, failure


def run_suite(programs, seed, max_ops=6, ops=None):
    """Random seeded programs. Returns (counts, failure)."""
    if type(programs) is not int or programs < 0:
        raise ValueError("programs must be a nonnegative integer")
    if type(max_ops) is not int or max_ops < 1:
        raise ValueError("max_ops must be at least 1")
    counts = _new_counts()
    failure = None
    rng = random.Random(f"suite:{seed}")
    with tempfile.TemporaryDirectory(prefix="dot-diff-run-") as tmp:
        for i in range(programs):
            lines, used = program(rng, rng.randint(1, max_ops), ops)
            counts["ops"].update(used)
            status, detail = run_one(lines, f"ps_dot_{i:04d}", tmp, counts["events"])
            _tally(counts, status, detail)
            if status == "mismatch" and failure is None:
                failure = (f"program {i} (seed {seed})", lines, detail)
    return counts, failure


REJECTED = (
    ("non-default op_sel on v_dot2_i32_i16", "v_dot2_i32_i16 v8, v9, v10, v11 op_sel:[1,0,0] op_sel_hi:[1,1,1]"),
    ("op_sel_hi:[0,0,0] on v_dot2_u32_u16", "v_dot2_u32_u16 v8, v9, v10, v11 op_sel:[0,0,0] op_sel_hi:[0,0,0]"),
    ("an S2 half select on v_dot4_i32_i8", "v_dot4_i32_i8 v8, v9, v10, v11 op_sel:[0,0,1] op_sel_hi:[1,1,1]"),
    ("neg_lo on v_dot4_i32_i8", "v_dot4_i32_i8 v8, v9, v10, v11 neg_lo:[1,0,0]"),
    ("neg_hi on v_dot8_i32_i4", "v_dot8_i32_i4 v8, v9, v10, v11 neg_hi:[0,1,0]"),
    ("source neg on v_dot4_u32_u8", "v_dot4_u32_u8 v8, -v9, v10, v11"),
    ("clamp on v_dot4c_i32_i8", "v_dot4c_i32_i8_e64 v8, v9, v10 clamp"),
    ("source abs on v_dot2c_i32_i16", "v_dot2c_i32_i16_e64 v8, |v9|, v10"),
    ("op_sel on v_dot8c_u32_u4", "v_dot8c_u32_u4_e64 v8, v9, v10 op_sel:[0,0,0]"),
)


def self_test():
    """True when the reference's hand-pinned words, the rejected forms and the harness canaries all hold."""
    ok = True

    def expect(cond, what):
        nonlocal ok
        print(("ok   " if cond else "FAIL ") + what)
        ok = ok and cond

    # Hand-pinned reference words, each derived from the ISA rule in the docstring.
    expect(dot("v_dot2_i32_i16", 0x80008000, 0x80008000, 0) == 0x80000000,
           "v_dot2_i32_i16: (-32768)^2 * 2 = 2^31 wraps to 0x80000000")
    expect(dot("v_dot2_i32_i16", 0x80008000, 0x80008000, 0, clamp=True) == 0x7FFFFFFF,
           "v_dot2_i32_i16 clamp: 2^31 saturates to 0x7fffffff")
    expect(dot("v_dot2_i32_i16", 0x80008000, 0x80008000, 0x80000000, clamp=True) == 0,
           "v_dot2_i32_i16 clamp: 2^31 + INT32_MIN = 0, the products' overflow cancels (no early saturation)")
    expect(dot("v_dot2_u32_u16", M32, M32, 0) == 0xFFFC0002,
           "v_dot2_u32_u16: 2 * 65535^2 = 0x1fffc0002 wraps to 0xfffc0002")
    expect(dot("v_dot2_u32_u16", M32, M32, 0, clamp=True) == M32, "v_dot2_u32_u16 clamp saturates to 0xffffffff")
    expect(dot("v_dot4_u32_u8", M32, M32, 5) == 4 * 255 * 255 + 5, "v_dot4_u32_u8: 4 * 255^2 + 5")
    expect(dot("v_dot4_i32_i8", 0x7F7F7F7F, 0x7F7F7F7F, 0x7FFFFFFF) == 0x8000FC03,
           "v_dot4_i32_i8 wraps upward without clamp (the gfx1036 word 0x8000fc03)")
    expect(dot("v_dot4_i32_i8", 0x7F7F7F7F, 0x7F7F7F7F, 0x7FFFFFFF, clamp=True) == 0x7FFFFFFF,
           "v_dot4_i32_i8 clamp saturates the same sum")
    expect(dot("v_dot8_i32_i4", 0x88888888, 0x88888888, 0) == 8 * 64, "v_dot8_i32_i4: 8 * (-8)^2 = 512")
    expect(dot("v_dot8_u32_u4", M32, M32, 0) == 8 * 225, "v_dot8_u32_u4: 8 * 15^2 = 1800")
    expect(dot("v_dot8_i32_i4", 0x77777777, 0x88888888, 0x80000000, clamp=True) == 0x80000000,
           "v_dot8_i32_i4 clamp: INT32_MIN - 448 saturates to 0x80000000")
    # op_sel 2 / op_sel_hi 5 exchanges S1's halves (the gfx1036-measured form): 0x00050000 reads as 5.
    expect(reference(["v_mov_b32 v9, 1", "v_mov_b32 v10, 0x00050000",
                      "v_dot4_i32_i8 v8, v9, v10, 0 op_sel:[0,1,0] op_sel_hi:[1,0,1]"])[0] == 5,
           "v_dot4_i32_i8 op_sel:[0,1,0] op_sel_hi:[1,0,1] swaps S1's 16-bit halves")
    # The accumulating form reads its old destination.
    expect(reference(["v_mov_b32 v8, 100", "v_mov_b32 v9, 0x01020304", "v_mov_b32 v10, 0x05060708",
                      "v_dot4c_i32_i8_e32 v8, v9, v10"])[0] == 170, "v_dot4c_i32_i8: 70 + old D 100")
    # A negative inline constant is the sign-extended dword (four -1 bytes), a positive one zero-extends.
    expect(reference(["v_mov_b32 v9, 0x01010101", "v_dot4_i32_i8 v8, v9, -1, 0"])[0] == M32 - 3,
           "the inline constant -1 reads as 0xffffffff (four -1 bytes: -4)")
    expect(reference(["v_mov_b32 v9, 0x01010101", "v_dot4_i32_i8 v8, v9, 5, 0"])[0] == 5,
           "the inline constant 5 reads as 0x00000005")

    # The two-oracle path agrees on a fixed program, and a sabotaged word is caught.
    lines = ["v_mov_b32 v8, 0x80008000", "v_mov_b32 v9, 0x80008000", "v_mov_b32 v10, 0x80000000",
             "v_dot2_i32_i16 v11, v8, v9, v10 clamp"]
    expect(check(lines) is None, "a hand-pinned clamp program matches on both oracles")
    real = translate_listing
    try:
        globals()["translate_listing"] = lambda l, n, t: real(l, n, t)[:3] + [1]
        expect(check(lines) is not None, "a sabotaged export word is reported")
    finally:
        globals()["translate_listing"] = real

    # Forms the contract rows say are refused, not guessed.
    with tempfile.TemporaryDirectory(prefix="dot-diff-neg-") as tmp:
        for what, bad in REJECTED:
            try:
                translate_listing(["v_mov_b32 v9, 1", "v_mov_b32 v10, 2", bad], "ps_dot_neg", tmp)
                expect(False, f"{what}: the translator rejected it")
            except (rdna2spirv.Unsupported, UnsupportedConstruct):
                expect(True, f"{what}: the translator rejected it")

    # Determinism: the same seed gives the same program stream.
    expect(program(random.Random(9), 4) == program(random.Random(9), 4), "generation is deterministic for a seed")
    counts, failure = run_suite(8, 3, 3)
    expect(failure is None and counts["pass"] == 8 and not counts["skip"],
           f"a short seeded run matches ({counts['pass']} programs, skips {dict(counts['skip'])})")
    return ok


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--programs", type=int, default=300, help="random programs after the sweep")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--max-ops", type=int, default=6, help="ops per random program")
    ap.add_argument("--ops", default=None, help="comma-separated opcodes to cover (default: all twelve; "
                                                "'gfx103x' selects the seven gfx103x encodings)")
    ap.add_argument("--no-sweep", action="store_true", help="skip the source-kind sweep and the edge grid")
    ap.add_argument("--list-ops", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)

    if args.list_ops:
        for op, form in OP_FORMS.items():
            tag = "gfx103x" if op in GFX103X_OPS else "gfx908 syntax"
            print(f"{op:18s} {tag:14s} {form}")
        return 0
    if args.self_test:
        return 0 if self_test() else 1

    if args.programs < 0:
        ap.error("--programs must be nonnegative")
    if args.max_ops < 1:
        ap.error("--max-ops must be at least 1")
    if args.no_sweep and args.programs == 0:
        ap.error("nothing to run: pass --programs >= 1 or omit --no-sweep")

    ops = None
    if args.ops is not None:
        if args.ops.strip() == "gfx103x":
            ops = list(GFX103X_OPS)
        else:
            tokens = [o.strip() for o in args.ops.split(",")]
            if not tokens or any(not o for o in tokens):
                ap.error("--ops must be a comma-separated list of opcodes (or 'gfx103x')")
            ops = list(dict.fromkeys(tokens))
            bad = [o for o in ops if o not in FAMILY]
            if bad:
                print(f"unknown op(s) {bad}; --list-ops shows the family", file=sys.stderr)
                return 2

    total_pass = total_run = total_mismatch = total_skip = 0
    per_op, events = Counter(), Counter()
    failure = None
    try:
        for label, enabled, runner in (
            ("sweep", not args.no_sweep, lambda: run_sweep(args.seed, ops)),
            ("edge grid", not args.no_sweep, lambda: run_grid(ops)),
            ("random", args.programs > 0, lambda: run_suite(args.programs, args.seed, args.max_ops, ops)),
        ):
            if not enabled:
                continue
            counts, first = runner()
            skips = sum(counts["skip"].values())
            ran = counts["pass"] + counts["mismatch"] + skips
            total_pass += counts["pass"]
            total_mismatch += counts["mismatch"]
            total_skip += skips
            total_run += ran
            per_op.update(counts["ops"])
            events.update(counts["events"])
            print(f"{label}: {counts['pass']}/{ran} programs match, {counts['mismatch']} mismatch; "
                  f"skips: {dict(counts['skip'])}")
            failure = failure or first
    except HarnessError as e:
        print(f"harness error: {e}", file=sys.stderr)
        return 2
    print("cases per op: " + ", ".join(f"{op} {per_op[op]}" for op in sorted(per_op)))
    print("results outside the 32-bit range (saturated under clamp / wrapped without): " + ", ".join(
        f"{op} {events[(op, 'saturated')]}/{events[(op, 'wrapped')]}" for op in sorted(per_op)))
    print(f"dot family: {total_pass}/{total_run} programs match, {total_mismatch} mismatch "
          f"({len(per_op)} of {len(FAMILY)} ops)")
    if failure:
        _report(failure)
        return 1
    if total_skip:
        print(f"SKIPPED {total_skip} program(s): every generated form should translate (reasons above)")
        return 1
    if total_run == 0:
        print("no program ran", file=sys.stderr)
        return 2
    return 0


def _report(failure):
    name, lines, detail = failure
    print(f"MISMATCH {name}: {detail}")
    print("program:")
    for line in lines:
        print("    " + line)


if __name__ == "__main__":
    try:  # a redirected stdout is strict ANSI code page under -I; write '?' for what it cannot encode
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError, OSError):
        pass
    sys.exit(main())
