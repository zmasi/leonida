// The AMD breadcrumbs of the device-fault report (src/vulkan_device_markers.h, LEONIDA_DEVICE_MARKERS=1) on a real
// device: dispatches recorded through CheckpointRecorder with the markers on, submitted and waited for, leave their
// queue's started and finished markers at the last dispatch's seq, and the report calls none of them hung. Each
// round spans multiple command buffers and submissions, with no wait between them. The shader is an empty compute
// program; nothing is meant to fault or hang. SKIP (77) without a Vulkan device or without VK_AMD_buffer_marker.
// LEONIDA_VK_VALIDATION=1 enables synchronization validation (any validation error fails the test).
#include "vk_test_device.h"
#include "../src/vulkan_device_fault.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

namespace {

using namespace leonida;

// OpCapability Shader; OpMemoryModel Logical GLSL450; OpEntryPoint GLCompute %1 "main"; LocalSize 1 1 1;
// void main() {}
constexpr std::uint32_t empty_compute[] = {
    0x07230203u, 0x00010000u, 0u, 5u, 0u,
    (2u << 16) | 17u, 1u,                                   // OpCapability Shader
    (3u << 16) | 14u, 0u, 1u,                               // OpMemoryModel Logical GLSL450
    (5u << 16) | 15u, 5u, 1u, 0x6e69616du, 0u,              // OpEntryPoint GLCompute %1 "main"
    (6u << 16) | 16u, 1u, 17u, 1u, 1u, 1u,                  // OpExecutionMode %1 LocalSize 1 1 1
    (2u << 16) | 19u, 2u,                                   // %2 = OpTypeVoid
    (3u << 16) | 33u, 3u, 2u,                               // %3 = OpTypeFunction %2
    (5u << 16) | 54u, 2u, 1u, 0u, 3u,                       // %1 = OpFunction %2 None %3
    (2u << 16) | 248u, 4u,                                  // %4 = OpLabel
    (1u << 16) | 253u,                                      // OpReturn
    (1u << 16) | 56u,                                       // OpFunctionEnd
};

struct Pipeline {
    const vktest::Device& d;
    VkShaderModule module = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;

    explicit Pipeline(const vktest::Device& device) : d(device) {
        VkShaderModuleCreateInfo code{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        code.codeSize = sizeof(empty_compute);
        code.pCode = empty_compute;
        if (d.fn<PFN_vkCreateShaderModule>("vkCreateShaderModule")(d.device, &code, nullptr, &module) != VK_SUCCESS)
            return;
        VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        if (d.fn<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(d.device, &layout_info, nullptr, &layout) !=
            VK_SUCCESS)
            return;
        VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module = module;
        info.stage.pName = "main";
        info.layout = layout;
        d.fn<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(d.device, VK_NULL_HANDLE, 1, &info, nullptr,
                                                                       &pipeline);
    }
    ~Pipeline() {
        if (pipeline) d.fn<PFN_vkDestroyPipeline>("vkDestroyPipeline")(d.device, pipeline, nullptr);
        if (layout) d.fn<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(d.device, layout, nullptr);
        if (module) d.fn<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(d.device, module, nullptr);
    }
};

struct MarkerCommands {
    const vktest::Device& d;
    VkCommandPool pool = VK_NULL_HANDLE;
    std::array<VkCommandBuffer, 3> buffers{};

    bool allocate() {
        VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_info.queueFamilyIndex = d.family;
        if (d.fn<PFN_vkCreateCommandPool>("vkCreateCommandPool")(d.device, &pool_info, nullptr, &pool) != VK_SUCCESS)
            return false;
        VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        info.commandPool = pool;
        info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        info.commandBufferCount = static_cast<std::uint32_t>(buffers.size());
        return d.fn<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(d.device, &info, buffers.data()) ==
               VK_SUCCESS;
    }

