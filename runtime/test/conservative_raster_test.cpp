#include "../src/conservative_raster.h"
#include <cstdio>
#include <limits>
#include <vector>

int main() {
    using leonida::conservative_raster::tier1;
    unsigned failures = 0;
    const auto check = [&](bool value) { if (!value) ++failures; };
    check(tier1(true, 8, 0.0f));
    check(tier1(true, 8, 1.0f / 256.0f));
    check(tier1(true, 8, 0.5f - 1.0f / 512.0f));
    check(!tier1(false, 8, 0.0f));
    check(!tier1(true, 7, 0.0f));
    check(!tier1(true, 8, 0.5f));
    check(!tier1(true, 8, -0.001f));
    check(!tier1(true, 8, std::numeric_limits<float>::infinity()));
    check(!tier1(true, 8, std::numeric_limits<float>::quiet_NaN()));
    using leonida::conservative_raster::tier;
    check(tier(false, 8, 0, true) == 0);
    check(tier(true, 7, 0, true) == 0);
    check(tier(true, 8, 0, true) == 2);
    check(tier(true, 8, 0, false) == 1);
    check(tier(true, 8, 1.0f / 512.0f, true) == 2);
    check(tier(true, 8, std::nextafter(1.0f / 512.0f, 1.0f), true) == 1);
    check(tier(true, 8, 1.0f / 256.0f, true) == 1);
    check(tier(true, 16, 0.003f, true) == 2);
    check(tier(true, 16, 0.003f, false) == 1);
    check(tier(true, 8, std::numeric_limits<float>::quiet_NaN(), true) == 0);
    check(tier(true, 8, 0, true, true, true) == 3);
    check(tier(true, 8, 0, true, false, true) == 2);
    check(tier(true, 8, 0, true, true, false) == 2);
    check(tier(true, 8, 0, false, true, true) == 1);
    check(tier(false, 8, 0, true, true, true) == 0);
    check(tier(true, 8, 1.0f / 256.0f, true, true, true) == 1);
    using leonida::conservative_raster::inner_coverage_requirements;
    const std::uint32_t module[] = {0x07230203, 0x00010500, 0, 2, 0, (2u << 16) | 17, 5265,
                                  (3u << 16) | 16, 1, 4446};
    const auto requirements = inner_coverage_requirements(module);
    check(requirements.valid && requirements.fully_covered && requirements.post_depth);
    check(!inner_coverage_requirements(std::span(module).first(9)).valid);
    using leonida::conservative_raster::mesh_outputs_triangles;
    const auto with_mode = [](std::uint32_t mode) {
        return std::vector<std::uint32_t>{0x07230203, 0x00010500, 0, 2, 0, (3u << 16) | 16, 1, mode};
    };
    check(mesh_outputs_triangles(with_mode(5298)));   // OutputTrianglesEXT
    check(!mesh_outputs_triangles(with_mode(5269)));  // OutputLinesEXT
    check(!mesh_outputs_triangles(with_mode(27)));    // OutputPoints
    check(!mesh_outputs_triangles(with_mode(26)));    // OutputVertices: no output topology
    check(!mesh_outputs_triangles({}));
    auto truncated = with_mode(5298);
    truncated.pop_back();
    check(!mesh_outputs_triangles(truncated));
    const std::uint32_t zero_count[] = {0x07230203, 0x00010500, 0, 2, 0, 0};
    check(!inner_coverage_requirements(zero_count).valid);
    const std::uint32_t ordinary[] = {0x07230203, 0x00010500, 0, 2, 0, (2u << 16) | 17, 1};
    const auto regular = inner_coverage_requirements(ordinary);
    check(regular.valid && !regular.fully_covered && !regular.post_depth);
    // Header validation: too short and wrong magic.
    check(!inner_coverage_requirements({}).valid);
    check(!inner_coverage_requirements(std::span(ordinary).first(4)).valid);
    const std::uint32_t bad_magic[] = {0x12345678, 0x00010500, 0, 2, 0};
    check(!inner_coverage_requirements(bad_magic).valid);
    // Header-only module (no instructions) is valid with neither requirement.
    const auto header_only = inner_coverage_requirements(std::span(ordinary).first(5));
    check(header_only.valid && !header_only.fully_covered && !header_only.post_depth);
    // Full preamble followed by debug/annotations/types/function body stops at the post-execution-mode boundary.
    {
        std::vector<std::uint32_t> full = {
            0x07230203, 0x00010500, 0, 10, 0,
            (2u << 16) | 17, 1,                 // OpCapability Shader
            (2u << 16) | 17, 5265,              // OpCapability FragmentFullyCoveredEXT
            (3u << 16) | 14, 0, 1,              // OpMemoryModel Logical GLSL450
            (4u << 16) | 15, 4, 1, 0x006e6961,  // OpEntryPoint Fragment %1 "ain"
            (3u << 16) | 16, 1, 7,              // OpExecutionMode %1 OriginUpperLeft
            (3u << 16) | 16, 1, 4446,           // OpExecutionMode %1 PostDepthCoverage
            (4u << 16) | 71, 2, 30, 0,          // OpDecorate %2 Location 0 (preamble stop)
            (2u << 16) | 19, 3,                 // OpTypeVoid %3
            (5u << 16) | 54, 3, 1, 0, 4,        // OpFunction ...
        };
        for (int k = 0; k < 2000; ++k) {
            full.push_back((1u << 16) | 0);     // OpNop body padding
        }
        const auto r = inner_coverage_requirements(full);
        check(r.valid && r.fully_covered && r.post_depth);
    }
    // Preamble stop triggers at OpName, OpDecorate, OpTypeVoid, OpConstant, OpVariable, and OpFunction:
    // placing a sentinel word (zero count) strictly AFTER the stop instruction confirms the scanner stopped
    // without touching the rest of the module, while a truncated stop instruction itself is still rejected.
    const std::uint32_t stop_ops[][4] = {
        {(3u << 16) | 5, 1, 0x0061, 3},      // OpName (3 words)
        {(4u << 16) | 71, 2, 30, 0},         // OpDecorate (4 words)
        {(2u << 16) | 19, 3, 0, 2},          // OpTypeVoid (2 words)
        {(4u << 16) | 43, 3, 4, 0},          // OpConstant (4 words)
        {(4u << 16) | 59, 3, 5, 2},          // OpVariable (4 words)
        {(2u << 16) | 54, 3, 0, 2},          // OpFunction (minimal 2-word framing)
    };
    for (const auto& stop : stop_ops) {
        const std::uint32_t stop_len = stop[0] >> 16;
        std::vector<std::uint32_t> m = {
            0x07230203, 0x00010500, 0, 10, 0,
            (2u << 16) | 17, 5265,              // OpCapability FragmentFullyCoveredEXT
            (3u << 16) | 14, 0, 1,              // OpMemoryModel
        };
        for (std::uint32_t w = 0; w < stop_len; ++w) m.push_back(stop[w]);
        // Truncated stop instruction must be rejected before stopping.
        check(!inner_coverage_requirements(std::span(m).first(m.size() - 1)).valid);
        // Trailing unvisited words (including a zero-word sentinel) prove scanning stopped at the boundary.
        m.push_back(0);
        const auto r = inner_coverage_requirements(m);
        check(r.valid && r.fully_covered && !r.post_depth);
    }
    if (failures) std::fprintf(stderr, "conservative raster capability checks: %u failures\n", failures);
    else std::puts("conservative raster: enabled extension, precision and uncertainty bounds checked");
    return failures ? 1 : 0;
}
