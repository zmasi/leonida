"""tools/xbox_prompt_glyphs.py: shapes, sprites and the rasteriser, on movies built by hand here (no game data)."""
import math
import os
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import ps_prompt_glyphs as g  # noqa: E402
import xbox_prompt_glyphs as x  # noqa: E402

T = g.TWIP
RED, BLUE, BLACK, WHITE = (200, 30, 20, 255), (10, 40, 220, 255), (0, 0, 0, 255), (255, 255, 255, 255)


def shape_body(cid, bounds, fills, lines, records, kinds=None):
    """A DefineShape3 body written record by record (twips). Records: ("move", x, y, f0, f1, ln), ("line", dx, dy),
    ("curve", cdx, cdy, adx, ady) and ("styles", fills, lines, f1): new style tables and fill style 1 of them."""
    def tables(fills, lines):
        return (bytes([len(fills)]) + b"".join(bytes([(kinds or {}).get(i, 0)]) + bytes(c) for i, c in enumerate(fills))
                + bytes([len(lines)]) + b"".join(struct.pack("<H", w) + bytes(c) for w, c in lines))

    b = g.Bits()
    fb, lb = len(fills).bit_length(), len(lines).bit_length()
    b.ub(fb, 4)
    b.ub(lb, 4)
    for rec in records:
        if rec[0] == "move":
            _, px, py, f0, f1, ln = rec
            b.ub(0, 1)
            b.ub((ln is not None) << 3 | (f1 is not None) << 2 | (f0 is not None) << 1 | 1, 5)
            n = g.sbits(px, py)
            b.ub(n, 5)
            b.sb(px, n)
            b.sb(py, n)
            for value, bits in ((f0, fb), (f1, fb), (ln, lb)):
                if value is not None:
                    b.ub(value, bits)
        elif rec[0] == "line":
            _, dx, dy = rec
            n = max(g.sbits(dx, dy), 2)
            b.ub(0b11, 2)
            b.ub(n - 2, 4)
            if dx and dy:
                b.ub(1, 1)
                b.sb(dx, n)
                b.sb(dy, n)
            else:  # the short forms: a vertical or a horizontal line
                b.ub(0, 1)
                b.ub(1 if dx == 0 else 0, 1)
                b.sb(dy if dx == 0 else dx, n)
        elif rec[0] == "curve":
            n = max(g.sbits(*rec[1:]), 2)
            b.ub(0b10, 2)
            b.ub(n - 2, 4)
            for v in rec[1:]:
                b.sb(v, n)
        else:
            _, new_fills, new_lines, f1 = rec
            b.ub(0, 1)
            b.ub(0b10100, 5)  # new styles, fill style 1
            b.ub(f1, fb)
            b.bytes()  # the tables start on a byte
            b.out += tables(new_fills, new_lines)
            fb, lb = len(new_fills).bit_length(), len(new_lines).bit_length()
            b.ub(fb, 4)
            b.ub(lb, 4)
    b.ub(0, 6)
    return struct.pack("<H", cid) + g.rect_bytes(*bounds) + tables(fills, lines) + b.bytes()


def box_records(x0, y0, x1, y1, f0=None, f1=None, ln=None):
    """A clockwise rectangle (pixels) as records."""
    w, h = (x1 - x0) * T, (y1 - y0) * T
    return [("move", round(x0 * T), round(y0 * T), f0, f1, ln), ("line", round(w), 0), ("line", 0, round(h)),
            ("line", -round(w), 0), ("line", 0, -round(h))]


def box(cid, x0, y0, x1, y1, color, side=1):
    return shape_body(cid, (round(x0 * T), round(x1 * T), round(y0 * T), round(y1 * T)), [color], [],
                      box_records(x0, y0, x1, y1, f1=1) if side == 1 else box_records(x0, y0, x1, y1, f0=1))


