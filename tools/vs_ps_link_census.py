r"""Census of the VS -> PS varying link: the pipelines whose producer's output order and pixel shader's input
order differ, so that Leonida's link by number and VIctory's link by signature row disagree.

Leonida links `exp paramN` of a vertex or mesh shader to Location N and a pixel shader's `v_interp attrA` to
Location A (docs/design.md, "Inputs and outputs" and "Pixel-shader interpolants"). VIctory links by signature row
(victory/crates/victory-shader/src/native.rs: VsOutput and PsInput, lines 37-48 and 203-241): the FSSM stage data
gives each param and each pixel input a row, and an input reads the output of its row. Both give the same pipeline
when every pixel input's attr number is the param of the producer's output with the same row. This tool counts the
pipelines where not ("by row"). As a cross-check it also joins the two through the signatures, where the row is a
register of the container's ISG1 / OSG1 (VIctory's reading): an input links to the output with its semantic (name in
any case and index; a system value by value and index), and differs by param number or by first component ("by
semantic"). Rows past every register of the signature, seen mostly in shaders with 16-bit elements, are counted:
they link by row only.

Per shader it reads, from a work folder's carve (`<work>/shaders/dxbc`, or that folder itself): the stage from
index.json, the OSG1 / ISG1 elements from `<file>` and the stage tables from `fssm/<name>.fssm`. Nothing is
translated, and spirv/index.json is not needed. Pipelines come from a pipeline manifest (`--manifest`,
tools/shader_link_check.py's format: "vs" or "ms", and "ps") or, by default, from the names: a producer and a pixel
shader whose names differ only in the stage token (VS_ or MS_ against PS_, as tools/user_sgpr_census.py --pairs
pairs a technique's stages). That approximates the pipelines the executable creates; it does not see them.

FSSM stage data, VIctory's layout (native.rs module docs). The stage's record is found through the slot table
(payload + 0x08 + 8 * slot for VS 4 and PS 5, payload + 0x48 for mesh; the payload starts at blob 0x10), and the
stage data follows it, E = record + 0x54:
  PS        E + 0x24: up to 32 records of 8 bytes (usage, row, mask, attr = the v_interp attr, default, second row
            of a packed 16-bit pair, parameter slot, flags); u32 count at E + 0x124 (tools/fssm_header.py reads it too).
  VS, mesh  E + 0xC8: up to 32 records of 6 bytes (usage, row, param = exp param<n> with bit 7 marking a mesh
            shader's per-primitive output, a row byte, slot, flags); u32 count at E + 0x188.
Only the first row of a record links here; the second row of a packed 16-bit pair is not followed.

Output: counts; `--list` adds the names of the pipelines that differ (a local report: shader names come from the
game and never go into the repository); `--json` writes the counts only.

Usage:
    python -I tools/vs_ps_link_census.py <work>/shaders/dxbc [--manifest pipelines.json] [--list] [--json OUT]

Exit status: 0 when every pipeline links the same both ways, 1 when some differ, 2 on unreadable input.
"""
import argparse
import json
import os
import re
import struct
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from report_paths import same_report_path  # noqa: E402

FSSM_MAGIC = b"FSSM"
PAYLOAD = 0x10
SLOT = {"vs": PAYLOAD + 0x08 + 8 * 4, "ps": PAYLOAD + 0x08 + 8 * 5, "ms": PAYLOAD + 0x48}
UNUSED_SLOT = 0xDEADBEEFDEADBEEF
STAGE_DATA = 0x54
PS_TABLE, PS_RECORD, PS_COUNT = 0x24, 8, 0x124
OUT_TABLE, OUT_RECORD, OUT_COUNT = 0xC8, 6, 0x188
MAX_RECORDS = 32
PRODUCERS = ("vs", "ms")
# "<optional one-character prefix>VS_<technique>_<8 hex digits>_Wrapped", MS_ or PS_ likewise; hash and suffix optional.
TECHNIQUE_RE = re.compile(r"^.?(VS|MS|PS)_(.+?)(?:_[0-9a-f]{8})?(?:_Wrapped)?$")


