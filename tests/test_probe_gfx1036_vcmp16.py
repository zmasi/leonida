"""CPU-only unit tests for tools/probe_gfx1036_vcmp16.py (stdlib-only)."""
import io
import math
import os
import re
import struct
import subprocess
import sys
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
SEMANTIC = ROOT / "tests" / "semantic"
for p in (str(ROOT), str(SEMANTIC), str(TOOLS)):
    if p not in sys.path:
        sys.path.insert(0, p)

import probe_gfx1036_vcmp16 as pr  # noqa: E402
from support import STUB_DXBC, rdna2spirv  # noqa: E402


def _f16_from_bits(bits: int) -> float:
    return struct.unpack("<e", struct.pack("<H", bits & 0xFFFF))[0]


def _i16_from_bits(bits: int) -> int:
    v = bits & 0xFFFF
    return v - 0x10000 if (v & 0x8000) else v


def _select_half(word: int, opsel: int) -> int:
    return ((word >> 16) & 0xFFFF) if opsel else (word & 0xFFFF)


def _eval_operand_word(tok: str, regs: dict[str, int], user_sgprs: list[int] | None) -> tuple[int, bool, bool]:
    """Return (32-bit word, abs_mod, neg_mod) for an RDNA2 v_cmp operand token."""
    abs_mod = False
    neg_mod = False
    t = tok.strip()
    if t == "-1":
        return 0xFFFFFFFF, abs_mod, neg_mod
    if t in ("1", "0"):
        return int(t) & 0xFFFFFFFF, abs_mod, neg_mod
    # Inline float constants: 1.0 -> 0x3C00 in lo, 0x0000 in hi; -2.0 -> 0xC000 in lo, 0x0000 in hi
    if "." in t:
        fval = float(t)
        lo_bits = struct.unpack("<H", struct.pack("<e", fval))[0]
        return lo_bits, abs_mod, neg_mod

    if t.startswith("-"):
        neg_mod = True
        t = t[1:].strip()
    if t.startswith("|") and t.endswith("|"):
        abs_mod = True
        t = t[1:-1].strip()

    if t in regs:
        return regs[t], abs_mod, neg_mod
    if t == "s4" and user_sgprs:
        return user_sgprs[0], abs_mod, neg_mod
    if t.startswith("0x") or t.startswith("0X"):
        return int(t, 16) & 0xFFFFFFFF, abs_mod, neg_mod
    raise AssertionError(f"Unexpected operand token: {tok}")


def _eval_vcmp16(op: str, w0: int, abs0: bool, neg0: bool, w1: int, abs1: bool, neg1: bool,
                 os0: int, os1: int) -> int:
    """Reference evaluator for v_cmp_*_f16 / u16 / i16 with op_sel and f16 modifiers."""
    h0 = _select_half(w0, os0)
    h1 = _select_half(w1, os1)

    if op.endswith("_u16_e64"):
        cmp_kind = op.removeprefix("v_cmp_").removesuffix("_u16_e64")
        if cmp_kind == "eq":
            return int(h0 == h1)
        if cmp_kind == "lt":
            return int(h0 < h1)
        raise AssertionError(f"Unsupported u16 op: {op}")

    if op.endswith("_i16_e64"):
        s0, s1 = _i16_from_bits(h0), _i16_from_bits(h1)
        cmp_kind = op.removeprefix("v_cmp_").removesuffix("_i16_e64")
        if cmp_kind == "eq":
            return int(s0 == s1)
        if cmp_kind == "lt":
            return int(s0 < s1)
        raise AssertionError(f"Unsupported i16 op: {op}")

    if op.endswith("_f16_e64"):
        if abs0:
            h0 &= 0x7FFF
        if neg0:
            h0 ^= 0x8000
        if abs1:
            h1 &= 0x7FFF
        if neg1:
            h1 ^= 0x8000
        f0, f1 = _f16_from_bits(h0), _f16_from_bits(h1)
        unord = math.isnan(f0) or math.isnan(f1)
        cmp_kind = op.removeprefix("v_cmp_").removeprefix("v_cmpx_").removesuffix("_f16_e64")
        if cmp_kind == "o":
            return int(not unord)
        if cmp_kind == "u":
            return int(unord)
        if cmp_kind == "eq":
            return int((not unord) and (f0 == f1))
        if cmp_kind == "neq":
            return int(unord or (f0 != f1))
        if cmp_kind == "lt":
            return int((not unord) and (f0 < f1))
        if cmp_kind == "gt":
            return int((not unord) and (f0 > f1))
        raise AssertionError(f"Unsupported f16 op: {op}")

    raise AssertionError(f"Unexpected compare op: {op}")