def place(depth, cid, tx=0, ty=0, scale=None, flags=0x06, extra=b""):
    b = g.Bits()
    if scale is None:
        b.ub(0, 1)
    else:
        v = round(scale * 65536)
        n = g.sbits(v)
        b.ub(1, 1)
        b.ub(n, 5)
        b.sb(v, n)
        b.sb(v, n)
    b.ub(0, 1)  # no rotation
    n = g.sbits(tx, ty)
    b.ub(n, 5)
    b.sb(tx, n)
    b.sb(ty, n)
    return g.tag(26, bytes([flags]) + struct.pack("<HH", depth, cid) + b.bytes() + extra)


def movie(shapes, sprites, signature=b"GFX"):
    """shapes: DefineShape3 bodies; sprites: {id: [PlaceObject2 tags]}, each exported as "s<id>"."""
    tags = [g.tag(32, body) for body in shapes]
    for sid, placed in sprites.items():
        tags.append(g.tag(39, struct.pack("<HH", sid, 1) + b"".join(placed) + g.tag(1, b"") + g.tag(0, b"")))
    tags.append(g.tag(56, struct.pack("<H", len(sprites))
                      + b"".join(struct.pack("<H", sid) + g.cstr("s%d" % sid) for sid in sprites)))
    body = g.HEADER_FRAME + b"".join(tags) + g.tag(1, b"") + g.tag(0, b"")
    return signature + bytes([42]) + struct.pack("<I", 8 + len(body)) + body


def draw(shapes, placed=None, scale=1):
    """The picture of one sprite placing `shapes` (ids 1, 2, ...) as they are: pixel(x, y) -> (r, g, b, a)."""
    placed = placed or [place(i + 1, i + 1) for i in range(len(shapes))]
    w, h, rgba = x.render(x.Movie(movie(shapes, {100: placed})), 100, scale=scale)
    return lambda px, py: tuple(rgba[4 * (py * w + px):4 * (py * w + px) + 4])


def area(shapes, placed=None, scale=1):
    w, h, rgba = x.render(x.Movie(movie(shapes, {100: placed or [place(1, 1)]})), 100, scale=scale)
    return sum(rgba[3::4]) / 255.0 / scale / scale


def enclosed(contour):
    """The area in square pixels inside a closed contour of lines and quadratic curves (points rounded to twips,
    as they are stored)."""
    cross = lambda a, b: a[0] * b[1] - a[1] * b[0]
    at = lambda p: (round(p[0]), round(p[1]))
    p0, total = at(contour[0]), 0.0
    for seg in contour[1]:
        p1 = at(seg[-1])
        if seg[0] == "L":
            total += cross(p0, p1)
        else:
            c = at(seg[1])
            total += (2 * cross(p0, c) + 2 * cross(c, p1) + cross(p0, p1)) / 3
        p0 = p1
    return abs(total) / 2 / T / T


def unpng(data):
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    o, chunks = 8, []
    while o < len(data):
        n, kind = struct.unpack_from(">I4s", data, o)
        body = data[o + 8:o + 8 + n]
        assert struct.unpack_from(">I", data, o + 8 + n)[0] == zlib.crc32(kind + body), "chunk checksum"
        chunks.append((kind, body))
        o += 12 + n
    assert [k for k, _ in chunks] == [b"IHDR", b"IDAT", b"IEND"]
    w, h, depth, kind = struct.unpack_from(">IIBB", chunks[0][1])
    assert (depth, kind) == (8, 6)
    raw = zlib.decompress(chunks[1][1])
    assert len(raw) == h * (4 * w + 1) and not any(raw[y * (4 * w + 1)] for y in range(h))
    return w, h, b"".join(raw[y * (4 * w + 1) + 1:(y + 1) * (4 * w + 1)] for y in range(h))


