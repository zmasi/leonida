"""tools/ps_prompt_glyphs.py: the generated movie is well formed and exports every symbol the title looks up."""
import os
import struct
import sys
import unittest
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import ps_prompt_glyphs as g  # noqa: E402

N = [*range(4, 40), 58, 59]


def tags(data, start, end):
    o = start
    while o < end:
        head = struct.unpack_from("<H", data, o)[0]
        code, length = head >> 6, head & 63
        o += 2
        if length == 63:
            length = struct.unpack_from("<I", data, o)[0]
            o += 4
        assert o + length <= end, "tag runs past its container"
        yield code, data[o:o + length]
        o += length
        if code == 0:
            assert o == end, "bytes after the End tag"
            return
    raise AssertionError("no End tag")


def u30(b, o):
    v = shift = 0
    while True:
        c = b[o]
        o += 1
        v |= (c & 0x7F) << shift
        shift += 7
        if not c & 0x80:
            return v, o


def skip_abc(b):
    """Walks the ABC up to its instance table; returns (class count, method count, offset)."""
    o = 4
    for _ in range(3):  # ints, uints, doubles (no entries: count 1)
        c, o = u30(b, o)
        assert c == 1
    c, o = u30(b, o)
    for _ in range(c - 1):
        n, o = u30(b, o)
        o += n
    c, o = u30(b, o)
    for _ in range(c - 1):
        o += 1
        _, o = u30(b, o)
    c, o = u30(b, o)
    assert c == 1
    c, o = u30(b, o)
    for _ in range(c - 1):
        assert b[o] == 7
        o += 1
        _, o = u30(b, o)
        _, o = u30(b, o)
    methods, o = u30(b, o)
    for _ in range(methods):
        for _ in range(3):
            _, o = u30(b, o)
        o += 1
    _, o = u30(b, o)  # metadata
    classes, o = u30(b, o)
    for _ in range(classes):
        _, o = u30(b, o)
        _, o = u30(b, o)
        o += 1
        for _ in range(3):  # interface count, iinit, trait count
            _, o = u30(b, o)
    return classes, methods, o


class Glyphs(unittest.TestCase):
    def setUp(self):
        self.data = g.build()

    def test_header(self):
        d = self.data
        self.assertEqual(d[:4], b"GFX*")
        self.assertEqual(struct.unpack_from("<I", d, 4)[0], len(d))

    def test_tags_and_exports(self):
        d = self.data
        nbits = d[8] >> 3
        start = 8 + (5 + 4 * nbits + 7) // 8 + 4
        seen = {}
        sprites, shapes, ids = set(), set(), set()
        for code, body in tags(d, start, len(d)):
            seen[code] = seen.get(code, 0) + 1
            if code == 32:
                shapes.add(struct.unpack_from("<H", body)[0])
            if code == 39:
                sid = struct.unpack_from("<H", body)[0]
                sprites.add(sid)
                inner = list(tags(body, 4, len(body)))
                self.assertEqual(inner[-1][0], 0)
                self.assertEqual(inner[-2][0], 1)
                for c, b in inner:
                    if c == 26:
                        self.assertIn(struct.unpack_from("<H", b, 3)[0], shapes)
            if code == 56:
                n = struct.unpack_from("<H", body)[0]
                o = 2
                names = {}
                for _ in range(n):
                    cid = struct.unpack_from("<H", body, o)[0]
                    e = body.index(b"\0", o + 2)
                    names[body[o + 2:e].decode()] = cid
                    o = e + 1
                self.assertEqual(o, len(body))
                self.assertEqual(sorted(names), sorted("platform_xbsx.input_icons.Main_Button_%d" % i for i in N))
                self.assertEqual(set(names.values()), sprites)
            if code == 76:
                n = struct.unpack_from("<H", body)[0]
                self.assertEqual(n, len(N) + 1)
            if code == 82:
                classes, methods, end = skip_abc(body[4 + len(b"frame1") + 1:])
                self.assertEqual(classes, len(N) + 1)
                self.assertEqual(methods, 2 * classes + 1)
        self.assertEqual(seen[32], len(shapes))
        self.assertEqual(seen[39], len(N))
        self.assertEqual(seen[0], 1)

    def test_fits_the_stored_size(self):
        self.assertLess(len(zlib.compress(self.data, 9)), 9644)
        self.assertEqual(zlib.decompress(zlib.compress(self.data, 9)), self.data)

    def test_deterministic(self):
        self.assertEqual(g.build(), self.data)

    def test_padding_to_a_size(self):
        for size in (len(self.data), len(self.data) + 2, len(self.data) + 40, len(self.data) + 64, len(self.data) + 65,
                     len(self.data) + 5000):
            d = g.pad_movie(self.data, size)
            self.assertEqual(len(d), size)
            self.assertEqual(struct.unpack_from("<I", d, 4)[0], size)
            nbits = d[8] >> 3
            start = 8 + (5 + 4 * nbits + 7) // 8 + 4
            self.assertEqual(list(tags(d, start, len(d)))[-1][0], 0)
        with self.assertRaises(ValueError):
            g.pad_movie(self.data, len(self.data) - 1)
        with self.assertRaises(ValueError):
            g.pad_movie(self.data, len(self.data) + 1)