class FakeSiliconOracle:
    """Simulates run_on_silicon for all kernels in probe_gfx1036_vcmp16.py."""

    def __init__(self, flip_opsel: bool = False, corrupt_inactive: bool = False,
                 corrupt_exec: bool = False, corrupt_nan_ordered: bool = False):
        self.flip_opsel = flip_opsel
        self.corrupt_inactive = corrupt_inactive
        self.corrupt_exec = corrupt_exec
        self.corrupt_nan_ordered = corrupt_nan_ordered
        self.calls: list[tuple[str, int, tuple[int, ...] | None]] = []

    def __call__(self, name, asm_lines, wave_size=32, user_sgprs=None):
        self.calls.append((name, wave_size, tuple(user_sgprs) if user_sgprs else None))
        text = "\n".join(asm_lines)

        if name.startswith("cs_probe_vcmp_inactive_"):
            out = []
            for lane in range(wave_size):
                if lane % 2 == 0:
                    out.append(1)
                else:
                    out.append(1 if self.corrupt_inactive else 0)
            return tuple(out)

        if name.startswith("cs_probe_vcmpx_exec_"):
            out = []
            for lane in range(wave_size):
                if lane % 2 == 0:
                    out.append(0x100 | lane)
                else:
                    out.append((0x100 | lane) if self.corrupt_exec else pr.SENTINEL)
            return tuple(out)

        regs: dict[str, int] = {}
        cmp_line = None
        for ln in asm_lines:
            m_mov = re.match(r"^v_mov_b32\s+(v\d+),\s*(0x[0-9a-fA-F]+|\d+)$", ln.strip())
            if m_mov:
                regs[m_mov.group(1)] = int(m_mov.group(2), 0)
            elif ln.strip().startswith("v_cmp_"):
                cmp_line = ln.strip()

        if cmp_line is None:
            raise AssertionError(f"No v_cmp_ line found in {name}:\n{text}")

        m_cmp = re.match(
            r"^(v_cmp_\w+)\s+s(?:\[[\d:]+\]|\d+),\s*([^,]+),\s*([^\s]+)\s+op_sel:\[(\d),(\d)\]$",
            cmp_line,
        )
        if not m_cmp:
            raise AssertionError(f"Cannot parse cmp line: {cmp_line}")

        op, src0_tok, src1_tok, os0_s, os1_s = m_cmp.groups()
        os0, os1 = int(os0_s), int(os1_s)
        if self.flip_opsel:
            os1 ^= 1

        w0, abs0, neg0 = _eval_operand_word(src0_tok, regs, user_sgprs)
        w1, abs1, neg1 = _eval_operand_word(src1_tok, regs, user_sgprs)
        bit = _eval_vcmp16(op, w0, abs0, neg0, w1, abs1, neg1, os0, os1)
        if self.corrupt_nan_ordered and op == "v_cmp_o_f16_e64":
            bit = 1
        return (bit,) + (0,) * (wave_size - 1)


class Pack16AndConstantsTests(unittest.TestCase):
    def test_pack16_packing_and_masking(self):
        self.assertEqual(pr.pack16(0x3C00, 0x4000), 0x40003C00)
        self.assertEqual(pr.pack16(0, 0), 0)
        self.assertEqual(pr.pack16(0xFFFF, 0xFFFF), 0xFFFFFFFF)
        self.assertEqual(pr.pack16(-1, 0x12345), 0x2345FFFF)

    def test_f16_constants_match_ieee754_binary16(self):
        exact = {
            pr.H_HALF: 0.5,
            pr.H_ONE: 1.0,
            pr.H_TWO: 2.0,
            pr.H_THREE: 3.0,
            pr.H_FOUR: 4.0,
            pr.H_MONE: -1.0,
            pr.H_MTWO: -2.0,
            pr.H_MFOUR: -4.0,
            pr.H_INF: float("inf"),
            pr.H_NINF: float("-inf"),
        }
        for bits, expected in exact.items():
            with self.subTest(bits=hex(bits)):
                self.assertEqual(_f16_from_bits(bits), expected)

        pz = _f16_from_bits(pr.H_ZERO)
        nz = _f16_from_bits(pr.H_NZERO)
        self.assertEqual(pz, 0.0)
        self.assertEqual(nz, 0.0)
        self.assertGreater(math.copysign(1.0, pz), 0.0)
        self.assertLess(math.copysign(1.0, nz), 0.0)

        self.assertTrue(math.isnan(_f16_from_bits(pr.H_QNAN)))
        self.assertTrue(math.isnan(_f16_from_bits(pr.H_SNAN)))
        self.assertEqual(pr.SENTINEL, 0xDEADBEEF)


