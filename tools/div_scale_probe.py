r"""Measure v_div_scale_f32 and v_div_scale_f64 (D and the per-lane flag) on RDNA2 hardware. Standard library only.

The translator refuses both ops: the public descriptions of them disagree at the edges, and nothing has been measured
on RDNA2 (docs/shader_validation.md, "v_div_scale on the hardware"). This probe runs hand-written kernels on a GFX10 GPU
through tools/hw_oracle and records, for every case, the destination D and the flag the op writes to its scalar mask
destination. Next to each measured row it prints what three candidate readings predict, so a run shows at once which
reading (if any) the hardware follows. The candidates are references written here from public sources; none of them
is known to be right:

* ``isa_newer``: the ISA pseudocode as worded in the RDNA3, RDNA4 and CDNA3 documents. Its f32 form classifies
  ``1.0 / S1`` as a binary64 value (``1.0 / 64'F(S1.f32) == DENORM.f64``), which no f32 S1 satisfies, so the two
  "reciprocal is denormal" branches never fire for f32.
* ``isa_recip_width``: the same text with ``1.0 / S1`` classified in the operand's own width. It differs from
  ``isa_newer`` for f32 only. (The public emulators of the older instruction sets classify with "not normal" instead
  of "== DENORM", which also differs for a zero, infinite or NaN reciprocal and for a quotient that underflows to
  zero. No candidate models that; the matrix has rows that tell it apart, e.g. ``1 / inf`` and, for f32,
  ``2^-126 / 2^30``.)
* ``rdna3_emu``: the rule tinygrad's RDNA3 emulator runs after its hand-written fixes, read from
  ``test/mockgpu/amd/emu.py`` (``_fix_div_scale``) in the tinygrad project. Its hardware tests
  (``test/amd/hw/test_vop3.py``) pin it at a few points only.

"D = S0 where no branch assigns it" is an assumption, applied identically in all three candidates: for ordinary
operands, and inside every taken branch that assigns nothing (the gap branch when S0 != S1, the quotient branch when
S0 != S2). It is what LLVM's divide lowering (``SIISelLowering.cpp``, ``LowerFDIV32``/``LowerFDIV64``) relies on, and
it is not measured. Every ``v_div_scale`` of the plain and modifier rows is preceded by a write of a sentinel to D
(``SENTINEL``; no operand of the matrix and no candidate prediction equals it), so a run shows whether D is written at
all: a row that reads the sentinel back is reported as ``d_unwritten``. One public emulator initialises D
differently, as a note and not as a candidate: yoshou's amdgpu-sim (``gcn_processor.rs``) starts from
``D = S0 * S2 / S1``; gem5's f64 model assigns D only in the branches that scale. Where a candidate gives NaN, any NaN
matches, since none of them fixes the payload. The candidates ignore MODE (they read the operand bits as given), so
a mode that flushes denormal inputs shows up as rows no candidate matches.

Cases, per width (f32 thresholds first, f64 in brackets), for numerator n and denominator d:

* 11 special pairs: zero, negative zero and both-zero; inf in either source and in both; quiet and signalling NaN in
  either source;
* 6 denormal pairs: the smallest and largest denormal as d and as n, denormal over denormal, and the smallest
  denormal under a mid-range n (2^-60 [2^-284], biased exponent field 67 [739]: a gap below 96 [768], so the
  denormal-S1 branch is reached and the gap branch is not);
* 6 exponent-gap pairs, biased exponent field of n minus that of d = 95, 96, 97 [767, 768, 769], once with powers
  of two and once with mantissas 1.5 / 1.75;
* 8 reciprocal pairs: d = the value below 2^126 [2^1022], 2^126 [2^1022] itself, the value above it, and
  2^127 [2^1023], each with n = 1.0 (the quotient falls to or below the smallest normal) and n = 2^64 [2^736]
  (a normal quotient);
* 5 quotient pairs around the smallest normal 2^-126 [2^-1022]: d = 2 with n = the value below 2^-125 [2^-1021],
  2^-125 [2^-1021] and the value above it; 2^-126 / 1.0 [2^-1022 / 1.0]; and 2^-126 / 2^30 [2^-1022 / 2^30];
* 6 tiny-numerator pairs, biased exponent field of n = 22, 23, 24 [52, 53, 54] with d = 1.0 and d = 0.75;
* 7 ordinary pairs: 1/4, 2/4, 3/7, -5.5/1.25, 1e10/3e-5, -1.0 / the value above 2^126 [2^1022], 1.0 / -2^127
  [-2^1023].

That is 49 pairs. Each runs as the denominator call ``(S0, S1, S2) = (d, d, n)`` and as the numerator call
``(n, d, n)``, the two calls of LLVM's lowering, with S0 the same register as S1 or S2. There are also 10 cases with
S0 = S1 = S2: 1.0, 3.0, a tiny normal (biased exponent 22 [52]), the smallest normal, the largest denormal, +0,
2^127 [2^1023], the largest finite value, +inf and a quiet NaN. That makes 108 rows per width. Each row runs twice,
once with the mask written to VCC and once to an SGPR (s10, or s[10:11] in wave64). The register is preset to all
ones, so both a cleared bit and the bits of inactive lanes are visible.

Modifier rows (VCC only): 5 numerator calls (3/7, gap 96 [768], tiny numerator, 1.0 / 2^127 [2^1023], and 1.0 / 0)
with ``neg`` on S0, S1 and S2, and with ``clamp``, ``mul:2`` and ``div:2``, which makes 30 rows per width. The neg
rows are compared with the candidates on the negated operands. The clamp and omod rows are only recorded: the text
says nothing about them. VOP3B has no ``abs``.

Divide sequence: each of the 49 pairs also runs through LLVM's whole divide (``v_div_scale`` twice, ``v_rcp``, the
FMAs, ``v_div_fmas``, ``v_div_fixup``) without the s_denorm_mode switch LLVM wraps around the f32 form, so the f32
sequence is comparable with LLVM's lowering only in the MODE that keeps f32 denormals (``i0_d3_f32keep``); the
summary labels the other f32 tallies. The final quotient is compared with the correctly rounded IEEE quotient, and
with that quotient flushed to zero when it is denormal.

All of it runs in wave32 and wave64 under five MODEs (the f64_omod_probe set, game MODE first), one lane per wave.

Usage:
    python -I tools/div_scale_probe.py [--widths f32,f64] [--waves 32,64] [--modes ...] [--json out.json] [--table]
    python -I tools/div_scale_probe.py --self-test

Exit 0 when the run completed and wave32 and wave64, and the VCC and SGPR destinations, agree on every row,
whichever candidate matches; 1 when they do not; 2 on a harness or CLI error; 77 when the hardware oracle is
unavailable or the GPU is not GFX10. This tool has not been run on hardware yet. Nothing here reads game files.
"""
import argparse
import json
import os
import struct
import sys
from fractions import Fraction

