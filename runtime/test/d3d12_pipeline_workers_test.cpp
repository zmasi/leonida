// Pipelines compiled by a submission on the device's workers (LEONIDA_PIPELINE_THREADS, src/pipeline_workers.h), through
// the D3D12 layer on a GPU. One submission draws 48 graphics pipeline states (16 of them with a render target left
// unbound, so through a variant) and dispatches 16 compute ones, each into its own pixels or buffer element; the image
// and the buffer are read back.
//   - workers off (0) and on: the same bytes, the same number of compiles, and several at a time only with workers;
//   - a compile the driver refuses: the same result, the same dropped dispatch and the same retry with and without workers;
//   - two queues that need the same pipelines at once compile each once;
//   - pipeline states released while a submission compiles: the ones it uses live on, and the release of any other waits
//     until the submission has its pipelines;
//   - a device whose last references go while its workers compile: the workers have ended before the pipeline cache is
//     read for pipeline_cache.session.bin;
//   - the prewarm from a recorded order (LEONIDA_PIPELINE_PREWARM_ORDER, src/pipeline_prewarm.h) builds its graphics
//     pipelines before the first bind for pipeline states made from a console stream (device slot 86), also when the
//     stream is kept for its first bind (LEONIDA_PIPELINE_COMPILE=on-demand), and nothing without a plan.
// With LEONIDA_LAZY_PIPELINES=0 and LEONIDA_LAZY_COMPUTE=0 the pipelines are compiled when they are created and only the
// variants are left to the submission; the cases that need more than that are then left out.
// The calls that create pipelines and read the cache are watched, and held back, through the device's vkGetDeviceProcAddr.
// --async-graphics-zero and --async-graphics-unset: LEONIDA_ASYNC_PIPELINES=1 with LEONIDA_ASYNC_MIN_BYTES over every
// module, and LEONIDA_ASYNC_MIN_BYTES_GRAPHICS 0 or unset, with the compile worker held (async_thresholds below).
// Self-contained: builds its SPIR-V in memory. Set LEONIDA_VK_VALIDATION=1 to run under VK_LAYER_KHRONOS_validation.
#include "vk_test_device.h"
#include "d3d12_test_shaders.h"
#include "test_temp_dir.h"
#include "../shims/d3d12_com.h"
#include "../shims/d3d12_xs_device.h"
#include "../shims/d3d12_pipeline_compile.h"
#include "../src/shader_library.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using d3d12test::Ref;
using leonida::ReplayResult;
using Words = std::vector<std::uint32_t>;

constexpr unsigned kGraphics = 48, kVariants = 16, kCompute = 16, kPipelines = kGraphics + kCompute;
// Programs padded past PipelineCompiles::large_spirv_bytes: compiled one after another, as one job of the pool. The first
// of the graphics ones is drawn through a variant.
bool large_graphics(unsigned n) { return n % 16 == 5; }
bool large_compute(unsigned n) { return n % 8 == 3; }
constexpr unsigned kLargeGraphics = 3, kLargeVariants = 1, kLargeCompute = 2;
constexpr unsigned kExtent = 16;  // the render target; each graphics pipeline draws one 2x2 cell

#define CHECK(expr)                                                                  \
    do {                                                                             \
        if (!(expr)) {                                                               \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);     \
            return false;                                                            \
        }                                                                            \
    } while (0)

// ---- The driver's pipeline calls, watched. ----

struct Hooks {
    std::mutex mutex;
    std::condition_variable changed;
    bool closed = false;  // creations wait while it is
    int meet = 0;         // creations wait (at most 10 s) until this many are in progress, the first time
    int in_flight = 0;
    int most_in_flight = 0;
    int creates = 0;
    int graphics = 0, compute = 0;  // of those
    int fail_graphics = -1;         // the graphics or compute create (counted from 0) the driver refuses
    int fail_compute = -1;
    std::set<std::thread::id> threads;
    // Threads that have created a pipeline while `track` was set and have not ended.
    bool track = false;
    int live = 0;
    int cache_reads = 0;
    bool cache_read_during_create = false;
    bool cache_read_with_workers = false;

    void reset() {
        std::lock_guard lock(mutex);
        closed = track = cache_read_during_create = cache_read_with_workers = false;
        meet = most_in_flight = creates = graphics = compute = cache_reads = 0;
        fail_graphics = fail_compute = -1;
        threads.clear();
    }
    void open() {
        std::lock_guard lock(mutex);
        closed = false;
        changed.notify_all();
    }
    bool wait_in_flight(int count) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 10s, [&] { return in_flight >= count; });
    }
} hooks;

struct Tracked {
    bool on = false;
    ~Tracked() {
        if (!on) return;
        std::lock_guard lock(hooks.mutex);
        --hooks.live;
    }
};
thread_local Tracked tracked;

PFN_vkGetInstanceProcAddr real_instance_proc = nullptr;
PFN_vkGetDeviceProcAddr real_device_proc = nullptr;
PFN_vkCreateGraphicsPipelines real_create_graphics = nullptr;
PFN_vkCreateComputePipelines real_create_compute = nullptr;
PFN_vkGetPipelineCacheData real_cache_data = nullptr;

// False: the driver is to refuse this one.
bool enter_create(bool compute) {
    std::unique_lock lock(hooks.mutex);
    ++hooks.creates;
    const int index = compute ? hooks.compute++ : hooks.graphics++;
    hooks.most_in_flight = std::max(hooks.most_in_flight, ++hooks.in_flight);
    hooks.threads.insert(std::this_thread::get_id());
    if (hooks.track && !tracked.on) {
        tracked.on = true;
        ++hooks.live;
    }
    hooks.changed.notify_all();
    if (hooks.meet) {
        hooks.changed.wait_for(lock, 10s, [] { return !hooks.meet || hooks.in_flight >= hooks.meet; });
        hooks.meet = 0;
        hooks.changed.notify_all();
    }
    hooks.changed.wait(lock, [] { return !hooks.closed; });
    return index != (compute ? hooks.fail_compute : hooks.fail_graphics);
}

void leave_create() {
    std::lock_guard lock(hooks.mutex);
    --hooks.in_flight;
    hooks.changed.notify_all();
}

