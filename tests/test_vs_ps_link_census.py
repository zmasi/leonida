"""tools/vs_ps_link_census.py on synthetic signatures and FSSM stage tables (no game data, standard library only).

    python -I tests/test_vs_ps_link_census.py

Each case writes a small carve (index.json, DXBC containers with an OSG1 or ISG1 part, FSSM blobs whose stage data
is laid out as VIctory's native.rs describes it) and checks which pipelines the census reports: the same link by
param number and by signature row, a pixel shader that reads a subset at other numbers, the same param in other
components, an input no producer writes, a mesh producer with a per-primitive param, pairing by technique name or
by manifest, and the command line.
"""
import contextlib
import io
import json
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
import vs_ps_link_census as census  # noqa: E402

UNUSED = 0xDEADBEEFDEADBEEF


def sig_part(elements):
    """ISG1/OSG1 part data: (name, index, system value, register, mask) per element."""
    head = struct.pack("<II", len(elements), 8)
    names, table = b"", b""
    base = 8 + 32 * len(elements)
    offsets = {}
    for name, *_ in elements:
        if name not in offsets:
            offsets[name] = base + len(names)
            names += name.encode() + b"\0"
    for name, index, system, register, mask in elements:
        table += struct.pack("<6IBBHI", 0, offsets[name], index, system, 3, register, mask, mask, 0, 0)
    return head + table + names


def container(parts):
    body, offsets = b"", []
    start = 32 + 4 * len(parts)
    for tag, data in parts.items():
        offsets.append(start + len(body))
        body += tag + struct.pack("<I", len(data)) + data
    head = b"DXBC" + bytes(16) + struct.pack("<III", 1, start + len(body), len(parts))
    return head + b"".join(struct.pack("<I", o) for o in offsets) + body


def fssm(stage, records):
    """An FSSM blob with one stage record at blob 0xC0 (payload 0xB0) and its stage table."""
    blob = bytearray(0x400)
    blob[:4] = b"FSSM"
    struct.pack_into("<IQ", blob, 4, 0x101, len(blob) - 0x10)
    for slot in range(6):
        struct.pack_into("<Q", blob, 0x18 + 8 * slot, UNUSED)
    struct.pack_into("<QQ", blob, 0x58, UNUSED, UNUSED)
    struct.pack_into("<Q", blob, census.SLOT[stage], 0xB0)
    e = 0xC0 + 0x54
    if stage == "ps":
        for i, (usage, register, attr) in enumerate(records):
            blob[e + 0x24 + 8 * i:e + 0x24 + 8 * i + 8] = bytes([usage, register, 0xF, attr, 0, register, attr + 1, 2])
        struct.pack_into("<I", blob, e + 0x124, len(records))
    else:
        for i, (usage, register, param) in enumerate(records):
            blob[e + 0xC8 + 6 * i:e + 0xC8 + 6 * i + 6] = bytes([usage, register, param, register, param + 1, 0])
        struct.pack_into("<I", blob, e + 0x188, len(records))
    return bytes(blob)


VS_SIG = [("SV_Position", 0, 1, 0, 0xF), ("TEXCOORD", 0, 0, 1, 0xF), ("TEXCOORD", 1, 0, 2, 0x3),
          ("TEXCOORD", 2, 0, 2, 0xC)]
VS_RECORDS = [(6, 1, 0), (6, 2, 1)]                      # reg 1 -> param 0, reg 2 (TEXCOORD1.xy, TEXCOORD2.zw) -> 1


