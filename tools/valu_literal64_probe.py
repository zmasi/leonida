r"""Measure how the hardware extends a 32-bit VALU literal in a 64-bit integer operand (HW-VALU-LITERAL64-EXT).

A VALU literal is one dword. In a 64-bit integer operand its high dword comes from somewhere: the translator
sign-extends it in an i64 operand and zero-extends it in a u64/b64 operand (`Gen.valu_imm_i64` in
`tools/rdna2spirv.py`); this is the "32-bit literal source of a signed 64-bit vector op" row of docs/design.md.
Measured on an RX 7800 XT (gfx1101): the hardware does the same for every op here.

Each kernel runs one lane; there is one kernel per wave size. Every op form of CASES runs with each literal of
LITERALS and stores its 64-bit result as (lo, hi). The forms are `v_cmp_{lt,gt}_{i64,u64}` (e64 with the literal as
src0 and as src1, and e32), `v_cmpx_{lt,gt}_{i64,u64}`, `v_ashrrev_i64`, `v_lshrrev_b64`, `v_lshlrev_b64`,
`v_mad_i64_i32` and `v_mad_u64_u32`. The VGPR operand is chosen so the two models (high dword sign-extended, or
zero-extended) give different results; `0x7fffffff` is a control both models agree on.

Usage:
    python -I tools/valu_literal64_probe.py [--waves 32,64] [--json out.json]
    python -I tools/valu_literal64_probe.py --self-test        # model checks, no GPU

Exit 0 when every result follows the row's rule (or --self-test passed), 1 when one does not or the report cannot be
written, 2 on a harness or CLI error, 77 when tools/hw_oracle is unavailable or the GPU is not gfx10 / gfx11.
Standard library only; nothing here reads game files.
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.dont_write_bytecode = True
if HERE not in sys.path:
    sys.path.insert(0, HERE)

FAMILIES = ("gfx10", "gfx11")
M64 = (1 << 64) - 1
LITERALS = (0xFFFFFF9C, 0x80000000, 0x7FFFFFFF)
MODELS = ("sext", "zext")


def extend(lit, model):
    """The 64-bit value of a 32-bit literal under a model."""
    if not 0 <= lit <= 0xFFFFFFFF:
        raise ValueError(f"literal 0x{lit:x} is not 32 bits")
    if model not in MODELS:
        raise ValueError(f"unknown model {model!r}")
    return lit | 0xFFFFFFFF00000000 if model == "sext" and lit & 0x80000000 else lit


def s64(x):
    x &= M64
    return x - (1 << 64) if x >> 63 else x


class Case:
    """One op form: `kind` is "i64" (the row says sign-extend) or "u64" (zero-extend), `vin` the 64-bit value of
    v[2:3], `body(wave, lit)` the lines that leave the result in v[10:11] and `model(vin, lit64)` its value."""

    def __init__(self, name, kind, vin, body, model):
        self.name, self.kind, self.vin, self.body, self.model = name, kind, vin, body, model

    @property
    def rule(self):
        return "sext" if self.kind == "i64" else "zext"


def _rel(op):
    return {"lt": lambda a, b: a < b, "gt": lambda a, b: a > b}[op.split("_")[-2]]


def _cmp_model(op, lit_first):
    rel, signed = _rel(op), op.endswith("i64")

    def model(vin, lit):
        a, b = (lit, vin) if lit_first else (vin, lit)
        return int(rel(s64(a), s64(b)) if signed else rel(a, b))
    return model


def _sdst(wave):
    return "s4" if wave == 32 else "s[4:5]"


def _cmp_e64(op, lit_first):
    def body(wave, lit):
        a, b = (f"0x{lit:08x}", "v[2:3]") if lit_first else ("v[2:3]", f"0x{lit:08x}")
        return [f"{op}_e64 {_sdst(wave)}, {a}, {b}", "s_nop 7", f"v_cndmask_b32_e64 v10, 0, 1, {_sdst(wave)}",
                "v_mov_b32 v11, 0"]
    kind = op[-3:]
    return Case(f"{op}_e64 {'lit, v' if lit_first else 'v, lit'}", kind, 0 if kind == "i64" else 1 << 32, body,
                _cmp_model(op, lit_first))


def _cmp_e32(op):
    def body(wave, lit):
        vcc = "vcc_lo" if wave == 32 else "vcc"
        return [f"{op}_e32 {vcc}, 0x{lit:08x}, v[2:3]", "s_nop 7", f"v_cndmask_b32_e64 v10, 0, 1, {vcc}",
                "v_mov_b32 v11, 0"]
    kind = op[-3:]
    return Case(f"{op}_e32 lit, v", kind, 0 if kind == "i64" else 1 << 32, body, _cmp_model(op, True))


def _cmpx(op):
    def body(wave, lit):
        if wave == 32:
            save, read, restore = "s_mov_b32 s8, exec_lo", "s_mov_b32 s6, exec_lo", "s_mov_b32 exec_lo, s8"
        else:
            save, read, restore = "s_mov_b64 s[8:9], exec", "s_mov_b64 s[6:7], exec", "s_mov_b64 exec, s[8:9]"
        return [save, f"{op}_e64 0x{lit:08x}, v[2:3]", "s_nop 7", read, restore, "s_nop 7", "v_mov_b32 v10, s6",
                "v_mov_b32 v11, 0"]
    kind = op[-3:]
    return Case(f"{op}_e64 lit, v", kind, 0 if kind == "i64" else 1 << 32, body, _cmp_model(op, True))


def _shift(op):
    def body(wave, lit):
        return [f"{op} v[10:11], 4, 0x{lit:08x}"]

    def model(vin, lit):
        if op == "v_ashrrev_i64":
            return s64(lit) >> 4
        return lit >> 4 if op == "v_lshrrev_b64" else lit << 4
    return Case(f"{op} 4, lit", "i64" if op == "v_ashrrev_i64" else "u64", 0, body, model)


def _mad(op):
    def body(wave, lit):
        return [f"{op} v[10:11], {'s6' if wave == 32 else 's[6:7]'}, 1, 1, 0x{lit:08x}"]

    def model(vin, lit):
        return 1 + (s64(lit) if op == "v_mad_i64_i32" else lit)
    return Case(f"{op} 1, 1, lit", "i64" if op == "v_mad_i64_i32" else "u64", 0, body, model)


CASES = tuple([_cmp_e64(f"v_cmp_{r}_{t}", first) for t in ("i64", "u64") for r in ("lt", "gt")
               for first in (True, False)]
              + [_cmp_e32(f"v_cmp_{r}_{t}") for t in ("i64", "u64") for r in ("lt", "gt")]
              + [_cmpx(f"v_cmpx_{r}_{t}") for t in ("i64", "u64") for r in ("lt", "gt")]
              + [_shift(op) for op in ("v_ashrrev_i64", "v_lshrrev_b64", "v_lshlrev_b64")]
              + [_mad(op) for op in ("v_mad_i64_i32", "v_mad_u64_u32")])


def runs():
    """(case, literal) in store order."""
    return [(case, lit) for case in CASES for lit in LITERALS]


def expected(case, lit, model):
    return case.model(case.vin, extend(lit, model)) & M64


def kernel_lines(wave):
    """The raw kernel body (GFX11 spelling; hw_oracle renames the stores for GFX10)."""
    if wave not in (32, 64):
        raise ValueError(f"wave size {wave} is not 32 or 64")
    body = ["v_mov_b32 v20, 0"]
    for k, (case, lit) in enumerate(runs()):
        body += [f"v_mov_b32 v2, 0x{case.vin & 0xFFFFFFFF:08x}", f"v_mov_b32 v3, 0x{case.vin >> 32:08x}"]
        body += case.body(wave, lit)
        body += [f"global_store_b32 v20, v10, s[2:3] offset:{8 * k}",
                 f"global_store_b32 v20, v11, s[2:3] offset:{8 * k + 4}"]
    body.append("s_endpgm")
    return body


def judge(wave, got):
    """The report entry of one kernel's stored words."""
    if len(got) != 2 * len(runs()):
        raise RuntimeError(f"wave{wave}: {len(got)} words, expected {2 * len(runs())}")
    results = []
    for k, (case, lit) in enumerate(runs()):
        value = got[2 * k] | got[2 * k + 1] << 32
        models = [m for m in MODELS if expected(case, lit, m) == value]
        results.append({"case": case.name, "operand": case.kind, "literal": f"0x{lit:08x}",
                        "got": f"0x{value:016x}", "models": models, "rule": case.rule,
                        "follows_rule": case.rule in models,
                        "control": expected(case, lit, "sext") == expected(case, lit, "zext")})
    return {"wave": wave, "results": results, "ok": all(r["follows_rule"] for r in results)}


