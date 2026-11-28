"""Value regressions for RDNA2's binary64 round-to-integral instructions."""
import contextlib
import math
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import support
import spirv_mini_interp


def words(value):
    return struct.unpack("<II", struct.pack("<d", value))


def fixture():
    path = Path(__file__).with_name("fixtures") / "ps_f64_round.asm"
    return support.translate_lines(path.stem, path.read_text(encoding="ascii").splitlines())


def run_op(op, value, *, inactive=False):
    low, high = words(value)
    return support.mrt0_words(support.translate_lines("ps_f64_case", [
        f"v_mov_b32 v0, {low:#x}", f"v_mov_b32 v1, {high:#x}",
        "v_mov_b32 v4, 0x11223344", "v_mov_b32 v5, 0x55667788",
        *(["s_mov_b32 exec_lo, 0"] if inactive else []),
        f"{op} v[4:5], v[0:1]",
        *(["s_mov_b32 exec_lo, -1"] if inactive else []),
        "exp mrt0 v4, v5, v4, v5 done vm", "s_endpgm",
    ]))[:2]


def run_bits(instruction, bits):
    """Run one instruction that rounds the pair v[0:1] = `bits` into v[4:5]; return v[4:5] as one 64-bit pattern."""
    low, high = support.mrt0_words(support.translate_lines("ps_f64_case", [
        f"v_mov_b32 v0, {bits & 0xFFFFFFFF:#x}", f"v_mov_b32 v1, {bits >> 32:#x}",
        instruction, "exp mrt0 v4, v5, v4, v5 done vm", "s_endpgm",
    ]))[:2]
    return low | high << 32


@contextlib.contextmanager
def driver_that_quiets_and_flushes():
    """The interpreter's double-precision Trunc, Ceil, RoundEven and Floor, replaced by what Vulkan allows a driver
    and what two of them were measured to do (cs_w32_round_f64): a NaN operand comes back with its quiet bit set,
    sign and payload kept (NVIDIA 620.70, llvmpipe), and a subnormal operand is read as a zero of its sign
    (llvmpipe; the module declares no DenormPreserve for 64 bits). Every other operand is rounded as before. Yields
    the list of operands the four functions were called with."""
    calls = []

    def driver(function):
        def rounded(x):
            calls.append(x)
            if x != x:
                bits = struct.unpack("<Q", struct.pack("<d", x))[0]
                return struct.unpack("<d", struct.pack("<Q", bits | 1 << 51))[0]
            if x != 0.0 and abs(x) < 2.0 ** -1022:
                x = math.copysign(0.0, x)
            return function(x)
        return rounded

    class Math:
        """The math module as the interpreter sees it: the three functions above, and a NaN is handed to them
        instead of being passed through."""
        floor, ceil, trunc = (staticmethod(driver(f)) for f in (math.floor, math.ceil, math.trunc))

        @staticmethod
        def isfinite(x):
            return not math.isinf(x)

        def __getattr__(self, name):
            return getattr(math, name)

    with patch.object(spirv_mini_interp, "math", Math()), \
            patch.object(spirv_mini_interp, "round", driver(round), create=True):
        yield calls


