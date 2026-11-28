"""Synthetic BVHs for tools/xdxr_bvh.py: layout, the traversal step, and closest hits against brute force.
No game data is read.

    python -I tests/test_xdxr_bvh.py
"""
import contextlib
import io
import math
import os
import random
import struct
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import xdxr_bvh as X  # noqa: E402

TRI = [[0.0, 0.0, 0.0], [1.0, 0.0, 0.0], [0.0, 1.0, 0.0]]


def rays(rng, n, spread=6.0, aim=3.0):
    for _ in range(n):
        o = [rng.uniform(-spread, spread) for _ in range(3)]
        t = [rng.uniform(-aim, aim) for _ in range(3)]
        yield o, [t[c] - o[c] for c in range(3)]


def same_hit(a, b):
    if a is None or b is None:
        return a is None and b is None
    return (a["primitive"] == b["primitive"] and a["instance"] == b["instance"]
            and math.isclose(a["t"], b["t"], rel_tol=1e-9, abs_tol=1e-12)
            and all(math.isclose(a["bary"][k], b["bary"][k], abs_tol=1e-9) for k in range(2)))


class Layout(unittest.TestCase):
    def test_pointer_encoding(self):
        self.assertEqual(X.ptr(0x9B640 - 0x100 + 0x100 - 0x9B640 + 9941 * 64, X.BOX16), 0x136AC)  # docs/bvh.md
        self.assertEqual(X.ptr_offset(0x136AC), 9941 * 64)
        self.assertEqual(X.ptr_type(0x136AC), X.BOX16)
        with self.assertRaises(ValueError):
            X.ptr(32, X.TRI0)
        for bad_off, bad_kind in ((-64, X.BOX16), (False, X.TRI0), (64, -1), (64, 8), (64, True)):
            with self.subTest(offset=bad_off, kind=bad_kind):
                with self.assertRaises(ValueError):
                    X.ptr(bad_off, bad_kind)

    def test_header_fields(self):
        data, info = X.build_blas([TRI])
        flags, units, crc, n, *rest = struct.unpack_from("<4I6fIiIIII", data, 0)
        self.assertEqual(flags, X.FLAG_BOTTOM_LEVEL | X.VERSION_2403)
        self.assertEqual((units + 1) * 64, len(data))
        self.assertEqual(n, 1)
        self.assertEqual(rest[0:6], [0.0, 0.0, 0.0, 1.0, 1.0, 0.0])
        nodes, _remap, root, elements, parents, cp = rest[6:]
        self.assertEqual(root, info["root"])
        self.assertEqual(X.ptr_type(root), X.BOX32)
        self.assertEqual(X.ptr_offset(root), nodes)
        self.assertEqual(elements % 64, 0)
        self.assertEqual(parents, 64)
        self.assertEqual(cp, 0)

    def test_leaf_record(self):
        data, info = X.build_blas([TRI], geometry_index=5, flags=0, opaque=True)
        off = info["elements"]
        self.assertEqual(struct.unpack_from("<9f", data, off), (0, 0, 0, 1, 0, 0, 0, 1, 0))
        gif, base, prim2, remap = struct.unpack_from("<4I", data, off + 48)
        self.assertEqual(gif, 5 << 8 | 1)
        self.assertEqual((base, prim2), (0, X.INVALID))
        self.assertEqual(remap & 0xFF, X.REMAP_IDENTITY)

    def test_unused_children_miss(self):
        data, info = X.build_blas([TRI])
        root = X.ptr_offset(info["root"])
        self.assertEqual(struct.unpack_from("<4I", data, root)[1:], (X.INVALID,) * 3)
        lo, hi = struct.unpack_from("<3f", data, root + 40), struct.unpack_from("<3f", data, root + 52)
        self.assertTrue(all(lo[c] > hi[c] for c in range(3)))

    def test_box16_is_conservative(self):
        rng = random.Random(7)
        for _ in range(2000):
            lo = [rng.uniform(-70000, 70000) for _ in range(3)]
            hi = [x + rng.uniform(0, 3) for x in lo]
            dlo, dhi = X.decode_box16(X.encode_box16(lo, hi))
            for c in range(3):
                self.assertLessEqual(dlo[c], lo[c])
                self.assertGreaterEqual(dhi[c], hi[c])

    def test_half_round_edges(self):
        # runtime/src/xdxr_build.comp half_directed computes the same bits; check-build compares the GPU's BOX16
        # words with encode_box16, which uses this function
        cases = [
            (0.0, False, 0x0000), (0.0, True, 0x0000), (-0.0, False, 0x8000), (-0.0, True, 0x8000),
            (1.0, False, 0x3C00), (1.0, True, 0x3C00), (1.0001, False, 0x3C00), (1.0001, True, 0x3C01),
            (-1.0001, False, 0xBC01), (-1.0001, True, 0xBC00),
            (65504.0, True, 0x7BFF), (65505.0, False, 0x7BFF), (65505.0, True, 0x7C00),
            (-65505.0, False, 0xFC00), (-65505.0, True, 0xFBFF),
            (math.inf, False, 0x7BFF), (-math.inf, True, 0xFBFF),          # an empty box stays min > max
            (2.0 ** -14, False, 0x0400), (2.0 ** -14, True, 0x0400),
            (3e-5, False, 0x0000), (3e-5, True, 0x0400), (-3e-5, False, 0x8400), (-3e-5, True, 0x8000),
            (1e-30, True, 0x0400), (-1e-30, False, 0x8400),
        ]
        for x, up, want in cases:
            self.assertEqual(X.half_round(x, up), want, (x, up))

    def test_half_round_never_denormal(self):
        rng = random.Random(3)
        for _ in range(4000):
            x = math.copysign(2.0 ** rng.uniform(-40, -10), rng.choice((-1, 1)))
            for up in (False, True):
                b = X.half_round(x, up)
                self.assertFalse(b & 0x7C00 == 0 and b & 0x3FF, f"{x} -> denormal {b:#06x}")
                v = X._half_value(b)
                self.assertTrue(v >= x if up else v <= x)

    def test_box16_rounding_check(self):
        data, _ = X.build_blas(X.random_scene(random.Random(4), 60), box16=True)
        self.assertEqual(X.box16_rounding(data), [])
        bad = bytearray(data)
        root = X.ptr_offset(struct.unpack_from("<I", data, 48)[0])
        w = struct.unpack_from("<I", bad, root + 16)[0]
        struct.pack_into("<I", bad, root + 16, w ^ 1)              # one half one step off
        self.assertTrue(X.box16_rounding(bytes(bad)))

    def test_parent_table(self):
        data, info = X.build_blas(X.random_scene(random.Random(2), 40))
        self.assertEqual(X.validate(data), [])
        root_unit = (X.ptr_offset(info["root"]) - info["elements"]) // 64
        self.assertEqual(struct.unpack_from("<I", data, info["parents"] + 4 * root_unit)[0], X.INVALID)


