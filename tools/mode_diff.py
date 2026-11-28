r"""Differential fuzzer for MODE.FP_ROUND[3:2], the f16/f64 rounding field shaders switch at run time (standard
library only).

The corpus writes this field 9,417 times (docs/design.md, Known approximations).
`s_setreg_imm32_b32 hwreg(HW_REG_MODE, 2, 2), 3` sets f16 rounding toward zero and `..., 0` sets it back to nearest
even; `s_round_mode` writes the same two bits (bits 3:2 of its immediate). While the field is toward zero, every VALU
f16 result except the seven transcendentals rounds toward zero, and a finite overflow gives +-65504. No other fuzzer
writes MODE (tools/f32_fuzz.py only reads it with s_getreg_b32), so the f16 fuzzers only ever run in nearest-even
mode.

Two oracles, three stages:

  * Exact reference (the sweep and the random programs). Seeded straight-line programs mix MODE writes with the
    19 ops below. The four exported mrt0 words go to two places:
      - the reference below, which tracks the mode and rounds the exact rational result (fractions.Fraction) once,
        in that mode. It is written from the rules docs/design.md records and shares no code with the translator;
      - tools/rdna2spirv.py, with the SPIR-V run in tests/semantic/spirv_mini_interp.py.
    The comparison is bit for bit, except that a NaN the reference computed matches any NaN (payloads are not
    modelled). A NaN that only passes through v_ldexp_f16 is compared exactly.
  * Mode invariance (the invariance stage). Programs of ops whose result must not depend on the f16 rounding field
    (the seven f16 transcendentals, the exact f16 ops, f16-to-int conversions, v_cvt_pkrtz_f16_f32, f32 arithmetic)
    run with their MODE writes and again with every MODE write removed. The words must be identical.

Reference rules (docs/design.md, "f16 rounding" and "Known approximations"):
  - f16 operands and results keep denormals (denorm_mode_16_64 = 3). An f32 operand of v_fma_mix* reads as a zero of
    its sign when it is denormal (denorm_mode_32 = 0, SHADER-298).
  - add, sub, subrev, mul, fma, fmac, fmamk, fmaak, the packed forms and v_fma_mix{lo,hi}_f16: the exact result,
    rounded once. Nearest even gives +-inf from 65520 up. Toward zero truncates, and a finite result past 65504
    gives +-65504. An exact zero sum is -0 only when both terms are -0.
  - v_cvt_f16_f32, v_cvt_f16_u16, v_cvt_f16_i16: the source value rounded the same way. VOP3 mul:2 / mul:4 / div:2
    on v_cvt_f16_f32 applies to the rounded f16, as Gen.f16_omod documents (a zero or denormal gives +0, div:2 of
    a smallest normal gives a signed zero, finite overflow gives +-inf, or +-65504 toward zero), then clamp.
  - v_ldexp_f16: S0 * 2^S1.i16, rounded. Zero gives a signed zero, inf and NaN pass through. With clamp, a negative
    result or a NaN gives +0. The toward-zero words are #5310's change and are not measured on hardware.
  - v_fract_f16: S0 - floor(S0), rounded (fract(-(2^-10 + 2^-20)) = 1 - 2^-10 - 2^-20 needs 20 bits, so the mode
    decides it). A result of 1.0 becomes 0x3bff. An infinity gives a NaN, or +0 with clamp.
  - clamp: to [0, 1] on the exact value, before rounding. Not judged on a NaN or a -0 (FClamp is undefined there in
    SPIR-V, and docs/design.md does not pin it). A program whose translation hits a value SPIR-V leaves undefined
    (OpFDiv by zero, Log2 or Sqrt of a negative, which the mini interpreter refuses) is not judged either.
  - The mode starts at nearest even. A write of 3 selects toward zero and a write of 0 selects nearest even (the
    two values the corpus uses; the translator refuses 1 and 2).

Program shape: v8..v15 seeded with v_mov_b32. Before an op, its operands are usually reseeded with values built for
it: an addend a few binades below the other operand, an f32 word near an f16 tie, the overflow edge, an f16 denormal,
a small negative for fract, an integer past 2048. Destinations are v8..v11, so later ops also read earlier results.
The program ends with `exp mrt0 v8, v9, v10, v11`. The sweep runs every form of every op under each of the four
MODE writes (setreg 0 / 3, s_round_mode 0x0 / 0xc). The random programs put a MODE write before about one op in
three.

Not covered: v_add_f64 (the mini interpreter has no binary64 arithmetic; tests/test_mode_round.py checks it against
a Fraction oracle), v_interp_p2_f16 (needs a pixel input), the _sdwa and _dpp forms (tools/sdwa_diff.py's domain),
omod on f16 arithmetic (SHADER-297, open) and op_sel on an f32 operand of v_fma_mix*.

Exit code 0 when every judged program matches, 1 on a mismatch (the first one is printed with its listing) or a
translator skip (every generated form is one the translator takes), 2 on a harness error.

Usage:
    python -I tools/mode_diff.py [--programs N] [--invariance N] [--seed S] [--max-ops N] [--ops v_add_f16,...]
    python -I tools/mode_diff.py --list-ops
    python -I tools/mode_diff.py --self-test
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
DSTS = (8, 9, 10, 11)
VGPRS = tuple(range(8, 16))
OPERAND_REGS = (12, 13, 14, 15)

SETREG = "s_setreg_imm32_b32 hwreg(HW_REG_MODE, 2, 2), {}"
MODE_WRITES = (SETREG.format(0), SETREG.format(3), "s_round_mode 0x0", "s_round_mode 0xc")
RTZ = 3

# op -> (forms, number of sources). Forms: e32 / e64 (printed suffix), vop3 / vop3p / k (no suffix).
FAMILY = {
    "v_add_f16": (("e32", "e64"), 2), "v_sub_f16": (("e32", "e64"), 2), "v_subrev_f16": (("e32", "e64"), 2),
    "v_mul_f16": (("e32", "e64"), 2), "v_fma_f16": (("vop3",), 3), "v_fmac_f16": (("e32", "e64"), 2),
    "v_fmamk_f16": (("k",), 2), "v_fmaak_f16": (("k",), 2),
    "v_pk_add_f16": (("vop3p",), 2), "v_pk_mul_f16": (("vop3p",), 2), "v_pk_fma_f16": (("vop3p",), 3),
    "v_pk_fmac_f16": (("e32",), 2),
    "v_fma_mixlo_f16": (("vop3p",), 3), "v_fma_mixhi_f16": (("vop3p",), 3),
    "v_cvt_f16_f32": (("e32", "e64"), 1), "v_cvt_f16_u16": (("e32", "e64"), 1), "v_cvt_f16_i16": (("e32", "e64"), 1),
    "v_ldexp_f16": (("e32", "e64"), 2), "v_fract_f16": (("e32", "e64"), 1),
}
OP_FORMS = {
    "v_add_f16": "_e32 d, s0, s1 | _e64 d, [-][|]s0[|], [-][|]s1[|] [op_sel:[a,b,d]] [clamp]",
    "v_sub_f16": "as v_add_f16",
    "v_subrev_f16": "as v_add_f16 (S1 - S0)",
    "v_mul_f16": "as v_add_f16",
    "v_fma_f16": "d, [-][|]s0[|], [-][|]s1[|], [-][|]s2[|] [op_sel:[a,b,c,d]] [clamp]",
    "v_fmac_f16": "_e32 d, s0, s1 | _e64 d, [-][|]s0[|], [-][|]s1[|] [clamp] (d is the addend)",
    "v_fmamk_f16": "d, s0, K, s1 (s0 * K + s1)",
    "v_fmaak_f16": "d, s0, s1, K (s0 * s1 + K)",
    "v_pk_add_f16": "d, s0, s1 [op_sel:[a,b]] [op_sel_hi:[a,b]] [neg_lo:[a,b]] [neg_hi:[a,b]] [clamp]",
    "v_pk_mul_f16": "as v_pk_add_f16",
    "v_pk_fma_f16": "d, s0, s1, s2 [op_sel:[a,b,c]] [op_sel_hi:[a,b,c]] [neg_lo:[a,b,c]] [neg_hi:[a,b,c]] [clamp]",
    "v_pk_fmac_f16": "_e32 d, s0, s1 (both halves of d are the addends)",
    "v_fma_mixlo_f16": "d, [-][|]s0[|], [-][|]s1[|], [-][|]s2[|] op_sel:[a,b,c] op_sel_hi:[x,y,z] [clamp]",
    "v_fma_mixhi_f16": "as v_fma_mixlo_f16, result in the high half",
    "v_cvt_f16_f32": "_e32 d, s0 | _e64 d, [-][|]s0[|] [mul:2|mul:4|div:2] [clamp]",
    "v_cvt_f16_u16": "_e32 d, s0 | _e64 d, s0 [op_sel:[a,d]] [clamp]",
    "v_cvt_f16_i16": "as v_cvt_f16_u16",
    "v_ldexp_f16": "_e32 d, s0, s1 | _e64 d, [-][|]s0[|], s1 [op_sel:[a,b,d]] [clamp]",
    "v_fract_f16": "_e32 d, s0 | _e64 d, [-][|]s0[|] [op_sel:[a,d]] [clamp]",
}
# The invariance stage: results that must not depend on MODE.FP_ROUND[3:2].
INVARIANT = (
    "v_rcp_f16", "v_rsq_f16", "v_sqrt_f16", "v_exp_f16", "v_log_f16", "v_sin_f16", "v_cos_f16",
    "v_floor_f16", "v_ceil_f16", "v_trunc_f16", "v_rndne_f16", "v_frexp_mant_f16", "v_frexp_exp_i16_f16",
    "v_cvt_f32_f16", "v_cvt_i16_f16", "v_cvt_u16_f16", "v_max_f16", "v_min_f16", "v_cvt_pkrtz_f16_f32",
    "v_add_f32", "v_sub_f32", "v_mul_f32", "v_fma_f32", "v_fma_mix_f32", "v_med3_f16", "v_pk_max_f16",
    "v_pk_min_f16",
)
INV_UNARY_OPSEL = INVARIANT[:13]       # f16 VOP1 ops that also take an _e64 op_sel:[a,d]
INV_VOP2 = ("v_max_f16", "v_min_f16", "v_cvt_pkrtz_f16_f32", "v_add_f32", "v_sub_f32", "v_mul_f32")
INV_VOP3 = ("v_fma_f32", "v_fma_mix_f32", "v_med3_f16")
INV_VOP3P = ("v_pk_max_f16", "v_pk_min_f16")

GRID_PER_PROGRAM = 4


class HarnessError(Exception):
    """The generator or the reference met something outside the modelled forms."""


class NotJudged(Exception):
    """The reference does not judge this program (the reason is counted)."""


# ---- numbers: (kind, sign, magnitude), kind "num" / "inf" / "nan", magnitude a Fraction ----

ZERO = ("num", 0, Fraction(0))
ONE = ("num", 0, Fraction(1))
NAN = ("nan", 0, Fraction(0))
HALF = Fraction(1, 2)


def ilog2(q):
    """floor(log2(q)) for a Fraction q > 0."""
    e = q.numerator.bit_length() - q.denominator.bit_length()
    return e if Fraction(2) ** e <= q else e - 1


def dec16(h):
    s, e, m = h >> 15 & 1, h >> 10 & 31, h & 0x3FF
    if e == 31:
        return NAN if m else ("inf", s, Fraction(0))
    return ("num", s, Fraction(m, 1 << 24) if e == 0 else Fraction(1024 + m) * Fraction(2) ** (e - 25))


def dec32(w, flush=False):
    s, e, m = w >> 31 & 1, w >> 23 & 255, w & 0x7FFFFF
    if e == 255:
        return NAN if m else ("inf", s, Fraction(0))
    if e == 0:
        return ("num", s, Fraction(0) if flush else Fraction(m) * Fraction(2) ** -149)
    return ("num", s, Fraction((1 << 23) + m) * Fraction(2) ** (e - 150))


def neg(v):
    return v if v[0] == "nan" else (v[0], v[1] ^ 1, v[2])


def fabs(v):
    return v if v[0] == "nan" else (v[0], 0, v[2])


def signed(v):
    return -v[2] if v[1] else v[2]


def add(x, y):
    if x[0] == "nan" or y[0] == "nan":
        return NAN
    if x[0] == "inf" or y[0] == "inf":
        if x[0] == y[0] == "inf" and x[1] != y[1]:
            return NAN
        return x if x[0] == "inf" else y
    s = signed(x) + signed(y)
    if s == 0:   # an exact zero is +0 in both modes unless both terms are -0
        return ("num", x[1] & y[1], Fraction(0))
    return ("num", int(s < 0), abs(s))


def mul(x, y):
    if x[0] == "nan" or y[0] == "nan":
        return NAN
    s = x[1] ^ y[1]
    if x[0] == "inf" or y[0] == "inf":
        if (x[0] == "num" and x[2] == 0) or (y[0] == "num" and y[2] == 0):
            return NAN
        return ("inf", s, Fraction(0))
    return ("num", s, x[2] * y[2])


def fma(x, y, z):
    return add(mul(x, y), z)


def round16(v, rtz):
    """f16 bits of v rounded once: nearest even or toward zero (denormals kept). None for a NaN."""
    kind, s, mag = v
    if kind == "nan":
        return None
    if kind == "inf":
        return s << 15 | 0x7C00
    if mag == 0:
        return s << 15
    e = ilog2(mag)
    if e > 15:
        return s << 15 | (0x7BFF if rtz else 0x7C00)
    e = max(e, -14)
    q = mag / Fraction(2) ** (e - 10)          # [1024, 2048) for a normal, [0, 1024) below the normal range
    n = q.numerator // q.denominator
    rem = q - n
    if not rtz and (rem > HALF or (rem == HALF and n & 1)):
        n += 1
    return s << 15 | (((e + 14) << 10) + n)    # n = 2048 carries into the exponent (0x7c00 past 65504)


def clamp01(v):
    """VOP3 clamp through FClamp: [0, 1] on the exact value. A NaN or a -0 is not judged."""
    kind, s, mag = v
    if kind == "nan":
        raise NotJudged("clamp of a NaN")
    if s:
        if kind == "num" and mag == 0:
            raise NotJudged("clamp of -0")
        return ZERO
    if kind == "inf" or mag > 1:
        return ONE
    return v


def is_nan16(h):
    return h & 0x7C00 == 0x7C00 and h & 0x3FF != 0


def omod16(h, k, rtz):
    """Gen.f16_omod on v_cvt_f16_f32's rounded bits."""
    if h is None:
        return None
    e, sign = h >> 10 & 31, h & 0x8000
    if e == 31:
        return h
    if e == 0:
        return 0
    if k == "div:2":
        return sign if e == 1 else h - 0x400
    d = 1 if k == "mul:2" else 2
    if e >= 31 - d:
        return sign | (0x7BFF if rtz else 0x7C00)
    return h + (d << 10)