class F64RoundSpecialSources(unittest.TestCase):
    """A NaN and a subnormal are rounded by the translated code, not by the driver's function (SHADER-ROUND-F64-
    SPECIALS): under a driver that quiets the one and reads the other as zero, the words are still the hardware's."""
    OPS = ("v_trunc_f64", "v_ceil_f64", "v_rndne_f64", "v_floor_f64")
    SIGN, ONE = 1 << 63, 0x3FF0000000000000

    def test_the_driver_model_is_what_the_fixture_measured(self):
        with driver_that_quiets_and_flushes() as calls:
            functions = spirv_mini_interp.math
            self.assertEqual(struct.pack("<d", functions.trunc(struct.unpack("<d", struct.pack("<Q", 0x7FF0000000000001))[0])),
                             struct.pack("<Q", 0x7FF8000000000001))
            self.assertEqual(functions.ceil(5e-324), 0)             # read as +0: the hardware's answer is 1
            self.assertEqual(functions.floor(-5e-324), 0)           # read as -0: the hardware's answer is -1
            self.assertEqual(functions.ceil(2.0 ** -1022), 1)       # the smallest normal is not flushed
            self.assertEqual(spirv_mini_interp.round(2.5), 2)
            self.assertEqual(len(calls), 5)
        self.assertIs(spirv_mini_interp.math, math)

    def test_a_nan_comes_back_unchanged(self):
        nans = (0x7FF8000000000000, 0xFFF8000000000000, 0x7FF0000000000001, 0xFFF0000080000000, 0x7FF8DEADBEEF1234,
                0x7FF4000000000000)
        with driver_that_quiets_and_flushes() as calls:
            for op in self.OPS:
                for bits in nans:
                    with self.subTest(op=op, bits=hex(bits)):
                        self.assertEqual(run_bits(f"{op} v[4:5], v[0:1]", bits), bits)
            # A source modifier works on the sign bit first, and the NaN that leaves it comes back unchanged.
            self.assertEqual(run_bits("v_trunc_f64_e64 v[4:5], -v[0:1]", 0x7FF0000000000001), 0xFFF0000000000001)
            self.assertEqual(run_bits("v_ceil_f64_e64 v[4:5], |v[0:1]|", 0xFFF0000080000000), 0x7FF0000080000000)
        self.assertTrue(calls, "the driver's function still ran; its answer for a NaN is what is not used")

    def test_a_subnormal_is_rounded_as_the_hardware_does(self):
        smallest, largest = 0x0000000000000001, 0x000FFFFFFFFFFFFF
        want = {"v_trunc_f64": (0, self.SIGN), "v_rndne_f64": (0, self.SIGN),
                "v_ceil_f64": (self.ONE, self.SIGN), "v_floor_f64": (0, self.SIGN | self.ONE)}
        with driver_that_quiets_and_flushes():
            for op, (positive, negative) in want.items():
                for bits in (smallest, largest):
                    with self.subTest(op=op, bits=hex(bits)):
                        self.assertEqual(run_bits(f"{op} v[4:5], v[0:1]", bits), positive)
                        self.assertEqual(run_bits(f"{op} v[4:5], v[0:1]", bits | self.SIGN), negative)
            # Source modifiers first: floor of -(+subnormal) is -1 and ceil of |-subnormal| is 1.
            self.assertEqual(run_bits("v_floor_f64_e64 v[4:5], -v[0:1]", smallest), self.SIGN | self.ONE)
            self.assertEqual(run_bits("v_ceil_f64_e64 v[4:5], |v[0:1]|", smallest | self.SIGN), self.ONE)
            # Of the output modifiers only `clamp` then works on that result (the omod is dropped, docs/design.md,
            # "v_add_f64 output modifiers"): ceil of the smallest subnormal stays 1 under mul:2, and floor of the
            # smallest negative one, -1, clamps to 0.
            self.assertEqual(run_bits("v_ceil_f64_e64 v[4:5], v[0:1] mul:2", smallest), self.ONE)
            self.assertEqual(run_bits("v_floor_f64_e64 v[4:5], v[0:1] clamp", smallest | self.SIGN), 0)

    def test_every_other_source_still_goes_through_the_driver_s_function(self):
        cases = (("v_trunc_f64", -0.75, -0.0), ("v_ceil_f64", -0.25, -0.0), ("v_rndne_f64", 2.5, 2.0),
                 ("v_floor_f64", 2.0 ** -1022, 0.0), ("v_ceil_f64", 2.0 ** -1022, 1.0),
                 ("v_floor_f64", -math.inf, -math.inf), ("v_rndne_f64", 2.0 ** 52 + 1, 2.0 ** 52 + 1),
                 ("v_trunc_f64", 0.0, 0.0), ("v_ceil_f64", -0.0, -0.0))
        with driver_that_quiets_and_flushes() as calls:
            for op, value, expected in cases:
                with self.subTest(op=op, value=value):
                    self.assertEqual(tuple(run_op(op, value)), words(expected))
        # The interpreter hands every finite operand to the function; it passes an infinity through itself.
        self.assertEqual(len(calls), sum(math.isfinite(value) for _, value, _ in cases))


