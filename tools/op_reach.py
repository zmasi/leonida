"""How many shaders of a disassembled corpus use each instruction, all encodings of it counted as one.

    python -I tools/op_reach.py <work>/shaders/dxbc/disasm [OP ...] [--index <work>/shaders/dxbc/index.json]
                                [--fssm <work>/shaders/dxbc/fssm] [--json OUT]

Reads every `.s` listing in the folder (shader_disasm.py output: an instruction line starts with a tab). An
instruction's base name drops the encoding suffix LLVM prints (`_e32`, `_e64`, `_sdwa`, `_dpp`, the same ones
rdna2spirv's `base_op()` strips), so `v_cndmask_b32_e32` and `v_cndmask_b32_e64` count as `v_cndmask_b32`, and a
shader using both counts once. tools/shader_profile.py counts the printed names instead.

An OP with `*` or `?` is a pattern over the printed names instead (`fnmatch`, case-sensitive): `'*_dpp'` counts
the shaders with any DPP instruction, `'v_*_f16_sdwa'` the 16-bit SDWA forms.

Only the program counts: lines after a listing's last `s_endpgm` are data words LLVM decoded anyway (some
listings run past the program; on 1.0.2.1 every `s_cmov_b32` in a listing is there), so they are left out and
their total is reported separately as `after_last_endpgm`. A listing with no `s_endpgm` counts whole.

With OPs, prints those only, an OP no listing uses with 0 (that is the answer to "does the game reach this
rule"). Without, prints every base name, most-used first. `--index` (the carve index) adds the count per stage;
`--fssm` (the folder of sidecar `.fssm` headers) the count per compiled wave size, from the header flag
`fssm_header.wave_size()` reads (`u32[0xD4]` bit 5), and with both, per stage and wave size.

Standard library only. Prints and writes counts and instruction names only, nothing from the listings.
"""
import argparse
import fnmatch
import json
import os
import re
import sys
import tempfile
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import file_mode  # noqa: E402
import fssm_header  # noqa: E402
import replace_retry  # noqa: E402
from report_paths import same_report_path  # noqa: E402

INSTRUCTION = re.compile(r"^\t([a-z][a-z0-9_]*)", re.M)
ENCODING = re.compile(r"_(e32|e64|sdwa|dpp)$")
HEADER_BYTES = 0x100            # wave_size() reads u32[0xD4]


def base_name(op):
    """The instruction without the encoding suffix LLVM prints."""
    return ENCODING.sub("", op)


def is_pattern(op):
    return any(c in op for c in "*?[")


def listing_ops(text):
    """(printed name -> instructions up to the last s_endpgm, instructions after it) for one listing."""
    ops = INSTRUCTION.findall(text)
    end = max((i for i, op in enumerate(ops) if op == "s_endpgm"), default=len(ops) - 1)
    return Counter(ops[:end + 1]), len(ops) - end - 1


def stages_of(index_path):
    """Listing stem -> stage from the carve index."""
    with open(index_path, encoding="utf-8-sig") as f:
        try:
            index = json.load(f)
        except RecursionError:
            raise ValueError("invalid index.json") from None
    if not isinstance(index, list) or not all(isinstance(e, dict) for e in index):
        raise ValueError("index.json is not a list of shader entries")
    stages = {}
    ambiguous = set()
    for e in index:
        raw_file = e.get("file")
        if not isinstance(raw_file, str):
            continue
        stem = os.path.splitext(os.path.basename(raw_file.replace("\\", "/")))[0]
        if not stem:
            continue
        stage = e.get("stage")
        if stage is not None and not isinstance(stage, str):
            raise ValueError("index.json entry stage must be a string")
        stage = stage.strip() if stage is not None else ""
        if not stage or stem in ambiguous:
            continue
        prev = stages.get(stem)
        if prev is not None and prev != stage:
            del stages[stem]
            ambiguous.add(stem)
        else:
            stages[stem] = stage
    return stages


def waves_of(fssm_dir, stems):
    """Listing stem -> 32, 64, or None (no sidecar, or not an FSSM header)."""
    waves = {}
    for stem in stems:
        path = os.path.join(fssm_dir, stem + ".fssm")
        if not os.path.isfile(path):
            waves[stem] = None
            continue
        try:
            with open(path, "rb") as f:
                waves[stem] = fssm_header.wave_size(f.read(HEADER_BYTES))
        except FileNotFoundError:
            waves[stem] = None
    return waves


