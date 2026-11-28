"""scripts/check_no_game_data.py catches game-data magics under any file name.

    python -I tests/test_check_no_game_data.py

Every input is a few hand-written header bytes in a temporary directory (no game files): each format under a harmless
name, files that must pass (text, PNG, a lone "MZ", magics past the start), and the default mode over a throwaway git
repository. The script runs as itself, through its own command line.
"""
import bz2
import contextlib
import errno
import gzip
import importlib.util
import io
import lzma
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import tarfile
import unittest
import zipfile
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT = os.path.join(ROOT, "scripts", "check_no_game_data.py")


def pe_header(e_lfanew=0x80):
    head = bytearray(e_lfanew + 4)
    head[0:2] = b"MZ"
    head[0x3C:0x40] = e_lfanew.to_bytes(4, "little")
    head[e_lfanew:e_lfanew + 4] = b"PE\0\0"
    return bytes(head)


# file name (deliberately not the format's extension), contents, format the script must report
CAUGHT = [
    ("archive.bin", b"9FPR" + bytes(12), "RPF9 archive"),
    ("pool.dat", b"TTPF" + bytes(12), "TTPF texture pool"),
    ("resource.dat", b"RSC8" + bytes(12), "RAGE resource"),
    ("page.dat", b"0DRP" + bytes(12), "RAGE page directory"),
    ("tree.dat", b"RBF0" + bytes(12), "RBF0 tree"),
    ("shader.txt", b"XBCH" + (3).to_bytes(4, "little") + bytes(8), "XBCH shader container"),
    ("shader2.txt", b"FSSM" + bytes(12), "FSSM shader"),
    ("container.txt", b"DXBC" + bytes(12), "DXBC container"),
    ("module.txt", bytes.fromhex("03022307") + bytes(12), "SPIR-V module"),
    ("module_be.txt", bytes.fromhex("07230203") + bytes(12), "SPIR-V module"),
    ("texture.png.txt", b"DDS " + (124).to_bytes(4, "little") + bytes(8), "DDS texture"),
    ("bank.json", b"ADAT" + bytes(12), "AWC audio bank"),
    ("music.md", b"OggS\0\x02" + bytes(10), "Ogg stream"),
    ("sound.csv", b"RIFF" + bytes(4) + b"WAVEfmt ", "RIFF WAVE audio"),
    ("movie.txt", b"KB2j" + bytes(12), "Bink 2 video"),
    ("model.json", b"glTF" + (2).to_bytes(4, "little") + bytes(8), "glTF binary"),
    ("game.txt", pe_header(), "PE executable"),
    ("far_pe.txt", pe_header(0x400), "PE executable"),
    # e_lfanew around the end of the first bytes read (0x208): before, straddling and after it
    *((f"pe_{n}.txt", pe_header(n), "PE executable") for n in range(0x204, 0x20A)),
    ("pe_1mib.txt", pe_header(1 << 20), "PE executable"),
    ("pe_1mib_plus.txt", pe_header((1 << 20) + 4), "PE executable"),
    ("image.bin", bytes(0x200) + b"msft-xvd" + bytes(8), "XVD image"),
    ("container2.dat", b"PSIN" + bytes(12), "PSIN metadata container"),
    ("heightmap.dat", b"PAMH" + bytes(12), "PAMH heightmap"),
    ("heightmap2.dat", b"MHIU" + bytes(12), "MHIU heightmap"),
    ("presets.dat", b"SPS1" + bytes(12), "SPS1 shader-preset table"),
    ("effects.dat", b"SGD5" + bytes(12), "SGD5 effects database"),
    ("profile.dat", b"BCPHIST1" + bytes(8), "local BCPack predictor profile"),
    ("texcontainer.dat", b"RDS5" + bytes(12), "RDS5 texture container"),
    ("bank2.json", b"AWC\x00" + bytes(12), "AWC audio bank"),
    ("script.lol", b"YSC\x00" + bytes(12), "YSC compiled script"),
    ("anim.bin", b"MoVE" + (19).to_bytes(4, "little") + bytes(8), "MoVE animation container"),
    ("bones.dat", b"SKEL_" + b"SKEL_Head\x00" + bytes(8), "SKEL_ bone table"),
    ("script2.lol", b"SCRP" + bytes(12), "SCRP compiled script"),
    ("story_save.txt", b"JSAV" + (3).to_bytes(4, "little") + bytes(8), "JSAV story save"),
    ("profile.json", b"ASAV" + (1).to_bytes(4, "little") + bytes(8), "ASAV save record"),
    ("chives.txt", b"SLAYER1\0" + bytes(8), "SLAYER1 packed save"),
    ("chunk.json", b"LPK1" + (2).to_bytes(4, "little") + b"[]", "LPK1 world staging pack"),
]

