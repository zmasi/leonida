"""x86-64 to arm64 static recompiler groundwork (the mobile "custom emulator" core).

The PC port runs the game's x86-64 code natively; on Apple silicon that is impossible, so the
mobile path recompiles it ahead of time (docs/mobile.md). This file holds the three pieces later
tools build on:

  * a length-and-operand x86-64 decoder (the first stage of any static recompiler),
  * an arm64 emitter (encodings only, no assembler dependency),
  * a function translator for the integer subset, plus a static aarch64 Linux ELF builder so
    translated code runs under qemu-aarch64 (or arm64 hardware) for verification.

Runtime contract (full version in docs/mobile.md):

  * guest rax..r15 live in fixed host regs: rax..r8 -> x19..x27, r9..r15 -> x9..x15. x28 is the
    context pointer; x18 is never touched (Darwin reserves it). x0-x8, x16, x17, x30 are scratch.
  * guest virtual addresses are host virtual addresses (identity mapping, like the PE loader).
  * x86 flags ride arm NZCV between a flag writer and its consumer when the consumer has exactly
    one reaching producer in the same block; subtract-kind producers keep arm-native C and the
    consumer's condition table compensates. Otherwise the producer stores a canonical x86 flag
    word in ctx (CF normalized there) and the consumer evaluates that. PF/AF are computed from
    ctx.res / ctx.a / ctx.b on demand.
  * every indirect transfer (indirect jmp/call, ret, and any target outside the function) goes
    through rt_dispatch, which maps a guest pc to a translated host pc through a generated table.
    Guest pc 0 is the exit sentinel used by the test runtime.

Standard library only; runs under `python -I` like the other tools.

CLI:
    python -I tools/x64_recomp.py --disasm <file.bin> [--base VA]
"""
from __future__ import annotations

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# ---------------------------------------------------------------------------
# x86-64 decoder
# ---------------------------------------------------------------------------

REG_NAMES = ("rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
             "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15")

CC_NAMES = ("o", "no", "b", "ae", "e", "ne", "be", "a",
            "s", "ns", "p", "np", "l", "ge", "le", "g")

# x86 flags each condition reads. N=SF Z=ZF C=CF V=OF P=PF A=AF.
CC_FLAGS = {
    "o": "V", "no": "V", "b": "C", "ae": "C", "e": "Z", "ne": "Z",
    "be": "CZ", "a": "CZ", "s": "N", "ns": "N", "p": "P", "np": "P",
    "l": "NV", "ge": "NV", "le": "ZNV", "g": "ZNV",
}

FLAG_WRITERS = {
    "add": "NZCVPA", "or": "NZCVPA", "adc": "NZCVPA", "sbb": "NZCVPA",
    "and": "NZCVPA", "sub": "NZCVPA", "xor": "NZCVPA", "cmp": "NZCVPA",
    "test": "NZCVPA", "neg": "NZCVPA",
    "inc": "NZVPA", "dec": "NZVPA",
    "shl": "NZCP", "shr": "NZCP", "sar": "NZCP", "sal": "NZCP",
    "rol": "C", "ror": "C", "rcl": "C", "rcr": "C",
    "imul": "CV", "mul": "CV",
    "bsf": "Z", "bsr": "Z",
    "bt": "C", "bts": "C", "btr": "C", "btc": "C",
    "popcnt": "Z",
    "cmps": "NZCVPA", "scas": "NZCVPA",
    "sahf": "NZCPA", "popf": "NZCVPA",
    "clc": "C", "stc": "C", "cmc": "C",
}

FLAG_READERS = {
    "adc": "C", "sbb": "C", "rcl": "C", "rcr": "C", "cmc": "C",
    "lahf": "NZCPA", "pushf": "NZCVPA",
}

# producer-kind groupings used by the condition tables
SUB_KIND = {"sub", "cmp", "sbb", "dec", "neg", "cmps", "scas"}
# producers whose arm C already holds the x86-polarity carry/borrow value
# (explicit materialization for shifts/multiplies, register load for popf/sahf)
ADD_KIND = {"add", "adc", "inc", "shl", "sal", "shr", "sar", "rol", "ror",
            "popf", "sahf", "mul", "imul", "clc", "stc", "cmc"}
LOGIC_KIND = {"and", "or", "xor", "test"}

_ALU = ("add", "or", "adc", "sbb", "and", "sub", "xor", "cmp")
_GROUP1 = _ALU
_GROUP2 = ("rol", "ror", "rcl", "rcr", "shl", "shr", "sal", "sar")
_GROUP3 = ("test", "test", "not", "neg", "mul", "imul", "div", "idiv")
_GROUP4 = ("inc", "dec", None, None, None, None, None, None)
_GROUP5 = ("inc", "dec", "call", "callf", "jmp", "jmpf", "push", None)
_GROUP8 = (None, None, None, None, "bt", "bts", "btr", "btc")
_GROUP15 = ("fxsave", "fxrstor", "ldmxcsr", "stmxcsr", "xsave", "xrstor", "xsaveopt", "fence")
# 0F 01 /r: memory forms select by the ModRM reg field; the register forms are whole
# ModRM bytes (0F 01 C1 is VMCALL, 0F 01 D0 is XGETBV, 0F 01 FC is CLZERO, ...).
_GROUP7 = ("sgdt", "sidt", "lgdt", "lidt", "smsw", None, "lmsw", "invlpg")
_GROUP7_REG = {
    0xC1: "vmcall", 0xC2: "vmlaunch", 0xC3: "vmresume", 0xC4: "vmxoff",
    0xC8: "monitor", 0xC9: "mwait", 0xCA: "clac", 0xCB: "stac",
    0xD0: "xgetbv", 0xD1: "xsetbv", 0xD4: "vmfunc", 0xD5: "xend", 0xD6: "xtest",
    0xD8: "vmrun", 0xD9: "vmmcall", 0xDA: "vmload", 0xDB: "vmsave",
    0xDC: "stgi", 0xDD: "clgi", 0xDE: "skinit", 0xDF: "invlpga",
    0xEE: "rdpkru", 0xEF: "wrpkru",
    0xF8: "swapgs", 0xF9: "rdtscp", 0xFA: "monitorx", 0xFB: "mwaitx", 0xFC: "clzero",
}

PREFIXES = {0x66, 0x67, 0xF0, 0xF2, 0xF3, 0x2E, 0x36, 0x3E, 0x26, 0x64, 0x65}
SEG_PREFIX = {0x2E: "cs", 0x36: "ss", 0x3E: "ds", 0x26: "es", 0x64: "fs", 0x65: "gs"}

# operand spec letters: E=modrm r/m, G=modrm reg, M=modrm mem, U=modrm xmm-only,
# V=modrm reg xmm, W=modrm r/m xmm, H=vex vvvv, I=imm, J=rel, O=opcode reg,
# A=accumulator, c=CL, 1=const 1, S=segment reg.
PRIMARY = {}
for _i, _m in enumerate(_ALU):
    _b = _i * 8
    PRIMARY[_b + 0] = (_m, "Eb Gb")
    PRIMARY[_b + 1] = (_m, "Ev Gv")
    PRIMARY[_b + 2] = (_m, "Gb Eb")
    PRIMARY[_b + 3] = (_m, "Gv Ev")
    PRIMARY[_b + 4] = (_m, "Ab Ib")
    PRIMARY[_b + 5] = (_m, "Av Iz")
PRIMARY.update({
    0x63: ("movsxd", "Gv Ed"),
    0x68: ("push", "Iz"), 0x69: ("imul", "Gv Ev Iz"), 0x6A: ("push", "Ib"),
    0x6B: ("imul", "Gv Ev Ib"),
    0x6C: ("ins", "sys"), 0x6D: ("ins", "sys"), 0x6E: ("outs", "sys"), 0x6F: ("outs", "sys"),
    0x80: ("grp1", "Eb Ib"), 0x81: ("grp1", "Ev Iz"), 0x83: ("grp1", "Ev Ib"),
    0x84: ("test", "Eb Gb"), 0x85: ("test", "Ev Gv"),
    0x86: ("xchg", "Eb Gb"), 0x87: ("xchg", "Ev Gv"),
    0x88: ("mov", "Eb Gb"), 0x89: ("mov", "Ev Gv"), 0x8A: ("mov", "Gb Eb"), 0x8B: ("mov", "Gv Ev"),
    0x8C: ("movseg", "Ev Sw"), 0x8D: ("lea", "Gv M"), 0x8E: ("movseg", "Sw Ev"),
    0x8F: ("grppop", "Eq"),
    0x98: ("cbtw", ""), 0x99: ("cwtd", ""),
    0x9A: ("callf", "invalid"), 0x9B: ("fwait", ""),
    0x9C: ("pushf", ""), 0x9D: ("popf", ""), 0x9E: ("sahf", ""), 0x9F: ("lahf", ""),
    0xA0: ("mov", "Ab Qb"), 0xA1: ("mov", "Av Qv"), 0xA2: ("mov", "Qb Ab"), 0xA3: ("mov", "Qv Av"),
    0xA4: ("movs", "Zb"), 0xA5: ("movs", "Zv"), 0xA6: ("cmps", "Zb"), 0xA7: ("cmps", "Zv"),
    0xA8: ("test", "Ab Ib"), 0xA9: ("test", "Av Iz"),
    0xAA: ("stos", "Zb"), 0xAB: ("stos", "Zv"), 0xAC: ("lods", "Zb"), 0xAD: ("lods", "Zv"),
    0xAE: ("scas", "Zb"), 0xAF: ("scas", "Zv"),
    0xC0: ("grp2", "Eb Ib"), 0xC1: ("grp2", "Ev Ib"),
    0xC2: ("ret", "Iw"), 0xC3: ("ret", ""),
    0xC6: ("grpmov", "Eb Ib"), 0xC7: ("grpmov", "Ev Iz"),
    0xC8: ("enter", "Iw Ib"), 0xC9: ("leave", ""),
    0xCA: ("retf", "invalid"), 0xCB: ("retf", "invalid"),
    0xCC: ("int3", ""), 0xCD: ("int", "Ib"), 0xCE: ("into", "invalid"), 0xCF: ("iret", ""),
    0xD0: ("grp2", "Eb 1"), 0xD1: ("grp2", "Ev 1"), 0xD2: ("grp2", "Eb c"), 0xD3: ("grp2", "Ev c"),
    0xD4: ("aam", "invalid"), 0xD5: ("aad", "invalid"), 0xD6: ("salc", "invalid"), 0xD7: ("xlat", ""),
    0xE0: ("loopne", "Jb"), 0xE1: ("loope", "Jb"), 0xE2: ("loop", "Jb"), 0xE3: ("jcxz", "Jb"),
    0xE4: ("in", "sys"), 0xE5: ("in", "sys"), 0xE6: ("out", "sys"), 0xE7: ("out", "sys"),
    0xE8: ("call", "Jz"), 0xE9: ("jmp", "Jz"), 0xEA: ("jmpf", "invalid"), 0xEB: ("jmp", "Jb"),
    0xEC: ("in", "sys"), 0xED: ("in", "sys"), 0xEE: ("out", "sys"), 0xEF: ("out", "sys"),
    0xF1: ("int1", ""), 0xF4: ("hlt", ""), 0xF5: ("cmc", ""),
    0xF6: ("grp3", "Eb"), 0xF7: ("grp3", "Ev"),
    0xF8: ("clc", ""), 0xF9: ("stc", ""), 0xFA: ("cli", ""), 0xFB: ("sti", ""),
    0xFC: ("cld", ""), 0xFD: ("std", ""),
    0xFE: ("grp4", "Eb"), 0xFF: ("grp5", "Ev"),
})
for _i in range(8):
    PRIMARY[0x50 + _i] = ("push", "Oq")
    PRIMARY[0x58 + _i] = ("pop", "Oq")
    PRIMARY[0xB0 + _i] = ("mov", "Ob Ib")
    PRIMARY[0xB8 + _i] = ("mov", "Ov Iv")
for _i in range(16):
    PRIMARY[0x70 + _i] = ("jcc", "Jb")
for _i in range(1, 8):
    PRIMARY[0x90 + _i] = ("xchg", "Oq Oq")
# 0x90 is handled below as NOP, PAUSE, or XCHG r8, RAX when REX.B is set.
# Keep it in the table so that special case is reached by the decoder.
PRIMARY[0x90] = ("nop", "")
for _i in range(8):
    PRIMARY[0xD8 + _i] = ("x87", "E")

OF = {
    0x01: ("grp7", "E"),
    0x05: ("syscall", "sys"), 0x07: ("sysret", "sys"), 0x0B: ("ud2", ""),
    0x0D: ("nop", "Ev"), 0x1E: ("endbr", "Eb"), 0x1F: ("nop", "Ev"),
    0x31: ("rdtsc", "sys"), 0x32: ("rdmsr", "sys"), 0x33: ("rdpmc", "sys"),
    0x34: ("sysenter", "sys"), 0x35: ("sysexit", "sys"),
    0xA0: ("pushseg", "fs"), 0xA1: ("popseg", "fs"), 0xA8: ("pushseg", "gs"), 0xA9: ("popseg", "gs"),
    0xA2: ("cpuid", "sys"),
    0xA3: ("bt", "Ev Gv"), 0xAB: ("bts", "Ev Gv"), 0xB3: ("btr", "Ev Gv"), 0xBB: ("btc", "Ev Gv"),
    0xAF: ("imul", "Gv Ev"), 0xBC: ("bsf", "Gv Ev"), 0xBD: ("bsr", "Gv Ev"),
    0xB0: ("cmpxchg", "Eb Gb"), 0xB1: ("cmpxchg", "Ev Gv"),
    0xB8: ("popcnt_jmpe", "Gv Ev"), 0xB9: ("ud1", "Ev"), 0xBA: ("grp8", "Ev Ib"),
    0xB6: ("movzx", "Gv Eb"), 0xB7: ("movzx", "Gv Ew"),
    0xBE: ("movsx", "Gv Eb"), 0xBF: ("movsx", "Gv Ew"),
    0xC0: ("xadd", "Eb Gb"), 0xC1: ("xadd", "Ev Gv"),
    0xC3: ("movnti", "Ev Gv"),
    0xAE: ("grp15", "E"),
    0x77: ("emms", ""),
    0x06: ("clts", "sys"), 0x09: ("wbinvd", "sys"),
    0x18: ("nop", "Ev"), 0x19: ("nop", "Ev"), 0x1A: ("nop", "Ev"), 0x1B: ("nop", "Ev"),
    0x1C: ("nop", "Ev"), 0x1D: ("nop", "Ev"),
    0x78: ("sse", "Ev Ib"), 0x79: ("sse", "Ev"),
}
for _i in range(16):
    OF[0x80 + _i] = ("jcc", "Jz")
    OF[0x90 + _i] = ("setcc", "Eb")
    OF[0x40 + _i] = ("cmov", "Gv Ev")
for _i in range(8):
    OF[0xC8 + _i] = ("bswap", "Ov")

_OF_SIMD_IMM8 = {0x70, 0x71, 0x72, 0x73, 0xC2, 0xC4, 0xC5, 0xC6}
_OF_SIMD_NORM = (list(range(0x10, 0x18)) + list(range(0x28, 0x30)) + [0x50] +
                 list(range(0x51, 0x6E)) + [0x6E, 0x6F, 0x74, 0x75, 0x76, 0x7E, 0x7F, 0xD6] +
                 list(range(0xD1, 0xD6)) + list(range(0xD8, 0xE0)) + list(range(0xE0, 0xE6)) +
                 list(range(0xE8, 0xF0)) + list(range(0xF1, 0xF7)) + list(range(0xF8, 0xFF)))
for _op in _OF_SIMD_NORM:
    OF.setdefault(_op, ("simd", "V W"))
for _op in _OF_SIMD_IMM8:
    OF[_op] = ("simd", "V W Ib")
OF[0x6E] = ("movd", "V Ed")
OF[0x7E] = ("movd", "Ed V")

_VEX_IMM8_MAP1 = {0x70, 0x71, 0x72, 0x73, 0xC2, 0xC4, 0xC5, 0xC6}


class Insn:
    __slots__ = ("addr", "size", "mnem", "ops", "kind", "cc", "flags_w", "flags_r",
                 "rep", "lock", "seg", "opsize", "rexw", "has_rex", "strsize",
                 "addrsize", "unknown", "error")

    def __init__(self):
        self.addr = 0
        self.size = 0
        self.mnem = "?"
        self.ops = ()
        self.kind = "misc"
        self.cc = None
        self.flags_w = ""
        self.flags_r = ""
        self.rep = ""
        self.lock = False
        self.seg = None
        self.opsize = 0
        self.rexw = 0
        self.has_rex = False
        self.strsize = 0
        self.addrsize = 0
        self.unknown = False
        self.error = None

    def __repr__(self):
        return f"{self.addr:#x}: {self.mnem} " + " ".join(_fmt_op(o) for o in self.ops)


def _fmt_op(o):
    if o[0] == "r":
        name = REG_NAMES[o[1]] + (".hi8" if len(o) > 3 and o[3] else "")
        return f"{name}/{o[2]}"
    if o[0] == "i":
        return f"{o[1]:#x}"
    if o[0] == "a":
        return f"->{o[1]:#x}"
    if o[0] == "m":
        m = o[1]
        base = "rip" if m["rip"] else (REG_NAMES[m["base"]] if m["base"] is not None else "")
        idx = "" if m["index"] is None else "+" + REG_NAMES[m["index"]] + f"*{1 << m['scale']}"
        return f"[{base}{idx}{m['disp']:+x}]/{m['size']}"
    return str(o)


class DecodeError(Exception):
    pass


def _vsize(opsize, rexw):
    if rexw:
        return 8
    return 2 if opsize else 4


def _size_of(letter, opsize, rexw):
    return {"b": 1, "w": 2, "d": 4, "q": 8,
            "v": _vsize(opsize, rexw),
            "z": 2 if opsize else 4}.get(letter, 0)


def _read_imm(buf, pos, end, n, signed=True):
    if pos + n > end:
        raise DecodeError("truncated imm")
    fmt = {1: "<b" if signed else "<B", 2: "<h" if signed else "<H",
           4: "<i" if signed else "<I", 8: "<q" if signed else "<Q"}[n]
    return struct.unpack_from(fmt, buf, pos)[0], pos + n


def _modrm(buf, pos, end, rex_r, rex_x, rex_b, has_rex):
    if pos >= end:
        raise DecodeError("truncated modrm")
    modrm = buf[pos]
    pos += 1
    mod, reg, rm = modrm >> 6, (modrm >> 3) & 7, modrm & 7
    reg_field = reg | (rex_r << 3)
    mem = {"base": None, "index": None, "scale": 0, "disp": 0, "rip": False,
           "size": 0, "seg": None, "abs": None}
    rm_reg = None
    if mod == 3:
        rm_reg = rm | (rex_b << 3)
    else:
        need_disp32 = False
        if rm == 4:
            if pos >= end:
                raise DecodeError("truncated sib")
            sib = buf[pos]
            pos += 1
            mem["scale"], idx, bas = sib >> 6, (sib >> 3) & 7, sib & 7
            idx_ext = idx | (rex_x << 3)
            if idx_ext != 4:
                mem["index"] = idx_ext
            if bas == 5 and mod == 0:
                need_disp32 = True
            else:
                mem["base"] = bas | (rex_b << 3)
        elif rm == 5 and mod == 0:
            mem["rip"] = True
            need_disp32 = True
        else:
            mem["base"] = rm | (rex_b << 3)
        if mod == 1:
            if pos + 1 > end:
                raise DecodeError("truncated disp8")
            mem["disp"] = struct.unpack_from("<b", buf, pos)[0]
            pos += 1
        elif mod == 2 or need_disp32:
            if pos + 4 > end:
                raise DecodeError("truncated disp32")
            mem["disp"] = struct.unpack_from("<i", buf, pos)[0]
            pos += 4

    def make(size):
        if rm_reg is not None:
            if size == 1 and not has_rex and 4 <= rm_reg <= 7:
                return ("r", rm_reg - 4, 1, True)
            return ("r", rm_reg, size)
        m = dict(mem)
        m["size"] = size
        return ("m", m)

    return reg_field, make, pos, mod


