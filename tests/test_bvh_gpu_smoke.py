"""tools/bvh_gpu_smoke.py without a GPU or game files: the heap image, the word count and the judging of runs, with
a fake vk_compute_test in place of the real one."""
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import bvh_gpu_smoke as smoke  # noqa: E402


def fake_runner(behaviour):
    """A runner that writes the heap image plus per-program changes to the dump, as vk_compute_test's VKC_DUMP does.
    behaviour: name -> ("ok", writes) | ("exit 1",) | ("validation",) | ("timeout",) | ("flaky", writes)."""
    calls = {}

    def run(vkc, spv, init, dump, heap_words, validation, timeout):
        name = Path(spv).stem
        calls[name] = calls.get(name, 0) + 1
        kind = behaviour.get(name, ("ok", {}))
        if kind[0] not in ("ok", "flaky"):
            return kind[0], "boom"
        heap = bytearray(Path(init).read_bytes())
        assert len(heap) == heap_words * 4
        for offset, word in kind[1].items():
            if kind[0] == "flaky" and calls[name] == 2:
                word ^= 1
            struct.pack_into("<I", heap, offset, word)
        Path(dump).write_bytes(bytes(heap))
        return "ok", "out[0..7] = ..."
    return run, calls


class HeapImage(unittest.TestCase):
    def test_table_then_zeros(self):
        image = smoke.heap_image(0x2000, 8192)
        self.assertEqual(len(image), 8192)
        self.assertEqual(struct.unpack_from("<Q", image, 0)[0], 0x2000)
        self.assertEqual(struct.unpack_from("<Q", image, 4088)[0], 0x2000)
        self.assertEqual(image[4096:], bytes(4096))

    def test_too_small(self):
        with self.assertRaises(ValueError):
            smoke.heap_image(0, 1024)

    def test_changed_words(self):
        a = bytes(16)
        b = struct.pack("<4I", 0, 7, 0, 9)
        self.assertEqual(smoke.changed_words(a, b), 2)


