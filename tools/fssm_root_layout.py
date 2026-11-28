r"""Read the root-layout and used-resource tables of the native programs in an FSSM blob (stdlib only).

Each native program in an FSSM blob has a record that names two tables: where the root signature's parameters
arrive in the program's user data, and which descriptors the program reads. The record layout is the one VIctory
reads (Marvisak/VIctory, GPL-3.0, crates/victory-shader/src/native.rs); the parser here is written for this
repository from that description, and none of it has been run on a real shader yet (docs/d3d12_vulkan.md section
3.5 says what is VIctory's reading and what this corpus already supports). Offsets stored in the blob count from the
payload, which starts at 0x10:

    blob    +0x00  'FSSM' u32 version (0x101), u64 payload size (blob size - 0x10)
    payload +0x00  u64 6
            +0x08  u64 record offset per hardware stage: LS, HS, ES, GS, VS, PS at +0x08..+0x30, mesh at +0x48,
                   compute at +0x60; 0xDEADBEEFDEADBEEF for a stage the blob has no program for
    record  +0x00  u64 code offset, u64 code size
            +0x28  u64 root-layout table offset, +0x30 u64 used-resource table offset
            +0x38  u32 root-layout entry count, +0x3c u32 used-resource entry count (the record is 0x54 bytes)
    root-layout entry, 20 bytes
            +0x00  u32 root type (0 constants, 1 root descriptor, 3 descriptor table, 4 immediate sampler,
                   5 resource heap, 6 sampler heap)
            +0x04  u32 offset of the range in its table, in dwords
            +0x08  u32 user-data dwords the parameter takes
            +0x0c  u32 shader register of the first descriptor
            +0x10  u8 first user-data dword, u8 root parameter index, u16 0
    used-resource entry, 20 bytes
            +0x00  u8 resource type (0 SRV, 1 sampler, 2 CBV, 3 UAV), u8 root type, u16 0
            +0x04  u32 offset in the table, in dwords, +0x08 u32 lower-bound register, +0x0c u32 stride
            +0x10  u8 dwords per descriptor, u8 first user-data dword, u8 root parameter index, u8 0

User-data dword n of a program is SGPR n in the VS, PS and compute stages and SGPR n + 8 in the merged and mesh
stages (LS, HS, ES, GS, MS), which have 8 system SGPRs first: `user_sgpr()`.

This module is on its own on purpose: the translator and make_game.py do not import it, so it is not one of the
translation sources and changing it never makes a shader library translate again. `tools/root_layout_census.py`
counts the tables over a library; this file's own command line writes them out.

Usage:
    python -I tools/fssm_root_layout.py <fssm file, dxbc file, or folder of either> [--out FILE]
        (prints one JSON object, a file per line: file name -> one entry per FSSM blob in it, each
         {"programs": root_tables()} or {"error": why its tables could not be read}. A bare .fssm has one blob, a
         container one per tag-0x0D payload. In a folder, a .dxbc with a .fssm of the same name beside it is
         skipped. --out writes the object to FILE instead and prints a one-line summary; it is large for a whole
         library. These are the user's own files: there are no game files in this repository.)

A path that does not exist, a folder or file that cannot be read and an --out path that cannot be written each give
one ERROR line and exit 2. A folder with no .fssm or .dxbc file prints "no input" and exits 0.
"""
import argparse
import json
import os
import struct
import sys
import zlib
from collections import Counter

# -I keeps the script directory off sys.path, so add it explicitly before importing the sibling modules.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fssm_unpack  # noqa: E402
from report_paths import same_report_path  # noqa: E402

FSSM_VERSION = 0x101
FSSM_PAYLOAD = 0x10                  # stored offsets count from here
FSSM_GRAPHICS_SLOTS = 6              # the u64 that starts the payload
FSSM_STAGE_SLOTS = (("ls", 0x08), ("hs", 0x10), ("es", 0x18), ("gs", 0x20), ("vs", 0x28), ("ps", 0x30),
                    ("ms", 0x48), ("cs", 0x60))                 # hardware stage, payload offset of its record offset
