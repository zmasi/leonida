#!/usr/bin/env python3
"""Vertex shaders that draw what rdna2spirv.py --mesh-compute mesh shaders wrote.

On a device without VK_EXT_mesh_shader, `rdna2spirv.py --mesh-compute` turns each mesh shader into a compute shader
with the same workgroup size. Each workgroup writes its vertex and primitive counts, its vertices and its
primitives into a slot of an output buffer. The layout is the shader's "mesh_compute" entry in index.json:
rdna2spirv.mesh_compute_layout. The buffer's device address is in push-constant dwords 36..37, for both the
compute shader and this vertex shader (152 push-constant bytes).

The draw that follows is non-indexed: vkCmdDraw(groups * max_primitives * 3), where groups is the number of
workgroups dispatched. Vertex v belongs to workgroup v / (3 * max_primitives), primitive (v / 3) % max_primitives,
corner v % 3. A primitive past the workgroup's count, or one marked culled, gets the same position outside the
clip volume at all three corners, so it is clipped away. Otherwise the corner's vertex index (clamped to
max_vertices - 1) selects the vertex whose position, clip and cull distances and params this shader outputs. The
params go out at the locations the mesh shader used, so the pixel shader is unchanged. With rdna2spirv.py
--f16-interp-fallback the layout also has "f16_halves" ({param: [low location, high location]}), and each of those
params' f16 halves goes out decoded to f32 at those locations too, as a translated VS exports them.

A 128-byte --pc-tail-ubo layout (address at tail dwords 4..5, not push dwords 36..37) has no verified companion
in this wave: vs_spvasm refuses it explicitly instead of emitting a push-reading shader for it.

BDA safety: stage shapes alone do not bind the index slot stride/offsets to the modules, so a
stale producer could allocate a smaller scratch slice than its baked stride. Every declared mesh-compute entry
carries a versioned seal <name>.mesh_compute.seal.json v1 beside its modules: canonical layout JSON
(sort_keys compact, 16 keys, numeric params/halves as decimal strings) hashed with SHA-256, plus SHA-256 of the
actual producer .spv bytes and, once built, of the companion .spvasm/.spv bytes. rdna2spirv.py writes the partial
seal (layout+producer) at generation; this tool verifies it before building and completes it after validation
(checking BOTH assembly and binary bytes on cache hits, never asm-hash + nonempty alone). The C++ runtime
(ShaderLibrary::load_mesh_compute_pair / validate_mesh_compute_modules) re-verifies all three hashes plus
execution models, LocalSize-vs-metadata agreement, and locations before any vkCreate*. Missing/malformed seals on
a declared representation are explicit errors, never silent native. Normal builds generate their own seals and
never trust foreign ones.

    python -I tools/mesh_compute_vs.py <spirv dir> [<spirv dir> ...]  # <name>.mesh_vs.spv per entry
    python -I tools/mesh_compute_vs.py --pack <spirv root>            # main + auxiliary indices in place

Compact API for make_game.py and pack builds:
    import mesh_compute_vs
    mesh_compute_vs.build_for_pack(spirv_root)  # main + auxiliaries, returns [spv]
    mesh_compute_vs.build_for_index(spirv_dir)  # one index.json, returns [spv] (all, including verified-skipped)
    mesh_compute_vs.vs_spvasm(layout)           # assembly text for one layout (raises ValueError for 128B pc-tail)
    mesh_compute_vs.canonical_layout_json(layout)  # sealed identity (sort_keys compact, byte-matches C++)

Standard library only; build_for_index assembles and validates with spirv-as / spirv-val from PATH, deduplicating
by assembly content (one compile per distinct layout) and skipping seal-verified-unchanged companions (layout,
producer, assembly AND binary hashes all match), so a resumed pack costs no tool invocations. Writes are atomic
(temp file + os.replace); failed builds write neither .spv nor seals and cannot falsely validate stale binaries.
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile

SUFFIX = ".mesh_vs"   # <ms name>.mesh_vs.spvasm / .spv next to the mesh shader
USER_SGPRS = 32       # push constants: s0..s31, heap base, code base, then the output address (rdna2spirv.py)
PUSH_152 = 152        # supported: output address in push dwords 36..37
PUSH_128 = 128        # --pc-tail-ubo: address at tail dwords 4..5, no verified companion this wave
ADDRESS_DWORDS = [USER_SGPRS + 4, USER_SGPRS + 5]  # [36, 37]
PC_TAIL_ADDRESS_DWORDS = [4, 5]
# make_game.py pack layout (runtime/src/shader_library.h reads the same set): main index plus auxiliaries.
PACK_SUBDIRS = ("", "fspx", "fspx_compute", "fspx_gfx", "bvh", "exe", "library")
MESH_COMPUTE_SEAL_VERSION = 1
SEAL_SUFFIX = ".mesh_compute.seal.json"  # <ms name>.mesh_compute.seal.json beside producer/companion


def canonical_layout_json(layout):
    """Deterministic layout identity, byte-identical to C++ canonical_mesh_compute_json and the parent receipt.

    `json.dumps(normalized, sort_keys=True, separators=(",", ":"))` over exactly the 16 validated keys, with
    params/f16_halves keys as decimal strings. Raises ValueError for missing/boolean/non-integral fields,
    malformed address lists (length checked before indexing), and invalid/duplicate numeric keys (fail closed).
    """
    def need_int(value, what):
        if isinstance(value, bool) or not isinstance(value, (int, float)) or int(value) != value:
            raise ValueError(f"mesh_compute_vs: layout {what} must be an integer, got {value!r}")
        return int(value)

    def need_key(key, what):
        if isinstance(key, bool):
            raise ValueError(f"mesh_compute_vs: layout {what} key {key!r} must be decimal")
        if isinstance(key, int) and not isinstance(key, bool):
            text = str(key)
        elif isinstance(key, float) and int(key) == key:
            text = str(int(key))
        elif isinstance(key, str):
            text = key
        else:
            raise ValueError(f"mesh_compute_vs: layout {what} key {key!r} must be decimal")
        # Leading zeros ("00") stay distinct as strings here; the numeric-duplicate check below rejects "0"+"00".
        if not text.isdigit():
            raise ValueError(f"mesh_compute_vs: layout {what} key {key!r} must be decimal")
        return text

    def need_dword_pair(value, what):
        if value is None:
            return None
        if not isinstance(value, (list, tuple)) or len(value) != 2:
            raise ValueError(f"mesh_compute_vs: layout {what} must be [lo, hi] or null")
        return [need_int(value[0], f"{what}[0]"), need_int(value[1], f"{what}[1]")]

    try:
        if not isinstance(layout, dict):
            raise ValueError("mesh_compute_vs: layout must be an object")
        params = layout["params"]
        halves = layout.get("f16_halves", {})
        if not isinstance(params, dict) or not isinstance(halves, dict):
            raise ValueError("mesh_compute_vs: layout params/f16_halves must be objects")
        norm_params, seen_locs = {}, set()
        for k, v in params.items():
            key = need_key(k, "params")
            numeric = int(key)
            if numeric in seen_locs:
                raise ValueError(f"mesh_compute_vs: duplicate params location {numeric}")
            seen_locs.add(numeric)
            norm_params[key] = need_int(v, f"params[{k!r}]")
        norm_halves, seen_params = {}, set()
        for k, v in halves.items():
            key = need_key(k, "f16_halves")
            numeric = int(key)
            if numeric in seen_params:
                raise ValueError(f"mesh_compute_vs: duplicate f16_halves param {numeric}")
            seen_params.add(numeric)
            if not isinstance(v, (list, tuple)) or len(v) != 2:
                raise ValueError(f"mesh_compute_vs: layout f16_halves[{k!r}] must be [lo, hi]")
            norm_halves[key] = [need_int(v[0], f"f16_halves[{k!r}][0]"),
                                need_int(v[1], f"f16_halves[{k!r}][1]")]
        if "address_dwords" not in layout:
            raise ValueError("mesh_compute_vs: layout lacks address_dwords")
        norm = {
            "slot_bytes": need_int(layout["slot_bytes"], "slot_bytes"),
            "vertices": need_int(layout["vertices"], "vertices"),
            "vertex_stride": need_int(layout["vertex_stride"], "vertex_stride"),
            "primitives": need_int(layout["primitives"], "primitives"),
            "primitive_stride": need_int(layout["primitive_stride"], "primitive_stride"),
            "max_vertices": need_int(layout["max_vertices"], "max_vertices"),
            "max_primitives": need_int(layout["max_primitives"], "max_primitives"),
            "clip_count": need_int(layout["clip_count"], "clip_count"),
            "clip_offset": need_int(layout["clip_offset"], "clip_offset"),
            "cull_count": need_int(layout["cull_count"], "cull_count"),
            "cull_offset": need_int(layout["cull_offset"], "cull_offset"),
            "params": norm_params,
            "push_constant_bytes": need_int(layout["push_constant_bytes"], "push_constant_bytes"),
            "address_dwords": need_dword_pair(layout["address_dwords"], "address_dwords"),
            "pc_tail_address_dwords": need_dword_pair(layout.get("pc_tail_address_dwords"), "pc_tail_address_dwords"),
            "f16_halves": norm_halves,
        }
    except (KeyError, TypeError, ValueError) as e:
        if isinstance(e, ValueError) and str(e).startswith("mesh_compute_vs:"):
            raise
        raise ValueError(f"mesh_compute_vs: malformed layout for canonical identity ({e})") from None
    return json.dumps(norm, sort_keys=True, separators=(",", ":"))


def layout_sha256(layout):
    """SHA-256 hex of canonical_layout_json(layout)."""
    return hashlib.sha256(canonical_layout_json(layout).encode("utf-8")).hexdigest()


def file_sha256(path):
    """SHA-256 hex of a file's bytes."""
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _is_hex64(value):
    return isinstance(value, str) and len(value) == 64 and all(c in "0123456789abcdef" for c in value)