class Carve:
    def __init__(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name
        os.mkdir(os.path.join(self.dir, "fssm"))
        self.rows = []

    def add(self, name, stage, sig, records):
        self.rows.append({"name": name, "file": name + ".dxbc", "stage": stage})
        tag = b"ISG1" if stage == "ps" else b"OSG1"
        with open(os.path.join(self.dir, name + ".dxbc"), "wb") as f:
            f.write(container({tag: sig_part(sig)}))
        with open(os.path.join(self.dir, "fssm", name + ".fssm"), "wb") as f:
            f.write(fssm(stage, records))
        with open(os.path.join(self.dir, "index.json"), "w", encoding="utf-8", newline="\n") as f:
            json.dump(self.rows, f)

    def close(self):
        self.tmp.cleanup()


class Census(unittest.TestCase):
    def setUp(self):
        self.carve = Carve()
        self.addCleanup(self.carve.close)
        self.carve.add("VS_Same_0123abcd_Wrapped", "vs", VS_SIG, VS_RECORDS)

    def pair(self, ps_sig, ps_records, name="Same"):
        self.carve.add(f"PS_{name}_89abcdef_Wrapped", "ps", ps_sig, ps_records)
        return census.census(self.carve.dir)

    def test_same_order(self):
        r = self.pair([("SV_Position", 0, 1, 0, 0xF), ("TEXCOORD", 0, 0, 1, 0xF), ("TEXCOORD", 1, 0, 2, 0x3)],
                      [(6, 1, 0), (6, 2, 1)])
        self.assertEqual(r["pipelines"], {"pipelines": 1, "same": 1, "inputs": 2})
        self.assertEqual(r["differ"], [])
        self.assertEqual(r["shaders"], {"vs": 1, "vs in row order": 1, "vs with an unresolved row": 0,
                                        "ps": 1, "ps in row order": 1, "ps with an unresolved row": 0})

    def test_subset_at_other_numbers(self):
        # The PS reads only TEXCOORD1, at register (row) 1, and calls it attr 0: by number it would read the VS's
        # param 0, TEXCOORD0. Through the signatures that is another param; by row the VS's row 1 is param 0 too,
        # since the two signatures give TEXCOORD1 different registers.
        r = self.pair([("SV_Position", 0, 1, 0, 0xF), ("texcoord", 1, 0, 1, 0x3)], [(6, 1, 0)])
        self.assertEqual((r["pipelines"]["differ semantic param"], r["pipelines"].get("differ row param", 0)), (1, 0))
        self.assertEqual(r["differ"], [("VS_Same_0123abcd_Wrapped -> PS_Same_89abcdef_Wrapped",
                                        {"semantic param": 1})])

    def test_row_at_other_numbers(self):
        # The PS skips the VS's row 1 and reads row 2 (TEXCOORD1, same register in both) as attr 0.
        r = self.pair([("TEXCOORD", 1, 0, 2, 0x3)], [(6, 2, 0)])
        self.assertEqual(r["differ"], [("VS_Same_0123abcd_Wrapped -> PS_Same_89abcdef_Wrapped",
                                        {"row param": 1, "semantic param": 1})])

    def test_rows_past_the_signature_link_by_row(self):
        # A row no ISG1 register has (seen with 16-bit elements): linked by row only.
        self.carve.add("VS_Wide_0000cccc", "vs", VS_SIG, VS_RECORDS + [(6, 7, 2)])
        r = self.pair([("TEXCOORD", 0, 0, 1, 0xF)], [(6, 1, 0), (6, 7, 1)], name="Wide")
        self.assertEqual(r["pipelines"]["inputs unresolved"], 1)
        self.assertEqual(r["pipelines"]["inputs row param"], 1)        # row 7 is param 2 there, attr 1 here
        self.assertEqual(r["shaders"]["ps with an unresolved row"], 1)

    def test_same_param_other_components(self):
        # TEXCOORD2 is param 1 .zw in the VS; the PS has it at attr 1 but in .xy.
        r = self.pair([("TEXCOORD", 0, 0, 1, 0xF), ("TEXCOORD", 2, 0, 2, 0x3)], [(6, 1, 0), (6, 2, 1)])
        self.assertEqual(r["pipelines"]["differ semantic component"], 1)
        self.assertEqual(r["pipelines"]["inputs semantic component"], 1)
        self.assertNotIn("differ row param", r["pipelines"])

    def test_input_without_an_output(self):
        r = self.pair([("COLOR", 0, 0, 1, 0xF)], [(6, 1, 0)])
        self.assertEqual(r["pipelines"]["inputs semantic missing"], 1)
        self.assertEqual(r["pipelines"]["differ"], 1)
        r = self.pair([("COLOR", 0, 0, 1, 0xF)], [(6, 5, 0)], name="Gap")   # unpaired name: no new pipeline
        self.assertEqual(r["pipelines"]["pipelines"], 1)

    def test_mesh_producer_and_per_primitive_bit(self):
        self.carve.add("MS_Mesh_0000aaaa", "ms", [("TEXCOORD", 0, 0, 1, 0xF), ("PRIMID", 0, 0, 2, 0x1)],
                       [(6, 1, 0), (6, 2, 0x80 | 1)])
        r = self.pair([("TEXCOORD", 0, 0, 1, 0xF), ("PRIMID", 0, 0, 2, 0x1)], [(6, 1, 0), (6, 2, 1)], name="Mesh")
        self.assertEqual(r["pipelines"]["pipelines"], 1)          # VS_Same has no PS_Same here: only the mesh pair
        self.assertEqual(r["pipelines"]["same"], 1)

    def test_names_pair_every_producer_with_every_ps_of_a_technique(self):
        self.carve.add("VS_Same_4444bbbb_Wrapped", "vs", VS_SIG, [(6, 1, 1), (6, 2, 0)])   # numbered the other way
        self.carve.add("VS_Alone", "vs", VS_SIG, VS_RECORDS)
        r = self.pair([("TEXCOORD", 0, 0, 1, 0xF)], [(6, 1, 0)])
        self.assertEqual((r["pipelines"]["pipelines"], r["pipelines"]["same"], r["pipelines"]["differ"]), (2, 1, 1))
        self.assertEqual(r["shaders"]["vs in row order"], 1)

    def test_manifest_and_unreadable_shaders(self):
        self.carve.add("PS_Other", "ps", [("TEXCOORD", 1, 0, 2, 0x3)], [(6, 2, 1)])
        manifest = os.path.join(self.carve.dir, "pipelines.json")
        with open(manifest, "w", encoding="utf-8", newline="\n") as f:
            json.dump({"version": 1, "pipelines": [{"name": "a", "vs": "VS_Same_0123abcd_Wrapped", "ps": "PS_Other"},
                                                   {"name": "b", "vs": {"shader": "VS_Same_0123abcd_Wrapped"},
                                                    "ps": "PS_Nowhere"},
                                                   {"name": "c", "vs": "VS_Same_0123abcd_Wrapped"}]}, f)
        r = census.census(self.carve.dir, census.manifest_pipelines(manifest))
        self.assertEqual(r["pipelines"], {"pipelines": 1, "same": 1, "inputs": 1, "unreadable": 1})

    def test_stage_table_errors(self):
        with self.assertRaises(ValueError):
            census.stage_records(b"NOPE" + bytes(0x400), "vs")
        blob = bytearray(fssm("vs", VS_RECORDS))
        struct.pack_into("<I", blob, 0xC0 + 0x54 + 0x188, 33)
        with self.assertRaises(ValueError):
            census.stage_records(bytes(blob), "vs")
        self.assertEqual(census.stage_records(fssm("ps", [(6, 3, 0), (7, 4, 1)]), "ps"), [(6, 3, 0), (7, 4, 1)])

    def test_command_line(self):
        self.pair([("texcoord", 1, 0, 1, 0x3)], [(6, 1, 0)])
        out = os.path.join(self.carve.dir, "counts.json")
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            code = census.main([self.carve.dir, "--list", "--json", out])
        self.assertEqual(code, 1)
        self.assertIn("1 pipelines read (0 unreadable): 0 link the same by number, by row and by semantic, 1 differ",
                      buf.getvalue())
        self.assertIn("by semantic: 1 at another param", buf.getvalue())
        self.assertIn("differs: VS_Same_0123abcd_Wrapped -> PS_Same_89abcdef_Wrapped: semantic param 1",
                      buf.getvalue())
        with open(out, "rb") as f:
            data = f.read()
        self.assertNotIn(b"\r\n", data)
        self.assertNotIn(b"VS_Same", data)                          # counts only
        self.assertEqual(json.loads(data)["pipelines"]["differ"], 1)
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.assertEqual(census.main([os.path.join(self.carve.dir, "nowhere")]), 2)
            self.assertEqual(census.main([self.carve.dir, "--json", os.path.join(self.carve.dir, "index.json")]), 2)
        self.assertEqual(err.getvalue().count("ERROR:"), 2)


if __name__ == "__main__":
    unittest.main()
