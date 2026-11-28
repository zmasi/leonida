"""Inspect a PE image (exe or dll) for porting work.

Usage:
    python -I pe_inspect.py <image> [--game-dir DIR] [--out DIR]

Writes <out>/<file>.json and <out>/<file>.md, where <file> is the image's full file name (game.exe.json,
game.dll.md), so an exe and a dll with the same stem get separate reports. Images with the same file name from
different directories get a numbered suffix (x.exe-2.json), and the name used is printed to stderr. The same file
given twice with the same import search folder is inspected once. Reads the file only; never runs it.

The report also lists run-time load candidates: DLL names the image holds as ASCII or UTF-16 strings without
importing them, where a LoadLibrary* the import table doesn't show would point (docs/runtime_imports.md).

Each DLL's "where" is bundled (in the game folder, any case), system, shim or, on a host that isn't Windows and has
no Wine prefix, unverified (classify_dll). On Windows, system means System32 or the OS API-set schema; elsewhere the
Wine prefix's system32 ($WINEPREFIX, else ~/.wine) and its apisetschema.dll stand in, and the report's
"system_dlls" says which was used.

Standard library only: the headers, the section table, the import, delay-import, export, TLS and relocation
directories are parsed with struct, so the tool runs under `python -I` without third-party packages and is covered
by the same standard-library-only CI job as the other tools.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import re
import struct
import sys
from collections import Counter
from collections.abc import Iterator

SYSTEM32 = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32")
# Whether this host is Windows: there SYSTEM32 and the OS API-set schema answer `system`; elsewhere a Wine prefix's
# system32 does (classify_dll). A module flag so that a test can stand in for either host.
IS_WINDOWS = os.name == "nt"
RUNTIME_SHIMS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "runtime", "shims")

MACHINES = {0x8664: "x86-64", 0x14C: "x86", 0xAA64: "ARM64"}
SUBSYSTEMS = {2: "GUI", 3: "Console", 14: "Xbox"}

DIR_EXPORT = 0
DIR_IMPORT = 1
DIR_EXCEPTION = 3
DIR_BASERELOC = 5
DIR_TLS = 9
DIR_DELAY_IMPORT = 13

MAX_ENTRIES = 65536  # a table this long is not a table; stops a malformed image from looping forever


class PeFormatError(ValueError):
    """The file is not a PE image this parser can read."""


def entropy(data: bytes) -> float:
    if not data:
        return 0.0
    n = len(data)
    return -sum(c / n * math.log2(c / n) for c in Counter(data).values())


# An MSVC decorated name can be 4,096 characters (the documented limit of a decorated name); with the 512 this
# was, a long template import became "?" and a DLL with a long name lost its whole entry.
NAME_LIMIT = 4097


def _cstring(data: bytes, offset: int, limit: int = NAME_LIMIT) -> str | None:
    end = data.find(b"\0", offset, min(len(data), offset + limit))
    if end < 0:
        return None
    return data[offset:end].decode(errors="replace")


class PeImage:
    """The parts of a PE file this tool reports on, read in place from the file bytes."""

    def __init__(self, data: bytes):
        self.data = data
        if data[:2] != b"MZ":
            raise PeFormatError("no MZ header")
        if len(data) < 0x40:
            # The MZ signature is there; the file is just too short to carry the MS-DOS header whose dword at
            # 0x3c holds e_lfanew. Saying "no MZ header" here would blame a signature that is present.
            raise PeFormatError(f"file is {len(data)} bytes, shorter than the 64-byte MS-DOS header")
        self.nt = struct.unpack_from("<I", data, 0x3C)[0]
        if self.nt + 24 > len(data):
            raise PeFormatError(f"file is {len(data)} bytes, too short to hold the PE header at 0x{self.nt:x}")
        if data[self.nt:self.nt + 4] != b"PE\0\0":
            raise PeFormatError("no PE signature")
        (self.machine, self.section_count, self.timestamp, _symbol_table, _symbols, opt_size,
         self.characteristics) = struct.unpack_from("<HHIIIHH", data, self.nt + 4)
        opt = self.nt + 24
        if opt + 2 > len(data):
            raise PeFormatError("truncated optional header")
        magic = struct.unpack_from("<H", data, opt)[0]
        if magic not in (0x10B, 0x20B):
            raise PeFormatError(f"unknown optional header magic 0x{magic:x}")
        # The declared optional header must contain every fixed field we read,
        # and the complete declared header must fit inside the file.
        if opt_size < 72 or opt + opt_size > len(data):
            raise PeFormatError("truncated optional header")
        self.is64 = magic == 0x20B
        self.entry_rva = struct.unpack_from("<I", data, opt + 16)[0]
        self.image_base = struct.unpack_from("<Q" if self.is64 else "<I", data, opt + (24 if self.is64 else 28))[0]
        self.size_of_image = struct.unpack_from("<I", data, opt + 56)[0]
        self.size_of_headers = struct.unpack_from("<I", data, opt + 60)[0]
        self.subsystem = struct.unpack_from("<H", data, opt + 68)[0]
        self.dll_characteristics = struct.unpack_from("<H", data, opt + 70)[0]

        # NumberOfRvaAndSizes counts the data directories: a slot past it is not a directory, whatever bytes it
        # holds (runtime/src/pe_loader.cpp reads them the same way). The count is the dword before the table.
        opt_end = min(len(data), opt + opt_size)
        dirs = opt + (112 if self.is64 else 96)
        count = struct.unpack_from("<I", data, dirs - 4)[0] if dirs <= opt_end else 0
        self.directories = []
        for i in range(16):
            at = dirs + i * 8
            present = i < count and at + 8 <= opt_end
            self.directories.append(struct.unpack_from("<II", data, at) if present else (0, 0))

        # A section's data can be virtual only, so lookups past SizeOfRawData have no file bytes to read.
        self.sections = []
        table = opt + opt_size
        for i in range(self.section_count):
            at = table + i * 40
            if at + 40 > len(data):
                raise PeFormatError("truncated section table")
            vsize, rva, rawsize, rawptr = struct.unpack_from("<IIII", data, at + 8)
            chars = struct.unpack_from("<I", data, at + 36)[0]
            self.sections.append({
                "name": data[at:at + 8].rstrip(b"\0").decode(errors="replace"),
                "rva": rva, "vsize": vsize, "rawsize": rawsize, "rawptr": rawptr,
                "exec": bool(chars & 0x20000000), "write": bool(chars & 0x80000000),
            })

    def directory(self, index: int) -> tuple[int, int]:
        return self.directories[index] if index < len(self.directories) else (0, 0)

    def import_descriptors(self, index: int, *, delay: bool = False) -> Iterator[tuple[int, int, int, bool]]:
        """Yield (dll_rva, iat_rva, lookup_rva, va_entries) for each import descriptor."""
        rva, size = self.directory(index)
        if not rva or not size:
            return

        step = 32 if delay else 20
        for i in range(min(size // step, MAX_ENTRIES)):
            at = self.offset(rva + i * step, step)
            if at is None:
                break
            va_entries = False
            if delay:
                attributes, dll_rva, _handle, iat_rva, lookup_rva = struct.unpack_from("<5I", self.data, at)
                if not dll_rva:
                    break
                # With dlattrRva clear, these DWORDs and named lookup entries hold legacy VAs.
                if not attributes & 1:
                    va_entries = True
                    dll_rva -= self.image_base
                    iat_rva -= self.image_base
                    lookup_rva -= self.image_base
            else:
                lookup_rva, _stamp, _forwarder, dll_rva, iat_rva = struct.unpack_from("<5I", self.data, at)
                if not dll_rva:
                    break
            yield dll_rva, iat_rva, lookup_rva, va_entries

    def _range(self, rva: int) -> tuple[int, int] | None:
        """Contiguous file [start, end) backed by headers or the single section holding `rva`."""
        if rva < 0:
            return None
        if rva < self.size_of_headers:
            end = min(self.size_of_headers, len(self.data))
            return (rva, end) if rva < end else None
        for s in self.sections:
            # A section is VirtualSize bytes in memory (SizeOfRawData when that is 0). File bytes past it are not
            # mapped, as in runtime/src/pe_loader.cpp: with SizeOfRawData larger than the span to the next
            # section, max(vsize, rawsize) answered the next section's RVAs from this one's raw tail.
            span = s["vsize"] or s["rawsize"]
            if s["rva"] <= rva < s["rva"] + span:
                delta = rva - s["rva"]
                mapped = min(s["rawsize"], span)
                if delta >= mapped:
                    return None
                at = s["rawptr"] + delta
                end = min(s["rawptr"] + mapped, len(self.data))
                return (at, end) if at < end else None
        return None

    def offset(self, rva: int, size: int = 1) -> int | None:
        """File offset of an RVA span wholly backed by headers or one section."""
        if size <= 0:
            return None
        bounds = self._range(rva)
        if bounds is None:
            return None
        start, end = bounds
        return start if start + size <= end else None

    def section_data(self, section: dict) -> bytes:
        at = section["rawptr"]
        if at >= len(self.data):
            return b""
        return self.data[at:at + min(section["rawsize"], len(self.data) - at)]

    def string_at(self, rva: int) -> str | None:
        bounds = self._range(rva)
        if bounds is None:
            return None
        start, end = bounds
        return _cstring(self.data, start, min(NAME_LIMIT, end - start))

    def word(self, at: int) -> int:
        return struct.unpack_from("<Q" if self.is64 else "<I", self.data, at)[0]

    def thunk_names(self, lookup_rva: int, *, va_entries: bool = False) -> list[str]:
        """Names (or '#ordinal') of one lookup table, which is terminated by a zero entry."""
        ordinal_flag = 1 << (63 if self.is64 else 31)
        out = []
        width = 8 if self.is64 else 4
        for i in range(MAX_ENTRIES):
            at = self.offset(lookup_rva + i * width, width)
            if at is None:
                break
            value = self.word(at)
            if not value:
                break
            if value & ordinal_flag:
                out.append(f"#{value & 0xFFFF}")
                continue
            if va_entries:
                value -= self.image_base
                if value <= 0:
                    out.append("?")
                    continue
            name = self.string_at(value + 2)  # skip the 2-byte hint
            out.append(name if name is not None else "?")
        return out


def _imports(img: PeImage, kind: str, index: int, delay: bool = False) -> list[dict]:
    entries = []
    for dll_rva, iat_rva, int_rva, va_entries in img.import_descriptors(index, delay=delay):
        dll = img.string_at(dll_rva) if dll_rva > 0 else None
        if dll is None:
            continue
        lookup = int_rva if int_rva > 0 else iat_rva
        entries.append({"dll": dll, "kind": kind,
                        "functions": img.thunk_names(lookup, va_entries=va_entries) if lookup > 0 else []})
    return entries


def _exports(img: PeImage) -> list[str]:
    rva, size = img.directory(DIR_EXPORT)
    if not rva or size < 40:
        return []
    at = img.offset(rva, 40)
    if at is None:
        start = img.offset(rva)
        if start is not None and start + 40 > len(img.data):
            raise PeFormatError("truncated export directory")
        return []
    (_chars, _stamp, _major, _minor, _name, base, functions, name_count, addr_functions,
     addr_names, addr_ordinals) = struct.unpack_from("<IIHHIIIIIII", img.data, at)

    def has_export_at(index: int) -> bool:
        if index >= functions:
            return False
        # A zero RVA marks an unused slot. An RVA inside the export directory is a valid forwarder.
        slot = img.offset(addr_functions + index * 4, 4)
        return slot is not None and struct.unpack_from("<I", img.data, slot)[0] != 0

    named, names = set(), []
    for i in range(min(name_count, MAX_ENTRIES)):
        name_at = img.offset(addr_names + i * 4, 4)
        ord_at = img.offset(addr_ordinals + i * 2, 2)
        if name_at is None or ord_at is None:
            break
        index = struct.unpack_from("<H", img.data, ord_at)[0]
        if not has_export_at(index):
            continue
        symbol = img.string_at(struct.unpack_from("<I", img.data, name_at)[0])
        names.append(symbol if symbol is not None else "?")
        named.add(index)
    for i in range(min(functions, MAX_ENTRIES)):
        if i not in named and has_export_at(i):
            names.append(f"#{base + i}")
    return names


# The resource directory (data directory 2) and its RT_VERSION block. The version block names the build, and its
# values are free UTF-16 text that can name the title, so the report never holds a value: the fixed info's numbers,
# the languages, the standard key names and, per value, its length and SHA-256. Neither are the names of named
# resource types or of a key outside VERSION_KEYS, which are counted or hashed instead. A damaged tree or block is
# reported in "problems" (offsets and counts only) and never stops the report.
DIR_RESOURCE = 2
RT_VERSION = 16
RT_NAMES = {1: "CURSOR", 2: "BITMAP", 3: "ICON", 4: "MENU", 5: "DIALOG", 6: "STRING", 7: "FONTDIR", 8: "FONT",
            9: "ACCELERATOR", 10: "RCDATA", 11: "MESSAGETABLE", 12: "GROUP_CURSOR", 14: "GROUP_ICON", 16: "VERSION",
            17: "DLGINCLUDE", 19: "PLUGPLAY", 20: "VXD", 21: "ANICURSOR", 22: "ANIICON", 23: "HTML", 24: "MANIFEST"}
# The StringFileInfo keys the VERSIONINFO documentation defines; any other key is reported by its hash.
VERSION_KEYS = frozenset({"Comments", "CompanyName", "FileDescription", "FileVersion", "InternalName",
                          "LegalCopyright", "LegalTrademarks", "OriginalFilename", "PrivateBuild", "ProductName",
                          "ProductVersion", "SpecialBuild"})
VS_FIXED_SIGNATURE = 0xFEEF04BD
VS_FF = {0x01: "DEBUG", 0x02: "PRERELEASE", 0x04: "PATCHED", 0x08: "PRIVATEBUILD", 0x10: "INFOINFERRED",
         0x20: "SPECIALBUILD"}
VOS = {0x00000: "UNKNOWN", 0x00004: "WINDOWS32", 0x10000: "DOS", 0x40000: "NT", 0x10001: "DOS_WINDOWS16",
       0x10004: "DOS_WINDOWS32", 0x40004: "NT_WINDOWS32"}
VFT = {0: "UNKNOWN", 1: "APP", 2: "DLL", 3: "DRV", 4: "FONT", 5: "VXD", 7: "STATIC_LIB"}
VERSION_KEY_LIMIT = 256  # characters; a block key longer than this is not a key


def _sha256(text: str) -> str:
    import hashlib  # here, not with the imports at the top, which other open changes edit
    return hashlib.sha256(text.encode("utf-8", "surrogatepass")).hexdigest()


def _align4(at: int) -> int:
    return (at + 3) & ~3


def _vs_block(data: bytes, at: int, end: int) -> dict | None:
    """One version block (wLength, wValueLength, wType, szKey, Value, Children) at `at`, read no further than `end`:
    the key, where its value and children start and where it ends. None when the header or key doesn't fit, or the
    length is shorter than a header."""
    if at + 6 > end:
        return None
    length, value_length, kind = struct.unpack_from("<HHH", data, at)
    if length < 6:
        return None
    stop = min(at + length, end)
    key_end = None
    for i in range(at + 6, min(stop - 1, at + 6 + 2 * VERSION_KEY_LIMIT), 2):
        if data[i] == 0 and data[i + 1] == 0:
            key_end = i
            break
    if key_end is None:
        return None
    value = _align4(key_end + 2)
    value_size = value_length * 2 if kind == 1 else value_length
    return {"key": data[at + 6:key_end].decode("utf-16-le", errors="replace"), "kind": kind,
            "value": min(value, stop), "value_end": min(value + value_size, stop),
            "children": min(_align4(value + value_size), stop), "end": stop, "cut": at + length > end}


def _vs_children(data: bytes, block: dict, what: str, problems: list[str]) -> list[dict]:
    """The child blocks of `block`. Zero bytes after the last child are padding; a child that can't be read ends the
    list with a problem."""
    out, at = [], block["children"]
    while at + 6 <= block["end"] and len(out) < MAX_ENTRIES:
        if not struct.unpack_from("<H", data, at)[0]:
            break
        child = _vs_block(data, at, block["end"])
        if child is None:
            problems.append(f"version block: {what} child at +{at:#x} is malformed")
            break
        if child["cut"]:
            problems.append(f"version block: {what} child at +{at:#x} runs past its parent")
        out.append(child)
        at = _align4(child["end"])
    return out


def _version_info(data: bytes, problems: list[str]) -> dict:
    """VS_VERSIONINFO read from the bytes of one RT_VERSION resource. String values are returned as length and
    SHA-256 (of the value as UTF-8, without its terminating NUL), never as text."""
    out = {"fixed": None, "translations": [], "string_tables": [], "string_count": 0, "other_blocks": 0}
    root = _vs_block(data, 0, len(data))
    if root is None:
        problems.append("version block: VS_VERSIONINFO header is malformed")
        return out
    if root["cut"]:
        problems.append(f"version block: declares more bytes than the resource's {len(data)}")
    if root["key"] != "VS_VERSION_INFO":
        problems.append("version block: key is not VS_VERSION_INFO")
    fixed_at, fixed_end = root["value"], root["value_end"]
    if fixed_end - fixed_at >= 52:
        (signature, struct_version, file_ms, file_ls, product_ms, product_ls, flags_mask, flags, os_id, file_type,
         subtype, date_ms, date_ls) = struct.unpack_from("<13I", data, fixed_at)
        if signature != VS_FIXED_SIGNATURE:
            problems.append(f"version block: VS_FIXEDFILEINFO signature {signature:#x} is not "
                            f"{VS_FIXED_SIGNATURE:#x}")
        else:
            out["fixed"] = {
                "file_version": f"{file_ms >> 16}.{file_ms & 0xFFFF}.{file_ls >> 16}.{file_ls & 0xFFFF}",
                "product_version": f"{product_ms >> 16}.{product_ms & 0xFFFF}.{product_ls >> 16}."
                                   f"{product_ls & 0xFFFF}",
                "struct_version": f"{struct_version >> 16}.{struct_version & 0xFFFF}",
                "flags_mask": flags_mask, "flags": flags,
                "flag_names": [name for bit, name in VS_FF.items() if flags & flags_mask & bit],
                "os": os_id, "os_name": VOS.get(os_id),
                "type": file_type, "type_name": VFT.get(file_type), "subtype": subtype,
                "date": (date_ms << 32) | date_ls,
            }
    elif fixed_end > fixed_at:
        problems.append(f"version block: VS_FIXEDFILEINFO is {fixed_end - fixed_at} bytes, not 52")
    for child in _vs_children(data, root, "VS_VERSIONINFO", problems):
        if child["key"] == "StringFileInfo":
            for table in _vs_children(data, child, "StringFileInfo", problems):
                key = table["key"]
                lang = key.lower() if re.fullmatch(r"[0-9A-Fa-f]{8}", key) else None
                if lang is None:
                    problems.append("version block: a string table's key is not 8 hex digits")
                strings = []
                for s in _vs_children(data, table, "string table", problems):
                    raw = data[s["value"]:s["end"]]
                    value = raw[:len(raw) & ~1].decode("utf-16-le", errors="replace").split("\0", 1)[0]
                    entry = {"key": s["key"]} if s["key"] in VERSION_KEYS else {"key": None,
                                                                               "key_sha256": _sha256(s["key"])}
                    entry.update({"chars": len(value), "sha256": _sha256(value)})
                    strings.append(entry)
                out["string_tables"].append({"lang_codepage": lang, "strings": strings})
                out["string_count"] += len(strings)
        elif child["key"] == "VarFileInfo":
            for var in _vs_children(data, child, "VarFileInfo", problems):
                if var["key"] != "Translation":
                    out["other_blocks"] += 1
                    continue
                for at in range(var["value"], var["value_end"] - 3, 4):
                    lang, codepage = struct.unpack_from("<HH", data, at)
                    out["translations"].append(f"{lang:04x}{codepage:04x}")
        else:
            out["other_blocks"] += 1
    return out


def _resources(img: PeImage) -> dict:
    """The resource directory: the types it holds and the first RT_VERSION block (_version_info). The tree is read
    three levels deep (type, name, language) inside the file bytes the directory's RVA maps to; a directory reached
    a second time (a loop, or two entries sharing a subdirectory) is not read again."""
    out = {"present": False, "types": [], "named_types": 0, "version": None, "problems": []}
    problems = out["problems"]
    rva, size = img.directory(DIR_RESOURCE)
    if not rva or not size:
        return out
    out["present"] = True
    bounds = img._range(rva)
    if bounds is None:
        problems.append(f"resource directory RVA {rva:#x} has no file bytes")
        return out
    base, end = bounds
    data = img.data
    seen: set[int] = set()
    budget = [MAX_ENTRIES]

    def entries(offset: int, level: str) -> list[tuple[int | None, int, bool]]:
        """(id, None for a named entry; child offset; child is a directory) of the directory at `offset`."""
        if offset in seen:
            problems.append(f"resource tree: {level} directory at +{offset:#x} is reached a second time "
                            "(a loop or a shared subdirectory)")
            return []
        seen.add(offset)
        at = base + offset
        if at + 16 > end:
            problems.append(f"resource tree: {level} directory at +{offset:#x} is past the end of its file bytes")
            return []
        named, ids = struct.unpack_from("<HH", data, at + 12)
        rows = []
        for i in range(named + ids):
            entry = at + 16 + i * 8
            if entry + 8 > end:
                problems.append(f"resource tree: {level} directory at +{offset:#x} is cut short after {i} of "
                                f"{named + ids} entries")
                break
            if budget[0] <= 0:
                problems.append(f"resource tree: more than {MAX_ENTRIES} entries; the rest are not read")
                break
            budget[0] -= 1
            name, child = struct.unpack_from("<II", data, entry)
            rows.append((None if name & 0x80000000 else name, child & 0x7FFFFFFF, bool(child & 0x80000000)))
        return rows

    # (type id or None, language, data entry offset) of every leaf, in tree order
    leaves: list[tuple[int | None, int | None, int]] = []
    for type_id, type_at, type_dir in entries(0, "type"):
        if type_id is None:
            out["named_types"] += 1
        if not type_dir:
            problems.append(f"resource tree: a type entry points at data (+{type_at:#x}), not a directory")
            continue
        for _name, name_at, name_dir in entries(type_at, "name"):
            if not name_dir:
                leaves.append((type_id, None, name_at))
                continue
            for lang, leaf_at, leaf_dir in entries(name_at, "language"):
                if leaf_dir:
                    problems.append(f"resource tree: a language entry points at a directory (+{leaf_at:#x}); "
                                    "only three levels are read")
                    continue
                leaves.append((type_id, lang, leaf_at))

    types: dict[int | None, dict] = {}
    for type_id, lang, leaf_at in leaves:
        at = base + leaf_at
        name = "(named)" if type_id is None else RT_NAMES.get(type_id, f"#{type_id}")
        row = types.setdefault(type_id, {"type": name, "id": type_id, "entries": 0, "bytes": 0})
        row["entries"] += 1
        if at + 16 > end:
            problems.append(f"resource tree: data entry at +{leaf_at:#x} is past the end of its file bytes")
            continue
        data_rva, data_size, codepage = struct.unpack_from("<III", data, at)
        row["bytes"] += data_size
        if type_id != RT_VERSION or out["version"] is not None:
            continue
        span = img._range(data_rva) if data_size else None
        if span is None:
            problems.append(f"version block: data RVA {data_rva:#x} ({data_size} bytes) has no file bytes")
            continue
        start, stop = span
        avail = min(data_size, stop - start)
        if avail < data_size:
            problems.append(f"version block: {data_size} bytes declared, {avail} in the file")
        version = {"language": lang, "codepage": codepage, "size": data_size}
        version.update(_version_info(bytes(data[start:start + avail]), problems))
        out["version"] = version
    out["types"] = sorted(types.values(), key=lambda t: (t["id"] is None, t["id"] or 0))
    return out


def resources(img: PeImage) -> dict:
    """_resources, with a table it misreads anyway reported as a problem instead of stopping the report."""
    try:
        return _resources(img)
    except (struct.error, IndexError) as e:
        return {"present": True, "types": [], "named_types": 0, "version": None,
                "problems": [f"resource directory: truncated or corrupt table ({e})"]}


def _export_module_name(img: PeImage) -> str:
    """The module name the export directory records (its Name field), or "" when there is none."""
    rva, size = img.directory(DIR_EXPORT)
    at = img.offset(rva, 40) if rva and size >= 40 else None
    if at is None:
        return ""
    return img.string_at(struct.unpack_from("<I", img.data, at + 12)[0]) or ""


def _tls_callbacks(img: PeImage) -> int:
    rva, size = img.directory(DIR_TLS)
    header_size = 40 if img.is64 else 24
    callbacks_end = 32 if img.is64 else 16
    if not rva or size < header_size:
        return 0
    at = img.offset(rva, header_size)
    if at is None:
        start = img.offset(rva)
        if start is not None and start + callbacks_end > len(img.data):
            raise PeFormatError("truncated TLS directory")
        return 0
    callbacks_va = struct.unpack_from("<Q" if img.is64 else "<I", img.data, at + (24 if img.is64 else 12))[0]
    if not callbacks_va or callbacks_va < img.image_base:
        return 0
    callbacks = callbacks_va - img.image_base
    count = 0
    width = 8 if img.is64 else 4
    for i in range(MAX_ENTRIES):
        entry = img.offset(callbacks + i * width, width)
        if entry is None or not img.word(entry):
            break
        count += 1
    return count


_apiset_cache: dict[str, bool] = {}


def _apiset_resolves(name: str) -> bool:
    """True if this Windows' API-set schema maps the contract to a host DLL (ntdll query; nothing is loaded)."""
    if name not in _apiset_cache:
        _apiset_cache[name] = _query_apiset(name)
    return _apiset_cache[name]


