// Buffer descriptors that name memory nothing backs become zero-size (docs/d3d12_vulkan.md section 3.3, "Title-chosen
// addresses"): a shader adds heap_base to a V#'s base, and heap_base + an address no allocation holds is a GPU MMU
// fault. Each path the layer sees a buffer address on writes such an address as a V# with no base and no records
// (loads return 0, stores are dropped) instead of passing it through:
//   - created views: CreateConstantBufferView and a slot-70 placed buffer view of an unmapped address;
//   - inline V#s: a V# the CPU writes into a bound table of a shader-visible heap itself, rewritten before the
//     submission;
//   - a console descriptor heap (Xbox slot 14) the title writes itself: its GPU copy's slot is null, the title's
//     bytes are left as written;
//   - and the null slots after a resource heap's GPU copy: an index past the heap reads a null descriptor, not the
//     arena's next bytes.
// A data slot (neither a V# nor a T#) of a console heap is null in its GPU copy too.
// Positive controls: an arena buffer's V#, an upload buffer's V# (outside the arena, a registered buffer) and an
// image-shaped slot keep their bytes. Nothing here dispatches through a descriptor, so a regression shows as a
// failed check, never as a GPU fault. No game data.
#include "vk_test_device.h"
#include "../shims/d3d12_com.h"
#include "../src/gfx10_descriptors.h"
#include "../src/title_va_alias.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

namespace {

using SerializeFn = HRESULT(WINAPI*)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);

template<class T>
struct Ref {
    T* p = nullptr;
    ~Ref() { reset(); }
    void reset() {
        if (p) p->Release();
        p = nullptr;
    }
    T* operator->() const { return p; }
};

constexpr UINT kDescriptor = 32;
constexpr UINT kViewBytes = 256;

D3D12_RESOURCE_DESC buffer_desc(UINT64 bytes) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

leonida::gfx10::BufferWords raw_vsharp(std::uint64_t address) {
    leonida::gfx10::Buffer b{};
    b.address = address;
    b.num_records = kViewBytes;
    b.format = leonida::gfx10::format_from_vulkan(VK_FORMAT_R32_UINT);
    b.oob_select = leonida::gfx10::OobSelect::raw;
    return leonida::gfx10::encode(b);
}

std::uint64_t base_of(const std::uint32_t* w) { return std::uint64_t(w[0]) | (std::uint64_t(w[1] & 0xFFFFu) << 32); }
bool zero_size(const std::uint32_t* w) { return base_of(w) == 0 && w[2] == 0; }

// An address no allocation holds and that is not the title's memory: reserved, then released. Top-down, so it lies
// far past the arena (which is below 4 GiB).
std::uint64_t unmapped_address() {
    void* p = VirtualAlloc(nullptr, 1 << 20, MEM_TOP_DOWN | MEM_RESERVE, PAGE_NOACCESS);
    if (!p) return 0;
    VirtualFree(p, 0, MEM_RELEASE);
    return reinterpret_cast<std::uint64_t>(p) + 0x1000;
}

std::vector<std::uint8_t> serialize(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC& desc) {
    const HMODULE d3d12 = LoadLibraryExA("d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto fn = d3d12 ? reinterpret_cast<SerializeFn>(
                                reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12SerializeVersionedRootSignature")))
                          : nullptr;
    std::vector<std::uint8_t> out;
    ID3DBlob* blob = nullptr;
    if (fn && SUCCEEDED(fn(&desc, &blob, nullptr)) && blob) {
        const auto* p = static_cast<const std::uint8_t*>(blob->GetBufferPointer());
        out.assign(p, p + blob->GetBufferSize());
        blob->Release();
    }
    return out;
}

struct Buffers {
    Ref<ID3D12Resource> arena, upload;
    std::uint64_t arena_va = 0, upload_va = 0;
};

int make_buffers(ID3D12Device* device, std::uint64_t arena_size, Buffers& b) {
    const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT}, upload_heap{D3D12_HEAP_TYPE_UPLOAD};
    const auto desc = buffer_desc(64 * 1024);
    EXPECT(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&b.arena.p))));
    EXPECT(SUCCEEDED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(&b.upload.p))));
    b.arena_va = b.arena->GetGPUVirtualAddress();
    b.upload_va = b.upload->GetGPUVirtualAddress();
    EXPECT(b.arena_va && b.arena_va < arena_size);
    EXPECT(b.upload_va);
    return 0;
}

