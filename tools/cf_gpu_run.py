r"""FUZZ-8 on a real GPU: run tools/cf_diff.py's control-flow programs through Vulkan and compare every lane with cf_ref.

tools/cf_diff.py judges the translator against two CPU models (tools/cf_ref.py, the ISA model, and the SPIR-V wave
interpreter). Neither shows what a driver does with the translated control flow: how it reconverges after a divergent
if/else, a per-lane loop with a break mask or a saveexec idiom. This tool takes the same generated programs, turns each
into a one-wave compute program that stores v8..v11 of every lane, runs it through tools/vk_fuzz_run.py (translator,
runtime/build/vk_compute_test) and compares each lane's four words with cf_ref's export for that lane.

    python -I tools/cf_gpu_run.py --programs 250 --seeds 1,2,3 --depths 2,3,4,5 --wave both

LEONIDA_VK_DEVICE picks the GPU (a substring of its name). A wave64 program on a 32-wide GPU needs the translator's
emulation: set LEONIDA_W64_EMULATE=1, otherwise those programs are reported as skipped with the reason. Programs come
with `; encoding:` byte counts from cf_diff, so llvm-mc is not needed. Before the campaign the tool runs a control: a
program whose reference is deliberately wrong (a branch sense flipped) must differ from the GPU's result, and the correct
reference must match it; if either fails the campaign does not start, because it could not see a mismatch.

Exit status: 0 no mismatch, 1 a mismatch (the minimised program is printed and kept in summary.json), 2 nothing ran
(no GPU, or every program skipped), 3 the control failed. A skipped program is never a pass: its reason is counted.
Writes summary.json under --logs (a temporary folder by default).
"""
import argparse
import collections
import json
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True
import cf_diff  # noqa: E402
import cf_ref  # noqa: E402
import cpu_ref  # noqa: E402
import vk_fuzz_run  # noqa: E402

FILL = 0xEEEEEEEE      # what vk_compute_test leaves in a word no lane wrote
OBS = ("v8", "v9", "v10", "v11")


def compute_listing(body, wave, name):
    """cf_diff's program as a one-wave compute shader: its pixel export becomes a per-lane store of v8..v11.

    The branch offsets are the ones cf_diff computed from the instruction sizes, so the instructions kept from
    cf_diff.build() are not touched; only the prologue (EXEC all ones) and the epilogue (the stores) are new."""
    original = cf_diff.build(body, wave)
    if not (original[-2][0].startswith("exp ") and original[-1][0] == "s_endpgm"):
        raise ValueError("cf_diff.build() no longer ends with exp + s_endpgm")
    program = [("s_mov_b64 exec, -1" if wave == 64 else "s_mov_b32 exec_lo, -1", 4)]
    program += original[:-2]
    program.append(("v_lshlrev_b32 v21, 4, v0", 4))
    for i, reg in enumerate(OBS):
        offset = f" offset:{i * 4}" if i else ""
        program.append((f"buffer_store_dword {reg}, v21, s[0:3], 0 offen{offset}", 8))
    program.append(("s_endpgm", 4))
    header = f"; {name}\n; group: {wave},1,1\n; rsrc2: 0x8088\n"
    if wave == 64:
        header += "; mattr: +wavefrontsize64,-wavefrontsize32\n"
    return header + cf_diff.listing_text(program, name)


