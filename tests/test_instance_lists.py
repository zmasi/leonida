"""Synthetic-data tests for tools/instance_lists.py.

    python -I tests/test_instance_lists.py

Every byte here is built by the test: a minimal 0x7a resource with the four list structs, one prop list with
two instances and one grass list with three. No game file, value or name is used.
"""
import json
import math
import os
import random
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOL = os.path.join(ROOT, "tools", "instance_lists.py")
sys.path.insert(0, os.path.join(ROOT, "tools"))

import instance_lists as il  # noqa: E402

M = il.M


def members(spec):
    """Member table: 16 bytes each (name hash, offset, type byte, padding)."""
    return b"".join(struct.pack("<IIBBHI", M[n], off, ty, 0, 0, 0) for n, off, ty in spec)


SCHEMA = {
    il.PROP_LIST: (0x170, [("lodLevel", 0x7E, 0x60), ("lodDist", 0x60, 0x21), ("InstanceList", 0x90, 0x52),
                           ("BatchAABB", 0xA0, 0x05), ("ScaleRange", 0x160, 0x30), ("InstanceLodDist", 0x168, 0x21)]),
    il.PROP_INST: (0x40, [("InstMat", 0x0, 0x50), ("Tint", 0x34, 0x15)]),
    il.GRASS_LIST: (0x1A0, [("lodLevel", 0x7E, 0x60), ("lodDist", 0x60, 0x21), ("BatchAABB", 0x80, 0x05),
                            ("ScaleRange", 0xA0, 0x33), ("LodFadeStartDist", 0xB8, 0x21),
                            ("InstanceLodDist", 0xBC, 0x21), ("OrientToTerrain", 0xC0, 0x21),
                            ("IsSLODList", 0x150, 0x01), ("InstanceList", 0x178, 0x52)]),
    il.GRASS_INST: (0x10, [("Position", 0x0, 0x50), ("NormalX", 0x6, 0x11), ("NormalY", 0x7, 0x11),
                           ("Color", 0x8, 0x50), ("Scale", 0xB, 0x11), ("ScaleZ", 0xC, 0x11),
                           ("RotationIdx", 0xD, 0x11)]),
}


def prop_list():
    b = bytearray(0x170)
    b[0x7E] = 1
    struct.pack_into("<f", b, 0x60, 300.0)
    struct.pack_into("<4f4f", b, 0xA0, 0, 0, 0, 0, 10, 10, 10, 0)
    struct.pack_into("<2f", b, 0x160, 0.8, 1.2)
    struct.pack_into("<f", b, 0x168, 150.0)
    return b


def prop_instances():
    # identity at (1,2,3) and a 90-degree yaw scaled by 2 at (4,5,6)
    a = struct.pack("<12f", 1, 0, 0, 1, 0, 1, 0, 2, 0, 0, 1, 3) + b"\0" * 16
    c = struct.pack("<12f", 0, -2, 0, 4, 2, 0, 0, 5, 0, 0, 2, 6) + b"\0" * 16
    return a + c


def grass_list():
    b = bytearray(0x1A0)
    b[0x7E] = 5
    struct.pack_into("<f", b, 0x60, 120.0)
    struct.pack_into("<4f4f", b, 0x80, 0, 0, 0, 0, 5, 5, 1, 0)
    struct.pack_into("<2f", b, 0xA0, 0.5, 1.0)
    struct.pack_into("<3f", b, 0xB8, 80.0, 60.0, 1.0)
    b[0x150] = 1
    return b


def grass_instances():
    out = b""
    for i, (nx, ny, s, sz, r) in enumerate(((128, 128, 200, 200, 3), (255, 128, 100, 90, 71), (0, 0, 0, 0, 72))):
        out += struct.pack("<3H", 1000 * i, 2000, 3000) + bytes((nx, ny, 10, 20, 30, s, sz, r)) + b"\0\0"
    return out


