"""Count and check the instanced props and grass in map resources (CMapData instancedData).

Map resources (docs/world.md) can hold two more list structs besides CEntityDef. Their names are the
case-sensitive joaat of the GTA V class names, and the resource's own schema gives every member offset:

    0xBA049770 rage__fwPropInstanceListDef               (0x170): CEntityDef members, InstanceList @0x90,
                                                          BatchAABB @0xA0, ScaleRange @0x160, InstanceLodDist @0x168
    0xEA6B3A96 rage__fwPropInstanceListDef__InstanceData (0x40):  InstMat @0 (3x4 floats, translation in the last
                                                          column), Tint @0x34
    0x7C475B5D rage__fwGrassInstanceListDef              (0x1A0): CEntityDef members, BatchAABB @0x80,
                                                          ScaleRange @0xA0, LodFadeStartDist @0xB8,
                                                          InstanceLodDist @0xBC, OrientToTerrain @0xC0,
                                                          IsSLODList @0x150, InstanceList @0x178
    0xED86F522 rage__fwGrassInstanceListDef__InstanceData (0x10): Position @0 (3 x u16 in BatchAABB), NormalX @6,
                                                          NormalY @7, Color @8 (3 bytes), Scale @0xB, ScaleZ @0xC,
                                                          RotationIdx @0xD

Fields are read through the schema member table (by name hash), never at fixed offsets, and the tool counts how
often the schema agrees with the offsets above. An InstanceList is an array: a u64 pointer whose low 20 bits are
the 1-based block index and whose bits 20+ are the byte offset in that block, then a u16 count. The block table
entry places the block in the virtual (0x5...) or physical (0x6...) buffer of the resource; the buffers are the
first v and next p zlib streams of the resource, with v and p in the block map (+0x08 -> +8, +9).

Only counts and aggregate checks are written. No position, matrix, name or other value leaves the tool.

Usage:
    python -I tools/instance_lists.py <archive.rpf> [...] [--start N] [--limit BYTES] [--json out.json]
"""
import argparse
import json
import math
import mmap
import os
import struct
import sys
import zlib
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import world_index as wi  # noqa: E402  (same 0x7a header, schema and stream helpers)

PROP_LIST, PROP_INST = 0xBA049770, 0xEA6B3A96
GRASS_LIST, GRASS_INST = 0x7C475B5D, 0xED86F522
STRUCT_SIZE = {PROP_LIST: 0x170, PROP_INST: 0x40, GRASS_LIST: 0x1A0, GRASS_INST: 0x10}
# member name -> joaat (case-sensitive), and the offset measured in the schema
M = {
    "InstanceList": 0x1C080BB9, "BatchAABB": 0x6ECEBA6E, "ScaleRange": 0xA0664EE6,
    "InstanceLodDist": 0xC0156D12, "LodFadeStartDist": 0x8419A4AA, "OrientToTerrain": 0xC72ADEFA,
    "IsSLODList": 0x4579099D, "lodDist": 0xBF74DFA7, "lodLevel": 0x6C8F1715, "archetypeName": 0xA023A02C,
    "InstMat": 0x7EE6CC43, "Tint": 0x2D5482DF,
    "Position": 0x0C928991, "NormalX": 0xBB0B13F0, "NormalY": 0x1051BE7C, "Color": 0x4E036667,
    "Scale": 0x3CBA3FE6, "ScaleZ": 0x2A728D38, "RotationIdx": 0xBA36796D,
}
EXPECTED = {
    PROP_LIST: {"InstanceList": 0x90, "BatchAABB": 0xA0, "ScaleRange": 0x160, "InstanceLodDist": 0x168},
    PROP_INST: {"InstMat": 0x0, "Tint": 0x34},
    GRASS_LIST: {"BatchAABB": 0x80, "ScaleRange": 0xA0, "LodFadeStartDist": 0xB8, "InstanceLodDist": 0xBC,
                 "OrientToTerrain": 0xC0, "IsSLODList": 0x150, "InstanceList": 0x178},
    GRASS_INST: {"Position": 0x0, "NormalX": 0x6, "NormalY": 0x7, "Color": 0x8, "Scale": 0xB, "ScaleZ": 0xC,
                 "RotationIdx": 0xD},
}


