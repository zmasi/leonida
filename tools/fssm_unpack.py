"""Unpack the native GPU program from each carved shader container.

Layout found in GTA VI's Xbox shaders (shader model 6.6 containers):

    DXBC
      DXIL part  = DxilProgramHeader (stub) + XDAT
        XDAT     = 'XDAT' u32 count, u32 header_size, then count x {u32 tag, u32 offset, u32 size}
                   (offsets are relative to the DXIL part)
          tag 0x12  16-byte hash
          tag 0x0d  'XBCH' u32 ?, u32 stored, u32 0, u32 unpacked, u32 0, then zlib -> 'FSSM' blob
          tag 0x07  DxilProgramHeader with 'NOBC' (no LLVM bitcode is shipped)

Some containers carry **two distinct tag-0x0D payloads** (195 of 26,503 measured). Both decompress to
valid FSSM blobs with the same code offset but different code sizes; the first is 64 or 128 bytes
longer. `xdat_entries_all` preserves their order, `xdat_entries` keeps the previous last-wins mapping
for compatibility, and `unpack()` defaults to the last payload (the one the existing corpus used) with
`payload="first"` available for the XDAT-order/SCARAB choice. The CLI exposes `--payload`.
`unpack_all()` returns every tag-0x0D payload in directory order, for callers that need both (a
two-payload VS container holds its ES and VS hardware variants; `tools/pal_stages.py` reads them).
      PRIV part  = Xbox private metadata

The FSSM blob is the precompiled RDNA2 program plus its metadata. It contains the s_endpgm (0xBF810000) that ends
the code.

Usage:
    python -I fssm_unpack.py <carved_shader_dir> [--carve-index FILE] [--payload first|last]
    (writes <dir>/fssm/<name>.fssm and fssm_index.json)
"""
import argparse
import json
import os
import struct
import sys
import tempfile
import zlib
from collections import Counter

# `python -I` leaves the script's directory off sys.path.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from file_mode import published_mode  # noqa: E402
from shader_stage_manifest import shader_stages  # noqa: E402
from replace_retry import replace as replace_retry  # noqa: E402

# Defensive per-blob ceiling, not a maximum defined by the XBCH format.
# 64 MiB matches the repository's existing per-resource cap in tools/world_index.py.
MAX_FSSM_BYTES = 64 << 20


def parts(d: bytes) -> dict:
    if len(d) < 32:
        raise ValueError("truncated DXBC header")
    if d[:4] != b"DXBC":
        raise ValueError(f"invalid DXBC magic {d[:4]!r}")
    count = struct.unpack_from("<I", d, 28)[0]
    table_end = 32 + 4 * count
    if table_end > len(d):
        raise ValueError("truncated DXBC offset table")
    out = {}
    for k in range(count):
        off = struct.unpack_from("<I", d, 32 + 4 * k)[0]
        if off < table_end or off + 8 > len(d):
            raise ValueError(f"invalid DXBC part {k} header offset {off}")
        size = struct.unpack_from("<I", d, off + 4)[0]
        end = off + 8 + size
        if end > len(d):
            raise ValueError(f"truncated DXBC part {k} payload")
        out[d[off:off + 4]] = d[off + 8:end]
    return out


def xdat_entries_all(dxil: bytes) -> list:
    """All XDAT entries in directory order, as (tag, payload) pairs."""
    x = dxil.find(b"XDAT")
    if x < 0:
        return []
    if x + 12 > len(dxil):
        raise ValueError("truncated XDAT header")
    count = struct.unpack_from("<I", dxil, x + 4)[0]
    table_end = x + 12 + 12 * count
    if table_end > len(dxil):
        raise ValueError("truncated XDAT entry table")
    out = []
    for k in range(count):
        tag, off, size = struct.unpack_from("<III", dxil, x + 12 + 12 * k)
        end = off + size
        if end > len(dxil):
            raise ValueError(f"XDAT entry {k} extends past the DXIL part")
        if size and off < table_end and end > x:
            raise ValueError(f"XDAT entry {k} overlaps its directory")
        out.append((tag, dxil[off:end]))
    return out


def xdat_entries(dxil: bytes) -> dict:
    """Last payload per tag, kept for callers that want the old single-value view."""
    return {tag: payload for tag, payload in xdat_entries_all(dxil)}


