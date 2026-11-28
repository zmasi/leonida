"""Every test executable the runtime builds must be registered with ctest.

``add_executable(foo_test ...)`` without an ``add_test(... COMMAND foo_test)`` builds
the test but never runs it -- in ``ctest``, in the Windows runtime job or under Wine.
This happened to ``d3d12_indirect_rt_test``; the only intentional exception is
``d3d12_clear_test`` (see the comment next to its target in runtime/CMakeLists.txt).
"""
import os
import re
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CMAKE = Path(os.environ.get("LEONIDA_RUNTIME_CMAKE", str(ROOT / "runtime" / "CMakeLists.txt")))
SOURCE = Path(os.environ.get("LEONIDA_RUNTIME_TEST_SOURCE",
                             str(ROOT / "runtime" / "test" / "d3d12_indirect_rt_test.cpp")))

# d3d12_clear_test is deliberately run by hand until #521's tie rounding lands.
NOT_IN_CTEST = {"d3d12_clear_test"}


def built_test_executables(text):
    return {name for name in re.findall(r"add_executable\(\s*([A-Za-z0-9_]+)", text)
            if name.endswith("_test")}


def registered_commands(text):
    # add_test may span lines, e.g. add_test(NAME x\n  COMMAND foo_test ...).
    return set(re.findall(r"add_test\([^)]*?\bCOMMAND\s+\"?([A-Za-z0-9_]+)", text, re.S))


class CTestRegistrationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cmake = CMAKE.read_text(encoding="utf-8")

    def test_all_test_executables_have_an_add_test(self):
        missing = built_test_executables(self.cmake) - registered_commands(self.cmake) - NOT_IN_CTEST
        self.assertEqual(missing, set(), f"built but never run by ctest: {sorted(missing)}")

    def test_the_exception_is_still_the_only_one(self):
        # An allowlist that grows unnoticed is how this bug happens again.
        self.assertEqual(NOT_IN_CTEST & built_test_executables(self.cmake), {"d3d12_clear_test"})

    def test_indirect_rt_skips_with_the_ctest_skip_code(self):
        self.assertTrue(
            "add_test(NAME d3d12_indirect_rt COMMAND d3d12_indirect_rt_test)" in self.cmake,
            "d3d12_indirect_rt_test is built but not registered with ctest",
        )
        self.assertTrue(
            "set_tests_properties(d3d12_indirect_rt PROPERTIES SKIP_RETURN_CODE 77)" in self.cmake,
            "d3d12_indirect_rt must skip with 77 on a machine without a Vulkan GPU",
        )

        lines = SOURCE.read_text(encoding="utf-8").splitlines()
        skips = []
        for i, line in enumerate(lines):
            if 'std::printf("SKIP:' in line:
                # The path's own return, not a conditional one inside it (a LEONIDA_VK_DEVICE filter that matches
                # nothing fails with `if (!filter.empty()) return 1;` just before the skip's `return 77;`).
                codes = [int(m) for l in lines[i:i + 7] if "if (" not in l for m in re.findall(r"return (\d+);", l)]
                skips.append(codes[0] if codes else None)
        # no loader, no ICD, no Vulkan 1.3 GPU -- all three are a ctest SKIP, not a failure.
        self.assertEqual(len(skips), 3, f"expected three whole-test SKIP paths, got {len(skips)}")
        self.assertEqual(skips, [77, 77, 77], "SKIP paths must return 77 for SKIP_RETURN_CODE 77")

    def test_xdxr_postbuild_readback_skips_with_the_ctest_skip_code(self):
        self.assertIn(
            "add_test(NAME vk_xdxr_postbuild_readback COMMAND vk_xdxr_postbuild_readback_test)",
            self.cmake,
            "vk_xdxr_postbuild_readback_test is built but not registered with ctest",
        )
        self.assertIn(
            "set_tests_properties(vk_xdxr_postbuild_readback PROPERTIES SKIP_RETURN_CODE 77)",
            self.cmake,
            "vk_xdxr_postbuild_readback must skip with 77 on a runner without a Vulkan GPU",
        )
        src = (ROOT / "runtime" / "test" / "vk_xdxr_postbuild_readback_test.cpp").read_text(encoding="utf-8")
        lines = src.splitlines()
        skips = [
            int(m.group(1))
            for i, line in enumerate(lines)
            if ": skipped (" in line and i + 1 < len(lines)
            for m in [re.match(r"\s*return\s+(\d+)\s*;", lines[i + 1])]
            if m
        ]
        self.assertEqual(skips, [77], "vk_xdxr_postbuild_readback skip path must return 77")

    def test_whole_test_skips_return_the_skip_code(self):
        # A test registered with SKIP_RETURN_CODE 77 that prints SKIP and returns 0 is counted as passed although it
        # checked nothing: bink_audio_test did that for a movie without audio tracks after #1580.
        wrong = whole_test_skips_not_77(self.cmake, ROOT / "runtime")
        self.assertEqual(wrong, [], f"SKIP followed by a return code other than 77: {wrong}")

    def test_a_named_skip_code_counts_by_its_value(self):
        cmake = ("add_executable(a_test test/a_test.cpp)\nadd_test(NAME a COMMAND a_test)\n"
                 "set_tests_properties(a PROPERTIES SKIP_RETURN_CODE 77)\n")
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "test" / "a_test.cpp"
            src.parent.mkdir()
            for value, want in (("77", []), ("0", [("test/a_test.cpp:3", "kSkip")])):
                src.write_text(f"constexpr int kSkip = {value};\nstd::printf(\"SKIP: none\\n\");\nreturn kSkip;\n",
                               encoding="utf-8", newline="\n")
                self.assertEqual(whole_test_skips_not_77(cmake, Path(tmp)), want, value)
            src.write_text('std::printf("SKIP: none\\n");\nreturn kUnknown;\n', encoding="utf-8", newline="\n")
            self.assertEqual(whole_test_skips_not_77(cmake, Path(tmp)), [("test/a_test.cpp:2", "kUnknown")])

    def test_host_parity_and_storage_tests_have_timeouts(self):
        for test_name in ("dstorage", "dxgi_host_parity", "dxgi_waitable_latency"):
            m = re.search(
                r"set_tests_properties\(\s*" + test_name + r"\b[^)]*TIMEOUT\s+[1-9]\d*",
                self.cmake,
                re.S,
            )
            self.assertIsNotNone(m, f"{test_name} must have an explicit TIMEOUT property")


