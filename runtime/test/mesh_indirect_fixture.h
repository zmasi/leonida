// Reference model and PSO helpers for d3d12_mesh_compute_indirect_test: the expected counts and depths of the cell
// pixel shader (glsl/ps_mesh_indirect_cell.frag) for a list of logical mesh grids, and the blend subobject
// mesh_compute_fixture.h lacks. No game data, no Vulkan, no D3D12 calls except building descriptors.
#pragma once

#include "mesh_compute_fixture.h"

#include <array>
#include <cstdint>

namespace meshindirecttest {

// A word nothing the test writes can equal, and a record XYZ that is invalid: 0x7E7E7E7E > 65535. The test fills
// every indirect buffer with it before the GPU writer stores the real words, so record padding, records the GPU
// never wrote and the words around the count stay it.
inline constexpr std::uint32_t kSentinel = 0x7E7E7E7Eu;
// D3D12 DispatchMesh limits (ExecuteIndirect shares them): each dimension <= 65535, total groups <= 2^22.
inline constexpr std::uint32_t kMaxDimension = 65535;
inline constexpr std::uint64_t kMaxGroups = std::uint64_t(1) << 22;
inline constexpr std::uint32_t kPixels = 256;  // the cell target: 16 x 16

// The groups one DISPATCH_MESH record runs: x * y * z when it is a legal grid, otherwise none. A zero dimension
// is a legal no-op; a dimension over 65535 or a product over 2^22 is an invalid record, which the replay
// must turn into zero work rather than wrap, truncate or fault on.
inline std::uint64_t groups_of(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    if (!x || !y || !z) return 0;
    if (x > kMaxDimension || y > kMaxDimension || z > kMaxDimension) return 0;
    const std::uint64_t groups = std::uint64_t(x) * y * z;
    return groups > kMaxGroups ? 0 : groups;
}

// What the cell shader leaves behind: per pixel the number of groups that landed on it (render target 0 is
// guard + count) and the last group that did (depth = g / 2^22, 1.0 where none did).
struct Plane {
    std::array<float, kPixels> count{};
    std::array<float, kPixels> depth{};
    Plane() { depth.fill(1.0f); }
};

// One logical grid of `groups` groups under roots (cells, width 16): group g lands on pixel g % cells when that
// pixel is under the scissor (the first `visible` pixels, row-major). Groups are applied in order, so the depth is
// the last group's, as depth test ALWAYS with writes on leaves it.
inline void apply(Plane& plane, std::uint64_t groups, std::uint32_t cells, std::uint32_t visible = kPixels) {
    for (std::uint64_t g = 0; g < groups; ++g) {
        const std::uint32_t pixel = std::uint32_t(g % cells);
        if (pixel >= kPixels || pixel >= visible) continue;
        plane.count[pixel] += 1.0f;
        plane.depth[pixel] = float(g) / 4194304.0f;
    }
}

using BlendSubobject = meshcomputetest::Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC>;

// Render target 0 adds the shader's output to what it holds.
inline D3D12_BLEND_DESC additive_blend() {
    D3D12_BLEND_DESC blend{};
    auto& rt = blend.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D12_BLEND_ONE;
    rt.DestBlend = D3D12_BLEND_ONE;
    rt.BlendOp = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt.DestBlendAlpha = D3D12_BLEND_ONE;
    rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt.LogicOp = D3D12_LOGIC_OP_NOOP;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    return blend;
}

inline D3D12_DEPTH_STENCIL_DESC depth_always_write() {
    D3D12_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = TRUE;
    depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    return depth;
}

inline D3D12_RT_FORMAT_ARRAY r32_target() {
    D3D12_RT_FORMAT_ARRAY formats{};
    formats.RTFormats[0] = DXGI_FORMAT_R32_FLOAT;
    formats.NumRenderTargets = 1;
    return formats;
}

}  // namespace meshindirecttest
