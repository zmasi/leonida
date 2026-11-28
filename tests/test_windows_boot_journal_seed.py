"""scripts/windows-boot/boot.ps1 hands the journal of `gta6rt --precompile-pipelines` to the boot as its read-only seed.

The pre-compile writes pipeline_cache.d beside gta6rt.exe; boot.ps1 points the runtime at its own cache folder, so without
LEONIDA_PIPELINE_JOURNAL_SEED_DIR a boot compiled again every pipeline the pre-compile had built. These cases run
`boot.ps1 -DryRun` (nothing is launched, no game files are needed) against a temporary runtime folder and read the
`[boot] env` lines it prints. Windows only: the script is Windows PowerShell.
"""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
BOOT = REPO / "scripts" / "windows-boot" / "boot.ps1"
POWERSHELL = shutil.which("powershell") or shutil.which("pwsh")
SEED = "LEONIDA_PIPELINE_JOURNAL_SEED_DIR"


@unittest.skipUnless(sys.platform == "win32" and POWERSHELL, "boot.ps1 is a Windows PowerShell script")
class BootJournalSeedTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="leonida_boot_seed_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.runtime = self.tmp / "port"
        self.runtime.mkdir()
        (self.runtime / "gta6rt.exe").write_bytes(b"")
        self.game = self.tmp / "game"
        self.game.mkdir()
        self.out = self.tmp / "runs"

    def env_lines(self, *pairs, shell=None, runtime=None, command_in=None):
        env = {k: v for k, v in os.environ.items() if not k.startswith("LEONIDA_") and k not in ("GTA6RT",)}
        env.update(shell or {})
        runtime = runtime or str(self.runtime / "gta6rt.exe")
        args = ["-DryRun", "-NoInput", "-Runtime", runtime, "-GameDir", str(self.game), "-OutDir", str(self.out), *pairs]
        if command_in:
            # Set-Location inside the session moves $PWD but not the process's current directory, as in an interactive shell.
            # Parameter names stay bare (quoted, '-DryRun' would be a KEY=VALUE pair); every value is quoted.
            quoted = " ".join(a if a.startswith("-") else "'" + a.replace("'", "''") + "'" for a in args)
            cmd = [POWERSHELL, "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command",
                   f"Set-Location -LiteralPath '{command_in.replace(chr(39), chr(39) * 2)}'; "
                   f"& '{str(BOOT).replace(chr(39), chr(39) * 2)}' {quoted}"]
        else:
            cmd = [POWERSHELL, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(BOOT), *args]
        # Windows PowerShell writes the console's OEM code page when its output is redirected.
        result = subprocess.run(cmd, capture_output=True, encoding="oem", errors="replace", env=env, timeout=180,
                                cwd=str(REPO))
        lines = {}
        for line in result.stdout.splitlines():
            if line.startswith("[boot] env "):
                key, _, value = line[len("[boot] env "):].strip().partition("=")
                lines[key] = value
        self.assertIn("LEONIDA_D3D12", lines, result.stdout + result.stderr)   # the script got as far as its env list
        return lines

    def test_precompiled_journal_beside_the_runtime_is_the_seed(self):
        (self.runtime / "pipeline_cache.d").mkdir()
        lines = self.env_lines()
        self.assertEqual(os.path.normcase(lines.get(SEED, "")), os.path.normcase(str(self.runtime / "pipeline_cache.d")))

    def test_no_precompiled_journal_no_seed(self):
        self.assertNotIn(SEED, self.env_lines())

    def test_a_seed_given_on_the_command_line_wins(self):
        (self.runtime / "pipeline_cache.d").mkdir()
        other = str(self.tmp / "elsewhere")
        self.assertEqual(self.env_lines(f"{SEED}={other}").get(SEED), other)

    def test_key_with_no_value_removes_the_seed(self):
        (self.runtime / "pipeline_cache.d").mkdir()
        self.assertNotIn(SEED, self.env_lines(f"{SEED}="))

    def test_a_seed_set_in_the_shell_is_kept(self):
        (self.runtime / "pipeline_cache.d").mkdir()
        other = str(self.tmp / "from-shell")
        self.assertEqual(self.env_lines(shell={SEED: other}).get(SEED), other)

    def test_no_seed_when_the_boot_cache_is_the_runtime_folder(self):
        # The runtime's writable journal would then be the seed itself, and it turns the journal off for that.
        (self.runtime / "pipeline_cache.d").mkdir()
        self.assertNotIn(SEED, self.env_lines(f"LEONIDA_PIPELINE_CACHE_DIR={self.runtime}"))

    def test_no_seed_when_the_boot_cache_sits_inside_it(self):
        (self.runtime / "pipeline_cache.d").mkdir()
        inside = self.runtime / "pipeline_cache.d" / "boot"
        self.assertNotIn(SEED, self.env_lines(f"LEONIDA_PIPELINE_CACHE_DIR={inside}"))


    def test_no_seed_when_the_cache_folder_is_empty(self):
        # The runtime then keeps its writable journal beside gta6rt.exe: that is the precompiled folder itself.
        (self.runtime / "pipeline_cache.d").mkdir()
        self.assertNotIn(SEED, self.env_lines("LEONIDA_PIPELINE_CACHE_DIR="))

    def test_no_seed_when_the_runtime_sits_under_the_boot_journal(self):
        cache = self.tmp / "cache"
        runtime = cache / "pipeline_cache.d" / "port"
        (runtime / "pipeline_cache.d").mkdir(parents=True)
        (runtime / "gta6rt.exe").write_bytes(b"")
        self.assertNotIn(SEED, self.env_lines(f"LEONIDA_PIPELINE_CACHE_DIR={cache}", runtime=str(runtime / "gta6rt.exe")))

    def test_paths_compare_without_case(self):
        (self.runtime / "pipeline_cache.d").mkdir()
        self.assertNotIn(SEED, self.env_lines(f"LEONIDA_PIPELINE_CACHE_DIR={str(self.runtime).upper()}"))

    def test_a_sibling_folder_is_not_an_overlap(self):
        (self.runtime / "pipeline_cache.d").mkdir()
        sibling = self.runtime / "pipeline_cache.d-x"
        lines = self.env_lines(f"LEONIDA_PIPELINE_CACHE_DIR={sibling}")
        self.assertEqual(os.path.normcase(lines.get(SEED, "")), os.path.normcase(str(self.runtime / "pipeline_cache.d")))

    def test_no_seed_for_a_relative_cache_folder(self):
        # The runtime resolves it against its own working folder, which this script cannot check.
        (self.runtime / "pipeline_cache.d").mkdir()
        self.assertNotIn(SEED, self.env_lines("LEONIDA_PIPELINE_CACHE_DIR=relative\\cache"))

    def test_a_long_path_prefix_names_the_same_folder(self):
        (self.runtime / "pipeline_cache.d").mkdir()
        self.assertNotIn(SEED, self.env_lines(f"LEONIDA_PIPELINE_CACHE_DIR=\\\\?\\{self.runtime}"))

    def test_a_device_path_prefix_names_the_same_folder(self):
        (self.runtime / "pipeline_cache.d").mkdir()
        self.assertNotIn(SEED, self.env_lines(f"LEONIDA_PIPELINE_CACHE_DIR=\\\\.\\{self.runtime}"))

    def test_a_junction_to_the_runtime_folder_is_recognised(self):
        (self.runtime / "pipeline_cache.d").mkdir()
        alias = self.tmp / "port-alias"
        made = subprocess.run(["cmd", "/c", "mklink", "/J", str(alias), str(self.runtime)], capture_output=True)
        if made.returncode != 0 or not alias.is_dir():
            self.skipTest("could not make a junction here")
        self.addCleanup(lambda: os.path.isdir(alias) and os.rmdir(alias))   # removes the junction, not its target
        self.assertNotIn(SEED, self.env_lines(f"LEONIDA_PIPELINE_CACHE_DIR={alias}"))

    def test_an_unusable_cache_folder_name_does_not_stop_the_script(self):
        (self.runtime / "pipeline_cache.d").mkdir()
        self.assertNotIn(SEED, self.env_lines("LEONIDA_PIPELINE_CACHE_DIR=C:\\a|b"))

    def test_a_relative_runtime_is_resolved_from_the_shell_location(self):
        (self.runtime / "pipeline_cache.d").mkdir()
        lines = self.env_lines(runtime="port\\gta6rt.exe", command_in=str(self.tmp))
        self.assertEqual(os.path.normcase(lines.get(SEED, "")), os.path.normcase(str(self.runtime / "pipeline_cache.d")))


if __name__ == "__main__":
    unittest.main()