def read_seal_file(path):
    """Parsed v1 seal dict, or ValueError naming the file (missing callers handle FileNotFoundError themselves)."""
    with open(path, encoding="utf-8") as f:
        seal = json.load(f)
    if not isinstance(seal, dict) or seal.get("version") != MESH_COMPUTE_SEAL_VERSION:
        raise ValueError(f"{path}: unsupported seal version (need {MESH_COMPUTE_SEAL_VERSION})")
    for key in ("layout_sha256", "producer_sha256"):
        if not _is_hex64(seal.get(key)):
            raise ValueError(f"{path}: malformed seal {key}")
    asm_hash, spv_hash = seal.get("companion_asm_sha256"), seal.get("companion_spv_sha256")
    if (asm_hash is None) != (spv_hash is None):
        raise ValueError(f"{path}: partial companion seal (both hashes or neither)")
    if asm_hash is not None and (not _is_hex64(asm_hash) or not _is_hex64(spv_hash)):
        raise ValueError(f"{path}: malformed companion seal hashes")
    return seal


def write_seal_file(path, seal):
    """Atomically write a seal dict (temp file + os.replace) beside its modules, inside this pack only."""
    tmp = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", newline="\n",
                                         dir=os.path.dirname(path) or ".",
                                         prefix="." + os.path.basename(path) + "-",
                                         suffix=".tmp", delete=False) as f:
            tmp = f.name
            json.dump(seal, f, sort_keys=True, separators=(",", ":"))
            f.write("\n")
        os.replace(tmp, path)
        tmp = None
    finally:
        if tmp is not None:
            try:
                os.unlink(tmp)
            except OSError:
                pass

