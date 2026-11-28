r"""Census of the root-layout and used-resource tables in FSSM program records (stdlib only).

Each native program in an FSSM blob names two tables in its record: where the root signature's parameters arrive in
the program's user data, and which descriptors it reads (`fssm_root_layout.root_tables()`; the record layout is the one
VIctory reads, Marvisak/VIctory, GPL-3.0, crates/victory-shader/src/native.rs). docs/d3d12_vulkan.md section 3.5
has so far inferred that layout from the user SGPRs the shaders read (`tools/user_sgpr_census.py`) and calls the
graphics bases a hypothesis. This tool counts what the tables themselves say, per hardware stage:

  * base user SGPR: the lowest SGPR a root parameter arrives in, per program, and the lowest one that holds an
    address (a root descriptor, a table or a heap), which is what the SGPR census measured;
  * dwords per parameter: how many user-data dwords the entries of each root type take (a table: 1 or 2?);
  * root types and the used-resource kinds (resource type / root type);
  * spill start: in a compute program, VIctory reads the parameters from user-data dword 16 on as living in memory
    behind a 40-bit pointer in user data 0-1. The census counts the programs with such an entry, the first spilled
    dword and whether dwords 0-1 are free of parameters there;
  * the section 3.5 hypothesis: the parameters start at the stage base (ps/vs/gs s8, cs s4, ms s16; --base
    overrides), a table takes one dword, a root descriptor two, and they lie in root order. A program agrees when
    none of its register-held parameters contradicts that; the reasons for the others are counted.

It also counts what would show the reading itself to be wrong: blobs that do not parse (by reason), record offsets,
the number of entries, reserved fields that are not zero, types the reading has no name for, used resources with no
layout entry for their root parameter, parameters that would end past s31, and whether the root-layout table
follows the code and the used-resource table follows the root-layout table.

What it does NOT measure: the root signatures. A table says where a parameter arrives, not what the game passes
there; and whether a table lists every parameter of the signature or only the ones the program reads is open, so a
lowest parameter above the base does not contradict the base (it is counted apart from "at the base").

It reads the user's own files and prints counts only: no shader name, no bytes. There are no game files in this
repository; give it your work folder.

Usage:
    python -I tools/root_layout_census.py <fssm file, dxbc file, or folder of either> [--json OUT]
        (<work>/shaders/dxbc reads every tag-0x0D payload of every container; <work>/shaders/dxbc/fssm reads the
         one payload per container that fssm_root_layout.py wrote. In a folder, a .dxbc with a .fssm of the same name
         beside it is skipped.)
    python -I tools/root_layout_census.py <path> --base ps=8 vs=8 gs=8 cs=4 ms=16 --expect-agree
        (exit 1 if a blob does not parse, a program disagrees with the hypothesis, or nothing was read)

Without a path, or for a folder with no .fssm or .dxbc file, it prints "no input" and exits 0 (1 with
--expect-agree). A path that does not exist, a folder or file that cannot be read and a --json path that cannot be
written each give one ERROR line and exit 2.
"""
import argparse
import json
import os
import sys
from collections import Counter, defaultdict

# -I keeps the script directory off sys.path, so add it explicitly before importing the sibling modules.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fssm_root_layout  # noqa: E402
from report_paths import same_report_path  # noqa: E402

# docs/d3d12_vulkan.md section 3.5: the first root-argument SGPR per stage. The merged stages have no entry there.
DEFAULT_BASE = {"ps": 8, "vs": 8, "gs": 8, "cs": 4, "ms": 16}
# VIctory's reading: a compute program has 16 user SGPRs; a parameter from user-data dword 16 on is in memory.
SPILL_FROM = {"cs": 16}
SPILL_POINTER_DWORDS = 2
DESCRIPTOR, TABLE = 1, 3
ADDRESS_ROOT_TYPES = (1, 3, 5, 6)   # root descriptor, table, resource heap, sampler heap
LAST_USER_SGPR = 31


def _sgprs(counter):
    return {f"s{k}": v for k, v in sorted(counter.items())}


def _counts(counter):
    return {str(k): v for k, v in sorted(counter.items())}


