"""gamedata_blobs.py must decode the remaining text and binary common.rpf payloads without game data.

    python -I tests/test_gamedata_blobs.py

Every fixture is synthetic: preload lists, visual-settings lines, fixed-stride 1,296-byte records, a compact
3-region spatial grid blob, and minimal SGD5 databases.

Set ``LEONIDA_GAMEDATA_BLOBS`` to another copy of the tool to use as a negative control; main's version fails
the bad-index cases with a bare KeyError or a raw index.json path.
"""
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")
TOOL = os.environ.get("LEONIDA_GAMEDATA_BLOBS", os.path.join(TOOLS, "gamedata_blobs.py"))
sys.path.insert(0, TOOLS)

import gamedata_blobs  # noqa: E402
import sgd5_effects  # noqa: E402


def fixed_table_1296_blob(records: int) -> bytes:
    """Build a synthetic 4 + N * 1296 fixed-stride table."""
    buf = bytearray(struct.pack("<I", records))
    for i in range(records):
        w0, w1 = 0x1000 + i, 0x2000 + i
        rec = bytearray(struct.pack("<4I", w0, w1, w0, w1))
        rec += bytes(64 * 20)
        buf += rec
    return bytes(buf)


def spatial_grid_blob(cells: int = 8) -> bytes:
    """Build a minimal valid 3-region grid (cells -> Region 3 ptrs -> Region 2 records of sizes 3, 6, 11)."""
    r2_offset = cells * 4
    rec_word_sizes = (3, 6, 11)
    r2_bytes = bytearray()
    r2_rec_offsets = []
    pos = r2_offset
    for idx, sz in enumerate(rec_word_sizes):
        r2_rec_offsets.append(pos)
        for w_i in range(sz):
            # Keep Region 2 words >= len(blob) (high bit set) so they are distinct from trailing pointers.
            r2_bytes += struct.pack("<I", 0x80000000 | (idx << 16) | w_i)
        pos += sz * 4

    r3_offset = r2_offset + len(r2_bytes)
    # 3 cell pointer lists: [rec0], [rec1, rec2], [rec0, rec2] -> 5 u32 words
    r3_lists = (
        [r2_rec_offsets[0]],
        [r2_rec_offsets[1], r2_rec_offsets[2]],
        [r2_rec_offsets[0], r2_rec_offsets[2]],
    )
    r3_bytes = bytearray()
    r3_cell_offs = []
    cur = r3_offset
    for lst in r3_lists:
        r3_cell_offs.append(cur)
        for ptr in lst:
            r3_bytes += struct.pack("<I", ptr)
        cur += len(lst) * 4

    grid_bytes = bytearray()
    for i in range(cells):
        if i == 0:
            grid_bytes += struct.pack("<I", gamedata_blobs.CELL_SENTINEL)
        else:
            lst_idx = i % len(r3_lists)
            word = r3_cell_offs[lst_idx] | (len(r3_lists[lst_idx]) << 23)
            grid_bytes += struct.pack("<I", word)
    return bytes(grid_bytes) + bytes(r2_bytes) + bytes(r3_bytes)


def synthetic_sgd5_archive(db0_effects: list[str], db1_effects: list[str]):
    """Minimal two-database SGD5 binary archive matching sgd5_effects.read_database."""
    def effect_record(name: str) -> bytes:
        nbytes = name.encode("ascii") + b"\x00"
        return (
            bytes([len(nbytes)]) + nbytes + bytes(4 + 24 + 80 + 16 + 12)
            + struct.pack("<I", 0x80000000) + bytes(12) + struct.pack("<I", 2) + b"\x00\x00"
        )

    def make_db(names: list[str], start: int):
        buf = bytearray(b"SGD5" + bytes(4))
        first_group = start + len(buf)
        buf += bytes(6 * 4)
        buf += struct.pack("<I", len(names))
        for name in names:
            buf += effect_record(name)
        buf += struct.pack("<I", 1)
        return bytes(buf), (start, first_group, start + len(buf))

    b0, l0 = make_db(db0_effects, 0)
    b1, l1 = make_db(db1_effects, len(b0))
    return b0 + b1, (l0, l1)


class GamedataBlobsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = self.tmp.name
        self.carve = os.path.join(self.root, "carve")
        os.makedirs(self.carve)
        self.rows = []
        self.next_off = 0x1000

    def tearDown(self):
        self.tmp.cleanup()

    def add(self, payload: bytes, chunks=None):
        parts = chunks if chunks is not None else [payload]
        for part in parts:
            off = self.next_off
            self.next_off += 0x1000
            with open(os.path.join(self.carve, f"{off:012x}.bin"), "wb") as f:
                f.write(part)
            self.rows.append({
                "off": off,
                "stored": len(part),
                "size": len(part),
                "head": part[:16].hex(),
                "pre": "",
            })

    def finish(self):
        with open(os.path.join(self.carve, "index.json"), "w", encoding="utf-8") as f:
            json.dump({"summary": {"archive": "synthetic", "files": len(self.rows)}, "files": self.rows}, f)

    def test_parses_fx_preload_vfx_dat_fixed_table_and_spatial_grid(self):
        fx = gamedata_blobs.parse_fx_preload_list("fx_a.fx\nfx_unshipped.fx\nfx_b.fx\n")
        self.assertEqual((fx["kind"], fx["entries"], fx["distinct_entries"], fx["root_entries"]),
                         ("fx_preload_list", 3, 3, 3))

        vfx = gamedata_blobs.parse_vfx_settings_dat("# header comment\n\nfx_a.alpha 1.0\nmisc.key = 2.0\nvec.rgb 1 2 3 4\n")
        self.assertEqual((vfx["kind"], vfx["data_lines"], vfx["comment_lines"], vfx["blank_lines"],
                          vfx["scalar_pairs"], vfx["equation_lines"], vfx["tuple_lines"], vfx["distinct_prefixes"]),
                         ("vfx_settings_dat", 3, 1, 1, 1, 1, 1, 3))

        tbl = gamedata_blobs.parse_fixed_table_1296(fixed_table_1296_blob(3))
        self.assertEqual(
            (tbl["kind"], tbl["record_count"], tbl["record_header_bytes"], tbl["record_body_bytes"],
             tbl["duplicated_header_pairs"], tbl["distinct_header_pairs"]),
            ("fixed_table_1296", 3, 16, 1280, 3, 3),
        )
        with self.assertRaisesRegex(ValueError, "1296"):
            gamedata_blobs.parse_fixed_table_1296(fixed_table_1296_blob(2) + b"\x00")

        blob = spatial_grid_blob(cells=8)
        grid = gamedata_blobs.parse_spatial_grid_blob(blob)
        self.assertEqual((grid["kind"], grid["grid_cells"], grid["empty_cells"], grid["populated_cells"],
                          grid["region2_records"], grid["region3_words"]),
                         ("spatial_grid_table", 8, 1, 7, 3, 5))
        self.assertEqual(grid["region2_record_words_hist"], {"3": 1, "6": 1, "11": 1})
        self.assertEqual(
            (grid["cells_in_region3"], grid["region3_words_in_region2"], grid["region3_end_inclusive"],
             grid["region3_spans"], grid["region3_span_words_min"], grid["region3_span_words_max"]),
            (7, 5, len(blob) - 1, 3, 1, 2),
        )

        # Negative spatial-grid checks: cell offset out of bounds, lowest cell offset skipping Region 3 start
        # where Region 2 cannot end there, and Region 3 pointer targeting Region 3 itself.
        bad_cell = bytearray(blob)
        struct.pack_into("<I", bad_cell, 4, 0)
        with self.assertRaisesRegex(ValueError, "Region 3 bounds"):
            gamedata_blobs.parse_spatial_grid_blob(bytes(bad_cell))

        # The first Region 3 word is the only pointer to the first Region 2 record, and no cell points at it.
        # Starting Region 3 one word later would move Region 2's start up into that record, so there is no
        # consistent split and the blob is still rejected.
        skip_start = bytearray(blob)
        r3_second_off = grid["region3_offset"] + 4
        for cell_idx in range(1, 8):
            word = struct.unpack_from("<I", skip_start, cell_idx * 4)[0]
            if (word & gamedata_blobs.CELL_OFFSET_MASK) == grid["region3_offset"]:
                struct.pack_into("<I", skip_start, cell_idx * 4, (word & ~gamedata_blobs.CELL_OFFSET_MASK) | r3_second_off)
        rec0 = grid["region2_offset"]
        rec2 = struct.unpack_from("<I", skip_start, len(skip_start) - 4)[0]
        for pos in range(grid["region3_offset"] + 4, len(skip_start), 4):
            if struct.unpack_from("<I", skip_start, pos)[0] == rec0:
                struct.pack_into("<I", skip_start, pos, rec2)
        with self.assertRaisesRegex(ValueError, "lowest cell offset"):
            gamedata_blobs.parse_spatial_grid_blob(bytes(skip_start))

        bad_r3 = bytearray(blob)
        struct.pack_into("<I", bad_r3, len(bad_r3) - 4, grid["region3_offset"])
        with self.assertRaisesRegex(ValueError, "Region 2 bounds"):
            gamedata_blobs.parse_spatial_grid_blob(bytes(bad_r3))

    def test_spatial_grid_region2_tail_that_looks_like_an_offset(self):
        """Region 1's lowest cell offset, not the tail scan, decides where Region 3 starts."""
        sentinel = gamedata_blobs.CELL_SENTINEL

        def finding_blob(last_region2_word):
            # cynbun's repro: Region 1 at 0x00, Region 2 (two 2-word records) at 0x10, Region 3 at 0x20.
            region1 = [0x20, sentinel, 0x24, sentinel]
            region2 = [0xDEADBEEF, 0x12345671, 0xCAFEBABD, last_region2_word]
            region3 = [0x10, 0x18]
            return struct.pack(f"<{len(region1) + len(region2) + len(region3)}I", *region1, *region2, *region3)

        expected = {
            "grid_cells": 4, "empty_cells": 2, "populated_cells": 2, "distinct_cell_offsets": 2,
            "region2_offset": 0x10, "region2_bytes": 16, "region2_records": 2,
            "region2_record_words_hist": {"2": 2},
            "region3_offset": 0x20, "region3_bytes": 8, "region3_words": 2,
            "region3_spans": 2, "region3_span_words_min": 1, "region3_span_words_max": 1,
        }
        # 0x13579BDF is the control (not 4-byte aligned); the others are aligned and below the blob size: a Region 1
        # offset (0x8), a Region 2 record start (0x10) and the Region 3 start itself (0x20).
        for word in (0x13579BDF, 0x8, 0x10, 0x20):
            with self.subTest(last_region2_word=hex(word)):
                grid = gamedata_blobs.parse_spatial_grid_blob(finding_blob(word))
                self.assertEqual({key: grid[key] for key in expected}, expected)

        # Several Region 2 tail words in a row that look like offsets: the last record of the fixture builder.
        blob = bytearray(spatial_grid_blob(cells=8))
        normal = gamedata_blobs.parse_spatial_grid_blob(bytes(blob))
        r3 = normal["region3_offset"]
        for pos, word in zip(range(r3 - 12, r3, 4), (normal["region2_offset"], 0x4, r3)):
            struct.pack_into("<I", blob, pos, word)
        grid = gamedata_blobs.parse_spatial_grid_blob(bytes(blob))
        self.assertEqual(grid, normal)

    def test_spatial_grid_truncated_blob_is_rejected(self):
        blob = spatial_grid_blob(cells=8)
        grid = gamedata_blobs.parse_spatial_grid_blob(blob)
        r3 = grid["region3_offset"]
        # Cut inside Region 3: a cell offset now points at or past the end.
        with self.assertRaisesRegex(ValueError, "Region 3 bounds"):
            gamedata_blobs.parse_spatial_grid_blob(blob[:r3 + 8])
        # Cut at the end of Region 2: no Region 3 left at all.
        with self.assertRaisesRegex(ValueError, "missing trailing Region 3"):
            gamedata_blobs.parse_spatial_grid_blob(blob[:r3])
        with self.assertRaisesRegex(ValueError, "4-byte aligned"):
            gamedata_blobs.parse_spatial_grid_blob(blob[:-1])

    def test_reads_carve_cross_references_sgd5_and_protects_paths(self):
        # Pad text/XML cleanly so printable() > 0.95 distinguishes text from binary in small fixtures
        preload0 = ("fx_one.fx\nfx_unused.fx\nfx_two.fx\n" + "\n" * 10).encode("ascii")
        preload1 = ("fx_db1_only.fx\n" * 1).encode("ascii") + b" \n"
        vfx_dat = ("# vfx\nfx_one.scale 0.5\nother.gamma 2.2\n" * 40).encode("ascii")
        # Include enough lines for printable() and make binary chunk distinguishable
        tbl = fixed_table_1296_blob(2)
        grid = spatial_grid_blob(cells=8)

        self.add(tbl)
        self.add(grid)
        self.add(vfx_dat)
        self.add(preload0)
        self.add(preload1)
        self.finish()

        archive_bytes, layouts = synthetic_sgd5_archive(["fx_one", "fx_two"], ["fx_db1_only"])
        shaders_path = os.path.join(self.root, "synthetic_shaders.rpf")
        with open(shaders_path, "wb") as f:
            f.write(archive_bytes)

        with mock.patch.object(sgd5_effects, "DATABASES", layouts):
            payloads = gamedata_blobs.read_payloads(self.carve)
            self.assertEqual([p["kind"] for p in payloads],
                             ["fixed_table_1296", "spatial_grid_table", "vfx_settings_dat", "fx_preload_list", "fx_preload_list"])
            xref = gamedata_blobs.cross_reference_sgd5(payloads, shaders_path)
            self.assertEqual(xref["preload_lists"][0]["in_db0"], 2)
            self.assertEqual(xref["preload_lists"][0]["unshipped"], 1)
            self.assertTrue(xref["preload_lists"][0]["exact_order_db0"])
            self.assertTrue(xref["preload_lists"][1]["exact_order_db1"])
            self.assertEqual(xref["vfx_settings"][0]["prefixes_in_db0"], 1)
            self.assertEqual(xref["vfx_settings"][0]["data_lines"], 80)
            self.assertEqual(xref["vfx_settings"][0]["data_lines_in_db0"], 40)

        out_json = os.path.join(self.root, "reports", "blobs.json")
        p = subprocess.run([sys.executable, "-I", TOOL, self.carve, "--json", out_json],
                           cwd=self.root, capture_output=True, text=True, encoding="utf-8", errors="replace")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        with open(out_json, "rb") as f:
            raw = f.read()
        self.assertIn(b"\n", raw)
        self.assertNotIn(b"\r\n", raw)
        for secret in ("fx_one", "fx_unused", "fx_two", "fx_db1_only", "other.gamma"):
            self.assertNotIn(secret, p.stdout)
            self.assertNotIn(secret.encode("ascii"), raw)

        # Collisions against carve index, bin, shaders.rpf, or existing directory fail cleanly
        for bad_out, extra in (
            (os.path.join(self.carve, "index.json"), []),
            (os.path.join(self.carve, f"{self.rows[0]['off']:012x}.bin"), []),
            (shaders_path, ["--shaders", shaders_path]),
        ):
            r = subprocess.run([sys.executable, "-I", TOOL, self.carve, *extra, "--json", bad_out],
                               cwd=self.root, capture_output=True, text=True, encoding="utf-8", errors="replace")
            self.assertEqual(r.returncode, 1)
            self.assertIn("error:", r.stderr)
            self.assertNotIn("Traceback", r.stderr)


