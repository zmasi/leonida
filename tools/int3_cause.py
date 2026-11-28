"""Name the call whose result sends an x86-64 PE image to a given int3, from the file alone.

Usage:
    python -I tools/int3_cause.py <image> <address> [<address> ...] [--json]
    python -I tools/int3_cause.py <image> --any <address> ...      # any instruction, e.g. a `return 0` path
    python -I tools/int3_cause.py <image> --returns <address> ...  # what the function holding it returns

A boot that stops on a title assertion reports where it stopped. LEONIDA_SKIP_ASSERTS, LEONIDA_SKIP_INT3 and the
fault log give the `int3` as `exe+0x...`, `game+0x...`, an RVA or a virtual address. What failed is the call whose
result the title tested before it branched there. That branch is often far from the `int3`, in another part of the
function, so the test is not in sight of the stop. This finds it, with nothing run:

1. The stop. The instruction at the address must be `int3`, `ud2` or `int 29h`. An address one past an `int3` is
   taken as that `int3`, because Wine reports a breakpoint after the instruction.
2. The doomed code. The function that holds the stop (its `.pdata` entry) is decoded from its start with
   tools/gdk_runtime_calls.py's decoder. An instruction is doomed when every way on from it reaches the stop without
   a return. That covers straight-line code, a jump to doomed code, and a conditional jump whose two sides are both
   doomed. Calls are taken to return.
3. The guards. A conditional jump with one side doomed and the other not is where the title decided to stop. Jumps
   into the doomed code from other functions count too. They are rel32 `jmp`/`jcc`, found by byte pattern and kept
   only when decoding their own function from its start lands on them. An unconditional jump from another function
   is followed into that function the same way.
4. The condition. The last instruction before the guard that sets the flags is read, skipping the moves, `lea`s and
   other flag-neutral instructions in between. The forms read are `test r, r`, `test r, imm`, `cmp r, imm` and
   `cmp r, r`, each also with one memory operand, plus `and` (read as `test`), `sub` (as `cmp`) and `bt`. The
   condition is written as what takes the doomed side: `eax < 0`, `al == 0`, `ecx != 1`, `bit 5 of eax is set`,
   with `<u` and `>u` for unsigned comparisons.
5. The call. From the flag-setting instruction, each tested register is followed back through register copies to
   what wrote it, and the result of a call (`rax`) is named:
   - an import (`dll!Name`, through its thunk or its address-table slot);
   - a vtable slot (`call [reg+disp]` is slot disp/8), with `this` noted when it was loaded from a global;
   - a function of the image, by RVA.
   A register loaded from memory, set to a constant or computed is reported as such. A copy, a `lea r, [r2+disp]`
   or an added constant is followed through. A block that follows a return or a jump is entered through the one
   jump that reaches it. A register nothing wrote since the function start is one of its arguments (rcx, rdx, r8,
   r9). The function's direct call sites are then read for what each passes in it, as a tally ("argument 1 at the
   function's 53 call sites: 5 the result of vtable slot 133, ..."). For a load, the pointer register is followed
   too. A value read from a global, or from a field of an object a global points to, is read at the global's
   stores, each `mov [rip+x], r` or `mov [rip+x], imm` to it, with what it stores (the call that set it, or 0).
6. A function whose start is doomed never returns. Each of its direct call sites is then read the same way, as if
   the call were the stop. Steps 3 and 6 go at most 3 functions away from the stop.

With `--any` the address may be any instruction, for example the `xor eax, eax` of a `return 0` path or the start of
a fallback. Steps 2 to 5 then name the branches that decide whether it runs, and step 6 is skipped. With
`--returns` the address may be anywhere in a function. Each value the function can return in rax (a constant with
its value, a call's result, an argument, a load) is found over the paths into each `ret` that the bounded search
recovers (up to a step and source limit; the report says when it stopped there), through register copies.
For each value, the instruction that sets it is read as with `--any`. A function that returns through a tail jump
is not read past the jump.

What it does not see:
- a stop reached through a jump table or a function pointer;
- a condition set by an instruction outside the forms in 4 (it names the instruction's RVA instead);
- a value that came through the stack.

Output is for local reading: it carries RVAs. What a run shares is the call's name and the condition
(docs/runtime_imports.md, "The retail executable (RT-5)"). Exit status 0 when every address is a stop the tool could
read, 1 when one is not, 2 when the image can't be read. Standard library only.
"""
from __future__ import annotations

import argparse
import bisect
import json
import os
import re
import struct
import sys
from array import array

ROOT = os.path.dirname(os.path.abspath(__file__))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)
from gdk_runtime_calls import R8, R9, RAX, RCX, RDX, RIP, RSP, VOLATILE, Code, writes  # noqa: E402
from import_call_sites import import_slots  # noqa: E402
import pe_inspect  # noqa: E402

DEPTH = 3                   # functions away from the stop that jumps in and callers are followed
TRACE_BACK = 48             # instructions walked back from a test to what wrote its register
FLAGS_BACK = 8              # instructions walked back from a guard to the instruction that set its flags
MAX_LINKS = 16              # jumps in or call sites followed per function
MAX_SITES = 256             # call sites read for an argument the stop depends on
MAX_STORES = 64             # stores read per global the stop depends on
_RIP_STORE = re.compile(rb"[\x88\x89\xc6\xc7][\x05\x0d\x15\x1d\x25\x2d\x35\x3d]")
_REL32 = re.compile(rb"\xe8|\xe9|\x0f[\x80-\x8f]")
_REL32_KINDS = ("call", "jmp", "jcc")
ARGUMENTS = {RCX: 1, RDX: 2, R8: 3, R9: 4}

