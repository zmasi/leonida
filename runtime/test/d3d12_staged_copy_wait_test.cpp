// Review D9: WriteToSubresource / ReadFromSubresource on a texture the host cannot copy (no
// VK_EXT_host_image_copy, or not in GENERAL) replay a CopyTextureRegion on a staging queue and wait for it. That wait
// was vkQueueWaitIdle on the device's first VkQueue, which the first D3D12 direct queue also uses, held under the
// device's submit lock: every such CPU copy waited for all the game work queued on that queue, and no queue could
// submit meanwhile. The staging copy now waits only for its own submission, on a VkQueue the game's first queues
// are not on: the copy family's for a colour texture (a second graphics VkQueue did not help on NVIDIA, which runs
// it behind the busy one: 32 ms either way).
//
// The standalone device queues about 30 ms of buffer copies on a direct queue, then writes and reads back a 64x64
// colour texture in COPY_DEST (so through the staging copy). Where there is a copy family, the write must not take
// as long as the queued work (it did: 31.8 ms of 32.1); the bytes must round-trip either way. Prints both times.
// Skips (77) without a Vulkan GPU. No shaders or game data.
#include "vk_test_device.h"
#include "../shims/d3d12_com.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

namespace {

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

double ms_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

}  // namespace

int main() {
    vktest::Device d;
    const auto device_status = d.initialize();
    if (device_status == vktest::DeviceStatus::unavailable) return 77;  // no Vulkan GPU: ctest skips
    if (device_status != vktest::DeviceStatus::ready) return 1;
    std::uint32_t family_count = 0;
    const auto families_of = d.instance_fn<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        "vkGetPhysicalDeviceQueueFamilyProperties");
    families_of(d.physical, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    families_of(d.physical, &family_count, families.data());
    // A family the standalone device makes D3D12 COPY queues in: transfer without graphics, copying whole texels.
    bool copy_family = false;
    for (const auto& f : families)
        copy_family |= (f.queueFlags & VK_QUEUE_TRANSFER_BIT) && !(f.queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                       f.minImageTransferGranularity.width == 1 && f.minImageTransferGranularity.height == 1 &&
                       f.minImageTransferGranularity.depth == 1;

    Ref<ID3D12Device> device;
    EXPECT(SUCCEEDED(rt::d3d12::create_vulkan_device_standalone(IID_PPV_ARGS(&device.p))));
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    Ref<ID3D12CommandQueue> queue;
    EXPECT(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))));
    Ref<ID3D12CommandAllocator> allocator;
    EXPECT(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
    Ref<ID3D12GraphicsCommandList> list;
    EXPECT(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, nullptr,
                                               IID_PPV_ARGS(&list.p))));
    Ref<ID3D12Fence> fence;
    EXPECT(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));

    const D3D12_HEAP_PROPERTIES gpu{D3D12_HEAP_TYPE_DEFAULT};
    constexpr UINT64 kBufferBytes = 96ull << 20;  // two fit the GPU address arena (256 MiB)
    D3D12_RESOURCE_DESC buffer_desc{};
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Width = kBufferBytes;
    buffer_desc.Height = 1;
    buffer_desc.DepthOrArraySize = 1;
    buffer_desc.MipLevels = 1;
    buffer_desc.SampleDesc.Count = 1;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Ref<ID3D12Resource> a, b;
    EXPECT(SUCCEEDED(device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                                     D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&a.p))));
    EXPECT(SUCCEEDED(device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                                     D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&b.p))));
    D3D12_RESOURCE_DESC texture_desc{};
    texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture_desc.Width = 64;
    texture_desc.Height = 64;
    texture_desc.DepthOrArraySize = 1;
    texture_desc.MipLevels = 1;
    texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture_desc.SampleDesc.Count = 1;
    Ref<ID3D12Resource> texture;
    EXPECT(SUCCEEDED(device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &texture_desc,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&texture.p))));
    std::vector<std::uint8_t> texels(64 * 64 * 4), back(texels.size());
    for (std::size_t i = 0; i < texels.size(); ++i) texels[i] = std::uint8_t(i * 31 + 7);
    // The first copy makes the staging queue; not timed.
    EXPECT(SUCCEEDED(texture->WriteToSubresource(0, nullptr, texels.data(), 256, 0)));
    // A first submission on the direct queue zero-fills the new buffers, which every other queue's next submission
    // waits for (as it should): done before the timed part.
    list->CopyBufferRegion(b.p, 0, a.p, 0, 256);
    EXPECT(SUCCEEDED(list->Close()));
    ID3D12CommandList* lists[] = {list.p};
    queue->ExecuteCommandLists(1, lists);
    EXPECT(SUCCEEDED(queue->Signal(fence.p, 1)));
    EXPECT(SUCCEEDED(fence->SetEventOnCompletion(1, nullptr)));
    EXPECT(SUCCEEDED(texture->WriteToSubresource(0, nullptr, texels.data(), 256, 0)));  // after that wait, untimed
    EXPECT(SUCCEEDED(allocator->Reset()));
    EXPECT(SUCCEEDED(list->Reset(allocator.p, nullptr)));

    // Work for the direct queue: 64 copies of 96 MiB, back and forth, each buffer moved between COPY_SOURCE and
    // COPY_DEST in between as D3D12 asks.
    const auto transition = [](ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        return barrier;
    };
    constexpr auto kSource = D3D12_RESOURCE_STATE_COPY_SOURCE, kDest = D3D12_RESOURCE_STATE_COPY_DEST;
    const D3D12_RESOURCE_BARRIER start_states[] = {transition(a.p, D3D12_RESOURCE_STATE_COMMON, kSource),
                                                   transition(b.p, D3D12_RESOURCE_STATE_COMMON, kDest)};
    list->ResourceBarrier(2, start_states);
    for (int i = 0; i < 64; ++i) {
        ID3D12Resource* from = i % 2 ? b.p : a.p;
        ID3D12Resource* to = i % 2 ? a.p : b.p;
        list->CopyBufferRegion(to, 0, from, 0, kBufferBytes);
        const D3D12_RESOURCE_BARRIER swap[] = {transition(from, kSource, kDest), transition(to, kDest, kSource)};
        list->ResourceBarrier(2, swap);
    }
    EXPECT(SUCCEEDED(list->Close()));
    const auto start = std::chrono::steady_clock::now();
    queue->ExecuteCommandLists(1, lists);
    EXPECT(SUCCEEDED(queue->Signal(fence.p, 2)));

    for (std::size_t i = 0; i < texels.size(); ++i) texels[i] = std::uint8_t(i * 13 + 5);
    const auto write_start = std::chrono::steady_clock::now();
    EXPECT(SUCCEEDED(texture->WriteToSubresource(0, nullptr, texels.data(), 256, 0)));
    const double write_ms = ms_since(write_start);
    const bool busy_after_write = fence->GetCompletedValue() < 2;
    EXPECT(SUCCEEDED(fence->SetEventOnCompletion(2, nullptr)));
    const double work_ms = ms_since(start);
    EXPECT(SUCCEEDED(texture->ReadFromSubresource(back.data(), 256, 0, 0, nullptr)));
    EXPECT(std::memcmp(back.data(), texels.data(), texels.size()) == 0);

    std::printf("staged 64x64 WriteToSubresource with the direct queue busy: %.2f ms; the queued copies: %.2f ms; "
                "direct queue still busy after the write: %s; copy family: %s\n",
                write_ms, work_ms, busy_after_write ? "yes" : "no", copy_family ? "yes" : "no");
    if (copy_family && work_ms >= 10.0) {
        EXPECT(busy_after_write);
        EXPECT(write_ms < work_ms / 2);
    } else {
        std::printf("no copy family, or too little queued work to tell: the times are not compared\n");
    }
    std::printf("d3d12_staged_copy_wait_test: OK\n");
    return 0;
}