def describe(entry):
    res = entry["results"]
    tell = [r for r in res if not r["control"]]
    by = {m: sum(r["models"] == [m] for r in tell) for m in MODELS}
    line = (f"wave{entry['wave']}: {sum(r['follows_rule'] for r in res)}/{len(res)} follow the rule "
            f"(i64 sign-extends, u64/b64 zero-extends); {by['sext']} match only sext, {by['zext']} only zext, "
            f"{sum(r['control'] for r in res)} controls")
    for r in res:
        if not r["follows_rule"]:
            line += (f"\n    {r['case']} lit={r['literal']}: got {r['got']}, matches "
                     f"{', '.join(r['models']) or 'neither model'} (rule: {r['rule']})")
    return line


def probe(oracle, asm_mod, waves):
    kernels = [asm_mod.Kernel(kernel_lines(w), wave=w, threads=1, words=2 * len(runs()), raw=True) for w in waves]
    entries = []
    for wave, got in zip(waves, oracle.run(kernels)):
        if isinstance(got, str):
            raise RuntimeError(f"wave{wave}: {got}")
        entries.append(judge(wave, got))
    return entries


# ---- self-test and CLI --------------------------------------------------------------------------------------------

def self_test():
    assert extend(0xFFFFFF9C, "sext") == 0xFFFFFFFFFFFFFF9C and extend(0xFFFFFF9C, "zext") == 0xFFFFFF9C
    assert extend(0x7FFFFFFF, "sext") == extend(0x7FFFFFFF, "zext") == 0x7FFFFFFF
    assert len(CASES) == 21 and len({c.name for c in CASES}) == 21
    assert sum(c.kind == "i64" for c in CASES) == 10
    for case in CASES:
        for lit in LITERALS:
            same = expected(case, lit, "sext") == expected(case, lit, "zext")
            assert same == (lit == 0x7FFFFFFF), (case.name, hex(lit))
    named = {c.name: c for c in CASES}
    # the words the row of docs/design.md gives
    assert expected(named["v_cmp_lt_i64_e64 v, lit"], 0xFFFFFF9C, "sext") == 0
    assert expected(named["v_ashrrev_i64 4, lit"], 0xFFFFFF9C, "sext") == 0xFFFFFFFFFFFFFFF9
    assert s64(expected(named["v_mad_i64_i32 1, 1, lit"], 0xFFFFFF9C, "sext")) == -99
    assert expected(named["v_lshlrev_b64 4, lit"], 0x80000000, "zext") == 0x800000000
    for wave in (32, 64):
        lines = kernel_lines(wave)
        assert sum(ln.startswith("global_store_b32") for ln in lines) == 2 * len(runs())
    rule_words = []
    for case, lit in runs():
        v = expected(case, lit, case.rule)
        rule_words += [v & 0xFFFFFFFF, v >> 32]
    assert judge(32, rule_words)["ok"]
    all_zext = []
    for case, lit in runs():
        v = expected(case, lit, "zext")
        all_zext += [v & 0xFFFFFFFF, v >> 32]
    assert not judge(64, all_zext)["ok"]
    print("self-test OK")