F16_HALF_TOP = 29


def is_pc_tail_layout(layout):
    """Whether a mesh_compute layout is the 128-byte --pc-tail-ubo variant (no verified companion this wave)."""
    try:
        if int(layout.get("push_constant_bytes", PUSH_152)) == PUSH_128:
            return True
        return layout.get("address_dwords") is None and "pc_tail_address_dwords" in layout
    except (AttributeError, TypeError, ValueError):
        return False


def check_push_contract(layout):
    """Raise ValueError unless the layout is the supported 152-byte push variant (explicit 128B distinction)."""
    try:
        push = layout.get("push_constant_bytes", PUSH_152)
        addr = layout.get("address_dwords", ADDRESS_DWORDS)
    except AttributeError:
        raise ValueError("mesh_compute_vs: layout must be an object") from None
    if push == PUSH_152 and (list(addr) if isinstance(addr, (list, tuple)) else addr) == ADDRESS_DWORDS:
        return
    if is_pc_tail_layout(layout):
        raise ValueError("mesh_compute_vs: 128-byte pc-tail UBO layout (push_constant_bytes=128, address at tail "
                         "dwords 4..5) has no verified companion in this wave; refusing (need the 152-byte push "
                         "variant with address_dwords [36, 37])")
    raise ValueError(f"mesh_compute_vs: unsupported push contract (push_constant_bytes={push!r}, "
                     f"address_dwords={addr!r}); need 152-byte push with address_dwords [36, 37]")


