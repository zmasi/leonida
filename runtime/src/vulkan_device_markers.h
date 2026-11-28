#pragma once

// LEONIDA_DEVICE_MARKERS=1 (diagnostic, with LEONIDA_DEVICE_FAULT=1 or 2): AMD breadcrumbs for the device-fault report
// (vulkan_device_fault.h). On a device with VK_AMD_buffer_marker every dispatch the report numbers writes its `seq`
// into its queue's slot of one small host-visible buffer twice: at top of pipe before the dispatch (started) and at
// bottom of pipe after it (finished). A bottom-of-pipe write waits for all earlier work of the queue, so the finished
// word is the last dispatch before which everything had completed; the dispatches after it up to the started word
// are the ones the GPU had begun and not finished. On a loss or a stall report the slots are read back and the oldest
// started, unfinished dispatch of a queue is reported as the likely hang. NVIDIA checkpoints do this on NVIDIA; RADV
// has none. The buffer lives in system memory, so the last values usually survive a ring reset, but they are only
// what the GPU wrote before it stopped: best-effort after a loss.

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

namespace leonida {

inline bool device_markers_requested() {
    static const bool on = [] {
        const char* value = std::getenv("LEONIDA_DEVICE_MARKERS");
        return value && !std::strcmp(value, "1");
    }();
    return on;
}

// One queue's slot as the GPU writes it: the low 32 bits of the seq of the last dispatch whose top-of-pipe write ran
// (started) and of the last whose bottom-of-pipe write ran (finished). 0 is no dispatch yet; seqs start at 1.
struct MarkerSlot {
    std::uint32_t started = 0;
    std::uint32_t finished = 0;
};

// `seq` comes after the 32-bit marker value `marker` (serial-number arithmetic, so a wrap past 2^32 still orders the
// recent dispatches the history holds).
constexpr bool marker_after(std::uint64_t seq, std::uint32_t marker) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(seq) - marker) > 0;
}

// What the marker history keeps of each recorded dispatch: enough to name one that left the report's 64-entry list.
struct MarkedDispatch {
    std::uint64_t seq = 0;
    std::uint64_t execute_call = 0;
    VkQueue queue = VK_NULL_HANDLE;
    const char* queue_kind = nullptr;
    std::uint32_t list = 0;
    std::uint32_t command = 0;
    const char* shader = nullptr;  // ShaderLibrary name, valid until device teardown
};

// The recent dispatches of every queue, oldest first once read. Allocated once, when the markers are on.
class MarkerHistory {
public:
    static constexpr std::size_t capacity = 16384;

    MarkerHistory() : entries_(capacity) {}

    void push(const MarkedDispatch& d) { entries_[count_++ % capacity] = d; }

    std::size_t size() const { return count_ < capacity ? count_ : capacity; }

    template<typename Visit>
    void for_each(Visit&& visit) const {  // oldest first
        for (std::size_t i = count_ - size(); i < count_; ++i) visit(entries_[i % capacity]);
    }

private:
    std::vector<MarkedDispatch> entries_;
    std::size_t count_ = 0;
};

// What one queue's slot says, judged against the history (MarkerHistory::for_each order).
struct MarkerQueueReport {
    MarkerSlot slot;
    const char* queue_kind = nullptr;   // from the history, null when it holds no dispatch of the queue
    std::vector<std::uint64_t> unfinished;  // seqs of this queue in (finished, started], oldest first
    std::uint64_t likely_hang = 0;      // the oldest of them; 0 when none was started and not finished
    std::uint64_t next_queued = 0;      // idle queue: the oldest submitted dispatch after `finished`, not started yet
    bool missing = false;               // started > finished but the history no longer holds any of them

    bool idle() const { return slot.started == slot.finished; }
};

