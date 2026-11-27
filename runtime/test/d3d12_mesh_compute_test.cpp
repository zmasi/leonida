// Mesh-as-compute replay through the public Xbox D3D12 proxy, on a device without
// VK_EXT_mesh_shader (MESH_COMPUTE_D3D12_PLAN.md step 7, direct DispatchMesh).
// The mesh producers are real --mesh-compute translator output (GLCompute
// modules plus index.json mesh_compute layouts) and the consumers the matching
// tools/mesh_compute_vs.py vertex shaders; every PSO is created from
// game-shaped DXBC containers through the real ShaderLibrary lookup, and every
// draw is a real DispatchMesh recorded on a D3D12 command list. No title
// inputs, no standalone harness, no test-owned Vulkan pipelines.
//
// What is verified:
//   - Grid A, ms_v5_workgroup DispatchMesh(2, 3, 4): a genuine XYZ grid. Each
//     group draws a full-screen triangle in its own group-index colour and the
//     last group (g = 23) wins every pixel; the fold pixel shader also reads a
//     nonzero graphics root constant, so dropped roots fail too. A replay that
//     forced Y or Z to 1 (the console clamp) would show g = 5 or 19.
//   - Grid B, ms_tri DispatchMesh(2, 2, 1) with a different root signature,
//     tint CBV, depth buffer and quadrant viewport/scissor: the (u, v, 0.5)
//     gradient, checked per pixel, and the D32 depth (0.0 in the quadrant).
//   - The ms_clip edge: clip distance through the generated vertex shader
//     clips the left half; the right half shows the gradient with a second tint.
//   - The simple f16 case, ms_f16_pack DispatchMesh(1, 1, 1) with
//     ps_interp_f16_high (both --f16-interp-fallback, main-native decoded halves,
//     no #1930 computed IJ): the mesh packs f16(u)|f16(v) plus f32 (u, v), the PS
//     reads both halves plus f32 v. Every pixel is (u, v, u, v) in 0..1,
//     checked per pixel with a one-step tolerance for f16/UNORM quantization.
//   - The native barycentric case, ms_tri DispatchMesh(1, 1, 1) with ps_bary
//     (translated plainly, no --bary-fallback flag): raw i/j need
//     VK_KHR_fragment_shader_barycentric. The test device requests it opt-in and
//     enables the extension+feature where available; where not actually enabled
//     (Vega 20) creation is E_NOTIMPL with a nulled pointer and no draw, and the
//     same enabled bit reaches info.fragment_shader_barycentric. Where enabled the
//     draw executes and the target is not clear. RX varies: branch on enabled, never spoof.
//   - Two grids plus the clip and f16 draws live in one submission (per-draw
//     scratch isolation), and grid A is drawn again in a second submission.
//     The readback PSOs state CULL_NONE explicitly: the fixtures emit
//     back-facing triangles (counter-clockwise in D3D screen space), so the
//     default back-face culling would remove them. Complementary controls
//     prove the cull state reaches the consumer: the same draw with CULL_BACK
//     leaves the target clear, with CULL_FRONT it renders.
//   - A zero Z grid records a Draw the replay executes as a no-op (target and
//     sentinels unchanged); zero X or Y grids record nothing.
//   - Purpose-built CreatePipelineState negatives, each asserting its own HRESULT
//     and that an initialized output pointer is nulled (never a reused rc1 or a
//     console message): a genuinely over-limit geometry interface (ms_overlimit,
//     32 params, 132 components, over the queried maxVertexOutputComponents on
//     Vega) is E_NOTIMPL; a native PerVertexKHR/BaryCoord PS without the queried
//     fragmentShaderBarycentric feature is E_NOTIMPL; MS+GS, MS+VS and truncated
//     streams are E_INVALIDARG. Native MeshEXT stays E_NOTIMPL, over-limit grids
//     fail at Close, and wrong-kind draws fail at execution.
//   - Lazy producers: a mesh-compute PSO is fully validated at creation, but its compute producer is a driver
//     build, compiled once at the first DispatchMesh that executes it. Under the default (lazy) run no producer
//     exists after creation and the process-wide build count has not moved; a PSO that is never drawn never gets
//     one; every drawn PSO gets exactly one and its draw renders (the pixel checks above). Under
//     LEONIDA_LAZY_PIPELINES=0 (d3d12_mesh_compute_eager) every producer is built at creation, as before.
//     Signature on stderr: "mesh-compute producers: mode=... psos=N drawn=M built=B unused_built=0|1".
//   - Controls on the no-extension device: MeshShaderTier stays NOT_SUPPORTED,
//     and a DISPATCH_MESH command signature is refused E_NOTIMPL (stock main;
//     this branch is independent of the signature fix, so no success is expected).
//     Direct DispatchMesh only; no ExecuteIndirect mesh in this test.
// On the unmodified base this test fails red at the first mesh-compute PSO
// creation (the stock no-extension refusal, E_NOTIMPL). It never skips for a
// missing mesh extension; exit 77 is only for no usable Vulkan device or a
// device missing a required feature or limit (BDA, Int64, clip distance,
// 152 push bytes).
#include "vk_test_device.h"
#include "mesh_compute_fixture.h"
#include "test_temp_dir.h"
#include "../shims/d3d12_com.h"
#include "../shims/d3d12_mesh_compute_direct.h"
#include "../shims/d3d12_xs_device.h"
#include "../shims/d3d12_xs_pso_stream.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <variant>
#include <vector>

#if !defined(LEONIDA_TEST_MESH_COMPUTE_DIR)
#error "LEONIDA_TEST_MESH_COMPUTE_DIR must name the folder build_mesh_compute.py wrote (spirv/index.json, fssm/*.fssm)"
#endif

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

using namespace d3d12test;
using namespace meshcomputetest;
using XboxHeapHandleFn = std::uint64_t(WINAPI*)(void* heap);  // heap slot 9: handle in RAX

