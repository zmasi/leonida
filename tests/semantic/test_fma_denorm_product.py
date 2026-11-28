"""FMA family on a product below 2^-126 (TEST-FMA-DENORM-PRODUCT, #1594): what each --fma mode emits.

RDNA2's v_fma_f32, v_fmac_f32, v_fmaak_f32 and v_fmamk_f32 round once and keep the exact product, so x * y + z with
x * y = -2^-127 and z = 0x00ffffff is the normal 0x00bfffff. On an RTX 4070 SUPER, v_fmamk_f32 gave the addend
0x00ffffff instead (fuzz-found by whoalin1, #14 comments 7150696174 (a) and 7152027316 (c); AMD's Vulkan driver gave
the hardware word): the driver flushed the denormal intermediate product. Nothing in the tree pinned that mix
(cs_fma_fused probes rounding, on normal products).

tests/semantic/fixtures/ps_fma_denorm_product.asm runs the four ops on push-constant operands. The interpreter runs the
translated module under three models of the driver's fused op (GLSL.std.450 Fma and OpFmaKHR alike):

  fused     exact product, one rounding: the interpreter's own si.fma_f32, and the hardware's word;
  flushing  a non-zero product below 2^-126 becomes a zero of its sign before the add: NVIDIA's word;
  poisoned  a NaN for every finite operand triple, so a word that is still right never came from that op.

--fma glsl and --fma khr emit one fused op per instruction, so the word is whatever the driver's op returns, and the
flushing model reproduces NVIDIA's 0x00ffffff with no GPU. SPV_KHR_fma calls OpFmaKHR correctly rounded, but nobody has
measured whether NVIDIA's OpFmaKHR keeps the product. This file only pins that khr leaves the product to that one op.
--fma exact builds the product from NoContraction f32 operations with the operands scaled by 2^96, so no intermediate
is a denormal a driver could flush. Its word is the fused one under every model, with or without every f32 denormal
result flushed. Its GLSL Fma stays in the module, selected only for infinite or NaN operands.

Standard library only, hand-written fixture, no game data.
"""
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from fractions import Fraction
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TOOLS_DIR = os.environ.get("LEONIDA_TOOLS_DIR") or os.path.join(ROOT, "tools")
sys.path.insert(0, TOOLS_DIR)
sys.path.insert(0, HERE)

import rdna2spirv  # noqa: E402
import spirv_mini_interp as si  # noqa: E402
from support import mrt0_words, translate_lines  # noqa: E402

FIXTURE = Path(HERE) / "fixtures" / "ps_fma_denorm_product.asm"
MODES = ("glsl", "khr", "exact")
FUSED = si.fma_f32
KEEP_F32 = si.f32
MIN_NORMAL = Fraction(1, 2 ** 126)
FMAMK_K, FMAAK_K = 0x20000000, 0x00FFFFFF          # the fixture's literals: 2^-63 and fmaak's addend

# (x, y, z) push-constant words -> the mrt0 words (fmamk x*K+z, fmaak x*y+0x00ffffff, fma x*y+z, fmac x*y+z) with the
# product kept, and with it flushed. y is K, so all four ops see one product.
CASES = {
    # x*y = -2^-127, whoalin1's mix: (2^24 - 1 - 2^22) * 2^-149 = 0x00bfffff exactly. Flushed: the addend.
    "product -2^-127": ((0x9F800000, 0x20000000, 0x00FFFFFF), (0x00BFFFFF,) * 4, (0x00FFFFFF,) * 4),
    # x*y = +2^-130 on z = 2^-126: 0x00880000 exactly. On fmaak's 0x00ffffff the sum (2^24 - 1 + 2^19) * 2^-149 is a
    # tie between steps of 2^-148, to even: 0x01040000. Flushed: the addends.
    "product +2^-130": ((0x1E000000, 0x20000000, 0x00800000), (0x00880000, 0x01040000, 0x00880000, 0x00880000),
                        (0x00800000, 0x00FFFFFF, 0x00800000, 0x00800000)),
    # Control: x*y = 1.5 * 2^-63 is normal and z is far below its half ulp, so no model moves the word.
    "normal product": ((0x3FC00000, 0x20000000, 0x00FFFFFF), (0x20400000,) * 4, (0x20400000,) * 4),
}