template<typename History>
MarkerQueueReport judge_markers(VkQueue queue, MarkerSlot slot, const History& history) {
    MarkerQueueReport report;
    // The started word is written before the finished word of the same dispatch and both only rise, so started is
    // never behind; a value that says otherwise (memory lost in a reset) is read as an idle queue.
    if (slot.started != slot.finished && !marker_after(slot.started, slot.finished)) slot.started = slot.finished;
    report.slot = slot;
    history.for_each([&](const MarkedDispatch& d) {
        if (d.queue != queue) return;
        if (!report.queue_kind) report.queue_kind = d.queue_kind;
        if (!marker_after(d.seq, slot.finished)) return;
        if (!marker_after(d.seq, slot.started)) report.unfinished.push_back(d.seq);
        else if (!report.next_queued) report.next_queued = d.seq;
    });
    if (!report.unfinished.empty()) report.likely_hang = report.unfinished.front();
    report.missing = !report.idle() && report.unfinished.empty();
    if (!report.idle()) report.next_queued = 0;  // only said of an idle queue
    return report;
}

// Where the recorder writes one queue's markers. Empty (no buffer) when the markers are off.
struct MarkerTarget {
    PFN_vkCmdWriteBufferMarkerAMD write = nullptr;
    PFN_vkCmdWriteBufferMarker2AMD write2 = nullptr;  // with synchronization2
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;  // the slot: started at +0, finished at +4
    PFN_vkCmdPipelineBarrier barrier = nullptr;

    explicit operator bool() const { return buffer != VK_NULL_HANDLE; }

    // Before a dispatch's marker pair, outside rendering. The two writes touch disjoint words, so one
    // dependency orders transfer accesses from previous pairs against both new writes, including across
    // submits on this queue. The marker commands themselves order same-stage markers at TOP or BOTTOM.
    void prepare_dispatch(VkCommandBuffer cb) const {
        VkBufferMemoryBarrier memory{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        memory.srcAccessMask = memory.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        memory.srcQueueFamilyIndex = memory.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        memory.buffer = buffer;
        memory.offset = offset;
        memory.size = 8;
        barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, nullptr, 1, &memory, 0, nullptr);
    }

    void started(VkCommandBuffer command_buffer, std::uint64_t seq) const {
        if (write2)
            write2(command_buffer, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, buffer, offset, std::uint32_t(seq));
        else
            write(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, buffer, offset, std::uint32_t(seq));
    }
    void finished(VkCommandBuffer command_buffer, std::uint64_t seq) const {
        if (write2)
            write2(command_buffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, buffer, offset + 4, std::uint32_t(seq));
        else
            write(command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, buffer, offset + 4, std::uint32_t(seq));
    }
};

// The persistent marker buffer: one 8-byte slot per queue, in host-visible, host-coherent memory that stays mapped.
class DeviceMarkers {
public:
    static constexpr std::uint32_t max_queues = 64;

    DeviceMarkers() = default;
    DeviceMarkers(const DeviceMarkers&) = delete;
    DeviceMarkers& operator=(const DeviceMarkers&) = delete;
    ~DeviceMarkers() {
        if (memory_ && unmap_) unmap_(device_, memory_);
        if (buffer_ && destroy_buffer_) destroy_buffer_(device_, buffer_, nullptr);
        if (memory_ && free_memory_) free_memory_(device_, memory_, nullptr);
    }

