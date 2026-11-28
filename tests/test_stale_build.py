"""tools/stale_build.py: which sources of a runtime/test program are newer than its built copy.

    python3 -I tests/test_stale_build.py

Standard library only. Every case builds a small source tree in a temporary folder and sets the modification times
explicitly, so the results do not depend on how fast the files were written. No game data.
"""
import os
import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))
import stale_build  # noqa: E402

BASE = 1_700_000_000
OLD = BASE + 100     # the built copy
NEW = BASE + 1000    # the sources


def write(root, rel, text=""):
    path = os.path.join(root, *rel.split("/"))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return path


def set_mtime(path, when):
    os.utime(path, (when, when))


def rel(root, paths):
    return sorted(os.path.relpath(p, root).replace(os.sep, "/") for p in paths)


class StaleBuildTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = self.tmp.name
        # prog.cpp: a header beside it, a header in runtime/src through "../src", and one that does not exist.
        self.cpp = write(self.root, "runtime/test/prog.cpp",
                         '#include "helper.h"\n#include "../src/x.h"\n#include "missing.h"\nint main() {}\n')
        self.helper = write(self.root, "runtime/test/helper.h", "#pragma once\n")
        self.x = write(self.root, "runtime/src/x.h", "#pragma once\n")
        self.exe = os.path.join(self.root, "build", "prog")
        os.makedirs(os.path.dirname(self.exe))
        write(self.root, "build/prog", "binary")
        for path in (self.cpp, self.helper, self.x):
            set_mtime(path, NEW)

    def build(self, when):
        set_mtime(self.exe, when)

    def test_program_sources_follows_includes_and_skips_missing(self):
        found = stale_build.program_sources(self.root, "prog")
        self.assertEqual(rel(self.root, found),
                         ["runtime/src/x.h", "runtime/test/helper.h", "runtime/test/prog.cpp"])

    def test_stale_exe_lists_the_newer_sources(self):
        self.build(OLD)
        self.assertEqual(sorted(stale_build.newer_sources(self.exe, self.root, "prog")),
                         ["runtime/src/x.h", "runtime/test/helper.h", "runtime/test/prog.cpp"])

    def test_only_the_touched_header_is_reported(self):
        self.build(BASE + 500)
        set_mtime(self.helper, BASE + 600)
        set_mtime(self.cpp, BASE + 400)
        set_mtime(self.x, BASE + 400)
        self.assertEqual(stale_build.newer_sources(self.exe, self.root, "prog"), ["runtime/test/helper.h"])

    def test_fresh_exe_lists_nothing(self):
        self.build(BASE + 2000)
        self.assertEqual(stale_build.newer_sources(self.exe, self.root, "prog"), [])

    def test_missing_exe_lists_nothing(self):
        self.assertEqual(stale_build.newer_sources(os.path.join(self.root, "build", "absent"),
                                                   self.root, "prog"), [])

    def test_missing_exe_path_outside_any_tree(self):
        self.assertEqual(stale_build.newer_sources("/nonexistent/exe", self.root, "prog"), [])

    def test_program_without_source_lists_nothing(self):
        self.build(OLD)
        self.assertEqual(stale_build.program_sources(self.root, "no_such_program"), [])
        self.assertEqual(stale_build.newer_sources(self.exe, self.root, "no_such_program"), [])

    def test_no_runtime_folder_at_all(self):
        empty = tempfile.TemporaryDirectory()
        self.addCleanup(empty.cleanup)
        self.assertEqual(stale_build.program_sources(empty.name, "prog"), [])

    def test_empty_source_file_is_a_source(self):
        empty = write(self.root, "runtime/test/empty.cpp", "")
        set_mtime(empty, NEW)
        self.build(OLD)
        self.assertEqual(rel(self.root, stale_build.program_sources(self.root, "empty")),
                         ["runtime/test/empty.cpp"])
        self.assertEqual(stale_build.newer_sources(self.exe, self.root, "empty"), ["runtime/test/empty.cpp"])

    def test_include_cycle_terminates_and_lists_each_file_once(self):
        write(self.root, "runtime/test/cyc.cpp", '#include "a.h"\n')
        write(self.root, "runtime/test/a.h", '#include "b.h"\n')
        write(self.root, "runtime/test/b.h", '#include "a.h"\n')
        found = stale_build.program_sources(self.root, "cyc")
        self.assertEqual(rel(self.root, found), ["runtime/test/a.h", "runtime/test/b.h", "runtime/test/cyc.cpp"])

    def test_header_included_twice_is_listed_once(self):
        write(self.root, "runtime/test/dup.cpp",
              '#include "helper.h"\n#include "helper.h"\n#include "../test/helper.h"\n')
        found = stale_build.program_sources(self.root, "dup")
        self.assertEqual(rel(self.root, found), ["runtime/test/dup.cpp", "runtime/test/helper.h"])

    def test_directory_named_like_an_include_is_skipped(self):
        os.makedirs(os.path.join(self.root, "runtime", "test", "isdir.h"))
        write(self.root, "runtime/test/dirinc.cpp", '#include "isdir.h"\n')
        found = stale_build.program_sources(self.root, "dirinc")
        self.assertEqual(rel(self.root, found), ["runtime/test/dirinc.cpp"])

    def test_exe_mtime_equal_to_source_is_not_stale(self):
        # newer_sources compares with ">", so a source whose mtime equals the exe's is not reported. This pins that
        # behaviour: a same-second build of an edited source is taken as current.
        self.build(NEW)
        self.assertEqual(stale_build.newer_sources(self.exe, self.root, "prog"), [])

    def test_exe_one_second_older_than_one_source_is_stale(self):
        self.build(NEW - 1)
        self.assertEqual(sorted(stale_build.newer_sources(self.exe, self.root, "prog")),
                         ["runtime/src/x.h", "runtime/test/helper.h", "runtime/test/prog.cpp"])

    def test_commented_include_is_not_followed(self):
        write(self.root, "runtime/test/cmt.cpp", '// #include "hidden.h"\n#include "helper.h"\n')
        write(self.root, "runtime/test/hidden.h", "")
        found = stale_build.program_sources(self.root, "cmt")
        self.assertEqual(rel(self.root, found), ["runtime/test/cmt.cpp", "runtime/test/helper.h"])

    def test_block_comment_and_raw_string_includes_ignored_while_inline_comment_prefix_followed(self):
        write(self.root, "runtime/test/hidden_blk.h", "")
        write(self.root, "runtime/test/hidden_raw.h", "")
        write(self.root, "runtime/test/after_str.h", "")
        write(
            self.root,
            "runtime/test/comments.cpp",
            '/*\n#include "hidden_blk.h"\n*/\n'
            'const char *s = R"(\n#include "hidden_raw.h"\n)";\n'
            'const char *open_blk = "/*";\n'
            '#include "after_str.h"\n'
            '/* note */ #include "helper.h"\n',
        )
        found = stale_build.program_sources(self.root, "comments")
        self.assertEqual(
            rel(self.root, found),
            ["runtime/test/after_str.h", "runtime/test/comments.cpp", "runtime/test/helper.h"],
        )

    def test_utf8_bom_first_line_include_is_followed(self):
        bom_cpp = os.path.join(self.root, "runtime", "test", "bom.cpp")
        with open(bom_cpp, "wb") as f:
            f.write(b'\xef\xbb\xbf#include "helper.h"\nint main() {}\n')
        found = stale_build.program_sources(self.root, "bom")
        self.assertEqual(rel(self.root, found), ["runtime/test/bom.cpp", "runtime/test/helper.h"])

    def test_backslash_include_resolves_and_absolute_include_is_ignored(self):
        write(self.root, "runtime/test/sub/nested.h", "#pragma once\n")
        outside = write(self.root, "outside.h", "#pragma once\n")
        abs_inc = outside.replace("\\", "/")
        write(
            self.root,
            "runtime/test/paths.cpp",
            f'#include "sub\\nested.h"\n#include "{abs_inc}"\n',
        )
        found = stale_build.program_sources(self.root, "paths")
        self.assertEqual(rel(self.root, found), ["runtime/test/paths.cpp", "runtime/test/sub/nested.h"])

    def test_sub_ulp_nanosecond_mtime_difference_is_detected(self):
        base_ns = BASE * 1_000_000_000
        os.utime(self.exe, ns=(base_ns, base_ns))
        os.utime(self.cpp, ns=(base_ns, base_ns))
        os.utime(self.x, ns=(base_ns, base_ns))
        os.utime(self.helper, ns=(base_ns + 100, base_ns + 100))
        if os.stat(self.helper).st_mtime_ns <= os.stat(self.exe).st_mtime_ns:
            self.skipTest("filesystem does not preserve 100ns mtime precision")
        # 100 ns is below float64 ULP at 1.7e9 (~238 ns), so float getmtime rounds both equal.
        self.assertEqual(os.path.getmtime(self.helper), os.path.getmtime(self.exe))
        self.assertEqual(stale_build.newer_sources(self.exe, self.root, "prog"), ["runtime/test/helper.h"])


if __name__ == "__main__":
    unittest.main()
