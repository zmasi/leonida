// Which pipelines LEONIDA_ASYNC_PIPELINES=1 builds asynchronously by size, LEONIDA_ASYNC_MIN_BYTES and
// LEONIDA_ASYNC_MIN_BYTES_GRAPHICS (runtime/src/async_pipeline_size.h): the thresholds' parsing, the graphics one's
// fallback to LEONIDA_ASYNC_MIN_BYTES, and the decision for each kind of pipeline. Pure stdlib, no Vulkan.
#include "../src/async_pipeline_size.h"

#include <cstdint>
#include <cstdio>
#include <initializer_list>

namespace {

int failures = 0;

#define CHECK(condition) do {                                              \
    if (!(condition)) {                                                    \
        std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition);  \
        ++failures;                                                        \
    }                                                                      \
} while (0)

using leonida::async_pipelines::asynchronous;
using leonida::async_pipelines::MinBytes;
using leonida::async_pipelines::parse_min_bytes;
using leonida::async_pipelines::parse_thresholds;

void parsing() {
    bool valid = false;
    // Unset or empty: no threshold, and nothing to complain about.
    CHECK(parse_min_bytes(nullptr, &valid) == 0 && valid);
    CHECK(parse_min_bytes("", &valid) == 0 && valid);
    CHECK(parse_min_bytes("0", &valid) == 0 && valid);
    CHECK(parse_min_bytes("2097152", &valid) == 2097152 && valid);
    CHECK(parse_min_bytes("1", &valid) == 1 && valid);
    CHECK(parse_min_bytes("18446744073709551615", &valid) == UINT64_MAX && valid);
    CHECK(parse_min_bytes("2097152") == 2097152);  // `valid` is optional
    // Not a whole decimal number of bytes: ignored (0, every pipeline asynchronous) and reported as such.
    for (const char* text : {"2M", "2MiB", "-1", " 1", "1 ", "0x200000", "1.5", "abc", "18446744073709551616",
                             "99999999999999999999"}) {
        valid = true;
        CHECK(parse_min_bytes(text, &valid) == 0);
        CHECK(!valid);
        if (valid) std::fprintf(stderr, "  accepted: \"%s\"\n", text);
    }
}

void decision() {
    constexpr std::uint64_t mib = 1u << 20;
    // Asynchronous builds off: nothing is asynchronous, whatever the threshold.
    for (std::uint64_t threshold : {std::uint64_t{0}, std::uint64_t{1}, 2 * mib})
        for (std::uint64_t bytes : {std::uint64_t{0}, std::uint64_t{4}, 2 * mib - 1, 2 * mib, 26 * mib})
            CHECK(!asynchronous(false, threshold, bytes));
    // On without a threshold (unset, or 0): every pipeline, as before the variable, the smallest included.
    for (std::uint64_t bytes : {std::uint64_t{0}, std::uint64_t{4}, 64 * std::uint64_t{1024}, 2 * mib, 26 * mib})
        CHECK(asynchronous(true, 0, bytes));
    // On with 2 MiB: under it waits, at it and over it is asynchronous.
    CHECK(!asynchronous(true, 2 * mib, 0));
    CHECK(!asynchronous(true, 2 * mib, 4));
    CHECK(!asynchronous(true, 2 * mib, 512 * std::uint64_t{1024}));  // a lighting or post program
    CHECK(!asynchronous(true, 2 * mib, 2 * mib - 1));
    CHECK(asynchronous(true, 2 * mib, 2 * mib));
    CHECK(asynchronous(true, 2 * mib, 26 * mib));  // the largest ray-tracing resolve programs
    // A threshold of 1 byte: as unset for any pipeline with code.
    CHECK(asynchronous(true, 1, 4));
    // A threshold over every pipeline: nothing asynchronous, as with asynchronous builds off.
    CHECK(!asynchronous(true, UINT64_MAX, 26 * mib));
    CHECK(asynchronous(true, UINT64_MAX, UINT64_MAX));
}

