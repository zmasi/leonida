// GPU-driven ExecuteIndirect(DISPATCH_MESH) through the public Xbox D3D12 proxy, on a device without
// VK_EXT_mesh_shader (Vega 20): the terminal-only mesh command signature replayed over the mesh-as-compute path,
// with every argument and count word written by an ACTUAL compute dispatch (glsl/cs_mesh_indirect_args.comp stores
// masked recipe words, so no upload is passed off as GPU-written) and read by ExecuteIndirect after a real UAV ->
// INDIRECT_ARGUMENT barrier. The producers are the --mesh-compute translator output of the existing synthetic
// ms_v5_workgroup fixture (v5 = group * 32 + lane, so param0 carries the logical linear group g of its record),
// a wide twin of it whose output slot is big enough that one record needs several scratch tiles, and two
// direct-only copies of it (marker removed / altered). The consumer shader (glsl/ps_mesh_indirect_cell.frag) keeps
// one fragment per group, at pixel g % cells of a 16 x 16 target: render target 0 (R32_FLOAT, additive blend)
// counts the groups that reached each pixel and depth (D32, ALWAYS, writes on) holds g / 2^22 of the last one, so
// a skipped, doubled or reordered record or tile, a wrong group index (a tile that restarted its numbering at 0),
// a dropped Y or Z (the console clamp) or stale state at execution each move a pixel. Guard values (1024.0 and
// depth 1.0) sit under every pixel no group reaches, and the argument and count buffers are read back and must
// still hold exactly what the GPU writer stored: ExecuteIndirect never writes them.
//
// What is verified (Close() results and replay results are exact; pixels are compared exactly):
//   - Terminal DISPATCH_MESH signatures (12, 28 and 64-byte strides) are S_OK on the no-extension device, which
//     still reports MeshShaderTier NOT_SUPPORTED, refuses a native MeshEXT PSO (E_NOTIMPL, nulled pointer) and
//     ignores nothing: a stride below 12 is E_INVALIDARG.
//   - Fixed counts and GPU counts, nonzero argument and count offsets, padded strides (the padding holds a
//     sentinel and must be ignored), min(count, max), count = 0, count > max, max = 0, records past the count
//     holding legal-looking grids (must not run), zero X, zero Y and zero Z grids between real ones (no work, no
//     disturbance), invalid records (a dimension over 65535, a product over 2^22, an all-ones record) which run
//     as zero work, legal Y and Z (the complete XYZ grid, not the console's y = z = 1), 65535 groups in one
//     dimension, and, on the wide producer, records of exactly one tile's capacity, one group more and two tiles,
//     where the last group sits in a later scratch tile than the first (a tile that forgot its base shows another
//     g). Nothing here rasterizes the largest legal record (2^22 groups): that boundary, and 2^22 + 1, run the real
//     tile-preparation pass in vulkan_mesh_indirect_args_test, which reads only the tile commands it writes.
//   - SetPredication true/false (EQUAL_ZERO and NOT_EQUAL_ZERO, the value at a nonzero offset) around an indirect
//     call: a false predicate leaves the target and depth at their guards, and an unpredicated direct draw after it
//     renders; where VK_EXT_conditional_rendering is not enabled, the predicated list is refused whole.
//   - A call whose records x tiles exceed the replay's restart bound (fixed or counted) is refused at submission
//     (ReplayResult::unsupported) without executing, not hung and not truncated.
//   - Bundles: a fixed-count ExecuteIndirect recorded in a bundle that inherits the PSO of the calling list renders
//     as the direct call does; a counted one is invalid in the bundle (E_INVALIDARG at the bundle's Close); a
//     bundle's eligibility (inherited PSO, root, viewport) is decided when the parent list executes it. Not
//     covered: a bundle that sets its own PSO.
//   - State: the draw uses the roots, viewport/scissor and PSO recorded at the call, not those of later commands;
//     a direct DispatchMesh (untagged) between and after indirect calls is unchanged, on the marked producer and on
//     a direct-only one; a direct-only producer still runs direct.
//   - Recording: a positive count with no PSO, a compute or ordinary PSO, a direct-only or wrongly marked
//     producer, no graphics root signature, an oversized one or no viewport closes E_INVALIDARG (fixed and counted
//     alike); max = 0 is a no-op that closes S_OK whatever is bound; argument and count ranges, alignment and
//     the exact-fit last record are checked as D3D12 does.
// Set LEONIDA_VK_VALIDATION=0 to run without the validation layer; by default the test runs under
// VK_LAYER_KHRONOS_validation with synchronization validation and fails on any error (the layer missing falls back
// to a plain run unless LEONIDA_VK_REQUIRE_VALIDATION is set, which CTest does). Exit 77 only for no usable
// Vulkan device or a missing required feature or limit (BDA, Int64, 152 push bytes).
#include "vk_test_device.h"
#include "mesh_indirect_fixture.h"
#include "test_temp_dir.h"
#include "../shims/d3d12_com.h"
#include "../shims/d3d12_mesh_compute_direct.h"
#include "../shims/d3d12_pipeline_compile.h"
#include "../shims/d3d12_xs_device.h"
#include "../shims/d3d12_xs_pso_stream.h"
#include "../src/indirect_draw_stats.h"
#include "../src/vulkan_mesh_indirect_tiles.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#if !defined(LEONIDA_TEST_MESH_INDIRECT_DIR)
#error "LEONIDA_TEST_MESH_INDIRECT_DIR must name the folder build_mesh_indirect_fixtures.py wrote (spirv/index.json, fssm/*.fssm)"
#endif

#define CHECK(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return false; } \
} while (0)

using namespace d3d12test;
using namespace meshcomputetest;
using namespace meshindirecttest;
using XboxHeapHandleFn = std::uint64_t(WINAPI*)(void* heap);

namespace {

constexpr UINT kSide = 16;
constexpr UINT kArgBytes = 4096, kCountBytes = 256, kRecipeBytes = 4096;
constexpr UINT kSideOffset = kArgBytes + kCountBytes + kRecipeBytes + 64, kSideBytes = 256;  // the unrelated upload
constexpr float kGuard = 1024.0f;
constexpr std::uint32_t kKey = 0xC3A5F00Du;  // masks the recipe words: the upload never holds an argument

struct Res {
    Ref<ID3D12Resource> r;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};

struct Rec {
    std::uint32_t x, y, z;
};

struct Scenario {
    const char* name = "";
    std::vector<Rec> records;              // stored by the GPU writer, record i at args_offset + i * stride
    std::optional<std::uint32_t> count;    // stored by the GPU writer at count_offset: a counted call
    UINT max = 0;                          // MaxCommandCount
    UINT stride = 28, args_offset = 36, count_offset = 12;
    ID3D12PipelineState* pso = nullptr;
    std::uint32_t cells = kPixels;         // root constant s8
    bool follow_up = false;                // different roots/scissor and a direct DispatchMesh after the call
    std::optional<Rec> direct;             // a direct DispatchMesh instead of ExecuteIndirect
    bool bundle = false;                   // the call is recorded in a bundle that inherits the PSO
    bool refused = false;                  // the replay must refuse the list whole (unsupported), nothing runs
    bool predicated = false;               // SetPredication around the call, then an unpredicated direct draw
    std::uint64_t predicate_value = 0;     // the 8 bytes at predicate offset 8 (offset 0 holds the opposite)
    D3D12_PREDICATION_OP predicate_op = D3D12_PREDICATION_OP_EQUAL_ZERO;
    bool side_copy = false;                // an unrelated upload -> buffer copy in the same list, read back and checked
};

struct CloseCase {
    ID3D12PipelineState* pso = nullptr;
    ID3D12RootSignature* root = nullptr;
    bool viewport = true;
    ID3D12CommandSignature* sig = nullptr;
    UINT max = 4;
    ID3D12Resource* args = nullptr;
    UINT64 args_offset = 36;
    ID3D12Resource* count = nullptr;
    UINT64 count_offset = 0;
};

class Rig {
public:
    bool setup(const std::filesystem::path& staged, vktest::Device& vk, std::uint32_t max_group_count_x);
    bool run(const Scenario& s);
    bool negatives();
    bool controls();
    bool bundles();
    // Runs the GPU-argument writer's compute dispatch until it really executed (an asynchronously built compute
    // pipeline is skipped until its worker published it), so the writer is ready before a worker is held.
    bool writer_ready();
    bool conditional_rendering() const { return conditional_; }
    // Groups of one full scratch tile of the wide producer, and the smallest max_command_count of the narrow one
    // whose records x tiles pass the replay's restart bound, both from the planner with this device's limits.
    std::uint32_t wide_slots() const { return wide_slots_; }
    std::uint32_t refusal_records() const { return refusal_records_; }
    unsigned scenarios = 0;
    unsigned long long groups_checked = 0;
    ID3D12PipelineState* v5() { return pso_v5.p; }
    ID3D12PipelineState* wide() { return pso_wide.p; }
    ID3D12PipelineState* legacy() { return pso_legacy.p; }
    bool teardown();

private:
    bool make_pso(const D3D12_SHADER_BYTECODE& ms, ID3D12RootSignature* root, Ref<ID3D12PipelineState>& out,
                  HRESULT* hr = nullptr);
    bool make_buffer(UINT bytes, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, Res& out);
    bool make_readback(UINT bytes, Ref<ID3D12Resource>& out);
    bool make_sig(UINT stride, Ref<ID3D12CommandSignature>& out);
    void to(const std::initializer_list<std::pair<Res*, D3D12_RESOURCE_STATES>>& moves);
    bool wait(UINT64 value);
    HRESULT close_with(const CloseCase& c);
    // A fixed or counted call recorded in a bundle that inherits `pso`, executed by a parent list with the
    // standard mesh state; the bundle's and the parent's Close results.
    void bundle_close(ID3D12PipelineState* pso, bool counted, UINT max, HRESULT& bundle_hr, HRESULT& parent_hr);
    void bind_graphics(ID3D12PipelineState* pso, std::uint32_t cells, bool scissor_top);
    D3D12_CPU_DESCRIPTOR_HANDLE heap_handle(ID3D12DescriptorHeap* heap) {
        return {SIZE_T(reinterpret_cast<XboxHeapHandleFn>(slot(heap, 9))(heap))};
    }

