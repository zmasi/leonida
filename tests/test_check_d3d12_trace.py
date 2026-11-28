"""Run runtime/test/check_d3d12_trace.py through a fake runner, without a Windows build."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[1]
CHECK = ROOT / "runtime" / "test" / "check_d3d12_trace.py"

COMMAND_LIST_TRACE = [
    "ID3D12GraphicsCommandList::SetPipelineState called",
    "ID3D12GraphicsCommandList::SetGraphicsRootSignature called",
    "ID3D12GraphicsCommandList::SetMarker called",
    "ID3D12GraphicsCommandList3::SetProtectedResourceSession unsupported",
    "ID3D12GraphicsCommandList3::SetProtectedResourceSession unsupported",
    "ID3D12GraphicsCommandList::Close ok",
    "ID3D12GraphicsCommandList::Close failed",
    "ID3D12GraphicsCommandList::Reset ok",
    "ID3D12GraphicsCommandList::SetDescriptorHeaps called",
    "ID3D12GraphicsCommandList::SetGraphicsRootDescriptorTable called",
    "ID3D12GraphicsCommandList::SetComputeRootSignature called",
    "ID3D12GraphicsCommandList::Dispatch called",
    "ID3D12GraphicsCommandList::DrawInstanced called",
    "ID3D12GraphicsCommandList::DrawIndexedInstanced called",
    "ID3D12GraphicsCommandList::Close failed",
]


class CheckD3D12TraceRunnerTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.cwd = Path(directory.name)

    def check(self, runner_source: str, runner_timeout: int = 20) -> subprocess.CompletedProcess:
        (self.cwd / "runner.py").write_text(runner_source, encoding="utf-8", newline="\n")
        # Shorten the runner limit without touching the script: run its main() with RUNNER_TIMEOUT replaced.
        launch = ("import runpy, sys; timeout = int(sys.argv.pop(1)); script = sys.argv.pop(1); sys.argv[0] = script; "
                  "main = runpy.run_path(script)['main']; main.__globals__['RUNNER_TIMEOUT'] = timeout; main()")
        return subprocess.run(
            [sys.executable, "-I", "-B", "-c", launch, str(runner_timeout), str(CHECK), "fixture.exe",
             f"--runner={sys.executable}", "--runner=-I", "--runner=runner.py"],
            cwd=self.cwd, capture_output=True, text=True, timeout=60,
        )

    def test_trace_through_runner(self):
        lines = "".join(f"[d3d12-trace] #{n} {entry}\\n" for n, entry in enumerate(COMMAND_LIST_TRACE, 1))
        result = self.check(
            "import sys\n"
            "assert sys.argv[1] == 'fixture.exe', sys.argv\n"
            f"if '--trace-d3d12' in sys.argv[2:]:\n    sys.stderr.write('{lines}')\n"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"D3D12 trace verified with {len(COMMAND_LIST_TRACE)} ordered entries", result.stdout)

    def test_failing_fixture_reports_its_output(self):
        result = self.check("import sys\nprint('fixture output')\nsys.exit(3)\n")
        self.assertEqual(result.returncode, 1)
        self.assertIn("fixture.exe exited with 3", result.stderr)
        self.assertIn("fixture output", result.stderr)

    def test_runner_timeout_does_not_wait_for_a_child_holding_the_output(self):
        # A grandchild inherits the launcher's stdout and stderr and outlives the limit, as a Wine fixture can: the
        # check must still fail at about the limit, not when that child lets go of the output (PR #9920 review).
        started = time.monotonic()
        result = self.check(
            "import subprocess, sys, time\n"
            "subprocess.Popen([sys.executable, '-I', '-c', 'import time; time.sleep(30)'])\n"
            "time.sleep(60)\n",
            runner_timeout=2,
        )
        elapsed = time.monotonic() - started
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("fixture.exe: no exit within 2s", result.stderr)
        self.assertNotIn("Traceback", result.stderr)
        self.assertLess(elapsed, 15, "the check waited for the child that holds the output")

    @unittest.skipUnless(os.name == "posix", "requires POSIX process groups and flock")
    def test_runner_timeout_stops_the_fixture_it_started(self):
        import fcntl

        # Like a Wine launcher: the fixture runs in a child that holds the output pipes and outlives the limit.
        result = self.check(
            "import subprocess, sys, time\n"
            "child = subprocess.Popen([sys.executable, '-c', "
            "'import fcntl, sys, time; lock = open(\"fixture.lock\", \"w\"); fcntl.flock(lock, fcntl.LOCK_EX); "
            "print(\"fixture ready\", flush=True); time.sleep(60)'], stdout=subprocess.PIPE)\n"
            "child.stdout.readline()\n"
            "time.sleep(60)\n",
            runner_timeout=2,
        )
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("fixture.exe: no exit within 2s", result.stderr)
        self.assertNotIn("Traceback", result.stderr)
        # The lock is released once the fixture exits, even while its PID is still a zombie.
        with (self.cwd / "fixture.lock").open("r+") as lock:
            deadline = time.monotonic() + 2
            while True:
                try:
                    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    break
                except BlockingIOError:
                    if time.monotonic() >= deadline:
                        self.fail("the fixture outlived the runner timeout")
                    time.sleep(0.02)


if __name__ == "__main__":
    unittest.main()
