r"""Measure two DPP edge cases natively through `tools/hw_oracle` (TOOL-DPP-HW-PROBE).

Group `neg`: does DPP apply src0's `neg` / `abs` to the 0 that `bound_ctrl:1` gives a lane whose source is invalid
(out of its row, or off in EXEC with `fi:0`)? `v_add_f32_dpp` / `v_mul_f32_dpp` with `-src0`, `src0` and `|src0|`;
each lane is compared with two models: the modifiers come after the zero (the invalid lane reads -0), or before it
(+0, what `Gen.dpp_insn` in `tools/rdna2spirv.py` emits). Measured on an RX 7800 XT (gfx1101): after.

Group `vopc`: what does a VOPC compare in DPP form (`v_cmp_*_dpp` into VCC, `v_cmpx_*_dpp` into EXEC) do to the bit
of a lane DPP leaves unwritten (an invalid source with `bound_ctrl:0`, or a lane `row_mask` / `bank_mask` turn
off)? VCC is preset to 0 and to all ones, and each lane is compared with three models: the bit is written 0, kept,
or the compare of a 0 source. Measured on an RX 7800 XT (gfx1101): written 0. This is the open point of the
"DPP on a VOPC compare" row of docs/design.md.

LLVM has no gfx10 encoding for a VOPC compare in DPP form ("dpp variant of this instruction is not supported" for
gfx1030), so the compares are emitted as `.long` words: the VOPC word with src0 = 250 (DPP16) and the DPP word, laid
out as AMD clang's llvm-mc encodes them for gfx1101, with the gfx10 VOPC opcodes of its gfx1030 e32 encodings.

Usage:
    python -I tools/dpp_hw_probe.py [--group neg,vopc] [--waves 32,64] [--json out.json]
    python -I tools/dpp_hw_probe.py --self-test        # model and encoding checks, no GPU

Prints, per case and wave size, which models every lane matches. Exit 0 when every case matches at least one model
(or --self-test passed), 1 when a case matches none or the report cannot be written, 2 on a harness or CLI error,
77 when tools/hw_oracle is unavailable or the GPU is not gfx10 / gfx11. Standard library only; nothing here reads
game files.
"""
import argparse
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.dont_write_bytecode = True
if HERE not in sys.path:
    sys.path.insert(0, HERE)

SENTINEL = 0xDEADBEEF
FAMILIES = ("gfx10", "gfx11")
GROUPS = ("neg", "vopc")

# VOPC opcodes (bits [24:17] of the VOPC word). gfx10 from llvm-mc -mcpu=gfx1030 e32 encodings, gfx11 from
# llvm-mc -mcpu=gfx1101 DPP encodings (AMD LLVM 23.0.0git, rocm-sdk-core 10.0.0).
VOPC_OPCODES = {
    "gfx10": {"v_cmp_lt_f32": 0x01, "v_cmp_gt_f32": 0x04, "v_cmpx_lt_f32": 0x11, "v_cmpx_gt_f32": 0x14},
    "gfx11": {"v_cmp_lt_f32": 0x11, "v_cmp_gt_f32": 0x14, "v_cmpx_lt_f32": 0x91, "v_cmpx_gt_f32": 0x94},
}
VOPC_ENC = 0x3E          # bits [31:25] of a VOPC word
SRC0_DPP16 = 250         # src0 field value that says "a DPP16 word follows"


