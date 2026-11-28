r"""Judge a diff fuzzer's translated programs on the local Vulkan GPU against its reference model. Standard library only.

pk16_diff.py, vint_diff.py and sdwa_diff.py run their generated programs three ways: the reference model (written from
the RDNA2 ISA pseudocode), the translator on a CPU interpreter, and, with --oracle hw,vk, the translated SPIR-V on a
Vulkan GPU judged against the words a native AMD GPU produced. The last needs an AMD GPU. This tool is the same Vulkan
run without one: the programs the fuzzer would generate for the same seed go through the translator and
vk_compute_test, and each result is compared with the fuzzer's reference model. It answers "does this GPU and driver
agree with the model on these programs" for any Vulkan device, NVIDIA and Intel included.

  python -I tools/vk_model_fuzz.py pk16_diff --programs 1500 --seed 11 --build runtime/build

--build is the folder with vk_compute_test (a Visual Studio configuration subfolder is found). llvm-mc with the AMDGPU
target is needed as for valu_fuzz.py (LLVM_MC, docs/testing.md). The programs for a seed are the ones
``pk16_diff.py --seed S --programs N`` generates, so a mismatch can be re-read with that tool.

A program the reference model declines (the fuzzer's Skip or RefError) is counted as a skip, not judged. Exit code 0:
every judged program matched, 1: a mismatch, 2: bad arguments or no vk_compute_test, 77: nothing could be judged.
"""
import argparse
import importlib
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

FUZZERS = ("pk16_diff", "vint_diff", "sdwa_diff")
# The ``uniform`` argument each fuzzer's own run_suite passes to vk_fuzz_run.judge (sdwa_diff programs differ by lane).
UNIFORM = {"pk16_diff": True, "vint_diff": True, "sdwa_diff": False}


def default_ops(module):
    """The fuzzer's own default op list: ALL_OPS, or every op of its OPS table (sdwa_diff)."""
    if hasattr(module, "ALL_OPS"):
        return list(module.ALL_OPS)
    return sorted(module.OPS)


def reference_words(module, lines):
    """The reference model's four words for one program, or a str saying why the model declines it."""
    try:
        return [int(word) & 0xFFFFFFFF for word in module.reference(lines)]
    except (module.Skip, module.RefError) as exc:
        return str(exc) or type(exc).__name__


def run(name, programs, seed, max_ops, wave, build, work, ops=None, judge=None):
    """(counts, mismatches) for ``programs`` programs of fuzzer ``name``.

    counts maps a verdict class to how many programs got it; mismatches is [(index, detail, lines)]. ``judge`` is
    vk_fuzz_run.judge unless a test passes its own."""
    import vk_fuzz_run
    module = importlib.import_module(name)
    if judge is None:
        judge = vk_fuzz_run.judge
    verdict = getattr(module, "vk_verdict", vk_fuzz_run.exact_verdict)
    rng = random.Random(seed)
    chosen = ops or default_ops(module)
    progs = [module.generate(rng, rng.randrange(1, max_ops + 1), chosen) for _ in range(programs)]
    want = [reference_words(module, lines) for lines in progs]
    # The model's declined programs are not sent to the GPU; vk_fuzz_run.judge would label them as hardware skips.
    judged = [i for i, words in enumerate(want) if not isinstance(words, str)]
    counts = {}
    for words in want:
        if isinstance(words, str):
            counts["skip: model declines: " + words] = counts.get("skip: model declines: " + words, 0) + 1
    bad = []
    if judged:
        got_counts, got_bad = judge([progs[i] for i in judged], [want[i] for i in judged], verdict, wave=wave,
                                    build=build, work_parent=work, opaque=True, uniform=UNIFORM[name])
        for klass, n in got_counts.items():
            counts[klass] = counts.get(klass, 0) + n
        bad = [(judged[i], detail, lines) for i, detail, lines in got_bad]
    return counts, bad


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("fuzzer", choices=FUZZERS)
    ap.add_argument("--programs", type=int, default=300)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--max-ops", type=int, default=6)
    ap.add_argument("--ops", help="comma-separated opcodes (default: the fuzzer's own list)")
    ap.add_argument("--wave", type=int, choices=(32, 64), default=32)
    ap.add_argument("--build", help="the folder with vk_compute_test (default: runtime/build)")
    ap.add_argument("--work", help="a folder for the temporary files")
    ap.add_argument("--max-report", type=int, default=5, help="mismatching programs to print (default 5)")
    args = ap.parse_args(argv)
    if args.programs < 1 or args.max_ops < 1 or args.max_report < 0:
        ap.error("--programs and --max-ops must be at least 1, --max-report at least 0")
    ops = None
    if args.ops is not None:
        ops = [token.strip() for token in args.ops.split(",")]
        if not all(ops):
            ap.error("--ops must be a comma-separated list of opcodes")
    try:
        counts, bad = run(args.fuzzer, args.programs, args.seed, args.max_ops, args.wave, args.build, args.work, ops)
    except FileNotFoundError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    summary = ", ".join(f"{klass} {n}" for klass, n in sorted(counts.items(), key=lambda kv: kv[0].startswith("skip")))
    print(f"{args.fuzzer} seed {args.seed}, {args.programs} programs, wave {args.wave}: {summary or 'nothing ran'}")
    for index, detail, lines in bad[:args.max_report]:
        print(f"MISMATCH program {index}: {detail}")
        print("\n".join("    " + line for line in lines))
    if len(bad) > args.max_report:
        print(f"... {len(bad) - args.max_report} more mismatching programs")
    if bad:
        return 1
    judged = sum(n for klass, n in counts.items() if not klass.startswith("skip"))
    return 0 if judged else 77


if __name__ == "__main__":
    try:  # a redirected stdout is strict ANSI code page under -I; write '?' for what it cannot encode
        sys.stdout.reconfigure(errors="replace")
    except AttributeError:
        pass
    sys.exit(main())
