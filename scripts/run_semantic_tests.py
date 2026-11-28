r"""Run the translator regression suite (tests/semantic) one process per test file, several files at a time.

    python -I scripts/run_semantic_tests.py tests/semantic          # one file per core
    python -I scripts/run_semantic_tests.py tests/semantic -p 'test_image_*.py'  # a matching subset
    python -I scripts/run_semantic_tests.py tests/semantic -j 4     # four at a time
    python -I scripts/run_semantic_tests.py tests/semantic -v       # also print the output of the files that pass
    python -I scripts/run_semantic_tests.py tests/semantic --timeout 300  # optional subprocess time limit

`-p` / `--pattern` selects test filenames using unittest discover's glob syntax (default `test*.py`). Quote the
pattern so the shell does not expand it. Only Python module filenames are selected, including inside packages;
the pattern matches their basenames, not directory paths. Package `__init__.py` modules are always included,
because discover imports them and runs any TestCase they define regardless of the pattern. No matching files is an
error in serial and parallel runs.

It runs the files `python -m unittest discover -s tests/semantic` finds (`test*.py` by default, and packages below it), each in its own process from that folder, so a file sees the same import path as under discover.

The files can run side by side because each one writes only into its own temporary folder; a new test must do the same. With -j 1,
or on a single core, it runs that discover command itself. Prints the output of every failing file, headed by its
exit code (a POSIX signal by name, a known Windows NTSTATUS with its meaning, e.g. 0xC0000005 access violation,
since a file that crashes prints nothing), and one summary line, and exits 1 if a file fails, none is found, or no
test ran. Standard library only.

The test count and verdict come from unittest's result, not from printed summaries. A child exiting with status 0
without a completed result, or after unittest reported failure, fails the run.

`--timeout SECONDS` limits each module subprocess in parallel mode, or the entire discover subprocess with
`-j 1`. The value must be positive and at most 86400 seconds (24 hours), keeping it within platform wait limits.
A timeout fails the run and reports any output already produced; other parallel modules keep running.
The default has no time limit. This stops the direct test subprocess, not any descendants it may have started.
"""
import argparse
import concurrent.futures
import fnmatch
import json
import locale
import math
import os
import re
import signal
import subprocess
import sys
import tempfile
import threading
import time

MAX_TIMEOUT_SECONDS = 24 * 60 * 60

# The child processes running right now: on SIGINT / Ctrl+C, the main thread terminates them so no orphans linger.
_active_processes = set()
_processes_lock = threading.Lock()


def _register_process(proc):
    with _processes_lock:
        _active_processes.add(proc)


def _unregister_process(proc):
    with _processes_lock:
        _active_processes.discard(proc)


def _kill_process_tree(proc):
    if proc is None:
        return
    if os.name == "nt" and proc.poll() is None:
        try:
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except OSError:
            pass
    try:
        proc.kill()
    except OSError:
        pass


def _terminate_active_processes():
    with _processes_lock:
        procs = list(_active_processes)
    if os.name != "nt":
        for p in procs:
            try:
                p.terminate()
            except OSError:
                pass
    # On Windows taskkill /T needs the root process alive to find its descendants.
    for p in procs:
        _kill_process_tree(p)
        try:
            p.wait(timeout=0.2)
        except (subprocess.TimeoutExpired, OSError):
            pass

# The child runs the module through unittest's own program and writes the result to a file, so the count of tests
# comes from the result object and nothing the module writes can change it. The exit codes are unittest's own: 1
# when a test failed, 5 when nothing ran at all, 0 otherwise. The module is left unset, as `python -m unittest` leaves it, so
# the name is imported and not looked up as an attribute of the program itself.
CHILD_PROGRAM = (
    "import json, os, sys, unittest\n"
    "program = unittest.TestProgram(module=None, argv=['unittest'] + sys.argv[1:], exit=False)\n"
    "result = program.result\n"
    "code = 1 if not result.wasSuccessful() else 5 if not result.testsRun and not result.skipped else 0\n"
    "with open(os.environ['LEONIDA_TEST_COUNT'], 'w', encoding='utf-8', newline='\\n') as handle:\n"
    "    json.dump({'testsRun': result.testsRun, 'exitCode': code}, handle)\n"
    "sys.exit(code)\n")

# Windows NTSTATUS codes python.exe exits with when it crashes. A file that dies this way prints nothing, so the code
# is the only clue there is (an access violation in a pure-Python test points at the machine, not at the test).
NTSTATUS_CRASHES = {
    0xC0000005: "access violation",
    0xC000001D: "illegal instruction",
    0xC0000017: "out of memory",
    0xC00000FD: "stack overflow",
    0xC0000374: "heap corruption",
    0xC0000409: "fail fast / stack buffer overrun",
}
# Known statuses that end a process without it crashing.
NTSTATUS_OTHER = {
    0xC000013A: "interrupted with Ctrl+C",
}


