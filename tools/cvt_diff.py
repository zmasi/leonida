r"""Differential fuzzer for the RDNA2 rounding and normalizing conversions (standard library only).

Covers eight opcodes that no other fuzzer generates (the `v_cvt_flr_i32_f32`/`v_cvt_rpi_i32_f32`,
`v_cvt_norm_*_f16`, `v_cvt_pknorm_*_f32` and `v_cvt_pknorm_*_f16` rows of docs/design.md's contract table):

    v_cvt_flr_i32_f32   v_cvt_rpi_i32_f32                         VOP1 (_e32, _e64), f32 -> i32
    v_cvt_norm_i16_f16  v_cvt_norm_u16_f16                        VOP1 (_e32, _e64), f16 -> snorm16 / unorm16
    v_cvt_pknorm_i16_f32 v_cvt_pknorm_u16_f32                     VOP3, two f32 -> packed snorm16 / unorm16
    v_cvt_pknorm_i16_f16 v_cvt_pknorm_u16_f16                     VOP3, two f16 -> packed snorm16 / unorm16

Seeded straight-line programs run through two independent oracles, and the four exported mrt0 words are compared
bit for bit:

  * the reference below, written from the rules docs/design.md records as measured on RDNA2, with exact rationals
    (fractions.Fraction), so no step of it rounds in binary32. It shares no code with the translator:
      - flr: sat_i32(floor(S0)); rpi: sat_i32(floor(S0 + 1/2)), the + 1/2 exact. |S0| >= 2^31, +-inf and NaN
        saturate by the operand's sign bit (+ -> 0x7FFFFFFF, - -> 0x80000000). An f32 denormal reads as zero
        (gfx1036 gives flr(-1e-45) = 0).
      - norm and pknorm: round_even(clamp(x, -1, 1) * 32767) (i16) or round_even(clamp(x, 0, 1) * 65535) (u16),
        rounded once from the exact product, NaN -> 0. f16 denormals are values (the MODE inferred for the game
        keeps them); an f32 denormal is too small to round to anything but 0 either way.
      - norm writes D[15:0] and keeps D[31:16]. pknorm writes S0's result to D[15:0] and S1's to D[31:16]; with
        the destination op_sel bit of the f16 forms, S0's result to D[31:16] and 0 to D[15:0].
    tests/test_cvt_diff.py pins it to the words measured on RDNA2 that tests/semantic records, and
  * tools/rdna2spirv.py, executing the SPIR-V with tests/semantic/spirv_mini_interp.py.

Program shape: seeds in v8..v15 and s4..s7, then the family ops, then `exp mrt0 v8, v9, v10, v11` so every result
is observed. Every destination is v8..v11, so later ops read earlier results (as floats) and norm keeps whatever
an earlier op left in the high half.

Sources: VGPR v8..v15, SGPR s4..s7, a hex literal, an integer inline constant (-16..64, the 32-bit two's complement
word) and a float inline constant (0.5, 1.0, 2.0, 4.0, their negatives, 0.15915494). A 16-bit float operand reads
the low half of a register or literal, and a float inline constant as its f16 value (docs/design.md, "f16 inline
constants"); an integer inline constant as the low half of its word. VOP3 programs stay encodable on GFX10: at most
one literal and two constant-bus reads.

Modifiers: source `neg`/`abs` on register operands of every _e64/VOP3 form (applied to the f32 word, or to the
selected f16 half); `op_sel:[a,b,c]` on v_cvt_pknorm_*_f16, where a and b pick the high half of a register source
and c = 1 writes S0's result to D[31:16] and 0 to D[15:0], dropping S1's (measured on gfx1036, docs/design.md);
and the measured hardware no-ops: VOP3 `clamp`, `mul:2`, `mul:4`, `div:2` on v_cvt_norm_*, and `clamp` on
v_cvt_pknorm_* (both widths).

Not generated, on purpose: SDWA and DPP forms (tools/sdwa_diff.py's domain), output modifiers on flr/rpi,
`mul:2`/`mul:4`/`div:2` on pknorm (the translator refuses them, and llvm-mc does for the f16 forms), `op_sel` on
norm (GFX10 has no encoding for it), and modifiers on constants. --self-test pins the forms the translator
refuses.

Three stages run one after the other:
  * a sweep: every op with every source kind and every modifier setting it takes, values from the edge lists;
  * an edge grid: every op on every edge input (zeros, denormals, the normal minimum, +-0.5 and its neighbours,
    the scale steps 1/32767 and 1/65535, near ties of x * 32767 and x * 65535, +-1, 2^23 and 2^31 neighbours, the
    largest finite values, +-inf, quiet and signalling NaNs of both signs), four results per program;
  * random programs (--programs), which widen the value coverage over the same space.

--exhaustive-f16 adds every one of the 65,536 f16 inputs through the four f16 ops (49,152 programs).

Usage:
    python -I tools/cvt_diff.py [--programs N] [--seed S] [--max-ops N] [--no-sweep] [--exhaustive-f16]
    python -I tools/cvt_diff.py --ops v_cvt_rpi_i32_f32,v_cvt_pknorm_i16_f32
    python -I tools/cvt_diff.py --list-ops
    python -I tools/cvt_diff.py --self-test

Exit code 0 when every program matches, 1 on a mismatch (the first one is printed with its listing) or a skip, 2 on
a harness error. Every generated form is one the translator takes, so a skip is a finding too: a translation the
interpreter calls undefined (an FClamp of a NaN, say) is reported with its reason, not passed. Nothing here reads game
files.
"""
import argparse
import gc
import math
import os
import random
import re
import struct
import sys
import tempfile
import warnings
from collections import Counter
from fractions import Fraction

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
M16 = 0xFFFF
STUB_DXBC = b"DXBC" + bytes(24) + struct.pack("<I", 0) + bytes(4)

