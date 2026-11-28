// A buffer the title places at its own VA (Xbox device slot 133, rt::d3d12::create_buffer_at_host_address) is read
// by a shader written to the translator's address contract (heap_base + console VA, docs/d3d12_vulkan.md section
// 3.3), through the two kinds of address the layer writes for it:
//   - a root view (SetComputeRootUnorderedAccessView with the title's VA): read 256 bytes past its base and written
//     at its base, then copied to a READBACK buffer;
//   - a buffer V# (CreateShaderResourceView of the title's buffer), the way translated vertex shaders fetch their
//     vertices: read and written to an arena buffer.
// The title's VA is a VirtualAlloc address, far past the 4 GiB arena. Both page kinds are covered: read-write pages
// (imported with VK_EXT_external_memory_host where the driver takes them) and write-combined pages (refused by the
// import, so a shadow copy in the arena, the retail case on an RTX 5070 Ti). The data is written after the buffer
// exists, so the shadow's copy before the submission is what the GPU reads.
// Then the title's own memory outside any slot-133 buffer (a constant ring, #8862 section 3.5): a V# the CPU writes
// into a bound table itself, a created CBV and a root view.
// Negative controls: a second title buffer with other data must not be what the first one's V# reads; an arena
// buffer's V# keeps its VA; the title's range is forgotten when the buffer is released; a raw arena V# and a
// T#-shaped slot in a scanned table keep their bytes; a reserved, uncommitted title address and memory that is nobody's get zero-size views. The shader is test/glsl/cs_user_data.comp. No game data. LEONIDA_VK_VALIDATION=1 for the
// Khronos validation layer.
#include "shader_fixture_path.h"
#include "test_temp_dir.h"
#include "d3d12_test_shaders.h"
#include "vk_test_device.h"
#include "../shims/d3d12_com.h"
#include "../shims/d3d12_xs_device.h"
#include "../src/gfx10_descriptors.h"
#include "../src/shader_library.h"
#include "../src/title_va_alias.h"
#include "../src/title_page_watch.h"
#include "../src/title_geometry_watch.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#ifndef LEONIDA_TEST_CS_SPV
#error "LEONIDA_TEST_CS_SPV must name the compiled test shader"
#endif

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

namespace {

using XboxHeapHandleFn = std::uint64_t(WINAPI*)(void* heap);  // heap slots 9/10: handle in RAX
using XboxComputePipelineFn = HRESULT(WINAPI*)(void* device, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, std::uint64_t,
                                               std::uint64_t, const IID*, void**);

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

void* slot(void* object, unsigned index) { return (*reinterpret_cast<void***>(object))[index]; }

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(std::uint8_t(v >> (8 * i)));
}

std::vector<std::uint8_t> zlib_stored(const std::vector<std::uint8_t>& data) {
    std::vector<std::uint8_t> out = {0x78, 0x01, 0x01};
    const auto length = std::uint16_t(data.size());
    out.insert(out.end(), {std::uint8_t(length), std::uint8_t(length >> 8), std::uint8_t(~length),
                           std::uint8_t(~length >> 8)});
    out.insert(out.end(), data.begin(), data.end());
    std::uint32_t a = 1, b = 0;
    for (const std::uint8_t x : data) {
        a = (a + x) % 65521;
        b = (b + a) % 65521;
    }
    const std::uint32_t adler = (b << 16) | a;
    out.insert(out.end(), {std::uint8_t(adler >> 24), std::uint8_t(adler >> 16), std::uint8_t(adler >> 8),
                           std::uint8_t(adler)});
    return out;
}

// The game's shader container shape (DXBC > DXIL > XDAT 0x0D > XBCH > zlib > FSSM), as d3d12_compute_test builds it.
std::vector<std::uint8_t> shader_container(const std::vector<std::uint8_t>& fssm) {
    const auto stream = zlib_stored(fssm);
    std::vector<std::uint8_t> xbch = {'X', 'B', 'C', 'H'};
    for (const std::uint32_t v : {1u, std::uint32_t(stream.size()), 0u, std::uint32_t(fssm.size()), 0u}) put32(xbch, v);
    xbch.insert(xbch.end(), stream.begin(), stream.end());
    std::vector<std::uint8_t> dxil(16, 0);
    dxil.insert(dxil.end(), {'X', 'D', 'A', 'T'});
    put32(dxil, 1);
    put32(dxil, 12);
    put32(dxil, 0x0d);
    put32(dxil, 16 + 12 + 12);
    put32(dxil, std::uint32_t(xbch.size()));
    dxil.insert(dxil.end(), xbch.begin(), xbch.end());
    std::vector<std::uint8_t> out = {'D', 'X', 'B', 'C'};
    out.resize(28, 0);
    put32(out, 1);
    put32(out, 36);
    out.insert(out.end(), {'D', 'X', 'I', 'L'});
    put32(out, std::uint32_t(dxil.size()));
    out.insert(out.end(), dxil.begin(), dxil.end());
    return out;
}

// The shared helper: Windows 10's in-box d3d12.dll refuses the heap-directly-indexed flags at every version.
std::vector<std::uint8_t> serialize(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC& desc) {
    return d3d12test::serialize_root_signature(desc);
}

D3D12_RESOURCE_DESC buffer_desc(UINT64 bytes, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    return desc;
}

// The 48-bit base address of the 16-byte GFX10 buffer V# at `cpu` (dword 0, and dword 1 bits 0-15).
std::uint64_t vsharp_base(std::uint64_t cpu) {
    std::uint32_t w[2];
    std::memcpy(w, reinterpret_cast<const void*>(cpu), sizeof(w));
    return std::uint64_t(w[0]) | (std::uint64_t(w[1] & 0xFFFFu) << 32);
}

D3D12_SHADER_RESOURCE_VIEW_DESC raw_srv(UINT first_word, UINT words) {
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Buffer.FirstElement = first_word;
    desc.Buffer.NumElements = words;
    desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    return desc;
}

constexpr UINT kElements = 64, kBytes = kElements * 4;
constexpr UINT64 kPages = 64 * 1024;
constexpr UINT kSrvWord = 256;   // the V# reads title words 256..319
constexpr UINT kRootAdd = 7;     // root view mode: title word i = title word 64 + i + 7

struct Context {
    ID3D12Device* inner = nullptr;  // the Vulkan device (slot 133's backend takes it)
    ID3D12Device* device = nullptr;  // the Xbox-ABI proxy over it
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Fence* fence = nullptr;
    UINT64 fence_value = 0;
    std::uint64_t arena = 0;
};

// One kind of title pages: `protect` is PAGE_READWRITE, or with PAGE_WRITECOMBINE.
int check_kind(Context& c, const char* kind, DWORD protect, int& checks) {
    void* pages = VirtualAlloc(nullptr, kPages, MEM_TOP_DOWN | MEM_RESERVE | MEM_COMMIT, protect);
    void* other_pages = VirtualAlloc(nullptr, kPages, MEM_TOP_DOWN | MEM_RESERVE | MEM_COMMIT, protect);
    EXPECT(pages && other_pages);
    const auto title_va = reinterpret_cast<std::uint64_t>(pages);
    EXPECT(title_va >= c.arena);  // outside the arena: heap_base + title_va names nothing of the layer's
    const D3D12_RESOURCE_DESC desc = buffer_desc(kPages);
    Ref<ID3D12Resource> title, other;
    EXPECT(SUCCEEDED(rt::d3d12::create_buffer_at_host_address(c.inner, pages, desc, 0xFFFFFFFFu,
                                                              IID_PPV_ARGS(&title.p))));
    EXPECT(SUCCEEDED(rt::d3d12::create_buffer_at_host_address(c.inner, other_pages, desc, 0xFFFFFFFFu,
                                                              IID_PPV_ARGS(&other.p))));
    EXPECT(title->GetGPUVirtualAddress() == title_va);  // the title keeps its own VA
    const std::uint64_t shader_va = leonida::title_va_aliases().shader_va(title_va);
    EXPECT(shader_va != title_va);
    const bool arena_copy = shader_va < c.arena;
    const bool vsharp_fits = (shader_va >> 48) == 0;

    // The data, written after the buffers exist.
    auto* words = static_cast<std::uint32_t*>(pages);
    auto* other_words = static_cast<std::uint32_t*>(other_pages);
    for (UINT i = 0; i < kPages / 4; ++i) {
        words[i] = 0xA0000000u + i * 3u;
        other_words[i] = 0xB0000000u + i * 5u;
    }

    // Descriptors: the title buffer's V# (and the other buffer's), and the negative controls.
    const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8,
                                               D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    Ref<ID3D12DescriptorHeap> heap;
    EXPECT(SUCCEEDED(c.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap.p))));
    const std::uint64_t cpu = reinterpret_cast<XboxHeapHandleFn>(slot(heap.p, 9))(heap.p);
    const std::uint64_t gpu = reinterpret_cast<XboxHeapHandleFn>(slot(heap.p, 10))(heap.p);
    const UINT inc = c.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const D3D12_SHADER_RESOURCE_VIEW_DESC srv = raw_srv(kSrvWord, kElements);
    c.device->CreateShaderResourceView(title.p, &srv, D3D12_CPU_DESCRIPTOR_HANDLE{cpu});
    c.device->CreateShaderResourceView(other.p, &srv, D3D12_CPU_DESCRIPTOR_HANDLE{cpu + inc});
    EXPECT(vsharp_base(cpu) == ((shader_va + kSrvWord * 4) & 0xFFFFFFFFFFFFull));

    // An arena buffer for the V# read's result, a READBACK buffer for both results.
    const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT}, readback_heap{D3D12_HEAP_TYPE_READBACK};
    Ref<ID3D12Resource> out, readback;
    const auto uav = buffer_desc(kBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    EXPECT(SUCCEEDED(c.device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &uav,
                                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                       IID_PPV_ARGS(&out.p))));
    const auto readback_desc = buffer_desc(3 * kBytes);
    EXPECT(SUCCEEDED(c.device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(&readback.p))));
    // An arena buffer's V# keeps its VA: identity outside title-placed buffers.
    const D3D12_CONSTANT_BUFFER_VIEW_DESC arena_cbv{out->GetGPUVirtualAddress(), 256};
    c.device->CreateConstantBufferView(&arena_cbv, D3D12_CPU_DESCRIPTOR_HANDLE{cpu + 3 * inc});
    EXPECT(vsharp_base(cpu + 3 * inc) == out->GetGPUVirtualAddress());

    Ref<ID3D12CommandAllocator> allocator;
    Ref<ID3D12GraphicsCommandList> list;
    EXPECT(SUCCEEDED(c.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
    EXPECT(SUCCEEDED(c.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, c.pso,
                                                 IID_PPV_ARGS(&list.p))));
    ID3D12DescriptorHeap* heaps[] = {heap.p};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(c.root);
    // Through the V# (buffer SRV mode, s5 = 0xFFFFFFFF), into the arena buffer. Skipped when the buffer's copy is
    // not within a V#'s 48 bits of heap_base (an import the driver put below the arena), which the layer logs.
    if (vsharp_fits) {
        list->SetComputeRoot32BitConstant(0, 0, 0);
        list->SetComputeRoot32BitConstant(0, 0xFFFFFFFFu, 1);
        list->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
        list->SetComputeRootDescriptorTable(2, D3D12_GPU_DESCRIPTOR_HANDLE{gpu});
        list->Dispatch(1, 1, 1);
    }
    // Through the root view: the title's own VA, read at +256 bytes and written at +0.
    list->SetComputeRoot32BitConstant(0, kRootAdd, 0);
    list->SetComputeRoot32BitConstant(0, 0xFFFFFFFEu, 1);
    list->SetComputeRootUnorderedAccessView(1, title_va);
    list->Dispatch(1, 1, 1);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    list->ResourceBarrier(1, &barrier);
    D3D12_RESOURCE_BARRIER to_copy{};
    to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_copy.Transition.pResource = out.p;
    to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &to_copy);
    list->CopyBufferRegion(readback.p, 0, out.p, 0, kBytes);
    list->CopyBufferRegion(readback.p, kBytes, title.p, 0, kBytes);
    EXPECT(list->Close() == S_OK);
    ID3D12CommandList* lists[] = {list.p};
    c.queue->ExecuteCommandLists(1, lists);
    EXPECT(rt::d3d12::last_execute_result(c.queue) == leonida::ReplayResult::success);
    EXPECT(SUCCEEDED(c.queue->Signal(c.fence, ++c.fence_value)));
    EXPECT(SUCCEEDED(c.fence->SetEventOnCompletion(c.fence_value, nullptr)));

    void* mapped = nullptr;
    EXPECT(SUCCEEDED(readback->Map(0, nullptr, &mapped)));
    const auto* got = static_cast<const std::uint32_t*>(mapped);
    UINT srv_ok = 0, srv_other = 0, root_ok = 0;
    for (UINT i = 0; i < kElements; ++i) {
        if (got[i] == words[kSrvWord + i]) ++srv_ok;
        if (got[i] == other_words[kSrvWord + i]) ++srv_other;
        if (got[kElements + i] == words[64 + i] + kRootAdd) ++root_ok;
    }
    readback->Unmap(0, nullptr);
    std::printf("%s pages: GPU copy %s (shader VA %s); V# read %s, %u/%u words matched, %u/%u read the other "
                "buffer's (negative control); root view read+write %u/%u matched\n",
                kind, arena_copy ? "in the arena" : "with its own device address",
                arena_copy ? "an arena offset" : "device address - heap_base",
                vsharp_fits ? "run" : "skipped (out of a V#'s 48 bits)", vsharp_fits ? srv_ok : 0, kElements,
                vsharp_fits ? srv_other : 0, kElements, root_ok, kElements);
    if (vsharp_fits) {
        EXPECT(srv_ok == kElements);
        EXPECT(srv_other == 0);
    }
    EXPECT(root_ok == kElements);

    // After release, the buffer's range is gone from the table: its old copy is not named any more.
    title.reset();
    EXPECT(leonida::title_va_aliases().shader_va(title_va) == title_va);
    checks += 2;
    other.reset();
    heap.reset();
    VirtualFree(pages, 0, MEM_RELEASE);
    VirtualFree(other_pages, 0, MEM_RELEASE);
    return 0;
}

