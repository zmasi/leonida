#!/usr/bin/env python3
"""Leonida Launcher: a desktop window to start the port, set its options and look after it.

Pages: Play (the PLAY button, the launcher's output and the state), Settings (game and data folders, the switches of
scripts/play-d3d12.ps1), Mods (the enabled switches of mods/*/mod.json), Tools (fetch NVIDIA's DLSS files, rebuild the
runtime, open folders) and About. Settings are kept in a JSON file under the user's config folder
(%APPDATA%\\Leonida\\launcher.json, ~/.config/leonida/launcher.json).

Standard library only (Tkinter for the window). It is Leonida's own design: no artwork of the game, no company logos;
the hero picture is the newest screenshot the user has made, read at run time, else a generated gradient.

    python -I tools/leonida_launcher.py           (Windows: Launcher.bat)
    python3 tools/leonida_launcher.py             (Linux)

The pure parts (settings, the command line, the mod list) have no Tkinter in them and are tested without a display
(tests/test_leonida_launcher.py).
"""
from __future__ import annotations

import importlib.util
import json
import os
import queue
import shutil
import signal
import subprocess
import sys
import threading
import time

EXPECTED_EXE = "game_scarlett_master.exe"
WINDOWS = os.name == "nt"
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Switches of scripts/play-d3d12.ps1 that the Settings page offers, with the value they have by default.
RESOLUTIONS = ("", "2160", "1440", "1080", "720", "540", "480")
DEFAULTS: dict = {
    "game_dir": "",
    "data_dir": "",
    "mods_dir": "",
    "dlss": True,            # off = -NoDlss
    "ray_tracing": False,    # -RayTracing
    "smooth_motion": False,  # -SmoothMotion
    "precompile": True,      # off = -NoPrecompile
    "series_x": False,       # -SeriesX
    "quality": False,        # -Quality
    "resolution": "",        # -Resolution <height>, empty = the default path
    "capture": False,        # -Capture
    "log": False,            # -Log
}


# ---------------------------------------------------------------- settings

def config_dir() -> str:
    if WINDOWS:
        base = os.environ.get("APPDATA") or os.path.join(os.path.expanduser("~"), "AppData", "Roaming")
    else:
        base = os.environ.get("XDG_CONFIG_HOME") or os.path.join(os.path.expanduser("~"), ".config")
    return os.path.join(base, "Leonida")


def config_path() -> str:
    return os.path.join(config_dir(), "launcher.json")


def normalise(data: object) -> dict:
    """The defaults with every valid value of `data` laid over them; anything of the wrong type is dropped."""
    settings = dict(DEFAULTS)
    if not isinstance(data, dict):
        return settings
    for key, default in DEFAULTS.items():
        value = data.get(key, default)
        if isinstance(default, bool):
            settings[key] = value if isinstance(value, bool) else default
        elif key == "resolution":
            settings[key] = value if value in RESOLUTIONS else default
        else:
            settings[key] = value if isinstance(value, str) else default
    return settings


def load_settings(path: str | None = None) -> dict:
    try:
        with open(path or config_path(), encoding="utf-8") as f:
            return normalise(json.load(f))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError):
        return normalise(None)


def save_settings(settings: dict, path: str | None = None) -> None:
    path = path or config_path()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(normalise(settings), f, indent=2, ensure_ascii=False)
        f.write("\n")


def game_exe_present(folder: str) -> bool:
    return bool(folder) and os.path.isfile(os.path.join(folder, EXPECTED_EXE))


def mods_dir_of(settings: dict) -> str:
    return settings.get("mods_dir") or os.path.join(REPO, "mods")


# ---------------------------------------------------------------- the command line

def switch_args(settings: dict) -> list[str]:
    """The PowerShell switches the options stand for (scripts/play-d3d12.ps1)."""
    args: list[str] = []
    if not settings["dlss"]:
        args.append("-NoDlss")
    if settings["ray_tracing"]:
        args.append("-RayTracing")
    if settings["smooth_motion"]:
        args.append("-SmoothMotion")
    if not settings["precompile"]:
        args.append("-NoPrecompile")
    if settings["series_x"]:
        args.append("-SeriesX")
    if settings["quality"]:
        args.append("-Quality")
    if settings["resolution"]:
        args += ["-Resolution", settings["resolution"]]
    if settings["capture"]:
        args.append("-Capture")
    if settings["log"]:
        args.append("-Log")
    return args


def play_command(settings: dict, repo: str = REPO, windows: bool = WINDOWS) -> list[str]:
    """The command PLAY runs. Raises ValueError when the game folder is not set."""
    game = settings["game_dir"].strip()
    if not game:
        raise ValueError("Choose the game folder in Settings first.")
    if windows:
        command = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                   os.path.join(repo, "scripts", "play-d3d12.ps1"), "-Game", game]
        if settings["data_dir"].strip():
            command += ["-Data", settings["data_dir"].strip()]
        return command + switch_args(settings)
    # scripts/play-linux.sh takes the executable and --series-x; the other options are Windows launcher switches.
    command = ["sh", os.path.join(repo, "scripts", "play-linux.sh"), os.path.join(game, EXPECTED_EXE)]
    if settings["series_x"]:
        command.append("--series-x")
    return command


def tool_command(name: str, repo: str = REPO, windows: bool = WINDOWS) -> list[str]:
    """The command of a Tools button: `get_dlss` or `build`. Both are Windows scripts."""
    if not windows:
        raise ValueError("This tool is a Windows script; on Linux see docs/linux.md.")
    runtime = os.path.join(repo, "d3d12")
    if name == "get_dlss":
        return ["cmd", "/c", os.path.join(runtime, "Get-NVIDIA-DLSS.cmd")]
    if name == "build":
        return ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                os.path.join(runtime, "scripts", "build-nvidia.ps1")]
    raise ValueError(f"unknown tool {name}")