# ---- the program text ----

_SRC = re.compile(r"(-?)(\|?)v(\d+)\|?$")
_BITS = re.compile(r"(op_sel|op_sel_hi|neg_lo|neg_hi):\[([01](?:,[01])*)\]$")


def split_line(line):
    """-> (op, operands, modifiers) for the generated forms: operands end at the first token without a comma."""
    toks = line.split()
    args, i = [], 1
    while i < len(toks):
        t = toks[i]
        i += 1
        args.append(t.rstrip(","))
        if not t.endswith(","):
            break
    return toks[0], args, toks[i:]


def bits(mods, name, n, default=0):
    for m in mods:
        g = _BITS.match(m)
        if g and g.group(1) == name:
            return ([int(b) for b in g.group(2).split(",")] + [0] * n)[:n]
    return [default] * n


def reg(tok):
    g = _SRC.match(tok)
    if not g:
        raise HarnessError(f"operand {tok!r}")
    return int(g.group(3)), bool(g.group(1)), bool(g.group(2))


class Machine:
    """The VGPR file as words, the halves that hold a computed NaN (payload not modelled), and the f16 rounding mode."""

    def __init__(self):
        self.v = [0] * 256
        self.unk = set()
        self.mode = 0

    def write_word(self, r, w):
        self.v[r] = w & M32
        self.unk.discard((r, 0))
        self.unk.discard((r, 1))

    def half(self, r, hi):
        return None if (r, hi) in self.unk else self.v[r] >> (16 * hi) & M16

    def set_half(self, r, hi, h):
        if h is None:
            self.unk.add((r, hi))
            h = 0x7E00
        else:
            self.unk.discard((r, hi))
        sh = 16 * hi
        self.v[r] = (self.v[r] & ~(M16 << sh) & M32) | (h & M16) << sh

    def word(self, r):
        if (r, 0) in self.unk or (r, 1) in self.unk:
            raise NotJudged("reads a computed NaN half as f32 or integer bits")
        return self.v[r]

    def int_half(self, r, hi):
        h = self.half(r, hi)
        if h is None:
            raise NotJudged("reads a computed NaN half as an integer")
        return h

    def f16(self, tok, hi):
        """An f16 operand (register half, or the literal K) with its neg/abs."""
        if tok.startswith("0x"):
            return dec16(int(tok, 16) & M16)
        r, n, a = reg(tok)
        h = self.half(r, hi)
        v = NAN if h is None else dec16(h)
        return neg(fabs(v) if a else v) if n else (fabs(v) if a else v)

    def f32(self, tok, flush=False):
        r, n, a = reg(tok)
        v = dec32(self.word(r), flush)
        return neg(fabs(v) if a else v) if n else (fabs(v) if a else v)


