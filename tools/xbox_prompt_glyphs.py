#!/usr/bin/env python3
"""Renders the button prompt glyphs of the title's own input-icon movie (the Xbox set) to PNG files.

The movie (docs/prompt_glyphs.md) is a Scaleform GFX container of vector shapes, not textures: one-frame sprites
``platform_xbsx.input_icons.Main_Button_<N>``, each placing a few DefineShape3 characters. This tool reads it from
your own ``data.rpf`` (read-only: the stream is plain zlib, nothing is decrypted and the table of contents is not
used), rasterises every exported sprite and writes one PNG per export, named after the export, a contact sheet
``sheet.png`` and a listing ``README.txt``:

    python -I tools/xbox_prompt_glyphs.py --from-archive "<package>/data.rpf" --out <folder>
    python -I tools/xbox_prompt_glyphs.py --movie glyphs.gfx --out <folder>      # an inflated movie file
    python -I tools/xbox_prompt_glyphs.py --movie glyphs.gfx --out <folder> --scale 1 --margin 0   # the 64 x 64 box

The pictures are the game's artwork: the output folder must be outside the repository (the tool refuses one inside
it), and they are never committed.

What is drawn: DefineShape, DefineShape2 and DefineShape3 with solid fills and plain strokes (straight and curved
edges, both fill sides of an edge, new style tables inside a shape), placed in a sprite's first frame by PlaceObject2
with a matrix. Fills use the even-odd rule over each style's edges; strokes have round caps and joins and scale with
the placement. Everything else (gradient and bitmap fills, DefineShape4, colour transforms, clip layers, nested
sprites) stops with an error that names it, so nothing is drawn wrong silently. Standard library only.
"""

import argparse
import math
import os
import re
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ps_prompt_glyphs as psg  # noqa: E402  (read_tags: the movie's top-level tags)

TWIP = 20  # twips per pixel
MOVIE_OFFSET = 0x117D7000  # the zlib stream of the movie in data.rpf (docs/prompt_glyphs.md)
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# N -> the button the glyph shows (docs/prompt_glyphs.md, "N to button").
BUTTONS = {
    4: "D-pad up lit", 5: "D-pad down lit", 6: "D-pad left lit", 7: "D-pad right lit", 8: "D-pad, nothing lit",
    9: "D-pad, all lit", 10: "D-pad up/down lit", 11: "D-pad left/right lit",
    12: "left stick up", 13: "left stick down", 14: "left stick left", 15: "left stick right",
    16: "left stick press (L3)", 17: "left stick, plain", 18: "left stick up/down", 19: "left stick left/right",
    20: "left stick rotate (arrow on the left)",
    21: "right stick up", 22: "right stick down", 23: "right stick left", 24: "right stick right",
    25: "right stick press (R3)", 26: "right stick, plain", 27: "right stick up/down", 28: "right stick left/right",
    29: "right stick rotate (arrow on the left)",
    30: "A", 31: "B", 32: "X", 33: "Y", 34: "LB", 35: "LT", 36: "RB", 37: "RT",
    38: "Menu (three bars)", 39: "View (two overlapping windows)",
    58: "left stick rotate (arrow on the right)", 59: "right stick rotate (arrow on the right)",
}


class Unsupported(ValueError):
    """The movie uses something this tool does not draw."""


# ---------------------------------------------------------------- reading


class BitReader:
    def __init__(self, data, pos=0):
        self.d = data
        self.pos = pos
        self.bit = 0

    def ub(self, nbits):
        v = 0
        for _ in range(nbits):
            v = (v << 1) | ((self.d[self.pos] >> (7 - self.bit)) & 1)
            self.bit += 1
            if self.bit == 8:
                self.bit = 0
                self.pos += 1
        return v

    def sb(self, nbits):
        v = self.ub(nbits)
        return v - (1 << nbits) if nbits and v >> (nbits - 1) else v

    def align(self):
        if self.bit:
            self.bit = 0
            self.pos += 1

    def u8(self):
        self.align()
        self.pos += 1
        return self.d[self.pos - 1]

    def u16(self):
        self.align()
        self.pos += 2
        return struct.unpack_from("<H", self.d, self.pos - 2)[0]

    def rect(self):
        """(x0, x1, y0, y1) in twips."""
        self.align()
        n = self.ub(5)
        r = tuple(self.sb(n) for _ in range(4))
        self.align()
        return r

    def matrix(self):
        """(a, b, c, d, tx, ty): x' = a x + c y + tx, y' = b x + d y + ty."""
        self.align()
        a = d = 1.0
        b = c = 0.0
        if self.ub(1):
            n = self.ub(5)
            a = self.sb(n) / 65536.0
            d = self.sb(n) / 65536.0
        if self.ub(1):
            n = self.ub(5)
            b = self.sb(n) / 65536.0
            c = self.sb(n) / 65536.0
        n = self.ub(5)
        tx = self.sb(n)
        ty = self.sb(n)
        self.align()
        return (a, b, c, d, float(tx), float(ty))