HERE = os.path.dirname(os.path.abspath(__file__))
sys.dont_write_bytecode = True
if HERE not in sys.path:
    sys.path.insert(0, HERE)

# The MODEs of tools/f64_omod_probe.py, the game's first.
MODES = {
    "game_i0_d3": {"round16_64": 0, "denorm16_64": 3, "denorm32": 0, "dx10_clamp": 1, "ieee": 0},
    "i0_d0": {"round16_64": 0, "denorm16_64": 0, "denorm32": 0, "dx10_clamp": 1, "ieee": 0},
    "i1_d3": {"round16_64": 0, "denorm16_64": 3, "denorm32": 0, "dx10_clamp": 1, "ieee": 1},
    "i1_d0": {"round16_64": 0, "denorm16_64": 0, "denorm32": 0, "dx10_clamp": 1, "ieee": 1},
    "i0_d3_f32keep": {"round16_64": 0, "denorm16_64": 3, "denorm32": 3, "dx10_clamp": 1, "ieee": 0},
}
WIDTHS = ("f32", "f64")
# Written to D (both words for f64) before every v_div_scale of the plain and modifier kernels. It is a finite normal
# value with different words, so a D that is not written, or half written, reads back as itself; sentinel_conflicts()
# checks that no operand and no candidate prediction of the matrix equals it.
SENTINEL = {"f32": 0x5EADBEEF, "f64": 0x5EADBEEFCAFEF00D}
CANDIDATES = ("isa_newer", "isa_recip_width", "rdna3_emu")
NAN = "nan"   # a predicted NaN: any NaN matches


class Fmt:
    """Binary floating-point format: total bits, mantissa bits, exponent bias, and the div_scale constants."""

    def __init__(self, name, bits, mant, bias, gap, scale, tiny):
        self.name, self.bits, self.mant, self.bias = name, bits, mant, bias
        self.gap, self.scale, self.tiny = gap, scale, tiny   # exponent-gap limit, 2^scale, tiny-numerator limit
        self.emask = (1 << (bits - 1 - mant)) - 1
        self.emin = 1 - bias                                 # exponent of the smallest normal
        self.words = bits // 32


FMTS = {"f32": Fmt("f32", 32, 23, 127, 96, 64, 23), "f64": Fmt("f64", 64, 52, 1023, 768, 128, 53)}


# ---- exact binary floating point ----

def exp_field(f, b):
    return (b >> f.mant) & f.emask


def is_nan(f, b):
    return exp_field(f, b) == f.emask and b & ((1 << f.mant) - 1) != 0


def is_inf(f, b):
    return exp_field(f, b) == f.emask and b & ((1 << f.mant) - 1) == 0


def is_denorm(f, b):
    return exp_field(f, b) == 0 and b & ((1 << f.mant) - 1) != 0


def sign_of(f, b):
    return b >> (f.bits - 1)


def value(f, b):
    """The exact value of a finite encoding as a Fraction (signed; -0 reads as 0)."""
    e, m = exp_field(f, b), b & ((1 << f.mant) - 1)
    mag = Fraction(m, 1 << f.mant) * Fraction(2) ** f.emin if e == 0 else \
        Fraction((1 << f.mant) | m, 1 << f.mant) * Fraction(2) ** (e - f.bias)
    return -mag if sign_of(f, b) else mag


def round_to(f, x, neg_zero=False):
    """x (a Fraction) rounded to nearest even in format f, with denormals and overflow to infinity."""
    sign = 1 if x < 0 or (x == 0 and neg_zero) else 0
    a = abs(x)
    top = sign << (f.bits - 1)
    if a == 0:
        return top
    e = a.numerator.bit_length() - a.denominator.bit_length()
    while Fraction(2) ** e > a:
        e -= 1
    while Fraction(2) ** (e + 1) <= a:
        e += 1
    e = max(e, f.emin)
    scaled = a / Fraction(2) ** (e - f.mant)          # integer part has mant+1 bits for a normal
    q, r = divmod(scaled.numerator, scaled.denominator)
    if 2 * r > scaled.denominator or (2 * r == scaled.denominator and q & 1):
        q += 1
    if q == 1 << (f.mant + 1):
        q, e = q >> 1, e + 1
    if e > f.bias:
        return top | (f.emask << f.mant)
    if q < 1 << f.mant:
        return top | q                                 # denormal (or rounded up into the smallest normal)
    return top | ((e + f.bias) << f.mant) | (q & ((1 << f.mant) - 1))


