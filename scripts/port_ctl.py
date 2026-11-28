#!/usr/bin/env python3
"""Drive a VIctory run of the title for testing: start, press, shot, fps, stop.

Everything the run writes (logs, frame captures, saves, caches) goes under a
work folder outside the repository (``--work``, default ``G:\\gta6port`` or
``LEONIDA_PORT_WORK``). Nothing from the game is read or written here beyond
what the runtime itself does.

    python -I scripts/port_ctl.py start --name run1        # launch the title
    python -I scripts/port_ctl.py press Enter              # tap A on the stand-in pad
    python -I scripts/port_ctl.py press W 3000             # hold left stick up for 3 s
    python -I scripts/port_ctl.py shot                     # save the next presented frame
    python -I scripts/port_ctl.py fps                      # latest frame-rate lines
    python -I scripts/port_ctl.py status                   # running?, fps, last warnings
    python -I scripts/port_ctl.py stop                     # end the run

Keys (the stand-in pad's table): sticks W A S D / I J K L, triggers Z C,
buttons X Y Q E F H, Enter (A), Back (B), Esc (Menu), Tab (View),
Up Down Left Right (D-pad). The runtime reads the pad file only while no real
controller is connected.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VICTORY = os.path.join(REPO, "victory")
DEFAULT_WORK = os.environ.get("LEONIDA_PORT_WORK", r"G:\gta6port")
DEFAULT_GAME = os.environ.get("LEONIDA_GAME_DIR", os.path.expanduser(r"~\Documents\GTA6"))
TITLE_EXE = "game_scarlett_master.exe"

FPS_RE = re.compile(r"(\d+(?:\.\d+)?) fps, (\d+(?:\.\d+)?) ms/frame")
ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")


def read_log(log):
    try:
        with open(log, encoding="utf-8", errors="replace") as f:
            return [ANSI_RE.sub("", ln) for ln in f]
    except OSError:
        return []


def paths(work):
    ctl = os.path.join(work, "ctl")
    return {
        "work": work,
        "ctl": ctl,
        "state": os.path.join(ctl, "state.json"),
        "pad": os.path.join(ctl, "pad.txt"),
        "shot": os.path.join(ctl, "shot.txt"),
        "logs": os.path.join(work, "logs"),
        "captures": os.path.join(work, "captures"),
        "cache": os.path.join(work, "cache"),
        "data": os.path.join(work, "data"),
    }


def load_state(p):
    try:
        with open(p["state"], encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def save_state(p, state):
    os.makedirs(p["ctl"], exist_ok=True)
    with open(p["state"], "w", encoding="utf-8", newline="\n") as f:
        json.dump(state, f, indent=1)


def alive(pid):
    if not pid:
        return False
    out = subprocess.run(
        ["tasklist", "/FI", f"PID eq {pid}", "/NH", "/FO", "CSV"],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
    ).stdout
    return f'"{pid}"' in out


def write_atomic(path, text):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    os.replace(tmp, path)


def cmd_start(args, p):
    state = load_state(p)
    if alive(state.get("pid")):
        print(f"a run is already going: pid {state['pid']} ({state.get('name')}); stop it first")
        return 1
    exe = args.exe or os.path.join(VICTORY, "target", args.profile, "victory.exe")
    if not os.path.isfile(exe):
        print(f"no runtime at {exe}; build it first (cargo build --release -p victory --features diagnostics --bins)")
        return 1
    title = os.path.join(args.game, TITLE_EXE)
    if not os.path.isfile(title):
        print(f"no title executable at {title}")
        return 1
    name = args.name or time.strftime("run-%Y%m%d-%H%M%S")
    for key in ("ctl", "logs", "captures", "cache", "data"):
        os.makedirs(p[key], exist_ok=True)
    captures = os.path.join(p["captures"], name)
    os.makedirs(captures, exist_ok=True)
    data = args.data or p["data"]
    os.makedirs(data, exist_ok=True)
    # Fresh command files: the runtime only acts on increasing numbers.
    for f in (p["pad"], p["shot"]):
        if os.path.exists(f):
            os.remove(f)
    env = dict(os.environ)
    env.update({
        "VICTORY_GAME_DIR": args.game,
        "VICTORY_CACHE_DIR": p["cache"],
        "VICTORY_DATA_DIR": data,
        "VICTORY_LOG": args.log,
        "VICTORY_CAPTURE": captures,
        "VICTORY_CAPTURE_REQUEST": p["shot"],
        "VICTORY_MOVEMENT_FILE": p["pad"],
    })
    if args.world:
        env["VICTORY_CAPTURE_WORLD"] = "1"
    for item in args.env or []:
        k, _, v = item.partition("=")
        env[k] = v
    log = os.path.join(p["logs"], f"{name}.log")
    cmd = [exe, "run", title, "--execute", "--console", args.console]
    flags = 0x00000008 | 0x00000200  # DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP
    # The runtime writes its own bytes here; newline only keeps the repository's line-ending rule.
    with open(log, "w", encoding="utf-8", newline="\n") as out:
        proc = subprocess.Popen(cmd, cwd=args.game, env=env, stdout=out, stderr=subprocess.STDOUT,
                                stdin=subprocess.DEVNULL, creationflags=flags)
    state = {
        "pid": proc.pid, "name": name, "log": log, "captures": captures, "started": time.time(),
        "pad_seq": 0, "shot_seq": 0, "cmd": cmd,
        "env": {k: v for k, v in env.items() if k.startswith("VICTORY_")},
    }
    save_state(p, state)
    print(f"started pid {proc.pid} as {name}\n  log      {log}\n  captures {captures}")
    return 0


def cmd_press(args, p):
    state = load_state(p)
    seq = int(time.time() * 1000)
    seq = max(seq, state.get("pad_seq", 0) + 1)
    write_atomic(p["pad"], f"{seq} {args.key} {args.ms}")
    state["pad_seq"] = seq
    save_state(p, state)
    print(f"pressed {args.key} for {args.ms} ms (sequence {seq})")
    if args.wait:
        time.sleep(args.ms / 1000.0 + 0.4)
    return 0


def newest_png(folder, since):
    best = None
    try:
        for entry in os.scandir(folder):
            if entry.name.endswith(".png") and entry.stat().st_mtime >= since:
                if best is None or entry.stat().st_mtime > best.stat().st_mtime:
                    best = entry
    except OSError:
        return None
    return best.path if best else None


def shrink(path, width):
    """A copy of the capture `width` pixels wide beside it (needs Pillow); the capture itself otherwise."""
    try:
        from PIL import Image
    except ImportError:
        return path
    with Image.open(path) as im:
        height = max(1, round(im.height * width / im.width))
        small = im.resize((width, height), Image.LANCZOS)
    out = path[:-4] + f"-w{width}.png"
    small.save(out)
    return out


def cmd_shot(args, p):
    state = load_state(p)
    seq = state.get("shot_seq", 0) + 1
    since = time.time() - 0.5
    write_atomic(p["shot"], str(seq))
    state["shot_seq"] = seq
    save_state(p, state)
    folder = state.get("captures")
    deadline = time.time() + args.timeout
    while time.time() < deadline:
        found = newest_png(folder, since)
        if found:
            time.sleep(0.5)  # let the writer finish
            if args.small:
                found = shrink(found, args.small)
            print(found)
            return 0
        time.sleep(0.25)
    print(f"no new frame in {folder} after {args.timeout} s (the title may not be presenting)")
    return 1


def fps_lines(log, count):
    hits = [ln.rstrip() for ln in read_log(log) if FPS_RE.search(ln)]
    return hits[-count:]


def summarize(line):
    m = FPS_RE.search(line)
    if not m:
        return line
    tail = line[m.start():]
    keep = []
    for pattern in (r"\d+(?:\.\d+)? fps, \d+(?:\.\d+)? ms/frame", r"\(\S+ preparing", r"[\d.]+ on the GPU",
                    r"\d+ draws/frame", r"dropped \d+ without a host pipeline \(\d+ being made\)",
                    r"cpu [\d.]+% gpu \d+% busy ram \d+ MB"):
        mm = re.search(pattern, tail)
        if mm:
            keep.append(mm.group(0))
    stamp = line[:m.start()].split(" ")[0]
    return f"{stamp} " + " | ".join(keep)


def cmd_fps(args, p):
    state = load_state(p)
    log = args.log or state.get("log")
    if not log:
        print("no run recorded")
        return 1
    for line in fps_lines(log, args.count):
        print(line if args.full else summarize(line))
    return 0


def cmd_status(args, p):
    state = load_state(p)
    if not state:
        print("no run recorded")
        return 1
    up = time.time() - state.get("started", time.time())
    print(f"{state.get('name')}: pid {state.get('pid')} {'running' if alive(state.get('pid')) else 'NOT running'}, "
          f"up {int(up // 60)}:{int(up % 60):02d}")
    for line in fps_lines(state["log"], 3):
        print("  " + summarize(line))
    lines = read_log(state["log"])
    bad = [ln.rstrip() for ln in lines if re.search(r"\b(ERROR|WARN)\b|panicked|crash|DEVICE_HUNG|DEVICE_REMOVED", ln)]
    for line in bad[-args.warnings:]:
        print("  ! " + line[:300])
    return 0


def cmd_stop(args, p):
    state = load_state(p)
    pid = state.get("pid")
    if not alive(pid):
        print("no run is going")
        return 0
    subprocess.run(["taskkill", "/PID", str(pid), "/T", "/F"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(40):
        if not alive(pid):
            break
        time.sleep(0.25)
    print(f"stopped pid {pid} ({state.get('name')})")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--work", default=DEFAULT_WORK, help="work folder outside the repository")
    sub = ap.add_subparsers(dest="command", required=True)

    s = sub.add_parser("start", help="launch the title under VIctory")
    s.add_argument("--name")
    s.add_argument("--game", default=DEFAULT_GAME)
    s.add_argument("--exe", help="runtime executable (default: victory/target/<profile>/victory.exe)")
    s.add_argument("--profile", default="release")
    s.add_argument("--console", default="series-x", choices=["series-x", "series-s"])
    s.add_argument("--log", default="info", help="VICTORY_LOG filter")
    s.add_argument("--data", help="data folder (saves); default <work>/data")
    s.add_argument("--world", action="store_true", help="also capture world frames (VICTORY_CAPTURE_WORLD)")
    s.add_argument("--env", action="append", help="extra K=V for the run (repeatable)")

    s = sub.add_parser("press", help="press a stand-in pad key")
    s.add_argument("key")
    s.add_argument("ms", nargs="?", type=int, default=200)
    s.add_argument("--wait", action="store_true", help="return after the hold ends")

    s = sub.add_parser("shot", help="save the next presented frame and print its path")
    s.add_argument("--timeout", type=float, default=30.0)
    s.add_argument("--small", type=int, default=0, metavar="WIDTH",
                   help="also write a copy this many pixels wide and print its path (needs Pillow)")

    s = sub.add_parser("fps", help="latest frame-rate lines of the run's log")
    s.add_argument("--count", type=int, default=5)
    s.add_argument("--log")
    s.add_argument("--full", action="store_true")

    s = sub.add_parser("status", help="whether the run is going, its fps and last warnings")
    s.add_argument("--warnings", type=int, default=5)

    sub.add_parser("stop", help="end the run")

    args = ap.parse_args()
    p = paths(args.work)
    return {"start": cmd_start, "press": cmd_press, "shot": cmd_shot, "fps": cmd_fps,
            "status": cmd_status, "stop": cmd_stop}[args.command](args, p)


if __name__ == "__main__":
    sys.exit(main())
