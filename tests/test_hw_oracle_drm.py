"""Standalone unit tests for tools/hw_oracle/drm.py (stdlib-only, no GPU or /dev/dri required)."""
import sys

sys.dont_write_bytecode = True

import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest import mock

TOOLS = Path(__file__).resolve().parents[1] / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from hw_oracle import drm, runner  # noqa: E402


def _make_elf64(sections_spec, symbols_spec):
    """Build a minimal little-endian ELF64 relocatable object for testing _symbols/text_section/kernel_registers.

    sections_spec: list of (name, sh_type, sh_addr, data_bytes)
    symbols_spec:  list of (name, shndx, st_value)
    """
    shstrtab = bytearray(b"\0")
    strtab = bytearray(b"\0")

    def add_str(buf, text):
        if not text:
            return 0
        off = len(buf)
        buf.extend(text.encode("ascii") + b"\0")
        return off

    # Section 0: SHT_NULL
    raw_sections = [(0, 0, 0, b"")]
    for name, sh_type, sh_addr, data in sections_spec:
        raw_sections.append((add_str(shstrtab, name), sh_type, sh_addr, data))

    sym_bytes = bytearray(24)  # STN_UNDEF
    for name, shndx, value in symbols_spec:
        sym_bytes.extend(struct.pack("<IBBHQQ", add_str(strtab, name), 0, 0, shndx, value, 0))

    strtab_idx = len(raw_sections)
    raw_sections.append((add_str(shstrtab, ".strtab"), 3, 0, bytes(strtab)))
    raw_sections.append((add_str(shstrtab, ".symtab"), 2, 0, bytes(sym_bytes)))
    shstrndx = len(raw_sections)
    raw_sections.append((add_str(shstrtab, ".shstrtab"), 3, 0, bytes(shstrtab)))

    body = bytearray(64)  # ELF64 header placeholder
    shdrs = bytearray()
    for i, (sh_name, sh_type, sh_addr, data) in enumerate(raw_sections):
        if i == 0:
            shdrs.extend(struct.pack("<IIQQQQIIQQ", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0))
            continue
        sh_offset = len(body)
        body.extend(data)
        sh_link = strtab_idx if sh_type == 2 else 0
        sh_entsize = 24 if sh_type == 2 else 0
        shdrs.extend(
            struct.pack("<IIQQQQIIQQ", sh_name, sh_type, 0, sh_addr, sh_offset, len(data), sh_link, 0, 8, sh_entsize)
        )

    shoff = len(body)
    body.extend(shdrs)
    # e_ident(16), e_type(2)=ET_REL, e_machine(2)=EM_AMDGPU, e_version(4), e_entry(8),
    # e_phoff(8), e_shoff(8), e_flags(4), e_ehsize(2), e_phentsize(2), e_phnum(2),
    # e_shentsize(2), e_shnum(2), e_shstrndx(2)
    struct.pack_into(
        "<16sHHIQQQIHHHHHH",
        body,
        0,
        b"\x7fELF\x02\x01\x01" + b"\0" * 9,
        1,
        224,
        1,
        0,
        0,
        shoff,
        0,
        64,
        0,
        0,
        64,
        len(raw_sections),
        shstrndx,
    )
    return bytes(body)


def _make_kd(group=0, private=0, rsrc3=0, rsrc1=0xE0AC0001, rsrc2=0x00000084, properties=0x0008):
    """Build a 64-byte AMDHSA kernel descriptor matching the offsets read by drm.kernel_registers()."""
    buf = bytearray(64)
    struct.pack_into("<II", buf, 0, group, private)
    struct.pack_into("<IIIH", buf, 44, rsrc3, rsrc1, rsrc2, properties)
    return bytes(buf)


class ModuleConstantsTests(unittest.TestCase):
    def test_module_paths_and_unavailable_alias(self):
        self.assertEqual(drm.HERE, os.path.dirname(os.path.abspath(drm.__file__)))
        self.assertEqual(drm.SOURCE, os.path.join(drm.HERE, "hw_oracle_drm.c"))
        self.assertIs(drm.Unavailable, runner.Unavailable)


