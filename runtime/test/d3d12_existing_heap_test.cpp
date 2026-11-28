// ID3D12Device3::OpenExistingHeapFromAddress, OpenExistingHeapFromFileMapping and ID3D12Device5::CreateLifetimeTracker
// on arguments the host D3D12 runtime refuses (null address, a file mapping that is not a handle, a null owner), all
// E_INVALIDARG on the RTX 4080 SUPER, Intel UHD 770 and WARP. Checked on the Vulkan-backed device and, when present,
// on the host devices too. With VK_EXT_external_memory_host, OpenExistingHeapFromAddress on real host memory is
// compared case by case with the host devices (open_cases: RTX 4090 and WARP agree), and a heap is opened on a
// committed range; without it, the device reports EXISTING_HEAPS unsupported and answers an accepted range E_NOTIMPL.
// No shaders or game data.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device3-openexistingheapfromaddress
#include "../shims/title_gpu_placements.h"
#include "../shims/xg_xs_layout.h"
#include "vk_test_device.h"
#include "host_adapter.h"
#include "host_wine.h"
#include "../shims/d3d12_com.h"
#include "../src/title_page_watch.h"
#include "../src/title_geometry_watch.h"
#include "../src/gfx10_descriptors.h"
#include "../src/deferred_image_binding.h"
#include <dxgi1_4.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "%s line %d: %s\n", who, __LINE__, #condition); return 1; } \
} while (0)
#define EXPECT_HR(expr, want) do { \
    const HRESULT got_ = (expr); \
    if (got_ != (want)) { \
        std::fprintf(stderr, "%s line %d: %s = 0x%08lX, expected 0x%08lX\n", who, __LINE__, #expr, \
                     static_cast<unsigned long>(got_), static_cast<unsigned long>(want)); \
        return 1; \
    } \
} while (0)

// The process heap as check_heap_lock_faults needs it: an allocation that waits while a thread holds the heap's lock.
// The NT heap's low-fragmentation front end serves most small sizes without that lock, so a real deadlock waits for an
// allocation its back end serves (measured natively: 8, 1000 and 4000-byte blocks wait while another thread holds the
// process heap's lock, 48 and 200-byte ones don't), which a short test can't count on. Here, while the gate is on,
// every operator new and delete of the program takes g_heap_gate shared, and the test's "title" threads that hold the
// heap's lock hold the gate exclusively: any allocation the layer makes while it holds a lock the write-fault handler
// waits for then deadlocks at once.
namespace {
SRWLOCK g_heap_gate = SRWLOCK_INIT;
std::atomic<bool> g_heap_gate_on{false};
struct HeapGate {
    const bool on = g_heap_gate_on.load(std::memory_order_acquire);
    HeapGate() {
        if (on) AcquireSRWLockShared(&g_heap_gate);
    }
    ~HeapGate() {
        if (on) ReleaseSRWLockShared(&g_heap_gate);
    }
};
}  // namespace

void* operator new(std::size_t bytes) {
    HeapGate gate;
    if (void* p = std::malloc(bytes ? bytes : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept {
    HeapGate gate;
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }

namespace {
template<class T>
struct Ref {
    T* p = nullptr;
    ~Ref() { if (p) p->Release(); }
    T* operator->() const { return p; }
};

int check(ID3D12Device* device, const char* who) {
    Ref<ID3D12Device3> device3;
    EXPECT(SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device3.p))));
    Ref<ID3D12Heap> heap;
    EXPECT_HR(device3->OpenExistingHeapFromAddress(nullptr, IID_PPV_ARGS(&heap.p)), E_INVALIDARG);
    EXPECT(heap.p == nullptr);
    EXPECT_HR(device3->OpenExistingHeapFromFileMapping(nullptr, IID_PPV_ARGS(&heap.p)), E_INVALIDARG);
    EXPECT_HR(device3->OpenExistingHeapFromFileMapping(reinterpret_cast<HANDLE>(0x1234), IID_PPV_ARGS(&heap.p)),
              E_INVALIDARG);
    EXPECT(heap.p == nullptr);
    Ref<ID3D12Device5> device5;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device5.p)))) {
        Ref<ID3D12LifetimeTracker> tracker;
        EXPECT_HR(device5->CreateLifetimeTracker(nullptr, IID_PPV_ARGS(&tracker.p)), E_INVALIDARG);
        EXPECT(tracker.p == nullptr);
    }
    std::printf("%s: existing-heap and lifetime-tracker refusals match the host runtime\n", who);
    return 0;
}

// The calls the host accepts or refuses on real host memory, one result per case, for the comparison in main().
struct Case {
    const char* name;
    HRESULT hr = S_OK;
    D3D12_HEAP_DESC desc{};
};

std::vector<Case> open_cases(ID3D12Device* device) {
    std::vector<Case> out;
    Ref<ID3D12Device3> device3;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device3.p)))) return out;
    auto run = [&](const char* name, const void* address) {
        Case c{name};
        Ref<ID3D12Heap> heap;
        c.hr = device3->OpenExistingHeapFromAddress(address, IID_PPV_ARGS(&heap.p));
        if (SUCCEEDED(c.hr) && heap.p) c.desc = heap->GetDesc();
        out.push_back(c);
    };
    constexpr SIZE_T k64 = 0x10000;
    // A whole 64 KiB committed read-write allocation, as the title's XMemVirtualAlloc gives it.
    void* whole = VirtualAlloc(nullptr, k64, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    run("64 KiB committed, at its base", whole);
    run("64 KiB committed, 4 KiB in", static_cast<char*>(whole) + 0x1000);
    // Two committed 64 KiB in one 192 KiB allocation whose last 64 KiB is only reserved.
    char* partial = static_cast<char*>(VirtualAlloc(nullptr, 3 * k64, MEM_RESERVE, PAGE_READWRITE));
    VirtualAlloc(partial, 2 * k64, MEM_COMMIT, PAGE_READWRITE);
    run("192 KiB reserved, first 128 KiB committed", partial);
    void* reserved = VirtualAlloc(nullptr, k64, MEM_RESERVE, PAGE_READWRITE);
    run("64 KiB reserved only", reserved);
    void* readonly = VirtualAlloc(nullptr, k64, MEM_RESERVE | MEM_COMMIT, PAGE_READONLY);
    run("64 KiB committed read-only", readonly);
    // The console title's graphics regions keep PAGE_WRITECOMBINE (or PAGE_NOCACHE) once XMemVirtualAlloc drops
    // the PAGE_GRAPHICS_* bits.
    void* combined = VirtualAlloc(nullptr, k64, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE | PAGE_WRITECOMBINE);
    run("64 KiB committed read-write write-combined", combined);
    void* uncached = VirtualAlloc(nullptr, k64, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE | PAGE_NOCACHE);
    run("64 KiB committed read-write uncached", uncached);
    void* big = VirtualAlloc(nullptr, 32 * k64, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    run("2 MiB committed", big);
    // The heap keeps no hold on the memory the test frees here; every heap above is already released.
    for (void* p : {whole, static_cast<void*>(partial), reserved, readonly, combined, uncached, big}) VirtualFree(p, 0, MEM_RELEASE);
    return out;
}

// Each case answers as the host does: the same HRESULT, and for an opened heap the same description.
int compare_cases(const std::vector<Case>& ours, const std::vector<Case>& host, const char* who) {
    int failures = 0;
    for (size_t i = 0; i < ours.size() && i < host.size(); ++i) {
        const D3D12_HEAP_DESC& a = ours[i].desc;
        const D3D12_HEAP_DESC& b = host[i].desc;
        // The host's Alignment varies from run to run for the same allocation (64 KiB on some runs, 4 MiB on others,
        // on an RX 9070 XT, an RTX 5090 and WARP; found in review), so it is not compared: the layer's must be 64 KiB
        // for an opened heap, and the host's either of the two.
        const bool alignment_ok =
            !SUCCEEDED(ours[i].hr) ||
            (a.Alignment == D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT &&
             (b.Alignment == D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT || b.Alignment == 4ull * 1024 * 1024));
        const bool same = ours[i].hr == host[i].hr && a.SizeInBytes == b.SizeInBytes && alignment_ok &&
                          a.Properties.Type == b.Properties.Type &&
                          a.Properties.CPUPageProperty == b.Properties.CPUPageProperty &&
                          a.Properties.MemoryPoolPreference == b.Properties.MemoryPoolPreference && a.Flags == b.Flags;
        if (!same) {
            std::fprintf(stderr, "vulkan vs %s: \"%s\" differs (hr 0x%08lX vs 0x%08lX)\n", who, ours[i].name,
                         static_cast<unsigned long>(ours[i].hr), static_cast<unsigned long>(host[i].hr));
            ++failures;
        }
    }
    if (!failures) std::printf("vulkan vs %s: %zu OpenExistingHeapFromAddress cases match\n", who, ours.size());
    return failures;
}

// The heap is the caller's memory: CheckFeatureSupport says so, and the heap stays valid while it is held.
// Without the extension the feature is unsupported and an accepted range is E_NOTIMPL.
int check_import(ID3D12Device* device, bool supported) {
    const char* who = "vulkan import";
    D3D12_FEATURE_DATA_EXISTING_HEAPS feature{};
    EXPECT(SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_EXISTING_HEAPS, &feature, sizeof(feature))));
    EXPECT(bool(feature.Supported) == supported);
    Ref<ID3D12Device3> device3;
    EXPECT(SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device3.p))));
    constexpr SIZE_T size = 4 * 0x10000;
    void* memory = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    EXPECT(memory != nullptr);
    {
        Ref<ID3D12Heap> heap;
        const HRESULT hr = device3->OpenExistingHeapFromAddress(memory, IID_PPV_ARGS(&heap.p));
        if (!supported) {
            EXPECT_HR(hr, E_NOTIMPL);
            EXPECT(heap.p == nullptr);
        } else {
            EXPECT_HR(hr, S_OK);
            EXPECT(heap.p != nullptr);
            EXPECT(heap->GetDesc().SizeInBytes == size);
            // The CPU still owns the pages: a write through them is what the GPU will read.
            static_cast<unsigned char*>(memory)[size - 1] = 0x5a;
            // A placed resource in it is refused until the placed path addresses existing heaps.
            D3D12_RESOURCE_DESC buffer{};
            buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width = 0x10000;
            buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
            buffer.SampleDesc.Count = 1;
            buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            Ref<ID3D12Resource> placed;
            EXPECT_HR(device->CreatePlacedResource(heap.p, 0, &buffer, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                   IID_PPV_ARGS(&placed.p)),
                      E_NOTIMPL);
        }
    }
    EXPECT(VirtualFree(memory, 0, MEM_RELEASE));
    std::printf("%s: existing heap %s\n", who, supported ? "opened on the caller's memory" : "unsupported, as reported");
    return 0;
}

// Xbox device slot 133's backend (rt::d3d12::create_buffer_at_host_address): a buffer on the caller's own pages.
// Its GPU VA is the address itself, and what the CPU writes there is what a GPU copy reads, with no upload: the
// pattern is written after the buffer exists and read back through a READBACK buffer. Checked on a VirtualAlloc range
// and on a view of a pagefile-backed section, the form xmem's physical pages are mapped in. A texture gets a committed
// texture whose GPU VA is the same title address (its pixels do not alias the pages yet); an address off the buffer
// alignment gets the shadow fallback at the same VA.
// `may_refuse`: the driver is allowed to refuse importing these pages, and the layer then answers with its fallback.
// RADV imports host memory through amdgpu userptr, which takes anonymous memory only, so it refuses a view of a
// pagefile-backed section with EPERM (measured on an RX 9060 XT under Wine, found in review), while NVIDIA and
// AMD's Windows driver import it. The fallback is checked to exist; the readback below needs the import. Since the shadow fallback
// keeps the title's VA, a refused import still reads back through the shadow.
int check_title_placed(ID3D12Device* device, const char* kind, void* pages, UINT64 size, bool may_refuse = false, bool buffers_only = false) {
    const char* who = "vulkan slot-133 buffer";
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const std::vector<std::uint8_t> initial(static_cast<std::uint8_t*>(pages),
                                             static_cast<std::uint8_t*>(pages) + size);
    Ref<ID3D12Resource> buffer;
    EXPECT_HR(rt::d3d12::create_buffer_at_host_address(device, pages, desc, 0xFFFFFFFFu, IID_PPV_ARGS(&buffer.p)), S_OK);
    if (may_refuse && buffer->GetGPUVirtualAddress() != reinterpret_cast<D3D12_GPU_VIRTUAL_ADDRESS>(pages)) {
        std::printf("%s: %s, %llu bytes: this driver refused the import; the fallback resource answered\n", who, kind,
                    static_cast<unsigned long long>(size));
        return 0;
    }
    EXPECT(buffer->GetGPUVirtualAddress() == reinterpret_cast<D3D12_GPU_VIRTUAL_ADDRESS>(pages));
    EXPECT(std::memcmp(pages, initial.data(), initial.size()) == 0);
    auto* bytes = static_cast<std::uint8_t*>(pages);
    for (UINT64 i = 0; i < size; ++i) bytes[i] = static_cast<std::uint8_t>(i * 131u + 7u);

    D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK};
    Ref<ID3D12Resource> readback;
    EXPECT(SUCCEEDED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&readback.p))));
    D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    Ref<ID3D12CommandQueue> queue;
    Ref<ID3D12CommandAllocator> allocator;
    Ref<ID3D12GraphicsCommandList> list;
    Ref<ID3D12Fence> fence;
    EXPECT(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))));
    EXPECT(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
    EXPECT(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, nullptr,
                                               IID_PPV_ARGS(&list.p))));
    list->CopyBufferRegion(readback.p, 0, buffer.p, 0, size);
    EXPECT(SUCCEEDED(list->Close()));
    ID3D12CommandList* lists[] = {list.p};
    queue->ExecuteCommandLists(1, lists);
    EXPECT(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));
    EXPECT(SUCCEEDED(queue->Signal(fence.p, 1)));
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    EXPECT(SUCCEEDED(fence->SetEventOnCompletion(1, done)));
    EXPECT(WaitForSingleObject(done, 10000) == WAIT_OBJECT_0);
    CloseHandle(done);
    void* mapped = nullptr;
    EXPECT(SUCCEEDED(readback->Map(0, nullptr, &mapped)));
    const bool same = std::memcmp(mapped, pages, static_cast<size_t>(size)) == 0;
    readback->Unmap(0, nullptr);
    EXPECT(same);

    // Reverse the copy: a real import writes the caller's pages, including for sub-page allocations.
    D3D12_HEAP_PROPERTIES upload_heap{D3D12_HEAP_TYPE_UPLOAD};
    Ref<ID3D12Resource> upload;
    EXPECT(SUCCEEDED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(&upload.p))));
    std::vector<std::uint8_t> reversed(static_cast<size_t>(size));
    for (size_t i = 0; i < reversed.size(); ++i) reversed[i] = static_cast<std::uint8_t>(i * 17u + 0x39u);
    EXPECT(SUCCEEDED(upload->Map(0, nullptr, &mapped)));
    std::memcpy(mapped, reversed.data(), reversed.size());
    upload->Unmap(0, nullptr);
    EXPECT(SUCCEEDED(allocator->Reset()));
    EXPECT(SUCCEEDED(list->Reset(allocator.p, nullptr)));
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {buffer.p, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST};
    list->ResourceBarrier(1, &barrier);
    list->CopyBufferRegion(buffer.p, 0, upload.p, 0, size);
    EXPECT(SUCCEEDED(list->Close()));
    queue->ExecuteCommandLists(1, lists);
    EXPECT(rt::d3d12::last_execute_result(queue.p) == leonida::ReplayResult::success);
    EXPECT(SUCCEEDED(queue->Signal(fence.p, 2)));
    done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    EXPECT(done != nullptr && SUCCEEDED(fence->SetEventOnCompletion(2, done)));
    const DWORD waited = WaitForSingleObject(done, 10000);
    CloseHandle(done);
    EXPECT(waited == WAIT_OBJECT_0);
    // A refused import (`may_refuse`) is a shadow at the same VA: filled from the pages before every submission but
    // never written back, so only a real import shows the copy in the caller's pages.
    if (!(may_refuse && rt::d3d12::title_shadow_buffer(buffer.p))) {
        EXPECT(std::memcmp(pages, reversed.data(), reversed.size()) == 0);
    }

    if (buffers_only) {
        // RGBA16 requires an 8-byte Vulkan buffer-image offset, while an imported host prefix may be +4.
        // Exercise both directions and verify every untouched host byte, including row padding.
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = 1;
        td.Height = 2;
        td.DepthOrArraySize = td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
        td.SampleDesc.Count = 1;
        D3D12_HEAP_PROPERTIES gpu_heap{D3D12_HEAP_TYPE_DEFAULT};
        Ref<ID3D12Resource> image;
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&gpu_heap, D3D12_HEAP_FLAG_NONE, &td,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&image.p))));
        D3D12_TEXTURE_COPY_LOCATION linear{};
        linear.pResource = buffer.p;
        linear.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        linear.PlacedFootprint.Footprint = {td.Format, 1, 2, 1, 256};
        D3D12_TEXTURE_COPY_LOCATION tex{};
        tex.pResource = image.p;
        tex.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        EXPECT(SUCCEEDED(allocator->Reset()));
        EXPECT(SUCCEEDED(list->Reset(allocator.p, nullptr)));
        barrier.Transition = {buffer.p, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE};
        list->ResourceBarrier(1, &barrier);
        list->CopyTextureRegion(&tex, 0, 0, 0, &linear, nullptr);
        EXPECT(SUCCEEDED(list->Close()));
        queue->ExecuteCommandLists(1, lists);
        EXPECT(rt::d3d12::last_execute_result(queue.p) == leonida::ReplayResult::success);
        EXPECT(SUCCEEDED(queue->Signal(fence.p, 3)));
        done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        EXPECT(done && SUCCEEDED(fence->SetEventOnCompletion(3, done)));
        const DWORD uploaded = WaitForSingleObject(done, 10000);
        CloseHandle(done);
        EXPECT(uploaded == WAIT_OBJECT_0);
        std::memset(pages, 0x6d, static_cast<size_t>(size));
        EXPECT(SUCCEEDED(allocator->Reset()));
        EXPECT(SUCCEEDED(list->Reset(allocator.p, nullptr)));
        D3D12_RESOURCE_BARRIER transitions[2] = {barrier, barrier};
        transitions[0].Transition = {image.p, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                     D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE};
        transitions[1].Transition = {buffer.p, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                     D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST};
        list->ResourceBarrier(2, transitions);
        list->CopyTextureRegion(&linear, 0, 0, 0, &tex, nullptr);
        EXPECT(SUCCEEDED(list->Close()));
        queue->ExecuteCommandLists(1, lists);
        EXPECT(rt::d3d12::last_execute_result(queue.p) == leonida::ReplayResult::success);
        EXPECT(SUCCEEDED(queue->Signal(fence.p, 4)));
        done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        EXPECT(done && SUCCEEDED(fence->SetEventOnCompletion(4, done)));
        const DWORD downloaded = WaitForSingleObject(done, 10000);
        CloseHandle(done);
        EXPECT(downloaded == WAIT_OBJECT_0);
        for (size_t i = 0; i < static_cast<size_t>(size); ++i) {
            const bool texel = i < 8 || (i >= 256 && i < 264);
            EXPECT(bytes[i] == (texel ? reversed[i] : 0x6d));
        }
        return 0;
    }

    D3D12_RESOURCE_DESC texture = desc;
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = texture.Height = 64;
    texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // A texture isn't placed on the pages yet: the title gets an ordinary committed texture instead of a refusal, and
    // its GPU VA is the title's address (the console behavior: a title clears a texture through that pointer).
    Ref<ID3D12Resource> placed_texture;
    EXPECT_HR(rt::d3d12::create_buffer_at_host_address(device, pages, texture, 0x6u, IID_PPV_ARGS(&placed_texture.p)), S_OK);
    EXPECT(placed_texture.p != nullptr && placed_texture->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D);
    EXPECT(placed_texture->GetGPUVirtualAddress() == reinterpret_cast<D3D12_GPU_VIRTUAL_ADDRESS>(pages));
    // The layout is validated as a CreateCommittedResource state: RENDER_TARGET (2) on a texture without
    // ALLOW_RENDER_TARGET is refused, and the COMMON retry still gives the title its texture. A description the
    // checks refuse outright (more mips than the size has) is refused, not created.
    Ref<ID3D12Resource> target_layout;
    EXPECT_HR(rt::d3d12::create_buffer_at_host_address(device, pages, texture, 0x2u, IID_PPV_ARGS(&target_layout.p)), S_OK);
    EXPECT(target_layout.p != nullptr);
    EXPECT(target_layout->GetGPUVirtualAddress() == reinterpret_cast<D3D12_GPU_VIRTUAL_ADDRESS>(pages));
    D3D12_RESOURCE_DESC too_many_mips = texture;
    too_many_mips.MipLevels = 12;
    Ref<ID3D12Resource> refused_texture;
    EXPECT(FAILED(rt::d3d12::create_buffer_at_host_address(device, pages, too_many_mips, 0x6u,
                                                           IID_PPV_ARGS(&refused_texture.p))) &&
           refused_texture.p == nullptr);
    // An address off the buffer alignment can't be bound inside the import: a shadow buffer answers, at the same VA
    // (a driver whose buffer alignment is 1 imports it; the VA is the address either way).
    Ref<ID3D12Resource> fallback;
    desc.Width = size - 1;
    EXPECT_HR(rt::d3d12::create_buffer_at_host_address(device, bytes + 1, desc, 0xFFFFFFFFu, IID_PPV_ARGS(&fallback.p)), S_OK);
    EXPECT(fallback->GetGPUVirtualAddress() == reinterpret_cast<D3D12_GPU_VIRTUAL_ADDRESS>(bytes + 1));
    std::printf("%s: %s, %llu bytes: GPU VA is the address, a GPU copy reads the CPU's writes\n", who, kind,
                static_cast<unsigned long long>(size));
    return 0;
}

