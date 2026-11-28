#!/usr/bin/env python3
"""Stage timeline of a boot from the runtime's stderr log: the title's own state (LEONIDA_PEEK lines) joined with the
scripted presses (LEONIDA_INPUT_SCRIPT), so a Confirm can be paired with the state change it caused.

Use the same hexadecimal game and session state RVAs as the boot that produced the log.
For an older log, pass its RVAs on the command line. If LEONIDA_PEEK also recorded the
loading-screen state, --load-state-rva lets the timeline mark its low byte changing from 2 to 0.
"""
import argparse
import json
import os
import re
import sys

PEEK = re.compile(r"\[rt\] peek t=(\d+)s:(.*)")
FIELD = re.compile(r"0[xX]([0-9a-fA-F]+)=([0-9a-fA-F?,]+)")
PRESS = re.compile(r"\[rt\] gameinput: scripted press of button (\d+) at ([0-9]+(?:\.[0-9]+)?) s")

SESSION = {1: "boot screens", 3: "landing", 0xD: "first-run pages", 0: "story load"}
GAME = {0: "not started", 2: "init", 3: "running"}


def first_value(text):
    if not text:
        return None
    head = text.split(",", 1)[0]
    if not head or "?" in head:
        return None
    try:
        return int(head, 16)
    except ValueError:
        return None


def hex_rva(text):
    if not re.fullmatch(r"(?:0[xX])?[0-9a-fA-F]+", text):
        raise argparse.ArgumentTypeError("expected a hexadecimal RVA, with or without 0x")
    return int(text, 16)


def read(path, game_rva, session_rva, load_rva):
    peeks, presses = [], []
    with open(path, encoding="utf-8-sig", errors="replace") as f:
        for line in f:
            m = PEEK.search(line)
            if m:
                fields = {int(k, 16): v for k, v in FIELD.findall(m.group(2))}
                if fields.keys().isdisjoint((game_rva, session_rva, load_rva)):
                    continue
                game = first_value(fields.get(game_rva))
                raw = first_value(fields.get(session_rva))
                session = None if raw is None else raw & 0xFFFFFFFF
                load = first_value(fields.get(load_rva))
                peeks.append({"t": int(m.group(1)), "game": game, "session": session, "load": load})
                continue
            m = PRESS.search(line)
            if m:
                presses.append({"t": float(m.group(2)), "button": int(m.group(1))})
    return peeks, presses


def timeline(peeks, presses):
    """Stage changes with the presses between them; each press is paired with the next stage change, if any."""
    events = []
    last = None
    seen_load_byte = False
    for p in peeks:
        load = p["load"]
        ended = p["game"] == 3 and p["session"] == 0 and load is not None and seen_load_byte and (load & 0xFF) == 0
        if load is not None and (load & 0xFF) == 2:
            seen_load_byte = True
        name = "story load (loading screen ended)" if ended else (
            SESSION.get(p["session"], f"session {p['session']:#x}") if p["game"] == 3 and p["session"] is not None
            else GAME.get(p["game"], "no title state yet" if p["game"] is None else f"game state {p['game']:#x}"))
        if name != last:
            events.append({"t": p["t"], "stage": name})
            last = name
    for pr in presses:
        nxt = next((e for e in events if e["t"] >= pr["t"]), None)
        pr["next_change"] = None if nxt is None else {"t": nxt["t"], "stage": nxt["stage"]}
    return events


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("--json", action="store_true", help="print the stages and presses as JSON")
    ap.add_argument("--game-state-rva", type=hex_rva, default=os.environ.get("LEONIDA_GAME_STATE_RVA"),
                    help="game state RVA (defaults to LEONIDA_GAME_STATE_RVA)")
    ap.add_argument("--session-state-rva", type=hex_rva, default=os.environ.get("LEONIDA_SESSION_STATE_RVA"),
                    help="session state RVA (defaults to LEONIDA_SESSION_STATE_RVA)")
    ap.add_argument("--load-state-rva", type=hex_rva, help="optional loading-screen state RVA")
    a = ap.parse_args(argv)
    if a.game_state_rva is None or a.session_state_rva is None:
        ap.error("set LEONIDA_GAME_STATE_RVA and LEONIDA_SESSION_STATE_RVA or pass "
                 "--game-state-rva and --session-state-rva")
    try:
        peeks, presses = read(a.log, a.game_state_rva, a.session_state_rva, a.load_state_rva)
    except OSError as e:
        print(f"stage_timeline: {a.log}: {e.strerror or e}", file=sys.stderr)
        return 2
    if not peeks:
        print("no peek fields matched the configured state RVAs; check the RVAs and the boot's LEONIDA_PEEK setting",
              file=sys.stderr)
        return 1
    events = timeline(peeks, presses)
    if a.json:
        print(json.dumps({"stages": events, "presses": presses}, indent=1))
        return 0
    print("stages (first peek showing each):")
    for e in events:
        print(f"  {e['t']:>5} s  {e['stage']}")
    print("scripted presses -> next stage change:")
    for pr in presses:
        n = pr["next_change"]
        print(f"  {pr['t']:>7.1f} s  button {pr['button']}  -> " +
              (f"{n['stage']} at {n['t']} s" if n else "no later change"))
    return 0


if __name__ == "__main__":
    try:  # a redirected stdout is strict ANSI code page under -I; write '?' for what it cannot encode
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError, OSError):
        pass
    sys.exit(main())