def _note(events, op, v, rtz):
    if events is None or v[0] != "num" or v[2] == 0:
        return
    ev = events.setdefault(op, Counter())
    ev["toward zero" if rtz else "nearest"] += 1
    if round16(v, True) != round16(v, False):
        ev["modes differ"] += 1
    if v[2] > 65504:
        ev["overflow"] += 1
    elif v[2] < Fraction(1, 1 << 14):
        ev["denormal"] += 1
    e = max(ilog2(v[2]), -14)
    q = v[2] / Fraction(2) ** (e - 10)
    if q - q.numerator // q.denominator == HALF:
        ev["tie"] += 1


def _out(mc, op, v, events):
    rtz = mc.mode == RTZ
    _note(events, op, v, rtz)
    return round16(v, rtz)


def base_op(op):
    return re.sub(r"_e(32|64)$", "", op)


def step(line, mc, events=None):
    """Run one line on the reference machine."""
    if line.startswith("s_setreg_imm32_b32 hwreg(HW_REG_MODE, 2, 2),"):
        mc.mode = int(line.rsplit(",", 1)[1], 0) & 3
        return
    op, args, mods = split_line(line)
    if op == "s_round_mode":
        mc.mode = int(args[0], 0) >> 2 & 3
        return
    if op == "v_mov_b32":
        mc.write_word(reg(args[0])[0], int(args[1], 0))
        return
    b = base_op(op)
    if mc.mode not in (0, RTZ):
        raise HarnessError(f"mode {mc.mode}")
    clamp = "clamp" in mods
    d = reg(args[0])[0]
    if b in ("v_add_f16", "v_sub_f16", "v_subrev_f16", "v_mul_f16"):
        sel = bits(mods, "op_sel", 3)
        x, y = mc.f16(args[1], sel[0]), mc.f16(args[2], sel[1])
        v = {"v_add_f16": lambda: add(x, y), "v_sub_f16": lambda: add(x, neg(y)),
             "v_subrev_f16": lambda: add(y, neg(x)), "v_mul_f16": lambda: mul(x, y)}[b]()
        mc.set_half(d, sel[2], _out(mc, b, clamp01(v) if clamp else v, events))
    elif b == "v_fma_f16":
        sel = bits(mods, "op_sel", 4)
        v = fma(*(mc.f16(t, sel[i]) for i, t in enumerate(args[1:4])))
        mc.set_half(d, sel[3], _out(mc, b, clamp01(v) if clamp else v, events))
    elif b == "v_fmac_f16":
        v = fma(mc.f16(args[1], 0), mc.f16(args[2], 0), mc.f16(args[0], 0))
        mc.set_half(d, 0, _out(mc, b, clamp01(v) if clamp else v, events))
    elif b in ("v_fmamk_f16", "v_fmaak_f16"):
        v = fma(*(mc.f16(t, 0) for t in args[1:4]))     # in print order both are a[1] * a[2] + a[3]
        mc.set_half(d, 0, _out(mc, b, v, events))
    elif b in ("v_pk_add_f16", "v_pk_mul_f16", "v_pk_fma_f16"):
        n = 3 if b == "v_pk_fma_f16" else 2
        sel, selhi = bits(mods, "op_sel", n), bits(mods, "op_sel_hi", n, 1)
        nlo, nhi = bits(mods, "neg_lo", n), bits(mods, "neg_hi", n)
        out = []
        for lane, (hs, ns) in enumerate(((sel, nlo), (selhi, nhi))):
            vals = [mc.f16(t, hs[i]) for i, t in enumerate(args[1:1 + n])]
            vals = [neg(x) if ns[i] else x for i, x in enumerate(vals)]
            v = add(*vals) if b == "v_pk_add_f16" else mul(*vals) if b == "v_pk_mul_f16" else fma(*vals)
            out.append(_out(mc, b, clamp01(v) if clamp else v, events))
        mc.set_half(d, 0, out[0])
        mc.set_half(d, 1, out[1])
    elif b == "v_pk_fmac_f16":
        out = [_out(mc, b, fma(mc.f16(args[1], k), mc.f16(args[2], k), mc.f16(args[0], k)), events) for k in (0, 1)]
        mc.set_half(d, 0, out[0])
        mc.set_half(d, 1, out[1])
    elif b in ("v_fma_mixlo_f16", "v_fma_mixhi_f16"):
        sel, selhi = bits(mods, "op_sel", 3), bits(mods, "op_sel_hi", 3)
        vals = [mc.f16(t, sel[i]) if selhi[i] else mc.f32(t, flush=True) for i, t in enumerate(args[1:4])]
        v = fma(*vals)
        mc.set_half(d, int(b == "v_fma_mixhi_f16"), _out(mc, b, clamp01(v) if clamp else v, events))
    elif b == "v_cvt_f16_f32":
        h = _out(mc, b, mc.f32(args[1]), events)
        k = next((m for m in mods if m in ("mul:2", "mul:4", "div:2")), None)
        if k:
            h = omod16(h, k, mc.mode == RTZ)
        if clamp:
            if h is None or is_nan16(h):
                raise NotJudged("clamp of a NaN")
            if h == 0x8000:
                raise NotJudged("clamp of -0")
            h = 0 if h & 0x8000 else min(h, 0x3C00)
        mc.set_half(d, 0, h)
    elif b in ("v_cvt_f16_u16", "v_cvt_f16_i16"):
        sel = bits(mods, "op_sel", 2)
        u = mc.int_half(reg(args[1])[0], sel[0])
        i = u - 0x10000 if b == "v_cvt_f16_i16" and u & 0x8000 else u
        v = ("num", int(i < 0), Fraction(abs(i)))
        mc.set_half(d, sel[1], _out(mc, b, clamp01(v) if clamp else v, events))
    elif b == "v_ldexp_f16":
        sel = bits(mods, "op_sel", 3)
        r, n, a = reg(args[1])
        h = mc.half(r, sel[0])
        k = mc.int_half(reg(args[2])[0], sel[1])
        k = k - 0x10000 if k & 0x8000 else k
        if h is not None:
            h = (h & 0x7FFF if a else h) ^ (0x8000 if n else 0)
            x = dec16(h)
            if x[0] == "num" and x[2] != 0:
                h = _out(mc, b, ("num", x[1], x[2] * Fraction(2) ** max(-64, min(64, k))), events)
        if clamp:   # on the rounded bits: negative (or -0) and NaN give +0
            h = 0 if h is None or is_nan16(h) or h & 0x8000 else min(h, 0x3C00)
        mc.set_half(d, sel[2], h)
    elif b == "v_fract_f16":
        sel = bits(mods, "op_sel", 2)
        x = mc.f16(args[1], sel[0])
        if x[0] == "nan":
            if clamp:
                raise NotJudged("clamp of a NaN")
            h = None
        elif x[0] == "inf":
            h = 0 if clamp else None
        else:
            q = signed(x)
            v = ("num", 0, q - math.floor(q))
            h = _out(mc, b, v, events)
            h = 0x3BFF if h == 0x3C00 else h
        mc.set_half(d, sel[1], h)
    else:
        raise HarnessError(f"no reference for {op}")


