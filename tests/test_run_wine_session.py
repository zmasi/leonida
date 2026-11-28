"""Check that scripts/run_wine_session.sh initializes and stops the prefix with the Wine named by WINE."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "run_wine_session.sh"


def fake(path: Path, name: str, log: Path) -> None:
    path.mkdir(parents=True, exist_ok=True)
    tool = path / name
    tool.write_text(f'#!/bin/sh\necho "{tool} $*" >> "{log}"\n', encoding="utf-8", newline="\n")
    tool.chmod(0o755)


@unittest.skipUnless(shutil.which("bash") and shutil.which("timeout") and os.name == "posix", "needs bash")
class WineSessionTests(unittest.TestCase):
    def run_session(self, env: dict[str, str], path: Path) -> subprocess.CompletedProcess:
        base = {"PATH": f"{path}:/usr/bin:/bin", "WINEPREFIX": str(path.parent / "prefix")}
        return subprocess.run(["bash", str(SCRIPT), "true"], env={**base, **env},
                              capture_output=True, text=True, timeout=60)

    def test_uses_wine_and_its_wineserver(self):
        with tempfile.TemporaryDirectory() as d:
            root, log = Path(d), Path(d) / "log"
            system, proton = root / "system bin", root / "Proton 11.0" / "files" / "bin"
            for name in ("wine", "wineboot", "wineserver"):
                fake(system, name, log)
            fake(proton, "wine", log)
            fake(proton, "wineserver", log)
            r = self.run_session({"WINE": str(proton / "wine")}, system)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(log.read_text().splitlines(),
                             [f"{proton / 'wine'} wineboot --init", f"{proton / 'wineserver'} -k"])

    def test_default_wine_from_path(self):
        with tempfile.TemporaryDirectory() as d:
            root, log = Path(d), Path(d) / "log"
            system = root / "bin"
            fake(system, "wine", log)
            fake(system, "wineserver", log)
            r = self.run_session({}, system)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(log.read_text().splitlines(),
                             [f"{system / 'wine'} wineboot --init", f"{system / 'wineserver'} -k"])

    def test_wineserver_override_and_path_fallback(self):
        with tempfile.TemporaryDirectory() as d:
            root, log = Path(d), Path(d) / "log"
            system, other = root / "bin", root / "other"
            fake(system, "wineserver", log)
            fake(other, "wine", log)  # no wineserver next to it
            r = self.run_session({"WINE": str(other / "wine")}, system)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(log.read_text().splitlines()[-1], f"{system / 'wineserver'} -k")
            fake(other, "my-wineserver", log)
            r = self.run_session({"WINE": str(other / "wine"), "WINESERVER": str(other / "my-wineserver")}, system)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(log.read_text().splitlines()[-1], f"{other / 'my-wineserver'} -k")

    def test_missing_wine_is_reported(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "bin").mkdir()
            r = self.run_session({"WINE": str(root / "nowhere" / "wine")}, root / "bin")
            self.assertEqual(r.returncode, 127)
            self.assertIn("Wine not found", r.stderr)


if __name__ == "__main__":
    unittest.main()
