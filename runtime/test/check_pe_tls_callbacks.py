"""gta6rt.exe must have no C++ runtime TLS callbacks (TEST-GTA6RT-NO-TLS-CALLBACKS).

A thread_local with a dynamic initializer or destructor in gta6rt makes the MSVC C++ runtime add two TLS callbacks,
__dyn_tls_init (tlsdyn.obj) and __dyn_tls_dtor (tlsdtor.obj). The CRT startup then registers __dyn_tls_dtor with
_register_thread_local_exe_atexit_callback, the UCRT accepts that only once per process, the title registers its own,
and the game fast-fails at startup with 0xC0000409 (RT-XMEM-STARTUP-FASTFAIL). Trivially destructible thread_local data
is fine: it adds no callback.

Other components may bring their own TLS callback. Rust's std has one (std::sys::thread_local::guard::windows::
tls_callback; std::sys::windows::thread_local_key::on_tls_callback in older toolchains) that runs Rust thread-local
destructors; it is linked in with the Rust audio decoders and does not touch the CRT's atexit registration.

So each callback is named from the linker's map file (gta6rt links with /MAP) and sorted:
  - __dyn_tls_init, __dyn_tls_dtor, or anything from tlsdyn.obj or tlsdtor.obj: fail, this is the startup fast-fail;
  - Rust std's thread-local callback: allowed;
  - anything else, or a callback the map does not name: fail, until someone checks what it does and adds it here.
Without a map, an image with no callbacks passes and one with callbacks fails, saying what to provide.

    python -I check_pe_tls_callbacks.py <gta6rt.exe> [--map <gta6rt.map>]

The map defaults to the image's path with .map. Exit 0 when every callback is allowed (or there is none), 1 otherwise or
when the files cannot be read. Standard library only; reads the files, so it also runs where gta6rt cannot.
"""
import os
import re
import struct
import sys

TLS_DIR_INDEX = 9
MAX_THUNK_HOPS = 4

CRT_DYN_TLS = frozenset(("__dyn_tls_init", "__dyn_tls_dtor"))
CRT_DYN_TLS_OBJECTS = ("tlsdyn.obj", "tlsdtor.obj")
RUST_STD_TLS_CALLBACKS = frozenset((("std", "sys", "thread_local", "guard", "windows", "tls_callback"),
                                    ("std", "sys", "windows", "thread_local_key", "on_tls_callback")))

DANGEROUS, ALLOWED, UNKNOWN = "dangerous", "allowed", "unknown"


def _image(data):
    """(image_base, TimeDateStamp, sections as (va, size, raw)) of a PE32+ image."""
    if data[:2] != b"MZ" or len(data) < 0x40:
        raise ValueError("not a PE (no MZ header)")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not a PE (no PE signature)")
    nsec, timestamp = struct.unpack_from("<HI", data, pe + 6)
    opt_size, = struct.unpack_from("<H", data, pe + 20)
    opt = pe + 24
    if struct.unpack_from("<H", data, opt)[0] != 0x20B:
        raise ValueError("not a PE32+ image")
    image_base, = struct.unpack_from("<Q", data, opt + 24)
    sections = []
    sec = opt + opt_size
    for i in range(nsec):
        vsize, va, rawsize, raw = struct.unpack_from("<IIII", data, sec + 40 * i + 8)
        sections.append((va, max(vsize, rawsize), raw))
    return image_base, timestamp, sections


def _offset(sections, rva):
    for va, size, raw in sections:
        if va <= rva < va + size:
            return raw + rva - va
    raise ValueError("RVA 0x%x is in no section" % rva)


def tls_callbacks(data):
    """RVAs (not VAs) of the TLS callbacks of a PE32+ image, in list order. [] when there is no TLS directory."""
    image_base, _, sections = _image(data)
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    opt = pe + 24
    ndirs, = struct.unpack_from("<I", data, opt + 108)
    if ndirs <= TLS_DIR_INDEX:
        return []
    tls_rva, tls_size = struct.unpack_from("<II", data, opt + 112 + 8 * TLS_DIR_INDEX)
    if not tls_rva or not tls_size:
        return []
    # IMAGE_TLS_DIRECTORY64: StartRaw, EndRaw, AddressOfIndex, AddressOfCallBacks (VAs).
    cb_va, = struct.unpack_from("<Q", data, _offset(sections, tls_rva) + 24)
    if not cb_va:
        return []
    out = []
    pos = _offset(sections, cb_va - image_base)
    while True:
        entry, = struct.unpack_from("<Q", data, pos)
        if not entry:
            return out
        out.append(entry - image_base)
        pos += 8


def thunk_chain(data, rva):
    """rva, then the target of each `jmp rel32` it starts with (an /INCREMENTAL link's thunk), a few hops at most."""
    _, _, sections = _image(data)
    chain = [rva]
    for _ in range(MAX_THUNK_HOPS):
        try:
            pos = _offset(sections, chain[-1])
        except ValueError:
            break
        if data[pos:pos + 1] != b"\xe9" or pos + 5 > len(data):
            break
        rel, = struct.unpack_from("<i", data, pos + 1)
        chain.append((chain[-1] + 5 + rel) & 0xFFFFFFFF)
    return chain


_MAP_SYMBOL = re.compile(r"^\s*([0-9A-Fa-f]{4}):[0-9A-Fa-f]{8}\s+(.+?)\s+([0-9A-Fa-f]{16})(?:\s+[fi])*(?:\s+(\S+))?\s*$")