// With imports on: a slot-133 buffer the layer does not import holds no import memory once
// create_buffer_at_host_address has returned, whichever step declined it. The memory an import declined after its
// allocation (one the driver put below the arena) used to wait in the deferred queue past the next submission, while
// the shadow made instead over the same pages made them read-only: RADV then rejects every submission (found and
// measured by NateWeav, #14). Two placements: at a page start (imported where the driver takes it) and 4 bytes into
// a page (declined after the buffer exists wherever the buffer's alignment is above 4). Then a submission over the
// shadow's pages, which must still succeed.
int check_declined_imports(Context& c) {
    const std::uint64_t before = rt::d3d12::title_host_imports(c.inner);
    void* pages = VirtualAlloc(nullptr, kPages, MEM_TOP_DOWN | MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    EXPECT(pages);
    const D3D12_RESOURCE_DESC desc = buffer_desc(kPages / 2);
    int shadows = 0;
    for (const std::size_t at : {std::size_t(0), std::size_t(4)}) {
        Ref<ID3D12Resource> buffer;
        EXPECT(SUCCEEDED(rt::d3d12::create_buffer_at_host_address(c.inner, static_cast<std::uint8_t*>(pages) + at,
                                                                  desc, 0xFFFFFFFFu, IID_PPV_ARGS(&buffer.p))));
        const bool shadow = rt::d3d12::title_shadow_buffer(buffer.p);
        shadows += shadow;
        const std::uint64_t live = rt::d3d12::title_host_imports(c.inner);
        std::printf("slot-133 buffer at page offset %zu: %s, %llu import(s) live\n", at,
                    shadow ? "declined (shadow)" : "imported", static_cast<unsigned long long>(live - before));
        EXPECT(live == before + (shadow ? 0 : 1));
        // A submission with the buffer alive: the shadow's tracker guards its pages here.
        Ref<ID3D12CommandAllocator> allocator;
        Ref<ID3D12GraphicsCommandList> list;
        EXPECT(SUCCEEDED(c.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                          IID_PPV_ARGS(&allocator.p))));
        EXPECT(SUCCEEDED(c.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, nullptr,
                                                     IID_PPV_ARGS(&list.p))));
        EXPECT(list->Close() == S_OK);
        ID3D12CommandList* lists[] = {list.p};
        c.queue->ExecuteCommandLists(1, lists);
        EXPECT(rt::d3d12::last_execute_result(c.queue) == leonida::ReplayResult::success);
        EXPECT(SUCCEEDED(c.queue->Signal(c.fence, ++c.fence_value)));
        EXPECT(SUCCEEDED(c.fence->SetEventOnCompletion(c.fence_value, nullptr)));
        buffer.reset();
        EXPECT(rt::d3d12::title_host_imports(c.inner) == before);
    }
    VirtualFree(pages, 0, MEM_RELEASE);
    std::printf("declined imports: %d of 2 placements took the shadow, none left import memory behind\n", shadows);
    return 0;
}

// With imports on: a page a shadow tracker watches and a live import shares stays writable across submissions. RADV
// rejects every submission once a page under a live import is read-only (found and measured by NateWeav, #14), so an
// import pins its pages (leonida::kernel_writes::hold) and the tracker passes over them. The shadow comes first: its
// pages are read-only when the buffer is made, so the layer does not import them. Made writable again, the tracker
// guards them at the next submission (the control); written, they are open again; then an import over the same pages,
// and two submissions that must leave them writable.
int check_import_over_shadow(Context& c) {
    auto* pages = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, kPages, MEM_TOP_DOWN | MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    EXPECT(pages);
    const auto protection = [&] {
        MEMORY_BASIC_INFORMATION region{};
        return VirtualQuery(pages, &region, sizeof(region)) ? region.Protect & 0xFF : 0;
    };
    const auto submit = [&]() -> int {
        Ref<ID3D12CommandAllocator> allocator;
        Ref<ID3D12GraphicsCommandList> list;
        EXPECT(SUCCEEDED(c.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                          IID_PPV_ARGS(&allocator.p))));
        EXPECT(SUCCEEDED(c.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, nullptr,
                                                     IID_PPV_ARGS(&list.p))));
        EXPECT(list->Close() == S_OK);
        ID3D12CommandList* lists[] = {list.p};
        c.queue->ExecuteCommandLists(1, lists);
        EXPECT(rt::d3d12::last_execute_result(c.queue) == leonida::ReplayResult::success);
        EXPECT(SUCCEEDED(c.queue->Signal(c.fence, ++c.fence_value)));
        EXPECT(SUCCEEDED(c.fence->SetEventOnCompletion(c.fence_value, nullptr)));
        return 0;
    };
    const D3D12_RESOURCE_DESC desc = buffer_desc(kPages);
    DWORD old = 0;
    EXPECT(VirtualProtect(pages, kPages, PAGE_READONLY, &old));
    Ref<ID3D12Resource> shadow, import;
    EXPECT(SUCCEEDED(rt::d3d12::create_buffer_at_host_address(c.inner, pages, desc, 0xFFFFFFFFu,
                                                              IID_PPV_ARGS(&shadow.p))));
    EXPECT(rt::d3d12::title_shadow_buffer(shadow.p));
    EXPECT(VirtualProtect(pages, kPages, PAGE_READWRITE, &old));
    if (submit()) return 1;
    const bool guarded = protection() == PAGE_READONLY;
    pages[0] = 1;  // the tracker's fault handler opens the page again
    EXPECT(protection() == PAGE_READWRITE);
    const std::uint64_t imports = rt::d3d12::title_host_imports(c.inner);
    EXPECT(SUCCEEDED(rt::d3d12::create_buffer_at_host_address(c.inner, pages, desc, 0xFFFFFFFFu,
                                                              IID_PPV_ARGS(&import.p))));
    const bool imported = !rt::d3d12::title_shadow_buffer(import.p);
    std::size_t pinned = 0;
    const auto begin = reinterpret_cast<std::uintptr_t>(pages);
    leonida::kernel_writes::for_each_pin(begin, begin + kPages,
                                         [&](std::uintptr_t from, std::uintptr_t to) { pinned += to - from; });
    std::printf("import over a shadow's pages: the tracker %s them alone; the second buffer %s, %zu KiB pinned\n",
                guarded ? "guarded" : "did not guard", imported ? "imported" : "declined (shadow)", pinned >> 10);
    EXPECT(guarded);  // the control: without a pin the tracker makes these pages read-only
    if (!imported) {
        EXPECT(pinned == 0 && rt::d3d12::title_host_imports(c.inner) == imports);
        import.reset();
        shadow.reset();
        VirtualFree(pages, 0, MEM_RELEASE);
        return 0;
    }
    EXPECT(pinned >= kPages && rt::d3d12::title_host_imports(c.inner) == imports + 1);
    pages[1] = 2;  // written again: the tracker would guard these pages at the next refresh
    for (int i = 0; i < 2; ++i) {
        if (submit()) return 1;
        EXPECT(protection() == PAGE_READWRITE);
    }
    import.reset();
    pinned = 0;
    leonida::kernel_writes::for_each_pin(begin, begin + kPages,
                                         [&](std::uintptr_t from, std::uintptr_t to) { pinned += to - from; });
    EXPECT(pinned == 0);  // the import's memory is freed: its pin ended
    pages[2] = 3;
    if (submit()) return 1;
    EXPECT(protection() == PAGE_READONLY);  // and the tracker guards the pages again
    shadow.reset();
    EXPECT(protection() == PAGE_READWRITE);
    VirtualFree(pages, 0, MEM_RELEASE);
    return 0;
}

// The title's own memory outside any slot-133 buffer: a VirtualAlloc range standing for its per-frame constant ring
// (#8862 section 3.5). Three ways a shader is given an address in it, each read back through an arena buffer:
//   - a buffer V# the CPU writes into a shader-visible heap itself, as the title does (rewritten before the submission);
//   - a V# made by CreateConstantBufferView (translated when written);
//   - a root view (translated when set).
// Negative controls: a raw V# whose base is an arena buffer and a T#-shaped slot whose base is the ring keep their
// bytes; committed memory that is not the title's, and a title range with no committed page, are left alone.
// Two CPU addresses of one mapped physical tile. The runtime gets this information from XMem in production.
static std::uintptr_t source_tile, peer_tile;
void aliases_of_test_tile(std::uintptr_t va, std::vector<std::uintptr_t>& peers) {
    if (va == source_tile) peers.push_back(peer_tile);
}
// LEONIDA_TITLE_WINDOWS_MB=0: no window fits the cap. A view of the title's memory must then never name the title's
// raw VA (one GPU read of that loses the device, #1537). The pages here are private, uncopied memory, so no geometry
// copy backs them either: both views are written zero-size (no base, no records), and a dispatch through one reads
// zeros, not the title's bytes.
int check_window_overflow(Context& c) {
    constexpr UINT64 ring_bytes = 256 * 1024;
    auto* ring = static_cast<std::uint32_t*>(VirtualAlloc(nullptr, ring_bytes, MEM_TOP_DOWN | MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    EXPECT(ring);
    const auto ring_va = reinterpret_cast<std::uint64_t>(ring);
    EXPECT(ring_va >= c.arena && !(ring_va & 0xFFFF));
    for (UINT i = 0; i < ring_bytes / 4; ++i) ring[i] = 0xC0000000u + i * 7u;  // never zero
    auto& aliases = leonida::title_va_aliases();
    aliases.add_title_memory(ring_va, ring_bytes);

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8,
                                         D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    Ref<ID3D12DescriptorHeap> heap;
    EXPECT(SUCCEEDED(c.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap.p))));
    const std::uint64_t cpu = reinterpret_cast<XboxHeapHandleFn>(slot(heap.p, 9))(heap.p);
    const std::uint64_t gpu = reinterpret_cast<XboxHeapHandleFn>(slot(heap.p, 10))(heap.p);
    const UINT inc = c.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const D3D12_CONSTANT_BUFFER_VIEW_DESC first{ring_va + 0x2000, 256}, second{ring_va + 0x30000, 256};
    c.device->CreateConstantBufferView(&first, D3D12_CPU_DESCRIPTOR_HANDLE{cpu});
    c.device->CreateConstantBufferView(&second, D3D12_CPU_DESCRIPTOR_HANDLE{cpu + inc});
    const auto a = vsharp_base(cpu), b = vsharp_base(cpu + inc);
    const auto records = [](std::uint64_t at) {
        std::uint32_t w[4];
        std::memcpy(w, reinterpret_cast<const void*>(at), sizeof(w));
        return w[2];
    };
    const auto named = [&](std::uint64_t base, std::uint64_t raw) {
        return base == raw ? "the raw VA" : !base ? "nothing (zero-size)" : base < c.arena ? "the arena" : "elsewhere";
    };
    std::printf("window cap 0: views of the title's memory named %s and %s\n", named(a, first.BufferLocation),
                named(b, second.BufferLocation));
    EXPECT(a != first.BufferLocation && b != second.BufferLocation);
    EXPECT(a == 0 && records(cpu) == 0);
    EXPECT(b == 0 && records(cpu + inc) == 0);
    EXPECT(aliases.shader_va(first.BufferLocation) == first.BufferLocation);  // no window was made

    const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT}, readback_heap{D3D12_HEAP_TYPE_READBACK};
    const auto uav = buffer_desc(kBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const auto readback_desc = buffer_desc(kBytes);
    Ref<ID3D12Resource> out, readback;
    EXPECT(SUCCEEDED(c.device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &uav,
                                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                       IID_PPV_ARGS(&out.p))));
    EXPECT(SUCCEEDED(c.device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(&readback.p))));
    Ref<ID3D12CommandAllocator> allocator;
    Ref<ID3D12GraphicsCommandList> list;
    EXPECT(SUCCEEDED(c.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
    EXPECT(SUCCEEDED(c.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, c.pso,
                                                 IID_PPV_ARGS(&list.p))));
    ID3D12DescriptorHeap* heaps[] = {heap.p};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(c.root);
    list->SetComputeRoot32BitConstant(0, 0, 0);  // the CBV mode of check_title_memory: its first four words
    list->SetComputeRoot32BitConstant(0, 0, 1);
    list->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
    list->SetComputeRootDescriptorTable(2, D3D12_GPU_DESCRIPTOR_HANDLE{gpu});
    list->Dispatch(1, 1, 1);
    D3D12_RESOURCE_BARRIER to_copy{};
    to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_copy.Transition.pResource = out.p;
    to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &to_copy);
    list->CopyBufferRegion(readback.p, 0, out.p, 0, kBytes);
    EXPECT(list->Close() == S_OK);
    ID3D12CommandList* lists[] = {list.p};
    c.queue->ExecuteCommandLists(1, lists);
    EXPECT(rt::d3d12::last_execute_result(c.queue) == leonida::ReplayResult::success);
    EXPECT(SUCCEEDED(c.queue->Signal(c.fence, ++c.fence_value)));
    EXPECT(SUCCEEDED(c.fence->SetEventOnCompletion(c.fence_value, nullptr)));
    void* mapped = nullptr;
    EXPECT(SUCCEEDED(readback->Map(0, nullptr, &mapped)));
    const auto* got = static_cast<const std::uint32_t*>(mapped);
    UINT zeros = 0;
    for (UINT i = 0; i < kElements; ++i) zeros += got[i] == 0;
    readback->Unmap(0, nullptr);
    std::printf("window cap 0: a dispatch through the zero-size view read %u/%u zero words\n", zeros, kElements);
    EXPECT(zeros == kElements);
    aliases.remove_title_memory(ring_va);
    VirtualFree(ring, 0, MEM_RELEASE);
    return 0;
}
// What the GPU writes into a title window goes back to the title's pages (#1537, the WRITE_INVALID stop: a window is
// a read-side mirror, so a UAV write used to land in the copy and the title never saw it). The shader's write is read
// back through the pages themselves; then a write by the title must survive the next submission's forward copy.
struct WriteBack {
    Context* c;
    ID3D12CommandAllocator* allocator;
    ID3D12GraphicsCommandList* list;
    ID3D12Resource* out;
    ID3D12DescriptorHeap* heap;
    UINT64 gpu;
    ID3D12CommandQueue* queue = nullptr;  // the context's when null
};

