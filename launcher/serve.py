#!/usr/bin/env python3
"""The Leonida Launcher's page and the small local server behind it.

    python -I launcher/serve.py                (Windows: Launcher.bat)
    python -I launcher/serve.py --no-window --no-run --port 8770     (look at the page; nothing is ever started)

Serves launcher/index.html on http://127.0.0.1:<port> (this machine only), opens it in a window of its own and does
what the page asks: read and save the settings, switch mods, list saves and captures, back saves up, and start and
stop the game through scripts/play-d3d12.ps1 (Linux: scripts/play-linux.sh). The settings, the command line and the
lists are the functions of tools/leonida_launcher.py, so this page and the Tkinter window agree.

Who may ask: only a page this server handed out. Requests must name the loopback address in Host (as the map
viewer's server checks), every /api request carries the token written into the page for this run, and the picture
and event streams carry it as a cookie. A page on another site can therefore neither read anything nor start
anything. Paths never come from the page: mods, captures and saves are named by folder, index or container name
and looked up in the server's own lists.

Standard library only. The one thing fetched from the internet is the game's key art for the background, once, when
there is no background picture yet (fetch_art; --no-art leaves it). It is kept in the launcher's settings folder,
never in the repository.
"""
from __future__ import annotations

import argparse
import collections
import glob
import hmac
import http.server
import importlib.util
import json
import os
import queue
import secrets
import shutil
import subprocess
import sys
import threading
import time
import urllib.request
import webbrowser

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
MAX_BODY = 64 * 1024
MAX_CAPTURES = 60
MAX_PICTURE = 40 * 1024 * 1024
PICTURE_NAMES = ("background.webp", "background.png", "background.jpg", "background.jpeg")
EXTENSIONS = {"image/png": ".png", "image/jpeg": ".jpg", "image/webp": ".webp"}
ART_URL = ("https://www.rockstargames.com/VI/_next/static/media/Jason_and_Lucia_Robbery_landscape.09c8a~do21h4p.jpg"
           "?akim=1&imdensity=1&imwidth=2560")
QUIET_SECONDS = 12.0     # with no page connected for this long, and nothing running, the server ends


def _load(name: str, path: str):
    """A module of this repository loaded by path (`python -I` leaves its folders off sys.path)."""
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules.setdefault(name, module)
    spec.loader.exec_module(module)
    return module


core = _load("leonida_launcher", os.path.join(REPO, "tools", "leonida_launcher.py"))
host_allowed = _load("leonida_viewer_serve", os.path.join(REPO, "viewer", "serve.py")).host_allowed


def _quiet_run(command: list[str], timeout: float = 5.0) -> str:
    kwargs: dict = {"creationflags": subprocess.CREATE_NO_WINDOW} if core.WINDOWS else {}
    try:
        done = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace",
                              timeout=timeout, check=False, stdin=subprocess.DEVNULL, **kwargs)
    except (OSError, subprocess.SubprocessError):
        return ""
    return done.stdout if done.returncode == 0 else ""


def repo_version(repo: str = REPO) -> str:
    return _quiet_run(["git", "-C", repo, "rev-parse", "--short", "HEAD"]).strip() or "(no git here)"


def package_version(repo: str = REPO) -> str:
    """The package the repository's manifest is for: game/xbox-<version>.json."""
    names = sorted(glob.glob(os.path.join(repo, "game", "xbox-*.json")))
    return os.path.basename(names[-1])[len("xbox-"):-len(".json")] if names else "?"


def pictures(folders: list[str], limit: int = MAX_CAPTURES) -> list[tuple[float, str]]:
    """The PNG files under `folders`, newest first, as (modified, path)."""
    found = []
    for folder in folders:
        for root, _dirs, files in os.walk(folder):
            for name in files:
                if name.lower().endswith(".png"):
                    path = os.path.join(root, name)
                    try:
                        found.append((os.path.getmtime(path), path))
                    except OSError:
                        continue
            if len(found) > 5000:
                break
    found.sort(reverse=True)
    return found[:limit]


def sniff(head: bytes) -> str | None:
    """image/png, image/jpeg or image/webp from a file's first 12 bytes; None for anything else."""
    if head.startswith(b"\x89PNG\r\n\x1a\n"):
        return "image/png"
    if head.startswith(b"\xff\xd8\xff"):
        return "image/jpeg"
    if head[:4] == b"RIFF" and head[8:12] == b"WEBP":
        return "image/webp"
    return None