FSSM_SLOT_TABLE_END = 0x68
FSSM_ABSENT = 0xDEADBEEFDEADBEEF
FSSM_RECORD_SIZE = 0x54
FSSM_RECORD_TABLES = 0x28            # u64 root-layout offset, u64 used-resource offset, u32 and u32 their counts
ROOT_ENTRY_SIZE = 20
ROOT_LAYOUT_ENTRY = struct.Struct("<IIIIBBH")
USED_RESOURCE_ENTRY = struct.Struct("<BBHIIIBBBB")
LAYOUT_KEYS = ("root_type", "table_offset", "user_sgprs", "base_register", "user_data", "root_slot", "reserved")
USED_KEYS = ("resource_type", "root_type", "reserved", "table_offset", "lower_bound", "stride", "descriptor_dwords",
             "user_data", "root_slot", "reserved_high")   # root_tables() folds reserved_high into reserved
ROOT_TYPES = {0: "constants", 1: "descriptor", 3: "table", 4: "immediate_sampler", 5: "resource_heap",
              6: "sampler_heap"}
RESOURCE_TYPES = {0: "srv", 1: "sampler", 2: "cbv", 3: "uav"}
SYSTEM_SGPRS = {"ls": 8, "hs": 8, "es": 8, "gs": 8, "ms": 8, "vs": 0, "ps": 0, "cs": 0}   # before the user data


class FssmLayoutError(ValueError):
    """An FSSM blob whose program records or root tables are not laid out as the module docstring describes.

    The message names the stage and what is wrong, without offsets, so a census can count blobs by message."""


def _fits(size: int, offset: int, length: int) -> bool:
    """offset + length <= size, for a stored 64-bit offset that may be anything."""
    return offset <= size and length <= size - offset


def _overlap(a: tuple, b: tuple) -> bool:
    """Do two non-empty (offset, length) ranges share a byte?"""
    return a[0] < b[0] + b[1] and b[0] < a[0] + a[1]


def root_type_name(value: int) -> str:
    return ROOT_TYPES.get(value, f"root{value}")


def resource_type_name(value: int) -> str:
    return RESOURCE_TYPES.get(value, f"resource{value}")


def user_sgpr(stage: str, dword: int) -> int:
    """The SGPR that holds user-data dword `dword` on entry to a program of this hardware stage."""
    return SYSTEM_SGPRS[stage] + dword


def _entries(fssm: bytes, span, entry: struct.Struct, keys: tuple) -> list:
    """The 20-byte entries of one table as dicts; span is the checked (payload offset, length) or None."""
    if not span:
        return []
    start = FSSM_PAYLOAD + span[0]
    return [dict(zip(keys, entry.unpack_from(fssm, at))) for at in range(start, start + span[1], ROOT_ENTRY_SIZE)]