def pow2(f, k, sign=0):
    return round_to(f, Fraction(-1 if sign else 1) * Fraction(2) ** k)


def from_float(f, x):
    if f.bits == 64:
        return struct.unpack("<Q", struct.pack("<d", x))[0]
    return struct.unpack("<I", struct.pack("<f", x))[0]


def next_up(f, b):
    return b + 1          # for positive finite encodings


def next_down(f, b):
    return b - 1


def neg(f, b):
    return b ^ (1 << (f.bits - 1))


def ldexp(f, b, n):
    if is_nan(f, b):
        return NAN
    if is_inf(f, b) or value(f, b) == 0:
        return b
    return round_to(f, value(f, b) * Fraction(2) ** n)


def feq(f, a, b):
    """IEEE equality: NaN equals nothing, +0 equals -0."""
    if is_nan(f, a) or is_nan(f, b):
        return False
    if is_inf(f, a) or is_inf(f, b):
        return a == b
    return value(f, a) == value(f, b)


def divide(f, n, d, out=None):
    """n / d correctly rounded into format `out` (default f): bits or NAN."""
    out = out or f
    if is_nan(f, n) or is_nan(f, d) or (is_inf(f, n) and is_inf(f, d)):
        return NAN
    sgn = sign_of(f, n) ^ sign_of(f, d)
    if is_inf(f, n):
        return (sgn << (out.bits - 1)) | (out.emask << out.mant)
    if is_inf(f, d):
        return sgn << (out.bits - 1)
    vn, vd = value(f, n), value(f, d)
    if vd == 0:
        return NAN if vn == 0 else (sgn << (out.bits - 1)) | (out.emask << out.mant)
    return round_to(out, vn / vd, neg_zero=bool(sgn))


def is_zero_val(f, b):
    return not is_nan(f, b) and not is_inf(f, b) and value(f, b) == 0


# ---- the candidate readings ----

def _isa_text(f, s0, s1, s2, recip_fmt):
    """The ISA pseudocode, with `1.0 / S1` classified in recip_fmt and D = S0 where no branch assigns it."""
    vcc, d, k = 0, s0, f.scale

    def denorm_div(fmt_out, n, dd):
        q = divide(f, n, dd, fmt_out)
        return q != NAN and is_denorm(fmt_out, q)

    one = round_to(f, Fraction(1))
    if is_zero_val(f, s2) or is_zero_val(f, s1):
        d = NAN
    elif exp_field(f, s2) - exp_field(f, s1) >= f.gap:
        vcc = 1
        if feq(f, s0, s1):
            d = ldexp(f, s0, k)
    elif is_denorm(f, s1):
        d = ldexp(f, s0, k)
    elif denorm_div(recip_fmt, one, s1) and denorm_div(f, s2, s1):
        vcc = 1
        if feq(f, s0, s1):
            d = ldexp(f, s0, k)
    elif denorm_div(recip_fmt, one, s1):
        d = ldexp(f, s0, -k)
    elif denorm_div(f, s2, s1):
        vcc = 1
        if feq(f, s0, s2):
            d = ldexp(f, s0, k)
    elif exp_field(f, s2) <= f.tiny:
        d = ldexp(f, s0, k)
    return d, vcc


def isa_newer(f, s0, s1, s2):
    """RDNA3/RDNA4/CDNA3 wording: for f32 the reciprocal is classified as a binary64 value."""
    return _isa_text(f, s0, s1, s2, FMTS["f64"])


def isa_recip_width(f, s0, s1, s2):
    """The same text with the reciprocal classified in the operand's width (equal to isa_newer for f64)."""
    return _isa_text(f, s0, s1, s2, f)


def rdna3_emu(f, s0, s1, s2):
    """tinygrad's RDNA3 emulator rule after its fixes (test/mockgpu/amd/emu.py, _fix_div_scale), as read there.

    A zero operand sets the flag as well as giving NaN. The S1-denormal and both reciprocal branches are gone. The
    quotient-denormal test compares exponent fields (field(S2) - field(S1) < 1 - bias) and sets the flag without
    scaling D. The tiny-numerator branch sets the flag for f32. For f64 the fix's search text ("<= 52") does not
    occur in the pseudocode ("<= 53"), so that branch keeps the text's 53 and leaves the flag clear. Last, a
    denormal S1 makes D a NaN whatever branch ran."""
    vcc, d, k = 0, s0, f.scale
    gap = exp_field(f, s2) - exp_field(f, s1)
    if is_zero_val(f, s2) or is_zero_val(f, s1):
        vcc, d = 1, NAN
    elif gap >= f.gap:
        vcc = 1
        if feq(f, s0, s1):
            d = ldexp(f, s0, k)
    elif gap < f.emin:
        vcc = 1
    elif exp_field(f, s2) <= f.tiny:
        d = ldexp(f, s0, k)
        if f.name == "f32":
            vcc = 1
    if is_denorm(f, s1):
        d = NAN
    return d, vcc


READINGS = {"isa_newer": isa_newer, "isa_recip_width": isa_recip_width, "rdna3_emu": rdna3_emu}


# ---- the case matrix ----

