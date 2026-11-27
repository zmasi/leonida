// The GPU side of the segmented ExecuteIndirect(DISPATCH_MESH) expansion for mesh-as-compute (no VK_EXT_mesh_shader):
// one small compute pass per logical record that reads the record's DispatchMesh(x, y, z) on the GPU and writes the
// record's tiles (vulkan_mesh_indirect_tiles.h has the layout and the replay's side of the protocol). Nothing is read
// back by the CPU, the application's argument and count buffers are only read (never written or clamped), and no value
// the title wrote is truncated: a record is either drawn whole (every legal x, y, z with at most 2^22 groups) or is zero
// work (a zero dimension, a dimension over 65535, more than 2^22 groups, or more groups than slots * tiles can draw).
//
// The replay calls prepare once per logical ExecuteIndirect while a list is prepared (claims the tile slab, makes the
// pipeline on first use) and emit once per logical record when it records. emit only binds the pass pipeline, pushes
// its constants and dispatches: every barrier around it, the conditional-rendering scope and the producer's rebinding
// are the replay's (before: all earlier writes -> compute shader read, and the previous record's indirect/vertex reads
// -> this write; after: compute write -> indirect command and compute shader read). It must be recorded outside any
// rendering scope. The pass writes only the tile slab, so a predicated list can run it unconditionally.
//
// The tile slab (tiles * 48 bytes) is device-local scratch with STORAGE_BUFFER (the pass writes it through its device
// address), INDIRECT_BUFFER (the dispatch and the draw of every tile read it) and SHADER_DEVICE_ADDRESS usage (the
// producer reads TileMeta through it), 256-byte aligned, from a pool of its own (vulkan_scratch.h) that is retired with
// the submission's timeline ticket. One slab serves every logical ExecuteIndirect of a submission: calls are recorded in
// order, each record's tiles are consumed before the next record's pass writes them (the replay's barriers), and the slab
// is grown (a larger one claimed, the old one kept until retirement) only when a later call needs more tiles. The output
// slab the producer fills (at most 64 MiB) is claimed the same way, grow-only per submission and shared by every call.
//
// The argument and count addresses are the replay's recorded physical device addresses of buffers created with
// SHADER_DEVICE_ADDRESS (ResourceBinding::buffer_address): this file never asks Vulkan for the address of a buffer it
// was handed, and refuses an address that is zero, misaligned or whose range would wrap. Prepared state is the by-value
// mesh_indirect::PassParams inside Tiles; no allocation per call.
//
// Single-threaded per list, as the replay's preparation is: the caller holds the context's tracker lock for prepare and
// emit, and for collect/retire/forget.
#pragma once
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

#include "mesh_indirect_args_spv.h"
#include "vulkan_aftermath.h"
#include "vulkan_mesh_indirect_tiles.h"
#include "vulkan_scratch.h"

namespace leonida {

namespace detail {

// The pass's push constants; the shader's block is the same (mesh_indirect_args.comp).
struct MeshIndirectPush {
    VkDeviceAddress arguments;  // this record's three words
    VkDeviceAddress count;      // the count word, 0 when not counted
    VkDeviceAddress commands;   // tile 0 of the slab
    VkDeviceAddress output;     // the real output slab
    std::uint32_t slots;
    std::uint32_t tile_count;
    std::uint32_t vertices_per_group;
    std::uint32_t record;
    std::uint32_t records;
    std::uint32_t counted;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
};
static_assert(sizeof(MeshIndirectPush) == 64);

inline constexpr VkBufferUsageFlags mesh_indirect_scratch_usage =
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

}  // namespace detail

class MeshIndirectTiles {
public:
    using Allocate = ScratchBuffers::Allocate;

