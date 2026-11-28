"""psin_psch.py must decode synthetic PMAP/PSCH/PSIG/STNS PSIN containers without game data.

    python -I tests/test_psin_psch.py

Every fixture is synthetic: minimal 'pppppppp' data blocks, big-endian PMAP and
PSCH struct/enum tables, PSIG signatures, STDN/STNS blocks and CHKS.
"""
import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")
TOOL = os.path.join(TOOLS, "psin_psch.py")
sys.path.insert(0, TOOLS)

import psin_psch  # noqa: E402


def _sec(tag: str, payload: bytes) -> bytes:
    return tag.encode("ascii") + struct.pack(">I", 8 + len(payload)) + payload


def build_synthetic_psch_psin(*, bad_array_desc: bool = False, bad_pmap_size: bool = False,
                              with_stdn: bool = True) -> bytes:
    """Build a synthetic complete PMAP/PSCH/PSIG/STNS PSIN container."""
    # Blocks at +0x10:
    #   block 1: struct 0x11111111, byte size 32
    #   block 2: anonymous scalar block type 0x1, length 16
    b1_len = 36 if bad_pmap_size else 32
    b2_len = 16
    data_area = bytearray(16 + b1_len + b2_len)
    data_area[0:4] = b"PSIN"
    struct.pack_into(">I", data_area, 4, len(data_area))  # first_section
    data_area[8:16] = b"pppppppp"

    # PMAP
    pmap_pay = struct.pack(">IHHH6s", 1, 0, 0, 2, b"pppppp")
    pmap_pay += struct.pack(">4I", 0x11111111, 16, 0, b1_len)
    pmap_pay += struct.pack(">4I", 0x00000001, 16 + b1_len, 0, b2_len)

    # PSCH (3 entries: Outer 0x11111111, Inner 0x22222222, Kind 0x33333333)
    outer_members = [
        (0x100, 0x0C, 0, 0, 0x22222222),              # [0] array elem descriptor (Inner)
        (0xAA01, 0x0D, 10, 0, 9 if bad_array_desc else 0),  # [1] array pointing to [0]
        (0xAA02, 0x0E, 1, 16, 0x33333333),            # [2] enum Kind
        (0x100, 0x0B, 0, 0, 0),                       # [3] map val elem descriptor
        (0x100, 0x07, 0, 0, 0),                       # [4] map key elem descriptor
        (0xAA03, 0x10, 1, 20, (3 << 16) | 4),         # [5] map (key=[4], val=[3])
    ]
    inner_members = [(0xBB01, 0x07, 0, 0, 0)]
    enum_values = [(0xCC01, 0, 1), (0xCC02, 0, -2)]

    e0 = struct.pack(">BBHII", 0, 12, len(outer_members), 32, 0) + b"".join(
        struct.pack(">IBBHI", *m) for m in outer_members
    )
    e1 = struct.pack(">BBHII", 0, 12, len(inner_members), 16, 0) + b"".join(
        struct.pack(">IBBHI", *m) for m in inner_members
    )
    e2 = struct.pack(">BBH", 1, 0, len(enum_values)) + b"".join(
        struct.pack(">IIq", *v) for v in enum_values
    )

    dir_end = 12 + 3 * 8
    o0 = dir_end
    o1 = o0 + len(e0)
    o2 = o1 + len(e1)
    psch_pay = (
        struct.pack(">I", 3)
        + struct.pack(">II", 0x11111111, o0)
        + struct.pack(">II", 0x22222222, o1)
        + struct.pack(">II", 0x33333333, o2)
        + e0 + e1 + e2
    )
    psig_pay = struct.pack(">3I", 0x01020304, 0x05060708, 0x090A0B0C)
    stns_rec = bytes(range(16))
    stns_pay = struct.pack("<3I", 1, 0x101, 16) + stns_rec + b"\x00\x00\x00\x00"

    sections = [
        _sec("PMAP", pmap_pay),
        _sec("PSCH", psch_pay),
        _sec("PSIG", psig_pay),
    ]
    if with_stdn:
        sections.append(_sec("STDN", stns_rec))
    sections.append(_sec("STNS", stns_pay))
    total = len(data_area) + sum(len(s) for s in sections) + 20
    sections.append(_sec("CHKS", struct.pack(">II", total, 0xCAFEBABE) + b"zppp"))
    return bytes(data_area) + b"".join(sections)


