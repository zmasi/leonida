"""Tests for GetDiskFreeSpace and GetDiskFreeSpaceEx shims in file_system.cpp (RT-FS-DISK-FREE-SPACE)."""
import os
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class FileSystemDiskFreeSpaceTest(unittest.TestCase):
    def test_disk_free_space_shims_present(self):
        fs_path = os.path.join(ROOT, "runtime", "shims", "file_system.cpp")
        self.assertTrue(os.path.exists(fs_path), "file_system.cpp must exist")
        with open(fs_path, "r", encoding="utf-8") as f:
            content = f.read()

        for symbol in [
            "get_disk_free_space_ex_w",
            "get_disk_free_space_ex_a",
            "get_disk_free_space_w",
            "get_disk_free_space_a",
            '"GetDiskFreeSpaceA"',
            '"GetDiskFreeSpaceW"',
            '"GetDiskFreeSpaceExA"',
            '"GetDiskFreeSpaceExW"',
        ]:
            self.assertIn(symbol, content, f"Expected {symbol} in file_system.cpp")


if __name__ == "__main__":
    unittest.main()
