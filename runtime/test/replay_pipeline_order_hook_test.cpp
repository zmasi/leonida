// The request hook of ComputePipeline and GraphicsPipeline (RT-PIPELINE-PREWARM-ORDER phase 1, pipeline_order.h): it is
// called at every dispatch and draw that names the pipeline, whether the pipeline is built already, built on this very
// request, not built at all (skipped) or an unbound-slot variant, and it changes nothing prepare() returns. A hook that
// only saw the requests that had to build would miss exactly the pipelines that were ready in time. No GPU: pipelines
// and resources are fakes (the same shape as replay_pipeline_compiles_test.cpp).
#include "../src/vulkan_replay.h"

#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {
using namespace leonida;

int failures = 0;
void check(bool ok, const std::string& what) {
    if (ok) return;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++failures;
}

template<class T>
T handle(std::uintptr_t n) {
    return reinterpret_cast<T>(n);
}

constexpr ResourceId color_id = 1;

// What the hook was told, per pipeline: the variants, in order of the calls.
struct Seen {
    std::vector<std::uint32_t> variants;
};
std::map<int, Seen> seen;
struct Context {
    int id;
};
Context contexts[8] = {{0}, {1}, {2}, {3}, {4}, {5}, {6}, {7}};

void on_request(void* context, std::uint32_t variant) {
    auto& s = seen[static_cast<Context*>(context)->id];
    s.variants.push_back(variant);
}

VkPipeline lazy_build(void*) { return handle<VkPipeline>(0x5000); }

ReplayFunctions functions(std::map<std::uintptr_t, ComputePipeline>& compute,
                          std::map<std::uintptr_t, GraphicsPipeline>& graphics) {
    ReplayFunctions vk{};
    vk.begin_rendering = +[](VkCommandBuffer, const VkRenderingInfo*) {};
    vk.end_rendering = +[](VkCommandBuffer) {};
    vk.bind_pipeline = +[](VkCommandBuffer, VkPipelineBindPoint, VkPipeline) {};
    vk.push_constants = +[](VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, std::uint32_t, std::uint32_t,
                            const void*) {};
    vk.dispatch = +[](VkCommandBuffer, std::uint32_t, std::uint32_t, std::uint32_t) {};
    vk.set_viewport = +[](VkCommandBuffer, std::uint32_t, std::uint32_t, const VkViewport*) {};
    vk.set_scissor = +[](VkCommandBuffer, std::uint32_t, std::uint32_t, const VkRect2D*) {};
    vk.set_primitive_topology = +[](VkCommandBuffer, VkPrimitiveTopology) {};
    vk.set_primitive_restart_enable = +[](VkCommandBuffer, VkBool32) {};
    vk.set_stencil_reference = +[](VkCommandBuffer, VkStencilFaceFlags, std::uint32_t) {};
    vk.set_blend_constants = +[](VkCommandBuffer, const float*) {};
    vk.bind_index_buffer = +[](VkCommandBuffer, VkBuffer, VkDeviceSize, VkIndexType) {};
    vk.bind_vertex_buffers2 = +[](VkCommandBuffer, std::uint32_t, std::uint32_t, const VkBuffer*, const VkDeviceSize*,
                                  const VkDeviceSize*, const VkDeviceSize*) {};
    vk.draw = +[](VkCommandBuffer, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) {};
    vk.draw_indexed = +[](VkCommandBuffer, std::uint32_t, std::uint32_t, std::uint32_t, std::int32_t, std::uint32_t) {};
    vk.image_view = [](const ResourceBinding&, const VkImageSubresourceRange&) { return handle<VkImageView>(8); };
    vk.buffer_at = [](std::uint64_t, std::uint64_t&, VkBuffer& buffer, VkDeviceSize& offset) {
        buffer = handle<VkBuffer>(9);
        offset = 0;
        return true;
    };
    vk.graphics_pipeline = [&graphics](std::uintptr_t id) -> const GraphicsPipeline* {
        const auto it = graphics.find(id);
        return it == graphics.end() ? nullptr : &it->second;
    };
    vk.compute_pipeline = [&compute](std::uintptr_t id) -> const ComputePipeline* {
        const auto it = compute.find(id);
        return it == compute.end() ? nullptr : &it->second;
    };
    return vk;
}

const ResourceBinding* resolve(ResourceId id) {
    static const ResourceBinding color = [] {
        ResourceBinding b{};
        b.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
        b.format = VK_FORMAT_R8G8B8A8_UNORM;
        b.width = b.height = 16;
        b.image = handle<VkImage>(10);
        return b;
    }();
    return id == color_id ? &color : nullptr;
}

Draw draw_of(std::uintptr_t pipeline, int colors_bound) {
    Draw d{};
    d.pipeline = pipeline;
    d.topology = 4;
    d.count = 3;
    d.instances = 1;
    d.first = 1;
    auto views = std::make_shared<DrawViews>();
    for (int i = 0; i < colors_bound; ++i) views->render_targets[std::size_t(i)] = {color_id, 0, 0, 1};
    views->vertex_buffers[0].va = 0x1000;
    views->vertex_buffers[0].size = 64;
    d.views = views;
    return d;
}

