"""Recover a private x64 exception stack with an explicitly supplied, manually mapped PE."""
import argparse
from dataclasses import dataclass
import mmap
import os
from pathlib import Path, PureWindowsPath
import re
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from pe_inspect import DIR_EXCEPTION, PeFormatError, PeImage
from report_paths import same_report_path


def require(condition, message):
    if not condition:
        raise ValueError(message)


def bounds(offset, size, end):
    require(0 <= offset <= end and 0 <= size <= end - offset, "truncated or invalid binary structure")


def read(data, fmt, offset):
    fmt = "<" + fmt
    bounds(offset, struct.calcsize(fmt), len(data))
    return struct.unpack_from(fmt, data, offset)


def records(start, count, stride, end):
    bounds(start, count * stride, end)
    return range(start, start + count * stride, stride)


def dump_streams(dump):
    magic, version, count, directory = read(dump, "4sIII", 0)
    require(magic == b"MDMP" and version & 0xffff == 0xa793, "expected a Windows minidump")
    streams = {}
    for pos in records(directory, count, 12, len(dump)):
        kind, size, offset = read(dump, "III", pos)
        if kind in (4, 5, 6, 7, 9):
            require(kind not in streams, "duplicate minidump stream")
            bounds(offset, size, len(dump))
            streams[kind] = (offset, offset + size)
    return streams


def stream(streams, kind, minimum):
    require(kind in streams, "required minidump stream missing")
    start, end = streams[kind]
    bounds(start, minimum, end)
    return start, end


def exception_context(dump, streams):
    system, _ = stream(streams, 7, 2)
    require(read(dump, "H", system)[0] == 9, "expected an AMD64 dump")
    require(6 in streams, "dump has no exception context (exit snapshots are unsupported)")
    exc, _ = stream(streams, 6, 168)
    size, offset = read(dump, "II", exc + 160)
    bounds(offset, size, len(dump))
    require(size >= 256 and read(dump, "I", offset + 48)[0] & 0x100001 == 0x100001,
            "missing AMD64 control context")
    return read(dump, "I", exc)[0], read(dump, "Q", offset + 248)[0], read(dump, "Q", offset + 152)[0]


def captured_ranges(dump, streams):
    if 5 in streams:  # MINIDUMP_MEMORY_LIST
        pos, end = stream(streams, 5, 4)
        for entry in records(pos + 4, read(dump, "I", pos)[0], 16, end):
            address, size, offset = read(dump, "QII", entry)
            bounds(offset, size, len(dump))
            yield address, size, offset
    if 9 in streams:  # MINIDUMP_MEMORY64_LIST stores all bytes consecutively
        pos, end = stream(streams, 9, 16)
        count, offset = read(dump, "QQ", pos)
        for entry in records(pos + 16, count, 16, end):
            address, size = read(dump, "QQ", entry)
            bounds(offset, size, len(dump))
            yield address, size, offset
            offset += size


def instruction_window(dump, streams, rip):
    window = None
    for address, size, offset in captured_ranges(dump, streams):
        if address <= rip < address + size:
            start, end = max(rip - 128, address), min(rip + 128, address + size)
            sample = (start, dump[offset + start - address:offset + end - address])
            require(window is None or sample == window, "ambiguous instruction capture")
            window = sample
    require(window is not None, "exception instructions were not captured")
    require(len(window[1]) >= 32 and len(set(window[1])) >= 8, "insufficient distinctive instruction bytes")
    return window


@dataclass(frozen=True)
class Mapping:
    base: int
    size: int
    timestamp: int
    thread: int
    rip: int
    rsp: int


def mapping_base(pe, address, sample):
    # Locate the captured instructions in executable disk bytes. This gives the
    # actual mapping even after rebasing, without trusting the PE preferred base.
    base = None
    previous_end = pe.size_of_headers
    bounds(0, previous_end, min(pe.size_of_image, len(pe.data)))
    for section in sorted(pe.sections, key=lambda item: item["rva"]):
        rva, raw, raw_size = section["rva"], section["rawptr"], section["rawsize"]
        span = section["vsize"] or raw_size
        require(rva >= previous_end, "overlapping PE sections")
        bounds(rva, span, pe.size_of_image)
        bounds(raw, raw_size, len(pe.data))
        previous_end = rva + span
        if not section["exec"]:
            continue
        raw_end = raw + min(span, raw_size)
        pos = pe.data.find(sample, raw, raw_end)
        if pos >= 0:
            require(base is None and pe.data.find(sample, pos + 1, raw_end) < 0,
                    "instructions match multiple locations in the image")
            base = address - (rva + pos - raw)
    require(base is not None, "exception instructions do not match this PE")
    require(base > 0 and base % 0x10000 == 0 and base + pe.size_of_image <= 2**64, "invalid inferred mapping base")
    return base


