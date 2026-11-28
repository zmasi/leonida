"""Compare compute heap-base reads with an independently known inline root layout. Print aggregate counts by default.

Usage: python -I tools/compute_heap_layout_census.py <carve>/disasm
       [--dispatch-log LOG ... --group GROUP] [--per-program]

Read the carve index, each compute container's SFI0 and embedded RTS0, and the listing and
launch metadata for comparable programs. An optional integration dispatch log pairs compute-layout
parameter records with compute-input program names by PSO within that log. Use one carve group
per invocation. Entries in different carves need not be unique programs.
"""
import argparse
from collections import Counter, defaultdict
import json
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import heap_index_census as heap
import shader_meta
import rootsig_census as roots

RESOURCE, SAMPLER = 0x400, 0x800  # D3D12 root-signature flags
PARAM = re.compile(r" \(s(\d+) type=(\d+) const=(\d+) slots=\d+\)")


def captured_layouts(logs, group):
    by_name = defaultdict(set)
    for path in logs:
        # Never join handles across processes, or a reused handle with ambiguous ownership.
        names, layouts = defaultdict(set), defaultdict(set)
        with path.open(encoding="utf-8-sig", errors="replace") as stream:
            for line in stream:
                if "[rt] compute-layout:" in line:
                    match = re.search(r"compute-layout: pso=(\w+) flags=([\da-fA-F]+) .*?shader-heap=\S+(.*)$", line)
                    if not match:
                        continue
                    handle, flags, tail = match.groups()
                    end, tables = 4, []
                    valid = not PARAM.sub("", tail).strip()
                    for slot, kind, count in PARAM.findall(tail):
                        width = {"0": int(count), "1": 2, "2": 1}.get(kind, 0)
                        valid &= int(slot) == end and width > 0
                        if kind == "2":
                            tables.append(end)
                        end += width
                    layouts[handle].add((int(flags, 16), end, tuple(tables)) if valid else None)
                elif "compute-input: execute=" in line and " pso=" in line:
                    m = re.search(r" name=(\S+) pso=(\w+)", line)
                    if m:
                        names[m[2]].add(m[1])
        for handle, programs in names.items():
            for program in programs:
                prefix, _, name = program.rpartition("/")
                if prefix == group:
                    by_name[name].update(layouts[handle] if len(programs) == 1 else {None})
    return by_name


def embedded_layout(payload):
    sig, error = roots.parse_rts0(payload)
    if error:
        return None
    end, tables = 4, []
    for p in sig["params"]:
        if p["type"] == "table":
            tables.append(end)
            end += 1
        else:
            end += p["count"] if p["type"] == "constants" else 2
    return sig["flags"], end, tuple(tables)


def compare(kind, layout, loads):
    flags, end, tables = layout
    if not flags & kind:
        return "incompatible flags"
    expected = end + (kind == SAMPLER and bool(flags & RESOURCE))
    if expected >= 32:
        return "no room"
    roles = {"V#", "T#"} if kind == RESOURCE else {"S#"}
    # Declared tables can use dynamic offsets too. Never use the predicted heap slot
    # to choose among multiple remaining candidates.
    candidates = set()
    for load in loads:
        if not roles.intersection(load["roles"]) or load["offset"][0] != "dynamic":
            continue
        base = re.fullmatch(r"user s(\d+)", load["base"])
        if not base or load["ptr"] != "ptr32":
            return "inconclusive"
        reg = int(base[1])
        if reg not in tables:
            candidates.add(reg)
    if len(candidates) != 1:
        return "inconclusive"
    return "match" if expected in candidates else "mismatch"


def program_loads(disasm, name):
    user = heap.cs_user_count(disasm, name)
    launch = disasm.parent / "fspx" / (name + ".json")
    if user is None and launch.is_file():
        try:
            with launch.open(encoding="utf-8-sig") as stream:
                doc = json.load(stream)
        except UnicodeDecodeError as exc:
            raise ValueError(f"{launch}: invalid UTF-8: {exc}") from exc
        except (ValueError, RecursionError) as exc:
            raise ValueError(f"{launch}: invalid JSON: {exc}") from exc
        if not isinstance(doc, dict):
            raise ValueError(f"{launch}: expected a JSON object")
        raw_user = doc.get("user_sgprs")
        if raw_user is not None:
            if type(raw_user) is not int or raw_user < 0:
                raise ValueError(f"{launch}: user_sgprs must be a nonnegative integer")
            user = raw_user
    listing = disasm / (name + ".s")
    if user is not None and listing.is_file():
        ir = heap.shader_ir.parse_file(str(listing))
        if ir["insns"] and not ir["invalid"]:
            try:
                return heap.analyse(ir["insns"], "cs", user)
            except heap.Skip:
                pass
    return []


def census(disasm, captured, per_program=False):
    result = Counter()
    for name, stage in heap.stage_index(disasm).items():
        if stage != "cs":
            continue
        result["compute"] += 1
        chunks = roots.parts((disasm.parent / (name + ".dxbc")).read_bytes())
        sfi = chunks.get(b"SFI0", b"")
        if len(sfi) < 8:
            result["missing SFI0"] += 1
            continue
        bits = struct.unpack_from("<Q", sfi)[0]
        used = (RESOURCE if bits & shader_meta.SFI0_RESOURCE_DESCRIPTOR_HEAP_INDEXING else 0) | (
            SAMPLER if bits & shader_meta.SFI0_SAMPLER_DESCRIPTOR_HEAP_INDEXING else 0)
        if not used:
            continue
        result["direct"] += 1
        result[{RESOURCE: "resource only", SAMPLER: "sampler only", RESOURCE | SAMPLER: "both"}[used]] += 1
        layouts = set(captured.get(name, ()))
        if b"RTS0" in chunks:
            layouts.add(embedded_layout(chunks[b"RTS0"]))
        kinds = [kind for kind in (RESOURCE, SAMPLER) if kind & used]
        if len(layouts) != 1 or None in layouts:
            status = "ambiguous or invalid layout" if layouts else "missing layout"
            checks = dict.fromkeys(kinds, status)
        else:
            loads = program_loads(disasm, name)
            checks = {kind: compare(kind, next(iter(layouts)), loads) for kind in kinds}
            status = next(s for s in ("incompatible flags", "no room", "mismatch", "inconclusive", "match")
                          if s in checks.values())
        result[status] += 1
        for kind, label in ((RESOURCE, "resource"), (SAMPLER, "sampler")):
            result[label + " matches"] += checks.get(kind) == "match"
        if per_program:
            print(f"{name}\tresource={checks.get(RESOURCE, 'unused')}\tsampler={checks.get(SAMPLER, 'unused')}")
    return result


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("disasm", type=Path)
    parser.add_argument("--dispatch-log", action="append", default=[], type=Path)
    parser.add_argument("--group", default="", help="program path prefix in the dispatch log")
    parser.add_argument("--per-program", action="store_true", help="also print local program names and each heap result")
    args = parser.parse_args(argv)
    try:
        counts = census(args.disasm, captured_layouts(args.dispatch_log, args.group), args.per_program)
    except (OSError, ValueError, KeyError, TypeError, RecursionError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    for key in ("compute", "missing SFI0", "direct", "resource only", "sampler only", "both",
                "match", "resource matches", "sampler matches", "mismatch", "no room", "inconclusive", "missing layout",
                "ambiguous or invalid layout", "incompatible flags"):
        print(f"{key}: {counts[key]}")
    return 0


if __name__ == "__main__":
    try:  # a redirected stdout is strict ANSI code page under -I; write '?' for what it cannot encode
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError, OSError):
        pass
    sys.exit(main())