def picture_type(path: str) -> str | None:
    """The type of a file with a picture's name that is one by its bytes; None for anything else, so that no other
    file can be served as the background."""
    if os.path.splitext(path)[1].lower() not in (".png", ".jpg", ".jpeg", ".webp"):
        return None
    try:
        if not 0 < os.path.getsize(path) <= MAX_PICTURE:
            return None
        with open(path, "rb") as f:
            return sniff(f.read(12))
    except OSError:
        return None


def background_picture(settings: dict, folder: str | None = None) -> str:
    """The picture behind PLAY: the one chosen in Settings, else the newest background.webp, .png or .jpg in the
    launcher's settings folder (%APPDATA%\\Leonida on Windows), where fetch_art keeps the key art; "" when there is
    none."""
    chosen = str(settings.get("background", "")).strip()
    if chosen:
        return chosen if os.path.isfile(chosen) and picture_type(chosen) else ""
    found = []
    for name in PICTURE_NAMES:
        path = os.path.join(folder or core.config_dir(), name)
        try:
            if os.path.isfile(path) and picture_type(path):
                found.append((os.path.getmtime(path), path))
        except OSError:
            continue
    return max(found)[1] if found else ""


def fetch_art(folder: str, url: str = ART_URL, timeout: float = 30.0) -> str:
    """Downloads the picture at `url` into `folder` as background.<its type> and returns that path. Raises OSError
    (no network, an HTTP error) or ValueError (not a PNG, JPEG or WebP, or too big); nothing is kept then. The request
    carries a plain browser's name and nothing about the user or this project."""
    request = urllib.request.Request(url, headers={
        "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64)",
        "Accept": "image/webp,image/jpeg,image/png;q=0.9,*/*;q=0.5"})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        data = response.read(MAX_PICTURE + 1)
    kind = sniff(data[:12])
    if kind is None or len(data) > MAX_PICTURE:
        raise ValueError("what came back is not a PNG, JPEG or WebP picture")
    os.makedirs(folder, exist_ok=True)
    path = os.path.join(folder, "background" + EXTENSIONS[kind])
    part = path + ".part"
    with open(part, "wb") as f:
        f.write(data)
    os.replace(part, path)
    return path


