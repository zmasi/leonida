"""Count how many real graphics PSOs put a code base in two stages (docs/d3d12_vulkan.md §5.2).

    python -I tools/pso_code_base_pairs.py <work>/shaders --library <shaders.rpf> [--out pairs.json]

§5.2 leaves the push-constant shape open -- one 168-byte block per PSO or one buffer per stage -- and says to decide
after measuring how many real PSOs have two code-base stages. `docs/boot.md` row 14 asks the same thing. The
translation index gives `uses_code_base` per program (`tools/feature_scan.py` prints the per-stage counts), and its
header says the pairing is not in `shaders.rpf`. It is: the pipeline library in the archive's zlib part records, for
every graphics PSO, the program in each stage slot (`tools/shader_library.py`), so a stage of a PSO joins to that
program's own `uses_code_base`.

The join needs one step in between. A library program is not the translation index's key: the two tables name the
same code differently. The library names a program by slot and index (`lib_ps_00042`), the translation index is keyed
by the container it was translated from. What they have in common is the digest: a stage's resource-layout blob ends
with the 16-byte HASH digest of the container its code came from (`shader_library.stage_digest`), and `shader_carve.py`
leaves that same HASH part in every container it carved into `<work>/shaders/dxbc`. So

    PSO -> stage slot -> program -> layout digest -> carved container -> translation-index row

and the container's file stem is the translation index's key. Where a container was carved twice under two stems the
stage counts as code-base if any of its rows does.

Counts only are printed and written: no shader name, listing, code or data leaves the work folder. Standard library
only; needs the retail package (for `shaders.rpf`) and the `shaders` stage of `make_game.py`.
"""
import argparse
import json
import mmap
import os
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shader_library  # noqa: E402
from report_paths import same_report_path  # noqa: E402


def container_stem(path: str) -> str:
    """The translation index's key for a carved container: its file name without the folder and extension."""
    return os.path.splitext(os.path.basename(path))[0]


def read_spirv(path: str) -> dict:
    """{container stem: True/False} for every translated program in a `spirv/index.json`.

    An object maps the container stem to its row; a list of rows is the other shape `rdna2spirv.py` writes, where
    the stem comes from the row's own file or name. Rows without a stem are skipped: they cannot be joined."""
    with open(path, encoding="utf-8-sig") as f:
        idx = json.load(f)
    if isinstance(idx, dict):
        pairs = idx.items()
    elif isinstance(idx, list):
        pairs = ((row.get("file") or row.get("name") or "", row) for row in idx if isinstance(row, dict))
    else:
        pairs = ()
    out = {}
    for key, row in pairs:
        if not isinstance(row, dict) or not key:
            continue
        out[container_stem(str(key))] = row.get("uses_code_base") is True
    return out


def code_base_flags(lib: dict, digests: dict, code_base: dict) -> dict:
    """{library program index: True / False / None} -- None when no carved container of that program is in the
    translation index, so the program's code base is unknown rather than absent."""
    flags = {}
    for i in range(lib["header"]["programs"]):
        stems = {container_stem(path) for d in lib["digest"].get(i, ()) for _, path in digests.get(d, ())}
        known = {code_base[s] for s in stems if s in code_base}
        flags[i] = None if not known else True in known
    return flags


def count_records(lib: dict, flags: dict) -> dict:
    """The report: how the graphics PSOs and compute records split by the code-base flag of their stages.

    `stages` keys are stage names joined with '+', sorted, so 'ps+vs' is one pixel and one vertex stage."""
    slots, by_number, pairs, unresolved = Counter(), Counter(), Counter(), Counter()
    by_stage = Counter()
    for rec in lib["graphics"]:
        flag = {slot: flags.get(ref["program"]) for slot, ref in rec["stages"].items()}
        slots["+".join(sorted(flag))] += 1
        code_base = sorted(slot for slot, known in flag.items() if known is True)
        by_number[len(code_base)] += 1
        by_stage.update(code_base)
        if len(code_base) >= 2:
            pairs["+".join(code_base)] += 1
        missing = sorted(slot for slot, known in flag.items() if known is None)
        if missing:
            unresolved["+".join(missing)] += 1
    compute = Counter(str(flags.get(rec["program"])) for rec in lib["compute"] if rec["program"] is not None)
    programs, program_code_base = Counter(), Counter()
    for i, stages in lib["stage"].items():
        programs["+".join(sorted(stages))] += 1
        if flags.get(i) is True:
            program_code_base["+".join(sorted(stages))] += 1
    return {
        "library": lib["header"],
        "stage_slots": dict(slots),
        "psos_by_code_base_stages": {str(n): c for n, c in by_number.items()},
        "psos_with_code_base_by_stage": dict(by_stage),
        "psos_with_two_code_base_stages": dict(pairs),
        "psos_unresolved": dict(unresolved),
        "compute_records_by_code_base": dict(compute),
        "programs": dict(programs),
        "programs_with_code_base": dict(program_code_base),
    }