class Step(unittest.TestCase):
    def test_triangle_hit_values(self):
        t_num, t_den, i, j = X.triangle_test(TRI, [0.25, 0.25, -2.0], [0.0, 0.0, 1.0])
        self.assertAlmostEqual(t_num / t_den, 2.0)
        self.assertAlmostEqual(i / t_den, 0.25)
        self.assertAlmostEqual(j / t_den, 0.25)
        self.assertEqual(X.triangle_test(TRI, [2.0, 2.0, -2.0], [0.0, 0.0, 1.0])[0], math.inf)
        self.assertEqual(X.triangle_test(TRI, [0.25, 0.25, 2.0], [0.0, 0.0, 1.0])[0], math.inf)   # behind

    def test_remap_selects_edge_functions(self):
        r = [X.triangle_test(TRI, [0.1, 0.3, -1.0], [0.0, 0.0, 1.0], remap)
             for remap in (1 | 2 << 2, 2 | 1 << 2, 0 | 1 << 2)]
        bary = [(x[2] / x[1], x[3] / x[1]) for x in r]
        self.assertAlmostEqual(bary[0][0], 0.1)
        self.assertAlmostEqual(bary[0][1], 0.3)
        self.assertAlmostEqual(bary[1][0], 0.3)
        self.assertAlmostEqual(bary[2][0], 0.6)

    def test_triangle_barycentrics_independent_of_ray_scale_and_direction(self):
        # Non-unit and diagonal rays (and scaled TLAS rays) have |dd[2]| != 1; i_num/t_den and j_num/t_den
        # must still equal the true barycentrics (0.25, 0.25) and reconstruct the 3D hit point org + t * d.
        target = [0.25, 0.25, 0.0]
        for d in ([0.0, 0.0, 2.0], [0.0, 0.0, -2.0], [0.5, 0.5, 0.5], [-0.4, -0.3, -0.5], [1.0, 2.0, -3.0]):
            org = [target[c] - 1.5 * d[c] for c in range(3)]
            with self.subTest(d=d):
                t_num, t_den, i_num, j_num = X.triangle_test(TRI, org, d)
                self.assertNotEqual(t_num, math.inf)
                t = t_num / t_den
                b1, b2 = i_num / t_den, j_num / t_den
                self.assertAlmostEqual(t, 1.5, places=12)
                self.assertAlmostEqual(b1, 0.25, places=12)
                self.assertAlmostEqual(b2, 0.25, places=12)
                b0 = 1.0 - b1 - b2
                for c in range(3):
                    rebuilt = b0 * TRI[0][c] + b1 * TRI[1][c] + b2 * TRI[2][c]
                    self.assertAlmostEqual(rebuilt, org[c] + t * d[c], places=12)

    def test_triangle_edge_hit_with_zero_i_and_negative_major_axis_keeps_positive_j(self):
        # Point (0.0, 0.25, 0.0) lies on edge v0-v2 (b1 == 0, b2 == 0.25). With dd[2] < 0, t_den < 0;
        # j_num must share t_den's sign even when i_num == 0 so j_num / t_den is +0.25, not -0.25.
        for dz in (-1.0, -2.5):
            with self.subTest(dz=dz):
                t_num, t_den, i_num, j_num = X.triangle_test(TRI, [0.0, 0.25, 1.0], [0.0, 0.0, dz])
                self.assertNotEqual(t_num, math.inf)
                self.assertAlmostEqual(i_num / t_den, 0.0, places=12)
                self.assertAlmostEqual(j_num / t_den, 0.25, places=12)

    def test_tri1_reads_slots_1_3_2(self):
        quad = [[[0, 0, 0], [1, 0, 0], [1, 1, 0]], [[0, 0, 0], [1, 1, 0], [0, 1, 0]]]
        data, info = X.build_blas(quad, pair=True)
        self.assertEqual(info["leaves"], 1)
        mem = X.Memory()
        mem.place(0x1000, data)
        leaf = info["elements"]
        p1 = X.ptr(leaf, X.TRI1)
        o, d = [0.2, 0.7, -1.0], [0.0, 0.0, 1.0]
        res = X.intersect_node(mem, 0x1000, p1, o, d, X._inverse_dir(d), math.inf)
        self.assertNotEqual(res[0], math.inf)
        hit = X.trace(mem, 0x1000, o, d)
        self.assertEqual(hit["primitive"], 1)
        want = X.brute_force([(None, 0xFF, quad)], o, d)
        self.assertTrue(same_hit(hit, want), (hit, want))

    def test_box_step_sorts_and_culls(self):
        # four triangles: three stacked on the ray at z = 3, 1, 2 and one off to the side
        tris = [[[0, 0, z], [1, 0, z], [0, 1, z]] for z in (3.0, 1.0, 2.0)] + [[[10, 0, 1], [11, 0, 1], [10, 1, 1]]]
        data, info = X.build_blas(tris)
        mem = X.Memory()
        mem.place(0, data)
        o, d = [0.2, 0.2, 0.0], [0.0, 0.0, 1.0]
        kids = X.intersect_node(mem, 0, info["root"], o, d, X._inverse_dir(d), math.inf)
        prim = lambda p: struct.unpack_from("<I", data, X.ptr_offset(p) + 52)[0]
        self.assertEqual(kids[3], X.INVALID)
        self.assertEqual([prim(p) for p in kids[:3]], [1, 2, 0])          # nearest first
        unsorted = X.intersect_node(mem, 0, info["root"], o, d, X._inverse_dir(d), math.inf, sort=False)
        self.assertEqual(sorted(unsorted), sorted(kids))
        culled = X.intersect_node(mem, 0, info["root"], o, d, X._inverse_dir(d), 1.5)
        self.assertEqual([prim(p) for p in culled if p != X.INVALID], [1])   # boxes beyond the extent miss


