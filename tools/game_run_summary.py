"""Counts for one game run under Wine (`scripts/run_game_wine.sh`): numbers and the runtime's own names, never log text.

    python -I tools/game_run_summary.py <run folder> [--limit-seconds N] [--top N] [--json]

Reads, in the run folder:

* `stderr.log`: the `[d3d12-trace] #<seq> <method> <outcome>` lines (`--trace-d3d12`) and the `[rt] ...` lines;
* `boot.jsonl`: the startup trace (`--trace`, runtime/README.md "Startup trace");
* `exit_code` and `seconds`: written by the launcher (the process exit code and the run time).

Prints the run time, how the run ended, the exceptions by code, the startup trace events by kind, the D3D12 calls by
outcome, the presents, the most frequent failing calls by method name, whether the runtime served its silent audio
endpoint (`LEONIDA_AUDIO=silent`), and the `[rt]` lines counted by category: the text before the line's first colon,
with every number masked as `#` (`slot 31` counts as `slot #`; a name such as `d3d12` stays) and anything that is not a short list of plain words counted as `(other)`.

It never prints a log line. The logs and traces in the run folder can quote what the title prints (debug strings,
file paths, the title's own messages), so they stay on the machine that made them: share this summary in issues and
pull requests, not the files it reads (AGENTS.md, "No game data"). Method names and outcomes come from fixed strings
in the runtime (runtime/shims/d3d12_trace.h), so they are safe to print.

Standard library only; reads files, never runs them. A missing file counts as empty; a truncated last line (the shape
a crash leaves) is skipped.
"""

from __future__ import annotations

import argparse
import collections
import json
import os
import re
import sys

# The method can hold spaces, so the outcome is the last field (as tools/boot_trace.py reads it).
D3D12_LINE = re.compile(r"^(?:.+ : )?\[d3d12-trace\] #(\d+) (.+) (\S+)\s*$")
D3D12_PROBLEMS = ("failed", "unsupported", "refused", "partial")
PRESENT_SUFFIXES = ("::PresentX", "::Present", "::Present1")
RT_LINE = re.compile(r"^(?:.+ : )?\[rt\] (.*)$")
# A category is a short run of plain words before the first colon: "audio", "present", "xmem pool", "slot 31".
CATEGORY = re.compile(r"^[A-Za-z][A-Za-z0-9_ ./-]{0,39}$")
SILENT_AUDIO = "audio: serving the silent output endpoint"
PACING = re.compile(r"^present: pacing summary frames=(\d+) ")
# The exception codes and module+RVA locations the startup trace writes; anything else is not printed.
EXCEPTION_CODE = re.compile(r"^(?:0x)?[0-9A-Fa-f]{1,8}$")
LOCATION = re.compile(r"^(?:host:)?[A-Za-z0-9_.-]{1,64}\+0x[0-9A-Fa-f]{1,16}$")
TIMEOUT_CODES = (124, 137)  # timeout(1): SIGTERM at the limit, SIGKILL after its grace period


def _lines(path):
    """The file's lines (UTF-8, or UTF-16 with a byte-order mark), or nothing when it is missing."""
    try:
        with open(path, "rb") as raw:
            head = raw.read(2)
    except OSError:
        return
    encoding = "utf-16" if head in (b"\xff\xfe", b"\xfe\xff") else "utf-8-sig"
    with open(path, encoding=encoding, errors="replace") as f:
        yield from f


def _read_int(path):
    try:
        with open(path, encoding="utf-8") as f:
            return int(f.read().strip())
    except (OSError, ValueError):
        return None


def rt_category(text: str) -> str:
    """The category of one `[rt]` line's text, numbers masked; `(other)` when it is not a short list of words."""
    head, colon, _ = text.partition(":")
    head = head.strip()
    if not colon or not CATEGORY.match(head):
        return "(other)"
    return re.sub(r"\b(?:0x[0-9A-Fa-f]+|\d+)\b", "#", head)