EXPORT = "exp mrt0 v8, v9, v10, v11 done vm"
EXPORT_REGS = (8, 9, 10, 11)

# op -> (kind, snorm); kind: "flr"/"rpi" (f32 -> i32), "norm" (f16 -> 16 bits), "pk32", "pk16"
FAMILY = {
    "v_cvt_flr_i32_f32": ("flr", None), "v_cvt_rpi_i32_f32": ("rpi", None),
    "v_cvt_norm_i16_f16": ("norm", True), "v_cvt_norm_u16_f16": ("norm", False),
    "v_cvt_pknorm_i16_f32": ("pk32", True), "v_cvt_pknorm_u16_f32": ("pk32", False),
    "v_cvt_pknorm_i16_f16": ("pk16", True), "v_cvt_pknorm_u16_f16": ("pk16", False),
}
VOP1_OPS = tuple(op for op, (k, _) in FAMILY.items() if k in ("flr", "rpi", "norm"))
OMOD_OPS = tuple(op for op, (k, _) in FAMILY.items() if k == "norm")     # clamp, mul:2, mul:4, div:2 are no-ops
CLAMP_OPS = OMOD_OPS + tuple(op for op, (k, _) in FAMILY.items() if k in ("pk32", "pk16"))
OMODS = ("clamp", "mul:2", "mul:4", "div:2", "clamp mul:2", "clamp div:2")
OP_FORMS = {
    **{op: f"{op}_e32 d, s0 | {op}_e64 d, [-][|]s0[|]" for op in VOP1_OPS},
    **{op: f"{op} d, [-][|]s0[|], [-][|]s1[|]" for op, (k, _) in FAMILY.items() if k == "pk32"},
    **{op: f"{op} d, [-][|]s0[|], [-][|]s1[|] [op_sel:[a,b,c]]" for op, (k, _) in FAMILY.items() if k == "pk16"},
}
for _op in OMOD_OPS:
    OP_FORMS[_op] += " [clamp] [mul:2|mul:4|div:2]"
for _op in CLAMP_OPS[len(OMOD_OPS):]:
    OP_FORMS[_op] += " [clamp]"

KINDS = ("vgpr", "sgpr", "lit", "int", "float")
MODS = ("", "-", "|", "-|")         # source modifiers on a register: none, neg, abs, neg(abs)
FLOAT_INLINE = {  # token -> (f32 word, f16 half)
    "0.5": (0x3F000000, 0x3800), "-0.5": (0xBF000000, 0xB800), "1.0": (0x3F800000, 0x3C00),
    "-1.0": (0xBF800000, 0xBC00), "2.0": (0x40000000, 0x4000), "-2.0": (0xC0000000, 0xC000),
    "4.0": (0x40800000, 0x4400), "-4.0": (0xC0800000, 0xC400), "0.15915494": (0x3E22F983, 0x3118),
}
INT_INLINE = tuple(range(0, 65)) + tuple(range(-16, 0))


class HarnessError(Exception):
    """The generator or the reference met something outside the modelled forms."""


# ---- the reference ----

def f32_value(w):
    """An f32 word as ("nan", sign), ("inf", sign) or ("num", Fraction). Denormals are values here; the ops decide
    whether they flush."""
    sign = w >> 31
    e, m = (w >> 23) & 0xFF, w & 0x7FFFFF
    if e == 0xFF:
        return ("nan", sign) if m else ("inf", sign)
    v = Fraction(m, 1 << 149) if e == 0 else Fraction((1 << 23) | m) * Fraction(2) ** (e - 150)
    return ("num", -v if sign else v)


def f16_value(h):
    sign = h >> 15
    e, m = (h >> 10) & 0x1F, h & 0x3FF
    if e == 0x1F:
        return ("nan", sign) if m else ("inf", sign)
    v = Fraction(m, 1 << 24) if e == 0 else Fraction((1 << 10) | m) * Fraction(2) ** (e - 25)
    return ("num", -v if sign else v)


def round_even(q):
    """Round a Fraction to the nearest integer, ties to even."""
    fl = math.floor(q)
    d = q - fl
    if d > Fraction(1, 2) or (d == Fraction(1, 2) and fl % 2):
        return fl + 1
    return fl


def to_i32(w, half_up):
    """v_cvt_flr_i32_f32 (half_up False) and v_cvt_rpi_i32_f32 (True) on an f32 word, modifiers already applied."""
    sign = w >> 31
    if w & 0x7FFFFFFF >= 0x4F000000:              # |x| >= 2^31, inf, NaN: by the sign bit
        return 0x80000000 if sign else 0x7FFFFFFF
    if w & 0x7F800000 == 0:                       # zero or denormal: flushed
        return 0
    v = f32_value(w)[1]
    return math.floor(v + Fraction(1, 2) if half_up else v) & M32


