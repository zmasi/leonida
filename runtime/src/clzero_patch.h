// Moves the title's CLZERO loops off the #UD path on a CPU without CLZERO (docs/cpu_features.md).
//
// The executable's 10 CLZERO sites are one leaf routine that zeroes buffers a 64-byte line at a time. Emulated on #UD
// (cpu_emulation.h), every line costs an exception dispatch and two VirtualQuery calls: measured on an i9-13900K,
// the routine took ~99% of one thread for the whole boot and the title presented at ~3 frames a second. Each site sits
// in one of two loop shapes, which are matched exactly (register forms only, nothing that refers to RIP):
//
//   A (12 bytes)  clzero; add rax, 40h; cmp rax, END; jb A                  (END: rdx or r8-r15)
//   B (15 bytes)  mov rax, CUR; add CUR, 40h; clzero; cmp CUR, END; jb B    (CUR, END: rdx or r8-r15)
//
// The loop's first instruction becomes a `jmp rel32` to a copy of the loop in a stub page within 2 GiB of the image,
// and the rest of the loop becomes int3 (nothing branches into a loop's middle: its jb targets its first byte). In the
// copy, CLZERO is eight 8-byte stores of zero to the line rAX points into, with RCX saved around them; the loop's own
// instructions are copied unchanged, so the registers and flags the loop leaves are the ones the original leaves. The
// copy ends with a jmp to the instruction after the original jb. A line the title cannot write faults in the stub, as
// the original would fault at its CLZERO; the stub page is logged so such an address can be placed.
//
// Run before any of the image's code and before its sections are protected. LEONIDA_CLZERO_PATCH=0 keeps the #UD path.
#pragma once

#ifdef _WIN32
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

