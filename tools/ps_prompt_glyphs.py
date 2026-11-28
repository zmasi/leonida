#!/usr/bin/env python3
"""Builds an original Scaleform GFX movie of PlayStation-style button prompt glyphs.

The title's input-icon movie exports one-frame sprites ``platform_xbsx.input_icons.Main_Button_<N>`` (N = 4..39,
58, 59: see docs/prompt_glyphs.md). This tool draws a replacement set of its own: every shape below is geometry
written here (discs, polygons, thick polylines, a tiny stroke font), nothing is traced from the game or from any
console maker's artwork. It needs no game file and writes only the files you name.

    python -I tools/ps_prompt_glyphs.py --out glyphs.gfx            # the movie, uncompressed (GFX header + tags)
    python -I tools/ps_prompt_glyphs.py --zlib glyphs.zlib          # the same, as one zlib stream (how it is stored)
    python -I tools/ps_prompt_glyphs.py --zlib glyphs.zlib --pad-to 9644   # zero-padded to a stored size
    python -I tools/ps_prompt_glyphs.py --selftest

The runtime (leonida-hle, `glyphs`) serves a movie of exactly the title's size (28,148 bytes) from
`ps_prompt_glyphs.gfx` beside leonida.exe (or `LEONIDA_PS_GLYPHS`). Two ways to build it:

    # over the title's own movie: keeps its ActionScript, export and class tables as they are and swaps only the
    # sprites' drawings. Reads the stream from your own data.rpf (read-only); the output holds those non-drawing
    # tags of the game's movie, so keep it on your PC and never commit it.
    python -I tools/ps_prompt_glyphs.py --from-archive "<package>/data.rpf" --out ps_prompt_glyphs.gfx

    # standalone: our own ActionScript block too (never run in the title), padded to the title's size
    python -I tools/ps_prompt_glyphs.py --pad-inflated 28148 --out ps_prompt_glyphs.gfx

The standalone movie holds, besides the sprites and their export names (tag 56), a small ActionScript 3 block (DoABC) that
declares one empty ``MovieClip`` subclass per export, and a SymbolClass tag binding each sprite to its class, so a
loader that looks symbols up by class name or by export name finds them either way.

Canvas: each glyph sits in the 64 x 64 pixel box (1280 twips) the Xbox glyphs use, registration at its top left, and
an invisible rectangle of the Xbox glyph's own bounds keeps the sprite's bounds the same.
"""

import argparse
import math
import struct
import sys
import zlib

TWIP = 20  # twips per pixel
BOX = 64 * TWIP

# ---------------------------------------------------------------- the glyph set

# N -> (button, family). The mapping from N to the Xbox button is documented in docs/prompt_glyphs.md.
GLYPHS = {
    4: "dpad up", 5: "dpad down", 6: "dpad left", 7: "dpad right", 8: "dpad idle", 9: "dpad all",
    10: "dpad up/down", 11: "dpad left/right",
    12: "L up", 13: "L down", 14: "L left", 15: "L right", 16: "L press", 17: "L plain", 18: "L up/down",
    19: "L left/right", 20: "L rotate a", 21: "R up", 22: "R down", 23: "R left", 24: "R right", 25: "R press",
    26: "R plain", 27: "R up/down", 28: "R left/right", 29: "R rotate a",
    30: "cross", 31: "circle", 32: "square", 33: "triangle",
    34: "L1", 35: "L2", 36: "R1", 37: "R2", 38: "options", 39: "touchpad",
    58: "L rotate b", 59: "R rotate b",
}

# Bounds (x0, y0, x1, y1) in twips of the Xbox glyphs that are not the plain 64 x 64 box.
BOUNDS = {
    16: (4, 21, 1281, 1265), 25: (4, 21, 1281, 1265),
    34: (3, 252, 1282, 1027), 36: (-2, 252, 1277, 1027),
    35: (31, 0, 1254, 1280), 37: (31, 0, 1250, 1280),
}

EXPORT_PREFIX = "platform_xbsx.input_icons."
PACKAGE = "platform_xbsx.input_icons"

PANEL = (28, 30, 42, 255)
RIM = (226, 229, 238, 255)
INK = (244, 245, 250, 255)
GREY = (92, 96, 112, 255)
LIT = (255, 255, 255, 255)
BLUE = (104, 160, 255, 255)
RED = (255, 92, 104, 255)
PINK = (255, 144, 212, 255)
GREEN = (72, 214, 164, 255)
CLEAR = (0, 0, 0, 0)

