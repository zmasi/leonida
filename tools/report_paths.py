"""Do two report paths name the same file? One answer for the tools that refuse an output that would overwrite
an input or another output.

    from report_paths import same_report_path        # after sys.path.insert(0, <this folder>), as -I needs

When both resolved paths can be statted, their file identity takes priority over the name rules below.
On Windows, the name fallback checks the parent directory before treating case variants as aliases.

`same_report_path(first, second)` is true when the two paths resolve to one name (`realpath`, then `normcase`, and on a
case-insensitive macOS volume also case-folded and with composed and decomposed accents treated alike) or when both
exist and `os.path.samefile` says they are one file (a hard link, another spelling the name rules missed). Neither
path has to exist: two report paths that don't exist yet still collide when they differ only in case on a volume that
ignores case. Where the name rules cannot tell, `same_name_on_folding_volume` asks the volume itself with one
scratch folder: a Linux mount that folds case (vfat, exFAT, CIFS) answers neither `normcase` nor `pathconf`.
`find_report_path_collision` indexes possible aliases for a batch of inputs and outputs, then uses the same
pairwise check for the candidates.
Standard library only; no game data.
"""
from collections import defaultdict
from itertools import chain
import os
import posixpath
import sys
import tempfile
import unicodedata

_PC_CASE_SENSITIVE = 11  # <sys/unistd.h> on macOS; os.pathconf_names doesn't list it


def case_insensitive_volume(path):
    """True when the volume that will hold `path` ignores letter case, as macOS's default APFS volume does.
    os.path.normcase folds case only on Windows, so on macOS `R.json` and `r.json` would pass as two reports."""
    if sys.platform != "darwin":
        return False
    while not os.path.exists(path):
        parent = os.path.dirname(path)
        if parent == path:
            break
        path = parent
    try:
        return os.pathconf(path, _PC_CASE_SENSITIVE) == 0
    except (OSError, ValueError):
        return True  # the macOS default; folding can only refuse a pair, never overwrite one


def path_key(path):
    """A name two spellings of one path share: realpath, normcase, and on a case-insensitive volume case-folded NFD."""
    return _resolved_key(os.path.realpath(path))


def _resolved_key(path):
    """`path_key` for a path `os.path.realpath` has already resolved."""
    # Use the path rules of the platform we are resolving for. On macOS os.path is
    # posixpath and keeps case; on Windows ntpath lowercases. Resolving a simulated
    # macOS path through ntpath would hide the case-sensitive branch under test.
    path = posixpath.normcase(path) if sys.platform == "darwin" else os.path.normcase(path)
    if case_insensitive_volume(path):
        # APFS and HFS+ also treat composed and decomposed accents as one name.
        return unicodedata.normalize("NFD", path.casefold())
    return path


def same_name_on_folding_volume(first, second):
    """True when two names that differ only by case are one file on the volume that holds them.

    os.path.normcase folds case only on Windows and the `pathconf` probe above answers only on macOS, so a Linux
    mount that ignores case (vfat, exFAT, CIFS) would pass `R.json` and `r.json` as two reports and let the second
    one overwrite the first. Ask the volume itself: create one spelling in a scratch folder beside the reports and
    look for the other. Names that differ by more than case are never one file, so that costs nothing.
    """
    return _same_resolved_name(os.path.realpath(first), os.path.realpath(second))


def _folded_basename(path):
    return os.path.basename(path).casefold()


def _same_resolved_name(first, second):
    """`same_name_on_folding_volume` for paths `os.path.realpath` has already resolved."""
    if _folded_basename(first) != _folded_basename(second):
        return False
    first_dir, first_name = os.path.split(first)
    second_dir, second_name = os.path.split(second)
    try:
        if not os.path.samefile(first_dir, second_dir):
            return False
        with tempfile.TemporaryDirectory(prefix=".leonida-report-", dir=first_dir) as probe:
            with open(os.path.join(probe, first_name), "xb"):
                return os.path.exists(os.path.join(probe, second_name))
    except OSError:
        return False


def same_report_path(first, second):
    # Resolve each path once: the key and the volume probe both want the resolved name, and a guard runs once
    # per input file, so a second realpath per path is the whole cost of the check (see #6220's review).
    first, second = os.path.realpath(first), os.path.realpath(second)
    if first == second:
        return True
    try:
        # Case-sensitive Windows directories can have distinct files with the same folded name.
        return os.path.samefile(first, second)
    except OSError:
        if _resolved_key(first) == _resolved_key(second):
            return sys.platform != "win32" or _same_windows_folded_path(first, second)
        return _same_resolved_name(first, second)


def _possible_alias_keys(path):
    """Each way same_report_path can match must share at least one of these keys."""
    resolved = os.path.realpath(path)
    yield "resolved", resolved
    yield "normalized", _resolved_key(resolved)
    yield "basename", _folded_basename(resolved)
    try:
        stat = os.stat(resolved)
    except OSError:
        return
    # samefile compares these two stat fields. This also finds hard links with unrelated names.
    yield "identity", stat.st_dev, stat.st_ino


def find_report_path_collision(input_paths, output_paths):
    """Return (output, conflicting path, conflict is an input), or None for distinct destinations.

    Check outputs in order, comparing against inputs first and then earlier outputs. Index path data for this
    call only. A shared key makes a pair a candidate, and same_report_path decides whether it really aliases.
    """
    inputs = tuple(input_paths)
    known_paths = []
    indices_by_key = defaultdict(list)
    for index, path in enumerate(chain(inputs, output_paths)):
        keys = tuple(_possible_alias_keys(path))
        if index >= len(inputs):
            candidates = {i for key in keys for i in indices_by_key.get(key, ())}
            for i in sorted(candidates):
                other = known_paths[i]
                if same_report_path(path, other):
                    return path, other, i < len(inputs)
        known_paths.append(path)
        for key in keys:
            indices_by_key[key].append(index)
    return None


def _windows_directory_keeps_case(path):
    """Check the directory that will contain a component, or its nearest existing ancestor."""
    while True:
        try:
            with tempfile.TemporaryDirectory(prefix=".leonida-report-", dir=path) as probe:
                # Look up the temporary entry itself so the test uses the parent directory's case rules.
                try:
                    os.stat(os.path.join(path, os.path.basename(probe).upper()))
                except FileNotFoundError:
                    return True
                return False
        except FileNotFoundError:
            parent = os.path.dirname(path)
            if parent == path:
                return False
            path = parent
        except OSError:
            return False  # Keep the conservative folded match if the directory cannot be probed.


def _same_windows_folded_path(first, second):
    """Check each case difference in two absolute paths that have the same Windows name key."""
    first, second = os.fsdecode(first), os.fsdecode(second)
    while first != second:
        first, first_name = os.path.split(first)
        second, second_name = os.path.split(second)
        if not first_name:  # Drive and share names do not use per-directory case sensitivity.
            break
        if (first_name != second_name and os.path.normcase(first_name) == os.path.normcase(second_name)
                and _windows_directory_keeps_case(first)):
            return False
    return True
