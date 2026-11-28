"""tools/conformance/native/scripts/run.ps1 deletes only what a run wrote: it checks the renderer is built before
touching -Output, and refuses (exit 2) an -Output that is a file, a link or junction, or a folder holding anything
but <scene>.png and manifest.json. Before, `Remove-Item $Output -Recurse -Force` ran first on whatever was passed.

The script runs from a copy of tools/conformance/native in a temporary folder, with a synthetic stub standing in for
conform_native.exe: it exits 0 at once and writes nothing, which is all these checks need. Windows only."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "conformance" / "native" / "scripts" / "run.ps1"


@unittest.skipUnless(os.name == "nt" and shutil.which("powershell.exe"), "requires Windows PowerShell")
class RunScriptTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.stub_dir = tempfile.TemporaryDirectory()
        cls.stub_exe = Path(cls.stub_dir.name) / "conform_native_stub.exe"
        env = {**os.environ, "STUB_EXE": str(cls.stub_exe)}
        env["PSModulePath"] = str(Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32/WindowsPowerShell/v1.0/Modules")
        compile_cmd = (
            "$ErrorActionPreference = 'Stop'; "
            "Add-Type -OutputAssembly $env:STUB_EXE -OutputType ConsoleApplication -TypeDefinition "
            "'public class Stub { public static int Main() { return 0; } }'"
        )
        subprocess.run(
            ["powershell.exe", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", compile_cmd],
            env=env,
            check=True,
            timeout=60,
        )

    @classmethod
    def tearDownClass(cls):
        cls.stub_dir.cleanup()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        native = self.base / "tools" / "conformance" / "native"
        (native / "scripts").mkdir(parents=True)
        shutil.copy(SCRIPT, native / "scripts" / "run.ps1")
        self.script = native / "scripts" / "run.ps1"
        self.exe = native / "build" / "Release" / "conform_native.exe"

    def build(self):
        self.exe.parent.mkdir(parents=True)
        shutil.copy(self.stub_exe, self.exe)

    def run_script(self, output):
        result = subprocess.run(
            ["powershell.exe", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
             "-File", str(self.script), "-Output", str(output)],
            capture_output=True, text=True, timeout=60)
        return result.returncode, result.stdout + result.stderr

    def folder(self, name, files):
        path = self.base / name
        path.mkdir()
        for file_name in files:
            (path / file_name).write_bytes(b"x")
        return path

    def test_unbuilt_renderer_touches_nothing(self):
        output = self.folder("work", ["notes.txt", "old.png"])
        code, text = self.run_script(output)
        self.assertEqual(code, 1, text)
        self.assertIn("build first", text)
        self.assertEqual(sorted(os.listdir(output)), ["notes.txt", "old.png"])

    def test_previous_run_output_is_emptied(self):
        self.build()
        output = self.folder("run1", ["old_scene.png", "manifest.json"])
        code, text = self.run_script(output)
        self.assertIn("EXITCODE", text)
        self.assertNotEqual(code, 2, text)
        self.assertFalse((output / "old_scene.png").exists(), text)
        self.assertFalse((output / "manifest.json").exists(), text)

    def test_folder_with_other_files_is_refused(self):
        self.build()
        for name, files in (("mixed", ["scene.png", "keep.txt"]), ("hidden", [".git"])):
            with self.subTest(name):
                output = self.folder(name, files)
                code, text = self.run_script(output)
                self.assertEqual(code, 2, text)
                self.assertIn("holds files a run does not write", text)
                self.assertEqual(sorted(os.listdir(output)), sorted(files))

    def test_subfolder_is_refused(self):
        self.build()
        output = self.folder("nested", ["scene.png"])
        (output / "sub").mkdir()
        (output / "sub" / "keep.png").write_bytes(b"x")
        code, text = self.run_script(output)
        self.assertEqual(code, 2, text)
        self.assertTrue((output / "sub" / "keep.png").exists())

    def test_file_is_refused(self):
        self.build()
        output = self.base / "report.png"
        output.write_bytes(b"x")
        code, text = self.run_script(output)
        self.assertEqual(code, 2, text)
        self.assertIn("is a file", text)
        self.assertTrue(output.exists())

    def test_junction_is_refused_and_its_target_kept(self):
        # A recursive Remove-Item on a junction can empty the folder it points to; mklink /J needs no privilege.
        self.build()
        target = self.folder("elsewhere", ["scene.png"])
        link = self.base / "link"
        made = subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(target)], capture_output=True, text=True)
        if made.returncode != 0:
            self.skipTest(f"cannot make a junction here: {made.stdout}{made.stderr}")
        code, text = self.run_script(link)
        self.assertEqual(code, 2, text)
        self.assertIn("link or junction", text)
        self.assertTrue((target / "scene.png").exists())

    def test_missing_output_folder_runs(self):
        self.build()
        code, text = self.run_script(self.base / "fresh")
        self.assertNotEqual(code, 2, text)
        self.assertIn("EXITCODE", text)


if __name__ == "__main__":
    unittest.main()