def state_for_line(state: str, line: str) -> str:
    """The state shown after a line of output (a heuristic over the scripts' own messages)."""
    text = line.strip()
    if "Building the layer" in text or text.startswith("Compiling "):
        return "Building the runtime"
    if "Precompiling" in text:
        return "Preparing shaders"
    if state == "Preparing shaders" and text and not line.startswith(" "):
        return "Running"
    if text.startswith("Series ") and "F1 in the game" in text:
        return "Running" if state != "Starting" else state
    return state


# ---------------------------------------------------------------- folders

def local_data_dir() -> str:
    if WINDOWS:
        return os.environ.get("LOCALAPPDATA") or os.path.join(os.path.expanduser("~"), "AppData", "Local")
    return os.environ.get("XDG_DATA_HOME") or os.path.join(os.path.expanduser("~"), ".local", "share")


def logs_dir() -> str:
    return os.path.join(local_data_dir(), "Leonida", "logs")


def captures_dir() -> str:
    return os.path.join(local_data_dir(), "Leonida", "captures")


def screenshot_dirs(repo: str = REPO) -> list[str]:
    """Where the runtime saves pictures: the F4 menu's screenshots (next to the exe) and F9 captures."""
    return [os.path.join(repo, "d3d12", "target", "diag", "screenshots"), captures_dir()]


def newest_picture(folders: list[str], limit: int = 2000) -> str | None:
    """The newest PNG under `folders` (read at run time; nothing is copied anywhere)."""
    best, best_time, seen = None, -1.0, 0
    for folder in folders:
        for root, _dirs, files in os.walk(folder):
            for name in files:
                if not name.lower().endswith(".png"):
                    continue
                seen += 1
                if seen > limit:
                    return best
                path = os.path.join(root, name)
                try:
                    stamp = os.path.getmtime(path)
                except OSError:
                    continue
                if stamp > best_time:
                    best, best_time = path, stamp
    return best


# ---------------------------------------------------------------- mods