    vktest::Device* vk_ = nullptr;
    std::unique_ptr<testfiles::ScratchDir> scratch_;
    Ref<ID3D12Device> device;
    std::vector<std::vector<std::uint8_t>> keep_;  // shader containers the bytecode views point into
    Ref<ID3D12RootSignature> root_cells, root_big, root_cs;
    Ref<ID3D12PipelineState> pso_v5, pso_wide, pso_legacy, pso_bad, pso_vs, pso_big, pso_cs;
    Ref<ID3D12CommandSignature> sig12, sig28, sig64;
    Ref<ID3D12CommandQueue> queue;
    Ref<ID3D12CommandAllocator> allocator;
    Ref<ID3D12GraphicsCommandList> list;
    Ref<ID3D12GraphicsCommandList6> list6;
    Ref<ID3D12Fence> fence;
    UINT64 fence_value = 0;
    Res args, count, recipe, rt, depth, pred, side;
    Ref<ID3D12Resource> upload, rb_rt, rb_depth, rb_args, rb_count, rb_side;
    Ref<ID3D12DescriptorHeap> rtv_heap, dsv_heap;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT depth_foot{};
    UINT64 depth_total = 0;
    std::uint8_t* upload_ptr = nullptr;
    bool conditional_ = false;
    std::uint32_t wide_slots_ = 0, refusal_records_ = 0;
    bool built_ = false;
    D3D12_SHADER_BYTECODE ms_v5, ms_wide, ms_legacy, ms_bad, ms_native, ps_cell, cs_args, vs, ps_draw;
};

bool Rig::make_pso(const D3D12_SHADER_BYTECODE& ms, ID3D12RootSignature* root, Ref<ID3D12PipelineState>& out,
                   HRESULT* hr) {
    Stream stream;
    stream.add(RootSubobject{.inner = root}).add(MsSubobject{.inner = ms});
    stream.add(PsSubobject{.inner = ps_cell}).add(TargetsSubobject{.inner = r32_target()});
    stream.add(DsvFormatSubobject{.inner = DXGI_FORMAT_D32_FLOAT});
    stream.add(DepthStencilSubobject{.inner = depth_always_write()});
    stream.add(RasterizerSubobject{.inner = cull_rasterizer(D3D12_CULL_MODE_NONE)});
    stream.add(BlendSubobject{.inner = additive_blend()});
    const HRESULT result = create_pso(device.p, stream, out);
    if (hr) *hr = result;
    return SUCCEEDED(result) && out.p;
}

bool Rig::make_buffer(UINT bytes, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, Res& out) {
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    const auto desc = buffer_desc(bytes, flags);
    out.state = state;
    return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                                     IID_PPV_ARGS(&out.r.p)));
}

bool Rig::make_readback(UINT bytes, Ref<ID3D12Resource>& out) {
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
    const auto desc = buffer_desc(bytes);
    return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&out.p)));
}

bool Rig::make_sig(UINT stride, Ref<ID3D12CommandSignature>& out) {
    const D3D12_INDIRECT_ARGUMENT_DESC arg{D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH};
    const D3D12_COMMAND_SIGNATURE_DESC desc{stride, 1, &arg, 0};
    return device->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&out.p)) == S_OK && out.p;
}

void Rig::to(const std::initializer_list<std::pair<Res*, D3D12_RESOURCE_STATES>>& moves) {
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    for (const auto& [res, state] : moves) {
        if (res->state == state) continue;
        barriers.push_back(transition(res->r.p, res->state, state));
        res->state = state;
    }
    if (!barriers.empty()) list->ResourceBarrier(UINT(barriers.size()), barriers.data());
}

bool Rig::wait(UINT64 value) {
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) return false;
    const bool done = SUCCEEDED(fence->SetEventOnCompletion(value, event)) &&
                      WaitForSingleObject(event, 120000) == WAIT_OBJECT_0;
    CloseHandle(event);
    return done;
}

// The graphics state of one mesh draw: roots (cells, 16), full viewport, a full or top-half scissor, the cell
// target and its depth, and the pipeline.
void Rig::bind_graphics(ID3D12PipelineState* pso, std::uint32_t cells, bool scissor_top) {
    const D3D12_VIEWPORT viewport{0, 0, float(kSide), float(kSide), 0, 1};
    const D3D12_RECT scissor{0, 0, LONG(kSide), LONG(scissor_top ? kSide / 2 : kSide)};
    const std::uint32_t roots[2] = {cells, kSide};
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = heap_handle(rtv_heap.p), dsv = heap_handle(dsv_heap.p);
    list->SetGraphicsRootSignature(root_cells.p);
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    list->SetPipelineState(pso);
    list->SetGraphicsRoot32BitConstants(0, 2, roots, 0);
}