SHAPE_VERSION = {2: 1, 22: 2, 32: 3}  # tag code -> DefineShape version


def read_styles(r, version):
    """The fill and line style tables at the reader: ([rgba], [(width in twips, rgba)])."""

    def color():
        c = (r.u8(), r.u8(), r.u8())
        return c + ((r.u8(),) if version >= 3 else (255,))

    n = r.u8()
    if n == 0xFF and version >= 2:
        n = r.u16()
    fills = []
    for _ in range(n):
        kind = r.u8()
        if kind != 0:
            raise Unsupported("fill style type %#04x (only solid fills are drawn)" % kind)
        fills.append(color())
    n = r.u8()
    if n == 0xFF:
        n = r.u16()
    lines = []
    for _ in range(n):
        width = r.u16()
        lines.append((width, color()))
    return fills, lines


class Shape:
    """A shape character: its bounds and its paint layers, bottom to top.

    A layer is ("fill", rgba, edges) or ("stroke", width in twips, rgba, edges); an edge is two points (a line) or
    three (a quadratic curve: start, control, end), in twips. A fill's edges all have the fill on their right.
    """

    def __init__(self, cid, bounds):
        self.cid = cid
        self.bounds = bounds  # (x0, x1, y0, y1)
        self.layers = []


def parse_shape(code, body):
    version = SHAPE_VERSION[code]
    r = BitReader(body)
    shape = Shape(r.u16(), r.rect())
    fills, lines = read_styles(r, version)
    r.align()
    fill_bits, line_bits = r.ub(4), r.ub(4)
    fill_edges, line_edges = {}, {}

    def flush():
        for i in sorted(fill_edges):
            shape.layers.append(("fill", fills[i - 1], fill_edges[i]))
        for i in sorted(line_edges):
            shape.layers.append(("stroke", lines[i - 1][0], lines[i - 1][1], line_edges[i]))
        fill_edges.clear()
        line_edges.clear()

    x = y = 0
    f0 = f1 = ln = 0
    while True:
        if r.ub(1) == 0:
            flags = r.ub(5)
            if flags == 0:
                break
            if flags & 1:
                n = r.ub(5)
                x, y = r.sb(n), r.sb(n)
            new = {}
            if flags & 2:
                new["f0"] = r.ub(fill_bits)
            if flags & 4:
                new["f1"] = r.ub(fill_bits)
            if flags & 8:
                new["ln"] = r.ub(line_bits)
            if flags & 16:
                if version < 2:
                    raise Unsupported("new styles inside a DefineShape (version 1) record")
                flush()
                fills, lines = read_styles(r, version)
                r.align()
                fill_bits, line_bits = r.ub(4), r.ub(4)
                f0 = f1 = ln = 0  # the indices of this record are into the new tables
            f0, f1, ln = new.get("f0", f0), new.get("f1", f1), new.get("ln", ln)
            if f0 > len(fills) or f1 > len(fills) or ln > len(lines):
                raise ValueError("shape %d selects a style it does not define" % shape.cid)
            continue
        straight = r.ub(1)
        n = r.ub(4) + 2
        if straight:
            if r.ub(1):
                dx, dy = r.sb(n), r.sb(n)
            elif r.ub(1):
                dx, dy = 0, r.sb(n)
            else:
                dx, dy = r.sb(n), 0
            edge = ((x, y), (x + dx, y + dy))
        else:
            cx, cy = x + r.sb(n), y + r.sb(n)
            edge = ((x, y), (cx, cy), (cx + r.sb(n), cy + r.sb(n)))
        if f1:
            fill_edges.setdefault(f1, []).append(edge)
        if f0:
            fill_edges.setdefault(f0, []).append(edge[::-1])
        if ln:
            line_edges.setdefault(ln, []).append(edge)
        x, y = edge[-1]
    flush()
    return shape


