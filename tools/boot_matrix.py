"""How far the retail executable boots for each named set of pull requests: one row per set.

Boot work on RT-5 is spread over many open pull requests, and the stop a contributor reports depends on which of
them were merged, in which order, onto which `main`, and on whether the shader library matched the translator. This
tool makes that explicit in three steps:

    python -I tools/boot_matrix.py merge --base origin/main --set rt5=445 --set rt5-565=445,565 \\
        --fetch origin --out <matrix folder>
    python -I tools/boot_matrix.py run <matrix folder> --set rt5 --tree <that set's checkout or export> \\
        --exe <game_scarlett_master.exe> --game-dir <package> --shaders <work>/shaders/spirv --build
    python -I tools/boot_matrix.py report <matrix folder>

`merge` (git only; nothing from a pull request is executed). For each set, in the order given, every PR is merged onto
the result so far with `git merge-tree --write-tree` and `git commit-tree`: no checkout, no hooks, the working tree is
not touched. A PR that `scripts/merge_check.py` finds already on the base (`already-merged`, the case where a stop
was fixed by a merge the stack predates) is skipped, and one that conflicts with the PRs before it is skipped with
its conflicted files listed. A PR merged and then reverted on the base is skipped as `already-merged` too: git merges
an ancestor as a no-op, so such a set does not have the PR's code; revert the revert on a branch and give it as
`N:REF` to test it. A set's branch that is checked out in the repository stops the merge before anything is
written. The result is the branch `boot-matrix/<set>` and `<out>/matrix.json`. A PR spec is `N` (the ref
`refs/boot-matrix/pr/N`, fetched from `pull/N/head` with `--fetch REMOTE`) or `N:REF` for a ref you have.
The merge commits use a fixed author and the base's commit date, so the same inputs give the same commit.

`run` builds and boots one set's tree: export or check out `boot-matrix/<set>` where you build PR code (another
machine, a separate user), and point `--tree` at it. The tree must hold the set's head: `merge` writes the blob id of
every file of that head to `<out>/<set>/files.json`, and `run` stops (exit 2) when a file is missing from the tree or
differs from it (CRLF line ends aside; files the head does not have, such as a build folder, are ignored). With
`--build` it configures and builds `runtime/` there, in a build folder of the head's own
(`runtime/build-boot-matrix-<head>`), so a binary built for another set is never reused; else `--gta6rt` names the
executable, and the run is recorded as built `prebuilt (unverified)`: nothing shows which source that binary came
from. It runs
`gta6rt --run --trace --trace-d3d12` with `LEONIDA_D3D12=vulkan` and `LEONIDA_SHADERS`, stops the process tree at
`--timeout`, and writes `<out>/<set>/runN/result.json`: the stop, the D3D12 call count and last call (summarised
with `tools/boot_trace.py`), and whether the shader library was translated by this tree's translator
(`translation_sources.sha256` that `make_game.py` writes beside it, against the same digest over the tree's
`TRANSLATION_SOURCES`). A library from another translator is the usual reason a
reported stop does not reproduce.

`report` prints a Markdown table (or `--json`) with the newest run of each set. Both carry only fields from a fixed
list, each checked for the form it should have, so they can be shared: the base and each set's head as commits, the
set names, each PR's number, status and why it was skipped, and of the run its build and shader state, the stop's kind,
exit or exception code, module+RVA and access kind, counts, the run's options (`LEONIDA_*` variable names, others
counted; timeout; whether `--overlay` was given), and the last D3D12 call as number, label and outcome. A label of
the shape the runtime writes is shown whole, one with free text after its name is cut to the name, anything else is
`(other)`; an outcome the runtime does not write is `other`, as are an exception code or access kind of another form.
Left out: the refs given for the base and the PRs (they can name local branches), merge_check's verdicts, conflicted
file names, the access address, the loader's error message (it can name a path), and a location outside every known
module (shown as `unmapped`). They stay in matrix.json, each run's `result.json` and the traces on your machine
(AGENTS.md, "No game data").

What a row says: `exit N` (the entry point returned), `running at timeout` (still going when stopped), `last
exception CODE at SITE` (the process ended without an exit event; exceptions are recorded before handlers run, so
the last one is where to look first, not proof of the cause), `error` (the loader threw; its message is in
`result.json`) or `no trace`.

Standard library only; needs git 2.38 or newer for `merge-tree --write-tree`. Exit 0 on success, 1 when a set had a
conflict, a build failed or a run did not reach a trace, 2 on a usage, git or file error.
"""
from __future__ import annotations

import argparse
import ast
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "scripts"))

import boot_trace  # noqa: E402
import merge_check  # noqa: E402