# ---------------------------------------------------------------- bit writer


class Bits:
    def __init__(self):
        self.out = bytearray()
        self.acc = 0
        self.n = 0

    def ub(self, value, nbits):
        for i in range(nbits - 1, -1, -1):
            self.acc = (self.acc << 1) | ((value >> i) & 1)
            self.n += 1
            if self.n == 8:
                self.out.append(self.acc)
                self.acc = self.n = 0

    def sb(self, value, nbits):
        self.ub(value & ((1 << nbits) - 1), nbits)

    def bytes(self):
        if self.n:
            self.out.append(self.acc << (8 - self.n))
            self.acc = self.n = 0
        return bytes(self.out)


def sbits(*values):
    """Bits needed to store all values as signed numbers."""
    need = 1
    for v in values:
        need = max(need, (v if v >= 0 else ~v).bit_length() + 1)
    return need


def rect_bytes(x0, x1, y0, y1):
    n = sbits(x0, x1, y0, y1)
    b = Bits()
    b.ub(n, 5)
    for v in (x0, x1, y0, y1):
        b.sb(v, n)
    return b.bytes()


# ---------------------------------------------------------------- tags


def tag(code, body):
    if len(body) < 63:
        return struct.pack("<H", code << 6 | len(body)) + body
    return struct.pack("<HI", code << 6 | 63, len(body)) + body


def cstr(s):
    return s.encode("ascii") + b"\0"


# ---------------------------------------------------------------- geometry

# A contour is a start point and a list of segments: ("L", (x, y)) or ("Q", (cx, cy), (x, y)); all in twips.


def circle_contour(cx, cy, r):
    """A circle as 8 quadratic arcs (control points at the tangent intersections)."""
    pts = lambda a, rad: (cx + rad * math.cos(a), cy + rad * math.sin(a))
    start = pts(0.0, r)
    segs = []
    ctrl = r / math.cos(math.pi / 8)
    for k in range(8):
        a0 = k * math.pi / 4
        segs.append(("Q", pts(a0 + math.pi / 8, ctrl), pts(a0 + math.pi / 4, r)))
    return start, segs


def poly_contour(points):
    return points[0], [("L", p) for p in points[1:]] + [("L", points[0])]


def area(points):
    return sum(points[i][0] * points[(i + 1) % len(points)][1] - points[(i + 1) % len(points)][0] * points[i][1]
               for i in range(len(points))) / 2


def clockwise(points):
    """Screen-clockwise (y down) order: the interior is to the right of each edge, as fill style 1 wants."""
    return points if area(points) > 0 else points[::-1]


def rounded_rect(x0, y0, x1, y1, radii):
    """Rounded rectangle; radii = (top-left, top-right, bottom-right, bottom-left)."""
    tl, tr, br, bl = radii
    pts = []

    def arc(cx, cy, r, a0):
        if r <= 0:
            pts.append((cx, cy))
            return
        for i in range(7):
            a = math.radians(a0 + 90 * i / 6)
            pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))

    arc(x0 + tl, y0 + tl, tl, 180)
    arc(x1 - tr, y0 + tr, tr, 270)
    arc(x1 - br, y1 - br, br, 0)
    arc(x0 + bl, y1 - bl, bl, 90)
    return pts


def stroke(points, width, closed=False, miter=2.2):
    """A polyline as one outline polygon (mitre joins, flat ends)."""
    n = len(points)
    h = width / 2
    normals = []
    segs = n if closed else n - 1
    for i in range(segs):
        (x0, y0), (x1, y1) = points[i], points[(i + 1) % n]
        d = math.hypot(x1 - x0, y1 - y0)
        normals.append(((y1 - y0) / d, -(x1 - x0) / d))
    left, right = [], []
    for i in range(n):
        if not closed and i == 0:
            a = b = normals[0]
        elif not closed and i == n - 1:
            a = b = normals[-1]
        else:
            a, b = normals[(i - 1) % segs], normals[i % segs]
        mx, my = a[0] + b[0], a[1] + b[1]
        ml = math.hypot(mx, my)
        if ml < 1e-6:
            mx, my, ml = a[0], a[1], 1.0
        mx, my = mx / ml, my / ml
        cosine = max(mx * a[0] + my * a[1], 1 / miter)
        k = h / cosine
        left.append((points[i][0] + mx * k, points[i][1] + my * k))
        right.append((points[i][0] - mx * k, points[i][1] - my * k))
    return left + right[::-1]