std::uint8_t pattern_byte(UINT64 i, unsigned seed) {
    return static_cast<std::uint8_t>(i * 131u + (i >> 8) * 7u + seed);
}

// Distinct, simultaneously live GPU resources may occupy the same console placement.
int check_placed_texture_views(ID3D12Device* device, void* pages) {
    const char* who = "placed texture views";
    D3D12_RESOURCE_DESC array{};
    array.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    array.Width = array.Height = 8;
    array.DepthOrArraySize = 6;
    array.MipLevels = 1;
    array.SampleDesc.Count = 1;
    array.Format = DXGI_FORMAT_R32_UINT;
    array.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    // A title-authored T# addresses the placement, even when newer resources share those pages.
    auto layers = array;
    layers.Width = layers.Height = 512;
    layers.Format = DXGI_FORMAT_R16_UNORM;
    auto face = layers;
    face.DepthOrArraySize = 1;
    rt::d3d12::TitleGpuPlacements<D3D12_RESOURCE_DESC> placements;
    placements.add(0x10000, &layers);
    placements.add(0x10000, &face);
    leonida::gfx10::Image raw{};
    raw.width = raw.height = 512;
    raw.type = leonida::gfx10::ImageType::image_2d_array;
    raw.mip_count = 1;
    raw.layer_count = 6;
    const auto fits = [&](const auto& d) { return placements.matches(raw, d); };
    EXPECT(placements.find(0x20000, 0x10000, fits) == &layers);
    placements.remove(0x10000, &layers);
    EXPECT(!placements.find(0x10000, 0, fits));
    Ref<ID3D12Resource> older, newer;
    EXPECT_HR(rt::d3d12::create_buffer_at_host_address(device, pages, array, 0, IID_PPV_ARGS(&older.p)), S_OK);
    D3D12_RESOURCE_DESC single = array;
    single.DepthOrArraySize = 1;
    EXPECT_HR(rt::d3d12::create_buffer_at_host_address(device, pages, single, 0, IID_PPV_ARGS(&newer.p)), S_OK);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = array.Format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    uav.Texture2DArray.FirstArraySlice = 5;
    uav.Texture2DArray.ArraySize = 1;
    std::array<uint32_t, 14> resource{};
    std::array<uint32_t, 16> view{};
    leonida::gfx10::ImageWords placed{}, expected{};
    std::memcpy(resource.data(), &array, sizeof(array));
    std::memcpy(view.data(), &uav, sizeof(uav));
    const auto va = reinterpret_cast<UINT64>(pages);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = array.Format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2DArray.MipLevels = 1;
    srv.Texture2DArray.ArraySize = 6;
    std::array<uint32_t, 16> srv_words{};
    std::memcpy(srv_words.data(), &srv, sizeof(srv));
    const D3D12_CPU_DESCRIPTOR_HANDLE slot{reinterpret_cast<SIZE_T>(placed.data())};
    EXPECT(!rt::d3d12::write_placed_texture_view(device, false, resource.data(), srv_words.data(), 0, slot) &&
           placed == leonida::gfx10::ImageWords{});
    EXPECT(rt::d3d12::write_placed_texture_view(device, false, resource.data(), srv_words.data(), va, slot));
    leonida::gfx10::Image restored{};
    EXPECT(leonida::gfx10::decode(placed, restored) && restored.layer_count == 6);
    EXPECT(rt::d3d12::write_placed_texture_view(device, true, resource.data(), view.data(), va,
                                                 {reinterpret_cast<SIZE_T>(placed.data())}));
    device->CreateUnorderedAccessView(older.p, nullptr, &uav, {reinterpret_cast<SIZE_T>(expected.data())});
    leonida::gfx10::Image decoded{};
    if (rt::d3d12::bindless_set_layout(device)) {
        std::uint32_t key[8];
        std::memcpy(key, placed.data(), sizeof(key));
        EXPECT(rt::d3d12::bindless_hash_lookup(device, key) != 0);
        std::puts("placed texture UAV registered in the GPU descriptor table");
    }
    EXPECT(placed == expected && leonida::gfx10::decode(placed, decoded) && decoded.first_layer == 5 &&
           decoded.layer_count == 1 && decoded.mip_count == 1);
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav.Texture2D = {};
    device->CreateUnorderedAccessView(newer.p, nullptr, &uav, {reinterpret_cast<SIZE_T>(expected.data())});
    leonida::gfx10::Image other{};
    EXPECT(leonida::gfx10::decode(expected, other) && other.address != decoded.address);
    EXPECT(!rt::d3d12::write_placed_texture_view(device, true, resource.data(), view.data(), 0,
                                                  {reinterpret_cast<SIZE_T>(placed.data())}));
    EXPECT(placed == leonida::gfx10::ImageWords{});
    auto volume = single;
    volume.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    volume.DepthOrArraySize = 8;
    volume.MipLevels = 2;
    Ref<ID3D12Resource> volume_target;
    EXPECT_HR(rt::d3d12::create_buffer_at_host_address(device, pages, volume, 0, IID_PPV_ARGS(&volume_target.p)), S_OK);
    std::memcpy(resource.data(), &volume, sizeof(volume));
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
    uav.Texture3D = {1, 0, 4};
    std::memcpy(view.data(), &uav, sizeof(uav));
    EXPECT(rt::d3d12::write_placed_texture_view(device, true, resource.data(), view.data(), va,
                                                 {reinterpret_cast<SIZE_T>(placed.data())}));
    EXPECT(leonida::gfx10::decode(placed, decoded) && decoded.type == leonida::gfx10::ImageType::image_3d &&
           decoded.first_mip == 1 && decoded.mip_count == 1);
    uav.Texture3D.WSize = 2;
    std::memcpy(view.data(), &uav, sizeof(uav));
    EXPECT(!rt::d3d12::write_placed_texture_view(device, true, resource.data(), view.data(), va,
                                                  {reinterpret_cast<SIZE_T>(placed.data())}) &&
           placed == leonida::gfx10::ImageWords{});
    return 0;
}

// The shadow fallback of slot 133: pages the driver does not import (write-combined, uncached, not all committed)
// still give a buffer whose GPU VA is the address. It is a copy of the pages, made again before every submission:
// a pattern written to the first `committed` bytes after the buffer exists is what a GPU copy in the next
// ExecuteCommandLists reads, and so is a second pattern in the submission after. Bytes past `committed` are not
// committed: they are never read, and the GPU sees zeros there. The same holds where the range was imported after
// all (a Wine that drops the cache modifier), so the caller need not know which path answered.
int check_title_placed_shadow(ID3D12Device* device, const char* kind, void* pages, UINT64 committed, UINT64 size) {
    const char* who = "vulkan slot-133 shadow";
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Ref<ID3D12Resource> buffer;
    EXPECT_HR(rt::d3d12::create_buffer_at_host_address(device, pages, desc, 0xFFFFFFFFu, IID_PPV_ARGS(&buffer.p)), S_OK);
    EXPECT(buffer->GetGPUVirtualAddress() == reinterpret_cast<D3D12_GPU_VIRTUAL_ADDRESS>(pages));
    D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK};
    Ref<ID3D12Resource> readback;
    EXPECT(SUCCEEDED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&readback.p))));
    D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    Ref<ID3D12CommandQueue> queue;
    Ref<ID3D12CommandAllocator> allocator;
    Ref<ID3D12GraphicsCommandList> list;
    Ref<ID3D12Fence> fence;
    EXPECT(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))));
    EXPECT(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
    EXPECT(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, nullptr,
                                               IID_PPV_ARGS(&list.p))));
    EXPECT(SUCCEEDED(list->Close()));
    EXPECT(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    EXPECT(done != nullptr);
    auto* bytes = static_cast<std::uint8_t*>(pages);
    UINT64 fence_value = 0;
    for (const unsigned seed : {7u, 93u}) {
        // Written after the buffer exists (and, the second time, after a submission read the first pattern).
        for (UINT64 i = 0; i < committed; ++i) bytes[i] = pattern_byte(i, seed);
        EXPECT(SUCCEEDED(allocator->Reset()));
        EXPECT(SUCCEEDED(list->Reset(allocator.p, nullptr)));
        list->CopyBufferRegion(readback.p, 0, buffer.p, 0, size);
        EXPECT(SUCCEEDED(list->Close()));
        ID3D12CommandList* lists[] = {list.p};
        queue->ExecuteCommandLists(1, lists);
        EXPECT(SUCCEEDED(queue->Signal(fence.p, ++fence_value)));
        EXPECT(SUCCEEDED(fence->SetEventOnCompletion(fence_value, done)));
        EXPECT(WaitForSingleObject(done, 30000) == WAIT_OBJECT_0);
        void* mapped = nullptr;
        EXPECT(SUCCEEDED(readback->Map(0, nullptr, &mapped)));
        const auto* got = static_cast<const std::uint8_t*>(mapped);
        UINT64 wrong = size;
        for (UINT64 i = 0; i < size && wrong == size; ++i)
            if (got[i] != (i < committed ? pattern_byte(i, seed) : 0)) wrong = i;
        readback->Unmap(0, nullptr);
        if (wrong != size)
            std::fprintf(stderr, "%s: %s: pattern %u: first wrong byte at %llu\n", who, kind, seed,
                         static_cast<unsigned long long>(wrong));
        EXPECT(wrong == size);
    }
    CloseHandle(done);
    std::printf("%s: %s, %llu bytes (%llu committed): GPU VA is the address, each submission reads the pages' "
                "latest bytes\n",
                who, kind, static_cast<unsigned long long>(size), static_cast<unsigned long long>(committed));
    return 0;
}

// Independent of OpenExistingHeapFromAddress: only actual imported-buffer copies and surrounding-byte checks.
int check_host_prefix_buffers(ID3D12Device* device) {
    constexpr SIZE_T size = 4 * 0x10000;
    auto* pages = static_cast<unsigned char*>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!pages) return 1;
    int failures = 0;
    for (unsigned prefix : {0u, 4u, 8u, 16u, 32u, 64u}) {
        std::memset(pages, 0xa5, size);
        failures += check_title_placed(device, "host prefix buffer", pages + prefix, 0x1000 - prefix,
                                      false, true);
        for (unsigned i = 0; i < prefix; ++i) failures += pages[i] != 0xa5;
        for (SIZE_T i = 0x1000; i < size; ++i) failures += pages[i] != 0xa5;
    }
    VirtualFree(pages, 0, MEM_RELEASE);
    std::printf("host prefix buffers: 0/4/8/16/32/64-byte offsets, bidirectional buffer/texture copies and surrounding bytes: %s\n",
                failures ? "FAIL" : "PASS");
    return failures;
}