def _mods_module():
    """tools/leonida_mods.py, loaded by path (`python -I` leaves this folder off sys.path)."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "leonida_mods.py")
    spec = importlib.util.spec_from_file_location("leonida_mods", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules.setdefault("leonida_mods", module)
    spec.loader.exec_module(module)
    return module


def list_mods(mods_dir: str) -> tuple[list[dict], list[str]]:
    """Every mods/*/mod.json as {folder, name, enabled, swaps, path}, and the errors of the ones that did not parse."""
    mods_module = _mods_module()
    mods, errors = mods_module.load_mods(mods_dir)
    rows = [{"folder": m.folder, "name": m.name, "enabled": m.enabled, "swaps": len(m.swaps),
             "path": os.path.join(mods_dir, m.folder, mods_module.MANIFEST)} for m in mods]
    return rows, errors


def set_mod_enabled(manifest_path: str, enabled: bool) -> None:
    """Writes `enabled` into the manifest the way the mod manager does (mods.ModsError on failure)."""
    _mods_module().set_enabled_in_manifest(manifest_path, enabled)


# ---------------------------------------------------------------- saves

def _saves_module():
    """tools/leonida_saves.py, loaded by path like the mod manager."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "leonida_saves.py")
    spec = importlib.util.spec_from_file_location("leonida_saves", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules.setdefault("leonida_saves", module)
    spec.loader.exec_module(module)
    return module


def data_root(settings: dict) -> str:
    """The data folder in use: the setting, else the default of scripts/play-d3d12.ps1 (<local>/Leonida/d3d12)."""
    return settings.get("data_dir", "").strip() or os.path.join(local_data_dir(), "Leonida", "d3d12")


def game_running(windows: bool = WINDOWS) -> bool:
    """Whether a runtime process (leonida.exe or its Smooth Motion copy) is running, started here or not."""
    try:
        if windows:
            out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True,
                                 creationflags=subprocess.CREATE_NO_WINDOW, check=False).stdout.lower()
            return '"leonida.exe"' in out or '"leonida_sm.exe"' in out
        return subprocess.run(["pgrep", "-f", "leonida(_sm)?[.]exe"], capture_output=True, check=False).returncode == 0
    except OSError:
        return False


# ---------------------------------------------------------------- processes

def stop_tree(process: subprocess.Popen) -> None:
    """Ends the process and everything it started."""
    if process.poll() is not None:
        return
    if WINDOWS:
        subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"], capture_output=True, check=False)
    else:
        try:
            os.killpg(os.getpgid(process.pid), signal.SIGTERM)
        except OSError:
            process.terminate()


def spawn(command: list[str], cwd: str) -> subprocess.Popen:
    kwargs: dict = {}
    if WINDOWS:
        kwargs["creationflags"] = subprocess.CREATE_NO_WINDOW
    else:
        kwargs["start_new_session"] = True
    return subprocess.Popen(command, cwd=cwd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, **kwargs)


def open_folder(path: str) -> None:
    os.makedirs(path, exist_ok=True)
    if WINDOWS:
        os.startfile(path)  # noqa: S606 (a folder the user asked to see)
    elif sys.platform == "darwin":
        subprocess.Popen(["open", path])
    else:
        subprocess.Popen(["xdg-open", path])


# ---------------------------------------------------------------- the window

BG = "#0e1116"
PANEL = "#151a22"
NAV = "#0a0d12"
LINE = "#232b36"
TEXT = "#e6e9ee"
MUTED = "#8a94a3"
ACCENT = "#22c3b0"
ACCENT_DARK = "#159a8b"
DANGER = "#d9534f"

ABOUT = """Leonida is a compatibility runtime that runs your own copy of the Xbox Series build of GTA VI on a PC: a \
loader that maps the game's executable and a Direct3D 12 layer that draws its frames on your graphics card. It is an \
unofficial, independent project. It is not made, endorsed or supported by the game's publisher, and it contains none of \
the game: you build it from a copy you own, and nothing goes online.

Built on the work of others (full credits in d3d12/README.md):
  - VIctory by Marvisak and its contributors: the loader, the runtime services, the Direct3D 12 back end, the shader \
recompiler
  - avatarmanz/VIctory-7900XTX, the fork this one started from
  - fxceless/victory-fork, DexNom/VIctory, Belobug/VIctory-Belobug-AMD, FQY55kai/ZJOU-VICTORY, myokex/victory-amd, \
and the people named in d3d12/README.md
  - NVIDIA's DLSS and Streamline, which you fetch yourself under NVIDIA's own licences

Licence: GPL-3.0-or-later (the LICENSE file in the repository). The launcher is part of it."""


def run_gui() -> int:
    try:
        import tkinter as tk
        from tkinter import filedialog, messagebox, ttk
    except ImportError:
        print("Tkinter is not installed; on Windows install Python from python.org with Tcl/Tk, "
              "on Linux the python3-tk package.", file=sys.stderr)
        return 1

    class Launcher:
        PAGES = ("Play", "Settings", "Mods", "Saves", "Tools", "About")

        def __init__(self, root):
            self.root = root
            self.settings = load_settings()
            self.process: subprocess.Popen | None = None
            self.what = ""
            self.lines: queue.Queue = queue.Queue()
            self.state = "Ready"
            self.hero_image = None
            self.vars: dict = {}
            self.current = ""
            self.nav_buttons: dict = {}
            root.title("Leonida Launcher")
            root.configure(bg=BG)
            root.geometry("1100x700")
            root.minsize(900, 600)
            root.protocol("WM_DELETE_WINDOW", self.close)
            self.style()
            root.columnconfigure(1, weight=1)
            root.rowconfigure(0, weight=1)
            self.build_nav()
            self.content = tk.Frame(root, bg=BG)
            self.content.grid(row=0, column=1, sticky="nsew")
            self.content.columnconfigure(0, weight=1)
            self.content.rowconfigure(0, weight=1)
            self.pages = {name: tk.Frame(self.content, bg=BG) for name in self.PAGES}
            for page in self.pages.values():
                page.grid(row=0, column=0, sticky="nsew")
            self.build_play()
            self.build_settings()
            self.build_mods()
            self.build_saves()
            self.build_tools()
            self.build_about()
            self.show("Play")
            self.pump()

        # ---- look
        def style(self):
            s = ttk.Style(self.root)
            try:
                s.theme_use("clam")
            except tk.TclError:
                pass
            s.configure(".", background=BG, foreground=TEXT, fieldbackground=PANEL, bordercolor=LINE,
                        lightcolor=PANEL, darkcolor=PANEL, troughcolor=PANEL, font=("Segoe UI", 10))
            s.configure("TFrame", background=BG)
            s.configure("TLabel", background=BG, foreground=TEXT)
            s.configure("Muted.TLabel", foreground=MUTED)
            s.configure("TCheckbutton", background=BG, foreground=TEXT)
            s.map("TCheckbutton", background=[("active", BG)], foreground=[("disabled", MUTED)])
            s.configure("TButton", background=PANEL, foreground=TEXT, padding=(14, 7), borderwidth=1)
            s.map("TButton", background=[("active", LINE), ("disabled", NAV)], foreground=[("disabled", MUTED)])
            s.configure("TEntry", insertcolor=TEXT, padding=5)
            s.configure("TCombobox", arrowcolor=TEXT, padding=4)
            s.map("TCombobox", fieldbackground=[("readonly", PANEL)], foreground=[("readonly", TEXT)])
            s.configure("Treeview", background=PANEL, fieldbackground=PANEL, foreground=TEXT, rowheight=28,
                        borderwidth=0)
            s.configure("Treeview.Heading", background=NAV, foreground=MUTED, relief="flat")
            s.map("Treeview", background=[("selected", ACCENT_DARK)])
            s.configure("Play.TButton", background=ACCENT, foreground="#04201c", font=("Segoe UI", 18, "bold"),
                        padding=(52, 14))
            s.map("Play.TButton", background=[("active", "#3ad8c5"), ("disabled", LINE)],
                  foreground=[("disabled", MUTED)])
            s.configure("Stop.TButton", background=DANGER, foreground="white", font=("Segoe UI", 12, "bold"),
                        padding=(24, 10))
            s.map("Stop.TButton", background=[("active", "#e2706c")])

        def build_nav(self):
            nav = tk.Frame(self.root, bg=NAV, width=190)
            nav.grid(row=0, column=0, sticky="ns")
            nav.grid_propagate(False)
            tk.Label(nav, text="LEONIDA", bg=NAV, fg=ACCENT, font=("Segoe UI", 17, "bold"),
                     anchor="w").pack(fill="x", padx=22, pady=(26, 0))
            tk.Label(nav, text="Launcher", bg=NAV, fg=MUTED, font=("Segoe UI", 10),
                     anchor="w").pack(fill="x", padx=22, pady=(0, 24))
            for name in self.PAGES:
                label = tk.Label(nav, text=name, bg=NAV, fg=MUTED, font=("Segoe UI", 12), anchor="w",
                                 padx=22, pady=11, cursor="hand2")
                label.pack(fill="x")
                label.bind("<Button-1>", lambda _e, n=name: self.show(n))
                label.bind("<Enter>", lambda _e, n=name: self.nav_hover(n, True))
                label.bind("<Leave>", lambda _e, n=name: self.nav_hover(n, False))
                self.nav_buttons[name] = label
            tk.Label(nav, text="Unofficial. Runs your own copy.", bg=NAV, fg=MUTED, font=("Segoe UI", 8),
                     anchor="w", wraplength=160, justify="left").pack(side="bottom", fill="x", padx=22, pady=16)

        def nav_hover(self, name, inside):
            if name != self.current:
                self.nav_buttons[name].configure(bg=PANEL if inside else NAV)

        def show(self, name):
            self.current = name
            self.pages[name].tkraise()
            for key, label in self.nav_buttons.items():
                label.configure(bg=PANEL if key == name else NAV, fg=TEXT if key == name else MUTED)
            if name == "Mods":
                self.refresh_mods()
            if name == "Saves":
                self.refresh_saves()
            if name == "Play":
                self.refresh_hero()

        # ---- Play
        def build_play(self):
            page = self.pages["Play"]
            page.columnconfigure(0, weight=1)
            page.rowconfigure(1, weight=1)
            self.hero = tk.Canvas(page, height=300, bg=NAV, highlightthickness=0)
            self.hero.grid(row=0, column=0, sticky="ew")
            self.hero.bind("<Configure>", lambda _e: self.draw_hero())
            self.play_button = ttk.Button(page, text="PLAY", style="Play.TButton", command=self.play)
            self.stop_button = ttk.Button(page, text="Stop", style="Stop.TButton", command=self.stop)
            self.hero_window = self.hero.create_window(40, 250, window=self.play_button, anchor="sw")
            self.status_var = tk.StringVar(value="Ready")
            bar = tk.Frame(page, bg=PANEL)
            bar.grid(row=2, column=0, sticky="ew")
            tk.Label(bar, textvariable=self.status_var, bg=PANEL, fg=TEXT, font=("Segoe UI", 10, "bold"),
                     anchor="w").pack(side="left", padx=18, pady=9)
            self.hint_var = tk.StringVar()
            tk.Label(bar, textvariable=self.hint_var, bg=PANEL, fg=MUTED, anchor="e").pack(side="right", padx=18)
            box = tk.Frame(page, bg=BG)
            box.grid(row=1, column=0, sticky="nsew", padx=24, pady=(16, 12))
            box.columnconfigure(0, weight=1)
            box.rowconfigure(1, weight=1)
            ttk.Label(box, text="Output", style="Muted.TLabel").grid(row=0, column=0, sticky="w")
            self.log = tk.Text(box, bg=NAV, fg=TEXT, insertbackground=TEXT, relief="flat", wrap="word",
                               font=("Consolas" if WINDOWS else "monospace", 9), state="disabled", height=8,
                               highlightthickness=1, highlightbackground=LINE)
            scroll = ttk.Scrollbar(box, orient="vertical", command=self.log.yview)
            self.log.configure(yscrollcommand=scroll.set)
            self.log.grid(row=1, column=0, sticky="nsew", pady=(4, 0))
            scroll.grid(row=1, column=1, sticky="ns", pady=(4, 0))
            self.refresh_hint()

        def refresh_hero(self):
            self.hero_image = None
            path = newest_picture(screenshot_dirs())
            if path:
                try:
                    image = tk.PhotoImage(file=path)
                    factor = max(1, image.width() // max(1, self.hero.winfo_width() or 1100))
                    self.hero_image = image.subsample(factor, factor) if factor > 1 else image
                except tk.TclError:
                    self.hero_image = None
            self.draw_hero()

        def draw_hero(self):
            canvas = self.hero
            canvas.delete("art")
            width = max(canvas.winfo_width(), 200)
            height = int(canvas["height"])
            if self.hero_image is not None:
                canvas.create_image(width // 2, height // 2, image=self.hero_image, tags="art")
                canvas.create_rectangle(0, 0, width, height, fill=NAV, stipple="gray50", outline="", tags="art")
            else:
                top, bottom = (0x0a, 0x2a, 0x33), (0x0e, 0x11, 0x16)
                for y in range(0, height, 3):
                    t = y / height
                    colour = "#%02x%02x%02x" % tuple(int(a + (b - a) * t) for a, b in zip(top, bottom))
                    canvas.create_rectangle(0, y, width, y + 3, fill=colour, outline="", tags="art")
            canvas.create_text(40, 56, text="GTA VI", fill=TEXT, anchor="w", font=("Segoe UI", 44, "bold"),
                               tags="art")
            canvas.create_text(42, 112, text="on Leonida, an unofficial compatibility runtime", fill=MUTED,
                               anchor="w", font=("Segoe UI", 12), tags="art")
            canvas.tag_lower("art")

        def refresh_hint(self):
            if self.process is not None:
                self.hint_var.set("")
            elif not game_exe_present(self.settings["game_dir"]):
                self.hint_var.set("Choose your game folder in Settings")
            else:
                self.hint_var.set(self.settings["game_dir"])

        def append(self, text):
            self.log.configure(state="normal")
            self.log.insert("end", text)
            self.log.see("end")
            self.log.configure(state="disabled")

        def set_state(self, state):
            self.state = state
            self.status_var.set(state)

        def play(self):
            if self.process is not None:
                return
            self.collect_settings()
            if not game_exe_present(self.settings["game_dir"]):
                messagebox.showwarning("Game folder", f"{EXPECTED_EXE} is not in the game folder.\n"
                                       "Choose the folder of your own copy in Settings.")
                self.show("Settings")
                return
            try:
                command = play_command(self.settings)
            except ValueError as exc:
                messagebox.showwarning("Play", str(exc))
                return
            self.run(command, "the game", "Starting")

        def run(self, command, what, state):
            if self.process is not None:
                messagebox.showinfo("Leonida", "Something is already running. Stop it first.")
                return
            self.log.configure(state="normal")
            self.log.delete("1.0", "end")
            self.log.configure(state="disabled")
            self.append("> " + " ".join(f'"{c}"' if " " in c else c for c in command) + "\n")
            try:
                self.process = spawn(command, REPO)
            except OSError as exc:
                self.append(f"Could not start: {exc}\n")
                self.set_state("Could not start")
                return
            self.what = what
            self.set_state(state)
            self.play_button.state(["disabled"])
            self.hero.itemconfigure(self.hero_window, window=self.stop_button)
            self.hint_var.set("")
            process = self.process
            threading.Thread(target=self.read_output, args=(process,), daemon=True).start()

        def read_output(self, process):
            for raw in iter(process.stdout.readline, b""):
                self.lines.put(("line", process, raw.decode("utf-8", errors="replace")))
            code = process.wait()
            self.lines.put(("exit", process, code))

        def pump(self):
            try:
                while True:
                    kind, process, value = self.lines.get_nowait()
                    if process is not self.process:
                        continue
                    if kind == "line":
                        self.append(value)
                        if self.what == "the game":
                            self.set_state(state_for_line(self.state, value))
                    else:
                        self.append(f"\nExited with code {value}.\n")
                        self.set_state(f"Exited with code {value}")
                        self.process = None
                        self.play_button.state(["!disabled"])
                        self.hero.itemconfigure(self.hero_window, window=self.play_button)
                        self.refresh_hint()
                        self.refresh_hero()
            except queue.Empty:
                pass
            self.root.after(100, self.pump)

        def stop(self):
            if self.process is None:
                return
            if messagebox.askyesno("Stop", f"Stop {self.what}? Everything it started is ended."):
                stop_tree(self.process)
                self.append("\nStopping ...\n")

        def close(self):
            if self.process is not None:
                if not messagebox.askyesno("Leonida", f"{self.what.capitalize()} is still running. "
                                           "Stop it and close the launcher?"):
                    return
                stop_tree(self.process)
            self.collect_settings()
            self.root.destroy()

        # ---- Settings
        def folder_row(self, parent, row, title, key, note):
            ttk.Label(parent, text=title).grid(row=row, column=0, sticky="w", pady=(10, 0))
            var = tk.StringVar(value=self.settings[key])
            self.vars[key] = var
            ttk.Entry(parent, textvariable=var).grid(row=row, column=1, sticky="ew", padx=8, pady=(10, 0))
            ttk.Button(parent, text="Browse ...",
                       command=lambda: self.browse(var)).grid(row=row, column=2, pady=(10, 0))
            ttk.Label(parent, text=note, style="Muted.TLabel", wraplength=640).grid(
                row=row + 1, column=1, sticky="w", padx=8)

        def build_settings(self):
            page = self.pages["Settings"]
            page.columnconfigure(0, weight=1)
            inner = ttk.Frame(page, padding=(28, 22))
            inner.grid(row=0, column=0, sticky="nsew")
            inner.columnconfigure(1, weight=1)
            ttk.Label(inner, text="Settings", font=("Segoe UI", 20, "bold")).grid(row=0, column=0, columnspan=3,
                                                                                    sticky="w")
            self.folder_row(inner, 1, "Game folder", "game_dir",
                            f"The folder of your own copy, the one with {EXPECTED_EXE}.")
            self.game_check = tk.StringVar()
            ttk.Label(inner, textvariable=self.game_check).grid(row=3, column=1, sticky="w", padx=8)
            self.vars["game_dir"].trace_add("write", lambda *_: self.check_game())
            self.folder_row(inner, 4, "Data folder", "data_dir",
                            "Shader caches and saves. Empty: the default (%LOCALAPPDATA%\\Leonida\\d3d12).")
            self.folder_row(inner, 6, "Mods folder", "mods_dir", "Empty: the mods folder of this repository.")
            options = ttk.LabelFrame(inner, text="Options (the switches of scripts\\play-d3d12.ps1)", padding=12)
            options.grid(row=8, column=0, columnspan=3, sticky="ew", pady=(20, 0))
            options.columnconfigure(0, weight=1)
            options.columnconfigure(1, weight=1)
            checks = (("dlss", "DLSS (off = -NoDlss: the picture is stretched)"),
                      ("ray_tracing", "Ray tracing (-RayTracing, slower)"),
                      ("smooth_motion", "Smooth Motion (-SmoothMotion, RTX 40 and 50)"),
                      ("precompile", "Prepare shaders before starting (off = -NoPrecompile)"),
                      ("series_x", "Series X profile (-SeriesX, much slower)"),
                      ("quality", "Quality (-Quality, slower)"),
                      ("capture", "Capture: F9 saves a frame (-Capture)"),
                      ("log", "Log to a file (-Log)"))
            for index, (key, text) in enumerate(checks):
                var = tk.BooleanVar(value=self.settings[key])
                self.vars[key] = var
                ttk.Checkbutton(options, text=text, variable=var).grid(row=index // 2, column=index % 2,
                                                                       sticky="w", pady=3)
            ttk.Label(options, text="Resolution (-Resolution)").grid(row=4, column=0, sticky="w", pady=(10, 0))
            res = tk.StringVar(value=self.settings["resolution"] or "default")
            self.vars["resolution"] = res
            ttk.Combobox(options, textvariable=res, state="readonly", width=12,
                         values=["default"] + [r for r in RESOLUTIONS if r]).grid(row=4, column=1, sticky="w",
                                                                                  pady=(10, 0))
            if not WINDOWS:
                ttk.Label(inner, text="On Linux the launcher starts scripts/play-linux.sh, which takes the "
                          "game folder and the Series X profile; the other options are for Windows.",
                          style="Muted.TLabel", wraplength=720).grid(row=9, column=0, columnspan=3, sticky="w",
                                                                     pady=(10, 0))
            buttons = ttk.Frame(inner)
            buttons.grid(row=10, column=0, columnspan=3, sticky="w", pady=(18, 0))
            ttk.Button(buttons, text="Save", command=self.save).pack(side="left")
            self.saved_var = tk.StringVar()
            ttk.Label(buttons, textvariable=self.saved_var, style="Muted.TLabel").pack(side="left", padx=12)
            self.check_game()

        def browse(self, var):
            chosen = filedialog.askdirectory(initialdir=var.get() or os.path.expanduser("~"))
            if chosen:
                var.set(os.path.normpath(chosen))

        def check_game(self):
            folder = self.vars["game_dir"].get().strip()
            if not folder:
                self.game_check.set("")
            elif game_exe_present(folder):
                self.game_check.set(f"Found {EXPECTED_EXE}.")
            else:
                self.game_check.set(f"{EXPECTED_EXE} is not in this folder.")

        def collect_settings(self):
            for key, var in self.vars.items():
                value = var.get()
                if key == "resolution":
                    value = "" if value == "default" else value
                self.settings[key] = value.strip() if isinstance(value, str) else value
            self.settings = normalise(self.settings)

        def save(self):
            self.collect_settings()
            try:
                save_settings(self.settings)
                self.saved_var.set("Saved to " + config_path())
            except OSError as exc:
                self.saved_var.set(f"Could not save: {exc}")
            self.refresh_hint()

        # ---- Mods
        def build_mods(self):
            page = self.pages["Mods"]
            page.columnconfigure(0, weight=1)
            page.rowconfigure(0, weight=1)
            inner = ttk.Frame(page, padding=(28, 22))
            inner.grid(row=0, column=0, sticky="nsew")
            inner.columnconfigure(0, weight=1)
            inner.rowconfigure(2, weight=1)
            ttk.Label(inner, text="Mods", font=("Segoe UI", 20, "bold")).grid(row=0, column=0, sticky="w")
            self.mods_note = tk.StringVar()
            ttk.Label(inner, textvariable=self.mods_note, style="Muted.TLabel").grid(row=1, column=0, sticky="w",
                                                                                      pady=(2, 10))
            self.tree = ttk.Treeview(inner, columns=("on", "folder", "name", "swaps"), show="headings",
                                     selectmode="browse")
            for column, title, width in (("on", "Enabled", 80), ("folder", "Folder", 200), ("name", "Name", 300),
                                         ("swaps", "Swaps", 70)):
                self.tree.heading(column, text=title)
                self.tree.column(column, width=width, anchor="w")
            self.tree.grid(row=2, column=0, sticky="nsew")
            self.tree.bind("<Double-1>", lambda _e: self.toggle_mod())
            row = ttk.Frame(inner)
            row.grid(row=3, column=0, sticky="w", pady=(12, 0))
            ttk.Button(row, text="Enable / disable", command=self.toggle_mod).pack(side="left")
            ttk.Button(row, text="Refresh", command=self.refresh_mods).pack(side="left", padx=8)
            ttk.Button(row, text="Open the mod manager", command=self.open_mod_manager).pack(side="left")
            self.mod_paths: dict = {}

        def refresh_mods(self):
            mods_dir = mods_dir_of(self.settings)
            self.tree.delete(*self.tree.get_children())
            self.mod_paths.clear()
            try:
                rows, errors = list_mods(mods_dir)
            except Exception as exc:  # a broken folder is shown, not raised
                rows, errors = [], [str(exc)]
            for index, mod in enumerate(rows):
                self.tree.insert("", "end", iid=str(index), values=("yes" if mod["enabled"] else "no",
                                 mod["folder"], mod["name"], mod["swaps"]))
                self.mod_paths[str(index)] = (mod["path"], mod["enabled"])
            note = f"{len(rows)} mod(s) in {mods_dir}"
            self.mods_note.set(note + (f"; {len(errors)} could not be read: " + errors[0] if errors else ""))

        def toggle_mod(self):
            selected = self.tree.selection()
            if not selected:
                return
            path, enabled = self.mod_paths[selected[0]]
            try:
                set_mod_enabled(path, not enabled)
            except Exception as exc:
                messagebox.showerror("Mods", str(exc))
            self.refresh_mods()

        def open_mod_manager(self):
            command = [sys.executable, "-I", os.path.join(REPO, "tools", "leonida_mods.py"),
                       "--dir", mods_dir_of(self.settings), "gui"]
            try:
                subprocess.Popen(command, cwd=REPO)
            except OSError as exc:
                messagebox.showerror("Mods", f"Could not open the mod manager: {exc}")

        # ---- Saves
        def build_saves(self):
            self.saves = _saves_module()
            page = self.pages["Saves"]
            page.columnconfigure(0, weight=1)
            page.rowconfigure(0, weight=1)
            inner = ttk.Frame(page, padding=(28, 22))
            inner.grid(row=0, column=0, sticky="nsew")
            inner.columnconfigure(0, weight=1)
            inner.rowconfigure(2, weight=1)
            ttk.Label(inner, text="Saves", font=("Segoe UI", 20, "bold")).grid(row=0, column=0, sticky="w")
            self.saves_note = tk.StringVar()
            ttk.Label(inner, textvariable=self.saves_note, style="Muted.TLabel", wraplength=800).grid(
                row=1, column=0, sticky="w", pady=(2, 10))
            self.stree = ttk.Treeview(inner, columns=("size", "modified", "version"), selectmode="browse")
            self.stree.heading("#0", text="Container / blob")
            self.stree.column("#0", width=330)
            for column, title, width in (("size", "Size", 90), ("modified", "Modified", 160),
                                         ("version", "Version", 70)):
                self.stree.heading(column, text=title)
                self.stree.column(column, width=width, anchor="w")
            self.stree.grid(row=2, column=0, sticky="nsew")
            self.stree.bind("<<TreeviewSelect>>", lambda _e: self.describe_selection())
            self.save_info = tk.StringVar()
            ttk.Label(inner, textvariable=self.save_info, style="Muted.TLabel", wraplength=800).grid(
                row=3, column=0, sticky="w", pady=(8, 0))
            rows = ((("Back up all", self.save_backup_all), ("Back up selected", self.save_backup_one),
                     ("Restore ...", self.save_restore), ("Duplicate ...", self.save_duplicate),
                     ("Delete", self.save_delete)),
                    (("Export blob ...", self.save_export), ("Import blob ...", self.save_import),
                     ("View bytes", self.save_view), ("Backups folder", self.save_open_backups),
                     ("Refresh", self.refresh_saves)))
            for number, buttons in enumerate(rows):
                row = ttk.Frame(inner)
                row.grid(row=4 + number, column=0, sticky="w", pady=(10 if number == 0 else 6, 0))
                for text, command in buttons:
                    ttk.Button(row, text=text, command=command).pack(side="left", padx=(0, 6))
            self.save_items: dict = {}

        def saves_paths(self):
            data = data_root(self.settings)
            return data, self.saves.saves_root(data), self.saves.backups_dir(data)

        def refresh_saves(self):
            _data, root, _backups = self.saves_paths()
            self.stree.delete(*self.stree.get_children())
            self.save_items.clear()
            containers = self.saves.scan(root)
            for index, c in enumerate(containers):
                iid = f"c{index}"
                self.save_items[iid] = (c, None)
                self.stree.insert("", "end", iid=iid, open=True, text=c.name or c.folder,
                                  values=(self.fmt_size(c.size), self.fmt_time(c.modified),
                                          f"v{c.version}" if c.version else c.problem))
                for blob in c.blobs:
                    bid = f"{iid}b{blob.index}"
                    self.save_items[bid] = (c, blob)
                    self.stree.insert(iid, "end", iid=bid, text=blob.name,
                                      values=(self.fmt_size(blob.size), self.fmt_time(blob.modified),
                                              f"v{c.version}"))
            self.saves_note.set(f"{len(containers)} container(s) in {root}" if containers else
                                f"No saves found in {root}. They appear after the game has saved once.")
            self.save_info.set("")

        @staticmethod
        def fmt_size(n):
            return f"{n / 1024:.1f} KB" if n >= 1024 else f"{n} B"

        @staticmethod
        def fmt_time(t):
            return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(t)) if t else ""

        def selected(self, want_blob=False):
            chosen = self.stree.selection()
            if not chosen:
                messagebox.showinfo("Saves", "Select a container or a blob first.")
                return None
            container, blob = self.save_items[chosen[0]]
            if want_blob and blob is None:
                messagebox.showinfo("Saves", "Select a blob (a row under a container).")
                return None
            return container, blob

        def describe_selection(self):
            chosen = self.stree.selection()
            if not chosen or chosen[0] not in self.save_items:
                return
            container, blob = self.save_items[chosen[0]]
            if blob is None:
                self.save_info.set(f"{container.name}: {len(container.blobs)} blob(s), newest version "
                                   f"v{container.version}, folder {container.folder}")
                return
            try:
                with open(blob.path, "rb") as f:
                    self.save_info.set(self.saves.explain(self.saves.describe(f.read())))
            except OSError as exc:
                self.save_info.set(str(exc))

        def guarded(self, action):
            """Runs a save action unless the game (or anything started here) is running."""
            if self.process is not None or game_running():
                messagebox.showwarning("Saves", "The game is running. Close it before changing saves.")
                return
            try:
                action()
            except (self.saves.SaveError, OSError) as exc:
                messagebox.showerror("Saves", str(exc))
            self.refresh_saves()

        def save_backup_all(self):
            def go():
                _d, root, backups = self.saves_paths()
                messagebox.showinfo("Saves", "Backed up to " + self.saves.backup(root, backups))
            self.guarded(go)

        def save_backup_one(self):
            def go():
                got = self.selected()
                if got:
                    _d, root, backups = self.saves_paths()
                    messagebox.showinfo("Saves", "Backed up to " + self.saves.backup(root, backups, [got[0]]))
            self.guarded(go)

        def save_restore(self):
            def go():
                _d, root, backups = self.saves_paths()
                zip_path = filedialog.askopenfilename(title="Restore from a backup",
                                                      initialdir=backups if os.path.isdir(backups) else None,
                                                      filetypes=[("Save backups", "*.zip")])
                if not zip_path:
                    return
                names = sorted(self.saves.zip_containers(zip_path))
                if not messagebox.askyesno("Restore", f"Restore {len(names)} container(s) from\n{zip_path}?\n\n"
                                           "The same containers now in your saves are replaced. The current "
                                           "saves are backed up first."):
                    return
                before = self.saves.restore(root, backups, zip_path)
                messagebox.showinfo("Saves", "Restored." + (f"\nPrevious state: {before}" if before else ""))
            self.guarded(go)

        def save_duplicate(self):
            def go():
                got = self.selected()
                if not got:
                    return
                from tkinter import simpledialog
                name = simpledialog.askstring("Duplicate", "Name of the copy (letters, digits, _ . -):",
                                              initialvalue=got[0].name + "_copy")
                if name:
                    self.saves.duplicate(self.saves_paths()[1], got[0], name.strip())
            self.guarded(go)

        def save_delete(self):
            def go():
                got = self.selected()
                if got and messagebox.askyesno(
                        "Delete", f"Delete {got[0].name}?\n\nIt is moved to the backups folder "
                        "(save-backups, deleted), not erased."):
                    messagebox.showinfo("Saves", "Moved to " + self.saves.delete(got[0], self.saves_paths()[2]))
            self.guarded(go)

        def save_export(self):
            def go():
                got = self.selected(want_blob=True)
                if got:
                    target = filedialog.asksaveasfilename(
                        initialfile=f"{got[0].name}-{got[1].name}.bin".replace(" ", "_"))
                    if target:
                        self.saves.export_blob(got[1], target)
            self.guarded(go)

        def save_import(self):
            def go():
                got = self.selected(want_blob=True)
                if not got:
                    return
                source = filedialog.askopenfilename(title="Blob file to import")
                if source and messagebox.askyesno(
                        "Import", f"Replace '{got[1].name}' of {got[0].name} with this file?\n\nThe container is "
                        "backed up first and the change is written as a new version. The header blob is not "
                        "changed: a size that no longer matches it can make the title reject the save."):
                    _d, root, backups = self.saves_paths()
                    number, _saved = self.saves.import_blob(root, backups, got[0], got[1], source)
                    messagebox.showinfo("Saves", f"Written as v{number}.")
            self.guarded(go)

        def save_view(self):
            got = self.selected(want_blob=True)
            if not got:
                return
            try:
                with open(got[1].path, "rb") as f:
                    data = f.read()
            except OSError as exc:
                messagebox.showerror("Saves", str(exc))
                return
            window = tk.Toplevel(self.root)
            window.title(f"{got[0].name} / {got[1].name} (read only)")
            window.configure(bg=BG)
            window.geometry("820x560")
            ttk.Label(window, text=self.saves.explain(self.saves.describe(data)), wraplength=780).pack(
                anchor="w", padx=12, pady=8)
            box = tk.Text(window, bg=NAV, fg=TEXT, relief="flat", font=("Consolas" if WINDOWS else "monospace", 9))
            box.pack(fill="both", expand=True, padx=12, pady=(0, 12))
            box.insert("end", self.saves.hexdump(data) + "\n\nStrings:\n" + "\n".join(self.saves.strings(data)))
            box.configure(state="disabled")

        def save_open_backups(self):
            self.open(self.saves_paths()[2])

        # ---- Tools
        def build_tools(self):
            page = self.pages["Tools"]
            inner = ttk.Frame(page, padding=(28, 22))
            inner.grid(row=0, column=0, sticky="nsew")
            ttk.Label(inner, text="Tools", font=("Segoe UI", 20, "bold")).grid(row=0, column=0, columnspan=2,
                                                                                sticky="w", pady=(0, 10))
            items = (("Fetch NVIDIA DLSS files", "Downloads about 70 MB from NVIDIA's GitHub, once.",
                      self.fetch_dlss),
                     ("Rebuild the runtime", "d3d12\\scripts\\build-nvidia.ps1 (a few minutes).", self.rebuild),
                     ("Open the logs folder", logs_dir(), lambda: self.open(logs_dir())),
                     ("Open the screenshots folder", "The F4 menu's screenshots, else the F9 captures.",
                      self.open_screenshots))
            for row, (title, note, command) in enumerate(items, start=1):
                ttk.Button(inner, text=title, command=command, width=28).grid(row=row, column=0, sticky="w",
                                                                              pady=6)
                ttk.Label(inner, text=note, style="Muted.TLabel").grid(row=row, column=1, sticky="w", padx=14)

        def fetch_dlss(self):
            if not messagebox.askokcancel("Fetch NVIDIA DLSS files",
                                          "This downloads about 70 MB from NVIDIA's GitHub "
                                          "(DLSS and Streamline, under NVIDIA's own licences).\n\nContinue?"):
                return
            self.run_tool("get_dlss", "the DLSS download")

        def rebuild(self):
            self.run_tool("build", "the runtime build")

        def run_tool(self, name, what):
            try:
                command = tool_command(name)
            except ValueError as exc:
                messagebox.showinfo("Tools", str(exc))
                return
            self.show("Play")
            self.run(command, what, what.capitalize())

        def open(self, path):
            try:
                open_folder(path)
            except OSError as exc:
                messagebox.showerror("Leonida", f"Could not open {path}: {exc}")

        def open_screenshots(self):
            shots = [d for d in screenshot_dirs() if os.path.isdir(d)]
            self.open(shots[0] if shots else screenshot_dirs()[-1])

        # ---- About
        def build_about(self):
            page = self.pages["About"]
            inner = ttk.Frame(page, padding=(28, 22))
            inner.grid(row=0, column=0, sticky="nsew")
            ttk.Label(inner, text="About", font=("Segoe UI", 20, "bold")).pack(anchor="w")
            ttk.Label(inner, text=ABOUT, wraplength=760, justify="left").pack(anchor="w", pady=(14, 0))

    root = tk.Tk()
    Launcher(root)
    root.mainloop()
    return 0


def main() -> int:
    return run_gui()


if __name__ == "__main__":
    raise SystemExit(main())