class Census:
    """Counts over fssm_root_layout.root_tables() programs: add() each one, then summary()."""

    def __init__(self, base=None):
        self.base = DEFAULT_BASE if base is None else base
        self.stages = defaultdict(lambda: defaultdict(Counter))   # stage -> counter name -> key -> count
        self.reading = Counter()
        self.record_offsets = Counter()

    def add(self, program):
        stage = program["stage"]
        s = self.stages[stage]
        s["n"]["programs"] += 1
        layout, used = program["layout"], program["used"]
        if not layout:
            s["n"]["without_layout"] += 1
        self.record_offsets[program["record"]] += 1
        self.reading["layout_entries"] += len(layout)
        self.reading["used_entries"] += len(used)
        if layout:
            follows = program["layout_offset"] == program["code_offset"] + program["code_size"]
            self.reading["layout_follows_code" if follows else "layout_not_after_code"] += 1
        if layout and used:
            layout_end = program["layout_offset"] + fssm_root_layout.ROOT_ENTRY_SIZE * len(layout)
            follows = program["used_offset"] == layout_end
            self.reading["used_follows_layout" if follows else "used_not_after_layout"] += 1

        placed = set()
        for e in layout:
            name = fssm_root_layout.root_type_name(e["root_type"])
            s["root_types"][name] += 1
            s["dwords:" + name][e["user_sgprs"]] += 1
            self.reading["nonzero_reserved"] += bool(e["reserved"])
            self.reading["unknown_root_type"] += e["root_type"] not in fssm_root_layout.ROOT_TYPES
            placed.add((e["root_slot"], e["user_data"]))
        for e in used:
            s["used_kinds"][f"{fssm_root_layout.resource_type_name(e['resource_type'])}/"
                            f"{fssm_root_layout.root_type_name(e['root_type'])}"] += 1
            self.reading["nonzero_reserved"] += bool(e["reserved"])
            self.reading["unknown_root_type"] += e["root_type"] not in fssm_root_layout.ROOT_TYPES
            self.reading["unknown_resource_type"] += e["resource_type"] not in fssm_root_layout.RESOURCE_TYPES
            if e["root_type"] in (DESCRIPTOR, TABLE):
                known = (e["root_slot"], e["user_data"]) in placed
                self.reading["used_with_layout_entry" if known else "used_without_layout_entry"] += 1

        spill_from = SPILL_FROM.get(stage)
        held = [e for e in layout if e["user_sgprs"]]
        spilled = [e for e in held if spill_from is not None and e["user_data"] >= spill_from]
        registers = [e for e in held if spill_from is None or e["user_data"] < spill_from]
        if spilled:
            s["n"]["spilled_programs"] += 1
            s["spill_start"][min(e["user_data"] for e in spilled)] += 1
            s["n"]["spill_pointer_dwords_free"] += not any(e["user_data"] < SPILL_POINTER_DWORDS for e in registers)
        if not registers:
            s["n"]["no_register_parameters"] += 1
            return

        def first(e):
            return fssm_root_layout.user_sgpr(stage, e["user_data"])

        lowest = min(first(e) for e in registers)
        highest = max(first(e) + e["user_sgprs"] - 1 for e in registers)
        s["lowest_sgpr"][lowest] += 1
        s["highest_sgpr"][highest] += 1
        self.reading["programs_past_s31"] += highest > LAST_USER_SGPR
        addresses = [first(e) for e in registers if e["root_type"] in ADDRESS_ROOT_TYPES]
        if addresses:
            s["lowest_pointer_sgpr"][min(addresses)] += 1

        reasons = []
        base = self.base.get(stage)
        if base is not None:
            if lowest < base:
                reasons.append("a parameter below the base")
            else:
                s["n"]["at_base" if lowest == base else "above_base"] += 1
        if any(e["root_type"] == TABLE and e["user_sgprs"] != 1 for e in registers):
            reasons.append("a table that is not one dword")
        if any(e["root_type"] == DESCRIPTOR and e["user_sgprs"] != 2 for e in registers):
            reasons.append("a root descriptor that is not two dwords")
        slots = {}
        for e in registers:
            slots[e["root_slot"]] = min(slots.get(e["root_slot"], e["user_data"]), e["user_data"])
        order = [slots[k] for k in sorted(slots)]
        if any(b <= a for a, b in zip(order, order[1:])):
            reasons.append("parameters not in root order")
        s["n"]["disagree" if reasons else "agree"] += 1
        for reason in reasons:
            s["disagree_reasons"][reason] += 1

    def summary(self):
        stages = {}
        for stage in sorted(self.stages):
            s = self.stages[stage]
            n = s["n"]
            stages[stage] = {
                "programs": n["programs"],
                "programs_without_layout": n["without_layout"],
                "root_types": dict(sorted(s["root_types"].items())),
                "used_kinds": dict(sorted(s["used_kinds"].items())),
                "dwords_per_parameter": {k[7:]: _counts(v) for k, v in sorted(s.items()) if k.startswith("dwords:")},
                "lowest_sgpr": _sgprs(s["lowest_sgpr"]),
                "lowest_pointer_sgpr": _sgprs(s["lowest_pointer_sgpr"]),
                "highest_sgpr": _sgprs(s["highest_sgpr"]),
                "spill": {"programs": n["spilled_programs"], "start": _counts(s["spill_start"]),
                          "pointer_dwords_free": n["spill_pointer_dwords_free"]},
                "hypothesis": {"base": self.base.get(stage), "agree": n["agree"], "disagree": n["disagree"],
                               "at_base": n["at_base"], "above_base": n["above_base"],
                               "no_register_parameters": n["no_register_parameters"],
                               "disagree_reasons": dict(sorted(s["disagree_reasons"].items()))},
            }
        keys = ("layout_entries", "used_entries", "layout_follows_code", "layout_not_after_code",
                "used_follows_layout", "used_not_after_layout", "used_with_layout_entry", "used_without_layout_entry",
                "nonzero_reserved", "unknown_root_type", "unknown_resource_type", "programs_past_s31")
        reading = {k: self.reading[k] for k in keys}
        reading["record_offsets"] = {f"0x{k:x}": v for k, v in sorted(self.record_offsets.items())}
        return {"stages": stages, "reading": reading}


