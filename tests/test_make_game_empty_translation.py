"""make_game.py: an empty translated shader index is not kept, and a later stage refuses one.

The synthetic two-shard integration test uses SPIRV-Tools when installed. All other cases mock translation.

    python -I tests/test_make_game_empty_translation.py
"""
import io
import json
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
import make_game  # noqa: E402


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


class Done:
    def poll(self):
        return 0

    def wait(self):
        return 0

    def terminate(self):
        pass

    def kill(self):
        pass


class TranslateEmptyIndexTest(unittest.TestCase):
    """translate(): a merge that published nothing is an error and must not leave a marker behind."""

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.disasm = os.path.join(tmp.name, "dxbc", "disasm")
        self.spirv = os.path.join(tmp.name, "spirv")
        os.makedirs(self.disasm)

    def translate_with_index(self, text):
        def fake_py(tool, *args):
            if "--merge" in args:
                write(os.path.join(self.spirv, "index.json"), text)

        with mock.patch.object(make_game, "log", lambda *a: None), \
                mock.patch.object(make_game, "py", side_effect=fake_py), \
                mock.patch.object(make_game.subprocess, "Popen", lambda *a, **k: Done()):
            make_game.translate(self.disasm, self.spirv, 1)

    def test_an_empty_index_is_removed_with_the_plan_and_the_retry_fails(self):
        for _ in range(2):
            with self.assertRaises(SystemExit) as cm:
                self.translate_with_index("{}")
            self.assertEqual(cm.exception.code, 1)
            self.assertFalse(os.path.exists(os.path.join(self.spirv, "index.json")))
            self.assertFalse(os.path.exists(os.path.join(self.spirv, make_game.SHARD_PLAN)))

    def test_an_unreadable_index_is_removed(self):
        with self.assertRaises(SystemExit) as cm:
            self.translate_with_index("not json")
        self.assertEqual(cm.exception.code, 1)
        self.assertFalse(os.path.exists(os.path.join(self.spirv, "index.json")))

    def test_merge_cannot_publish_a_non_object(self):
        with self.assertRaises(SystemExit):
            self.translate_with_index('["shader"]')
        self.assertFalse(os.path.exists(os.path.join(self.spirv, "index.json")))

    def test_a_non_empty_index_is_kept(self):
        self.translate_with_index(json.dumps({"shader_0": {"stage": "ps"}}))
        with open(os.path.join(self.spirv, "index.json"), encoding="utf-8") as f:
            self.assertEqual(json.load(f), {"shader_0": {"stage": "ps"}})

    def test_a_failed_shard_removes_a_stale_index_and_skips_merge(self):
        index = os.path.join(self.spirv, "index.json")
        write(index, json.dumps({"old": {}}))
        states = iter([None, 1])

        class FailingShard(Done):
            def poll(self):
                return next(states, 1)

            def wait(self):
                return 1

        err = io.StringIO()
        merge_py = mock.Mock()
        with mock.patch.object(make_game, "log", lambda *a: None), \
                mock.patch.object(make_game, "py", merge_py), \
                mock.patch.object(make_game.time, "sleep", lambda *a: None), \
                mock.patch.object(make_game.subprocess, "Popen", lambda *a, **k: FailingShard()), \
                mock.patch("sys.stderr", err), \
                self.assertRaises(SystemExit) as cm:
            make_game.translate(self.disasm, self.spirv, 1)
        self.assertEqual(cm.exception.code, 1)
        self.assertIn("rdna2spirv.py: shard 0 exited with 1.", err.getvalue())
        self.assertFalse(os.path.exists(index))
        merge_py.assert_not_called()