def resource(*, rotation_first: bool = False) -> tuple[bytes, bytes]:
    """Virtual buffer (header, schema, lists) and physical buffer (instances) of one 0x7a resource."""
    V = bytearray(0x1000)
    V[0x10:0x15] = b"0DRPz"
    names = list(SCHEMA)
    struct.pack_into("<HHH", V, 0x48, len(names), 0, 4)
    st, it, mt = 0x100, 0x200, 0x300
    struct.pack_into("<Q", V, 0x28, 0x50000000 | st)
    struct.pack_into("<Q", V, 0x38, 0x50000000 | it)
    for i, n in enumerate(names):
        size, spec = SCHEMA[n]
        if n == il.GRASS_INST and rotation_first:
            spec = [(field, 0 if field == "RotationIdx" else offset + 1, kind)
                    for field, offset, kind in spec]
        table = members(spec)
        V[mt:mt + len(table)] = table
        o = st + 32 * i
        struct.pack_into("<I", V, o, n)
        struct.pack_into("<Q", V, o + 0x10, 0x50000000 | mt)
        struct.pack_into("<I", V, o + 0x18, size)
        struct.pack_into("<H", V, o + 0x1E, len(spec))
        mt += len(table)
    pl, gl = prop_list(), grass_list()
    pl_at, gl_at = 0x800, 0xA00
    grass = grass_instances()
    if rotation_first:
        # Move RotationIdx from byte 13 to byte 0 of each 16-byte record.
        grass = b"".join(grass[at + 13:at + 14] + grass[at:at + 13] + grass[at + 14:at + 16]
                         for at in range(0, len(grass), 16))
    P = bytearray(prop_instances() + grass)
    blocks = [(il.PROP_LIST, len(pl), 0x50000000 | pl_at), (il.GRASS_LIST, len(gl), 0x50000000 | gl_at),
              (il.PROP_INST, 0x80, 0x60000000), (il.GRASS_INST, 0x30, 0x60000000 | 0x80)]
    for i, (n, size, p) in enumerate(blocks):
        struct.pack_into("<IIQ", V, it + 16 * i, n, size, p)
    # arrays: low 12 bits = 1-based block index, bits 20+ = offset in that block, then a u16 count
    struct.pack_into("<QH", pl, 0x90, 3, 2)
    struct.pack_into("<QH", gl, 0x178, 4, 3)
    V[pl_at:pl_at + len(pl)] = pl
    V[gl_at:gl_at + len(gl)] = gl
    # block map at +0x08: one virtual and one physical stream
    struct.pack_into("<Q", V, 8, 0x50000000 | 0xF00)
    V[0xF00 + 8], V[0xF00 + 9] = 1, 1
    return bytes(V), bytes(P)


class PointerTests(unittest.TestCase):
    def test_instance_list_block_number_is_20_bits(self):
        # 4097 blocks: block 1 at V+0, block 4097 at V+0x100. A 12-bit block field would read 4097 as block 1.
        V = bytes(0x200)
        blocks = [(il.GRASS_INST, 0x10, 0x50000000)] * 4096 + [(il.GRASS_INST, 0x40, 0x50000100)]
        ref = struct.pack("<QH", 4097 | (0x10 << 20), 3)
        self.assertEqual(il.array(V, b"", blocks, ref, 0), (V, 0x110, 3))


