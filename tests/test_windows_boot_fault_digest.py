"""Exercise the Windows boot digest on synthetic stderr without launching the runtime."""

from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest


BOOT = Path(__file__).resolve().parents[1] / "scripts" / "windows-boot" / "boot.ps1"
POWERSHELL = shutil.which("powershell") or shutil.which("pwsh")


@unittest.skipUnless(os.name == "nt" and POWERSHELL, "requires Windows PowerShell")
class FaultDigestTest(unittest.TestCase):
    def digest(self, lines):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "test.log").write_text("\n".join(lines), encoding="utf-8", newline="\n")
            (root / "test.jsonl").write_bytes(b"")
            subprocess.run(
                [POWERSHELL, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(BOOT),
                 "-DigestOnly", "-Name", "test", "-OutDir", folder, "-SummaryLines", "0",
                 "-Python", sys.executable],
                check=True, capture_output=True, timeout=60,
            )
            return (root / "test.digest.txt").read_text(encoding="utf-8-sig")

    def test_classes_after_checkpoints_and_private_fields(self):
        prefix = "[rt] device-fault: "
        lines = [prefix + "VK_ERROR_DEVICE_LOST from synthetic-private-operation"]
        lines += [prefix + "submitted dispatch seq=1 name=synthetic-private-stage"] * 10
        lines += [prefix + f"address[{i}] type={kind} address=0xdeadbeef precision=4096"
                  for i, kind in enumerate((1, 2, 2, 4, 99))]
        result = self.digest(lines)
        self.assertIn("device loss reported", result)
        self.assertIn("READ_INVALID=1, WRITE_INVALID=2, INSTRUCTION_POINTER_UNKNOWN=1, OTHER=1", result)
        self.assertNotIn("synthetic-private", result)
        self.assertNotIn("deadbeef", result)

    def test_normal_telemetry_is_not_a_loss(self):
        result = self.digest(["[rt] device-fault: enabled (synthetic-private)",
                              "[rt] device-fault: submitted dispatch seq=1 synthetic-private"])
        self.assertIn("faults:   none", result)
        self.assertNotIn("synthetic-private", result)

    def test_loss_without_address_info(self):
        result = self.digest(["[rt] a Vulkan call returned VK_ERROR_DEVICE_LOST"])
        self.assertIn("device loss reported", result)
        self.assertIn("address records: none", result)

    def test_runtime_loss_marker_without_the_vulkan_token(self):
        result = self.digest(["[rt] vulkan: the device was lost (synthetic-private-reason)"])
        self.assertIn("device loss reported, address records: none", result)
        self.assertNotIn("synthetic-private", result)

    def test_address_records_without_a_loss_marker(self):
        result = self.digest(["[rt] device-fault: address[0] type=4 address=0x11 precision=4"])
        self.assertIn("no device-loss marker, address records: INSTRUCTION_POINTER_UNKNOWN=1", result)


if __name__ == "__main__":
    unittest.main()