def group_keys(stage, wave, has_waves):
    """(stage key, wave key, stage-and-wave key) a listing counts under; None where that split is not asked for."""
    w = (f"w{wave}" if wave else "w?") if has_waves else None
    return stage, w, (f"{stage}/{w}" if stage and w else None)


def census(disasm_dir, stages=None, fssm_dir=None):
    """Every .s file's printed-name counts, with its stage (index) and wave size (FSSM header) when given."""
    names = sorted(
        n for n in os.listdir(disasm_dir)
        if n.endswith(".s") and os.path.isfile(os.path.join(disasm_dir, n))
    )
    stems = [n[:-2] for n in names]
    waves = waves_of(fssm_dir, stems) if fssm_dir is not None else None
    listings = []
    after = 0
    by_name = defaultdict(list)     # printed name -> [(listing, count)]
    for i, (name, stem) in enumerate(zip(names, stems)):
        with open(os.path.join(disasm_dir, name), encoding="utf-8", errors="replace") as f:
            ops, tail = listing_ops(f.read())
        after += tail
        stage = (stages.get(stem) or "unindexed") if stages is not None else None
        wave = waves[stem] if waves is not None else None
        listings.append((stage, wave))
        for op, count in ops.items():
            by_name[op].append((i, count))
    groups = [Counter(), Counter(), Counter()]
    for stage, wave in listings:
        for g, key in zip(groups, group_keys(stage, wave, waves is not None)):
            if key:
                g[key] += 1
    shaders, instructions = Counter(), Counter()
    for op, uses in by_name.items():
        base = base_name(op)
        instructions[base] += sum(c for _, c in uses)
    for base in instructions:
        shaders[base] = len(_users(by_name, [op for op in by_name if base_name(op) == base]))
    return {"listings": len(names), "after_last_endpgm": after, "has_stages": stages is not None,
            "has_waves": waves is not None, "listing_groups": listings, "by_name": by_name,
            "listings_by_stage": dict(sorted(groups[0].items())), "listings_by_wave": dict(sorted(groups[1].items())),
            "listings_by_stage_wave": dict(sorted(groups[2].items())),
            "shaders": shaders, "instructions": instructions}


def _users(by_name, names):
    users = set()
    for op in names:
        users.update(i for i, _ in by_name[op])
    return users


def report(result, ops=None):
    """Rows for the selected OPs (base names or patterns), or every base name: shaders, instructions, and the
    shader count per stage / wave size / both when the census has them."""
    by_name = result["by_name"]
    if ops:
        chosen = list(dict.fromkeys(op if is_pattern(op) else base_name(op) for op in ops))
    else:
        chosen = sorted(result["shaders"], key=lambda o: (-result["shaders"][o], o))
    rows = []
    for op in chosen:
        if is_pattern(op):
            names = [n for n in by_name if fnmatch.fnmatchcase(n, op)]
        else:
            names = [n for n in by_name if base_name(n) == op]
        users = _users(by_name, names)
        row = {"op": op, "shaders": len(users), "instructions": sum(c for n in names for _, c in by_name[n])}
        groups = [Counter(), Counter(), Counter()]
        for i in users:
            stage, wave = result["listing_groups"][i]
            for g, key in zip(groups, group_keys(stage, wave, result["has_waves"])):
                if key:
                    g[key] += 1
        row["by_stage"] = dict(sorted(groups[0].items()))
        if result["has_waves"]:
            row["by_wave"] = dict(sorted(groups[1].items()))
            if result["has_stages"]:
                row["by_stage_wave"] = dict(sorted(groups[2].items()))
        rows.append(row)
    return rows


def _inside_dir(path, folder):
    """True when `path` is `folder` itself or resides inside it (case/symlink-aware via same_report_path)."""
    cur = os.path.abspath(path)
    while True:
        if same_report_path(cur, folder):
            return True
        parent = os.path.dirname(cur)
        if parent == cur:
            return False
        cur = parent


