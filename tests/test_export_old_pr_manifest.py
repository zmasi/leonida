"""Synthetic tests for scripts/export_old_pr_manifest.py (issue #563): no game data, no network."""
import hashlib
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT = os.path.join(ROOT, "scripts", "export_old_pr_manifest.py")

ISSUE_TABLE = """Index of the issues. Nothing else.

| Old # | Title | Author | Labels |
|---|---|---|---|
| 94 | Wave64 on 32-lane subgroups | `ttd0x` |  |
| 4 | Would you consider\tMIT License? | `Ahmet-Dedeler` | license |
"""


def git(cwd, *args, env=None):
    return subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True,
                          encoding="utf-8", check=True, env=env)


def run(args, stdin=None, cwd=None):
    return subprocess.run([sys.executable, "-I", SCRIPT, *args], capture_output=True, text=True,
                          encoding="utf-8", input=stdin, cwd=cwd)


class OldPrManifestTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="old-pr-manifest-")
        self.repo = self.tmp.name
        env = dict(os.environ, GIT_AUTHOR_NAME="Ada", GIT_AUTHOR_EMAIL="ada@example.invalid",
                   GIT_COMMITTER_NAME="Ada", GIT_COMMITTER_EMAIL="ada@example.invalid",
                   GIT_AUTHOR_DATE="2026-11-22T20:00:00+00:00",
                   GIT_COMMITTER_DATE="2026-11-22T20:00:00+00:00")
        self.env = env
        git(self.repo, "init", "--quiet", "-b", "main", env=env)
        open(os.path.join(self.repo, "f"), "w", encoding="utf-8").close()
        git(self.repo, "add", "f", env=env)
        git(self.repo, "commit", "--quiet", "-m", "base", env=env)
        # Old numbers 10 and 9: the numeric order differs from the ref sort order on purpose.
        for number, subject in ((10, "second\tpatch"), (9, "first patch")):
            git(self.repo, "checkout", "--quiet", "-b", f"old-pr/{number}", env=env)
            with open(os.path.join(self.repo, "f"), "a", encoding="utf-8") as f:
                f.write(f"{number}\n")
            msg = os.path.join(self.repo, "msg")
            with open(msg, "w", encoding="utf-8", newline="\n") as m:
                m.write(subject + "\n")
            git(self.repo, "commit", "--quiet", "-a", "-F", msg, env=env)
            git(self.repo, "update-ref", f"refs/remotes/origin/old-pr/{number}",
                f"refs/heads/old-pr/{number}", env=env)

    def tearDown(self):
        self.tmp.cleanup()

    def test_branches_sorts_numerically_and_collapses_tabs(self):
        r = run(["branches", "--out", self.repo, "--remote", "origin"], cwd=self.repo)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("2 old pull request branches", r.stdout)
        lines = open(os.path.join(self.repo, "old-pr.tsv"), encoding="utf-8").read().splitlines()
        self.assertEqual(lines[0], "number\ttip\tauthor_date\tauthor\tsubject")
        self.assertTrue(lines[1].startswith("9\t"))
        self.assertTrue(lines[2].startswith("10\t"))
        self.assertIn("second patch", lines[2])  # the tab in the subject became a space
        self.assertEqual(len(lines[2].split("\t")), 5)
        tip = git(self.repo, "rev-parse", "refs/remotes/origin/old-pr/9", env=self.env).stdout.strip()
        self.assertEqual(lines[1].split("\t")[1], tip)

    def test_sha256_file_matches_and_repeats_byte_for_byte(self):
        run(["branches", "--out", self.repo, "--remote", "origin"], cwd=self.repo)
        first = open(os.path.join(self.repo, "old-pr.tsv"), "rb").read()
        sha_line = open(os.path.join(self.repo, "old-pr.tsv.sha256"), encoding="utf-8").read().strip()
        self.assertEqual(sha_line, hashlib.sha256(first).hexdigest() + "  old-pr.tsv")
        run(["branches", "--out", self.repo, "--remote", "origin"], cwd=self.repo)
        self.assertEqual(open(os.path.join(self.repo, "old-pr.tsv"), "rb").read(), first)

    def test_missing_refs_is_an_error(self):
        r = run(["branches", "--out", self.repo, "--remote", "nope"], cwd=self.repo)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("no refs matched", r.stderr)

    def test_issues_reads_the_table_and_sorts(self):
        r = run(["issues", "--out", self.repo, "--index", "-"], stdin=ISSUE_TABLE, cwd=self.repo)
        self.assertEqual(r.returncode, 0, r.stderr)
        lines = open(os.path.join(self.repo, "issues.tsv"), encoding="utf-8").read().splitlines()
        self.assertEqual(lines[0], "number\ttitle\tauthor\tlabels")
        self.assertTrue(lines[1].startswith("4\tWould you consider MIT License?\t"))
        self.assertTrue(lines[2].startswith("94\t"))
        self.assertEqual(lines[2].split("\t")[3], "")  # empty labels stay a field

    def test_issues_without_a_table_is_an_error(self):
        r = run(["issues", "--out", self.repo, "--index", "-"], stdin="nothing here\n", cwd=self.repo)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("no issue rows found", r.stderr)


if __name__ == "__main__":
    unittest.main()