def _inflate_xbch(blob: bytes):
    """Decompress one XBCH entry payload to its FSSM blob, or None when it is not XBCH."""
    if not blob or blob[:4] != b"XBCH":
        return None
    if len(blob) < 24:
        raise struct.error(f"truncated XBCH header ({len(blob)} bytes, need 24)")
    stored = struct.unpack_from("<I", blob, 8)[0]
    unpacked = struct.unpack_from("<I", blob, 16)[0]
    if unpacked > MAX_FSSM_BYTES:
        raise ValueError(f"XBCH declared size {unpacked} exceeds limit {MAX_FSSM_BYTES}")
    compressed = blob[24:]
    if stored != len(compressed):
        raise ValueError(f"XBCH stored size {stored} does not match payload size {len(compressed)}")
    decompressor = zlib.decompressobj()
    # The extra byte detects a stream that expands past the declared size without inflating it all.
    data = decompressor.decompress(compressed, unpacked + 1)
    if len(data) > unpacked:
        raise ValueError(f"decompressed XBCH exceeds declared size {unpacked}")
    if not decompressor.eof:
        raise ValueError("truncated XBCH zlib stream")
    if decompressor.unused_data:
        raise ValueError("trailing data after XBCH zlib stream")
    if len(data) != unpacked:
        raise ValueError(f"size mismatch {len(data)} != {unpacked}")
    if len(data) < 6:
        raise ValueError("truncated FSSM magic or version")
    if data[:4] != b"FSSM":
        raise ValueError(f"invalid FSSM magic {data[:4]!r}")
    return data


def unpack_all(dxbc: bytes):
    """Every tag-0x0D payload in directory order, decompressed to its FSSM blob."""
    dxil = parts(dxbc).get(b"DXIL", b"")
    out = []
    for tag, value in xdat_entries_all(dxil):
        if tag == 0x0D:
            data = _inflate_xbch(value)
            if data is None:
                raise ValueError("a tag-0x0D XDAT entry is not an XBCH payload")
            out.append(data)
    return out


def _unpack_with_count(dxbc: bytes, payload: str = "last"):
    p = parts(dxbc)
    dxil = p.get(b"DXIL", b"")
    all_entries = xdat_entries_all(dxil)
    ents = {tag: value for tag, value in all_entries}
    blobs = [value for tag, value in all_entries if tag == 0x0D]
    if not blobs:
        return None, ents, 0
    if payload == "first":
        blob = blobs[0]
    elif payload == "last":
        blob = blobs[-1]
    else:
        raise ValueError(f"payload must be 'first' or 'last', not {payload!r}")
    return _inflate_xbch(blob), ents, len(blobs)


def unpack(dxbc: bytes, payload: str = "last"):
    data, ents, _ = _unpack_with_count(dxbc, payload=payload)
    return data, ents