MATRIX = "matrix.json"
RESULT = "result.json"
FILES = "files.json"   # per set: the blob id of every file of its head, to check the tree a run builds
SHOWN_LIMIT = 5   # differing files named when a tree is refused
PR_REF = "refs/boot-matrix/pr/{}"
BRANCH = "boot-matrix/{}"
SET_NAME = re.compile(r"^(?!.*\.\.)(?!.*\.lock$)[A-Za-z0-9][A-Za-z0-9._-]*(?<!\.)$")
# A set's name is a folder: Windows refuses these device names (with any extension) as one.
RESERVED_NAMES = {"con", "prn", "aux", "nul", *(f"com{i}" for i in range(1, 10)), *(f"lpt{i}" for i in range(1, 10))}
MERGE_AUTHOR = {"GIT_AUTHOR_NAME": "boot_matrix", "GIT_AUTHOR_EMAIL": "boot-matrix@invalid",
                "GIT_COMMITTER_NAME": "boot_matrix", "GIT_COMMITTER_EMAIL": "boot-matrix@invalid"}
DEFAULT_TIMEOUT = 600   # seconds; the retail boot reaches its frame loop well within this on a desktop GPU
TAIL_LINES = 20
PREBUILT = "prebuilt (unverified)"
# What `report` passes on from a result.json and from its stop; everything else stays on the machine.
SHARED_RESULT = ("run", "head", "build", "shaders", "translator_matches_set", "d3d12_calls", "last_d3d12_method",
                 "seconds")   # and "options", reduced by shareable()
SHARED_STOP = ("kind", "code", "at", "access", "exceptions", "unimplemented", "returncode")
# A location a report may show: module+RVA, as the trace names it (`shims::module_offset`, also `host:<dll>+0x..`).
# An address outside every known module comes as a bare absolute address; a report says `unmapped` instead.
MODULE_SITE = re.compile(r"^(?:host:)?[^\s+\\/:]+\+0x[0-9a-fA-F]+$")   # a file name, never a path
# The labels the runtime writes in `[d3d12-trace]` lines: one or two identifier words and an optional
# `(device slot N)`. Checked against every label of three world-start runs (43 distinct).
D3D12_LABEL = re.compile(r"^[A-Za-z_][\w:]*(?: [A-Za-z_][\w:]*)?(?: \(device slot \d+\))?$")
IDENTIFIER = re.compile(r"^[A-Za-z_][\w:]*$")
EXCEPTION_CODE = re.compile(r"^0x[0-9a-fA-F]{1,8}$")
ACCESS_KINDS = ("read", "write", "execute")
PR_STATUSES = ("merged", "already-merged", "conflict")


class MatrixError(Exception):
    """A usage, git or file problem that stops the step (exit 2)."""


# --- set specs and git ---------------------------------------------------------------------------------------------

def parse_set(text: str) -> tuple[str, list[dict]]:
    """`name=7286,8517:refs/x` -> ("name", [{"pr": 7286, "ref": "refs/boot-matrix/pr/7286"}, ...]).

    `name=` is the base alone, the row the other sets are compared with."""
    name, sep, specs = text.partition("=")
    if not sep or not SET_NAME.match(name):
        raise argparse.ArgumentTypeError(f"expected NAME=PR[,PR...], got {text!r}")
    if name.split(".")[0].lower() in RESERVED_NAMES:
        raise argparse.ArgumentTypeError(f"set {name!r}: a name Windows keeps for a device cannot be a folder")
    if not specs:
        return name, []
    prs = []
    for spec in specs.split(","):
        number, _, ref = spec.strip().partition(":")
        number = number.lstrip("#")
        if not number.isdigit():
            raise argparse.ArgumentTypeError(f"{spec!r} in set {name!r}: expected N or N:REF")
        prs.append({"pr": int(number), "ref": ref or PR_REF.format(int(number))})
    if len({p["pr"] for p in prs}) != len(prs):
        raise argparse.ArgumentTypeError(f"set {name!r} names a PR twice")
    return name, prs


def git(repo: str, *args: str, env: dict | None = None, check: bool = True) -> subprocess.CompletedProcess:
    full_env = None if env is None else {**os.environ, **env}
    r = subprocess.run(["git", "-C", repo, *args], capture_output=True, env=full_env)
    r.stdout = r.stdout.decode("utf-8", "replace")
    r.stderr = r.stderr.decode("utf-8", "replace")
    if check and r.returncode != 0:
        raise MatrixError(f"git {' '.join(args)}: {r.stderr.strip() or r.returncode}")
    return r


def rev(repo: str, ref: str) -> str:
    r = git(repo, "rev-parse", "--verify", "--quiet", f"{ref}^{{commit}}", check=False)
    if r.returncode != 0:
        raise MatrixError(f"no commit for {ref!r} (fetch it, or give --fetch REMOTE)")
    return r.stdout.strip()


def fetch(repo: str, remote: str, numbers: list[int]) -> None:
    refspecs = [f"+pull/{n}/head:{PR_REF.format(n)}" for n in numbers]
    git(repo, "fetch", "--quiet", "--no-tags", remote, *refspecs)


# --- the translator digest make_game.py keeps beside the shader library ------------------------------------------