int check_title_placed_all(ID3D12Device* device) {
    constexpr SIZE_T size = 4 * 0x10000;
    int failures = 0;
    void* pages = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!pages) return 1;
    failures += check_title_placed(device, "VirtualAlloc range", pages, size);
    failures += check_placed_texture_views(device, pages);
    // Inside a page, as the title places small UAV buffers (64-byte aligned, found in review on an RTX 5090): the
    // import starts at the page and the buffer is bound at the offset.
    failures += check_title_placed(device, "64 bytes into a page", static_cast<char*>(pages) + 64, 0x1000 - 64);
    failures += check_host_prefix_buffers(device);
    VirtualFree(pages, 0, MEM_RELEASE);
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, DWORD(size), nullptr);
    void* view = section ? MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, size) : nullptr;
    if (!view) return failures + 1;
    failures += check_title_placed(device, "section view", view, size, /*may_refuse=*/true);
    UnmapViewOfFile(view);
    CloseHandle(section);

    // Cache-modified ranges are not imported (WC imports read wrong bytes on an RTX 5090 in review): a shadow buffer
    // answers at the same VA. Some Wine versions report a requested WC allocation as plain read-write memory; check
    // the protection actually supplied. PAGE_NOCACHE also exercises the fallback on those hosts, rather than
    // dropping the cache-protection check.
    const char* who = "vulkan slot-133 buffer";
    for (DWORD modifier : {DWORD(PAGE_WRITECOMBINE), DWORD(PAGE_NOCACHE)}) {
        void* pages = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE | modifier);
        EXPECT(pages != nullptr);
        MEMORY_BASIC_INFORMATION region{};
        EXPECT(VirtualQuery(pages, &region, sizeof(region)) == sizeof(region));
        const bool cache_modified = (region.Protect & (PAGE_WRITECOMBINE | PAGE_NOCACHE)) != 0;
        if (!cache_modified) EXPECT(region.Protect == PAGE_READWRITE);
        const int failed = check_title_placed_shadow(device, modifier == PAGE_WRITECOMBINE ? "write-combined range"
                                                                                           : "uncached range",
                                                     pages, size, size);
        std::printf("%s: cache modifier 0x%lx, reported protection 0x%lx: %s\n", who, modifier, region.Protect,
                    failed ? "FAILED"
                           : cache_modified ? "shadow buffer verified"
                                            : "modifier not reported; read-write import verified");
        failures += failed;
        VirtualFree(pages, 0, MEM_RELEASE);
    }
    {
        // A larger write-combined range, so the per-submission copy the layer logs is measured on more than a few
        // pages (retail places 182 MiB, 74 MiB of it write-combined).
        constexpr SIZE_T large = 64u << 20;
        void* pages = VirtualAlloc(nullptr, large, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE | PAGE_WRITECOMBINE);
        EXPECT(pages != nullptr);
        failures += check_title_placed_shadow(device, "64 MiB write-combined range", pages, large, large);
        VirtualFree(pages, 0, MEM_RELEASE);
    }
    char* partial = static_cast<char*>(VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_READWRITE));
    EXPECT(partial != nullptr && VirtualAlloc(partial, size / 2, MEM_COMMIT, PAGE_READWRITE) == partial);
    // Pages not committed yet (xmem commits on first touch) are not imported either: the shadow copies the committed
    // half and never reads the other, which a GPU copy sees as zeros.
    failures += check_title_placed_shadow(device, "half-committed range", partial, size / 2, size);
    VirtualFree(partial, 0, MEM_RELEASE);
    return failures;
}

// Submits a GPU copy of all of `buffer` to a READBACK buffer and waits for it: what the GPU read at that submission.
struct GpuReader {
    const char* who = "vulkan slot-133 written pages";
    ID3D12Device* device = nullptr;
    UINT64 size = 0;
    Ref<ID3D12Resource> readback;
    Ref<ID3D12CommandQueue> queue;
    Ref<ID3D12CommandAllocator> allocator;
    Ref<ID3D12GraphicsCommandList> list;
    Ref<ID3D12Fence> fence;
    HANDLE done = nullptr;
    UINT64 fence_value = 0;

    ~GpuReader() {
        if (done) CloseHandle(done);
    }
    int init(ID3D12Device* d, UINT64 bytes) {
        device = d;
        size = bytes;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK};
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&readback.p))));
        D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
        EXPECT(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))));
        EXPECT(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
        EXPECT(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, nullptr,
                                                   IID_PPV_ARGS(&list.p))));
        EXPECT(SUCCEEDED(list->Close()));
        EXPECT(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));
        done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        EXPECT(done != nullptr);
        return 0;
    }
    int read(ID3D12Resource* buffer, std::vector<std::uint8_t>& out) {
        EXPECT(SUCCEEDED(allocator->Reset()));
        EXPECT(SUCCEEDED(list->Reset(allocator.p, nullptr)));
        list->CopyBufferRegion(readback.p, 0, buffer, 0, size);
        EXPECT(SUCCEEDED(list->Close()));
        ID3D12CommandList* lists[] = {list.p};
        queue->ExecuteCommandLists(1, lists);
        EXPECT(SUCCEEDED(queue->Signal(fence.p, ++fence_value)));
        EXPECT(SUCCEEDED(fence->SetEventOnCompletion(fence_value, done)));
        EXPECT(WaitForSingleObject(done, 30000) == WAIT_OBJECT_0);
        void* mapped = nullptr;
        EXPECT(SUCCEEDED(readback->Map(0, nullptr, &mapped)));
        out.assign(static_cast<const std::uint8_t*>(mapped), static_cast<const std::uint8_t*>(mapped) + size);
        readback->Unmap(0, nullptr);
        return 0;
    }
};

// The first byte where the GPU's copy differs from `want`, or -1.
long long first_difference(const std::vector<std::uint8_t>& got, const std::vector<std::uint8_t>& want) {
    for (size_t i = 0; i < got.size() && i < want.size(); ++i)
        if (got[i] != want[i]) return static_cast<long long>(i);
    return got.size() == want.size() ? -1 : static_cast<long long>(std::min(got.size(), want.size()));
}

ID3D12Resource* place_shadow(ID3D12Device* device, void* pages, UINT64 size) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* buffer = nullptr;
    if (FAILED(rt::d3d12::create_buffer_at_host_address(device, pages, desc, 0xFFFFFFFFu, IID_PPV_ARGS(&buffer))))
        return nullptr;
    if (buffer->GetGPUVirtualAddress() != reinterpret_cast<D3D12_GPU_VIRTUAL_ADDRESS>(pages)) {
        buffer->Release();
        return nullptr;
    }
    return buffer;
}

DWORD protection_of(const void* address) {
    MEMORY_BASIC_INFORMATION region{};
    return VirtualQuery(address, &region, sizeof(region)) && region.State == MEM_COMMIT ? region.Protect : 0;
}

// A shadow is copied again only where the title wrote since the last submission (the shadow's pages are read-only
// between submissions, and a write fault marks them). Checked by what a GPU copy reads after each step, compared byte
// for byte with what the CPU wrote, over the whole buffer:
//   1. a pattern over everything committed, then a submission;
//   2. one 4 KiB page rewritten (plus the last byte of the range), then a submission: the new page arrives and the rest
//      keeps the first pattern;
//   3. nothing written: the same bytes again;
//   4. a page that was read-only after a submission is written by a second thread while the first waits, then a
//      submission.
// `committed` bytes from the start are committed; the rest reads as zeros. The protection after a submission is
// printed: PAGE_READONLY with the range's cache attribute when the writes are watched.
int check_written_pages(ID3D12Device* device, const char* kind, std::uint8_t* pages, UINT64 committed, UINT64 size) {
    const char* who = "vulkan slot-133 written pages";
    Ref<ID3D12Resource> buffer;
    buffer.p = place_shadow(device, pages, size);
    EXPECT(buffer.p != nullptr);
    GpuReader reader;
    if (reader.init(device, size)) return 1;
    std::vector<std::uint8_t> want(size, 0), got;
    for (UINT64 i = 0; i < committed; ++i) want[i] = pages[i] = pattern_byte(i, 11);
    EXPECT(reader.read(buffer.p, got) == 0);
    EXPECT(first_difference(got, want) == -1);
    const DWORD watched = protection_of(pages);

    // Step 2: one page in the middle of the committed part, and the last committed byte.
    const UINT64 page = (committed / 2) & ~UINT64(0xfff);
    for (UINT64 i = page; i < page + 0x1000 && i < committed; ++i) want[i] = pages[i] = pattern_byte(i, 201);
    want[committed - 1] = pages[committed - 1] = 0xa5;
    EXPECT(reader.read(buffer.p, got) == 0);
    long long wrong = first_difference(got, want);
    if (wrong >= 0) std::fprintf(stderr, "%s: %s: one page rewritten: first wrong byte at %lld\n", who, kind, wrong);
    EXPECT(wrong == -1);

    // Step 3: nothing written since.
    EXPECT(reader.read(buffer.p, got) == 0);
    EXPECT(first_difference(got, want) == -1);

    // Step 4: a write from another thread, to the first page.
    HANDLE writer = CreateThread(nullptr, 0, [](void* p) -> DWORD {
        auto* bytes = static_cast<std::uint8_t*>(p);
        for (int i = 0; i < 64; ++i) bytes[i] = std::uint8_t(0xc0 + i);
        return 0;
    }, pages, 0, nullptr);
    EXPECT(writer != nullptr && WaitForSingleObject(writer, 10000) == WAIT_OBJECT_0);
    CloseHandle(writer);
    for (int i = 0; i < 64; ++i) want[i] = std::uint8_t(0xc0 + i);
    EXPECT(reader.read(buffer.p, got) == 0);
    EXPECT(first_difference(got, want) == -1);
    const DWORD after = protection_of(pages);
    buffer.p->Release();
    buffer.p = nullptr;
    // Released: the pages are the title's again, read-write, and a write doesn't fault.
    EXPECT(protection_of(pages) == 0 || (protection_of(pages) & 0xff) == PAGE_READWRITE);
    pages[0] ^= 1;
    std::printf("%s: %s, %llu bytes (%llu committed): a rewritten page, a write from another thread and an unchanged "
                "submission all read back right; protection between submissions 0x%lx, 0x%lx\n",
                who, kind, static_cast<unsigned long long>(size), static_cast<unsigned long long>(committed), watched,
                after);
    return 0;
}

// Pages that change under a live shadow: committed later, decommitted, written after a submission. A reserved range
// whose first half is committed; then a page of the second half is committed and written, and the first 64 KiB is
// decommitted (it reads as zeros again).
int check_commit_changes(ID3D12Device* device) {
    const char* who = "vulkan slot-133 written pages";
    constexpr SIZE_T size = 4 * 0x10000;
    auto* pages = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_READWRITE));
    EXPECT(pages != nullptr && VirtualAlloc(pages, size / 2, MEM_COMMIT, PAGE_READWRITE) == pages);
    int failures = 0;
    {
        Ref<ID3D12Resource> buffer;
        buffer.p = place_shadow(device, pages, size);
        EXPECT(buffer.p != nullptr);
        GpuReader reader;
        if (reader.init(device, size)) return 1;
        std::vector<std::uint8_t> want(size, 0), got;
        for (UINT64 i = 0; i < size / 2; ++i) want[i] = pages[i] = pattern_byte(i, 3);
        EXPECT(reader.read(buffer.p, got) == 0);
        EXPECT(first_difference(got, want) == -1);
        EXPECT((protection_of(pages) & 0xff) == PAGE_READONLY);
        EXPECT(reader.read(buffer.p, got) == 0);
        wchar_t flag[2]{};
        const bool full_walk = GetEnvironmentVariableW(L"LEONIDA_SHADOW_FULL_WALK", flag, 2) == 1 && flag[0] == L'1';
        // The full walk also queries the clean, protected half.
        EXPECT(rt::d3d12::title_shadow_refresh_queries(device) == (full_walk ? 2 : 1));
        EXPECT(first_difference(got, want) == -1);
        // Committed after the shadow was made, then written.
        std::uint8_t* later = pages + 3 * 0x10000;
        EXPECT(VirtualAlloc(later, 0x1000, MEM_COMMIT, PAGE_READWRITE) == later);
        leonida::title_memory_map::changed();  // as the title's VirtualAlloc shim does
        for (UINT64 i = 0; i < 0x1000; ++i) want[3 * 0x10000 + i] = later[i] = pattern_byte(i, 77);
        EXPECT(reader.read(buffer.p, got) == 0);
        long long wrong = first_difference(got, want);
        if (wrong >= 0) std::fprintf(stderr, "%s: page committed later: first wrong byte at %lld\n", who, wrong);
        EXPECT(wrong == -1);
        // Decommitted: zeros, and the rest unchanged.
        EXPECT(VirtualFree(pages, 0x10000, MEM_DECOMMIT));
        leonida::title_memory_map::changed();  // as the title's VirtualFree shim does
        std::fill(want.begin(), want.begin() + 0x10000, std::uint8_t(0));
        EXPECT(reader.read(buffer.p, got) == 0);
        wrong = first_difference(got, want);
        if (wrong >= 0) std::fprintf(stderr, "%s: decommitted page: first wrong byte at %lld\n", who, wrong);
        EXPECT(wrong == -1);
        // Committed again and written: it arrives.
        EXPECT(VirtualAlloc(pages, 0x1000, MEM_COMMIT, PAGE_READWRITE) == pages);
        leonida::title_memory_map::changed();
        for (UINT64 i = 0; i < 0x1000; ++i) want[i] = pages[i] = pattern_byte(i, 5);
        EXPECT(reader.read(buffer.p, got) == 0);
        EXPECT(first_difference(got, want) == -1);
        // A title write opens the watched page without changing the map generation.
        const auto generation = leonida::title_memory_map::generation();
        EXPECT((protection_of(pages) & 0xff) == PAGE_READONLY);
        want[0] = pages[0] ^= 0xff;
        EXPECT((protection_of(pages) & 0xff) == PAGE_READWRITE);
        EXPECT(leonida::title_memory_map::generation() == generation);
        EXPECT(reader.read(buffer.p, got) == 0);
        EXPECT(first_difference(got, want) == -1);
    }
    std::printf("%s: a page committed after the shadow, a decommitted page and a recommitted one read back right\n",
                who);
    VirtualFree(pages, 0, MEM_RELEASE);
    return failures;
}

// The negative control, and the case it stands for. One pagefile-backed section (SEC_RESERVE, half committed, as xmem
// backs physical pages) is mapped at two addresses, A and B; the shadow is placed at A. A write through B doesn't fault
// at A, so without a mark the page it changed is not copied: the GPU still reads the old bytes. That shows the test
// sees a page the tracking missed. Once the page is reported aliased (what xmem does when it maps one physical page
// at two addresses: leonida::title_page_aliases), it is copied at every submission and the write arrives.
int check_aliased_write(ID3D12Device* device) {
    const char* who = "vulkan slot-133 aliased page";
    constexpr SIZE_T size = 4 * 0x10000;
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_RESERVE, 0, DWORD(size),
                                        nullptr);
    EXPECT(section != nullptr);
    auto* a = static_cast<std::uint8_t*>(MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, size));
    auto* b = static_cast<std::uint8_t*>(MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, size));
    EXPECT(a != nullptr && b != nullptr && VirtualAlloc(a, size / 2, MEM_COMMIT, PAGE_READWRITE) == a);
    const UINT64 committed = size / 2;
    long long unmarked = -1, marked = -1;
    {
        Ref<ID3D12Resource> buffer;
        buffer.p = place_shadow(device, a, size);
        EXPECT(buffer.p != nullptr);
        GpuReader reader;
        if (reader.init(device, size)) return 1;
        std::vector<std::uint8_t> want(size, 0), got;
        for (UINT64 i = 0; i < committed; ++i) want[i] = a[i] = pattern_byte(i, 19);
        EXPECT(reader.read(buffer.p, got) == 0);
        EXPECT(first_difference(got, want) == -1);
        // Through B, the page at offset 0x11000 (in A's second 64 KiB).
        constexpr UINT64 at = 0x11000;
        for (UINT64 i = at; i < at + 0x1000; ++i) want[i] = b[i] = pattern_byte(i, 133);
        EXPECT(std::memcmp(a + at, want.data() + at, 0x1000) == 0);  // the same memory
        EXPECT(reader.read(buffer.p, got) == 0);
        unmarked = first_difference(got, want);
        // Reported aliased: copied from now on.
        leonida::title_page_aliases::set(reinterpret_cast<std::uintptr_t>(a + at), true);
        EXPECT(reader.read(buffer.p, got) == 0);
        marked = first_difference(got, want);
        // And a later write through B, while it stays aliased.
        for (UINT64 i = at; i < at + 64; ++i) want[i] = b[i] = 0x3c;
        EXPECT(reader.read(buffer.p, got) == 0);
        EXPECT(first_difference(got, want) == -1);
        leonida::title_page_aliases::set(reinterpret_cast<std::uintptr_t>(a + at), false);
    }
    UnmapViewOfFile(b);
    UnmapViewOfFile(a);
    CloseHandle(section);
    std::printf("%s: a write through another view of the page, not marked: first stale byte at %lld (expected: in "
                "the page, 69632); marked aliased: %s\n",
                who, unmarked, marked == -1 ? "arrives" : "STILL STALE");
    EXPECT(unmarked == 0x11000);
    EXPECT(marked == -1);
    return 0;
}

// The same page, guarded at both addresses instead of copied at every submission. With the other address known (what
// xmem answers through leonida::geometry_writes::aliases), the shadow at A arms the geometry watch's cells of the
// aliased 64 KiB at A and at B: both read-only between submissions, and a submission that follows no write copies
// nothing of it. The control: a write through a third view nobody reported doesn't fault, so it stays stale until a
// write through B (which faults) marks the block; then both arrive. Each view commits its pages itself: Wine shows a
// page of a SEC_RESERVE view as committed only in the view that committed it.
std::uintptr_t g_alias_a, g_alias_b;
void aliases_of_test_views(std::uintptr_t va, std::vector<std::uintptr_t>& others) {
    if (va >= g_alias_a && va < g_alias_a + 4 * 0x10000) others.push_back(va - g_alias_a + g_alias_b);
    if (va >= g_alias_b && va < g_alias_b + 4 * 0x10000) others.push_back(va - g_alias_b + g_alias_a);
}