# ---------------------------------------------------------------- shape characters


def contour_bytes(contours, color):
    """The DefineShape3 body after the id (bounds, one solid fill, the contours)."""
    xs, ys = [], []
    rounded = []
    for start, segs in contours:
        rs = (round(start[0]), round(start[1]))
        rsegs = []
        xs.append(rs[0])
        ys.append(rs[1])
        for s in segs:
            r = tuple((round(p[0]), round(p[1])) for p in s[1:])
            for p in r:
                xs.append(p[0])
                ys.append(p[1])
            rsegs.append((s[0],) + r)
        rounded.append((rs, rsegs))
    out = rect_bytes(min(xs), max(xs), min(ys), max(ys))
    out += bytes([1, 0]) + bytes(color) + bytes([0])  # one solid fill, no line styles
    b = Bits()
    b.ub(1, 4)  # fill index bits
    b.ub(0, 4)  # line index bits
    for start, segs in rounded:
        n = sbits(*start)
        b.ub(0, 1)
        b.ub(0b00101, 5)  # fill style 1 selected, move to
        b.ub(n, 5)
        b.sb(start[0], n)
        b.sb(start[1], n)
        b.ub(1, 1)  # fill style 1
        x, y = start
        for s in segs:
            if s[0] == "L":
                (nx, ny) = s[1]
                dx, dy = nx - x, ny - y
                n = max(sbits(dx, dy), 2)
                b.ub(1, 1)
                b.ub(1, 1)
                b.ub(n - 2, 4)
                b.ub(1, 1)  # general line
                b.sb(dx, n)
                b.sb(dy, n)
                x, y = nx, ny
            else:
                (cx, cy), (nx, ny) = s[1], s[2]
                d = (cx - x, cy - y, nx - cx, ny - cy)
                n = max(sbits(*d), 2)
                b.ub(1, 1)
                b.ub(0, 1)
                b.ub(n - 2, 4)
                for v in d:
                    b.sb(v, n)
                x, y = nx, ny
    b.ub(0, 1)
    b.ub(0, 5)  # end of shape
    return out + b.bytes()


class Movie:
    def __init__(self):
        self.shapes = {}  # key -> (id, body)
        self.order = []
        self.next_id = 1

    def shape(self, contours, color):
        """Returns (character id, tx, ty): the shape is stored relative to its own top left."""
        xs = [round(c[0][0]) for c in contours] + [round(p[0]) for c in contours for s in c[1] for p in s[1:]]
        ys = [round(c[0][1]) for c in contours] + [round(p[1]) for c in contours for s in c[1] for p in s[1:]]
        ox, oy = min(xs), min(ys)
        norm = []
        for start, segs in contours:
            norm.append(((round(start[0]) - ox, round(start[1]) - oy),
                         [(s[0],) + tuple((round(p[0]) - ox, round(p[1]) - oy) for p in s[1:]) for s in segs]))
        key = (repr(norm), tuple(color))
        if key not in self.shapes:
            body = struct.pack("<H", self.next_id) + contour_bytes(norm, color)
            self.shapes[key] = (self.next_id, body)
            self.order.append(key)
            self.next_id += 1
        return self.shapes[key][0], ox, oy


class Glyph:
    """The pieces placed in one sprite, bottom to top."""

    def __init__(self, movie):
        self.m = movie
        self.items = []

    def poly(self, points, color):
        self.items.append(self.m.shape([poly_contour([(x * TWIP, y * TWIP) for x, y in clockwise(points)])], color))

    def disc(self, cx, cy, r, color):
        self.items.append(self.m.shape([circle_contour(cx * TWIP, cy * TWIP, r * TWIP)], color))

    def stroke(self, points, width, color, closed=False):
        self.poly(stroke(points, width, closed), color)

    def rrect(self, x0, y0, x1, y1, radii, color):
        self.poly(rounded_rect(x0, y0, x1, y1, radii), color)

    def box(self, x0, y0, x1, y1, color):
        self.poly([(x0, y0), (x1, y0), (x1, y1), (x0, y1)], color)


# ---------------------------------------------------------------- the stroke font (units: a letter is 0.6 x 1)


def arc(cx, cy, r, a0, a1, steps=10):
    return [(cx + r * math.cos(math.radians(a0 + (a1 - a0) * i / steps)),
             cy + r * math.sin(math.radians(a0 + (a1 - a0) * i / steps))) for i in range(steps + 1)]


