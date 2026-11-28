r"""Whole-wave differential fuzzing of the M0-relative VGPR moves. Standard library only.

No other fuzzer generates v_movrels_b32, v_movreld_b32, v_movrelsd_b32, v_movrelsd_2_b32, v_swaprel_b32 or
v_swap_b32. This one generates seeded straight-line programs of them, with M0 and EXEC rewritten between them and
plain VGPR moves and adds around them, and runs each program through

  * the reference below, a per-lane model of the RDNA2 ISA guide's opcode descriptions, and
  * tools/rdna2spirv.py, executing the SPIR-V with tests/semantic/spirv_wave_interp.py (one subgroup = one wave,
    every lane at once),

then compares all 32 VGPRs of the window v0..v31 in every lane bit for bit (eight exports, mrt0..mrt7). The reference
shares no code with the translator or with the other fuzzers.

The model (VOP1 opcodes 66, 67, 68, 72, 101 and 104; docs/design.md "Relative VGPR moves" and "v_swaprel_b32"):
  * v_movrels_b32 D, S0      D = VGPR[S0 + M0]
  * v_movreld_b32 D, S0      VGPR[D + M0] = S0. S0 is any VOP1 source: a VGPR, an SGPR, M0, an inline constant or
                             a literal (LLVM's VOP_MOVRELD takes VSrc_b32). The other four relative forms take a VGPR.
  * v_movrelsd_b32 D, S0     VGPR[D + M0] = VGPR[S0 + M0]
  * v_movrelsd_2_b32 D, S0   VGPR[D + M0[25:16]] = VGPR[S0 + M0[9:0]]; the other M0 bits are ignored
  * v_swaprel_b32 D, S0      swap VGPR[D + M0[25:16]] and VGPR[S0 + M0[9:0]], both read before either is written
  * v_swap_b32 D, S0         swap D and S0
M0 is the whole register for the first three. An inactive lane keeps every VGPR.

What is generated:
  * M0 from `s_mov_b32 m0, K`, `s_mov_b32 m0, sN` and `s_add_u32 m0, m0, K`. For the two-field forms, random bits in
    M0[15:10] and M0[31:26], which must be ignored, and a source and destination that are sometimes one register;
  * EXEC from `s_mov_b32 exec_lo, K` (and `exec_hi` in wave64), K from edge masks and random words;
  * the e32 forms, and e64 for v_movrels_b32 and v_movreld_b32;
  * v_movreld_b32 sources: VGPRs, SGPRs s4..s15, M0, integer inline constants -16..64, float inline constants
    (0.15915494 too) and literals;
  * between them, v_mov_b32 / v_add_nc_u32 / v_xor_b32 on the window, so static and relative accesses mix.
Every effective index stays inside the window, and M0 is never negative: the hardware checks an index against the
wave's VGPR allocation, which the translator does not model (#2935 adds a bound). Not generated: exec_lo or vcc_lo
as v_movreld_b32's source (a lane mask read as data, #293), and the DPP and SDWA forms.

The prologue sets v0 to the lane number, seeds v1..v31 with a different value per lane (eight of them get a
v_writelane_b32 patch), the SGPRs and M0, all with EXEC all on. The epilogue turns EXEC back on and exports v0..v31.

Wave size: --wave 32 (default), 64 or both. A wave64 program is detected by the translator from v_mbcnt_hi; when it
decides otherwise, the program is a counted skip.

Usage:
    python -I tools/vrel_diff.py [--programs N] [--seed S] [--max-ops N] [--wave 32|64|both]
    python -I tools/vrel_diff.py --ops v_swaprel_b32,v_movrelsd_2_b32
    python -I tools/vrel_diff.py --list-ops
    python -I tools/vrel_diff.py --self-test

Exit 0 when every program matches, 1 on a mismatch or a skip (the failing program is printed after removing the
instructions that do not matter), 2 on a harness error.
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
sys.dont_write_bytecode = True
if HERE not in sys.path:
    sys.path.insert(0, HERE)
SEM_DIR = os.path.join(os.path.dirname(HERE), "tests", "semantic")
if SEM_DIR not in sys.path:
    sys.path.insert(0, SEM_DIR)

M32 = 0xFFFFFFFF
STUB_DXBC = b"DXBC" + bytes(24) + struct.pack("<I", 0) + bytes(4)

WINDOW = 32                       # v0..v31: every VGPR a program touches, all exported
SREGS = tuple(range(4, 16))       # data SGPRs
PATCH = 30                        # s30 carries the v_writelane_b32 patches of the prologue
OPS = ("v_movrels_b32", "v_movreld_b32", "v_movrelsd_b32", "v_movrelsd_2_b32", "v_swaprel_b32", "v_swap_b32")
WHOLE_M0 = ("v_movrels_b32", "v_movreld_b32", "v_movrelsd_b32")
TWO_FIELDS = ("v_movrelsd_2_b32", "v_swaprel_b32")
E64 = ("v_movrels_b32", "v_movreld_b32")
FORMS = {
    "v_movrels_b32": "D = VGPR[S0 + M0]; e32/e64",
    "v_movreld_b32": "VGPR[D + M0] = S0, S0 any VOP1 source; e32/e64",
    "v_movrelsd_b32": "VGPR[D + M0] = VGPR[S0 + M0]",
    "v_movrelsd_2_b32": "VGPR[D + M0[25:16]] = VGPR[S0 + M0[9:0]]",
    "v_swaprel_b32": "swap VGPR[D + M0[25:16]] and VGPR[S0 + M0[9:0]]",
    "v_swap_b32": "swap D and S0",
}
FLOATS = {"0.5": 0x3F000000, "-0.5": 0xBF000000, "1.0": 0x3F800000, "-1.0": 0xBF800000, "2.0": 0x40000000,
          "-2.0": 0xC0000000, "4.0": 0x40800000, "-4.0": 0xC0800000, "0.15915494": 0x3E22F983}
INTERESTING = (0, 1, 2, 31, 32, 0x7FFFFFFF, 0x80000000, M32, 0xFFFFFFFE, 0xFFFF, 0x10000, 0x7FC00001, 0x12345678,
               0xDEADBEEF, 0x55555555, 0xAAAAAAAA)
EXEC_EDGES = (M32, 0, 1, 0x80000000, 0x0000FFFF, 0xFFFF0000, 0x55555555, 0xFFFFFFFE)


class Skip(Exception):
    """The translator or the interpreter could not run the program: counted, never a pass."""


class RefError(Exception):
    """The program is outside what the reference models (an index outside the window, an unknown line)."""


def rand_value(rng):
    r = rng.random()
    if r < 0.4:
        return rng.choice(INTERESTING)
    if r < 0.55:
        return rng.randrange(0, 64)
    return rng.getrandbits(32)


def offsets(op, m0):
    """(destination offset, source offset) M0 gives op, or None when an index could leave the window."""
    if op == "v_swap_b32":
        return 0, 0
    if m0 is None:
        return None
    if op in WHOLE_M0:
        return (m0, m0) if m0 < WINDOW else None
    d, s = (m0 >> 16) & 0x3FF, m0 & 0x3FF
    return (d, s) if d < WINDOW and s < WINDOW else None


# ---- program generation -------------------------------------------------------------------------------------

class Gen:
    def __init__(self, rng, wave, ops=None):
        self.rng, self.wave, self.ops = rng, wave, tuple(ops or OPS)
        self.m0 = 0
        self.sgpr = {}

    def prologue(self):
        rng = self.rng
        lines = ["v_mbcnt_lo_u32_b32 v0, -1, 0"] + (["v_mbcnt_hi_u32_b32 v0, -1, v0"] if self.wave == 64 else [])
        for v in range(1, WINDOW):
            lines.append(f"v_mul_lo_u32 v{v}, v0, 0x{rng.getrandbits(32) | 1:x}")
            lines.append(f"v_xor_b32_e32 v{v}, 0x{rng.getrandbits(32):x}, v{v}")
        for v in rng.sample(range(1, WINDOW), 8):
            lines.append(f"s_mov_b32 s{PATCH}, 0x{rand_value(rng):x}")
            lines.append(f"v_writelane_b32 v{v}, s{PATCH}, {rng.randrange(self.wave)}")
        for s in SREGS:
            self.sgpr[s] = rand_value(rng)
            lines.append(f"s_mov_b32 s{s}, 0x{self.sgpr[s]:x}")
        lines.append("s_mov_b32 m0, 0")
        return lines

    def new_m0(self, op):
        """Lines that give M0 a value op can use."""
        rng = self.rng
        if op in TWO_FIELDS:
            k = (rng.randrange(WINDOW) << 16) | rng.randrange(WINDOW)
            if rng.random() < 0.6:                       # bits the two-field forms ignore
                k |= (rng.getrandbits(6) << 26) | (rng.getrandbits(6) << 10)
        else:
            k = rng.choice((0, 1, 2, 3)) if rng.random() < 0.5 else rng.randrange(WINDOW)
        old = self.m0
        self.m0 = k
        if op in WHOLE_M0 and old is not None and old <= k and rng.random() < 0.3:
            return [f"s_add_u32 m0, m0, {k - old}"]
        if rng.random() < 0.3:
            s = rng.choice(SREGS)
            self.sgpr[s] = k
            return [f"s_mov_b32 s{s}, 0x{k:x}", f"s_mov_b32 m0, s{s}"]
        return [f"s_mov_b32 m0, {k}" if k <= 64 else f"s_mov_b32 m0, 0x{k:x}"]

    def movreld_source(self):
        rng = self.rng
        r = rng.random()
        if r < 0.35:
            return f"v{rng.randrange(WINDOW)}", "vgpr"
        if r < 0.55:
            return f"s{rng.choice(SREGS)}", "sgpr"
        if r < 0.6:
            return "m0", "m0"
        if r < 0.75:
            return str(rng.randint(-16, 64)), "inline integer"
        if r < 0.85:
            return rng.choice(tuple(FLOATS)), "inline float"
        return f"0x{rng.getrandbits(32):x}", "literal"

    def rel(self, op):
        rng = self.rng
        lines = []
        if offsets(op, self.m0) is None or rng.random() < 0.35:
            lines += self.new_m0(op)
        do, so = offsets(op, self.m0)
        d = rng.randrange(WINDOW - do)
        s = rng.randrange(WINDOW - so)
        if op in TWO_FIELDS and rng.random() < 0.15 and 0 <= d + do - so < WINDOW:
            s = d + do - so                              # both fields name one register
        name = op + ("_e64" if op in E64 and rng.random() < 0.3 else "")
        src = self.movreld_source()[0] if op == "v_movreld_b32" else f"v{s}"
        return lines + [f"{name} v{d}, {src}"]

    def filler(self):
        rng = self.rng
        v = lambda: f"v{rng.randrange(WINDOW)}"         # noqa: E731
        op = rng.choice(("v_mov_b32", "v_add_nc_u32", "v_xor_b32"))
        if op == "v_mov_b32":
            src = v() if rng.random() < 0.7 else f"s{rng.choice(SREGS)}"
            return [f"v_mov_b32_e32 {v()}, {src}"]
        return [f"{op}_e32 {v()}, {v()}, {v()}"]

    def exec_write(self):
        rng = self.rng
        k = rng.choice(EXEC_EDGES) if rng.random() < 0.6 else rng.getrandbits(32)
        half = "exec_hi" if self.wave == 64 and rng.random() < 0.4 else "exec_lo"
        return [f"s_mov_b32 {half}, 0x{k:x}"]

    def op(self):
        r = self.rng.random()
        if r < 0.6:
            return self.rel(self.rng.choice(self.ops))
        if r < 0.8:
            return self.filler()
        if r < 0.92:
            return self.exec_write()
        return self.new_m0(self.rng.choice(self.ops))


def epilogue(wave):
    out = ["s_mov_b32 exec_lo, -1"] + (["s_mov_b32 exec_hi, -1"] if wave == 64 else [])
    return out + [f"exp mrt{i} v{4 * i}, v{4 * i + 1}, v{4 * i + 2}, v{4 * i + 3}" + (" done vm" if i == 7 else "")
                  for i in range(WINDOW // 4)]


class Program:
    """init + body + the epilogue; body is what shrinking removes from."""

    def __init__(self, init, body, wave):
        self.init, self.body, self.wave = list(init), list(body), wave

    def lines(self):
        return self.init + self.body + epilogue(self.wave)

    def with_body(self, body):
        return Program(self.init, body, self.wave)


def generate(rng, n_ops, ops=None, wave=32):
    gen = Gen(rng, wave, ops)
    init = gen.prologue()
    body = []
    for _ in range(n_ops):
        body += gen.op()
    return Program(init, body, wave)


# ---- the reference -------------------------------------------------------------------------------------------

class State:
    def __init__(self, wave):
        self.wave = wave
        self.v = [[0] * wave for _ in range(WINDOW)]
        self.s = {}
        self.m0 = None
        self.exec = (1 << wave) - 1

    def scalar(self, tok):
        if not tok:
            raise RefError("empty scalar operand")
        if tok == "m0":
            if self.m0 is None:
                raise RefError("M0 read before it is written")
            return self.m0
        if tok[0] == "s" and tok[1:].isdigit():
            if int(tok[1:]) not in self.s:
                raise RefError(f"{tok} read before it is written")
            return self.s[int(tok[1:])]
        if tok in FLOATS:
            return FLOATS[tok]
        try:
            return int(tok, 0) & M32
        except ValueError:
            raise RefError(f"source {tok}") from None

    def vreg(self, tok):
        if not tok or tok[0] != "v" or not tok[1:].isdigit():
            raise RefError(f"not a VGPR: {tok}")
        return self.index(int(tok[1:]))

    def index(self, i):
        if not 0 <= i < WINDOW:
            raise RefError(f"VGPR index {i} outside v0..v{WINDOW - 1}")
        return i

    def lanes(self, tok):
        """Per-lane values of a source."""
        if tok and tok[0] == "v" and tok[1:].isdigit():
            return list(self.v[self.vreg(tok)])
        return [self.scalar(tok)] * self.wave

    def write(self, reg, vals):
        for lane in range(self.wave):
            if self.exec >> lane & 1:
                self.v[reg][lane] = vals[lane] & M32

    def set_scalar(self, dst, val):
        val &= M32
        if dst == "m0":
            self.m0 = val
        elif dst == "exec_lo":
            self.exec = (self.exec & ~M32) | val if self.wave == 64 else val
        elif dst == "exec_hi" and self.wave == 64:
            self.exec = (self.exec & M32) | (val << 32)
        elif dst and dst[0] == "s" and dst[1:].isdigit():
            self.s[int(dst[1:])] = val
        else:
            raise RefError(f"scalar destination {dst}")


def step(st, line):
    op, _, rest = line.strip().partition(" ")
    args = [a.strip() for a in rest.split(",")] if rest.strip() else []
    base = op[:-4] if op.endswith(("_e32", "_e64")) else op
    if base == "s_mov_b32":
        if len(args) != 2:
            raise RefError(f"{op}: expected 2 operands, got {len(args)}")
        st.set_scalar(args[0], st.scalar(args[1]))
    elif base == "s_add_u32":
        if len(args) != 3:
            raise RefError(f"{op}: expected 3 operands, got {len(args)}")
        st.set_scalar(args[0], st.scalar(args[1]) + st.scalar(args[2]))
    elif base == "v_mbcnt_lo_u32_b32" and args[1:] == ["-1", "0"]:
        st.write(st.vreg(args[0]), [min(lane, 32) for lane in range(st.wave)])
    elif base == "v_mbcnt_hi_u32_b32" and len(args) == 3 and args[1] == "-1":
        add = st.lanes(args[2])
        st.write(st.vreg(args[0]), [max(lane - 32, 0) + add[lane] for lane in range(st.wave)])
    elif base in ("v_mov_b32", "v_mul_lo_u32", "v_xor_b32", "v_add_nc_u32"):
        want_argc = 2 if base == "v_mov_b32" else 3
        if len(args) != want_argc:
            raise RefError(f"{op}: expected {want_argc} operands, got {len(args)}")
        srcs = [st.lanes(a) for a in args[1:]]
        f = {"v_mov_b32": lambda a: a[0], "v_mul_lo_u32": lambda a: a[0] * a[1], "v_xor_b32": lambda a: a[0] ^ a[1],
             "v_add_nc_u32": lambda a: a[0] + a[1]}[base]
        st.write(st.vreg(args[0]), [f([s[lane] for s in srcs]) for lane in range(st.wave)])
    elif base == "v_writelane_b32":
        if len(args) != 3:
            raise RefError(f"{op}: expected 3 operands, got {len(args)}")
        try:
            lane = int(args[2], 0) % st.wave
        except ValueError:
            raise RefError(f"{op}: bad lane {args[2]!r}") from None
        st.v[st.vreg(args[0])][lane] = st.scalar(args[1])
    elif base in OPS:
        rel(st, base, args)
    else:
        raise RefError(f"line not modelled: {line}")


def rel(st, op, args):
    if len(args) != 2:
        raise RefError(f"{op}: expected 2 operands, got {len(args)}")
    d, s = args
    if op != "v_movreld_b32" and not (s and s[0] == "v" and s[1:].isdigit()):
        raise RefError(f"{op} takes a VGPR source")
    if op == "v_swap_b32":
        do = so = 0
    elif op in WHOLE_M0:
        do = so = st.scalar("m0")
    else:
        m0 = st.scalar("m0")
        do, so = (m0 >> 16) & 0x3FF, m0 & 0x3FF
    dst = st.index(st.vreg(d) + do)
    if op == "v_movreld_b32":
        st.write(dst, st.lanes(s))
    elif op == "v_movrels_b32":
        st.write(st.vreg(d), list(st.v[st.index(st.vreg(s) + so)]))
    elif op in ("v_movrelsd_b32", "v_movrelsd_2_b32"):
        st.write(dst, list(st.v[st.index(st.vreg(s) + so)]))
    else:                                                # the two swaps: both read before either is written
        src = st.index(st.vreg(s) + so)
        a, b = list(st.v[dst]), list(st.v[src])
        st.write(dst, b)
        st.write(src, a)


def reference(lines, wave):
    """Run the program; returns per lane the words of v0..v31."""
    st = State(wave)
    for line in lines:
        if not line.startswith("exp "):
            step(st, line)
    return [tuple(st.v[r][lane] for r in range(WINDOW)) for lane in range(wave)]


# ---- the translator side -----------------------------------------------------------------------------------

def translated(lines, wave, name="ps_vrel"):
    """Translate and run one subgroup; returns per lane the 32 exported words, or raises Skip."""
    import rdna2spirv
    from spirv_wave_interp import run_wave
    with tempfile.TemporaryDirectory(prefix="vrel-diff-") as tmp:
        dis = os.path.join(tmp, "disasm")
        os.mkdir(dis)
        path = os.path.join(dis, name + ".s")
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(f"; {name}  vrel_diff case\n")
            for line in lines:
                f.write("\t" + line + "\n")
            f.write("\ts_endpgm\n")
        with open(os.path.join(tmp, name + ".dxbc"), "wb") as f:
            f.write(STUB_DXBC)
        try:
            with warnings.catch_warnings():
                warnings.simplefilter("ignore", ResourceWarning)
                spv = rdna2spirv.translate(path)
                gen = rdna2spirv.translate.last_gen
                gc.collect()
        except rdna2spirv.Unsupported as e:
            raise Skip(f"translator unsupported: {e}") from None
    if bool(getattr(gen, "wave64", False)) != (wave == 64):
        raise Skip(f"translator compiled the wave{wave} program as wave{64 if wave == 32 else 32}")
    try:
        out = run_wave(spv, wave=wave)["outputs"]
    except Exception as e:  # noqa: BLE001 - an interpreter gap is a counted skip, never a pass
        raise Skip(f"wave interp: {type(e).__name__}: {str(e)[:80]}") from None
    res = []
    for lane in range(wave):
        got = out[lane] or {}
        words = [got.get(f"%out_mrt{i}") for i in range(WINDOW // 4)]
        res.append(None if None in words else tuple(w & M32 for ws in words for w in ws))
    return res


# ---- comparison, shrinking, the run --------------------------------------------------------------------------

def compare(want, got):
    """None when every lane's 32 words match, else the first difference."""
    if not isinstance(got, (list, tuple)) or len(got) != len(want):
        got_desc = len(got) if isinstance(got, (list, tuple)) else type(got).__name__
        return f"lane count mismatch: expected {len(want)}, got {got_desc}"
    for lane, (w, g) in enumerate(zip(want, got)):
        if g is None:
            return f"lane {lane}: the translated program exported nothing"
        if not isinstance(g, (list, tuple)) or len(g) != WINDOW:
            got_desc = len(g) if isinstance(g, (list, tuple)) else type(g).__name__
            return f"lane {lane}: expected {WINDOW} words, got {got_desc}"
        for r in range(WINDOW):
            if w[r] != g[r]:
                return f"lane {lane} v{r}: reference 0x{w[r]:08x}, translated 0x{g[r]:08x}"
    return None