bool Rig::setup(const std::filesystem::path& staged, vktest::Device& vk, std::uint32_t max_group_count_x) {
    vk_ = &vk;
    conditional_ = vk.conditional_rendering;
    scratch_ = std::make_unique<testfiles::ScratchDir>("leonida_mesh_indirect_test");
    const std::filesystem::path& library = scratch_->path();
    CHECK(stage_library(staged / "spirv", library));
    const auto program = [&](const char* name) {
        auto bytes = read_bytes(staged / "fssm" / (std::string(name) + ".fssm"));
        return bytes;
    };
    const auto wrap = [&](const char* name, bool mesh) {
        const auto fssm = program(name);
        keep_.push_back(mesh ? mesh_container(fssm) : graphics_container(fssm));
        const auto& container = keep_.back();
        return D3D12_SHADER_BYTECODE{container.data(), container.size()};
    };
    keep_.reserve(16);  // the views above point into these vectors: no reallocation
    ms_v5 = wrap("ms_v5_workgroup", true);
    ms_wide = wrap("ms_v5_wide", true);
    ms_legacy = wrap("ms_v5_legacy", true);
    ms_bad = wrap("ms_v5_badmarker", true);
    ms_native = wrap("ms_native", true);
    ps_cell = wrap("ps_mesh_indirect_cell", false);
    cs_args = wrap("cs_mesh_indirect_args", false);
    vs = wrap("vs_fullscreen", false);
    ps_draw = wrap("ps_draw", false);
    for (const auto& code : keep_) CHECK(!code.empty() && code.size() > 64);

    // The no-extension device: the emulation must work where native mesh is absent.
    rt::d3d12::VulkanDeviceInfo info{};
    info.prepare_on_bind = vk.prepare_on_bind;
    info.cold_graphics_compile = vk.cold_graphics_compile;
    info.heavy_pipeline_cache = vk.heavy_pipeline_cache;
    info.get_instance_proc = vk.instance_proc;
    info.instance = vk.instance;
    info.physical = vk.physical;
    info.device = vk.device;
    info.queue = vk.queue;
    info.queue_family = vk.family;
    info.buffer_device_address = true;
    info.shader_dir = library.string();
    info.mesh_shader = false;
    info.mesh_shader_queries = false;
    info.fragment_shader_barycentric = vk.fragment_shader_barycentric;
    info.pipeline_statistics_query = vk.pipeline_statistics_query;
    info.conservative_rasterization = vk.conservative_rasterization;
    info.draw_indirect_first_instance = vk.enabled.drawIndirectFirstInstance == VK_TRUE;
    info.multi_draw_indirect = vk.enabled.multiDrawIndirect == VK_TRUE;
    info.draw_indirect_count_enabled = vk.draw_indirect_count_enabled;
    info.max_draw_indirect_count = vk.max_draw_indirect_count;
    info.features = vk.enabled;
    info.conditional_rendering = vk.conditional_rendering;  // exactly what vkCreateDevice got
    {
        ID3D12Device* inner = nullptr;
        CHECK(SUCCEEDED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&inner))));
        CHECK(SUCCEEDED(rt::d3d12_xs::create_device_proxy(inner, __uuidof(ID3D12Device),
                                                          reinterpret_cast<void**>(&device.p))));
        rt::d3d12::set_device_outer(inner, device.p);
    }
    built_ = true;
    // No native mesh anywhere: the tier stays NOT_SUPPORTED, so none of what follows can be the native path.
    CHECK(mesh_tier(device.p) == D3D12_MESH_SHADER_TIER_NOT_SUPPORTED);
    CHECK(!vk.mesh_shader);

    // Roots: the cell shader's two constants (s8, s9); an oversized one (40 dwords > the 32 user dwords); the GPU
    // writer's three root UAVs plus two constants (s4:s5, s6:s7, s8:s9, s10, s11).
    {
        D3D12_ROOT_PARAMETER param{};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        param.Constants = {0, 0, 2};
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
        root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
        root_desc.Desc_1_0 = {1, &param, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        auto blob = serialize_root_signature(root_desc);
        CHECK(!blob.empty() &&
              SUCCEEDED(device->CreateRootSignature(0, blob.data(), blob.size(), IID_PPV_ARGS(&root_cells.p))));
        param.Constants = {0, 0, 40};
        blob = serialize_root_signature(root_desc);
        CHECK(!blob.empty() &&
              SUCCEEDED(device->CreateRootSignature(0, blob.data(), blob.size(), IID_PPV_ARGS(&root_big.p))));
        D3D12_ROOT_PARAMETER params[4] = {};
        for (UINT i = 0; i < 3; ++i) {
            params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            params[i].Descriptor = {i, 0};
        }
        params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[3].Constants = {3, 0, 2};
        root_desc.Desc_1_0 = {4, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        blob = serialize_root_signature(root_desc);
        CHECK(!blob.empty() &&
              SUCCEEDED(device->CreateRootSignature(0, blob.data(), blob.size(), IID_PPV_ARGS(&root_cs.p))));
    }
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = root_cs.p;
        desc.CS = cs_args;
        CHECK(SUCCEEDED(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso_cs.p))));
    }

    // The mesh PSOs. The marked producers, the wide one, and two that are direct-only: sealed fine, but not the v1
    // marker. Their creation succeeds (direct DispatchMesh works); only a positive ExecuteIndirect refuses them.
    HRESULT hr = S_OK;
    CHECK(make_pso(ms_v5, root_cells.p, pso_v5, &hr));
    CHECK(make_pso(ms_wide, root_cells.p, pso_wide, &hr));
    CHECK(make_pso(ms_legacy, root_cells.p, pso_legacy, &hr));
    CHECK(make_pso(ms_bad, root_cells.p, pso_bad, &hr));
    CHECK(rt::d3d12::pipeline_is_mesh_compute(pso_v5.p) && rt::d3d12::pipeline_is_mesh_compute(pso_wide.p));
    CHECK(rt::d3d12::pipeline_is_mesh_compute(pso_legacy.p) && rt::d3d12::pipeline_is_mesh_compute(pso_bad.p));
    CHECK(rt::d3d12::pipeline_supports_mesh_compute_indirect(pso_v5.p));
    CHECK(rt::d3d12::pipeline_supports_mesh_compute_indirect(pso_wide.p));
    CHECK(!rt::d3d12::pipeline_supports_mesh_compute_indirect(pso_legacy.p));
    CHECK(!rt::d3d12::pipeline_supports_mesh_compute_indirect(pso_bad.p));
    CHECK(!rt::d3d12::pipeline_supports_mesh_compute_indirect(pso_cs.p));
    {
        // The wide producer really has the bigger slot (the multi-tile scenarios rely on it).
        std::uint64_t narrow = 0, wide_bytes = 0;
        std::uint32_t prims = 0;
        CHECK(rt::d3d12::pipeline_mesh_compute_info(pso_v5.p, &narrow, &prims, nullptr));
        CHECK(rt::d3d12::pipeline_mesh_compute_info(pso_wide.p, &wide_bytes, &prims, nullptr));
        CHECK(wide_bytes >= 1024 && wide_bytes >= 8 * narrow);
        std::fprintf(stderr, "slot bytes: narrow %llu, wide %llu\n", (unsigned long long)narrow,
                     (unsigned long long)wide_bytes);
        // The planner's view of this device: the wide producer's tile capacity, and how many records of the
        // narrow producer tip a call over the restart bound. (The replay may cap the tile lower still, which only
        // adds tiles to the scenarios below.)
        leonida::mesh_indirect::Limits limits;
        limits.max_group_count_x = max_group_count_x;
        leonida::mesh_indirect::Plan plan;
        CHECK(leonida::mesh_indirect::plan(wide_bytes, prims, 1, limits, plan));
        wide_slots_ = plan.slots;
        CHECK(wide_slots_ >= 2 && wide_slots_ < 65535);  // a 3 KiB slot cannot fill a 64 MiB slab with 65535 slots
        for (std::uint32_t n = 1; n < 1000 && !refusal_records_; ++n)
            if (!leonida::mesh_indirect::plan(narrow, 1, n, limits, plan)) refusal_records_ = n;
        CHECK(refusal_records_ > 1 && std::uint64_t(refusal_records_ - 1) * 28 + 36 + 12 <= kArgBytes);
        std::fprintf(stderr, "wide tile capacity %u groups; first refused narrow MaxCommandCount %u\n", wide_slots_,
                     refusal_records_);
    }
    {
        // An ordinary vertex PSO, and an over-sized-root mesh PSO (refused at creation or at Close).
        Stream stream;
        stream.add(RootSubobject{.inner = root_cells.p}).add(VsSubobject{.inner = vs});
        stream.add(PsSubobject{.inner = ps_draw});
        stream.add(TopologySubobject{.inner = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE});
        stream.add(TargetsSubobject{.inner = r32_target()});
        CHECK(SUCCEEDED(create_pso(device.p, stream, pso_vs)) && pso_vs.p);
        const bool created = make_pso(ms_v5, root_big.p, pso_big, &hr);
        CHECK(created || hr == E_NOTIMPL || hr == E_INVALIDARG);
        CHECK(created || !pso_big.p);
        std::fprintf(stderr, "oversized-root mesh PSO: %s (hr %08lx)\n", created ? "created" : "refused",
                     static_cast<unsigned long>(hr));
    }
    {
        // Native MeshEXT stays refused on this device, with a nulled pointer: not spoofed by the emulation.
        Stream stream;
        stream.add(RootSubobject{.inner = root_cells.p}).add(MsSubobject{.inner = ms_native});
        stream.add(PsSubobject{.inner = ps_cell}).add(TargetsSubobject{.inner = r32_target()});
        ID3D12PipelineState* raw = reinterpret_cast<ID3D12PipelineState*>(std::uintptr_t(0x1234));
        Ref<ID3D12Device2> device2;
        CHECK(SUCCEEDED(device.p->QueryInterface(IID_PPV_ARGS(&device2.p))));
        const D3D12_PIPELINE_STATE_STREAM_DESC desc = stream.desc();
        CHECK(device2->CreatePipelineState(&desc, IID_PPV_ARGS(&raw)) == E_NOTIMPL && raw == nullptr);
    }

    CHECK(make_sig(12, sig12) && make_sig(28, sig28) && make_sig(64, sig64));
    {
        leonida::IndirectKind kind{};
        UINT stride = 0;
        bool terminal_only = false;
        CHECK(rt::d3d12::command_signature_info(sig28.p, kind, stride, terminal_only) &&
              kind == leonida::IndirectKind::dispatch_mesh && stride == 28 && terminal_only);
    }

    // Resources. The GPU-written buffers are UAV buffers; the cell target and its depth are 16 x 16.
    CHECK(make_buffer(kArgBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE, args));
    CHECK(make_buffer(kCountBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE,
                      count));
    CHECK(make_buffer(kRecipeBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, recipe));
    CHECK(make_buffer(64, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, pred));
    CHECK(make_buffer(kSideBytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, side));
    {
        const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
        const auto desc = buffer_desc(kSideOffset + kSideBytes);
        CHECK(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                        IID_PPV_ARGS(&upload.p))));
        CHECK(SUCCEEDED(upload->Map(0, nullptr, reinterpret_cast<void**>(&upload_ptr))) && upload_ptr);
    }
    {
        const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = desc.Height = kSide;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        CHECK(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                        D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                        IID_PPV_ARGS(&rt.r.p))));
        desc.Format = DXGI_FORMAT_D32_FLOAT;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = DXGI_FORMAT_D32_FLOAT;
        clear.DepthStencil.Depth = 1.0f;
        CHECK(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                        D3D12_RESOURCE_STATE_COMMON, &clear,
                                                        IID_PPV_ARGS(&depth.r.p))));
        device->GetCopyableFootprints(&desc, 0, 1, 0, &depth_foot, nullptr, nullptr, &depth_total);
        CHECK(depth_total > 0 && depth_foot.Footprint.RowPitch >= kSide * 4);
        const D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        const D3D12_DESCRIPTOR_HEAP_DESC dsv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        CHECK(SUCCEEDED(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap.p))));
        CHECK(SUCCEEDED(device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsv_heap.p))));
        device->CreateRenderTargetView(rt.r.p, nullptr, heap_handle(rtv_heap.p));
        device->CreateDepthStencilView(depth.r.p, nullptr, heap_handle(dsv_heap.p));
    }
    CHECK(make_readback(kSide * 256, rb_rt) && make_readback(UINT(depth_total), rb_depth));
    CHECK(make_readback(kArgBytes, rb_args) && make_readback(kCountBytes, rb_count));
    CHECK(make_readback(kSideBytes, rb_side));

    const D3D12_COMMAND_QUEUE_DESC queue_desc{};
    CHECK(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))));
    CHECK(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
    CHECK(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, nullptr,
                                              IID_PPV_ARGS(&list.p))));
    CHECK(SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&list6.p))));
    CHECK(SUCCEEDED(list->Close()));
    CHECK(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));
    return true;
}

