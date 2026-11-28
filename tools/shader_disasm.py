"""Disassemble the native RDNA2 code in unpacked FSSM shader blobs with LLVM's AMDGPU disassembler.

FSSM header (little-endian qwords at 0x00..): 'FSSM' 01 01, blob size, ..., then a table where the
code range appears as (offset, size) = qword[6], qword[7] in every blob checked so far (e.g. 0x2f8, 0x100).
The code ends with s_endpgm (0xBF810000). If the header guess doesn't hold for a blob, the tool falls back to
scanning back from the first s_endpgm to the start that decodes with no invalid instructions.

Usage:
    python -I shader_disasm.py <fssm dir or file> [--unpack-index FILE] [--out DIR] [--mcpu gfx1013] [--limit N] [--skip-existing]
Writes <out>/<name>.s and prints a summary (decoded cleanly / with invalid words / failed).
Set LLVM_MC to llvm-mc's path, or to a whole command such as
'wsl -d Ubuntu -e /usr/lib/llvm-21/bin/llvm-mc' when the tool is not on PATH. Off Windows, a versioned install
(llvm-mc-21, /usr/lib/llvm-21/bin/llvm-mc) is found without it (tools/mc_command.py).
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mc_command import install_hint, mc_command, not_found  # noqa: E402
from report_paths import path_key, same_report_path  # noqa: E402
from shader_stage_manifest import shader_stems  # noqa: E402
import fssm_header  # noqa: E402
import rdna2_decode  # noqa: E402

TRIPLE = "amdgcn--amdpal"
ENDPGM = b"\x00\x00\x81\xbf"
DOT4_I8_OPCODE = 0x16         # VOP3P v_dot4_i32_i8
PROBE = "0x00 0x00 0x81 0xbf"  # s_endpgm: the one instruction every shader ends with
FAILED = []                    # (path, first stderr line) for shaders that produced no disassembly


MC = mc_command()


def fail(msg: str) -> "None":
    print(f"ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def output_path(path: str, out: str) -> str:
    return os.path.join(out, os.path.splitext(os.path.basename(path))[0] + ".s")


def validate_output(out: str, files: list) -> None:
    """Refuse a planned <out>/<stem>.s that is a folder or names an input, before probing llvm-mc or creating --out.
    A single `foo.s` given with --out at its own folder (or the default ../disasm of a file already in disasm/) was
    replaced by its own listing; on Windows and macOS `FOO.S` is the same file as the `FOO.s` written for it."""
    inputs = {path_key(p): p for p in files}
    for src in files:
        dst = output_path(src, out)
        if os.path.isdir(dst):
            fail(f"cannot write {dst}: destination is a folder, not a file")
        hit = inputs.get(path_key(dst))
        if hit is None and same_report_path(src, dst):  # a hard link or another spelling the name rules missed
            hit = src
        if hit is not None:
            fail(f"output {dst} would overwrite input {hit}")


def positive_int(text: str) -> int:
    """--jobs 0 made ThreadPoolExecutor raise, and --limit -1 silently dropped the last file."""
    try:
        value = int(text)
    except ValueError:
        raise argparse.ArgumentTypeError(f"invalid int value: {text!r}") from None
    if value < 1:
        raise argparse.ArgumentTypeError("must be at least 1")
    return value


def probe(mcpu: str) -> None:
    """Disassemble one s_endpgm first, so a missing tool or AMDGPU target fails now and not per shader."""
    try:
        p = subprocess.run(MC + ["--disassemble", f"-triple={TRIPLE}", f"-mcpu={mcpu}"],
                           input=PROBE, capture_output=True, text=True, errors="replace",
                           creationflags=getattr(subprocess, "BELOW_NORMAL_PRIORITY_CLASS", 0))
    except OSError as e:
        fail(not_found(MC[0], e))
    if p.returncode != 0 or "s_endpgm" not in p.stdout:
        why = (p.stderr.strip().splitlines() or [f"exit code {p.returncode}"])[0]
        fail(f"'{' '.join(MC)}' did not disassemble an s_endpgm: {why}\n"
             f"The disassembler needs LLVM 21+ with the AMDGPU target (docs/design.md, 'Disassembling shaders').\n"
             f"{install_hint()}")


def disasm(code: bytes, mcpu: str, backend: str = "llvm-mc", wave32: bool = True, compute: bool = False):
    """`compute`: a pipeline-library or FSPX compute program. llvm-mc 21 does not decode SMEM loads whose SOFFSET is
    SGPR_NULL (0x7D, e.g. s_load_dwordx8 ... 0xd60 with soffset null): it reports the first dword invalid, then reads
    the second (0xFA......) as an 8-byte EXP and the rest of the listing goes out of step (gfx1013 and gfx1030 alike).
    In 749 of the 2,684 library programs of 1.0.2.1 that refused 945 programs ('fall-through into undecodable
    words'). tools/rdna2_decode.py decodes them; such a program takes its listing when it has fewer invalid words."""
    if backend == "auto":
        asm, bad, err = disasm(code, mcpu, backend="llvm-mc", wave32=wave32)
        if bad == 0 and asm.strip():
            return asm, bad, err
        py_asm, py_bad, _ = disasm(code, mcpu, backend="python", wave32=wave32)
        if py_asm.strip() and (not asm.strip() or py_bad < bad):
            return py_asm, py_bad, ""
        return asm, bad, err
    if compute and backend != "python":
        asm, bad, err = disasm(code, mcpu, backend=backend, wave32=wave32)
        if bad:
            py_asm, py_bad = rdna2_decode.disasm_bytes(code, mcpu=mcpu, wave32=wave32)
            if py_bad < bad:
                return py_asm, py_bad, ""
        return asm, bad, err
    if backend == "python":
        asm, bad = rdna2_decode.disasm_bytes(code, mcpu=mcpu, wave32=wave32)
        return asm, bad, ""
    asm, bad, err = run_mc(code, mcpu, wave32)
    if bad and asm.strip():
        fixed = with_vopc_dpp(code, mcpu, err, wave32)
        if fixed:
            return fixed
        fixed = with_dot4_op_sel(code, mcpu, bad, err, wave32)
        if fixed:
            return fixed
    return asm, bad, err


def run_mc(code: bytes, mcpu: str, wave32: bool = True):
    """One llvm-mc pass over `code`: (listing, invalid word count, stderr), or ("", count, reason) when it failed."""
    # llvm-mc echoes the entire input line for each bad word. Newlines are whitespace, even inside
    # a multiword instruction. Bound each diagnostic to one dword instead of the whole shader.
    text = "\n".join(" ".join(f"0x{b:02x}" for b in code[i:i + 4]) for i in range(0, len(code), 4))
    try:
        wave_flags = [] if wave32 else ["-mattr=-wavefrontsize32,+wavefrontsize64"]
        p = subprocess.run(MC + ["--disassemble", f"-triple={TRIPLE}", f"-mcpu={mcpu}", *wave_flags, "-show-encoding"],
                           input=text, capture_output=True, text=True, errors="replace",
                           creationflags=getattr(subprocess, "BELOW_NORMAL_PRIORITY_CLASS", 0))
    except OSError as e:
        return "", 0, str(e)
    bad = p.stderr.count("invalid instruction encoding")
    if p.returncode != 0:
        return "", bad, p.stderr.strip() or f"llvm-mc exited with code {p.returncode}"
    return p.stdout, bad, p.stderr


ENCODING = re.compile(r"; encoding: \[([^\]]*)\]")
INVALID_AT = re.compile(r":(\d+):(\d+): warning: invalid instruction encoding")
VOPC = 0x3E                    # bits 31..25 of a VOPC word
DPP_SRC0 = (250, 233, 234)     # the src0 field of a DPP16, DPP8 and DPP8-with-fi word
STAND_IN = 0x10                # VOP2 v_max_f32: takes the same DPP second word, and llvm-mc decodes it
STAND_IN_TEXT = "v_max_f32_dpp v0, "


def invalid_offsets(err: str) -> set:
    """Byte offsets llvm-mc rejected: its warning names the line and column of the word in the input of 5-character
    "0xNN " tokens it was given, one line per dword (run_mc); a listing made from one line of the whole code has
    every warning on line 1."""
    return {(int(line) - 1) * 4 + (int(col) - 1) // 5 for line, col in INVALID_AT.findall(err)}


def vopc_dpp_words(code: bytes, offsets) -> dict:
    """{offset: (compare name, word0, word1)} of the rejected words at `offsets` that are a VOPC compare whose src0
    field is a DPP marker. The name is the decoder's (rdna2_decode's VOPC table)."""
    found = {}
    for at in sorted(offsets):
        if at % 4 or at + 8 > len(code):
            continue
        w0, w1 = struct.unpack_from("<2I", code, at)
        entry = rdna2_decode.T.VOPC.get((w0 >> 17) & 0xFF)
        if w0 >> 25 == VOPC and w0 & 0x1FF in DPP_SRC0 and entry:
            found[at] = (entry[0], w0, w1)
    return found


def with_vopc_dpp(code: bytes, mcpu: str, err: str, wave32: bool = True):
    """The listing of `code` with each rejected VOPC compare in DPP form printed as the instruction it is, or None
    when there is none (docs/design.md, "DPP on a VOPC compare"). llvm-mc has no decoding for the pair on gfx10, so
    it is decoded with the first word turned into a VOP2 that takes the same DPP second word (v_max_f32 into v0),
    which gives llvm-mc's own text for the two sources and the DPP controls; each such line then gets the compare's
    name, its VCC destination and the real encoding back, so the translator reads the instruction and checks its
    encoding against the blob. A word that does not come out as that stand-in at an instruction boundary was not an
    instruction (it sits in a literal or in data): it is left as it was and the pass runs again without it."""
    words = vopc_dpp_words(code, invalid_offsets(err))
    while words:
        patched = bytearray(code)
        for at, (_, w0, _) in words.items():
            struct.pack_into("<I", patched, at, STAND_IN << 25 | (w0 & 0x1FFFF))
        asm, bad, err2 = run_mc(bytes(patched), mcpu, wave32)
        if not asm.strip():
            return None
        invalid, pos, hit, lines = invalid_offsets(err2), 0, set(), []
        for line in asm.split("\n"):
            enc = ENCODING.search(line)
            if not enc:
                lines.append(line)
                continue
            while pos in invalid:
                pos += 4
            text = line[:enc.start()].strip()
            if pos in words and text.startswith(STAND_IN_TEXT):
                hit.add(pos)
                name, w0, w1 = words[pos]
                operands = text[len(STAND_IN_TEXT):]
                # v_cmpx writes EXEC and prints no destination; a compare prints VCC as llvm-mc does here, vcc_lo.
                text = f"{name}_dpp {'' if name.startswith('v_cmpx_') else 'vcc_lo, '}{operands}"
                original = ",".join(f"0x{b:02x}" for b in struct.pack("<2I", w0, w1))
                line = f"\t{text:<40}; encoding: [{original}]"
            lines.append(line)
            pos += len(enc.group(1).split(","))
        if hit == set(words):
            return "\n".join(lines), bad, err2
        words = {at: w for at, w in words.items() if at in hit}
    return None


def dot4_op_sel_words(code: bytes, offsets) -> dict:
    """{offset: (word0, word1)} of the dword pairs at `offsets` that are v_dot4_i32_i8 with op_sel 2 and op_sel_hi 5,
    no clamp and no neg: the form 89 words of the FidelityFX upscaler's programs take (docs/fspx.md). llvm-mc has no
    decoding for it, and a plain v_dot4_i32_i8 has op_sel 0 and op_sel_hi 7. Only this combination is recognised:
    its result was measured on hardware (tests/semantic/test_alu_dot4_i8.py), the other op_sel values were not."""
    found = {}
    for at in sorted(offsets):
        if at % 4 or at + 8 > len(code):
            continue
        w0, w1 = struct.unpack_from("<2I", code, at)
        if (w0 >> 23 == 0x198 and (w0 >> 16) & 0x7F == DOT4_I8_OPCODE and (w0 >> 8) & 7 == 0     # VOP3P, neg_hi
                and (w0 >> 11) & 7 == 2 and (w0 >> 14) & 1 == 1 and (w0 >> 15) & 1 == 0          # op_sel, hi[2], clamp
                and (w1 >> 27) & 3 == 1 and (w1 >> 29) & 7 == 0):                                # op_sel_hi[1:0], neg_lo
            found[at] = (w0, w1)
    return found


def with_dot4_op_sel(code: bytes, mcpu: str, bad: int, err: str, wave32: bool = True):
    """The listing of `code` with each rejected v_dot4_i32_i8 op_sel 2 / op_sel_hi 5 word printed as the instruction
    it is, or None when there is none. llvm-mc rejects the pair, so it is decoded with the two fields at their
    defaults (op_sel 0, op_sel_hi 7), and each such line gets the real fields and the real encoding back: the
    translator then reads the instruction and checks its encoding against the blob. A word that is not printed as
    v_dot4_i32_i8 at an instruction boundary was not an instruction (it sits in a literal or in data), so it is
    left as it was and the pass runs again without it."""
    words = dot4_op_sel_words(code, invalid_offsets(err))
    while words and bad:
        patched = bytearray(code)
        for at, (w0, w1) in words.items():
            struct.pack_into("<2I", patched, at, w0 & ~(7 << 11), w1 | (2 << 27))
        asm, bad2, err2 = run_mc(bytes(patched), mcpu, wave32)
        if not asm.strip():
            return None
        invalid, pos, hit, lines = invalid_offsets(err2), 0, set(), []
        for line in asm.split("\n"):
            enc = ENCODING.search(line)
            if not enc:
                lines.append(line)
                continue
            while pos in invalid:
                pos += 4
            if pos in words and line.lstrip("\t").startswith("v_dot4_i32_i8 "):
                hit.add(pos)
                w0, w1 = words[pos]
                original = ",".join(f"0x{b:02x}" for b in struct.pack("<2I", w0, w1))
                line = line[:enc.start()].rstrip() + " op_sel:[0,1,0] op_sel_hi:[1,0,1]"
                line = f"{line:<40}; encoding: [{original}]"
            lines.append(line)
            pos += len(enc.group(1).split(","))
        if hit == set(words):
            return "\n".join(lines), bad2, err2
        words = {at: w for at, w in words.items() if at in hit}
    return None


MAX_DATA_DWORDS = 1 << 16


def data_block_start(b: bytes, start: int, end: int) -> int:
    """Where a constant-data block begins at the end of the code range [start, end), or `end` if there is none.

    Some blobs carry, after the last real s_endpgm, a block laid out as: s_endpgm, a dword count N, N dwords
    of data, then one more s_endpgm (the one the range ends on) and s_code_end padding. The data is not code:
    llvm-mc reports its words as invalid (and reads a 0xBF810000 inside it as a second program end). The
    block is recognised only when the count N matches the distance to the closing s_endpgm and the dword
    before the count is itself an s_endpgm, so ordinary code is not cut."""
    last = end - 4
    for n in range(min(MAX_DATA_DWORDS, (last - start) // 4 - 1) + 1):
        count = last - 4 * (n + 1)
        if count - 4 < start:
            break
        if struct.unpack_from("<I", b, count)[0] == n and b[count - 4:count] == ENDPGM:
            return count
    return end


def code_range(b: bytes):
    """The code section offset is the u32 at 0xC0; instructions start 0x28 bytes into it and run to the last
    4-byte-aligned s_endpgm in the blob (some blobs hold several programs), minus a trailing constant-data
    block (see data_block_start). Found by correlating against clean decodes of 154 random shaders."""
    # A blob too short to hold the offset goes to the scan like any other header miss; reading it anyway raised
    # struct.error, which stopped the whole run (and make_game.py at the same blob on every rerun).
    off = struct.unpack_from("<I", b, 0xC0)[0] if len(b) >= 0xC4 else 0
    start = off + 0x28
    if 0 < off and start < len(b):
        last = b.rfind(ENDPGM, start)
        while last >= 0 and (last - start) % 4:
            last = b.rfind(ENDPGM, start, last)
        if last >= 0:
            return start, data_block_start(b, start, last + 4), "header"
    end = b.find(ENDPGM)
    # Fallback instructions are dword-aligned in the blob. A matching byte sequence can straddle metadata words.
    while end >= 0 and end % 4:
        end = b.find(ENDPGM, end + 1)
    return None, end + 4 if end >= 0 else None, "scan"


def one(path, out, mcpu, backend="llvm-mc"):
    with open(path, "rb") as f:
        b = f.read()
    wave32 = fssm_header.wave_size(b) != 64
    start, end, how = code_range(b)
    if end is None:
        return "no s_endpgm"
    if start is None:
        # Walk back in 4-byte steps from the end and keep the longest clean decode.
        start, best = end - 4, None
        for s in range(end - 4, max(0x100, end - 0x4000) - 1, -4):
            _, bad, _ = disasm(b[s:end], mcpu, backend=backend, wave32=wave32)
            if bad == 0:
                best = s
        if best is None:
            return "scan failed"
        start = best
    asm, bad, err = disasm(b[start:end], mcpu, backend=backend, wave32=wave32)
    if not asm.strip():  # a broken toolchain, not a shader: never report this as "clean"
        reason = (err.strip().splitlines() or ["no output"])[0]
        FAILED.append((path, reason))
        return "failed"
    name = os.path.splitext(os.path.basename(path))[0]
    # Only a published listing can be reused after an interruption.
    listing = tempfile.NamedTemporaryFile(mode="w", dir=out, prefix=".disasm-", suffix=".tmp",
                                          encoding="utf-8", newline="\n", delete=False)
    try:
        with listing:
            listing.write(f"; {name}  code 0x{start:x}..0x{end:x} ({how}), {mcpu}, {bad} invalid words\n")
            listing.write(asm)
            if err.strip():
                listing.write("\n; --- llvm-mc messages ---\n; " + err.strip().replace("\n", "\n; ") + "\n")
        os.replace(listing.name, output_path(path, out))
    finally:
        if os.path.exists(listing.name):
            os.unlink(listing.name)
    return "clean" if bad == 0 else "invalid words"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("--unpack-index", help="disassemble only the FSSMs listed in this unpack index")
    ap.add_argument("--out")
    ap.add_argument("--skip-existing", action="store_true", help="reuse complete listings from a matching run")
    ap.add_argument("--mcpu", default="gfx1013")
    ap.add_argument("--backend", choices=("llvm-mc", "python", "auto"), default="llvm-mc",
                    help="disassembler backend: llvm-mc (default), pure-Python RDNA2 decoder (#1901), or auto")
    ap.add_argument("--limit", type=positive_int)
    ap.add_argument("--jobs", type=positive_int, default=max(1, (os.cpu_count() or 2) // 2))
    a = ap.parse_args()
    if os.path.isfile(a.src):
        if a.unpack_index:
            ap.error("--unpack-index requires an FSSM directory")
        files = [a.src]
    elif os.path.isdir(a.src):
        try:
            if a.unpack_index:
                names = shader_stems(a.unpack_index, "name")
                files = sorted(os.path.join(a.src, name + ".fssm") for name in names)
                for path in files:
                    if not os.path.isfile(path):
                        raise ValueError(f"{a.unpack_index}: declared FSSM file is missing: {path}")
            else:
                with os.scandir(a.src) as entries:
                    files = sorted(entry.path for entry in entries
                                   if entry.name.endswith(".fssm") and entry.is_file())
        except (OSError, ValueError) as e:
            fail(f"cannot list shaders in {a.src}: {e}")
    elif not os.path.exists(a.src):
        print(f"ERROR: {a.src}: does not exist", file=sys.stderr)
        return 2
    else:
        fail(f"{a.src} is not a file or folder")
    if not files:
        fail(f"no .fssm files in {a.src}")
    files = files[: a.limit] if a.limit else files
    out = a.out or os.path.join(os.path.dirname(files[0]), "..", "disasm")
    validate_output(out, files)
    try:
        os.makedirs(out, exist_ok=True)
    except OSError as e:
        fail(f"cannot create the output folder {out}: {e.strerror or e}")
    from collections import Counter
    res = Counter()
    total = len(files)
    every_stem = {os.path.splitext(os.path.basename(p))[0] for p in files}   # before --skip-existing narrows files
    if a.skip_existing:
        pending = []
        for path in files:
            listing = output_path(path, out)
            if not os.path.islink(listing) and os.path.isfile(listing) and os.path.getsize(listing) > 0:
                res["reused"] += 1
            else:
                pending.append(path)
        files = pending
    if files:
        if a.backend == "llvm-mc":
            probe(a.mcpu)
        elif a.backend == "auto":
            try:
                probe(a.mcpu)
            except SystemExit:
                a.backend = "python"
        with ThreadPoolExecutor(a.jobs) as ex:
            res.update(ex.map(lambda p: one(p, out, a.mcpu, backend=a.backend), files))
    print(f"{total} shaders -> {os.path.abspath(out)}: {dict(res)}")
    if FAILED:
        path, reason = FAILED[0]
        fail(f"{len(FAILED)} shader(s) produced no disassembly, first {path}: {reason}")
    if not (res["clean"] or res["invalid words"] or res["reused"]):
        fail(f"no disassembly produced for {a.src}")
    if os.path.isdir(a.src) and not a.limit:
        # A shader dropped by a re-carve must not keep its old listing: the translator reads every .s in this folder,
        # so a stale one would still be translated and indexed even though the corpus no longer has it. Prune only
        # after a successful decode, so a missing llvm-mc or a failed shader never deletes a usable listing. --limit
        # is a partial run on purpose and leaves the rest of the folder alone.
        # Every input's stem, not just this run's `files`: --skip-existing leaves the reused listings out of `files`,
        # and pruning against that list deleted every listing a resumed run had just reused.
        stems = every_stem
        stale = []
        try:
            with os.scandir(out) as entries:
                stale = sorted(e.path for e in entries
                               if e.name.endswith(".s") and e.is_file() and e.name[:-2] not in stems)
            for path in stale:
                os.remove(path)
        except OSError as e:
            fail(f"cannot prune stale .s files in {out}: {e.strerror or e}")
        if stale:
            print(f"removed {len(stale)} stale .s file(s) that no .fssm in {a.src} names")
    return 0


if __name__ == "__main__":
    try:  # a redirected stdout is strict ANSI code page under -I; write '?' for what it cannot encode
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError, OSError):
        pass
    sys.exit(main())
