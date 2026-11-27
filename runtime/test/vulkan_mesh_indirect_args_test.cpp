// GPU test of the tile-preparation pass of ExecuteIndirect(DISPATCH_MESH) on mesh-as-compute
// (runtime/src/mesh_indirect_args.comp through vulkan_mesh_indirect_args.h): the pass is run for real on a device,
// and what it wrote - the 48-byte tiles: indirect dispatch, indirect draw and TileMeta - is read back and compared,
// word for word, with the contract of vulkan_mesh_indirect_tiles.h. Nothing is dispatched or drawn from those tiles:
// the legal maximum (2048 x 2048 x 1 = 2^22 groups, and 65535 x 64 x 1) and one group more (2048 x 2049 x 1, 65535 x
// 65 x 1) are checked on the values alone, which is the only thing a shared GPU can afford of a 2^22-group record.
// The end-to-end draws are d3d12_mesh_compute_indirect_test, with modest grids.
//
// Per case: every word of the tile slab is set to a sentinel by the GPU first (so a word the pass forgets shows), the
// pass runs once per record in the order the replay emits it (the slab is reused, each record read back before the
// next one runs), and the argument records sit at a nonzero offset with a padded stride whose padding is a sentinel
// too. Covered: legal records (a whole grid, split into slots-sized tiles whose first group and draw counts add up to
// exactly x*y*z), zero dimensions, records over a dimension (65535) or the total (2^22) which are zero work in every
// tile, the unfilled all-ones record, records at or past the executed count (min(count, max), count 0, count over
// max), for a small slot (65 535 groups per tile) and a large one (several tiles per 65 536 groups). No game data. Run
// with LEONIDA_VK_VALIDATION=1 to count VUIDs.
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include "vk_test_device.h"
#include "shader_fixture_path.h"

#include "../src/vulkan_memory.h"
#include "../src/vulkan_mesh_indirect_args.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <vector>

#ifndef LEONIDA_TEST_WORDS_SPV
#error "LEONIDA_TEST_WORDS_SPV must name cs_buffer_words.spv"
#endif

namespace {

namespace mi = leonida::mesh_indirect;

int failures = 0;

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            std::fprintf(stderr, "line %d: CHECK failed: %s\n", __LINE__, #cond);            \
            ++failures;                                                                      \
        }                                                                                    \
    } while (0)

constexpr std::uint32_t kSentinelWord = 0xCDCDCDCDu;   // what the GPU fills the slab with before the pass
constexpr std::uint32_t kPadding = 0x7E7E7E7Eu;        // record padding in the argument buffer
constexpr std::uint32_t kStride = 28, kArgOffset = 16, kCountOffset = 8;

struct WordTool {
    vktest::Device* device = nullptr;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    struct Push {
        std::uint64_t src, dst;
        std::uint32_t n, fill, mode, pad;
    };

    bool make(vktest::Device& d, const std::filesystem::path& spv) {
        device = &d;
        std::ifstream in(spv, std::ios::binary);
        const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (bytes.empty() || bytes.size() % 4) return false;
        VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shader.codeSize = bytes.size();
        shader.pCode = reinterpret_cast<const std::uint32_t*>(bytes.data());
        VkShaderModule module = VK_NULL_HANDLE;
        if (d.fn<PFN_vkCreateShaderModule>("vkCreateShaderModule")(d.device, &shader, nullptr, &module) != VK_SUCCESS)
            return false;
        const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
        VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &range;
        if (d.fn<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(d.device, &layout_info, nullptr, &layout) !=
            VK_SUCCESS)
            return false;
        VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module = module;
        info.stage.pName = "main";
        info.layout = layout;
        const bool ok = d.fn<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(d.device, VK_NULL_HANDLE, 1, &info,
                                                                                       nullptr, &pipeline) == VK_SUCCESS;
        d.fn<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(d.device, module, nullptr);
        return ok;
    }
    void destroy() {
        if (!device) return;
        if (pipeline) device->fn<PFN_vkDestroyPipeline>("vkDestroyPipeline")(device->device, pipeline, nullptr);
        if (layout) device->fn<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(device->device, layout, nullptr);
    }
    void run(VkCommandBuffer cb, std::uint64_t src, std::uint64_t dst, std::uint32_t n, std::uint32_t fill,
             std::uint32_t mode) const {
        const Push push{src, dst, n, fill, mode, 0};
        device->fn<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        device->fn<PFN_vkCmdPushConstants>("vkCmdPushConstants")(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                                                                 &push);
        device->fn<PFN_vkCmdDispatch>("vkCmdDispatch")(cb, (n - 1) / 64 + 1, 1, 1);
    }
};