def _finish(buf, pos, end, ins, start, mnem, opspec, state, addr, opcode_byte):
    """opspec-driven operand parse. state = dict(opsize, rexw, rex bits, has_rex, vex)."""
    opsize, rexw = state["opsize"], state["rexw"]
    rexr, rexx, rexb = state["rexr"], state["rexx"], state["rexb"]
    has_rex, vex = state["has_rex"], state["vex"]

    has_modrm = opspec and any(t[0] in "EGMUW" for t in opspec.split())
    reg_field, rm_make = 0, None
    if has_modrm:
        reg_field, rm_make, pos, _ = _modrm(buf, pos, end, rexr, rexx, rexb, has_rex)

    ops = []
    for tok in opspec.split() if opspec else ():
        t, s = tok[0], tok[1] if len(tok) > 1 else ""
        if t == "G":
            hi8 = s == "b" and not has_rex and 4 <= reg_field <= 7
            ops.append(("r", reg_field - 4 if hi8 else reg_field,
                        _size_of(s, opsize, rexw), hi8))
        elif t in "EMUW":
            ops.append(rm_make(_size_of(s, opsize, rexw)))
        elif t == "H":
            ops.append(("r", vex["vvvv"] if vex else 0, 16))
        elif t == "O":
            r = (opcode_byte & 7) | (rexb << 3)
            hi8 = s == "b" and not has_rex and 4 <= r <= 7
            ops.append(("r", r - 4 if hi8 else r, _size_of(s, opsize, rexw), hi8))
        elif t == "I":
            n = _size_of(s, opsize, rexw)
            n = min(n, 4) if s == "z" else n
            v, pos = _read_imm(buf, pos, end, n, s != "v")
            ops.append(("i", v, _size_of(s, opsize, rexw) or n))
        elif t == "J":
            n = 1 if s == "b" else 4
            v, pos = _read_imm(buf, pos, end, n, True)
            ops.append(("a", addr + pos + v, 0))
        elif t == "A":
            ops.append(("r", 0, _size_of(s, opsize, rexw)))
        elif t == "R":
            # Fixed RAX operand, unaffected by REX.B.
            ops.append(("r", 0, _size_of(s, opsize, rexw), False))
        elif t == "c":
            ops.append(("r", 1, 1))
        elif t == "1":
            ops.append(("i", 1, 1))
        elif t == "S":
            ops.append(("seg", reg_field, 0))
        elif t == "Q":
            v, pos = _read_imm(buf, pos, end, 4 if ins.addrsize else 8, signed=False)
            ops.append(("m", {"base": None, "index": None, "scale": 0, "disp": 0,
                              "rip": False, "size": _size_of(s, opsize, rexw),
                              "seg": ins.seg, "abs": v, "moffs": True}))
        elif t == "Z":
            ins.strsize = _size_of(s, opsize, rexw)

    ins.mnem = mnem
    ins.ops = tuple(ops)
    ins.size = pos - start
    for o in ins.ops:
        if o[0] == "m":
            if ins.addrsize:
                o[1]["addrsize"] = 4
            if o[1]["rip"]:
                abs_a = ins.addr + ins.size + o[1]["disp"]
                o[1]["abs"] = abs_a & 0xFFFFFFFF if ins.addrsize else abs_a
    _classify(ins, mnem, opcode_byte & 0xF, state)
    return ins


def _cond_flag_writer(ins):
    """A shift or rotate by cl: x86 changes no flag when its masked count is 0, so it writes its
    flags only on some runs. Flag analysis treats it as a writer that may leave them alone."""
    return ins.kind == "shift" and len(ins.ops) >= 2 and ins.ops[1][0] == "r"


def _classify(ins, mnem, cc_index, state):
    cc = cc_index
    if mnem == "jcc":
        ins.kind, ins.cc = "branch", CC_NAMES[cc]
        ins.flags_r = CC_FLAGS[ins.cc]
    elif mnem == "jmp":
        ins.kind, ins.cc = "branch", "al"
    elif mnem == "call":
        ins.kind = "call"
    elif mnem == "ret":
        ins.kind = "ret"
    elif mnem == "setcc":
        ins.kind, ins.cc = "cc", CC_NAMES[cc]
        ins.flags_r = CC_FLAGS[ins.cc]
    elif mnem == "cmov":
        ins.kind, ins.cc = "cc", CC_NAMES[cc]
        ins.flags_r = CC_FLAGS[ins.cc]
    elif mnem in ("loop", "loope", "loopne", "jcxz"):
        ins.kind, ins.cc = "branch", mnem
        ins.flags_r = "Z" if mnem in ("loope", "loopne") else ""
    elif mnem in ("movs", "stos", "lods", "scas", "cmps"):
        ins.kind = "str"
        if mnem in ("cmps", "scas"):
            ins.flags_w = "NZCVPA"
            ins.flags_r = "NZCVPA"
    elif mnem in _ALU or mnem in ("neg", "test"):
        ins.kind = "alu"
        ins.flags_w = FLAG_WRITERS[mnem]
        ins.flags_r = FLAG_READERS.get(mnem, "")
    elif mnem in ("inc", "dec"):
        ins.kind = "alu"
        ins.flags_w = FLAG_WRITERS[mnem]
    elif mnem in ("shl", "shr", "sar", "sal", "rol", "ror", "rcl", "rcr"):
        ins.kind = "shift"
        is_imm_zero = False
        if len(ins.ops) >= 2 and ins.ops[1][0] == "i":
            sz = ins.ops[0][1]["size"] if ins.ops[0][0] == "m" else ins.ops[0][2]
            width = sz * 8
            cval = (ins.ops[1][1] & 0x1F) if width < 64 else (ins.ops[1][1] & 0x3F)
            if cval == 0:
                is_imm_zero = True
        if is_imm_zero:
            ins.flags_w = ""
            ins.flags_r = ""
        else:
            ins.flags_w = FLAG_WRITERS[mnem]
            ins.flags_r = FLAG_READERS.get(mnem, "")
    elif mnem in ("imul", "mul", "div", "idiv"):
        ins.kind = "mul"
        ins.flags_w = FLAG_WRITERS.get(mnem, "")
    elif mnem in ("mov", "movzx", "movsx", "movsxd", "movnti", "movd"):
        ins.kind = "mov"
    elif mnem == "lea":
        ins.kind = "lea"
    elif mnem in ("push", "pushseg", "pushf"):
        ins.kind = "push"
        ins.flags_r = FLAG_READERS.get(mnem, "")
    elif mnem in ("movbe", "crc32"):
        ins.kind = "misc"
    elif mnem in ("pop", "popseg", "popf"):
        ins.kind = "pop"
        ins.flags_w = "NZCVPA" if mnem == "popf" else ""
    elif mnem in ("xchg", "cmpxchg", "xadd"):
        ins.kind = "xchg"
        if mnem != "xchg":
            ins.flags_w = "NZCVPA"
            ins.flags_r = "NZCVPA"
    elif mnem in ("cbtw", "cwtd"):
        ins.kind = "conv"
    elif mnem == "bswap":
        ins.kind = "misc"
    elif mnem in ("bsf", "bsr", "popcnt"):
        ins.kind = "alu"
        ins.flags_w = FLAG_WRITERS.get(mnem, "")
    elif mnem in ("bt", "bts", "btr", "btc"):
        ins.kind = "bt"
        ins.flags_w = "C"
        ins.flags_r = "C" if mnem != "bt" else ""
    elif mnem in ("nop", "endbr", "fwait", "emms"):
        ins.kind = "nop"
        if mnem == "nop" and ins.rep == "f3":
            ins.mnem = "pause"
    elif mnem in ("xlat", "enter"):
        ins.kind = "misc"
    elif mnem in ("clc", "stc", "cmc"):
        ins.kind = "alu"
        ins.flags_w = "C"
        ins.flags_r = "C" if mnem == "cmc" else ""
    elif mnem in ("cld", "std"):
        ins.kind = "df"
    elif mnem in ("lahf", "sahf"):
        ins.kind = "conv"
        if mnem == "sahf":
            ins.flags_w = "NZCPA"
        else:
            ins.flags_r = "NZCPA"
    elif mnem == "x87":
        ins.kind = "x87"
    elif mnem in ("simd", "sse"):
        ins.kind = "simd"
    elif mnem in ("ud1", "ud2", "int3"):
        ins.kind = "trap"
    elif mnem == "leave":
        ins.kind = "leave"
    elif mnem == "not":
        ins.kind = "alu"
    else:
        ins.kind = "sys"


def decode_one(buf, pos, end, addr):
    """Decode one instruction at buf[pos:end], guest addr `addr`. Never reads past end."""
    ins = Insn()
    ins.addr = addr + pos
    start = pos
    opsize = addrsize = 0
    rep = ""
    seg = None
    lock = False
    while pos < end and buf[pos] in PREFIXES:
        b = buf[pos]
        if b == 0x66:
            opsize = 1
        elif b == 0x67:
            addrsize = 1
        elif b == 0xF0:
            lock = True
        elif b == 0xF2:
            rep = "f2"
        elif b == 0xF3:
            rep = "f3"
        else:
            seg = SEG_PREFIX[b]
        pos += 1
    if pos >= end:
        raise DecodeError("truncated prefixes")

    vex = None
    b0 = buf[pos]
    if b0 == 0x62 and pos + 3 < end:
        p0, p1, p2 = buf[pos + 1], buf[pos + 2], buf[pos + 3]
        if p1 & 0x04:
            vex = {"evex": True, "map": p0 & 0x1F,
                   "r": (~p0 >> 7) & 1, "x": (~p0 >> 6) & 1, "b": (~p0 >> 5) & 1,
                   "w": (p1 >> 7) & 1, "vvvv": (~p1 >> 3) & 0xF, "pp": p1 & 3}
            pos += 4
            b0 = buf[pos]
    elif b0 in (0xC4, 0xC5) and pos + 1 < end:
        if b0 == 0xC5:
            b1 = buf[pos + 1]
            vex = {"evex": False, "map": 1, "r": (~b1 >> 7) & 1, "x": 1, "b": 1,
                   "w": 0, "vvvv": (~b1 >> 3) & 0xF, "pp": b1 & 3}
            pos += 2
        else:
            b1, b2 = buf[pos + 1], buf[pos + 2]
            vex = {"evex": False, "map": b1 & 0x1F, "r": (~b1 >> 7) & 1,
                   "x": (~b1 >> 6) & 1, "b": (~b1 >> 5) & 1,
                   "w": (b2 >> 7) & 1, "vvvv": (~b2 >> 3) & 0xF, "pp": b2 & 3}
            pos += 3
        b0 = buf[pos]

    rex = 0
    has_rex = False
    if vex is None and 0x40 <= b0 <= 0x4F and pos + 1 <= end:
        rex = b0
        has_rex = True
        pos += 1
        if pos >= end:
            raise DecodeError("truncated after rex")
        b0 = buf[pos]
    rexw, rexr, rexx, rexb = (rex >> 3) & 1, (rex >> 2) & 1, (rex >> 1) & 1, rex & 1

    ins.rep, ins.lock, ins.seg, ins.opsize = rep, lock, seg, opsize
    ins.rexw, ins.has_rex = rexw, has_rex
    ins.addrsize = addrsize

    state = {"opsize": 0 if rexw else opsize, "rexw": rexw, "rexr": rexr, "rexx": rexx, "rexb": rexb,
             "has_rex": has_rex, "vex": vex}

    op = b0
    pos += 1
    opcode_byte = op

    if vex is not None:
        omap = vex["map"]
        if omap == 1:
            spec = ("simd", "V W Ib") if op in _VEX_IMM8_MAP1 else ("simd", "V W")
        elif omap == 2:
            spec = ("simd", "V W")
        elif omap == 3:
            spec = ("simd", "V W Ib")
        else:
            ins.unknown, ins.mnem = True, f"vexmap{omap}"
            ins.size = pos - start
            return ins
        if not vex["evex"] and omap in (2, 3):
            bmi = _decode_vex_bmi(buf, pos, end, ins, start, vex, op, addr)
            if bmi is not None:
                return bmi
        return _finish(buf, pos, end, ins, start, f"v{op:02x}", spec[1], state, addr, opcode_byte)

    if op == 0x0F:
        if pos >= end:
            raise DecodeError("truncated 0F map")
        op = buf[pos]
        pos += 1
        opcode_byte = op
        if op == 0x38:
            if pos >= end:
                raise DecodeError("truncated 0F38")
            opcode_byte = buf[pos]
            pos += 1
            if opcode_byte in (0xF0, 0xF1):
                if rep == "f2":
                    dst_s = "Gq" if rexw else "Gd"
                    if opcode_byte == 0xF0:
                        spec = ("crc32", f"{dst_s} Eb")
                    else:
                        src_s = "Eq" if rexw else ("Ew" if opsize else "Ed")
                        spec = ("crc32", f"{dst_s} {src_s}")
                elif rep == "" and pos < end and (buf[pos] >> 6) != 3:
                    sz_c = "q" if rexw else ("w" if opsize else "d")
                    spec = ("movbe", f"G{sz_c} M{sz_c}" if opcode_byte == 0xF0 else f"M{sz_c} G{sz_c}")
                else:
                    _, _, pos, _ = _modrm(buf, pos, end, rexr, rexx, rexb, has_rex)
                    ins.unknown, ins.mnem, ins.size = True, f"op0f38_{opcode_byte:02x}", pos - start
                    return ins
            else:
                spec = ("simd", "V W")
        elif op == 0x3A:
            if pos >= end:
                raise DecodeError("truncated 0F3A")
            opcode_byte = buf[pos]
            pos += 1
            spec = ("simd", "V W Ib")
        else:
            spec = OF.get(op)
            if spec is None:
                ins.unknown, ins.mnem = True, f"op0f_{op:02x}"
                ins.size = pos - start
                return ins
        mnem, opspec = spec
    else:
        spec = PRIMARY.get(op)
        if spec is None:
            ins.unknown, ins.mnem = True, f"op_{op:02x}"
            ins.size = pos - start
            return ins
        mnem, opspec = spec
        if op == 0x90:
            if rexb:
                # The opcode-embedded register is extended by REX.B; RAX is
                # the fixed accumulator operand and must not be extended.
                mnem, opspec = "xchg", "Oq Rq"
            else:
                mnem, opspec = ("pause" if rep == "f3" else "nop"), ""

    if mnem == "invalid" or opspec in ("invalid", "sys"):
        pass  # still decoded below so operands/length are right where known
    if mnem in ("grp1", "grp2", "grp3", "grp4", "grp5", "grp7", "grp8", "grppop", "grpmov", "grp15"):
        return _finish_group(buf, pos, end, ins, start, mnem, opspec, state, addr, opcode_byte)
    if mnem in ("invalid",):
        ins.mnem, ins.kind, ins.error = "invalid", "invalid", "invalid opcode"
        ins.size = pos - start
        return ins
    return _finish(buf, pos, end, ins, start, mnem, opspec, state, addr, opcode_byte)


def _finish_group(buf, pos, end, ins, start, group, opspec, state, addr, opcode_byte):
    reg_field, rm_make, pos, mod = _modrm(buf, pos, end, state["rexr"], state["rexx"],
                                          state["rexb"], state["has_rex"])
    reg7 = reg_field & 7
    if group == "grp1":
        mnem, spec = _GROUP1[reg7], opspec
    elif group == "grp2":
        mnem, spec = _GROUP2[reg7], opspec
    elif group == "grp3":
        mnem = _GROUP3[reg7]
        if reg7 < 2:
            spec = opspec + (" Ib" if opspec.endswith("b") else " Iz")
        else:
            spec = opspec
    elif group == "grp4":
        mnem, spec = _GROUP4[reg7], opspec
        if mnem is None:
            ins.unknown, ins.mnem, ins.size = True, f"grp4_{reg7}", pos - start
            return ins
    elif group == "grp5":
        mnem = _GROUP5[reg7]
        spec = "Eq" if mnem in ("push", "call", "jmp") else opspec
        if mnem in ("callf", "jmpf"):
            ins.mnem, ins.kind, ins.error, ins.size = mnem, "invalid", "far transfer", pos - start
            return ins
        if mnem is None:
            ins.unknown, ins.mnem, ins.size = True, f"grp5_{reg7}", pos - start
            return ins
    elif group == "grp8":
        mnem, spec = _GROUP8[reg7], opspec
        if mnem is None:
            ins.unknown, ins.mnem, ins.size = True, f"grp8_{reg7}", pos - start
            return ins
    elif group == "grp15":
        mnem, spec = ("fence" if mod == 3 else _GROUP15[reg7]), ""
        ins.mnem, ins.size = mnem, pos - start
        ins.kind = "nop" if mnem in ("fence", "xsaveopt") else "sys"
        return ins
    elif group == "grp7":
        if mod == 3:
            mnem = _GROUP7_REG.get(buf[pos - 1])
            if mnem is None:
                mnem = "smsw" if reg7 == 4 else "lmsw" if reg7 == 6 else "sys"
        else:
            mnem = _GROUP7[reg7] or "sys"
        ins.mnem, ins.size = mnem, pos - start
        ins.kind = "sys"
        return ins
    elif group == "grppop":
        if reg7 != 0:
            ins.unknown, ins.mnem, ins.size = True, f"grp1a_{reg7}", pos - start
            return ins
        mnem, spec = "pop", "Eq"
    elif group == "grpmov":
        if reg7 != 0:
            ins.unknown, ins.mnem, ins.size = True, f"grp11_{reg7}", pos - start
            return ins
        mnem, spec = "mov", opspec
    else:
        raise DecodeError(group)

    ops = []
    for tok in spec.split() if spec else ():
        t, s = tok[0], tok[1] if len(tok) > 1 else ""
        if t == "G":
            hi8 = s == "b" and not state["has_rex"] and 4 <= reg_field <= 7
            ops.append(("r", reg_field - 4 if hi8 else reg_field,
                        _size_of(s, state["opsize"], state["rexw"]), hi8))
        elif t in "EMUW":
            ops.append(rm_make(_size_of(s, state["opsize"], state["rexw"])))
        elif t == "I":
            n = _size_of(s, state["opsize"], state["rexw"])
            n = min(n, 4) if s in "zv" else n
            v, pos = _read_imm(buf, pos, end, n, s != "v")
            ops.append(("i", v, _size_of(s, state["opsize"], state["rexw"]) or n))
        elif t == "J":
            n = 1 if s == "b" else 4
            v, pos = _read_imm(buf, pos, end, n, True)
            ops.append(("a", addr + pos + v, 0))
        elif t == "A":
            ops.append(("r", 0, _size_of(s, state["opsize"], state["rexw"])))
        elif t == "c":
            ops.append(("r", 1, 1))
        elif t == "1":
            ops.append(("i", 1, 1))
        elif t == "S":
            ops.append(("seg", reg_field, 0))
    ins.mnem = mnem
    ins.ops = tuple(ops)
    ins.size = pos - start
    for o in ins.ops:
        if o[0] == "m":
            if ins.addrsize:
                o[1]["addrsize"] = 4
            if o[1]["rip"]:
                abs_a = ins.addr + ins.size + o[1]["disp"]
                o[1]["abs"] = abs_a & 0xFFFFFFFF if ins.addrsize else abs_a
    _classify(ins, mnem, opcode_byte & 0xF, state)
    return ins


_BMI_FLAGS = {
    "andn": "NZCV",
    "blsr": "NZCV",
    "blsmsk": "NZCV",
    "blsi": "NZCV",
    "bextr": "NZCV",
    "bzhi": "NZCV",
}
FLAG_WRITERS.update(_BMI_FLAGS)