int main() {
    // Belt and braces: the representation is the index's mesh_compute layout,
    // but an implementation that also gates on the harness flag must see it.
    EXPECT(SetEnvironmentVariableW(L"LEONIDA_MESH_COMPUTE", L"1"));
    vktest::Device d;
    d.request_mesh_shader = true;
    d.request_buffer_device_address = true;  // output slots are addressed (genuine BDA, gated below)
    d.request_clip_distance = true;  // the ms_clip edge (genuine clip, gated below)
    d.request_fragment_shader_barycentric = true;  // native bary PS: enabled where available, refused without
    const auto device_status = d.initialize();
    if (device_status == vktest::DeviceStatus::unavailable) {
        std::fprintf(stderr, "SKIP: no usable Vulkan 1.3 device\n");
        return 77;
    }
    EXPECT(device_status == vktest::DeviceStatus::ready);
    // Mesh compute needs buffer device addresses, 64-bit integers and clip
    // distance, all genuinely enabled on the device (never assumed): 77 where
    // the hardware lacks them, as for push constants below. No geometry-shader
    // requirement: this branch builds no runtime GS (bary fallback is refused).
    if (!d.buffer_device_address) {
        std::fprintf(stderr, "SKIP: need bufferDeviceAddress\n");
        return 77;
    }
    if (!d.enabled.shaderInt64) {
        std::fprintf(stderr, "SKIP: need shaderInt64\n");
        return 77;
    }
    if (!d.enabled.shaderClipDistance) {
        std::fprintf(stderr, "SKIP: need shaderClipDistance\n");
        return 77;
    }

    // Mesh compute needs the 152-byte push block (144 plus the output address
    // in dwords 36..37). A 128-byte device cannot run it: 77, as vk_harness.
    // The limits query captures what the over-limit negative branches on
    // (queried, never spoofed; RX varies). Barycentrics branch on the *actually
    // enabled* device bit below, not a physical query: a capable card that did
    // not enable the extension+feature must still take the refusal path.
    std::uint32_t max_vertex_out = 0, max_frag_in = 0;
    {
        const auto get_props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
            d.instance_proc(d.instance, "vkGetPhysicalDeviceProperties"));
        EXPECT(get_props != nullptr);
        VkPhysicalDeviceProperties props{};
        get_props(d.physical, &props);
        if (props.limits.maxPushConstantsSize < 152) {
            std::fprintf(stderr, "SKIP: need 152 push constant bytes, device has %u\n",
                         props.limits.maxPushConstantsSize);
            return 77;
        }
        max_vertex_out = props.limits.maxVertexOutputComponents;
        max_frag_in = props.limits.maxFragmentInputComponents;
    }
    const bool barycentric_capable = d.fragment_shader_barycentric;
    std::fprintf(stderr, "limits: maxVertexOutput=%u maxFragmentInput=%u barycentric-enabled=%d\n",
                 max_vertex_out, max_frag_in, barycentric_capable ? 1 : 0);

    // The staged library: translator-derived producers, generated vertex shaders, GLSL pixel
    // shaders, pair seals and the merged index, keyed by their staged FSSM programs. A per-process
    // scratch folder: eager/lazy CTest copies may share a prefix or run concurrently.
    const std::filesystem::path staged = LEONIDA_TEST_MESH_COMPUTE_DIR;
    testfiles::ScratchDir scratch("leonida_mesh_compute_test");
    const std::filesystem::path& library = scratch.path();
    EXPECT(stage_library(staged / "spirv", library));
    const auto read_staged = [&](const char* name) { return read_bytes(staged / "fssm" / name); };
    const auto v5_program = read_staged("ms_v5_workgroup.fssm");
    const auto tri_program = read_staged("ms_tri.fssm");
    const auto clip_program = read_staged("ms_clip.fssm");
    const auto f16_ms_program = read_staged("ms_f16_pack.fssm");
    const auto f16_ps_program = read_staged("ps_interp_f16_high.fssm");
    const auto bary_ps_program = read_staged("ps_bary.fssm");
    const auto over_program = read_staged("ms_overlimit.fssm");
    const auto ps_draw_program = read_staged("ps_draw.fssm");
    const auto fold_program = read_staged("ps_mesh_group_fold.fssm");
    const auto native_program = read_staged("ms_native.fssm");
    const auto vs_program = read_staged("vs_fullscreen.fssm");
    EXPECT(!v5_program.empty() && !tri_program.empty() && !clip_program.empty());
    EXPECT(!f16_ms_program.empty() && !f16_ps_program.empty());
    EXPECT(!bary_ps_program.empty() && !over_program.empty());
    EXPECT(!ps_draw_program.empty() && !fold_program.empty());
    EXPECT(!native_program.empty() && !vs_program.empty());
    const auto v5_container = mesh_container(v5_program);
    const auto tri_container = mesh_container(tri_program);
    const auto clip_container = mesh_container(clip_program);
    const auto f16_ms_container = mesh_container(f16_ms_program);
    const auto over_container = mesh_container(over_program);
    const auto native_container = mesh_container(native_program);
    const auto ps_draw_container = graphics_container(ps_draw_program);
    const auto fold_container = graphics_container(fold_program);
    const auto f16_ps_container = graphics_container(f16_ps_program);
    const auto bary_ps_container = graphics_container(bary_ps_program);
    const auto vs_container = graphics_container(vs_program);
    const D3D12_SHADER_BYTECODE ms_v5{v5_container.data(), v5_container.size()};
    const D3D12_SHADER_BYTECODE ms_tri{tri_container.data(), tri_container.size()};
    const D3D12_SHADER_BYTECODE ms_clip{clip_container.data(), clip_container.size()};
    const D3D12_SHADER_BYTECODE ms_f16{f16_ms_container.data(), f16_ms_container.size()};
    const D3D12_SHADER_BYTECODE ms_over{over_container.data(), over_container.size()};
    const D3D12_SHADER_BYTECODE ms_native{native_container.data(), native_container.size()};
    const D3D12_SHADER_BYTECODE ps_draw{ps_draw_container.data(), ps_draw_container.size()};
    const D3D12_SHADER_BYTECODE ps_fold{fold_container.data(), fold_container.size()};
    const D3D12_SHADER_BYTECODE ps_f16{f16_ps_container.data(), f16_ps_container.size()};
    const D3D12_SHADER_BYTECODE ps_bary{bary_ps_container.data(), bary_ps_container.size()};
    const D3D12_SHADER_BYTECODE vs{vs_container.data(), vs_container.size()};

    // Always the no-extension device: emulation must work where native mesh is
    // absent, and the native controls must refuse there on every GPU.
    rt::d3d12::VulkanDeviceInfo info{};
    info.prepare_on_bind = d.prepare_on_bind;
    info.cold_graphics_compile = d.cold_graphics_compile;
    info.heavy_pipeline_cache = d.heavy_pipeline_cache;
    info.get_instance_proc = d.instance_proc;
    info.instance = d.instance;
    info.physical = d.physical;
    info.device = d.device;
    info.queue = d.queue;
    info.queue_family = d.family;
    info.buffer_device_address = true;
    info.shader_dir = library.string();
    info.mesh_shader = false;
    info.mesh_shader_queries = false;
    info.fragment_shader_barycentric = d.fragment_shader_barycentric;  // exactly what vkCreateDevice got
    info.pipeline_statistics_query = d.pipeline_statistics_query;
    info.conservative_rasterization = d.conservative_rasterization;
    info.features = d.enabled;  // exactly what vkCreateDevice got (clip, Int64, geometry, ...)
    Ref<ID3D12Device> device;
    {
        ID3D12Device* inner = nullptr;
        EXPECT(SUCCEEDED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&inner))));
        EXPECT(SUCCEEDED(rt::d3d12_xs::create_device_proxy(inner, __uuidof(ID3D12Device),
                                                           reinterpret_cast<void**>(&device.p))));
        rt::d3d12::set_device_outer(inner, device.p);
    }
    // Grid A roots: two constants (push 8, 9); the fold shader reads k at s8.
    const auto root_signature_a = [&](Ref<ID3D12RootSignature>& root) {
        D3D12_ROOT_PARAMETER param{};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        param.Constants = {0, 0, 2};
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
        root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
        root_desc.Desc_1_0 = {1, &param, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        const auto blob = serialize_root_signature(root_desc);
        return !blob.empty() &&
               SUCCEEDED(device->CreateRootSignature(0, blob.data(), blob.size(), IID_PPV_ARGS(&root.p)));
    };
    // Grid B roots: two constants plus a root CBV (the tint), as in d3d12_mesh_test.
    const auto root_signature_b = [&](Ref<ID3D12RootSignature>& root) {
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants = {0, 0, 2};
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[1].Descriptor = {1, 0};
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
        root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
        root_desc.Desc_1_0 = {2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        const auto blob = serialize_root_signature(root_desc);
        return !blob.empty() &&
               SUCCEEDED(device->CreateRootSignature(0, blob.data(), blob.size(), IID_PPV_ARGS(&root.p)));
    };
    Ref<ID3D12RootSignature> root_a, root_b;
    EXPECT(root_signature_a(root_a));
    EXPECT(root_signature_b(root_b));

    // Emulation advertises no mesh tier.
    EXPECT(mesh_tier(device.p) == D3D12_MESH_SHADER_TIER_NOT_SUPPORTED);
    // A native MeshEXT module on this device is still refused, with or without
    // the emulation: stock behaviour, unchanged. The output pointer is
    // initialized to a sentinel so a missing null is caught, not a reused rc1.
    {
        Stream native_stream;
        native_stream.add(RootSubobject{.inner = root_b.p}).add(MsSubobject{.inner = ms_native});
        native_stream.add(PsSubobject{.inner = ps_draw}).add(TargetsSubobject{.inner = one_target()});
        Ref<ID3D12PipelineState> refused;
        EXPECT(create_pso(device.p, native_stream, refused) == E_NOTIMPL && !refused.p);
        ID3D12PipelineState* raw = reinterpret_cast<ID3D12PipelineState*>(std::uintptr_t(0x1234));
        Ref<ID3D12Device2> device2;
        EXPECT(SUCCEEDED(device.p->QueryInterface(IID_PPV_ARGS(&device2.p))));
        const D3D12_PIPELINE_STATE_STREAM_DESC native_desc = native_stream.desc();
        EXPECT(device2->CreatePipelineState(&native_desc, IID_PPV_ARGS(&raw)) == E_NOTIMPL && raw == nullptr);
    }
    // Stock command-signature behaviour on the no-extension device: a DISPATCH_MESH
    // description is refused E_NOTIMPL with a nulled pointer. This branch is
    // independent of the signature fix, so no success is expected here; the
    // separate d3d12_mesh controls cover the with-extension path.
    {
        const D3D12_INDIRECT_ARGUMENT_DESC mesh_sig_arg{D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH};
        const D3D12_COMMAND_SIGNATURE_DESC mesh_sig{12, 1, &mesh_sig_arg, 0};
        ID3D12CommandSignature* raw_sig =
            reinterpret_cast<ID3D12CommandSignature*>(std::uintptr_t(0x5678));
        EXPECT(device->CreateCommandSignature(&mesh_sig, nullptr, IID_PPV_ARGS(&raw_sig)) == E_NOTIMPL &&
               raw_sig == nullptr);
    }

    // The emulated PSOs. Grid A: no depth buffer, depth explicitly off. Grid B:
    // D32 depth, LESS, writes on. The clip edge reuses grid B's shape. All readback
    // PSOs state CULL_NONE: the fixtures are back-facing, and the default CULL_BACK
    // would remove them. pso_back/pso_front repeat grid A with culled faces to prove
    // the state reaches the consumer. pso_f16 pairs the simple f16 mesh/PS
    // (main-native decoded halves, no #1930). pso_bary pairs ms_tri with the
    // native-bary PS where the queried device is barycentric-capable; otherwise
    // its creation is the E_NOTIMPL refusal below and no draw uses it.
    Ref<ID3D12PipelineState> pso_a, pso_b, pso_clip, pso_back, pso_front, pso_f16, pso_bary, pso_unused, vs_pso,
        refused;
    // Producer builds so far: every refusal above happens before a producer is built, so this is the baseline.
    const std::uint64_t producer_builds_at_start = rt::d3d12::mesh_compute_producer_builds();
    const auto grid_a_stream = [&](D3D12_CULL_MODE cull) {
        Stream stream;
        stream.add(RootSubobject{.inner = root_a.p}).add(MsSubobject{.inner = ms_v5});
        stream.add(PsSubobject{.inner = ps_fold}).add(TargetsSubobject{.inner = one_target()});
        D3D12_DEPTH_STENCIL_DESC depth_off{};
        depth_off.DepthEnable = FALSE;
        depth_off.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        depth_off.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        stream.add(DepthStencilSubobject{.inner = depth_off});
        stream.add(RasterizerSubobject{.inner = cull_rasterizer(cull)});
        return stream;
    };
    {
        Stream stream_a = grid_a_stream(D3D12_CULL_MODE_NONE);
        // Red on the unmodified base: the stock no-extension refusal (E_NOTIMPL).
        const HRESULT hr_a = create_pso(device.p, stream_a, pso_a);
        if (hr_a == E_NOTIMPL) std::fprintf(stderr, "mesh-compute PSO refused as not implemented\n");
        EXPECT(SUCCEEDED(hr_a) && pso_a.p);
        Stream stream_back = grid_a_stream(D3D12_CULL_MODE_BACK);
        EXPECT(SUCCEEDED(create_pso(device.p, stream_back, pso_back)) && pso_back.p);
        Stream stream_front = grid_a_stream(D3D12_CULL_MODE_FRONT);
        EXPECT(SUCCEEDED(create_pso(device.p, stream_front, pso_front)) && pso_front.p);
        // A mesh PSO no command list ever records: the lazy proof's unused PSO. Its own shape (ms_tri with the
        // grid-B pixel shader, no depth, front-face culling), so no other PSO shares what it is made of.
        Stream stream_unused;
        stream_unused.add(RootSubobject{.inner = root_b.p}).add(MsSubobject{.inner = ms_tri});
        stream_unused.add(PsSubobject{.inner = ps_draw}).add(TargetsSubobject{.inner = one_target()});
        stream_unused.add(RasterizerSubobject{.inner = cull_rasterizer(D3D12_CULL_MODE_FRONT)});
        EXPECT(SUCCEEDED(create_pso(device.p, stream_unused, pso_unused)) && pso_unused.p);
    }
    {
        D3D12_DEPTH_STENCIL_DESC depth_on{};
        depth_on.DepthEnable = TRUE;
        depth_on.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        depth_on.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        Stream stream_b;
        stream_b.add(RootSubobject{.inner = root_b.p}).add(MsSubobject{.inner = ms_tri});
        stream_b.add(PsSubobject{.inner = ps_draw}).add(TargetsSubobject{.inner = one_target()});
        stream_b.add(DsvFormatSubobject{.inner = DXGI_FORMAT_D32_FLOAT});
        stream_b.add(DepthStencilSubobject{.inner = depth_on});
        stream_b.add(RasterizerSubobject{.inner = cull_rasterizer(D3D12_CULL_MODE_NONE)});
        EXPECT(SUCCEEDED(create_pso(device.p, stream_b, pso_b)) && pso_b.p);
        Stream stream_clip;
        stream_clip.add(RootSubobject{.inner = root_b.p}).add(MsSubobject{.inner = ms_clip});
        stream_clip.add(PsSubobject{.inner = ps_draw}).add(TargetsSubobject{.inner = one_target()});
        stream_clip.add(RasterizerSubobject{.inner = cull_rasterizer(D3D12_CULL_MODE_NONE)});
        EXPECT(SUCCEEDED(create_pso(device.p, stream_clip, pso_clip)) && pso_clip.p);
        Stream stream_f16;
        stream_f16.add(RootSubobject{.inner = root_a.p}).add(MsSubobject{.inner = ms_f16});
        stream_f16.add(PsSubobject{.inner = ps_f16}).add(TargetsSubobject{.inner = one_target()});
        stream_f16.add(RasterizerSubobject{.inner = cull_rasterizer(D3D12_CULL_MODE_NONE)});
        EXPECT(SUCCEEDED(create_pso(device.p, stream_f16, pso_f16)) && pso_f16.p);
    }
    // Native barycentrics gate on the queried feature: refusal without it (with a
    // nulled pointer), a usable PSO with it. RX varies; Vega 20 takes the refusal.
    {
        Stream stream_bary;
        stream_bary.add(RootSubobject{.inner = root_a.p}).add(MsSubobject{.inner = ms_tri});
        stream_bary.add(PsSubobject{.inner = ps_bary}).add(TargetsSubobject{.inner = one_target()});
        stream_bary.add(RasterizerSubobject{.inner = cull_rasterizer(D3D12_CULL_MODE_NONE)});
        if (!barycentric_capable) {
            ID3D12PipelineState* raw_bary =
                reinterpret_cast<ID3D12PipelineState*>(std::uintptr_t(0x9ABC));
            Ref<ID3D12Device2> device2b;
            EXPECT(SUCCEEDED(device.p->QueryInterface(IID_PPV_ARGS(&device2b.p))));
            const D3D12_PIPELINE_STATE_STREAM_DESC bary_desc = stream_bary.desc();
            EXPECT(device2b->CreatePipelineState(&bary_desc, IID_PPV_ARGS(&raw_bary)) == E_NOTIMPL &&
                   raw_bary == nullptr);
            std::fprintf(stderr, "native bary PSO refused E_NOTIMPL (no fragmentShaderBarycentric)\n");
        } else {
            EXPECT(SUCCEEDED(create_pso(device.p, stream_bary, pso_bary)) && pso_bary.p);
        }
    }
    // Genuinely over-limit geometry: ms_overlimit exports 32 params (132
    // components with position), over the queried vertex-output limit on Vega.
    // Proved over before asserting, so an RX with higher limits skips explicitly.
    {
        const unsigned over_total = 4 + 32 * 4;
        if (over_total > max_vertex_out) {
            Stream stream_over;
            stream_over.add(RootSubobject{.inner = root_a.p}).add(MsSubobject{.inner = ms_over});
            stream_over.add(PsSubobject{.inner = ps_draw}).add(TargetsSubobject{.inner = one_target()});
            stream_over.add(RasterizerSubobject{.inner = cull_rasterizer(D3D12_CULL_MODE_NONE)});
            ID3D12PipelineState* raw_over =
                reinterpret_cast<ID3D12PipelineState*>(std::uintptr_t(0xDEF0));
            Ref<ID3D12Device2> device2o;
            EXPECT(SUCCEEDED(device.p->QueryInterface(IID_PPV_ARGS(&device2o.p))));
            const D3D12_PIPELINE_STATE_STREAM_DESC over_desc = stream_over.desc();
            EXPECT(device2o->CreatePipelineState(&over_desc, IID_PPV_ARGS(&raw_over)) == E_NOTIMPL &&
                   raw_over == nullptr);
            std::fprintf(stderr, "over-limit PSO refused E_NOTIMPL (%u > %u vertex-output)\n",
                         over_total, max_vertex_out);
        } else {
            std::fprintf(stderr, "SKIP over-limit refusal: device allows %u components\n", max_vertex_out);
        }
    }
    {
        Stream stream_vs;
        stream_vs.add(RootSubobject{.inner = root_b.p}).add(VsSubobject{.inner = vs});
        stream_vs.add(PsSubobject{.inner = ps_draw});
        stream_vs.add(TopologySubobject{.inner = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE});
        stream_vs.add(TargetsSubobject{.inner = one_target()});
        EXPECT(SUCCEEDED(create_pso(device.p, stream_vs, vs_pso)) && vs_pso.p);
    }
    // A mesh stream takes no other shader stage, emulated or not; a truncated
    // stream is malformed too. Each asserts E_INVALIDARG with a nulled pointer.
    {
        Ref<ID3D12Device2> device2m;
        EXPECT(SUCCEEDED(device.p->QueryInterface(IID_PPV_ARGS(&device2m.p))));
        Stream bad;
        bad.add(RootSubobject{.inner = root_a.p}).add(MsSubobject{.inner = ms_v5});
        bad.add(GsSubobject{.inner = vs}).add(PsSubobject{.inner = ps_fold});
        bad.add(TargetsSubobject{.inner = one_target()});
        EXPECT(create_pso(device.p, bad, refused) == E_INVALIDARG && !refused.p);
        ID3D12PipelineState* raw_gs =
            reinterpret_cast<ID3D12PipelineState*>(std::uintptr_t(0x1111));
        const D3D12_PIPELINE_STATE_STREAM_DESC bad_gs = bad.desc();
        EXPECT(device2m->CreatePipelineState(&bad_gs, IID_PPV_ARGS(&raw_gs)) == E_INVALIDARG &&
               raw_gs == nullptr);
        bad = Stream{};
        bad.add(RootSubobject{.inner = root_a.p}).add(MsSubobject{.inner = ms_v5});
        bad.add(VsSubobject{.inner = vs}).add(PsSubobject{.inner = ps_fold});
        bad.add(TargetsSubobject{.inner = one_target()});
        EXPECT(create_pso(device.p, bad, refused) == E_INVALIDARG && !refused.p);
        ID3D12PipelineState* raw_vs =
            reinterpret_cast<ID3D12PipelineState*>(std::uintptr_t(0x3333));
        const D3D12_PIPELINE_STATE_STREAM_DESC bad_vs = bad.desc();
        EXPECT(device2m->CreatePipelineState(&bad_vs, IID_PPV_ARGS(&raw_vs)) == E_INVALIDARG &&
               raw_vs == nullptr);
        Stream truncated = grid_a_stream(D3D12_CULL_MODE_NONE);
        ID3D12PipelineState* raw_trunc =
            reinterpret_cast<ID3D12PipelineState*>(std::uintptr_t(0x2222));
        const D3D12_PIPELINE_STATE_STREAM_DESC trunc_desc = truncated.desc(8);
        EXPECT(device2m->CreatePipelineState(&trunc_desc, IID_PPV_ARGS(&raw_trunc)) == E_INVALIDARG &&
               raw_trunc == nullptr);
    }

    // Lazy producers, after creation: every PSO above passed the sealed-pair, feature and limit validation, and none
    // has been drawn. Lazy (the default): no producer was compiled and no PSO owns one. Eager
    // (LEONIDA_LAZY_PIPELINES=0): all were compiled at creation.
    wchar_t lazy_value[2]{};
    const bool lazy_producers =
        !(GetEnvironmentVariableW(L"LEONIDA_LAZY_PIPELINES", lazy_value, 2) == 1 && lazy_value[0] == L'0');
    std::vector<ID3D12PipelineState*> mesh_psos{pso_a.p, pso_back.p, pso_front.p, pso_b.p,
                                                pso_clip.p, pso_f16.p, pso_unused.p};
    if (pso_bary.p) mesh_psos.push_back(pso_bary.p);
    for (ID3D12PipelineState* pso : mesh_psos) {
        EXPECT(rt::d3d12::pipeline_is_mesh_compute(pso));
        EXPECT(rt::d3d12::pipeline_mesh_compute_producer_built(pso) == !lazy_producers);
    }
    EXPECT(rt::d3d12::mesh_compute_producer_builds() ==
           producer_builds_at_start + (lazy_producers ? 0 : mesh_psos.size()));

    constexpr UINT size = 64;
    constexpr UINT small_extent = 16;
    constexpr UINT row_pitch = size * 4;
    constexpr UINT small_pitch = 256;  // 16 pixels at 4 bytes, padded to D3D12's 256-byte pitch
    const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
    const D3D12_HEAP_PROPERTIES upload_heap{D3D12_HEAP_TYPE_UPLOAD};
    const D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK};
    Ref<ID3D12Resource> upload, readback_a, readback_b, readback_c, readback_d, readback_e, readback_f,
        readback_depth;
    Ref<ID3D12Resource> target_a, target_b, target_c, target_d, target_e, target_f, depth;
    const auto upload_desc = buffer_desc(4096);
    EXPECT(SUCCEEDED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(&upload.p))));
    const auto make_readback = [&](UINT bytes, Ref<ID3D12Resource>& out) {
        const auto desc = buffer_desc(bytes);
        return SUCCEEDED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&out.p)));
    };
    EXPECT(make_readback(size * row_pitch, readback_a));
    EXPECT(make_readback(size * row_pitch, readback_b));
    EXPECT(make_readback(small_extent * small_pitch, readback_c));
    EXPECT(make_readback(size * row_pitch, readback_d));
    EXPECT(make_readback(size * row_pitch, readback_e));
    EXPECT(make_readback(size * row_pitch, readback_f));
    const auto make_target = [&](UINT side, Ref<ID3D12Resource>& out) {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = desc.Height = side;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        return SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                         D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                         IID_PPV_ARGS(&out.p)));
    };
    EXPECT(make_target(size, target_a));
    EXPECT(make_target(size, target_b));
    EXPECT(make_target(small_extent, target_c));
    EXPECT(make_target(size, target_d));
    EXPECT(make_target(size, target_e));
    EXPECT(make_target(size, target_f));
    D3D12_RESOURCE_DESC depth_desc{};
    depth_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depth_desc.Width = depth_desc.Height = size;
    depth_desc.DepthOrArraySize = 1;
    depth_desc.MipLevels = 1;
    depth_desc.Format = DXGI_FORMAT_D32_FLOAT;
    depth_desc.SampleDesc.Count = 1;
    depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE depth_clear{};
    depth_clear.Format = DXGI_FORMAT_D32_FLOAT;
    depth_clear.DepthStencil.Depth = 1.0f;
    EXPECT(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &depth_desc,
                                                     D3D12_RESOURCE_STATE_DEPTH_WRITE, &depth_clear,
                                                     IID_PPV_ARGS(&depth.p))));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT depth_foot{};
    UINT depth_pitch = 0;
    UINT64 depth_total = 0;
    device->GetCopyableFootprints(&depth_desc, 0, 1, 0, &depth_foot, nullptr, nullptr, &depth_total);
    EXPECT(depth_total > 0 && depth_foot.Footprint.RowPitch > 0);
    depth_pitch = depth_foot.Footprint.RowPitch;
    EXPECT(make_readback(UINT(depth_total), readback_depth));

    // Tints at 0 (grid B) and 256 (clip edge); CBVs are 256-byte aligned. The
    // sentinel dwords at 768 must still be there after every draw: mesh scratch
    // is its own allocation, and no draw writes the upload heap.
    const float tint_b[3] = {1.0f, 0.25f, 0.75f};
    const float tint_clip[3] = {0.5f, 0.75f, 0.5f};
    std::uint8_t* up = nullptr;
    EXPECT(SUCCEEDED(upload->Map(0, nullptr, reinterpret_cast<void**>(&up))));
    std::memcpy(up, tint_b, sizeof(tint_b));
    std::memcpy(up + 256, tint_clip, sizeof(tint_clip));
    std::memset(up + 768, 0xA5, 256);
    const D3D12_GPU_VIRTUAL_ADDRESS upload_va = upload->GetGPUVirtualAddress();

    const auto make_rtv = [&](ID3D12Resource* target, Ref<ID3D12DescriptorHeap>& heap) {
        const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1,
                                                   D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap.p)))) return false;
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv{
            SIZE_T(reinterpret_cast<XboxHeapHandleFn>(slot(heap.p, 9))(heap.p))};
        device->CreateRenderTargetView(target, nullptr, rtv);
        return true;
    };
    Ref<ID3D12DescriptorHeap> rtv_heap_a, rtv_heap_b, rtv_heap_c, rtv_heap_d, rtv_heap_e, rtv_heap_f,
        dsv_heap;
    EXPECT(make_rtv(target_a.p, rtv_heap_a));
    EXPECT(make_rtv(target_b.p, rtv_heap_b));
    EXPECT(make_rtv(target_c.p, rtv_heap_c));
    EXPECT(make_rtv(target_d.p, rtv_heap_d));
    EXPECT(make_rtv(target_e.p, rtv_heap_e));
    EXPECT(make_rtv(target_f.p, rtv_heap_f));
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv_a{
        SIZE_T(reinterpret_cast<XboxHeapHandleFn>(slot(rtv_heap_a.p, 9))(rtv_heap_a.p))};
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv_b{
        SIZE_T(reinterpret_cast<XboxHeapHandleFn>(slot(rtv_heap_b.p, 9))(rtv_heap_b.p))};
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv_c{
        SIZE_T(reinterpret_cast<XboxHeapHandleFn>(slot(rtv_heap_c.p, 9))(rtv_heap_c.p))};
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv_d{
        SIZE_T(reinterpret_cast<XboxHeapHandleFn>(slot(rtv_heap_d.p, 9))(rtv_heap_d.p))};
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv_e{
        SIZE_T(reinterpret_cast<XboxHeapHandleFn>(slot(rtv_heap_e.p, 9))(rtv_heap_e.p))};
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv_f{
        SIZE_T(reinterpret_cast<XboxHeapHandleFn>(slot(rtv_heap_f.p, 9))(rtv_heap_f.p))};
    {
        const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1,
                                                   D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&dsv_heap.p))));
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv{
        SIZE_T(reinterpret_cast<XboxHeapHandleFn>(slot(dsv_heap.p, 9))(dsv_heap.p))};
    device->CreateDepthStencilView(depth.p, nullptr, dsv);

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    Ref<ID3D12CommandQueue> queue;
    Ref<ID3D12CommandAllocator> allocator;
    Ref<ID3D12GraphicsCommandList> list;
    Ref<ID3D12GraphicsCommandList6> list6;
    Ref<ID3D12Fence> fence;
    EXPECT(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))));
    EXPECT(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
    EXPECT(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, pso_a.p,
                                               IID_PPV_ARGS(&list.p))));
    EXPECT(SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&list6.p))));
    EXPECT(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));

    const D3D12_VIEWPORT viewport_full{0, 0, float(size), float(size), 0, 1};
    const D3D12_RECT scissor_full{0, 0, LONG(size), LONG(size)};
    const D3D12_VIEWPORT viewport_quad{float(size / 2), 0, float(size / 2), float(size / 2), 0, 1};
    const D3D12_RECT scissor_quad{LONG(size / 2), 0, LONG(size), LONG(size / 2)};
    const std::uint32_t roots_a[2] = {1, 0x5EED};  // k = 1 for the fold shader
    const std::uint32_t roots_b[2] = {7, 11};
    // One submission, three grids plus a zero no-op, each with its own target,
    // roots and raster state: the draws share nothing but the submission.
    {
        const FLOAT blue[4] = {0, 0, 1, 1};
        const FLOAT magenta[4] = {1, 0, 1, 1};
        D3D12_RESOURCE_BARRIER to_target[] = {
            transition(target_a.p, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET),
            transition(target_b.p, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET),
            transition(target_c.p, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET),
            transition(target_d.p, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET),
            transition(target_e.p, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET),
            transition(target_f.p, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET),
        };
        list->ResourceBarrier(6, to_target);
        list->ClearRenderTargetView(rtv_a, blue, 0, nullptr);
        list->ClearRenderTargetView(rtv_b, blue, 0, nullptr);
        list->ClearRenderTargetView(rtv_c, magenta, 0, nullptr);
        list->ClearRenderTargetView(rtv_d, blue, 0, nullptr);
        list->ClearRenderTargetView(rtv_e, blue, 0, nullptr);
        list->ClearRenderTargetView(rtv_f, blue, 0, nullptr);
        list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        // Grid A: 2x3x4, full viewport, no depth.
        list->SetGraphicsRootSignature(root_a.p);
        list->RSSetViewports(1, &viewport_full);
        list->RSSetScissorRects(1, &scissor_full);
        list->OMSetRenderTargets(1, &rtv_a, FALSE, nullptr);
        list->SetPipelineState(pso_a.p);
        list->SetGraphicsRoot32BitConstants(0, 2, roots_a, 0);
        list6->DispatchMesh(2, 3, 4);
        // Grid B: 2x2x1, quadrant viewport/scissor, depth on, tinted gradient.
        list->SetGraphicsRootSignature(root_b.p);
        list->RSSetViewports(1, &viewport_quad);
        list->RSSetScissorRects(1, &scissor_quad);
        list->OMSetRenderTargets(1, &rtv_b, FALSE, &dsv);
        list->SetPipelineState(pso_b.p);
        list->SetGraphicsRoot32BitConstants(0, 2, roots_b, 0);
        list->SetGraphicsRootConstantBufferView(1, upload_va);
        list6->DispatchMesh(2, 2, 1);
        // The clip edge: 1x1x1, full viewport, second tint.
        list->SetGraphicsRootSignature(root_b.p);
        list->RSSetViewports(1, &viewport_full);
        list->RSSetScissorRects(1, &scissor_full);
        list->OMSetRenderTargets(1, &rtv_d, FALSE, nullptr);
        list->SetPipelineState(pso_clip.p);
        list->SetGraphicsRoot32BitConstants(0, 2, roots_b, 0);
        list->SetGraphicsRootConstantBufferView(1, upload_va + 256);
        list6->DispatchMesh(1, 1, 1);
        // The simple f16 case: 1x1x1, full viewport, decoded halves (u, v, u) plus f32 v.
        list->SetGraphicsRootSignature(root_a.p);
        list->RSSetViewports(1, &viewport_full);
        list->RSSetScissorRects(1, &scissor_full);
        list->OMSetRenderTargets(1, &rtv_e, FALSE, nullptr);
        list->SetPipelineState(pso_f16.p);
        list->SetGraphicsRoot32BitConstants(0, 2, roots_a, 0);
        list6->DispatchMesh(1, 1, 1);
        // Native barycentrics where capable: 1x1x1, full viewport. On Vega the PSO
        // was refused above and this draw is skipped (no silent success).
        if (pso_bary.p) {
            list->SetGraphicsRootSignature(root_a.p);
            list->RSSetViewports(1, &viewport_full);
            list->RSSetScissorRects(1, &scissor_full);
            list->OMSetRenderTargets(1, &rtv_f, FALSE, nullptr);
            list->SetPipelineState(pso_bary.p);
            list->SetGraphicsRoot32BitConstants(0, 2, roots_a, 0);
            list6->DispatchMesh(1, 1, 1);
        }
        // A zero Z grid records a Draw the replay must run as a no-op.
        list->SetGraphicsRootSignature(root_a.p);
        list->RSSetViewports(1, &viewport_full);
        list->RSSetScissorRects(1, &scissor_full);
        list->OMSetRenderTargets(1, &rtv_c, FALSE, nullptr);
        list->SetPipelineState(pso_a.p);
        list->SetGraphicsRoot32BitConstants(0, 2, roots_a, 0);
        list6->DispatchMesh(1, 1, 0);

        D3D12_RESOURCE_BARRIER to_copy[] = {
            transition(target_a.p, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition(target_b.p, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition(target_c.p, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition(target_d.p, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition(target_e.p, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition(target_f.p, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition(depth.p, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE),
        };
        list->ResourceBarrier(7, to_copy);
        const auto copy_target = [&](ID3D12Resource* target, ID3D12Resource* readback, UINT side, UINT pitch) {
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = readback;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = {0, {DXGI_FORMAT_R8G8B8A8_UNORM, side, side, 1, pitch}};
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = target;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        };
        copy_target(target_a.p, readback_a.p, size, row_pitch);
        copy_target(target_b.p, readback_b.p, size, row_pitch);
        copy_target(target_c.p, readback_c.p, small_extent, small_pitch);
        copy_target(target_d.p, readback_d.p, size, row_pitch);
        copy_target(target_e.p, readback_e.p, size, row_pitch);
        copy_target(target_f.p, readback_f.p, size, row_pitch);
        {
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = readback_depth.p;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = depth_foot;
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = depth.p;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        EXPECT(list->Close() == S_OK);
        ID3D12CommandList* lists[] = {list.p};
        queue->ExecuteCommandLists(1, lists);
        EXPECT(rt::d3d12::last_execute_result(queue.p) == leonida::ReplayResult::success);
        EXPECT(SUCCEEDED(queue->Signal(fence.p, 1)) && SUCCEEDED(fence->SetEventOnCompletion(1, nullptr)));
    }
    // The first submission drew pso_a, pso_b, pso_clip, pso_f16 and (where capable) pso_bary; each of those has its
    // producer now, exactly one each, and (lazy) the PSOs it did not draw still have none. The pixel checks below
    // prove those draws rendered.
    {
        std::vector<ID3D12PipelineState*> drawn{pso_a.p, pso_b.p, pso_clip.p, pso_f16.p};
        if (pso_bary.p) drawn.push_back(pso_bary.p);
        for (ID3D12PipelineState* pso : drawn) EXPECT(rt::d3d12::pipeline_mesh_compute_producer_built(pso));
        if (lazy_producers) {
            EXPECT(!rt::d3d12::pipeline_mesh_compute_producer_built(pso_back.p));
            EXPECT(!rt::d3d12::pipeline_mesh_compute_producer_built(pso_front.p));
            EXPECT(!rt::d3d12::pipeline_mesh_compute_producer_built(pso_unused.p));
        }
        EXPECT(rt::d3d12::mesh_compute_producer_builds() ==
               producer_builds_at_start + (lazy_producers ? drawn.size() : mesh_psos.size()));
    }
    std::vector<std::uint8_t> pixels_a(size * row_pitch), pixels_b(size * row_pitch);
    std::vector<std::uint8_t> pixels_c(small_extent * small_pitch), pixels_d(size * row_pitch);
    std::vector<std::uint8_t> pixels_e(size * row_pitch);
    std::vector<std::uint8_t> pixels_f(size * row_pitch);
    std::vector<std::uint8_t> pixels_depth(static_cast<std::size_t>(depth_total));
    {
        void* mapped = nullptr;
        EXPECT(SUCCEEDED(readback_a->Map(0, nullptr, &mapped)));
        std::memcpy(pixels_a.data(), mapped, pixels_a.size());
        readback_a->Unmap(0, nullptr);
        EXPECT(SUCCEEDED(readback_b->Map(0, nullptr, &mapped)));
        std::memcpy(pixels_b.data(), mapped, pixels_b.size());
        readback_b->Unmap(0, nullptr);
        EXPECT(SUCCEEDED(readback_c->Map(0, nullptr, &mapped)));
        std::memcpy(pixels_c.data(), mapped, pixels_c.size());
        readback_c->Unmap(0, nullptr);
        EXPECT(SUCCEEDED(readback_d->Map(0, nullptr, &mapped)));
        std::memcpy(pixels_d.data(), mapped, pixels_d.size());
        readback_d->Unmap(0, nullptr);
        EXPECT(SUCCEEDED(readback_e->Map(0, nullptr, &mapped)));
        std::memcpy(pixels_e.data(), mapped, pixels_e.size());
        readback_e->Unmap(0, nullptr);
        EXPECT(SUCCEEDED(readback_f->Map(0, nullptr, &mapped)));
        std::memcpy(pixels_f.data(), mapped, pixels_f.size());
        readback_f->Unmap(0, nullptr);
        EXPECT(SUCCEEDED(readback_depth->Map(0, nullptr, &mapped)));
        std::memcpy(pixels_depth.data(), mapped, pixels_depth.size());
        readback_depth->Unmap(0, nullptr);
    }

    // Zero X or Y grids record nothing at all.
    {
        EXPECT(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.p, pso_a.p)));
        list->SetGraphicsRootSignature(root_a.p);
        list->RSSetViewports(1, &viewport_full);
        list->SetGraphicsRoot32BitConstants(0, 2, roots_a, 0);
        list6->DispatchMesh(0, 2, 3);
        list6->DispatchMesh(2, 0, 3);
        EXPECT(list->Close() == S_OK);
        std::vector<leonida::Command> commands;
        EXPECT(rt::d3d12::recorded_commands(list.p, commands));
        for (const auto& command : commands) EXPECT(std::get_if<leonida::Draw>(&command) == nullptr);
    }
    // No indirect mesh in this branch: the stock signature refusal above leaves no
    // signature object, so ExecuteIndirect mesh is untestable here by design.
    // The wrong kind of draw for the pipeline fails at execution.
    {
        ID3D12CommandList* lists[] = {list.p};
        for (const bool vertex_draw : {true, false}) {
            EXPECT(SUCCEEDED(allocator->Reset()) &&
                   SUCCEEDED(list->Reset(allocator.p, vertex_draw ? pso_a.p : vs_pso.p)));
            list->SetGraphicsRootSignature(root_b.p);
            list->RSSetViewports(1, &viewport_full);
            list->RSSetScissorRects(1, &scissor_full);
            list->OMSetRenderTargets(1, &rtv_a, FALSE, nullptr);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            if (vertex_draw) list->DrawInstanced(3, 1, 0, 0);
            else list6->DispatchMesh(1, 1, 1);
            EXPECT(list->Close() == S_OK);
            queue->ExecuteCommandLists(1, lists);
            EXPECT(rt::d3d12::last_execute_result(queue.p) == leonida::ReplayResult::invalid_draw);
        }
        // A grid beyond D3D12's limits (65535 per count, 2^22 groups) is a recording error.
        EXPECT(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.p, pso_a.p)));
        list->SetGraphicsRootSignature(root_a.p);
        list->RSSetViewports(1, &viewport_full);
        list6->DispatchMesh(65536, 1, 1);
        EXPECT(list->Close() == E_INVALIDARG);
        EXPECT(SUCCEEDED(list->Reset(allocator.p, pso_a.p)));
        list->SetGraphicsRootSignature(root_a.p);
        list->RSSetViewports(1, &viewport_full);
        list6->DispatchMesh(4096, 1025, 1);
        EXPECT(list->Close() == E_INVALIDARG);
    }
    // Grid A again, alone in a second submission: scratch reuse is clean.
    {
        EXPECT(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.p, pso_a.p)));
        D3D12_RESOURCE_BARRIER again =
            transition(target_a.p, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ResourceBarrier(1, &again);
        const FLOAT blue[4] = {0, 0, 1, 1};
        list->ClearRenderTargetView(rtv_a, blue, 0, nullptr);
        list->SetGraphicsRootSignature(root_a.p);
        list->RSSetViewports(1, &viewport_full);
        list->RSSetScissorRects(1, &scissor_full);
        list->OMSetRenderTargets(1, &rtv_a, FALSE, nullptr);
        list->SetGraphicsRoot32BitConstants(0, 2, roots_a, 0);
        list6->DispatchMesh(2, 3, 4);
        D3D12_RESOURCE_BARRIER to_copy =
            transition(target_a.p, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &to_copy);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = readback_a.p;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = {0, {DXGI_FORMAT_R8G8B8A8_UNORM, size, size, 1, row_pitch}};
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = target_a.p;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        EXPECT(list->Close() == S_OK);
        ID3D12CommandList* lists[] = {list.p};
        queue->ExecuteCommandLists(1, lists);
        EXPECT(rt::d3d12::last_execute_result(queue.p) == leonida::ReplayResult::success);
        EXPECT(SUCCEEDED(queue->Signal(fence.p, 2)) && SUCCEEDED(fence->SetEventOnCompletion(2, nullptr)));
        void* mapped = nullptr;
        EXPECT(SUCCEEDED(readback_a->Map(0, nullptr, &mapped)));
        std::memcpy(pixels_a.data(), mapped, pixels_a.size());
        readback_a->Unmap(0, nullptr);
    }

    // Cull-direction controls in a third submission: the same grid-A draw with
    // CULL_BACK must leave the target clear, with CULL_FRONT it must render.
    // The BACK result lands in readback_b (grid B's pixels are already copied
    // out), the FRONT result back in readback_a.
    std::vector<std::uint8_t> pixels_cull_back(size * row_pitch), pixels_cull_front(size * row_pitch);
    {
        EXPECT(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.p, pso_back.p)));
        D3D12_RESOURCE_BARRIER to_target =
            transition(target_a.p, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ResourceBarrier(1, &to_target);
        const FLOAT blue[4] = {0, 0, 1, 1};
        list->ClearRenderTargetView(rtv_a, blue, 0, nullptr);
        list->SetGraphicsRootSignature(root_a.p);
        list->RSSetViewports(1, &viewport_full);
        list->RSSetScissorRects(1, &scissor_full);
        list->OMSetRenderTargets(1, &rtv_a, FALSE, nullptr);
        list->SetGraphicsRoot32BitConstants(0, 2, roots_a, 0);
        list6->DispatchMesh(2, 3, 4);
        D3D12_RESOURCE_BARRIER to_copy =
            transition(target_a.p, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &to_copy);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = readback_b.p;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = {0, {DXGI_FORMAT_R8G8B8A8_UNORM, size, size, 1, row_pitch}};
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = target_a.p;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER to_target_again =
            transition(target_a.p, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ResourceBarrier(1, &to_target_again);
        list->ClearRenderTargetView(rtv_a, blue, 0, nullptr);
        list->SetPipelineState(pso_front.p);
        list->SetGraphicsRoot32BitConstants(0, 2, roots_a, 0);
        list6->DispatchMesh(2, 3, 4);
        list->ResourceBarrier(1, &to_copy);
        dst.pResource = readback_a.p;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        EXPECT(list->Close() == S_OK);
        ID3D12CommandList* lists[] = {list.p};
        queue->ExecuteCommandLists(1, lists);
        EXPECT(rt::d3d12::last_execute_result(queue.p) == leonida::ReplayResult::success);
        EXPECT(SUCCEEDED(queue->Signal(fence.p, 3)) && SUCCEEDED(fence->SetEventOnCompletion(3, nullptr)));
        void* mapped = nullptr;
        EXPECT(SUCCEEDED(readback_b->Map(0, nullptr, &mapped)));
        std::memcpy(pixels_cull_back.data(), mapped, pixels_cull_back.size());
        readback_b->Unmap(0, nullptr);
        EXPECT(SUCCEEDED(readback_a->Map(0, nullptr, &mapped)));
        std::memcpy(pixels_cull_front.data(), mapped, pixels_cull_front.size());
        readback_a->Unmap(0, nullptr);
    }

    // Readback verdicts.
    unsigned wrong_a = 0, wrong_b = 0, wrong_c = 0, wrong_d = 0, wrong_depth = 0;
    unsigned wrong_cull_back = 0, wrong_cull_front = 0, wrong_f16 = 0, wrong_bary = 0;
    const auto want_a = fold_pixel(23, 1);
    for (UINT row = 0; row < size; ++row) {
        for (UINT col = 0; col < size; ++col) {
            const std::uint8_t* got = pixels_a.data() + row * row_pitch + col * 4;
            if (std::equal(want_a.begin(), want_a.end(), got)) continue;
            if (wrong_a++ < 6)
                std::fprintf(stderr, "grid A pixel (%u, %u): got %u,%u,%u,%u want %u,%u,%u,%u\n", col, row,
                             got[0], got[1], got[2], got[3], want_a[0], want_a[1], want_a[2], want_a[3]);
        }
    }
    // Simple f16: decoded halves (u, v, u) plus f32 v, each in 0..1 across the
    // full-screen triangle. D3D maps positive clip Y toward the TOP of the target;
    // the mesh exports v=(clipY+1)/2, so v decreases with the framebuffer row.
    // One UNORM step covers f16 plus 8-bit quantization.
    for (UINT row = 0; row < size; ++row) {
        for (UINT col = 0; col < size; ++col) {
            const std::uint8_t* got = pixels_e.data() + row * row_pitch + col * 4;
            const double u = (double(col) + 0.5) / double(size);
            const double v = 1.0 - (double(row) + 0.5) / double(size);
            const double want_r = u * 255.0, want_g = v * 255.0;
            const bool ok = std::abs(double(got[0]) - want_r) <= 1.0 &&
                            std::abs(double(got[1]) - want_g) <= 1.0 &&
                            std::abs(double(got[2]) - want_r) <= 1.0 &&
                            std::abs(double(got[3]) - want_g) <= 1.0;
            if (ok) continue;
            if (wrong_f16++ < 6)
                std::fprintf(stderr, "f16 pixel (%u, %u): got %u,%u,%u,%u want %g,%g,%g,%g+-1\n",
                             col, row, got[0], got[1], got[2], got[3], want_r, want_g, want_r, want_g);
        }
    }
    // Native barycentrics where capable: every pixel must differ from the clear
    // colour (the draw executed). Pixel-exact bary is covered by the standalone
    // GPU tests; here the gate is creation (refusal without the feature) plus
    // execution (non-clear with it). On Vega this block is skipped by design.
    if (pso_bary.p) {
        for (UINT row = 0; row < size; ++row) {
            for (UINT col = 0; col < size; ++col) {
                const std::uint8_t* got = pixels_f.data() + row * row_pitch + col * 4;
                if (got[0] == 0 && got[1] == 0 && got[2] == 255 && got[3] == 255) {
                    if (wrong_bary++ < 6)
                        std::fprintf(stderr, "bary pixel (%u, %u) still clear\n", col, row);
                }
            }
        }
    }
    for (UINT row = 0; row < size; ++row) {
        for (UINT col = 0; col < size; ++col) {
            const std::uint8_t* got = pixels_cull_back.data() + row * row_pitch + col * 4;
            if (got[0] == 0 && got[1] == 0 && got[2] == 255 && got[3] == 255) continue;
            if (wrong_cull_back++ < 6)
                std::fprintf(stderr, "cull BACK pixel (%u, %u): got %u,%u,%u,%u want 0,0,255,255\n", col,
                             row, got[0], got[1], got[2], got[3]);
        }
    }
    for (UINT row = 0; row < size; ++row) {
        for (UINT col = 0; col < size; ++col) {
            const std::uint8_t* got = pixels_cull_front.data() + row * row_pitch + col * 4;
            if (std::equal(want_a.begin(), want_a.end(), got)) continue;
            if (wrong_cull_front++ < 6)
                std::fprintf(stderr, "cull FRONT pixel (%u, %u): got %u,%u,%u,%u want %u,%u,%u,%u\n", col, row,
                             got[0], got[1], got[2], got[3], want_a[0], want_a[1], want_a[2], want_a[3]);
        }
    }
    {
        const std::uint8_t green = tri_green(tint_b[1]);
        const std::uint8_t blue_ch = tri_blue(tint_b[2]);
        for (UINT row = 0; row < size; ++row) {
            for (UINT col = 0; col < size; ++col) {
                const std::uint8_t* got = pixels_b.data() + row * row_pitch + col * 4;
                const bool covered = col >= size / 2 && row < size / 2;
                bool ok = false;
                if (!covered) {
                    ok = got[0] == 0 && got[1] == 0 && got[2] == 255 && got[3] == 255;
                } else {
                    const double want_r = tri_red(col - size / 2, size / 2, tint_b[0]);
                    ok = std::abs(double(got[0]) - want_r) <= 1.0 && got[1] == green && got[2] == blue_ch &&
                         got[3] == 255;
                }
                if (ok) continue;
                if (wrong_b++ < 6)
                    std::fprintf(stderr, "grid B pixel (%u, %u): got %u,%u,%u,%u%s\n", col, row, got[0],
                                 got[1], got[2], got[3], covered ? "" : " want 0,0,255,255");
            }
        }
    }
    for (UINT row = 0; row < small_extent; ++row) {
        for (UINT col = 0; col < small_extent; ++col) {
            const std::uint8_t* got = pixels_c.data() + row * small_pitch + col * 4;
            if (got[0] == 255 && got[1] == 0 && got[2] == 255 && got[3] == 255) continue;
            if (wrong_c++ < 6)
                std::fprintf(stderr, "zero pixel (%u, %u): got %u,%u,%u,%u want 255,0,255,255\n", col, row,
                             got[0], got[1], got[2], got[3]);
        }
    }
    {
        const std::uint8_t green = tri_green(tint_clip[1]);
        const std::uint8_t blue_ch = tri_blue(tint_clip[2]);
        for (UINT row = 0; row < size; ++row) {
            for (UINT col = 0; col < size; ++col) {
                const std::uint8_t* got = pixels_d.data() + row * row_pitch + col * 4;
                const bool covered = col >= size / 2;  // clip x < 0 leaves the right half
                bool ok = false;
                if (!covered) {
                    ok = got[0] == 0 && got[1] == 0 && got[2] == 255 && got[3] == 255;
                } else {
                    const double want_r = tri_red(col, size, tint_clip[0]);
                    ok = std::abs(double(got[0]) - want_r) <= 1.0 && got[1] == green && got[2] == blue_ch &&
                         got[3] == 255;
                }
                if (ok) continue;
                if (wrong_d++ < 6)
                    std::fprintf(stderr, "clip pixel (%u, %u): got %u,%u,%u,%u%s\n", col, row, got[0],
                                 got[1], got[2], got[3], covered ? "" : " want 0,0,255,255");
            }
        }
    }
    for (UINT row = 0; row < size; ++row) {
        for (UINT col = 0; col < size; ++col) {
            float got = 0;
            std::memcpy(&got, pixels_depth.data() + row * depth_pitch + col * 4, 4);
            const bool covered = col >= size / 2 && row < size / 2;
            const bool ok = covered ? got == 0.0f : got == 1.0f;
            if (ok) continue;
            if (wrong_depth++ < 6)
                std::fprintf(stderr, "depth (%u, %u): got %g want %g\n", col, row, double(got),
                             covered ? 0.0 : 1.0);
        }
    }
    EXPECT(!wrong_a && !wrong_b && !wrong_c && !wrong_d && !wrong_depth && !wrong_cull_back && !wrong_cull_front &&
           !wrong_f16 && !wrong_bary);
    // Every submission is done: the cull controls drew pso_back and pso_front, so only pso_unused (never recorded)
    // is without a producer under lazy, and no PSO compiled a second one.
    {
        const bool unused_built = rt::d3d12::pipeline_mesh_compute_producer_built(pso_unused.p);
        EXPECT(unused_built == !lazy_producers);
        for (ID3D12PipelineState* pso : mesh_psos)
            if (pso != pso_unused.p) EXPECT(rt::d3d12::pipeline_mesh_compute_producer_built(pso));
        const std::uint64_t built = rt::d3d12::mesh_compute_producer_builds() - producer_builds_at_start;
        EXPECT(built == mesh_psos.size() - (lazy_producers ? 1 : 0));
        std::fprintf(stderr, "mesh-compute producers: mode=%s psos=%zu drawn=%zu built=%llu unused_built=%d\n",
                     lazy_producers ? "lazy" : "eager", mesh_psos.size(), mesh_psos.size() - 1,
                     static_cast<unsigned long long>(built), unused_built ? 1 : 0);
    }
    for (UINT i = 0; i < 256; ++i) EXPECT(up[768 + i] == 0xA5);
    EXPECT(std::memcmp(up, tint_b, sizeof(tint_b)) == 0);
    EXPECT(std::memcmp(up + 256, tint_clip, sizeof(tint_clip)) == 0);

    const bool bary_drew = pso_bary.p != nullptr;
    for (auto* r : {&upload, &readback_a, &readback_b, &readback_c, &readback_d, &readback_e, &readback_f,
                    &readback_depth, &target_a, &target_b, &target_c, &target_d, &target_e, &target_f, &depth})
        r->reset();
    for (auto* p : {&pso_a, &pso_b, &pso_clip, &pso_back, &pso_front, &pso_f16, &pso_bary, &pso_unused, &vs_pso,
                    &refused})
        p->reset();
    root_a.reset();
    root_b.reset();
    rtv_heap_a.reset();
    rtv_heap_b.reset();
    rtv_heap_c.reset();
    rtv_heap_d.reset();
    rtv_heap_e.reset();
    rtv_heap_f.reset();
    dsv_heap.reset();
    list6.reset();
    list.reset();
    allocator.reset();
    queue.reset();
    fence.reset();
    EXPECT(rt::d3d12_xs::device_proxy_ref_count(device.p) == 1);
    device.reset();
    // The staged library goes away with `scratch` (per-process folder, so eager/lazy copies never share it).

    if (d.validation_errors()) {
        std::fprintf(stderr, "%u validation errors\n", d.validation_errors());
        return 1;
    }
    std::printf("mesh compute: 2x3x4 group pattern, 2x2x1 gradient with depth, the clip edge and the "
                "simple f16 case matched (%u+%u+%u+%u+%u pixels)%s; zero grids are no-ops; cull BACK "
                "stays clear and cull FRONT renders (%u+%u pixels); native mesh refused, DISPATCH_MESH "
                "signature refused (stock); over-limit and native-bary negatives %s; "
                "MS+GS/MS+VS/truncated and wrong-kind draws refused\n",
                size * size, size * size, small_extent * small_extent, size * size, size * size,
                bary_drew ? " plus native bary executed" : ", native bary refused without the feature",
                size * size, size * size,
                barycentric_capable ? "over-limit asserted, bary executed" : "both asserted E_NOTIMPL");
    return 0;
}
