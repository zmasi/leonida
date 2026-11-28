// The measurement behind LEONIDA_COMPILE_GATE=auto (compile_gate_policy.h): whether this driver holds a queue
// submission while a pipeline create is in it. Run once per driver at device creation, before the title has the
// device, on the device's own queue: a worker thread creates the slow compute pipeline of
// compile_gate_policy::slow_compute_spirv (no pipeline cache, a new seed each time), and this thread submits an empty
// command buffer and waits for its fence over and over until the create returns. A create too fast to tell is tried
// again larger, up to kMaxTrials. Every failure is a classic verdict; the caller does not cache those.
#pragma once

#include <vulkan/vulkan.h>

#include "compile_gate_policy.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace leonida::compile_gate_calibration {

inline constexpr std::uint32_t kFirstStatements = 1000;
inline constexpr std::uint32_t kMostStatements = 32000;
inline constexpr unsigned kMaxTrials = 4;
inline constexpr double kStartTrialsWithinMs = 6000;   // no new trial after this long
inline constexpr std::uint64_t kFenceTimeoutNs = 10'000'000'000ull;
// After a wait that failed, how long the fence gets before the fence and command pool are destroyed: they must not be
// destroyed while the submission is pending. Still not signalled after this, or the device lost: they are left alive.
inline constexpr std::uint64_t kDrainTimeoutNs = 60'000'000'000ull;
inline constexpr std::size_t kMostSamples = 4000;      // per trial; the create may outlast them

struct Result {
    bool ok = false;           // false: a Vulkan call failed (`error` says which); the verdict is then unclear
    std::string error;
    compile_gate_policy::Verdict verdict;
    unsigned trials = 0;
    std::uint32_t statements = 0;  // of the last trial
    double total_ms = 0;
};

namespace detail {

struct Fns {
    PFN_vkCreateDescriptorSetLayout create_set_layout = nullptr;
    PFN_vkDestroyDescriptorSetLayout destroy_set_layout = nullptr;
    PFN_vkCreatePipelineLayout create_layout = nullptr;
    PFN_vkDestroyPipelineLayout destroy_layout = nullptr;
    PFN_vkCreateShaderModule create_module = nullptr;
    PFN_vkDestroyShaderModule destroy_module = nullptr;
    PFN_vkCreateComputePipelines create_compute = nullptr;
    PFN_vkDestroyPipeline destroy_pipeline = nullptr;
    PFN_vkCreateCommandPool create_pool = nullptr;
    PFN_vkDestroyCommandPool destroy_pool = nullptr;
    PFN_vkAllocateCommandBuffers allocate_cbs = nullptr;
    PFN_vkBeginCommandBuffer begin_cb = nullptr;
    PFN_vkEndCommandBuffer end_cb = nullptr;
    PFN_vkCreateFence create_fence = nullptr;
    PFN_vkDestroyFence destroy_fence = nullptr;
    PFN_vkResetFences reset_fences = nullptr;
    PFN_vkWaitForFences wait_fences = nullptr;
    PFN_vkQueueSubmit queue_submit = nullptr;