JCC = ("jo", "jno", "jb", "jae", "je", "jne", "jbe", "ja", "js", "jns", "jp", "jnp", "jl", "jge", "jle", "jg")
# Taken-when of a jcc after `cmp a, b`, as `a <rel> b`.
CMP_REL = {0x2: "<u", 0x3: ">=u", 0x4: "==", 0x5: "!=", 0x6: "<=u", 0x7: ">u", 0xC: "<", 0xD: ">=", 0xE: "<=",
           0xF: ">"}
# After `test a, b` (also `and`): CF = OF = 0, so a condition on the sign and zero of a & b.
SELF_TEST = {0x4: "== 0", 0x5: "!= 0", 0x6: "== 0", 0x7: "!= 0", 0x8: "< 0", 0x9: ">= 0", 0xC: "< 0", 0xD: ">= 0",
             0xE: "<= 0", 0xF: "> 0"}

NAMES64 = ("rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi") + tuple(f"r{n}" for n in range(8, 16))
NAMES32 = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi") + tuple(f"r{n}d" for n in range(8, 16))
NAMES16 = ("ax", "cx", "dx", "bx", "sp", "bp", "si", "di") + tuple(f"r{n}w" for n in range(8, 16))
NAMES8 = ("al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil") + tuple(f"r{n}b" for n in range(8, 16))
PREFIXES = frozenset((0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65, 0x66, 0x67, 0xF0, 0xF2, 0xF3))

# Instructions that leave the flags alone (opcode numbering as in gdk_runtime_calls.decode).
_NEUTRAL = frozenset([0x63, 0x68, 0x6A, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x8B, 0x8D, 0x8F, *range(0x90, 0x9A),
                      *range(0xA0, 0xA6), 0xAA, 0xAB, 0xC6, 0xC7, *range(0x50, 0x60), *range(0xB0, 0xC0),
                      *range(0x110, 0x118), 0x118, 0x11F, 0x128, 0x129, *range(0x140, 0x150), *range(0x16E, 0x170),
                      0x17E, 0x17F, *range(0x190, 0x1A0), 0x1B6, 0x1B7, 0x1BE, 0x1BF, *range(0x1C8, 0x1D0), 0x1D6,
                      0x1E7])
# VEX forms that set the flags: vucomis*/vcomis*, vtestp*, vptest, andn, blsr/blsmsk/blsi, bzhi, bextr.
_VEX_FLAGS = frozenset((0x112E, 0x112F, 0x120E, 0x120F, 0x1217, 0x12F2, 0x12F3, 0x12F5, 0x12F7))
COPY_OPS = frozenset((0x8A, 0x8B, 0x63, 0x1B6, 0x1B7, 0x1BE, 0x1BF))     # r <- r/m
LOAD_OPS = COPY_OPS
_BYTE_REG_OPS = frozenset((0x02, 0x0A, 0x12, 0x1A, 0x22, 0x2A, 0x32, 0x3A, 0x86, 0x8A, 0x1C0))
_BYTE_RM_OPS = frozenset((0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x80, 0x86, 0x88, 0xC0, 0xC6,
                          0xD0, 0xD2, 0xF6, 0xFE, 0x1C0, *range(0x190, 0x1A0)))


def insn_writes(insn) -> list[int]:
    """Like gdk_runtime_calls.writes, mapping 8-bit ModRM operands 4..7 without REX (ah..bh) to rax..rbx."""
    regs = writes(insn)
    if insn.rex or insn.mod is None or not regs:
        return regs
    out = list(regs)
    if insn.op in _BYTE_REG_OPS and 4 <= insn.reg <= 7:
        out = [r - 4 if r == insn.reg else r for r in out]
    if insn.mod == 3 and insn.op in _BYTE_RM_OPS and 4 <= insn.rm <= 7:
        out = [r - 4 if r == insn.rm else r for r in out]
    return out