def summarize(run: str, limit_seconds: int | None = None, top: int = 12) -> dict:
    calls = collections.Counter()
    bad = {k: collections.Counter() for k in D3D12_PROBLEMS}
    presents = collections.Counter()
    categories = collections.Counter()
    rt_total = silent_audio = 0
    paced_frames = None
    for line in _lines(os.path.join(run, "stderr.log")):
        m = D3D12_LINE.match(line.rstrip("\r\n"))
        if m:
            method, outcome = m.group(2), m.group(3)
            calls[outcome] += 1
            if outcome in bad:
                bad[outcome][method] += 1
            if method.endswith(PRESENT_SUFFIXES):
                presents[f"{method} {outcome}"] += 1
            continue
        m = RT_LINE.match(line.rstrip("\r\n"))
        if m:
            rt_total += 1
            text = m.group(1)
            categories[rt_category(text)] += 1
            if text.startswith(SILENT_AUDIO):
                silent_audio += 1
            p = PACING.match(text)
            if p:
                paced_frames = max(paced_frames or 0, int(p.group(1)))

    kinds = collections.Counter()
    exception_codes = collections.Counter()
    last_exception = None
    exit_event = None
    bad_lines = 0
    for line in _lines(os.path.join(run, "boot.jsonl")):
        if not line.strip():
            continue
        try:
            event = json.loads(line)
        except ValueError:
            bad_lines += 1
            continue
        if not isinstance(event, dict) or "kind" not in event:
            bad_lines += 1
            continue
        kind = str(event["kind"])
        kinds[kind if re.fullmatch(r"[a-z0-9_]{1,40}", kind) else "(other)"] += 1
        if kind == "exception":
            code = str(event.get("code", ""))
            code = code if EXCEPTION_CODE.match(code) else "(other)"
            exception_codes[code] += 1
            at = str(event.get("at", ""))
            last_exception = {"code": code, "at": at if LOCATION.match(at) else None}
        elif kind in ("exit", "error"):
            exit_event = kind, event.get("code") if type(event.get("code")) is int else None

    code = _read_int(os.path.join(run, "exit_code"))
    seconds = _read_int(os.path.join(run, "seconds"))
    # GNU timeout also forwards a child's exit status and can report a SIGKILL before its deadline.
    finished_before_limit = limit_seconds is not None and seconds is not None and 0 <= seconds < limit_seconds
    if code is None:
        end = "no exit code recorded: the run did not start or the session ended first"
    elif code in TIMEOUT_CODES and not finished_before_limit:
        end = f"time limit{f' ({limit_seconds} s)' if limit_seconds else ''} reached, stopped (exit {code})"
    elif exit_event and exit_event[0] == "exit":
        end = f"guest returned {exit_event[1]} (process exit {code})"
    elif exit_event:
        end = f"loader error event (process exit {code})"
    else:
        end = f"process exit {code} (0x{code & 0xFFFFFFFF:08x}), no exit event in the startup trace"

    return {
        "seconds": seconds,
        "exit_code": code,
        "end": end,
        "exceptions": sum(exception_codes.values()),
        "exception_codes": dict(exception_codes.most_common()),
        "last_exception": last_exception,
        "startup_events": dict(kinds.most_common()),
        "startup_bad_lines": bad_lines,
        "d3d12_calls": sum(calls.values()),
        "d3d12_outcomes": dict(calls.most_common()),
        "presents": sum(presents.values()),
        "presents_by_method": dict(presents.most_common()),
        "paced_frames": paced_frames,
        "failing_calls": {k: {"total": sum(v.values()), "methods": len(v), "top": dict(v.most_common(top))}
                          for k, v in bad.items() if v},
        "silent_audio_endpoint": silent_audio,
        "rt_lines": rt_total,
        "rt_categories": dict(categories.most_common()),
    }


def _pairs(counts, sep=" "):
    return ", ".join(f"{k}{sep}{n}" for k, n in counts.items()) or "none"


def render(s: dict, top: int = 12) -> str:
    secs = "unknown" if s["seconds"] is None else f"{s['seconds']} s"
    out = [f"run time: {secs}; end: {s['end']}"]
    exc = f"exceptions traced: {s['exceptions']}"
    if s["exception_codes"]:
        exc += f" (by code: {_pairs(s['exception_codes'])})"
    if s["last_exception"]:
        where = s["last_exception"]["at"] or "(location not printed)"
        exc += f"; last: {s['last_exception']['code']} at {where}"
    out.append(exc)
    events = f"startup trace events: {_pairs(s['startup_events'])}"
    if s["startup_bad_lines"]:
        events += f"; unreadable lines {s['startup_bad_lines']}"
    out.append(events)
    out.append(f"D3D12 calls traced: {s['d3d12_calls']} ({_pairs(s['d3d12_outcomes'])})")
    presents = f"presents: {s['presents']}"
    if s["presents_by_method"]:
        presents += f" ({_pairs(s['presents_by_method'], ': ')})"
    if s["paced_frames"] is not None:
        presents += f"; pacing summary frames {s['paced_frames']}"
    out.append(presents)
    for outcome, row in s["failing_calls"].items():
        more = f" (+{row['methods'] - len(row['top'])} more methods)" if row["methods"] > len(row["top"]) else ""
        out.append(f"{outcome}: {row['total']} -> {_pairs(row['top'])}{more}")
    out.append("silent audio endpoint: " + (f"served ({s['silent_audio_endpoint']} lines)"
                                             if s["silent_audio_endpoint"] else "not seen"))
    cats = list(s["rt_categories"].items())
    shown = ", ".join(f"{k} {n}" for k, n in cats[:top * 2]) or "none"
    more = f" (+{len(cats) - top * 2} more)" if len(cats) > top * 2 else ""
    out.append(f"[rt] lines: {s['rt_lines']} by category: {shown}{more}")
    return "\n".join(out) + "\n"


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="Counts for one scripts/run_game_wine.sh run: run time, end, exceptions, D3D12 calls by "
                    "outcome, presents, the top failing calls and [rt] log categories with digits masked. It never "
                    "prints a log line: the run folder's own logs can quote what the title prints and stay local; "
                    "share this summary, not them.")
    parser.add_argument("run", help="the run folder (stderr.log, boot.jsonl, exit_code, seconds)")
    parser.add_argument("--limit-seconds", type=int, help="the run's time limit, named when it was reached")
    parser.add_argument("--top", type=int, default=12, help="failing methods listed per outcome (default 12)")
    parser.add_argument("--json", action="store_true", help="print the counts as JSON")
    args = parser.parse_args(argv)
    if not os.path.isdir(args.run):
        parser.error(f"no run folder: {args.run}")
    if args.top < 1:
        parser.error("--top must be 1 or more")
    s = summarize(args.run, args.limit_seconds, args.top)
    sys.stdout.write(json.dumps(s, indent=1) + "\n" if args.json else render(s, args.top))
    return 0


if __name__ == "__main__":
    try:  # a redirected stdout is strict ANSI code page under -I; write '?' for what it cannot encode
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError, OSError):
        pass
    sys.exit(main())
