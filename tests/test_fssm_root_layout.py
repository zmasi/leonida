"""The root-layout and used-resource tables of an FSSM program record: tools/fssm_root_layout.py (root_tables()
and its command line) and tools/root_layout_census.py.

    python -I tests/test_fssm_root_layout.py

Zero game data: every blob here is built by hand from the layout in tools/fssm_root_layout.py's docstring, with
placeholder words for code. The numbers in the tables are made up to cover the entry kinds (constants, a root
descriptor, tables, an immediate sampler, the heaps, a spilled parameter), not copied from a shader.
"""
import json
import os
import random
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "tests"))

import fssm_root_layout  # noqa: E402
import fssm_unpack  # noqa: E402
import root_layout_census as census  # noqa: E402
from test_fssm_unpack import make_dxbc, make_xbch, make_xdat_list  # noqa: E402

DUMP = os.path.join(ROOT, "tools", "fssm_root_layout.py")
CENSUS = os.path.join(ROOT, "tools", "root_layout_census.py")
PAYLOAD = 0x10
RECORD = 0x54
FIRST_RECORD = 0xB0            # in the payload; any offset past the slot table would do
ABSENT = 0xDEADBEEFDEADBEEF
SLOTS = dict(fssm_root_layout.FSSM_STAGE_SLOTS)
CODE = struct.pack("<4I", 0xBF810000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000)   # s_endpgm and s_code_end padding
CONSTANTS, DESCRIPTOR, TABLE, IMMEDIATE_SAMPLER, RESOURCE_HEAP, SAMPLER_HEAP = 0, 1, 3, 4, 5, 6
SRV, SAMPLER, CBV, UAV = 0, 1, 2, 3


def param(root_type, user_data, sgprs, slot, register=0, table_offset=0, reserved=0):
    """A 20-byte root-layout entry."""
    return struct.pack("<IIIIBBH", root_type, table_offset, sgprs, register, user_data, slot, reserved)


def resource(kind, root_type, slot, user_data, table_offset=0, lower_bound=0, stride=0, dwords=8, pad=0, pad2=0):
    """A 20-byte used-resource entry."""
    return struct.pack("<BBHIIIBBBB", kind, root_type, pad, table_offset, lower_bound, stride, dwords, user_data,
                       slot, pad2)


def build(programs):
    """A synthetic FSSM blob -> (bytearray, {stage: blob offset of its record}).

    programs: {stage: (root-layout entries, used-resource entries)}. The records follow the slot table, then for
    each program its code, its root-layout table and its used-resource table. An empty table gets a count of 0 and
    the 0xDEADBEEFDEADBEEF offset."""
    stages = [stage for stage, _ in fssm_root_layout.FSSM_STAGE_SLOTS if stage in programs]
    payload = bytearray(FIRST_RECORD + 0x88 * len(stages))
    struct.pack_into("<Q", payload, 0, 6)
    for slot in range(0x08, 0x68, 8):
        struct.pack_into("<Q", payload, slot, ABSENT)
    records = {}
    for k, stage in enumerate(stages):
        layout, used = programs[stage]
        start = FIRST_RECORD + 0x88 * k
        struct.pack_into("<Q", payload, SLOTS[stage], start)
        struct.pack_into("<QQ", payload, start, len(payload), len(CODE))
        payload += CODE
        offsets = []
        for table in (layout, used):
            offsets.append(len(payload) if table else ABSENT)
            payload += b"".join(table)
        struct.pack_into("<QQII", payload, start + 0x28, offsets[0], offsets[1], len(layout), len(used))
        records[stage] = PAYLOAD + start
    blob = bytearray(b"FSSM" + struct.pack("<IQ", 0x101, len(payload)) + payload)
    return blob, records


def resize(blob, size):
    """The blob cut to `size` bytes, or grown with zeros, with the stored payload size made to match."""
    out = bytearray(blob[:size]) + bytes(max(0, size - len(blob)))
    if len(out) >= 16:
        struct.pack_into("<Q", out, 8, len(out) - PAYLOAD)
    return out


# A pixel program the way docs/d3d12_vulkan.md section 3.5 expects it: parameters in root order from s8, one dword
# per table, two per root descriptor. Slot 2 is a table with two ranges; slot 5 a static sampler with no user data.
PS_LAYOUT = [
    param(CONSTANTS, 8, 4, 0, register=0),
    param(DESCRIPTOR, 12, 2, 1, register=1),
    param(TABLE, 14, 1, 2, register=0),
    param(TABLE, 14, 1, 2, register=4, table_offset=32),
    param(TABLE, 15, 1, 3, register=0),
    param(RESOURCE_HEAP, 16, 1, 4),
    param(IMMEDIATE_SAMPLER, 0, 0, 5, register=2),
]
PS_USED = [
    resource(CBV, DESCRIPTOR, 1, 12, lower_bound=1, dwords=2),
    resource(SRV, TABLE, 2, 14, lower_bound=0, stride=8),
    resource(UAV, TABLE, 2, 14, table_offset=32, lower_bound=4, stride=8),
    resource(SAMPLER, TABLE, 3, 15, lower_bound=0, stride=4, dwords=4),
    resource(SAMPLER, IMMEDIATE_SAMPLER, 5, 0, lower_bound=2, dwords=4),
]
# A mesh program: the same root signature seen 8 system SGPRs later (user data 8 is s16).
MS_LAYOUT = [param(CONSTANTS, 8, 4, 0), param(TABLE, 14, 1, 2)]
MS_USED = [resource(SRV, TABLE, 2, 14, stride=8)]
# A compute program whose signature did not fit: two tables in registers from s4, then a root descriptor and
# constants from user-data dword 16 on, in memory behind the pointer in user data 0-1.
CS_LAYOUT = [
    param(TABLE, 4, 1, 0),
    param(TABLE, 5, 1, 1),
    param(DESCRIPTOR, 16, 2, 2, register=0),
    param(CONSTANTS, 18, 6, 3),
]
CS_USED = [resource(SRV, TABLE, 0, 4, stride=8), resource(SAMPLER, TABLE, 1, 5, dwords=4),
           resource(CBV, DESCRIPTOR, 2, 16, dwords=2)]