PASSED = [
    ("README.md", b"# Notes\n\nThe RPF9 magic is stored as 9FPR; DDS files start with 'DDS '.\n"),
    ("logo.png", b"\x89PNG\r\n\x1a\n" + bytes(16)),
    ("mz.txt", b"MZ is not enough on its own\n"),
    ("metadata.txt", b"METADATA ADAT later in the line\n"),
    ("bones.md", b"# SKEL_HEAD, MoVE and SCRP name formats; none of them starts this file\n"),
    ("riff.bin", b"RIFF" + bytes(4) + b"AVI LIST"),
    ("saves.md", b"# Saves\n\nA story save starts with JSAV, a profile with ASAV, its packed copy with SLAYER1.\n"),
    ("slayer.txt", b"SLAYER1 without the zero byte after it is just a word\n"),
    ("empty.txt", b""),
    ("mz_past_end.txt", pe_header(0x80)[:0x40]),  # e_lfanew points past the end of the file
    ("mz_cut_signature.txt", pe_header(0x300)[:-2]),  # only "PE" of the signature is there
    ("mz_not_pe.txt", pe_header(0x80)[:-4] + b"NE\0\0"),
]


def rmtree(path):
    """shutil.rmtree that also removes read-only files (Git's objects on Windows)."""
    def retry(func, p, _):
        os.chmod(p, stat.S_IWRITE)
        func(p)
    if sys.version_info >= (3, 12):
        shutil.rmtree(path, onexc=retry)
    else:
        shutil.rmtree(path, onerror=retry)


def zipped(members):
    """A zip archive (bytes) holding `members`, {name: bytes}."""
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in members.items():
            z.writestr(name, data)
    return buf.getvalue()


def tarred(members):
    """A tar archive (bytes) holding `members`, {name: bytes}."""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w") as t:
        for name, data in members.items():
            info = tarfile.TarInfo(name)
            info.size = len(data)
            t.addfile(info, io.BytesIO(data))
    return buf.getvalue()


def v7_tarred(members):
    """Hand-built legacy tar headers, without the later ustar marker or extension fields."""
    buf = io.BytesIO()
    for name, data in members.items():
        info = tarfile.TarInfo(name)
        info.size = len(data)
        info.type = tarfile.AREGTYPE
        header = bytearray(info.tobuf(format=tarfile.USTAR_FORMAT))
        header[257:] = bytes(tarfile.BLOCKSIZE - 257)
        header[148:156] = b"        "
        header[148:156] = ("%06o\0 " % sum(header)).encode("ascii")
        buf.write(header)
        buf.write(data)
        buf.write(bytes(-len(data) % tarfile.BLOCKSIZE))
    buf.write(bytes(2 * tarfile.BLOCKSIZE))
    return buf.getvalue()


def corrupt_deflate(archive):
    """`archive` (one deflated zip member) with the first byte of its deflate data made an invalid block type."""
    data = bytearray(archive)
    name_len, extra_len = int.from_bytes(data[26:28], "little"), int.from_bytes(data[28:30], "little")
    data[30 + name_len + extra_len] = 0xFF  # BTYPE 11: zlib.error "invalid block type"
    return bytes(data)


DDS = b"DDS " + (124).to_bytes(4, "little") + bytes(8)
FSSM = b"FSSM" + bytes(12)
RPF = b"9FPR" + bytes(12)