VKAPI_ATTR VkResult VKAPI_CALL create_graphics(VkDevice device, VkPipelineCache cache, std::uint32_t count,
                                               const VkGraphicsPipelineCreateInfo* infos,
                                               const VkAllocationCallbacks* allocator, VkPipeline* out) {
    VkResult result = VK_ERROR_UNKNOWN;
    if (enter_create(false)) result = real_create_graphics(device, cache, count, infos, allocator, out);
    leave_create();
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL create_compute(VkDevice device, VkPipelineCache cache, std::uint32_t count,
                                              const VkComputePipelineCreateInfo* infos,
                                              const VkAllocationCallbacks* allocator, VkPipeline* out) {
    VkResult result = VK_ERROR_UNKNOWN;
    if (enter_create(true)) result = real_create_compute(device, cache, count, infos, allocator, out);
    leave_create();
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL cache_data(VkDevice device, VkPipelineCache cache, std::size_t* size, void* data) {
    {
        std::lock_guard lock(hooks.mutex);
        ++hooks.cache_reads;
        if (hooks.in_flight) hooks.cache_read_during_create = true;
        if (hooks.live > (tracked.on ? 1 : 0)) hooks.cache_read_with_workers = true;
    }
    return real_cache_data(device, cache, size, data);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL device_proc(VkDevice device, const char* name) {
    const PFN_vkVoidFunction real = real_device_proc(device, name);
    if (!real) return nullptr;
    if (!std::strcmp(name, "vkCreateGraphicsPipelines")) {
        real_create_graphics = reinterpret_cast<PFN_vkCreateGraphicsPipelines>(real);
        return reinterpret_cast<PFN_vkVoidFunction>(create_graphics);
    }
    if (!std::strcmp(name, "vkCreateComputePipelines")) {
        real_create_compute = reinterpret_cast<PFN_vkCreateComputePipelines>(real);
        return reinterpret_cast<PFN_vkVoidFunction>(create_compute);
    }
    if (!std::strcmp(name, "vkGetPipelineCacheData")) {
        real_cache_data = reinterpret_cast<PFN_vkGetPipelineCacheData>(real);
        return reinterpret_cast<PFN_vkVoidFunction>(cache_data);
    }
    return real;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL instance_proc(VkInstance instance, const char* name) {
    if (!std::strcmp(name, "vkGetDeviceProcAddr")) {
        real_device_proc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(real_instance_proc(instance, name));
        return reinterpret_cast<PFN_vkVoidFunction>(device_proc);
    }
    return real_instance_proc(instance, name);
}

// ---- The shaders. ----

void op(Words& out, std::uint16_t code, std::initializer_list<std::uint32_t> operands) {
    out.push_back((std::uint32_t(operands.size() + 1) << 16) | code);
    out.insert(out.end(), operands.begin(), operands.end());
}

std::uint32_t bits(float f) { return std::bit_cast<std::uint32_t>(f); }

// 1.25 MiB of OpString, which compile to nothing, where the module's layout has them: before the decorations.
void pad(Words& w, std::uint32_t first_id) {
    for (std::uint32_t i = 0; i < 5; ++i) {
        const std::size_t at = w.size();
        w.resize(at + 65535, 0x20202020u);
        w[at] = 65535u << 16 | 7;  // OpString
        w[at + 1] = first_id + i;
        w.back() = 0;
    }
}

// A full-screen triangle from VertexIndex.
Words vertex_words() {
    Words w = {0x07230203u, 0x00010500u, 0u, 42u, 0u};
    op(w, 17, {1});                                  // OpCapability Shader
    op(w, 14, {0, 1});                               // OpMemoryModel Logical GLSL450
    op(w, 15, {0, 4, 0x6e69616du, 0u, 10, 11});      // OpEntryPoint Vertex %4 "main" %10 %11
    op(w, 71, {10, 11, 0});                          // OpDecorate %10 BuiltIn Position
    op(w, 71, {11, 11, 42});                         // OpDecorate %11 BuiltIn VertexIndex
    op(w, 19, {1});                                  // %1 = OpTypeVoid
    op(w, 33, {2, 1});                               // %2 = OpTypeFunction %1
    op(w, 22, {3, 32});                              // %3 = OpTypeFloat 32
    op(w, 23, {6, 3, 4});                            // %6 = OpTypeVector %3 4
    op(w, 21, {15, 32, 1});                          // %15 = OpTypeInt 32 1
    op(w, 32, {7, 3, 6});                            // %7 = OpTypePointer Output %6
    op(w, 32, {8, 1, 15});                           // %8 = OpTypePointer Input %15
    op(w, 59, {7, 10, 3});                           // %10 = OpVariable (Position)
    op(w, 59, {8, 11, 1});                           // %11 = OpVariable (VertexIndex)
    op(w, 43, {15, 24, 1});                          // %24 = OpConstant 1
    op(w, 43, {15, 25, 2});                          // %25 = OpConstant 2
    op(w, 43, {3, 26, bits(2.0f)});
    op(w, 43, {3, 27, bits(-1.0f)});
    op(w, 43, {3, 28, bits(0.0f)});
    op(w, 43, {3, 29, bits(1.0f)});
    op(w, 54, {1, 4, 0, 2});                         // OpFunction
    op(w, 248, {30});                                // OpLabel
    op(w, 61, {15, 31, 11});                         // %31 = VertexIndex
    op(w, 196, {15, 32, 31, 24});                    // %32 = %31 << 1
    op(w, 199, {15, 33, 32, 25});                    // %33 = %32 & 2
    op(w, 199, {15, 34, 31, 25});                    // %34 = %31 & 2
    op(w, 111, {3, 35, 33});
    op(w, 111, {3, 36, 34});
    op(w, 133, {3, 37, 35, 26});
    op(w, 133, {3, 38, 36, 26});
    op(w, 129, {3, 39, 37, 27});                     // x = 2 * %35 - 1
    op(w, 129, {3, 40, 38, 27});                     // y = 2 * %36 - 1
    op(w, 80, {6, 41, 39, 40, 28, 29});
    op(w, 62, {10, 41});                             // Position = (x, y, 0, 1)
    op(w, 253, {});
    op(w, 56, {});
    return w;
}

// The colour (n/255, (255 - 2n)/255, 0.25 * FragCoord.x / 16 + 0.5, 1) of pipeline n: a constant of its own, and one that
// depends on the pixel.
Words fragment_words(unsigned n) {
    Words w = {0x07230203u, 0x00010500u, 0u, 40u, 0u};
    op(w, 17, {1});
    op(w, 14, {0, 1});
    op(w, 15, {4, 4, 0x6e69616du, 0u, 8, 9});        // OpEntryPoint Fragment %4 "main" %8 %9
    op(w, 16, {4, 7});                               // OpExecutionMode OriginUpperLeft
    if (large_graphics(n)) pad(w, 30);
    op(w, 71, {8, 11, 15});                          // OpDecorate %8 BuiltIn FragCoord
    op(w, 71, {9, 30, 0});                           // OpDecorate %9 Location 0
    op(w, 19, {1});
    op(w, 33, {2, 1});
    op(w, 22, {3, 32});
    op(w, 23, {6, 3, 4});
    op(w, 32, {7, 1, 6});
    op(w, 32, {10, 3, 6});
    op(w, 59, {7, 8, 1});
    op(w, 59, {10, 9, 3});
    op(w, 43, {3, 20, bits(float(n) / 255.0f)});
    op(w, 43, {3, 21, bits(float(255 - 2 * n) / 255.0f)});
    op(w, 43, {3, 22, bits(0.25f / float(kExtent))});
    op(w, 43, {3, 23, bits(0.5f)});
    op(w, 43, {3, 24, bits(1.0f)});
    op(w, 54, {1, 4, 0, 2});
    op(w, 248, {11});
    op(w, 61, {6, 12, 8});
    op(w, 81, {3, 13, 12, 0});                       // FragCoord.x
    op(w, 133, {3, 14, 13, 22});
    op(w, 129, {3, 15, 14, 23});
    op(w, 80, {6, 16, 20, 21, 15, 24});
    op(w, 62, {9, 16});
    op(w, 253, {});
    op(w, 56, {});
    return w;
}

// Stores 1000 + n at the root UAV's element s[6] (the translator's push-constant block; the console's compute arguments
// start at s[4]: s[4]:s[5] is the root UAV's console VA, s[6] the root constant).
Words compute_words(unsigned n) {
    Words w = {0x07230203u, 0x00010500u, 0u, 80u, 0u};
    op(w, 17, {1});
    op(w, 17, {11});                                 // Int64
    op(w, 17, {5347});                               // PhysicalStorageBufferAddresses
    op(w, 14, {5348, 1});                            // OpMemoryModel PhysicalStorageBuffer64 GLSL450
    op(w, 15, {5, 4, 0x6e69616du, 0u, 20});          // OpEntryPoint GLCompute %4 "main" %20
    op(w, 16, {4, 17, 1, 1, 1});                     // OpExecutionMode LocalSize 1 1 1
    if (large_compute(n)) pad(w, 70);
    op(w, 71, {17, 6, 4});                           // ArrayStride 4
    op(w, 71, {18, 2});                              // Block
    op(w, 72, {18, 0, 35, 0});                       // s[32], heap base, code base
    op(w, 72, {18, 1, 35, 128});
    op(w, 72, {18, 2, 35, 136});
    op(w, 71, {23, 2});
    op(w, 72, {23, 0, 35, 0});
    op(w, 19, {1});
    op(w, 33, {2, 1});
    op(w, 21, {6, 32, 0});                           // %6 = uint
    op(w, 21, {11, 64, 0});                          // %11 = ulong
    op(w, 43, {6, 16, 32});
    op(w, 28, {17, 6, 16});
    op(w, 30, {18, 17, 11, 11});
    op(w, 32, {19, 9, 18});
    op(w, 59, {19, 20, 9});                          // %20 = the push-constant block
    op(w, 32, {21, 9, 6});
    op(w, 32, {22, 9, 11});
    op(w, 30, {23, 6});
    op(w, 32, {24, 5349, 23});
    op(w, 32, {25, 5349, 6});
    op(w, 43, {6, 26, 0});
    op(w, 43, {6, 27, 1});
    op(w, 43, {6, 28, 4});
    op(w, 43, {6, 32, 5});
    op(w, 43, {6, 33, 6});
    op(w, 43, {11, 29, 32, 0});
    op(w, 43, {11, 30, 2, 0});
    op(w, 43, {6, 31, 1000 + n});
    op(w, 54, {1, 4, 0, 2});
    op(w, 248, {40});
    op(w, 65, {21, 41, 20, 26, 28});
    op(w, 65, {21, 42, 20, 26, 32});
    op(w, 65, {21, 43, 20, 26, 33});
    op(w, 65, {22, 44, 20, 27});
    op(w, 61, {6, 45, 41});
    op(w, 61, {6, 46, 42});
    op(w, 61, {6, 47, 43});
    op(w, 61, {11, 48, 44});
    op(w, 113, {11, 49, 45});
    op(w, 113, {11, 50, 46});
    op(w, 113, {11, 51, 47});
    op(w, 196, {11, 52, 50, 29});                    // s[5] << 32
    op(w, 196, {11, 53, 51, 30});                    // s[6] << 2
    op(w, 197, {11, 54, 52, 49});
    op(w, 128, {11, 55, 54, 53});
    op(w, 128, {11, 56, 55, 48});                    // + heap base
    op(w, 120, {24, 57, 56});                        // OpConvertUToPtr
    op(w, 65, {25, 58, 57, 26});
    op(w, 62, {58, 31, 2, 4});                       // OpStore Aligned 4
    op(w, 253, {});
    op(w, 56, {});
    return w;
}

std::vector<std::uint8_t> program(unsigned stage, unsigned n) {
    return d3d12test::fssm_program({0x50574b00u + stage, n, 0xbf810000u});
}

bool write_library(const std::filesystem::path& dir) {
    const auto write = [&](const std::string& name, const Words& words) {
        std::ofstream f(dir / (name + ".spv"), std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(words.data()), std::streamsize(words.size() * 4));
        return bool(f);
    };
    CHECK(write("vs", vertex_words()));
    std::ofstream index(dir / "index.json", std::ios::binary | std::ios::trunc);
    index << "{\"vs\": {\"stage\": \"vs\", \"fssm_sha256\": \"" << leonida::sha256_hex(program(0, 0))
          << "\", \"bindings\": [], \"push_constant_bytes\": 144, \"input_signature\": []}";
    for (unsigned n = 0; n <= kGraphics; ++n) {  // one more than is drawn: the pipeline state released on its own
        const std::string name = "ps" + std::to_string(n);
        CHECK(write(name, fragment_words(n)));
        index << ",\n\"" << name << "\": {\"stage\": \"ps\", \"fssm_sha256\": \"" << leonida::sha256_hex(program(1, n))
              << "\", \"bindings\": [], \"push_constant_bytes\": 144, \"inputs\": [], \"outputs\": [0]}";
    }
    for (unsigned n = 0; n < kCompute; ++n) {
        const std::string name = "cs" + std::to_string(n);
        CHECK(write(name, compute_words(n)));
        index << ",\n\"" << name << "\": {\"stage\": \"cs\", \"fssm_sha256\": \"" << leonida::sha256_hex(program(2, n))
              << "\", \"bindings\": [], \"push_constant_bytes\": 144, \"local_size\": [1, 1, 1], \"wave64\": false}";
    }
    index << "}";
    return bool(index);
}

// ---- The device and what it draws. ----

// The settings the runtime reads once: with both off, pipelines are compiled when they are created.
bool off(const char* name) {
    char value[8]{};
    std::size_t length = 0;
    return getenv_s(&length, value, name) == 0 && length == 2 && value[0] == '0';
}
const bool first_use = !off("LEONIDA_LAZY_PIPELINES");

struct Setup {
    vktest::Device* vk;
    std::string library;

    bool device(const char* threads, Ref<ID3D12Device>& out, bool prepare_on_bind = false) const {
        _putenv_s("LEONIDA_PIPELINE_THREADS", threads);
        rt::d3d12::VulkanDeviceInfo info{};
        info.get_instance_proc = instance_proc;
        info.instance = vk->instance;
        info.physical = vk->physical;
        info.device = vk->device;
        info.queue = vk->queue;
        info.queue_family = vk->family;
        info.buffer_device_address = true;
        info.subgroup = vk->subgroup;
        info.features = vk->enabled;
        info.shader_dir = library;
        info.prepare_on_bind = prepare_on_bind;  // LEONIDA_PIPELINE_COMPILE=on-demand
        CHECK(SUCCEEDED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&out.p))));
        return true;
    }
};

// One pipeline state stream subobject as d3dx12.h declares it (CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT).
template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename Inner>
struct alignas(void*) Subobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
    Inner inner;
};
struct Stream {
    std::vector<std::uint8_t> bytes;
    template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename Inner>
    Stream& add(const Inner& inner) {
        const Subobject<Type, Inner> subobject{Type, inner};
        static_assert(sizeof(subobject) % sizeof(void*) == 0);
        const std::size_t at = bytes.size();
        bytes.resize(at + sizeof(subobject));
        std::memcpy(bytes.data() + at, &subobject, sizeof(subobject));
        return *this;
    }
};