int check_aliased_guard(ID3D12Device* device) {
    const char* who = "vulkan slot-133 aliased page, guarded";
    constexpr SIZE_T size = 4 * 0x10000;
    constexpr UINT64 committed = size / 2, block = 0x10000, at = 0x11000;
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_RESERVE, 0, DWORD(size),
                                        nullptr);
    EXPECT(section != nullptr);
    std::uint8_t* views[3]{};
    for (auto& view : views) {
        view = static_cast<std::uint8_t*>(MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, size));
        EXPECT(view != nullptr && VirtualAlloc(view, committed, MEM_COMMIT, PAGE_READWRITE) == view);
    }
    std::uint8_t *a = views[0], *b = views[1], *c = views[2];
    g_alias_a = reinterpret_cast<std::uintptr_t>(a);
    g_alias_b = reinterpret_cast<std::uintptr_t>(b);
    leonida::geometry_writes::aliases.store(&aliases_of_test_views, std::memory_order_release);
    leonida::title_page_aliases::set(g_alias_a + block, true);
    leonida::title_page_aliases::set(g_alias_b + block, true);
    long long unreported = -1, faulted = -1;
    DWORD at_a = 0, at_b = 0, b_written = 0;
    {
        Ref<ID3D12Resource> buffer;
        buffer.p = place_shadow(device, a, size);
        EXPECT(buffer.p != nullptr);
        GpuReader reader;
        if (reader.init(device, size)) return 1;
        std::vector<std::uint8_t> want(size, 0), got;
        for (UINT64 i = 0; i < committed; ++i) want[i] = a[i] = pattern_byte(i, 23);
        EXPECT(reader.read(buffer.p, got) == 0);
        EXPECT(first_difference(got, want) == -1);
        at_a = protection_of(a + at);
        at_b = protection_of(b + at);
        EXPECT((at_a & 0xff) == PAGE_READONLY);
        EXPECT((at_b & 0xff) == PAGE_READONLY);
        // Nothing written: the block is not copied, so a write the cells can't see is not either.
        for (UINT64 i = at; i < at + 64; ++i) want[i] = c[i] = 0x5a;
        EXPECT(reader.read(buffer.p, got) == 0);
        unreported = first_difference(got, want);
        // Through B: faults, and the block is copied again (the write through C with it).
        for (UINT64 i = at + 0x800; i < at + 0x1000; ++i) want[i] = b[i] = pattern_byte(i, 151);
        b_written = protection_of(b + at);
        EXPECT(reader.read(buffer.p, got) == 0);
        faulted = first_difference(got, want);
        EXPECT((protection_of(b + at) & 0xff) == PAGE_READONLY);
        // Through A, the shadow's own address.
        for (UINT64 i = at; i < at + 16; ++i) want[i] = a[i] = 0xc3;
        EXPECT(reader.read(buffer.p, got) == 0);
        EXPECT(first_difference(got, want) == -1);
    }
    // Released: both addresses are writable again.
    EXPECT((protection_of(a + at) & 0xff) == PAGE_READWRITE);
    EXPECT((protection_of(b + at) & 0xff) == PAGE_READWRITE);
    leonida::title_page_aliases::set(g_alias_a + block, false);
    leonida::title_page_aliases::set(g_alias_b + block, false);
    leonida::geometry_writes::aliases.store(nullptr, std::memory_order_release);
    for (auto* view : views) UnmapViewOfFile(view);
    CloseHandle(section);
    std::printf("%s: protection between submissions 0x%lx at A, 0x%lx at B (0x%lx after the write through B); an "
                "unreported view's write: first stale byte at %lld (expected 69632); after a write through B: %s\n",
                who, at_a, at_b, b_written, unreported, faulted == -1 ? "arrives" : "STILL STALE");
    EXPECT(unreported == static_cast<long long>(at));
    EXPECT((b_written & 0xff) == PAGE_READWRITE);
    EXPECT(faulted == -1);
    return 0;
}

// An alias that appears after the cells were armed. The page is mapped at A and B and guarded at both; then xmem maps
// it at C too and, as note_mapping does once the peers are updated, moves the mapping of the cells at every address.
// The next submission asks for the other addresses again and guards C as well, so a write through C faults and is
// copied at the submission after it.
std::uintptr_t g_alias_c;
void aliases_of_late_views(std::uintptr_t va, std::vector<std::uintptr_t>& others) {
    const std::uintptr_t views[3] = {g_alias_a, g_alias_b, g_alias_c};
    for (const std::uintptr_t from : views) {
        if (!from || va < from || va >= from + 4 * 0x10000) continue;
        for (const std::uintptr_t to : views)
            if (to && to != from) others.push_back(va - from + to);
    }
}

int check_aliased_guard_late_peer(ID3D12Device* device) {
    const char* who = "vulkan slot-133 aliased page, guarded, alias added later";
    constexpr SIZE_T size = 4 * 0x10000;
    constexpr UINT64 committed = size / 2, block = 0x10000, at = 0x11000;
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_RESERVE, 0, DWORD(size),
                                        nullptr);
    EXPECT(section != nullptr);
    std::uint8_t* views[3]{};
    for (auto& view : views) {
        view = static_cast<std::uint8_t*>(MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, size));
        EXPECT(view != nullptr && VirtualAlloc(view, committed, MEM_COMMIT, PAGE_READWRITE) == view);
    }
    std::uint8_t *a = views[0], *b = views[1], *c = views[2];
    g_alias_a = reinterpret_cast<std::uintptr_t>(a);
    g_alias_b = reinterpret_cast<std::uintptr_t>(b);
    g_alias_c = 0;  // not mapped there yet, as far as xmem knows
    leonida::geometry_writes::aliases.store(&aliases_of_late_views, std::memory_order_release);
    leonida::title_page_aliases::set(g_alias_a + block, true);
    leonida::title_page_aliases::set(g_alias_b + block, true);
    DWORD c_before = 0, c_armed = 0, c_written = 0;
    long long late = -1;
    {
        Ref<ID3D12Resource> buffer;
        buffer.p = place_shadow(device, a, size);
        EXPECT(buffer.p != nullptr);
        GpuReader reader;
        if (reader.init(device, size)) return 1;
        std::vector<std::uint8_t> want(size, 0), got;
        for (UINT64 i = 0; i < committed; ++i) want[i] = a[i] = pattern_byte(i, 41);
        EXPECT(reader.read(buffer.p, got) == 0);
        EXPECT(first_difference(got, want) == -1);
        EXPECT((protection_of(a + at) & 0xff) == PAGE_READONLY);
        EXPECT((protection_of(b + at) & 0xff) == PAGE_READONLY);
        c_before = protection_of(c + at);
        // Mapped at C now: the peers first, then the cells' mapping at every address (xmem's note_mapping).
        g_alias_c = reinterpret_cast<std::uintptr_t>(c);
        leonida::title_page_aliases::set(g_alias_c + block, true);
        namespace watch = leonida::geometry_writes;
        for (const std::uintptr_t view : {g_alias_a, g_alias_b, g_alias_c})
            watch::notify(view + block, view + 2 * block, watch::Event::mapping);
        EXPECT(reader.read(buffer.p, got) == 0);
        EXPECT(first_difference(got, want) == -1);
        c_armed = protection_of(c + at);
        // Through C: faults, and the block is copied at the next submission.
        for (UINT64 i = at + 0x400; i < at + 0xc00; ++i) want[i] = c[i] = pattern_byte(i, 97);
        c_written = protection_of(c + at);
        EXPECT(reader.read(buffer.p, got) == 0);
        late = first_difference(got, want);
    }
    EXPECT((protection_of(a + at) & 0xff) == PAGE_READWRITE);
    EXPECT((protection_of(b + at) & 0xff) == PAGE_READWRITE);
    EXPECT((protection_of(c + at) & 0xff) == PAGE_READWRITE);
    leonida::title_page_aliases::set(g_alias_a + block, false);
    leonida::title_page_aliases::set(g_alias_b + block, false);
    leonida::title_page_aliases::set(g_alias_c + block, false);
    leonida::geometry_writes::aliases.store(nullptr, std::memory_order_release);
    g_alias_c = 0;
    for (auto* view : views) UnmapViewOfFile(view);
    CloseHandle(section);
    std::printf("%s: protection at C 0x%lx before it was reported, 0x%lx after (0x%lx after the write through C); "
                "the write through C: %s\n",
                who, c_before, c_armed, c_written, late == -1 ? "arrives" : "STILL STALE");
    EXPECT((c_before & 0xff) == PAGE_READWRITE);
    EXPECT((c_armed & 0xff) == PAGE_READONLY);
    EXPECT((c_written & 0xff) == PAGE_READWRITE);
    EXPECT(late == -1);
    return 0;
}

// Kernel writes into a watched page. ReadFile writes from kernel mode, so a page a shadow made read-only fails it
// with ERROR_NOACCESS instead of faulting into the layer: the plain call fails (the control). Through the file shims'
// path (leonida::kernel_writes::read_file, what the title's ReadFile import resolves to), the destination is pinned
// and opened first: the read succeeds and its bytes reach the GPU at the next submission, synchronously and with an
// overlapped read on an asynchronous handle.
int check_kernel_write(ID3D12Device* device) {
    const char* who = "vulkan slot-133 kernel write";
    constexpr SIZE_T size = 4 * 0x10000;
    constexpr DWORD chunk = 0x2000;
    wchar_t directory[MAX_PATH]{}, path[MAX_PATH]{};
    EXPECT(GetTempPathW(MAX_PATH, directory) && GetTempFileNameW(directory, L"lsh", 0, path));
    std::vector<std::uint8_t> file_bytes(2 * chunk);
    for (size_t i = 0; i < file_bytes.size(); ++i) file_bytes[i] = std::uint8_t(i * 29u + 1u);
    {
        const HANDLE out = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        DWORD wrote = 0;
        EXPECT(out != INVALID_HANDLE_VALUE);
        const BOOL ok = WriteFile(out, file_bytes.data(), DWORD(file_bytes.size()), &wrote, nullptr);
        CloseHandle(out);
        EXPECT(ok && wrote == file_bytes.size());
    }
    auto* pages = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_READWRITE | PAGE_WRITECOMBINE));
    EXPECT(pages != nullptr && VirtualAlloc(pages, size / 2, MEM_COMMIT, PAGE_READWRITE | PAGE_WRITECOMBINE) == pages);
    DWORD plain_error = 0, watched = 0;
    bool plain_failed = false;
    {
        Ref<ID3D12Resource> buffer;
        buffer.p = place_shadow(device, pages, size);
        EXPECT(buffer.p != nullptr);
        GpuReader reader;
        if (reader.init(device, size)) return 1;
        std::vector<std::uint8_t> want(size, 0), got;
        for (UINT64 i = 0; i < size / 2; ++i) want[i] = pages[i] = pattern_byte(i, 41);
        EXPECT(reader.read(buffer.p, got) == 0);
        EXPECT(first_difference(got, want) == -1);
        watched = protection_of(pages + 0x4000);

        // The control: a plain ReadFile into a page made read-only by the last submission.
        HANDLE in = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        EXPECT(in != INVALID_HANDLE_VALUE);
        DWORD read = 0;
        plain_failed = !::ReadFile(in, pages + 0x4000, chunk, &read, nullptr);
        plain_error = plain_failed ? GetLastError() : 0;
        if (!plain_failed) std::memcpy(want.data() + 0x4000, file_bytes.data(), chunk);  // not watched after all

        // Through the shims' path: the same read succeeds.
        SetFilePointer(in, 0, nullptr, FILE_BEGIN);
        read = 0;
        const BOOL ok = leonida::kernel_writes::read_file(in, pages + 0x4000, chunk, &read, nullptr);
        CloseHandle(in);
        EXPECT(ok && read == chunk);
        std::memcpy(want.data() + 0x4000, file_bytes.data(), chunk);
        EXPECT(reader.read(buffer.p, got) == 0);
        long long wrong = first_difference(got, want);
        if (wrong >= 0) std::fprintf(stderr, "%s: synchronous read: first wrong byte at %lld\n", who, wrong);
        EXPECT(wrong == -1);

        // Overlapped, on an asynchronous handle, into the next page (read-only again after that submission).
        in = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        EXPECT(in != INVALID_HANDLE_VALUE);
        OVERLAPPED overlapped{};
        overlapped.Offset = chunk;
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        BOOL queued = leonida::kernel_writes::read_file(in, pages + 0x8000, chunk, nullptr, &overlapped);
        const bool pending = !queued && GetLastError() == ERROR_IO_PENDING;
        EXPECT(queued || pending);
        // A submission while it may still be in flight leaves the pinned page writable.
        EXPECT(reader.read(buffer.p, got) == 0);
        read = 0;
        EXPECT(GetOverlappedResult(in, &overlapped, &read, TRUE) && read == chunk);
        CloseHandle(overlapped.hEvent);
        CloseHandle(in);
        std::memcpy(want.data() + 0x8000, file_bytes.data() + chunk, chunk);
        EXPECT(reader.read(buffer.p, got) == 0);
        wrong = first_difference(got, want);
        if (wrong >= 0) std::fprintf(stderr, "%s: overlapped read: first wrong byte at %lld\n", who, wrong);
        EXPECT(wrong == -1);
        // Its pin is gone once the read completed: nothing is left pinned.
        EXPECT(leonida::kernel_writes::state().count.load() == 0);
    }
    VirtualFree(pages, 0, MEM_RELEASE);
    DeleteFileW(path);
    std::printf("%s: plain ReadFile into a watched page (protection 0x%lx): %s (error %lu); through the file shims: "
                "synchronous and overlapped reads succeed and reach the GPU\n",
                who, watched, plain_failed ? "fails" : "succeeds", plain_error);
    // The control must show the failure the shims prevent, wherever the page was watched.
    if ((watched & 0xff) == PAGE_READONLY) EXPECT(plain_failed && plain_error == ERROR_NOACCESS);
    return 0;
}