class MeasureTests(unittest.TestCase):
    def setUp(self):
        self.V, self.P = resource()
        self.T = {}
        self.assertTrue(il.measure(self.V, self.P, self.T))

    def test_schema_matches(self):
        for name, ok in self.T["member_offset_matches"].items():
            self.assertEqual(ok, 1, name)
        self.assertEqual(set(self.T["struct_size_matches"].values()), {1})

    def test_prop_matrices(self):
        p = self.T["prop"]
        self.assertEqual((p["lists"], p["instances"]), (1, 2))
        self.assertEqual(p["matrix_orthonormal_axes"], 2)
        self.assertEqual(p["matrix_right_handed"], 2)
        self.assertEqual(p["matrix_uniform_scale"], 2)
        self.assertEqual(p["matrix_unit_scale"], 1)
        self.assertEqual(p["translation_in_aabb"], 2)
        self.assertEqual(p["lodLevel"], {"1": 1})

    def test_grass_fields(self):
        g = self.T["grass"]
        self.assertEqual((g["lists"], g["instances"]), (1, 3))
        self.assertEqual(g["IsSLODList"], {"1": 1})
        self.assertEqual(g["OrientToTerrain"], {"1.0": 1})
        self.assertEqual(g["normal_xy_inside_unit_disc"], 2)   # (0, 0) bytes is the corner (-1, -1)
        self.assertEqual(g["scalez_equals_scale"], 2)
        self.assertEqual(g["scale_zero"], 1)
        self.assertEqual(g["rotation_idx_max"], 72)
        self.assertEqual(g["instance_loddist_le_loddist"], 1)

    def test_grass_measure_uses_zero_rotation_offset(self) -> None:
        virtual, physical = resource(rotation_first=True)
        summary: dict = {}
        self.assertTrue(il.measure(virtual, physical, summary))
        self.assertEqual(summary["grass"]["rotation_idx_max"], 72)
        self.assertEqual(summary["grass"], self.T["grass"])

    def test_empty_list_is_counted(self):
        V = bytearray(self.V)
        struct.pack_into("<H", V, 0xA00 + 0x178 + 8, 0)
        T = {}
        il.measure(bytes(V), self.P, T)
        self.assertEqual(T["grass"]["lists_empty"], 1)

    def test_not_a_map_resource(self):
        self.assertFalse(il.measure(b"\0" * 0x100, b"", {}))

    def test_offset_zero_grass_member_is_preserved(self):
        structs = {
            il.GRASS_LIST: (0x1A0, {M["InstanceList"]: (0x178, 0x52)}),
            il.GRASS_INST: (0x10, {
                M["Scale"]: (0x0, 0x11),
                M["ScaleZ"]: (0x1, 0x11),
                M["NormalX"]: (0x6, 0x11),
                M["NormalY"]: (0x7, 0x11),
                M["RotationIdx"]: (0xD, 0x11),
            }),
        }
        gl = bytearray(0x1A0)
        struct.pack_into("<QH", gl, 0x178, 1, 1)
        inst = bytearray(0x10)
        inst[0x0] = 0       # Scale at offset 0 is 0
        inst[0x1] = 0       # ScaleZ at offset 1 is 0
        inst[0xB] = 99      # default Scale offset (0xB) holds a non-zero value
        inst[0xC] = 77
        blocks = [(il.GRASS_INST, 0x10, 0x60000000)]
        R = {}
        il.grass_list(bytes(gl), bytes(inst), blocks, structs, bytes(gl), 0, R)
        self.assertEqual(R["scale_zero"], 1)
        self.assertEqual(R["scalez_equals_scale"], 1)

    def test_nonzero_instmat_offset_past_buffer_counts_as_truncated_instance(self):
        structs = {
            il.PROP_LIST: (0x170, {M["InstanceList"]: (0x90, 0x52)}),
            il.PROP_INST: (0x40, {M["InstMat"]: (0x20, 0x50)}),
        }
        pl = bytearray(0x170)
        struct.pack_into("<QH", pl, 0x90, 1, 1)
        # 56 bytes is >= 48, but < 0x20 + 48 (80), so InstMat at +0x20 is truncated
        inst = b"\0" * 56
        blocks = [(il.PROP_INST, len(inst), 0x60000000)]
        R = {}
        il.prop_list(bytes(pl), inst, blocks, structs, bytes(pl), 0, R)
        self.assertEqual(R.get("instances_truncated"), 1)


