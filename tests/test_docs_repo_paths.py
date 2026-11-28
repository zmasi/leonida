"""Repository paths the docs name in backticks exist.

    python -I tests/test_docs_repo_paths.py

`tests/test_docs_links.py` checks Markdown links; this checks the other way the docs point at the tree, a backticked
`tools/...`, `runtime/...`, `tests/...`, `scripts/...`, `docs/...`, `viewer/...` or `game/...` path (TEST-871: until
the last three were added, `docs/audio_config.md` named a `docs/music.md` that was never in the repository).
`third_party/` is left out: its files are a submodule's, which `git ls-files` doesn't list.
A rename, or a page written against an unmerged pull request, leaves such a path pointing at nothing
(docs/saves.md described #279's `xgameruntime_services.cpp` as the runtime's save store while #279 was still
open). Reads tracked files only (`git ls-files`, with a directory walk when git is not available); no game files.
"""
import os
import re
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PATH = re.compile(r"`((?:tools|runtime|tests|scripts|docs|viewer|game)/[A-Za-z0-9_./\-]+)`")
FENCE_OPEN = re.compile(r"(`{3,})[^`]*$|(~{3,}).*$")
# Inline Markdown links pointing at an external URL (e.g. another repository's tree).
EXTERNAL_LINK = re.compile(
    r"""\[[^\]]*\]\(
    (?:<[^>\n]*://[^>\n]*>|[^)\s]*://[^)\s]*)
    (?:\s+(?:
        "(?:\\.|[^"\\])*" | '(?:\\.|[^'\\])*' | \((?:\\.|[^()\\])*\)
    ))?
    \)""",
    re.VERBOSE,
)
# Pages that quote another project's tree (SCARAB, americas-recomp): their paths are not this repository's.
OTHER_PROJECTS = {"docs/fssm_crosscheck.md", "docs/d3d12_xs_surface.md"}
# Build output, not tracked.
BUILD_OUTPUT = ("runtime/build",)


def _update_fence(fence, line):
    """Return (new_fence, is_fence_line) for CommonMark backtick or tilde fences."""
    stripped = line.lstrip(" \t").rstrip("\r")
    if fence is not None:
        char, length = fence
        run = len(stripped) - len(stripped.lstrip(char))
        if run >= length and not stripped[run:].strip(" \t"):
            return None, True
        return fence, False
    match = FENCE_OPEN.match(stripped)
    if match:
        run = match.group(1) or match.group(2)
        return (run[0], len(run)), True
    return None, False


def tracked_files():
    try:
        out = subprocess.run(["git", "ls-files"], cwd=ROOT, capture_output=True, text=True, check=True).stdout
        return [p for p in out.splitlines() if p]
    except (OSError, subprocess.CalledProcessError):
        found = []
        for root, dirs, files in os.walk(ROOT):
            dirs[:] = [d for d in dirs if d not in (".git", "build", "__pycache__", ".pytest_cache")]
            found += [os.path.relpath(os.path.join(root, f), ROOT).replace(os.sep, "/") for f in files]
        return found


def missing_paths(pages, files):
    """[(page:line, path)] for backticked repository paths in `pages` ({name: text}) that `files` lacks."""
    known = set(files)
    for f in files:
        parts = f.split("/")
        known.update("/".join(parts[:i]) for i in range(1, len(parts)))
    out = []
    for page, text in sorted(pages.items()):
        fence = None
        for n, line in enumerate(text.split("\n"), 1):
            fence, is_fence_line = _update_fence(fence, line)
            if is_fence_line or fence is not None:
                continue
            scan = EXTERNAL_LINK.sub("", line)
            for m in PATH.finditer(scan):
                path = m.group(1).rstrip("/.")
                if "*" in path or path.startswith(BUILD_OUTPUT) or path in known:
                    continue
                out.append((f"{page}:{n}", path))
    return out


class DocsRepoPaths(unittest.TestCase):
    def test_checker_reports_only_the_missing_path(self):
        pages = {"docs/a.md": "Run `tools/real.py`, then `tools/gone.py`.\n```\n`tools/in_fence.py`\n```\n"
                              "`runtime/build/x.exe` and `tests/` and [`tools/x.py`](https://example.org/tools/x.py)"}
        self.assertEqual(missing_paths(pages, ["tools/real.py", "tests/t.py"]), [("docs/a.md:1", "tools/gone.py")])

    def test_url_on_line_does_not_hide_missing_repo_path(self):
        pages = {
            "docs/u.md": (
                "See https://example.org/spec and `tools/gone.py`.\n"
                "See [`tools/other.py`](https://example.org/tools/other.py) and `tools/also_gone.py`.\n"
                "See [upstream](https://example.org/repo \"title\") and `tools/real.py`.\n"
                "See [`tools/angled.py`](<https://example.org/tools/angled.py>) and `tools/real.py`.\n"
            )
        }
        self.assertEqual(
            missing_paths(pages, ["tools/real.py"]),
            [("docs/u.md:1", "tools/gone.py"), ("docs/u.md:2", "tools/also_gone.py")],
        )

    def test_tilde_and_nested_fences_are_tracked(self):
        pages = {
            "docs/f.md": "\n".join([
                "~~~",
                "`tools/in_tilde.py`",
                "```",
                "~~~",
                "`tools/after_tilde.py`",
                "````md",
                "```",
                "`tools/in_four.py`",
                "````",
                "`tools/after_four.py`",
                "```inline``` and `tools/after_inline.py`",
            ])
        }
        self.assertEqual(
            missing_paths(pages, ["tools/real.py"]),
            [
                ("docs/f.md:5", "tools/after_tilde.py"),
                ("docs/f.md:10", "tools/after_four.py"),
                ("docs/f.md:11", "tools/after_inline.py"),
            ],
        )

    def test_docs_viewer_and_game_paths_are_checked(self):
        # TEST-871: a missing page, viewer file or package manifest is reported like a missing tool.
        pages = {"docs/b.md": "See `docs/real.md`, `docs/gone.md`, `viewer/index.html`, `viewer/gone.js`, "
                              "`game/xbox-1.0.2.1.json` and `game/gone.json`."}
        files = ["docs/real.md", "viewer/index.html", "game/xbox-1.0.2.1.json"]
        self.assertEqual(missing_paths(pages, files),
                         [("docs/b.md:1", "docs/gone.md"), ("docs/b.md:1", "viewer/gone.js"),
                          ("docs/b.md:1", "game/gone.json")])

    def test_docs_name_existing_paths(self):
        files = tracked_files()
        pages = {}
        for p in files:
            if p.startswith("docs/") and p.endswith(".md") and p not in OTHER_PROJECTS:
                with open(os.path.join(ROOT, p), encoding="utf-8") as f:
                    pages[p] = f.read()
        self.assertTrue(pages)
        missing = missing_paths(pages, files)
        self.assertEqual(missing, [], "backticked paths that name no tracked file: " +
                         "; ".join(f"{where} {path}" for where, path in missing))


if __name__ == "__main__":
    unittest.main()
