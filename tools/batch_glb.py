"""Batch converter from Wavefront OBJ files to binary glTF 2.0 (.glb).

Uses tools/glb_writer.py to process entire directories of OBJ models
with multi-core parallelism and progress reporting. Can optionally link
and embed textures (.png or .dds) directly into glTF materials. A texture
that cannot be read or decoded is left out of that model and reported (one
line each, counted in the summary), never dropped silently; a model that
fails to convert is named with its error. Exit status is 1 only when a
model failed. A missing input folder or an output path that cannot be created
exits 2.

Usage:
    python -I tools/batch_glb.py <in_dir> <out_dir> [--jobs N] [--up z|y] [--textures <dir>]
Usage: `python -I tools/batch_glb.py --help` lists the options.

PNG is preferred when PNG and DDS share a stem. If it cannot be loaded, its diagnostic is
retained and the converter tries the matching DDS.
A matched texture is also reported as unembedded when none of its primitives has UV coordinates.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import os
import struct
import sys
import threading
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import glb_writer
import dds_png


def _existing_texture_dir(value: str) -> str:
    """--textures must name a directory: a typo otherwise silently embedded nothing and still exited 0."""
    if not os.path.isdir(value):
        raise argparse.ArgumentTypeError(f"not a directory: {value}")
    return value


def _build_texture_index(texture_dir: str | None) -> dict[str, list[str]]:
    """Index files by lowercase stem, in PNG-before-DDS preference order."""
    index: dict[str, list[str]] = {}
    if texture_dir:
        with os.scandir(texture_dir) as entries:
            for entry in entries:
                stem, ext = os.path.splitext(entry.name.lower())
                if ext in (".png", ".dds") and entry.is_file():
                    index.setdefault(stem, []).append(entry.path)
        for paths in index.values():
            paths.sort(key=lambda path: (not path.lower().endswith(".png"), path))
    return index


_PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def _validate_png_bytes(data: bytes) -> str | None:
    if not data:
        return "the PNG file is empty"
    if len(data) < 33 or not data.startswith(_PNG_SIGNATURE):
        return "not a valid PNG header"
    pos = len(_PNG_SIGNATURE)
    chunk_idx = 0
    saw_idat = False
    while pos < len(data):
        if pos + 12 > len(data):
            return "truncated PNG chunk header"
        length = struct.unpack_from(">I", data, pos)[0]
        ctype = data[pos + 4:pos + 8]
        chunk_end = pos + 12 + length
        if chunk_end > len(data):
            return f"truncated PNG {ctype.decode('ascii', 'replace')} chunk"
        expected_crc = struct.unpack_from(">I", data, pos + 8 + length)[0]
        actual_crc = zlib.crc32(data[pos + 4:pos + 8 + length]) & 0xFFFFFFFF
        if actual_crc != expected_crc:
            return f"bad CRC in PNG {ctype.decode('ascii', 'replace')} chunk"
        if chunk_idx == 0:
            if ctype != b"IHDR" or length != 13:
                return "missing valid PNG IHDR chunk"
            width, height = struct.unpack_from(">II", data, pos + 8)
            if width <= 0 or height <= 0:
                return "PNG IHDR has zero dimension"
        elif ctype == b"IDAT":
            saw_idat = True
        elif ctype == b"IEND":
            if length != 0:
                return "non-empty PNG IEND chunk"
            if chunk_end != len(data):
                return "trailing data after PNG IEND"
            if not saw_idat:
                return "PNG has no IDAT chunk"
            return None
        pos = chunk_end
        chunk_idx += 1
    return "missing PNG IEND chunk"


def _load_png_bytes(tex_path: str, max_size: int = 0) -> tuple[bytes | None, str]:
    """(PNG bytes from file or DDS decode, or None and why it cannot be embedded: a texture the model
    can live without is skipped and reported, never a silent hole in the output)."""
    lower = tex_path.lower()
    if lower.endswith(".png"):
        try:
            with open(tex_path, "rb") as f:
                data = f.read()
        except OSError as exc:
            return None, str(exc)
        err = _validate_png_bytes(data)
        if err:
            return None, err
        return data, ""
    elif lower.endswith(".dds"):
        try:
            data = dds_png.dds_file_to_png_bytes(tex_path, max_size=max_size)
        except Exception as exc:
            return None, f"DDS decode: {exc}"
        if not data:
            return None, "DDS decode produced no bytes"
        return data, ""
    return None, "not a .png or .dds"


class TextureCache:
    """Thread-safe (bytes, reason) per texture path, so the batch decodes a broken DDS once even when two
    worker threads reach the same file at the same time (the pool runs one model per thread)."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._entries: dict[str, tuple[bytes | None, str]] = {}

    def get_or_load(self, path: str, max_size: int = 0) -> tuple[bytes | None, str]:
        with self._lock:
            if path not in self._entries:
                self._entries[path] = _load_png_bytes(path, max_size=max_size)
            return self._entries[path]


