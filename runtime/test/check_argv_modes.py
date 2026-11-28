"""Compare real MSVC argv startup modes natively and through the PE loader.

Each fixture is linked with the default, setargv/wsetargv, or noarg option.
Raw Windows command lines retain quoted wildcard cases and identical argv[0].
All files are synthetic; the active code page's native result is the oracle.
"""

import argparse
import ctypes
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time


def run(executable, command, cwd, environment):
    try:
        result = subprocess.run(
            command, executable=executable, cwd=cwd, env=environment,
            capture_output=True, timeout=15,
        )
    except (subprocess.TimeoutExpired, OSError) as exc:
        return {
            "command": command,
            "exit_code": None,
            "lines": [],
            "stdout": "",
            "stderr": str(exc),
        }
    output = result.stdout.decode("utf-8", "replace")
    return {
        "command": command,
        "exit_code": result.returncode,
        "lines": [line for line in output.splitlines()
                  if line.startswith(("STATE ", "NOOP ", "CONFIG "))],
        "stdout": output,
        "stderr": result.stderr.decode("utf-8", "replace"),
    }


def _is_windows() -> bool:
    return os.name == "nt"


def _strip_env_keys(environment, names):
    """Remove environment keys (case-insensitively on Windows)."""
    if _is_windows():
        targets = {name.upper() for name in names}
        for key in list(environment):
            if key.upper() in targets:
                del environment[key]
    else:
        for name in names:
            environment.pop(name, None)


def _preflight_paths(args, parser):
    raw_loader = str(args.loader)
    if not raw_loader.strip() or not Path(raw_loader).is_file():
        parser.error(f"--loader is not an existing file: {raw_loader}")
    for flag, path in (("--fixtures", args.fixtures), ("--work-dir", args.work_dir)):
        raw = str(path)
        if not raw.strip() or not Path(raw).is_dir():
            parser.error(f"{flag} is not an existing directory: {raw}")
    if args.report is not None:
        raw_rep = str(args.report)
        if not raw_rep.strip():
            parser.error("--report path must not be empty")
        if raw_rep.endswith(("/", "\\")) or (os.sep and raw_rep.endswith(os.sep)):
            parser.error(f"--report must name a file, not a directory: {raw_rep}")
        for cand in (Path(os.path.abspath(raw_rep)), Path(os.path.realpath(raw_rep))):
            if cand.is_dir():
                parser.error(f"--report is a directory: {raw_rep}")
            anc = cand.parent
            while str(anc) and not (anc.exists() or anc.is_symlink()):
                if anc.parent == anc:
                    break
                anc = anc.parent
            if str(anc) and not anc.is_dir():
                parser.error(f"--report parent is not a directory: {anc}")