def parse_sprite(body):
    """(character id, [(depth, placed character id, matrix)] of the sprite's first frame, bottom to top)."""
    sid = struct.unpack_from("<H", body, 0)[0]
    placed = []
    o = 4
    while o < len(body):
        head = struct.unpack_from("<H", body, o)[0]
        code, length = head >> 6, head & 63
        o += 2
        if length == 63:
            length = struct.unpack_from("<I", body, o)[0]
            o += 4
        if code in (0, 1):  # End, ShowFrame: the first frame is complete
            break
        if code == 43:  # a frame label draws nothing
            o += length
            continue
        if code != 26:
            raise Unsupported("tag %d in sprite %d (only PlaceObject2 is drawn)" % (code, sid))
        flags = body[o]
        if flags & ~0x26 or not flags & 0x02:  # character, matrix and a name are all that is understood
            raise Unsupported("PlaceObject2 flags %#04x in sprite %d (colour transform, clip layer, ratio or a "
                              "move of a placed character)" % (flags, sid))
        r = BitReader(body, o + 1)
        depth, cid = r.u16(), r.u16()
        placed.append((depth, cid, r.matrix() if flags & 0x04 else (1.0, 0.0, 0.0, 1.0, 0.0, 0.0)))
        o += length
    return sid, sorted(placed)


class Movie:
    def __init__(self, data):
        if data[:3] not in (b"GFX", b"FWS"):
            raise ValueError("not an uncompressed GFX or SWF movie (signature %r)" % bytes(data[:3]))
        if struct.unpack_from("<I", data, 4)[0] != len(data):
            raise ValueError("the movie's length field says %d bytes, it has %d"
                             % (struct.unpack_from("<I", data, 4)[0], len(data)))
        self.size = len(data)
        self.signature = data[:3].decode("ascii")
        self.version = data[3]
        self.shapes = {}
        self.sprites = {}
        self.exports = []  # (character id, export name), in the movie's order
        self.label = None
        tags, _ = psg.read_tags(data)
        for code, body in tags:
            if code in SHAPE_VERSION:
                shape = parse_shape(code, body)
                self.shapes[shape.cid] = shape
            elif code == 39:
                self.sprites[struct.unpack_from("<H", body, 0)[0]] = body
            elif code == 43 and self.label is None:
                self.label = body.split(b"\0", 1)[0].decode("ascii", "replace")
            elif code == 56:
                o = 2
                for _ in range(struct.unpack_from("<H", body, 0)[0]):
                    cid = struct.unpack_from("<H", body, o)[0]
                    end = body.index(b"\0", o + 2)
                    self.exports.append((cid, body[o + 2:end].decode("ascii", "replace")))
                    o = end + 1

    def placements(self, sid):
        """[(shape, matrix)] of an exported sprite, bottom to top."""
        if sid not in self.sprites:
            raise Unsupported("export %d is not a sprite" % sid)
        out = []
        for _, cid, matrix in parse_sprite(self.sprites[sid])[1]:
            if cid not in self.shapes:
                raise Unsupported("sprite %d places character %d, which is %s, not a DefineShape 1-3"
                                  % (sid, cid, "a sprite" if cid in self.sprites else "something else"))
            out.append((self.shapes[cid], matrix))
        return out

    def bounds(self, sid):
        """(x0, y0, x1, y1) in twips of an exported sprite: its shapes' own bounds, placed."""
        xs, ys = [], []
        for shape, m in self.placements(sid):
            x0, x1, y0, y1 = shape.bounds
            for px, py in ((x0, y0), (x1, y0), (x0, y1), (x1, y1)):
                xs.append(m[0] * px + m[2] * py + m[4])
                ys.append(m[1] * px + m[3] * py + m[5])
        return (min(xs), min(ys), max(xs), max(ys)) if xs else (0.0, 0.0, 0.0, 0.0)


