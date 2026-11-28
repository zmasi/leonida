"""Synthetic CLI tests for conformance heatmap output paths."""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "conformance" / "compare.py"
SPEC = importlib.util.spec_from_file_location("conformance_compare_output_paths", SCRIPT)
C = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = C
SPEC.loader.exec_module(C)


class DiffOutputPathTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.reference_dir = self.root / "reference"
        self.candidate_dir = self.root / "candidate"
        self.reference_dir.mkdir()
        self.candidate_dir.mkdir()
        self.reference = self.reference_dir / "manifest.json"
        self.candidate = self.candidate_dir / "manifest.json"

    def write_inputs(self, manifest, image_dir, scenes):
        rows = []
        for name, filename in scenes:
            image = image_dir / filename
            C.write_png(image, C.make_solid_raster(1, 1, C.PNG_COLOUR_TYPE_RGB, (10, 20, 30), str(image)))
            rows.append({"name": name, "png": filename,
                         "checks": [{"x": 0, "y": 0, "r": 10, "g": 20, "b": 30, "a": 255}]})
        C.write_selftest_manifest(manifest, "synthetic", "CPU", rows)

    def write_default_inputs(self):
        self.write_inputs(self.reference, self.reference_dir, [("scene", "frame.png")])
        self.write_inputs(self.candidate, self.candidate_dir, [("scene", "frame.png")])

    def run_diff(self, *options):
        return subprocess.run(
            [sys.executable, "-IB", "-X", "utf8", str(SCRIPT), "diff", "--reference", str(self.reference),
             "--candidate", str(self.candidate), *options],
            capture_output=True, encoding="utf-8", timeout=15,
        )

    def snapshot(self):
        return {path.relative_to(self.root): path.read_bytes() for path in self.root.rglob("*") if path.is_file()}

    def assert_refuses_collision(self, output, other, *options):
        before = self.snapshot()
        result = self.run_diff(*options)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn(str(output), result.stderr)
        self.assertIn(str(other), result.stderr)
        self.assertEqual(self.snapshot(), before)

    def test_first_heatmap_is_a_later_candidate_image(self):
        self.write_inputs(self.reference, self.reference_dir, [("first", "one.png"), ("second", "two.png")])
        self.write_inputs(self.candidate, self.candidate_dir, [("first", "one.png"), ("second", "first.diff.png")])
        collision = self.candidate_dir / "first.diff.png"
        self.assert_refuses_collision(collision, collision)

    def test_later_collision_is_rejected_before_any_heatmap_is_written(self):
        self.write_inputs(self.reference, self.reference_dir, [("first", "one.png"), ("second", "two.png")])
        self.write_inputs(self.candidate, self.candidate_dir, [("first", "second.diff.png"), ("second", "two.png")])
        collision = self.candidate_dir / "second.diff.png"
        self.assert_refuses_collision(collision, collision)

    def test_explicit_image_directories_can_make_a_heatmap_alias_a_reference_image(self):
        images = self.root / "images"
        images.mkdir()
        self.write_inputs(self.reference, images, [("scene", "scene.diff.png")])
        self.write_inputs(self.candidate, images, [("scene", "frame.png")])
        collision = images / "scene.diff.png"
        self.assert_refuses_collision(collision, collision, "--reference-dir", str(images),
                                      "--candidate-dir", str(images))

    def test_heatmap_cannot_replace_either_manifest(self):
        self.write_default_inputs()
        default_reference, default_candidate = self.reference, self.candidate
        collision = self.candidate_dir / "scene.diff.png"
        for side in ("reference", "candidate"):
            with self.subTest(side=side):
                self.reference, self.candidate = default_reference, default_candidate
                collision.write_bytes(getattr(self, side).read_bytes())
                setattr(self, side, collision)
                self.assert_refuses_collision(collision, collision, "--reference-dir", str(self.reference_dir))

    def test_heatmap_matches_candidate_image_on_a_case_insensitive_volume(self):
        self.write_inputs(self.reference, self.reference_dir, [("scene", "frame.png")])
        self.write_inputs(self.candidate, self.candidate_dir, [("scene", "SCENE.DIFF.PNG")])
        output = self.candidate_dir / "scene.diff.png"
        if not output.is_file():
            self.skipTest("output directory is case-sensitive")
        self.assert_refuses_collision(output, self.candidate_dir / "SCENE.DIFF.PNG")

    def test_heatmap_link_to_a_reference_image(self):
        self.write_default_inputs()
        source = self.reference_dir / "frame.png"
        output = self.candidate_dir / "scene.diff.png"
        for link in (os.link, os.symlink):
            with self.subTest(link=link.__name__):
                try:
                    link(source, output)
                except OSError as exc:
                    self.skipTest(f"{link.__name__} unavailable: {exc}")
                try:
                    self.assert_refuses_collision(output, source)
                finally:
                    output.unlink()

    def test_two_heatmaps_share_a_file(self):
        scenes = [("first", "one.png"), ("second", "two.png")]
        self.write_inputs(self.reference, self.reference_dir, scenes)
        self.write_inputs(self.candidate, self.candidate_dir, scenes)
        first = self.candidate_dir / "first.diff.png"
        second = self.candidate_dir / "second.diff.png"
        first.write_bytes(b"old heatmap")
        try:
            os.link(first, second)
        except OSError as exc:
            self.skipTest(f"hard links unavailable: {exc}")
        self.assert_refuses_collision(first, second)

    def test_missing_case_only_heatmaps_follow_directory_case_rules(self):
        scenes = [("scene", "one.png"), ("SCENE", "two.png")]
        self.write_inputs(self.reference, self.reference_dir, scenes)
        self.write_inputs(self.candidate, self.candidate_dir, scenes)
        first = self.candidate_dir / "scene.diff.png"
        second = self.candidate_dir / "SCENE.diff.png"
        self.assertFalse(first.exists())
        self.assertFalse(second.exists())
        if (self.candidate_dir / "ONE.PNG").exists():
            self.assert_refuses_collision(second, first)
        else:
            result = self.run_diff()
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertFalse(first.samefile(second))

    def test_dangling_heatmap_symlink_aliases_a_missing_heatmap(self):
        scenes = [("first", "one.png"), ("second", "two.png")]
        self.write_inputs(self.reference, self.reference_dir, scenes)
        self.write_inputs(self.candidate, self.candidate_dir, scenes)
        first = self.candidate_dir / "first.diff.png"
        second = self.candidate_dir / "second.diff.png"
        try:
            second.symlink_to(first.name)
        except OSError as exc:
            self.skipTest(f"symlink unavailable: {exc}")
        self.assert_refuses_collision(second, first)
        self.assertFalse(first.exists())
        self.assertEqual(second.readlink(), Path(first.name))

    def test_diff_still_replaces_an_existing_noncolliding_heatmap(self):
        self.write_default_inputs()
        heatmap = self.candidate_dir / "scene.diff.png"
        heatmap.write_bytes(b"old heatmap")
        result = self.run_diff()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(C.read_png(heatmap).pixel_rgba(0, 0), (48, 48, 48, 255))


if __name__ == "__main__":
    unittest.main()
