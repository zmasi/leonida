"""The rectangle-list geometry stage (runtime/src/rect_list_gs.h), run on the CPU.

The D3D12 layer makes the stage at pipeline creation; runtime/test/rect_list_gs_tool.cpp runs the same generator on
vertex shaders this test assembles with tools/spirv_asm.py, and a small interpreter here runs the geometry shader it
writes on three vertices. Checked: the corner orders of the title's full-screen passes (top left, bottom left, bottom
right) and tiled lighting (top left, bottom left, top right) and the third one, every float output continued into
the fourth corner and integer outputs kept from the first vertex, both triangles led by the first vertex and turned
as the title's; the triangles that stay triangles (w unequal or not positive, no corner, two vertices in one place);
clip distances, glslang's gl_PerVertex block, the cache per output signature and the refusals. spirv-val, on PATH or
in VULKAN_SDK, checks every module. The rule is VIctory's (GPL-3.0-or-later, Marvisak and contributors, DexNom fork;
victory/crates/victory-shader/src/emit/rect.rs).

Standard library only; no GPU, no game data. LEONIDA_RECT_LIST_GS_TOOL names the built rect_list_gs_tool (ctest's
rect_list_gs sets it); without it the tool is looked for under runtime/build*/, and the tests skip when there is none.

    python -I tests/test_rect_list_gs.py
"""
import copy
import glob
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS_DIR = os.path.join(ROOT, "tools")
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

from spirv_asm import assemble  # noqa: E402


def find_tool():
    named = os.environ.get("LEONIDA_RECT_LIST_GS_TOOL")
    if named:
        return os.path.abspath(named)
    found = []
    for build in glob.glob(os.path.join(ROOT, "runtime", "build*")):
        for config in ("RelWithDebInfo", "Release", "Debug", ""):
            for name in ("rect_list_gs_tool.exe", "rect_list_gs_tool"):
                path = os.path.join(build, config, name)
                if os.path.isfile(path):
                    found.append(path)
    return max(found, key=os.path.getmtime) if found else None


def find_spirv_val():
    path = shutil.which("spirv-val")
    if path:
        return path
    sdk = os.environ.get("VULKAN_SDK")
    for name in ("spirv-val.exe", "spirv-val"):
        for sub in ("Bin", "bin"):
            candidate = os.path.join(sdk, sub, name) if sdk else ""
            if candidate and os.path.isfile(candidate):
                return candidate
    return None


TOOL = find_tool()
SPIRV_VAL = find_spirv_val()

# ---- Vertex shaders ----

TYPES = """%void = OpTypeVoid
%fn = OpTypeFunction %void
%float = OpTypeFloat 32
%int = OpTypeInt 32 1
%uint = OpTypeInt 32 0
%v2float = OpTypeVector %float 2
%v4float = OpTypeVector %float 4
%v2uint = OpTypeVector %uint 2
%uint_1 = OpConstant %uint 1
%uint_2 = OpConstant %uint 2
%uint_4 = OpConstant %uint 4
%float_1 = OpTypeArray %float %uint_1
%float_2 = OpTypeArray %float %uint_2
%v4float_4 = OpTypeArray %v4float %uint_4
%gl_PerVertex = OpTypeStruct %v4float %float %float_1 %float_1
"""
BLOCK_DECORATIONS = ["OpMemberDecorate %gl_PerVertex 0 BuiltIn Position",
                     "OpMemberDecorate %gl_PerVertex 1 BuiltIn PointSize",
                     "OpMemberDecorate %gl_PerVertex 2 BuiltIn ClipDistance",
                     "OpMemberDecorate %gl_PerVertex 3 BuiltIn CullDistance",
                     "OpDecorate %gl_PerVertex Block"]


def vertex_shader(outputs, capabilities=(), decorations=()):
    """A vertex shader declaring `outputs` = [(name, pointee type, [decoration words])]; its body writes nothing
    (the generator reads only the interface)."""
    lines = ["OpCapability Shader", *[f"OpCapability {c}" for c in capabilities], "OpMemoryModel Logical GLSL450",
             'OpEntryPoint Vertex %main "main" ' + " ".join(f"%{n}" for n, _, _ in outputs)]
    lines += [f"OpDecorate %{n} {d}" for n, _, decs in outputs for d in decs]
    lines += list(decorations)
    lines.append(TYPES)
    pointees = sorted({t for _, t, _ in outputs})
    lines += [f"%out_{t} = OpTypePointer Output %{t}" for t in pointees]
    lines += [f"%{n} = OpVariable %out_{t} Output" for n, t, _ in outputs]
    lines += ["%main = OpFunction %void None %fn", "%entry = OpLabel", "OpReturn", "OpFunctionEnd"]
    return assemble("\n".join(lines) + "\n", target_env="vulkan1.2")