def read_stream(path, offset=MOVIE_OFFSET):
    """The zlib stream at `offset` of an archive, inflated: (bytes, stored size). The archive is only read."""
    with open(path, "rb") as f:
        f.seek(offset)
        raw = f.read(1 << 20)
    d = zlib.decompressobj()
    try:
        data = d.decompress(raw, 1 << 24)
    except zlib.error as e:
        raise ValueError("%s has no zlib stream at %#x (%s)" % (path, offset, e))
    if not d.eof:
        raise ValueError("the zlib stream at %#x of %s does not end within 1 MiB" % (offset, path))
    return data, len(raw) - len(d.unused_data)


# ---------------------------------------------------------------- rasteriser

FLAT = 0.02  # pixels a flattened curve may leave its curve by


def flatten(edges, m):
    """Directed line segments (x0, y0, x1, y1) in pixels of `edges` under the twips-to-pixels matrix `m`."""
    a, b, c, d, tx, ty = m
    out = []
    for e in edges:
        p = [(a * x + c * y + tx, b * x + d * y + ty) for x, y in e]
        if len(p) == 2:
            out.append(p[0] + p[1])
            continue
        (x0, y0), (cx, cy), (x1, y1) = p
        n = max(1, math.ceil(math.sqrt(math.hypot(x0 - 2 * cx + x1, y0 - 2 * cy + y1) / (4 * FLAT))))
        px, py = x0, y0
        for i in range(1, n + 1):
            t = i / n
            u = 1 - t
            qx = u * u * x0 + 2 * u * t * cx + t * t * x1
            qy = u * u * y0 + 2 * u * t * cy + t * t * y1
            out.append((px, py, qx, qy))
            px, py = qx, qy
    return out


def capsules(segments, half):
    """The outlines of the discs of radius `half` swept along each segment (round caps and joins), all wound the
    same way, so their union is what the non-zero rule fills."""
    n = max(3, min(48, math.ceil(math.pi / (2 * math.acos(max(0.0, 1 - FLAT / half))))))
    out = []
    for x0, y0, x1, y1 in segments:
        ang = math.atan2(y1 - y0, x1 - x0) if (x0, y0) != (x1, y1) else 0.0
        pts = [(x1 + half * math.cos(ang - math.pi / 2 + math.pi * i / n),
                y1 + half * math.sin(ang - math.pi / 2 + math.pi * i / n)) for i in range(n + 1)]
        pts += [(x0 + half * math.cos(ang + math.pi / 2 + math.pi * i / n),
                 y0 + half * math.sin(ang + math.pi / 2 + math.pi * i / n)) for i in range(n + 1)]
        out.extend(pts[i] + pts[(i + 1) % len(pts)] for i in range(len(pts)))
    return out


def coverage(segments, width, height, nonzero, ss):
    """{row: [coverage 0..1 of each pixel]} of the area the directed `segments` enclose, by the even-odd rule or
    the non-zero one. `ss` sample lines a pixel row, exact along each line."""
    crossings = {}
    top = height * ss
    for x0, y0, x1, y1 in segments:
        if y0 == y1:
            continue
        step = 1
        if y0 > y1:
            x0, y0, x1, y1, step = x1, y1, x0, y0, -1
        slope = (x1 - x0) / (y1 - y0)
        for j in range(max(0, math.ceil(y0 * ss - 0.5)), min(top, math.ceil(y1 * ss - 0.5))):
            crossings.setdefault(j, []).append((x0 + ((j + 0.5) / ss - y0) * slope, step))
    full, part = {}, {}  # per row: a difference array of whole pixels, and the partly covered ends
    for j, xs in crossings.items():
        row = j // ss
        if row not in full:
            full[row] = [0.0] * (width + 2)
            part[row] = [0.0] * (width + 2)
        f, p = full[row], part[row]
        xs.sort()
        wind = 0
        start = 0.0
        for x, step in xs:
            was = wind != 0 if nonzero else wind & 1
            wind += step
            now = wind != 0 if nonzero else wind & 1
            if now and not was:
                start = x
            elif was and not now:
                xa, xb = min(max(start, 0.0), width), min(max(x, 0.0), width)
                if xb <= xa:
                    continue
                ia, ib = int(xa), int(xb)
                if ia == ib:
                    p[ia] += xb - xa
                else:
                    p[ia] += ia + 1 - xa
                    p[ib] += xb - ib
                    f[ia + 1] += 1.0
                    f[ib] -= 1.0
    out = {}
    for row, f in full.items():
        p = part[row]
        run = 0.0
        cov = []
        for i in range(width):
            run += f[i]
            cov.append(min(1.0, (run + p[i]) / ss))
        out[row] = cov
    return out


