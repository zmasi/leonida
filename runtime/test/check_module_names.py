"""Original PE fixtures only; native Windows, real mapped entry, and strict UTF-8 traces."""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time


def exact_acp(text: str) -> bytes | None:
    try:
        encoded = text.encode("mbcs", "strict")
        return encoded if encoded.decode("mbcs", "strict") == text else None
    except (UnicodeError, LookupError):
        return None


def _is_windows() -> bool:
    return os.name == "nt"


def _preflight_paths(args: argparse.Namespace, parser: argparse.ArgumentParser) -> None:
    for name in ("loader", "guest", "importer", "dependency"):
        raw = str(getattr(args, name))
        if not raw.strip():
            parser.error(f"--{name} path must not be empty")
        if not Path(raw).is_file():
            parser.error(f"--{name} is not an existing file: {raw}")
    raw_work = str(args.work_dir)
    if not raw_work.strip():
        parser.error("--work-dir must not be empty")
    work_abs = Path(os.path.abspath(raw_work))
    if (work_abs.exists() or work_abs.is_symlink()) and not work_abs.is_dir():
        parser.error(f"--work-dir is not a directory: {raw_work}")
    probe = work_abs.parent
    while str(probe) and not (probe.exists() or probe.is_symlink()):
        if probe.parent == probe:
            break
        probe = probe.parent
    if str(probe) and not probe.is_dir():
        parser.error(f"--work-dir parent is not a directory: {probe}")
    if args.report is not None:
        raw_rep = str(args.report)
        if not raw_rep.strip():
            parser.error("--report path must not be empty")
        if raw_rep.endswith(("/", "\\")) or (os.sep and raw_rep.endswith(os.sep)):
            parser.error(f"--report must name a file, not a directory: {raw_rep}")
        for cand in (Path(os.path.abspath(raw_rep)), Path(os.path.realpath(raw_rep))):
            if cand.is_dir():
                parser.error(f"--report is a directory: {raw_rep}")
            anc = cand.parent
            while str(anc) and not (anc.exists() or anc.is_symlink()):
                if anc.parent == anc:
                    break
                anc = anc.parent
            if str(anc) and not anc.is_dir():
                parser.error(f"--report parent is not a directory: {anc}")