# The translator's form: Position alone and vec4 params, here also an integer and a partial-location output.
TRANSLATOR_VS = [("out_pos", "v4float", ["BuiltIn Position"]), ("out0", "v4float", ["Location 0"]),
                 ("out1", "v2uint", ["Location 1"]), ("out2", "v2float", ["Location 2", "Component 2"])]
CLIP_VS = TRANSLATOR_VS[:2] + [("out_clip", "float_2", ["BuiltIn ClipDistance"])]
GLSLANG_VS = [("pv", "gl_PerVertex", []), ("color", "v4float", ["Location 0"])]


def run_tool(shaders):
    """Runs the generator on each vertex shader in one process: [(gs words, first index with the same words, key)],
    or raises Refused."""
    with tempfile.TemporaryDirectory() as tmp:
        args = []
        for i, vs in enumerate(shaders):
            vs_path, gs_path = os.path.join(tmp, f"vs{i}.spv"), os.path.join(tmp, f"gs{i}.spv")
            with open(vs_path, "wb") as f:
                f.write(vs)
            args += [vs_path, gs_path]
        r = subprocess.run([TOOL, *args], capture_output=True, text=True, timeout=60)
        if r.returncode == 2:
            raise Refused(r.stdout.strip())
        if r.returncode:
            raise AssertionError(f"rect_list_gs_tool failed ({r.returncode}): {r.stdout}{r.stderr}")
        results = []
        for i, line in enumerate(r.stdout.splitlines()):
            index, first, key = line.split(" ", 2)
            assert int(index) == i, line
            with open(os.path.join(tmp, f"gs{i}.spv"), "rb") as f:
                data = f.read()
            results.append((list(struct.unpack(f"<{len(data) // 4}I", data)), int(first), key))
        return results


class Refused(Exception):
    pass


# ---- A geometry-shader interpreter for the generator's subset of SPIR-V ----

OP = {"capability": 17, "entry_point": 15, "execution_mode": 16, "decorate": 71, "member_decorate": 72,
      "type_void": 19, "type_bool": 20, "type_int": 21, "type_float": 22, "type_vector": 23, "type_array": 28,
      "type_struct": 30, "type_pointer": 32, "type_function": 33, "constant": 43, "variable": 59, "function": 54,
      "function_end": 56, "label": 248, "return": 253, "load": 61, "store": 62, "access_chain": 65,
      "composite_construct": 80, "composite_extract": 81, "bitcast": 124, "f_add": 129, "f_sub": 131,
      "logical_or": 166, "logical_and": 167, "logical_not": 168, "select": 169, "i_equal": 170,
      "f_ord_equal": 180, "f_ord_greater_than": 186, "emit_vertex": 218, "end_primitive": 219,
      "memory_model": 14}
TYPE_OPS = {OP[k] for k in ("type_void", "type_bool", "type_int", "type_float", "type_vector", "type_array",
                            "type_struct", "type_pointer", "type_function")}
POSITION, POINT_SIZE, CLIP, CULL, PRIMITIVE_ID = 0, 1, 3, 4, 7
BUILTIN_KEYS = {POSITION: "pos", POINT_SIZE: "size", CLIP: "clip", CULL: "cull", PRIMITIVE_ID: "prim"}
LOCATION, COMPONENT, BUILTIN, BLOCK = 30, 31, 11, 2


def bits(x):
    try:
        return struct.unpack("<I", struct.pack("<f", x))[0]
    except OverflowError:
        return 0xFF800000 if x < 0 else 0x7F800000


def flt(b):
    return struct.unpack("<f", struct.pack("<I", b))[0]


def lift(fn, *args):
    if isinstance(args[0], tuple):
        return tuple(fn(*parts) for parts in zip(*args))
    return fn(*args)


