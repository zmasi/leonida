// clzero_patch.h: the two CLZERO loop shapes the title uses are found, rewritten to jump to stubs, and the stubs
// zero exactly the lines the loops would zero and leave the registers the loops leave. The loops are hand-assembled
// here (no game code), and only the patched copies run, so the test runs on any x86-64 CPU.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "clzero_patch.h"

namespace {

int failures = 0;
int checks = 0;

void check(bool ok, const char* what) {
    ++checks;
    if (!ok) ++failures;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
}

// uint64_t f(void* begin, void* end): shape A over [begin, end), returns rax at the loop's exit.
const uint8_t kShapeA[] = {
    0x48, 0x89, 0xC8,        // mov rax, rcx
    0x49, 0x89, 0xD2,        // mov r10, rdx
    0x0F, 0x01, 0xFC,        // L: clzero
    0x48, 0x83, 0xC0, 0x40,  // add rax, 40h
    0x4C, 0x39, 0xD0,        // cmp rax, r10
    0x72, 0xF4,              // jb L
    0xC3,                    // ret
};

// uint64_t f(void* begin, void* end): shape B over [begin, end), returns the counter at the loop's exit.
const uint8_t kShapeB[] = {
    0x49, 0x89, 0xCA,        // mov r10, rcx
    0x49, 0x89, 0xD3,        // mov r11, rdx
    0x4C, 0x89, 0xD0,        // L: mov rax, r10
    0x49, 0x83, 0xC2, 0x40,  // add r10, 40h
    0x0F, 0x01, 0xFC,        // clzero
    0x4D, 0x39, 0xDA,        // cmp r10, r11
    0x72, 0xF1,              // jb L
    0x4C, 0x89, 0xD0,        // mov rax, r10
    0xC3,                    // ret
};

// Shape A with cmp rax, rcx: rcx is the register the stub saves, so this loop is left alone.
const uint8_t kShapeAWithRcx[] = {
    0x0F, 0x01, 0xFC,        // clzero
    0x48, 0x83, 0xC0, 0x40,  // add rax, 40h
    0x48, 0x39, 0xC8,        // cmp rax, rcx
    0x72, 0xF4,              // jb
    0xC3,
};

using Loop = uint64_t (*)(void*, void*);

// Fills a buffer, runs `loop` over [buf + first, buf + last) and checks which bytes changed.
bool zeroes_exactly(Loop loop, size_t first, size_t last, uint64_t& returned) {
    alignas(64) static uint8_t buf[4096];
    std::memset(buf, 0xAB, sizeof buf);
    returned = loop(buf + first, buf + last);
    for (size_t i = 0; i < sizeof buf; ++i) {
        const bool inside = i >= (first & ~size_t(63)) && i < last;
        if (buf[i] != (inside ? 0 : 0xAB)) return false;
    }
    return returned == reinterpret_cast<uint64_t>(buf + last);
}

}  // namespace

int main() {
    namespace cz = rt::clzero_patch;
    {
        const auto a = cz::find(kShapeA, sizeof kShapeA);
        check(a.size() == 1 && a[0].head == 6 && a[0].length == 12 && a[0].clzero == 0, "shape A is found at its CLZERO");
        const auto b = cz::find(kShapeB, sizeof kShapeB);
        check(b.size() == 1 && b[0].head == 6 && b[0].length == 15 && b[0].clzero == 7,
              "shape B is found at its mov rax, CUR");
        check(cz::find(kShapeAWithRcx, sizeof kShapeAWithRcx).empty(), "a loop that compares against rcx is left alone");
        const uint8_t lone[] = {0x0F, 0x01, 0xFC, 0xC3};
        check(cz::find(lone, sizeof lone).empty(), "a CLZERO outside both loop shapes is left alone");
        check(cz::find(nullptr, 64).empty(), "null code buffer returns no sites");
        check(cz::stub_page(0x1000, 0x2000, 0) == nullptr, "stub_page rejects size 0");
        check(cz::stub_page(0x2000, 0x1000, 0x10000) == nullptr, "stub_page rejects inverted [lo, hi)");
        int32_t rel = 0;
        check(cz::rel32_offset(0x100000000ull, 0x100000000ull + INT32_MAX, rel) && rel == INT32_MAX,
              "rel32_offset accepts INT32_MAX");
        check(!cz::rel32_offset(0x100000000ull, 0x100000000ull + uint64_t(INT32_MAX) + 1, rel),
              "rel32_offset rejects INT32_MAX + 1");
        check(cz::rel32_offset(0x180000000ull, 0x100000000ull, rel) && rel == INT32_MIN,
              "rel32_offset accepts INT32_MIN");
        check(!cz::rel32_offset(0x180000001ull, 0x100000000ull, rel),
              "rel32_offset rejects INT32_MIN - 1");
        const cz::Site site_a{6, 12, 0};
        check(cz::stub(kShapeA + 6, site_a, 0x100000000ull, 0x500000000ull).empty(),
              "stub refuses resume address beyond +/-2 GiB");
    }

    auto* code = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    check(code != nullptr, "code page");
    if (!code) return 1;
    std::memcpy(code, kShapeA, sizeof kShapeA);
    std::memcpy(code + 0x100, kShapeB, sizeof kShapeB);
    const auto r = cz::patch({{code, 0x1000}}, uint64_t(code), uint64_t(code) + 0x1000);
    check(r.sites == 2 && r.stubs != nullptr, "both loops are patched");
    check(code[6] == 0xE9 && code[0x106] == 0xE9, "each loop starts with a jmp rel32 to its stub");
    const int64_t distance = int64_t(r.stubs) - int64_t(code);
    check(distance > -(int64_t(1) << 31) && distance < (int64_t(1) << 31), "the stubs are within 2 GiB of the code");
    FlushInstructionCache(GetCurrentProcess(), code, 0x1000);

    const auto shape_a = reinterpret_cast<Loop>(code);
    const auto shape_b = reinterpret_cast<Loop>(code + 0x100);
    uint64_t ret = 0;
    check(zeroes_exactly(shape_a, 64, 64 + 1024, ret), "shape A zeroes its 16 lines and returns rax = end");
    check(zeroes_exactly(shape_a, 128, 192, ret), "shape A runs once for a one-line range");
    check(zeroes_exactly(shape_b, 256, 256 + 0x380, ret), "shape B zeroes its 14 lines and leaves CUR = end");
    {
        // An unaligned start: CLZERO zeroes the whole line rAX points into, and the loop steps by 64 from rAX.
        alignas(64) static uint8_t buf[512];
        std::memset(buf, 0xAB, sizeof buf);
        shape_a(buf + 64 + 8, buf + 64 + 8 + 128);
        bool ok = true;
        for (size_t i = 0; i < sizeof buf; ++i) ok &= buf[i] == ((i >= 64 && i < 64 + 128) ? 0 : 0xAB);
        check(ok, "an unaligned start zeroes the lines rAX points into");
    }

    std::printf("clzero_patch_test: %s (%d checks)\n", failures ? "FAILED" : "OK", checks);
    return failures ? 1 : 0;
}