def read_map(text):
    """(preferred load address or None, timestamp or None, {rva: [(symbol, lib:object)]}) of a link.exe/lld-link map."""
    base = re.search(r"Preferred load address is ([0-9A-Fa-f]+)", text)
    stamp = re.search(r"Timestamp is ([0-9A-Fa-f]{8})", text)
    base = int(base.group(1), 16) if base else None
    symbols = {}
    for line in text.splitlines():
        m = _MAP_SYMBOL.match(line)
        if not m or m.group(1) == "0000" or base is None:  # 0000: absolute symbols, not code
            continue
        symbols.setdefault(int(m.group(3), 16) - base, []).append((m.group(2), m.group(4) or ""))
    return base, (int(stamp.group(1), 16) if stamp else None), symbols


def rust_path(name):
    """The path of a Rust symbol (legacy _ZN or demangled a::b form), without its hash; [] when it is not one."""
    name = name.lstrip("_")
    if "::" in name:
        parts = name.split("::")
    elif name.startswith("ZN") and name.endswith("E"):
        parts, i, body = [], 0, name[2:-1]
        while i < len(body):
            m = re.match(r"\d+", body[i:])
            if not m:
                return []
            n, i = int(m.group()), i + len(m.group())
            parts.append(body[i:i + n])
            i += n
    elif name.startswith("R"):  # v0: crate root after its disambiguator, then the identifiers in order
        m = re.search(r"C(?:s[0-9A-Za-z]*_)?3std((?:\d+[A-Za-z_][0-9A-Za-z_]*)+)$", name)
        if not m:
            return []
        parts, rest = ["std"], m.group(1)
        while rest:
            m = re.match(r"\d+", rest)
            if not m:
                return []
            n = m.group()
            parts.append(rest[len(n):len(n) + int(n)])
            rest = rest[len(n) + int(n):]
    else:
        return []
    if parts and re.fullmatch(r"h[0-9a-f]{16}", parts[-1]):
        parts = parts[:-1]
    return parts


def classify(name, obj):
    """DANGEROUS, ALLOWED or UNKNOWN for one map symbol and its lib:object."""
    if name in CRT_DYN_TLS or obj.lower().endswith(CRT_DYN_TLS_OBJECTS):
        return DANGEROUS
    path = rust_path(name)
    if tuple(path) in RUST_STD_TLS_CALLBACKS:
        return ALLOWED
    return UNKNOWN


def judge(data, symbols):
    """[(rva, verdict, names)] for each TLS callback of `data`, named with `symbols` from read_map."""
    out = []
    for rva in tls_callbacks(data):
        named = [s for hop in thunk_chain(data, rva) for s in symbols.get(hop, [])]
        verdicts = {classify(n, o) for n, o in named}
        verdict = DANGEROUS if DANGEROUS in verdicts else UNKNOWN if not named or UNKNOWN in verdicts else ALLOWED
        out.append((rva, verdict, named))
    return out


def main(argv):
    args = argv[1:]
    map_path = None
    if len(args) == 3 and args[1] == "--map":
        map_path = args[2]
        args = args[:1]
    elif len(args) == 2 and args[1].startswith("--map="):
        map_path = args[1][len("--map="):]
        args = args[:1]
    if len(args) != 1:
        print(__doc__)
        return 1
    exe = args[0]
    try:
        with open(exe, "rb") as f:
            data = f.read()
        image_base, timestamp, _ = _image(data)
        cbs = tls_callbacks(data)
    except (OSError, ValueError, struct.error) as e:
        print("check_pe_tls_callbacks: cannot read %s: %s" % (exe, e))
        return 1
    if not cbs:
        print("ok: %s has no TLS callbacks" % exe)
        return 0
    rvas = ", ".join("0x%x" % c for c in cbs)
    map_path = map_path or os.path.splitext(exe)[0] + ".map"
    try:
        with open(map_path, encoding="utf-8", errors="replace") as f:
            base, map_stamp, symbols = read_map(f.read())
    except OSError:
        print("FAIL: %s has %d TLS callback(s) (RVAs %s) and there is no linker map to name them (%s). Link with /MAP "
              "(the CMake build does) and keep the .map next to the image, or pass --map <file>."
              % (exe, len(cbs), rvas, map_path))
        return 1
    if base is None or base != image_base or map_stamp is None or map_stamp != timestamp:
        print("FAIL: %s is not the map of %s (load address or timestamp differ); relink and run again."
              % (map_path, exe))
        return 1
    try:
        results = judge(data, symbols)
    except (ValueError, struct.error) as e:
        print("check_pe_tls_callbacks: cannot read %s: %s" % (exe, e))
        return 1
    for rva, verdict, named in results:
        print("  TLS callback at RVA 0x%x: %s (%s)" % (rva, verdict, "; ".join(
            "%s in %s" % (n, o or "?") for n, o in named) or "no symbol at this address in the map"))
    bad = [r for r in results if r[1] != ALLOWED]
    if any(v == DANGEROUS for _, v, _ in bad):
        print("FAIL: %s has the C++ runtime's thread_local callbacks (__dyn_tls_init/__dyn_tls_dtor): a thread_local "
              "with a dynamic initializer or destructor was linked in, and the game's own "
              "_register_thread_local_exe_atexit_callback then fast-fails (0xC0000409)." % exe)
        return 1
    if bad:
        print("FAIL: %s has TLS callback(s) this check does not know. Find out what each one does; if it is not a "
              "C++ thread_local with a destructor, add it to the allowed callbacks in check_pe_tls_callbacks.py "
              "with the reason." % exe)
        return 1
    print("ok: %s has %d TLS callback(s), all Rust std's thread-local callback" % (exe, len(cbs)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
