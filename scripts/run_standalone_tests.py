"""Run selected standalone Python test files, one subprocess per file.

Read a newline-delimited list of paths from stdin and run each with python -I
from the caller's working directory. Selection stays with the CI workflow, so
pytest and standalone tests use the same predicate. Bounded parallelism keeps
each file's output together and still reports every failure at the end.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import subprocess
import sys


def positive_int(value):
    jobs = int(value)
    if jobs < 1:
        raise argparse.ArgumentTypeError("jobs must be positive")
    return jobs


def run_file(path):
    try:
        # A test (or one of its children) must not read the stream of selected paths.
        result = subprocess.run([sys.executable, "-I", path], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, errors="replace")
    except OSError as exc:
        return 1, f"Could not start test: {exc}\n"
    return result.returncode, result.stdout


def run_tests(paths, jobs):
    if not paths:
        print("No standalone test files selected", file=sys.stderr)
        return 1

    failed = []
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        pending = {pool.submit(run_file, path): path for path in paths}
        for future in as_completed(pending):
            path = pending.pop(future)
            code, output = future.result()
            print(f"== {path}", flush=True)
            if output:
                print(output, end="" if output.endswith("\n") else "\n", flush=True)
            if code:
                failed.append(path)

    # Match the Windows check job's file annotations, regardless of completion order.
    for path in sorted(failed):
        print(f"::error file={path}::{path} failed")
    print(f"{len(paths)} files, {len(failed)} files failed", flush=True)
    return int(bool(failed))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-j", "--jobs", type=positive_int, default=1, help="files at a time (default: 1)")
    args = parser.parse_args(argv)
    return run_tests([path for path in sys.stdin.read().splitlines() if path], args.jobs)


if __name__ == "__main__":
    if sys.stdout.isatty():
        sys.stdout.reconfigure(errors="replace")
    else:
        # GitHub Actions reads annotations as UTF-8, including on Windows.
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    # Git Bash writes its find/printf paths as UTF-8, even on Windows.
    sys.stdin.reconfigure(encoding="utf-8")
    sys.exit(main())