def pairs(f):
    """[(name, n, d)] for one width, in the order of the module docstring."""
    one, two = pow2(f, 0), pow2(f, 1)
    big_exp = -f.emin                      # 126 [1022]: 1 / 2^big_exp is the smallest normal
    qnan = (f.emask << f.mant) | (1 << (f.mant - 1))
    snan = (f.emask << f.mant) | 1
    inf = f.emask << f.mant
    zero, nzero = 0, 1 << (f.bits - 1)
    dmin, dmax = 1, (1 << f.mant) - 1
    out = [("1 / +0", one, zero), ("1 / -0", one, nzero), ("+0 / 1", zero, one), ("+0 / +0", zero, zero),
           ("inf / 1", inf, one), ("1 / inf", one, inf), ("inf / inf", inf, inf),
           ("qnan / 1", qnan, one), ("1 / qnan", one, qnan), ("snan / 1", snan, one), ("1 / snan", one, snan),
           ("1 / min denorm", one, dmin), ("1 / max denorm", one, dmax),
           ("min denorm / 1", dmin, one), ("max denorm / 1", dmax, one), ("max denorm / min denorm", dmax, dmin),
           (f"2^{f.gap - 29 - f.bias} / min denorm", pow2(f, f.gap - 29 - f.bias), dmin)]
    for g in (f.gap - 1, f.gap, f.gap + 1):
        out.append((f"gap {g}: 2^{g} / 1", pow2(f, g), one))
    for g in (f.gap - 1, f.gap, f.gap + 1):
        out.append((f"gap {g}: 1.5*2^{g} / 1.75", round_to(f, Fraction(3, 2) * Fraction(2) ** g),
                    round_to(f, Fraction(7, 4))))
    edge = pow2(f, big_exp)
    dens = [(f"below 2^{big_exp}", next_down(f, edge)), (f"2^{big_exp}", edge),
            (f"above 2^{big_exp}", next_up(f, edge)), (f"2^{big_exp + 1}", pow2(f, big_exp + 1))]
    n_big = f.gap - 32
    for label, dd in dens:
        out.append((f"1 / {label}", one, dd))
        out.append((f"2^{n_big} / {label}", pow2(f, n_big), dd))
    lo = pow2(f, f.emin + 1)
    out += [(f"below 2^{f.emin + 1} / 2", next_down(f, lo), two), (f"2^{f.emin + 1} / 2", lo, two),
            (f"above 2^{f.emin + 1} / 2", next_up(f, lo), two), (f"2^{f.emin} / 1", pow2(f, f.emin), one),
            (f"2^{f.emin} / 2^30", pow2(f, f.emin), pow2(f, 30))]
    for e in (f.tiny - 1, f.tiny, f.tiny + 1):
        n = pow2(f, e - f.bias)
        out.append((f"field {e} / 1", n, one))
        out.append((f"field {e} / 0.75", n, round_to(f, Fraction(3, 4))))
    out += [("1 / 4", one, pow2(f, 2)), ("2 / 4", two, pow2(f, 2)), ("3 / 7", round_to(f, Fraction(3)),
                                                                     round_to(f, Fraction(7))),
            ("-5.5 / 1.25", round_to(f, Fraction(-11, 2)), round_to(f, Fraction(5, 4))),
            ("1e10 / 3e-5", from_float(f, 1e10), from_float(f, 3e-5)),
            (f"-1 / above 2^{big_exp}", neg(f, one), next_up(f, edge)),
            (f"1 / -2^{big_exp + 1}", one, pow2(f, big_exp + 1, sign=1))]
    return out


def same_values(f):
    """[(name, x)] for the S0 = S1 = S2 rows."""
    return [("1", pow2(f, 0)), ("3", round_to(f, Fraction(3))), (f"field {f.tiny - 1}", pow2(f, f.tiny - 1 - f.bias)),
            ("min normal", pow2(f, f.emin)), ("max denorm", (1 << f.mant) - 1), ("+0", 0),
            (f"2^{-f.emin + 1}", pow2(f, -f.emin + 1)),
            ("max finite", ((f.emask - 1) << f.mant) | ((1 << f.mant) - 1)),
            ("+inf", f.emask << f.mant), ("qnan", (f.emask << f.mant) | (1 << (f.mant - 1)))]


def scale_rows(f):
    """[(case, call, s0, s1, s2, operand registers)] for the plain rows: 48 pairs x 2 calls + 10 same rows."""
    rows = []
    for name, n, d in pairs(f):
        rows.append((name, "den", d, d, n))
        rows.append((name, "num", n, d, n))
    for name, x in same_values(f):
        rows.append((name, "same", x, x, x))
    return rows


MOD_FORMS = ("-s0", "-s1", "-s2", "clamp", "mul:2", "div:2")


def mod_rows(f):
    """[(case, form, n, d)]: numerator calls of five pairs under each modifier form."""
    one = pow2(f, 0)
    base = [("3 / 7", round_to(f, Fraction(3)), round_to(f, Fraction(7))),
            (f"gap {f.gap}", pow2(f, f.gap), one),
            (f"field {f.tiny}", pow2(f, f.tiny - f.bias), one),
            (f"1 / 2^{-f.emin + 1}", one, pow2(f, -f.emin + 1)),
            ("1 / +0", one, 0)]
    return [(name, form, n, d) for name, n, d in base for form in MOD_FORMS]


def predict(f, s0, s1, s2):
    return {c: READINGS[c](f, s0, s1, s2) for c in CANDIDATES}


def sentinel_conflicts(f):
    """Rows (plain and modifier, neg forms included) where the D sentinel equals an operand or a predicted D."""
    sentinel, bad = SENTINEL[f.name], []
    rows = [(case + " " + call, (s0, s1, s2)) for case, call, s0, s1, s2 in scale_rows(f)]
    for case, form, n, d in mod_rows(f):
        ops = [n, d, n]
        if form.startswith("-s"):
            ops[int(form[2])] = neg(f, ops[int(form[2])])
        rows.append((f"{case} {form}", tuple(ops)))
    if is_nan(f, sentinel) or is_inf(f, sentinel) or is_denorm(f, sentinel):
        bad.append("the sentinel is not a finite normal value")
    for name, ops in rows:
        if sentinel in ops or any(pred[0] == sentinel for pred in predict(f, *ops).values()):
            bad.append(name)
    return bad