# file name, contents, the line the script must print after "<name>: "
CAUGHT_IN_ARCHIVES = [
    ("notes.zip.txt", zipped({"readme.txt": b"text\n", "tex/a.png": DDS}), "DDS texture in zip archive (tex/a.png)"),
    ("data.bin", gzip.compress(FSSM), "FSSM shader in gzip stream"),
    ("tool.bin", bz2.compress(pe_header()), "PE executable in bzip2 stream"),
    ("pack.bin", lzma.compress(RPF), "RPF9 archive in xz stream"),
    ("far.bin", gzip.compress(pe_header(0x400)), "PE executable in gzip stream"),
    ("nested.bin", gzip.compress(zipped({"inner/x.bin": RPF})), "RPF9 archive in zip archive (inner/x.bin) in gzip stream"),
    ("a.bin", b"7z\xbc\xaf\x27\x1c" + bytes(26), "7-Zip archive (contents can't be checked)"),
    ("b.bin", b"\x28\xb5\x2f\xfd" + bytes(12), "Zstandard stream (contents can't be checked)"),
    ("c.bin", b"Rar!\x1a\x07\x01\x00" + bytes(8), "RAR archive (contents can't be checked)"),
    ("broken.bin", b"PK\x03\x04" + bytes(40), "zip archive that can't be opened (contents can't be checked)"),
    ("broken.gz", b"\x1f\x8b\x09\x00" + bytes(12), "gzip stream that can't be opened (contents can't be checked)"),
    # broken.gz names compression method 9, which isn't deflate; deep.bin is a DDS under four gzip layers
    ("corrupt.gz", gzip.compress(DDS)[:12] + b"\xff" * 12,
     "gzip stream that can't be opened (contents can't be checked)"),
    ("deep.bin", gzip.compress(gzip.compress(gzip.compress(gzip.compress(DDS)))),
     "gzip stream nested more than 3 deep (contents can't be checked) in gzip stream in gzip stream in gzip stream"),
    # Review of #4040: tar archives, zips with bytes in front, a corrupt deflate stream, a newline in a member name.
    ("x.tar", tarred({"x.dds": DDS}), "DDS texture in tar archive (x.dds)"),
    ("x.tar.gz", gzip.compress(tarred({"a.txt": b"text\n", "y.bin": FSSM})),
     "FSSM shader in tar archive (y.bin) in gzip stream"),
    ("x.tar.xz", lzma.compress(tarred({"z.bin": RPF})), "RPF9 archive in tar archive (z.bin) in xz stream"),
    ("prefixed.txt", b"JUNK" + zipped({"a.png": DDS}), "DDS texture in zip archive (a.png)"),
    ("sfx.exe.txt", b"#!/bin/sh\nexit 0\n" + zipped({"b.bin": RPF}), "RPF9 archive in zip archive (b.bin)"),
    ("nested_prefixed.zip", zipped({"inner.bin": bytes(8) + zipped({"x.bin": RPF})}),
     "RPF9 archive in zip archive (x.bin) in zip archive (inner.bin)"),
    ("bad_deflate.zip", corrupt_deflate(zipped({"a.bin": DDS * 64})),
     "zip archive that can't be opened (contents can't be checked)"),
    ("newline.zip", zipped({"a\nb.png": DDS}), "DDS texture in zip archive (a?b.png)"),
]

PASSED_ARCHIVES = [
    ("docs.zip", zipped({"a.txt": b"plain text\n", "dir/b.md": b"# notes\n", "empty.txt": b""})),
    ("docs.tar", tarred({"a.txt": b"plain text\n"})),
    ("docs.tar.bz2", bz2.compress(tarred({"a.txt": b"plain text\n"}))),
    ("prefixed_clean.bin", b"JUNK" + zipped({"a.txt": b"text\n"})),
    ("notes.gz", gzip.compress(b"The RPF9 magic is stored as 9FPR.\n")),
    ("notes.bz2", bz2.compress(b"plain text\n")),
    ("notes.xz", lzma.compress(b"plain text\n")),
    ("nested_clean.gz", gzip.compress(zipped({"a.txt": b"text\n"}))),
]


def run(args, cwd=None, env=None):
    return subprocess.run([sys.executable, "-I", SCRIPT, *args], cwd=cwd, env=env, capture_output=True, text=True)