    // `limits` is the device's: the pass dispatches one thread per tile, so only maxComputeWorkGroupCount[0] matters.
    // `allocate` gives the tile slab device-local memory allocated with VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT.
    MeshIndirectTiles(PFN_vkGetDeviceProcAddr get, VkDevice device, const VkPhysicalDeviceLimits& limits,
                      Allocate allocate, DeviceFaultDiagnostics* diagnostics = nullptr,
                      std::vector<std::uint32_t> families = {})
        : get_(get), device_(device), max_group_count_x_(limits.maxComputeWorkGroupCount[0]),
          scratch_(get, device, std::move(allocate), diagnostics, detail::mesh_indirect_scratch_usage,
                   std::move(families)) {}
    ~MeshIndirectTiles() {
        if (!pipeline_ && !layout_) return;
        // Nothing may still be executing a pass that uses them: the owner destroys the context's queues first, and this
        // is the same wait the scratch pool does for its buffers.
        if (const auto idle = fn<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle")) idle(device_);
        if (pipeline_) fn<PFN_vkDestroyPipeline>("vkDestroyPipeline")(device_, pipeline_, nullptr);
        if (layout_) fn<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(device_, layout_, nullptr);
    }
    MeshIndirectTiles(const MeshIndirectTiles&) = delete;
    MeshIndirectTiles& operator=(const MeshIndirectTiles&) = delete;

    // False when the pass cannot run on this device (no device addresses, no compute): the replay then refuses indirect
    // DispatchMesh.
    bool loaded() const { return scratch_.loaded() && max_group_count_x_ != 0; }

    // The slab's life cycle, as the clamp's record pool (vulkan_scratch.h): collect at the first use of a submission,
    // retire with its ticket (0: nothing was submitted), forget when the timeline's queue is idle and going away.
    void collect() { scratch_.collect(); }
    void retire(VkSemaphore timeline, std::uint64_t ticket) {
        scratch_.retire(timeline, ticket);
        commands_slab_ = {};  // the claims belonged to the batch that has just left the pool's hands
        output_slab_ = {};
    }
    void forget(VkSemaphore timeline) { scratch_.forget(timeline); }

    // Calls this pass refused since the process started, by reason index of the last refusal (for tests and logs).
    std::uint64_t refusals() const { return refusals_; }
    const char* last_refusal() const { return last_refusal_; }

    // Claims the tile slab and the pass state of one logical ExecuteIndirect. `out` is cleared first and stays empty on
    // false; a false is a refusal the caller must act on (the replay refuses the draw). The pipeline is made on the
    // first call.
    bool prepare(const mesh_indirect::TileRequest& request, mesh_indirect::Tiles& out) {
        out = {};
        if (!loaded()) return refuse("no device addresses or compute group limit");
        const mesh_indirect::TileRequest& r = request;
        if (!r.records || !r.tiles || !r.slots || !r.max_primitives) return refuse("an empty request");
        if (r.slots > mesh_indirect::kMaxTileGroups || r.slots > max_group_count_x_)
            return refuse("slots over the physical dispatch limit");
        // One thread per tile: ceil(tiles / 64) groups of x, at most Vulkan's guaranteed 65535.
        if (r.tiles > std::uint64_t(65535) * local_size || r.tiles > mesh_indirect::kMaxRecordGroups)
            return refuse("more tiles than one preparation dispatch covers");
        if (r.stride < mesh_indirect::kRecordBytes || r.stride % 4) return refuse("an argument stride this pass does not handle");
        if (!r.output_bytes) return refuse("no output slab size");
        const std::uint64_t vertices = std::uint64_t(r.max_primitives) * 3;
        if (vertices > UINT32_MAX || std::uint64_t(r.slots) * vertices > UINT32_MAX)
            return refuse("a tile draw past 32-bit vertex counts");
        // The addresses are the replay's recorded physical ones (a buffer without SHADER_DEVICE_ADDRESS has none and is
        // refused there): nothing here queries a buffer. The last record's three words and the count word must be
        // 4-aligned and lie inside the 64-bit address space, checked as the remaining room before each addition.
        const std::uint64_t span = std::uint64_t(r.records - 1) * r.stride + mesh_indirect::kRecordBytes;
        if (!address_range_ok(r.arguments_address, span) || (r.counted && !address_range_ok(r.count_address,
                                                                                           sizeof(std::uint32_t))))
            return refuse("an argument or count address that is missing, misaligned or wraps");
        if (!make_pipeline()) return refuse("no compute pipeline");

        const std::uint64_t bytes = std::uint64_t(r.tiles) * mesh_indirect::kTileBytes;
        Slab commands{};
        if (!claim(commands_slab_, bytes, commands)) return refuse("no scratch for the tile slab");
        if (!mesh_indirect::valid_commands(commands.buffer, commands.address, commands.offset, bytes))
            return refuse("a tile slab that is not 16-aligned");
        // The output slab is the submission's shared one: every indirect call of the submission reuses it (the replay's
        // barriers order the reuse), so the worst case is the largest call's output_bytes, not their sum.
        Slab output{};
        if (!claim(output_slab_, r.output_bytes, output)) return refuse("no scratch for the output slab");
        if (!mesh_indirect::valid_commands(output.buffer, output.address, output.offset, r.output_bytes))
            return refuse("an output slab that is not 16-aligned");

        out.commands = commands.buffer;
        out.commands_offset = commands.offset;
        out.commands_address = commands.address;
        out.output_buffer = output.buffer;
        out.output_offset = output.offset;
        out.output_address = output.address;
        out.pass.arguments = r.arguments_address;
        out.pass.count = r.counted ? r.count_address : 0;
        out.pass.stride = r.stride;
        out.pass.slots = r.slots;
        out.pass.tiles = r.tiles;
        out.pass.vertices_per_group = std::uint32_t(vertices);
        out.pass.records = r.records;
        return true;
    }

    // Records record `record`'s pass: bind, push, dispatch. No barriers (the replay's), outside any rendering scope.
    // Leaves the compute pipeline bound; the replay rebinds its producer. The pipeline is this object's, made by
    // prepare, which the same list called first.
    void emit(VkCommandBuffer cb, const mesh_indirect::Tiles& tiles, std::uint32_t record) const {
        const mesh_indirect::PassParams& pass = tiles.pass;
        if (!pipeline_ || record >= pass.records) return;
        const detail::MeshIndirectPush constants{pass.arguments + std::uint64_t(record) * pass.stride,
                                                 pass.count,
                                                 tiles.commands_address,
                                                 tiles.output_address,
                                                 pass.slots,
                                                 pass.tiles,
                                                 pass.vertices_per_group,
                                                 record,
                                                 pass.records,
                                                 pass.count ? 1u : 0u,
                                                 0u,
                                                 0u};
        bind_(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
        push_(cb, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
        dispatch_(cb, (pass.tiles - 1u) / local_size + 1u, 1, 1);
    }

private:
    static constexpr std::uint32_t local_size = 64;  // mesh_indirect_args.comp

    // A non-null, 4-aligned address whose `bytes` from it end inside the 64-bit space: the room left after the base is
    // compared with the span, so nothing is ever added past the end and wrapped.
    static bool address_range_ok(std::uint64_t address, std::uint64_t bytes) {
        return address && address % 4 == 0 && bytes != 0 && bytes - 1 <= UINT64_MAX - address;
    }

    struct Slab {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        VkDeviceAddress address = 0;
        std::uint64_t bytes = 0;
    };

    template<typename T> T fn(const char* name) const { return reinterpret_cast<T>(get_(device_, name)); }

    bool refuse(const char* reason) {
        last_refusal_ = reason;
        const std::uint64_t total = ++refusals_;
        if (!(total & (total - 1)))  // the 1st, 2nd, 4th... refusal
            std::fprintf(stderr, "[rt] mesh indirect: %llu indirect DispatchMesh call(s) refused (%s)\n",
                         static_cast<unsigned long long>(total), reason);
        return false;
    }

    // A slab of this submission, grown when a call needs more than the one claimed so far. A grown slab is at least
    // twice the old one (up to the default output budget), so a run of ever larger calls claims a logarithmic number of
    // ranges instead of one each: the abandoned ranges stay until the submission retires, and their sum is under twice
    // the last. A claim the device refuses at the larger size is retried at the size the call needs.
    bool claim(Slab& cache, std::uint64_t bytes, Slab& out) {
        if (cache.buffer && cache.bytes >= bytes) {
            out = cache;
            return true;
        }
        std::uint64_t size = bytes;
        if (cache.buffer) size = std::max(bytes, std::min(cache.bytes * 2, mesh_indirect::kDefaultOutputBudget));
        Slab slab{};
        slab.bytes = size;
        if (!scratch_.get(size, slab.buffer, slab.offset, slab.address)) {
            if (size == bytes) return false;
            slab.bytes = bytes;
            if (!scratch_.get(bytes, slab.buffer, slab.offset, slab.address)) return false;
        }
        cache = slab;
        out = slab;
        return true;
    }

    bool make_pipeline() {
        if (pipeline_) return true;
        const auto create_module = fn<PFN_vkCreateShaderModule>("vkCreateShaderModule");
        const auto destroy_module = fn<PFN_vkDestroyShaderModule>("vkDestroyShaderModule");
        const auto create_layout = fn<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout");
        const auto destroy_layout = fn<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout");
        const auto create_pipeline = fn<PFN_vkCreateComputePipelines>("vkCreateComputePipelines");
        const auto destroy_pipeline = fn<PFN_vkDestroyPipeline>("vkDestroyPipeline");
        bind_ = fn<PFN_vkCmdBindPipeline>("vkCmdBindPipeline");
        push_ = fn<PFN_vkCmdPushConstants>("vkCmdPushConstants");
        dispatch_ = fn<PFN_vkCmdDispatch>("vkCmdDispatch");
        if (!create_module || !destroy_module || !create_layout || !destroy_layout || !create_pipeline ||
            !destroy_pipeline || !bind_ || !push_ || !dispatch_ ||
            !fn<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle"))
            return false;
        VkShaderModule module = VK_NULL_HANDLE;
        VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shader.codeSize = sizeof(mesh_indirect_args_spirv);
        shader.pCode = mesh_indirect_args_spirv;
        const aftermath::ShaderName aftermath_name("(runtime) mesh_indirect_args");
        bool ok = create_module(device_, &shader, nullptr, &module) == VK_SUCCESS;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(detail::MeshIndirectPush)};
        VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &range;
        ok = ok && create_layout(device_, &layout_info, nullptr, &layout) == VK_SUCCESS;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module = module;
        info.stage.pName = "main";
        info.layout = layout;
        ok = ok && create_pipeline(device_, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) == VK_SUCCESS;
        if (module) destroy_module(device_, module, nullptr);
        if (!ok) {
            if (pipeline) destroy_pipeline(device_, pipeline, nullptr);
            if (layout) destroy_layout(device_, layout, nullptr);
            return false;
        }
        layout_ = layout;
        pipeline_ = pipeline;
        return true;
    }

    PFN_vkGetDeviceProcAddr get_;
    VkDevice device_;
    std::uint32_t max_group_count_x_;
    ScratchBuffers scratch_;
    PFN_vkCmdBindPipeline bind_ = nullptr;
    PFN_vkCmdPushConstants push_ = nullptr;
    PFN_vkCmdDispatch dispatch_ = nullptr;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    Slab commands_slab_;  // this submission's tile slab: reset at retire
    Slab output_slab_;    // this submission's output slab, shared by every call that does not bring its own
    std::uint64_t refusals_ = 0;
    const char* last_refusal_ = nullptr;
};

}  // namespace leonida
