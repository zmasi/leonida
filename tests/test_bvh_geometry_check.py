"""Synthetic checks for tools/bvh_geometry_check.py: the right geometry passes, a shifted, smaller or re-indexed one
fails the check that should catch it. No game data is read.

    python -I tests/test_bvh_geometry_check.py
"""
import contextlib
import io
import json
import math
import os
import struct
import sys
import tempfile
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "tests"))

import bvh_geometry_check as C  # noqa: E402
import bvh_rehydrate as R  # noqa: E402
from test_bvh_rehydrate import build  # noqa: E402

# Two triangles sharing the edge (1, 2); bounds x 0..1, y 0..1, z 0
VB = struct.pack("<12f", 0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0)
IB = struct.pack("<6I", 0, 1, 2, 2, 1, 3)
TRUE = R.Geometry(VB, indices=IB)
LEAVES = [(0, 0x81, 0, 1, 0x09)]          # one leaf holding both triangles
BOUNDS = (0, 0, 0, 1, 1, 0)


class TestChecks(unittest.TestCase):
    def test_true_geometry_is_consistent(self):
        src, _, _ = build(LEAVES, bounds=BOUNDS)
        r = C.check(src, [TRUE])
        self.assertTrue(r["consistent"], r)
        self.assertEqual((r["leaves_outside"], r["bounds_gap"], r["pairs"]), (0, 0.0, {"0": [1, 1]}))
        self.assertGreater(r["internal_area"], 0.0)

    def test_shifted_geometry_fails_bounds(self):
        shifted = R.Geometry(VB, indices=IB, transform=((1, 0, 0, 0.5), (0, 1, 0, 0), (0, 0, 1, 0)))
        src, _, _ = build(LEAVES, bounds=BOUNDS)
        r = C.check(src, [shifted])
        self.assertFalse(r["consistent"])
        self.assertEqual(r["leaves_outside"], 1)
        self.assertAlmostEqual(r["bounds_gap"], 0.5, places=6)

    def test_smaller_geometry_inside_the_bounds_still_fails(self):
        src, _, _ = build(LEAVES, bounds=(0, 0, 0, 2, 1, 0))
        r = C.check(src, [TRUE])
        self.assertEqual(r["leaves_outside"], 0)
        self.assertFalse(r["consistent"])

    def test_reindexed_geometry_fails_pairs_without_positions(self):
        # same triangles count, but the second one no longer shares an edge with the first
        src, _, _ = build(LEAVES, bounds=BOUNDS)
        loose = R.Geometry(VB + struct.pack("<6f", 1, 1, 0, 1, 1, 0), indices=struct.pack("<6I", 0, 1, 2, 3, 4, 5))
        r = C.check(src, [loose])
        self.assertEqual(r["pairs"], {"0": [0, 1]})
        self.assertFalse(r["consistent"])

    def test_overrides(self):
        tight = build([(0, 0x01, 0, None, 0x09), (0, 0x01, 1, None, 0x09)], overrides={6: 0.25}, bounds=BOUNDS)[0]
        self.assertEqual(C.check(tight, [TRUE])["overrides_consistent"], 1)
        one_ulp = struct.unpack("<f", struct.pack("<I", 0x3FFFFFFF))[0]          # just below 2.0, slot 1 max x is 1.0
        loose = build([(0, 0x01, 0, None, 0x09), (0, 0x01, 1, None, 0x09)], overrides={9: one_ulp}, bounds=BOUNDS)[0]
        self.assertEqual(C.check(loose, [TRUE])["overrides_inconsistent"], 1)
        near = struct.unpack("<f", struct.pack("<I", 0x3F7FFFFF))[0]             # one ulp below 1.0: within tolerance
        ok = build([(0, 0x01, 0, None, 0x09), (0, 0x01, 1, None, 0x09)], overrides={9: near}, bounds=BOUNDS)[0]
        self.assertEqual(C.check(ok, [TRUE])["overrides_inconsistent"], 0)

    def test_command_line(self):
        src, _, _ = build(LEAVES, bounds=BOUNDS)
        with tempfile.TemporaryDirectory() as d:
            paths = [os.path.join(d, n) for n in ("in.bin", "vb.bin", "ib.bin", "r.json")]
            for p, data in zip(paths, (src, VB, IB)):
                with open(p, "wb") as f:
                    f.write(data)
            with contextlib.redirect_stdout(io.StringIO()) as out:
                rc = C.main([paths[0], "--geom", paths[1] + "," + paths[2], "--json", paths[3]])
            self.assertEqual(rc, 0)
            self.assertIn("geometry 0 1/1", out.getvalue())
            self.assertTrue(os.path.getsize(paths[3]) > 0)
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(C.main([paths[0], "--geom", paths[1] + "," + paths[2], "--transform", "1,0,0,9,0,1,0,0,0,0,1,0"]), 1)
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(C.main([paths[0]]), 2)

    def test_json_must_not_overwrite_input_or_buffers(self):
        src, _, _ = build(LEAVES, bounds=BOUNDS)
        with tempfile.TemporaryDirectory() as d:
            paths = [os.path.join(d, n) for n in ("in.bin", "vb.bin", "ib.bin", "r.json")]
            for p, data in zip(paths, (src, VB, IB)):
                with open(p, "wb") as f:
                    f.write(data)
            # --json names the dehydrated input itself
            for target in (paths[0], paths[1], paths[2]):
                with contextlib.redirect_stderr(io.StringIO()) as err:
                    rc = C.main([paths[0], "--geom", paths[1] + "," + paths[2], "--json", target])
                self.assertEqual(rc, 2)
                self.assertIn("must not overwrite", err.getvalue())
            # a hard link to the input also counts
            link = os.path.join(d, "link.json")
            try:
                os.link(paths[0], link)
            except (OSError, NotImplementedError, AttributeError) as e:
                self.skipTest(f"hard links unavailable: {e}")
            with contextlib.redirect_stderr(io.StringIO()) as err:
                rc = C.main([paths[0], "--geom", paths[1] + "," + paths[2], "--json", link])
            self.assertEqual(rc, 2)
            self.assertIn("must not overwrite", err.getvalue())

    def test_nan_leaf_bounds_fails_consistency_even_when_other_leaf_covers_bounds(self):
        # Leaf 0 covers BOUNDS; leaf 1 has all-NaN vertices. Previously NaN comparison
        # returned False and min(finite, NaN) kept finite, falsely reporting consistent=True.
        vb = VB + struct.pack("<9f", *([math.nan] * 9))
        ib = struct.pack("<6I", 0, 1, 2, 4, 5, 6)
        geom = R.Geometry(vb, indices=ib)
        leaves = [(0, 0x01, 0, None, 0x09), (0, 0x01, 1, None, 0x09)]
        src, _, _ = build(leaves, bounds=(0, 0, 0, 1, 1, 0))
        r = C.check(src, [geom])
        self.assertEqual(r["leaves_outside"], 1)
        self.assertTrue(math.isinf(r["bounds_gap"]))
        self.assertFalse(r["consistent"])

    def test_nonfinite_scene_bounds_fails_consistency(self):
        src, _, _ = build(LEAVES, bounds=(0.0, 0.0, 0.0, math.inf, 1.0, 0.0))
        r = C.check(src, [TRUE])
        self.assertEqual(r["leaves_outside"], 1)
        self.assertTrue(math.isinf(r["bounds_gap"]))
        self.assertFalse(r["consistent"])

    def test_infinite_child_bound_does_not_bypass_override_range_check(self):
        # If child min_x is -inf and max_x is 10.0, a min_x override of 1000.0 (> max_x)
        # must be rejected rather than passing because tol blew up to +inf.
        src, _, _ = build(
            [(0, 0x01, 0, None, 0x09), (0, 0x01, 1, None, 0x09)],
            overrides={0: 1000.0},
            bounds=BOUNDS,
        )
        hdr = R.Header(src, 0)
        with mock.patch.object(C.R, "node_bounds", return_value=[-math.inf, 0.0, 0.0, 10.0, 1.0, 0.0]):
            good, bad = C.check_overrides(src, hdr, bytearray(hdr.total_bytes))
        self.assertEqual((good, bad), (0, 1))

        src_ok, _, _ = build(
            [(0, 0x01, 0, None, 0x09), (0, 0x01, 1, None, 0x09)],
            overrides={0: 5.0},
            bounds=BOUNDS,
        )
        hdr_ok = R.Header(src_ok, 0)
        with mock.patch.object(C.R, "node_bounds", return_value=[-math.inf, 0.0, 0.0, 10.0, 1.0, 0.0]):
            good, bad = C.check_overrides(src_ok, hdr_ok, bytearray(hdr_ok.total_bytes))
        self.assertEqual((good, bad), (1, 0))

    def test_invalid_geom_spec_and_json_directory_rejected(self):
        src, _, _ = build(LEAVES, bounds=BOUNDS)
        with tempfile.TemporaryDirectory() as d:
            in_path = os.path.join(d, "in.bin")
            vb_path = os.path.join(d, "vb.bin")
            ib_path = os.path.join(d, "ib.bin")
            out_dir = os.path.join(d, "out_dir")
            os.makedirs(out_dir)
            for p, data in ((in_path, src), (vb_path, VB), (ib_path, IB)):
                with open(p, "wb") as f:
                    f.write(data)
            for bad_spec in (f"{vb_path},{ib_path},extra", f",{ib_path}", ""):
                with contextlib.redirect_stderr(io.StringIO()) as err:
                    rc = C.main([in_path, "--geom", bad_spec])
                self.assertEqual(rc, 2, bad_spec)
                self.assertIn("--geom", err.getvalue())
            with contextlib.redirect_stderr(io.StringIO()) as err:
                rc = C.main([in_path, "--geom", f"{vb_path},{ib_path}", "--json", out_dir])
            self.assertEqual(rc, 2)
            self.assertIn("directory", err.getvalue())

    def test_json_report_strict_rfc8259_with_nonfinite_gap(self):
        vb = VB + struct.pack("<9f", *([math.nan] * 9))
        ib = struct.pack("<6I", 0, 1, 2, 4, 5, 6)
        leaves = [(0, 0x01, 0, None, 0x09), (0, 0x01, 1, None, 0x09)]
        src, _, _ = build(leaves, bounds=BOUNDS)
        with tempfile.TemporaryDirectory() as d:
            in_path = os.path.join(d, "in.bin")
            vb_path = os.path.join(d, "vb.bin")
            ib_path = os.path.join(d, "ib.bin")
            rep_path = os.path.join(d, "report.json")
            for p, data in ((in_path, src), (vb_path, vb), (ib_path, ib)):
                with open(p, "wb") as f:
                    f.write(data)
            with contextlib.redirect_stdout(io.StringIO()):
                rc = C.main([in_path, "--geom", f"{vb_path},{ib_path}", "--json", rep_path])
            self.assertEqual(rc, 1)
            with open(rep_path, "r", encoding="utf-8") as f:
                raw = f.read()

            def _reject(tok):
                raise ValueError(f"non-standard JSON token {tok}")

            parsed = json.loads(raw, parse_constant=_reject)
            self.assertFalse(parsed["consistent"])
            self.assertEqual(parsed["leaves_outside"], 1)
            self.assertIsNone(parsed["bounds_gap"])


if __name__ == "__main__":
    unittest.main()
