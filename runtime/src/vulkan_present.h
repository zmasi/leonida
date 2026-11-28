#pragma once

// Puts a D3D12 back buffer on screen through a Vulkan swapchain, for whichever front end the game turns out to use
// (DXGI or the console's PresentX; docs/d3d12_vulkan.md question 6). The back buffer is an ordinary texture in the
// D3D12 PRESENT (common) state; present() blits it to the acquired swapchain image, scaling if the window size
// differs, and queues the present. The application's own work reaches the same VkQueue first, so every draw it
// submitted before present() is in the frame.
//
// Needs VK_KHR_swapchain on the device and a surface from the caller (which this object then owns). The swapchain
// is recreated when the window changes size or the surface reports it out of date; a minimised window skips
// presents. Two frames are in flight; present() waits for the older one before reusing its command buffer.
//
// Beyond the plain blit (the Xbox PresentX path, docs/d3d12_vulkan.md section 4.5): a source rectangle (only that part
// of the back buffer is scaled to the window), a blank present (the window cleared to black), a swapchain format
// that follows the source (an sRGB source gets an sRGB swapchain, so the blit's decode and encode cancel; every
// other source gets a UNORM one), and an optional capture that copies each presented frame to host memory.
//
// Plane composition (PresentX with placement, a filter, HDR10 or a second plane): present_planes() runs a compute pass
// (present_compose.comp, vulkan_present_compose.h) over up to two textures into an RGBA16F image and copies that to the
// swapchain; with an HDR10 plane and a surface that lists an ST.2084 format the swapchain is created in it. The plain
// blit above is unchanged.
//
// Tearing on request (PresentX's ImmediateThresholdPercent): with VK_KHR_swapchain_maintenance1 (or the EXT original)
// the vsync swapchain is created with FIFO plus IMMEDIATE (FIFO_RELAXED when the surface cannot switch to IMMEDIATE),
// and a present that asks for it is queued in that mode alone. Without the extension, or without either mode, a
// request is ignored and the frame waits for the vblank.

#include "vulkan_backend.h"
#include "vulkan_present_compose.h"
#include "fsdraw_probe.h"  // LOCAL DIAGNOSTIC
#include "local_diagnostics.h"
#include "present_dump.h"

// Use the original EXT spellings. They are aliases of the promoted KHR types and are available in older headers.
#if defined(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME) && defined(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME)
#define LEONIDA_SWAPCHAIN_MAINTENANCE1 1
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace leonida {

// A sub-rectangle of a back buffer in texels; left/top inclusive, right/bottom exclusive (D3D12_RECT's convention).
struct PresentRect {
    std::int32_t left, top, right, bottom;
};

// One plane of present_planes(): a colour texture, the part of it shown, and where in the output it goes.
struct PresentPlane {
    ResourceId source{};
    PresentRect source_rect{};   // texels; used when has_source_rect
    bool has_source_rect = false;
    float dest[4] = {0, 0, 1, 1};  // left, top, right, bottom as fractions of the output (0..1)
    bool point_filter = false;     // nearest texel; otherwise bilinear inside the source rectangle
    bool hdr10 = false;            // the texture holds ST.2084 (PQ) Rec.2020 values; otherwise sRGB or G2.2 (PresentPlanes::sdr_eotf) Rec.709 ones
    bool straight_alpha = false;   // plane 1 only: alpha is not premultiplied into the colour
};
struct PresentPlanes {
    PresentPlane plane[2];
    std::uint32_t count = 1;
    // What SDR white maps to in an HDR10 output, and what HDR10 is divided by for an SDR one (the white of the SDR output).
    float sdr_white_nits = 203.0f;
    // How SDR planes (and an SDR output) are encoded: piecewise sRGB (default; what Windows applies to SDR content in HDR
    // mode, community-sourced, docs section 4.5) or a pure power of 2.2.
    enum class SdrEotf : std::uint32_t { srgb = 0, gamma22 = 1 };
    SdrEotf sdr_eotf = SdrEotf::srgb;
    float hdr_peak_nits = 1000.0f;  // peak of the HDR10 content: the white point of the tone map on an SDR output
};

namespace detail {
inline bool is_srgb_format(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8_SRGB: case VK_FORMAT_R8G8_SRGB: case VK_FORMAT_R8G8B8_SRGB: case VK_FORMAT_B8G8R8_SRGB:
    case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        return true;
    default:
        return false;
    }
}
// What vkCmdBlitImage cannot read: integer and block-compressed colour formats.
inline bool blit_unsupported_format(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SINT:
    case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT:
    case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_UINT:
    case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: case VK_FORMAT_UNDEFINED:
        return true;
    default:
        return format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && format <= VK_FORMAT_BC7_SRGB_BLOCK;
    }
}
}  // namespace detail

class VulkanPresenter {
public:
    VulkanPresenter(VulkanContext& context, PFN_vkGetInstanceProcAddr get_instance, VkInstance instance,
                    VkPhysicalDevice physical, VkQueue queue, std::uint32_t family, VkSurfaceKHR surface,
                    bool swapchain_maintenance1 = false,
                    std::function<VkExtent2D()> extent_fn = nullptr,
                    std::function<VkSurfaceKHR()> recreate_surface_fn = nullptr)
        : context_(context), instance_(instance), physical_(physical), queue_(queue), surface_(surface),
          maintenance1_(swapchain_maintenance1), extent_fn_(std::move(extent_fn)),
          recreate_surface_fn_(std::move(recreate_surface_fn)) {
#define LEONIDA_INSTANCE_FN(name) name##_ = reinterpret_cast<PFN_##name>(get_instance(instance, #name));
        LEONIDA_INSTANCE_FN(vkGetPhysicalDeviceSurfaceSupportKHR)
        LEONIDA_INSTANCE_FN(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)
        LEONIDA_INSTANCE_FN(vkGetPhysicalDeviceSurfaceFormatsKHR)
        LEONIDA_INSTANCE_FN(vkGetPhysicalDeviceSurfacePresentModesKHR)
        LEONIDA_INSTANCE_FN(vkGetPhysicalDeviceSurfaceCapabilities2KHR)
        LEONIDA_INSTANCE_FN(vkDestroySurfaceKHR)
        LEONIDA_INSTANCE_FN(vkGetDeviceProcAddr)
        LEONIDA_INSTANCE_FN(vkGetPhysicalDeviceMemoryProperties)
        LEONIDA_INSTANCE_FN(vkGetPhysicalDeviceFormatProperties)
#undef LEONIDA_INSTANCE_FN
        if (!vkGetDeviceProcAddr_ || !vkDestroySurfaceKHR_) return;
        const VkDevice device = context.device();
#define LEONIDA_DEVICE_FN(name) name##_ = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr_(device, #name));
        LEONIDA_DEVICE_FN(vkCreateSwapchainKHR)
        LEONIDA_DEVICE_FN(vkDestroySwapchainKHR)
        LEONIDA_DEVICE_FN(vkGetSwapchainImagesKHR)
        LEONIDA_DEVICE_FN(vkAcquireNextImageKHR)
        LEONIDA_DEVICE_FN(vkQueuePresentKHR)
        LEONIDA_DEVICE_FN(vkCmdBlitImage)
        LEONIDA_DEVICE_FN(vkCmdClearColorImage)
        LEONIDA_DEVICE_FN(vkCreateFence)
        LEONIDA_DEVICE_FN(vkDestroyFence)
        LEONIDA_DEVICE_FN(vkWaitForFences)
        LEONIDA_DEVICE_FN(vkResetFences)
#undef LEONIDA_DEVICE_FN
        if (!vkCreateSwapchainKHR_ || !vkQueuePresentKHR_ || !vkCmdBlitImage_ || !vkCreateFence_) return;

        VkBool32 supported = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR_(physical, family, surface, &supported);
        if (!supported) return;

        auto& vk = context.vk;
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = family;
        if (vk.vkCreateCommandPool(device, &pool, nullptr, &pool_) != VK_SUCCESS) return;
        for (Frame& frame : frames_) {
            VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            allocate.commandPool = pool_;
            allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate.commandBufferCount = 1;
            VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            if (vk.vkAllocateCommandBuffers(device, &allocate, &frame.commands) != VK_SUCCESS ||
                vkCreateFence_(device, &fence, nullptr, &frame.done) != VK_SUCCESS ||
                vk.vkCreateSemaphore(device, &semaphore, nullptr, &frame.acquired) != VK_SUCCESS)
                return;
        }
        // LEONIDA_CAPTURE=<dir>: besides presenting, copy the swapchain image to host memory and write it out as a
        // binary PPM, every LEONIDA_CAPTURE_EVERY presented frames (default 30), at most LEONIDA_CAPTURE_LIMIT of
        // them (default 24). Off unless the variable is set; a capture that cannot be made shows nothing and is
        // reported here instead of failing the presenter.
        if (const char* dir = std::getenv("LEONIDA_CAPTURE"); dir && *dir) {
            capture_dir_ = dir;
            // Create the folder: a capture run names its own output folder and should not have to make it first.
            std::error_code created;
            std::filesystem::create_directories(capture_dir_, created);
            capture_to_disk_ = true;
            capture_ = true;  // provision TRANSFER_SRC and staging; disk-only copies follow the requested cadence
            if (const char* every = std::getenv("LEONIDA_CAPTURE_EVERY")) capture_every_ = std::max(1, std::atoi(every));
            if (const char* limit = std::getenv("LEONIDA_CAPTURE_LIMIT")) capture_limit_ = std::max(1, std::atoi(limit));
            std::fprintf(stderr, "[rt] present: capture on: every %d presented frame(s), at most %d, into %s\n",
                         capture_every_, capture_limit_, capture_dir_.c_str());
            // LOCAL DIAGNOSTIC: LEONIDA_CAPTURE_SOURCE=1 also reads back the presented source texture itself.
            if (const char* source = std::getenv("LEONIDA_CAPTURE_SOURCE"); source && *source && *source != '0') {
                probe_source_ = true;
                std::fprintf(stderr, "[rt] present-probe: source capture on\n");
            }
        }
        ok_ = recreate(true, false);
        if (!ok_ && capture_to_disk_) {
            // The surface refused a TRANSFER_SRC swapchain, or the staging buffer could not be made: present
            // without the capture rather than not at all.
            std::fprintf(stderr, "[rt] present: capture is unavailable on this surface; presenting without it\n");
            capture_to_disk_ = false;
            capture_ = false;
            capture_dir_.clear();
            ok_ = recreate(true, false);
        }
    }

    ~VulkanPresenter() {
        context_.holds().cancel_after(this);  // presents still queued behind held work are dropped
        auto& vk = context_.vk;
        const VkDevice device = context_.device();
        {
            std::lock_guard lock(context_.submit_mutex());
            const SubmitMutex::SubmitTurn turn(context_.submit_mutex(), false);  // a queue operation: the compile gate
            context_.observe_result(vk.vkQueueWaitIdle(queue_), "vkQueueWaitIdle (present)");
            // A WSI layer may perform a device-wide idle while destroying the swapchain.
            // Keep that implicit idle serialized with submits from every context queue.
            release(swapchain_);
        }
        swapchain_ = VK_NULL_HANDLE;
        // A capture still pending has its copy complete once the queue is idle: write it before the buffer goes.
        finish_pending_capture();
        release_capture();
        composer_.reset();
        for (Frame& frame : frames_) {
            drop_views(frame);
            if (frame.done) vkDestroyFence_(device, frame.done, nullptr);
            if (frame.acquired) vk.vkDestroySemaphore(device, frame.acquired, nullptr);
        }
        if (probe_fence_) vkDestroyFence_(device, probe_fence_, nullptr);  // LOCAL DIAGNOSTIC
        if (pool_) vk.vkDestroyCommandPool(device, pool_, nullptr);
        if (surface_ && vkDestroySurfaceKHR_) vkDestroySurfaceKHR_(instance_, surface_, nullptr);
    }

