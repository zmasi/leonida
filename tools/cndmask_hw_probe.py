r"""Measure the VOP3 source modifiers of `v_cndmask_b32` natively through `tools/hw_oracle` (TOOL-CNDMASK-HW-PROBE).

Are `abs` / `neg` on `v_cndmask_b32_e64`'s sources sign-bit moves (`abs` clears bit 31, `neg` then flips it), or
float operations that quiet a signalling NaN or flush a denormal? The translator emits sign-bit moves
(`Gen.src_u_sign_mods` in `tools/rdna2spirv.py`); this is the "`v_cndmask_b32` source modifiers" row of
docs/design.md. Measured on an RX 7800 XT (gfx1101): sign-bit moves in every mode.

Each kernel runs one lane. Every word of WORDS goes through `v`, `-v`, `|v|` and `-|v|`, as the selected src0
(condition 0) and as the selected src1 (condition all ones); the other source is 0x12345678. There is one kernel per
wave size and MODE (`ieee` 0/1 x `denorm32` 0/3). Each kernel also stores two float ops as controls that show its
MODE is in effect: `v_add_f32(+min denormal, 0)` is flushed exactly when denorm32 = 0, and
`v_mul_f32(sNaN, 1.0)` is quieted exactly when ieee = 1.

Usage:
    python -I tools/cndmask_hw_probe.py [--waves 32,64] [--json out.json]
    python -I tools/cndmask_hw_probe.py --self-test        # model checks, no GPU

Exit 0 when every result is a sign-bit move and every control shows its MODE (or --self-test passed), 1 when one is
not or the report cannot be written, 2 on a harness or CLI error, 77 when tools/hw_oracle is unavailable or the GPU
cannot set these MODEs (not gfx10 / gfx11, or gfx1170 and later). Standard library only; nothing here reads game
files.
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
WORDS = (
    (0x00000000, "+0"), (0x80000000, "-0"), (0x00000001, "+min denormal"), (0x80000001, "-min denormal"),
    (0x007FFFFF, "+max denormal"), (0x3F800000, "+1.0"), (0xC0000000, "-2.0"), (0x7F7FFFFF, "+max normal"),
    (0x7F800000, "+inf"), (0xFF800000, "-inf"), (0x7FC00000, "+qNaN"), (0xFFC00001, "-qNaN payload 1"),
    (0x7F800001, "+sNaN payload 1"), (0xFF800001, "-sNaN payload 1"), (0x7FA00000, "+sNaN payload 0x200000"),
)
MODS = (("v", "{}"), ("-v", "-{}"), ("|v|", "|{}|"), ("-|v|", "-|{}|"))
SIDES = ("src0", "src1")
OTHER = 0x12345678
MODES = tuple((ieee, den) for ieee in (0, 1) for den in (0, 3))
# (name, input word, instruction writing v3 from v1)
CONTROLS = (
    ("denorm_add", 0x00000001, "v_add_f32_e64 v3, v1, 0"),
    ("snan_mul", 0x7F800001, "v_mul_f32_e64 v3, v1, 1.0"),
)
CLASSES = ("bit_move", "quieted", "flushed", "other")


def runs():
    """(word, name, mod, side) for every result a kernel stores before its controls."""
    return [(w, name, mod, side) for w, name in WORDS for mod, _ in MODS for side in SIDES]


def kernel_lines(wave):
    """The raw kernel body (GFX11 spelling; hw_oracle renames the stores for GFX10)."""
    if wave not in (32, 64):
        raise ValueError(f"wave size {wave} is not 32 or 64")
    cond = "s4" if wave == 32 else "s[4:5]"
    fmt = dict(MODS)
    body = ["v_mov_b32_e32 v10, 0", f"v_mov_b32_e32 v2, 0x{OTHER:08x}"]
    for k, (w, _, mod, side) in enumerate(runs()):
        sel = -1 if side == "src1" else 0
        body += [f"v_mov_b32_e32 v1, 0x{w:08x}", f"s_mov_b32 s4, {sel}"]
        if wave == 64:
            body.append(f"s_mov_b32 s5, {sel}")
        a, b = (fmt[mod].format("v1"), "v2") if side == "src0" else ("v2", fmt[mod].format("v1"))
        body += [f"v_cndmask_b32_e64 v3, {a}, {b}, {cond}", f"global_store_b32 v10, v3, s[2:3] offset:{4 * k}"]
    base = 4 * len(runs())
    for j, (_, w, insn) in enumerate(CONTROLS):
        body += [f"v_mov_b32_e32 v1, 0x{w:08x}", insn, f"global_store_b32 v10, v3, s[2:3] offset:{base + 4 * j}"]
    body.append("s_endpgm")
    return body


def words_per_kernel():
    return len(runs()) + len(CONTROLS)


def bit_move(word, mod):
    """The sign-bit-move model: `abs` clears bit 31, then `neg` flips it."""
    if "|" in mod:
        word &= 0x7FFFFFFF
    if mod.startswith("-"):
        word ^= 0x80000000
    return word


def classify(word, mod, got):
    """'bit_move', 'quieted' (a signalling NaN came back quiet), 'flushed' (a denormal came back a zero of its
    sign) or 'other', against the sign-bit move of `word`."""
    want = bit_move(word, mod)
    if got == want:
        return "bit_move"
    exp, frac = (want >> 23) & 0xFF, want & 0x7FFFFF
    if exp == 0xFF and frac and not frac & 0x400000 and got == want | 0x400000:
        return "quieted"
    if exp == 0 and frac and got == want & 0x80000000:
        return "flushed"
    return "other"


def control_expected(name, ieee, denorm32):
    """The word a control stores when its MODE is in effect."""
    if name == "denorm_add":
        return 0x00000000 if denorm32 == 0 else 0x00000001
    if name == "snan_mul":
        return 0x7FC00001 if ieee else 0x7F800001
    raise ValueError(f"unknown control {name!r}")


def judge(wave, ieee, denorm32, got):
    """The report entry of one kernel's stored words."""
    if len(got) != words_per_kernel():
        raise RuntimeError(f"wave{wave} ieee={ieee} denorm32={denorm32}: {len(got)} words, "
                           f"expected {words_per_kernel()}")
    results, counts = [], {c: 0 for c in CLASSES}
    for (w, name, mod, side), g in zip(runs(), got):
        cls = classify(w, mod, g)
        counts[cls] += 1
        results.append({"word": f"0x{w:08x}", "name": name, "mod": mod, "side": side, "got": f"0x{g:08x}",
                        "class": cls})
    controls = []
    for (name, w, insn), g in zip(CONTROLS, got[len(runs()):]):
        want = control_expected(name, ieee, denorm32)
        controls.append({"name": name, "insn": insn, "input": f"0x{w:08x}", "got": f"0x{g:08x}",
                         "expected": f"0x{want:08x}", "ok": g == want})
    return {"wave": wave, "ieee": ieee, "denorm32": denorm32, "counts": counts,
            "controls_ok": all(c["ok"] for c in controls), "controls": controls, "results": results}


