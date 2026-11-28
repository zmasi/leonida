"""Failed replace of a read-only OBJ leaves no staged file and keeps the real error.

Synthetic archive only; no game data. Run: python3 -I tests/test_navmesh_tile_stage_cleanup.py
"""
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "tools"))
from test_navmesh_decode import build_navmesh, make_rpf  # noqa: E402
import navmesh_tile_obj as T  # noqa: E402


class StageCleanupTest(unittest.TestCase):
    def test_failed_replace_makes_staged_file_writable_before_cleanup(self):
        with tempfile.TemporaryDirectory() as d:
            archive = Path(d) / "in.rpf"
            out = Path(d) / "tile.obj"
            make_rpf(archive, [*build_navmesh(flags=3)])
            out.write_text("old report", encoding="utf-8")
            chmods = []
            real_chmod = os.chmod

            def chmod(path, mode, *args, **kwargs):
                chmods.append((str(path), mode))
                return real_chmod(path, mode, *args, **kwargs)

            with mock.patch.object(T.os, "replace", side_effect=OSError("replace denied")), \
                    mock.patch.object(T.os, "chmod", chmod):
                rc = T.main([str(archive), "--tile", "0", "--out", str(out)])
            self.assertEqual(rc, 2)
            self.assertTrue(any(mode == 0o600 for _, mode in chmods), chmods)
            self.assertEqual([n for n in os.listdir(d) if n.startswith("tmp")], [])
            self.assertEqual(out.read_text(encoding="utf-8"), "old report")


if __name__ == "__main__":
    unittest.main()
