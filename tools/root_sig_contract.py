r"""Reconcile a root signature against the translator's user-SGPR reads (stdlib only).

A shader is identified by its index.json entry (user_sgpr_reads from
tools/shader_meta.py); the candidate root signature is a hand-written JSON
file in this tool's format (see --example), never game data.
When a 64-bit pointer read names both source SGPRs in `pair`, they must supply
the low and high DWORDs of the same root address, in that order.

Usage:
    python -I tools/root_sig_contract.py <index.json> --rootsig <sig.json>
    python -I tools/root_sig_contract.py <index.json> --rootsig <sig.json> --cluster
    python -I tools/root_sig_contract.py <index.json> --rootsig <sig.json> --model console
        (rt::rootsig::pack_stage: each shader's stage base, one dword per table; D3D-448)
    python -I tools/root_sig_contract.py --example

Exit code is 0 when every read is compatible, 1 on the first contradiction
(hard fail: a mismatch means every draw/dispatch with that pair is corrupt).
"""
import argparse
import json
import sys

# Packing order mirrors runtime/src/root_signature.{h,cpp}: constants take
# NumValues dwords (none for 0 values), root descriptors and tables take 2
# (VA lo/hi), static samplers take none. With a stage (--model console) it mirrors pack_stage():
# from the stage's base, a table takes ONE dword (its 32-bit VA), and the
# SGPRs below the base are the driver's (D3D-448).
STAGE_BASE = {"ps": 8, "vs": 8, "gs": 8, "cs": 4, "ms": 16}
KIND_TO_TYPES = {
    "const32": {"constants"},
    "ptr64_lo": {"descriptor", "table"},
    "ptr64_hi": {"descriptor", "table"},
    "ptr32_lo": {"descriptor", "table"},
    "vsharp": {"descriptor", "table"},
    "tsharp": {"descriptor", "table"},
    "ssharp": {"descriptor", "table"},
}


def pack_layout(sig, stage=None):
    """Lay out root params into 32 dwords. Returns (dwords, error).

    dwords[i] = {"param": index, "sub": sub-dword, "ptype": ptype}, indexed by SGPR.
    stage None: rt::rootsig::pack (from s0, two dwords per table). A stage from
    STAGE_BASE: rt::rootsig::pack_stage (from the stage base, one dword per table;
    dwords below the base are {"param": None, "sub": 0, "ptype": "driver"}).
    error is set (and dwords empty) on spill, cost overflow or bad input:
    refuse, never truncate.
    """
    if stage is not None and (not isinstance(stage, str) or stage not in STAGE_BASE):
        return [], f"no console base for stage {stage!r}"
    if not isinstance(sig, dict):
        return [], "root signature must be a JSON object"
    raw_params = sig.get("params", [])
    if not isinstance(raw_params, list):
        return [], "'params' must be a list"
    if any(not isinstance(p, dict) for p in raw_params):
        return [], "each root parameter must be a JSON object"
    base = STAGE_BASE[stage] if stage is not None else 0
    dwords = [{"param": None, "sub": 0, "ptype": "driver"} for _ in range(base)]
    cost = 0
    params = [dict(p) for p in raw_params]  # normalized copy; input untouched
    if len(params) > 64:
        return [], "more than 64 root parameters"
    for index, p in enumerate(params):
        ptype = p.get("type", "")
        if ptype == "constants":
            count = p.get("count", 0)
            # A count of 0 is valid, as in rt::rootsig::pack and pack_stage: the host serializer warns and
            # CreateRootSignature answers S_OK. Such a parameter takes no dwords and its neighbours pack as if it
            # were absent.
            if type(count) is not int or count < 0:
                return [], f"param {index}: constants need an integer count >= 0"
            width, cost_add = count, count
        elif ptype in ("cbv", "srv", "uav"):
            width, cost_add = 2, 2
            ptype = "descriptor"
        elif ptype == "table":
            width, cost_add = (1 if stage is not None else 2), 1
        else:
            return [], f"param {index}: unknown type {ptype!r}"
        cost += cost_add
        if cost > 64:
            return [], "exceeds the 64-dword D3D12 cost limit"
        if len(dwords) + width > 32:
            if stage is not None:
                return [], (f"param {index} does not fit in the user SGPRs from s{base} "
                            "(spill array not modelled)")
            return [], "needs more than 32 user SGPRs (spill unsupported)"
        for sub in range(width):
            dwords.append({"param": index, "sub": sub, "ptype": ptype})
    return dwords, ""


