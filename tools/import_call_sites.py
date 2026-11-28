"""Count and locate the direct calls to chosen imports of an x86-64 PE image, from the file alone.

Usage:
    python -I tools/import_call_sites.py <image> [--import DLL!NAME ...] [--dll DLL ...] [--json]

An MSVC image calls an imported function through a thunk, `jmp qword [rip+disp32]` through the import's address-table
slot, and reaches the thunk with `call rel32` (or `jmp rel32` as a tail call), or directly with `call qword [rip+disp32]`
through the slot. This finds each wanted import's thunk and direct call sites, then every `E8`/`E9 rel32` in an
executable section whose target is that thunk, plus every `FF 15 disp32` targeting the slot directly. Nothing is run
and nothing is disassembled, so this is a byte pattern: a stray `E8`, `E9` or `FF 15` inside another instruction that
happens to land on a thunk or slot would be counted too, which is rare, and a call that goes through a register
(`call rax` after a load of the slot) is not seen. Read the counts as a lower bound for indirect uses and an upper bound
for direct ones.

With no `--import` and no `--dll`, ordinary and delay-loaded imports are listed. An import named `#N` is an ordinal
import. Slots with the same `DLL!NAME` spelling contribute to one row. If both tables claim the same slot RVA, the
ordinary import takes precedence. An `--import` value must carry both halves of `DLL!NAME` and a `--dll` value only
a DLL name. Each selector must match an import in the image. Output is counts and RVAs only, no instruction bytes
(docs/runtime_imports.md, "The retail executable (RT-5)").
"""
import argparse
import json
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)
import pe_inspect  # noqa: E402

JMP_INDIRECT = re.compile(b"\xff\x25")
CALL_INDIRECT = re.compile(b"\xff\x15")
REL_BRANCH = re.compile(b"[\xe8\xe9]")


def import_slots(img):
    """{IAT slot RVA: 'dll!name'} for every import of a PE32+ image."""
    return {at: f"{dll}!{name}" for at, (dll, name) in import_entries(img).items()}


