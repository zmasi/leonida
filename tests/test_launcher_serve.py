"""launcher/serve.py: who may ask, what is served, and the background picture (no browser, no internet, no game data).

    python -I tests/test_launcher_serve.py -v
"""
import functools
import http.client
import http.server
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import threading
import unittest

ROOT = Path(__file__).resolve().parents[1]
_spec = importlib.util.spec_from_file_location("leonida_launcher_serve", ROOT / "launcher" / "serve.py")
serve = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(serve)

PNG = b"\x89PNG\r\n\x1a\n" + b"\0" * 24
JPEG = b"\xff\xd8\xff\xe0" + b"\0" * 24
WEBP = b"RIFF\x10\0\0\0WEBPVP8 " + b"\0" * 16


class QuietFiles(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *_args):
        pass


class PictureTests(unittest.TestCase):
    def test_kinds_by_their_bytes(self):
        with tempfile.TemporaryDirectory() as tmp:
            def put(name, data):
                path = os.path.join(tmp, name)
                Path(path).write_bytes(data)
                return path
            self.assertEqual(serve.picture_type(put("a.png", PNG)), "image/png")
            self.assertEqual(serve.picture_type(put("b.jpg", JPEG)), "image/jpeg")
            self.assertEqual(serve.picture_type(put("c.webp", WEBP)), "image/webp")
            self.assertEqual(serve.picture_type(put("d.jpg", PNG)), "image/png")      # by its bytes
            self.assertIsNone(serve.picture_type(put("e.png", b"not a picture at all")))
            self.assertIsNone(serve.picture_type(put("f.txt", PNG)))                    # not a picture's name
            self.assertIsNone(serve.picture_type(os.path.join(tmp, "missing.png")))

    def test_which_background(self):
        with tempfile.TemporaryDirectory() as tmp:
            old, new = os.path.join(tmp, "background.webp"), os.path.join(tmp, "background.jpg")
            Path(old).write_bytes(WEBP)
            Path(new).write_bytes(JPEG)
            os.utime(old, (1000, 1000))
            os.utime(new, (2000, 2000))
            self.assertEqual(serve.background_picture({"background": ""}, tmp), new)    # the newest one
            chosen = os.path.join(tmp, "mine.png")
            Path(chosen).write_bytes(PNG)
            self.assertEqual(serve.background_picture({"background": chosen}, tmp), chosen)
            self.assertEqual(serve.background_picture({"background": os.path.join(tmp, "no.png")}, tmp), "")
            os.remove(old)
            os.remove(new)
            self.assertEqual(serve.background_picture({"background": ""}, tmp), "")


class FetchTests(unittest.TestCase):
    def files_at(self, folder):
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), functools.partial(QuietFiles, directory=folder))
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        return f"http://127.0.0.1:{server.server_address[1]}/"

    def test_kept_by_what_came_back(self):
        with tempfile.TemporaryDirectory() as src, tempfile.TemporaryDirectory() as dst:
            Path(src, "art.jpg").write_bytes(WEBP)
            path = serve.fetch_art(dst, self.files_at(src) + "art.jpg")
            self.assertEqual(path, os.path.join(dst, "background.webp"))
            self.assertEqual(Path(path).read_bytes(), WEBP)
            self.assertEqual(serve.background_picture({"background": ""}, dst), path)

    def test_anything_else_is_not_kept(self):
        with tempfile.TemporaryDirectory() as src, tempfile.TemporaryDirectory() as dst:
            Path(src, "page.html").write_bytes(b"<html>not a picture</html>")
            url = self.files_at(src)
            with self.assertRaises(ValueError):
                serve.fetch_art(dst, url + "page.html")
            with self.assertRaises(OSError):
                serve.fetch_art(dst, url + "missing.jpg")
            self.assertEqual(os.listdir(dst), [])


class ServerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.picture = os.path.join(cls.tmp.name, "mine.png")
        Path(cls.picture).write_bytes(PNG)
        settings = os.path.join(cls.tmp.name, "launcher.json")
        Path(settings).write_text(json.dumps({"data_dir": cls.tmp.name, "mods_dir": cls.tmp.name,
                                              "background": cls.picture}), encoding="utf-8")
        cls.launcher = serve.Launcher(settings_path=settings, can_run=False)
        cls.token = "t" * 32
        handler = type("Handler", (serve.Handler,), {"launcher": cls.launcher, "token": cls.token})
        cls.server = serve.Server(("127.0.0.1", 0), handler)
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.tmp.cleanup()

    def ask(self, method, path, headers=None, body=None):
        connection = http.client.HTTPConnection("127.0.0.1", self.server.server_address[1], timeout=20)
        connection.request(method, path, body=body, headers=headers or {})
        response = connection.getresponse()
        data = response.read()
        connection.close()
        return response.status, data

    def test_the_page_carries_the_token(self):
        status, data = self.ask("GET", "/")
        self.assertEqual(status, 200)
        self.assertIn(f'content="{self.token}"'.encode(), data)

    def test_the_api_needs_the_token(self):
        self.assertEqual(self.ask("GET", "/api/state")[0], 403)
        self.assertEqual(self.ask("GET", "/api/state", {"X-Leonida-Token": "wrong"})[0], 403)
        status, data = self.ask("GET", "/api/state", {"X-Leonida-Token": self.token})
        self.assertEqual(status, 200)
        state = json.loads(data)
        self.assertEqual(state["background"], self.picture)
        self.assertEqual(state["hero"]["kind"], "picture")

    def test_other_hosts_and_pages_are_refused(self):
        token = {"X-Leonida-Token": self.token}
        self.assertEqual(self.ask("GET", "/api/state", dict(token, Host="example.com"))[0], 403)
        self.assertEqual(self.ask("POST", "/api/play", dict(token, Origin="http://example.com"), b"{}")[0], 403)

    def test_pictures_need_the_cookie(self):
        self.assertEqual(self.ask("GET", "/media/background")[0], 403)
        self.assertEqual(self.ask("GET", "/media/background", {"Cookie": f"leonida={self.token}"}), (200, PNG))

    def test_preview_starts_nothing(self):
        status, data = self.ask("POST", "/api/tool", {"X-Leonida-Token": self.token}, json.dumps({"name": "build"}))
        self.assertEqual(status, 200)
        self.assertTrue(json.loads(data)["message"])
        self.assertIsNone(self.launcher.process)

    def test_no_fetch_with_a_picture(self):
        calls = []
        original = serve.fetch_art
        serve.fetch_art = lambda folder, *rest, **named: calls.append(folder) or ""
        try:
            self.launcher.ensure_art()
        finally:
            serve.fetch_art = original
        self.assertEqual(calls, [])


if __name__ == "__main__":
    unittest.main()
