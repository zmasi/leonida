#!/usr/bin/env python3
"""Builds the keyboard-and-mouse button prompt movie from a folder of pictures the player names.

The title has one set of button prompt glyphs, a Scaleform movie of 38 one-frame vector sprites
``platform_xbsx.input_icons.Main_Button_<N>`` (N = 4..39, 58, 59: see docs/prompt_glyphs.md). This tool turns a
set of flat-coloured pictures, one PNG per N, into the vector shapes of such a movie; the runtime (leonida-hle,
`glyphs`) serves it in place of the title's own while the keyboard and mouse are in use. Standard library only.

    # over the title's own movie (the confirmed way: its ActionScript, export and class tables are kept, only the
    # drawings change). Reads the stream from your own data.rpf, read-only; the output holds those tags of the
    # game's movie, so keep it on your PC and never commit it.
    python -I tools/pc_prompt_glyphs.py --glyphs <folder> --from-archive "<package>/data.rpf" --out pc_prompt_glyphs.gfx

    # standalone, with the ActionScript block of tools/ps_prompt_glyphs.py (never run in the title)
    python -I tools/pc_prompt_glyphs.py --glyphs <folder> --pad-inflated 28148 --out pc_prompt_glyphs.gfx

    python -I tools/pc_prompt_glyphs.py --glyphs <folder> --sheet sheet.png    # the vector result, drawn back
    python -I tools/pc_prompt_glyphs.py --selftest                             # needs no pictures and no game file

The pictures: ``<folder>/platform_pc.input_icons.Main_Button_<N>.png`` (any ``*Main_Button_<N>.png`` is taken),
8-bit, square, the 64 x 64 glyph canvas with one more canvas pixel on every side, drawn at a whole multiple (4x is
264 x 264). Three flat colours: black, white and the grey (103, 103, 103), with anti-aliased edges, transparent
where nothing is drawn. The pictures are the player's own: nothing here ships them, and the movie built from them
stays on the player's PC.

How a picture becomes shapes:

1. every pixel is split into how much of it is covered at all, by white and by grey (an edge pixel is a mix of
   the two flat colours nearest to it);
2. the half-covered line of each of the three is followed between the pixel centres (marching squares), which
   places an edge to a fraction of a pixel;
3. each closed line is one painted piece: the outline of everything opaque in black, white and grey areas on top
   of it, what is black inside those (a letter) on top again, and so on inwards. Only a hole through the whole
   picture is cut out (of every piece painted over it: where several are, their edges add up and the hole's rim
   comes out a little heavy);
4. each line is fitted with straight edges and quadratic curves that stay within ``--tolerance`` of it, and a corner
   the anti-aliasing rounded is put back where the edges on its two sides cross (`fit_outline`);
5. a line that is the same in several pictures, or at several places, is defined once, and pieces that always
   appear together are one shape.

The movie must be exactly as long as the title's (28,148 bytes) with its own tags kept, which leaves 15,226 bytes
for all shapes and sprites; the tool says how much it used and fails when the set does not fit.

Unless ``--quiet``, the movie just built is read back, every sprite is drawn at the pictures' scale with exact-area
anti-aliasing and compared with its picture: per N, the summed difference in pixels (1.0 is one pixel fully wrong),
that as a share of the picture's opaque area, the pixels more than half wrong and the largest single difference.
Coordinates are whole twips (a fifth of a 4x pixel), so an edge the picture has on a quarter or a third of a pixel
is up to a tenth of a pixel out however it is fitted.
"""

import argparse
import math
import os
import re
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ps_prompt_glyphs as swf  # noqa: E402  (the SWF/GFX writer and the build over the title's movie)

CANVAS = 64  # canvas pixels of a glyph
MARGIN = 1  # canvas pixels of empty border in a picture
GREY = 103  # the grey of unlit parts
RGBA = {"K": (0, 0, 0, 255), "W": (255, 255, 255, 255), "G": (GREY, GREY, GREY, 255)}
TOLERANCE = 0.03  # canvas pixels a fitted edge may stray from the traced line
MOVIE_SIZE = 28148  # the title's movie, inflated

# ---------------------------------------------------------------- PNG


