#!/usr/bin/env python3
"""One-command synthetic source-only smoke suite for Leonida (DX-07).

Validates prerequisites, compiles source trees, enforces clean-room boundaries,
and executes fast synthetic smoke tests on Windows, Linux, and macOS without game files.
"""

from __future__ import annotations

import argparse
import compileall
import contextlib
import io
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
MIN_PYTHON_VERSION = (3, 10)


def check_prerequisites(require_toolchains: list[str] | None = None) -> tuple[dict, bool]:
    """Check required environment and discover optional toolchains."""
    py_ver = sys.version_info
    py_ok = (py_ver.major, py_ver.minor) >= MIN_PYTHON_VERSION

    prereqs = {
        "python": {
            "version": f"{py_ver.major}.{py_ver.minor}.{py_ver.micro}",
            "satisfies": py_ok,
            "required": f">={MIN_PYTHON_VERSION[0]}.{MIN_PYTHON_VERSION[1]}",
        }
    }

    toolchains = {
        "cmake": _detect_command("cmake", ["--version"]),
        "ninja": _detect_command("ninja", ["--version"]),
        "compiler_msvc": _detect_command("cl", []),
        "compiler_gcc": _detect_command("gcc", ["--version"]),
        "compiler_clang": _detect_command("clang", ["--version"]),
        "vulkan_sdk": _detect_vulkan(),
        "node": _detect_command("node", ["--version"]),
        "dotnet": _detect_command("dotnet", ["--version"]),
    }

    all_ok = py_ok
    if require_toolchains:
        for req in require_toolchains:
            found = False
            if req == "compiler":
                found = any(
                    toolchains[c]["available"]
                    for c in ("compiler_msvc", "compiler_gcc", "compiler_clang")
                )
            elif req in toolchains:
                found = toolchains[req]["available"]
            if not found:
                all_ok = False

    return {"prerequisites": prereqs, "toolchains": toolchains}, all_ok


def _detect_command(command: str, version_args: list[str]) -> dict:
    exe = shutil.which(command)
    if not exe:
        return {"available": False, "path": None, "version": None}

    version_str = None
    if version_args:
        try:
            res = subprocess.run(
                [exe, *version_args],
                capture_output=True,
                text=True,
                timeout=5,
            )
            if res.returncode == 0:
                first_line = (res.stdout or res.stderr).strip().splitlines()
                if first_line:
                    version_str = first_line[0]
        except (OSError, subprocess.TimeoutExpired):
            pass

    return {"available": True, "path": exe, "version": version_str}


def _detect_vulkan() -> dict:
    sdk_dir = os.environ.get("VULKAN_SDK")
    glslang = shutil.which("glslangValidator") or shutil.which("glslang")
    available = bool(sdk_dir or glslang)
    return {
        "available": available,
        "path": sdk_dir or (glslang if glslang else None),
        "version": sdk_dir if sdk_dir else ("glslang found" if glslang else None),
    }


