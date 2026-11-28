"""make_game.py's runtime stage stages its files before replacing the old install (RT-249).

A copy that fails part way (disk full, a file locked by a running gta6rt.exe) used to leave the output folder half
replaced: a new gta6rt.exe next to an old gta6rt_tls_host.dll, or a new runtime with the old game_path.txt. Now every
file is written to a staging folder inside --out first and only then moved into place. Synthetic folders only; no
game files.

    python -I tests/test_make_game_runtime_staging.py
"""
import os
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)
import make_game


# gta6rt.sh's text, as runtime() passes it from wine_launcher(); Windows writes no launcher
LAUNCHER = "#!/bin/sh\n# test launcher\n" if os.name != "nt" else None


def install(game, out, artifacts, work=None):
    return make_game.install_runtime(game, out, artifacts, work, LAUNCHER)


def read(path):
    with open(path, "rb") as f:
        return f.read()


class RuntimeStagingTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.build = os.path.join(self.tmp, "build")
        self.game = os.path.join(self.tmp, "game")
        self.out = os.path.join(self.tmp, "out")
        for d in (self.build, self.game, self.out):
            os.makedirs(d)
        self.artifacts = []
        for name in make_game.RUNTIME_FILES:
            path = os.path.join(self.build, name)
            with open(path, "wb") as f:
                f.write(b"new " + name.encode())
            self.artifacts.append(path)
        # the previous install
        self.old = {name: b"old " + name.encode() for name in (*make_game.RUNTIME_FILES, "game_path.txt", "gta6rt.sh")}
        for name, data in self.old.items():
            with open(os.path.join(self.out, name), "wb") as f:
                f.write(data)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def entries(self):
        return sorted(os.listdir(self.out))

    def test_success_replaces_everything_and_leaves_no_staging(self):
        install(self.game, self.out, self.artifacts)
        for name in make_game.RUNTIME_FILES:
            self.assertEqual(read(os.path.join(self.out, name)), b"new " + name.encode())
        self.assertEqual(read(os.path.join(self.out, "game_path.txt")).decode(), os.path.abspath(self.game))
        self.assertEqual(self.entries(), sorted(self.old))
        if os.name != "nt":
            self.assertEqual(read(os.path.join(self.out, "gta6rt.sh")).decode(), LAUNCHER)
            self.assertTrue(os.access(os.path.join(self.out, "gta6rt.sh"), os.X_OK))

    def test_failed_copy_leaves_the_old_install(self):
        real = shutil.copy2
        calls = []

        def copy2(src, dst, *a, **k):
            calls.append(src)
            if len(calls) == 2:  # the second runtime file: the first is already copied (into the staging folder)
                raise OSError(28, "No space left on device")
            return real(src, dst, *a, **k)

        with patch.object(make_game.shutil, "copy2", copy2):
            with self.assertRaises(OSError):
                install(self.game, self.out, self.artifacts)
        self.assertEqual(len(calls), 2)
        for name, data in self.old.items():
            self.assertEqual(read(os.path.join(self.out, name)), data, name)
        self.assertEqual(self.entries(), sorted(self.old))  # no staging folder left behind

    def test_failed_game_path_write_leaves_the_old_install(self):
        real = open

        def failing_open(path, mode="r", *a, **k):
            if os.path.basename(str(path)) == "game_path.txt" and "w" in mode:
                raise OSError(5, "Input/output error")
            return real(path, mode, *a, **k)

        with patch("builtins.open", failing_open):
            with self.assertRaises(OSError):
                install(self.game, self.out, self.artifacts)
        for name, data in self.old.items():
            self.assertEqual(read(os.path.join(self.out, name)), data, name)
        self.assertEqual(self.entries(), sorted(self.old))

    def failing_replace(self, fail_on):
        real = os.replace
        calls = []

        def replace(src, dst):
            calls.append((os.path.basename(src), os.path.basename(dst)))
            if len(calls) == fail_on:
                raise PermissionError(13, "Access is denied", dst)
            return real(src, dst)
        return replace, calls

    def test_failed_publish_restores_the_old_install(self):
        # @shdann and @Flyander on #5278: a replace that fails after the first file was published (on Windows, the
        # destination DLL held open by a reader) left the new exe next to the old DLL. Fail at every step in turn.
        names = [*make_game.RUNTIME_FILES, "game_path.txt"] + (["gta6rt.sh"] if os.name != "nt" else [])
        for fail_on in range(1, 2 * len(names) + 1):
            with self.subTest(fail_on=fail_on):
                self.tearDown(); self.setUp()
                replace, calls = self.failing_replace(fail_on)
                with patch.object(make_game.os, "replace", replace):
                    with self.assertRaises(PermissionError):
                        install(self.game, self.out, self.artifacts)
                for name, data in self.old.items():
                    self.assertEqual(read(os.path.join(self.out, name)), data, name)
                self.assertEqual(self.entries(), sorted(self.old))

    def test_failed_publish_removes_files_that_had_no_old_copy(self):
        first, second = make_game.RUNTIME_FILES[:2]
        os.remove(os.path.join(self.out, first))   # a fresh exe: published with one replace, nothing moved aside
        before = self.entries()
        replace, _ = self.failing_replace(2)          # moving the old DLL aside fails, after the exe is published
        with patch.object(make_game.os, "replace", replace):
            with self.assertRaises(PermissionError):
                install(self.game, self.out, self.artifacts)
        self.assertEqual(self.entries(), before)      # the new exe is taken back out
        for name in (second, "game_path.txt"):
            self.assertEqual(read(os.path.join(self.out, name)), self.old[name])

    def test_blocked_rollback_keeps_the_old_files_and_says_where(self):
        # @Desamodeth on #5278: a second reader opens the new exe after it was published, so moving the old exe back
        # fails too. The old copy must not be deleted with the staging folder, and the error must name the folder.
        real = os.replace
        calls = []

        def replace(src, dst):
            calls.append(os.path.basename(dst))
            if len(calls) == 3:                     # moving the old DLL aside (the DLL is held open)
                raise PermissionError(13, "Access is denied", dst)
            if len(calls) == 4:                     # putting the old exe back (the new one is now held open)
                raise PermissionError(13, "Access is denied", dst)
            return real(src, dst)

        exe, dll = make_game.RUNTIME_FILES[:2]
        with patch.object(make_game.os, "replace", replace):
            with self.assertRaises(make_game.IncompleteRollback) as raised:
                install(self.game, self.out, self.artifacts)
        e = raised.exception
        self.assertEqual([name for name, _ in e.failures], [exe])
        self.assertIn(e.stage, str(e))
        self.assertEqual(read(os.path.join(e.stage, exe + ".old")), self.old[exe])   # kept for recovery
        self.assertEqual(read(os.path.join(self.out, dll)), self.old[dll])
        self.assertEqual(read(os.path.join(self.out, "game_path.txt")), self.old["game_path.txt"])

    def test_failed_restore_of_the_file_being_published_keeps_its_old_copy(self):
        # Moving the new exe in fails and putting the old exe back fails too: the old exe then exists only in the
        # staging folder, which must be kept (IncompleteRollback), not removed with the failed install.
        real = os.replace
        exe = make_game.RUNTIME_FILES[0]

        def replace(src, dst):
            if os.path.basename(dst) == exe and not src.endswith(".old"):
                raise PermissionError(13, "Access is denied", dst)       # the new exe cannot take its place
            if os.path.basename(dst) == exe:
                raise PermissionError(13, "Access is denied", dst)       # nor can the old one go back
            return real(src, dst)

        with patch.object(make_game.os, "replace", replace):
            with self.assertRaises(make_game.IncompleteRollback) as raised:
                install(self.game, self.out, self.artifacts)
        e = raised.exception
        self.assertEqual([name for name, _ in e.failures], [exe])
        self.assertEqual(read(os.path.join(e.stage, exe + ".old")), self.old[exe])

    def test_stale_staging_folders_are_removed_by_the_next_install(self):
        stale = os.path.join(self.out, ".runtime-staging-left-by-a-running-exe")
        os.makedirs(stale)
        with open(os.path.join(stale, "gta6rt.exe.old"), "wb") as f:
            f.write(b"old")
        install(self.game, self.out, self.artifacts)
        self.assertFalse(os.path.exists(stale))

    def test_bcpack_history_is_built_where_the_runtime_reads_it(self):
        # runtime/shims/dstorage.cpp reads <work>/shaders/bcpack/history.bcph (shaders.parent_path() / "bcpack")
        import ttpf_mips
        work = os.path.join(self.tmp, "work")
        calls = []
        with patch.object(ttpf_mips, "find_pools", lambda game: [object()]), \
                patch.object(make_game, "py", lambda *args: calls.append(args)), \
                patch.object(make_game, "wine_launcher", lambda *args: LAUNCHER), \
                patch.object(make_game, "log", lambda *args, **kw: None):
            make_game.runtime(self.game, self.out, {"expected_exe": "game.exe"}, self.build, work=work)
        self.assertEqual(len(calls), 1)
        out = calls[0][calls[0].index("--out") + 1]
        self.assertEqual(out, os.path.join(os.path.abspath(work), "shaders", "bcpack", "history.bcph"))

    def test_runtime_build_in_the_output_folder_is_left_in_place(self):
        # --runtime-build pointing at --out: the runtime files are already there and are not copied onto themselves
        artifacts = [os.path.join(self.out, name) for name in make_game.RUNTIME_FILES]
        with patch.object(make_game.shutil, "copy2", side_effect=AssertionError("copied onto itself")):
            install(self.game, self.out, artifacts)
        for name in make_game.RUNTIME_FILES:
            self.assertEqual(read(os.path.join(self.out, name)), self.old[name])
        self.assertEqual(read(os.path.join(self.out, "game_path.txt")).decode(), os.path.abspath(self.game))
        self.assertEqual(self.entries(), sorted(self.old))

    def test_shader_path_is_staged_with_the_rest(self):
        # main's shader_path.txt (from --work) goes through the same staging: a failed copy leaves no new one
        work = os.path.join(self.tmp, "work")
        install(self.game, self.out, self.artifacts, work)
        self.assertEqual(read(os.path.join(self.out, "shader_path.txt")),
                         os.fsencode(os.path.join(os.path.abspath(work), "shaders", "spirv"))
                         if os.name != "nt" else os.path.join(os.path.abspath(work), "shaders", "spirv").encode())
        os.remove(os.path.join(self.out, "shader_path.txt"))
        with patch.object(make_game.shutil, "copy2", side_effect=OSError(28, "No space left on device")):
            with self.assertRaises(OSError):
                install(self.game, self.out, self.artifacts, work)
        self.assertNotIn("shader_path.txt", os.listdir(self.out))

    def test_missing_output_folder_is_created(self):
        out = os.path.join(self.tmp, "fresh", "out")
        install(self.game, out, self.artifacts)
        names = sorted(os.listdir(out))
        want = sorted([*make_game.RUNTIME_FILES, "game_path.txt"] + (["gta6rt.sh"] if os.name != "nt" else []))
        self.assertEqual(names, want)


if __name__ == "__main__":
    unittest.main()
