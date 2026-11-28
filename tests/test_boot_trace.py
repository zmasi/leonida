"""Unit tests for tools/boot_trace.py on synthetic traces (no game data, no Windows, no GPU)."""

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

import boot_trace as bt

STARTUP = [
    {"seq": 1, "t": 0.0, "kind": "trace_open", "pid": 123},
    {
        "seq": 2,
        "kind": "map",
        "module": "game.exe",
        "base": "0x140000000",
        "size": "0x1000",
        "relocated": False,
    },
    {
        "seq": 3,
        "kind": "map",
        "module": "x.dll",
        "base": "0x180000000",
        "size": "0x2000",
        "relocated": True,
    },
    {
        "seq": 4,
        "kind": "tls_callback",
        "module": "x.dll",
        "at": "x.dll+0x10",
        "reason": 1,
    },
    {"seq": 5, "kind": "dllmain", "module": "x.dll", "result": 1},
    {"seq": 6, "kind": "dllmain", "module": "y.dll", "result": 0},
    {
        "seq": 7,
        "kind": "unimplemented_call",
        "import": "x.dll!Foo",
        "caller": "game.exe+0x10",
    },
    {
        "seq": 8,
        "kind": "unimplemented_call",
        "import": "x.dll!Foo",
        "caller": "game.exe+0x20",
    },
    {
        "seq": 9,
        "kind": "unimplemented_call",
        "import": "y.dll!Bar",
        "caller": "game.exe+0x30",
    },
    {"seq": 10, "kind": "debug_string", "text": "secret game text"},
    {
        "seq": 11,
        "kind": "exception",
        "code": "0xc0000005",
        "at": "game.exe+0x40",
        "access": "read",
        "address": "0x0",
        "stack": ["game.exe+0x40", "game.exe+0x120"],
    },
    {
        "seq": 12,
        "kind": "exception",
        "code": "0xe06d7363",
        "at": "game.exe+0x30",
        "cpp": True,
    },
    {"seq": 13, "kind": "future_event", "detail": "a kind this version does not know"},
    {"seq": 14, "kind": "exit", "code": 0},
]

D3D12 = [
    "[rt] ordinary diagnostic",
    "[d3d12-trace] #1 D3D12XboxCreateDevice ok",
    "[d3d12-trace] #2 CreateCommandQueue failed",
    "[d3d12-trace] #3 Foo unsupported",
    "[d3d12-trace] #4 D3D12XboxCreateDevice ok",
    "[d3d12-trace] #5 SetMarker/BeginEvent with unknown metadata unsupported",
    "[d3d12-trace] #6 MapTiles refused",
    "[d3d12-trace] #7 CreateRootSignature called",
    "[d3d12-trace] #8 SetMarker ignored",
    "d3d12_test.exe : [d3d12-trace] #9 ID3D12CommandQueue::ExecuteCommandLists called",
]


class BootTraceTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.startup = os.path.join(self.tmp.name, "boot.jsonl")
        self.d3d12 = os.path.join(self.tmp.name, "d3d12.log")

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, path, events, newline="\n"):
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.writelines(json.dumps(event) + newline for event in events)

    def write_lines(self, path, lines):
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(lines) + "\n")

    def test_startup_summary(self):
        self.write(self.startup, STARTUP)
        trace = bt.parse_startup(self.startup)
        self.assertEqual(trace["pid"], 123)
        self.assertEqual(trace["events"], len(STARTUP))
        self.assertEqual(trace["bad_lines"], 0)
        self.assertEqual(trace["kinds"]["map"], 2)
        self.assertEqual(trace["kinds"]["future_event"], 1)
        self.assertEqual([m["module"] for m in trace["modules"]], ["game.exe", "x.dll"])
        self.assertEqual(trace["tls_callbacks"], 1)
        self.assertEqual(
            trace["dllmain"], {"ok": 1, "failed": 1, "failed_modules": ["y.dll"]}
        )
        self.assertEqual(
            [u["import"] for u in trace["unimplemented"]], ["x.dll!Foo", "y.dll!Bar"]
        )
        self.assertEqual(trace["unimplemented"][0]["caller"], "game.exe+0x10")
        self.assertEqual(trace["unimplemented_by_dll"], {"x.dll": 1, "y.dll": 1})
        self.assertEqual(trace["debug_strings"], 1)
        self.assertEqual(trace["exception_count"], 2)
        self.assertEqual(
            trace["exceptions"][0]["stack"], ["game.exe+0x40", "game.exe+0x120"]
        )
        self.assertIs(trace["exceptions"][1]["cpp"], True)
        self.assertEqual(trace["exit"], 0)

    def test_unimplemented_calls_rank_by_frequency_and_preserve_first_caller(self):
        names = ("x.dll!Foo", "y.dll!Bar", "y.dll!Bar", "y.dll!Bar")
        self.write(self.startup, [
            {"kind": "unimplemented_call", "import": name, "caller": f"synthetic+{i}"}
            for i, name in enumerate(names)
        ])
        trace = bt.parse_startup(self.startup, limit=1)
        self.assertEqual(trace["unimplemented_call_counts"], {"x.dll!Foo": 1, "y.dll!Bar": 3})
        self.assertEqual(trace["unimplemented_by_dll"], {"x.dll": 1, "y.dll": 1})
        report = bt.format_text(bt.summarize(trace, None), 1)
        self.assertIn("unimplemented: 2 unique, 4 calls", report)
        self.assertIn("y.dll!Bar from synthetic+1 (3 calls)", report)
        self.assertNotIn("x.dll!Foo from", report)

    def test_exception_limit_keeps_final_observation(self):
        early = [
            {"kind": "exception", "code": "0xe06d7363", "at": f"synthetic.exe+0x{0x1100 + i:x}", "cpp": True}
            for i in range(10)
        ]
        final = {
            "kind": "exception",
            "code": "0xc0000005",
            "at": "synthetic.exe+0x2010",
            "access": "read",
            "address": "0x0",
            "stack": ["synthetic.exe+0x2010", "synthetic.exe+0x1000"],
        }
        for limit, preceding in ((1, early), (10, early), (10, early[:-1])):
            with self.subTest(limit=limit, count=len(preceding) + 1):
                self.write(self.startup, [STARTUP[0], *preceding, final])
                output = io.StringIO()
                with contextlib.redirect_stdout(output):
                    self.assertEqual(bt.main([self.startup, "--limit", str(limit), "--json"]), 0)
                summary = json.loads(output.getvalue())
                startup = summary["startup"]
                self.assertEqual(startup["exception_count"], len(preceding) + 1)
                self.assertEqual(
                    [exc["at"] for exc in startup["exceptions"]],
                    [exc["at"] for exc in preceding[:limit - 1]] + [final["at"]],
                )
                self.assertEqual(startup["exceptions"][-1]["stack"], final["stack"])
                self.assertIn("synthetic.exe+0x2010 (read 0x0)", bt.format_text(summary, limit))

    def test_debug_text_is_not_printed(self):
        self.write(self.startup, STARTUP)
        text = bt.format_text(bt.summarize(bt.parse_startup(self.startup), None), 10)
        self.assertIn("debug strings: 1", text)
        self.assertNotIn("secret game text", text)

    def test_truncated_line_is_counted_not_fatal(self):
        self.write(self.startup, STARTUP)
        with open(self.startup, "a", encoding="utf-8", newline="\n") as f:
            f.write('{"seq": 15, "kind":')
        trace = bt.parse_startup(self.startup)
        self.assertEqual(trace["events"], len(STARTUP))
        self.assertEqual(trace["bad_lines"], 1)
        self.assertEqual(trace["bad_line_numbers"], [len(STARTUP) + 1])

    def test_crlf_and_non_object_lines_are_counted(self):
        self.write(
            self.startup,
            [
                {"seq": 1, "kind": "trace_open", "pid": 7},
                {"seq": 2, "kind": "exit", "code": 0},
            ],
            newline="\r\n",
        )
        with open(self.startup, "a", encoding="utf-8", newline="\n") as f:
            f.write("null\n")
        trace = bt.parse_startup(self.startup)
        self.assertEqual(trace["events"], 2)
        self.assertEqual(trace["bad_lines"], 1)

    def test_malformed_exit_records_preserve_the_last_valid_code(self):
        for code in (0, -7):
            with self.subTest(code=code):
                self.write(self.startup, [
                    {"kind": "trace_open", "pid": 123},
                    {"kind": "exit", "code": code},
                    {"kind": "exit"},
                    {"kind": "exit", "code": "0"},
                    {"kind": "exit", "code": True},
                    {"kind": "exit", "code": None},
                    {"kind": "exit", "code": 0.0},
                    {"kind": "debug_string", "text": "synthetic"},
                ])
                trace = bt.parse_startup(self.startup)
                self.assertEqual(trace["bad_lines"], 5)
                self.assertEqual(trace["bad_line_numbers"], [3, 4, 5, 6, 7])
                self.assertEqual(trace["events"], 3)
                self.assertEqual(trace["kinds"]["exit"], 1)
                self.assertIs(type(trace["exit"]), int)
                self.assertEqual(trace["exit"], code)
                self.assertEqual(trace["debug_strings"], 1)

    def test_d3d12_summary(self):
        self.write_lines(self.d3d12, D3D12)
        trace = bt.parse_d3d12(self.d3d12)
        self.assertEqual(trace["calls"], 9)
        self.assertEqual(trace["ignored_lines"], 1)
        self.assertEqual(
            trace["outcomes"],
            {
                "ok": 2,
                "failed": 1,
                "unsupported": 2,
                "refused": 1,
                "called": 2,
                "ignored": 1,
            },
        )
        self.assertEqual(
            trace["methods"],
            [
                "D3D12XboxCreateDevice",
                "CreateCommandQueue",
                "Foo",
                "SetMarker/BeginEvent with unknown metadata",
                "MapTiles",
                "CreateRootSignature",
                "SetMarker",
                "ID3D12CommandQueue::ExecuteCommandLists",
            ],
        )
        self.assertEqual(
            trace["first"]["D3D12XboxCreateDevice"], {"seq": 1, "outcome": "ok"}
        )
        self.assertEqual(
            trace["first"]["ID3D12CommandQueue::ExecuteCommandLists"],
            {"seq": 9, "outcome": "called"},
        )
        self.assertEqual(trace["failed"], [{"seq": 2, "method": "CreateCommandQueue"}])
        self.assertEqual(
            trace["unsupported"],
            [
                {"seq": 3, "method": "Foo"},
                {"seq": 5, "method": "SetMarker/BeginEvent with unknown metadata"},
            ],
        )
        self.assertEqual(trace["refused"], [{"seq": 6, "method": "MapTiles"}])
        self.assertEqual(trace["last_seq"], 9)
        self.assertEqual(trace["sequence_break_count"], 0)
        self.assertEqual(trace["sequence_breaks"], [])

    def test_repeated_method_failures_are_counted_beyond_the_sample_limit(self):
        self.write_lines(self.d3d12, [
            "[d3d12-trace] #1 CreatePipelineState ok",
            "[d3d12-trace] #2 CreatePipelineState failed",
            "[d3d12-trace] #3 CreatePipelineState failed",
            "[d3d12-trace] #4 ExecuteCommandLists partial",
        ])
        trace = bt.parse_d3d12(self.d3d12, limit=1)
        self.assertEqual(trace["method_outcomes"], {
            "CreatePipelineState": {"ok": 1, "failed": 2},
            "ExecuteCommandLists": {"partial": 1},
        })
        self.assertEqual(len(trace["failed"]), 1)
        self.assertIn("CreatePipelineState: failed 2", bt.format_text({"startup": None, "d3d12": trace}, 1))

    def test_sequence_breaks_count_boundaries_and_limit_examples(self):
        self.write_lines(self.d3d12, [
            "[d3d12-trace] #4 Foo called",
            "[rt] ordinary diagnostic",
            "[d3d12-trace] #5 Foo called",
            "[d3d12-trace] #12 Foo called",
            "[d3d12-trace] #1 Foo called",
            "[d3d12-trace] #1 Foo failed",
            "[d3d12-trace] #2 Foo called",
        ])
        examples = [
            {"expected": 1, "observed": 4},
            {"expected": 6, "observed": 12},
            {"expected": 13, "observed": 1},
            {"expected": 2, "observed": 1},
        ]
        for limit in (2, 4):
            with self.subTest(limit=limit):
                output = io.StringIO()
                with contextlib.redirect_stdout(output):
                    self.assertEqual(bt.main(["--d3d12", self.d3d12, "--json", "--limit", str(limit)]), 0)
                summary = json.loads(output.getvalue())
                trace = summary["d3d12"]
                self.assertEqual(trace["sequence_break_count"], 4)
                self.assertEqual(trace["sequence_breaks"], examples[:limit])
                self.assertEqual(trace["calls"], 6)
                self.assertEqual(trace["outcomes"], {"called": 5, "failed": 1})
                self.assertEqual(trace["last_seq"], 2)
                self.assertEqual(trace["ignored_lines"], 1)
                text = bt.format_text(summary, limit)
                self.assertIn("sequence breaks: 4", text)
                self.assertIn("expected #6, observed #12", text)
                self.assertEqual(text.count("observed #"), limit)

    def test_powershell_error_record_prefix_keeps_the_first_call(self):
        self.write_lines(
            self.d3d12,
            [
                "d3d12_test.exe : [d3d12-trace] #1 ID3D12GraphicsCommandList::SetPipelineState called",
                "At line:1 char:1",
                "+ & '.\\d3d12_test.exe' --trace-d3d12 2> log",
            ],
        )
        trace = bt.parse_d3d12(self.d3d12)
        self.assertEqual(trace["calls"], 1)
        self.assertEqual(
            trace["first"]["ID3D12GraphicsCommandList::SetPipelineState"],
            {"seq": 1, "outcome": "called"},
        )

    def test_d3d12_text_report_names_refused_and_spaced_methods(self):
        self.write_lines(self.d3d12, D3D12)
        text = bt.format_text(bt.summarize(None, bt.parse_d3d12(self.d3d12)), 10)
        self.assertIn("refused: #6 MapTiles", text)
        self.assertIn("#5 SetMarker/BeginEvent with unknown metadata", text)
        self.assertIn("ID3D12CommandQueue::ExecuteCommandLists", text)
        self.assertIn("[rt] diagnostics are not summarized", text)
        self.assertNotIn("sequence breaks", text)

    def test_partial_submissions_are_named_in_text_and_json(self):
        method = "ID3D12CommandQueue::ExecuteCommandLists"
        self.write_lines(self.d3d12, [
            f"[d3d12-trace] #1 {method} ok",
            f"[d3d12-trace] #2 {method} partial",
            f"[d3d12-trace] #3 {method} partial",
        ])
        trace = bt.parse_d3d12(self.d3d12, limit=1)
        self.assertEqual(trace["first"][method], {"seq": 1, "outcome": "ok"})
        self.assertEqual(trace["outcomes"]["partial"], 2)
        text = bt.format_text(bt.summarize(None, trace), 1)
        self.assertIn(f"  partial: #2 {method}", text.splitlines())
        self.assertNotIn(f"  partial: #3 {method}", text.splitlines())

        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            self.assertEqual(bt.main(["--d3d12", self.d3d12, "--limit", "1", "--json"]), 0)
        parsed = json.loads(stdout.getvalue())["d3d12"]
        self.assertEqual(parsed["partial"], [{"seq": 2, "method": method}])
        self.assertEqual(parsed["outcomes"]["partial"], 2)

    def test_json_cli(self):
        self.write(self.startup, STARTUP)
        self.write_lines(self.d3d12, D3D12)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(
                bt.main([self.startup, "--d3d12", self.d3d12, "--json"]), 0
            )
        data = json.loads(out.getvalue())
        self.assertEqual(data["startup"]["pid"], 123)
        self.assertEqual(data["d3d12"]["calls"], 9)

    def test_counts_only_json_omits_raw_trace_fields(self):
        self.write(self.startup, [
            {"kind": "exception", "code": "private-code", "address": "private-address",
             "stack": ["private-stack"]},
            {"kind": "file_open_failed", "path": "private-file", "caller": "private-caller"},
            {"kind": "error", "what": "private-message"},
            {"kind": "private-kind"},
        ])
        self.write_lines(self.d3d12, [
            "[d3d12-trace] #1 private-method failed",
            "[d3d12-trace] #2 private-method private-outcome",
        ])
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(bt.main([self.startup, "--d3d12", self.d3d12, "--counts-only"]), 0)
        text = out.getvalue()
        self.assertNotIn("private-", text)
        self.assertNotIn("boot.jsonl", text)
        self.assertNotIn("d3d12.log", text)
        data = json.loads(text)
        self.assertEqual(data["startup"]["exception_count"], 1)
        self.assertEqual(data["startup"]["events"], 4)
        self.assertEqual(data["d3d12"]["outcomes"], {"failed": 1, "other": 1})
        self.assertEqual(data["d3d12"]["calls"], 2)

    def test_counts_only_redacts_missing_input_paths_from_stderr(self):
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.assertEqual(bt.main([self.startup, "--counts-only"]), 2)
        self.assertIn("startup trace unavailable", err.getvalue())
        self.assertNotIn("boot.jsonl", err.getvalue())

    def test_missing_and_empty_files_are_errors(self):
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.assertEqual(bt.main([os.path.join(self.tmp.name, "missing.jsonl")]), 2)
            self.write(self.startup, [])
            self.assertEqual(bt.main([self.startup]), 2)
            self.assertEqual(bt.main(["--limit", "0", self.startup]), 2)
        self.assertIn("cannot read", err.getvalue())

    def test_negative_limit_is_an_error(self):
        self.write(self.startup, STARTUP)
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.assertEqual(bt.main([self.startup, "--limit", "-1"]), 2)
        self.assertIn("--limit", err.getvalue())

    def test_attach_failure_is_reported_before_any_d3d12_call(self):
        self.write(self.startup, [{"kind": "error", "what": "guest.dll: DllMain returned FALSE"}])
        self.write_lines(self.d3d12, ["error: guest.dll: DllMain returned FALSE"])
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            self.assertEqual(bt.main([self.startup, "--d3d12", self.d3d12]), 2)
        self.assertIn("error: guest.dll: DllMain returned FALSE", out.getvalue())
        self.assertIn("no [d3d12-trace] line", err.getvalue())

    def test_missing_startup_preserves_d3d12_json(self):
        self.write_lines(self.d3d12, ["[d3d12-trace] #1 CreateCommandQueue failed"])
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            self.assertEqual(bt.main([self.startup, "--d3d12", self.d3d12, "--json"]), 2)
        self.assertIn("cannot read", err.getvalue())
        self.assertEqual(json.loads(out.getvalue()), bt.summarize(None, bt.parse_d3d12(self.d3d12)))

    def test_no_report_when_neither_input_can_be_read(self):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            self.assertEqual(bt.main([self.startup, "--d3d12", self.d3d12, "--json"]), 2)
        self.assertEqual(out.getvalue(), "")
        self.assertIn(self.startup, err.getvalue())
        self.assertIn(self.d3d12, err.getvalue())

    def test_last_recorded_d3d12_call_can_repeat_an_earlier_method(self):
        self.write_lines(self.d3d12, [
            "[d3d12-trace] #1 CreateCommandAllocator ok",
            "[d3d12-trace] #2 CreateCommittedResource failed",
            "[d3d12-trace] #3 CreateCommandAllocator ok",
            "[rt] a runtime diagnostic after the last call",
            "[d3d12-trace] #4",
        ])
        trace = bt.parse_d3d12(self.d3d12, limit=1)
        self.assertEqual(trace["ignored_lines"], 2)
        self.assertEqual(trace["first"]["CreateCommandAllocator"], {"seq": 1, "outcome": "ok"})
        self.assertEqual(trace["failed"], [{"seq": 2, "method": "CreateCommittedResource"}])

        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(bt.main(["--d3d12", self.d3d12, "--limit", "1", "--json"]), 0)
        parsed = json.loads(output.getvalue())["d3d12"]
        self.assertEqual(parsed["last_seq"], 3)
        self.assertEqual(parsed["last_call"], {"seq": 3, "method": "CreateCommandAllocator", "outcome": "ok"})
        text = bt.format_text(bt.summarize(None, trace), 1)
        self.assertIn("  last recorded call: #3 CreateCommandAllocator ok", text.splitlines())

    def test_unterminated_d3d12_outcome_must_be_complete(self):
        first = b"[d3d12-trace] #1 CreateCommandAllocator ok\n"
        second = b"[d3d12-trace] #2 CreateCommittedResource "
        for ending, seq, ignored, outcomes in (
            (b"fai", 1, 1, {"ok": 1}),
            (b"failed", 2, 0, {"ok": 1, "failed": 1}),
            (b"ignored", 2, 0, {"ok": 1, "ignored": 1}),
            (b"future-result\n", 2, 0, {"ok": 1, "future-result": 1}),
        ):
            with self.subTest(ending=ending):
                with open(self.d3d12, "wb") as fh:
                    fh.write(first + second + ending)
                trace = bt.parse_d3d12(self.d3d12)
                self.assertEqual(trace["last_call"]["seq"], seq)
                self.assertEqual(trace["last_call"]["outcome"], next(reversed(outcomes)))
                self.assertEqual(trace["last_seq"], seq)
                self.assertEqual(trace["calls"], seq)
                self.assertEqual(trace["outcomes"], outcomes)
                self.assertEqual(trace["ignored_lines"], ignored)
                self.assertEqual(trace["sequence_break_count"], 0)

    def test_d3d12_log_without_trace_lines_is_an_error(self):
        self.write_lines(self.d3d12, ["[rt] one", "[rt] two"])
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.assertEqual(bt.main(["--d3d12", self.d3d12]), 2)
        self.assertIn("no [d3d12-trace] line", err.getvalue())

    def test_utf16_logs_keep_both_wrapped_and_unwrapped_calls(self):
        lines = ("synthetic.exe : [d3d12-trace] #1 First called\r\n"
                 "[d3d12-trace] #2 Second ok\r\n")
        for encoding, bom in (("utf-16-le", b"\xff\xfe"), ("utf-16-be", b"\xfe\xff")):
            with self.subTest(encoding=encoding):
                with open(self.d3d12, "wb") as f:
                    f.write(bom + lines.encode(encoding))
                output = io.StringIO()
                errors = io.StringIO()
                with contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
                    result = bt.main(["--d3d12", self.d3d12, "--json"])
                self.assertEqual(result, 0, errors.getvalue())
                trace = json.loads(output.getvalue())["d3d12"]
                self.assertEqual(trace["calls"], 2)
                self.assertEqual(trace["methods"], ["First", "Second"])

    def test_utf8_bom_is_stripped(self):
        with open(self.d3d12, "wb") as f:
            f.write(b"\xef\xbb\xbf" + b"[d3d12-trace] #1 Foo ok\n")
        trace = bt.parse_d3d12(self.d3d12)
        self.assertEqual(trace["calls"], 1)
        self.assertEqual(trace["methods"], ["Foo"])

    def test_text_report_has_the_triage_units(self):
        self.write(self.startup, STARTUP)
        text = bt.format_text(bt.summarize(bt.parse_startup(self.startup), None), 10)
        for needle in (
            "pid 123",
            "modules (2)",
            "game.exe, x.dll (relocated)",
            "dllmain: 1 ok, 1 failed",
            "unimplemented: 2 unique",
            "x.dll!Foo from game.exe+0x10",
            "by dll (unique imports): x.dll 1, y.dll 1",
            "exceptions: 2",
            "code 0xc0000005 at game.exe+0x40 (read 0x0)",
            "code 0xe06d7363 at game.exe+0x30 (C++ throw)",
            "stack: game.exe+0x40, game.exe+0x120",
            "exit: 0",
        ):
            self.assertIn(needle, text)

    def test_failed_file_opens_and_skipped_int3_are_listed(self):
        self.write(
            self.startup,
            [
                {"seq": 1, "kind": "trace_open", "pid": 1},
                {"seq": 2, "kind": "file_open_failed", "api": "CreateFileW", "path": "GAME:/probe/missing.bin", "error": 2, "caller": "game.exe+0x20"},
                {"seq": 3, "kind": "int3_skipped", "at": "game.exe+0x1330"},
            ],
        )
        trace = bt.parse_startup(self.startup)
        self.assertEqual(trace["file_open_failed"], [{"path": "GAME:/probe/missing.bin", "error": 2, "caller": "game.exe+0x20"}])
        self.assertEqual(trace["int3_skipped"], ["game.exe+0x1330"])
        text = bt.format_text(bt.summarize(trace, None), 10)
        self.assertIn("failed file opens: 1", text)
        self.assertIn("GAME:/probe/missing.bin error 2 from game.exe+0x20", text)
        self.assertIn("game.exe+0x1330", text)

    def test_last_observed_tls_callback_in_incomplete_startup(self):
        first = {"kind": "tls_callback", "module": "first.dll", "at": "first.dll+0x1100", "reason": 1}
        for fields, reason, phase in (
            ({"reason": 1}, 1, "process attach"),
            ({"reason": 0}, 0, "process detach"),
            ({}, None, "phase unknown"),
        ):
            with self.subTest(phase=phase):
                last = {"kind": "tls_callback", "module": "fixture.exe", "at": "fixture.exe+0x2200", **fields}
                self.write(self.startup, [STARTUP[0], first, last])
                with open(self.startup, "a", encoding="utf-8", newline="\n") as f:
                    f.write('{"kind":"tls_callback","at":"truncated')

                output = io.StringIO()
                with contextlib.redirect_stdout(output):
                    self.assertEqual(bt.main([self.startup, "--json"]), 0)
                summary = json.loads(output.getvalue())
                startup = summary["startup"]
                self.assertEqual(startup["tls_callbacks"], 2)
                self.assertEqual(startup["bad_lines"], 1)
                self.assertIsNone(startup["exit"])
                self.assertEqual(
                    startup["last_tls_callback"],
                    {"module": "fixture.exe", "at": "fixture.exe+0x2200", "reason": reason},
                )
                text = bt.format_text(summary, 10)
                self.assertIn("tls callbacks: 2", text)
                self.assertIn(f"last observed TLS callback: fixture.exe+0x2200 ({phase})", text)

    def physical_pages_reports(self, observations, limit):
        self.write(self.startup, [
            {"seq": seq, "t": 0.0, "kind": "xmem_physical_pages", **fields}
            for seq, fields in enumerate(observations, 1)
        ])
        args = [self.startup, "--limit", str(limit)]
        text, report = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(text):
            self.assertEqual(bt.main(args), 0)
        with contextlib.redirect_stdout(report):
            self.assertEqual(bt.main([*args, "--json"]), 0)
        startup = json.loads(report.getvalue())["startup"]
        self.assertEqual(startup["kinds"]["xmem_physical_pages"], len(observations))
        self.assertIn(f"  XMem physical page observations: {len(observations)}", text.getvalue())
        return text.getvalue(), startup["xmem_physical_pages"]

    def test_single_xmem_physical_pages_observation(self):
        observation = {
            "api": "XMemAllocatePhysicalPages", "attributes": "0x0", "pages": 0, "caller": "0x20",
        }
        text, records = self.physical_pages_reports([observation], 1)
        self.assertEqual(records, [observation])
        self.assertIn("    XMemAllocatePhysicalPages attributes 0x0 requested 0 pages from 0x20\n", text)
        self.assertNotIn("more", text)

    def test_xmem_physical_pages_observations_keep_trace_order_and_full_count(self):
        observations = [
            {"api": "XMemAllocatePhysicalPages", "attributes": attributes, "pages": pages, "caller": caller}
            for attributes, pages, caller in (
                ("0xffffffffffffffff", (1 << 64) - 1, "fixture.dll+0x30"),
                ("0x10", 2, "fixture.dll+0x20"),
                ("0x20", 1, "fixture.dll+0x10"),
            )
        ]
        for limit in (0, 2, 3):
            with self.subTest(limit=limit):
                text, records = self.physical_pages_reports(observations, limit)
                self.assertEqual(records, observations[:limit])
                self.assertEqual(
                    [line for line in text.splitlines() if line.startswith("    XMemAllocatePhysicalPages ")],
                    [f"    {item['api']} attributes {item['attributes']} requested {item['pages']} pages"
                     f" from {item['caller']}" for item in observations[:limit]],
                )
                if limit < len(observations):
                    self.assertIn(f"    ... {len(observations) - limit} more", text)
                else:
                    self.assertNotIn("more", text)

    def test_counts_only_includes_all_xmem_physical_page_observations(self):
        self.write(self.startup, [
            {"kind": "xmem_physical_pages", "api": "private-api", "attributes": "0x20",
             "pages": 4, "caller": "private-caller"},
        ] * 3)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(bt.main([self.startup, "--counts-only", "--limit", "1"]), 0)
        summary = json.loads(out.getvalue())["startup"]
        self.assertEqual(summary["xmem_physical_pages_count"], 3)
        self.assertNotIn("xmem_physical_pages", summary)
        self.assertNotIn("private-", out.getvalue())

    def test_xmem_physical_pages_handles_missing_and_malformed_fields(self):
        unknown = {"api": None, "attributes": None, "pages": None, "caller": None}
        nbsp_caller = "fixture\u00a0one.dll+0x20"
        text, records = self.physical_pages_reports([
            {},
            {"api": ["unexpected"], "attributes": "not hex", "pages": True, "caller": "\x1b[2J"},
            {"api": "XMemAllocatePhysicalPages", "attributes": "0x10", "pages": -1, "caller": "fixture.dll+0x50"},
            {"api": "XMemAllocatePhysicalPages", "attributes": "0x20", "pages": 4, "caller": nbsp_caller},
        ], 4)
        self.assertEqual(records, [
            unknown,
            unknown,
            {"api": "XMemAllocatePhysicalPages", "attributes": "0x10", "pages": None, "caller": "fixture.dll+0x50"},
            {"api": "XMemAllocatePhysicalPages", "attributes": "0x20", "pages": 4, "caller": nbsp_caller},
        ])
        self.assertEqual(text.count("    ? attributes ? requested ? pages from ?\n"), 2)
        self.assertIn("    XMemAllocatePhysicalPages attributes 0x10 requested ? pages from fixture.dll+0x50\n", text)
        self.assertIn(f"    XMemAllocatePhysicalPages attributes 0x20 requested 4 pages from {nbsp_caller}\n", text)

    def test_pipeline_timing_counts_overlapping_compiles_and_waits(self):
        self.write_lines(self.d3d12, [
            D3D12[1],
            "fixture.exe : [rt] pipeline-compile-begin trigger=draw tid=7 bytes=512 "
            "variant=0 begin_ms=100 key=synthetic-key shaders=private_vs,private_ps",
            "[rt] pipeline-compile-begin trigger=draw tid=8 bytes=512 "
            "variant=0 begin_ms=101 key=synthetic-key shaders=private_vs,private_ps",
            "[rt] pipeline-compile trigger=draw tid=8 start_us=1500 us=800 hit=1 bytes=512 "
            "variant=0 result=0 end_ms=102 key=synthetic-key shaders=private_vs,private_ps",
            "[rt] pipeline-compile trigger=dispatch tid=9 start_us=1600 us=5000 hit=-1 bytes=512 "
            "variant=0 result=-4 end_ms=106 key=other-key shaders=private_cs",
            "[rt] pipeline-use trigger=draw tid=10 us=6000 ready=0 end_ms=107",
            "[rt] pipeline-use trigger=draw tid=11 us=1000 ready=1 end_ms=107",
        ])
        trace = bt.parse_d3d12(self.d3d12)
        self.assertEqual(trace["ignored_lines"], 0)
        self.assertEqual(trace["pipeline_timing"], {
            "started": 2, "completed": 2, "completions_without_start": 1, "non_success_results": 1,
            "cache_hits": 1, "max_compile_us": 5000, "logged_waits": 2, "waits_not_ready": 1,
            "max_wait_us": 6000, "starts_without_completion": 1,
        })
        summary = bt.summarize(None, trace)
        text = bt.format_text(summary, 10)
        safe_json = json.dumps(bt.counts_only(summary))
        self.assertIn("1 without a recorded completion", text)
        self.assertIn("longest 6000 us", text)
        self.assertEqual(bt.counts_only(summary)["d3d12"]["pipeline_timing"], trace["pipeline_timing"])
        for raw in ("private_", "synthetic-key", "other-key"):
            self.assertNotIn(raw, text)
            self.assertNotIn(raw, safe_json)

    def test_frame_pacing_summary_parsed_formatted_and_kept_in_counts_only(self):
        self.write_lines(self.d3d12, [
            "[d3d12-trace] #1 XboxPresentation::PresentX ok",
            "gta6rt.exe : [rt] present: pacing summary frames=101 samples=100 target_us=16667 "
            "mean_us=16712.5 drift_us=+45.5 p50_us=16667 p95_us=17100 p99_us=18200 "
            "latency_samples=101 latency_p50_us=4200 latency_p95_us=6100 latency_p99_us=7900",
        ])
        trace = bt.parse_d3d12(self.d3d12)
        self.assertEqual(trace["ignored_lines"], 0)
        self.assertEqual(trace["frame_pacing"], {
            "frames": 101,
            "samples": 100,
            "target_us": 16667,
            "mean_us": 16712.5,
            "drift_us": 45.5,
            "p50_us": 16667,
            "p95_us": 17100,
            "p99_us": 18200,
            "latency_samples": 101,
            "latency_p50_us": 4200,
            "latency_p95_us": 6100,
            "latency_p99_us": 7900,
        })
        text = bt.format_text(bt.summarize(None, trace), 10)
        self.assertIn(
            "frame pacing: 101 frame(s), 100 interval(s), target 16667 us, mean 16712.5 us (drift +45.5 us), "
            "p50/p95/p99 16667/17100/18200 us; origin-to-present p50/p95/p99 4200/6100/7900 us",
            text,
        )
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(bt.main(["--d3d12", self.d3d12, "--counts-only"]), 0)
        counts = json.loads(out.getvalue())
        self.assertEqual(counts["d3d12"]["frame_pacing"]["drift_us"], 45.5)
        self.assertEqual(counts["d3d12"]["frame_pacing"]["p99_us"], 18200)


if __name__ == "__main__":
    unittest.main()
