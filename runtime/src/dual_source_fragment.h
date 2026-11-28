#pragma once

// Dual-source blending (RT-DUAL-SOURCE-BLEND). D3D12 takes the second blend source from SV_Target1, which the
// translated pixel shader declares as the output at Location 1. Vulkan takes it from Location 0, Index 1
// (VK_BLEND_FACTOR_SRC1_*, dualSrcBlend). For a pipeline whose blend state uses a SRC1 factor, the fragment module's
// Location 1 output is moved to Location 0 and given Index 1. Standard library only.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace leonida::dual_source {

// Rewrites `words` in place and returns true, or returns false and leaves them unchanged when the module is not one
// this rewrite understands: not SPIR-V, no unique Output variable at Location 1, duplicate Location decoration on an
// Output variable, an output at Location 2 or above (Vulkan has one dual-source attachment), or an Index decoration
// already present.
inline bool remap(std::vector<std::uint32_t>& words) {
    constexpr std::uint32_t kOpFunction = 54, kOpDecorate = 71, kOpVariable = 59;
    constexpr std::uint32_t kLocation = 30, kIndex = 32, kBuiltIn = 11, kStorageOutput = 3;
    if (words.size() < 5 || words[0] != 0x07230203u) return false;

    struct LocationDecl {
        std::uint32_t id;
        std::uint32_t location;
        std::size_t literal_at;
    };
    std::vector<LocationDecl> locations;
    std::vector<std::uint32_t> outputs;
    std::vector<std::uint32_t> builtins;

    // Annotations and global Output variables live before the first OpFunction (SPIR-V logical layout §2.4).
    for (std::size_t at = 5; at < words.size();) {
        const std::uint32_t n = words[at] >> 16, op = words[at] & 0xffffu;
        if (!n || n > words.size() - at) return false;
        if (op == kOpFunction) break;
        if (op == kOpDecorate && n >= 3) {
            const std::uint32_t dec = words[at + 2];
            if (dec == kLocation && n == 4) {
                locations.push_back({words[at + 1], words[at + 3], at + 3});
            } else if (dec == kIndex) {
                return false;
            } else if (dec == kBuiltIn) {
                builtins.push_back(words[at + 1]);
            }
        } else if (op == kOpVariable && n >= 4 && words[at + 3] == kStorageOutput) {
            outputs.push_back(words[at + 2]);
        }
        at += n;
    }

    if (outputs.empty() || locations.empty()) return false;
    std::sort(builtins.begin(), builtins.end());
    builtins.erase(std::unique(builtins.begin(), builtins.end()), builtins.end());
    std::sort(outputs.begin(), outputs.end());
    outputs.erase(std::unique(outputs.begin(), outputs.end()), outputs.end());
    std::sort(locations.begin(), locations.end(), [](const LocationDecl& a, const LocationDecl& b) {
        return a.id != b.id ? a.id < b.id : a.literal_at < b.literal_at;
    });

    std::uint32_t second = 0;
    std::size_t second_literal_at = 0;
    for (const std::uint32_t id : outputs) {
        if (std::binary_search(builtins.begin(), builtins.end(), id)) continue;
        const auto it = std::lower_bound(locations.begin(), locations.end(), id,
                                         [](const LocationDecl& d, std::uint32_t target) { return d.id < target; });
        if (it == locations.end() || it->id != id) continue;
        // Refuse ambiguous duplicate Location decorations on the same Output variable.
        if (it + 1 != locations.end() && (it + 1)->id == id) return false;
        if (it->location >= 2u) return false;
        if (it->location == 1u) {
            if (second) return false;  // more than one Output at Location 1
            second = id;
            second_literal_at = it->literal_at;
        }
    }
    if (!second) return false;

    words[second_literal_at] = 0u;
    // OpDecorate %second Index 1, right after its Location decoration (annotations stay together).
    const std::array<std::uint32_t, 4> index{(4u << 16) | kOpDecorate, second, kIndex, 1u};
    words.insert(words.begin() + static_cast<std::ptrdiff_t>(second_literal_at + 1), index.begin(), index.end());
    return true;
}

}  // namespace leonida::dual_source