// One scenario: the GPU writer stores the records and count, the barrier hands them to the indirect command, the
// replay draws, and the target, depth and both indirect buffers are read back and compared exactly.
bool Rig::run(const Scenario& s) {
    const unsigned errors_before = vk_->validation_errors();
    const ULONGLONG started = GetTickCount64();
    std::fprintf(stderr, "scenario %s\n", s.name);
    std::vector<std::uint32_t> args_image(kArgBytes / 4, kSentinel), count_image(kCountBytes / 4, kSentinel);
    std::vector<std::uint32_t> pairs;
    const auto put = [&](bool in_count, UINT word, std::uint32_t value) {
        (in_count ? count_image : args_image)[word] = value;
        pairs.push_back((in_count ? 0x80000000u : 0u) | word);
        pairs.push_back(value ^ kKey);
    };
    for (std::size_t i = 0; i < s.records.size(); ++i) {
        const UINT base = UINT((s.args_offset + i * s.stride) / 4);
        CHECK(base + 3 <= args_image.size());
        put(false, base, s.records[i].x);
        put(false, base + 1, s.records[i].y);
        put(false, base + 2, s.records[i].z);
    }
    if (s.count) put(true, s.count_offset / 4, *s.count);
    CHECK(!pairs.empty() && pairs.size() * 4 <= kRecipeBytes);
    const UINT pair_count = UINT(pairs.size() / 2);
    {
        std::vector<std::uint32_t> fill(kArgBytes / 4, kSentinel);
        std::memcpy(upload_ptr, fill.data(), kArgBytes);
        std::memcpy(upload_ptr + kArgBytes, fill.data(), kCountBytes);
        std::memcpy(upload_ptr + kArgBytes + kCountBytes, pairs.data(), pairs.size() * 4);
        // The predicate: the wanted value at byte 8, its opposite at byte 0 (a predicate read at the wrong
        // offset would run what it must skip), sentinel words around.
        std::uint8_t predicate_bytes[64];
        std::memset(predicate_bytes, 0x7E, sizeof(predicate_bytes));
        const std::uint64_t opposite = s.predicate_value ? 0 : 1;
        std::memcpy(predicate_bytes, &opposite, 8);
        std::memcpy(predicate_bytes + 8, &s.predicate_value, 8);
        std::memcpy(upload_ptr + kArgBytes + kCountBytes + kRecipeBytes, predicate_bytes, sizeof(predicate_bytes));
        for (UINT i = 0; i < kSideBytes; ++i) upload_ptr[kSideOffset + i] = std::uint8_t(0x40 + i + scenarios);
    }
    // A bundle with a fixed-count call, recorded with no PSO: it inherits the calling list's.
    Ref<ID3D12CommandAllocator> bundle_allocator;
    Ref<ID3D12GraphicsCommandList> bundle;
    if (s.bundle) {
        CHECK(!s.count);  // a count in a bundle is invalid: bundle_close() checks that
        CHECK(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE,
                                                       IID_PPV_ARGS(&bundle_allocator.p))));
        CHECK(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, bundle_allocator.p, nullptr,
                                                  IID_PPV_ARGS(&bundle.p))));
        bundle->ExecuteIndirect(s.stride == 12 ? sig12.p : s.stride == 64 ? sig64.p : sig28.p, s.max, args.r.p,
                                s.args_offset, nullptr, 0);
        CHECK(bundle->Close() == S_OK);
    }

    // A refused list never runs its barriers: the resources keep the states they had.
    const D3D12_RESOURCE_STATES saved[6] = {args.state, count.state, recipe.state, rt.state, depth.state, pred.state};
    CHECK(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.p, nullptr)));
    to({{&args, D3D12_RESOURCE_STATE_COPY_DEST}, {&count, D3D12_RESOURCE_STATE_COPY_DEST},
        {&recipe, D3D12_RESOURCE_STATE_COPY_DEST}, {&pred, D3D12_RESOURCE_STATE_COPY_DEST}});
    list->CopyBufferRegion(pred.r.p, 0, upload.p, kArgBytes + kCountBytes + kRecipeBytes, 64);
    if (s.side_copy) {
        // An upload unrelated to the mesh call, in the same submission: it must survive whatever the call does.
        to({{&side, D3D12_RESOURCE_STATE_COPY_DEST}});
        list->CopyBufferRegion(side.r.p, 0, upload.p, kSideOffset, kSideBytes);
    }
    list->CopyBufferRegion(args.r.p, 0, upload.p, 0, kArgBytes);
    list->CopyBufferRegion(count.r.p, 0, upload.p, kArgBytes, kCountBytes);
    list->CopyBufferRegion(recipe.r.p, 0, upload.p, kArgBytes + kCountBytes, pairs.size() * 4);
    to({{&args, D3D12_RESOURCE_STATE_UNORDERED_ACCESS}, {&count, D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
        {&recipe, D3D12_RESOURCE_STATE_UNORDERED_ACCESS}});
    // The GPU writer: an actual compute dispatch produces every argument and count word.
    list->SetComputeRootSignature(root_cs.p);
    list->SetPipelineState(pso_cs.p);
    list->SetComputeRootUnorderedAccessView(0, args.r->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(1, count.r->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(2, recipe.r->GetGPUVirtualAddress());
    const std::uint32_t writer_roots[2] = {kKey, pair_count};
    list->SetComputeRoot32BitConstants(3, 2, writer_roots, 0);
    list->Dispatch((pair_count + 63) / 64, 1, 1);
    to({{&args, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT}, {&count, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT},
        {&rt, D3D12_RESOURCE_STATE_RENDER_TARGET}, {&depth, D3D12_RESOURCE_STATE_DEPTH_WRITE},
        {&pred, D3D12_RESOURCE_STATE_PREDICATION}});

    const FLOAT guard[4] = {kGuard, 0, 0, 0};
    list->ClearRenderTargetView(heap_handle(rtv_heap.p), guard, 0, nullptr);
    list->ClearDepthStencilView(heap_handle(dsv_heap.p), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    ID3D12CommandSignature* sig = s.stride == 12 ? sig12.p : s.stride == 64 ? sig64.p : sig28.p;
    const auto indirect = [&]() {
        if (s.bundle) {
            list->ExecuteBundle(bundle.p);
            return;
        }
        list->ExecuteIndirect(sig, s.max, args.r.p, s.args_offset, s.count ? count.r.p : nullptr,
                              s.count ? s.count_offset : 0);
    };
    bind_graphics(s.pso, s.cells, false);
    if (s.predicated) list->SetPredication(pred.r.p, 8, s.predicate_op);
    if (s.direct) list6->DispatchMesh(s.direct->x, s.direct->y, s.direct->z);
    else indirect();
    if (s.predicated) {
        // Predication off again: a direct draw must render whatever the predicate said, on the state the indirect
        // call left (a false predicate suppressed the producer and the draw together, not just one of them).
        list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
        list6->DispatchMesh(2, 3, 4);
    }
    if (s.follow_up) {
        // Different roots and a top-half scissor for what comes next: the indirect draw above must not see them,
        // a direct (untagged) DispatchMesh and a second indirect call must.
        bind_graphics(s.pso, 200, true);
        list6->DispatchMesh(2, 3, 4);
        indirect();
    }
    to({{&rt, D3D12_RESOURCE_STATE_COPY_SOURCE}, {&depth, D3D12_RESOURCE_STATE_COPY_SOURCE},
        {&args, D3D12_RESOURCE_STATE_COPY_SOURCE}, {&count, D3D12_RESOURCE_STATE_COPY_SOURCE}});
    if (s.side_copy) {
        to({{&side, D3D12_RESOURCE_STATE_COPY_SOURCE}});
        list->CopyBufferRegion(rb_side.p, 0, side.r.p, 0, kSideBytes);
    }
    {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = rb_rt.p;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = {0, {DXGI_FORMAT_R32_FLOAT, kSide, kSide, 1, 256}};
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = rt.r.p;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        dst.pResource = rb_depth.p;
        dst.PlacedFootprint = depth_foot;
        src.pResource = depth.r.p;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    list->CopyBufferRegion(rb_args.p, 0, args.r.p, 0, kArgBytes);
    list->CopyBufferRegion(rb_count.p, 0, count.r.p, 0, kCountBytes);
    CHECK(list->Close() == S_OK);
    ID3D12CommandList* lists[] = {list.p};
    queue->ExecuteCommandLists(1, lists);
    const leonida::ReplayResult result = rt::d3d12::last_execute_result(queue.p);
    if (s.refused || (s.predicated && !conditional_)) {
        // Refused whole at submission: nothing of the list ran (the targets are not even read back), the queue still
        // signals, and the refusal is the planner's (unsupported), or for a predicated list without
        // VK_EXT_conditional_rendering the replay's.
        if (s.refused) CHECK(result == leonida::ReplayResult::unsupported);
        else CHECK(result != leonida::ReplayResult::success);
        CHECK(SUCCEEDED(queue->Signal(fence.p, ++fence_value)) && wait(fence_value));
        CHECK(vk_->validation_errors() == errors_before);
        args.state = saved[0];
        count.state = saved[1];
        recipe.state = saved[2];
        rt.state = saved[3];
        depth.state = saved[4];
        pred.state = saved[5];
        ++scenarios;
        std::fprintf(stderr, "  ok: list refused (%s)\n", s.refused ? "restart bound" : "no conditional rendering");
        return true;
    }
    CHECK(result == leonida::ReplayResult::success);
    CHECK(SUCCEEDED(queue->Signal(fence.p, ++fence_value)) && wait(fence_value));

    // The reference: every executed record in order, a record that was never written reads the sentinel.
    Plane want;
    std::uint64_t total = 0;
    const auto expect_records = [&](std::uint32_t cells, std::uint32_t visible) {
        UINT executed = s.max;
        if (s.count) executed = std::min(executed, *s.count);
        for (UINT r = 0; r < executed; ++r) {
            const Rec rec = r < s.records.size() ? s.records[r] : Rec{kSentinel, kSentinel, kSentinel};
            const std::uint64_t groups = groups_of(rec.x, rec.y, rec.z);
            total += groups;
            apply(want, groups, cells, visible);
        }
    };
    if (s.direct) {
        const std::uint64_t groups = groups_of(s.direct->x, s.direct->y, s.direct->z);
        total += groups;
        apply(want, groups, s.cells);
    } else {
        // The D3D predicate names the condition that suppresses the command.
        const bool runs = !s.predicated || (s.predicate_op == D3D12_PREDICATION_OP_EQUAL_ZERO
                                                ? s.predicate_value != 0
                                                : s.predicate_value == 0);
        if (runs) expect_records(s.cells, kPixels);
    }
    if (s.predicated) {
        total += 24;
        apply(want, 24, s.cells);
    }
    if (s.follow_up) {
        apply(want, 24, 200, kPixels / 2);
        expect_records(200, kPixels / 2);
    }
    unsigned wrong = 0;
    const auto report = [&](const char* what, UINT pixel, double got, double expected) {
        if (wrong++ < 8)
            std::fprintf(stderr, "  %s: %s pixel %u (%u,%u): got %.9g want %.9g\n", s.name, what, pixel,
                         pixel % kSide, pixel / kSide, got, expected);
    };
    {
        void* mapped = nullptr;
        CHECK(SUCCEEDED(rb_rt->Map(0, nullptr, &mapped)) && mapped);
        const auto* bytes = static_cast<const std::uint8_t*>(mapped);
        for (UINT p = 0; p < kPixels; ++p) {
            float got = 0;
            std::memcpy(&got, bytes + (p / kSide) * 256 + (p % kSide) * 4, 4);
            if (got != kGuard + want.count[p]) report("count", p, got, kGuard + want.count[p]);
        }
        rb_rt->Unmap(0, nullptr);
        CHECK(SUCCEEDED(rb_depth->Map(0, nullptr, &mapped)) && mapped);
        bytes = static_cast<const std::uint8_t*>(mapped);
        for (UINT p = 0; p < kPixels; ++p) {
            float got = 0;
            std::memcpy(&got, bytes + (p / kSide) * depth_foot.Footprint.RowPitch + (p % kSide) * 4, 4);
            if (got != want.depth[p]) report("depth", p, got, want.depth[p]);
        }
        rb_depth->Unmap(0, nullptr);
        // ExecuteIndirect must not write what it reads: both buffers hold exactly the GPU writer's words.
        CHECK(SUCCEEDED(rb_args->Map(0, nullptr, &mapped)) && mapped);
        if (std::memcmp(mapped, args_image.data(), kArgBytes) != 0) report("argument buffer", 0, 0, 0);
        rb_args->Unmap(0, nullptr);
        CHECK(SUCCEEDED(rb_count->Map(0, nullptr, &mapped)) && mapped);
        if (std::memcmp(mapped, count_image.data(), kCountBytes) != 0) report("count buffer", 0, 0, 0);
        rb_count->Unmap(0, nullptr);
        if (s.side_copy) {
            CHECK(SUCCEEDED(rb_side->Map(0, nullptr, &mapped)) && mapped);
            for (UINT i = 0; i < kSideBytes; ++i)
                if (static_cast<const std::uint8_t*>(mapped)[i] != std::uint8_t(0x40 + i + scenarios)) {
                    if (wrong++ < 8) std::fprintf(stderr, "  %s: unrelated upload byte %u wrong\n", s.name, i);
                }
            rb_side->Unmap(0, nullptr);
        }
    }
    CHECK(wrong == 0);
    CHECK(vk_->validation_errors() == errors_before);
    ++scenarios;
    groups_checked += total;
    std::fprintf(stderr, "  ok: %llu groups, %llu ms\n", (unsigned long long)total,
                 (unsigned long long)(GetTickCount64() - started));
    return true;
}

HRESULT Rig::close_with(const CloseCase& c) {
    Ref<ID3D12CommandAllocator> alloc;
    Ref<ID3D12GraphicsCommandList> l;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc.p)))) return E_FAIL;
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.p, nullptr, IID_PPV_ARGS(&l.p))))
        return E_FAIL;
    const D3D12_VIEWPORT viewport{0, 0, float(kSide), float(kSide), 0, 1};
    const D3D12_RECT scissor{0, 0, LONG(kSide), LONG(kSide)};
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = heap_handle(rtv_heap.p), dsv = heap_handle(dsv_heap.p);
    if (c.root) l->SetGraphicsRootSignature(c.root);
    if (c.viewport) {
        l->RSSetViewports(1, &viewport);
        l->RSSetScissorRects(1, &scissor);
    }
    l->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    if (c.pso) l->SetPipelineState(c.pso);
    l->ExecuteIndirect(c.sig, c.max, c.args, c.args_offset, c.count, c.count_offset);
    return l->Close();
}