def normalize(kind_value, snorm):
    """round_even(clamp(x, lo, 1) * scale) & 0xFFFF for an f32_value/f16_value result; NaN gives 0."""
    kind, v = kind_value
    if kind == "nan":
        return 0
    lo = Fraction(-1) if snorm else Fraction(0)
    if kind == "inf":
        v = lo if v else Fraction(1)
    v = min(max(v, lo), Fraction(1))
    return round_even(v * (32767 if snorm else 65535)) & M16


def modify(word, mod, bits):
    """Source abs/neg on a `bits`-wide float: abs clears the sign, neg flips it (abs first for -|x|)."""
    sbit = 1 << (bits - 1)
    if "|" in mod:
        word &= ~sbit
    if mod.startswith("-"):
        word ^= sbit
    return word


_REG = re.compile(r"([sv])(\d+)$")
_SRC = re.compile(r"(-?)(\|?)([sv]\d+)\|?$")
_MOD = re.compile(r"\s+(op_sel:\[[01],[01],[01]\]|clamp|mul:[24]|div:2)$")


def source(tok, vgpr, sgpr, f16):
    """(word or half, modifier string, is_register) for a source token. A 16-bit operand is the low half of a register
    or literal, the f16 value of a float inline constant, or the low half of an integer inline constant's word."""
    tok = tok.strip()
    m = _SRC.fullmatch(tok)
    if m:
        neg, bar, reg = m.groups()
        if bar and not tok.endswith("|"):
            raise HarnessError(f"unbalanced abs bars in {tok!r}")
        r = _REG.fullmatch(reg)
        regs = vgpr if r.group(1) == "v" else sgpr
        return regs.get(int(r.group(2)), 0), neg + bar, True
    if tok in FLOAT_INLINE:
        return FLOAT_INLINE[tok][1 if f16 else 0], "", False
    if tok.lower().startswith("0x"):
        return int(tok, 16) & M32, "", False
    if tok.lstrip("-").isdigit():
        return int(tok, 10) & M32, "", False
    raise HarnessError(f"unknown source {tok!r}")


def split_line(line):
    """(opcode, raw opcode, operand tokens, modifier list) for one generated line."""
    mods = []
    body = line.rstrip()
    while True:
        m = _MOD.search(body)
        if not m:
            break
        mods.insert(0, m.group(1))
        body = body[:m.start()]
    raw, _, rest = body.partition(" ")
    op = raw[:-4] if raw.endswith(("_e32", "_e64")) else raw
    return op, raw, [t.strip() for t in rest.split(",")], mods


def _dst_index(dst):
    m = _REG.fullmatch(dst.strip())
    if not m or m.group(1) != "v":
        raise HarnessError(f"unknown destination {dst!r}")
    return int(m.group(2))


def _opsel(mods):
    for m in mods:
        if m.startswith("op_sel:"):
            return tuple(int(b) for b in m[len("op_sel:"):].strip("[]").split(","))
    return (0, 0, 0)


def step(line, vgpr, sgpr, events=None):
    """Execute one generated listing line on the reference's registers. `events` (a Counter), when given, counts what
    each op's inputs were: (op, "nan"), (op, "inf"), (op, "saturated") for an out-of-range or clamped input, and
    (op, "tie") for a product within TIE_WINDOW of k + 1/2 (an exact x.5 for rpi)."""
    op, raw, args, mods = split_line(line)
    if op == "v_mov_b32":
        vgpr[_dst_index(args[0])] = source(args[1], vgpr, sgpr, False)[0]
        return
    if op == "s_mov_b32":
        if not args[0].startswith("s"):
            raise HarnessError(line)
        sgpr[int(args[0][1:])] = source(args[1], vgpr, sgpr, False)[0]
        return
    if op not in FAMILY:
        raise HarnessError(line)
    kind, snorm = FAMILY[op]
    d = _dst_index(args[0])
    sel = _opsel(mods)
    if sel != (0, 0, 0) and kind != "pk16":
        raise HarnessError(f"op_sel outside the modelled forms: {line}")
    if kind in ("flr", "rpi"):
        w, mod, _ = source(args[1], vgpr, sgpr, False)
        w = modify(w, mod, 32)
        if events is not None:
            _count(events, op, f32_value(w), 1, half_up=kind == "rpi")
        vgpr[d] = to_i32(w, kind == "rpi")
        return
    if kind == "norm":
        w, mod, _ = source(args[1], vgpr, sgpr, True)
        h = modify(w & M16, mod, 16)
        if events is not None:
            _count(events, op, f16_value(h), 32767 if snorm else 65535)
        vgpr[d] = (vgpr.get(d, 0) & 0xFFFF0000) | normalize(f16_value(h), snorm)
        return
    halves = []
    for k in (0, 1):
        w, mod, is_reg = source(args[1 + k], vgpr, sgpr, kind == "pk16")
        if kind == "pk32":
            val = f32_value(modify(w, mod, 32))
        else:
            if sel[k] and not is_reg:
                raise HarnessError(f"op_sel high half of a constant: {line}")
            val = f16_value(modify((w >> 16 if sel[k] else w) & M16, mod, 16))
        if events is not None:
            _count(events, op, val, 32767 if snorm else 65535)
        halves.append(normalize(val, snorm))
    # op_sel[2] (f16 forms): S0's result in D[31:16], D[15:0] = 0, S1's result dropped (gfx1036, docs/design.md)
    vgpr[d] = halves[0] << 16 if sel[2] else halves[0] | (halves[1] << 16)


