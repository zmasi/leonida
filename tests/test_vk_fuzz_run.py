"""tools/vk_fuzz_run.py (the compute-program wrapper behind the --oracle hw,vk modes of the differential fuzzers) and the
--oracle vk guard of each fuzzer that has the mode. Standard library only; no GPU, no llvm-mc.

    python -I tests/test_vk_fuzz_run.py
"""
import contextlib
import importlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
import unittest.mock as mock
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
sys.dont_write_bytecode = True

import hw_oracle.runner  # noqa: E402
import vk_fuzz_run as R  # noqa: E402


class ProgramText(unittest.TestCase):
    def test_wave32_shell(self):
        text = R.program_text("cs_x", ["v_mov_b32 v1, 5"], 4, 32).splitlines()
        self.assertEqual(text[:5], ["; cs_x", "; group: 32,1,1", "; rsrc2: 0x8088", "s_mov_b32 exec_lo, -1",
                                    "v_mov_b32 v20, v0"])
        self.assertIn("v_lshlrev_b32 v21, 4, v20", text)
        self.assertEqual(text[-1], "s_endpgm")
        self.assertEqual(sum(ln.startswith("buffer_store_dword") for ln in text), 4)

    def test_store_stride_follows_the_word_count(self):
        for words, shift in ((1, 2), (2, 3), (4, 4), (8, 5)):
            self.assertIn(f"v_lshlrev_b32 v21, {shift}, v20", R.program_text("c", [], words, 32).splitlines())

    def test_wave64_rewrites_masks_and_vcc_data(self):
        w = R.wave64_text
        self.assertEqual(w("s_mov_b32 vcc_lo, 1"), "s_mov_b64 vcc, 1")
        self.assertEqual(w("s_mov_b32 vcc_lo, 0"), "s_mov_b64 vcc, 0")
        self.assertEqual(w("v_cndmask_b32_e64 v11, 0, 1, vcc_lo"), "v_cndmask_b32_e64 v11, 0, 1, vcc")
        self.assertEqual(w("v_cmp_lt_u32_e64 s20, v1, v2"), "v_cmp_lt_u32_e64 s[20:21], v1, v2")
        self.assertEqual(w("v_cndmask_b32_e64 v1, 0, 1, s21"), "v_cndmask_b32_e64 v1, 0, 1, s[22:23]")
        self.assertEqual(w("s_mov_b32 s4, 7"), "s_mov_b32 s4, 7")

    def test_rewrite_false_keeps_lines_that_already_spell_wave64(self):
        lines = ["v_cmp_lt_u32_e64 s[20:21], v1, v2", "s_mov_b32 s20, 3"]
        text = R.program_text("c", lines, 4, 64, rewrite=False).splitlines()
        self.assertIn("; mattr: +wavefrontsize64,-wavefrontsize32", text)
        self.assertIn("s_mov_b64 exec, -1", text)
        self.assertIn("s_mov_b32 s20, 3", text)
        self.assertIn("v_cmp_lt_u32_e64 s[20:21], v1, v2", text)

    def test_opaque_seeds_compute_the_same_values_at_run_time(self):
        self.assertEqual(R.opaque_seed("v_mov_b32_e32 v3, 0x3f800000"), "v_xor_b32_e32 v3, 0xd16eeeee, v22")
        self.assertEqual(R.opaque_seed("v_mov_b32 v1, 5"), "v_xor_b32_e32 v1, 0xeeeeeeeb, v22")
        for line in ("s_mov_b32 s4, 1", "v_mov_b32 v1, v2", "v_mov_b32 v20, 3", "v_add_f32 v1, v2, v3"):
            self.assertEqual(R.opaque_seed(line), line)
        text = R.program_text("c", ["v_mov_b32 v1, 5"], 4, 32, opaque=True).splitlines()
        self.assertIn("buffer_load_dword v22, v21, s[0:3], 0 offen", text)
        self.assertLess(text.index("s_waitcnt vmcnt(0)"), text.index("v_xor_b32_e32 v1, 0xeeeeeeeb, v22"))
        for k in (0x0, 0x1234, 0xFFFFFFFF, 0x7FC00000):
            self.assertEqual(k ^ R.FILL ^ R.FILL, k)

    def test_exact_verdict(self):
        lines = ["v_add_u32 v1, v2, v3", "v_mov_b32 v8, v1", "v_mov_b32 v9, v2", "v_mov_b32 v10, v3", "v_mov_b32 v11, v4"]
        self.assertEqual(R.exact_verdict(lines, [1, 2, 3, 4], [1, 2, 3, 4]), ("match", ""))
        klass, why = R.exact_verdict(lines, [1, 2, 3, 4], [1, 9, 3, 4])
        self.assertEqual(klass, "mismatch")
        self.assertIn("word 1 (v2)", why)
        klass, why = R.exact_verdict(["s_nop 0"], [1, 2], [1, 3])
        self.assertEqual(klass, "mismatch")
        self.assertIn("word 1 (word 1)", why)
        klass, why = R.exact_verdict(lines, [1, 2], [1])
        self.assertEqual(klass, "mismatch")
        self.assertIn("word count differs", why)

    def test_run_programs_rejects_invalid_wave_and_cleans_up_on_error(self):
        import os
        import tempfile
        import unittest.mock as mock
        with self.assertRaises(ValueError):
            R.run_programs([], words=4, wave=16)
        with tempfile.TemporaryDirectory() as tmp:
            fake_exe = Path(tmp) / ("vk_compute_test.exe" if os.name == "nt" else "vk_compute_test")
            fake_exe.write_bytes(b"")
            with mock.patch("mc_command.mc_command", side_effect=RuntimeError("boom")):
                with self.assertRaises(RuntimeError):
                    R.run_programs([["s_nop 0"]], words=4, wave=32, build=tmp, work_parent=tmp)
            leftovers = [p for p in Path(tmp).iterdir() if p.name.startswith("vk_fuzz_")]
            self.assertEqual(leftovers, [])

    def test_read_dump_takes_the_first_wave_of_a_larger_buffer(self):
        import struct
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            dump = Path(tmp) / "d.bin"
            # wave32, words=1: vk_compute_test's buffer is at least 64 words, so the dump is 64 words long. Each lane
            # stores its own value, and the tail holds a sentinel that is not the harness fill, so a read that took
            # the wrong words, the wrong order or the tail would not match.
            sentinel = 0xDEADBEEF
            self.assertNotEqual(sentinel, R.FILL)
            dump.write_bytes(struct.pack("<64I", *([0x100 + lane for lane in range(32)] + [sentinel] * 32)))
            lanes = R.read_dump(str(dump), 1, 32)
            self.assertEqual(lanes, [[0x100 + lane] for lane in range(32)])
            self.assertNotIn([sentinel], lanes)
            # wave64, words=1 from a two-group dump (128 words): the buffer is longer than the request the other way
            # round, and only the first 64 words are the wave's.
            dump.write_bytes(struct.pack("<128I", *([0x200 + lane for lane in range(64)] + [sentinel] * 64)))
            self.assertEqual(R.read_dump(str(dump), 1, 64), [[0x200 + lane] for lane in range(64)])
            # An exact-size dump still reads as before.
            dump.write_bytes(struct.pack("<128I", *range(128)))
            lanes = R.read_dump(str(dump), 4, 32)
            self.assertEqual(len(lanes), 32)
            self.assertEqual(lanes[0], [0, 1, 2, 3])
            self.assertEqual(lanes[31], [124, 125, 126, 127])
            # Too short or missing: no result.
            dump.write_bytes(struct.pack("<31I", *range(31)))
            self.assertIsNone(R.read_dump(str(dump), 1, 32))
            self.assertIsNone(R.read_dump(str(Path(tmp) / "missing.bin"), 1, 32))

    def test_judge_counts_skips_and_mismatches(self):
        import unittest.mock as mock
        obs = ["v_mov_b32 v8, v1", "v_mov_b32 v9, v2", "v_mov_b32 v10, v3", "v_mov_b32 v11, v4"]
        progs = [["a"] + obs, ["b"] + obs, ["c"] + obs]
        with mock.patch.object(R, "run_programs", return_value=[[1, 2, 3, 4], [1, 2, 3, 5], "llvm-mc: bad"]):
            counts, bad = R.judge(progs, [[1, 2, 3, 4], [1, 2, 3, 4], [0, 0, 0, 0]], R.exact_verdict)
        self.assertEqual(counts, {"match": 1, "mismatch": 1, "skip: llvm-mc: bad": 1})
        self.assertEqual([b[0] for b in bad], [1])

    def test_all_skipped(self):
        self.assertTrue(R.all_skipped({}))
        self.assertTrue(R.all_skipped({"skip: vk_compute_test: unsupported": 3}))
        self.assertTrue(R.all_skipped({"skip: hardware: no": 2, "match": 0}))
        self.assertFalse(R.all_skipped({"match": 1, "skip: x": 2}))
        self.assertFalse(R.all_skipped({"denormal": 1, "skip: x": 2}))

    def _run_with_fake_gpu(self, tmp, outcomes, calls, run, retries=0):
        """Run the real assembly/translation/GPU orchestration with synthetic child procedures."""
        if R.SHADERS not in sys.path:
            sys.path.insert(0, R.SHADERS)
        import compute_fixture
        import mc_command

        (Path(tmp) / ("vk_compute_test.exe" if os.name == "nt" else "vk_compute_test")).touch()

        def child(cmd, *, env=None, **_kwargs):
            if "--out" in cmd:
                out = Path(cmd[cmd.index("--out") + 1])
                out.mkdir()
                (out / "index.json").write_text(json.dumps({f"cs_vkfuzz_{i}": {} for i in range(len(outcomes))}),
                                                   encoding="utf-8", newline="\n")
                for i in range(len(outcomes)):
                    (out / f"cs_vkfuzz_{i}.spv").write_bytes(b"synthetic")
                return subprocess.CompletedProcess(cmd, 0, "", "")
            i = int(Path(cmd[1]).stem.rsplit("_", 1)[1])
            calls.append(i)
            Path(env["VKC_DUMP"]).write_bytes(bytes(128))  # fresh, complete 32-lane, one-word dump
            rc, stdout, stderr = outcomes[i]
            return subprocess.CompletedProcess(cmd, rc, stdout, stderr)

        mc_ok = subprocess.CompletedProcess([], 0, "v_mov_b32 v8, 0\n", "")
        with mock.patch.object(mc_command, "mc_command", return_value=["synthetic-llvm-mc"]), \
             mock.patch.object(mc_command, "run", return_value=mc_ok), \
             mock.patch.object(compute_fixture, "compute_header", return_value=b"synthetic"), \
             mock.patch.object(R.subprocess, "run", side_effect=child), \
             mock.patch.dict(os.environ, {"VK_FUZZ_KEEP": ""}):
            return run(tmp, retries)

    def test_failed_gpu_exit_is_harness_error_even_with_fresh_dump_and_subgroup_warning(self):
        lines = [["v_mov_b32 v8, 0"]]
        for warning in ("compute subgroup size: 8 not available\n", "compute subgroup size: 32\n"):
            with self.subTest(warning=warning), tempfile.TemporaryDirectory() as tmp:
                calls = []
                with self.assertRaisesRegex(RuntimeError, r"cs_vkfuzz_0: vk_compute_test exit 1") as error:
                    self._run_with_fake_gpu(
                        tmp, [(1, warning, "synthetic validation failure after dump\n")], calls,
                        lambda folder, _retries: R.judge(lines, [[0]], R.exact_verdict, wave=32, words=1,
                                                         build=folder, work_parent=folder))
                self.assertIn("synthetic validation failure after dump", str(error.exception))
                self.assertEqual(calls, [0, 0, 0])  # the existing retry budget is exhausted first
                self.assertFalse(any(p.name.startswith("vk_fuzz_") for p in Path(tmp).iterdir()))

    def test_successful_unpinned_and_unsupported_runs_still_skip(self):
        outcomes = [(0, "compute subgroup size: 8 not available\n", ""),
                    (77, "", "unsupported: no matching device\n"),
                    (0, "compute subgroup size: 32\n", "")]
        with tempfile.TemporaryDirectory() as tmp:
            calls = []
            got = self._run_with_fake_gpu(
                tmp, outcomes, calls,
                lambda folder, retries: R.run_programs([["v_mov_b32 v8, 0"]] * 3, words=1, wave=32,
                                                       build=folder, work_parent=folder, retries=retries))
        self.assertEqual(calls, [0, 1, 2])
        self.assertEqual(got, ["the device can't run 32-lane subgroups (8 lanes)",
                               "vk_compute_test: unsupported: no matching device", [0]])


    def test_encoded_listings_skip_llvm_mc_and_follow_the_wave64_emulation_switch(self):
        listing = "; cs_vkfuzz_0\n; encoding: [0x00,0x00,0x80,0xbf]\ns_endpgm\n"
        if R.SHADERS not in sys.path:
            sys.path.insert(0, R.SHADERS)
        import compute_fixture
        import mc_command

        for wave, emulate in ((32, ""), (64, ""), (64, "1")):
            with self.subTest(wave=wave, emulate=emulate), tempfile.TemporaryDirectory() as tmp:
                (Path(tmp) / ("vk_compute_test.exe" if os.name == "nt" else "vk_compute_test")).touch()
                seen = {"translator": None, "gpu": None, "env": None, "listing": None}

                def child(cmd, *, env=None, **_kwargs):
                    if "--out" in cmd:
                        out = Path(cmd[cmd.index("--out") + 1])
                        out.mkdir()
                        seen["translator"] = list(cmd)
                        seen["listing"] = (Path(cmd[cmd.index("--out") - 1]) / "cs_vkfuzz_0.s").read_text(
                            encoding="utf-8")
                        (out / "index.json").write_text(json.dumps({"cs_vkfuzz_0": {}}), encoding="utf-8",
                                                        newline="\n")
                        (out / "cs_vkfuzz_0.spv").write_bytes(b"synthetic")
                        return subprocess.CompletedProcess(cmd, 0, "", "")
                    seen["gpu"], seen["env"] = list(cmd), env
                    Path(env["VKC_DUMP"]).write_bytes(bytes(4 * wave))
                    return subprocess.CompletedProcess(cmd, 0, "", "")

                with mock.patch.object(mc_command, "mc_command", side_effect=AssertionError("llvm-mc was asked")), \
                     mock.patch.object(mc_command, "run", side_effect=AssertionError("llvm-mc was run")), \
                     mock.patch.object(compute_fixture, "compute_header", return_value=b"synthetic"), \
                     mock.patch.object(R.subprocess, "run", side_effect=child), \
                     mock.patch.dict(os.environ, {"LEONIDA_W64_EMULATE": emulate}):
                    got = R.run_programs([listing], words=1, wave=wave, build=tmp, work_parent=tmp, encoded=True)
                self.assertEqual(got, [[0]])
                self.assertEqual(seen["listing"], "; cs_vkfuzz_0\n" + listing)  # the text, as given
                emulating = wave == 64 and emulate == "1"
                self.assertEqual("--wave64-emulate" in seen["translator"], emulating)
                self.assertEqual("--require-wave64" in seen["gpu"], wave == 64 and not emulating)
                self.assertEqual(seen["env"].get("VKC_SUBGROUP_SIZE"), None if wave == 64 and not emulating else "32")


