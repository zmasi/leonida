"""Which CPU instructions an x64 image needs, and which of them a desktop CPU may not run.

Usage:
    python -I tools/cpu_feature_scan.py <image.exe|image.dll> [--sites] [--json]
    python -I tools/cpu_feature_scan.py <dumpbin-listing.txt> [--image <image>] [--base 0x140000000] [--sites] [--json]

The Xbox Series CPU is an AMD Zen 2. The runtime maps the game's code and runs it as-is (docs/design.md, "Why a
loader before a recompiler"), so an instruction the host CPU lacks raises #UD (STATUS_ILLEGAL_INSTRUCTION) where it
is reached, and a privileged one faults. This tool lists those instructions: it streams MSVC
`dumpbin /disasm:nobytes` (as tools/inline_scan.py does), names the CPUID feature or privilege class of each
instruction, and counts them per feature. Each feature has one of these classes:

    amd-only     no Intel CPU has it (SSE4a, CLZERO, MONITORX/MWAITX, RDPRU, XOP, FMA4, TBM): #UD on every
                 Intel host
    newer-intel  Zen 2 has it, but Intel CPUs only from a later generation on (CLWB, SHA, RDPID): #UD on older
                 Intel hosts, such as the client Skylake to Comet Lake parts
    not-zen2     the console CPU does not have it (AVX-512, VAES, VPCLMULQDQ, WAITPKG, ...): a hit is most likely
                 data that the linear disassembly read as code
    privileged   ring 0 only (port I/O, CLI/HLT, MSRs, control registers, XSETBV, MONITOR/MWAIT, ...): faults in user
                 mode on every PC
    kernel-call  SYSCALL, SYSENTER and INT n other than the debug/assert/fast-fail vectors: a direct call into the
                 console kernel
    host-query   CPUID, XGETBV, RDTSC(P), RDPID: run on every host but answer with the host's values
    baseline     a feature every x86-64 PC the runtime targets has (AVX2, FMA, BMI, AES, ...): counted, not flagged

The image's `.pdata` table (read from the PE, or from `--image` for a listing) splits each count into hits inside
a function's range and hits elsewhere. A linear disassembly also decodes padding, jump tables and other data in the
code sections, so a hit outside every `.pdata` range is often not code; but a leaf function that does not touch the
stack has no `.pdata` entry either, so read those sites by hand (`--sites` prints their RVAs).

Nothing is executed. The output is counts, feature names and RVAs, no code bytes. An input with no instructions in
it exits 2 instead of reporting zero hits. Standard library only.
"""
from __future__ import annotations

import argparse
import bisect
import json
import os
import re
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inline_scan  # noqa: E402
from pe_inspect import DIR_EXCEPTION, PeFormatError, PeImage  # noqa: E402

FLAGGED = ("amd-only", "newer-intel", "not-zen2", "privileged", "kernel-call")
CLASSES = FLAGGED + ("host-query", "baseline")

