#!/usr/bin/env python3
"""Stage translator-derived mesh-compute shaders for d3d12_mesh_compute_test.

    python3 -I runtime/test/shaders/build_mesh_compute.py <out dir>
        --ps-draw <ps_draw.spv> --fold <ps_mesh_group_fold.spv>
        --ms-native <ms_draw.spv> --vs <vs_fullscreen.spv>

For each hand-written ms_*.asm fixture (MESH_NAMES below):
  * assemble it with llvm-mc (LLVM_MC in the environment or llvm-mc on PATH,
    gfx1013 like scripts/run_gpu_tests.py),
  * write the synthetic DXBC it needs next to the listing (a mesh PSV0, plus
    the OSG1 ms_clip declares), exactly as scripts/run_gpu_tests.py builds them,
  * write a synthetic <name>.fssm sidecar (the FSSM magic plus name-derived
    words; the translator only hashes it, as for the game's programs),
  * translate the listing with tools/rdna2spirv.py --mesh-compute --fssm-dir,
    so the producer is a GLCompute module and index.json carries the real
    mesh_compute layout, root window, workgroup size, max vertices/primitives
    and 152 push-constant bytes. The ms_f16_pack/ps_interp_f16_high pair goes
    through a second invocation adding --f16-interp-fallback (main-native
    decoded halves, no #1930 computed IJ; the plain triple is untouched).
    ps_bary goes plainly (native barycentrics, no --bary-fallback flag),
  * write its consumer with tools/mesh_compute_vs.py (<name>.mesh_vs.spv),
    which also verifies and completes the versioned pair seal.

Then copy the four GLSL modules (compiled by CMake with glslangValidator)
into spirv/ and append their synthetic index entries: ps_draw and the fold
shader for the two readback grids, ms_native (a MeshEXT module with NO
mesh_compute layout: the stock-refusal control) and vs_fullscreen (the
wrong-kind control). Every entry is keyed by the SHA-256 of its published
fssm/<name>.fssm, which the test wraps in a game-shaped DXBC container, so
PSO creation goes through the real ShaderLibrary lookup. No game data:
every input is a hand-written .asm/.glsl fixture.

Publishes <out>/spirv/index.json, <out>/spirv/*.spv (+ *.mesh_vs.spv and the
<name>.mesh_compute.seal.json pair seals) and <out>/fssm/*.fssm. Standard library
only besides the repo's own tools.
"""

import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.normpath(os.path.join(HERE, "..", "..", "..", "tools"))
TRANSLATOR = os.path.join(TOOLS, "rdna2spirv.py")
sys.path.insert(0, TOOLS)
import mc_command  # noqa: E402

# ms_v5_workgroup: grid A's genuine 2x3x4 XYZ pattern (group index in param0).
# ms_tri: grid B's gradient (param0 = u, v, 0.5). ms_clip: the clip-distance edge.
MESH_NAMES = ("ms_v5_workgroup", "ms_tri", "ms_clip", "ms_overlimit")
# Simple f16 pair (main-native, no #1930 computed IJ): the mesh packs f16 halves
# plus f32, the PS reads decoded halves via --f16-interp-fallback.
F16_MESH_NAMES = ("ms_f16_pack",)
F16_PS_NAMES = ("ps_interp_f16_high",)
# Native barycentric PS (no --bary-fallback flag): raw i/j need
# VK_KHR_fragment_shader_barycentric, gated on the queried device feature.
BARY_PS_NAMES = ("ps_bary",)


# --- Minimal DXBC builders (scripts/run_gpu_tests.py: make_dxbc, make_sig_chunk,
# --- make_ms_psv0). Synthetic containers only; no game content. ---
def make_dxbc(chunks):
    """Build a minimal DXBC container from (fourcc, payload) pairs."""
    n = len(chunks)
    hdr_size = 32 + 4 * n
    offsets = []
    body = bytearray()
    for tag, payload in chunks:
        offsets.append(hdr_size + len(body))
        body += tag + struct.pack("<I", len(payload)) + payload
    total = hdr_size + len(body)
    hdr = bytearray(b"DXBC") + bytes(16) + struct.pack("<III", 1, total, n)
    for off in offsets:
        hdr += struct.pack("<I", off)
    return bytes(hdr) + bytes(body)