// Shadows that share pages, as the title places them: buffers of a few KiB back to back, off page boundaries, so most
// 4 KiB pages hold the end of one buffer and the start of the next. A page has one owner that may make it read-only,
// so the shadows over a run of pages that overlap or touch share one tracker; checked, with what the GPU reads after
// each step compared byte for byte with what the CPU wrote:
//   1. six buffers back to back are one tracker, and two apart (a page between them) two more; a buffer reaching
//      both joins those two into one;
//   2. a submission with nothing written queries no page (with a tracker per buffer, every buffer but a page's owner
//      found it read-only, not its own, and queried and copied it at every submission);
//   3. bytes written on both sides of a buffer boundary, inside one page, reach both buffers;
//   4. a kernel write (ReadFile through the file shims) into a shared page reaches both buffers;
//   5. a released buffer's own pages are writable again (no fault, no copy), and its neighbours still read right.
int check_shared_pages(ID3D12Device* device) {
    const char* who = "vulkan slot-133 shared pages";
    constexpr SIZE_T size = 4 * 0x10000;
    auto* pages = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE | PAGE_WRITECOMBINE));
    EXPECT(pages != nullptr);
    struct Placed {
        UINT64 offset, bytes;
        Ref<ID3D12Resource> buffer;
        GpuReader reader;
    };
    // Back to back from 0x40: page 0 holds the end of 0 and the start of 1, page 1 the end of 1 and the start of 2,
    // and so on; buffer 3 alone covers pages 3 and 4. Buffers 6 and 7 are apart (page 0x11 between them); 8 bridges them.
    const UINT64 layout[][2] = {{0x40, 0xc00},     {0xc40, 0x1000},   {0x1c40, 0x400},   {0x2040, 0x3000},
                                {0x5040, 0x800},   {0x5840, 0xa40},   {0x10100, 0x800},  {0x12100, 0x800},
                                {0x10900, 0x1800}};
    constexpr size_t kCount = sizeof(layout) / sizeof(layout[0]);
    std::vector<std::unique_ptr<Placed>> placed;
    std::vector<std::uint8_t> want(size), got;
    for (UINT64 i = 0; i < size; ++i) want[i] = pages[i] = pattern_byte(i, 19);
    const auto read_all = [&](size_t skip) -> long long {  // the first buffer that reads wrong, or -1
        for (size_t k = 0; k < placed.size(); ++k) {
            if (k == skip || !placed[k]->buffer.p) continue;
            if (placed[k]->reader.read(placed[k]->buffer.p, got)) return static_cast<long long>(k);
            const std::vector<std::uint8_t> expected(want.begin() + std::ptrdiff_t(placed[k]->offset),
                                                     want.begin() + std::ptrdiff_t(placed[k]->offset + placed[k]->bytes));
            const long long wrong = first_difference(got, expected);
            if (wrong >= 0) {
                std::fprintf(stderr, "%s: buffer %zu (+0x%llx): first wrong byte at +0x%llx\n", who, k,
                             static_cast<unsigned long long>(placed[k]->offset), wrong);
                return static_cast<long long>(k);
            }
        }
        return -1;
    };
    // Which placements are shadows is the driver's: an import needs the buffer alignment (0x40 isn't), and some Wine
    // versions report write-combined pages as plain memory, so a driver there may import the aligned ones (6, 7, 8).
    // The trackers expected follow the shadows actually made, joined as the layer joins them: a new shadow joins every
    // tracker whose pages overlap or touch its own, and a release never splits one (it goes with its last shadow).
    struct Run {
        UINT64 first, last;
        std::vector<size_t> members;
    };
    std::vector<Run> runs;
    std::vector<bool> shadowed(kCount, false);
    const auto join_runs = [&](size_t k) {
        const UINT64 first = layout[k][0] & ~UINT64(0xfff);
        const UINT64 last = (layout[k][0] + layout[k][1] + 0xfff) & ~UINT64(0xfff);
        Run joined{first, last, {k}};
        std::erase_if(runs, [&](const Run& run) {
            if (run.first > last || first > run.last) return false;
            joined.first = std::min(joined.first, run.first);
            joined.last = std::max(joined.last, run.last);
            joined.members.insert(joined.members.end(), run.members.begin(), run.members.end());
            return true;
        });
        runs.push_back(std::move(joined));
    };
    const auto leave_runs = [&](size_t k) {
        for (Run& run : runs) std::erase(run.members, k);
        std::erase_if(runs, [](const Run& run) { return run.members.empty(); });
    };
    const std::uint64_t trackers_before = rt::d3d12::title_shadow_trackers(device);
    for (size_t k = 0; k < kCount; ++k) {
        auto p = std::make_unique<Placed>();
        p->offset = layout[k][0];
        p->bytes = layout[k][1];
        p->buffer.p = place_shadow(device, pages + p->offset, p->bytes);
        EXPECT(p->buffer.p != nullptr);
        if (p->reader.init(device, p->bytes)) return 1;
        shadowed[k] = rt::d3d12::title_shadow_buffer(p->buffer.p);
        if (shadowed[k]) join_runs(k);
        placed.push_back(std::move(p));
        // All shadows (Windows): 0-5 are one tracker, 6 and 7 one each.
        if (k == kCount - 2) EXPECT(rt::d3d12::title_shadow_trackers(device) - trackers_before == runs.size());
    }
    const size_t shadows = size_t(std::count(shadowed.begin(), shadowed.end(), true));
    // Step 1: buffer 8 joined the trackers of 6 and 7 (all shadows: 2 trackers).
    const std::uint64_t trackers = rt::d3d12::title_shadow_trackers(device) - trackers_before;
    EXPECT(trackers == runs.size());
    EXPECT(read_all(SIZE_MAX) == -1);
    if (shadowed[0] || shadowed[1])  // watched, by the one owner of page 0
        EXPECT((protection_of(pages + 0xc40) & 0xff) == PAGE_READONLY);

    // Step 2: nothing written since.
    EXPECT(read_all(SIZE_MAX) == -1);
    const std::uint64_t clean_queries = rt::d3d12::title_shadow_refresh_queries(device);
    EXPECT(clean_queries == 0);

    // Step 3: the last byte of buffer 0 and the first of buffer 1 (page 0), and the bytes around 7 and 8's boundary.
    want[0xc3f] = pages[0xc3f] = 0x5a;
    want[0xc40] = pages[0xc40] = 0xa5;
    for (UINT64 i = 0x120f0; i < 0x12110; ++i) want[i] = pages[i] = std::uint8_t(i * 13u);
    EXPECT(read_all(SIZE_MAX) == -1);

    // Step 4: a kernel write into page 1 (buffers 1 and 2) through the file shims.
    wchar_t directory[MAX_PATH]{}, path[MAX_PATH]{};
    EXPECT(GetTempPathW(MAX_PATH, directory) && GetTempFileNameW(directory, L"lsp", 0, path));
    std::vector<std::uint8_t> file_bytes(0x200);
    for (size_t i = 0; i < file_bytes.size(); ++i) file_bytes[i] = std::uint8_t(i * 7u + 3u);
    {
        const HANDLE out = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        DWORD wrote = 0;
        EXPECT(out != INVALID_HANDLE_VALUE);
        const BOOL ok = WriteFile(out, file_bytes.data(), DWORD(file_bytes.size()), &wrote, nullptr);
        CloseHandle(out);
        EXPECT(ok && wrote == file_bytes.size());
    }
    const HANDLE in = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    EXPECT(in != INVALID_HANDLE_VALUE);
    DWORD read = 0;
    const BOOL read_ok = leonida::kernel_writes::read_file(in, pages + 0x1b00, DWORD(file_bytes.size()), &read, nullptr);
    CloseHandle(in);
    DeleteFileW(path);
    EXPECT(read_ok && read == file_bytes.size());
    std::copy(file_bytes.begin(), file_bytes.end(), want.begin() + 0x1b00);
    EXPECT(read_all(SIZE_MAX) == -1);

    // Step 5: buffer 3 goes. Pages 3 and 4 were its alone: writable again, and passed over. Pages 2 and 5, shared
    // with buffers 2 and 4, are still watched and still reach them.
    EXPECT(read_all(SIZE_MAX) == -1);  // pages read-only again after the kernel write's pin ended
    placed[3]->buffer.p->Release();
    placed[3]->buffer.p = nullptr;
    leave_runs(3);
    EXPECT((protection_of(pages + 0x3000) & 0xff) == PAGE_READWRITE);
    EXPECT((protection_of(pages + 0x4000) & 0xff) == PAGE_READWRITE);
    if (shadowed[2]) EXPECT((protection_of(pages + 0x2000) & 0xff) == PAGE_READONLY);
    want[0x2030] = pages[0x2030] = 0x11;  // buffer 2's last bytes
    want[0x5050] = pages[0x5050] = 0x22;  // buffer 4's
    pages[0x3800] = 0x33;                  // nobody's now
    EXPECT(read_all(3) == -1);
    EXPECT(rt::d3d12::title_shadow_trackers(device) - trackers_before == runs.size());

    placed.clear();
    EXPECT(rt::d3d12::title_shadow_trackers(device) == trackers_before);
    EXPECT((protection_of(pages + 0x40) & 0xff) == PAGE_READWRITE);
    EXPECT((protection_of(pages + 0x12000) & 0xff) == PAGE_READWRITE);
    VirtualFree(pages, 0, MEM_RELEASE);
    std::printf("%s: %zu buffers back to back and across pages (%zu of them shadows) share %llu trackers; an unchanged "
                "submission queries %llu pages; boundary writes, a kernel write into a shared page and a released "
                "neighbour read back right\n",
                who, kCount, shadows, static_cast<unsigned long long>(trackers),
                static_cast<unsigned long long>(clean_queries));
    return 0;
}

// Creating a logical RTV needs no memory. Its first GPU clear binds the placement before replay.
int check_placed_target_backing(ID3D12Device* device) {
    const char* who = "slot-133 first GPU use";
    std::atomic<unsigned> attempts{0};
    leonida::DeferredImageBinding retry(&attempts, [](void* p) {
        return static_cast<std::atomic<unsigned>*>(p)->fetch_add(1) != 0;
    });
    EXPECT(!retry.ensure() && !retry.bound());
    std::array<std::thread, 4> callers;
    for (auto& thread : callers) thread = std::thread([&] { retry.ensure(); });
    for (auto& thread : callers) thread.join();
    EXPECT(retry.bound() && attempts.load() == 2);
    constexpr UINT width = 16, height = 8, pitch = 256;
    void* pages = VirtualAlloc(nullptr, 65536, MEM_RESERVE, PAGE_NOACCESS);
    EXPECT(pages);
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = width;
    d.Height = height;
    d.DepthOrArraySize = d.MipLevels = d.SampleDesc.Count = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    Ref<ID3D12Resource> target;
    EXPECT_HR(rt::d3d12::create_buffer_at_host_address(device, pages, d, 2, IID_PPV_ARGS(&target.p)), S_OK);
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
    Ref<ID3D12DescriptorHeap> heap;
    EXPECT_HR(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap.p)), S_OK);
    auto rtv = heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target.p, nullptr, rtv);
    const char* setting = std::getenv("LEONIDA_PLACED_IMAGE_DEFER");
    const bool lazy = !setting || std::strcmp(setting, "0");
    EXPECT(bool(rt::d3d12::vulkan_memory_flags(target.p)) == !lazy);
    GpuReader gpu;
    EXPECT(!gpu.init(device, pitch * height));
    EXPECT_HR(gpu.allocator->Reset(), S_OK);
    EXPECT_HR(gpu.list->Reset(gpu.allocator.p, nullptr), S_OK);
    const float color[]{1, 0, 1, 1};
    gpu.list->ClearRenderTargetView(rtv, color, 0, nullptr);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Transition = {target.p, 0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
    gpu.list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION src{target.p, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION dst{gpu.readback.p, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint.Footprint = {d.Format, width, height, 1, pitch};
    gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    EXPECT_HR(gpu.list->Close(), S_OK);
    ID3D12CommandList* lists[]{gpu.list.p};
    gpu.queue->ExecuteCommandLists(1, lists);
    EXPECT_HR(gpu.queue->Signal(gpu.fence.p, 1), S_OK);
    EXPECT_HR(gpu.fence->SetEventOnCompletion(1, gpu.done), S_OK);
    EXPECT(WaitForSingleObject(gpu.done, 30000) == WAIT_OBJECT_0);
    EXPECT(rt::d3d12::vulkan_memory_flags(target.p));
    void* mapped = nullptr;
    EXPECT_HR(gpu.readback->Map(0, nullptr, &mapped), S_OK);
    for (UINT y = 0; y < height; ++y) {
        const auto* pixels = reinterpret_cast<const std::uint32_t*>(static_cast<const char*>(mapped) + y * pitch);
        EXPECT(std::all_of(pixels, pixels + width, [](auto px) { return px == 0xffff00ff; }));
    }
    gpu.readback->Unmap(0, nullptr);
    target.p->Release();
    target.p = nullptr;
    VirtualFree(pages, 0, MEM_RELEASE);
    return 0;
}

// Two sampled BC placements have distinct T#s. A CPU write to the first and a GPU copy into
// the second must each promote only their destination and keep previously copied descriptors usable.
int check_shared_bc_placements(ID3D12Device* device, bool must_share = false) {
    const char* who = "shared slot-133 BC images";
    if (!rt::d3d12::bindless_set_layout(device)) return 0;
    constexpr UINT width = 32, height = 32, pitch = width / 4 * 8;
    void* pages = VirtualAlloc(nullptr, 2 * 65536, MEM_RESERVE, PAGE_NOACCESS);
    EXPECT(pages);
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_BC1_UNORM;
    Ref<ID3D12Resource> a, b;
    EXPECT_HR(rt::d3d12::create_buffer_at_host_address(device, pages, desc, 0, IID_PPV_ARGS(&a.p)), S_OK);
    EXPECT_HR(rt::d3d12::create_buffer_at_host_address(device, static_cast<char*>(pages) + 65536, desc, 0,
                                                        IID_PPV_ARGS(&b.p)), S_OK);
    std::uint32_t av[8]{}, bv[8]{};
    device->CreateShaderResourceView(a.p, nullptr, {reinterpret_cast<SIZE_T>(av)});
    device->CreateShaderResourceView(b.p, nullptr, {reinterpret_cast<SIZE_T>(bv)});
    const auto first = rt::d3d12::bindless_hash_lookup(device, av);
    const auto second = rt::d3d12::bindless_hash_lookup(device, bv);
    EXPECT(first && second && first != second);
    const bool shared = !rt::d3d12::vulkan_memory_flags(a.p);
    const char* defer = std::getenv("LEONIDA_PLACED_IMAGE_DEFER");
    EXPECT(shared || !must_share || (defer && !std::strcmp(defer, "0")));
    EXPECT(bool(rt::d3d12::vulkan_memory_flags(b.p)) == !shared);
    std::array<std::uint8_t, pitch * height / 4> pattern{}, result{};
    for (std::size_t i = 0; i < pattern.size(); ++i) pattern[i] = std::uint8_t(i * 13 + 71);
    EXPECT_HR(a->WriteToSubresource(0, nullptr, pattern.data(), pitch, pattern.size()), S_OK);
    EXPECT(rt::d3d12::vulkan_memory_flags(a.p));
    EXPECT(bool(rt::d3d12::vulkan_memory_flags(b.p)) == !shared);
    const auto private_a = rt::d3d12::bindless_hash_lookup(device, av);
    EXPECT(private_a && (!shared || private_a != first));
    EXPECT(rt::d3d12::bindless_hash_lookup(device, bv) == second);
    GpuReader gpu;
    EXPECT(!gpu.init(device, 65536));
    EXPECT_HR(gpu.allocator->Reset(), S_OK);
    EXPECT_HR(gpu.list->Reset(gpu.allocator.p, nullptr), S_OK);
    gpu.list->CopyResource(b.p, a.p);
    EXPECT_HR(gpu.list->Close(), S_OK);
    ID3D12CommandList* lists[]{gpu.list.p};
    gpu.queue->ExecuteCommandLists(1, lists);
    EXPECT_HR(gpu.queue->Signal(gpu.fence.p, 1), S_OK);
    EXPECT_HR(gpu.fence->SetEventOnCompletion(1, gpu.done), S_OK);
    EXPECT(WaitForSingleObject(gpu.done, 30000) == WAIT_OBJECT_0);
    EXPECT(rt::d3d12::vulkan_memory_flags(b.p));
    const auto private_b = rt::d3d12::bindless_hash_lookup(device, bv);
    EXPECT(private_b && (!shared || private_b != second));
    EXPECT_HR(b->ReadFromSubresource(result.data(), pitch, result.size(), 0, nullptr), S_OK);
    EXPECT(result == pattern);
    std::printf("%s: %s, CPU write and GPU copy preserved all %zu bytes\n", who, shared ? "shared" : "eager", result.size());
    a.p->Release(); a.p = nullptr;
    b.p->Release(); b.p = nullptr;
    VirtualFree(pages, 0, MEM_RELEASE);
    return 0;
}

// Slot 70 may hand out several T#s before any texels exist. Keep copies of the original
// descriptors, replace one pending image at the same address, then fill from one notified write.
int check_pending_stream_views(ID3D12Device* device) {
    const char* who = "pending slot-70 views";
    if (!rt::d3d12::bindless_set_layout(device)) return 0;
    constexpr SIZE_T bytes = 65536;
    void* pages = VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_NOACCESS);
    EXPECT(pages);
    GpuReader gpu;
    EXPECT(!gpu.init(device, bytes));
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Height = 8;
    desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8_UNORM;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = desc.Format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    leonida::gfx10::ImageWords slot{};
    auto view = [&](unsigned width, UINT mapping) {
        desc.Width = width;
        srv.Shader4ComponentMapping = mapping;
        std::array<UINT, 14> resource{};
        std::array<UINT, 16> words{};
        std::memcpy(resource.data(), &desc, sizeof(desc));
        std::memcpy(words.data(), &srv, sizeof(srv));
        return rt::d3d12::write_placed_texture_view(device, false, resource.data(), words.data(),
                     reinterpret_cast<UINT64>(pages), {reinterpret_cast<SIZE_T>(slot.data())});
    };
    auto index = [&](const auto& descriptor) {
        std::uint32_t key[8];
        std::memcpy(key, descriptor.data(), sizeof(key));
        return rt::d3d12::bindless_hash_lookup(device, key);
    };
    EXPECT(view(16, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING));
    const auto replaced = slot;
    EXPECT(view(32, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING));
    const auto first = slot;
    EXPECT(view(32, D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0, 0, 0, 0)));
    const auto second = slot;
    slot = {};
    EXPECT(first != second && first != replaced);
    const char* defer = std::getenv("LEONIDA_TITLE_IMAGE_DEFER");
    if (!defer || std::strcmp(defer, "0"))
        EXPECT(!index(first) && !index(second));
    EXPECT(!index(replaced));
    EXPECT(VirtualAlloc(pages, bytes, MEM_COMMIT, PAGE_READWRITE) == pages);
    std::memset(pages, 73, bytes);
    leonida::title_texel_writes::note(pages, bytes);
    ID3D12CommandList* lists[]{gpu.list.p};
    gpu.queue->ExecuteCommandLists(1, lists);
    EXPECT_HR(gpu.queue->Signal(gpu.fence.p, 1), S_OK);
    EXPECT_HR(gpu.fence->SetEventOnCompletion(1, gpu.done), S_OK);
    EXPECT(WaitForSingleObject(gpu.done, 30000) == WAIT_OBJECT_0);
    EXPECT(index(first) && index(second) && !index(replaced));
    // Replace the synthetic owner before releasing the CPU pages.
    Ref<ID3D12Resource> placement;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    EXPECT_HR(rt::d3d12::create_buffer_at_host_address(device, pages, desc, 6, IID_PPV_ARGS(&placement.p)), S_OK);
    EXPECT(!index(first) && !index(second));
    placement.p->Release();
    placement.p = nullptr;
    VirtualFree(pages, 0, MEM_RELEASE);
    return 0;
}

