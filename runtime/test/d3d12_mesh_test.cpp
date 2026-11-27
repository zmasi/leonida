// D3D-29: mesh-shader PSOs from pipeline state streams and DispatchMesh, through the Xbox-ABI proxy over the Vulkan
// device (docs/d3d12_vulkan.md section 5, "Pipelines").
//   - A device without VK_EXT_mesh_shader reports D3D12_MESH_SHADER_TIER_NOT_SUPPORTED and refuses a mesh PSO with
//     E_NOTIMPL; a terminal-only DISPATCH_MESH signature is still created there. That part runs on every GPU.
//     The rest needs the extension (exit 77 without it).
//   - With it: MeshShaderTier 1; CreatePipelineState builds an MS + PS PSO and a VS + PS PSO from streams laid out as
//     d3dx12.h lays them out (each subobject a type and its description, pointer-aligned); streams the PC runtime
//     refuses (MS with VS, CS with PS, a subobject twice or in two versions, an unknown type, a truncated stream) are E_INVALIDARG, and
//     state not built yet (an amplification shader) is E_NOTIMPL; a depth-bounds test builds where the device
//     reports DepthBoundsTestSupported, and is E_NOTIMPL where it does not.
//   - Draws: DispatchMesh(2, 2, 1) with root constants (0, 0) and a red tint fills the top-left quadrant;
//     DispatchMesh(1, 1, 2) moved by (1, -1) with a green tint fills the top half of the bottom-right one, so the
//     Y and Z counts reach the shader; the stream's VS PSO draws a full-screen triangle through a viewport on the
//     bottom-left quadrant (yellow). Each pixel is read back and compared.
//   - A vertex draw with the mesh PSO, and DispatchMesh with the VS one, fail at execution (ReplayResult::
//     invalid_draw); a mesh grid beyond D3D12's limits fails at Close; an empty grid records nothing.
//   - With the extension, ExecuteIndirect with a DISPATCH_MESH signature draws as DispatchMesh (one record
//     {2, 1, 1} fills two cells), and without graphics state it records nothing and closes. Without it, the
//     terminal description is still created; executing it fails Close (d3d12_command_signature_mesh_test).
// The shaders are GLSL written to the translator's contract (test/glsl/ms_draw.mesh, ps_draw.frag, vs_fullscreen.vert).
// No game data. Set LEONIDA_VK_VALIDATION=1 to run under VK_LAYER_KHRONOS_validation.
#include "vk_test_device.h"
#include "d3d12_test_shaders.h"
#include "../shims/d3d12_com.h"
#include "../shims/d3d12_xs_device.h"
#include "../shims/d3d12_xs_pso_stream.h"
#include "../src/shader_library.h"

#include <string_view>
#include <array>
#include <cstring>

#if !defined(LEONIDA_TEST_MS_SPV) || !defined(LEONIDA_TEST_PS_SPV) || !defined(LEONIDA_TEST_VS_FULLSCREEN_SPV)
#error "LEONIDA_TEST_MS_SPV, LEONIDA_TEST_PS_SPV and LEONIDA_TEST_VS_FULLSCREEN_SPV must name the compiled test shaders"
#endif

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

using namespace d3d12test;
using XboxHeapHandleFn = std::uint64_t(WINAPI*)(void* heap);  // heap slot 9: handle in RAX

namespace {

// One pipeline state stream subobject as d3dx12.h declares it (CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT).
template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename Inner>
struct alignas(void*) Subobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
    Inner inner;
};
using RootSubobject = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature*>;
using VsSubobject = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE>;
using PsSubobject = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE>;
using CsSubobject = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS, D3D12_SHADER_BYTECODE>;
using AsSubobject = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, D3D12_SHADER_BYTECODE>;
using MsSubobject = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE>;
using TopologySubobject = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE>;
using TargetsSubobject = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY>;
using DepthStencil1Subobject = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1, D3D12_DEPTH_STENCIL_DESC1>;
using DepthStencilSubobject = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC>;
using RasterizerSubobject = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC>;
using Rasterizer2Subobject = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2, D3D12_RASTERIZER_DESC2>;
using MaskSubobject = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT>;

// Subobjects appended one after another: each is pointer-aligned and a multiple of the pointer size, so the bytes
// are what a struct of the same members would hold.
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
    D3D12_PIPELINE_STATE_STREAM_DESC desc(std::size_t trim = 0) { return {bytes.size() - trim, bytes.data()}; }
};

D3D12_RT_FORMAT_ARRAY one_target() {
    D3D12_RT_FORMAT_ARRAY formats{};
    formats.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    formats.NumRenderTargets = 1;
    return formats;
}

HRESULT create(ID3D12Device* device, Stream& stream, Ref<ID3D12PipelineState>& out, std::size_t trim = 0) {
    Ref<ID3D12Device2> device2;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device2.p)))) return E_NOINTERFACE;
    const D3D12_PIPELINE_STATE_STREAM_DESC desc = stream.desc(trim);
    out.reset();
    return device2->CreatePipelineState(&desc, IID_PPV_ARGS(&out.p));
}

D3D12_MESH_SHADER_TIER mesh_tier(ID3D12Device* device) {
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 options{};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options, sizeof(options))))
        return D3D12_MESH_SHADER_TIER(-1);
    return options.MeshShaderTier;
}

}  // namespace

