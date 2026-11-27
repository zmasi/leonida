// Direct mesh expansion: exact output bounds and the actual queue-family requirements.
#include "../src/vulkan_mesh_compute_direct.h"

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

using leonida::mesh_compute_direct::checked_counts;
using leonida::mesh_compute_direct::Counts;
using leonida::mesh_compute_direct::is_noop;
using leonida::mesh_compute_direct::queue_supports_mesh_compute;
using leonida::mesh_compute_direct::valid_output;
using leonida::mesh_compute_direct::within_compute_limits;

void test_noop() {
    CHECK(is_noop(0, 1, 1));
    CHECK(is_noop(1, 0, 1));
    CHECK(is_noop(1, 1, 0));
    CHECK(!is_noop(1, 1, 1));
    Counts out{};
    CHECK(checked_counts(0, 4, 4, 16, 1, out) && out.groups == 0 && out.bytes == 0 && out.vertices == 0);
}

void test_counts() {
    Counts out{};
    CHECK(checked_counts(1, 1, 1, 16, 1, out) && out.groups == 1 && out.bytes == 16 && out.vertices == 3);
    CHECK(checked_counts(2, 3, 4, 32, 2, out) && out.groups == 24 && out.bytes == 768 && out.vertices == 144);
    CHECK(checked_counts(65535, 1, 1, 16, 1, out) && out.vertices == 196605);
    // Each dimension bounded.
    CHECK(!checked_counts(65536, 1, 1, 16, 1, out));
    CHECK(!checked_counts(1, 65536, 1, 16, 1, out));
    CHECK(!checked_counts(1, 1, 65536, 16, 1, out));
    // Total groups bounded (256^3 = 2^24 > 2^22, each dimension legal).
    CHECK(!checked_counts(256, 256, 256, 16, 1, out));
    // Slot contract: positive and 16-aligned.
    CHECK(!checked_counts(1, 1, 1, 0, 1, out));
    CHECK(!checked_counts(1, 1, 1, 20, 1, out));
    CHECK(!checked_counts(1, 1, 1, 16, 0, out));
    // Byte product must not wrap: G=2 with the largest 16-aligned slot overflows.
    CHECK(!checked_counts(2, 1, 1, 0xFFFFFFFFFFFFFFF0ull, 1, out));
    // Draw count must fit uint32: G=2^22 with max primitives overflows the draw.
    CHECK(!checked_counts(256, 128, 128, 16, 0xFFFFFFFFu, out));
    // Draw-count boundary: 1*1431655765*3 == UINT32_MAX fits, plus one primitive does not.
    CHECK(checked_counts(1, 1, 1, 16, 1431655765u, out) && out.vertices == 0xFFFFFFFFu);
    CHECK(!checked_counts(1, 1, 1, 16, 1431655766u, out));
}

void test_compute_limits() {
    const std::uint32_t unknown[3] = {0, 0, 0};
    CHECK(!within_compute_limits(1, 1, 1, unknown));
    const std::uint32_t vega[3] = {65535, 65535, 65535};
    CHECK(within_compute_limits(65535, 65535, 1, vega));
    CHECK(!within_compute_limits(65536, 1, 1, vega));
    const std::uint32_t small[3] = {8, 8, 8};
    CHECK(!within_compute_limits(9, 1, 1, small));
}

void test_output() {
    CHECK(!valid_output(0, 0, 16));
    CHECK(!valid_output(0x1000, 0, 0));
    CHECK(!valid_output(0x1001, 0, 16));  // address misaligned
    CHECK(!valid_output(0x1000, 8, 16));  // offset misaligned
    // Base+bytes must not wrap in either base.
    CHECK(!valid_output(0xFFFFFFFFFFFFFFF0ull, 0, 16));
    CHECK(!valid_output(0x1000, 0xFFFFFFFFFFFFFFF0ull, 32));
    CHECK(valid_output(0x1000, 0x100, 0x100));
}

void test_queue_support() {
    // GRAPHICS|COMPUTE admits; GRAPHICS-only lacking COMPUTE refuses (the old transfer_only heuristic admitted it).
    CHECK(queue_supports_mesh_compute(VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT));
    CHECK(queue_supports_mesh_compute(VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT));
    CHECK(!queue_supports_mesh_compute(VK_QUEUE_GRAPHICS_BIT));
    CHECK(!queue_supports_mesh_compute(VK_QUEUE_COMPUTE_BIT));
    CHECK(!queue_supports_mesh_compute(VK_QUEUE_TRANSFER_BIT));
    CHECK(!queue_supports_mesh_compute(0));
}

}  // namespace

int main() {
    test_noop();
    test_counts();
    test_compute_limits();
    test_output();
    test_queue_support();
    std::printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
