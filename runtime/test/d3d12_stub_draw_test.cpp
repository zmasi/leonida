// LEONIDA_STUB_PIPELINES draws (MVP-GPU-04). With no shader library, a graphics PSO is created from the built-in
// stages in src/stub_pipeline.h as a real, registered Vulkan pipeline, and its draws run through replay:
//   - LEONIDA_STUB_PIPELINES=1: the vertex stage puts every vertex outside the clip volume, so a draw leaves both
//     render targets (R8G8B8A8_UNORM and R32_UINT, cleared to blue and 7) as they were;
//   - LEONIDA_STUB_PIPELINES=color: three vertices cover the viewport and the pixel stage writes leonida::stub_colour,
//     magenta to the UNORM target and 1 to the integer one (an integer output for an integer format);
//   - a point-list PSO (whose vertex stage must write PointSize) is created and drawn too;
//   - =color still covers with the D3D12 default back-face cull, wireframe fill, a zero sample mask, and a reverse-Z
//     GREATER depth test against a depth target cleared to 0: the stub's own fixed-function state replaces the title's;
//   - a draw with a PSO this device never created (another implementation's) still fails the list, stubs or not;
//   - a mesh stub's DispatchMesh records nothing on its own device's list and fails another device's list (skipped
//     where the device makes no mesh pipeline).
// The PSO keeps an input layout and a bound vertex buffer the stub stages do not read. No game data, no shader
// library. Set LEONIDA_VK_VALIDATION=1 to run under VK_LAYER_KHRONOS_validation: the draws must raise no message.
#include "vk_test_device.h"
#include "d3d12_test_shaders.h"
#include "../shims/d3d12_com.h"
#include "../shims/d3d12_xs_device.h"
#include "../src/shader_library.h"
#include "../src/stub_pipeline.h"

#include <cstdlib>
#include <cstring>

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

using namespace d3d12test;
using XboxHeapHandleFn = std::uint64_t(WINAPI*)(void* heap);  // heap slot 9: handle in RAX

namespace {

D3D12_CPU_DESCRIPTOR_HANDLE heap_start(ID3D12DescriptorHeap* heap) {
    return {SIZE_T(reinterpret_cast<XboxHeapHandleFn>(slot(heap, 9))(heap))};
}

constexpr UINT size = 64;
constexpr UINT pitch = 256;  // size * 4 bytes, already D3D12_TEXTURE_DATA_PITCH_ALIGNMENT

// Every pixel of both targets: RGBA8 `colour` and R32_UINT `value`.
bool targets_hold(const std::uint8_t* rgba, const std::uint8_t* uints, const std::uint8_t (&colour)[4],
                  std::uint32_t value, const char* what) {
    unsigned bad = 0;
    for (UINT row = 0; row < size; ++row) {
        for (UINT col = 0; col < size; ++col) {
            const std::uint8_t* got = rgba + row * pitch + col * 4;
            std::uint32_t word;
            std::memcpy(&word, uints + row * pitch + col * 4, 4);
            if (std::memcmp(got, colour, 4) == 0 && word == value) continue;
            if (bad++ < 4)
                std::fprintf(stderr, "%s: pixel (%u, %u) got %u,%u,%u,%u / %u, want %u,%u,%u,%u / %u\n", what, col, row,
                             got[0], got[1], got[2], got[3], word, colour[0], colour[1], colour[2], colour[3], value);
        }
    }
    return bad == 0;
}

// One pipeline state stream subobject as d3dx12.h declares it (CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT), and a stream
// of them appended one after another (each is pointer-aligned and a multiple of the pointer size).
template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename Inner>
struct alignas(void*) Subobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
    Inner inner;
};
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
};

// A pipeline state from another implementation: the Vulkan device does not know it (it answers none of its tags).
// It lives on the stack and outlives the list that holds a reference to it.
class ForeignPipelineState final : public ID3D12PipelineState {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
            riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12PipelineState)) {
            *out = static_cast<ID3D12PipelineState*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs_; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return DXGI_ERROR_NOT_FOUND; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void** device) override {
        if (device) *device = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetCachedBlob(ID3DBlob** blob) override {
        if (blob) *blob = nullptr;
        return E_NOTIMPL;
    }

private:
    ULONG refs_ = 1;
};

}  // namespace

