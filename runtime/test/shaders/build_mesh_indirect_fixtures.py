#!/usr/bin/env python3
"""Stage the shader library of d3d12_mesh_compute_indirect_test.

    python3 -I runtime/test/shaders/build_mesh_indirect_fixtures.py <mesh-compute dir> <out dir>
        --ps-cell <ps_mesh_indirect_cell.spv> --cs-args <cs_mesh_indirect_args.spv>

<mesh-compute dir> is what build_mesh_compute.py staged (spirv/index.json, spirv/*.spv, fssm/*.fssm); it is copied
unchanged into <out dir>, then these are added (every one a synthetic, hand-written fixture; no game data):
  * ms_v5_wide: ms_v5_workgroup.asm again, translated with --mesh-compute and a PSV0 that declares 64 vertices and
    64 primitives. The code is the same three-vertex triangle, but its output slot is ~3 KiB instead of ~0.1 KiB,
    so one 65 536-group record cannot fit one scratch allocation: it needs several tiles.
  * ms_v5_legacy: the sealed ms_v5_workgroup producer without its OpModuleProcessed
    "leonida.mesh_indirect_tile.v1", re-sealed. That is what a producer translated before the tile ABI is: it
    still runs a direct DispatchMesh, and a positive ExecuteIndirect must refuse it.
  * ms_v5_badmarker: the same with the marker string altered to "...tile.v2" (same word count), which must not
    count as the v1 marker either.
  * ps_mesh_indirect_cell, cs_mesh_indirect_args: the glslangValidator-built GLSL modules and their index entries.
The derived producers share the companion and layout of ms_v5_workgroup; only producer_sha256 of their copied
seal changes. Standard library only besides the repo's own tools.
"""

import copy
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import build_mesh_compute as bmc  # noqa: E402  (puts tools/ on sys.path)

MARKER = "leonida.mesh_indirect_tile.v1"
BAD_MARKER = "leonida.mesh_indirect_tile.v2"
OP_MODULE_PROCESSED = 330


