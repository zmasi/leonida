#pragma once
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <span>

namespace leonida::conservative_raster {
// Tier 1 bounds the outer uncertainty to half a pixel and requires a 16.8 rasterization grid. Leave room for
// half a grid step at that minimum precision in addition to Vulkan's fixed primitive overestimation. Do not
// infer Tier 3 from degenerate-triangle support: InnerCoverage also needs an explicit shader ABI opt-in.
inline bool tier1(bool extension_enabled, std::uint32_t subpixel_bits, float overestimate) {
    constexpr float maximum = 0.5f - 1.0f / 512.0f;
    return extension_enabled && subpixel_bits >= 8 && std::isfinite(overestimate) &&
           overestimate >= 0.0f && overestimate <= maximum;
}
inline std::uint32_t tier(bool extension_enabled, std::uint32_t subpixel_bits, float overestimate,
                          bool degenerate_triangles, bool fully_covered = false, bool inner_coverage_abi = false) {
    if (!tier1(extension_enabled, subpixel_bits, overestimate)) return 0;
    // Tier 2 allows 1/256 pixel total uncertainty and requires post-snap degenerates. Reserve half of the
    // device's actual raster grid step, conservatively capped at float's useful precision, for snapping.
    const float snap = std::ldexp(0.5f, -int(std::min(subpixel_bits, 24u)));
    if (!degenerate_triangles || overestimate > 1.0f / 256.0f - snap) return 1;
    // The physical property alone does not establish the guest shader-input ABI. Caller-owned libraries can
    // opt in once their InnerCoverage input is lowered to FullyCoveredEXT; the standalone device does not.
    return fully_covered && inner_coverage_abi ? 3u : 2u;
}

struct InnerCoverageRequirements {
    bool valid = false;
    bool fully_covered = false;
    bool post_depth = false;
};

// Inspect the actual module, not optional index metadata, before vkCreateShaderModule. Every valid use of
// FullyCoveredEXT requires FragmentFullyCoveredEXT; PostDepthCoverage has an additional device property.
// Per SPIR-V Logical Layout (§2.4), OpCapability (17) and OpExecutionMode (16) precede all debug,
// annotation, type, constant, global variable, and function sections, so scanning stops at the first
// post-execution-mode instruction rather than walking the full module body.
inline InnerCoverageRequirements inner_coverage_requirements(std::span<const std::uint32_t> words) {
    constexpr auto is_post_execution_mode = [](std::uint32_t op) noexcept {
        switch (op) {
            case 1:     // OpUndef
            case 2:     // OpSourceContinued
            case 3:     // OpSource
            case 4:     // OpSourceExtension
            case 5:     // OpName
            case 6:     // OpMemberName
            case 7:     // OpString
            case 54:    // OpFunction
            case 59:    // OpVariable
            case 71:    // OpDecorate
            case 72:    // OpMemberDecorate
            case 73:    // OpDecorationGroup
            case 74:    // OpGroupDecorate
            case 75:    // OpGroupMemberDecorate
            case 330:   // OpModuleProcessed
            case 332:   // OpDecorateId
            case 5632:  // OpDecorateString
            case 5633:  // OpMemberDecorateString
                return true;
            default:
                return (op >= 19 && op <= 39) || (op >= 41 && op <= 52);  // OpType* / OpConstant*
        }
    };
    InnerCoverageRequirements result;
    if (words.size() < 5 || words[0] != 0x07230203) return result;
    for (std::size_t i = 5; i < words.size();) {
        const auto count = words[i] >> 16, op = words[i] & 0xffff;
        if (!count || count > words.size() - i) return result;
        if (is_post_execution_mode(op)) break;
        if (op == 17 && count >= 2 && words[i + 1] == 5265) result.fully_covered = true;
        if (op == 16 && count >= 3 && words[i + 2] == 4446) result.post_depth = true;
        i += count;
    }
    result.valid = true;
    return result;
}

// True when the mesh shader module outputs triangles (OutputTrianglesEXT, 5298). A module with lines (5269), points (27)
// or no output mode does not. Each mesh entry point of a valid module has exactly one of the three.
inline bool mesh_outputs_triangles(std::span<const std::uint32_t> words) {
    if (words.size() < 5 || words[0] != 0x07230203) return false;
    bool triangles = false;
    for (std::size_t i = 5; i < words.size();) {
        const auto count = words[i] >> 16, op = words[i] & 0xffff;
        if (!count || count > words.size() - i) return false;
        if (op == 16 && count >= 3) {  // OpExecutionMode
            if (words[i + 2] == 5298) triangles = true;
            else if (words[i + 2] == 5269 || words[i + 2] == 27) return false;
        }
        i += count;
    }
    return triangles;
}
} // namespace leonida::conservative_raster