def _query_apiset(name: str) -> bool:
    if os.name != "nt":
        return False
    try:
        import ctypes
        from ctypes import wintypes

        class UNICODE_STRING(ctypes.Structure):
            _fields_ = [("Length", wintypes.USHORT), ("MaximumLength", wintypes.USHORT), ("Buffer", wintypes.LPWSTR)]

        query = ctypes.WinDLL("ntdll").ApiSetQueryApiSetPresence
        query.argtypes = [ctypes.POINTER(UNICODE_STRING), ctypes.POINTER(ctypes.c_ubyte)]
        query.restype = ctypes.c_long
        base = name[:-4] if name.lower().endswith(".dll") else name
        buf = ctypes.create_unicode_buffer(base)
        text = ctypes.cast(buf, wintypes.LPWSTR)
        us = UNICODE_STRING(len(base) * 2, (len(base) + 1) * 2, text)
        present = ctypes.c_ubyte(0)
        status = query(ctypes.byref(us), ctypes.byref(present))
        return status == 0 and bool(present.value)
    except (OSError, AttributeError, ValueError):
        return False


# A DLL file name as an image stores it for LoadLibrary*: ASCII or UTF-16LE, letters, digits, '_', '-' and '.'.
# A UTF-16 name starts after a NUL (or at the start), so the tail of an ASCII string followed by one isn't read as one.
_DLL_ASCII = re.compile(rb"[A-Za-z0-9_][A-Za-z0-9_.\-]{0,63}\.[dD][lL][lL](?![A-Za-z0-9_])")
_DLL_WIDE = re.compile(rb"(?<![^\x00])(?:[A-Za-z0-9_]\x00)(?:[A-Za-z0-9_.\-]\x00){0,63}"
                       rb"\.\x00[dD]\x00[lL]\x00[lL]\x00(?![A-Za-z0-9_]\x00)")


