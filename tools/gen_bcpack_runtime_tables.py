"""Build C++ codec tables from the shipped Python decoder. Reads no user input."""
import argparse
import os
from pathlib import Path
import struct
import sys
import tempfile

TOOLS = Path(__file__).resolve().parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))
import bcpack as bc  # noqa: E402
import bcpack_tables as tables  # noqa: E402
import bcpack_tables_bptc as bptc  # noqa: E402
from file_mode import published_mode  # noqa: E402
from replace_retry import replace as replace_retry  # noqa: E402
from report_paths import same_report_path  # noqa: E402


def initializer(values):
    if isinstance(values, int):
        return str(values)
    return "{" + ", ".join(initializer(v) for v in values) + "}"


def emit(out, ctype, name, rows):
    print(f"constexpr {ctype} {name} = {{", file=out)
    for row in rows:
        print("    " + initializer(row) + ",", file=out)
    print("};", file=out)


def generate(out):
    emit(out, "uint32_t", "freq[11][16][11]", [
        [tables.PROB_TABLES.get((w, q), ()) for q in range(16)] for w in range(11)
    ])
    emit(out, "int", "widths[8][16]", [bc.LANE_WIDTHS.get(c, ()) for c in range(8)])
    for codec in (1, 3, 4, 5):
        known = getattr(tables, f"BC{codec}_HISTORY_CONFIRMED")
        fitted = getattr(tables, f"BC{codec}_HISTORY_PROVISIONAL")
        emit(out, "Seed", f"history{codec}[256]", [
            (known.get(i, fitted.get(i, (-1,))), int(i not in known and i in fitted)) for i in range(256)
        ])
    for name, rows in (("quad1", tables.BC1_QUADRANT), ("quad3", tables.BC3_COLOUR_QUADRANT),
                       ("pair3", tables.BC3_ALPHA_PAIR), ("pair4", tables.BC4_PAIR), ("pair5", tables.BC5_PAIR)):
        values = [v if isinstance(v, int) else v[0] | (v[1] << 3) for v in rows]
        print(f"constexpr uint8_t {name}[] = {initializer(values)};", file=out)
    for name, rows in (("anchor2", bptc.BC7_ANCHOR2), ("anchor3a", bptc.BC7_ANCHOR3A),
                       ("anchor3b", bptc.BC7_ANCHOR3B)):
        print(f"constexpr uint8_t {name}[] = {initializer(rows)};", file=out)
    cfg = bptc.load_bc7()
    emit(out, "uint8_t", "bc7Config[512][4]", [
        ((cfg["config"][r][0] | (128 if r in cfg["inferred"] else 0)), *cfg["config"][r][1:])
        if r >= 0 else (255, 0, 0, 0)
        for r in cfg["rank_of"]
    ])
    modes, plans = [], []
    for code in bc._BC6H_CODE[1:]:
        trans, base, deltas, regions = bc._BC6H_INFO[code]
        plan, _ = bc._bc6h_plan(code)
        modes.append((code, trans, base, deltas, regions, len(plans), len(plan)))
        plans.extend((c, q, bit) for c, q, bit, pos in plan)
    emit(out, "BC6Mode", "bc6Modes[14]", modes)
    emit(out, "int8_t", "bc6Plan[][3]", plans)


def _validate_output_path(raw_path: str, label: str) -> None:
    if not raw_path.strip():
        raise ValueError(f"{label} path must not be empty")
    if raw_path.endswith(("/", "\\")) or (os.sep and raw_path.endswith(os.sep)):
        raise ValueError(f"{label} is a directory path: {raw_path}")
    abs_path = os.path.abspath(raw_path)
    if os.path.isdir(abs_path):
        raise ValueError(f"{label} is a directory: {abs_path}")
    cur = os.path.dirname(abs_path)
    while cur:
        if os.path.exists(cur):
            if not os.path.isdir(cur):
                raise ValueError(f"{label} parent path is not a directory: {cur}")
            break
        parent = os.path.dirname(cur)
        if parent == cur:
            break
        cur = parent


def _atomic_write(output, writer, *, binary: bool, label: str = "output") -> None:
    out_str = os.fspath(output)
    _validate_output_path(out_str, label)
    dest = os.path.realpath(out_str)
    parent = os.path.dirname(dest) or "."
    os.makedirs(parent, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=parent, prefix=".bcpack-", suffix=".tmp")
    try:
        try:
            fh = os.fdopen(fd, "wb") if binary else os.fdopen(fd, "w", encoding="utf-8", newline="\n")
        except BaseException:
            os.close(fd)
            raise
        with fh:
            writer(fh)
            fh.flush()
            os.fsync(fh.fileno())
        os.chmod(tmp, published_mode(dest))
        replace_retry(tmp, dest)
    except BaseException:
        try:
            os.chmod(tmp, 0o600)
        except OSError:
            pass
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def write_profile(source, output):
    """Serialize local calibration JSON. Preserve both the available and confirmed precision."""
    out_str = os.fspath(output)
    _validate_output_path(out_str, "--profile OUT")
    if isinstance(source, (str, os.PathLike)):
        src_str = os.fspath(source)
        if same_report_path(src_str, out_str):
            raise ValueError(f"--profile output would overwrite input {src_str}")
        if os.path.isdir(src_str):
            for fname in ("bc6h.json", "bc7.json"):
                cand = os.path.join(src_str, fname)
                if os.path.isfile(cand) and same_report_path(cand, out_str):
                    raise ValueError(f"--profile output would overwrite input {cand}")
    ep, ix, ec, ic, ep_cb, ix_cb = bptc.load_bc6h(source, with_confirmed_bits=True)
    b7 = bptc.load_bc7(source)

    def _write_bin(out):
        out.write(b"BCPHIST1")
        for i in range(256):
            out.write(struct.pack("<12H", *(v for channel in ep[i] for v in channel)))
            out.write(bytes(ix[i]))
            out.write(bytes(v for channel in ep_cb[i] for v in channel))
            out.write(bytes(ix_cb[i]))
            out.write(bytes((ec[i], ic[i])))
        for i in range(256):
            for key in ("history_endpoints", "history_endpoint_bits", "history_endpoint_confirmed_bits",
                        "history_indices", "history_index_bits", "history_index_confirmed_bits"):
                out.write(bytes(b7[key][i]))

    _atomic_write(output, _write_bin, binary=True, label="--profile OUT")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", nargs="?", help="the C++ tables to write (bcpack_tables.inc)")
    parser.add_argument("--profile", nargs=2, metavar=("JSON", "OUT"),
                        help="serialize a local calibration JSON to OUT instead of writing the tables")
    args = parser.parse_args(argv)
    if args.profile and args.output:
        parser.error("cannot combine positional output with --profile")
    if not args.profile and not args.output:
        parser.error("name the tables file to write, or use --profile")
    if args.profile:
        try:
            write_profile(*args.profile)
        except (OSError, ValueError, KeyError, TypeError, RecursionError) as e:
            print(f"gen_bcpack_runtime_tables: {e}", file=sys.stderr)
            return 2
    else:
        try:
            _validate_output_path(args.output, "output")
        except ValueError as e:
            print(f"gen_bcpack_runtime_tables: {e}", file=sys.stderr)
            return 2
        try:
            _atomic_write(args.output, generate, binary=False, label="output")
        except OSError as e:
            print(f"gen_bcpack_runtime_tables: {e}", file=sys.stderr)
            return 2
    return 0


if __name__ == "__main__":
    try:  # a redirected stdout is strict ANSI code page under -I; write '?' for what it cannot encode
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError, OSError):
        pass
    sys.exit(main())