// One submission that dispatches through a root UAV at `va` (and at `va2` too, when given) and waits for it. With
// `wait` false it is only executed (no signal: the caller knows when it is done). False on a failed call.
int submit_write(WriteBack& s, std::uint64_t va, std::uint64_t va2 = 0, bool wait = true) {
    EXPECT(s.allocator->Reset() == S_OK);
    EXPECT(s.list->Reset(s.allocator, s.c->pso) == S_OK);
    ID3D12DescriptorHeap* heaps[] = {s.heap};
    s.list->SetDescriptorHeaps(1, heaps);
    s.list->SetComputeRootSignature(s.c->root);
    s.list->SetComputeRoot32BitConstant(0, 0, 0);
    s.list->SetComputeRoot32BitConstant(0, 1, 1);  // the step: a word the shader writes
    s.list->SetComputeRootUnorderedAccessView(1, va);
    s.list->SetComputeRootDescriptorTable(2, D3D12_GPU_DESCRIPTOR_HANDLE{s.gpu});
    s.list->Dispatch(1, 1, 1);
    if (va2) {
        s.list->SetComputeRootUnorderedAccessView(1, va2);
        s.list->Dispatch(1, 1, 1);
    }
    D3D12_RESOURCE_BARRIER to_copy{};
    to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_copy.Transition.pResource = s.out;
    to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    s.list->ResourceBarrier(1, &to_copy);
    EXPECT(s.list->Close() == S_OK);
    ID3D12CommandList* lists[] = {s.list};
    ID3D12CommandQueue* queue = s.queue ? s.queue : s.c->queue;
    queue->ExecuteCommandLists(1, lists);
    EXPECT(rt::d3d12::last_execute_result(queue) == leonida::ReplayResult::success);
    if (!wait) return 0;
    EXPECT(SUCCEEDED(queue->Signal(s.c->fence, ++s.c->fence_value)));
    s.c->fence->SetEventOnCompletion(s.c->fence_value, nullptr);  // a null event waits, as D3D12 does
    return 0;
}

// The same gap on a buffer the title places at its own address (slot 133, the retail case for a compute result it
// reads back): the pages are not imported here, so the buffer is a shadow, and the write of a root UAV over the
// title's VA must reach the pages at the next submission - and the title's own newer words must still win.
int check_placed_write_back(Context& c, bool geometry) {
    void* pages = VirtualAlloc(nullptr, kPages, MEM_TOP_DOWN | MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    EXPECT(pages);
    const auto title_va = reinterpret_cast<std::uint64_t>(pages);
    auto* words = static_cast<std::uint32_t*>(pages);
    for (UINT i = 0; i < kPages / 4; ++i) words[i] = 0xA0000000u + i * 3u;  // never zero, unlike the shader's rows
    auto& aliases = leonida::title_va_aliases();
    aliases.add_title_memory(title_va, kPages);  // what the xmem shims do for the title's allocations
    const D3D12_RESOURCE_DESC desc = buffer_desc(kPages);
    Ref<ID3D12Resource> placed;
    EXPECT(SUCCEEDED(
        rt::d3d12::create_buffer_at_host_address(c.inner, pages, desc, 0xFFFFFFFFu, IID_PPV_ARGS(&placed.p))));
    EXPECT(placed->GetGPUVirtualAddress() == title_va);
    EXPECT(aliases.shader_va(title_va) != title_va);  // there is a copy to be reached through

    const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT}, readback_heap{D3D12_HEAP_TYPE_READBACK};
    const auto uav = buffer_desc(kBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const auto readback_desc = buffer_desc(kBytes);
    Ref<ID3D12Resource> out, readback;
    EXPECT(SUCCEEDED(c.device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &uav,
                                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                       IID_PPV_ARGS(&out.p))));
    EXPECT(SUCCEEDED(c.device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(&readback.p))));
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8,
                                         D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    Ref<ID3D12DescriptorHeap> heap;
    EXPECT(SUCCEEDED(c.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap.p))));
    Ref<ID3D12CommandAllocator> allocator;
    Ref<ID3D12GraphicsCommandList> list;
    EXPECT(SUCCEEDED(c.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
    EXPECT(SUCCEEDED(c.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, c.pso,
                                                 IID_PPV_ARGS(&list.p))));
    EXPECT(list->Close() == S_OK);
    WriteBack s{&c, allocator.p, list.p, out.p, heap.p,
                reinterpret_cast<XboxHeapHandleFn>(slot(heap.p, 10))(heap.p)};

    const std::vector<std::uint32_t> before(words, words + kElements);
    EXPECT(submit_write(s, title_va) == 0);
    UINT untouched = 0;
    for (UINT i = 0; i < kElements; ++i) untouched += words[i] == before[i];
    // An imported buffer is the same memory and needs no copy; only a shadow owes the pages the GPU's words.
    const bool shadow = !geometry && aliases.shader_va(title_va) >= c.arena;
    if (shadow) {
        std::printf("placed buffer write back: %u/%u words still held the title's before the next submission\n",
                    untouched, kElements);
        EXPECT(untouched == kElements);
    }

    EXPECT(submit_write(s, title_va) == 0);
    std::vector<std::uint32_t> written(words, words + kElements);
    UINT reached = 0, changed = 0, nonzero = 0;
    for (UINT i = 0; i < kElements; ++i) {
        reached += written[i] == (0xA0000000u + i * 3u);
        changed += written[i] != before[i];
        nonzero += written[i] != 0;
    }
    std::printf("placed buffer write back: %u/%u words reached the pages (%u changed, %u nonzero)\n", kElements - reached,
                kElements, changed, nonzero);
    EXPECT(changed == kElements && nonzero > 0);
    (void)reached;

    // The title writes its own words over the result: the next submission copies them forward and they stay.
    for (UINT i = 0; i < kElements; ++i) words[i] = 0xB16B00B5u;
    EXPECT(submit_write(s, title_va) == 0);
    UINT kept = 0;
    for (UINT i = 0; i < kElements; ++i) kept += words[i] == 0xB16B00B5u;
    std::printf("placed buffer write back: the title's own words survived %u/%u\n", kept, kElements);
    EXPECT(kept == kElements);

    placed.reset();  // the shadow goes, the pages stay writable
    aliases.remove_title_memory(title_va);
    VirtualFree(pages, 0, MEM_RELEASE);
    return 0;
}

int check_window_write_back(Context& c, bool geometry) {
    constexpr UINT64 ring_bytes = 256 * 1024;
    auto* ring = static_cast<std::uint32_t*>(
        VirtualAlloc(nullptr, ring_bytes, MEM_TOP_DOWN | MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    EXPECT(ring);
    const auto ring_va = reinterpret_cast<std::uint64_t>(ring);
    for (UINT i = 0; i < ring_bytes / 4; ++i) ring[i] = 0;
    auto& aliases = leonida::title_va_aliases();
    aliases.add_title_memory(ring_va, ring_bytes);

    const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT}, readback_heap{D3D12_HEAP_TYPE_READBACK};
    const auto uav = buffer_desc(kBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const auto readback_desc = buffer_desc(kBytes);
    Ref<ID3D12Resource> out, readback;
    EXPECT(SUCCEEDED(c.device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &uav,
                                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                       IID_PPV_ARGS(&out.p))));
    EXPECT(SUCCEEDED(c.device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(&readback.p))));
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8,
                                         D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    Ref<ID3D12DescriptorHeap> heap;
    EXPECT(SUCCEEDED(c.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap.p))));
    Ref<ID3D12CommandAllocator> allocator;
    Ref<ID3D12GraphicsCommandList> list;
    EXPECT(SUCCEEDED(c.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
    EXPECT(SUCCEEDED(c.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, c.pso,
                                                 IID_PPV_ARGS(&list.p))));
    EXPECT(list->Close() == S_OK);  // Reset needs it closed, as the title leaves it
    WriteBack s{&c, allocator.p, list.p, out.p, heap.p,
                reinterpret_cast<XboxHeapHandleFn>(slot(heap.p, 10))(heap.p)};

    // The shader writes the range the root UAV names, which is the title's own pages here, far past the arena: the
    // resolve already has to make a window for them before the submission.
    EXPECT(aliases.shader_va(ring_va) == ring_va);
    EXPECT(submit_write(s, ring_va) == 0);
    EXPECT(aliases.shader_va(ring_va) != ring_va);  // a window was made for the title's pages
    if (!geometry) EXPECT(ring[0] == 0);  // the copy took the write: the pages get it only after the next submission

    // The next submission's refresh copies the GPU's writes back, before it copies the pages forward.
    EXPECT(submit_write(s, ring_va) == 0);
    const std::vector<std::uint32_t> written(ring, ring + kElements);
    UINT nonzero = 0;
    for (std::uint32_t w : written) nonzero += w != 0;
    std::printf("write back: %u of %u words the shader wrote are in the title's pages\n", nonzero, kElements);
    EXPECT(nonzero > 0);

    // The same submission again: the pages keep what they hold, the values do not drift.
    EXPECT(submit_write(s, ring_va) == 0);
    UINT stable = 0;
    for (UINT i = 0; i < kElements; ++i) stable += ring[i] == written[i];
    std::printf("write back: the pages kept those words across two submissions %u/%u\n", stable, kElements);
    EXPECT(stable == kElements);

    // A write by the title after a submission is copied forward over the result again, not overwritten by it.
    for (UINT i = 0; i < kElements; ++i) ring[i] = 0xB16B00B5u;
    EXPECT(submit_write(s, ring_va) == 0);
    UINT kept = 0;
    for (UINT i = 0; i < kElements; ++i) kept += ring[i] == 0xB16B00B5u;
    std::printf("write back: the title's own words survived the next submission %u/%u\n", kept, kElements);
    EXPECT(kept == kElements);

    aliases.remove_title_memory(ring_va);
    VirtualFree(ring, 0, MEM_RELEASE);
    return 0;
}

// A write-back whose copy faults (#2418). The copy runs under its tracker's lock and can land on a page another guard
// keeps read-only: here a hot page (write-watched, so this tracker does not guard it) that a geometry watch cell has
// made read-only, as a geometry view over the same pages does in the title. The copy faults, the handler reopens the
// cell and asks every tracker over the address, the writing one too; that one used to take its own (non-recursive)
// lock again on the same thread and the submission never returned. A watchdog ends the process if it does not.
//   1. the ring is write-watched and the first submission makes its window: the tracker guards the pages;
//   2. the title writes the first page outside the words the shader writes: the fault makes its block hot (writable,
//      asked about rather than guarded), and the next submission drops the GPU's words there (the page was written)
//      and copies the page forward again;
//   3. a geometry cell over the ring's first 64 KiB makes the hot page read-only, unknown to the tracker;
//   4. the next submission's write-back faults on that page, and must return with the GPU's words in the pages.
// With `same_tile`, a second run in the same 64 KiB tile: step 2's submission dispatches at the third page too, and
// the title then writes the second page (hot, so no fault: the write watch has it). Step 4's write-back (of step 2's
// results) then runs over [page 0] (faults; the geometry cell reopens the whole tile, and the tracker's pages there
// with it), [page 1] (the title's: held) and [page 2 ...]. The tile's reopening on the write-back's own thread used to
// make page 2 count as written by the title, so its GPU words were dropped and the forward copy put the title's zeros
// over them. The title's word on page 1 must still win.
int check_window_write_back_fault(Context& c, bool same_tile) {
    if (!leonida::title_write_watch::enabled()) {
        std::fprintf(stderr, "SKIP: write watching is off (LEONIDA_WRITE_WATCH=0, Wine, or no host heap)\n");
        return 77;
    }
    constexpr UINT64 ring_bytes = 256 * 1024;
    auto* ring = static_cast<std::uint32_t*>(
        VirtualAlloc(nullptr, ring_bytes, MEM_TOP_DOWN | MEM_RESERVE | MEM_WRITE_WATCH, PAGE_READWRITE));
    EXPECT(ring && VirtualAlloc(ring, ring_bytes, MEM_COMMIT, PAGE_READWRITE) == ring);
    const auto ring_va = reinterpret_cast<std::uint64_t>(ring);
    EXPECT((ring_va & (leonida::geometry_writes::tile_size - 1)) == 0);  // a reservation starts on 64 KiB
    leonida::title_write_watch::add(ring_va, ring_bytes);  // as xmem records it
    auto& aliases = leonida::title_va_aliases();
    aliases.add_title_memory(ring_va, ring_bytes);
    const auto protection = [](const void* p) {
        MEMORY_BASIC_INFORMATION m{};
        return VirtualQuery(p, &m, sizeof m) ? m.Protect & 0xff : 0;
    };

    const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
    const auto uav = buffer_desc(kBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    Ref<ID3D12Resource> out;
    EXPECT(SUCCEEDED(c.device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &uav,
                                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                       IID_PPV_ARGS(&out.p))));
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8,
                                         D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    Ref<ID3D12DescriptorHeap> heap;
    EXPECT(SUCCEEDED(c.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap.p))));
    Ref<ID3D12CommandAllocator> allocator;
    Ref<ID3D12GraphicsCommandList> list;
    EXPECT(SUCCEEDED(c.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
    EXPECT(SUCCEEDED(c.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, c.pso,
                                                 IID_PPV_ARGS(&list.p))));
    EXPECT(list->Close() == S_OK);
    WriteBack s{&c, allocator.p, list.p, out.p, heap.p,
                reinterpret_cast<XboxHeapHandleFn>(slot(heap.p, 10))(heap.p)};

    // Step 1.
    EXPECT(submit_write(s, ring_va) == 0);
    EXPECT(aliases.shader_va(ring_va) != ring_va);  // a window was made for the title's pages
    EXPECT(ring[0] == 0);                           // the copy took the write
    const DWORD guarded = protection(ring);
    EXPECT(guarded == PAGE_READONLY);  // guarded until written

    // Step 2: word 1000 is on the first page, past the 64 words the shader writes.
    const std::uint64_t faults_before = rt::d3d12::title_shadow_watch(c.inner).faults;
    ring[1000] = 0x5EED0001u;
    const std::uint64_t title_faults = rt::d3d12::title_shadow_watch(c.inner).faults - faults_before;
    EXPECT(title_faults == 1);
    EXPECT(rt::d3d12::title_shadow_watch(c.inner).watched_bytes > 0);  // the window's pages are write-watched
    constexpr UINT kPageWords = 4096 / 4;
    EXPECT(submit_write(s, ring_va, same_tile ? ring_va + 2 * 4096 : 0) == 0);
    UINT dropped = 0;
    for (UINT i = 0; i < kElements; ++i) dropped += ring[i] == 0;
    EXPECT(dropped == kElements);  // the title wrote that page since it was copied: the GPU's words were not taken
    const DWORD hot = protection(ring);
    EXPECT(hot == PAGE_READWRITE);  // asked about, not guarded
    EXPECT(rt::d3d12::title_shadow_watch(c.inner).hot_bytes > 0);
    if (same_tile) {
        EXPECT(protection(ring + kPageWords) == PAGE_READWRITE);  // the fault made the whole block hot
        ring[kPageWords + 500] = 0x5EED0002u;                     // no fault: the write watch has it
    }

    // Step 3: what a geometry view over these pages arms (GeometryViews), standing in for the other guard.
    const auto tile = std::uintptr_t(ring_va);
    leonida::geometry_writes::Cell* cell = leonida::geometry_writes::acquire(tile);
    EXPECT(cell && leonida::geometry_writes::arm(tile, *cell));
    const DWORD armed = protection(ring);
    EXPECT(armed == PAGE_READONLY);

    // Step 4.
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    EXPECT(done);
    HANDLE watchdog = CreateThread(
        nullptr, 0,
        [](void* event) -> DWORD {
            if (WaitForSingleObject(static_cast<HANDLE>(event), 20000) == WAIT_OBJECT_0) return 0;
            std::fprintf(stderr, "FAIL: the submission whose write-back faulted on a page another guard keeps "
                                 "read-only did not return in 20 s (the fault handler waits on the tracker's lock its "
                                 "own thread holds)\n");
            std::fflush(stderr);
            TerminateProcess(GetCurrentProcess(), 3);
            return 1;
        },
        done, 0, nullptr);
    EXPECT(watchdog);
    const int submitted = submit_write(s, ring_va);
    SetEvent(done);
    WaitForSingleObject(watchdog, INFINITE);
    CloseHandle(watchdog);
    CloseHandle(done);
    EXPECT(submitted == 0);
    UINT nonzero = 0;
    for (UINT i = 0; i < kElements; ++i) nonzero += ring[i] != 0;
    std::printf("write back fault: protection 0x%lx guarded, 0x%lx hot after %llu title fault, 0x%lx under the "
                "geometry cell; the faulting write-back returned and %u of %u words the shader wrote are in the "
                "title's pages, the title's own word %s\n",
                guarded, hot, static_cast<unsigned long long>(title_faults), armed, nonzero, kElements,
                ring[1000] == 0x5EED0001u ? "kept" : "lost");
    EXPECT(nonzero > 0);
    EXPECT(ring[1000] == 0x5EED0001u);
    if (same_tile) {
        // The run past the title's page, in the tile the faulting copy reopened: its GPU words went back too.
        UINT later = 0;
        for (UINT i = 0; i < kElements; ++i) later += ring[2 * kPageWords + i] != 0;
        std::printf("write back fault, same tile: %u of %u words the second dispatch wrote on the third page are in "
                    "the title's pages, the title's word on the second page %s\n",
                    later, kElements, ring[kPageWords + 500] == 0x5EED0002u ? "kept" : "lost");
        EXPECT(later == nonzero);  // the same shader, the same words
        EXPECT(ring[kPageWords + 500] == 0x5EED0002u);
    }

    leonida::geometry_writes::release(tile);
    aliases.remove_title_memory(ring_va);
    leonida::title_write_watch::remove(ring_va);
    VirtualFree(ring, 0, MEM_RELEASE);
    return 0;
}