# feature -> (class, what a host without it does)
FEATURES = {
    "SSE4A": ("amd-only", "#UD on every Intel CPU"),
    "CLZERO": ("amd-only", "#UD on every Intel CPU"),
    "MONITORX": ("amd-only", "#UD on every Intel CPU"),
    "RDPRU": ("amd-only", "#UD on every Intel CPU; Zen 2 and later"),
    "XOP": ("amd-only", "#UD on every Intel CPU and on Zen"),
    "FMA4": ("amd-only", "#UD on every Intel CPU; not reported by Zen"),
    "TBM": ("amd-only", "#UD on every Intel CPU and on Zen"),
    "3DNOW": ("amd-only", "#UD on every Intel CPU and on Zen"),
    "CLWB": ("newer-intel", "#UD on Intel client CPUs before Ice Lake / Tiger Lake"),
    "SHA": ("newer-intel", "#UD on Intel client CPUs before Ice Lake (Goldmont Atoms have it)"),
    "RDPID": ("newer-intel", "#UD on Intel CPUs before Ice Lake"),
    "AVX512": ("not-zen2", "not on the console CPU"),
    "VAES": ("not-zen2", "not on the console CPU (Zen 3 and later)"),
    "VPCLMULQDQ": ("not-zen2", "not on the console CPU (Zen 3 and later)"),
    "WAITPKG": ("not-zen2", "Intel only"),
    "MOVDIR": ("not-zen2", "Intel only"),
    "SERIALIZE": ("not-zen2", "Intel only"),
    "RTM": ("not-zen2", "Intel only"),
    "GFNI": ("not-zen2", "not on the console CPU"),
    "PKU": ("not-zen2", "not on the console CPU (Zen 3 and later)"),
    "PORT-IO": ("privileged", "faults in user mode"),
    "SYSTEM": ("privileged", "faults in user mode"),
    "MONITOR": ("privileged", "faults in user mode on Windows"),
    "XSETBV": ("privileged", "faults in user mode"),
    "SYSCALL": ("kernel-call", "enters the host kernel with a console service number"),
    "INT": ("kernel-call", "a software interrupt Windows does not serve for the title"),
    "CPUID": ("host-query", "answers with the host CPU's vendor, family and features"),
    "XGETBV": ("host-query", "answers with the host OS's enabled state"),
    "RDTSC": ("host-query", "counts at the host's TSC rate"),
    "AVX": ("baseline", ""), "AVX2": ("baseline", ""), "FMA": ("baseline", ""), "F16C": ("baseline", ""),
    "BMI1": ("baseline", ""), "BMI2": ("baseline", ""), "LZCNT": ("baseline", ""), "POPCNT": ("baseline", ""),
    "ADX": ("baseline", ""), "MOVBE": ("baseline", ""), "AES": ("baseline", ""), "PCLMULQDQ": ("baseline", ""),
    "RDRAND": ("baseline", ""), "RDSEED": ("baseline", ""), "CLFLUSHOPT": ("baseline", ""),
    "XSAVE": ("baseline", ""), "FSGSBASE": ("baseline", ""), "CX16": ("baseline", ""),
    "PREFETCHW": ("baseline", ""), "SSE4.2": ("baseline", ""),
}

# Exact mnemonics (dumpbin spelling, lower case) -> feature.
MNEMONICS: dict[str, str] = {}


def _names(feature: str, *names: str):
    for name in names:
        MNEMONICS[name] = feature


_names("SSE4A", "extrq", "insertq", "movntsd", "movntss")
_names("CLZERO", "clzero")
_names("MONITORX", "monitorx", "mwaitx")
_names("RDPRU", "rdpru")
_names("XOP", "vpperm", "vpcmov", "vprotb", "vprotw", "vprotd", "vprotq", "vpshab", "vpshaw", "vpshad", "vpshaq",
       "vpshlb", "vpshlw", "vpshld", "vpshlq", "vfrczps", "vfrczpd", "vfrczss", "vfrczsd", "vpermil2ps", "vpermil2pd",
       "vphaddbw", "vphaddbd", "vphaddbq", "vphaddwd", "vphaddwq", "vphadddq", "vphaddubw", "vphaddubd",
       "vphaddubq", "vphadduwd", "vphadduwq", "vphaddudq", "vphsubbw", "vphsubwd", "vphsubdq", "vpmacssww",
       "vpmacsww", "vpmacsswd", "vpmacswd", "vpmacssdd", "vpmacsdd", "vpmacssdql", "vpmacsdql", "vpmacssdqh",
       "vpmacsdqh", "vpmadcsswd", "vpmadcswd", "vpcomb", "vpcomw", "vpcomd", "vpcomq", "vpcomub", "vpcomuw",
       "vpcomud", "vpcomuq")
_names("FMA4", *(f"{op}{t}" for op in ("vfmadd", "vfmsub", "vfnmadd", "vfnmsub") for t in ("ps", "pd", "ss", "sd")),
       "vfmaddsubps", "vfmaddsubpd", "vfmsubaddps", "vfmsubaddpd")
_names("TBM", "blcfill", "blci", "blcic", "blcmsk", "blcs", "blsfill", "blsic", "t1mskc", "tzmsk")
_names("3DNOW", "femms", "pavgusb", "pf2id", "pfacc", "pfadd", "pfcmpeq", "pfcmpge", "pfcmpgt", "pfmax", "pfmin",
       "pfmul", "pfrcp", "pfrcpit1", "pfrcpit2", "pfrsqit1", "pfrsqrt", "pfsub", "pfsubr", "pi2fd", "pmulhrw",
       "pf2iw", "pfnacc", "pfpnacc", "pi2fw", "pswapd")