class RootTablesTest(unittest.TestCase):
    def test_reads_both_tables_of_every_program(self):
        blob, records = build({"ps": (PS_LAYOUT, PS_USED), "ms": (MS_LAYOUT, MS_USED)})
        programs = fssm_root_layout.root_tables(bytes(blob))
        self.assertEqual([p["stage"] for p in programs], ["ps", "ms"])           # slot order
        ps, ms = programs
        self.assertEqual((ps["record"], ms["record"]), (records["ps"], records["ms"]))
        self.assertEqual(ps["record"], PAYLOAD + FIRST_RECORD)
        self.assertEqual(bytes(blob[ps["code_offset"]:ps["code_offset"] + ps["code_size"]]), CODE)
        self.assertEqual(ps["layout_offset"], ps["code_offset"] + ps["code_size"])
        self.assertEqual(ps["used_offset"], ps["layout_offset"] + 20 * len(PS_LAYOUT))
        self.assertEqual(len(ps["layout"]), 7)
        self.assertEqual(ps["layout"][0], {"root_type": 0, "table_offset": 0, "user_sgprs": 4, "base_register": 0,
                                           "user_data": 8, "root_slot": 0, "reserved": 0})
        self.assertEqual(ps["layout"][3], {"root_type": 3, "table_offset": 32, "user_sgprs": 1, "base_register": 4,
                                           "user_data": 14, "root_slot": 2, "reserved": 0})
        self.assertEqual([e["root_type"] for e in ps["layout"]], [0, 1, 3, 3, 3, 5, 4])
        self.assertEqual(len(ps["used"]), 5)
        self.assertEqual(ps["used"][2], {"resource_type": 3, "root_type": 3, "table_offset": 32, "lower_bound": 4,
                                         "stride": 8, "descriptor_dwords": 8, "user_data": 14, "root_slot": 2,
                                         "reserved": 0})
        self.assertEqual(ps["used"][4]["root_type"], IMMEDIATE_SAMPLER)
        self.assertEqual(ms["layout"][1]["user_data"], 14)
        self.assertEqual(ms["used"], [{"resource_type": 0, "root_type": 3, "table_offset": 0, "lower_bound": 0,
                                       "stride": 8, "descriptor_dwords": 8, "user_data": 14, "root_slot": 2,
                                       "reserved": 0}])

    def test_entry_bytes_are_read_at_the_documented_offsets(self):
        # Spelled out byte by byte, so that a field swapped in both param() and the parser would still fail here.
        layout = bytes.fromhex("03000000" "30000000" "01000000" "07000000" "0a" "02" "0000")
        used = bytes.fromhex("03" "03" "0000" "30000000" "07000000" "08000000" "06" "0a" "02" "00")
        blob, records = build({"cs": ([layout], [used])})
        self.assertEqual(records["cs"], 0xC0)
        self.assertEqual(struct.unpack_from("<Q", blob, 0x10 + 0x60)[0], 0xB0)           # the compute slot
        self.assertEqual(struct.unpack_from("<II", blob, 0xC0 + 0x38), (1, 1))           # the two counts
        self.assertEqual(blob[0x10 + struct.unpack_from("<Q", blob, 0xC0 + 0x28)[0]:][:20], layout)
        self.assertEqual(blob[0x10 + struct.unpack_from("<Q", blob, 0xC0 + 0x30)[0]:][:20], used)
        (cs,) = fssm_root_layout.root_tables(bytes(blob))
        self.assertEqual(cs["layout"], [{"root_type": 3, "table_offset": 0x30, "user_sgprs": 1, "base_register": 7,
                                         "user_data": 10, "root_slot": 2, "reserved": 0}])
        self.assertEqual(cs["used"], [{"resource_type": 3, "root_type": 3, "table_offset": 0x30, "lower_bound": 7,
                                       "stride": 8, "descriptor_dwords": 6, "user_data": 10, "root_slot": 2,
                                       "reserved": 0}])

    def test_reads_a_spilled_compute_layout(self):
        blob, _ = build({"cs": (CS_LAYOUT, CS_USED)})
        (cs,) = fssm_root_layout.root_tables(bytes(blob))
        self.assertEqual(cs["stage"], "cs")
        self.assertEqual([(e["root_type"], e["user_data"], e["user_sgprs"]) for e in cs["layout"]],
                         [(3, 4, 1), (3, 5, 1), (1, 16, 2), (0, 18, 6)])
        self.assertEqual([(e["resource_type"], e["user_data"]) for e in cs["used"]], [(0, 4), (1, 5), (2, 16)])

    def test_user_data_dword_to_sgpr(self):
        self.assertEqual([fssm_root_layout.user_sgpr(stage, 8) for stage in ("vs", "ps", "cs")], [8, 8, 8])
        self.assertEqual([fssm_root_layout.user_sgpr(stage, 8) for stage in ("ls", "hs", "es", "gs", "ms")], [16] * 5)
        self.assertEqual(set(fssm_root_layout.SYSTEM_SGPRS), set(SLOTS))

    def test_an_empty_table_ignores_its_offset(self):
        blob, records = build({"vs": ([], [])})
        (vs,) = fssm_root_layout.root_tables(bytes(blob))
        self.assertEqual((vs["layout"], vs["used"], vs["layout_offset"], vs["used_offset"]), ([], [], None, None))
        struct.pack_into("<QQ", blob, records["vs"] + 0x28, 3, 1 << 63)          # nonsense offsets, counts still 0
        self.assertEqual(fssm_root_layout.root_tables(bytes(blob)), [vs])

    def test_a_blob_with_no_program_has_no_tables(self):
        blob, _ = build({})
        self.assertEqual(fssm_root_layout.root_tables(bytes(blob)), [])

    def test_unnamed_types_and_reserved_fields_are_kept_for_the_census(self):
        layout = [param(2, 8, 1, 0, reserved=0x1234), param(77, 9, 1, 1)]
        used = [resource(9, 2, 0, 8, pad=0xBEEF, pad2=0x5A)]
        blob, _ = build({"ps": (layout, used)})
        (ps,) = fssm_root_layout.root_tables(bytes(blob))
        self.assertEqual([(e["root_type"], e["reserved"]) for e in ps["layout"]], [(2, 0x1234), (77, 0)])
        self.assertEqual((ps["used"][0]["resource_type"], ps["used"][0]["reserved"]), (9, 0x5ABEEF))
        self.assertEqual(fssm_root_layout.root_type_name(3), "table")
        self.assertEqual(fssm_root_layout.root_type_name(77), "root77")
        self.assertEqual(fssm_root_layout.resource_type_name(2), "cbv")
        self.assertEqual(fssm_root_layout.resource_type_name(9), "resource9")


