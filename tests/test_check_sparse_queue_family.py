"""Synthetic tests for runtime/test/check_sparse_queue_family.py (TEST-1089).

The contract checker runs the GPU test twice, once under VK_LAYER_LUNARG_api_dump and once under
VK_LAYER_KHRONOS_validation, because api_dump's vkDestroyDevice can crash on the AMD Windows driver. These
tests check that each run loads exactly one of the two layers, that only that crash is allowed and only in
the api_dump run, and that any other crash, a skip or a missing case is still reported. No GPU or game data: the "GPU test" is a stub script.

Run:
    python -I tests/test_check_sparse_queue_family.py
"""
from __future__ import annotations

import contextlib
import importlib.util
import io
import re
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("check_sparse_queue_family",
                                              ROOT / "runtime" / "test" / "check_sparse_queue_family.py")
checker = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(checker)

PASS_OUT = "".join(f"RESULT LAYER_MAP_{kind}_COPY_COMPUTE PASS mapped_mismatch=0/65536 unmapped_nonzero=0/65536\n"
                   for kind in ("DIRECT", "COPY", "COMPUTE")) + "LAYER_FINAL PASS\n"


DESTROY_TRACE = ("vkDestroyPipelineLayout(device, pipelineLayout, pAllocator) returns void:\n"
                 "    device:                         VkDevice = 0000000000000000\n"
                 "vkDestroyDevice(device, pAllocator) returns void\n")


class Completed:
    def __init__(self, returncode: int, stdout: str = "", stderr: str = "") -> None:
        self.returncode, self.stdout, self.stderr = returncode, stdout, stderr


class PassEnvsTest(unittest.TestCase):
    def test_each_run_loads_one_layer(self) -> None:
        base = {"PATH": "x", "VK_INSTANCE_LAYERS": "VK_LAYER_LUNARG_api_dump", "LEONIDA_VK_VALIDATION": "1"}
        dump, validation = checker.pass_envs(base, Path("settings"))
        self.assertEqual(dump["VK_INSTANCE_LAYERS"], "VK_LAYER_LUNARG_api_dump")
        self.assertEqual(dump["LEONIDA_VK_VALIDATION"], "0")
        self.assertEqual(dump["VK_LAYER_SETTINGS_PATH"], "settings")
        self.assertNotIn("VK_INSTANCE_LAYERS", validation)
        self.assertEqual(validation["LEONIDA_VK_VALIDATION"], "1")
        self.assertEqual(dump["PATH"], "x")
        self.assertEqual(validation["PATH"], "x")

    def test_base_environment_is_not_changed(self) -> None:
        base = {"LEONIDA_VK_VALIDATION": "1"}
        checker.pass_envs(base, Path("s"))
        self.assertEqual(base, {"LEONIDA_VK_VALIDATION": "1"})