// LEONIDA_ASYNC_MIN_BYTES_GRAPHICS: its own threshold for graphics when set, LEONIDA_ASYNC_MIN_BYTES's otherwise.
void graphics_thresholds() {
    bool all_valid = false, graphics_valid = false;
    // Both unset: no threshold for either, as before both variables.
    MinBytes t = parse_thresholds(nullptr, nullptr, &all_valid, &graphics_valid);
    CHECK(t.compute == 0 && t.graphics == 0 && !t.graphics_set && all_valid && graphics_valid);
    // Graphics unset or empty: the same threshold as compute (behaviour unchanged from LEONIDA_ASYNC_MIN_BYTES alone).
    for (const char* graphics : {static_cast<const char*>(nullptr), ""}) {
        t = parse_thresholds("1048576", graphics, &all_valid, &graphics_valid);
        CHECK(t.compute == 1048576 && t.graphics == 1048576 && !t.graphics_set && all_valid && graphics_valid);
    }
    // Graphics 0: every graphics pipeline asynchronous, compute keeps its threshold.
    t = parse_thresholds("1048576", "0", &all_valid, &graphics_valid);
    CHECK(t.compute == 1048576 && t.graphics == 0 && t.graphics_set && all_valid && graphics_valid);
    // Graphics set without the general one: compute has no threshold (every compute pipeline asynchronous).
    t = parse_thresholds(nullptr, "2097152", &all_valid, &graphics_valid);
    CHECK(t.compute == 0 && t.graphics == 2097152 && t.graphics_set && all_valid && graphics_valid);
    // Graphics higher than compute.
    t = parse_thresholds("4096", "18446744073709551615", &all_valid, &graphics_valid);
    CHECK(t.compute == 4096 && t.graphics == UINT64_MAX && t.graphics_set);
    // Graphics text that is not a number: reported, and graphics fall back to LEONIDA_ASYNC_MIN_BYTES.
    for (const char* text : {"1M", "-1", "0x10", " 0", "abc", "18446744073709551616"}) {
        t = parse_thresholds("1048576", text, &all_valid, &graphics_valid);
        CHECK(t.compute == 1048576 && t.graphics == 1048576 && !t.graphics_set && all_valid && !graphics_valid);
        if (graphics_valid) std::fprintf(stderr, "  accepted: \"%s\"\n", text);
    }
    // The general one not a number: compute has no threshold, and an unset graphics one follows it.
    t = parse_thresholds("2M", nullptr, &all_valid, &graphics_valid);
    CHECK(t.compute == 0 && t.graphics == 0 && !t.graphics_set && !all_valid && graphics_valid);
    t = parse_thresholds("2M", "0", &all_valid, &graphics_valid);
    CHECK(t.compute == 0 && t.graphics == 0 && t.graphics_set && !all_valid && graphics_valid);
    t = parse_thresholds("2M", "65536", &all_valid, &graphics_valid);
    CHECK(t.compute == 0 && t.graphics == 65536 && t.graphics_set && !all_valid && graphics_valid);
    // The flags are optional.
    t = parse_thresholds("8", "16");
    CHECK(t.compute == 8 && t.graphics == 16 && t.graphics_set);
}

// The decision per kind: a graphics pipeline (all its stages) against the graphics threshold, a compute one (its
// module) against LEONIDA_ASYNC_MIN_BYTES.
void per_kind_decision() {
    constexpr std::uint64_t kib = 1024, mib = kib * kib;
    // Graphics unset: both kinds decide exactly as asynchronous(on, LEONIDA_ASYNC_MIN_BYTES, bytes) did.
    for (const char* all : {static_cast<const char*>(nullptr), "0", "1", "1048576", "2097152", "18446744073709551615"}) {
        const MinBytes t = parse_thresholds(all, nullptr);
        const std::uint64_t threshold = parse_min_bytes(all);
        for (bool on : {false, true})
            for (std::uint64_t bytes : {std::uint64_t{0}, std::uint64_t{4}, 512 * kib, mib - 1, mib, 2 * mib, 26 * mib})
                for (bool graphics : {false, true})
                    CHECK(asynchronous(on, t, graphics, bytes) == asynchronous(on, threshold, bytes));
    }
    // LEONIDA_ASYNC_MIN_BYTES=1 MiB with graphics 0 (the draw-thread case): every graphics pipeline asynchronous, the
    // smallest included; a compute pipeline under 1 MiB (lighting, post) is built in place, one of 1 MiB or more is not.
    const MinBytes split = parse_thresholds("1048576", "0");
    for (std::uint64_t bytes : {std::uint64_t{0}, std::uint64_t{4}, 64 * kib, mib - 1, mib, 2600 * kib})
        CHECK(asynchronous(true, split, true, bytes));
    CHECK(!asynchronous(true, split, false, 4));
    CHECK(!asynchronous(true, split, false, 512 * kib));
    CHECK(!asynchronous(true, split, false, mib - 1));
    CHECK(asynchronous(true, split, false, mib));
    CHECK(asynchronous(true, split, false, 26 * mib));
    // Asynchronous builds off: nothing, whatever the thresholds.
    for (std::uint64_t bytes : {std::uint64_t{0}, std::uint64_t{4}, 26 * mib}) {
        CHECK(!asynchronous(false, split, true, bytes));
        CHECK(!asynchronous(false, split, false, bytes));
    }
    // A graphics threshold over compute's: graphics under it wait while compute of the same size is asynchronous.
    const MinBytes high = parse_thresholds("65536", "4194304");
    CHECK(!asynchronous(true, high, true, 2 * mib));
    CHECK(asynchronous(true, high, false, 2 * mib));
    CHECK(asynchronous(true, high, true, 4 * mib));
    CHECK(!asynchronous(true, high, false, 4 * kib));
}

}  // namespace

int main() {
    parsing();
    decision();
    graphics_thresholds();
    per_kind_decision();
    if (failures) {
        std::fprintf(stderr, "async_pipeline_size_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("async_pipeline_size_test: ok\n");
    return 0;
}