class Ctrl:
    """One DPP16 control: ("row_shr", n) or ("quad_perm", (a, b, c, d)), the masks, bound_ctrl and fi."""

    def __init__(self, kind, arg, row_mask=0xF, bank_mask=0xF, bound_ctrl=False, fi=False):
        if kind not in ("row_shr", "quad_perm"):
            raise ValueError(f"unsupported DPP control {kind!r}")
        if kind == "row_shr" and not 1 <= arg <= 15:
            raise ValueError(f"row_shr:{arg} is not in 1..15")
        if kind == "quad_perm" and (len(arg) != 4 or any(not 0 <= x <= 3 for x in arg)):
            raise ValueError(f"quad_perm:{list(arg)} needs four selects in 0..3")
        if not (0 <= row_mask <= 0xF and 0 <= bank_mask <= 0xF):
            raise ValueError("row_mask and bank_mask are 4-bit")
        self.kind, self.arg, self.row_mask, self.bank_mask = kind, arg, row_mask, bank_mask
        self.bound_ctrl, self.fi = bound_ctrl, fi

    def dpp_ctrl(self):
        """The 9-bit DPP_CTRL field: quad_perm 0x000-0x0FF, row_shr:n 0x110 + n."""
        if self.kind == "quad_perm":
            a, b, c, d = self.arg
            return a | b << 2 | c << 4 | d << 6
        return 0x110 + self.arg

    def text(self):
        """The control as assembler syntax (bound_ctrl:0 is written by leaving it out)."""
        sel = f"row_shr:{self.arg}" if self.kind == "row_shr" else "quad_perm:[" + ",".join(map(str, self.arg)) + "]"
        out = f"{sel} row_mask:0x{self.row_mask:x} bank_mask:0x{self.bank_mask:x}"
        return out + (" bound_ctrl:1" if self.bound_ctrl else "") + (" fi:1" if self.fi else "")

    def source(self, lane):
        """The lane this lane reads, or None when that is out of its row of 16."""
        if self.kind == "row_shr":
            return lane - self.arg if lane % 16 >= self.arg else None
        return (lane & ~3) + self.arg[lane & 3]

    def masked(self, lane):
        """True when row_mask or bank_mask turn this lane off (DPP does not write it)."""
        return not (self.row_mask >> (lane // 16) & 1 and self.bank_mask >> ((lane % 16) // 4) & 1)


def dpp_word(src0, ctrl, src0_neg=False, src0_abs=False, src1_neg=False, src1_abs=False):
    """The DPP16 word: src0 [7:0], DPP_CTRL [16:8], FI [18], BOUND_CTRL [19], src0/src1 neg [20]/[22] and abs
    [21]/[23], BANK_MASK [27:24], ROW_MASK [31:28]."""
    if not 0 <= src0 <= 255:
        raise ValueError(f"src0 v{src0} is not a VGPR")
    return (src0 | ctrl.dpp_ctrl() << 8 | ctrl.fi << 18 | ctrl.bound_ctrl << 19 | src0_neg << 20 | src0_abs << 21
            | src1_neg << 22 | src1_abs << 23 | ctrl.bank_mask << 24 | ctrl.row_mask << 28)


def vopc_dpp_words(fam, op, src0, vsrc1, ctrl):
    """The two words of `op` (VOPC, f32) in DPP16 form: compare v<src0> of the selected lane with this lane's
    v<vsrc1>."""
    if fam not in VOPC_OPCODES:
        raise ValueError(f"no VOPC opcodes for {fam!r} (expected one of {', '.join(FAMILIES)})")
    if not 0 <= vsrc1 <= 255:
        raise ValueError(f"vsrc1 v{vsrc1} is not a VGPR")
    word0 = VOPC_ENC << 25 | VOPC_OPCODES[fam][op] << 17 | vsrc1 << 9 | SRC0_DPP16
    return word0, dpp_word(src0, ctrl)


def f32(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


# ---- group neg: src0 modifiers and the bound_ctrl zero ---------------------------------------------------------

# (name, op, src0 modifier, control, EXEC pattern); every lane has v1 = lane + 1.0, v2 = -0.0, v7 = 1.0
NEG_CASES = [
    ("add_neg_row_shr", "v_add_f32", "neg", Ctrl("row_shr", 1, bound_ctrl=True), "all"),
    ("add_plain_row_shr", "v_add_f32", "", Ctrl("row_shr", 1, bound_ctrl=True), "all"),
    ("add_abs_row_shr", "v_add_f32", "abs", Ctrl("row_shr", 1, bound_ctrl=True), "all"),
    ("mul_neg_row_shr", "v_mul_f32", "neg", Ctrl("row_shr", 1, bound_ctrl=True), "all"),
    ("add_neg_row_shr_even_fi0", "v_add_f32", "neg", Ctrl("row_shr", 1, bound_ctrl=True), "even"),
    ("add_neg_row_shr_even_fi1", "v_add_f32", "neg", Ctrl("row_shr", 1, bound_ctrl=True, fi=True), "even"),
    ("add_neg_quad_even_fi0", "v_add_f32", "neg", Ctrl("quad_perm", (1, 0, 3, 2), bound_ctrl=True), "even"),
]
NEG_MODELS = ("mods_after_zero", "mods_before_zero")


def lane_active(lane, exec_pattern):
    return exec_pattern == "all" or lane % 2 == 0


def source_valid(lane, ctrl, exec_pattern):
    src = ctrl.source(lane)
    return src is not None and (lane_active(src, exec_pattern) or ctrl.fi)


def neg_model(case, lane, model):
    """The word lane `lane` stores for a NEG_CASES entry under `model`."""
    _, op, mod, ctrl, ex = case
    if not lane_active(lane, ex) or ctrl.masked(lane):
        return SENTINEL
    if source_valid(lane, ctrl, ex):
        x = ctrl.source(lane) + 1.0
        x = abs(x) if mod == "abs" else -x if mod == "neg" else x
    else:
        x = 0.0
        if model == "mods_after_zero" and mod == "neg":
            x = -0.0
    return f32(x * 1.0) if op == "v_mul_f32" else f32(x + -0.0)


def neg_text(case, dst):
    _, op, mod, ctrl, _ = case
    src0 = "-v1" if mod == "neg" else "|v1|" if mod == "abs" else "v1"
    src1 = "v7" if op == "v_mul_f32" else "v2"
    return f"{op}_dpp v{dst}, {src0}, {src1} {ctrl.text()}"


# ---- group vopc: the mask bit of lanes DPP does not write -----------------------------------------------------

# (name, control, EXEC pattern); every lane has v1 = lane + 1.0 (> 0.5), v2 = 0.5
VOPC_CTRLS = [
    ("row_shr_bc0", Ctrl("row_shr", 1), "all"),
    ("row_shr_bc1", Ctrl("row_shr", 1, bound_ctrl=True), "all"),
    ("row_mask5_bc1", Ctrl("quad_perm", (0, 1, 2, 3), row_mask=0x5, bound_ctrl=True), "all"),
    ("bank_mask5_bc1", Ctrl("quad_perm", (0, 1, 2, 3), bank_mask=0x5, bound_ctrl=True), "all"),
    ("quad_even_fi0_bc0", Ctrl("quad_perm", (1, 0, 3, 2)), "even"),
    ("quad_even_fi0_bc1", Ctrl("quad_perm", (1, 0, 3, 2), bound_ctrl=True), "even"),
]
VOPC_OPS = ("v_cmp_gt_f32", "v_cmp_lt_f32", "v_cmpx_gt_f32", "v_cmpx_lt_f32")
VOPC_MODELS = ("unwritten_gets_0", "unwritten_keeps_bit", "unwritten_compares_0")


def vopc_runs():
    """(name, op, control, EXEC pattern, VCC preset or None) for every run; v_cmpx has no preset (EXEC is its
    destination and starts as the case's pattern)."""
    runs = []
    for op in VOPC_OPS:
        for name, ctrl, ex in VOPC_CTRLS:
            for preset in ((0, 1) if op.startswith("v_cmp_") else (None,)):
                runs.append((f"{op}_{name}" + ("" if preset is None else f"_preset{preset}"), op, ctrl, ex, preset))
    return runs


def lane_category(lane, ctrl, ex):
    """inactive / masked / valid / zero_src (bound_ctrl:1 zero) / unwritten (bound_ctrl:0, invalid source)."""
    if not lane_active(lane, ex):
        return "inactive"
    if ctrl.masked(lane):
        return "masked"
    if source_valid(lane, ctrl, ex):
        return "valid"
    return "zero_src" if ctrl.bound_ctrl else "unwritten"


def vopc_model(run, lane, model):
    """The bit lane `lane` holds after a vopc run under `model` (VCC for v_cmp, EXEC for v_cmpx)."""
    _, op, ctrl, ex, preset = run
    gt = op.endswith("gt_f32")
    cat = lane_category(lane, ctrl, ex)
    if cat == "inactive":
        return 0
    if cat == "valid":
        return int(gt)                 # lane + 1.0 > 0.5
    if cat == "zero_src":
        return int(not gt)             # 0 < 0.5
    if model == "unwritten_gets_0":
        return 0
    if model == "unwritten_keeps_bit":
        return 1 if preset is None else preset   # v_cmpx: the lane was on in EXEC
    return int(not gt)                 # unwritten_compares_0


# ---- kernels ----------------------------------------------------------------------------------------------------

def _set32(lo, hi, value, wave):
    return [f"s_mov_b32 {lo}, {value}"] + ([f"s_mov_b32 {hi}, {value}"] if wave == 64 else [])


def _exec(pattern, wave):
    return _set32("exec_lo", "exec_hi", -1 if pattern == "all" else "0x55555555", wave)


def _guard(lines):
    """s_nop 7 after every instruction: the VALU -> SALU reads of VCC / EXEC need wait states, and the model has
    none (as crosslane_fuzz's guard)."""
    out = []
    for ln in lines:
        out.append(ln)
        if ln != "s_endpgm":
            out.append("s_nop 7")
    return out


def neg_lines(wave):
    """Kernel body for group neg: one word per case and lane."""
    dsts = [d for d in range(3, 4 + len(NEG_CASES)) if d != 7][:len(NEG_CASES)]   # v7 holds the multiplier
    body = ["v_cvt_f32_u32_e32 v1, v0", "v_add_f32_e32 v1, 1.0, v1", "v_mov_b32_e32 v2, 0x80000000",
            "v_mov_b32_e32 v7, 1.0"]
    body +=[f"v_mov_b32_e32 v{d}, 0x{SENTINEL:08x}" for d in dsts]
    for case, d in zip(NEG_CASES, dsts):
        body += _exec(case[4], wave) + [neg_text(case, d)] + _exec("all", wave)
    body.append(f"v_mul_u32_u24_e32 v20, {4 * len(NEG_CASES)}, v0")
    body += [f"global_store_b32 v20, v{d}, s[2:3] offset:{4 * j}" for j, d in enumerate(dsts)]
    body.append("s_endpgm")
    return _guard(body)


def vopc_lines(wave, fam):
    """Kernel body for group vopc: two words (the mask) per run; every lane stores the same words."""
    runs = vopc_runs()
    body = ["v_cvt_f32_u32_e32 v1, v0", "v_add_f32_e32 v1, 1.0, v1", "v_mov_b32_e32 v2, 0.5",
            f"v_mul_u32_u24_e32 v20, {8 * len(runs)}, v0"]
    for i, (_, op, ctrl, ex, preset) in enumerate(runs):
        if preset is not None:
            body += _set32("vcc_lo", "vcc_hi", -preset, wave)       # 0 or -1
        body += _exec(ex, wave)
        w0, w1 = vopc_dpp_words(fam, op, 1, 2, ctrl)
        body.append(f".long 0x{w0:08x}, 0x{w1:08x}  ; {op}_dpp {ctrl.text()}")
        mask = "exec" if op.startswith("v_cmpx_") else "vcc"
        body += ["s_mov_b32 s10, " + mask + "_lo", "s_mov_b32 s11, " + (mask + "_hi" if wave == 64 else "0")]
        body += _exec("all", wave)
        body += ["v_mov_b32_e32 v3, s10", "v_mov_b32_e32 v4, s11",
                 f"global_store_b32 v20, v3, s[2:3] offset:{8 * i}",
                 f"global_store_b32 v20, v4, s[2:3] offset:{8 * i + 4}"]
    body.append("s_endpgm")
    return _guard(body)


# ---- comparison -------------------------------------------------------------------------------------------------

def judge_neg(wave, words):
    """{case: {"models": [...matching], "lanes": {lane: word} for lanes 0, 1, 2, 15, 16, 17}} for group neg."""
    n = len(NEG_CASES)
    out = {}
    for j, case in enumerate(NEG_CASES):
        got = [words[lane * n + j] for lane in range(wave)]
        models = [m for m in NEG_MODELS if all(got[l] == neg_model(case, l, m) for l in range(wave))]
        out[case[0]] = {"instruction": neg_text(case, "D"), "exec": case[4], "models": models,
                        "lanes": {str(l): f"0x{got[l]:08x}" for l in (0, 1, 2, 15, 16, 17)}}
    return out


def judge_vopc(wave, words):
    """{run: {"models": [...matching], "mask": hex}} for group vopc, from lane 0's words; also checks that every
    lane stored the same words."""
    runs = vopc_runs()
    per = 2 * len(runs)
    if any(words[l * per:(l + 1) * per] != words[:per] for l in range(wave)):
        raise RuntimeError(f"wave{wave}: lanes stored different mask words")
    out = {}
    for i, run in enumerate(runs):
        mask = words[2 * i] | words[2 * i + 1] << 32
        models = [m for m in VOPC_MODELS if all((mask >> l & 1) == vopc_model(run, l, m) for l in range(wave))]
        out[run[0]] = {"control": run[2].text(), "exec": run[3], "models": models,
                       "mask": f"0x{mask:0{wave // 4}x}"}
    return out


def probe(oracle, asm_mod, fam, groups, waves):
    report = {}
    kernels, keys = [], []
    for wave in waves:
        if "neg" in groups:
            kernels.append(asm_mod.Kernel(neg_lines(wave), wave, wave, wave * len(NEG_CASES), raw=True))
            keys.append(("neg", wave))
        if "vopc" in groups:
            kernels.append(asm_mod.Kernel(vopc_lines(wave, fam), wave, wave, wave * 2 * len(vopc_runs()), raw=True))
            keys.append(("vopc", wave))
    for (group, wave), got in zip(keys, oracle.run(kernels)):
        if isinstance(got, str):
            raise RuntimeError(f"{group} wave{wave}: {got}")
        judged = judge_neg(wave, got) if group == "neg" else judge_vopc(wave, got)
        report.setdefault(group, {})[str(wave)] = judged
    return report


def summary(report):
    """Per group and wave: the models every case matches, and the cases that match none."""
    out = {}
    for group, by_wave in report.items():
        models = NEG_MODELS if group == "neg" else VOPC_MODELS
        for wave, cases in by_wave.items():
            common = [m for m in models if all(m in c["models"] for c in cases.values())]
            none = sorted(k for k, c in cases.items() if not c["models"])
            out[f"{group} wave{wave}"] = {"consistent_models": common, "unmatched_cases": none}
    return out


# ---- self-test and CLI --------------------------------------------------------------------------------------------

def self_test():
    # DPP words as AMD clang's llvm-mc encodes them for gfx1101 and gfx1030 (v_max_f32_dpp carries the same word)
    assert dpp_word(1, Ctrl("row_shr", 1)) == 0xFF011101
    assert dpp_word(3, Ctrl("quad_perm", (1, 0, 3, 2), row_mask=5, bank_mask=0xA, bound_ctrl=True, fi=True)) == 0x5A0CB103
    assert dpp_word(3, Ctrl("quad_perm", (2, 3, 0, 1)), src0_neg=True, src1_abs=True) == 0xFF904E03
    # gfx11 VOPC DPP words (llvm-mc -mcpu=gfx1101 --show-encoding)
    assert vopc_dpp_words("gfx11", "v_cmp_lt_f32", 1, 2, Ctrl("row_shr", 1)) == (0x7C2204FA, 0xFF011101)
    assert vopc_dpp_words("gfx11", "v_cmp_gt_f32", 1, 2, Ctrl("row_shr", 1))[0] == 0x7C2804FA
    assert vopc_dpp_words("gfx11", "v_cmpx_lt_f32", 1, 2, Ctrl("row_shr", 1))[0] == 0x7D2204FA
    assert vopc_dpp_words("gfx11", "v_cmpx_gt_f32", 1, 2, Ctrl("row_shr", 1))[0] == 0x7D2804FA
    # gfx10 opcodes: the e32 words llvm-mc -mcpu=gfx1030 prints, with src0 = 250 in place of v1
    for op, e32 in (("v_cmp_lt_f32", 0x7C020501), ("v_cmp_gt_f32", 0x7C080501), ("v_cmpx_lt_f32", 0x7C220501),
                    ("v_cmpx_gt_f32", 0x7C280501)):
        assert vopc_dpp_words("gfx10", op, 1, 2, Ctrl("row_shr", 1))[0] == (e32 & ~0x1FF) | SRC0_DPP16, op
    # the neg models differ exactly on the invalid lanes of the negated cases
    case = NEG_CASES[0]
    diff = [l for l in range(32) if neg_model(case, l, "mods_after_zero") != neg_model(case, l, "mods_before_zero")]
    assert diff == [0, 16], diff
    assert neg_model(case, 0, "mods_after_zero") == 0x80000000 and neg_model(case, 0, "mods_before_zero") == 0
    assert neg_model(NEG_CASES[2], 0, "mods_after_zero") == 0            # |0| + -0
    # vopc: the three models are told apart across the two presets and the two compares
    runs = {r[0]: r for r in vopc_runs()}
    lane0 = lambda name, m: vopc_model(runs[name], 0, m)
    assert [lane0("v_cmp_gt_f32_row_shr_bc0_preset1", m) for m in VOPC_MODELS] == [0, 1, 0]
    assert [lane0("v_cmp_lt_f32_row_shr_bc0_preset0", m) for m in VOPC_MODELS] == [0, 0, 1]
    assert vopc_model(runs["v_cmp_gt_f32_row_mask5_bc1_preset1"], 16, "unwritten_keeps_bit") == 1
    assert lane_category(16, VOPC_CTRLS[2][1], "all") == "masked"
    assert lane_category(1, VOPC_CTRLS[4][1], "even") == "inactive"
    assert lane_category(0, VOPC_CTRLS[4][1], "even") == "unwritten"
    print("self-test OK")


def parse_list(text, allowed, what):
    tokens = [t.strip() for t in text.split(",") if t.strip()]
    if not tokens:
        raise ValueError(f"--{what} must not be empty")
    out = []
    for tok in tokens:
        if tok not in allowed:
            raise ValueError(f"invalid {what[:-1]} {tok!r}; expected {' or '.join(allowed)}")
        if tok not in out:
            out.append(tok)
    return out


def validate_json_path(path):
    """An error string if `path` cannot be a --json output file, or None."""
    if not path or not path.strip():
        return "--json path must not be empty"
    if "\x00" in path:
        return "--json path contains a NUL byte"
    if path.endswith(os.sep) or (os.altsep and path.endswith(os.altsep)) or os.path.isdir(path):
        return f"--json path is a directory: {path}"
    cur = os.path.dirname(os.path.abspath(path))
    while cur:
        if os.path.isdir(cur):
            break
        if os.path.lexists(cur):
            return f"--json parent is not a directory: {cur}"
        nxt = os.path.dirname(cur)
        if nxt == cur:
            break
        cur = nxt
    return None


def write_json_report(path, data):
    """Create missing parent directories and write `data` as LF-terminated JSON, or return an error string."""
    parent = os.path.dirname(os.path.abspath(path))
    try:
        os.makedirs(parent, exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(data, f, indent=2)
            f.write("\n")
    except OSError as e:
        return f"cannot write --json {path}: {e.strerror or e}"
    return None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--group", default="neg,vopc", help="case groups to run: neg, vopc (default: both)")
    ap.add_argument("--waves", default="32,64", help="wave sizes to run (default: 32,64)")
    ap.add_argument("--json", dest="json_out", help="write the full report to this path")
    ap.add_argument("--self-test", action="store_true", help="check the models and encodings without a GPU")
    args = ap.parse_args(argv)
    if args.self_test:
        self_test()
        return 0
    try:
        groups = parse_list(args.group, GROUPS, "groups")
        waves = [int(w) for w in parse_list(args.waves, ("32", "64"), "waves")]
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    if args.json_out is not None:
        err = validate_json_path(args.json_out)
        if err:
            print(f"error: {err}", file=sys.stderr)
            return 2
    from hw_oracle import asm, runner
    try:
        oracle = runner.open_oracle()
    except runner.Unavailable as exc:
        print(f"SKIP: {exc}", file=sys.stderr)
        return 77
    try:
        try:
            fam = asm.family(oracle.arch)
        except ValueError:
            fam = None
        if fam not in FAMILIES:
            print(f"SKIP: {oracle.arch} is not gfx10 or gfx11 (no VOPC opcodes for it here)", file=sys.stderr)
            return 77
        try:
            report = probe(oracle, asm, fam, groups, waves)
        except RuntimeError as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 2
    finally:
        close = getattr(oracle, "close", None)
        if close:
            close()
    summ = summary(report)
    for key, s in summ.items():
        models = ", ".join(s["consistent_models"]) or "none"
        print(f"{key} ({oracle.arch}): every case matches {models}"
              + (f"; no model matches {', '.join(s['unmatched_cases'])}" if s["unmatched_cases"] else ""))
    if args.json_out is not None:
        err = write_json_report(args.json_out, {"arch": oracle.arch, "summary": summ, "cases": report})
        if err:
            print(f"error: {err}", file=sys.stderr)
            return 1
    return 1 if any(s["unmatched_cases"] for s in summ.values()) else 0


if __name__ == "__main__":
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")
    raise SystemExit(main())