FONT = {
    "L": [[(0.0, 0.0), (0.0, 1.0), (0.58, 1.0)]],
    "R": [[(0.0, 0.0), (0.0, 1.0)],
          [(-0.02, 0.04)] + arc(0.3, 0.27, 0.23, 270, 450) + [(-0.02, 0.5)],
          [(0.3, 0.5), (0.6, 1.0)]],
    "1": [[(0.02, 0.2), (0.3, 0.0), (0.3, 1.0)]],
    "2": [arc(0.3, 0.28, 0.26, 195, 395) + [(0.0, 1.0), (0.6, 1.0)]],
    "3": [arc(0.27, 0.26, 0.24, 205, 450), arc(0.27, 0.74, 0.26, 270, 525)],
    "T": [[(0.0, 0.0), (0.6, 0.0)], [(0.3, 0.0), (0.3, 1.0)]],
    "B": [[(0.0, 0.0), (0.0, 1.0)]],
}


def text(g, s, cx, cy, h, color):
    """Centred text of height `h` pixels."""
    w = 0.6 * h
    gap = 0.16 * h
    total = len(s) * w + (len(s) - 1) * gap
    x = cx - total / 2
    y = cy - h / 2
    thick = 0.17 * h
    for ch in s:
        for line in FONT[ch]:
            pts = [(x + px * w / 0.6, y + py * h) for px, py in line]
            g.stroke(pts, thick, color)
        x += w + gap


# ---------------------------------------------------------------- glyph drawings


def disc_base(g):
    g.disc(32, 32, 32, RIM)
    g.disc(32, 32, 29.5, PANEL)


def tri(g, cx, cy, w, h, direction, color):
    hw, hh = w / 2, h / 2
    pts = {"up": [(cx, cy - hh), (cx + hw, cy + hh), (cx - hw, cy + hh)],
           "down": [(cx, cy + hh), (cx - hw, cy - hh), (cx + hw, cy - hh)],
           "left": [(cx - hh, cy), (cx + hh, cy - hw), (cx + hh, cy + hw)],
           "right": [(cx + hh, cy), (cx - hh, cy + hw), (cx - hh, cy - hw)]}[direction]
    g.poly(pts, color)


def draw_dpad(g, lit):
    arm = 24.0
    lo, hi = 32 - arm / 2, 32 + arm / 2
    cross = lambda inset: [
        (lo + inset, inset), (hi - inset, inset), (hi - inset, lo + inset), (64 - inset, lo + inset),
        (64 - inset, hi - inset), (hi - inset, hi - inset), (hi - inset, 64 - inset), (lo + inset, 64 - inset),
        (lo + inset, hi - inset), (inset, hi - inset), (inset, lo + inset), (lo + inset, lo + inset)]
    g.poly(cross(0), RIM)
    g.poly(cross(3), LIT if lit == "all" else GREY)
    a, b = lo + 3, hi - 3
    if lit in ("up", "updown"):
        g.box(a, 3, b, a + 0.01 + 3 + 17, LIT)
    if lit in ("down", "updown"):
        g.box(a, b - 17, b, 61, LIT)
    if lit in ("left", "leftright"):
        g.box(3, a, a + 20, b, LIT)
    if lit in ("right", "leftright"):
        g.box(b - 20, a, 61, b, LIT)


DPAD = {4: "up", 5: "down", 6: "left", 7: "right", 8: "idle", 9: "all", 10: "updown", 11: "leftright"}


def draw_stick(g, side, mode):
    disc_base(g)
    letter = "L" if side == "L" else "R"
    h = 26.0
    cy = 32.0
    if mode in ("up", "updown"):
        cy += 3.5 if mode == "up" else 0
        h = 22 if mode == "updown" else 24
    if mode == "down":
        cy -= 3.5
        h = 24
    if mode in ("left", "right", "leftright"):
        h = 26
    cx = 32.0 + {"left": 4.5, "right": -4.5}.get(mode, 0)
    text(g, letter, cx, cy, h, INK)
    if mode in ("up", "updown"):
        tri(g, 32, 11 if mode == "up" else 10, 13, 8, "up", INK)
    if mode in ("down", "updown"):
        tri(g, 32, 53 if mode == "down" else 54, 13, 8, "down", INK)
    if mode in ("left", "leftright"):
        tri(g, 11, 32, 13, 8, "left", INK)
    if mode in ("right", "leftright"):
        tri(g, 53, 32, 13, 8, "right", INK)


