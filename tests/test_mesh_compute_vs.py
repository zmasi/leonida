"""Unit tests for tools/mesh_compute_vs.py (standard library; the assembly checks need SPIRV-Tools on PATH)."""
import contextlib
import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import mesh_compute_vs as vs  # noqa: E402
import rdna2spirv  # noqa: E402

HAVE_SPIRV_TOOLS = bool(shutil.which("spirv-as") and shutil.which("spirv-val"))


def layout(maxv=64, maxp=3, clip=0, cull=0, params=()):
    """A real `mesh_compute` index entry, from the translator's own layout function."""
    return rdna2spirv.mesh_compute_layout(maxv, maxp, clip, cull, list(params))


def write_index(directory, index):
    path = os.path.join(directory, "index.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(index, f)
    return path

def write_producer_and_seal(directory, name, layout_dict, producer_bytes=None):
    """Translator partial seal for tests: dummy producer .spv + v1 seal (layout+producer, no companion)."""
    if producer_bytes is None:
        producer_bytes = ("producer-bytes-for-" + name).encode("utf-8")
    producer_path = os.path.join(directory, name + ".spv")
    with open(producer_path, "wb") as f:
        f.write(producer_bytes)
    canonical = vs.canonical_layout_json(layout_dict)
    seal = {"version": vs.MESH_COMPUTE_SEAL_VERSION,
            "layout": json.loads(canonical),
            "layout_sha256": hashlib.sha256(canonical.encode("utf-8")).hexdigest(),
            "producer_sha256": hashlib.sha256(producer_bytes).hexdigest(),
            "companion_asm_sha256": None, "companion_spv_sha256": None}
    seal_path = os.path.join(directory, name + vs.SEAL_SUFFIX)
    with open(seal_path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(seal, f, sort_keys=True, separators=(",", ":"))
        f.write("\n")
    return producer_path, seal_path


class Interface(unittest.TestCase):
    def test_entry_point_carries_the_mesh_shader_locations(self):
        asm = vs.vs_spvasm(layout(clip=5, cull=2, params=(2, 9)))
        entry = asm.split("OpEntryPoint Vertex", 1)[1].splitlines()[0]
        for name in ("%vertex_index", "%pc", "%out_pos", "%out_clip", "%out_cull", "%out2", "%out9"):
            self.assertIn(name, entry)
        for decoration in ("BuiltIn ClipDistance", "BuiltIn CullDistance", "Location 2", "Location 9"):
            self.assertIn(decoration, asm)

    def test_no_clip_or_cull_means_no_such_outputs(self):
        asm = vs.vs_spvasm(layout(params=(3,)))
        self.assertNotIn("ClipDistance", asm)
        self.assertNotIn("CullDistance", asm)
        self.assertNotIn("%out_clip", asm)
        self.assertIn("Location 3", asm)

    def test_pair_f16_halves_for_high_param_producer(self):
        """SHADER-378: a 16-param mesh-compute producer paired with a PS reading f16 on params (0, 4, 5, 6, 7) emits
        only those 5 decoded-half pairs within locations 0..29, with zero overlap."""
        lay = layout(params=tuple(range(16)))
        ps_halves = {"0": [28, 29], "4": [26, 27], "5": [24, 25], "6": [22, 23], "7": [20, 21]}
        paired = vs.pair_f16_halves(lay, ps_halves, ps_raw_inputs=range(15))
        self.assertEqual(paired, ps_halves)
        asm = vs.vs_spvasm(lay, f16_halves=paired)
        for loc in range(16):
            self.assertIn(f"OpDecorate %out{loc} Location {loc}", asm)
        for lo, hi in paired.values():
            self.assertIn(f"OpDecorate %out{lo} Location {lo}", asm)
            self.assertIn(f"OpDecorate %out{hi} Location {hi}", asm)
        self.assertNotIn("Location 30", asm)

        # Collision with producer param15 relocates both halves into free slots <= 29.
        reloc = vs.pair_f16_halves(lay, {"7": [15, 29]}, ps_raw_inputs=range(15))
        self.assertEqual(reloc, {"7": [28, 29]})
        asm_reloc = vs.vs_spvasm(lay, f16_halves=reloc)
        self.assertIn("OpDecorate %out28 Location 28", asm_reloc)
        self.assertIn("OpDecorate %out29 Location 29", asm_reloc)

        # Missing producer param and colliding explicit override are rejected cleanly.
        with self.assertRaises(ValueError):
            vs.pair_f16_halves(layout(params=(0, 1)), {"7": [28, 29]})
        with self.assertRaises(ValueError):
            vs.vs_spvasm(layout(params=(0, 1)), f16_halves={"0": [1, 29]})

    def test_the_index_drives_write_for_index(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(vs.write_for_index(tmp), [])       # no index.json: nothing to do
            write_index(tmp, {"ms_none": {"subgroup_size": 64}, "ms_null": None,
                              "ms_a": {"mesh_compute": layout(clip=1)}})
            written = vs.write_for_index(tmp)
            self.assertEqual([os.path.basename(p) for p in written], ["ms_a.mesh_vs.spvasm"])
            with open(written[0], encoding="utf-8") as f:
                self.assertEqual(f.read(), vs.vs_spvasm(layout(clip=1)))


class Assembly(unittest.TestCase):
    def test_every_layout_assembles_and_validates(self):
        if not HAVE_SPIRV_TOOLS:
            self.skipTest("spirv-as / spirv-val not on PATH")
        layouts = {
            "ms_plain": layout(),
            "ms_clip": layout(clip=1),
            "ms_full": layout(clip=5, cull=2, params=(2, 5)),
            "ms_small": layout(maxv=1, maxp=256),
            "ms_large": layout(maxv=128, maxp=64, clip=4, params=(0,)),
        }
        with tempfile.TemporaryDirectory() as tmp:
            write_index(tmp, {name: {"mesh_compute": lay} for name, lay in layouts.items()})
            for name, lay in layouts.items():
                write_producer_and_seal(tmp, name, lay)
            built = vs.build_for_index(tmp)
            self.assertEqual([os.path.basename(p) for p in built],
                             [name + ".mesh_vs.spv" for name in sorted(layouts)])
            for spv in built:
                self.assertGreater(os.path.getsize(spv), 0)
                self.assertTrue(os.path.isfile(spv[: -len(".spv")] + ".spvasm"))
                with open(spv[: -len(".mesh_vs.spv")] + vs.SEAL_SUFFIX, encoding="utf-8") as f:
                    seal = json.load(f)
                    self.assertEqual(seal["version"], vs.MESH_COMPUTE_SEAL_VERSION)
                    self.assertTrue(seal["companion_spv_sha256"])

    def test_only_validated_binary_reaches_the_final_path(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            lay = layout()
            write_index(tmp, {"ms_a": {"mesh_compute": lay}})
            write_producer_and_seal(tmp, "ms_a", lay)
            final = os.path.join(tmp, "ms_a.mesh_vs.spv")
            seal_path = os.path.join(tmp, "ms_a" + vs.SEAL_SUFFIX)
            with open(final, "wb") as f:
                f.write(b"previous shader")

            def assemble_and_validate(cmd: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
                if cmd[0] == "spirv-as":
                    with open(cmd[-1], "wb") as f:
                        f.write(b"new shader")
                    return subprocess.CompletedProcess(cmd, 0, "", "")
                self.assertEqual(cmd[0], "spirv-val")
                with open(cmd[-1], "rb") as f:
                    self.assertEqual(f.read(), b"new shader")
                return subprocess.CompletedProcess(cmd, validation_code, "", "validator output")

            with (mock.patch.object(vs, "tool_path", side_effect=lambda name: name),
                  mock.patch.object(vs.subprocess, "run", side_effect=assemble_and_validate)):
                validation_code = 1
                with self.assertRaisesRegex(RuntimeError, "spirv-val failed"):
                    vs.build_for_index(tmp)
                with open(final, "rb") as f:
                    self.assertEqual(f.read(), b"previous shader")
                with open(seal_path, encoding="utf-8") as f:
                    seal = json.load(f)  # failed build leaves partial seal untouched (no companion hashes)
                    self.assertIsNone(seal["companion_asm_sha256"])
                    self.assertIsNone(seal["companion_spv_sha256"])

                validation_code = 0
                self.assertEqual(vs.build_for_index(tmp), [final])
                with open(final, "rb") as f:
                    self.assertEqual(f.read(), b"new shader")
                with open(seal_path, encoding="utf-8") as f:
                    seal = json.load(f)
                    with open(os.path.join(tmp, "ms_a.mesh_vs.spvasm"), "rb") as af:
                        self.assertEqual(seal["companion_asm_sha256"], hashlib.sha256(af.read()).hexdigest())
                    self.assertEqual(seal["companion_spv_sha256"], hashlib.sha256(b"new shader").hexdigest())

            self.assertCountEqual(os.listdir(tmp), ["index.json", "ms_a.mesh_compute.seal.json", "ms_a.spv",
                                                    "ms_a.mesh_vs.spv", "ms_a.mesh_vs.spvasm"])

    def test_a_rejected_shader_names_the_tool(self):
        with tempfile.TemporaryDirectory() as tmp:
            lay = layout()
            write_index(tmp, {"ms_a": {"mesh_compute": lay}})
            write_producer_and_seal(tmp, "ms_a", lay)

            def fail_as(cmd, **kw):
                if cmd[0] == "spirv-as":
                    return subprocess.CompletedProcess(cmd, 1, "", "error: invalid assembly")
                return subprocess.CompletedProcess(cmd, 0, "", "")

            with mock.patch.object(vs, "tool_path", side_effect=lambda name: name), \
                    mock.patch.object(vs.subprocess, "run", side_effect=fail_as):
                with self.assertRaises(RuntimeError) as ctx:
                    vs.build_for_index(tmp)
        self.assertIn("spirv-as failed for", str(ctx.exception))


class Cli(unittest.TestCase):
    def run_main(self, argv, **env):
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.dict(os.environ, env):
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                rc = vs.main(argv)
        return rc, out.getvalue(), err.getvalue()

    def test_a_missing_spirv_tool_is_one_error_line(self):
        with tempfile.TemporaryDirectory() as tmp:
            lay = layout()
            write_index(tmp, {"ms_a": {"mesh_compute": lay}})
            write_producer_and_seal(tmp, "ms_a", lay)
            rc, stdout, stderr = self.run_main(["mesh_compute_vs.py", tmp], PATH="")
        self.assertEqual(rc, 1)
        self.assertEqual(stdout, "")
        self.assertEqual(len(stderr.splitlines()), 1)
        self.assertIn("spirv-as not found on PATH", stderr)

    def test_invalid_index_leaves_existing_assembly_untouched(self):
        with tempfile.TemporaryDirectory() as tmp:
            index = os.path.join(tmp, "index.json")
            asm = os.path.join(tmp, "ms_a.mesh_vs.spvasm")
            for payload, message in ((b"{", "invalid JSON"), (b"[]", "expected a shader index object"),
                                     (b'{"ms_a": {"mesh_compute": {"params": {}}}}', "layout for 'ms_a'")):
                with self.subTest(payload=payload):
                    with open(asm, "wb") as f:
                        f.write(b"previous assembly")
                    with open(index, "wb") as f:
                        f.write(payload)
                    with mock.patch.object(vs, "tool_path", side_effect=lambda name: name):
                        rc, stdout, stderr = self.run_main(["mesh_compute_vs.py", tmp])
                    self.assertEqual((rc, stdout), (1, ""))
                    self.assertIn(index, stderr)
                    self.assertIn(message, stderr)
                    with open(asm, "rb") as f:
                        self.assertEqual(f.read(), b"previous assembly")

    def test_a_wrong_argument_count_is_a_usage_error(self):
        rc, stdout, stderr = self.run_main(["mesh_compute_vs.py"])
        self.assertEqual(rc, 2)
        self.assertEqual(stdout, "")
        self.assertIn("Vertex shaders that draw", stderr)

    def test_help_prints_usage(self):
        for flag in ("-h", "--help"):
            with self.subTest(flag=flag):
                rc, stdout, stderr = self.run_main(["mesh_compute_vs.py", flag])
                self.assertEqual(rc, 0)
                self.assertIn("mesh_compute_vs.py <spirv dir>", stdout)
                self.assertEqual(stderr, "")

    def test_a_missing_directory_is_one_error_line(self):
        with tempfile.TemporaryDirectory() as tmp:
            missing = os.path.join(tmp, "missing")
            with mock.patch.object(vs, "build_for_index") as build:
                rc, stdout, stderr = self.run_main(["mesh_compute_vs.py", missing])
            build.assert_not_called()
            self.assertEqual(os.listdir(tmp), [])
        self.assertEqual(rc, 1)
        self.assertEqual(stdout, "")
        self.assertEqual(len(stderr.splitlines()), 1)
        self.assertIn("not a directory", stderr)

    def test_an_unreadable_index_is_one_error_line(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = write_index(tmp, {})
            os.mkdir(os.path.join(tmp, "dir"))
            # write_for_index() runs first (tool_path only when a build is needed), so stub tool_path out too:
            # otherwise a machine without SPIRV-Tools reports the missing tool instead of the write error here.
            with mock.patch.object(vs, "tool_path", return_value="spirv-as"), \
                    mock.patch.object(vs, "write_for_index",
                                      side_effect=OSError(13, "Permission denied", path)):
                rc, _, stderr = self.run_main(["mesh_compute_vs.py", tmp])
        self.assertEqual(rc, 1)
        self.assertEqual(len(stderr.splitlines()), 1)
        self.assertIn("Permission denied", stderr)


class Production(unittest.TestCase):
    """Production pack behavior: 128B refusal, unchanged caching, and main+auxiliary structure."""

    def pc_tail_layout(self, **kw):
        lay = layout(**kw)
        lay.update(push_constant_bytes=128, address_dwords=None, pc_tail_address_dwords=[4, 5])
        return lay

    def test_128b_pc_tail_has_no_companion(self):
        lay = self.pc_tail_layout(params=(0,))
        self.assertTrue(vs.is_pc_tail_layout(lay))
        self.assertFalse(vs.is_pc_tail_layout(layout(params=(0,))))
        with self.assertRaisesRegex(ValueError, "128-byte.*pc-tail"):
            vs.check_push_contract(lay)
        with self.assertRaisesRegex(ValueError, "128-byte.*pc-tail"):
            vs.vs_spvasm(lay)
        with tempfile.TemporaryDirectory() as tmp:
            index_path = write_index(tmp, {"ms_a": {"stage": "ms", "mesh_compute": lay}})
            with self.assertRaises(RuntimeError) as ctx:
                vs.write_for_index(tmp)
            message = str(ctx.exception)
            for needle in (index_path, "ms_a", "128-byte", "pc-tail"):
                self.assertIn(needle, message)
            self.assertEqual(os.listdir(tmp), ["index.json"])  # no companion artifact written
            with mock.patch.object(vs, "tool_path", side_effect=lambda name: name):
                with self.assertRaises(RuntimeError) as bctx:
                    vs.build_for_index(tmp)
            for needle in ("ms_a", "128-byte"):
                self.assertIn(needle, str(bctx.exception))

    def test_unsupported_push_is_not_silent(self):
        lay = layout()
        lay["push_constant_bytes"] = 144
        with self.assertRaisesRegex(ValueError, "unsupported push contract"):
            vs.vs_spvasm(lay)

    def fake_tools(self, test):
        def run(cmd, **kw):
            test.calls.append(cmd[0])
            if cmd[0] == "spirv-as":
                with open(cmd[-1], "wb") as f:
                    f.write(b"spv-bytes")
                return subprocess.CompletedProcess(cmd, 0, "", "")
            test.assertEqual(cmd[0], "spirv-val")
            return subprocess.CompletedProcess(cmd, 0, "", "")
        return run

    def test_unchanged_companions_cost_no_subprocess(self):
        la, lb = layout(clip=1), layout(params=(0, 1))
        with tempfile.TemporaryDirectory() as tmp:
            write_index(tmp, {"ms_a": {"mesh_compute": la}, "ms_b": {"mesh_compute": lb}})
            write_producer_and_seal(tmp, "ms_a", la)
            write_producer_and_seal(tmp, "ms_b", lb)
            self.calls = []
            with mock.patch.object(vs, "tool_path", side_effect=lambda name: name), \
                    mock.patch.object(vs.subprocess, "run", side_effect=self.fake_tools(self)):
                first = vs.build_for_index(tmp)
                self.assertEqual(len(self.calls), 4)  # 2 distinct layouts x (as + val)
                self.assertEqual([os.path.basename(p) for p in first],
                                 ["ms_a.mesh_vs.spv", "ms_b.mesh_vs.spv"])
                for spv in first:
                    name = os.path.basename(spv)[: -len(".mesh_vs.spv")]
                    with open(os.path.join(tmp, name + vs.SEAL_SUFFIX), encoding="utf-8") as f:
                        seal = json.load(f)
                        self.assertEqual(seal["version"], vs.MESH_COMPUTE_SEAL_VERSION)
                        self.assertTrue(seal["companion_spv_sha256"])
                self.calls.clear()
                with mock.patch.object(vs, "tool_path",
                                       side_effect=AssertionError("must not resolve tools when unchanged")):
                    second = vs.build_for_index(tmp)
                self.assertEqual(second, first)
                self.assertEqual(self.calls, [])
                self.assertEqual(sorted(os.listdir(tmp)),
                                 ["index.json", "ms_a.mesh_compute.seal.json", "ms_a.mesh_vs.spv",
                                  "ms_a.mesh_vs.spvasm", "ms_a.spv", "ms_b.mesh_compute.seal.json",
                                  "ms_b.mesh_vs.spv", "ms_b.mesh_vs.spvasm", "ms_b.spv"])

    def test_changed_layout_and_missing_spv_rebuild_only_those(self):
        with tempfile.TemporaryDirectory() as tmp:
            la = layout()
            write_index(tmp, {"ms_a": {"mesh_compute": la}, "ms_b": {"mesh_compute": la}})
            write_producer_and_seal(tmp, "ms_a", la)
            write_producer_and_seal(tmp, "ms_b", la)
            self.calls = []
            with mock.patch.object(vs, "tool_path", side_effect=lambda name: name), \
                    mock.patch.object(vs.subprocess, "run", side_effect=self.fake_tools(self)):
                vs.build_for_index(tmp)
                self.assertEqual(len(self.calls), 2)  # identical layouts dedup to one compile
                with open(os.path.join(tmp, "ms_a.mesh_vs.spv"), "rb") as fa, \
                        open(os.path.join(tmp, "ms_b.mesh_vs.spv"), "rb") as fb:
                    self.assertEqual(fa.read(), fb.read())
                lb = layout(clip=1)
                write_index(tmp, {"ms_a": {"mesh_compute": la}, "ms_b": {"mesh_compute": lb}})
                write_producer_and_seal(tmp, "ms_b", lb)  # legitimate retranslation updates producer+seal
                self.calls.clear()
                vs.build_for_index(tmp)
                self.assertEqual(len(self.calls), 2)
                os.unlink(os.path.join(tmp, "ms_a.mesh_vs.spv"))
                self.calls.clear()
                vs.build_for_index(tmp)
                self.assertEqual(len(self.calls), 2)

    def test_cold_duplicates_compile_once_per_distinct_layout(self):
        index = {f"ms_{i}": {"mesh_compute": layout(clip=(i % 2))} for i in range(6)}
        with tempfile.TemporaryDirectory() as tmp:
            write_index(tmp, index)
            for name, meta in index.items():
                write_producer_and_seal(tmp, name, meta["mesh_compute"])
            self.calls = []

            def content_aware(cmd, **kw):
                self.calls.append(cmd[0])
                if cmd[0] == "spirv-as":
                    with open(cmd[3], encoding="utf-8") as f:
                        tag = hashlib.sha256(f.read().encode("utf-8")).hexdigest()[:16]
                    with open(cmd[-1], "wb") as f:
                        f.write(("spv:" + tag).encode("utf-8"))
                    return subprocess.CompletedProcess(cmd, 0, "", "")
                return subprocess.CompletedProcess(cmd, 0, "", "")

            with mock.patch.object(vs, "tool_path", side_effect=lambda name: name), \
                    mock.patch.object(vs.subprocess, "run", side_effect=content_aware):
                built = vs.build_for_index(tmp)
            self.assertEqual(len(self.calls), 4)
            self.assertEqual(len(built), 6)
            blobs = {}
            for spv in built:
                with open(spv, "rb") as f:
                    blobs.setdefault(f.read(), []).append(spv)
            self.assertEqual(sorted(len(v) for v in blobs.values()), [3, 3])

    def test_failed_rebuild_does_not_validate_stale_binary(self):
        with tempfile.TemporaryDirectory() as tmp:
            la = layout()
            write_index(tmp, {"ms_a": {"mesh_compute": la}})
            write_producer_and_seal(tmp, "ms_a", la)
            self.calls = []
            with mock.patch.object(vs, "tool_path", side_effect=lambda name: name), \
                    mock.patch.object(vs.subprocess, "run", side_effect=self.fake_tools(self)):
                vs.build_for_index(tmp)
            with open(os.path.join(tmp, "ms_a.mesh_vs.spv"), "rb") as f:
                good = f.read()
            lb = layout(clip=1)
            write_index(tmp, {"ms_a": {"mesh_compute": lb}})
            write_producer_and_seal(tmp, "ms_a", lb)

            def fail_val(cmd, **kw):
                self.calls.append(cmd[0])
                if cmd[0] == "spirv-as":
                    with open(cmd[-1], "wb") as f:
                        f.write(b"candidate")
                    return subprocess.CompletedProcess(cmd, 0, "", "")
                return subprocess.CompletedProcess(cmd, 1, "", "validator output")

            with mock.patch.object(vs, "tool_path", side_effect=lambda name: name), \
                    mock.patch.object(vs.subprocess, "run", side_effect=fail_val):
                with self.assertRaisesRegex(RuntimeError, "spirv-val failed"):
                    vs.build_for_index(tmp)
            with open(os.path.join(tmp, "ms_a.mesh_vs.spv"), "rb") as f:
                self.assertEqual(f.read(), good)
            with open(os.path.join(tmp, "ms_a" + vs.SEAL_SUFFIX), encoding="utf-8") as f:
                self.assertIsNone(json.load(f)["companion_spv_sha256"])
            self.calls.clear()
            with mock.patch.object(vs, "tool_path", side_effect=lambda name: name), \
                    mock.patch.object(vs.subprocess, "run", side_effect=self.fake_tools(self)):
                vs.build_for_index(tmp)
                self.assertEqual(len(self.calls), 2)

    def test_canonical_layout_json_is_sort_keys_compact(self):
        lay = layout(maxv=3, maxp=2, clip=1, params=(2, 10))
        lay["params"] = {str(k): v for k, v in lay["params"].items()}
        lay["f16_halves"] = {"10": [28, 29], "2": [26, 27]}
        text = vs.canonical_layout_json(lay)
        norm = json.loads(text)
        self.assertEqual(text, json.dumps(norm, sort_keys=True, separators=(",", ":")))
        self.assertLess(text.index('"10":'), text.index('"2":'))  # lexicographic ("10" < "2"), byte-matches C++
        self.assertEqual(vs.layout_sha256(lay), hashlib.sha256(text.encode("utf-8")).hexdigest())
        with self.assertRaises(ValueError):
            vs.canonical_layout_json(dict(lay, slot_bytes=64.5))

    def test_swapped_producer_same_shape_refused_without_tools(self):
        la = layout(clip=0, params=(0,))
        lb = layout(clip=4, params=(0,))  # same location {0}, different offsets/stride/slot
        self.assertEqual(set(la["params"]), {"0" if isinstance(k, str) else 0 for k in lb["params"]})
        self.assertNotEqual(vs.layout_sha256(la), vs.layout_sha256(lb))
        with tempfile.TemporaryDirectory() as tmp:
            write_index(tmp, {"ms_a": {"mesh_compute": la}, "ms_b": {"mesh_compute": lb}})
            pa, _ = write_producer_and_seal(tmp, "ms_a", la, producer_bytes=b"producer-A")
            pb, _ = write_producer_and_seal(tmp, "ms_b", lb, producer_bytes=b"producer-B")
            with open(pa, "rb") as fa, open(pb, "rb") as fb:
                ba, bb = fa.read(), fb.read()
            with open(pa, "wb") as fa, open(pb, "wb") as fb:  # swap
                fa.write(bb)
                fb.write(ba)
            with mock.patch.object(vs, "tool_path",
                                   side_effect=AssertionError("must not resolve tools on swapped producer")), \
                    mock.patch.object(vs.subprocess, "run",
                                      side_effect=AssertionError("must not invoke tools on swapped producer")):
                with self.assertRaises(RuntimeError) as ctx:
                    vs.build_for_index(tmp)
            self.assertIn("producer bytes do not match the seal", str(ctx.exception))
            self.assertFalse(os.path.exists(os.path.join(tmp, "ms_a.mesh_vs.spv")))

    def test_tampered_index_layout_refused_without_tools(self):
        lay = layout()
        with tempfile.TemporaryDirectory() as tmp:
            write_index(tmp, {"ms_a": {"mesh_compute": lay}})
            write_producer_and_seal(tmp, "ms_a", lay)
            tampered = dict(lay, slot_bytes=lay["slot_bytes"] + 16)
            write_index(tmp, {"ms_a": {"mesh_compute": tampered}})
            with mock.patch.object(vs, "tool_path",
                                   side_effect=AssertionError("must not resolve tools on tampered index")), \
                    mock.patch.object(vs.subprocess, "run",
                                      side_effect=AssertionError("must not invoke tools on tampered index")):
                with self.assertRaises(RuntimeError) as ctx:
                    vs.build_for_index(tmp)
            self.assertIn("index layout does not match the seal", str(ctx.exception))

    def test_replaced_cached_binary_rebuilds_not_skipped(self):
        lay = layout()
        with tempfile.TemporaryDirectory() as tmp:
            write_index(tmp, {"ms_a": {"mesh_compute": lay}})
            write_producer_and_seal(tmp, "ms_a", lay)
            self.calls = []
            with mock.patch.object(vs, "tool_path", side_effect=lambda name: name), \
                    mock.patch.object(vs.subprocess, "run", side_effect=self.fake_tools(self)):
                vs.build_for_index(tmp)
                self.assertEqual(len(self.calls), 2)
                with open(os.path.join(tmp, "ms_a.mesh_vs.spv"), "wb") as f:  # tamper cached binary
                    f.write(b"forged-bytes")
                self.calls.clear()
                vs.build_for_index(tmp)
                self.assertEqual(len(self.calls), 2)  # binary hash mismatch forces rebuild, not skip
                with open(os.path.join(tmp, "ms_a.mesh_vs.spv"), "rb") as f:
                    self.assertEqual(f.read(), b"spv-bytes")

    def test_missing_seal_for_declared_representation_errors(self):
        with tempfile.TemporaryDirectory() as tmp:
            write_index(tmp, {"ms_a": {"mesh_compute": layout()}})
            with open(os.path.join(tmp, "ms_a.spv"), "wb") as f:
                f.write(b"producer")
            with self.assertRaises(RuntimeError) as ctx:
                vs.build_for_index(tmp)
            self.assertIn("missing seal for declared mesh_compute", str(ctx.exception))
            write_index(tmp, {"ms_native": {"stage": "ms"}})  # native: no seal required, nothing to build
            self.assertEqual(vs.build_for_index(tmp), [])

    def test_write_skips_identical_without_temp_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            write_index(tmp, {"ms_a": {"mesh_compute": layout()}})
            first = vs.write_for_index(tmp)
            mtime = os.path.getmtime(first[0])
            second = vs.write_for_index(tmp)
            self.assertEqual(second, first)
            self.assertEqual(os.path.getmtime(first[0]), mtime)
            self.assertEqual(sorted(os.listdir(tmp)), ["index.json", "ms_a.mesh_vs.spvasm"])

    def test_pack_retains_local_structure(self):
        la, lb = layout(), layout(clip=1)
        with tempfile.TemporaryDirectory() as root:
            write_index(root, {"ms_main": {"mesh_compute": la}})
            write_producer_and_seal(root, "ms_main", la)
            os.mkdir(os.path.join(root, "fspx"))
            write_index(os.path.join(root, "fspx"), {"ms_aux": {"mesh_compute": lb}})
            write_producer_and_seal(os.path.join(root, "fspx"), "ms_aux", lb)
            os.mkdir(os.path.join(root, "bvh"))
            self.calls = []
            with mock.patch.object(vs, "tool_path", side_effect=lambda name: name), \
                    mock.patch.object(vs.subprocess, "run", side_effect=self.fake_tools(self)):
                built = vs.build_for_pack(root)
            self.assertEqual(built, sorted([os.path.join(root, "ms_main.mesh_vs.spv"),
                                            os.path.join(root, "fspx", "ms_aux.mesh_vs.spv")]))
            self.assertTrue(os.path.isfile(os.path.join(root, "ms_main.mesh_vs.spvasm")))
            self.assertTrue(os.path.isfile(os.path.join(root, "fspx", "ms_aux.mesh_vs.spvasm")))
            self.assertEqual(os.listdir(os.path.join(root, "bvh")), [])
            self.calls.clear()
            with mock.patch.object(vs, "tool_path", side_effect=lambda name: name), \
                    mock.patch.object(vs.subprocess, "run", side_effect=self.fake_tools(self)):
                again = vs.build_for_pack(root)
            self.assertEqual(again, built)
            self.assertEqual(self.calls, [])

    def test_pack_propagates_128b_with_index_name(self):
        with tempfile.TemporaryDirectory() as root:
            la = layout()
            write_index(root, {"ms_ok": {"mesh_compute": la}})
            write_producer_and_seal(root, "ms_ok", la)
            os.mkdir(os.path.join(root, "exe"))
            write_index(os.path.join(root, "exe"), {"ms_bad": {"mesh_compute": self.pc_tail_layout()}})
            self.calls = []
            with mock.patch.object(vs, "tool_path", side_effect=lambda name: name), \
                    mock.patch.object(vs.subprocess, "run", side_effect=self.fake_tools(self)):
                with self.assertRaises(RuntimeError) as ctx:
                    vs.build_for_pack(root)
            message = str(ctx.exception)
            for needle in ("ms_bad", "128-byte"):
                self.assertIn(needle, message)
            self.assertIn("exe", message)
            self.assertEqual(len(self.calls), 2)
            self.assertTrue(os.path.isfile(os.path.join(root, "ms_ok.mesh_vs.spv")))
            self.assertEqual(sorted(os.listdir(os.path.join(root, "exe"))), ["index.json"])


    def run_main(self, argv):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = vs.main(argv)
        return rc, out.getvalue(), err.getvalue()

    def test_cli_pack_and_multiple_dirs(self):
        with tempfile.TemporaryDirectory() as root:
            first = os.path.join(root, "a")
            second = os.path.join(root, "b")
            os.mkdir(first)
            os.mkdir(second)
            la, lb = layout(), layout()
            write_index(first, {"ms_a": {"mesh_compute": la}})
            write_index(second, {"ms_b": {"mesh_compute": lb}})
            write_producer_and_seal(first, "ms_a", la)
            write_producer_and_seal(second, "ms_b", lb)
            with mock.patch.object(vs, "tool_path", side_effect=lambda name: name), \
                    mock.patch.object(vs.subprocess, "run",
                                      side_effect=lambda cmd, **kw: self.fake_cli_build(cmd)):
                rc, out, err = self.run_main(["mesh_compute_vs.py", first, second])
                self.assertEqual((rc, err), (0, ""))
                self.assertEqual(sorted(out.split()),
                                 [os.path.join(first, "ms_a.mesh_vs.spv"),
                                  os.path.join(second, "ms_b.mesh_vs.spv")])
                rc, out, err = self.run_main(["mesh_compute_vs.py", "--pack", root])
                self.assertEqual(rc, 0)  # no index.json at root/a/b structure: nothing to do still succeeds

    def fake_cli_build(self, cmd):
        if cmd[0] == "spirv-as":
            with open(cmd[-1], "wb") as f:
                f.write(b"x")
        return subprocess.CompletedProcess(cmd, 0, "", "")

    def test_cli_pack_needs_exactly_one_root(self):
        rc, _, err = self.run_main(["mesh_compute_vs.py", "--pack"])
        self.assertEqual(rc, 2)
        self.assertIn("mesh_compute_vs.py", err)


if __name__ == "__main__":
    unittest.main()
