"""Leonida's save folders: list, back up, restore, duplicate, delete, import and export blobs, look at bytes.

The layout is the D3D12 runtime's (d3d12/crates/leonida-hle/src/modules/xgamesave.rs):

    <data>/data/7952987A/saves/<user id>/<configuration>/<Name>~<FNV-1a 64 hex>/v<N>/{0, 1, ..., manifest}

`<Name>` is the container name with every byte but letters, digits, `-` and `_` written `%XX` and cut to 100
characters, the hash is the 64-bit FNV-1a of the exact name. The highest `v<N>` with a readable manifest is the
container. `manifest` is length-prefixed strings (u32 LE): container name, display name, then the blob names; blob
`i` is the file `i`. The runtime removes older versions when it next opens the container.

Standard library only and no Tkinter: the launcher's Saves page (tools/leonida_launcher.py) is a window over this,
and tests/test_leonida_saves.py runs it on fake trees. Nothing here decrypts or decompresses anything: a blob whose
bytes look random is reported as packed or encrypted and left alone (docs/saves.md, docs/launcher.md).
"""
from __future__ import annotations

import math
import os
import re
import shutil
import struct
import time
import zipfile
from collections import Counter
from dataclasses import dataclass, field

TITLE_DIR = "7952987A"
READABLE = 100
MANIFEST = "manifest"
FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3


class SaveError(Exception):
    """A user-facing failure."""


# ---------------------------------------------------------------- names