def root_tables(fssm: bytes) -> list:
    """The native programs of one FSSM blob with their root-layout and used-resource tables, in slot order.

    One dict per program: `stage` (the hardware stage of the slot that names the record), `record`, `code_offset`,
    `layout_offset` and `used_offset` (offsets in the blob, 0x10 more than the stored ones; None for a table with
    no entries), `code_size`, `layout` and `used`. A `layout` entry has `root_type`, `table_offset`, `user_sgprs`,
    `base_register`, `user_data`, `root_slot` and `reserved`; a `used` entry has `resource_type`, `root_type`,
    `table_offset`, `lower_bound`, `stride`, `descriptor_dwords`, `user_data`, `root_slot` and `reserved` (the u16
    at +2, with the u8 at +19 above it). The values are the stored ones: a type this reading has no name for and a
    non-zero reserved field are kept, not refused, so that a census can count them.

    Raises FssmLayoutError when the header is not version 0x101 with a matching payload size, when a record or a
    program's code lies outside the payload, when records overlap each other or the slot table, or when a table
    runs past the payload or overlaps a record, its program's code or the program's other table. A table with a
    count of 0 is empty whatever its offset is. Nothing outside the blob is read."""
    if len(fssm) < FSSM_PAYLOAD + FSSM_SLOT_TABLE_END:
        raise FssmLayoutError("truncated FSSM header")
    if fssm[:4] != b"FSSM":
        raise FssmLayoutError("not an FSSM blob")
    version, stored_size, slots = struct.unpack_from("<IQQ", fssm, 4)
    if version != FSSM_VERSION:
        raise FssmLayoutError(f"unknown FSSM version 0x{version:x}")
    size = len(fssm) - FSSM_PAYLOAD
    if stored_size != size:
        raise FssmLayoutError("FSSM payload size disagrees with the blob size")
    if slots != FSSM_GRAPHICS_SLOTS:
        raise FssmLayoutError("unexpected FSSM slot count")
    records = []
    for stage, slot in FSSM_STAGE_SLOTS:
        start = struct.unpack_from("<Q", fssm, FSSM_PAYLOAD + slot)[0]
        if start == FSSM_ABSENT:
            continue
        if start < FSSM_SLOT_TABLE_END or not _fits(size, start, FSSM_RECORD_SIZE):
            raise FssmLayoutError(f"{stage} record lies outside the payload")
        records.append((stage, start))
    starts = sorted(start for _, start in records)
    if any(b - a < FSSM_RECORD_SIZE for a, b in zip(starts, starts[1:])):
        raise FssmLayoutError("program records overlap")
    out = []
    for stage, start in records:
        record = FSSM_PAYLOAD + start
        code_offset, code_size = struct.unpack_from("<QQ", fssm, record)
        if not _fits(size, code_offset, code_size):
            raise FssmLayoutError(f"{stage} code runs past the payload")
        offsets = struct.unpack_from("<QQ", fssm, record + FSSM_RECORD_TABLES)
        counts = struct.unpack_from("<II", fssm, record + FSSM_RECORD_TABLES + 16)
        tables = []
        for what, offset, count in zip(("root-layout", "used-resource"), offsets, counts):
            if not count:
                tables.append(None)
                continue
            span = (offset, ROOT_ENTRY_SIZE * count)
            if not _fits(size, *span):
                raise FssmLayoutError(f"{stage} {what} table runs past the payload")
            if offset < FSSM_SLOT_TABLE_END or any(_overlap(span, (r, FSSM_RECORD_SIZE)) for r in starts):
                raise FssmLayoutError(f"{stage} {what} table overlaps a program record")
            if code_size and _overlap(span, (code_offset, code_size)):
                raise FssmLayoutError(f"{stage} {what} table overlaps the program's code")
            tables.append(span)
        if tables[0] and tables[1] and _overlap(*tables):
            raise FssmLayoutError(f"{stage} root-layout and used-resource tables overlap")
        layout = _entries(fssm, tables[0], ROOT_LAYOUT_ENTRY, LAYOUT_KEYS)
        used = _entries(fssm, tables[1], USED_RESOURCE_ENTRY, USED_KEYS)
        for entry in used:
            entry["reserved"] |= entry.pop("reserved_high") << 16
        out.append({"stage": stage, "record": record, "code_offset": FSSM_PAYLOAD + code_offset,
                    "code_size": code_size,
                    "layout_offset": FSSM_PAYLOAD + tables[0][0] if tables[0] else None,
                    "used_offset": FSSM_PAYLOAD + tables[1][0] if tables[1] else None,
                    "layout": layout, "used": used})
    return out


def files_of(path: str) -> list:
    """Files to read: every .fssm, plus a .dxbc whose stem has no .fssm beside it (as tools/pal_stages.py)."""
    if os.path.isfile(path):
        return [path]
    with os.scandir(path) as entries:
        names = sorted(entry.path for entry in entries if entry.is_file())
    fssm = [name for name in names if name.endswith(".fssm")]
    taken = {os.path.splitext(os.path.basename(name))[0] for name in fssm}
    dxbc = [name for name in names
            if name.endswith(".dxbc") and os.path.splitext(os.path.basename(name))[0] not in taken]
    return sorted(fssm + dxbc)


