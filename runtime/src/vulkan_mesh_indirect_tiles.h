// Segmented ExecuteIndirect(DISPATCH_MESH) expansion for mesh-as-compute (no VK_EXT_mesh_shader). No Vulkan calls:
// unit-testable without a device. vulkan_replay.h uses it to plan the fixed memory of one logical ExecuteIndirect, and
// the GPU prepare pass (a different file) uses the same layout constants to write the tiles it emits.
//
// One logical record is DispatchMesh(x, y, z) read from the argument buffer by the GPU; its G = x*y*z groups can exceed
// the slot capacity of the one output slab, so the record is cut into `tiles` consecutive TILES of at most `slots`
// groups. A prepare pass (one compute dispatch per logical record) writes `tiles` 48-byte Tile entries; replay then
// runs, for tile t = 0..tiles-1 of every record, in this order and outside any rendering scope:
//     vkCmdDispatchIndirect(commands, tile.dispatch)   producer, 1-D, writes local slots 0..n-1 of the output slab
//     barrier  producer write -> vertex shader read
//     vkCmdBeginRendering; graphics state; vkCmdDrawIndirect(commands, tile.draw); vkCmdEndRendering
//     barrier  vertex read, indirect read -> next producer/prepare write
// The consumer (vertex) pipeline is the ordinary one and reads the real output slab; the producer gets the address of
// the tile's TileMeta with bit 0 set (kMetaTag) as its output base and finds the real slab, its first logical linear
// group and nothing else through it.
//
//   Tile (48 bytes, 16-aligned, every field always written; an inactive or invalid tile is all zero):
//     byte  0  VkDispatchIndirectCommand {n, 1, 1}   n = active groups of the tile, <= 65535, <= slots; 0 when inactive
//     byte 12  reserved uint (0)
//     byte 16  VkDrawIndirectCommand {n * max_primitives * 3, 1, 0, 0}; {0, 0, 0, 0} when inactive
//     byte 32  TileMeta { uvec2 output slab address (16-aligned); uint first logical linear group; uint reserved }
#pragma once

#include <cstdint>
#include <vulkan/vulkan_core.h>

#include "vulkan_mesh_compute_direct.h"