class CensusError(Exception):
    """An input that cannot be read: one ERROR line and exit 2."""


def signature(container, tag):
    """The elements of the container's `tag` part (b"ISG1" / b"OSG1") as dicts, or None when there is no such part.
    Element (32 bytes): stream, name offset (in the part), semantic index, system value, component type, register,
    mask byte, read/write mask byte, 2 pad, minimum precision."""
    if len(container) < 32 or container[:4] != b"DXBC":
        raise ValueError("not a DXBC container")
    count = struct.unpack_from("<I", container, 28)[0]
    for k in range(count):
        off = struct.unpack_from("<I", container, 32 + 4 * k)[0]
        if container[off:off + 4] != tag:
            continue
        size = struct.unpack_from("<I", container, off + 4)[0]
        part = container[off + 8:off + 8 + size]
        n, first = struct.unpack_from("<II", part, 0)
        out = []
        for i in range(n):
            at = first + 32 * i
            _, name_at, index, system, ctype, register = struct.unpack_from("<6I", part, at)
            end = part.index(b"\0", name_at)
            out.append({"name": part[name_at:end].decode("ascii", "replace"), "index": index, "system": system,
                        "type": ctype, "register": register, "mask": part[at + 24]})
        return out
    return None


def stage_records(blob, stage):
    """The FSSM stage table of a VS, mesh or pixel shader: [(usage, row, number)], number = the exp param of a
    producer (bit 7, a mesh shader's per-primitive mark, cleared) or the v_interp attr of a pixel input. ValueError
    for a blob that doesn't hold one."""
    if blob[:4] != FSSM_MAGIC or len(blob) < SLOT[stage] + 8:
        raise ValueError("not an FSSM blob")
    rel = struct.unpack_from("<Q", blob, SLOT[stage])[0]
    if rel == UNUSED_SLOT:
        raise ValueError(f"no {stage} record in the FSSM slot table")
    data = PAYLOAD + rel + STAGE_DATA
    table, size, count_at = (data + PS_TABLE, PS_RECORD, data + PS_COUNT) if stage == "ps" else \
        (data + OUT_TABLE, OUT_RECORD, data + OUT_COUNT)
    if len(blob) < count_at + 4:
        raise ValueError("FSSM stage data cut short")
    count = struct.unpack_from("<I", blob, count_at)[0]
    if count > MAX_RECORDS:
        raise ValueError(f"{count} stage records (at most {MAX_RECORDS})")
    out = []
    for i in range(count):
        r = blob[table + size * i:table + size * (i + 1)]
        number = r[3] if stage == "ps" else r[2] & 0x7F
        out.append((r[0], r[1], number))
    return out


def semantic(element):
    """What links an element across stages: a system value by its value and index, else name (any case) and index."""
    if element["system"]:
        return ("system", element["system"], element["index"])
    return (element["name"].upper(), element["index"])


def first_component(mask):
    return (mask & -mask).bit_length() - 1 if mask else None


class Shader:
    """One shader's links: `rows` {row: number}; `semantics` {semantic: (number, first component)} through the
    signature registers its rows name; `unresolved` the rows no signature register has."""

    def __init__(self, sig, records):
        self.records = records
        self.rows = {}
        for _, row, number in records:
            self.rows.setdefault(row, number)
        registers = {e["register"] for e in sig}
        self.unresolved = {row for row in self.rows if row not in registers}
        self.semantics = {}
        for e in sig:
            if e["register"] in self.rows:
                self.semantics.setdefault(semantic(e), (self.rows[e["register"]], first_component(e["mask"])))

    def in_row_order(self):
        """Whether it numbers its params (attrs) 0, 1, 2, ... in row order: then numbering by row or by param is the
        same within the shader (a pipeline can still differ, by the rows one of its shaders skips)."""
        numbers = [number for _, _, number in sorted(self.records, key=lambda r: r[1])]
        return numbers == list(range(len(numbers)))


