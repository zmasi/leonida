"""Synthetic tests for the wine launcher of runtime/test/check_device_filter_tools.py (issue #1150). No GPU, no wine."""
import importlib.util
import io
import os
import tempfile
import unittest
from contextlib import redirect_stdout
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT = os.path.join(ROOT, "runtime", "test", "check_device_filter_tools.py")


def load():
    spec = importlib.util.spec_from_file_location("check_device_filter_tools", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class LauncherTest(unittest.TestCase):
    def setUp(self):
        self.mod = load()
        self.tmp = tempfile.TemporaryDirectory(prefix="device-filter-")
        self.pe = os.path.join(self.tmp.name, "tool.exe")
        with open(self.pe, "wb") as f:
            f.write(b"MZ\x90\x00rest")
        self.elf = os.path.join(self.tmp.name, "tool")
        with open(self.elf, "wb") as f:
            f.write(b"\x7fELFrest")
        self.env = mock.patch.dict(os.environ, {}, clear=False)
        self.env.start()
        os.environ.pop("LEONIDA_WINE", None)

    def tearDown(self):
        self.env.stop()
        self.tmp.cleanup()

    def test_windows_runs_the_tool_directly(self):
        self.assertEqual(self.mod.launcher(self.pe, host="nt"), [])

    def test_posix_runs_a_pe_through_wine(self):
        self.assertEqual(self.mod.launcher(self.pe, host="posix", which=lambda n: "/usr/bin/" + n),
                         ["/usr/bin/wine"])

    def test_posix_runs_a_non_pe_directly(self):
        self.assertEqual(self.mod.launcher(self.elf, host="posix", which=lambda n: "/usr/bin/" + n), [])

    def test_posix_without_wine_still_names_a_loader(self):
        # main turns this into a skip with a message; the launcher itself does not decide.
        self.assertEqual(self.mod.launcher(self.pe, host="posix", which=lambda n: None), ["wine"])

    def test_leonida_wine_overrides_the_binary(self):
        os.environ["LEONIDA_WINE"] = "wine64"
        self.assertEqual(self.mod.launcher(self.pe, host="posix", which=lambda n: "/usr/bin/wine"), ["wine64"])

    def test_missing_tool_has_no_prefix(self):
        self.assertEqual(self.mod.launcher(os.path.join(self.tmp.name, "absent.exe"), host="posix"), [])

    def test_main_skips_when_wine_is_absent(self):
        out = io.StringIO()
        with mock.patch.object(self.mod, "launcher", return_value=["no-such-wine-loader"]), \
                mock.patch.object(self.mod.shutil, "which", return_value=None), \
                redirect_stdout(out):
            code = self.mod.main(["check", self.pe, self.pe, self.tmp.name])
        self.assertEqual(code, 77)
        self.assertIn("no-such-wine-loader is not on PATH", out.getvalue())


if __name__ == "__main__":
    unittest.main()