def reference(lines, events=None):
    """-> (the four exported words, the set of (register, half) that hold a computed NaN)."""
    mc = Machine()
    for line in lines:
        step(line, mc, events)
    return [mc.v[r] for r in EXPORT_REGS], {(i, h) for i, r in enumerate(EXPORT_REGS) for h in (0, 1)
                                            if (r, h) in mc.unk}


def compare(want, nans, got):
    """None when the translator's words match the reference's, else a description of the first difference."""
    for i, (w, g) in enumerate(zip(want, got)):
        for hi in (0, 1):
            wh, gh = w >> (16 * hi) & M16, g >> (16 * hi) & M16
            if (i, hi) in nans:
                if not is_nan16(gh):
                    return f"v{EXPORT_REGS[i]}.{'hi' if hi else 'lo'}: want a NaN, got 0x{gh:04x}"
            elif wh != gh:
                return f"v{EXPORT_REGS[i]}.{'hi' if hi else 'lo'}: want 0x{wh:04x}, got 0x{gh:04x}"
    return None


# ---- operand values ----

BOUND16 = (0x0001, 0x03FF, 0x0400, 0x3BFF, 0x3C00, 0x3C01, 0x7BFF, 0x7BFE, 0x5800, 0x1400, 0x6800, 0x2000)
SPECIAL16 = (0x0000, 0x8000, 0x7C00, 0xFC00, 0x7E00, 0x7C01)
SPECIAL32 = (0x00000000, 0x80000000, 0x7F800000, 0xFF800000, 0x7FC00000, 0x00000001, 0x807FFFFF)


def rand_half(rng, special=0.04):
    k = rng.random()
    if k < special:
        return rng.choice(SPECIAL16)
    if k < 0.3:
        h = (rng.choice(BOUND16) + rng.randint(-3, 3)) & 0x7FFF
    elif k < 0.42:
        h = rng.randint(1, 0x3FF)
    else:
        h = rng.randint(1, 30) << 10 | rng.getrandbits(10)
    if h >> 10 == 31:
        h = 0x7BFF
    return h | rng.getrandbits(1) << 15