def compare(producer, consumer):
    """Counter of the ways a pixel shader's inputs differ from its producer's outputs: by row ("row param": the
    output of the input's row has another number, "row missing": no output has that row) and through the
    signatures ("semantic param", "semantic component", "semantic missing"), plus the inputs whose row only links
    by row ("unresolved")."""
    found = Counter()
    for row, attr in consumer.rows.items():
        have = producer.rows.get(row)
        if have is None:
            found["row missing"] += 1
        elif have != attr:
            found["row param"] += 1
        if row in consumer.unresolved:
            found["unresolved"] += 1
    for sem, (attr, comp) in consumer.semantics.items():
        have = producer.semantics.get(sem)
        if have is None:
            found["semantic missing"] += 1
        elif have[0] != attr:
            found["semantic param"] += 1
        elif have[1] != comp:
            found["semantic component"] += 1
    return found


DIFFER = ("row param", "row missing", "semantic param", "semantic component", "semantic missing")


def technique(name):
    """(stage token, technique) of a shader name, or None."""
    m = TECHNIQUE_RE.match(name)
    return (m.group(1).lower(), m.group(2)) if m else None


def load_carve(directory):
    """{name: {"stage", "file"}} from the carve's index.json (a list of rows with name, file and stage; a translated
    index's {name: {"stage": ...}} works too)."""
    path = os.path.join(directory, "index.json")
    try:
        with open(path, encoding="utf-8") as f:
            rows = json.load(f)
    except (OSError, ValueError) as e:
        raise CensusError(f"cannot read {path}: {getattr(e, 'strerror', None) or e}") from None
    if isinstance(rows, dict):
        rows = [{"name": k, "file": k + ".dxbc", **v} for k, v in rows.items() if isinstance(v, dict)]
    if not isinstance(rows, list):
        raise CensusError(f"{path}: expected a list of shader rows")
    out = {}
    for row in rows:
        if isinstance(row, dict) and isinstance(row.get("name"), str) and row.get("stage") in PRODUCERS + ("ps",):
            out[row["name"]] = {"stage": row["stage"], "file": row.get("file") or row["name"] + ".dxbc"}
    return out


def read_shader(directory, name, entry):
    """Shader for one carve entry, or None when its container or FSSM blob is missing or unreadable."""
    try:
        with open(os.path.join(directory, entry["file"]), "rb") as f:
            container = f.read()
        with open(os.path.join(directory, "fssm", name + ".fssm"), "rb") as f:
            blob = f.read()
        stage = entry["stage"]
        sig = signature(container, b"ISG1" if stage == "ps" else b"OSG1")
        return Shader(sig or [], stage_records(blob, stage))
    except (OSError, ValueError, struct.error, IndexError):
        return None


def name_pipelines(shaders):
    """[(name, producer, pixel shader)] by technique name: every producer x pixel shader of one technique."""
    groups = {}
    for name, entry in shaders.items():
        t = technique(name)
        if t and t[0] == entry["stage"]:
            group = groups.setdefault(t[1], {"producers": [], "ps": []})
            group["ps" if entry["stage"] == "ps" else "producers"].append(name)
    out = []
    for tech in sorted(groups):
        for p in sorted(groups[tech]["producers"]):
            for c in sorted(groups[tech]["ps"]):
                out.append((f"{p} -> {c}", p, c))
    return out


def manifest_pipelines(path):
    """[(name, producer, pixel shader)] of a tools/shader_link_check.py manifest; pipelines without both skipped."""
    try:
        with open(path, encoding="utf-8-sig") as f:
            doc = json.load(f)
    except (OSError, ValueError) as e:
        raise CensusError(f"cannot read {path}: {getattr(e, 'strerror', None) or e}") from None
    rows = doc.get("pipelines") if isinstance(doc, dict) else None
    if not isinstance(rows, list):
        raise CensusError(f"{path}: expected {{\"pipelines\": [...]}}")
    out = []
    for n, row in enumerate(rows):
        if not isinstance(row, dict) or "ps" not in row or not any(s in row for s in PRODUCERS):
            continue
        ref = lambda r: r.get("shader") if isinstance(r, dict) else r      # noqa: E731
        producer, ps = ref(row.get("ms", row.get("vs"))), ref(row["ps"])
        if isinstance(producer, str) and isinstance(ps, str):
            out.append((str(row.get("name", f"pipelines[{n}]")), producer, ps))
    return out


