"""d3d12_xs_surface: the 116-slot export table and the 83 standard GUIDs.

    python -m pytest tests/test_d3d12_xs_surface.py -q
    python -I tests/test_d3d12_xs_surface.py

The SCARAB cross-check runs only when SCARAB_REF points at a checkout; it is not needed for CI.
"""
import os
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
import d3d12_xs_surface as S  # noqa: E402


def test_slots_are_116():
    assert S.check_slots() == []


def test_data_classes():
    assert S.check_data_classes() == []


def test_guid_values():
    assert S.check_guid_values() == []


def test_iid_block_is_alphabetical():
    assert S.check_iid_order() == []


def test_unrecovered_console_iids_stay_outside_the_table():
    # SCARAB's provenance says the runtime does not export these; they must not acquire a
    # guessed ordinal or GUID value from ordering.
    assert len(S.UNRECOVERED_CONSOLE_IIDS) == 4
    assert S.check_unrecovered_console_iids() == []


def test_standard_iids_match_sdk():
    um, _shared = S.find_sdk_include()
    if not um:
        raise unittest.SkipTest("Windows SDK headers not found")
    errors, checked, _um = S.check_sdk()
    assert errors == []
    assert checked == 83


def test_sdk_include_override_accepts_a_trailing_separator():
    # WINDOWS_SDK_INCLUDE may name the version folder or its um folder, with or without a
    # trailing separator; all four spellings must find the same um/shared pair.
    with tempfile.TemporaryDirectory() as tmp:
        version = os.path.join(tmp, "10.0.99999.0")
        um = os.path.join(version, "um")
        os.makedirs(um)
        os.makedirs(os.path.join(version, "shared"))
        saved = os.environ.get("WINDOWS_SDK_INCLUDE")
        try:
            for value in (um, um + os.sep, version, version + os.sep):
                os.environ["WINDOWS_SDK_INCLUDE"] = value
                got_um, got_shared = S.find_sdk_include()
                assert got_um is not None, value
                assert os.path.normcase(got_um) == os.path.normcase(um), (value, got_um)
                assert os.path.normcase(got_shared) == os.path.normcase(
                    os.path.join(version, "shared")), (value, got_shared)
        finally:
            if saved is None:
                os.environ.pop("WINDOWS_SDK_INCLUDE", None)
            else:
                os.environ["WINDOWS_SDK_INCLUDE"] = saved


def test_sdk_discovery_sorts_versions_numerically():
    # A lexicographic sort puts 10.0.99999.0 above 10.0.100000.0, but numerically 100000 is the
    # newer build; automatic discovery must compare the version by number, not by string.
    with tempfile.TemporaryDirectory() as tmp:
        include = os.path.join(tmp, "Windows Kits", "10", "Include")
        os.makedirs(include)
        for ver in ("10.0.99999.0", "10.0.100000.0"):
            vdir = os.path.join(include, ver)
            os.makedirs(os.path.join(vdir, "um"))
            os.makedirs(os.path.join(vdir, "shared"))
            open(os.path.join(vdir, "um", "d3d12.h"), "w").close()
        with mock.patch.dict(os.environ, {"ProgramFiles(x86)": tmp, "ProgramFiles": tmp},
                             clear=False):
            os.environ.pop("WINDOWS_SDK_INCLUDE", None)
            os.environ.pop("SDK_INCLUDE", None)
            got_um, got_shared = S.find_sdk_include()
        assert got_um is not None
        assert os.path.normcase(got_um) == os.path.normcase(
            os.path.join(include, "10.0.100000.0", "um")), got_um
        assert os.path.normcase(got_shared) == os.path.normcase(
            os.path.join(include, "10.0.100000.0", "shared")), got_shared


def test_sdk_discovery_skips_a_newer_driver_kit_folder():
    # A Windows Driver Kit adds a newer version folder whose um/ holds only driver headers (measured: 43 files in
    # 10.0.26100.0 beside a full 10.0.19041.0 on Windows 11); picking it found none of the 83 GUIDs, so the SDK check
    # failed instead of using the SDK that is there.
    with tempfile.TemporaryDirectory() as tmp:
        include = os.path.join(tmp, "Windows Kits", "10", "Include")
        sdk = os.path.join(include, "10.0.19041.0")
        wdk = os.path.join(include, "10.0.26100.0")
        for vdir in (sdk, wdk):
            os.makedirs(os.path.join(vdir, "um"))
            os.makedirs(os.path.join(vdir, "shared"))
        open(os.path.join(sdk, "um", "d3d12.h"), "w").close()
        open(os.path.join(wdk, "um", "d3d12umddi.h"), "w").close()
        with mock.patch.dict(os.environ, {"ProgramFiles(x86)": tmp, "ProgramFiles": tmp}, clear=False):
            os.environ.pop("WINDOWS_SDK_INCLUDE", None)
            os.environ.pop("SDK_INCLUDE", None)
            got_um, got_shared = S.find_sdk_include()
            assert got_um is not None
            assert os.path.normcase(got_um) == os.path.normcase(os.path.join(sdk, "um")), got_um
            assert os.path.normcase(got_shared) == os.path.normcase(os.path.join(sdk, "shared")), got_shared
            # Only the driver kit: no SDK, so the check skips rather than failing on 83 missing GUIDs.
            os.remove(os.path.join(sdk, "um", "d3d12.h"))
            assert S.find_sdk_include() == (None, None)