    ~MarkerCommands() {
        if (pool) d.fn<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(d.device, pool, nullptr);
    }
};

int run(vktest::Device& d, bool synchronization2) {
    VkPhysicalDeviceMemoryProperties memory{};
    d.instance_fn<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(d.physical, &memory);
    DeviceFaultExtensions enabled;
    enabled.markers = true;
    // Use the runtime's synchronization2 path and also exercise the legacy marker command on the GPU.
    DeviceFaultDiagnostics diagnostics(d.device_proc, d.device, enabled, &memory, synchronization2);
    EXPECT(diagnostics.markers_on());
    diagnostics.add_queue(d.queue);
    EXPECT(diagnostics.marker_slot(d.queue).started == 0 && diagnostics.marker_slot(d.queue).finished == 0);

    Pipeline pipeline(d);
    EXPECT(pipeline.pipeline);
    const auto bind = d.fn<PFN_vkCmdBindPipeline>("vkCmdBindPipeline");
    const auto dispatch = d.fn<PFN_vkCmdDispatch>("vkCmdDispatch");
    const auto pipeline_id = reinterpret_cast<std::uintptr_t>(&pipeline);
    diagnostics.pipeline_names[pipeline_id] = {"cs_empty_test"};
    const std::array<std::uint32_t, 32> roots{};

    std::uint64_t last_seq = 0;
    const bool marker2 = synchronization2 && d.device_proc(d.device, "vkCmdWriteBufferMarker2AMD");
    for (unsigned round = 0; round < 2; ++round) {
        const unsigned dispatches = round == 0 ? 5 : 3;
        auto submission = diagnostics.begin_submission(diagnostics.next_execute_call(), d.queue, "direct", 1);
        EXPECT(submission.markers);
        const CheckpointRecorder recorder{nullptr, submission, diagnostics.pipeline_names, 0};
        EXPECT(bool(submission.markers.write2) == marker2);
        MarkerCommands commands{d};
        EXPECT(commands.allocate());
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        unsigned i = 0;
        for (const VkCommandBuffer cb : commands.buffers) {
            EXPECT(d.fn<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(cb, &begin) == VK_SUCCESS);
            const bool last = cb == commands.buffers.back();
            const unsigned end = i + (last || round ? 1u : 2u);
            for (; i < end; ++i) {
                recorder.dispatch(cb, i, pipeline.pipeline, pipeline_id, roots, {1 + i, 1, 1});
                bind(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.pipeline);
                dispatch(cb, 1 + i, 1, 1);
                recorder.dispatched(cb);
            }
            if (last) {
                // The test reads both words on the host. Publish them only after the last marker; this
                // barrier does not order the marker writes against one another.
                VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                d.fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(
                    cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                    0, 1, &host, 0, nullptr, 0, nullptr);
            }
            EXPECT(d.fn<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(cb) == VK_SUCCESS);
        }
        // Two command buffers in the first submit, then one in the second. No semaphore/fence or wait orders them.
        VkSubmitInfo submits[] = {{VK_STRUCTURE_TYPE_SUBMIT_INFO}, {VK_STRUCTURE_TYPE_SUBMIT_INFO}};
        submits[0].commandBufferCount = 2;
        submits[0].pCommandBuffers = commands.buffers.data();
        submits[1].commandBufferCount = 1;
        submits[1].pCommandBuffers = &commands.buffers.back();
        EXPECT(d.fn<PFN_vkQueueSubmit>("vkQueueSubmit")(d.queue, 2, submits, VK_NULL_HANDLE) == VK_SUCCESS);
        EXPECT(d.fn<PFN_vkQueueWaitIdle>("vkQueueWaitIdle")(d.queue) == VK_SUCCESS);
        EXPECT(submission.dispatches.size() == dispatches);
        EXPECT(submission.dispatches.front().seq == last_seq + 1);
        last_seq = submission.dispatches.back().seq;
        EXPECT(last_seq == (round == 0 ? 5u : 8u));
        diagnostics.publish(std::move(submission));
        // The test's host barrier and vkQueueWaitIdle have completed.
        const MarkerSlot slot = diagnostics.marker_slot(d.queue);
        std::printf("%s round %u: started=%u finished=%u, last seq %llu\n",
                    marker2 ? "marker2" : "legacy", round, slot.started, slot.finished,
                    static_cast<unsigned long long>(last_seq));
        EXPECT(slot.started == last_seq && slot.finished == last_seq);
        EXPECT(diagnostics.report_markers(false).empty());
    }
    return 0;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    vktest::Device d;
    d.request_buffer_marker = true;
    const auto status = d.initialize();
    if (status == vktest::DeviceStatus::unavailable) {
        std::printf("SKIP: no usable Vulkan device\n");
        return 77;
    }
    if (status != vktest::DeviceStatus::ready) return 1;
    if (!d.buffer_marker) {
        std::printf("SKIP: the device has no VK_AMD_buffer_marker\n");
        return 77;
    }
    int rc = run(d, true);
    if (!rc) rc = run(d, false);
    const unsigned errors = d.validation_errors();
    std::printf("Vulkan validation errors: %u%s\n", errors, d.validation ? "" : " (validation off)");
    return rc ? rc : errors ? 1 : 0;
}