int main() {
    vktest::Device d;
    d.request_mesh_shader = true;  // for the mesh stub cases, where the GPU has VK_EXT_mesh_shader
    const auto device_status = d.initialize();
    if (device_status == vktest::DeviceStatus::unavailable) return 77;  // no Vulkan GPU (hosted CI): ctest skips
    EXPECT(device_status == vktest::DeviceStatus::ready);

    // No shader library, whatever the environment says.
    SetEnvironmentVariableW(L"LEONIDA_SHADERS", nullptr);
    const auto vs_program = fssm_program({0x56530001, 0xbf810000});
    const auto ps_program = fssm_program({0x50530001, 0xbf810000});
    const auto vs_container = shader_container(vs_program), ps_container = shader_container(ps_program);

    const auto ms_container = shader_container(fssm_program({0x4D530001, 0xbf810000}));
    std::vector<std::uint8_t> empty_pixels(2 * size * pitch), colour_pixels(2 * size * pitch),
        cull_pixels(2 * size * pitch), reverse_z_pixels(2 * size * pitch), wireframe_pixels(2 * size * pitch),
        no_samples_pixels(2 * size * pitch);
    HRESULT foreign_close = S_OK;
    HRESULT mesh_create = E_FAIL, mesh_own_close = E_FAIL, mesh_other_close = S_OK, wireframe_create = E_FAIL;
    {
        ForeignPipelineState foreign;  // declared before the list, so it outlives the list's reference
        rt::d3d12::VulkanDeviceInfo info{};
        info.get_instance_proc = d.instance_proc;
        info.instance = d.instance;
        info.physical = d.physical;
        info.device = d.device;
        info.queue = d.queue;
        info.queue_family = d.family;
        info.buffer_device_address = true;
        info.mesh_shader = d.mesh_shader;
        ID3D12Device* inner = nullptr;
        EXPECT(SUCCEEDED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&inner))));
        Ref<ID3D12Device> device;
        EXPECT(SUCCEEDED(rt::d3d12_xs::create_device_proxy(inner, __uuidof(ID3D12Device),
                                                           reinterpret_cast<void**>(&device.p), /*console_lists=*/false)));
        rt::d3d12::set_device_outer(inner, device.p);
        if (rt::d3d12::device_push_constants_need_fallback(inner)) {
            std::printf("d3d12_stub_draw_test: skipped, the PSOs need 144 push-constant bytes and this GPU has %u\n",
                        rt::d3d12::device_max_push_constants_size(inner));
            return 77;
        }

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
        root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
        root_desc.Desc_1_0 = {0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
        const auto root_blob = serialize_root_signature(root_desc);
        EXPECT(!root_blob.empty());
        Ref<ID3D12RootSignature> root;
        EXPECT(SUCCEEDED(device->CreateRootSignature(0, root_blob.data(), root_blob.size(), IID_PPV_ARGS(&root.p))));

        const D3D12_INPUT_ELEMENT_DESC elements[1] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
        pso_desc.pRootSignature = root.p;
        pso_desc.VS = {vs_container.data(), vs_container.size()};
        pso_desc.PS = {ps_container.data(), ps_container.size()};
        pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso_desc.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso_desc.SampleMask = 0xffffffff;
        pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso_desc.RasterizerState.DepthClipEnable = TRUE;
        pso_desc.InputLayout = {elements, 1};
        pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso_desc.NumRenderTargets = 2;
        pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pso_desc.RTVFormats[1] = DXGI_FORMAT_R32_UINT;
        pso_desc.SampleDesc = {1, 0};

        Ref<ID3D12PipelineState> off, empty, colour, points, cull_back, reverse_z, wireframe, no_samples;
        _putenv_s("LEONIDA_STUB_PIPELINES", "");
        EXPECT(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&off.p)) == E_FAIL && !off.p);
        _putenv_s("LEONIDA_STUB_PIPELINES", "1");
        EXPECT(SUCCEEDED(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&empty.p))));
        D3D12_GRAPHICS_PIPELINE_STATE_DESC point_desc = pso_desc;
        point_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        EXPECT(SUCCEEDED(device->CreateGraphicsPipelineState(&point_desc, IID_PPV_ARGS(&points.p))));
        _putenv_s("LEONIDA_STUB_PIPELINES", "color");
        EXPECT(SUCCEEDED(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&colour.p))));
        // State that would hide the cover: the D3D12 default cull mode, and a reverse-Z test (GREATER against a clear
        // of 0, with the stub at z = 0).
        D3D12_GRAPHICS_PIPELINE_STATE_DESC cull_desc = pso_desc;
        cull_desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        EXPECT(SUCCEEDED(device->CreateGraphicsPipelineState(&cull_desc, IID_PPV_ARGS(&cull_back.p))));
        D3D12_GRAPHICS_PIPELINE_STATE_DESC reverse_z_desc = pso_desc;
        reverse_z_desc.DepthStencilState.DepthEnable = TRUE;
        reverse_z_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        reverse_z_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
        reverse_z_desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        EXPECT(SUCCEEDED(device->CreateGraphicsPipelineState(&reverse_z_desc, IID_PPV_ARGS(&reverse_z.p))));
        D3D12_GRAPHICS_PIPELINE_STATE_DESC wireframe_desc = pso_desc;
        wireframe_desc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;  // refused without fillModeNonSolid
        wireframe_create = device->CreateGraphicsPipelineState(&wireframe_desc, IID_PPV_ARGS(&wireframe.p));
        D3D12_GRAPHICS_PIPELINE_STATE_DESC no_samples_desc = pso_desc;
        no_samples_desc.SampleMask = 0;
        EXPECT(SUCCEEDED(device->CreateGraphicsPipelineState(&no_samples_desc, IID_PPV_ARGS(&no_samples.p))));
        _putenv_s("LEONIDA_STUB_PIPELINES", "");
        for (ID3D12PipelineState* pso : {empty.p, points.p, colour.p, cull_back.p, reverse_z.p, no_samples.p}) {
            EXPECT(rt::d3d12::graphics_pipeline_id(pso) != 0);  // registered: SetPipelineState binds it
            EXPECT(rt::d3d12::compute_pipeline_id(pso) == 0);
        }

        const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
        const D3D12_HEAP_PROPERTIES upload_heap{D3D12_HEAP_TYPE_UPLOAD};
        const D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK};
        Ref<ID3D12Resource> vertices, readback, rgba, uints, depth;
        const auto vb_desc = buffer_desc(256);
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &vb_desc,
                                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(&vertices.p))));
        const auto readback_desc = buffer_desc(2 * size * pitch);
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&readback.p))));
        D3D12_RESOURCE_DESC rt_desc{};
        rt_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rt_desc.Width = rt_desc.Height = size;
        rt_desc.DepthOrArraySize = 1;
        rt_desc.MipLevels = 1;
        rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rt_desc.SampleDesc = {1, 0};
        rt_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &rt_desc,
                                                         D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                         IID_PPV_ARGS(&rgba.p))));
        rt_desc.Format = DXGI_FORMAT_R32_UINT;
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &rt_desc,
                                                         D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                         IID_PPV_ARGS(&uints.p))));
        D3D12_RESOURCE_DESC depth_desc = rt_desc;
        depth_desc.Format = DXGI_FORMAT_D32_FLOAT;
        depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &depth_desc,
                                                         D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr,
                                                         IID_PPV_ARGS(&depth.p))));
        void* mapped_vertices = nullptr;
        EXPECT(SUCCEEDED(vertices->Map(0, nullptr, &mapped_vertices)));
        std::memset(mapped_vertices, 0, 256);  // never read by the stub stages

        const D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2,
                                                       D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        Ref<ID3D12DescriptorHeap> rtv_heap;
        EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap.p))));
        const D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2] = {heap_start(rtv_heap.p),
                                                     {heap_start(rtv_heap.p).ptr + rt::d3d12::descriptor_size}};
        device->CreateRenderTargetView(rgba.p, nullptr, rtvs[0]);
        device->CreateRenderTargetView(uints.p, nullptr, rtvs[1]);
        const D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1,
                                                       D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        Ref<ID3D12DescriptorHeap> dsv_heap;
        EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&dsv_heap_desc, IID_PPV_ARGS(&dsv_heap.p))));
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = heap_start(dsv_heap.p);
        device->CreateDepthStencilView(depth.p, nullptr, dsv);

        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        Ref<ID3D12CommandQueue> queue;
        Ref<ID3D12CommandAllocator> allocator;
        Ref<ID3D12GraphicsCommandList> list;
        Ref<ID3D12Fence> fence;
        EXPECT(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))));
        EXPECT(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
        EXPECT(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, nullptr,
                                                   IID_PPV_ARGS(&list.p))));
        EXPECT(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));
        UINT64 fence_value = 0;

        // Clear, draw with each PSO in `draws`, and read both targets back into `pixels`. With `depth_target`, the
        // depth target is cleared to 0 and bound as well.
        const auto run = [&](std::initializer_list<std::pair<ID3D12PipelineState*, D3D_PRIMITIVE_TOPOLOGY>> draws,
                             std::vector<std::uint8_t>& pixels, bool depth_target = false) -> bool {
            if (fence_value && (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.p, nullptr)))) return false;
            const FLOAT blue[4] = {0, 0, 1, 1}, seven[4] = {7, 0, 0, 0};
            list->ClearRenderTargetView(rtvs[0], blue, 0, nullptr);
            list->ClearRenderTargetView(rtvs[1], seven, 0, nullptr);
            if (depth_target) list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
            list->SetGraphicsRootSignature(root.p);
            const D3D12_VERTEX_BUFFER_VIEW vbv{vertices->GetGPUVirtualAddress(), 256, 8};
            list->IASetVertexBuffers(0, 1, &vbv);
            const D3D12_VIEWPORT viewport{0, 0, float(size), float(size), 0, 1};
            const D3D12_RECT scissor{0, 0, LONG(size), LONG(size)};
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            list->OMSetRenderTargets(2, rtvs, FALSE, depth_target ? &dsv : nullptr);
            for (const auto& [pso, topology] : draws) {
                list->SetPipelineState(pso);
                list->IASetPrimitiveTopology(topology);
                list->DrawInstanced(topology == D3D_PRIMITIVE_TOPOLOGY_POINTLIST ? 4 : 3, 1, 0, 0);
            }
            ID3D12Resource* targets[2] = {rgba.p, uints.p};
            const DXGI_FORMAT formats[2] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R32_UINT};
            for (UINT i = 0; i < 2; ++i) {
                D3D12_RESOURCE_BARRIER to_copy = transition(targets[i], D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                            D3D12_RESOURCE_STATE_COPY_SOURCE);
                list->ResourceBarrier(1, &to_copy);
                D3D12_TEXTURE_COPY_LOCATION dst{};
                dst.pResource = readback.p;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint = {i * size * pitch, {formats[i], size, size, 1, pitch}};
                D3D12_TEXTURE_COPY_LOCATION src{};
                src.pResource = targets[i];
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                D3D12_RESOURCE_BARRIER back = transition(targets[i], D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                         D3D12_RESOURCE_STATE_RENDER_TARGET);
                list->ResourceBarrier(1, &back);
            }
            if (list->Close() != S_OK) return false;
            ID3D12CommandList* lists[] = {list.p};
            queue->ExecuteCommandLists(1, lists);
            if (rt::d3d12::last_execute_result(queue.p) != leonida::ReplayResult::success) return false;
            if (FAILED(queue->Signal(fence.p, ++fence_value)) || FAILED(fence->SetEventOnCompletion(fence_value, nullptr)))
                return false;
            void* mapped = nullptr;
            if (FAILED(readback->Map(0, nullptr, &mapped))) return false;
            std::memcpy(pixels.data(), mapped, pixels.size());
            readback->Unmap(0, nullptr);
            return true;
        };
        EXPECT(run({{empty.p, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST}, {points.p, D3D_PRIMITIVE_TOPOLOGY_POINTLIST}},
                   empty_pixels));
        EXPECT(run({{colour.p, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST}}, colour_pixels));
        EXPECT(run({{cull_back.p, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST}}, cull_pixels));
        EXPECT(run({{reverse_z.p, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST}}, reverse_z_pixels, /*depth_target=*/true));
        if (wireframe.p) EXPECT(run({{wireframe.p, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST}}, wireframe_pixels));
        EXPECT(run({{no_samples.p, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST}}, no_samples_pixels));

        // A PSO from another implementation, with the switch off: the draw is a recording error, so Close fails.
        EXPECT(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.p, nullptr)));
        {
            list->SetGraphicsRootSignature(root.p);
            const D3D12_VIEWPORT viewport{0, 0, float(size), float(size), 0, 1};
            const D3D12_RECT scissor{0, 0, LONG(size), LONG(size)};
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            list->OMSetRenderTargets(2, rtvs, FALSE, nullptr);
            list->SetPipelineState(&foreign);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->DrawInstanced(3, 1, 0, 0);
            foreign_close = list->Close();
        }

        // A mesh stub (no shader library, so no translated mesh stage) and the device that made it: DispatchMesh with
        // it records nothing on that device's list, and is a recording error on a list of another device.
        {
            D3D12_VERSIONED_ROOT_SIGNATURE_DESC mesh_root_desc{};
            mesh_root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
            mesh_root_desc.Desc_1_0 = {0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
            const auto mesh_root_blob = serialize_root_signature(mesh_root_desc);
            Ref<ID3D12RootSignature> mesh_root;
            EXPECT(SUCCEEDED(device->CreateRootSignature(0, mesh_root_blob.data(), mesh_root_blob.size(),
                                                         IID_PPV_ARGS(&mesh_root.p))));
            D3D12_RT_FORMAT_ARRAY formats{};
            formats.NumRenderTargets = 2;
            formats.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            formats.RTFormats[1] = DXGI_FORMAT_R32_UINT;
            Stream stream;
            stream.add(Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature*>{
                          D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, mesh_root.p})
                .add(Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE>{
                    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, {ms_container.data(), ms_container.size()}})
                .add(Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE>{
                    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, {ps_container.data(), ps_container.size()}})
                .add(Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE>{
                    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE})
                .add(Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY>{
                    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, formats});
            const D3D12_PIPELINE_STATE_STREAM_DESC stream_desc{stream.bytes.size(), stream.bytes.data()};
            Ref<ID3D12Device2> device2;
            Ref<ID3D12PipelineState> mesh_stub;
            _putenv_s("LEONIDA_STUB_PIPELINES", "1");
            if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device2.p))))
                mesh_create = device2->CreatePipelineState(&stream_desc, IID_PPV_ARGS(&mesh_stub.p));
            _putenv_s("LEONIDA_STUB_PIPELINES", "");
            if (SUCCEEDED(mesh_create) && mesh_stub.p) {
                // Records a DispatchMesh with the stub on a new list of `owner` and returns what Close gives.
                const auto dispatch_mesh = [&](ID3D12Device* owner, ID3D12RootSignature* signature) -> HRESULT {
                    Ref<ID3D12CommandAllocator> mesh_allocator;
                    Ref<ID3D12GraphicsCommandList> mesh_list;
                    Ref<ID3D12GraphicsCommandList6> mesh_list6;
                    if (FAILED(owner->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                             IID_PPV_ARGS(&mesh_allocator.p))) ||
                        FAILED(owner->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, mesh_allocator.p, nullptr,
                                                        IID_PPV_ARGS(&mesh_list.p))) ||
                        FAILED(mesh_list->QueryInterface(IID_PPV_ARGS(&mesh_list6.p))))
                        return E_NOINTERFACE;
                    if (signature) mesh_list->SetGraphicsRootSignature(signature);
                    mesh_list->SetPipelineState(mesh_stub.p);
                    mesh_list6->DispatchMesh(1, 1, 1);
                    return mesh_list->Close();
                };
                mesh_own_close = dispatch_mesh(device.p, mesh_root.p);
                rt::d3d12::VulkanDeviceInfo other_info{};
                other_info.get_instance_proc = d.instance_proc;
                other_info.instance = d.instance;
                other_info.physical = d.physical;
                other_info.device = d.device;
                other_info.queue = d.queue;
                other_info.queue_family = d.family;
                other_info.buffer_device_address = true;
                other_info.mesh_shader = d.mesh_shader;
                Ref<ID3D12Device> other;
                EXPECT(SUCCEEDED(rt::d3d12::create_vulkan_device(other_info, IID_PPV_ARGS(&other.p))));
                mesh_other_close = dispatch_mesh(other.p, nullptr);
            }
        }

        for (auto* r : {&vertices, &readback, &rgba, &uints, &depth}) r->reset();
        for (auto* p : {&empty, &points, &colour, &cull_back, &reverse_z, &wireframe, &no_samples}) p->reset();
        root.reset();
        rtv_heap.reset();
        dsv_heap.reset();
        list.reset();
        allocator.reset();
        queue.reset();
        fence.reset();
        EXPECT(rt::d3d12_xs::device_proxy_ref_count(device.p) == 1);
    }

    const std::uint8_t blue[4] = {0, 0, 255, 255};
    std::uint8_t magenta[4];
    for (int i = 0; i < 4; ++i) magenta[i] = std::uint8_t(leonida::stub_colour[i] * 255.0f);
    // Every case is checked and reported, so one failure does not hide another.
    bool ok = targets_hold(empty_pixels.data(), empty_pixels.data() + size * pitch, blue, 7, "LEONIDA_STUB_PIPELINES=1");
    ok &= targets_hold(colour_pixels.data(), colour_pixels.data() + size * pitch, magenta, 1,
                       "LEONIDA_STUB_PIPELINES=color");
    ok &= targets_hold(cull_pixels.data(), cull_pixels.data() + size * pitch, magenta, 1,
                       "LEONIDA_STUB_PIPELINES=color, CULL_MODE_BACK");
    ok &= targets_hold(reverse_z_pixels.data(), reverse_z_pixels.data() + size * pitch, magenta, 1,
                       "LEONIDA_STUB_PIPELINES=color, reverse-Z GREATER against 0");
    if (FAILED(wireframe_create)) {
        std::fprintf(stderr, "LEONIDA_STUB_PIPELINES=color, FILL_MODE_WIREFRAME: CreateGraphicsPipelineState returned "
                             "0x%08lx\n",
                     static_cast<unsigned long>(wireframe_create));
        ok = false;
    } else {
        ok &= targets_hold(wireframe_pixels.data(), wireframe_pixels.data() + size * pitch, magenta, 1,
                           "LEONIDA_STUB_PIPELINES=color, FILL_MODE_WIREFRAME");
    }
    ok &= targets_hold(no_samples_pixels.data(), no_samples_pixels.data() + size * pitch, magenta, 1,
                       "LEONIDA_STUB_PIPELINES=color, SampleMask 0");
    if (foreign_close != E_INVALIDARG) {
        std::fprintf(stderr, "a draw with another implementation's PSO: Close returned 0x%08lx, want E_INVALIDARG\n",
                     static_cast<unsigned long>(foreign_close));
        ok = false;
    }
    const bool mesh_checked = SUCCEEDED(mesh_create);
    if (!mesh_checked) {
        std::printf("d3d12_stub_draw_test: mesh stub cases skipped, CreatePipelineState returned 0x%08lx\n",
                    static_cast<unsigned long>(mesh_create));
    } else {
        if (mesh_own_close != S_OK) {
            std::fprintf(stderr, "DispatchMesh with this device's mesh stub: Close returned 0x%08lx, want S_OK\n",
                         static_cast<unsigned long>(mesh_own_close));
            ok = false;
        }
        if (mesh_other_close != E_INVALIDARG) {
            std::fprintf(stderr, "DispatchMesh with another device's mesh stub: Close returned 0x%08lx, want "
                                 "E_INVALIDARG\n",
                         static_cast<unsigned long>(mesh_other_close));
            ok = false;
        }
    }
    if (!ok) return 1;
    if (d.validation_errors()) {
        std::fprintf(stderr, "%u validation errors\n", d.validation_errors());
        return 1;
    }
    std::printf("stub pipelines: %u pixels per target; =1 drew nothing (triangles and points), =color drew magenta "
                "and integer 1 over the whole viewport (also with back-face culling, wireframe, sample mask 0 and "
                "reverse-Z); a foreign PSO's draw failed the list%s\n",
                size * size, mesh_checked ? "; a mesh stub's DispatchMesh closed on its device and failed another's"
                                          : "");
    return 0;
}