def dll_strings(data: bytes, imported: set[str], own_name: str | tuple[str, ...] = "") -> list[dict]:
    """DLL names the image holds as strings but doesn't import: candidates for a run-time LoadLibrary*, which the
    import table doesn't show (XCurl.dll and libHttpClient.GDK.dll reach xgameruntime.dll only this way,
    docs/runtime_imports.md). `imported` is the lower-cased names from the import tables; the image's own names
    (its file name and the name in its export directory) are left out too. Each entry is {"name", "encoding":
    "ascii" | "utf-16" | "both"}, sorted by name. A string is not proof of a load, only of where to look."""
    seen: dict[str, set[str]] = {}
    for m in _DLL_ASCII.finditer(data):
        seen.setdefault(m.group().decode("ascii").lower(), set()).add("ascii")
    for m in _DLL_WIDE.finditer(data):
        seen.setdefault(m.group().decode("utf-16-le").lower(), set()).add("utf-16")
    own = (own_name,) if isinstance(own_name, str) else own_name
    skip = imported | {n.lower() for n in own if n}
    return [{"name": n, "encoding": "both" if len(e) == 2 else next(iter(e))}
            for n, e in sorted(seen.items()) if n not in skip]


# Host DLLs the runtime's shim registry does not own although a SHIM_TABLE lists them: only a few of their functions
# are replaced (runtime/shims/registry.cpp, owns_dll). API sets are never owned either.
_SHIM_HOST_DLLS = frozenset({"kernel32.dll", "kernelbase.dll", "msvcrt.dll", "ucrtbase.dll", "ucrtbased.dll",
                             "vulkan-1.dll", "ntdll.dll", "user32.dll", "ws2_32.dll", "wsock32.dll",
                             "advapi32.dll"})
