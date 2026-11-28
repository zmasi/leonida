"""Judge a CI test run that has known failures, each fixed by an open pull request.

    python -I scripts/ci_expected_failures.py FAILED_LIST EXIT_STATUS [EXPECTED ...]

FAILED_LIST is a UTF-8 file with one failed test name per line and an optional BOM.
EXIT_STATUS is the test runner's exit status. Each EXPECTED argument is one or more whitespace-separated
test names, so a workflow can pass one quoted list.

Exits 1 when an unexpected test failed, the runner failed without naming a test, or a signal terminated the runner.
Exits 2 on a usage error. `--help` prints this text. An expected failure absent from a completed run gets a GitHub
Actions notice so the list in the workflow can shrink.
"""

import sys


def judge(failed: list[str], status: int, expected: list[str]) -> tuple[int, list[str]]:
    """(exit status, GitHub Actions workflow commands to print)."""
    unexpected = [name for name in failed if name not in expected]
    lines = [f"::error::{name} failed" for name in unexpected]
    # subprocess reports a signal as -N; the shell pipelines in runtime.yml report it as 128+N.
    if status < 0 or 128 < status <= 255:
        lines.append(f"::error::abnormal test runner exit {status}; the failure list may be incomplete")
        return 1, lines
    if status != 0 and not failed:
        lines.append(f"::error::the test runner exited with {status} without naming a failed test")
        return 1, lines
    for name in expected:
        if name in failed:
            lines.append(f"::warning::{name} failed, as expected until its fix is merged")
        else:
            lines.append(f"::notice::{name} passed: drop it from the expected failures")
    return (1 if unexpected else 0), lines


def main(argv: list[str]) -> int:
    if argv and argv[0] in ("-h", "--help"):
        print(__doc__.strip())
        return 0
    if len(argv) < 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    try:
        status = int(argv[1])
    except ValueError:
        print(f"ci_expected_failures: EXIT_STATUS {argv[1]!r} is not an integer", file=sys.stderr)
        return 2
    try:
        with open(argv[0], encoding="utf-8-sig") as f:
            failed = [line.strip() for line in f if line.strip()]
    except (OSError, UnicodeDecodeError) as e:
        print(f"ci_expected_failures: {argv[0]}: {getattr(e, 'strerror', None) or e}", file=sys.stderr)
        return 2
    expected = [name for arg in argv[2:] for name in arg.split()]
    code, lines = judge(failed, status, expected)
    print("\n".join(lines))
    return code


if __name__ == "__main__":
    try:  # a redirected stdout is strict ANSI code page under -I; write '?' for what it cannot encode
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError, OSError):
        pass
    sys.exit(main(sys.argv[1:]))