def translation_sources(make_game_text: str) -> tuple[str, ...]:
    """The TRANSLATION_SOURCES tuple of a make_game.py, read without running it."""
    for node in ast.parse(make_game_text).body:
        if isinstance(node, ast.Assign) and any(getattr(t, "id", None) == "TRANSLATION_SOURCES" for t in node.targets):
            names = ast.literal_eval(node.value)
            if isinstance(names, tuple) and all(isinstance(n, str) for n in names):
                return names
    raise MatrixError("make_game.py has no TRANSLATION_SOURCES tuple of file names")


def translation_digest(read) -> str:
    """make_game.translation_sources_digest() over files given by read(relative path) -> bytes."""
    names = translation_sources(read("make_game.py").decode("utf-8"))
    h = hashlib.sha256(b"")
    for name in names:
        data = read(f"tools/{name}")
        h.update(f"{name}\0{len(data)}\0".encode())
        h.update(data)
    return h.hexdigest()


def digest_of_commit(repo: str, commit: str) -> str:
    def read(path: str) -> bytes:
        r = subprocess.run(["git", "-C", repo, "cat-file", "blob", f"{commit}:{path}"], capture_output=True)
        if r.returncode != 0:
            raise MatrixError(f"{commit[:12]} has no {path}")
        return r.stdout
    return translation_digest(read)


def digest_of_tree(tree: str) -> str:
    def read(path: str) -> bytes:
        try:
            with open(os.path.join(tree, *path.split("/")), "rb") as f:
                return f.read()
        except OSError as e:
            raise MatrixError(f"cannot read {path} in {tree}: {e.strerror or e}") from e
    return translation_digest(read)


def shader_library_state(shaders: str, tree_digest: str) -> str:
    try:
        with open(os.path.join(shaders, "translation_sources.sha256"), encoding="utf-8") as f:
            recorded = f.read().strip()
    except OSError:
        return "unknown"
    return "current" if recorded == tree_digest else "stale"


# --- the files of a set's head, to check the tree a run builds -----------------------------------------------------

def head_files(repo: str, head: str) -> dict:
    """{"head", "object_format", "files": {path: blob id}} for the regular files of head (no links, no submodules)."""
    fmt = git(repo, "rev-parse", "--show-object-format", check=False).stdout.strip() or "sha1"
    files = {}
    for item in git(repo, "ls-tree", "-r", "-z", "--full-tree", head).stdout.split("\0"):
        meta, _, path = item.partition("\t")
        fields = meta.split()
        if len(fields) == 3 and fields[1] == "blob" and fields[0] in ("100644", "100755"):
            files[path] = fields[2]
    return {"head": head, "object_format": fmt, "files": files}


def blob_id(data: bytes, fmt: str) -> str:
    h = hashlib.new(fmt)
    h.update(b"blob %d\0" % len(data))
    h.update(data)
    return h.hexdigest()


def tree_differences(tree: str, manifest: dict) -> list[str]:
    """The head's files the tree lacks or holds with other content. A file written with CRLF line ends (a checkout or
    export with core.autocrlf) counts as the same; files the head does not have are not looked at."""
    fmt = manifest["object_format"]
    out = []
    for path, blob in sorted(manifest["files"].items()):
        try:
            with open(os.path.join(tree, *path.split("/")), "rb") as f:
                data = f.read()
        except OSError:
            out.append(f"{path} (missing)")
            continue
        if blob_id(data, fmt) == blob:
            continue
        if b"\r\n" in data and blob_id(data.replace(b"\r\n", b"\n"), fmt) == blob:
            continue
        out.append(f"{path} (differs)")
    return out


def verify_tree(matrix_dir: str, entry: dict, tree: str) -> None:
    """Stop unless tree holds the set's head, so a result is never filed under a head it was not built from."""
    path = os.path.join(matrix_dir, entry["name"], FILES)
    if not os.path.isfile(path):
        raise MatrixError(f"no {FILES} for set {entry['name']!r}: merge again (an older boot_matrix wrote this matrix)")
    manifest = read_json(path)
    if manifest.get("head") != entry["head"]:
        raise MatrixError(f"{path} is for another head than the set's {entry['head'][:12]}: merge again")
    differences = tree_differences(tree, manifest)
    if differences:
        shown = ", ".join(differences[:SHOWN_LIMIT])
        if len(differences) > SHOWN_LIMIT:
            shown += f" and {len(differences) - SHOWN_LIMIT} more"
        raise MatrixError(f"{tree} is not set {entry['name']}'s head {entry['head'][:12]} ({shown}); check out or "
                          f"export {entry['branch']} there")


# --- merge ---------------------------------------------------------------------------------------------------------

