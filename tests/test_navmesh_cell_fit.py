"""tools/navmesh_cell_fit.py: the grid fit, its chance baseline and the per-cell summary, on synthetic vertices.
No game files."""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import navmesh_cell_fit as cf  # noqa: E402


def quantise(points, ext):
    return [tuple(round(p[k] / ext[k] * 65535) if ext[k] else 0 for k in range(3)) for p in points]


class CellFit(unittest.TestCase):
    def test_points_on_a_cell_grid_fit_it_and_not_another(self):
        ext = (1.755, 5.1675, 0.0)                   # 54 x 159 cells of 0.0325
        pts = [(i * 0.0325, j * 0.0325, 0.0) for i in range(0, 55, 6) for j in range(0, 160, 7)]
        q = quantise(pts, ext)
        self.assertGreaterEqual(cf.fit_fraction(q, ext, 0.0325), 0.99)
        self.assertLess(cf.fit_fraction(q, ext, 0.03125), 0.5)

    def test_control_mean_includes_late_tiles_and_weights_duplicates(self):
        from unittest.mock import patch
        fits = dict.fromkeys(cf.DOC_CELLS + cf.OTHER_CELLS, 1.0)
        small = {"ext": (1, 1, 0), "fit": fits}
        large = {"ext": (1, 2, 0), "fit": fits}
        flat = {"ext": (0, 0, 0), "fit": dict.fromkeys(fits, 0.0)}
        tiles = [small] * 20 + [large, flat]
        with patch.object(cf, "baseline", side_effect=lambda extent, cell: extent / 4) as baseline:
            report = cf.summarise_control(tiles)
            self.assertEqual(report["cells"]["0.0625"], {"fit": 21, "baseline_mean": round(5.5 / 21, 4)})
            self.assertEqual(baseline.call_count, 2 * len(fits))
            self.assertEqual(report, cf.summarise_control(list(reversed(tiles))))
            self.assertIsNone(cf.summarise_control([flat])["cells"]["0.0625"]["baseline_mean"])

    def test_baseline_is_small_for_a_fine_step(self):
        self.assertLess(cf.baseline(2.0625, 0.0625), 0.06)
        # a 128 m extent's step is 128/65535, a little over 1/512 m: it drifts off a 1/32 m grid, so the static
        # tiles are a real control (3 levels in 16 land within 1.5 steps)
        self.assertAlmostEqual(cf.baseline(128.0, 0.03125), 0.1875, places=3)

    def test_summary_takes_the_coarsest_documented_cell(self):
        ext = (2.0, 4.0, 0.0)
        coarse = quantise([(i * 0.0625, j * 0.0625, 0) for i in range(33) for j in range(0, 65, 4)], ext)
        fine = quantise([(i * 0.03125 + 0.03125 * (i % 2 == 1), j * 0.03125, 0) for i in range(0, 63, 2)
                         for j in range(1, 128, 2)], ext)
        meshes = [{"n": len(q), "ext": ext, "fit": {c: cf.fit_fraction(q, ext, c) for c in cf.DOC_CELLS + cf.OTHER_CELLS}}
                  for q in (coarse, fine)]
        s = cf.summarise(meshes)
        self.assertEqual(s["cells"]["0.0625"]["meshes"], 1)
        self.assertEqual(s["cells"]["0.03125"]["meshes"], 1)
        self.assertEqual(s["cells"]["0.0325"]["meshes"], 0)
        self.assertEqual(s["fit_none"], 0)

    def test_output_cannot_overwrite_a_common_carve_input(self):
        import subprocess
        import tempfile
        from pathlib import Path

        with tempfile.TemporaryDirectory() as root:
            archive = Path(root) / "nav.rpf"
            archive.write_bytes(b"9FPR" + (8).to_bytes(4, "little") + bytes(8))
            doc = Path(root) / "vehicle.bin"
            original = b"<CVehiclePoints><ModelName>Test</ModelName></CVehiclePoints>"
            doc.write_bytes(original)
            alias = Path(root) / "report.json"
            os.link(doc, alias)
            for output in (doc, alias):
                with self.subTest(output=output):
                    proc = subprocess.run([sys.executable, "-I", cf.__file__, str(archive), "--common", root,
                                           "--out", str(output)], capture_output=True, text=True)
                    self.assertEqual(proc.returncode, 2)
                    self.assertIn("--common", proc.stderr)
                    self.assertEqual(doc.read_bytes(), original)

    def test_name_hashes_cover_case_and_suffix(self):
        h = cf.name_hashes({"Wagon01"})
        self.assertIn(cf.nd.joaat_cs("wagon01"), h)
        self.assertIn(cf.nd.joaat_cs("Wagon01.yft"), h)
        self.assertEqual(len(h), 4)


if __name__ == "__main__":
    unittest.main()
