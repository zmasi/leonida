"""Counts-only radio label checks with hand-built AWC banks and text tables."""

from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_awc_markers import build_bank, event  # noqa: E402
from test_text_tables import T, make_resource  # noqa: E402

TOOL = Path(__file__).resolve().parents[1] / "tools" / "track_labels.py"
TRACK_ID = 0xA6D93246
STREAM_ID = 0x10E76433  # hashdict.joaat("fixture_stream_7"), unchanged by the 29-bit mask
SENTINEL = "Synthetic title that must stay private"
DATA_ERROR = b"track_labels: could not read valid AWC banks and one string table"
ARGS_ERROR = b"track_labels: invalid arguments (see --help)"


class TrackLabelsTest(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.root = Path(scratch.name)
        self.table = self.root / "private-table-name.bin"

    def bank(self, filename, numbers):
        markers = b"".join(event(TRACK_ID, float(n), i) for i, n in enumerate(numbers))
        markers += event(0x11111111, 91.0, 9) + event(TRACK_ID, 92.0, 10, flags=1)
        path = self.root / filename
        path.write_bytes(build_bank([(STREAM_ID, [(0xBD, markers)])]))
        return path

    def run_tool(self, *args):
        return subprocess.run([sys.executable, "-IB", TOOL, *args], capture_output=True)

    def test_counts_rows_and_distinct_numbers_with_one_table(self):
        first = self.bank("first.awc", [3, 3])
        second = self.bank("second.awc", [3, 12, 0])
        self.table.write_bytes(make_resource({"3s": SENTINEL, "3a": "Artist", "12s": "", "12a": ""}))
        # Independently computed with hashdict.joaat, also used by the resource builder.
        self.assertIn(bytes.fromhex("ead5bb87"), self.table.read_bytes())
        result = self.run_tool(first, second, "--table", self.table)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, b"")
        self.assertEqual(result.stdout, (
            b"marker_rows 5\ndistinct_numbers 3\n"
            b"title_present 1\ntitle_empty 1\ntitle_absent 1\n"
            b"artist_present 1\nartist_empty 1\nartist_absent 1\n"
        ))

    def test_does_not_use_the_stream_id_or_stream_name_hash(self):
        bank = self.bank("mix.awc", [3])
        self.table.write_bytes(make_resource({"fixture_stream_7": SENTINEL}))
        result = self.run_tool(bank, "--table", self.table)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, b"")
        self.assertEqual(result.stdout, (
            b"marker_rows 1\ndistinct_numbers 1\n"
            b"title_present 0\ntitle_empty 0\ntitle_absent 1\n"
            b"artist_present 0\nartist_empty 0\nartist_absent 1\n"
        ))

    def test_bad_inputs_emit_only_fixed_errors(self):
        bank = self.bank("private-bank-name.awc", [3])
        resource = make_resource({"3s": SENTINEL})
        self.table.write_bytes(resource)
        ambiguous = self.root / "two-tables.bin"
        # Two descriptors can point at the same hand-built table data.
        two_tables = resource + T.DESCRIPTOR.search(resource).group()
        self.assertEqual(len(list(T.string_tables(two_tables))), 2)
        ambiguous.write_bytes(two_tables)
        bad = self.root / SENTINEL
        bad.write_bytes(SENTINEL.encode())
        missing = self.root / "private-missing.awc"
        partial = self.root / "partial.awc"
        partial.write_bytes(build_bank([(STREAM_ID, [(0xBD, event(TRACK_ID, 3.0, 0) + b"\0")])]))
        fractional = self.bank("fractional.awc", [3.5])
        negative = self.bank("negative.awc", [-1])
        cases = [
            (1, DATA_ERROR, (path, "--table", self.table))
            for path in (bad, missing, partial, fractional, negative)
        ] + [
            (1, DATA_ERROR, (bank, "--table", path)) for path in (bad, missing, ambiguous)
        ] + [
            (2, ARGS_ERROR, (bank, "--table", self.table, *extra))
            for extra in (("--table", self.table), ("--" + SENTINEL,))
        ]
        for code, message, args in cases:
            with self.subTest(args=args):
                result = self.run_tool(*args)
                self.assertEqual(result.returncode, code)
                self.assertEqual(result.stdout, b"")
                self.assertEqual(result.stderr.splitlines(), [message])


if __name__ == "__main__":
    unittest.main()