def describe_exit(code):
    """A failed module's exit code in words: `exit code 1`, `killed by SIGSEGV (-11)` for a POSIX signal, or for a
    known Windows NTSTATUS its hexadecimal value and meaning (3221225477 -> 0xC0000005, access violation). Only the
    statuses in NTSTATUS_CRASHES are called a crash: a process can also exit with any 32-bit value on purpose, so an
    unknown one is reported as just its value."""
    if code < 0:
        try:
            return f"killed by {signal.Signals(-code).name} ({code})"
        except ValueError:
            return f"killed by signal {-code} ({code})"
    if code in NTSTATUS_CRASHES:
        return f"exit code 0x{code:08X}, {NTSTATUS_CRASHES[code]} (the process crashed)"
    if code in NTSTATUS_OTHER:
        return f"exit code 0x{code:08X}, {NTSTATUS_OTHER[code]}"
    if code > 0xFFFF:  # an NTSTATUS-sized value reads better in hexadecimal
        return f"exit code 0x{code:08X}"
    return f"exit code {code}"


def module_path(directory, module):
    """Source path of a dotted module name, including a package's `__init__.py`."""
    parts = module.split(".")
    if parts[-1] == "__init__":
        return os.path.join(directory, *parts[:-1], "__init__.py")
    return os.path.join(directory, *parts) + ".py"


def test_modules(directory, pattern="test*.py"):
    """Dotted names of the test modules unittest discover finds under `directory` (it enters packages only)."""
    names = []
    for base, dirs, files in os.walk(directory):
        dirs[:] = sorted(d for d in dirs if os.path.isfile(os.path.join(base, d, "__init__.py")))
        prefix = os.path.relpath(base, directory).replace(os.sep, ".")
        # Discover imports every package below its start folder even when no filename in it matches the pattern,
        # and loads TestCase classes defined in the package initializer. Parallel runs must see those too.
        if prefix != "." and "__init__.py" in files:
            init = f"{prefix}.__init__"
            if init not in names:
                names.append(init)
        for name in sorted(files):
            if re.fullmatch(r"[_a-z]\w*\.py", name, re.IGNORECASE) and fnmatch.fnmatch(name, pattern):
                module = name[:-3] if prefix == "." else f"{prefix}.{name[:-3]}"
                if module not in names:
                    names.append(module)
    return names


def positive_seconds(value):
    seconds = float(value)
    if not math.isfinite(seconds) or not 0 < seconds <= MAX_TIMEOUT_SECONDS:
        raise argparse.ArgumentTypeError(
            f"timeout must be a positive finite number of seconds no greater than {MAX_TIMEOUT_SECONDS} (24 hours)")
    return seconds


def read_result(path, process_code):
    """Return the effective exit code, completed test count, and any diagnostic."""
    try:
        with open(path, encoding="utf-8") as handle:
            result = json.load(handle)
        count, test_code = result["testsRun"], result["exitCode"]
        if type(count) is not int or count < 0 or type(test_code) is not int or test_code not in (0, 1, 5):
            raise ValueError("invalid unittest result")
    except (OSError, ValueError, KeyError, TypeError):
        if process_code == 0:
            return 1, 0, "process exited without a unittest result"
        return process_code, None, ""
    if process_code in (0, 5) and process_code != test_code:
        return 1, count, f"process exited with status {process_code} but unittest reported {test_code}"
    return process_code, count, ""


def result_environment(scratch):
    result_path = os.path.join(scratch, "count.json")
    return result_path, dict(os.environ, LEONIDA_TEST_COUNT=result_path)


def run_discover(directory, pattern, verbose, timeout):
    args = ["discover", "-s", directory, "-p", pattern, *(["-v"] if verbose else [])]
    with tempfile.TemporaryDirectory() as scratch:
        path, env = result_environment(scratch)
        proc = subprocess.Popen([sys.executable, "-c", CHILD_PROGRAM, *args], env=env)
        _register_process(proc)
        start_time = time.monotonic()
        try:
            while True:
                try:
                    code = proc.wait(timeout=0.2)
                    break
                except subprocess.TimeoutExpired:
                    if timeout is not None and time.monotonic() - start_time >= timeout:
                        raise
        except subprocess.TimeoutExpired:
            _kill_process_tree(proc)
            print(f"TIMED OUT after {timeout:g} s: unittest discover in {directory}", file=sys.stderr)
            return 1
        except KeyboardInterrupt:
            _kill_process_tree(proc)
            try:
                proc.wait(timeout=0.5)
            except (subprocess.TimeoutExpired, OSError):
                pass
            print("interrupted", file=sys.stderr)
            return 130
        except BaseException:
            _kill_process_tree(proc)
            raise
        finally:
            _unregister_process(proc)
        code, _, diagnostic = read_result(path, code)
    if diagnostic:
        print(f"unittest discover in {directory}: {diagnostic}", file=sys.stderr)
    return code