class F64RoundTests(unittest.TestCase):
    def test_fixture(self):
        text = fixture()
        self.assertIn("OpCapability Float64", text)
        result = support.run_fragment(text)
        self.assertEqual(tuple(result["%out_mrt0"]), (0, 0xBFF00000, 0, 0xC0000000))
        self.assertEqual(tuple(result["%out_mrt1"]), (0, 0xC0000000, 0, 0xC0000000))

    def test_binary64_precision_and_ties(self):
        cases = (
            ("v_trunc_f64", -0.75, -0.0),
            ("v_ceil_f64", -0.25, -0.0),
            ("v_floor_f64", math.nextafter(1.0, 0.0), 0.0),
            ("v_ceil_f64", math.nextafter(1.0, 2.0), 2.0),
            ("v_trunc_f64", 2**40 + 1.75, float(2**40 + 1)),
            ("v_rndne_f64", 2.5, 2.0),
            ("v_rndne_f64", 3.5, 4.0),
            ("v_rndne_f64", -3.5, -4.0),
            ("v_rndne_f64", -0.5, -0.0),
        )
        for op, value, want in cases:
            with self.subTest(op=op, value=value):
                self.assertEqual(tuple(run_op(op, value)), words(want))

    def test_vop3_modifiers(self):
        # clamp applies; mul:2/mul:4/div:2 do nothing on RDNA2 while f64 denormals are kept (docs/design.md,
        # "v_add_f64 output modifiers"): trunc(|-2.75|) = 2.0 clamped to 1.0, floor(-(-2.75)) = 2.0 unscaled.
        low, high = words(-2.75)
        text = support.translate_lines("ps_f64_mod", [
            f"v_mov_b32 v0, {low:#x}", f"v_mov_b32 v1, {high:#x}",
            "v_trunc_f64_e64 v[2:3], |v[0:1]| clamp div:2",
            "v_floor_f64_e64 v[4:5], -v[0:1] mul:2",
            "exp mrt0 v2, v3, v4, v5 done vm", "s_endpgm",
        ])
        self.assertEqual(tuple(support.mrt0_words(text)), words(1.0) + words(2.0))

    def test_vop3_modifiers_on_the_other_three(self):
        # The case above puts an omod on v_floor_f64. A `mul:2` on each of the other three is what separates them
        # from main: trunc(|-2.75|) = 2.0, ceil(-2.75) = -2.0, rndne(2.5) = 2.0, none of them scaled. A scale would
        # give 4.0, -4.0 and 4.0, and clamp is left out so nothing hides the difference.
        low, high = words(-2.75)
        cases = (
            ("v_trunc_f64_e64 v[2:3], |v[0:1]| mul:2", 2.0),
            ("v_ceil_f64_e64 v[2:3], v[0:1] mul:2", -2.0),
            ("v_rndne_f64_e64 v[2:3], 0x40040000 mul:4", 2.0),
        )
        for instruction, want in cases:
            with self.subTest(instruction=instruction):
                text = support.translate_lines("ps_f64_mod", [
                    f"v_mov_b32 v0, {low:#x}", f"v_mov_b32 v1, {high:#x}",
                    instruction,
                    "exp mrt0 v2, v3 done vm", "s_endpgm",
                ])
                self.assertEqual(tuple(support.mrt0_words(text)), tuple(words(want)) + (0, 0))

    def test_rejects_bad_operand_forms(self):
        for instruction in ("v_floor_f64 v4, v[0:1]", "v_round_f64 v[4:5], v[0:1]",
                            "v_trunc_f64 v[4:5], v0"):
            with self.subTest(instruction=instruction):
                with self.assertRaises(support.rdna2spirv.Unsupported):
                    support.translate_lines("ps_f64_bad", [instruction, "s_endpgm"])

    def test_inline_float_and_exec(self):
        text = support.translate_lines("ps_f64_inline", [
            "v_ceil_f64_e32 v[0:1], 0.5",
            "v_floor_f64_e32 v[2:3], -0.5",
            "exp mrt0 v0, v1, v2, v3 done vm", "s_endpgm",
        ])
        self.assertEqual(tuple(support.mrt0_words(text)), words(1.0) + words(-1.0))
        self.assertEqual(run_op("v_rndne_f64", 3.5, inactive=True), [0x11223344, 0x55667788])

    @unittest.skipUnless(shutil.which("spirv-as") and shutil.which("spirv-val"), "SPIRV-Tools not installed")
    def test_spirv_validates(self):
        with tempfile.TemporaryDirectory() as directory:
            asm = Path(directory) / "shader.spvasm"
            binary = asm.with_suffix(".spv")
            asm.write_text(fixture(), encoding="ascii")
            subprocess.run(["spirv-as", "--target-env", "vulkan1.2", str(asm), "-o", str(binary)], check=True)
            subprocess.run(["spirv-val", "--target-env", "vulkan1.2", str(binary)], check=True)
