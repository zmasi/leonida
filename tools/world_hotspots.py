#!/usr/bin/env python3
"""Rank the densest cells of the placed world, to pick a teleport target (for example the high-rise cluster of the city).

Reads the rows ``world_index.py --entities-out`` writes (one JSON line per placed entity; the file is derived from
your own copy of the game and stays in your local work folder) and counts the full-detail placements (``lod_level``
0 and 5, the HD and ORPHAN_HD tiers of docs/world.md) in square cells of ``--cell`` metres (RAGE world x y, Z-up).
It prints the densest cells with the placements' mean x y, the median and 90th-percentile z, and a suggested
target ``x,y,z`` (the median ground height of the cell plus ``--above`` metres). ``--out FILE`` writes that
target of the densest cell (or of ``--pick N``) as ``x,y,z``, which the runtime reads from
``LEONIDA_VICE_CITY`` or from ``%TEMP%\\leonida-player\\vice.txt`` (docs/mod-menu.md).

Nothing in the output is game content beyond a few coordinates; do not commit it. Standard library only.
Usage: ``python -I tools/world_hotspots.py <work>/entities.jsonl [--cell 100] [--top 15] [--bbox x0 y0 x1 y1]``.
"""
import argparse
import json
import math
import os
import sys

FULL_DETAIL = (0, 5)


def cells(path, cell, bbox):
    """{(i, j): [xs, ys, zs]} of the full-detail placements, and the row counts."""
    out = {}
    rows = kept = 0
    with open(path, encoding="utf-8") as f:
        for line in f:
            rows += 1
            try:
                row = json.loads(line)
                x, y, z = (float(v) for v in row["position"])
            except (ValueError, KeyError, TypeError):
                continue
            if row.get("lod_level") not in FULL_DETAIL or not all(map(math.isfinite, (x, y, z))):
                continue
            if bbox and not (bbox[0] <= x <= bbox[2] and bbox[1] <= y <= bbox[3]):
                continue
            kept += 1
            c = out.setdefault((math.floor(x / cell), math.floor(y / cell)), ([], [], []))
            c[0].append(x)
            c[1].append(y)
            c[2].append(z)
    return out, rows, kept


def quantile(values, q):
    v = sorted(values)
    return v[min(len(v) - 1, int(q * len(v)))]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("entities", help="world_index.py --entities-out file (JSON lines)")
    ap.add_argument("--cell", type=float, default=100.0, help="cell size in metres (default 100)")
    ap.add_argument("--top", type=int, default=15)
    ap.add_argument("--bbox", type=float, nargs=4, metavar=("X0", "Y0", "X1", "Y1"), help="only placements inside")
    ap.add_argument("--above", type=float, default=3.0, help="metres above the median z for the suggested target")
    ap.add_argument("--pick", type=int, default=0, help="which of the listed cells --out writes (0 = densest)")
    ap.add_argument("--out", help="write the picked target as x,y,z")
    args = ap.parse_args(argv)
    if not (math.isfinite(args.cell) and args.cell > 0) or args.top < 1:
        ap.error("--cell must be positive and --top at least 1")
    try:
        found, rows, kept = cells(args.entities, args.cell, args.bbox)
    except OSError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 2
    if not found:
        print("ERROR: no full-detail placement in the file", file=sys.stderr)
        return 2
    ranked = sorted(found.items(), key=lambda kv: -len(kv[1][0]))[: args.top]
    print(f"{rows} rows, {kept} full-detail placements in {len(found)} cells of {args.cell:g} m")
    targets = []
    for n, ((i, j), (xs, ys, zs)) in enumerate(ranked):
        mx, my = sum(xs) / len(xs), sum(ys) / len(ys)
        # Ground: the placements nearest the mean position, not the cell's roofs.
        near = sorted(range(len(xs)), key=lambda k: (xs[k] - mx) ** 2 + (ys[k] - my) ** 2)[: max(5, len(xs) // 10)]
        ground = quantile([zs[k] for k in near], 0.5)
        target = (round(mx, 1), round(my, 1), round(ground + args.above, 1))
        targets.append(target)
        print(
            f"#{n}: cell ({i},{j}) x {i * args.cell:.0f}..{(i + 1) * args.cell:.0f} y {j * args.cell:.0f}..{(j + 1) * args.cell:.0f}: "
            f"{len(xs)} placements, mean ({mx:.1f}, {my:.1f}), z median {quantile(zs, 0.5):.1f} p90 {quantile(zs, 0.9):.1f}, "
            f"target {target[0]},{target[1]},{target[2]}"
        )
    if args.out:
        if not 0 <= args.pick < len(targets):
            ap.error("--pick is not among the listed cells")
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        with open(args.out, "w", encoding="utf-8", newline="\n") as f:
            f.write("%s,%s,%s\n" % targets[args.pick])
        print(f"wrote #{args.pick} to {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