def check(prog, run=None):
    """None on a match, else a description. Raises Skip (translator side) or RefError (reference side)."""
    lines = prog.lines()
    want = reference(lines, prog.wave)
    return compare(want, (run or translated)(lines, prog.wave))


def shrink(prog, check_fn=check):
    """Drop body lines, one at a time, while the program still mismatches."""
    def fails(p):
        try:
            return bool(check_fn(p))
        except (Skip, RefError):
            return False
    body = list(prog.body)
    i = len(body) - 1
    while i >= 0:
        trial = prog.with_body(body[:i] + body[i + 1:])
        if fails(trial):
            body = trial.body
        i -= 1
    return prog.with_body(body)


def _new_counts():
    return {"pass": 0, "mismatch": 0, "skip": Counter(), "ops": Counter(), "events": Counter()}


def events(prog):
    """What a program exercised, for the summary: per op, the e64 forms, v_movreld_b32 source kinds, ignored M0 bits
    under a two-field form, both fields naming one register, and a relative write under a partial EXEC."""
    ev = Counter()
    st = State(prog.wave)
    for line in prog.lines():
        if line.startswith("exp "):
            continue
        op, _, rest = line.partition(" ")
        base = op[:-4] if op.endswith(("_e32", "_e64")) else op
        if base in OPS:
            ev[base] += 1
            d, s = (a.strip() for a in rest.split(","))
            if op.endswith("_e64"):
                ev["e64 form"] += 1
            if base == "v_movreld_b32":
                kind = ("vgpr" if s[0] == "v" else "sgpr" if s[0] == "s" else "m0" if s == "m0"
                        else "inline float" if s in FLOATS else "literal" if s.startswith("0x") else "inline integer")
                ev[f"v_movreld_b32 {kind} source"] += 1
            if base in TWO_FIELDS:
                m0 = st.scalar("m0")
                if m0 & 0xFC00FC00:
                    ev["ignored M0 bits set"] += 1
                if st.vreg(d) + (m0 >> 16 & 0x3FF) == st.vreg(s) + (m0 & 0x3FF):
                    ev["one register on both sides"] += 1
            if st.exec != (1 << prog.wave) - 1:
                ev["partial EXEC"] += 1
        step(st, line)
    return ev