void remove_test_pair(std::uintptr_t gpu, std::uintptr_t cpu) {
    auto& state = leonida::title_gpu_views::state();
    std::unique_lock lock(state.mutex);
    std::erase_if(state.views, [&](const auto& pair) { return pair.gpu == gpu && pair.cpu == cpu; });
    state.count.store(state.views.size(), std::memory_order_release);
}

int check_title_memory(Context& c, int& checks, bool geometry) {
    constexpr UINT64 ring_bytes = 256 * 1024;
    auto* ring = static_cast<std::uint32_t*>(VirtualAlloc(nullptr, ring_bytes, MEM_TOP_DOWN | MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    EXPECT(ring);
    const auto ring_va = reinterpret_cast<std::uint64_t>(ring);
    EXPECT(ring_va >= c.arena);
    auto& aliases = leonida::title_va_aliases();
    aliases.add_title_memory(ring_va, ring_bytes);  // as the xmem shims record what they hand the title
    const std::uint64_t table_before = aliases.translated(leonida::TitleVaAliases::table_view);
    const std::uint64_t created_before = aliases.translated(leonida::TitleVaAliases::created_view);
    const std::uint64_t root_before = aliases.translated(leonida::TitleVaAliases::root_view);

    const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT}, readback_heap{D3D12_HEAP_TYPE_READBACK};
    const auto uav = buffer_desc(kBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    Ref<ID3D12Resource> out[4], readback;
    for (auto& o : out)
        EXPECT(SUCCEEDED(c.device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &uav,
                                                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                           IID_PPV_ARGS(&o.p))));
    const auto readback_desc = buffer_desc(4 * kBytes);
    EXPECT(SUCCEEDED(c.device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(&readback.p))));

    struct ConsoleHeap {
        D3D12_DESCRIPTOR_HEAP_DESC desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8,
                                       D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        UINT64 address{}, size{};
    } h;
    const bool console = geometry || std::getenv("LEONIDA_TEST_CONSOLE_SHADOW");
    if (console) {
        h.size = 65536;
        h.desc.NumDescriptors = UINT(h.size / 32);
        h.address = reinterpret_cast<UINT64>(VirtualAlloc(nullptr, h.size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        EXPECT(h.address);
    }
    Ref<ID3D12DescriptorHeap> heap;
    EXPECT(SUCCEEDED(c.device->CreateDescriptorHeap(&h.desc, IID_PPV_ARGS(&heap.p))));
    const std::uint64_t cpu = reinterpret_cast<XboxHeapHandleFn>(slot(heap.p, 9))(heap.p);
    const std::uint64_t gpu = reinterpret_cast<XboxHeapHandleFn>(slot(heap.p, 10))(heap.p);
    const UINT inc = c.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    EXPECT(!console || cpu == h.address);
    const auto raw_vsharp = [](std::uint64_t address) {
        leonida::gfx10::Buffer b{};
        b.address = address;
        b.num_records = kBytes;
        b.format = leonida::gfx10::format_from_vulkan(VK_FORMAT_R32_UINT);
        b.oob_select = leonida::gfx10::OobSelect::raw;
        return leonida::gfx10::encode(b);
    };
    if (!console) {
        // Unknown high address and a view extending past a real buffer.
        const auto missing = raw_vsharp((1ull << 46) + 4096);
        const auto partial = raw_vsharp(out[0]->GetGPUVirtualAddress() + kBytes - 4);
        auto* words = reinterpret_cast<std::uint32_t*>(cpu + 6 * inc);
        std::memcpy(words, missing.data(), sizeof(missing));
        std::memcpy(words + 4, partial.data(), sizeof(partial));
        aliases.scan_table(gpu + 6 * inc, 1);
        EXPECT(words[0] == 0 && words[1] == 0 && words[2] == 0 && words[3] == missing[3]);
        EXPECT(words[4] == 0 && words[5] == 0 && words[6] == 0 && words[7] == partial[3]);
    }
    // Slot 0: a V# the CPU writes itself, its base in the ring. Slot 1: CreateConstantBufferView of the ring.
    // Slot 2: a raw 64-bit VA (out[2]) for the root-view mode. Slots 3 and 4: the negative controls.
    std::uint64_t guest = ring_va;
    HANDLE section{};
    void* primary{};
    void* peer{};
    constexpr auto data_offset = 0x1000u;
    if (geometry) {
        section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 65536, nullptr);
        EXPECT(section);
        primary = MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, 0);
        peer = MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, 0);
        EXPECT(primary && peer);
        source_tile = reinterpret_cast<std::uintptr_t>(primary);
        peer_tile = reinterpret_cast<std::uintptr_t>(peer);
        leonida::geometry_writes::aliases.store(&aliases_of_test_tile);
        auto* gpu_range = VirtualAlloc(nullptr, ring_bytes, MEM_TOP_DOWN | MEM_RESERVE, PAGE_NOACCESS);
        EXPECT(gpu_range);
        guest = reinterpret_cast<std::uintptr_t>(gpu_range);
        leonida::title_gpu_views::note_reservation(source_tile, 65536, false);
        EXPECT(leonida::title_gpu_views::note_reservation(guest, 65536, true));
        aliases.add_title_memory(guest, ring_bytes);
        const auto srv = raw_srv(0, kElements);
        EXPECT(rt::d3d12::write_placed_buffer_view(c.inner, false, &srv, guest + data_offset, kBytes,
                                                    D3D12_CPU_DESCRIPTOR_HANDLE{cpu + 7 * inc}));
        EXPECT(vsharp_base(cpu + 7 * inc) == guest + data_offset);
    }
    const auto v0 = raw_vsharp(guest + data_offset);
    std::memcpy(reinterpret_cast<void*>(cpu + (geometry ? 9 * inc : 0)), v0.data(), sizeof(v0));
    const D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{ring_va + 0x2000, 256};
    // Simulate a Wine process whose title reservations leave no usable address in the main arena.
    const auto* opt = std::getenv("LEONIDA_STREAM_BUFFER_WINDOWS");
    const bool external = opt && !std::strcmp(opt, "1");
    if (external) aliases.add_title_memory(1, c.arena - 1);
    c.device->CreateConstantBufferView(&cbv, D3D12_CPU_DESCRIPTOR_HANDLE{cpu + inc});
    if (external) aliases.remove_title_memory(1);
    const auto created = vsharp_base(cpu + inc);
    EXPECT(created != cbv.BufferLocation && created == aliases.shader_va(cbv.BufferLocation));
    EXPECT((created >= c.arena) == external);  // translated when written
    const std::uint64_t out2 = out[2]->GetGPUVirtualAddress();
    std::memcpy(reinterpret_cast<void*>(cpu + 2 * inc), &out2, sizeof(out2));
    const auto arena_vsharp = raw_vsharp(out[0]->GetGPUVirtualAddress());
    std::memcpy(reinterpret_cast<void*>(cpu + 3 * inc), arena_vsharp.data(), sizeof(arena_vsharp));
    auto image_like = raw_vsharp(ring_va + 0x4000);
    image_like[3] |= 0x8u << 28;  // TYPE: an image
    std::memcpy(reinterpret_cast<void*>(cpu + 4 * inc), image_like.data(), sizeof(image_like));
    std::uint8_t before3[32], before4[32];
    std::memcpy(before3, reinterpret_cast<const void*>(cpu + 3 * inc), 32);
    std::memcpy(before4, reinterpret_cast<const void*>(cpu + 4 * inc), 32);
    const auto missing = raw_vsharp((1ull << 46) + 4096);
    std::memcpy(reinterpret_cast<void*>(cpu + 5 * inc), missing.data(), sizeof(missing));
    // The ring's data, written before the submission (after the descriptors, as a title fills its ring per frame).
    for (UINT i = 0; i < ring_bytes / 4; ++i) ring[i] = 0xC0000000u + i * 7u;
    if (geometry) {
        // Only the other mapping changes after the view is made. Never write through the address the runtime reads.
        std::memcpy(static_cast<std::uint8_t*>(peer) + data_offset, ring + data_offset / 4, kBytes);
    }

    Ref<ID3D12CommandAllocator> allocator;
    Ref<ID3D12GraphicsCommandList> list;
    EXPECT(SUCCEEDED(c.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
    EXPECT(SUCCEEDED(c.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, c.pso,
                                                 IID_PPV_ARGS(&list.p))));
    ID3D12DescriptorHeap* heaps[] = {heap.p};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(c.root);
    // The CPU-written V# (buffer SRV mode).
    list->SetComputeRoot32BitConstant(0, geometry ? 9 : 0, 0);
    list->SetComputeRoot32BitConstant(0, geometry ? 0xFFFFFFFCu : 0xFFFFFFFFu, 1);
    list->SetComputeRootUnorderedAccessView(1, out[0]->GetGPUVirtualAddress());
    list->SetComputeRootDescriptorTable(2, D3D12_GPU_DESCRIPTOR_HANDLE{gpu});
    list->Dispatch(1, 1, 1);
    // A console CBV uses 16-byte records. Read its first four words through the same indexed-load fixture.
    list->SetComputeRoot32BitConstant(0, geometry ? 1 : 0, 0);
    list->SetComputeRoot32BitConstant(0, geometry ? 0xFFFFFFFCu : 0, 1);
    list->SetComputeRootUnorderedAccessView(1, out[1]->GetGPUVirtualAddress());
    list->SetComputeRootDescriptorTable(2, D3D12_GPU_DESCRIPTOR_HANDLE{gpu + inc});
    list->Dispatch(1, 1, 1);
    // A root view into the ring, read 256 bytes past its base, written through slot 2's raw VA.
    list->SetComputeRoot32BitConstant(0, kRootAdd, 0);
    list->SetComputeRoot32BitConstant(0, 0xFFFFFFFDu, 1);
    list->SetComputeRootUnorderedAccessView(1, ring_va + 0x3000);
    list->SetComputeRootDescriptorTable(2, D3D12_GPU_DESCRIPTOR_HANDLE{gpu + 2 * inc});
    list->Dispatch(1, 1, 1);
    list->SetComputeRoot32BitConstant(0, 0xFFFFFFFAu, 1);  // cs_user_data's descriptor-word mode
    list->SetComputeRootUnorderedAccessView(1, out[3]->GetGPUVirtualAddress());
    list->SetComputeRootDescriptorTable(2, D3D12_GPU_DESCRIPTOR_HANDLE{gpu + 5 * inc});
    list->Dispatch(1, 1, 1);
    // The negative-control slots are bound too, so the scan reads them.
    list->SetComputeRootDescriptorTable(2, D3D12_GPU_DESCRIPTOR_HANDLE{gpu + 3 * inc});
    list->SetComputeRootDescriptorTable(2, D3D12_GPU_DESCRIPTOR_HANDLE{gpu + 4 * inc});
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    list->ResourceBarrier(1, &barrier);
    for (UINT k = 0; k < 4; ++k) {
        D3D12_RESOURCE_BARRIER to_copy{};
        to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_copy.Transition.pResource = out[k].p;
        to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &to_copy);
        list->CopyBufferRegion(readback.p, k * kBytes, out[k].p, 0, kBytes);
    }
    EXPECT(list->Close() == S_OK);
    ID3D12CommandList* lists[] = {list.p};
    c.queue->ExecuteCommandLists(1, lists);
    EXPECT(rt::d3d12::last_execute_result(c.queue) == leonida::ReplayResult::success);
    EXPECT(SUCCEEDED(c.queue->Signal(c.fence, ++c.fence_value)));
    EXPECT(SUCCEEDED(c.fence->SetEventOnCompletion(c.fence_value, nullptr)));

    void* mapped = nullptr;
    EXPECT(SUCCEEDED(readback->Map(0, nullptr, &mapped)));
    const auto* got = static_cast<const std::uint32_t*>(mapped);
    UINT raw_ok = 0, cbv_ok = 0, root_ok = 0;
    for (UINT i = 0; i < kElements; ++i) {
        if (got[i] == ring[data_offset / 4 + (geometry ? i % 4 : i)]) ++raw_ok;
        if (got[kElements + i] == ring[0x2000 / 4 + (geometry ? i % 4 : i)]) ++cbv_ok;
        if (got[2 * kElements + i] == ring[0x3000 / 4 + 64 + i] + kRootAdd) ++root_ok;
    }
    const auto* descriptor = got + 3 * kElements;
    EXPECT(!descriptor[0] && !descriptor[1] && !descriptor[2] && descriptor[3] == missing[3]);
    if (console) EXPECT(!std::memcmp(reinterpret_cast<void*>(cpu + 5 * inc), missing.data(), sizeof(missing)));
    readback->Unmap(0, nullptr);
    const bool rewritten = console
        ? vsharp_base(cpu + (geometry ? 9 * inc : 0)) == guest + data_offset  // shader followed the copy; original bytes intact
        : vsharp_base(cpu) != ring_va + data_offset && vsharp_base(cpu) == aliases.shader_va(ring_va + data_offset);
    const bool arena_kept = !std::memcmp(before3, reinterpret_cast<const void*>(cpu + 3 * inc), 32);
    const bool image_kept = !std::memcmp(before4, reinterpret_cast<const void*>(cpu + 4 * inc), 32);
    std::printf("title memory (a constant ring): CPU-written table V# %u/%u (rewritten before the submission: %s), "
                "created CBV %u/%u, root view %u/%u; negative controls: arena V# kept %s, T#-shaped slot kept %s; "
                "translated: %llu table V#, %llu created, %llu root\n",
                raw_ok, kElements, rewritten ? "yes" : "no", cbv_ok, kElements, root_ok, kElements,
                arena_kept ? "yes" : "no", image_kept ? "yes" : "no",
                static_cast<unsigned long long>(aliases.translated(leonida::TitleVaAliases::table_view) - table_before),
                static_cast<unsigned long long>(aliases.translated(leonida::TitleVaAliases::created_view) -
                                                created_before),
                static_cast<unsigned long long>(aliases.translated(leonida::TitleVaAliases::root_view) - root_before));
    EXPECT(raw_ok == kElements && cbv_ok == kElements && root_ok == kElements);
    EXPECT(rewritten && arena_kept && image_kept);
    // Committed memory that is not the title's (the runtime's own, say) and that no allocation of the device's holds:
    // heap_base + its address is no allocation either, so its view gets no base and no records, as a title range
    // with no committed page does (it gets no copy). Neither is counted as left untranslated, and neither has a copy.
    const std::uint64_t untranslated_before = aliases.untranslated(leonida::TitleVaAliases::created_view);
    void* host = VirtualAlloc(nullptr, 65536, MEM_TOP_DOWN | MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    void* gone = VirtualAlloc(nullptr, 65536, MEM_TOP_DOWN | MEM_RESERVE, PAGE_READWRITE);
    const auto host_va = reinterpret_cast<std::uint64_t>(host), gone_va = reinterpret_cast<std::uint64_t>(gone);
    aliases.add_title_memory(gone_va, 65536);
    const D3D12_CONSTANT_BUFFER_VIEW_DESC not_title{host_va, 256}, reserved_only{gone_va, 256};
    c.device->CreateConstantBufferView(&not_title, D3D12_CPU_DESCRIPTOR_HANDLE{cpu + 5 * inc});
    c.device->CreateConstantBufferView(&reserved_only, D3D12_CPU_DESCRIPTOR_HANDLE{cpu + 6 * inc});
    EXPECT(vsharp_base(cpu + 5 * inc) == 0);
    // A title range with no committed page gets no copy: the view is written with no base and no records (loads
    // return 0, stores are dropped), never with the raw title address a shader would add heap_base to.
    EXPECT(vsharp_base(cpu + 6 * inc) == 0);
    EXPECT(aliases.untranslated(leonida::TitleVaAliases::created_view) == untranslated_before);
    if (console) heap.reset();
    if (geometry) {
        leonida::geometry_writes::aliases.store(nullptr);
        remove_test_pair(guest, source_tile);
        UnmapViewOfFile(peer);
        UnmapViewOfFile(primary);
        CloseHandle(section);
        aliases.remove_title_memory(guest);
        VirtualFree(reinterpret_cast<void*>(guest), 0, MEM_RELEASE);
    }
    if (console) VirtualFree(reinterpret_cast<void*>(h.address), 0, MEM_RELEASE);
    aliases.remove_title_memory(gone_va);
    aliases.remove_title_memory(ring_va);
    VirtualFree(host, 0, MEM_RELEASE);
    VirtualFree(gone, 0, MEM_RELEASE);
    checks += 4;
    return 0;
}

