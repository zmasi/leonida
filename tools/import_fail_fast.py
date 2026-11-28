"""List the call sites where an x86-64 PE image stops itself when an import returns a failure, from the file alone.

Usage:
    python -I tools/import_fail_fast.py <image> [--import DLL!NAME ...] [--dll DLL ...] [--all] [--json]

A title treats some results as impossible: it tests the returned value and branches to `int3` (MSVC's `__debugbreak`
and the marker it places after a call that does not return) when the value is the failing one. A shim that returns
that value at such a site stops the boot there, with no error message. This lists, per import, how many of its call
sites do that and on which value, so the results each shim must get right are known before a run reaches them.

How, with nothing run:
1. Call sites. A `call rel32` to the import's thunk (tools/import_call_sites.py finds the thunks) and a
   `call qword [rip+disp32]` through the import's address-table slot, the form an image uses for the
   `api-ms-win-*` imports. A site's RVA is that of its `E8` or `FF` byte. Tail jumps are skipped: their result
   is tested by a caller, not here.
2. The test. The instructions after the call are decoded (tools/gdk_runtime_calls.py's decoder) up to the first
   `test` or `cmp` of the result: rax at any width, or a register it was copied to with `mov`. A `test` of the
   register with itself or with a constant, or a `cmp` with a constant. When a branch, a call, a return or
   another write to every copy of the result comes first, the site is not one this tool can judge.
3. The branch. The next instruction must be a conditional jump. Each side is followed for a few instructions, and
   through up to two unconditional jumps. A side is fatal when it reaches `int3`, `ud2` or `int 29h`
   (`__fastfail`), or a `call` whose next instruction is `int3` (a call that does not return).
4. The condition is the value that takes the fatal side, written with the result register at the width the test
   read: `eax == 0`, `rax == -1`, `eax < 0` (a failed HRESULT), `eax != 1`, `(al & 0x1) == 0`. `<u` and `>u`
   are unsigned comparisons. A site where both sides are fatal is not counted.

What a count means: a site found here is code that can stop the title, not proof that the boot reaches it. Only
straight-line code after the call is read, so a result stored and tested later, or a failure path that logs before
it stops, is not seen. The counts are heuristic: they can miss sites that way, and a stray `E8` or `FF 15` byte pair
inside another instruction could add a false one, which is rare. Output is import names, counts, conditions and RVAs only, no instruction bytes.

`--json` prints every import the image names (or every one `--import`/`--dll` selects), including those with no
call site (`"calls": 0`), with `"fatal"` empty when none of its sites is fatal. The table's last line,
"N of M import(s) called directly", gives both numbers: M imports named (or selected), N of them with at least one
direct call site. The rest are reached only through a stored pointer or a tail jump, or not at all.
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
from gdk_runtime_calls import RAX, decode, writes  # noqa: E402
from import_call_sites import call_sites, import_slots  # noqa: E402

CALL_SLOT = re.compile(b"\xff\x15")
PREFIXES = frozenset((0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65, 0x66, 0x67, 0xF0, 0xF2, 0xF3))
COPY_OPS = frozenset((0x8B, 0x63, 0x1B6, 0x1B7, 0x1BE, 0x1BF))    # mov, movsxd, movzx, movsx: r <- r/m
TEST_WINDOW = 8         # instructions read after a call to find the test of its result
SIDE_WINDOW = 4         # instructions read on each side of the branch, per jump followed
SIDE_JUMPS = 2          # unconditional jumps followed on a side

# Condition codes of jcc (low nibble of 70-7F / 0F 80-8F), as the comparison that takes the jump.
CMP_CONDITION = {0x2: "<u", 0x3: ">=u", 0x4: "==", 0x5: "!=", 0x6: "<=u", 0x7: ">u",
                 0xC: "<", 0xD: ">=", 0xE: "<=", 0xF: ">"}
# After `test r, r`: CF = OF = 0, so the condition is one on the sign and zero of r.
SELF_TEST_CONDITION = {0x4: "== 0", 0x5: "!= 0", 0x6: "== 0", 0x7: "!= 0", 0x8: "< 0", 0x9: ">= 0",
                       0xC: "< 0", 0xD: ">= 0", 0xE: "<= 0", 0xF: "> 0"}
RESULT_NAME = {8: "al", 16: "ax", 32: "eax", 64: "rax"}


def instruction_bytes(img, insn):
    at = img.offset(insn.rva, insn.size)
    return img.data[at:at + insn.size] if at is not None else b""


def decode_at(img, rva):
    at = img.offset(rva)
    if at is None:
        return None
    return decode(img.data, at, rva - at)


def width(raw, insn):
    """Operand width of an ALU instruction: 8 for the byte forms, else 64 with REX.W, 16 with a 0x66 prefix, 32."""
    if insn.op in (0x84, 0x80, 0x3C, 0xA8, 0xF6):
        return 8
    if insn.wide:
        return 64
    for b in raw:
        if b == 0x66:
            return 16
        if b not in PREFIXES:
            break
    return 32


def immediate(raw, insn, size):
    """The instruction's trailing immediate (sign-extended)."""
    fmt = {1: "<b", 2: "<h", 4: "<i"}[size]
    return struct.unpack_from(fmt, raw, len(raw) - size)[0]