class Image:
    def __init__(self, data: bytes):
        self.img = pe_inspect.PeImage(data)
        self.code = Code(self.img)
        self.slots = import_slots(self.img)
        self._runs = {}
        self._flows = {}
        self._rel32 = None   # sorted target << shift | rva << 2 | kind, for every rel32 call/jmp/jcc pattern
        self._stores = None  # sorted target << shift | rva, for every `mov [rip+x], r/imm` pattern
        # Room for any RVA of the image (code targets and globals alike), plus two bits for the kind.
        ends = [self.img.size_of_image] + [b + len(d) for b, d in self.code.sections]
        self._shift = max(ends).bit_length() + 2

    # ---- decoding ----

    def raw(self, insn) -> bytes:
        data, base = self.code.view(insn.rva)
        return bytes(data[insn.rva - base:insn.end - base]) if data is not None else b""

    def function(self, rva: int):
        return self.code.function(rva)

    def run(self, func):
        """The function's instructions from its start, as far as decoding goes (up to its end)."""
        if func not in self._runs:
            out, rva = [], func[0]
            while rva < func[1]:
                insn = self.code.decode(rva)
                if insn is None:
                    break
                out.append(insn)
                rva = insn.end
            self._runs[func] = out
        return self._runs[func]

    def flow(self, func):
        """(instructions, {rva: index}, {rva: [index of each jump to it]}) of the function."""
        if func not in self._flows:
            insns = self.run(func)
            into = {}
            for n, i in enumerate(insns):
                if (is_jcc(i) or is_jmp(i)) and i.target is not None:
                    into.setdefault(i.target, []).append(n)
            self._flows[func] = (insns, {i.rva: n for n, i in enumerate(insns)}, into)
        return self._flows[func]

    def lands_on(self, func, rva: int) -> bool:
        return rva in self.flow(func)[1]

    # ---- indexes: one pass over the code, built on first use, then a bisect per target ----
    #
    # Each entry packs (target, rva, kind) into one integer that sorts by target, then rva, so a lookup is a bisect
    # on the target's range and the results come out in address order, as a scan would find them. Matches are the
    # same non-overlapping regex matches a scan sees; a pattern that is not really an instruction is dropped later,
    # when the caller checks that decoding the function lands on it.

    def _pack(self, entries: list[int]):
        entries.sort()
        return array("Q", entries) if 2 * self._shift - 2 <= 64 else entries

    def _lookup(self, index, target: int):
        lo = bisect.bisect_left(index, target << self._shift)
        hi = bisect.bisect_left(index, (target + 1) << self._shift)
        return index[lo:hi]

    def rel32_from(self, target: int):
        """[(rva, kind)] of each rel32 call/jmp/jcc pattern aimed at target, in address order."""
        if self._rel32 is None:
            limit, shift, entries = 1 << (self._shift - 2), self._shift, []
            for base, data in self.code.sections:
                end = len(data) - 4
                for m in _REL32.finditer(data):
                    at = m.end()
                    if at > end:
                        continue
                    to = base + at + 4 + struct.unpack_from("<i", data, at)[0]
                    if 0 <= to < limit:
                        kind = {0xE8: 0, 0xE9: 1}.get(data[m.start()], 2)
                        entries.append(to << shift | (base + m.start()) << 2 | kind)
            self._rel32 = self._pack(entries)
        mask = (1 << (self._shift - 2)) - 1
        return [(e >> 2 & mask, _REL32_KINDS[e & 3]) for e in self._lookup(self._rel32, target)]

    def stores_to(self, target: int):
        """RVAs of each `mov [rip+x], r/imm` pattern (its opcode) storing to target, in address order."""
        if self._stores is None:
            limit, shift, entries = 1 << (self._shift - 2), self._shift, []
            for base, data in self.code.sections:
                for m in _RIP_STORE.finditer(data):
                    pos = m.start()
                    op = data[pos]
                    if op in (0xC6, 0xC7) and data[pos + 1] != 0x05:
                        continue
                    at = m.end()
                    if at + 4 > len(data):
                        continue
                    disp = struct.unpack_from("<i", data, at)[0]
                    tails = {0xC6: (1,), 0xC7: (4,)}.get(op, (0,))
                    if op == 0xC7 and (
                        (pos >= 1 and data[pos - 1] == 0x66)
                        or (pos >= 2 and data[pos - 2] == 0x66 and (data[pos - 1] & 0xF8) == 0x40)
                    ):
                        tails = (2, 4)
                    for tail in tails:
                        if at + 4 + tail > len(data):
                            continue
                        to = base + at + 4 + tail + disp
                        if 0 <= to < limit:
                            entries.append(to << shift | (base + pos))
            self._stores = self._pack(entries)
        mask = (1 << self._shift) - 1
        return [e & mask for e in self._lookup(self._stores, target)]

    # ---- names ----

    def import_at_thunk(self, rva: int):
        insn = self.code.decode(rva)
        if insn is not None and insn.op == 0xFF and insn.base == RIP and insn.reg & 7 == 4:
            return self.slots.get(insn.target)
        return None

    def call_name(self, insns, k):
        """What the call insns[k] reaches: an import, a vtable slot (with `this` when it came from a global), or an
        in-image function."""
        insn = insns[k]
        if insn.op == 0xE8:
            name = self.import_at_thunk(insn.target)
            return {"import": name} if name else {"function": insn.target}
        if insn.base == RIP:
            name = self.slots.get(insn.target)
            return {"import": name} if name else {"pointer": insn.target}
        if (insn.mod in (0, 1, 2) and insn.base is not None and insn.index is None and insn.disp >= 0
                and insn.disp % 8 == 0):
            out = {"slot": insn.disp // 8}
            for prev in reversed(insns[max(0, k - 12):k]):
                if RCX in insn_writes(prev):
                    if prev.op == 0x8B and prev.base == RIP:
                        out["this_global"] = prev.target
                    break
                if prev.ends_flow() or prev.is_call():
                    break
            return out
        return {"indirect": True}


def stop_at(image: Image, rva: int):
    """(rva, kind) of the int3, ud2 or int 29h at rva, or one byte before it (Wine), else None."""
    for at in (rva, rva - 1):
        insn = image.code.decode(at)
        if insn is None:
            continue
        if insn.op == 0xCC:
            return at, "int3"
        if at == rva and insn.op == 0x10B:
            return at, "ud2"
        if at == rva and insn.op == 0xCD and image.raw(insn)[-1:] == b"\x29":
            return at, "int 29h"
    return None


def is_jcc(insn) -> bool:
    return 0x70 <= insn.op <= 0x7F or 0x180 <= insn.op <= 0x18F or 0xE0 <= insn.op <= 0xE3


def is_jmp(insn) -> bool:
    return insn.op in (0xE9, 0xEB)


def doomed_code(insns, seeds: set[int]) -> set[int]:
    """The RVAs of the instructions from which every way on reaches one of `seeds` without a return."""
    doomed = set(seeds)
    changed = True
    while changed:
        changed = False
        for insn in reversed(insns):
            if insn.rva in doomed:
                continue
            if is_jcc(insn):
                hit = insn.target in doomed and insn.end in doomed
            elif is_jmp(insn):
                hit = insn.target in doomed
            elif insn.ends_flow() or insn.op in (0xCC, 0xCD, 0x10B):
                hit = False
            else:
                hit = insn.end in doomed
            if hit:
                doomed.add(insn.rva)
                changed = True
    return doomed


def width(raw: bytes, insn) -> int:
    if insn.op in (0x84, 0x80, 0x20, 0x22, 0x24, 0x28, 0x2A, 0x2C, 0x38, 0x3A, 0x3C, 0xA8, 0xF6):
        return 8
    if insn.wide:
        return 64
    for b in raw:
        if b == 0x66:
            return 16
        if b not in PREFIXES:
            break
    return 32


def reg_name(reg: int, bits: int, rex: int) -> str:
    if bits == 8 and not rex and 4 <= reg <= 7:
        return ("ah", "ch", "dh", "bh")[reg - 4]
    return {8: NAMES8, 16: NAMES16, 32: NAMES32, 64: NAMES64}[bits][reg]


def gpr(reg: int, bits: int, rex: int) -> int:
    """The full register an operand is part of (ah..bh are rax..rbx without a REX prefix)."""
    return reg - 4 if bits == 8 and not rex and 4 <= reg <= 7 else reg


def memory_name(insn, bits: int) -> str:
    size = {8: "byte", 16: "word", 32: "dword", 64: "qword"}[bits]
    if insn.base == RIP:
        return f"{size} [global {insn.target:#x}]"
    base = NAMES64[insn.base] if insn.base is not None else ""
    index = f"+{NAMES64[insn.index]}*n" if insn.index is not None else ""
    disp = f"{insn.disp:+#x}" if insn.disp else ""
    return f"{size} [{base}{index}{disp}]"


def immediate(raw: bytes, size: int) -> int:
    return struct.unpack_from({1: "<b", 2: "<h", 4: "<i"}[size], raw, len(raw) - size)[0]


def show(value: int) -> str:
    return str(value) if -10 < value < 10 else (f"-{-value:#x}" if value < 0 else f"{value:#x}")


# ALU forms read as a test: `and` as `test` ("mask"), `sub` as `cmp`. op r/m, r | op r, r/m | op al/eax, imm | /n
_ALU_FORMS = {0x20: ("mask", "rm"), 0x21: ("mask", "rm"), 0x22: ("mask", "reg"), 0x23: ("mask", "reg"),
              0x84: ("mask", "rm"), 0x85: ("mask", "rm"),
              0x28: ("cmp", "rm"), 0x29: ("cmp", "rm"), 0x2A: ("cmp", "reg"), 0x2B: ("cmp", "reg"),
              0x38: ("cmp", "rm"), 0x39: ("cmp", "rm"), 0x3A: ("cmp", "reg"), 0x3B: ("cmp", "reg")}
_ACC_FORMS = {0x24: "mask", 0x25: "mask", 0xA8: "mask", 0xA9: "mask", 0x2C: "cmp", 0x2D: "cmp", 0x3C: "cmp",
              0x3D: "cmp"}
_GROUP1 = {4: "mask", 5: "cmp", 7: "cmp"}           # 80/81/83 /4 and, /5 sub, /7 cmp


def read_test(insn, raw: bytes):
    """The test or comparison insn makes, as {form, left, right, bits, registers}: form "self" (`test r, r`), "mask"
    (`test`/`and`) or "cmp" (`cmp`/`sub`); left and right as text (right an int for an immediate); registers the
    general registers it reads. None for any other instruction. raw is the instruction's bytes."""
    op, bits = insn.op, width(raw, insn)
    mask = (1 << bits) - 1

    def rm():
        if insn.mod == 3:
            return reg_name(insn.rm, bits, insn.rex), [gpr(insn.rm, bits, insn.rex)]
        regs = [r for r in (insn.base, insn.index) if r is not None and r != RIP]
        return memory_name(insn, bits), regs

    def reg():
        return reg_name(insn.reg, bits, insn.rex), [gpr(insn.reg, bits, insn.rex)]

    def imm(size):
        value = immediate(raw, size)
        return value & mask if form == "mask" else value

    if op in _ALU_FORMS:
        form, first = _ALU_FORMS[op]
        if insn.mod == 3 and insn.rm == insn.reg and form == "mask":
            name, regs = reg()
            return {"form": "self", "left": name, "right": None, "bits": bits, "registers": regs}
        (a, ra), (b, rb) = (rm(), reg()) if first == "rm" else (reg(), rm())
        return {"form": form, "left": a, "right": b, "bits": bits, "registers": ra + rb}
    if op in _ACC_FORMS:
        form = _ACC_FORMS[op]
        size = 1 if bits == 8 else (2 if bits == 16 else 4)
        return {"form": form, "left": reg_name(RAX, bits, insn.rex), "right": imm(size), "bits": bits,
                "registers": [RAX]}
    if op in (0x80, 0x81, 0x83) and insn.reg & 7 in _GROUP1:
        form = _GROUP1[insn.reg & 7]
        size = 1 if op in (0x80, 0x83) else (2 if bits == 16 else 4)
        left, regs = rm()
        return {"form": form, "left": left, "right": imm(size), "bits": bits, "registers": regs}
    if op == 0x1A3 or (op == 0x1BA and insn.reg & 7 == 4):     # bt r/m, r | bt r/m, imm8
        left, regs = rm()
        if op == 0x1A3:
            right, more = reg()
            return {"form": "bit", "left": left, "right": right, "bits": bits, "registers": regs + more}
        return {"form": "bit", "left": left, "right": raw[-1], "bits": bits, "registers": regs}
    if op in (0xF6, 0xF7) and insn.reg & 7 in (0, 1):
        form = "mask"
        size = 1 if op == 0xF6 else (2 if bits == 16 else 4)
        left, regs = rm()
        return {"form": form, "left": left, "right": imm(size), "bits": bits, "registers": regs}
    return None


def condition(test, cc: int):
    """The condition under which a jcc with condition code cc is taken after `test`, as text, or None."""
    form, left, right, bits = test["form"], test["left"], test["right"], test["bits"]
    if form == "bit":                          # bt: CF is the bit
        return {0x2: f"bit {right} of {left} is set", 0x3: f"bit {right} of {left} is clear"}.get(cc)
    if form in ("self", "mask"):
        rel = SELF_TEST.get(cc)
        if rel is None:
            return None
        if form == "self":
            return f"{left} {rel}"
        right = f"{right:#x}" if isinstance(right, int) else right
        return f"({left} & {right}) {rel}"
    rel = CMP_REL.get(cc)
    if rel is None:
        return None
    if isinstance(right, int):
        right = f"{right & ((1 << bits) - 1):#x}" if rel.endswith("u") else show(right)
    return f"{left} {rel} {right}"


def flags_setter(insns, k):
    """Index of the instruction that set the flags insns[k] (a jcc) reads, or None when a call, a branch or the
    function start comes first."""
    for j in range(k - 1, max(-1, k - 1 - FLAGS_BACK), -1):
        insn = insns[j]
        if insn.is_call() or insn.ends_flow():
            return None
        if is_jcc(insn) or insn.op in _NEUTRAL or (insn.op >= 0x1000 and insn.op not in _VEX_FLAGS) or (
                insn.op in (0xF6, 0xF7) and insn.reg & 7 == 2) or (insn.op == 0xFF and insn.reg & 7 == 6):
            continue
        return j
    return None


def load_bits(raw: bytes, insn) -> int:
    """Memory operand width in bits for an instruction in LOAD_OPS."""
    if insn.op in (0x8A, 0x1B6, 0x1BE):
        return 8
    if insn.op in (0x1B7, 0x1BF):
        return 16
    if insn.op == 0x63:
        return 32
    return width(raw, insn)


def step(image: Image, func, i: int, reg: int, level: int):
    """What instruction i of the function does to `reg`, walking back: None when it leaves it alone,
    ("follow", register, how) for a copy (how "copied") or an offset (how "derived"), ("result", report, tag) when
    it is where the value comes from (tag False for a report that is not about the value)."""
    insns = image.flow(func)[0]
    insn = insns[i]
    if insn.is_call():
        if reg == RAX:
            return "result", {"call": insn.rva, **image.call_name(insns, i)}, True
        if reg in VOLATILE:
            return "result", {"value": "clobbered by a call", "at": insn.rva}, False
        return None
    if reg not in insn_writes(insn):
        return None
    if insn.mod == 3 and insn.op in COPY_OPS:
        src = gpr(insn.rm, 8, insn.rex) if insn.op in (0x8A, 0x1B6, 0x1BE) else insn.rm
        return "follow", src, "copied" if src != reg else None
    if insn.mod == 3 and insn.op in (0x88, 0x89):
        src = gpr(insn.reg, 8, insn.rex) if insn.op == 0x88 else insn.reg
        return "follow", src, "copied" if src != reg else None
    if insn.op == 0x8D and insn.base not in (None, RIP) and insn.index is None:
        return "follow", insn.base, "derived"
    if insn.op in (0x81, 0x83) and insn.mod == 3 and insn.reg & 7 in (0, 5) and insn.rm == reg:
        return "follow", reg, "derived"
    if insn.op in LOAD_OPS and insn.mod != 3:
        bits = load_bits(image.raw(insn), insn)
        out = {"value": "loaded from memory", "at": insn.rva, "from": memory_name(insn, bits)}
        if insn.base == RIP:
            out["global"] = insn.target
        elif insn.base == RSP:
            out["value"] = "loaded from a stack slot"
        elif insn.base not in (None, RIP) and level < 2:
            out["base"] = {"register": NAMES64[insn.base], **trace(image, func, i, insn.base, level + 1)}
        return "result", out, True
    if (insn.op in (0x30, 0x31, 0x32, 0x33) and insn.mod == 3 and insn.reg == insn.rm) or (
            0xB0 <= insn.op <= 0xBF) or (insn.op in (0xC6, 0xC7) and insn.mod == 3):
        raw = image.raw(insn)
        if insn.op in (0x30, 0x31, 0x32, 0x33):
            value = 0
        elif insn.op < 0xB8 or insn.op == 0xC6:
            value = struct.unpack_from("<b", raw, len(raw) - 1)[0]
        elif 0xB8 <= insn.op <= 0xBF and insn.wide:
            value = struct.unpack_from("<q", raw, len(raw) - 8)[0]
        else:
            value = immediate(raw, 2 if width(raw, insn) == 16 else 4)
        return "result", {"value": "a constant", "constant": value, "at": insn.rva}, True
    return "result", {"value": "computed", "at": insn.rva}, True


def entry_value(reg: int):
    if reg in ARGUMENTS:
        return {"value": "argument", "argument": ARGUMENTS[reg], "argument_register": NAMES64[reg]}
    return {"value": "set before the function is entered"}


def with_how(out, copied: bool, derived: bool):
    if derived:
        out["derived"] = True
    elif copied:
        out["copied"] = True
    return out


def falls_into(insns, i: int) -> bool:
    """Whether instruction i - 1 falls through to instruction i."""
    return i > 0 and not (insns[i - 1].ends_flow() or insns[i - 1].op in (0xCC, 0xCD, 0x10B))


def _source_key(r):
    return (r.get("at", r.get("call")), r.get("value", "call"), r.get("argument"))


def trace(image: Image, func, k: int, reg: int, level: int = 0):
    """What wrote `reg` before the instruction at index k of the function. Followed back through register copies,
    `lea r, [r2+disp]` and adding a constant (marked derived), and into a block through the one jump that reaches
    it; a register no instruction wrote since the function start is an argument (rcx, rdx, r8, r9). For a load,
    the pointer register is traced too (two levels)."""
    insns, _, into = image.flow(func)
    copied = derived = False
    i = k
    for _ in range(TRACE_BACK):
        if i == 0:
            return with_how(entry_value(reg), copied, derived)
        if not falls_into(insns, i):
            ways = into.get(insns[i].rva, [])
            if len(ways) != 1:
                return {"value": "not found: the block it is in is reached by "
                                 + ("several jumps" if ways else "no jump this tool read")}
            i = ways[0]                        # the jump writes nothing; go on before it
            continue
        i -= 1
        r = step(image, func, i, reg, level)
        if r is None:
            continue
        if r[0] == "follow":
            reg = r[1]
            copied, derived = copied or r[2] == "copied", derived or r[2] == "derived"
            continue
        return with_how(r[1], copied, derived) if r[2] else r[1]
    return {"value": "not found within the instructions read"}


def trace_paths(image: Image, func, k: int, reg: int, budget: int = 4000, limit: int = 64):
    """The sources of `reg` before the instruction at index k, over the paths into it the search recovers (each
    instruction and register visited once, up to `budget` steps and `limit` sources)."""
    insns, _, into = image.flow(func)
    found, seen, stack, steps = [], set(), [(k, reg, False, False)], 0
    while stack and steps < budget and len(found) < limit:
        i, reg, copied, derived = stack.pop()
        steps += 1
        ways = ([i - 1] if falls_into(insns, i) else []) + into.get(insns[i].rva, [])
        if i == 0:
            found.append(with_how(entry_value(reg), copied, derived))
        elif not ways:
            found.append({"value": "not found: no way into this block was read"})
        for p in ways:
            if (p, reg) in seen:
                continue
            seen.add((p, reg))
            r = step(image, func, p, reg, 0)
            if r is None:
                stack.append((p, reg, copied, derived))
            elif r[0] == "follow":
                stack.append((p, r[1], copied or r[2] == "copied", derived or r[2] == "derived"))
            else:
                found.append(with_how(r[1], copied, derived) if r[2] else r[1])
    out, keys = [], set()
    for r in found:
        key = _source_key(r)
        if key not in keys:
            keys.add(key)
            out.append(r)
    return {"sources": out, "complete": not stack}


def guard(image: Image, func, k: int, taken: bool):
    """The guard at index k of the function: its condition for the doomed side and what the tested registers
    hold."""
    insns = image.flow(func)[0]
    insn = insns[k]
    cc = insn.op & 0xF
    out = {"branch": insn.rva, "jump": JCC[cc] if insn.op < 0xE0 or insn.op >= 0x100 else "loop/jrcxz",
           "doomed_side": "taken" if taken else "fall-through"}
    j = flags_setter(insns, k) if insn.op < 0xE0 or insn.op >= 0x100 else None
    if j is None:
        out["condition"] = None
        out["note"] = "no flag-setting instruction found before the branch"
        return out
    test = read_test(insns[j], image.raw(insns[j]))
    out["flags_at"] = insns[j].rva
    if test is None:
        out["condition"] = None
        out["note"] = "the flags come from an instruction this tool does not read"
        return out
    out["condition"] = condition(test, cc if taken else cc ^ 1)
    out["registers"] = [{"register": NAMES64[r], **trace(image, func, j, r)}
                        for r in dict.fromkeys(test["registers"])]
    return out


def rel32_sources(image: Image, targets: set[int], skip):
    """RVAs of `jmp rel32`, `jcc rel32` and `call rel32` in executable sections whose target is in `targets`,
    outside the range `skip`, each with its kind."""
    found = [(rva, kind, target) for target in targets for rva, kind in image.rel32_from(target)
             if not (skip[0] <= rva < skip[1])]
    return sorted(found)


def global_writers(image: Image, target: int):
    """Each `mov [rip+x], r` / `mov [rip+x], imm` storing to the global at `target`, with what it stores."""
    found = []
    seen_rvas = set()
    for at in image.stores_to(target):
        for back in (0, 1, 2):                 # the opcode, or a REX / 66 prefix before it
            rva = at - back
            if rva in seen_rvas:
                break
            func = image.function(rva)
            if func is None or not image.lands_on(func, rva):
                continue
            k = image.flow(func)[1][rva]
            insn = image.flow(func)[0][k]
            if (insn.op in (0x88, 0x89, 0xC6, 0xC7) and insn.base == RIP and insn.target == target
                    and (insn.op not in (0xC6, 0xC7) or insn.reg & 7 == 0)):
                seen_rvas.add(rva)
                found.append((rva, func, k, insn))
                break
    out = []
    for rva, func, k, insn in found[:MAX_STORES]:
        if insn.op in (0xC6, 0xC7):
            raw = image.raw(insn)
            value = immediate(raw, 1 if insn.op == 0xC6 else (2 if width(raw, insn) == 16 else 4))
            src = {"value": "a constant", "constant": value}
        else:
            reg = gpr(insn.reg, 8, insn.rex) if insn.op == 0x88 else insn.reg
            src = trace(image, func, k, reg)
        out.append({"at": rva, "function": list(func), "source": src})
    return {"global": target, "writers": out, "writers_not_read": max(0, len(found) - MAX_STORES)}


def argument_sources(image: Image, func, numbers):
    """For each argument number, what each direct call site of the function passes in it."""
    sites = [rva for rva, kind, _ in rel32_sources(image, {func[0]}, func) if kind == "call"]
    out = []
    for rva in sites[:MAX_SITES]:
        caller = image.function(rva)
        if caller is None or not image.lands_on(caller, rva):
            continue
        k = image.flow(caller)[1][rva]
        for n in numbers:
            reg = next(r for r, m in ARGUMENTS.items() if m == n)
            out.append({"at": rva, "caller": list(caller), "passed_as": n, "source": trace(image, caller, k, reg)})
    return {"sites": out, "sites_not_read": max(0, len(sites) - MAX_SITES)}


def explain(image: Image, seed: int, depth: int, seen: set, stop: bool = True):
    """Report for a stop (or a call that does not return, or a jump into doomed code) at `seed`. With stop False,
    seed is any instruction (`--any`): the guards decide whether it runs, and callers are not followed."""
    func = image.function(seed)
    if func is None:
        return {"at": seed, "function": None, "note": "no .pdata entry holds it"}
    insns = image.run(func)
    if not image.lands_on(func, seed):
        return {"at": seed, "function": list(func), "note": "decoding from the function start does not land on it"}
    doomed = doomed_code(insns, {seed})
    out = {"at": seed, "function": list(func), "guards": [], "jumps_in": [], "callers": []}
    for k, insn in enumerate(insns):
        if is_jcc(insn):
            t, f = insn.target in doomed, insn.end in doomed
            if t != f:
                out["guards"].append(guard(image, func, k, t))
    out["never_returns"] = stop and func[0] in doomed
    if not stop:
        out["any"] = True
        out["reached_from_start"] = func[0] in doomed
    if depth <= 0 or func in seen:
        return out
    seen = seen | {func}
    numbers = sorted({r["argument"] for g in out["guards"] for r in g.get("registers", [])
                      if r.get("value") == "argument"})
    if numbers:
        out["arguments"] = argument_sources(image, func, numbers)
    globals_read = []
    for g in out["guards"]:
        for r in g.get("registers", []):
            while r is not None:
                if "global" in r and r["global"] not in globals_read:
                    globals_read.append(r["global"])
                r = r.get("base")
    if globals_read:
        out["globals"] = [global_writers(image, x) for x in globals_read[:4]]
    links = [(rva, kind) for rva, kind, target in rel32_sources(image, doomed, func)
             if kind != "call" or (target == func[0] and out["never_returns"])]
    for rva, kind in links[:MAX_LINKS]:
        other = image.function(rva)
        if other is None or not image.lands_on(other, rva):
            continue
        if kind == "jcc":
            k = image.flow(other)[1][rva]
            out["jumps_in"].append({"from_function": list(other), **guard(image, other, k, True)})
        elif kind == "jmp":
            out["jumps_in"].append({"jump": "jmp", "branch": rva, "from": explain(image, rva, depth - 1, seen)})
        else:
            out["callers"].append(explain(image, rva, depth - 1, seen))
    out["links_not_followed"] = max(0, len(links) - MAX_LINKS)
    return out


def returns(image: Image, rva: int):
    """What the function holding rva returns in rax: the sources over the paths into each `ret` that the bounded
    search recovers, each with the branches that decide whether its instruction runs."""
    func = image.function(rva)
    if func is None:
        return {"at": rva, "function": None, "note": "no .pdata entry holds it"}
    insns = image.run(func)
    rets = [k for k, i in enumerate(insns) if i.op in (0xC3, 0xC2)]
    out, keys, complete = [], set(), True
    for k in rets:
        paths = trace_paths(image, func, k, RAX)
        complete = complete and paths["complete"]
        for x in paths["sources"]:
            key = _source_key(x)
            if key in keys:
                continue
            keys.add(key)
            at = x.get("call", x.get("at"))
            if at is not None:
                e = explain(image, at, 0, set(), stop=False)
                x["guards"], x["reached_from_start"] = e.get("guards", []), e.get("reached_from_start", False)
            out.append(x)
    return {"at": rva, "function": list(func), "rets": len(rets), "returns": out, "complete": complete}


def parse_address(text: str, image_base: int) -> int:
    m = re.fullmatch(r"(?:[\w.\-]+\+)?(?:0[xX])?([0-9a-fA-F]+)", text.strip())
    if not m:
        raise ValueError(f"not an address: {text!r}")
    value = int(m.group(1), 16)
    return value - image_base if value >= image_base else value


def cause(data: bytes, addresses, any_instruction: bool = False, returned: bool = False):
    """{address text: report} for each address. With any_instruction, an address may be any instruction; with
    returned, the report is what the function holding the address returns."""
    image = Image(data)
    out = {}
    for text in addresses:
        rva = parse_address(text, image.img.image_base)
        if returned:
            out[text] = {"rva": rva, "stop": "returns", **returns(image, rva)}
            continue
        if any_instruction:
            if image.code.decode(rva) is None:
                out[text] = {"rva": rva, "stop": None, "note": "no instruction decodes there"}
                continue
            out[text] = {"rva": rva, "stop": "instruction", **explain(image, rva, DEPTH, set(), stop=False)}
            continue
        stop = stop_at(image, rva)
        if stop is None:
            out[text] = {"rva": rva, "stop": None, "note": "not an int3, ud2 or int 29h (nor one byte past an int3)"}
            continue
        at, kind = stop
        out[text] = {"rva": rva, "stop": kind, **explain(image, at, DEPTH, set())}
    return out


# ---- text output ----

def source(r, where: bool = False) -> str:
    """What a traced register holds, as text (without the register's name); with `where`, the RVAs too."""
    how = "derived from " if r.get("derived") else ("a copy of " if r.get("copied") else "")
    if "call" in r:
        if "import" in r:
            what = r["import"]
        elif "slot" in r:
            what = f"vtable slot {r['slot']}" + (f" (this: global {r['this_global']:#x})" if "this_global" in r
                                                 else "")
        elif "function" in r:
            what = f"function {r['function']:#x}"
        elif "pointer" in r:
            what = f"the function pointer at {r['pointer']:#x}"
        else:
            what = "a call through a register"
        return f"{how}the result of {what}" + (f", call at {r['call']:#x}" if where else "")
    if r["value"] == "argument":
        return f"{how}argument {r['argument']} ({r['argument_register']})"
    src = f" ({r['from']})" if "from" in r else (f" ({show(r['constant'])})" if "constant" in r else "")
    at = f" at {r['at']:#x}" if where and "at" in r else ""
    base = f"; {r['base']['register']}: {source(r['base'], where)}" if "base" in r else ""
    return f"{how}{r['value']}{src}{at}{base}"


def describe_register(r) -> str:
    return f"{r['register']}: {source(r, True)}"


def tally_lines(items, indent: str):
    tally = {}
    for what in items:
        tally[what] = tally.get(what, 0) + 1
    return [f"{indent}  {count:4}  {what}" for what, count in sorted(tally.items(), key=lambda kv: (-kv[1], kv[0]))]


def global_lines(globs, indent: str):
    lines = []
    for g in globs:
        n = len(g["writers"]) + g["writers_not_read"]
        lines.append(f"{indent}global {g['global']:#x} is written at {n} site(s)" + (":" if n else
                     " this tool reads (none by a direct `mov`: through a pointer or not at all)"))
        lines += tally_lines([source(w["source"]) for w in g["writers"]], indent)
        if g["writers_not_read"]:
            lines.append(f"{indent}  {g['writers_not_read']} more not read")
    return lines


def argument_lines(args, indent: str):
    """A tally of what the call sites pass, most frequent first."""
    sites = args["sites"]
    lines = []
    for n in sorted({x["passed_as"] for x in sites}):
        passed = [source(x["source"]) for x in sites if x["passed_as"] == n]
        lines.append(f"{indent}argument {n} at the function's {len(passed)} direct call site(s):")
        lines += tally_lines(passed, indent)
    if args.get("sites_not_read"):
        lines.append(f"{indent}{args['sites_not_read']} more call site(s) not read")
    return lines


def guard_lines(g, indent: str, verb: str = "stops when"):
    head = f"{indent}{g['jump']} at {g['branch']:#x}, {g['doomed_side']} side: "
    if g.get("condition"):
        lines = [head + f"{verb} {g['condition']}"]
    else:
        where = f" (at {g['flags_at']:#x})" if "flags_at" in g else ""
        lines = [head + g.get("note", "condition not read") + where]
    for r in g.get("registers", []):
        lines.append(f"{indent}  {describe_register(r)}")
    return lines


def report_lines(rep, indent: str = "  "):
    if rep.get("function") is None or "guards" not in rep:
        return [f"{indent}{rep.get('note')}"]
    lines = []
    for g in rep["guards"]:
        lines += guard_lines(g, indent, "runs when" if rep.get("any") else "stops when")
    if rep.get("arguments"):
        lines += argument_lines(rep["arguments"], indent)
    if rep.get("globals"):
        lines += global_lines(rep["globals"], indent)
    for j in rep["jumps_in"]:
        if j.get("jump") == "jmp":
            f = j["from"]
            span = f"function {f['function'][0]:#x}" if f.get("function") else "no function"
            lines.append(f"{indent}jmp at {j['branch']:#x} from {span}:")
            lines += report_lines(f, indent + "  ")
        else:
            lines.append(f"{indent}from function {j['from_function'][0]:#x}:")
            lines += guard_lines(j, indent + "  ")
    if rep.get("never_returns") and rep["callers"]:
        lines.append(f"{indent}the function never returns once entered; its direct call sites "
                     f"({len(rep['callers'])}):")
        for c in rep["callers"]:
            span = f"function {c['function'][0]:#x}" if c.get("function") else "no function"
            lines.append(f"{indent}  call at {c['at']:#x} in {span}:")
            lines += report_lines(c, indent + "    ")
    if not lines:
        if rep.get("reached_from_start"):
            lines.append(f"{indent}no guard: every path from the function start reaches it")
        else:
            lines.append(f"{indent}no guard found: the stop is reached unconditionally from the function start, "
                         f"and nothing calls or jumps to it directly (a jump table or a pointer, if anything)")
    if rep.get("links_not_followed"):
        lines.append(f"{indent}{rep['links_not_followed']} more jump(s) or call(s) into it not followed")
    return lines


def returns_lines(rep, indent: str = "  "):
    lines = []
    for x in rep["returns"]:
        lines.append(f"{indent}{source(x, True)}:")
        if x.get("reached_from_start"):
            lines.append(f"{indent}  on every path from the function start (unless set again later)")
        elif x.get("guards"):
            for g in x["guards"]:
                lines += guard_lines(g, indent + "  ", "runs when")
        elif "guards" in x:
            lines.append(f"{indent}  no guard read (reached through a loop, a jump table or another function)")
    if not rep["complete"]:
        lines.append(f"{indent}(not every path was read: the search stopped at its limit)")
    return lines


def to_text(results) -> str:
    lines = []
    for text, rep in results.items():
        if rep["stop"] is None:
            lines.append(f"{text}: {rep['note']}")
            continue
        span = (f"function {rep['function'][0]:#x}-{rep['function'][1]:#x}" if rep.get("function")
                else "no function")
        if rep["stop"] == "returns":
            lines.append(f"{text}: {span}, {rep['rets']} ret(s); it returns:" if rep.get("function") else
                         f"{text}: {rep.get('note')}")
            lines += returns_lines(rep) if rep.get("function") else []
            continue
        lines.append(f"{text}: {rep['stop']} at {rep['at']:#x}, {span}")
        lines += report_lines(rep)
    return "\n".join(lines) + "\n"


def readable(results) -> bool:
    return all(r["stop"] is not None and r.get("function") is not None and ("guards" in r or "returns" in r)
               for r in results.values())


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Name the call whose result sends an x86-64 PE image to an int3.")
    ap.add_argument("image")
    ap.add_argument("addresses", nargs="+", metavar="address",
                    help="the stop: exe+0x..., an RVA or a virtual address (hex)")
    ap.add_argument("--any", action="store_true",
                    help="an address may be any instruction (a `return 0` path, a fallback): print the branches "
                         "that decide whether it runs")
    ap.add_argument("--returns", action="store_true",
                    help="an address may be anywhere in a function: print what the function returns in rax, each "
                         "value with where it is set and the branches that decide that")
    ap.add_argument("--json", action="store_true", help="print JSON instead of text")
    args = ap.parse_args(argv)
    try:
        with open(args.image, "rb") as f:
            data = f.read()
        results = cause(data, args.addresses, args.any, args.returns)
    except (OSError, pe_inspect.PeFormatError, ValueError) as e:
        print(f"{os.path.basename(sys.argv[0])}: {e}", file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps(results, indent=1))
    else:
        sys.stdout.write(to_text(results))
    return 0 if readable(results) else 1


if __name__ == "__main__":
    try:  # a redirected stdout is strict ANSI code page under -I; write '?' for what it cannot encode
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError, OSError):
        pass
    sys.exit(main())