def fnv1a64(name: str) -> int:
    h = FNV_OFFSET
    for b in name.encode("utf-8"):
        h = ((h ^ b) * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return h


def file_name(name: str) -> str:
    out = []
    for b in name.encode("utf-8"):
        c = chr(b)
        out.append(c if (c.isascii() and (c.isalnum() or c in "-_")) else f"%{b:02X}")
    return "".join(out)


def container_dir_name(name: str) -> str:
    """The folder name of container `name`, as the runtime makes it."""
    return f"{file_name(name)[:READABLE]}~{fnv1a64(name):016x}"


def valid_container_name(name: str) -> bool:
    """The runtime's rule (XGameSave's documented container names)."""
    path, _, file = name.rpartition("/")
    if len(name) > 256 or not file or file.endswith(".") or ".." in file:
        return False
    return (all(c.isascii() and (c.isalnum() or c in "_/") for c in path)
            and all(c.isascii() and (c.isalnum() or c in "_.-") for c in file))


def build_manifest(name: str, display: str, blobs: list[str]) -> bytes:
    out = bytearray()
    for text in [name, display, *blobs]:
        raw = text.encode("utf-8")
        out += struct.pack("<I", len(raw)) + raw
    return bytes(out)


def parse_manifest(data: bytes) -> list[str] | None:
    out, pos = [], 0
    while pos < len(data):
        if pos + 4 > len(data):
            return None
        (length,) = struct.unpack_from("<I", data, pos)
        pos += 4
        if pos + length > len(data):
            return None
        try:
            out.append(data[pos:pos + length].decode("utf-8"))
        except UnicodeDecodeError:
            return None
        pos += length
    return out if len(out) >= 2 else None


def version_number(name: str) -> int | None:
    m = re.fullmatch(r"v([1-9][0-9]*)", name)
    return int(m.group(1)) if m else None


# ---------------------------------------------------------------- scanning

@dataclass
class Blob:
    name: str
    index: int
    path: str
    size: int
    modified: float


@dataclass
class Container:
    user: str
    config: str
    folder: str          # <Name>~<hash>
    path: str
    name: str = ""
    display: str = ""
    version: int = 0     # newest readable version
    highest: int = 0     # highest v<N> of any kind
    blobs: list[Blob] = field(default_factory=list)
    problem: str = ""

    @property
    def modified(self) -> float:
        return max((b.modified for b in self.blobs), default=0.0)

    @property
    def size(self) -> int:
        return sum(b.size for b in self.blobs)


def saves_root(data_dir: str) -> str:
    """<data>/data/7952987A/saves, the folder the runtime keeps saves in (LEONIDA_DATA_DIR is <data>/data)."""
    return os.path.join(data_dir, "data", TITLE_DIR, "saves")


def backups_dir(data_dir: str) -> str:
    return os.path.join(data_dir, "save-backups")


def read_container(user: str, config: str, path: str) -> Container:
    folder = os.path.basename(path)
    container = Container(user, config, folder, path)
    numbers = []
    for entry in os.listdir(path):
        n = version_number(entry)
        if n is not None and os.path.isdir(os.path.join(path, entry)):
            numbers.append(n)
    container.highest = max(numbers, default=0)
    for n in sorted(numbers, reverse=True):
        vdir = os.path.join(path, f"v{n}")
        try:
            with open(os.path.join(vdir, MANIFEST), "rb") as f:
                names = parse_manifest(f.read())
        except OSError:
            names = None
        if names is None:
            continue
        container.version, container.name, container.display = n, names[0], names[1]
        for index, blob in enumerate(names[2:]):
            bpath = os.path.join(vdir, str(index))
            try:
                st = os.stat(bpath)
                container.blobs.append(Blob(blob, index, bpath, st.st_size, st.st_mtime))
            except OSError:
                container.blobs.append(Blob(blob, index, bpath, 0, 0.0))
        return container
    container.problem = "no version has a readable manifest" if numbers else "no version folder"
    return container


def scan(root: str) -> list[Container]:
    """Every container under `root` (the saves folder): user / configuration / container folders."""
    found = []
    if not os.path.isdir(root):
        return found
    for user in sorted(os.listdir(root)):
        upath = os.path.join(root, user)
        if not os.path.isdir(upath):
            continue
        for config in sorted(os.listdir(upath)):
            cpath = os.path.join(upath, config)
            if not os.path.isdir(cpath):
                continue
            for folder in sorted(os.listdir(cpath)):
                path = os.path.join(cpath, folder)
                if os.path.isdir(path) and "~" in folder and not folder.endswith((".gone", ".new")):
                    found.append(read_container(user, config, path))
    return found


# ---------------------------------------------------------------- backups

def stamp() -> str:
    return time.strftime("%Y%m%d-%H%M%S")


def unique_path(folder: str, base: str, ext: str) -> str:
    os.makedirs(folder, exist_ok=True)
    path, n = os.path.join(folder, base + ext), 1
    while os.path.exists(path):
        n += 1
        path = os.path.join(folder, f"{base}-{n}{ext}")
    return path


def backup(root: str, backup_folder: str, containers: list[Container] | None = None, label: str = "") -> str:
    """A ZIP of the given containers (all when None) in `backup_folder`, paths relative to `root`."""
    chosen = scan(root) if containers is None else containers
    if not chosen:
        raise SaveError("There are no saves to back up.")
    path = unique_path(backup_folder, stamp() + (f"-{label}" if label else ""), ".zip")
    tmp = path + ".part"
    try:
        with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as z:
            for c in chosen:
                for base, _dirs, files in os.walk(c.path):
                    for name in files:
                        full = os.path.join(base, name)
                        z.write(full, os.path.relpath(full, root).replace(os.sep, "/"))
        os.replace(tmp, path)
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)
    return path


def list_backups(backup_folder: str) -> list[str]:
    if not os.path.isdir(backup_folder):
        return []
    return sorted((os.path.join(backup_folder, n) for n in os.listdir(backup_folder) if n.endswith(".zip")),
                  reverse=True)


def zip_containers(zip_path: str) -> set[str]:
    """The `<user>/<config>/<container folder>` prefixes inside a backup; SaveError when it is not a backup."""
    prefixes = set()
    try:
        with zipfile.ZipFile(zip_path) as z:
            for name in z.namelist():
                parts = name.split("/")
                if len(parts) < 5 or any(p in ("", ".", "..") or ":" in p or "\\" in p for p in parts):
                    raise SaveError(f"Not a save backup: unexpected entry {name!r}.")
                prefixes.add("/".join(parts[:3]))
    except zipfile.BadZipFile as exc:
        raise SaveError(f"Not a ZIP file: {exc}") from exc
    if not prefixes:
        raise SaveError("The backup is empty.")
    return prefixes


