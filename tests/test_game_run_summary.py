"""tools/game_run_summary.py on synthetic run folders: counts, endings, masked categories, and no log text printed.

    python -I tests/test_game_run_summary.py

Each test writes a small run folder (stderr.log, boot.jsonl, exit_code, seconds) by hand; no game files, no Wine,
no GPU.
"""
import contextlib
import io
import json
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

import game_run_summary as summary

# A line the summary must never print: it stands for text the title itself would put in a log.
SECRET = "Synthetic Title Text 42"

STDERR = [
    "[rt] loader: mapped 7 modules",
    "[rt] audio: serving the silent output endpoint (LEONIDA_AUDIO=silent): 48 kHz float 7.1, every sample discarded",
    "[d3d12-trace] #1 ID3D12Device::CreateCommandQueue ok",
    "[d3d12-trace] #2 ID3D12Device::CreateHeap failed",
    "[d3d12-trace] #3 ID3D12Device::CreateHeap failed",
    "[d3d12-trace] #4 SetMarker/BeginEvent with unknown metadata unsupported",
    "[d3d12-trace] #5 XboxID3D12CommandQueue::PresentX ok",
    "[d3d12-trace] #6 XboxID3D12CommandQueue::PresentX ok",
    "[d3d12-trace] #7 XboxID3D12CommandQueue::Slot40 console-only",
    "[rt] slot 31: 12 pages at 0x1f000",
    "[rt] slot 32: 3 pages",
    f"[rt] game dialog: {SECRET}",
    f"[rt] {SECRET}, said the title; with a colon: here",
    "[rt] present: pacing summary frames=2 samples=2 target_us=16666 mean_us=16666.0 drift_us=+0.0 p50_us=1 "
    "p95_us=1 p99_us=1 latency_samples=0 latency_p50_us=0 latency_p95_us=0 latency_p99_us=0",
    SECRET,
]
STARTUP = [
    {"seq": 1, "t": 0.0, "kind": "trace_open", "pid": 1},
    {"seq": 2, "kind": "debug_string", "text": SECRET},
    {"seq": 3, "kind": "exception", "code": "0xc0000005", "at": "game.exe+0x1234", "access": "read",
     "address": "0x10"},
    {"seq": 4, "kind": "exception", "code": "0x406d1388", "at": SECRET},
    {"seq": 5, "kind": "file_open_failed", "path": SECRET, "error": 2, "caller": "game.exe+0x10"},
]