// Created views: a CBV and a slot-70 buffer SRV of an unmapped address get no base and no records; a CBV of an
// arena buffer and of an upload buffer keep their addresses.
int check_created_views(ID3D12Device* device, const Buffers& b, std::uint64_t unmapped) {
    alignas(32) std::uint32_t slots[4][8] = {};
    const auto handle = [&](int i) { return D3D12_CPU_DESCRIPTOR_HANDLE{reinterpret_cast<SIZE_T>(slots[i])}; };
    const D3D12_CONSTANT_BUFFER_VIEW_DESC nowhere{unmapped, kViewBytes}, arena{b.arena_va, kViewBytes},
        upload{b.upload_va, kViewBytes};
    device->CreateConstantBufferView(&nowhere, handle(0));
    device->CreateConstantBufferView(&arena, handle(1));
    device->CreateConstantBufferView(&upload, handle(2));
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements = kViewBytes / 4;
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    EXPECT(rt::d3d12::write_placed_buffer_view(device, false, &srv, unmapped, kViewBytes, handle(3)));
    EXPECT(zero_size(slots[0]));
    EXPECT(base_of(slots[1]) == b.arena_va && slots[1][2] != 0);
    EXPECT(base_of(slots[2]) == (b.upload_va & 0xFFFFFFFFFFFFull) && slots[2][2] != 0);
    EXPECT(zero_size(slots[3]));
    std::printf("created views: a CBV and a placed SRV of unmapped 0x%llx are zero-size; arena and upload CBVs kept\n",
                static_cast<unsigned long long>(unmapped));
    return 0;
}

// Inline V#s in a bound table of one of the layer's heaps, written by the CPU itself as the title does: before the
// submission an unmapped base becomes zero-size in place; arena, upload and image-shaped slots keep their bytes.
int check_inline_table(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64& fence_value,
                       const Buffers& b, std::uint64_t unmapped) {
    const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8,
                                               D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    Ref<ID3D12DescriptorHeap> heap;
    EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap.p))));
    auto* cpu = reinterpret_cast<std::uint8_t*>(heap->GetCPUDescriptorHandleForHeapStart().ptr);
    const auto put = [&](int slot, const leonida::gfx10::BufferWords& w) {
        std::memcpy(cpu + slot * kDescriptor, w.data(), sizeof(w));
    };
    put(0, raw_vsharp(unmapped));
    put(1, raw_vsharp(b.arena_va));
    put(2, raw_vsharp(b.upload_va & 0xFFFFFFFFFFFFull));
    auto image_like = raw_vsharp(unmapped);
    image_like[3] |= 0x8u << 28;  // TYPE: an image
    put(3, image_like);
    std::uint8_t kept[3][kDescriptor];
    for (int i = 0; i < 3; ++i) std::memcpy(kept[i], cpu + (i + 1) * kDescriptor, kDescriptor);

    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 8, 0, 0, 0};
    D3D12_ROOT_PARAMETER param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable = {1, &range};
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
    root_desc.Desc_1_0 = {1, &param, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    const auto blob = serialize(root_desc);
    EXPECT(!blob.empty());
    Ref<ID3D12RootSignature> root;
    EXPECT(SUCCEEDED(device->CreateRootSignature(0, blob.data(), blob.size(), IID_PPV_ARGS(&root.p))));
    Ref<ID3D12CommandAllocator> allocator;
    Ref<ID3D12GraphicsCommandList> list;
    EXPECT(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
    EXPECT(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, nullptr,
                                               IID_PPV_ARGS(&list.p))));
    ID3D12DescriptorHeap* heaps[] = {heap.p};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root.p);
    list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    EXPECT(list->Close() == S_OK);
    ID3D12CommandList* lists[] = {list.p};
    queue->ExecuteCommandLists(1, lists);
    EXPECT(SUCCEEDED(queue->Signal(fence, ++fence_value)));
    EXPECT(SUCCEEDED(fence->SetEventOnCompletion(fence_value, nullptr)));

    const auto* slot0 = reinterpret_cast<const std::uint32_t*>(cpu);
    EXPECT(zero_size(slot0));
    for (int i = 0; i < 3; ++i) EXPECT(!std::memcmp(kept[i], cpu + (i + 1) * kDescriptor, kDescriptor));
    std::printf("inline table V#s: unmapped base written zero-size before the submission; arena, upload and "
                "image-shaped slots kept\n");
    return 0;
}