class CheckNoGameData(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(rmtree, self.dir)

    def write(self, name, data):
        path = os.path.join(self.dir, name)
        with open(path, "wb") as f:
            f.write(data)
        return path

    def test_each_format_is_caught_under_another_name(self):
        for name, data, fmt in CAUGHT:
            with self.subTest(name=name):
                result = run([self.write(name, data)])
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn(f"{name}: {fmt}", result.stdout)

    def test_game_data_inside_archives_is_caught(self):
        for name, data, line in CAUGHT_IN_ARCHIVES:
            with self.subTest(name=name):
                result = run([self.write(name, data)])
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn(f"{name}: {line}", result.stdout)

    def test_v7_tar_members_are_checked_without_a_ustar_marker(self):
        data = v7_tarred({"readme.txt": b"plain text\n", "synthetic.bin": DDS})
        self.assertEqual(data[257:263], bytes(6))
        # Confirm this is a readable archive, rather than depending on our scanner's interpretation.
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:") as archive:
            self.assertEqual(archive.getnames(), ["readme.txt", "synthetic.bin"])
            with archive.extractfile("synthetic.bin") as member:
                self.assertEqual(member.read(), DDS)
        expected = "DDS texture in tar archive (synthetic.bin)"
        cases = [
            ("legacy.bin", data, expected),
            ("legacy_gzip.bin", gzip.compress(data), expected + " in gzip stream"),
            ("legacy_bzip2.bin", bz2.compress(data), expected + " in bzip2 stream"),
            ("legacy_xz.bin", lzma.compress(data), expected + " in xz stream"),
            ("nested.bin", zipped({"inner.bin": data}), expected + " in zip archive (inner.bin)"),
        ]
        for name, payload, line in cases:
            with self.subTest(name=name):
                result = run([self.write(name, payload)])
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn(f"{name}: {line}", result.stdout)

    def test_v7_tar_containing_only_text_passes(self):
        data = v7_tarred({"readme.txt": b"plain text\n", "empty.txt": b""})
        paths = [self.write("legacy.bin", data), self.write("legacy_gzip.bin", gzip.compress(data)),
                 self.write("nested.bin", zipped({"inner.bin": data}))]
        result = run(paths)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout, "")

    def test_v7_header_probe_does_not_reject_non_archives(self):
        bad_checksum = bytearray(v7_tarred({"readme.txt": b"plain text\n"}))
        bad_checksum[0] ^= 1
        cases = [("bad_checksum.bin", bad_checksum), ("short.bin", bad_checksum[:511]),
                 ("zeros.bin", bytes(1024)), ("text.txt", b"# ordinary text\n" * 40)]
        result = run([self.write(name, data) for name, data in cases])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout, "")

    def test_broken_ustar_is_still_reported_as_unchecked(self):
        data = bytearray(tarred({"readme.txt": b"plain text\n"}))
        data[0] ^= 1
        result = run([self.write("broken.bin", data)])
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("tar archive that can't be opened (contents can't be checked)", result.stdout)

    def test_decompression_is_bounded(self):
        # A gzip of 65 MiB of zeros (about 64 KiB on disk) holding a zip: past the 64 MiB budget, reported, not read
        # to the end; and a zip with more members than the budget lists.
        bomb = io.BytesIO()
        with gzip.GzipFile(fileobj=bomb, mode="wb") as g:
            g.write(b"PK\x03\x04")
            chunk = bytes(1 << 20)
            for _ in range(65):
                g.write(chunk)
        many = io.BytesIO()
        with zipfile.ZipFile(many, "w", zipfile.ZIP_STORED) as z:
            for i in range(10001):
                z.writestr(f"{i}.txt", b"")
        for name, data, kind in [("bomb.gz", bomb.getvalue(), "gzip stream"), ("many.zip", many.getvalue(), "zip archive")]:
            with self.subTest(name=name):
                result = run([self.write(name, data)])
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn(f"{name}: {kind} too large to check", result.stdout)

    def test_archives_of_ordinary_files_pass(self):
        paths = [self.write(name, data) for name, data in PASSED_ARCHIVES]
        result = run(paths)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout, "")

    def test_ordinary_files_pass(self):
        paths = [self.write(name, data) for name, data in PASSED]
        result = run(paths)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout, "")

    def test_reports_every_match_among_clean_files(self):
        clean = self.write("README.md", b"# fine\n")
        bad1 = self.write("a.bin", b"DDS " + bytes(12))
        bad2 = self.write("b.bin", b"OggS" + bytes(12))
        result = run([clean, bad1, bad2])
        self.assertEqual(result.returncode, 1)
        self.assertEqual(len(result.stdout.splitlines()), 2, result.stdout)
        self.assertIn("2 file(s)", result.stderr)

    def test_directories_and_missing_paths_are_skipped(self):
        os.mkdir(os.path.join(self.dir, "sub"))
        result = run([os.path.join(self.dir, "sub"), os.path.join(self.dir, "missing.bin")])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def run_with_unreadable(self, locked, paths):
        """main() in this process with open() refusing `locked`; (exit status, stdout, stderr)."""
        spec = importlib.util.spec_from_file_location("check_no_game_data_under_test", SCRIPT)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        real_open = open

        def refusing_open(file, *args, **kwargs):
            if os.fspath(file) == locked:
                raise PermissionError(errno.EACCES, "Permission denied", locked)
            return real_open(file, *args, **kwargs)

        out, err = io.StringIO(), io.StringIO()
        with mock.patch("builtins.open", refusing_open), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = module.main(paths)
        return rc, out.getvalue(), err.getvalue()

    def test_unreadable_file_is_one_stderr_line_and_exit_2(self):
        locked = self.write("locked.bin", b"plain text\n")
        sibling = self.write("sibling.bin", b"DDS " + bytes(12))
        clean = self.write("README.md", b"# fine\n")
        rc, out, err = self.run_with_unreadable(locked, [locked, clean])
        self.assertEqual(rc, 2, out + err)
        self.assertEqual(out, "")
        self.assertEqual(len(err.splitlines()), 1, err)
        self.assertTrue(err.startswith("check_no_game_data: cannot read "), err)
        self.assertIn("locked.bin: Permission denied", err)
        self.assertNotIn("Traceback", err)

        # the other files are still scanned; a real match wins over the unreadable one
        rc, out, err = self.run_with_unreadable(locked, [locked, sibling, clean])
        self.assertEqual(rc, 1, out + err)
        # display() reports the path relative to the working directory and keeps the absolute path when the two are
        # on different drives: a Windows temp file is on C: while the checkout is on D:.
        try:
            expected = os.path.relpath(sibling)
        except ValueError:
            expected = sibling
        self.assertEqual(out.splitlines(), [f"{expected}: DDS texture"], out)
        self.assertIn("cannot read", err)
        self.assertIn("1 file(s) start like game data", err)
        self.assertNotIn("Traceback", err)

        pe = self.write("pe.bin", pe_header())
        rc, out, err = self.run_with_unreadable(locked, [locked, pe])
        self.assertEqual(rc, 1, out + err)
        self.assertIn("PE executable", out)

    def test_git_missing_is_one_stderr_line_and_exit_2(self):
        # PATH holds no git: the default mode cannot list the tracked files, and that is not "game data found" (1)
        empty = os.path.join(self.dir, "empty-bin")
        os.mkdir(empty)
        self.write("notes.txt", b"plain text\n")
        result = run([], cwd=self.dir, env=dict(os.environ, PATH=empty))
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertEqual(len(result.stderr.splitlines()), 1, result.stderr)
        self.assertTrue(result.stderr.startswith("check_no_game_data: cannot list the tracked files with git: "),
                        result.stderr)
        self.assertNotIn("Traceback", result.stderr)

    def test_outside_a_repository_is_one_stderr_line_and_exit_2(self):
        # GIT_CEILING_DIRECTORIES stops git from finding a repository above the temporary directory, and LC_ALL=C
        # keeps git's message in English; a GIT_DIR, GIT_WORK_TREE or GIT_INDEX_FILE of the caller would point git at a
        # repository anyway (a git hook sets them), so they are removed
        self.write("notes.txt", b"plain text\n")
        env = {k: v for k, v in os.environ.items() if k not in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE")}
        env.update(GIT_CEILING_DIRECTORIES=os.path.dirname(self.dir), LC_ALL="C")
        result = run([], cwd=self.dir, env=env)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertEqual(len(result.stderr.splitlines()), 1, result.stderr)
        self.assertTrue(result.stderr.startswith("check_no_game_data: cannot list the tracked files with git: "),
                        result.stderr)
        self.assertIn("not a git repository", result.stderr)
        self.assertNotIn("Traceback", result.stderr)

    @unittest.skipUnless(shutil.which("git"), "git not installed")
    def test_default_mode_reads_tracked_files_only(self):
        git = ["git", "-c", "init.defaultBranch=main"]
        subprocess.run([*git, "init", "-q", self.dir], check=True)
        self.write("notes.txt", b"plain text\n")
        self.write("untracked.bin", b"DDS " + bytes(12))
        subprocess.run(["git", "add", "notes.txt"], cwd=self.dir, check=True)
        result = run([], cwd=self.dir)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        self.write("renamed.txt", b"FSSM" + bytes(12))
        subprocess.run(["git", "add", "renamed.txt"], cwd=self.dir, check=True)
        result = run([], cwd=self.dir)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("renamed.txt: FSSM shader", result.stdout)
        self.assertNotIn("untracked.bin", result.stdout)


if __name__ == "__main__":
    unittest.main()
