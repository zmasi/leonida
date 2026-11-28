"""Unit tests for tools/shader_carve_accel.py buffer safety and fallback guards."""

import ctypes
import os
import re
import sys
import unittest
from pathlib import Path
from unittest import mock

TOOLS_DIR = Path(__file__).resolve().parents[1] / "tools"
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import shader_carve_accel as accel  # noqa: E402


class ShaderCarveAccelSafetyTests(unittest.TestCase):
    PATTERN = re.compile(rb"DXBC\w{4}")

    def test_load_native_returns_none_without_cdll_when_missing(self):
        fake_ctypes = mock.Mock()
        with mock.patch.object(Path, "is_file", return_value=False):
            got = accel._load_native(fake_ctypes)
        self.assertIsNone(got)
        fake_ctypes.CDLL.assert_not_called()

    def test_disable_env_variants_skip_native_loader(self):
        data = b"zzDXBCabcd"
        for value in ("0", "false", "off", "no", " FALSE ", " Off "):
            with (
                self.subTest(env=value),
                mock.patch.dict(os.environ, {"LEONIDA_SHADER_CARVE_NATIVE": value}),
                mock.patch.object(accel, "_load_native") as loader,
                accel.entry_search(data, self.PATTERN) as search,
            ):
                loader.assert_not_called()
                m = search(0)
                self.assertIsNotNone(m)
                self.assertEqual(m.group(0), b"DXBCabcd")

    @unittest.skipUnless(sys.implementation.name == "cpython" and sys.version_info >= (3, 11),
                         "requires CPython Py_buffer ABI")
    def test_writable_and_empty_buffers_release_pin_immediately(self):
        scan = mock.Mock()
        buf = bytearray(b"xxDXBC1234yy")
        with (
            mock.patch.dict(os.environ, {"LEONIDA_SHADER_CARVE_NATIVE": "1"}),
            mock.patch.object(accel, "_load_native", return_value=scan),
            accel.entry_search(buf, self.PATTERN) as search,
        ):
            # A writable buffer must not remain pinned during the context body.
            buf.append(0x21)
            m = search(0)
            self.assertIsNotNone(m)
            self.assertEqual(m.span(), (2, 10))
        scan.assert_not_called()

        release_calls = []
        orig_acquire = accel._acquire_buffer

        def spy_acquire(ct, mapping):
            view, rel = orig_acquire(ct, mapping)

            def wrapped_rel(ptr):
                release_calls.append(True)
                return rel(ptr)

            return view, wrapped_rel

        with (
            mock.patch.dict(os.environ, {"LEONIDA_SHADER_CARVE_NATIVE": "1"}),
            mock.patch.object(accel, "_load_native", return_value=scan),
            mock.patch.object(accel, "_acquire_buffer", side_effect=spy_acquire),
            accel.entry_search(b"", self.PATTERN) as search,
        ):
            self.assertEqual(len(release_calls), 1)
            self.assertIsNone(search(0))
        self.assertEqual(len(release_calls), 1)
        scan.assert_not_called()

    @unittest.skipUnless(sys.implementation.name == "cpython" and sys.version_info >= (3, 11),
                         "requires CPython Py_buffer ABI")
    def test_native_paths_validation_fallback_and_single_release_on_exception(self):
        data = b"aaDXBC1111bbDXBC2222cc"

        # 1. Valid hits: pos match, native offset hit, and end-of-buffer miss.
        scan = mock.Mock(side_effect=[12, len(data)])
        with (
            mock.patch.dict(os.environ, {"LEONIDA_SHADER_CARVE_NATIVE": "1"}),
            mock.patch.object(accel, "_load_native", return_value=scan),
            accel.entry_search(data, self.PATTERN) as search,
        ):
            self.assertEqual(search(2).span(), (2, 10))   # direct pattern.match hit at pos=2
            self.assertEqual(search(3).span(), (12, 20))  # native offset=12 hit
            self.assertIsNone(search(21))                 # native offset == view.len -> None
            # bool / negative pos must bypass native scan cleanly without breaking scan.
            self.assertEqual(search(-1).span(), (2, 10))
        self.assertEqual(scan.call_count, 2)

        with (
            mock.patch.dict(os.environ, {"LEONIDA_SHADER_CARVE_NATIVE": "1"}),
            mock.patch.object(accel, "_load_native", return_value=scan),
            accel.entry_search(data, self.PATTERN) as search,
        ):
            scan_before = scan.call_count
            self.assertEqual(search(True).span(), (2, 10))
            self.assertEqual(scan.call_count, scan_before)

        # 2. Invalid offsets and exceptions disable native scan and match reference().
        bad_outcomes = [
            (len(data) + 5, None),
            (0, None),
            (1, None),  # not matching at offset 1
            (None, OSError("boom")),
            (None, ValueError("bad")),
            (None, TypeError("type")),
            (None, OverflowError("huge")),
            (None, ctypes.ArgumentError("arg")),
        ]
        for ret_val, exc in bad_outcomes:
            with self.subTest(ret_val=ret_val, exc=exc):
                bad_scan = mock.Mock(return_value=ret_val, side_effect=exc)
                with (
                    mock.patch.dict(os.environ, {"LEONIDA_SHADER_CARVE_NATIVE": "1"}),
                    mock.patch.object(accel, "_load_native", return_value=bad_scan),
                    accel.entry_search(data, self.PATTERN) as search,
                ):
                    self.assertEqual(search(3).span(), (12, 20))
                    # Once disabled, subsequent search uses reference() without re-calling scan.
                    self.assertEqual(search(0).span(), (2, 10))
                    bad_scan.assert_called_once()

        # 3. Caller exception still releases the Py_buffer exactly once.
        releases = []
        orig_acquire = accel._acquire_buffer

        def spy_acquire(ct, mapping):
            view, rel = orig_acquire(ct, mapping)

            def wrapped_rel(ptr):
                releases.append(True)
                return rel(ptr)

            return view, wrapped_rel

        class CallerError(RuntimeError):
            pass

        with self.assertRaises(CallerError):
            with (
                mock.patch.dict(os.environ, {"LEONIDA_SHADER_CARVE_NATIVE": "1"}),
                mock.patch.object(accel, "_load_native", return_value=mock.Mock(return_value=len(data))),
                mock.patch.object(accel, "_acquire_buffer", side_effect=spy_acquire),
                accel.entry_search(data, self.PATTERN),
            ):
                raise CallerError("stop")
        self.assertEqual(len(releases), 1)


if __name__ == "__main__":
    unittest.main()
