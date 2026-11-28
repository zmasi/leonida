"""The verdict logic of runtime/test/check_tiny_frame.py on synthetic output: no loader, GPU or game files."""
import contextlib
import importlib.util
import io
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location("check_tiny_frame", ROOT / "runtime" / "test" / "check_tiny_frame.py")
check = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(check)


def good_output(extra_trace=(), drop=None, binding="shim"):
    """What a passing run prints: the binding report, the numbered trace and the guest's verdict."""
    calls = [call for call in list(check.EXPECTED) if call != drop]
    calls[10:10] = extra_trace  # between the views and the list's commands
    lines = ["tiny_frame.exe  base=0000000140000000 size=61440 entry=+0x1000  shim=7 host=40 module=0 unimplemented=0",
             f"    {binding:<13} d3d12_xs.dll!D3D12XboxCreateDevice",
             "running tiny_frame.exe"]
    lines += [f"[d3d12-trace] #{number} {call}" for number, call in enumerate(calls, 1)]
    lines += ["tiny_frame: PASS all 32 pixels are the clear colour (64, 128, 191, 255)", "tiny_frame: OK",
              "game exited with code 0", ""]
    return "\n".join(lines)


class CheckOutputTest(unittest.TestCase):
    def test_a_passing_run_has_no_problems(self):
        self.assertEqual(check.check_output(0, good_output()), [])

    def test_crlf_output_and_calls_between_the_expected_ones_are_accepted(self):
        output = good_output(extra_trace=["XboxID3D12CommandQueue::GetDesc called"]).replace("\n", "\r\n")
        self.assertEqual(check.check_output(0, output), [])

    def test_nonzero_exit_code(self):
        self.assertIn("exit code 3", check.check_output(3, good_output()))

    def test_missing_verdict_and_failed_check(self):
        output = good_output().replace("tiny_frame: OK", "tiny_frame: FAIL Signal\ntiny_frame: FAILED")
        problems = check.check_output(0, output)
        self.assertIn("the guest did not print `tiny_frame: OK`", problems)
        self.assertIn("the guest reported a failed check", problems)

    def test_missing_loader_exit_line(self):
        output = good_output().replace("game exited with code 0", "game exited with code 1")
        self.assertIn("gta6rt did not report `game exited with code 0`", check.check_output(0, output))

    def test_host_binding_is_reported(self):
        problems = check.check_output(0, good_output(binding="host"))
        self.assertEqual(problems, ["d3d12_xs.dll!D3D12XboxCreateDevice was not bound to the shim"])

    def test_missing_call_names_the_first_one_out_of_order(self):
        problems = check.check_output(0, good_output(drop="ID3D12CommandQueue::Signal ok"))
        self.assertEqual(problems, ["the trace has no `ID3D12CommandQueue::Signal ok` at its place in the "
                                    "first-frame order"])

    def test_a_call_that_did_not_succeed_is_reported(self):
        problems = check.check_output(0, good_output(extra_trace=["ID3D12Device::CreateHeap unsupported"]))
        self.assertEqual(problems, ["traced calls that did not succeed: ID3D12Device::CreateHeap unsupported"])

    def test_a_gap_in_the_numbering_is_an_invalid_trace(self):
        output = good_output().replace("[d3d12-trace] #3 ", "[d3d12-trace] #4 ")
        problems = check.check_output(0, output)
        self.assertEqual(len(problems), 1)
        self.assertTrue(problems[0].startswith("invalid trace sequence at entry 3"))

    def test_no_trace_at_all(self):
        output = "\n".join(line for line in good_output().split("\n") if not line.startswith("[d3d12-trace]"))
        self.assertEqual(check.check_output(0, output), ["the trace has no `D3D12XboxCreateDevice ok` at its place "
                                                         "in the first-frame order"])


class SubsequenceTest(unittest.TestCase):
    def test_repeated_entries_need_as_many_occurrences(self):
        self.assertIsNone(check.ordered_subsequence(["a", "a", "b"], ["a", "x", "a", "b"]))
        self.assertEqual(check.ordered_subsequence(["a", "a", "b"], ["a", "b"]), "a")

    def test_order_matters(self):
        self.assertEqual(check.ordered_subsequence(["a", "b"], ["b", "a"]), "b")


class MainAndBomTests(unittest.TestCase):
    def test_parse_trace_and_check_output_strip_utf8_bom_on_line_one(self):
        self.assertEqual(check.parse_trace(["\ufeff[d3d12-trace] #1 Foo ok"]), [("Foo", "ok")])
        self.assertEqual(check.check_output(0, "\ufeff" + good_output()), [])

    def test_upfront_validation_rejects_blank_missing_and_dir_paths_with_exit_2(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            exe = root / "tool.exe"
            exe.write_bytes(b"")
            subdir = root / "dir"
            subdir.mkdir()
            with mock.patch.object(check.subprocess, "run", side_effect=AssertionError("called subprocess")):
                for argv in (["", str(exe)], [str(exe), ""], [str(exe), str(exe), "--probe", "  "]):
                    with self.subTest(blank_argv=argv), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as cm:
                        check.main(argv)
                    self.assertEqual(cm.exception.code, 2)
                for argv in (
                    [str(root / "missing.exe"), str(exe)],
                    [str(exe), str(subdir)],
                    [str(exe), str(exe), "--probe", str(root / "missing_probe.exe")],
                ):
                    with self.subTest(bad_argv=argv):
                        err = io.StringIO()
                        with contextlib.redirect_stderr(err):
                            rc = check.main(argv)
                        self.assertEqual(rc, 2)
                        self.assertIn("ERROR:", err.getvalue())

    def test_subprocess_errors_probe_failure_and_line_join(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            loader = root / "gta6rt.exe"
            guest = root / "tiny_frame.exe"
            probe = root / "probe.exe"
            for p in (loader, guest, probe):
                p.write_bytes(b"")

            for exc in (FileNotFoundError("missing"), subprocess.TimeoutExpired(["x"], 90)):
                with self.subTest(exc=type(exc).__name__):
                    err = io.StringIO()
                    with mock.patch.object(check.subprocess, "run", side_effect=exc), \
                            contextlib.redirect_stderr(err):
                        rc = check.main([str(loader), str(guest), "--probe", str(probe)])
                    self.assertEqual(rc, 1)
                    self.assertIn("FAIL:", err.getvalue())

            err_probe = io.StringIO()
            with mock.patch.object(check.subprocess, "run", return_value=subprocess.CompletedProcess(["p"], 3)), \
                    contextlib.redirect_stderr(err_probe):
                rc_probe = check.main([str(loader), str(guest), "--probe", str(probe)])
            self.assertEqual(rc_probe, 1)
            self.assertIn("exited with code 3", err_probe.getvalue())

            # Stdout without trailing newline joined with stderr without gluing lines together.
            full = good_output().rstrip("\n")
            head, tail = full.rsplit("\n", 1)
            out = io.StringIO()
            with mock.patch.object(
                check.subprocess,
                "run",
                return_value=subprocess.CompletedProcess(["run"], 0, stdout=head, stderr=tail + "\n"),
            ), contextlib.redirect_stdout(out):
                rc_ok = check.main([str(loader), str(guest)])
            self.assertEqual(rc_ok, 0)
            self.assertIn("OK", out.getvalue())


if __name__ == "__main__":
    unittest.main()