class RunOnSiliconTests(unittest.TestCase):
    def test_run_on_silicon_writes_inputs_invokes_runner_and_unpacks_output(self):
        captured: dict[str, object] = {}

        def fake_translate(s_path):
            s_p = Path(s_path)
            captured["s_text"] = s_p.read_text(encoding="utf-8")
            captured["dxbc"] = (s_p.parent.parent / f"{s_p.stem}.dxbc").read_bytes()
            captured["fssm"] = (s_p.parent.parent / "fssm" / f"{s_p.stem}.fssm").read_bytes()
            return "; fake spvasm\n"

        def fake_assemble(sa_path, spv_path):
            captured["sa_text"] = Path(sa_path).read_text(encoding="utf-8")
            Path(spv_path).write_bytes(b"SPV")

        def fake_subproc(cmd, env=None, capture_output=False, text=False):
            captured["cmd"] = cmd
            captured["env"] = env
            words = list(range(100, 168))
            Path(env["VKC_DUMP"]).write_bytes(struct.pack(f"<{len(words)}I", *words))
            return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")

        with mock.patch.object(rdna2spirv, "translate", side_effect=fake_translate), \
             mock.patch.object(rdna2spirv, "assemble_spirv", side_effect=fake_assemble), \
             mock.patch.object(subprocess, "run", side_effect=fake_subproc):
            out32 = pr.run_on_silicon("k32", ["v_mov_b32 v1, 1"], wave_size=32, user_sgprs=None)

        self.assertEqual(out32, tuple(range(100, 132)))
        self.assertEqual(captured["s_text"], "; k32\n\tv_mov_b32 v1, 1\n\ts_endpgm\n")
        self.assertEqual(captured["dxbc"], STUB_DXBC)
        fssm = captured["fssm"]
        self.assertEqual(len(fssm), 0x138 + 16)
        self.assertEqual(fssm[:0x138], bytes(0x138))
        self.assertEqual(struct.unpack("<4I", fssm[0x138:]), (0x8088, 32, 1, 1))
        self.assertNotIn("--require-wave64", captured["cmd"])
        self.assertNotIn("VKC_SGPRS", captured["env"])
        self.assertEqual(captured["env"]["LEONIDA_VK_DEVICE"], "radeon")
        self.assertEqual(captured["env"]["VKC_OUT_WORDS"], "32")

        with mock.patch.object(rdna2spirv, "translate", side_effect=fake_translate), \
             mock.patch.object(rdna2spirv, "assemble_spirv", side_effect=fake_assemble), \
             mock.patch.object(subprocess, "run", side_effect=fake_subproc):
            out64 = pr.run_on_silicon("k64", ["v_mov_b32 v1, 2"], wave_size=64, user_sgprs=[0x44003C00, 0x12])

        self.assertEqual(out64, tuple(range(100, 164)))
        self.assertEqual(struct.unpack("<4I", captured["fssm"][0x138:]), (0x8090, 64, 1, 1))
        self.assertIn("--require-wave64", captured["cmd"])
        self.assertEqual(captured["env"]["VKC_SGPRS"], "0x44003c00,0x12")
        self.assertEqual(captured["env"]["VKC_OUT_WORDS"], "64")

    def test_run_on_silicon_nonzero_rc_returns_none(self):
        fail_proc = subprocess.CompletedProcess(["vk_compute_test.exe"], 77, stdout="no gpu", stderr="unavailable")
        with mock.patch.object(rdna2spirv, "translate", return_value="; spv\n"), \
             mock.patch.object(rdna2spirv, "assemble_spirv"), \
             mock.patch.object(subprocess, "run", return_value=fail_proc), \
             redirect_stdout(io.StringIO()) as buf:
            out = pr.run_on_silicon("k_fail", ["v_mov_b32 v1, 1"], wave_size=32)

        self.assertIsNone(out)
        self.assertIn("FAILED k_fail (rc=77)", buf.getvalue())

    def test_run_on_silicon_real_translate_cpu_smoke(self):
        body = [
            "v_mov_b32 v1, 0x1234",
            "v_mov_b32 v2, 0x1234",
            "v_cmp_eq_u16_e64 s4, v1, v2 op_sel:[0,0]",
            "v_and_b32_e32 v3, 1, s4",
            "v_lshlrev_b32_e32 v6, 2, v0",
            "buffer_store_dword v3, v6, s[0:3], 0 offen",
        ]

        def fake_assemble(sa_path, spv_path):
            spv_text = Path(sa_path).read_text(encoding="utf-8")
            self.assertIn("OpCapability", spv_text)
            Path(spv_path).write_bytes(b"SPV")

        def fake_subproc(cmd, env=None, capture_output=False, text=False):
            Path(env["VKC_DUMP"]).write_bytes(struct.pack("<32I", *([1] * 32)))
            return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")

        with mock.patch.object(rdna2spirv, "assemble_spirv", side_effect=fake_assemble), \
             mock.patch.object(subprocess, "run", side_effect=fake_subproc):
            out = pr.run_on_silicon("cs_real_translate_smoke", body, wave_size=32)

        self.assertEqual(out, (1,) * 32)