def pair_f16_halves(layout, ps_f16_halves, ps_raw_inputs=(), top=F16_HALF_TOP):
    """Choose non-colliding `f16_halves` ({str(param): [lo, hi]}) for a (--mesh-compute, PS) pair (SHADER-378).

    Keeps the PS's own `[lo, hi]` locations whenever every requested param is exported by `layout["params"]` and no
    half location collides with any producer param or exceeds `top`. Otherwise packs all requested pairs downward
    from `top` into locations disjoint from `producer_params | ps_raw_inputs`.
    """
    producer_params = {int(k) for k in layout.get("params", {})}
    ps_pairs = {int(k): [int(v[0]), int(v[1])] for k, v in ps_f16_halves.items()}
    if not ps_pairs:
        return {}
    missing = sorted(p for p in ps_pairs if p not in producer_params)
    if missing:
        raise ValueError(f"mesh_compute_vs: producer does not export f16_halves param(s) {missing}")
    if all(0 <= lo <= top and 0 <= hi <= top and lo != hi and lo not in producer_params and hi not in producer_params
           for lo, hi in ps_pairs.values()):
        used_halves = [loc for pair in ps_pairs.values() for loc in pair]
        if len(set(used_halves)) == len(used_halves):
            return {str(k): list(v) for k, v in sorted(ps_pairs.items())}
    reserved = producer_params | {int(x) for x in ps_raw_inputs}
    free = [loc for loc in range(top, -1, -1) if loc not in reserved]
    need = 2 * len(ps_pairs)
    if len(free) < need:
        raise ValueError(f"mesh_compute_vs: need {need} decoded-half locations, only {len(free)} free in 0..{top}")
    return {str(p): [free[2 * k + 1], free[2 * k]] for k, p in enumerate(sorted(ps_pairs))}