// A console descriptor heap the title fills itself: its GPU copy nulls the V# that names unmapped memory and keeps the
// others; the title's own bytes are left as written. After the copy come null slots, which no other allocation takes.
int check_console_heap(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Fence* fence,
                       UINT64& fence_value, const Buffers& b, std::uint64_t unmapped) {
    constexpr UINT slots = 2048;
    constexpr std::uint64_t bytes = std::uint64_t(slots) * kDescriptor;
    auto* title = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    EXPECT(title);
    // Written before the heap exists: the first refresh copies and fixes every slot (no write tracking needed).
    const auto put = [&](int slot, const leonida::gfx10::BufferWords& w) {
        std::memcpy(title + slot * kDescriptor, w.data(), sizeof(w));
    };
    put(0, raw_vsharp(unmapped));
    for (int i = 4; i < 131; ++i) put(i, raw_vsharp(unmapped));
    put(1, raw_vsharp(b.arena_va));
    put(2, raw_vsharp(b.upload_va & 0xFFFFFFFFFFFFull));
    // Slot 3: data the title keeps in a slot, neither a V# (TYPE 4) nor a T#; read as a V# its words name nothing.
    const std::uint32_t data[8] = {0x12345000u, 0x00007777u, 0x00000400u, 0x40000000u, 1, 2, 3, 4};
    std::memcpy(title + 3 * kDescriptor, data, sizeof(data));
    std::uint8_t before[4][kDescriptor];
    for (int i = 0; i < 4; ++i) std::memcpy(before[i], title + i * kDescriptor, kDescriptor);

    const D3D12_DESCRIPTOR_HEAP_DESC desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, slots,
                                          D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    Ref<ID3D12DescriptorHeap> heap;
    EXPECT(SUCCEEDED(rt::d3d12::create_console_descriptor_heap(device, &desc, reinterpret_cast<std::uint64_t>(title),
                                                               bytes, IID_PPV_ARGS(&heap.p))));
    D3D12_DESCRIPTOR_HEAP_DESC got{};
    D3D12_GPU_VIRTUAL_ADDRESS shader_va = 0;
    EXPECT(rt::d3d12::descriptor_heap_info(heap.p, got, shader_va));
    std::uint32_t copy[4][kDescriptor / 4];
    EXPECT(rt::d3d12::read_arena(device, shader_va, copy, sizeof(copy)));
    EXPECT(zero_size(copy[0]));
    EXPECT(!std::memcmp(copy[1], before[1], kDescriptor));
    EXPECT(!std::memcmp(copy[2], before[2], kDescriptor));
    EXPECT(!(copy[3][0] | copy[3][1] | copy[3][2] | copy[3][3]));  // the data slot's descriptor half is null
    for (int i = 0; i < 4; ++i) EXPECT(!std::memcmp(title + i * kDescriptor, before[i], kDescriptor));

    // 128 descriptors wait on one absent range; the title's bytes never change. Publishing its alias fixes every one
    // of them at the next submission (the heap's slot records: an addition over a waiting V#), not one round-robin
    // probe at a time.
    Ref<ID3D12CommandAllocator> allocator;
    Ref<ID3D12GraphicsCommandList> list;
    EXPECT(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
    EXPECT(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, nullptr,
                                               IID_PPV_ARGS(&list.p))));
    EXPECT(SUCCEEDED(list->Close()));
    ID3D12CommandList* lists[] = {list.p};
    const auto submit = [&] {
        queue->ExecuteCommandLists(1, lists);
        return SUCCEEDED(queue->Signal(fence, ++fence_value)) &&
               SUCCEEDED(fence->SetEventOnCompletion(fence_value, nullptr));
    };
    const auto count_restored = [&](unsigned& restored) {
        restored = 0;
        for (unsigned slot = 0; slot < 131; ++slot) {
            if (slot > 0 && slot < 4) continue;
            std::uint32_t words[8];
            if (!rt::d3d12::read_arena(device, shader_va + slot * kDescriptor, words, sizeof(words))) return false;
            if (base_of(words) == b.arena_va && words[2]) ++restored;
            else if (!zero_size(words)) return false;
        }
        return true;
    };
    unsigned restored = 0;
    EXPECT(submit());
    EXPECT(count_restored(restored) && restored == 0);
    leonida::title_va_aliases().add(unmapped, kViewBytes, b.arena_va);
    EXPECT(submit());
    EXPECT(count_restored(restored) && restored == 128);
    // A slot written now with the words the first refresh had no copy for: the refresh that copies the write asks
    // for them again rather than reuse that answer (fix_vsharp's answers last one pass over the heap).
    put(140, raw_vsharp(unmapped));
    EXPECT(submit());
    std::uint32_t rewritten[8];
    EXPECT(rt::d3d12::read_arena(device, shader_va + 140 * kDescriptor, rewritten, sizeof(rewritten)));
    EXPECT(base_of(rewritten) == b.arena_va && rewritten[2]);
    std::printf("128 waiting zero descriptors, all restored at the first submission after their alias came, a "
                "rewritten one at once\n");
    // The alias goes: at the next submission none of them names its copy any more.
    leonida::title_va_aliases().remove(unmapped, b.arena_va);
    EXPECT(submit());
    EXPECT(count_restored(restored) && restored == 0);

    // A released buffer: the slots that named it are null at the next submission, the others keep their V#s. The
    // title's bytes stay as written (each slot is rewritten through its page, as the title does, so the heap's write
    // tracking sees it).
    {
        Ref<ID3D12Resource> gone;
        const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
        const auto gone_desc = buffer_desc(64 * 1024);
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &gone_desc,
                                                         D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&gone.p))));
        const std::uint64_t gone_va = gone->GetGPUVirtualAddress();
        for (int i = 140; i < 150; ++i) put(i, raw_vsharp(gone_va + std::uint64_t(i - 140) * kViewBytes));
        for (int i = 150; i < 160; ++i) put(i, raw_vsharp(b.arena_va));
        EXPECT(submit());
        std::uint32_t words[8];
        for (unsigned slot = 140; slot < 160; ++slot) {
            EXPECT(rt::d3d12::read_arena(device, shader_va + slot * kDescriptor, words, sizeof(words)));
            EXPECT(words[2] && base_of(words) == (slot < 150 ? gone_va + (slot - 140) * kViewBytes : b.arena_va));
        }
        gone.reset();
        // The arena range goes once the device has retired the buffer: a few submissions at most.
        bool nulled = false;
        for (int pass = 0; pass < 8 && !nulled; ++pass) {
            EXPECT(submit());
            nulled = true;
            for (unsigned slot = 140; slot < 150; ++slot) {
                EXPECT(rt::d3d12::read_arena(device, shader_va + slot * kDescriptor, words, sizeof(words)));
                nulled &= zero_size(words);
            }
        }
        EXPECT(nulled);
        for (unsigned slot = 150; slot < 160; ++slot) {
            EXPECT(rt::d3d12::read_arena(device, shader_va + slot * kDescriptor, words, sizeof(words)));
            EXPECT(words[2] && base_of(words) == b.arena_va);
        }
        std::printf("released buffer: the 10 slots that named it are null at the next submissions, 10 others kept\n");
    }

    // The null guard: past the heap's last slot, up to kHeapIndexReach from its start, every byte reads 0.
    constexpr std::uint64_t reach = 64ull << 20;
    std::vector<std::uint8_t> tail(1 << 20);
    for (std::uint64_t at = bytes; at < reach; at += tail.size()) {
        const std::size_t n = std::size_t(std::min<std::uint64_t>(tail.size(), reach - at));
        EXPECT(rt::d3d12::read_arena(device, shader_va + at, tail.data(), n));
        for (std::size_t i = 0; i < n; ++i)
            if (tail[i]) {
                std::fprintf(stderr, "guard byte +0x%llx is 0x%02x\n", static_cast<unsigned long long>(at + i), tail[i]);
                return 1;
            }
    }
    // Reserved with the heap: a buffer made now lies outside the copy and its guard.
    Ref<ID3D12Resource> later;
    const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
    const auto later_desc = buffer_desc(64 * 1024);
    EXPECT(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &later_desc,
                                                     D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&later.p))));
    const std::uint64_t later_va = later->GetGPUVirtualAddress();
    EXPECT(later_va + 64 * 1024 <= shader_va || later_va >= shader_va + reach);
    std::printf("console heap: GPU copy at 0x%llx nulls the unmapped V# and a data slot, keeps arena and upload V#s, title bytes "
                "intact; %llu MiB of null slots after it\n",
                static_cast<unsigned long long>(shader_va), static_cast<unsigned long long>(reach >> 20));
    heap.reset();
    VirtualFree(title, 0, MEM_RELEASE);
    return 0;
}

