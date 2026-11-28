"""make_game.py's pipelines stage logs, after each batch, the shaders checked, the share of the estimated compile time
and the time left, weighting each module by its SPIR-V size (MB ** PIPELINE_COST_EXPONENT). No GPU or game files.

    python -I tests/test_make_game_pipeline_eta.py
"""
import contextlib
import io
import json
import os
import sys
import tempfile
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)
import make_game  # noqa: E402

MB = 1_000_000


class PipelineCostsTest(unittest.TestCase):
    def test_cost_is_the_size_in_mb_to_the_exponent_and_zero_without_a_file(self):
        with tempfile.TemporaryDirectory() as spirv:
            for name, size in (("one", MB), ("four", 4 * MB)):
                with open(os.path.join(spirv, name + ".spv"), "wb") as f:
                    f.truncate(size)
            with mock.patch.object(make_game, "PIPELINE_COST_EXPONENT", 1.5):
                self.assertEqual(make_game.pipeline_costs(spirv, ["one", "four", "missing"]), [1.0, 8.0, 0.0])

    def test_a_larger_module_weighs_more_than_its_bytes(self):
        self.assertGreater(make_game.PIPELINE_COST_EXPONENT, 1)


class PipelinesProgressLineTest(unittest.TestCase):
    def test_share_and_time_left(self):
        self.assertEqual(make_game.pipelines_progress(500, 2000, 0.25, 60),
                         "pipelines: 500/2000 shaders checked, 25% of the estimated compile time, ~3m left")

    def test_the_last_batch_gives_the_elapsed_time(self):
        self.assertEqual(make_game.pipelines_progress(2000, 2000, 1.0, 3725),
                         "pipelines: 2000/2000 shaders checked, 1h 02m elapsed")

    def test_no_estimate_before_any_cost_is_done(self):
        self.assertEqual(make_game.pipelines_progress(3, 9, 0.0, 5),
                         "pipelines: 3/9 shaders checked, 0% of the estimated compile time")


class PipelinesStageTest(unittest.TestCase):
    """The real pipelines() with a checker that always succeeds and a clock that advances per batch."""

    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix="pipeline eta ")
        self.addCleanup(tmp.cleanup)
        self.work = os.path.join(tmp.name, "work")
        self.out = os.path.join(tmp.name, "out")
        self.spirv = os.path.join(self.work, "shaders", "spirv")
        os.makedirs(self.spirv)
        # Batches of 2 (name-sorted): a+b = 1 + 1, c+d = 8 + 0 (d has no .spv), e = 8; total cost 18.
        sizes = {"a": 1, "b": 1, "c": 4, "e": 4}
        for name, mb in sizes.items():
            with open(os.path.join(self.spirv, name + ".spv"), "wb") as f:
                f.truncate(mb * MB)
        with open(os.path.join(self.spirv, "index.json"), "w", encoding="utf-8", newline="\n") as f:
            json.dump({n: {} for n in ("e", "d", "c", "b", "a")}, f)

    def invoke(self, results):
        logs = []
        clock = iter([0.0, 20.0, 110.0, 200.0])   # stage start, then after each batch
        with mock.patch.object(make_game, "pipeline_check_exe", return_value="checker"), \
                mock.patch.object(make_game, "checker_supports", return_value=True), \
                mock.patch.object(make_game, "stale_checker", return_value=[]), \
                mock.patch.object(make_game, "PIPELINE_BATCH_SIZE", 2), \
                mock.patch.object(make_game, "PIPELINE_COST_EXPONENT", 1.5), \
                mock.patch.object(make_game, "run", side_effect=results), \
                mock.patch.object(make_game.time, "monotonic", side_effect=lambda: next(clock)), \
                mock.patch.object(make_game, "log", side_effect=logs.append), \
                contextlib.redirect_stderr(io.StringIO()):   # the stopped stage's ERROR line
            try:
                make_game.pipelines(self.work, self.out, jobs=2)
            except SystemExit:
                pass
        return [line for line in logs if "shaders checked" in line]

    def test_each_batch_logs_the_share_of_the_cost_and_the_time_left(self):
        self.assertEqual(self.invoke([0, 0, 0]), [
            "pipelines: 2/5 shaders checked, 11% of the estimated compile time, ~2m 40s left",   # 2 of 18 in 20 s
            "pipelines: 4/5 shaders checked, 55% of the estimated compile time, ~1m 28s left",   # 10 of 18 in 110 s
            "pipelines: 5/5 shaders checked, 3m 20s elapsed",
        ])

    def test_a_stopped_batch_logs_no_progress_for_itself(self):
        # 2: the checker could not set up; the stage stops after the first batch's line.
        self.assertEqual(self.invoke([0, 2, 0]),
                         ["pipelines: 2/5 shaders checked, 11% of the estimated compile time, ~2m 40s left"])


if __name__ == "__main__":
    unittest.main()