def parse_waves(text):
    out = []
    for tok in (t.strip() for t in text.split(",")):
        if tok not in ("32", "64"):
            raise ValueError(f"invalid wave {tok!r}; expected 32 or 64")
        if int(tok) not in out:
            out.append(int(tok))
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
    ap.add_argument("--waves", default="32,64", help="wave sizes to run (default: 32,64)")
    ap.add_argument("--json", dest="json_out", help="write the full report to this path")
    ap.add_argument("--self-test", action="store_true", help="check the models without a GPU")
    args = ap.parse_args(argv)
    if args.self_test:
        self_test()
        return 0
    try:
        waves = parse_waves(args.waves)
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
            print(f"SKIP: {oracle.arch} is not gfx10 or gfx11", file=sys.stderr)
            return 77
        try:
            entries = probe(oracle, asm, waves)
        except RuntimeError as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 2
    finally:
        close = getattr(oracle, "close", None)
        if close:
            close()
    print(f"device: {oracle.arch}")
    for entry in entries:
        print(describe(entry))
    ok = all(e["ok"] for e in entries)
    print("result: " + ("every literal is extended as the row says" if ok
                        else "NOT every literal is extended as the row says; see above"))
    if args.json_out is not None:
        err = write_json_report(args.json_out, {"arch": oracle.arch, "ok": ok, "kernels": entries})
        if err:
            print(f"error: {err}", file=sys.stderr)
            return 1
    return 0 if ok else 1


if __name__ == "__main__":
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")
    raise SystemExit(main())
