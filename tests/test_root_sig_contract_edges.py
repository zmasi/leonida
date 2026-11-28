"""Stdlib unittest edge-case tests for tools/root_sig_contract.py (no pytest, no GPU, no game files).

    python -I tests/test_root_sig_contract_edges.py -v
"""
import contextlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOL = os.path.join(ROOT, "tools", "root_sig_contract.py")
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(ROOT, "tools"))

import root_sig_contract as rsc  # noqa: E402

ROOT_SIG = {
    "version": "1.1",
    "flags": 0,
    "params": [
        {"type": "constants", "count": 4, "visibility": "all"},
        {"type": "cbv", "visibility": "vertex"},
        {"type": "table", "ranges": [{"kind": "srv", "count": 8}], "visibility": "pixel"},
    ],
}


class RootSigContractEdgeTests(unittest.TestCase):
    def test_reconcile_ptr64_hi_on_constant_slot_reports_single_incompatibility(self):
        dwords, err = rsc.pack_layout(ROOT_SIG)
        self.assertEqual(err, "")
        for sgpr in (0, 1, 2, 3):
            with self.subTest(sgpr=sgpr):
                issues = rsc.reconcile([{"sgpr": sgpr, "kind": "ptr64_hi"}], dwords)
                self.assertEqual(len(issues), 1, f"expected 1 issue on constant slot s{sgpr}, got {issues}")
                self.assertIn("incompatible", issues[0])
                self.assertNotIn("paired lo", issues[0])

    def test_reconcile_ptr64_hi_on_descriptor_and_table_halves(self):
        dwords, err = rsc.pack_layout(ROOT_SIG)
        self.assertEqual(err, "")
        # High halves of CBV (s5) and table (s7) are compatible; low halves (s4, s6) fail paired-lo check.
        self.assertEqual(rsc.reconcile([{"sgpr": 5, "kind": "ptr64_hi"}], dwords), [])
        self.assertEqual(rsc.reconcile([{"sgpr": 7, "kind": "ptr64_hi"}], dwords), [])
        for sgpr in (4, 6):
            with self.subTest(sgpr=sgpr):
                issues = rsc.reconcile([{"sgpr": sgpr, "kind": "ptr64_hi"}], dwords)
                self.assertEqual(len(issues), 1)
                self.assertIn("paired lo", issues[0])

    def test_pack_layout_refuses_malformed_sig_and_params_without_raising(self):
        bad_sigs = [
            None,
            [],
            "not a dict",
            42,
            {"params": None},
            {"params": "cbv"},
            {"params": {"type": "cbv"}},
            {"params": [1]},
            {"params": ["cbv"]},
            {"params": [None]},
        ]
        for sig in bad_sigs:
            with self.subTest(sig=sig):
                dwords, error = rsc.pack_layout(sig)
                self.assertEqual(dwords, [])
                self.assertTrue(error, f"expected non-empty error for {sig!r}")

    def test_cli_deeply_nested_json_returns_exit_2_without_traceback(self):
        depth = max(sys.getrecursionlimit() * 6, 6000)
        nested = "[" * depth + "]" * depth
        with tempfile.TemporaryDirectory() as tmp:
            valid_index = os.path.join(tmp, "ok_index.json")
            valid_sig = os.path.join(tmp, "ok_sig.json")
            nested_index = os.path.join(tmp, "nested_index.json")
            nested_sig = os.path.join(tmp, "nested_sig.json")
            with open(valid_index, "w", encoding="utf-8") as f:
                json.dump({}, f)
            with open(valid_sig, "w", encoding="utf-8") as f:
                json.dump(ROOT_SIG, f)
            with open(nested_index, "w", encoding="utf-8") as f:
                f.write(nested)
            with open(nested_sig, "w", encoding="utf-8") as f:
                f.write(nested)

            for label, idx_path, sig_path in (
                ("nested_index", nested_index, valid_sig),
                ("nested_rootsig", valid_index, nested_sig),
            ):
                with self.subTest(case=label):
                    res = subprocess.run(
                        [sys.executable, "-I", TOOL, idx_path, "--rootsig", sig_path],
                        capture_output=True, text=True,
                    )
                    self.assertEqual(res.returncode, 2, res.stdout + res.stderr)
                    # Python <= 3.13 hits RecursionError inside json.load ("ERROR: cannot read");
                    # Python 3.14+'s iterative JSON parser decodes the nested list and trips the
                    # top-level schema check ("expected a JSON object"). Both are clean exit-2 errors.
                    self.assertTrue(
                        "ERROR: cannot read" in res.stderr or "expected a JSON object" in res.stderr,
                        f"unexpected stderr: {res.stderr!r}",
                    )
                    self.assertNotIn("Traceback", res.stderr)

    def test_recursion_error_in_json_load_returns_exit_2_without_traceback(self):
        with tempfile.TemporaryDirectory() as tmp:
            idx_path = os.path.join(tmp, "index.json")
            sig_path = os.path.join(tmp, "sig.json")
            with open(idx_path, "w", encoding="utf-8") as f:
                json.dump({}, f)
            with open(sig_path, "w", encoding="utf-8") as f:
                json.dump(ROOT_SIG, f)

            # Direct _load_json check: converts RecursionError into ValueError("cannot read ...").
            with mock.patch("json.load", side_effect=RecursionError("maximum recursion depth exceeded")):
                with self.assertRaisesRegex(ValueError, "cannot read"):
                    rsc._load_json(idx_path)

            # CLI checks for both index and --rootsig regardless of Python's JSON parser depth limit.
            for label, side_effect, expected_path in (
                ("index", [RecursionError("maximum recursion depth exceeded")], idx_path),
                ("rootsig", [{}, RecursionError("maximum recursion depth exceeded")], sig_path),
            ):
                with self.subTest(case=label):
                    err = io.StringIO()
                    with mock.patch("json.load", side_effect=side_effect), contextlib.redirect_stderr(err):
                        rc = rsc.main([idx_path, "--rootsig", sig_path])
                    stderr = err.getvalue()
                    self.assertEqual(rc, 2, stderr)
                    self.assertIn("ERROR: cannot read", stderr)
                    self.assertIn(expected_path, stderr)
                    self.assertNotIn("Traceback", stderr)

    def test_main_accepts_argv_for_example_and_reconcile(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = rsc.main(["--example"])
        self.assertEqual(rc, 0)
        parsed = json.loads(out.getvalue())
        self.assertIn("params", parsed)

        with tempfile.TemporaryDirectory() as tmp:
            idx_path = os.path.join(tmp, "index.json")
            sig_path = os.path.join(tmp, "sig.json")
            with open(idx_path, "w", encoding="utf-8") as f:
                json.dump({"ok": {"user_sgpr_reads": [{"sgpr": 0, "kind": "const32"}]}}, f)
            with open(sig_path, "w", encoding="utf-8") as f:
                json.dump(ROOT_SIG, f)

            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                rc = rsc.main([idx_path, "--rootsig", sig_path])
            self.assertEqual(rc, 0)
            self.assertIn("1 compatible, 0 contradictory", out.getvalue())

            with open(idx_path, "w", encoding="utf-8") as f:
                json.dump({"bad": {"user_sgpr_reads": [{"sgpr": 0, "kind": "ptr64_hi"}]}}, f)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                rc = rsc.main([idx_path, "--rootsig", sig_path])
            self.assertEqual(rc, 1)
            self.assertEqual(out.getvalue().count("  - "), 1)

    def test_console_reconcile_rejects_ptr64_lo_on_one_dword_table(self):
        sig = {"params": [{"type": "table"}, {"type": "table"}, {"type": "cbv"}]}
        dwords, err = rsc.pack_layout(sig, "cs")
        self.assertEqual(err, "")
        # s4 and s5 are 1-dword console tables; s6..s7 is a 2-dword root CBV.
        self.assertEqual(rsc.reconcile([{"sgpr": 4, "kind": "ptr32_lo"}], dwords), [])
        self.assertEqual(rsc.reconcile([{"sgpr": 6, "kind": "ptr64_lo"}], dwords), [])
        for sgpr in (4, 5):
            with self.subTest(sgpr=sgpr):
                issues = rsc.reconcile([{"sgpr": sgpr, "kind": "ptr64_lo"}], dwords)
                self.assertEqual(len(issues), 1)
                self.assertIn("without paired hi dword", issues[0])
        # Trailing 1-dword table at end of layout also fails without paired hi dword.
        tail_dwords, err = rsc.pack_layout({"params": [{"type": "table"}]}, "cs")
        self.assertEqual(err, "")
        issues = rsc.reconcile([{"sgpr": 4, "kind": "ptr64_lo"}], tail_dwords)
        self.assertEqual(len(issues), 1)
        self.assertIn("without paired hi dword", issues[0])

    def test_pack_layout_rejects_unhashable_stage(self):
        for stage in ([], {}, 123):
            with self.subTest(stage=stage):
                dwords, err = rsc.pack_layout(ROOT_SIG, stage)
                self.assertEqual(dwords, [])
                self.assertIn("no console base", err)

    def test_reconcile_and_cluster_handle_unhashable_kind(self):
        dwords, err = rsc.pack_layout(ROOT_SIG)
        self.assertEqual(err, "")
        for kind in (["const32"], {"k": 1}, 42):
            with self.subTest(kind=kind):
                issues = rsc.reconcile([{"sgpr": 0, "kind": kind}], dwords)
                self.assertEqual(len(issues), 1)
                self.assertIn("unknown read kind", issues[0])
        ranked = rsc.cluster({
            "a": {"user_sgpr_reads": [{"sgpr": 0, "kind": ["const32"]}, {"sgpr": 1, "kind": "const32"}]},
            "b": {"user_sgpr_reads": [{"sgpr": 0, "kind": {"k": 1}}]},
        })
        self.assertEqual(len(ranked), 2)
        self.assertEqual(sum(count for _, count, _ in ranked), 2)

    def test_cli_bom_json_inputs_load(self):
        with tempfile.TemporaryDirectory() as tmp:
            idx_path = os.path.join(tmp, "index.json")
            sig_path = os.path.join(tmp, "sig.json")
            with open(idx_path, "w", encoding="utf-8-sig", newline="\n") as f:
                json.dump({"ok": {"user_sgpr_reads": [{"sgpr": 0, "kind": "const32"}]}}, f)
            with open(sig_path, "w", encoding="utf-8-sig", newline="\n") as f:
                json.dump(ROOT_SIG, f)
            res = subprocess.run(
                [sys.executable, "-I", TOOL, idx_path, "--rootsig", sig_path],
                capture_output=True, text=True,
            )
            self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
            self.assertIn("1 compatible, 0 contradictory", res.stdout)

    def test_cli_console_model_empty_index_validates_rootsig(self):
        with tempfile.TemporaryDirectory() as tmp:
            idx_path = os.path.join(tmp, "index.json")
            bad_sig = os.path.join(tmp, "bad_sig.json")
            wide_sig = os.path.join(tmp, "wide_sig.json")
            with open(idx_path, "w", encoding="utf-8", newline="\n") as f:
                json.dump({}, f)
            with open(bad_sig, "w", encoding="utf-8", newline="\n") as f:
                json.dump({"params": [{"type": "bogus"}]}, f)
            # 17 console tables fit s4..s20 (cs) even though they would spill under s0 (34 dwords).
            with open(wide_sig, "w", encoding="utf-8", newline="\n") as f:
                json.dump({"params": [{"type": "table"} for _ in range(17)]}, f)
            res = subprocess.run(
                [sys.executable, "-I", TOOL, idx_path, "--rootsig", bad_sig, "--model", "console"],
                capture_output=True, text=True,
            )
            self.assertEqual(res.returncode, 1, res.stdout + res.stderr)
            self.assertIn("root signature refused", res.stdout)

            ok = subprocess.run(
                [sys.executable, "-I", TOOL, idx_path, "--rootsig", wide_sig, "--model", "console"],
                capture_output=True, text=True,
            )
            self.assertEqual(ok.returncode, 0, ok.stdout + ok.stderr)
            self.assertIn("0 shaders: 0 compatible, 0 contradictory", ok.stdout)

    def test_cli_console_model_rejects_missing_or_non_string_stage(self):
        with tempfile.TemporaryDirectory() as tmp:
            sig_path = os.path.join(tmp, "sig.json")
            with open(sig_path, "w", encoding="utf-8", newline="\n") as f:
                json.dump(ROOT_SIG, f)
            for label, entry in (
                ("missing", {"user_sgpr_reads": []}),
                ("null", {"stage": None, "user_sgpr_reads": []}),
                ("int", {"stage": 4, "user_sgpr_reads": []}),
            ):
                with self.subTest(case=label):
                    idx_path = os.path.join(tmp, f"index_{label}.json")
                    with open(idx_path, "w", encoding="utf-8", newline="\n") as f:
                        json.dump({"s": entry}, f)
                    res = subprocess.run(
                        [sys.executable, "-I", TOOL, idx_path, "--rootsig", sig_path, "--model", "console"],
                        capture_output=True, text=True,
                    )
                    self.assertEqual(res.returncode, 2, res.stdout + res.stderr)
                    self.assertIn("'stage' must be a string", res.stderr)
                    self.assertNotIn("Traceback", res.stderr)

    def test_cli_rejects_whitespace_paths(self):
        for argv in (["   ", "--rootsig", "sig.json"], ["idx.json", "--rootsig", "   "]):
            with self.subTest(argv=argv):
                res = subprocess.run([sys.executable, "-I", TOOL, *argv], capture_output=True, text=True)
                self.assertEqual(res.returncode, 2, res.stdout + res.stderr)
                self.assertIn("required", res.stderr)


if __name__ == "__main__":
    unittest.main()
