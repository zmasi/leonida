// Helpers for d3d12_mesh_compute_test: stage the build's mesh-compute library,
// wrap its FSSM programs in game-shaped DXBC containers, build pipeline state
// streams, and compute the expected readback pixels. No game data: every
// shader is a hand-written .asm/.glsl fixture translated or compiled at build
// time by runtime/test/shaders/build_mesh_compute.py.
#pragma once

#include "d3d12_test_shaders.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace meshcomputetest {

inline std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// A game-shaped DXBC container for an FSSM program, with the DXIL shader kind
// the proxy reads. Mesh shaders are kind 13, as in d3d12_mesh_test (the third
// byte of the DXIL part's header); pixel and vertex shaders keep kind 0.
inline std::vector<std::uint8_t> graphics_container(const std::vector<std::uint8_t>& fssm,
                                                    std::uint8_t dxil_kind = 0) {
    auto container = d3d12test::shader_container(fssm);
    if (dxil_kind && container.size() > 36 + 8 + 2) container[36 + 8 + 2] = dxil_kind;
    return container;
}

inline std::vector<std::uint8_t> mesh_container(const std::vector<std::uint8_t>& fssm) {
    return graphics_container(fssm, 13);
}

// Copy the staged spirv/ folder (index.json, producers, generated vertex
// shaders, GLSL modules) to the temp library the device loads.
inline bool stage_library(const std::filesystem::path& staged_spirv,
                          const std::filesystem::path& library) {
    std::error_code ec;
    std::filesystem::create_directories(library, ec);
    if (ec) return false;
    for (const auto& entry : std::filesystem::directory_iterator(staged_spirv, ec)) {
        if (ec || !entry.is_regular_file()) continue;
        std::filesystem::copy_file(entry.path(), library / entry.path().filename(),
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) return false;
    }
    return !ec && std::filesystem::is_regular_file(library / "index.json");
}

// One pipeline state stream subobject as d3dx12.h declares it
// (CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT), as in d3d12_mesh_test.
template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename Inner>
struct alignas(void*) Subobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
    Inner inner;
};
using RootSubobject = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature*>;
using VsSubobject = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE>;
using PsSubobject = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE>;
using GsSubobject = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS, D3D12_SHADER_BYTECODE>;
using MsSubobject = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE>;
using TopologySubobject =
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE>;
using TargetsSubobject =
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY>;
using DepthStencilSubobject =
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC>;
using DsvFormatSubobject = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT>;
using RasterizerSubobject = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC>;

// Subobjects appended one after another: each is pointer-aligned and a multiple
// of the pointer size, so the bytes are what a struct of the same members would hold.
struct Stream {
    std::vector<std::uint8_t> bytes;
    template <typename S>
    Stream& add(const S& subobject) {
        static_assert(sizeof(S) % sizeof(void*) == 0);
        const std::size_t at = bytes.size();
        bytes.resize(at + sizeof(S));
        std::memcpy(bytes.data() + at, &subobject, sizeof(S));
        return *this;
    }
    D3D12_PIPELINE_STATE_STREAM_DESC desc(std::size_t trim = 0) {
        return {bytes.size() - trim, bytes.data()};
    }
};

inline D3D12_RT_FORMAT_ARRAY one_target() {
    D3D12_RT_FORMAT_ARRAY formats{};
    formats.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    formats.NumRenderTargets = 1;
    return formats;
}

// Solid fill, depth clip on, and the given cull mode; front faces are
// clockwise (FrontCounterClockwise FALSE), the D3D12 default.
inline D3D12_RASTERIZER_DESC cull_rasterizer(D3D12_CULL_MODE cull) {
    D3D12_RASTERIZER_DESC r{};
    r.FillMode = D3D12_FILL_MODE_SOLID;
    r.CullMode = cull;
    r.FrontCounterClockwise = FALSE;
    r.DepthClipEnable = TRUE;
    return r;
}

inline HRESULT create_pso(ID3D12Device* device, Stream& stream, d3d12test::Ref<ID3D12PipelineState>& out,
                          std::size_t trim = 0) {
    d3d12test::Ref<ID3D12Device2> device2;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device2.p)))) return E_NOINTERFACE;
    const D3D12_PIPELINE_STATE_STREAM_DESC desc = stream.desc(trim);
    out.reset();
    return device2->CreatePipelineState(&desc, IID_PPV_ARGS(&out.p));
}

inline D3D12_MESH_SHADER_TIER mesh_tier(ID3D12Device* device) {
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 options{};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options, sizeof(options))))
        return D3D12_MESH_SHADER_TIER(-1);
    return options.MeshShaderTier;
}

// Grid A (ms_v5_workgroup 2x3x4 through ps_mesh_group_fold.frag): the last of
// the 24 groups wins every pixel. g = 23, k = 1 gives (255, 255, 191, 255);
// a replay that dropped Z shows g = 5 (255, 0, 64), one that dropped Y g = 19
// (255, 255, 159). Exact: every fold input is a small integer in f32.
inline std::array<std::uint8_t, 4> fold_pixel(std::uint32_t g, std::uint32_t k) {
    const double r = std::fmod(double(g), 2.0);
    const double gg = std::fmod(std::floor(double(g) / 2.0), 2.0);
    const double b = (std::floor(double(g) / 4.0) + double(k)) / 8.0;
    const auto u8 = [](double v) { return std::uint8_t(std::lround(v * 255.0)); };
    return {u8(r), u8(gg), u8(b), 255};
}

// Grid B (ms_tri through ps_draw.frag with an (tintR, tintG, tintB) tint):
// param0 is (u, v, 0.5, 1) with u, v in 0..1 across the viewport, and the pixel
// shader outputs (u * tintR, tintG, 0.5 * tintB, 1). R interpolates, so the
// caller compares with a tolerance; G and B are exact.
inline double tri_red(std::uint32_t x, std::uint32_t size, double tint_r) {
    return (double(x) + 0.5) / double(size) * tint_r * 255.0;
}

inline std::uint8_t tri_green(double tint_g) { return std::uint8_t(std::lround(tint_g * 255.0)); }

inline std::uint8_t tri_blue(double tint_b) { return std::uint8_t(std::lround(0.5 * tint_b * 255.0)); }

}  // namespace meshcomputetest
