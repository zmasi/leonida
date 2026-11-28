"""Run the Windows build's CTest tests under Wine, on Linux.

Usage:
    python -I scripts/run_wine_tests.py --tests ctest.json --windows-root D:/a/Leonida/Leonida
        [--root DIR] [--wine CMD] [--require NAME ...] [--timeout SECONDS] [--jobs N] [--summary FILE] [--label TEXT]

--tests is the output of `ctest --show-only=json-v1` from the Windows build, and --windows-root the checkout it was
made in (forward slashes, as `cygpath -m` prints it). Paths under it are mapped to --root, the Linux checkout (default:
this repository), which must hold the Windows build's runtime/build folder at the same place.

Some tests open files through absolute paths compiled into them (the GLSL test shaders, LEONIDA_TEST_*_SPV under
runtime/build/generated). For those, the drive of --windows-root is pointed at --root in the Wine prefix for the run
($WINEPREFIX, else ~/.wine), unless that drive is already in use there.

Every test is reported. Each name given to --require must be listed and all of its tests must pass, or the
exit code is 1. The other results show what does and doesn't work under Wine yet without failing the job.
--timeout caps each process; a shorter positive CTest TIMEOUT takes precedence for that test.
Relative executable paths in --wine or WINE are read from the directory where you start this runner.

Each test gets WINEDLLOVERRIDES with winedbg.exe and winemenubuilder.exe disabled, so a crash fails at once instead of
waiting in the debugger until the timeout; an entry of the caller's or the test's for the same program wins. One
wineserver is kept alive for the whole run (`wineserver -p`: $WINESERVER, else the one next to the --wine command,
else the one on PATH) and stopped at the end, instead of a new one starting for every test.
--jobs N runs N tests at a time. Tests marked RUN_SERIAL run alone, tests sharing a RESOURCE_LOCK never overlap, and
DEPENDS and fixtures are kept; the report is in the same order as with --jobs 1 (the default).
"""
import argparse
from concurrent.futures import FIRST_COMPLETED, ThreadPoolExecutor, wait
from graphlib import CycleError, TopologicalSorter
import json
import math
import ntpath
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _make_parent(path: str) -> None:
    """Create the folder a report lives in: the run is over by the time the summary is written."""
    parent = os.path.dirname(os.path.abspath(path))
    if parent:
        os.makedirs(parent, exist_ok=True)


def norm(path: str) -> str:
    path = ntpath.normpath(path).replace("\\", "/")
    # Removing this slash would turn an absolute drive root into a drive-relative path.
    return path if re.fullmatch(r"[A-Za-z]:/", path) else path.rstrip("/")


def map_path(arg: str, windows_root: str, root: str) -> str | None:
    """The Linux path for a path under the Windows checkout, or None if it isn't one."""
    a = norm(arg)
    w = norm(windows_root)
    if a.lower() == w.lower():
        return root
    prefix = w.rstrip("/") + "/"
    if a.lower().startswith(prefix.lower()):
        return os.path.join(root, *a[len(prefix):].split("/"))
    return None


class DriveMap:
    """Points the drive of the Windows checkout at the Linux one inside the Wine prefix, for the run.

    A test that opens D:/a/Leonida/Leonida/runtime/build/generated/x.spv (a path compiled into it) then reads
    <root>/runtime/build/generated/x.spv. The drive is a dosdevices link to a scratch folder holding a/Leonida/Leonida
    as a link to the root. If the Windows checkout is the drive root itself, the drive links directly to the
    Linux checkout. Temporary links and scratch folders are removed afterwards. A drive the prefix already has
    (c:, z:, or one set up by hand) is left alone, and so is a root without an absolute drive path.
    """

    def __init__(self, windows_root: str, root: str, prefix: str):
        self.windows_root, self.root, self.prefix = norm(windows_root), os.path.abspath(root), prefix
        self.link = self.scratch = None
        self.note = ""

    def __enter__(self):
        m = re.fullmatch(r"([A-Za-z]):/(.*)", self.windows_root)
        if not m:
            self.note = f"{self.windows_root} has no drive letter: no drive mapped"
            return self
        drive, rest = m.group(1).lower(), [p for p in m.group(2).split("/") if p]
        dosdevices = os.path.join(self.prefix, "dosdevices")
        link = os.path.join(dosdevices, drive + ":")
        if not os.path.isdir(dosdevices):
            self.note = f"no Wine prefix at {self.prefix}: drive {drive}: not mapped"
            return self
        already_set = f"drive {drive}: is already set in {dosdevices}: left as it is"
        if os.path.lexists(link):
            self.note = already_set
            return self
        target = self.root
        try:
            if rest:
                self.scratch = tempfile.mkdtemp(prefix="wine-drive-")
                parent = os.path.join(self.scratch, *rest[:-1])
                os.makedirs(parent, exist_ok=True)
                os.symlink(self.root, os.path.join(parent, rest[-1]))
                target = self.scratch
            try:
                os.symlink(target, link)
            except FileExistsError:
                self.note = already_set
                return self
            self.link = link
        finally:
            # __exit__ only runs after a successful __enter__.
            if self.link is None and self.scratch is not None:
                shutil.rmtree(self.scratch, ignore_errors=True)
                self.scratch = None
        self.note = f"drive {drive}: maps {self.windows_root} to {self.root} for this run"
        return self

    def __exit__(self, *exc):
        if self.link and os.path.islink(self.link):
            os.unlink(self.link)
        if self.scratch:
            shutil.rmtree(self.scratch, ignore_errors=True)   # removes the links, never what they point to
        return False