class PsinPschTests(unittest.TestCase):
    def test_parse_validates_pmap_psch_psig_stns_and_cross_checks(self):
        blob = build_synthetic_psch_psin()
        self.assertTrue(psin_psch.is_psch_container(blob))
        rep = psin_psch.parse_psch_container(blob)
        self.assertTrue(rep["valid"], rep["errors"])
        self.assertEqual(rep["pmap_blocks"], 2)
        self.assertEqual(rep["pmap_struct_blocks"], 1)
        self.assertEqual(rep["pmap_anon_kinds"], {"0x1": 1})
        self.assertTrue(rep["pmap_exact_end"])
        self.assertEqual((rep["structs"], rep["members"], rep["enums"], rep["enum_values"]), (2, 7, 1, 2))
        self.assertEqual(rep["psig_count"], 3)
        self.assertEqual((rep["stns_blocks"], rep["stns_records"], rep["stns_tail_pad"]), (1, 1, 4))
        self.assertTrue(rep["stdn_match"])

        # Empty (count=0) or 2-block STNS sections pad to a 16-byte boundary with 12 zero bytes.
        stns_zero = psin_psch.decode_stns(struct.pack("<I12x", 0))
        self.assertEqual((stns_zero["blocks"], stns_zero["records"], stns_zero["tail_pad"]), ([], 0, 12))
        with self.assertRaisesRegex(ValueError, "padding"):
            psin_psch.decode_stns(struct.pack("<I", 0) + b"\x01" * 12)

    def test_detects_bad_array_descriptor_and_pmap_size_mismatch(self):
        bad_arr = psin_psch.parse_psch_container(build_synthetic_psch_psin(bad_array_desc=True))
        self.assertFalse(bad_arr["valid"])
        self.assertTrue(any("bad descriptor" in e for e in bad_arr["errors"]))

        bad_pmap = psin_psch.parse_psch_container(build_synthetic_psch_psin(bad_pmap_size=True))
        self.assertFalse(bad_pmap["valid"])
        self.assertTrue(any("not divisible" in e for e in bad_pmap["errors"]))

    def test_rejects_overlapping_pmap_blocks_and_duplicate_known_sections(self):
        # Two non-empty blocks where block 2 starts at 0x18 while block 1 covers 0x10..0x28.
        pmap_hdr = struct.pack(">IHHH6s", 1, 1, 0, 2, b"pppppp")
        rows = struct.pack(">4I", 0x11111111, 0x10, 0, 0x18) + struct.pack(">4I", 0x22222222, 0x18, 0, 0x18)
        blob = b"PSIN" + struct.pack(">I", 0x30) + b"pppppppp" + bytes(0x20)
        with self.assertRaisesRegex(ValueError, "overlaps"):
            psin_psch.decode_pmap(pmap_hdr + rows, blob, 0x30)

        # Disjoint out-of-order middle blocks and empty blocks share no bytes and remain accepted.
        pmap_hdr4 = struct.pack(">IHHH6s", 1, 1, 0, 5, b"pppppp")
        rows4 = (
            struct.pack(">4I", 0x11111111, 0x10, 0, 0x10)
            + struct.pack(">4I", 0x22222222, 0x30, 0, 0x10)
            + struct.pack(">4I", 0x33333333, 0x18, 0, 0)
            + struct.pack(">4I", 0x44444444, 0x20, 0, 0x10)
            + struct.pack(">4I", 0x55555555, 0x40, 0, 0x10)
        )
        blob4 = b"PSIN" + struct.pack(">I", 0x50) + b"pppppppp" + bytes(0x40)
        decoded = psin_psch.decode_pmap(pmap_hdr4 + rows4, blob4, 0x50)
        self.assertEqual(len(decoded["blocks"]), 5)

        # Duplicate consumed sections (e.g. STFN) must raise ValueError rather than silently overwriting.
        valid = build_synthetic_psch_psin(with_stdn=False)
        dup_sec = _sec("STFN", b"\x00" * 8)
        with self.assertRaisesRegex(ValueError, "duplicate STFN"):
            psin_psch.parse_psch_container(valid + dup_sec + dup_sec)

        # Repeated unknown sections are not consumed by parse_psch_container and remain accepted.
        unknown_sec = _sec("TEST", b"\x00" * 8)
        parsed = psin_psch.parse_psch_container(valid + unknown_sec + unknown_sec)
        self.assertTrue(parsed["valid"])
        self.assertEqual(parsed["section_tags"].count("TEST"), 2)

    def test_cli_scans_carve_dir_writes_lf_json_and_refuses_overwrite(self):
        with tempfile.TemporaryDirectory() as tmp:
            carve = os.path.join(tmp, "carve")
            os.makedirs(carve)
            # Mix one LSCH-style PSIN (must be ignored) and one PMAP/PSCH PSIN
            lsch_psin = bytearray(16)
            lsch_psin[0:4] = b"PSIN"
            struct.pack_into(">I", lsch_psin, 4, 16)
            lsch_blob = bytes(lsch_psin) + _sec("LSCH", b"") + _sec("LENM", b"") + _sec("CHKS", b"\x00" * 12)
            psch_blob = build_synthetic_psch_psin(with_stdn=False)

            rows = []
            for off, data in ((0x1000, lsch_blob), (0x2000, psch_blob)):
                with open(os.path.join(carve, f"{off:012x}.bin"), "wb") as f:
                    f.write(data)
                rows.append({"off": off, "stored": len(data), "size": len(data), "head": data[:16].hex()})
            with open(os.path.join(carve, "index.json"), "w", encoding="utf-8", newline="\n") as f:
                json.dump({"summary": {"archive": "synthetic", "files": len(rows)}, "files": rows}, f)

            out_json = os.path.join(tmp, "out", "psch.json")
            p = subprocess.run([sys.executable, "-I", TOOL, carve, "--json", out_json],
                               capture_output=True, text=True, encoding="utf-8", errors="replace")
            self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
            with open(out_json, "rb") as f:
                raw = f.read()
            self.assertIn(b"\n", raw)
            self.assertNotIn(b"\r\n", raw)
            rep = json.loads(raw.decode("utf-8"))
            self.assertEqual(rep["containers"], 1)
            self.assertEqual(rep["valid_containers"], 1)

            # Collision refusal
            p2 = subprocess.run([sys.executable, "-I", TOOL, carve, "--json", os.path.join(carve, "00002000".rjust(12, "0") + ".bin")],
                                capture_output=True, text=True, encoding="utf-8", errors="replace")
            self.assertEqual(p2.returncode, 2)
            self.assertIn("overwrite", p2.stderr)

    def test_pmap_exact_end_handles_trailing_empty_and_out_of_order_last_block(self):
        # 1. Trailing empty block (length == 0) after block covering 0x10..0x30
        pmap_hdr = struct.pack(">IHHH6s", 1, 0, 0, 2, b"pppppp")
        rows_empty_tail = (
            struct.pack(">4I", 0x11111111, 0x10, 0, 0x20)
            + struct.pack(">4I", 0x22222222, 0x10, 0, 0)
        )
        blob_30 = b"PSIN" + struct.pack(">I", 0x30) + b"pppppppp" + bytes(0x20)
        dec1 = psin_psch.decode_pmap(pmap_hdr + rows_empty_tail, blob_30, 0x30)
        self.assertTrue(dec1["exact_end"])

        # 2. Out-of-order final row where block 2 ends at first_section (0x50) and block 3 sits at 0x20..0x30
        pmap_hdr3 = struct.pack(">IHHH6s", 1, 0, 0, 3, b"pppppp")
        rows_ooo = (
            struct.pack(">4I", 0x11111111, 0x10, 0, 0x10)
            + struct.pack(">4I", 0x22222222, 0x30, 0, 0x20)
            + struct.pack(">4I", 0x33333333, 0x20, 0, 0x10)
        )
        blob_50 = b"PSIN" + struct.pack(">I", 0x50) + b"pppppppp" + bytes(0x40)
        dec2 = psin_psch.decode_pmap(pmap_hdr3 + rows_ooo, blob_50, 0x50)
        self.assertTrue(dec2["exact_end"])

    def test_schema_detects_self_referencing_descriptors_and_duplicate_hashes(self):
        pmap = {"root_block_id": 1, "blocks": [{"id": 1, "hash": 0x11111111, "offset": 0x10, "length": 16}]}
        psch = {
            "structs": [
                {
                    "hash": 0x11111111,
                    "size": 16,
                    "members": [
                        {"hash": psin_psch.ARRAY_ELEMENT, "type": psin_psch.PSCH_TYPE_ARRAY, "sub": 0, "offset": 0, "ref": 0},
                        {"hash": psin_psch.ARRAY_ELEMENT, "type": psin_psch.PSCH_TYPE_MAP, "sub": 0, "offset": 0, "ref": (1 << 16) | 1},
                    ],
                },
                {"hash": 0x11111111, "size": 16, "members": []},
            ],
            "enums": [
                {"hash": 0x33333333, "values": []},
                {"hash": 0x33333333, "values": []},
            ],
        }
        errs = psin_psch.check_psch_schema(pmap, psch)
        self.assertTrue(any("duplicate PSCH struct hash" in e for e in errs), errs)
        self.assertTrue(any("duplicate PSCH enum hash" in e for e in errs), errs)
        self.assertTrue(any("array member 0 bad descriptor 0" in e for e in errs), errs)
        self.assertTrue(any("map member 1 bad key descriptor 1" in e for e in errs), errs)
        self.assertTrue(any("map member 1 bad value descriptor 1" in e for e in errs), errs)

    def test_scan_target_handles_non_string_and_uppercase_head_values(self):
        psch_blob = build_synthetic_psch_psin(with_stdn=False)
        with tempfile.TemporaryDirectory() as tmp:
            carve = os.path.join(tmp, "carve")
            os.makedirs(carve)
            with open(os.path.join(carve, "000000002000.bin"), "wb") as f:
                f.write(psch_blob)
            rows = [
                {"off": 0x1000, "stored": 16, "size": 16, "head": None},
                {"off": 0x1800, "stored": 16, "size": 16, "head": 12345},
                {"off": 0x2000, "stored": len(psch_blob), "size": len(psch_blob), "head": psch_blob[:16].hex().upper()},
            ]
            with open(os.path.join(carve, "index.json"), "w", encoding="utf-8", newline="\n") as f:
                json.dump({"summary": {"archive": "synthetic", "files": len(rows)}, "files": rows}, f)
            summary = psin_psch.scan_target(carve)
            self.assertEqual(summary["containers"], 1)
            self.assertEqual(summary["valid_containers"], 1)

    def test_carve_dir_bin_overwrite_guard_and_malformed_index_guard(self):
        psch_blob = build_synthetic_psch_psin(with_stdn=False)
        with tempfile.TemporaryDirectory() as tmp:
            carve = os.path.join(tmp, "carve")
            os.makedirs(carve)
            bin_path = os.path.join(carve, "000000002000.bin")
            with open(bin_path, "wb") as f:
                f.write(psch_blob)
            rows = [{"off": 0x2000, "stored": len(psch_blob), "size": len(psch_blob), "head": psch_blob[:16].hex()}]
            with open(os.path.join(carve, "index.json"), "w", encoding="utf-8", newline="\n") as f:
                json.dump({"summary": {"archive": "synthetic", "files": 1}, "files": rows}, f)

            fake_rpf = os.path.join(tmp, "fake.rpf")
            with open(fake_rpf, "wb") as f:
                f.write(b"9FPR" + b"\x00" * 60)

            p1 = subprocess.run(
                [sys.executable, "-I", TOOL, fake_rpf, "--carve-dir", carve, "--json", bin_path],
                capture_output=True, text=True, encoding="utf-8", errors="replace",
            )
            self.assertEqual(p1.returncode, 2, p1.stderr)
            self.assertIn("overwrite", p1.stderr)
            with open(bin_path, "rb") as f:
                self.assertEqual(f.read(), psch_blob)

            # Even when index.json is malformed, --json targeting index.json is refused with exit 2
            bad_carve = os.path.join(tmp, "bad_carve")
            os.makedirs(bad_carve)
            bad_idx = os.path.join(bad_carve, "index.json")
            with open(bad_idx, "w", encoding="utf-8", newline="\n") as f:
                f.write("{broken")
            p2 = subprocess.run(
                [sys.executable, "-I", TOOL, bad_carve, "--json", bad_idx],
                capture_output=True, text=True, encoding="utf-8", errors="replace",
            )
            self.assertEqual(p2.returncode, 2, p2.stderr)
            self.assertIn("overwrite", p2.stderr)
            with open(bad_idx, encoding="utf-8") as f:
                self.assertEqual(f.read(), "{broken")

    def test_atomic_json_write_trailing_lf_and_rejects_nan(self):
        psch_blob = build_synthetic_psch_psin(with_stdn=False)
        with tempfile.TemporaryDirectory() as tmp:
            in_bin = os.path.join(tmp, "one.bin")
            out_json = os.path.join(tmp, "out.json")
            with open(in_bin, "wb") as f:
                f.write(psch_blob)
            p = subprocess.run(
                [sys.executable, "-I", TOOL, in_bin, "--json", out_json],
                capture_output=True, text=True, encoding="utf-8", errors="replace",
            )
            self.assertEqual(p.returncode, 0, p.stderr)
            with open(out_json, "rb") as f:
                written = f.read()
            self.assertTrue(written.endswith(b"\n"))

            sentinel = '{"keep": true}\n'
            with open(out_json, "w", encoding="utf-8", newline="\n") as f:
                f.write(sentinel)
            with self.assertRaises(ValueError):
                psin_psch._write_json(out_json, {"bad": float("nan")})
            with open(out_json, encoding="utf-8") as f:
                self.assertEqual(f.read(), sentinel)
            self.assertEqual(sorted(os.listdir(tmp)), ["one.bin", "out.json"])


if __name__ == "__main__":
    unittest.main()