def census(programs, base=None):
    """programs: fssm_root_layout.root_tables() dicts; base: {stage: first root-argument SGPR} -> summary dict."""
    c = Census(base)
    for program in programs:
        c.add(program)
    return c.summary()


def read_files(files, base=None):
    """Census over files -> (summary, blobs read, {reason: blobs not read}). OSError when a file cannot be read."""
    c = Census(base)
    blobs, unread = 0, Counter()
    for path in files:
        with open(path, "rb") as f:
            data = f.read()
        try:
            found = fssm_root_layout.blobs_of(data)
        except ValueError as e:
            unread[str(e)] += 1
            continue
        if not found:
            unread["a container with no tag-0x0D payload"] += 1
        for blob in found:
            try:
                programs = fssm_root_layout.root_tables(blob)
            except fssm_root_layout.FssmLayoutError as e:
                unread[str(e)] += 1
                continue
            blobs += 1
            for program in programs:
                c.add(program)
    return c.summary(), blobs, dict(sorted(unread.items()))


def stage_sgpr(text):
    stage, _, value = text.partition("=")
    message = f"expected STAGE=SGPR with a known stage and a nonnegative SGPR: {text!r}"
    try:
        sgpr = int(value)
    except ValueError:
        raise argparse.ArgumentTypeError(message) from None
    if stage not in fssm_root_layout.SYSTEM_SGPRS or sgpr < 0:
        raise argparse.ArgumentTypeError(message)
    return stage, sgpr