def draw_rotate(g, side, flip):
    disc_base(g)
    text(g, side, 32, 32, 24, INK)
    # An open ring with an arrowhead at its end.
    sweep = [(32 + 24.5 * math.cos(math.radians(a)), 32 + 24.5 * math.sin(math.radians(a))) for a in
             [(-90 + 235 * i / 24) if not flip else (-90 - 235 * i / 24) for i in range(25)]]
    g.stroke(sweep, 3.2, INK)
    ex, ey = sweep[-1]
    px, py = sweep[-3]
    d = math.hypot(ex - px, ey - py)
    ux, uy = (ex - px) / d, (ey - py) / d
    nx, ny = -uy, ux
    tip = (ex + ux * 6, ey + uy * 6)
    g.poly([tip, (ex + nx * 5.5, ey + ny * 5.5), (ex - nx * 5.5, ey - ny * 5.5)], INK)


def draw_press(g, side):
    """A stick cap with an arrow pressing down onto it and the L3/R3 label."""
    g.poly([(32, 14), (21, 2), (43, 2)], RIM)
    g.poly([(32, 11.6), (24.5, 3.9), (39.5, 3.9)], PANEL)
    g.rrect(0.2, 17, 63.9, 29, (6, 6, 6, 6), RIM)
    g.rrect(2.2, 19, 61.9, 27, (4, 4, 4, 4), PANEL)
    g.rrect(10, 29, 54, 63, (0, 0, 10, 10), RIM)
    g.rrect(12, 29, 52, 61, (0, 0, 8, 8), PANEL)
    text(g, side + "3", 32, 46, 16, INK)


def draw_face(g, which):
    disc_base(g)
    if which == "cross":
        g.stroke([(20.5, 20.5), (43.5, 43.5)], 5.2, BLUE)
        g.stroke([(43.5, 20.5), (20.5, 43.5)], 5.2, BLUE)
    elif which == "circle":
        g.disc(32, 32, 14.5, RED)
        g.disc(32, 32, 10, PANEL)
    elif which == "square":
        g.box(19, 19, 45, 45, PINK)
        g.box(23.4, 23.4, 40.6, 40.6, PANEL)
    else:
        pts = [(32, 14.5), (47.5, 41.5), (16.5, 41.5)]
        g.poly(pts, GREEN)
        cx = sum(p[0] for p in pts) / 3
        cy = sum(p[1] for p in pts) / 3
        k = 0.55
        g.poly([(cx + (x - cx) * k, cy + (y - cy) * k) for x, y in pts], PANEL)


def draw_shoulder(g, label):
    g.rrect(0.2, 12.6, 63.9, 51.4, (16, 20, 8, 8), RIM)
    g.rrect(2.6, 15, 61.5, 49, (14, 18, 6, 6), PANEL)
    text(g, label, 32, 32, 19, INK)


def draw_trigger(g, label, left):
    radii = (8, 26, 10, 10) if not left else (26, 8, 10, 10)
    x0, x1 = 1.6, 62.7
    g.rrect(x0, 0, x1, 64, radii, RIM)
    g.rrect(x0 + 2.4, 2.4, x1 - 2.4, 61.6, tuple(max(r - 2.4, 0) for r in radii), PANEL)
    text(g, label, 32, 33, 21, INK)


def draw_options(g):
    disc_base(g)
    for y in (22.5, 30.5, 38.5):
        g.rrect(17, y - 1.7, 47, y + 1.7, (1.7, 1.7, 1.7, 1.7), INK)


def draw_touchpad(g):
    disc_base(g)
    g.rrect(14, 19, 50, 45, (6, 6, 9, 9), INK)
    g.rrect(16.6, 21.6, 47.4, 42.4, (4, 4, 7, 7), PANEL)
    g.box(31, 22, 33, 42, INK)