# ---- kernels ----

def _reg(f, base):
    return f"v{base}" if f.words == 1 else f"v[{base}:{base + 1}]"


def _seed(f, base, bits):
    lines = [f"v_mov_b32_e32 v{base}, {bits & 0xFFFFFFFF:#010x}"]
    if f.words == 2:
        lines.append(f"v_mov_b32_e32 v{base + 1}, {bits >> 32:#010x}")
    return lines


def mask_regs(wave, dst):
    """(sdst operand, the 32-bit registers to read back) for a VCC or SGPR mask destination."""
    if dst == "vcc":
        return ("vcc_lo", ["vcc_lo"]) if wave == 32 else ("vcc", ["vcc_lo", "vcc_hi"])
    return ("s10", ["s10"]) if wave == 32 else ("s[10:11]", ["s10", "s11"])


class _Writer:
    """Collects lines and the stores of a raw kernel (v20 holds the byte offset, v21 a copied mask word)."""

    def __init__(self):
        self.lines, self.words = [], 0

    def store(self, vreg):
        self.lines += [f"v_mov_b32_e32 v20, {self.words * 4}", f"global_store_dword v20, {vreg}, s[2:3]"]
        self.words += 1

    def store_sreg(self, sreg):
        self.lines.append(f"v_mov_b32_e32 v21, {sreg}")
        self.store("v21")

    def finish(self):
        return self.lines + ["s_waitcnt vmcnt(0)", "s_endpgm"]


def _div_scale_line(f, sdst, s0, s1, s2, tail=""):
    return f"v_div_scale_{f.name} {_reg(f, 8)}, {sdst}, {s0}, {s1}, {s2}{tail}"


def scale_kernel_lines(f, wave):
    """The plain rows, once with VCC and once with an SGPR destination. Registers: d in 2.., n in 4.., D in 8.."""
    w = _Writer()
    den, num = _reg(f, 2), _reg(f, 4)
    for dst in ("vcc", "sgpr"):
        sdst, reads = mask_regs(wave, dst)
        preset = f"s_mov_b32 {sdst}, -1" if wave == 32 else f"s_mov_b64 {sdst}, -1"
        for _case, call, s0, s1, s2 in scale_rows(f):
            # den: (d, d, n); num: (n, d, n); same: x in both registers, read as (d, d, d)
            if call == "same":
                w.lines += _seed(f, 2, s0)
                ops = (den, den, den)
            else:
                w.lines += _seed(f, 2, s1) + _seed(f, 4, s2)
                ops = (den, den, num) if call == "den" else (num, den, num)
            w.lines += _seed(f, 8, SENTINEL[f.name]) + [preset, _div_scale_line(f, sdst, *ops)]
            for k in range(f.words):
                w.store(f"v{8 + k}")
            for r in reads:
                w.store_sreg(r)
    return w.finish(), w.words


def mod_kernel_lines(f, wave):
    w = _Writer()
    den, num = _reg(f, 2), _reg(f, 4)
    sdst, reads = mask_regs(wave, "vcc")
    preset = f"s_mov_b32 {sdst}, -1" if wave == 32 else f"s_mov_b64 {sdst}, -1"
    for _case, form, n, d in mod_rows(f):
        ops = [num, den, num]
        tail = ""
        if form.startswith("-s"):
            i = int(form[2])
            ops[i] = "-" + ops[i]
        else:
            tail = " " + form
        w.lines += _seed(f, 2, d) + _seed(f, 4, n) + _seed(f, 8, SENTINEL[f.name]) + \
            [preset, _div_scale_line(f, sdst, *ops, tail)]
        for k in range(f.words):
            w.store(f"v{8 + k}")
        for r in reads:
            w.store_sreg(r)
    return w.finish(), w.words


def sequence_lines(f, wave):
    """LLVM's divide (LowerFDIV32 / LowerFDIV64) on d in 2.., n in 4..; the quotient lands in 8.."""
    junk = "s12" if wave == 32 else "s[12:13]"
    vcc = "vcc_lo" if wave == 32 else "vcc"
    if f.name == "f32":
        return [f"v_div_scale_f32 v6, {junk}, v2, v2, v4",          # denominator, scaled
                f"v_div_scale_f32 v7, {vcc}, v4, v2, v4",           # numerator, scaled; its flag feeds div_fmas
                "v_rcp_f32 v10, v6",
                "v_fma_f32 v11, -v6, v10, 1.0",
                "v_fmac_f32 v10, v11, v10",
                "v_mul_f32 v11, v7, v10",
                "v_fma_f32 v12, -v6, v11, v7",
                "v_fmac_f32 v11, v12, v10",
                "v_fma_f32 v12, -v6, v11, v7",
                "v_div_fmas_f32 v12, v12, v10, v11",
                "v_div_fixup_f32 v8, v12, v2, v4"]
    return [f"v_div_scale_f64 v[6:7], {junk}, v[2:3], v[2:3], v[4:5]",
            "v_rcp_f64 v[10:11], v[6:7]",
            "v_fma_f64 v[12:13], -v[6:7], v[10:11], 1.0",
            "v_fma_f64 v[10:11], v[10:11], v[12:13], v[10:11]",
            "v_fma_f64 v[12:13], -v[6:7], v[10:11], 1.0",
            f"v_div_scale_f64 v[14:15], {vcc}, v[4:5], v[2:3], v[4:5]",
            "v_fma_f64 v[10:11], v[10:11], v[12:13], v[10:11]",
            "v_mul_f64 v[12:13], v[14:15], v[10:11]",
            "v_fma_f64 v[16:17], -v[6:7], v[12:13], v[14:15]",
            "v_div_fmas_f64 v[16:17], v[16:17], v[10:11], v[12:13]",
            "v_div_fixup_f64 v[8:9], v[16:17], v[2:3], v[4:5]"]