def test_scarab_cross_check():
    root = os.environ.get("SCARAB_REF")
    if not root:
        raise unittest.SkipTest("SCARAB_REF is not set")
    errors, _notes = S.check_scarab(root)
    assert errors == []


def test_check_guid_values_rejects_negative_components():
    first_name = next(iter(S.STANDARD_GUIDS))
    bad = (-1, 0, 0x4000, 0x80, 0, 0, 0, 0, 0, 0, 0)
    orig = S.STANDARD_GUIDS[first_name]
    try:
        S.STANDARD_GUIDS[first_name] = bad
        errors = S.check_guid_values()
        assert any(first_name in e for e in errors), errors
        S.STANDARD_GUIDS[first_name] = (0, 0, 0x4000, 0x80, 0, -5, 0, 0, 0, 0, 0)
        errors = S.check_guid_values()
        assert any(first_name in e for e in errors), errors
    finally:
        S.STANDARD_GUIDS[first_name] = orig


def test_scarab_parsers_handle_bom_and_crlf():
    import tempfile

    with tempfile.TemporaryDirectory() as tmp:
        cat = os.path.join(tmp, "catalogue.tsv")
        with open(cat, "wb") as f:
            f.write(
                "\ufeffkind\tname\tsource\r\nmethod\tCreateCommandQueueX\tbridge\r\nenum\tD3D12_FOO\tbridge\r\n".encode(
                    "utf-8"
                )
            )
        rows = S.parse_scarab_catalogue(cat)
        assert rows == [("method", "CreateCommandQueueX", "bridge"), ("enum", "D3D12_FOO", "bridge")], rows

        tbl = os.path.join(tmp, "iid_table.cpp")
        with open(tbl, "wb") as f:
            f.write(
                "\ufeffstatic const GUID IID_ID3D12Foo = {0x1,0x2,0x3,{0x4,0x5,0x6,0x7,0x8,0x9,0xa,0xb}};\r\n".encode(
                    "utf-8"
                )
            )
        parsed_tbl = S.parse_scarab_iid_table(tbl)
        assert parsed_tbl == {"IID_ID3D12Foo": (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11)}, parsed_tbl

        inc = os.path.join(tmp, "iid_console.inc")
        with open(inc, "wb") as f:
            f.write("\ufeff// GUID IID_ID3D12Foo ordinal 28\r\n".encode("utf-8"))
        con = S.parse_scarab_iid_console(inc)
        assert con == {"IID_ID3D12Foo": 28}, con


def test_check_scarab_and_cli_guard_bad_paths_and_decoding():
    import io
    import tempfile
    from contextlib import redirect_stdout

    with tempfile.TemporaryDirectory() as tmp:
        # Empty --scarab "" must not silently pass.
        out = io.StringIO()
        with redirect_stdout(out):
            rc = S.main(["--scarab", ""])
        assert rc == 1, rc
        assert "FAIL:" in out.getvalue(), out.getvalue()

        # Directory where a file is expected + invalid UTF-8 bytes must return clean errors.
        bridge = os.path.join(tmp, "components", "runtime-bridge", "src")
        ext_src = os.path.join(tmp, "components", "extension-wrapper", "src")
        ext_doc = os.path.join(tmp, "components", "extension-wrapper", "docs")
        os.makedirs(bridge)
        os.makedirs(ext_src)
        os.makedirs(ext_doc)
        os.makedirs(os.path.join(bridge, "iid_table.cpp"))
        with open(os.path.join(ext_src, "iid_console.inc"), "wb") as f:
            f.write(b"\xff\xfe\x00\x00invalid-utf8\n")
        with open(os.path.join(ext_doc, "console-extension-catalogue.tsv"), "wb") as f:
            f.write(b"method\tFirstMethod\r\n")

        errors, _notes = S.check_scarab(tmp)
        assert any("iid_table.cpp" in e for e in errors), errors
        assert any("iid_console.inc" in e for e in errors), errors


if __name__ == "__main__":
    test_slots_are_116()
    test_data_classes()
    test_guid_values()
    test_iid_block_is_alphabetical()
    test_unrecovered_console_iids_stay_outside_the_table()
    test_sdk_include_override_accepts_a_trailing_separator()
    test_sdk_discovery_sorts_versions_numerically()
    test_sdk_discovery_skips_a_newer_driver_kit_folder()
    test_check_guid_values_rejects_negative_components()
    test_scarab_parsers_handle_bom_and_crlf()
    test_check_scarab_and_cli_guard_bad_paths_and_decoding()
    # Call the two optional checks through their real definitions so this runner cannot drift from them; both
    # raise unittest.SkipTest when their inputs (SDK headers, SCARAB_REF) are absent.
    for optional in (test_standard_iids_match_sdk, test_scarab_cross_check):
        try:
            optional()
        except unittest.SkipTest as skip:
            print(f"skipped: {skip}")
    print("ok")