// For a stream pool, the title reserves its CPU view and a GPU-only alias of the
// same size; both name the same frames. One mapping serves the pool's readable run, and a view naming either address
// resolves to it, so streamed vertex buffers stop asking for a 128 KiB title window each and no address is left raw.
int check_stream_pool(Context& c, int& checks) {
    constexpr UINT64 pool_bytes = 256 * 1024;
    void* cpu_pages = VirtualAlloc(nullptr, pool_bytes, MEM_TOP_DOWN | MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    void* gpu_alias = VirtualAlloc(nullptr, pool_bytes, MEM_TOP_DOWN | MEM_RESERVE, PAGE_NOACCESS);
    EXPECT(cpu_pages && gpu_alias);
    const auto cpu_va = reinterpret_cast<std::uint64_t>(cpu_pages);
    const auto gpu_va = reinterpret_cast<std::uint64_t>(gpu_alias);
    EXPECT(cpu_va >= c.arena && gpu_va >= c.arena);
    // The reservations the xmem shims record, CPU view first (title_gpu_views pairs a GPU-only one with the
    // CPU-mappable one of the same size made just before it).
    EXPECT(!leonida::title_gpu_views::note_reservation(cpu_va, pool_bytes, false));  // the CPU twin
    EXPECT(leonida::title_gpu_views::note_reservation(gpu_va, pool_bytes, true));    // pairs with it
    auto& aliases = leonida::title_va_aliases();
    aliases.add_title_memory(cpu_va, pool_bytes);
    aliases.add_title_memory(gpu_va, pool_bytes);
    EXPECT(leonida::title_gpu_views::to_host_view(gpu_va + 0x2000, 0x100) == cpu_va + 0x2000);

    constexpr UINT64 seam = pool_bytes / 2;
    constexpr UINT64 existing = 0x100000;
    const auto root = leonida::TitleVaAliases::root_view;
    EXPECT(aliases.add(cpu_va + seam, 0x1000, existing));
    const auto ranges_at_seam = aliases.size();
    EXPECT(aliases.resolve(gpu_va + seam - 0x100, 0x200, root) == 0);
    EXPECT(aliases.resolve(gpu_va + seam, 0x2000, root) == 0);
    EXPECT(aliases.resolve(cpu_va + seam, 0x2000, root) == 0);
    EXPECT(aliases.size() == ranges_at_seam);
    EXPECT(aliases.shader_va(cpu_va + seam) == existing);
    aliases.remove(cpu_va + seam, existing);

    const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4,
                                               D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    Ref<ID3D12DescriptorHeap> heap;
    EXPECT(SUCCEEDED(c.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap.p))));
    const std::uint64_t handle = reinterpret_cast<XboxHeapHandleFn>(slot(heap.p, 9))(heap.p);
    const UINT inc = c.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const std::size_t ranges_before = aliases.size();
    // A view naming the GPU-only alias, then one naming the CPU view of the same bytes.
    const D3D12_CONSTANT_BUFFER_VIEW_DESC alias_view{gpu_va + 0x2000, 256};
    const D3D12_CONSTANT_BUFFER_VIEW_DESC cpu_view{cpu_va + 0x2000, 256};
    c.device->CreateConstantBufferView(&alias_view, D3D12_CPU_DESCRIPTOR_HANDLE{handle});
    c.device->CreateConstantBufferView(&cpu_view, D3D12_CPU_DESCRIPTOR_HANDLE{handle + inc});
    const std::uint64_t alias_base = vsharp_base(handle);
    const std::uint64_t twin_base = vsharp_base(handle + inc);
    // One mapping, entered for both addresses: the same backing, and no raw address.
    EXPECT(alias_base != 0 && alias_base != ((gpu_va + 0x2000) & 0xFFFFFFFFFFFFull));
    EXPECT(alias_base == twin_base);
    EXPECT(alias_base == (aliases.shader_va(gpu_va + 0x2000) & 0xFFFFFFFFFFFFull));
    EXPECT(aliases.shader_va(gpu_va + 0x2000) == aliases.shader_va(cpu_va + 0x2000));
    // Two ranges (the two views' addresses), each a page-aligned run of the pool: one mapping, not one per view.
    EXPECT(aliases.size() == ranges_before + 2);
    EXPECT(aliases.resolve(gpu_va + pool_bytes - 0x100, 0x200, root) == 0);
    EXPECT(aliases.size() == ranges_before + 2);
    std::printf("stream pool: GPU-only alias 0x%llx and CPU view 0x%llx share one %llu KiB mapping (V# base "
                "0x%llx); a second view naming the twin made no mapping\n",
                static_cast<unsigned long long>(gpu_va), static_cast<unsigned long long>(cpu_va),
                static_cast<unsigned long long>(pool_bytes >> 10), static_cast<unsigned long long>(alias_base));

    heap.reset();
    aliases.remove_title_memory(gpu_va);
    aliases.remove_title_memory(cpu_va);
    remove_test_pair(gpu_va, cpu_va);
    VirtualFree(gpu_alias, 0, MEM_RELEASE);
    VirtualFree(cpu_pages, 0, MEM_RELEASE);
    checks += 3;
    return 0;
}

// One submission that reads 64 title words through a root view at `va`, 256 bytes past it, into an arena buffer
// (cs_user_data's mode 0xFFFFFFFD: the table entry is the buffer's raw VA), then copies them to a READBACK buffer.
// `matched`: how many are words[64 + i] + kRootAdd, the bytes at `va` as the title wrote them.
struct Reader {
    Context* c;
    ID3D12CommandAllocator* allocator;
    ID3D12GraphicsCommandList* list;
    ID3D12Resource* out;
    ID3D12Resource* readback;
    ID3D12DescriptorHeap* heap;
    UINT64 gpu;
};
int read_root(Reader& r, std::uint64_t va, const std::uint32_t* words, UINT& matched) {
    EXPECT(r.allocator->Reset() == S_OK);
    EXPECT(r.list->Reset(r.allocator, r.c->pso) == S_OK);
    ID3D12DescriptorHeap* heaps[] = {r.heap};
    r.list->SetDescriptorHeaps(1, heaps);
    r.list->SetComputeRootSignature(r.c->root);
    r.list->SetComputeRoot32BitConstant(0, kRootAdd, 0);
    r.list->SetComputeRoot32BitConstant(0, 0xFFFFFFFDu, 1);
    r.list->SetComputeRootUnorderedAccessView(1, va);
    r.list->SetComputeRootDescriptorTable(2, D3D12_GPU_DESCRIPTOR_HANDLE{r.gpu});
    r.list->Dispatch(1, 1, 1);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = r.out;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    r.list->ResourceBarrier(1, &barrier);
    r.list->CopyBufferRegion(r.readback, 0, r.out, 0, kBytes);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    r.list->ResourceBarrier(1, &barrier);
    EXPECT(r.list->Close() == S_OK);
    ID3D12CommandList* lists[] = {r.list};
    r.c->queue->ExecuteCommandLists(1, lists);
    EXPECT(rt::d3d12::last_execute_result(r.c->queue) == leonida::ReplayResult::success);
    EXPECT(SUCCEEDED(r.c->queue->Signal(r.c->fence, ++r.c->fence_value)));
    EXPECT(SUCCEEDED(r.c->fence->SetEventOnCompletion(r.c->fence_value, nullptr)));
    void* mapped = nullptr;
    EXPECT(SUCCEEDED(r.readback->Map(0, nullptr, &mapped)));
    const auto* got = static_cast<const std::uint32_t*>(mapped);
    matched = 0;
    for (UINT i = 0; i < kElements; ++i) matched += got[i] == words[64 + i] + kRootAdd;
    r.readback->Unmap(0, nullptr);
    return 0;
}