def census(directory, pipelines=None):
    """{"pipelines": counts, "shaders": counts, "differ": [(name, counts)]} over the carve in `directory`."""
    shaders = load_carve(directory)
    pipelines = name_pipelines(shaders) if pipelines is None else pipelines
    cache, stats, per_shader, differ = {}, Counter(), Counter(), []

    def get(name):
        if name not in cache:
            entry = shaders.get(name)
            cache[name] = read_shader(directory, name, entry) if entry else None
            if cache[name] is not None:
                per_shader[entry["stage"]] += 1
                per_shader[f"{entry['stage']} in row order"] += cache[name].in_row_order()
                per_shader[f"{entry['stage']} with an unresolved row"] += bool(cache[name].unresolved)
        return cache[name]

    for name, producer, ps in pipelines:
        p, c = get(producer), get(ps)
        if p is None or c is None:
            stats["unreadable"] += 1
            continue
        stats["pipelines"] += 1
        stats["inputs"] += len(c.rows)
        found = compare(p, c)
        for kind, n in found.items():
            stats[f"inputs {kind}"] += n
        bad = {k: found[k] for k in DIFFER if found[k]}
        if bad:
            stats["differ"] += 1
            for kind in bad:
                stats[f"differ {kind}"] += 1
            differ.append((name, bad))
        else:
            stats["same"] += 1
    return {"pipelines": dict(stats), "shaders": dict(per_shader), "differ": differ}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("carve", help="<work>/shaders/dxbc (index.json, the .dxbc containers and fssm/)")
    ap.add_argument("--manifest", help="pipelines to check (tools/shader_link_check.py's manifest format)")
    ap.add_argument("--list", action="store_true", help="also print the pipelines that differ (a local report)")
    ap.add_argument("--json", help="write the counts (no names) here")
    a = ap.parse_args(argv)
    try:
        if a.json and (same_report_path(a.json, os.path.join(a.carve, "index.json"))
                       or a.manifest and same_report_path(a.json, a.manifest)):
            raise CensusError(f"--json {a.json} names an input")
        result = census(a.carve, manifest_pipelines(a.manifest) if a.manifest else None)
    except CensusError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 2
    s = result["pipelines"]
    g = lambda k: s.get(k, 0)                                                 # noqa: E731
    print(f"{g('pipelines')} pipelines read ({g('unreadable')} unreadable): {g('same')} link the same by number, "
          f"by row and by semantic, {g('differ')} differ")
    print(f"  by row: {g('differ row param')} pipelines with an input at another param, {g('differ row missing')} "
          f"with an input whose row the producer has not")
    print(f"  by semantic: {g('differ semantic param')} at another param, {g('differ semantic component')} in "
          f"other components, {g('differ semantic missing')} with an input the producer has not")
    print(f"  {g('inputs')} pixel inputs: by row {g('inputs row param')} at another param, {g('inputs row missing')} "
          f"without the row; by semantic {g('inputs semantic param')} at another param, "
          f"{g('inputs semantic component')} in other components, {g('inputs semantic missing')} without the "
          f"semantic; {g('inputs unresolved')} with a row no ISG1 register has (they link by row only)")
    for key, n in sorted(result["shaders"].items()):
        print(f"  shaders {key}: {n}")
    if a.list:
        for name, found in result["differ"]:
            print(f"  differs: {name}: " + ", ".join(f"{k} {v}" for k, v in sorted(found.items())))
    if a.json:
        try:
            with open(a.json, "w", encoding="utf-8", newline="\n") as f:
                json.dump({"pipelines": s, "shaders": result["shaders"]}, f, indent=1, sort_keys=True)
                f.write("\n")
        except OSError as e:
            print(f"ERROR: cannot write {a.json}: {e.strerror or e}", file=sys.stderr)
            return 2
    return 1 if s.get("differ") else 0


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError, OSError):
        pass
    sys.exit(main())