def _write_report_atomic(path: Path, payload: dict) -> None:
    dest = Path(os.path.realpath(path))
    dest.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=dest.parent, prefix=".report-", suffix=".tmp")
    try:
        try:
            f = os.fdopen(fd, "w", encoding="utf-8", newline="\n")
        except BaseException:
            os.close(fd)
            raise
        with f:
            json.dump(payload, f, indent=2)
            f.write("\n")
            f.flush()
            os.fsync(f.fileno())
        if os.name == "posix":
            try:
                shutil.copymode(dest, tmp)
            except FileNotFoundError:
                pass
        for delay in (0.01, 0.02, 0.04) if _is_windows() else ():
            try:
                os.replace(tmp, dest)
                return
            except PermissionError as exc:
                if getattr(exc, "winerror", None) not in (5, 32, 33):
                    raise
                time.sleep(delay)
        os.replace(tmp, dest)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("loader", "guest", "importer", "dependency", "work-dir"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args(argv)
    if not _is_windows():
        parser.error("this comparison needs native Windows")
    _preflight_paths(args, parser)
    args.work_dir.mkdir(parents=True, exist_ok=True)
    page = ctypes.windll.kernel32.GetACP()
    # The guest also looks up the upper-case spelling through GetModuleHandleA.
    # ACP 936 can spell U+00E9 but not U+00C9, so require both forms to round-trip.
    acp_char = next((c for c in "\u00e9\u03b4\u0436\u6f22" if exact_acp(c) and exact_acp(c.upper())), None)
    unicode_char = next((c for c in "\u6f22\u5b57\u0436\u03b4\u00e9" if exact_acp(c) is None), "\u6f22")
    rows: list[dict] = []
    failures: list[str] = []
    skips: list[str] = []
    environment = os.environ.copy()
    for name in list(environment):
        if name.upper().startswith("LEONIDA_"):
            del environment[name]
    environment["LEONIDA_UNIMPLEMENTED"] = "stop"

    with tempfile.TemporaryDirectory(prefix="module_names_", dir=args.work_dir.resolve()) as temp:
        root = Path(temp)
        package = root / "package"
        package.mkdir()
        originals: dict[Path, bytes] = {}

        def copy(source: Path, destination: Path) -> None:
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, destination)
            originals[destination] = destination.read_bytes()

        def run(label: str, mode: str, image: Path, modules: set[str], self_probe: bool) -> None:
            trace = root / (label + "_" + mode.removeprefix("--") + ".jsonl")
            command = ([str(image), image.name] if mode == "native" else
                       [str(args.loader.resolve()), mode, str(image),
                        "--game-dir", str(image.parent), "--overlay", str(root / "overlay"),
                        "--trace", str(trace), "--", image.name])
            errors: list[str] = []
            try:
                child = subprocess.run(command, cwd=root, env=environment, capture_output=True, timeout=15)
                stdout = child.stdout.decode("utf-8", "replace")
                stderr = child.stderr.decode("utf-8", "replace")
                code = child.returncode
            except (OSError, subprocess.TimeoutExpired) as exc:
                code, stdout, stderr = None, "", ""
                errors.append(f"failed to run child: {exc}")
            if code != 0:
                errors.append(f"exit code {code}")
            entry = next((line for line in stdout.splitlines() if line.startswith("NAME_PROBE ")), None)
            if self_probe and mode in ("native", "--run"):
                if not entry or "entered=1" not in entry:
                    errors.append("guest did not reach its name checks")
                # The wide lookups (as named, in the system table's upper case, and LoadLibraryW of the mapped
                # image) must find the image for every name; the ANSI ones, exact, ASCII-case and code-page-case,
                # for a name the code page can spell.
                required = ["self_w=1", "case_w=1", "load_w=1"]
                if exact_acp(image.name) is not None:
                    required += ["representable=1", "self_a=1", "ascii_case_a=1", "acp_case_a=1"]
                # Under the loader, a shim-served DLL named through a directory outside ASCII is answered by basename
                # (natively the probe depends on what the machine has installed, so it is only checked here).
                if mode == "--run":
                    required += ["shim_dir_w=1", "shim_dir_load_w=1"]
                missing = [field for field in required if not entry or field not in entry.split()]
                if missing:
                    errors.append("guest lookups not retained: " + " ".join(missing))
            mapped_names: list[str] = []
            if mode != "native":
                try:
                    events = [json.loads(line) for line in trace.read_text(encoding="utf-8-sig", errors="replace").splitlines() if line.strip()]
                    mapped_names = [event["module"] for event in events if event.get("kind") == "map"]
                    if set(mapped_names) != modules or len(mapped_names) != len(modules):
                        errors.append(f"map trace names differ: {mapped_names!r}")
                except (OSError, UnicodeError, ValueError, KeyError) as exc:
                    errors.append(f"invalid UTF-8 JSON trace: {exc}")
            row = dict(case=label, mode=mode, command=command, exit_code=code, entry=entry,
                       mapped_names=mapped_names, stdout=stdout, stderr=stderr, errors=errors)
            rows.append(row)
            failures.extend(f"{label}/{mode}: {error}" for error in errors)
            print(json.dumps({key: row[key] for key in ("case", "mode", "exit_code", "entry", "errors")}),
                  flush=True)

        cases = [
            ("ascii", "plain", "Name-plain.exe"),
            ("unicode_basename", "plain", f"Name-{unicode_char}.exe"),
            ("unicode_parent", f"{unicode_char}-folder", "Name-plain.exe"),
        ]
        if acp_char:
            cases.insert(1, ("acp", "plain", f"Name-{acp_char}.exe"))
        else:
            skips.append("No selected non-ASCII character round-trips through the active ANSI code page")
        for label, directory, name in cases:
            image = package / directory / name
            copy(args.guest, image)
            for mode in ("native", "--dry-run", "--init", "--run"):
                run(label, mode, image, {name}, self_probe=True)

        # Only the import-name string in our own compiled tiny_exe is replaced. Its function and data imports
        # still have to resolve and execute successfully in both native and mapped children.
        importer = args.importer.read_bytes()
        needle = b"tiny_dll.dll\0"
        if importer.count(needle) != 1:
            raise RuntimeError("expected exactly one original synthetic DLL import name")
        imports = [("import_ascii", "Import-plain.exe", "tiny_dll.dll", needle)]
        if acp_char:
            dll = f"d-{acp_char}.dll"
            encoded = exact_acp(dll)
            if encoded is None or len(encoded) + 1 > len(needle):
                raise RuntimeError("ACP import fixture does not fit the original name span")
            replacement = (encoded + b"\0").ljust(len(needle), b"\0")
            imports.extend([
                ("import_acp", "Import-acp.exe", dll, replacement),
                ("import_unicode_exe", f"Import-{unicode_char}.exe", dll, replacement),
            ])
        for label, name, dll, replacement in imports:
            folder = package / label
            folder.mkdir()
            image = folder / name
            image.write_bytes(importer.replace(needle, replacement))
            originals[image] = image.read_bytes()
            copy(args.dependency, folder / dll)
            for mode in ("native", "--run"):
                run(label, mode, image, {name, dll}, self_probe=False)

        for path, original in originals.items():
            if path.read_bytes() != original:
                failures.append(f"fixture bytes changed: {path.name}")
        hashes = {str(path.relative_to(root)): hashlib.sha256(original).hexdigest()
                  for path, original in originals.items()}
        result = dict(acp=page, children=len(rows), failures=failures, skips=skips,
                      source_sha256={name: hashlib.sha256(getattr(args, name).read_bytes()).hexdigest()
                                     for name in ("loader", "guest", "importer", "dependency")},
                      fixture_sha256=hashes, rows=rows)
        if args.report:
            _write_report_atomic(args.report, result)
    for skip in skips:
        print("SKIP", skip)
    print(f"module names: {len(rows)} native/mapped children, {len(failures)} failures, ACP {page}")
    return int(bool(failures))


if __name__ == "__main__":
    raise SystemExit(main())