class Launcher:
    """What the page shows and what it can ask for. One job (the game, or a tool) runs at a time."""

    def __init__(self, settings_path: str | None = None, can_run: bool = True, repo: str = REPO):
        self.settings_path = settings_path
        self.can_run = can_run
        self.repo = repo
        self.lock = threading.RLock()
        self.process: subprocess.Popen | None = None
        self.kind = ""
        self.job_state = "Ready"
        self.log: collections.deque = collections.deque(["The launcher is ready."], maxlen=2000)
        self.listeners: list[queue.Queue] = []
        self.capture_paths: list[str] = []
        self.version = repo_version(repo)
        self.package = package_version(repo)
        self.last_seen = time.monotonic()

    # ---- what the page shows

    def settings(self) -> dict:
        return core.load_settings(self.settings_path)

    def mods_dir(self, settings: dict) -> str:
        return settings.get("mods_dir") or os.path.join(self.repo, "mods")

    def background(self, settings: dict) -> str:
        return background_picture(settings)

    def hero(self, settings: dict, captures: list[dict]) -> dict | None:
        """The picture behind PLAY: the background picture, else the newest capture, else None (the page draws one)."""
        path = self.background(settings)
        if path:
            try:
                stamp = os.path.getmtime(path)
            except OSError:
                stamp = 0.0
            return {"url": f"/media/background?v={int(stamp)}", "time": stamp, "kind": "picture"}
        if captures:
            return {"url": captures[0]["url"], "time": captures[0]["time"], "kind": "capture"}
        return None

    def ensure_art(self) -> None:
        """Fetches the key art once when no background picture is chosen or kept yet, so the window opens on it from
        then on. A failure leaves the newest capture or the drawn art in its place, and is tried again next start."""
        settings = self.settings()
        if str(settings.get("background", "")).strip() or self.background(settings):
            return
        try:
            path = fetch_art(core.config_dir())
        except (OSError, ValueError) as err:   # urllib's URLError and HTTPError are OSErrors
            self.say(f"The background picture could not be fetched ({err}); the drawn art stays for now.")
            return
        self.say(f"The background picture was fetched once and kept: {path}")
        self._send("state", None)

    def checks(self, settings: dict) -> list[dict]:
        rows = []
        game = settings["game_dir"].strip()
        if core.game_exe_present(game):
            rows.append({"id": "game", "state": "ok", "title": "Game folder", "detail": core.EXPECTED_EXE + " found"})
        else:
            rows.append({"id": "game", "state": "todo", "title": "Game folder", "go": "#settings", "action": "Choose",
                         "detail": f"No {core.EXPECTED_EXE} in this folder" if game else "Not chosen yet"})
        data = core.data_root(settings)
        rows.append({"id": "data", "state": "ok", "title": "Data folder",
                     "detail": data if os.path.isdir(data) else data + " (made at the first start)"})
        if core.WINDOWS:
            runtime = os.path.join(self.repo, "d3d12", "target", "diag", "leonida.exe")
            built = {"id": "runtime", "state": "ok", "title": "Runtime", "detail": "leonida.exe is built"}
            missing = {"id": "runtime", "state": "todo", "title": "Runtime", "go": "#tools", "action": "Build",
                       "detail": "Not built yet: the first start builds it (minutes)"}
            rows.append(built if os.path.isfile(runtime) else missing)
            dlss = os.path.join(self.repo, "d3d12", "nvidia-sdk", "DLSS", "lib", "Windows_x86_64", "rel",
                                "nvngx_dlss.dll")
            fetched = {"id": "dlss", "state": "ok", "title": "NVIDIA's DLSS files", "detail": "Fetched"}
            absent = {"id": "dlss", "state": "todo", "title": "NVIDIA's DLSS files", "go": "#tools", "action": "Fetch",
                      "detail": "Not fetched: about 70 MB from NVIDIA's GitHub"}
            rows.append(fetched if os.path.isfile(dlss) else absent)
        else:
            runtime = os.path.join(self.repo, "scripts", "leonida.exe")
            rows.append({"id": "runtime", "state": "ok" if os.path.isfile(runtime) else "todo", "title": "Runtime",
                         "detail": "leonida.exe is beside the start script" if os.path.isfile(runtime)
                         else "No scripts/leonida.exe: see docs/linux.md"})
            rows.append({"id": "wine", "state": "ok" if shutil.which("wine") else "todo", "title": "Wine",
                         "detail": "Found" if shutil.which("wine") else "Not found: your distribution's wine package"})
        return rows

    def saves(self, settings: dict) -> dict:
        saves = core._saves_module()
        root = saves.saves_root(core.data_root(settings))
        try:
            found = saves.scan(root)
        except OSError as err:
            return {"root": root, "containers": [], "error": f"The saves folder could not be read: {err}"}
        rows = [{"name": c.name or c.folder, "version": c.version, "size": c.size, "modified": c.modified,
                 "blobs": len(c.blobs), "problem": c.problem} for c in found]
        return {"root": root, "containers": rows}

    def state(self, with_log: bool = True) -> dict:
        settings = self.settings()
        try:
            command = core.play_command(settings, repo=self.repo)
        except ValueError:
            command = None
        mods_dir = self.mods_dir(settings)
        mods, errors = core.list_mods(mods_dir)
        shots = pictures(core.screenshot_dirs(self.repo))
        with self.lock:
            self.capture_paths = [path for _stamp, path in shots]
            job = {"running": self.running(), "state": self.job_state, "kind": self.kind}
            log = list(self.log) if with_log else []
        captures = [{"name": os.path.basename(path), "time": stamp, "url": f"/media/capture/{i}?v={int(stamp)}"}
                    for i, (stamp, path) in enumerate(shots)]
        return {
            "sample": False, "can_run": self.can_run, "platform": "windows" if core.WINDOWS else "linux",
            "version": self.version, "package": self.package, "settings": settings, "checks": self.checks(settings),
            "command": command,
            "mods": [{"folder": m["folder"], "name": m["name"], "enabled": m["enabled"], "swaps": m["swaps"]}
                     for m in mods],
            "mod_errors": errors, "mods_dir": mods_dir, "saves": self.saves(settings), "captures": captures,
            "hero": self.hero(settings, captures), "background": self.background(settings),
            "job": job, "log": log,
        }

    # ---- the stream of output and state

    def subscribe(self) -> queue.Queue:
        q: queue.Queue = queue.Queue(maxsize=4000)
        with self.lock:
            self.listeners.append(q)
        return q

    def unsubscribe(self, q: queue.Queue) -> None:
        with self.lock:
            if q in self.listeners:
                self.listeners.remove(q)
            self.last_seen = time.monotonic()

    def _send(self, event: str, data: object) -> None:
        with self.lock:
            listeners = list(self.listeners)
        for q in listeners:
            try:
                q.put_nowait((event, data))
            except queue.Full:
                pass    # a page that stopped reading; it gets the whole state when it asks again

    def say(self, *lines: str) -> None:
        with self.lock:
            self.log.extend(lines)
        self._send("log", list(lines))

    def set_state(self, text: str) -> None:
        with self.lock:
            changed = text != self.job_state
            self.job_state = text
        if changed:
            self._send("state", None)

    # ---- running things

    def running(self) -> bool:
        return self.process is not None and self.process.poll() is None

    def start(self, kind: str, command: list[str]) -> str:
        """Starts `command` as the one job. Returns the message for the page ("" when it started)."""
        shown = " ".join(command)
        with self.lock:
            if self.running():
                return "Something is already running. Stop it first, or wait for it."
            if not self.can_run:
                self.say("Preview (--no-run): nothing was started. This would run:", "  " + shown)
                return "Preview: nothing was started. The command is in the log on the Play page."
            try:
                self.process = core.spawn(command, cwd=self.repo)
            except OSError as err:
                self.say(f"Could not start: {err}")
                return f"Could not start: {err}"
            self.kind = kind
            self.job_state = "Starting"
        self.say("> " + shown)
        self._send("state", None)
        threading.Thread(target=self._follow, args=(self.process, kind), daemon=True).start()
        return ""

    def _follow(self, process: subprocess.Popen, kind: str) -> None:
        state = "Starting"
        for raw in process.stdout:
            line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
            self.say(line)
            if kind == "play":
                new = core.state_for_line(state, line + "\n")
                if new != state:
                    state = new
                    self.set_state(state)
        code = process.wait()
        self.say(f"Exited with code {code}.")
        with self.lock:
            self.kind = ""
        self.set_state("Ready" if code == 0 else f"Exited with code {code}")
        self._send("state", None)

    def play(self) -> str:
        settings = self.settings()
        if not core.game_exe_present(settings["game_dir"].strip()):
            return f"Choose the game folder in Settings first: the folder with {core.EXPECTED_EXE}."
        return self.start("play", core.play_command(settings, repo=self.repo))

    def stop(self) -> str:
        with self.lock:
            process = self.process if self.running() else None
        if process is None:
            return "Nothing is running."
        self.say("Stopping ...")
        core.stop_tree(process)
        return ""

    def tool(self, name: str) -> str:
        try:
            if name == "dry_run":
                settings = self.settings()
                if not core.WINDOWS:
                    raise ValueError("The dry run is a switch of the Windows start script.")
                command = core.play_command(settings, repo=self.repo) + ["-DryRun"]
            else:
                command = core.tool_command(name, repo=self.repo)
        except ValueError as err:
            return str(err)
        return self.start(name, command)

    # ---- the smaller requests

    def folder_of(self, what: str) -> str | None:
        settings = self.settings()
        data = core.data_root(settings)
        return {"logs": core.logs_dir(), "captures": core.captures_dir(), "mods": self.mods_dir(settings),
                "backups": core._saves_module().backups_dir(data)}.get(what)

    def open_folder(self, what: str) -> str:
        path = self.folder_of(what)
        if path is None:
            return "No such folder."
        if not self.can_run:
            return f"Preview: this would open {path}"
        try:
            core.open_folder(path)
        except OSError as err:
            return f"Could not open {path}: {err}"
        return ""

    def set_mod(self, folder: object, enabled: object) -> str:
        if not isinstance(folder, str) or not isinstance(enabled, bool):
            return "That is not a mod switch."
        mods, _errors = core.list_mods(self.mods_dir(self.settings()))
        for mod in mods:
            if mod["folder"] == folder:
                try:
                    core.set_mod_enabled(mod["path"], enabled)
                except (OSError, ValueError, core._mods_module().ModsError) as err:
                    return f"{mod['name']}: {err}"
                return ""
        return "That mod is not in the mods folder any more."

    def save_action(self, action: object, name: object) -> str:
        saves = core._saves_module()
        if action not in ("backup", "backup-all"):
            return "That is not something the Saves page does."
        if core.game_running():
            return "The game is running: close it first, so a save is not copied half written."
        data = core.data_root(self.settings())
        root = saves.saves_root(data)
        try:
            chosen = None
            if action == "backup":
                chosen = [c for c in saves.scan(root) if (c.name or c.folder) == name]
                if not chosen:
                    return "That save is not there any more."
            path = saves.backup(root, saves.backups_dir(data), chosen)
        except (OSError, saves.SaveError) as err:
            return str(err)
        return f"Backed up to {path}"

    def browse(self, key: object) -> tuple[str, str]:
        """A folder picker for one of the folder settings, or a file picker for the background picture.
        Returns (message, chosen path)."""
        if key not in ("game_dir", "data_dir", "mods_dir", "background"):
            return "That is not a folder setting.", ""
        if not self.can_run:
            return "Preview: the picker opens when the launcher is started from Launcher.bat.", ""
        if key == "background":
            ask = ("f.askopenfilename(parent=r, initialdir=sys.argv[1] or None, "
                   "filetypes=[('Pictures', '*.png *.jpg *.jpeg *.webp')])")
        else:
            ask = "f.askdirectory(parent=r, initialdir=sys.argv[1] or None, mustexist=True)"
        code = ("import sys, tkinter, tkinter.filedialog as f\n"
                "r = tkinter.Tk(); r.withdraw(); r.attributes('-topmost', True)\n"
                f"sys.stdout.buffer.write(({ask} or '').encode('utf-8'))\n")
        start = self.settings()[key]
        folder = os.path.dirname(start) if key == "background" else start
        try:    # a process of its own: Tk wants the main thread, and this is a request thread
            done = subprocess.run([sys.executable, "-I", "-c", code, folder if os.path.isdir(folder) else ""],
                                  capture_output=True, timeout=600, check=False, stdin=subprocess.DEVNULL)
        except (OSError, subprocess.SubprocessError):
            return "No folder picker here: type the path into the box.", ""
        if done.returncode != 0:
            return "No folder picker here (Tkinter is missing): type the path into the box.", ""
        return "", os.path.normpath(done.stdout.decode("utf-8", errors="replace")) if done.stdout else ""


