"""Statistics of a colour table dumped with LEONIDA_DUMP_VOLUMES=1.

The runtime saves `volume-<w>x<h>x<d>-<frame>-<n>.bin` to the LEONIDA_CAPTURE
folder: raw little-endian half floats, RGBA per texel, x fastest, then y, then z.
This prints how many texels are distinct, the share of neighbour pairs that are
exactly equal along each axis, and whether the table is stepped (runs of equal
neighbours) or smooth. Standard library only.

    python -I tools/volume_stats.py volume-33x33x33-1200-0.bin [...]
"""

import os
import re
import struct
import sys

STEPPED_ABOVE = 0.1


def stats(raw, w, h, d):
    """(texels, distinct, zero, [equal share along x, y, z]) of RGBA half floats."""
    texels = w * h * d
    if len(raw) != texels * 8:
        raise ValueError("%d bytes, but %dx%dx%d RGBA16F is %d" % (len(raw), w, h, d, texels * 8))
    cells = [raw[i * 8 : i * 8 + 8] for i in range(texels)]
    zero_cell = bytes(8)
    strides = (1, w, w * h)
    dims = (w, h, d)
    equal = []
    for axis in range(3):
        pairs = same = 0
        for z in range(d):
            for y in range(h):
                for x in range(w):
                    if (x, y, z)[axis] + 1 >= dims[axis]:
                        continue
                    i = (z * h + y) * w + x
                    pairs += 1
                    same += cells[i] == cells[i + strides[axis]]
        equal.append(same / pairs if pairs else 0.0)
    return texels, len(set(cells)), sum(c == zero_cell for c in cells), equal


def halves(raw, count):
    return struct.unpack("<%de" % count, raw[: count * 2])


def report(path):
    name = os.path.basename(path)
    m = re.match(r"volume-(\d+)x(\d+)x(\d+)-", name)
    if not m:
        print("%s: name is not volume-<w>x<h>x<d>-..." % name)
        return
    w, h, d = map(int, m.groups())
    with open(path, "rb") as f:
        raw = f.read()
    texels, distinct, zero, equal = stats(raw, w, h, d)
    print("%s: %dx%dx%d, %d texels, %d distinct, %d all-zero" % (name, w, h, d, texels, distinct, zero))
    steps = []
    for axis, share in zip("xyz", equal):
        kind = "STEPPED" if share > STEPPED_ABOVE else "smooth"
        print("  along %s: %.1f%% of neighbour pairs equal (%s)" % (axis, share * 100, kind))
        if share > STEPPED_ABOVE:
            steps.append(axis)
    # The red channel along the diagonal, to see the ramp's shape.
    diag = [halves(raw[((i * h + i) * w + i) * 8 :], 4) for i in range(min(w, h, d))]
    print("  diagonal red: " + " ".join("%.4g" % t[0] for t in diag[:: max(1, len(diag) // 12)]))
    print("  verdict: " + ("stepped along " + ",".join(steps) if steps else "smooth along every axis"))


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    for path in argv:
        report(path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
