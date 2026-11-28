"""Synthetic checks for tools/veh_spawn_intensities.py: the aggregate census of the vehicle
spawn-intensity table, band ranges, mud-band placement and rain grouping. Every entry name and
value in the fixtures is made up; the checker also asserts the CLI never prints key names, which
is game data.

    python -I tests/test_veh_spawn_intensities.py
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
TOOL = os.path.join(ROOT, "tools", "veh_spawn_intensities.py")
sys.path.insert(0, os.path.join(ROOT, "tools"))

import veh_spawn_intensities as vsi  # noqa: E402

# Entries shaped like the table: one 7-band ladder, one variant with mud in band 6, one 2-band.
A_SETS = [(-1.0, 0.0, 0.0, 0.0), (0.0, 0.1, 0.0, 0.0), (0.1, 0.2, 0.0, 0.0), (0.2, 0.4, 0.0, 0.0),
          (0.3, 0.6, 0.0, 0.0), (0.4, 0.8, 0.0, 0.0), (0.5, 1.0, 0.8, 1.0)]
B_SETS = A_SETS[:5] + [(0.45, 0.9, 0.9, 1.0), (0.5, 1.0, 0.8, 1.0)]
C_SETS = [(0.1, 0.3, 0.0, 0.0), (0.3, 0.9, 0.0, 0.0)]

DOC = """<?xml version="1.0" encoding="UTF-8"?>
<CGraphicsDefinitionsData>
  <VehSpawnIntensities>
    <Item key="spawn_alpha_test">
      <Name>spawn_alpha_test</Name>
      <IntensitySets>
        {a_sets}
      </IntensitySets>
      <DirtRainChance value="0.500000" /><DirtRainMin value="0.700000" /><DirtRainMax value="1.000000" />
    </Item>
    <Item key="spawn_beta_test">
      <Name>spawn_beta_test</Name>
      <IntensitySets>
        {b_sets}
      </IntensitySets>
      <DirtRainChance value="0.000000" /><DirtRainMin value="0.000000" /><DirtRainMax value="1.000000" />
    </Item>
    <Item key="spawn_gamma_test">
      <Name>spawn_gamma_test</Name>
      <IntensitySets>
        {c_sets}
      </IntensitySets>
      <DirtRainChance value="0.300000" /><DirtRainMin value="0.300000" /><DirtRainMax value="0.500000" />
    </Item>
  </VehSpawnIntensities>