def fma_flushing(x, y, z):
    """The driver's fused op with a non-zero product below 2^-126 flushed to a zero of its sign before the add."""
    if all(math.isfinite(v) for v in (x, y, z)):
        p = Fraction(x) * Fraction(y)
        if p and abs(p) < MIN_NORMAL:
            return FUSED(-0.0 if p < 0 else 0.0, 1.0, z)
    return FUSED(x, y, z)


def fma_poisoned(x, y, z):
    """A driver op that is wrong for every finite operand triple."""
    if all(math.isfinite(v) for v in (x, y, z)):
        return math.nan
    return FUSED(x, y, z)


def f32_ftz(v):
    """Binary32 rounding on a driver that flushes every denormal f32 result to a zero of its sign."""
    r = KEEP_F32(v)
    return math.copysign(0.0, r) if r != 0.0 and abs(r) < 2.0 ** -126 else r


def translate(mode):
    old = rdna2spirv.FMA_KHR, rdna2spirv.FMA_EXACT
    rdna2spirv.FMA_KHR, rdna2spirv.FMA_EXACT = mode == "khr", mode == "exact"
    try:
        return translate_lines(FIXTURE.stem, FIXTURE.read_text(encoding="ascii").splitlines())
    finally:
        rdna2spirv.FMA_KHR, rdna2spirv.FMA_EXACT = old


def run(spv, xyz, fma=FUSED, ftz=False):
    """The four mrt0 words for push constants x, y, z, with the interpreter's fused op replaced by `fma`."""
    si.fma_f32, si.f32 = fma, f32_ftz if ftz else KEEP_F32
    try:
        return mrt0_words(spv, push_constants=list(xyz) + [0] * 29)
    finally:
        si.fma_f32, si.f32 = FUSED, KEEP_F32


def hexs(words):
    return [f"0x{w:08x}" for w in words]


def value(word):
    """The exact value of a finite binary32 word."""
    sign, exp, frac = word >> 31, (word >> 23) & 0xFF, word & 0x7FFFFF
    mag = Fraction(frac, 2 ** 149) if exp == 0 else Fraction(0x800000 | frac, 2 ** 23) * Fraction(2) ** (exp - 127)
    return -mag if sign else mag


class PinnedWords(unittest.TestCase):
    """The expected words in CASES, checked against exact rationals rather than against the interpreter."""

    def test_fused_words_are_the_once_rounded_sums(self):
        for name, ((x, y, z), fused, _) in CASES.items():
            sums = (value(x) * value(FMAMK_K) + value(z), value(x) * value(y) + value(FMAAK_K),
                    value(x) * value(y) + value(z), value(x) * value(y) + value(z))
            for op, exact, word in zip(("fmamk", "fmaak", "fma", "fmac"), sums, fused):
                with self.subTest(case=name, op=op):
                    off = abs(exact - value(word))
                    half_down, half_up = (value(word) - value(word - 1)) / 2, (value(word + 1) - value(word)) / 2
                    self.assertLessEqual(off, half_down if exact < value(word) else half_up)
                    if off in (half_down, half_up):
                        self.assertEqual(word & 1, 0, "a tie rounds to the even word")

    def test_products_are_below_the_normal_range_except_the_control(self):
        for name, ((x, y, _), _, _) in CASES.items():
            with self.subTest(case=name):
                self.assertEqual(y, FMAMK_K)
                self.assertEqual(abs(value(x) * value(y)) < MIN_NORMAL, name != "normal product")

    def test_flushed_words_are_the_addends(self):
        for name, ((x, y, z), fused, flushed) in CASES.items():
            with self.subTest(case=name):
                if name == "normal product":
                    self.assertEqual(flushed, fused)
                else:
                    self.assertEqual(flushed, (z, FMAAK_K, z, z))