namespace leonida {
namespace mesh_indirect {

inline constexpr std::uint32_t kTileBytes = 48;
inline constexpr std::uint32_t kDispatchOffset = 0;
inline constexpr std::uint32_t kReservedOffset = 12;
inline constexpr std::uint32_t kDrawOffset = 16;
inline constexpr std::uint32_t kMetaOffset = 32;
inline constexpr std::uint64_t kTileAlignment = 16;  // keeps TileMeta 16-aligned, so bit 0 of its address is free
inline constexpr std::uint64_t kMetaTag = 1;         // bit 0 of the producer's output base: "this is a TileMeta"
// A DISPATCH_MESH record: three uints (VkDrawMeshTasksIndirectCommandEXT); a stride may pad it.
inline constexpr std::uint32_t kRecordBytes = 12;
// The most groups of one physical dispatch: flattened 1-D (D3D12 limits each dimension to 65535 as well).
inline constexpr std::uint32_t kMaxTileGroups = 65535;
// Groups of one logical record, whole: the D3D12 DispatchMesh limit.
inline constexpr std::uint64_t kMaxRecordGroups = mesh_compute_direct::kMaxGroups;
static_assert(sizeof(VkDispatchIndirectCommand) == 12 && sizeof(VkDrawIndirectCommand) == 16);
static_assert(kDispatchOffset + 12 <= kReservedOffset && kReservedOffset + 4 <= kDrawOffset &&
              kDrawOffset + 16 <= kMetaOffset && kMetaOffset + 16 == kTileBytes);

// Defaults of the planner's tunable limits. They bound memory and recording cost. A plan always covers every legal
// record (ceil(2^22 / slots) tiles); a call that would need more than max_restarts physical tiles is refused as a whole,
// never truncated: a legal DispatchMesh is never silently shortened.
inline constexpr std::uint64_t kDefaultOutputBudget = 64ull << 20;  // bytes of the one output slab
// Physical tiles (render-scope restarts) one logical ExecuteIndirect may record: records * tiles.
inline constexpr std::uint64_t kDefaultMaxRestarts = 8192;

struct Tuning {
    std::uint64_t output_budget = kDefaultOutputBudget;
    std::uint64_t max_restarts = kDefaultMaxRestarts;
};

// What the planner needs of the device; zero size limits are unknown and skipped, a zero group count refuses.
struct Limits {
    Tuning tuning;
    std::uint64_t max_buffer_size = 0;
    std::uint64_t max_allocation_size = 0;
    std::uint32_t max_group_count_x = 0;  // maxComputeWorkGroupCount[0]
};

// The fixed memory and the physical work of one logical ExecuteIndirect.
struct Plan {
    std::uint32_t slots = 0;     // output slots = groups of one full tile, 1..kMaxTileGroups
    std::uint32_t tiles = 0;     // Tile entries per logical record
    std::uint32_t records = 0;   // logical records (max_count)
    std::uint64_t output_bytes = 0;   // slots * slot_bytes, one slab reused by every tile of every record
    std::uint64_t command_bytes = 0;  // tiles * kTileBytes, one slab reused by every record
    std::uint64_t restarts = 0;       // records * tiles physical tiles recorded
    // slots * tiles >= kMaxRecordGroups always: every legal DispatchMesh record is drawn in full.
};

// Checked plan. False: the PSO's slot cannot be tiled within the limits (never wraps or truncates silently), or the
// call would record more tiles than max_restarts; the caller must refuse the draw.
inline bool plan(std::uint64_t slot_bytes, std::uint32_t max_primitives, std::uint32_t records, const Limits& limits,
                 Plan& out) {
    out = {};
    if (!slot_bytes || slot_bytes % mesh_compute_direct::kOutputAlignment || !max_primitives || !records) return false;
    if (!limits.max_group_count_x || !limits.tuning.output_budget || !limits.tuning.max_restarts) return false;
    const std::uint64_t vertices_per_group = std::uint64_t(max_primitives) * 3;  // < 2^34, no overflow
    std::uint64_t slots = kMaxTileGroups;
    const auto cap = [&](std::uint64_t value) {
        if (value < slots) slots = value;
    };
    cap(limits.max_group_count_x);
    cap(limits.tuning.output_budget / slot_bytes);
    if (limits.max_buffer_size) cap(limits.max_buffer_size / slot_bytes);
    if (limits.max_allocation_size) cap(limits.max_allocation_size / slot_bytes);
    cap(UINT32_MAX / vertices_per_group);  // one tile's draw is slots * max_primitives * 3 vertices
    if (!slots) return false;
    const std::uint64_t tiles = (kMaxRecordGroups + slots - 1) / slots;  // tiles that cover any legal record, <= 2^22
    const std::uint64_t restarts = std::uint64_t(records) * tiles;  // < 2^32 * 2^22, fits
    if (restarts > limits.tuning.max_restarts) return false;
    out.slots = std::uint32_t(slots);
    out.tiles = std::uint32_t(tiles);
    out.records = records;
    out.output_bytes = slots * slot_bytes;  // <= output_budget
    out.command_bytes = tiles * kTileBytes;
    out.restarts = restarts;
    return true;
}

// A DISPATCH_MESH record the prepare pass may draw: every dimension in 1..65535 and G <= 2^22 (the D3D12 limits). A record
// with a zero dimension is a legal no-op; a record outside the limits is invalid and is emitted as zero work.
inline bool record_groups(std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint64_t& groups) {
    groups = 0;
    if (mesh_compute_direct::is_noop(x, y, z)) return true;
    if (x > mesh_compute_direct::kMaxGroupIndex || y > mesh_compute_direct::kMaxGroupIndex ||
        z > mesh_compute_direct::kMaxGroupIndex)
        return false;
    groups = std::uint64_t(x) * y * z;
    return groups <= kMaxRecordGroups;
}

// Address of tile `tile`'s TileMeta tagged for the producer's push constant (dwords 36-37). `commands_address` is the
// 16-aligned device address of tile 0.
inline std::uint64_t tagged_meta_address(std::uint64_t commands_address, std::uint32_t tile) {
    return (commands_address + std::uint64_t(tile) * kTileBytes + kMetaOffset) | kMetaTag;
}

// The command slab claimed by the prepare callback: non-null buffer and address, 16-aligned, `bytes` without wrap.
inline bool valid_commands(const void* buffer, std::uint64_t address, std::uint64_t offset, std::uint64_t bytes) {
    if (!buffer || !address || !bytes) return false;
    if (address % kTileAlignment || offset % kTileAlignment) return false;
    return address <= UINT64_MAX - bytes && offset <= UINT64_MAX - bytes;
}

// What the replay hands the prepare callback for one logical ExecuteIndirect.
struct TileRequest {
    // Physical addresses recorded by the resource registration (ResourceBinding::buffer_address + the argument offset),
    // never queried: replay refuses before calling when the buffer has none. Nothing is read on the CPU.
    VkDeviceAddress arguments_address = 0;  // record 0, 4-aligned, [arguments_address, +(records-1)*stride+12) valid
    VkDeviceAddress count_address = 0;      // the GPU-written uint32, 4-aligned; meaningful only when `counted`
    bool counted = false;                   // false: the executed count is records; true: min(*count, records)
    std::uint32_t stride = 0;             // bytes between records: >= kRecordBytes, a multiple of 4
    std::uint32_t records = 0;            // max_count >= 1; the executed count is records, or min(*count, records)
    std::uint32_t slots = 0;              // Plan::slots
    std::uint32_t tiles = 0;              // Plan::tiles: ceil(2^22 / slots), so no legal record exceeds slots * tiles
    std::uint32_t max_primitives = 0;     // the draw of a tile is n * max_primitives * 3 vertices
    // The output slab (the producer's slots, read by the consumer) is always owned by prepare: it returns it in
    // Tiles::output_*, the same grow-only slab for every call of the submission whose output_bytes fits, so hundreds
    // of calls never claim output_bytes each.
    std::uint64_t output_bytes = 0;       // Plan::output_bytes: the bytes slots * slot_bytes the slab must hold
};

// What emit_mesh_indirect_tiles pushes to the preparation pass (its commands and output addresses are Tiles').
struct PassParams {
    VkDeviceAddress arguments = 0;  // as TileRequest::arguments_address
    VkDeviceAddress count = 0;      // 0: fixed count; else as TileRequest::count_address
    std::uint32_t stride = 0, slots = 0, tiles = 0, vertices_per_group = 0, records = 0;
};

// The prepared pass of one logical ExecuteIndirect: the tile slab and its parameters. Owned by the replay's prepared
// draw; the callback keeps the slabs alive until the submission completes.
struct Tiles {
    VkBuffer commands = VK_NULL_HANDLE;      // created with INDIRECT_BUFFER (and what the prepare pass writes it with)
    VkDeviceSize commands_offset = 0;        // of tile 0 within `commands`, kTileAlignment-aligned
    VkDeviceAddress commands_address = 0;    // device address of tile 0, kTileAlignment-aligned
    // The real output slab all tiles of every record of this call write and draw from (16-aligned, >= output_bytes):
    // the consumer's output base and TileMeta words 0-1. Reused across records, tiles and calls: the replay orders
    // every reuse with barriers.
    VkBuffer output_buffer = VK_NULL_HANDLE;
    VkDeviceSize output_offset = 0;
    VkDeviceAddress output_address = 0;
    // The by-value parameters of the preparation pass; the pipeline and its layout stay with the callback's owner.
    PassParams pass;
};

}  // namespace mesh_indirect
}  // namespace leonida