class Gs:
    """The generated module, parsed; run() executes its one function on three vertices."""

    def __init__(self, words):
        assert words[0] == 0x07230203, "not SPIR-V"
        self.words = words
        self.version = words[1]
        self.capabilities, self.modes, self.interface = [], [], []
        self.model = None
        self.types, self.constants, self.variables = {}, {}, {}
        self.decorations, self.member_decorations = {}, {}
        self.body = []
        in_function = False
        at = 5
        while at < len(words):
            count, code = words[at] >> 16, words[at] & 0xFFFF
            assert count, "a zero-length instruction"
            ops = words[at + 1:at + count]
            at += count
            if code == OP["capability"]:
                self.capabilities.append(ops[0])
            elif code == OP["entry_point"]:
                self.model = ops[0]
                i = 2
                while i < len(ops):
                    w = ops[i]
                    i += 1
                    if not all((w >> s) & 0xFF for s in (0, 8, 16, 24)):
                        break
                self.interface = list(ops[i:])
            elif code == OP["execution_mode"]:
                self.modes.append(tuple(ops[1:]))
            elif code == OP["decorate"]:
                self.decorations.setdefault(ops[0], {})[ops[1]] = tuple(ops[2:])
            elif code == OP["member_decorate"]:
                self.member_decorations.setdefault((ops[0], ops[1]), {})[ops[2]] = tuple(ops[3:])
            elif code in TYPE_OPS:
                self.types[ops[0]] = (code, tuple(ops[1:]))
            elif code == OP["constant"]:
                self.constants[ops[1]] = ops[2]
            elif code == OP["variable"]:
                self.variables[ops[1]] = (ops[0], ops[2])
            elif code == OP["function"]:
                in_function = True
            elif code == OP["function_end"]:
                in_function = False
            elif in_function:
                self.body.append((code, ops))
            elif code != OP["memory_model"]:
                raise NotImplementedError(f"opcode {code} outside the function")

    def pointee(self, var):
        return self.types[self.variables[var][0]][1][1]

    def storage(self, var):
        return self.variables[var][1]

    def is_float(self, t):
        code, ops = self.types[t]
        if code == OP["type_vector"]:
            return self.is_float(ops[0])
        if code == OP["type_array"]:
            return self.is_float(ops[0])
        return code == OP["type_float"]

    def element(self, t):
        return self.types[t][1][0]

    def default(self, t):
        code, ops = self.types[t]
        if code == OP["type_struct"]:
            return [self.default(m) for m in ops]
        if code == OP["type_array"]:
            return [self.default(ops[0]) for _ in range(self.constants[ops[1]])]
        return None

    def struct_builtins(self, t):
        return [self.member_decorations[(t, m)][BUILTIN][0] for m in range(len(self.types[t][1]))]

    def encode(self, t, value):
        """A test value as the bits the variable of type `t` holds."""
        code, ops = self.types[t]
        if code == OP["type_vector"]:
            return tuple(self.encode(ops[0], v) for v in value)
        if code == OP["type_array"]:
            return [self.encode(ops[0], v) for v in value]
        if code == OP["type_float"]:
            return value if isinstance(value, Bits) else bits(value)
        return value & 0xFFFFFFFF

    def inputs(self, vertices, primitive_id):
        """The value of every input variable for three vertices: dicts with "pos", "clip", "cull" and
        (location, component) keys."""
        values = {}
        for var in self.interface:
            if self.storage(var) != 1:
                continue
            deco = self.decorations.get(var, {})
            t = self.pointee(var)
            if BUILTIN in deco and deco[BUILTIN][0] == PRIMITIVE_ID:
                values[var] = primitive_id
                continue
            element = self.element(t)  # the per-vertex array
            per_vertex = []
            for v in vertices:
                if BUILTIN in deco:
                    per_vertex.append(self.encode(element, v[BUILTIN_KEYS[deco[BUILTIN][0]]]))
                elif LOCATION in deco:
                    key = (deco[LOCATION][0], deco.get(COMPONENT, (0,))[0])
                    per_vertex.append(self.encode(element, v[key]))
                else:  # gl_PerVertex
                    members = self.types[element][1]
                    value = []
                    for m, b in enumerate(self.struct_builtins(element)):
                        given = v.get(BUILTIN_KEYS[b], 1.0 if b == POINT_SIZE else None)
                        if given is None:
                            given = [0.0] * self.constants[self.types[members[m]][1][1]]
                        value.append(self.encode(members[m], given))
                    per_vertex.append(value)
            values[var] = per_vertex
        return values

    def run(self, vertices, primitive_id=5):
        """The strips the stage emits: [[{output key: bits}]]."""
        inputs = self.inputs(vertices, primitive_id)
        outputs = {var: self.default(self.pointee(var)) for var in self.interface if self.storage(var) == 3}
        values = dict(self.constants)
        for var in self.variables:
            values[var] = ("ptr", var, ())
        strips, strip = [], []

        def walk(root, path):
            for i in path:
                root = root[i]
            return root

        for code, ops in self.body:
            if code == OP["label"]:
                continue
            if code == OP["return"]:
                break
            if code == OP["access_chain"]:
                _, result, base, *indices = ops
                _, var, path = values[base]
                values[result] = ("ptr", var, path + tuple(values[i] for i in indices))
            elif code == OP["load"]:
                _, result, pointer = ops
                _, var, path = values[pointer]
                source = inputs if self.storage(var) == 1 else outputs
                values[result] = copy.deepcopy(walk(source[var], path))
            elif code == OP["store"]:
                _, var, path = values[ops[0]]
                assert self.storage(var) == 3, "a store to an input"
                if not path:
                    outputs[var] = copy.deepcopy(values[ops[1]])
                else:
                    walk(outputs[var], path[:-1])[path[-1]] = copy.deepcopy(values[ops[1]])
            elif code == OP["composite_extract"]:
                _, result, composite, *indices = ops
                values[result] = walk(values[composite], indices)
            elif code == OP["composite_construct"]:
                _, result, *parts = ops
                values[result] = tuple(values[p] for p in parts)
            elif code == OP["bitcast"]:
                values[ops[1]] = values[ops[2]]
            elif code in (OP["f_add"], OP["f_sub"]):
                sign = 1 if code == OP["f_add"] else -1
                values[ops[1]] = lift(lambda a, b: bits(flt(a) + sign * flt(b)), values[ops[2]], values[ops[3]])
            elif code == OP["i_equal"]:
                values[ops[1]] = lift(lambda a, b: a == b, values[ops[2]], values[ops[3]])
            elif code == OP["f_ord_equal"]:
                values[ops[1]] = lift(lambda a, b: flt(a) == flt(b), values[ops[2]], values[ops[3]])
            elif code == OP["f_ord_greater_than"]:
                values[ops[1]] = lift(lambda a, b: flt(a) > flt(b), values[ops[2]], values[ops[3]])
            elif code == OP["logical_and"]:
                values[ops[1]] = lift(lambda a, b: a and b, values[ops[2]], values[ops[3]])
            elif code == OP["logical_or"]:
                values[ops[1]] = lift(lambda a, b: a or b, values[ops[2]], values[ops[3]])
            elif code == OP["logical_not"]:
                values[ops[1]] = lift(lambda a: not a, values[ops[2]])
            elif code == OP["select"]:
                condition, a, b = values[ops[2]], values[ops[3]], values[ops[4]]
                if isinstance(condition, tuple):
                    values[ops[1]] = tuple(x if c else y for c, x, y in zip(condition, a, b))
                else:
                    values[ops[1]] = a if condition else b
            elif code == OP["emit_vertex"]:
                strip.append(self.snapshot(outputs))
            elif code == OP["end_primitive"]:
                strips.append(strip)
                strip = []
            else:
                raise NotImplementedError(f"opcode {code}")
        assert not strip, "vertices emitted after the last strip ended"
        return strips

    def snapshot(self, outputs):
        vertex = {}
        for var, value in outputs.items():
            deco = self.decorations.get(var, {})
            if BUILTIN in deco:
                vertex[BUILTIN_KEYS[deco[BUILTIN][0]]] = copy.deepcopy(value)
            elif LOCATION in deco:
                vertex[(deco[LOCATION][0], deco.get(COMPONENT, (0,))[0])] = copy.deepcopy(value)
            else:
                for b, member in zip(self.struct_builtins(self.pointee(var)), value):
                    vertex[BUILTIN_KEYS[b]] = copy.deepcopy(member)
        return vertex


