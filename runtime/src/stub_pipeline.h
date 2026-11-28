#pragma once

// Built-in stages for LEONIDA_STUB_PIPELINES (MVP-GPU-04): a graphics pipeline whose translated shader is missing is
// still created as a real Vulkan pipeline, from a vertex and a pixel stage written here, so its draws go through the
// replay path (render pass, attachments, layouts, push constants) and the validation layer like any other draw.
//
// The vertex stage reads no vertex input. With `cover` false (LEONIDA_STUB_PIPELINES=1) every vertex lands at the same
// point outside the clip volume, so the draw rasterizes nothing: no game target changes, as with the old no-op stub.
// With `cover` true (LEONIDA_STUB_PIPELINES=color) each run of three vertices is a triangle over the whole viewport
// and the pixel stage writes stub_colour to every render target the pipeline declares, so a stubbed pass shows up.
// Plain word building with no dependency beyond the standard library; d3d12_stub_pipelines_test draws with both.

#include <cstdint>
#include <cstring>
#include <vector>

namespace leonida {

// What a pixel-stage output location holds, from its render-target format: D3D12 writes an integer format from an
// integer output (SV_Target uint4 / int4), every other format from float.
enum class StubOutput : std::uint8_t { none, float_, uint_, sint_ };

// Magenta: exact in every UNORM format the layer draws to (4-bit channels included), 1 or 0 in integer ones.
inline constexpr float stub_colour[4] = {1.0f, 0.0f, 1.0f, 1.0f};

namespace stub_spirv {

enum : std::uint32_t {
    op_entry_point = 15, op_execution_mode = 16, op_capability = 17, op_memory_model = 14, op_type_void = 19,
    op_type_int = 21, op_type_float = 22, op_type_vector = 23, op_type_pointer = 32, op_type_function = 33,
    op_constant = 43, op_constant_composite = 44, op_function = 54, op_function_end = 56, op_variable = 59,
    op_load = 61, op_store = 62, op_decorate = 71, op_composite_construct = 80, op_convert_s_to_f = 111,
    op_f_sub = 131, op_f_mul = 133, op_s_mod = 139, op_shift_left_logical = 196, op_bitwise_and = 199, op_label = 248,
    op_return = 253,
};
enum : std::uint32_t { storage_input = 1, storage_output = 3 };
enum : std::uint32_t { decoration_builtin = 11, decoration_location = 30 };
enum : std::uint32_t { builtin_position = 0, builtin_point_size = 1, builtin_vertex_index = 42 };
enum : std::uint32_t { model_vertex = 0, model_fragment = 4, mode_origin_upper_left = 7 };

inline std::uint32_t bits(float f) {
    std::uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

// A module under construction: the header, then sections filled in SPIR-V's required order and joined by words().
class Builder {
public:
    std::uint32_t id() { return next_++; }
    void preamble(std::uint32_t op, std::initializer_list<std::uint32_t> operands) { put(head_, op, operands); }
    void entry_point(std::uint32_t model, std::uint32_t function, const std::vector<std::uint32_t>& io) {
        std::vector<std::uint32_t> operands = {model, function, 0x6e69616du /* "main" */, 0u};
        operands.insert(operands.end(), io.begin(), io.end());
        put_vector(entry_, op_entry_point, operands);
    }
    void execution_mode(std::initializer_list<std::uint32_t> operands) { put(entry_, op_execution_mode, operands); }
    void decorate(std::initializer_list<std::uint32_t> operands) { put(decorations_, op_decorate, operands); }
    void global(std::uint32_t op, std::initializer_list<std::uint32_t> operands) { put(globals_, op, operands); }
    void code(std::uint32_t op, std::initializer_list<std::uint32_t> operands) { put(code_, op, operands); }
    std::vector<std::uint32_t> words() const {
        std::vector<std::uint32_t> w = {0x07230203u, 0x00010000u, 0, next_, 0};
        for (const auto* section : {&head_, &entry_, &decorations_, &globals_, &code_})
            w.insert(w.end(), section->begin(), section->end());
        return w;
    }

private:
    static void put(std::vector<std::uint32_t>& out, std::uint32_t op, std::initializer_list<std::uint32_t> operands) {
        out.push_back(std::uint32_t(operands.size() + 1) << 16 | op);
        out.insert(out.end(), operands.begin(), operands.end());
    }
    static void put_vector(std::vector<std::uint32_t>& out, std::uint32_t op, const std::vector<std::uint32_t>& operands) {
        out.push_back(std::uint32_t(operands.size() + 1) << 16 | op);
        out.insert(out.end(), operands.begin(), operands.end());
    }
    std::uint32_t next_ = 1;
    std::vector<std::uint32_t> head_, entry_, decorations_, globals_, code_;
};

}  // namespace stub_spirv

// The vertex stage. Position is a bare Output variable, not a gl_PerVertex block, so rect_list_geometry can take this
// stage as it takes a translated one. `points`: the pipeline draws point lists, which need PointSize written.
inline std::vector<std::uint32_t> stub_vertex_stage(bool cover, bool points) {
    using namespace stub_spirv;
    Builder b;
    b.preamble(op_capability, {1 /* Shader */});
    b.preamble(op_memory_model, {0 /* Logical */, 1 /* GLSL450 */});
    const std::uint32_t t_void = b.id(), t_fn = b.id(), t_float = b.id(), t_vec4 = b.id(), t_int = b.id();
    const std::uint32_t p_out_vec4 = b.id(), p_out_float = b.id(), p_in_int = b.id();
    b.global(op_type_void, {t_void});
    b.global(op_type_function, {t_fn, t_void});
    b.global(op_type_float, {t_float, 32});
    b.global(op_type_vector, {t_vec4, t_float, 4});
    b.global(op_type_int, {t_int, 32, 1});
    b.global(op_type_pointer, {p_out_vec4, storage_output, t_vec4});
    b.global(op_type_pointer, {p_out_float, storage_output, t_float});
    b.global(op_type_pointer, {p_in_int, storage_input, t_int});
    const std::uint32_t f0 = b.id(), f1 = b.id(), f2 = b.id(), i1 = b.id(), i2 = b.id(), i3 = b.id();
    b.global(op_constant, {t_float, f0, bits(0.0f)});
    b.global(op_constant, {t_float, f1, bits(1.0f)});
    b.global(op_constant, {t_float, f2, bits(2.0f)});
    b.global(op_constant, {t_int, i1, 1});
    b.global(op_constant, {t_int, i2, 2});
    b.global(op_constant, {t_int, i3, 3});

    const std::uint32_t position = b.id();
    b.global(op_variable, {p_out_vec4, position, storage_output});
    b.decorate({position, decoration_builtin, builtin_position});
    std::vector<std::uint32_t> io = {position};
    std::uint32_t point_size = 0, vertex_index = 0;
    if (points) {
        point_size = b.id();
        b.global(op_variable, {p_out_float, point_size, storage_output});
        b.decorate({point_size, decoration_builtin, builtin_point_size});
        io.push_back(point_size);
    }
    if (cover) {
        vertex_index = b.id();
        b.global(op_variable, {p_in_int, vertex_index, storage_input});
        b.decorate({vertex_index, decoration_builtin, builtin_vertex_index});
        io.push_back(vertex_index);
    }
    std::uint32_t outside = 0;
    if (!cover) {  // x = y = 2w: past the right and top clip planes for every vertex
        outside = b.id();
        b.global(op_constant_composite, {t_vec4, outside, f2, f2, f0, f1});
    }

    const std::uint32_t main = b.id(), label = b.id();
    b.entry_point(model_vertex, main, io);
    b.code(op_function, {t_void, main, 0, t_fn});
    b.code(op_label, {label});
    std::uint32_t value = outside;
    if (cover) {  // the full-viewport triangle (-1,-1) (3,-1) (-1,3) from VertexIndex % 3
        const std::uint32_t vi = b.id(), m = b.id(), sh = b.id(), xi = b.id(), yi = b.id(), xf = b.id(), yf = b.id(),
                            x2 = b.id(), y2 = b.id(), x = b.id(), y = b.id(), pos = b.id();
        b.code(op_load, {t_int, vi, vertex_index});
        b.code(op_s_mod, {t_int, m, vi, i3});
        b.code(op_shift_left_logical, {t_int, sh, m, i1});
        b.code(op_bitwise_and, {t_int, xi, sh, i2});
        b.code(op_bitwise_and, {t_int, yi, m, i2});
        b.code(op_convert_s_to_f, {t_float, xf, xi});
        b.code(op_convert_s_to_f, {t_float, yf, yi});
        b.code(op_f_mul, {t_float, x2, xf, f2});
        b.code(op_f_mul, {t_float, y2, yf, f2});
        b.code(op_f_sub, {t_float, x, x2, f1});
        b.code(op_f_sub, {t_float, y, y2, f1});
        b.code(op_composite_construct, {t_vec4, pos, x, y, f0, f1});
        value = pos;
    }
    b.code(op_store, {position, value});
    if (points) b.code(op_store, {point_size, f1});
    b.code(op_return, {});
    b.code(op_function_end, {});
    return b.words();
}

// The pixel stage: stub_colour to output location i for each outputs[i] that is not none.
inline std::vector<std::uint32_t> stub_pixel_stage(const std::vector<StubOutput>& outputs) {
    using namespace stub_spirv;
    Builder b;
    b.preamble(op_capability, {1 /* Shader */});
    b.preamble(op_memory_model, {0 /* Logical */, 1 /* GLSL450 */});
    const std::uint32_t t_void = b.id(), t_fn = b.id(), t_float = b.id(), t_int = b.id(), t_uint = b.id();
    const std::uint32_t t_vec4 = b.id(), t_ivec4 = b.id(), t_uvec4 = b.id();
    const std::uint32_t p_vec4 = b.id(), p_ivec4 = b.id(), p_uvec4 = b.id();
    b.global(op_type_void, {t_void});
    b.global(op_type_function, {t_fn, t_void});
    b.global(op_type_float, {t_float, 32});
    b.global(op_type_int, {t_int, 32, 1});
    b.global(op_type_int, {t_uint, 32, 0});
    b.global(op_type_vector, {t_vec4, t_float, 4});
    b.global(op_type_vector, {t_ivec4, t_int, 4});
    b.global(op_type_vector, {t_uvec4, t_uint, 4});
    b.global(op_type_pointer, {p_vec4, storage_output, t_vec4});
    b.global(op_type_pointer, {p_ivec4, storage_output, t_ivec4});
    b.global(op_type_pointer, {p_uvec4, storage_output, t_uvec4});
    const std::uint32_t f0 = b.id(), f1 = b.id(), i0 = b.id(), i1 = b.id(), u0 = b.id(), u1 = b.id();
    b.global(op_constant, {t_float, f0, bits(0.0f)});
    b.global(op_constant, {t_float, f1, bits(1.0f)});
    b.global(op_constant, {t_int, i0, 0});
    b.global(op_constant, {t_int, i1, 1});
    b.global(op_constant, {t_uint, u0, 0});
    b.global(op_constant, {t_uint, u1, 1});
    const auto channel = [&](float c, std::uint32_t zero, std::uint32_t one) { return c != 0.0f ? one : zero; };
    const std::uint32_t c_vec4 = b.id(), c_ivec4 = b.id(), c_uvec4 = b.id();
    b.global(op_constant_composite, {t_vec4, c_vec4, channel(stub_colour[0], f0, f1), channel(stub_colour[1], f0, f1),
                                     channel(stub_colour[2], f0, f1), channel(stub_colour[3], f0, f1)});
    b.global(op_constant_composite, {t_ivec4, c_ivec4, channel(stub_colour[0], i0, i1), channel(stub_colour[1], i0, i1),
                                     channel(stub_colour[2], i0, i1), channel(stub_colour[3], i0, i1)});
    b.global(op_constant_composite, {t_uvec4, c_uvec4, channel(stub_colour[0], u0, u1), channel(stub_colour[1], u0, u1),
                                     channel(stub_colour[2], u0, u1), channel(stub_colour[3], u0, u1)});

    struct Out {
        std::uint32_t variable, value;
    };
    std::vector<Out> written;
    std::vector<std::uint32_t> io;
    for (std::uint32_t location = 0; location < outputs.size(); ++location) {
        if (outputs[location] == StubOutput::none) continue;
        const bool sint = outputs[location] == StubOutput::sint_, uint = outputs[location] == StubOutput::uint_;
        const std::uint32_t variable = b.id();
        b.global(op_variable, {sint ? p_ivec4 : uint ? p_uvec4 : p_vec4, variable, storage_output});
        b.decorate({variable, decoration_location, location});
        io.push_back(variable);
        written.push_back({variable, sint ? c_ivec4 : uint ? c_uvec4 : c_vec4});
    }

    const std::uint32_t main = b.id(), label = b.id();
    b.entry_point(model_fragment, main, io);
    b.execution_mode({main, mode_origin_upper_left});
    b.code(op_function, {t_void, main, 0, t_fn});
    b.code(op_label, {label});
    for (const Out& out : written) b.code(op_store, {out.variable, out.value});
    b.code(op_return, {});
    b.code(op_function_end, {});
    return b.words();
}

}  // namespace leonida