def run_checks(root: Path, quick: bool = False, verbose: bool = False) -> list[dict]:
    """Execute synthetic source-only sanity checks."""
    checks = []

    # 1. Bytecode compilation of Python source trees
    t0 = time.monotonic()
    compile_ok = True
    compile_err = None
    compile_buf = io.StringIO()
    try:
        with contextlib.redirect_stdout(compile_buf):
            if quick:
                # Quick mode: verify core entry points compile
                for p in (root / "make_game.py", root / "tools" / "rdna2spirv.py", root / "scripts" / "dev.py", root / "scripts" / "smoke.py"):
                    if p.is_file() and not compileall.compile_file(str(p), quiet=1, force=False):
                        compile_ok = False
                        compile_err = f"{p.name} failed bytecode compilation"
                        break
            else:
                # Full mode: verify all tools and scripts
                res = compileall.compile_dir(str(root / "tools"), quiet=1, force=False)
                res = compileall.compile_dir(str(root / "scripts"), quiet=1, force=False) and res
                res = compileall.compile_file(str(root / "make_game.py"), quiet=1, force=False) and res
                if not res:
                    compile_ok = False
                    compile_err = "One or more files failed bytecode compilation"
    except Exception as exc:  # noqa: BLE001
        compile_ok = False
        compile_err = str(exc)
    if not compile_ok and compile_buf.getvalue().strip():
        print(compile_buf.getvalue().rstrip(), file=sys.stderr)

    checks.append({
        "name": "python_compileall",
        "status": "PASS" if compile_ok else "FAIL",
        "duration_ms": int((time.monotonic() - t0) * 1000),
        "details": compile_err or ("Core entry points compile cleanly" if quick else "tools/, scripts/, make_game.py compile cleanly"),
    })

    # 2. Clean-room validation (no game data)
    t0 = time.monotonic()
    check_no_game_script = root / "scripts" / "check_no_game_data.py"
    if check_no_game_script.is_file():
        cmd = [sys.executable, "-I", str(check_no_game_script), "make_game.py", "scripts/dev.py"]
        proc = subprocess.run(cmd, cwd=root, capture_output=True, text=True)
        no_game_ok = proc.returncode == 0
        checks.append({
            "name": "clean_room_no_game_data",
            "status": "PASS" if no_game_ok else "FAIL",
            "duration_ms": int((time.monotonic() - t0) * 1000),
            "details": proc.stderr.strip() if not no_game_ok else "No game data or retail assets detected",
        })
    else:
        checks.append({
            "name": "clean_room_no_game_data",
            "status": "SKIP",
            "duration_ms": 0,
            "details": "scripts/check_no_game_data.py not present",
        })

    # 3. Core CLI --help entry points
    cli_targets = [
        ("make_game.py", root / "make_game.py"),
        ("tools/rdna2spirv.py", root / "tools" / "rdna2spirv.py"),
        ("scripts/dev.py", root / "scripts" / "dev.py"),
    ]
    for label, script_path in cli_targets:
        if not script_path.is_file():
            checks.append({
                "name": f"cli_help_{label}",
                "status": "SKIP",
                "duration_ms": 0,
                "details": f"{label} not found",
            })
            continue

        t0 = time.monotonic()
        proc = subprocess.run(
            [sys.executable, "-I", str(script_path), "--help"],
            cwd=root,
            capture_output=True,
            text=True,
            timeout=10,
        )
        ok = proc.returncode == 0 and bool(proc.stdout)
        checks.append({
            "name": f"cli_help_{label}",
            "status": "PASS" if ok else "FAIL",
            "duration_ms": int((time.monotonic() - t0) * 1000),
            "details": f"Exit code {proc.returncode}" if not ok else f"{label} --help exited with 0",
        })

    # 4. Fast synthetic unit test smoke pass (unless --quick)
    if not quick:
        test_targets = [
            "tests/test_dev_discovery.py",
            "tests/test_shader_ir_parse.py",
        ]
        for rel_test in test_targets:
            test_path = root / rel_test
            if not test_path.is_file():
                continue

            t0 = time.monotonic()
            proc = subprocess.run(
                [sys.executable, "-I", str(test_path)],
                cwd=root,
                capture_output=True,
                text=True,
                timeout=30,
            )
            ok = proc.returncode == 0
            checks.append({
                "name": f"test_{Path(rel_test).stem}",
                "status": "PASS" if ok else "FAIL",
                "duration_ms": int((time.monotonic() - t0) * 1000),
                "details": proc.stderr.strip() if not ok else "Test suite passed",
            })

    return checks


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="smoke.py",
        description="One-command synthetic source-only smoke suite (DX-07).",
    )
    parser.add_argument(
        "--quick",
        action="store_true",
        help="Run fast sanity compilation and CLI help checks only (< 1s)",
    )
    parser.add_argument(
        "--prereqs-only",
        action="store_true",
        help="Discover toolchains and report prerequisite status without running checks",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Print machine-readable JSON summary",
    )
    parser.add_argument(
        "--require",
        nargs="+",
        metavar="TOOL",
        help="Require specific toolchain(s) to be present (e.g. cmake, ninja, compiler)",
    )
    parser.add_argument(
        "-v", "--verbose",
        action="store_true",
        help="Print detailed execution output",
    )

    args = parser.parse_args(argv)

    diag, prereqs_ok = check_prerequisites(require_toolchains=args.require)

    if not prereqs_ok:
        if args.json:
            print(json.dumps({
                "schema_version": 1,
                "platform": platform.platform(),
                "verdict": "ERROR",
                "error": "Required prerequisites or toolchains missing",
                **diag,
            }, indent=2))
        else:
            print("error: required environment prerequisites or toolchains missing:", file=sys.stderr)
            for k, v in diag["prerequisites"].items():
                if not v["satisfies"]:
                    print(f"  - {k}: {v['version']} (requires {v['required']})", file=sys.stderr)
            if args.require:
                for req in args.require:
                    print(f"  - required toolchain '{req}' was not found", file=sys.stderr)
        return 2

    if args.prereqs_only:
        if args.json:
            print(json.dumps({
                "schema_version": 1,
                "platform": platform.platform(),
                "verdict": "PASS",
                **diag,
            }, indent=2))
        else:
            print(f"Leonida Smoke Suite — Environment Discovery ({platform.platform()})")
            print("Prerequisites:")
            for k, v in diag["prerequisites"].items():
                status = "OK" if v["satisfies"] else "FAIL"
                print(f"  - {k}: {v['version']} [{status}]")
            print("Toolchains (optional for synthetic tests):")
            for k, v in diag["toolchains"].items():
                status = "PRESENT" if v["available"] else "NOT FOUND"
                detail = f" ({v['version']})" if v["version"] else ""
                print(f"  - {k}: {status}{detail}")
        return 0

    if args.json:
        with contextlib.redirect_stdout(sys.stderr):
            checks = run_checks(ROOT, quick=args.quick, verbose=args.verbose)
    else:
        checks = run_checks(ROOT, quick=args.quick, verbose=args.verbose)
    all_passed = all(c["status"] in ("PASS", "SKIP") for c in checks)

    if args.json:
        report = {
            "schema_version": 1,
            "platform": platform.platform(),
            "verdict": "PASS" if all_passed else "FAIL",
            **diag,
            "checks": checks,
        }
        print(json.dumps(report, indent=2))
    else:
        print(f"Leonida Smoke Suite — Source-Only Validation ({platform.platform()})")
        for c in checks:
            status_tag = f"[{c['status']}]"
            print(f"  {status_tag:<8} {c['name']:<30} ({c['duration_ms']} ms) — {c['details']}")
        print("Verdict: " + ("PASS (All source-only checks succeeded)" if all_passed else "FAIL"))

    return 0 if all_passed else 1


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError, OSError):
        pass
    raise SystemExit(main())