_SHIM_TABLE_DLL = re.compile(r'[{(]\s*"([A-Za-z0-9_.\-]+\.[dD][lL][lL])"\s*[,)]')
_shim_dlls_cache: dict[str, frozenset[str]] = {}


def _shim_table_bodies(text: str) -> list[str]:
    """The argument text of every SHIM_TABLE(...) in a source file (parentheses matched; export names hold none)."""
    bodies = []
    at = text.find("SHIM_TABLE(")
    while at >= 0:
        start = i = at + len("SHIM_TABLE(")
        depth = 1
        while i < len(text) and depth:
            depth += {"(": 1, ")": -1}.get(text[i], 0)
            i += 1
        bodies.append(text[start:i - 1])
        at = text.find("SHIM_TABLE(", i)
    return bodies


def runtime_shim_dlls(folder: str = RUNTIME_SHIMS) -> frozenset[str]:
    """Lower-cased names of the DLLs the runtime's shims own (owns_dll in runtime/shims/registry.cpp): every DLL a
    SHIM_TABLE in `folder`'s .cpp files lists, less the host DLLs and API sets the registry only patches. Empty when
    the sources are not there (a copy of this tool outside the repository)."""
    key = os.path.abspath(folder)
    if key not in _shim_dlls_cache:
        names: set[str] = set()
        try:
            files = sorted(n for n in os.listdir(folder) if n.endswith(".cpp"))
        except OSError:
            files = []
        for n in files:
            try:
                with open(os.path.join(folder, n), encoding="utf-8", errors="replace") as f:
                    text = f.read()
            except OSError:
                continue
            for body in _shim_table_bodies(text):
                names.update(m.lower() for m in _SHIM_TABLE_DLL.findall(body))
        _shim_dlls_cache[key] = frozenset(
            n for n in names if n not in _SHIM_HOST_DLLS and not n.startswith(("api-ms-", "ext-ms-")))
    return _shim_dlls_cache[key]


