"""Write a portable, verifiable manifest of the original repository's surviving history (issue #563).

The original `ItsNotRexed/Leonida` repository went private in November 2026. Its pull requests survive only as the
`old-pr/<old number>` branches kept here (issue #17) and its issues only as the title/author index in issue #18. Both
live on GitHub, which is the single point of failure this manifest removes: the branch tips, their authors and dates
are recorded as a sorted TSV with a SHA-256 beside it, so the record can be checked after the branches themselves are
gone, and fed to a migration (Radicle, issue #563) without re-reading GitHub.

    python -I scripts/export_old_pr_manifest.py branches --out docs/history --remote origin
    python -I scripts/export_old_pr_manifest.py issues --index issue-18.md --out docs/history

`branches` reads `refs/remotes/<remote>/old-pr/*` from the local clone; `issues` reads the markdown table posted in
issue #18. Both write `<kind>.tsv` and `<kind>.sha256` in the output directory, and both are deterministic: the same
input gives the same bytes, sorting numerically by old number so the order does not depend on ref iteration. Commit
dates are kept as the commit's own ISO-8601 author date, not as a generation timestamp, for the same reason.

Nothing of the game is read: a branch number, a SHA, a name, a date and a subject line.
"""
import argparse
import hashlib
import os
import re
import subprocess
import sys

# A subject can contain a tab or a newline; the TSV keeps one record per line with single spaces.
_WHITESPACE = re.compile(r"\s+")

# | 4 | Would you consider MIT License? | `Ahmet-Dedeler` |  |
_ISSUE_ROW = re.compile(r"^\|\s*(\d+)\s*\|(.+?)\|(.+?)\|(.*?)\|\s*$")


def collapse(text: str) -> str:
    return _WHITESPACE.sub(" ", text).strip()


def git(branch_format: str, pattern: str) -> list:
    r = subprocess.run(
        ["git", "for-each-ref", "--format=" + branch_format, pattern],
        capture_output=True, text=True, encoding="utf-8",
    )
    if r.returncode:
        raise SystemExit(f"git for-each-ref failed: {r.stderr.strip()}")
    return r.stdout.splitlines()


def branches(remote: str) -> list:
    """Rows (number, tip sha, author date, author, subject) for every surviving old pull request branch."""
    pattern = f"refs/remotes/{remote}/old-pr/*"
    fmt = "%(refname:lstrip=3)%09%(objectname)%09%(authordate:iso-strict)%09%(authorname)%09%(subject)"
    rows = []
    for line in git(fmt, pattern):
        if not line.strip():
            continue
        ref, sha, date, author, subject = (line.split("\t", 4) + [""] * 5)[:5]
        number = ref.partition("old-pr/")[2]
        if not number.isdigit() or not re.fullmatch(r"[0-9a-f]{40}", sha):
            raise SystemExit(f"unexpected ref line: {line!r}")
        rows.append((int(number), sha, date, collapse(author), collapse(subject)))
    if not rows:
        raise SystemExit(f"no refs matched {pattern}; fetch the branches first (git fetch {remote})")
    numbers = [r[0] for r in rows]
    if len(set(numbers)) != len(numbers):
        raise SystemExit("two refs share an old number")
    return sorted(rows)


def issues(text: str) -> list:
    """Rows (number, title, author, labels) from the markdown table of issue #18."""
    rows = []
    for line in text.splitlines():
        if not line.startswith("|"):
            continue
        m = _ISSUE_ROW.match(line)
        if not m:
            continue
        number, title, author, labels = m.groups()
        if not title.strip() or set(title.strip()) <= set("-: "):
            continue
        rows.append((int(number), collapse(title), collapse(author), collapse(labels)))
    if not rows:
        raise SystemExit("no issue rows found; is this the table from issue #18?")
    return sorted(rows)


def write_tsv(out_dir: str, kind: str, header: tuple, rows: list) -> str:
    os.makedirs(out_dir, exist_ok=True)
    body = "\t".join(header) + "\n"
    body += "".join("\t".join(str(c) for c in row) + "\n" for row in rows)
    path = os.path.join(out_dir, kind + ".tsv")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(body)
    digest = hashlib.sha256(body.encode("utf-8")).hexdigest()
    with open(path + ".sha256", "w", encoding="utf-8", newline="\n") as f:
        f.write(f"{digest}  {kind}.tsv\n")
    return digest


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="kind", required=True)
    b = sub.add_parser("branches", help="manifest the old-pr/* branches")
    b.add_argument("--remote", default="origin", help="remote that carries the old-pr/* refs (default: origin)")
    i = sub.add_parser("issues", help="manifest the issue index from issue #18")
    i.add_argument("--index", required=True, help="file holding the issue table (or - for stdin)")
    for p in (b, i):
        p.add_argument("--out", default="docs/history", help="output directory (default: docs/history)")
    a = ap.parse_args(argv)

    if a.kind == "branches":
        rows = branches(a.remote)
        digest = write_tsv(a.out, "old-pr", ("number", "tip", "author_date", "author", "subject"), rows)
        print(f"{len(rows)} old pull request branches, sha256 {digest}")
    else:
        text = sys.stdin.read() if a.index == "-" else open(a.index, encoding="utf-8").read()
        rows = issues(text)
        digest = write_tsv(a.out, "issues", ("number", "title", "author", "labels"), rows)
        print(f"{len(rows)} old issues, sha256 {digest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