# A crashed test would wait in winedbg until the timeout; winemenubuilder writes file associations and menu entries
# into the user's profile when a fresh prefix is set up.
DEFAULT_DLL_OVERRIDES = ("winedbg.exe=d", "winemenubuilder.exe=d")


def _override_names(entry: str) -> set[str]:
    """The programs one WINEDLLOVERRIDES entry ("a,b.exe=n") names, lower case and without .dll/.exe."""
    names = set()
    for name in entry.partition("=")[0].split(","):
        name = name.strip().lower()
        for suffix in (".dll", ".exe"):
            if name.endswith(suffix):
                name = name[:-len(suffix)]
        if name:
            names.add(name)
    return names


def merge_dll_overrides(given: str | None) -> str:
    """WINEDLLOVERRIDES with the defaults added, leaving out a default for a program the given value already names."""
    entries = [e for e in (given or "").split(";") if e.strip()]
    named = set().union(*(_override_names(e) for e in entries))
    return ";".join([d for d in DEFAULT_DLL_OVERRIDES if not _override_names(d) & named] + entries)


def find_wineserver(wine: list[str]) -> str | None:
    """The wineserver the --wine command uses: $WINESERVER, else the one next to it, else the one on PATH."""
    given = os.environ.get("WINESERVER")
    if given and shutil.which(given):
        return shutil.which(given)
    for part in wine:
        resolved = shutil.which(part)
        if resolved and os.path.basename(resolved).lower() in ("wine", "wine64"):
            for folder in (os.path.dirname(resolved), os.path.dirname(os.path.realpath(resolved))):
                sibling = shutil.which(os.path.join(folder, "wineserver"))
                if sibling:
                    return sibling
    return shutil.which("wineserver")


class KeptServer:
    """Keeps one wineserver alive for the run instead of one starting and stopping for every test.

    `wineserver -p` starts a persistent server for $WINEPREFIX; `wineserver -k` stops it and the prefix's Wine
    processes at the end, also after a failure or Ctrl+C. A server still shutting down (wineboot's lingers a few
    seconds) is waited for first; one that stays (started by hand with -p) is used and left running.
    The first Wine program a server runs starts the prefix's services, which inherit its output and live as long as
    the server: `wine wineboot` starts them with no output, or the first test would wait for the end of its output
    until the timeout.
    """

    def __init__(self, command: str | None, wine: list[str]):
        self.command, self.wine, self.started, self.note = command, wine, False, ""

    def _run(self, *flags, timeout=30.0) -> int | None:
        try:
            return subprocess.run([self.command, *flags], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL, timeout=timeout).returncode
        except (OSError, subprocess.TimeoutExpired):
            return None

    def __enter__(self):
        if not self.command:
            self.note = "no wineserver found: each test starts its own"
            return self
        code = self._run("-p")
        if code == 2:                     # one is already running for this prefix
            if self._run("-w", timeout=15.0) == 0:
                code = self._run("-p")
            else:
                self.note = "a wineserver already runs for this prefix: used and left running"
                return self
        self.started = code == 0
        if not self.started:
            self.note = f"could not start {self.command} -p: each test starts its own server"
            return self
        self.note = "wineserver kept alive for the run"
        env = dict(os.environ, WINEDLLOVERRIDES=merge_dll_overrides(os.environ.get("WINEDLLOVERRIDES")))
        try:
            subprocess.run(self.wine + ["wineboot"], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, env=env, timeout=120)
        except (OSError, subprocess.TimeoutExpired):
            pass                          # the tests start the services then, as without a kept server
        return self

    def __exit__(self, *exc):
        if self.started:
            self._run("-k")
        return False


