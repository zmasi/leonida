#!/usr/bin/env python3
"""Throughput of the translator (tools/rdna2spirv.py): microseconds per instruction, so a slowdown is visible.

    python -I scripts/bench_translate.py                 # fixtures + synthetic programs, best of 3
    python -I scripts/bench_translate.py --json          # the same numbers as JSON
    python -I scripts/bench_translate.py --runs 5 --max-slowdown 1.5

The repository tests what the translator *emits*; nothing measures how long it takes. A change to the parser or the
instruction table can cost a large share of the build time and still pass every test, and about 26,503 shaders are
translated per build. This script is the cheap check for that: it times `rdna2spirv.translate()` on inputs that
need no game data, no LLVM and no SPIRV-Tools:

- `corpus`: every fixture in tests/semantic/fixtures/disasm, the hand-written listings the semantic tests use.
  Pixel and vertex listings translate as they are; compute listings need the FSSM header the tests pass separately,
  so they are counted as skipped.
- `synthetic`: generated straight-line programs (ALU, s_nop) at fixed instruction counts. The ALU program is the
  instruction work, the s_nop program is mostly per-instruction overhead, and the two together separate a slower
  parser from a slower code generator.

Times are best-of-`--runs` seconds of one `translate()` call per program; medians are not used because a single
unlucky run (another process taking the CPU) is what dominates the tail. The numbers are machine-specific: compare
them with a run of the same commit, not with the table in the pull request that added this script.
"""
import argparse
import json
import os
import statistics
import struct
import subprocess
import sys
import tempfile
import time
import warnings

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.environ.get("LEONIDA_TOOLS_DIR") or os.path.join(ROOT, "tools")
FIXTURES = os.path.join(ROOT, "tests", "semantic", "fixtures", "disasm")
sys.dont_write_bytecode = True
sys.path.insert(0, TOOLS)
import rdna2spirv  # noqa: E402

# A DXBC container with zero parts declares no inputs, which is enough for a listing that reads no attribute
# (tests/semantic/support.py uses the same stub).
STUB_DXBC = b"DXBC" + bytes(24) + struct.pack("<I", 0) + bytes(4)

ALU_BODY = ("\tv_add_f32 v1, v1, 1.0\n"
            "\tv_mul_f32 v2, v1, 2.0\n"
            "\tv_cndmask_b32 v3, v1, v2, vcc\n"
            "\tv_and_b32 v4, v3, 0xff\n")
NOP_BODY = "\ts_nop 0\n"


