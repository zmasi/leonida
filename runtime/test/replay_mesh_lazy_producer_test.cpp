// A lazy mesh-compute producer (MeshComputeDirect::build) as prepare() meets it. The PSO compiles the producer once,
// at the first DispatchMesh that will execute; the replay layer must ask for it only then:
//   - a draw that executes gets the built producer (and asks the PSO, which answers from its cache afterwards);
//   - a zero grid, an over-limit grid and a draw whose consumer is still pending ask for nothing, so an unused or
//     refused PSO costs no driver compile;
//   - a failed build fails the draw (unsupported), even for an asynchronous PSO outside strict replay: the error
//     is propagated, the draw is never skipped;
//   - an eagerly created PSO (producer set, no build) and a PSO with neither (a pending asynchronous producer) behave
//     as before.
// No GPU: pipelines and resources are fakes. Run a second time with LEONIDA_STRICT_REPLAY=1.
#include "../src/vulkan_replay.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
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
constexpr std::uintptr_t pipeline_id = 1;

// What the PSO's build counts: how often the replay layer asked for the producer.
struct Owner {
    int asks = 0;
    bool fails = false;
};

VkPipeline build_producer(void* context) {
    auto* owner = static_cast<Owner*>(context);
    ++owner->asks;
    return owner->fails ? VK_NULL_HANDLE : handle<VkPipeline>(0x7001);
}

// One mesh-compute PSO: a built consumer, and a producer that is lazy, eager or absent. Not copyable: the pipeline
// points at the direct block.
struct Fixture {
    enum class Producer { lazy, eager, none };
    Owner owner;
    MeshComputeDirect direct;
    GraphicsPipeline pipeline;