def print_summary(summary, blobs, unread):
    programs = sum(s["programs"] for s in summary["stages"].values())
    print(f"{blobs} blobs, {programs} programs, blobs not read: {unread}")
    for stage, s in summary["stages"].items():
        h = s["hypothesis"]
        print(f"{stage}: {s['programs']} programs ({s['programs_without_layout']} without a root-layout table); "
              f"root types {s['root_types']}; used resources {s['used_kinds']}")
        print(f"  base: lowest user SGPR {s['lowest_sgpr']}, lowest pointer SGPR {s['lowest_pointer_sgpr']}, "
              f"highest SGPR {s['highest_sgpr']}")
        print(f"  dwords per parameter: {s['dwords_per_parameter']}")
        print(f"  spill: {s['spill']['programs']} programs, first spilled user-data dword {s['spill']['start']}, "
              f"user data 0-1 free in {s['spill']['pointer_dwords_free']}")
        print(f"  section 3.5 hypothesis (base {'s%d' % h['base'] if h['base'] is not None else 'none given'}, "
              f"one dword per table, root order): {h['agree']} agree ({h['at_base']} at the base, "
              f"{h['above_base']} above it), {h['disagree']} disagree {h['disagree_reasons']}, "
              f"{h['no_register_parameters']} with no parameter in a register")
    r = summary["reading"]
    print(f"reading checks: record offsets {r['record_offsets']}; root-layout table right after the code in "
          f"{r['layout_follows_code']} programs, elsewhere in {r['layout_not_after_code']}; used-resource table right "
          f"after it in {r['used_follows_layout']}, elsewhere in {r['used_not_after_layout']}")
    print(f"  {r['layout_entries']} root-layout and {r['used_entries']} used-resource entries; "
          f"used descriptors and tables with a root-layout entry {r['used_with_layout_entry']}, without "
          f"{r['used_without_layout_entry']}; entries with a non-zero reserved field {r['nonzero_reserved']}; unnamed "
          f"root types {r['unknown_root_type']}, resource types {r['unknown_resource_type']}; programs with a "
          f"parameter past s{LAST_USER_SGPR}: {r['programs_past_s31']}")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("path", nargs="?", help="an .fssm file, a DXBC container, or a folder of either (your own files)")
    ap.add_argument("--json", help="also write the summary here")
    ap.add_argument("--base", nargs="*", type=stage_sgpr, default=[], metavar="STAGE=SGPR",
                    help="first root-argument SGPR of a stage, instead of the section 3.5 one")
    ap.add_argument("--expect-agree", action="store_true",
                    help="exit 1 if a blob is not read, a program disagrees with the hypothesis or nothing was read")
    a = ap.parse_args(argv)
    if a.json and a.path and same_report_path(a.path, a.json):
        ap.error("--json must not overwrite the input")
    files = []
    if a.path:
        if not os.path.exists(a.path):
            print(f"ERROR: no input: {a.path} does not exist", file=sys.stderr)
            return 2
        try:
            files = fssm_root_layout.files_of(a.path)
        except OSError as e:
            print(f"ERROR: cannot list {a.path}: {e.strerror or e}", file=sys.stderr)
            return 2
    if not files:
        where = f"{a.path} holds no .fssm or .dxbc file" if a.path else "give the folder with your own unpacked " \
            ".fssm blobs or carved .dxbc containers"
        print(f"no input: {where}")
        return 1 if a.expect_agree else 0
    try:
        summary, blobs, unread = read_files(files, {**DEFAULT_BASE, **dict(a.base)})
    except OSError as e:
        print(f"ERROR: cannot read {e.filename or a.path}: {e.strerror or e}", file=sys.stderr)
        return 2
    print_summary(summary, blobs, unread)
    report = {**summary, "blobs": blobs, "blobs_not_read": unread}
    if a.json:
        try:
            with open(a.json, "w", encoding="utf-8", newline="\n") as f:
                json.dump(report, f, indent=1, sort_keys=True)
        except OSError as e:
            print(f"ERROR: cannot write {a.json}: {e.strerror or e}", file=sys.stderr)
            return 2
    if not a.expect_agree:
        return 0
    bad = [f"{count} blobs not read: {reason}" for reason, count in unread.items()]
    bad += [f"{stage}: {s['hypothesis']['disagree']} programs disagree {s['hypothesis']['disagree_reasons']}"
            for stage, s in summary["stages"].items() if s["hypothesis"]["disagree"]]
    if not blobs:
        bad.append("no blob was read")
    for b in bad:
        print("MISMATCH", b, file=sys.stderr)
    return 1 if bad else 0


if __name__ == "__main__":
    try:  # a redirected stdout is strict ANSI code page under -I; write '?' for what it cannot encode
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError, OSError):
        pass
    sys.exit(main())
