"""Unit tests for tools/pso_code_base_pairs.py (how many real PSOs pair two code-base stages).

    python -I tests/test_pso_code_base_pairs.py

Zero game data: the pipeline library, the carved containers and the translation index are all built here from
synthetic bytes, using the builders in tests/test_shader_library.py.
"""
import contextlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "tests"))

import pso_code_base_pairs as pairs  # noqa: E402
import shader_library as sl  # noqa: E402
import test_shader_library as synth  # noqa: E402

TOOL = Path(ROOT) / "tools" / "pso_code_base_pairs.py"


def library():
    """Three graphics PSOs and one compute record: VS+PS with both stages on a code base, MS+PS with only the
    pixel stage on one, and an MS-only record whose program cannot be resolved."""
    blobs = [synth.layout_blob(2, synth.D_PS), synth.layout_blob(2, synth.D_VS),
             synth.layout_blob(2, synth.D_MS), synth.layout_blob(3, synth.D_CS), b"\x05" * 8]
    programs = [synth.prog(tag) for tag in (0x11112222, 0x33334444, 0x55556666, 0x77778888)]
    graphics = [(4, 4, {0: (0, 0), 1: (1, 1)}),        # pixel and vertex, both on a code base
                (4, 4, {0: (0, 0), 2: (2, 2)}),        # mesh without, pixel with
                (4, 4, {2: (3, 3)})]                   # the mesh program of the compute record too
    return synth.build(graphics, [(4, 4, 3, 3)], blobs, programs)


def corpus(root: str, spirv_shape: str = "map"):
    """<work>/shaders: a carve folder with one container per digest and a translation index over those containers.
    The mesh container says no code base; the compute container is missing from both sides."""
    shaders = Path(root) / "shaders"
    dxbc = shaders / "dxbc"
    dxbc.mkdir(parents=True)
    entries = []
    for stem, digest, code_base in (("PS_ONE", synth.D_PS, True), ("VS_ONE", synth.D_VS, True),
                                    ("MS_ONE", synth.D_MS, False)):
        (dxbc / f"{stem}.dxbc").write_bytes(synth.dxbc(digest))
        entries.append({"file": f"{stem}.dxbc", "name": f"synthetic {stem.lower()}"})
    (dxbc / "index.json").write_text(json.dumps(entries), encoding="utf-8")
    spirv = shaders / "spirv"
    spirv.mkdir()
    rows = {"PS_ONE": {"uses_code_base": True}, "VS_ONE": {"uses_code_base": True},
            "MS_ONE": {"uses_code_base": False}}
    index = rows if spirv_shape == "map" else [dict(row, file=f"{stem}.dxbc") for stem, row in rows.items()]
    (spirv / "index.json").write_text(json.dumps(index), encoding="utf-8")
    return str(shaders)