def _write_report_atomic(path: Path, payload: dict) -> None:
    dest = Path(os.path.realpath(path))
    dest.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=dest.parent, prefix=".report-", suffix=".tmp")
    try:
        try:
            f = os.fdopen(fd, "w", encoding="utf-8", newline="\n")
        except BaseException:
            os.close(fd)
            raise
        with f:
            json.dump(payload, f, indent=2)
            f.write("\n")
            f.flush()
            os.fsync(f.fileno())
        if os.name == "posix":
            try:
                shutil.copymode(dest, tmp)
            except FileNotFoundError:
                pass
        for delay in (0.01, 0.02, 0.04) if _is_windows() else ():
            try:
                os.replace(tmp, dest)
                return
            except PermissionError as exc:
                if getattr(exc, "winerror", None) not in (5, 32, 33):
                    raise
                time.sleep(delay)
        os.replace(tmp, dest)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def entry_count(lines):
    if not lines:
        return None
    match = re.fullmatch(r"STATE entry argc=(\d+) A=.* W=.*", lines[0])
    return int(match[1]) if match else None


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--loader", type=Path, required=True)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--report", type=Path, help="optional retained JSON evidence")
    args = parser.parse_args(argv)
    if not _is_windows():
        parser.error("this native MSVC startup comparison requires Windows")
    _preflight_paths(args, parser)
    loader = args.loader.resolve(strict=True)
    fixtures = args.fixtures.resolve(strict=True)
    work_dir = args.work_dir.resolve(strict=True)
    rows = []
    failures = []
    tails = {
        "plain": "data/*.txt literal",
        "quoted": '"data/*.txt" literal',
        "unmatched": "data/*.missing literal",
        "empty": "empty/* literal",
        "space": '"space dir/*.txt" literal',
        "encoding": "unicode/\u6f22*.txt literal",
        "sequence": "data/*.txt literal",
    }
    with tempfile.TemporaryDirectory(prefix="argv_modes_", dir=work_dir) as temporary:
        root = Path(temporary)
        for directory in ("data", "empty", "space dir", "unicode", "package"):
            (root / directory).mkdir()
        for name in ("data/a.txt", "data/b.txt", "data/c.bin",
                     "space dir/a.txt", "space dir/b.txt",
                     "unicode/A-only.txt", "unicode/\u6f22-only.txt"):
            (root / name).write_text("synthetic\n", encoding="utf-8", newline="\n")
        for width in ("a", "w"):
            for mode in ("default", "expand", "noargs"):
                guest = (fixtures / f"tiny_argv_{mode}_{width}.exe").resolve(strict=True)
                cases = ["plain", "quoted", "unmatched"]
                if mode == "expand":
                    cases += ["empty", "space", "encoding"]
                if mode == "noargs":
                    cases += ["sequence"]
                native_plain = None
                for case in cases:
                    label = f"{width}/{mode}/{case}"
                    environment = os.environ.copy()
                    _strip_env_keys(environment, ("LEONIDA_GAME_DIR", "LEONIDA_ARGV_SEQUENCE"))
                    if case == "sequence":
                        environment["LEONIDA_ARGV_SEQUENCE"] = "1"
                    native_command = subprocess.list2cmdline([guest.name]) + " " + tails[case]
                    mapped_command = subprocess.list2cmdline([
                        loader.name, "--run", str(guest), "--game-dir", str(root / "package"),
                        "--overlay", str(root / "overlay"),
                    ]) + " -- " + tails[case]
                    native = run(guest, native_command, root, environment)
                    mapped = run(loader, mapped_command, root, environment)
                    problems = []
                    for route, result in (("native", native), ("mapped", mapped)):
                        if result["exit_code"] != 0 or entry_count(result["lines"]) is None:
                            problems.append(f"{route} did not complete its fixture")
                    expected = 0 if mode == "noargs" else 3
                    if mode == "expand" and case in ("plain", "quoted", "space"):
                        expected = 4
                    if case != "encoding" and case != "empty":
                        if entry_count(native["lines"]) != expected:
                            problems.append(f"native fixture did not produce argc={expected}")
                    if mode == "noargs" and native["lines"] and native["lines"][0] != "STATE entry argc=0 A=null W=null":
                        problems.append("native noarg fixture did not start unconfigured")
                    if case == "plain":
                        native_plain = native["lines"]
                    if case == "quoted" and native["lines"] != native_plain:
                        problems.append("native quoted wildcard differs from unquoted")
                    if case == "sequence":
                        noops = [line for line in native["lines"] if line.startswith("NOOP ")]
                        configs = [line for line in native["lines"] if line.startswith("CONFIG ")]
                        if len(noops) != 6 or any(not line.endswith("status=0 unchanged=1") for line in noops):
                            problems.append("native no-argument reconfiguration changed state")
                        if len(configs) != 4 or any(not line.endswith("status=0") for line in configs):
                            problems.append("native reconfiguration did not succeed")
                    if native["lines"] != mapped["lines"]:
                        problems.append("mapped argv state differs from native")
                    if "game exited with code 0" not in mapped["stdout"]:
                        problems.append("loader did not report a completed guest run")
                    if problems:
                        failures.append(label + ": " + "; ".join(problems))
                        print("FAIL " + failures[-1], flush=True)
                        print("  native: " + repr(native["lines"]), flush=True)
                        print("  mapped: " + repr(mapped["lines"]), flush=True)
                    rows.append({"case": label, "native": native, "mapped": mapped, "problems": problems})
        if any((root / "package").iterdir()):
            failures.append("the synthetic package was modified")
    report = {"acp": ctypes.windll.kernel32.GetACP(), "pairs": len(rows), "failures": failures, "rows": rows}
    if args.report:
        _write_report_atomic(args.report, report)
    print(f"argv modes: {len(rows)} native/mapped pairs, {len(failures)} failures, ACP {report['acp']}")
    return int(bool(failures))


if __name__ == "__main__":
    raise SystemExit(main())