def _decode_vex_bmi(buf, pos, end, ins, start, vex, op, addr):
    """Decode VEX.LZ BMI1 (andn/blsr/blsmsk/blsi/bextr) and BMI2 (bzhi/pext/pdep/mulx/shlx/sarx/shrx/rorx)."""
    # All 13 BMI1/BMI2 VEX encodings use 3-byte C4 (map 2 or 3) with VEX.L == 0.
    if (buf[pos - 2] >> 2) & 1:
        return None
    omap, pp, vvvv = vex["map"], vex["pp"], vex["vvvv"]
    sz = 8 if vex["w"] else 4

    if omap == 2:
        if op == 0xF2 and pp == 0:
            kind_op = "andn"
        elif op == 0xF3 and pp == 0:
            kind_op = "grp_f3"
        elif op == 0xF5 and pp in (0, 2, 3):
            kind_op = {0: "bzhi", 2: "pext", 3: "pdep"}[pp]
        elif op == 0xF6 and pp == 3:
            kind_op = "mulx"
        elif op == 0xF7 and pp in (0, 1, 2, 3):
            kind_op = {0: "bextr", 1: "shlx", 2: "sarx", 3: "shrx"}[pp]
        else:
            return None
    elif omap == 3 and op == 0xF0 and pp == 3 and vvvv == 0:
        kind_op = "rorx"
    else:
        return None

    reg_field, rm_make, pos, _ = _modrm(buf, pos, end, vex["r"], vex["x"], vex["b"], True)
    if kind_op == "grp_f3":
        reg7 = reg_field & 7
        mnem = {1: "blsr", 2: "blsmsk", 3: "blsi"}.get(reg7)
        if mnem is None:
            ins.unknown, ins.mnem, ins.size = True, f"vex_f3_{reg7}", pos - start
            return ins
        ops = (("r", vvvv, sz), rm_make(sz))
    elif kind_op == "rorx":
        imm, pos = _read_imm(buf, pos, end, 1, signed=False)
        mnem = "rorx"
        ops = (("r", reg_field, sz), rm_make(sz), ("i", imm, 1))
    elif kind_op in ("andn", "pdep", "pext", "mulx"):
        mnem = kind_op
        ops = (("r", reg_field, sz), ("r", vvvv, sz), rm_make(sz))
    else:
        mnem = kind_op
        ops = (("r", reg_field, sz), rm_make(sz), ("r", vvvv, sz))

    ins.mnem = mnem
    ins.ops = ops
    ins.size = pos - start
    ins.rexw = vex["w"]
    for o in ins.ops:
        if o[0] == "m" and o[1]["rip"]:
            o[1]["abs"] = ins.addr + ins.size + o[1]["disp"]
    ins.kind = "bmi"
    ins.flags_w = _BMI_FLAGS.get(mnem, "")
    ins.flags_r = ""
    return ins


def disasm(buf, base=0):
    """Yield Insn objects linearly. An undecodable byte yields an Insn with unknown/error set and
    decoding continues from the next byte."""
    pos = 0
    end = len(buf)
    while pos < end:
        try:
            ins = decode_one(buf, pos, end, base)
        except DecodeError as e:
            ins = Insn()
            ins.addr = base + pos
            ins.size = max(1, end - pos)
            ins.mnem = "db"
            ins.error = str(e)
            ins.unknown = True
            yield ins
            return
        yield ins
        pos += max(ins.size, 1)


# ---------------------------------------------------------------------------
# arm64 emitter
# ---------------------------------------------------------------------------

COND = {"eq": 0, "ne": 1, "cs": 2, "cc": 3, "mi": 4, "pl": 5, "vs": 6, "vc": 7,
        "hi": 8, "ls": 9, "ge": 10, "lt": 11, "gt": 12, "le": 13, "al": 14}
INV_COND = {v: k for k, v in COND.items()}
INV = {"eq": "ne", "ne": "eq", "cs": "cc", "cc": "cs", "mi": "pl", "pl": "mi",
       "vs": "vc", "vc": "vs", "hi": "ls", "ls": "hi", "ge": "lt", "lt": "ge",
       "gt": "le", "le": "gt", "al": "al"}