class TranslateLockedIndexTest(unittest.TestCase):
    """translate(): an index that cannot be read or removed (on Windows, a file an antivirus scan or the indexer
    holds) ends the stage with its own message naming the file once, not with a PermissionError traceback."""

    setUp = TranslateEmptyIndexTest.setUp
    translate_with_index = TranslateEmptyIndexTest.translate_with_index

    def locked(self, path):
        return PermissionError(13, "Permission denied", path)

    def failure(self, text, open_locked=False, shard_code=0):
        index = os.path.join(self.spirv, "index.json")
        real_remove, real_open = os.remove, open

        def remove(path, *a, **k):
            if os.path.abspath(path) == os.path.abspath(index):
                raise self.locked(path)
            return real_remove(path, *a, **k)

        def opener(path, mode="r", *a, **k):
            if open_locked and mode == "r" and os.path.abspath(path) == os.path.abspath(index):
                raise self.locked(path)
            return real_open(path, mode, *a, **k)

        class Exited(Done):
            def poll(self):
                return shard_code

            def wait(self):
                return shard_code

        err = io.StringIO()
        merge_py = mock.Mock()
        with mock.patch.object(make_game.os, "remove", remove), \
                mock.patch("builtins.open", opener), \
                mock.patch.object(make_game.subprocess, "Popen", lambda *a, **k: Exited()), \
                mock.patch("sys.stderr", err), \
                self.assertRaises(SystemExit) as cm:
            if shard_code:
                write(index, text)
                with mock.patch.object(make_game, "log", lambda *a: None), \
                        mock.patch.object(make_game, "py", merge_py):
                    make_game.translate(self.disasm, self.spirv, 1)
            else:
                self.translate_with_index(text)
        if shard_code:
            merge_py.assert_not_called()
        self.assertEqual(cm.exception.code, 1)
        message = err.getvalue()
        self.assertIn(f"{index} could not be removed (Permission denied): delete it before running", message)
        self.assertNotIn(repr(index), message)
        return message

    def test_an_unopenable_index_that_cannot_be_removed(self):
        message = self.failure('{"shader_0": {}}', open_locked=True)
        index = os.path.join(self.spirv, "index.json")
        self.assertIn(f"--merge wrote an unreadable {index}: Permission denied.", message)

    def test_an_unparsable_index_that_cannot_be_removed(self):
        self.assertIn("--merge wrote an unreadable", self.failure("not json"))

    def test_an_empty_index_that_cannot_be_removed(self):
        self.assertIn("translated 0 shaders", self.failure("{}"))

    def test_a_stale_index_after_a_failed_shard_that_cannot_be_removed(self):
        self.assertIn("rdna2spirv.py: shard 0 exited with 1.", self.failure('{"old": {}}', shard_code=1))


class RequireTranslatedShadersTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.work = tmp.name
        self.spirv = os.path.join(self.work, "shaders", "spirv")
        os.makedirs(self.spirv)

    def call(self):
        err = io.StringIO()
        with mock.patch("sys.stderr", err):
            return make_game.require_translated_shaders(self.work), err.getvalue()

    def test_missing_index_names_the_stage(self):
        with self.assertRaises(SystemExit) as cm:
            self.call()
        self.assertEqual(cm.exception.code, 1)

    def test_an_empty_index_is_refused(self):
        write(os.path.join(self.spirv, "index.json"), "{}")
        with self.assertRaises(SystemExit) as cm:
            self.call()
        self.assertEqual(cm.exception.code, 1)

    def test_an_unreadable_index_is_refused(self):
        write(os.path.join(self.spirv, "index.json"), "not json")
        with self.assertRaises(SystemExit) as cm:
            self.call()
        self.assertEqual(cm.exception.code, 1)

    def test_a_non_empty_index_is_accepted(self):
        write(os.path.join(self.spirv, "index.json"), '{"shader_0": {}}')
        self.assertEqual(self.call()[0], self.spirv)

    def test_a_list_is_not_a_translation_index(self):
        write(os.path.join(self.spirv, "index.json"), '["shader"]')
        with self.assertRaises(SystemExit):
            self.call()