def vs_spvasm(layout, f16_halves=None):
    """SPIR-V assembly of the vertex shader for one --mesh-compute layout (index.json "mesh_compute").

    Optional `f16_halves` ({param: [low_loc, high_loc]}) overrides `layout["f16_halves"]` for per-pipeline pairing
    (SHADER-378), so a producer with many params only exports the decoded halves its paired PS actually reads.
    """
    check_push_contract(layout)
    lay = dict(layout)
    if f16_halves is not None:
        lay["f16_halves"] = f16_halves
    params = sorted((int(k), int(v)) for k, v in lay["params"].items())
    maxv, maxp = int(lay["max_vertices"]), int(lay["max_primitives"])
    clip, cull = int(lay["clip_count"]), int(lay["cull_count"])
    consts = {}

    def c(v):
        consts.setdefault(int(v), f"%c{int(v)}")
        return consts[int(v)]

    ulongs = {}

    def cl(v):
        ulongs.setdefault(int(v), f"%cl{int(v)}")
        return ulongs[int(v)]

    body = []
    n = [0]

    def tmp(p="t"):
        n[0] += 1
        return f"%{p}{n[0]}"

    def load_word(base64, off):
        """uint at slot byte offset `off` (constant) from the ulong address `base64`."""
        a, p, v = tmp("a"), tmp("p"), tmp("w")
        body.extend([f"{a} = OpIAdd %ulong {base64} {cl(off)}" if off else f"{a} = OpCopyObject %ulong {base64}",
                     f"{p} = OpConvertUToPtr %_ptr_PSB_uint {a}", f"{v} = OpLoad %uint {p} Aligned 4"])
        return v

    def load_floats(base64, off, count):
        out = []
        for i in range(count):
            w = load_word(base64, off + 4 * i)
            f = tmp("f")
            body.append(f"{f} = OpBitcast %float {w}")
            out.append(f)
        return out

    c(0), c(1), c(3), c(5), c(6)
    if clip:
        c(clip)
    if cull:
        c(cull)
    body += [
        "%vid = OpLoad %int %vertex_index", "%vidu = OpBitcast %uint %vid",
        f"%per = OpCopyObject %uint {c(3 * maxp)}",
        "%wg = OpUDiv %uint %vidu %per", "%rem = OpUMod %uint %vidu %per",
        f"%prim = OpUDiv %uint %rem {c(3)}", f"%corner = OpUMod %uint %rem {c(3)}",
        f"%alo_p = OpAccessChain %_ptr_PC_uint %pc {c(5)}", f"%ahi_p = OpAccessChain %_ptr_PC_uint %pc {c(6)}",
        "%alo = OpLoad %uint %alo_p", "%ahi = OpLoad %uint %ahi_p",
        "%alo64 = OpUConvert %ulong %alo", "%ahi64 = OpUConvert %ulong %ahi",
        f"%ahis = OpShiftLeftLogical %ulong %ahi64 {cl(32)}", "%addr = OpBitwiseOr %ulong %ahis %alo64",
        "%wg64 = OpUConvert %ulong %wg", f"%soff = OpIMul %ulong %wg64 {cl(lay['slot_bytes'])}",
        "%slot = OpIAdd %ulong %addr %soff"]
    pcount = load_word("%slot", 4)
    body += [f"%inrange = OpULessThan %bool %prim {pcount}",
             f"%pofs = OpIMul %uint %prim {c(lay['primitive_stride'])}", "%pofs64 = OpUConvert %ulong %pofs",
             f"%pbase0 = OpIAdd %ulong %slot {cl(lay['primitives'])}", "%pbase = OpIAdd %ulong %pbase0 %pofs64",
             f"%cofs = OpIMul %uint %corner {c(4)}", "%cofs64 = OpUConvert %ulong %cofs",
             "%iaddr = OpIAdd %ulong %pbase %cofs64", "%iptr = OpConvertUToPtr %_ptr_PSB_uint %iaddr",
             "%idx0 = OpLoad %uint %iptr Aligned 4",
             f"%idx = OpExtInst %uint %glsl UMin %idx0 {c(maxv - 1)}"]
    culled_w = load_word("%pbase", 12)
    body += [f"%culled = OpINotEqual %bool {culled_w} {c(0)}", "%notculled = OpLogicalNot %bool %culled",
             "%valid = OpLogicalAnd %bool %inrange %notculled",
             f"%vofs = OpIMul %uint %idx {c(lay['vertex_stride'])}", "%vofs64 = OpUConvert %ulong %vofs",
             f"%vbase0 = OpIAdd %ulong %slot {cl(lay['vertices'])}", "%vbase = OpIAdd %ulong %vbase0 %vofs64"]
    pos = load_floats("%vbase", 0, 4)
    body += [f"%pos = OpCompositeConstruct %v4float {' '.join(pos)}",
             "%pos_out = OpSelect %v4float %valid %pos %outside", "OpStore %out_pos %pos_out"]
    if clip:
        vals = load_floats("%vbase", lay["clip_offset"], clip)
        body += [f"%clipv = OpCompositeConstruct %arr_clip {' '.join(vals)}", "OpStore %out_clip %clipv"]
    if cull:
        vals = load_floats("%vbase", lay["cull_offset"], cull)
        body += [f"%cullv = OpCompositeConstruct %arr_cull {' '.join(vals)}", "OpStore %out_cull %cullv"]
    for loc, off in params:
        vals = load_floats("%vbase", off, 4)
        body += [f"%par{loc} = OpCompositeConstruct %v4float {' '.join(vals)}", f"OpStore %out{loc} %par{loc}"]
    halves = sorted((int(k), [int(x) for x in v]) for k, v in lay.get("f16_halves", {}).items())
    offsets = dict(params)
    used_locs = set(offsets)
    for loc, hlocs in halves:
        if loc not in offsets:
            raise ValueError(f"mesh_compute_vs: f16_halves param {loc} is not in layout params")
        if len(hlocs) != 2 or hlocs[0] == hlocs[1] or hlocs[0] < 0 or hlocs[1] < 0:
            raise ValueError(f"mesh_compute_vs: invalid f16_halves locations {hlocs} for param {loc}")
        for hloc in hlocs:
            if hloc in used_locs:
                raise ValueError(f"mesh_compute_vs: f16_halves location {hloc} collides with another output")
            used_locs.add(hloc)
    for loc, hlocs in halves:      # --f16-interp-fallback: each channel's two f16 halves decoded to f32
        parts = ([], [])
        for i in range(4):
            w, h2 = load_word("%vbase", offsets[loc] + 4 * i), tmp("h")
            body.append(f"{h2} = OpExtInst %v2float %glsl UnpackHalf2x16 {w}")
            for half in (0, 1):
                e = tmp("e")
                body.append(f"{e} = OpCompositeExtract %float {h2} {half}")
                parts[half].append(e)
        for half, hloc in enumerate(hlocs):
            body += [f"%hv{hloc} = OpCompositeConstruct %v4float {' '.join(parts[half])}",
                     f"OpStore %out{hloc} %hv{hloc}"]
    hout = [hloc for _, hlocs in halves for hloc in hlocs]
    body += ["OpReturn", "OpFunctionEnd"]

    outs = ["%out_pos"] + (["%out_clip"] if clip else []) + (["%out_cull"] if cull else []) + \
        [f"%out{loc}" for loc, _ in params] + [f"%out{h}" for h in hout]
    caps = ["Shader", "Int64", "PhysicalStorageBufferAddresses"] + (["ClipDistance"] if clip else []) + \
        (["CullDistance"] if cull else [])
    head = [f"OpCapability {x}" for x in caps] + [
        'OpExtension "SPV_KHR_physical_storage_buffer"',
        '%glsl = OpExtInstImport "GLSL.std.450"',
        "OpMemoryModel PhysicalStorageBuffer64 GLSL450",
        f'OpEntryPoint Vertex %main "main" %vertex_index %pc {" ".join(outs)}',
        "OpDecorate %vertex_index BuiltIn VertexIndex",
        "OpDecorate %out_pos BuiltIn Position"]
    if clip:
        head.append("OpDecorate %out_clip BuiltIn ClipDistance")
    if cull:
        head.append("OpDecorate %out_cull BuiltIn CullDistance")
    head += [f"OpDecorate %out{loc} Location {loc}" for loc in [p for p, _ in params] + hout]
    head += ["OpDecorate %arr_ud ArrayStride 4", "OpDecorate %PC Block", "OpMemberDecorate %PC 0 Offset 0"]
    head += [f"OpMemberDecorate %PC {k} Offset {4 * USER_SGPRS + 4 * (k - 1)}" for k in range(1, 7)]
    head += ["OpDecorate %rt_uint ArrayStride 4"]
    types = ["%void = OpTypeVoid", "%fn = OpTypeFunction %void", "%bool = OpTypeBool",
             "%uint = OpTypeInt 32 0", "%int = OpTypeInt 32 1", "%ulong = OpTypeInt 64 0",
             "%float = OpTypeFloat 32", "%v4float = OpTypeVector %float 4", "%v2float = OpTypeVector %float 2"]
    types += [f"{name} = OpConstant %uint {v}" for v, name in sorted(consts.items())]
    types += [f"{name} = OpConstant %ulong {v}" for v, name in sorted(ulongs.items())]
    types += [f"%n_ud = OpConstant %uint {USER_SGPRS}", "%arr_ud = OpTypeArray %uint %n_ud",
              "%PC = OpTypeStruct %arr_ud %uint %uint %uint %uint %uint %uint",
              "%_ptr_PC = OpTypePointer PushConstant %PC", "%_ptr_PC_uint = OpTypePointer PushConstant %uint",
              "%rt_uint = OpTypeRuntimeArray %uint",
              "%_ptr_PSB_uint = OpTypePointer PhysicalStorageBuffer %uint",
              "%_ptr_In_int = OpTypePointer Input %int", "%_ptr_Out_v4 = OpTypePointer Output %v4float",
              "%f1 = OpConstant %float 1", "%f2 = OpConstant %float 2",
              "%outside = OpConstantComposite %v4float %f2 %f2 %f2 %f1"]
    if clip:
        types += ["%arr_clip = OpTypeArray %float " + c(clip), "%_ptr_Out_clip = OpTypePointer Output %arr_clip"]
    if cull:
        types += ["%arr_cull = OpTypeArray %float " + c(cull), "%_ptr_Out_cull = OpTypePointer Output %arr_cull"]
    gvars = ["%vertex_index = OpVariable %_ptr_In_int Input", "%pc = OpVariable %_ptr_PC PushConstant",
             "%out_pos = OpVariable %_ptr_Out_v4 Output"]
    if clip:
        gvars.append("%out_clip = OpVariable %_ptr_Out_clip Output")
    if cull:
        gvars.append("%out_cull = OpVariable %_ptr_Out_cull Output")
    gvars += [f"%out{loc} = OpVariable %_ptr_Out_v4 Output" for loc in [p for p, _ in params] + hout]
    fn = ["%main = OpFunction %void None %fn", "%entry = OpLabel"]
    return "\n".join(head + types + gvars + fn + body) + "\n"


