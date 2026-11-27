// Pure checked arithmetic for direct DispatchMesh expansion (mesh-as-compute, no
// VK_EXT_mesh_shader). No Vulkan calls: unit-testable without a device. The replay
// (vulkan_replay.h) uses it to validate a logical Draw::mesh against the PSO's slot
// contract before claiming submission scratch.
#pragma once

#include <cstdint>
#include <vulkan/vulkan_core.h>

namespace leonida {
namespace mesh_compute_direct {

// D3D12 DispatchMesh limits: each dimension <= 65535, total groups <= 2^22.
inline constexpr std::uint32_t kMaxGroupIndex = 65535;
inline constexpr std::uint64_t kMaxGroups = 1ull << 22;
// Output slots are 16-byte aligned; the scratch serving them is 256-byte aligned.
inline constexpr std::uint64_t kOutputAlignment = 16;
// Push-constant contract: 32 graphics root dwords + heap/code + output BDA.
inline constexpr std::uint32_t kPushBytes = 152;

struct Counts {
    std::uint64_t groups = 0;   // G = x*y*z
    std::uint64_t bytes = 0;    // G*slot_bytes
    std::uint32_t vertices = 0;  // G*max_primitives*3, for vkCmdDraw
};

inline bool is_noop(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    return x == 0 || y == 0 || z == 0;
}

// Checked G/bytes/vertexCount. Zero in any dimension is a no-op (zeros, true) with
// no allocation. False: over-limit, misaligned, or unrepresentable; the caller must
// refuse, never wrap, truncate, or split the grid (group IDs must be preserved).
inline bool checked_counts(std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint64_t slot_bytes,
                           std::uint32_t max_primitives, Counts& out) {
    out = {};
    if (is_noop(x, y, z)) return true;
    if (x > kMaxGroupIndex || y > kMaxGroupIndex || z > kMaxGroupIndex) return false;
    if (!slot_bytes || (slot_bytes % kOutputAlignment) != 0) return false;
    if (!max_primitives) return false;
    // x,y,z <= 65535: the product is below 2^48, no 64-bit overflow here.
    const std::uint64_t groups = std::uint64_t(x) * y * z;
    if (!groups || groups > kMaxGroups) return false;
    if (slot_bytes > UINT64_MAX / groups) return false;
    const std::uint64_t bytes = groups * slot_bytes;
    if (!bytes) return false;
    if (groups > UINT64_MAX / max_primitives) return false;
    const std::uint64_t prims = groups * max_primitives;
    if (prims > UINT64_MAX / 3) return false;
    const std::uint64_t verts = prims * 3;
    if (!verts || verts > UINT32_MAX) return false;
    out.groups = groups;
    out.bytes = bytes;
    out.vertices = std::uint32_t(verts);
    return true;
}

// Physical compute grid limits (maxComputeWorkGroupCount). All-zero means unknown:
// the caller must refuse direct draws, not skip the check.
inline bool within_compute_limits(std::uint32_t x, std::uint32_t y, std::uint32_t z,
                                  const std::uint32_t max_count[3]) {
    if (!max_count[0] || !max_count[1] || !max_count[2]) return false;
    return x <= max_count[0] && y <= max_count[1] && z <= max_count[2];
}

// A claimed output region: non-null 16-aligned address/offset, non-empty, no wrap in either base+bytes.
inline bool valid_output(std::uint64_t address, std::uint64_t offset, std::uint64_t bytes) {
    if (!address || !bytes) return false;
    if ((address % kOutputAlignment) != 0 || (offset % kOutputAlignment) != 0) return false;
    if (address > UINT64_MAX - bytes) return false;
    return offset <= UINT64_MAX - bytes;
}

// Executing queue support: the exact family must expose GRAPHICS|COMPUTE (dispatch plus dynamic rendering).
// A GRAPHICS-only family lacking COMPUTE refuses, as do COMPUTE-only, TRANSFER-only, and empty flags.
inline bool queue_supports_mesh_compute(VkQueueFlags flags) {
    constexpr VkQueueFlags need = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    return (flags & need) == need;
}

}  // namespace mesh_compute_direct
}  // namespace leonida