def report_ok(entries):
    return all(e["counts"]["bit_move"] == len(e["results"]) and e["controls_ok"] for e in entries)


def probe(oracle, asm_mod, waves):
    kernels, keys = [], []
    for wave in waves:
        lines = kernel_lines(wave)
        for ieee, den in MODES:
            kernels.append(asm_mod.Kernel(lines, wave=wave, threads=1, words=words_per_kernel(), raw=True,
                                          mode={"ieee": ieee, "denorm32": den}))
            keys.append((wave, ieee, den))
    entries = []
    for (wave, ieee, den), got in zip(keys, oracle.run(kernels)):
        if isinstance(got, str):
            raise RuntimeError(f"wave{wave} ieee={ieee} denorm32={den}: {got}")
        entries.append(judge(wave, ieee, den, got))
    return entries


def describe(entry):
    c = entry["counts"]
    line = (f"wave{entry['wave']} ieee={entry['ieee']} denorm32={entry['denorm32']}: "
            f"{c['bit_move']}/{len(entry['results'])} sign-bit moves")
    others = [f"{c[k]} {k}" for k in CLASSES[1:] if c[k]]
    if others:
        line += " (" + ", ".join(others) + ")"
    ctl = "; ".join(f"{x['name']} {x['got']}" + ("" if x["ok"] else f" (expected {x['expected']})")
                    for x in entry["controls"])
    line += f"; controls: {ctl}" + ("" if entry["controls_ok"] else " - MODE NOT SHOWN")
    return line


# ---- self-test and CLI --------------------------------------------------------------------------------------------

def self_test():
    assert bit_move(0x7F800001, "-v") == 0xFF800001
    assert bit_move(0xFF800001, "|v|") == 0x7F800001
    assert bit_move(0x80000000, "-|v|") == 0x80000000
    assert bit_move(0x80000000, "-v") == 0x00000000
    assert classify(0x7F800001, "-v", 0xFF800001) == "bit_move"
    assert classify(0x7F800001, "-v", 0xFFC00001) == "quieted"
    assert classify(0x7FC00000, "v", 0x7FC00000) == "bit_move"
    assert classify(0x00000001, "-v", 0x80000000) == "flushed"
    assert classify(0x00000001, "|v|", 0x00000000) == "flushed"
    assert classify(0x3F800000, "-v", 0x3F800000) == "other"
    assert len(runs()) == len(WORDS) * len(MODS) * len(SIDES) == 120
    for wave in (32, 64):
        lines = kernel_lines(wave)
        assert sum(ln.startswith("global_store_b32") for ln in lines) == words_per_kernel()
        assert lines[-1] == "s_endpgm"
    assert "v_cndmask_b32_e64 v3, -|v1|, v2, s4" in kernel_lines(32)
    assert "v_cndmask_b32_e64 v3, v2, -|v1|, s[4:5]" in kernel_lines(64)
    good = [bit_move(w, m) for w, _, m, _ in runs()]
    for ieee, den in MODES:
        ctl = [control_expected(n, ieee, den) for n, _, _ in CONTROLS]
        assert report_ok([judge(32, ieee, den, good + ctl)])
        assert not report_ok([judge(32, ieee, den, good + [ctl[0] ^ 1, ctl[1]])])
    quiet = [g | 0x400000 if (g >> 23) & 0xFF == 0xFF and g & 0x3FFFFF and not g & 0x400000 else g for g in good]
    entry = judge(64, 0, 0, quiet + [0, 0x7F800001])
    assert entry["counts"]["quieted"] == 4 * 2 * 3 and not report_ok([entry])
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
        if fam not in FAMILIES or asm.mode_skip_reason({"ieee": 1}, oracle.arch):
            print(f"SKIP: {oracle.arch} cannot set the IEEE mode these kernels need (gfx10 / gfx11 before "
                  "gfx1170 only)", file=sys.stderr)
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
    ok = report_ok(entries)
    print("result: " + ("every source modifier is a sign-bit move, every MODE shown" if ok
                        else "NOT every result is a sign-bit move with its MODE shown; see above"))
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