def near_half(rng, x):
    """An addend that leaves bits below x's last place: a few binades under x, either sign, exactly half an ulp, or
    so far under x that the f32 sum rounds back to x (only the exact sum then tells the toward-zero f16 result)."""
    e = x >> 10 & 31
    if e in (0, 31) or rng.random() < 0.25:
        return rand_half(rng)
    sign = rng.getrandbits(1) << 15
    if e >= 16 and rng.random() < 0.25:     # at most half an f32 ulp of x: the f32 sum rounds back to |x|
        if e >= 25 and rng.random() < 0.3:
            return (e - 24) << 10 | sign        # exactly half an f32 ulp
        if e >= 26:
            return rng.randint(1, e - 25) << 10 | rng.getrandbits(10) | sign
        return rng.randint(1, min(0x3FF, (1 << (e - 15)) - 1)) | sign
    if rng.random() < 0.2:      # x +- half an ulp: a tie
        h = round16(("num", 0, Fraction(2) ** (max(e, 1) - 26)), False)
        return h | sign
    ey = e - rng.randint(1, 14)
    if ey < 1:
        return rng.randint(1, 0x3FF) | sign
    return ey << 10 | rng.getrandbits(10) | sign


def addend_for(rng, x, y):
    """z for x * y + z: near the product, its exact negation, or any half."""
    p = mul(dec16(x), dec16(y))
    k = rng.random()
    if p[0] != "num" or k < 0.25:
        return rand_half(rng)
    pb = round16(p, False)
    if k < 0.35 and pb is not None:
        return pb ^ 0x8000          # cancels to the product's rounding error
    return near_half(rng, pb & 0x7FFF) if pb is not None and pb & 0x7C00 != 0x7C00 else rand_half(rng)


def rand_f32(rng):
    k = rng.random()
    s = rng.getrandbits(1) << 31
    if k < 0.04:
        return rng.choice(SPECIAL32)
    if k < 0.25:   # an f16 tie, or one f32 ulp either side
        e = rng.randint(113, 142)
        return s | e << 23 | rng.getrandbits(10) << 13 | (0x1000 + rng.choice((0, 0, -1, 1)))
    if k < 0.38:   # the overflow edge: 65504 .. 65536 and a little past
        return s | (0x477FE000 + rng.randint(-0x4000, 0x24000))
    if k < 0.5:    # the f16 denormal range
        return s | rng.randint(102, 112) << 23 | rng.getrandbits(23)
    return s | rng.randint(100, 150) << 23 | rng.getrandbits(23)


def rand_int16(rng):
    return rng.choice((rng.getrandbits(16), rng.randint(2049, 4200), 0xFFFF - rng.randint(0, 40),
                       0x7FFF - rng.randint(0, 40), 0x8000 + rng.randint(0, 40), rng.randint(0, 2048)))


def rand_fract_half(rng):
    """Mostly a small negative with low bits, where 1 + x needs rounding."""
    if rng.random() < 0.6:
        return 0x8000 | rng.randint(1, 14) << 10 | rng.getrandbits(10)
    return rand_half(rng)


def pack(lo, hi):
    return (hi & M16) << 16 | lo & M16


def operand_words(rng, op, n, mods):
    """Crafted words for the operands of one op (the accumulator last, for v_fmac_f16 / v_pk_fmac_f16)."""
    if op in ("v_add_f16", "v_sub_f16", "v_subrev_f16", "v_pk_add_f16"):
        xs = [rand_half(rng), rand_half(rng)]
        ys = [near_half(rng, x & 0x7FFF) if rng.random() < 0.8 else rand_half(rng) for x in xs]
        return [pack(*xs), pack(*ys)]
    if op in ("v_mul_f16", "v_pk_mul_f16"):
        return [pack(rand_half(rng), rand_half(rng)) for _ in range(2)]
    if op in ("v_fma_f16", "v_fmac_f16", "v_fmamk_f16", "v_fmaak_f16", "v_pk_fma_f16", "v_pk_fmac_f16"):
        xs, ys = [rand_half(rng), rand_half(rng)], [rand_half(rng), rand_half(rng)]
        zs = [addend_for(rng, x, y) for x, y in zip(xs, ys)]
        return [pack(*xs), pack(*ys), pack(*zs)]
    if op in ("v_fma_mixlo_f16", "v_fma_mixhi_f16"):
        hi = bits(mods, "op_sel_hi", 3)
        out = []
        for i in range(3):
            if hi[i]:
                out.append(pack(rand_half(rng), rand_half(rng)))
            elif rng.random() < 0.15:
                out.append(rng.choice(SPECIAL32))
            else:
                out.append(rng.getrandbits(1) << 31 | rng.randint(110, 140) << 23 | rng.getrandbits(23))
        if not any(hi) and rng.random() < 0.5:      # x * y near an f16 tie, z small: a single-rounding case
            x, z = out[0], out[2]
            out[1] = rng.getrandbits(1) << 31 | 0x3F800000
            out[0] = x & 0x80000000 | rng.randint(113, 142) << 23 | rng.getrandbits(10) << 13 | 0x1000
            out[2] = z & 0x80000000 | rng.randint(80, 110) << 23 | rng.getrandbits(23)
        return out
    if op == "v_cvt_f16_f32":
        if ("mul:2" in mods or "mul:4" in mods) and rng.random() < 0.5:    # an f16 at 2^14 .. 65504: the omod overflows
            return [rng.getrandbits(1) << 31 | rng.randint(141, 142) << 23 | rng.getrandbits(23)]
        if "div:2" in mods and rng.random() < 0.5:     # an f16 denormal or a smallest normal before div:2
            return [rng.getrandbits(1) << 31 | rng.randint(110, 113) << 23 | rng.getrandbits(23)]
        return [rand_f32(rng)]
    if op in ("v_cvt_f16_u16", "v_cvt_f16_i16"):
        return [pack(rand_int16(rng), rand_int16(rng))]
    if op == "v_ldexp_f16":
        es = [rng.choice((rng.randint(-30, 30), rng.randint(-30, 30), rng.choice((-64, -65, 64, 65, -200, 300))))
              for _ in range(2)]
        return [pack(rand_half(rng), rand_half(rng)), pack(*es)]
    if op == "v_fract_f16":
        return [pack(rand_fract_half(rng), rand_fract_half(rng))]
    raise HarnessError(f"no operands for {op}")


def _src(rng, r, mods_ok):
    if not mods_ok or rng.random() < 0.6:
        return f"v{r}"
    return rng.choice((f"-v{r}", f"|v{r}|", f"-|v{r}|"))


def _bitlist(rng, n, p=0.5):
    return [int(rng.random() < p) for _ in range(n)]