def blobs_of(data: bytes) -> list:
    """The FSSM blobs of one file's bytes: the file itself, or every tag-0x0D payload of a DXBC container.
    ValueError when it is neither."""
    if data[:4] == b"FSSM":
        return [data]
    try:
        return fssm_unpack.unpack_all(data)
    except (ValueError, struct.error, IndexError, zlib.error):
        raise ValueError("not an FSSM blob or a container that unpacks") from None


def write_root_layouts(f, files: list) -> tuple:
    """Write the root tables of `files` to the text file `f` as one JSON object, a file per line: file name -> a
    list with one entry per FSSM blob in the file, {"programs": root_tables()} or {"error": message} (a file that
    holds no blob has one error entry). One file is in memory at a time. Returns (counts of files, blobs read,
    programs, layout and used entries; blobs or files not read per message). OSError when a file cannot be read
    or `f` cannot be written."""
    counts, problems = Counter(), Counter()
    f.write("{")
    for k, path in enumerate(files):
        with open(path, "rb") as source:
            data = source.read()
        counts["files"] += 1
        try:
            blobs = blobs_of(data)
            if not blobs:
                raise ValueError("a container with no tag-0x0D payload")
        except ValueError as e:
            blobs, entries = [], [{"error": str(e)}]
            problems[str(e)] += 1
        else:
            entries = []
        for blob in blobs:
            try:
                programs = root_tables(blob)
            except FssmLayoutError as e:
                entries.append({"error": str(e)})
                problems[str(e)] += 1
                continue
            entries.append({"programs": programs})
            counts["blobs"] += 1
            counts["programs"] += len(programs)
            counts["layout"] += sum(len(p["layout"]) for p in programs)
            counts["used"] += sum(len(p["used"]) for p in programs)
        f.write(("," if k else "") + "\n" + json.dumps(os.path.basename(path)) + ": " + json.dumps(entries))
    f.write("\n}\n")
    f.flush()
    return counts, problems


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("path", help="an .fssm file, a DXBC container, or a folder of either (your own files)")
    ap.add_argument("--out", help="write the JSON here instead of printing it")
    a = ap.parse_args(argv)
    if a.out and same_report_path(a.path, a.out):
        ap.error("--out must not overwrite the input")
    if not os.path.exists(a.path):
        print(f"ERROR: {a.path} does not exist", file=sys.stderr)
        return 2
    try:
        files = files_of(a.path)
    except OSError as e:
        print(f"ERROR: cannot list {a.path}: {e.strerror or e}", file=sys.stderr)
        return 2
    if not files:
        print(f"no input: {a.path} holds no .fssm or .dxbc file", file=sys.stderr)
        return 0
    try:
        if a.out:
            with open(a.out, "w", encoding="utf-8", newline="\n") as f:
                counts, problems = write_root_layouts(f, files)
        else:
            counts, problems = write_root_layouts(sys.stdout, files)
    except OSError as e:
        print(f"ERROR: cannot read or write {e.filename or a.out or a.path}: {e.strerror or e}", file=sys.stderr)
        return 2
    # Beside the JSON on stdout the summary would break the document, so it goes to stderr there.
    print(f"root layout: {counts['programs']} programs in {counts['blobs']} blobs of {counts['files']} files "
          f"({counts['layout']} root-layout and {counts['used']} used-resource entries), not read: "
          f"{dict(sorted(problems.items()))}", file=sys.stdout if a.out else sys.stderr)
    return 0


if __name__ == "__main__":
    try:  # a redirected stdout is strict ANSI code page under -I; write '?' for what it cannot encode
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError, OSError):
        pass
    sys.exit(main())
