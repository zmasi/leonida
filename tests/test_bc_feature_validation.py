"""Validation tests for tools/bc_feature.py block_average and bc1_average.

    python -m pytest tests/test_bc_feature_validation.py
"""
import os
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")
if TOOLS not in sys.path:
    sys.path.insert(0, TOOLS)

try:
    import numpy as np
    import bc_feature
    HAVE_NUMPY = True
except ImportError:
    HAVE_NUMPY = False


@unittest.skipUnless(HAVE_NUMPY, "numpy not installed")
class BcFeatureValidationTests(unittest.TestCase):
    def test_block_average_rejects_unsupported_bpe(self):
        for bpe in (0, 4, 12, 32):
            blocks = np.zeros((1, max(bpe, 1)), dtype=np.uint8)
            with self.subTest(bpe=bpe):
                with self.assertRaises(ValueError):
                    bc_feature.block_average(blocks, bpe=bpe)

    def test_block_average_rejects_mismatched_shape(self):
        with self.assertRaises(ValueError):
            bc_feature.block_average(np.zeros((1, 16), dtype=np.uint8), bpe=8)
        with self.assertRaises(ValueError):
            bc_feature.block_average(np.zeros((1, 8), dtype=np.uint8), bpe=16)
        with self.assertRaises(ValueError):
            bc_feature.block_average(np.zeros((8,), dtype=np.uint8), bpe=8)
        with self.assertRaises(ValueError):
            bc_feature.block_average(np.zeros((1, 1, 8), dtype=np.uint8), bpe=8)

    def test_bc1_average_rejects_mismatched_shape(self):
        for shape in ((8,), (1, 16), (1, 4), (1, 1, 8)):
            with self.subTest(shape=shape):
                with self.assertRaises(ValueError):
                    bc_feature.bc1_average(np.zeros(shape, dtype=np.uint8))

    def test_empty_valid_arrays_return_empty_rgb(self):
        for bpe in (8, 16):
            out = bc_feature.block_average(np.zeros((0, bpe), dtype=np.uint8), bpe=bpe)
            self.assertEqual(out.shape, (0, 3))


class SmoothnessAndDtypeSafetyTests(unittest.TestCase):
    def test_non_uint8_blocks_raise_value_error(self):
        for dtype in (np.int32, np.float32):
            with self.subTest(fn="bc1_average", dtype=dtype):
                with self.assertRaises(ValueError):
                    bc_feature.bc1_average(np.zeros((2, 8), dtype=dtype))
            for bpe in (8, 16):
                with self.subTest(fn="block_average", bpe=bpe, dtype=dtype):
                    with self.assertRaises(ValueError):
                        bc_feature.block_average(np.zeros((2, bpe), dtype=dtype), bpe=bpe)

    def test_smoothness_rejects_1d_and_empty_images(self):
        for shape in ((4,), (0, 4, 3), (4, 0, 3)):
            with self.subTest(shape=shape):
                with self.assertRaises(ValueError):
                    bc_feature.smoothness(np.zeros(shape, dtype=np.float32))

    def test_smoothness_handles_degenerate_mip_shapes_without_warnings(self):
        import math
        import warnings

        cases = [
            ("1x4x3", np.arange(12, dtype=np.float32).reshape(1, 4, 3), None),
            ("4x1x3", np.arange(12, dtype=np.float32).reshape(4, 1, 3), None),
            ("1x1x3", np.array([[[10.0, 20.0, 30.0]]], dtype=np.float32), 0.0),
        ]
        for label, img, exact in cases:
            with self.subTest(case=label):
                with warnings.catch_warnings(record=True) as caught:
                    warnings.simplefilter("always")
                    val = bc_feature.smoothness(img)
                runtime_warnings = [w for w in caught if issubclass(w.category, RuntimeWarning)]
                self.assertEqual(runtime_warnings, [])
                self.assertIsInstance(val, float)
                self.assertTrue(math.isfinite(val))
                if exact is not None:
                    self.assertEqual(val, exact)

    def test_smoothness_uint8_step_matches_float32_without_underflow(self):
        step_u8 = np.array([[0, 255], [0, 255]], dtype=np.uint8)
        step_f32 = step_u8.astype(np.float32)
        self.assertAlmostEqual(
            bc_feature.smoothness(step_u8),
            bc_feature.smoothness(step_f32),
            places=5,
        )


if __name__ == "__main__":
    unittest.main()
