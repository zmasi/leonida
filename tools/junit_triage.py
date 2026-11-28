"""Compare two synthetic or local JUnit reports without exposing logs or paths.

Usage: python -I tools/junit_triage.py before.xml after.xml [--json]
Exit 0: no regressions; 1: regressions; 2: invalid or unreadable input.
Test identities are compared internally but never emitted; outputs contain counts only.
"""
import argparse
from collections import Counter
import json
import math
import sys
import xml.etree.ElementTree as ET

MAX_REPORT_BYTES = 32 * 1024 * 1024
MAX_CASES = 200000
STATUSES = ("passed", "failed", "skipped")


class ReportError(ValueError):
    pass


def read_report(path):
    try:
        with open(path, "rb") as fh:
            raw = fh.read(MAX_REPORT_BYTES + 1)
    except OSError as exc:
        raise ReportError("cannot read report") from exc
    if len(raw) > MAX_REPORT_BYTES:
        raise ReportError("report exceeds size limit")
    # JUnit needs no DTD. Refuse declarations before the XML parser can process entities.
    try:
        xml = raw.decode("utf-8-sig")
    except UnicodeDecodeError as exc:
        raise ReportError("report must be UTF-8") from exc
    if "<!DOCTYPE" in xml.upper() or "<!ENTITY" in xml.upper():
        raise ReportError("XML declarations are not supported")
    try:
        root = ET.fromstring(xml)
    except (ET.ParseError, ValueError, RecursionError) as exc:
        raise ReportError("invalid XML report") from exc
    if root.tag not in ("testsuite", "testsuites"):
        raise ReportError("expected a JUnit testsuite or testsuites")
    rows = {}
    for case in root.iter("testcase"):
        if len(rows) >= MAX_CASES:
            raise ReportError("report exceeds test limit")
        name = case.get("name")
        if not name:
            raise ReportError("testcase has no name")
        key = (case.get("classname", ""), name)
        if key in rows:
            raise ReportError("duplicate testcase identity")
        if (case.get("status") or "").strip().lower() == "disabled":
            status = "skipped"
        else:
            status = "failed" if any(c.tag in ("failure", "error") for c in case) else (
                "skipped" if any(c.tag == "skipped" for c in case) else "passed")
        duration = case.get("time")
        if duration is not None:
            try:
                seconds = float(duration)
            except ValueError as exc:
                raise ReportError("invalid testcase duration") from exc
            if not math.isfinite(seconds) or seconds < 0:
                raise ReportError("invalid testcase duration")
        rows[key] = status
    return rows


def compare(before, after):
    common = before.keys() & after.keys()
    transitions = Counter((before[k], after[k]) for k in common)
    return {
        "before": {s: sum(v == s for v in before.values()) for s in STATUSES},
        "after": {s: sum(v == s for v in after.values()) for s in STATUSES},
        "matched": len(common),
        "added": len(after.keys() - before.keys()),
        "missing": len(before.keys() - after.keys()),
        "regressions": transitions[("passed", "failed")],
        "fixed": transitions[("failed", "passed")],
        "added_failed": sum(after[k] == "failed" for k in after.keys() - before.keys()),
        "skipped_to_failed": transitions[("skipped", "failed")],
        "newly_skipped": sum(transitions[(s, "skipped")] for s in ("passed", "failed")),
        "resumed": sum(transitions[("skipped", s)] for s in ("passed", "failed")),
        "still_failed": transitions[("failed", "failed")],
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("before")
    parser.add_argument("after")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    try:
        result = compare(read_report(args.before), read_report(args.after))
    except ReportError as exc:
        print(f"junit_triage: {exc}", file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        print("Matched: {matched}; regressions: {regressions}; fixed: {fixed}; "
              "added failures: {added_failed}; skipped to failed: {skipped_to_failed}; "
              "newly skipped: {newly_skipped}; resumed: {resumed}; added: {added}; missing: {missing}".format(**result))
    return 1 if result["regressions"] else 0


if __name__ == "__main__":
    try:  # a redirected stdout is strict ANSI code page under -I; write '?' for what it cannot encode
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError, OSError):
        pass
    sys.exit(main())