// Close() verdicts, exactly. Nothing here executes: a rejected list must never reach the GPU, and an accepted one
// is the same call the executed scenarios make.
bool Rig::negatives() {
    CloseCase good;
    good.pso = pso_v5.p;
    good.root = root_cells.p;
    good.sig = sig28.p;
    good.args = args.r.p;
    CloseCase counted = good;
    counted.count = count.r.p;
    counted.count_offset = 12;
    CHECK(close_with(good) == S_OK);
    CHECK(close_with(counted) == S_OK);
    CHECK(close_with([&] { auto c = good; c.pso = pso_wide.p; return c; }()) == S_OK);

    // Positive maximum without a usable mesh-compute indirect path: refused at recording, never silently dropped.
    const auto refused = [&](const char* what, CloseCase c) {
        for (const bool use_count : {false, true}) {
            c.count = use_count ? count.r.p : nullptr;
            c.count_offset = use_count ? 12 : 0;
            const HRESULT hr = close_with(c);
            if (hr != E_INVALIDARG)
                std::fprintf(stderr, "%s (%s): Close %08lx, want E_INVALIDARG\n", what,
                             use_count ? "counted" : "fixed", static_cast<unsigned long>(hr));
            CHECK(hr == E_INVALIDARG);
        }
        return true;
    };
    CHECK(refused("no PSO", [&] { auto c = good; c.pso = nullptr; return c; }()));
    CHECK(refused("ordinary vertex PSO", [&] { auto c = good; c.pso = pso_vs.p; return c; }()));
    CHECK(refused("compute PSO", [&] { auto c = good; c.pso = pso_cs.p; return c; }()));
    CHECK(refused("direct-only producer (no marker)", [&] { auto c = good; c.pso = pso_legacy.p; return c; }()));
    CHECK(refused("wrongly marked producer", [&] { auto c = good; c.pso = pso_bad.p; return c; }()));
    CHECK(refused("no graphics root signature", [&] { auto c = good; c.root = nullptr; return c; }()));
    CHECK(refused("no viewport", [&] { auto c = good; c.viewport = false; return c; }()));
    if (pso_big.p)
        CHECK(refused("oversized mesh root", [&] { auto c = good; c.pso = pso_big.p; c.root = root_big.p; return c; }()));

    // max = 0 is a validated no-op whatever is bound (even nothing), fixed or counted.
    for (ID3D12PipelineState* pso : {static_cast<ID3D12PipelineState*>(nullptr), pso_vs.p, pso_legacy.p, pso_v5.p}) {
        for (const bool use_count : {false, true}) {
            CloseCase c = good;
            c.pso = pso;
            c.max = 0;
            c.count = use_count ? count.r.p : nullptr;
            c.count_offset = use_count ? 12 : 0;
            CHECK(close_with(c) == S_OK);
        }
    }

    // Ranges and alignment of the arguments and the count, exactly as D3D12 reads them: (max - 1) * stride + 12
    // bytes from the offset; the last record may fit while its stride runs past the buffer.
    const UINT exact = kArgBytes - ((3 - 1) * 28 + 12);  // 4028: three records of stride 28 end at the buffer end
    CHECK(close_with([&] { auto c = good; c.max = 3; c.args_offset = exact; return c; }()) == S_OK);
    CHECK(close_with([&] { auto c = good; c.max = 3; c.args_offset = exact + 4; return c; }()) == E_INVALIDARG);
    CHECK(close_with([&] { auto c = good; c.args_offset = 38; return c; }()) == E_INVALIDARG);          // misaligned
    CHECK(close_with([&] { auto c = good; c.max = 1000; return c; }()) == E_INVALIDARG);                // too long
    CHECK(close_with([&] { auto c = good; c.args = nullptr; return c; }()) == E_INVALIDARG);
    CHECK(close_with([&] { auto c = counted; c.count_offset = kCountBytes - 4; return c; }()) == S_OK);
    CHECK(close_with([&] { auto c = counted; c.count_offset = kCountBytes; return c; }()) == E_INVALIDARG);
    CHECK(close_with([&] { auto c = counted; c.count_offset = 14; return c; }()) == E_INVALIDARG);
    CHECK(close_with([&] { auto c = good; c.count_offset = 8; return c; }()) == E_INVALIDARG);          // offset, no buffer
    // Strides: below the 12-byte record the signature is refused and the output nulled; one not a multiple of 4
    // is refused at creation or at Close, never replayed.
    {
        const D3D12_INDIRECT_ARGUMENT_DESC arg{D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH};
        const D3D12_COMMAND_SIGNATURE_DESC short_sig{8, 1, &arg, 0};
        void* out = reinterpret_cast<void*>(std::uintptr_t(0xc0ffee));
        CHECK(device->CreateCommandSignature(&short_sig, nullptr, __uuidof(ID3D12CommandSignature), &out) ==
                  E_INVALIDARG && !out);
        const D3D12_COMMAND_SIGNATURE_DESC odd_sig{14, 1, &arg, 0};
        Ref<ID3D12CommandSignature> odd;
        const HRESULT hr = device->CreateCommandSignature(&odd_sig, nullptr, IID_PPV_ARGS(&odd.p));
        if (SUCCEEDED(hr)) CHECK(close_with([&] { auto c = good; c.sig = odd.p; return c; }()) == E_INVALIDARG);
        else CHECK(hr == E_INVALIDARG && !odd.p);
    }
    return true;
}