class ClosestHit(unittest.TestCase):
    def check_blas(self, tris, **kw):
        rng = random.Random(11)
        data, info = X.build_blas(tris, **kw)
        self.assertEqual(X.validate(data, expect_bottom=True), [])
        mem = X.Memory()
        mem.place(0x40000, data)
        hits = 0
        for o, d in rays(rng, 600):
            a = X.trace(mem, 0x40000, o, d)
            b = X.brute_force([(None, 0xFF, tris)], o, d)
            self.assertTrue(same_hit(a, b), (o, d, a, b))
            hits += a is not None
        self.assertGreater(hits, 30)

    def test_random_box32(self):
        self.check_blas(X.random_scene(random.Random(1), 80))

    def test_random_box16(self):
        self.check_blas(X.random_scene(random.Random(1), 80), box16=True)

    def test_grid_paired(self):
        self.check_blas(X.grid_mesh(), pair=True)

    def test_grid_paired_box16(self):
        self.check_blas(X.grid_mesh(), pair=True, box16=True)

    def test_tlas(self):
        rng = random.Random(5)
        mesh_a, mesh_b = X.grid_mesh(4, 2.0), X.random_scene(rng, 30, 1.0)
        blas_a, ia = X.build_blas(mesh_a, pair=True)
        blas_b, ib = X.build_blas(mesh_b, box16=True)
        mem = X.Memory()
        va_a, va_b = mem.place(0x100000, blas_a), mem.place(0x200000, blas_b)
        c, s = math.cos(0.7), math.sin(0.7)
        xforms = [
            ([[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0]], va_a, ia, 0x01),
            ([[c, -s, 0, 3], [s, c, 0, -1], [0, 0, 1, 0.5]], va_a, ia, 0x02),
            ([[2, 0, 0, -3], [0, 0.5, 0, 2], [0, 0, 1.5, 1]], va_b, ib, 0x01),
            ([[c, 0, s, 0], [0, 1, 0, 3], [-s, 0, c, -2]], va_b, ib, 0x04),
        ]
        insts = [X.Instance(m, va, info["root"], info["bounds"], instance_id=10 + k, mask=mask)
                 for k, (m, va, info, mask) in enumerate(xforms)]
        tlas, it = X.build_tlas(insts)
        self.assertEqual(X.validate(tlas, expect_bottom=False), [])
        va_t = mem.place(0x300000, tlas)
        rec = X.ptr_offset(struct.unpack_from("<I", tlas, 48)[0])
        self.assertEqual(it["instances"], 4)
        scene = [(m, mask, mesh_a if va == va_a else mesh_b) for m, va, _, mask in xforms]
        hits = 0
        for mask in (0xFF, 0x01, 0x06):
            for o, d in rays(rng, 300, spread=8.0, aim=4.0):
                a = X.trace(mem, va_t, o, d, mask=mask)
                b = X.brute_force(scene, o, d, mask=mask)
                if a is not None:
                    hits += 1
                    # world t survives the object-space transform; tolerance for the float32 inverse matrix
                    self.assertIsNotNone(b)
                    self.assertEqual((a["instance"], a["primitive"]), (b["instance"], b["primitive"]))
                    self.assertTrue(math.isclose(a["t"], b["t"], rel_tol=1e-5))
                else:
                    self.assertIsNone(b)
        self.assertGreater(hits, 50)
        self.assertGreater(rec, 0)

    def test_tlas_bounds_and_inverse_use_the_stored_transform(self) -> None:
        triangles = [[(1, 0, 0), (2, 0, 0), (1, 1, 0)]]
        blas, info = X.build_blas(triangles)
        mem = X.Memory()
        blas_va = mem.place(0x100000, blas)
        # The x scale rounds down in float32. Its translation then places
        # the triangle's left edge at x = 0 rather than x = 1.
        matrix = [[16777217, 0, 0, -16777216], [0, 1, 0, 0], [0, 0, 1, 0]]
        instance = X.Instance(matrix, blas_va, info["root"], info["bounds"])
        tlas, metadata = X.build_tlas([instance])
        root = X.ptr_offset(metadata["root"])
        instance_offset = X.ptr_offset(struct.unpack_from("<I", tlas, root)[0])
        stored_x = struct.unpack_from("<4f", tlas, instance_offset)
        self.assertEqual(stored_x, (16777216.0, 0.0, 0.0, -16777216.0))
        self.assertEqual(struct.unpack_from("<6f", tlas, root + 16),
                         (0.0, 0.0, 0.0, 16777216.0, 1.0, 0.0))
        self.assertEqual(struct.unpack_from("<4f", tlas, instance_offset + 64),
                         (2.0 ** -24, 0.0, 0.0, 1.0))
        tlas_va = mem.place(0x200000, tlas)
        hit = X.trace(mem, tlas_va, (0.25, 0.25, -1.0), (0.0, 0.0, 1.0))
        self.assertIsNotNone(hit)
        self.assertEqual(hit["primitive"], 0)

    def test_instance_record(self):
        data, info = X.build_blas([TRI])
        inst = X.Instance([[1, 0, 0, 5], [0, 1, 0, 0], [0, 0, 1, 0]], 0x12300, info["root"], info["bounds"],
                          instance_id=0x123456, mask=0xAB, contribution=7, flags=0x3)
        tlas, _ = X.build_tlas([inst])
        off = X.ptr_offset(struct.unpack_from("<4I", tlas, X.ptr_offset(struct.unpack_from("<I", tlas, 48)[0]))[0])
        id_mask, contrib, internal, blas = struct.unpack_from("<4I", tlas, off + 48)
        self.assertEqual(id_mask, 0xAB123456)
        self.assertEqual(contrib, 0x03000007)
        self.assertEqual(internal, info["root"] | X.INST_IDENTITY_ROTATION)
        self.assertEqual(blas, 0x123)
        self.assertEqual(struct.unpack_from("<4f", tlas, off + 64), (1.0, 0.0, 0.0, -5.0))
        with self.assertRaises(ValueError):
            X.build_tlas([X.Instance(inst.transform, 0x12340, info["root"], info["bounds"])])
        for bad_va in (-256, 1 << 40, False, True):
            with self.subTest(blas_va=bad_va):
                with self.assertRaises(ValueError):
                    X.build_tlas([X.Instance(inst.transform, bad_va, info["root"], info["bounds"])])

    def test_inactive_nan_triangles_produce_empty_boxes_and_valid_blas(self):
        # D3D12 marks inactive triangles with a NaN vertex (e.g. v0.x = NaN); xdxr_build.comp gives them an empty
        # box (+inf, -inf) and never pairs them. Both 1-level and multi-level builds must validate and trace cleanly.
        nan_sharing_edge = [[math.nan, 0.0, 0.0], [1.0, 0.0, 0.0], [0.0, 0.0, 0.0]]
        active = [[[2.0 * i, 0.0, 0.0], [2.0 * i + 1.0, 0.0, 0.0], [2.0 * i, 1.0, 0.0]] for i in range(6)]
        tris = [nan_sharing_edge, *active]
        for box16 in (False, True):
            for pair in (False, True):
                with self.subTest(box16=box16, pair=pair):
                    data, info = X.build_blas(tris, box16=box16, pair=pair)
                    self.assertEqual(X.validate(data, expect_bottom=True), [])
                    self.assertEqual(X.box16_rounding(data), [])
                    self.assertEqual(info["bounds"], ([0.0, 0.0, 0.0], [11.0, 1.0, 0.0]))
                    if pair:
                        self.assertEqual(X.count_pairs(data), 0)
                    mem = X.Memory()
                    mem.place(0x10000, data)
                    for i, tri in enumerate(active, start=1):
                        cx = sum(p[0] for p in tri) / 3.0
                        cy = sum(p[1] for p in tri) / 3.0
                        o, d = [cx - 0.5, cy - 0.5, 1.0], [0.5, 0.5, -1.0]
                        got = X.trace(mem, 0x10000, o, d)
                        want = X.brute_force([(None, 0xFF, tris)], o, d)
                        self.assertTrue(same_hit(got, want), (i, got, want))
                        self.assertEqual(got["primitive"], i)

    def test_all_inactive_triangles_blas_is_valid_and_misses(self):
        tris = [[[math.nan, float(i), 0.0], [1.0, 0.0, 0.0], [0.0, 1.0, 0.0]] for i in range(6)]
        for box16 in (False, True):
            with self.subTest(box16=box16):
                data, info = X.build_blas(tris, box16=box16, pair=True)
                self.assertEqual(X.validate(data, expect_bottom=True), [])
                self.assertEqual(X.box16_rounding(data), [])
                self.assertEqual(info["bounds"], ([math.inf] * 3, [-math.inf] * 3))
                self.assertEqual(X.count_pairs(data), 0)
                mem = X.Memory()
                mem.place(0x10000, data)
                self.assertIsNone(X.trace(mem, 0x10000, [0.25, 0.25, 1.0], [0.0, 0.0, -1.0]))

    def test_inactive_tlas_instances_get_empty_boxes(self):
        active_blas, active_info = X.build_blas([TRI])
        empty_blas, empty_info = X.build_blas([[[math.nan, 0.0, 0.0], [1.0, 0.0, 0.0], [0.0, 1.0, 0.0]]])
        mem = X.Memory()
        va_active = mem.place(0x100000, active_blas)
        va_empty = mem.place(0x200000, empty_blas)
        ident = [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0]]
        zero = [[0, 0, 0, 0], [0, 0, 0, 0], [0, 0, 0, 0]]
        insts = [
            X.Instance(zero, 0, X.INVALID, active_info["bounds"], mask=0xFF),           # null BLAS, singular matrix
            X.Instance(ident, va_active, active_info["root"], active_info["bounds"], mask=0),  # zero mask
            X.Instance(ident, va_empty, empty_info["root"], empty_info["bounds"], mask=0xFF),  # empty BLAS
            X.Instance([[1, 0, 0, 3], [0, 1, 0, 0], [0, 0, 1, 0]], va_active, active_info["root"],
                       active_info["bounds"], mask=0xFF),
            X.Instance(ident, 0, X.INVALID, empty_info["bounds"], mask=0xFF),           # 5 instances -> 2-level TLAS
        ]
        for box16 in (False, True):
            with self.subTest(box16=box16):
                tlas, info = X.build_tlas(insts, box16=box16)
                self.assertEqual(X.validate(tlas, expect_bottom=False), [])
                self.assertEqual(info["bounds"], ([3.0, 0.0, 0.0], [4.0, 1.0, 0.0]))
                va_tlas = mem.place(0x300000 + (0x10000 if box16 else 0), tlas)
                scene = [
                    (zero, 0xFF, []),
                    (ident, 0, [TRI]),
                    (ident, 0xFF, [[[math.nan, 0.0, 0.0], [1.0, 0.0, 0.0], [0.0, 1.0, 0.0]]]),
                    ([[1, 0, 0, 3], [0, 1, 0, 0], [0, 0, 1, 0]], 0xFF, [TRI]),
                    (ident, 0xFF, []),
                ]
                for o, d in (([3.25, 0.25, 1.0], [0.0, 0.0, -1.0]), ([0.25, 0.25, 1.0], [0.0, 0.0, -1.0])):
                    got = X.trace(mem, va_tlas, o, d)
                    want = X.brute_force(scene, o, d)
                    self.assertTrue(same_hit(got, want), (o, got, want))
                self.assertEqual(X.trace(mem, va_tlas, [3.25, 0.25, 1.0], [0.0, 0.0, -1.0])["instance"], 3)

    def test_pair_triangles_skips_zero_length_edges(self):
        # Two degenerate triangles sharing only a zero-length edge (0,0,0)->(0,0,0) must not pair.
        tris = [
            [[0.0, 0.0, 0.0], [0.0, 0.0, 0.0], [1.0, 0.0, 0.0]],
            [[0.0, 0.0, 0.0], [0.0, 0.0, 0.0], [0.0, 1.0, 0.0]],
        ]
        data, info = X.build_blas(tris, pair=True)
        self.assertEqual(X.validate(data, expect_bottom=True), [])
        self.assertEqual(X.count_pairs(data), 0)
        self.assertEqual(info["leaves"], 2)

    def test_box_test_misses_nan_bounds(self):
        for d in ([0.5, 0.5, -1.0], [0.0, 0.0, -1.0]):
            with self.subTest(d=d):
                inv = X._inverse_dir(d)
                self.assertEqual(X._box_test(10.0, [0.0, 0.0, 1.0], d, inv, [math.nan, 0.0, 0.0], [math.nan, 1.0, 0.0]),
                                 math.inf)
                self.assertEqual(X._box_test(10.0, [0.0, 0.0, 1.0], d, inv, [0.0, 0.0, 0.0], [1.0, math.nan, 0.0]),
                                 math.inf)

    def test_nan_transform_falls_back_to_zero_inverse_in_tlas(self):
        _, active_info = X.build_blas([TRI])
        nan_rot = [[math.nan, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0]]
        nan_trans = [[1, 0, 0, math.nan], [0, 1, 0, 0], [0, 0, 1, 0]]
        for matrix in (nan_rot, nan_trans):
            with self.subTest(matrix=matrix):
                with self.assertRaises(ValueError):
                    X.invert_affine(matrix)
                inst = X.Instance(matrix, 0x10000, active_info["root"], active_info["bounds"])
                tlas, info = X.build_tlas([inst])
                self.assertEqual(X.validate(tlas, expect_bottom=False), [])
                self.assertEqual(info["bounds"], ([math.inf] * 3, [-math.inf] * 3))
                off = X.ptr_offset(struct.unpack_from("<I", tlas, X.ptr_offset(info["root"]))[0])
                self.assertEqual(struct.unpack_from("<12f", tlas, off + 64), (0.0,) * 12)

    def test_singular_transform_with_valid_blas_is_inactive_in_tlas_and_brute_force(self):
        # Matches xdxr_build.comp / docs/bvh.md 7a: a singular transform with a real BLAS gets an empty box and zero
        # inverse so neither build_tlas nor brute_force fails on it, and rays skip it cleanly.
        blas, blas_info = X.build_blas([TRI])
        mem = X.Memory()
        va_blas = mem.place(0x100000, blas)
        zero = [[0, 0, 0, 0], [0, 0, 0, 0], [0, 0, 0, 0]]
        flat = [[1, 0, 0, 0], [0, 0, 0, 0], [0, 0, 1, 0]]
        shifted = [[1, 0, 0, 4], [0, 1, 0, 0], [0, 0, 1, 0]]
        insts = [
            X.Instance(zero, va_blas, blas_info["root"], blas_info["bounds"], mask=0xFF),
            X.Instance(flat, va_blas, blas_info["root"], blas_info["bounds"], mask=0xFF),
            X.Instance(shifted, va_blas, blas_info["root"], blas_info["bounds"], mask=0xFF),
        ]
        tlas, info = X.build_tlas(insts)
        self.assertEqual(X.validate(tlas, expect_bottom=False), [])
        self.assertEqual(info["bounds"], ([4.0, 0.0, 0.0], [5.0, 1.0, 0.0]))
        root = X.ptr_offset(info["root"])
        for slot in (0, 1):
            inst_off = X.ptr_offset(struct.unpack_from("<I", tlas, root + 4 * slot)[0])
            self.assertEqual(struct.unpack_from("<12f", tlas, inst_off + 64), (0.0,) * 12)
        va_tlas = mem.place(0x200000, tlas)
        scene = [(zero, 0xFF, [TRI]), (flat, 0xFF, [TRI]), (shifted, 0xFF, [TRI])]
        for o, d in (([0.25, 0.25, -1.0], [0.0, 0.0, 1.0]), ([4.25, 0.25, -1.0], [0.0, 0.0, 1.0])):
            got = X.trace(mem, va_tlas, o, d)
            want = X.brute_force(scene, o, d)
            self.assertTrue(same_hit(got, want), (o, got, want))
        self.assertEqual(X.trace(mem, va_tlas, [4.25, 0.25, -1.0], [0.0, 0.0, 1.0])["instance"], 2)