    // Returns the VkResult of the step that failed, or VK_SUCCESS.
    VkResult create(PFN_vkGetDeviceProcAddr get, VkDevice device, const VkPhysicalDeviceMemoryProperties& memory,
                    bool synchronization2) {
        device_ = device;
#define LEONIDA_MARKER_LOAD(type, name) const auto name = reinterpret_cast<type>(get(device, #name))
        LEONIDA_MARKER_LOAD(PFN_vkCreateBuffer, vkCreateBuffer);
        LEONIDA_MARKER_LOAD(PFN_vkGetBufferMemoryRequirements, vkGetBufferMemoryRequirements);
        LEONIDA_MARKER_LOAD(PFN_vkAllocateMemory, vkAllocateMemory);
        LEONIDA_MARKER_LOAD(PFN_vkBindBufferMemory, vkBindBufferMemory);
        LEONIDA_MARKER_LOAD(PFN_vkMapMemory, vkMapMemory);
        LEONIDA_MARKER_LOAD(PFN_vkCmdWriteBufferMarkerAMD, vkCmdWriteBufferMarkerAMD);
        LEONIDA_MARKER_LOAD(PFN_vkCmdWriteBufferMarker2AMD, vkCmdWriteBufferMarker2AMD);
        LEONIDA_MARKER_LOAD(PFN_vkCmdPipelineBarrier, vkCmdPipelineBarrier);
#undef LEONIDA_MARKER_LOAD
        destroy_buffer_ = reinterpret_cast<PFN_vkDestroyBuffer>(get(device, "vkDestroyBuffer"));
        free_memory_ = reinterpret_cast<PFN_vkFreeMemory>(get(device, "vkFreeMemory"));
        unmap_ = reinterpret_cast<PFN_vkUnmapMemory>(get(device, "vkUnmapMemory"));
        if (!vkCreateBuffer || !vkGetBufferMemoryRequirements || !vkAllocateMemory || !vkBindBufferMemory ||
            !vkMapMemory || !destroy_buffer_ || !free_memory_ || !vkCmdWriteBufferMarkerAMD || !vkCmdPipelineBarrier)
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        write_ = vkCmdWriteBufferMarkerAMD;
        write2_ = synchronization2 ? vkCmdWriteBufferMarker2AMD : nullptr;
        barrier_ = vkCmdPipelineBarrier;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = VkDeviceSize{8} * max_queues;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;  // written by every queue, never read on the GPU
        VkResult result = vkCreateBuffer(device, &info, nullptr, &buffer_);
        if (result != VK_SUCCESS) return result;
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, buffer_, &requirements);
        const std::uint32_t type = memory_type(memory, requirements.memoryTypeBits);
        if (type == UINT32_MAX) return VK_ERROR_OUT_OF_HOST_MEMORY;
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = type;
        if ((result = vkAllocateMemory(device, &allocate, nullptr, &memory_)) != VK_SUCCESS) return result;
        if ((result = vkBindBufferMemory(device, buffer_, memory_, 0)) != VK_SUCCESS) return result;
        void* mapped = nullptr;
        if ((result = vkMapMemory(device, memory_, 0, VK_WHOLE_SIZE, 0, &mapped)) != VK_SUCCESS) return result;
        std::memset(mapped, 0, std::size_t(info.size));
        words_ = static_cast<volatile std::uint32_t*>(mapped);
        return VK_SUCCESS;
    }

    bool ready() const { return words_ != nullptr; }
    bool synchronization2() const { return write2_ != nullptr; }

    // The queue's slot, assigned on first use; false past max_queues.
    bool add_queue(VkQueue queue) {
        if (!ready() || !queue) return false;
        for (std::uint32_t i = 0; i < count_; ++i)
            if (queues_[i] == queue) return true;
        if (count_ == max_queues) return false;
        queues_[count_++] = queue;
        return true;
    }

    MarkerTarget target(VkQueue queue) const {
        for (std::uint32_t i = 0; i < count_; ++i)
            if (queues_[i] == queue) return {write_, write2_, buffer_, VkDeviceSize{8} * i, barrier_};
        return {};
    }

    std::span<const VkQueue> queues() const { return {queues_.data(), count_}; }

    MarkerSlot read(VkQueue queue) const {
        for (std::uint32_t i = 0; i < count_; ++i)
            if (queues_[i] == queue) return {words_[2 * i], words_[2 * i + 1]};
        return {};
    }

    // Host-visible and coherent, not device-local if there is such a type: system memory outlives a VRAM reset.
    static std::uint32_t memory_type(const VkPhysicalDeviceMemoryProperties& memory, std::uint32_t allowed) {
        constexpr VkMemoryPropertyFlags need = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        std::uint32_t fallback = UINT32_MAX;
        for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
            const VkMemoryPropertyFlags flags = memory.memoryTypes[i].propertyFlags;
            if (!(allowed & (1u << i)) || (flags & need) != need) continue;
            if (!(flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) return i;
            if (fallback == UINT32_MAX) fallback = i;
        }
        return fallback;
    }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    PFN_vkDestroyBuffer destroy_buffer_ = nullptr;
    PFN_vkFreeMemory free_memory_ = nullptr;
    PFN_vkUnmapMemory unmap_ = nullptr;
    PFN_vkCmdWriteBufferMarkerAMD write_ = nullptr;
    PFN_vkCmdWriteBufferMarker2AMD write2_ = nullptr;
    PFN_vkCmdPipelineBarrier barrier_ = nullptr;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    volatile std::uint32_t* words_ = nullptr;
    std::array<VkQueue, max_queues> queues_{};
    std::uint32_t count_ = 0;
};

}  // namespace leonida