def op_lines(rng, op, form=None, craft=True):
    """The lines for one op: its reseeded operands (when ``craft``), then the op."""
    forms, n = FAMILY[op]
    form = form or rng.choice(forms)
    dst = rng.choice(DSTS)
    regs = list(OPERAND_REGS[:n]) if craft else [rng.choice(VGPRS) for _ in range(n)]
    mods = []
    e64 = form in ("e64", "vop3", "vop3p")
    if op in ("v_fma_mixlo_f16", "v_fma_mixhi_f16"):
        hi = _bitlist(rng, 3, 0.4)
        sel = [s & h for s, h in zip(_bitlist(rng, 3), hi)]    # op_sel only on f16 operands
        mods += [f"op_sel:[{','.join(map(str, sel))}]", f"op_sel_hi:[{','.join(map(str, hi))}]"]
    elif form == "vop3p":
        for name, p in (("op_sel", 0.5), ("op_sel_hi", 0.5), ("neg_lo", 0.3), ("neg_hi", 0.3)):
            if rng.random() < 0.5:
                mods.append(f"{name}:[{','.join(map(str, _bitlist(rng, n, p if name != 'op_sel_hi' else 0.7)))}]")
    elif e64 and op not in ("v_fmac_f16", "v_cvt_f16_f32") and rng.random() < 0.5:
        nb = {"v_fma_f16": 4, "v_cvt_f16_u16": 2, "v_cvt_f16_i16": 2, "v_fract_f16": 2}.get(op, 3)
        mods.append(f"op_sel:[{','.join(map(str, _bitlist(rng, nb)))}]")
    if op == "v_cvt_f16_f32" and e64 and rng.random() < 0.4:
        mods.append(rng.choice(("mul:2", "mul:4", "div:2")))
    if e64 and op != "v_pk_fmac_f16" and rng.random() < 0.2:
        mods.append("clamp")
    pre, k = [], None
    words = operand_words(rng, op, n, mods) if craft else None
    if op in ("v_fmamk_f16", "v_fmaak_f16"):    # K is the literal's low half; the words are x, y, z of x * y + z
        if craft:
            x, y, z = words
            k, words = (y, [x, z]) if op == "v_fmamk_f16" else (z, [x, y])
        else:
            k = rand_half(rng)
        k = f"0x{k & M16:04x}"
    if craft:
        if op in ("v_fmac_f16", "v_pk_fmac_f16"):
            pre.append(f"v_mov_b32 v{dst}, 0x{words[2]:08x}")
        pre += [f"v_mov_b32 v{r}, 0x{w:08x}" for r, w in zip(regs, words)]
    abs_ok = e64 and form != "vop3p" and op not in ("v_cvt_f16_u16", "v_cvt_f16_i16") or op.startswith("v_fma_mix")
    srcs = [_src(rng, r, abs_ok and not (op == "v_ldexp_f16" and i == 1)) for i, r in enumerate(regs)]
    if k:
        srcs = [srcs[0], k, srcs[1]] if op == "v_fmamk_f16" else [srcs[0], srcs[1], k]
    name = op + ("_" + form if form in ("e32", "e64") else "")
    tail = (" " + " ".join(mods)) if mods else ""
    return pre + [f"{name} v{dst}, {', '.join(srcs)}{tail}"]


def seeds(rng):
    out = []
    for r in VGPRS:
        k = rng.random()
        w = rand_f32(rng) if k < 0.3 else pack(rand_int16(rng), rand_int16(rng)) if k < 0.4 else \
            pack(rand_half(rng), rand_half(rng))
        out.append(f"v_mov_b32 v{r}, 0x{w:08x}")
    return out


def program(rng, n_ops, ops=None):
    """A random program: MODE writes before about one op in three. -> (lines, ops used)."""
    pool = list(ops or FAMILY)
    lines, used = seeds(rng), []
    for _ in range(n_ops):
        if rng.random() < 0.35:
            lines.append(rng.choice(MODE_WRITES))
        op = rng.choice(pool)
        lines += op_lines(rng, op, craft=rng.random() < 0.75)
        used.append(op)
    return lines, used


def sweep_cases(ops=None):
    return [(op, form, mw) for op in (ops or FAMILY) for form in FAMILY[op][0] for mw in MODE_WRITES]


def sweep_program(case, rng):
    op, form, mw = case
    lines = seeds(rng) + [mw]
    for _ in range(GRID_PER_PROGRAM):
        lines += op_lines(rng, op, form)
    return lines


# ---- the invariance stage ----

def invariant_line(rng, op):
    dst = rng.choice(DSTS)
    r = [rng.choice(VGPRS) for _ in range(3)]
    if op in INV_VOP3P:
        sel = f" op_sel:[{rng.getrandbits(1)},{rng.getrandbits(1)}]" if rng.random() < 0.5 else ""
        return f"{op} v{dst}, v{r[0]}, v{r[1]}{sel}"
    if op in INV_VOP3:
        if op == "v_fma_mix_f32":
            hi = _bitlist(rng, 3, 0.4)
            sel = [s & h for s, h in zip(_bitlist(rng, 3), hi)]
            return (f"{op} v{dst}, v{r[0]}, v{r[1]}, v{r[2]} op_sel:[{','.join(map(str, sel))}] "
                    f"op_sel_hi:[{','.join(map(str, hi))}]")
        return f"{op} v{dst}, v{r[0]}, v{r[1]}, v{r[2]}"
    if op in INV_VOP2:
        return f"{op}_e32 v{dst}, v{r[0]}, v{r[1]}"
    if op in ("v_log_f16", "v_sqrt_f16", "v_rsq_f16"):     # GLSL leaves a negative operand undefined
        return f"{op}_e64 v{dst}, |v{r[0]}| op_sel:[{rng.getrandbits(1)},{rng.getrandbits(1)}]"
    if op in INV_UNARY_OPSEL and rng.random() < 0.4:
        return f"{op}_e64 v{dst}, {_src(rng, r[0], True)} op_sel:[{rng.getrandbits(1)},{rng.getrandbits(1)}]"
    return f"{op}_e32 v{dst}, v{r[0]}"


def invariance_program(rng, n_ops):
    """-> (lines with MODE writes, the same lines without them)."""
    lines = seeds(rng)
    first = True
    for _ in range(n_ops):
        if first or rng.random() < 0.4:
            lines.append(rng.choice(MODE_WRITES[1::2]) if first else rng.choice(MODE_WRITES))
            first = False
        if rng.random() < 0.5:
            r = rng.choice(VGPRS)
            w = pack(rand_half(rng, 0.08), rand_half(rng, 0.08)) if rng.random() < 0.7 else rand_f32(rng)
            lines.append(f"v_mov_b32 v{r}, 0x{w:08x}")
        lines.append(invariant_line(rng, rng.choice(INVARIANT)))
    plain = [ln for ln in lines if ln not in MODE_WRITES]
    return lines, plain


# ---- running the translator ----

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


def check(lines, name="ps_mode_case"):
    """None when both oracles agree, else the first difference."""
    with tempfile.TemporaryDirectory(prefix="mode-diff-") as tmp:
        want, nans = reference(lines)
        got = translate_listing(lines, name, tmp)
    return compare(want, nans, got)


