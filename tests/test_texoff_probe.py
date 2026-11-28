"""tools/texoff_probe.py without a GPU: the rows tell a signed field from an unsigned one, and the kernels build.

    python -I tests/test_texoff_probe.py
"""
import os
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "tools", "hw_oracle"))

import texoff_probe as p  # noqa: E402


def unsigned_reading(dim, start, off):
    """The same sample if the fields were read unsigned (the rule the rows must rule out)."""
    x0, b0 = start
    x = p.clamp(x0 + (off & 0x3F), p.W)
    shift, n = (8, p.H2D) if dim == "2d" else (16, p.D3D)
    b = p.clamp(b0 + ((off >> shift) & 0x3F), n)
    return float(b * p.W + x)


class RowsTest(unittest.TestCase):
    def test_sext6(self):
        self.assertEqual([p.sext6(v) for v in (0, 0x1F, 0x20, 0x3F, 0x40, 0x7F)], [0, 31, -32, -1, 0, -1])

    def test_rows_with_a_negative_field_tell_signed_from_unsigned(self):
        for name, dim, start, off in p.ROWS:
            field = (off >> (8 if dim == "2d" else 16)) & 0x3F
            if field & 0x20 and field != 0x3F:
                with self.subTest(name):
                    self.assertNotEqual(p.expected(dim, start, off), unsigned_reading(dim, start, off))

    def test_both_axes_are_covered(self):
        self.assertEqual({d for _n, d, _s, _o in p.ROWS}, {"2d", "3d"})
        self.assertEqual(len({n for n, *_ in p.ROWS}), len(p.ROWS))

    def test_expected_values(self):
        self.assertEqual(p.expected("2d", (5, 40), 0x20 << 8), 8 * 64 + 5)       # 40 - 32
        self.assertEqual(p.expected("2d", (5, 10), 0x20 << 8), 0 * 64 + 5)       # clamps at row 0
        self.assertEqual(p.expected("2d", (5, 40), 0xC0 << 8), 40 * 64 + 5)      # bits 14-15 ignored
        self.assertEqual(p.expected("3d", (20, 40), (0x3C << 16) | (0x3F << 8) | 0x3E), 36 * 64 + 18)

    def test_kernels_build_for_rdna2_and_rdna3(self):
        for arch in ("gfx1036", "gfx1150"):
            for dim in ("2d", "3d"):
                rows = [r for r in p.ROWS if r[1] == dim]
                lines = p.kernel(arch, dim, rows)
                with self.subTest(arch=arch, dim=dim):
                    self.assertEqual(sum("image_sample_lz_o" in ln for ln in lines), len(rows))
                    self.assertIn(f"dim:SQ_RSRC_IMG_{dim.upper()}", "\n".join(lines))
                    self.assertEqual(lines[-1].startswith("  .long"), True)

    def test_3d_descriptor_type_and_depth(self):
        t = p.words("gfx1036", "3d")
        self.assertEqual(t[3] >> 28, p.SQ_RSRC_IMG_3D)
        self.assertEqual(t[4], p.D3D - 1)
        self.assertEqual(p.sampler_words()[0] & 0x1FF, 2 | 2 << 3 | 2 << 6)


