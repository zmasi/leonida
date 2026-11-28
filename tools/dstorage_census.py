"""Summarize a local LEONIDA_DSTORAGE_TRACE=1 run in one pass (no filenames in the report).

A trace that cannot be read, or a ds-request line with a missing or malformed field, is one ERROR line and exit
code 2.
"""
import argparse
from collections import Counter, defaultdict
import json
import re
import sys


class TraceError(Exception):
    """A ds-request line the census cannot read: names the line and the field."""


def census(lines, from_file=None):
    """Summarize the trace; a line with a missing or malformed field raises TraceError."""
    seen = [0]

    def counted():
        for line in lines:
            seen[0] += 1
            yield line

    try:
        return _census(counted(), from_file)
    except KeyError as exc:
        raise TraceError(f"line {seen[0]}: ds-request has no {exc.args[0]}=") from None
    except ValueError as exc:
        raise TraceError(f"line {seen[0]}: ds-request has a malformed number ({exc})") from None


def _census(lines, from_file=None):
    groups = defaultdict(Counter)
    errors = Counter()
    priorities = Counter()
    stages = Counter()
    paired = 0
    active = not from_file
    first_tick = last_tick = None
    peak_reservation = drained_peak = None
    for line in lines:
        if "[rt] ds-staging " in line:
            peak = re.search(r"\bpeak_bytes=(\d+)", line)
            if peak:
                peak_reservation = int(peak[1])
                if "idle_bytes=0 " in line:
                    drained_peak = peak_reservation
            continue
        if not active:
            if "[rt] ds-request " not in line:
                continue
            path = re.search(r"\bfile=(.*?) offset=", line)
            if not path or from_file.casefold() not in path[1].casefold():
                continue
            active = True
        if "[rt] ds-notify " in line:
            m = re.search(r"\bus=(\d+)", line)
            if m:
                stages["notify_us"] += int(m[1])
        if "[rt] ds-request " not in line:
            continue
        fields = dict(re.findall(r"\b(\w+)=([^\s]+)", line))
        if "tick_ms" in fields:
            tick = int(fields["tick_ms"])
            first_tick = tick if first_tick is None else min(first_tick, tick)
            last_tick = tick if last_tick is None else max(last_tick, tick)
        o = int(fields["options"], 16)
        codec = (o >> 1) & 15
        tags = ["paged" if fields["dest"] == "pages" else "buffer"]
        if int(fields["zlib"]):
            tags.append("zlib")
        if codec:
            tags.append("BC6H" if codec == 6 else f"BC{codec}")
        if int(fields["sw"]):
            tags.append("swizzle" + fields["sw"])
        if len(tags) == 1:
            tags.append("plain")
        g = groups[" ".join(tags)]
        g["requests"] += 1
        g["source_bytes"] += int(fields["src"])
        g["destination_bytes"] += int(fields["dst"])
        if int(fields["hr"], 16):
            reason = fields["hr"] + " " + fields.get("reason", "unspecified")
            errors[reason] += 1
            g["rejected"] += 1
        elif fields["cancelled"] == "1":
            g["cancelled"] += 1
        else:
            g["successful"] += 1
            g["written_bytes"] += int(fields["dst"])
        if "priority" in fields:
            priorities[fields["priority"]] += 1
        paired += int(fields.get("paired", 0))
        for key in ("read_us", "inflate_us", "bc_us", "sw_us", "copy_us"):
            stages[key] += int(fields.get(key, 0))
    totals = sum(groups.values(), Counter())
    interval = None if first_tick is None else last_tick - first_tick
    result = dict(shapes=dict(groups), totals=dict(totals), failures=dict(errors), worker_microseconds=dict(stages),
                  prepared_in_parallel=paired, queue_priorities=dict(priorities), retirement_window_ms=interval)
    if peak_reservation is not None:
        result["process_peak_staging_reservation_bytes"] = peak_reservation
        result["process_drained_through_peak_bytes"] = drained_peak
    if interval:
        result["successful_requests_per_second"] = round(totals["successful"] * 1000 / interval, 2)
        result["written_MiB_per_second"] = round(totals["written_bytes"] * 1000 / (1048576 * interval), 2)
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace")
    parser.add_argument("--from-file", metavar="SUBSTRING",
                        help="start with the first matching source path, then include all queues and files")
    args = parser.parse_args(argv)
    try:
        with open(args.trace, encoding="utf-8", errors="replace") as stream:
            report = census(stream, args.from_file)
    except OSError as exc:  # a missing trace, a folder, no permission: one line, not a traceback
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    except TraceError as exc:
        print(f"ERROR: {args.trace}: {exc}", file=sys.stderr)
        return 2
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:  # a redirected stdout is strict ANSI code page under -I; write '?' for what it cannot encode
        sys.stdout.reconfigure(errors="replace")
    except AttributeError:
        pass
    sys.exit(main())
