"""tools/pc_prompt_glyphs.py: pictures made here are traced into shapes that draw back to the same pictures, lines
that repeat are defined once, and the movie written from them reads back as what was traced. No artwork and no
game file: every picture is drawn by the tool's own rasteriser or written pixel by pixel below."""
import math
import os
import struct
import sys
import unittest
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import pc_prompt_glyphs as pc  # noqa: E402
import ps_prompt_glyphs as swf  # noqa: E402

N = sorted(swf.GLYPHS)
PICTURES = pc.synthetic()


def filtered_png(w, h, rgba, kinds):
    """A PNG whose rows use the filter types `kinds` in turn (the tool's own writer uses none)."""
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    raw, prev = bytearray(), bytes(w * 4)
    for y in range(h):
        row, kind = rgba[y * w * 4:(y + 1) * w * 4], kinds[y % len(kinds)]
        out = bytearray([kind])
        for i, v in enumerate(row):
            a = row[i - 4] if i >= 4 else 0
            b, c = prev[i], prev[i - 4] if i >= 4 else 0
            p = a + b - c
            paeth = a if abs(p - a) <= abs(p - b) and abs(p - a) <= abs(p - c) else b if abs(p - b) <= abs(p - c) else c
            out.append((v - (0, a, b, (a + b) >> 1, paeth)[kind]) & 255)
        raw += out
        prev = row
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b""))


def points(contour):
    """The corners of a contour of straight edges."""
    start, segs = contour
    assert all(s[0] == "L" for s in segs), segs
    assert segs[-1][1] == start, "the contour closes"
    return [s[1] for s in segs]


def same_ring(a, b):
    """Whether two closed lists of points are one polygon, from any start."""
    return len(a) == len(b) and any(a[k:] + a[:k] == b for k in range(len(a)))


class Png(unittest.TestCase):
    def test_a_picture_survives_every_filter_type(self):
        w, h, rgba = PICTURES["key"]
        self.assertEqual(pc.read_png(pc.write_png(w, h, rgba)), (w, h, rgba))
        for kinds in ([1], [2], [3], [4], [0, 1, 2, 3, 4]):
            self.assertEqual(pc.read_png(filtered_png(w, h, rgba, kinds)), (w, h, rgba), kinds)

    def test_what_is_not_a_picture_it_reads_is_refused(self):
        with self.assertRaises(ValueError):
            pc.read_png(b"GIF89a" + bytes(32))
        sixteen_bit = struct.pack(">I4s", 13, b"IHDR") + struct.pack(">IIBBBBB", 1, 1, 16, 6, 0, 0, 0) + bytes(4)
        with self.assertRaises(ValueError):
            pc.read_png(b"\x89PNG\r\n\x1a\n" + sixteen_bit)