def verify_unwind(dump, streams, pe, base, rip):
    # A synthetic module label alone does not prove that CDB has unwind data.
    # Require a file-backed RUNTIME_FUNCTION for the fault, and cross-check its
    # metadata and the PE headers wherever the dump actually captured them.
    pdata, pdata_size = pe.directory(DIR_EXCEPTION)
    pdata_off = pe.offset(pdata, pdata_size)
    require(pdata >= pe.size_of_headers and pdata_off is not None and pdata_size % 12 == 0,
            "missing or invalid x64 exception directory")
    metadata = [(base, min(pe.size_of_headers, 4096), 0)]
    function_found = False
    for entry in range(pdata_off, pdata_off + pdata_size, 12):
        begin, end, unwind = read(pe.data, "III", entry)
        if begin <= rip - base < end:
            require(not function_found, "ambiguous x64 function entry")
            function_found = True
            unwind_off = pe.offset(unwind, 4)
            require(unwind >= pe.size_of_headers and unwind_off is not None
                    and pe.data[unwind_off] & 7 in (1, 2), "unsupported x64 unwind information")
            # The header is followed by an array of 2-byte unwind codes.
            unwind_size = 4 + pe.data[unwind_off + 2] * 2
            require(pe.offset(unwind, unwind_size) is not None, "truncated x64 unwind codes")
            metadata.extend([(base + pdata + entry - pdata_off, 12, entry), (base + unwind, unwind_size, unwind_off)])
    require(function_found, "no x64 unwind entry covers the exception instructions")
    for address, length, offset in captured_ranges(dump, streams):
        for start, count, raw in metadata:
            lo, hi = max(address, start), min(address + length, start + count)
            if lo < hi:
                disk = pe.data[raw + lo - start:raw + hi - start]
                require(dump[offset + lo - address:offset + hi - address] == disk,
                        "captured image metadata differs from the supplied PE")


def check_registered_modules(dump, streams, base, size):
    modules, end = stream(streams, 4, 4)
    for entry in records(modules + 4, read(dump, "I", modules)[0], 108, end):
        loaded, extent = read(dump, "QI", entry)
        require(base + size <= loaded or base >= loaded + extent, "mapping overlaps a registered module")
        name_rva = read(dump, "I", entry + 20)[0]
        length = read(dump, "I", name_rva)[0]
        require(0 < length <= 65534 and length % 2 == 0, "invalid registered module name")
        start = name_rva + 4
        bounds(start, length, len(dump))
        name = PureWindowsPath(dump[start:start + length].decode("utf-16-le"))
        require(name.stem.casefold() != "leonida_mapped", "staging name already used by a registered module")
        require((len(name.parts) == 1 and not name.anchor)
                or re.fullmatch(r"[A-Za-z]:", name.drive) and name.is_absolute(),
                "dump contains a nonlocal module path")
        if os.name == "nt" and name.drive:
            require(local_drive(name.anchor), "dump references a module on a nonlocal drive")


def identify(dump, image):
    streams = dump_streams(dump)
    thread, rip, rsp = exception_context(dump, streams)
    address, sample = instruction_window(dump, streams, rip)
    pe = PeImage(image)
    require(pe.machine == 0x8664 and pe.is64, "expected an AMD64 PE32+ image")
    base = mapping_base(pe, address, sample)
    check_registered_modules(dump, streams, base, pe.size_of_image)
    verify_unwind(dump, streams, pe, base, rip)
    return Mapping(base, pe.size_of_image, pe.timestamp, thread, rip, rsp)


ROW = re.compile(r"^\s*([0-9a-f]+)\s+[0-9a-f]{8}\x60[0-9a-f]{8}\s+[0-9a-f]{8}\x60[0-9a-f]{8}\s+(.+)", re.I)
BEGIN = "LEONIDA_STACK_BEGIN"
END = "LEONIDA_STACK_END"


def stack_rows(output, mapping):
    lines = output.splitlines()
    require(lines.count(BEGIN) == 1 and lines.count(END) == 1, "CDB did not finish the stack walk")
    report = lines[lines.index(BEGIN) + 1:lines.index(END)]
    require(report and report[0].strip().lower() == f"{mapping.rip:x} {mapping.rsp:x}",
            "CDB could not select the dump's exception context")
    rows = []
    for line in report[1:]:
        if "following frames may be wrong" in line.lower():
            break
        frame = ROW.match(line)
        if frame:
            require(int(frame[1], 16) == len(rows), "unexpected CDB frame sequence")
            if not rows:
                require(re.match(r"leonida_mapped(?:[+!]|$)", frame[2], re.I), "CDB did not load the mapped image")
            rows.append(line.strip())
    require(len(rows) >= 2, "CDB could not recover a caller")
    return rows