class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "LeonidaLauncher"
    protocol_version = "HTTP/1.1"
    launcher: Launcher
    token = ""

    def log_message(self, *_args) -> None:   # the console stays for the start script's own output
        pass

    # ---- who is asking

    def _refuse(self, code: int, text: str) -> None:
        self._json(code, {"message": text})

    def _local(self) -> bool:
        hosts = self.headers.get_all("Host", [])
        if len(hosts) > 1 or not host_allowed(hosts[0] if hosts else None):
            self._refuse(403, "Open the launcher at http://127.0.0.1 or http://localhost.")
            return False
        origin = self.headers.get("Origin")
        if origin is not None:
            scheme, _sep, rest = origin.partition("://")
            if scheme != "http" or not host_allowed(rest):
                self._refuse(403, "This request came from another page.")
                return False
        return True

    def _has_token(self) -> bool:
        return hmac.compare_digest(self.headers.get("X-Leonida-Token", ""), self.token)

    def _has_cookie(self) -> bool:
        for part in self.headers.get("Cookie", "").split(";"):
            name, _eq, value = part.strip().partition("=")
            if name == "leonida" and hmac.compare_digest(value, self.token):
                return True
        return False

    # ---- answers

    def _head(self, code: int, kind: str, length: int | None, extra: dict | None = None) -> None:
        self.send_response(code)
        self.send_header("Content-Type", kind)
        if length is not None:
            self.send_header("Content-Length", str(length))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        for name, value in (extra or {}).items():
            self.send_header(name, value)
        self.end_headers()

    def _json(self, code: int, data: dict) -> None:
        body = json.dumps(data, ensure_ascii=False).encode("utf-8")
        self._head(code, "application/json; charset=utf-8", len(body))
        self.wfile.write(body)

    def _page(self) -> None:
        with open(os.path.join(HERE, "index.html"), encoding="utf-8") as f:
            text = f.read()
        body = text.replace('<meta name="leonida-token" content="">',
                            f'<meta name="leonida-token" content="{self.token}">', 1).encode("utf-8")
        self._head(200, "text/html; charset=utf-8", len(body), {
            "Set-Cookie": f"leonida={self.token}; HttpOnly; SameSite=Strict; Path=/",
            "Content-Security-Policy": "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
                                       "img-src 'self'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'",
        })
        self.wfile.write(body)

    def _logo(self) -> None:
        """The repository's own logo (assets/logo.svg), which the page shows in the rail and over PLAY."""
        try:
            with open(os.path.join(REPO, "assets", "logo.svg"), "rb") as f:
                body = f.read()
        except OSError:
            self._refuse(404, "No assets/logo.svg here.")
            return
        self._head(200, "image/svg+xml", len(body), {"Content-Security-Policy": "default-src 'none'; style-src 'unsafe-inline'"})
        self.wfile.write(body)

    def _background(self) -> None:
        path = self.launcher.background(self.launcher.settings())
        kind = picture_type(path) if path else None
        try:
            if not kind:
                raise OSError
            with open(path, "rb") as f:
                body = f.read(MAX_PICTURE + 1)
        except OSError:
            self._refuse(404, "No background picture.")
            return
        self._head(200, kind, len(body))
        self.wfile.write(body)

    def _capture(self, index_text: str) -> None:
        try:
            with self.launcher.lock:
                path = self.launcher.capture_paths[int(index_text)]
            with open(path, "rb") as f:
                body = f.read()
        except (ValueError, IndexError, OSError):
            self._refuse(404, "That picture is not there.")
            return
        self._head(200, "image/png", len(body))
        self.wfile.write(body)

    def _events(self) -> None:
        """Server-sent events: the script's output as it comes, and the state when it changes."""
        launcher = self.launcher
        q = launcher.subscribe()
        try:
            self._head(200, "text/event-stream; charset=utf-8", None, {"Connection": "close"})
            self.close_connection = True
            while True:
                try:
                    event, data = q.get(timeout=10)
                except queue.Empty:
                    self.wfile.write(b": still here\n\n")
                    self.wfile.flush()
                    continue
                if event == "log":
                    self._emit("log", data)
                else:
                    self._emit("state", launcher.state(with_log=False))
        except (OSError, ValueError):
            pass    # the page went away
        finally:
            launcher.unsubscribe(q)

    def _emit(self, event: str, data: object) -> None:
        self.wfile.write(f"event: {event}\ndata: {json.dumps(data, ensure_ascii=False)}\n\n".encode("utf-8"))
        self.wfile.flush()

    # ---- requests

    def do_GET(self) -> None:
        if not self._local():
            return
        path = self.path.split("?", 1)[0]
        if path in ("/", "/index.html"):
            self._page()
        elif path == "/assets/logo.svg":
            self._logo()
        elif path == "/api/state":
            if self._has_token():
                self._json(200, self.launcher.state())
            else:
                self._refuse(403, "Reload the launcher's page.")
        elif path == "/api/events":
            if self._has_cookie():
                self._events()
            else:
                self._refuse(403, "Reload the launcher's page.")
        elif path == "/media/background":
            if self._has_cookie():
                self._background()
            else:
                self._refuse(403, "Reload the launcher's page.")
        elif path.startswith("/media/capture/"):
            if self._has_cookie():
                self._capture(path[len("/media/capture/"):])
            else:
                self._refuse(403, "Reload the launcher's page.")
        else:
            self._refuse(404, "Nothing here.")

    def do_POST(self) -> None:
        if not self._local():
            return
        if not self._has_token():
            self._refuse(403, "Reload the launcher's page.")
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 <= length <= MAX_BODY:
                raise ValueError
            body = json.loads(self.rfile.read(length) or b"{}")
            if not isinstance(body, dict):
                raise ValueError
        except (ValueError, UnicodeDecodeError):
            self._refuse(400, "That request could not be read.")
            return
        launcher = self.launcher
        path = self.path.split("?", 1)[0]
        message = ""
        if path == "/api/settings":
            core.save_settings(body, launcher.settings_path)
        elif path == "/api/play":
            message = launcher.play()
        elif path == "/api/stop":
            message = launcher.stop()
        elif path == "/api/tool":
            message = launcher.tool(str(body.get("name", "")))
        elif path == "/api/open":
            message = launcher.open_folder(str(body.get("what", "")))
        elif path == "/api/mod":
            message = launcher.set_mod(body.get("folder"), body.get("enabled"))
        elif path == "/api/save":
            message = launcher.save_action(body.get("action"), body.get("name"))
        elif path == "/api/browse":
            message, chosen = launcher.browse(body.get("key"))
            if chosen:
                settings = launcher.settings()
                settings[body["key"]] = chosen
                core.save_settings(settings, launcher.settings_path)
        else:
            self._refuse(404, "Nothing here.")
            return
        self._json(200, {"message": message, "state": launcher.state()})


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = False