def build_sprite(movie, n):
    g = Glyph(movie)
    x0, y0, x1, y1 = BOUNDS.get(n, (0, 0, BOX, BOX))
    g.items.append(movie.shape([poly_contour(clockwise([(x0, y0), (x1, y0), (x1, y1), (x0, y1)]))], CLEAR))
    if n in DPAD:
        draw_dpad(g, DPAD[n])
    elif n in (12, 13, 14, 15, 17, 18, 19):
        draw_stick(g, "L", {12: "up", 13: "down", 14: "left", 15: "right", 17: "plain", 18: "updown", 19: "leftright"}[n])
    elif n in (21, 22, 23, 24, 26, 27, 28):
        draw_stick(g, "R", {21: "up", 22: "down", 23: "left", 24: "right", 26: "plain", 27: "updown", 28: "leftright"}[n])
    elif n in (20, 58):
        draw_rotate(g, "L", n == 58)
    elif n in (29, 59):
        draw_rotate(g, "R", n == 59)
    elif n in (16, 25):
        draw_press(g, "L" if n == 16 else "R")
    elif n in (30, 31, 32, 33):
        draw_face(g, {30: "cross", 31: "circle", 32: "square", 33: "triangle"}[n])
    elif n in (34, 36):
        draw_shoulder(g, "L1" if n == 34 else "R1")
    elif n in (35, 37):
        draw_trigger(g, "L2" if n == 35 else "R2", n == 35)
    elif n == 38:
        draw_options(g)
    elif n == 39:
        draw_touchpad(g)
    else:
        raise ValueError(n)
    return g


# ---------------------------------------------------------------- ActionScript 3 block


def u30(v):
    out = bytearray()
    while True:
        byte = v & 0x7F
        v >>= 7
        if v:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def build_abc(names):
    """An ABC file declaring `Main` and one empty MovieClip subclass per name, all in PACKAGE."""
    classes = ["Main"] + list(names)
    strings = [PACKAGE, "flash.display", "MovieClip"] + classes
    sidx = {s: i + 1 for i, s in enumerate(strings)}
    out = bytearray()
    out += struct.pack("<HH", 16, 46)  # minor, major version
    out += u30(1) + u30(1) + u30(1)  # no ints, uints, doubles
    out += u30(len(strings) + 1)
    for s in strings:
        raw = s.encode("ascii")
        out += u30(len(raw)) + raw
    out += u30(3)  # namespaces: 1 = package PACKAGE, 2 = package flash.display
    out += bytes([0x16]) + u30(sidx[PACKAGE]) + bytes([0x16]) + u30(sidx["flash.display"])
    out += u30(1)  # no ns sets
    mnames = ["MovieClip"] + classes  # multiname i + 1
    out += u30(len(mnames) + 1)
    for i, m in enumerate(mnames):
        ns = 2 if i == 0 else 1
        out += bytes([0x07]) + u30(ns) + u30(sidx[m])
    mc = 1
    cls_mn = lambda i: i + 2
    count = len(classes)
    # methods: per class iinit (2i) and cinit (2i + 1), then the script init
    out += u30(2 * count + 1)
    for _ in range(2 * count + 1):
        out += u30(0) + u30(0) + u30(0) + bytes([0])
    out += u30(0)  # no metadata
    out += u30(count)
    for i in range(count):
        out += u30(cls_mn(i)) + u30(mc) + bytes([0]) + u30(0) + u30(2 * i) + u30(0)
    for i in range(count):
        out += u30(2 * i + 1) + u30(0)
    out += u30(1)  # one script
    out += u30(2 * count)
    out += u30(count)
    for i in range(count):
        out += u30(cls_mn(i)) + bytes([0x04]) + u30(0) + u30(i)
    bodies = bytearray()
    for i in range(count):
        iinit = bytes([0xD0, 0x30, 0xD0, 0x49, 0x00, 0x47])
        bodies += u30(2 * i) + u30(1) + u30(1) + u30(2) + u30(3) + u30(len(iinit)) + iinit + u30(0) + u30(0)
        cinit = bytes([0xD0, 0x30, 0x47])
        bodies += u30(2 * i + 1) + u30(1) + u30(1) + u30(1) + u30(2) + u30(len(cinit)) + cinit + u30(0) + u30(0)
    code = bytearray([0xD0, 0x30])
    for i in range(count):
        code += bytes([0x65, 0x00, 0x60]) + u30(mc) + bytes([0x30, 0x60]) + u30(mc)
        code += bytes([0x58]) + u30(i) + bytes([0x1D, 0x68]) + u30(cls_mn(i))
    code += bytes([0x47])
    bodies += u30(2 * count) + u30(3) + u30(1) + u30(0) + u30(2) + u30(len(code)) + bytes(code) + u30(0) + u30(0)
    out += u30(2 * count + 1) + bytes(bodies)
    return bytes(out)