def _whole_address_copy(sgpr, slot, by_sgpr, dwords):
    """True when `sgpr` is one half of a two-dword address slot and the shader reads both halves as const32.

    A shader that builds a buffer descriptor from a root CBV copies its 64-bit VA as two plain dwords; the read
    classifier calls those const32 (the FidelityFX programs copy their root CBVs this way at s6..s13,
    tools/rootsig_census.py --disasm "whole pairs"). One half alone stays a contradiction."""
    if slot["ptype"] not in ("descriptor", "table") or slot["param"] is None:
        return False
    other = sgpr + 1 if slot["sub"] == 0 else sgpr - 1
    if not 0 <= other < len(dwords):
        return False
    pair = dwords[other]
    return (pair["param"] == slot["param"] and pair["sub"] == 1 - slot["sub"]
            and "const32" in by_sgpr.get(other, []))


def reconcile(shader_reads, dwords):
    """Check one shader's reads against the packed layout.

    Returns a list of contradiction strings (empty = compatible).
    """
    issues = []
    by_sgpr = {}
    for read in shader_reads:
        sgpr = read["sgpr"]
        if type(sgpr) is not int:  # bool is an int subclass, but not a valid SGPR index.
            issues.append(f"invalid SGPR index {sgpr!r}: expected an integer")
            continue
        kind = read["kind"]
        if kind in ("ptr64_lo", "ptr64_hi") and "pair" in read:
            # shader_meta records the source SGPRs, which need not be neighbours.
            # Checking each slot's kind alone accepts a VA spliced from two parameters.
            lo = sgpr if kind == "ptr64_lo" else sgpr - 1
            hi = lo + 1
            pair = read["pair"]
            if not (pair == [lo, hi] and all(type(i) is int for i in pair)
                    and 0 <= lo < hi < len(dwords)
                    and dwords[lo]["ptype"] in ("descriptor", "table")
                    and dwords[lo]["sub"] == 0 and dwords[hi]["sub"] == 1
                    and dwords[lo]["param"] == dwords[hi]["param"]):
                issues.append(f"s{sgpr} ({kind}): pair {pair!r} does not name both halves of one root address")
        by_sgpr.setdefault(sgpr, []).append(kind)
    for sgpr in sorted(by_sgpr):
        kinds = by_sgpr[sgpr]
        if sgpr < 0 or sgpr >= len(dwords):
            issues.append(f"s{sgpr} read but layout has {len(dwords)} dwords")
            continue
        slot = dwords[sgpr]
        if slot["ptype"] == "driver":
            # Below the stage base: the driver's block, not a root argument. Constants there are system values
            # (the corpus reads s1/s3 that way); an address there would be the spill pointer (section 7).
            for kind in kinds:
                if kind != "const32":
                    issues.append(f"s{sgpr} ({kind}) reads the driver block below the stage base as an address "
                                  "(a spill pointer?)")
            continue
        for kind in kinds:
            allowed = KIND_TO_TYPES.get(kind) if isinstance(kind, str) else None
            if allowed is None:
                issues.append(f"s{sgpr}: unknown read kind {kind!r}")
            elif kind == "const32" and _whole_address_copy(sgpr, slot, by_sgpr, dwords):
                pass  # both halves of a root view's 64-bit VA copied as data (into a V#), not loaded through
            elif slot["ptype"] not in allowed:
                issues.append(
                    f"s{sgpr} ({kind}) incompatible with param "
                    f"{slot['param']} ({slot['ptype']})")
            elif kind == "ptr32_lo":
                if slot["sub"] != 0:
                    issues.append(f"s{sgpr} (ptr32_lo) reads the high half of the address")
            elif kind == "ptr64_lo":
                if slot["sub"] != 0:
                    issues.append(f"s{sgpr} (ptr64_lo) reads the high half of the address")
                else:
                    hi = dwords[sgpr + 1] if sgpr + 1 < len(dwords) else None
                    if hi is None or hi["param"] != slot["param"] or hi["sub"] != 1:
                        issues.append(f"s{sgpr} (ptr64_lo) without paired hi dword")
            elif kind == "ptr64_hi":
                lo = dwords[sgpr - 1] if sgpr > 0 else None
                if slot["sub"] != 1 or lo is None or lo["param"] != slot["param"] \
                        or lo["sub"] != 0:
                    issues.append(f"s{sgpr} (ptr64_hi) without paired lo dword")
    return issues


def cluster(shaders):
    """Group by read-set shape, using the first shader name to break size ties."""
    groups = {}
    for name in sorted(shaders):
        reads = shaders[name].get("user_sgpr_reads", [])
        shape = tuple(sorted(
            ((r["sgpr"] if type(r["sgpr"]) is int else f"invalid:{r['sgpr']!r}",
              r["kind"] if isinstance(r["kind"], str) else f"invalid:{r['kind']!r}")
             for r in reads),
            key=lambda item: (type(item[0]) is not int, item[0], item[1])))
        groups.setdefault(shape, []).append(name)
    ranked = sorted(groups.items(), key=lambda kv: -len(kv[1]))
    return [(shape, len(names), names) for shape, names in ranked]


def _load_json(path):
    try:
        with open(path, encoding="utf-8-sig") as f:
            return json.load(f)
    except (OSError, ValueError, RecursionError) as e:
        reason = getattr(e, "strerror", None) or e
        raise ValueError(f"cannot read {path}: {reason}") from None