int main(int argc, char** argv) {
    const bool pc_only = argc == 2 && std::string_view(argv[1]) == "--pc-stream-only";
    EXPECT(argc == 1 || pc_only);
    EXPECT(SetEnvironmentVariableW(L"LEONIDA_XS_STREAM_ENUM", pc_only ? L"0" : L"1"));
    vktest::Device d;
    d.request_mesh_shader = true;
    const auto device_status = d.initialize();
    if (device_status == vktest::DeviceStatus::unavailable) {
        std::fprintf(stderr, "SKIP: no usable Vulkan 1.3 device\n");
        return 77;
    }
    EXPECT(device_status == vktest::DeviceStatus::ready);

    const auto ms_program = fssm_program({0x4d530001, 0xbf810000});
    // The same mesh shader listed without "root_window": one translated before the graphics root window (D3D-448).
    const auto ms_old_program = fssm_program({0x4d530002, 0xbf810000});
    const auto ps_program = fssm_program({0x50530001, 0xbf810000});
    const auto vs_program = fssm_program({0x56530002, 0xbf810000});
    auto ms_container = shader_container(ms_program);
    auto ms_old_container = shader_container(ms_old_program);
    const auto ps_container = shader_container(ps_program);
    // The synthetic container has one part at 36, followed by its eight-byte header.
    ms_container[36 + 8 + 2] = 13;  // DXIL mesh shader kind
    ms_old_container[36 + 8 + 2] = 13;
    const auto vs_container = shader_container(vs_program);
    const auto library = std::filesystem::temp_directory_path() / "leonida_mesh_test";
    std::filesystem::create_directories(library);
    std::filesystem::copy_file(LEONIDA_TEST_MS_SPV, library / "ms_draw.spv", std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(LEONIDA_TEST_MS_SPV, library / "ms_old.spv", std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(LEONIDA_TEST_PS_SPV, library / "ps_draw.spv", std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(LEONIDA_TEST_VS_FULLSCREEN_SPV, library / "vs_fullscreen.spv",
                               std::filesystem::copy_options::overwrite_existing);
    {
        std::ofstream index(library / "index.json", std::ios::binary);
        index << "{\"ms_draw\": {\"stage\": \"ms\", \"fssm_sha256\": \"" << leonida::sha256_hex(ms_program)
              << "\", \"bindings\": [], \"push_constant_bytes\": 144, \"local_size\": [32, 1, 1], "
                 "\"max_vertices\": 4, \"max_primitives\": 2, \"root_window\": {\"sgpr\": 16, \"dword\": 8}},\n"
                 " \"ms_old\": {\"stage\": \"ms\", \"fssm_sha256\": \"" << leonida::sha256_hex(ms_old_program)
              << "\", \"bindings\": [], \"push_constant_bytes\": 144, \"local_size\": [32, 1, 1], "
                 "\"max_vertices\": 4, \"max_primitives\": 2},\n"
                 " \"vs_fullscreen\": {\"stage\": \"vs\", \"fssm_sha256\": \"" << leonida::sha256_hex(vs_program)
              << "\", \"bindings\": [], \"push_constant_bytes\": 144, \"input_signature\": []},\n"
                 " \"ps_draw\": {\"stage\": \"ps\", \"fssm_sha256\": \"" << leonida::sha256_hex(ps_program)
              << "\", \"bindings\": [], \"push_constant_bytes\": 144, \"outputs\": [0]}}";
    }

    const auto device_info = [&](bool mesh) {
        rt::d3d12::VulkanDeviceInfo info{};
        info.prepare_on_bind=d.prepare_on_bind;
        info.cold_graphics_compile=d.cold_graphics_compile;
        info.heavy_pipeline_cache=d.heavy_pipeline_cache;
        info.get_instance_proc = d.instance_proc;
        info.instance = d.instance;
        info.physical = d.physical;
        info.device = d.device;
        info.queue = d.queue;
        info.queue_family = d.family;
        info.buffer_device_address = true;
        info.shader_dir = library.string();
        info.mesh_shader = mesh;
        info.mesh_shader_queries = mesh && d.mesh_shader_queries;
        info.pipeline_statistics_query = d.pipeline_statistics_query;
        info.conservative_rasterization = d.conservative_rasterization;
        return info;
    };
    const auto make_device = [&](bool mesh, Ref<ID3D12Device>& device) {
        ID3D12Device* inner = nullptr;
        if (FAILED(rt::d3d12::create_vulkan_device(device_info(mesh), IID_PPV_ARGS(&inner)))) return false;
        if (FAILED(rt::d3d12_xs::create_device_proxy(inner, __uuidof(ID3D12Device),
                                                     reinterpret_cast<void**>(&device.p))))
            return false;
        rt::d3d12::set_device_outer(inner, device.p);
        return true;
    };
    const auto root_signature = [&](ID3D12Device* device, Ref<ID3D12RootSignature>& root) {
        // [0] two constants (push dwords 8, 9: offset), [1] a root CBV (10, 11: tint), as in d3d12_draw_test. The
        // mesh shader reads them as s16..s19 and the pixel shader as s8..s11 (the graphics root window, D3D-448).
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
    // Twenty constants: s8..s27 for a pixel shader, but past a mesh shader's 16-dword window (s16..s31). Thirty:
    // past the graphics window (24 from s8) and the compute one (28 from s4) too, as a retail start-up signature is.
    const auto wide_root_signature = [&](ID3D12Device* device, Ref<ID3D12RootSignature>& root, UINT values = 20,
                                         D3D12_ROOT_SIGNATURE_FLAGS flags = D3D12_ROOT_SIGNATURE_FLAG_NONE) {
        D3D12_ROOT_PARAMETER param{};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        param.Constants = {0, 0, values};
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
        root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
        root_desc.Desc_1_0 = {1, &param, 0, nullptr, flags};
        const auto blob = serialize_root_signature(root_desc);
        return !blob.empty() &&
               SUCCEEDED(device->CreateRootSignature(0, blob.data(), blob.size(), IID_PPV_ARGS(&root.p)));
    };
    const D3D12_SHADER_BYTECODE ms{ms_container.data(), ms_container.size()};
    const D3D12_SHADER_BYTECODE ms_old{ms_old_container.data(), ms_old_container.size()};
    const D3D12_SHADER_BYTECODE ps{ps_container.data(), ps_container.size()};
    const D3D12_SHADER_BYTECODE vs{vs_container.data(), vs_container.size()};

    // Without the extension: no tier, and a mesh PSO is refused as not implemented.
    {
        Ref<ID3D12Device> device;
        Ref<ID3D12RootSignature> root;
        EXPECT(make_device(false, device));
        EXPECT(root_signature(device.p, root));
        EXPECT(mesh_tier(device.p) == D3D12_MESH_SHADER_TIER_NOT_SUPPORTED);
        Stream stream;
        stream.add(RootSubobject{.inner = root.p}).add(MsSubobject{.inner = ms}).add(PsSubobject{.inner = ps});
        stream.add(TargetsSubobject{.inner = one_target()});
        Ref<ID3D12PipelineState> pso;
        EXPECT(create(device.p, stream, pso) == E_NOTIMPL && !pso.p);
        // Creating the terminal record description is independent of executing a mesh indirect draw.
        const D3D12_INDIRECT_ARGUMENT_DESC mesh_arg{D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH};
        const D3D12_COMMAND_SIGNATURE_DESC mesh_sig{12, 1, &mesh_arg, 0};
        Ref<ID3D12CommandSignature> sig;
        EXPECT(SUCCEEDED(device->CreateCommandSignature(&mesh_sig, nullptr, IID_PPV_ARGS(&sig.p))) && sig.p);
        leonida::IndirectKind kind{};
        UINT stride = 0;
        bool terminal_only = false;
        EXPECT(rt::d3d12::command_signature_info(sig.p, kind, stride, terminal_only) &&
               kind == leonida::IndirectKind::dispatch_mesh && stride == 12 && terminal_only);
        sig.reset();
        root.reset();
        EXPECT(rt::d3d12_xs::device_proxy_ref_count(device.p) == 1);
    }
    if (!d.mesh_shader) {
        std::filesystem::remove_all(library);
        std::printf("no VK_EXT_mesh_shader on this device: only the refusal without it was checked\n");
        return 77;
    }

    constexpr UINT size = 64;
    std::vector<std::uint8_t> pixels(size * 256);
    {
        Ref<ID3D12Device> device;
        Ref<ID3D12RootSignature> root;
        EXPECT(make_device(true, device));
        EXPECT(root_signature(device.p, root));
        EXPECT(mesh_tier(device.p) == D3D12_MESH_SHADER_TIER_1);

        // The PSOs. Every other subobject takes its default: back-face culling, depth test on (no DSV here), no
        // blending, all samples.
        Ref<ID3D12PipelineState> mesh_pso, vs_pso, refused;
        Stream mesh_stream;
        mesh_stream.add(RootSubobject{.inner = root.p}).add(MsSubobject{.inner = ms}).add(PsSubobject{.inner = ps});
        mesh_stream.add(TargetsSubobject{.inner = one_target()});
        EXPECT(SUCCEEDED(create(device.p, mesh_stream, mesh_pso)));
        Stream vs_stream;
        vs_stream.add(RootSubobject{.inner = root.p}).add(VsSubobject{.inner = vs}).add(PsSubobject{.inner = ps});
        vs_stream.add(TopologySubobject{.inner = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE});
        vs_stream.add(TargetsSubobject{.inner = one_target()});
        EXPECT(SUCCEEDED(create(device.p, vs_stream, vs_pso)));
        // Conservative rasterization in a mesh pipeline: its stream has no input assembler, so it need not name a topology
        // type. A vertex pipeline without TRIANGLE is still refused, as is a wireframe mesh one.
        {
            D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
            EXPECT(SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof options)));
            if (options.ConservativeRasterizationTier != D3D12_CONSERVATIVE_RASTERIZATION_TIER_NOT_SUPPORTED) {
                std::printf("conservative rasterization tier %d: mesh pipelines checked\n", int(options.ConservativeRasterizationTier));
                D3D12_RASTERIZER_DESC conservative{};
                conservative.FillMode = D3D12_FILL_MODE_SOLID;
                conservative.CullMode = D3D12_CULL_MODE_NONE;
                conservative.DepthClipEnable = TRUE;
                conservative.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON;
                Ref<ID3D12PipelineState> made;
                Stream conservative_mesh = mesh_stream;
                conservative_mesh.add(RasterizerSubobject{.inner = conservative});
                EXPECT(SUCCEEDED(create(device.p, conservative_mesh, made)) && made.p);
                conservative.FillMode = D3D12_FILL_MODE_WIREFRAME;
                Stream wireframe_mesh = mesh_stream;
                wireframe_mesh.add(RasterizerSubobject{.inner = conservative});
                EXPECT(create(device.p, wireframe_mesh, refused) == E_INVALIDARG && !refused.p);
                conservative.FillMode = D3D12_FILL_MODE_SOLID;
                Stream no_topology_vs;
                no_topology_vs.add(RootSubobject{.inner = root.p}).add(VsSubobject{.inner = vs}).add(PsSubobject{.inner = ps});
                no_topology_vs.add(TargetsSubobject{.inner = one_target()});
                no_topology_vs.add(RasterizerSubobject{.inner = conservative});
                EXPECT(create(device.p, no_topology_vs, refused) == E_INVALIDARG && !refused.p);
            }
        }
        // A mesh shader translated before the graphics root window would read its root arguments from the wrong
        // dwords: refused, with a log line that asks for the shaders stage again.
        {
            Stream old_stream;
            old_stream.add(RootSubobject{.inner = root.p}).add(MsSubobject{.inner = ms_old}).add(PsSubobject{.inner = ps});
            old_stream.add(TargetsSubobject{.inner = one_target()});
            EXPECT(create(device.p, old_stream, refused) == E_INVALIDARG && !refused.p);
        }

        // Streams the PC runtime refuses.
        Stream bad = mesh_stream;
        bad.add(VsSubobject{.inner = vs});  // a mesh shader and a vertex shader
        EXPECT(create(device.p, bad, refused) == E_INVALIDARG && !refused.p);
        bad = mesh_stream;
        bad.add(PsSubobject{.inner = ps});  // the same subobject twice
        EXPECT(create(device.p, bad, refused) == E_INVALIDARG && !refused.p);
        bad = mesh_stream;
        bad.add(CsSubobject{.inner = ps});  // compute with graphics stages
        EXPECT(create(device.p, bad, refused) == E_INVALIDARG && !refused.p);
        EXPECT(create(device.p, mesh_stream, refused, 4) == E_INVALIDARG && !refused.p);  // truncated
        bad = mesh_stream;
        bad.add(Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE(23), UINT>{.inner = 0});  // 23 is no subobject type
        EXPECT(create(device.p, bad, refused) == E_INVALIDARG && !refused.p);
        Stream empty;
        EXPECT(create(device.p, empty.add(MaskSubobject{.inner = UINT_MAX}), refused) == E_INVALIDARG && !refused.p);
        // Versions of one subobject count as the same subobject (D3D12_PIPELINE_STATE_STREAM_DESC): DEPTH_STENCIL with
        // DEPTH_STENCIL1 in either order, and RASTERIZER with RASTERIZER2. Each description alone is accepted.
        const D3D12_DEPTH_STENCIL_DESC depth{FALSE, D3D12_DEPTH_WRITE_MASK_ZERO, D3D12_COMPARISON_FUNC_ALWAYS};
        const D3D12_DEPTH_STENCIL_DESC1 depth1{FALSE, D3D12_DEPTH_WRITE_MASK_ZERO, D3D12_COMPARISON_FUNC_ALWAYS};
        D3D12_RASTERIZER_DESC raster{};
        raster.FillMode = D3D12_FILL_MODE_SOLID;
        raster.CullMode = D3D12_CULL_MODE_NONE;
        raster.DepthClipEnable = TRUE;  // D3D12's default; depth clamping is not built
        D3D12_RASTERIZER_DESC2 raster2{};
        raster2.FillMode = D3D12_FILL_MODE_SOLID;
        raster2.CullMode = D3D12_CULL_MODE_NONE;
        raster2.DepthClipEnable = TRUE;  // D3D12's default; depth clamping is not built
        for (int order = 0; order < 2; ++order) {
            bad = mesh_stream;
            if (order == 0) bad.add(DepthStencilSubobject{.inner = depth}).add(DepthStencil1Subobject{.inner = depth1});
            else bad.add(DepthStencil1Subobject{.inner = depth1}).add(DepthStencilSubobject{.inner = depth});
            EXPECT(create(device.p, bad, refused) == E_INVALIDARG && !refused.p);
        }
        bad = mesh_stream;
        bad.add(RasterizerSubobject{.inner = raster}).add(Rasterizer2Subobject{.inner = raster2});
        EXPECT(create(device.p, bad, refused) == E_INVALIDARG && !refused.p);
        for (int which = 0; which < 4; ++which) {
            Stream one = mesh_stream;
            if (which == 0) one.add(DepthStencilSubobject{.inner = depth});
            if (which == 1) one.add(DepthStencil1Subobject{.inner = depth1});
            if (which == 2) one.add(RasterizerSubobject{.inner = raster});
            if (which == 3) one.add(Rasterizer2Subobject{.inner = raster2});
            Ref<ID3D12PipelineState> accepted;
            const HRESULT one_hr = create(device.p, one, accepted);
            if (FAILED(one_hr)) std::fprintf(stderr, "single subobject %d: 0x%08lx\n", which, static_cast<unsigned long>(one_hr));
            EXPECT(SUCCEEDED(one_hr));
        }
        // State the layer does not build yet.
        bad = mesh_stream;
        bad.add(AsSubobject{.inner = vs});
        EXPECT(create(device.p, bad, refused) == E_NOTIMPL && !refused.p);
        bad = mesh_stream;
        D3D12_DEPTH_STENCIL_DESC1 bounds{};
        bounds.DepthBoundsTestEnable = TRUE;
        bad.add(DepthStencil1Subobject{.inner = bounds});
        {
            D3D12_FEATURE_DATA_D3D12_OPTIONS2 o2{};
            EXPECT(SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2, &o2, sizeof o2)));
            Ref<ID3D12PipelineState> with_bounds;
            const HRESULT bounds_hr = create(device.p, bad, with_bounds);
            EXPECT(o2.DepthBoundsTestSupported ? SUCCEEDED(bounds_hr) && with_bounds.p
                                               : bounds_hr == E_NOTIMPL && !with_bounds.p);
        }

        // A console stream: its numbering has no gap at 23, so its mesh shader is 24 (the PC's AS) and its
        // RASTERIZER1 26 (the PC's DEPTH_STENCIL2). An AS without an MS is no PC stream, so the layer reads it with the
        // console's numbering and makes the mesh PSO. The PC reading of the same bytes alone would refuse it.
        D3D12_RASTERIZER_DESC1 raster1{};
        raster1.FillMode = D3D12_FILL_MODE_SOLID;
        raster1.CullMode = D3D12_CULL_MODE_NONE;
        raster1.DepthClipEnable = TRUE;
        for (int with_raster1 = 0; with_raster1 < 2; ++with_raster1) {
            Stream console;
            console.add(RootSubobject{.inner = root.p});
            console.add(Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE(24), D3D12_SHADER_BYTECODE>{.inner = ms});
            console.add(PsSubobject{.inner = ps}).add(TargetsSubobject{.inner = one_target()});
            if (with_raster1)
                console.add(Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE(26), D3D12_RASTERIZER_DESC1>{.inner = raster1});
            console.add(MaskSubobject{.inner = UINT_MAX});
            Ref<ID3D12PipelineState> console_pso;
            using Create = HRESULT(WINAPI*)(ID3D12Device*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
            const auto desc = console.desc();
            const HRESULT console_hr = reinterpret_cast<Create>(slot(device.p, 86))(device.p, &desc, IID_PPV_ARGS(&console_pso.p));
            if (pc_only) {
                EXPECT(FAILED(console_hr) && !console_pso.p);
            } else {
                EXPECT(SUCCEEDED(console_hr) && console_pso.p);
                // Slot 55 must also walk raw 26 as RASTERIZER1 and retain the mesh bytecode after the base is released.
                using Derive = HRESULT(WINAPI*)(ID3D12Device*, ID3D12PipelineState*, UINT, const void*, REFIID, void**);
                std::array<std::uint8_t, rt::d3d12_xs::kDerivedRecordSize> record{};
                const std::uint32_t mask[] = {3, UINT_MAX};
                std::memcpy(record.data(), mask, sizeof(mask));
                mesh_pso.reset();
                EXPECT(SUCCEEDED(reinterpret_cast<Derive>(slot(device.p, 55))(
                    device.p, console_pso.p, 1, record.data(), IID_PPV_ARGS(&mesh_pso.p))) && mesh_pso.p);
            }
        }
        // Even a known mesh FSSM must not reinterpret type 24 if the container declares another stage.
        auto as_container = ms_container;
        as_container[36 + 8 + 2] = 14;  // amplification
        Stream as_only;
        as_only.add(RootSubobject{.inner = root.p})
               .add(AsSubobject{.inner = {as_container.data(), as_container.size()}})
               .add(PsSubobject{.inner = ps}).add(TargetsSubobject{.inner = one_target()});
        EXPECT(FAILED(create(device.p, as_only, refused)) && !refused.p);
        // An MS and an AS in PC numbering stay the PC's (and refused, the layer builds no AS).
        bad = mesh_stream;
        bad.add(Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE(24), D3D12_SHADER_BYTECODE>{.inner = ms});
        EXPECT(create(device.p, bad, refused) == E_NOTIMPL && !refused.p);
        std::printf("console-numbered mesh streams: %s\n", pc_only ? "refused in PC-only mode" : "both created");

        const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
        const D3D12_HEAP_PROPERTIES upload_heap{D3D12_HEAP_TYPE_UPLOAD};
        const D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK};
        Ref<ID3D12Resource> upload, readback, target;
        const auto upload_desc = buffer_desc(4096);
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(&upload.p))));
        const auto readback_desc = buffer_desc(size * 256);
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&readback.p))));
        D3D12_RESOURCE_DESC rt_desc{};
        rt_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rt_desc.Width = rt_desc.Height = size;
        rt_desc.DepthOrArraySize = 1;
        rt_desc.MipLevels = 1;
        rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rt_desc.SampleDesc.Count = 1;
        rt_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &rt_desc,
                                                         D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&target.p))));

        // Tints at 0 (red stays red), 256 (green) and 512 (white: yellow stays yellow); CBVs are 256-byte aligned.
        const float tints[3][3] = {{1, 0, 1}, {0, 1, 0}, {1, 1, 1}};
        std::uint8_t* up = nullptr;
        EXPECT(SUCCEEDED(upload->Map(0, nullptr, reinterpret_cast<void**>(&up))));
        for (int i = 0; i < 3; ++i) std::memcpy(up + 256 * i, tints[i], sizeof(tints[i]));
        const D3D12_GPU_VIRTUAL_ADDRESS upload_va = upload->GetGPUVirtualAddress();

        const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        Ref<ID3D12DescriptorHeap> rtv_heap;
        EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap.p))));
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv{
            SIZE_T(reinterpret_cast<XboxHeapHandleFn>(slot(rtv_heap.p, 9))(rtv_heap.p))};
        device->CreateRenderTargetView(target.p, nullptr, rtv);

        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        Ref<ID3D12CommandQueue> queue;
        Ref<ID3D12CommandAllocator> allocator;
        Ref<ID3D12GraphicsCommandList> list;
        Ref<ID3D12GraphicsCommandList6> list6;
        Ref<ID3D12Fence> fence;
        EXPECT(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))));
        EXPECT(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
        EXPECT(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, mesh_pso.p,
                                                   IID_PPV_ARGS(&list.p))));
        EXPECT(SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&list6.p))));
        EXPECT(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));

        D3D12_RESOURCE_BARRIER to_target =
            transition(target.p, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ResourceBarrier(1, &to_target);
        const FLOAT blue[4] = {0, 0, 1, 1};
        list->ClearRenderTargetView(rtv, blue, 0, nullptr);
        list->SetGraphicsRootSignature(root.p);
        const D3D12_VIEWPORT viewport{0, 0, float(size), float(size), 0, 1};
        const D3D12_RECT scissor{0, 0, LONG(size), LONG(size)};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

        // Mesh draw 1: a 2x2 grid of quads, no offset, red -> the top-left quadrant. No topology was set: a mesh
        // draw does not need one. An empty grid before it records nothing.
        const float origin[2] = {0, 0}, down_right[2] = {1, -1};
        list->SetGraphicsRoot32BitConstants(0, 2, origin, 0);
        list->SetGraphicsRootConstantBufferView(1, upload_va);
        list6->DispatchMesh(0, 1, 1);
        list6->DispatchMesh(2, 2, 1);
        // Mesh draw 2: groups (0,0,0) and (0,0,1), columns 0 and 1 of row 0, moved by (1, -1), green -> the top half
        // of the bottom-right quadrant.
        list->SetGraphicsRoot32BitConstants(0, 2, down_right, 0);
        list->SetGraphicsRootConstantBufferView(1, upload_va + 256);
        list6->DispatchMesh(1, 1, 2);
        // The stream's VS PSO: a full-screen triangle through a viewport on the bottom-left quadrant, yellow.
        const D3D12_VIEWPORT bottom_left{0, float(size / 2), float(size / 2), float(size / 2), 0, 1};
        list->RSSetViewports(1, &bottom_left);
        list->SetPipelineState(vs_pso.p);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->SetGraphicsRootConstantBufferView(1, upload_va + 512);
        list->DrawInstanced(3, 1, 0, 0);

        D3D12_RESOURCE_BARRIER to_copy =
            transition(target.p, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &to_copy);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = readback.p;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = {0, {DXGI_FORMAT_R8G8B8A8_UNORM, size, size, 1, 256}};
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = target.p;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        EXPECT(list->Close() == S_OK);
        ID3D12CommandList* lists[] = {list.p};
        queue->ExecuteCommandLists(1, lists);
        EXPECT(rt::d3d12::last_execute_result(queue.p) == leonida::ReplayResult::success);
        EXPECT(SUCCEEDED(queue->Signal(fence.p, 1)) && SUCCEEDED(fence->SetEventOnCompletion(1, nullptr)));
        void* mapped = nullptr;
        EXPECT(SUCCEEDED(readback->Map(0, nullptr, &mapped)));
        std::memcpy(pixels.data(), mapped, pixels.size());

        // ExecuteIndirect with a DISPATCH_MESH signature draws as DispatchMesh (vkCmdDrawMeshTasksIndirectEXT after
        // the clamp pass): one record {2, 1, 1} at upload + 768 fills columns 0 and 1 of row 0, the top half of the
        // top-left quadrant, red; the rest of the cleared target stays blue.
        {
            const std::uint32_t record[3] = {2, 1, 1};
            std::memcpy(up + 768, record, sizeof record);
            const D3D12_INDIRECT_ARGUMENT_DESC mesh_arg{D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH};
            const D3D12_COMMAND_SIGNATURE_DESC mesh_sig{12, 1, &mesh_arg, 0};
            Ref<ID3D12CommandSignature> sig;
            Ref<ID3D12Fence> indirect_fence;
            EXPECT(SUCCEEDED(device->CreateCommandSignature(&mesh_sig, nullptr, IID_PPV_ARGS(&sig.p))) && sig.p);
            EXPECT(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&indirect_fence.p))));
            EXPECT(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.p, mesh_pso.p)));
            D3D12_RESOURCE_BARRIER again =
                transition(target.p, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            list->ResourceBarrier(1, &again);
            list->ClearRenderTargetView(rtv, blue, 0, nullptr);
            list->SetGraphicsRootSignature(root.p);
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            list->SetGraphicsRoot32BitConstants(0, 2, origin, 0);
            list->SetGraphicsRootConstantBufferView(1, upload_va);
            list->ExecuteIndirect(sig.p, 1, upload.p, 768, nullptr, 0);
            list->ResourceBarrier(1, &to_copy);
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            EXPECT(list->Close() == S_OK);
            queue->ExecuteCommandLists(1, lists);
            EXPECT(rt::d3d12::last_execute_result(queue.p) == leonida::ReplayResult::success);
            EXPECT(SUCCEEDED(queue->Signal(indirect_fence.p, 1)) &&
                   SUCCEEDED(indirect_fence->SetEventOnCompletion(1, nullptr)));
            unsigned wrong = 0;
            for (UINT row = 0; row < size; ++row) {
                for (UINT col = 0; col < size; ++col) {
                    const bool red = row < size / 4 && col < size / 2;
                    const std::uint8_t want[4] = {std::uint8_t(red ? 255 : 0), 0, std::uint8_t(red ? 0 : 255), 255};
                    const auto* got = static_cast<const std::uint8_t*>(mapped) + row * 256 + col * 4;
                    if (std::equal(want, want + 4, got)) continue;
                    if (wrong++ < 6)
                        std::fprintf(stderr, "indirect mesh pixel (%u, %u): got %u,%u,%u,%u want %u,%u,%u,%u\n", col,
                                     row, got[0], got[1], got[2], got[3], want[0], want[1], want[2], want[3]);
                }
            }
            EXPECT(!wrong);
            std::printf("ExecuteIndirect DISPATCH_MESH {2, 1, 1} drew as DispatchMesh(2, 1, 1): %u pixels matched\n",
                        size * size);
        }

        // The wrong kind of draw for the pipeline fails at execution: a vertex draw with the mesh PSO, then
        // DispatchMesh with the VS PSO.
        for (const bool vertex_draw : {true, false}) {
            EXPECT(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.p, vertex_draw ? mesh_pso.p
                                                                                                    : vs_pso.p)));
            list->SetGraphicsRootSignature(root.p);
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            if (vertex_draw) list->DrawInstanced(3, 1, 0, 0);
            else list6->DispatchMesh(1, 1, 1);
            EXPECT(list->Close() == S_OK);
            queue->ExecuteCommandLists(1, lists);
            EXPECT(rt::d3d12::last_execute_result(queue.p) == leonida::ReplayResult::invalid_draw);
        }
        // A grid beyond D3D12's limits (65535 per count, 2^22 groups) is a recording error.
        EXPECT(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.p, mesh_pso.p)));
        list->SetGraphicsRootSignature(root.p);
        list->RSSetViewports(1, &viewport);
        list6->DispatchMesh(65536, 1, 1);
        EXPECT(list->Close() == E_INVALIDARG);
        EXPECT(SUCCEEDED(list->Reset(allocator.p, mesh_pso.p)));
        list->SetGraphicsRootSignature(root.p);
        list->RSSetViewports(1, &viewport);
        list6->DispatchMesh(4096, 1025, 1);
        EXPECT(list->Close() == E_INVALIDARG);
        // A root signature the graphics bind point places (s8..s27) but a mesh shader's window cannot (s16..s31): a
        // vertex draw may use it, DispatchMesh may not.
        {
            Ref<ID3D12RootSignature> wide;
            EXPECT(wide_root_signature(device.p, wide));
            EXPECT(SUCCEEDED(list->Reset(allocator.p, mesh_pso.p)));
            list->SetGraphicsRootSignature(wide.p);
            list->RSSetViewports(1, &viewport);
            list6->DispatchMesh(1, 1, 1);
            EXPECT(list->Close() == E_INVALIDARG);
            // Past the console graphics window (24 dwords from s8): still created, on pack()'s s0 layout (logged).
            // Setting its 30 constants is valid; a mesh shader, which reads the console layout, may not use it.
            Ref<ID3D12RootSignature> huge;
            EXPECT(wide_root_signature(device.p, huge, 30));
            EXPECT(SUCCEEDED(list->Reset(allocator.p, mesh_pso.p)));
            list->SetGraphicsRootSignature(huge.p);
            const std::array<std::uint32_t, 30> values{};
            list->SetGraphicsRoot32BitConstants(0, 30, values.data(), 0);
            EXPECT(SUCCEEDED(list->Close()));
            EXPECT(SUCCEEDED(list->Reset(allocator.p, mesh_pso.p)));
            list->SetGraphicsRootSignature(huge.p);
            list->RSSetViewports(1, &viewport);
            list6->DispatchMesh(1, 1, 1);
            EXPECT(list->Close() == E_INVALIDARG);  // a mesh shader cannot read it
            // Four constants: the console layout, and the mesh window holds them.
            Ref<ID3D12RootSignature> console_layout;
            EXPECT(wide_root_signature(device.p, console_layout, 4));
            EXPECT(SUCCEEDED(list->Reset(allocator.p, mesh_pso.p)));
            list->SetGraphicsRootSignature(console_layout.p);
            list->RSSetViewports(1, &viewport);
            list6->DispatchMesh(1, 1, 1);
            EXPECT(SUCCEEDED(list->Close()));

            // RT-DIRECT-HEAP-GRAPHICS: with the directly-indexed flags, the dword after the graphics root arguments
            // holds the resource heap's 32-bit address and the next one the sampler heap's, for vertex and mesh draws
            // alike (a mesh shader reads them through its window). 4 constants take s8..s11, so the heaps are at 12
            // and 13; 23 constants leave room for the resource heap only (s31); a signature past the graphics window
            // (30 constants) keeps pack()'s s0 layout, which has no measured slot, so nothing is written.
            const auto direct = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(
                D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
                D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED);
            Ref<ID3D12RootSignature> direct4, direct23, direct30;
            EXPECT(wide_root_signature(device.p, direct4, 4, direct));
            EXPECT(wide_root_signature(device.p, direct23, 23, direct));
            EXPECT(wide_root_signature(device.p, direct30, 30, direct));
            D3D12_DESCRIPTOR_HEAP_DESC resources_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8,
                                                      D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
            auto samplers_desc = resources_desc;
            samplers_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
            Ref<ID3D12DescriptorHeap> resources, samplers;
            EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&resources_desc, IID_PPV_ARGS(&resources.p))));
            EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&samplers_desc, IID_PPV_ARGS(&samplers.p))));
            const std::uint64_t res_va = reinterpret_cast<XboxHeapHandleFn>(slot(resources.p, 10))(resources.p);
            const std::uint64_t smp_va = reinterpret_cast<XboxHeapHandleFn>(slot(samplers.p, 10))(samplers.p);
            EXPECT(res_va && res_va < (1ull << 32) && smp_va && smp_va < (1ull << 32));
            const std::array<std::uint32_t, 4> four{1, 2, 3, 4};
            ID3D12DescriptorHeap* both[] = {resources.p, samplers.p};
            EXPECT(SUCCEEDED(list->Reset(allocator.p, vs_pso.p)));
            list->SetDescriptorHeaps(2, both);
            list->SetGraphicsRootSignature(direct4.p);
            list->SetGraphicsRoot32BitConstants(0, 4, four.data(), 0);
            list->RSSetViewports(1, &viewport);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->DrawInstanced(3, 1, 0, 0);
            list->SetPipelineState(mesh_pso.p);
            list6->DispatchMesh(1, 1, 1);
            list->SetPipelineState(vs_pso.p);
            list->SetDescriptorHeaps(1, &resources.p);  // rebinding only the resource heap unbinds the sampler heap
            list->DrawInstanced(3, 1, 0, 0);
            list->SetDescriptorHeaps(2, both);
            list->SetGraphicsRootSignature(direct23.p);
            list->SetGraphicsRoot32BitConstant(0, 77, 22);
            list->DrawInstanced(3, 1, 0, 0);
            list->SetGraphicsRootSignature(direct30.p);
            list->DrawInstanced(3, 1, 0, 0);
            EXPECT(SUCCEEDED(list->Close()));
            std::vector<leonida::Command> commands;
            EXPECT(rt::d3d12::recorded_commands(list.p, commands));
            std::vector<const leonida::Draw*> draws;
            for (const auto& command : commands)
                if (const auto* recorded = std::get_if<leonida::Draw>(&command)) draws.push_back(recorded);
            EXPECT(draws.size() == 5);
            char setting[2]{};
            const bool enabled = GetEnvironmentVariableA("LEONIDA_GRAPHICS_DIRECT_HEAPS", setting, 2) != 1 ||
                                 setting[0] != '0';
            const auto res = enabled ? std::uint32_t(res_va) : 0u, smp = enabled ? std::uint32_t(smp_va) : 0u;
            const auto& vertex = draws[0]->user_data;
            EXPECT(vertex[8] == 1 && vertex[11] == 4 && vertex[12] == res && vertex[13] == smp);
            EXPECT(draws[1]->mesh && draws[1]->user_data == vertex);  // the mesh draw: the same dwords
            EXPECT(draws[2]->user_data[12] == res && draws[2]->user_data[13] == 0);
            EXPECT(draws[3]->user_data[30] == 77 && draws[3]->user_data[31] == res);
            for (std::uint32_t k = 0; k < 32; ++k) EXPECT(draws[4]->user_data[k] == 0);
            std::printf("direct heaps on graphics draws: resource heap at s%u, sampler heap at s%u%s\n", 12u, 13u,
                        enabled ? "" : " (LEONIDA_GRAPHICS_DIRECT_HEAPS=0: not written)");
        }
        // A DISPATCH_MESH command signature (D3D12_DISPATCH_MESH_ARGUMENTS, 12 bytes) is created; a stride below its
        // 12 bytes is refused as the PC runtime refuses it. ExecuteIndirect with it and no viewport records
        // nothing, so the list still closes.
        {
            const D3D12_INDIRECT_ARGUMENT_DESC mesh_arg{D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH};
            const D3D12_COMMAND_SIGNATURE_DESC mesh_sig{12, 1, &mesh_arg, 0};
            const D3D12_COMMAND_SIGNATURE_DESC short_sig{8, 1, &mesh_arg, 0};
            Ref<ID3D12CommandSignature> sig, short_one;
            EXPECT(SUCCEEDED(device->CreateCommandSignature(&mesh_sig, nullptr, IID_PPV_ARGS(&sig.p))) && sig.p);
            EXPECT(device->CreateCommandSignature(&short_sig, nullptr, IID_PPV_ARGS(&short_one.p)) == E_INVALIDARG);
            EXPECT(SUCCEEDED(list->Reset(allocator.p, mesh_pso.p)));
            list->SetGraphicsRootSignature(root.p);
            list->ExecuteIndirect(sig.p, 1, upload.p, 0, nullptr, 0);
            EXPECT(list->Close() == S_OK);
        }
        EXPECT(SUCCEEDED(queue->Signal(fence.p, 2)) && SUCCEEDED(fence->SetEventOnCompletion(2, nullptr)));

        // D3D-291: pipeline statistics around mesh draws. Query 0 holds a vertex draw, two mesh draws and another
        // vertex draw, which the layer splits into a full, a mesh and a full segment; query 1 holds one mesh draw;
        // query 2 the two vertex draws alone. A mesh draw has no input assembly or vertex shader, so those fields
        // count only the two full-screen triangles (3 vertices, 1 primitive each). How many clipping and pixel
        // shader invocations a draw makes is the driver's (RADV's PSInvocations is not the pixel count), so query
        // 0 is checked against query 2: more pixel shader invocations is the mesh segment's share. A mesh segment
        // runs in a pool that counts the fragment and compute invocations only, because
        // VUID-vkCmdDrawMeshTasksEXT-pipelineStatistics-07076 leaves the clipping counters out of a pool that is
        // active during a mesh draw; D3D12 counts the clipping invocations and primitives of a mesh pipeline, so
        // those two fields read 0 here. Under LEONIDA_VK_VALIDATION=1 the run must stay free of the VUID.
        // Where the device has the meshShaderQueries feature, the clipping invocations of a mesh segment come
        // from a mesh-primitives query instead (D3D-MESH-CLIPPING-COUNTER): the number of primitives the mesh
        // shader emitted that reached the fragment shader. How many that is depends on the driver, so the counts
        // are compared with each other and not written down.
        if (d.pipeline_statistics_query) {
            const D3D12_QUERY_HEAP_DESC statistics_desc{D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, 3, 0};
            Ref<ID3D12QueryHeap> statistics;
            EXPECT(SUCCEEDED(device->CreateQueryHeap(&statistics_desc, IID_PPV_ARGS(&statistics.p))));
            Ref<ID3D12Resource> results;
            const auto results_desc = buffer_desc(3 * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS));
            EXPECT(SUCCEEDED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &results_desc,
                                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                             IID_PPV_ARGS(&results.p))));
            EXPECT(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.p, mesh_pso.p)));
            D3D12_RESOURCE_BARRIER back_to_target =
                transition(target.p, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            list->ResourceBarrier(1, &back_to_target);
            list->SetGraphicsRootSignature(root.p);
            list->RSSetScissorRects(1, &scissor);
            list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            list->SetGraphicsRoot32BitConstants(0, 2, origin, 0);
            list->SetGraphicsRootConstantBufferView(1, upload_va);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            const auto vertex_draw = [&] {
                list->SetPipelineState(vs_pso.p);
                list->RSSetViewports(1, &bottom_left);
                list->DrawInstanced(3, 1, 0, 0);
            };
            const auto mesh_draw = [&](UINT x, UINT y) {
                list->SetPipelineState(mesh_pso.p);
                list->RSSetViewports(1, &viewport);
                list6->DispatchMesh(x, y, 1);
            };
            list->BeginQuery(statistics.p, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
            vertex_draw();
            mesh_draw(2, 2);
            mesh_draw(1, 1);
            vertex_draw();
            list->EndQuery(statistics.p, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
            list->BeginQuery(statistics.p, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 1);
            mesh_draw(2, 2);
            list->EndQuery(statistics.p, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 1);
            list->BeginQuery(statistics.p, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 2);
            vertex_draw();
            vertex_draw();
            list->EndQuery(statistics.p, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 2);
            list->ResolveQueryData(statistics.p, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 3, results.p, 0);
            EXPECT(list->Close() == S_OK);
            queue->ExecuteCommandLists(1, lists);
            EXPECT(rt::d3d12::last_execute_result(queue.p) == leonida::ReplayResult::success);
            EXPECT(SUCCEEDED(queue->Signal(fence.p, 3)) && SUCCEEDED(fence->SetEventOnCompletion(3, nullptr)));
            D3D12_QUERY_DATA_PIPELINE_STATISTICS data[3]{};
            void* stats_mapped = nullptr;
            EXPECT(SUCCEEDED(results->Map(0, nullptr, &stats_mapped)));
            std::memcpy(data, stats_mapped, sizeof(data));
            results->Unmap(0, nullptr);
            const auto& mixed = data[0];
            const auto& mesh_only = data[1];
            const auto& vertex_only = data[2];
            std::printf("statistics, mixed: IA %llu/%llu VS %llu C %llu/%llu PS %llu CS %llu; mesh only: C %llu/%llu "
                        "PS %llu; vertex only: C %llu PS %llu\n",
                        mixed.IAVertices, mixed.IAPrimitives, mixed.VSInvocations, mixed.CInvocations,
                        mixed.CPrimitives, mixed.PSInvocations, mixed.CSInvocations, mesh_only.CInvocations,
                        mesh_only.CPrimitives, mesh_only.PSInvocations, vertex_only.CInvocations,
                        vertex_only.PSInvocations);
            EXPECT(mixed.IAVertices == 6 && mixed.IAPrimitives == 2 && mixed.VSInvocations >= 1);
            EXPECT(vertex_only.IAVertices == 6 && vertex_only.IAPrimitives == 2);
            EXPECT(mixed.GSInvocations == 0 && mixed.GSPrimitives == 0 && mixed.HSInvocations == 0 &&
                   mixed.DSInvocations == 0 && mixed.CSInvocations == 0);
            // The clipping invocations of the mesh segments (D3D-MESH-CLIPPING-COUNTER). Without the feature they
            // read 0; with it they are the mesh-primitives count of those segments.
            const auto mesh_clipping = mixed.CInvocations - vertex_only.CInvocations;
            if (d.mesh_shader_queries) {
                // The count has to be wired up, not merely self-consistent. A layer that reported the feature but
                // never created the third pool would leave both counts at 0, and 0 >= 0 would still hold. How many
                // primitives the mesh shader emits is the driver's, so ask for a positive count and not for a
                // number: the mesh draw above fills the top-left quadrant, so it emits at least one.
                EXPECT(mesh_clipping >= mesh_only.CInvocations);
                EXPECT(mesh_only.CInvocations > 0);
            } else {
                EXPECT(mesh_clipping == 0);
            }
            EXPECT(mixed.PSInvocations > vertex_only.PSInvocations);
            EXPECT(mesh_only.IAVertices == 0 && mesh_only.IAPrimitives == 0 && mesh_only.VSInvocations == 0 &&
                   mesh_only.GSInvocations == 0 && mesh_only.GSPrimitives == 0 && mesh_only.HSInvocations == 0 &&
                   mesh_only.DSInvocations == 0 && mesh_only.CSInvocations == 0);
            // Vulkan has no clipped-primitive counter, so the clipping primitives of a mesh segment stay 0.
            EXPECT(mesh_only.CPrimitives == 0 && mesh_only.PSInvocations > 0);
            if (!d.mesh_shader_queries) EXPECT(mesh_only.CInvocations == 0);
        } else {
            std::printf("pipelineStatisticsQuery not available: the D3D-291 statistics check is skipped\n");
        }

        for (auto* r : {&upload, &readback, &target}) r->reset();
        for (auto* p : {&mesh_pso, &vs_pso, &refused}) p->reset();
        root.reset();
        rtv_heap.reset();
        list6.reset();
        list.reset();
        allocator.reset();
        queue.reset();
        fence.reset();
        EXPECT(rt::d3d12_xs::device_proxy_ref_count(device.p) == 1);
    }
    std::filesystem::remove_all(library);

    unsigned mismatches = 0;
    for (UINT row = 0; row < size; ++row) {
        for (UINT col = 0; col < size; ++col) {
            const bool top = row < size / 2, left = col < size / 2;
            using Pixel = std::array<std::uint8_t, 4>;
            const Pixel want = top && left                            ? Pixel{255, 0, 0, 255}
                               : !top && left                         ? Pixel{255, 255, 0, 255}
                               : !top && !left && row < size * 3 / 4  ? Pixel{0, 255, 0, 255}
                                                                      : Pixel{0, 0, 255, 255};
            const std::uint8_t* got = pixels.data() + row * 256 + col * 4;
            if (std::equal(want.begin(), want.end(), got)) continue;
            if (mismatches++ < 6)
                std::fprintf(stderr, "pixel (%u, %u): got %u,%u,%u,%u want %u,%u,%u,%u\n", col, row, got[0], got[1],
                             got[2], got[3], want[0], want[1], want[2], want[3]);
        }
    }
    EXPECT(!mismatches);
    if (d.validation_errors()) {
        std::fprintf(stderr, "%u validation errors\n", d.validation_errors());
        return 1;
    }
    std::printf("D3D-29: mesh tier 1; mesh and VS PSOs from pipeline state streams (9 refused as invalid, 2 as not "
                "implemented); DispatchMesh over X, Y and Z and a stream VS draw matched all %u pixels; wrong-kind "
                "draws fail at execution, oversized grids at Close; statistics queries around mesh draws resolve\n",
                size * size);
    return 0;
}