class Tracing(unittest.TestCase):
    def test_an_edge_is_placed_by_its_pixel_s_coverage(self):
        # A black box whose edges fall inside pixels: left at 20.25, right at 99.5, top at 30.75, bottom at 60 + 1/3.
        size = 264
        rows = []
        for y in range(size):
            cy = min(max(y + 1 - 30.75, 0.0), 1.0) - min(max(y + 1 - (60 + 1 / 3), 0.0), 1.0)
            row = bytearray()
            for x in range(size):
                cx = min(max(x + 1 - 20.25, 0.0), 1.0) - min(max(x + 1 - 99.5, 0.0), 1.0)
                row += bytes((0, 0, 0, round(255 * cx * cy)))
            rows.append(bytes(row))
        (piece,) = pc.pieces_of(size, size, b"".join(rows))
        self.assertEqual((piece["colour"], piece["depth"], piece["cuts"]), ("K", 0, []))
        for got, want in zip(pc.bounds(piece["pts"]), (20.25, 30.75, 99.5, 60 + 1 / 3)):
            self.assertAlmostEqual(got, want, delta=0.01)
        # Fitted: four straight edges through those places (in pixels here, no rounding to twips).
        corners = points(pc.fit_outline(piece["pts"], 0.12))
        self.assertEqual(len(corners), 4)
        for x, y in corners:
            self.assertAlmostEqual(min(abs(x - 20.25), abs(x - 99.5)), 0, delta=0.02)
            self.assertAlmostEqual(min(abs(y - 30.75), abs(y - (60 + 1 / 3))), 0, delta=0.02)

    def test_a_key_comes_back_corner_for_corner(self):
        # The picture: an outline with 3-unit chamfers at (4, 6), a face with 1-unit chamfers 3 inside it, a triangle.
        glyphs = pc.GlyphSet()
        glyphs.add(4, *PICTURES["key"])
        (k, (outline,)), (w, (face,)), (k2, (arrow,)) = glyphs.plan()[4]
        self.assertEqual((k, w, k2), ("K", "W", "K"))
        t = swf.TWIP
        at = lambda pts: [((4 + x) * t, (6 + y) * t) for x, y in pts]  # noqa: E731
        key = [(3, 0), (29, 0), (32, 3), (32, 29), (29, 32), (3, 32), (0, 29), (0, 3)]
        self.assertTrue(same_ring(points(outline), at(key)), points(outline))
        lit = [(4, 3), (28, 3), (29, 4), (29, 28), (28, 29), (4, 29), (3, 28), (3, 4)]
        self.assertTrue(same_ring(points(face), at(lit)), points(face))
        self.assertTrue(same_ring(points(arrow), at([(16, 9), (24, 22), (8, 22)])), points(arrow))
        # Every contour runs clockwise on screen: its inside is on the right, as fill style 1 wants.
        for contour in (outline, face, arrow):
            self.assertGreater(swf.area(points(contour)), 0)

    def test_a_curve_stays_within_the_tolerance(self):
        glyphs = pc.GlyphSet()
        glyphs.add(4, *PICTURES["bar"])
        (k, (disc,)), (g, (inner,)), (k2, (bar,)) = glyphs.plan()[4]
        self.assertEqual((k, g, k2), ("K", "G", "K"))
        for contour, radius in ((disc, 31), (inner, 28)):
            start, segs = contour
            self.assertTrue(all(s[0] == "Q" for s in segs) and 6 <= len(segs) <= 16, segs)
            here, worst = start, 0.0
            for _, control, end in segs:
                for k in range(9):
                    u = k / 8
                    x = (1 - u) ** 2 * here[0] + 2 * u * (1 - u) * control[0] + u * u * end[0]
                    y = (1 - u) ** 2 * here[1] + 2 * u * (1 - u) * control[1] + u * u * end[1]
                    worst = max(worst, abs(math.hypot(x - 32 * swf.TWIP, y - 32 * swf.TWIP) - radius * swf.TWIP))
                here = end
            # The tolerance, half a twip of rounding, and what the tracing itself is out by.
            self.assertLess(worst / swf.TWIP, pc.TOLERANCE + 0.025 + 0.03, (radius, worst))
        # The bar's straight sides are straight edges, its round ends curves.
        self.assertEqual(sum(1 for s in bar[1] if s[0] == "L"), 2)

    def test_a_hole_through_the_picture_is_cut_out_not_painted(self):
        w, h, rgba = PICTURES["ring"]
        glyphs = pc.GlyphSet()
        glyphs.add(4, w, h, rgba)
        (k, ring), (white, (dot,)) = glyphs.plan()[4]
        self.assertEqual((k, white, len(ring)), ("K", "W", 2))
        outer, hole = ring
        self.assertGreater(swf.area([s[-1] for s in outer[1]]), 0)
        self.assertLess(swf.area([s[-1] for s in hole[1]]), 0, "the hole runs the other way round")
        drawn = pc.draw([(pc.RGBA[c], cs) for c, cs in glyphs.plan()[4]], w, glyphs.scale)
        centre = (pc.MARGIN + 32) * glyphs.scale
        self.assertEqual(drawn.alpha[centre][centre], 0)
        self.assertEqual(drawn.alpha[centre][centre + 20 * glyphs.scale], 255)
        total, bad, worst, opaque = pc.difference(drawn, rgba)
        self.assertLess(total, 0.004 * opaque)
        self.assertEqual(bad, 0)

    def test_a_line_that_repeats_is_defined_once(self):
        glyphs = pc.GlyphSet()
        for n, name in ((4, "key"), (5, "key unlit"), (6, "key moved"), (7, "key")):
            glyphs.add(n, *PICTURES[name])
        # Three lines in all: the outline, the face, the triangle; the moved key is the same three elsewhere.
        self.assertEqual(len(glyphs.lib.items), 3)
        movie = swf.Movie(compact=True)
        placed = {n: glyphs.sprite(movie, n).items for n in (4, 5, 6, 7)}
        visible = [key for key in movie.order if key[1][3]]
        self.assertEqual(len(visible), 4, "outline, white face, grey face, triangle")
        self.assertEqual(placed[4], placed[7])
        by_id = lambda items: [i for i, _, _ in items]  # noqa: E731
        self.assertEqual(by_id(placed[4]), by_id(placed[6]), "the moved key places the same shapes")
        moved = {(x6 - x4, y6 - y4) for (_, x4, y4), (_, x6, y6) in zip(placed[4][1:], placed[6][1:])}
        self.assertEqual(moved, {(24 * swf.TWIP, 18 * swf.TWIP)})
        self.assertEqual(by_id(placed[4])[1], by_id(placed[5])[1], "lit or not, one outline")
        self.assertNotEqual(by_id(placed[4])[2], by_id(placed[5])[2], "the face is another colour")

    def test_pictures_must_be_the_canvas_at_a_whole_scale(self):
        glyphs = pc.GlyphSet()
        with self.assertRaises(ValueError):
            glyphs.add(4, 100, 100, bytes(100 * 100 * 4))
        glyphs.add(4, *PICTURES["key"])
        with self.assertRaises(ValueError):
            glyphs.add(5, 132, 132, bytes(132 * 132 * 4))