def merge_set(repo: str, base: str, name: str, prs: list[dict], checks: dict, env: dict) -> dict:
    head = base
    rows = []
    for pr in prs:
        row = {"pr": pr["pr"], "ref": pr["ref"], "sha": pr["sha"], "verdict": checks[pr["pr"]]["verdict"],
               "duplicates": sum(checks[pr["pr"]]["duplicates"].values()),
               "conflicts_with_base": checks[pr["pr"]]["conflict"], "conflict_files": []}
        if row["verdict"] == "already-merged":
            # Its commits are on the base, so a merge would add nothing; that holds for a PR merged and then
            # reverted on the base too (git merges an ancestor as a no-op), and such a set does not have its code.
            row["status"] = "already-merged"
            rows.append(row)
            continue
        r = git(repo, "merge-tree", "--write-tree", "--name-only", "--no-messages", head, pr["sha"], check=False)
        lines = [line for line in r.stdout.splitlines() if line]
        if r.returncode == 1:
            row["status"] = "conflict"
            row["conflict_files"] = lines[1:]
        elif r.returncode != 0 or not lines:
            raise MatrixError(f"git merge-tree for #{pr['pr']} in set {name}: {r.stderr.strip() or r.returncode}")
        else:
            message = f"boot_matrix: merge #{pr['pr']} into set {name}"
            head = git(repo, "commit-tree", lines[0], "-p", head, "-p", pr["sha"], "-m", message,
                       env=env).stdout.strip()
            row["status"] = "merged"
        rows.append(row)
    branch = BRANCH.format(name)
    git(repo, "update-ref", f"refs/heads/{branch}", head)
    return {"name": name, "branch": branch, "head": head, "prs": rows,
            "translation_digest": digest_of_commit(repo, head)}


def cmd_merge(a: argparse.Namespace) -> int:
    repo = os.path.abspath(a.repo)
    sets = dict(a.set)
    if len({name.lower() for name in sets}) != len(a.set):
        raise MatrixError("two sets share a name (names are compared without case: they are folders too)")
    # Before anything is written: a set's branch checked out here would move under its own working tree.
    checked_out = git(repo, "symbolic-ref", "-q", "HEAD", check=False).stdout.strip()
    for name in sets:
        if checked_out == f"refs/heads/{BRANCH.format(name)}":
            raise MatrixError(f"{BRANCH.format(name)} is checked out in {repo}: merging would move it under that "
                              "working tree; check out another branch there first")
    numbers = sorted({p["pr"] for prs in sets.values() for p in prs if p["ref"] == PR_REF.format(p["pr"])})
    if a.fetch and numbers:
        fetch(repo, a.fetch, numbers)
    base = rev(repo, a.base)
    for prs in sets.values():
        for pr in prs:
            pr["sha"] = rev(repo, pr["ref"])
    unique = {}
    for prs in sets.values():
        for pr in prs:
            if unique.setdefault(pr["pr"], pr["sha"]) != pr["sha"]:
                raise MatrixError(f"#{pr['pr']} is given two different refs; use one per PR number")
    try:
        history = merge_check.prior_merges(repo, base, unique, a.depth)
        checks = {n: merge_check.check(repo, base, n, sha, a.min_length, history[n]) for n, sha in unique.items()}
    except merge_check.GitError as e:
        raise MatrixError(str(e)) from e
    date = git(repo, "show", "-s", "--format=%cI", base).stdout.strip()
    env = {**MERGE_AUTHOR, "GIT_AUTHOR_DATE": date, "GIT_COMMITTER_DATE": date}
    matrix = {"tool": "boot_matrix", "version": 1, "base": {"ref": a.base, "sha": base},
              "sets": [merge_set(repo, base, name, prs, checks, env) for name, prs in sets.items()]}
    os.makedirs(a.out, exist_ok=True)
    for s in matrix["sets"]:
        os.makedirs(os.path.join(a.out, s["name"]), exist_ok=True)
        write_json(os.path.join(a.out, s["name"], FILES), head_files(repo, s["head"]))
    write_json(os.path.join(a.out, MATRIX), matrix)
    for s in matrix["sets"]:
        merged = sum(1 for p in s["prs"] if p["status"] == "merged")
        skipped = [f"#{p['pr']} {skip_reason(p)}" for p in s["prs"] if p["status"] != "merged"]
        print(f"{s['name']}: {s['branch']} {s['head'][:12]}, {merged}/{len(s['prs'])} merged"
              + (f"; skipped {', '.join(skipped)}" if skipped else ""))
    conflicts = any(p["status"] == "conflict" for s in matrix["sets"] for p in s["prs"])
    return 1 if conflicts else 0


# --- run -----------------------------------------------------------------------------------------------------------

def read_json(path: str) -> dict:
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError) as e:
        raise MatrixError(f"cannot read {path}: {e}") from e


def write_json(path: str, data: dict) -> None:
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, indent=1)
        f.write("\n")


def find_set(matrix: dict, name: str) -> dict:
    for s in matrix["sets"]:
        if s["name"] == name:
            return s
    raise MatrixError(f"no set {name!r} in the matrix ({', '.join(s['name'] for s in matrix['sets'])})")


