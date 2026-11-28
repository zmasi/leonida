"""tools/bink_play.py: name matching and the name-table cache. Synthetic names only, no game files."""
import sys
from io import StringIO
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import bink_play  # noqa: E402

NAMES = [
    ("Stinger/INTRO_A.bk2", 4096, "3840", "2160", "450"),
    ("Stinger/INTRO_B.bk2", 8192, "3840", "2160", "300"),
    ("TV/News_01.bk2", 12288, "1280", "720", "40"),
]


class ListTest(unittest.TestCase):
    def test_filter_accepts_both_path_separators_and_ignores_case(self):
        from io import StringIO

        names = [("Clips/Demo.bk2", 4096, "640", "480", "24"), *NAMES]
        with mock.patch.object(Path, "is_file", return_value=True), \
                mock.patch.object(bink_play, "load_names", return_value=names):
            for query in ("Clips/Demo", r"Clips\Demo", "cLiPs/dEmO", r"cLiPs\dEmO"):
                with self.subTest(query=query), mock.patch.object(sys, "stdout", new_callable=StringIO) as stdout:
                    rc = bink_play.main(["--game-dir", "synthetic", "--list", query])
                    self.assertEqual(rc, 0)
                    self.assertEqual(stdout.getvalue(), "Clips/Demo.bk2  640x480, 24 frames\n")


class PickTest(unittest.TestCase):
    def test_exact_path_wins_case_insensitively_and_with_backslashes(self):
        hit, _ = bink_play.pick(NAMES, "stinger\\intro_a.BK2")
        self.assertEqual(hit[1], 4096)

    def test_ambiguous_exact_path_excludes_partial_matches(self):
        first = NAMES[0]
        duplicate = (first[0].lower(), 16384, *first[2:])
        partial = ("Alt/" + first[0], 20480, *first[2:])
        hit, candidates = bink_play.pick([first, partial, duplicate], r"Stinger\INTRO_A.bk2")
        self.assertIsNone(hit)
        self.assertEqual(candidates, [first, duplicate])

    def test_a_unique_part_of_the_name_is_enough(self):
        hit, _ = bink_play.pick(NAMES, "news")
        self.assertEqual(hit[0], "TV/News_01.bk2")

    def test_an_ambiguous_part_returns_the_candidates(self):
        hit, candidates = bink_play.pick(NAMES, "intro")
        self.assertIsNone(hit)
        self.assertEqual([c[0] for c in candidates], ["Stinger/INTRO_A.bk2", "Stinger/INTRO_B.bk2"])

    def test_no_match(self):
        self.assertEqual(bink_play.pick(NAMES, "credits"), (None, []))