// A bindless-sized heap of the layer's own (64Ki slots): an index past it reads null slots up to the reach.
int check_layer_heap_guard(ID3D12Device* device) {
    constexpr UINT slots = 65536;
    constexpr std::uint64_t bytes = std::uint64_t(slots) * kDescriptor, reach = 64ull << 20;
    const D3D12_DESCRIPTOR_HEAP_DESC desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, slots,
                                          D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    Ref<ID3D12DescriptorHeap> heap;
    EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap.p))));
    const std::uint64_t base = heap->GetGPUDescriptorHandleForHeapStart().ptr;
    std::vector<std::uint8_t> tail(1 << 20);
    for (std::uint64_t at = bytes; at < bytes + reach; at += tail.size()) {
        EXPECT(rt::d3d12::read_arena(device, base + at, tail.data(), tail.size()));
        for (const std::uint8_t v : tail) EXPECT(v == 0);
    }
    Ref<ID3D12Resource> later;
    const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
    const auto later_desc = buffer_desc(64 * 1024);
    EXPECT(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &later_desc,
                                                     D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&later.p))));
    const std::uint64_t later_va = later->GetGPUVirtualAddress();
    EXPECT(later_va + 64 * 1024 <= base || later_va >= base + bytes + reach);
    std::printf("layer heap: %u slots, then %llu MiB of null slots no other allocation takes\n", slots,
                static_cast<unsigned long long>(reach >> 20));
    return 0;
}

}  // namespace