def next_run_dir(set_dir: str) -> str:
    os.makedirs(set_dir, exist_ok=True)
    n = 1
    while os.path.exists(os.path.join(set_dir, f"run{n}")):
        n += 1
    path = os.path.join(set_dir, f"run{n}")
    os.makedirs(path)
    return path


def tail(path: str, lines: int = TAIL_LINES) -> list[str]:
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()[-lines:]
    except OSError:
        return []


def build_dir(tree: str, head: str) -> str:
    """Where `run --build` builds a set's head: a folder of that head's own. An export of another set unpacked over
    the same tree keeps the commit date of the base in every file, older than the objects already built, so a shared
    build folder would hand back the earlier set's gta6rt.exe as built from this one."""
    return os.path.join(tree, "runtime", f"build-boot-matrix-{head[:12]}")


def build_runtime(tree: str, build: str, log_path: str, config: str,
                  cmake_args: list[str]) -> tuple[bool, str | None]:
    """Configure and build gta6rt (and the TLS host DLL it depends on) in `build`."""
    steps = (["cmake", "-S", os.path.join(tree, "runtime"), "-B", build, *cmake_args],
             ["cmake", "--build", build, "--config", config, "--target", "gta6rt"])
    with open(log_path, "w", encoding="utf-8", newline="\n") as log:
        for step in steps:
            log.write("$ " + " ".join(step) + "\n")
            log.flush()
            try:
                r = subprocess.run(step, stdout=log, stderr=subprocess.STDOUT)
            except OSError as e:
                log.write(f"cannot start: {e}\n")
                return False, None
            if r.returncode != 0:
                return False, None
    return True, find_gta6rt(build, config)


def find_gta6rt(build: str, config: str) -> str | None:
    """A single-config generator (Ninja) puts it in the build folder, a multi-config one under <config>."""
    for path in (os.path.join(build, "gta6rt.exe"), os.path.join(build, config, "gta6rt.exe")):
        if os.path.isfile(path):
            return path
    return None


def stop_tree(proc: subprocess.Popen) -> None:
    """End the process and its children (gta6rt restarts itself suspended to reserve the title's base)."""
    if os.name == "nt":
        subprocess.run(["taskkill", "/T", "/F", "/PID", str(proc.pid)], capture_output=True)
    if proc.poll() is None:
        proc.kill()
    try:
        proc.wait(timeout=30)
    except subprocess.TimeoutExpired:
        pass


def boot(command: list[str], run_dir: str, env: dict, timeout: float) -> dict:
    trace = os.path.join(run_dir, "boot.jsonl")
    d3d12 = os.path.join(run_dir, "d3d12.log")
    argv = [*command, "--trace", trace, "--trace-d3d12"]
    started = time.monotonic()
    with open(d3d12, "wb") as err, open(os.path.join(run_dir, "stdout.log"), "wb") as out:
        proc = subprocess.Popen(argv, stdout=out, stderr=err, env={**os.environ, **env}, cwd=run_dir)
        timed_out = False
        try:
            proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            stop_tree(proc)
        except BaseException:   # Ctrl-C: do not leave gta6rt and its suspended child running
            stop_tree(proc)
            raise
    return {"returncode": None if timed_out else proc.returncode, "timed_out": timed_out,
            "seconds": round(time.monotonic() - started, 1), "trace": trace, "d3d12": d3d12}


def shared_call(call: str) -> str:
    """A trace line's call label as a report may show it. Most labels are fixed names (`d3d12_trace.h`):
    `ID3D12Device::CreateCommittedResource`, `XboxIDXGIDevice::GetAdapter`, `Xbox CreatePlacedResourceX (device
    slot 133)`; one of that shape is shown whole. Others carry free text after the name (`PipelineStream::Refused
    <reason>`) and are cut to the name; text that does not start with a name at all is `(other)`."""
    if D3D12_LABEL.match(call):
        return call
    words = call.split()
    return words[0] if words and IDENTIFIER.match(words[0]) else "(other)"


def shared_outcome(outcome: str) -> str:
    """A trace line's outcome as a report may show it: one of the outcomes the runtime writes, else `other`."""
    return outcome if outcome in boot_trace.D3D12_OUTCOMES else "other"


def last_d3d12_call(path: str) -> tuple[str | None, str | None]:
    """The last D3D12 trace line whole (`#N label result`, kept in result.json) and as a report shows it
    (`#N label result` with the label passed through `shared_call`)."""
    last = None
    try:
        with boot_trace._open_text(path) as fh:
            for line in fh:
                m = boot_trace.D3D12_LINE.match(line.rstrip("\n"))
                if m:
                    last = m
    except boot_trace.TraceError:
        return None, None
    if last is None:
        return None, None
    seq, call, outcome = last.groups()
    return f"#{seq} {call} {outcome}", f"#{seq} {shared_call(call)} {shared_outcome(outcome)}"