// Everything before and after is ordered with this: the helper passes, the pass under test and the host read.
void barrier(vktest::Device& d, VkCommandBuffer cb, VkPipelineStageFlags dst_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
             VkAccessFlags dst_access = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT) {
    VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    memory.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    memory.dstAccessMask = dst_access;
    d.fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, dst_stage, 0, 1,
                                                           &memory, 0, nullptr, 0, nullptr);
}

struct Slot {
    const char* name;
    std::uint64_t bytes;
    std::uint32_t primitives;
};

struct Case {
    const char* name;
    std::vector<std::array<std::uint32_t, 3>> records;
    std::optional<std::uint32_t> count;
    std::uint32_t max = 0;  // max_command_count; 0: records.size()
};

// The contract's tile words of one record: what the pass must have written for each of `plan.tiles` tiles.
std::vector<std::uint32_t> expected_tiles(const mi::Plan& plan, std::uint32_t vertices_per_group,
                                          std::uint64_t output, std::array<std::uint32_t, 3> r, bool executed) {
    std::uint64_t groups = 0;
    if (executed && !mi::record_groups(r[0], r[1], r[2], groups)) groups = 0;
    const std::uint64_t needed = (groups + plan.slots - 1) / plan.slots;
    if (needed > plan.tiles) groups = 0;  // never a partial draw of a record the slab cannot cover
    std::vector<std::uint32_t> words;
    for (std::uint32_t t = 0; t < plan.tiles; ++t) {
        const bool live = groups != 0 && t < needed;
        const std::uint64_t first = std::uint64_t(t) * plan.slots;
        const std::uint32_t n = live ? std::uint32_t(std::min<std::uint64_t>(plan.slots, groups - first)) : 0;
        const std::uint32_t one = live ? 1 : 0;
        const std::uint32_t tile[12] = {n, one, one, 0, n * vertices_per_group, one, 0, 0,
                                        std::uint32_t(output), std::uint32_t(output >> 32),
                                        live ? std::uint32_t(first) : 0u, 0};
        words.insert(words.end(), tile, tile + 12);
    }
    return words;
}

}  // namespace