# ---------------------------------------------------------------- the movie

# Frame size, rate and frame count of the movie (a 1-frame, 30 fps, 150 x 150 px stage), as RECT + fixed 8.8 + u16.
HEADER_FRAME = rect_bytes(0, 150 * TWIP, 0, 150 * TWIP) + struct.pack("<HH", 30 << 8, 1)


def sprite_tag(sid, g):
    """DefineSprite `sid` of one frame placing the glyph's shapes."""
    body = struct.pack("<HH", sid, 1)
    for depth, (cid, tx, ty) in enumerate(g.items, 1):
        b = Bits()
        b.ub(0, 2)  # no scale, no rotate
        nb = sbits(tx, ty)
        b.ub(nb, 5)
        b.sb(tx, nb)
        b.sb(ty, nb)
        body += tag(26, bytes([0x06]) + struct.pack("<HH", depth, cid) + b.bytes())
    body += tag(1, b"") + tag(0, b"")
    return tag(39, body)


def padding_tag(size):
    """A Metadata tag (77, empty text) of exactly `size` bytes, which every player skips."""
    if size < 2:
        raise ValueError("cannot pad by %d byte(s)" % size)
    if size <= 64:
        return struct.pack("<H", 77 << 6 | (size - 2)) + bytes(size - 2)
    return struct.pack("<HI", 77 << 6 | 63, size - 6) + bytes(size - 6)


def pad_movie(data, size):
    """`data` (a whole movie ending with the End tag) grown to `size` bytes by a padding tag before its End tag,
    and its length field set."""
    if len(data) > size:
        raise ValueError("the movie is %d bytes, over %d" % (len(data), size))
    if len(data) < size:
        data = data[:-2] + padding_tag(size - len(data)) + data[-2:]
    return data[:4] + struct.pack("<I", len(data)) + data[8:]


def read_tags(data):
    """The top-level tags of a movie, End included: [(code, body)], and the offset the first one starts at."""
    n = data[8] >> 3
    o = 8 + (5 + 4 * n + 7) // 8 + 4  # frame rectangle, rate and frame count
    start, out = o, []
    while o < len(data):
        head = struct.unpack_from("<H", data, o)[0]
        code, length = head >> 6, head & 63
        o += 2
        if length == 63:
            length = struct.unpack_from("<I", data, o)[0]
            o += 4
        out.append((code, data[o:o + length]))
        o += length
        if code == 0:
            break
    return out, start


def exports_of(tags):
    """{character id: N} of the `Main_Button_<N>` exports in a movie's ExportAssets tags."""
    prefix = EXPORT_PREFIX + "Main_Button_"
    found = {}
    for code, body in tags:
        if code != 56:
            continue
        o = 2
        for _ in range(struct.unpack_from("<H", body, 0)[0]):
            cid = struct.unpack_from("<H", body, o)[0]
            end = body.index(b"\0", o + 2)
            name = body[o + 2:end].decode("ascii", "replace")
            o = end + 1
            if name.startswith(prefix):
                found[cid] = int(name[len(prefix):])
    return found


def build_over(base):
    """The title's own movie `base` with each exported sprite's drawing swapped for ours: same character ids, so its
    ExportAssets, SymbolClass and DoABC tags (the ActionScript) stay exactly as they are; its shapes are dropped and
    ours take their place before the first sprite. Padded to the length of `base`."""
    if base[:3] not in (b"GFX", b"FWS"):
        raise ValueError("not an uncompressed movie")
    tags, start = read_tags(base)
    ids = exports_of(tags)
    unknown = sorted(n for n in ids.values() if n not in GLYPHS)
    if unknown or not ids:
        raise ValueError("the movie's exports are not the ones this tool draws: %s" % (unknown or "none found"))
    movie = Movie()
    movie.next_id = 1 + max(struct.unpack_from("<H", body, 0)[0] for code, body in tags if code in (2, 22, 32, 39, 83))
    sprites = {cid: sprite_tag(cid, build_sprite(movie, n)) for cid, n in ids.items()}
    out, placed = [], False
    for code, body in tags:
        if code in (32, 39) and not placed:
            out.extend(tag(32, movie.shapes[key][1]) for key in movie.order)
            placed = True
        if code == 32:
            continue
        if code == 39:
            cid = struct.unpack_from("<H", body, 0)[0]
            if cid not in sprites:
                raise ValueError("a sprite (%d) that is not one of the exports" % cid)
            out.append(sprites[cid])
            continue
        out.append(tag(code, body))
    data = base[:4] + struct.pack("<I", 0) + base[8:start] + b"".join(out)
    return pad_movie(data, len(base))