class ArchiveTests(unittest.TestCase):
    def test_end_to_end(self):
        V, P = resource()
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "fake.rpf")
            first = zlib.compress(V)
            first += b"\0" * (-len(first) % 16)
            with open(path, "wb") as fh:
                fh.write(b"RPF9" + struct.pack("<I", 16) + b"\0" * 8 + first + zlib.compress(P))
            out = os.path.join(tmp, "out.json")
            r = subprocess.run([sys.executable, "-I", TOOL, path, "--json", out], capture_output=True, text=True, errors="replace")
            self.assertEqual(r.returncode, 0, r.stderr)
            with open(out, encoding="utf-8") as fh:
                T = json.load(fh)
            self.assertEqual(T["resources_with_lists"], 1)
            self.assertEqual(T["prop"]["instances"], 2)
            self.assertEqual(T["grass"]["instances"], 3)

    def test_bad_limit(self):
        r = subprocess.run([sys.executable, "-I", TOOL, TOOL, "--limit", "0"], capture_output=True, text=True, errors="replace")
        self.assertEqual(r.returncode, 2)

    @staticmethod
    def _archive(tmp):
        V, P = resource()
        path = os.path.join(tmp, "fake.rpf")
        first = zlib.compress(V)
        first += b"\0" * (-len(first) % 16)
        data = b"RPF9" + struct.pack("<I", 16) + b"\0" * 8 + first + zlib.compress(P)
        with open(path, "wb") as fh:
            fh.write(data)
        return path, data

    def test_json_over_an_input_archive_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            path, data = self._archive(tmp)
            other = os.path.join(tmp, ".", "fake.rpf")
            r = subprocess.run([sys.executable, "-I", TOOL, path, "--json", other], capture_output=True, text=True, errors="replace")
            self.assertEqual(r.returncode, 2, r.stderr)
            self.assertIn("would overwrite the input", r.stderr)
            self.assertNotIn("Traceback", r.stderr)
            self.assertEqual(r.stdout, "")
            with open(path, "rb") as fh:
                self.assertEqual(fh.read(), data)

    def test_json_that_is_a_directory_is_refused_before_the_scan(self):
        with tempfile.TemporaryDirectory() as tmp:
            path, _ = self._archive(tmp)
            r = subprocess.run([sys.executable, "-I", TOOL, path, "--json", tmp], capture_output=True, text=True, errors="replace")
            self.assertEqual(r.returncode, 2, r.stderr)
            self.assertIn("is a directory", r.stderr)
            self.assertNotIn("Traceback", r.stderr)
            self.assertEqual(r.stdout, "")

    def test_json_in_a_missing_folder_is_created(self):
        with tempfile.TemporaryDirectory() as tmp:
            path, _ = self._archive(tmp)
            out = os.path.join(tmp, "reports", "lists", "out.json")
            r = subprocess.run([sys.executable, "-I", TOOL, path, "--json", out], capture_output=True, text=True, errors="replace")
            self.assertEqual(r.returncode, 0, r.stderr)
            with open(out, encoding="utf-8") as fh:
                self.assertEqual(json.load(fh)["prop"]["instances"], 2)

    def test_json_that_cannot_be_written_is_one_error_line(self):
        with tempfile.TemporaryDirectory() as tmp:
            path, _ = self._archive(tmp)
            blocker = os.path.join(tmp, "blocker")
            with open(blocker, "w", encoding="utf-8") as fh:
                fh.write("a file, not a folder")
            out = os.path.join(blocker, "out.json")
            r = subprocess.run([sys.executable, "-I", TOOL, path, "--json", out], capture_output=True, text=True, errors="replace")
            self.assertEqual(r.returncode, 2, r.stderr)
            self.assertIn("cannot write", r.stderr)
            self.assertNotIn("Traceback", r.stderr)
            self.assertIn('"resources_with_lists"', r.stdout)

    def test_short_or_non_rpf_archives_are_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            cases = (
                ("empty.rpf", b"", "not an RPF9 archive"),
                ("short.rpf", b"RPF9", "not an RPF9 archive"),
                ("bad_magic.rpf", b"NOPE" + struct.pack("<I", 8) + b"\0" * 8, "not an RPF9 archive"),
                ("bad_start.rpf", b"RPF9" + struct.pack("<I", 4) + b"\0" * 8, "data start is outside"),
                ("past_end.rpf", b"RPF9" + struct.pack("<I", 64) + b"\0" * 8, "data start is outside"),
            )
            for name, payload, expected in cases:
                with self.subTest(name=name):
                    bad = os.path.join(tmp, name)
                    with open(bad, "wb") as fh:
                        fh.write(payload)
                    r = subprocess.run([sys.executable, "-I", TOOL, bad], capture_output=True, text=True, errors="replace")
                    self.assertEqual(r.returncode, 2, r.stderr)
                    self.assertIn(expected, r.stderr)
                    self.assertNotIn("Traceback", r.stderr)
                    self.assertEqual(r.stdout, "")

    def test_negative_or_non_integer_start_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            path, _ = self._archive(tmp)
            for bad in ("-1", "nope"):
                with self.subTest(start=bad):
                    r = subprocess.run([sys.executable, "-I", TOOL, path, "--start", bad],
                                       capture_output=True, text=True, errors="replace")
                    self.assertEqual(r.returncode, 2, r.stderr)
                    self.assertNotIn("<lambda>", r.stderr)
                    self.assertNotIn("Traceback", r.stderr)
                    self.assertEqual(r.stdout, "")