class RunPassTest(unittest.TestCase):
    def run_with(self, result: Completed) -> tuple[str, int]:
        with mock.patch.object(checker.subprocess, "run", return_value=result):
            return checker.run_pass(Path("t.exe"), {}, Path("."))

    def test_all_cases_pass(self) -> None:
        self.assertEqual(self.run_with(Completed(0, PASS_OUT))[1], 0)

    def test_skip_code_is_a_skip(self) -> None:
        self.assertEqual(self.run_with(Completed(77))[1], 77)

    def test_missing_layer_is_a_skip(self) -> None:
        self.assertEqual(self.run_with(Completed(1, stderr="[rt] d3d12: vkCreateInstance (Vulkan 1.3) failed"))[1], 77)

    def test_access_violation_fails(self) -> None:
        with self.assertRaises(AssertionError), contextlib.redirect_stderr(io.StringIO()):
            self.run_with(Completed(0xC0000005, PASS_OUT))

    def run_dump_with(self, result: Completed, *traces: str) -> int:
        with tempfile.TemporaryDirectory() as td:
            for i, trace in enumerate(traces):
                (Path(td) / f"sparse_create_info.log{i or ''}").write_text(trace, encoding="utf-8")
            with mock.patch.object(checker.subprocess, "run", return_value=result), \
                 contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                return checker.run_pass(Path("t.exe"), {}, Path("."), trace_dir=Path(td))[1]

    def test_api_dump_crash_in_destroy_device_after_the_results_is_allowed(self) -> None:
        self.assertEqual(self.run_dump_with(Completed(0xC0000005, PASS_OUT), DESTROY_TRACE), 0)

    def test_api_dump_crash_in_another_call_fails(self) -> None:
        for trace in (DESTROY_TRACE + "vkDestroyInstance(instance, pAllocator) returns void:\n",
                      "vkQueueSubmit2(queue, submitCount, pSubmits, fence) returns VkResult:\n", ""):
            with self.subTest(trace=trace), self.assertRaises(AssertionError):
                self.run_dump_with(Completed(0xC0000005, PASS_OUT), trace)

    def test_api_dump_crash_without_exactly_one_trace_fails(self) -> None:
        for traces in ((), (DESTROY_TRACE, DESTROY_TRACE)):
            with self.subTest(traces=len(traces)), self.assertRaises(AssertionError):
                self.run_dump_with(Completed(0xC0000005, PASS_OUT), *traces)

    def test_api_dump_crash_before_every_result_fails(self) -> None:
        for out in (PASS_OUT.replace("LAYER_FINAL PASS", ""), PASS_OUT.replace("LAYER_MAP_COPY", "LAYER_MAP_OTHER")):
            with self.subTest(out=out), self.assertRaises(AssertionError):
                self.run_dump_with(Completed(0xC0000005, out), DESTROY_TRACE)

    def test_each_case_and_the_final_line_must_be_there_exactly_once(self) -> None:
        # Review on #4900: a repeated case stood in for a missing one, and any line starting with the final line passed.
        copy_row = next(ln for ln in PASS_OUT.splitlines(True) if "LAYER_MAP_COPY_" in ln)
        direct_row = next(ln for ln in PASS_OUT.splitlines(True) if "LAYER_MAP_DIRECT_" in ln)
        outputs = {"repeated case for a missing one": PASS_OUT.replace(copy_row, direct_row),
                   "extra case": PASS_OUT + direct_row,
                   "longer final line": PASS_OUT.replace("LAYER_FINAL PASS", "LAYER_FINAL PASSING"),
                   "prefixed final line": PASS_OUT.replace("LAYER_FINAL PASS", "X LAYER_FINAL PASS"),
                   "two final lines": PASS_OUT + "LAYER_FINAL PASS\n"}
        for name, out in outputs.items():
            with self.subTest(name), self.assertRaises(AssertionError):
                self.run_with(Completed(0, out))
            with self.subTest(name + ", allowed crash"), self.assertRaises(AssertionError):
                self.run_dump_with(Completed(0xC0000005, out), DESTROY_TRACE)

    def test_api_dump_other_exit_codes_fail(self) -> None:
        for code in (1, 2, 0xC0000374, 0xC0000409):
            with self.subTest(code=hex(code)), self.assertRaises(AssertionError):
                self.run_dump_with(Completed(code, PASS_OUT), DESTROY_TRACE)

    def test_missing_case_fails(self) -> None:
        with self.assertRaises(AssertionError):
            self.run_with(Completed(0, PASS_OUT.replace("LAYER_MAP_COPY", "LAYER_MAP_OTHER")))


EXCLUSIVE_TRACE = (checker.SPARSE_FLAGS + ")\n"
                   "    sharingMode: VkSharingMode = VK_SHARING_MODE_EXCLUSIVE (0)\n"
                   "    queueFamilyIndexCount: uint32_t = 0\n") * 6
CONCURRENT_TRACE = (checker.SPARSE_FLAGS + ")\n"
                    "    sharingMode: VkSharingMode = VK_SHARING_MODE_CONCURRENT (1)\n"
                    "    queueFamilyIndexCount: uint32_t = 2\n"
                    "    pQueueFamilyIndices[0]: const uint32_t = 0\n"
                    "    pQueueFamilyIndices[1]: const uint32_t = 1\n") * 6