// What the GPU wrote in a window goes back to the title's pages when the window is retired before that submission
// has run: its copy stays a target of its pages' tracker until the submissions that may name it have completed and
// been copied back, and goes back only where the live copy over the same pages changed nothing. The writing
// submission waits on a fence only the CPU signals, so it is still queued when the next one's refresh retires the
// window (the title placed a slot-133 buffer over its pages meanwhile).
int check_retired_window_write_back(Context& c) {
    auto& aliases = leonida::title_va_aliases();
    const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
    const auto uav = buffer_desc(kBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    Ref<ID3D12Resource> out;
    EXPECT(SUCCEEDED(c.device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &uav,
                                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                       IID_PPV_ARGS(&out.p))));
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8,
                                         D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    Ref<ID3D12DescriptorHeap> heap;
    EXPECT(SUCCEEDED(c.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap.p))));
    Ref<ID3D12CommandAllocator> allocators[2];
    Ref<ID3D12GraphicsCommandList> lists[2];
    for (int i = 0; i < 2; ++i) {
        EXPECT(SUCCEEDED(c.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                          IID_PPV_ARGS(&allocators[i].p))));
        EXPECT(SUCCEEDED(c.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[i].p, c.pso,
                                                     IID_PPV_ARGS(&lists[i].p))));
        EXPECT(lists[i]->Close() == S_OK);
    }
    Ref<ID3D12Fence> gate, held_done;
    EXPECT(SUCCEEDED(c.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate.p))));
    EXPECT(SUCCEEDED(c.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&held_done.p))));
    // The held submission's own queue: a queue holds what follows a Wait until it is signalled, so the next
    // submission, whose refresh retires the window, runs on the context's.
    const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    Ref<ID3D12CommandQueue> gated;
    EXPECT(SUCCEEDED(c.device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&gated.p))));
    const UINT64 gpu = reinterpret_cast<XboxHeapHandleFn>(slot(heap.p, 10))(heap.p);
    WriteBack held{&c, allocators[0].p, lists[0].p, out.p, heap.p, gpu, gated.p};
    WriteBack next{&c, allocators[1].p, lists[1].p, out.p, heap.p, gpu};
    const std::uint64_t arena_va = out->GetGPUVirtualAddress();  // not the title's memory: nothing to copy back
    // A reader through the buffer placed over the ring, with its own heap and output.
    const D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK};
    const auto readback_desc = buffer_desc(kBytes);
    Ref<ID3D12Resource> read_out, readback;
    EXPECT(SUCCEEDED(c.device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &uav,
                                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                       IID_PPV_ARGS(&read_out.p))));
    EXPECT(SUCCEEDED(c.device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(&readback.p))));
    Ref<ID3D12DescriptorHeap> read_heap;
    EXPECT(SUCCEEDED(c.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&read_heap.p))));
    const std::uint64_t read_out_va = read_out->GetGPUVirtualAddress();
    std::memcpy(reinterpret_cast<void*>(reinterpret_cast<XboxHeapHandleFn>(slot(read_heap.p, 9))(read_heap.p)),
                &read_out_va, sizeof(read_out_va));
    Ref<ID3D12CommandAllocator> read_allocator;
    Ref<ID3D12GraphicsCommandList> read_list;
    EXPECT(SUCCEEDED(c.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                      IID_PPV_ARGS(&read_allocator.p))));
    EXPECT(SUCCEEDED(c.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, read_allocator.p, c.pso,
                                                 IID_PPV_ARGS(&read_list.p))));
    EXPECT(read_list->Close() == S_OK);
    Reader reader{&c, read_allocator.p, read_list.p, read_out.p, readback.p, read_heap.p,
                  reinterpret_cast<XboxHeapHandleFn>(slot(read_heap.p, 10))(read_heap.p)};
    // Pages no range answers for yet: a range of a buffer the tests before this one placed can outlive the buffer at
    // an address the ring might get again, so such pages are kept aside until the end. Write-combined, so the buffer
    // placed over them is a copy refreshed before each submission on every driver (one that imports read-write pages
    // would read the pages themselves).
    constexpr UINT64 ring_bytes = 128 * 1024;
    // The held submission's words: 256 bytes from here, through the window. Half of them share a 256-byte piece with
    // the words a later submission writes through the buffer placed over the ring.
    constexpr UINT64 held_at = 384;
    std::vector<void*> taken;
    std::uint32_t* ring = nullptr;
    for (int tries = 0; tries < 32 && !ring; ++tries) {
        void* pages = VirtualAlloc(nullptr, ring_bytes, MEM_TOP_DOWN | MEM_RESERVE | MEM_COMMIT,
                                   PAGE_READWRITE | PAGE_WRITECOMBINE);
        EXPECT(pages);
        const auto at = reinterpret_cast<std::uint64_t>(pages);
        if (aliases.shader_va(at) == at && aliases.shader_va(at + ring_bytes - 1) == at + ring_bytes - 1)
            ring = static_cast<std::uint32_t*>(pages);
        else
            taken.push_back(pages);
    }
    EXPECT(ring);
    const auto ring_va = reinterpret_cast<std::uint64_t>(ring);
    for (UINT i = 0; i < ring_bytes / 4; ++i) ring[i] = 0;
    aliases.add_title_memory(ring_va, ring_bytes);

    // Held on the GPU: it writes the ring's window once the gate opens.
    const auto empty = rt::d3d12::title_windows(c.inner);
    EXPECT(SUCCEEDED(gated->Wait(gate.p, 1)));
    EXPECT(submit_write(held, ring_va + held_at, 0, false) == 0);
    EXPECT(SUCCEEDED(gated->Signal(held_done.p, 1)));
    const std::uint64_t window = aliases.shader_va(ring_va);
    const auto before = rt::d3d12::title_windows(c.inner);
    EXPECT(window != ring_va && before.count == empty.count + 1);
    Ref<ID3D12Resource> placed;
    EXPECT(SUCCEEDED(rt::d3d12::create_buffer_at_host_address(c.inner, ring, buffer_desc(ring_bytes), 0xFFFFFFFFu,
                                                              IID_PPV_ARGS(&placed.p))));
    EXPECT(!aliases.answers_into(window, 1));
    EXPECT(submit_write(next, arena_va, 0, false) == 0);  // its refresh retires the window, its write still queued
    const auto retired = rt::d3d12::title_windows(c.inner);
    EXPECT(retired.retired == before.retired + 1 && retired.waiting >= 1);
    EXPECT(held_done->GetCompletedValue() == 0);  // the write has not run yet
    // Another submission, completed: the window is still kept, because the held batch, recorded before its
    // retirement, names it and has not run (its memory must not be handed out again meanwhile).
    EXPECT(submit_write(next, arena_va) == 0);
    const auto kept = rt::d3d12::title_windows(c.inner);
    EXPECT(kept.waiting == retired.waiting);
    EXPECT(SUCCEEDED(gate->Signal(1)));
    EXPECT(SUCCEEDED(held_done->SetEventOnCompletion(1, nullptr)));
    // A shader writes through the buffer the 256 bytes before the held words: the buffer's copy changed the first
    // half of the 256-byte piece [256, 512) whose second half only the window wrote. Both go back, each its own bytes.
    EXPECT(submit_write(next, ring_va + held_at - 256) == 0);
    // The title resets the list it recorded (a list may be executed again until then); then the window goes, and
    // what the held submission wrote in it reaches the title's pages first, and the copy of the buffer over them: the
    // submission that frees it reads them through the buffer (256 bytes past ring + held_at - 256).
    EXPECT(allocators[0]->Reset() == S_OK);
    EXPECT(lists[0]->Reset(allocators[0].p, c.pso) == S_OK);
    EXPECT(lists[0]->Close() == S_OK);
    rt::d3d12::TitleWindows freed{};
    UINT seen = 0;
    for (int i = 0; i < 8; ++i) {
        EXPECT(read_root(reader, ring_va + held_at - 256, ring + (held_at - 256) / 4, seen) == 0);
        freed = rt::d3d12::title_windows(c.inner);
        if (!freed.waiting) break;
    }
    UINT written = 0;
    for (UINT i = 0; i < kElements; ++i) written += ring[held_at / 4 + i] != 0;
    std::printf("retired window write back: kept while a held batch named it (%llu waiting); %u of %u words the held "
                "submission wrote reached the title's pages, %u read through the buffer by the submission that freed "
                "the window; %llu retired windows wait for the GPU\n",
                static_cast<unsigned long long>(kept.waiting), written, kElements, seen,
                static_cast<unsigned long long>(freed.waiting));
    EXPECT(freed.waiting == 0);
    EXPECT(written == kElements);
    EXPECT(seen == kElements);
    // The buffer's own words go back with its submission's batch (none with LEONIDA_TITLE_WRITEBACK=0).
    const char* write_back = std::getenv("LEONIDA_TITLE_WRITEBACK");
    UINT buffer_words = 0;
    for (UINT i = 0; i < kElements; ++i) buffer_words += ring[(held_at - 256) / 4 + i] != 0;
    EXPECT(buffer_words == ((write_back && write_back[0] == '0') ? 0 : kElements));

    placed.reset();
    aliases.remove_title_memory(ring_va);
    VirtualFree(ring, 0, MEM_RELEASE);
    for (void* pages : taken) VirtualFree(pages, 0, MEM_RELEASE);
    return 0;
}