int main() {
    vktest::Device d;
    const auto status = d.initialize();
    if (status == vktest::DeviceStatus::unavailable) {
        std::fprintf(stderr, "SKIP: no usable Vulkan 1.3 device\n");
        return 77;
    }
    EXPECT(status == vktest::DeviceStatus::ready);
    {
        rt::d3d12::VulkanDeviceInfo info{};
        info.get_instance_proc = d.instance_proc;
        info.instance = d.instance;
        info.physical = d.physical;
        info.device = d.device;
        info.queue = d.queue;
        info.queue_family = d.family;
        info.buffer_device_address = true;
        info.subgroup = d.subgroup;
        Ref<ID3D12Device> device;
        EXPECT(SUCCEEDED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&device.p))));
        const std::uint64_t arena = rt::d3d12::device_arena_size(device.p);
        EXPECT(arena > 0);
        const std::uint64_t unmapped = unmapped_address();
        EXPECT(unmapped > arena && unmapped < (1ull << 47));
        std::uint64_t lo = 0, hi = 0;
        EXPECT(!leonida::title_va_aliases().title_memory(unmapped, lo, hi));

        const D3D12_COMMAND_QUEUE_DESC queue_desc{};
        Ref<ID3D12CommandQueue> queue;
        Ref<ID3D12Fence> fence;
        EXPECT(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))));
        EXPECT(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));
        UINT64 fence_value = 0;
        Buffers b;
        if (make_buffers(device.p, arena, b)) return 1;
        auto& aliases = leonida::title_va_aliases();
        EXPECT(aliases.resolve(unmapped, kViewBytes, leonida::TitleVaAliases::root_view) == 0);
        EXPECT(aliases.resolve(b.upload_va, kViewBytes, leonida::TitleVaAliases::root_view) == b.upload_va);
        if (check_created_views(device.p, b, unmapped)) return 1;
        if (check_inline_table(device.p, queue.p, fence.p, fence_value, b, unmapped)) return 1;
        if (check_console_heap(device.p, queue.p, fence.p, fence_value, b, unmapped)) return 1;
        if (check_layer_heap_guard(device.p)) return 1;
    }
    if (d.validation_errors()) {
        std::fprintf(stderr, "%u validation errors\n", d.validation_errors());
        return 1;
    }
    std::puts("ok");
    return 0;
}