namespace {
std::atomic<DWORD> g_view_av_thread{0};
std::atomic<unsigned> g_view_avs{0};

LONG CALLBACK count_view_av(PEXCEPTION_POINTERS info) {
    if (info && info->ExceptionRecord && info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        GetCurrentThreadId() == g_view_av_thread.load(std::memory_order_relaxed)) {
        g_view_avs.fetch_add(1, std::memory_order_relaxed);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace

// Map changes reported to title_memory_map between submissions must both retire a cached readable answer before a
// synchronous slot-70 view hashes decommitted pages, and wake a pending slot-70 view without waiting for a submission.
int check_placed_view_map_changes(ID3D12Device* device) {
    const char* who = "slot-70 view map changes";
    if (!rt::d3d12::bindless_set_layout(device)) return 0;
    GpuReader gpu;
    EXPECT(!gpu.init(device, 4096));
    auto env_is = [](const char* name, const char* want) {
        char buf[16]{};
        const DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
        return n > 0 && n < sizeof(buf) && std::strcmp(buf, want) == 0;
    };
    auto index = [&](const leonida::gfx10::ImageWords& descriptor) {
        std::uint32_t key[8];
        std::memcpy(key, descriptor.data(), sizeof(key));
        return rt::d3d12::bindless_hash_lookup(device, key);
    };

    // 1. Committed >256 KiB texture -> decommit interior 64 KiB + map::changed -> synchronous view takes zero AVs.
    {
        constexpr SIZE_T bytes = 512 * 1024;
        auto* pages = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        EXPECT(pages);
        std::memset(pages, 0x5a, bytes);
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = 512;
        desc.Height = 1024;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_R8_UNORM;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = desc.Format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        leonida::gfx10::ImageWords slot{};
        auto write_view = [&](UINT mapping) {
            srv.Shader4ComponentMapping = mapping;
            std::array<UINT, 14> resource{};
            std::array<UINT, 16> words{};
            std::memcpy(resource.data(), &desc, sizeof(desc));
            std::memcpy(words.data(), &srv, sizeof(srv));
            return rt::d3d12::write_placed_texture_view(device, false, resource.data(), words.data(),
                                                        reinterpret_cast<UINT64>(pages),
                                                        {reinterpret_cast<SIZE_T>(slot.data())});
        };
        EXPECT(write_view(D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING));
        EXPECT(index(slot) != 0);

        EXPECT(VirtualFree(pages + 65536, 65536, MEM_DECOMMIT) != FALSE);
        leonida::title_memory_map::changed(pages + 65536, 65536);

        g_view_avs.store(0, std::memory_order_relaxed);
        g_view_av_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
        PVOID veh = AddVectoredExceptionHandler(1, &count_view_av);
        EXPECT(veh != nullptr);
        const bool wrote_after_unmap = write_view(D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0, 0, 0, 0));
        RemoveVectoredExceptionHandler(veh);
        g_view_av_thread.store(0, std::memory_order_relaxed);
        EXPECT(wrote_after_unmap);
        EXPECT(g_view_avs.load(std::memory_order_relaxed) == 0);

        EXPECT(VirtualAlloc(pages + 65536, 65536, MEM_COMMIT, PAGE_READWRITE) == pages + 65536);
        leonida::title_memory_map::changed(pages + 65536, 65536);
        Ref<ID3D12Resource> placement;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        EXPECT_HR(rt::d3d12::create_buffer_at_host_address(device, pages, desc, 6, IID_PPV_ARGS(&placement.p)), S_OK);
        placement.p->Release();
        placement.p = nullptr;
        VirtualFree(pages, 0, MEM_RELEASE);
    }

    // 2. Reserved texture -> commit + map::changed (no texel write note, no submission) -> synchronous view publishes.
    {
        constexpr SIZE_T bytes = 65536;
        void* pages = VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_NOACCESS);
        EXPECT(pages);
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = 32;
        desc.Height = 8;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_R8_UNORM;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = desc.Format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        leonida::gfx10::ImageWords first{}, second{};
        auto write_view = [&](UINT mapping, leonida::gfx10::ImageWords& out) {
            srv.Shader4ComponentMapping = mapping;
            std::array<UINT, 14> resource{};
            std::array<UINT, 16> words{};
            std::memcpy(resource.data(), &desc, sizeof(desc));
            std::memcpy(words.data(), &srv, sizeof(srv));
            return rt::d3d12::write_placed_texture_view(device, false, resource.data(), words.data(),
                                                        reinterpret_cast<UINT64>(pages),
                                                        {reinterpret_cast<SIZE_T>(out.data())});
        };
        EXPECT(write_view(D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, first));
        if (!env_is("LEONIDA_TITLE_IMAGE_DEFER", "0")) EXPECT(!index(first));

        EXPECT(VirtualAlloc(pages, bytes, MEM_COMMIT, PAGE_READWRITE) == pages);
        std::memset(pages, 0x33, bytes);
        leonida::title_memory_map::changed(pages, bytes);

        EXPECT(write_view(D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0, 0, 0, 0), second));
        if (!env_is("LEONIDA_TITLE_FILL_ASYNC", "1")) EXPECT(index(first) && index(second));

        Ref<ID3D12Resource> placement;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        EXPECT_HR(rt::d3d12::create_buffer_at_host_address(device, pages, desc, 6, IID_PPV_ARGS(&placement.p)), S_OK);
        placement.p->Release();
        placement.p = nullptr;
        VirtualFree(pages, 0, MEM_RELEASE);
    }
    return 0;
}

// A persistent Map can become readable without notifying xmem or calling Map a second time.
int check_mapped_plane(ID3D12Device* device) {
    const char* who = "persistent slot-133 plane";
    constexpr UINT width = 256, height = 8, size = width * height;
    void* pages = VirtualAlloc(nullptr, 65536, MEM_RESERVE, PAGE_NOACCESS);
    EXPECT(pages);
    GpuReader gpu;
    EXPECT(!gpu.init(device, size));
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_R8_UNORM;
    desc.SampleDesc.Count = desc.DepthOrArraySize = desc.MipLevels = 1;
    leonida::title_xg_layouts::note({UINT(desc.Dimension), UINT(desc.Format), height, 1, 1, width}, 1);
    Ref<ID3D12Resource> texture;
    EXPECT_HR(rt::d3d12::create_buffer_at_host_address(device, pages, desc, 6, IID_PPV_ARGS(&texture.p)), S_OK);
    void* mapped = nullptr;
    EXPECT_HR(texture->Map(0, nullptr, &mapped), S_OK);
    EXPECT(mapped == pages);
    ID3D12CommandList* lists[]{gpu.list.p};
    std::array<std::uint8_t, size> pixels{};
    for (unsigned pass = 0; pass < 3; ++pass) {
        if (pass) {
            if (pass == 1) EXPECT(VirtualAlloc(mapped, size, MEM_COMMIT, PAGE_READWRITE) == mapped);
            std::memset(mapped, 61 * pass, size);
        }
        gpu.queue->ExecuteCommandLists(1, lists);
        EXPECT_HR(gpu.queue->Signal(gpu.fence.p, pass + 1), S_OK);
        EXPECT_HR(gpu.fence->SetEventOnCompletion(pass + 1, gpu.done), S_OK);
        EXPECT(WaitForSingleObject(gpu.done, 30000) == WAIT_OBJECT_0);
        if (pass) {
            EXPECT_HR(texture->ReadFromSubresource(pixels.data(), width, size, 0, nullptr), S_OK);
            EXPECT(std::all_of(pixels.begin(), pixels.end(), [&](auto value) { return value == 61 * pass; }));
        }
    }
    texture->Unmap(0, nullptr);
    texture.p->Release();
    texture.p = nullptr;
    VirtualFree(pages, 0, MEM_RELEASE);
    return 0;
}

// LEONIDA_TEXTURE_REFRESH_ASYNC=1 (the d3d12_mapped_plane_async test): a texture the title maps and writes, and then
// reads on the GPU in its very first submission, has its bytes there, though the refresh pass runs on a worker: the
// submission fills a mapped texture that was never filled before it records anything (fill_first_mapped_textures).
int check_mapped_plane_first_use(ID3D12Device* device) {
    const char* who = "persistent slot-133 plane, first use";
    constexpr UINT width = 256, height = 8, size = width * height;
    void* pages = VirtualAlloc(nullptr, 65536, MEM_RESERVE, PAGE_NOACCESS);
    EXPECT(pages);
    EXPECT(VirtualAlloc(pages, size, MEM_COMMIT, PAGE_READWRITE) == pages);
    std::memset(pages, 61, size);
    GpuReader gpu;
    EXPECT(!gpu.init(device, size));
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_R8_UNORM;
    desc.SampleDesc.Count = desc.DepthOrArraySize = desc.MipLevels = 1;
    leonida::title_xg_layouts::note({UINT(desc.Dimension), UINT(desc.Format), height, 1, 1, width}, 1);
    Ref<ID3D12Resource> texture;
    EXPECT_HR(rt::d3d12::create_buffer_at_host_address(device, pages, desc, 6, IID_PPV_ARGS(&texture.p)), S_OK);
    void* mapped = nullptr;
    EXPECT_HR(texture->Map(0, nullptr, &mapped), S_OK);
    EXPECT(mapped == pages);
    EXPECT_HR(gpu.allocator->Reset(), S_OK);
    EXPECT_HR(gpu.list->Reset(gpu.allocator.p, nullptr), S_OK);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Transition = {texture.p, 0, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE};
    gpu.list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION src{texture.p, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION dst{gpu.readback.p, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint.Footprint = {desc.Format, width, height, 1, width};
    gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    EXPECT_HR(gpu.list->Close(), S_OK);
    ID3D12CommandList* lists[]{gpu.list.p};
    gpu.queue->ExecuteCommandLists(1, lists);
    EXPECT_HR(gpu.queue->Signal(gpu.fence.p, 1), S_OK);
    EXPECT_HR(gpu.fence->SetEventOnCompletion(1, gpu.done), S_OK);
    EXPECT(WaitForSingleObject(gpu.done, 30000) == WAIT_OBJECT_0);
    void* read = nullptr;
    EXPECT_HR(gpu.readback->Map(0, nullptr, &read), S_OK);
    const auto* bytes = static_cast<const std::uint8_t*>(read);
    const bool filled = std::all_of(bytes, bytes + size, [](auto value) { return value == 61; });
    gpu.readback->Unmap(0, nullptr);
    EXPECT(filled);
    texture->Unmap(0, nullptr);
    texture.p->Release();
    texture.p = nullptr;
    VirtualFree(pages, 0, MEM_RELEASE);
    return 0;
}

// Several threads write the same watched pages while submissions keep making them read-only again. Two threads that
// fault on one page race: the first fault reopens it, a submission can protect it again before the second fault is
// handled, and the second write must still be retried, not refused (refused, the process ends: seen natively). After
// the writers stop, one more submission brings every page across. With `write_watched`, over memory reserved with
// MEM_WRITE_WATCH as xmem records it (leonida::title_write_watch): the threads race on the faults that make the block
// hot, and the writes go on with no guard.
int check_concurrent_writes(ID3D12Device* device, bool write_watched) {
    const char* who = "vulkan slot-133 written pages";
    constexpr SIZE_T size = 2 * 0x10000, committed = size / 2;  // one 64 KiB block: every fault lands in it
    constexpr unsigned kWriters = 12;
    auto* pages = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, size, MEM_RESERVE | (write_watched ? MEM_WRITE_WATCH : 0), PAGE_READWRITE));
    EXPECT(pages != nullptr && VirtualAlloc(pages, committed, MEM_COMMIT, PAGE_READWRITE) == pages);
    if (write_watched) leonida::title_write_watch::add(reinterpret_cast<std::uintptr_t>(pages), size);
    int failures = 0;
    unsigned submissions = 0;
    {
        Ref<ID3D12Resource> buffer;
        buffer.p = place_shadow(device, pages, size);
        EXPECT(buffer.p != nullptr);
        GpuReader reader;
        if (reader.init(device, size)) return 1;
        std::vector<std::uint8_t> got;
        EXPECT(reader.read(buffer.p, got) == 0);
        EXPECT((protection_of(pages) & 0xff) == PAGE_READONLY);
        std::atomic<bool> stop{false};
        std::vector<std::thread> writers;
        for (unsigned t = 0; t < kWriters; ++t)
            writers.emplace_back([&stop, pages, t] {
                for (std::uint8_t round = 1; !stop.load(std::memory_order_relaxed); ++round)
                    for (SIZE_T at = t * 8; at < committed; at += 0x1000)
                        *reinterpret_cast<volatile std::uint8_t*>(pages + at) = round;
            });
        bool read_failed = false;
        const ULONGLONG until = GetTickCount64() + 2000;
        while (!read_failed && GetTickCount64() < until) {
            read_failed = reader.read(buffer.p, got) != 0;
            ++submissions;
        }
        stop = true;
        for (std::thread& w : writers) w.join();
        EXPECT(!read_failed);
        EXPECT(reader.read(buffer.p, got) == 0);
        const std::vector<std::uint8_t> want(pages, pages + committed);
        got.resize(committed);
        const long long wrong = first_difference(got, want);
        if (wrong >= 0) std::fprintf(stderr, "%s: concurrent writers: first wrong byte at %lld\n", who, wrong);
        EXPECT(wrong == -1);
    }
    std::printf("%s: %u threads writing the same %s pages through %u submissions: no write refused, the last "
                "submission reads back right\n",
                who, kWriters, write_watched ? "write-watched" : "guarded", submissions);
    if (write_watched) leonida::title_write_watch::remove(reinterpret_cast<std::uintptr_t>(pages));
    VirtualFree(pages, 0, MEM_RELEASE);
    return failures;
}