int main() {
    vktest::Device device;
    device.request_buffer_device_address = true;
    if (!device.create()) {
        std::printf("vulkan_mesh_indirect_args_test: SKIP (no Vulkan device)\n");
        return 77;
    }
    if (!device.buffer_device_address) {
        std::printf("vulkan_mesh_indirect_args_test: SKIP (no bufferDeviceAddress)\n");
        return 77;
    }
    VkPhysicalDeviceProperties properties{};
    device.instance_fn<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(device.physical, &properties);
    VkPhysicalDeviceMemoryProperties memory{};
    device.instance_fn<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(device.physical,
                                                                                                         &memory);
    const auto allocate = [&](const VkMemoryRequirements& requirements) -> VkDeviceMemory {
        VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
        flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags};
        info.allocationSize = requirements.size;
        for (const std::uint32_t type : leonida::memory_type_order(
                 memory, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, requirements.size)) {
            info.memoryTypeIndex = type;
            VkDeviceMemory out = VK_NULL_HANDLE;
            if (device.fn<PFN_vkAllocateMemory>("vkAllocateMemory")(device.device, &info, nullptr, &out) == VK_SUCCESS)
                return out;
        }
        return VK_NULL_HANDLE;
    };
    leonida::MeshIndirectTiles pass(device.device_proc, device.device, properties.limits, allocate);
    if (!pass.loaded()) {
        std::printf("vulkan_mesh_indirect_args_test: SKIP (the pass cannot run on this device)\n");
        return 77;
    }
    WordTool words;
    if (!words.make(device, vktest::shader_fixture_path(LEONIDA_TEST_WORDS_SPV))) {
        std::fprintf(stderr, "cannot create the word helper from %s\n", LEONIDA_TEST_WORDS_SPV);
        return 1;
    }

    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    void *arguments_map = nullptr, *counts_map = nullptr, *readback_map = nullptr;
    constexpr VkDeviceSize kBytes = 1 << 20;
    const VkBuffer arguments = device.buffer(4096, usage, &arguments_map);
    const VkBuffer counts = device.buffer(256, usage, &counts_map);
    const VkBuffer readback = device.buffer(kBytes, usage, &readback_map);
    CHECK(arguments && counts && readback && arguments_map && counts_map && readback_map);
    if (failures) return 1;

    mi::Limits limits;
    limits.max_group_count_x = properties.limits.maxComputeWorkGroupCount[0];
    const Slot slots[] = {{"narrow slot (128 B, 1 primitive)", 128, 1}, {"wide slot (3088 B, 64 primitives)", 3088, 64}};
    const std::array<std::uint32_t, 3> kLegalMax{2048, 2048, 1};

    for (const Slot& slot : slots) {
        mi::Plan plan;
        CHECK(mi::plan(slot.bytes, slot.primitives, 12, limits, plan));
        if (failures) return 1;
        const std::uint32_t vertices_per_group = slot.primitives * 3;
        std::fprintf(stderr, "%s: %u slots x %u tiles\n", slot.name, plan.slots, plan.tiles);
        // The tile count covers the legal maximum exactly: this is what "no legal XYZ truncation" rests on.
        CHECK(std::uint64_t(plan.slots) * plan.tiles >= mi::kMaxRecordGroups);
        CHECK(std::uint64_t(plan.slots) * (plan.tiles - 1) < mi::kMaxRecordGroups);

        const std::vector<Case> cases = {
            {"the legal maximum, one group more, and their neighbours",
             {kLegalMax, {2048, 2049, 1}, {65535, 64, 1}, {65535, 65, 1}, {1, 2049, 2048}, {256, 128, 128},
              {256, 128, 129}, {1, 1, 65536}, {65536, 1, 1}, {1, 1, 1}, {2, 3, 4}, {1, 1, 65535}}},
            // No work is launched from these records. A 32-bit product would incorrectly accept
            // the last two (2^32 + 2^22, and 2^32 + 983024). The reference uses 64-bit math.
            {"exact maximum plus one, and large products which wrap to legal nonzero 32-bit counts",
             {{5, 397, 2113}, {2048, 2048, 1025}, {16, 65535, 4097}, {1, 1, 1}}},
            {"zero dimensions, the unfilled all-ones record, huge dimensions",
             {{0, 5, 5}, {4, 0, 1}, {2, 2, 0}, {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu}, {0xFFFFFFFFu, 1, 1},
              {1, 0xFFFFFFFFu, 0}, {3, 1, 1}, {kPadding, kPadding, kPadding}}},
            {"a few tiles' worth: slots, slots + 1, 2 * slots, 65 536 groups",
             {{plan.slots, 1, 1}, {plan.slots + 1 > 65535 ? 65535 : plan.slots + 1, 1, 1}, {2, plan.slots / 2 + 1, 1},
              {256, 256, 1}, {255, 257, 1}}},
            {"counted 2 of 4: records past the count are not read",
             {{3, 1, 1}, {4, 2, 1}, {0xFFFFFFFFu, 1, 1}, {5, 5, 5}}, 2},
            {"counted 0 of 3", {{3, 1, 1}, {4, 2, 1}, {5, 5, 5}}, 0},
            {"count 100 over max 3", {{3, 1, 1}, {4, 2, 1}, {1, 1, 65536}}, 100},
            {"max 3 of 5 records: the others are never reached", {{3, 1, 1}, {4, 2, 1}, {5, 5, 5}, {6, 1, 1}, {7, 1, 1}}, std::nullopt, 3},
        };
        for (const Case& c : cases) {
            const std::uint32_t records = c.max ? c.max : std::uint32_t(c.records.size());
            // Arguments at a nonzero offset, stride 28, padding sentinel; the count word at offset 8.
            std::memset(arguments_map, 0, 4096);
            for (std::size_t i = 0; i < 4096 / 4; ++i) static_cast<std::uint32_t*>(arguments_map)[i] = kPadding;
            for (std::size_t i = 0; i < c.records.size(); ++i)
                std::memcpy(static_cast<std::uint8_t*>(arguments_map) + kArgOffset + i * kStride, c.records[i].data(), 12);
            for (std::size_t i = 0; i < 64; ++i) static_cast<std::uint32_t*>(counts_map)[i] = kPadding;
            if (c.count) std::memcpy(static_cast<std::uint8_t*>(counts_map) + kCountOffset, &*c.count, 4);

            mi::Plan p;
            CHECK(mi::plan(slot.bytes, slot.primitives, records, limits, p));
            CHECK(p.slots == plan.slots && p.tiles == plan.tiles);
            mi::TileRequest request;
            // Real device addresses of the two BDA buffers, the logical offsets added once.
            request.arguments_address = device.device_address(arguments) + kArgOffset;
            request.count_address = c.count ? device.device_address(counts) + kCountOffset : 0;
            request.counted = c.count.has_value();
            CHECK(request.arguments_address > kArgOffset && (!c.count || request.count_address > kCountOffset));
            request.stride = kStride;
            request.records = records;
            request.slots = p.slots;
            request.tiles = p.tiles;
            request.max_primitives = slot.primitives;
            request.output_bytes = p.output_bytes;
            mi::Tiles tiles;
            const bool prepared = pass.prepare(request, tiles);
            CHECK(prepared);
            if (!prepared) continue;
            CHECK(mi::valid_commands(tiles.commands, tiles.commands_address, tiles.commands_offset, p.command_bytes));
            CHECK(tiles.output_buffer && tiles.output_address % mi::kTileAlignment == 0 && tiles.output_address);

            const std::uint32_t tile_words = p.tiles * 12;
            const std::uint64_t slab_words_bytes = std::uint64_t(tile_words) * 4;
            const std::uint64_t readback_address = device.device_address(readback);
            CHECK(slab_words_bytes * records <= kBytes);
            const bool ok = device.run([&](VkCommandBuffer cb) {
                for (std::uint32_t r = 0; r < records; ++r) {
                    // Sentinel the slab, run the pass for record r, copy the slab out: the slab is the replay's one
                    // reused allocation, so each record is read before the next one overwrites it.
                    words.run(cb, 0, tiles.commands_address, tile_words, kSentinelWord, 1);
                    barrier(device, cb);
                    pass.emit(cb, tiles, r);
                    barrier(device, cb);
                    words.run(cb, tiles.commands_address, readback_address + r * slab_words_bytes, tile_words, 0, 0);
                    barrier(device, cb);
                }
                barrier(device, cb, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
                return true;
            });
            CHECK(ok);
            unsigned wrong = 0;
            const auto* got = static_cast<const std::uint32_t*>(readback_map);
            const std::uint32_t executed = c.count ? std::min(*c.count, records) : records;
            std::uint64_t drawn = 0;
            for (std::uint32_t r = 0; r < records; ++r) {
                const std::array<std::uint32_t, 3> record =
                    r < c.records.size() ? c.records[r] : std::array<std::uint32_t, 3>{kPadding, kPadding, kPadding};
                const auto want = expected_tiles(p, vertices_per_group, tiles.output_address, record, r < executed);
                std::uint64_t groups_got = 0;
                for (std::uint32_t t = 0; t < p.tiles; ++t) groups_got += got[std::size_t(r) * tile_words + t * 12];
                drawn += groups_got;
                for (std::uint32_t w = 0; w < tile_words; ++w) {
                    const std::uint32_t have = got[std::size_t(r) * tile_words + w];
                    if (have == want[w]) continue;
                    if (wrong++ < 6)
                        std::fprintf(stderr, "%s / %s: record %u (%u,%u,%u) tile %u word %u: got %#x want %#x\n", slot.name,
                                     c.name, r, record[0], record[1], record[2], w / 12, w % 12, have, want[w]);
                }
                // Independent of the table above: a legal executed record's tiles add up to exactly x*y*z groups,
                // and an invalid, zero or unexecuted one to nothing.
                std::uint64_t groups = 0;
                const bool legal = r < executed && mi::record_groups(record[0], record[1], record[2], groups);
                CHECK(groups_got == (legal ? groups : 0));
            }
            CHECK(wrong == 0);
            std::fprintf(stderr, "  %s: ok, %llu groups prepared over %u records\n", c.name, (unsigned long long)drawn,
                         records);
            pass.retire(VK_NULL_HANDLE, 0);
            pass.collect();
        }
    }
    words.destroy();
    if (device.validation_errors()) {
        std::fprintf(stderr, "%u validation errors\n", device.validation_errors());
        ++failures;
    }
    std::printf("%s: tile preparation of the legal maximum, maximum + 1, zero, invalid and counted records\n",
                failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
