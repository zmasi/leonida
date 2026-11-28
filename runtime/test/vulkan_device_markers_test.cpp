// The AMD breadcrumbs of the device-fault report (src/vulkan_device_markers.h, LEONIDA_DEVICE_MARKERS=1), on the
// CPU: seq numbering, the marker writes a recorded dispatch makes, the history ring, and which dispatch the report
// calls the likely hang for given marker values. The driver is faked: its marker writes are logged, and a "GPU"
// applies a prefix of them to the mapped words, as a queue that stopped at that point would have. No GPU, no game
// data.
#include "../src/vulkan_device_fault.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <vector>

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

using namespace leonida;

namespace {

template<typename Handle>
Handle fake_handle(std::uintptr_t value) {
    if constexpr (std::is_pointer_v<Handle>) return reinterpret_cast<Handle>(value);
    else return Handle(value);
}

struct Write {
    VkCommandBuffer command_buffer;
    std::uint64_t stage;
    VkDeviceSize offset;
    std::uint32_t marker;
    bool synchronization2;
};
std::vector<Write> writes;
struct Barrier {
    VkCommandBuffer command_buffer;
    VkDeviceSize offset;
    std::size_t preceding_writes;
    bool slot_dependency;
};
std::vector<Barrier> barriers;
std::uint32_t words[2 * DeviceMarkers::max_queues];
bool mapped = false, unmapped = false, freed = false;

VKAPI_ATTR VkResult VKAPI_CALL create_buffer(VkDevice, const VkBufferCreateInfo* info, const VkAllocationCallbacks*,
                                             VkBuffer* buffer) {
    if (info->size < sizeof(words)) return VK_ERROR_UNKNOWN;
    *buffer = fake_handle<VkBuffer>(0x100);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL requirements(VkDevice, VkBuffer, VkMemoryRequirements* out) {
    *out = {sizeof(words), 4, 0x6};
}
VKAPI_ATTR VkResult VKAPI_CALL allocate(VkDevice, const VkMemoryAllocateInfo* info, const VkAllocationCallbacks*,
                                        VkDeviceMemory* memory) {
    if (info->memoryTypeIndex != 2) return VK_ERROR_UNKNOWN;  // host-visible, coherent, not device-local
    *memory = fake_handle<VkDeviceMemory>(0x200);
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL bind(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL map(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkMemoryMapFlags, void** out) {
    std::memset(words, 0xcd, sizeof(words));
    *out = words;
    mapped = true;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL unmap(VkDevice, VkDeviceMemory) { unmapped = true; }
VKAPI_ATTR void VKAPI_CALL destroy_buffer(VkDevice, VkBuffer, const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL free_memory(VkDevice, VkDeviceMemory, const VkAllocationCallbacks*) { freed = true; }
VKAPI_ATTR void VKAPI_CALL write_marker2(VkCommandBuffer command_buffer, VkPipelineStageFlags2 stage, VkBuffer,
                                         VkDeviceSize offset, std::uint32_t marker) {
    writes.push_back({command_buffer, stage, offset, marker, true});
}
VKAPI_ATTR void VKAPI_CALL write_marker(VkCommandBuffer command_buffer, VkPipelineStageFlagBits stage, VkBuffer,
                                        VkDeviceSize offset, std::uint32_t marker) {
    writes.push_back({command_buffer, static_cast<std::uint64_t>(stage), offset, marker, false});
}

VKAPI_ATTR void VKAPI_CALL pipeline_barrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst,
                                           VkDependencyFlags flags, std::uint32_t memory_count, const VkMemoryBarrier*,
                                           std::uint32_t buffer_count, const VkBufferMemoryBarrier* buffer,
                                           std::uint32_t image_count, const VkImageMemoryBarrier*) {
    const auto* slot = buffer_count == 1 ? buffer : nullptr;
    const bool slot_dependency = src == VK_PIPELINE_STAGE_TRANSFER_BIT && dst == VK_PIPELINE_STAGE_TRANSFER_BIT &&
        !flags && !memory_count && !image_count && slot && slot->srcAccessMask == VK_ACCESS_TRANSFER_WRITE_BIT &&
        slot->dstAccessMask == VK_ACCESS_TRANSFER_WRITE_BIT && slot->srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED &&
        slot->dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED && slot->buffer == fake_handle<VkBuffer>(0x100) &&
        slot->size == 8;
    barriers.push_back({cb, slot ? slot->offset : 0, writes.size(), slot_dependency});
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL get_proc(VkDevice, const char* name) {
#define LEONIDA_FAKE(entry, fn) if (!std::strcmp(name, entry)) return reinterpret_cast<PFN_vkVoidFunction>(fn)
    LEONIDA_FAKE("vkCreateBuffer", create_buffer);
    LEONIDA_FAKE("vkGetBufferMemoryRequirements", requirements);
    LEONIDA_FAKE("vkAllocateMemory", allocate);
    LEONIDA_FAKE("vkBindBufferMemory", bind);
    LEONIDA_FAKE("vkMapMemory", map);
    LEONIDA_FAKE("vkUnmapMemory", unmap);
    LEONIDA_FAKE("vkDestroyBuffer", destroy_buffer);
    LEONIDA_FAKE("vkFreeMemory", free_memory);
    LEONIDA_FAKE("vkCmdWriteBufferMarkerAMD", write_marker);
    LEONIDA_FAKE("vkCmdWriteBufferMarker2AMD", write_marker2);
    LEONIDA_FAKE("vkCmdPipelineBarrier", pipeline_barrier);
#undef LEONIDA_FAKE
    return nullptr;
}

VkPhysicalDeviceMemoryProperties memory_types() {
    VkPhysicalDeviceMemoryProperties memory{};
    memory.memoryTypeCount = 4;
    memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    memory.memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    memory.memoryTypes[2].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    memory.memoryTypes[3].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                          VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    return memory;
}

// The "GPU": runs the first `count` logged marker writes, in order.
void execute(std::size_t count) {
    for (std::size_t i = 0; i < count && i < writes.size(); ++i)
        words[writes[i].offset / 4] = writes[i].marker;
}

struct History {  // a plain list in MarkerHistory::for_each order, for judge_markers
    std::vector<MarkedDispatch> entries;
    template<typename Visit>
    void for_each(Visit&& visit) const {
        for (const auto& d : entries) visit(d);
    }
};

int pure_logic() {
    // Serial order on the 32 bits the GPU writes, across the wrap.
    EXPECT(marker_after(5, 4) && !marker_after(4, 4) && !marker_after(3, 4));
    EXPECT(marker_after(0x100000002ull, 0xfffffffeu));   // seq 2^32 + 2 is after marker 2^32 - 2
    EXPECT(!marker_after(0xfffffffeull, 0x00000003u));   // and 2^32 - 2 is before a wrapped 3

    // The history ring keeps the newest `capacity`, oldest first.
    MarkerHistory ring;
    EXPECT(ring.size() == 0);
    for (std::uint64_t seq = 1; seq <= MarkerHistory::capacity + 5; ++seq) ring.push({seq});
    EXPECT(ring.size() == MarkerHistory::capacity);
    std::uint64_t expect = 6, visited = 0;
    bool ordered = true;
    ring.for_each([&](const MarkedDispatch& d) { ordered &= d.seq == expect++; ++visited; });
    EXPECT(ordered && visited == MarkerHistory::capacity);

    // Two queues interleaved: compute has 1 3 5 6 8, direct has 2 4 7.
    const VkQueue compute = fake_handle<VkQueue>(0x10), direct = fake_handle<VkQueue>(0x20);
    History history;
    for (std::uint64_t seq : {1, 3, 5, 6, 8}) history.entries.push_back({seq, 1, compute, "compute"});
    for (std::uint64_t seq : {2, 4, 7}) history.entries.push_back({seq, 2, direct, "direct"});
    std::sort(history.entries.begin(), history.entries.end(),
              [](const auto& a, const auto& b) { return a.seq < b.seq; });

    // compute finished 3, started 6: 5 and 6 run, 5 is the likely hang; 8 was submitted and not started.
    auto r = judge_markers(compute, {6, 3}, history);
    EXPECT(!r.idle() && r.unfinished == std::vector<std::uint64_t>({5, 6}) && r.likely_hang == 5 && !r.missing);
    EXPECT(r.next_queued == 0 && r.queue_kind && !std::strcmp(r.queue_kind, "compute"));
    // One dispatch started and not finished.
    r = judge_markers(compute, {5, 3}, history);
    EXPECT(r.unfinished == std::vector<std::uint64_t>({5}) && r.likely_hang == 5);
    // direct idle after 4: nothing reported as hung, 7 is the next one it had queued.
    r = judge_markers(direct, {4, 4}, history);
    EXPECT(r.idle() && r.unfinished.empty() && !r.likely_hang && r.next_queued == 7 && !r.missing);
    // Everything done.
    r = judge_markers(direct, {7, 7}, history);
    EXPECT(r.idle() && !r.likely_hang && !r.next_queued);
    // Started past the history's newest dispatch of the queue: nothing to name.
    r = judge_markers(compute, {9, 8}, history);
    EXPECT(r.missing && !r.likely_hang && r.unfinished.empty());
    // A started word behind the finished one (memory a reset scrambled) is read as idle, not as a hang.
    r = judge_markers(compute, {1, 5}, history);
    EXPECT(r.idle() && !r.likely_hang && r.next_queued == 6);
    // No marker yet: every dispatch of the queue is queued, none hung.
    r = judge_markers(compute, {0, 0}, history);
    EXPECT(r.idle() && r.next_queued == 1 && !r.likely_hang);
    // A queue the history does not know.
    r = judge_markers(fake_handle<VkQueue>(0x30), {0, 0}, history);
    EXPECT(!r.queue_kind && !r.next_queued);

    // Across the 32-bit wrap: seqs 2^32 - 1, 2^32, 2^32 + 1 on one queue; markers hold their low 32 bits.
    History wrapped;
    for (std::uint64_t seq : {0xffffffffull, 0x100000000ull, 0x100000001ull})
        wrapped.entries.push_back({seq, 1, compute, "compute"});
    r = judge_markers(compute, {0x00000001u, 0xffffffffu}, wrapped);
    EXPECT(r.unfinished == std::vector<std::uint64_t>({0x100000000ull, 0x100000001ull}) &&
           r.likely_hang == 0x100000000ull);

    // Memory type: host-visible and coherent, system memory first.
    const auto memory = memory_types();
    EXPECT(DeviceMarkers::memory_type(memory, 0xf) == 2);
    EXPECT(DeviceMarkers::memory_type(memory, 0x3) == 1);  // only device-local ones allowed: the BAR one
    EXPECT(DeviceMarkers::memory_type(memory, 0x1) == UINT32_MAX);
    return 0;
}

// Recording through CheckpointRecorder with the markers on, then the report's verdict for a stopped "GPU".
int recorded(bool synchronization2) {
    writes.clear();
    barriers.clear();
    mapped = unmapped = freed = false;
    const auto memory = memory_types();
    DeviceFaultExtensions enabled;
    enabled.markers = true;
    const VkQueue compute = fake_handle<VkQueue>(0x10), direct = fake_handle<VkQueue>(0x20);
    const VkCommandBuffer cb = fake_handle<VkCommandBuffer>(0x30);
    {
        DeviceFaultDiagnostics diagnostics(get_proc, VK_NULL_HANDLE, enabled, &memory, synchronization2);
        EXPECT(diagnostics.markers_on() && mapped);
        diagnostics.add_queue(compute);
        diagnostics.add_queue(direct);
        diagnostics.add_queue(compute);  // once is enough
        EXPECT(words[0] == 0 && words[3] == 0);  // the slots start at no dispatch
        diagnostics.pipeline_names[4] = {"cs_example_a"};
        diagnostics.pipeline_names[5] = {"cs_example_b"};
        const std::array<std::uint32_t, 32> roots{};

        // Submission 1 on the compute queue: seqs 1..3. Submission 2 on the direct queue: 4..5.
        auto first = diagnostics.begin_submission(diagnostics.next_execute_call(), compute, "compute", 1);
        EXPECT(first.markers && first.markers.offset == 0);
        const CheckpointRecorder a{nullptr, first, diagnostics.pipeline_names, 0};
        for (unsigned i = 0; i < 3; ++i) {
            a.dispatch(cb, 10 + i, VK_NULL_HANDLE, i == 1 ? 5 : 4, roots, {1, 1, 1});
            a.dispatched(cb);
        }
        a.dispatched(cb);  // nothing open: no write
        EXPECT(writes.size() == 6);
        auto second = diagnostics.begin_submission(diagnostics.next_execute_call(), direct, "direct", 1);
        EXPECT(second.markers && second.markers.offset == 8);
        const CheckpointRecorder b{nullptr, second, diagnostics.pipeline_names, 0};
        for (unsigned i = 0; i < 2; ++i) {
            b.dispatch(cb, 20 + i, VK_NULL_HANDLE, 4, roots, {}, true, 1, 0, 0, 0, 1, 12);
            b.dispatched(cb);
        }
        EXPECT(writes.size() == 10);
        EXPECT(barriers.size() == writes.size() / 2);
        // Each dispatch: a slot dependency, then started (top of pipe, slot +0) and finished (bottom, slot +4).
        for (std::size_t i = 0; i < writes.size(); ++i) {
            const Write& w = writes[i];
            const std::uint32_t seq = std::uint32_t(i / 2 + 1);
            const bool top = i % 2 == 0;
            const VkDeviceSize slot = seq <= 3 ? 0 : 8;
            const std::uint64_t top_stage = synchronization2 ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT
                                                             : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            const std::uint64_t bottom_stage = synchronization2 ? VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT
                                                                : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
            EXPECT(w.marker == seq && w.command_buffer == cb && w.offset == slot + (top ? 0 : 4) &&
                   w.stage == (top ? top_stage : bottom_stage) && w.synchronization2 == synchronization2);
            if (top) {
                const Barrier& barrier = barriers[i / 2];
                EXPECT(barrier.slot_dependency && barrier.preceding_writes == i &&
                       barrier.command_buffer == w.command_buffer && barrier.offset == slot);
            }
        }
        // The report's list numbers them the same way.
        EXPECT(first.dispatches.size() == 3 && first.dispatches[0].seq == 1 && first.dispatches[2].seq == 3);
        EXPECT(second.dispatches.size() == 2 && second.dispatches[1].seq == 5);
        EXPECT(first.marked.size() == 3 && first.marked[1].shader && !std::strcmp(first.marked[1].shader, "cs_example_b"));
        diagnostics.publish(std::move(first));
        diagnostics.publish(std::move(second));

        // Nothing has run: no queue is reported as hung.
        EXPECT(diagnostics.report_markers(false).empty());
        // The compute queue stopped inside seq 2 (its started write ran, its finished one did not); the direct queue
        // ran seq 4 completely and stopped inside 5.
        execute(3);                          // seq 1 started and finished, seq 2 started
        words[2] = 4; words[3] = 4;          // direct: seq 4 done
        words[2] = 5;                        // seq 5 started
        auto hangs = diagnostics.report_markers(true);
        EXPECT(hangs == std::vector<std::uint64_t>({2, 5}));
        diagnostics.report_stall("synthetic marker check", 2, 1);  // the list marks seq 2 and 5 as likely hangs
        // Everything finished: no hang.
        execute(writes.size());
        EXPECT(diagnostics.report_markers(false).empty());
        const MarkerSlot compute_slot = diagnostics.marker_slot(compute), direct_slot = diagnostics.marker_slot(direct);
        EXPECT(compute_slot.started == 3 && compute_slot.finished == 3);
        EXPECT(direct_slot.started == 5 && direct_slot.finished == 5);
    }
    EXPECT(unmapped && freed);
    return 0;
}

// Without LEONIDA_DEVICE_MARKERS the extension is not asked for, and with it only where the device has it.
int extension_request() {
    const VkExtensionProperties with[] = {{VK_AMD_BUFFER_MARKER_EXTENSION_NAME, 1}};
    VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    const auto no_features = [](VkPhysicalDevice, VkPhysicalDeviceFeatures2*) {};
    DeviceFaultFeatures features;
    std::vector<const char*> extensions;
    DeviceFaultRequest request;
    EXPECT(!features.enable(VK_NULL_HANDLE, no_features, with, extensions, device, request).markers);
    EXPECT(extensions.empty());
    request.markers = true;
    EXPECT(!features.enable(VK_NULL_HANDLE, no_features, {}, extensions, device, request).markers);
    EXPECT(extensions.empty());
    EXPECT(features.enable(VK_NULL_HANDLE, no_features, with, extensions, device, request).markers);
    EXPECT(extensions.size() == 1 && !std::strcmp(extensions[0], VK_AMD_BUFFER_MARKER_EXTENSION_NAME));

    // Markers enabled but no memory types given: off, the rest of the report unchanged.
    DeviceFaultExtensions enabled;
    enabled.markers = true;
    DeviceFaultDiagnostics diagnostics(get_proc, VK_NULL_HANDLE, enabled);
    EXPECT(!diagnostics.markers_on());
    auto submission = diagnostics.begin_submission(diagnostics.next_execute_call(), VK_NULL_HANDLE, "compute", 1);
    EXPECT(!submission.markers);
    writes.clear();
    barriers.clear();
    const CheckpointRecorder recorder{nullptr, submission, diagnostics.pipeline_names, 0};
    recorder.dispatch(VK_NULL_HANDLE, 0, VK_NULL_HANDLE, 0, {}, {1, 1, 1});
    recorder.dispatched(VK_NULL_HANDLE);
    EXPECT(writes.empty() && barriers.empty() && submission.dispatches[0].seq == 1 && submission.marked.empty());
    EXPECT(diagnostics.report_markers(true).empty());
    return 0;
}

}  // namespace

int main() {
    if (int failed = pure_logic()) return failed;
    if (int failed = recorded(true)) return failed;
    if (int failed = recorded(false)) return failed;
    if (int failed = extension_request()) return failed;
    std::puts("vulkan_device_markers: ok");
    return 0;
}