def write_for_index(spirv_dir, *, skip_unchanged=True):
    """Write <name>.mesh_vs.spvasm for every entry of <spirv_dir>/index.json with a "mesh_compute" layout.

    With skip_unchanged (default), an identical existing .spvasm is left untouched (mtime preserved) so a
    resumed pack costs no rewrite; the path is still returned. Writes are atomic (temp file + os.replace).
    A 128-byte pc-tail layout raises RuntimeError naming the index and entry (explicit, never a wrong shader).
    """
    index_path = os.path.join(spirv_dir, "index.json")
    if not os.path.isfile(index_path):
        return []
    try:
        with open(index_path, encoding="utf-8") as f:
            index = json.load(f)
    except (ValueError, RecursionError) as e:
        raise RuntimeError(f"{index_path}: invalid JSON ({e})") from None
    if not isinstance(index, dict):
        raise RuntimeError(f"{index_path}: expected a shader index object")
    written = []
    for name, meta in sorted(index.items()):
        lay = meta.get("mesh_compute") if isinstance(meta, dict) else None
        if not lay:
            continue
        try:
            asm = vs_spvasm(lay)
        except (AttributeError, KeyError, OverflowError, TypeError, ValueError) as e:
            raise RuntimeError(f"{index_path}: invalid mesh_compute layout for {name!r}: {e}") from None
        path = os.path.join(spirv_dir, name + SUFFIX + ".spvasm")
        if skip_unchanged:
            try:
                with open(path, encoding="utf-8") as f:
                    if f.read() == asm:
                        written.append(path)
                        continue
            except (OSError, ValueError, UnicodeError):
                pass
        tmp = None
        try:
            with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", newline="\n",
                                             dir=spirv_dir, prefix="." + name + SUFFIX + "-",
                                             suffix=".tmp", delete=False) as f:
                tmp = f.name
                f.write(asm)
            os.replace(tmp, path)
            tmp = None
        finally:
            if tmp is not None:
                try:
                    os.unlink(tmp)
                except OSError:
                    pass
        written.append(path)
    return written