struct Pipelines {
    Ref<ID3D12RootSignature> root;
    ID3D12Device* console = nullptr;  // set: graphics pipeline states through this console device's slot 86
    std::vector<ID3D12PipelineState*> states;  // kGraphics, then kCompute
    ~Pipelines() { release(); }
    void release() {
        for (auto* s : states) s->Release();
        states.clear();
    }

    static ID3D12PipelineState* graphics(ID3D12Device* dev, ID3D12RootSignature* root, unsigned n) {
        const auto vs = d3d12test::shader_container(program(0, 0)), ps = d3d12test::shader_container(program(1, n));
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = root;
        pd.VS = {vs.data(), vs.size()};
        pd.PS = {ps.data(), ps.size()};
        for (auto& rt : pd.BlendState.RenderTarget) {
            rt.SrcBlend = rt.SrcBlendAlpha = D3D12_BLEND_ONE;
            rt.DestBlend = rt.DestBlendAlpha = D3D12_BLEND_ZERO;
            rt.BlendOp = rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            rt.LogicOp = D3D12_LOGIC_OP_NOOP;
            rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        }
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = n < kVariants ? 2 : 1;  // the second is never bound: those draws take a variant
        for (UINT i = 0; i < pd.NumRenderTargets; ++i) pd.RTVFormats[i] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pd.SampleDesc = {1, 0};
        ID3D12PipelineState* out = nullptr;
        return SUCCEEDED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&out))) ? out : nullptr;
    }

    // The same description as a stream, through the console device's CreatePipelineState (slot 86).
    static ID3D12PipelineState* console_graphics(ID3D12Device* dev, ID3D12RootSignature* root, unsigned n) {
        const auto vs = d3d12test::shader_container(program(0, 0)), ps = d3d12test::shader_container(program(1, n));
        D3D12_BLEND_DESC blend{};
        for (auto& rt : blend.RenderTarget) {
            rt.SrcBlend = rt.SrcBlendAlpha = D3D12_BLEND_ONE;
            rt.DestBlend = rt.DestBlendAlpha = D3D12_BLEND_ZERO;
            rt.BlendOp = rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            rt.LogicOp = D3D12_LOGIC_OP_NOOP;
            rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        }
        D3D12_RASTERIZER_DESC raster{};
        raster.FillMode = D3D12_FILL_MODE_SOLID;
        raster.CullMode = D3D12_CULL_MODE_NONE;
        raster.DepthClipEnable = TRUE;
        D3D12_DEPTH_STENCIL_DESC depth{};
        depth.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        D3D12_RT_FORMAT_ARRAY targets{};
        targets.NumRenderTargets = n < kVariants ? 2 : 1;
        for (UINT i = 0; i < targets.NumRenderTargets; ++i) targets.RTFormats[i] = DXGI_FORMAT_R8G8B8A8_UNORM;
        Stream stream;
        stream.add<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE>(root)
            .add<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS>(D3D12_SHADER_BYTECODE{vs.data(), vs.size()})
            .add<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS>(D3D12_SHADER_BYTECODE{ps.data(), ps.size()})
            .add<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND>(blend)
            .add<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK>(UINT(UINT_MAX))
            .add<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER>(raster)
            .add<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL>(depth)
            .add<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY>(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE)
            .add<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS>(targets)
            .add<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC>(DXGI_SAMPLE_DESC{1, 0});
        const D3D12_PIPELINE_STATE_STREAM_DESC desc{stream.bytes.size(), stream.bytes.data()};
        using Create = HRESULT(WINAPI*)(ID3D12Device*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
        ID3D12PipelineState* out = nullptr;
        const auto create = reinterpret_cast<Create>(d3d12test::slot(dev, 86));
        return SUCCEEDED(create(dev, &desc, IID_PPV_ARGS(&out))) ? out : nullptr;
    }

    bool create(ID3D12Device* dev) {
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.Num32BitValues = 1;
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
        desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
        desc.Desc_1_0 = {2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        const auto blob = d3d12test::serialize_root_signature(desc);
        CHECK(SUCCEEDED(dev->CreateRootSignature(0, blob.data(), blob.size(), IID_PPV_ARGS(&root.p))));
        for (unsigned n = 0; n < kGraphics; ++n) {
            auto* state = console ? console_graphics(console, root.p, n) : graphics(dev, root.p, n);
            CHECK(state);
            states.push_back(state);
        }
        for (unsigned n = 0; n < kCompute; ++n) {
            const auto cs = d3d12test::shader_container(program(2, n));
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature = root.p;
            pd.CS = {cs.data(), cs.size()};
            ID3D12PipelineState* state = nullptr;
            CHECK(SUCCEEDED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&state))));
            states.push_back(state);
        }
        return true;
    }
};