def whole_test_skips_not_77(cmake_text, runtime_dir):
    """[(source:line, code)] for a SKIP print directly followed by `return N;` (N != 77) in the sources of a test
    registered with SKIP_RETURN_CODE 77. A SKIP print with no return right after it skips one section, not the test.
    A named code (`return kSkip;`) counts by the value of its `constexpr int` in the same source."""
    skip_tests = set(re.findall(r"set_tests_properties\(\s*([A-Za-z0-9_]+)\s+PROPERTIES[^)]*SKIP_RETURN_CODE\s+77",
                                cmake_text))
    commands = dict(re.findall(r"add_test\(\s*NAME\s+([A-Za-z0-9_]+)\s+COMMAND\s+\"?([A-Za-z0-9_]+)", cmake_text))
    wrong = []
    for test in sorted(skip_tests):
        exe = commands.get(test)
        m = exe and re.search(r"add_executable\(\s*" + re.escape(exe) + r"\b([^)]*)\)", cmake_text)
        if not m:
            continue
        for src in re.findall(r"(test/[A-Za-z0-9_./]+\.cpp)", m.group(1)):
            path = runtime_dir / src
            if not path.is_file():
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            named = dict(re.findall(r"\bconstexpr\s+int\s+(\w+)\s*=\s*(\d+)\s*;", text))
            lines = text.splitlines()
            for i, line in enumerate(lines):
                if not re.search(r'(printf|puts)\(\s*"SKIP', line):
                    continue
                after = re.match(r"\s*return\s+(\w+)\s*;", lines[i + 1]) if i + 1 < len(lines) else None
                if after and named.get(after.group(1), after.group(1)) != "77":
                    wrong.append((f"{src}:{i + 2}", after.group(1)))
    return wrong


if __name__ == "__main__":
    unittest.main()