def run_module(directory, module, verbose, timeout=None):
    """(return code, tests run, output) of `python -m unittest <target>` run from `directory`."""
    # `python -m unittest pkg.__init__` imports pkg and then runs the same file again as pkg.__init__, so a
    # registration in the initializer happens twice. Running the package itself imports it once; module_path() is
    # still the initializer's path, so the jobs stay sorted by the file they run.
    target = module[:-len(".__init__")] if module.endswith(".__init__") else module
    cmd = [sys.executable, "-c", CHILD_PROGRAM, *(["-v"] if verbose else []), target]
    with tempfile.TemporaryDirectory() as scratch:
        result_path, env = result_environment(scratch)
        proc = subprocess.Popen(cmd, cwd=directory, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True, errors="replace")
        _register_process(proc)
        try:
            stdout, stderr = proc.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            _kill_process_tree(proc)
            stdout, stderr = proc.communicate()
            # TimeoutExpired retains bytes even when text=True.
            parts = [chunk.decode(locale.getpreferredencoding(False), errors="replace") if isinstance(chunk, bytes)
                     else chunk for chunk in (stdout, stderr) if chunk]
            out = "".join(parts)
            return 1, 0, out + f"\nTIMED OUT after {timeout:g} s: {module}\n"
        except BaseException:
            _kill_process_tree(proc)
            try:
                proc.wait(timeout=0.5)
            except (subprocess.TimeoutExpired, OSError):
                pass
            raise
        finally:
            _unregister_process(proc)
        code, count, diagnostic = read_result(result_path, proc.returncode)
    out = stdout + stderr
    if diagnostic:
        out += f"\n{diagnostic}\n"
    # A package initializer that completed with no tests isn't a failed module.
    if code == 5 and count == 0 and module.endswith(".__init__"):
        return 0, 0, out
    return code, count or 0, out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("directory", help="the suite's folder, tests/semantic")
    ap.add_argument("-p", "--pattern", default="test*.py", help="test filename glob (default: test*.py)")
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 1, help="files at a time (default: cores)")
    ap.add_argument("-v", "--verbose", action="store_true", help="unittest -v, and print passing files' output")
    ap.add_argument("--timeout", type=positive_seconds, default=None, metavar="SECONDS",
                    help=f"positive finite time limit up to {MAX_TIMEOUT_SECONDS} seconds (24 hours) per module "
                         "subprocess, or for the whole discover subprocess with -j 1 (default: no limit)")
    a = ap.parse_args(argv)
    directory = os.path.abspath(a.directory)
    modules = test_modules(directory, a.pattern)
    if not modules:
        print(f"no {a.pattern} files in {directory}")
        return 1
    if a.jobs <= 1:
        return run_discover(directory, a.pattern, a.verbose, a.timeout)
    # Largest files first: they tend to run longest, and one started last would set the wall time alone.
    modules.sort(key=lambda m: -os.path.getsize(module_path(directory, m)))
    start, total, failed = time.monotonic(), 0, []
    pool = concurrent.futures.ThreadPoolExecutor(a.jobs)
    jobs = {}
    try:
        for m in modules:
            jobs[pool.submit(run_module, directory, m, a.verbose, a.timeout)] = m
        pending = set(jobs)
        while pending:
            done, _ = concurrent.futures.wait(pending, timeout=0.2, return_when=concurrent.futures.FIRST_COMPLETED)
            for job in done:
                pending.remove(job)
                code, ran, out = job.result()
                total += ran
                if code:
                    failed.append(jobs[job])
                    silent = "" if out.strip() else "(no output: the process ended before printing anything)\n"
                    print(f"==== FAILED: {jobs[job]} ({describe_exit(code)})\n{silent}{out}", flush=True)
                elif a.verbose:
                    print(f"==== {jobs[job]}\n{out}", flush=True)
    except KeyboardInterrupt:
        for job in jobs:
            job.cancel()
        try:
            pool.shutdown(wait=False, cancel_futures=True)
        except TypeError:
            pool.shutdown(wait=False)
        _terminate_active_processes()
        print("interrupted", file=sys.stderr)
        return 130
    except BaseException:
        for job in jobs:
            job.cancel()
        try:
            pool.shutdown(wait=False, cancel_futures=True)
        except TypeError:
            pool.shutdown(wait=False)
        _terminate_active_processes()
        raise
    else:
        pool.shutdown(wait=True)
    took = time.monotonic() - start
    summary = f"{len(modules)} files, {total} tests, {len(failed)} files failed in {took:.0f} s with {a.jobs} jobs"
    print(summary + (": " + ", ".join(sorted(failed)) if failed else ""))
    if total == 0:
        # An empty package initializer is a job that legitimately runs no tests; a run where every job did is not a
        # pass (discover, which -j 1 forwards to, exits 5 for it).
        print(f"no tests ran in {directory}")
        return 1
    return 1 if failed else 0


if __name__ == "__main__":
    try:  # a redirected stdout is strict ANSI code page under -I; write '?' for what it cannot encode
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError, OSError):
        pass
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        sys.exit(130)