def _validate_json_path(json_path, disasm_dir, index_path=None, fssm_dir=None):
    """Return an error string if `json_path` cannot safely be written as --json output, else None."""
    if not json_path or not json_path.strip():
        return "--json path must not be empty"
    if "\x00" in json_path:
        return "--json path contains a NUL byte"
    if json_path.endswith(os.sep) or (os.altsep and json_path.endswith(os.altsep)) or os.path.isdir(json_path):
        return f"--json is a directory: {json_path}"
    cur = os.path.dirname(os.path.abspath(json_path))
    while cur:
        if os.path.isdir(cur):
            break
        if os.path.lexists(cur):
            return f"--json parent is not a directory: {cur}"
        nxt = os.path.dirname(cur)
        if nxt == cur:
            break
        cur = nxt
    if index_path is not None and same_report_path(json_path, index_path):
        return f"--json would overwrite --index: {index_path}"
    if _inside_dir(json_path, disasm_dir):
        return f"--json must not write inside the disasm folder: {json_path}"
    if fssm_dir is not None and _inside_dir(json_path, fssm_dir):
        return f"--json must not write inside the --fssm folder: {json_path}"
    for name in sorted(os.listdir(disasm_dir)):
        if not name.endswith(".s"):
            continue
        candidate = os.path.join(disasm_dir, name)
        if os.path.isfile(candidate) and same_report_path(json_path, candidate):
            return f"--json would overwrite input {candidate}"
    if fssm_dir is not None:
        for name in sorted(os.listdir(fssm_dir)):
            if not name.endswith(".fssm"):
                continue
            candidate = os.path.join(fssm_dir, name)
            if os.path.isfile(candidate) and same_report_path(json_path, candidate):
                return f"--json would overwrite input {candidate}"
    return None


def _write_json_atomic(path, doc):
    """Write `doc` as strict LF JSON via a staged temp file next to `path`."""
    text = json.dumps(doc, indent=1, allow_nan=False) + "\n"
    abs_path = os.path.abspath(path)
    parent = os.path.dirname(abs_path) or "."
    os.makedirs(parent, exist_ok=True)
    fd, tmp = tempfile.mkstemp(
        prefix=os.path.basename(abs_path) + ".",
        suffix=".tmp",
        dir=parent,
    )
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        os.chmod(tmp, file_mode.published_mode(abs_path))
        replace_retry.replace(tmp, abs_path)
    except BaseException:
        try:
            os.chmod(tmp, 0o600)
            os.unlink(tmp)
        except OSError:
            pass
        raise


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("disasm", help="folder of .s listings (<work>/shaders/dxbc/disasm)")
    ap.add_argument("ops", nargs="*", help="base instruction names or name patterns to report (default: all names)")
    ap.add_argument("--index", help="carve index.json: also count per stage")
    ap.add_argument("--fssm", help="folder of the sidecar .fssm headers: also count per wave size")
    ap.add_argument("--json", help="also write the rows as JSON")
    a = ap.parse_args(argv)
    for path in (a.disasm, a.fssm):
        if path is not None and not os.path.isdir(path):
            print(f"ERROR: {path} is not a folder", file=sys.stderr)
            return 2
    if a.index is not None and (not a.index.strip() or os.path.isdir(a.index)):
        print(f"ERROR: {a.index} is not a file", file=sys.stderr)
        return 2
    if a.json is not None:
        try:
            err = _validate_json_path(a.json, a.disasm, a.index, a.fssm)
        except OSError as e:
            print(f"ERROR: {e}", file=sys.stderr)
            return 2
        if err:
            print(f"ERROR: {err}", file=sys.stderr)
            return 2
    try:
        stages = stages_of(a.index) if a.index else None
        result = census(a.disasm, stages, a.fssm)
    except (OSError, ValueError) as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 2
    if not result["listings"]:
        print(f"ERROR: no .s listing in {a.disasm}", file=sys.stderr)
        return 1
    rows = report(result, a.ops)
    groups = {k: result[k] for k in ("listings_by_stage", "listings_by_wave", "listings_by_stage_wave") if result[k]}
    print(f"{result['listings']} listings" + "".join(f" {v}" for v in groups.values()) +
          f", {result['after_last_endpgm']} decoded instructions after the last s_endpgm left out")
    for row in rows:
        split = row.get("by_stage_wave") or row.get("by_stage") or row.get("by_wave")
        detail = f"  {split}" if (stages is not None or a.fssm) and split else ""
        print(f"{row['op']:32s} {row['shaders']:7d} shaders {row['instructions']:9d} instructions{detail}")
    if a.json is not None:
        try:
            _write_json_atomic(a.json, {"listings": result["listings"], **groups,
                                        "after_last_endpgm": result["after_last_endpgm"], "rows": rows})
        except (OSError, ValueError) as e:
            msg = getattr(e, "strerror", None) or str(e)
            print(f"ERROR: cannot write {a.json}: {msg}", file=sys.stderr)
            return 2
    return 0


if __name__ == "__main__":
    try:  # a redirected stdout is strict ANSI code page under -I; write '?' for what it cannot encode
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError, OSError):
        pass
    sys.exit(main())