def suite_items(programs, seed, max_ops, ops, waves):
    """(label, program) of the random programs, made one at a time."""
    rng = random.Random(f"vrel_diff:{seed}")
    for i in range(programs):
        wave = waves[i % len(waves)]
        yield f"program {i} (seed {seed}, wave{wave})", generate(rng, rng.randint(1, max_ops), ops, wave)


def run_suite(programs, seed, max_ops=8, ops=None, waves=(32,), run=None):
    """Returns (counts, failure); failure is (label, program, shrunk program, detail) of the first mismatch."""
    counts, failure = _new_counts(), None
    for label, prog in suite_items(programs, seed, max_ops, ops, waves):
        counts["events"].update(events(prog))
        try:
            detail = check(prog, run)
        except Skip as e:
            counts["skip"][re.sub(r"\b\d+\b", "N", str(e))] += 1      # one line per kind, not per index
            continue
        if detail is None:
            counts["pass"] += 1
            continue
        counts["mismatch"] += 1
        if failure is None:
            small = shrink(prog, (lambda p: check(p, run)))
            try:
                small_detail = check(small, run) or detail
            except Skip:
                small_detail = detail
            failure = (label, prog, small, small_detail)
    return counts, failure


# ---- self-test ----------------------------------------------------------------------------------------------