_names("CLWB", "clwb")
_names("SHA", "sha1rnds4", "sha1nexte", "sha1msg1", "sha1msg2", "sha256rnds2", "sha256msg1", "sha256msg2")
_names("RDPID", "rdpid")
_names("WAITPKG", "umonitor", "umwait", "tpause")
_names("MOVDIR", "movdiri", "movdir64b", "enqcmd", "enqcmds")
_names("SERIALIZE", "serialize")
_names("RTM", "xbegin", "xend", "xabort", "xtest")
_names("GFNI", "gf2p8affineqb", "gf2p8affineinvqb", "gf2p8mulb", "vgf2p8affineqb", "vgf2p8affineinvqb",
       "vgf2p8mulb")
_names("PKU", "rdpkru", "wrpkru")
_names("AVX512", "kmovb", "kmovw", "kmovd", "kmovq", "kandw", "korw", "kxorw", "knotw", "kortestw", "kshiftlw",
       "kshiftrw", "vpternlogd", "vpternlogq", "vpcompressd", "vpcompressq", "vpexpandd", "vpexpandq",
       "vpermi2d", "vpermi2q", "vpermi2ps", "vpermi2pd", "vpermt2d", "vpermt2q", "vpermt2ps", "vpermt2pd")
_names("PORT-IO", "in", "out", "ins", "insb", "insw", "insd", "outs", "outsb", "outsw", "outsd")
_names("SYSTEM", "cli", "sti", "hlt", "rdmsr", "wrmsr", "rdpmc", "invd", "wbinvd", "wbnoinvd", "invlpg", "invpcid",
       "lgdt", "lidt", "lldt", "ltr", "clts", "swapgs", "sysret", "sysretq", "sysexit", "lmsw",
       "invlpga", "vmrun", "vmload", "vmsave", "stgi", "clgi", "skinit", "vmcall", "vmmcall", "rsm")
_names("MONITOR", "monitor", "mwait")
_names("XSETBV", "xsetbv")
_names("SYSCALL", "syscall", "sysenter")
_names("AVX", "vzeroupper", "vzeroall")
_names("CPUID", "cpuid")
_names("XGETBV", "xgetbv")
_names("RDTSC", "rdtsc", "rdtscp")
_names("F16C", "vcvtph2ps", "vcvtps2ph")
_names("BMI1", "andn", "bextr", "blsi", "blsmsk", "blsr", "tzcnt")
_names("BMI2", "bzhi", "mulx", "pdep", "pext", "rorx", "sarx", "shlx", "shrx")
_names("LZCNT", "lzcnt")
_names("POPCNT", "popcnt")
_names("ADX", "adcx", "adox")
_names("MOVBE", "movbe")
_names("RDRAND", "rdrand")
_names("RDSEED", "rdseed")
_names("CLFLUSHOPT", "clflushopt")
_names("XSAVE", "xsave", "xsave64", "xsaveopt", "xsaveopt64", "xsavec", "xsavec64", "xsaves", "xsaves64",
       "xrstor", "xrstor64", "xrstors", "xrstors64")
_names("FSGSBASE", "rdfsbase", "rdgsbase", "wrfsbase", "wrgsbase")
_names("CX16", "cmpxchg16b")
_names("PREFETCHW", "prefetchw")
_names("SSE4.2", "crc32", "pcmpestri", "pcmpestrm", "pcmpistri", "pcmpistrm", "vpcmpestri", "vpcmpestrm",
       "vpcmpistri", "vpcmpistrm")
_names("AVX2", "vpbroadcastb", "vpbroadcastw", "vpbroadcastd", "vpbroadcastq", "vbroadcasti128", "vextracti128",
       "vinserti128", "vperm2i128", "vpermd", "vpermq", "vpermps", "vpermpd", "vpmaskmovd", "vpmaskmovq",
       "vpsllvd", "vpsllvq", "vpsrlvd", "vpsrlvq", "vpsravd", "vpgatherdd", "vpgatherdq", "vpgatherqd",
       "vpgatherqq", "vgatherdps", "vgatherdpd", "vgatherqps", "vgatherqpd", "vpblendd")