def open_window(url: str) -> None:
    """The page in a window of its own (a Chromium browser's --app mode), else in the default browser."""
    names = ["msedge", "chrome", "chromium", "chromium-browser", "google-chrome", "microsoft-edge", "brave"]
    found = [shutil.which(name) for name in names]
    if core.WINDOWS:
        for base in (os.environ.get("ProgramFiles(x86)"), os.environ.get("ProgramFiles"), os.environ.get("LOCALAPPDATA")):
            if base:
                found += [os.path.join(base, "Microsoft", "Edge", "Application", "msedge.exe"),
                          os.path.join(base, "Google", "Chrome", "Application", "chrome.exe")]
    for exe in found:
        if exe and os.path.isfile(exe):
            try:
                subprocess.Popen([exe, f"--app={url}", "--window-size=1360,860"], stdin=subprocess.DEVNULL,
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                return
            except OSError:
                continue
    webbrowser.open(url)


def port_number(text: str) -> int:
    port = int(text)
    if not 0 <= port <= 65535:
        raise argparse.ArgumentTypeError("a port is 0 to 65535")
    return port


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--port", type=port_number, default=0, help="the port on 127.0.0.1 (default: any free one)")
    ap.add_argument("--no-window", action="store_true", help="do not open the page; the address is printed")
    ap.add_argument("--no-run", action="store_true",
                    help="look only: PLAY, the tools and the folder buttons say what they would do and start nothing")
    ap.add_argument("--settings", help="another settings file than the user's launcher.json")
    ap.add_argument("--stay", action="store_true", help="keep serving after the page is closed")
    ap.add_argument("--repo", default=REPO,
                    help="the checkout whose start scripts and builds are meant (default: the one this file is in)")
    ap.add_argument("--no-art", action="store_true",
                    help="do not fetch the key art when there is no background picture yet")
    a = ap.parse_args()

    launcher = Launcher(settings_path=a.settings, can_run=not a.no_run, repo=os.path.abspath(a.repo))
    handler = type("LauncherHandler", (Handler,), {"launcher": launcher, "token": secrets.token_urlsafe(24)})
    try:
        server = Server(("127.0.0.1", a.port), handler)
    except OSError as err:
        print(f"launcher: port {a.port} on 127.0.0.1 is taken ({err}); try --port 0", file=sys.stderr)
        return 1
    url = f"http://127.0.0.1:{server.server_address[1]}/"
    print(f"launcher: {url}" + ("  (preview: nothing is started)" if a.no_run else ""), flush=True)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    if not a.no_art:
        threading.Thread(target=launcher.ensure_art, daemon=True).start()
    if not a.no_window:
        open_window(url)
    try:
        seen_a_page = False
        while True:
            time.sleep(1.0)
            with launcher.lock:
                connected = bool(launcher.listeners)
                idle = time.monotonic() - launcher.last_seen
                busy = launcher.running()
            seen_a_page = seen_a_page or connected
            if not a.stay and not a.no_window and seen_a_page and not connected and not busy and idle > QUIET_SECONDS:
                break       # the window was closed; a game started here is waited for, never ended
    except KeyboardInterrupt:
        pass
    server.shutdown()
    return 0


if __name__ == "__main__":
    sys.exit(main())