class Fills(unittest.TestCase):
    def test_a_box_and_its_half_covered_edge(self):
        px = draw([box(1, 10.5, 10, 30, 30, RED)])
        self.assertEqual(px(11, 10), RED)
        self.assertEqual(px(29, 29), RED)
        self.assertEqual(px(10, 10), RED[:3] + (128,))  # half of the pixel, the colour not darkened
        for p in ((9, 10), (30, 20), (20, 9), (20, 30), (0, 0), (63, 63)):
            self.assertEqual(px(*p), (0, 0, 0, 0), p)

    def test_the_fill_on_either_side_of_the_edges(self):
        one = x.render(x.Movie(movie([box(1, 10.5, 10, 30, 30, RED)], {100: [place(1, 1)]})), 100, scale=1)
        other = x.render(x.Movie(movie([box(1, 10.5, 10, 30, 30, RED, side=0)], {100: [place(1, 1)]})), 100, scale=1)
        self.assertEqual(one, other)

    def test_two_fills_share_an_edge(self):
        # Red left of x = 20, blue right of it; the edge between them carries both (fill 1 red, fill 0 blue).
        records = [("move", 10 * T, 10 * T, None, 1, None), ("line", 10 * T, 0),
                   ("move", 20 * T, 10 * T, 2, 1, None), ("line", 0, 20 * T),
                   ("move", 20 * T, 30 * T, 0, 1, None), ("line", -10 * T, 0), ("line", 0, -20 * T),
                   ("move", 20 * T, 10 * T, 0, 2, None), ("line", 10 * T, 0), ("line", 0, 20 * T), ("line", -10 * T, 0)]
        px = draw([shape_body(1, (10 * T, 30 * T, 10 * T, 30 * T), [RED, BLUE], [], records)])
        self.assertEqual([px(15, 20), px(19, 29), px(20, 10), px(25, 20)], [RED, RED, BLUE, BLUE])
        self.assertEqual([px(9, 20), px(30, 20)], [(0, 0, 0, 0)] * 2)

    def test_curves(self):
        circle = g.circle_contour(32 * T, 32 * T, 20 * T)  # eight quadratic arcs: 0.4 % more than the disc
        body = struct.pack("<H", 1) + g.contour_bytes([circle], WHITE)
        self.assertAlmostEqual(enclosed(circle) / (math.pi * 400), 1.004, delta=0.001)
        self.assertAlmostEqual(area([body], scale=2) / enclosed(circle), 1.0, delta=0.001)
        px = draw([body])
        self.assertEqual([px(32, 13), px(32, 11), px(16, 16), px(18, 18)], [WHITE, (0, 0, 0, 0), (0, 0, 0, 0), WHITE])

    def test_new_styles_start_a_layer_on_top(self):
        records = box_records(10, 10, 40, 40, f1=1) + [("styles", [BLUE], [], 1)] + box_records(20, 20, 30, 30)[1:]
        records.insert(6, ("move", 20 * T, 20 * T, None, None, None))
        px = draw([shape_body(1, (10 * T, 40 * T, 10 * T, 40 * T), [RED], [], records)])
        self.assertEqual([px(15, 15), px(25, 25), px(35, 35), px(19, 25), px(20, 25)], [RED, BLUE, RED, RED, BLUE])

    def test_layers_blend_and_alpha_stays_straight(self):
        veil = (255, 255, 255, 128)
        px = draw([box(1, 10, 10, 30, 30, BLACK), box(2, 20, 10, 40, 30, veil)])
        self.assertEqual(px(15, 20), BLACK)
        self.assertEqual(px(35, 20), veil)
        self.assertEqual(px(25, 20), (128, 128, 128, 255))