def _bitmask_imm(value, width):
    v = value & ((1 << width) - 1)
    if v == 0 or v == (1 << width) - 1:
        return None
    for e in (2, 4, 8, 16, 32, width):
        if width % e:
            continue
        pat = v & ((1 << e) - 1)
        rep = 0
        for i in range(width // e):
            rep |= pat << (i * e)
        if rep != v:
            continue
        ones = bin(pat).count("1")
        if ones == 0 or ones == e:
            continue
        rot = -1
        p = pat
        for r in range(e):
            if p == (1 << ones) - 1:
                rot = r
                break
            p = ((p >> 1) | (p << (e - 1))) & ((1 << e) - 1)
        if rot < 0:
            continue
        n = 1 if e == 64 else 0
        imms = (((-e) << 1) | (ones - 1)) & 0x3F
        immr = (-rot) & (e - 1)
        return n, immr, imms
    return None


class Asm:
    """arm64 machine-code emitter with labels."""

    def __init__(self):
        self.words = []
        self.labels = {}
        self.fixups = []  # (word_index, kind, label)

    def tell(self):
        return len(self.words) * 4

    def emit(self, w):
        self.words.append(w & 0xFFFFFFFF)

    def label(self, name):
        self.labels[name] = self.tell()

    def b(self, label):
        self.fixups.append((len(self.words), "b26", label))
        self.emit(0x14000000)

    def bl(self, label):
        self.fixups.append((len(self.words), "b26", label))
        self.emit(0x94000000)

    def bcond(self, cond, label):
        self.fixups.append((len(self.words), "cond19", label))
        self.emit(0x54000000 | COND[cond])

    def cbz(self, rt, label, sf=1):
        self.fixups.append((len(self.words), "cb19", label))
        self.emit(0x34000000 | (sf << 31) | rt)

    def cbnz(self, rt, label, sf=1):
        self.fixups.append((len(self.words), "cb19", label))
        self.emit(0x35000000 | (sf << 31) | rt)

    def tbz(self, rt, bit, label):
        self.fixups.append((len(self.words), "tb14", label))
        self.emit(0x36000000 | ((bit & 0x20) << 26) | ((bit & 0x1F) << 19) | rt)

    def tbnz(self, rt, bit, label):
        self.fixups.append((len(self.words), "tb14", label))
        self.emit(0x37000000 | ((bit & 0x20) << 26) | ((bit & 0x1F) << 19) | rt)

    def br(self, rn):
        self.emit(0xD61F0000 | rn << 5)

    def blr(self, rn):
        self.emit(0xD63F0000 | rn << 5)

    def ret(self, rn=30):
        self.emit(0xD65F0000 | rn << 5)

    def finalize(self, base=0):
        for idx, kind, lbl in self.fixups:
            tgt = self.labels[lbl]
            here = idx * 4
            if kind == "b26":
                imm = (tgt - here) >> 2
                assert -(1 << 25) <= imm < (1 << 25), f"b out of range to {lbl}"
                self.words[idx] |= imm & 0x03FFFFFF
            elif kind in ("cond19", "cb19"):
                imm = (tgt - here) >> 2
                assert -(1 << 18) <= imm < (1 << 18)
                self.words[idx] |= (imm & 0x7FFFF) << 5
            elif kind == "tb14":
                imm = (tgt - here) >> 2
                assert -(1 << 13) <= imm < (1 << 13)
                self.words[idx] |= (imm & 0x3FFF) << 5
        return b"".join(struct.pack("<I", w) for w in self.words)

    # moves
    def movz(self, rd, imm, hw=0, sf=1):
        self.emit((0xD2800000 if sf else 0x52800000) | hw << 21 | (imm & 0xFFFF) << 5 | rd)

    def movn(self, rd, imm, hw=0, sf=1):
        self.emit((0x92800000 if sf else 0x12800000) | hw << 21 | (imm & 0xFFFF) << 5 | rd)

    def movk(self, rd, imm, hw=0, sf=1):
        self.emit((0xF2800000 if sf else 0x72800000) | hw << 21 | (imm & 0xFFFF) << 5 | rd)

    def li(self, rd, val, pad4=False):
        """Materialize a 64-bit constant. pad4 pads to exactly 4 words with nops (for patching)."""
        v = val & 0xFFFFFFFFFFFFFFFF
        chunks = [(v >> (16 * i)) & 0xFFFF for i in range(4)]
        nz = [i for i in range(4) if chunks[i] != 0]
        nzc = [i for i in range(4) if chunks[i] != 0xFFFF]
        start = len(self.words)
        if not nz:
            self.movz(rd, 0)
        elif not nzc:
            self.movn(rd, 0)
        elif len(nzc) < len(nz):
            self.movn(rd, chunks[nzc[0]] ^ 0xFFFF, nzc[0])
            for i in nzc[1:]:
                self.movk(rd, chunks[i], i)
        else:
            self.movz(rd, chunks[nz[0]], nz[0])
            for i in nz[1:]:
                self.movk(rd, chunks[i], i)
        while pad4 and len(self.words) - start < 4:
            self.nop()

    def mov(self, rd, rn, sf=1):
        self.emit((0xAA0003E0 if sf else 0x2A0003E0) | rn << 16 | rd)

    def mvn(self, rd, rn, sf=1):
        self.emit((0xAA2003E0 if sf else 0x2A2003E0) | rn << 16 | rd)

    # data processing, immediate forms
    def _dp_imm(self, base, rd, rn, imm, sf):
        if imm < 0:
            imm = -imm
            base ^= 0x40000000
        if imm % 4096 == 0 and 0 < imm // 4096 < 4096:
            self.emit((base if sf else base - 0x80000000) | 1 << 22 | (imm // 4096) << 10 | rn << 5 | rd)
            return
        assert 0 <= imm < 4096, f"dp imm {imm}"
        self.emit((base if sf else base - 0x80000000) | imm << 10 | rn << 5 | rd)

    def add(self, rd, rn, imm, sf=1):
        self._dp_imm(0x91000000, rd, rn, imm, sf)

    def adds(self, rd, rn, imm, sf=1):
        self._dp_imm(0xB1000000, rd, rn, imm, sf)

    def sub(self, rd, rn, imm, sf=1):
        self._dp_imm(0xD1000000, rd, rn, imm, sf)

    def subs(self, rd, rn, imm, sf=1):
        self._dp_imm(0xF1000000, rd, rn, imm, sf)

    def cmp(self, rn, imm, sf=1):
        self.subs(31, rn, imm, sf)

    def _dp_reg(self, base, rd, rn, rm, shift, imm6, sf):
        self.emit((base if sf else base - 0x80000000) | shift << 22 | rm << 16 | imm6 << 10 | rn << 5 | rd)

    def add_r(self, rd, rn, rm, sf=1, shift=0, imm6=0):
        self._dp_reg(0x8B000000, rd, rn, rm, shift, imm6, sf)

    def adds_r(self, rd, rn, rm, sf=1, shift=0, imm6=0):
        self._dp_reg(0xAB000000, rd, rn, rm, shift, imm6, sf)

    def sub_r(self, rd, rn, rm, sf=1, shift=0, imm6=0):
        self._dp_reg(0xCB000000, rd, rn, rm, shift, imm6, sf)

    def subs_r(self, rd, rn, rm, sf=1, shift=0, imm6=0):
        self._dp_reg(0xEB000000, rd, rn, rm, shift, imm6, sf)

    def cmp_r(self, rn, rm, sf=1, shift=0, imm6=0):
        self._dp_reg(0xEB000000, 31, rn, rm, shift, imm6, sf)

    def adcs(self, rd, rn, rm, sf=1):
        self.emit((0xBA000000 if sf else 0x3A000000) | rm << 16 | rn << 5 | rd)

    def sbcs(self, rd, rn, rm, sf=1):
        self.emit((0xFA000000 if sf else 0x7A000000) | rm << 16 | rn << 5 | rd)

    def _log_reg(self, base, rd, rn, rm, shift, imm6, sf):
        self.emit((base if sf else base - 0x80000000) | shift << 22 | rm << 16 | imm6 << 10 | rn << 5 | rd)

    def and_r(self, rd, rn, rm, sf=1, shift=0, imm6=0):
        self._log_reg(0x8A000000, rd, rn, rm, shift, imm6, sf)

    def bic_r(self, rd, rn, rm, sf=1, shift=0, imm6=0):
        self._log_reg(0x8A200000, rd, rn, rm, shift, imm6, sf)

    def orr_r(self, rd, rn, rm, sf=1, shift=0, imm6=0):
        self._log_reg(0xAA000000, rd, rn, rm, shift, imm6, sf)

    def eor_r(self, rd, rn, rm, sf=1, shift=0, imm6=0):
        self._log_reg(0xCA000000, rd, rn, rm, shift, imm6, sf)

    def eor_shift(self, rd, rn, rm, shift, imm6, sf=1):
        self._log_reg(0xCA000000, rd, rn, rm, shift, imm6, sf)

    def ands_r(self, rd, rn, rm, sf=1, shift=0, imm6=0):
        self._log_reg(0xEA000000, rd, rn, rm, shift, imm6, sf)

    def tst_r(self, rn, rm, sf=1, shift=0, imm6=0):
        self._log_reg(0xEA000000, 31, rn, rm, shift, imm6, sf)

    def _log_imm(self, base, rd, rn, val, sf):
        enc = _bitmask_imm(val, 64 if sf else 32)
        assert enc is not None, f"{val:#x} not a bitmask imm"
        n, immr, imms = enc
        self.emit((base if sf else base - 0x80000000) | n << 22 | immr << 16 | imms << 10 | rn << 5 | rd)

    def and_i(self, rd, rn, val, sf=1):
        self._log_imm(0x92000000, rd, rn, val, sf)

    def orr_i(self, rd, rn, val, sf=1):
        self._log_imm(0xB2000000, rd, rn, val, sf)

    def eor_i(self, rd, rn, val, sf=1):
        self._log_imm(0xD2000000, rd, rn, val, sf)

    def tst_i(self, rn, val, sf=1):
        self._log_imm(0xF2000000, 31, rn, val, sf)

    # bitfield
    def _bfm(self, base, rd, rn, immr, imms, sf):
        # bit22 (N) must equal sf for BFM-family ops; base carries N=1 (64-bit form)
        self.emit((base if sf else base - 0x80400000) | immr << 16 | imms << 10 | rn << 5 | rd)

    def ubfm(self, rd, rn, immr, imms, sf=1):
        self._bfm(0xD3400000, rd, rn, immr, imms, sf)

    def sbfm(self, rd, rn, immr, imms, sf=1):
        self._bfm(0x93400000, rd, rn, immr, imms, sf)

    def bfm(self, rd, rn, immr, imms, sf=1):
        self._bfm(0xB3400000, rd, rn, immr, imms, sf)

    def bfi(self, rd, rn, lsb, width):
        self.bfm(rd, rn, (-lsb) & 63, width - 1)

    def ubfx(self, rd, rn, lsb, width, sf=1):
        self.ubfm(rd, rn, lsb, lsb + width - 1, sf)

    def sbfx(self, rd, rn, lsb, width, sf=1):
        self.sbfm(rd, rn, lsb, lsb + width - 1, sf)

    def lsl(self, rd, rn, sh, sf=1):
        w = 64 if sf else 32
        self.ubfm(rd, rn, (-sh) % w, w - 1 - sh, sf)

    def lsr(self, rd, rn, sh, sf=1):
        self.ubfm(rd, rn, sh, w := (64 if sf else 32) - 1, sf)

    def asr(self, rd, rn, sh, sf=1):
        self.sbfm(rd, rn, sh, (64 if sf else 32) - 1, sf)

    def uxtb(self, rd, rn):
        self.ubfm(rd, rn, 0, 7, 0)

    def uxth(self, rd, rn):
        self.ubfm(rd, rn, 0, 15, 0)

    def sxtb(self, rd, rn, sf=1):
        self.sbfm(rd, rn, 0, 7, sf)

    def sxth(self, rd, rn, sf=1):
        self.sbfm(rd, rn, 0, 15, sf)

    def sxtw(self, rd, rn):
        self.sbfm(rd, rn, 0, 31, 1)

    def extr(self, rd, rn, rm, lsb, sf=1):
        self.emit((0x93C00000 if sf else 0x13800000) | rm << 16 | lsb << 10 | rn << 5 | rd)

    def ror(self, rd, rn, sh, sf=1):
        self.extr(rd, rn, rn, sh, sf)

    def lslv(self, rd, rn, rm, sf=1):
        self.emit((0x9AC02000 if sf else 0x1AC02000) | rm << 16 | rn << 5 | rd)

    def lsrv(self, rd, rn, rm, sf=1):
        self.emit((0x9AC02400 if sf else 0x1AC02400) | rm << 16 | rn << 5 | rd)

    def asrv(self, rd, rn, rm, sf=1):
        self.emit((0x9AC02800 if sf else 0x1AC02800) | rm << 16 | rn << 5 | rd)

    def rorv(self, rd, rn, rm, sf=1):
        self.emit((0x9AC02C00 if sf else 0x1AC02C00) | rm << 16 | rn << 5 | rd)

    def rev16(self, rd, rn, sf=0):
        self.emit((0xDAC00400 if sf else 0x5AC00400) | rn << 5 | rd)

    # multiply/divide
    def madd(self, rd, rn, rm, ra, sf=1):
        self.emit((0x9B000000 if sf else 0x1B000000) | rm << 16 | ra << 10 | rn << 5 | rd)

    def msub(self, rd, rn, rm, ra, sf=1):
        self.emit((0x9B008000 if sf else 0x1B008000) | rm << 16 | ra << 10 | rn << 5 | rd)

    def mul(self, rd, rn, rm, sf=1):
        self.madd(rd, rn, rm, 31, sf)

    def umaddl(self, rd, rn, rm, ra):
        self.emit(0x9BA00000 | rm << 16 | ra << 10 | rn << 5 | rd)

    def smaddl(self, rd, rn, rm, ra):
        self.emit(0x9B200000 | rm << 16 | ra << 10 | rn << 5 | rd)

    def smulh(self, rd, rn, rm):
        self.emit(0x9B407C00 | rm << 16 | rn << 5 | rd)

    def umulh(self, rd, rn, rm):
        self.emit(0x9BC07C00 | rm << 16 | rn << 5 | rd)

    def sdiv(self, rd, rn, rm, sf=1):
        self.emit((0x9AC00C00 if sf else 0x1AC00C00) | rm << 16 | rn << 5 | rd)

    def udiv(self, rd, rn, rm, sf=1):
        self.emit((0x9AC00800 if sf else 0x1AC00800) | rm << 16 | rn << 5 | rd)

    # loads/stores
    def ldr(self, rt, rn, off=0, size=8, signed=False, target_sf=1):
        if signed:
            base = {1: 0x39800000 if target_sf else 0x39C00000,
                    2: 0x79800000 if target_sf else 0x79C00000,
                    4: 0xB9800000}[size]
        else:
            base = {1: 0x39400000, 2: 0x79400000, 4: 0xB9400000, 8: 0xF9400000}[size]
        assert 0 <= off < 4096 * size and off % size == 0
        self.emit(base | (off // size) << 10 | rn << 5 | rt)

    def str(self, rt, rn, off=0, size=8):
        base = {1: 0x39000000, 2: 0x79000000, 4: 0xB9000000, 8: 0xF9000000}[size]
        assert 0 <= off < 4096 * size and off % size == 0
        self.emit(base | (off // size) << 10 | rn << 5 | rt)

    def ldaxr(self, rt, rn, size=8):
        base = {1: 0x085FFC00, 2: 0x485FFC00, 4: 0x885FFC00, 8: 0xC85FFC00}[size]
        self.emit(base | rn << 5 | rt)

    def stlxr(self, rs, rt, rn, size=8):
        assert rs != rt and rs != rn
        base = {1: 0x0800FC00, 2: 0x4800FC00, 4: 0x8800FC00, 8: 0xC800FC00}[size]
        self.emit(base | rs << 16 | rn << 5 | rt)

    def ldur(self, rt, rn, off, size=8):
        base = {1: 0x38400000, 2: 0x78400000, 4: 0xB8400000, 8: 0xF8400000}[size]
        assert -256 <= off <= 255
        self.emit(base | (off & 0x1FF) << 12 | rn << 5 | rt)

    def stur(self, rt, rn, off, size=8):
        base = {1: 0x38000000, 2: 0x78000000, 4: 0xB8000000, 8: 0xF8000000}[size]
        assert -256 <= off <= 255
        self.emit(base | (off & 0x1FF) << 12 | rn << 5 | rt)

    def ldr_post(self, rt, rn, off, size=8):
        base = {1: 0x38400400, 2: 0x78400400, 4: 0xB8400400, 8: 0xF8400400}[size]
        assert -256 <= off <= 255
        self.emit(base | (off & 0x1FF) << 12 | rn << 5 | rt)

    def str_pre(self, rt, rn, off, size=8):
        base = {1: 0x38000C00, 2: 0x78000C00, 4: 0xB8000C00, 8: 0xF8000C00}[size]
        assert -256 <= off <= 255
        self.emit(base | (off & 0x1FF) << 12 | rn << 5 | rt)

    # conditional select
    def _csel(self, base, rd, rn, rm, cond, sf):
        self.emit((base if sf else base - 0x80000000) | rm << 16 | COND[cond] << 12 | rn << 5 | rd)

    def csel(self, rd, rn, rm, cond, sf=1):
        self._csel(0x9A800000, rd, rn, rm, cond, sf)

    def csinc(self, rd, rn, rm, cond, sf=1):
        self._csel(0x9A800400, rd, rn, rm, cond, sf)

    def csinv(self, rd, rn, rm, cond, sf=1):
        self._csel(0x9A808000, rd, rn, rm, cond, sf)

    def cset(self, rd, cond, sf=1):
        self.csinc(rd, 31, 31, INV[cond], sf)

    def csetm(self, rd, cond, sf=1):
        self.csinv(rd, 31, 31, INV[cond], sf)

    # system
    def nop(self):
        self.emit(0xD503201F)

    def brk(self, imm=0):
        self.emit(0xD4200000 | (imm & 0xFFFF) << 5)

    def svc(self, imm=0):
        self.emit(0xD4000001 | (imm & 0xFFFF) << 5)

    def mrs_nzcv(self, rt):
        self.emit(0xD53B4200 | rt)

    def msr_nzcv(self, rt):
        self.emit(0xD51B4200 | rt)

    def rev(self, rd, rn, sf=1):
        self.emit((0xDAC00C00 if sf else 0x5AC00800) | rn << 5 | rd)


# ---------------------------------------------------------------------------
# guest->host register/flag contract
# ---------------------------------------------------------------------------

# guest reg -> host reg. Scratch: x0-x4, x16(ADDR_T), x17(GTMP), x30(LINK). x28=ctx. x18 free.
GMAP = {0: 19, 1: 20, 2: 21, 3: 22, 4: 23, 5: 24, 6: 25, 7: 26,
        8: 27, 9: 9, 10: 10, 11: 11, 12: 12, 13: 13, 14: 14, 15: 15}
CTX_REG = 28
ADDR_T = 16
GTMP = 17
T0, T1, T2, T3 = 0, 1, 2, 3
LINK = 30

CTX_EFLAGS = 0x00
CTX_RES = 0x08
CTX_A = 0x10
CTX_B = 0x18

EF_CF, EF_PF, EF_AF, EF_ZF, EF_SF, EF_OF = 0x1, 0x4, 0x10, 0x40, 0x80, 0x800


class UnhandledInsn(Exception):
    def __init__(self, ins, why):
        super().__init__(f"{ins.mnem} at {ins.addr:#x}: {why}")
        self.why = why


class FunctionTranslator:
    """Translate decoded Insn objects of one function to arm64 via self.asm.

    `ns` prefixes the function-local "S*" labels so several translators can be
    merged into one Asm image without collisions ("G*" labels are guest VAs and
    already unique; "X*" exit stubs dedupe by VA)."""

    def __init__(self, insns, ns=""):
        self.insns = [i for i in insns if not i.unknown and not i.error]
        self.by_addr = {i.addr: i for i in self.insns}
        self.asm = Asm()
        self.stats = {"insns": 0, "unhandled": []}
        self._lbl_ctr = 0
        self.ns = ns
        self.exit_stubs = {}   # guest_va -> label
        self._collect_leaders()
        self._flag_analysis()

    # -- CFG + flag liveness -------------------------------------------------
    def _collect_leaders(self):
        self.leaders = {self.insns[0].addr}
        for i, ins in enumerate(self.insns):
            if ins.kind in ("branch", "call") and ins.ops and ins.ops[0][0] == "a":
                t = ins.ops[0][1]
                if t in self.by_addr:
                    self.leaders.add(t)
            if ins.kind in ("branch", "ret", "call", "trap") and i + 1 < len(self.insns):
                self.leaders.add(self.insns[i + 1].addr)
        self.blocks = []
        cur = []
        for ins in self.insns:
            if ins.addr in self.leaders and cur:
                self.blocks.append(cur)
                cur = []
            cur.append(ins)
        if cur:
            self.blocks.append(cur)

    def _flag_analysis(self):
        """Per-block flag liveness; produces:
          needed[addr]    - flags a writer must produce (consumed downstream before rewrite)
          mat             - writer addrs that must store the canonical ctx flag word
          arm_path[addr]  - consumer addr -> producer Insn for the arm-NZCV path, else None
        """
        idx = self.by_addr
        succ = {}
        for bi, b in enumerate(self.blocks):
            last = b[-1]
            s = []
            fall = self.blocks[bi + 1][0].addr if bi + 1 < len(self.blocks) else None
            if last.kind == "branch":
                if last.cc != "al" and fall:
                    s.append(fall)
                if last.ops and last.ops[0][0] == "a" and last.ops[0][1] in idx:
                    s.append(last.ops[0][1])
            elif last.kind == "call":
                if fall:
                    s.append(fall)
                if last.ops and last.ops[0][0] == "a" and last.ops[0][1] in idx:
                    s.append(last.ops[0][1])
            elif last.kind in ("ret", "trap"):
                pass
            elif fall:
                s.append(fall)
            succ[b[0].addr] = s

        live_in = {b[0].addr: set() for b in self.blocks}
        live_out = {b[0].addr: set() for b in self.blocks}
        for _ in range(64):
            changed = False
            for b in reversed(self.blocks):
                key = b[0].addr
                out = set()
                for s in succ[key]:
                    out |= live_in[s]
                live = set(out)
                for ins in reversed(b):
                    if not _cond_flag_writer(ins):
                        live -= set(ins.flags_w)
                    live |= set(ins.flags_r)
                if out != live_out[key]:
                    live_out[key] = out
                    changed = True
                if live != live_in[key]:
                    live_in[key] = live
                    changed = True
            if not changed:
                break
        self.live_in = live_in

        self.needed = {}
        for b in self.blocks:
            live = set(live_out[b[0].addr])
            for ins in reversed(b):
                self.needed[ins.addr] = set(live) & set(ins.flags_w)
                if not _cond_flag_writer(ins):
                    live -= set(ins.flags_w)
                live |= set(ins.flags_r)

        # materialize: writer whose written flags are live at block end stores the ctx word.
        # A shift by cl may leave the flags alone, so the writer before it stores them too.
        self.mat = set()
        for b in self.blocks:
            pending = set(live_out[b[0].addr])
            if not pending:
                continue
            for ins in reversed(b):
                hit = pending & set(ins.flags_w)
                if hit:
                    self.mat.add(ins.addr)
                    if not _cond_flag_writer(ins):
                        pending -= hit
                if not pending:
                    break

        # per-consumer producer: the arm-NZCV path is allowed only when ONE writer is
        # the nearest producer of every flag the consumer reads. Otherwise the flag
        # word must come from ctx, so each contributing writer is materialized. A shift
        # by cl on the way writes the flags only when its count is not 0, so the consumer
        # reads ctx, the shift stores its word, and the search goes on past it: the writer
        # before it stores its word too, or the flags come into the block through ctx.
        self.arm_path = {}
        for b in self.blocks:
            writers = []
            for ins in b:
                if ins.flags_r:
                    need = set(ins.flags_r)
                    nearest = {}
                    passed = set()
                    for w in reversed(writers):
                        for f in need - set(nearest):
                            if f in w.flags_w:
                                if _cond_flag_writer(w):
                                    passed.add(w)
                                else:
                                    nearest[f] = w
                    prods = set(nearest.values())
                    if not passed and len(prods) == 1 and set(nearest) == need:
                        self.arm_path[ins.addr] = next(iter(prods))
                    else:
                        self.arm_path[ins.addr] = None
                        for w in prods | passed:
                            self.mat.add(w.addr)
                if ins.flags_w:
                    writers.append(ins)

    # -- emit helpers ----------------------------------------------------------
    def _lbl(self, guest_addr):
        return f"G{guest_addr:x}"

    def _target_lbl(self, guest_addr):
        if guest_addr in self.by_addr:
            return self._lbl(guest_addr)
        if guest_addr not in self.exit_stubs:
            self._lbl_ctr += 1
            self.exit_stubs[guest_addr] = f"X{guest_addr:x}"
        return self.exit_stubs[guest_addr]

    def _skip_lbl(self):
        self._lbl_ctr += 1
        return f"{self.ns}S{self._lbl_ctr}"

    def _op_size(self, op):
        return op[1]["size"] if op[0] == "m" else op[2]

    def _emit_xlat(self, ins):
        """Emit xlat/xlatb (0xD7): AL := byte ptr [RBX/EBX + zero_extend(AL)], no flags."""
        a = self.asm
        a.uxtb(T0, GMAP[0])
        a.add_r(ADDR_T, GMAP[3], T0, sf=0 if ins.addrsize else 1)
        a.ldr(T0, ADDR_T, 0, size=1)
        a.bfi(GMAP[0], T0, 0, 8)

    def _emit_enter(self, ins):
        """Emit enter imm16, imm8 (0xC8): create nested stack frame, no flags."""
        a = self.asm
        alloc = ins.ops[0][1] & 0xFFFF
        nesting = ins.ops[1][1] & 0x1F
        rsp, rbp = GMAP[4], GMAP[5]
        a.str_pre(rbp, rsp, -8)
        a.mov(T0, rsp)
        if nesting > 0:
            if nesting > 1:
                a.movz(T1, nesting - 1, 0, sf=0)
                loop_lbl = self._skip_lbl()
                a.label(loop_lbl)
                a.sub(rbp, rbp, 8)
                a.ldr(T2, rbp, 0, size=8)
                a.str_pre(T2, rsp, -8)
                a.sub(T1, T1, 1, sf=0)
                a.cbnz(T1, loop_lbl, sf=0)
            a.str_pre(T0, rsp, -8)
        a.mov(rbp, T0)
        if alloc:
            if alloc < 4096:
                a.sub(rsp, rsp, alloc)
            else:
                a.li(T1, alloc)
                a.sub_r(rsp, rsp, T1)

    def _load(self, ins, op, dst, as_size=None):
        a = self.asm
        size = as_size or self._op_size(op)
        if op[0] == "r":
            _, reg, osz = op[0], op[1], op[2]
            hi8 = len(op) > 3 and op[3]
            g = GMAP[reg]
            if osz == 8 and as_size in (None, 8):
                if g != dst:
                    a.mov(dst, g)
            elif osz == 4:
                a.mov(dst, g, sf=0)
            elif osz == 2:
                a.uxth(dst, g)
            elif osz == 1:
                if hi8:
                    a.ubfx(dst, g, 8, 8, sf=0)
                else:
                    a.uxtb(dst, g)
            else:
                a.mov(dst, g, sf=0)
        elif op[0] == "i":
            mask = (1 << (size * 8)) - 1 if size < 8 else 0xFFFFFFFFFFFFFFFF
            a.li(dst, op[1] & mask)
        elif op[0] == "m":
            self._mem_addr(op[1], ADDR_T)
            a.ldr(dst, ADDR_T, 0, size=size)
        elif op[0] == "a":
            a.li(dst, op[1])
        else:
            raise UnhandledInsn(ins, f"load {op}")

    def _store(self, ins, op, src):
        a = self.asm
        if op[0] == "r":
            _, reg, osz = op[0], op[1], op[2]
            hi8 = len(op) > 3 and op[3]
            g = GMAP[reg]
            if osz == 8:
                if g != src:
                    a.mov(g, src)
            elif osz == 4:
                a.mov(g, src, sf=0)
            elif osz == 2:
                a.bfi(g, src, 0, 16)
            else:
                a.bfi(g, src, 8, 8) if hi8 else a.bfi(g, src, 0, 8)
        elif op[0] == "m":
            self._mem_addr(op[1], ADDR_T)
            a.str(src, ADDR_T, 0, size=self._op_size(op))
        else:
            raise UnhandledInsn(ins, f"store {op}")

    _LOCK_ALU_OPS = frozenset({
        "add", "adc", "and", "sub", "sbb", "or", "xor", "inc", "dec", "neg", "not",
    })

    def _emit_lock_alu(self, ins):
        """Emit LOCK-prefixed memory RMW ALU ops using ARMv8.0 LDAXR/STLXR loops.

        The LL/SC loop computes only the new value using non-flag-setting instructions
        so incoming CF (for adc/sbb, captured into GTMP before the loop) and prior
        NZCV (for not, inc/dec CF preservation, and retries) stay intact across retries.
        Flags are computed once after the atomic store commits.
        """
        a = self.asm
        m = ins.mnem
        dst = ins.ops[0]
        sz = self._op_size(dst)
        sf = 1 if sz == 8 else 0
        mask = (1 << (sz * 8)) - 1 if sz < 8 else 0xFFFFFFFFFFFFFFFF
        need = self.needed.get(ins.addr, set())
        mat = ins.addr in self.mat
        flaggy = m != "not" and (bool(need) or mat)

        # Load source operand into T1 BEFORE computing EA (T1 stays read-only across retries).
        if len(ins.ops) == 2:
            src = ins.ops[1]
            if src[0] == "i":
                v = src[1] & mask
                if v == 0xFFFFFFFFFFFFFFFF:
                    a.movn(T1, 0)
                else:
                    a.li(T1, v)
            else:
                self._load(ins, src, T1)
        elif m in ("inc", "dec"):
            a.movz(T1, 1, 0, sf=0)

        # Compute EA into ADDR_T BEFORE capturing incoming CF into GTMP (_mem_addr may clobber GTMP).
        self._mem_addr(dst[1], ADDR_T)

        # For adc/sbb, capture original incoming x86 CF (0 or 1) into GTMP before retries.
        if m in ("adc", "sbb"):
            prod = self.arm_path.get(ins.addr)
            if prod is not None:
                if prod.mnem in LOGIC_KIND:
                    a.movz(GTMP, 0, 0, sf=0)
                else:
                    a.mrs_nzcv(GTMP)
                    a.ubfx(GTMP, GTMP, 29, 1)
                    if prod.mnem in SUB_KIND:
                        a.eor_i(GTMP, GTMP, 1, sf=0)
            else:
                a.ldr(GTMP, CTX_REG, CTX_EFLAGS, size=4)
                a.and_i(GTMP, GTMP, EF_CF, sf=0)

        def _compute_value():
            if m == "add":
                a.add_r(T2, T0, T1, sf=sf)
            elif m == "sub":
                a.sub_r(T2, T0, T1, sf=sf)
            elif m == "and":
                a.and_r(T2, T0, T1, sf=sf)
            elif m == "or":
                a.orr_r(T2, T0, T1, sf=sf)
            elif m == "xor":
                a.eor_r(T2, T0, T1, sf=sf)
            elif m == "inc":
                a.add(T2, T0, 1, sf=sf)
            elif m == "dec":
                a.sub(T2, T0, 1, sf=sf)
            elif m == "neg":
                a.sub_r(T2, 31, T0, sf=sf)
            elif m == "not":
                a.mvn(T2, T0, sf=sf)
            elif m == "adc":
                a.add_r(T2, T0, T1, sf=sf)
                a.add_r(T2, T2, GTMP, sf=sf)
            else:  # sbb
                a.sub_r(T2, T0, T1, sf=sf)
                a.sub_r(T2, T2, GTMP, sf=sf)

        retry_lbl = self._skip_lbl()
        if sz > 1:
            unaligned_lbl = self._skip_lbl()
            done_lbl = self._skip_lbl()
            # Guard ARMv8.0 LDAXR/STLXR alignment without touching NZCV.
            a.and_i(T0, ADDR_T, sz - 1)
            a.cbnz(T0, unaligned_lbl)

            a.label(retry_lbl)
            a.ldaxr(T0, ADDR_T, size=sz)
            _compute_value()
            a.stlxr(T3, T2, ADDR_T, size=sz)
            a.cbnz(T3, retry_lbl, sf=0)
            a.b(done_lbl)

            a.label(unaligned_lbl)
            a.ldr(T0, ADDR_T, 0, size=sz)
            _compute_value()
            a.str(T2, ADDR_T, 0, size=sz)
            a.label(done_lbl)
        else:
            a.label(retry_lbl)
            a.ldaxr(T0, ADDR_T, size=1)
            _compute_value()
            a.stlxr(T3, T2, ADDR_T, size=1)
            a.cbnz(T3, retry_lbl, sf=0)

        if not flaggy:
            return

        if sz == 1:
            a.uxtb(T2, T2)
        elif sz == 2:
            a.uxth(T2, T2)

        if "A" in need or mat:
            if m == "neg":
                a.stur(31, CTX_REG, CTX_A, size=8)
                a.stur(T0, CTX_REG, CTX_B, size=8)
            else:
                a.stur(T0, CTX_REG, CTX_A, size=8)
                a.stur(T1, CTX_REG, CTX_B, size=8)
        self._stash_res(ins, T2)

        sh = (32 - sz * 8) if sz < 4 else 0
        if m in ("and", "or", "xor"):
            if sh:
                a.lsl(T2, T2, sh, sf=0)
            a.ands_r(31, T2, T2, sf=sf)
            self._maybe_mkflags(ins)
            return

        if m in ("add", "sub"):
            if sh:
                a.lsl(T0, T0, sh, sf=0)
                a.lsl(T1, T1, sh, sf=0)
            (a.adds_r if m == "add" else a.subs_r)(31, T0, T1, sf=sf)
            self._maybe_mkflags(ins)
            return

        if m == "neg":
            if sh:
                a.lsl(T0, T0, sh, sf=0)
            a.subs_r(31, 31, T0, sf=sf)
            self._maybe_mkflags(ins)
            return

        if m in ("inc", "dec"):
            a.mrs_nzcv(GTMP)
            a.ubfx(GTMP, GTMP, 29, 1)
            if sh:
                a.lsl(T0, T0, sh, sf=0)
                a.movz(T1, 1 << (sh - 16), 1, sf=0)
                (a.adds_r if m == "inc" else a.subs_r)(31, T0, T1, sf=0)
            else:
                (a.adds if m == "inc" else a.subs)(31, T0, 1, sf=sf)
            a.mrs_nzcv(T3)
            a.bfi(T3, GTMP, 29, 1)
            a.msr_nzcv(T3)
            if mat:
                a.ldur(GTMP, CTX_REG, CTX_EFLAGS, size=8)
                self._mkflags(0)
                a.ldur(T0, CTX_REG, CTX_EFLAGS, size=8)
                a.bfi(T0, GTMP, 0, 1)
                a.stur(T0, CTX_REG, CTX_EFLAGS, size=8)
            return

        # adc / sbb: GTMP holds incoming x86 CF (0 or 1).
        if sh:
            a.lsl(T0, T0, sh, sf=0)
            a.lsl(T1, T1, sh, sf=0)
            a.sub_r(T3, 31, GTMP, sf=0)          # 0 if cf==0 else 0xFFFFFFFF
            a.lsr(T3, T3, 32 - sh, sf=0)         # low sh bits filled iff cf==1
            if m == "adc":
                a.orr_r(T0, T0, T3, sf=0)
                a.lsl(T3, GTMP, 29)
                a.msr_nzcv(T3)
                a.adcs(31, T0, T1, sf=0)
            else:
                a.orr_r(T1, T1, T3, sf=0)
                a.eor_i(T3, GTMP, 1, sf=0)
                a.lsl(T3, T3, 29)
                a.msr_nzcv(T3)
                a.sbcs(31, T0, T1, sf=0)
        else:
            if m == "adc":
                a.lsl(T3, GTMP, 29)
                a.msr_nzcv(T3)
                a.adcs(31, T0, T1, sf=sf)
            else:
                a.eor_i(T3, GTMP, 1, sf=0)
                a.lsl(T3, T3, 29)
                a.msr_nzcv(T3)
                a.sbcs(31, T0, T1, sf=sf)
        self._maybe_mkflags(ins)

    def _mem_addr(self, m, rd):
        a = self.asm
        disp = m["disp"]
        sf = 0 if m.get("addrsize") == 4 else 1
        if m["rip"] or m.get("moffs") or (m["base"] is None and m["index"] is None):
            v = m["abs"] if m["abs"] is not None else disp
            a.li(rd, v & (0xFFFFFFFF if not sf else 0xFFFFFFFFFFFFFFFF))
            return
        base, idx = m["base"], m["index"]
        if idx is None:
            gb = GMAP[base]
            if disp == 0:
                if not sf or gb != rd:
                    a.mov(rd, gb, sf=sf)
            elif -256 <= disp <= 255:
                a.add(rd, gb, disp, sf=sf)
            elif 0 <= disp < 4096:
                a.add(rd, gb, disp, sf=sf)
            elif -4096 < disp < 0:
                a.sub(rd, gb, -disp, sf=sf)
            else:
                a.li(rd, disp if sf else (disp & 0xFFFFFFFF))
                a.add_r(rd, rd, gb, sf=sf)
            return
        if base is not None:
            a.add_r(rd, GMAP[base], GMAP[idx], sf=sf, shift=0, imm6=m["scale"])
        else:
            a.lsl(rd, GMAP[idx], m["scale"], sf=sf)
        if disp:
            if -4096 < disp < 4096:
                a.add(rd, rd, disp, sf=sf)
            else:
                a.li(GTMP, disp if sf else (disp & 0xFFFFFFFF))
                a.add_r(rd, rd, GTMP, sf=sf)

    def _emit_bmi(self, ins):
        """Emit BMI1 (andn/blsr/blsmsk/blsi/bextr) and BMI2 (bzhi/shlx/sarx/shrx/rorx/mulx/pdep/pext)."""
        a = self.asm
        m = ins.mnem
        sz = self._op_size(ins.ops[0])
        sf = 1 if sz == 8 else 0
        w = 64 if sf else 32
        need = self.needed.get(ins.addr, set())
        mat = ins.addr in self.mat

        if m == "andn":
            dst, src1, src2 = ins.ops
            self._load(ins, src2, T0)
            a.mov(T1, GMAP[src1[1]], sf=sf)
            a.bic_r(T2, T0, T1, sf=sf)
            a.mov(GMAP[dst[1]], T2, sf=sf)
            if need or mat:
                a.ands_r(31, T2, T2, sf=sf)
                if mat:
                    self._stash_res(ins, T2)
                    self._mkflags(0)
            return

        if m in ("blsr", "blsmsk", "blsi"):
            dst, src = ins.ops
            self._load(ins, src, T0)
            if m == "blsr":
                a.sub(T1, T0, 1, sf=sf)
                a.and_r(T2, T0, T1, sf=sf)
            elif m == "blsmsk":
                a.sub(T1, T0, 1, sf=sf)
                a.eor_r(T2, T0, T1, sf=sf)
            else:
                a.sub_r(T1, 31, T0, sf=sf)
                a.and_r(T2, T0, T1, sf=sf)
            a.mov(GMAP[dst[1]], T2, sf=sf)
            if need or mat:
                a.ands_r(31, T2, T2, sf=sf)
                if "C" in need or mat:
                    a.mrs_nzcv(T1)
                    a.cmp(T0, 0, sf=sf)
                    a.cset(T3, "ne" if m == "blsi" else "eq", sf=0)
                    a.bfi(T1, T3, 29, 1)
                    a.msr_nzcv(T1)
                if mat:
                    self._stash_res(ins, T2)
                    self._mkflags(0)
            return

        if m == "bextr":
            dst, src, ctrl = ins.ops
            self._load(ins, src, T0)
            g_ctrl = GMAP[ctrl[1]]
            a.uxtb(T1, g_ctrl)
            a.ubfx(T3, g_ctrl, 8, 8, sf=0)
            zero_lbl, join_lbl = self._skip_lbl(), self._skip_lbl()
            a.cmp(T1, w, sf=0)
            a.bcond("cs", zero_lbl)
            a.cbz(T3, zero_lbl, sf=0)
            a.lsrv(T2, T0, T1, sf=sf)
            a.cmp(T3, w, sf=0)
            a.bcond("cs", join_lbl)
            a.movz(GTMP, 1, 0, sf=sf)
            a.lslv(GTMP, GTMP, T3, sf=sf)
            a.sub(GTMP, GTMP, 1, sf=sf)
            a.and_r(T2, T2, GTMP, sf=sf)
            a.b(join_lbl)
            a.label(zero_lbl)
            a.movz(T2, 0, 0, sf=1)
            a.label(join_lbl)
            a.mov(GMAP[dst[1]], T2, sf=sf)
            if need or mat:
                a.ands_r(31, T2, T2, sf=sf)
                if mat:
                    self._stash_res(ins, T2)
                    self._mkflags(0)
            return

        if m == "bzhi":
            dst, src, idx = ins.ops
            self._load(ins, src, T0)
            a.uxtb(T1, GMAP[idx[1]])
            full_lbl, join_lbl = self._skip_lbl(), self._skip_lbl()
            a.cmp(T1, w, sf=0)
            a.bcond("cs", full_lbl)
            a.movz(T3, 1, 0, sf=sf)
            a.lslv(T3, T3, T1, sf=sf)
            a.sub(T3, T3, 1, sf=sf)
            a.and_r(T2, T0, T3, sf=sf)
            a.movz(GTMP, 0, 0, sf=0)
            a.b(join_lbl)
            a.label(full_lbl)
            a.mov(T2, T0, sf=sf)
            a.movz(GTMP, 1, 0, sf=0)
            a.label(join_lbl)
            a.mov(GMAP[dst[1]], T2, sf=sf)
            if need or mat:
                a.ands_r(31, T2, T2, sf=sf)
                if "C" in need or mat:
                    a.mrs_nzcv(T1)
                    a.bfi(T1, GTMP, 29, 1)
                    a.msr_nzcv(T1)
                if mat:
                    self._stash_res(ins, T2)
                    self._mkflags(0)
            return

        if m in ("shlx", "sarx", "shrx"):
            dst, src, cnt = ins.ops
            self._load(ins, src, T0)
            g_dst, g_cnt = GMAP[dst[1]], GMAP[cnt[1]]
            if m == "shlx":
                a.lslv(g_dst, T0, g_cnt, sf=sf)
            elif m == "sarx":
                a.asrv(g_dst, T0, g_cnt, sf=sf)
            else:
                a.lsrv(g_dst, T0, g_cnt, sf=sf)
            return

        if m == "rorx":
            dst, src, imm_op = ins.ops
            self._load(ins, src, T0)
            c = imm_op[1] & (w - 1)
            if c == 0:
                a.mov(GMAP[dst[1]], T0, sf=sf)
            else:
                a.ror(GMAP[dst[1]], T0, c, sf=sf)
            return

        if m == "mulx":
            hi, lo, src = ins.ops
            self._load(ins, src, T0)
            if sf:
                a.mul(T1, GMAP[2], T0, sf=1)
                a.umulh(T2, GMAP[2], T0)
            else:
                a.umaddl(T1, GMAP[2], T0, 31)
                a.lsr(T2, T1, 32, sf=1)
            a.mov(GMAP[lo[1]], T1, sf=sf)
            a.mov(GMAP[hi[1]], T2, sf=sf)
            return

        if m == "pdep":
            dst, src, mask = ins.ops
            self._load(ins, mask, T0)
            a.mov(T1, GMAP[src[1]], sf=sf)
            a.movz(T2, 0, 0, sf=1)
            loop_lbl, done_lbl = self._skip_lbl(), self._skip_lbl()
            a.label(loop_lbl)
            a.cbz(T0, done_lbl, sf=sf)
            a.sub_r(T3, 31, T0, sf=sf)
            a.and_r(T3, T0, T3, sf=sf)
            a.and_i(GTMP, T1, 1, sf=1)
            a.sub_r(GTMP, 31, GTMP, sf=1)
            a.and_r(GTMP, GTMP, T3, sf=sf)
            a.orr_r(T2, T2, GTMP, sf=sf)
            a.lsr(T1, T1, 1, sf=sf)
            a.eor_r(T0, T0, T3, sf=sf)
            a.b(loop_lbl)
            a.label(done_lbl)
            a.mov(GMAP[dst[1]], T2, sf=sf)
            return

        if m == "pext":
            dst, src, mask = ins.ops
            self._load(ins, mask, T0)
            a.mov(T1, GMAP[src[1]], sf=sf)
            a.movz(T2, 0, 0, sf=1)
            a.movz(ADDR_T, 1, 0, sf=1)
            loop_lbl, skip_lbl, done_lbl = self._skip_lbl(), self._skip_lbl(), self._skip_lbl()
            a.label(loop_lbl)
            a.cbz(T0, done_lbl, sf=sf)
            a.sub_r(T3, 31, T0, sf=sf)
            a.and_r(T3, T0, T3, sf=sf)
            a.and_r(GTMP, T1, T3, sf=sf)
            a.cbz(GTMP, skip_lbl, sf=sf)
            a.orr_r(T2, T2, ADDR_T, sf=sf)
            a.label(skip_lbl)
            a.lsl(ADDR_T, ADDR_T, 1, sf=sf)
            a.eor_r(T0, T0, T3, sf=sf)
            a.b(loop_lbl)
            a.label(done_lbl)
            a.mov(GMAP[dst[1]], T2, sf=sf)
            return

        raise UnhandledInsn(ins, f"bmi {m}")

    def _stash_res(self, ins, res_reg):
        need = self.needed.get(ins.addr, set())
        if "P" in need or "A" in need or ins.addr in self.mat:
            self.asm.stur(res_reg, CTX_REG, CTX_RES, size=8)

    def _mkflags(self, kind):
        a = self.asm
        a.movz(T0, kind, 0, sf=0)
        a.bl("rt_mkflags")

    # -- condition emission ----------------------------------------------------
    # arm cond encodings for a cc given the producer mnemonic. Values:
    #   "eq"..  single arm cond, "al" always, None = never taken,
    #   ("or",c1,c2) taken when either holds, ("and",c1,c2) taken when both hold.
    def _arm_cond(self, cc, mnem):
        if mnem in LOGIC_KIND:
            # C=V=0: o/b never taken, no/ae always; l<->mi, ge<->pl (V=0), be<->Z, a<->!Z
            return {"o": None, "no": "al", "b": None, "ae": "al", "e": "eq", "ne": "ne",
                    "be": "eq", "a": "ne", "s": "mi", "ns": "pl",
                    "p": "ctx", "np": "ctx",
                    "l": "mi", "ge": "pl", "le": ("or", "mi", "eq"), "g": ("and", "pl", "ne")}[cc]
        sub = mnem in SUB_KIND
        return {"o": "vs", "no": "vc", "b": "cc" if sub else "cs",
                "ae": "cs" if sub else "cc", "e": "eq", "ne": "ne",
                "be": "ls" if sub else ("or", "cs", "eq"),
                "a": "hi" if sub else ("and", "cc", "ne"),
                "s": "mi", "ns": "pl", "p": "ctx", "np": "ctx",
                "l": "lt", "ge": "ge", "le": "le", "g": "gt"}[cc]

    def _cond_branch(self, cond, lbl):
        """Emit a branch to lbl for a _arm_cond result; returns False when it can never be taken."""
        a = self.asm
        if cond == "al":
            a.b(lbl)
        elif cond is None:
            pass
            return False
        elif cond == "ctx":
            raise UnhandledInsn(None, "cond ctx")  # type: ignore
        elif isinstance(cond, tuple):
            if cond[0] == "or":
                a.bcond(cond[1], lbl)
                a.bcond(cond[2], lbl)
            else:
                skip = self._skip_lbl()
                a.bcond(INV[cond[1]], skip)
                a.bcond(cond[2], lbl)
                a.label(skip)
        else:
            a.bcond(cond, lbl)
        return True

    def _emit_jcc(self, ins):
        a = self.asm
        lbl = self._target_lbl(ins.ops[0][1])
        if ins.cc in ("p", "np"):
            self._parity_jcc(ins, lbl)
            return
        if self._producer_ok(ins):
            self._cond_branch(self._arm_cond(ins.cc, self.arm_path[ins.addr].mnem), lbl)
            return
        self._ctx_jcc(ins.cc, lbl)

    def _parity_jcc(self, ins, lbl):
        a = self.asm
        prod = self.arm_path.get(ins.addr)
        if prod is None or prod.mnem in ("popf", "sahf") or prod.addr in self.mat:
            a.ldr(T2, CTX_REG, CTX_EFLAGS, size=4)
            (a.tbnz if ins.cc == "p" else a.tbz)(T2, 2, lbl)
            return
        a.ldur(T2, CTX_REG, CTX_RES, size=8)
        a.eor_shift(T2, T2, T2, 1, 4, sf=0)
        a.eor_shift(T2, T2, T2, 1, 2, sf=0)
        a.eor_shift(T2, T2, T2, 1, 1, sf=0)
        (a.tbz if ins.cc == "p" else a.tbnz)(T2, 0, lbl)

    def _ctx_jcc(self, cc, lbl):
        a = self.asm
        a.ldr(T2, CTX_REG, CTX_EFLAGS, size=4)
        self._ctx_jcc_reg(cc, T2, lbl)

    def _ctx_jcc_reg(self, cc, w, lbl):
        a = self.asm
        simple = {"e": (6, True), "ne": (6, False), "b": (0, True),
                  "ae": (0, False), "s": (7, True), "ns": (7, False),
                  "o": (11, True), "no": (11, False), "p": (2, True),
                  "np": (2, False)}
        if cc in simple:
            bit, want_set = simple[cc]
            (a.tbnz if want_set else a.tbz)(w, bit, lbl)
            return
        if cc in ("be", "a"):
            a.tst_i(w, EF_CF | EF_ZF, sf=0)
            a.bcond("ne" if cc == "be" else "eq", lbl)
            return
        if cc in ("l", "ge", "le", "g"):
            a.eor_shift(T1, w, w, 1, 4, sf=0)  # T1 bit7 = SF ^ OF
            if cc == "l":
                a.tbnz(T1, 7, lbl)
            elif cc == "ge":
                a.tbz(T1, 7, lbl)
            elif cc == "le":
                a.tbnz(T1, 7, lbl)
                a.tbnz(w, 6, lbl)
            else:
                skip = self._skip_lbl()
                a.tbnz(T1, 7, skip)
                a.tbz(w, 6, lbl)
                a.label(skip)
            return
        raise UnhandledInsn(None, f"ctx cc {cc}")  # type: ignore

    def _emit_movbe(self, ins):
        if len(ins.ops) != 2 or not (
            (ins.ops[0][0] == "r" and ins.ops[1][0] == "m")
            or (ins.ops[0][0] == "m" and ins.ops[1][0] == "r")
        ):
            raise UnhandledInsn(ins, "invalid movbe operands")
        a = self.asm
        dst, src = ins.ops
        sz = self._op_size(dst)
        self._load(ins, src, T0)
        if sz == 2:
            a.rev16(T0, T0, sf=0)
        elif sz in (4, 8):
            a.rev(T0, T0, 1 if sz == 8 else 0)
        else:
            raise UnhandledInsn(ins, f"movbe size {sz}")
        self._store(ins, dst, T0)

    def _emit_crc32(self, ins):
        if len(ins.ops) != 2 or ins.ops[0][0] != "r":
            raise UnhandledInsn(ins, "invalid crc32 operands")
        a = self.asm
        dst, src = ins.ops
        src_sz = self._op_size(src)
        if src_sz not in (1, 2, 4, 8):
            raise UnhandledInsn(ins, f"crc32 size {src_sz}")
        g = GMAP[dst[1]]
        self._load(ins, src, T1)
        a.mov(T0, g, sf=0)
        a.eor_r(T0, T0, T1, sf=1)
        a.li(T1, 0x82F63B78)
        a.movz(T2, src_sz * 8, 0, sf=0)
        loop_lbl = self._skip_lbl()
        a.label(loop_lbl)
        a.and_i(T3, T0, 1, sf=1)
        a.sub_r(T3, 31, T3, sf=1)
        a.and_r(T3, T3, T1, sf=1)
        a.lsr(T0, T0, 1, sf=1)
        a.eor_r(T0, T0, T3, sf=1)
        a.sub(T2, T2, 1, sf=0)
        a.cbnz(T2, loop_lbl, sf=0)
        a.mov(g, T0, sf=0)

    # -- integer ops -------------------------------------------------------------
    def _emit_alu(self, ins):
        m = ins.mnem
        if m in ("cmp", "test"):
            self._emit_cmp(ins)
            return
        if m in ("inc", "dec"):
            self._emit_incdec(ins)
            return
        if m in ("adc", "sbb"):
            self._emit_adcsbb(ins)
            return
        if m in ("neg", "not"):
            self._emit_negnot(ins)
            return
        if m in ("clc", "stc", "cmc"):
            self._emit_flagctl(ins)
            return
        if m in ("sahf", "lahf"):
            a = self.asm
            if m == "sahf":
                # NZCV from ah: N=ah7, Z=ah6, C=ah0 (x86 polarity); V left as-is.
                # eflags word: replace SZ_A_P_C bits (mask 0xD5) from ah.
                a.ubfx(T0, GMAP[0], 8, 8)
                a.mrs_nzcv(GTMP)
                a.and_i(GTMP, GTMP, 0x1FFFFFFF)      # keep V, clear N Z C
                a.ubfx(T1, T0, 7, 1)
                a.bfi(GTMP, T1, 31, 1)
                a.ubfx(T1, T0, 6, 1)
                a.bfi(GTMP, T1, 30, 1)
                a.bfi(GTMP, T0, 29, 1)               # ah0 -> C
                a.msr_nzcv(GTMP)
                a.ldur(T1, CTX_REG, CTX_EFLAGS, size=8)
                a.bfi(T1, T0, 0, 8)                # low byte := ah flag bits
                a.stur(T1, CTX_REG, CTX_EFLAGS, size=8)
            else:
                self._mkflags(0)
                a.ldur(T0, CTX_REG, CTX_EFLAGS, size=8)
                a.bfi(GMAP[0], T0, 8, 8)
            self.arm_path[ins.addr] = ins
            return
        a = self.asm
        dst, src = ins.ops
        sz = self._op_size(dst)
        need = self.needed.get(ins.addr, set())
        flaggy = bool(need) or ins.addr in self.mat
        self._load(ins, src, T1, as_size=sz)
        self._load(ins, dst, T0, as_size=sz)
        # partial-width ops compute in a widened temp to get correct N/Z at sz;
        # stash unshifted operands first for lazy AF
        if "A" in need or ins.addr in self.mat:
            a.stur(T0, CTX_REG, CTX_A, size=8)
            a.stur(T1, CTX_REG, CTX_B, size=8)
        sh = (32 - sz * 8) if sz < 4 else 0
        if sh:
            a.lsl(T0, T0, sh, sf=0)
            a.lsl(T1, T1, sh, sf=0)
        self._alu_emit(ins, m, T0, T1, 1 if sz == 8 else 0, flaggy)
        if sh:
            a.lsr(T0, T0, sh, sf=0)
        self._store(ins, dst, T0)
        self._stash_res(ins, T0)
        self._maybe_mkflags(ins)

    def _alu_emit(self, ins, m, rd, rm, sf, flaggy):
        a = self.asm
        if m == "add":
            (a.adds_r if flaggy else a.add_r)(rd, rd, rm, sf)
        elif m == "sub":
            (a.subs_r if flaggy else a.sub_r)(rd, rd, rm, sf)
        elif m == "and":
            if flaggy:
                a.ands_r(rd, rd, rm, sf)
                self._clear_cv(ins)
            else:
                a.and_r(rd, rd, rm, sf)
        elif m in ("or", "xor"):
            (a.orr_r if m == "or" else a.eor_r)(rd, rd, rm, sf)
            if flaggy:
                a.tst_r(rd, rd, sf)
                self._clear_cv(ins)
        else:
            raise UnhandledInsn(ins, m)

    def _clear_cv(self, ins):
        need = self.needed.get(ins.addr, set())
        if "C" in need or "V" in need or ins.addr in self.mat:
            a = self.asm
            a.mrs_nzcv(GTMP)
            a.and_i(GTMP, GTMP, 0xC0000000)
            a.msr_nzcv(GTMP)

    def _maybe_mkflags(self, ins):
        if ins.addr in self.mat:
            # rt_mkflags preserves live ARM NZCV (needed by same-block consumers
            # on arm_path) and converts SUB_KIND borrow polarity when kind == 1.
            kind = 2 if ins.mnem in LOGIC_KIND else (1 if ins.mnem in SUB_KIND else 0)
            self._mkflags(kind)

    def _emit_cmp(self, ins):
        a = self.asm
        m = ins.mnem
        dst, src = ins.ops
        sz = self._op_size(dst)
        need = self.needed.get(ins.addr, set())
        self._load(ins, src, T1, as_size=sz)
        self._load(ins, dst, T0, as_size=sz)
        # stash unshifted result/operands before widening for flag semantics
        if "P" in need or "A" in need or ins.addr in self.mat:
            if m == "cmp":
                a.sub_r(T2, T0, T1, 1)
            else:
                a.and_r(T2, T0, T1, 1)
            a.stur(T2, CTX_REG, CTX_RES, size=8)
            if "A" in need or ins.addr in self.mat:
                a.stur(T0, CTX_REG, CTX_A, size=8)
                a.stur(T1, CTX_REG, CTX_B, size=8)
        sh = (32 - sz * 8) if sz < 4 else 0
        if sh:
            a.lsl(T0, T0, sh, sf=0)
            a.lsl(T1, T1, sh, sf=0)
        sf = 1 if sz == 8 else 0
        if m == "cmp":
            a.subs_r(31, T0, T1, sf)
        else:
            a.tst_r(T0, T1, sf)
            self._clear_cv(ins)
        self._maybe_mkflags(ins)

    def _emit_incdec(self, ins):
        a = self.asm
        op = ins.ops[0]
        sz = self._op_size(op)
        sf = 1 if sz == 8 else 0
        need = self.needed.get(ins.addr, set())
        flaggy = bool(need) or ins.addr in self.mat
        self._load(ins, op, T0)
        if flaggy and ("A" in need or ins.addr in self.mat):
            a.stur(T0, CTX_REG, CTX_A, size=8)
            a.movz(T1, 1, 0, sf=0)
            a.stur(T1, CTX_REG, CTX_B, size=8)
        sh = (32 - sz * 8) if sz < 4 else 0
        if not flaggy:
            (a.add if ins.mnem == "inc" else a.sub)(T0, T0, 1, sf)
            if sz == 1:
                a.uxtb(T0, T0)
            elif sz == 2:
                a.uxth(T0, T0)
            self._store(ins, op, T0)
            return
        if sh:
            a.lsl(T0, T0, sh, sf=0)
            a.movz(T1, 1 << (sh - 16), 1, sf=0)
        a.mrs_nzcv(GTMP)
        if sh:
            (a.adds_r if ins.mnem == "inc" else a.subs_r)(T0, T0, T1, sf=0)
        else:
            (a.adds if ins.mnem == "inc" else a.subs)(T0, T0, 1, sf)
        a.mrs_nzcv(T3)
        a.ubfx(GTMP, GTMP, 29, 1)
        a.bfi(T3, GTMP, 29, 1)
        a.msr_nzcv(T3)
        if sh:
            a.lsr(T0, T0, sh, sf=0)
        self._store(ins, op, T0)
        self._stash_res(ins, T0)
        if ins.addr in self.mat:
            # Preserve ctx.eflags CF (bit 0) and do not invert ARM C (dec does not write CF).
            a.ldur(GTMP, CTX_REG, CTX_EFLAGS, size=8)
            self._mkflags(0)
            a.ldur(T0, CTX_REG, CTX_EFLAGS, size=8)
            a.bfi(T0, GTMP, 0, 1)
            a.stur(T0, CTX_REG, CTX_EFLAGS, size=8)

    def _emit_negnot(self, ins):
        a = self.asm
        op = ins.ops[0]
        sz = self._op_size(op)
        sf = 1 if sz == 8 else 0
        self._load(ins, op, T0)
        if ins.mnem == "not":
            a.mvn(T0, T0, sf)
            if sz == 1:
                a.uxtb(T0, T0)
            elif sz == 2:
                a.uxth(T0, T0)
            self._store(ins, op, T0)
            return
        need = self.needed.get(ins.addr, set())
        if "A" in need or ins.addr in self.mat:
            a.stur(31, CTX_REG, CTX_A, size=8)
            a.stur(T0, CTX_REG, CTX_B, size=8)
        sh = (32 - sz * 8) if sz < 4 else 0
        if sh:
            a.lsl(T0, T0, sh, sf=0)
        a.subs_r(T0, 31, T0, sf)
        if sh:
            a.lsr(T0, T0, sh, sf=0)
        self._store(ins, op, T0)
        self._stash_res(ins, T0)
        self._maybe_mkflags(ins)

    def _emit_flagctl(self, ins):
        a = self.asm
        m = ins.mnem
        if m == "clc" or m == "stc":
            a.mrs_nzcv(T0)
            if m == "clc":
                a.and_i(T0, T0, 0xDFFFFFFF, sf=0)
            else:
                a.orr_i(T0, T0, 0x20000000)
            a.msr_nzcv(T0)
        else:
            # cmc: new x86 CF = !old. arm C holds x86 C for add-kind producers and
            # !x86 C for sub-kind, so the toggle is a no-op exactly for sub-kind.
            prod = self.arm_path.get(ins.addr)
            if prod is not None and "C" in FLAG_WRITERS.get(prod.mnem, ""):
                if prod.mnem not in SUB_KIND:
                    a.mrs_nzcv(T0)
                    a.eor_i(T0, T0, 0x20000000)
                    a.msr_nzcv(T0)
            else:
                a.ldur(T0, CTX_REG, CTX_EFLAGS, size=8)
                a.eor_i(T0, T0, EF_CF, sf=0)
                a.stur(T0, CTX_REG, CTX_EFLAGS, size=8)
                a.and_i(T0, T0, EF_CF, sf=0)
                a.mrs_nzcv(T1)
                a.bfi(T1, T0, 29, 1)
                a.msr_nzcv(T1)
        if ins.addr in self.mat:
            # Update only EF_CF (bit 0) of ctx.eflags from ARM NZCV bit 29 without
            # clobbering SF/ZF/OF/PF/AF via rt_mkflags.
            a.mrs_nzcv(T0)
            a.ubfx(T0, T0, 29, 1)
            a.ldr(T1, CTX_REG, CTX_EFLAGS, size=4)
            a.bfi(T1, T0, 0, 1)
            a.str(T1, CTX_REG, CTX_EFLAGS, size=4)

    def _emit_adcsbb(self, ins):
        a = self.asm
        m = ins.mnem
        dst, src = ins.ops
        sz = self._op_size(dst)
        if sz < 4:
            raise UnhandledInsn(ins, f"{m} size {sz}")
        sf = 1 if sz == 8 else 0
        # normalize arm C to the value adcs/sbcs needs:
        #   adc wants arm C = x86 CF, sbb wants arm C = !x86 CF (borrow).
        prod = self.arm_path.get(ins.addr)
        if prod is not None and "C" in FLAG_WRITERS.get(prod.mnem, ""):
            x86c = "cc" if prod.mnem in SUB_KIND else "cs"   # arm cond iff x86 CF=1
            if m == "sbb":
                x86c = INV[x86c]
            a.cset(T3, x86c, sf=0)
        else:
            a.ldur(T3, CTX_REG, CTX_EFLAGS, size=8)
            a.and_i(T3, T3, EF_CF, sf=0)
            if m == "sbb":
                a.eor_i(T3, T3, 1, sf=0)
        a.cmp(T3, 1, sf=0)                      # arm C := prepared carry-in
        self._load(ins, src, T1)
        self._load(ins, dst, T0)
        (a.adcs if m == "adc" else a.sbcs)(T0, T0, T1, sf)
        self._store(ins, dst, T0)
        self._stash_res(ins, T0)
        self._maybe_mkflags(ins)

    # -- moves / stack ----------------------------------------------------------
    def _emit_mov(self, ins):
        a = self.asm
        m = ins.mnem
        dst, src = ins.ops
        if m in ("mov", "movnti"):
            if dst[0] == "r" and src[0] == "r" and dst[2] == src[2] == 8:
                if GMAP[dst[1]] != GMAP[src[1]]:
                    a.mov(GMAP[dst[1]], GMAP[src[1]])
                return
            self._load(ins, src, T0, as_size=self._op_size(dst))
            self._store(ins, dst, T0)
        elif m == "movzx":
            self._load(ins, src, T0)
            self._store(ins, dst, T0)
        elif m == "movsx":
            if src[0] == "m":
                self._mem_addr(src[1], ADDR_T)
                a.ldr(T0, ADDR_T, 0, size=self._op_size(src), signed=True, target_sf=1)
            else:
                _, reg, osz = src[0], src[1], src[2]
                hi8 = len(src) > 3 and src[3]
                g = GMAP[reg]
                if hi8:
                    a.sbfx(T0, g, 8, 8)
                elif osz == 1:
                    a.sxtb(T0, g)
                elif osz == 2:
                    a.sxth(T0, g)
                else:
                    a.sxtw(T0, g)
            self._store(ins, dst, T0)
        elif m == "movsxd":
            self._load(ins, src, T0)
            a.sxtw(T0, T0)
            self._store(ins, dst, T0)
        else:
            raise UnhandledInsn(ins, m)

    def _emit_lea(self, ins):
        dst, src = ins.ops
        self._mem_addr(src[1], T0)
        self._store(ins, dst, T0)

    def _emit_pushpop(self, ins):
        a = self.asm
        rsp = GMAP[4]
        m = ins.mnem
        if m == "push":
            op = ins.ops[0]
            sz = 2 if (ins.opsize and not ins.rexw) else 8
            self._load(ins, op, T0, as_size=sz)
            a.str_pre(T0, rsp, -sz, size=sz)
        elif m == "pop":
            op = ins.ops[0]
            sz = 2 if (ins.opsize and not ins.rexw) else 8
            a.ldr_post(T0, rsp, sz, size=sz)
            if sz == 2:
                if op[0] == "r":
                    a.bfi(GMAP[op[1]], T0, 0, 16)
                elif op[0] == "m":
                    self._mem_addr(op[1], ADDR_T)
                    a.str(T0, ADDR_T, 0, size=2)
                else:
                    raise UnhandledInsn(ins, f"pop16 {op}")
            else:
                self._store(ins, op, T0)
        elif m == "pushf":
            self._mkflags(0)
            a.ldur(T0, CTX_REG, CTX_EFLAGS, size=8)
            a.orr_i(T0, T0, 2, sf=0)
            a.str_pre(T0, rsp, -8)
        elif m == "popf":
            a.ldr_post(T0, rsp, 8)
            a.stur(T0, CTX_REG, CTX_EFLAGS, size=8)
            # reflect SF/ZF/CF/OF into NZCV (x86-polarity C: consumers treat popf as add-kind)
            a.ubfx(T1, T0, 7, 1)
            a.lsl(T1, T1, 3)                          # N
            a.ubfx(T2, T0, 6, 1)
            a.lsl(T2, T2, 2)
            a.orr_r(T1, T1, T2, sf=0)                 # Z
            a.ubfx(T2, T0, 0, 1)
            a.lsl(T2, T2, 1)
            a.orr_r(T1, T1, T2, sf=0)                 # C
            a.ubfx(T2, T0, 11, 1)
            a.orr_r(T1, T1, T2, sf=0)                 # V
            a.lsl(T1, T1, 28)
            a.msr_nzcv(T1)
        else:
            raise UnhandledInsn(ins, m)

    def _emit_xchg(self, ins):
        if ins.mnem != "xchg":
            raise UnhandledInsn(ins, f"{ins.mnem} needs atomics")
        d, s = ins.ops
        self._load(ins, d, T0)
        self._load(ins, s, T1)
        self._store(ins, d, T1)
        self._store(ins, s, T0)

    def _emit_conv(self, ins):
        a = self.asm
        m = ins.mnem
        if m == "cbtw":
            if ins.rexw:
                a.sxtw(GMAP[0], GMAP[0])
            elif ins.opsize:
                a.sxtb(T0, GMAP[0], sf=0)
                a.bfi(GMAP[0], T0, 0, 16)
            else:
                a.sxth(GMAP[0], GMAP[0], sf=0)
        elif m == "cwtd":
            if ins.rexw:
                a.asr(GMAP[2], GMAP[0], 63)
            elif ins.opsize:
                a.sxth(T0, GMAP[0], sf=0)
                a.asr(T0, T0, 16, sf=0)
                a.bfi(GMAP[2], T0, 0, 16)
            else:
                a.asr(GMAP[2], GMAP[0], 31, sf=0)
        else:
            raise UnhandledInsn(ins, m)

    def _emit_shift(self, ins):
        a = self.asm
        m = ins.mnem
        dst, cnt = ins.ops
        sz = self._op_size(dst)
        sf = 1 if sz == 8 else 0
        width = sz * 8
        if m in ("rcl", "rcr"):
            raise UnhandledInsn(ins, m)
        need = self.needed.get(ins.addr, set())
        flaggy = bool(need) or ins.addr in self.mat
        need_c = "C" in need or ins.addr in self.mat
        if cnt[0] == "i":
            cval = (cnt[1] & 0x1F) if width < 64 else (cnt[1] & 0x3F)
            if cval == 0:
                return
            self._load(ins, dst, T0)
            if m == "sar" and width < 32:
                (a.sxtb if width == 8 else a.sxth)(T0, T0, sf=0)
            a.mov(T2, T0)  # original for CF
            if m in ("shl", "sal"):
                if cval >= width:
                    a.movz(T0, 0, 0, sf=0)
                else:
                    a.lsl(T0, T0, cval, sf)
                    if width == 8:
                        a.uxtb(T0, T0)
                    elif width == 16:
                        a.uxth(T0, T0)
            elif m == "shr":
                if cval >= width:
                    a.movz(T0, 0, 0, sf=0)
                else:
                    a.lsr(T0, T0, cval, sf)
            elif m == "sar":
                a.asr(T0, T0, cval, sf)
                if width == 8:
                    a.uxtb(T0, T0)
                elif width == 16:
                    a.uxth(T0, T0)
            elif m in ("rol", "ror"):
                r = cval % width
                if r != 0:
                    if width < 32:
                        if width == 8:
                            a.orr_r(T0, T0, T0, sf=0, shift=0, imm6=8)
                        a.orr_r(T0, T0, T0, sf=0, shift=0, imm6=16)
                        a.ror(T0, T0, r if m == "ror" else width - r, sf=0)
                        (a.uxtb if width == 8 else a.uxth)(T0, T0)
                    else:
                        a.ror(T0, T0, r if m == "ror" else width - r, sf)
            self._store(ins, dst, T0)
            if flaggy:
                if m in ("rol", "ror"):
                    if need_c:
                        self._shift_cf_imm(ins, m, cval, width)
                        if ins.addr in self.mat:
                            a.ldur(T1, CTX_REG, CTX_EFLAGS, size=8)
                            a.bfi(T1, T3, 0, 1)
                            a.stur(T1, CTX_REG, CTX_EFLAGS, size=8)
                else:
                    if width < 32:
                        (a.sxtb if width == 8 else a.sxth)(T3, T0, sf=0)
                        a.tst_r(T3, T3, sf=0)
                    else:
                        res = GMAP[dst[1]] if dst[0] == "r" else T0
                        a.tst_r(res, res, sf)
                    if need_c:
                        self._shift_cf_imm(ins, m, cval, width)
                    self._stash_res(ins, T0)
                    self._maybe_mkflags(ins)
            return
        # variable count in cl
        self._load(ins, dst, T0)
        if m == "sar" and width < 32:
            (a.sxtb if width == 8 else a.sxth)(T0, T0, sf=0)
        a.mov(T2, T0)
        a.mov(T1, GMAP[1], sf=0)
        a.and_i(T1, T1, 0x3F if width == 64 else 0x1F, sf=0)
        skip = self._skip_lbl()
        # A count of 0 skips the whole shift, its flag word included: _flag_analysis makes every
        # consumer behind a shift by cl read ctx and has the writer before it store its word, so
        # ctx still holds the flags x86 leaves untouched.
        a.cbz(T1, skip, sf=0)
        if m in ("shl", "sal"):
            a.lslv(T0, T0, T1, sf)
            if width == 8:
                a.uxtb(T0, T0)
            elif width == 16:
                a.uxth(T0, T0)
        elif m == "shr":
            a.lsrv(T0, T0, T1, sf)
        elif m == "sar":
            a.asrv(T0, T0, T1, sf)
            if width == 8:
                a.uxtb(T0, T0)
            elif width == 16:
                a.uxth(T0, T0)
        elif m in ("rol", "ror"):
            if width < 32:
                if width == 8:
                    a.orr_r(T0, T0, T0, sf=0, shift=0, imm6=8)
                a.orr_r(T0, T0, T0, sf=0, shift=0, imm6=16)
                if m == "ror":
                    a.rorv(T0, T0, T1, sf=0)
                else:
                    a.movz(T3, 32, 0, sf=0)
                    a.sub_r(T3, T3, T1, sf=0)
                    a.rorv(T0, T0, T3, sf=0)
                (a.uxtb if width == 8 else a.uxth)(T0, T0)
            elif m == "ror":
                a.rorv(T0, T0, T1, sf)
            else:
                a.movz(T3, width, 0, sf=0)
                a.sub_r(T3, T3, T1, sf=0)
                a.rorv(T0, T0, T3, sf)
        self._store(ins, dst, T0)
        if flaggy:
            if m in ("rol", "ror"):
                if need_c:
                    self._shift_cf_var(ins, m, width)
                    if ins.addr in self.mat:
                        a.ldur(T1, CTX_REG, CTX_EFLAGS, size=8)
                        a.bfi(T1, T3, 0, 1)
                        a.stur(T1, CTX_REG, CTX_EFLAGS, size=8)
            else:
                if width < 32:
                    (a.sxtb if width == 8 else a.sxth)(T3, T0, sf=0)
                    a.tst_r(T3, T3, sf=0)
                else:
                    res = GMAP[dst[1]] if dst[0] == "r" else T0
                    a.tst_r(res, res, sf)
                if need_c:
                    self._shift_cf_var(ins, m, width)
                self._stash_res(ins, T0)
                self._maybe_mkflags(ins)
        a.label(skip)

    def _set_cf(self, bit_reg):
        """Insert bit0 of bit_reg into arm C, keeping N/Z/V."""
        a = self.asm
        a.mrs_nzcv(GTMP)
        a.bfi(GTMP, bit_reg, 29, 1)
        a.msr_nzcv(GTMP)

    def _shift_cf_imm(self, ins, m, cval, width):
        a = self.asm
        sf = 1 if width == 64 else 0
        if m == "rol":
            a.and_i(T3, T0, 1, sf=0)
        elif m == "ror":
            a.ubfx(T3, T0, width - 1, 1, sf)
        elif m == "sar":
            a.ubfx(T3, T2, min(cval - 1, width - 1), 1, sf)
        elif cval > width:
            a.movz(T3, 0, 0, sf=0)
        elif m in ("shl", "sal"):
            a.ubfx(T3, T2, width - cval, 1, sf)
        elif m == "shr":
            a.ubfx(T3, T2, cval - 1, 1, sf)
        self._set_cf(T3)

    def _shift_cf_var(self, ins, m, width):
        a = self.asm
        sf = 1 if width == 64 else 0
        # count in T1, original in T2, result in T0
        if m in ("shl", "sal"):
            a.movz(T3, width, 0, sf=0)
            a.sub_r(T3, T3, T1, sf=0)
            a.lsrv(T3, T2, T3, sf)
        elif m == "shr":
            a.sub(T3, T1, 1, sf=0)
            a.lsrv(T3, T2, T3, sf)
        elif m == "sar":
            a.sub(T3, T1, 1, sf=0)
            a.asrv(T3, T2, T3, sf)
        elif m == "rol":
            a.mov(T3, T0, sf=0)
        elif m == "ror":
            a.lsr(T3, T0, width - 1, sf)
        a.and_i(T3, T3, 1, sf=0)
        self._set_cf(T3)

    def _emit_mul(self, ins):
        a = self.asm
        m = ins.mnem
        need = self.needed.get(ins.addr, set())
        want_cv = "C" in need or "V" in need or ins.addr in self.mat

        def emit_cv(ovf_reg):
            a.mrs_nzcv(GTMP)
            a.and_i(GTMP, GTMP, 0xC0000000)   # keep N,Z; clear C,V
            a.bfi(GTMP, ovf_reg, 28, 1)       # V
            a.bfi(GTMP, ovf_reg, 29, 1)       # C
            a.msr_nzcv(GTMP)
            self._maybe_mkflags(ins)

        if m == "imul" and len(ins.ops) >= 2:
            dst, src = ins.ops[0], ins.ops[1]
            sz = self._op_size(dst)
            self._load(ins, src, T0)
            if len(ins.ops) > 2:
                imm = ins.ops[2][1] & 0xFFFFFFFFFFFFFFFF
                if imm == 0xFFFFFFFFFFFFFFFF:
                    a.movn(T1, 0)
                else:
                    a.li(T1, imm)
            else:
                self._load(ins, dst, T1)
            if sz == 2:
                a.sxth(T0, T0, sf=0)
                a.sxth(T1, T1, sf=0)
                a.mul(T2, T1, T0, sf=0)
                self._store(ins, dst, T2)
                if want_cv:
                    a.sxth(T3, T2, sf=0)
                    a.cmp_r(T2, T3, sf=0)
                    a.cset(T3, "ne", sf=0)
                    emit_cv(T3)
            elif sz == 4:
                a.smaddl(T2, T1, T0, 31)
                self._store(ins, dst, T2)
                if want_cv:
                    a.sxtw(T3, T2)
                    a.cmp_r(T2, T3, sf=1)
                    a.cset(T3, "ne", sf=0)
                    emit_cv(T3)
            elif sz == 8:
                a.mul(T2, T1, T0, sf=1)
                if want_cv:
                    a.smulh(T3, T1, T0)
                self._store(ins, dst, T2)
                if want_cv:
                    a.asr(GTMP, T2, 63, sf=1)
                    a.cmp_r(T3, GTMP, sf=1)
                    a.cset(T3, "ne", sf=0)
                    emit_cv(T3)
            else:
                raise UnhandledInsn(ins, f"imul size {sz}")
            return
        if m in ("mul", "imul") and len(ins.ops) == 1:
            src = ins.ops[0]
            sz = self._op_size(src)
            self._load(ins, src, T0)
            if sz == 1:
                if m == "mul":
                    a.uxtb(T1, GMAP[0])
                    a.mul(T2, T1, T0, sf=0)
                    a.bfi(GMAP[0], T2, 0, 16)
                    if want_cv:
                        a.ubfx(T3, T2, 8, 8, sf=0)
                        a.cmp(T3, 0, sf=0)
                        a.cset(T3, "ne", sf=0)
                        emit_cv(T3)
                else:
                    a.sxtb(T0, T0, sf=0)
                    a.sxtb(T1, GMAP[0], sf=0)
                    a.mul(T2, T1, T0, sf=0)
                    a.bfi(GMAP[0], T2, 0, 16)
                    if want_cv:
                        a.sxtb(T3, T2, sf=0)
                        a.cmp_r(T2, T3, sf=0)
                        a.cset(T3, "ne", sf=0)
                        emit_cv(T3)
            elif sz == 2:
                if m == "mul":
                    a.uxth(T1, GMAP[0])
                    a.mul(T2, T1, T0, sf=0)
                    a.bfi(GMAP[0], T2, 0, 16)
                    a.lsr(T3, T2, 16, sf=0)
                    a.bfi(GMAP[2], T3, 0, 16)
                    if want_cv:
                        a.cmp(T3, 0, sf=0)
                        a.cset(T3, "ne", sf=0)
                        emit_cv(T3)
                else:
                    a.sxth(T0, T0, sf=0)
                    a.sxth(T1, GMAP[0], sf=0)
                    a.mul(T2, T1, T0, sf=0)
                    a.bfi(GMAP[0], T2, 0, 16)
                    a.lsr(T3, T2, 16, sf=0)
                    a.bfi(GMAP[2], T3, 0, 16)
                    if want_cv:
                        a.sxth(T3, T2, sf=0)
                        a.cmp_r(T2, T3, sf=0)
                        a.cset(T3, "ne", sf=0)
                        emit_cv(T3)
            elif sz == 4:
                (a.umaddl if m == "mul" else a.smaddl)(T1, GMAP[0], T0, 31)
                a.mov(GMAP[0], T1, sf=0)
                a.lsr(GMAP[2], T1, 32)
                if want_cv:
                    if m == "mul":
                        a.cmp(GMAP[2], 0, sf=0)
                    else:
                        a.asr(T3, GMAP[0], 31, sf=0)
                        a.cmp_r(GMAP[2], T3, sf=0)
                    a.cset(T3, "ne", sf=0)
                    emit_cv(T3)
            elif sz == 8:
                (a.umulh if m == "mul" else a.smulh)(T2, GMAP[0], T0)
                a.mul(GMAP[0], GMAP[0], T0, sf=1)
                a.mov(GMAP[2], T2, sf=1)
                if want_cv:
                    if m == "mul":
                        a.cmp(GMAP[2], 0, sf=1)
                    else:
                        a.asr(T3, GMAP[0], 63, sf=1)
                        a.cmp_r(GMAP[2], T3, sf=1)
                    a.cset(T3, "ne", sf=0)
                    emit_cv(T3)
            else:
                raise UnhandledInsn(ins, f"{m} size {sz}")
            return
        if m in ("div", "idiv") and len(ins.ops) == 1:
            src = ins.ops[0]
            sz = self._op_size(src)
            self._load(ins, src, T1)
            if sz == 1:
                if m == "idiv":
                    a.sxtb(T1, T1, sf=0)
                    a.sxth(T0, GMAP[0], sf=0)
                else:
                    a.uxth(T0, GMAP[0])
                ok_nz, ok_ovf = self._skip_lbl(), self._skip_lbl()
                a.cbnz(T1, ok_nz, sf=0)
                a.brk(0xD1)
                a.label(ok_nz)
                (a.udiv if m == "div" else a.sdiv)(T2, T0, T1, sf=0)
                if m == "div":
                    a.lsr(GTMP, T2, 8, sf=0)
                    a.cbz(GTMP, ok_ovf, sf=0)
                else:
                    a.sxtb(GTMP, T2, sf=0)
                    a.cmp_r(T2, GTMP, sf=0)
                    a.bcond("eq", ok_ovf)
                a.brk(0xD2)
                a.label(ok_ovf)
                a.msub(T3, T2, T1, T0, sf=0)
                a.bfi(GMAP[0], T2, 0, 8)
                a.bfi(GMAP[0], T3, 8, 8)
            elif sz == 2:
                if m == "idiv":
                    a.sxth(T1, T1, sf=0)
                ok_nz, ok_ovf = self._skip_lbl(), self._skip_lbl()
                a.cbnz(T1, ok_nz, sf=0)
                a.brk(0xD1)
                a.label(ok_nz)
                a.uxth(T0, GMAP[0])
                a.bfi(T0, GMAP[2], 16, 16)
                (a.udiv if m == "div" else a.sdiv)(T2, T0, T1, sf=0)
                if m == "div":
                    a.lsr(GTMP, T2, 16, sf=0)
                    a.cbz(GTMP, ok_ovf, sf=0)
                else:
                    a.sxth(GTMP, T2, sf=0)
                    a.cmp_r(T2, GTMP, sf=0)
                    a.bcond("eq", ok_ovf)
                a.brk(0xD2)
                a.label(ok_ovf)
                a.msub(T3, T2, T1, T0, sf=0)
                a.bfi(GMAP[0], T2, 0, 16)
                a.bfi(GMAP[2], T3, 0, 16)
            elif sz == 4:
                if m == "idiv":
                    a.sxtw(T1, T1)
                ok_nz, ok_ovf = self._skip_lbl(), self._skip_lbl()
                a.cbnz(T1, ok_nz, sf=1)
                a.brk(0xD1)
                a.label(ok_nz)
                # dividend = edx:eax
                a.mov(T0, GMAP[0], sf=0)
                a.orr_r(T0, T0, GMAP[2], sf=1, shift=0, imm6=32)
                (a.udiv if m == "div" else a.sdiv)(T2, T0, T1, sf=1)
                if m == "div":
                    a.lsr(GTMP, T2, 32, sf=1)
                    a.cbz(GTMP, ok_ovf, sf=1)
                else:
                    a.sxtw(GTMP, T2)
                    a.cmp_r(T2, GTMP, sf=1)
                    a.bcond("eq", ok_ovf)
                a.brk(0xD2)
                a.label(ok_ovf)
                a.msub(T3, T2, T1, T0, sf=1)
                a.mov(GMAP[0], T2, sf=0)
                a.mov(GMAP[2], T3, sf=0)
            elif sz == 8:
                ok_nz = self._skip_lbl()
                a.cbnz(T1, ok_nz, sf=1)
                a.brk(0xD1)
                a.label(ok_nz)
                slow, done = self._skip_lbl(), self._skip_lbl()
                if m == "div":
                    # Fast path when rdx == 0 (plain 64 / 64 unsigned division).
                    a.cbnz(GMAP[2], slow, sf=1)
                    a.udiv(T2, GMAP[0], T1, sf=1)
                    a.msub(T3, T2, T1, GMAP[0], sf=1)
                    a.mov(GMAP[0], T2, sf=1)
                    a.mov(GMAP[2], T3, sf=1)
                    a.b(done)
                    # Full 128-by-64 unsigned restoring division: T2:T0 / T1.
                    a.label(slow)
                    a.mov(T0, GMAP[0], sf=1)
                    a.mov(T2, GMAP[2], sf=1)
                    ok_ovf = self._skip_lbl()
                    a.cmp_r(T2, T1, sf=1)
                    a.bcond("cc", ok_ovf)
                    a.brk(0xD2)
                    a.label(ok_ovf)
                    a.movz(T3, 64, 0, sf=0)
                    loop, do_sub, no_sub = self._skip_lbl(), self._skip_lbl(), self._skip_lbl()
                    a.label(loop)
                    a.lsr(GTMP, T2, 63, sf=1)
                    a.lsl(T2, T2, 1, sf=1)
                    a.orr_r(T2, T2, T0, sf=1, shift=1, imm6=63)
                    a.lsl(T0, T0, 1, sf=1)
                    a.cbnz(GTMP, do_sub, sf=1)
                    a.cmp_r(T2, T1, sf=1)
                    a.bcond("cc", no_sub)
                    a.label(do_sub)
                    a.sub_r(T2, T2, T1, sf=1)
                    a.orr_i(T0, T0, 1, sf=1)
                    a.label(no_sub)
                    a.subs(T3, T3, 1, sf=0)
                    a.bcond("ne", loop)
                    a.mov(GMAP[0], T0, sf=1)
                    a.mov(GMAP[2], T2, sf=1)
                    a.label(done)
                else:
                    # Fast path when rdx == (rax >> 63) and divisor != -1 (handles cqo; idiv r64).
                    a.asr(T0, GMAP[0], 63, sf=1)
                    a.cmp_r(GMAP[2], T0, sf=1)
                    a.bcond("ne", slow)
                    a.adds(GTMP, T1, 1, sf=1)
                    a.bcond("eq", slow)
                    a.sdiv(T2, GMAP[0], T1, sf=1)
                    a.msub(T3, T2, T1, GMAP[0], sf=1)
                    a.mov(GMAP[0], T2, sf=1)
                    a.mov(GMAP[2], T3, sf=1)
                    a.b(done)
                    # Full 128-by-64 signed division: T3[7:0]=64, T3[8]=sign(dividend), T3[9]=sign(divisor).
                    a.label(slow)
                    a.mov(T0, GMAP[0], sf=1)
                    a.mov(T2, GMAP[2], sf=1)
                    a.lsr(T3, T2, 63, sf=1)
                    a.lsl(T3, T3, 8, sf=1)
                    a.lsr(GTMP, T1, 63, sf=1)
                    a.orr_r(T3, T3, GTMP, sf=1, shift=0, imm6=9)
                    a.add(T3, T3, 64, sf=1)
                    pos_divd, pos_divr = self._skip_lbl(), self._skip_lbl()
                    a.tbz(T2, 63, pos_divd)
                    a.subs_r(T0, 31, T0, sf=1)
                    a.sbcs(T2, 31, T2, sf=1)
                    a.label(pos_divd)
                    a.tbz(T1, 63, pos_divr)
                    a.sub_r(T1, 31, T1, sf=1)
                    a.label(pos_divr)
                    ok_uovf = self._skip_lbl()
                    a.cmp_r(T2, T1, sf=1)
                    a.bcond("cc", ok_uovf)
                    a.brk(0xD2)
                    a.label(ok_uovf)
                    loop, do_sub, no_sub = self._skip_lbl(), self._skip_lbl(), self._skip_lbl()
                    a.label(loop)
                    a.lsr(GTMP, T2, 63, sf=1)
                    a.lsl(T2, T2, 1, sf=1)
                    a.orr_r(T2, T2, T0, sf=1, shift=1, imm6=63)
                    a.lsl(T0, T0, 1, sf=1)
                    a.cbnz(GTMP, do_sub, sf=1)
                    a.cmp_r(T2, T1, sf=1)
                    a.bcond("cc", no_sub)
                    a.label(do_sub)
                    a.sub_r(T2, T2, T1, sf=1)
                    a.orr_i(T0, T0, 1, sf=1)
                    a.label(no_sub)
                    a.sub(T3, T3, 1, sf=1)
                    a.tst_i(T3, 0xFF, sf=0)
                    a.bcond("ne", loop)
                    # Quotient sign = bit8 ^ bit9; max unsigned magnitude is 2^63-1 (pos) or 2^63 (neg).
                    a.lsr(GTMP, T3, 8, sf=1)
                    a.eor_shift(GTMP, GTMP, GTMP, 1, 1, sf=0)
                    a.and_i(GTMP, GTMP, 1, sf=0)
                    a.movn(T1, 0, 0, sf=1)
                    a.lsr(T1, T1, 1, sf=1)
                    a.add_r(T1, T1, GTMP, sf=1)
                    ok_sovf = self._skip_lbl()
                    a.cmp_r(T0, T1, sf=1)
                    a.bcond("ls", ok_sovf)
                    a.brk(0xD2)
                    a.label(ok_sovf)
                    skip_neg_q, skip_neg_r = self._skip_lbl(), self._skip_lbl()
                    a.cbz(GTMP, skip_neg_q, sf=0)
                    a.sub_r(T0, 31, T0, sf=1)
                    a.label(skip_neg_q)
                    a.tbz(T3, 8, skip_neg_r)
                    a.sub_r(T2, 31, T2, sf=1)
                    a.label(skip_neg_r)
                    a.mov(GMAP[0], T0, sf=1)
                    a.mov(GMAP[2], T2, sf=1)
                    a.label(done)
            else:
                raise UnhandledInsn(ins, f"{m} size {sz}")
            return
        raise UnhandledInsn(ins, f"{m} form")

    # -- string ops --------------------------------------------------------------
    def _emit_str(self, ins):
        a = self.asm
        m = ins.mnem
        sz = ins.strsize or 8
        if ins.rep == "f2" and m not in ("scas", "cmps"):
            raise UnhandledInsn(ins, "repne on non-compare string op")
        if m in ("movs", "stos"):
            self._emit_movs_stos(ins, sz)
        elif m in ("scas", "cmps"):
            self._emit_scas_cmps(ins, sz)
        else:
            raise UnhandledInsn(ins, m)

    def _emit_movs_stos(self, ins, sz):
        a = self.asm
        rsi, rdi, rcx = GMAP[6], GMAP[7], GMAP[1]
        if ins.mnem == "movs":
            load_src, store_dst = rsi, rdi
        else:
            load_src, store_dst = None, rdi
        rep = ins.rep == "f3"
        top = self._skip_lbl()
        done = self._skip_lbl()
        if rep:
            a.cbz(rcx, done)
        a.label(top)
        if ins.mnem == "movs":
            a.ldr(T0, load_src, 0, size=sz)
            a.add(load_src, load_src, sz)
        else:
            op = ("r", 0, sz)
            self._load(ins, op, T0)
        a.str(T0, store_dst, 0, size=sz)
        a.add(store_dst, store_dst, sz)
        if rep:
            a.sub(rcx, rcx, 1)
            a.cbnz(rcx, top)
        a.label(done)

    def _emit_scas_cmps(self, ins, sz):
        a = self.asm
        rsi, rdi, rcx = GMAP[6], GMAP[7], GMAP[1]
        rep = ins.rep  # f3 = repe/repz, f2 = repne
        top = self._skip_lbl()
        done = self._skip_lbl()
        if rep:
            a.cbz(rcx, done)
            a.label(top)
        if ins.mnem == "scas":
            a.ldr(T0, rdi, 0, size=sz)
            a.add(rdi, rdi, sz)
            a.mov(T1, GMAP[0], sf=0 if sz < 8 else 1)
            if sz < 8:
                sh = 32 - sz * 8
                a.lsl(T0, T0, sh, sf=0)
                a.lsl(T1, T1, sh, sf=0)
            a.subs_r(31, T1, T0, 0 if sz < 8 else 1)
        else:
            a.ldr(T0, rsi, 0, size=sz)
            a.add(rsi, rsi, sz)
            a.ldr(T1, rdi, 0, size=sz)
            a.add(rdi, rdi, sz)
            if sz < 8:
                sh = 32 - sz * 8
                a.lsl(T0, T0, sh, sf=0)
                a.lsl(T1, T1, sh, sf=0)
            a.subs_r(31, T0, T1, 0 if sz < 8 else 1)
        if rep:
            a.sub(rcx, rcx, 1)
            a.cbz(rcx, done)
            a.bcond("eq" if rep == "f3" else "ne", top)
            a.label(done)

    # -- control flow ------------------------------------------------------------
    def _emit_branch(self, ins):
        a = self.asm
        if ins.cc == "al":
            op = ins.ops[0]
            if op[0] == "a":
                a.b(self._target_lbl(op[1]))
            else:
                self._load(ins, op, ADDR_T, as_size=8)
                a.b("rt_dispatch")
            return
        if ins.cc == "jcxz":
            a.cbz(GMAP[1], self._target_lbl(ins.ops[0][1]), sf=0 if ins.addrsize else 1)
            return
        if ins.cc in ("loop", "loope", "loopne"):
            sf = 0 if ins.addrsize else 1
            lbl = self._target_lbl(ins.ops[0][1])
            a.sub(GMAP[1], GMAP[1], 1, sf)
            if ins.cc == "loop":
                a.cbnz(GMAP[1], lbl, sf)
            else:
                skip = self._skip_lbl()
                a.cbz(GMAP[1], skip, sf)
                prod = self.arm_path.get(ins.addr)
                if prod is not None and "Z" in FLAG_WRITERS.get(prod.mnem, ""):
                    a.bcond("eq" if ins.cc == "loope" else "ne", lbl)
                else:
                    self._ctx_jcc("e" if ins.cc == "loope" else "ne", lbl)
                a.label(skip)
            return
        self._emit_jcc(ins)

    def _emit_call(self, ins):
        a = self.asm
        op = ins.ops[0]
        if op[0] == "a" and op[1] in self.by_addr:
            a.li(T0, ins.addr + ins.size)
            a.str_pre(T0, GMAP[4], -8)
            a.b(self._lbl(op[1]))
        else:
            self._load(ins, op, ADDR_T, as_size=8)
            a.li(T0, ins.addr + ins.size)
            a.str_pre(T0, GMAP[4], -8)
            a.b("rt_dispatch")

    def _emit_ret(self, ins):
        a = self.asm
        a.ldr_post(ADDR_T, GMAP[4], 8)
        if ins.ops and ins.ops[0][0] == "i":
            imm = ins.ops[0][1] & 0xFFFF
            if imm:
                if imm < 4096:
                    a.add(GMAP[4], GMAP[4], imm)
                else:
                    a.li(T0, imm)
                    a.add_r(GMAP[4], GMAP[4], T0)
        a.b("rt_dispatch")

    def _emit_leave(self, ins):
        a = self.asm
        a.mov(GMAP[4], GMAP[5])
        a.ldr_post(GMAP[5], GMAP[4], 8)

    def _emit_one(self, ins):
        k, m = ins.kind, ins.mnem
        if k == "mov":
            self._emit_mov(ins)
        elif k == "alu" and ins.lock and ins.ops and ins.ops[0][0] == "m" and m in self._LOCK_ALU_OPS:
            self._emit_lock_alu(ins)
        elif k == "lea":
            self._emit_lea(ins)
        elif k == "alu":
            self._emit_alu(ins)
        elif m == "movbe":
            self._emit_movbe(ins)
        elif m == "crc32":
            self._emit_crc32(ins)
        elif k in ("push", "pop"):
            self._emit_pushpop(ins)
        elif k == "xchg":
            self._emit_xchg(ins)
        elif k == "conv":
            self._emit_conv(ins)
        elif k == "shift":
            self._emit_shift(ins)
        elif k == "mul":
            self._emit_mul(ins)
        elif k == "str":
            self._emit_str(ins)
        elif k == "cc":
            self._emit_setcc(ins) if m == "setcc" else self._emit_cmov(ins)
        elif k == "branch":
            self._emit_branch(ins)
        elif k == "call":
            self._emit_call(ins)
        elif k == "bmi":
            self._emit_bmi(ins)
        elif k == "ret":
            self._emit_ret(ins)
        elif k == "leave":
            self._emit_leave(ins)
        elif m == "bswap":
            sz = self._op_size(ins.ops[0])
            g = GMAP[ins.ops[0][1]]
            self.asm.rev(g, g, 1 if sz == 8 else 0)
        elif m == "cld":
            pass
        elif m == "std":
            raise UnhandledInsn(ins, "std/DF=1")
        elif k == "nop":
            pass
        elif m == "xlat":
            self._emit_xlat(ins)
        elif m == "enter":
            self._emit_enter(ins)
        else:
            raise UnhandledInsn(ins, f"kind {k}")

    def _producer_ok(self, ins):
        """True when the reaching arm-NZCV producer actually writes every flag the cc reads."""
        prod = self.arm_path.get(ins.addr)
        if prod is None or prod.mnem not in FLAG_WRITERS:
            return False
        return set(CC_FLAGS[ins.cc]) <= set(FLAG_WRITERS[prod.mnem])

    def _emit_setcc(self, ins):
        a = self.asm
        op = ins.ops[0]
        tl, dl = self._skip_lbl(), self._skip_lbl()
        if ins.cc in ("p", "np"):
            self._parity_jcc(ins, tl)
        elif self._producer_ok(ins):
            cond = self._arm_cond(ins.cc, self.arm_path[ins.addr].mnem)
            self._cond_branch(cond, tl)
        else:
            self._ctx_jcc(ins.cc, tl)
        a.movz(T0, 0, 0, sf=0)
        a.b(dl)
        a.label(tl)
        a.movz(T0, 1, 0, sf=0)
        a.label(dl)
        self._store(ins, op, T0)

    def _emit_cmov(self, ins):
        a = self.asm
        dst, src = ins.ops
        if dst[0] != "r":
            raise UnhandledInsn(ins, "cmov to memory")
        self._load(ins, src, T0)
        g = GMAP[dst[1]]
        sf = 1 if self._op_size(dst) == 8 else 0
        if ins.cc not in ("p", "np") and self._producer_ok(ins):
            cond = self._arm_cond(ins.cc, self.arm_path[ins.addr].mnem)
            if cond == "al":
                a.mov(g, T0, sf)
                return
            if cond is None:
                return
            if isinstance(cond, str):
                self._load(ins, dst, T1, as_size=self._op_size(dst))
                a.csel(g, T0, T1, cond, sf)
                return
        tl = self._skip_lbl()
        done = self._skip_lbl()
        if ins.cc in ("p", "np"):
            self._parity_jcc(ins, tl)
        elif self._producer_ok(ins):
            self._cond_branch(cond, tl)
        else:
            self._ctx_jcc(ins.cc, tl)
        a.b(done)
        a.label(tl)
        a.mov(g, T0, sf)
        a.label(done)

    def translate(self):
        for ins in self.insns:
            if ins.addr in self.leaders:
                self.asm.label(self._lbl(ins.addr))
            try:
                self._emit_one(ins)
                self.stats["insns"] += 1
            except UnhandledInsn as e:
                self.stats["unhandled"].append(f"{ins.mnem}@{ins.addr:x}:{e.why}")
                self.asm.brk(0x100)
            except Exception as e:
                self.stats["unhandled"].append(f"{ins.mnem}@{ins.addr:x}:{e}")
                self.asm.brk(0x100)
        # external-target stubs
        for gva, lbl in self.exit_stubs.items():
            self.asm.label(lbl)
            self.asm.li(ADDR_T, gva)
            self.asm.b("rt_dispatch")
        return self.insns[0].addr


# ---------------------------------------------------------------------------
# runtime + ELF
# ---------------------------------------------------------------------------

def emit_rt(a: Asm, table_va, bare_metal=False, ctx_va=None):
    """rt_dispatch + rt_mkflags. table_va is the host VA of the pc table.

    Exit protocol: guest pc 0 means "leave the translated world".
      bare_metal=False -> linux svc exit(edi)
      bare_metal=True  -> semihosting: print "R=xxxxxxxx" (guest edi) then SYS_EXIT
    """
    a.label("rt_dispatch")
    a.cbnz(ADDR_T, "rt_lookup")
    a.mov(0, GMAP[7], sf=0)          # result = guest edi
    if bare_metal:
        a.bl("rt_print")
        a.movz(0, 0x18, 0, sf=0)     # SYS_EXIT
        a.li(1, ctx_va + 0x38, pad4=True)  # -> {0x20026, status} block
        a.emit(0xD45E0000)           # hlt #0xF000
    else:
        a.movz(8, 93, 0)
        a.svc(0)
    a.label("rt_lookup")
    a.li(LINK, table_va, pad4=True)
    a.label("rt_scan")
    a.ldr_post(0, LINK, 16)
    a.cbz(0, "rt_miss")
    a.cmp_r(0, ADDR_T)
    a.bcond("ne", "rt_scan")
    a.ldur(0, LINK, -8, size=8)
    a.br(0)
    a.label("rt_miss")
    a.brk(1)
    # rt_mkflags: x0=kind (0 add,1 sub,2 logic); NZCV + ctx -> ctx.eflags.
    # clobbers x0-x4,x30; PRESERVES NZCV (saved in x5).
    a.label("rt_mkflags")
    a.mrs_nzcv(5)
    a.mrs_nzcv(1)
    a.ubfx(2, 1, 31, 1)
    a.lsl(2, 2, 7)
    a.ubfx(3, 1, 30, 1)
    a.lsl(3, 3, 6)
    a.orr_r(2, 2, 3)
    a.ubfx(3, 1, 28, 1)
    a.cmp(0, 2, sf=0)
    a.csel(3, 31, 3, "eq", sf=0) # kind==logic -> OF=0
    a.lsl(3, 3, 11)
    a.orr_r(2, 2, 3)
    a.ubfx(3, 1, 29, 1)          # arm C
    a.eor_i(4, 3, 1, sf=0)
    a.cmp(0, 1, sf=0)
    a.csel(3, 4, 3, "eq", sf=0)  # kind==sub -> invert to x86 borrow
    a.cmp(0, 2, sf=0)
    a.csel(3, 31, 3, "eq", sf=0) # kind==logic -> CF=0
    a.orr_r(2, 2, 3)
    a.ldur(3, CTX_REG, CTX_RES, size=8)
    a.eor_shift(3, 3, 3, 1, 4, sf=0)
    a.eor_shift(3, 3, 3, 1, 2, sf=0)
    a.eor_shift(3, 3, 3, 1, 1, sf=0)
    a.and_i(3, 3, 1, sf=0)
    a.eor_i(3, 3, 1, sf=0)
    a.lsl(3, 3, 2)
    a.orr_r(2, 2, 3)
    a.ldur(3, CTX_REG, CTX_A, size=8)
    a.ldur(4, CTX_REG, CTX_B, size=8)
    a.eor_r(3, 3, 4)
    a.ldur(4, CTX_REG, CTX_RES, size=8)
    a.eor_r(3, 3, 4)
    a.and_i(3, 3, 0x10, sf=0)
    a.orr_r(2, 2, 3)
    a.str(2, CTX_REG, CTX_EFLAGS, size=8)
    a.msr_nzcv(5)
    a.ret(LINK)
    if bare_metal:
        # rt_print: w0 -> "R=xxxxxxxx\n\0" at ctx+0x40, SYS_WRITE0 it. clobbers x0-x5.
        a.label("rt_print")
        a.movz(1, 0x3D52, 0, sf=0)            # "R="
        a.str(1, CTX_REG, 0x40, size=2)
        a.str(31, CTX_REG, 0x48, size=8)      # zero digit tail + terminator
        a.movz(4, 28, 0, sf=0)
        a.movz(5, 0, 0, sf=0)
        a.label("rt_phex")
        a.lsrv(3, 0, 4, sf=0)
        a.and_i(3, 3, 0xF, sf=0)
        a.cmp(3, 10, sf=0)
        a.movz(2, ord("0"), 0, sf=0)
        a.movz(1, ord("a") - 10, 0, sf=0)
        a.csel(2, 2, 1, "cc", sf=0)
        a.add_r(3, 3, 2, sf=0)
        # buffer offset 2 + digit index (5)
        a.add_r(1, 5, CTX_REG, sf=1)
        a.str(3, 1, 0x42, size=1)
        a.add(5, 5, 1, sf=0)
        a.subs(4, 4, 4, sf=0)
        a.bcond("ge", "rt_phex")
        a.movz(1, 0x0A, 0, sf=0)
        a.add_r(2, 5, CTX_REG, sf=1)
        a.str(1, 2, 0x42, size=1)
        a.movz(0, 4, 0, sf=0)                 # SYS_WRITE0
        a.add(1, CTX_REG, 0x40)
        a.emit(0xD45E0000)                    # hlt #0xF000
        a.ret(LINK)


def translate_function(code, guest_va):
    """Decode and translate one function's x86-64 bytes. Returns FunctionTranslator."""
    good = []
    for i in disasm(code, guest_va):
        if i.unknown or i.error:
            break
        good.append(i)
    tr = FunctionTranslator(good)
    tr.translate()
    return tr


def _finalize_full(a, rx_base, off):
    """Resolve fixups. Labels are raw word positions; the host VA of a label is
    `rx_base + off + pos` (off = stub/lead-in bytes prepended by the caller)."""
    for idx, kind, lbl in a.fixups:
        if kind in ("tbl_guest", "tbl_host"):
            continue
        tgt = a.labels[lbl]
        here = idx * 4
        if kind == "b26":
            a.words[idx] |= ((tgt - here) >> 2) & 0x03FFFFFF
        elif kind in ("cond19", "cb19"):
            a.words[idx] |= (((tgt - here) >> 2) & 0x7FFFF) << 5
        elif kind == "tb14":
            a.words[idx] |= (((tgt - here) >> 2) & 0x3FFF) << 5
    for idx, kind, lbl in a.fixups:
        if kind == "tbl_guest":
            a.words[idx] = lbl & 0xFFFFFFFF
            a.words[idx + 1] = (lbl >> 32) & 0xFFFFFFFF
        elif kind == "tbl_host":
            va = rx_base + off + a.labels[lbl]
            a.words[idx] = va & 0xFFFFFFFF
            a.words[idx + 1] = (va >> 32) & 0xFFFFFFFF
    return b"".join(struct.pack("<I", w) for w in a.words)


def build_test_elf(code, guest_va, rw_base=0x700000, rx_base=0x400000):
    """Full pipeline: x86-64 function bytes -> runnable aarch64 ELF bytes + translator stats.

    Layout: [stub @ RX_BASE][image @ RX_BASE+0x1000][rt][pc table]
    The stub sets up ctx + guest stack and jumps to the translated entry.
    """
    stub = Asm()
    stub.li(CTX_REG, rw_base)
    stub.li(GMAP[4], rw_base + 0x8000)
    stub.li(GTMP, rx_base + 0x1000)   # entry offset patched after assembly
    stub.br(GTMP)
    stub_b = stub.finalize()
    assert len(stub_b) <= 0x1000

    tr = translate_function(code, guest_va)
    # emit rt once to learn its length so the pc-table VA is known up front
    # (the table-VA li is pad4, so rt length is value-independent)
    probe = Asm()
    emit_rt(probe, 0)
    rt_len = probe.tell()

    a = Asm()
    a.words += tr.asm.words
    for name, pos in tr.asm.labels.items():
        a.labels[name] = pos
    for f in tr.asm.fixups:
        a.fixups.append(f)
    tbl_va = rx_base + 0x1000 + a.tell() + rt_len
    emit_rt(a, tbl_va)
    assert a.tell() == tr.asm.tell() + rt_len
    a.label("pc_table")
    for gva in sorted(tr.leaders):
        a.words += [0, 0, 0, 0]
        a.fixups += [(len(a.words) - 4, "tbl_guest", gva),
                      (len(a.words) - 2, "tbl_host", f"G{gva:x}")]
    a.words += [0, 0, 0, 0]
    img = _finalize_full(a, rx_base, 0x1000)
    assert a.labels["pc_table"] == tbl_va - rx_base - 0x1000

    entry_va = rx_base + 0x1000 + a.labels[f"G{tr.insns[0].addr:x}"]
    # entry stub: sits right after the ELF headers (va = rx_base + its file offset)
    stub = Asm()
    stub.li(CTX_REG, rw_base)
    stub.li(GMAP[4], rw_base + 0x8000)
    stub.li(GTMP, entry_va)
    stub.br(GTMP)
    stub_b = stub.finalize()

    rw = bytearray(max(0x10000, guest_va - rw_base + len(code) + 0x1000))
    rw[guest_va - rw_base:guest_va - rw_base + len(code)] = code

    hdr_len = 64 + 56 * 2
    stub_va = rx_base + hdr_len
    rx_file_sz = 0x1000 + len(img)
    rw_file_off = (rx_file_sz + 0xFFF) & ~0xFFF
    ident = b"\x7fELF" + bytes([2, 1, 1, 0]) + bytes(8)
    ehdr = ident + struct.pack("<HHIQQQIHHHHHH", 2, 0xB7, 1, stub_va, 64, 0, 0,
                               64, 56, 2, 0, 0, 0)
    ph_rx = struct.pack("<IIQQQQQQ", 1, 5, 0, rx_base, rx_base,
                        rx_file_sz, rx_file_sz, 0x1000)
    ph_rw = struct.pack("<IIQQQQQQ", 1, 6, rw_file_off, rw_base, rw_base,
                        len(rw), len(rw), 0x1000)
    out = bytearray()
    out += ehdr + ph_rx + ph_rw
    out += stub_b
    out += bytes(0x1000 - len(out))
    out += img
    out += bytes(rw_file_off - len(out))
    out += rw
    return bytes(out), tr


KERNEL_BASE = 0x40080000          # qemu-system virt -kernel load address
KERNEL_RW = 0x47000000            # scratch RAM: ctx @ +0, guest stack @ +0x8000


def build_test_kernel(code, guest_va, rw_base=KERNEL_RW, rx_base=KERNEL_BASE):
    """x86-64 function bytes -> flat aarch64 kernel image for qemu-system-aarch64.

    Layout: [boot stub][translated code][rt][pc table], all inside the image at
    rx_base. Guest code bytes are NOT mapped (no RIP-relative/self reads in
    tests). Guest ret off an empty stack hits pc 0 -> semihosting exit.
    Returns (image_bytes, translator).
    """
    tr = translate_function(code, guest_va)
    probe = Asm()
    emit_rt(probe, 0, bare_metal=True, ctx_va=0)
    rt_len = probe.tell()

    def stub_bytes(entry_va):
        s = Asm()
        s.li(CTX_REG, rw_base)
        s.li(GMAP[4], rw_base + 0x8000)
        s.str(31, GMAP[4], 0)              # guest stack top: sentinel ret -> pc 0
        s.li(0, 0x20026)
        s.str(0, CTX_REG, 0x38, size=4)    # ADP_Stopped_ApplicationExit
        s.li(GTMP, entry_va, pad4=True)
        s.br(GTMP)
        return s.finalize()

    stub_len = len(stub_bytes(rx_base))    # length fixed: entry li is pad4
    a = Asm()
    a.words += tr.asm.words
    for name, pos in tr.asm.labels.items():
        a.labels[name] = pos
    a.fixups += tr.asm.fixups
    tbl_va = rx_base + stub_len + a.tell() + rt_len
    emit_rt(a, tbl_va, bare_metal=True, ctx_va=rw_base)
    a.label("pc_table")
    for gva in sorted(tr.leaders):
        a.words += [0, 0, 0, 0]
        a.fixups += [(len(a.words) - 4, "tbl_guest", gva),
                      (len(a.words) - 2, "tbl_host", f"G{gva:x}")]
    a.words += [0, 0, 0, 0]
    img = _finalize_full(a, rx_base, stub_len)
    entry_va = rx_base + stub_len + a.labels[f"G{tr.insns[0].addr:x}"]
    return stub_bytes(entry_va) + img, tr


QEMU_BIN = os.environ.get(
    "QEMU_AARCH64",
    "/home/nolan/.nix-profile/bin/qemu-system-aarch64"
    if os.path.exists("/home/nolan/.nix-profile/bin/qemu-system-aarch64")
    else "qemu-system-aarch64")


def run_kernel(code, guest_va=0x46000000, timeout=15):
    """Build + run under qemu-system-aarch64 semihosting.

    Semihosting output lands on stderr with -serial stdio; returns
    (stderr_bytes, translator)."""
    import subprocess
    import tempfile
    img, tr = build_test_kernel(code, guest_va)
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        f.write(img)
        path = f.name
    try:
        p = subprocess.run(
            [QEMU_BIN, "-M", "virt", "-cpu", "cortex-a53", "-kernel", path,
             "-semihosting", "-nographic", "-monitor", "none", "-serial", "stdio"],
            capture_output=True, timeout=timeout)
        return p.stderr + p.stdout, tr
    finally:
        os.unlink(path)


def run_result(code, guest_va=0x46000000):
    """Run and parse 'R=xxxxxxxx' -> int (the guest edi at exit)."""
    out, tr = run_kernel(code, guest_va)
    for line in out.split():
        if line.startswith(b"R="):
            return int(line[2:], 16), tr
    raise RuntimeError(f"no result in output: {out!r} (unhandled: {tr.stats['unhandled']})")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _cmd_disasm(args):
    data = open(args.file, "rb").read()
    for ins in disasm(data, args.base):
        raw = data[ins.addr - args.base:ins.addr - args.base + max(ins.size, 1)]
        print(f"{ins.addr:10x}  {raw.hex():24} {ins}")
        if ins.error or ins.unknown:
            break


def main(argv=None):
    p = argparse.ArgumentParser(description="x86-64 -> arm64 recompiler groundwork")
    p.add_argument("--disasm", dest="file")
    p.add_argument("--base", type=lambda s: int(s, 0), default=0)
    args = p.parse_args(argv)
    if args.file:
        _cmd_disasm(args)
        return 0
    p.print_help()
    return 2


if __name__ == "__main__":
    try:  # a redirected stdout is strict ANSI code page under -I; write '?' for what it cannot encode
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError, OSError):
        pass
    sys.exit(main())