def _oracle_grass(ib, io, n, isz, nx, ny, sc, sz, rot):
    """The per-instance reading of a grass list, one record at a time."""
    need = max(isz, nx + 1, ny + 1, sc + 1, sz + 1, rot + 1)
    R = {"instances": n}
    rmax = 0
    for i in range(n):
        o = io + i * isz
        if min(nx, ny, sc, sz, rot) < 0 or o + need > len(ib):
            R["instances_truncated"] = R.get("instances_truncated", 0) + 1
            break
        x, y = ib[o + nx] / 127.5 - 1, ib[o + ny] / 127.5 - 1
        for k, v in (("normal_xy_inside_unit_disc", x * x + y * y <= 1.0 + 2 / 127.5),
                     ("scale_zero", ib[o + sc] == 0), ("scalez_equals_scale", ib[o + sz] == ib[o + sc])):
            R[k] = R.get(k, 0) + int(v)
        rmax = max(rmax, ib[o + rot])
    R["rotation_idx_max"] = rmax
    return R


def _oracle_prop(ib, io, n, isz, mat, box):
    R = {"instances": n}
    for i in range(n):
        o = io + i * isz
        if mat < 0 or o + max(48, mat + 48) > len(ib):
            R["instances_truncated"] = R.get("instances_truncated", 0) + 1
            break
        m = struct.unpack_from("<12f", ib, o + mat)
        cols = [(m[0], m[4], m[8]), (m[1], m[5], m[9]), (m[2], m[6], m[10])]
        t = (m[3], m[7], m[11])
        lens = [math.sqrt(sum(c * c for c in col)) for col in cols]
        if min(lens) <= 1e-6 or not all(math.isfinite(x) for x in m):
            R["matrix_degenerate"] = R.get("matrix_degenerate", 0) + 1
            continue
        u = [[c / l for c in col] for col, l in zip(cols, lens)]
        ortho = max(abs(sum(u[a][k] * u[b][k] for k in range(3))) for a, b in ((0, 1), (0, 2), (1, 2)))
        det = (u[0][0] * (u[1][1] * u[2][2] - u[1][2] * u[2][1]) - u[0][1] * (u[1][0] * u[2][2] - u[1][2] * u[2][0])
               + u[0][2] * (u[1][0] * u[2][1] - u[1][1] * u[2][0]))
        checks = [("matrix_orthonormal_axes", ortho < 0.01), ("matrix_right_handed", det > 0),
                  ("matrix_uniform_scale", max(lens) - min(lens) < 0.01 * max(lens)),
                  ("matrix_unit_scale", all(abs(x - 1) < 0.01 for x in lens))]
        if box:
            checks.append(("translation_in_aabb", all(box[0][k] - 0.01 <= t[k] <= box[1][k] + 0.01 for k in range(3))))
        for k, v in checks:
            R[k] = R.get(k, 0) + int(v)
    return R


def _random_matrices(rng, count, stride, at):
    out = bytearray(count * stride)
    for i in range(count):
        kind = rng.random()
        if kind < 0.05:
            m = [0.0] * 12
        elif kind < 0.08:
            m = [float("nan")] + [1.0] * 11
        elif kind < 0.5:
            m = [rng.uniform(-3, 3) for _ in range(12)]
        else:
            th, s = rng.uniform(0, 6.28), rng.choice((1.0, 1.0, 2.0, 0.5))
            c, sn = math.cos(th) * s, math.sin(th) * s
            m = [c, -sn, 0, rng.uniform(-6, 16), sn, c, 0, rng.uniform(-6, 16), 0, 0, s * rng.choice((1, 1, -1)),
                 rng.uniform(-6, 16)]
        struct.pack_into("<12f", out, i * stride + at, *m)
    return bytes(out)