def debugger_stack(cdb, dump, symbols, scratch, mapping):
    # CDB parses its own command language. Only parsed integers enter it. The
    # PE always has the same safe filename in a new, private working directory.
    command = (f".reload /f leonida_mapped.exe={mapping.base:x},{mapping.size:x},{mapping.timestamp:x}; "
               f'~~[{mapping.thread:x}]s; .ecxr; .echo {BEGIN}; '
               f'.printf "%I64x %I64x\\n", @rip, @rsp; kn 40; .echo {END}; q')
    require(not any(c in str(scratch) for c in ";*"), "temporary directory cannot form a debugger search path")
    search = str(scratch) if symbols is None else str(scratch) + ";" + str(symbols)
    env = {key: value for key, value in os.environ.items() if key.upper() in {"SYSTEMROOT", "WINDIR", "SYSTEMDRIVE"}}
    env.update(dict.fromkeys(("TEMP", "TMP", "LOCALAPPDATA", "APPDATA", "USERPROFILE"), str(scratch)))
    run = subprocess.run([str(cdb), "-sins", "-sicv", "-netsyms:no", "-nosqm", "-noshell", "-y", search,
                          "-i", search, "-srcpath", str(scratch), "-z", str(dump), "-c", command],
                         cwd=scratch, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, timeout=90)
    require(run.returncode == 0, "CDB failed to analyze the dump")
    return stack_rows(run.stdout.decode("utf-8", errors="replace"), mapping)


def local_drive(anchor):
    import ctypes
    return ctypes.windll.kernel32.GetDriveTypeW(str(anchor)) in (2, 3, 5, 6)


def local_path(path):
    # Reject UNC, device namespace and mapped network drives before opening
    # inputs or using a directory as a debugger search path.
    absolute = Path(os.path.abspath(path))
    require(re.fullmatch(r"[A-Za-z]:", absolute.drive) and local_drive(absolute.anchor),
            "expected a local filesystem path")
    resolved = absolute.resolve()
    require(re.fullmatch(r"[A-Za-z]:", resolved.drive) and local_drive(resolved.anchor),
            "path resolves outside the local filesystem")
    return resolved


def recover(args):
    require(os.name == "nt", "the x64 Windows debugger is required")
    dump, image = local_path(args.dump), local_path(args.image)
    report = local_path(args.output) if args.output else dump.with_name("mapped-stack.txt")
    require(not report.is_relative_to(ROOT) and not dump.parent.is_relative_to(ROOT),
            "keep the dump and the private report outside the checkout")
    require(not any(same_report_path(report, path) for path in (dump, image)), "report collides with an input")
    require(not report.exists(), "output already exists; choose a fresh report path")
    debugger = args.cdb or shutil.which("cdb.exe")
    require(debugger, "x64 CDB not found; provide --cdb")
    cdb = local_path(debugger)
    symbols = local_path(args.symbols) if args.symbols else None
    require(symbols is None or symbols.is_dir() and not any(c in str(symbols) for c in ";*"),
            "--symbols requires a single local directory")
    with tempfile.TemporaryDirectory(dir=dump.parent) as scratch:
        staged = Path(scratch) / "leonida_mapped.exe"
        shutil.copyfile(image, staged)
        with dump.open("rb") as df, staged.open("rb") as ef:
            with mmap.mmap(df.fileno(), 0, access=mmap.ACCESS_READ) as md, \
                    mmap.mmap(ef.fileno(), 0, access=mmap.ACCESS_READ) as pe:
                mapping = identify(md, pe)
                rows = debugger_stack(cdb, dump, symbols, scratch, mapping)
    # On Windows rename publishes the completed report and refuses an existing
    # destination, including one created by a concurrent analysis.
    with tempfile.TemporaryDirectory(dir=report.parent) as pending:
        finished = Path(pending) / "stack.txt"
        finished.write_text("\n".join(rows) + "\n", encoding="utf-8", newline="\n")
        finished.rename(report)
    print(f"Recovered {len(rows)} frames. Keep the stack report private.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump", type=Path, help="private Windows exception minidump")
    parser.add_argument("image", type=Path, help="the exact mapped PE from the crashing build")
    parser.add_argument("--symbols", type=Path, help="local directory with matching runtime images and PDBs")
    parser.add_argument("--cdb", type=Path, help="path to the trusted x64 cdb.exe (default: search PATH)")
    parser.add_argument("--output", type=Path, help="new private output path (default: mapped-stack.txt beside dump)")
    args = parser.parse_args()
    try:
        recover(args)
    except PeFormatError:
        parser.exit(1, "invalid PE image\n")
    except ValueError as error:
        parser.exit(1, f"{error}\n")
    except subprocess.TimeoutExpired:
        parser.exit(1, "CDB timed out\n")
    except OSError as error:
        parser.exit(1, f"local file or debugger operation failed ({type(error).__name__})\n")


if __name__ == "__main__":
    main()