class Join(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.shaders = corpus(self._tmp.name)
        self.lib = sl.parse(library())
        self.code_base = pairs.read_spirv(os.path.join(self.shaders, "spirv", "index.json"))
        self.digests = shader_carve_digests(os.path.join(self.shaders, "dxbc"))

    def report(self):
        return pairs.count_records(self.lib, pairs.code_base_flags(self.lib, self.digests, self.code_base))

    def test_two_code_base_stages_are_counted_by_stage_set(self):
        r = self.report()
        self.assertEqual(r["stage_slots"], {"ps+vs": 1, "ms+ps": 1, "ms": 1})
        self.assertEqual(r["psos_by_code_base_stages"], {"2": 1, "1": 1, "0": 1})
        self.assertEqual(r["psos_with_two_code_base_stages"], {"ps+vs": 1})

    def test_unresolved_stages_are_reported_separately(self):
        # Program 3's layout digest is D_CS and no carved container carries it, so neither of the two records
        # that name it can say whether that stage uses a code base.
        r = self.report()
        self.assertEqual(r["psos_unresolved"], {"ms": 1})
        self.assertEqual(r["compute_records_by_code_base"], {"None": 1})
        self.assertEqual(r["psos_by_code_base_stages"], {"2": 1, "1": 1, "0": 1})

    def test_per_stage_program_counts_match_feature_scan(self):
        r = self.report()
        # Program 3 is the mesh stage of the last record and the compute record's program, so it is counted once,
        # under the pair of stages it serves.
        self.assertEqual(r["programs"], {"ps": 1, "vs": 1, "ms": 1, "cs+ms": 1})
        self.assertEqual(r["programs_with_code_base"], {"ps": 1, "vs": 1})
        self.assertEqual(r["psos_with_code_base_by_stage"], {"ps": 2, "vs": 1})

    def test_a_container_carved_twice_counts_as_a_code_base_once(self):
        dxbc = Path(self.shaders) / "dxbc"
        (dxbc / "PS_TWICE.dxbc").write_bytes(synth.dxbc(synth.D_PS))
        entries = json.loads((dxbc / "index.json").read_text(encoding="utf-8"))
        entries.append({"file": "PS_TWICE.dxbc", "name": "synthetic ps again"})
        (dxbc / "index.json").write_text(json.dumps(entries), encoding="utf-8")
        index_path = Path(self.shaders) / "spirv" / "index.json"
        rows = json.loads(index_path.read_text(encoding="utf-8"))
        rows["PS_TWICE"] = {"uses_code_base": False}          # the other copy says no
        index_path.write_text(json.dumps(rows), encoding="utf-8")
        self.digests = shader_carve_digests(str(dxbc))
        self.code_base = pairs.read_spirv(str(index_path))
        self.assertEqual(self.report()["psos_with_two_code_base_stages"], {"ps+vs": 1})

    def test_list_shaped_translation_index_joins_by_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            shaders = corpus(tmp, spirv_shape="list")
            code_base = pairs.read_spirv(os.path.join(shaders, "spirv", "index.json"))
            self.assertEqual(code_base, {"PS_ONE": True, "VS_ONE": True, "MS_ONE": False})
            flags = pairs.code_base_flags(self.lib, self.digests, code_base)
            self.assertEqual(pairs.count_records(self.lib, flags)["psos_with_two_code_base_stages"], {"ps+vs": 1})


def shader_carve_digests(dxbc: str):
    """digest -> containers, over a carve folder, with the helper's own error lines captured."""
    with contextlib.redirect_stderr(io.StringIO()):
        return sl.carve_digests(dxbc, errors=[])


class CommandLine(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = self._tmp.name
        self.shaders = corpus(self.root)

    def run_tool(self, library_path, *extra):
        out = os.path.join(self.root, "pairs.json")
        proc = subprocess.run([sys.executable, "-I", str(TOOL), self.shaders, "--library", library_path,
                               *extra, "--out", out], capture_output=True, text=True)
        return proc, (json.loads(Path(out).read_text(encoding="utf-8")) if os.path.exists(out) else None)

    def test_library_blob_and_archive_are_both_accepted(self):
        lib = library()
        raw = os.path.join(self.root, "library.bin")
        Path(raw).write_bytes(lib)
        packed = os.path.join(self.root, "shaders.rpf")
        Path(packed).write_bytes(synth.archive(lib))
        for path in (raw, packed):
            with self.subTest(path=os.path.basename(path)):
                proc, report = self.run_tool(path)
                self.assertEqual(proc.returncode, 0, proc.stderr)
                self.assertEqual(report["psos_with_two_code_base_stages"], {"ps+vs": 1})
                self.assertIn("stage slots per graphics PSO", proc.stdout)

    def test_report_names_no_shader(self):
        raw = os.path.join(self.root, "library.bin")
        Path(raw).write_bytes(library())
        proc, report = self.run_tool(raw)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        written = proc.stdout + json.dumps(report)
        for name in ("PS_ONE", "VS_ONE", "MS_ONE", "ps one"):
            self.assertNotIn(name, written)

    def test_out_naming_an_input_is_refused(self):
        raw = os.path.join(self.root, "library.bin")
        Path(raw).write_bytes(library())
        input_path = os.path.join(self.shaders, "spirv", "index.json")
        proc = subprocess.run([sys.executable, "-I", str(TOOL), self.shaders, "--library", raw,
                               "--out", input_path], capture_output=True, text=True)
        self.assertEqual(proc.returncode, 2)
        self.assertIn("is an input", proc.stderr)

    def test_missing_work_folder(self):
        raw = os.path.join(self.root, "library.bin")
        Path(raw).write_bytes(library())
        proc = subprocess.run([sys.executable, "-I", str(TOOL), os.path.join(self.root, "nope"),
                               "--library", raw], capture_output=True, text=True)
        self.assertEqual(proc.returncode, 2)
        self.assertIn("does not exist", proc.stderr)


if __name__ == "__main__":
    unittest.main()