_listing_cache: dict[tuple[str, int], dict[str, str]] = {}


def _file_names(folder: str) -> dict[str, str]:
    """{lower-cased name: name on disk} of the files in `folder` (empty when it can't be read). Read once per folder
    state: the key holds the folder's mtime, which changes when a file is added, removed or renamed."""
    try:
        stamp = os.stat(folder).st_mtime_ns
    except OSError:
        return {}
    key = (os.path.abspath(folder), stamp)
    if key not in _listing_cache:
        try:
            with os.scandir(folder) as entries:
                _listing_cache[key] = {e.name.lower(): e.name for e in entries if e.is_file()}
        except OSError:
            _listing_cache[key] = {}
    return _listing_cache[key]


def wine_system32() -> str | None:
    """On a host that isn't Windows, the Wine prefix's system32, which stands in for System32:
    $WINEPREFIX/drive_c/windows/system32 when WINEPREFIX is set, else ~/.wine's. None when that folder is missing (a
    WINEPREFIX that names no prefix is not replaced by ~/.wine, which would answer for another prefix)."""
    prefix = os.environ.get("WINEPREFIX") or os.path.join(os.path.expanduser("~"), ".wine")
    path = os.path.join(prefix, "drive_c", "windows", "system32")
    return path if os.path.isdir(path) else None