def _translate(lines, name, tmpdir):
    """-> (words, None), or (None, why). A value SPIR-V leaves undefined (Log2 of 0, ...) raises NotJudged."""
    try:
        return translate_listing(lines, name, tmpdir), None
    except UnsupportedConstruct as e:
        if "(undefined" in str(e):
            raise NotJudged("undefined in SPIR-V: " + re.sub(r" of .*| \(undefined.*", "", str(e))) from None
        return None, f"UnsupportedConstruct: {e}"
    except rdna2spirv.Unsupported as e:
        return None, f"{type(e).__name__}: {e}"
    except (IndexError, KeyError, ValueError) as e:
        return None, f"translator error {type(e).__name__}: {e}"


def run_one(lines, name, tmpdir, events=None):
    """(status, detail) with status in {"match", "mismatch", "skip", "unjudged"}."""
    try:
        want, nans = reference(lines, events)
    except NotJudged as e:
        return ("unjudged", str(e))
    try:
        got, err = _translate(lines, name, tmpdir)
    except NotJudged as e:
        return ("unjudged", str(e))
    if err:
        return ("skip", err)
    diff = compare(want, nans, got)
    return ("mismatch", diff) if diff else ("match", "")


def run_invariant(pair, name, tmpdir):
    with_mode, plain = pair
    try:
        a, err = _translate(with_mode, name, tmpdir)
        b, err2 = (None, None) if err else _translate(plain, name + "p", tmpdir)
    except NotJudged as e:
        return ("unjudged", str(e))
    if err or err2:
        return ("skip", err or err2)
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return ("mismatch", f"v{EXPORT_REGS[i]}: 0x{x:08x} with the MODE writes, 0x{y:08x} without")
    return ("match", "")


# ---- the suites ----

def _new_counts():
    return {"pass": 0, "mismatch": 0, "skip": Counter(), "unjudged": Counter(), "ops": Counter(), "events": {}}


def _tally(counts, status, detail):
    if status == "match":
        counts["pass"] += 1
    elif status in ("skip", "unjudged"):
        counts[status][detail] += 1
    else:
        counts["mismatch"] += 1


def _run(counts, items, tag, invariance=False):
    failure = None
    with tempfile.TemporaryDirectory(prefix=f"mode-diff-{tag}-") as tmp:
        for i, (label, lines, used) in enumerate(items):
            counts["ops"].update(used)
            name = f"ps_mode_{tag}{i:05d}"
            status, detail = (run_invariant(lines, name, tmp) if invariance else
                              run_one(lines, name, tmp, counts["events"]))
            _tally(counts, status, detail)
            if status == "mismatch" and failure is None:
                failure = (label, lines[0] if invariance else lines, detail)
    return failure


# Each stage yields its (label, lines, ops) items one at a time, so a run holds one program's text, not the whole list.
def sweep_items(seed, ops=None):
    for case in sweep_cases(ops):
        tag = ":".join(case)
        yield f"sweep {tag} (seed {seed})", sweep_program(case, random.Random(f"{seed}:{tag}")), [case[0]]


def suite_items(programs, seed, max_ops=6, ops=None):
    rng = random.Random(f"suite:{seed}")
    for i in range(programs):
        lines, used = program(rng, rng.randint(1, max_ops), ops)
        yield f"program {i} (seed {seed})", lines, used


def invariance_items(programs, seed, max_ops=6):
    rng = random.Random(f"invariance:{seed}")
    for i in range(programs):
        pair = invariance_program(rng, rng.randint(1, max_ops))
        used = [base_op(split_line(ln)[0]) for ln in pair[1] if not ln.startswith("v_mov_b32 ")]
        yield f"invariance program {i} (seed {seed})", pair, used


def run_sweep(seed, ops=None):
    counts = _new_counts()
    return counts, _run(counts, sweep_items(seed, ops), "sw")


def run_suite(programs, seed, max_ops=6, ops=None):
    if type(programs) is not int or programs < 0:
        raise ValueError("programs must be a nonnegative integer")
    if type(max_ops) is not int or max_ops < 1:
        raise ValueError("max_ops must be at least 1")
    counts = _new_counts()
    return counts, _run(counts, suite_items(programs, seed, max_ops, ops), "rn")


def run_invariance(programs, seed, max_ops=6):
    if type(programs) is not int or programs < 0:
        raise ValueError("programs must be a nonnegative integer")
    if type(max_ops) is not int or max_ops < 1:
        raise ValueError("max_ops must be at least 1")
    counts = _new_counts()
    return counts, _run(counts, invariance_items(programs, seed, max_ops), "iv", invariance=True)


REJECTED = (
    ("directed rounding toward +inf", SETREG.format(1)),
    ("directed rounding toward -inf", SETREG.format(2)),
    ("s_round_mode with directed f16 rounding", "s_round_mode 0x4"),
    ("s_round_mode with f32 rounding", "s_round_mode 0x3"),
    ("a MODE write from an SGPR", "s_setreg_b32 hwreg(HW_REG_MODE, 2, 2), s0"),
    ("a write to other MODE bits", "s_setreg_imm32_b32 hwreg(HW_REG_MODE, 0, 4), 0xc"),
    ("s_denorm_mode", "s_denorm_mode 0xf"),
)

# RX 6800 words (tests/test_mode_round.py, SHADER-35): (setup, op, word without a MODE write / with RTZ, mask)
RX6800 = (
    (("v_mov_b32 v1, 0x4000", "v_mov_b32 v2, 0x0001"), "v_add_f16_e32 v3, v1, v2", 0x4000, 0x4000),
    (("v_mov_b32 v1, 0x7bff", "v_mov_b32 v2, 0x4c00"), "v_add_f16_e32 v3, v1, v2", 0x7C00, 0x7BFF),
    (("v_mov_b32 v1, 0xbc00", "v_mov_b32 v2, 0x1001"), "v_sub_f16_e32 v3, v1, v2", 0xBC01, 0xBC00),
    (("v_mov_b32 v1, 0x3f800000", "v_mov_b32 v2, 0x40000000", "v_mov_b32 v4, 0xb0800000"),
     "v_fma_mixlo_f16 v3, v1, v2, v4", 0x4000, 0x3FFF),
)


def measured_cases():
    """(name, setup, op, want nearest, want toward zero, mask) for the f16 words measured on RDNA2: the
    tools/mode_round_probe.py cases (gfx1036; its two v_ldexp_f16 cases are not measured yet) and the RX 6800 rows."""
    import mode_round_probe as probe
    out = []
    for name, setup, op, is_64, rne, rtz in probe.CASES:
        if not is_64:
            out.append((name, setup, op, rne, rtz, M32 if rne > M16 else M16))
    for i, (setup, op, rne, rtz) in enumerate(RX6800):
        out.append((f"rx6800_{i}", setup, op, rne, rtz, M16))
    return out