def import_entries(img):
    """{IAT slot RVA: (dll, name)}: the two parts kept apart, since a DLL name may itself contain '!'."""
    rva, size = img.directory(pe_inspect.DIR_IMPORT)
    slots = {}
    for i in range(size // 20 if rva else 0):
        at = img.offset(rva + i * 20, 20)
        if at is None:
            break
        oft, _stamp, _forwarder, dll_rva, iat_rva = struct.unpack_from("<IIIII", img.data, at)
        if not dll_rva:
            break
        dll = img.string_at(dll_rva)
        lookup = oft or iat_rva
        if dll is None or not lookup:
            continue
        for j, name in enumerate(img.thunk_names(lookup)):
            slots[iat_rva + j * 8] = (dll, name)

    for dll_rva, iat_rva, lookup, va_entries in img.import_descriptors(pe_inspect.DIR_DELAY_IMPORT, delay=True):
        # Delay IAT slots hold resolver addresses before binding, so require a positive mapped INT
        # and reject a legacy DLL-name VA that normalizes to RVA 0 (the DOS header).
        if dll_rva <= 0 or iat_rva <= 0 or lookup <= 0:
            continue
        if img.offset(iat_rva, 8) is None or img.offset(lookup, 8) is None:
            continue
        dll = img.string_at(dll_rva)
        if dll is None:
            continue
        for j, name in enumerate(img.thunk_names(lookup, va_entries=va_entries)):
            slot = iat_rva + j * 8
            if img.offset(slot, 8) is None:
                break
            slots.setdefault(slot, (dll, name))
    return slots


def call_sites(data, wanted=None, dlls=None):
    """Per import: its thunk RVAs and the RVAs of the `call` and `jmp` instructions that target them.

    `wanted` is a set of 'dll!name' (compared case-insensitively on the dll), `dlls` a set of dll names; with both
    None every import is scanned.
    """
    img = pe_inspect.PeImage(data)
    if not img.is64:
        raise pe_inspect.PeFormatError("only PE32+ (x86-64) images are scanned")
    lower_wanted = {w.lower() for w in wanted} if wanted else None
    lower_dlls = {d.lower() for d in dlls} if dlls else None

    def selected(dll, name):
        if lower_wanted is None and lower_dlls is None:
            return True
        return (lower_wanted is not None and f"{dll.lower()}!{name}".lower() in lower_wanted) or \
               (lower_dlls is not None and dll.lower() in lower_dlls)

    slots = {rva: f"{dll}!{name}" for rva, (dll, name) in import_entries(img).items() if selected(dll, name)}
    code = [(s["rva"], img.section_data(s)) for s in img.sections if s["exec"]]

    thunks = {}
    for rva, blob in code:
        for m in JMP_INDIRECT.finditer(blob):
            s = m.start()
            if s + 6 > len(blob):
                continue
            target = rva + s + 6 + struct.unpack_from("<i", blob, s + 2)[0]
            if target in slots:
                thunks[rva + s] = slots[target]

    out = {key: {"thunks": [], "call": [], "jmp": []} for key in slots.values()}
    for at, key in thunks.items():
        out[key]["thunks"].append(at)
    for rva, blob in code:
        for m in REL_BRANCH.finditer(blob):
            s = m.start()
            if s + 5 > len(blob):
                continue
            key = thunks.get(rva + s + 5 + struct.unpack_from("<i", blob, s + 1)[0])
            if key:
                out[key]["call" if blob[s] == 0xE8 else "jmp"].append(rva + s)
        for m in CALL_INDIRECT.finditer(blob):
            s = m.start()
            if s + 6 > len(blob):
                continue
            target = rva + s + 6 + struct.unpack_from("<i", blob, s + 2)[0]
            if target in slots:
                out[slots[target]]["call"].append(rva + s)
    for entry in out.values():
        for field in entry.values():
            field.sort()
    return {"image_base": img.image_base, "imports": out}


def import_selector(text: str) -> str:
    """--import: `DLL!NAME`, the form `selected()` matches. A bare DLL, a bare name or an empty half selects no import,
    so the run printed "0 import(s), 0 call site(s)" for a typo in either half: the answer read as "the image never
    calls it" (docs/xmem_exe_calls.md reads those counts that way)."""
    dll, sep, name = text.partition("!")
    if not sep or not dll or not name:
        raise argparse.ArgumentTypeError(f"expected DLL!NAME, got {text!r}")
    return text


def dll_selector(text: str) -> str:
    """--dll: a DLL name. `DLL!NAME` is the --import form and names no DLL, so it selected nothing."""
    if not text or "!" in text:
        raise argparse.ArgumentTypeError(f"expected a DLL name, got {text!r}")
    return text


def unmatched_selectors(data: bytes, wanted, dlls) -> list:
    """The well-formed selectors that name nothing this image imports: a misspelled half of `DLL!NAME`, or a DLL the
    image does not import. They answered "0 import(s), 0 call site(s)", which reads as "the image never calls it".
    `call_sites()` keeps answering `{}` for a caller that selects nothing on purpose."""
    entries = import_entries(pe_inspect.PeImage(data))
    names = {f"{dll}!{name}".lower() for dll, name in entries.values()}
    libs = {dll.lower() for dll, _ in entries.values()}
    return [f"--import {w}" for w in wanted if w.lower() not in names] + \
           [f"--dll {d}" for d in dlls if d.lower() not in libs]


def main(argv=None):
    ap = argparse.ArgumentParser(description="Count the direct calls to chosen imports of an x86-64 PE image.")
    ap.add_argument("image")
    ap.add_argument("--import", dest="imports", action="append", default=[], metavar="DLL!NAME", type=import_selector,
                    help="an import to look for (repeatable); '#N' names an ordinal import")
    ap.add_argument("--dll", dest="dlls", action="append", default=[], metavar="DLL", type=dll_selector,
                    help="every import of this DLL (repeatable)")
    ap.add_argument("--json", action="store_true", help="print JSON (RVAs in hex) instead of the table")
    args = ap.parse_args(argv)
    try:
        with open(args.image, "rb") as f:
            data = f.read()
        result = call_sites(data, set(args.imports) or None, set(args.dlls) or None)
    except (OSError, pe_inspect.PeFormatError) as e:
        print(f"{os.path.basename(sys.argv[0])}: {e}", file=sys.stderr)
        return 2
    imports = result["imports"]
    missing = unmatched_selectors(data, args.imports, args.dlls)
    if missing:
        print(f"{os.path.basename(sys.argv[0])}: {os.path.basename(args.image)}: "
              f"no import matches {', '.join(missing)}", file=sys.stderr)
        return 2
    if args.json:
        printable = {k: {f: [hex(x) for x in v] for f, v in e.items()} for k, e in imports.items()}
        print(json.dumps({"image_base": hex(result["image_base"]), "imports": printable}, indent=1))
        return 0
    print(f"{'import':56s} {'thunks':>6s} {'call':>5s} {'jmp':>4s}")
    for key, e in sorted(imports.items(), key=lambda kv: (-(len(kv[1]["call"]) + len(kv[1]["jmp"])), kv[0])):
        print(f"{key:56s} {len(e['thunks']):6d} {len(e['call']):5d} {len(e['jmp']):4d}")
    total = sum(len(e["call"]) + len(e["jmp"]) for e in imports.values())
    print(f"{len(imports)} import(s), {total} call site(s)")
    return 0


if __name__ == "__main__":
    try:  # a redirected stdout is strict ANSI code page under -I; write '?' for what it cannot encode
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError, OSError):
        pass
    sys.exit(main())