class Bits(int):
    """A float component given by its bit pattern."""


# ---- Vertices ----

def linear(x, y):
    """Float outputs that are linear in the position, with values whose sums are exact in binary32."""
    return {(0, 0): (x, y, 2 * x + 3 * y + 0.5, 0.25), (2, 2): (0.5 * x - 1, -y)}


def vertices(corners, w=(1.0, 1.0, 1.0)):
    out = []
    for i, ((x, y), wi) in enumerate(zip(corners, w)):
        v = {"pos": (x * wi, y * wi, 0.5, wi), (1, 0): (7 + i, 100 + i), "clip": [x + 1, 1 - y]}
        v.update(linear(x, y))
        out.append(v)
    return out


def encoded(vertex):
    """A test vertex as the bits the stage's variables hold."""
    out = {}
    for key, value in vertex.items():
        if key in ("pos", (0, 0), (2, 2)):
            out[key] = tuple(v if isinstance(v, Bits) else bits(v) for v in value)
        elif key == "clip":
            out[key] = [bits(v) for v in value]
        else:
            out[key] = tuple(value)
    return out


def area(a, b, c):
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])


def xy(vertex):
    return (flt(vertex["pos"][0]), flt(vertex["pos"][1]))


@unittest.skipUnless(TOOL, "rect_list_gs_tool is not built (set LEONIDA_RECT_LIST_GS_TOOL)")
class RectangleStage(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not os.path.isfile(TOOL):
            raise AssertionError(f"LEONIDA_RECT_LIST_GS_TOOL names no file: {TOOL}")
        cls.translator_gs = Gs(run_tool([vertex_shader(TRANSLATOR_VS)])[0][0])

    def check_rectangle(self, corners, corner, fourth):
        """Three corners whose corner between the other two is `corner`: two triangles led by vertex 0, the
        fourth corner at `fourth`, every float output continued into it and integers kept from vertex 0."""
        given = vertices(corners)
        strips = self.translator_gs.run(given)
        self.assertEqual([len(s) for s in strips], [3, 3])
        v = [encoded(g) for g in given]
        fourth_vertex = encoded({"pos": (*fourth, 0.5, 1.0), **linear(*fourth)})
        fourth_vertex[(1, 0)] = v[0][(1, 0)]  # an integer (flat) output: the first vertex's
        v.append(fourth_vertex)
        order = {0: [(0, 1, 3), (0, 3, 2)], 1: [(0, 1, 2), (0, 2, 3)], 2: [(0, 1, 2), (0, 3, 1)]}[corner]
        for strip, indices in zip(strips, order):
            for emitted, i in zip(strip, indices):
                for key in ("pos", (0, 0), (1, 0), (2, 2)):
                    self.assertEqual(emitted[key], v[i][key], f"vertex {i} of {indices}, output {key}")
                self.assertEqual(emitted["prim"], 5)
        # Both triangles turn as the title's does, and together cover the rectangle.
        title = area(*corners)
        halves = [area(*map(xy, strip)) for strip in strips]
        for half in halves:
            self.assertGreater(half * title, 0)
        self.assertEqual(sum(abs(h) for h in halves), 2 * abs(title))

    def test_full_screen_order_corner_second(self):
        # Top left, bottom left, bottom right: the fourth corner is the top right.
        self.check_rectangle([(-1.0, 1.0), (-1.0, -1.0), (1.0, -1.0)], 1, (1.0, 1.0))

    def test_tiled_lighting_order_corner_first(self):
        # Top left, bottom left, top right: the fourth corner is the bottom right.
        self.check_rectangle([(-0.75, 0.5), (-0.75, -0.25), (0.5, 0.5)], 0, (0.5, -0.25))

    def test_corner_third(self):
        self.check_rectangle([(-1.0, -1.0), (1.0, 1.0), (-1.0, 1.0)], 2, (1.0, -1.0))

    def test_corner_first_turned_the_other_way(self):
        # Top left, top right, bottom left: counter-clockwise on the screen.
        self.check_rectangle([(-1.0, 1.0), (1.0, 1.0), (-1.0, -1.0)], 0, (1.0, -1.0))

    def check_triangle(self, given):
        """The three vertices stay their triangle; the second triangle is empty."""
        strips = self.translator_gs.run(given)
        self.assertEqual([len(s) for s in strips], [3, 3])
        v = [encoded(g) for g in given]
        for emitted, i in zip(strips[0] + strips[1], (0, 1, 2, 0, 0, 1)):
            for key in ("pos", (0, 0), (1, 0), (2, 2)):
                self.assertEqual(emitted[key], v[i][key], f"vertex {i}, output {key}")
        self.assertEqual(area(*map(xy, strips[1])), 0)

    def test_unequal_w_stays_a_triangle(self):
        self.check_triangle(vertices([(-1.0, 1.0), (-1.0, -1.0), (1.0, -1.0)], w=(1.0, 2.0, 1.0)))
        self.check_triangle(vertices([(-1.0, 1.0), (-1.0, -1.0), (1.0, -1.0)], w=(2.0, 2.0, 1.0)))

    def test_w_not_positive_stays_a_triangle(self):
        self.check_triangle(vertices([(-1.0, 1.0), (-1.0, -1.0), (1.0, -1.0)], w=(-1.0, -1.0, -1.0)))
        self.check_triangle(vertices([(-1.0, 1.0), (-1.0, -1.0), (1.0, -1.0)], w=(0.0, 0.0, 0.0)))

    def test_no_corner_stays_a_triangle(self):
        self.check_triangle(vertices([(0.0, 0.0), (1.0, 0.5), (0.25, 1.0)]))
        self.check_triangle(vertices([(0.0, 0.0), (0.0, 1.0), (1.0, 0.5)]))  # shares x only

    def test_two_vertices_in_one_place_stay_a_triangle(self):
        self.check_triangle(vertices([(0.0, 0.0), (0.0, 0.0), (1.0, 1.0)]))

    def test_values_equal_at_the_corner_keep_their_bits(self):
        # a + b - corner in binary32: (2^24 + 1) - 1 is 2^24 - 1, not 2^24, and a NaN payload would be lost. Where a
        # (or b) has the corner's bits the fourth vertex takes b (or a) as it is.
        given = vertices([(-1.0, 1.0), (-1.0, -1.0), (1.0, -1.0)])
        nan = Bits(0x7FC00123)
        for g, first in zip(given, (16777216.0, 1.0, 1.0)):
            g[(0, 0)] = (first, nan, 0.1, -0.0)
        strips = self.translator_gs.run(given)
        fourth = strips[1][2]  # corner second: (v0, v2, v3)
        self.assertEqual(fourth[(0, 0)], (bits(16777216.0), 0x7FC00123, bits(0.1), bits(-0.0)))

    def test_clip_distances_continue_into_the_fourth_corner(self):
        gs = Gs(run_tool([vertex_shader(CLIP_VS, capabilities=["ClipDistance"])])[0][0])
        self.assertIn(32, gs.capabilities)  # ClipDistance
        strips = gs.run(vertices([(-1.0, 1.0), (-1.0, -1.0), (1.0, -1.0)]))
        self.assertEqual(strips[1][2]["pos"][:2], (bits(1.0), bits(1.0)))
        self.assertEqual(strips[1][2]["clip"], [bits(2.0), bits(0.0)])  # (x + 1, 1 - y) at (1, 1)
        self.assertEqual(strips[0][1]["clip"], [bits(0.0), bits(2.0)])

    def test_clip_distances_without_the_capability_are_not_copied(self):
        gs = Gs(run_tool([vertex_shader(CLIP_VS)])[0][0])
        self.assertNotIn(32, gs.capabilities)
        self.assertNotIn("clip", gs.run(vertices([(-1.0, 1.0), (-1.0, -1.0), (1.0, -1.0)]))[0][0])

    def test_glslang_block_is_mirrored(self):
        for capabilities, clip in (((), False), (("ClipDistance",), True)):
            gs = Gs(run_tool([vertex_shader(GLSLANG_VS, capabilities, BLOCK_DECORATIONS)])[0][0])
            blocks = [v for v in gs.interface if BUILTIN not in gs.decorations.get(v, {})
                      and LOCATION not in gs.decorations.get(v, {})]
            self.assertEqual(len(blocks), 2)  # gl_in[3] and the output block
            for var in blocks:
                t = gs.pointee(var)
                if gs.storage(var) == 1:
                    self.assertEqual(gs.constants[gs.types[t][1][1]], 3)
                    t = gs.element(t)
                self.assertIn(BLOCK, gs.decorations[t])
                self.assertEqual(gs.struct_builtins(t), [POSITION, POINT_SIZE, CLIP, CULL])
            self.assertEqual(32 in gs.capabilities, clip)
            corners = [(0.0, 0.0), (0.0, -1.0), (1.0, 0.0)]  # corner first
            given = [{"pos": (x, y, 0.0, 1.0), (0, 0): (x, -y, 0.0, 1.0), "clip": [x + y]} for x, y in corners]
            strips = gs.run(given)
            fourth = strips[0][2]
            self.assertEqual(fourth["pos"], tuple(bits(c) for c in (1.0, -1.0, 0.0, 1.0)))
            self.assertEqual(fourth[(0, 0)], tuple(bits(c) for c in (1.0, 1.0, 0.0, 1.0)))
            self.assertEqual(fourth["clip"], [bits(0.0)] if clip else [None])
            self.assertEqual(fourth["size"], None)  # a triangle has no point size: left unwritten

    def test_stage_interface(self):
        gs = self.translator_gs
        self.assertEqual(gs.version, 0x00010000)
        self.assertEqual(gs.model, 3)  # Geometry
        self.assertEqual(gs.capabilities, [1, 2])  # Shader, Geometry
        # Triangles, one invocation, a triangle strip of six vertices (two triangles).
        self.assertEqual(sorted(gs.modes), [(0, 1), (22,), (26, 6), (29,)])
        vector = {"v4float": (OP["type_float"], 4), "v2uint": (OP["type_int"], 2), "v2float": (OP["type_float"], 2)}
        located = {}
        for var in gs.interface:
            deco = gs.decorations.get(var, {})
            if LOCATION in deco:
                t = gs.pointee(var)
                if gs.storage(var) == 1:
                    self.assertEqual(gs.constants[gs.types[t][1][1]], 3)
                    t = gs.element(t)
                code, (component, count) = gs.types[t]
                located.setdefault((deco[LOCATION][0], deco.get(COMPONENT, (0,))[0]), []).append(
                    (gs.storage(var), gs.types[component][0], count))
        for name, pointee, decorations in TRANSLATOR_VS[1:]:
            words = dict(d.split() for d in decorations)
            key = (int(words["Location"]), int(words.get("Component", 0)))
            self.assertEqual(sorted(located[key]), [(1, *vector[pointee]), (3, *vector[pointee])], name)
        builtins = sorted((gs.decorations[v][BUILTIN][0], gs.storage(v)) for v in gs.interface
                          if BUILTIN in gs.decorations.get(v, {}))
        self.assertEqual(builtins, [(POSITION, 1), (POSITION, 3), (PRIMITIVE_ID, 1), (PRIMITIVE_ID, 3)])

    def test_cache_serves_one_signature_once(self):
        reordered = [TRANSLATOR_VS[2], TRANSLATOR_VS[0], TRANSLATOR_VS[3], TRANSLATOR_VS[1]]
        results = run_tool([vertex_shader(TRANSLATOR_VS), vertex_shader(reordered), vertex_shader(CLIP_VS)])
        self.assertEqual([first for _, first, _ in results], [0, 0, 2])
        self.assertEqual(results[0][0], results[1][0])
        self.assertEqual(results[0][2], results[1][2])
        self.assertNotEqual(results[0][2], results[2][2])

    def test_refusals_name_the_reason(self):
        cases = [
            ([("out0", "v4float", ["Location 0"])], (), "no Position"),
            (TRANSLATOR_VS[:1] + [("layer", "int", ["BuiltIn Layer"])], ("ShaderLayer",), "Layer"),
            (TRANSLATOR_VS[:1] + [("out3", "v4float_4", ["Location 3"])], (), "location 3"),
        ]
        for outputs, capabilities, reason in cases:
            with self.assertRaises(Refused, msg=reason) as refused:
                run_tool([vertex_shader(outputs, capabilities)])
            self.assertIn(reason, str(refused.exception))

    @unittest.skipUnless(SPIRV_VAL, "spirv-val is not on PATH or in VULKAN_SDK")
    def test_spirv_val_accepts_every_stage(self):
        shaders = [vertex_shader(TRANSLATOR_VS), vertex_shader(CLIP_VS, capabilities=["ClipDistance"]),
                   vertex_shader(GLSLANG_VS, (), BLOCK_DECORATIONS),
                   vertex_shader(GLSLANG_VS, ("ClipDistance", "CullDistance"), BLOCK_DECORATIONS),
                   vertex_shader(TRANSLATOR_VS[:1])]
        with tempfile.TemporaryDirectory() as tmp:
            for i, (words, _, key) in enumerate(run_tool(shaders)):
                path = os.path.join(tmp, f"gs{i}.spv")
                with open(path, "wb") as f:
                    f.write(struct.pack(f"<{len(words)}I", *words))
                for env in ("vulkan1.0", "vulkan1.3"):
                    r = subprocess.run([SPIRV_VAL, "--target-env", env, path], capture_output=True, text=True)
                    self.assertEqual(r.returncode, 0, f"{key} ({env}): {r.stdout}{r.stderr}")


if __name__ == "__main__":
    unittest.main()