def classify(startup: dict | None, timed_out: bool, returncode: int | None) -> dict:
    """The stop of one run, from the startup summary and how the process ended."""
    if startup is None:
        return {"kind": "no trace", "returncode": returncode}
    stop = {"exceptions": startup["exception_count"], "unimplemented": len(startup["unimplemented"])}
    last = startup["exceptions"][-1] if startup["exceptions"] else None
    if startup["exit"] is not None:
        stop.update(kind="exit", code=startup["exit"])
    elif startup["error"]:
        stop.update(kind="error", what=startup["error"], returncode=returncode)
    elif timed_out:
        stop.update(kind="running at timeout")
    elif last:
        stop.update(kind="last exception", code=last["code"], at=last["at"], access=last.get("access"),
                    address=last.get("address"), returncode=returncode)
    else:
        stop.update(kind="ended without exit event", returncode=returncode)
    return stop


def summarize_run(outcome: dict) -> dict:
    try:
        startup = boot_trace.parse_startup(outcome["trace"])
    except boot_trace.TraceError:
        startup = None
    try:
        calls = boot_trace.parse_d3d12(outcome["d3d12"])["calls"]
    except boot_trace.TraceError:
        calls = 0
    last, method = last_d3d12_call(outcome["d3d12"])
    return {"stop": classify(startup, outcome["timed_out"], outcome["returncode"]), "d3d12_calls": calls,
            "last_d3d12_call": last, "last_d3d12_method": method, "seconds": outcome["seconds"]}


def cmd_run(a: argparse.Namespace) -> int:
    a.matrix = os.path.abspath(a.matrix)   # gta6rt runs in the run folder, so every path it gets is absolute
    matrix = read_json(os.path.join(a.matrix, MATRIX))
    entry = find_set(matrix, a.set)
    tree = os.path.abspath(a.tree)
    verify_tree(a.matrix, entry, tree)
    tree_digest = digest_of_tree(tree)
    run_dir = next_run_dir(os.path.join(a.matrix, a.set))
    result = {"set": a.set, "head": entry["head"], "run": os.path.basename(run_dir),
              "translator_matches_set": tree_digest == entry["translation_digest"],
              "shaders": shader_library_state(a.shaders, tree_digest), "build": PREBUILT,
              # What else decides the stop, so two runs of a set can be told apart (variable names only).
              "options": {"env": sorted(name for name, _ in a.env), "timeout": a.timeout,
                          "overlay": "given" if a.overlay else "fresh", "config": a.config if a.build else None,
                          "cmake_args": len(a.cmake_arg) if a.build else 0}}
    gta6rt = a.gta6rt
    if a.build:
        ok, built = build_runtime(tree, build_dir(tree, entry["head"]), os.path.join(run_dir, "build.log"),
                                  a.config, a.cmake_arg)
        result["build"] = "ok" if ok and built else "failed"
        if result["build"] == "failed":
            result["build_tail"] = tail(os.path.join(run_dir, "build.log"))
            write_json(os.path.join(run_dir, RESULT), result)
            print(f"{a.set}: build failed, see {os.path.join(run_dir, 'build.log')}")
            return 1
        gta6rt = built
    if not gta6rt or not os.path.isfile(gta6rt):
        raise MatrixError("give --build or --gta6rt <path to gta6rt.exe>")
    # A fresh overlay (unless --overlay names one) and save folder per run: what an earlier run wrote must not change
    # this boot.
    overlay = os.path.abspath(a.overlay) if a.overlay else os.path.join(run_dir, "overlay")
    save = os.path.join(run_dir, "save")
    os.makedirs(overlay, exist_ok=True)
    os.makedirs(save, exist_ok=True)
    env = {"LEONIDA_D3D12": "vulkan", "LEONIDA_SHADERS": os.path.abspath(a.shaders), "LEONIDA_SAVE_DIR": save}
    env.update(dict(a.env))
    command = [os.path.abspath(gta6rt), "--run", os.path.abspath(a.exe), "--game-dir", os.path.abspath(a.game_dir),
               "--overlay", overlay]
    result.update(summarize_run(boot(command, run_dir, env, a.timeout)))
    write_json(os.path.join(run_dir, RESULT), result)
    print(f"{a.set} {result['run']}: {format_stop(result['stop'])}; {result['d3d12_calls']} D3D12 calls"
          f"; shaders {result['shaders']}")
    return 1 if result["stop"]["kind"] == "no trace" else 0


# --- report --------------------------------------------------------------------------------------------------------

def format_stop(stop: dict) -> str:
    """One line for a stop, without the access address or the loader's message (both stay in result.json)."""
    kind = stop["kind"]
    if kind == "exit":
        return f"exit {stop['code']}"
    if kind == "last exception":
        text = f"last exception {stop['code']} at {stop['at']}"
        return text + (f" ({stop['access']})" if stop.get("access") else "")
    if kind == "error":
        return "error (the loader's message is in result.json)"
    if stop.get("returncode") is not None:
        return f"{kind} (process exit {stop['returncode']})"
    return kind


