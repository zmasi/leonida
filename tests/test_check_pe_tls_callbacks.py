"""runtime/test/check_pe_tls_callbacks.py on PE32+ images and linker maps built in memory (no MSVC, no game data).

    python -I tests/test_check_pe_tls_callbacks.py

Standard library only.
"""
import struct
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "runtime" / "test"))
import check_pe_tls_callbacks as chk  # noqa: E402

BASE = 0x140000000
STAMP = 0x6745A1B2
CRT_INIT, CRT_DTOR, RUST_CB, OTHER_CB, THUNK = 0x1100, 0x1180, 0x1200, 0x1280, 0x1300
RUST_LEGACY = "_ZN3std3sys12thread_local5guard7windows12tls_callback17h0123456789abcdefE"


def make_pe(callbacks, tls=True, thunks=()):
    """One section (RVA 0x1000, file offset 0x200): TLS directory at +0, callback list at +0x40, code from +0x100.

    thunks: (rva, target) pairs written as `jmp rel32`, as an /INCREMENTAL link does."""
    data = bytearray(0x200 + 0x400)
    data[0:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    pe = 0x80
    data[pe:pe + 4] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", data, pe + 4, 0x8664, 1, STAMP, 0, 0, 240, 0x22)
    opt = pe + 24
    struct.pack_into("<H", data, opt, 0x20B)
    struct.pack_into("<Q", data, opt + 24, BASE)
    struct.pack_into("<I", data, opt + 108, 16)
    if tls:
        struct.pack_into("<II", data, opt + 112 + 8 * 9, 0x1000, 40)
    sec = opt + 240
    data[sec:sec + 8] = b".text\0\0\0"
    struct.pack_into("<IIII", data, sec + 8, 0x400, 0x1000, 0x400, 0x200)
    struct.pack_into("<Q", data, 0x200 + 24, BASE + 0x1040)  # AddressOfCallBacks
    for i, rva in enumerate(list(callbacks) + [0]):
        struct.pack_into("<Q", data, 0x240 + 8 * i, BASE + rva if rva else 0)
    for rva, target in thunks:
        pos = 0x200 + rva - 0x1000
        data[pos] = 0xE9
        struct.pack_into("<i", data, pos + 1, target - (rva + 5))
    return bytes(data)


def make_map(symbols, base=BASE, stamp=STAMP, static=()):
    """A link.exe-style map: publics, then static symbols, each (rva, name, lib:object)."""
    def line(rva, name, obj):
        return " 0001:%08x       %-40s %016x f   %s" % (rva - 0x1000, name, base + rva, obj)
    out = [" gta6rt", "", " Timestamp is %08x (Thu Nov 26 09:16:26 2026)" % stamp, "",
           " Preferred load address is %016x" % base, "",
           " Start         Length     Name                   Class",
           " 0001:00000000 00000400H .text$mn                CODE", "",
           "  Address         Publics by Value              Rva+Base               Lib:Object", "",
           " 0000:00000000       __guard_flags              0000000000000000     <absolute>"]
    out += [line(*s) for s in symbols]
    out += ["", " entry point at        0001:00000000", "", " Static symbols", ""]
    out += [line(*s) for s in static]
    return "\n".join(out) + "\n"


CRT_SYMBOLS = [(CRT_INIT, "__dyn_tls_init", "MSVCRT:tlsdyn.obj"), (CRT_DTOR, "__dyn_tls_dtor", "MSVCRT:tlsdtor.obj")]
RUST_SYMBOL = [(RUST_CB, RUST_LEGACY, "leonida_xma2_decoder.lib:std-0123.std.4567-cgu.0.rcgu.o")]


def run(tmp, pe, map_text=None):
    exe = tmp / "app.exe"
    exe.write_bytes(pe)
    map_file = tmp / "app.map"
    if map_file.exists():
        map_file.unlink()
    if map_text is not None:
        map_file.write_text(map_text, encoding="utf-8", newline="\n")
    return chk.main(["x", str(exe)])


def test_two_callbacks_are_listed():
    assert chk.tls_callbacks(make_pe([0x1100, 0x1200])) == [0x1100, 0x1200]


def test_empty_list_and_no_directory_are_clean():
    assert chk.tls_callbacks(make_pe([])) == []
    assert chk.tls_callbacks(make_pe([], tls=False)) == []


def test_rust_symbol_forms():
    for name in (RUST_LEGACY, "std::sys::thread_local::guard::windows::tls_callback::h0123456789abcdef",
                 "_ZN3std3sys7windows16thread_local_key15on_tls_callback17h0123456789abcdefE",
                 "_RNvNtNtNtNtCs1a2B3c_3std3sys12thread_local5guard7windows12tls_callback"):
        assert chk.classify(name, "x.lib:y.o") == chk.ALLOWED, name
    for name in ("_ZN4core3sys12thread_local12tls_callback17h0123456789abcdefE",
                 "_ZN3std6thread5local12tls_callback17h0123456789abcdefE", "tls_callback", "?cb@@YAXPEAXK0@Z"):
        assert chk.classify(name, "x.lib:y.o") == chk.UNKNOWN, name
    assert chk.classify("__dyn_tls_init", "MSVCRT:tlsdyn.obj") == chk.DANGEROUS
    assert chk.classify("renamed", "LIBCMT:tlsdyn.obj") == chk.DANGEROUS
    assert chk.classify("__dyn_tls_dtor", "MSVCRT:tlsdtor.obj") == chk.DANGEROUS
    assert chk.classify("renamed", "MSVCRT:tlsdtor.obj") == chk.DANGEROUS
    # Only the two exact std paths; a look-alike path is not vetted.
    assert chk.classify("std::evil::thread_local_x::tls_callback", "x.lib:y.o") == chk.UNKNOWN
    assert chk.classify("_ZN3std4evil14thread_local_x12tls_callback17h0123456789abcdefE", "x.lib:y.o") == chk.UNKNOWN
    # Malformed v0 names are not Rust std's callback, and do not raise.
    for name in ("_RNvNtCs1_3std12thread_local12tls_callbackB5_", "_RNvCs_3std1ab"):
        assert chk.rust_path(name) == [], name
        assert chk.classify(name, "x.lib:y.o") == chk.UNKNOWN, name


def test_main_cases():
    with tempfile.TemporaryDirectory() as d:
        tmp = Path(d)
        rust_map = make_map(CRT_SYMBOLS, static=RUST_SYMBOL)
        # None: no map needed.
        assert run(tmp, make_pe([])) == 0
        assert run(tmp, make_pe([], tls=False)) == 0
        # The C++ runtime's pair alone: the startup fast-fail.
        assert run(tmp, make_pe([CRT_INIT, CRT_DTOR]), rust_map) == 1
        # Rust std's callback alone: allowed (the old check failed here).
        assert run(tmp, make_pe([RUST_CB]), rust_map) == 0
        # Both: still the fast-fail.
        assert run(tmp, make_pe([RUST_CB, CRT_INIT, CRT_DTOR]), rust_map) == 1
        assert run(tmp, make_pe([RUST_CB, CRT_DTOR]), rust_map) == 1
        # Two Rust decoders, each with its own std: no count to keep in step.
        two = make_map([], static=RUST_SYMBOL + [(OTHER_CB, RUST_LEGACY.replace("0123", "fedc"), "opus.lib:std.o")])
        assert run(tmp, make_pe([RUST_CB, OTHER_CB]), two) == 0
        # Through an /INCREMENTAL thunk.
        assert run(tmp, make_pe([THUNK], thunks=[(THUNK, RUST_CB)]), rust_map) == 0
        assert run(tmp, make_pe([THUNK], thunks=[(THUNK, CRT_DTOR)]), rust_map) == 1
        # Unknown: no map, an address the map does not name, a symbol nobody vetted.
        assert run(tmp, make_pe([RUST_CB])) == 1
        assert run(tmp, make_pe([OTHER_CB]), rust_map) == 1
        assert run(tmp, make_pe([OTHER_CB]), make_map([(OTHER_CB, "some_tls_callback", "x.lib:y.obj")])) == 1
        # A map from another link.
        assert run(tmp, make_pe([RUST_CB]), make_map([], stamp=STAMP + 1, static=RUST_SYMBOL)) == 1
        assert run(tmp, make_pe([RUST_CB]), make_map([], base=0x10000000000, static=RUST_SYMBOL)) == 1
        no_stamp = "\n".join(l for l in rust_map.splitlines() if "Timestamp is" not in l) + "\n"
        assert run(tmp, make_pe([RUST_CB]), no_stamp) == 1


def test_explicit_map_and_unreadable_inputs():
    with tempfile.TemporaryDirectory() as d:
        tmp = Path(d)
        exe, other = tmp / "a.exe", tmp / "elsewhere.map"
        exe.write_bytes(make_pe([RUST_CB]))
        other.write_text(make_map([], static=RUST_SYMBOL), encoding="utf-8", newline="\n")
        assert chk.main(["x", str(exe), "--map", str(other)]) == 0
        assert chk.main(["x", str(exe), "--map=" + str(other)]) == 0
        assert chk.main(["x", str(exe)]) == 1
        junk = tmp / "junk.exe"
        junk.write_bytes(b"not a pe")
        assert chk.main(["x", str(junk)]) == 1
        assert chk.main(["x", str(tmp / "missing.exe")]) == 1
        assert chk.main(["x"]) == 1


if __name__ == "__main__":
    test_two_callbacks_are_listed()
    test_empty_list_and_no_directory_are_clean()
    test_rust_symbol_forms()
    test_main_cases()
    test_explicit_map_and_unreadable_inputs()
    print("ok")
