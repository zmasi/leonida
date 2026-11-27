// Terminal-only DISPATCH_MESH command signatures without mesh support (docs/d3d12_vulkan.md, mesh shaders).
// A 12-byte null-root signature with the single DISPATCH_MESH operation is record metadata: it is created with
// S_OK on a device without VK_EXT_mesh_shader, keeps its COM identity (QueryInterface, GetDevice, lifetime),
// and still reports MeshShaderTier NOT_SUPPORTED. Executing it is a recording error: ExecuteIndirect with a
// positive maximum fails Close (fixed and GPU-supplied counts alike), while a zero maximum is a valid no-op.
// Non-terminal mesh signatures still need the extension (E_NOTIMPL), and ordinary draw/dispatch signatures are
// unchanged. No shaders, no host D3D12, no game data: only synthetic 12/16/20-byte records. Runs on every
// Vulkan 1.3 GPU; returns 0 on a no-mesh adapter (gfx906), 77 only when no usable Vulkan device exists.
#include "vk_test_device.h"
#include "d3d12_test_shaders.h"
#include "../shims/d3d12_com.h"
#include "../shims/d3d12_xs_device.h"

#include <cstdint>
#include <cstdio>

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

using namespace d3d12test;

namespace {

D3D12_MESH_SHADER_TIER mesh_tier(ID3D12Device* device) {
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 options{};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options, sizeof(options))))
        return D3D12_MESH_SHADER_TIER(-1);
    return options.MeshShaderTier;
}

bool make_device(vktest::Device& d, bool mesh, Ref<ID3D12Device>& out) {
    rt::d3d12::VulkanDeviceInfo info{};
    info.get_instance_proc = d.instance_proc;
    info.instance = d.instance;
    info.physical = d.physical;
    info.device = d.device;
    info.queue = d.queue;
    info.queue_family = d.family;
    info.mesh_shader = mesh;
    ID3D12Device* inner = nullptr;
    if (FAILED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&inner)))) return false;
    if (FAILED(rt::d3d12_xs::create_device_proxy(inner, __uuidof(ID3D12Device),
                                                 reinterpret_cast<void**>(&out.p)))) {
        inner->Release();
        return false;
    }
    rt::d3d12::set_device_outer(inner, out.p);
    return true;
}

// Close() of a fresh direct list holding one ExecuteIndirect with the given maximum and count buffer.
HRESULT close_with_indirect(ID3D12Device* device, ID3D12CommandSignature* sig, UINT max_count,
                            ID3D12Resource* args, ID3D12Resource* count) {
    Ref<ID3D12CommandAllocator> alloc;
    Ref<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc.p))))
        return E_FAIL;
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.p, nullptr,
                                          IID_PPV_ARGS(&list.p))))
        return E_FAIL;
    list->ExecuteIndirect(sig, max_count, args, 0, count, 0);
    return list->Close();
}

bool make_empty_root(ID3D12Device* device, Ref<ID3D12RootSignature>& out) {
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rs{};
    rs.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
    rs.Desc_1_0 = {0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    const auto blob = serialize_root_signature(rs);
    if (blob.empty()) return false;
    return SUCCEEDED(device->CreateRootSignature(0, blob.data(), blob.size(), IID_PPV_ARGS(&out.p)));
}

}  // namespace