class OverAMovie(unittest.TestCase):
    """build_over keeps everything of a movie but the drawings, on a stand-in made here (not the game's)."""

    MARKER = b"\x01\x00\x00\x00stand-in actionscript\x00\xaa\xbb\xcc"

    def standin(self):
        data = g.build()
        nbits = data[8] >> 3
        start = 8 + (5 + 4 * nbits + 7) // 8 + 4
        out = []
        for code, body in tags(data, start, len(data)):
            if code == 82:
                body = self.MARKER
            if code == 32:
                # Another shape set: the ids sit above ours so that the new ones must go beyond them.
                body = struct.pack("<H", struct.unpack_from("<H", body)[0] + 40) + body[2:]
            out.append(g.tag(code, body))
        out.insert(5, g.tag(32, struct.pack("<H", 900) + b"\0" * 20))
        base = data[:8] + data[8:start] + b"".join(out)
        return g.pad_movie(base, 20000)

    def test_keeps_the_scripts_and_the_tables_and_the_length(self):
        base = self.standin()
        # The stand-in's sprites refer to shapes it renumbered; only their tag layout matters here.
        mine = g.build_over(base)
        self.assertEqual(len(mine), len(base))
        self.assertEqual(struct.unpack_from("<I", mine, 4)[0], len(base))
        b, start = g.read_tags(base)
        m, mstart = g.read_tags(mine)
        self.assertEqual(start, mstart)
        for code in (82, 56, 76, 1000, 69, 43):
            self.assertEqual([x for x in b if x[0] == code], [x for x in m if x[0] == code], code)
        self.assertEqual([x[1] for x in m if x[0] == 82], [self.MARKER])
        self.assertEqual(m[-1][0], 0)
        # Same sprite ids, drawn by shapes of ids above everything the stand-in had.
        old_max = max(struct.unpack_from("<H", x[1])[0] for x in b if x[0] in (32, 39))
        ids = [struct.unpack_from("<H", x[1])[0] for x in m if x[0] == 39]
        self.assertEqual(ids, [struct.unpack_from("<H", x[1])[0] for x in b if x[0] == 39])
        shapes = [struct.unpack_from("<H", x[1])[0] for x in m if x[0] == 32]
        self.assertTrue(shapes and min(shapes) > old_max and len(set(shapes)) == len(shapes))
        first_sprite = next(i for i, x in enumerate(m) if x[0] == 39)
        self.assertTrue(all(i < first_sprite for i, x in enumerate(m) if x[0] == 32), "shapes come first")
        placed = {struct.unpack_from("<H", bd, 3)[0] for x in m if x[0] == 39 for c, bd in tags(x[1], 4, len(x[1])) if c == 26}
        self.assertTrue(placed <= set(shapes))

    def test_refuses_what_it_cannot_draw(self):
        with self.assertRaises(ValueError):
            g.build_over(b"CWS\x09" + bytes(20))
        data = self.standin()
        tags_, _ = g.read_tags(data)
        self.assertEqual(g.exports_of(tags_), {cid: n for cid, n in g.exports_of(tags_).items()})
        self.assertEqual(sorted(g.exports_of(tags_).values()), N)


if __name__ == "__main__":
    unittest.main()