class SysfsNodeTests(unittest.TestCase):
    def _create_node(self, root, name, connectors):
        node = os.path.join(root, name)
        with open(node, "wb"):
            pass
        sysfs = os.path.join(root, "sysfs")
        for card, conn, state in connectors:
            cdir = os.path.join(sysfs, name, "device", "drm", card, conn)
            os.makedirs(cdir, exist_ok=True)
            with open(os.path.join(cdir, "status"), "w", encoding="ascii") as f:
                f.write(state)
        return node, sysfs

    def test_connected_displays_filters_and_sorts(self):
        with tempfile.TemporaryDirectory() as tmp:
            node, sysfs = self._create_node(
                tmp,
                "renderD129",
                [
                    ("card1", "card1-HDMI-A-1", "disconnected\n"),
                    ("card1", "card1-DP-2", "connected\n"),
                    ("card1", "card1-DP-1", "  connected  "),
                    ("card1", "card1-eDP-1", "unknown\n"),
                ],
            )
            # Make an unreadable directory named 'status' to verify OSError is ignored.
            bad_status = os.path.join(sysfs, "renderD129", "device", "drm", "card1", "card1-DVI-D-1", "status")
            os.makedirs(bad_status, exist_ok=True)

            self.assertEqual(drm.connected_displays(node, sysfs=sysfs), ["card1-DP-1", "card1-DP-2"])
            self.assertEqual(drm.connected_displays("renderD129", sysfs=sysfs), ["card1-DP-1", "card1-DP-2"])
            self.assertEqual(drm.connected_displays(os.path.join(tmp, "renderD999"), sysfs=sysfs), [])

    def test_find_node_paths_and_negative_controls(self):
        with tempfile.TemporaryDirectory() as tmp:
            free_node, sysfs = self._create_node(tmp, "renderD128", [("card0", "card0-DP-1", "disconnected\n")])
            busy_node, _ = self._create_node(tmp, "renderD129", [("card1", "card1-DP-1", "connected\n")])

            with self.assertRaisesRegex(drm.Unavailable, "LEONIDA_HW_DRM_NODE"):
                drm.find_node({}, sysfs=sysfs)

            with self.assertRaisesRegex(drm.Unavailable, "LEONIDA_HW_DRM_NODE"):
                drm.find_node({"LEONIDA_HW_DRM_NODE": ""}, sysfs=sysfs)

            missing = os.path.join(tmp, "renderD200")
            with self.assertRaisesRegex(drm.Unavailable, "does not exist"):
                drm.find_node({"LEONIDA_HW_DRM_NODE": missing}, sysfs=sysfs)

            self.assertEqual(drm.find_node({"LEONIDA_HW_DRM_NODE": free_node}, sysfs=sysfs), free_node)

            with self.assertRaisesRegex(drm.Unavailable, "card1-DP-1"):
                drm.find_node({"LEONIDA_HW_DRM_NODE": busy_node}, sysfs=sysfs)

            with self.assertRaisesRegex(drm.Unavailable, "card1-DP-1"):
                drm.find_node({"LEONIDA_HW_DRM_NODE": busy_node, "LEONIDA_HW_DRM_ALLOW_DISPLAY": "0"}, sysfs=sysfs)

            self.assertEqual(
                drm.find_node({"LEONIDA_HW_DRM_NODE": busy_node, "LEONIDA_HW_DRM_ALLOW_DISPLAY": "1"}, sysfs=sysfs),
                busy_node,
            )

            with mock.patch.dict(os.environ, {"LEONIDA_HW_DRM_NODE": free_node}, clear=False):
                self.assertEqual(drm.find_node(None, sysfs=sysfs), free_node)