def _init(wave=32):
    return [f"v_mov_b32_e32 v{r}, 0x{0xA000 + r:x}" for r in range(WINDOW)] + ["s_mov_b32 s4, 0x1234"]


# (body, register, value every lane holds there afterwards); v_r starts as 0xa000 + r.
CANARIES = (
    (["s_mov_b32 m0, 0x14000a", "v_movrelsd_2_b32 v5, v7"], 25, 0xA011),      # the ISA's example: v25 = v17
    (["s_mov_b32 m0, 0xfc14fc0a", "v_movrelsd_2_b32 v5, v7"], 25, 0xA011),    # M0[31:26] and M0[15:10] ignored
    (["s_mov_b32 m0, 3", "v_movrels_b32 v1, v20"], 1, 0xA017),
    (["s_mov_b32 m0, 2", "v_movreld_b32 v30, s4"], 0, 0xA000),                 # v32 is outside: see RefError
    (["s_mov_b32 m0, 2", "v_movreld_b32 v20, s4"], 22, 0x1234),
    (["s_mov_b32 m0, 1", "v_movreld_b32_e64 v20, 0.15915494"], 21, 0x3E22F983),
    (["s_mov_b32 m0, 4", "v_movrelsd_b32 v2, v10"], 6, 0xA00E),
    (["s_mov_b32 m0, 0x20003", "v_swaprel_b32 v1, v4"], 3, 0xA007),           # v3 <-> v7
    (["s_mov_b32 m0, 0x20003", "v_swaprel_b32 v1, v4"], 7, 0xA003),
    (["s_mov_b32 m0, 0x30001", "v_swaprel_b32 v1, v3"], 4, 0xA004),           # one register on both sides
    (["v_swap_b32 v9, v10"], 9, 0xA00A),
)