class Movie(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        names = sorted(PICTURES)
        cls.pictures = {n: PICTURES[names[i % len(names)]] for i, n in enumerate(N)}
        cls.glyphs = pc.trace(cls.pictures)

    def test_a_shape_reads_back_as_it_was_written(self):
        contours = [
            ((0, 0), [("L", (400, 0)), ("L", (400, 300)), ("Q", (410, 520), (200, 520)), ("L", (-3, 517)),
                      ("L", (0, 0))]),
            ((100, 100), [("L", (100, 200)), ("L", (33000, 200)), ("L", (100, 100))]),
        ]
        for compact in (False, True):
            body = bytes(2) + swf.contour_bytes(contours, (1, 2, 3, 255), compact)
            self.assertEqual(pc.read_shape(body), ((1, 2, 3, 255), contours))
        # A level or upright edge takes the short record, and an edge of no length none at all.
        def edge(end, compact):
            return len(swf.contour_bytes([((0, 0), [("L", end)])], (0, 0, 0, 255), compact))

        self.assertLess(edge((0, 250), True), edge((250, 250), True))
        self.assertLess(edge((0, 0), True), edge((0, 250), True))
        self.assertEqual(edge((250, 250), True), edge((250, 250), False))

    def test_the_standalone_movie_draws_back_to_the_pictures(self):
        data = pc.build(self.glyphs, pc.MOVIE_SIZE)
        self.assertEqual(data[:4], b"GFX*")
        self.assertEqual(struct.unpack_from("<I", data, 4)[0], len(data))
        self.assertEqual(len(data), pc.MOVIE_SIZE)
        placed = pc.read_movie(data)
        self.assertEqual(sorted(placed), N)
        plan = self.glyphs.plan()
        for n in N:
            w, h, rgba = self.pictures[n]
            # What the movie holds is what was traced, shape for shape (after an invisible rectangle, if any).
            seen = [(colour, contours) for colour, contours in placed[n] if colour[3]]
            traced = [(pc.RGBA[c], [(tuple(s), [tuple(e) for e in segs]) for s, segs in cs]) for c, cs in plan[n]]
            self.assertEqual(seen, traced, n)
            total, bad, worst, opaque = pc.difference(pc.draw(placed[n], w, self.glyphs.scale), rgba)
            self.assertLess(total, 0.004 * opaque, n)
            self.assertEqual(bad, 0, n)

    def test_a_sprite_is_never_smaller_than_the_glyph_it_stands_in_for(self):
        placed = pc.read_movie(pc.build(self.glyphs))
        for n in N:
            xs = [p[0] for _, cs in placed[n] for start, segs in cs for p in [start] + [q for s in segs for q in s[1:]]]
            ys = [p[1] for _, cs in placed[n] for start, segs in cs for p in [start] + [q for s in segs for q in s[1:]]]
            x0, y0, x1, y1 = swf.BOUNDS.get(n, (0, 0, swf.BOX, swf.BOX))
            self.assertTrue(min(xs) <= x0 + 2 and min(ys) <= y0 + 2 and max(xs) >= x1 - 2 and max(ys) >= y1 - 2, n)

    def test_built_over_another_movie_only_its_drawings_change(self):
        # Stand-in for the title's movie: the PlayStation tool's own, padded as the title's is long.
        base = swf.pad_movie(swf.build(), pc.MOVIE_SIZE)
        data = pc.build_over(base, self.glyphs)
        self.assertEqual(len(data), len(base))
        self.assertEqual(struct.unpack_from("<I", data, 4)[0], len(data))
        other = lambda movie: [(c, b) for c, b in swf.read_tags(movie)[0] if c not in (32, 39, 77)]  # noqa: E731
        self.assertEqual(other(data), other(base), "exports, ActionScript and class table are kept as they are")
        tags = swf.read_tags(data)[0]
        self.assertEqual(swf.exports_of(tags), swf.exports_of(swf.read_tags(base)[0]), "the sprites keep their ids")
        self.assertLessEqual(pc.used(data), pc.budget(base))
        placed = pc.read_movie(data)
        w, h, rgba = self.pictures[30]
        total, bad, worst, opaque = pc.difference(pc.draw(placed[30], w, self.glyphs.scale), rgba)
        self.assertLess(total, 0.004 * opaque)

    def test_a_set_that_does_not_fit_is_refused(self):
        # A movie with no drawings to give up: no shapes, and sprites that place nothing.
        full = swf.build()
        tags, start = swf.read_tags(full)
        bare = [swf.tag(code, body[:4] + swf.tag(0, b"") if code == 39 else body) for code, body in tags if code != 32]
        base = full[:4] + struct.pack("<I", start + sum(map(len, bare))) + full[8:start] + b"".join(bare)
        self.assertLess(pc.budget(base), 400)
        with self.assertRaises(ValueError):
            pc.build_over(base, self.glyphs)


if __name__ == "__main__":
    unittest.main()