    VulkanPresenter(const VulkanPresenter&) = delete;
    VulkanPresenter& operator=(const VulkanPresenter&) = delete;

    bool ok() const { return ok_; }
    VkExtent2D extent() const {
        const std::uint64_t packed = extent_packed_;
        return {std::uint32_t(packed >> 32), std::uint32_t(packed)};
    }
    VkFormat format() const { return format_; }
    std::uint64_t presented() const { return presented_; }
    // The present mode a tearing present uses (IMMEDIATE, else FIFO_RELAXED) while the current swapchain was created
    // for it, else VK_PRESENT_MODE_MAX_ENUM_KHR: ImmediateThresholdPercent cannot be honoured then.
    VkPresentModeKHR tear_mode() const { return tear_mode_; }
    bool tear_supported() const { return tear_mode_ != VK_PRESENT_MODE_MAX_ENUM_KHR; }
    // Whether the last present was queued in the tearing mode, and how many were.
    // A present queued behind held work (see submit()) is reported as not tearing.
    bool last_present_tore() const { return last_tore_; }
    std::uint64_t tear_presents() const { return tear_presents_; }

    // Blits the colour texture `source` (mip 0, layer 0, in D3D12's PRESENT state) to the window and presents it.
    // `source_rect`, when given, limits the blit to that part of the texture (stretched to the whole window); it
    // must be non-empty and inside the texture (VK_ERROR_UNKNOWN otherwise). Integer, block-compressed and
    // multisampled sources are refused (VK_ERROR_FORMAT_NOT_SUPPORTED); the other formats are converted by the blit.
    // The blit filters bilinearly, so with a rectangle the outermost texels blend with their neighbours outside it.
    // vsync: FIFO; otherwise MAILBOX or IMMEDIATE when the surface has them. VK_SUCCESS also covers a skipped
    // present (minimised window) and one the surface reported out of date (the swapchain is recreated next time);
    // VK_ERROR_OUT_OF_DATE_KHR means recreating the swapchain failed, and the next present tries again.
    //
    // A failure after an image was acquired (the blit's submission) is recovered here, so the next present does not
    // wait on a fence nothing will signal: an empty batch takes the acquire semaphore and signals the frame's fence,
    // and the swapchain is recreated next time, which gives back the image that was never presented. If even that
    // batch fails, the presenter stops (ok() is false) and its owner has to make a new one.
    //
    // `tear` (only with vsync): queue the present in tear_mode() instead of FIFO when the swapchain can, so a frame
    // that missed its vblank is shown at once; last_present_tore() says whether it was.
    VkResult present(ResourceId source, bool vsync, const PresentRect* source_rect = nullptr, bool tear = false) {
        ResourceBinding binding;
        {
            std::shared_lock lock(context_.resource_mutex_);  // the resource table (vulkan_backend.h)
            const ResourceBinding* found = context_.resolve(source);
            if (!found || !found->image || found->aspects != VK_IMAGE_ASPECT_COLOR_BIT) return VK_ERROR_UNKNOWN;
            binding = *found;
        }
        // Multisampled sources (no resolve into a temporary yet), integer formats (a UNORM swapchain cannot take them)
        // and anything the GPU cannot blit from are refused.
        if (binding.samples > 1 || detail::blit_unsupported_format(binding.format) ||
            !(format_features(binding.format) & VK_FORMAT_FEATURE_BLIT_SRC_BIT))
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
        if (source_rect && (source_rect->left < 0 || source_rect->top < 0 || source_rect->left >= source_rect->right ||
                            source_rect->top >= source_rect->bottom ||
                            std::uint32_t(source_rect->right) > binding.width ||
                            std::uint32_t(source_rect->bottom) > binding.height))
            return VK_ERROR_UNKNOWN;
        return submit(&binding, source, vsync, source_rect, detail::is_srgb_format(binding.format), tear);
    }