</CGraphicsDefinitionsData>
""".format(
    a_sets="".join(f'<Item><DirtMin value="{a:g}" /><DirtMax value="{b:g}" />'
                   f'<MudMin value="{c:g}" /><MudMax value="{d:g}" /></Item>' for a, b, c, d in A_SETS),
    b_sets="".join(f'<Item><DirtMin value="{a:g}" /><DirtMax value="{b:g}" />'
                   f'<MudMin value="{c:g}" /><MudMax value="{d:g}" /></Item>' for a, b, c, d in B_SETS),
    c_sets="".join(f'<Item><DirtMin value="{a:g}" /><DirtMax value="{b:g}" />'
                   f'<MudMin value="{c:g}" /><MudMax value="{d:g}" /></Item>' for a, b, c, d in C_SETS),
)

WANT = {
    "bands_with_mud": [0, 0, 0, 0, 0, 1, 2],
    "entries": 3,
    "rain_group_sizes": [1, 1, 1],
    "rain_groups": 3,
    "set_counts": [2, 7],
    "signature_groups": [1, 1, 1],
    "signatures": 3,
    "per_set_ranges": [
        {"DirtMax": [0.0, 0.3], "DirtMin": [-1.0, 0.1], "MudMax": [0.0, 0.0], "MudMin": [0.0, 0.0]},
        {"DirtMax": [0.1, 0.9], "DirtMin": [0.0, 0.3], "MudMax": [0.0, 0.0], "MudMin": [0.0, 0.0]},
        {"DirtMax": [0.2, 0.2], "DirtMin": [0.1, 0.1], "MudMax": [0.0, 0.0], "MudMin": [0.0, 0.0]},
        {"DirtMax": [0.4, 0.4], "DirtMin": [0.2, 0.2], "MudMax": [0.0, 0.0], "MudMin": [0.0, 0.0]},
        {"DirtMax": [0.6, 0.6], "DirtMin": [0.3, 0.3], "MudMax": [0.0, 0.0], "MudMin": [0.0, 0.0]},
        {"DirtMax": [0.8, 0.9], "DirtMin": [0.4, 0.45], "MudMax": [0.0, 1.0], "MudMin": [0.0, 0.9]},
        {"DirtMax": [1.0, 1.0], "DirtMin": [0.5, 0.5], "MudMax": [1.0, 1.0], "MudMin": [0.8, 0.8]},
    ],
}


def write_doc(directory):
    path = os.path.join(directory, "graphics.xml")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(DOC)
    return path


class RecordsTest(unittest.TestCase):
    def test_records_and_aggregates(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(vsi.census(write_doc(tmp)), WANT)

    def test_empty_table_and_mud_min_above_zero(self):
        with tempfile.TemporaryDirectory() as tmp:
            empty_path = os.path.join(tmp, "empty.xml")
            with open(empty_path, "w", encoding="utf-8", newline="\n") as f:
                f.write("<CGraphicsDefinitionsData><VehSpawnIntensities/></CGraphicsDefinitionsData>\n")
            self.assertEqual(vsi.census(empty_path), {
                "entries": 0, "signatures": 0, "signature_groups": [], "set_counts": [],
                "per_set_ranges": [], "bands_with_mud": [], "rain_groups": 0, "rain_group_sizes": [],
            })
            mud_min_only = [{"key": "", "sets": ((0.0, 0.2, 0.5, 0.0),), "rain": (0.0, 0.0, 0.0)}]
            self.assertEqual(vsi.aggregate(mud_min_only)["bands_with_mud"], [1])


class CliTest(unittest.TestCase):
    def run_tool(self, *argv):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            status = vsi.main(list(argv))
        return status, out.getvalue(), err.getvalue()

    def test_other_document_with_valid_table_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "other.xml")
            report = os.path.join(tmp, "report.json")
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(DOC.replace("CGraphicsDefinitionsData", "UnrelatedRoot"))
            status, out, err = self.run_tool(path, "--json", report)
            self.assertEqual(status, 2)
            self.assertEqual(out, "")
            self.assertEqual(err, "ERROR: expected CGraphicsDefinitionsData document root\n")
            self.assertFalse(os.path.exists(report))

    def test_stdout_aggregates_and_no_key_names(self):
        with tempfile.TemporaryDirectory() as tmp:
            status, out, err = self.run_tool(write_doc(tmp))
            self.assertEqual(status, 0, err)
            self.assertEqual(json.loads(out), WANT)
            # Entry keys and Name values are the table's own text: the aggregate never carries them.
            for token in ("spawn_alpha_test", "spawn_beta_test", "spawn_gamma_test"):
                self.assertNotIn(token, out)
                self.assertNotIn(token, err)

    def test_json_output_matches_and_is_well_formed(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = os.path.join(tmp, "report.json")
            status, _, err = self.run_tool(write_doc(tmp), "--json", report)
            self.assertEqual(status, 0, err)
            with open(report, encoding="utf-8") as f:
                self.assertEqual(json.load(f), WANT)

    def test_json_over_the_input_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = write_doc(tmp)
            status, _, err = self.run_tool(path, "--json", path)
            self.assertEqual(status, 2)
            self.assertIn("--json must not overwrite the input", err)
            with open(path, encoding="utf-8") as f:
                self.assertIn("spawn_alpha_test", f.read())  # the input document is untouched

    def test_bad_input_is_one_error_line(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "broken.xml")
            with open(path, "w", encoding="utf-8") as f:
                f.write("<CGraphicsDefinitionsData><VehSpawnIntensities><Item/></VehSpawnIntensities>")
            status, _, err = self.run_tool(path)
            self.assertEqual(status, 2)
            self.assertTrue(err.startswith("ERROR:"))
            self.assertNotIn("Traceback", err)

    def test_a_non_finite_value_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = write_doc(tmp)
            with open(path, encoding="utf-8") as f:
                doc = f.read().replace('DirtMin value="-1"', 'DirtMin value="NaN"', 1)
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(doc)
            report = os.path.join(tmp, "report.json")
            status, out, err = self.run_tool(path, "--json", report)
            self.assertEqual(status, 2)
            self.assertTrue(err.startswith("ERROR:"), err)
            self.assertFalse(os.path.exists(report))
            with open(path, encoding="utf-8") as f:          # the input document is untouched
                self.assertIn('DirtMin value="NaN"', f.read())

    def test_carve_directory_input(self):
        with tempfile.TemporaryDirectory() as tmp:
            blob = DOC.encode("utf-8")
            first, second = blob[:256], blob[256:]
            rows = []
            for i, chunk in enumerate((first, second)):
                off = 0x1000 + i * len(first)
                with open(os.path.join(tmp, f"{off:012x}.bin"), "wb") as f:
                    f.write(chunk)
                rows.append({"off": off, "stored": len(chunk), "size": len(chunk),
                             "pre": "00" * 16, "head": chunk[:16].hex()})
            with open(os.path.join(tmp, "index.json"), "w", encoding="utf-8") as f:
                json.dump({"files": rows}, f)
            status, out, err = self.run_tool(tmp)
            self.assertEqual(status, 0, err)
            self.assertEqual(json.loads(out), WANT)

    def test_carve_without_the_document_is_an_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            empty = os.path.join(tmp, "0x1000.bin")
            with open(empty, "wb") as f:
                f.write(b"other plain bytes no xml")
            with open(os.path.join(tmp, "index.json"), "w", encoding="utf-8") as f:
                json.dump({"files": [{"off": 0x1000, "stored": 24, "size": 24,
                                      "pre": "00" * 16, "head": "other plain bytes ".encode().hex()[:32]}]}, f)
            status, _, err = self.run_tool(tmp)
            self.assertEqual(status, 2)
            self.assertIn("ERROR:", err)

    def test_carve_overwrite_and_empty_or_dir_json_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            carve = os.path.join(tmp, "carve")
            os.mkdir(carve)
            blob = DOC.encode("utf-8")
            stream = os.path.join(carve, "000000001000.bin")
            with open(stream, "wb") as f:
                f.write(blob)
            index_path = os.path.join(carve, "index.json")
            index_text = json.dumps({"files": [{"off": 0x1000, "stored": len(blob), "size": len(blob),
                                                "pre": "00" * 16, "head": blob[:16].hex()}]})
            with open(index_path, "w", encoding="utf-8", newline="\n") as f:
                f.write(index_text)

            cwd = os.path.join(tmp, "cwd")
            os.mkdir(cwd)
            old_cwd = os.getcwd()
            os.chdir(cwd)
            try:
                for bad in ("", "   ", carve, index_path, stream):
                    with self.subTest(bad=bad):
                        status, out, err = self.run_tool(carve, "--json", bad)
                        self.assertEqual(status, 2)
                        self.assertEqual(out, "")
                        self.assertIn("ERROR:", err)
                self.assertEqual(os.listdir(cwd), [])
            finally:
                os.chdir(old_cwd)

            with open(index_path, encoding="utf-8") as f:
                self.assertEqual(f.read(), index_text)
            with open(stream, "rb") as f:
                self.assertEqual(f.read(), blob)

            alias = os.path.join(tmp, "alias.json")
            try:
                os.link(stream, alias)
            except OSError:
                self.skipTest("hard links unavailable")
            status, out, err = self.run_tool(carve, "--json", alias)
            self.assertEqual(status, 2)
            self.assertEqual(out, "")
            self.assertIn("--json must not overwrite the input", err)
            with open(stream, "rb") as f:
                self.assertEqual(f.read(), blob)

    def test_atomic_json_write_preserves_existing_on_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            doc_path = write_doc(tmp)
            report = os.path.join(tmp, "report.json")
            with open(report, "w", encoding="utf-8", newline="\n") as f:
                f.write('{"keep": 1}\n')
            with mock.patch.object(vsi, "replace_file", side_effect=OSError("disk full")):
                status, out, err = self.run_tool(doc_path, "--json", report)
            self.assertEqual(status, 2)
            self.assertEqual(out, "")
            self.assertIn("disk full", err)
            with open(report, encoding="utf-8") as f:
                self.assertEqual(f.read(), '{"keep": 1}\n')
            self.assertEqual([n for n in os.listdir(tmp) if n.startswith(".veh-spawn-")], [])


if __name__ == "__main__":
    unittest.main()