void Rig::bundle_close(ID3D12PipelineState* pso, bool counted, UINT max, HRESULT& bundle_hr, HRESULT& parent_hr) {
    bundle_hr = parent_hr = E_FAIL;
    Ref<ID3D12CommandAllocator> bundle_allocator, allocator_direct;
    Ref<ID3D12GraphicsCommandList> bundle, parent;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&bundle_allocator.p))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, bundle_allocator.p, nullptr,
                                         IID_PPV_ARGS(&bundle.p))))
        return;
    bundle->ExecuteIndirect(sig28.p, max, args.r.p, 36, counted ? count.r.p : nullptr, counted ? 12 : 0);
    bundle_hr = bundle->Close();
    if (FAILED(bundle_hr)) {
        parent_hr = S_OK;  // nothing to execute
        return;
    }
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_direct.p))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_direct.p, nullptr,
                                         IID_PPV_ARGS(&parent.p))))
        return;
    const D3D12_VIEWPORT viewport{0, 0, float(kSide), float(kSide), 0, 1};
    const D3D12_RECT scissor{0, 0, LONG(kSide), LONG(kSide)};
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = heap_handle(rtv_heap.p), dsv = heap_handle(dsv_heap.p);
    parent->SetGraphicsRootSignature(root_cells.p);
    parent->RSSetViewports(1, &viewport);
    parent->RSSetScissorRects(1, &scissor);
    parent->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    if (pso) parent->SetPipelineState(pso);
    parent->ExecuteBundle(bundle.p);
    parent_hr = parent->Close();
}

// A call recorded in a bundle: the bundle records no PSO, so what it may run is decided where it is executed.
bool Rig::bundles() {
    HRESULT bundle_hr = 0, parent_hr = 0;
    bundle_close(pso_v5.p, false, 4, bundle_hr, parent_hr);  // fixed, inherited marked producer: eligible
    CHECK(bundle_hr == S_OK && parent_hr == S_OK);
    bundle_close(pso_wide.p, false, 4, bundle_hr, parent_hr);
    CHECK(bundle_hr == S_OK && parent_hr == S_OK);
    bundle_close(pso_v5.p, true, 4, bundle_hr, parent_hr);   // a GPU count in a bundle is invalid at the bundle
    CHECK(bundle_hr == E_INVALIDARG);
    for (ID3D12PipelineState* pso : {static_cast<ID3D12PipelineState*>(nullptr), pso_vs.p, pso_legacy.p, pso_bad.p,
                                     pso_cs.p}) {
        bundle_close(pso, false, 4, bundle_hr, parent_hr);   // fixed, but the inherited PSO cannot run it
        if (bundle_hr != S_OK || parent_hr != E_INVALIDARG)
            std::fprintf(stderr, "bundle with inherited PSO %p: bundle %08lx parent %08lx\n",
                         static_cast<void*>(pso), static_cast<unsigned long>(bundle_hr),
                         static_cast<unsigned long>(parent_hr));
        CHECK(bundle_hr == S_OK && parent_hr == E_INVALIDARG);
        bundle_close(pso, false, 0, bundle_hr, parent_hr);   // max 0 is a no-op wherever it runs
        CHECK(bundle_hr == S_OK && parent_hr == S_OK);
    }
    return true;
}