class Smoke(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.spirv = Path(self.dir.name)
        for name in ("Alpha", "Beta", "Gamma", "Delta", "Echo", "LeafRehydration"):
            (self.spirv / (name + ".spv")).write_bytes(b"\x03\x02\x23\x07")

    def tearDown(self):
        self.dir.cleanup()

    def run_smoke(self, behaviour, **kw):
        run, calls = fake_runner(behaviour)
        args = dict(table_qword=0x2000, heap_bytes=8192, validation=True, timeout=5, include_rehydration=False)
        args.update(kw)
        return smoke.smoke(self.spirv, "vkc", runner=run, **args), calls

    def test_statuses(self):
        results, calls = self.run_smoke({
            "Alpha": ("ok", {0x1000: 5, 0x1004: 6}),
            "Beta": ("exit 1",),
            "Gamma": ("validation",),
            "Delta": ("timeout",),
            "Echo": ("flaky", {0x1800: 1}),
        })
        got = {r["name"]: (r["status"], r["words_changed"]) for r in results}
        self.assertEqual(got, {"Alpha": ("pass", 2), "Beta": ("exit 1", None), "Gamma": ("validation", None),
                               "Delta": ("timeout", None), "Echo": ("nondeterministic", 1)})
        self.assertEqual(calls["Alpha"], 2, "a passing program runs twice")
        self.assertEqual(calls["Beta"], 1, "a failing first run is not repeated")
        self.assertEqual([r["name"] for r in results], sorted(got), "name order")

    def test_rehydration_is_left_to_test_376(self):
        results, calls = self.run_smoke({})
        self.assertNotIn("LeafRehydration", calls)
        results, calls = self.run_smoke({}, include_rehydration=True)
        self.assertIn("LeafRehydration", [r["name"] for r in results])


class Checked(unittest.TestCase):
    """--checked: the inputs each program gets and how its output is judged."""

    def test_heap_layout(self):
        heap = smoke.checked_heap([(0x8, 8, smoke.DST), (0x14, 4, 7)], 1 << 16)
        self.assertEqual(struct.unpack_from("<Q", heap, 0x8)[0], smoke.DST)
        self.assertEqual(struct.unpack_from("<I", heap, 0x14)[0], 7)
        self.assertEqual(heap[0x18:0x20], bytes(8), "the rest of the table is zero")
        self.assertEqual(heap[smoke.DST:smoke.DST + 4], bytes([smoke.FILL]) * 4)
        self.assertEqual(heap[smoke.SRC:smoke.SRC + 16], smoke._source_pattern(0))

    def test_expect_entries(self):
        heap = bytearray([smoke.FILL]) * 0x400
        entry = lambda i: struct.pack("<I", i)
        self.assertTrue(smoke._expect_entries(heap, 0, 4, 3, entry), "nothing written yet")
        for i in range(3):
            heap[4 * i:4 * i + 4] = entry(i)
        self.assertEqual(smoke._expect_entries(heap, 0, 4, 3, entry), [])
        heap[12] = 0
        self.assertIn("bytes past entry 2 changed", smoke._expect_entries(heap, 0, 4, 3, entry))

    def test_every_case_passes_with_the_output_it_describes(self):
        """A fake runner that does what each case says the program does: all pass; one wrong word: mismatch."""
        def ideal(spv, heap):
            name = Path(spv).stem
            fields = {off: val for off, _, val in smoke.CHECKED[name][0]}
            n = smoke.COUNT
            if name == "RadixReset":
                for i in range(n):
                    heap[fields[0] + 16 * i: fields[0] + 16 * (i + 1)] = bytes(16)
            elif name == "ResetKDOP":
                for i in range(n):
                    struct.pack_into("<2I", heap, fields[0] + 8 * i, 0xFF7FFFFF, 0x7F7FFFFF)
            elif name == "ResetToIndexBuffer":
                for i in range(n):
                    struct.pack_into("<4I", heap, fields[8] + 16 * i, *(fields[0x18] + 4 * i + k for k in range(4)))
            elif name == "CopyCounters":
                heap[fields[8]:fields[8] + 16 * n] = heap[fields[0]:fields[0] + 16 * n]
            elif name == "ResetCounterValues":
                for i in range(n):
                    struct.pack_into("<4I", heap, fields[8] + 16 * i, *[fields[0x18]] * 4)
            elif name == "PrefixSum64":
                values = struct.unpack_from("<64I", heap, fields[0])
                run = 0
                for i, x in enumerate(values):
                    struct.pack_into("<I", heap, fields[0] + 4 * i, run)
                    run = (run + x) & 0xFFFFFFFF
                struct.pack_into("<I", heap, fields[8], run)

        def runner_for(corrupt):
            def run(vkc, spv, init, dump, heap_words, validation, timeout):
                heap = bytearray(Path(init).read_bytes())
                ideal(spv, heap)
                if corrupt:
                    heap[smoke.DST + 4] ^= 1
                Path(dump).write_bytes(bytes(heap))
                return "ok", ""
            return run

        with tempfile.TemporaryDirectory() as d:
            for name in smoke.CHECKED:
                (Path(d) / (name + ".spv")).write_bytes(b"\x03\x02\x23\x07")
            good = smoke.run_checked(d, "vkc", heap_bytes=1 << 16, validation=False, timeout=5, runner=runner_for(False))
            self.assertEqual({r["name"]: r["status"] for r in good}, {n: "pass" for n in smoke.CHECKED})
            bad = smoke.run_checked(d, "vkc", heap_bytes=1 << 16, validation=False, timeout=5, runner=runner_for(True))
            self.assertEqual({r["status"] for r in bad}, {"mismatch"})

    def test_missing_program(self):
        with tempfile.TemporaryDirectory() as d:
            results = smoke.run_checked(d, "vkc", heap_bytes=1 << 16, validation=False, timeout=5,
                                        runner=lambda *a: ("ok", ""))
            self.assertEqual({r["status"] for r in results}, {"missing"})


class Cli(unittest.TestCase):
    def test_missing_vk_compute_test(self):
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(SystemExit) as e:
                smoke.main(["--spirv", d, "--build", d])
            self.assertIn("vk_compute_test", str(e.exception))

    def test_bad_heap_size(self):
        with self.assertRaises(SystemExit):
            smoke.main(["--spirv", ".", "--heap-bytes", "100"])

    def test_default_heap_bytes_is_16_mib(self):
        # At the default --table-qword 0x2000, BuildKDOP accesses ~2 MiB and RadixCount ~8 MiB; 1 MiB faults on NVIDIA.
        import subprocess
        import unittest.mock as mock
        seen = []

        def fake_run(cmd, env=None, **_kwargs):
            init = Path(env["VKC_HEAP_INIT"]).read_bytes()
            seen.append((int(env["VKC_OUT_WORDS"]), len(init), struct.unpack_from("<Q", init, 0)[0]))
            Path(env["VKC_DUMP"]).write_bytes(init)
            return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")

        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "Demo.spv").write_bytes(b"")
            with mock.patch.object(smoke, "find_vk_compute_test", return_value=Path(d) / "vkc"), \
                    mock.patch.object(smoke.subprocess, "run", side_effect=fake_run):
                code = smoke.main(["--spirv", d, "--no-validation"])
        self.assertEqual(code, 0)
        self.assertEqual(seen, [((16 << 20) // 4, 16 << 20, 0x2000)] * 2)

    def test_missing_dll(self):
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(SystemExit) as e:
                smoke.translate(d, d, 32768)
            self.assertIn("XboxBvhBuilder.dll", str(e.exception))

    def test_temp_work_dir_is_cleaned_up_on_success_and_error(self):
        with tempfile.TemporaryDirectory() as root:
            root_path = Path(root)
            seen_work = []

            def fake_translate(_game_dir, work_dir, _max_sm):
                work_dir = Path(work_dir)
                seen_work.append(work_dir)
                self.assertTrue(work_dir.is_dir())
                (work_dir / "Demo.spv").write_bytes(b"")
                return work_dir

            temp_base = root_path / "tmpbase"
            temp_base.mkdir()
            with mock.patch.object(smoke, "find_vk_compute_test", return_value=root_path / "vkc"), \
                    mock.patch.object(smoke, "translate", side_effect=fake_translate), \
                    mock.patch.object(smoke, "smoke", return_value=[{"name": "Demo", "status": "pass",
                                                                      "words_changed": 0, "detail": ""}]), \
                    mock.patch.object(smoke.tempfile, "gettempdir", return_value=str(temp_base)):
                rc = smoke.main(["--game-dir", root])
                self.assertEqual(rc, 0)
                self.assertEqual(len(seen_work), 1)
                self.assertFalse(seen_work[0].exists(), f"leaked temp work dir: {seen_work[0]}")

                def failing_translate(_game_dir, work_dir, _max_sm):
                    seen_work.append(Path(work_dir))
                    raise SystemExit("translate failed")

                with mock.patch.object(smoke, "translate", side_effect=failing_translate):
                    with self.assertRaises(SystemExit):
                        smoke.main(["--game-dir", root])
                self.assertEqual(len(seen_work), 2)
                self.assertFalse(seen_work[1].exists(), f"leaked temp work dir on error: {seen_work[1]}")

                explicit_work = root_path / "keep_work"
                explicit_work.mkdir()
                rc = smoke.main(["--game-dir", root, "--work", str(explicit_work)])
                self.assertEqual(rc, 0)
                self.assertTrue(explicit_work.is_dir())

    def test_nonpositive_max_shared_memory_rejected(self):
        for bad in ("0", "-64"):
            with self.subTest(val=bad):
                with self.assertRaises(SystemExit) as cm:
                    smoke.main(["--spirv", ".", "--max-shared-memory", bad])
                self.assertEqual(cm.exception.code, 2)

    def test_json_rejects_directory_and_input_aliases(self):
        with tempfile.TemporaryDirectory() as d:
            dpath = Path(d)
            spv_dir = dpath / "spirv"
            spv_dir.mkdir()
            spv = spv_dir / "Demo.spv"
            spv.write_bytes(b"spv")
            out_dir = dpath / "out_dir"
            out_dir.mkdir()
            game_dir = dpath / "game"
            game_dir.mkdir()
            dll = game_dir / "XboxBvhBuilder.dll"
            dll.write_bytes(b"dll")

            cases = [
                (["--spirv", str(spv_dir), "--json", str(out_dir)], out_dir, None),
                (["--spirv", str(spv_dir), "--json", str(spv)], spv, b"spv"),
                (["--game-dir", str(game_dir), "--json", str(dll)], dll, b"dll"),
            ]
            for argv, target, expected in cases:
                with mock.patch.object(smoke, "smoke") as run_smoke:
                    with self.assertRaises(SystemExit) as cm:
                        smoke.main(argv)
                    self.assertEqual(cm.exception.code, 2)
                    run_smoke.assert_not_called()
                if expected is not None:
                    self.assertEqual(target.read_bytes(), expected)

    def test_json_atomic_write_preserves_existing_and_cleans_ro_staging(self):
        with tempfile.TemporaryDirectory() as d:
            dpath = Path(d)
            (dpath / "Demo.spv").write_bytes(b"")
            report = dpath / "report.json"
            previous = '{"previous": true}\n'
            report.write_text(previous, encoding="utf-8", newline="\n")
            os.chmod(report, 0o444)
            fake_results = [{"name": "Demo", "status": "pass", "words_changed": 0, "detail": ""}]
            try:
                with mock.patch.object(smoke, "find_vk_compute_test", return_value=dpath / "vkc"), \
                        mock.patch.object(smoke, "smoke", return_value=fake_results), \
                        mock.patch.object(os, "replace", side_effect=PermissionError("replace denied")):
                    rc = smoke.main(["--spirv", str(dpath), "--json", str(report)])
                self.assertEqual(rc, 2)
                self.assertEqual(report.read_text(encoding="utf-8"), previous)
                self.assertEqual([p.name for p in dpath.iterdir() if p.name.startswith(".bvh_gpu_smoke-")], [])

                if os.name == "nt":
                    with mock.patch.object(smoke, "find_vk_compute_test", return_value=dpath / "vkc"), \
                            mock.patch.object(smoke, "smoke", return_value=fake_results):
                        rc = smoke.main(["--spirv", str(dpath), "--json", str(report)])
                    self.assertEqual(rc, 2)
                    self.assertEqual(report.read_text(encoding="utf-8"), previous)
                    self.assertEqual([p.name for p in dpath.iterdir() if p.name.startswith(".bvh_gpu_smoke-")], [])
            finally:
                for entry in dpath.iterdir():
                    try:
                        os.chmod(entry, 0o600)
                    except OSError:
                        pass


if __name__ == "__main__":
    unittest.main()