// The title's ordinary memory, which xmem reserves with MEM_WRITE_WATCH and records (leonida::title_write_watch). Its
// pages start guarded; the first write fault on a 64 KiB block makes it hot, and from then on its shadows ask the kernel
// which pages were written instead of making it read-only, until it goes kCool (64) refreshes unwritten. Checked, half
// committed (so the pages are not imported and a shadow answers) and with each cache attribute, by what a GPU copy reads
// after each step compared byte for byte with what the CPU wrote:
//   1. a pattern after the shadow exists: one fault per block, which makes it hot; read-write from then on, and a
//      submission with nothing written finds no page written and queries only the reserved half;
//   2. a title write to a hot page reaches the GPU at the next submission, with no fault;
//   3. a plain ReadFile (no file shim) into a hot page succeeds, and its bytes reach the GPU;
//   4. an overlapped read through the file shims (the page pinned while it may be in flight) reaches it too;
//   5. a second device's shadow of the same pages, a tracker of its own: a write reaches both devices, though the
//      first one's asking cleared the kernel's bit (the journal keeps it for the other);
//   6. a page decommitted and committed again between submissions (zeros, and no write) arrives once marked;
//   7. after 66 submissions with nothing written the blocks are guarded again (read-only), and the next write faults,
//      makes them hot again and arrives.
// No fault comes after a block went hot until it cooled, on either device.
int check_write_watched(ID3D12Device* device, ID3D12Device* second, DWORD modifier) {
    const char* who = "vulkan slot-133 write-watched pages";
    const char* kind = modifier ? "write-combined" : "read-write";
    constexpr SIZE_T size = 4 * 0x10000;
    auto* pages = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_READWRITE | modifier));
    EXPECT(pages != nullptr && VirtualAlloc(pages, size / 2, MEM_COMMIT, PAGE_READWRITE | modifier) == pages);
    leonida::title_write_watch::add(reinterpret_cast<std::uintptr_t>(pages), size);  // as xmem records it
    wchar_t directory[MAX_PATH]{}, path[MAX_PATH]{};
    EXPECT(GetTempPathW(MAX_PATH, directory) && GetTempFileNameW(directory, L"lww", 0, path));
    std::vector<std::uint8_t> file_bytes(0x3000);
    for (size_t i = 0; i < file_bytes.size(); ++i) file_bytes[i] = std::uint8_t(i * 23u + 9u);
    {
        const HANDLE out = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        DWORD wrote = 0;
        EXPECT(out != INVALID_HANDLE_VALUE);
        const BOOL ok = WriteFile(out, file_bytes.data(), DWORD(file_bytes.size()), &wrote, nullptr);
        CloseHandle(out);
        EXPECT(ok && wrote == file_bytes.size());
    }
    rt::d3d12::TitleShadowWatch clean{};
    std::uint64_t clean_queries = 0, hot_faults = 0;
    DWORD before = 0, hot = 0, cooled = 0;
    {
        Ref<ID3D12Resource> buffer, other;
        buffer.p = place_shadow(device, pages, size);
        EXPECT(buffer.p != nullptr);
        GpuReader reader, other_reader;
        if (reader.init(device, size)) return 1;
        std::vector<std::uint8_t> want(size, 0), got;
        const auto read_back = [&](const char* step) -> long long {
            if (reader.read(buffer.p, got)) return 0;
            const long long wrong = first_difference(got, want);
            if (wrong >= 0) std::fprintf(stderr, "%s: %s: %s: first wrong byte at %lld\n", who, kind, step, wrong);
            return wrong;
        };
        const auto faults = [](ID3D12Device* d) { return rt::d3d12::title_shadow_watch(d).faults; };

        // Step 1.
        before = protection_of(pages);
        EXPECT((before & 0xff) == PAGE_READONLY);  // guarded until written
        const std::uint64_t faults_before = faults(device);
        for (UINT64 i = 0; i < size / 2; ++i) want[i] = pages[i] = pattern_byte(i, 61);
        EXPECT(faults(device) - faults_before == 2);  // one per 64 KiB block
        EXPECT(read_back("pattern") == -1);
        hot = protection_of(pages);
        EXPECT((hot & 0xff) == PAGE_READWRITE);
        EXPECT(rt::d3d12::title_shadow_watch(device).hot_bytes == size / 2);
        EXPECT(rt::d3d12::title_shadow_watch(device).watched_bytes == size);
        EXPECT(read_back("nothing written") == -1);
        clean = rt::d3d12::title_shadow_watch(device);
        clean_queries = rt::d3d12::title_shadow_refresh_queries(device);
        EXPECT(clean.asks == 1 && clean.written == 0);
        EXPECT(clean_queries == 1);  // the reserved half
        hot_faults = faults(device);

        // Step 2.
        for (UINT64 i = 0x5000; i < 0x6000; ++i) want[i] = pages[i] = pattern_byte(i, 177);
        EXPECT(read_back("one page written") == -1);
        EXPECT(rt::d3d12::title_shadow_watch(device).written == 1);

        // Step 3.
        HANDLE in = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        EXPECT(in != INVALID_HANDLE_VALUE);
        DWORD read = 0;
        const BOOL plain = ::ReadFile(in, pages + 0x8800, 0x2000, &read, nullptr);
        CloseHandle(in);
        EXPECT(plain && read == 0x2000);
        std::memcpy(want.data() + 0x8800, file_bytes.data(), 0x2000);
        EXPECT(read_back("plain ReadFile") == -1);

        // Step 4.
        in = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        EXPECT(in != INVALID_HANDLE_VALUE);
        OVERLAPPED overlapped{};
        overlapped.Offset = 0x1000;
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        const BOOL queued = leonida::kernel_writes::read_file(in, pages + 0x10000, 0x2000, nullptr, &overlapped);
        EXPECT(queued || GetLastError() == ERROR_IO_PENDING);
        EXPECT(reader.read(buffer.p, got) == 0);  // while it may still be in flight
        read = 0;
        EXPECT(GetOverlappedResult(in, &overlapped, &read, TRUE) && read == 0x2000);
        CloseHandle(overlapped.hEvent);
        CloseHandle(in);
        std::memcpy(want.data() + 0x10000, file_bytes.data() + 0x1000, 0x2000);
        EXPECT(read_back("overlapped read through the file shims") == -1);
        EXPECT(leonida::kernel_writes::state().count.load() == 0);
        EXPECT(faults(device) == hot_faults);

        // Step 5. The second device guards the pages at first: its own fault makes them hot there too.
        if (second) {
            other.p = place_shadow(second, pages, size);
            EXPECT(other.p != nullptr);
            if (other_reader.init(second, size)) return 1;
            EXPECT(other_reader.read(other.p, got) == 0);
            EXPECT(first_difference(got, want) == -1);
            EXPECT((protection_of(pages + 0x3000) & 0xff) == PAGE_READONLY);
            for (UINT64 i = 0x3010; i < 0x3020; ++i) want[i] = pages[i] = std::uint8_t(0x90 + i);
            EXPECT(faults(second) == 1);
            EXPECT(read_back("written, first device") == -1);
            EXPECT(other_reader.read(other.p, got) == 0);
            long long wrong = first_difference(got, want);
            if (wrong >= 0) std::fprintf(stderr, "%s: %s: second device: first wrong byte at %lld\n", who, kind, wrong);
            EXPECT(wrong == -1);
            // Hot on both: a write reaches both, whichever asks first.
            for (UINT64 i = 0x3020; i < 0x3030; ++i) want[i] = pages[i] = std::uint8_t(0x30 + i);
            EXPECT(other_reader.read(other.p, got) == 0);
            EXPECT(first_difference(got, want) == -1);
            EXPECT(read_back("written, second device asked first") == -1);
            EXPECT(faults(second) == 1);
        }
        EXPECT(faults(device) == hot_faults);

        // Step 6.
        EXPECT(VirtualFree(pages + 0x7000, 0x1000, MEM_DECOMMIT));
        EXPECT(VirtualAlloc(pages + 0x7000, 0x1000, MEM_COMMIT, PAGE_READWRITE | modifier) == pages + 0x7000);
        leonida::title_memory_map::changed();
        leonida::title_write_watch::mark(reinterpret_cast<std::uintptr_t>(pages + 0x7000),
                                         reinterpret_cast<std::uintptr_t>(pages + 0x8000));
        std::fill(want.begin() + 0x7000, want.begin() + 0x8000, std::uint8_t(0));
        EXPECT(read_back("decommitted and committed again") == -1);
        EXPECT(faults(device) == hot_faults);

        // Step 7, with the second device's copy gone (its guards would make the pages read-only meanwhile).
        if (other.p) {
            other.p->Release();
            other.p = nullptr;
        }
        for (int i = 0; i < 66; ++i) EXPECT(reader.read(buffer.p, got) == 0);
        EXPECT(first_difference(got, want) == -1);
        cooled = protection_of(pages);
        EXPECT((cooled & 0xff) == PAGE_READONLY);
        EXPECT(rt::d3d12::title_shadow_watch(device).hot_bytes == 0);
        want[0x100] = pages[0x100] = 0x7e;
        EXPECT(faults(device) == hot_faults + 1);
        EXPECT(read_back("written after cooling") == -1);
        EXPECT((protection_of(pages) & 0xff) == PAGE_READWRITE);
    }
    leonida::title_write_watch::remove(reinterpret_cast<std::uintptr_t>(pages));
    VirtualFree(pages, 0, MEM_RELEASE);
    DeleteFileW(path);
    std::printf("%s: %s, %llu bytes (half committed): protection 0x%lx guarded, 0x%lx once written (hot), 0x%lx after "
                "66 unwritten submissions; while hot no fault, and an unchanged submission asks %llu times, finds "
                "nothing written and queries %llu pages; a title write, a plain ReadFile, an overlapped read, %s and a "
                "recommitted page read back right\n",
                who, kind, static_cast<unsigned long long>(size), before, hot, cooled,
                static_cast<unsigned long long>(clean.asks), static_cast<unsigned long long>(clean_queries),
                second ? "a second device's copy" : "(no second device)");
    return 0;
}

// A write-watched reservation the title releases and reserves again at the same address between two submissions, as
// xmem records it: remove(), then add() of the new reservation, and nothing marked (a MEM_RESERVE | MEM_COMMIT in one
// call writes nothing). Its pages were hot and clean in the tracker and are write-watched again, now by a reservation
// whose kernel bits are clear, so no asking finds them written: the copy must still read the new reservation's zeros,
// then what the title writes there. Over two adjacent reservations under one shadow:
//   1. the second one released and reserved again: it reads as zeros, the first keeps its bytes;
//   2. a write into the new reservation arrives;
//   3. both released and reserved again as one reservation over the whole range: zeros everywhere.
int check_rereserved(ID3D12Device* device) {
    const char* who = "vulkan slot-133 write-watched pages";
    constexpr SIZE_T half = 2 * 0x10000, size = 2 * half;
    // Two adjacent reservations: a range reserved and released, then reserved again as two halves.
    auto* pages = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_READWRITE));
    EXPECT(pages != nullptr && VirtualFree(pages, 0, MEM_RELEASE));
    const auto reserve = [](std::uint8_t* at, SIZE_T bytes, DWORD type) {
        if (VirtualAlloc(at, bytes, type | MEM_WRITE_WATCH, PAGE_READWRITE) != at) return false;
        leonida::title_write_watch::add(reinterpret_cast<std::uintptr_t>(at), bytes);  // as xmem records it
        leonida::title_memory_map::changed();
        return true;
    };
    const auto release = [](std::uint8_t* at) {
        leonida::title_write_watch::remove(reinterpret_cast<std::uintptr_t>(at));
        const bool ok = VirtualFree(at, 0, MEM_RELEASE) != 0;
        leonida::title_memory_map::changed();
        return ok;
    };
    EXPECT(reserve(pages, half, MEM_RESERVE) && reserve(pages + half, half, MEM_RESERVE));
    // The first 64 KiB of each committed: not imported, so a shadow answers.
    EXPECT(VirtualAlloc(pages, 0x10000, MEM_COMMIT, PAGE_READWRITE) == pages);
    EXPECT(VirtualAlloc(pages + half, 0x10000, MEM_COMMIT, PAGE_READWRITE) == pages + half);
    bool whole = false;
    {
        Ref<ID3D12Resource> buffer;
        buffer.p = place_shadow(device, pages, size);
        EXPECT(buffer.p != nullptr);
        GpuReader reader;
        if (reader.init(device, size)) return 1;
        std::vector<std::uint8_t> want(size, 0), got;
        const auto read_back = [&](const char* step) -> long long {
            if (reader.read(buffer.p, got)) return 0;
            const long long wrong = first_difference(got, want);
            if (wrong >= 0) std::fprintf(stderr, "%s: re-reserved: %s: first wrong byte at %lld\n", who, step, wrong);
            return wrong;
        };
        // Written once (a fault per block makes it hot), then a submission that copies it and one that finds it clean.
        const auto write_all = [&](unsigned seed) {
            for (SIZE_T i = 0; i < size; ++i) {
                MEMORY_BASIC_INFORMATION region{};
                if ((i & 0xfff) == 0 && (!VirtualQuery(pages + i, &region, sizeof(region)) || region.State != MEM_COMMIT)) {
                    i += 0xfff;
                    continue;
                }
                want[i] = pages[i] = pattern_byte(i, seed);
            }
        };
        EXPECT(read_back("placed") == -1);
        write_all(91);
        EXPECT(read_back("written") == -1);
        EXPECT(read_back("nothing written") == -1);
        EXPECT(rt::d3d12::title_shadow_watch(device).hot_bytes == 2 * 0x10000);

        // Step 1.
        EXPECT(release(pages + half) && reserve(pages + half, half, MEM_RESERVE | MEM_COMMIT));
        std::fill(want.begin() + half, want.end(), std::uint8_t(0));
        EXPECT(read_back("second reservation released and reserved again") == -1);

        // Step 2.
        for (SIZE_T i = half + 0x2000; i < half + 0x2100; ++i) want[i] = pages[i] = std::uint8_t(i * 7u + 1u);
        EXPECT(read_back("written in the new reservation") == -1);

        // Step 3, hot and clean everywhere first.
        write_all(93);
        EXPECT(read_back("written again") == -1);
        EXPECT(read_back("nothing written again") == -1);
        EXPECT(release(pages) && release(pages + half) && reserve(pages, size, MEM_RESERVE | MEM_COMMIT));
        whole = true;
        std::fill(want.begin(), want.end(), std::uint8_t(0));
        EXPECT(read_back("both released, reserved again as one") == -1);
    }
    if (whole) {
        release(pages);
    } else {
        release(pages);
        release(pages + half);
    }
    std::printf("%s: a write-watched reservation released and reserved again at the same address between two "
                "submissions reads as the new reservation's zeros, then its writes (one of two adjacent reservations, "
                "and both as one)\n",
                who);
    return 0;
}

// Title threads take write faults while they hold the process heap's lock: XMemAlloc is _aligned_malloc, so HeapAlloc
// and HeapFree write block headers on pages a shadow may guard. The fault handler waits for the tracker's lock, so
// whoever holds that lock, and whatever lock that holder waits for (the write-watch record and journal, the alias set,
// the kernel-write pins), must never wait for the process heap meanwhile. One shadow over two adjacent reservations, a
// write-watched one (kept hot, so every submission asks about it) and a plain one (guarded), while submissions run back
// to back and:
//   - threads take the process heap's lock (HeapLock, as HeapAlloc does inside, and the heap gate above: every
//     allocation waits for them), _aligned_malloc and _aligned_free under it and write a guarded page: a fault, with
//     the heap's lock held, at every submission;
//   - threads _aligned_malloc and _aligned_free blocks of every size class without it;
//   - a thread keeps the write-watched half hot;
//   - threads change the write-watch record (a reservation added and removed), the journal (mark), the alias set and
//     the kernel-write pins, each of which the next refresh takes.
// A watchdog ends the process (exit code 3) when no submission or writer got anywhere for 30 s: a deadlock.
int check_heap_lock_faults(ID3D12Device* device) {
    const char* who = "vulkan slot-133 heap-lock faults";
    constexpr SIZE_T half = 4 * 0x10000, size = 2 * half, committed = half / 2;
    const bool write_watched = leonida::title_write_watch::enabled();
    auto* pages = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_READWRITE));
    EXPECT(pages != nullptr && VirtualFree(pages, 0, MEM_RELEASE));
    std::uint8_t* const watched = pages;        // write-watched (when write watching is on)
    std::uint8_t* const guarded = pages + half;  // plain
    EXPECT(VirtualAlloc(watched, half, MEM_RESERVE | (write_watched ? MEM_WRITE_WATCH : 0), PAGE_READWRITE) == watched);
    EXPECT(VirtualAlloc(guarded, half, MEM_RESERVE, PAGE_READWRITE) == guarded);
    EXPECT(VirtualAlloc(watched, committed, MEM_COMMIT, PAGE_READWRITE) == watched);
    EXPECT(VirtualAlloc(guarded, committed, MEM_COMMIT, PAGE_READWRITE) == guarded);
    if (write_watched) leonida::title_write_watch::add(reinterpret_cast<std::uintptr_t>(watched), half);
    // Elsewhere: a write-watched reservation for the record thread to add and remove, and pages to pin.
    auto* spare = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    EXPECT(spare != nullptr);

    std::atomic<std::uint64_t> progress{0};  // bumped by every submission and every writer's round
    std::atomic<bool> stop{false}, finished{false};
    std::thread watchdog([&] {
        std::uint64_t seen = progress.load();
        ULONGLONG since = GetTickCount64();
        while (!finished.load()) {
            Sleep(100);
            const std::uint64_t now = progress.load();
            if (now != seen) {
                seen = now;
                since = GetTickCount64();
            } else if (GetTickCount64() - since > 30000) {
                // No heap here: the process heap may be what is stuck.
                static const char message[] =
                    "vulkan slot-133 heap-lock faults: no progress for 30 s: deadlock (a lock the write-fault handler "
                    "waits for is held by a thread waiting for the process heap)\n";
                DWORD wrote = 0;
                WriteFile(GetStdHandle(STD_ERROR_HANDLE), message, DWORD(sizeof(message) - 1), &wrote, nullptr);
                TerminateProcess(GetCurrentProcess(), 3);
            }
        }
    });
    unsigned submissions = 0;
    std::uint64_t faults = 0;
    int failed = 0;
    {
        Ref<ID3D12Resource> buffer;
        buffer.p = place_shadow(device, pages, size);
        GpuReader reader;
        std::vector<std::uint8_t> got;
        if (!buffer.p || reader.init(device, size) || reader.read(buffer.p, got)) {
            failed = 1;
        } else {
            for (SIZE_T i = 0; i < committed; i += 0x1000) watched[i] = 1;  // hot from now on
            const std::uint64_t faults_before = rt::d3d12::title_shadow_watch(device).faults;
            std::vector<std::thread> threads;
            g_heap_gate_on = true;
            for (unsigned t = 0; t < 3; ++t)
                threads.emplace_back([&, t] {
                    const HANDLE heap = GetProcessHeap();
                    for (std::uint8_t round = 1; !stop.load(std::memory_order_relaxed); ++round) {
                        AcquireSRWLockExclusive(&g_heap_gate);
                        HeapLock(heap);
                        void* block = _aligned_malloc(64 + (round % 32) * 160, 16);
                        for (SIZE_T at = t * 64; at < committed; at += 0x10000)  // a byte per block
                            *reinterpret_cast<volatile std::uint8_t*>(guarded + at) = round;
                        _aligned_free(block);
                        HeapUnlock(heap);
                        ReleaseSRWLockExclusive(&g_heap_gate);
                        progress.fetch_add(1, std::memory_order_relaxed);
                        // About 50 us outside the lock: the other threads get to allocate.
                        LARGE_INTEGER now{}, frequency{};
                        QueryPerformanceFrequency(&frequency);
                        QueryPerformanceCounter(&now);
                        const LONGLONG until = now.QuadPart + frequency.QuadPart / 20000;
                        while (QueryPerformanceCounter(&now) && now.QuadPart < until) YieldProcessor();
                    }
                });
            for (unsigned t = 0; t < 3; ++t)
                threads.emplace_back([&, t] {
                    std::vector<void*> blocks(64, nullptr);
                    for (unsigned round = 0; !stop.load(std::memory_order_relaxed); ++round) {
                        void*& block = blocks[(round * 7 + t) % blocks.size()];
                        _aligned_free(block);
                        block = _aligned_malloc(16u << ((round + t) % 13), 16);
                        if (round % 64 == 0) progress.fetch_add(1, std::memory_order_relaxed);
                    }
                    for (void* block : blocks) _aligned_free(block);
                });
            threads.emplace_back([&] {
                for (std::uint8_t round = 1; !stop.load(std::memory_order_relaxed); ++round)
                    for (SIZE_T at = 8; at < committed; at += 0x1000)
                        *reinterpret_cast<volatile std::uint8_t*>(watched + at) = round;
            });
            threads.emplace_back([&] {
                const auto spare_at = reinterpret_cast<std::uintptr_t>(spare);
                for (unsigned round = 0; !stop.load(std::memory_order_relaxed); ++round) {
                    if (write_watched) {
                        leonida::title_write_watch::add(spare_at, 0x10000);
                        leonida::title_write_watch::mark(reinterpret_cast<std::uintptr_t>(watched) + (round % 16) * 0x1000,
                                                         reinterpret_cast<std::uintptr_t>(watched) + committed);
                        leonida::title_write_watch::remove(spare_at);
                    }
                    leonida::title_page_aliases::set(reinterpret_cast<std::uintptr_t>(guarded + half - 0x10000),
                                                     round % 2 == 0);
                    const std::uint64_t pin = leonida::kernel_writes::begin_write(spare + (round % 16) * 0x1000, 0x1000,
                                                                                   nullptr);
                    leonida::kernel_writes::end_write(pin, false);
                }
                leonida::title_page_aliases::set(reinterpret_cast<std::uintptr_t>(guarded + half - 0x10000), false);
            });
            const ULONGLONG until = GetTickCount64() + 3000;
            while (!failed && GetTickCount64() < until) {
                failed = reader.read(buffer.p, got);
                ++submissions;
                progress.fetch_add(1, std::memory_order_relaxed);
            }
            stop = true;
            for (std::thread& t : threads) t.join();
            g_heap_gate_on = false;
            const auto watch_after = rt::d3d12::title_shadow_watch(device);
            faults = watch_after.faults - faults_before;
            // With write watching on, the watched half must have stayed hot (asked each refresh, not guarded): else
            // the asking path this stresses never ran and the test proves nothing about it.
            if (write_watched && !failed && watch_after.hot_bytes == 0) {
                std::fprintf(stderr, "%s: the write-watched half is not hot after the stress\n", who);
                failed = 1;
            }
            if (!failed) failed = reader.read(buffer.p, got);
            if (!failed) {
                const std::vector<std::uint8_t> want_guarded(guarded, guarded + committed);
                const std::vector<std::uint8_t> got_guarded(got.begin() + half, got.begin() + half + committed);
                const long long wrong = first_difference(got_guarded, want_guarded);
                if (wrong >= 0) std::fprintf(stderr, "%s: guarded half: first wrong byte at %lld\n", who, wrong);
                failed = wrong >= 0;
            }
        }
    }
    finished = true;
    watchdog.join();
    if (write_watched) leonida::title_write_watch::remove(reinterpret_cast<std::uintptr_t>(watched));
    VirtualFree(watched, 0, MEM_RELEASE);
    VirtualFree(guarded, 0, MEM_RELEASE);
    VirtualFree(spare, 0, MEM_RELEASE);
    EXPECT(!failed);
    EXPECT(faults > 0);
    std::printf("%s: %u submissions while threads faulted on guarded pages holding the process heap's lock (%llu "
                "faults) and others churned the heap, the write-watch record and journal, the aliases and the pins: "
                "no deadlock, the last submission reads back right\n",
                who, submissions, static_cast<unsigned long long>(faults));
    return 0;
}