    bool load(PFN_vkGetDeviceProcAddr get, VkDevice device) {
        const auto fn = [&](auto& out, const char* name) {
            out = reinterpret_cast<std::remove_reference_t<decltype(out)>>(get(device, name));
            return out != nullptr;
        };
        return fn(create_set_layout, "vkCreateDescriptorSetLayout") &&
               fn(destroy_set_layout, "vkDestroyDescriptorSetLayout") &&
               fn(create_layout, "vkCreatePipelineLayout") && fn(destroy_layout, "vkDestroyPipelineLayout") &&
               fn(create_module, "vkCreateShaderModule") && fn(destroy_module, "vkDestroyShaderModule") &&
               fn(create_compute, "vkCreateComputePipelines") && fn(destroy_pipeline, "vkDestroyPipeline") &&
               fn(create_pool, "vkCreateCommandPool") && fn(destroy_pool, "vkDestroyCommandPool") &&
               fn(allocate_cbs, "vkAllocateCommandBuffers") && fn(begin_cb, "vkBeginCommandBuffer") &&
               fn(end_cb, "vkEndCommandBuffer") && fn(create_fence, "vkCreateFence") &&
               fn(destroy_fence, "vkDestroyFence") && fn(reset_fences, "vkResetFences") &&
               fn(wait_fences, "vkWaitForFences") && fn(queue_submit, "vkQueueSubmit");
    }
};

using clock = std::chrono::steady_clock;
inline double ms_between(clock::time_point a, clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

}  // namespace detail

// Runs the calibration on `queue` (of `family`). Nothing else may use the queue meanwhile. `seed` varies the program
// (the caller passes something new each run, so the driver's own shader cache never answers). Progress goes to
// stderr with `tag` in front.
inline Result calibrate(PFN_vkGetDeviceProcAddr get, VkDevice device, VkQueue queue, std::uint32_t family,
                        std::uint32_t seed, const char* tag = "[rt] compile-gate:") {
    namespace policy = compile_gate_policy;
    Result result;
    const auto began = detail::clock::now();
    detail::Fns vk;
    if (!get || !device || !queue || !vk.load(get, device)) {
        result.error = "the device's entry points are missing";
        return result;
    }
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool keep_queue_objects = false;  // a submission may still be pending: never destroy its fence or command pool
    const auto cleanup = [&] {
        if (fence && !keep_queue_objects) vk.destroy_fence(device, fence, nullptr);
        if (pool && !keep_queue_objects) vk.destroy_pool(device, pool, nullptr);
        if (layout) vk.destroy_layout(device, layout, nullptr);
        if (set_layout) vk.destroy_set_layout(device, set_layout, nullptr);
    };
    const auto fail = [&](const char* what, VkResult r) {
        char text[120];
        std::snprintf(text, sizeof text, "%s failed (VkResult %d)", what, int(r));
        result.error = text;
        cleanup();
        result.total_ms = detail::ms_between(began, detail::clock::now());
        return result;
    };

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = 1;
    set_info.pBindings = &binding;
    if (const VkResult r = vk.create_set_layout(device, &set_info, nullptr, &set_layout); r != VK_SUCCESS)
        return fail("vkCreateDescriptorSetLayout", r);
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &set_layout;
    if (const VkResult r = vk.create_layout(device, &layout_info, nullptr, &layout); r != VK_SUCCESS)
        return fail("vkCreatePipelineLayout", r);
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.queueFamilyIndex = family;
    if (const VkResult r = vk.create_pool(device, &pool_info, nullptr, &pool); r != VK_SUCCESS)
        return fail("vkCreateCommandPool", r);
    VkCommandBufferAllocateInfo cb_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cb_info.commandPool = pool;
    cb_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb_info.commandBufferCount = 1;
    if (const VkResult r = vk.allocate_cbs(device, &cb_info, &cb); r != VK_SUCCESS)
        return fail("vkAllocateCommandBuffers", r);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if (const VkResult r = vk.begin_cb(cb, &begin); r != VK_SUCCESS) return fail("vkBeginCommandBuffer", r);
    if (const VkResult r = vk.end_cb(cb); r != VK_SUCCESS) return fail("vkEndCommandBuffer", r);
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (const VkResult r = vk.create_fence(device, &fence_info, nullptr, &fence); r != VK_SUCCESS)
        return fail("vkCreateFence", r);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;

    std::uint32_t statements = kFirstStatements;
    bool retried_unclear = false;
    for (unsigned trial = 0; trial < kMaxTrials; ++trial) {
        if (trial && detail::ms_between(began, detail::clock::now()) > kStartTrialsWithinMs) break;
        const std::vector<std::uint32_t> words = policy::slow_compute_spirv(statements);
        VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        module_info.codeSize = words.size() * sizeof(std::uint32_t);
        module_info.pCode = words.data();
        VkShaderModule module = VK_NULL_HANDLE;
        if (const VkResult r = vk.create_module(device, &module_info, nullptr, &module); r != VK_SUCCESS)
            return fail("vkCreateShaderModule", r);
        const std::uint32_t trial_seed = seed + trial;
        VkSpecializationMapEntry entry{0, 0, sizeof(std::uint32_t)};
        VkSpecializationInfo spec{1, &entry, sizeof trial_seed, &trial_seed};
        VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module = module;
        info.stage.pName = "main";
        info.stage.pSpecializationInfo = &spec;
        info.layout = layout;

        std::atomic<bool> entered{false}, left{false};
        detail::clock::time_point create_begin{}, create_end{};
        VkResult create_result = VK_SUCCESS;
        VkPipeline pipeline = VK_NULL_HANDLE;
        std::thread worker([&] {
            create_begin = detail::clock::now();
            entered.store(true, std::memory_order_release);
            create_result = vk.create_compute(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
            create_end = detail::clock::now();
            left.store(true, std::memory_order_release);
        });
        while (!entered.load(std::memory_order_acquire)) std::this_thread::yield();
        const detail::clock::time_point start_mark = create_begin;
        policy::Trial t;
        VkResult submit_result = VK_SUCCESS;
        bool in_flight = false;  // submitted, its fence not yet seen signalled
        // Let the create get into the driver before the first submission.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        while (!left.load(std::memory_order_acquire) && t.samples.size() < kMostSamples) {
            const auto s0 = detail::clock::now();
            submit_result = vk.queue_submit(queue, 1, &submit, fence);
            if (submit_result != VK_SUCCESS) break;
            in_flight = true;
            submit_result = vk.wait_fences(device, 1, &fence, VK_TRUE, kFenceTimeoutNs);
            const auto s1 = detail::clock::now();
            if (submit_result != VK_SUCCESS) break;
            in_flight = false;
            submit_result = vk.reset_fences(device, 1, &fence);
            if (submit_result != VK_SUCCESS) break;
            t.samples.push_back({detail::ms_between(start_mark, s0), detail::ms_between(start_mark, s1)});
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        worker.join();
        if (pipeline) vk.destroy_pipeline(device, pipeline, nullptr);
        vk.destroy_module(device, module, nullptr);
        if (submit_result != VK_SUCCESS) {
            // A wait that timed out leaves the submission pending: the fence and the command buffer's pool are
            // destroyed only once its fence signals. On a lost device, or if it still has not, they are left alive
            // (a small leak, on a path that ends in the classic gate anyway).
            if (submit_result == VK_ERROR_DEVICE_LOST) keep_queue_objects = true;
            else if (in_flight && vk.wait_fences(device, 1, &fence, VK_TRUE, kDrainTimeoutNs) != VK_SUCCESS)
                keep_queue_objects = true;
            return fail("an empty submission", submit_result);
        }
        if (create_result != VK_SUCCESS) return fail("vkCreateComputePipelines", create_result);
        t.create_ms = detail::ms_between(create_begin, create_end);
        result.verdict = policy::judge(t);
        result.trials = trial + 1;
        result.statements = statements;
        std::fprintf(stderr, "%s calibration trial %u: %u statements, create %.0f ms, %zu submissions, %s\n", tag,
                     trial + 1, unsigned(statements), t.create_ms, t.samples.size(),
                     policy::outcome_name(result.verdict.outcome));
        if (result.verdict.outcome == policy::Outcome::adaptive || result.verdict.outcome == policy::Outcome::holds)
            break;
        if (result.verdict.outcome == policy::Outcome::too_fast) {
            if (statements >= kMostStatements) break;
            statements = policy::next_statements(statements, t.create_ms, kMostStatements);
        } else {  // unclear: once more at the same size (another process may have used the GPU), then give up
            if (retried_unclear) break;
            retried_unclear = true;
        }
    }
    cleanup();
    result.ok = true;
    result.total_ms = detail::ms_between(began, detail::clock::now());
    return result;
}

}  // namespace leonida::compile_gate_calibration