// No window is left that nothing reaches (RT-TITLE-WINDOW-RETIRE). A range entered over a live one replaces it whole,
// so a mapping made over bytes another one serves left that one's copy answered by nothing, still refreshed and
// charged to LEONIDA_TITLE_WINDOWS_MB. First a stream pool: a request just before an extent the pool already has gets
// an extent that stops where that one starts, and both keep answering. Then a slot-133 buffer the title places over a
// window's pages: the window, now reached by nothing, is retired at the next submission, its bytes returned and its
// memory freed after the submission completes, and the GPU reads the title's bytes through the buffer.
int check_window_orphans(Context& c) {
    const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT}, readback_heap{D3D12_HEAP_TYPE_READBACK};
    const auto uav = buffer_desc(kBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const auto readback_desc = buffer_desc(kBytes);
    Ref<ID3D12Resource> out, readback;
    EXPECT(SUCCEEDED(c.device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &uav,
                                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                       IID_PPV_ARGS(&out.p))));
    EXPECT(SUCCEEDED(c.device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(&readback.p))));
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8,
                                         D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    Ref<ID3D12DescriptorHeap> heap;
    EXPECT(SUCCEEDED(c.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap.p))));
    const std::uint64_t out_va = out->GetGPUVirtualAddress();
    std::memcpy(reinterpret_cast<void*>(reinterpret_cast<XboxHeapHandleFn>(slot(heap.p, 9))(heap.p)), &out_va,
                sizeof(out_va));
    Ref<ID3D12CommandAllocator> allocator;
    Ref<ID3D12GraphicsCommandList> list;
    EXPECT(SUCCEEDED(c.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
    EXPECT(SUCCEEDED(c.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, c.pso,
                                                 IID_PPV_ARGS(&list.p))));
    EXPECT(list->Close() == S_OK);
    Reader r{&c, allocator.p, list.p, out.p, readback.p, heap.p,
             reinterpret_cast<XboxHeapHandleFn>(slot(heap.p, 10))(heap.p)};
    auto& aliases = leonida::title_va_aliases();
    UINT matched = 0;

    // A 512 KiB stream pool: its CPU view committed, its GPU-only alias reserved.
    constexpr UINT64 pool_bytes = 512 * 1024;
    auto* pool = static_cast<std::uint32_t*>(
        VirtualAlloc(nullptr, pool_bytes, MEM_TOP_DOWN | MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    void* gpu_alias = VirtualAlloc(nullptr, pool_bytes, MEM_TOP_DOWN | MEM_RESERVE, PAGE_NOACCESS);
    EXPECT(pool && gpu_alias);
    const auto cpu_va = reinterpret_cast<std::uint64_t>(pool), gpu_va = reinterpret_cast<std::uint64_t>(gpu_alias);
    EXPECT(!leonida::title_gpu_views::note_reservation(cpu_va, pool_bytes, false));
    EXPECT(leonida::title_gpu_views::note_reservation(gpu_va, pool_bytes, true));
    aliases.add_title_memory(cpu_va, pool_bytes);
    aliases.add_title_memory(gpu_va, pool_bytes);
    for (UINT i = 0; i < pool_bytes / 4; ++i) pool[i] = 0xD0000000u + i * 3u;
    constexpr UINT64 later = 0x40000, earlier = 0x10000;  // the second request lands 192 KiB before the first's extent
    const auto before = rt::d3d12::title_windows(c.inner);
    EXPECT(read_root(r, gpu_va + later, pool + later / 4, matched) == 0);
    EXPECT(matched == kElements);
    const std::uint64_t first = aliases.shader_va(cpu_va + later);
    EXPECT(first != cpu_va + later && aliases.shader_va(gpu_va + later) == first);
    EXPECT(read_root(r, gpu_va + earlier, pool + earlier / 4, matched) == 0);
    EXPECT(matched == kElements);
    const auto pooled = rt::d3d12::title_windows(c.inner);
    std::printf("window orphans: two pool requests made %llu windows of %llu KiB; the first extent is answered at its "
                "CPU view %s and its alias %s\n",
                static_cast<unsigned long long>(pooled.count - before.count),
                static_cast<unsigned long long>((pooled.bytes - before.bytes) >> 10),
                aliases.shader_va(cpu_va + later) == first ? "yes" : "no",
                aliases.shader_va(gpu_va + later) == first ? "yes" : "no");
    EXPECT(pooled.count == before.count + 2);
    // [earlier, later) and [later, pool end): no byte twice.
    EXPECT(pooled.bytes - before.bytes == pool_bytes - earlier);
    EXPECT(aliases.shader_va(cpu_va + later) == first && aliases.shader_va(gpu_va + later) == first);
    EXPECT(aliases.shader_va(gpu_va + earlier) == aliases.shader_va(cpu_va + earlier));
    // Both still read the title's bytes, the first through its own extent; and neither window is retired.
    EXPECT(read_root(r, cpu_va + later, pool + later / 4, matched) == 0);
    EXPECT(matched == kElements);
    EXPECT(read_root(r, gpu_va + earlier, pool + earlier / 4, matched) == 0);
    EXPECT(matched == kElements);
    EXPECT(rt::d3d12::title_windows(c.inner).count == pooled.count);

    // A view whose bytes run from the second extent into the first gets one mapping for all of them, over both, never
    // one that stops at the first one's start: a V# with too few bytes is refused, and a root view would reach past
    // its copy into whatever the arena holds next. Both extents are retired at the next submission, and the GPU
    // reads the title's bytes across the seam.
    const UINT inc = c.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const std::uint64_t cbv = reinterpret_cast<XboxHeapHandleFn>(slot(heap.p, 9))(heap.p) + inc;
    constexpr UINT64 seam = later - 0x8000, seam_bytes = 0x10000;  // 32 KiB on each side of the first extent's start
    const D3D12_CONSTANT_BUFFER_VIEW_DESC crossing{gpu_va + seam, UINT(seam_bytes)};
    c.device->CreateConstantBufferView(&crossing, D3D12_CPU_DESCRIPTOR_HANDLE{cbv});
    const std::uint64_t covered = aliases.shader_va(gpu_va + seam, seam_bytes);
    const auto merged = rt::d3d12::title_windows(c.inner);
    std::printf("window orphans: a 64 KiB view across the two extents' seam got V# base 0x%llx (%s), one mapping of "
                "%llu KiB over both\n",
                static_cast<unsigned long long>(vsharp_base(cbv)),
                covered != gpu_va + seam ? "all its bytes in one copy" : "no copy holds them all",
                static_cast<unsigned long long>((merged.bytes - pooled.bytes) >> 10));
    EXPECT(covered != gpu_va + seam && vsharp_base(cbv) == (covered & 0xFFFFFFFFFFFFull));
    EXPECT(aliases.shader_va(cpu_va + seam, seam_bytes) == covered);
    EXPECT(merged.count == pooled.count + 1 && merged.bytes - pooled.bytes == pool_bytes - earlier);
    EXPECT(aliases.shader_va(cpu_va + later) == covered + (later - seam));
    EXPECT(aliases.shader_va(gpu_va + earlier) == covered - (seam - earlier));
    EXPECT(read_root(r, gpu_va + later - 0x180, pool + (later - 0x180) / 4, matched) == 0);  // 512 bytes over it
    EXPECT(matched == kElements);
    const auto settled = rt::d3d12::title_windows(c.inner);
    EXPECT(settled.retired == merged.retired + 2 && settled.count == merged.count - 2);
    EXPECT(settled.bytes == pooled.bytes);
    EXPECT(read_root(r, cpu_va + earlier, pool + earlier / 4, matched) == 0);
    EXPECT(matched == kElements);

    // A 128 KiB constant ring: a window for its first page, then a buffer the title places over the whole ring.
    constexpr UINT64 ring_bytes = 128 * 1024;
    auto* ring = static_cast<std::uint32_t*>(
        VirtualAlloc(nullptr, ring_bytes, MEM_TOP_DOWN | MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    EXPECT(ring);
    const auto ring_va = reinterpret_cast<std::uint64_t>(ring);
    aliases.add_title_memory(ring_va, ring_bytes);
    for (UINT i = 0; i < ring_bytes / 4; ++i) ring[i] = 0xE0000000u + i * 5u;
    EXPECT(read_root(r, ring_va, ring, matched) == 0);
    EXPECT(matched == kElements);
    const auto windowed = rt::d3d12::title_windows(c.inner);
    const std::uint64_t window = aliases.shader_va(ring_va);
    EXPECT(windowed.count == settled.count + 1 && window != ring_va);
    const std::uint64_t window_bytes = windowed.bytes - settled.bytes;
    Ref<ID3D12Resource> placed;
    EXPECT(SUCCEEDED(rt::d3d12::create_buffer_at_host_address(c.inner, ring, buffer_desc(ring_bytes), 0xFFFFFFFFu,
                                                              IID_PPV_ARGS(&placed.p))));
    EXPECT(aliases.shader_va(ring_va) != window && aliases.shader_va(ring_va) != ring_va);
    EXPECT(!aliases.answers_into(window, window_bytes));  // nothing reaches the window now
    for (UINT i = 0; i < ring_bytes / 4; ++i) ring[i] = 0xF0000000u + i * 7u;  // new bytes, after the buffer exists
    EXPECT(read_root(r, ring_va, ring, matched) == 0);  // through the buffer; the window is retired after the refresh
    const auto retired = rt::d3d12::title_windows(c.inner);
    std::printf("window orphans: a buffer over the ring's window: %u/%u words read through it; %llu -> %llu windows, "
                "%llu -> %llu KiB (%llu retired, %llu waiting for the GPU)\n",
                matched, kElements, static_cast<unsigned long long>(windowed.count),
                static_cast<unsigned long long>(retired.count), static_cast<unsigned long long>(windowed.bytes >> 10),
                static_cast<unsigned long long>(retired.bytes >> 10),
                static_cast<unsigned long long>(retired.retired - windowed.retired),
                static_cast<unsigned long long>(retired.waiting));
    EXPECT(matched == kElements);
    EXPECT(retired.count == windowed.count - 1 && retired.bytes == windowed.bytes - window_bytes);
    EXPECT(retired.retired == windowed.retired + 1);
    // Its memory goes once the submission that retired it has completed; the pool's windows stay.
    EXPECT(read_root(r, ring_va, ring, matched) == 0);
    EXPECT(matched == kElements);
    const auto freed = rt::d3d12::title_windows(c.inner);
    EXPECT(freed.waiting == 0 && freed.count == retired.count);
    EXPECT(read_root(r, gpu_va + later, pool + later / 4, matched) == 0);
    EXPECT(matched == kElements);

    placed.reset();
    aliases.remove_title_memory(ring_va);
    aliases.remove_title_memory(gpu_va);
    aliases.remove_title_memory(cpu_va);
    remove_test_pair(gpu_va, cpu_va);
    VirtualFree(ring, 0, MEM_RELEASE);
    VirtualFree(gpu_alias, 0, MEM_RELEASE);
    VirtualFree(pool, 0, MEM_RELEASE);
    return 0;
}

// Four 4 MiB slabs fill the 16 MiB fixture budget. A final 1 MiB view still fits
// immediately after the 3 MiB view in the second slab.
int check_geometry_slab_tails(Context& c) {
    constexpr std::uint64_t mib = 1ull << 20;
    constexpr std::array<unsigned, 5> sizes{3, 3, 4, 4, 1};
    void* pages = VirtualAlloc(nullptr, 15 * mib, MEM_TOP_DOWN | MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    EXPECT(pages);
    const auto source = reinterpret_cast<std::uintptr_t>(pages);
    auto& aliases = leonida::title_va_aliases();
    aliases.add_title_memory(source, 15 * mib);
    alignas(32) std::uint32_t slot[8]{};
    const auto dest = reinterpret_cast<std::uintptr_t>(slot);
    std::array<std::uint64_t, sizes.size()> addresses{};
    std::uint64_t offset = 0;
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        const auto bytes = sizes[i] * mib;
        const auto srv = raw_srv(0, UINT(bytes / 4));
        EXPECT(rt::d3d12::write_placed_buffer_view(c.inner, false, &srv, source + offset, bytes, {dest}));
        addresses[i] = vsharp_base(dest);
        EXPECT(addresses[i] != source + offset);
        offset += bytes;
    }
    EXPECT(addresses[4] == addresses[1] + sizes[1] * mib);
    aliases.remove_title_memory(source);
    VirtualFree(pages, 0, MEM_RELEASE);
    std::puts("geometry slab tails: the final 1 MiB view used free space in an earlier slab at the cap");
    return 0;
}

// Bind the same interior GPU handle first through a short graphics table, then a wide compute table.
// Update its last CBV after Close and again before resubmitting the identical list.
// (Adapted from the tests of the patches descriptor-table-refresh and descriptor-write-through of
// Sigtryggur-Mani-Gudmundsson/graphics-runtime-patches at d9a0bc1b, GPL-3.0.)
int check_rebound_tables(Context& c, ID3D12RootSignature* narrow, bool mirror, bool api_copy) {
    constexpr UINT selected = 64;
    const auto alloc_backing = [](std::size_t n, std::uint64_t) -> void* {
        return VirtualAlloc(nullptr, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    };
    const auto free_backing = [](void* p, std::uint64_t) { VirtualFree(p, 0, MEM_RELEASE); };
    Ref<ID3D12DescriptorHeap> heap;
    const D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, selected + 4,
                                        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    rt::d3d12_xs::set_descriptor_heap_allocator(alloc_backing, free_backing);
    const HRESULT created = c.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap.p));
    rt::d3d12_xs::set_descriptor_heap_allocator(nullptr, nullptr);
    EXPECT(created == S_OK);
    const UINT step = c.device->GetDescriptorHandleIncrementSize(hd.Type);
    const auto cpu = reinterpret_cast<XboxHeapHandleFn>(slot(heap.p, 9))(heap.p);
    const auto gpu = reinterpret_cast<XboxHeapHandleFn>(slot(heap.p, 10))(heap.p);
    const auto real = rt::d3d12_xs::translate_cpu_descriptor_handle(c.device, {SIZE_T(cpu)});
    EXPECT(real.ptr && real.ptr != cpu && gpu && step == 32);

    const D3D12_HEAP_PROPERTIES def{D3D12_HEAP_TYPE_DEFAULT}, up{D3D12_HEAP_TYPE_UPLOAD},
        rb{D3D12_HEAP_TYPE_READBACK};
    Ref<ID3D12Resource> upload, source, out, readback;
    const auto data_desc = buffer_desc(2 * kBytes);
    const auto result_desc = buffer_desc(kBytes);
    const auto uav_desc = buffer_desc(kBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    EXPECT(c.device->CreateCommittedResource(&up, {}, &data_desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                             nullptr, IID_PPV_ARGS(&upload.p)) == S_OK);
    EXPECT(c.device->CreateCommittedResource(&def, {}, &data_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                             nullptr, IID_PPV_ARGS(&source.p)) == S_OK);
    EXPECT(c.device->CreateCommittedResource(&def, {}, &uav_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                             nullptr, IID_PPV_ARGS(&out.p)) == S_OK);
    EXPECT(c.device->CreateCommittedResource(&rb, {}, &result_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                             nullptr, IID_PPV_ARGS(&readback.p)) == S_OK);
    std::uint32_t* input = nullptr;
    EXPECT(upload->Map(0, nullptr, reinterpret_cast<void**>(&input)) == S_OK);
    for (UINT i = 0; i < 2 * kElements; ++i) input[i] = 0x10500000u + 37 * i;
    upload->Unmap(0, nullptr);
    Ref<ID3D12CommandAllocator> alloc;
    Ref<ID3D12GraphicsCommandList> list;
    EXPECT(c.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc.p)) == S_OK);
    EXPECT(c.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.p, c.pso, IID_PPV_ARGS(&list.p)) == S_OK);
    const auto transition_resource = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
        list->ResourceBarrier(1, &barrier);
    };
    list->CopyBufferRegion(source.p, 0, upload.p, 0, data_desc.Width);
    transition_resource(source.p, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12DescriptorHeap* heaps[] = {heap.p};
    list->SetDescriptorHeaps(1, heaps);
    const D3D12_GPU_DESCRIPTOR_HANDLE table{gpu + step};
    list->SetGraphicsRootSignature(narrow);
    list->SetGraphicsRootDescriptorTable(2, table);
    list->SetComputeRootSignature(c.root);
    list->SetComputeRootDescriptorTable(2, table);
    list->SetGraphicsRootDescriptorTable(2, table);
    const UINT args[] = {selected, 0xfffffffbu};
    list->SetComputeRoot32BitConstants(0, 2, args, 0);
    list->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
    list->Dispatch(1, 1, 1);
    transition_resource(out.p, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.p, 0, out.p, 0, kBytes);
    transition_resource(out.p, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition_resource(source.p, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    EXPECT(list->Close() == S_OK);

    // Build two real CBVs in a CPU-only staging heap. Only raw CPU writes reach the title-owned slots below.
    Ref<ID3D12DescriptorHeap> staging;
    const D3D12_DESCRIPTOR_HEAP_DESC staging_desc{hd.Type, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    EXPECT(c.inner->CreateDescriptorHeap(&staging_desc, IID_PPV_ARGS(&staging.p)) == S_OK);
    const auto staging_cpu = staging->GetCPUDescriptorHandleForHeapStart();
    unsigned matched[2]{};
    bool api_slots_correct = true;
    for (UINT pass = 0; pass < 2; ++pass) {
        const D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{source->GetGPUVirtualAddress() + kBytes * pass, kBytes};
        const D3D12_CPU_DESCRIPTOR_HANDLE src{staging_cpu.ptr + pass * step};
        // The reference uses the producer of the slot under test. Xbox CreateCBV
        // addresses 16-byte rows. A PC descriptor copied through the API retains its byte layout.
        const bool xbox_cbv = api_copy && pass == 0;
        (xbox_cbv ? c.device : c.inner)->CreateConstantBufferView(&cbv, src);
        leonida::gfx10::BufferWords words{};
        std::memcpy(words.data(), reinterpret_cast<const void*>(src.ptr), sizeof(words));
        leonida::gfx10::Buffer reference_cbv{};
        EXPECT(leonida::gfx10::decode(words, reference_cbv));
        // The device writes the console's 16-byte rows for both producers unless LEONIDA_CBV_RAW=1.
        wchar_t raw_env[2]{};
        const bool rows = xbox_cbv || !(GetEnvironmentVariableW(L"LEONIDA_CBV_RAW", raw_env, 2) == 1 && raw_env[0] == L'1');
        EXPECT(reference_cbv.stride == (rows ? 16u : 0u));
        EXPECT(reference_cbv.num_records == (rows ? kBytes / 16u : kBytes));
        const void* copy_from = reinterpret_cast<const void*>(src.ptr);
        if (api_copy) {
            // Write to a shader-visible slot outside the bound table. The game copies from its CPU backing.
            const D3D12_CPU_DESCRIPTOR_HANDLE from{static_cast<SIZE_T>(cpu + (selected + 3) * step)};
            copy_from = reinterpret_cast<const void*>(from.ptr);
            if (pass == 0) {
                DWORD previous = 0;
                EXPECT(VirtualProtect(reinterpret_cast<void*>(cpu), hd.NumDescriptors * step, PAGE_READONLY, &previous));
                c.device->CreateConstantBufferView(&cbv, from);
                EXPECT(vsharp_base(from.ptr) == 0);
                EXPECT(VirtualProtect(reinterpret_cast<void*>(cpu), hd.NumDescriptors * step, previous, &previous));
                c.device->CreateConstantBufferView(&cbv, from);
            } else {
                c.device->CopyDescriptorsSimple(1, from, src, hd.Type);
            }
            const void* reference = reinterpret_cast<const void*>(src.ptr);
            const auto* inner = reinterpret_cast<const void*>(real.ptr + (selected + 3) * step);
            const bool real_match = std::memcmp(inner, reference, step) == 0;
            const bool title_match = std::memcmp(copy_from, reference, step) == 0;
            std::printf("API slot %u: real matches staging=%d, title matches staging=%d\n", pass, real_match, title_match);
            api_slots_correct &= real_match && (mirror ? title_match : vsharp_base(from.ptr) == 0);
        }
        for (UINT slot_index : {0u, 1 + selected, 2 + selected})
            std::memcpy(reinterpret_cast<void*>(cpu + slot_index * step), copy_from, step);
        ID3D12CommandList* lists[] = {list.p};
        c.queue->ExecuteCommandLists(1, lists);
        EXPECT(rt::d3d12::last_execute_result(c.queue) == leonida::ReplayResult::success);
        EXPECT(c.queue->Signal(c.fence, ++c.fence_value) == S_OK);
        EXPECT(c.fence->SetEventOnCompletion(c.fence_value, nullptr) == S_OK);
        void* mapped = nullptr;
        EXPECT(readback->Map(0, nullptr, &mapped) == S_OK);
        const auto* got = static_cast<const std::uint32_t*>(mapped);
        for (UINT i = 0; i < kElements; ++i)
            matched[pass] += got[i] == (mirror ? 0x10500000u + 37 * (kElements * pass + i) : 0u);
        readback->Unmap(0, nullptr);
        EXPECT(vsharp_base(real.ptr) == 0 && vsharp_base(real.ptr + (selected + 2) * step) == 0);
    }
    std::printf("rebound table (%s): %u/%u and %u/%u GPU words matched on consecutive submissions\n",
                mirror ? "updated CBV" : "null CBV", matched[0], kElements, matched[1], kElements);
    EXPECT(api_slots_correct && matched[0] == kElements && matched[1] == kElements);
    return 0;
}

// Two slot-133 shadows over the same title page share one tracker and its sums (LEONIDA_SHADOW_KEEP, src/
// shadow_keep.h). A is copied with the title's byte at 1; the byte becomes 2 and B is made over the page, so B's first
// copy holds 2; the byte goes back to 1 and a submission refreshes both. Both copies must hold 1, with the flag on or
// off: B's first copy brings its pieces to the sums, or the sums still read 1 and B keeps 2 (found by lixonik).
int check_shadow_keep_refill(Context& c) {
    constexpr UINT64 kPage = 4096, kAt = 16;  // the byte: page 1 of A, page 0 of B
    auto* pages = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, kPages, MEM_TOP_DOWN | MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    EXPECT(pages);
    pages[kPage + kAt] = 1;
    const D3D12_RESOURCE_DESC desc = buffer_desc(2 * kPage);
    Ref<ID3D12Resource> a, b, readback;
    EXPECT(SUCCEEDED(rt::d3d12::create_buffer_at_host_address(c.inner, pages, desc, 0xFFFFFFFFu,
                                                              IID_PPV_ARGS(&a.p))));
    EXPECT(rt::d3d12::title_shadow_buffer(a.p));
    pages[kPage + kAt] = 2;
    EXPECT(SUCCEEDED(rt::d3d12::create_buffer_at_host_address(c.inner, pages + kPage, desc, 0xFFFFFFFFu,
                                                              IID_PPV_ARGS(&b.p))));
    EXPECT(rt::d3d12::title_shadow_buffer(b.p));
    pages[kPage + kAt] = 1;

    const D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK};
    const auto readback_desc = buffer_desc(2 * kBytes);
    EXPECT(SUCCEEDED(c.device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(&readback.p))));
    Ref<ID3D12CommandAllocator> allocator;
    Ref<ID3D12GraphicsCommandList> list;
    EXPECT(SUCCEEDED(c.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
    EXPECT(SUCCEEDED(c.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, nullptr,
                                                 IID_PPV_ARGS(&list.p))));
    list->CopyBufferRegion(readback.p, 0, a.p, kPage, kBytes);
    list->CopyBufferRegion(readback.p, kBytes, b.p, 0, kBytes);
    EXPECT(list->Close() == S_OK);
    ID3D12CommandList* lists[] = {list.p};
    c.queue->ExecuteCommandLists(1, lists);  // the refresh, then the copies
    EXPECT(rt::d3d12::last_execute_result(c.queue) == leonida::ReplayResult::success);
    EXPECT(SUCCEEDED(c.queue->Signal(c.fence, ++c.fence_value)));
    EXPECT(SUCCEEDED(c.fence->SetEventOnCompletion(c.fence_value, nullptr)));
    void* mapped = nullptr;
    EXPECT(SUCCEEDED(readback->Map(0, nullptr, &mapped)));
    const std::uint8_t got_a = static_cast<const std::uint8_t*>(mapped)[kAt];
    const std::uint8_t got_b = static_cast<const std::uint8_t*>(mapped)[kBytes + kAt];
    readback->Unmap(0, nullptr);
    std::printf("shadow keep refill: A holds %u, B holds %u (the title's byte is 1)\n", got_a, got_b);
    EXPECT(got_a == 1);
    EXPECT(got_b == 1);
    a.reset();
    b.reset();
    VirtualFree(pages, 0, MEM_RELEASE);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const bool api_copy_off = argc == 2 && std::strcmp(argv[1], "--api-descriptor-copy-off") == 0;
    const bool mirror_off = api_copy_off || (argc == 2 && std::strcmp(argv[1], "--rebound-tables-off") == 0);
    const bool second_device = argc == 2 && std::strcmp(argv[1], "--rebound-second-device") == 0;
    const bool api_copy = api_copy_off || (argc == 2 && std::strcmp(argv[1], "--api-descriptor-copy") == 0);
    const bool rebind = api_copy || mirror_off || second_device || (argc == 2 && std::strcmp(argv[1], "--rebound-tables") == 0);
    const bool window_cap = argc == 2 && std::strcmp(argv[1], "--window-cap") == 0;
    const bool window_write_back = argc == 2 && std::strcmp(argv[1], "--window-writeback") == 0;
    const bool window_orphans = argc == 2 && std::strcmp(argv[1], "--window-orphans") == 0;
    const bool window_write_back_tile = argc == 2 && std::strcmp(argv[1], "--window-writeback-fault-tile") == 0;
    const bool window_write_back_fault =
        window_write_back_tile || (argc == 2 && std::strcmp(argv[1], "--window-writeback-fault") == 0);
    const bool placed_write_back = argc == 2 && std::strcmp(argv[1], "--placed-writeback") == 0;
    const bool keep_refill = argc == 2 && std::strcmp(argv[1], "--shadow-keep-refill") == 0;
    if (window_cap) {
        const char* cap = std::getenv("LEONIDA_TITLE_WINDOWS_MB");
        if (!cap || std::strcmp(cap, "0")) {
            std::fprintf(stderr, "FAIL: --window-cap needs LEONIDA_TITLE_WINDOWS_MB=0\n");
            return 1;
        }
    }
    const char* setting = std::getenv("LEONIDA_GEOMETRY_MB");
    const bool geometry = setting && std::atoi(setting) > 0;
    vktest::Device d;
    const auto device_status = d.initialize();
    if (device_status == vktest::DeviceStatus::unavailable) {
        std::fprintf(stderr, "SKIP: no usable Vulkan 1.3 device\n");
        return 77;
    }
    EXPECT(device_status == vktest::DeviceStatus::ready);

    std::vector<std::uint8_t> fssm = {'F', 'S', 'S', 'M'};
    put32(fssm, 6000);
    for (std::uint32_t i = 0; i < 14; ++i) put32(fssm, 0xbf810000u + i);
    const auto container = shader_container(fssm);
    const testfiles::ScratchDir scratch("leonida_title_va_test");
    const auto& library = scratch.path();
    EXPECT(testfiles::copy_file(vktest::shader_fixture_path(LEONIDA_TEST_CS_SPV), library / "cs_user_data.spv"));
    {
        std::ofstream index(library / "index.json", std::ios::binary);
        index << "{\"cs_user_data\": {\"stage\": \"cs\", \"fssm_sha256\": \"" << leonida::sha256_hex(fssm)
              << "\", \"bindings\": [], \"push_constant_bytes\": 144, \"local_size\": [64, 1, 1], "
                 "\"uses_code_base\": true, \"wave64\": false}}";
    }

    int checks = 0;
    {
        rt::d3d12::VulkanDeviceInfo info{};
        info.get_instance_proc = d.instance_proc;
        info.instance = d.instance;
        info.physical = d.physical;
        info.device = d.device;
        info.queue = d.queue;
        info.queue_family = d.family;
        info.buffer_device_address = true;
        if (geometry) info.host_pointer_alignment = d.host_pointer_alignment;
        info.subgroup = d.subgroup;
        info.shader_dir = library.string();
        // Keep another compatibility device alive over the same physical GPU. Each owns its own arena.
        Ref<ID3D12Device> first;
        if (second_device) EXPECT(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&first.p)) == S_OK);
        ID3D12Device* inner = nullptr;  // owned by the proxy, as in d3d12_compute_test
        EXPECT(SUCCEEDED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&inner))));
        Ref<ID3D12Device> device;
        EXPECT(SUCCEEDED(rt::d3d12_xs::create_device_proxy(inner, __uuidof(ID3D12Device),
                                                           reinterpret_cast<void**>(&device.p))));
        rt::d3d12::set_device_outer(inner, device.p);

        // [0] two constants (s4, s5), [1] a root UAV (s6:s7), [2] a one-descriptor table (s8): the console compute
        // layout cs_user_data.comp reads.
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_CBV, rebind ? 65u : 1u, 1, 0, 0};
        D3D12_ROOT_PARAMETER params[3] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants = {0, 0, 2};
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[1].Descriptor = {0, 0};
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable = {1, &range};
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
        root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
        root_desc.Desc_1_0 = {3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        if (geometry) root_desc.Desc_1_0.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
        const auto root_blob = serialize(root_desc);
        EXPECT(!root_blob.empty());
        Ref<ID3D12RootSignature> root;
        EXPECT(SUCCEEDED(device->CreateRootSignature(0, root_blob.data(), root_blob.size(), IID_PPV_ARGS(&root.p))));
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
        pso_desc.pRootSignature = root.p;
        pso_desc.CS = {container.data(), container.size()};
        const IID pso_iid = __uuidof(ID3D12PipelineState);
        Ref<ID3D12PipelineState> pso;
        EXPECT(SUCCEEDED(reinterpret_cast<XboxComputePipelineFn>(slot(device.p, 62))(
            device.p, &pso_desc, 0, 0, &pso_iid, reinterpret_cast<void**>(&pso.p))));

        const D3D12_COMMAND_QUEUE_DESC queue_desc{};
        Ref<ID3D12CommandQueue> queue;
        Ref<ID3D12Fence> fence;
        EXPECT(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))));
        EXPECT(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));

        Context c;
        c.inner = inner;
        c.device = device.p;
        c.root = root.p;
        c.pso = pso.p;
        c.queue = queue.p;
        c.fence = fence.p;
        c.arena = rt::d3d12::device_arena_size(device.p);
        EXPECT(c.arena > 0);
        if (rebind) {
            range.NumDescriptors = 1;
            const auto narrow_blob = serialize(root_desc);
            Ref<ID3D12RootSignature> narrow;
            EXPECT(!narrow_blob.empty());
            EXPECT(device->CreateRootSignature(0, narrow_blob.data(), narrow_blob.size(), IID_PPV_ARGS(&narrow.p)) == S_OK);
            EXPECT(check_rebound_tables(c, narrow.p, !mirror_off, api_copy) == 0);
            EXPECT(d.validation_errors() == 0);
            return 0;
        }
        if (window_cap) {
            if (check_window_overflow(c)) return 1;
            if (d.validation_errors()) {
                std::fprintf(stderr, "%u validation errors\n", d.validation_errors());
                return 1;
            }
            std::printf("title memory past the window cap: the views were zero-size and the shader read zeros\n");
            return 0;
        }
        if (placed_write_back) {
            if (check_placed_write_back(c, geometry)) return 1;
            if (d.validation_errors()) {
                std::fprintf(stderr, "%u validation errors\n", d.validation_errors());
                return 1;
            }
            std::printf("title-placed buffer write back: the GPU's writes reached the title's pages\n");
            return 0;
        }
        if (keep_refill) {
            if (check_shadow_keep_refill(c)) return 1;
            if (d.validation_errors()) {
                std::fprintf(stderr, "%u validation errors\n", d.validation_errors());
                return 1;
            }
            return 0;
        }
        if (window_write_back_fault) {
            if (const int result = check_window_write_back_fault(c, window_write_back_tile)) return result;
            if (d.validation_errors()) {
                std::fprintf(stderr, "%u validation errors\n", d.validation_errors());
                return 1;
            }
            std::printf("title window write back fault: a write-back copy that faulted on a page another guard keeps "
                        "read-only returned, and the GPU's words reached the title's pages\n");
            return 0;
        }
        if (window_write_back) {
            if (check_window_write_back(c, geometry)) return 1;
            if (d.validation_errors()) {
                std::fprintf(stderr, "%u validation errors\n", d.validation_errors());
                return 1;
            }
            std::printf("title window write back: the GPU's writes reached the title's pages and the title's own "
                        "words won\n");
            return 0;
        }
        if (window_orphans) {
            if (check_window_orphans(c)) return 1;
            if (check_retired_window_write_back(c)) return 1;
            if (d.validation_errors()) {
                std::fprintf(stderr, "%u validation errors\n", d.validation_errors());
                return 1;
            }
            std::printf("title window orphans: no window overlapped another; the one nothing reached was retired\n");
            return 0;
        }
        if (geometry && check_declined_imports(c)) return 1;
        if (geometry && check_import_over_shadow(c)) return 1;
        if (check_kind(c, "read-write", PAGE_READWRITE, checks)) return 1;
        if (check_kind(c, "write-combined", PAGE_READWRITE | PAGE_WRITECOMBINE, checks)) return 1;
        if (check_title_memory(c, checks, geometry)) return 1;
        if (check_stream_pool(c, checks)) return 1;
        if (geometry && std::atoi(setting) == 16 && check_geometry_slab_tails(c)) return 1;
    }
    if (d.validation_errors()) {
        std::fprintf(stderr, "%u validation errors\n", d.validation_errors());
        return 1;
    }
    std::printf("title-placed buffers: a translated shader reached both page kinds through a root view and a buffer "
                "V#; %d descriptor-level negative controls held\n",
                checks);
    return 0;
}
