"""tools/ps_helper_probe.py: the analysis on a CPU model of what gfx1036 does (each check, and that it fails when the
model breaks that rule), the programs' shape, and (with LEONIDA_PS_ORACLE_BUILD set to a runtime build
and an AMD GPU selected) the measurement itself. Standard library only."""
import contextlib
import io
import json
import os
import struct
import sys
import tempfile
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import ps_helper_probe as H  # noqa: E402

W, Hh = H.SIZE


def u32(v):
    return struct.unpack("<I", struct.pack("<f", v))[0]


def model(helpers_at_entry=False, empty_quads=False, swap_lanes=False, helper_export=False, flat_helpers=False):
    """What the measured gfx1036 gives, built from the geometry: pixel centres inside the triangle are covered, the
    quads with a covered pixel are packed 16 to a wave64 in raster order. Each flag breaks one measured rule."""
    def inside(p):
        i, j = H.bary(p)
        return i > 0 and j > 0 and i + j < 1
    cov = {(x, y) for y in range(Hh) for x in range(W) if inside((x, y))}
    quads = sorted({(x // 2, y // 2) for x, y in cov}, key=lambda q: (q[1], q[0]))
    if empty_quads:
        quads.insert(0, (99, 99))          # a quad with no covered pixel takes lanes 0..3
    lane, wave_of = {}, {}
    for n, q in enumerate(quads):
        for k in range(4):
            p = (2 * q[0] + (k & 1), 2 * q[1] + (k >> 1))
            if swap_lanes:
                k ^= 1
            lane[p], wave_of[p] = (n % 16) * 4 + k, n // 16
    entry, wqm = {}, {}
    for p in cov:
        w = wave_of[p]
        entry[w] = entry.get(w, 0) | 1 << lane[p]
        if helpers_at_entry:
            for k in (1, 2, 3):
                entry[w] |= 1 << lane[H.neighbour(p, k)]
    for p in lane:
        wqm[wave_of[p]] = wqm.get(wave_of[p], 0) | 1 << lane[p]
    res = {n: {(x, y): H.CLEAR for y in range(Hh) for x in range(W)} for n in H.PROGRAMS}
    for p in cov:
        e, q = entry[wave_of[p]], wqm[wave_of[p]]
        x, y = H.ndc(p)
        res["masks"][p] = (lane[p], e & 0xFFFFFFFF, e >> 32, q & 0xFFFFFFFF)
        res["self"][p] = (lane[p], q >> 32, u32(x), u32(y))
        nb = {k: H.ndc(H.neighbour(p, k)) for k in (1, 2, 3)}
        if flat_helpers:
            nb = {k: (x, y) if H.neighbour(p, k) not in cov else v for k, v in nb.items()}
        res["nb12"][p] = (u32(nb[1][0]), u32(nb[1][1]), u32(nb[2][0]), u32(nb[2][1]))
        res["nb3"][p] = (u32(nb[3][0]), u32(nb[3][1])) * 2
        i, j = H.bary(H.neighbour(p, 1))
        res["nowqm_ij"][p] = (u32(i), u32(j), 0xDEADBEEF, H.SENTINEL)
        res["nowqm_masks"][p] = (lane[p], e & 0xFFFFFFFF, e >> 32, e & 0xFFFFFFFF)
        res["helper_export"][p] = (0x3F800000,) * 4
    if helper_export:
        outside = next(p for p in lane if p not in cov)
        res["helper_export"][outside] = (0x3F800000,) * 4
    return res


class Analysis(unittest.TestCase):
    def failing(self, res):
        return [n for n, ok, _d in H.analyze(res) if not ok]

    def test_the_measured_behaviour_passes_every_check(self):
        checks = H.analyze(model())
        self.assertEqual([n for n, ok, _d in checks if not ok], [])
        self.assertEqual(len(checks), 10)

    def test_each_rule_broken_fails_its_check(self):
        for flag, check in (("helpers_at_entry", "entry EXEC"), ("empty_quads", "quads packed"),
                            ("swap_lanes", "lane % 4"), ("helper_export", "an exp"),
                            ("flat_helpers", "after s_wqm a helper")):
            with self.subTest(flag=flag):
                failing = self.failing(model(**{flag: True}))
                self.assertTrue(any(n.startswith(check) for n in failing), failing)

    def test_full_coverage_has_no_helpers_to_measure(self):
        res = model()
        for p in res["masks"]:
            if res["masks"][p] == H.CLEAR:
                for name in H.PROGRAMS:
                    res[name][p] = res[name][next(iter(q for q in res["masks"] if res["masks"][q] != H.CLEAR))]
        self.assertIn("partial quads at the edge", self.failing(res))

    def test_geometry(self):
        self.assertEqual(H.bary((0, 0)), H.bary((0, 0)))
        i, j = H.bary((0, 0))
        self.assertTrue(0 < i < 0.1 and 0 < j < 0.1)
        self.assertEqual([H.neighbour((4, 6), k) for k in (1, 2, 3)], [(5, 6), (4, 7), (5, 7)])
        self.assertEqual(H.ndc((0, 0)), (-1 + 1 / 16, -1 + 1 / 16))


class Programs(unittest.TestCase):
    def test_every_program_ends_and_exports_once(self):
        for name in H.PROGRAMS:
            with self.subTest(name=name):
                lines = H.program(name)
                self.assertEqual(lines[-1], "s_endpgm")
                self.assertEqual(sum(ln.startswith("exp ") for ln in lines), 1)
                self.assertEqual("s_wqm_b64 exec, exec" in lines, H.PROGRAMS[name][0])


class CliAndJsonTests(unittest.TestCase):
    def test_empty_coverage_does_not_crash_and_fails_coverage_checks(self):
        res = {name: {(x, y): H.CLEAR for y in range(Hh) for x in range(W)} for name in H.PROGRAMS}
        checks = H.analyze(res)
        self.assertEqual(len(checks), 10)
        by_name = {name: ok for name, ok, _ in checks}
        for name, ok in by_name.items():
            if not name.startswith("an exp"):
                self.assertFalse(ok, f"expected {name!r} to fail when nothing is covered")

    def test_json_preflight_rejects_bad_paths_before_measure(self):
        with tempfile.TemporaryDirectory() as tmp:
            blocker = os.path.join(tmp, "not_a_dir")
            with open(blocker, "w", encoding="utf-8", newline="\n") as f:
                f.write("x")
            for bad in ("", "   ", tmp, tmp + os.sep, os.path.join(blocker, "out.json")):
                with self.subTest(bad=bad):
                    err = io.StringIO()
                    with mock.patch.object(H, "measure", side_effect=AssertionError("should not run GPU")), \
                            contextlib.redirect_stderr(err):
                        rc = H.main(["--build", "dummy", "--json", bad])
                    self.assertEqual(rc, 2)
                    self.assertTrue(err.getvalue().strip())

    def test_json_write_is_atomic_has_trailing_newline_and_preserves_existing_on_failure(self):
        res = model()
        with tempfile.TemporaryDirectory() as tmp:
            out_json = os.path.join(tmp, "nested", "probe.json")
            with mock.patch.object(H, "measure", return_value=("mock-gpu", res)), \
                    contextlib.redirect_stdout(io.StringIO()):
                rc = H.main(["--build", "dummy", "--json", out_json])
            self.assertEqual(rc, 0)
            with open(out_json, "rb") as f:
                raw = f.read()
            self.assertTrue(raw.endswith(b"\n"))
            self.assertNotIn(b"\r\n", raw)
            doc = json.loads(raw.decode("utf-8"))
            self.assertEqual(doc["device"], "mock-gpu")

            before = b'{"keep": true}\n'
            with open(out_json, "wb") as f:
                f.write(before)
            nested_dir = os.path.dirname(out_json)

            def fail_dump(*_args, **_kwargs):
                raise OSError("disk full")

            for target_obj, attr, side_effect in (
                (H.json, "dump", fail_dump),
                (os, "replace", OSError("replace failed")),
            ):
                with self.subTest(mock_attr=attr):
                    err = io.StringIO()
                    with mock.patch.object(H, "measure", return_value=("mock-gpu", res)), \
                            mock.patch.object(target_obj, attr, side_effect=side_effect), \
                            contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(err):
                        rc = H.main(["--build", "dummy", "--json", out_json])
                    self.assertEqual(rc, 2)
                    self.assertNotIn("Traceback", err.getvalue())
                    with open(out_json, "rb") as f:
                        self.assertEqual(f.read(), before)
                    self.assertEqual(sorted(os.listdir(nested_dir)), ["probe.json"])

    def test_symlinked_json_updates_target_in_place(self):
        res = model()
        with tempfile.TemporaryDirectory() as tmp:
            real_path = os.path.join(tmp, "real.json")
            with open(real_path, "wb") as f:
                f.write(b"{}\n")
            link_path = os.path.join(tmp, "link.json")
            try:
                os.symlink(real_path, link_path)
            except OSError as exc:
                self.skipTest(f"symlink unavailable: {exc}")
            with mock.patch.object(H, "measure", return_value=("mock-gpu", res)), \
                    contextlib.redirect_stdout(io.StringIO()):
                rc = H.main(["--build", "dummy", "--json", link_path])
            self.assertEqual(rc, 0)
            self.assertTrue(os.path.islink(link_path))
            with open(real_path, encoding="utf-8") as f:
                self.assertEqual(json.load(f)["device"], "mock-gpu")

    def test_validate_json_path_and_upfront_refusal(self):
        with tempfile.TemporaryDirectory() as tmp:
            bad_parent = os.path.join(tmp, "file_not_dir")
            with open(bad_parent, "w", encoding="utf-8", newline="\n") as f:
                f.write("x")
            for bad in ("", "   ", tmp, tmp + os.sep, os.path.join(bad_parent, "out.json")):
                self.assertIsNotNone(H.validate_json_path(bad), bad)
                err_buf = io.StringIO()
                with mock.patch.object(H, "measure", side_effect=AssertionError("must not run")) as m, \
                        contextlib.redirect_stderr(err_buf):
                    rc = H.main(["--build", "dummy", "--json", bad])
                self.assertEqual(rc, 2)
                m.assert_not_called()

    def test_atomic_json_write_and_replace_failure_preserves_existing(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, "report.json")
            err = H.write_json_report(out, {"device": "ok", "checks": [], "words": {}})
            self.assertIsNone(err)
            self.assertEqual(os.listdir(tmp), ["report.json"])
            with open(out, "w", encoding="utf-8", newline="\n") as f:
                f.write('{"keep": true}\n')
            with mock.patch.object(H, "replace_retry", side_effect=OSError("replace denied")):
                err = H.write_json_report(out, {"device": "new"})
            self.assertIsNotNone(err)
            self.assertEqual(os.listdir(tmp), ["report.json"])
            with open(out, encoding="utf-8") as f:
                self.assertEqual(f.read(), '{"keep": true}\n')


@unittest.skipUnless(os.environ.get("LEONIDA_PS_ORACLE_BUILD"), "set LEONIDA_PS_ORACLE_BUILD to a runtime build "
                     "and select an AMD GPU (LEONIDA_VK_DEVICE) to measure helper lanes on hardware")
class OnHardware(unittest.TestCase):
    def test_helper_lanes_at_a_triangle_edge(self):
        try:
            _device, res = H.measure(os.environ["LEONIDA_PS_ORACLE_BUILD"])
        except H.ps_oracle.Unavailable as e:
            self.skipTest(str(e))
        self.assertEqual([(n, d) for n, ok, d in H.analyze(res) if not ok], [])


if __name__ == "__main__":
    unittest.main()