    // PresentX with planes (docs section 4.5): one or two textures, each scaled from its source rectangle to its
    // destination rectangle of the window, plane 1 blended over plane 0, composed by a compute pass into an RGBA16F image
    // that is then copied to the swapchain image. Colour spaces are converted to the swapchain's: when a plane is HDR10 and
    // the surface lists an HDR10 (ST.2084) format the swapchain uses it (hdr_active()), else the HDR10 planes are
    // tone mapped to SDR (shoulder above 0.8 of SDR white, then clipped). Planes are filtered and blended in linear light. The swapchain
    // is always UNORM here; sRGB-format sources are
    // encoded again by the pass. Sources must be single-sample, sampled (no DENY_SHADER_RESOURCE) float/UNORM/sRGB
    // colour textures; others are refused (VK_ERROR_FORMAT_NOT_SUPPORTED). Rectangles outside their range give
    // VK_ERROR_UNKNOWN. vsync and `tear` as for present().
    VkResult present_planes(const PresentPlanes& planes, bool vsync, bool tear = false) {
        if (planes.count < 1 || planes.count > 2) return VK_ERROR_UNKNOWN;
        if (!(planes.sdr_white_nits > 0.0f) || !std::isfinite(planes.sdr_white_nits)) return VK_ERROR_UNKNOWN;
        if (!(planes.hdr_peak_nits > 0.0f) || !std::isfinite(planes.hdr_peak_nits)) return VK_ERROR_UNKNOWN;
        for (std::uint32_t i = 0; i < planes.count; ++i) {
            const PresentPlane& plane = planes.plane[i];
            ResourceBinding binding;
            {
                std::shared_lock lock(context_.resource_mutex_);
                const ResourceBinding* found = context_.resolve(plane.source);
                if (!found || !found->image) return VK_ERROR_UNKNOWN;
                binding = *found;
            }
            if (binding.aspects != VK_IMAGE_ASPECT_COLOR_BIT || binding.samples > 1 || !binding.sampled || detail::blit_unsupported_format(binding.format) ||
                !(format_features(binding.format) & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
                return VK_ERROR_FORMAT_NOT_SUPPORTED;
            const PresentRect& r = plane.source_rect;
            if (plane.has_source_rect && (r.left < 0 || r.top < 0 || r.left >= r.right || r.top >= r.bottom ||
                                          std::uint32_t(r.right) > binding.width || std::uint32_t(r.bottom) > binding.height))
                return VK_ERROR_UNKNOWN;
            const float* d = plane.dest;
            if (!(d[0] >= 0.0f && d[0] < d[2] && d[2] <= 1.0f && d[1] >= 0.0f && d[1] < d[3] && d[3] <= 1.0f))
                return VK_ERROR_UNKNOWN;  // (also false for NaN)
        }
        return submit(nullptr, ResourceId{}, vsync, nullptr, false, tear, &planes);
    }
    // The swapchain holds HDR10 (ST.2084) values: a plane asked for it and the surface has the format.
    bool hdr_active() const { return hdr_active_; }
    // Presents that went through the compose pass (present_planes); plain blits and blank frames are not counted.
    std::uint64_t composed_presents() const { return composed_; }
    // Tests, on surfaces without HDR10: compose as if the output were HDR10 but write the PQ values into the (8-bit,
    // UNORM) swapchain, so the encoder can be checked on any GPU. Takes effect with the next present_planes().
    void force_hdr_encoding(bool on) { force_hdr_encoding_ = on; }

    // Polled while a present waits for a swapchain image (every 50 ms): when it returns true the present gives up with
    // VK_NOT_READY, so a caller that is being suspended is never held by a window that stopped taking frames.
    void set_abort(std::function<bool()> abort) {
        std::lock_guard lock(abort_mutex_);
        abort_ = std::move(abort);
    }

    // Presents a black frame: the swapchain image cleared, nothing read. PresentX with no planes.
    VkResult present_blank(bool vsync, bool tear = false) {
        return submit(nullptr, ResourceId{}, vsync, nullptr, swapchain_srgb_, tear);
    }

    // From now on every presented frame is also copied to host memory (the swapchain gets TRANSFER_SRC usage), for
    // read_back(). False when the surface cannot do it or its format is not 8 bits per channel with 4 channels.
    // Call before the first present.
    bool enable_capture() {
        std::lock_guard lock(present_mutex_);
        splash_capture_only_ = false;
        continuous_readback_ = enable_capture_storage();
        return continuous_readback_;
    }

    // Present the shell image immediately. Keep it on screen until the window has a non-black title frame.
    bool present_splash(ResourceId source, std::shared_ptr<void> owner);

    // The last presented frame as the swapchain stores it (4 bytes a texel, rows tightly packed, format() order).
    // Waits for the frame to finish. False when nothing was presented since enable_capture(), after a present
    // that was skipped because the window is minimised (it returned VK_SUCCESS but showed nothing), until the next
    // frame that is shown, on an unsampled disk-only present, or when the wait fails.
    bool read_back(std::vector<std::uint8_t>& pixels, VkExtent2D* extent, VkFormat* format) {
        std::lock_guard lock(present_mutex_);
        if (!capture_ || !capture_ptr_ || !captured_) return false;
        const Frame& frame = frames_[(frame_index_ + frames_.size() - 1) % frames_.size()];
        if (context_.observe_result(vkWaitForFences_(context_.device(), 1, &frame.done, VK_TRUE, UINT64_MAX),
                                    "vkWaitForFences (present readback)") != VK_SUCCESS)
            return false;
        pixels.assign(capture_ptr_, capture_ptr_ + std::size_t(captured_extent_.width) * captured_extent_.height * 4);
        if (extent) *extent = captured_extent_;
        if (format) *format = captured_format_;
        return true;
    }

    // Shared by present(), present_blank() and present_planes().
    //
    // A Present is ordered behind the work the queues hold (a batch that waits for a Signal not yet submitted,
    // queue_holds.h): it would otherwise blit whatever the source texture held before that batch ran. It is not
    // blocked: it is queued, returns VK_SUCCESS as ExecuteCommandLists does for a held batch, and is submitted,
    // in order with the presents before it, by the thread that releases the last operation held when it was called
    // (its failure is printed there). Held work that is never released holds back the presents after it, as it
    // holds back the queue's own work; with nothing held this is the plain path. A blank present reads nothing and
    // is only ordered behind earlier presents. Waiting instead was rejected: the Signal that releases the work may
    // be one the application's own thread makes after Present returns. Like a full frame-latency queue, a Present that
    // finds max_deferred_presents queued ahead of it does wait, for held work to release (logged once; set_abort ends
    // the wait), so the queue behind never-released work is bounded. A queued Present reports its tear as false
    // (last_present_tore()): the frame pacer's default sync.
    static constexpr std::size_t max_deferred_presents = 8;

    VkResult submit(const ResourceBinding* binding, ResourceId source, bool vsync, const PresentRect* source_rect,
                    bool srgb, bool tear = false, const PresentPlanes* planes = nullptr) {
        wait_for_room();
        std::lock_guard lock(present_mutex_);
        // `binding` null and `planes` null: clear instead of blit; `planes`: compose them (`binding`, `source`,
        // `source_rect` unused). Only a present that reads a texture waits for held work.
        if (deferred_.empty() && !((binding || planes) && context_.holds().any_held()))
            return submit_now(binding, source, vsync, source_rect, srgb, tear, planes);
        PendingPresent pending{binding != nullptr, source, vsync, source_rect != nullptr, {}, srgb, tear,
                               planes != nullptr, {}};
        if (source_rect) pending.rect = *source_rect;
        if (planes) pending.planes = *planes;
        deferred_.push_back(pending);
        if (const std::uint64_t n = ++probe_deferred_; local_diagnostics() && (n <= 8 || n % 600 == 0))
            std::fprintf(stderr, "[rt] present-probe: present #%llu queued behind held work (%zu queued)\n",
                         static_cast<unsigned long long>(n), deferred_.size());
        last_tore_ = false;
        context_.holds().after_held(this, [this] { run_deferred(); });
        return VK_SUCCESS;
    }

private:
    // Called with present_mutex_ held. Reserving staging for the splash or rare disk samples
    // is distinct from promising the caller a copy of every frame through read_back().
    bool enable_capture_storage() {
        if (!ok_) return false;
        capture_ = true;
        stale_ = true;
        if (!recreate(vsync_, swapchain_srgb_, swapchain_hdr_req_) || (swapchain_ && !capture_buffer_)) {
            capture_ = false;
            return false;
        }
        return true;
    }

    bool should_abort() {
        std::function<bool()> abort;
        {
            std::lock_guard lock(abort_mutex_);
            abort = abort_;
        }
        return abort && abort();
    }

    // Called without present_mutex_: the thread that releases the queued presents needs it.
    void wait_for_room() {
        bool logged = false;
        for (;;) {
            {
                std::lock_guard lock(present_mutex_);
                if (deferred_.size() < max_deferred_presents) return;
            }
            if (!logged) {
                logged = true;
                std::fprintf(stderr, "[rt] d3d12: %zu Presents are queued behind held work; this one waits for it\n",
                             max_deferred_presents);
            }
            if (should_abort()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    struct PendingPresent {
        bool has_source;
        ResourceId source;
        bool vsync;
        bool has_rect;
        PresentRect rect;
        bool srgb;
        bool tear;
        bool has_planes;
        PresentPlanes planes;
    };

    void run_deferred() {
        std::lock_guard lock(present_mutex_);
        if (deferred_.empty()) return;
        const PendingPresent p = deferred_.front();
        deferred_.pop_front();
        ResourceBinding binding{};
        const VkResult result = submit_now(p.has_source ? &binding : nullptr, p.source, p.vsync,
                                           p.has_rect ? &p.rect : nullptr, p.srgb, p.tear,
                                           p.has_planes ? &p.planes : nullptr);
        if (result != VK_SUCCESS)
            std::fprintf(stderr, "[rt] d3d12: a Present held behind queued work failed when it was released (%d)\n",
                         int(result));
    }

    VkResult submit_now(const ResourceBinding* binding, ResourceId source, bool vsync, const PresentRect* source_rect,
                        bool srgb, bool tear = false, const PresentPlanes* planes = nullptr) {
        last_tore_ = false;
        probe_path(binding, source, planes);  // LOCAL DIAGNOSTIC
        if (!ok_) return VK_ERROR_INITIALIZATION_FAILED;
        if (surface_lost_) {  // the window's display went away under a presented swapchain
            // Try to make a new surface for the same window (the hwnd is retained by the factory's callback).
            // Until that succeeds, vkd3d's precedent keeps rendering to nothing: the frame is not shown, but
            // the run continues instead of wedging on a sticky E_FAIL.
            surface_ = recreate_surface_fn_ ? recreate_surface_fn_() : VK_NULL_HANDLE;
            if (!surface_) {
                captured_ = false;  // nothing was shown: read_back() must not hand out the frame
                return VK_SUCCESS;
            }
            surface_lost_ = false;
            stale_ = true;  // the old swapchain is gone: rebuild on the new surface below
        }
        // The swapchain's HDR10 format is only for composed planes that ask for it; a blit of an SDR source or a blank
        // frame keeps whatever the swapchain has (black is black), except that a blit needs the SDR one.
        bool want_hdr = swapchain_hdr_req_;
        if (planes)
            want_hdr = (planes->plane[0].hdr10) || (planes->count > 1 && planes->plane[1].hdr10);
        else if (binding)
            want_hdr = false;
        if (vsync != vsync_ || stale_ || srgb != swapchain_srgb_ || want_hdr != swapchain_hdr_req_) {
            // The swapchain's colour encoding is only recorded once the new swapchain exists, so a failed
            // recreate is tried again at the next present.
            if (!recreate_for_present(vsync, srgb, want_hdr)) return VK_ERROR_OUT_OF_DATE_KHR;
        }
        if (!swapchain_) {  // minimised: try again next frame
            stale_ = true;
            captured_ = false;  // nothing was shown: read_back() must not hand out the frame before
            return VK_SUCCESS;
        }
        if (binding && !presentable(source)) return VK_ERROR_UNKNOWN;  // checked before an image is acquired for it
        if ((binding || planes) && !(format_features(format_) & VK_FORMAT_FEATURE_BLIT_DST_BIT))
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
        if (planes && !prepare_compose()) return VK_ERROR_FORMAT_NOT_SUPPORTED;
        const VkDevice device = context_.device();
        Frame& frame = frames_[frame_index_ % frames_.size()];
        if (const VkResult waited = context_.observe_result(
                vkWaitForFences_(device, 1, &frame.done, VK_TRUE, UINT64_MAX), "vkWaitForFences (present)");
            waited != VK_SUCCESS)
            return stop(waited);
        // Drain the preceding disk sample before reusing its fence or advancing its metadata.
        if (const VkResult r = finish_pending_capture(); r != VK_SUCCESS) return stop(r);

        std::uint32_t image = 0;
        VkResult acquired = acquire(frame.acquired, &image);
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
            if (!recreate_for_present(vsync, swapchain_srgb_, swapchain_hdr_req_)) return acquired;
            if (!swapchain_) {  // minimised since the last present: skipped, as above
                captured_ = false;
                return VK_SUCCESS;
            }
            acquired = acquire(frame.acquired, &image);
        }
        if (acquired == VK_ERROR_SURFACE_LOST_KHR) {
            captured_ = false;  // nothing was acquired, nothing was shown
            mark_surface_lost();
            return VK_SUCCESS;
        }
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) return acquired;  // nothing was acquired
        if (acquired == VK_SUBOPTIMAL_KHR) stale_ = true;
        // An out-of-date retry above may have given the swapchain another extent: the compose target follows it.
        if (planes && !prepare_compose()) return give_back(frame, VK_ERROR_FORMAT_NOT_SUPPORTED);

        // The ordinal counts accepted presents, not submissions or deferred requests. Decide after
        // draining the previous disk sample and any acquire/recreate retry; an unshown attempt consumes no slot.
        const bool disk_sample = capture_to_disk_ && capture_written_ < capture_limit_ &&
                                 presented_ % std::uint64_t(capture_every_) == 0;
        const bool inspect = splash_owner_ && capture_ptr_;
        // Merely allocating a temporary splash buffer does not require a copy. In particular, a minimized
        // initial splash can return before taking ownership; later ordinary frames must not become continuous.
        const bool copy_frame = capture_buffer_ && ((capture_ && (disk_sample || continuous_readback_ || inspect)) || dump_.due(presented_));
        captured_ = false;  // never expose a stale sample after an unsampled or unshown frame
        bool recorded = false;
        std::unique_lock tracker_lock(context_.tracker_mutex_, std::defer_lock);
        bool rollback_source = false;
        VkImageLayout prev_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        std::array<RollbackPlane, 2> plane_rollbacks{};  // present_planes(): the same, per plane
        if (planes) {
            std::array<ResourceBinding, 2> current{};
            bool found_all = true;
            {
                std::shared_lock resource_lock(context_.resource_mutex_);
                for (std::uint32_t i = 0; i < planes->count; ++i) {
                    const ResourceBinding* found = context_.resolve(planes->plane[i].source);
                    if (found && found->image && found->aspects == VK_IMAGE_ASPECT_COLOR_BIT && found->sampled)
                        current[i] = *found;
                    else
                        found_all = false;
                }
                if (found_all) {
                    tracker_lock.lock();
                    for (std::uint32_t i = 0; i < planes->count; ++i) {
                        plane_rollbacks[i].id = planes->plane[i].source;
                        plane_rollbacks[i].tracked = context_.tracker_.tracked(plane_rollbacks[i].id);
                        if (plane_rollbacks[i].tracked) plane_rollbacks[i].layout = context_.tracker_.layout(plane_rollbacks[i].id, 0);
                    }
                    recorded = record_planes(frame, std::uint32_t(frame_index_ % frames_.size()), current, *planes,
                                             image, copy_frame);
                }
            }
        } else if (binding) {
            // Lock hierarchy: resource_mutex_ -> tracker_mutex_ -> submit_mutex_ (vulkan_backend.h).
            // record() only touches subresource 0 (mip 0, layer 0, color) of `source`, so saving that one layout
            // rolls back a failed submit in O(1) without copying the entire LayoutTracker map.
            std::shared_lock resource_lock(context_.resource_mutex_);
            const ResourceBinding* current = context_.resolve(source);
            if (current && current->image && current->aspects == VK_IMAGE_ASPECT_COLOR_BIT) {
                tracker_lock.lock();
                rollback_source = context_.tracker_.tracked(source);
                if (rollback_source) prev_layout = context_.tracker_.layout(source, 0);
                record(frame.commands, *current, source, image, source_rect, copy_frame);
                recorded = true;
            }
        } else {
            record_blank(frame.commands, image, copy_frame);
            recorded = true;
        }
        const auto rollback = [&] {
            if (rollback_source)
                context_.tracker_.set(source, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, prev_layout);
            if (planes)
                for (std::uint32_t i = 0; i < planes->count; ++i)
                    if (plane_rollbacks[i].tracked)
                        context_.tracker_.set(plane_rollbacks[i].id, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                                              plane_rollbacks[i].layout);
        };
        if (!recorded) return give_back(frame, VK_ERROR_UNKNOWN);  // released by another thread since presentable()
        if (const VkResult reset = context_.observe_result(vkResetFences_(device, 1, &frame.done),
                                                          "vkResetFences (present)"); reset != VK_SUCCESS) {
            rollback();
            return stop(reset);
        }
        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit_info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit_info.waitSemaphoreCount = 1;
        submit_info.pWaitSemaphores = &frame.acquired;
        submit_info.pWaitDstStageMask = &wait_stage;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &frame.commands;
        submit_info.signalSemaphoreCount = 1;
        submit_info.pSignalSemaphores = &ready_[image];  // one per image: reused only once that image comes back
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &ready_[image];
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain_;
        present.pImageIndices = &image;
#ifdef LEONIDA_SWAPCHAIN_MAINTENANCE1
        // The swapchain was created with FIFO and tear_mode_ (both come from the same recreate).
        VkSwapchainPresentModeInfoEXT present_mode{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT};
        const bool tearing = tear && vsync_ && tear_supported();
        if (tearing) {
            present_mode.swapchainCount = 1;
            present_mode.pPresentModes = &tear_mode_;
            present.pNext = &present_mode;
        }
#else
        const bool tearing = false;
        (void)tear;
#endif
        bool show_title = false;
        if (inspect) {
            submit_info.signalSemaphoreCount = 0;  // signal ready only after choosing the image the window will show
            if (!tracker_lock.owns_lock()) tracker_lock.lock();
        }
        VkResult submitted, result = VK_SUCCESS;
        {
            std::lock_guard lock(context_.submit_mutex());
            // One turn of the compile gate for the submit and the present after it: no pipeline create enters the
            // driver between the two (queue_submit's own turn nests inside this one). Not a probe: it also covers
            // finish_startup_frame's fence wait and the present; the nested turn probes the vkQueueSubmit alone.
            const SubmitMutex::SubmitTurn turn(context_.submit_mutex(), false);  // a queue operation: the compile gate
            submitted = context_.observe_result(context_.queue_submit(queue_, 1, &submit_info, frame.done),
                                                "vkQueueSubmit (present)");
            if (submitted == VK_SUCCESS) context_.note_image_work_locked();
            if (submitted != VK_SUCCESS) rollback();
            if (submitted == VK_SUCCESS && inspect) {
                result = finish_startup_frame(frame, image, show_title);
                if (result != VK_SUCCESS) return stop(result);
            }
            if (tracker_lock.owns_lock()) tracker_lock.unlock();
            if (submitted == VK_SUCCESS)
                result = context_.observe_result(vkQueuePresentKHR_(queue_, &present), "vkQueuePresentKHR");
        }
        if (show_title && (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)) {
            splash_owner_.reset();
            if (splash_capture_only_) {
                splash_capture_only_ = false;
                capture_ = false;
                release_capture();
            }
        }
        if (submitted != VK_SUCCESS) return give_back(frame, submitted);
        ++frame_index_;
        // Only a present the engine took counts (SUBOPTIMAL is shown too). OUT_OF_DATE was not shown: the swapchain
        // is recreated next time, and the caller sees VK_SUCCESS, since its work was submitted.
        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
            const auto shown = ++presented_;
            submit_phases::totals().presents.fetch_add(1, std::memory_order_relaxed);  // frames a second, phase line
            submit_phases::totals().present_serial.fetch_add(1, std::memory_order_relaxed);
            if (dump_.due(shown - 1)) {
                // Presentation is already queued and the queue/tracker locks are released. A sampled frame
                // may stall here for its transfer fence and a private CPU copy. PNG and disk I/O run on a worker.
                if (vkWaitForFences_(device, 1, &frame.done, VK_TRUE, UINT64_MAX) == VK_SUCCESS)
                    dump_.save(shown, extent_, format_, dump_pq_, capture_ptr_, size_t(capture_size_));
                else
                    dump_.disable("present dump fence wait failed");
            }
            if (planes) ++composed_;
            if (tearing) {
                last_tore_ = true;
                ++tear_presents_;
            }
            if (copy_frame && capture_ptr_) {  // a temporary splash-only allocation may have just been released
                captured_ = true;
                captured_extent_ = extent_;
                captured_format_ = format_;
                if (disk_sample) pending_capture_ = frame.done;
            }
        }
        if (result == VK_ERROR_SURFACE_LOST_KHR) {
            // The surface was lost during the present (display disconnected, a driver reset of the window's
            // plane): vkd3d keeps rendering to nothing, and the next present recreates surface and swapchain.
            captured_ = false;
            mark_surface_lost();
            result = VK_SUCCESS;
        }
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            stale_ = true;
            result = VK_SUCCESS;
        }
        return result;
    }

private:
    // ---- LOCAL DIAGNOSTIC (not for upstream): which path each present takes, and the source texture's contents ----
    bool probe_source_ = false;
    std::uint64_t probe_calls_ = 0, probe_blit_ = 0, probe_compose_ = 0, probe_blank_ = 0, probe_unresolved_ = 0;
    std::atomic<std::uint64_t> probe_deferred_{0};
    int probe_written_ = 0;
    VkCommandBuffer probe_commands_ = VK_NULL_HANDLE;
    VkFence probe_fence_ = VK_NULL_HANDLE;

    static float probe_half(std::uint16_t h) {
        const std::uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 0x1f, mantissa = h & 0x3ff;
        float v;
        if (exponent == 0) v = std::ldexp(float(mantissa), -24);
        else if (exponent == 31) v = mantissa ? 0.0f : 65504.0f;  // NaN reads as 0, inf as the largest half
        else v = std::ldexp(float(mantissa | 0x400), int(exponent) - 25);
        return sign ? -v : v;
    }
    static float probe_uf11(std::uint32_t bits, int mantissa_bits) {  // B10G11R11 channels (5-bit exponent)
        const std::uint32_t exponent = (bits >> mantissa_bits) & 0x1f, mantissa = bits & ((1u << mantissa_bits) - 1);
        if (exponent == 0) return std::ldexp(float(mantissa), -14 - mantissa_bits);
        if (exponent == 31) return 65504.0f;
        return std::ldexp(float(mantissa | (1u << mantissa_bits)), int(exponent) - 15 - mantissa_bits);
    }
    // RGB in 0..1 (float formats unclamped) of one texel; false for a format this probe does not decode.
    static bool probe_decode(VkFormat format, const std::uint8_t* t, float rgb[3]) {
        switch (format) {
        case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
            rgb[0] = t[2] / 255.0f; rgb[1] = t[1] / 255.0f; rgb[2] = t[0] / 255.0f; return true;
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB:
            rgb[0] = t[0] / 255.0f; rgb[1] = t[1] / 255.0f; rgb[2] = t[2] / 255.0f; return true;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2R10G10B10_UNORM_PACK32: {
            std::uint32_t w; std::memcpy(&w, t, 4);
            const float lo = (w & 0x3ff) / 1023.0f, mid = ((w >> 10) & 0x3ff) / 1023.0f, hi = ((w >> 20) & 0x3ff) / 1023.0f;
            const bool bgra = format == VK_FORMAT_A2R10G10B10_UNORM_PACK32;
            rgb[0] = bgra ? hi : lo; rgb[1] = mid; rgb[2] = bgra ? lo : hi; return true;
        }
        case VK_FORMAT_R16G16B16A16_SFLOAT: {
            std::uint16_t h[3]; std::memcpy(h, t, 6);
            for (int i = 0; i < 3; ++i) rgb[i] = probe_half(h[i]);
            return true;
        }
        case VK_FORMAT_R16G16B16A16_UNORM: {
            std::uint16_t h[3]; std::memcpy(h, t, 6);
            for (int i = 0; i < 3; ++i) rgb[i] = h[i] / 65535.0f;
            return true;
        }
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32: {
            std::uint32_t w; std::memcpy(&w, t, 4);
            rgb[0] = probe_uf11(w & 0x7ff, 6); rgb[1] = probe_uf11((w >> 11) & 0x7ff, 6); rgb[2] = probe_uf11(w >> 22, 5);
            return true;
        }
        case VK_FORMAT_R32G32B32A32_SFLOAT:
            std::memcpy(rgb, t, 12); return true;
        default:
            return false;
        }
    }

    // Called at the top of submit_now(), so after any held work it was queued behind.
    void probe_path(const ResourceBinding* binding, ResourceId source, const PresentPlanes* planes) {
        const std::uint64_t n = ++probe_calls_;
        const char* path = planes ? "present_planes(compose)" : binding ? "present(blit)" : "present_blank";
        (planes ? probe_compose_ : binding ? probe_blit_ : probe_blank_)++;
        const ResourceId id = planes ? planes->plane[0].source : binding ? source : ResourceId{};
        ResourceBinding found{};
        bool resolved = false;
        if (id) {
            std::shared_lock lock(context_.resource_mutex_);
            if (const ResourceBinding* b = context_.resolve(id)) {
                found = *b;
                resolved = true;
            }
        }
        if (id && !resolved) ++probe_unresolved_;
        if (local_diagnostics() && (n <= 6 || n % 600 == 0))
            std::fprintf(stderr,
                         "[rt] present-probe: submit #%llu path %s source 0x%llx resolved %d image %p format %d %ux%u "
                         "mips %u layers %u samples %u sampled %d (blit %llu compose %llu blank %llu unresolved %llu)\n",
                         static_cast<unsigned long long>(n), path, static_cast<unsigned long long>(id), int(resolved),
                         reinterpret_cast<void*>(found.image), int(found.format), found.width, found.height,
                         found.mip_levels, found.array_layers, found.samples, int(found.sampled),
                         static_cast<unsigned long long>(probe_blit_), static_cast<unsigned long long>(probe_compose_),
                         static_cast<unsigned long long>(probe_blank_), static_cast<unsigned long long>(probe_unresolved_));
        // The same frames the swapchain capture takes: the one presented_ becomes when this present is shown.
        if (probe_source_ && resolved && found.image && probe_written_ < capture_limit_ &&
            presented_ % std::uint64_t(capture_every_) == 0)
        {
            probe_read_source(id, presented_ + 1, path);
            // LOCAL DIAGNOSTIC (fsdraw): the render targets of the frame's fullscreen draws, read back with it.
            for (const std::uint64_t target : fsdraw::targets())
                if (target != id) probe_read_source(target, presented_ + 1, "fullscreen-draw target", "target");
        }
    }

    // Copies mip 0, layer 0 of `id` to host memory on the presenter's queue (waited for), logs and writes it.
    void probe_read_source(ResourceId id, std::uint64_t number, const char* path, const char* prefix = "source") {
        const bool is_source = std::strcmp(prefix, "source") == 0;
        auto& vk = context_.vk;
        const VkDevice device = context_.device();
        const ReplayFunctions& fn = context_.replay_;
        if (!probe_commands_) {
            VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            allocate.commandPool = pool_;
            allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate.commandBufferCount = 1;
            VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            if (vk.vkAllocateCommandBuffers(device, &allocate, &probe_commands_) != VK_SUCCESS ||
                vkCreateFence_(device, &fence, nullptr, &probe_fence_) != VK_SUCCESS) {
                std::fprintf(stderr, "[rt] present-probe: cannot make the readback command buffer\n");
                probe_source_ = false;
                return;
            }
        }
        std::shared_lock resource_lock(context_.resource_mutex_);
        const ResourceBinding* found = context_.resolve(id);
        if (!found || !found->image) return;
        const ResourceBinding binding = *found;
        const BlockFormat bf = block_format(binding.format, VK_IMAGE_ASPECT_COLOR_BIT);
        // Held only while the copy is recorded (fsdraw: holding them through the wait, the slow host reads and the
        // file write stalled every resource creation and the boot ran out of memory).
        std::unique_lock tracker_lock(context_.tracker_mutex_);
        const bool tracked = context_.tracker_.tracked(id);
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        const VkImageLayout common = image_layout(binding, VK_IMAGE_LAYOUT_GENERAL);
        LayoutPieces pieces = detail::current(&context_.tracker_, id, range, common);
        const VkImageLayout layout = pieces.empty() ? VK_IMAGE_LAYOUT_UNDEFINED : pieces[0].layout;
        std::fprintf(stderr,
                     "[rt] present-probe: %s frame %llu via %s: id 0x%llx format %d %ux%u mips %u layers %u samples %u "
                     "texel bytes %u tracked %d layout %d\n",
                     prefix, static_cast<unsigned long long>(number), path, static_cast<unsigned long long>(id),
                     int(binding.format), binding.width, binding.height, binding.mip_levels, binding.array_layers,
                     binding.samples, bf.bytes, int(tracked), int(layout));
        if (layout == VK_IMAGE_LAYOUT_UNDEFINED) {
            std::fprintf(stderr, "[rt] present-probe: %s frame %llu is UNDEFINED in the tracker: no tracked GPU "
                                 "operation ever wrote it (the present zero-fills it)\n",
                         prefix, static_cast<unsigned long long>(number));
            if (is_source) ++probe_written_;
            return;
        }
        if (binding.samples > 1 || bf.block != 1 || !bf.bytes) {
            std::fprintf(stderr, "[rt] present-probe: %s format %d not read back\n", prefix, int(binding.format));
            return;
        }
        const VkDeviceSize size = VkDeviceSize(binding.width) * binding.height * bf.bytes;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vk.vkCreateBuffer(device, &info, nullptr, &buffer) != VK_SUCCESS) return;
        VkMemoryRequirements requirements{};
        vk.vkGetBufferMemoryRequirements(device, buffer, &requirements);
        VkPhysicalDeviceMemoryProperties props{};
        vkGetPhysicalDeviceMemoryProperties_(physical_, &props);
        constexpr VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        std::uint32_t type = UINT32_MAX;
        // Host-cached system memory first (fsdraw): a resizable-BAR type is uncached for host reads.
        for (int pass = 0; pass < 2 && type == UINT32_MAX; ++pass)
            for (std::uint32_t i = 0; i < props.memoryTypeCount; ++i) {
                const VkMemoryPropertyFlags f = props.memoryTypes[i].propertyFlags;
                const bool cached = (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) && !(f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                if ((requirements.memoryTypeBits & (1u << i)) && (f & wanted) == wanted && (pass || cached)) {
                    type = i;
                    break;
                }
            }
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = type;
        const auto cleanup = [&] {
            if (memory) vk.vkFreeMemory(device, memory, nullptr);
            if (buffer) vk.vkDestroyBuffer(device, buffer, nullptr);
        };
        if (type == UINT32_MAX || vk.vkAllocateMemory(device, &allocate, nullptr, &memory) != VK_SUCCESS ||
            vk.vkBindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS ||
            vk.vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
            cleanup();
            return;
        }
        VkCommandBuffer commands = probe_commands_;
        vk.vkResetCommandBuffer(commands, 0);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk.vkBeginCommandBuffer(commands, &begin);
        constexpr VkAccessFlags any = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        for (const auto& piece : pieces)
            detail::image_barrier(fn, commands, binding, piece.range, piece.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  any, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {binding.width, binding.height, 1};
        fn.copy_image_to_buffer(commands, binding.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
        for (const auto& piece : pieces)
            detail::image_barrier(fn, commands, binding, piece.range, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, piece.layout,
                                  VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, any,
                                  VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        VkBufferMemoryBarrier to_host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        to_host.srcQueueFamilyIndex = to_host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_host.buffer = buffer;
        to_host.size = VK_WHOLE_SIZE;
        fn.pipeline_barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                            &to_host, 0, nullptr);
        vk.vkEndCommandBuffer(commands);
        tracker_lock.unlock();
        resource_lock.unlock();
        vkResetFences_(device, 1, &probe_fence_);
        VkSubmitInfo submit_info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &commands;
        VkResult submitted;
        {
            std::lock_guard lock(context_.submit_mutex());
            submitted = context_.observe_result(context_.queue_submit(queue_, 1, &submit_info, probe_fence_),
                                                "vkQueueSubmit (present probe)");
            if (submitted == VK_SUCCESS) context_.note_image_work_locked();
        }
        if (submitted != VK_SUCCESS ||
            context_.observe_result(vkWaitForFences_(device, 1, &probe_fence_, VK_TRUE, 5'000'000'000ull),
                                    "vkWaitForFences (present probe)") != VK_SUCCESS) {
            std::fprintf(stderr, "[rt] present-probe: readback submit/wait failed (%d)\n", int(submitted));
            cleanup();
            return;
        }
        const std::size_t texels = std::size_t(binding.width) * binding.height;
        const auto* bytes = static_cast<const std::uint8_t*>(mapped);
        std::vector<std::uint8_t> rgb(texels * 3);
        double sum[3] = {0, 0, 0};
        float peak = 0.0f;
        std::size_t nonzero = 0;
        bool decoded = true;
        for (std::size_t i = 0; i < texels && decoded; ++i) {
            float c[3];
            if (!probe_decode(binding.format, bytes + i * bf.bytes, c)) {
                decoded = false;
                break;
            }
            bool any_set = false;
            for (int k = 0; k < 3; ++k) {
                sum[k] += c[k];
                peak = std::max(peak, c[k]);
                if (c[k] != 0.0f) any_set = true;
                rgb[i * 3 + k] = std::uint8_t(std::clamp(c[k], 0.0f, 1.0f) * 255.0f + 0.5f);
            }
            nonzero += any_set;
        }
        // Raw bytes too: a format the probe cannot decode still says whether anything was written.
        std::size_t raw_nonzero = 0;
        std::size_t first_nonzero = SIZE_MAX;
        for (std::size_t i = 0; i < std::size_t(size); ++i)
            if (bytes[i]) {
                if (first_nonzero == SIZE_MAX) first_nonzero = i;
                ++raw_nonzero;
            }
        {  // LOCAL DIAGNOSTIC (fsdraw): raw texels at the centre and at the first non-zero byte
            const std::size_t centre = (std::size_t(binding.height / 2) * binding.width + binding.width / 2) * bf.bytes;
            std::string c, f;
            char b[8];
            for (std::uint32_t k = 0; k < bf.bytes && k < 16; ++k) {
                std::snprintf(b, sizeof b, "%02x", bytes[centre + k]);
                c += b;
            }
            if (first_nonzero != SIZE_MAX) {
                const std::size_t t = first_nonzero / bf.bytes * bf.bytes;
                for (std::uint32_t k = 0; k < bf.bytes && k < 16; ++k) {
                    std::snprintf(b, sizeof b, "%02x", bytes[t + k]);
                    f += b;
                }
            }
            std::fprintf(stderr, "[rt] present-probe: %s frame %llu raw centre texel %s first non-zero texel %lld (%s)\n",
                         prefix, static_cast<unsigned long long>(number), c.c_str(),
                         first_nonzero == SIZE_MAX ? -1ll : static_cast<long long>(first_nonzero / bf.bytes), f.c_str());
        }
        cleanup();
        const double scale = 255.0 / double(texels);
        std::fprintf(stderr,
                     "[rt] present-probe: %s frame %llu mean rgb %.2f %.2f %.2f (0..255 scale) peak %.3f nonzero "
                     "texels %zu of %zu, nonzero bytes %zu of %llu%s\n",
                     prefix, static_cast<unsigned long long>(number), sum[0] * scale, sum[1] * scale, sum[2] * scale, peak,
                     nonzero, texels, raw_nonzero, static_cast<unsigned long long>(size),
                     decoded ? "" : " (format not decoded)");
        std::string dir = capture_dir_;
        if (dir.back() != '/' && dir.back() != '\\') dir += '/';
        if (decoded && (is_source || nonzero)) {
            char name[64];
            std::snprintf(name, sizeof name, "%s_%05llu.ppm", prefix, static_cast<unsigned long long>(number));
            std::ofstream file(dir + name, std::ios::binary | std::ios::trunc);
            if (file) {
                file << "P6\n" << binding.width << ' ' << binding.height << "\n255\n";
                file.write(reinterpret_cast<const char*>(rgb.data()), std::streamsize(rgb.size()));
            }
        }
        std::ofstream index(dir + "source_index.txt", std::ios::app);
        if (index)
            index << prefix << " frame " << number << " id 0x" << std::hex << id << std::dec << " path " << path << " format " << int(binding.format) << " extent "
                  << binding.width << 'x' << binding.height << " layout " << int(layout) << " mean rgb " << std::fixed
                  << std::setprecision(2) << sum[0] * scale << ' ' << sum[1] * scale << ' ' << sum[2] * scale
                  << " nonzero texels " << nonzero << " nonzero bytes " << raw_nonzero << '\n';
        if (is_source) ++probe_written_;
    }
    // ---- end LOCAL DIAGNOSTIC ----

    struct Frame {
        VkCommandBuffer commands = VK_NULL_HANDLE;
        VkFence done = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        std::array<VkImageView, 2> views{};  // the plane views of the last composed present, until the frame is reused
    };

    bool recreate_for_present(bool vsync, bool srgb, bool hdr);
    VkResult finish_startup_frame(Frame& frame, std::uint32_t image, bool& show_title);

    // Whether `source` is a colour texture the device knows, so present() can refuse it before acquiring an image.
    bool presentable(ResourceId source) {
        std::shared_lock lock(context_.resource_mutex_);  // the resource table (vulkan_backend.h)
        const ResourceBinding* binding = context_.resolve(source);
        return binding && binding->image && binding->aspects == VK_IMAGE_ASPECT_COLOR_BIT;
    }

    // An image was acquired into `frame` but its blit was not submitted. An empty batch waits on the acquire
    // semaphore and signals the frame's fence, so both are usable again; the swapchain is recreated on the next
    // present, which gives back the image that was never presented. Returns `failure`, the error to report.
    VkResult give_back(Frame& frame, VkResult failure) {
        const VkDevice device = context_.device();
        if (context_.observe_result(vkResetFences_(device, 1, &frame.done), "vkResetFences (present)") != VK_SUCCESS)
            return stop(failure);
        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo submit_info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit_info.waitSemaphoreCount = 1;
        submit_info.pWaitSemaphores = &frame.acquired;
        submit_info.pWaitDstStageMask = &wait_stage;
        VkResult submitted;
        {
            std::lock_guard lock(context_.submit_mutex());
            submitted = context_.observe_result(context_.queue_submit(queue_, 1, &submit_info, frame.done),
                                                "vkQueueSubmit (return present image)");
        }
        if (submitted != VK_SUCCESS) return stop(failure);
        stale_ = true;
        return failure;
    }

    // The swapchain image as a binding, for the barrier helpers.
    ResourceBinding target_of(std::uint32_t image) const {
        ResourceBinding target{};
        target.image = images_[image];
        target.format = format_;
        target.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
        return target;
    }

    // The frame's fence or acquire semaphore can no longer be trusted: no more presents (ok() is false), and the
    // owner makes a new presenter. Returns `failure`.
    VkResult stop(VkResult failure) {
        ok_ = false;
        return failure;
    }

    // The window's surface is dead (VK_ERROR_SURFACE_LOST_KHR): the swapchain that presented into it is retired
    // and the surface destroyed. The next submit_now() first tries the factory's recreate-surface callback; until
    // it succeeds the presenter keeps the vkd3d "render to nothing" behaviour, not a sticky failure.
    void mark_surface_lost() {
        auto& vk = context_.vk;
        const VkDevice device = context_.device();
        {
            std::lock_guard lock(context_.submit_mutex());
            const SubmitMutex::SubmitTurn turn(context_.submit_mutex(), false);  // a queue operation: the compile gate
            // The old swapchain's images and semaphores may still be in use.
            context_.observe_result(vk.vkQueueWaitIdle(queue_), "vkQueueWaitIdle (present)");
            release(swapchain_);
        }
        swapchain_ = VK_NULL_HANDLE;
        if (surface_ && vkDestroySurfaceKHR_) vkDestroySurfaceKHR_(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
        surface_lost_ = true;
        stale_ = true;
        captured_ = false;
    }

    void record(VkCommandBuffer commands, const ResourceBinding& binding, ResourceId source, std::uint32_t image,
                const PresentRect* source_rect, bool copy_frame) {
        auto& vk = context_.vk;
        const ReplayFunctions& fn = context_.replay_;
        vk.vkResetCommandBuffer(commands, 0);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk.vkBeginCommandBuffer(commands, &begin);

        constexpr VkAccessFlags any = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        // PRESENT is D3D12's common state: GENERAL for an ordinary texture. A never-written back buffer is
        // zero-filled first, as any committed resource is.
        const VkImageLayout common = image_layout(binding, VK_IMAGE_LAYOUT_GENERAL);
        auto pieces = detail::current(&context_.tracker_, source, range, common);
        detail::initialize(fn, commands, binding, source, pieces, common, &context_.tracker_);
        for (const auto& piece : pieces)
            detail::image_barrier(fn, commands, binding, piece.range, piece.layout,
                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT,
                                  VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT);
        ResourceBinding target{};  // the swapchain image, for the barrier helpers
        target.image = images_[image];
        target.format = format_;
        target.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
        // The acquire semaphore is waited at the transfer stage, so the swapchain image's barrier starts there.
        detail::image_barrier(fn, commands, target, range, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        const PresentRect whole{0, 0, std::int32_t(binding.width), std::int32_t(binding.height)};
        const PresentRect& src = source_rect ? *source_rect : whole;
        const std::int32_t sw = src.right - src.left, sh = src.bottom - src.top;
        const std::int32_t dw = std::int32_t(extent_.width), dh = std::int32_t(extent_.height);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[0] = {src.left, src.top, 0};
        blit.srcOffsets[1] = {src.right, src.bottom, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[1] = {dw, dh, 1};
        vkCmdBlitImage_(commands, binding.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, images_[image],
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                        sw == dw && sh == dh ? VK_FILTER_NEAREST : blit_filter(binding.format));

        finish_image(commands, image, copy_frame);
        for (const auto& piece : pieces)
            detail::image_barrier(fn, commands, binding, piece.range, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  piece.layout, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, any,
                                  VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        vk.vkEndCommandBuffer(commands);
    }

    struct RollbackPlane {
        ResourceId id{};
        bool tracked = false;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };

    void drop_views(Frame& frame) {
        for (VkImageView& view : frame.views)
            if (view) context_.vk.vkDestroyImageView(context_.device(), view, nullptr);
        frame.views = {};
    }

    // The compose pass and its target image, for the swapchain's current size.
    bool prepare_compose() {
        if (!composer_) {
            VkPhysicalDeviceMemoryProperties memory{};
            if (!vkGetPhysicalDeviceMemoryProperties_) return false;
            vkGetPhysicalDeviceMemoryProperties_(physical_, &memory);
            composer_ = std::make_unique<PresentComposer>(context_, memory);
        }
        if (!composer_->ready()) return false;
        if (composer_->target() && composer_->extent().width == extent_.width &&
            composer_->extent().height == extent_.height)
            return true;
        {
            std::lock_guard lock(context_.submit_mutex());
            const SubmitMutex::SubmitTurn turn(context_.submit_mutex(), false);  // a queue operation: the compile gate
            // The old target may still be in use.
            context_.observe_result(context_.vk.vkQueueWaitIdle(queue_), "vkQueueWaitIdle (present)");
        }
        bool changed = false;
        return composer_->ensure_target(extent_, changed);
    }

    // Records the composition of `planes` and its copy to swapchain image `image`. False, with nothing recorded and the
    // tracker untouched, when a view cannot be made. The caller holds the resource and tracker locks.
    bool record_planes(Frame& frame, std::uint32_t slot, const std::array<ResourceBinding, 2>& current,
                       const PresentPlanes& planes, std::uint32_t image, bool copy_frame) {
        auto& vk = context_.vk;
        const ReplayFunctions& fn = context_.replay_;
        if (composer_->target() == VK_NULL_HANDLE || composer_->extent().width != extent_.width ||
            composer_->extent().height != extent_.height)
            return false;  // the target does not match the swapchain: nothing recorded
        drop_views(frame);
        for (std::uint32_t i = 0; i < planes.count; ++i) {
            VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view_info.image = current[i].image;
            view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view_info.format = current[i].format;
            view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            if (vk.vkCreateImageView(context_.device(), &view_info, nullptr, &frame.views[i]) != VK_SUCCESS) {
                drop_views(frame);
                return false;
            }
        }
        if (planes.count == 1) frame.views[1] = VK_NULL_HANDLE;  // the shader never reads it; plane 0's view is bound twice

        VkCommandBuffer commands = frame.commands;
        vk.vkResetCommandBuffer(commands, 0);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk.vkBeginCommandBuffer(commands, &begin);

        constexpr VkAccessFlags any = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        const bool same = planes.count > 1 && planes.plane[1].source == planes.plane[0].source;
        std::array<LayoutPieces, 2> pieces;
        std::array<VkImageLayout, 2> read_layout{};
        // Each distinct source: its PRESENT (common) layout, then readable by the compute pass. A never-written back buffer
        // is zero-filled first, as any committed resource is.
        for (std::uint32_t i = 0; i < planes.count; ++i) {
            read_layout[i] = image_layout(current[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            if (i == 1 && same) continue;
            const VkImageLayout common = image_layout(current[i], VK_IMAGE_LAYOUT_GENERAL);
            pieces[i] = detail::current(&context_.tracker_, planes.plane[i].source, range, common);
            detail::initialize(fn, commands, current[i], planes.plane[i].source, pieces[i], common, &context_.tracker_);
            for (const auto& piece : pieces[i])
                detail::image_barrier(fn, commands, current[i], piece.range, piece.layout, read_layout[i],
                                      VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                      VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        }

        // The pass: destinations in output pixels, sources in texels.
        PresentComposePush push{};
        const float out_w = float(extent_.width), out_h = float(extent_.height);
        std::uint32_t* flags[2] = {&push.flags0, &push.flags1};
        float* dest[2] = {push.dest0, push.dest1};
        float* src[2] = {push.src0, push.src1};
        for (std::uint32_t i = 0; i < planes.count; ++i) {
            const PresentPlane& plane = planes.plane[i];
            dest[i][0] = plane.dest[0] * out_w;
            dest[i][1] = plane.dest[1] * out_h;
            dest[i][2] = plane.dest[2] * out_w;
            dest[i][3] = plane.dest[3] * out_h;
            const PresentRect whole{0, 0, std::int32_t(current[i].width), std::int32_t(current[i].height)};
            const PresentRect& r = plane.has_source_rect ? plane.source_rect : whole;
            src[i][0] = float(r.left);
            src[i][1] = float(r.top);
            src[i][2] = float(r.right);
            src[i][3] = float(r.bottom);
            *flags[i] = (plane.point_filter ? compose_flag::point : 0u) |
                        (detail::is_srgb_format(current[i].format) ? compose_flag::srgb_source : 0u) |
                        (plane.hdr10 ? compose_flag::hdr10 : 0u) | (plane.straight_alpha ? compose_flag::straight_alpha : 0u);
        }
        push.plane_count = planes.count;
        push.out_hdr = (hdr_active_ || force_hdr_encoding_) ? 1u : 0u;
        push.sdr_white = planes.sdr_white_nits;
        push.sdr_eotf = std::uint32_t(planes.sdr_eotf);
        push.hdr_peak = planes.hdr_peak_nits;
        push.width = extent_.width;
        push.height = extent_.height;
        composer_->record(commands, slot, frame.views[0], read_layout[0], frame.views[1] ? frame.views[1] : frame.views[0],
                          read_layout[1] ? read_layout[1] : read_layout[0], push);

        // The swapchain image, written by a copy of the composed image (same size: no scaling).
        ResourceBinding target = target_of(image);
        detail::image_barrier(fn, commands, target, range, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {std::int32_t(extent_.width), std::int32_t(extent_.height), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[1] = {std::int32_t(extent_.width), std::int32_t(extent_.height), 1};
        vkCmdBlitImage_(commands, composer_->target(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, images_[image],
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
        finish_image(commands, image, copy_frame);
        for (std::uint32_t i = 0; i < planes.count; ++i) {
            if (i == 1 && same) continue;
            for (const auto& piece : pieces[i])
                detail::image_barrier(fn, commands, current[i], piece.range, read_layout[i], piece.layout,
                                      VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, any,
                                      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        }
        vk.vkEndCommandBuffer(commands);
        return true;
    }

    // vkGetPhysicalDeviceFormatProperties, optimal tiling, cached per format.
    VkFormatFeatureFlags format_features(VkFormat format) {
        const auto it = features_.find(format);
        if (it != features_.end()) return it->second;
        VkFormatProperties properties{};
        if (vkGetPhysicalDeviceFormatProperties_) vkGetPhysicalDeviceFormatProperties_(physical_, format, &properties);
        return features_[format] = properties.optimalTilingFeatures;
    }
    // Scaling blits filter bilinearly when the source format allows it, else take the nearest texel.
    VkFilter blit_filter(VkFormat source) {
        return (format_features(source) & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? VK_FILTER_LINEAR
                                                                                              : VK_FILTER_NEAREST;
    }

    VkResult acquire(VkSemaphore semaphore, std::uint32_t* image) {
        for (;;) {
            const VkResult result = context_.observe_result(
                vkAcquireNextImageKHR_(context_.device(), swapchain_, 50'000'000, semaphore, VK_NULL_HANDLE, image),
                "vkAcquireNextImageKHR");
            if (result != VK_TIMEOUT && result != VK_NOT_READY) return result;
            if (should_abort()) return VK_NOT_READY;
        }
    }

    // Clears the swapchain image to opaque black.
    void record_blank(VkCommandBuffer commands, std::uint32_t image, bool copy_frame) {
        auto& vk = context_.vk;
        const ReplayFunctions& fn = context_.replay_;
        vk.vkResetCommandBuffer(commands, 0);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk.vkBeginCommandBuffer(commands, &begin);
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        detail::image_barrier(fn, commands, target_of(image), range, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        const VkClearColorValue black{{0.0f, 0.0f, 0.0f, 1.0f}};
        vkCmdClearColorImage_(commands, images_[image], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        finish_image(commands, image, copy_frame);
        vk.vkEndCommandBuffer(commands);
    }

    // The swapchain image is written (TRANSFER_DST): optionally copy it to the capture buffer, then hand it to
    // the presentation engine.
    void finish_image(VkCommandBuffer commands, std::uint32_t image, bool copy_frame) {
        const ReplayFunctions& fn = context_.replay_;
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageLayout layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        VkAccessFlags access = VK_ACCESS_TRANSFER_WRITE_BIT;
        if (copy_frame) {
            // Two frames may be in flight and share `capture_buffer_`: serialize the prior frame's capture copy
            // before overwriting it, while moving this frame's swapchain image to TRANSFER_SRC.
            VkBufferMemoryBarrier capture_waw{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            capture_waw.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            capture_waw.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            capture_waw.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            capture_waw.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            capture_waw.buffer = capture_buffer_;
            capture_waw.offset = 0;
            capture_waw.size = VK_WHOLE_SIZE;
            VkImageMemoryBarrier to_src{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            to_src.srcAccessMask = access;
            to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            to_src.oldLayout = layout;
            to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_src.image = images_[image];
            to_src.subresourceRange = range;
            fn.pipeline_barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                nullptr, 1, &capture_waw, 1, &to_src);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {extent_.width, extent_.height, 1};
            fn.copy_image_to_buffer(commands, images_[image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, capture_buffer_, 1,
                                    &copy);
            auto to_host = capture_waw;
            to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            fn.pipeline_barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                                0, nullptr, 1, &to_host, 0, nullptr);
            layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            access = VK_ACCESS_TRANSFER_READ_BIT;
        }
        detail::image_barrier(fn, commands, target_of(image), range, layout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, access,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    }

    // (Re)creates the capture buffer for the current swapchain size: host visible, coherent, persistently mapped.
    bool make_capture_buffer() {
        release_capture();
        auto& vk = context_.vk;
        const VkDevice device = context_.device();
        capture_size_ = VkDeviceSize(extent_.width) * extent_.height * present_dump::texel_size(format_);
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = capture_size_;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vk.vkCreateBuffer(device, &info, nullptr, &capture_buffer_) != VK_SUCCESS) return false;
        VkMemoryRequirements requirements{};
        vk.vkGetBufferMemoryRequirements(device, capture_buffer_, &requirements);
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties_(physical_, &memory);
        constexpr VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        std::uint32_t type = UINT32_MAX;
        for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if ((requirements.memoryTypeBits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & wanted) == wanted) {
                type = i;
                break;
            }
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = type;
        void* mapped = nullptr;
        if (type == UINT32_MAX || vk.vkAllocateMemory(device, &allocate, nullptr, &capture_memory_) != VK_SUCCESS ||
            vk.vkBindBufferMemory(device, capture_buffer_, capture_memory_, 0) != VK_SUCCESS ||
            vk.vkMapMemory(device, capture_memory_, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
            release_capture();
            return false;
        }
        capture_ptr_ = static_cast<std::uint8_t*>(mapped);
        captured_ = false;
        return true;
    }

    void release_capture() {
        auto& vk = context_.vk;
        const VkDevice device = context_.device();
        if (capture_memory_) vk.vkFreeMemory(device, capture_memory_, nullptr);  // unmaps
        if (capture_buffer_) vk.vkDestroyBuffer(device, capture_buffer_, nullptr);
        capture_memory_ = VK_NULL_HANDLE;
        capture_buffer_ = VK_NULL_HANDLE;
        capture_ptr_ = nullptr;
        captured_ = false;
    }

    // The name VkFormat has in the index; the few formats a capture can be written from, else its number.
    static const char* capture_format_name(VkFormat format) {
        switch (format) {
        case VK_FORMAT_B8G8R8A8_UNORM: return "VK_FORMAT_B8G8R8A8_UNORM";
        case VK_FORMAT_B8G8R8A8_SRGB: return "VK_FORMAT_B8G8R8A8_SRGB";
        case VK_FORMAT_R8G8B8A8_UNORM: return "VK_FORMAT_R8G8B8A8_UNORM";
        case VK_FORMAT_R8G8B8A8_SRGB: return "VK_FORMAT_R8G8B8A8_SRGB";
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "VK_FORMAT_A2B10G10R10_UNORM_PACK32";
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32: return "VK_FORMAT_A2R10G10B10_UNORM_PACK32";
        default: return "unknown";
        }
    }

    VkResult finish_pending_capture() {
        if (!pending_capture_) return VK_SUCCESS;
        const VkResult waited = context_.observe_result(
            vkWaitForFences_(context_.device(), 1, &pending_capture_, VK_TRUE, UINT64_MAX), "vkWaitForFences (capture)");
        pending_capture_ = VK_NULL_HANDLE;
        if (waited == VK_SUCCESS) write_capture();
        return waited;
    }

    void write_capture() {
        const VkExtent2D extent = captured_extent_;
        const VkFormat format = captured_format_;
        const std::uint64_t number = presented_;
        const std::size_t texels = std::size_t(extent.width) * extent.height;
        if (!capture_ptr_ || !texels || texels > capture_size_ / 4) return;
        std::vector<std::uint8_t> rgb(texels * 3);
        std::uint64_t sum[3] = {0, 0, 0};
        const std::uint8_t* src = capture_ptr_;
        switch (format) {
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
            for (std::size_t i = 0; i < texels; ++i) {
                const std::uint8_t b = src[i * 4 + 0], g = src[i * 4 + 1], r = src[i * 4 + 2];
                rgb[i * 3 + 0] = r;
                rgb[i * 3 + 1] = g;
                rgb[i * 3 + 2] = b;
                sum[0] += r;
                sum[1] += g;
                sum[2] += b;
            }
            break;
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
            for (std::size_t i = 0; i < texels; ++i) {
                const std::uint8_t r = src[i * 4 + 0], g = src[i * 4 + 1], b = src[i * 4 + 2];
                rgb[i * 3 + 0] = r;
                rgb[i * 3 + 1] = g;
                rgb[i * 3 + 2] = b;
                sum[0] += r;
                sum[1] += g;
                sum[2] += b;
            }
            break;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32: {
            const bool bgra = format == VK_FORMAT_A2R10G10B10_UNORM_PACK32;  // B in the low bits
            for (std::size_t i = 0; i < texels; ++i) {
                std::uint32_t word = 0;
                std::memcpy(&word, src + i * 4, sizeof word);
                const std::uint32_t low = word & 0x3ffu, mid = (word >> 10) & 0x3ffu, high = (word >> 20) & 0x3ffu;
                const auto to8 = [](std::uint32_t v) { return std::uint8_t((v * 255u + 511u) / 1023u); };
                const std::uint8_t r = to8(bgra ? high : low), g = to8(mid), b = to8(bgra ? low : high);
                rgb[i * 3 + 0] = r;
                rgb[i * 3 + 1] = g;
                rgb[i * 3 + 2] = b;
                sum[0] += r;
                sum[1] += g;
                sum[2] += b;
            }
            break;
        }
        default: return;  // not a format the capture can be written from (recreate() refused the others)
        }
        char name[64];
        std::snprintf(name, sizeof name, "frame_%05llu.ppm", static_cast<unsigned long long>(number));
        std::string path = capture_dir_;
        if (path.back() != '/' && path.back() != '\\') path += '/';
        path += name;
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file) {
            std::fprintf(stderr, "[rt] present: cannot write %s\n", path.c_str());
            return;
        }
        file << "P6\n" << extent.width << ' ' << extent.height << "\n255\n";
        file.write(reinterpret_cast<const char*>(rgb.data()), std::streamsize(rgb.size()));
        file.close();
        const double scale = 1.0 / double(texels);
        const double mean[3] = {sum[0] * scale, sum[1] * scale, sum[2] * scale};
        path.resize(path.size() - std::strlen(name));
        path += "index.txt";
        std::ofstream index(path, std::ios::app);
        if (index)
            index << "frame " << number << " format " << int(format) << " (" << capture_format_name(format)
                  << ") extent " << extent.width << 'x' << extent.height << " mean rgb " << std::fixed
                  << std::setprecision(2) << mean[0] << ' ' << mean[1] << ' ' << mean[2] << '\n';
        ++capture_written_;
        std::fprintf(stderr, "[rt] present: capture %s (%ux%u, mean rgb %.2f %.2f %.2f)\n", name, extent.width,
                     extent.height, mean[0], mean[1], mean[2]);
    }

    // (Re)creates the swapchain for the surface's current size. A zero-sized surface (minimised) leaves no
    // swapchain and is retried on the next present. Keep the old swapchain until the surface queries succeed and
    // the replacement can be attempted. An incomplete replacement stays stale so the next present tries again.
    bool recreate(bool vsync, bool srgb, bool hdr = false) {
        auto& vk = context_.vk;
        const VkDevice device = context_.device();
        {
            std::lock_guard lock(context_.submit_mutex());
            const SubmitMutex::SubmitTurn turn(context_.submit_mutex(), false);  // a queue operation: the compile gate
            // The old swapchain's images and semaphores may still be in use.
            context_.observe_result(vk.vkQueueWaitIdle(queue_), "vkQueueWaitIdle (present)");
        }
        if (finish_pending_capture() != VK_SUCCESS) return false;
        stale_ = true;  // until this succeeds
        tear_mode_ = VK_PRESENT_MODE_MAX_ENUM_KHR;
        VkSurfaceCapabilitiesKHR caps{};
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR_(physical_, surface_, &caps) != VK_SUCCESS) return false;
        VkExtent2D extent = caps.currentExtent;
        // When the platform leaves the swapchain extent to the application (0xFFFFFFFF, as on Intel UHD / Win32 WSI),
        // query the window client area or fall back to the last known extent, and clamp to the surface bounds (D3D-165).
        if (extent.width == 0xFFFFFFFF || extent.height == 0xFFFFFFFF) {
            if (extent_fn_) extent = extent_fn_();
            // ponytail: if no window extent function was supplied, fall back to last extent_ or minImageExtent
            if (extent.width == 0xFFFFFFFF || extent.height == 0xFFFFFFFF || (extent.width == 0 && extent.height == 0)) {
                extent = extent_.width && extent_.height ? extent_ : caps.minImageExtent;
            }
            if (extent.width && extent.height) {
                extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
                extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
            }
        }
        if (extent.width == 0 || extent.height == 0) {
            {
                std::lock_guard lock(context_.submit_mutex());
                release(swapchain_);
            }
            swapchain_ = VK_NULL_HANDLE;
            vsync_ = vsync;
            swapchain_srgb_ = srgb;
            swapchain_hdr_req_ = hdr;
            hdr_active_ = false;
            stale_ = false;
            return true;
        }
        VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (dump_.enabled() && !(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
            dump_.disable("surface does not support swapchain readback");
        if (capture_ || dump_.enabled()) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if ((caps.supportedUsageFlags & usage) != usage) return false;
        // Not used by the presenter itself (it blits or copies into the images), but overlays that hook the swapchain
        // draw their own frame into its images through a framebuffer, which needs COLOR_ATTACHMENT: without it
        // Bandicam's VK_LAYER_bandicam_helper builds framebuffers and image views on images that don't allow it
        // (VUID-VkFramebufferCreateInfo-pAttachments-00877, VUID-VkImageViewCreateInfo-image-04441). The surface
        // reports it for every swapchain (the spec requires it in supportedUsageFlags), and the DXGI swapchain
        // (dxgi_swapchain.h) already asks for it.
        if (caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

        std::uint32_t count = 0;
        if (vkGetPhysicalDeviceSurfaceFormatsKHR_(physical_, surface_, &count, nullptr) != VK_SUCCESS || !count)
            return false;
        std::vector<VkSurfaceFormatKHR> formats(count);
        if (vkGetPhysicalDeviceSurfaceFormatsKHR_(physical_, surface_, &count, formats.data()) != VK_SUCCESS || !count)
            return false;
        formats.resize(count);
        // An sRGB source wants an sRGB swapchain (the blit decodes and encodes, which cancel); any other source
        // wants UNORM (the bytes go through as they are). BGRA before RGBA, as the surface lists them.
        VkSurfaceFormatKHR chosen = formats[0];
        bool found = false;
        bool chose_hdr = false;
        if (hdr) {
            // A plane in HDR10 (PQ, Rec.2020): the surface's ST.2084 format, which needs VK_EXT_swapchain_colorspace on
            // the instance. Without it the composition converts to SDR.
            for (const VkFormat wanted : {VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2R10G10B10_UNORM_PACK32}) {
                for (const auto& f : formats)
                    if (f.format == wanted && f.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT) {
                        chosen = f;
                        found = chose_hdr = true;
                        break;
                    }
                if (found) break;
            }
            if (!found && !warned_hdr_) {
                warned_hdr_ = true;
                std::fprintf(stderr, "[rt] present: the surface has no HDR10 (ST.2084) format; HDR10 planes are converted to "
                                     "SDR by tone mapping\n");
            }
        }
        for (const VkFormat wanted : srgb ? std::array<VkFormat, 2>{VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB}
                                                : std::array<VkFormat, 2>{VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}) {
            if (chose_hdr) break;
            for (const auto& f : formats)
                if (f.format == wanted && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                    chosen = f;
                    found = true;
                    break;
                }
            if (found) break;
        }
        if (!found && srgb) {
            if (!warned_srgb_) std::fprintf(stderr, "[rt] present: the surface has no sRGB 8-bit format; an sRGB back buffer is shown darker\n");
            warned_srgb_ = true;
            for (const auto& f : formats)
                if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
                    f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                    chosen = f;
                    break;
                }
        }
        if (vkGetPhysicalDeviceSurfacePresentModesKHR_(physical_, surface_, &count, nullptr) != VK_SUCCESS || !count)
            return false;
        std::vector<VkPresentModeKHR> modes(count);
        if (vkGetPhysicalDeviceSurfacePresentModesKHR_(physical_, surface_, &count, modes.data()) != VK_SUCCESS || !count)
            return false;
        modes.resize(count);
        VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;  // always available
        if (!vsync) {
            for (const VkPresentModeKHR wanted : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR})
                if (std::find(modes.begin(), modes.end(), wanted) != modes.end()) {
                    mode = wanted;
                    break;
                }
        }

        VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        info.surface = surface_;
        info.minImageCount = std::max(caps.minImageCount + 1, 2u);
        if (caps.maxImageCount) info.minImageCount = std::min(info.minImageCount, caps.maxImageCount);
        info.imageFormat = chosen.format;
        info.imageColorSpace = chosen.colorSpace;
        info.imageExtent = extent;
        info.imageArrayLayers = 1;
        info.imageUsage = usage;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = caps.currentTransform;
        info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        if (!(caps.supportedCompositeAlpha & info.compositeAlpha)) info.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
        info.presentMode = mode;
#ifdef LEONIDA_SWAPCHAIN_MAINTENANCE1
        // A vsync swapchain that can also be presented in IMMEDIATE (or FIFO_RELAXED), switched per present.
        VkSwapchainPresentModesCreateInfoEXT switchable{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_EXT};
        std::array<VkPresentModeKHR, 2> switchable_modes{VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAX_ENUM_KHR};
        VkPresentModeKHR tear = VK_PRESENT_MODE_MAX_ENUM_KHR;
        if (vsync && mode == VK_PRESENT_MODE_FIFO_KHR) tear = find_tear_mode(modes, info.minImageCount, info.imageUsage, info.imageExtent);
        if (tear != VK_PRESENT_MODE_MAX_ENUM_KHR) {
            switchable_modes[1] = tear;
            switchable.presentModeCount = 2;
            switchable.pPresentModes = switchable_modes.data();
            info.pNext = &switchable;
        }
#endif
        info.clipped = VK_TRUE;
        const VkSwapchainKHR old = swapchain_;
        swapchain_ = VK_NULL_HANDLE;
        info.oldSwapchain = old;
        VkSwapchainKHR fresh = VK_NULL_HANDLE;
        const VkResult created = context_.observe_result(vkCreateSwapchainKHR_(device, &info, nullptr, &fresh),
                                                        "vkCreateSwapchainKHR");
        {
            std::lock_guard lock(context_.submit_mutex());
            release(old);  // retired by the creation attempt, whether or not it succeeded
        }
        if (created != VK_SUCCESS) return false;

        std::vector<VkImage> images;
        std::vector<VkSemaphore> ready;
        const auto discard = [&] {
            for (VkSemaphore semaphore : ready) vk.vkDestroySemaphore(device, semaphore, nullptr);
            // The WSI implementation may block on the device here too.
            std::lock_guard lock(context_.submit_mutex());
            vkDestroySwapchainKHR_(device, fresh, nullptr);
            return false;
        };
        if (vkGetSwapchainImagesKHR_(device, fresh, &count, nullptr) != VK_SUCCESS || !count) return discard();
        images.resize(count);
        if (vkGetSwapchainImagesKHR_(device, fresh, &count, images.data()) != VK_SUCCESS) return discard();
        images.resize(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VkSemaphore semaphore = VK_NULL_HANDLE;
            if (vk.vkCreateSemaphore(device, &semaphore_info, nullptr, &semaphore) != VK_SUCCESS) return discard();
            ready.push_back(semaphore);
        }
        swapchain_ = fresh;
#ifdef LEONIDA_SWAPCHAIN_MAINTENANCE1
        tear_mode_ = tear;
#endif
        images_ = std::move(images);
        ready_ = std::move(ready);
        format_ = chosen.format;
        extent_ = extent;
        extent_packed_ = (std::uint64_t(extent.width) << 32) | extent.height;
        hdr_active_ = chose_hdr;
        dump_pq_ = chosen.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT;
        if (capture_ || dump_.enabled()) {
            const auto stride = present_dump::texel_size(format_);
            if (capture_ && stride != 4) return false;  // read_back() promises four bytes per texel
            if (!stride || !make_capture_buffer()) {
                dump_.disable("cannot read back this swapchain format or allocate its capture buffer");
                if (capture_) return false;
            }
        }
        vsync_ = vsync;
        swapchain_srgb_ = srgb;
        swapchain_hdr_req_ = hdr;
        stale_ = false;
        return true;
    }

#ifdef LEONIDA_SWAPCHAIN_MAINTENANCE1
    // The mode a FIFO swapchain can also present in, so that a late frame can be shown at once: IMMEDIATE, else
    // FIFO_RELAXED, each only if the surface lists it and reports it compatible with FIFO (VK_KHR_surface_maintenance1).
    // `min_images` is raised to what that mode needs too. The mode must also accept the swapchain's image usage and
    // extent (its capabilities can differ from FIFO's). MAX_ENUM when there is none or the extension is not in use.
    VkPresentModeKHR find_tear_mode(const std::vector<VkPresentModeKHR>& modes, std::uint32_t& min_images,
                                    VkImageUsageFlags usage, VkExtent2D extent) {
        if (!maintenance1_ || !vkGetPhysicalDeviceSurfaceCapabilities2KHR_) return VK_PRESENT_MODE_MAX_ENUM_KHR;
        // The capabilities of the surface for one present mode, and the modes it can switch to from there.
        const auto ask = [&](VkPresentModeKHR asked, VkSurfaceCapabilitiesKHR& caps,
                             std::array<VkPresentModeKHR, 16>* compatible, std::uint32_t* compatible_count) {
            VkSurfacePresentModeEXT for_mode{VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_EXT};
            for_mode.presentMode = asked;
            VkPhysicalDeviceSurfaceInfo2KHR surface_info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR, &for_mode};
            surface_info.surface = surface_;
            VkSurfacePresentModeCompatibilityEXT compat{VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_COMPATIBILITY_EXT};
            if (compatible) {
                compat.presentModeCount = std::uint32_t(compatible->size());
                compat.pPresentModes = compatible->data();
            }
            VkSurfaceCapabilities2KHR out{VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR, compatible ? &compat : nullptr};
            if (vkGetPhysicalDeviceSurfaceCapabilities2KHR_(physical_, &surface_info, &out) != VK_SUCCESS) return false;
            caps = out.surfaceCapabilities;
            if (compatible_count) *compatible_count = std::min<std::uint32_t>(compat.presentModeCount, 16);
            return true;
        };
        VkSurfaceCapabilitiesKHR fifo_caps{};
        std::array<VkPresentModeKHR, 16> compatible{};
        std::uint32_t compatible_count = 0;
        if (!ask(VK_PRESENT_MODE_FIFO_KHR, fifo_caps, &compatible, &compatible_count)) return VK_PRESENT_MODE_MAX_ENUM_KHR;
        for (const VkPresentModeKHR wanted : {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_FIFO_RELAXED_KHR}) {
            if (std::find(modes.begin(), modes.end(), wanted) == modes.end() ||
                std::find(compatible.begin(), compatible.begin() + compatible_count, wanted) ==
                    compatible.begin() + compatible_count)
                continue;
            VkSurfaceCapabilitiesKHR mode_caps{};
            if (!ask(wanted, mode_caps, nullptr, nullptr)) continue;
            if ((mode_caps.supportedUsageFlags & usage) != usage) continue;
            if (extent.width < mode_caps.minImageExtent.width || extent.height < mode_caps.minImageExtent.height ||
                extent.width > mode_caps.maxImageExtent.width || extent.height > mode_caps.maxImageExtent.height)
                continue;
            // One image count that every mode of the swapchain accepts.
            std::uint32_t count = std::max({min_images, fifo_caps.minImageCount, mode_caps.minImageCount});
            const std::uint32_t most = std::min(fifo_caps.maxImageCount ? fifo_caps.maxImageCount : UINT32_MAX,
                                                mode_caps.maxImageCount ? mode_caps.maxImageCount : UINT32_MAX);
            if (count > most) continue;
            min_images = count;
            return wanted;
        }
        return VK_PRESENT_MODE_MAX_ENUM_KHR;
    }
#endif

    // Destroys a swapchain that is being replaced (or the last one), with the per-image semaphores that went with it.
    void release(VkSwapchainKHR swapchain) {
        const VkDevice device = context_.device();
        for (VkSemaphore semaphore : ready_)
            if (semaphore) context_.vk.vkDestroySemaphore(device, semaphore, nullptr);
        ready_.clear();
        images_.clear();
        if (swapchain) vkDestroySwapchainKHR_(device, swapchain, nullptr);
    }

    VulkanContext& context_;
    VkInstance instance_;
    VkPhysicalDevice physical_;
    VkQueue queue_;
    mutable std::recursive_mutex present_mutex_;  // all presenter state: submit(), a deferred present, the accessors
    mutable std::mutex abort_mutex_;               // abort_ alone: set_abort() must work while a present waits
    std::deque<PendingPresent> deferred_;  // presents queued behind held work, in call order
    VkSurfaceKHR surface_;
    bool maintenance1_ = false;  // VK_KHR/EXT_swapchain_maintenance1 (and the surface extension) are enabled
    VkPresentModeKHR tear_mode_ = VK_PRESENT_MODE_MAX_ENUM_KHR;  // set with the swapchain, see tear_mode()
    std::atomic<bool> last_tore_{false};
    std::atomic<std::uint64_t> extent_packed_{0};  // extent_ for readers that must not wait for a present
    std::atomic<std::uint64_t> tear_presents_{0};
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    std::vector<VkImage> images_;
    std::vector<VkSemaphore> ready_;  // per swapchain image: blit done, present may start
    std::array<Frame, 2> frames_{};
    VkCommandPool pool_ = VK_NULL_HANDLE;
    std::uint64_t frame_index_ = 0;
    std::atomic<std::uint64_t> presented_{0};
    bool vsync_ = true;
    bool stale_ = false;
    bool ok_ = false;
    bool swapchain_srgb_ = false;  // the swapchain format follows the source being presented (set once it exists)
    bool swapchain_hdr_req_ = false;  // the swapchain was built for an HDR10 request (it has the format only if hdr_active_)
    bool hdr_active_ = false;
    std::atomic<std::uint64_t> composed_{0};
    std::atomic<bool> force_hdr_encoding_{false};
    bool warned_hdr_ = false;
    std::unique_ptr<PresentComposer> composer_;
    std::function<bool()> abort_;
    std::function<VkExtent2D()> extent_fn_;
    std::function<VkSurfaceKHR()> recreate_surface_fn_;  // makes a new Win32 surface for the same window
    bool surface_lost_ = false;  // set by mark_surface_lost(); cleared once a new surface was created
    std::map<VkFormat, VkFormatFeatureFlags> features_;
    bool warned_srgb_ = false;
    std::shared_ptr<void> splash_owner_;
    ResourceBinding splash_binding_{};
    ResourceId splash_source_{};
    bool splash_capture_only_ = false;
    bool capture_ = false;
    present_dump::PresentDump dump_;
    bool dump_pq_ = false;
    bool continuous_readback_ = false;  // explicit enable_capture(), independent of disk sampling
    VkBuffer capture_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory capture_memory_ = VK_NULL_HANDLE;
    VkDeviceSize capture_size_ = 0;
    std::uint8_t* capture_ptr_ = nullptr;
    bool captured_ = false;
    VkExtent2D captured_extent_{};
    VkFormat captured_format_ = VK_FORMAT_UNDEFINED;
    // LEONIDA_CAPTURE: write the copies the staging buffer makes to <capture_dir_> (see finish_pending_capture()).
    bool capture_to_disk_ = false;
    std::string capture_dir_;
    int capture_every_ = 30;  // presented frames between files
    int capture_limit_ = 24;  // files at most
    VkFence pending_capture_ = VK_NULL_HANDLE;  // owns the shared staging bytes until written
    std::uint64_t capture_written_ = 0;

    PFN_vkGetPhysicalDeviceSurfaceSupportKHR vkGetPhysicalDeviceSurfaceSupportKHR_ = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR vkGetPhysicalDeviceSurfaceCapabilitiesKHR_ = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR vkGetPhysicalDeviceSurfaceFormatsKHR_ = nullptr;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR vkGetPhysicalDeviceSurfacePresentModesKHR_ = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR vkGetPhysicalDeviceSurfaceCapabilities2KHR_ = nullptr;
    PFN_vkDestroySurfaceKHR vkDestroySurfaceKHR_ = nullptr;
    PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr_ = nullptr;
    PFN_vkCreateSwapchainKHR vkCreateSwapchainKHR_ = nullptr;
    PFN_vkDestroySwapchainKHR vkDestroySwapchainKHR_ = nullptr;
    PFN_vkGetSwapchainImagesKHR vkGetSwapchainImagesKHR_ = nullptr;
    PFN_vkAcquireNextImageKHR vkAcquireNextImageKHR_ = nullptr;
    PFN_vkQueuePresentKHR vkQueuePresentKHR_ = nullptr;
    PFN_vkCmdBlitImage vkCmdBlitImage_ = nullptr;
    PFN_vkCmdClearColorImage vkCmdClearColorImage_ = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties_ = nullptr;
    PFN_vkGetPhysicalDeviceFormatProperties vkGetPhysicalDeviceFormatProperties_ = nullptr;
    PFN_vkCreateFence vkCreateFence_ = nullptr;
    PFN_vkDestroyFence vkDestroyFence_ = nullptr;
    PFN_vkWaitForFences vkWaitForFences_ = nullptr;
    PFN_vkResetFences vkResetFences_ = nullptr;
};

} // namespace leonida

#include "vulkan_present_splash.h"