def seq_kernel_lines(f, wave):
    w = _Writer()
    for _name, n, d in pairs(f):
        w.lines += _seed(f, 2, d) + _seed(f, 4, n) + sequence_lines(f, wave)
        for k in range(f.words):
            w.store(f"v{8 + k}")
    return w.finish(), w.words


SECTIONS = {"scale": scale_kernel_lines, "mods": mod_kernel_lines, "seq": seq_kernel_lines}


def build_kernels(asm, modes, waves, widths):
    kernels, meta = [], []
    for wave in waves:
        for mode_name in modes:
            for width in widths:
                for section, make in SECTIONS.items():
                    lines, words = make(FMTS[width], wave)
                    kernels.append(asm.Kernel(lines, wave=wave, threads=1, words=words, raw=True,
                                              mode=dict(MODES[mode_name])))
                    meta.append((wave, mode_name, width, section))
    return kernels, meta


# ---- evaluation ----

def _bits_from(words, f):
    return words[0] if f.words == 1 else words[0] | (words[1] << 32)


def _match(f, pred, got):
    if pred == NAN:
        return is_nan(f, got)
    return pred == got


def _hex(f, b):
    return NAN if b == NAN else f"{b:0{f.bits // 4}x}"


def evaluate(meta, raw_results):
    """Turn the words of each kernel into rows; summarise candidates, wave and destination agreement."""
    measured = {}
    for (wave, mode_name, width, section), result in zip(meta, raw_results):
        if isinstance(result, str):
            raise RuntimeError(f"{width} {section} kernel, wave{wave}, {mode_name}: {result}")
        words = [x & 0xFFFFFFFF for x in (result["words"] if isinstance(result, dict) else result)]
        measured[(wave, mode_name, width, section)] = words

    rows, seq_rows = [], []
    summary = {"candidates": {}, "wave_diffs": 0, "dst_diffs": 0, "first_wave_diffs": [], "first_dst_diffs": [],
               "d_unwritten": 0, "first_d_unwritten": [],
               "first_mismatches": {c: [] for c in CANDIDATES}, "sequence": {}}
    for (wave, mode_name, width, section) in meta:
        f = FMTS[width]
        words = measured[(wave, mode_name, width, section)]
        mask_words = 1 if wave == 32 else 2
        step = f.words + mask_words
        pos = 0
        if section == "scale":
            for dst in ("vcc", "sgpr"):
                for case, call, s0, s1, s2 in scale_rows(f):
                    chunk = words[pos:pos + step]
                    pos += step
                    rows.append({"wave": wave, "mode": mode_name, "width": width, "dst": dst, "case": case,
                                 "call": call, "form": "", "s0": s0, "s1": s1, "s2": s2,
                                 "d": _bits_from(chunk, f), "mask": chunk[f.words:],
                                 "pred": predict(f, s0, s1, s2)})
        elif section == "mods":
            for case, form, n, d in mod_rows(f):
                chunk = words[pos:pos + step]
                pos += step
                s = [n, d, n]
                if form.startswith("-s"):
                    s[int(form[2])] = neg(f, s[int(form[2])])
                pred = predict(f, *s) if form.startswith("-s") else None
                rows.append({"wave": wave, "mode": mode_name, "width": width, "dst": "vcc", "case": case,
                             "call": "num", "form": form, "s0": s[0], "s1": s[1], "s2": s[2],
                             "d": _bits_from(chunk, f), "mask": chunk[f.words:], "pred": pred})
        else:
            for name, n, d in pairs(f):
                got = _bits_from(words[pos:pos + f.words], f)
                pos += f.words
                cr = divide(f, n, d)
                ftz = cr if cr == NAN or not is_denorm(f, cr) else cr & (1 << (f.bits - 1))
                verdict = ("nan" if cr == NAN and is_nan(f, got) else "cr" if got == cr else
                           "cr_ftz" if got == ftz else "other")
                seq_rows.append({"wave": wave, "mode": mode_name, "width": width, "case": name, "n": n, "d": d,
                                 "q": got, "cr": cr, "verdict": verdict})

    for r in rows:
        r["unwritten"] = r["d"] == SENTINEL[r["width"]]
        if r["unwritten"]:
            summary["d_unwritten"] += 1
            if len(summary["first_d_unwritten"]) < 5:
                summary["first_d_unwritten"].append(
                    f"w{r['wave']} {r['mode']} {r['width']} {r['dst']} {r['case']} ({r['call']}"
                    f"{' ' + r['form'] if r['form'] else ''})")

    # Candidates: the primary rows are the smaller wave's, VCC destination (the SGPR and other-wave rows must agree).
    key = lambda r: (r["mode"], r["width"], r["dst"], r["case"], r["call"], r["form"])  # noqa: E731
    by_key = {}
    for r in rows:
        by_key.setdefault(key(r), {})[r["wave"]] = r
    for r in rows:
        if r["wave"] != min(by_key[key(r)]) or r["dst"] != "vcc" or r["pred"] is None:
            continue
        f = FMTS[r["width"]]
        vcc_bit = r["mask"][0] & 1
        tally = summary["candidates"].setdefault(f"{r['mode']} {r['width']}", {c: [0, 0] for c in CANDIDATES})
        for c in CANDIDATES:
            pd, pv = r["pred"][c]
            ok = _match(f, pd, r["d"]) and pv == vcc_bit
            tally[c][1] += 1
            tally[c][0] += ok
            if not ok and len(summary["first_mismatches"][c]) < 8:
                summary["first_mismatches"][c].append(
                    f"{r['mode']} {r['width']} {r['case']} ({r['call']}{' ' + r['form'] if r['form'] else ''}): "
                    f"D {_hex(f, r['d'])} flag {vcc_bit}, predicted D {_hex(f, pd)} flag {pv}")
    for k, per_wave in by_key.items():
        f = FMTS[k[1]]
        present = sorted(per_wave)
        base = per_wave[present[0]]
        for wave in present[1:]:
            other = per_wave[wave]
            same_flag = base["mask"][0] & 1 == other["mask"][0] & 1
            both_nan = is_nan(f, base["d"]) and is_nan(f, other["d"])
            if not same_flag or (base["d"] != other["d"] and not both_nan):
                summary["wave_diffs"] += 1
                if len(summary["first_wave_diffs"]) < 5:
                    summary["first_wave_diffs"].append(" ".join(map(str, k)) + f" wave{wave}")
        if k[2] == "vcc":
            for wave, r in per_wave.items():
                twin = by_key.get((k[0], k[1], "sgpr", k[3], k[4], k[5]), {}).get(wave)
                if twin and (twin["d"], twin["mask"]) != (r["d"], r["mask"]):
                    summary["dst_diffs"] += 1
                    if len(summary["first_dst_diffs"]) < 5:
                        summary["first_dst_diffs"].append(" ".join(map(str, k[:2] + k[3:])) + f" wave{wave}")
    first_wave = {}
    for s in seq_rows:
        k = (s["mode"], s["width"])
        first_wave[k] = min(first_wave.get(k, s["wave"]), s["wave"])
    for s in seq_rows:
        if s["wave"] != first_wave[(s["mode"], s["width"])]:
            continue
        # LLVM wraps the f32 FMAs in an s_denorm_mode switch that these kernels do not have.
        comparable = s["width"] != "f32" or MODES[s["mode"]]["denorm32"] == 3
        tally = summary["sequence"].setdefault(f"{s['mode']} {s['width']}", {
            "cr": 0, "cr_ftz": 0, "nan": 0, "other": 0, "first_other": [], "llvm_comparable": comparable})
        tally[s["verdict"]] += 1
        if s["verdict"] == "other" and len(tally["first_other"]) < 5:
            f = FMTS[s["width"]]
            tally["first_other"].append(f"{s['case']}: got {_hex(f, s['q'])}, correctly rounded {_hex(f, s['cr'])}")
    return rows, seq_rows, summary