def make_sig_chunk(elements):
    """Build ISG1/OSG1 payload: elements of (sysv, ctype, reg, mask, rw_mask)."""
    out = bytearray(struct.pack("<II", len(elements), 8))
    for sysv, ctype, reg, mask, rw in elements:
        e = bytearray(32)
        struct.pack_into("<6I", e, 0, 0, 0, 0, sysv, ctype, reg)
        e[24] = mask
        e[25] = rw
        out += e
    return bytes(out)


def make_ms_psv0(threads_x, maxv, maxp, topology=2, group_shared=0):
    """Build PSV0 payload for a mesh shader (stage 13)."""
    r = bytearray(48)
    struct.pack_into("<I", r, 0, group_shared)
    struct.pack_into("<HH", r, 12, maxv, maxp)
    r[24] = 13  # mesh stage
    r[27] = topology  # 2 = triangles
    struct.pack_into("<III", r, 36, threads_x, 1, 1)
    return struct.pack("<I", len(r)) + bytes(r)


def fixture_dxbc(name):
    """The synthetic DXBC scripts/run_gpu_tests.py stages for fixture `name`: a mesh PSV0
    (plus ms_clip's OSG1), or an empty container for pixel shaders (code-driven inputs)."""
    if name.startswith("ps_"):
        return make_dxbc([])
    if name == "ms_v5_workgroup":
        return make_dxbc([(b"PSV0", make_ms_psv0(32, 3, 1, group_shared=128))])
    if name == "ms_clip":
        return make_dxbc([
            (b"PSV0", make_ms_psv0(128, 64, 64, 2)),
            # OSG1: SV_Position (sysv=1) reg 0 xyzw, SV_ClipDistance (sysv=2) reg 1 x
            (b"OSG1", make_sig_chunk([(1, 3, 0, 0xF, 0xF), (2, 3, 1, 0x1, 0xE)])),
        ])
    return make_dxbc([(b"PSV0", make_ms_psv0(128, 64, 64, 2))])


def fssm_marker(name):
    """Deterministic synthetic FSSM program for `name`: the magic plus 12
    name-derived bytes. The translator only hashes it (--fssm-dir); the test
    wraps the same bytes in its DXBC containers, so the lookup key matches."""
    digest = hashlib.sha256(b"leonida-mesh-compute-test\x00" + name.encode("utf-8")).digest()
    return b"FSSM" + digest[:12]


def asm_mattr(path):
    """A fixture's "; mattr: ..." header comment, or None (runtime/test/shaders/build.py)."""
    for ln in Path(path).read_text(encoding="utf-8").splitlines():
        s = ln.strip()
        if not s:
            continue
        if not s.startswith(";"):
            break
        m = re.match(r";\s*mattr:\s*(\S+)", s)
        if m:
            return m.group(1)
    return None


def assemble_to_s(llvm_mc_args, asm_path, name):
    """Assemble raw RDNA2 .asm with llvm-mc -show-encoding into the .s text
    rdna2spirv.py reads (scripts/run_gpu_tests.py: assemble_to_s)."""
    with open(asm_path, encoding="utf-8") as source:
        raw = source.read()
    cmd = list(llvm_mc_args) + ["-triple=amdgcn--amdpal", "-mcpu=gfx1013"]
    mattr = asm_mattr(asm_path)
    if mattr:
        cmd.append(f"-mattr={mattr}")
    cmd.append("-show-encoding")
    p = mc_command.run(cmd, flag="--llvm-mc", input=raw, capture_output=True, text=True)
    if p.returncode != 0:
        raise RuntimeError(f"llvm-mc failed on {name}:\n{p.stderr}")
    lines = [f"; {name}  code 0x0..0x0 (synthetic), gfx1013, 0 invalid words"]
    for ln in p.stdout.splitlines():
        if ln.startswith("\t.") or ln.strip().startswith("."):
            continue
        if ln.startswith("\t"):
            lines.append(ln)
    return "\n".join(lines) + "\n"