def tool_path(name):
    """The SPIR-V tool's full path, or RuntimeError naming it: subprocess's own FileNotFoundError says only
    "the system cannot find the file specified", which does not say which tool is missing."""
    path = shutil.which(name)
    if path is None:
        raise RuntimeError(f"{name} not found on PATH (it ships with SPIRV-Tools, e.g. VulkanSDK/Bin)")
    return path


def build_for_index(spirv_dir, *, skip_unchanged=True):
    """write_for_index, then assemble to <name>.mesh_vs.spv and validate (Vulkan 1.2, as for the translator).

    Returns all .spv paths (including skipped-unchanged). Requires the translator's partial seal
    (<name>.mesh_compute.seal.json v1 with layout+producer) before building any companion; missing/mismatched
    seals, swapped producers, or tampered index layouts fail without invoking tools and without blessing stale
    binaries. Cold packs deduplicate by assembly content (one compile per distinct layout). Cached companions
    verify BOTH assembly and binary bytes against the seal (never asm-hash + nonempty alone); only
    new/changed/mismatched companions run spirv-as + spirv-val, after which their seals gain companion hashes.
    Seals live beside their modules inside this pack only; no global state. Failed builds write neither .spv
    nor seals. Raises RuntimeError with the tool output on failure.
    """
    asm_paths = write_for_index(spirv_dir, skip_unchanged=skip_unchanged)
    all_spv = [asm[: -len(".spvasm")] + ".spv" for asm in asm_paths]
    if not asm_paths:
        return []
    index_path = os.path.join(spirv_dir, "index.json")
    with open(index_path, encoding="utf-8") as f:
        index = json.load(f)
    entries = {}
    for asm, spv in zip(asm_paths, all_spv):
        name = os.path.basename(asm)[: -len(SUFFIX + ".spvasm")]
        meta = index.get(name)
        lay = meta.get("mesh_compute") if isinstance(meta, dict) else None
        if not lay:
            continue  # write_for_index already filtered; defensive (renamed mid-flight)
        producer = os.path.join(spirv_dir, name + ".spv")
        if not os.path.isfile(producer) and isinstance(meta.get("spv"), str) and meta["spv"]:
            producer = os.path.join(spirv_dir, meta["spv"])
        seal_path = os.path.join(spirv_dir, name + SEAL_SUFFIX)
        try:
            seal = read_seal_file(seal_path)
        except FileNotFoundError:
            raise RuntimeError(f"{seal_path}: missing seal for declared mesh_compute {name!r} (translator "
                               "must seal at generation; regenerate the producer with this translator)") from None
        except (ValueError, OSError) as e:
            raise RuntimeError(f"{seal_path}: invalid seal for {name!r}: {e}") from None
        try:
            want_layout = layout_sha256(lay)
        except ValueError as e:
            raise RuntimeError(f"{index_path}: invalid mesh_compute layout for {name!r}: {e}") from None
        if seal["layout_sha256"] != want_layout:
            raise RuntimeError(f"{seal_path}: index layout does not match the seal for {name!r} "
                               "(tampered index while modules unchanged)")
        try:
            producer_hash = file_sha256(producer)
        except OSError:
            raise RuntimeError(f"{producer}: missing producer SPIR-V for sealed {name!r}") from None
        if seal["producer_sha256"] != producer_hash:
            raise RuntimeError(f"{seal_path}: producer bytes do not match the seal for {name!r} "
                               "(swapped or stale module)")
        with open(asm, encoding="utf-8") as f:
            asm_hash = hashlib.sha256(f.read().encode("utf-8")).hexdigest()
        entries[spv] = (name, asm, seal_path, seal, asm_hash)
    by_asm = {}
    for spv, (name, asm, seal_path, seal, asm_hash) in entries.items():
        by_asm.setdefault(asm_hash, []).append((spv, name, asm, seal_path, seal))
    need_groups = []
    for asm_hash, items in by_asm.items():
        need = []
        for spv, name, asm, seal_path, seal in items:
            if skip_unchanged and seal.get("companion_asm_sha256") == asm_hash:
                want_spv = seal.get("companion_spv_sha256")
                try:
                    if want_spv is not None and file_sha256(spv) == want_spv:
                        continue
                except OSError:
                    pass
            need.append((spv, name, asm, seal_path, seal))
        if need:
            need_groups.append((asm_hash, need))
    if not need_groups:
        return all_spv
    steps = [("spirv-as", tool_path("spirv-as")), ("spirv-val", tool_path("spirv-val"))]
    for asm_hash, need in need_groups:
        rep_asm = need[0][2]
        with tempfile.TemporaryDirectory(prefix=".mesh-vs-", dir=os.path.dirname(need[0][0])) as staging:
            staged_spv = os.path.join(staging, os.path.basename(need[0][0]))
            for tool_name, tool in steps:
                args = [rep_asm, "-o", staged_spv] if tool_name == "spirv-as" else [staged_spv]
                cmd = [tool, "--target-env", "vulkan1.2"] + args
                r = subprocess.run(cmd, capture_output=True, text=True)
                if r.returncode:
                    raise RuntimeError(f"{tool_name} failed for {rep_asm}:\n{r.stdout}{r.stderr}")
            staged_hash = file_sha256(staged_spv)
            for spv, name, asm, seal_path, seal in need:
                tmp_spv = None
                try:
                    with tempfile.NamedTemporaryFile(dir=os.path.dirname(spv) or ".",
                                                     prefix="." + os.path.basename(spv) + "-",
                                                     suffix=".tmp", delete=False) as f:
                        tmp_spv = f.name
                    shutil.copyfile(staged_spv, tmp_spv)
                    os.replace(tmp_spv, spv)
                    tmp_spv = None
                finally:
                    if tmp_spv is not None:
                        try:
                            os.unlink(tmp_spv)
                        except OSError:
                            pass
                seal["companion_asm_sha256"] = asm_hash
                seal["companion_spv_sha256"] = staged_hash
                try:
                    write_seal_file(seal_path, seal)
                except OSError as e:
                    raise RuntimeError(f"{seal_path}: cannot seal companion for {name!r}: {e}") from None
    return all_spv