int main() {
    vktest::Device d;
    d.request_mesh_shader = true;
    const auto device_status = d.initialize();
    if (device_status == vktest::DeviceStatus::unavailable) {
        std::fprintf(stderr, "SKIP: no usable Vulkan 1.3 device\n");
        return 77;
    }
    EXPECT(device_status == vktest::DeviceStatus::ready);
    const unsigned errors_before = d.validation_errors();

    // Without the extension: no tier, but the terminal record description is still owned.
    Ref<ID3D12Device> device;
    EXPECT(make_device(d, false, device));
    EXPECT(mesh_tier(device.p) == D3D12_MESH_SHADER_TIER_NOT_SUPPORTED);
    EXPECT(rt::d3d12_xs::device_proxy_ref_count(device.p) == 1);

    const D3D12_INDIRECT_ARGUMENT_DESC mesh_arg{D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH};
    const D3D12_COMMAND_SIGNATURE_DESC mesh_sig{12, 1, &mesh_arg, 0};
    Ref<ID3D12CommandSignature> sig;
    EXPECT(device->CreateCommandSignature(&mesh_sig, nullptr, IID_PPV_ARGS(&sig.p)) == S_OK && sig.p);
    leonida::IndirectKind kind{};
    UINT stride = 0;
    bool terminal_only = false;
    EXPECT(rt::d3d12::command_signature_info(sig.p, kind, stride, terminal_only) &&
           kind == leonida::IndirectKind::dispatch_mesh && stride == 12 && terminal_only);
    EXPECT(rt::d3d12_xs::device_proxy_ref_count(device.p) == 2);

    // COM identity: the signature answers its own interfaces, nothing else, and its device is this proxy.
    {
        void* qi = nullptr;
        EXPECT(sig.p->QueryInterface(__uuidof(ID3D12CommandSignature), &qi) == S_OK && qi);
        static_cast<IUnknown*>(qi)->Release();
        qi = nullptr;
        EXPECT(sig.p->QueryInterface(__uuidof(IUnknown), &qi) == S_OK && qi);
        static_cast<IUnknown*>(qi)->Release();
        qi = nullptr;
        EXPECT(sig.p->QueryInterface(__uuidof(ID3D12Object), &qi) == S_OK && qi);
        static_cast<IUnknown*>(qi)->Release();
        qi = nullptr;
        EXPECT(sig.p->QueryInterface(__uuidof(ID3D12DeviceChild), &qi) == S_OK && qi);
        static_cast<IUnknown*>(qi)->Release();
        qi = reinterpret_cast<void*>(static_cast<uintptr_t>(0xc0ffee));
        EXPECT(sig.p->QueryInterface(__uuidof(ID3D12Fence), &qi) == E_NOINTERFACE && !qi);
        Ref<ID3D12Device> got;
        EXPECT(sig.p->GetDevice(IID_PPV_ARGS(&got.p)) == S_OK && got.p);
        EXPECT(rt::d3d12_xs::device_proxy_ref_count(device.p) == 3);
        IUnknown* via_sig = nullptr;
        IUnknown* via_device = nullptr;
        EXPECT(SUCCEEDED(got.p->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&via_sig))) &&
               via_sig);
        EXPECT(SUCCEEDED(device.p->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&via_device))) &&
               via_device == via_sig);
        via_sig->Release();
        via_device->Release();
        got.reset();
        EXPECT(rt::d3d12_xs::device_proxy_ref_count(device.p) == 2);
    }

    // A stride below the 12-byte record is refused, and the output is cleared even from a sentinel.
    {
        const D3D12_COMMAND_SIGNATURE_DESC short_sig{8, 1, &mesh_arg, 0};
        void* out = reinterpret_cast<void*>(static_cast<uintptr_t>(0xc0ffee));
        EXPECT(device->CreateCommandSignature(&short_sig, nullptr, __uuidof(ID3D12CommandSignature), &out) ==
                   E_INVALIDARG &&
               !out);
        EXPECT(rt::d3d12_xs::device_proxy_ref_count(device.p) == 2);
    }

    // Prefix rules without the extension: anything before the terminal mesh operation still needs it.
    {
        D3D12_INDIRECT_ARGUMENT_DESC prefixed[2] = {};
        prefixed[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW;
        prefixed[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;
        const D3D12_COMMAND_SIGNATURE_DESC compound{28, 2, prefixed, 0};
        void* out = reinterpret_cast<void*>(static_cast<uintptr_t>(0xc0ffee));
        EXPECT(device->CreateCommandSignature(&compound, nullptr, __uuidof(ID3D12CommandSignature), &out) ==
                   E_NOTIMPL &&
               !out);
        Ref<ID3D12RootSignature> root;
        EXPECT(make_empty_root(device.p, root));
        out = reinterpret_cast<void*>(static_cast<uintptr_t>(0xc0ffee));
        EXPECT(device->CreateCommandSignature(&mesh_sig, root.p, __uuidof(ID3D12CommandSignature), &out) ==
                   E_NOTIMPL &&
               !out);
    }

    // Ordinary signatures are unchanged on the same device.
    {
        const D3D12_INDIRECT_ARGUMENT_DESC dispatch_arg{D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH};
        const D3D12_COMMAND_SIGNATURE_DESC dispatch_sig{12, 1, &dispatch_arg, 0};
        Ref<ID3D12CommandSignature> dispatch;
        EXPECT(SUCCEEDED(device->CreateCommandSignature(&dispatch_sig, nullptr, IID_PPV_ARGS(&dispatch.p))) &&
               dispatch.p);
        const D3D12_INDIRECT_ARGUMENT_DESC draw_arg{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW};
        const D3D12_COMMAND_SIGNATURE_DESC draw_sig{16, 1, &draw_arg, 0};
        Ref<ID3D12CommandSignature> draw;
        EXPECT(SUCCEEDED(device->CreateCommandSignature(&draw_sig, nullptr, IID_PPV_ARGS(&draw.p))) && draw.p);
    }

    // Recording: a positive maximum without an indirect mesh path fails Close; zero is a no-op.
    {
        const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
        const auto desc = buffer_desc(256);
        Ref<ID3D12Resource> args, count;
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                         D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                         IID_PPV_ARGS(&args.p))));
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                         D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                         IID_PPV_ARGS(&count.p))));
        EXPECT(close_with_indirect(device.p, sig.p, 1, args.p, nullptr) == E_INVALIDARG);
        EXPECT(close_with_indirect(device.p, sig.p, 1, args.p, count.p) == E_INVALIDARG);
        EXPECT(close_with_indirect(device.p, sig.p, 0, args.p, nullptr) == S_OK);
        EXPECT(close_with_indirect(device.p, sig.p, 0, args.p, count.p) == S_OK);
    }
    sig.reset();
    EXPECT(rt::d3d12_xs::device_proxy_ref_count(device.p) == 1);

    // Where the GPU has the extension, the same terminal description still executes as before: a positive
    // maximum without graphics state records nothing and closes, rather than failing.
    if (d.mesh_shader) {
        Ref<ID3D12Device> mesh_device;
        EXPECT(make_device(d, true, mesh_device));
        EXPECT(mesh_tier(mesh_device.p) == D3D12_MESH_SHADER_TIER_1);
        Ref<ID3D12CommandSignature> mesh_sig_obj;
        EXPECT(mesh_device->CreateCommandSignature(&mesh_sig, nullptr, IID_PPV_ARGS(&mesh_sig_obj.p)) == S_OK &&
               mesh_sig_obj.p);
        const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
        const auto desc = buffer_desc(256);
        Ref<ID3D12Resource> args;
        EXPECT(SUCCEEDED(mesh_device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                              D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                              IID_PPV_ARGS(&args.p))));
        EXPECT(close_with_indirect(mesh_device.p, mesh_sig_obj.p, 1, args.p, nullptr) == S_OK);
        EXPECT(close_with_indirect(mesh_device.p, mesh_sig_obj.p, 0, args.p, nullptr) == S_OK);
        mesh_sig_obj.reset();
        args.reset();
        EXPECT(rt::d3d12_xs::device_proxy_ref_count(mesh_device.p) == 1);
        std::printf("mesh-capable device: terminal mesh execution still closes\n");
    } else {
        std::printf("no VK_EXT_mesh_shader on this device: only the terminal metadata without it was checked\n");
    }

    if (d.validation_errors() != errors_before) return 1;
    std::printf("command_signature_mesh: terminal DISPATCH_MESH metadata without mesh, execution refused\n");
    return 0;
}