    Fixture(Producer producer, bool async, bool consumer_pending = false) {
        direct.producer_layout = handle<VkPipelineLayout>(0x7002);
        direct.slot_bytes = 64;
        direct.max_primitives = 1;
        if (producer == Producer::lazy) {
            direct.build = build_producer;
            direct.build_context = &owner;
        } else if (producer == Producer::eager) {
            direct.producer = handle<VkPipeline>(0x7001);
        }
        pipeline.layout = handle<VkPipelineLayout>(7);
        if (consumer_pending)
            pipeline.variant = [](std::uint32_t) { return VkPipeline(VK_NULL_HANDLE); };
        else
            pipeline.pipeline = handle<VkPipeline>(0x7003);
        pipeline.color_formats.assign(1, VK_FORMAT_R8G8B8A8_UNORM);
        pipeline.mesh = true;
        pipeline.mesh_compute_direct = &direct;
        pipeline.async_build = async;
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
};

CommandList record(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    CommandList list;
    Draw d{};
    d.pipeline = pipeline_id;
    d.topology = 4;
    d.count = x;
    d.instances = y;
    d.first = z;
    d.mesh = true;
    auto views = std::make_shared<DrawViews>();
    views->render_targets[0] = {color_id, 0, 0, 1};
    d.views = views;
    list.draw(d);
    list.close();
    return list;
}

ReplayFunctions functions(const Fixture& fixture) {
    ReplayFunctions vk{};
    vk.begin_rendering = +[](VkCommandBuffer, const VkRenderingInfo*) {};
    vk.end_rendering = +[](VkCommandBuffer) {};
    vk.bind_pipeline = +[](VkCommandBuffer, VkPipelineBindPoint, VkPipeline) {};
    vk.push_constants = +[](VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, std::uint32_t, std::uint32_t,
                            const void*) {};
    vk.dispatch = +[](VkCommandBuffer, std::uint32_t, std::uint32_t, std::uint32_t) {};
    vk.pipeline_barrier = +[](VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags,
                              std::uint32_t, const VkMemoryBarrier*, std::uint32_t, const VkBufferMemoryBarrier*,
                              std::uint32_t, const VkImageMemoryBarrier*) {};
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
    vk.graphics_pipeline = [&fixture](std::uintptr_t id) -> const GraphicsPipeline* {
        return id == pipeline_id ? &fixture.pipeline : nullptr;
    };
    vk.mesh_compute_scratch = [](VkDeviceSize, VkBuffer& buffer, VkDeviceSize& offset, VkDeviceAddress& address) {
        buffer = handle<VkBuffer>(0x7004);
        offset = 0;
        address = 0x10000;
        return true;
    };
    vk.mesh_compute_max_group_count[0] = vk.mesh_compute_max_group_count[1] = vk.mesh_compute_max_group_count[2] = 65535;
    vk.mesh_compute_queue = true;
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

struct Outcome {
    ReplayResult result = ReplayResult::success;
    PreparedList prepared;
    const detail::PreparedDraw* draw() const {
        return prepared.draw_count ? &prepared.draws[prepared.commands[0].draw] : nullptr;
    }
};

Outcome prepare_draws(const Fixture& fixture, std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    const CommandList list = record(x, y, z);
    const ReplayFunctions vk = functions(fixture);
    Outcome out;
    out.result = prepare(list, resolve, vk, out.prepared);
    return out;
}
}  // namespace

int main() {
    const bool strict = strict_replay();
    const VkPipeline producer = handle<VkPipeline>(0x7001);

    {  // A draw that executes asks for the producer, and gets it.
        Fixture f(Fixture::Producer::lazy, false);
        check(f.owner.asks == 0, "lazy: creation asks nothing");
        const Outcome o = prepare_draws(f, 3, 1, 1);
        check(o.result == ReplayResult::success, "lazy draw: prepared");
        check(f.owner.asks == 1, "lazy draw: the producer is asked for once for the one draw");
        const auto* d = o.draw();
        check(d && d->mesh_compute && !d->skipped, "lazy draw: expands as a direct mesh draw");
        check(d && d->mesh_producer == producer, "lazy draw: the built producer reaches the draw");
        check(d && d->mesh_producer_layout == f.direct.producer_layout, "lazy draw: its layout");
        check(d && d->mesh_vertex_count == 9 && d->mesh_groups[0] == 3, "lazy draw: counts");
        const Outcome again = prepare_draws(f, 3, 1, 1);
        check(again.result == ReplayResult::success && again.draw() && again.draw()->mesh_producer == producer,
              "lazy draw: a second draw gets the same producer");
    }
    {  // A zero grid, an over-limit grid and a pending consumer ask for no producer.
        Fixture f(Fixture::Producer::lazy, false);
        const Outcome zero = prepare_draws(f, 3, 1, 0);
        check(zero.result == ReplayResult::success, "zero grid: a no-op");
        check(f.owner.asks == 0, "zero grid: no producer asked for");
        const Outcome over = prepare_draws(f, 65535, 65535, 1);
        check(over.result == ReplayResult::unsupported, "over-limit grid: refused");
        check(f.owner.asks == 0, "over-limit grid: no producer asked for");
    }
    {  // The consumer is still being built: the draw waits, and so does the producer.
        Fixture f(Fixture::Producer::lazy, true, true);
        const Outcome o = prepare_draws(f, 3, 1, 1);
        check(f.owner.asks == 0, "pending consumer: no producer asked for");
        if (strict) {
            check(o.result == ReplayResult::unsupported, "pending consumer (strict): fails the list");
        } else {
            check(o.result == ReplayResult::success && o.draw() && o.draw()->skipped, "pending consumer: drawn later");
        }
    }
    for (const bool async : {false, true}) {  // A failed build fails the draw: never skipped, whatever the mode.
        Fixture f(Fixture::Producer::lazy, async);
        f.owner.fails = true;
        const Outcome o = prepare_draws(f, 3, 1, 1);
        check(f.owner.asks == 1, "failed build: asked once");
        check(o.result == ReplayResult::unsupported, async ? "failed build (async PSO): the error is propagated"
                                                           : "failed build: the error is propagated");
        check(!o.draw() || !o.draw()->mesh_compute, "failed build: nothing is expanded");
    }
    {  // Eager: the producer exists, nothing is asked.
        Fixture f(Fixture::Producer::eager, false);
        const Outcome o = prepare_draws(f, 3, 1, 1);
        check(o.result == ReplayResult::success, "eager draw: prepared");
        check(o.draw() && o.draw()->mesh_producer == producer, "eager draw: its producer");
        check(f.owner.asks == 0, "eager draw: no build");
    }
    {  // Neither a producer nor a build, asynchronous: unchanged (waits, or fails under strict replay).
        Fixture f(Fixture::Producer::none, true);
        const Outcome o = prepare_draws(f, 3, 1, 1);
        if (strict) {
            check(o.result == ReplayResult::unsupported, "no producer (strict): fails the list");
        } else {
            check(o.result == ReplayResult::success && o.draw() && o.draw()->skipped, "no producer: drawn later");
        }
        Fixture sync(Fixture::Producer::none, false);
        check(prepare_draws(sync, 3, 1, 1).result == ReplayResult::unsupported, "no producer, synchronous: refused");
    }
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("replay mesh lazy producer: asked only by executing draws, failure propagated%s\n",
                strict ? " (strict replay)" : "");
    return 0;
}
