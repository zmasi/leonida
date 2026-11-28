"""Read shader names from a completed carve or unpack index.

An index.json saved with a UTF-8 BOM (Windows PowerShell 5.1, old Notepad) is read, not refused.
"""

import json
import ntpath
import posixpath


def _stem_rows(path: str, field: str, suffix: str = "") -> list[tuple[str, dict]]:
    """Each entry of the index as (validated stem, row)."""
    try:
        # utf-8-sig: PowerShell 5.1 and old Notepad write JSON with a UTF-8 BOM.
        with open(path, encoding="utf-8-sig") as f:
            rows = json.load(f)
    except UnicodeDecodeError as exc:
        raise ValueError(f"{path}: invalid UTF-8: {exc}") from exc
    except (ValueError, RecursionError) as exc:
        raise ValueError(f"{path}: invalid JSON: {exc}") from exc
    if not isinstance(rows, list):
        raise ValueError(f"{path}: expected an array of shader entries")
    out = []
    for row in rows:
        value = row.get(field) if isinstance(row, dict) else None
        if (
            not isinstance(value, str)
            or not value
            or any(ord(c) < 32 or ord(c) == 127 for c in value)
            or ":" in value
            or "/" in value
            or "\\" in value
            or ntpath.splitdrive(value)[0]
            or ntpath.basename(value) != value
            or posixpath.basename(value) != value
        ):
            raise ValueError(f"{path}: expected a shader basename in {field!r}")
        if suffix and (not value.lower().endswith(suffix.lower()) or len(value) == len(suffix)):
            raise ValueError(f"{path}: expected a {suffix} filename in {field!r}")
        stem = value[:-len(suffix)] if suffix else value
        if not stem or stem in (".", ".."):
            raise ValueError(f"{path}: expected a shader basename in {field!r}")
        out.append((stem, row))
    return out


def shader_stems(path: str, field: str, suffix: str = "") -> set[str]:
    return {stem for stem, _ in _stem_rows(path, field, suffix)}


def shader_stages(path: str, field: str, suffix: str = "") -> dict[str, str | None]:
    """The stage the index records for each shader, by stem. None where an entry has no stage string or duplicate
    entries disagree: the stage is then unknown, not absent, and the caller says so."""
    stages: dict[str, str | None] = {}
    conflicted: set[str] = set()
    for stem, row in _stem_rows(path, field, suffix):
        stage = row.get("stage")
        val = stage if isinstance(stage, str) and stage else None
        if stem in conflicted:
            continue
        if stem not in stages:
            stages[stem] = val
        elif val is not None:
            prev = stages[stem]
            if prev is None:
                stages[stem] = val
            elif prev != val:
                stages[stem] = None
                conflicted.add(stem)
    return stages