class MalformedTest(unittest.TestCase):
    def setUp(self):
        self.blob, self.records = build({"ps": (PS_LAYOUT, PS_USED)})
        self.record = self.records["ps"]
        self.layout_at, self.used_at = struct.unpack_from("<QQ", self.blob, self.record + 0x28)   # in the payload

    def refuse(self, blob, message):
        with self.assertRaises(fssm_root_layout.FssmLayoutError) as caught:
            fssm_root_layout.root_tables(bytes(blob))
        self.assertEqual(str(caught.exception), message)
        self.assertIsInstance(caught.exception, ValueError)

    def test_the_unchanged_blob_parses(self):
        self.assertEqual(len(fssm_root_layout.root_tables(bytes(self.blob))), 1)

    def test_header(self):
        self.refuse(b"", "truncated FSSM header")
        self.refuse(self.blob[:PAYLOAD + 0x67], "truncated FSSM header")
        self.refuse(b"XBCH" + self.blob[4:], "not an FSSM blob")
        blob = bytearray(self.blob)
        struct.pack_into("<I", blob, 4, 0x102)
        self.refuse(blob, "unknown FSSM version 0x102")
        self.refuse(self.blob + b"\0", "FSSM payload size disagrees with the blob size")
        self.refuse(self.blob[:-1], "FSSM payload size disagrees with the blob size")
        blob = bytearray(self.blob)
        struct.pack_into("<Q", blob, PAYLOAD, 7)
        self.refuse(blob, "unexpected FSSM slot count")

    def test_record_bounds(self):
        size = len(self.blob) - PAYLOAD
        for start in (0, 0x60, size - RECORD + 1, size, 1 << 40, ABSENT - 1):
            blob = bytearray(self.blob)
            struct.pack_into("<Q", blob, PAYLOAD + SLOTS["ps"], start)
            self.refuse(blob, "ps record lies outside the payload")
        blob = bytearray(self.blob)
        struct.pack_into("<Q", blob, PAYLOAD + SLOTS["cs"], FIRST_RECORD + RECORD - 1)   # a second record on the first
        self.refuse(blob, "program records overlap")
        blob = bytearray(self.blob)
        struct.pack_into("<Q", blob, PAYLOAD + SLOTS["vs"], FIRST_RECORD)                # two slots, one record
        self.refuse(blob, "program records overlap")

    def test_code_bounds(self):
        blob = bytearray(self.blob)
        struct.pack_into("<Q", blob, self.record + 8, len(self.blob))
        self.refuse(blob, "ps code runs past the payload")
        struct.pack_into("<QQ", blob, self.record, ABSENT, 0)
        self.refuse(blob, "ps code runs past the payload")

    def test_truncated_tables(self):
        # Cut inside the used-resource table, then inside the root-layout table: the counts now run past the end.
        used_end = PAYLOAD + self.used_at + 20 * len(PS_USED)
        self.assertEqual(used_end, len(self.blob))
        self.refuse(resize(self.blob, used_end - 1), "ps used-resource table runs past the payload")
        self.refuse(resize(self.blob, PAYLOAD + self.used_at + 1), "ps used-resource table runs past the payload")
        self.refuse(resize(self.blob, PAYLOAD + self.used_at - 1), "ps root-layout table runs past the payload")
        self.refuse(resize(self.blob, PAYLOAD + self.layout_at), "ps root-layout table runs past the payload")

    def test_count_overflow(self):
        for field, what in ((0x38, "root-layout"), (0x3C, "used-resource")):
            for count in (0xFFFFFFFF, 0x0CCCCCCD, len(self.blob)):   # 20 * 0x0CCCCCCD wraps to 4 in 32 bits
                blob = bytearray(self.blob)
                struct.pack_into("<I", blob, self.record + field, count)
                self.refuse(blob, f"ps {what} table runs past the payload")
        blob = bytearray(self.blob)
        struct.pack_into("<I", blob, self.record + 0x38, len(PS_LAYOUT) + 1)             # one entry too many
        self.refuse(blob, "ps root-layout and used-resource tables overlap")

    def test_offsets_past_the_payload(self):
        for field, what in ((0x28, "root-layout"), (0x30, "used-resource")):
            for offset in (ABSENT, 1 << 63, len(self.blob) - PAYLOAD - 19, 0xFFFFFFFFFFFFFFFF):
                blob = bytearray(self.blob)
                struct.pack_into("<Q", blob, self.record + field, offset)
                self.refuse(blob, f"ps {what} table runs past the payload")

    def test_overlapping_tables(self):
        code_at = struct.unpack_from("<Q", self.blob, self.record)[0]
        cases = (
            (0x28, 0x08, "ps root-layout table overlaps a program record"),            # in the slot table
            (0x28, FIRST_RECORD - 20 * len(PS_LAYOUT) + 1, "ps root-layout table overlaps a program record"),
            (0x30, FIRST_RECORD + RECORD - 1, "ps used-resource table overlaps a program record"),
            (0x28, code_at + len(CODE) - 1, "ps root-layout table overlaps the program's code"),
            (0x30, code_at - 1, "ps used-resource table overlaps the program's code"),   # starts in the gap
            (0x30, code_at + len(CODE) - 1, "ps used-resource table overlaps the program's code"),
            (0x30, self.layout_at, "ps root-layout and used-resource tables overlap"),
            (0x30, self.used_at - 1, "ps root-layout and used-resource tables overlap"),
            (0x28, self.used_at - 20 * len(PS_LAYOUT) + 1, "ps root-layout and used-resource tables overlap"),
        )
        for field, offset, message in cases:
            blob = resize(self.blob, len(self.blob) + 0x400)      # room, so that only the overlap is wrong
            struct.pack_into("<Q", blob, self.record + field, offset)
            self.refuse(blob, message)

    def test_a_table_may_touch_its_neighbours(self):
        # build() leaves 0x34 bytes between the record and the code: room for a two-entry table at either end.
        blob, records = build({"ms": (MS_LAYOUT, MS_USED)})
        code_at = struct.unpack_from("<Q", blob, records["ms"])[0]
        for offset in (FIRST_RECORD + RECORD, code_at - 20 * len(MS_LAYOUT)):
            struct.pack_into("<Q", blob, records["ms"] + 0x28, offset)
            (ms,) = fssm_root_layout.root_tables(bytes(blob))
            self.assertEqual(ms["layout_offset"], PAYLOAD + offset)
        blob = resize(self.blob, len(self.blob) + 20 * len(PS_USED))                     # the used table at the end
        struct.pack_into("<Q", blob, self.record + 0x30, len(self.blob) - PAYLOAD)
        (ps,) = fssm_root_layout.root_tables(bytes(blob))
        self.assertEqual(ps["used_offset"], len(self.blob))
        self.assertTrue(all(e == dict.fromkeys(e, 0) for e in ps["used"]))

    def test_no_prefix_and_no_corruption_crashes(self):
        """Whatever the bytes, the parser returns programs or raises FssmLayoutError: never another exception."""
        blob, _ = build({"gs": (PS_LAYOUT, PS_USED), "vs": (MS_LAYOUT, MS_USED), "cs": (CS_LAYOUT, CS_USED)})
        self.assertEqual(len(fssm_root_layout.root_tables(bytes(blob))), 3)

        def outcome(data):
            try:
                return len(fssm_root_layout.root_tables(bytes(data)))
            except fssm_root_layout.FssmLayoutError:
                return None

        for size in range(len(blob)):
            self.assertIsNone(outcome(blob[:size]), size)                    # the stored payload size disagrees
            outcome(resize(blob, size))                                      # ... and with the size made to match
        rng = random.Random(0x464D5353)
        header = PAYLOAD + FIRST_RECORD + 0x88 * 3                           # the slot table and the three records
        refused = 0
        for _ in range(4000):
            data = bytearray(blob)
            for _ in range(rng.randint(1, 4)):
                at = rng.randrange(8 if rng.random() < 0.9 else 0, header)
                data[at] = rng.choice((0x00, 0x01, 0x7F, 0x80, 0xFF, rng.randrange(256)))
            refused += outcome(data) is None
        self.assertGreater(refused, 100)


class DumpCliTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.src = os.path.join(self.tmp.name, "library")
        os.mkdir(self.src)
        self.good, _ = build({"ps": (PS_LAYOUT, PS_USED)})
        self.bad, records = build({"cs": (CS_LAYOUT, CS_USED)})
        struct.pack_into("<I", self.bad, records["cs"] + 0x38, 0xFFFFFFFF)
        ms, _ = build({"ms": (MS_LAYOUT, MS_USED)})
        self.write("good.fssm", self.good)
        self.write("bad.fssm", self.bad)
        self.write("good.dxbc", b"skipped: good.fssm is beside it")
        # One container with two payloads, as the two-payload VS containers have: one entry each, in order.
        self.write("pair.dxbc", make_dxbc([(b"DXIL", make_xdat_list([(0x0D, make_xbch(bytes(self.bad))),
                                                                      (0x0D, make_xbch(bytes(ms)))]))]))
        self.write("empty.dxbc", make_dxbc([(b"DXIL", b"a part without the directory")]))
        self.write("junk.dxbc", b"not a container")
        self.write("notes.txt", b"not read")

    def write(self, name, data):
        path = os.path.join(self.src, name)
        with open(path, "wb") as f:
            f.write(bytes(data))
        return path

    def run_tool(self, *args):
        return subprocess.run([sys.executable, "-I", DUMP, *args], capture_output=True, text=True, errors="replace")

    def check(self, layouts):
        self.assertEqual(sorted(layouts), ["bad.fssm", "empty.dxbc", "good.fssm", "junk.dxbc", "pair.dxbc"])
        self.assertEqual(layouts["good.fssm"], [{"programs": fssm_root_layout.root_tables(bytes(self.good))}])
        self.assertEqual(layouts["good.fssm"][0]["programs"][0]["layout"][1]["user_data"], 12)
        self.assertEqual(layouts["bad.fssm"], [{"error": "cs root-layout table runs past the payload"}])
        self.assertEqual(layouts["pair.dxbc"][0], {"error": "cs root-layout table runs past the payload"})
        self.assertEqual([p["stage"] for p in layouts["pair.dxbc"][1]["programs"]], ["ms"])
        self.assertEqual(layouts["empty.dxbc"], [{"error": "a container with no tag-0x0D payload"}])
        self.assertEqual(layouts["junk.dxbc"], [{"error": "not an FSSM blob or a container that unpacks"}])

    def test_out_writes_the_tables_and_prints_a_summary(self):
        before = sorted(os.listdir(self.src))
        out = os.path.join(self.tmp.name, "root_layout.json")
        result = self.run_tool(self.src, "--out", out)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(),
                         "root layout: 2 programs in 2 blobs of 5 files (9 root-layout and 6 used-resource entries), "
                         "not read: {'a container with no tag-0x0D payload': 1, "
                         "'cs root-layout table runs past the payload': 2, "
                         "'not an FSSM blob or a container that unpacks': 1}")
        self.assertEqual(result.stderr, "")
        with open(out, "rb") as f:
            raw = f.read()
        self.assertNotIn(b"\r", raw)
        self.assertEqual(raw.count(b"\n"), 7)                       # a file per line between the braces
        self.check(json.loads(raw))
        self.assertEqual(sorted(os.listdir(self.src)), before)      # nothing is written into the library

    def test_without_out_the_json_is_printed(self):
        result = self.run_tool(self.src)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.check(json.loads(result.stdout))
        self.assertTrue(result.stderr.startswith("root layout: 2 programs in 2 blobs of 5 files"), result.stderr)
        one = self.run_tool(os.path.join(self.src, "good.fssm"))
        self.assertEqual(one.returncode, 0, one.stderr)
        self.assertEqual(json.loads(one.stdout), {"good.fssm": [{"programs":
                                                                  fssm_root_layout.root_tables(bytes(self.good))}]})

    def test_missing_empty_and_unwritable(self):
        missing = os.path.join(self.tmp.name, "missing")
        result = self.run_tool(missing)
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stderr.strip(), f"ERROR: {missing} does not exist")
        empty = os.path.join(self.tmp.name, "empty")
        os.mkdir(empty)
        result = self.run_tool(empty)
        self.assertEqual((result.returncode, result.stdout), (0, ""))
        self.assertEqual(result.stderr.strip(), f"no input: {empty} holds no .fssm or .dxbc file")
        good = os.path.join(self.src, "good.fssm")
        result = self.run_tool(good, "--out", good)
        self.assertEqual(result.returncode, 2)
        self.assertIn("--out must not overwrite the input", result.stderr)
        with open(good, "rb") as f:
            self.assertEqual(f.read(), bytes(self.good))
        result = self.run_tool(good, "--out", os.path.join(self.tmp.name, "no", "such", "folder", "r.json"))
        self.assertEqual(result.returncode, 2)
        self.assertIn("ERROR: cannot read or write", result.stderr)
        self.assertEqual(self.run_tool().returncode, 2)             # the path is required