class Validate(unittest.TestCase):
    def setUp(self):
        self.data, self.info = X.build_blas(X.random_scene(random.Random(4), 30))

    def corrupt(self, off, fmt, *vals):
        b = bytearray(self.data)
        struct.pack_into(fmt, b, off, *vals)
        return X.validate(bytes(b))

    def test_size_mismatch(self):
        self.assertTrue(any("totalSizeIn64BNodes" in e for e in X.validate(self.data + bytes(64))))

    def test_element_count(self):
        self.assertTrue(any("numElements" in e for e in self.corrupt(12, "<I", 29)))

    def test_child_outside_parent(self):
        root = X.ptr_offset(self.info["root"])
        errs = self.corrupt(root + 16, "<3f", 1e6, 1e6, 1e6)
        self.assertTrue(errs)

    def test_parent_entry(self):
        b = bytearray(self.data)
        root = X.ptr_offset(self.info["root"])
        child = struct.unpack_from("<I", b, root)[0]
        if X.ptr_type(child) > X.TRI3:
            u = (X.ptr_offset(child) - self.info["elements"]) // 64
            struct.pack_into("<I", b, self.info["parents"] + 4 * u, 0x1234)
            self.assertTrue(any("parent table" in e for e in X.validate(bytes(b))))

    def test_parent_entry_outside_buffer_is_reported(self) -> None:
        # offsetToParents is the signed offset at +56 in the runtime header.
        errors = self.corrupt(56, "<i", len(self.data))
        self.assertTrue(any("parent table entry" in error and "outside the buffer" in error for error in errors))

    def test_dehydrated_refused(self):
        errs = self.corrupt(0, "<I", 0x81020006 | X.FLAG_DEHYDRATED)
        self.assertTrue(any("dehydrated" in e for e in errs))

    def test_short_buffer(self):
        self.assertEqual(X.validate(b"\0" * 10), ["buffer shorter than the 64-byte header"])

    def test_nan_child_box_at_root_is_rejected(self):
        data, info = X.build_blas([TRI])
        b = bytearray(data)
        root = X.ptr_offset(info["root"])
        struct.pack_into("<f", b, root + 16, math.nan)
        self.assertTrue(any("NaN" in e for e in X.validate(bytes(b), expect_bottom=True)))

    def test_misaligned_offset_to_parents_is_rejected(self):
        errs = self.corrupt(56, "<i", -63)
        self.assertTrue(any("offsetToParents" in e and "4-byte aligned" in e for e in errs), errs)

    def test_cyclic_and_short_buffers_terminate_cleanly_across_walkers(self):
        data, info = X.build_blas(X.grid_mesh(3), box16=True, pair=True)
        root_ptr = info["root"]
        root_off = X.ptr_offset(root_ptr)
        cyc = bytearray(data)
        struct.pack_into("<I", cyc, root_off, root_ptr)
        cyc_bytes = bytes(cyc)

        self.assertTrue(any("cycle" in e for e in X.box16_rounding(cyc_bytes)))
        self.assertEqual(X.box16_rounding(b"\0" * 10), ["buffer shorter than the 64-byte header"])
        self.assertIn(X.BOX16, X.node_census(cyc_bytes))
        self.assertEqual(X.node_census(b"\0" * 10), {})
        self.assertIsInstance(X.pair_rotations(cyc_bytes), set)
        self.assertEqual(X.pair_rotations(b"\0" * 10), set())

        mem = X.Memory()
        mem.place(0x10000, cyc_bytes)
        lo, hi = X.decode_box16(struct.unpack_from("<3I", cyc_bytes, root_off + 16))
        mid = [(lo[c] + hi[c]) * 0.5 for c in range(3)]
        with self.assertRaisesRegex(ValueError, "cycle"):
            X.trace(mem, 0x10000, [mid[0], mid[1], lo[2] - 5.0], [0.0, 0.0, 1.0])

        # Out-of-bounds root pointer is reported cleanly without struct.error.
        oob = bytearray(data)
        struct.pack_into("<I", oob, 48, X.ptr(len(data) + 64, X.BOX16))
        self.assertTrue(any("outside the buffer" in e for e in X.box16_rounding(bytes(oob))))
        self.assertEqual(X.node_census(bytes(oob)), {})
        self.assertEqual(X.pair_rotations(bytes(oob)), set())



