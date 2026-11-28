#!/usr/bin/env python3
"""Give set 0's descriptor arrays fixed, non-overlapping Metal argument-buffer IDs (SHADER-161).

SPIRV-Cross (1.4.363.0) puts every runtime descriptor array of an argument buffer at one ID: with Leonida's
set 0 (sampled images at binding 0, samplers at 1, more image arrays at 3+) it comments all but one out as
"Overlapping binding", so the shader can't reach them. This rewrites a translated module for a native Metal
path only (the Vulkan module and layout stay as they are):

- each set-0 runtime descriptor array becomes a sized array with that binding's capacity from
  runtime/src/vulkan_bindless.h (`bindings`), and
- each set-0 Binding becomes the first ID of its range: the capacities of all lower bindings added up.

Convert the result with `spirv-cross --msl --msl-argument-buffers --msl-argument-buffer-tier 1
--msl-decoration-binding`, so the IDs are the same in every shader, whichever bindings it uses. The argument
buffer then holds binding b's element i at ID `metal_id(b) + i` (`--print-layout` prints the table).

Usage: spirv_metal_set0.py in.spv out.spv     |     spirv_metal_set0.py --print-layout
Needs spirv-dis and spirv-as (SPIRV-Tools) on PATH, or SPIRV_DIS / SPIRV_AS.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BINDLESS_H = os.path.join(ROOT, "runtime", "src", "vulkan_bindless.h")


def capacities(path=BINDLESS_H):
    """Descriptor counts by binding number, from the `bindings` table of vulkan_bindless.h."""
    with open(path, encoding="utf-8") as f:
        src = f.read()
    m = re.search(r"std::array<Binding,\s*(\d+)>\s*bindings\s*=\s*\{\{(.*?)\}\};", src, re.S)
    if not m:
        raise SystemExit(f"{path}: no `bindings` table")
    counts = [int(c) for c in re.findall(r"\{VK_DESCRIPTOR_TYPE_\w+,\s*(\d+)\}", m.group(2))]
    if len(counts) != int(m.group(1)):
        raise SystemExit(f"{path}: `bindings` declares {m.group(1)} entries, found {len(counts)}")
    return counts


def metal_ids(counts):
    """First argument-buffer ID of each binding: the capacities of the lower bindings added up."""
    ids, total = [], 0
    for c in counts:
        ids.append(total)
        total += c
    return ids


def _fresh_id(base, used):
    """Return `base` if unused, otherwise `base_1`, `base_2`, ... and record it in `used`."""
    cand = base
    i = 1
    while cand in used:
        cand = f"{base}_{i}"
        i += 1
    used.add(cand)
    return cand


def rewrite(text, counts):
    """Rewrite the disassembly (spirv-dis --raw-id is not needed; names are kept) of one module."""
    ids = metal_ids(counts)
    lines = text.splitlines()
    group_ids = set()
    group_targets = {}
    for ln in lines:
        m = re.match(r"\s*(%\S+) = OpDecorationGroup$", ln)
        if m:
            group_ids.add(m.group(1))
        m = re.match(r"\s*OpGroupDecorate (%\S+)\s+(.+)$", ln)
        if m:
            group_targets.setdefault(m.group(1), []).extend(m.group(2).split())

    direct_sets, direct_binds = {}, {}
    for ln in lines:
        m = re.match(r"\s*OpDecorate (%\S+) DescriptorSet (\d+)$", ln)
        if m:
            direct_sets[m.group(1)] = int(m.group(2))
        m = re.match(r"\s*OpDecorate (%\S+) Binding (\d+)$", ln)
        if m:
            direct_binds[m.group(1)] = int(m.group(2))

    sets = {k: v for k, v in direct_sets.items() if k not in group_ids}
    binds = {k: v for k, v in direct_binds.items() if k not in group_ids}
    for grp in group_ids:
        for target in group_targets.get(grp, ()):
            if grp in direct_sets:
                sets[target] = direct_sets[grp]
            if grp in direct_binds:
                binds[target] = direct_binds[grp]

    set0 = {v: b for v, b in binds.items() if sets.get(v) == 0}
    for v, b in set0.items():
        if b >= len(counts):
            raise SystemExit(f"{v}: binding {b} isn't in vulkan_bindless.h")

    all_set0_bind_groups = set()
    split_bind_groups = set()
    for grp in group_ids:
        if grp not in direct_binds:
            continue
        targets = group_targets.get(grp, ())
        has_set0 = any(t in set0 for t in targets)
        has_other = any(t not in set0 for t in targets)
        if has_set0 and has_other:
            split_bind_groups.add(grp)
        elif has_set0:
            all_set0_bind_groups.add(grp)

    defs = {}       # result id -> instruction text
    definition_positions: dict[str, int] = {}
    for idx, ln in enumerate(lines):
        m = re.match(r"\s*(%\S+) = (.*)$", ln)
        if m:
            defs[m.group(1)] = m.group(2)
            definition_positions[m.group(1)] = idx
    uint = next((r for r, d in defs.items() if d == "OpTypeInt 32 0"), None)

    candidates = []
    for order, (v, b) in enumerate(set0.items()):
        var = re.match(r"OpVariable (%\S+) UniformConstant$", defs.get(v, ""))
        if not var:
            continue    # the hash table (a storage buffer) and other non-array resources keep their type
        old_ptr = var.group(1)
        ptr = re.match(r"OpTypePointer UniformConstant (%\S+)$", defs.get(old_ptr, ""))
        rt = re.match(r"OpTypeRuntimeArray (%\S+)$", defs.get(ptr.group(1), "")) if ptr else None
        if not rt:
            continue
        candidates.append((b, definition_positions[old_ptr], order, v, old_ptr, rt.group(1)))

    used_ids = set(re.findall(r"%\S+", text))
    if candidates:
        first_pointer = min(candidate[1] for candidate in candidates)
        if uint is None:
            uint = _fresh_id("%msl_uint", used_ids)
            lines.insert(first_pointer, f"{uint} = OpTypeInt 32 0")
        else:
            uint_position = definition_positions[uint]
            if uint_position > first_pointer:
                # The generated length may precede an original declaration of its integer type.
                lines.insert(first_pointer, lines.pop(uint_position))

    cap_by_bind = {}    # binding -> OpConstant id (emitted once per binding)
    ptr_by_key = {}     # (binding, elem_type) -> new OpTypePointer id
    new_types = []      # inserted right after the variable's pointer type is declared
    retype = {}         # resized variable, copy, or helper param ID -> new pointer type
    for b, _, _, v, old_ptr, elem in sorted(candidates):
        if (b, elem) in ptr_by_key:
            retype[v] = ptr_by_key[(b, elem)]
            continue
        decl = []
        cap = cap_by_bind.get(b)
        if cap is None:
            cap = _fresh_id(f"%msl_cap_b{b}", used_ids)
            cap_by_bind[b] = cap
            decl.append(f"{cap} = OpConstant {uint} {counts[b]}")
        arr = _fresh_id(f"%msl_arr_b{b}", used_ids)
        nptr = _fresh_id(f"%msl_ptr_b{b}", used_ids)
        decl.extend([
            f"{arr} = OpTypeArray {elem} {cap}",
            f"{nptr} = OpTypePointer UniformConstant {arr}",
        ])
        new_types.append((old_ptr, decl))
        ptr_by_key[(b, elem)] = nptr
        retype[v] = nptr

    fn_type_defs = {}
    for tid, body in defs.items():
        m = re.match(r"OpTypeFunction\s+(%\S+)(.*)$", body)
        if m:
            fn_type_defs[tid] = (m.group(1), tuple(m.group(2).split()))

    fn_info = {}
    cur_fn = None
    for ln in lines:
        m = re.match(r"\s*(%\S+) = OpFunction (%\S+) (\S+) (%\S+)$", ln)
        if m:
            fn_id, ret_ty, ctrl, fn_ty = m.groups()
            cur_fn = fn_id
            fn_info[fn_id] = {"ret": ret_ty, "ctrl": ctrl, "fn_ty": fn_ty, "params": []}
            continue
        m = re.match(r"\s*(%\S+) = OpFunctionParameter (%\S+)$", ln)
        if m and cur_fn is not None:
            fn_info[cur_fn]["params"].append((m.group(1), m.group(2)))
            continue
        if cur_fn is not None and not ln.strip().startswith(";"):
            cur_fn = None

    copies = []
    calls = []
    for ln in lines:
        m = re.match(r"\s*(%\S+) = OpCopyObject (%\S+) (%\S+)$", ln)
        if m:
            copies.append(m.groups())
            continue
        m = re.match(r"\s*(%\S+) = OpFunctionCall (%\S+) (%\S+)(.*)$", ln)
        if m:
            res_id, ret_ty, callee_id, rest_args = m.groups()
            calls.append((res_id, ret_ty, callee_id, rest_args.split()))

    # Phase 1: converge retyped pointer IDs across OpCopyObject and OpFunctionCall.
    changed = True
    while changed:
        changed = False
        for res_id, _, src_id in copies:
            if src_id in retype and res_id not in retype:
                retype[res_id] = retype[src_id]
                changed = True
        for _, _, callee_id, args in calls:
            info = fn_info.get(callee_id)
            if not info:
                continue
            for (p_id, _), arg_id in zip(info["params"], args):
                if arg_id in retype and p_id not in retype:
                    retype[p_id] = retype[arg_id]
                    changed = True

    # Phase 2: ensure every call site passes a consistent pointer type for each parameter.
    for _, _, callee_id, args in calls:
        info = fn_info.get(callee_id)
        if not info:
            continue
        for idx, ((p_id, p_ty), arg_id) in enumerate(zip(info["params"], args)):
            expected_ty = retype.get(p_id, p_ty)
            actual_ty = retype.get(arg_id, p_ty)
            if actual_ty != expected_ty:
                raise SystemExit(
                    f"{callee_id}: parameter {idx} ({p_id}) called with conflicting descriptor array types"
                )

    for ln in lines:
        m = re.match(r"\s*(%\S+) = (OpPhi|OpSelect)\s+(%\S+)\s+(.+)$", ln)
        if m:
            res_id, opname, _, operands_str = m.groups()
            if res_id in retype or any(tok in retype for tok in operands_str.split()):
                raise SystemExit(f"{res_id}: {opname} of resized descriptor pointer is not supported")

    fn_retype = {}
    fn_type_by_key = {}
    new_fn_types = []
    for fn_id, info in fn_info.items():
        if not any(p_id in retype for p_id, _ in info["params"]):
            continue
        old_fn_ty = info["fn_ty"]
        ret_ty, _ = fn_type_defs[old_fn_ty]
        new_param_tys = tuple(retype.get(p_id, p_ty) for p_id, p_ty in info["params"])
        key = (old_fn_ty, new_param_tys)
        new_fn_ty = fn_type_by_key.get(key)
        if new_fn_ty is None:
            new_fn_ty = _fresh_id(f"%msl_fn_{fn_id.lstrip('%')}", used_ids)
            fn_type_by_key[key] = new_fn_ty
            ops = " ".join((ret_ty, *new_param_tys))
            new_fn_types.append((old_fn_ty, f"{new_fn_ty} = OpTypeFunction {ops}"))
        fn_retype[fn_id] = new_fn_ty

    out = []
    for ln in lines:
        m = re.match(r"^(\s*)OpDecorate (%\S+) Binding (\d+)$", ln)
        if m:
            indent, target, b_str = m.groups()
            if target in split_bind_groups:
                b_val = int(b_str)
                for grp_target in group_targets[target]:
                    mapped = ids[b_val] if grp_target in set0 else b_val
                    out.append(f"{indent}OpDecorate {grp_target} Binding {mapped}")
                continue
            if target in set0 or target in all_set0_bind_groups:
                ln = f"{indent}OpDecorate {target} Binding {ids[int(b_str)]}"
        m = re.match(r"(\s*(%\S+) = OpVariable )(%\S+)( UniformConstant)$", ln)
        if m and m.group(2) in retype:
            ln = f"{m.group(1)}{retype[m.group(2)]}{m.group(4)}"
        # Retype copies by precomputed result ID so out-of-order blocks and helpers stay consistent.
        m = re.match(r"(\s*(%\S+) = OpCopyObject )%\S+( %\S+)$", ln)
        if m and m.group(2) in retype:
            ln = f"{m.group(1)}{retype[m.group(2)]}{m.group(3)}"
        m = re.match(r"(\s*(%\S+) = OpFunction %\S+ \S+ )%\S+$", ln)
        if m and m.group(2) in fn_retype:
            ln = f"{m.group(1)}{fn_retype[m.group(2)]}"
        m = re.match(r"(\s*(%\S+) = OpFunctionParameter )%\S+$", ln)
        if m and m.group(2) in retype:
            ln = f"{m.group(1)}{retype[m.group(2)]}"
        out.append(ln)
        m = re.match(r"\s*(%\S+) = OpTypePointer", ln)
        if m:
            for after, decl in new_types:
                if after == m.group(1):
                    out.extend(decl)
        m = re.match(r"\s*(%\S+) = OpTypeFunction", ln)
        if m:
            for after, decl_line in new_fn_types:
                if after == m.group(1):
                    out.append(decl_line)
    return "\n".join(out) + "\n"


def main(argv):
    rest = argv[1:]
    if rest and rest[0] in ("-h", "--help"):
        for line in __doc__.strip().splitlines()[-2:]:
            print(line)
        return 0
    counts = capacities()
    if argv[1:] == ["--print-layout"]:
        for b, (c, i) in enumerate(zip(counts, metal_ids(counts))):
            print(f"binding {b:2}: {c:5} descriptors at IDs {i}..{i + c - 1}")
        return 0
    if len(argv) != 3:
        print(__doc__.strip().splitlines()[-2], file=sys.stderr)
        return 2
    dis, asm = _tools()
    # encoding="utf-8", errors="replace": keep non-ASCII OpNames intact across Windows code pages while
    # staying safe on unexpected invalid bytes from external tools.
    text = _run([dis, argv[1]], capture_output=True, text=True, encoding="utf-8", errors="replace").stdout
    env = re.search(r"; Version: (\d+)\.(\d+)", text)
    target = f"spv{env.group(1)}.{env.group(2)}" if env else "vulkan1.2"
    _run([asm, "--target-env", target, "-", "-o", argv[2]], input=rewrite(text, counts),
         capture_output=True, text=True, encoding="utf-8", errors="replace")
    return 0


def _tools():
    return os.environ.get("SPIRV_DIS", "spirv-dis"), os.environ.get("SPIRV_AS", "spirv-as")


def _run(cmd, **kwargs):
    """subprocess.run(check=True). Windows does not name the program it could not start, so name it here."""
    try:
        return subprocess.run(cmd, check=True, **kwargs)
    except OSError as e:
        if e.filename is None:
            e.filename = cmd[0]
        raise


def cli(argv):
    """main() for the command line: a SPIRV-Tools program that cannot be started, or that rejects its input, is
    one ERROR line and exit 1 instead of a traceback. main() itself still raises, for callers that catch it."""
    try:
        return main(argv)
    except subprocess.CalledProcessError as e:
        program = e.cmd[0] if isinstance(e.cmd, (list, tuple)) else e.cmd
        said = next((line.strip() for line in (e.stderr or "").splitlines() if line.strip()), "")
        print(f"ERROR: {program} failed with exit status {e.returncode}" + (f": {said}" if said else ""),
              file=sys.stderr)
    except OSError as e:
        hint = " (SPIRV-Tools must be on PATH, or set SPIRV_DIS / SPIRV_AS)" if e.filename in _tools() else ""
        print(f"ERROR: {e.filename or 'spirv_metal_set0.py'}: {e.strerror or e}{hint}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    try:  # a redirected stdout is strict ANSI code page under -I; write '?' for what it cannot encode
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError, OSError):
        pass
    sys.exit(cli(sys.argv))