def synthetic(name, body, instructions):
    """A whole pixel shader: comment, the repeated body, and the export the stage needs to be a shader."""
    lines = [f"; ps_{name} synthetic benchmark program\n"]
    lines.extend([body] * ((instructions + body.count("\n") - 1) // body.count("\n")))
    lines.extend(["\tv_mov_b32 v1, 1\n",
                  "\texp mrt0 v1, v1, v1, v1 done\n",
                  "\ts_waitcnt expcnt(0)\n",
                  "\ts_endpgm\n"])
    return "".join(lines).encode("utf-8")


def translate_bytes(stem, listing, runs):
    """Best of `runs` wall-clock seconds for one translate() of this listing, and the instruction count."""
    with tempfile.TemporaryDirectory() as tmp:
        os.mkdir(os.path.join(tmp, "disasm"))
        with open(os.path.join(tmp, "disasm", stem + ".s"), "wb") as f:
            f.write(listing)
        with open(os.path.join(tmp, stem + ".dxbc"), "wb") as f:
            f.write(STUB_DXBC)
        path = os.path.join(tmp, "disasm", stem + ".s")
        best = None
        for _ in range(runs):
            start = time.perf_counter()
            with warnings.catch_warnings():
                # The translator leaves its .dxbc handle to the garbage collector; that is its own business here.
                warnings.simplefilter("ignore")
                rdna2spirv.translate(path)
            elapsed = time.perf_counter() - start
            best = elapsed if best is None else min(best, elapsed)
    return best


def instruction_count(listing):
    return sum(1 for line in listing.decode("utf-8", "replace").splitlines()
               if line.startswith("\t") and line.strip())


def corpus_rows(runs):
    """Time every fixture that translates on its own, and count the ones that need an FSSM header."""
    rows, skipped = [], []
    for name in sorted(os.listdir(FIXTURES)):
        if not name.endswith(".s"):
            continue
        stem = name[:-2]
        if stem.startswith("cs_"):
            # A compute listing reads its group size and RSRC2 out of the FSSM header, which translate() takes from
            # <dir>/../fssm/<stem>.fssm; the semantic tests pass it per fixture (tests/semantic/support.py).
            skipped.append(f"{stem}: compute listing, needs a FSSM header")
            continue
        with open(os.path.join(FIXTURES, name), "rb") as f:
            listing = f.read()
        try:
            seconds = translate_bytes(stem, listing, runs)
        except Exception as error:      # a listing this run cannot translate: report it, never count it as fast
            skipped.append(f"{stem}: {type(error).__name__}: {error}")
            continue
        instructions = instruction_count(listing)
        rows.append({"name": stem, "instructions": instructions, "seconds": seconds,
                     "us_per_instruction": seconds / instructions * 1e6 if instructions else 0.0})
    return rows, skipped


def synthetic_rows(runs, size):
    rows = []
    for name, body, instructions in (("alu", ALU_BODY, size), ("nop", NOP_BODY, size)):
        listing = synthetic(name, body, instructions)
        seconds = translate_bytes("ps_" + name, listing, runs)
        count = instruction_count(listing)
        rows.append({"name": name, "instructions": count, "seconds": seconds,
                     "us_per_instruction": seconds / count * 1e6})
    return rows


def summarise(rows):
    if not rows:
        return {"programs": 0, "instructions": 0, "seconds": 0.0, "us_per_instruction": 0.0}
    instructions = sum(r["instructions"] for r in rows)
    seconds = sum(r["seconds"] for r in rows)
    return {"programs": len(rows), "instructions": instructions, "seconds": seconds,
            "us_per_instruction": seconds / instructions * 1e6 if instructions else 0.0}


# Recorded on this machine (AMD Ryzen 9 desktop, Windows 11, Python 3.12.4, commit 1cc6e3c) with --runs 3: the numbers
# to compare a later run against on the same machine. A different machine moves the absolute value; the shape does not.
BASELINE = {"corpus_us_per_instruction": 55.4, "alu_us_per_instruction": 32.3, "nop_us_per_instruction": 14.0}


def compare(reference, candidate, runs, size, rounds, max_slowdown=None):
    """Interleaved A/B over two tools/ trees: each round measures both, and the best per side is kept.

    Loading the tree is the subprocess's job (LEONIDA_TOOLS_DIR, as tests/semantic/support.py does it), so the two
    versions never share a module. Alternating the two in the same round is the point: a machine that slows down or
    speeds up between measurements moves both sides, not just one.
    """
    def measure(path):
        env = dict(os.environ, LEONIDA_TOOLS_DIR=os.path.abspath(path))
        result = subprocess.run([sys.executable, "-I", os.path.abspath(__file__), "--json",
                                 "--runs", str(runs), "--size", str(size)],
                                capture_output=True, text=True, env=env)
        if result.returncode != 0:
            raise SystemExit(f"{path}: the benchmark failed\n{result.stderr.strip()}")
        return json.loads(result.stdout)["measured"]

    best = {"reference": {}, "candidate": {}}
    measured_any = False
    for round_index in range(rounds + 1):   # the first round warms the caches and is not counted
        order = [("reference", reference), ("candidate", candidate)]
        if round_index % 2:
            order.reverse()                 # whoever runs first pays the cold file cache, so the order alternates
        for side, path in order:
            measured = measure(path)
            measured_any = measured_any or bool(measured)
            if round_index == 0:
                continue
            for key, value in measured.items():
                best[side][key] = min(best[side].get(key, value), value)
    if not measured_any:
        raise SystemExit("both trees measured nothing (an empty corpus?); nothing to compare")
    if sorted(best["reference"]) != sorted(best["candidate"]):
        only_reference = sorted(set(best["reference"]) - set(best["candidate"]))
        only_candidate = sorted(set(best["candidate"]) - set(best["reference"]))
        raise SystemExit("the two trees do not cover the same measurements"
                         + (f"; only in the reference: {', '.join(only_reference)}" if only_reference else "")
                         + (f"; only in the candidate: {', '.join(only_candidate)}" if only_candidate else "")
                         + "; a missing program would read as a ratio that looks like a win")
    rows = []
    for key in best["reference"]:
        ref = best["reference"][key]
        cand = best["candidate"].get(key, float("nan"))
        rows.append({"name": key, "reference": ref, "candidate": cand, "ratio": cand / ref if ref else None})
    print(f"{'measurement':<32} {'reference':>10} {'candidate':>10} {'ratio':>7}")
    for row in rows:
        ratio = f"{row['ratio']:>6.2f}x" if row["ratio"] is not None else "   n/a"
        print(f"{row['name']:<32} {row['reference']:>10.1f} {row['candidate']:>10.1f} {ratio}")
    print(f"\nreference {os.path.abspath(reference)}\ncandidate {os.path.abspath(candidate)}")
    print("ratio below 1.00 is the candidate being faster; rounds %d, min per side" % rounds)
    if max_slowdown is not None:
        # The limit means here what it means against a baseline: a measurement over it is a failure. The ratio is
        # already candidate/reference, so the limit applies to it directly.
        slow = [row for row in rows if row["ratio"] is not None and row["ratio"] > max_slowdown]
        if slow:
            for row in slow:
                print(f"slower than the reference by {row['ratio']:.2f}x (limit {max_slowdown:.2f}x): {row['name']}",
                      file=sys.stderr)
            return 1
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--runs", type=int, default=3, help="translate() calls per program, best one counted")
    parser.add_argument("--size", type=int, default=2000, help="instructions in each synthetic program")
    parser.add_argument("--json", action="store_true", help="print one JSON object instead of the tables")
    parser.add_argument("--max-slowdown", type=float, default=None,
                        help="exit 1 when a per-instruction time exceeds the baseline by this factor, in every output "
                             "format")
    parser.add_argument("--baseline", metavar="FILE", default=None,
                        help="compare against a baseline written by --write-baseline instead of the committed one; "
                             "the committed numbers are from one machine and mean nothing on another")
    parser.add_argument("--write-baseline", metavar="FILE", default=None,
                        help="write this run's numbers to FILE as a baseline for --baseline")
    parser.add_argument("--compare", nargs=2, metavar=("REFERENCE", "CANDIDATE"), default=None,
                        help="time two tools/ trees against each other, interleaved; prints a ratio per measurement")
    parser.add_argument("--rounds", type=int, default=3, help="rounds of --compare; the best per side is kept")
    arguments = parser.parse_args(argv)
    if arguments.rounds < 1:
        parser.error("--rounds must be at least 1")
    if arguments.baseline and arguments.write_baseline and \
            os.path.abspath(arguments.baseline) == os.path.abspath(arguments.write_baseline):
        # One file for both: a run that fails the limit would still replace the reference it failed against, and the
        # same workload would pass on the retry.
        parser.error("--baseline and --write-baseline name the same file; write to a different one")
    if arguments.compare:
        for flag, value in (("--json", arguments.json), ("--write-baseline", arguments.write_baseline),
                            ("--baseline", arguments.baseline)):
            if value:
                parser.error(f"{flag} does not apply to --compare, which times two trees and prints its own table")
        return compare(*arguments.compare, arguments.runs, arguments.size, arguments.rounds,
                       arguments.max_slowdown)
    if arguments.baseline and not os.path.isfile(arguments.baseline):
        parser.error(f"--baseline {arguments.baseline} does not exist; --write-baseline records one")
    if arguments.runs < 1:
        parser.error("--runs must be at least 1")
    if arguments.size < 1:
        parser.error("--size must be at least 1")

    corpus, skipped = corpus_rows(arguments.runs)
    synthetic_programs = synthetic_rows(arguments.runs, arguments.size)
    corpus_summary = summarise(corpus)
    synthetic_summary = summarise(synthetic_programs)

    measured = {"corpus_us_per_instruction": corpus_summary["us_per_instruction"]}
    measured.update({f"{row['name']}_us_per_instruction": row["us_per_instruction"]
                     for row in synthetic_programs})

    baseline, baseline_source = BASELINE, "committed"
    if arguments.baseline:
        with open(arguments.baseline, encoding="utf-8") as handle:
            loaded = json.load(handle)
        baseline = {key: float(value) for key, value in (loaded.get("measured") or loaded).items()}
        baseline_source = os.path.abspath(arguments.baseline)
    if arguments.write_baseline:
        with open(arguments.write_baseline, "w", encoding="utf-8", newline="\n") as handle:
            json.dump({"measured": measured, "runs": arguments.runs, "size": arguments.size}, handle,
                      indent=2, sort_keys=True)
            handle.write("\n")
        print(f"baseline written to {os.path.abspath(arguments.write_baseline)}", file=sys.stderr)

    # The limit is part of the contract of every output format: a job that collects --json must not read a success
    # code for a run whose numbers are over the limit. It is evaluated here, once, before either format prints.
    slow = {}
    if arguments.max_slowdown is not None:
        slow = {key: value / baseline[key] for key, value in measured.items()
                if baseline.get(key) and value > baseline[key] * arguments.max_slowdown}

    if arguments.json:
        print(json.dumps({
            "runs": arguments.runs,
            "corpus": {"summary": corpus_summary, "programs": corpus, "skipped": skipped},
            "synthetic": {"summary": synthetic_summary, "programs": synthetic_programs, "size": arguments.size},
            "baseline": baseline,
            "baseline_source": baseline_source,
            "measured": measured,
            "slowdown": {k: (measured[k] / baseline[k] if baseline.get(k) else None) for k in measured},
            "over_limit": sorted(slow),
            "max_slowdown": arguments.max_slowdown,
        }, indent=2, sort_keys=True))
        return 1 if slow else 0

    print(f"corpus: {corpus_summary['programs']} fixtures, {corpus_summary['instructions']} instructions, "
          f"{corpus_summary['seconds']:.3f} s, {corpus_summary['us_per_instruction']:.1f} us/instruction "
          f"({len(skipped)} skipped)")
    worst = sorted(corpus, key=lambda r: -r["us_per_instruction"])[:5]
    for row in worst:
        print(f"  {row['name']:<28} {row['instructions']:>5} instr  {row['seconds'] * 1e3:>8.2f} ms  "
              f"{row['us_per_instruction']:>6.1f} us/instr")
    print(f"\nsynthetic ({arguments.size} instructions):")
    for row in synthetic_programs:
        reference = baseline.get(row["name"] + "_us_per_instruction")
        compared = (f"({measured[row['name'] + '_us_per_instruction'] / reference:.2f}x the {baseline_source} baseline)"
                    if reference else "(no baseline for this measurement)")
        print(f"  {row['name']:<28} {row['instructions']:>5} instr  {row['seconds'] * 1e3:>8.2f} ms  "
              f"{row['us_per_instruction']:>6.1f} us/instr  {compared}")
    if skipped:
        print("\nskipped (need the FSSM header, which these runs do not pass):")
        for line in skipped:
            print("  " + line)

    if slow:
        for name, factor in sorted(slow.items()):
            print(f"slower than baseline ({baseline_source}) by {factor:.2f}x: {name}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