PREFIXES = {"lock", "rep", "repe", "repz", "repne", "repnz", "xacquire", "xrelease", "bnd", "notrack"}
ALLOWED_INT = {3, 0x29, 0x2C, 0x2D}  # breakpoint, __fastfail, __assert, debug service: Windows serves them
FMA3 = re.compile(r"^vf(?:n?m(?:add|sub)|maddsub|msubadd)(?:132|213|231)(?:ps|pd|ss|sd)$")
AES = {"aesenc", "aesenclast", "aesdec", "aesdeclast", "aesimc", "aeskeygenassist"}
CONTROL_REG = re.compile(r"\b[cd]r\d+\b", re.IGNORECASE)
IMMEDIATE = re.compile(r"^(?:0[xX]([0-9A-Fa-f]+)|([0-9A-Fa-f]+)[hH]?)$")
ZMM = re.compile(r"\b(?:zmm\d+|[xy]mm(?:1[6-9]|2\d|3[01]))\b|\{k\d\}", re.IGNORECASE)
YMM = re.compile(r"\bymm\d+\b", re.IGNORECASE)


def classify(mnem: str, ops: str) -> str | None:
    """The feature (a FEATURES key) one dumpbin instruction needs, or None for the plain x86-64 baseline."""
    mnem = mnem.strip().lower()
    ops = ops.strip()
    while mnem in PREFIXES and ops:
        parts = ops.split(None, 1)
        mnem = parts[0].lower()
        ops = parts[1].strip() if len(parts) > 1 else ""
    if mnem == "int":
        m = IMMEDIATE.match(ops)
        val = int(m.group(1) or m.group(2), 16) if m else None
        return None if val in ALLOWED_INT else "INT"
    if mnem == "int1" or mnem == "into":
        return "INT"
    if mnem == "mov" and CONTROL_REG.search(ops):
        return "SYSTEM"
    if ZMM.search(ops):
        return "AVX512"
    base = mnem[1:] if mnem.startswith("v") else mnem
    if base in AES:
        return "VAES" if mnem.startswith("v") and YMM.search(ops) else "AES"
    if base == "pclmulqdq":
        return "VPCLMULQDQ" if mnem.startswith("v") and YMM.search(ops) else "PCLMULQDQ"
    if mnem in MNEMONICS:
        return MNEMONICS[mnem]
    if FMA3.match(mnem):
        return "FMA"
    if mnem.startswith("vp") and YMM.search(ops):
        return "AVX2"  # a 256-bit integer operation
    if mnem.startswith("v") and (YMM.search(ops) or "xmm" in ops.lower()):
        return "AVX"
    return None


class Functions:
    """The image's .pdata ranges, as sorted, merged [begin, end) RVA pairs."""

    def __init__(self, ranges):
        merged = []
        for begin, end in sorted(r for r in ranges if r[1] > r[0]):
            if merged and begin <= merged[-1][1]:
                merged[-1][1] = max(merged[-1][1], end)
            else:
                merged.append([begin, end])
        self.begins = [b for b, _ in merged]
        self.ends = [e for _, e in merged]
        self.count = len(ranges)

    @classmethod
    def from_image(cls, img: PeImage) -> "Functions":
        rva, size = img.directory(DIR_EXCEPTION)
        at = img.offset(rva, size) if rva and size else None
        if at is None:
            return cls([])
        return cls([struct.unpack_from("<II", img.data, at + i)
                    for i in range(0, size - size % 12, 12)])

    def contains(self, rva: int) -> bool:
        i = bisect.bisect_right(self.begins, rva) - 1
        return i >= 0 and rva < self.ends[i]


def scan(lines, functions: Functions | None, base: int | None, stats: dict | None = None):
    """Yield (feature, rva, in_function) for every instruction that needs a feature. `stats` gets the counts."""
    stats = {} if stats is None else stats
    stats.update(instructions=0, in_functions=0)
    for addr, mnem, ops in inline_scan.instructions(lines):
        if base is None:
            base = addr & ~0xFFFFFFF
        rva = addr - base
        inside = functions.contains(rva) if functions else None
        stats["instructions"] += 1
        stats["in_functions"] += bool(inside)
        feature = classify(mnem, ops)
        if feature:
            yield feature, rva, inside