class EmittedOps(unittest.TestCase):
    """What each --fma mode puts in the module for the four ops."""

    @classmethod
    def setUpClass(cls):
        cls.spv = {mode: translate(mode) for mode in MODES}

    def test_glsl_is_one_glsl_fma_per_op(self):
        spv = self.spv["glsl"]
        self.assertEqual(spv.count("OpExtInst %float %glsl Fma "), 4)
        self.assertNotIn("OpFmaKHR", spv)
        self.assertNotIn("FMAKHR", spv)
        self.assertNotIn("SPV_KHR_fma", spv)

    def test_khr_is_one_opfmakhr_per_op(self):
        spv = self.spv["khr"]
        self.assertEqual(spv.count("OpFmaKHR %float "), 4)
        self.assertNotIn("%glsl Fma", spv)
        self.assertIn("OpCapability FMAKHR", spv)
        self.assertIn('OpExtension "SPV_KHR_fma"', spv)

    def test_exact_product_is_no_contraction_f32_arithmetic(self):
        spv = self.spv["exact"]
        self.assertNotIn("OpFmaKHR", spv)
        arith = re.findall(r"^\s*(%\w+) = Op(?:FMul|FAdd|FSub) %float ", spv, re.M)
        decorated = set(re.findall(r"OpDecorate (%\w+) NoContraction", spv))
        self.assertGreater(len(arith), 4 * 20)
        self.assertEqual([i for i in arith if i not in decorated], [])

    def test_exact_glsl_fma_only_feeds_selects(self):
        spv = self.spv["exact"]
        fmas = re.findall(r"^\s*(%\w+) = OpExtInst %float %glsl Fma ", spv, re.M)
        self.assertEqual(len(fmas), 4)
        for g in fmas:
            uses = [ln.strip() for ln in spv.splitlines() if re.search(rf"{re.escape(g)}\b", ln)
                    and not ln.strip().startswith(f"{g} =")]
            with self.subTest(fma=g):
                self.assertTrue(uses)
                self.assertEqual([u for u in uses if " = OpSelect %float " not in u], [])

    @unittest.skipUnless(shutil.which("spirv-as") and shutil.which("spirv-val"), "SPIRV-Tools not installed")
    def test_spirv_tools_accept_every_mode(self):
        # As test_fma_khr: an older SPIRV-Tools without SPV_KHR_fma skips khr unless LEONIDA_REQUIRE_SPIRV_TOOLS=1.
        require_tools = os.environ.get("LEONIDA_REQUIRE_SPIRV_TOOLS") == "1"
        with tempfile.TemporaryDirectory() as tmp:
            for mode in MODES:
                with self.subTest(mode=mode):
                    asm, out = os.path.join(tmp, f"{mode}.spvasm"), os.path.join(tmp, f"{mode}.spv")
                    with open(asm, "w", encoding="utf-8") as f:
                        f.write(self.spv[mode])
                    made = subprocess.run(["spirv-as", asm, "-o", out, "--target-env", "vulkan1.2"],
                                          capture_output=True, text=True)
                    if (mode == "khr" and not require_tools and made.returncode
                            and made.stderr.rstrip().endswith("Invalid capability 'FMAKHR'.")):
                        continue
                    self.assertEqual(made.returncode, 0, made.stderr)
                    checked = subprocess.run(["spirv-val", "--target-env", "vulkan1.2",
                                              "--allow-offset-texture-operand", out], capture_output=True, text=True)
                    if (mode == "khr" and not require_tools and checked.returncode
                            and checked.stderr.rstrip().endswith("Invalid capability operand: 6030")):
                        continue
                    self.assertEqual(checked.returncode, 0, checked.stderr)


class Words(unittest.TestCase):
    """The mrt0 words of each mode under each model of the driver's fused op."""

    @classmethod
    def setUpClass(cls):
        cls.spv = {mode: translate(mode) for mode in MODES}

    def check(self, mode, fma, ftz, want):
        for name, (xyz, fused, flushed) in CASES.items():
            with self.subTest(case=name, mode=mode, fma=fma.__name__, ftz=ftz):
                self.assertEqual(hexs(run(self.spv[mode], xyz, fma, ftz)), hexs(fused if want == "fused" else flushed))

    def test_fused_driver_gives_the_hardware_word_in_every_mode(self):
        for mode in MODES:
            self.check(mode, FUSED, False, "fused")

    def test_flushing_driver_gives_the_addend_under_glsl_and_khr(self):
        for mode in ("glsl", "khr"):
            self.check(mode, fma_flushing, False, "flushed")
            # whoalin1's word: v_fmamk_f32 with the -2^-127 product returns the addend.
            self.assertEqual(run(self.spv[mode], CASES["product -2^-127"][0], fma_flushing)[0], 0x00FFFFFF)

    def test_exact_keeps_the_product_under_a_flushing_driver(self):
        for ftz in (False, True):
            self.check("exact", fma_flushing, ftz, "fused")

    def test_exact_takes_no_finite_word_from_the_driver_fma(self):
        for ftz in (False, True):
            self.check("exact", fma_poisoned, ftz, "fused")


if __name__ == "__main__":
    unittest.main()