def make_queue_trace(pdev: str, families: list[tuple[int, int, tuple[int, int, int]]],
                     created_families: int, buffers: str, *, graphics_family: int = 0) -> str:
    entries = []
    for i, (flags, count, (w, h, d)) in enumerate(families):
        entries.append(
            f"        pQueueFamilyProperties[{i}]: VkQueueFamilyProperties = 0x{i:x}:\n"
            f"            queueFlags: VkQueueFlags = {flags}\n"
            f"            queueCount: uint32_t = {count}\n"
            f"            minImageTransferGranularity: VkExtent3D = 0:\n"
            f"                width: uint32_t = {w}\n"
            f"                height: uint32_t = {h}\n"
            f"                depth: uint32_t = {d}\n"
        )
    return (
        "vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, pQueueFamilyPropertyCount, pQueueFamilyProperties) returns void:\n"
        "<before calling into implementation>\n"
        f"    physicalDevice: VkPhysicalDevice = {pdev}\n"
        "<after returning from implementation>\n"
        f"    physicalDevice: VkPhysicalDevice = {pdev}\n"
        f"    pQueueFamilyPropertyCount: uint32_t* = {len(families)}\n"
        + "".join(entries)
        + "vkCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice) returns VkResult:\n"
        "<before calling into implementation>\n"
        f"    physicalDevice: VkPhysicalDevice = {pdev}\n"
        f"        queueCreateInfoCount: uint32_t = {created_families}\n"
        "            pQueueCreateInfos[0]: const VkDeviceQueueCreateInfo = 0x1:\n"
        f"                queueFamilyIndex: uint32_t = {graphics_family}\n"
        + buffers
    )


SINGLE_FAMILY_TRACE = make_queue_trace(
    "000002C74B774620",
    [(15, 1, (1, 1, 1)), (32, 2, (1, 1, 1))],  # graphics+compute+transfer+sparse, plus video-decode only
    created_families=1,
    buffers=EXCLUSIVE_TRACE,
)
NO_ASYNC_SPARSE_TRACE = make_queue_trace(
    "000002C74B774620",
    [(15, 1, (1, 1, 1)), (6, 2, (1, 1, 1)), (4, 1, (1, 1, 1))],  # async compute & copy lack SPARSE_BINDING_BIT (8)
    created_families=1,
    buffers=EXCLUSIVE_TRACE,
)
MULTI_FAMILY_DROPPED_TRACE = make_queue_trace(
    "000001C77C556640",
    [(15, 16, (1, 1, 1)), (12, 2, (1, 1, 1)), (14, 8, (1, 1, 1))],
    created_families=1,
    buffers=EXCLUSIVE_TRACE,
)
MULTI_FAMILY_OK_TRACE = make_queue_trace(
    "000001C77C556640",
    [(15, 16, (1, 1, 1)), (12, 2, (1, 1, 1)), (14, 8, (1, 1, 1))],
    created_families=3,
    buffers=CONCURRENT_TRACE,
)


class TraceTest(unittest.TestCase):
    def check(self, text):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "sparse.log"
            path.write_text(text, encoding="utf-8", newline="\n")
            return checker.check_trace(path, expect_exclusive=False)

    def test_large_family_list_is_not_truncated(self):
        record = (f"{checker.SPARSE_FLAGS})\nsharingMode: VkSharingMode = VK_SHARING_MODE_CONCURRENT\n"
                  "queueFamilyIndexCount: uint32_t = 12\n")
        record += "".join(f"pQueueFamilyIndices[{i}]: const uint32_t = {2*i}\n" for i in range(12))
        self.assertEqual(self.check(record * 6), (3, 6))

    def test_each_sparse_buffer_must_supply_its_own_complete_array(self):
        for invalid in (CONCURRENT_TRACE.replace("Indices[1]", "Indices[0]", 1),
                        CONCURRENT_TRACE.replace("Indices[1]", "Indices[2]", 1),
                        CONCURRENT_TRACE.replace("    pQueueFamilyIndices[1]: const uint32_t = 1\n", "", 1),
                        CONCURRENT_TRACE.replace("    pQueueFamilyIndices[1]", "vkOtherCall()\n    pQueueFamilyIndices[1]", 1)):
            with self.subTest(trace=invalid[:300]), self.assertRaises(AssertionError):
                self.check(invalid)