def latest_result(matrix_dir: str, name: str, head: str) -> dict | None:
    """The newest run of the set's current head; runs of an earlier merge of the same name are ignored."""
    set_dir = os.path.join(matrix_dir, name)
    runs = []
    if os.path.isdir(set_dir):
        for entry in os.listdir(set_dir):
            m = re.fullmatch(r"run(\d+)", entry)
            if m and os.path.isfile(os.path.join(set_dir, entry, RESULT)):
                runs.append((int(m.group(1)), entry))
    for _, entry in sorted(runs, reverse=True):
        result = read_json(os.path.join(set_dir, entry, RESULT))
        if result.get("head") == head:
            return result
    return None


def skip_reason(pr: dict) -> str:
    """Why a PR is not in the set's head: already on the base, or which merge it conflicts with."""
    if pr["status"] != "conflict":
        return pr["status"]
    return "conflicts with base" if pr.get("conflicts_with_base") else "conflicts with an earlier PR"


def pr_cell(prs: list[dict], status: str) -> str:
    return " ".join(f"#{p['pr']}" for p in prs if p["status"] == status) or "-"


def shareable(result: dict | None) -> dict | None:
    """The fields of a result that a report passes on (SHARED_RESULT, and SHARED_STOP of its stop)."""
    if result is None:
        return None
    out = {key: result[key] for key in SHARED_RESULT if key in result}
    if "stop" in result:
        stop = {key: value for key, value in result["stop"].items() if key in SHARED_STOP}
        if "at" in stop:
            stop["at"] = shared_site(stop["at"])
        if "code" in stop:   # an exit code (a number) or an exception code (hex); the trace's own text otherwise
            code = stop["code"]
            ok = (isinstance(code, int) and not isinstance(code, bool)) or (
                isinstance(code, str) and EXCEPTION_CODE.match(code))
            stop["code"] = code if ok else "other"
        if "access" in stop and stop["access"] is not None and stop["access"] not in ACCESS_KINDS:
            stop["access"] = "other"
        for key in ("exceptions", "unimplemented", "returncode"):
            if key in stop and not (stop[key] is None or (isinstance(stop[key], int) and
                                                           not isinstance(stop[key], bool))):
                stop[key] = "other"
        out["stop"] = stop
    if isinstance(result.get("options"), dict):
        env = result["options"].get("env") or []
        # Variable names only; a name of the runtime's own family as it is, others counted.
        named = sorted(n for n in env if isinstance(n, str) and re.fullmatch(r"LEONIDA_[A-Z0-9_]+", n))
        others = len(env) - len(named)
        out["options"] = {"env": named + ([f"({others} other)"] if others else []),
                          "timeout": result["options"].get("timeout"),
                          "overlay": "given" if result["options"].get("overlay") == "given" else "fresh"}
    return out


def shared_prs(prs: list[dict]) -> list[dict]:
    """A set's PR rows as a report may show them: number, status and why it was skipped. The refs given for them
    (`N:REF` can name a local branch), shas, merge_check's verdict and conflicted file names stay in matrix.json."""
    rows = []
    for p in prs:
        status = p.get("status") if p.get("status") in PR_STATUSES else "other"
        row = {"pr": p["pr"], "status": status}
        if status != "merged":
            row["skipped"] = skip_reason({**p, "status": status})
        rows.append(row)
    return rows


def shared_site(at: object) -> str:
    """A trace location as a report may show it: module+RVA as it is, anything else (a bare address) `unmapped`."""
    return at if isinstance(at, str) and MODULE_SITE.match(at) else "unmapped"


def report_rows(matrix_dir: str) -> tuple[dict, list[dict]]:
    matrix = read_json(os.path.join(matrix_dir, MATRIX))
    rows = []
    for s in matrix["sets"]:
        result = shareable(latest_result(matrix_dir, s["name"], s["head"]))
        rows.append({"set": s["name"], "head": s["head"], "prs": shared_prs(s["prs"]), "result": result})
    return matrix, rows


def format_markdown(matrix: dict, rows: list[dict]) -> str:
    # The base as its commit only: the ref given for it can name a local branch or remote.
    out = [f"Base `{matrix['base']['sha'][:12]}`.", "",
           "| Set | Head | Merged | Skipped | Build | Shaders | D3D12 calls | Last D3D12 call | Stop |",
           "|---|---|---|---|---|---|---|---|---|"]
    for row in rows:
        skipped = " ".join(f"#{p['pr']} ({p['skipped']})" for p in row["prs"] if p["status"] != "merged") or "-"
        r = row["result"]
        if r is None:
            run = ["not run"] + ["-"] * 4
        else:
            last = f"`{r['last_d3d12_method']}`" if r.get("last_d3d12_method") else "-"
            stop = format_stop(r["stop"]) if "stop" in r else "-"
            run = [r["build"], r["shaders"], str(r.get("d3d12_calls", "-")), last, stop]
        cells = [row["set"], f"`{row['head'][:12]}`", pr_cell(row["prs"], "merged"), skipped, *run]
        out.append("| " + " | ".join(cell.replace("|", "\\|") for cell in cells) + " |")
    return "\n".join(out)