def reference(body, wave, name, parent):
    """cf_ref's per-lane (v8, v9, v10, v11); a lane that never exported keeps FILL, as on the GPU."""
    listing = cf_diff.listing_text(cf_diff.build(body, wave), name)
    with tempfile.TemporaryDirectory(prefix="ref_", dir=parent) as tmp:
        path = os.path.join(tmp, name + ".s")
        with open(path, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(listing)
        exports = cf_ref.run(path, wave=wave)["exports"]
    return [list(exports.get(lane, (FILL,) * 4)) for lane in range(wave)]


def first_difference(want, got):
    """The first (lane, register) where the two disagree, or None."""
    for lane, (expected, actual) in enumerate(zip(want, got)):
        for word, (a, b) in enumerate(zip(expected, actual)):
            if a != b:
                return {"lane": lane, "register": OBS[word], "expected": f"0x{a:08x}", "actual": f"0x{b:08x}"}
    return None


def shapes(body):
    """The control-flow shapes a program uses (cf_diff's node kinds), 'plain' when it has none."""
    found = set()

    def visit(nodes):
        for node in nodes:
            if node[0] not in ("op", "probe", "cnd"):
                found.add(node[0])
            for item in node[1:]:
                if isinstance(item, list):
                    visit(item)
    visit(body)
    return found or {"plain"}


def gpu_batch(cases, wave, build, logs):
    """One result per case: a list of `wave` lists of 4 words, or a string saying why there is none."""
    texts = [compute_listing(case[3], wave, f"cs_vkfuzz_{i}") for i, case in enumerate(cases)]
    return vk_fuzz_run.run_programs(texts, words=4, wave=wave, build=build, work_parent=logs,
                                    per_lane=True, encoded=True)


def minimize(body, wave, build, logs):
    """Greedy reduction with cf_diff's variants, keeping any program that still differs from cf_ref on the GPU."""
    changed = True
    while changed:
        changed = False
        for candidate in cf_diff._variants(body):
            try:
                want = reference(candidate, wave, "ps_cf_min", logs)
                got = gpu_batch([(0, 0, 0, candidate, want, shapes(candidate))], wave, build, logs)[0]
            except (cpu_ref.CpuRefError, ValueError):
                continue
            if isinstance(got, list) and first_difference(want, got):
                body, changed = candidate, True
                break
    return body


def control(build, logs):
    """The harness must see a mismatch: the correct reference matches the GPU, one with a flipped branch does not."""
    body = [("if", ("lt", "u32", 16, 0), True, [("probe", 5)]), ("probe", 6)]
    wrong = [("if", ("ge", "u32", 16, 0), True, [("probe", 5)]), ("probe", 6)]
    want = reference(body, 32, "ps_cf_control", logs)
    wrong_want = reference(wrong, 32, "ps_cf_control_wrong", logs)
    actual = gpu_batch([(0, 2, 0, body, want, shapes(body))], 32, build, logs)[0]
    if isinstance(actual, str):
        return {"ok": False, "error": actual}
    caught = first_difference(wrong_want, actual)
    return {"ok": first_difference(want, actual) is None and caught is not None,
            "correct_reference_matches": first_difference(want, actual) is None,
            "wrong_branch_caught": caught is not None, "first_wrong_branch_difference": caught}


def new_row():
    return {"generated": 0, "ran": 0, "mismatched": 0, "skipped": collections.Counter()}


def write_summary(summary, logs):
    path = os.path.join(logs, "summary.json")
    with open(path, "w", encoding="utf-8", newline="\n") as stream:
        json.dump(summary, stream, indent=2, sort_keys=True)
        stream.write("\n")
    return path


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--programs", type=int, default=20, help="programs per seed, depth and wave (default 20)")
    parser.add_argument("--start", type=int, default=0, help="first program index")
    parser.add_argument("--seeds", default="1", help="comma-separated seeds")
    parser.add_argument("--depths", default="2,3,4,5", help="comma-separated nesting depths")
    parser.add_argument("--wave", choices=("32", "64", "both"), default="32")
    parser.add_argument("--batch", type=int, default=16, help="programs per vk_compute_test run")
    parser.add_argument("--build", default=os.path.join(os.path.dirname(HERE), "runtime", "build"),
                        help="CMake build folder with vk_compute_test (default runtime/build)")
    parser.add_argument("--logs", help="folder for summary.json and the work files (default: a temporary one)")
    parser.add_argument("--no-minimize", action="store_true", help="print a mismatch without reducing it first")
    parser.add_argument("--control-only", action="store_true", help="run the control and stop")
    args = parser.parse_args(argv)
    try:
        seeds = [int(s) for s in args.seeds.split(",")]
        depths = [int(s) for s in args.depths.split(",")]
    except ValueError:
        parser.error("--seeds and --depths take comma-separated integers")
    if args.programs < 1 or args.batch < 1 or args.start < 0 or not depths or min(depths) < 1:
        parser.error("--programs and --batch must be positive, --start not negative, every depth at least 1")
    waves = (32, 64) if args.wave == "both" else (int(args.wave),)
    logs = args.logs or tempfile.mkdtemp(prefix="cf_gpu_")
    os.makedirs(logs, exist_ok=True)

    check = control(args.build, logs)
    print(f"control: {check}", flush=True)
    if not check["ok"]:
        print("control failed: this run could not see a mismatch, so it does not start the campaign", file=sys.stderr)
        return 3
    if args.control_only:
        return 0

    summary = {"generated": 0, "ran": 0, "mismatched": 0, "skipped": collections.Counter(), "by_shape": {},
               "control": check, "mismatches": []}
    pending = {wave: [] for wave in waves}

    def row(kind):
        return summary["by_shape"].setdefault(kind, new_row())

    def process(wave):
        cases = pending[wave]
        if not cases:
            return
        got = gpu_batch(cases, wave, args.build, logs)
        for (seed, depth, index, body, want, kinds), actual in zip(cases, got):
            rows = [row(k) for k in kinds]
            if isinstance(actual, str):
                summary["skipped"][actual] += 1
                for r in rows:
                    r["skipped"][actual] += 1
                continue
            summary["ran"] += 1
            for r in rows:
                r["ran"] += 1
            diff = first_difference(want, actual)
            if diff is None:
                continue
            summary["mismatched"] += 1
            for r in rows:
                r["mismatched"] += 1
            reduced = body if args.no_minimize else minimize(body, wave, args.build, logs)
            summary["mismatches"].append({
                "seed": seed, "depth": depth, "index": index, "wave": wave, "difference": diff,
                "minimal_program": compute_listing(reduced, wave, "cs_cf_repro"),
                "cpu_interpreter": cf_diff.run_case("ps_cf_repro", reduced, wave)})
            print(f"MISMATCH seed={seed} depth={depth} index={index} wave={wave}: {diff}", flush=True)
        cases.clear()

    for wave in waves:
        for seed in seeds:
            for depth in depths:
                for index in range(args.start, args.start + args.programs):
                    body = cf_diff.program_for(seed, index, wave, depth)
                    kinds = shapes(body)
                    summary["generated"] += 1
                    for kind in kinds:
                        row(kind)["generated"] += 1
                    try:
                        want = reference(body, wave, f"ps_cf_{index}", logs)
                    except cpu_ref.CpuRefError as error:
                        why = f"cf_ref: {error}"
                        summary["skipped"][why] += 1
                        for kind in kinds:
                            row(kind)["skipped"][why] += 1
                        continue
                    pending[wave].append((seed, depth, index, body, want, kinds))
                    if len(pending[wave]) >= args.batch:
                        process(wave)
                process(wave)
                print(f"wave={wave} seed={seed} depth={depth}: generated={summary['generated']} "
                      f"ran={summary['ran']} mismatch={summary['mismatched']} "
                      f"skipped={sum(summary['skipped'].values())}", flush=True)
    path = write_summary(summary, logs)
    print(f"summary: {path}")
    print(f"{'shape':10s} {'generated':>9s} {'ran':>7s} {'mismatch':>8s} {'skipped':>7s}")
    for kind, r in sorted(summary["by_shape"].items()):
        print(f"{kind:10s} {r['generated']:9d} {r['ran']:7d} {r['mismatched']:8d} {sum(r['skipped'].values()):7d}")
    for why, count in sorted(summary["skipped"].items()):
        print(f"skipped x{count}: {why}")
    print(f"total: generated={summary['generated']} ran={summary['ran']} mismatched={summary['mismatched']} "
          f"skipped={sum(summary['skipped'].values())}", flush=True)
    return 1 if summary["mismatched"] else 2 if summary["ran"] == 0 else 0


if __name__ == "__main__":
    sys.stdout.reconfigure(errors="replace")
    raise SystemExit(main())