def read_archive_movie(path, offset=0x117D7000, size=28148):
    """The inflated movie in the title's archive (a zlib stream at `offset`), read-only."""
    with open(path, "rb") as f:
        f.seek(offset)
        raw = f.read(1 << 20)
    data = zlib.decompressobj().decompress(raw, size + 1)
    if len(data) != size or data[:3] != b"GFX":
        raise ValueError("%s has no %d byte GFX movie at %#x" % (path, size, offset))
    return data


def build():
    movie = Movie()
    sprites = []  # (n, character id placeholder, glyph)
    for n in sorted(GLYPHS):
        sprites.append((n, build_sprite(movie, n)))
    next_id = movie.next_id
    tags = [
        tag(1000, bytes([1, 4, 0, 0, 0, 0, 0x0E, 0, 0, 0x0B]) + b"input_icons\0\0"),  # exporter info
        tag(69, bytes([9, 0, 0, 0])),  # FileAttributes: ActionScript 3, uses network
        tag(9, bytes([0, 0, 0])),
        tag(43, cstr("platform_xbsx_input_icons_Main")),
    ]
    for key in movie.order:
        tags.append(tag(32, movie.shapes[key][1]))
    exports = []
    classes = []
    for i, (n, g) in enumerate(sprites):
        sid = next_id + i
        tags.append(sprite_tag(sid, g))
        exports.append((sid, EXPORT_PREFIX + "Main_Button_%d" % n))
        classes.append(PACKAGE + ".Main_Button_%d" % n)
    tags.append(tag(56, struct.pack("<H", len(exports)) + b"".join(struct.pack("<H", s) + cstr(nm) for s, nm in exports)))
    abc = build_abc(["Main_Button_%d" % n for n, _ in sprites])
    tags.append(tag(82, struct.pack("<I", 1) + cstr("frame1") + abc))
    sym = [(s, nm) for s, nm in exports] + [(0, PACKAGE + ".Main")]
    tags.append(tag(76, struct.pack("<H", len(sym)) + b"".join(struct.pack("<H", s) + cstr(nm) for s, nm in sym)))
    tags.append(tag(1, b""))
    tags.append(tag(0, b""))
    body = HEADER_FRAME + b"".join(tags)
    # "GFX" is the uncompressed Scaleform container (SWF's "FWS"); 42 is the version byte the Xbox movie has.
    return b"GFX" + bytes([42]) + struct.pack("<I", 8 + len(body)) + body


def selftest():
    data = build()
    assert data[:4] == b"GFX*" and struct.unpack_from("<I", data, 4)[0] == len(data)
    assert zlib.decompress(zlib.compress(data, 9)) == data
    print("ok: %d bytes, %d zlib bytes" % (len(data), len(zlib.compress(data, 9))))


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--out", help="write the uncompressed movie (GFX header + tags) here")
    ap.add_argument("--zlib", help="write the movie as one zlib stream here")
    ap.add_argument("--pad-to", type=int, help="with --zlib: zero-pad the stream to this many bytes")
    ap.add_argument("--pad-inflated", type=int, help="pad the movie to this many bytes (28148: the title's)")
    ap.add_argument("--from-archive", help="build over the title's own movie in this data.rpf (see above)")
    ap.add_argument("--base", help="build over this inflated movie file instead of the archive")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args(argv)
    if args.selftest:
        selftest()
    if args.from_archive or args.base:
        if args.base:
            with open(args.base, "rb") as f:
                base = f.read()
        else:
            base = read_archive_movie(args.from_archive)
        data = build_over(base)
    else:
        data = build()
        if args.pad_inflated:
            data = pad_movie(data, args.pad_inflated)
    if args.out:
        with open(args.out, "wb") as f:
            f.write(data)
    if args.zlib:
        packed = zlib.compress(data, 9)
        if args.pad_to:
            if len(packed) > args.pad_to:
                sys.exit("the zlib stream (%d bytes) does not fit in %d" % (len(packed), args.pad_to))
            packed += bytes(args.pad_to - len(packed))
        with open(args.zlib, "wb") as f:
            f.write(packed)
    if not (args.out or args.zlib or args.selftest):
        ap.print_help()


if __name__ == "__main__":
    main(sys.argv[1:])