def show(value):
    return str(value) if -10 < value < 10 else (f"-{-value:#x}" if value < 0 else f"{value:#x}")


def result_test(img, insn, held):
    """('self'|'mask'|'cmp', width, constant) when `insn` tests a register in `held` (the result's copies)."""
    raw = instruction_bytes(img, insn)
    op = insn.op
    if op in (0x84, 0x85) and insn.mod == 3 and insn.rm in held and insn.reg == insn.rm:
        return "self", width(raw, insn), None
    if op in (0xA8, 0xA9) and RAX in held:                         # test al/eax/rax, imm
        w = width(raw, insn)
        return "mask", w, immediate(raw, insn, 1 if op == 0xA8 else (2 if w == 16 else 4))
    if op in (0xF6, 0xF7) and insn.mod == 3 and insn.reg & 7 == 0 and insn.rm in held:   # test r/m, imm
        w = width(raw, insn)
        return "mask", w, immediate(raw, insn, 1 if op == 0xF6 else (2 if w == 16 else 4))
    if op in (0x3C, 0x3D) and RAX in held:                         # cmp al/eax/rax, imm
        w = width(raw, insn)
        return "cmp", w, immediate(raw, insn, 1 if op == 0x3C else (2 if w == 16 else 4))
    if op in (0x80, 0x81, 0x83) and insn.mod == 3 and insn.reg & 7 == 7 and insn.rm in held:   # cmp r/m, imm
        w = width(raw, insn)
        return "cmp", w, immediate(raw, insn, 4 if op == 0x81 and w != 16 else (2 if op == 0x81 else 1))
    return None


def copies_result(insn, held):
    """The register a `mov`, `movzx`, `movsx` or `movsxd` from a register copies the result into, else None."""
    if insn.mod != 3:
        return None
    if insn.op in COPY_OPS and insn.rm in held:
        return insn.reg
    if insn.op == 0x89 and insn.reg in held:
        return insn.rm
    return None


def fatal_side(img, rva):
    """Whether straight-line code from `rva` stops the program: int3, ud2, int 29h, or a call followed by int3."""
    jumps = 0
    steps = 0
    while steps < SIDE_WINDOW:
        insn = decode_at(img, rva)
        if insn is None:
            return False
        if insn.op == 0xCC or insn.op == 0x10B:
            return True
        if insn.op == 0xCD:
            return instruction_bytes(img, insn)[-1:] == b"\x29"
        if insn.is_call():
            after = decode_at(img, insn.end)
            return after is not None and after.op == 0xCC
        if insn.op in (0xE9, 0xEB) and insn.target is not None and jumps < SIDE_JUMPS:
            jumps += 1
            steps = 0
            rva = insn.target
            continue
        if insn.ends_flow() or 0x70 <= insn.op <= 0x7F or 0x180 <= insn.op <= 0x18F:
            return False
        rva = insn.end
        steps += 1
    return False


def condition(kind, bits, constant, cc):
    name = RESULT_NAME[bits]
    if kind == "self":
        rel = SELF_TEST_CONDITION.get(cc)
        return f"{name} {rel}" if rel else None
    if kind == "mask":
        rel = {0x4: "==", 0x6: "==", 0x5: "!=", 0x7: "!="}.get(cc)
        return f"({name} & {constant & ((1 << bits) - 1):#x}) {rel} 0" if rel else None
    rel = CMP_CONDITION.get(cc)
    if rel is None:
        return None
    value = constant & ((1 << bits) - 1) if rel.endswith("u") else constant
    return f"{name} {rel} {show(value)}"


def classify(img, after_call):
    """The condition on the result that takes a fatal branch, for a call whose next instruction is at `after_call`."""
    held = {RAX}
    rva = after_call
    for _ in range(TEST_WINDOW):
        insn = decode_at(img, rva)
        if insn is None or insn.is_call() or insn.ends_flow():
            return None
        if 0x70 <= insn.op <= 0x7F or 0x180 <= insn.op <= 0x18F:
            return None
        test = result_test(img, insn, held)
        if test:
            branch = decode_at(img, insn.end)
            if branch is None or not (0x70 <= branch.op <= 0x7F or 0x180 <= branch.op <= 0x18F):
                return None
            cc = branch.op & 0xF
            taken, fall = fatal_side(img, branch.target), fatal_side(img, branch.end)
            if taken == fall:
                return None
            return condition(*test, cc if taken else cc ^ 1)
        if insn.op == 0x98:                                         # cdqe/cwde: rax still holds the result
            rva = insn.end
            continue
        copy = copies_result(insn, held)
        for reg in writes(insn):
            held.discard(reg)
        if copy is not None:
            held.add(copy)
        if not held:
            return None
        rva = insn.end
    return None