class Canvas:
    """A float RGBA picture, colour premultiplied by alpha while it is painted."""

    def __init__(self, width, height):
        self.width, self.height = width, height
        self.rows = [[0.0] * (4 * width) for _ in range(height)]

    def paint(self, cover, rgba):
        r, g, b, a = (v / 255.0 for v in rgba)
        for y, cov in cover.items():
            row = self.rows[y]
            for x, c in enumerate(cov):
                if c <= 0.0:
                    continue
                sa = a * c
                keep = 1.0 - sa
                i = 4 * x
                row[i] = r * sa + row[i] * keep
                row[i + 1] = g * sa + row[i + 1] * keep
                row[i + 2] = b * sa + row[i + 2] * keep
                row[i + 3] = sa + row[i + 3] * keep

    def rgba(self):
        """8-bit RGBA bytes, straight (not premultiplied) alpha."""
        out = bytearray(4 * self.width * self.height)
        o = 0
        for row in self.rows:
            for i in range(0, len(row), 4):
                a = row[i + 3]
                if a > 0.0:
                    out[o] = min(255, int(row[i] / a * 255 + 0.5))
                    out[o + 1] = min(255, int(row[i + 1] / a * 255 + 0.5))
                    out[o + 2] = min(255, int(row[i + 2] / a * 255 + 0.5))
                    out[o + 3] = min(255, int(a * 255 + 0.5))
                o += 4
        return bytes(out)


def render(movie, sid, canvas=(64, 64), scale=4, margin=0, ss=8):
    """Exported sprite `sid` on a `canvas` (pixels, its registration point at the top left) with `margin` more
    pixels on every side, drawn `scale` times as large: (width, height, RGBA bytes)."""
    width, height = (canvas[0] + 2 * margin) * scale, (canvas[1] + 2 * margin) * scale
    k = scale / TWIP
    pic = Canvas(width, height)
    for shape, m in movie.placements(sid):
        dm = tuple(v * k for v in m[:4]) + (m[4] * k + margin * scale, m[5] * k + margin * scale)
        for layer in shape.layers:
            segments = flatten(layer[-1], dm)
            if layer[0] == "fill":
                pic.paint(coverage(segments, width, height, False, ss), layer[1])
            else:
                # A stroke scales with the placement (the root of the area scale); never thinner than a pixel.
                wide = max(1.0, layer[1] * math.sqrt(abs(dm[0] * dm[3] - dm[1] * dm[2])))
                pic.paint(coverage(capsules(segments, wide / 2), width, height, True, ss), layer[2])
    return width, height, pic.rgba()


# ---------------------------------------------------------------- output


def png(width, height, rgba):
    """An 8-bit RGBA PNG file."""

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    raw = b"".join(b"\0" + rgba[y * width * 4:(y + 1) * width * 4] for y in range(height))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


DIGITS = {"0": "111101101101111", "1": "010110010010111", "2": "111001111100111", "3": "111001111001111",
          "4": "101101111001001", "5": "111100111001111", "6": "111100111101111", "7": "111001001001001",
          "8": "111101111101111", "9": "111101111001111"}  # 3 x 5 dots, row by row


