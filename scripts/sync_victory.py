#!/usr/bin/env python3
"""Check DexNom/VIctory for new commits and optionally merge them into victory/.

VIctory lives in this repository as a git subtree under ``victory/`` (added with
``git subtree add --prefix=victory victory main``). This tool fetches the
``victory`` remote, reports the commits that are not merged yet and, with
``--merge``, pulls them in with ``git subtree pull`` so upstream history is kept
and local changes under ``victory/`` are merged, not overwritten.

Run from anywhere inside the repository:

    python -I scripts/sync_victory.py            # report only
    python -I scripts/sync_victory.py --merge    # report, then merge if clean

Exit codes: 0 up to date or merged, 10 updates waiting (report only),
2 git failure, 3 working tree not clean (merge refused), 4 merge conflict.
"""

import argparse
import datetime
import os
import subprocess
import sys

REMOTE = "victory"
BRANCH = "main"
PREFIX = "victory"


def git(*args, check=True):
    proc = subprocess.run(
        ["git", *args],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if check and proc.returncode != 0:
        print(f"git {' '.join(args)} failed ({proc.returncode}):\n{proc.stdout}")
        sys.exit(2)
    return proc


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--merge", action="store_true", help="merge new commits with git subtree pull")
    args = ap.parse_args()

    top = git("rev-parse", "--show-toplevel").stdout.strip()
    os.chdir(top)
    stamp = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    git("fetch", "--quiet", REMOTE, BRANCH)
    upstream = git("rev-parse", f"{REMOTE}/{BRANCH}").stdout.strip()
    merged = git("merge-base", "--is-ancestor", upstream, "HEAD", check=False).returncode == 0
    if merged:
        print(f"[{stamp}] {REMOTE}/{BRANCH} {upstream[:9]} is already merged; nothing to do.")
        return 0

    new = git("log", "--oneline", "--no-decorate", f"HEAD..{upstream}").stdout.strip().splitlines()
    print(f"[{stamp}] {len(new)} new {REMOTE}/{BRANCH} commit(s) up to {upstream[:9]}:")
    for line in new:
        print(f"  {line}")
    if not args.merge:
        return 10

    dirty = git("status", "--porcelain", "--untracked-files=no").stdout.strip()
    if dirty:
        print("Working tree has uncommitted changes; commit or stash them, then run --merge again.")
        print(dirty)
        return 3

    msg = (
        f"Merge {REMOTE}/{BRANCH} {upstream[:9]} into {PREFIX}/ ({len(new)} commit(s))\n\n"
        "Pulled with scripts/sync_victory.py --merge (git subtree pull).\n"
    )
    pull = git("subtree", "pull", f"--prefix={PREFIX}", REMOTE, BRANCH, "-m", msg, check=False)
    print(pull.stdout.strip())
    if pull.returncode != 0:
        conflicts = git("diff", "--name-only", "--diff-filter=U", check=False).stdout.strip()
        if conflicts:
            print("Merge stopped on conflicts in:")
            print(conflicts)
            return 4
        print("git subtree pull failed.")
        return 2
    print(f"Merged {REMOTE}/{BRANCH} {upstream[:9]} into {PREFIX}/.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