TIE_WINDOW = Fraction(1, 256)


def _count(events, op, val, scale, half_up=False):
    kind, v = val
    if kind != "num":
        events[(op, kind)] += 1
        return
    if scale == 1:
        if abs(v) >= 1 << 31:
            events[(op, "saturated")] += 1
        elif half_up and (v - Fraction(1, 2)) == math.floor(v - Fraction(1, 2)):
            events[(op, "tie")] += 1
        return
    lo = -1 if scale == 32767 else 0
    if v < lo or v > 1:
        events[(op, "saturated")] += 1
        return
    p = v * scale
    if abs(p - math.floor(p) - Fraction(1, 2)) <= TIE_WINDOW:
        events[(op, "tie")] += 1


def reference(lines, events=None):
    """The four exported mrt0 words the ISA gives for a program (seeds and ops; exp and s_endpgm are ignored)."""
    vgpr, sgpr = {}, {}
    for line in lines:
        if line.split()[0] in ("exp", "s_endpgm"):
            continue
        step(line, vgpr, sgpr, events)
    return [vgpr.get(r, 0) for r in EXPORT_REGS]


# ---- edge values ----

def _f32(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


def _f16(x):
    return struct.unpack("<H", struct.pack("<e", x))[0]


def _near_ties32(scale, count, rng):
    """f32 words whose product with `scale` lies next to k + 1/2: the rounded (k + 1/2) / scale and its two f32
    neighbours, for `count` k spread over the range."""
    out = []
    for k in sorted(rng.sample(range(scale), count)):
        w = _f32((k + 0.5) / scale)
        out += [w - 1, w, w + 1]
    return out


def _near_ties16(scale):
    """Every f16 in (0, 1] whose product with `scale` lies within TIE_WINDOW of k + 1/2 (and its negation). Every
    f16 input whose f32-rounded product rounds differently from the exact one is within 2^-9, so all are in here."""
    out = []
    for h in range(0x0001, 0x3C01):
        p = f16_value(h)[1] * scale
        if abs(p - math.floor(p) - Fraction(1, 2)) <= TIE_WINDOW:
            out += [h, h | 0x8000]
    return out


F32_EDGES = (
    0x00000000, 0x80000000, 0x00000001, 0x80000001, 0x007FFFFF, 0x807FFFFF, 0x00800000, 0x80800000,
    0x3F000000, 0xBF000000, 0x3EFFFFFF, 0xBEFFFFFF, 0x3F000001, 0xBF000001, 0x3F800000, 0xBF800000,
    0x3F7FFFFF, 0xBF7FFFFF, 0x3F800001, 0xBF800001, 0x3FC00000, 0xBFC00000, 0x40200000, 0xC0200000,
    _f32(1 / 32767), _f32(-1 / 32767), _f32(1 / 65535), _f32(0.5 / 32767), _f32(0.5 / 65535), _f32(-0.5 / 32767),
    0x4AFFFFFF, 0xCAFFFFFF, 0x4B000000, 0x4B000001, 0xCB000001, 0x4B7FFFFF, 0x4B800000,
    0x4EFFFFFF, 0xCEFFFFFF, 0x4F000000, 0xCF000000, 0x4F000001, 0xCF000001, 0x7F7FFFFF, 0xFF7FFFFF,
    0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00000, 0x7F800001, 0xFF800001, 0x7FBFFFFF, 0xFFC00123,
)
F16_EDGES = (
    0x0000, 0x8000, 0x0001, 0x8001, 0x0200, 0x03FF, 0x83FF, 0x0400, 0x8400, 0x3400, 0xB400, 0x33FF, 0x3401,
    0x3800, 0xB800, 0x37FF, 0xB7FF, 0x3801, 0xB801, 0x3BFF, 0xBBFF, 0x3C00, 0xBC00, 0x3C01, 0xBC01, 0x3E00, 0xBE00,
    0x4000, 0x7BFF, 0xFBFF, 0x7C00, 0xFC00, 0x7E00, 0xFE00, 0x7C01, 0xFC01, 0x7DFF, 0xFE12,
    _f16(1 / 32767), _f16(1 / 65535), _f16(0.5 / 32767), _f16(1.5 / 32767), _f16(0.5 / 65535), _f16(-0.5 / 32767),
)
_EDGE_RNG = random.Random("cvt_diff:edges")
F32_TIES = tuple(_near_ties32(32767, 24, _EDGE_RNG) + _near_ties32(65535, 24, _EDGE_RNG))
F16_TIES = tuple(sorted(set(_near_ties16(32767) + _near_ties16(65535))))


def _rand_f32(rng):
    k = rng.random()
    if k < 0.3:
        return rng.choice(F32_EDGES)
    if k < 0.45:
        return rng.choice(F32_TIES) ^ rng.choice((0, 0x80000000))
    if k < 0.75:   # a value in [-2, 2], where the normalized conversions are not saturated
        return _f32(rng.uniform(-2.0, 2.0))
    if k < 0.9:    # an i32-range value with a fraction, for flr/rpi
        return _f32(rng.uniform(-2.0 ** 31, 2.0 ** 31) / (1 << rng.randrange(0, 30)))
    return rng.getrandbits(32)


def _rand_f16(rng):
    k = rng.random()
    if k < 0.35:
        return rng.choice(F16_EDGES)
    if k < 0.55:
        return rng.choice(F16_TIES)
    if k < 0.85:
        return rng.randrange(0, 0x3C01) | rng.choice((0, 0x8000))
    return rng.getrandbits(16)


def _seed_word(rng):
    k = rng.random()
    if k < 0.5:
        return _rand_f32(rng)
    return _rand_f16(rng) | (_rand_f16(rng) << 16)


# ---- program generation ----

def _source(rng, kind, mod=""):
    if kind == "vgpr":
        return f"{mod}v{rng.randrange(8, 16)}" + ("|" if "|" in mod else "")
    if kind == "sgpr":
        return f"{mod}s{rng.randrange(4, 8)}" + ("|" if "|" in mod else "")
    if kind == "lit":
        w = _seed_word(rng)
        if w <= 64:
            w |= 0x3C000000      # keep it a literal, never an inline constant
        return f"0x{w:x}"
    if kind == "int":
        return str(rng.choice(INT_INLINE))
    return rng.choice(tuple(FLOAT_INLINE))


def encodable(op, kinds, form):
    """GFX10 operand limits: VOP1 _e32 takes anything in S0; VOP3 takes at most one literal and at most two
    constant-bus reads (SGPRs and the literal)."""
    if form == "e32":
        return op in VOP1_OPS and len(kinds) == 1
    return kinds.count("lit") <= 1 and kinds.count("lit") + kinds.count("sgpr") <= 2


def op_line(rng, op, kinds, form="e64", mods=None, sel=(0, 0), omod=""):
    """One instruction. form "e32" or "e64" for the VOP1 ops; the VOP3-only ops always take "e64" (no suffix).
    mods: one MODS entry per source, applied to register sources only. sel: op_sel bits of a pk16 op's S0/S1 and,
    optionally, its destination."""
    mods = mods or ("",) * len(kinds)
    srcs = [_source(rng, k, m if k in ("vgpr", "sgpr") else "") for k, m in zip(kinds, mods)]
    d = f"v{rng.randrange(8, 12)}"
    name = f"{op}_{form}" if op in VOP1_OPS else op
    line = f"{name} {d}, " + ", ".join(srcs)
    if any(sel):
        line += f" op_sel:[{sel[0]},{sel[1]},{sel[2] if len(sel) > 2 else 0}]"
    if omod:
        line += " " + omod
    return line


def _random_op(rng, op):
    kind = FAMILY[op][0]
    n = 2 if kind.startswith("pk") else 1
    form = "e32" if op in VOP1_OPS and rng.random() < 0.4 else "e64"
    while True:
        kinds = tuple(rng.choices(KINDS, weights=(5, 2, 1, 1, 1), k=n))
        if encodable(op, kinds, form):
            break
    mods = tuple(rng.choice(MODS) if form == "e64" else "" for _ in kinds)
    sel = (0, 0)
    if kind == "pk16":
        sel = tuple(rng.randrange(2) if k in ("vgpr", "sgpr") else 0 for k in kinds) + (int(rng.random() < 0.25),)
    omod = ""
    if form == "e64" and op in CLAMP_OPS and rng.random() < 0.25:
        omod = rng.choice(OMODS) if op in OMOD_OPS else "clamp"
    return op_line(rng, op, kinds, form, mods, sel, omod)


def seeds(rng):
    lines = [f"v_mov_b32_e32 v{r}, 0x{_seed_word(rng):x}" for r in range(8, 16)]
    lines += [f"s_mov_b32 s{r}, 0x{_seed_word(rng):x}" for r in range(4, 8)]
    return lines


def program(rng, n_ops, ops=None):
    """Seeds, then n_ops family ops. Returns (lines, ops used)."""
    ops = ops or tuple(FAMILY)
    lines = seeds(rng)
    used = []
    for _ in range(n_ops):
        op = rng.choice(ops)
        used.append(op)
        lines.append(_random_op(rng, op))
    return lines, used


def sweep_ops(ops=None):
    """(op, form, kinds, mods, sel, omod) for every source kind and every modifier setting each op takes."""
    out = []
    for op in ops or FAMILY:
        kind = FAMILY[op][0]
        n = 2 if kind.startswith("pk") else 1
        forms = ("e32", "e64") if op in VOP1_OPS else ("e64",)
        for form in forms:
            for kinds in _product(KINDS, n):
                if not encodable(op, kinds, form):
                    continue
                reg = [k in ("vgpr", "sgpr") for k in kinds]
                mod_sets = [("",) * n] if form == "e32" else \
                    [m for m in _product(MODS, n) if all(r or not x for r, x in zip(reg, m))]
                sel_sets = [(0, 0)]
                if kind == "pk16":
                    sel_sets = [s + (dhi,) for dhi in (0, 1) for s in _product((0, 1), n)
                                if all(r or not x for r, x in zip(reg, s))]
                omods = [""]
                if form == "e64" and op in CLAMP_OPS:
                    omods += list(OMODS) if op in OMOD_OPS else ["clamp"]
                for mods in mod_sets:
                    for sel in sel_sets:
                        for omod in omods:
                            if omod and (any(mods) or any(sel[:2])):
                                continue     # output modifiers once per source kind and destination half
                            out.append((op, form, kinds, mods, sel, omod))
    return out


def _product(items, n):
    if n == 0:
        return [()]
    return [(x,) + rest for x in items for rest in _product(items, n - 1)]


def sweep_program(case, rng):
    op, form, kinds, mods, sel, omod = case
    return seeds(rng) + [op_line(rng, op, kinds, form, mods, sel, omod) for _ in range(3)]


GRID_PER_PROGRAM = 4


def grid_cases(ops=None):
    """(op, input word, second input word) for every op and every edge input. The f32 ops read F32_EDGES and
    F32_TIES; the f16 ops F16_EDGES and F16_TIES. A pk op pairs each input with the next one as S1."""
    out = []
    for op in ops or FAMILY:
        kind = FAMILY[op][0]
        vals = list(F32_EDGES + F32_TIES) if kind in ("flr", "rpi", "pk32") else list(F16_EDGES + F16_TIES)
        for i, v in enumerate(vals):
            out.append((op, v, vals[(i + 1) % len(vals)]))
    return out


def grid_program(chunk, rng):
    """Inputs in v12..v15 (an f16 input in the low half, with a different f16 edge in the high half), then one op per
    case writing v8..v11. A pk16 case reads its two inputs from the halves op_sel picks."""
    lines = [f"v_mov_b32_e32 v{8 + i}, 0x{0xA5A50000 | i:x}" for i in range(4)]
    body = []
    for i, (op, a, b) in enumerate(chunk):
        kind = FAMILY[op][0]
        if kind in ("flr", "rpi"):
            lines.append(f"v_mov_b32_e32 v{12 + i}, 0x{a:x}")
            body.append(f"{op}_e32 v{8 + i}, v{12 + i}")
        elif kind == "norm":
            lines.append(f"v_mov_b32_e32 v{12 + i}, 0x{a | (rng.choice(F16_EDGES) << 16):x}")
            body.append(f"{op}_e32 v{8 + i}, v{12 + i}")
        elif kind == "pk32":
            lines.append(f"v_mov_b32_e32 v{12 + i}, 0x{a:x}")
            lines.append(f"s_mov_b32 s{4 + i}, 0x{b:x}")
            body.append(f"{op} v{8 + i}, v{12 + i}, s{4 + i}")
        else:
            hi = i & 1
            lines.append(f"v_mov_b32_e32 v{12 + i}, 0x{(a << 16 | b) if hi else (b << 16 | a):x}")
            body.append(f"{op} v{8 + i}, v{12 + i}, v{12 + i}" + (" op_sel:[1,0,0]" if hi else " op_sel:[0,1,0]"))
    return lines + body


def exhaustive_programs(ops=None):
    """Every f16 input through each f16 op: four per program for norm (VOP1, low half), eight per program for pknorm
    (S0 and S1 from the two halves of one register). Yields (op, lines)."""
    for op in ops or FAMILY:
        kind = FAMILY[op][0]
        if kind == "norm":
            for base in range(0, 1 << 16, 4):
                lines = [f"v_mov_b32_e32 v{8 + i}, 0x{0x5A5A0000 | i:x}" for i in range(4)]
                lines += [f"v_mov_b32_e32 v{12 + i}, 0x{base + i:x}" for i in range(4)]
                lines += [f"{op}_e32 v{8 + i}, v{12 + i}" for i in range(4)]
                yield op, lines
        elif kind == "pk16":
            for base in range(0, 1 << 16, 8):
                lines = [f"v_mov_b32_e32 v{12 + i}, 0x{(base + 2 * i + 1) << 16 | (base + 2 * i):x}"
                         for i in range(4)]
                lines += [f"{op} v{8 + i}, v{12 + i}, v{12 + i} op_sel:[0,1,0]" for i in range(4)]
                yield op, lines


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


def check(lines, name="ps_cvt_case"):
    """None when both oracles agree, else the first differing word with both values."""
    with tempfile.TemporaryDirectory(prefix="cvt-diff-") as tmp:
        want = reference(lines)
        got = translate_listing(lines, name, tmp)
    return compare(want, got)


def compare(want, got):
    """None when the translator exported the same words as the reference, else what differs. A short export is a
    difference too: zip alone would compare only the words both sides have."""
    if len(got) != len(want):
        return f"exported {len(got)} words, want {len(want)}"
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
    detail = compare(want, got)
    return ("mismatch", detail) if detail else ("match", "")


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
    with tempfile.TemporaryDirectory(prefix="cvt-diff-sweep-") as tmp:
        for i, case in enumerate(sweep_ops(ops)):
            op, form, kinds, mods, sel, omod = case
            tag = f"{op}:{form}:{'/'.join(kinds)}:{'/'.join(m or '.' for m in mods)}:{sel}:{omod}"
            lines = sweep_program(case, random.Random(f"{seed}:{tag}"))
            counts["ops"][op] += 1
            status, detail = run_one(lines, f"ps_cvt_sw{i:04d}", tmp, counts["events"])
            _tally(counts, status, detail)
            if status == "mismatch" and failure is None:
                failure = (f"sweep {tag} (case {i}, seed {seed})", lines, detail)
    return counts, failure


def run_grid(ops=None):
    """The edge grid, GRID_PER_PROGRAM cases per program. Returns (counts, failure); counts["ops"] counts cases."""
    counts = _new_counts()
    failure = None
    cases = grid_cases(ops)
    rng = random.Random("cvt_diff:grid")
    with tempfile.TemporaryDirectory(prefix="cvt-diff-grid-") as tmp:
        for i in range(0, len(cases), GRID_PER_PROGRAM):
            chunk = cases[i:i + GRID_PER_PROGRAM]
            lines = grid_program(chunk, rng)
            counts["ops"].update(op for op, *_ in chunk)
            status, detail = run_one(lines, f"ps_cvt_gr{i // GRID_PER_PROGRAM:04d}", tmp, counts["events"])
            _tally(counts, status, detail)
            if status == "mismatch" and failure is None:
                failure = (f"edge grid program {i // GRID_PER_PROGRAM}", lines, detail)
    return counts, failure


def run_exhaustive(ops=None):
    """Every f16 input through the f16 ops. Returns (counts, failure); counts["ops"] counts op lines (four per
    program)."""
    counts = _new_counts()
    failure = None
    with tempfile.TemporaryDirectory(prefix="cvt-diff-ex-") as tmp:
        for i, (op, lines) in enumerate(exhaustive_programs(ops)):
            counts["ops"][op] += GRID_PER_PROGRAM
            status, detail = run_one(lines, f"ps_cvt_ex{i:05d}", tmp, counts["events"])
            _tally(counts, status, detail)
            if status == "mismatch" and failure is None:
                failure = (f"exhaustive f16 program {i} ({op})", lines, detail)
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
    with tempfile.TemporaryDirectory(prefix="cvt-diff-run-") as tmp:
        for i in range(programs):
            lines, used = program(rng, rng.randint(1, max_ops), ops)
            counts["ops"].update(used)
            status, detail = run_one(lines, f"ps_cvt_{i:04d}", tmp, counts["events"])
            _tally(counts, status, detail)
            if status == "mismatch" and failure is None:
                failure = (f"program {i} (seed {seed})", lines, detail)
    return counts, failure


REJECTED = (
    ("mul:2 on v_cvt_pknorm_u16_f16", "v_cvt_pknorm_u16_f16 v8, v9, v10 mul:2"),
    ("mul:2 on v_cvt_pknorm_i16_f32", "v_cvt_pknorm_i16_f32 v8, v9, v10 mul:2"),
    ("div:2 on v_cvt_pknorm_u16_f32", "v_cvt_pknorm_u16_f32 v8, v9, v10 div:2"),
)


def self_test():
    """True when the reference's hand-pinned words, the rejected forms and the harness canaries all hold."""
    ok = True

    def expect(cond, what):
        nonlocal ok
        print(("ok   " if cond else "FAIL ") + what)
        ok = ok and cond

    # Hand-pinned reference words: the docs/design.md rows and tests/semantic/test_alu_cvt_flr_rpi.py.
    expect(to_i32(0x3EFFFFFF, True) == 0, "rpi(0.49999997) = 0: the + 1/2 is exact, not rounded in binary32")
    expect(to_i32(0x4B000001, True) == 0x00800001, "rpi(8388609.0) = 8388609")
    expect(to_i32(0x4B7FFFFF, True) == 0x00FFFFFF, "rpi(16777215.0) = 16777215")
    expect(to_i32(0xBF000000, True) == 0 and to_i32(0xBFC00000, True) == M32,
           "rpi(-0.5) = 0 and rpi(-1.5) = -1 (floor(x + 1/2))")
    expect(to_i32(0xCF000001, False) == 0x80000000, "flr below -2^31 saturates to INT_MIN")
    expect(to_i32(0x7FC00000, False) == 0x7FFFFFFF and to_i32(0xFFC00000, False) == 0x80000000,
           "flr of a NaN saturates by its sign bit")
    expect(to_i32(0x80000001, False) == 0, "flr(-1e-45) = 0: an f32 denormal reads as zero (gfx1036)")
    expect(normalize(f16_value(0x3C00), True) == 0x7FFF and normalize(f16_value(0xBC00), True) == 0x8001,
           "norm_i16(+-1.0) = 0x7fff / 0x8001")
    expect(normalize(f16_value(0xFC00), True) == 0x8001 and normalize(f16_value(0xFC00), False) == 0,
           "-inf clamps to -1 (i16) and to 0 (u16)")
    expect(normalize(f16_value(0x7E00), True) == 0 and normalize(f32_value(0xFFC00000), False) == 0, "NaN gives 0")
    expect(normalize(f16_value(0x3801), True) == 16399, "norm_i16(0.5 + 2^-11): 16399.499... rounds down once")
    expect(normalize(f32_value(0x3F000000), True) == 0x4000 and normalize(f32_value(0xBF000000), True) == 0xC000
           and normalize(f32_value(0x3F000000), False) == 0x8000,
           "x = +-0.5 is an exact tie (16383.5, 32767.5) and goes to even: 0x4000, 0xc000, 0x8000")
    expect(normalize(f16_value(0x03FF), True) == 2, "an f16 denormal is a value: (2^-14 - 2^-24) * 32767 = 1.99")
    # norm keeps the destination's high half; pknorm reads op_sel halves.
    expect(reference(["v_mov_b32 v8, 0x12340000", "v_mov_b32 v9, 0x3c00", "v_cvt_norm_u16_f16_e32 v8, v9"])[0]
           == 0x1234FFFF, "v_cvt_norm_u16_f16 writes D[15:0] and keeps D[31:16]")
    expect(reference(["v_mov_b32 v9, 0xbc003c00", "v_cvt_pknorm_i16_f16 v8, v9, v9 op_sel:[1,0,0]"])[0]
           == 0x7FFF8001, "v_cvt_pknorm_i16_f16 op_sel:[1,0,0]: S0 from the high half")
    # The destination op_sel and clamp of the f16 forms, as measured on gfx1036 (tests/semantic/
    # test_cvt_pknorm_f16_mods.py): clamp changes nothing, op_sel[2] moves S0's result up and zeroes D[15:0].
    pk = ["v_mov_b32 v8, 0xdeadbeef", "v_mov_b32 v9, 0x3c003400", "v_mov_b32 v10, 0xbc003801"]
    expect(reference(pk + ["v_cvt_pknorm_i16_f16 v8, v9, v10 clamp"])[0] == 0x400F2000,
           "v_cvt_pknorm_i16_f16 clamp: a no-op (0.25 -> 0x2000, 0.5 + 2^-11 -> 0x400f)")
    expect(reference(pk + ["v_cvt_pknorm_i16_f16 v8, v9, v10 op_sel:[0,0,1] clamp"])[0] == 0x20000000,
           "v_cvt_pknorm_i16_f16 op_sel:[0,0,1]: S0's result in D[31:16], D[15:0] = 0, S1 dropped")
    expect(reference(pk + ["v_cvt_pknorm_u16_f16 v8, v9, v10 op_sel:[0,1,1]"])[0] == 0x40000000,
           "v_cvt_pknorm_u16_f16 op_sel:[0,1,1]: the high-half S1 is read but dropped")
    expect(reference(["v_cvt_pknorm_u16_f16 v8, 1.0, 0.5"])[0] == 0x8000FFFF,
           "float inline constants read as f16 values in a 16-bit operand (1.0 -> 0xffff, 0.5 -> 0x8000)")

    # The two-oracle path agrees on a fixed program, and a sabotaged word is caught.
    lines = ["v_mov_b32 v9, 0x3effffff", "v_mov_b32 v10, 0xbc003801",
             "v_cvt_rpi_i32_f32_e32 v8, v9", "v_cvt_norm_i16_f16_e64 v11, -v10"]
    expect(check(lines) is None, "a hand-pinned program matches on both oracles")
    real = translate_listing
    try:
        globals()["translate_listing"] = lambda l, n, t: real(l, n, t)[:3] + [1]
        expect(check(lines) is not None, "a sabotaged export word is reported")
    finally:
        globals()["translate_listing"] = real

    # Forms the contract rows say are refused, not guessed.
    with tempfile.TemporaryDirectory(prefix="cvt-diff-neg-") as tmp:
        for what, bad in REJECTED:
            try:
                translate_listing(["v_mov_b32 v9, 1", "v_mov_b32 v10, 2", bad], "ps_cvt_neg", tmp)
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
    ap.add_argument("--ops", default=None, help="comma-separated opcodes to cover (default: all eight)")
    ap.add_argument("--no-sweep", action="store_true", help="skip the source-kind sweep and the edge grid")
    ap.add_argument("--exhaustive-f16", action="store_true", help="also run every f16 input through the f16 ops")
    ap.add_argument("--list-ops", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)

    if args.list_ops:
        for op, form in OP_FORMS.items():
            print(f"{op:22s} {form}")
        return 0
    if args.self_test:
        return 0 if self_test() else 1

    if args.programs < 0:
        ap.error("--programs must be nonnegative")
    if args.max_ops < 1:
        ap.error("--max-ops must be at least 1")
    if args.no_sweep and not args.exhaustive_f16 and args.programs == 0:
        ap.error("nothing to run: pass --programs >= 1, --exhaustive-f16, or omit --no-sweep")

    ops = None
    if args.ops is not None:
        tokens = [o.strip() for o in args.ops.split(",")]
        if not tokens or any(not o for o in tokens):
            ap.error("--ops must be a comma-separated list of opcodes")
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
            ("exhaustive f16", args.exhaustive_f16, lambda: run_exhaustive(ops)),
            ("random", args.programs > 0, lambda: run_suite(args.programs, args.seed, args.max_ops, ops)),
        ):
            if not enabled:
                continue
            counts, first = runner()
            ran = counts["pass"] + counts["mismatch"] + sum(counts["skip"].values())
            total_pass += counts["pass"]
            total_mismatch += counts["mismatch"]
            total_skip += sum(counts["skip"].values())
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
    print("inputs per op (NaN / inf / saturated / near tie): " + ", ".join(
        f"{op} {events[(op, 'nan')]}/{events[(op, 'inf')]}/{events[(op, 'saturated')]}/{events[(op, 'tie')]}"
        for op in sorted(per_op)))
    print(f"cvt family: {total_pass}/{total_run} programs match, {total_mismatch} mismatch "
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