def console_layout(data, k):
    """The same structure with the parent table laid out as the console writes it: offsetToParents negative, the entries
    of the first k units (the leaves) before the header and not stored, the rest stored from the header end on."""
    flags, units, crc, n, *rest = struct.unpack_from("<4I6fIiIIiI", data, 0)
    nodes, remap, root, elements, parents, cp = rest[6:]
    out = bytearray(data)
    slots = (elements - parents) // 4
    entries = [struct.unpack_from("<I", data, parents + 4 * u)[0] for u in range(slots)]
    kept = entries[k:] + [0] * k
    for i, e in enumerate(kept):
        struct.pack_into("<I", out, parents + 4 * i, e)
    struct.pack_into("<4I6fIiIIiI", out, 0, flags, units, crc, n, *rest[0:6], nodes, remap, root, elements,
                     parents - 4 * k, cp)
    return bytes(out), parents - 4 * k


class ConsoleParentTable(unittest.TestCase):
    """RT-151 review: real console structures have offsetToParents negative and store only the entries from the header
    end on (docs/bvh.md 3.1); validate() used to raise struct.error on them."""

    def build(self):
        tris = [((i * 0.1, 0, 0), (i * 0.1 + 1, 0, 0), (i * 0.1, 1, 0)) for i in range(20)]
        data, info = X.build_blas(tris)
        return data, info

    def test_a_negative_offset_to_parents_validates(self):
        data, info = self.build()
        k = (info["node_offset"] - info["elements"]) // 64           # the leaf units
        con, off = console_layout(data, k)
        self.assertLess(off, 0)
        self.assertEqual(X.validate(con), [])

    def test_a_wrong_stored_entry_is_still_caught(self):
        data, info = self.build()
        k = (info["node_offset"] - info["elements"]) // 64
        con, off = console_layout(data, k)
        bad = bytearray(con)
        for entry in range(64, info["elements"], 4):                # every stored entry
            struct.pack_into("<I", bad, entry, struct.unpack_from("<I", bad, entry)[0] ^ 0x10)
        self.assertTrue(any("parent table entry" in e for e in X.validate(bytes(bad))))

    def test_unstored_entries_are_not_compared(self):
        data, info = self.build()
        k = (info["node_offset"] - info["elements"]) // 64
        con, off = console_layout(data, k)
        self.assertLess(off + 4 * (k - 1), 64)                      # the last leaf entry lies before the header end
        self.assertEqual(X.validate(con), [])

    def test_a_runtime_build_keeps_its_offset_and_is_still_checked(self):
        data, info = self.build()
        self.assertEqual(X.validate(data), [])
        bad = bytearray(data)
        for entry in range(64, info["elements"], 4):
            struct.pack_into("<I", bad, entry, struct.unpack_from("<I", bad, entry)[0] ^ 0x10)
        self.assertTrue(any("parent table entry" in e for e in X.validate(bytes(bad))))