class NotATranslationSourceTest(unittest.TestCase):
    """The parser must stay out of the translator's inputs: a change to one of those makes every user translate
    their whole shader library again (make_game.TRANSLATION_SOURCES, runtime/cmake/translator_deps.cmake)."""

    def read(self, *parts):
        with open(os.path.join(ROOT, *parts), encoding="utf-8") as f:
            return f.read()

    def test_no_translation_input_names_the_new_tools(self):
        for parts in (("make_game.py",), ("runtime", "cmake", "translator_deps.cmake"),
                      ("tools", "fssm_unpack.py"), ("tools", "rdna2spirv.py"), ("tools", "shader_meta.py")):
            text = self.read(*parts)
            for name in ("fssm_root_layout", "root_layout_census"):
                self.assertNotIn(name, text, os.path.join(*parts))

    def test_fssm_unpack_has_no_root_table_code(self):
        self.assertFalse(hasattr(fssm_unpack, "root_tables"))
        self.assertNotIn("root-layout", self.read("tools", "fssm_unpack.py"))


def programs_of(table):
    """root_tables() of one synthetic blob per {stage: (layout, used)} dict in `table`."""
    out = []
    for programs in table:
        out += fssm_root_layout.root_tables(bytes(build(programs)[0]))
    return out


class CensusTest(unittest.TestCase):
    def test_the_console_layout_agrees_with_the_hypothesis(self):
        s = census.census(programs_of([{"ps": (PS_LAYOUT, PS_USED)}, {"ms": (MS_LAYOUT, MS_USED)},
                                       {"ps": ([], [])}]))
        ps, ms = s["stages"]["ps"], s["stages"]["ms"]
        self.assertEqual((ps["programs"], ps["programs_without_layout"]), (2, 1))
        self.assertEqual(ps["root_types"], {"constants": 1, "descriptor": 1, "immediate_sampler": 1,
                                            "resource_heap": 1, "table": 3})
        self.assertEqual(ps["used_kinds"], {"cbv/descriptor": 1, "sampler/immediate_sampler": 1, "sampler/table": 1,
                                            "srv/table": 1, "uav/table": 1})
        self.assertEqual(ps["dwords_per_parameter"], {"constants": {"4": 1}, "descriptor": {"2": 1},
                                                      "immediate_sampler": {"0": 1}, "resource_heap": {"1": 1},
                                                      "table": {"1": 3}})
        self.assertEqual(ps["lowest_sgpr"], {"s8": 1})
        self.assertEqual(ps["lowest_pointer_sgpr"], {"s12": 1})       # the root descriptor; the constants are not one
        self.assertEqual(ps["highest_sgpr"], {"s16": 1})
        self.assertEqual(ps["hypothesis"], {"base": 8, "agree": 1, "disagree": 0, "at_base": 1, "above_base": 0,
                                            "no_register_parameters": 1, "disagree_reasons": {}})
        self.assertEqual(ps["spill"], {"programs": 0, "start": {}, "pointer_dwords_free": 0})
        self.assertEqual(ms["lowest_sgpr"], {"s16": 1})               # user data 8 behind 8 system SGPRs
        self.assertEqual(ms["lowest_pointer_sgpr"], {"s22": 1})
        self.assertEqual(ms["hypothesis"]["agree"], 1)
        r = s["reading"]
        self.assertEqual(r["record_offsets"], {"0xc0": 3})
        self.assertEqual((r["layout_entries"], r["used_entries"]), (9, 6))
        self.assertEqual((r["layout_follows_code"], r["layout_not_after_code"]), (2, 0))
        self.assertEqual((r["used_follows_layout"], r["used_not_after_layout"]), (2, 0))
        self.assertEqual((r["used_with_layout_entry"], r["used_without_layout_entry"]), (5, 0))
        self.assertEqual((r["nonzero_reserved"], r["unknown_root_type"], r["unknown_resource_type"],
                          r["programs_past_s31"]), (0, 0, 0, 0))

    def test_the_pack_layout_disagrees(self):
        # rt::rootsig::pack: from s0, two dwords per table. Same signature as PS_LAYOUT's first four parameters.
        packed = [param(CONSTANTS, 0, 4, 0), param(DESCRIPTOR, 4, 2, 1), param(TABLE, 6, 2, 2), param(TABLE, 8, 2, 3)]
        h = census.census(programs_of([{"ps": (packed, [])}]))["stages"]["ps"]["hypothesis"]
        self.assertEqual((h["agree"], h["disagree"], h["at_base"], h["above_base"]), (0, 1, 0, 0))
        self.assertEqual(h["disagree_reasons"], {"a parameter below the base": 1, "a table that is not one dword": 1})
        # The same layout is what a base of s0 with two-dword tables would be, so only the table width is left.
        h = census.census(programs_of([{"ps": (packed, [])}]), {"ps": 0})["stages"]["ps"]["hypothesis"]
        self.assertEqual(h["disagree_reasons"], {"a table that is not one dword": 1})

    def test_each_reason_is_counted_once_per_program(self):
        table = [
            {"vs": ([param(TABLE, 9, 1, 0), param(TABLE, 10, 1, 1)], [])},                    # above the base: agrees
            {"vs": ([param(DESCRIPTOR, 8, 1, 0), param(DESCRIPTOR, 9, 1, 1)], [])},           # one-dword descriptors
            {"vs": ([param(TABLE, 9, 1, 0), param(TABLE, 8, 1, 1)], [])},                     # slot 1 before slot 0
            {"vs": ([param(TABLE, 8, 1, 0), param(CONSTANTS, 8, 1, 1)], [])},                 # two slots, one dword
            {"cs": ([param(TABLE, 2, 1, 0)], [])},                                            # below s4
            {"es": ([param(TABLE, 0, 1, 0)], [])},                                            # no base given: s8
        ]
        s = census.census(programs_of(table))["stages"]
        vs = s["vs"]["hypothesis"]
        self.assertEqual((vs["agree"], vs["disagree"], vs["at_base"], vs["above_base"]), (1, 3, 3, 1))
        self.assertEqual(vs["disagree_reasons"], {"a root descriptor that is not two dwords": 1,
                                                  "parameters not in root order": 2})
        self.assertEqual(s["cs"]["hypothesis"]["disagree_reasons"], {"a parameter below the base": 1})
        self.assertEqual(s["es"]["hypothesis"], {"base": None, "agree": 1, "disagree": 0, "at_base": 0,
                                                 "above_base": 0, "no_register_parameters": 0,
                                                 "disagree_reasons": {}})
        self.assertEqual(s["es"]["lowest_sgpr"], {"s8": 1})

    def test_spill_start(self):
        only_spilled = [param(CONSTANTS, 20, 4, 0)]
        pointer_taken = [param(CONSTANTS, 1, 1, 0), param(TABLE, 4, 1, 1), param(TABLE, 17, 1, 2)]
        table = [{"cs": (CS_LAYOUT, CS_USED)}, {"cs": (only_spilled, [])}, {"cs": (pointer_taken, [])},
                 {"ps": ([param(CONSTANTS, 20, 4, 0)], [])}]                 # s20 of a pixel program is a register
        s = census.census(programs_of(table))["stages"]
        self.assertEqual(s["cs"]["spill"], {"programs": 3, "start": {"16": 1, "17": 1, "20": 1},
                                            "pointer_dwords_free": 2})
        self.assertEqual(s["cs"]["lowest_sgpr"], {"s1": 1, "s4": 1})         # the spilled ones are not registers
        self.assertEqual(s["cs"]["highest_sgpr"], {"s4": 1, "s5": 1})
        self.assertEqual(s["cs"]["hypothesis"]["no_register_parameters"], 1)
        self.assertEqual(s["ps"]["spill"]["programs"], 0)
        self.assertEqual(s["ps"]["highest_sgpr"], {"s23": 1})

    def test_reading_checks_count_what_would_contradict_the_reading(self):
        layout = [param(2, 8, 1, 0, reserved=1), param(TABLE, 30, 4, 1)]                  # s30..s33
        used = [resource(SRV, TABLE, 1, 30, pad2=1), resource(7, TABLE, 3, 30), resource(CBV, DESCRIPTOR, 1, 8),
                resource(SAMPLER, 9, 0, 0)]
        blob, records = build({"ps": (layout, used)})
        blob = resize(blob + bytes(0x80) + b"".join(used), len(blob) + 0x80 + 20 * len(used))
        struct.pack_into("<Q", blob, records["ps"] + 0x30, len(blob) - PAYLOAD - 20 * len(used))   # a gap before it
        r = census.census(fssm_root_layout.root_tables(bytes(blob)))["reading"]
        self.assertEqual(r["nonzero_reserved"], 2)
        self.assertEqual(r["unknown_root_type"], 2)                 # root type 2 in the layout, 9 in the used table
        self.assertEqual(r["unknown_resource_type"], 1)
        self.assertEqual((r["used_with_layout_entry"], r["used_without_layout_entry"]), (1, 2))
        self.assertEqual(r["programs_past_s31"], 1)
        self.assertEqual((r["layout_follows_code"], r["used_follows_layout"], r["used_not_after_layout"]), (1, 0, 1))


class CensusCliTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.src = self.tmp.name

    def write(self, name, data):
        path = os.path.join(self.src, name)
        with open(path, "wb") as f:
            f.write(bytes(data))
        return path

    def run_tool(self, *args):
        return subprocess.run([sys.executable, "-I", CENSUS, *args], capture_output=True, text=True, errors="replace")

    def corpus(self):
        ps, _ = build({"ps": (PS_LAYOUT, PS_USED)})
        ms, _ = build({"ms": (MS_LAYOUT, MS_USED)})
        es, _ = build({"es": ([param(TABLE, 0, 1, 0)], [])})
        self.write("a.fssm", ps)
        self.write("a.dxbc", b"skipped: a.fssm is beside it")
        # One container with two payloads, as the two-payload VS containers have: both are read.
        self.write("b.dxbc", make_dxbc([(b"DXIL", make_xdat_list([(0x0D, make_xbch(bytes(es))),
                                                                   (0x0D, make_xbch(bytes(ms)))]))]))
        self.write("notes.txt", b"not read")

    def test_counts_a_folder_of_blobs_and_containers(self):
        self.corpus()
        out = os.path.join(self.src, "report.json")
        result = self.run_tool(self.src, "--json", out, "--expect-agree")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        lines = result.stdout.splitlines()
        self.assertEqual(lines[0], "3 blobs, 3 programs, blobs not read: {}")
        self.assertIn("ps: 1 programs (0 without a root-layout table); root types {'constants': 1, 'descriptor': 1, "
                      "'immediate_sampler': 1, 'resource_heap': 1, 'table': 3}", result.stdout)
        self.assertIn("  base: lowest user SGPR {'s8': 1}, lowest pointer SGPR {'s12': 1}, highest SGPR {'s16': 1}",
                      lines)
        self.assertIn("  section 3.5 hypothesis (base s8, one dword per table, root order): 1 agree (1 at the base, "
                      "0 above it), 0 disagree {}, 0 with no parameter in a register", lines)
        self.assertIn("  section 3.5 hypothesis (base none given, one dword per table, root order): 1 agree (0 at "
                      "the base, 0 above it), 0 disagree {}, 0 with no parameter in a register", lines)
        self.assertIn("reading checks: record offsets {'0xc0': 3}; root-layout table right after the code in 3 "
                      "programs, elsewhere in 0; used-resource table right after it in 2, elsewhere in 0", lines)
        self.assertNotIn("a.fssm", result.stdout)                   # counts only, no names
        with open(out, "rb") as f:
            raw = f.read()
        self.assertNotIn(b"\r", raw)
        report = json.loads(raw)
        self.assertEqual((report["blobs"], report["blobs_not_read"]), (3, {}))
        self.assertEqual(sorted(report["stages"]), ["es", "ms", "ps"])
        self.assertEqual(report["stages"]["ms"]["lowest_sgpr"], {"s16": 1})

    def test_one_file_and_base_override(self):
        path = self.write("a.fssm", build({"ps": (PS_LAYOUT, PS_USED)})[0])
        self.assertEqual(self.run_tool(path, "--expect-agree").returncode, 0)
        result = self.run_tool(path, "--base", "ps=9", "--expect-agree")
        self.assertEqual(result.returncode, 1)
        self.assertIn("MISMATCH ps: 1 programs disagree {'a parameter below the base': 1}", result.stderr)
        self.assertEqual(self.run_tool(path, "--base", "ps=9").returncode, 0)       # a census unless asked to check
        for bad in ("ps", "ps=x", "ps=-1", "xx=8"):
            result = self.run_tool(path, "--base", bad)
            self.assertEqual(result.returncode, 2)
            self.assertIn("expected STAGE=SGPR", result.stderr)

    def test_unreadable_blobs_are_counted_not_fatal(self):
        self.corpus()
        bad, records = build({"cs": (CS_LAYOUT, CS_USED)})
        struct.pack_into("<I", bad, records["cs"] + 0x3C, 0xFFFFFFFF)
        self.write("c.fssm", bad)
        self.write("d.fssm", b"FSSM")
        self.write("e.dxbc", b"DXBC" + bytes(40))
        self.write("f.dxbc", make_dxbc([(b"DXIL", b"no XDAT here")]))
        result = self.run_tool(self.src)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("3 blobs, 3 programs, blobs not read: {'a container with no tag-0x0D payload': 1, "
                      "'cs used-resource table runs past the payload': 1, "
                      "'not an FSSM blob or a container that unpacks': 1, 'truncated FSSM header': 1}", result.stdout)
        result = self.run_tool(self.src, "--expect-agree")
        self.assertEqual(result.returncode, 1)
        self.assertIn("MISMATCH 1 blobs not read: truncated FSSM header", result.stderr)

    def test_no_input(self):
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(result.stdout.startswith("no input: give the folder"), result.stdout)
        result = self.run_tool(self.src)                            # an empty folder
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), f"no input: {self.src} holds no .fssm or .dxbc file")
        self.write("notes.txt", b"FSSM is not read from a .txt in a folder")
        self.assertIn("no input", self.run_tool(self.src).stdout)
        self.assertEqual(self.run_tool(self.src, "--expect-agree").returncode, 1)   # nothing checked is not a pass
        self.assertEqual(self.run_tool("--expect-agree").returncode, 1)
        missing = os.path.join(self.src, "missing")
        result = self.run_tool(missing)
        self.assertEqual(result.returncode, 2)
        self.assertIn(f"ERROR: no input: {missing} does not exist", result.stderr)
        self.assertFalse(os.path.exists(missing))

    def test_json_must_not_overwrite_the_input(self):
        path = self.write("a.fssm", build({"ps": (PS_LAYOUT, PS_USED)})[0])
        result = self.run_tool(path, "--json", path)
        self.assertEqual(result.returncode, 2)
        self.assertIn("--json must not overwrite the input", result.stderr)
        with open(path, "rb") as f:
            self.assertEqual(f.read(4), b"FSSM")
        result = self.run_tool(path, "--json", os.path.join(self.src, "no", "such", "folder", "r.json"))
        self.assertEqual(result.returncode, 2)
        self.assertIn("ERROR: cannot write", result.stderr)


if __name__ == "__main__":
    unittest.main()
