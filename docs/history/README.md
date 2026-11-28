# Portable record of the original repository's history

The original `ItsNotRexed/Leonida` repository went private on 23 November 2026. Its code survives as branches here and
its issues as an index in [#18](https://github.com/ubiquitarianism/leonida/issues/18), but both of those still live on
one service. This directory is the part that does not: the old pull request branch tips and the old issue index as
sorted text files with a SHA-256 beside each, so the record can be checked and moved without GitHub. Requested by
[#563](https://github.com/ubiquitarianism/leonida/issues/563) (a migration to [Radicle](https://radicle.xyz/)).

Nothing of the game is in them. A row is an old number, a SHA, a name, a date and a subject line, or an old issue's
number, title, author and labels.

## Files

| File | Rows | Contents |
|---|---|---|
| `old-pr.tsv` | 251 | `number`, `tip`, `author_date`, `author`, `subject` of each surviving `old-pr/*` branch |
| `old-pr.tsv.sha256` | 1 | SHA-256 of `old-pr.tsv` |
| `issues.tsv` | 135 | `number`, `title`, `author`, `labels` of each indexed old issue |
| `issues.tsv.sha256` | 1 | SHA-256 of `issues.tsv` |

Both files are tab-separated, UTF-8, LF, sorted numerically by old number, with a header line. Tabs and newlines
inside a field are collapsed to single spaces so one record stays one line. `author_date` is the tip commit's own
ISO-8601 author date, not the time of the export, which is what makes an export reproducible.

## Regenerating and checking

```powershell
git fetch origin 'refs/heads/old-pr/*:refs/remotes/origin/old-pr/*'
python -I scripts/export_old_pr_manifest.py branches --out docs/history --remote origin
python -I scripts/export_old_pr_manifest.py issues  --out docs/history --index issue-18.md

python -I -c "import hashlib,pathlib; p=pathlib.Path('docs/history/old-pr.tsv'); print(hashlib.sha256(p.read_bytes()).hexdigest())"
```

The last line must print the hash in `old-pr.tsv.sha256`. A different hash from the same branches means a branch moved
after this file was written, which is the point of keeping it: the record is checkable on its own. Issue #18's body is
not in the repository, so save it to `issue-18.md` before running the `issues` step.

## What it is and is not

- **It is** a durable record of which old pull requests existed, where each one ended (its last commit), who wrote it
  and what it was about, plus the same for the index of old issues.
- **It is not** the discussions. Review comments, review states, CI results and the text of the issues are not
  recoverable from here; nobody has an export of them.
- The branches themselves are still the content. The manifest names each tip so a branch that disappears can be
  recognised, not restored.

## Moving it to Radicle

Radicle identifies content by hash, which is why the manifest is written as hashes and sorted fields rather than as a
web page: a patch mirrored there can be checked against the `tip` recorded here, and an issue imported there keeps its
old number in the first column. `rad patch open` per branch, `rad issue open` per issue row, citing `old-pr.tsv`'s
hash in the description, keeps the old numbering and the provenance.