def contact_sheet(pictures, columns=8, pad=12, dot=3, back=((70, 74, 84), (88, 92, 102)), ink=(236, 238, 244)):
    """One picture of all `pictures` [(label of digits, width, height, RGBA bytes)] in a grid, each over a
    checkerboard (so light, dark and transparent parts all show) with its label under it: (width, height, RGBA)."""
    cw = max(p[1] for p in pictures)
    ch = max(p[2] for p in pictures)
    label_h = 5 * dot
    cell_w, cell_h = cw + pad, ch + label_h + 2 * pad
    rows = (len(pictures) + columns - 1) // columns
    width, height = columns * cell_w + pad, rows * cell_h + pad
    check = 8 * dot // 3
    sheet = bytearray(bytes((52, 55, 63, 255)) * (width * height))
    for n, (label, w, h, rgba) in enumerate(pictures):
        left = pad + (n % columns) * cell_w + (cw - w) // 2
        top = pad + (n // columns) * cell_h + (ch - h) // 2
        for y in range(h):
            o = 4 * ((top + y) * width + left)
            for x in range(w):
                r, g, b, a = rgba[4 * (y * w + x):4 * (y * w + x) + 4]
                under = back[((x // check) + (y // check)) & 1]
                i = o + 4 * x
                sheet[i] = (r * a + under[0] * (255 - a) + 127) // 255
                sheet[i + 1] = (g * a + under[1] * (255 - a) + 127) // 255
                sheet[i + 2] = (b * a + under[2] * (255 - a) + 127) // 255
        lx = pad + (n % columns) * cell_w + (cw - (4 * len(label) - 1) * dot) // 2
        ly = pad + (n // columns) * cell_h + ch + pad // 2
        for k, digit in enumerate(label):
            bits = DIGITS[digit]
            for j in range(15):
                if bits[j] == "1":
                    for yy in range(dot):
                        o = 4 * ((ly + (j // 3) * dot + yy) * width + lx + (4 * k + j % 3) * dot)
                        sheet[o:o + 4 * dot] = bytes(ink + (255,)) * dot
    return width, height, bytes(sheet)


def export_number(name):
    m = re.search(r"(\d+)$", name)
    return int(m.group(1)) if m else None


def file_name(name):
    return re.sub(r"[^A-Za-z0-9._-]", "_", name) + ".png"


def inside_repository(path):
    a, b = (os.path.normcase(os.path.realpath(p)) for p in (path, REPO))
    try:
        return os.path.commonpath([a, b]) == b
    except ValueError:  # another drive
        return False


def write_all(movie, out_dir, source, scale=4, canvas=(64, 64), margin=1, columns=8, log=print):
    """Writes one PNG per export of `movie`, sheet.png and README.txt into `out_dir`; returns the PNG names."""
    out_dir = os.path.abspath(out_dir)
    if inside_repository(out_dir):
        raise ValueError("%s is inside the repository: the pictures are game content, write them elsewhere" % out_dir)
    exports = sorted(movie.exports, key=lambda e: (export_number(e[1]) is None, export_number(e[1]) or 0, e[1]))
    if not exports:
        raise ValueError("the movie exports nothing")
    os.makedirs(out_dir, exist_ok=True)
    listing, pictures = [], []
    for sid, name in exports:
        w, h, rgba = render(movie, sid, canvas, scale, margin)
        data = png(w, h, rgba)
        with open(os.path.join(out_dir, file_name(name)), "wb") as f:
            f.write(data)
        n = export_number(name)
        button = BUTTONS.get(n, "?") if name.startswith(psg.EXPORT_PREFIX + "Main_Button_") else "?"
        x0, y0, x1, y1 = (v / TWIP for v in movie.bounds(sid))
        over = max(-x0, -y0, x1 - canvas[0], y1 - canvas[1]) - margin
        ink = sum(1 for i in range(3, len(rgba), 4) if rgba[i])
        listing.append((file_name(name), button, sid, len(movie.placements(sid)),
                        "%.2f..%.2f x %.2f..%.2f" % (x0, x1, y0, y1), "%d x %d" % (w, h), len(data), over, ink))
        pictures.append((str(n) if n is not None else "", w, h, rgba))
        log("%s  %s  %d x %d  %d bytes" % (listing[-1][0], listing[-1][1], w, h, len(data)))
    sw, sh, sheet = contact_sheet(pictures, columns)
    sheet_png = png(sw, sh, sheet)
    with open(os.path.join(out_dir, "sheet.png"), "wb") as f:
        f.write(sheet_png)
    lines = [
        "Button prompt glyphs rendered from the title's own input-icon movie",
        "",
        "Source:   %s" % source,
        "Movie:    %s container, version byte %d, %d bytes, frame label %s; %d shapes, %d sprites, %d exports"
        % (movie.signature, movie.version, movie.size, movie.label, len(movie.shapes), len(movie.sprites),
           len(exports)),
        "          The glyphs are vector sprites; the movie holds no bitmaps and no texture dictionary is involved.",
        "Pictures: one PNG per export, named after the export: the %d x %d pixel canvas of a glyph (registration at"
        % canvas,
        "          its top left) with %d more on every side, so an outline that leaves the canvas by a fraction of a"
        % margin,
        "          pixel is not cut; drawn at %dx, 8-bit RGBA, straight alpha, transparent where nothing is drawn."
        % scale,
        "Tool:     tools/xbox_prompt_glyphs.py (format notes: docs/prompt_glyphs.md)",
        "",
        "These are the game's artwork from your own copy: keep them on your PC, do not publish or commit them.",
        "",
        "file | button | sprite id | shapes | bounds in pixels (x0..x1 x y0..y1) | picture | PNG bytes",
    ]
    lines += ["%s | %s | %d | %d | %s | %s | %d" % row[:7] for row in listing]
    lines += ["", "sheet.png | contact sheet of all %d over a checkerboard, each labelled with its N | %d x %d | %d"
              % (len(listing), sw, sh, len(sheet_png))]
    notes = ["%s: its bounds leave the picture by %.2f px (cut off there)" % (row[0], row[7])
             for row in listing if row[7] > 0]
    notes += ["%s: nothing was drawn (an empty picture)" % row[0] for row in listing if not row[8]]
    if notes:
        lines += ["", "Notes:"] + ["  " + n for n in notes]
    with open(os.path.join(out_dir, "README.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    return [row[0] for row in listing]


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--from-archive", help="read the movie from this data.rpf (your own copy; read-only)")
    ap.add_argument("--offset", type=lambda s: int(s, 0), default=MOVIE_OFFSET,
                    help="with --from-archive: the offset of the movie's zlib stream (default %#x)" % MOVIE_OFFSET)
    ap.add_argument("--movie", help="read this inflated GFX/SWF movie file instead")
    ap.add_argument("--out", required=True,
                    help="folder for the PNGs, sheet.png and README.txt (outside the repository)")
    ap.add_argument("--scale", type=int, default=4, help="pixels per pixel of the title's canvas (default 4)")
    ap.add_argument("--canvas", default="64x64", help="the canvas of one glyph, WxH pixels (default 64x64)")
    ap.add_argument("--margin", type=int, default=1, help="canvas pixels added on every side (default 1)")
    ap.add_argument("--columns", type=int, default=8, help="glyphs in a row of the contact sheet (default 8)")
    args = ap.parse_args(argv)
    m = re.fullmatch(r"(\d+)x(\d+)", args.canvas)
    if not m or args.scale < 1 or args.columns < 1 or args.margin < 0 or not int(m.group(1)) or not int(m.group(2)):
        ap.error("--canvas is WxH in pixels, --scale and --columns are 1 or more, --margin 0 or more")
    if bool(args.from_archive) == bool(args.movie):
        ap.error("give one of --from-archive and --movie")
    try:
        if args.from_archive:
            data, stored = read_stream(args.from_archive, args.offset)
            source = ("%s, the zlib stream at archive offset %#x (%d bytes stored, %d inflated)"
                      % (os.path.basename(args.from_archive), args.offset, stored, len(data)))
        else:
            with open(args.movie, "rb") as f:
                data = f.read()
            source = "%s (%d bytes)" % (os.path.basename(args.movie), len(data))
        names = write_all(Movie(data), args.out, source, args.scale, (int(m.group(1)), int(m.group(2))), args.margin,
                          args.columns)
    except (ValueError, OSError) as e:
        sys.exit("xbox_prompt_glyphs: %s" % e)
    print("%d glyphs, sheet.png and README.txt written to %s" % (len(names), os.path.abspath(args.out)))


if __name__ == "__main__":
    main(sys.argv[1:])
