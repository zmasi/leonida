"""Make synthetic DirectStorage requests with the test rANS encoder and the reference BCn decoder."""
import argparse
import io
import os
from pathlib import Path
import shutil
import sys
import tempfile
import time
import zlib

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "tests"))
import test_bcpack as enc


def build(codec):
    block_count = 64
    single = codec in (1, 4)
    count = block_count if single else 2 * block_count
    lanes = [[0] * count for _ in enc.bcpack.LANE_WIDTHS[codec]]
    for k in range(2, 11 if codec >= 6 else 12):
        for i in range(count):
            lanes[k][i] = (i % 4 + k) % 5
    if codec == 6:
        for i in range(block_count):
            mode = i % 14
            lanes[15][2*i:2*i+2] = (mode, i % 32)
            bits = 3 if mode < 10 else 4
            # The initial half-range index needs a negative delta to reach zero.
            if i:
                lanes[1][2*i:2*i+2] = enc._deint4(1)
            delta = 0 if i else (1 << bits) - 1
            for y in range(4):
                lanes[11+y][2*i:2*i+2] = [enc._pack_ix_pair(delta, delta, y)] * 2
    if codec == 7:
        # Use all eight modes with zero p-bits and legal anchor indices.
        cfg = enc.bptc.load_bc7()
        for i in range(block_count):
            rank = next(r for r, v in enumerate(cfg["config"])
                        if v[0] == i % 8 and r not in cfg["inferred"])
            lanes[15][2*i:2*i+2] = divmod(cfg["rank_of"].index(rank), 16)
        for k in (2, 6, 7, 8, 10):
            lanes[k] = [0] * count
    # Select the smallest model encoding per lane so both bare and wrapped requests
    # meet DirectStorage's requirement that the destination exceed the source.
    widths = enc.bcpack.LANE_WIDTHS[codec]
    q = [min(range(16), key=lambda q: len(enc._encode_fragment(symbols, w, w, q)[0]))
         for symbols, w in zip(lanes, widths)]
    raw = enc.encode_lanes(lanes, codec, q=q)
    plain = enc.bcpack.decode(raw, codec, 8 * count)
    assert len(raw) < len(plain) and len(zlib.compress(raw)) < len(plain)
    return raw, plain


def write(out):
    for codec in (1, 3, 4, 5, 6, 7):
        raw, plain = build(codec)
        for name, data in (("raw", raw), ("plain", plain), ("wrapped", zlib.compress(raw))):
            print(f"static constexpr uint8_t {name}{codec}[] = {{", file=out)
            for i in range(0, len(data), 24):
                print("    " + ", ".join(map(str, data[i:i+24])) + ",", file=out)
            print("};", file=out)
    for codec in (6, 7):
        lanes = [[0] * 64 for _ in range(16)]
        cfg = enc.bptc.load_bc7()
        rank = cfg["config"].index((6, 0, 0, 0))
        for block in range(32):
            lanes[1][2*block:2*block+2] = enc._deint4(1)
            if codec == 7:
                lanes[15][2*block:2*block+2] = divmod(cfg["rank_of"].index(rank), 16)
        history = {"bc6h": {"endpointHistory": {255: [[4096, 0, 0, 0]] * 3}, "indexHistory": {255: [0]*16},
                             "endpointClass": {255: 3}, "indexClass": {255: 3}},
                   "bc7": {"historyEndpoints": {255: [64]*24}, "historyEndpointBits": {255: [8]*24},
                           "historyEndpointConfirmedBits": {255: [8]*24}, "historyIndices": {255: [0]*32},
                           "historyIndexBits": {255: [4]*32}, "historyIndexConfirmedBits": {255: [4]*32}}}
        packed = enc.encode_lanes(lanes, codec, q=10)
        expected = enc.bcpack.decode(packed, codec, 512, history=history)
        assert expected[:16] * 32 == expected
        for label, data in (("profile_raw", packed), ("profile_first", expected[:16])):
            print(f"static constexpr uint8_t {label}{codec}[] = {{{', '.join(map(str, data))}}};", file=out)
    seed = next(iter(enc.tables.BC1_HISTORY_PROVISIONAL))
    lanes = [[0] * 64 for _ in range(12)]
    distance = 256 - seed
    lanes[0][0] = enc._zig(distance if distance < 128 else distance - 256)
    raw = enc.encode_lanes(lanes, 1, q=12)
    plain, info = enc.bcpack.decode_with_info(raw, 1, 512, allow_provisional=True)
    assert info
    print(f"static constexpr uint8_t fitted_raw[] = {{{', '.join(map(str, raw))}}};", file=out)
    print(f"static constexpr uint8_t fitted_first[] = {{{', '.join(map(str, plain[:8]))}}};", file=out)
    print("static constexpr Vector vectors[] = {", file=out)
    for c in (1, 3, 4, 5, 6, 7):
        print(f"    {{{c}, raw{c}, plain{c}, wrapped{c}}},", file=out)
    print("};", file=out)


def _validate_output_path(raw: str, parser: argparse.ArgumentParser) -> Path:
    if not raw.strip():
        parser.error("output path must not be empty")
    if raw.endswith(("/", "\\")) or (os.sep and raw.endswith(os.sep)):
        parser.error(f"output must be a file path, not a directory: {raw}")
    for cand in (Path(os.path.abspath(raw)), Path(os.path.realpath(raw))):
        if (cand.exists() or cand.is_symlink()) and cand.is_dir():
            parser.error(f"output path is a directory: {raw}")
        anc = cand.parent
        while str(anc) and not (anc.exists() or anc.is_symlink()):
            if anc.parent == anc:
                break
            anc = anc.parent
        if str(anc) and not anc.is_dir():
            parser.error(f"output parent is not a directory: {anc}")
    return Path(raw)


def write_file_atomic(path: Path | str, text: str) -> None:
    """Atomically write UTF-8 LF text to `path`, preserving existing content on failure."""
    dest = Path(os.path.realpath(path))
    dest.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=dest.parent, prefix=".dstorage-vectors-", suffix=".tmp")
    try:
        try:
            f = os.fdopen(fd, "w", encoding="utf-8", newline="\n")
        except BaseException:
            os.close(fd)
            raise
        with f:
            f.write(text)
            f.flush()
            os.fsync(f.fileno())
        if os.name == "posix":
            try:
                shutil.copymode(dest, tmp)
            except FileNotFoundError:
                pass
        for delay in (0.01, 0.02, 0.04) if os.name == "nt" else ():
            try:
                os.replace(tmp, dest)
                return
            except PermissionError as exc:
                if getattr(exc, "winerror", None) not in (5, 32, 33):
                    raise
                time.sleep(delay)
        os.replace(tmp, dest)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", help="header file to write")
    args = parser.parse_args(argv)
    out_path = _validate_output_path(args.output, parser)
    buf = io.StringIO()
    write(buf)
    try:
        write_file_atomic(out_path, buf.getvalue())
    except OSError as exc:
        print(f"ERROR: cannot write {out_path}: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