def system_dll_source() -> tuple[str, str | None]:
    """Where `system` is checked on this host: ("windows", SYSTEM32), ("wine-prefix", <the prefix's system32>), or
    ("none", None) on a host that isn't Windows and has no Wine prefix."""
    if IS_WINDOWS:
        return "windows", SYSTEM32
    path = wine_system32()
    return ("wine-prefix", path) if path else ("none", None)


_wine_apiset_cache: dict[tuple[str, int], frozenset[str]] = {}


def _schema_contracts(data: bytes) -> frozenset[str]:
    """The contracts an API-set schema (an .apiset section) maps to a host DLL. Version 6 only, the layout of
    Windows 10 and later and of Wine: a header of version, size, flags, count, entry offset, hash offset and hash
    factor; a 24-byte namespace entry of flags, name offset, name length, hashed length, value offset and value count;
    a 20-byte value entry of flags, name offset, name length, value offset and value length. Each contract is the
    lower-cased name cut at its hashed length (up to the last hyphen), which is what a lookup compares."""
    if len(data) < 28 or struct.unpack_from("<I", data, 0)[0] != 6:
        return frozenset()
    count, entries = struct.unpack_from("<II", data, 12)
    found = set()
    for i in range(min(count, MAX_ENTRIES)):
        at = entries + i * 24
        if at + 24 > len(data):
            break
        _flags, name_at, _name_len, hashed_len, values_at, value_count = struct.unpack_from("<6I", data, at)
        if name_at + hashed_len > len(data):
            continue
        hosts = 0
        for v in range(min(value_count, MAX_ENTRIES)):
            value = values_at + v * 20
            if value + 20 > len(data):
                break
            hosts += struct.unpack_from("<I", data, value + 16)[0] > 0
        if hosts:
            found.add(data[name_at:name_at + hashed_len].decode("utf-16-le", errors="replace").lower())
    return frozenset(found)


def wine_apisets(system32: str) -> frozenset[str]:
    """The API-set contracts the prefix's apisetschema.dll resolves (_schema_contracts); empty when the file, its
    .apiset section or a version-6 schema is missing. Wine resolves API sets this way and installs no file for most
    of them."""
    disk = _file_names(system32).get("apisetschema.dll")
    if disk is None:
        return frozenset()
    path = os.path.join(system32, disk)
    try:
        key = (os.path.abspath(path), os.stat(path).st_mtime_ns)
    except OSError:
        return frozenset()
    if key not in _wine_apiset_cache:
        try:
            with open(path, "rb") as f:
                img = PeImage(f.read())
            section = next((s for s in img.sections if s["name"] == ".apiset"), None)
            _wine_apiset_cache[key] = _schema_contracts(img.section_data(section) if section else b"")
        except (OSError, PeFormatError, struct.error):
            _wine_apiset_cache[key] = frozenset()
    return _wine_apiset_cache[key]


def _apiset_key(name: str) -> str:
    """An API-set name as a schema lookup compares it: lower case, without ".dll" and the part after the last
    hyphen."""
    base = name.lower()
    if base.endswith(".dll"):
        base = base[:-4]
    return base.rsplit("-", 1)[0]


def _bundled(name: str, game_dir: str | None) -> bool:
    """Ships in the game folder, compared without case as Windows does: an import of MSVCP140.dll finds the package's
    msvcp140.dll on a case-sensitive file system too."""
    if not game_dir:
        return False
    return os.path.isfile(os.path.join(game_dir, name)) or name.lower() in _file_names(game_dir)


# The values of an import's "where". "unverified" is reported only on a host that isn't Windows and has no Wine
# prefix: the DLL is not bundled and the runtime's shims don't own it, so it is most likely a system DLL, but there
# was nothing to check it against. The Markdown report shows it as "system (unverified)".
CLASSES = ("bundled", "system", "shim", "unverified")
LABELS = {"unverified": "system (unverified)"}


def classify_dll(name: str, game_dir: str | None) -> str:
    """One of CLASSES. bundled = ships beside the game; system = present on this Windows, or in the Wine prefix's
    system32 (or its API-set schema) on another host; shim = must be reimplemented; unverified = on a host that isn't
    Windows and has no Wine prefix, neither bundled nor owned by the runtime's shims (runtime_shim_dlls)."""
    if _bundled(name, game_dir):
        return "bundled"
    low = name.lower()
    apiset = low.startswith("api-ms-win-") or low.startswith("ext-ms-win-")
    if IS_WINDOWS:
        if apiset:
            # API sets resolve through the OS schema: ask this Windows whether it knows the contract.
            return "system" if _apiset_resolves(name) else "shim"
        if os.path.isfile(os.path.join(SYSTEM32, name)):
            return "system"
        return "shim"
    system32 = wine_system32()
    if system32 is None:
        # Nothing to check against: only a module the runtime's shims own is known to need a shim.
        return "shim" if not apiset and low in runtime_shim_dlls() else "unverified"
    if low in _file_names(system32):
        return "system"
    if apiset:
        return "system" if _apiset_key(name) in wine_apisets(system32) else "shim"
    return "shim"