class GameRunSummaryTest(unittest.TestCase):
    def run_folder(self, stderr=STDERR, startup=STARTUP, exit_code=None, seconds=None, tail=""):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        run = tmp.name
        with open(os.path.join(run, "stderr.log"), "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(stderr) + "\n")
        with open(os.path.join(run, "boot.jsonl"), "w", encoding="utf-8", newline="\n") as f:
            f.write("".join(json.dumps(e) + "\n" for e in startup) + tail)
        for name, value in (("exit_code", exit_code), ("seconds", seconds)):
            if value is not None:
                with open(os.path.join(run, name), "w", encoding="utf-8", newline="\n") as f:
                    f.write(f"{value}\n")
        return run

    def printed(self, *argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(summary.main(list(argv)), 0)
        return out.getvalue()

    def test_counts(self):
        s = summary.summarize(self.run_folder(exit_code=124, seconds=121), limit_seconds=120)
        self.assertEqual(s["seconds"], 121)
        self.assertEqual(s["d3d12_calls"], 7)
        self.assertEqual(s["d3d12_outcomes"], {"failed": 2, "ok": 3, "unsupported": 1, "console-only": 1})
        self.assertEqual(s["presents"], 2)
        self.assertEqual(s["presents_by_method"], {"XboxID3D12CommandQueue::PresentX ok": 2})
        self.assertEqual(s["paced_frames"], 2)
        self.assertEqual(s["failing_calls"]["failed"], {"total": 2, "methods": 1,
                                                        "top": {"ID3D12Device::CreateHeap": 2}})
        self.assertEqual(s["failing_calls"]["unsupported"]["top"], {"SetMarker/BeginEvent with unknown metadata": 1})
        self.assertEqual(s["silent_audio_endpoint"], 1)
        self.assertEqual(s["exceptions"], 2)
        self.assertEqual(s["exception_codes"], {"0xc0000005": 1, "0x406d1388": 1})
        self.assertEqual(s["last_exception"], {"code": "0x406d1388", "at": None})
        self.assertEqual(s["startup_events"], {"exception": 2, "trace_open": 1, "debug_string": 1,
                                               "file_open_failed": 1})

    def test_categories_mask_numbers_and_hide_free_text(self):
        s = summary.summarize(self.run_folder())
        self.assertEqual(s["rt_lines"], 7)
        self.assertEqual(s["rt_categories"], {"slot #": 2, "loader": 1, "audio": 1, "game dialog": 1,
                                              "(other)": 1, "present": 1})
        self.assertEqual(summary.rt_category("pool 0x1f00 stats: x"), "pool # stats")
        self.assertEqual(summary.rt_category("no colon at all"), "(other)")
        self.assertEqual(summary.rt_category("d3d12_xs: x"), "d3d12_xs")

    def test_endings(self):
        cases = [
            (dict(exit_code=124), [], "time limit (120 s) reached, stopped (exit 124)"),
            (dict(exit_code=137), [], "time limit (120 s) reached, stopped (exit 137)"),
            (dict(exit_code=124, seconds=120), [], "time limit (120 s) reached, stopped (exit 124)"),
            (dict(exit_code=124, seconds=4), [{"kind": "exit", "code": 124}],
             "guest returned 124 (process exit 124)"),
            (dict(exit_code=137, seconds=4), [], "process exit 137 (0x00000089), no exit event"),
            (dict(exit_code=0), [{"kind": "exit", "code": 3}], "guest returned 3 (process exit 0)"),
            (dict(exit_code=1), [{"kind": "error", "what": SECRET}], "loader error event (process exit 1)"),
            (dict(exit_code=-1073741819), [], "process exit -1073741819 (0xc0000005), no exit event"),
            (dict(), [], "no exit code recorded"),
        ]
        for files, events, want in cases:
            with self.subTest(want=want):
                run = self.run_folder(startup=[{"kind": "trace_open"}] + events, **files)
                self.assertIn(want, summary.summarize(run, limit_seconds=120)["end"])

    def test_a_truncated_trace_line_is_counted_not_fatal(self):
        s = summary.summarize(self.run_folder(tail='{"kind": "excep'))
        self.assertEqual(s["startup_bad_lines"], 1)
        self.assertEqual(s["exceptions"], 2)

    def test_an_empty_folder_gives_zero_counts(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        text = self.printed(tmp.name)
        self.assertIn("D3D12 calls traced: 0 (none)", text)
        self.assertIn("silent audio endpoint: not seen", text)

    def test_the_printed_report_never_quotes_the_logs(self):
        run = self.run_folder(exit_code=124, seconds=121)
        for argv in ([run, "--limit-seconds", "120"], [run, "--json"]):
            with self.subTest(argv=argv[1:]):
                text = self.printed(*argv)
                self.assertNotIn("Synthetic", text)
                self.assertNotIn("12 pages", text)
                self.assertNotIn("said the title", text)
        text = self.printed(run, "--limit-seconds", "120")
        self.assertIn("run time: 121 s; end: time limit (120 s) reached", text)
        self.assertIn("presents: 2 (XboxID3D12CommandQueue::PresentX ok: 2); pacing summary frames 2", text)
        self.assertIn("failed: 2 -> ID3D12Device::CreateHeap 2", text)
        self.assertIn("silent audio endpoint: served (1 lines)", text)
        self.assertIn("last: 0x406d1388 at (location not printed)", text)
        self.assertEqual(json.loads(self.printed(run, "--json"))["presents"], 2)

    def test_top_limits_the_methods_listed(self):
        lines = [f"[d3d12-trace] #{i} M{i % 5}::Call failed" for i in range(1, 21)]
        text = self.printed(self.run_folder(stderr=lines), "--top", "2")
        self.assertIn("failed: 20 -> ", text)
        self.assertIn("(+3 more methods)", text)

    def test_bad_arguments_fail(self):
        for argv in (["/nonexistent/run/folder"], [HERE, "--top", "0"]):
            with self.subTest(argv=argv), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as raised:
                    summary.main(argv)
                self.assertEqual(raised.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