class Strokes(unittest.TestCase):
    LINE = [("move", 16 * T, 32 * T, None, None, 1), ("line", 32 * T, 0)]

    def line(self, width):
        half = width * T // 2
        bounds = (16 * T - half, 48 * T + half, 32 * T - half, 32 * T + half)
        return shape_body(1, bounds, [], [(width * T, BLACK)], self.LINE)

    def test_width_and_round_caps(self):
        px = draw([self.line(8)])
        self.assertEqual([px(32, 28), px(32, 35), px(16, 32), px(13, 32), px(50, 31)], [BLACK] * 5)
        for p in ((32, 27), (32, 36), (11, 32), (52, 32), (12, 28), (51, 35)):  # the last two: a square cap's corners
            self.assertEqual(px(*p)[3], 0, p)
        self.assertAlmostEqual(area([self.line(8)], scale=2) / (32 * 8 + math.pi * 16), 1.0, delta=0.004)

    def test_a_joint_is_round_and_drawn_once(self):
        veil = (0, 0, 0, 128)
        records = [("move", 16 * T, 16 * T, None, None, 1), ("line", 24 * T, 0), ("line", 0, 24 * T)]
        px = draw([shape_body(1, (12 * T, 44 * T, 12 * T, 44 * T), [], [(8 * T, veil)], records)])
        self.assertEqual([px(30, 16), px(40, 30), px(40, 16), px(42, 14)], [veil] * 4)  # the overlap is not darker
        self.assertEqual(px(43, 12)[3], 0)  # the outer corner is rounded off

    def test_a_placement_scales_and_moves(self):
        px = draw([box(1, 0, 0, 10, 10, RED)], [place(1, 1, 4 * T, 6 * T, scale=2.0)])
        self.assertEqual([px(4, 6), px(23, 25)], [RED, RED])
        self.assertEqual([px(3, 6)[3], px(24, 25)[3], px(23, 26)[3], px(4, 5)[3]], [0] * 4)
        # The line becomes one from (16, 32) to (80, 32), 4 wide.
        px = draw([self.line(2)], [place(1, 1, -16 * T, -32 * T, scale=2.0)])
        self.assertEqual([px(40, 30), px(40, 33), px(40, 29)[3], px(40, 34)[3]], [BLACK, BLACK, 0, 0])

    def test_a_hairline_is_a_pixel_wide(self):
        self.assertAlmostEqual(area([shape_body(1, (16 * T, 48 * T, 32 * T, 32 * T), [], [(1, BLACK)], self.LINE)]),
                               32 + math.pi / 4, delta=0.2)


class Movies(unittest.TestCase):
    def test_refuses_what_it_does_not_draw(self):
        gradient = shape_body(1, (0, 200, 0, 200), [RED], [], box_records(0, 0, 10, 10, f1=1), kinds={0: 0x10})
        with self.assertRaises(x.Unsupported):
            x.Movie(movie([gradient], {100: [place(1, 1)]}))
        good = box(1, 0, 0, 10, 10, RED)
        for placed in ([place(1, 1, flags=0x0E, extra=bytes(4))],  # a colour transform
                       [place(1, 1, flags=0x46, extra=bytes(2))],  # a clip layer
                       [place(1, 1, flags=0x05)],  # a move without a character
                       [place(1, 7)],  # nothing with that id
                       [place(1, 100)],  # a sprite in a sprite
                       [g.tag(70, bytes(8))]):  # PlaceObject3
            with self.assertRaises(x.Unsupported):
                x.render(x.Movie(movie([good], {100: placed})), 100)
        ok = movie([good], {100: [place(1, 1)]})
        for bad in (movie([good], {100: [place(1, 1)]}, b"CWS"), ok + b"\0", ok[:-1]):
            with self.assertRaises(ValueError):
                x.Movie(bad)

    def test_bounds_and_exports(self):
        m = x.Movie(movie([box(1, 2, 3, 12, 23, RED)], {100: [place(1, 1, 5 * T, -T)], 101: [place(1, 1)]}))
        self.assertEqual(m.exports, [(100, "s100"), (101, "s101")])
        self.assertEqual(m.bounds(100), (7 * T, 2 * T, 17 * T, 22 * T))
        self.assertEqual((m.signature, m.version, len(m.shapes), len(m.sprites)), ("GFX", 42, 1, 2))

    def test_our_own_replacement_movie(self):
        m = x.Movie(g.build())
        self.assertEqual(sorted(x.export_number(name) for _, name in m.exports), sorted(g.GLYPHS))
        self.assertEqual(m.label, "platform_xbsx_input_icons_Main")
        self.assertEqual(sorted(x.BUTTONS), sorted(g.GLYPHS))
        sid = next(sid for sid, name in m.exports if name.endswith("Main_Button_30"))
        w, h, rgba = x.render(m, sid, scale=1)
        self.assertEqual((w, h), (64, 64))
        self.assertEqual(tuple(rgba[4 * (64 + 32):4 * (64 + 32) + 4]), g.RIM)  # the rim of the disc, at (32, 1)
        self.assertEqual(rgba[3], 0)  # the corner: the sprite's invisible bounds rectangle draws nothing
        w, h, wide = x.render(m, sid, scale=1, margin=2)
        self.assertEqual((w, h), (68, 68))
        self.assertEqual(tuple(wide[4 * (3 * 68 + 34):4 * (3 * 68 + 34) + 4]), g.RIM)