class ElfAndDescriptorTests(unittest.TestCase):
    def test_symbols_and_text_section(self):
        code = b"\x01\x02\x03\x04\x05\x06\x07\x08"
        elf = _make_elf64(
            [(".rodata", 1, 0, b"\xaa\xbb"), (".text", 1, 0x1000, code)],
            [("data_sym", 1, 0), ("k0", 2, 0x1000), ("k1", 2, 0x1004), ("undef", 0, 0), ("oob", 99, 0)],
        )
        raw_text, offsets = drm.text_section(elf)
        self.assertEqual(raw_text, code)
        self.assertEqual(offsets, {"k0": 0, "k1": 4})

    def test_text_section_negative_controls(self):
        for bad in (b"", b"MZ" + b"\0" * 64, b"\x7fELF\x02\x02" + b"\0" * 64):
            with self.assertRaisesRegex(ValueError, "ELF64"):
                drm.text_section(bad)

        no_text = _make_elf64([(".rodata", 1, 0, b"\0" * 16)], [])
        with self.assertRaisesRegex(ValueError, "no .text section"):
            drm.text_section(no_text)

    def test_kernel_registers_wave_sizes_and_lds_rounding(self):
        kd_w32 = _make_kd(group=0, rsrc1=0x11111111, rsrc2=0x00000084, properties=0x0408)
        kd_w64 = _make_kd(group=1, rsrc1=0x22222222, rsrc2=(0x1FF << 15) | 0x84, properties=0x0008)
        kd_4k = _make_kd(group=4096, rsrc1=0x33333333, rsrc2=0x00000084, properties=0x0408)
        kd_max = _make_kd(group=65536, rsrc1=0x44444444, rsrc2=0x00000084, properties=0x0008)
        rodata = kd_w32 + kd_w64 + kd_4k + kd_max

        elf = _make_elf64(
            [(".rodata", 1, 0x200, rodata), (".text", 1, 0, b"\0" * 16)],
            [
                ("k_w32.kd", 1, 0x200),
                ("k_w64.kd", 1, 0x200 + 64),
                ("k_4k.kd", 1, 0x200 + 128),
                ("k_max.kd", 1, 0x200 + 192),
                ("k_w32", 2, 0),
            ],
        )
        regs = drm.kernel_registers(elf)
        self.assertEqual(
            regs,
            {
                "k_w32": (0x11111111, 0x84, 32),
                "k_w64": (0x22222222, (1 << 15) | 0x84, 64),
                "k_4k": (0x33333333, (8 << 15) | 0x84, 32),
                "k_max": (0x44444444, (128 << 15) | 0x84, 64),
            },
        )

    def test_kernel_registers_negative_controls(self):
        # Truncated descriptor running past section end
        short_elf = _make_elf64([(".rodata", 1, 0, b"\0" * 32)], [("k0.kd", 1, 0)])
        with self.assertRaisesRegex(ValueError, "runs past its section"):
            drm.kernel_registers(short_elf)

        cases = [
            ("missing kernarg bit", {"properties": 0x0400}, "kernarg pointer"),
            ("unsupported property bit", {"properties": 0x0409}, "kernel code properties"),
            ("non-zero private memory", {"private": 16}, "scratch memory"),
            ("non-zero rsrc3", {"rsrc3": 1}, "COMPUTE_PGM_RSRC3"),
            ("scratch_en in rsrc2", {"rsrc2": 0x85}, "scratch memory"),
            ("group LDS > 65536", {"group": 65537}, "at most 65536"),
            ("user SGPRs != 2 (0)", {"rsrc2": 0x80}, "0 user SGPRs"),
            ("user SGPRs != 2 (4)", {"rsrc2": 0x88}, "4 user SGPRs"),
        ]
        for label, fields, pattern in cases:
            with self.subTest(label=label):
                elf = _make_elf64([(".rodata", 1, 0, _make_kd(**fields))], [("k0.kd", 1, 0)])
                with self.assertRaisesRegex(RuntimeError, pattern):
                    drm.kernel_registers(elf)


class LauncherTokensTests(unittest.TestCase):
    def test_launcher_tokens_formatting_and_errors(self):
        offsets = {"k0": 0, "k1": 128}
        registers = {"k0": (0xE0AC0001, 0x84, 32), "k1": (0xE0AC3007, 0x40084, 64)}

        self.assertEqual(
            drm.launcher_tokens(["k0", "k1:64:896"], offsets, registers),
            ["k0@0/0xe0ac0001/0x84/32", "k1@128/0xe0ac3007/0x40084/64:64:896"],
        )
        with self.assertRaisesRegex(RuntimeError, "k_missing"):
            drm.launcher_tokens(["k_missing"], offsets, registers)
        with self.assertRaisesRegex(RuntimeError, "k0"):
            drm.launcher_tokens(["k0"], offsets, {})