def literal(text):
    raw = text.encode("utf-8") + b"\0"
    raw += b"\0" * (-len(raw) % 4)
    return struct.unpack("<%dI" % (len(raw) // 4), raw)


def rewrite_marker(spv_bytes, replacement):
    """The module with its exact OpModuleProcessed MARKER removed (replacement None) or replaced by `replacement`
    (same word count). Fails unless there is exactly one."""
    words = list(struct.unpack("<%dI" % (len(spv_bytes) // 4), spv_bytes))
    out = words[:5]
    pos, found = 5, 0
    want = literal(MARKER)
    while pos < len(words):
        head = words[pos]
        count, op = head >> 16, head & 0xFFFF
        if count == 0 or pos + count > len(words):
            raise ValueError("malformed SPIR-V")
        if op == OP_MODULE_PROCESSED and tuple(words[pos + 1:pos + count]) == want:
            found += 1
            if replacement is not None:
                new = literal(replacement)
                if len(new) != len(want):
                    raise ValueError("replacement marker changes the word count")
                out.append(head)
                out.extend(new)
        else:
            out.extend(words[pos:pos + count])
        pos += count
    if found != 1:
        raise ValueError(f"expected exactly one OpModuleProcessed {MARKER!r}, found {found}: "
                         "the translator did not emit the mesh indirect tile marker")
    return struct.pack("<%dI" % len(out), *out)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def write_json(path, value, **kw):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(value, f, **kw)
        f.write("\n")


def main(argv):
    if any(a in ("-h", "--help") for a in argv[1:]):
        print(__doc__)
        return 0
    args, glsl, i = [], {}, 1
    while i < len(argv):
        if argv[i] in ("--ps-cell", "--cs-args"):
            if i + 1 >= len(argv):
                print(f"ERROR: {argv[i]} needs a path", file=sys.stderr)
                return 2
            glsl[argv[i][2:]] = argv[i + 1]
            i += 2
        else:
            args.append(argv[i])
            i += 1
    if len(args) != 2 or sorted(glsl) != ["cs-args", "ps-cell"]:
        print(__doc__, file=sys.stderr)
        return 2
    base, out = Path(args[0]), Path(args[1])
    for key, path in glsl.items():
        if not os.path.isfile(path):
            print(f"ERROR: --{key} {path}: not a file", file=sys.stderr)
            return 2
    if os.environ.get("LEONIDA_SPIRV_TOOLS"):
        os.environ["PATH"] = os.environ["LEONIDA_SPIRV_TOOLS"] + os.pathsep + os.environ.get("PATH", "")
    llvm_mc = bmc.mc_command.mc_command()
    if out.resolve() == base.resolve():
        print("ERROR: <out dir> must differ from <mesh-compute dir>", file=sys.stderr)
        return 2
    if out.exists():
        shutil.rmtree(out)
    shutil.copytree(base / "spirv", out / "spirv")
    shutil.copytree(base / "fssm", out / "fssm")
    spirv, fssm = out / "spirv", out / "fssm"
    with open(spirv / "index.json", encoding="utf-8") as f:
        index = json.load(f)

    # ms_v5_wide: the same assembly, a bigger declared output.
    with tempfile.TemporaryDirectory(prefix="staging-", dir=out) as tmp:
        work = Path(tmp)
        disasm, sidecars, wide = work / "disasm", work / "sidecars", work / "spirv"
        disasm.mkdir()
        sidecars.mkdir()
        name = "ms_v5_wide"
        (disasm / (name + ".s")).write_text(
            bmc.assemble_to_s(llvm_mc, os.path.join(HERE, "ms_v5_workgroup.asm"), name),
            encoding="utf-8", newline="\n")
        (work / (name + ".dxbc")).write_bytes(
            bmc.make_dxbc([(b"PSV0", bmc.make_ms_psv0(32, 64, 64, group_shared=128))]))
        (sidecars / (name + ".fssm")).write_bytes(bmc.fssm_marker(name))
        cmd = [sys.executable, "-I", bmc.TRANSLATOR, str(disasm), "--out", str(wide),
               "--fssm-dir", str(sidecars), "--mesh-compute"]
        r = subprocess.run(cmd, capture_output=True, text=True)
        print(r.stdout, end="")
        if r.returncode != 0:
            print(r.stderr, end="", file=sys.stderr)
            return r.returncode or 1
        import mesh_compute_vs
        mesh_compute_vs.build_for_index(str(wide))
        seal_suffix = mesh_compute_vs.SEAL_SUFFIX
        with open(wide / "index.json", encoding="utf-8") as f:
            wide_index = json.load(f)
        entry = wide_index.get(name, {})
        needed = [name + ".spv", name + ".mesh_vs.spv", name + seal_suffix]
        if "mesh_compute" not in entry or not all((wide / n).is_file() for n in needed):
            print(f"ERROR: no validated --mesh-compute module for {name}", file=sys.stderr)
            return 1
        for n in needed:
            shutil.copyfile(wide / n, spirv / n)
        index[name] = entry
        (fssm / (name + ".fssm")).write_bytes(bmc.fssm_marker(name))

    # Direct-only producers derived from the sealed ms_v5_workgroup.
    source = "ms_v5_workgroup"
    producer = (spirv / (source + ".spv")).read_bytes()
    for name, replacement in (("ms_v5_legacy", None), ("ms_v5_badmarker", BAD_MARKER)):
        try:
            derived = rewrite_marker(producer, replacement)
        except ValueError as e:
            print(f"ERROR: {source}: {e}", file=sys.stderr)
            return 1
        (spirv / (name + ".spv")).write_bytes(derived)
        shutil.copyfile(spirv / (source + ".mesh_vs.spv"), spirv / (name + ".mesh_vs.spv"))
        with open(spirv / (source + seal_suffix), encoding="utf-8") as f:
            seal = json.load(f)
        seal["producer_sha256"] = sha256(derived)
        write_json(spirv / (name + seal_suffix), seal, sort_keys=True, separators=(",", ":"))
        program = bmc.fssm_marker(name)
        (fssm / (name + ".fssm")).write_bytes(program)
        entry = copy.deepcopy(index[source])
        entry["fssm_sha256"] = sha256(program)
        index[name] = entry

    # The GLSL modules.
    glsl_entries = {
        "ps_mesh_indirect_cell": ("ps-cell", {"stage": "ps", "bindings": [], "push_constant_bytes": 144,
                                              "outputs": [0]}),
        "cs_mesh_indirect_args": ("cs-args", {"stage": "cs", "bindings": [], "push_constant_bytes": 144,
                                              "local_size": [64, 1, 1], "uses_code_base": False,
                                              "wave64": False}),
    }
    for name, (key, meta) in glsl_entries.items():
        program = bmc.fssm_marker(name)
        index[name] = dict(meta, fssm_sha256=sha256(program))
        shutil.copyfile(glsl[key], spirv / (name + ".spv"))
        (fssm / (name + ".fssm")).write_bytes(program)
    write_json(spirv / "index.json", index, indent=2)
    print("staged mesh indirect fixtures in", out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