def summarize(hits, stats: dict, functions: Functions | None) -> dict:
    counts = defaultdict(Counter)
    sites = defaultdict(list)
    for feature, rva, inside in hits:
        where = "unknown" if inside is None else "in_functions" if inside else "elsewhere"
        counts[feature][where] += 1
        if FEATURES[feature][0] in FLAGGED:
            sites[feature].append({"rva": rva, "in_function": inside})
    report = {"instructions": stats["instructions"],
              "pdata_entries": functions.count if functions else None,
              "instructions_in_functions": stats["in_functions"] if functions else None,
              "features": {}}
    order = {c: i for i, c in enumerate(CLASSES)}
    for feature in sorted(counts, key=lambda f: (order[FEATURES[f][0]], f)):
        cls, note = FEATURES[feature]
        c = counts[feature]
        report["features"][feature] = {"class": cls, "note": note, "total": sum(c.values()),
                                       "in_functions": c["in_functions"], "elsewhere": c["elsewhere"],
                                       "sites": sites.get(feature, [])}
    return report


def to_text(report: dict, show_sites: bool) -> str:
    out = [f"instructions scanned: {report['instructions']}"]
    if report["pdata_entries"] is not None:
        out.append(f".pdata entries: {report['pdata_entries']}, instructions inside them: "
                   f"{report['instructions_in_functions']}")
    else:
        out.append("no .pdata read (pass --image for a listing): hits are not split by function")
    flagged = [f for f, r in report["features"].items() if r["class"] in FLAGGED]
    out.append(f"flagged features: {len(flagged)}" + (f" ({', '.join(flagged)})" if flagged else ""))
    out.append(f"{'feature':<12} {'class':<12} {'total':>9} {'in .pdata':>9} {'elsewhere':>9}  note")
    for feature, r in report["features"].items():
        known = report["pdata_entries"] is not None
        inside = r["in_functions"] if known else "-"
        elsewhere = r["elsewhere"] if known else "-"
        out.append(f"{feature:<12} {r['class']:<12} {r['total']:>9} {inside:>9} {elsewhere:>9}  {r['note']}")
    if show_sites:
        for feature, r in report["features"].items():
            for s in r["sites"]:
                where = {True: "in .pdata", False: "outside .pdata", None: "?"}[s["in_function"]]
                out.append(f"  {feature} +0x{s['rva']:x} {where}")
    return "\n".join(out)


def _parse_hex_base(value: str) -> int:
    try:
        base = int(value, 16)
    except ValueError:
        raise argparse.ArgumentTypeError(f"invalid hex base: {value!r}") from None
    if base < 0:
        raise argparse.ArgumentTypeError(f"base must be non-negative: {value!r}")
    return base


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("source", type=Path, help="an x64 PE (disassembled with dumpbin) or a saved dumpbin listing")
    p.add_argument("--image", type=Path, help="for a listing: the PE it came from, to read .pdata and the base")
    p.add_argument("--base", type=_parse_hex_base, help="image base (default: the PE's, else inferred)")
    p.add_argument("--sites", action="store_true", help="print the RVA of every flagged instruction")
    p.add_argument("--json", action="store_true", help="print the report as JSON")
    args = p.parse_args(argv)
    if not args.source.is_file():
        print(f"{args.source} not found", file=sys.stderr)
        return 2
    is_image = args.source.suffix.lower() in (".exe", ".dll")
    image_path = args.source if is_image else args.image
    functions, base = None, args.base
    if image_path is not None:
        try:
            img = PeImage(image_path.read_bytes())
        except (OSError, PeFormatError) as e:
            print(f"{image_path}: {e}", file=sys.stderr)
            return 2
        functions = Functions.from_image(img)
        if base is None:
            base = img.image_base
    stats = {}
    try:
        report = summarize(scan(inline_scan.read_lines(args.source), functions, base, stats), stats, functions)
    except OSError as e:
        print(f"{args.source}: {e}", file=sys.stderr)
        return 2
    if not stats["instructions"]:
        print(f"{args.source}: no instructions found (not a dumpbin listing, or an image without code)",
              file=sys.stderr)
        return 2
    print(json.dumps(report, indent=1) if args.json else to_text(report, args.sites))
    return 0


if __name__ == "__main__":
    try:  # a redirected stdout is strict ANSI code page under -I; write '?' for what it cannot encode
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError, OSError):
        pass
    sys.exit(main())