def add(d, k, v=1):
    d[k] = d.get(k, 0) + v


def schema(V):
    """(structs {name: (size, members)}, blocks [(name, size, pointer)]) of a 0x7a resource, or None."""
    if len(V) < 0x50 or V[0x10:0x14] != wi.SIGNATURE or V[0x14] != wi.VERSION:
        return None
    nst, nbl = wi._u16(V, 0x48), wi._u16(V, 0x4C)
    st, it = wi._ptr(wi._u64(V, 0x28), len(V)), wi._ptr(wi._u64(V, 0x38), len(V))
    if st is None or it is None or st + 32 * nst > len(V) or it + 16 * nbl > len(V):
        return None
    structs = {}
    for i in range(nst):
        o = st + 32 * i
        name, size, count = wi._u32(V, o), wi._u32(V, o + 0x18), wi._u16(V, o + 0x1E)
        mem = wi._members(V, wi._ptr(wi._u64(V, o + 0x10), len(V)), count, len(V))
        if mem is None:
            return None
        structs.setdefault(name, (size, mem))
    blocks = [(wi._u32(V, it + 16 * i), wi._u32(V, it + 16 * i + 4), wi._u64(V, it + 16 * i + 8)) for i in range(nbl)]
    return structs, blocks


def resolve(V, P, p):
    """Byte offset and buffer of a 0x5/0x6 resource pointer."""
    if p >> 28 == 5:
        off = p & 0x0FFFFFFF
        return (V, off) if off < len(V) else (None, None)
    if p >> 28 == 6:
        off = p & 0x0FFFFFFF
        return (P, off) if off < len(P) else (None, None)
    return None, None


def array(V, P, blocks, base_buf, at):
    """Follow a PSO array (u64 block pointer + u16 count) to (buffer, offset, count), or None."""
    p = struct.unpack_from("<Q", base_buf, at)[0]
    count = struct.unpack_from("<H", base_buf, at + 8)[0]
    if count == 0:
        return "empty"
    k = wi.split_pointer(p)[0] - 1
    if not 0 <= k < len(blocks):
        return None
    buf, boff = resolve(V, P, blocks[k][2])
    if buf is None:
        return None
    off = boff + (p >> 20)
    return buf, off, count


def member(structs, name, field):
    size, mem = structs.get(name, (0, {}))
    m = mem.get(M[field])
    return m[0] if m else None


def member_or(structs, name, field, default):
    off = member(structs, name, field)
    return default if off is None else off