def self_test(verbose=True, run=None):
    ok = True

    def report(name, good, detail=""):
        nonlocal ok
        ok &= good
        if verbose:
            print(f"  {'ok  ' if good else 'FAIL'} {name}{': ' + detail if detail else ''}")

    for body, reg, want in CANARIES:
        prog = Program(_init(), body, 32)
        name = " ; ".join(body)
        if body == ["s_mov_b32 m0, 2", "v_movreld_b32 v30, s4"]:
            try:
                reference(prog.lines(), 32)
                report(name, False, "the reference took an index outside the window")
            except RefError:
                report(name + " (refused: v32)", True)
            continue
        got = reference(prog.lines(), 32)
        report(name + " (reference)", all(lane[reg] == want for lane in got), f"v{reg} = 0x{got[0][reg]:x}")
        try:
            detail = check(prog, run)
            report(name + " (translated)", detail is None, detail or "")
        except Skip as e:
            report(name + " (translated)", False, str(e))
    prog = Program(_init(), ["s_mov_b32 exec_lo, 0x5", "s_mov_b32 m0, 1", "v_movrelsd_b32 v2, v10"], 32)
    got = reference(prog.lines(), 32)
    report("an inactive lane keeps the register", got[0][3] == 0xA00B and got[1][3] == 0xA003 and got[2][3] == 0xA00B)
    prog = generate(random.Random(1), 6)
    bad = lambda lines, wave: [tuple(w ^ (1 if r == 17 else 0) for r, w in enumerate(lane))   # noqa: E731
                               for lane in reference(lines, wave)]
    report("a wrong translation is caught", check(prog, bad) is not None)
    return ok