class ManyInstancesTests(unittest.TestCase):
    """Lists of tens of thousands of instances: same counts as the one-record-at-a-time reading, much faster."""

    GRASS = {il.GRASS_LIST: (0x1A0, {M["InstanceList"]: (0x178, 0x52)}),
             il.GRASS_INST: (0x10, {M["NormalX"]: (6, 0x11), M["NormalY"]: (7, 0x11), M["Scale"]: (0xB, 0x11),
                                    M["ScaleZ"]: (0xC, 0x11), M["RotationIdx"]: (0xD, 0x11)})}

    def _grass(self, data, n, io=0):
        gl = bytearray(0x1A0)
        struct.pack_into("<QH", gl, 0x178, 1, n)
        R = {}
        blocks = [(il.GRASS_INST, len(data), 0x60000000)]
        il.grass_list(bytes(gl), bytes(data), blocks, self.GRASS, bytes(gl), 0, R)
        R.pop("lists"), R.pop("lodLevel")   # list-level fields, not instance counts
        return R

    def test_grass_counts_match_the_record_by_record_reading(self):
        rng = random.Random(7)
        data = bytes(rng.randrange(256) for _ in range(0x10 * 5000))
        for n, cut in ((5000, 0), (5000, 37), (5000, 0x10 * 5000 - 3), (3, 0), (1, 0x10 * 5000 - 5)):
            with self.subTest(n=n, cut=cut):
                buf = data[:len(data) - cut] if cut else data
                got = self._grass(buf, n)
                self.assertEqual(got, _oracle_grass(buf, 0, n, 0x10, 6, 7, 0xB, 0xC, 0xD))

    def test_prop_counts_match_the_record_by_record_reading(self):
        rng = random.Random(11)
        box = ((0.0,) * 3, (10.0,) * 3)
        structs = {il.PROP_LIST: (0x170, {M["InstanceList"]: (0x90, 0x52), M["BatchAABB"]: (0xA0, 0x05)}),
                   il.PROP_INST: (0x40, {M["InstMat"]: (0x4, 0x50)})}
        data = _random_matrices(rng, 3000, 0x40, 4)
        # The last record has 12 padding bytes after its matrix. Only cutting into
        # the matrix itself makes that instance incomplete.
        for n, cut in ((3000, 0), (3000, 1), (3000, 12), (3000, 13), (3000, 1000), (2, 0)):
            with self.subTest(n=n, cut=cut):
                pl = bytearray(0x170)
                struct.pack_into("<QH", pl, 0x90, 1, n)
                struct.pack_into("<4f4f", pl, 0xA0, *box[0], 0, *box[1], 0)
                buf = data[:len(data) - cut]
                R = {}
                il.prop_list(bytes(pl), buf, [(il.PROP_INST, len(buf), 0x60000000)], structs, bytes(pl), 0, R)
                R.pop("lists", None), R.pop("lodLevel", None), R.pop("aabb_valid", None)
                self.assertEqual(R, _oracle_prop(buf, 0, n, 0x40, 4, box))

    def test_prop_handedness_preserves_reference_rounding(self):
        # Row three is three times row one. Rounding the column lengths differently
        # can change the sign of the computed determinant at zero.
        data = struct.pack("<12f", 6, 1, 1, 0, 3 * 2**26, 0, 2, 0, 18, 3, 3, 0)
        header = struct.pack("<QH", 1, 1)
        structs = {il.PROP_LIST: (len(header), {M["InstanceList"]: (0, 0x52)}),
                   il.PROP_INST: (48, {})}
        counts = {}
        il.prop_list(header, data, [(il.PROP_INST, len(data), 0x60000000)], structs, header, 0, counts)
        reference = _oracle_prop(data, 0, 1, 48, 0, None)
        self.assertEqual(counts["matrix_right_handed"], reference["matrix_right_handed"])

    def test_a_full_grass_list_is_read_much_faster_than_record_by_record(self):
        rng = random.Random(3)
        n = 60000
        # Near-upright normals, as in real lists: a few distinct (NormalX, NormalY) pairs.
        data = b"".join(bytes((0, 0, 0, 0, 0, 0, 120 + rng.randrange(16), 120 + rng.randrange(16),
                               1, 2, 3, rng.randrange(256), rng.randrange(256), rng.randrange(73), 0, 0))
                        for _ in range(n))

        def best(fn):
            times = []
            for _ in range(3):
                t0 = time.perf_counter()
                fn()
                times.append(time.perf_counter() - t0)
            return min(times)

        fast = best(lambda: self._grass(data, n))
        slow = best(lambda: _oracle_grass(data, 0, n, 0x10, 6, 7, 0xB, 0xC, 0xD))
        self.assertLess(fast * 4, slow, f"bulk {fast:.4f}s vs record-by-record {slow:.4f}s")


if __name__ == "__main__":
    unittest.main()