def measure(V, P, T):
    sc = schema(V)
    if sc is None:
        return False
    structs, blocks = sc
    if PROP_LIST not in structs and GRASS_LIST not in structs:
        return False
    add(T, "resources_with_lists")
    for sname, size_expected in STRUCT_SIZE.items():
        if sname in structs:
            add(T.setdefault("struct_size_matches", {}), f"{sname:08x}", structs[sname][0] == size_expected)
            for field, off in EXPECTED[sname].items():
                add(T.setdefault("member_offset_matches", {}), f"{sname:08x}.{field}",
                    member(structs, sname, field) == off)
    for bname, bsize, bptr in blocks:
        if bname not in (PROP_LIST, GRASS_LIST):
            continue
        lsize = structs.get(bname, (0,))[0]   # a block the schema has no struct for is unresolved
        buf, boff = resolve(V, P, bptr)
        if buf is None or lsize == 0 or bsize % lsize:
            add(T, "list_blocks_unresolved")
            continue
        for k in range(bsize // lsize):
            lo = boff + k * lsize
            if bname == PROP_LIST:
                prop_list(V, P, blocks, structs, buf, lo, T.setdefault("prop", {}))
            else:
                grass_list(V, P, blocks, structs, buf, lo, T.setdefault("grass", {}))
    return True


def _f(buf, at, n=1):
    return struct.unpack_from(f"<{n}f", buf, at)


def list_common(structs, name, buf, lo, R):
    add(R, "lists")
    lvl = buf[lo + member(structs, name, "lodLevel")] if member(structs, name, "lodLevel") is not None else None
    add(R.setdefault("lodLevel", {}), str(lvl))
    sr_at = member(structs, name, "ScaleRange")
    if sr_at is not None:
        smin, smax = _f(buf, lo + sr_at, 2)
        add(R, "scale_range_ordered", int(0 < smin <= smax))
    ild = member(structs, name, "InstanceLodDist")
    ld = member(structs, name, "lodDist")
    if ild is not None and ld is not None:
        a, b = _f(buf, lo + ild)[0], _f(buf, lo + ld)[0]
        add(R, "instance_loddist_le_loddist", int(0 < a <= b))
    bb = member(structs, name, "BatchAABB")
    box = None
    if bb is not None:
        mn, mx = _f(buf, lo + bb, 3), _f(buf, lo + bb + 16, 3)
        if all(math.isfinite(x) for x in mn + mx) and all(mn[i] <= mx[i] for i in range(3)):
            add(R, "aabb_valid")
            box = (mn, mx)
    return box


def prop_list(V, P, blocks, structs, buf, lo, R):
    box = list_common(structs, PROP_LIST, buf, lo, R)
    il = member(structs, PROP_LIST, "InstanceList")
    ref = array(V, P, blocks, buf, lo + il) if il is not None else None
    if ref == "empty":
        add(R, "lists_empty")
        return
    if ref is None:
        add(R, "lists_unresolved")
        return
    ib, io, n = ref
    isz = structs.get(PROP_INST, (0x40, {}))[0] or 0x40
    mat = member_or(structs, PROP_INST, "InstMat", 0)
    add(R, "instances", n)
    need = max(48, mat + 48)
    fit = _fit(len(ib), io, isz, need, n) if mat >= 0 else 0
    if fit < n:
        add(R, "instances_truncated")
    degenerate = ok = ortho_n = right_n = uniform_n = unit_n = in_box = 0
    sqrt, isfinite = math.sqrt, math.isfinite
    for m in _matrices(ib, io, fit, isz, mat):
        # Three row-major rows of four floats; the axes are the columns of the 3x3 part.
        # Keep the record-by-record sum() rounding at the classification boundaries.
        l0 = sqrt(sum((m[0] * m[0], m[4] * m[4], m[8] * m[8])))
        l1 = sqrt(sum((m[1] * m[1], m[5] * m[5], m[9] * m[9])))
        l2 = sqrt(sum((m[2] * m[2], m[6] * m[6], m[10] * m[10])))
        if not isfinite(l0 + l1 + l2 + m[3] + m[7] + m[11]) or min(l0, l1, l2) <= 1e-6:
            degenerate += 1
            continue
        a = (m[0] / l0, m[4] / l0, m[8] / l0)
        b = (m[1] / l1, m[5] / l1, m[9] / l1)
        c = (m[2] / l2, m[6] / l2, m[10] / l2)
        dab = abs(sum((a[0] * b[0], a[1] * b[1], a[2] * b[2])))
        dac = abs(sum((a[0] * c[0], a[1] * c[1], a[2] * c[2])))
        dbc = abs(sum((b[0] * c[0], b[1] * c[1], b[2] * c[2])))
        det = (a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0])
               + a[2] * (b[0] * c[1] - b[1] * c[0]))
        ok += 1
        ortho_n += max(dab, dac, dbc) < 0.01
        right_n += det > 0
        top = max(l0, l1, l2)
        uniform_n += top - min(l0, l1, l2) < 0.01 * top
        unit_n += abs(l0 - 1) < 0.01 and abs(l1 - 1) < 0.01 and abs(l2 - 1) < 0.01
        if box:
            mn, mx = box
            in_box += (mn[0] - 0.01 <= m[3] <= mx[0] + 0.01 and mn[1] - 0.01 <= m[7] <= mx[1] + 0.01
                       and mn[2] - 0.01 <= m[11] <= mx[2] + 0.01)
    if degenerate:
        add(R, "matrix_degenerate", degenerate)
    if ok:
        add(R, "matrix_orthonormal_axes", ortho_n)
        add(R, "matrix_right_handed", right_n)
        add(R, "matrix_uniform_scale", uniform_n)
        add(R, "matrix_unit_scale", unit_n)
        if box:
            add(R, "translation_in_aabb", in_box)


