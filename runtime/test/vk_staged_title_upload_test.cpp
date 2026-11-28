// Staged title image uploads skip waiting on pending queue tickets when an unreferenced image is still in
// VK_IMAGE_LAYOUT_UNDEFINED, and report VK_NOT_READY (retrying later without failing or advancing past a free
// ring slot) when a wait on an already-published image times out.
#include "../src/vulkan_backend.h"
#ifdef _WIN32
#include "../shims/d3d12_title_image_upload.h"
#endif

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <vector>

namespace {
int g_failures = 0;
int g_wait_calls = 0;
int g_submit_calls = 0;
int g_copy_calls = 0;
VkResult g_wait_result = VK_SUCCESS;
std::uint64_t g_pending_counter = 0;
std::uintptr_t g_next_handle = 10;

struct FakeMemory {
    std::vector<std::uint8_t> bytes;
};
std::vector<FakeMemory*> g_allocations;

void expect(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

template<typename Handle>
Handle handle(std::uintptr_t value) {
    if constexpr (std::is_pointer_v<Handle>) return reinterpret_cast<Handle>(value);
    else return static_cast<Handle>(value);
}

VkSemaphore pending_semaphore() { return handle<VkSemaphore>(99); }

VkResult VKAPI_CALL create_pool(VkDevice, const VkCommandPoolCreateInfo*, const VkAllocationCallbacks*,
                                VkCommandPool* out) {
    *out = handle<VkCommandPool>(++g_next_handle);
    return VK_SUCCESS;
}
void VKAPI_CALL destroy_pool(VkDevice, VkCommandPool, const VkAllocationCallbacks*) {}

VkResult VKAPI_CALL create_semaphore(VkDevice, const VkSemaphoreCreateInfo*, const VkAllocationCallbacks*,
                                     VkSemaphore* out) {
    *out = handle<VkSemaphore>(++g_next_handle);
    return VK_SUCCESS;
}
void VKAPI_CALL destroy_semaphore(VkDevice, VkSemaphore, const VkAllocationCallbacks*) {}

VkResult VKAPI_CALL semaphore_counter(VkDevice, VkSemaphore sem, std::uint64_t* out) {
    *out = (sem == pending_semaphore()) ? g_pending_counter : UINT64_MAX;
    return VK_SUCCESS;
}

VkResult VKAPI_CALL wait_semaphores(VkDevice, const VkSemaphoreWaitInfo*, std::uint64_t) {
    ++g_wait_calls;
    return g_wait_result;
}

VkResult VKAPI_CALL allocate_commands(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer* out) {
    *out = handle<VkCommandBuffer>(++g_next_handle);
    return VK_SUCCESS;
}
VkResult VKAPI_CALL reset_command(VkCommandBuffer, VkCommandBufferResetFlags) { return VK_SUCCESS; }
VkResult VKAPI_CALL begin_command(VkCommandBuffer, const VkCommandBufferBeginInfo*) { return VK_SUCCESS; }
VkResult VKAPI_CALL end_command(VkCommandBuffer) { return VK_SUCCESS; }
VkResult VKAPI_CALL queue_submit(VkQueue, std::uint32_t, const VkSubmitInfo*, VkFence) {
    ++g_submit_calls;
    return VK_SUCCESS;
}
VkResult VKAPI_CALL queue_idle(VkQueue) { return VK_SUCCESS; }

void VKAPI_CALL cmd_barrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags,
                            std::uint32_t, const VkMemoryBarrier*, std::uint32_t, const VkBufferMemoryBarrier*,
                            std::uint32_t, const VkImageMemoryBarrier*) {}

void VKAPI_CALL cmd_copy_buffer_to_image(VkCommandBuffer, VkBuffer, VkImage, VkImageLayout,
                                         std::uint32_t, const VkBufferImageCopy*) {
    ++g_copy_calls;
}

VkResult VKAPI_CALL create_buffer(VkDevice, const VkBufferCreateInfo* info, const VkAllocationCallbacks*,
                                  VkBuffer* out) {
    *out = handle<VkBuffer>(static_cast<std::uintptr_t>(info->size ? info->size : 1));
    return VK_SUCCESS;
}
void VKAPI_CALL destroy_buffer(VkDevice, VkBuffer, const VkAllocationCallbacks*) {}

void VKAPI_CALL buffer_requirements(VkDevice, VkBuffer buf, VkMemoryRequirements* req) {
    *req = {};
    req->size = reinterpret_cast<std::uintptr_t>(buf);
    req->alignment = 64;
    req->memoryTypeBits = 1;
}
VkResult VKAPI_CALL bind_buffer_memory(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize) { return VK_SUCCESS; }
VkResult VKAPI_CALL map_memory(VkDevice, VkDeviceMemory mem, VkDeviceSize offset, VkDeviceSize, VkMemoryMapFlags,
                               void** out) {
    auto* block = reinterpret_cast<FakeMemory*>(mem);
    *out = block->bytes.data() + offset;
    return VK_SUCCESS;
}
void VKAPI_CALL free_memory(VkDevice, VkDeviceMemory mem, const VkAllocationCallbacks*) {
    auto* block = reinterpret_cast<FakeMemory*>(mem);
    delete block;
}

PFN_vkVoidFunction VKAPI_CALL fake_proc(VkDevice, const char* name) {
#define PROC(vk_name, fn) \
    if (std::strcmp(name, #vk_name) == 0) return reinterpret_cast<PFN_vkVoidFunction>(fn)
    PROC(vkCreateCommandPool, create_pool);
    PROC(vkDestroyCommandPool, destroy_pool);
    PROC(vkCreateSemaphore, create_semaphore);
    PROC(vkDestroySemaphore, destroy_semaphore);
    PROC(vkGetSemaphoreCounterValue, semaphore_counter);
    PROC(vkWaitSemaphores, wait_semaphores);
    PROC(vkAllocateCommandBuffers, allocate_commands);
    PROC(vkResetCommandBuffer, reset_command);
    PROC(vkBeginCommandBuffer, begin_command);
    PROC(vkEndCommandBuffer, end_command);
    PROC(vkQueueSubmit, queue_submit);
    PROC(vkQueueWaitIdle, queue_idle);
    PROC(vkCmdPipelineBarrier, cmd_barrier);
    PROC(vkCmdCopyBufferToImage, cmd_copy_buffer_to_image);
    PROC(vkCreateBuffer, create_buffer);
    PROC(vkDestroyBuffer, destroy_buffer);
    PROC(vkGetBufferMemoryRequirements, buffer_requirements);
    PROC(vkBindBufferMemory, bind_buffer_memory);
    PROC(vkMapMemory, map_memory);
    PROC(vkFreeMemory, free_memory);
#undef PROC
    return nullptr;
}

leonida::ResourceBinding make_image_binding(std::uintptr_t img_id, std::uint32_t w = 16, std::uint32_t h = 16) {
    leonida::ResourceBinding b{};
    b.image = handle<VkImage>(img_id);
    b.format = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    b.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
    b.width = w;
    b.height = h;
    b.mip_levels = 1;
    b.array_layers = 1;
    b.depth = 1;
    b.samples = 1;
    b.sampled = true;
    return b;
}

void test_context_staged_upload_waits() {
    g_wait_calls = 0;
    g_submit_calls = 0;
    g_copy_calls = 0;
    g_pending_counter = 0;
    g_wait_result = VK_TIMEOUT;

    leonida::VulkanContext ctx(fake_proc, handle<VkDevice>(1), false);
    {
        std::lock_guard lock(ctx.submit_mutex());
        ctx.record_submit_locked(pending_semaphore(), 1);
    }

    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    ctx.add_resource(101, make_image_binding(101), VK_IMAGE_LAYOUT_UNDEFINED);

    // 1. An unreferenced image still in UNDEFINED skips waiting on unrelated pending queue work.
    int copies = 0;
    VkResult r = ctx.write_staged_title_image(
        101, range, [&](const leonida::ReplayFunctions&, const leonida::LayoutPieces& pieces) {
            expect(pieces.size() == 1 && pieces[0].layout == VK_IMAGE_LAYOUT_UNDEFINED,
                   "initial pieces should be UNDEFINED");
            ++copies;
            return VK_SUCCESS;
        },
        /*unreferenced=*/true);
    expect(r == VK_SUCCESS, "unreferenced initial upload should succeed without waiting");
    expect(g_wait_calls == 0 && copies == 1, "unreferenced initial upload must not call vkWaitSemaphores");
    expect(ctx.layout(101, 0) == VK_IMAGE_LAYOUT_GENERAL, "initial upload should transition tracker to GENERAL");

    // 2. A second upload to the now-GENERAL image must still wait on pending_semaphore() and map VK_TIMEOUT to VK_NOT_READY.
    r = ctx.write_staged_title_image(
        101, range, [&](const leonida::ReplayFunctions&, const leonida::LayoutPieces&) {
            ++copies;
            return VK_SUCCESS;
        },
        /*unreferenced=*/true);
    expect(r == VK_NOT_READY, "timed-out upload on GENERAL image should return VK_NOT_READY");
    expect(g_wait_calls == 1 && copies == 1, "timed-out upload must call vkWaitSemaphores and skip copy");

    // 3. A potentially referenced image (unreferenced == false) waits even while tracker layout is UNDEFINED.
    ctx.add_resource(102, make_image_binding(102), VK_IMAGE_LAYOUT_UNDEFINED);
    r = ctx.write_staged_title_image(
        102, range, [&](const leonida::ReplayFunctions&, const leonida::LayoutPieces&) {
            ++copies;
            return VK_SUCCESS;
        },
        /*unreferenced=*/false);
    expect(r == VK_NOT_READY && g_wait_calls == 2 && copies == 1,
           "referenced UNDEFINED upload must wait and return VK_NOT_READY on timeout");
    expect(ctx.layout(102, 0) == VK_IMAGE_LAYOUT_UNDEFINED, "skipped upload must keep UNDEFINED layout");

    // 4. Once pending work completes, the deferred upload succeeds and clears image_work_pending_.
    g_wait_result = VK_SUCCESS;
    r = ctx.write_staged_title_image(
        102, range, [&](const leonida::ReplayFunctions&, const leonida::LayoutPieces&) {
            ++copies;
            return VK_SUCCESS;
        },
        /*unreferenced=*/false);
    expect(r == VK_SUCCESS && g_wait_calls == 3 && copies == 2, "upload after semaphore completion should succeed");
    expect(ctx.layout(102, 0) == VK_IMAGE_LAYOUT_GENERAL, "completed retry should transition tracker to GENERAL");
}

#ifdef _WIN32
struct FakeUploadDevice {
    struct Info {
        VkDevice device{};
        std::uint32_t queue_family = 0;
        VkQueue queue{};
        std::vector<VkQueue> command_queues;
        std::vector<VkQueue> compute_queues;
        std::vector<VkQueue> copy_queues;
    } info;
    leonida::VulkanContext* context = nullptr;

    VkDeviceMemory allocate(const VkMemoryRequirements& req, VkMemoryPropertyFlags, VkMemoryAllocateFlags) {
        auto* mem = new FakeMemory();
        mem->bytes.resize(static_cast<std::size_t>(req.size ? req.size : 64), 0);
        return reinterpret_cast<VkDeviceMemory>(mem);
    }
};

void test_title_image_upload_ring() {
    g_wait_calls = 0;
    g_submit_calls = 0;
    g_copy_calls = 0;
    g_pending_counter = 0;
    g_wait_result = VK_TIMEOUT;

    leonida::VulkanContext ctx(fake_proc, handle<VkDevice>(1), false);
    {
        std::lock_guard lock(ctx.submit_mutex());
        ctx.record_submit_locked(pending_semaphore(), 1);
    }

    FakeUploadDevice dev{};
    dev.info.device = ctx.device();
    dev.info.queue = handle<VkQueue>(1);
    dev.info.compute_queues = {handle<VkQueue>(2)};
    dev.context = &ctx;

    rt::xg_xs::ResourceDesc desc{};
    desc.dimension = rt::xg_xs::kTexture2D;
    desc.width = 16;
    desc.height = 16;
    desc.depth_or_array = 1;
    desc.mips = 1;
    desc.format = DXGI_FORMAT_BC1_UNORM;
    desc.samples = 1;
    desc.layout = rt::xg_xs::kSwizzled | rt::xg_xs::k4kbS;
    rt::xg_xs::Texture layout{};
    expect(rt::xg_xs::build_texture(desc, layout), "build_texture for 16x16 BC1 failed");

    std::vector<std::uint8_t> tiled(static_cast<std::size_t>(layout.layout.size), 0x5a);
    std::vector<std::uint64_t> columns;
    const auto binding = make_image_binding(201, 16, 16);
    ctx.add_resource(201, binding, VK_IMAGE_LAYOUT_UNDEFINED);

    rt::d3d12::TitleImageUpload uploader;
    bool nonzero = false, busy = true;

    // Initial unreferenced upload bypasses pending wait, submits transfer + consumer wait, and reports !busy.
    const bool first_ok = uploader.write(dev, 201, binding, layout, tiled.data(), columns, true, nonzero, 0, true, &busy);
    expect(first_ok && !busy && nonzero, "initial unreferenced TitleImageUpload::write should succeed");
    expect(g_wait_calls == 0 && g_copy_calls == 1 && g_submit_calls == 2,
           "initial upload should submit primary + consumer queue without waiting");

    // Six consecutive timed-out re-uploads (> 4 ring slots) report busy=true and rewind next_ cleanly.
    for (int attempt = 0; attempt < 6; ++attempt) {
        busy = false;
        const bool ok = uploader.write(dev, 201, binding, layout, tiled.data(), columns, false, nonzero, 0, false, &busy);
        expect(!ok && busy, "timed-out re-upload should report busy=true rather than hard failure");
    }
    expect(g_wait_calls == 6 && g_submit_calls == 2, "timed-out re-uploads must not submit new transfers");

    // Genuine Vulkan errors report busy=false so fill_failures still catches real faults.
    g_wait_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    busy = true;
    expect(!uploader.write(dev, 201, binding, layout, tiled.data(), columns, false, nonzero, 0, false, &busy) && !busy,
           "hard Vulkan error must report busy=false");

    // Once pending queue work finishes, the queued refresh succeeds cleanly.
    g_wait_result = VK_SUCCESS;
    busy = true;
    expect(uploader.write(dev, 201, binding, layout, tiled.data(), columns, false, nonzero, 0, false, &busy) && !busy,
           "retry after queue completion should succeed");
    expect(g_submit_calls == 4, "successful retry should submit transfer + consumer wait");

    // Partial-mip initial upload (from = 1 of 2 mips) skips the wait while mip 1 is UNDEFINED, then waits when
    // upgrading to full mips (from = 0) because mip 1 is already GENERAL.
    {
        std::lock_guard lock(ctx.submit_mutex());
        ctx.record_submit_locked(pending_semaphore(), 2);
    }
    g_wait_result = VK_TIMEOUT;
    rt::xg_xs::ResourceDesc mip_desc = desc;
    mip_desc.mips = 2;
    rt::xg_xs::Texture mip_layout{};
    expect(rt::xg_xs::build_texture(mip_desc, mip_layout), "build_texture for 2-mip BC1 failed");
    std::vector<std::uint8_t> mip_tiled(static_cast<std::size_t>(mip_layout.layout.size), 0x33);
    auto mip_binding = make_image_binding(202, 16, 16);
    mip_binding.mip_levels = 2;
    ctx.add_resource(202, mip_binding, VK_IMAGE_LAYOUT_UNDEFINED);
    const int waits_before = g_wait_calls;
    expect(uploader.write(dev, 202, mip_binding, mip_layout, mip_tiled.data(), columns, false, nonzero, 1, true, &busy) &&
               !busy && g_wait_calls == waits_before,
           "partial-mip initial upload should skip waiting while mip tail is UNDEFINED");
    expect(!uploader.write(dev, 202, mip_binding, mip_layout, mip_tiled.data(), columns, false, nonzero, 0, true, &busy) &&
               busy && g_wait_calls == waits_before + 1,
           "upgrading a partially filled texture must wait on pending work covering filled tail mips");

    uploader.release(dev);
}
#endif
}  // namespace

int main() {
    test_context_staged_upload_waits();
#ifdef _WIN32
    test_title_image_upload_ring();
#endif
    if (g_failures) return 1;
    std::puts("vk_staged_title_upload_test: ok");
    return 0;
}
