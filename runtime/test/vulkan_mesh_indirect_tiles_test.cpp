// Segmented ExecuteIndirect(DISPATCH_MESH) planning: the legal and invalid record shapes, the fixed memory and tile
// count of one logical call, and the bounds that refuse a call whole instead of truncating it. CPU only: no device, no
// shader, no source-text matching. (The GPU side is d3d12_mesh_compute_indirect_test.)
#include "../src/vulkan_mesh_indirect_tiles.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>

namespace {

int failures = 0;
int checks = 0;

#define CHECK(cond)                                                                                          \
    do {                                                                                                     \
        ++checks;                                                                                            \
        if (!(cond)) {                                                                                       \
            ++failures;                                                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                       \
        }                                                                                                    \
    } while (0)

namespace mi = leonida::mesh_indirect;

constexpr std::uint64_t kMaxGroups = std::uint64_t(1) << 22;

// A Vega 20-like device: 65 535 groups in x at least, big buffers.
mi::Limits vega() {
    mi::Limits limits;
    limits.max_buffer_size = std::uint64_t(2) << 30;
    limits.max_allocation_size = std::uint64_t(2) << 30;
    limits.max_group_count_x = 65535;
    return limits;
}

void test_layout() {
    static_assert(mi::kTileBytes == 48 && mi::kTileBytes % mi::kTileAlignment == 0);
    static_assert(mi::kMetaOffset == 32 && mi::kDrawOffset == 16 && mi::kDispatchOffset == 0 &&
                  mi::kReservedOffset == 12);
    static_assert(mi::kRecordBytes == 12 && mi::kMaxTileGroups == 65535 && mi::kMetaTag == 1);
    CHECK(mi::kMaxRecordGroups == kMaxGroups);
}

void test_record_groups() {
    std::uint64_t groups = 123;
    CHECK(mi::record_groups(0, 5, 5, groups) && groups == 0);   // a zero dimension is a legal no-op
    CHECK(mi::record_groups(5, 0, 5, groups) && groups == 0);
    CHECK(mi::record_groups(5, 5, 0, groups) && groups == 0);
    CHECK(mi::record_groups(0, 0, 0, groups) && groups == 0);
    CHECK(mi::record_groups(0xFFFFFFFFu, 0, 7, groups) && groups == 0);  // zero wins over the other bounds
    CHECK(mi::record_groups(1, 1, 1, groups) && groups == 1);
    CHECK(mi::record_groups(2, 3, 4, groups) && groups == 24);
    CHECK(mi::record_groups(65535, 1, 1, groups) && groups == 65535);
    CHECK(mi::record_groups(1, 65535, 1, groups) && groups == 65535);
    CHECK(mi::record_groups(1, 1, 65535, groups) && groups == 65535);
    CHECK(mi::record_groups(2048, 2048, 1, groups) && groups == kMaxGroups);  // the largest legal record
    CHECK(mi::record_groups(256, 128, 128, groups) && groups == kMaxGroups);
    // Invalid: a dimension over 65535, or a product over 2^22 with every dimension legal.
    CHECK(!mi::record_groups(65536, 1, 1, groups));
    CHECK(!mi::record_groups(1, 65536, 1, groups));
    CHECK(!mi::record_groups(1, 1, 65536, groups));
    CHECK(!mi::record_groups(0xFFFFFFFFu, 1, 1, groups));
    CHECK(!mi::record_groups(2048, 2049, 1, groups));
    CHECK(!mi::record_groups(65535, 65535, 65535, groups));
    CHECK(!mi::record_groups(0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, groups));
}

void test_plan_covers_every_legal_record() {
    // Whatever the slot size, slots * tiles >= 2^22: a plan never shortens a legal DispatchMesh.
    for (const std::uint64_t slot : {16ull, 128ull, 3088ull, 65536ull, 1ull << 20, 8ull << 20, 64ull << 20}) {
        for (const std::uint32_t prims : {1u, 64u, 256u}) {
            mi::Plan plan;
            if (!mi::plan(slot, prims, 1, vega(), plan)) continue;  // a refusal is allowed, a short plan is not
            CHECK(plan.slots >= 1 && plan.slots <= mi::kMaxTileGroups);
            CHECK(std::uint64_t(plan.slots) * plan.tiles >= kMaxGroups);
            CHECK(std::uint64_t(plan.slots) * (plan.tiles - 1) < kMaxGroups);  // and no tile is spare
            CHECK(plan.output_bytes == std::uint64_t(plan.slots) * slot && plan.output_bytes <= mi::kDefaultOutputBudget);
            CHECK(plan.command_bytes == std::uint64_t(plan.tiles) * mi::kTileBytes);
            CHECK(plan.records == 1 && plan.restarts == plan.tiles);
            CHECK(std::uint64_t(plan.slots) * prims * 3 <= UINT32_MAX);  // one tile's draw count fits
        }
    }
    // The small slot of ms_v5_workgroup-like producers: one 65 535-group tile, 65 tiles cover 2^22.
    mi::Plan plan;
    CHECK(mi::plan(128, 1, 4, vega(), plan) && plan.slots == 65535 && plan.tiles == 65 && plan.restarts == 260);
    // A ~3 KiB slot cannot hold 65 536 groups in 64 MiB: a 65 536-group record needs several tiles.
    CHECK(mi::plan(3088, 64, 1, vega(), plan));
    CHECK(plan.slots == mi::kDefaultOutputBudget / 3088 && plan.slots < 65536 && plan.tiles >= 4);
}

void test_caps() {
    mi::Plan plan;
    // Each device limit caps the tile on its own.
    auto limits = vega();
    limits.max_group_count_x = 1000;
    CHECK(mi::plan(128, 1, 1, limits, plan) && plan.slots == 1000 && plan.tiles == 4195);
    limits = vega();
    limits.max_buffer_size = 1 << 20;
    CHECK(mi::plan(128, 1, 1, limits, plan) && plan.slots == 8192);
    limits = vega();
    limits.tuning.max_restarts = std::uint64_t(1) << 40;  // tiny tiles are many: allow them where the cap is the point
    limits.max_allocation_size = 128 * 100;
    CHECK(mi::plan(128, 1, 1, limits, plan) && plan.slots == 100);
    limits = vega();
    limits.tuning.output_budget = 128 * 7;
    limits.tuning.max_restarts = std::uint64_t(1) << 40;
    CHECK(mi::plan(128, 1, 1, limits, plan) && plan.slots == 7 && plan.tiles == (kMaxGroups + 6) / 7);
    CHECK(plan.output_bytes == 128 * 7);
    limits.tuning.max_restarts = 8192;  // the same plan under the default bound: 599 186 tiles, refused whole
    CHECK(!mi::plan(128, 1, 1, limits, plan));
    // A tile's vertex count fits uint32: 65535 primitives of 3 vertices cap a tile at 21845 groups.
    CHECK(mi::plan(16, 65535, 1, vega(), plan) && plan.slots == 21845);
    // Unknown (zero) buffer limits are not caps; an unknown group count is a refusal.
    limits = vega();
    limits.max_buffer_size = 0;
    limits.max_allocation_size = 0;
    CHECK(mi::plan(128, 1, 1, limits, plan) && plan.slots == 65535);
    limits.max_group_count_x = 0;
    CHECK(!mi::plan(128, 1, 1, limits, plan) && plan.slots == 0 && plan.tiles == 0);
}

void test_refusals() {
    mi::Plan plan;
    CHECK(!mi::plan(0, 1, 1, vega(), plan));            // no slot
    CHECK(!mi::plan(20, 1, 1, vega(), plan));           // slot not 16-aligned
    CHECK(!mi::plan(128, 0, 1, vega(), plan));          // no primitives
    CHECK(!mi::plan(128, 1, 0, vega(), plan));          // no records
    // A slot over the output budget cannot be tiled at all: refused, not truncated to zero slots.
    CHECK(!mi::plan(mi::kDefaultOutputBudget + 16, 1, 1, vega(), plan));
    auto limits = vega();
    limits.tuning.output_budget = 0;
    CHECK(!mi::plan(128, 1, 1, limits, plan));
    limits = vega();
    limits.tuning.max_restarts = 0;
    CHECK(!mi::plan(128, 1, 1, limits, plan));
    // max_restarts is exact: 65 tiles per record of the small slot, 126 records fit 8192, 127 do not.
    CHECK(mi::plan(128, 1, 126, vega(), plan) && plan.restarts == 126 * 65 && plan.restarts <= mi::kDefaultMaxRestarts);
    CHECK(!mi::plan(128, 1, 127, vega(), plan) && plan.slots == 0 && plan.restarts == 0);
    // Records scale the physical work only, never the fixed memory.
    mi::Plan one, many;
    CHECK(mi::plan(128, 1, 1, vega(), one) && mi::plan(128, 1, 100, vega(), many));
    CHECK(one.output_bytes == many.output_bytes && one.command_bytes == many.command_bytes &&
          many.restarts == 100 * one.restarts);
    // 2^32 - 1 records of any plan is a refusal, with no wrap in the product.
    CHECK(!mi::plan(128, 1, 0xFFFFFFFFu, vega(), plan));
}

void test_tags_and_commands() {
    const std::uint64_t base = 0x7000000000ull;
    for (const std::uint32_t tile : {0u, 1u, 7u, 193u}) {
        const std::uint64_t tagged = mi::tagged_meta_address(base, tile);
        CHECK((tagged & 1) == 1);                                            // bit 0: this is a TileMeta
        CHECK(((tagged & ~std::uint64_t(1)) % mi::kTileAlignment) == 0);     // the real address is 16-aligned
        CHECK((tagged & ~std::uint64_t(1)) == base + std::uint64_t(tile) * mi::kTileBytes + mi::kMetaOffset);
    }
    int object = 0;
    CHECK(mi::valid_commands(&object, 0x1000, 0, 48));
    CHECK(!mi::valid_commands(nullptr, 0x1000, 0, 48));
    CHECK(!mi::valid_commands(&object, 0, 0, 48));
    CHECK(!mi::valid_commands(&object, 0x1000, 0, 0));
    CHECK(!mi::valid_commands(&object, 0x1008, 0, 48));                       // address not 16-aligned
    CHECK(!mi::valid_commands(&object, 0x1000, 8, 48));                       // offset not 16-aligned
    CHECK(!mi::valid_commands(&object, 0xFFFFFFFFFFFFFFF0ull, 0, 48));        // address + bytes wraps
    CHECK(!mi::valid_commands(&object, 0x1000, 0xFFFFFFFFFFFFFFF0ull, 48));   // offset + bytes wraps
    // The last representable range: address + bytes == UINT64_MAX fits, one byte more wraps.
    CHECK(mi::valid_commands(&object, 0xFFFFFFFFFFFFFFC0ull, 0, 63));
    CHECK(!mi::valid_commands(&object, 0xFFFFFFFFFFFFFFC0ull, 0, 64));
    CHECK(!mi::valid_commands(&object, 0xFFFFFFFFFFFFFFF0ull, 0, 48));
}

}  // namespace

int main() {
    test_layout();
    test_record_groups();
    test_plan_covers_every_legal_record();
    test_caps();
    test_refusals();
    test_tags_and_commands();
    std::printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
