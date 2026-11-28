"""Check that a copy of the retail executable is the file this repository measured.

Usage:
    python -I exe_sha.py <exe> [--expect SHA256] [--manifest PATH] [--json] [--print-keys]

Contributors run the tools in this folder against their own copy of the game, and two people can disagree about a
result simply because their copies differ. The package manifest records the executable's size and SHA-256
(`expected_exe_size`, `expected_exe_sha256`, both optional, in `game/<version>.json`); this prints the numbers of the
file it is given, compares them with the recorded ones or with `--expect`, and exits 1 when they differ. The size is
checked first, so a truncated copy is reported as a size mismatch without hashing 120 MB.

With nothing recorded and no `--expect` it prints the numbers and exits 0: it is a report, not a verification. Use
`--print-keys` to get the two manifest lines for the file in hand. `--expect` takes a hash someone else quoted, so a
contributor can check a copy before the manifest records one.

Reads the file only; never runs it. Standard library only, like the other tools.
"""
from __future__ import annotations

import argparse
import glob
import hashlib
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CHUNK = 1 << 20
_HEX_DIGITS = frozenset("0123456789abcdef")


def _valid_sha256(value: object) -> str | None:
    if not isinstance(value, str):
        return None
    s = value.strip().lower()
    return s if len(s) == 64 and all(c in _HEX_DIGITS for c in s) else None


def sha256_of(path: str) -> str:
    """The SHA-256 of a file, read in chunks so a 120 MB image is not one allocation."""
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(CHUNK), b""):
            digest.update(block)
    return digest.hexdigest()


def find_manifest(exe: str) -> str | None:
    """The package manifest that names this file as its executable, if there is one."""
    name = os.path.basename(exe)
    for path in sorted(glob.glob(os.path.join(ROOT, "game", "*.json"))):
        try:
            with open(path, encoding="utf-8-sig") as f:
                data = json.load(f)
        except (OSError, ValueError):
            continue
        if isinstance(data, dict) and data.get("expected_exe") == name:
            return path
    return None


def recorded(manifest: str | None, *, strict: bool = False) -> tuple[str | None, int | None, str | None]:
    """(source, expected size, expected sha256) from a manifest path, ignoring absent or unusable keys."""
    if not manifest:
        return None, None, None
    try:
        with open(manifest, encoding="utf-8-sig") as f:
            data = json.load(f)
    except (OSError, ValueError):
        if strict:
            raise
        return manifest, None, None
    if not isinstance(data, dict):
        if strict:
            raise ValueError("manifest root must be a JSON object")
        return manifest, None, None
    size = data.get("expected_exe_size")
    size = size if isinstance(size, int) and not isinstance(size, bool) and size >= 0 else None
    digest = _valid_sha256(data.get("expected_exe_sha256"))
    return manifest, size, digest


def check(exe: str, expect: str | None = None, manifest: str | None = None) -> dict:
    """Compare one file with the recorded or given size and hash. Raises OSError if the file is unreadable."""
    used = manifest
    expected_size = expected_sha = None
    if manifest is None and expect is None:
        used = find_manifest(exe)
    if expect is None:
        used, expected_size, expected_sha = recorded(used, strict=(manifest is not None))
    else:
        expect = expect.strip().lower()
        expected_sha = _valid_sha256(expect)

    size = os.path.getsize(exe)
    result = {
        "path": exe,
        "size": size,
        "sha256": None,
        "expected_size": expected_size,
        "expected_sha256": expected_sha,
        "source": "--expect" if expect is not None else used,
        "status": "unrecorded",
        "mismatches": [],
    }
    if expected_sha is None and expect is not None:
        result["status"] = "mismatch"
        result["mismatches"].append(f"--expect {expect!r} is not a SHA-256 (64 hex digits)")
        return result
    if expected_size is not None and expected_size != size:
        result["mismatches"].append(f"size: {size} bytes, expected {expected_size}")
        result["status"] = "mismatch"
        return result
    result["sha256"] = sha256_of(exe)
    if expected_sha is not None:
        if expected_sha != result["sha256"]:
            result["mismatches"].append(f"sha256: {result['sha256']}, expected {expected_sha}")
            result["status"] = "mismatch"
        else:
            result["status"] = "match"
    elif expected_size is not None:
        result["status"] = "match"
    return result


def keys_for(result: dict) -> str:
    return f'"expected_exe_size": {result["size"]},\n"expected_exe_sha256": "{result["sha256"]}",'


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Check a copy of the retail executable against its recorded size and SHA-256.")
    ap.add_argument("exe", help="the executable to read (never run)")
    ap.add_argument("--expect", metavar="SHA256", help="compare with this hash instead of the manifest's")
    ap.add_argument("--manifest", metavar="PATH", help="package manifest to read the recorded numbers from")
    ap.add_argument("--json", action="store_true", help="print the result as JSON")
    ap.add_argument("--print-keys", action="store_true", help="print the manifest lines for this file")
    args = ap.parse_args(argv)

    if not os.path.isfile(args.exe):
        print(f"exe_sha: {args.exe}: not a file", file=sys.stderr)
        return 2
    try:
        result = check(args.exe, args.expect, args.manifest)
    except OSError as exc:
        bad = args.manifest if args.manifest and getattr(exc, "filename", None) == args.manifest else args.exe
        print(f"exe_sha: {bad}: {exc.strerror or exc}", file=sys.stderr)
        return 2
    except ValueError as exc:
        print(f"exe_sha: {args.manifest or args.exe}: {exc}", file=sys.stderr)
        return 2

    if args.json:
        print(json.dumps(result, indent=2))
    elif not args.print_keys:
        print(f"size: {result['size']}")
        if result["sha256"]:
            print(f"sha256: {result['sha256']}")
        if result["status"] == "match":
            where = "--expect" if result["source"] == "--expect" else f"the manifest ({result['source']})"
            print(f"ok: matches {where}")
        elif result["status"] == "mismatch":
            for line in result["mismatches"]:
                print(f"exe_sha: {line}", file=sys.stderr)
        else:
            print("no recorded size or hash to compare with (--print-keys writes them for a manifest)")

    if args.print_keys:
        if not result["sha256"]:
            print("exe_sha: the file was not hashed (size mismatch), so there are no keys to print", file=sys.stderr)
            return 1
        print(keys_for(result))
    return 1 if result["status"] == "mismatch" else 0


if __name__ == "__main__":
    sys.stdout.reconfigure(errors="replace")
    sys.exit(main())