def cmd_report(a: argparse.Namespace) -> int:
    matrix, rows = report_rows(a.matrix)
    if a.json:
        print(json.dumps({"base": {"sha": matrix["base"]["sha"]}, "rows": rows}, indent=1))
    else:
        print(format_markdown(matrix, rows))
    return 0


# --- command line --------------------------------------------------------------------------------------------------

def parse_env(text: str) -> tuple[str, str]:
    key, sep, value = text.partition("=")
    if not sep or not key:
        raise argparse.ArgumentTypeError(f"expected NAME=VALUE, got {text!r}")
    return key, value


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    sub = ap.add_subparsers(dest="command", required=True)

    m = sub.add_parser("merge", help="merge each set onto the base (git only)")
    m.add_argument("--base", required=True, help="the commit every set starts from, e.g. origin/main")
    m.add_argument("--set", action="append", required=True, type=parse_set, metavar="NAME=PR[,PR...]",
                   help="a named set, merged in the order given; a PR is N or N:REF (repeat for more sets)")
    m.add_argument("--fetch", metavar="REMOTE", help="fetch pull/N/head for each PR given as N from this remote")
    m.add_argument("--repo", default=".", help="repository folder (default: the current one)")
    m.add_argument("--out", required=True, help="the matrix folder (matrix.json, later the runs)")
    m.add_argument("--min-length", type=int, default=40, help="merge_check: shortest duplicate line counted (40)")
    m.add_argument("--depth", type=int, default=5000, help="merge_check: first-parent commits searched (5000)")
    m.set_defaults(func=cmd_merge)

    r = sub.add_parser("run", help="build and boot one set's tree")
    r.add_argument("matrix", help="the matrix folder written by merge")
    r.add_argument("--set", required=True, help="the set's name")
    r.add_argument("--tree", required=True, help="a checkout or export of the set's branch (checked against its head)")
    r.add_argument("--exe", required=True, help="game_scarlett_master.exe")
    r.add_argument("--game-dir", required=True, help="the package folder")
    r.add_argument("--shaders", required=True, help="<work>/shaders/spirv, passed as LEONIDA_SHADERS")
    r.add_argument("--build", action="store_true", help="configure and build runtime/ in the tree first")
    r.add_argument("--config", default="RelWithDebInfo", help="build configuration (RelWithDebInfo)")
    r.add_argument("--cmake-arg", action="append", default=[], metavar="ARG",
                   help="extra configure argument for --build, e.g. -GNinja or -DLEONIDA_VULKAN_HEADERS_FETCH=OFF "
                        "on a machine without network (repeatable; use --cmake-arg=-G... for a leading dash)")
    r.add_argument("--gta6rt", help=f"an already built gta6rt.exe (instead of --build; recorded as {PREBUILT})")
    r.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT, help=f"seconds before the run is stopped "
                   f"({DEFAULT_TIMEOUT})")
    r.add_argument("--overlay", help="gta6rt's --overlay (default: a fresh <run folder>/overlay)")
    r.add_argument("--env", action="append", default=[], type=parse_env, metavar="NAME=VALUE",
                   help="extra environment for gta6rt, e.g. LEONIDA_SKIP_INT3=1 (repeatable)")
    r.set_defaults(func=cmd_run)

    p = sub.add_parser("report", help="the newest run of each set as a Markdown table")
    p.add_argument("matrix", help="the matrix folder")
    p.add_argument("--json", action="store_true", help="JSON instead of Markdown")
    p.set_defaults(func=cmd_report)
    return ap


def main(argv: list[str] | None = None) -> int:
    a = build_parser().parse_args(argv)
    if getattr(a, "timeout", 1) <= 0:
        print("boot_matrix: --timeout must be positive", file=sys.stderr)
        return 2
    if shutil.which("git") is None and a.command == "merge":
        print("boot_matrix: git is not on PATH", file=sys.stderr)
        return 2
    try:
        return a.func(a)
    except MatrixError as e:
        print(f"boot_matrix: {e}", file=sys.stderr)
        return 2
    except OSError as e:   # an unwritable matrix folder, a gta6rt or exe that cannot be started
        print(f"boot_matrix: {e.filename or ''}: {e.strerror or e}", file=sys.stderr)
        return 2
    except (KeyError, TypeError) as e:   # a matrix.json or result.json this version did not write
        print(f"boot_matrix: unexpected matrix or result file ({type(e).__name__}: {e})", file=sys.stderr)
        return 2


if __name__ == "__main__":
    try:  # a redirected stdout is strict ANSI code page under -I; write '?' for what it cannot encode
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError, OSError):
        pass
    sys.exit(main())