def main(argv=None):
    ap = argparse.ArgumentParser(description="Root-signature vs shader-reads reconciler.")
    ap.add_argument("index", nargs="?", help="spirv/index.json")
    ap.add_argument("--rootsig", default=None, help="hand-written root-signature JSON")
    ap.add_argument("--cluster", action="store_true", help="print read-set shape clustering")
    ap.add_argument("--example", action="store_true", help="print an empty hand-written sig template")
    ap.add_argument("--model", choices=("s0", "console"), default="s0",
                    help="s0: rt::rootsig::pack (default); console: rt::rootsig::pack_stage with each shader's "
                         "own stage base, one dword per table (D3D-448)")
    a = ap.parse_args(argv)
    if a.example:
        print(json.dumps({"version": "1.1", "flags": 0, "params": [
            {"type": "constants", "count": 4, "visibility": "all"},
            {"type": "cbv", "visibility": "vertex"},
            {"type": "table", "ranges": [{"kind": "srv", "count": 8}],
             "visibility": "pixel"}],
            "static_samplers": []}, indent=2))
        return 0
    if not a.index or not a.index.strip() or not a.rootsig or not a.rootsig.strip():
        ap.error("index.json and --rootsig are required (or --example)")
    try:
        index = _load_json(a.index)
        sig = _load_json(a.rootsig)
    except ValueError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 2
    if not isinstance(index, dict):
        print(f"ERROR: {a.index}: expected a JSON object of shader -> metadata entries", file=sys.stderr)
        return 2
    if any(not isinstance(entry, dict) for entry in index.values()):
        print(f"ERROR: {a.index}: each shader entry must be a JSON object", file=sys.stderr)
        return 2
    if not isinstance(sig, dict):
        print(f"ERROR: {a.rootsig}: expected a JSON object root signature", file=sys.stderr)
        return 2
    params = sig.get("params", [])
    if not isinstance(params, list):
        print(f"ERROR: {a.rootsig}: 'params' must be a JSON array", file=sys.stderr)
        return 2
    if any(not isinstance(p, dict) for p in params):
        print(f"ERROR: {a.rootsig}: each param must be a JSON object", file=sys.stderr)
        return 2
    for name in sorted(index):
        reads = index[name].get("user_sgpr_reads", [])
        if not isinstance(reads, list):
            print(f"ERROR: {a.index}: {name}: 'user_sgpr_reads' must be a JSON array", file=sys.stderr)
            return 2
        for read in reads:
            if not isinstance(read, dict) or "sgpr" not in read or "kind" not in read:
                print(f"ERROR: {a.index}: {name}: each read needs 'sgpr' and 'kind' fields",
                      file=sys.stderr)
                return 2
            if not isinstance(read["kind"], str):
                print(f"ERROR: {a.index}: {name}: each read kind must be a string",
                      file=sys.stderr)
                return 2
    if a.model == "console":
        layouts = {}
        stages = set()
        for name in sorted(index):
            stage = index[name].get("stage")
            if not isinstance(stage, str):
                print(f"ERROR: {a.index}: {name}: 'stage' must be a string for --model console",
                      file=sys.stderr)
                return 2
            stages.add(stage)
        if not stages:
            _, error = pack_layout(sig, min(STAGE_BASE, key=STAGE_BASE.get))
            if error:
                print(f"root signature refused: {error}")
                return 1
        for stage in sorted(stages):
            dwords, error = pack_layout(sig, stage)
            if error:
                print(f"root signature refused for {stage}: {error}")
                return 1
            layouts[stage] = dwords
            print(f"{stage}: packed {len(dwords) - STAGE_BASE[stage]} dwords from s{STAGE_BASE[stage]} "
                  f"({len(sig.get('params', []))} params)")
    else:
        dwords, error = pack_layout(sig)
        if error:
            print(f"root signature refused: {error}")
            return 1
        print(f"packed {len(dwords)} dwords from {len(sig.get('params', []))} params")
    bad = 0
    for name in sorted(index):
        reads = index[name].get("user_sgpr_reads", [])
        issues = reconcile(reads, layouts[index[name]["stage"]] if a.model == "console" else dwords)
        if issues:
            bad += 1
            print(f"CONTRADICTION {name}:")
            for issue in issues:
                print(f"  - {issue}")
    if a.cluster:
        print("read-set shapes (shape -> shaders):")
        for shape, count, names in cluster(index)[:20]:
            print(f"  {count:6}  {shape}  e.g. {names[0]}")
    print(f"{len(index)} shaders: {len(index) - bad} compatible, {bad} contradictory")
    return 1 if bad else 0


if __name__ == "__main__":
    try:  # a redirected stdout is strict ANSI code page under -I; write '?' for what it cannot encode
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError, OSError):
        pass
    sys.exit(main())