def reference_word(setup, op, mode_line, reg_=3):
    mc = Machine()
    for line in tuple(setup) + ((mode_line,) if mode_line else ()) + (op,):
        step(line, mc)
    return mc.v[reg_]


def self_test():
    ok = True

    def report(good, what):
        nonlocal ok
        ok &= good
        print(("ok   " if good else "FAIL ") + what)

    cases = measured_cases()
    bad = [name for name, setup, op, rne, rtz, mask in cases
           if (reference_word(setup, op, MODE_WRITES[0]) & mask, reference_word(setup, op, MODE_WRITES[1]) & mask)
           != (rne, rtz)]
    report(not bad, f"the reference gives the measured words of {len(cases)} cases in both modes {bad or ''}")
    report(reference_word(cases[0][1], cases[0][2], "s_round_mode 0xc") & M16 == cases[0][4],
           "s_round_mode 0xc selects toward zero")
    report(round16(("num", 0, Fraction(65520)), False) == 0x7C00 and round16(("num", 0, Fraction(65535)), True)
           == 0x7BFF and round16(("num", 1, Fraction(3, 1 << 25)), True) == 0x8001,
           "round16: 65520 -> inf nearest, 65535 -> 65504 toward zero, -1.5 * 2^-24 -> 0x8001 toward zero")
    mc = Machine()
    for line in ("v_mov_b32 v1, 0x9401", SETREG.format(3), "v_fract_f16_e32 v2, v1", SETREG.format(0),
                 "v_fract_f16_e32 v3, v1"):
        step(line, mc)
    report((mc.v[2] & M16, mc.v[3] & M16) == (0x3BFD, 0x3BFE),
           "fract(-(2^-10 + 2^-20)) is 0x3bfd toward zero and 0x3bfe to nearest even")
    for what, line in REJECTED:
        try:
            with tempfile.TemporaryDirectory(prefix="mode-diff-st-") as tmp:
                translate_listing(seeds(random.Random(1)) + [line], "ps_mode_rej", tmp)
            report(False, f"{what}: the translator accepted it")
        except (rdna2spirv.Unsupported, UnsupportedConstruct):
            report(True, f"{what}: the translator rejected it")
    a = program(random.Random("st"), 5)[0]
    b = program(random.Random("st"), 5)[0]
    report(a == b, "generation is deterministic for a seed")
    counts, failure = run_suite(8, 1)
    report(failure is None and not counts["skip"],
           f"a short seeded run matches ({counts['pass']} programs, skips {dict(counts['skip'])})")
    counts, failure = run_invariance(4, 1)
    report(failure is None and not counts["skip"], f"a short invariance run matches ({counts['pass']} programs)")
    return ok


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--programs", type=int, default=300, help="random programs after the sweep")
    ap.add_argument("--invariance", type=int, default=100, help="mode-invariance programs")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--max-ops", type=int, default=6, help="ops per random program")
    ap.add_argument("--ops", default=None, help="comma-separated opcodes for the sweep and the random programs")
    ap.add_argument("--no-sweep", action="store_true", help="skip the sweep of every form under every MODE write")
    ap.add_argument("--list-ops", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)
    if args.list_ops:
        for op in FAMILY:
            print(f"{op:18s} {OP_FORMS[op]}")
        print("invariance stage: " + " ".join(INVARIANT))
        return 0
    if args.self_test:
        return 0 if self_test() else 1
    if args.programs < 0:
        ap.error("--programs must be nonnegative")
    if args.invariance < 0:
        ap.error("--invariance must be nonnegative")
    if args.max_ops < 1:
        ap.error("--max-ops must be at least 1")
    if args.no_sweep and args.programs == 0 and args.invariance == 0:
        ap.error("nothing to run: pass --programs >= 1, --invariance >= 1, or omit --no-sweep")
    ops = None
    if args.ops is not None:
        tokens = [o.strip() for o in args.ops.split(",")]
        if not tokens or any(not o for o in tokens):
            ap.error("--ops must be a comma-separated list of opcodes")
        ops = list(dict.fromkeys(tokens))
        bad = [o for o in ops if o not in FAMILY]
        if bad:
            print(f"unknown op(s): {', '.join(bad)} (see --list-ops)", file=sys.stderr)
            return 2
    stages = []
    if not args.no_sweep:
        stages.append(("sweep", lambda: run_sweep(args.seed, ops)))
    if args.programs > 0:
        stages.append(("random", lambda: run_suite(args.programs, args.seed, args.max_ops, ops)))
    if args.invariance > 0:
        stages.append(("invariance", lambda: run_invariance(args.invariance, args.seed, args.max_ops)))
    total = {"pass": 0, "mismatch": 0, "skip": 0, "unjudged": Counter(), "events": {}}
    first = None
    try:
        for name, fn in stages:
            counts, failure = fn()
            n = counts["pass"] + counts["mismatch"] + sum(counts["skip"].values()) + sum(counts["unjudged"].values())
            print(f"{name}: {counts['pass']}/{n} programs match, {counts['mismatch']} mismatch; "
                  f"skips: {dict(counts['skip'])}; not judged: {sum(counts['unjudged'].values())}")
            total["pass"] += counts["pass"]
            total["mismatch"] += counts["mismatch"]
            total["skip"] += sum(counts["skip"].values())
            total["unjudged"].update(counts["unjudged"])
            for op, ev in counts["events"].items():
                total["events"].setdefault(op, Counter()).update(ev)
            if failure and first is None:
                first = failure
    except HarnessError as e:
        print(f"harness error: {e}", file=sys.stderr)
        return 2
    if total["events"]:
        print("rounded results per op (nearest / toward zero / the modes differ / overflow / denormal / tie): " +
              ", ".join(f"{op} {ev['nearest']}/{ev['toward zero']}/{ev['modes differ']}/{ev['overflow']}/"
                        f"{ev['denormal']}/{ev['tie']}" for op, ev in sorted(total["events"].items())))
    if total["unjudged"]:
        print("not judged: " + ", ".join(f"{why} {n}" for why, n in total["unjudged"].most_common()))
    judged = total["pass"] + total["mismatch"] + total["skip"]
    print(f"MODE.FP_ROUND: {total['pass']}/{judged} judged programs match, {total['mismatch']} mismatch")
    if first:
        _report(first)
        return 1
    if total["skip"]:
        print(f"SKIPPED {total['skip']} program(s): every generated form should translate (reasons above)")
        return 1
    if judged == 0:
        print("no judged program ran", file=sys.stderr)
        return 2
    return 0


def _report(failure):
    label, lines, detail = failure
    print(f"\nFIRST MISMATCH: {label}: {detail}")
    for line in lines:
        print("  " + line)


if __name__ == "__main__":
    try:  # a redirected stdout is strict ANSI code page under -I; write '?' for what it cannot encode
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError, OSError):
        pass
    sys.exit(main())