def restore(root: str, backup_folder: str, zip_path: str) -> str:
    """Puts the containers of a backup back, replacing the same ones. Backs up the current saves first (when
    there are any); returns that backup's path ('' if none was needed)."""
    prefixes = zip_containers(zip_path)
    before = ""
    if scan(root):
        before = backup(root, backup_folder, label="before-restore")
    staging = os.path.join(os.path.dirname(root), "restore.new")
    shutil.rmtree(staging, ignore_errors=True)
    os.makedirs(staging)
    try:
        with zipfile.ZipFile(zip_path) as z:
            z.extractall(staging)
        for prefix in sorted(prefixes):
            target = os.path.join(root, *prefix.split("/"))
            if os.path.isdir(target):
                shutil.rmtree(target)
            os.makedirs(os.path.dirname(target), exist_ok=True)
            os.replace(os.path.join(staging, *prefix.split("/")), target)
    finally:
        shutil.rmtree(staging, ignore_errors=True)
    return before


# ---------------------------------------------------------------- changing saves

def _write_version(c: Container, blob_data: dict[int, bytes], name: str, display: str) -> int:
    """Commits v<highest+1> of container `c` the runtime's way (staged as .new, one rename): blobs from the newest
    readable version with `blob_data` laid over them. The old version stays."""
    if not c.version:
        raise SaveError("This container has no readable version.")
    number = c.highest + 1
    staged = os.path.join(c.path, f"v{number}.new")
    shutil.rmtree(staged, ignore_errors=True)
    os.makedirs(staged)
    try:
        for blob in c.blobs:
            if blob.index in blob_data:
                with open(os.path.join(staged, str(blob.index)), "wb") as f:
                    f.write(blob_data[blob.index])
            else:
                shutil.copyfile(blob.path, os.path.join(staged, str(blob.index)))
        with open(os.path.join(staged, MANIFEST), "wb") as f:
            f.write(build_manifest(name, display, [b.name for b in c.blobs]))
        os.rename(staged, os.path.join(c.path, f"v{number}"))
    except BaseException:
        shutil.rmtree(staged, ignore_errors=True)
        raise
    return number


def duplicate(root: str, c: Container, new_name: str) -> str:
    """A copy of the container under `new_name`: its own folder (name and FNV hash), the manifest naming it. The
    display name follows when it was the container's name. Returns the new folder."""
    if not valid_container_name(new_name):
        raise SaveError("A container name has letters, digits and _ . - (and / before the last part), at most 256.")
    if not c.version:
        raise SaveError("This container has no readable version.")
    target = os.path.join(os.path.dirname(c.path), container_dir_name(new_name))
    if os.path.exists(target):
        raise SaveError(f"A container named {new_name} already exists.")
    staged = os.path.join(target, "v1.new")
    os.makedirs(staged)
    try:
        for blob in c.blobs:
            shutil.copyfile(blob.path, os.path.join(staged, str(blob.index)))
        display = new_name if c.display == c.name else c.display
        with open(os.path.join(staged, MANIFEST), "wb") as f:
            f.write(build_manifest(new_name, display, [b.name for b in c.blobs]))
        os.rename(staged, os.path.join(target, "v1"))
    except BaseException:
        shutil.rmtree(target, ignore_errors=True)
        raise
    return target


def delete(c: Container, backup_folder: str) -> str:
    """Moves the container folder into `<backups>/deleted/` (nothing is erased); returns where it went."""
    folder = os.path.join(backup_folder, "deleted")
    os.makedirs(folder, exist_ok=True)
    target = unique_path(folder, f"{stamp()}-{c.folder}", "")
    shutil.move(c.path, target)
    return target