class BadCarveIndexTest(unittest.TestCase):
    """A folder that is not an rpf9_carve.py carve is one error line naming the index and the bad row."""

    def run_tool(self, target):
        # UTF-8 mode: a piped stderr is the ANSI code page on Windows, and the errors below name a path.
        return subprocess.run([sys.executable, "-I", "-X", "utf8", TOOL, target],
                              cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace")

    def assert_one_error(self, result, *needles, code=1, prefix="error:"):
        self.assertEqual(result.returncode, code, result.stdout + result.stderr)
        self.assertNotIn("Traceback", result.stderr)
        lines = result.stderr.strip().splitlines()
        self.assertEqual(len(lines), 1, result.stderr)
        self.assertTrue(lines[0].startswith(prefix), lines[0])
        for needle in needles:
            self.assertIn(needle, lines[0])

    def make_folder(self, index_json=None):
        folder = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, folder, True)
        if index_json is not None:
            with open(os.path.join(folder, "index.json"), "w", encoding="utf-8") as f:
                f.write(index_json)
        return folder

    def test_no_index_json_names_the_folder(self):
        folder = os.path.join(self.make_folder(), "carve-\u00f8")
        os.mkdir(folder)
        self.assert_one_error(self.run_tool(folder), folder, "no index.json")

    def test_malformed_json_names_the_index(self):
        folder = self.make_folder("{oops")
        self.assert_one_error(self.run_tool(folder), "index.json", "is not valid JSON")

    def test_index_without_files_names_the_key(self):
        folder = self.make_folder("{}")
        self.assert_one_error(self.run_tool(folder), "index.json", "no 'files' list")

    def test_row_without_stored_names_the_row(self):
        folder = self.make_folder(json.dumps({"files": [{"off": 0}]}))
        self.assert_one_error(self.run_tool(folder), "index.json", "row 0", "'off'", "'stored'")

    def test_missing_file_names_what_is_wrong(self):
        # A missing input is "ERROR: ..." and exit 2 (#6701, tests/test_missing_input_exit_codes.py).
        folder = self.make_folder()
        self.assert_one_error(self.run_tool(os.path.join(folder, "nope.rpf")), "nope.rpf", "no such file or folder",
                              code=2, prefix="ERROR:")

    def test_reads_carve_index_with_utf8_bom(self):
        folder = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, folder, True)
        payload = fixed_table_1296_blob(1)
        with open(os.path.join(folder, f"{0x1000:012x}.bin"), "wb") as f:
            f.write(payload)
        doc = {"files": [{"off": 0x1000, "stored": len(payload), "size": len(payload), "head": payload[:16].hex(), "pre": ""}]}
        with open(os.path.join(folder, "index.json"), "wb") as f:
            f.write(b"\xef\xbb\xbf" + json.dumps(doc).encode("utf-8"))
        r = self.run_tool(folder)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("fixed_table_1296", r.stdout)


if __name__ == "__main__":
    unittest.main()
