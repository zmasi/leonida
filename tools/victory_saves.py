#!/usr/bin/env python3
"""Convert a VIctory data folder's saves and local storage into gta6rt's layout.

VIctory keeps a title's data under `<data>\\<title id>\\`:
  saves\\<user>\\<scid>\\<container>~<hash>\\v<N>\\{manifest, 0, 1, ...}
      manifest: length-prefixed strings (u32 little-endian): the container's
      name, its display name, then the blob names in the order of the numbered
      files (victory/crates/victory-hle/src/modules/xgamesave.rs, manifest()).
      The highest v<N> whose manifest and blobs are all there is the save.
  storage\\...    the title's persistent local storage.

gta6rt keeps them as (runtime/shims/xgameruntime_gamesave.cpp, docs/saves.md):
  <saves root>\\local-user\\<container>\\<blob>, the display name in
  <container>\\.display; names %-encoded: [A-Za-z0-9_-] and '.' (not first)
  stay, every other UTF-8 byte becomes %XX.
  <overlay>\\.gta6rt-persistent\\...   persistent local storage
  (runtime/shims/file_system.h, kPersistentStorageDir).

The saves root is what gta6rt reads from LEONIDA_SAVE_DIR (it appends
Leonida\\saves), else %LOCALAPPDATA%\\Leonida\\saves.

    python -I tools/victory_saves.py <victory data>\\<title id> --saves <root>\\Leonida\\saves --overlay <overlay>

Nothing is overwritten unless --force. Only the user's own save files are
read; nothing of the game is involved.
"""

import argparse
import os
import shutil
import struct
import sys

KEEP = set(b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-")
PERSISTENT_DIR = ".gta6rt-persistent"


def encode_name(name):
    """gta6rt's encode_name: one spelling per name."""
    out = []
    for i, c in enumerate(name.encode("utf-8")):
        if c in KEEP or (c == ord(".") and i > 0):
            out.append(chr(c))
        else:
            out.append("%%%02X" % c)
    return "".join(out)


def parse_manifest(data):
    """[name, display, blob...] or None for a manifest VIctory could not have written."""
    names, at = [], 0
    while at < len(data):
        if at + 4 > len(data):
            return None
        (n,) = struct.unpack_from("<I", data, at)
        at += 4
        if at + n > len(data):
            return None
        try:
            names.append(data[at:at + n].decode("utf-8"))
        except UnicodeDecodeError:
            return None
        at += n
    return names if len(names) >= 2 else None


def newest_version(container_dir):
    """(version folder, [name, display, blob...]) of the newest complete save, or None."""
    versions = []
    for entry in os.listdir(container_dir):
        if entry.startswith("v") and entry[1:].isdigit():
            versions.append((int(entry[1:]), entry))
    for _, entry in sorted(versions, reverse=True):
        folder = os.path.join(container_dir, entry)
        try:
            with open(os.path.join(folder, "manifest"), "rb") as f:
                names = parse_manifest(f.read())
        except OSError:
            continue
        if names and all(os.path.isfile(os.path.join(folder, str(i))) for i in range(len(names) - 2)):
            return folder, names
    return None


def find_containers(saves_dir):
    """Every <container>~<hash> folder under saves\\<user>\\<scid>\\."""
    found = []
    for root, dirs, _ in os.walk(saves_dir):
        for d in dirs:
            if "~" in d:
                found.append(os.path.join(root, d))
        dirs[:] = [d for d in dirs if "~" not in d]
    return sorted(found)


def convert(title_dir, saves_root, overlay, force=False, log=print):
    saves_dir = os.path.join(title_dir, "saves")
    user_dir = os.path.join(saves_root, "local-user")
    converted = 0
    if os.path.isdir(saves_dir):
        for container_dir in find_containers(saves_dir):
            newest = newest_version(container_dir)
            if not newest:
                log(f"skipped {container_dir}: no complete version")
                continue
            folder, names = newest
            name, display, blobs = names[0], names[1], names[2:]
            out = os.path.join(user_dir, encode_name(name))
            if os.path.exists(out) and not force:
                log(f"skipped {name}: {out} exists (use --force)")
                continue
            os.makedirs(out, exist_ok=True)
            for i, blob in enumerate(blobs):
                shutil.copyfile(os.path.join(folder, str(i)), os.path.join(out, encode_name(blob)))
            with open(os.path.join(out, ".display"), "wb") as f:
                f.write(display.encode("utf-8"))
            log(f"save {name} ({os.path.basename(folder)}): {len(blobs)} blob(s) -> {out}")
            converted += 1
    storage = os.path.join(title_dir, "storage")
    if overlay and os.path.isdir(storage):
        target = os.path.join(overlay, PERSISTENT_DIR)
        for root, _, files in os.walk(storage):
            rel = os.path.relpath(root, storage)
            dest_dir = os.path.normpath(os.path.join(target, rel))
            os.makedirs(dest_dir, exist_ok=True)
            for name in files:
                dest = os.path.join(dest_dir, name)
                if os.path.exists(dest) and not force:
                    log(f"skipped {dest}: exists (use --force)")
                    continue
                shutil.copyfile(os.path.join(root, name), dest)
                log(f"storage {os.path.join(rel, name)} -> {dest}")
    return converted


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("title_dir", help=r"VIctory's <data>\<title id> folder (it holds saves\ and storage\)")
    ap.add_argument("--saves", required=True, help=r"gta6rt's saves root (<LEONIDA_SAVE_DIR>\Leonida\saves)")
    ap.add_argument("--overlay", help="gta6rt's --overlay folder, for persistent local storage")
    ap.add_argument("--force", action="store_true", help="overwrite existing containers and files")
    args = ap.parse_args(argv)
    if not os.path.isdir(args.title_dir):
        print(f"not a folder: {args.title_dir}")
        return 1
    n = convert(args.title_dir, args.saves, args.overlay, args.force)
    print(f"{n} container(s) converted")
    return 0


if __name__ == "__main__":
    sys.exit(main())