def inspect(path: str, game_dir: str | None) -> dict:
    """The report for one image. A file cut off inside a table raises PeFormatError like any other bad image;
    the struct.error it used to raise escaped main() and stopped a multi-file run at that file."""
    try:
        return _inspect(path, game_dir)
    except struct.error as e:
        raise PeFormatError(f"truncated or corrupt table: {e}") from None


def _inspect(path: str, game_dir: str | None) -> dict:
    with open(path, "rb") as f:
        img = PeImage(f.read())
    sections = []
    for s in img.sections:
        data = img.section_data(s)
        e = entropy(data)
        sections.append({
            "name": s["name"],
            "rva": s["rva"], "vsize": s["vsize"], "rawsize": s["rawsize"],
            "exec": s["exec"],
            "write": s["write"],
            "entropy": round(e, 3),
            "looks_encrypted": e > 7.9 and len(data) > 4096,
        })

    imports = _imports(img, "normal", DIR_IMPORT) + _imports(img, "delay", DIR_DELAY_IMPORT, delay=True)
    for entry in imports:
        entry["where"] = classify_dll(entry["dll"], game_dir)

    exports = _exports(img)
    own_names = (os.path.basename(path), _export_module_name(img))
    loads = dll_strings(img.data, {i["dll"].lower() for i in imports}, own_names)
    for entry in loads:
        entry["where"] = classify_dll(entry["name"], game_dir)
    pdata_rva, pdata_size = img.directory(DIR_EXCEPTION)
    reloc_rva, reloc_size = img.directory(DIR_BASERELOC)
    checked, system_dir = system_dll_source()

    return {
        "file": os.path.basename(path),
        "size": os.path.getsize(path),
        "machine": MACHINES.get(img.machine, hex(img.machine)),
        "timestamp": img.timestamp,
        "is_dll": bool(img.characteristics & 0x2000),
        "subsystem": SUBSYSTEMS.get(img.subsystem, img.subsystem),
        "image_base": hex(img.image_base),
        "size_of_image": img.size_of_image,
        "entry_rva": hex(img.entry_rva),
        "has_relocs": bool(reloc_rva and reloc_size),
        "dynamic_base": bool(img.dll_characteristics & 0x40),
        "resources": resources(img),
        "sections": sections,
        "imports": imports,
        "export_count": len(exports),
        "exports": exports,
        "pdata_functions": pdata_size // 12 if pdata_rva and pdata_size >= 12 else 0,
        "tls_callbacks": _tls_callbacks(img),
        "runtime_load_candidates": loads,
        # Where `system` was checked: "windows" (System32 and the OS API-set schema), "wine-prefix" (its system32
        # and apisetschema.dll) or "none" (no Wine prefix on a host that isn't Windows: see "unverified").
        "system_dlls": {"checked": checked, "dir": system_dir},
    }


def system_dlls_note(r: dict) -> str | None:
    """One line on where system DLLs were checked, for a report made on a host that isn't Windows; None otherwise."""
    info = r.get("system_dlls") or {}
    if info.get("checked") == "wine-prefix":
        return f"System DLLs checked in the Wine prefix's `{info['dir']}`."
    if info.get("checked") == "none":
        return ("System DLLs could not be checked: no Wine prefix (set `WINEPREFIX`, or make `~/.wine`). A DLL that "
                "is not bundled and that the runtime's shims don't own is listed as `system (unverified)`.")
    return None


def to_markdown(r: dict) -> str:
    out = [f"# {r['file']}", "",
           f"- {r['machine']}, {'DLL' if r['is_dll'] else 'EXE'}, subsystem {r['subsystem']}, {r['size']:,} bytes",
           f"- image base {r['image_base']}, size {r['size_of_image']:,}, entry RVA {r['entry_rva']}",
           f"- relocations: {'yes' if r['has_relocs'] else 'NO (must load at preferred base)'}; "
           f"ASLR flag: {r['dynamic_base']}",
           f"- .pdata functions: {r['pdata_functions']:,}; TLS callbacks: {r['tls_callbacks']}; exports: {r['export_count']}",
           "", "## Sections", "", "| name | rva | vsize | exec | write | entropy | |", "|---|---|---|---|---|---|---|"]
    for s in r["sections"]:
        flag = "**encrypted?**" if s["looks_encrypted"] else ""
        out.append(f"| {s['name']} | {s['rva']:#x} | {s['vsize']:,} | {s['exec']} | {s['write']} | {s['entropy']} | {flag} |")
    out += ["", "## Imports", ""]
    by_where = Counter()
    for imp in r["imports"]:
        by_where[imp["where"]] += len(imp["functions"])
    out.append(", ".join(f"{LABELS.get(k, k)}: {v}" for k, v in sorted(by_where.items())) or "none")
    note = system_dlls_note(r)
    if note:
        out += ["", note]
    out += ["", "| dll | kind | resolves | functions |", "|---|---|---|---|"]
    for imp in sorted(r["imports"], key=lambda i: (i["where"] != "shim", i["dll"].lower())):
        out.append(f"| {imp['dll']} | {imp['kind']} | {LABELS.get(imp['where'], imp['where'])} | "
                   f"{len(imp['functions'])} |")
    loads = r.get("runtime_load_candidates", [])
    if loads:
        out += ["", "## Run-time load candidates", "",
                "DLL names held as strings but not imported: a `LoadLibrary*` the import table doesn't show "
                "(a string is where to look, not proof of a load).", "",
                "| dll | encoding | resolves |", "|---|---|---|"]
        out += [f"| {d['name']} | {d['encoding']} | {LABELS.get(d['where'], d['where'])} |" for d in loads]
    shims = [i for i in r["imports"] if i["where"] == "shim"]
    if shims:
        out += ["", "## Needs checking or a shim", ""]
        for imp in shims:
            out.append(f"### {imp['dll']} ({imp['where']})")
            out.append("")
            out.append(", ".join(f"`{f}`" for f in imp["functions"]))
            out.append("")
    out += resources_markdown(r.get("resources"))
    return "\n".join(out) + "\n"