class MainTest(unittest.TestCase):
    def run_main(self, fake_run) -> int:
        with tempfile.TemporaryDirectory() as td:
            exe = Path(td) / "t.exe"
            exe.write_bytes(b"")
            with mock.patch.object(checker.subprocess, "run", side_effect=fake_run), \
                 mock.patch.object(sys, "argv", ["check", str(exe)]), \
                 contextlib.redirect_stdout(io.StringIO()):
                return checker.main()

    def test_broken_trace_fails_even_when_validation_skips(self) -> None:
        # Review on #4900: a skip from the validation run must not hide EXCLUSIVE sparse buffers from the first run.
        def fake_run(argv, cwd, env, **kwargs):
            if env.get("VK_INSTANCE_LAYERS") == "VK_LAYER_LUNARG_api_dump":
                (Path(cwd) / "sparse_create_info.log").write_text(EXCLUSIVE_TRACE, encoding="utf-8")
                return Completed(0, PASS_OUT)
            return Completed(77)

        with self.assertRaises(AssertionError):
            self.run_main(fake_run)

    def test_a_crash_in_the_validation_run_fails(self) -> None:
        # The api_dump trace on disk ends in vkDestroyDevice, but only the api_dump run may crash there.
        def fake_run(argv, cwd, env, **kwargs):
            if env.get("VK_INSTANCE_LAYERS") == "VK_LAYER_LUNARG_api_dump":
                (Path(cwd) / "sparse_create_info.log").write_text(CONCURRENT_TRACE + DESTROY_TRACE, encoding="utf-8")
            return Completed(0xC0000005, PASS_OUT)

        with self.assertRaises(AssertionError), contextlib.redirect_stderr(io.StringIO()):
            self.run_main(fake_run)

    def test_an_allowed_crash_still_passes_the_whole_check(self) -> None:
        def fake_run(argv, cwd, env, **kwargs):
            if env.get("VK_INSTANCE_LAYERS") == "VK_LAYER_LUNARG_api_dump":
                (Path(cwd) / "sparse_create_info.log").write_text(CONCURRENT_TRACE + DESTROY_TRACE, encoding="utf-8")
                return Completed(0xC0000005, PASS_OUT)
            return Completed(0, PASS_OUT)

        self.assertEqual(self.run_main(fake_run), 0)

    def test_both_runs_fit_the_ctest_timeout(self) -> None:
        cmake = (ROOT / "runtime" / "CMakeLists.txt").read_text(encoding="utf-8")
        limit = re.search(r"d3d12_sparse_queue_family_contract PROPERTIES[^)]*TIMEOUT (\d+)", cmake)
        self.assertIsNotNone(limit)
        self.assertLess(2 * checker.RUN_TIMEOUT, int(limit.group(1)))

    def test_validation_errors_fail_the_check(self) -> None:
        calls = []

        def fake_run(argv, cwd, env, **kwargs):
            calls.append(dict(env))
            if env.get("LEONIDA_VK_VALIDATION") == "1":
                return Completed(0, PASS_OUT + "Validation Error: [ VUID-vkCmdDraw-None-02699 ] synthetic\n")
            (Path(cwd) / "sparse_create_info.log").write_text(CONCURRENT_TRACE, encoding="utf-8")
            return Completed(0, PASS_OUT)

        with self.assertRaises(AssertionError):
            self.run_main(fake_run)
        self.assertEqual(len(calls), 2)
        self.assertEqual([c.get("VK_INSTANCE_LAYERS") for c in calls], ["VK_LAYER_LUNARG_api_dump", None])

    def run_main_with_trace(self, trace: str, *, extra_argv: list[str] | None = None,
                            validation_out: str = PASS_OUT) -> int:
        def fake_run(argv, cwd, env, **kwargs):
            if env.get("VK_INSTANCE_LAYERS") == "VK_LAYER_LUNARG_api_dump":
                (Path(cwd) / "sparse_create_info.log").write_text(trace, encoding="utf-8")
                return Completed(0, PASS_OUT)
            return Completed(0, validation_out)

        with tempfile.TemporaryDirectory() as td:
            exe = Path(td) / "t.exe"
            exe.write_bytes(b"")
            with mock.patch.object(checker.subprocess, "run", side_effect=fake_run), \
                 mock.patch.object(sys, "argv", ["check", str(exe), *(extra_argv or [])]), \
                 contextlib.redirect_stdout(io.StringIO()):
                return checker.main()

    def test_single_sparse_family_device_skips_with_77(self) -> None:
        self.assertEqual(self.run_main_with_trace(SINGLE_FAMILY_TRACE), 77)
        self.assertEqual(self.run_main_with_trace(NO_ASYNC_SPARSE_TRACE), 77)

    def test_single_sparse_family_baseline_mode_skips_with_77(self) -> None:
        self.assertEqual(self.run_main_with_trace(SINGLE_FAMILY_TRACE, extra_argv=["--expect-baseline-exclusive"]), 77)

    def test_multi_sparse_family_device_creating_one_family_still_fails(self) -> None:
        with self.assertRaises(AssertionError):
            self.run_main_with_trace(MULTI_FAMILY_DROPPED_TRACE)

    def test_single_family_device_with_concurrent_buffers_fails(self) -> None:
        bad = make_queue_trace("000002C74B774620", [(15, 1, (1, 1, 1))], created_families=1, buffers=CONCURRENT_TRACE)
        with self.assertRaises(AssertionError):
            self.run_main_with_trace(bad)

    def test_single_family_device_validation_error_still_fails(self) -> None:
        with self.assertRaises(AssertionError):
            self.run_main_with_trace(
                SINGLE_FAMILY_TRACE,
                validation_out=PASS_OUT + "Validation Error: [ VUID-vkCmdDraw-None-02699 ] synthetic\n",
            )

    def test_multi_family_device_passes_concurrent_contract(self) -> None:
        self.assertEqual(self.run_main_with_trace(MULTI_FAMILY_OK_TRACE), 0)

    def test_non_sparse_async_compute_ahead_of_sparse_compute_skips_with_77(self) -> None:
        # Mirrors queue_families.h: pick_queue_families chooses the first async compute family ([1]) and
        # sparse_capable_queue_families falls back to graphics ([0]) when [1] lacks SPARSE_BINDING_BIT.
        trace = make_queue_trace(
            "000002C74B774620",
            [(15, 1, (1, 1, 1)), (6, 2, (1, 1, 1)), (14, 2, (1, 1, 1))],
            created_families=1,
            buffers=EXCLUSIVE_TRACE,
        )
        self.assertEqual(self.run_main_with_trace(trace), 77)
        # Zero-count non-sparse compute is skipped by pick_queue_families, so [2] is selected and 1 family must fail.
        zero_count = make_queue_trace(
            "000002C74B774620",
            [(15, 1, (1, 1, 1)), (6, 0, (1, 1, 1)), (14, 2, (1, 1, 1))],
            created_families=1,
            buffers=EXCLUSIVE_TRACE,
        )
        with self.assertRaises(AssertionError):
            self.run_main_with_trace(zero_count)

    def test_non_sparse_dma_ahead_of_sparse_dma_skips_with_77(self) -> None:
        # Mirrors queue_families.h: pick_queue_families chooses the first fine-grained DMA family ([1]) and
        # sparse_capable_queue_families falls back to graphics ([0]) when [1] lacks SPARSE_BINDING_BIT.
        trace = make_queue_trace(
            "000002C74B774620",
            [(15, 1, (1, 1, 1)), (4, 1, (1, 1, 1)), (12, 1, (1, 1, 1))],
            created_families=1,
            buffers=EXCLUSIVE_TRACE,
        )
        self.assertEqual(self.run_main_with_trace(trace), 77)
        # Coarse-grained non-sparse DMA ([1]) is not chosen, so fine-grained sparse DMA ([2]) is picked -> must fail.
        coarse_first = make_queue_trace(
            "000002C74B774620",
            [(15, 1, (1, 1, 1)), (4, 1, (2, 2, 1)), (12, 1, (1, 1, 1))],
            created_families=1,
            buffers=EXCLUSIVE_TRACE,
        )
        with self.assertRaises(AssertionError):
            self.run_main_with_trace(coarse_first)
        # Non-sparse compute ([1]) + fine-grained sparse DMA ([2]) still leaves copy=[2] -> must fail.
        mixed = make_queue_trace(
            "000002C74B774620",
            [(15, 1, (1, 1, 1)), (6, 2, (1, 1, 1)), (12, 1, (1, 1, 1))],
            created_families=1,
            buffers=EXCLUSIVE_TRACE,
        )
        with self.assertRaises(AssertionError):
            self.run_main_with_trace(mixed)

    def test_incomplete_queue_family_dump_fails(self) -> None:
        two_family = make_queue_trace(
            "000002C74B774620",
            [(15, 1, (1, 1, 1)), (14, 2, (1, 1, 1))],
            created_families=1,
            buffers=EXCLUSIVE_TRACE,
        )
        head, tail = two_family.split("        pQueueFamilyProperties[1]:", 1)
        tail_entry, rest = tail.split("vkCreateDevice(", 1)
        count_query = (
            "vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, pQueueFamilyPropertyCount, pQueueFamilyProperties) returns void:\n"
            "<before calling into implementation>\n"
            "    physicalDevice: VkPhysicalDevice = 000002C74B774620\n"
            "<after returning from implementation>\n"
            "    physicalDevice: VkPhysicalDevice = 000002C74B774620\n"
            "    pQueueFamilyPropertyCount: uint32_t* = 2\n"
            "    pQueueFamilyProperties: VkQueueFamilyProperties* = NULL\n"
        )
        one_family_trace = make_queue_trace(
            "000002C74B774620",
            [(15, 1, (1, 1, 1))],
            created_families=1,
            buffers=EXCLUSIVE_TRACE,
        )
        broken = {
            "missing extent fields on entry 1": (
                head + "        pQueueFamilyProperties[1]:"
                + tail_entry.split("            minImageTransferGranularity:", 1)[0]
                + "vkCreateDevice(" + rest
            ),
            "entry 1 dropped while count=2": head + "vkCreateDevice(" + rest,
            "duplicate index 0 instead of 1": two_family.replace("pQueueFamilyProperties[1]:", "pQueueFamilyProperties[0]:", 1),
            "out-of-range index 2": two_family.replace("pQueueFamilyProperties[1]:", "pQueueFamilyProperties[2]:", 1),
            "missing pQueueFamilyPropertyCount": SINGLE_FAMILY_TRACE.replace(
                "    pQueueFamilyPropertyCount: uint32_t* = 2\n", "", 1
            ),
            "partial dump smaller than count query": count_query + one_family_trace,
        }
        for name, trace in broken.items():
            with self.subTest(name), self.assertRaises(AssertionError):
                self.run_main_with_trace(trace)

    def test_main_accepts_argv_writes_lf_settings_reads_bom_and_handles_oserror(self) -> None:
        settings_bytes = []

        def fake_run(argv, cwd, env, **kwargs):
            settings_bytes.append((Path(cwd) / "vk_layer_settings.txt").read_bytes())
            if env.get("VK_INSTANCE_LAYERS") == "VK_LAYER_LUNARG_api_dump":
                # Emit sorted trace with UTF-8 BOM on disk.
                (Path(cwd) / "sparse_create_info.log.0").write_bytes(
                    b"\xef\xbb\xbf" + MULTI_FAMILY_OK_TRACE.encode("utf-8")
                )
                return Completed(0, PASS_OUT)
            return Completed(0, PASS_OUT)

        with tempfile.TemporaryDirectory() as td:
            exe = Path(td) / "t.exe"
            exe.write_bytes(b"")
            with mock.patch.object(checker.subprocess, "run", side_effect=fake_run), \
                    contextlib.redirect_stdout(io.StringIO()):
                rc = checker.main([str(exe)])
        self.assertEqual(rc, 0)
        self.assertTrue(settings_bytes)
        self.assertNotIn(b"\r\n", settings_bytes[0])

        with self.assertRaises(AssertionError):
            with mock.patch.object(checker.subprocess, "run", side_effect=FileNotFoundError("no binary")):
                checker.run_pass(Path("missing.exe"), {}, Path("."))


if __name__ == "__main__":
    unittest.main()
