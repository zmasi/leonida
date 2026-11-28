"""Read back DATA0 from equal-offset LDS writes (RDNA 2 ISA §10.4.3)."""
import unittest
from pathlib import Path

import lds_run
from support import rdna2spirv


class LdsWrite2EqualOffsets(unittest.TestCase):
    def test_equal_offsets_use_only_data0(self):
        fixture = Path(__file__).with_name("fixtures") / "lds_write2_equal_offsets.asm"
        lines = [line for line in fixture.read_text(encoding="ascii").splitlines()
                 if line and not line.startswith(";")]
        # lds_run supplies the end instruction when translating the listing.
        memory = lds_run.run(lines[:-1], lanes=1)
        low, high = 0x11111111, 0x22222222
        for op, slot, expected in (
            ("ds_write2_b32", 0, (low,)),
            ("ds_write2st64_b32", 1, (low,)),
            ("ds_write2_b64", 2, (low, high)),
            ("ds_write2st64_b64", 4, (low, high)),
            ("ds_write2_b64 with default offsets", 6, (low, high)),
        ):
            with self.subTest(op=op):
                observed = tuple(lds_run.out(memory, 0, slot + word) for word in range(len(expected)))
                self.assertEqual(observed, expected)

    def test_equal_offsets_still_validate_both_data_operands(self):
        for instruction, error in (
            ("ds_write2_b64 v1, v[4:5], v6", "data"),
            ("ds_write2st64_b64 v1, v[4:5]", "operands"),
            ("ds_write2_b32 v1, v4, v[6:7]", "64-bit operand"),
        ):
            with self.subTest(instruction=instruction), self.assertRaisesRegex(rdna2spirv.Unsupported, error):
                lds_run.translate([f"{instruction} offset0:2 offset1:2"])


if __name__ == "__main__":
    unittest.main()
