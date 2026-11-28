"""Count title and artist labels for trackid markers in extracted AWC banks."""

import argparse
import os
from pathlib import Path
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from audio_config import joaat  # noqa: E402
from awc_markers import EVENT, TAG_MARKER, events  # noqa: E402
from rsc8_audio import parse_adat_at_file  # noqa: E402
from text_tables import MAX_RESOURCE_BYTES, string_tables  # noqa: E402

TRACK_ID = joaat("trackid")


def track_numbers(path):
    with path.open("rb") as source:
        bank = parse_adat_at_file(source, 0, os.fstat(source.fileno()).st_size)
        if bank is None:
            raise ValueError("invalid AWC bank")
        for _, tags in bank["entries"]:
            for kind, offset, size in tags:
                if kind != TAG_MARKER:
                    continue
                source.seek(offset)
                chunk = source.read(size)
                if len(chunk) != size or size % EVENT.size:
                    raise ValueError("incomplete marker chunk")
                for event_type, bits, _, flags in events(chunk):
                    if event_type == TRACK_ID and flags == 0:
                        number, = struct.unpack("<f", bits.to_bytes(4, "little"))
                        if not number.is_integer() or number < 0:
                            raise ValueError("invalid track number")
                        yield int(number)


def counts(banks, table):
    with table.open("rb") as source:
        data = source.read(MAX_RESOURCE_BYTES + 1)
    if len(data) > MAX_RESOURCE_BYTES:
        raise ValueError("string table resource too large")
    labels, = string_tables(data)
    numbers = set()
    rows = 0
    for bank in banks:
        for number in track_numbers(bank):
            rows += 1
            numbers.add(number)
    result = {"marker_rows": rows, "distinct_numbers": len(numbers)}
    for name, suffix in (("title", "s"), ("artist", "a")):
        matches = [labels.get(joaat(f"{number}{suffix}")) for number in numbers]
        result.update({
            f"{name}_present": sum(bool(text) for text in matches),
            f"{name}_empty": matches.count(""),
            f"{name}_absent": matches.count(None),
        })
    return result


class ArgumentParser(argparse.ArgumentParser):
    def error(self, message):
        # argparse's default message can include argument values and file paths.
        self.exit(2, "track_labels: invalid arguments (see --help)\n")


def main(argv=None):
    parser = ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("banks", nargs="+", type=Path, help="paths to extracted AWC banks")
    parser.add_argument("--table", action="append", required=True, type=Path,
                        help="one extracted resource containing exactly one string table")
    args = parser.parse_args(argv)
    if len(args.table) != 1:
        parser.error("expected exactly one table")
    try:
        report = counts(args.banks, args.table[0])
    except (OSError, ValueError):
        print("track_labels: could not read valid AWC banks and one string table", file=sys.stderr)
        return 1
    output = "".join(f"{name} {value}\n" for name, value in report.items())
    sys.stdout.buffer.write(output.encode("ascii"))
    return 0


if __name__ == "__main__":
    sys.stdout.reconfigure(errors="replace")
    sys.exit(main())