GraphicsPipeline graphics_pipeline(int id, bool built) {
    GraphicsPipeline p{};
    p.layout = handle<VkPipelineLayout>(7);
    if (built) p.pipeline = handle<VkPipeline>(0x4000 + std::uintptr_t(id));
    p.color_formats.assign(2, VK_FORMAT_R8G8B8A8_UNORM);
    p.vertex_slots = 1;
    p.variant = [](std::uint32_t flags) { return handle<VkPipeline>(0x6000 + flags); };
    p.requested = &on_request;
    p.request_context = &contexts[id];
    return p;
}

}  // namespace

int main() {
    std::map<std::uintptr_t, ComputePipeline> compute;
    std::map<std::uintptr_t, GraphicsPipeline> graphics;

    // 1: compute, handle ready, no build function: the hook still runs.
    ComputePipeline ready{};
    ready.layout = handle<VkPipelineLayout>(7);
    ready.pipeline = handle<VkPipeline>(0x1000);
    ready.requested = &on_request;
    ready.request_context = &contexts[1];
    compute[1] = ready;
    // 2: compute, lazy, not built yet: it is built where prepare() meets it, and told all the same.
    ComputePipeline lazy{};
    lazy.layout = handle<VkPipelineLayout>(7);
    lazy.build = &lazy_build;
    lazy.requested = &on_request;
    lazy.request_context = &contexts[2];
    compute[2] = lazy;
    // 3: compute that is never built on request (skipped): a dispatch dropped, still a request.
    ComputePipeline skipped{};
    skipped.layout = handle<VkPipelineLayout>(7);
    skipped.skipped = true;
    skipped.build = [](void*) -> VkPipeline { return VK_NULL_HANDLE; };
    skipped.requested = &on_request;
    skipped.request_context = &contexts[3];
    compute[3] = skipped;
    // 4: compute with no hook at all (the log is off): nothing is called and nothing breaks.
    ComputePipeline plain{};
    plain.layout = handle<VkPipelineLayout>(7);
    plain.pipeline = handle<VkPipeline>(0x1001);
    compute[4] = plain;
    // 5: graphics built, both targets bound: variant 0.  6: graphics not built, slot 1 unbound: variant 2.
    graphics[5] = graphics_pipeline(5, true);
    graphics[6] = graphics_pipeline(6, false);

    CommandList list;
    list.dispatch({1, {}, 1, 1, 1});
    list.dispatch({2, {}, 1, 1, 1});
    list.dispatch({3, {}, 1, 1, 1});
    list.dispatch({4, {}, 1, 1, 1});
    list.dispatch({1, {}, 1, 1, 1});  // asked again: told again (the recorder decides what is new)
    list.draw(draw_of(5, 2));
    list.draw(draw_of(6, 1));
    list.draw(draw_of(6, 2));
    list.close();

    ReplayFunctions vk = functions(compute, graphics);
    PreparedList prepared;
    const ReplayResult result = prepare(list, resolve, vk, prepared);
    check(result == ReplayResult::success, "prepare succeeds");

    check(seen[1].variants == std::vector<std::uint32_t>({0, 0}), "a ready compute pipeline is told at each dispatch");
    check(seen[2].variants == std::vector<std::uint32_t>({0}), "a lazy compute pipeline is told once");
    check(seen[3].variants == std::vector<std::uint32_t>({0}), "a skipped compute pipeline is still a request");
    check(seen.count(4) == 0, "no hook, no call");
    check(seen[5].variants == std::vector<std::uint32_t>({0}), "a ready graphics pipeline is told, variant 0");
    check(seen[6].variants == std::vector<std::uint32_t>({2, 0}), "unbound slot 1 is variant 2, then both bound is 0");

    // What prepare() gives is what it gave without a hook.
    auto without_compute = compute;
    for (auto& entry : without_compute) {
        entry.second.requested = nullptr;
        entry.second.request_context = nullptr;
    }
    auto without_graphics = graphics;
    for (auto& entry : without_graphics) {
        entry.second.requested = nullptr;
        entry.second.request_context = nullptr;
    }
    seen.clear();
    ReplayFunctions plain_vk = functions(without_compute, without_graphics);
    PreparedList plain_prepared;
    const ReplayResult plain_result = prepare(list, resolve, plain_vk, plain_prepared);
    check(plain_result == result, "the same result without the hook");
    check(seen.empty(), "nobody is told when no pipeline has a hook");
    check(plain_prepared.commands.size() == prepared.commands.size() && plain_prepared.draw_count == prepared.draw_count,
          "the same commands without the hook");
    for (std::size_t i = 0; i < plain_prepared.draw_count && i < prepared.draw_count; ++i)
        check(plain_prepared.draws[i].pipeline == prepared.draws[i].pipeline, "the same pipeline for draw " + std::to_string(i));

    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("replay_pipeline_order_hook_test: ok");
    return 0;
}