def print_report(report: dict, carve_errors: int) -> None:
    h = report["library"]
    print(f"library {h['name']} {h['version']} hash {h['hash']}: {h['graphics']} graphics + {h['compute']} "
          f"compute records, {h['programs']} programs")
    print("\nstage slots per graphics PSO:")
    for name, n in sorted(report["stage_slots"].items(), key=lambda x: (-x[1], x[0])):
        print(f"  {n:7d}  {name}")
    print("\ngraphics PSOs by the number of stages that use a code base:")
    for n in sorted(report["psos_by_code_base_stages"], key=int):
        print(f"  {report['psos_by_code_base_stages'][n]:7d}  {n}")
    print("\ngraphics PSOs with a code-base stage, counted per stage:")
    for name, n in sorted(report["psos_with_code_base_by_stage"].items(), key=lambda x: (-x[1], x[0])):
        print(f"  {n:7d}  {name}")
    two = report["psos_with_two_code_base_stages"]
    print("\ngraphics PSOs with two or more code-base stages, by stage:")
    for name, n in sorted(two.items(), key=lambda x: (-x[1], x[0])):
        print(f"  {n:7d}  {name}")
    if not two:
        print("        0  (none)")
    print("\nlibrary programs that use a code base, by stage (compare tools/feature_scan.py):")
    for name in sorted(report["programs_with_code_base"]):
        print(f"  {report['programs_with_code_base'][name]:7d} of {report['programs'][name]:7d}  {name}")
    unknown = report["psos_unresolved"]
    print(f"\ngraphics PSOs with a stage whose code base is unknown: {sum(unknown.values())}")
    for name, n in sorted(unknown.items(), key=lambda x: (-x[1], x[0])):
        print(f"  {n:7d}  {name}")
    if carve_errors:
        print(f"\n{carve_errors} carved containers were unreadable, above.")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("shaders", help="<work>/shaders from make_game.py")
    ap.add_argument("--library", required=True,
                    help="shaders.rpf, or a pipeline library saved by tools/shader_library.py")
    ap.add_argument("--out", help="also write the counts as JSON")
    a = ap.parse_args()

    spirv_index = os.path.join(a.shaders, "spirv", "index.json")
    dxbc_dir = os.path.join(a.shaders, "dxbc")
    for path, what in ((a.shaders, "folder"), (spirv_index, "file"), (dxbc_dir, "folder")):
        if not os.path.exists(path):
            print(f"ERROR: {path} {what} does not exist. Give the <work>/shaders folder make_game.py writes.",
                  file=sys.stderr)
            return 2
    for input_path in (spirv_index, os.path.join(dxbc_dir, "index.json")):
        if a.out and same_report_path(a.out, input_path):
            print(f"ERROR: --out {a.out} is an input ({input_path}).", file=sys.stderr)
            return 2

    with open(a.library, "rb") as f:
        head = f.read(4)
        f.seek(0)
        if head == shader_library.MAGIC:                     # a library saved by tools/shader_library.py
            raw = f.read()
        else:                                                # an archive: reassemble it from its zlib blocks
            m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
            try:
                raw = shader_library.find_library(m)
            finally:
                m.close()
    lib = shader_library.parse(raw)

    code_base = read_spirv(spirv_index)
    errors = []
    digests = shader_library.carve_digests(dxbc_dir, errors=errors)
    report = count_records(lib, code_base_flags(lib, digests, code_base))
    print_report(report, len(errors))
    if a.out:
        with open(a.out, "w", encoding="utf-8", newline="\n") as f:
            json.dump(report, f, indent=1, sort_keys=True)
            f.write("\n")
        print(f"\nwrote {a.out}")
    return 0


if __name__ == "__main__":
    sys.stdout.reconfigure(errors="replace")
    sys.exit(main())