class CacheTest(unittest.TestCase):
    def test_matching_but_damaged_cache_is_rebuilt_from_the_sources(self):
        with tempfile.TemporaryDirectory() as d:
            archive, mount, cache = (Path(d) / n for n in ("streams.csv", "mount.xml", "cache.csv"))
            archive.write_text("source,offset,size,width,height,frames\nsample,32,64,16,16,2\n",
                               encoding="utf-8", newline="\n")
            mount.write_text("<filename>$(common)/movies/synthetic.bk2</filename>",
                             encoding="utf-8", newline="\n")
            stamp = ("# " + bink_play.stamp(archive) + "|" + bink_play.stamp(mount) + "\n").encode()
            header = b"path,offset,width,height,frames\n"
            expected = [("synthetic.bk2", 32, "16", "16", "2")]
            with mock.patch.object(bink_play, "cache_path", return_value=cache):
                for damaged in (b"", b"wrong,columns\n", header + b"prefix,1,1,1,1\nbad,row\n",
                                header + b"x,garbled,1,1,1\n", header + b'x,1,1,1,"2\n',
                                header + b"\xff,1,1,1,2\n"):
                    with self.subTest(damaged=damaged):
                        cache.write_bytes(stamp + damaged)
                        self.assertEqual(bink_play.load_names(archive, mount), expected)
                        with mock.patch.object(bink_play.bink_names, "main", side_effect=AssertionError):
                            self.assertEqual(bink_play.load_names(archive, mount), expected)
                cache.write_bytes(stamp + header)
                with mock.patch.object(bink_play.bink_names, "main", side_effect=AssertionError):
                    self.assertEqual(bink_play.load_names(archive, mount), [])

    def test_the_table_is_named_once_and_rebuilt_when_the_archive_changes(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            archive, mount = d / "movies.rpf", d / "common.rpf"
            archive.write_bytes(b"x" * 16)
            mount.write_bytes(b"y")
            calls = []

            def fake_names(argv):
                calls.append(argv)
                out = Path(argv[argv.index("--out") + 1])
                out.write_text("source,offset,size,path,confidence,width,height,frames\n"
                               "movies.rpf,4096,10,Stinger/INTRO_A.bk2,anchored,3840,2160,450\n", encoding="utf-8")
                return 0

            with mock.patch.object(bink_play, "cache_path", return_value=d / "cache" / "bink_names.csv"), \
                    mock.patch.object(bink_play.bink_names, "main", side_effect=fake_names):
                first = bink_play.load_names(archive, mount)
                second = bink_play.load_names(archive, mount)
                self.assertEqual(len(calls), 1, "the second call reads the cache")
                self.assertEqual(first, second)
                self.assertEqual(first[0][:2], ("Stinger/INTRO_A.bk2", 4096))
                archive.write_bytes(b"x" * 32)  # a different archive size: the cache is stale
                bink_play.load_names(archive, mount)
                self.assertEqual(len(calls), 2)
                bink_play.load_names(archive, mount, use_cache=False)
                self.assertEqual(len(calls), 3, "--no-cache names again")

    def test_a_different_mount_source_names_the_streams_again(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            archive, old, new = d / "movies.rpf", d / "old.xml", d / "new.xml"
            archive.write_bytes(b"x" * 16)
            old.write_bytes(b"<old/>")
            new.write_bytes(b"<new/>")
            cache = d / "cache" / "bink_names.csv"

            def fake_names(argv):
                name = "OLD.bk2" if Path(argv[argv.index("--mount") + 1]).name == "old.xml" else "NEW.bk2"
                Path(argv[argv.index("--out") + 1]).write_text(
                    "source,offset,size,path,confidence,width,height,frames\n"
                    f"movies.rpf,4096,10,Stinger/{name},anchored,3840,2160,450\n", encoding="utf-8", newline="\n")
                return 0

            with mock.patch.object(bink_play, "cache_path", return_value=cache), \
                    mock.patch.object(bink_play.bink_names, "main", side_effect=fake_names):
                self.assertEqual(bink_play.load_names(archive, old)[0][0], "Stinger/OLD.bk2")
                self.assertEqual(bink_play.load_names(archive, new)[0][0], "Stinger/NEW.bk2",
                                 "names from another mount source are not served from the cache")
                # a table written before the mount source was part of the key is a miss, not a crash
                cache.write_text("# " + bink_play.stamp(archive) + "\npath,offset,width,height,frames\n"
                                 "Stinger/STALE.bk2,4096,3840,2160,450\n", encoding="utf-8", newline="\n")
                self.assertEqual(bink_play.load_names(archive, new)[0][0], "Stinger/NEW.bk2")

    def test_two_lookups_at_once_each_get_their_own_names(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            archive, first, second = d / "movies.rpf", d / "first.xml", d / "second.xml"
            archive.write_bytes(b"x" * 16)
            first.write_bytes(b"<a/>")
            second.write_bytes(b"<b/>")
            ready, done = threading.Event(), threading.Event()
            got = {}

            def fake_names(argv):
                mount = Path(argv[argv.index("--mount") + 1])
                Path(argv[argv.index("--out") + 1]).write_text(
                    "source,offset,size,path,confidence,width,height,frames\n"
                    f"movies.rpf,4096,10,Stinger/{mount.stem}.bk2,anchored,3840,2160,450\n",
                    encoding="utf-8", newline="\n")
                if mount == first:  # hold the first lookup between writing its report and reading it
                    ready.set()
                    done.wait(10)
                return 0

            def look_up(mount):
                try:
                    got[mount.stem] = bink_play.load_names(archive, mount)[0][0]
                except SystemExit as e:
                    got[mount.stem] = str(e)

            with mock.patch.object(bink_play, "cache_path", return_value=d / "cache" / "bink_names.csv"), \
                    mock.patch.object(bink_play.bink_names, "main", side_effect=fake_names):
                worker = threading.Thread(target=look_up, args=(first,))
                worker.start()
                self.assertTrue(ready.wait(10))
                try:
                    look_up(second)
                finally:
                    done.set()
                    worker.join()
            self.assertEqual(got, {"first": "Stinger/first.bk2", "second": "Stinger/second.bk2"})
            self.assertEqual(sorted(p.name for p in (d / "cache").iterdir()), ["bink_names.csv"])

    def test_load_names_closes_mkstemp_fd_on_open_error(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            archive, mount = d / "movies.rpf", d / "common.rpf"
            archive.write_bytes(b"x" * 16)
            mount.write_bytes(b"y")
            closed = []
            real_close = bink_play.os.close

            def track_close(fd):
                closed.append(fd)
                return real_close(fd)

            def fake_names(argv):
                out = Path(argv[argv.index("--out") + 1])
                out.write_text("source,offset,size,path,confidence,width,height,frames\n"
                               "movies.rpf,4096,10,Stinger/INTRO_A.bk2,anchored,3840,2160,450\n", encoding="utf-8")
                return 0

            with mock.patch.object(bink_play, "cache_path", return_value=d / "cache" / "bink_names.csv"), \
                 mock.patch.object(bink_play.bink_names, "main", side_effect=fake_names), \
                 mock.patch("bink_play.open", side_effect=OSError("open failure")), \
                 mock.patch("bink_play.os.close", side_effect=track_close):
                with self.assertRaises(OSError):
                    bink_play.load_names(archive, mount)
            self.assertTrue(len(closed) > 0)

    def test_load_names_restores_write_mode_on_replace_failure(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            archive, mount = d / "movies.rpf", d / "common.rpf"
            archive.write_bytes(b"x" * 16)
            mount.write_bytes(b"y")
            chmod_calls = []
            real_chmod = bink_play.os.chmod

            def track_chmod(p, mode, **kwargs):
                chmod_calls.append((p, mode))
                return real_chmod(p, mode, **kwargs)

            def fake_names(argv):
                out = Path(argv[argv.index("--out") + 1])
                out.write_text("source,offset,size,path,confidence,width,height,frames\n"
                               "movies.rpf,4096,10,Stinger/INTRO_A.bk2,anchored,3840,2160,450\n", encoding="utf-8")
                return 0

            with mock.patch.object(bink_play, "cache_path", return_value=d / "cache" / "bink_names.csv"), \
                 mock.patch.object(bink_play.bink_names, "main", side_effect=fake_names), \
                 mock.patch("bink_play.os.replace", side_effect=OSError("replace failed")), \
                 mock.patch("bink_play.os.chmod", side_effect=track_chmod):
                with self.assertRaises(OSError):
                    bink_play.load_names(archive, mount)
            self.assertTrue(any(mode == 0o600 for p, mode in chmod_calls))


class NoCacheTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.archive = self.directory / "streams.csv"
        self.mount = self.directory / "mount.xml"
        self.archive.write_text("source,offset,size,width,height,frames\nsample,32,64,16,16,2\n",
                                encoding="utf-8", newline="\n")
        self.mount.write_text("<filename>$(common)/movies/synthetic.bk2</filename>",
                              encoding="utf-8", newline="\n")
        self.cache = self.directory / "cache" / "names.csv"
        self.expected = [("synthetic.bk2", 32, "16", "16", "2")]

    def test_cli_lists_without_accessing_an_unusable_cache_directory(self):
        self.cache.parent.write_bytes(b"unrelated file, not a directory")
        with mock.patch.object(bink_play, "cache_path", return_value=self.cache), \
                mock.patch.object(sys, "stdout", new_callable=StringIO) as stdout:
            rc = bink_play.main(["--game-dir", str(self.directory), "--archive", self.archive.name,
                                 "--mount", self.mount.name, "--no-cache", "--list"])
        self.assertEqual(rc, 0)
        self.assertIn("synthetic.bk2  16x16, 2 frames", stdout.getvalue().splitlines())
        self.assertEqual(self.cache.parent.read_bytes(), b"unrelated file, not a directory")

    def test_no_cache_does_not_create_a_persistent_cache_directory(self):
        with mock.patch.object(bink_play, "cache_path", return_value=self.cache):
            self.assertEqual(bink_play.load_names(self.archive, self.mount, use_cache=False), self.expected)
        self.assertFalse(self.cache.parent.exists())

    def test_existing_cache_is_untouched_and_temporary_report_is_removed(self):
        self.cache.parent.mkdir()
        self.cache.write_bytes(b"existing cache must remain unchanged")
        reports = []
        real_names = bink_play.bink_names.main

        def generate(argv):
            reports.append(Path(argv[argv.index("--out") + 1]))
            return real_names(argv)

        with mock.patch.object(bink_play, "cache_path", return_value=self.cache) as cache_path, \
                mock.patch.object(bink_play.bink_names, "main", side_effect=generate):
            self.assertEqual(bink_play.load_names(self.archive, self.mount, use_cache=False), self.expected)
        cache_path.assert_not_called()
        self.assertEqual(self.cache.read_bytes(), b"existing cache must remain unchanged")
        self.assertEqual(list(self.cache.parent.iterdir()), [self.cache])
        self.assertEqual(len(reports), 1)
        self.assertFalse(reports[0].exists())

    def test_failed_no_cache_generation_removes_temporary_report(self):
        reports = []

        def fail(argv):
            path = Path(argv[argv.index("--out") + 1])
            reports.append(path)
            path.write_bytes(b"partial report")
            raise OSError("synthetic report failure")

        with mock.patch.object(bink_play, "cache_path", return_value=self.cache), \
                mock.patch.object(bink_play.bink_names, "main", side_effect=fail):
            with self.assertRaisesRegex(OSError, "synthetic report failure"):
                bink_play.load_names(self.archive, self.mount, use_cache=False)
        self.assertEqual(len(reports), 1)
        self.assertFalse(reports[0].exists())
        self.assertFalse(self.cache.parent.exists())


if __name__ == "__main__":
    unittest.main()