bool Rig::writer_ready() {
    const std::uint32_t want = 0x1234ABCDu;
    const ULONGLONG deadline = GetTickCount64() + 60000;
    while (GetTickCount64() < deadline) {
        const std::vector<std::uint32_t> fill(kArgBytes / 4, kSentinel);
        std::memcpy(upload_ptr, fill.data(), kArgBytes);
        const std::uint32_t pair[2] = {0u, want ^ kKey};
        std::memcpy(upload_ptr + kArgBytes + kCountBytes, pair, sizeof(pair));
        CHECK(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.p, nullptr)));
        to({{&args, D3D12_RESOURCE_STATE_COPY_DEST}, {&recipe, D3D12_RESOURCE_STATE_COPY_DEST}});
        list->CopyBufferRegion(args.r.p, 0, upload.p, 0, kArgBytes);
        list->CopyBufferRegion(recipe.r.p, 0, upload.p, kArgBytes + kCountBytes, sizeof(pair));
        to({{&args, D3D12_RESOURCE_STATE_UNORDERED_ACCESS}, {&count, D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
            {&recipe, D3D12_RESOURCE_STATE_UNORDERED_ACCESS}});
        list->SetComputeRootSignature(root_cs.p);
        list->SetPipelineState(pso_cs.p);
        list->SetComputeRootUnorderedAccessView(0, args.r->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(1, count.r->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(2, recipe.r->GetGPUVirtualAddress());
        const std::uint32_t roots[2] = {kKey, 1};
        list->SetComputeRoot32BitConstants(3, 2, roots, 0);
        list->Dispatch(1, 1, 1);
        to({{&args, D3D12_RESOURCE_STATE_COPY_SOURCE}});
        list->CopyBufferRegion(rb_args.p, 0, args.r.p, 0, kArgBytes);
        CHECK(list->Close() == S_OK);
        ID3D12CommandList* lists[] = {list.p};
        queue->ExecuteCommandLists(1, lists);
        CHECK(rt::d3d12::last_execute_result(queue.p) == leonida::ReplayResult::success);
        CHECK(SUCCEEDED(queue->Signal(fence.p, ++fence_value)) && wait(fence_value));
        void* mapped = nullptr;
        CHECK(SUCCEEDED(rb_args->Map(0, nullptr, &mapped)) && mapped);
        const bool ran = static_cast<const std::uint32_t*>(mapped)[0] == want;
        rb_args->Unmap(0, nullptr);
        if (ran) return true;
        Sleep(10);  // the writer's compute pipeline is still being built by the worker: the dispatch was skipped
    }
    std::fprintf(stderr, "the GPU argument writer never ran within 60 s\n");
    return false;
}

// Controls on what the emulation must not change.
bool Rig::controls() {
    // A direct-only producer still runs a direct DispatchMesh (the same grid, the same pixels as the marked one),
    // and so does the wrongly marked one: the marker gates ExecuteIndirect only.
    for (ID3D12PipelineState* pso : {pso_v5.p, pso_legacy.p, pso_bad.p, pso_wide.p}) {
        Scenario s;
        s.name = "direct DispatchMesh(2,3,4)";
        s.pso = pso;
        s.records = {{1, 1, 1}};  // the writer still stores a record: it is not executed
        s.direct = Rec{2, 3, 4};
        CHECK(run(s));
    }
    return true;
}

bool Rig::teardown() {
    if (upload.p && upload_ptr) upload->Unmap(0, nullptr);
    upload_ptr = nullptr;
    for (auto* r : {&upload, &rb_rt, &rb_depth, &rb_args, &rb_count, &rb_side}) r->reset();
    for (auto* r : {&args, &count, &recipe, &rt, &depth, &pred, &side}) r->r.reset();
    for (auto* p : {&pso_v5, &pso_wide, &pso_legacy, &pso_bad, &pso_vs, &pso_big, &pso_cs}) p->reset();
    for (auto* r : {&root_cells, &root_big, &root_cs}) r->reset();
    sig12.reset();
    sig28.reset();
    sig64.reset();
    rtv_heap.reset();
    dsv_heap.reset();
    list6.reset();
    list.reset();
    allocator.reset();
    queue.reset();
    fence.reset();
    if (built_) CHECK(rt::d3d12_xs::device_proxy_ref_count(device.p) == 1);
    device.reset();
    scratch_.reset();
    return true;
}

// --async-pending: asynchronous, non-strict pipeline builds with the graphics variant of the mesh consumer held on
// the compile worker. A positive emulated ExecuteIndirect must still run, synchronously building the variant it
// needs, with an unrelated upload in the same submission surviving, one logical `emitted` for the call (not per
// tile), no validation error, and a clean completion before the worker is released; after the release the same
// pipeline is reused with no new build.
bool run_async_pending(Rig& rig, vktest::Device& vk) {
    namespace ind = leonida::indirect_stats;
    auto& compiler = rt::d3d12::PipelineCompiler::instance();
    CHECK(rt::d3d12::async_replay_compile());
    // The GPU-argument writer first: its compute pipeline is async too, and a dispatch of a pipeline still pending is
    // skipped, which would make this an unrelated test artifact. Nothing is held yet.
    CHECK(rig.writer_ready());

    std::atomic<unsigned> held_jobs{0};
    struct HeldWorker {  // released and cleared before the rig (the device waits for a held build) goes
        HANDLE release = nullptr;
        ~HeldWorker() {
            if (!release) return;
            SetEvent(release);
            rt::d3d12::PipelineCompiler::instance().set_job_probe(nullptr);
            CloseHandle(release);
        }
    } held;
    held.release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CHECK(held.release != nullptr);
    const std::uint64_t completed_before = compiler.stats().completed;
    compiler.set_job_probe([&held_jobs, release = held.release](const void*) {
        held_jobs.fetch_add(1);
        WaitForSingleObject(release, 120000);  // bounded: a failed run never waits forever
    });

    const auto emitted = [] {
        return ind::counts().values[std::size_t(ind::Event::emitted)].load(std::memory_order_relaxed);
    };
    const std::uint32_t slots = rig.wide_slots();
    Scenario s;
    s.name = "async pending: wide producer, 3 tiles, variant held on the worker";
    s.pso = rig.wide();
    s.records = {{slots + 1, 1, 1}, {2, 3, 4}};
    s.max = 2;
    s.side_copy = true;
    const auto emitted_before = emitted();
    CHECK(rig.run(s));  // success, the unrelated upload, the draw and the buffers, all before any release
    CHECK(emitted() == emitted_before + 1);
    // The worker really holds the queued job (the draw did not wait for it): bounded wait for it to arrive.
    for (int i = 0; i < 1000 && held_jobs.load() == 0; ++i) Sleep(10);
    CHECK(held_jobs.load() >= 1);
    CHECK(compiler.stats().completed == completed_before);

    SetEvent(held.release);
    // Every job that reached a worker finishes; a second one may still be starting, so let the count settle first.
    const ULONGLONG deadline = GetTickCount64() + 60000;
    for (;;) {
        const unsigned started = held_jobs.load();
        while (compiler.stats().completed < completed_before + started && GetTickCount64() < deadline) Sleep(5);
        Sleep(200);
        if (held_jobs.load() == started || GetTickCount64() >= deadline) break;
    }
    const unsigned jobs = held_jobs.load();
    CHECK(compiler.stats().completed == completed_before + jobs);
    const std::uint64_t settled = compiler.stats().completed;

    Scenario again = s;
    again.name = "async pending: same pipeline after the release";
    again.records = {{3, 3, 3}, {slots + 2, 1, 1}};
    const auto emitted_again = emitted();
    CHECK(rig.run(again));
    CHECK(emitted() == emitted_again + 1);
    CHECK(held_jobs.load() == jobs && compiler.stats().completed == settled);  // reused: nothing was built again
    CHECK(vk.validation_errors() == 0);
    std::printf("mesh compute indirect async-pending: ok (%u held job(s), %llu emitted, unrelated upload intact, "
                "reuse after release without a rebuild)\n",
                jobs, static_cast<unsigned long long>(emitted() - emitted_before));
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const bool async_pending = argc == 2 && std::strcmp(argv[1], "--async-pending") == 0;
    if (argc > 1 && !async_pending) return 1;
    if (async_pending) {  // before the first PSO: the runtime reads these once
        _putenv_s("LEONIDA_ASYNC_PIPELINES", "1");
        _putenv_s("LEONIDA_STRICT_REPLAY", "0");
        _putenv_s("LEONIDA_COMPUTE_WARMUP", "0");
        // LEONIDA_ASYNC_MIN_BYTES stays unset (every pipeline is asynchronous) and so does LEONIDA_LAZY_PIPELINES.
    }
    // Validation is the default: the layer's own synchronization validation is the barrier/hazard gate for the
    // GPU-written argument buffers, the prepare/emit passes and the scratch reuse between tiles.
    const auto env_set = [](const wchar_t* name) { return GetEnvironmentVariableW(name, nullptr, 0) > 0; };
    wchar_t value[8] = {};
    const bool user_choice = env_set(L"LEONIDA_VK_VALIDATION");
    const bool user_off = user_choice && GetEnvironmentVariableW(L"LEONIDA_VK_VALIDATION", value, 8) > 0 &&
                          value[0] == L'0';
    const bool require_validation = env_set(L"LEONIDA_VK_REQUIRE_VALIDATION");
    if (!user_choice) SetEnvironmentVariableW(L"LEONIDA_VK_VALIDATION", L"1");
    const auto make_device = [&](vktest::DeviceStatus& status) {
        auto device = std::make_unique<vktest::Device>();
        device->request_buffer_device_address = true;
        device->request_multi_draw_indirect = true;
        device->request_draw_indirect_count = true;
        device->request_draw_indirect_first_instance = true;
        status = device->initialize();
        return device;
    };
    vktest::DeviceStatus status{};
    auto vk = make_device(status);
    bool validation = !user_off;
    if (status == vktest::DeviceStatus::failed && !user_choice && !require_validation) {
        std::fprintf(stderr, "note: validation layer unavailable, running without it\n");
        SetEnvironmentVariableW(L"LEONIDA_VK_VALIDATION", L"0");
        vk = make_device(status);
        validation = false;
    }
    if (status == vktest::DeviceStatus::unavailable) {
        std::fprintf(stderr, "SKIP: no usable Vulkan 1.3 device\n");
        return 77;
    }
    if (status != vktest::DeviceStatus::ready) return 1;
    if (!vk->buffer_device_address) {
        std::fprintf(stderr, "SKIP: need bufferDeviceAddress\n");
        return 77;
    }
    if (!vk->enabled.shaderInt64) {
        std::fprintf(stderr, "SKIP: need shaderInt64\n");
        return 77;
    }
    {
        const auto get_props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
            vk->instance_proc(vk->instance, "vkGetPhysicalDeviceProperties"));
        if (!get_props) return 1;
        VkPhysicalDeviceProperties props{};
        get_props(vk->physical, &props);
        if (props.limits.maxPushConstantsSize < 152) {
            std::fprintf(stderr, "SKIP: need 152 push constant bytes, device has %u\n",
                         props.limits.maxPushConstantsSize);
            return 77;
        }
        std::fprintf(stderr, "device %s, maxComputeWorkGroupCount x %u, validation %s\n", props.deviceName,
                     props.limits.maxComputeWorkGroupCount[0], validation ? "on (with synchronization)" : "off");
    }
    if (require_validation && !validation) {
        std::fprintf(stderr, "LEONIDA_VK_REQUIRE_VALIDATION is set but validation is off\n");
        return 1;
    }
    // Mesh compute is the path under test on every GPU: the harness flag the producers' index layout implies.
    if (!SetEnvironmentVariableW(L"LEONIDA_MESH_COMPUTE", L"1")) return 1;

    Rig rig;
    std::uint32_t max_group_count_x = 0;
    {
        const auto get_props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
            vk->instance_proc(vk->instance, "vkGetPhysicalDeviceProperties"));
        VkPhysicalDeviceProperties props{};
        get_props(vk->physical, &props);
        max_group_count_x = props.limits.maxComputeWorkGroupCount[0];
    }
    if (!rig.setup(LEONIDA_TEST_MESH_INDIRECT_DIR, *vk, max_group_count_x)) return 1;
    if (async_pending) {
        if (!run_async_pending(rig, *vk)) return 1;
        if (!rig.teardown()) return 1;
        if (vk->validation_errors()) {
            std::fprintf(stderr, "%u validation errors\n", vk->validation_errors());
            return 1;
        }
        return 0;
    }

    // Producers: ms_v5_workgroup (small slot, one tile for up to ~65 535 groups) and ms_v5_wide (a ~3 KiB slot:
    // 65 536 groups need several tiles under any bounded scratch).
    std::vector<Scenario> list;
    const auto add = [&](Scenario s) { list.push_back(std::move(s)); };
    {
        Scenario s;
        s.name = "fixed: real XYZ grids, padded stride 28, offset 36";
        s.pso = rig.v5();
        s.records = {{2, 3, 4}, {1, 1, 1}, {3, 1, 2}, {5, 1, 1}};
        s.max = 4;
        add(s);
        s.name = "counted: count 3 of max 6, legal-looking grids past it must not run";
        s.records = {{2, 3, 4}, {4, 1, 1}, {1, 7, 1}, {9, 1, 1}, {9, 9, 1}, {9, 1, 9}};
        s.max = 6;
        s.count = 3;
        add(s);
        s.name = "counted: count 100 above max 3 -> 3, count offset 252";
        s.records = {{3, 3, 3}, {2, 2, 2}, {1, 2, 3}, {9, 1, 1}};
        s.max = 3;
        s.count = 100;
        s.count_offset = 252;
        add(s);
        s.name = "counted: count 0 of max 4 runs nothing";
        s.records = {{3, 1, 1}, {2, 2, 2}, {1, 1, 1}, {7, 1, 1}};
        s.max = 4;
        s.count = 0;
        s.count_offset = 4;
        add(s);
        s.name = "max 0, fixed: no-op";
        s.records = {{3, 1, 1}};
        s.max = 0;
        s.count.reset();
        s.count_offset = 12;
        add(s);
        s.name = "max 0, counted 5: no-op";
        s.count = 5;
        add(s);
        s.name = "zero X, zero Y, zero Z, all-zero grids between real ones";
        s.records = {{0, 5, 5}, {4, 0, 1}, {2, 2, 0}, {3, 1, 1}, {0, 0, 0}, {2, 1, 1}};
        s.max = 6;
        s.count.reset();
        add(s);
        s.name = "invalid records run as zero work (over 65535, over 2^22, all ones)";
        s.records = {{65536, 1, 1}, {1, 65536, 1}, {1, 1, 65536}, {2048, 2049, 1}, {0xFFFFFFFFu, 2, 2},
                     {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu}, {3, 1, 1}, {2, 3, 1}};
        s.max = 8;
        add(s);
        s.name = "records the GPU writer never wrote hold the sentinel: zero work";
        s.records = {{3, 1, 1}, {1, 2, 1}};
        s.max = 5;
        add(s);
        s.name = "tight stride 12, offset 0";
        s.stride = 12;
        s.args_offset = 0;
        s.records = {{2, 1, 1}, {1, 2, 1}, {3, 1, 2}};
        s.max = 3;
        add(s);
        s.name = "stride 64, offset 100, counted 2 of 4";
        s.stride = 64;
        s.args_offset = 100;
        s.records = {{1, 3, 1}, {1, 1, 5}, {9, 1, 1}, {9, 1, 1}};
        s.max = 4;
        s.count = 2;
        add(s);
        s.name = "cells 64 (a root constant the replay must carry)";
        s.stride = 28;
        s.args_offset = 36;
        s.cells = 64;
        s.records = {{2, 3, 4}, {4, 4, 4}};
        s.max = 2;
        s.count.reset();
        add(s);
    }
    {
        Scenario s;
        s.name = "state: roots and scissor change after the call, direct and indirect after it";
        s.pso = rig.v5();
        s.records = {{2, 3, 4}, {5, 1, 2}, {1, 1, 1}};
        s.max = 3;
        s.follow_up = true;
        add(s);
        s.name = "state with a GPU count";
        s.count = 2;
        s.max = 3;
        add(s);
    }
    {
        Scenario s;
        s.name = "65535 groups in each dimension of one record";
        s.pso = rig.v5();
        s.records = {{65535, 1, 1}, {1, 65535, 1}, {1, 1, 65535}};
        s.max = 3;
        add(s);
        // The wide producer's scratch tile holds `slots` groups: a record of exactly one tile, one group more (the
        // second tile holds a single group, which must still carry its own index) and exactly two tiles. Modest:
        // 4 * slots groups in all.
        const std::uint32_t slots = rig.wide_slots();
        s.name = "wide slot: records of 1 tile, 1 tile + 1 group, 2 tiles (2 x slots)";
        s.pso = rig.wide();
        s.records = {{slots, 1, 1}, {slots + 1, 1, 1}, {2, slots, 1}};
        s.max = 3;
        add(s);
        s.name = "wide slot, counted 2 of 3: the third must not run";
        s.count = 2;
        add(s);
        s.name = "wide slot, cells 100 (g mod 100) over a tile boundary";
        s.count.reset();
        s.cells = 100;
        s.records = {{slots + 1, 1, 1}};
        s.max = 1;
        add(s);
    }
    {
        Scenario s;
        s.pso = rig.v5();
        s.records = {{2, 3, 4}, {5, 1, 2}};
        s.max = 2;
        for (const D3D12_PREDICATION_OP op :
             {D3D12_PREDICATION_OP_EQUAL_ZERO, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO}) {
            for (const std::uint64_t value : {std::uint64_t(0), std::uint64_t(1), std::uint64_t(0x100000000ull)}) {
                s.name = op == D3D12_PREDICATION_OP_EQUAL_ZERO ? "predicated EQUAL_ZERO" : "predicated NOT_EQUAL_ZERO";
                s.predicated = true;
                s.predicate_op = op;
                s.predicate_value = value;  // the third has only a high dword set: a 32-bit read would see zero
                add(s);
            }
        }
        s.name = "predicated, counted 1 of 2, zero predicate";
        s.count = 1;
        s.predicate_op = D3D12_PREDICATION_OP_NOT_EQUAL_ZERO;
        s.predicate_value = 0;
        add(s);
        s.name = "predicated, counted 1 of 2, nonzero predicate";
        s.predicate_value = 1;
        add(s);
    }
    {
        Scenario s;
        s.name = "bundle: fixed call inheriting the marked producer";
        s.pso = rig.v5();
        s.records = {{2, 3, 4}, {1, 1, 1}, {5, 1, 2}};
        s.max = 3;
        s.bundle = true;
        add(s);
        s.name = "bundle: fixed call on the wide producer, zero grids between";
        s.pso = rig.wide();
        s.records = {{0, 4, 4}, {3, 2, 1}, {rig.wide_slots() + 1, 1, 1}};
        add(s);
    }
    {
        // Over the restart bound (records x tiles of one logical call): refused at submission, fixed and counted
        // alike (the plan is made from max_command_count, the GPU count unread), and nothing of the list runs.
        Scenario s;
        s.name = "restart bound: fixed call over it is refused whole";
        s.pso = rig.v5();
        s.records = {{3, 1, 1}};
        s.max = rig.refusal_records();
        s.refused = true;
        add(s);
        s.name = "restart bound: counted 1 of the same max is refused whole";
        s.count = 1;
        add(s);
    }
    for (const auto& s : list)
        if (!rig.run(s)) {
            std::fprintf(stderr, "FAILED: %s\n", s.name);
            return 1;
        }
    if (!rig.controls()) return 1;
    if (!rig.negatives()) return 1;
    if (!rig.bundles()) return 1;
    const unsigned scenarios = rig.scenarios;
    const unsigned long long groups = rig.groups_checked;
    if (!rig.teardown()) return 1;
    if (vk->validation_errors()) {
        std::fprintf(stderr, "%u validation errors\n", vk->validation_errors());
        return 1;
    }
    std::printf("mesh compute indirect: %u GPU-written scenarios (%llu groups checked pixel by pixel, counts, "
                "last-group depth, guards and both indirect buffers), Close verdicts exact, validation %s\n",
                scenarios, groups, validation ? "clean (core + synchronization)" : "not run");
    return 0;
}