def slot_calls(img, slots):
    """{key: [RVAs of `call qword [rip+disp32]` through the key's address-table slot]}."""
    out = {}
    for s in img.sections:
        if not s["exec"]:
            continue
        blob = img.section_data(s)
        for m in CALL_SLOT.finditer(blob):
            at = m.start()
            if at + 6 > len(blob):
                continue
            key = slots.get(s["rva"] + at + 6 + struct.unpack_from("<i", blob, at + 2)[0])
            if key:
                out.setdefault(key, []).append(s["rva"] + at)
    return out


def fail_fast(data, wanted=None, dlls=None):
    """Per import: its call sites and the fatal ones, each with the condition that stops the program."""
    thunk_calls = call_sites(data, wanted, dlls)["imports"]
    img = pe_inspect.PeImage(data)
    slots = {rva: key for rva, key in import_slots(img).items() if key in thunk_calls}
    direct = slot_calls(img, slots)
    out = {}
    for key, entry in thunk_calls.items():
        site_map = {}
        for rva in entry["call"]:
            at = img.offset(rva, 1)
            site_map[rva] = rva + (6 if at is not None and img.data[at] == 0xFF else 5)
        for rva in direct.get(key, []):
            site_map[rva] = rva + 6
        sites = sorted(site_map.items())
        fatal = []
        for rva, after in sites:
            cond = classify(img, after)
            if cond:
                fatal.append({"rva": rva, "condition": cond})
        out[key] = {"calls": len(sites), "fatal": fatal}
    return {"image_base": img.image_base, "imports": out}


def summary(entry):
    counts = {}
    for f in entry["fatal"]:
        counts[f["condition"]] = counts.get(f["condition"], 0) + 1
    return ", ".join(f"{c} ({n})" for c, n in sorted(counts.items(), key=lambda kv: (-kv[1], kv[0])))


def main(argv=None):
    ap = argparse.ArgumentParser(description="List the call sites where an x86-64 PE image stops itself when an "
                                             "import returns a failure.")
    ap.add_argument("image")
    ap.add_argument("--import", dest="imports", action="append", default=[], metavar="DLL!NAME",
                    help="an import to look at (repeatable); '#N' names an ordinal import")
    ap.add_argument("--dll", dest="dlls", action="append", default=[], metavar="DLL",
                    help="every import of this DLL (repeatable)")
    ap.add_argument("--all", action="store_true", help="list the imports with no fatal site too")
    ap.add_argument("--json", action="store_true", help="print JSON (RVAs in hex) instead of the table")
    args = ap.parse_args(argv)
    try:
        with open(args.image, "rb") as f:
            data = f.read()
        result = fail_fast(data, set(args.imports) or None, set(args.dlls) or None)
    except (OSError, pe_inspect.PeFormatError) as e:
        print(f"{os.path.basename(sys.argv[0])}: {e}", file=sys.stderr)
        return 2
    imports = result["imports"]
    if args.json:
        printable = {k: {"calls": e["calls"], "fatal": [{"rva": hex(f["rva"]), "condition": f["condition"]}
                                                         for f in e["fatal"]]} for k, e in imports.items()}
        print(json.dumps({"image_base": hex(result["image_base"]), "imports": printable}, indent=1))
        return 0
    rows = [(k, e) for k, e in imports.items() if e["fatal"] or args.all]
    w = max([len("import")] + [len(k) for k, _ in rows])
    print(f"{'import':{w}s} {'calls':>5s} {'fatal':>5s}  stops the program when")
    for key, e in sorted(rows, key=lambda kv: (-len(kv[1]["fatal"]), kv[0])):
        print(f"{key:{w}s} {e['calls']:5d} {len(e['fatal']):5d}  {summary(e)}")
    called = sum(1 for e in imports.values() if e["calls"])
    with_fatal = sum(1 for e in imports.values() if e["fatal"])
    sites = sum(len(e["fatal"]) for e in imports.values())
    print(f"{called} of {len(imports)} import(s) called directly, {with_fatal} with a fatal result check, "
          f"{sites} fatal site(s)")
    return 0


if __name__ == "__main__":
    try:  # a redirected stdout is strict ANSI code page under -I; write '?' for what it cannot encode
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError, OSError):
        pass
    sys.exit(main())
