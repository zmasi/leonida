"""Synthetic unit tests for tools/gen_bcpack_runtime_tables.py. Zero game data required.

    python -I tests/test_gen_bcpack_runtime_tables.py
"""
import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")
TOOL = os.path.join(TOOLS, "gen_bcpack_runtime_tables.py")
if TOOLS not in sys.path:
    sys.path.insert(0, TOOLS)

import gen_bcpack_runtime_tables as gen  # noqa: E402


def _synthetic_calibration() -> dict:
    eh0 = [[10, 20, 30, 40], [50, 60, 70, 80], [90, 100, 110, 120]]
    ih0 = list(range(16))
    return {
        "bc6h": {
            "endpointHistory": {"0": eh0},
            "indexHistory": {"0": ih0},
            "endpointClass": {"0": 3},
            "indexClass": {"0": 2},
        },
        "bc7": {
            "historyEndpoints": {"5": list(range(24))},
            "historyEndpointBits": {"5": [8] * 24},
            "historyEndpointConfirmedBits": {"5": [8] * 24},
            "historyIndices": {"5": [i % 16 for i in range(32)]},
            "historyIndexBits": {"5": [4] * 16 + [2] * 16},
            "historyIndexConfirmedBits": {"5": [4] * 16 + [0] * 16},
        },
    }


class GenBcpackRuntimeTablesTests(unittest.TestCase):
    def test_generate_writes_cpp_tables_with_lf_endings(self):
        with tempfile.TemporaryDirectory() as tmp:
            out_inc = os.path.join(tmp, "nested", "bcpack_tables.inc")
            p = subprocess.run(
                [sys.executable, "-I", TOOL, out_inc],
                capture_output=True, encoding="utf-8", errors="replace",
            )
            self.assertEqual(p.returncode, 0, p.stderr)
            with open(out_inc, "rb") as f:
                raw = f.read()
            self.assertIn(b"\n", raw)
            self.assertNotIn(b"\r\n", raw)
            text = raw.decode("utf-8")
            for marker in (
                "constexpr uint32_t freq[11][16][11]",
                "constexpr uint8_t bc7Config[512][4]",
                "constexpr BC6Mode bc6Modes[14]",
            ):
                self.assertIn(marker, text)

    def test_profile_roundtrip_writes_exact_bcphist1_binary(self):
        with tempfile.TemporaryDirectory() as tmp:
            cal_json = os.path.join(tmp, "cal.json")
            out_bin = os.path.join(tmp, "nested", "profile.bin")
            with open(cal_json, "w", encoding="utf-8", newline="\n") as f:
                json.dump(_synthetic_calibration(), f)
            p = subprocess.run(
                [sys.executable, "-I", TOOL, "--profile", cal_json, out_bin],
                capture_output=True, encoding="utf-8", errors="replace",
            )
            self.assertEqual(p.returncode, 0, p.stderr)
            with open(out_bin, "rb") as f:
                raw = f.read()
            expected_len = 8 + 256 * (24 + 16 + 12 + 16 + 2) + 256 * (24 * 3 + 32 * 3)
            self.assertEqual(raw[:8], b"BCPHIST1")
            self.assertEqual(len(raw), expected_len)
            self.assertEqual(
                struct.unpack_from("<12H", raw, 8),
                (10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120),
            )
            self.assertEqual(raw[8 + 256 * 70 + 5 * 168:8 + 256 * 70 + 5 * 168 + 24], bytes(range(24)))

    def test_profile_refuses_self_overwrite(self):
        with tempfile.TemporaryDirectory() as tmp:
            cal_json = os.path.join(tmp, "cal.json")
            raw_cal = json.dumps(_synthetic_calibration()) + "\n"
            with open(cal_json, "w", encoding="utf-8", newline="\n") as f:
                f.write(raw_cal)
            p = subprocess.run(
                [sys.executable, "-I", TOOL, "--profile", cal_json, cal_json],
                capture_output=True, encoding="utf-8", errors="replace",
            )
            self.assertEqual(p.returncode, 2, p.stdout + p.stderr)
            self.assertNotIn("Traceback", p.stderr)
            self.assertIn("overwrite", p.stderr)
            with open(cal_json, "r", encoding="utf-8") as f:
                self.assertEqual(f.read(), raw_cal)

    def test_rejects_combining_output_and_profile(self):
        with tempfile.TemporaryDirectory() as tmp:
            cal_json = os.path.join(tmp, "cal.json")
            out_inc = os.path.join(tmp, "out.inc")
            out_bin = os.path.join(tmp, "out.bin")
            with open(cal_json, "w", encoding="utf-8", newline="\n") as f:
                json.dump(_synthetic_calibration(), f)
            p = subprocess.run(
                [sys.executable, "-I", TOOL, out_inc, "--profile", cal_json, out_bin],
                capture_output=True, encoding="utf-8", errors="replace",
            )
            self.assertEqual(p.returncode, 2)
            self.assertFalse(os.path.exists(out_inc))
            self.assertFalse(os.path.exists(out_bin))

    def test_directory_output_and_missing_profile_exit_2_without_traceback(self):
        with tempfile.TemporaryDirectory() as tmp:
            missing = os.path.join(tmp, "missing.json")
            out_bin = os.path.join(tmp, "out.bin")
            for argv in ([tmp], ["--profile", missing, out_bin], ["--profile", missing, tmp]):
                with self.subTest(argv=argv):
                    p = subprocess.run(
                        [sys.executable, "-I", TOOL, *argv],
                        capture_output=True, encoding="utf-8", errors="replace",
                    )
                    self.assertEqual(p.returncode, 2, p.stdout + p.stderr)
                    self.assertNotIn("Traceback", p.stderr)
                    self.assertTrue(p.stderr.strip())

    def test_write_profile_failure_preserves_existing_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            cal_json = os.path.join(tmp, "cal.json")
            out_bin = os.path.join(tmp, "profile.bin")
            with open(cal_json, "w", encoding="utf-8", newline="\n") as f:
                json.dump(_synthetic_calibration(), f)
            previous = b"PREVIOUS"
            with open(out_bin, "wb") as f:
                f.write(previous)
            with mock.patch.object(gen.struct, "pack", side_effect=OSError("disk full")):
                with self.assertRaisesRegex(OSError, "disk full"):
                    gen.write_profile(cal_json, out_bin)
            with open(out_bin, "rb") as f:
                self.assertEqual(f.read(), previous)
            self.assertEqual(sorted(os.listdir(tmp)), ["cal.json", "profile.bin"])


    def test_generate_bugs_propagate_instead_of_exiting_2(self):
        with tempfile.TemporaryDirectory() as tmp:
            out_inc = os.path.join(tmp, "out.inc")
            for exc in (TypeError("internal bug"), KeyError("missing_key")):
                with self.subTest(exc=type(exc).__name__):
                    with mock.patch.object(gen, "generate", side_effect=exc):
                        with self.assertRaises(type(exc)):
                            gen.main([out_inc])
                    self.assertFalse(os.path.exists(out_inc))


if __name__ == "__main__":
    unittest.main()