class DrmOracleMockedTests(unittest.TestCase):
    def _new_oracle(self):
        return object.__new__(drm.DrmOracle)

    def test_init_missing_source_raises_unavailable(self):
        with mock.patch.object(drm.os.path, "isfile", return_value=False):
            with self.assertRaisesRegex(drm.Unavailable, "hw_oracle_drm.c is missing"):
                drm.DrmOracle()

    def test_init_happy_path_and_unsupported_arch(self):
        with tempfile.TemporaryDirectory() as tmp:
            with (
                mock.patch.object(drm, "find_node", return_value="/dev/dri/renderD129"),
                mock.patch.object(drm.runner, "find_clang", return_value="/usr/bin/clang"),
                mock.patch.object(drm.DrmOracle, "_build_launcher", return_value="/tmp/launcher"),
                mock.patch.object(drm.DrmOracle, "_device_arch", return_value="gfx1100") as dev_arch,
                mock.patch.object(drm.DrmOracle, "_probe", return_value="MockGPU") as probe,
                mock.patch.object(drm.DrmOracle, "_probe_waves", create=True) as probe_waves,
                mock.patch.object(drm.os.path, "isfile", return_value=True),
            ):
                oracle = drm.DrmOracle(workdir=tmp, chunk=250)
                self.assertEqual(oracle.node, "/dev/dri/renderD129")
                self.assertEqual(oracle.clang, "/usr/bin/clang")
                self.assertEqual(oracle.chunk, 250)
                self.assertEqual(oracle.workdir, tmp)
                self.assertEqual(oracle.exe, "/tmp/launcher")
                self.assertEqual(oracle.arch, "gfx1100")
                self.assertEqual(oracle.device, "MockGPU")
                dev_arch.assert_called_once_with()
                probe.assert_called_once_with()
                probe_waves.assert_called_once_with()

            with (
                mock.patch.object(drm, "find_node", return_value="/dev/dri/renderD129"),
                mock.patch.object(drm.runner, "find_clang", return_value="/usr/bin/clang"),
                mock.patch.object(drm.DrmOracle, "_build_launcher", return_value="/tmp/launcher"),
                mock.patch.object(drm.os.path, "isfile", return_value=True),
                self.assertRaisesRegex(drm.Unavailable, "LEONIDA_HW_ARCH"),
            ):
                drm.DrmOracle(arch="gfx90a", workdir=tmp)

    def test_build_launcher_cached_and_compile_paths(self):
        oracle = self._new_oracle()
        with tempfile.TemporaryDirectory() as tmp:
            fake_src = os.path.join(tmp, "hw_oracle_drm.c")
            with open(fake_src, "wb") as f:
                f.write(b"/* mock c source */")

            with (
                mock.patch.object(drm, "SOURCE", fake_src),
                mock.patch.object(drm.tempfile, "gettempdir", return_value=tmp),
            ):
                # 1. Non-gcc compiler is rejected
                with (
                    mock.patch.object(drm.shutil, "which", return_value=None),
                    mock.patch.object(drm.runner, "find_cc", return_value=("msvc", "cl.exe")),
                ):
                    with self.assertRaisesRegex(drm.Unavailable, "needs gcc"):
                        oracle._build_launcher()

                # 2. Failed gcc invocation raises Unavailable with stderr
                fail = subprocess.CompletedProcess([], 1, "", "drm/amdgpu_drm.h: No such file")
                with (
                    mock.patch.object(drm.shutil, "which", return_value=None),
                    mock.patch.object(drm.runner, "find_cc", return_value=("gcc", "/usr/bin/gcc")),
                    mock.patch.object(drm.subprocess, "run", return_value=fail) as run_mock,
                ):
                    with self.assertRaisesRegex(drm.Unavailable, "did not build"):
                        oracle._build_launcher()
                    cmd = run_mock.call_args[0][0]
                    self.assertIn("-I/usr/include/libdrm", cmd)
                    self.assertIn("-ldrm_amdgpu", cmd)

                # 3. Successful build with pkg-config flags creates exe via os.replace
                pkg_ok = subprocess.CompletedProcess([], 0, "-I/opt/drm/include -ldrm_amdgpu\n", "")
                gcc_ok = subprocess.CompletedProcess([], 0, "", "")

                def fake_run(cmd, **_kw):
                    if os.path.basename(cmd[0]) == "pkg-config":
                        return pkg_ok
                    tmp_out = cmd[cmd.index("-o") + 1]
                    with open(tmp_out, "wb") as f:
                        f.write(b"ELF")
                    return gcc_ok

                with (
                    mock.patch.object(drm.shutil, "which", return_value="/usr/bin/pkg-config"),
                    mock.patch.object(drm.runner, "find_cc", return_value=("gcc", "/usr/bin/gcc")),
                    mock.patch.object(drm.subprocess, "run", side_effect=fake_run),
                ):
                    exe = oracle._build_launcher()
                    self.assertTrue(os.path.isfile(exe))

                # 4. Subsequent call hits cache without invoking find_cc or subprocess
                with (
                    mock.patch.object(drm.runner, "find_cc", side_effect=AssertionError("should not compile")),
                    mock.patch.object(drm.subprocess, "run", side_effect=AssertionError("should not run")),
                ):
                    self.assertEqual(oracle._build_launcher(), exe)

    def test_device_arch_parsing_and_negative_controls(self):
        oracle = self._new_oracle()
        oracle.exe = "/tmp/launcher"
        oracle.node = "/dev/dri/renderD129"

        for line, expected in (
            ("# GPU arch=gfx1150", "gfx1150"),
            ("# GPU arch=gfx10xx", "gfx1030"),
            ("# GPU arch=gfx11xx", "gfx1100"),
            ("# GPU arch=gfx12xx", "gfx1201"),
            ("# GPU unknown", "gfx1030"),
        ):
            done = subprocess.CompletedProcess([], 0, line + "\n", "")
            with mock.patch.object(drm.subprocess, "run", return_value=done):
                self.assertEqual(oracle._device_arch(), expected)

        for stdout, stderr, pattern in (
            ("", "open(/dev/dri/renderD129): Permission denied", "Permission denied"),
            ("unexpected output", "", "no device line"),
        ):
            bad = subprocess.CompletedProcess([], 1, stdout, stderr)
            with mock.patch.object(drm.subprocess, "run", return_value=bad):
                with self.assertRaisesRegex(drm.Unavailable, pattern):
                    oracle._device_arch()

    def test_assemble_and_launch(self):
        oracle = self._new_oracle()
        oracle.clang = "/usr/bin/clang"
        oracle.arch = "gfx1100"
        oracle.exe = "/tmp/launcher"
        oracle.node = "/dev/dri/renderD129"

        with tempfile.TemporaryDirectory() as tmp:
            oracle.workdir = tmp

            # Failed assembly returns (None, stderr)
            fail = subprocess.CompletedProcess([], 1, "", "error: invalid instruction")
            with mock.patch.object(drm.subprocess, "run", return_value=fail) as run_mock:
                code, err = oracle._assemble("bad", wave=64)
                self.assertIsNone(code)
                self.assertEqual(err, "error: invalid instruction")
                self.assertIn("-mwavefrontsize64", run_mock.call_args[0][0])

            # Successful assembly extracts .text and kernel_registers, then _launch formats tokens
            elf = _make_elf64(
                [(".rodata", 1, 0, _make_kd(group=512, rsrc1=0x1234, rsrc2=0x84, properties=0x0408)),
                 (".text", 1, 0, b"\xde\xad\xbe\xef")],
                [("k0.kd", 1, 0), ("k0", 2, 0)],
            )

            def fake_clang(cmd, **_kw):
                obj_path = cmd[cmd.index("-o") + 1]
                with open(obj_path, "wb") as f:
                    f.write(elf)
                return subprocess.CompletedProcess(cmd, 0, "", "")

            with mock.patch.object(drm.subprocess, "run", side_effect=fake_clang) as clang_mock:
                code, err = oracle._assemble("s_endpgm\n", wave=32)
                self.assertEqual(err, "")
                self.assertNotIn("-mwavefrontsize64", clang_mock.call_args[0][0])
                with open(code, "rb") as f:
                    self.assertEqual(f.read(), b"\xde\xad\xbe\xef")
                self.assertEqual(oracle._offsets, {"k0": 0})
                self.assertEqual(oracle._registers, {"k0": (0x1234, (1 << 15) | 0x84, 32)})

            launch_res = subprocess.CompletedProcess([], 0, "00000001\n", "")
            with mock.patch.object(drm.subprocess, "run", return_value=launch_res) as launch_mock:
                rc, out, err = oracle._launch(code, ["k0:32:4"], words=4)
                self.assertEqual((rc, out, err), (0, "00000001\n", ""))
                self.assertEqual(launch_mock.call_args[0][0], ["/tmp/launcher", code, "4", "/dev/dri/renderD129"])
                self.assertEqual(launch_mock.call_args[1]["input"], "k0@0/0x1234/0x8084/32:32:4\n")


if __name__ == "__main__":
    unittest.main()