def wine_path(path: str) -> str:
    """Wine's Z: drive is the Unix root, so a Windows program reads /x/y as Z:\\x\\y."""
    return "Z:" + os.path.abspath(path).replace("/", "\\")


def properties(test: dict) -> dict:
    return {p["name"]: p["value"] for p in test.get("properties", [])}


def fixture_dependencies(tests: list[dict]) -> dict[int, tuple[int, ...]]:
    """Find the listed setup tests that must finish before each fixture consumer."""
    metadata = [properties(test) for test in tests]
    setups: dict[str, list[int]] = {}
    for index, props in enumerate(metadata):
        for fixture in props.get("FIXTURES_SETUP", []):
            setups.setdefault(fixture, []).append(index)
    return {
        index: tuple(dict.fromkeys(
            setup
            for fixture in props.get("FIXTURES_REQUIRED", [])
            for setup in setups.get(fixture, ())
            if setup != index
        ))
        for index, props in enumerate(metadata)
    }


def truthy(value) -> bool:
    """A ctest boolean property. The JSON carries it as a string, so "0" must stay false."""
    return str(value).strip().lower() in ("1", "on", "true", "yes")


def regex_list(value: str | list[str]) -> list[str]:
    """Treat a single string as one output expression, like a one-element CTest list."""
    return [value] if isinstance(value, str) else value


def run_test(test: dict, args, wine: list[str], *, failed_setups: tuple[str, ...] = ()) -> tuple[str, str]:
    """Returns (status, detail): status is pass, fail or skip."""
    props = properties(test)
    if truthy(props.get("DISABLED")):
        return "skip", "disabled by CTest"
    command = test.get("command")
    if not command:
        return "fail", "no command (target not built?)"
    exe = map_path(command[0], args.windows_root, args.root)
    if exe is None:
        # A test that runs a tool of the Windows runner (cmake -P, python) rather than one this build made.
        return "skip", f"runs {command[0]}, not a program from the build"
    if not os.path.isfile(exe):
        return "fail", f"executable not in the artifact: {command[0]}"
    if failed_setups:
        return "fail", "failed fixture setup: " + ", ".join(failed_setups)
    argv = list(wine) + [exe]
    for a in command[1:]:
        mapped = map_path(a, args.windows_root, args.root)
        argv.append(wine_path(mapped) if mapped is not None else a)
    listed_cwd = props.get("WORKING_DIRECTORY")
    cwd = map_path(listed_cwd, args.windows_root, args.root) if listed_cwd else None
    if cwd is not None and not os.path.isdir(cwd):
        return "fail", f"CTest working directory is not a directory: {listed_cwd} (mapped to {cwd})"
    if cwd is None:
        cwd = os.path.dirname(exe)
    assignments = props.get("ENVIRONMENT", [])
    if not isinstance(assignments, list):
        return "fail", "invalid CTest ENVIRONMENT: expected a list of NAME=value entries"
    env = os.environ.copy()
    for assignment in assignments:
        if not isinstance(assignment, str):
            return "fail", "invalid CTest ENVIRONMENT entry: expected NAME=value"
        key, equals, value = assignment.partition("=")
        if not key or not equals or "\0" in assignment:
            return "fail", "invalid CTest ENVIRONMENT entry: expected NAME=value"
        env[key] = value
    env["WINEDLLOVERRIDES"] = merge_dll_overrides(env.get("WINEDLLOVERRIDES"))
    timeout = args.timeout
    if "TIMEOUT" in props:
        try:
            ctest_timeout = float(props["TIMEOUT"])
        except (TypeError, ValueError):
            return "fail", "invalid CTest TIMEOUT"
        if not math.isfinite(ctest_timeout) or ctest_timeout < 0:
            return "fail", "invalid CTest TIMEOUT"
        if ctest_timeout > 0:
            timeout = min(timeout, ctest_timeout)
    skip_code = props.get("SKIP_RETURN_CODE")
    if skip_code is not None:
        error = "invalid CTest SKIP_RETURN_CODE: expected an integer from 0 to 255"
        try:
            skip_code = int(str(skip_code))
        except ValueError:
            return "fail", error
        if not 0 <= skip_code <= 255:
            return "fail", error
    start = time.monotonic()
    try:
        proc = run_test_process(argv, cwd=cwd, env=env, timeout=timeout)
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b"").decode("utf-8", "replace")
        return "fail", f"timed out after {timeout} s\n{out}"
    except OSError as e:                  # a mistyped --wine, or a missing one: report the test, do not abort the run
        return "fail", f"could not start {argv[0]}: {e.strerror or e}"
    elapsed = time.monotonic() - start
    out = proc.stdout.decode("utf-8", "replace")
    code = proc.returncode
    if code < 0:
        # A negative return code is a host-signal termination (e.g. SIGSEGV), not a process exit code:
        # report it as a failure before SKIP_RETURN_CODE or WILL_FAIL can reinterpret it.
        return "fail", f"killed by signal {-code} in {elapsed:.1f} s\n{out}"
    if code == skip_code:
        return "skip", f"exit {code} (skip) in {elapsed:.1f} s\n{out}"
    detail = f"exit {code} in {elapsed:.1f} s\n{out}"
    skip_patterns = regex_list(props.get("SKIP_REGULAR_EXPRESSION", []))
    required = regex_list(props.get("PASS_REGULAR_EXPRESSION", []))
    forbidden = regex_list(props.get("FAIL_REGULAR_EXPRESSION", []))
    # A required match takes the place of the exit code. A forbidden match overrides either kind of success.
    try:
        if any(re.search(pattern, out, re.DOTALL) for pattern in skip_patterns):
            return "skip", f"matched CTest SKIP_REGULAR_EXPRESSION: {detail}"
        passed = any(re.search(pattern, out, re.DOTALL) for pattern in required) if required else code == 0
        if any(re.search(pattern, out, re.DOTALL) for pattern in forbidden):
            passed = False
    except re.error as e:
        return "fail", f"invalid output regular expression: {e}\n{detail}"
    if truthy(props.get("WILL_FAIL")):
        passed = not passed
    return ("pass" if passed else "fail"), detail