def export_blob(blob: Blob, destination: str) -> None:
    shutil.copyfile(blob.path, destination)


def import_blob(root: str, backup_folder: str, c: Container, blob: Blob, source: str) -> tuple[int, str]:
    """Replaces one blob with the bytes of `source` as a new version, after a backup of the container. Returns the
    new version number and the backup path. The blob's companion (the header blob) is not touched."""
    with open(source, "rb") as f:
        data = f.read()
    if not data:
        raise SaveError("The file is empty.")
    saved = backup(root, backup_folder, [c], label="before-import")
    return _write_version(c, {blob.index: data}, c.name, c.display), saved


# ---------------------------------------------------------------- looking at bytes

def entropy(data: bytes) -> float:
    """Shannon entropy in bits per byte (8.0 is random: compressed or encrypted)."""
    if not data:
        return 0.0
    n = len(data)
    return -sum(c / n * math.log2(c / n) for c in Counter(data).values())


def hexdump(data: bytes, limit: int = 4096) -> str:
    lines = []
    for off in range(0, min(len(data), limit), 16):
        chunk = data[off:off + 16]
        hexes = " ".join(f"{b:02x}" for b in chunk).ljust(47)
        text = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append(f"{off:08x}  {hexes}  {text}")
    if len(data) > limit:
        lines.append(f"... {len(data) - limit} more bytes")
    return "\n".join(lines)


def strings(data: bytes, minimum: int = 6, limit: int = 400) -> list[str]:
    """Printable ASCII runs and UTF-16LE runs of at least `minimum` characters."""
    found = [m.group().decode("ascii") for m in re.finditer(rb"[ -~\t]{%d,}" % minimum, data)]
    found += [m.group().decode("utf-16-le") for m in
              re.finditer(rb"(?:[ -~]\x00){%d,}" % minimum, data)]
    return found[:limit]


def describe(data: bytes) -> dict:
    """What a blob is, from its bytes: `kind` (asav, text or unknown), `packed` (random-looking body), `entropy`,
    the ASAV header fields, and `editable` (always False: see docs/launcher.md)."""
    info: dict = {"size": len(data), "entropy": round(entropy(data), 2), "kind": "unknown", "packed": False,
                  "editable": False}
    if data[:4] == b"ASAV" and len(data) >= 0x170:
        version, kind, mode, _a, offset, packed_size, size = struct.unpack_from("<7I", data, 4)
        (when,) = struct.unpack_from("<Q", data, 0x20)
        info.update(kind="asav", version=version, asav_kind=kind, mode=mode, payload_offset=offset,
                    time=when, title=data[0x28:0x128].decode("utf-16-le", errors="replace").split("\0")[0],
                    slot=data[0x128:0x160].split(b"\0")[0].decode("ascii", errors="replace"))
        body = data[offset:]
        info["packed"] = mode == 3 or (len(body) > 256 and entropy(body) > 7.5)
        info["payload_entropy"] = round(entropy(body), 2)
    elif len(data) > 256 and entropy(data) > 7.5:
        info["packed"] = True
    elif data and all(b in b"\t\r\n" or 32 <= b < 127 for b in data[:512]):
        info["kind"] = "text"
    return info


def explain(info: dict) -> str:
    """One paragraph for the page."""
    if info["kind"] == "asav" and info["packed"]:
        return ("This save's body is packed (entropy %.2f of 8): compressed or encrypted, and the launcher does not "
                "try to decode it. Editing is not supported; backup, restore, duplicate and export still work."
                % info.get("payload_entropy", info["entropy"]))
    if info["kind"] == "asav":
        return ("Plain ASAV container (kind %s). Its header carries a checksum whose algorithm is not known "
                "(docs/saves.md), so values are not edited here; export the blob, change it, and import it "
                "(a backup is made and a new version written)." % info["asav_kind"])
    if info["packed"]:
        return "The bytes look compressed or encrypted (entropy %.2f of 8). Editing is not supported." % info["entropy"]
    return "No known format. The viewer is read-only; export and import replace the whole blob."