def row_json(r):
    f = FMTS[r["width"]]
    out = {k: r[k] for k in ("wave", "mode", "width", "dst", "case", "call", "form")}
    out.update({"s0": _hex(f, r["s0"]), "s1": _hex(f, r["s1"]), "s2": _hex(f, r["s2"]), "d": _hex(f, r["d"]),
                "d_unwritten": r["unwritten"], "mask_lo": f"{r['mask'][0]:08x}",
                "mask_hi": f"{r['mask'][1]:08x}" if len(r["mask"]) > 1 else None, "flag": r["mask"][0] & 1})
    if r["pred"] is not None:
        out["candidates"] = {c: {"d": _hex(f, r["pred"][c][0]), "flag": r["pred"][c][1],
                                 "match": _match(f, r["pred"][c][0], r["d"]) and r["pred"][c][1] == (r["mask"][0] & 1)}
                             for c in CANDIDATES}
    return out


def seq_json(s):
    f = FMTS[s["width"]]
    return {"wave": s["wave"], "mode": s["mode"], "width": s["width"], "case": s["case"], "n": _hex(f, s["n"]),
            "d": _hex(f, s["d"]), "q": _hex(f, s["q"]), "correctly_rounded": _hex(f, s["cr"]), "verdict": s["verdict"]}


TABLE_LEGEND = ("# columns: wave mode width mask-destination call modifier case D=<result, hex without 0x, high word "
                "first, or <unwritten> when the D sentinel was read back> flag=<bit 0 of the mask, lane 0> then, "
                "per candidate, ok (predicts D and flag) or x. The --json file also has the whole mask words "
                "(mask_lo, then mask_hi in wave64).")


def table_lines(rows):
    out = [TABLE_LEGEND]
    for r in rows:
        f = FMTS[r["width"]]
        cands = "" if r["pred"] is None else "  " + "  ".join(
            f"{c}={'ok' if _match(f, r['pred'][c][0], r['d']) and r['pred'][c][1] == r['mask'][0] & 1 else 'x'}"
            for c in CANDIDATES)
        out.append(f"w{r['wave']} {r['mode']} {r['width']} {r['dst']:4} {r['call']:4} {r['form'] or '-':6} "
                   f"{r['case']:28} D={'<unwritten>' if r['unwritten'] else _hex(f, r['d'])} "
                   f"flag={r['mask'][0] & 1}{cands}")
    return out


# ---- self-test ----