class ShadersStageTest(unittest.TestCase):
    """shaders(): a saved `{}` from a merge that produced nothing is translated again, and its plan goes with it."""

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.work = tmp.name
        self.dxbc = os.path.join(self.work, "shaders", "dxbc")
        self.spirv = os.path.join(self.work, "shaders", "spirv")

    def index(self):
        return os.path.join(self.spirv, "index.json")

    def plan(self):
        return os.path.join(self.spirv, make_game.SHARD_PLAN)

    def run_stage(self):
        ran = []

        def fake_py(tool, *args):
            if tool == "shader_carve.py":
                write(os.path.join(self.dxbc, "index.json"), json.dumps([{"file": "shader.dxbc"}]))
            elif tool == "fssm_unpack.py":
                write(os.path.join(self.dxbc, "fssm_index.json"), json.dumps([{"name": "shader"}]))
            elif tool == "shader_disasm.py":
                write(os.path.join(self.dxbc, "disasm", "shader.s"), "synthetic listing\n")

        def fake_translate(disasm, spirv, jobs, emulate=False, fallbacks=()):
            ran.append("translate")
            write(os.path.join(self.spirv, "index.json"), json.dumps({"shader_0": {"stage": "ps"}}))

        with mock.patch.object(make_game, "py", fake_py), \
                mock.patch.object(make_game, "need", lambda *a: None), \
                mock.patch.object(make_game, "translate", fake_translate), \
                mock.patch.object(make_game, "llvm_mc_signature", return_value="test llvm-mc 21"), \
                mock.patch.object(make_game, "log", lambda *a: None):
            make_game.shaders(os.path.join(self.work, "game"), self.work, 2)
        return ran

    def test_an_empty_saved_index_is_translated_again_without_its_plan(self):
        self.assertEqual(self.run_stage(), ["translate"])
        write(self.index(), "{}")
        write(self.plan(), "{}")
        self.assertEqual(self.run_stage(), ["translate"])
        self.assertFalse(os.path.exists(self.plan()))

    def test_a_saved_index_that_names_a_shader_is_kept(self):
        self.run_stage()
        self.assertEqual(self.run_stage(), [])
        with open(self.index(), encoding="utf-8") as f:
            self.assertEqual(json.load(f), {"shader_0": {"stage": "ps"}})

    def test_damaged_index_is_regenerated_and_its_shards_can_be_reused(self):
        self.run_stage()
        write(self.plan(), '{"jobs":2}')
        for damaged in ('{"shader_0":', '["shader_0"]', 'null', b'\xff'):
            with self.subTest(damaged=damaged):
                with open(self.index(), "wb") as f:
                    f.write(damaged if isinstance(damaged, bytes) else damaged.encode())
                with mock.patch.object(make_game, "llvm_mc_signature", return_value="test llvm-mc 21"):
                    self.assertFalse(make_game.shader_outputs_reusable(self.work, False, False))
                self.assertEqual(self.run_stage(), ["translate"])
                self.assertTrue(os.path.isfile(self.plan()))
                self.assertEqual(self.run_stage(), [])

    def test_utf8_bom_index_remains_reusable(self):
        self.run_stage()
        write(self.index(), '\ufeff{"shader_0":{"stage":"ps"}}')
        self.assertEqual(self.run_stage(), [])
        with mock.patch.object(make_game, "llvm_mc_signature", return_value="test llvm-mc 21"):
            self.assertTrue(make_game.shader_outputs_reusable(self.work, False, False))

    def test_temporarily_locked_index_survives_the_retry(self):
        self.run_stage()
        saved = Path(self.index()).read_bytes()
        with mock.patch.object(make_game, "read_translation_index", side_effect=PermissionError(13, "locked")), \
                mock.patch("sys.stderr", new_callable=io.StringIO) as err, self.assertRaises(SystemExit):
            self.run_stage()
        self.assertIn("locked", err.getvalue())
        self.assertEqual(Path(self.index()).read_bytes(), saved)
        self.assertEqual(self.run_stage(), [])

    @unittest.skipUnless(shutil.which("spirv-as") and shutil.which("spirv-val"), "needs SPIRV-Tools")
    def test_real_shards_restore_the_index_without_rebuilding_modules(self):
        dxbc, spirv = Path(self.dxbc), Path(self.spirv)
        names = ("ps_first", "ps_second")
        (dxbc / "disasm").mkdir(parents=True)
        for name in names:
            (dxbc / "disasm" / (name + ".s")).write_text("\ts_endpgm\n", encoding="utf-8")
            (dxbc / (name + ".dxbc")).write_bytes(b"DXBC" + bytes(28))
        write(dxbc / "index.json", json.dumps([{"file": n + ".dxbc", "stage": "ps"} for n in names]))
        write(dxbc / "fssm_index.json", json.dumps([{"name": n} for n in names]))
        write(dxbc / "disasm.done", "synthetic llvm-mc")
        with mock.patch.object(make_game, "llvm_mc_signature", return_value="synthetic llvm-mc"):
            make_game.shaders(self.work, self.work, jobs=2)
            index = spirv / "index.json"
            original = index.read_bytes()
            self.assertEqual(set(json.loads(original)), set(names))
            products = [spirv / (n + ".spv") for n in names] + sorted(spirv.glob("index.part*.json"))
            self.assertEqual(len(products), 4)

            def snapshot():
                return [(p.stat().st_mtime_ns, p.read_bytes()) for p in products]

            before = snapshot()
            index.write_bytes(original[:len(original) // 2])
            self.assertTrue(make_game.shader_outputs_reusable(self.work, False, False, jobs=2))
            self.assertFalse(make_game.shader_outputs_reusable(self.work, False, False, jobs=1))
            make_game.shaders(self.work, self.work, jobs=2)
            self.assertEqual(index.read_bytes(), original)
            self.assertEqual(snapshot(), before)
            self.assertEqual(make_game.require_translated_shaders(self.work), self.spirv)


if __name__ == "__main__":
    unittest.main()
