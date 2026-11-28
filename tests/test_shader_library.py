"""Unit tests for tools/shader_library.py (the pipeline library in the zlib part of shaders.rpf).

    python -I tests/test_shader_library.py

Zero game data: every library and archive is built here from synthetic bytes.
"""
import contextlib
import hashlib
import io
import json
import os
import struct
import sys
import tempfile
import unittest
from unittest import mock
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import shader_disasm  # noqa: E402
import shader_library as sl  # noqa: E402
import shader_meta  # noqa: E402

ENDPGM = struct.pack("<I", 0xBF810000)
CODE_END = struct.pack("<I", 0xBF9F0000)
V_MOV = struct.pack("<I", 0x7E000280)
NONE = 0xFFFFFFFF


def prologue(tag):
    """Three 12-byte VOP3 opcode-0x180 words with literals: the shape every library program starts with."""
    out = b""
    for lo in (tag & 0xFFFF, (tag >> 16) & 0xFFFF, 0):
        out += struct.pack("<III", 0xD5800000 | lo, 0x03FE0100, 0x00000AC0)
    return out


def prog(tag, body=2):
    code = prologue(tag) + V_MOV * body + ENDPGM
    return code + CODE_END * ((-len(code)) % 256 // 4)


def kind0(threads=(8, 8, 1), rsrc2=0xB8A):
    x, y, z = threads
    return struct.pack("<7I", 0x4005CF00, 0x6C090102, 0, x | y << 16, z, 0x402C0343, rsrc2)


def layout_blob(kind, digest, records=1):
    """A resource-layout blob: head (16 bytes for a graphics stage, 24 for compute) ending in a 0x40-tagged word,
    32-byte 0x41/0x42 records, then 0x43, the 16-byte digest and 12 bytes of padding."""
    head = struct.pack("<I", kind) + bytes(sl.STAGE_HEAD[kind] - 8) + struct.pack("<I", 0x00030440)
    recs = b"".join(struct.pack("<I", 0x41 + (r & 1)) + bytes(16) + struct.pack("<I", 0x43) + bytes(8)
                    for r in range(records))    # a 0x43 inside a record must not end the walk
    return head + recs + struct.pack("<I", 0x43) + digest + bytes(12)


def dxbc(digest, extra=b""):
    """A DXBC container with a HASH part (flags + 16-byte digest) and one more part."""
    parts = [(b"HASH", struct.pack("<I", 0) + digest), (b"SFI0", extra or bytes(8))]
    off = 32 + 4 * len(parts)
    table, body = b"", b""
    for tag, data in parts:
        table += struct.pack("<I", off + len(body))
        body += tag + struct.pack("<I", len(data)) + data
    total = off + len(body)
    return b"DXBC" + bytes(16) + struct.pack("<III", 1, total, len(parts)) + table + body


D_PS, D_VS, D_MS, D_CS = (bytes([n]) * 16 for n in (1, 2, 3, 4))


def build(graphics, compute, blobs, programs, name=b"0x00000000", version=b"1.0"):
    """graphics: [(state, layout, {slot: (program, blob)})]; compute: [(state, blob, program, layout)]."""
    head = bytearray(sl.HEADER)
    head[:4] = sl.MAGIC
    head[4:4 + len(name)] = name
    head[0x24:0x24 + len(version)] = version
    struct.pack_into("<Q", head, 0x48, 0x0123456789ABCDEF)
    blob_data, blob_tab = b"", b""
    for x in blobs:
        blob_tab += struct.pack("<II", len(blob_data), len(x))
        blob_data += x
    code, prog_tab = b"", b""
    for x in programs:
        code += b"\0" * ((-len(code)) % 256)
        prog_tab += struct.pack("<II", len(code), len(x))
        code += x
    struct.pack_into("<6I", head, 0x50, len(graphics), len(compute), len(blobs), len(programs), len(blob_data),
                     len(code))
    gfx = b""
    for i, (state, layout, stages) in enumerate(graphics):
        pairs = []
        for slot in range(7):
            pairs += list(stages.get(slot, (NONE, NONE)))
        gfx += struct.pack("<QII", 0x1000 + i, state, layout) + bytes(40) + struct.pack("<14I", *pairs)
    cs = b"".join(struct.pack("<QIIII", 0x2000 + i, *c) for i, c in enumerate(compute))
    return bytes(head) + gfx + cs + blob_tab + prog_tab + blob_data + code


def sample():
    blobs = [kind0(), b"\x01" * 20, layout_blob(3, D_CS, 3), b"\x03" * 388, b"POSITION\0",
             layout_blob(2, D_PS), layout_blob(2, D_VS, 2), layout_blob(2, D_MS, 0)]
    programs = [prog(0x11112222), prog(0x33334444, 5), prog(0x55556666), prog(0x77778888, 3)]
    graphics = [(3, 4, {0: (0, 5), 1: (1, 6)}),      # VS + PS
                (3, 4, {0: (0, 5), 2: (2, 7)})]      # MS + PS, the PS shared
    compute = [(0, 1, 3, 2)]
    return build(graphics, compute, blobs, programs)


def archive(lib, junk=b"\x78\x9c not a stream " * 50):
    """An archive-like file: unrelated bytes (with false zlib headers), then the library as 64 KiB zlib blocks with
    small gaps between them, as in shaders.rpf."""
    out = bytearray(b"9FPR" + bytes(60) + junk)
    for k in range(0, len(lib), sl.BLOCK):
        out += zlib.compress(lib[k:k + sl.BLOCK], 9)
        out += b"\0" * (k // sl.BLOCK % 13)
    return bytes(out)


class Parse(unittest.TestCase):
    def test_header_and_tables(self):
        b = sample()
        lib = sl.parse(b)
        h = lib["header"]
        self.assertEqual((h["graphics"], h["compute"], h["blobs"], h["programs"]), (2, 1, 8, 4))
        self.assertEqual(h["name"], "0x00000000")
        self.assertEqual(lib["layout"]["end"], len(b))
        g0, g1 = lib["graphics"]
        self.assertEqual(g0["stages"], {"ps": {"program": 0, "blob": 5}, "vs": {"program": 1, "blob": 6}})
        self.assertEqual(set(g1["stages"]), {"ps", "ms"})
        self.assertEqual(lib["compute"][0]["program"], 3)
        self.assertEqual({k: sorted(v) for k, v in lib["stage"].items()},
                         {0: ["ps"], 1: ["vs"], 2: ["ms"], 3: ["cs"]})
        self.assertEqual(sl.program(b, lib, 1)[:36], prologue(0x33334444))
        self.assertEqual(sl.blob(b, lib, 4), b"POSITION\0")
        self.assertEqual(lib["digest"], {0: {D_PS}, 1: {D_VS}, 2: {D_MS}, 3: {D_CS}})

    def test_stage_digest(self):
        self.assertEqual(sl.stage_digest(layout_blob(2, D_PS, 4)), D_PS)
        self.assertEqual(sl.stage_digest(layout_blob(3, D_CS, 0)), D_CS)
        self.assertIsNone(sl.stage_digest(b""))
        self.assertIsNone(sl.stage_digest(layout_blob(2, D_PS)[:-20]))        # cut before the digest ends
        bad = bytearray(layout_blob(2, D_PS))
        bad[12] = 0x3F                                                         # head not closed by a 0x40 word
        self.assertIsNone(sl.stage_digest(bytes(bad)))
        bad = bytearray(layout_blob(3, D_CS, 1))
        bad[24] = 0x44                                                         # unknown record tag
        self.assertIsNone(sl.stage_digest(bytes(bad)))
        self.assertIsNone(sl.stage_digest(b"\x05" + layout_blob(2, D_PS)[1:]))  # unknown kind

    def test_rejects(self):
        b = sample()
        with self.assertRaises(sl.LibraryError):
            sl.parse(b"XX" + b[2:])
        with self.assertRaises(sl.LibraryError):
            sl.parse(b + b"\0")                      # size must match the header exactly
        bad = bytearray(b)                           # a stage naming a program past the table
        struct.pack_into("<I", bad, sl.HEADER + 56, 99)
        with self.assertRaises(sl.LibraryError):
            sl.parse(bytes(bad))
        bad = bytearray(b)                           # a program entry ending past the code section
        lay = sl.layout(sl.header(b))
        struct.pack_into("<I", bad, lay["program_table"] + 4, 1 << 20)
        with self.assertRaises(sl.LibraryError):
            sl.parse(bytes(bad))

    def test_compute_with_missing_program_or_blob(self):
        # A compute record may name no program or no layout blob (0xFFFFFFFF, "a missing stage is program
        # 0xFFFFFFFF"): it must not crash parse(), take a "cs" stage, or link a digest to a missing program.
        blobs = [kind0(), layout_blob(3, D_CS)]
        programs = [prog(0x11112222)]
        b = build([], [(0, 0, NONE, NONE), (0, NONE, 0, NONE)], blobs, programs)
        lib = sl.parse(b)
        self.assertEqual([c["program"] for c in lib["compute"]], [None, 0])
        self.assertEqual(lib["compute"][0]["layout"], None)
        self.assertEqual(lib["stage"], {0: {"cs"}})
        self.assertEqual(lib["digest"], {})

    def test_find_in_archive(self):
        lib = sample() + b""
        # make it span several 64 KiB blocks
        big = build([], [(0, 0, 0, 0)], [kind0()], [prog(1, 40000)])
        for b in (lib, big):
            self.assertEqual(sl.find_library(archive(b)), b)
        with self.assertRaises(sl.LibraryError):
            sl.find_library(archive(big)[:-200])    # the last block cut off: size no longer matches
        with self.assertRaises(sl.LibraryError):
            sl.find_library(b"9FPR" + bytes(4096))


class Main(unittest.TestCase):
    def run_main(self, *args):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = sl.main(list(args))
        return rc, out.getvalue()

    def assert_save_overlap(self, src, out, save, *args):
        err = io.StringIO()
        with contextlib.redirect_stderr(err), self.assertRaises(SystemExit) as caught:
            self.run_main(src, "--out", out, "--save", save, *args)
        self.assertEqual(caught.exception.code, 2)
        self.assertIn("--save overlaps an --out path", err.getvalue())
        self.assertIn(save, err.getvalue())

    def test_save_cannot_create_an_extraction_output(self):
        for parts in ((), ("index.json",), ("library.json",), ("fspx",),
                      ("fspx", "lib_ps_00000.code"), ("fspx", "lib_ps_00000.json"),
                      ("fspx", "lib_cs_00003.state"), ("fspx", "lib_ps_00000.state"),
                      ("disasm",), ("disasm", "lib_ps_00000.s")):
            with self.subTest(parts=parts), tempfile.TemporaryDirectory() as tmp:
                src = os.path.join(tmp, "shaders.rpf")
                with open(src, "wb") as f:
                    f.write(archive(sample()))
                out = os.path.join(tmp, "out")
                args = ("--disasm",) if parts and parts[0] == "disasm" else ()
                self.assert_save_overlap(src, out, os.path.join(out, *parts), *args)
                self.assertEqual(os.listdir(tmp), ["shaders.rpf"])

    def test_save_overlap_leaves_existing_outputs_intact(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, "lib.bin")
            with open(src, "wb") as f:
                f.write(sample())
            out = os.path.join(tmp, "out")
            fspx = os.path.join(out, "fspx")
            os.makedirs(fspx)
            index = os.path.join(out, "index.json")
            code = os.path.join(fspx, "lib_ps_00000.code")
            for path, contents in ((index, b"old index"), (code, b"old code")):
                with open(path, "wb") as f:
                    f.write(contents)

            aliases = [index, os.path.join("out", "fspx", "..", ".", "index.json"), code]
            case_alias = os.path.join(out, "INDEX.JSON")
            if os.path.exists(case_alias):
                aliases.append(case_alias)
            for save in aliases:
                with self.subTest(save=save), contextlib.chdir(tmp):
                    self.assert_save_overlap(src, out, save)
                    self.assertEqual(sorted(os.listdir(out)), ["fspx", "index.json"])
                    self.assertEqual(os.listdir(fspx), ["lib_ps_00000.code"])
                    with open(index, "rb") as f:
                        self.assertEqual(f.read(), b"old index")
                    with open(code, "rb") as f:
                        self.assertEqual(f.read(), b"old code")

    def test_save_overlap_through_symlink_or_hardlink(self):
        for link in (os.symlink, os.link):
            with self.subTest(link=link.__name__), tempfile.TemporaryDirectory() as tmp:
                src = os.path.join(tmp, "lib.bin")
                with open(src, "wb") as f:
                    f.write(sample())
                out = os.path.join(tmp, "out")
                os.mkdir(out)
                index = os.path.join(out, "index.json")
                with open(index, "wb") as f:
                    f.write(b"old index")
                save = os.path.join(tmp, "alias")
                try:
                    link(index, save)
                except (OSError, NotImplementedError) as e:
                    self.skipTest(f"{link.__name__} unavailable: {e}")
                self.assert_save_overlap(src, out, save)
                self.assertEqual(os.listdir(out), ["index.json"])
                for path in (index, save):
                    with open(path, "rb") as f:
                        self.assertEqual(f.read(), b"old index")

    def test_save_cannot_alias_an_out_ancestor_directory(self):
        for precreate in (False, True):
            with self.subTest(precreate=precreate), tempfile.TemporaryDirectory() as tmp:
                src = os.path.join(tmp, "lib.bin")
                with open(src, "wb") as f:
                    f.write(sample())
                parent = os.path.join(tmp, "parent")
                if precreate:
                    with open(parent, "wb") as f:
                        f.write(b"sentinel")
                out = os.path.join(parent, "nested")
                self.assert_save_overlap(src, out, parent)
                if precreate:
                    with open(parent, "rb") as f:
                        self.assertEqual(f.read(), b"sentinel")
                else:
                    self.assertEqual(os.listdir(tmp), ["lib.bin"])

    def test_save_can_write_noncolliding_file_inside_fresh_out_tree(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, "lib.bin")
            with open(src, "wb") as f:
                f.write(sample())
            out = os.path.join(tmp, "out")
            save = os.path.join(out, "fspx", "raw.bin")
            rc, _ = self.run_main(src, "--out", out, "--save", save)
            self.assertEqual(rc, 0)
            with open(save, "rb") as f:
                self.assertEqual(f.read(), sample())
            self.assertTrue(os.path.isfile(os.path.join(out, "index.json")))

    def test_save_can_use_a_filtered_program_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, "lib.bin")
            with open(src, "wb") as f:
                f.write(sample())
            out = os.path.join(tmp, "out")
            fspx = os.path.join(out, "fspx")
            os.makedirs(fspx)
            save = os.path.join(fspx, "lib_ps_00000.code")
            rc, _ = self.run_main(src, "--out", out, "--stages", "vs", "--save", save)
            self.assertEqual(rc, 0)
            with open(save, "rb") as f:
                self.assertEqual(f.read(), sample())
            self.assertEqual(sorted(os.listdir(fspx)), ["lib_ps_00000.code", "lib_vs_00001.code", "lib_vs_00001.json"])

    def test_unpack_layout(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, "shaders.rpf")
            with open(src, "wb") as f:
                f.write(archive(sample()))
            out = os.path.join(tmp, "out")
            other = os.path.join(tmp, "other")
            os.mkdir(other)
            save = os.path.join(other, "saved.json")
            rc, text = self.run_main(src, "--out", out, "--save", save)
            self.assertEqual(rc, 0)
            self.assertIn("2 graphics + 1 compute pipelines, 4 programs; 4 written", text)
            with open(save, "rb") as f:
                self.assertEqual(f.read(), sample())
            with open(os.path.join(out, "index.json"), encoding="utf-8") as f:
                index = json.load(f)
            self.assertEqual([e["file"] for e in index],
                             ["lib_ps_00000.dxbc", "lib_vs_00001.dxbc", "lib_ms_00002.dxbc", "lib_cs_00003.dxbc"])
            cs = index[3]
            self.assertEqual((cs["stage"], cs["threads"], cs["wave32"]), ("cs", [8, 8, 1], False))
            with open(os.path.join(out, "fspx", "lib_cs_00003.json"), encoding="utf-8") as f:
                meta = json.load(f)
            self.assertEqual((meta["rsrc2"], meta["user_sgprs"], meta["code"]["start"]), (0xB8A, 5, 36))
            self.assertTrue(meta["code"]["prologue"])
            with open(os.path.join(out, "fspx", "lib_vs_00001.code"), "rb") as f:
                code = f.read()
            self.assertEqual(code[36:36 + 20], V_MOV * 5)
            self.assertEqual(meta["code"]["end"], 36 + 3 * 4 + 4)
            with open(os.path.join(out, "library.json"), encoding="utf-8") as f:
                summary = json.load(f)
            self.assertEqual(summary["graphics"][1]["stages"], {"ps": "lib_ps_00000", "ms": "lib_ms_00002"})
            self.assertEqual(summary["compute"][0]["program"], "lib_cs_00003")
            # The compute program gets its record's state blob as kind 0, which the run time finds a slot-67
            # pipeline by (fspx_state_sha256); a graphics program gets none.
            with open(os.path.join(out, "fspx", "lib_cs_00003.state"), "rb") as f:
                self.assertEqual(f.read(), kind0())
            self.assertFalse(os.path.exists(os.path.join(out, "fspx", "lib_vs_00001.state")))
            disasm_file = os.path.join(out, "disasm", "lib_cs_00003.s")
            self.assertEqual(shader_meta.fspx_state_sha256_file(disasm_file, "lib_cs_00003"),
                             hashlib.sha256(kind0()).hexdigest())
            self.assertEqual(shader_meta.fspx_code_sha256_file(disasm_file, "lib_cs_00003"),
                             hashlib.sha256(sl.program(sample(), sl.parse(sample()), 3)).hexdigest())

    def test_stage_filter_and_saved_input(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, "lib.bin")
            with open(src, "wb") as f:
                f.write(sample())
            out = os.path.join(tmp, "out")
            rc, text = self.run_main(src, "--out", out, "--stages", "cs,vs")
            self.assertEqual(rc, 0)
            self.assertEqual(sorted(os.listdir(os.path.join(out, "fspx"))),
                             ["lib_cs_00003.code", "lib_cs_00003.json", "lib_cs_00003.state", "lib_vs_00001.code",
                              "lib_vs_00001.json"])

    def test_compute_missing_state_removes_stale_state_and_skips_none_blob(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, "out")
            os.makedirs(os.path.join(out, "fspx"))
            stale = os.path.join(out, "fspx", "lib_cs_00000.state")
            with open(stale, "wb") as f:
                f.write(b"stale state")
            src = os.path.join(tmp, "lib.bin")
            # Compute record with NONE state (0xFFFFFFFF) must not crash or keep a stale .state sidecar.
            with open(src, "wb") as f:
                f.write(build([], [(NONE, NONE, 0, NONE)], [], [prog(0x11112222)]))
            rc, _ = self.run_main(src, "--out", out, "--stages", "cs")
            self.assertEqual(rc, 0)
            self.assertFalse(os.path.exists(stale))
            disasm_file = os.path.join(out, "disasm", "lib_cs_00000.s")
            self.assertIsNone(shader_meta.fspx_state_sha256_file(disasm_file, "lib_cs_00000"))
            meta = shader_meta.collect(disasm_file, "lib_cs_00000", [[]], [[]])
            self.assertIn("fspx_code_sha256", meta)
            self.assertNotIn("fspx_state_sha256", meta)

    def two_state_library(self):
        """Program 0 with three state blobs, two of them the same bytes, and once without a state; program 1 with one
        of those blobs; program 2 in a graphics pipeline. Returns (library, the two distinct states, program 0)."""
        first, second = kind0(), kind0(threads=(32, 1, 1))
        code = prog(3)
        compute = [(0, NONE, 0, NONE), (1, NONE, 0, NONE), (2, NONE, 0, NONE), (NONE, NONE, 0, NONE),
                   (1, NONE, 1, NONE)]
        lib = build([(NONE, NONE, {0: (2, 3)})], compute, [first, second, first, layout_blob(2, D_PS)],
                    [code, prog(4), prog(5)])
        return lib, (first, second), code

    # The entries of that library's compute programs: program 0, its second state (blob 1) and program 1.
    TWO_STATE_NAMES = [sl.program_name(0, {"cs"}), sl.program_name(0, {"cs"}) + "_state_1", sl.program_name(1, {"cs"})]

    def test_program_with_two_states_gets_an_entry_for_each(self):
        lib, (first, second), code = self.two_state_library()
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, "lib.bin")
            with open(src, "wb") as f:
                f.write(lib)
            out = os.path.join(tmp, "out")
            # A further state is an output like any other: --save must not name it.
            names = self.TWO_STATE_NAMES
            self.assert_save_overlap(src, out, os.path.join(out, "fspx", names[1] + ".state"), "--stages", "cs")
            self.assertFalse(os.path.exists(out))
            rc, text = self.run_main(src, "--out", out, "--stages", "cs")
            self.assertEqual(rc, 0)
            self.assertIn("3 written", text)
            with open(os.path.join(out, "index.json"), encoding="utf-8") as f:
                index = json.load(f)
            self.assertEqual([(e["file"], e["program"], e["threads"]) for e in index],
                             [(names[0] + ".dxbc", 0, [8, 8, 1]), (names[1] + ".dxbc", 0, [32, 1, 1]),
                              (names[2] + ".dxbc", 1, [32, 1, 1])])
            self.assertEqual(sorted(os.listdir(os.path.join(out, "fspx"))),
                             sorted(n + ext for n in names for ext in (".code", ".json", ".state")))
            for name, state in zip(names, (first, second, second)):
                with open(os.path.join(out, "fspx", name + ".state"), "rb") as f:
                    self.assertEqual(f.read(), state)
            for name in names[:2]:
                with open(os.path.join(out, "fspx", name + ".code"), "rb") as f:
                    self.assertEqual(f.read(), code)
            # Both entries of program 0 have its code key, and each its own state key.
            metas = [shader_meta.collect(os.path.join(out, "disasm", n + ".s"), n, [[]], [[]]) for n in names[:2]]
            self.assertEqual({m["fspx_code_sha256"] for m in metas}, {hashlib.sha256(code).hexdigest()})
            self.assertEqual([m["fspx_state_sha256"] for m in metas],
                             [hashlib.sha256(first).hexdigest(), hashlib.sha256(second).hexdigest()])
            # Every record names the entry with its state; the one without a state names the program.
            with open(os.path.join(out, "library.json"), encoding="utf-8") as f:
                self.assertEqual([c["program"] for c in json.load(f)["compute"]],
                                 [names[0], names[1], names[0], names[0], names[2]])

    def test_compute_hashes_select_programs_by_their_code(self):
        lib, (first, second), code = self.two_state_library()
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, "lib.bin")
            with open(src, "wb") as f:
                f.write(lib)
            hashes = os.path.join(tmp, "hashes.txt")
            with open(hashes, "w", encoding="utf-8", newline="\n") as f:
                f.write("\n " + hashlib.sha256(code).hexdigest().upper() + "\n\n")
            out = os.path.join(tmp, "out")
            names = self.TWO_STATE_NAMES[:2]
            self.assert_save_overlap(src, out, os.path.join(out, "fspx", names[1] + ".code"),
                                     "--compute-hashes", hashes)
            self.assertFalse(os.path.exists(out))
            rc, text = self.run_main(src, "--out", out, "--compute-hashes", hashes)
            self.assertEqual(rc, 0)
            self.assertIn("2 written", text)
            with open(os.path.join(out, "index.json"), encoding="utf-8") as f:
                self.assertEqual([e["file"] for e in json.load(f)], [n + ".dxbc" for n in names])
            self.assertEqual(sorted(os.listdir(os.path.join(out, "fspx"))),
                             sorted(n + ext for n in names for ext in (".code", ".json", ".state")))
            for name, state in zip(names, (first, second)):
                with open(os.path.join(out, "fspx", name + ".state"), "rb") as f:
                    self.assertEqual(f.read(), state)
            # The tables are the library's, whatever was selected.
            with open(os.path.join(out, "library.json"), encoding="utf-8") as f:
                summary = json.load(f)
            self.assertEqual([c["program"] for c in summary["compute"]],
                             [names[0], names[1], names[0], names[0], self.TWO_STATE_NAMES[2]])
            self.assertEqual(summary["graphics"][0]["stages"], {"ps": sl.program_name(2, {"ps"})})

    def test_compute_hashes_are_checked_before_anything_is_written(self):
        lib, _, code = self.two_state_library()
        digest = hashlib.sha256(code).hexdigest()
        pixel = hashlib.sha256(sl.program(lib, sl.parse(lib), 2)).hexdigest()
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, "lib.bin")
            with open(src, "wb") as f:
                f.write(lib)
            out = os.path.join(tmp, "out")
            listed = os.path.join(tmp, "hashes.txt")

            def usage_error(text, *args):
                with open(listed, "w", encoding="utf-8", newline="\n") as f:
                    f.write(text)
                err = io.StringIO()
                with contextlib.redirect_stderr(err), self.assertRaises(SystemExit) as caught:
                    self.run_main(src, *args)
                self.assertEqual(caught.exception.code, 2)
                return err.getvalue()

            self.assertIn("not allowed with", usage_error(digest, "--out", out, "--stages", "cs",
                                                          "--compute-hashes", listed))
            self.assertIn("--compute-hashes needs --out", usage_error(digest, "--save", os.path.join(tmp, "l.bin"),
                                                                      "--compute-hashes", listed))
            for text in ("", "\n", digest[:-1], digest + " not-a-digest", "0x" + digest):
                with self.subTest(text=text):
                    self.assertIn("cannot read --compute-hashes", usage_error(text, "--out", out,
                                                                              "--compute-hashes", listed))
            self.assertIn("cannot read --compute-hashes",
                          usage_error(digest, "--out", out, "--compute-hashes", os.path.join(tmp, "missing.txt")))
            # A digest no compute program has, here an unknown one and a pixel program's.
            for text in (digest + "\n" + "0" * 64, pixel):
                with self.subTest(text=text):
                    with open(listed, "w", encoding="utf-8", newline="\n") as f:
                        f.write(text)
                    with self.assertRaises(sl.LibraryError) as caught:
                        self.run_main(src, "--out", out, "--compute-hashes", listed)
                    self.assertIn("match no compute program", str(caught.exception))
            self.assertFalse(os.path.exists(out))
            self.assertFalse(os.path.exists(os.path.join(tmp, "l.bin")))

    def test_same_code_with_distinct_states_gets_distinct_state_keys(self):
        code = prog(0x44556677)
        s_a, s_b = kind0(threads=(32, 1, 1)), kind0(threads=(64, 1, 1))
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, "lib.bin")
            with open(src, "wb") as f:
                f.write(build([], [(0, NONE, 0, NONE), (1, NONE, 1, NONE)], [s_a, s_b], [code, code]))
            out = os.path.join(tmp, "out")
            rc, _ = self.run_main(src, "--out", out, "--stages", "cs")
            self.assertEqual(rc, 0)
            metas = [
                shader_meta.collect(os.path.join(out, "disasm", f"lib_cs_{i:05d}.s"), f"lib_cs_{i:05d}", [[]], [[]])
                for i in range(2)
            ]
            self.assertEqual({m["fspx_code_sha256"] for m in metas}, {hashlib.sha256(code).hexdigest()})
            self.assertEqual([m["fspx_state_sha256"] for m in metas],
                             [hashlib.sha256(s_a).hexdigest(), hashlib.sha256(s_b).hexdigest()])

    def test_compute_hash_filter_keeps_distinct_states(self):
        first, second = kind0(), kind0(threads=(32, 1, 1))
        code = prog(3)
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, "lib.bin")
            with open(src, "wb") as f:
                f.write(build([], [(i, NONE, 0, NONE) for i in range(3)] + [(0, NONE, 1, NONE)],
                              [first, second, first], [code, prog(4)]))
            hashes = os.path.join(tmp, "hashes.txt")
            with open(hashes, "w", encoding="utf-8", newline="\n") as f:
                f.write(hashlib.sha256(code).hexdigest() + "\n")
            out = os.path.join(tmp, "out")
            rc, _ = self.run_main(src, "--out", out, "--compute-hashes", hashes)
            self.assertEqual(rc, 0)
            names = ["lib_cs_00000", "lib_cs_00000_state_1"]
            with open(os.path.join(out, "index.json"), encoding="utf-8") as f:
                self.assertEqual([e["file"] for e in json.load(f)], [n + ".dxbc" for n in names])
            for name, state in zip(names, (first, second)):
                with open(os.path.join(out, "fspx", name + ".state"), "rb") as f:
                    self.assertEqual(f.read(), state)
            with open(os.path.join(out, "library.json"), encoding="utf-8") as f:
                self.assertEqual([e["program"] for e in json.load(f)["compute"][:3]], [*names, names[0]])

    def test_carve_names_sources_and_copies_containers(self):
        with tempfile.TemporaryDirectory() as tmp:
            carve = os.path.join(tmp, "carve")
            os.makedirs(carve)
            index = []
            for name, d in (("PS_A", D_PS), ("PS_A~1", D_PS), ("CS_B", D_CS), ("VS_C", D_VS)):
                with open(os.path.join(carve, name + ".dxbc"), "wb") as f:
                    f.write(dxbc(d))
                index.append({"name": name, "file": name + ".dxbc"})
            index.append({"name": "gone", "file": "gone.dxbc"})       # listed but missing: skipped
            with open(os.path.join(carve, "index.json"), "w", encoding="utf-8", newline="\n") as f:
                json.dump(index, f)
            src = os.path.join(tmp, "lib.bin")
            with open(src, "wb") as f:
                f.write(sample())
            out = os.path.join(tmp, "out")
            self.assert_save_overlap(src, out, os.path.join(out, "lib_vs_00001.dxbc"), "--carve", carve)
            self.assertFalse(os.path.exists(out))
            os.mkdir(out)
            save = os.path.join(out, "lib_ms_00002.dxbc")
            rc, text = self.run_main(src, "--out", out, "--carve", carve, "--save", save)
            self.assertEqual(rc, 0)
            self.assertIn("3 of 4 named by a carved shader's HASH digest, 1 not in the carve", text)
            with open(os.path.join(out, "index.json"), encoding="utf-8") as f:
                by = {e["file"]: e for e in json.load(f)}
            self.assertEqual(by["lib_ps_00000.dxbc"]["source"], ["PS_A", "PS_A~1"])
            self.assertEqual(by["lib_cs_00003.dxbc"]["source"], ["CS_B"])
            self.assertEqual(by["lib_ms_00002.dxbc"]["source"], [])
            with open(os.path.join(out, "lib_vs_00001.dxbc"), "rb") as f:
                self.assertEqual(f.read(), dxbc(D_VS))
            with open(save, "rb") as f:
                self.assertEqual(f.read(), sample())
            with open(os.path.join(out, "fspx", "lib_ps_00000.json"), encoding="utf-8") as f:
                self.assertEqual(json.load(f)["digest"], [D_PS.hex()])

    def test_translator_reads_compute_sidecar(self):
        import rdna2spirv
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, "lib.bin")
            with open(src, "wb") as f:
                f.write(sample())
            out = os.path.join(tmp, "out")
            self.run_main(src, "--out", out, "--stages", "cs")
            s_path = os.path.join(out, "disasm", "lib_cs_00003.s")
            self.assertEqual(rdna2spirv.cs_dispatch_state(s_path, "lib_cs_00003"), (0xB8A, 8, 8, 1))
            self.assertEqual(rdna2spirv.native_blob(s_path, "lib_cs_00003")[:36], prologue(0x77778888))

    def test_translator_rejects_pixel_program_without_container(self):
        """A pixel program whose source isn't in the carve has no <name>.dxbc for its input signature: Unsupported for
        that shader, not a FileNotFoundError that stops the run."""
        import rdna2spirv
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, "lib.bin")
            with open(src, "wb") as f:
                f.write(sample())
            out = os.path.join(tmp, "out")
            self.run_main(src, "--out", out, "--stages", "ps")
            os.makedirs(os.path.join(out, "disasm"))
            s_path = os.path.join(out, "disasm", "lib_ps_00000.s")
            with open(s_path, "w", encoding="utf-8", newline="\n") as f:
                f.write("; lib_ps_00000  code 0x24..0x30 (fspx), gfx1013, 0 invalid words\n\ts_endpgm\n")
            rdna2spirv._STAGES = None                    # stage_of caches the first index.json it reads
            try:
                with self.assertRaisesRegex(rdna2spirv.Unsupported, "no shader container"):
                    rdna2spirv.translate(s_path)
            finally:
                rdna2spirv._STAGES = None

    def test_carve_reports_corrupt_and_unreadable_containers(self):
        with tempfile.TemporaryDirectory() as tmp:
            carve = os.path.join(tmp, "carve")
            os.makedirs(carve)
            with open(os.path.join(carve, "PS_A.dxbc"), "wb") as f:
                f.write(dxbc(D_PS))
            with open(os.path.join(carve, "VS_C.dxbc"), "wb") as f:
                f.write(dxbc(D_VS)[:20])                             # truncated header
            with open(os.path.join(carve, "MS_D.dxbc"), "wb") as f:
                f.write(dxbc(D_MS)[:-4])                             # truncated part payload
            os.makedirs(os.path.join(carve, "CS_B.dxbc"))            # exists on disk, unreadable OSError
            index = [
                {"name": "PS_A", "file": "PS_A.dxbc"},
                {"name": "gone", "file": "gone.dxbc"},               # missing: skipped silently
                {"name": "VS_C", "file": "VS_C.dxbc"},
                {"name": "MS_D", "file": "MS_D.dxbc"},
                {"name": "CS_B", "file": "CS_B.dxbc"},
            ]
            with open(os.path.join(carve, "index.json"), "w", encoding="utf-8", newline="\n") as f:
                json.dump(index, f)
            src = os.path.join(tmp, "lib.bin")
            with open(src, "wb") as f:
                f.write(sample())
            out = os.path.join(tmp, "out")
            stdout, stderr = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                rc = sl.main([src, "--out", out, "--carve", carve])
            self.assertEqual(rc, 0)
            self.assertIn("1 of 4 named by a carved shader's HASH digest, 3 not in the carve, "
                          "3 unreadable/corrupt containers", stdout.getvalue())
            err_lines = stderr.getvalue().splitlines()
            self.assertEqual(len(err_lines), 3)
            self.assertEqual(err_lines[0], "carve: VS_C.dxbc: truncated DXBC header")
            self.assertEqual(err_lines[1], "carve: MS_D.dxbc: truncated DXBC part payload")
            self.assertTrue(err_lines[2].startswith("carve: CS_B.dxbc: ") and len(err_lines[2]) > len("carve: CS_B.dxbc: "))
            self.assertNotIn("gone.dxbc", stderr.getvalue())
            self.assertTrue(os.path.isfile(os.path.join(out, "lib_ps_00000.dxbc")))

    def test_carve_missing_only_stays_silent(self):
        with tempfile.TemporaryDirectory() as tmp:
            carve = os.path.join(tmp, "carve")
            os.makedirs(carve)
            with open(os.path.join(carve, "PS_A.dxbc"), "wb") as f:
                f.write(dxbc(D_PS))
            index = [
                {"name": "PS_A", "file": "PS_A.dxbc"},
                {"name": "gone", "file": "gone.dxbc"},
            ]
            with open(os.path.join(carve, "index.json"), "w", encoding="utf-8", newline="\n") as f:
                json.dump(index, f)
            src = os.path.join(tmp, "lib.bin")
            with open(src, "wb") as f:
                f.write(sample())
            out = os.path.join(tmp, "out")
            stdout, stderr = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                rc = sl.main([src, "--out", out, "--carve", carve])
            self.assertEqual(rc, 0)
            self.assertEqual(stderr.getvalue(), "")
            self.assertIn("1 of 4 named by a carved shader's HASH digest, 3 not in the carve\n", stdout.getvalue())
            self.assertNotIn("unreadable/corrupt", stdout.getvalue())

    def test_carve_single_corrupt_container_and_direct_api(self):
        with tempfile.TemporaryDirectory() as tmp:
            carve = os.path.join(tmp, "carve")
            os.makedirs(carve)
            with open(os.path.join(carve, "PS_A.dxbc"), "wb") as f:
                f.write(dxbc(D_PS))
            with open(os.path.join(carve, "ZERO.dxbc"), "wb") as f:
                f.write(bytes(64))                                   # valid length/count=0, wrong magic
            index = [
                {"name": "PS_A", "file": "PS_A.dxbc"},
                {"name": "ZERO", "file": "ZERO.dxbc"},
            ]
            with open(os.path.join(carve, "index.json"), "w", encoding="utf-8", newline="\n") as f:
                json.dump(index, f)
            direct_err = io.StringIO()
            with contextlib.redirect_stderr(direct_err):
                digests = sl.carve_digests(carve)
            self.assertEqual(digests, {D_PS: [("PS_A", os.path.join(carve, "PS_A.dxbc"))]})
            self.assertEqual(direct_err.getvalue().splitlines(), ["carve: ZERO.dxbc: not a DXBC container"])
            src = os.path.join(tmp, "lib.bin")
            with open(src, "wb") as f:
                f.write(sample())
            out = os.path.join(tmp, "out")
            stdout, stderr = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                rc = sl.main([src, "--out", out, "--carve", carve])
            self.assertEqual(rc, 0)
            self.assertIn("1 of 4 named by a carved shader's HASH digest, 3 not in the carve, "
                          "1 unreadable/corrupt container\n", stdout.getvalue())
            self.assertEqual(stderr.getvalue().splitlines(), ["carve: ZERO.dxbc: not a DXBC container"])

    def test_disasm_auto_falls_back_per_program_without_regressing_clean_programs(self):
        ps_prog = prog(0x11112222)
        vs_body = bytes.fromhex("02 1a 28 f4 00 00 00 fa") + ENDPGM
        vs_code = prologue(0x33334444) + vs_body
        vs_prog = vs_code + CODE_END * ((-len(vs_code)) % 256 // 4)
        blobs = [layout_blob(2, D_PS), layout_blob(2, D_VS)]
        graphics = [(NONE, NONE, {0: (0, 0), 1: (1, 1)})]
        lib_bytes = build(graphics, [], blobs, [ps_prog, vs_prog])

        def fake_run_mc(code, mcpu, wave32=True):
            if code == V_MOV * 2 + ENDPGM:
                return "\t; mocked llvm-mc ps\n\ts_endpgm\n", 0, ""
            return "\ts_endpgm\n", 2, "<stdin>:1:1: warning: invalid instruction encoding\n"

        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, "lib.bin")
            with open(src, "wb") as f:
                f.write(lib_bytes)
            out = os.path.join(tmp, "out")
            with mock.patch.object(shader_disasm, "probe") as probe_mock, \
                 mock.patch.object(shader_disasm, "run_mc", side_effect=fake_run_mc):
                rc, _ = self.run_main(src, "--out", out, "--disasm", "--jobs", "1")
            self.assertEqual(rc, 0)
            probe_mock.assert_called_once_with("gfx1013")
            with open(os.path.join(out, "disasm", "lib_ps_00000.s"), encoding="utf-8") as f:
                ps_text = f.read()
            self.assertIn("; mocked llvm-mc ps", ps_text)
            self.assertIn("0 invalid words", ps_text)
            self.assertNotIn("; --- llvm-mc messages ---", ps_text)
            with open(os.path.join(out, "disasm", "lib_vs_00001.s"), encoding="utf-8") as f:
                vs_text = f.read()
            self.assertIn("0 invalid words", vs_text)
            self.assertIn("[s104,s105,vcc_lo,vcc_hi]", vs_text)
            self.assertNotIn("; --- llvm-mc messages ---", vs_text)
            with open(os.path.join(out, "index.json"), encoding="utf-8") as f:
                by = {e["file"]: e for e in json.load(f)}
            self.assertEqual(by["lib_ps_00000.dxbc"]["invalid_words"], 0)
            self.assertEqual(by["lib_vs_00001.dxbc"]["invalid_words"], 0)


if __name__ == "__main__":
    unittest.main()