def program_counts(rows: list, stages: dict) -> dict:
    """Containers, programs (tag-0x0D payloads) and containers with more than one payload, per stage.

    A container is one carved DXBC file and one row here; a program is one tag-0x0D payload in it. The two counts
    differ wherever a container carries more than one payload (docs/shader_library.md, "Containers and programs").
    `stages` maps a row's name to the stage the carve index recorded. A name it lacks, or one it holds without a
    stage, is counted under None: its stage is unknown."""
    out = {}
    for row in rows:
        counts = out.setdefault(stages.get(row["name"]), {"containers": 0, "programs": 0, "multi_payload": 0})
        counts["containers"] += 1
        counts["programs"] += row["xbch_payloads"]
        counts["multi_payload"] += row["xbch_payloads"] > 1
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Unpack the FSSM program from each carved shader container.")
    ap.add_argument("src", help="carved shader directory")
    ap.add_argument("--carve-index", help="unpack only the DXBC files listed in this carve index")
    ap.add_argument("--payload", choices=("first", "last"), default="last",
                    help="tag-0x0D payload when a container has more than one (default: last)")
    a = ap.parse_args(argv)
    src = a.src
    # Check before anything is created: makedirs below would otherwise make a mistyped source folder.
    if not os.path.isdir(src):
        what = "is not a folder" if os.path.exists(src) else "does not exist"
        print(f"ERROR: {src} {what}. Give the folder with the carved .dxbc files.", file=sys.stderr)
        return 2
    index_path = os.path.join(src, "fssm_index.json")
    if os.path.isdir(index_path):
        print(f"ERROR: cannot write {index_path}: destination is a directory", file=sys.stderr)
        return 2
    stages = {}
    try:
        if a.carve_index is not None:
            carve_index = a.carve_index.strip()
            if not carve_index:
                print("ERROR: --carve-index must not be empty", file=sys.stderr)
                return 2
            stages = shader_stages(carve_index, "file", ".dxbc")
            shaders = sorted(os.path.join(src, name + ".dxbc") for name in stages)
            for path in shaders:
                if not os.path.isfile(path):
                    raise ValueError(f"{carve_index}: declared shader file is missing: {path}")
        else:
            with os.scandir(src) as entries:
                shaders = sorted(entry.path for entry in entries
                                 if entry.name.endswith(".dxbc") and entry.is_file())
    except (OSError, ValueError) as e:
        print(f"ERROR: cannot list shaders in {src}: {e}", file=sys.stderr)
        return 2
    out = os.path.join(src, "fssm")
    try:
        os.makedirs(out, exist_ok=True)
    except OSError as e:
        print(f"ERROR: cannot create output directory {out}: {e.strerror or e}", file=sys.stderr)
        return 2
    rows, problems = [], Counter()
    for shader in shaders:
        try:
            with open(shader, "rb") as f:
                d = f.read()
        except OSError as e:
            print(f"ERROR: cannot read {shader}: {e.strerror or e}", file=sys.stderr)
            return 2
        try:
            fssm, ents, payloads = _unpack_with_count(d, payload=a.payload)
        except Exception as e:  # noqa: BLE001
            problems[type(e).__name__] += 1
            continue
        if fssm is None:
            problems["no XBCH"] += 1
            continue
        stem = os.path.basename(shader)[:-5]
        fssm_path = os.path.join(out, stem + ".fssm")
        try:
            with open(fssm_path, "wb") as f:
                f.write(fssm)
        except OSError as e:
            print(f"ERROR: cannot write {fssm_path}: {e.strerror or e}", file=sys.stderr)
            return 2
        ends = [i for i in range(0, len(fssm) - 3, 4) if fssm[i:i + 4] == b"\x00\x00\x81\xbf"]
        rows.append({"name": stem, "size": len(fssm), "magic": fssm[:4].decode("latin-1"),
                     "version": struct.unpack_from("<H", fssm, 4)[0], "endpgm": ends,
                     "tags": sorted(hex(t) for t in ents), "xbch_payloads": payloads})
    print(f"{len(rows)} FSSM blobs, problems: {dict(problems)}")
    if not rows:
        # No index: make_game.py takes fssm_index.json as proof this step finished, and an empty one is not.
        print(f"ERROR: no shader in {src} unpacked. Check that it holds the carved .dxbc files of a supported "
              "game version.", file=sys.stderr)
        return 1
    # Stage privately and publish only after the index is complete and synced.
    target = os.path.realpath(index_path) if os.path.islink(index_path) else index_path
    stage_dir = os.path.dirname(target) or "."
    try:
        with tempfile.TemporaryDirectory(dir=stage_dir) as staging:
            tmp = os.path.join(staging, "fssm_index.json")
            with open(tmp, "w", encoding="utf-8", newline="\n") as f:
                json.dump(rows, f)
                f.write("\n")
                f.flush()
                os.fsync(f.fileno())
            os.chmod(tmp, published_mode(target))
            replace_retry(tmp, target)   # a scanner or sync client may hold tmp briefly on Windows
    except OSError as e:
        print(f"ERROR: cannot write {index_path}: {e.strerror or e}", file=sys.stderr)
        return 2
    print("magic:", dict(Counter(r["magic"] for r in rows)))
    print("s_endpgm per blob:", dict(Counter(len(r["endpgm"]) for r in rows)))
    counts = program_counts(rows, stages)
    print(f"programs: {sum(c['programs'] for c in counts.values())} tag-0x0D payloads in {len(rows)} containers, "
          f"{sum(c['multi_payload'] for c in counts.values())} containers with more than one "
          f"(--payload {a.payload} is the one unpacked)")
    if stages:  # a stage comes from the carve index; without one there is nothing to break the counts down by
        for stage in sorted(counts, key=lambda s: (s is None, s or "")):
            c = counts[stage]
            print(f"  {stage or 'stage unknown'}: {c['containers']} containers, {c['programs']} payloads, "
                  f"{c['multi_payload']} with more than one")
    sizes = sorted(r["size"] for r in rows)
    print(f"size min {sizes[0]}, median {sizes[len(sizes) // 2]}, max {sizes[-1]}, total {sum(sizes) / 2**20:.0f} MiB")
    return 0


if __name__ == "__main__":
    try:  # a redirected stdout is strict ANSI code page under -I; write '?' for what it cannot encode
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError, OSError):
        pass
    sys.exit(main())