// A queue, and two lists that use every pipeline once and copy what they wrote to a readback buffer.
struct Frame {
    Ref<ID3D12Resource> target, buffer, readback;
    Ref<ID3D12DescriptorHeap> rtvs;
    Ref<ID3D12CommandQueue> queue;
    Ref<ID3D12CommandAllocator> allocator;
    Ref<ID3D12GraphicsCommandList> lists[2];
    Ref<ID3D12Fence> fence;
    UINT64 fence_value = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT foot{};

    bool create(ID3D12Device* dev, const Pipelines& p) {
        const D3D12_HEAP_PROPERTIES def{D3D12_HEAP_TYPE_DEFAULT}, back{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = td.Height = kExtent;
        td.DepthOrArraySize = td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc = {1, 0};
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        CHECK(SUCCEEDED(dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                     nullptr, IID_PPV_ARGS(&target.p))));
        const auto bd = d3d12test::buffer_desc(4 * kCompute, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        CHECK(SUCCEEDED(dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &bd,
                                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                     IID_PPV_ARGS(&buffer.p))));
        UINT64 image_bytes = 0;
        dev->GetCopyableFootprints(&td, 0, 1, 0, &foot, nullptr, nullptr, &image_bytes);
        const auto rd = d3d12test::buffer_desc(4096 + 4 * kCompute);
        CHECK(image_bytes <= 4096);
        CHECK(SUCCEEDED(dev->CreateCommittedResource(&back, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                     nullptr, IID_PPV_ARGS(&readback.p))));
        const D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
        CHECK(SUCCEEDED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvs.p))));
        const auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
        dev->CreateRenderTargetView(target.p, nullptr, rtv);
        const D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
        CHECK(SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue.p))));
        CHECK(SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));
        CHECK(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
        for (int l = 0; l < 2; ++l) {
            auto& list = lists[l];
            CHECK(SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, nullptr,
                                                   IID_PPV_ARGS(&list.p))));
            if (l == 0) {
                const FLOAT clear[4] = {0, 0, 0, 0};
                list->ClearRenderTargetView(rtv, clear, 0, nullptr);
            }
            list->SetGraphicsRootSignature(p.root.p);
            list->SetComputeRootSignature(p.root.p);
            list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            const D3D12_VIEWPORT vp{0, 0, float(kExtent), float(kExtent), 0, 1};
            list->RSSetViewports(1, &vp);
            list->SetComputeRootUnorderedAccessView(0, buffer->GetGPUVirtualAddress());
            for (unsigned n = unsigned(l); n < kPipelines; n += 2) {  // the even ones in one list, the odd in the other
                list->SetPipelineState(p.states[n]);
                if (n < kGraphics) {
                    const LONG x = LONG(n % 8 * 2), y = LONG(n / 8 * 2);
                    const D3D12_RECT cell{x, y, x + 2, y + 2};
                    list->RSSetScissorRects(1, &cell);
                    list->DrawInstanced(3, 1, 0, 0);
                    list->DrawInstanced(3, 1, 0, 0);  // again: the same pipeline, once compiled
                } else {
                    list->SetComputeRoot32BitConstant(1, n - kGraphics, 0);
                    list->Dispatch(1, 1, 1);
                }
            }
            CHECK(l == 0 || copy_back(list.p));
            CHECK(SUCCEEDED(list->Close()));
        }
        return true;
    }

    bool copy_back(ID3D12GraphicsCommandList* list) {
        const D3D12_RESOURCE_BARRIER to_source[2] = {
            d3d12test::transition(target.p, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
            d3d12test::transition(buffer.p, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE)};
        list->ResourceBarrier(2, to_source);
        D3D12_TEXTURE_COPY_LOCATION src{target.p, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        src.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION dst{readback.p, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        dst.PlacedFootprint = foot;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        list->CopyBufferRegion(readback.p, 4096, buffer.p, 0, 4 * kCompute);
        const D3D12_RESOURCE_BARRIER back[2] = {
            d3d12test::transition(target.p, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
            d3d12test::transition(buffer.p, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
        list->ResourceBarrier(2, back);
        return true;
    }

    ReplayResult submit() {
        ID3D12CommandList* both[] = {lists[0].p, lists[1].p};
        queue->ExecuteCommandLists(2, both);
        const ReplayResult result = rt::d3d12::last_execute_result(queue.p);
        queue->Signal(fence.p, ++fence_value);
        fence->SetEventOnCompletion(fence_value, nullptr);
        return result;
    }

    // The image's rows, then the buffer; the readback buffer is cleared, so stale bytes cannot pass.
    std::vector<std::uint8_t> read() {
        std::vector<std::uint8_t> out;
        void* mapped = nullptr;
        if (FAILED(readback->Map(0, nullptr, &mapped)) || !mapped) return out;
        auto* bytes = static_cast<std::uint8_t*>(mapped);
        for (unsigned y = 0; y < kExtent; ++y)
            out.insert(out.end(), bytes + foot.Offset + y * foot.Footprint.RowPitch,
                       bytes + foot.Offset + y * foot.Footprint.RowPitch + 4 * kExtent);
        out.insert(out.end(), bytes + 4096, bytes + 4096 + 4 * kCompute);
        std::memset(bytes, 0xee, 4096 + 4 * kCompute);
        readback->Unmap(0, nullptr);
        return out;
    }
};

// What the shaders say, for the exact channels: red is the pipeline's number, green 255 - 2n, and the buffer 1000 + n.
bool expected(const std::vector<std::uint8_t>& bytes) {
    CHECK(bytes.size() == 4 * kExtent * kExtent + 4 * kCompute);
    for (unsigned n = 0; n < kGraphics; ++n)
        for (unsigned dy = 0; dy < 2; ++dy)
            for (unsigned dx = 0; dx < 2; ++dx) {
                const auto* pixel = &bytes[4 * ((n / 8 * 2 + dy) * kExtent + n % 8 * 2 + dx)];
                CHECK(pixel[0] == n && pixel[1] == 255 - 2 * n && pixel[3] == 255 && pixel[2] >= 127 && pixel[2] <= 192);
            }
    for (unsigned n = 0; n < kCompute; ++n) {
        std::uint32_t value = 0;
        std::memcpy(&value, &bytes[4 * kExtent * kExtent + 4 * n], 4);
        CHECK(value == 1000 + n);
    }
    return true;
}

std::vector<std::uint8_t> reference;  // what the device without workers drew

// One device: creates, submits twice, and leaves the bytes of both submissions (equal) in `out`.
bool draw_all(const Setup& setup, const char* threads, int meet, std::vector<std::uint8_t>& out) {
    Ref<ID3D12Device> dev;
    CHECK(setup.device(threads, dev));
    Pipelines pipelines;
    Frame frame;
    hooks.reset();
    CHECK(pipelines.create(dev.p));
    const int at_creation = hooks.creates;
    CHECK(frame.create(dev.p, pipelines));
    {
        std::lock_guard lock(hooks.mutex);
        hooks.meet = meet;
        hooks.most_in_flight = 0;
    }
    CHECK(frame.submit() == ReplayResult::success);
    out = frame.read();
    CHECK(expected(out));
    // First use: each base pipeline the draws need (none of the variants'), each variant, each compute pipeline, once.
    // Otherwise the base and compute pipelines were built at creation and only the variants are left.
    CHECK(at_creation == (first_use ? 0 : int(kPipelines)));
    const int drawn = hooks.creates - at_creation;
    CHECK(drawn == int(first_use ? kPipelines : kVariants));
    const auto stats = rt::d3d12::pipeline_worker_stats(dev.p);
    const bool workers = std::strcmp(threads, "0") != 0;
    CHECK(stats.threads == unsigned(std::atoi(threads)));
    const unsigned large = first_use ? kLargeGraphics + kLargeCompute : kLargeVariants;
    CHECK(stats.batches == (workers ? 1u : 0u) && !stats.running);
    CHECK(stats.jobs == (workers ? unsigned(drawn) - large + 1 : 0u));
    CHECK(workers ? hooks.most_in_flight >= 2 && hooks.most_in_flight <= std::atoi(threads) + 1
                  : hooks.most_in_flight == 1);
    CHECK(workers || hooks.threads.size() == 1);
    CHECK(hooks.threads.size() <= std::size_t(std::atoi(threads)) + 1);
    CHECK(frame.submit() == ReplayResult::success);
    CHECK(frame.read() == out);
    CHECK(hooks.creates == at_creation + drawn);
    CHECK(rt::d3d12::pipeline_worker_stats(dev.p).batches == stats.batches);
    return true;
}

bool same_image(const Setup& setup) {
    std::vector<std::uint8_t> out;
    CHECK(draw_all(setup, "0", 0, reference));
    for (const char* threads : {"1", "4", "16"}) {
        CHECK(draw_all(setup, threads, 2, out));
        CHECK(out == reference);
    }
    return true;
}

// The driver refuses one graphics compile: the submission is unsupported and nothing of it is drawn; the next one succeeds.
bool refused_graphics(const Setup& setup) {
    for (const char* threads : {"0", "4"}) {
        Ref<ID3D12Device> dev;
        CHECK(setup.device(threads, dev));
        Pipelines pipelines;
        Frame frame;
        CHECK(pipelines.create(dev.p) && frame.create(dev.p, pipelines));
        hooks.reset();
        hooks.fail_graphics = 4;
        CHECK(frame.submit() == ReplayResult::unsupported);
        // Serially the fifth draw's is the last one tried; with workers the others were asked for too.
        const bool workers = std::strcmp(threads, "0") != 0;
        CHECK(hooks.creates == (workers ? int(kPipelines) : 5));
        hooks.reset();
        CHECK(frame.submit() == ReplayResult::success);
        CHECK(frame.read() == reference);
        CHECK(hooks.creates == (workers ? 1 : int(kPipelines) - 4));
    }
    return true;
}

// The driver refuses one compute compile: its dispatch is dropped, the rest of the submission runs, and it is not tried
// again, with and without workers.
bool refused_compute(const Setup& setup) {
    for (const char* threads : {"0", "4"}) {
        Ref<ID3D12Device> dev;
        CHECK(setup.device(threads, dev));
        Pipelines pipelines;
        Frame frame;
        CHECK(pipelines.create(dev.p) && frame.create(dev.p, pipelines));
        hooks.reset();
        hooks.fail_compute = 4;
        for (int round = 0; round < 2; ++round) {
            CHECK(frame.submit() == ReplayResult::success);
            const auto bytes = frame.read();
            CHECK(std::equal(bytes.begin(), bytes.begin() + 4 * kExtent * kExtent, reference.begin()));
            unsigned missing = 0;
            for (unsigned n = 0; n < kCompute; ++n) {
                std::uint32_t value = 0;
                std::memcpy(&value, &bytes[4 * kExtent * kExtent + 4 * n], 4);
                CHECK(value == 1000 + n || value == 0);  // a committed buffer reads zero where nothing wrote it
                missing += value == 0;
            }
            CHECK(missing == 1);
            CHECK(hooks.creates == int(kPipelines));
        }
    }
    return true;
}

// Two queues submit the same pipelines at once: each is compiled once and both images are right.
bool two_queues(const Setup& setup) {
    for (int round = 0; round < 3; ++round) {
        Ref<ID3D12Device> dev;
        CHECK(setup.device("4", dev));
        Pipelines pipelines;
        Frame frames[2];
        CHECK(pipelines.create(dev.p) && frames[0].create(dev.p, pipelines) && frames[1].create(dev.p, pipelines));
        hooks.reset();
        hooks.meet = 2;
        ReplayResult results[2]{};
        std::vector<std::uint8_t> bytes[2];
        std::thread threads[2];
        for (int i = 0; i < 2; ++i)
            threads[i] = std::thread([&, i] {
                results[i] = frames[i].submit();
                bytes[i] = frames[i].read();
            });
        for (auto& t : threads) t.join();
        for (int i = 0; i < 2; ++i) CHECK(results[i] == ReplayResult::success && bytes[i] == reference);
        CHECK(hooks.creates == int(kPipelines));
    }
    return true;
}

// Pipeline states released while a submission compiles them, and one it does not use.
bool released_while_compiling(const Setup& setup) {
    Ref<ID3D12Device> dev;
    CHECK(setup.device("4", dev));
    Pipelines pipelines;
    Frame frame;
    CHECK(pipelines.create(dev.p) && frame.create(dev.p, pipelines));
    auto* other = Pipelines::graphics(dev.p, pipelines.root.p, kGraphics);
    CHECK(other);
    hooks.reset();
    hooks.closed = true;
    ReplayResult result = ReplayResult::unsupported;
    std::thread submitting([&] { result = frame.submit(); });
    const bool compiling = hooks.wait_in_flight(2);
    pipelines.release();  // the application's references; the lists keep theirs
    std::atomic<bool> released = false;
    std::thread releasing([&] {
        other->Release();
        released = true;
    });
    std::this_thread::sleep_for(200ms);
    const bool early = released;
    hooks.open();
    submitting.join();
    releasing.join();
    CHECK(compiling);
    CHECK(!early);  // its destructor waits for the submission that looks pipelines up
    CHECK(result == ReplayResult::success && frame.read() == reference);
    return true;
}

// The device's last references are released by the thread whose submission is compiling.
bool device_destroyed(const Setup& setup) {
    _putenv_s("LEONIDA_PIPELINE_CACHE_BYTES", "1");  // read the cache at teardown, write no file
    Ref<ID3D12Device> dev;
    CHECK(setup.device("4", dev));
    auto pipelines = std::make_unique<Pipelines>();
    auto frame = std::make_unique<Frame>();
    CHECK(pipelines->create(dev.p) && frame->create(dev.p, *pipelines));
    hooks.reset();
    hooks.track = true;
    hooks.closed = true;
    ReplayResult result = ReplayResult::unsupported;
    std::vector<std::uint8_t> bytes;
    std::thread submitting([&] {
        result = frame->submit();
        bytes = frame->read();
        frame.reset();
        pipelines.reset();
    });
    const bool compiling = hooks.wait_in_flight(2);
    dev.reset();
    hooks.open();
    submitting.join();
    _putenv_s("LEONIDA_PIPELINE_CACHE_BYTES", "0");
    CHECK(compiling);
    CHECK(result == ReplayResult::success && bytes == reference);
    CHECK(hooks.cache_reads > 0);
    CHECK(!hooks.cache_read_during_create);
    CHECK(!hooks.cache_read_with_workers);
    CHECK(hooks.live == 0);
    return true;
}

// LEONIDA_PIPELINE_PREWARM_ORDER (src/pipeline_prewarm.h) with graphics pipeline states from console streams. A first
// device records the order (LEONIDA_PIPELINE_ORDER_LOG=1); the next ones read it with an eight-entry cap and wait, before
// recording any command, for the prewarm to build those eight. With the stream kept for its first bind (on-demand) the
// parse is the prewarm's first job, so the eight are built before any bind there too. Without a plan nothing is built
// before the submission.
bool prewarm_from_recorded_order(const Setup& setup) {
    const testfiles::ScratchDir dir("leonida_pipeline_prewarm_test");
    const std::string log = (dir.path() / "pipeline_order.log").string();
    _putenv_s("LEONIDA_PIPELINE_CACHE_DIR", dir.path().string().c_str());
    const auto draw = [&](bool on_demand, int before_bind) {
        Ref<ID3D12Device> inner, dev;
        CHECK(setup.device("4", inner, on_demand));
        // The console device (slot 86 is its CreatePipelineState), as the title sees it.
        CHECK(SUCCEEDED(rt::d3d12_xs::create_device_proxy(inner.p, __uuidof(ID3D12Device),
                                                          reinterpret_cast<void**>(&dev.p))));
        rt::d3d12::set_device_outer(inner.p, dev.p);
        ID3D12Device* const pc = inner.p;  // the rest is made through the PC device the proxy wraps
        inner.p = nullptr;                 // the proxy owns that reference now
        Pipelines pipelines;
        pipelines.console = dev.p;
        hooks.reset();
        CHECK(pipelines.create(pc));
        if (before_bind) {
            std::unique_lock lock(hooks.mutex);
            hooks.changed.wait_for(lock, 8s, [&] { return hooks.graphics >= before_bind; });
        }
        std::this_thread::sleep_for(200ms);  // nothing more comes
        {
            std::lock_guard lock(hooks.mutex);
            if (hooks.graphics != before_bind)
                std::fprintf(stderr, "prewarm (%s): %d graphics builds before the first bind, %d expected\n",
                             on_demand ? "on-demand" : "console stream", hooks.graphics, before_bind);
            CHECK(hooks.graphics == before_bind);
        }
        Frame frame;
        CHECK(frame.create(pc, pipelines));
        CHECK(frame.submit() == ReplayResult::success);
        CHECK(frame.read() == reference);
        CHECK(hooks.graphics == int(kGraphics));  // each built once, prewarmed or not
        return true;
    };
    _putenv_s("LEONIDA_PIPELINE_ORDER_LOG", "1");
    const bool recorded = draw(false, 0);  // no plan yet: nothing before the bind
    _putenv_s("LEONIDA_PIPELINE_ORDER_LOG", "");
    CHECK(recorded);
    CHECK(std::filesystem::exists(log));
    _putenv_s("LEONIDA_PIPELINE_PREWARM_ORDER", log.c_str());
    _putenv_s("LEONIDA_PIPELINE_PREWARM_MAX_ENTRIES", "8");
    const bool console = draw(false, 8);
    const bool on_demand = draw(true, 8);
    _putenv_s("LEONIDA_PIPELINE_PREWARM_ORDER", "");
    _putenv_s("LEONIDA_PIPELINE_PREWARM_MAX_ENTRIES", "");
    const bool no_plan = draw(true, 0);
    _putenv_s("LEONIDA_PIPELINE_CACHE_DIR", "");
    CHECK(console && on_demand && no_plan);
    return true;
}

// ---- LEONIDA_ASYNC_MIN_BYTES_GRAPHICS (src/async_pipeline_size.h), with LEONIDA_ASYNC_PIPELINES=1. ----

// --async-graphics-zero: LEONIDA_ASYNC_MIN_BYTES over every module and LEONIDA_ASYNC_MIN_BYTES_GRAPHICS=0, with every
// job of the compile worker held. The first submission builds each compute pipeline where its dispatch needs it (every
// buffer word is written) and no graphics one: their draws are skipped, so the image stays as cleared, and their
// builds wait on the worker. Once the worker is let go, a later submission draws every cell.
// --async-graphics-unset: the same with LEONIDA_ASYNC_MIN_BYTES_GRAPHICS unset. Graphics pipelines then take
// LEONIDA_ASYNC_MIN_BYTES and are built in place too: the first submission draws everything and the worker gets nothing.
bool async_thresholds(const Setup& setup, bool graphics_async) {
    CHECK(first_use);  // lazy pipelines: nothing is built at creation
    CHECK(rt::d3d12::async_replay_compile());
    CHECK(rt::d3d12::async_min_bytes().compute == 1073741824);
    CHECK(rt::d3d12::async_min_bytes().graphics == (graphics_async ? 0u : 1073741824u));
    Ref<ID3D12Device> dev;
    CHECK(setup.device("0", dev));
    Pipelines pipelines;
    Frame frame;
    // Declared after the device and the pipelines, so it lets the worker go before they are released, also when a
    // check fails.
    std::atomic<unsigned> worker_jobs{0};
    struct HeldWorker {
        HANDLE release = nullptr;
        void let_go() {
            if (!release) return;
            SetEvent(release);
            rt::d3d12::PipelineCompiler::instance().set_job_probe(nullptr);
        }
        ~HeldWorker() {
            let_go();
            if (release) CloseHandle(release);
        }
    } held;
    held.release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CHECK(held.release != nullptr);
    rt::d3d12::PipelineCompiler::instance().set_job_probe([&worker_jobs, release = held.release](const void*) {
        worker_jobs.fetch_add(1);
        WaitForSingleObject(release, INFINITE);
    });
    hooks.reset();
    CHECK(pipelines.create(dev.p) && frame.create(dev.p, pipelines));
    CHECK(hooks.creates == 0);
    CHECK(frame.submit() == ReplayResult::success);
    std::vector<std::uint8_t> bytes = frame.read();
    CHECK(bytes.size() == 4 * kExtent * kExtent + 4 * kCompute);
    if (!graphics_async) {
        CHECK(expected(bytes));
        CHECK(hooks.graphics == int(kGraphics) && hooks.compute == int(kCompute));
        CHECK(worker_jobs == 0);
        return true;
    }
    // Every compute pipeline was built in place (the worker is held), no graphics one reached the driver.
    for (unsigned n = 0; n < kCompute; ++n) {
        std::uint32_t value = 0;
        std::memcpy(&value, &bytes[4 * kExtent * kExtent + 4 * n], 4);
        CHECK(value == 1000 + n);
    }
    CHECK(std::all_of(bytes.begin(), bytes.begin() + 4 * kExtent * kExtent, [](std::uint8_t b) { return b == 0; }));
    CHECK(hooks.compute == int(kCompute) && hooks.graphics == 0);
    CHECK(worker_jobs >= 1);
    // The worker let go: it builds the graphics pipelines (and variants), and a later submission draws them.
    held.let_go();
    const auto deadline = Clock::now() + 60s;
    const auto drawn = [&] {
        for (unsigned n = 0; n < kGraphics; ++n)
            if (bytes[4 * ((n / 8 * 2) * kExtent + n % 8 * 2) + 3] != 255) return false;
        return true;
    };
    do {
        std::this_thread::sleep_for(20ms);
        CHECK(frame.submit() == ReplayResult::success);
        bytes = frame.read();
    } while (!drawn() && Clock::now() < deadline);
    CHECK(expected(bytes));
    CHECK(hooks.graphics == int(kGraphics) && hooks.compute == int(kCompute));  // compute was not built again
    return true;
}

bool run(const Setup& setup) {
    if (!same_image(setup)) return false;
    return !first_use || (refused_graphics(setup) && refused_compute(setup) && two_queues(setup) &&
                          released_while_compiling(setup) && device_destroyed(setup) &&
                          prewarm_from_recorded_order(setup));
}

}  // namespace

int main(int argc, char** argv) {
    const bool graphics_zero = argc == 2 && std::strcmp(argv[1], "--async-graphics-zero") == 0;
    const bool graphics_unset = argc == 2 && std::strcmp(argv[1], "--async-graphics-unset") == 0;
    if (argc != 1 && !graphics_zero && !graphics_unset) {
        std::fprintf(stderr, "usage: d3d12_pipeline_workers_test [--async-graphics-zero | --async-graphics-unset]\n");
        return 2;
    }
    if (graphics_zero || graphics_unset) {  // before the first pipeline state: the runtime reads these once
        _putenv_s("LEONIDA_ASYNC_PIPELINES", "1");
        _putenv_s("LEONIDA_STRICT_REPLAY", "0");
        _putenv_s("LEONIDA_ASYNC_MIN_BYTES", "1073741824");  // 1 GiB: over any module
        _putenv_s("LEONIDA_ASYNC_MIN_BYTES_GRAPHICS", graphics_zero ? "0" : "");  // "" removes it
    }
    vktest::Device vk;
    vk.request_buffer_device_address = true;
    const auto status = vk.initialize();
    if (status == vktest::DeviceStatus::unavailable) return 77;
    if (status != vktest::DeviceStatus::ready) return 1;
    VkPhysicalDeviceProperties properties{};
    vk.instance_fn<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(vk.physical, &properties);
    if (!vk.buffer_device_address || !vk.enabled.shaderInt64) {
        std::printf("SKIP: the GPU lacks bufferDeviceAddress or shaderInt64\n");
        return 77;
    }
    if (properties.limits.maxPushConstantsSize < 144) {
        std::printf("SKIP: maxPushConstantsSize %u, below the 144-byte block\n", properties.limits.maxPushConstantsSize);
        return 77;
    }
    real_instance_proc = vk.instance_proc;
    _putenv_s("LEONIDA_PIPELINE_CACHE_BYTES", "0");  // no session file in or out
    _putenv_s("LEONIDA_COMPUTE_WARMUP", "0");        // every compile is a submission's
    const testfiles::ScratchDir library("leonida_pipeline_workers_test");
    if (!write_library(library.path())) return 1;
    const Setup setup{&vk, library.path().string()};
    if (graphics_zero || graphics_unset ? !async_thresholds(setup, graphics_zero) : !run(setup)) return 1;
    if (vk.validation_errors()) {
        std::fprintf(stderr, "FAIL: %u validation errors\n", vk.validation_errors());
        return 1;
    }
    std::printf("d3d12_pipeline_workers_test: passed on %s\n", properties.deviceName);
    return 0;
}