def self_test():
    f32, f64 = FMTS["f32"], FMTS["f64"]
    one, z = pow2(f32, 0), 0
    # A zero operand: the text leaves the flag clear, the RDNA3-derived rule sets it.
    assert isa_newer(f32, one, z, one) == (NAN, 0)
    assert rdna3_emu(f32, one, z, one) == (NAN, 1)
    # 2^-126 / 1.0, numerator call: the text scales with the flag clear; the RDNA3-derived rule sets the flag.
    n = pow2(f32, -126)
    assert isa_newer(f32, n, one, n) == (pow2(f32, -62), 0)
    assert rdna3_emu(f32, n, one, n) == (pow2(f32, -62), 1)
    # S0 = S2 = 1.0, S1 = 2^127: the newer wording scales S0, the f32-width reading and the RDNA3 rule do not.
    d = pow2(f32, 127)
    assert isa_newer(f32, one, d, one) == (pow2(f32, 64), 1)
    assert isa_recip_width(f32, one, d, one) == (one, 1)
    assert rdna3_emu(f32, one, d, one) == (one, 1)
    # Its denominator call under the f32-width reading scales 2^127 by 2^64: +inf.
    assert isa_recip_width(f32, d, d, one) == (f32.emask << f32.mant, 1)
    # Ordinary operands pass S0 through with the flag clear in every reading, both widths.
    for f in (f32, f64):
        for c in CANDIDATES:
            assert READINGS[c](f, pow2(f, 1), pow2(f, 2), pow2(f, 1)) == (pow2(f, 1), 0), (f.name, c)
    for f in (f32, f64):
        assert not sentinel_conflicts(f), (f.name, sentinel_conflicts(f))
    rows = sum(len(scale_rows(f)) for f in (f32, f64))
    print(f"self-test ok ({len(CANDIDATES)} candidates, {rows} rows over 2 widths, {len(MODES)} modes)")
    return 0


def write_report(path, report):
    try:
        folder = os.path.dirname(path)
        if folder:
            os.makedirs(folder, exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(report, fh, ensure_ascii=False, indent=1)
        return True
    except OSError as exc:
        print(f"cannot write {path}: {exc}")
        return False


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--modes", default=",".join(MODES))
    ap.add_argument("--waves", default="32,64")
    ap.add_argument("--widths", default=",".join(WIDTHS))
    ap.add_argument("--json", help="write the measurement report (every row) here")
    ap.add_argument("--table", action="store_true", help="print every row, not only the summary")
    ap.add_argument("--hw-arch")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()

    modes = [m.strip() for m in args.modes.split(",") if m.strip()]
    wave_tokens = [w.strip() for w in args.waves.split(",") if w.strip()]
    widths = [w.strip() for w in args.widths.split(",") if w.strip()]
    bad = [m for m in modes if m not in MODES] + [w for w in wave_tokens if w not in ("32", "64")] + \
        [w for w in widths if w not in WIDTHS]
    if bad or not modes or not wave_tokens or not widths:
        ap.error("unknown or empty --modes/--waves/--widths entry: " + ", ".join(bad or ["--modes/--waves/--widths"]))
    waves = [int(w) for w in wave_tokens]
    if args.json and os.path.isdir(args.json):
        ap.error(f"--json {args.json} is a folder, not a file")

    from hw_oracle import asm, runner

    try:
        oracle = runner.open_oracle(arch=args.hw_arch)
    except runner.Unavailable as exc:
        print(f"hardware oracle unavailable: {exc}")
        return 77
    try:
        if asm.family(oracle.arch) != "gfx10":
            print(f"hardware oracle unavailable: {oracle.arch} is not GFX10")
            return 77
        print(f"hardware oracle: {oracle.device} ({oracle.arch})")
        kernels, meta = build_kernels(asm, modes, waves, widths)
        rows, seq_rows, summary = evaluate(meta, oracle.run(kernels))
    except (RuntimeError, KeyError, TypeError, IndexError) as exc:
        print(f"harness error: {exc}")
        return 2
    finally:
        oracle.close()

    if args.json and not write_report(args.json, {
            "device": str(oracle.device), "arch": oracle.arch, "modes": modes, "waves": waves, "widths": widths,
            "candidates": list(CANDIDATES), "summary": summary,
            "rows": [row_json(r) for r in rows], "sequence": [seq_json(s) for s in seq_rows]}):
        return 2

    if args.table:
        for line in table_lines(rows):
            print(line)
    for group, tally in summary["candidates"].items():
        print(f"{group}: " + ", ".join(f"{c} {ok}/{n}" for c, (ok, n) in tally.items()) +
              "  (rows whose D and flag the candidate predicts)")
    for group, t in summary["sequence"].items():
        label = "" if t["llvm_comparable"] else \
            "  [f32 sequence without LLVM's s_denorm_mode switch: not comparable with LLVM's lowering in this MODE]"
        print(f"{group} divide sequence: cr={t['cr']}, cr_ftz={t['cr_ftz']}, nan={t['nan']}, other={t['other']}{label}")
        for line in t["first_other"]:
            print(f"  OTHER {group} {line}")
    for c in CANDIDATES:
        for line in summary["first_mismatches"][c]:
            print(f"  {c} MISMATCH {line}")
    print(f"d_unwritten={summary['d_unwritten']} (rows that read the D sentinel back: D was not written)")
    for line in summary["first_d_unwritten"]:
        print(f"  D UNWRITTEN {line}")
    print(f"wave_diffs={summary['wave_diffs']}, dst_diffs={summary['dst_diffs']}")
    for line in summary["first_wave_diffs"]:
        print(f"  WAVE DIFF {line}")
    for line in summary["first_dst_diffs"]:
        print(f"  DST DIFF {line}")
    if any(not t["llvm_comparable"] for t in summary["sequence"].values()):
        print("note: the f32 divide sequence has no s_denorm_mode switch; its verdicts are comparable with LLVM's "
              "lowering only in i0_d3_f32keep.")
    if not args.json:
        print("note: this summary is not the measurement. Re-run with --json <file> and post that file.")
    return 1 if (summary["wave_diffs"] or summary["dst_diffs"]) else 0


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError, OSError):
        pass
    sys.exit(main())
