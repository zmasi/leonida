"""Write <spirv>/native_hashes.txt: the HASH digest of each carved DXBC container -> its stage and path.

Xbox device slot 57 creates the game's graphics pipelines from native code, and each stage's records name the
source container by the 16-byte digest of its HASH part (docs/fspx.md, kind 7). This catalog connects that
identifier to the original local container and the stage in the translation index.

Usage: python -I tools/native_pso_hashes.py <work>/shaders/dxbc <work>/shaders/spirv

Only containers in the translator's index are written. Entries sharing a digest must agree on stage and
fssm_sha256. Adapted from enx/leonida's native graphics PSO index (commit 5c175db90).
"""
import json
import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from shader_carve import parts

HEADER = "# native_pso_hashes 1"
OUT_NAME = "native_hashes.txt"


class DigestClash(ValueError):
    """One digest names two different programs."""


def hash_digest(data: bytes):
    """The 16-byte digest of a DXBC container's HASH part (after its u32 flags), or None."""
    if data[:4] != b"DXBC":
        return None
    try:
        chunk = parts(data).get("HASH")
    except ValueError:
        return None
    if not chunk or chunk[1] < 20:
        return None
    offset, _ = chunk
    return data[offset + 4:offset + 20]


def build(dxbc: str, spirv: str) -> dict:
    """{digest hex: (stage, path relative to spirv)}. Raises DigestClash when one digest names two programs."""
    workspace = os.path.dirname(os.path.realpath(spirv))
    if os.path.commonpath((workspace, os.path.realpath(dxbc))) != workspace:
        raise ValueError("the carved folder must be inside the same shader workspace as the translation")
    with open(os.path.join(spirv, "index.json"), encoding="utf-8-sig") as f:
        index = json.load(f)
    if not isinstance(index, dict):
        raise ValueError("index.json is not an object")
    found = {}
    for name in sorted(os.listdir(dxbc)):
        if not name.endswith(".dxbc"):
            continue
        stem = name[:-5]
        entry = index.get(stem)
        if not isinstance(entry, dict):
            continue
        with open(os.path.join(dxbc, name), "rb") as f:
            digest = hash_digest(f.read())
        if digest is None:
            continue
        stage, fssm = entry.get("stage"), entry.get("fssm_sha256")
        if (not isinstance(stage, str) or re.fullmatch(r"[a-z]+", stage) is None or
                not isinstance(fssm, str) or re.fullmatch(r"[0-9a-f]{64}", fssm) is None):
            raise ValueError(f"invalid translated stage or FSSM hash for {name!r}")
        if any(c in stem for c in "\\:\t\r\n"):
            raise ValueError(f"cannot represent this container name in the native catalog: {name!r}")
        key = digest.hex()
        path = os.path.relpath(os.path.join(dxbc, name), spirv).replace(os.sep, "/")
        if key in found:
            first = found[key]
            if first[2] != fssm or first[0] != stage:
                raise DigestClash(f"digest {key} names two programs: {first[3]} and {stem}")
            continue
        found[key] = (stage, path, fssm, stem)
    return {k: (v[0], v[1]) for k, v in found.items()}


def write(spirv: str, table: dict) -> str:
    out = os.path.join(spirv, OUT_NAME)
    tmp = tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", newline="\n", dir=spirv,
                                      prefix=".native_hashes.", suffix=".tmp", delete=False)
    try:
        with tmp:
            tmp.write(HEADER + "\n")
            for key in sorted(table):
                stage, path = table[key]
                tmp.write(f"{key}\t{stage}\t{path}\n")
        os.replace(tmp.name, out)
    finally:
        if os.path.exists(tmp.name):
            os.unlink(tmp.name)
    return out


def main(argv) -> int:
    if argv[1:] in (["-h"], ["--help"]):
        print(__doc__.strip())
        return 0
    if len(argv) != 3:
        print(__doc__.strip().splitlines()[0], file=sys.stderr)
        print("usage: native_pso_hashes.py <dxbc dir> <spirv dir>", file=sys.stderr)
        return 2
    dxbc, spirv = argv[1], argv[2]
    try:
        table = build(dxbc, spirv)
        out = write(spirv, table)
    except (OSError, ValueError) as e:
        print(f"native_pso_hashes: {e}", file=sys.stderr)
        return 1
    print(f"native_pso_hashes: {len(table)} digests -> {out}")
    return 0


if __name__ == "__main__":
    try:  # a redirected stdout is strict ANSI code page under -I; write '?' for what it cannot encode
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError, OSError):
        pass
    sys.exit(main(sys.argv))