class CheckBuild(unittest.TestCase):
    """check-build on cases written the way vk_xdxr_build_test writes them, built here by the reference."""

    def write_cases(self, d, corrupt=False):
        import json
        tris = X.grid_mesh(4, 3.0)
        blas, info = X.build_blas(tris)
        if corrupt:                                    # move one leaf's first vertex: traces must disagree
            b = bytearray(blas)
            for leaf in range(info["triangles"]):
                struct.pack_into("<f", b, info["elements"] + 64 * leaf, 50.0)
            blas = bytes(b)
        va, bias = 0x12345600, 0x7F0000000000
        with open(os.path.join(d, "blas_grid.bin"), "wb") as f:
            f.write(blas)
        with open(os.path.join(d, "blas_grid.json"), "w") as f:
            json.dump({"kind": "blas", "address": va + bias,
                       "triangles": [t + [0, i] for i, t in enumerate(tris)]}, f)
        m = [[1, 0, 0, 2], [0, 1, 0, -1], [0, 0, 1, 0.5]]
        tlas, _ = X.build_tlas([X.Instance(m, va, info["root"], info["bounds"])])
        with open(os.path.join(d, "tlas.bin"), "wb") as f:
            f.write(tlas)
        with open(os.path.join(d, "tlas.json"), "w") as f:
            json.dump({"kind": "tlas", "address": 0x40000000, "bias": bias, "blas": ["blas_grid"],
                       "instances": [{"transform": sum(m, []), "mask": 255, "blas": 0}]}, f)

    def run_check(self, corrupt):
        with tempfile.TemporaryDirectory() as d:
            self.write_cases(d, corrupt)
            with contextlib.redirect_stdout(io.StringIO()):
                return X.check_build(d, rays=200)

    def test_reference_cases_pass(self):
        self.assertEqual(self.run_check(False), [])

    def test_corrupted_leaves_fail(self):
        self.assertTrue(self.run_check(True))

    def test_literal_bracket_path_via_main(self):
        # a directory with '[' and ']' in its name must not be treated as a glob: main must
        # discover the cases and run validation rather than silently returning the 77 skip code.
        with tempfile.TemporaryDirectory() as root:
            d = os.path.join(root, "cases[1]")
            os.mkdir(d)
            self.write_cases(d, corrupt=False)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                rc = X.main(["check-build", d])
            self.assertEqual(rc, 0, out.getvalue())
            self.assertIn("all cases match", out.getvalue())

    def test_empty_directory(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(X.check_build(d), [f"no cases in {d}"])


class Cli(unittest.TestCase):
    def test_validate_file(self):
        data, _ = X.build_blas(X.grid_mesh(3), pair=True)
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "blas.bin")
            with open(p, "wb") as f:
                f.write(data)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(X.main(["validate", p]), 0)
            self.assertIn("valid", out.getvalue())
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                self.assertEqual(X.main(["validate", os.path.join(d, "missing.bin")]), 2)

    def test_validate_rejects_a_root_with_a_parent(self) -> None:
        data, info = X.build_blas(X.grid_mesh(3), box16=True)
        self.assertEqual(X.validate(data), [])
        corrupt = bytearray(data)
        root_unit = (X.ptr_offset(info["root"]) - info["elements"]) // X.UNIT
        struct.pack_into("<I", corrupt, info["parents"] + 4 * root_unit, info["root"])

        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "root-parent.bin")
            with open(path, "wb") as f:
                f.write(corrupt)
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(X.main(["validate", path]), 1)
            self.assertIn("parent table entry", output.getvalue())
            self.assertIn(f"expected {X.INVALID:#x}", output.getvalue())
    def check_build_stderr(self, d):
        """stderr of `check-build d`, which must be exit 2 with exactly one ERROR line (no traceback)."""
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.assertEqual(X.main(["check-build", d]), 2)
        text = err.getvalue().strip()
        self.assertEqual(len(text.splitlines()), 1, text)
        self.assertNotIn("Traceback", text)
        self.assertTrue(text.startswith("ERROR: "), text)
        return text

    def test_check_build_case_without_its_bin_is_one_line(self):
        # check-build runs on a build folder that may hold other reports: any *.json there is treated as a case,
        # so a stray one must name itself instead of dying on the missing .bin beside it.
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "stray.json"), "w", encoding="utf-8") as f:
                f.write('{"kind": "blas", "address": 0, "triangles": []}')
            text = self.check_build_stderr(d)
            self.assertIn("stray.json", text)
            self.assertIn("stray.bin", text)

    def test_check_build_unparseable_case_json_is_one_line(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "broken.bin"), "wb") as f:
                f.write(b"")
            with open(os.path.join(d, "broken.json"), "w", encoding="utf-8") as f:
                f.write("not json at all")
            self.assertIn("broken.json", self.check_build_stderr(d))

    def test_check_build_json_without_a_kind_is_one_line(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "case.bin"), "wb") as f:
                f.write(b"")
            with open(os.path.join(d, "case.json"), "w", encoding="utf-8") as f:
                f.write('{"address": 0, "triangles": []}')
            self.assertIn("no 'kind' key", self.check_build_stderr(d))

    def test_check_build_tlas_without_an_address_names_its_json(self):
        # Several cases in one folder: the missing key must be reported against the file that has it.
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "good.json"), "w", encoding="utf-8") as f:
                f.write('{"kind": "blas", "address": 0, "triangles": []}')
            with open(os.path.join(d, "good.bin"), "wb") as f:
                f.write(b"")
            with open(os.path.join(d, "broken.json"), "w", encoding="utf-8") as f:
                f.write('{"kind": "tlas", "blas": [], "instances": []}')
            with open(os.path.join(d, "broken.bin"), "wb") as f:
                f.write(b"")
            text = self.check_build_stderr(d)
            self.assertIn("broken.json", text)
            self.assertIn("no 'address' key", text)
            self.assertNotIn("good.json", text)

    def test_check_build_instance_without_a_transform_names_its_json(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "broken.json"), "w", encoding="utf-8") as f:
                f.write('{"kind": "tlas", "address": 4096, "blas": [], "instances": [{"blas": 0, "mask": 1}]}')
            with open(os.path.join(d, "broken.bin"), "wb") as f:
                f.write(b"")
            text = self.check_build_stderr(d)
            self.assertIn("broken.json", text)
            self.assertIn("instance 0 has no 'transform' key", text)

    def test_check_build_unknown_blas_name_names_its_json(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "broken.json"), "w", encoding="utf-8") as f:
                f.write('{"kind": "tlas", "address": 4096, "blas": ["missing_bvh"], "instances": []}')
            with open(os.path.join(d, "broken.bin"), "wb") as f:
                f.write(b"")
            text = self.check_build_stderr(d)
            self.assertIn("broken.json", text)
            self.assertIn("BLAS 'missing_bvh' is not a case in this folder", text)

    def test_check_build_tlas_without_its_instances_names_its_json(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "broken.json"), "w", encoding="utf-8") as f:
                f.write('{"kind": "tlas", "address": 4096, "blas": []}')
            with open(os.path.join(d, "broken.bin"), "wb") as f:
                f.write(b"")
            text = self.check_build_stderr(d)
            self.assertIn("broken.json", text)
            self.assertIn("no 'instances' key", text)

    def test_check_build_accepts_utf8_bom_case_json(self):
        import json
        blas, _ = X.build_blas([TRI])
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "bom.bin"), "wb") as f:
                f.write(blas)
            with open(os.path.join(d, "bom.json"), "w", encoding="utf-8-sig") as f:
                json.dump({"kind": "blas", "address": 0x10000, "triangles": [TRI + [0, 0]]}, f)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                rc = X.main(["check-build", d, "--rays", "20"])
            self.assertEqual(rc, 0, out.getvalue())

    def test_check_build_rejects_bool_indices_and_bad_transform_or_rays(self):
        import json
        blas, info = X.build_blas([TRI])
        m = [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0]]
        tlas, _ = X.build_tlas([X.Instance(m, 0x10000, info["root"], info["bounds"])])
        cases = [
            ("bool_blas", {"kind": "tlas", "address": 0x20000, "blas": ["b"],
                           "instances": [{"transform": sum(m, []), "mask": 255, "blas": False}]}),
            ("short_xform", {"kind": "tlas", "address": 0x20000, "blas": ["b"],
                             "instances": [{"transform": [1, 0, 0], "mask": 255, "blas": 0}]}),
            ("zero_rays", {"kind": "blas", "address": 0x10000, "rays": 0, "triangles": [TRI + [0, 0]]}),
            ("bool_addr", {"kind": "blas", "address": True, "triangles": [TRI + [0, 0]]}),
        ]
        for label, payload in cases:
            with self.subTest(label=label):
                with tempfile.TemporaryDirectory() as d:
                    with open(os.path.join(d, "b.bin"), "wb") as f:
                        f.write(blas)
                    with open(os.path.join(d, "b.json"), "w", encoding="utf-8") as f:
                        json.dump({"kind": "blas", "address": 0x10000, "triangles": [TRI + [0, 0]]}, f)
                    with open(os.path.join(d, "bad.bin"), "wb") as f:
                        f.write(tlas if payload["kind"] == "tlas" else blas)
                    with open(os.path.join(d, "bad.json"), "w", encoding="utf-8") as f:
                        json.dump(payload, f)
                    text = self.check_build_stderr(d)
                    self.assertIn("bad.json", text)

    def test_check_build_corrupt_truncated_or_cyclic_bin_exits_1_not_2(self):
        import json
        blas, info = X.build_blas(X.grid_mesh(3))
        cyc = bytearray(blas)
        struct.pack_into("<I", cyc, X.ptr_offset(info["root"]), info["root"])
        for label, payload in (("truncated", b"\0" * 10), ("cyclic", bytes(cyc))):
            with self.subTest(label=label):
                with tempfile.TemporaryDirectory() as d:
                    with open(os.path.join(d, "bad.bin"), "wb") as f:
                        f.write(payload)
                    with open(os.path.join(d, "bad.json"), "w", encoding="utf-8") as f:
                        json.dump({"kind": "blas", "address": 0x10000,
                                   "triangles": [t + [0, i] for i, t in enumerate(X.grid_mesh(3))]}, f)
                    out, err = io.StringIO(), io.StringIO()
                    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                        rc = X.main(["check-build", d, "--rays", "20"])
                    self.assertEqual(rc, 1, (out.getvalue(), err.getvalue()))
                    self.assertEqual(err.getvalue(), "")
                    self.assertIn("problems", out.getvalue())

    def test_selftest(self):
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(X.main(["selftest", "--rays", "50"]), 0)


if __name__ == "__main__":
    unittest.main()