def read_png(data):
    """(width, height, RGBA bytes) of an 8-bit, non-interlaced PNG: grey or RGB, with or without alpha."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    o, idat, head = 8, [], None
    while o + 8 <= len(data):
        n, kind = struct.unpack_from(">I4s", data, o)
        body = data[o + 8:o + 8 + n]
        o += 12 + n
        if kind == b"IHDR":
            head = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            break
    if head is None:
        raise ValueError("a PNG without a header")
    w, h, depth, colour, _, _, interlace = head
    channels = {0: 1, 2: 3, 4: 2, 6: 4}.get(colour)
    if depth != 8 or channels is None or interlace:
        raise ValueError("only 8-bit, non-interlaced grey or RGB PNGs (with or without alpha) are read")
    raw = zlib.decompress(b"".join(idat))
    stride = w * channels
    rows, prev, o = [], bytes(stride), 0
    for _ in range(h):
        kind, line = raw[o], bytearray(raw[o + 1:o + 1 + stride])
        o += 1 + stride
        if kind == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 255
        elif kind == 2:
            line = bytearray((a + b) & 255 for a, b in zip(line, prev))
        elif kind == 3:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 255
        elif kind == 4:
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = prev[i]
                c = prev[i - channels] if i >= channels else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        elif kind:
            raise ValueError("PNG filter %d" % kind)
        rows.append(bytes(line))
        prev = line
    flat = b"".join(rows)
    if channels == 4:
        return w, h, flat
    out = bytearray(w * h * 4)
    if channels == 3:
        out[0::4], out[1::4], out[2::4], out[3::4] = flat[0::3], flat[1::3], flat[2::3], bytes([255]) * (w * h)
    else:
        grey = flat[0::channels]
        out[0::4] = out[1::4] = out[2::4] = grey
        out[3::4] = flat[1::2] if channels == 2 else bytes([255]) * (w * h)
    return w, h, bytes(out)


def write_png(w, h, rgba):
    """The bytes of an 8-bit RGBA PNG."""
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    raw = b"".join(b"\0" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


# ---------------------------------------------------------------- a picture as three coverages


def coverage(w, h, rgba):
    """Per pixel, as lists of rows: how much of it is covered at all, by white and by grey (0..1), and its flat
    colour ("T" transparent, "K", "W", "G"; None on an anti-aliased edge).

    A pixel of exactly the grey is grey only inside a grey area: a black-to-white edge passes through the same
    value, as a line one pixel wide, so grey must survive a 3 x 3 opening."""
    lum = [[(rgba[i] + rgba[i + 1] + rgba[i + 2]) // 3 for i in range(y * w * 4, (y + 1) * w * 4, 4)] for y in range(h)]
    alpha = [list(rgba[y * w * 4 + 3:(y + 1) * w * 4:4]) for y in range(h)]
    kind = [[None] * w for _ in range(h)]
    maybe = []
    for y in range(h):
        for x, (v, a) in enumerate(zip(lum[y], alpha[y])):
            if a == 0:
                kind[y][x] = "T"
            elif a == 255:
                if v <= 1:
                    kind[y][x] = "K"
                elif v >= 254:
                    kind[y][x] = "W"
                elif abs(v - GREY) <= 1:
                    kind[y][x] = "g"
                    maybe.append((x, y))
    around = [(dx, dy) for dy in (-1, 0, 1) for dx in (-1, 0, 1)]
    core = set()
    for x, y in maybe:
        if 0 < x < w - 1 and 0 < y < h - 1 and all(kind[y + dy][x + dx] == "g" for dx, dy in around):
            core.add((x, y))
    for x, y in maybe:
        if not any((x + dx, y + dy) in core for dx, dy in around):
            kind[y][x] = None
    for x, y in maybe:
        if kind[y][x] == "g":
            kind[y][x] = "G"
    cover = [[a / 255 for a in row] for row in alpha]
    white = [[1.0 if k == "W" else 0.0 for k in row] for row in kind]
    grey = [[1.0 if k == "G" else 0.0 for k in row] for row in kind]
    span = 255 - GREY
    for y in range(h):
        for x in range(w):
            if kind[y][x] is not None:
                continue
            near = {}
            for yy in range(max(y - 2, 0), min(y + 3, h)):
                row = kind[yy]
                for xx in range(max(x - 2, 0), min(x + 3, w)):
                    k = row[xx]
                    if k is not None and k != "T":
                        d = (xx - x) ** 2 + (yy - y) ** 2
                        if d < near.get(k, 99):
                            near[k] = d
            pair = set(sorted(near, key=lambda k: (near[k], k))[:2])
            v, a = lum[y][x], alpha[y][x] / 255
            if pair == {"W", "G"}:
                part = min(max((v - GREY) / span, 0.0), 1.0)
                white[y][x], grey[y][x] = part * a, (1 - part) * a
            elif "G" in pair:
                grey[y][x] = min(v / GREY, 1.0) * a
            else:
                white[y][x] = v / 255 * a
    return cover, white, grey, kind


# ---------------------------------------------------------------- level lines

# For each case of a cell's four corners (bit 1 top left, 2 top right, 4 bottom right, 8 bottom left: set when
# covered): the pieces of line through it, each from one side of the cell (Top, Right, Bottom, Left) to another,
# with the covered corners on the right of the direction of travel (y grows downwards).
PIECES = {1: ("TL",), 2: ("RT",), 4: ("BR",), 8: ("LB",), 3: ("RL",), 6: ("BT",), 12: ("LR",), 9: ("TB",),
          7: ("BL",), 11: ("RB",), 13: ("TR",), 14: ("LT",)}
SADDLES = {5: (("TR", "BL"), ("TL", "BR")), 10: (("LT", "RB"), ("RT", "LB"))}  # (centre covered, centre not)


def iso_lines(field, w, h):
    """The closed lines along which `field` (rows of values at the pixel centres, 0 beyond the picture) is 0.5, as
    lists of (x, y) in picture pixels. The covered side is on the right: the outside of a covered area runs
    clockwise on screen (a positive `area`), the edge of a hole in it counter-clockwise."""
    wide = w + 2
    masks = [0] * (h + 2)
    for y in range(h):
        bits = "".join("1" if v >= 0.5 else "0" for v in reversed(field[y]))
        masks[y + 1] = int(bits, 2) << 1 if "1" in bits else 0

    def value(x, y):
        return field[y][x] if 0 <= x < w and 0 <= y < h else 0.0

    nxt = {}
    for cy in range(-1, h):
        r0, r1 = masks[cy + 1], masks[cy + 2]
        v = r0 ^ r1
        active = (r0 ^ (r0 >> 1)) | (r1 ^ (r1 >> 1)) | v | (v >> 1)
        while active:
            low = active & -active
            active ^= low
            i = low.bit_length() - 1
            cx = i - 1
            case = (r0 >> i & 1) | (r0 >> (i + 1) & 1) << 1 | (r1 >> (i + 1) & 1) << 2 | (r1 >> i & 1) << 3
            if case in SADDLES:
                centre = value(cx, cy) + value(cx + 1, cy) + value(cx, cy + 1) + value(cx + 1, cy + 1)
                pieces = SADDLES[case][0 if centre >= 2.0 else 1]
            else:
                pieces = PIECES.get(case, ())
            here = ((cy + 1) * wide + cx + 1) * 2
            side = {"T": here, "L": here + 1, "B": here + 2 * wide, "R": here + 3}
            for a, b in pieces:
                nxt[side[a]] = side[b]

    def between(a, b):
        """Where the edge crosses the line from a pixel centre of coverage `a` to the next one of coverage `b`,
        0..1. The two coverages add up to how much of the two pixels the edge leaves covered, so a straight edge
        across them is placed exactly (interpolating to 0.5 would be up to 0.09 pixels out)."""
        t = 1.5 - a - b if a < b else a + b - 0.5
        return min(max(t, 0.0), 1.0)

    def point(i):
        cell, upright = divmod(i, 2)
        y, x = divmod(cell, wide)
        x, y = x - 1, y - 1
        if upright:
            return (x + 0.5, y + 0.5 + between(value(x, y), value(x, y + 1)))
        return (x + 0.5 + between(value(x, y), value(x + 1, y)), y + 0.5)

    loops = []
    while nxt:
        start = cur = next(iter(nxt))
        pts = []
        while True:
            p = point(cur)
            if not pts or abs(p[0] - pts[-1][0]) + abs(p[1] - pts[-1][1]) > 1e-6:
                pts.append(p)
            cur = nxt.pop(cur)
            if cur == start:
                break
        if len(pts) > 1 and abs(pts[0][0] - pts[-1][0]) + abs(pts[0][1] - pts[-1][1]) <= 1e-6:
            pts.pop()
        if len(pts) >= 3:
            loops.append(pts)
    return loops


def bounds(pts):
    xs, ys = [p[0] for p in pts], [p[1] for p in pts]
    return min(xs), min(ys), max(xs), max(ys)


def inside(p, pts):
    """Whether the point is inside the closed line (even-odd)."""
    x, y = p
    hit = False
    x0, y0 = pts[-1]
    for x1, y1 in pts:
        if (y0 > y) != (y1 > y) and x < x0 + (y - y0) * (x1 - x0) / (y1 - y0):
            hit = not hit
        x0, y0 = x1, y1
    return hit


# ---------------------------------------------------------------- fitting edges to a traced line


def tip(turn):
    """How far from the tip of a corner of that turn (radians) the traced line passes, at most (pixels): the
    anti-aliasing leaves the tip pixel less than half covered, the more so the sharper the corner."""
    return 0.5 / max(math.cos(min(turn, 3.0) / 2), 0.1) + 0.15


def bent(turn):
    """How far along the line from a corner of that turn the anti-aliasing bends it (pixels)."""
    return min(max(1.2 + 0.9 * math.tan(min(turn, 3.0) / 2), 1.5), 6.0)


def bends(pts, reach=2.5, sharp=math.radians(32)):
    """Indices of the points where a closed line turns by more than `sharp` between the chords to the points
    `reach` behind and ahead of it along the line, the strongest of each cluster: its corners, and the middles of
    its tight curves. No edge is fitted across one."""
    n = len(pts)
    steps = [math.hypot(pts[(i + 1) % n][0] - pts[i][0], pts[(i + 1) % n][1] - pts[i][1]) for i in range(n)]
    total = sum(steps)
    if total < 4 * reach:
        return []

    def along(i, direction):
        left, j = reach, i
        while True:
            k = (j + direction) % n
            step = steps[j] if direction > 0 else steps[k]
            if step >= left:
                t = left / step
                return (pts[j][0] + (pts[k][0] - pts[j][0]) * t, pts[j][1] + (pts[k][1] - pts[j][1]) * t)
            left -= step
            j = k

    found = []
    for i in range(n):
        (bx, by), (fx, fy), (x, y) = along(i, -1), along(i, 1), pts[i]
        ax, ay, cx, cy = x - bx, y - by, fx - x, fy - y
        turn = abs(math.atan2(ax * cy - ay * cx, ax * cx + ay * cy))
        if turn >= sharp:
            found.append((turn, i))
    place = [0.0]
    for s in steps[:-1]:
        place.append(place[-1] + s)
    kept = []
    for _, i in sorted(found, reverse=True):
        if all(min(abs(place[i] - place[j]), total - abs(place[i] - place[j])) > 1.2 * reach for j in kept):
            kept.append(i)
    return sorted(kept)


def line_fits(pts, i, j, tol):
    (x0, y0), (x1, y1) = pts[i], pts[j]
    dx, dy = x1 - x0, y1 - y0
    d2 = dx * dx + dy * dy
    if d2 < 1e-12:
        return all(math.hypot(pts[k][0] - x0, pts[k][1] - y0) <= tol for k in range(i + 1, j))
    limit = tol * tol * d2
    for k in range(i + 1, j):
        px, py = pts[k][0] - x0, pts[k][1] - y0
        t = px * dx + py * dy
        if t < 0 or t > d2:
            return False
        cross = px * dy - py * dx
        if cross * cross > limit:
            return False
    return True


def curve_through(start, end, between, tol):
    """The control point of the quadratic curve from `start` to `end` that stays within `tol` of the points
    `between` them (in order), or None."""
    (x0, y0), (x2, y2) = start, end
    run = [start] + list(between) + [end]
    at = [0.0]
    for (ax, ay), (bx, by) in zip(run, run[1:]):
        at.append(at[-1] + math.hypot(bx - ax, by - ay))
    if at[-1] < 1e-9 or len(run) < 3:
        return None
    at = [v / at[-1] for v in at]
    chord = math.hypot(x2 - x0, y2 - y0)
    limit = tol * tol
    for _ in range(3):
        nx = ny = den = 0.0
        for (x, y), u in zip(run[1:-1], at[1:-1]):
            b, a0, a2 = 2 * u * (1 - u), (1 - u) * (1 - u), u * u
            nx += b * (x - a0 * x0 - a2 * x2)
            ny += b * (y - a0 * y0 - a2 * y2)
            den += b * b
        if den < 1e-9:
            return None
        cx, cy = nx / den, ny / den
        if math.hypot(cx - (x0 + x2) / 2, cy - (y0 + y2) / 2) > 2 * chord + 4 * tol:
            return None
        worst = 0.0
        for k in range(1, len(run) - 1):
            (x, y), u = run[k], at[k]
            b, a0, a2 = 2 * u * (1 - u), (1 - u) * (1 - u), u * u
            ex, ey = a0 * x0 + b * cx + a2 * x2 - x, a0 * y0 + b * cy + a2 * y2 - y
            worst = max(worst, ex * ex + ey * ey)
            # One Newton step towards the nearest point of the curve, for the next round.
            d1x, d1y = 2 * ((1 - u) * (cx - x0) + u * (x2 - cx)), 2 * ((1 - u) * (cy - y0) + u * (y2 - cy))
            d2x, d2y = 2 * (x2 - 2 * cx + x0), 2 * (y2 - 2 * cy + y0)
            slope = d1x * d1x + d1y * d1y + ex * d2x + ey * d2y
            if slope > 1e-9:
                at[k] = min(max(u - (ex * d1x + ey * d1y) / slope, 0.0), 1.0)
        if worst <= limit:
            return (cx, cy)
    return None


def sample(i, j, most=23):
    """Indices strictly between i and j, at most `most` of them, evenly spread."""
    count = j - i - 1
    return range(i + 1, j) if count <= most else [i + (j - i) * k // (most + 1) for k in range(1, most + 1)]


def curve_fit(pts, i, j, tol):
    """`curve_through` for pts[i]..pts[j], checked on at most 23 of the points between them."""
    return curve_through(pts[i], pts[j], [pts[k] for k in sample(i, j)], tol)


def furthest(i, end, ok):
    """The furthest j in i + 1..end that `ok` accepts, looked for by doubling the step and then halving the gap
    (i + 1 counts as accepted)."""
    good, step, bad = i + 1, 2, None
    while good < end:
        j = min(i + step, end)
        if ok(j):
            good, step = j, step * 2
        else:
            bad = j
            break
    if bad is None:
        return good
    while bad - good > 1:
        mid = (good + bad) // 2
        if ok(mid):
            good = mid
        else:
            bad = mid
    return good


# A curve record is about 1.6 times a straight one: a curve is taken when it reaches that much further.
CURVE_PAYS = 1.6


def fit_run(pts, a, b, tol):
    """pts[a..b] as edges, each as long as it can be: [[kind, i, j, control], ...], kind "L" or "Q"."""
    out, i = [], a
    while i < b:
        straight = furthest(i, b, lambda j: line_fits(pts, i, j, tol))
        if straight < b:
            controls = {}

            def curved(j):
                controls[j] = curve_fit(pts, i, j, tol)
                return controls[j] is not None

            bent = furthest(i, b, curved)
            if controls.get(bent) is not None and bent - i >= CURVE_PAYS * (straight - i):
                out.append(["Q", i, bent, controls[bent]])
                i = bent
                continue
        out.append(["L", i, straight, None])
        i = straight
    return out


def line_through(pts, i, j):
    """The least-squares line through pts[i..j], pointing from i to j: (a point on it, its direction). A long run
    is taken without its last pixel at each end (where a corner's rounding starts), and a line within 0.15 pixels
    of level or upright over its length is made exactly so."""
    run = pts[i:j + 1]
    length = sum(math.hypot(b[0] - a[0], b[1] - a[1]) for a, b in zip(run, run[1:]))
    if length >= 6:
        a, b, gone = 0, len(run) - 1, 0.0
        while gone < 1.0:
            gone += math.hypot(run[a + 1][0] - run[a][0], run[a + 1][1] - run[a][1])
            a += 1
        gone = 0.0
        while gone < 1.0:
            gone += math.hypot(run[b][0] - run[b - 1][0], run[b][1] - run[b - 1][1])
            b -= 1
        if b - a >= 2:
            run = run[a:b + 1]
    mx, my = sum(p[0] for p in run) / len(run), sum(p[1] for p in run) / len(run)
    sxx = sum((p[0] - mx) ** 2 for p in run)
    syy = sum((p[1] - my) ** 2 for p in run)
    sxy = sum((p[0] - mx) * (p[1] - my) for p in run)
    angle = 0.5 * math.atan2(2 * sxy, sxx - syy)
    dx, dy = math.cos(angle), math.sin(angle)
    if abs(dy) * length < 0.15:
        dx, dy = 1.0, 0.0
    elif abs(dx) * length < 0.15:
        dx, dy = 0.0, 1.0
    if dx * (pts[j][0] - pts[i][0]) + dy * (pts[j][1] - pts[i][1]) < 0:
        dx, dy = -dx, -dy
    return (mx, my), (dx, dy)


def crossing(a, b):
    """Where two lines ((point, direction)) cross, or None when they are within 3 degrees of parallel."""
    (ax, ay), (adx, ady) = a
    (bx, by), (bdx, bdy) = b
    cross = adx * bdy - ady * bdx
    if abs(cross) <= 0.05 * math.hypot(adx, ady) * math.hypot(bdx, bdy):
        return None
    t = ((bx - ax) * bdy - (by - ay) * bdx) / cross
    return (ax + adx * t, ay + ady * t)


def onto(line, p):
    """The point of a line nearest to `p`."""
    (ax, ay), (dx, dy) = line
    t = ((p[0] - ax) * dx + (p[1] - ay) * dy) / (dx * dx + dy * dy)
    return (ax + dx * t, ay + dy * t)


def fit_outline(pts, tol):
    """A closed traced line as a contour of the SWF writer: (start, [("L", p) | ("Q", control, p), ...]), in the
    units of `pts`; None when nothing with an area is left.

    The line is first cut into edges that stay within `tol` of it, none across a bend (`bends`). That leaves every
    corner as the anti-aliasing made it, rounded: a few very short edges between two long ones. Where the long
    edges on either side, continued, cross within `tip` of the traced line, the corner was a sharp one, and the
    short edges give way to that crossing. A corner the drawing itself rounds, and a tight curve, are further from
    the crossing and stay as fitted."""
    n = len(pts)
    stops = bends(pts)
    first_bend = stops[0] if stops else None
    first = first_bend or 0
    if first_bend is None:
        # A line with no bend has no natural place to start, and starting in the middle of an edge would cut that
        # edge in two: start where the first edge fitted from anywhere ends.
        first = fit_run(pts + [pts[0]], 0, n, tol)[0][2] % n
    ring = pts[first:] + pts[:first] + [pts[first]]
    place = [0.0]
    for (ax, ay), (bx, by) in zip(ring, ring[1:]):
        place.append(place[-1] + math.hypot(bx - ax, by - ay))
    stops = [i - first for i in stops] + [n] if stops else [n]
    edges = []  # [kind, i, j, control]: "L" straight, "Q" curved, "S" the short straight edge next to a bend
    for a, b in zip(stops, stops[1:]) if first_bend is not None else [(0, n)]:
        # From bend to bend; the pixel next to a bend, which its rounding reaches, is a short edge of its own.
        lo, hi = a, b
        while lo < b and place[lo] - place[a] < 1.0:
            lo += 1
        while hi > a and place[b] - place[hi] < 1.0:
            hi -= 1
        if first_bend is not None and hi > lo and place[hi] - place[lo] >= 2.0:
            edges.append(["S", a, lo, None])
            edges.extend(fit_run(ring, lo, hi, tol))
            edges.append(["S", hi, b, None])
        else:
            edges.extend(fit_run(ring, a, b, tol))
    if len(edges) < 2:
        return None
    count = len(edges)
    size = [place[e[2]] - place[e[1]] for e in edges]
    lines = [line_through(ring, e[1], e[2]) if e[0] == "L" else None for e in edges]

    def leaves(k):
        """The line edge k sets out along, (point, direction of travel); None when the edge is too short to say."""
        kind, i, j, control = edges[k]
        if kind == "L":
            return lines[k] if size[k] >= 2.0 else None
        if kind == "S":
            return None
        d = (control[0] - ring[i][0], control[1] - ring[i][1])
        return (ring[i], d) if size[k] >= 3 and math.hypot(*d) > 1e-6 else None

    def arrives(k):
        kind, i, j, control = edges[k]
        if kind == "L":
            return lines[k] if size[k] >= 2.0 else None
        if kind == "S":
            return None
        d = (ring[j][0] - control[0], ring[j][1] - control[1])
        return (ring[j], d) if size[k] >= 3 and math.hypot(*d) > 1e-6 else None

    def corner(a, b, skipped):
        """The sharp corner between edges a and b with the edges `skipped` between them, or None."""
        ta, tb = arrives(a), leaves(b)
        if ta is None or tb is None:
            return None
        (adx, ady), (bdx, bdy) = ta[1], tb[1]
        turn = abs(math.atan2(adx * bdy - ady * bdx, adx * bdx + ady * bdy))
        if turn < math.radians(20):
            return None
        # What is left out is one bend, with the stubs the fitting left in its rounding: never two corners.
        i, j = edges[a][2], edges[b][1] + (n if edges[b][1] < edges[a][2] else 0)
        between = sum(1 for m in stops[:-1] if i <= m <= j or i <= m + n <= j)
        if between != (1 if skipped else 0) or any(size[k] > bent(turn) for k in skipped if edges[k][0] != "S"):
            return None
        v = crossing(ta, tb)
        if v is None:
            return None
        if min(math.hypot(v[0] - ring[m % n][0], v[1] - ring[m % n][1]) for m in range(i, j + 1)) > tip(turn):
            return None
        # The crossing lies on from where a ends and before where b starts, not back along either.
        end, start = ring[edges[a][2]], ring[edges[b][1]]
        la, lb = math.hypot(adx, ady), math.hypot(bdx, bdy)
        if ((v[0] - end[0]) * adx + (v[1] - end[1]) * ady) / la < -0.75 \
                or ((start[0] - v[0]) * bdx + (start[1] - v[1]) * bdy) / lb < -0.75:
            return None
        return v

    # Round the loop from its longest edge; `kept` are the edges that stay, `joints[t]` where kept[t] ends.
    order = [(max(range(count), key=lambda k: size[k]) + k) % count for k in range(count)]
    kept, joints, at = [order[0]], [], 0
    while at < count:
        cur = order[at]
        for skip in (5, 4, 3, 2, 1, 0):
            if at + skip + 1 > count or (at + skip + 1 == count and len(kept) == 1 and skip + 1 == count):
                continue
            nxt = order[(at + skip + 1) % count]
            v = corner(cur, nxt, [order[at + q] for q in range(1, skip + 1)]) if nxt != cur else None
            if v is not None:
                break
        else:
            skip, nxt = 0, order[(at + 1) % count]
            raw = ring[edges[cur][2]]
            both = crossing(lines[cur], lines[nxt]) if lines[cur] and lines[nxt] else None
            v = both or (onto(lines[cur] or lines[nxt], raw) if lines[cur] or lines[nxt] else raw)
            if math.hypot(v[0] - raw[0], v[1] - raw[1]) > 2 * tol:
                v = raw
        joints.append(v)
        at += skip + 1
        if at < count:
            kept.append(order[at])
    segs = []
    for t, k in enumerate(kept):
        begin, end = joints[t - 1], joints[t]
        kind, i, j, control = edges[k]
        if kind != "Q":
            segs.append(("L", end))
            continue
        if begin == ring[i] and end == ring[j]:
            segs.append(("Q", control, end))
            continue
        # The curve was fitted between two traced points; an end moved (to a corner, or onto a line).
        between = ([ring[i]] if begin != ring[i] else []) + [ring[m] for m in sample(i, j, 21)] \
            + ([ring[j]] if end != ring[j] else [])
        moved = curve_through(begin, end, between, 1.5 * tol)
        if moved is not None:
            segs.append(("Q", moved, end))
        else:
            if begin != ring[i]:
                segs.append(("L", ring[i]))
            segs.append(("Q", control, ring[j]))
            if end != ring[j]:
                segs.append(("L", end))
    if len(segs) < 2 or (len(segs) == 2 and all(s[0] == "L" for s in segs)):
        return None
    return joints[-1], segs


# ---------------------------------------------------------------- one definition per distinct line


class Outlines:
    """The distinct closed lines of a set of pictures. Two lines are one when they have the same size and area and
    every point of one is within `same` pixels of the other after their top left corners are put together."""

    def __init__(self, tol, same=0.3):
        self.tol = tol
        self.same = same
        self.items = []  # {"pts", "w", "h", "area", "grid", "fit", "at"}
        self.by_size = {}

    @staticmethod
    def _near(grid, pts, p, limit):
        """Whether `p` is within `limit` of the closed line `pts` (looked up through its 2-pixel grid)."""
        gx, gy = int(p[0] // 2), int(p[1] // 2)
        n = len(pts)
        for yy in (gy - 1, gy, gy + 1):
            for xx in (gx - 1, gx, gx + 1):
                for k in grid.get((xx, yy), ()):
                    (x0, y0), (x1, y1) = pts[k], pts[(k + 1) % n]
                    dx, dy = x1 - x0, y1 - y0
                    d2 = dx * dx + dy * dy
                    t = 0.0 if d2 < 1e-12 else min(max(((p[0] - x0) * dx + (p[1] - y0) * dy) / d2, 0.0), 1.0)
                    if math.hypot(p[0] - x0 - dx * t, p[1] - y0 - dy * t) <= limit:
                        return True
        return False

    def find(self, pts):
        """(index of the line's definition, (x, y) of this copy's top left corner)."""
        x0, y0, x1, y1 = bounds(pts)
        rel = [(x - x0, y - y0) for x, y in pts]
        w, h, a = x1 - x0, y1 - y0, swf.area(rel)
        key = (round(w), round(h))
        for kx in (key[0] - 1, key[0], key[0] + 1):
            for ky in (key[1] - 1, key[1], key[1] + 1):
                for i in self.by_size.get((kx, ky), ()):
                    it = self.items[i]
                    if (abs(it["w"] - w) <= self.same and abs(it["h"] - h) <= self.same
                            and abs(it["area"] - a) <= 0.01 * abs(a) + 1
                            and all(self._near(it["grid"], it["pts"], p, self.same) for p in rel)):
                        return i, (x0, y0)
        grid = {}
        for k, (x, y) in enumerate(rel):
            grid.setdefault((int(x // 2), int(y // 2)), []).append(k)
        self.items.append({"pts": rel, "w": w, "h": h, "area": a, "grid": grid, "fit": False, "at": []})
        self.by_size.setdefault(key, []).append(len(self.items) - 1)
        return len(self.items) - 1, (x0, y0)

    def fit(self, i):
        """The line's edges (None when it has no area left), fitted once."""
        it = self.items[i]
        if it["fit"] is False:
            it["fit"] = fit_outline(it["pts"], self.tol)
        return it["fit"]


# ---------------------------------------------------------------- a picture as painted pieces


def pieces_of(w, h, rgba):
    """The painted pieces of a picture, each {"colour", "depth", "pts", "cuts"}: `pts` runs clockwise round what is
    filled, `cuts` are the transparent holes in it, `depth` counts the pieces around it (paint in that order)."""
    cover, white, grey, kind = coverage(w, h, rgba)
    outer, holes = [], []
    for field, colour in ((cover, "K"), (white, "W"), (grey, "G")):
        for pts in iso_lines(field, w, h):
            a = swf.area(pts)
            if abs(a) < 2.0:
                continue
            if a > 0:
                outer.append({"colour": colour, "pts": pts, "area": a, "cuts": []})
                continue
            if colour == "K":  # a hole through everything
                holes.append(pts)
                continue
            # What is in the hole, read 1.5 pixels inside it (on the left of the direction of travel).
            seen, n = {}, len(pts)
            for k in range(0, n, 3):
                (ax, ay), (bx, by) = pts[k - 1], pts[(k + 1) % n]
                d = math.hypot(bx - ax, by - ay)
                if d < 1e-9:
                    continue
                x, y = int(pts[k][0] + (by - ay) / d * 1.5), int(pts[k][1] - (bx - ax) / d * 1.5)
                if 0 <= x < w and 0 <= y < h and kind[y][x] is not None:
                    seen[kind[y][x]] = seen.get(kind[y][x], 0) + 1
            if not seen or max(seen, key=lambda k: (seen[k], k)) == "K":
                outer.append({"colour": "K", "pts": pts[::-1], "area": -a, "cuts": []})
            # Otherwise the other light colour fills it (its own piece paints that), or it is a hole through
            # everything, which the opaque area's own line has too.
    boxes = [bounds(p["pts"]) for p in outer]

    def holds(i, point):
        x0, y0, x1, y1 = boxes[i]
        return x0 <= point[0] <= x1 and y0 <= point[1] <= y1 and inside(point, outer[i]["pts"])

    for pts in holes:  # cut out of every piece painted over it
        for i, p in enumerate(outer):
            if holds(i, pts[0]):
                p["cuts"].append(pts)
    for i, p in enumerate(outer):
        pts, n = p["pts"], len(p["pts"])
        probe = None
        for k in range(0, n, max(n // 16, 1)):
            (ax, ay), (bx, by) = pts[k], pts[(k + 1) % n]
            d = math.hypot(bx - ax, by - ay)
            if d < 1e-9:
                continue
            probe = ((ax + bx) / 2 - (by - ay) / d * 0.6, (ay + by) / 2 + (bx - ax) / d * 0.6)
            if inside(probe, pts):
                break
        p["depth"] = sum(1 for j, q in enumerate(outer) if j != i and q["area"] > p["area"] and holds(j, probe))
    return [{"colour": p["colour"], "depth": p["depth"], "pts": p["pts"], "cuts": p["cuts"]} for p in outer]


# ---------------------------------------------------------------- the set


class GlyphSet:
    """Pictures traced into shared shapes. `add` each picture, then `plan` gives, per N, what its sprite places."""

    def __init__(self, tolerance=TOLERANCE):
        self.tolerance = tolerance
        self.lib = None
        self.scale = None
        self.items = {}  # n -> [(colour, depth, key, (tx, ty))]; key = (outline, ((outline, dx, dy), ...))
        self._same = {}  # a picture's bytes -> its items: several N often share one picture
        self._plan = None

    def _twips(self, x, y):
        unit = swf.TWIP / self.scale
        return (x - MARGIN * self.scale) * unit, (y - MARGIN * self.scale) * unit

    def _where(self, index, x, y):
        """Whole twips of a copy's corner; a copy within a twip of an earlier one of the same line sits on it."""
        tx, ty = (round(v) for v in self._twips(x, y))
        seen = self.lib.items[index]["at"]
        for sx, sy in seen:
            if abs(sx - tx) <= 1 and abs(sy - ty) <= 1:
                return sx, sy
        seen.append((tx, ty))
        return tx, ty

    def add(self, n, w, h, rgba):
        if w != h or w % (CANVAS + 2 * MARGIN):
            raise ValueError("glyph %d: %d x %d is not a square of %d canvas pixels at a whole scale"
                             % (n, w, h, CANVAS + 2 * MARGIN))
        scale = w // (CANVAS + 2 * MARGIN)
        if self.scale not in (None, scale):
            raise ValueError("glyph %d: drawn at %dx, the others at %dx" % (n, scale, self.scale))
        if self.lib is None:
            self.scale, self.lib = scale, Outlines(self.tolerance * scale)
        self._plan = None
        if rgba in self._same:
            self.items[n] = self._same[rgba]
            return
        items = []
        for piece in pieces_of(w, h, rgba):
            index, (x, y) = self.lib.find(piece["pts"])
            at = self._where(index, x, y)
            cuts = []
            for pts in piece["cuts"]:
                ci, (cx, cy) = self.lib.find(pts[::-1])
                cat = self._where(ci, cx, cy)
                cuts.append((ci, cat[0] - at[0], cat[1] - at[1]))
            items.append((piece["colour"], piece["depth"], (index, tuple(sorted(cuts))), at))
        self.items[n] = self._same[rgba] = items

    def _contours(self, key, at):
        """The contours of one piece in whole twips: its line clockwise, its cuts the other way round."""
        unit = swf.TWIP / self.scale
        out = []
        for index, dx, dy, flip in [(key[0], 0, 0, False)] + [(c[0], c[1], c[2], True) for c in key[1]]:
            fit = self.lib.fit(index)
            if fit is None:
                continue
            ox, oy = at[0] + dx, at[1] + dy
            move = lambda p: (ox + round(p[0] * unit), oy + round(p[1] * unit))  # noqa: E731
            start, segs = move(fit[0]), [(s[0],) + tuple(move(p) for p in s[1:]) for s in fit[1]]
            # An edge the rounding made empty is dropped.
            kept, here = [], start
            for s in segs:
                if s[-1] != here or (s[0] == "Q" and s[1] != here):
                    kept.append(s)
                    here = s[-1]
            if len(kept) < 2:
                continue
            if flip:
                points = [start] + [s[-1] for s in kept]
                back = []
                for k in range(len(kept) - 1, -1, -1):
                    back.append(("L", points[k]) if kept[k][0] == "L" else ("Q", kept[k][1], points[k]))
                start, kept = points[-1], back
            out.append((start, kept))
        return out

    def plan(self):
        """{n: [(colour, [contour, ...]), ...]}, bottom to top, in whole twips. Pieces of one colour and depth that
        appear together wherever any of them appears, the same way apart, are one entry (one shape)."""
        if self._plan is not None:
            return self._plan
        seen = {}  # (colour, key) -> {(n, depth, tx, ty)}
        for n, items in self.items.items():
            for colour, depth, key, at in items:
                seen.setdefault((colour, key), set()).add((n, depth, at[0], at[1]))
        plan = {}
        for n, items in self.items.items():
            buckets = {}
            for item in items:
                buckets.setdefault((item[1], item[0]), []).append(item)
            entries = []
            for (depth, colour), bucket in sorted(buckets.items()):
                groups = []
                for item in sorted(bucket, key=lambda it: (it[3][1], it[3][0], it[2])):
                    mine = seen[(colour, item[2])]
                    for group in groups:
                        first = group[0]
                        dx, dy = item[3][0] - first[3][0], item[3][1] - first[3][1]
                        if {(g, d, x + dx, y + dy) for g, d, x, y in seen[(colour, first[2])]} == mine:
                            group.append(item)
                            break
                    else:
                        groups.append([item])
                for group in groups:
                    contours = [c for item in group for c in self._contours(item[2], item[3])]
                    if contours:
                        entries.append((colour, contours))
            plan[n] = entries
        self._plan = plan
        return plan

    def sprite(self, movie, n):
        """The sprite of glyph `n` for the SWF writer: an object whose `items` are (shape id, x, y) to place."""
        g = swf.Glyph(movie)
        entries = self.plan()[n]
        xs = [p[0] for _, cs in entries for start, segs in cs for p in [start] + [q for s in segs for q in s[1:]]]
        ys = [p[1] for _, cs in entries for start, segs in cs for p in [start] + [q for s in segs for q in s[1:]]]
        x0, y0, x1, y1 = swf.BOUNDS.get(n, (0, 0, swf.BOX, swf.BOX))
        # An invisible rectangle keeps the sprite at least as large as the Xbox glyph it stands in for.
        if not xs or min(xs) > x0 + 2 or min(ys) > y0 + 2 or max(xs) < x1 - 2 or max(ys) < y1 - 2:
            g.items.append(movie.shape([swf.poly_contour([(x0, y0), (x1, y0), (x1, y1), (x0, y1)])], swf.CLEAR))
        for colour, contours in entries:
            g.items.append(movie.shape(contours, RGBA[colour]))
        return g


# ---------------------------------------------------------------- drawing the shapes back


def _edge(rows, h, x0, y0, x1, y1):
    """Adds one edge to the signed-area rows (a running sum along a row is each pixel's coverage)."""
    if y0 == y1:
        return
    sign = 1.0
    if y0 > y1:
        x0, y0, x1, y1, sign = x1, y1, x0, y0, -1.0
    slope = (x1 - x0) / (y1 - y0)
    x = x0 - y0 * slope if y0 < 0 else x0
    for y in range(max(int(math.floor(y0)), 0), min(int(math.ceil(y1)), h)):
        row = rows[y]
        dy = (y1 if y1 < y + 1 else y + 1.0) - (y0 if y0 > y else float(y))
        xn = x + slope * dy
        d = dy * sign
        xa, xb = (x, xn) if x < xn else (xn, x)
        fa = math.floor(xa)
        ia, ib = int(fa), int(math.ceil(xb))
        if ib <= ia + 1:
            xm = 0.5 * (x + xn) - fa
            row[ia] = row.get(ia, 0.0) + d - d * xm
            row[ia + 1] = row.get(ia + 1, 0.0) + d * xm
        else:
            s = 1.0 / (xb - xa)
            f0 = xa - fa
            a0 = 0.5 * s * (1 - f0) * (1 - f0)
            f1 = xb - ib + 1
            am = 0.5 * s * f1 * f1
            row[ia] = row.get(ia, 0.0) + d * a0
            if ib == ia + 2:
                row[ia + 1] = row.get(ia + 1, 0.0) + d * (1 - a0 - am)
            else:
                a1 = s * (1.5 - f0)
                row[ia + 1] = row.get(ia + 1, 0.0) + d * (a1 - a0)
                for xi in range(ia + 2, ib - 1):
                    row[xi] = row.get(xi, 0.0) + d * s
                row[ib - 1] = row.get(ib - 1, 0.0) + d * (1 - a1 - (ib - ia - 3) * s - am)
            row[ib] = row.get(ib, 0.0) + d * am
        x = xn


class Canvas:
    """A picture drawn with exact-area anti-aliasing: `lum` (premultiplied) and `alpha` rows, 0..255."""

    def __init__(self, w, h):
        self.w, self.h = w, h
        self.lum = [[0.0] * w for _ in range(h)]
        self.alpha = [[0.0] * w for _ in range(h)]

    def fill(self, contours, colour):
        """Paints closed contours ((start, segments), in pixels) in one colour (r, g, b, a) over what is there."""
        if colour[3] == 0:
            return
        value = (colour[0] + colour[1] + colour[2]) / 3
        w, rows = self.w, [{} for _ in range(self.h)]
        for start, segs in contours:
            x, y = start
            for s in segs:
                if s[0] == "L":
                    _edge(rows, self.h, x, y, s[1][0], s[1][1])
                else:
                    (cx, cy), (ex, ey) = s[1], s[2]
                    bend = math.hypot(x - 2 * cx + ex, y - 2 * cy + ey) / 4
                    steps = min(max(int(math.ceil(math.sqrt(bend / 0.01))), 1), 64)
                    px, py = x, y
                    for k in range(1, steps + 1):
                        u = k / steps
                        qx = (1 - u) * (1 - u) * x + 2 * u * (1 - u) * cx + u * u * ex
                        qy = (1 - u) * (1 - u) * y + 2 * u * (1 - u) * cy + u * u * ey
                        _edge(rows, self.h, px, py, qx, qy)
                        px, py = qx, qy
                x, y = s[-1]
        for y, row in enumerate(rows):
            if not row:
                continue
            lum, alpha = self.lum[y], self.alpha[y]
            run, before = 0.0, None
            for x in sorted(row):
                if before is not None and x - before > 1:
                    c = min(abs(run), 1.0)
                    a, b = max(before + 1, 0), min(x, w)
                    if c > 0.999 and b > a:
                        lum[a:b] = [value] * (b - a)
                        alpha[a:b] = [255.0] * (b - a)
                    elif c > 0.001:
                        for i in range(a, b):
                            lum[i] += (value - lum[i]) * c
                            alpha[i] += (255.0 - alpha[i]) * c
                run += row[x]
                if 0 <= x < w:
                    c = min(abs(run), 1.0)
                    if c > 0.001:
                        lum[x] += (value - lum[x]) * c
                        alpha[x] += (255.0 - alpha[x]) * c
                before = x

    def rgba(self):
        """Straight-alpha RGBA bytes."""
        out = bytearray()
        for lum, alpha in zip(self.lum, self.alpha):
            for v, a in zip(lum, alpha):
                g = min(max(int(round(v * 255 / a)), 0), 255) if a > 0.5 else 0
                out += bytes((g, g, g, min(max(int(round(a)), 0), 255)))
        return bytes(out)


def draw(entries, size, scale):
    """Shapes ([((r, g, b, a), [contour, ...]), ...] in twips, bottom to top) drawn at `scale` pixels a canvas pixel
    on a picture of `size` pixels, the canvas `MARGIN` in from its top left."""
    canvas = Canvas(size, size)
    k = scale / swf.TWIP
    move = lambda p: (p[0] * k + MARGIN * scale, p[1] * k + MARGIN * scale)  # noqa: E731
    for colour, contours in entries:
        canvas.fill([(move(start), [(s[0],) + tuple(move(p) for p in s[1:]) for s in segs])
                     for start, segs in contours], colour)
    return canvas


def difference(canvas, rgba):
    """How a drawn canvas differs from a picture, on premultiplied values: (the summed difference in pixels (1.0 is
    one pixel fully wrong), the pixels more than half wrong (an edge over half a pixel out), the largest
    difference 0..1, the picture's opaque pixels)."""
    w, total, bad, worst, opaque = canvas.w, 0.0, 0, 0.0, 0.0
    for y in range(canvas.h):
        lum, alpha = canvas.lum[y], canvas.alpha[y]
        o = y * w * 4
        for x in range(w):
            a = rgba[o + 3]
            v = (rgba[o] + rgba[o + 1] + rgba[o + 2]) * a / 765
            d = max(abs(lum[x] - v), abs(alpha[x] - a))
            if d > 0.5:
                total += d
                if d > 127.5:
                    bad += 1
                if d > worst:
                    worst = d
            opaque += a
            o += 4
    return total / 255, bad, worst / 255, opaque / 255


# ---------------------------------------------------------------- reading a built movie back


class BitReader:
    def __init__(self, data, start=0):
        self.data = data
        self.at = start * 8

    def ub(self, n):
        value = 0
        for _ in range(n):
            value = value << 1 | (self.data[self.at >> 3] >> (7 - (self.at & 7)) & 1)
            self.at += 1
        return value

    def sb(self, n):
        value = self.ub(n)
        return value - (1 << n) if n and value >> (n - 1) else value

    def byte(self):
        """The index of the next whole byte."""
        return (self.at + 7) >> 3


def read_shape(body):
    """((r, g, b, a), [contour, ...]) of a DefineShape3 tag body as this writer makes them: one solid fill, no line
    style; straight (general, level, upright) and curved edges."""
    r = BitReader(body, 2)
    n = r.ub(5)
    r.at += 4 * n
    o = r.byte()
    if body[o] != 1 or body[o + 1] != 0 or body[o + 6] != 0:
        raise ValueError("a shape with more than one solid fill, or with line styles")
    colour = tuple(body[o + 2:o + 6])
    r = BitReader(body, o + 7)
    fill_bits, line_bits = r.ub(4), r.ub(4)
    contours, x, y = [], 0, 0
    while True:
        if r.ub(1) == 0:
            flags = r.ub(5)
            if flags == 0:
                return colour, contours
            if flags & 16:
                raise ValueError("a shape that changes its styles")
            if flags & 1:
                n = r.ub(5)
                x, y = r.sb(n), r.sb(n)
                contours.append(((x, y), []))
            r.at += (fill_bits if flags & 2 else 0) + (fill_bits if flags & 4 else 0) + (line_bits if flags & 8 else 0)
        elif r.ub(1):
            n = r.ub(4) + 2
            if r.ub(1):
                dx, dy = r.sb(n), r.sb(n)
            elif r.ub(1):
                dx, dy = 0, r.sb(n)
            else:
                dx, dy = r.sb(n), 0
            x, y = x + dx, y + dy
            contours[-1][1].append(("L", (x, y)))
        else:
            n = r.ub(4) + 2
            cx, cy = x + r.sb(n), y + r.sb(n)
            x, y = cx + r.sb(n), cy + r.sb(n)
            contours[-1][1].append(("Q", (cx, cy), (x, y)))


def read_movie(data):
    """{N: [((r, g, b, a), [contour, ...]), ...]} of a movie this tool (or ps_prompt_glyphs) built: what each
    exported sprite places, bottom to top, in twips from the sprite's origin."""
    tags, _ = swf.read_tags(data)
    shapes = {struct.unpack_from("<H", body, 0)[0]: read_shape(body) for code, body in tags if code == 32}
    names = swf.exports_of(tags)
    out = {}
    for code, body in tags:
        if code != 39:
            continue
        placed, o = [], 4
        while o < len(body):
            head = struct.unpack_from("<H", body, o)[0]
            kind, length = head >> 6, head & 63
            o += 2
            if length == 63:
                length = struct.unpack_from("<I", body, o)[0]
                o += 4
            if kind == 26:
                if body[o] != 0x06:
                    raise ValueError("a placement that is not a shape at a position")
                depth, shape = struct.unpack_from("<HH", body, o + 1)
                r = BitReader(body, o + 5)
                if r.ub(1) or r.ub(1):
                    raise ValueError("a scaled or rotated placement")
                n = r.ub(5)
                tx, ty = r.sb(n), r.sb(n)
                colour, contours = shapes[shape]
                move = lambda p: (p[0] + tx, p[1] + ty)  # noqa: E731
                placed.append((depth, colour, [(move(start), [(s[0],) + tuple(move(p) for p in s[1:]) for s in segs])
                                               for start, segs in contours]))
            o += length
        sprite = struct.unpack_from("<H", body, 0)[0]
        if sprite in names:
            out[names[sprite]] = [(colour, contours) for _, colour, contours in sorted(placed, key=lambda p: p[0])]
    return out


# ---------------------------------------------------------------- the contact sheet

DIGITS = {"0": "111101101101111", "1": "010110010010111", "2": "111001111100111", "3": "111001111001111",
          "4": "101101111001001", "5": "111100111001111", "6": "111100111101111", "7": "111001010010010",
          "8": "111101111101111", "9": "111101111001111"}


def sheet(drawn, size, columns=8):
    """(width, height, RGBA bytes) of the drawn glyphs ({n: Canvas}) over a checkerboard, each with its N under it."""
    gap, label = 12, 26
    rows = (len(drawn) + columns - 1) // columns
    w, h = columns * (size + gap) + gap, rows * (size + gap + label) + gap
    out = bytearray(bytes((34, 36, 46, 255)) * (w * h))
    for i, n in enumerate(sorted(drawn)):
        left, top = gap + (i % columns) * (size + gap), gap + (i // columns) * (size + gap + label)
        canvas = drawn[n]
        for y in range(size):
            lum, alpha = canvas.lum[y], canvas.alpha[y]
            o = ((top + y) * w + left) * 4
            for x in range(size):
                back = 62 if (x // 12 + y // 12) & 1 else 50
                v = min(max(int(lum[x] + back * (1 - alpha[x] / 255) + 0.5), 0), 255)
                out[o:o + 3] = bytes((v, v, v))
                o += 4
        text = str(n)
        x0 = left + (size - len(text) * 12) // 2
        for k, ch in enumerate(text):
            for j, bit in enumerate(DIGITS[ch]):
                if bit == "1":
                    for yy in range(3):
                        o = ((top + size + 6 + (j // 3) * 3 + yy) * w + x0 + k * 12 + (j % 3) * 3) * 4
                        out[o:o + 12] = bytes((214, 216, 226, 255)) * 3
    return w, h, bytes(out)


# ---------------------------------------------------------------- pictures, movie, report

NAME = re.compile(r"Main_Button_(\d+)\.png$", re.IGNORECASE)


def read_folder(folder):
    """{n: (width, height, RGBA)} of the pictures in a folder; every N of the set must be there."""
    found = {}
    for name in sorted(os.listdir(folder)):
        m = NAME.search(name)
        if m and int(m.group(1)) in swf.GLYPHS:
            with open(os.path.join(folder, name), "rb") as f:
                found[int(m.group(1))] = read_png(f.read())
    missing = sorted(set(swf.GLYPHS) - set(found))
    if missing:
        raise ValueError("%s has no picture for N = %s" % (folder, ", ".join(map(str, missing))))
    return found


def trace(pictures, tolerance=TOLERANCE):
    """The pictures ({n: (width, height, RGBA)}) as a `GlyphSet`."""
    glyphs = GlyphSet(tolerance)
    for n in sorted(pictures):
        glyphs.add(n, *pictures[n])
    return glyphs


def build_over(base, glyphs):
    """The title's movie `base` with the traced set's drawings: see `ps_prompt_glyphs.build_over`. Raises
    ValueError when the set does not fit the length of `base`."""
    return swf.build_over(base, glyphs.sprite, swf.Movie(compact=True))


def build(glyphs, pad=None):
    """The standalone movie of the traced set."""
    data = swf.build(glyphs.sprite, swf.Movie(compact=True))
    return swf.pad_movie(data, pad) if pad else data


def used(data, codes=(32, 39)):
    """Bytes of a movie's shape and sprite tags, headers included."""
    return sum((2 if len(body) < 63 else 6) + len(body) for code, body in swf.read_tags(data)[0] if code in codes)


def budget(base):
    """Bytes a movie built over `base` has for its shapes and sprites: those of `base`, and any padding in it."""
    return used(base, (32, 39, 77))


def report(data, pictures, out=sys.stdout):
    """Draws every sprite of the built movie `data` back at the pictures' scale and prints how each differs from
    its picture. Returns the drawn canvases, {n: Canvas}."""
    placed, drawn, rows = read_movie(data), {}, []
    out.write("   N  shapes  edges  differs (px)  of opaque  px > 50%   worst\n")
    for n in sorted(pictures):
        w, h, rgba = pictures[n]
        drawn[n] = draw(placed[n], w, w // (CANVAS + 2 * MARGIN))
        total, bad, worst, opaque = difference(drawn[n], rgba)
        shapes = [e for e in placed[n] if e[0][3]]
        edges = sum(len(segs) for _, cs in shapes for _, segs in cs)
        rows.append((n, len(shapes), edges, total, 100 * total / max(opaque, 1), bad, 100 * worst))
        out.write("%4d %7d %6d %13.1f %9.2f%% %9d %6.0f%%\n" % rows[-1])
    out.write(" all %7s %6d %13.1f %9s %10d %6.0f%%\n" % (
        "", sum(r[2] for r in rows), sum(r[3] for r in rows), "", sum(r[5] for r in rows), max(r[6] for r in rows)))
    worst = sorted(rows, key=lambda r: -r[4])[:5]
    out.write("furthest from their pictures: %s\n" % ", ".join("N %d (%.2f%%)" % (r[0], r[4]) for r in worst))
    return drawn


# ---------------------------------------------------------------- self-test


def synthetic(scale=4):
    """Pictures made here, no artwork: a key with a triangle on it (lit and unlit, at two places), a black ring
    (a disc with a hole through it) with a white dot on it, a grey disc with a rounded bar. {name: (w, h, RGBA)}."""
    size = (CANVAS + 2 * MARGIN) * scale

    def poly(points):
        pts = [((x + MARGIN) * scale, (y + MARGIN) * scale) for x, y in points]
        return pts[0], [("L", p) for p in pts[1:]] + [("L", pts[0])]

    def disc(cx, cy, r, arcs=24):
        step, far = 2 * math.pi / arcs, r / math.cos(math.pi / arcs)

        def at(rad, a):
            return (cx + rad * math.cos(a) + MARGIN) * scale, (cy + rad * math.sin(a) + MARGIN) * scale

        return at(r, 0.0), [("Q", at(far, (k + 0.5) * step), at(r, (k + 1) * step)) for k in range(arcs)]

    def key(x, y, face):
        c = Canvas(size, size)
        c.fill([poly([(x + 3, y), (x + 29, y), (x + 32, y + 3), (x + 32, y + 29), (x + 29, y + 32), (x + 3, y + 32),
                      (x, y + 29), (x, y + 3)])], RGBA["K"])
        c.fill([poly([(x + 4, y + 3), (x + 28, y + 3), (x + 29, y + 4), (x + 29, y + 28), (x + 28, y + 29),
                      (x + 4, y + 29), (x + 3, y + 28), (x + 3, y + 4)])], RGBA[face])
        c.fill([poly([(x + 16, y + 9), (x + 24, y + 22), (x + 8, y + 22)])], RGBA["K"])
        return c

    ring = Canvas(size, size)
    ring.fill([disc(32, 32, 30)], RGBA["K"])
    ring.fill([disc(51, 32, 6)], RGBA["W"])
    hole = Canvas(size, size)
    hole.fill([disc(32, 32, 9)], RGBA["W"])
    for y in range(size):  # the middle of the ring is cut out
        for x in range(size):
            keep = 1 - hole.alpha[y][x] / 255
            ring.lum[y][x] *= keep
            ring.alpha[y][x] *= keep
    bar = Canvas(size, size)
    bar.fill([disc(32, 32, 31)], RGBA["K"])
    bar.fill([disc(32, 32, 28)], RGBA["G"])
    bar.fill([poly(swf.rounded_rect(14, 26, 50, 38, (6, 6, 6, 6)))], RGBA["K"])
    drawn = {"key": key(4, 6, "W"), "key unlit": key(4, 6, "G"), "key moved": key(28, 24, "W"), "ring": ring,
             "bar": bar}
    return {name: (size, size, c.rgba()) for name, c in drawn.items()}


def selftest():
    made = synthetic()
    names = sorted(made)
    pictures = {n: made[names[i % len(names)]] for i, n in enumerate(sorted(swf.GLYPHS))}
    for w, h, rgba in made.values():
        assert read_png(write_png(w, h, rgba)) == (w, h, rgba)
    glyphs = trace(pictures)
    data = build(glyphs, MOVIE_SIZE)
    assert data[:4] == b"GFX*" and struct.unpack_from("<I", data, 4)[0] == len(data) == MOVIE_SIZE
    tags, _ = swf.read_tags(data)
    assert tags[-1][0] == 0 and sum(1 for code, _ in tags if code == 39) == len(swf.GLYPHS)
    assert sorted(swf.exports_of(tags).values()) == sorted(swf.GLYPHS)
    shapes = sum(1 for code, body in tags if code == 32 and read_shape(body)[0][3])
    assert shapes == 9, shapes  # five drawings that share their lines: nine shapes for 114 painted pieces
    placed = read_movie(data)
    for n, (w, h, rgba) in pictures.items():
        total, bad, worst, opaque = difference(draw(placed[n], w, glyphs.scale), rgba)
        assert total < 0.004 * opaque and bad == 0, (n, total, bad, worst, opaque)
    print("ok: %d shapes for %d sprites, %d bytes of shapes and sprites, padded to %d"
          % (shapes, len(swf.GLYPHS), used(data), len(data)))


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--glyphs", help="the folder of pictures (one *Main_Button_<N>.png per N)")
    ap.add_argument("--out", help="write the uncompressed movie (GFX header + tags) here")
    ap.add_argument("--from-archive", help="build over the title's own movie in this data.rpf (see above)")
    ap.add_argument("--base", help="build over this inflated movie file instead of the archive")
    ap.add_argument("--pad-inflated", type=int, help="standalone movie: pad it to this many bytes (28148: the title's)")
    ap.add_argument("--tolerance", type=float, default=TOLERANCE,
                    help="canvas pixels an edge may stray from the traced line (default %(default)s)")
    ap.add_argument("--sheet", help="write a contact sheet of the movie's sprites, drawn back, to this PNG")
    ap.add_argument("--quiet", action="store_true", help="do not draw the movie back and compare it with the pictures")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args(argv)
    if args.selftest:
        selftest()
    if not args.glyphs:
        if not args.selftest:
            ap.print_help()
        return
    pictures = read_folder(args.glyphs)
    glyphs = trace(pictures, args.tolerance)
    if args.from_archive or args.base:
        if args.base:
            with open(args.base, "rb") as f:
                base = f.read()
        else:
            base = swf.read_archive_movie(args.from_archive)
        try:
            data = build_over(base, glyphs)
        except ValueError as error:
            sys.exit("the set does not fit the title's movie: %s (raise --tolerance)" % error)
        room, took = budget(base), used(data)
        print("%d bytes; shapes and sprites take %d of the %d the title's movie leaves them (%d to spare)"
              % (len(data), took, room, room - took))
    else:
        data = build(glyphs, args.pad_inflated)
        print("standalone movie: %d bytes, %d of shapes and sprites" % (len(data), used(data)))
    if args.out:
        with open(args.out, "wb") as f:
            f.write(data)
    if not args.quiet or args.sheet:
        drawn = report(data, pictures)
        if args.sheet:
            size = next(iter(pictures.values()))[0]
            with open(args.sheet, "wb") as f:
                f.write(write_png(*sheet(drawn, size)))


if __name__ == "__main__":
    main(sys.argv[1:])