int check_written_pages_all(ID3D12Device* device) {
    int failures = 0;
    constexpr SIZE_T size = 4 * 0x10000;
    for (DWORD modifier : {DWORD(0), DWORD(PAGE_WRITECOMBINE)}) {
        // Half committed, so the import is refused whatever the cache attribute and the shadow answers.
        auto* pages = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_READWRITE | modifier));
        if (!pages || VirtualAlloc(pages, size / 2, MEM_COMMIT, PAGE_READWRITE | modifier) != pages) return failures + 1;
        failures += check_written_pages(device, modifier ? "half-committed write-combined range"
                                                         : "half-committed range",
                                        pages, size / 2, size);
        VirtualFree(pages, 0, MEM_RELEASE);
        // Wholly committed and write-combined (not imported), and placed 64 bytes into its first page.
        if (modifier) {
            pages = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT,
                                                            PAGE_READWRITE | modifier));
            if (!pages) return failures + 1;
            failures += check_written_pages(device, "write-combined range", pages, size, size);
            failures += check_written_pages(device, "write-combined range, 64 bytes in", pages + 64, size - 64,
                                            size - 64);
            VirtualFree(pages, 0, MEM_RELEASE);
        }
    }
    failures += check_commit_changes(device);
    failures += check_concurrent_writes(device, false);
    if (leonida::title_write_watch::enabled()) failures += check_concurrent_writes(device, true);
    failures += check_aliased_write(device);
    failures += check_aliased_guard(device);
    failures += check_aliased_guard_late_peer(device);
    failures += check_kernel_write(device);
    failures += check_shared_pages(device);
    return failures;
}

// A mapped plane above 256 KiB is hashed by samples every pass and every byte each sixth one. A byte the samples do
// not cover must still reach the GPU, from ordinary pages and from write-combined ones (the title's graphics
// memory), whose every-byte hash reads through a streaming copy.
int check_mapped_plane_byte(ID3D12Device* device, DWORD modifier) {
    const char* who = modifier ? "persistent write-combined plane, one byte" : "persistent plane, one byte";
    constexpr UINT width = 1024, height = 512, size = width * height;
    auto* pages = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE | modifier));
    EXPECT(pages);
    GpuReader gpu;
    EXPECT(!gpu.init(device, size));
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_R8_UNORM;
    desc.SampleDesc.Count = desc.DepthOrArraySize = desc.MipLevels = 1;
    leonida::title_xg_layouts::note({UINT(desc.Dimension), UINT(desc.Format), height, 1, 1, width}, 1);
    Ref<ID3D12Resource> texture;
    EXPECT_HR(rt::d3d12::create_buffer_at_host_address(device, pages, desc, 6, IID_PPV_ARGS(&texture.p)), S_OK);
    void* mapped = nullptr;
    EXPECT_HR(texture->Map(0, nullptr, &mapped), S_OK);
    EXPECT(mapped == pages);
    std::memset(pages, 0x40, size);
    ID3D12CommandList* lists[]{gpu.list.p};
    std::vector<std::uint8_t> pixels(size);
    std::ptrdiff_t changed = 0;
    for (unsigned pass = 0; pass < 14 && changed != 1; ++pass) {
        // 1,000 bytes into a page: the sampled hash reads the first 64 bytes of a page and 64 from its middle.
        if (pass == 1) pages[5 * 4096 + 1000] = 0x41;
        gpu.queue->ExecuteCommandLists(1, lists);
        EXPECT_HR(gpu.queue->Signal(gpu.fence.p, pass + 1), S_OK);
        EXPECT_HR(gpu.fence->SetEventOnCompletion(pass + 1, gpu.done), S_OK);
        EXPECT(WaitForSingleObject(gpu.done, 30000) == WAIT_OBJECT_0);
        EXPECT_HR(texture->ReadFromSubresource(pixels.data(), width, size, 0, nullptr), S_OK);
        changed = std::count(pixels.begin(), pixels.end(), std::uint8_t(0x41));
        EXPECT(std::count(pixels.begin(), pixels.end(), std::uint8_t(0x40)) == std::ptrdiff_t(size) - changed);
        if (!pass) EXPECT(!changed);
    }
    EXPECT(changed == 1);
    texture->Unmap(0, nullptr);
    texture.p->Release();
    texture.p = nullptr;
    VirtualFree(pages, 0, MEM_RELEASE);
    std::printf("%s: seen on the GPU\n", who);
    return 0;
}

// Replace mapped bytes twice, including compressed edge blocks, mip tails and array slices.
int check_mapped_bc_array(ID3D12Device* device) {
    const char* who = "persistent BC1 array";
    rt::xg_xs::ResourceDesc x{};
    x.dimension = rt::xg_xs::kTexture2D;
    x.width = 28;
    x.height = 20;
    x.depth_or_array = 2;
    x.mips = 4;
    x.format = DXGI_FORMAT_BC1_UNORM;
    x.samples = 1;
    rt::xg_xs::Texture tiled{};
    EXPECT(rt::xg_xs::build_texture(x, tiled));
    auto* pages = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, tiled.layout.size, MEM_RESERVE | MEM_COMMIT,
                                                          PAGE_READWRITE));
    EXPECT(pages);
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Format = DXGI_FORMAT_BC1_UNORM;
    desc.Width = x.width;
    desc.Height = x.height;
    desc.SampleDesc.Count = 1;
    desc.DepthOrArraySize = UINT16(x.depth_or_array);
    desc.MipLevels = UINT16(x.mips);
    leonida::title_xg_layouts::note({UINT(desc.Dimension), UINT(desc.Format), desc.Height,
                                    desc.DepthOrArraySize, desc.MipLevels, desc.Width}, x.layout);
    GpuReader gpu;
    EXPECT(!gpu.init(device, 256));
    Ref<ID3D12Resource> texture;
    EXPECT_HR(rt::d3d12::create_buffer_at_host_address(device, pages, desc, 6, IID_PPV_ARGS(&texture.p)), S_OK);
    void* mapped = nullptr;
    EXPECT_HR(texture->Map(0, nullptr, &mapped), S_OK);
    EXPECT(mapped == pages);
    std::vector<std::uint64_t> columns;
    std::vector<std::vector<std::uint8_t>> patterns(x.mips * x.depth_or_array);
    for (unsigned pass = 1; pass <= 2; ++pass) {
        for (unsigned sub = 0; sub < patterns.size(); ++sub) {
            const auto level = sub % x.mips;
            const auto& mip = tiled.layout.plane[0].mips[level];
            const auto row = mip.width_elements * tiled.format.bytes, bytes = row * mip.height_elements;
            auto& pattern = patterns[sub];
            pattern.resize(bytes);
            for (unsigned i = 0; i < bytes; ++i) pattern[i] = std::uint8_t(pass * 71 + sub * 13 + i);
            rt::xg_xs::copy_elements<true>(tiled, level, sub / x.mips, pattern.data(), pages, row, bytes, columns);
        }
        ID3D12CommandList* lists[]{gpu.list.p};
        gpu.queue->ExecuteCommandLists(1, lists);
        EXPECT_HR(gpu.queue->Signal(gpu.fence.p, pass), S_OK);
        EXPECT_HR(gpu.fence->SetEventOnCompletion(pass, gpu.done), S_OK);
        EXPECT(WaitForSingleObject(gpu.done, 30000) == WAIT_OBJECT_0);
        for (unsigned sub = 0; sub < patterns.size(); ++sub) {
            const auto& mip = tiled.layout.plane[0].mips[sub % x.mips];
            std::vector<std::uint8_t> got(patterns[sub].size());
            EXPECT_HR(texture->ReadFromSubresource(got.data(), mip.width_elements * tiled.format.bytes,
                                                   UINT(got.size()), sub, nullptr), S_OK);
            EXPECT(got == patterns[sub]);
        }
    }
    texture->Unmap(0, nullptr);
    texture.p->Release();
    texture.p = nullptr;
    VirtualFree(pages, 0, MEM_RELEASE);
    return 0;
}

void print_cases(const std::vector<Case>& cases, const char* who) {
    for (const Case& c : cases)
        std::printf("%s: %-44s hr=0x%08lX size=%llu align=%llu type=%d cpu=%d pool=%d flags=0x%x\n", who, c.name,
                    static_cast<unsigned long>(c.hr), static_cast<unsigned long long>(c.desc.SizeInBytes),
                    static_cast<unsigned long long>(c.desc.Alignment), int(c.desc.Properties.Type),
                    int(c.desc.Properties.CPUPageProperty), int(c.desc.Properties.MemoryPoolPreference),
                    unsigned(c.desc.Flags));
}
}  // namespace

int main(int argc, char** argv) {
    const bool prefix_only = argc == 2 && std::strcmp(argv[1], "--host-prefix-only") == 0;
    const bool async_plane = argc == 2 && std::strcmp(argv[1], "--mapped-plane-async") == 0;
    if (argc != 1 && !prefix_only && !async_plane) return 2;
    if (async_plane) _putenv_s("LEONIDA_TEXTURE_REFRESH_ASYNC", "1");  // read once, at the device's first refresh
    _putenv_s("LEONIDA_TITLE_TEXTURE_PERIOD_MS", "0");
    _putenv_s("LEONIDA_TITLE_TEXTURE_CHECK_KB", "0");
    vktest::Device vk;
    const auto status = vk.initialize();
    if (status == vktest::DeviceStatus::unavailable) return 77;
    if (status != vktest::DeviceStatus::ready) return 1;
    int failures = 0;
    {
        Ref<ID3D12Device> ours;
        rt::d3d12::VulkanDeviceInfo info{};
        info.get_instance_proc = vk.instance_proc;
        info.instance = vk.instance;
        info.physical = vk.physical;
        info.device = vk.device;
        info.queue = vk.queue;
        info.queue_family = vk.family;
        info.host_pointer_alignment = vk.host_pointer_alignment;
        info.host_image_copy = vk.host_image_copy;
        info.bindless = vk.bindless;
        info.bindless_partially_bound = vk.partially_bound;
        info.robust_image_access = vk.robust_image_access;  // a Texture3D UAV is bound only with it
        if (FAILED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&ours.p)))) return 1;
        if (prefix_only) {
            if (!vk.host_pointer_alignment) return 77;
            const int result = check_host_prefix_buffers(ours.p);
            return result || vk.validation_errors() ? 1 : 0;
        }
        if (async_plane) {
            int result = vk.host_image_copy ? check_mapped_plane_first_use(ours.p) : 0;
            auto no_host_copy = info;  // the staged upload too
            no_host_copy.host_image_copy = false;
            Ref<ID3D12Device> staged;
            if (FAILED(rt::d3d12::create_vulkan_device(no_host_copy, IID_PPV_ARGS(&staged.p)))) return 1;
            result += check_mapped_plane_first_use(staged.p);
            return result || vk.validation_errors() ? 1 : 0;
        }
        failures += check(ours.p, "vulkan");
        const std::vector<Case> vulkan_cases = open_cases(ours.p);
        print_cases(vulkan_cases, "vulkan");
        failures += check_import(ours.p, vk.host_pointer_alignment != 0);
        failures += check_placed_target_backing(ours.p);
        if (vk.host_pointer_alignment) failures += check_title_placed_all(ours.p);
        if (vk.host_image_copy) {
            failures += check_pending_stream_views(ours.p);
            failures += check_placed_view_map_changes(ours.p);
            failures += check_shared_bc_placements(ours.p);
            failures += check_mapped_plane(ours.p);
            for (const DWORD modifier : {DWORD(0), DWORD(PAGE_WRITECOMBINE)})
                failures += check_mapped_plane_byte(ours.p, modifier);
            failures += check_mapped_bc_array(ours.p);
        }
        {
            // Without host image copies a title image is filled through the staged upload, a linear plane too.
            auto no_host_copy = info;
            no_host_copy.host_image_copy = false;
            Ref<ID3D12Device> staged;
            if (FAILED(rt::d3d12::create_vulkan_device(no_host_copy, IID_PPV_ARGS(&staged.p)))) return 1;
            failures += check_placed_view_map_changes(staged.p);
            failures += check_shared_bc_placements(staged.p, true);
            failures += check_mapped_plane(staged.p);
            failures += check_mapped_bc_array(staged.p);
        }
        failures += check_written_pages_all(ours.p);
        {
            // A second device over the same Vulkan device: its shadows are trackers of their own.
            Ref<ID3D12Device> second;
            if (FAILED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&second.p)))) second.p = nullptr;
            // LEONIDA_WRITE_WATCH=0 (the d3d12_existing_heap_no_write_watch test): xmem records nothing, and only the
            // guarded cases run.
            if (leonida::title_write_watch::enabled()) {
                for (const DWORD modifier : {DWORD(0), DWORD(PAGE_WRITECOMBINE)})
                    failures += check_write_watched(ours.p, second.p, modifier);
                failures += check_rereserved(ours.p);
            }
        }
        failures += check_heap_lock_faults(ours.p);
        Ref<IDXGIFactory4> factory;
        if (!hostwine::host_is_wine() && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory.p)))) {
            for (const bool warp : {false, true}) {
                Ref<IDXGIAdapter> adapter;
                Ref<ID3D12Device> host;
                std::string adapter_name;
                if (!hostadapter::reference(factory.p, vk, warp, &adapter.p, adapter_name)) continue;
                if (FAILED(D3D12CreateDevice(adapter.p, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&host.p)))) continue;
                failures += check(host.p, ("host " + adapter_name).c_str());
                const std::string who = "host " + adapter_name;
                const std::vector<Case> host_cases = open_cases(host.p);
                print_cases(host_cases, who.c_str());
                if (vk.host_pointer_alignment) failures += compare_cases(vulkan_cases, host_cases, who.c_str());
            }
        }
    }
    if (vk.validation_errors()) {
        std::fprintf(stderr, "%u validation errors\n", vk.validation_errors());
        return 1;
    }
    return failures ? 1 : 0;
}