def run_test_process(argv: list[str], *, cwd: str, env: dict[str, str],
                     timeout: float) -> subprocess.CompletedProcess:
    options = dict(cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if os.name != "posix":
        return subprocess.run(argv, **options, timeout=timeout)

    # Wine's Windows children and launcher wrappers can outlive the immediate child.
    with subprocess.Popen(argv, **options, start_new_session=True) as proc:
        with _running_lock:
            _running.add(proc.pid)
        try:
            out, _ = proc.communicate(timeout=timeout)
        except (subprocess.TimeoutExpired, KeyboardInterrupt):
            # The launcher may have exited while a helper still holds the output pipe.
            _kill_group(proc.pid)
            proc.wait()
            raise
        finally:
            with _running_lock:
                _running.discard(proc.pid)
        return subprocess.CompletedProcess(argv, proc.returncode, out)


# The process groups of the tests running now: with --jobs, Ctrl+C reaches only the main thread, which stops them.
_running: set[int] = set()
_running_lock = threading.Lock()


def _kill_group(pid: int) -> None:
    try:
        os.killpg(pid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        pass


def stop_running_tests() -> None:
    if os.name != "posix":
        return
    with _running_lock:
        pids = list(_running)
    for pid in pids:
        _kill_group(pid)


def positive_seconds(text: str) -> float:
    """--timeout: a finite value greater than 0, so subprocess.run never gets a bad timeout to raise on."""
    try:
        value = float(text)
    except ValueError:
        raise argparse.ArgumentTypeError(f"invalid float value: {text!r}") from None
    if not math.isfinite(value):
        raise argparse.ArgumentTypeError("must be a finite number")
    if value <= 0:
        raise argparse.ArgumentTypeError("must be greater than 0")
    return value


def positive_int(text: str) -> int:
    """--jobs: a whole number of at least 1."""
    try:
        value = int(text)
    except ValueError:
        raise argparse.ArgumentTypeError(f"invalid int value: {text!r}") from None
    if value < 1:
        raise argparse.ArgumentTypeError("must be at least 1")
    return value


def wine_command(text: str) -> list[str]:
    """--wine: split into a command the way a shell would, refusing blank or malformed values up front."""
    try:
        parts = shlex.split(text)
    except ValueError as e:
        raise argparse.ArgumentTypeError(f"malformed --wine: {e}") from None
    if not parts:
        raise argparse.ArgumentTypeError("must be a nonempty command")
    return parts


def valid_test_property(entry) -> bool:
    if not isinstance(entry, dict) or not isinstance(entry.get("name"), str) or "value" not in entry:
        return False
    name, value = entry["name"], entry["value"]
    if name == "WORKING_DIRECTORY":
        return isinstance(value, str) and "\0" not in value
    if name in ("PASS_REGULAR_EXPRESSION", "FAIL_REGULAR_EXPRESSION", "SKIP_REGULAR_EXPRESSION"):
        return isinstance(value, str) or (isinstance(value, list) and all(isinstance(p, str) for p in value))
    return True


def valid_test_row(test) -> bool:
    if not isinstance(test, dict) or not isinstance(test.get("name"), str):
        return False
    # CTest can omit the command for a target that was not built.
    command = test.get("command", [])
    if not isinstance(command, list) or not all(isinstance(arg, str) and "\0" not in arg for arg in command):
        return False
    entries = test.get("properties", [])
    return isinstance(entries, list) and all(valid_test_property(entry) for entry in entries)


def read_tests(path: str) -> list[dict] | None:
    """The tests in a `ctest --show-only=json-v1` file, or None when it is not one."""
    try:
        with open(path, encoding="utf-8-sig") as f:
            doc = json.load(f)
    except (OSError, ValueError) as e:
        print(f"ERROR: cannot read {path}: {e}", file=sys.stderr)
        return None
    tests = doc.get("tests") if isinstance(doc, dict) else None
    if not isinstance(tests, list) or not all(valid_test_row(t) for t in tests):
        print(f"ERROR: {path} is not the output of ctest --show-only=json-v1", file=sys.stderr)
        return None
    return tests


def run_parallel(tests: list[dict], order: list[int], before: dict[int, set[int]], jobs: int, run_one, outcomes: dict,
                 results_by_index: dict, report) -> None:
    """Run up to `jobs` tests at a time and report them in `order`, as the serial loop does.

    A test starts once the tests it depends on have finished. A RUN_SERIAL test runs alone, and two tests that share
    a RESOURCE_LOCK never run at the same time.
    """
    serial = {index for index, test in enumerate(tests) if truthy(properties(test).get("RUN_SERIAL"))}
    locks = {index: set(regex_list(properties(test).get("RESOURCE_LOCK", []))) for index, test in enumerate(tests)}
    waiting, running, held = list(order), {}, set()
    next_report = 0
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        try:
            while waiting or running:
                for index in list(waiting):
                    if len(running) >= jobs or any(r in serial for r in running.values()):
                        break
                    if before[index] - results_by_index.keys() or locks[index] & held:
                        continue
                    if index in serial and running:
                        continue
                    waiting.remove(index)
                    held |= locks[index]
                    running[pool.submit(run_one, index)] = index
                    if index in serial:
                        break
                done, _ = wait(running, return_when=FIRST_COMPLETED)
                for future in done:
                    index = running.pop(future)
                    outcomes[index] = future.result()
                    results_by_index[index] = outcomes[index][0]
                    held -= locks[index]
                while next_report < len(order) and order[next_report] in outcomes:
                    report(order[next_report])
                    next_report += 1
        except BaseException:
            # Ctrl+C reaches this thread only: stop the tests the workers are waiting on, start no more.
            pool.shutdown(wait=False, cancel_futures=True)
            stop_running_tests()
            raise


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tests", required=True, help="ctest --show-only=json-v1 output from the Windows build")
    ap.add_argument("--windows-root", required=True, help="the Windows checkout the tests were listed in")
    ap.add_argument("--root", type=os.path.abspath, default=ROOT, help="the Linux checkout (default: this repository)")
    ap.add_argument("--wine", type=wine_command, default=os.environ.get("WINE", "wine"),
                    help="Wine command (default: $WINE or wine)")
    ap.add_argument("--require", action="append", default=[], help="a test name whose entries must all pass (repeatable)")
    ap.add_argument("--timeout", type=positive_seconds, default=120.0,
                    help="maximum seconds per test; a shorter CTest TIMEOUT applies (default 120)")
    ap.add_argument("--jobs", type=positive_int, default=1,
                    help="tests run at a time; RUN_SERIAL and RESOURCE_LOCK are kept (default 1)")
    ap.add_argument("--summary", help="append a Markdown table to this file (e.g. $GITHUB_STEP_SUMMARY)")
    ap.add_argument("--label", help="named in the summary's heading, e.g. the Wine version")
    args = ap.parse_args()

    tests = read_tests(args.tests)
    if tests is None:
        return 2
    names = [test.get("name", "?") for test in tests]
    indices = {name: index for index, name in enumerate(names)}
    setups = fixture_dependencies(tests)
    sorter = TopologicalSorter(setups)
    before = {index: set(setups[index]) for index in range(len(tests))}
    for index, test in enumerate(tests):
        # DEPENDS controls order. Only failed fixture setups block a consumer.
        depends = [indices[name] for name in properties(test).get("DEPENDS", []) if name in indices]
        sorter.add(index, *depends)
        before[index].update(depends)
    try:
        order = list(sorter.static_order())
    except CycleError:
        print("ERROR: CTest test dependencies form a cycle", file=sys.stderr)
        return 2
    wine = args.wine
    # Each test starts in its own CTest working directory. Keep bare commands on PATH.
    if os.path.dirname(wine[0]) and not os.path.isabs(wine[0]):
        wine[0] = os.path.join(os.getcwd(), wine[0])
    results_by_index: dict[int, str] = {}
    prefix = os.environ.get("WINEPREFIX") or os.path.join(os.path.expanduser("~"), ".wine")
    outcomes: dict[int, tuple[str, str]] = {}

    def run_one(index: int) -> tuple[str, str]:
        failed_setups = tuple(names[setup] for setup in setups[index] if results_by_index[setup] == "fail")
        return run_test(tests[index], args, wine, failed_setups=failed_setups)

    def report(index: int) -> None:
        status, detail = outcomes[index]
        print(f"[{status.upper()}] {names[index]}", flush=True)
        if status != "pass":
            for line in detail.rstrip().splitlines()[-40:]:
                print(f"    {line}")

    with DriveMap(args.windows_root, args.root, prefix) as drive, KeptServer(find_wineserver(wine), wine) as server:
        print(drive.note, flush=True)
        print(server.note, flush=True)
        if args.jobs == 1:
            for index in order:
                outcomes[index] = run_one(index)
                results_by_index[index] = outcomes[index][0]
                report(index)
        else:
            run_parallel(tests, order, before, args.jobs, run_one, outcomes, results_by_index, report)

    missing = list(dict.fromkeys(n for n in args.require if n not in indices))
    nonpassing_names = {names[index] for index, status in results_by_index.items() if status != "pass"}
    failed_required = [n for n in args.require if n not in indices or n in nonpassing_names]
    counts = {s: sum(1 for v in results_by_index.values() if v == s) for s in ("pass", "fail", "skip")}
    print(f"\n{counts['pass']} passed, {counts['fail']} failed, {counts['skip']} skipped under Wine "
          f"({len(results_by_index)} tests)")
    for n in missing:
        print(f"required test not listed by the Windows build: {n}")
    if failed_required:
        print("required tests that did not pass: " + ", ".join(failed_required))

    if args.summary:
        try:
            _make_parent(args.summary)
            with open(args.summary, "a", encoding="utf-8", newline="\n") as f:
                f.write(f"### Windows tests under {args.label or 'Wine'}\n\n")
                f.write(f"{counts['pass']} passed, {counts['fail']} failed, {counts['skip']} skipped.\n\n")
                f.write("| Test | Result | Required |\n|---|---|---|\n")
                for index in order:
                    name, status = names[index], results_by_index[index]
                    f.write(f"| `{name}` | {status} | {'yes' if name in args.require else ''} |\n")
                for name in missing:
                    # The job fails on these, so the table must name them, not only the log.
                    f.write(f"| `{name}` | missing: not listed by the Windows build | yes |\n")
                f.write("\n")
        except OSError as e:
            print(f"ERROR: cannot write the summary {args.summary}: {e.strerror or e}", file=sys.stderr)
            return 1
    return 1 if failed_required else 0


if __name__ == "__main__":
    sys.exit(main())