# ---- command line -------------------------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--programs", type=int, default=300)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--max-ops", type=int, default=8)
    ap.add_argument("--wave", choices=("32", "64", "both"), default="32")
    ap.add_argument("--ops", help="comma-separated subset of --list-ops")
    ap.add_argument("--list-ops", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)
    if args.programs < 1 or args.max_ops < 1:
        ap.error("--programs and --max-ops must be at least 1")
    if args.list_ops:
        for op in OPS:
            print(f"{op:18} {FORMS[op]}")
        return 0
    if args.self_test:
        return 0 if self_test() else 1
    ops = None
    if args.ops is not None:
        tokens = [o.strip() for o in args.ops.split(",")]
        if not any(tokens) or any(not o for o in tokens):
            print("--ops must be a comma-separated list of opcodes (see --list-ops)", file=sys.stderr)
            return 2
        ops = list(dict.fromkeys(tokens))
        unknown = [o for o in ops if o not in OPS]
        if unknown:
            print(f"unknown op(s): {', '.join(unknown)} (see --list-ops)", file=sys.stderr)
            return 2
    waves = {"32": (32,), "64": (64,), "both": (32, 64)}[args.wave]
    try:
        counts, failure = run_suite(args.programs, args.seed, args.max_ops, ops, waves)
    except RefError as e:
        print(f"harness error: the reference refused a generated program: {e}", file=sys.stderr)
        return 2
    ev = counts["events"]
    print("relative moves: " + ", ".join(f"{op} {ev[op]}" for op in OPS if ev[op]))
    print("cases: " + ", ".join(f"{k} {n}" for k, n in sorted(ev.items()) if k not in OPS))
    if counts["skip"]:
        print("skipped: " + "; ".join(f"{r} ({n})" for r, n in counts["skip"].most_common()))
    if failure:
        label, prog, small, detail = failure
        print(f"first mismatch: {label}: {detail}")
        print("shortest failing body:")
        for line in small.body:
            print("    " + line)
    judged = counts["pass"] + counts["mismatch"]
    print(f"vrel family: {counts['pass']}/{judged} programs match, {counts['mismatch']} mismatch, "
          f"{sum(counts['skip'].values())} skipped (wave {args.wave})")
    return 1 if counts["mismatch"] or counts["skip"] else 0


if __name__ == "__main__":
    try:  # a redirected stdout is strict ANSI code page under -I; write '?' for what it cannot encode
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError, OSError):
        pass
    sys.exit(main())
