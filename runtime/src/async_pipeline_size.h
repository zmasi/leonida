// Which pipelines LEONIDA_ASYNC_PIPELINES=1 builds asynchronously, by size (LEONIDA_ASYNC_MIN_BYTES). Pure stdlib, so
// async_pipeline_size_test checks it without a device; shims/d3d12_pipeline_compile.h reads the variables.
//
// An asynchronous pipeline that is not built yet has its draws and dispatches skipped until the compile worker has
// it (queue_build in shims/d3d12_vulkan_device.cpp). With every pipeline asynchronous the small lighting and post
// programs are skipped with the large ones, and the screen can stay black for as long as anything is compiling; with
// none, a submission waits minutes for each large ray-tracing program. LEONIDA_ASYNC_MIN_BYTES=<n> splits the two: a
// pipeline with less than n bytes of SPIR-V (compute: its module; graphics: all its stages) is built where it is
// needed, as without LEONIDA_ASYNC_PIPELINES, and only one of n bytes or more is asynchronous.
// LEONIDA_ASYNC_MIN_BYTES_GRAPHICS=<n> sets the graphics (and mesh) pipelines' threshold apart from compute's: a draw
// whose pipeline is skipped loses one draw, while the lighting and post compute programs make the image, so graphics
// can be left asynchronous (0) while small compute programs are built in place. Unset: graphics take
// LEONIDA_ASYNC_MIN_BYTES, as before the variable.
#pragma once

#include <cstdint>

namespace leonida::async_pipelines {

// LEONIDA_ASYNC_MIN_BYTES as text: decimal bytes. 0 (every pipeline asynchronous, as without the variable) for null,
// empty, "0" or text that is not a whole decimal number; `valid` (when given) says whether the text was usable, so the
// caller can say that it ignored it.
inline std::uint64_t parse_min_bytes(const char* text, bool* valid = nullptr) {
    if (valid) *valid = true;
    if (!text || !*text) return 0;
    std::uint64_t value = 0;
    for (const char* c = text; *c; ++c) {
        const std::uint64_t digit = std::uint64_t(*c - '0');
        if (*c < '0' || *c > '9' || value > (UINT64_MAX - digit) / 10) {  // not a digit, or over 2^64 - 1
            if (valid) *valid = false;
            return 0;
        }
        value = value * 10 + digit;
    }
    return value;
}

// Both thresholds, as read from the environment: `compute` is LEONIDA_ASYNC_MIN_BYTES, `graphics` is
// LEONIDA_ASYNC_MIN_BYTES_GRAPHICS or, when that is unset, empty or not a number, the same as `compute`.
struct MinBytes {
    std::uint64_t compute = 0;
    std::uint64_t graphics = 0;
    bool graphics_set = false;  // LEONIDA_ASYNC_MIN_BYTES_GRAPHICS gave the graphics threshold
};

// `all_text`: LEONIDA_ASYNC_MIN_BYTES; `graphics_text`: LEONIDA_ASYNC_MIN_BYTES_GRAPHICS (null when unset). The
// `valid` flags (when given) say whether each text was usable; an unset or empty one is valid.
inline MinBytes parse_thresholds(const char* all_text, const char* graphics_text, bool* all_valid = nullptr,
                                 bool* graphics_valid = nullptr) {
    MinBytes out;
    out.compute = parse_min_bytes(all_text, all_valid);
    bool valid = true;
    const std::uint64_t graphics = parse_min_bytes(graphics_text, &valid);
    if (graphics_valid) *graphics_valid = valid;
    out.graphics_set = valid && graphics_text && *graphics_text;
    out.graphics = out.graphics_set ? graphics : out.compute;
    return out;
}

// Whether a pipeline of `spirv_bytes` is built asynchronously: only with asynchronous builds on, and then when no
// threshold is set (0) or the pipeline is at least that large.
inline bool asynchronous(bool async_on, std::uint64_t min_bytes, std::uint64_t spirv_bytes) {
    return async_on && (min_bytes == 0 || spirv_bytes >= min_bytes);
}

// The same for a graphics (or mesh) pipeline, whose size is all its stages, or a compute one (its module).
inline bool asynchronous(bool async_on, const MinBytes& thresholds, bool graphics, std::uint64_t spirv_bytes) {
    return asynchronous(async_on, graphics ? thresholds.graphics : thresholds.compute, spirv_bytes);
}

}  // namespace leonida::async_pipelines