class ProbeFunctionsTests(unittest.TestCase):
    def test_all_probe_functions_pass_with_fake_silicon_oracle(self):
        oracle = FakeSiliconOracle()
        with mock.patch.object(pr, "run_on_silicon", side_effect=oracle), \
             redirect_stdout(io.StringIO()) as buf:
            pr.main()

        text = buf.getvalue()
        self.assertIn("ALL GFX1036 SILICON PROBES (WAVE32 & WAVE64) PASSED", text)
        waves_seen = {w for _, w, _ in oracle.calls}
        self.assertEqual(waves_seen, {32, 64})
        self.assertGreater(len(oracle.calls), 80)


class NegativeControlTests(unittest.TestCase):
    def test_unavailable_runner_raises_assertion_in_probe(self):
        with mock.patch.object(pr, "run_on_silicon", return_value=None):
            with self.assertRaises(AssertionError):
                pr.test_vcmp_inactive_lanes(32)

    def test_corrupted_inactive_lane_is_caught(self):
        oracle = FakeSiliconOracle(corrupt_inactive=True)
        with mock.patch.object(pr, "run_on_silicon", side_effect=oracle):
            with self.assertRaises(AssertionError) as ctx:
                pr.test_vcmp_inactive_lanes(32)
        self.assertIn("lane 1", str(ctx.exception))

    def test_corrupted_vcmpx_exec_is_caught(self):
        oracle = FakeSiliconOracle(corrupt_exec=True)
        with mock.patch.object(pr, "run_on_silicon", side_effect=oracle):
            with self.assertRaises(AssertionError) as ctx:
                pr.test_vcmpx_exec_update(32)
        self.assertIn("lane 1", str(ctx.exception))

    def test_wrong_opsel_bit_is_caught_in_constants_and_sgpr(self):
        oracle = FakeSiliconOracle(flip_opsel=True)
        with mock.patch.object(pr, "run_on_silicon", side_effect=oracle):
            with self.assertRaises(AssertionError):
                pr.test_constants_and_literals(32)
            with self.assertRaises(AssertionError):
                pr.test_sgpr_high_halves(32)

    def test_corrupted_nan_ordered_result_is_caught(self):
        oracle = FakeSiliconOracle(corrupt_nan_ordered=True)
        with mock.patch.object(pr, "run_on_silicon", side_effect=oracle):
            with self.assertRaises(AssertionError):
                pr.test_f16_nan_and_modifiers(32)


if __name__ == "__main__":
    unittest.main()