def _fit(buf_len, io, isz, need, n):
    """How many of the n records of size isz starting at io still hold need bytes inside the buffer."""
    room = buf_len - io - need
    return 0 if room < 0 else min(n, room // isz + 1)


def _matrices(ib, io, fit, isz, mat):
    """The 12 floats of each of the first fit records, read by one C-level pass when records do not overlap."""
    if fit <= 0:
        return ()
    if isz >= mat + 48 and io + fit * isz <= len(ib):
        rec = struct.Struct(f"<{mat}x12f{isz - mat - 48}x")
        return rec.iter_unpack(memoryview(ib)[io:io + fit * isz])
    return (struct.unpack_from("<12f", ib, io + i * isz + mat) for i in range(fit))


def grass_list(V, P, blocks, structs, buf, lo, R):
    list_common(structs, GRASS_LIST, buf, lo, R)
    for field, key in (("IsSLODList", "IsSLODList"),):
        at = member(structs, GRASS_LIST, field)
        if at is not None:
            add(R.setdefault(key, {}), str(buf[lo + at]))
    ot = member(structs, GRASS_LIST, "OrientToTerrain")
    if ot is not None:
        add(R.setdefault("OrientToTerrain", {}), repr(round(_f(buf, lo + ot)[0], 2)))
    il = member(structs, GRASS_LIST, "InstanceList")
    ref = array(V, P, blocks, buf, lo + il) if il is not None else None
    if ref == "empty":
        add(R, "lists_empty")
        return
    if ref is None:
        add(R, "lists_unresolved")
        return
    ib, io, n = ref
    isz = structs.get(GRASS_INST, (0x10, {}))[0] or 0x10
    nx, ny = member_or(structs, GRASS_INST, "NormalX", 6), member_or(structs, GRASS_INST, "NormalY", 7)
    sz, rot = member_or(structs, GRASS_INST, "ScaleZ", 0xC), member_or(structs, GRASS_INST, "RotationIdx", 0xD)
    sc = member_or(structs, GRASS_INST, "Scale", 0xB)
    need = max(isz, nx + 1, ny + 1, sc + 1, sz + 1, rot + 1)
    add(R, "instances", n)
    fit = _fit(len(ib), io, isz, need, n) if min(nx, ny, sc, sz, rot) >= 0 else 0
    if fit < n:
        add(R, "instances_truncated")
    rmax = 0
    if fit:
        # One strided slice per byte field (the stride is the record size), then counts at C speed: a field
        # pair is counted once per distinct value pair, and equality of two fields is one XOR of whole columns.
        col = {f: ib[io + f:io + f + (fit - 1) * isz + 1:isz] for f in {nx, ny, sc, sz, rot}}
        inside = 0
        pairs = bytearray(2 * fit)
        pairs[0::2], pairs[1::2] = col[nx], col[ny]
        for pair, k in Counter(memoryview(pairs).cast("H")).items():
            bx, by = pair.to_bytes(2, sys.byteorder)   # the two bytes in memory order, whatever the host
            x, y = bx / 127.5 - 1, by / 127.5 - 1
            inside += k * (x * x + y * y <= 1.0 + 2 / 127.5)
        add(R, "normal_xy_inside_unit_disc", inside)
        add(R, "scale_zero", col[sc].count(0))
        diff = int.from_bytes(col[sc], "big") ^ int.from_bytes(col[sz], "big")
        add(R, "scalez_equals_scale", diff.to_bytes(fit, "big").count(0))
        rmax = max(col[rot])
    R["rotation_idx_max"] = max(R.get("rotation_idx_max", 0), rmax)


def resource_buffers(m, s, size):
    """The virtual and physical buffers of the resource whose first stream starts at s."""
    data, used, eof, _ = wi._inflate_stream(m, s, size, 64 << 20, keep=True)
    if not eof or len(data) < 0x20:
        return None, None, max(used, 1)
    bm = wi._ptr(wi._u64(data, 8), len(data))
    if bm is None or bm + 10 > len(data):
        return data, b"", used
    vc, pc = max(1, data[bm + 8]), data[bm + 9]
    parts, total, pos = [data], used, s + used
    for _ in range(vc + pc - 1):
        nxt = (pos + 15) & ~15
        if nxt >= size or not wi.ZHDR.match(m, nxt, size):
            break
        more, mused, meof, _ = wi._inflate_stream(m, nxt, size, 64 << 20, keep=True)
        if not meof:
            break
        parts.append(more)
        total = nxt + mused - s
        pos = nxt + mused
    V = b"".join(parts[:vc])
    P = b"".join(parts[vc:vc + pc])
    return V, P, total


def scan(path, T, start=None, limit=None):
    with open(path, "rb") as f:
        if os.fstat(f.fileno()).st_size < 8:
            raise ValueError(f"{path}: not an RPF9 archive")
        with mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as m:
            size = len(m)
            if m[:4] not in (b"RPF9", b"9FPR"):
                raise ValueError(f"{path}: not an RPF9 archive")
            data_start = wi._u32(m[:8], 4)
            if not 8 <= data_start <= size:
                raise ValueError(f"{path}: RPF9 data start is outside the archive")
            if start is not None and not data_start <= start < size:
                raise ValueError(f"--start 0x{start:x} is outside the data area of {path}")
            pos = start if start is not None else data_start
            end = min(size, pos + limit) if limit is not None else size
            while pos < end:
                found = wi.ZHDR.search(m, pos, end)
                if not found:
                    break
                s = found.start()
                if s % 16:
                    pos = s + 1
                    continue
                try:
                    head = zlib.decompressobj().decompress(m[s:s + 4096], 0x15)
                except zlib.error:
                    pos = s + 1
                    continue
                if head[0x10:0x15] != b"0DRPz":
                    pos = s + 1
                    continue
                try:
                    V, P, used = resource_buffers(m, s, size)
                except zlib.error:
                    pos = s + 1
                    continue
                add(T, "map_resources_read")
                if V is not None:
                    try:
                        measure(V, P, T)
                    except (struct.error, IndexError):
                        add(T, "resources_truncated")
                pos = s + max(used, 1)


def same_output_path(first, second):
    if os.path.normcase(os.path.realpath(first)) == os.path.normcase(os.path.realpath(second)):
        return True
    try:
        return os.path.samefile(first, second)
    except OSError:
        return False


def _int_arg(text):
    try:
        return int(text, 0)
    except ValueError:
        raise argparse.ArgumentTypeError(f"invalid int value: {text!r}") from None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("inputs", nargs="+", help=".rpf archives")
    ap.add_argument("--start", type=_int_arg)
    ap.add_argument("--limit", type=_int_arg)
    ap.add_argument("--json")
    args = ap.parse_args(argv)
    if args.start is not None and args.start < 0:
        print("error: --start must be >= 0", file=sys.stderr)
        return 2
    if args.limit is not None and args.limit <= 0:
        print("error: --limit must be > 0", file=sys.stderr)
        return 2
    # The report is written after the whole scan, so check its path first: an archive given as --json used to be
    # replaced by the report (exit 0), and a directory ended the run in a traceback.
    if args.json:
        if os.path.isdir(args.json):
            print(f"error: --json {args.json} is a directory", file=sys.stderr)
            return 2
        for path in args.inputs:
            if same_output_path(args.json, path):
                print(f"error: --json would overwrite the input {path}", file=sys.stderr)
                return 2
    T = {}
    for path in args.inputs:
        if not os.path.isfile(path):
            print(f"error: {path} is not a file", file=sys.stderr)
            return 2
        try:
            scan(path, T, args.start, args.limit)
        except (ValueError, OSError) as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 2
    print(json.dumps(T, indent=1, sort_keys=True))
    if args.json:
        try:
            os.makedirs(os.path.dirname(os.path.abspath(args.json)), exist_ok=True)
            with open(args.json, "w", encoding="utf-8", newline="\n") as fh:
                json.dump(T, fh, indent=1, sort_keys=True)
        except OSError as exc:
            print(f"error: cannot write {args.json}: {exc.strerror or exc}", file=sys.stderr)
            return 2
    return 0


if __name__ == "__main__":
    try:  # a redirected stdout is strict ANSI code page under -I; write '?' for what it cannot encode
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError, OSError):
        pass
    sys.exit(main())