def resources_markdown(res: dict | None) -> list[str]:
    """The resource section of the Markdown report: types and counts, then the version block without its values."""
    if not res or not res["present"]:
        return ["", "## Resources", "", "none"]
    out = ["", "## Resources", ""]
    types = ", ".join(f"{t['type']} {t['entries']} ({t['bytes']:,} bytes)" for t in res["types"])
    out.append(f"- types: {types or 'none'}" + (f"; named types: {res['named_types']}" if res["named_types"] else ""))
    out += [f"- problem: {p}" for p in res["problems"]]
    v = res["version"]
    if v is None:
        out += ["", "No version block (RT_VERSION)."]
        return out
    out += ["", "### Version block", ""]
    lang = "-" if v["language"] is None else f"{v['language']:#06x}"
    out.append(f"- resource language {lang}, code page {v['codepage']}, {v['size']:,} bytes")
    f = v["fixed"]
    if f:
        out.append(f"- file version {f['file_version']}, product version {f['product_version']}")
        out.append(f"- flags: {', '.join(f['flag_names']) or 'none'} (flags {f['flags']:#x}, mask "
                   f"{f['flags_mask']:#x}); OS {f['os_name'] or hex(f['os'])}; type "
                   f"{f['type_name'] or f['type']}, subtype {f['subtype']}; date {f['date']}")
    else:
        out.append("- no VS_FIXEDFILEINFO")
    out.append(f"- translations: {', '.join(v['translations']) or 'none'}; string tables: "
               f"{', '.join(t['lang_codepage'] or '?' for t in v['string_tables']) or 'none'}; "
               f"other blocks: {v['other_blocks']}")
    if v["string_count"]:
        out += ["", f"{v['string_count']} string values, not shown: each is listed by key with its length and the "
                "start of its SHA-256 (the JSON has the full hash). A key outside the standard names is listed by "
                "its hash too.", "", "| table | key | chars | sha256 |", "|---|---|---|---|"]
        for t in v["string_tables"]:
            for s in t["strings"]:
                key = s["key"] or f"(other, {s['key_sha256'][:16]})"
                out.append(f"| {t['lang_codepage'] or '?'} | {key} | {s['chars']} | {s['sha256'][:16]} |")
    return out


def version_summary(r: dict) -> str | None:
    """The stdout line for an image with a version block: numbers and counts only."""
    res = r.get("resources") or {}
    v = res.get("version")
    if v is None:
        return f"{r['file']}: resource directory problems: {len(res['problems'])}" if res.get("problems") else None
    f = v["fixed"]
    numbers = f"file {f['file_version']}, product {f['product_version']}" if f else "no fixed info"
    return (f"{r['file']}: version block {numbers}, {v['string_count']} string keys (values not shown)"
            + (f", problems: {len(res['problems'])}" if res["problems"] else ""))


def same_output_path(first: str, second: str) -> bool:
    """True when writing ``second`` would truncate ``first`` (same file, path or hard link)."""
    if os.path.normcase(os.path.realpath(first)) == os.path.normcase(os.path.realpath(second)):
        return True
    try:
        return os.path.samefile(first, second)
    except OSError:
        return False


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("image", nargs="+")
    ap.add_argument("--game-dir")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "..", "reports", "pe"))
    a = ap.parse_args(argv)
    if a.game_dir is not None and not os.path.isdir(a.game_dir):
        print(f"ERROR: --game-dir {a.game_dir} does not exist or is not a directory", file=sys.stderr)
        return 2
    missing = [img for img in a.image if not os.path.exists(img)]
    if missing:
        for img in missing:
            print(f"ERROR: {img}: does not exist", file=sys.stderr)
        return 2
    if os.path.isfile(a.out):
        print(f"cannot create output directory {a.out}: path is a file, not a directory", file=sys.stderr)
        return 1
    try:
        os.makedirs(a.out, exist_ok=True)
    except OSError as e:
        print(f"cannot create output directory {a.out}: {e.strerror or e}", file=sys.stderr)
        return 1
    rc = 0
    seen_paths: set[tuple[str, str]] = set()
    used_names: set[str] = set()
    notes: list[str] = []
    for img in a.image:
        search_dir = a.game_dir or os.path.dirname(os.path.abspath(img))
        key = (os.path.normcase(os.path.realpath(img)), os.path.normcase(os.path.abspath(search_dir)))
        if key in seen_paths:
            print(f"{img}: given more than once; inspected once", file=sys.stderr)
            continue
        seen_paths.add(key)
        try:
            r = inspect(img, search_dir)
        except (PeFormatError, OSError) as e:
            print(f"{img}: not a PE image ({e})", file=sys.stderr)
            rc = 1
            continue
        # One report per image: the full file name keeps game.exe and game.dll apart, and a numbered suffix keeps
        # a/x.exe and b/x.exe apart. Compared case-insensitively, as Windows file names are.
        name = r["file"]
        n = 1
        while name.lower() in used_names:
            n += 1
            name = f"{r['file']}-{n}"
        used_names.add(name.lower())
        if n > 1:
            print(f"{img}: another input is also named {r['file']}; report written as {name}.json/.md",
                  file=sys.stderr)
        stem = os.path.join(a.out, name)
        json_path = stem + ".json"
        md_path = stem + ".md"
        collision = None
        for other in a.image:
            if same_output_path(other, json_path):
                collision = json_path
                break
            if same_output_path(other, md_path):
                collision = md_path
                break
        if collision:
            print(f"{img}: report {collision} would overwrite input file", file=sys.stderr)
            rc = 1
            continue
        try:
            with open(json_path, "w", encoding="utf-8", newline="\n") as f:
                json.dump(r, f, indent=1)
            with open(md_path, "w", encoding="utf-8", newline="\n") as f:
                f.write(to_markdown(r))
            version = version_summary(r)  # numbers and counts only, never a value of the block
            if version:
                print(version)
        except OSError as e:
            print(f"{img}: cannot write report ({e.strerror or e})", file=sys.stderr)
            rc = 1
            continue
        enc = [s["name"] for s in r["sections"] if s["looks_encrypted"]]
        shim = sum(len(i["functions"]) for i in r["imports"] if i["where"] == "shim")
        unverified = sum(len(i["functions"]) for i in r["imports"] if i["where"] == "unverified")
        print(f"{r['file']:<40} {r['machine']:<7} subsys={r['subsystem']!s:<7} pdata={r['pdata_functions']:<7} "
              f"shim-imports={shim:<5} encrypted={','.join(enc) or '-'}"
              + (f" unverified-imports={unverified}" if unverified else ""))
        note = system_dlls_note(r)
        if note and note not in notes:
            notes.append(note)
    for note in notes:  # once per run, after the per-image lines; only on a host that isn't Windows
        print(note.replace("`", ""))
    return rc


if __name__ == "__main__":
    try:  # a redirected stdout is strict ANSI code page under -I; write '?' for what it cannot encode
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError, OSError):
        pass
    sys.exit(main())