def main(argv):
    if any(arg in ("-h", "--help") for arg in argv[1:]):
        print(__doc__)
        return 0
    # Glslang-built modules, each --<key> <path>; the index entry is fixed below.
    glsl_keys = ("--ps-draw", "--fold", "--ms-native", "--vs")
    args, glsl = [], {}
    i = 1
    while i < len(argv):
        if argv[i] in glsl_keys:
            if i + 1 >= len(argv):
                print(f"ERROR: {argv[i]} needs a path", file=sys.stderr)
                return 2
            glsl[argv[i][2:]] = argv[i + 1]
            i += 2
        else:
            args.append(argv[i])
            i += 1
    if len(args) != 1 or sorted(glsl) != ["fold", "ms-native", "ps-draw", "vs"]:
        print(__doc__, file=sys.stderr)
        return 2
    out = Path(args[0])
    for key, path in sorted(glsl.items()):
        if not os.path.isfile(path):
            print(f"ERROR: --{key} {path}: not a file", file=sys.stderr)
            return 2
    # The CMake test targets name the spirv-as/spirv-val folder here instead of
    # passing a whole PATH (runtime/test/shaders/build.py: main).
    if os.environ.get("LEONIDA_SPIRV_TOOLS"):
        os.environ["PATH"] = os.environ["LEONIDA_SPIRV_TOOLS"] + os.pathsep + os.environ.get("PATH", "")
    llvm_mc = mc_command.mc_command()
    out.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="staging-", dir=out) as tmp:
        work = Path(tmp)
        disasm = work / "disasm"
        disasm.mkdir()
        disasm_fb = work / "disasm_fb"
        disasm_fb.mkdir()
        sidecars = work / "sidecars"
        sidecars.mkdir()
        markers = work / "fssm_markers"
        markers.mkdir()
        for name, folder in ([(n, disasm) for n in MESH_NAMES + BARY_PS_NAMES] +
                             [(n, disasm_fb) for n in F16_MESH_NAMES + F16_PS_NAMES]):
            asm_path = os.path.join(HERE, name + ".asm")
            (folder / (name + ".s")).write_text(assemble_to_s(llvm_mc, asm_path, name),
                                                encoding="utf-8", newline="\n")
            # The translator reads <folder>/../<name>.dxbc for a listing in <folder>.
            (work / (name + ".dxbc")).write_bytes(fixture_dxbc(name))
            (sidecars / (name + ".fssm")).write_bytes(fssm_marker(name))
        spirv = work / "spirv"
        # Two flag sets, one merged index: the plain triple plus the native-bary PS,
        # then the f16 pair (--f16-interp-fallback only; --mesh-compute is a no-op for ps).
        runs = (("plain", disasm, ["--mesh-compute"]),
                ("f16", disasm_fb, ["--mesh-compute", "--f16-interp-fallback"]))
        for label, folder, flags in runs:
            cmd = [sys.executable, "-I", TRANSLATOR, str(folder), "--out", str(spirv),
                   "--fssm-dir", str(sidecars)] + flags
            r = subprocess.run(cmd, capture_output=True, text=True)
            print(f"=== rdna2spirv.py {label} ({' '.join(flags)}) ===")
            print(r.stdout, end="")
            if r.returncode != 0:
                print(r.stderr, end="", file=sys.stderr)
                return r.returncode or 1
        import mesh_compute_vs
        try:
            built_vs = mesh_compute_vs.build_for_index(str(spirv))
        except (RuntimeError, OSError) as e:
            print(f"ERROR: {e}", file=sys.stderr)
            return 1
        for vs_spv in built_vs:
            print("mesh compute VS:", os.path.basename(vs_spv))
        seal_suffix = mesh_compute_vs.SEAL_SUFFIX  # versioned pair seal, completed above
        with open(spirv / "index.json", encoding="utf-8") as f:
            index = json.load(f)
        missing = [n for n in MESH_NAMES
                   if n not in index or "mesh_compute" not in index[n]
                   or not (spirv / (n + ".spv")).is_file()
                   or not (spirv / (n + ".mesh_vs.spv")).is_file()
                   or not (spirv / (n + seal_suffix)).is_file()]
        for n in F16_MESH_NAMES:
            lay = index.get(n, {}).get("mesh_compute", {})
            if (n not in index or not lay or "f16_halves" not in index[n]
                    or "f16_halves" not in lay
                    or not (spirv / (n + ".spv")).is_file()
                    or not (spirv / (n + ".mesh_vs.spv")).is_file()
                    or not (spirv / (n + seal_suffix)).is_file()):
                missing.append(n)
        for n in F16_PS_NAMES:
            meta = index.get(n, {})
            if ("f16_halves" not in meta or "bary_fallback_input" in meta
                    or not (spirv / (n + ".spv")).is_file()):
                missing.append(n)
        for n in BARY_PS_NAMES:
            meta = index.get(n, {})
            feats = meta.get("features", [])
            if (n not in index or "bary_fallback_input" in meta
                    or "fragmentShaderBarycentric" not in feats
                    or not (spirv / (n + ".spv")).is_file()):
                missing.append(n)
        if missing:
            print(f"ERROR: no validated --mesh-compute module for: {', '.join(missing)}", file=sys.stderr)
            return 1
        # The GLSL modules and their synthetic entries (runtime/test/d3d12_mesh_test.cpp
        # writes the same shape by hand). ms_native is MeshEXT with NO mesh_compute
        # layout: a native module the no-extension device must still refuse.
        entries = {
            "ps_draw": ("ps-draw", "ps", {"bindings": [], "push_constant_bytes": 144, "outputs": [0]}),
            "ps_mesh_group_fold": ("fold", "ps", {"bindings": [], "push_constant_bytes": 144, "outputs": [0]}),
            "ms_native": ("ms-native", "ms", {"bindings": [], "push_constant_bytes": 144,
                                              "local_size": [32, 1, 1], "max_vertices": 4,
                                              "max_primitives": 2,
                                              "root_window": {"sgpr": 16, "dword": 8}}),
            "vs_fullscreen": ("vs", "vs", {"bindings": [], "push_constant_bytes": 144,
                                           "input_signature": []}),
        }
        for entry, (key, stage, meta) in entries.items():
            program = fssm_marker(entry)
            meta = dict(meta, stage=stage, fssm_sha256=hashlib.sha256(program).hexdigest())
            index[entry] = meta
            shutil.copyfile(glsl[key], spirv / (entry + ".spv"))
            (markers / (entry + ".fssm")).write_bytes(program)
        with open(spirv / "index.json", "w", encoding="utf-8", newline="\n") as f:
            json.dump(index, f, indent=2)
            f.write("\n")
        out_spirv = out / "spirv"
        out_fssm = out / "fssm"
        out_spirv.mkdir(parents=True, exist_ok=True)
        out_fssm.mkdir(parents=True, exist_ok=True)
        for artifact in out_spirv.glob("*"):
            artifact.unlink()
        for artifact in out_fssm.glob("*"):
            artifact.unlink()
        for name in list(MESH_NAMES) + list(F16_MESH_NAMES) + list(F16_PS_NAMES) + list(BARY_PS_NAMES) + [e for e in entries]:
            shutil.copyfile(spirv / (name + ".spv"), out_spirv / (name + ".spv"))
        for name in MESH_NAMES + F16_MESH_NAMES:
            for suffix in (".mesh_vs.spv", ".mesh_vs.spvasm", ".spvasm", seal_suffix):
                artifact = spirv / (name + suffix)
                if artifact.is_file():
                    shutil.copyfile(artifact, out_spirv / (name + suffix))
        shutil.copyfile(spirv / "index.json", out_spirv / "index.json")
        for name in MESH_NAMES + F16_MESH_NAMES + F16_PS_NAMES + BARY_PS_NAMES:
            shutil.copyfile(sidecars / (name + ".fssm"), out_fssm / (name + ".fssm"))
        for entry in entries:
            shutil.copyfile(markers / (entry + ".fssm"), out_fssm / (entry + ".fssm"))
    print("staged mesh-compute shaders in", out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