namespace rt::clzero_patch {

struct Site {
    uint32_t head;    // offset of the loop's first byte from the scanned code's start
    uint32_t length;  // 12 (shape A) or 15 (shape B)
    uint32_t clzero;  // offset of the CLZERO within the loop
};

// `cmp r/m64, r64` (REX.W 39 /r, register form) or `mov`/`add` with the given register as ModRM r/m, for rdx or
// r8-r15: the registers the routine uses. rcx is excluded because the stub saves it around its stores.
inline bool is_cmp_rr(const uint8_t* p) {
    return (p[0] == 0x48 || p[0] == 0x49 || p[0] == 0x4C || p[0] == 0x4D) && p[1] == 0x39 && (p[2] & 0xC0) == 0xC0;
}

inline bool clzero_at(const uint8_t* p) { return p[0] == 0x0F && p[1] == 0x01 && p[2] == 0xFC; }

// Register number (0-15) of ModRM.rm / ModRM.reg with REX.
inline unsigned rm_reg(uint8_t rex, uint8_t modrm) { return (modrm & 7) | ((rex & 1) << 3); }
inline unsigned reg_reg(uint8_t rex, uint8_t modrm) { return ((modrm >> 3) & 7) | ((rex & 4) << 1); }

// Finds the loops in `code[0, size)`. A CLZERO that is in neither shape is left to the #UD path.
inline std::vector<Site> find(const uint8_t* code, size_t size) {
    std::vector<Site> sites;
    if (!code || size < 12) return sites;
    for (size_t k = 0; k + 3 <= size; ++k) {
        if (!clzero_at(code + k)) continue;
        // Shape A: the CLZERO is the loop's first instruction.
        if (k + 12 <= size) {
            const uint8_t* p = code + k;
            const bool add = p[3] == 0x48 && p[4] == 0x83 && p[5] == 0xC0 && p[6] == 0x40;  // add rax, 40h
            // cmp rax, END: REX.W (+R for r8-r15) 39 /r with rm = rax.
            const bool cmp = is_cmp_rr(p + 7) && rm_reg(p[7], p[9]) == 0 && (p[7] & 1) == 0;
            const unsigned end = cmp ? reg_reg(p[7], p[9]) : 0;
            const bool jb = p[10] == 0x72 && int8_t(p[11]) == -12;
            if (add && cmp && jb && end != 0 && end != 1 && end != 4) {
                sites.push_back({uint32_t(k), 12, 0});
                k += 2;
                continue;
            }
        }
        // Shape B: mov rax, CUR; add CUR, 40h before the CLZERO; cmp CUR, END; jb after it.
        if (k >= 7 && k + 8 <= size) {
            const uint8_t* p = code + k - 7;
            // mov rax, CUR: REX.W(+R) 89 /r, rm = rax, reg = CUR.
            const bool mov = (p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x89 && (p[2] & 0xC7) == 0xC0;
            const unsigned cur = mov ? reg_reg(p[0], p[2]) : 0;
            // add CUR, 40h: REX.W(+B) 83 /0 ib with rm = CUR.
            const bool add = (p[3] == 0x48 || p[3] == 0x49) && p[4] == 0x83 && (p[5] & 0xF8) == 0xC0 &&
                             rm_reg(p[3], p[5]) == cur && p[6] == 0x40;
            // cmp CUR, END: REX.W 39 /r with rm = CUR, reg = END.
            const bool cmp = is_cmp_rr(p + 10) && rm_reg(p[10], p[12]) == cur;
            const unsigned end = cmp ? reg_reg(p[10], p[12]) : 0;
            const bool jb = p[13] == 0x72 && int8_t(p[14]) == -15;
            const auto ok = [](unsigned r) { return r == 2 || r >= 8; };
            if (mov && add && cmp && jb && ok(cur) && ok(end) && cur != end) {
                sites.push_back({uint32_t(k - 7), 15, 7});
                k += 7;
                continue;
            }
        }
    }
    return sites;
}

// Overflow-free check that a rel32 branch from `from_next` to `to` fits in signed 32 bits.
inline bool rel32_offset(uint64_t from_next, uint64_t to, int32_t& out) {
    constexpr uint64_t kMaxPos = static_cast<uint64_t>(INT32_MAX);
    constexpr uint64_t kMaxNeg = static_cast<uint64_t>(INT32_MAX) + 1;
    if (to >= from_next) {
        const uint64_t d = to - from_next;
        if (d > kMaxPos) return false;
        out = static_cast<int32_t>(d);
        return true;
    }
    const uint64_t d = from_next - to;
    if (d > kMaxNeg) return false;
    out = d == kMaxNeg ? INT32_MIN : -static_cast<int32_t>(d);
    return true;
}

// The stub for one loop: its instructions before the CLZERO, the zeroing block, its instructions after the CLZERO up
// to the jb, a jb back to the stub's start, then a jmp to `resume` (the instruction after the original jb).
// Returns an empty vector if the final jmp rel32 cannot reach `resume`.
inline std::vector<uint8_t> stub(const uint8_t* loop, const Site& s, uint64_t stub_at, uint64_t resume) {
    std::vector<uint8_t> out(loop, loop + s.clzero);  // shape B: mov rax, CUR; add CUR, 40h
    const uint8_t zero_line[] = {
        0x51,                                            // push rcx
        0x48, 0x89, 0xC1,                                // mov rcx, rax
        0x48, 0x83, 0xE1, 0xC0,                          // and rcx, -64
        0x48, 0xC7, 0x01, 0, 0, 0, 0,                    // mov qword [rcx], 0
        0x48, 0xC7, 0x41, 0x08, 0, 0, 0, 0,              // mov qword [rcx+8], 0
        0x48, 0xC7, 0x41, 0x10, 0, 0, 0, 0,
        0x48, 0xC7, 0x41, 0x18, 0, 0, 0, 0,
        0x48, 0xC7, 0x41, 0x20, 0, 0, 0, 0,
        0x48, 0xC7, 0x41, 0x28, 0, 0, 0, 0,
        0x48, 0xC7, 0x41, 0x30, 0, 0, 0, 0,
        0x48, 0xC7, 0x41, 0x38, 0, 0, 0, 0,
        0x59,                                            // pop rcx
    };
    out.insert(out.end(), std::begin(zero_line), std::end(zero_line));
    // The instructions between the CLZERO and the jb (shape A: add rax, 40h; cmp; shape B: cmp).
    out.insert(out.end(), loop + s.clzero + 3, loop + s.length - 2);
    const int back = -int(out.size() + 2);
    out.push_back(0x72);
    out.push_back(uint8_t(int8_t(back)));
    out.push_back(0xE9);
    int32_t rel32 = 0;
    if (out.size() + 4 > UINT64_MAX - stub_at || !rel32_offset(stub_at + out.size() + 4, resume, rel32)) return {};
    const auto* b = reinterpret_cast<const uint8_t*>(&rel32);
    out.insert(out.end(), b, b + 4);
    return out;
}

// Reserves a page within 2 GiB above or below [lo, hi) for the stubs (the image does not move after this).
inline uint8_t* stub_page(uint64_t lo, uint64_t hi, size_t size) {
    if (!size || hi < lo) return nullptr;
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const uint64_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
    const uint64_t limit = uint64_t(1) << 31;
    const uint64_t max_span = limit - 0x1000;
    if (hi - lo < max_span && hi <= UINT64_MAX - (gran - 1)) {
        const uint64_t start = (hi + gran - 1) & ~(gran - 1);
        const uint64_t ceiling = lo <= UINT64_MAX - max_span ? lo + max_span : UINT64_MAX;
        for (uint64_t at = start; size <= ceiling && at <= ceiling - size;) {
            if (void* p = VirtualAlloc(reinterpret_cast<void*>(at), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
                return static_cast<uint8_t*>(p);
            if (at > UINT64_MAX - gran) break;
            at += gran;
        }
    }
    if (lo >= size) {
        uint64_t at = (lo - size) & ~(gran - 1);
        while (at >= gran && hi - at < max_span) {
            if (void* p = VirtualAlloc(reinterpret_cast<void*>(at), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
                return static_cast<uint8_t*>(p);
            if (at < gran) break;
            at -= gran;
        }
    }
    return nullptr;
}

inline bool enabled() {
    char v[8] = {};
    size_t n = 0;
    return !(getenv_s(&n, v, "LEONIDA_CLZERO_PATCH") == 0 && n == 2 && v[0] == '0');
}

struct Result {
    size_t sites = 0;
    uint8_t* stubs = nullptr;
    size_t stub_bytes = 0;
};

// Patches every loop found in the executable ranges `ranges` (pairs of start address and size, all inside
// [image_lo, image_hi)). The ranges must be writable. On any failure nothing is patched.
inline Result patch(const std::vector<std::pair<uint8_t*, size_t>>& ranges, uint64_t image_lo, uint64_t image_hi) {
    Result r;
    std::vector<std::pair<uint8_t*, Site>> all;
    for (const auto& [code, size] : ranges)
        for (const Site& s : find(code, size)) all.push_back({code, s});
    if (all.empty()) return r;
    const size_t page = 0x10000;
    uint8_t* const stubs = stub_page(image_lo, image_hi, page);
    if (!stubs) return r;
    size_t used = 0;
    std::vector<std::vector<uint8_t>> bodies;
    std::vector<int32_t> jump_rels;
    bodies.reserve(all.size());
    jump_rels.reserve(all.size());
    for (const auto& [code, s] : all) {
        uint8_t* const loop = code + s.head;
        const uint64_t at = reinterpret_cast<uint64_t>(stubs + used);
        int32_t jmp_rel = 0;
        if (!rel32_offset(reinterpret_cast<uint64_t>(loop + 5), at, jmp_rel)) {
            VirtualFree(stubs, 0, MEM_RELEASE);
            return r;
        }
        auto body = stub(loop, s, at, reinterpret_cast<uint64_t>(loop + s.length));
        if (body.empty()) {
            VirtualFree(stubs, 0, MEM_RELEASE);
            return r;
        }
        const size_t padded = (body.size() + 15) & ~size_t(15);
        if (padded > page - used) {
            VirtualFree(stubs, 0, MEM_RELEASE);
            return r;
        }
        bodies.push_back(std::move(body));
        jump_rels.push_back(jmp_rel);
        used += padded;
    }
    size_t offset = 0;
    for (const auto& body : bodies) {
        std::memcpy(stubs + offset, body.data(), body.size());
        offset += (body.size() + 15) & ~size_t(15);
    }
    DWORD old = 0;
    if (!VirtualProtect(stubs, page, PAGE_EXECUTE_READ, &old)) {
        VirtualFree(stubs, 0, MEM_RELEASE);
        return r;
    }
    FlushInstructionCache(GetCurrentProcess(), stubs, page);
    // Only overwrite the image loops once the stub page is populated and executable.
    for (size_t i = 0; i < all.size(); ++i) {
        const auto& [code, s] = all[i];
        uint8_t* const loop = code + s.head;
        loop[0] = 0xE9;
        std::memcpy(loop + 1, &jump_rels[i], 4);
        std::memset(loop + 5, 0xCC, s.length - 5);
    }
    r.sites = all.size();
    r.stubs = stubs;
    r.stub_bytes = used;
    return r;
}

}  // namespace rt::clzero_patch
#endif
