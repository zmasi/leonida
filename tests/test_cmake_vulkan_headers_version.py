"""A Windows runtime configure with Vulkan headers older than it needs must stop with a message naming the minimum.

Any vulkan/vulkan.h used to be accepted, so conda-forge's 1.3.231 headers configured fine and then failed deep in the
build ("'calibrated': unknown override specifier" in d3d12_com.h, 100+ errors in d3d12_vulkan_device.cpp).

Runs runtime/cmake/vulkan_headers_version.cmake with ``cmake -P`` on hand-written vulkan_core.h stubs (no compiler,
no real headers needed); skipped without cmake.
"""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MODULE = ROOT / "runtime" / "cmake" / "vulkan_headers_version.cmake"
CMAKE_LISTS = ROOT / "runtime" / "CMakeLists.txt"
CMAKE = shutil.which("cmake")


def module_minimum():
    m = re.search(r"^set\(LEONIDA_VULKAN_HEADERS_MIN (\d+)\)", MODULE.read_text(encoding="utf-8"), re.M)
    assert m, "set(LEONIDA_VULKAN_HEADERS_MIN <number>) not found"
    return int(m.group(1))


def run_module(version, wrap=None):
    """Writes a vulkan_core.h stub (version None: no VK_HEADER_VERSION line) and runs the check on its folder."""
    with tempfile.TemporaryDirectory() as tmp:
        inc = Path(tmp) / "include"
        (inc / "vulkan").mkdir(parents=True)
        line = f"#define VK_HEADER_VERSION {version}\n" if version is not None else ""
        (inc / "vulkan" / "vulkan_core.h").write_text(f"#ifndef VULKAN_CORE_H_\n{line}#endif\n", encoding="utf-8",
                                                     newline="\n")
        dirs = inc.as_posix() if wrap is None else wrap.format(inc.as_posix())
        script = Path(tmp) / "driver.cmake"
        script.write_text(f'include("{MODULE.as_posix()}")\n'
                          f'leonida_vulkan_headers_version("{dirs}")\n'
                          f'message(STATUS "found=${{LEONIDA_VULKAN_HEADER_VERSION}}")\n',
                          encoding="utf-8", newline="\n")
        return subprocess.run([CMAKE, "-P", str(script)], capture_output=True, text=True, timeout=60)


@unittest.skipUnless(CMAKE, "cmake not on PATH")
class VulkanHeadersVersionTest(unittest.TestCase):
    def test_old_headers_fail_and_name_the_minimum(self):
        r = run_module(231)
        self.assertNotEqual(r.returncode, 0)
        text = " ".join(r.stderr.split())
        self.assertIn("CMake Error", text)
        self.assertIn("VK_HEADER_VERSION 231", text)
        self.assertIn(f"needs {module_minimum()} or newer", text)
        for part in ("VULKAN_SDK", "CMakeCache.txt", "LEONIDA_VULKAN_HEADERS_FETCH=ON"):
            self.assertIn(part, text)

    def test_one_below_the_minimum_fails(self):
        self.assertNotEqual(run_module(module_minimum() - 1).returncode, 0)

    def test_minimum_passes(self):
        r = run_module(module_minimum())
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn(f"found={module_minimum()}", r.stdout)

    def test_fetchcontent_build_interface_is_unwrapped(self):
        self.assertNotEqual(run_module(231, wrap="$<BUILD_INTERFACE:{}>").returncode, 0)
        self.assertEqual(run_module(module_minimum(), wrap="$<BUILD_INTERFACE:{}>").returncode, 0)

    def test_header_without_a_version_is_left_alone(self):
        r = run_module(None)
        self.assertEqual(r.returncode, 0, r.stderr)


class PinsMeetTheMinimumTest(unittest.TestCase):
    def test_fetchcontent_pin(self):
        text = CMAKE_LISTS.read_text(encoding="utf-8")
        m = re.search(r"# vulkan-sdk-1\.\d+\.(\d+)\.\d+ \(same commit pinned", text)
        self.assertIsNotNone(m, "the FetchContent pin comment changed; update this test")
        self.assertGreaterEqual(int(m.group(1)), module_minimum())

    def test_ci_pin(self):
        ci = (ROOT / ".github" / "workflows" / "runtime.yml").read_text(encoding="utf-8")
        pins = re.findall(r"Checkout Vulkan headers \(vulkan-sdk-1\.\d+\.(\d+)\.\d+\)", ci)
        self.assertTrue(pins, "no 'Checkout Vulkan headers (vulkan-sdk-...)' step in runtime.yml")
        for p in pins:
            self.assertGreaterEqual(int(p), module_minimum())


class CMakeListsUsesTheModuleTest(unittest.TestCase):
    def test_windows_runtime_configure_calls_the_module(self):
        text = CMAKE_LISTS.read_text(encoding="utf-8")
        m = re.search(r"^if\(TARGET Vulkan::Headers AND WIN32 AND LEONIDA_BUILD_WINDOWS_RUNTIME\)\n(.*?)^endif\(\)",
                      text, re.S | re.M)
        self.assertIsNotNone(m, "the Windows-runtime header version check is missing from runtime/CMakeLists.txt")
        self.assertIn("vulkan_headers_version.cmake", m.group(1))
        self.assertIn("leonida_vulkan_headers_version(", m.group(1))
        # Before the targets that need the headers are declared.
        self.assertLess(m.start(), text.index("\nif(TARGET Vulkan::Headers)\n"))


if __name__ == "__main__":
    unittest.main()