def convert_single(
    args_tuple: tuple[str, str, str, dict[str, str | list[str]], int, TextureCache],
) -> tuple[bool, str, str, list[tuple[str, str]]]:
    in_path, out_path, up_axis, tex_index, max_tex_size, tex_cache = args_tuple
    dropped: list[tuple[str, str]] = []
    dropped_seen: set[str] = set()
    try:
        primitives = glb_writer.parse_obj(in_path, is_path=True, up_axis=up_axis)
        materials: dict[str, dict] = {}

        if tex_index:
            base_stem = os.path.splitext(os.path.basename(in_path))[0].lower()
            # Clean possible hash suffix like 'ambient_femalebb._ft_at_...' -> 'ambient_femalebb'
            clean_stem = base_stem.split(".")[0]
            texture_has_uv: dict[str, bool] = {}

            for prim in primitives:
                mat_name = prim.get("material", "")
                lookup_keys = []
                if mat_name:
                    lookup_keys.extend([
                        mat_name.lower(),
                        f"{mat_name.lower()}_d",
                        f"{mat_name.lower()}_diff",
                    ])
                lookup_keys.extend([
                    base_stem,
                    clean_stem,
                    f"{clean_stem}_d",
                    f"{clean_stem}_diff",
                ])

                candidates = next((tex_index[key] for key in lookup_keys if key in tex_index), [])
                if isinstance(candidates, str):
                    candidates = [candidates]   # callers may supply a single path for each stem
                for found_path in candidates:
                    png_bytes, why_not = tex_cache.get_or_load(found_path, max_size=max_tex_size)
                    if why_not and found_path not in dropped_seen:
                        dropped_seen.add(found_path)        # one line per texture per model, not per primitive
                        dropped.append((found_path, why_not))
                    if png_bytes:
                        texture_has_uv[found_path] = texture_has_uv.get(found_path, False) or bool(prim.get("uvs"))
                        materials[mat_name] = {
                            "name": mat_name or clean_stem,
                            "png_bytes": png_bytes,
                        }
                        break
            dropped.extend((path, "matching primitives have no texture coordinates")
                           for path, has_uv in texture_has_uv.items() if not has_uv)

        glb_writer.write_glb(out_path, primitives, materials=materials if materials else None)
        return True, in_path, "", dropped
    except Exception as exc:
        return False, in_path, str(exc), dropped


def batch_convert(
    in_dir: str,
    out_dir: str,
    up_axis: str = "z",
    jobs: int | None = None,
    texture_dir: str | None = None,
    max_tex_size: int = 0,
) -> dict[str, int]:
    if not os.path.isdir(in_dir):
        raise ValueError(f"Input directory does not exist: {in_dir}")

    os.makedirs(out_dir, exist_ok=True)
    obj_files = [
        f for f in os.listdir(in_dir) if f.lower().endswith(".obj") and os.path.isfile(os.path.join(in_dir, f))
    ]

    tex_index = _build_texture_index(texture_dir)
    tex_cache = TextureCache()

    tasks = [
        (
            os.path.join(in_dir, f),
            os.path.join(out_dir, os.path.splitext(f)[0] + ".glb"),
            up_axis,
            tex_index,
            max_tex_size,
            tex_cache,
        )
        for f in obj_files
    ]

    total = len(tasks)
    succeeded = 0
    failed = 0
    failures: list[tuple[str, str]] = []          # (model, error): counted and named
    dropped_textures: list[tuple[str, str, str]] = []

    max_workers = jobs if jobs and jobs > 0 else (os.cpu_count() or 4)

    with concurrent.futures.ThreadPoolExecutor(max_workers=max_workers) as executor:
        for ok, path, err, dropped in executor.map(convert_single, tasks):
            if ok:
                succeeded += 1
            else:
                failed += 1
                failures.append((path, err))
            dropped_textures.extend((path, tex, why) for tex, why in dropped)

    return {
        "total": total,
        "succeeded": succeeded,
        "failed": failed,
        "failures": failures,
        "dropped_textures": dropped_textures,
    }


def _positive_jobs(value: str) -> int:
    jobs = int(value)
    if jobs <= 0:
        raise argparse.ArgumentTypeError(f"must be a positive integer (got {value!r})")
    return jobs


def _non_negative_int(value: str) -> int:
    num = int(value)
    if num < 0:
        raise argparse.ArgumentTypeError(f"must be >= 0 (got {value!r})")
    return num


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Batch convert Wavefront OBJ models to binary glTF 2.0 (.glb)."
    )
    parser.add_argument("in_dir", help="Directory containing .obj files")
    parser.add_argument("out_dir", help="Output directory for .glb files")
    parser.add_argument(
        "--jobs",
        type=_positive_jobs,
        default=None,
        help="Number of parallel worker threads (default: CPU count)",
    )
    parser.add_argument(
        "--up",
        choices=("z", "y"),
        default="z",
        help="Source up axis: 'z' converts RAGE Z-up to glTF Y-up (default), 'y' keeps Y-up",
    )
    parser.add_argument(
        "--textures",
        type=_existing_texture_dir,
        default=None,
        help="Optional directory containing textures (.png or .dds) to link and embed",
    )
    parser.add_argument(
        "--max-tex-size",
        type=_non_negative_int,
        default=0,
        help="Max texture dimension in pixels when decoding DDS (0 = unconstrained)",
    )

    args = parser.parse_args(argv)
    t0 = time.monotonic()
    try:
        stats = batch_convert(
            args.in_dir,
            args.out_dir,
            up_axis=args.up,
            jobs=args.jobs,
            texture_dir=args.textures,
            max_tex_size=args.max_tex_size,
        )
    except (ValueError, OSError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    elapsed = time.monotonic() - t0

    for path, err in stats["failures"]:
        print(f"batch_glb: {path}: {err}")
    for model, tex, why in stats["dropped_textures"]:
        print(f"batch_glb: {os.path.basename(model)}: texture {tex} not embedded: {why}")
    dropped = len(stats["dropped_textures"])
    note = f", {dropped} unembedded texture" + ("" if dropped == 1 else "s") if dropped else ""
    print(
        f"batch_glb: converted {stats['succeeded']}/{stats['total']} models "
        f"({stats['failed']} failed{note}) in {elapsed:.2f}s"
    )
    return 0 if stats["failed"] == 0 else 1


if __name__ == "__main__":
    sys.stdout.reconfigure(errors="replace")
    sys.exit(main())