def build_for_pack(spirv_root, subdirs=None, *, skip_unchanged=True):
    """Build companions for a pack's main index plus its auxiliary indices, retaining pack structure.

    Each index.json found under <spirv_root>[/<sub>] gets its companions next to its own producers (never
    flattened into one folder). subdirs defaults to PACK_SUBDIRS (main + fspx/fspx_compute/fspx_gfx/bvh/exe/
    library); missing index.json files are skipped. Returns all .spv paths (sorted, including skipped-
    unchanged). A 128-byte pc-tail layout anywhere raises RuntimeError naming its index and entry.
    """
    if subdirs is None:
        subdirs = PACK_SUBDIRS
    built = []
    for sub in subdirs:
        index_dir = spirv_root if not sub else os.path.join(spirv_root, sub)
        if not os.path.isfile(os.path.join(index_dir, "index.json")):
            continue
        built.extend(build_for_index(index_dir, skip_unchanged=skip_unchanged))
    return sorted(built)


def main(argv):
    if argv[1:] in (["-h"], ["--help"]):
        print(__doc__)
        return 0
    args = argv[1:]
    pack = False
    if args[:1] == ["--pack"]:
        pack = True
        args = args[1:]
    if not args or (pack and len(args) != 1):
        print(__doc__, file=sys.stderr)
        return 2
    for directory in args:
        if not os.path.isdir(directory):
            print(f"ERROR: {directory}: not a directory", file=sys.stderr)
            return 1
    try:
        if pack:
            built = build_for_pack(args[0])
        else:
            built = []
            for directory in args:
                built.extend(build_for_index(directory))
    except RuntimeError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1
    except OSError as e:
        print(f"ERROR: {e.filename or args[0]}: {e.strerror or e}", file=sys.stderr)
        return 1
    for path in built:
        print(path)
    return 0


if __name__ == "__main__":
    try:  # a redirected stdout is strict ANSI code page under -I; write '?' for what it cannot encode
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError, OSError):
        pass
    sys.exit(main(sys.argv))