class OracleLifecycleTest(unittest.TestCase):
    def _run_hw(self, fake_oracle):
        import contextlib
        import io
        from unittest import mock
        import runner

        buf = io.StringIO()
        with (
            mock.patch.object(runner, "open_oracle", return_value=fake_oracle) as open_mock,
            mock.patch.object(runner, "Oracle", side_effect=AssertionError("Oracle() called directly")),
            contextlib.redirect_stdout(buf),
        ):
            rc = p.main(["--oracle", "hw"])
        open_mock.assert_called_once_with()
        return rc, buf.getvalue()

    def test_hw_uses_open_oracle_and_closes_on_match(self):
        import struct

        class FakeOracle:
            device = "fake-gpu"
            arch = "gfx1036"
            closed = False

            def run(self, kernels):
                text = "\n".join(kernels[0].lines)
                dim = "2d" if "SQ_RSRC_IMG_2D" in text else "3d"
                rows = [r for r in p.ROWS if r[1] == dim]
                return [[struct.unpack("<I", struct.pack("<f", p.expected(dim, s, o)))[0] for _n, _d, s, o in rows]]

            def close(self):
                self.closed = True

        fake = FakeOracle()
        rc, out = self._run_hw(fake)
        self.assertEqual(rc, 0)
        self.assertTrue(fake.closed)
        self.assertIn("device: fake-gpu", out)

    def test_hw_closes_on_mismatch(self):
        class FakeOracle:
            device = "fake-gpu"
            arch = "gfx1036"
            closed = False

            def run(self, kernels):
                return [[0] * kernels[0].words]

            def close(self):
                self.closed = True

        fake = FakeOracle()
        rc, out = self._run_hw(fake)
        self.assertEqual(rc, 1)
        self.assertTrue(fake.closed)
        self.assertIn("DIFFERS", out)

    def test_hw_closes_on_kernel_rejection(self):
        class FakeOracle:
            device = "fake-gpu"
            arch = "gfx1036"
            closed = False

            def run(self, kernels):
                return ["assembler rejected"]

            def close(self):
                self.closed = True

        fake = FakeOracle()
        rc, out = self._run_hw(fake)
        self.assertEqual(rc, 2)
        self.assertTrue(fake.closed)
        self.assertIn("kernel rejected", out)

    def test_hw_unavailable_returns_77(self):
        import contextlib
        import io
        from unittest import mock
        import runner

        buf = io.StringIO()
        with (
            mock.patch.object(runner, "open_oracle", side_effect=runner.Unavailable("no device")),
            mock.patch.object(runner, "Oracle", side_effect=AssertionError("Oracle() called directly")),
            contextlib.redirect_stdout(buf),
        ):
            rc = p.main(["--oracle", "hw"])
        self.assertEqual(rc, 77)
        self.assertIn("no device", buf.getvalue())

    def test_hw_poison_returns_2_and_closes(self):
        import struct
        import tsharp

        for poisoned_wave in (32, 64):
            class FakeOracle:
                device = "fake-gpu"
                arch = "gfx1036"
                closed = False

                def run(self, kernels):
                    k = kernels[0]
                    if k.wave == poisoned_wave:
                        return [[tsharp.POISON] * k.words]
                    text = "\n".join(k.lines)
                    dim = "2d" if "SQ_RSRC_IMG_2D" in text else "3d"
                    rows = [r for r in p.ROWS if r[1] == dim]
                    return [[struct.unpack("<I", struct.pack("<f", p.expected(dim, s, o)))[0]
                             for _n, _d, s, o in rows]]

                def close(self):
                    self.closed = True

            fake = FakeOracle()
            with self.subTest(poisoned_wave=poisoned_wave):
                rc, out = self._run_hw(fake)
                self.assertEqual(rc, 2)
                self.assertTrue(fake.closed)
                self.assertIn("POISON", out)
                self.assertNotIn("DIFFERS", out)

    def test_hw_unsupported_arch_returns_2_and_closes(self):
        class FakeOracle:
            device = "fake-rdna4"
            arch = "gfx1200"
            closed = False

            def run(self, kernels):
                raise AssertionError("run() should not be called for an unsupported arch")

            def close(self):
                self.closed = True

        fake = FakeOracle()
        rc, out = self._run_hw(fake)
        self.assertEqual(rc, 2)
        self.assertTrue(fake.closed)
        self.assertIn("cannot build", out)
        self.assertIn("gfx1200", out)

    def test_hw_closes_on_malformed_results_and_run_errors(self):
        import struct
        import runner

        def expected_for(dim):
            rows = [r for r in p.ROWS if r[1] == dim]
            return [struct.unpack("<I", struct.pack("<f", p.expected(dim, s, o)))[0] for _n, _d, s, o in rows]

        n2d = len(expected_for("2d"))

        class StubOracle:
            device = "fake-gpu"
            arch = "gfx1036"

            def __init__(self, run_fn):
                self._run_fn = run_fn
                self.closed = False

            def run(self, kernels):
                return self._run_fn(kernels)

            def close(self):
                self.closed = True

        bad_cases = [
            ("empty_batch", [], "expected 1"),
            ("none_batch", None, "NoneType"),
            ("str_batch", "bad", "str"),
            ("extra_batch", [expected_for("2d"), expected_for("2d")], "got 2"),
            ("none_words", [None], "NoneType"),
            ("short_words", [[0] * 2], f"expected {n2d} words, got 2"),
            ("overlong_words", [[0] * (n2d + 1)], f"expected {n2d} words, got {n2d + 1}"),
            ("bool_word", [[True] * n2d], "bool"),
            ("none_word", [[None] * n2d], "NoneType"),
        ]
        for label, bad_ret, hint in bad_cases:
            for fail_wave in (32, 64):
                with self.subTest(case=label, wave=fail_wave):
                    def run_fn(ks, fw=fail_wave, br=bad_ret):
                        k = ks[0]
                        dim = "2d" if "SQ_RSRC_IMG_2D" in "\n".join(k.lines) else "3d"
                        if dim == "2d" and k.wave == fw:
                            return br
                        return [expected_for(dim)]

                    fake = StubOracle(run_fn)
                    rc, out = self._run_hw(fake)
                    self.assertEqual(rc, 2, out)
                    self.assertTrue(fake.closed)
                    self.assertIn(f"2d wave{fail_wave}:", out)
                    self.assertIn(hint, out)

        # runner.Unavailable during oracle.run() exits 77 cleanly and closes oracle.
        def raise_unavail(_ks):
            raise runner.Unavailable("mode unsupported")

        fake = StubOracle(raise_unavail)
        rc, out = self._run_hw(fake)
        self.assertEqual(rc, 77, out)
        self.assertTrue(fake.closed)
        self.assertIn("hardware oracle unavailable: mode unsupported", out)

        # RuntimeError during oracle.run() exits 2 cleanly and closes oracle.
        def raise_runtime(_ks):
            raise RuntimeError("gpu hung")

        fake = StubOracle(raise_runtime)
        rc, out = self._run_hw(fake)
        self.assertEqual(rc, 2, out)
        self.assertTrue(fake.closed)
        self.assertIn("harness error: gpu hung", out)


if __name__ == "__main__":
    unittest.main()