class Output(unittest.TestCase):
    def test_png(self):
        rgba = bytes(range(96))
        self.assertEqual(unpng(x.png(4, 6, rgba)), (4, 6, rgba))

    def test_contact_sheet(self):
        red = bytes(RED) * 64
        half = bytes((255, 255, 255, 0)) * 64
        pictures = [("4", 8, 8, red), ("12", 8, 8, half), ("7", 8, 8, red)]
        w, h, sheet = x.contact_sheet(pictures, columns=2, pad=12, dot=3)
        self.assertEqual((w, h, len(sheet)), (2 * 20 + 12, 2 * (8 + 15 + 24) + 12, 4 * w * h))
        px = lambda px_, py_: tuple(sheet[4 * (py_ * w + px_):4 * (py_ * w + px_) + 4])
        self.assertEqual(px(12, 12), RED)
        self.assertIn(px(32, 12), {(70, 74, 84, 255), (88, 92, 102, 255)})  # nothing drawn: the checkerboard shows
        self.assertEqual(px(12, 12 + 47), RED)
        labels = [px(i, j) for j in range(12 + 8, 12 + 8 + 6 + 15) for i in range(w)]
        self.assertIn((236, 238, 244, 255), labels)
        self.assertTrue(all(p[3] == 255 for p in labels))

    def test_reads_a_stream_and_only_that(self):
        payload = movie([box(1, 0, 0, 10, 10, RED)], {100: [place(1, 1)]})
        packed = zlib.compress(payload, 9)
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "archive.bin")
            with open(path, "wb") as f:
                f.write(bytes(4096) + packed + bytes(100))
            self.assertEqual(x.read_stream(path, 4096), (payload, len(packed)))
            with self.assertRaises(ValueError):
                x.read_stream(path, 16)

    def test_writes_the_folder(self):
        data = g.build()
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, "glyphs")
            lines = []
            names = x.write_all(x.Movie(data), out, "a hand-made movie", scale=1, log=lines.append)
            self.assertEqual(sorted(os.listdir(out)), sorted(names + ["README.txt", "sheet.png"]))
            self.assertEqual(names, ["platform_xbsx.input_icons.Main_Button_%d.png" % n for n in sorted(g.GLYPHS)])
            self.assertEqual(len(lines), len(names))
            for name in names:
                with open(os.path.join(out, name), "rb") as f:
                    w, h, rgba = unpng(f.read())
                self.assertEqual((w, h), (66, 66))  # the 64 x 64 canvas and a pixel around it
                self.assertTrue(any(rgba[3::4]), name)
            with open(os.path.join(out, "sheet.png"), "rb") as f:
                w, h, _ = unpng(f.read())
            self.assertEqual((w, h), (8 * (66 + 12) + 12, 5 * (66 + 15 + 24) + 12))
            with open(os.path.join(out, "README.txt"), "rb") as f:
                text = f.read()
            self.assertNotIn(b"\r", text)
            text = text.decode("utf-8")
            self.assertIn("a hand-made movie", text)
            for name in names:
                self.assertIn(name + " | ", text)
            self.assertIn("Main_Button_30.png | A | ", text)
            self.assertNotIn("cut off", text)

    def test_never_writes_into_the_repository(self):
        inside = os.path.join(os.path.dirname(os.path.abspath(__file__)), "no_such_folder_for_glyphs")
        with self.assertRaises(ValueError):
            x.write_all(x.Movie(g.build()), inside, "a hand-made movie", scale=1)
        self.assertFalse(os.path.exists(inside))
        with tempfile.TemporaryDirectory() as tmp:
            self.assertFalse(x.inside_repository(os.path.join(tmp, "glyphs")))
        self.assertTrue(x.inside_repository(os.path.join(x.REPO, "tools")))


if __name__ == "__main__":
    unittest.main()