class Guard(unittest.TestCase):
    FUZZERS = ("pk16_diff", "vint_diff", "sdwa_diff", "wint_diff", "fmisc_diff")

    def test_gpu_harness_exception_exits_two_in_each_caller(self):
        for tool in self.FUZZERS:
            mod = importlib.import_module(tool)
            oracle = mock.Mock(device="synthetic", arch="gfx1030")
            out = io.StringIO()
            with self.subTest(tool=tool), \
                 mock.patch.object(hw_oracle.runner, "open_oracle", return_value=oracle), \
                 mock.patch.object(mod, "run_suite", side_effect=RuntimeError("vk_compute_test exit 1")), \
                 contextlib.redirect_stdout(out):
                self.assertEqual(mod.main(["--oracle", "hw,vk", "--programs", "2"]), 2)
            self.assertIn("harness error: vk_compute_test exit 1", out.getvalue())

    def test_vk_without_hw_is_a_usage_error(self):
        for tool in self.FUZZERS:
            with self.subTest(tool=tool):
                r = subprocess.run([sys.executable, "-I", str(TOOLS / f"{tool}.py"), "--oracle", "vk", "--programs", "1"],
                                   capture_output=True, text=True)
                self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
                self.assertIn("--oracle hw,vk", r.stderr)

    def test_hw_vk_exits_two_when_all_vk_programs_skip(self):
        for tool in self.FUZZERS:
            mod = importlib.import_module(tool)
            for vk_counts, expected_rc in (
                ({"skip: vk_compute_test: unsupported": 2}, 2),
                ({"match": 1, "skip: vk_compute_test: unsupported": 1}, 0),
            ):
                oracle = mock.Mock(device="mock", arch="gfx1030")
                counts = {"pass": 2, "mismatch": 0, "skip": {}, "vk": dict(vk_counts), "vk_failures": []}
                out = io.StringIO()
                with self.subTest(tool=tool, expected_rc=expected_rc):
                    with mock.patch.object(hw_oracle.runner, "open_oracle", return_value=oracle), \
                         mock.patch.object(mod, "run_suite", return_value=(counts, [])), \
                         contextlib.redirect_stdout(out):
                        rc = mod.main(["--oracle", "hw,vk", "--programs", "2"])
                    self.assertEqual(rc, expected_rc, out.getvalue())
                    if expected_rc == 2:
                        self.assertIn("no Vulkan program ran: every one was skipped", out.getvalue())
                    else:
                        self.assertNotIn("no Vulkan program ran", out.getvalue())


if __name__ == "__main__":
    unittest.main()
