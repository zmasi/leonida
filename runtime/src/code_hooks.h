// Hooks on a function of the loaded game image, by redirecting its direct callers (docs/modding.md, "Day one").
//
// Patching inside a function needs an instruction decoder to move its prologue. Redirecting the callers does not:
// every `call rel32` (E8) and `jmp rel32` (E9) in the image's executable sections whose target is the function is
// pointed at a thunk instead. The thunk saves the argument registers (rcx, rdx, r8, r9, xmm0-3), passes one 32-bit
// argument through a C++ function, restores the rest and jumps to the original. A call through a pointer (vtable,
// import slot) is not redirected, which suits RAGE's archetype lookup: a static function called directly.
//
// A byte pattern can look like E8/E9 + rel32 inside another instruction; it is only rewritten when its target is
// exactly the hooked function, so a false match must also point there. x64 Windows only.
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace leonida::hooks {

using RemapFn = uint32_t (*)(uint32_t);

// Executable memory within +-2 GiB of `close_to`, so a rel32 branch from the image reaches it. nullptr if none is free.
inline uint8_t* alloc_near(const void* close_to, std::size_t size) {
    if (!close_to || size == 0) return nullptr;
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity ? static_cast<uintptr_t>(si.dwAllocationGranularity)
                                                      : uintptr_t{0x10000};
    const uintptr_t addr = reinterpret_cast<uintptr_t>(close_to);
    const uintptr_t center = addr - (addr % gran);
    const uintptr_t span = 0x70000000;  // stay well inside the rel32 range from anywhere in a large image
    for (uintptr_t delta = gran; delta < span;) {
        for (int side = 0; side < 2; ++side) {
            if (side == 0 && center < delta) continue;
            if (side == 1 && UINTPTR_MAX - center < delta) continue;
            const uintptr_t at = side ? center + delta : center - delta;
            if (void* p = VirtualAlloc(reinterpret_cast<void*>(at), size, MEM_RESERVE | MEM_COMMIT,
                                       PAGE_EXECUTE_READWRITE))
                return static_cast<uint8_t*>(p);
        }
        if (span - delta <= gran) break;
        delta += gran;
    }
    return nullptr;
}

// A thunk's page: its code, then the unwind data Windows needs to walk the stack through it. Without that data the
// unwinder takes the thunk for a leaf function and reads the return address from the wrong place, so an exception
// raised below it (in the hooked function or the callback) finds no handler and ends the process, and a stack trace
// stops there.
constexpr DWORD kThunkUnwindOffset = 0x200;
constexpr DWORD kThunkFunctionOffset = 0x240;

// Seals a thunk of `code_size` bytes: writes its UNWIND_INFO (`unwind`: version, prolog size, code count and codes) and
// RUNTIME_FUNCTION, makes the page executable and registers the function table. Frees the page and returns null on
// failure.
inline uint8_t* finish_thunk(uint8_t* t, std::size_t code_size, std::initializer_list<uint8_t> unwind) {
    if (!t) return nullptr;
    if (code_size == 0 || code_size > kThunkUnwindOffset ||
        unwind.size() > kThunkFunctionOffset - kThunkUnwindOffset) {
        VirtualFree(t, 0, MEM_RELEASE);
        return nullptr;
    }
    std::size_t i = 0;
    for (uint8_t b : unwind) t[kThunkUnwindOffset + i++] = b;
    auto* function = reinterpret_cast<RUNTIME_FUNCTION*>(t + kThunkFunctionOffset);
    function->BeginAddress = 0;
    function->EndAddress = static_cast<DWORD>(code_size);
    function->UnwindData = kThunkUnwindOffset;
    DWORD old = 0;
    if (!VirtualProtect(t, 4096, PAGE_EXECUTE_READ, &old) ||
        !RtlAddFunctionTable(function, 1, reinterpret_cast<DWORD64>(t))) {
        VirtualFree(t, 0, MEM_RELEASE);
        return nullptr;
    }
    FlushInstructionCache(GetCurrentProcess(), t, code_size);
    return t;
}

// Frees a thunk that nothing points at. Its function table goes first: a table left behind would send every later
// unwind through freed memory.
inline void free_thunk(uint8_t* thunk) {
    if (!thunk) return;
    RtlDeleteFunctionTable(reinterpret_cast<RUNTIME_FUNCTION*>(thunk + kThunkFunctionOffset));
    VirtualFree(thunk, 0, MEM_RELEASE);
}

// The thunk: argument `arg` (0 = rcx, 1 = rdx; the low 32 bits) goes through `fn`, then the original runs with every
// other argument register as the caller left it. Stack: the caller's call leaves rsp = 8 mod 16; four pushes and
// 0x68 bytes make it 16-aligned for the call to `fn`, with its 32-byte shadow space and room for xmm0-3.
inline uint8_t* make_remap_thunk(const void* close_to, int arg, RemapFn fn, const void* original) {
    if (!close_to || !fn || !original || (arg != 0 && arg != 1)) return nullptr;
    uint8_t* t = alloc_near(close_to, 4096);
    if (!t) return nullptr;
    uint8_t* p = t;
    auto put = [&](std::initializer_list<uint8_t> bytes) { for (uint8_t b : bytes) *p++ = b; };
    auto put64 = [&](uint64_t v) { std::memcpy(p, &v, 8); p += 8; };
    put({0x51, 0x52, 0x41, 0x50, 0x41, 0x51});        // push rcx, rdx, r8, r9
    put({0x48, 0x83, 0xEC, 0x68});                    // sub rsp, 0x68
    put({0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20});        // movdqu [rsp+0x20], xmm0
    put({0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x30});        // movdqu [rsp+0x30], xmm1
    put({0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x40});        // movdqu [rsp+0x40], xmm2
    put({0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x50});        // movdqu [rsp+0x50], xmm3
    if (arg == 1) put({0x89, 0xD1});                   // mov ecx, edx
    put({0x48, 0xB8}); put64(reinterpret_cast<uint64_t>(fn));         // mov rax, fn
    put({0xFF, 0xD0});                                 // call rax
    put({0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x20});        // movdqu xmm0, [rsp+0x20]
    put({0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x30});        // movdqu xmm1, [rsp+0x30]
    put({0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x40});        // movdqu xmm2, [rsp+0x40]
    put({0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x50});        // movdqu xmm3, [rsp+0x50]
    put({0x48, 0x83, 0xC4, 0x68});                    // add rsp, 0x68
    put({0x41, 0x59, 0x41, 0x58, 0x5A, 0x59});        // pop r9, r8, rdx, rcx
    put({0x89, static_cast<uint8_t>(arg == 0 ? 0xC1 : 0xC2)});         // mov ecx|edx, eax (the remapped value)
    put({0x48, 0xB8}); put64(reinterpret_cast<uint64_t>(original));   // mov rax, original
    put({0xFF, 0xE0});                                 // jmp rax
    // UNWIND_INFO version 1, 10-byte prolog, five UWOP_ALLOC_SMALL codes (size / 8 - 1 in the high nibble), the last
    // prolog instruction first: sub rsp,0x68 at 10, then 8 bytes for each push (the pushed registers are volatile).
    return finish_thunk(t, p - t, {0x01, 10, 5, 0, 10, 0xC2, 6, 0x02, 4, 0x02, 2, 0x02, 1, 0x02, 0, 0});
}

using ResultFn = uint32_t (*)(uint32_t result, uint32_t arg0);

// A thunk that runs the original and then passes (its 32-bit result, its first argument) through `fn`, returning
// what `fn` returns: for a getter such as "the view mode of context N". Only for functions with at most four
// register arguments (a stack argument would be one slot off) and an integer result.
// Stack: rsp = 8 mod 16 at entry; one push and 0x20 bytes of shadow space make it 16-aligned for both calls.
inline uint8_t* make_result_thunk(const void* close_to, ResultFn fn, const void* original) {
    if (!close_to || !fn || !original) return nullptr;
    uint8_t* t = alloc_near(close_to, 4096);
    if (!t) return nullptr;
    uint8_t* p = t;
    auto put = [&](std::initializer_list<uint8_t> bytes) { for (uint8_t b : bytes) *p++ = b; };
    auto put64 = [&](uint64_t v) { std::memcpy(p, &v, 8); p += 8; };
    put({0x51});                                       // push rcx (the first argument, for fn)
    put({0x48, 0x83, 0xEC, 0x20});                     // sub rsp, 0x20
    put({0x48, 0xB8}); put64(reinterpret_cast<uint64_t>(original));   // mov rax, original
    put({0xFF, 0xD0});                                 // call rax (rcx, rdx, r8, r9 still the caller's)
    put({0x48, 0x8B, 0x54, 0x24, 0x20});               // mov rdx, [rsp+0x20] (the saved first argument)
    put({0x89, 0xC1});                                 // mov ecx, eax (the original's result)
    put({0x48, 0xB8}); put64(reinterpret_cast<uint64_t>(fn));         // mov rax, fn
    put({0xFF, 0xD0});                                 // call rax
    put({0x48, 0x83, 0xC4, 0x20});                     // add rsp, 0x20
    put({0x59, 0xC3});                                 // pop rcx ; ret
    // As above: 5-byte prolog, sub rsp,0x20 at 5, push rcx at 1.
    return finish_thunk(t, p - t, {0x01, 5, 2, 0, 5, 0x32, 1, 0x02});
}

// Points every E8/E9 rel32 in [begin, begin + size) whose target is `target` at `thunk`. The range must be writable.
// Returns how many were rewritten.
inline std::size_t redirect_direct_branches(uint8_t* begin, std::size_t size, const uint8_t* target,
                                            const uint8_t* thunk) {
    if (!begin || !target || !thunk || size < 5) return 0;
    std::size_t n = 0;
    for (std::size_t i = 0; i + 5 <= size; ++i) {
        if (begin[i] != 0xE8 && begin[i] != 0xE9) continue;
        int32_t rel;
        std::memcpy(&rel, begin + i + 1, 4);
        const uint8_t* next = begin + i + 5;
        const uintptr_t next_addr = reinterpret_cast<uintptr_t>(next);
        const uintptr_t dst_addr = next_addr + static_cast<uintptr_t>(static_cast<intptr_t>(rel));
        if (dst_addr != reinterpret_cast<uintptr_t>(target)) continue;
        const uintptr_t thunk_addr = reinterpret_cast<uintptr_t>(thunk);
        int32_t rel2 = 0;
        if (thunk_addr >= next_addr) {
            const uintptr_t diff = thunk_addr - next_addr;
            if (diff > static_cast<uintptr_t>(INT32_MAX)) continue;
            rel2 = static_cast<int32_t>(diff);
        } else {
            const uintptr_t diff = next_addr - thunk_addr;
            if (diff > static_cast<uintptr_t>(INT32_MAX) + 1u) continue;
            rel2 = static_cast<int32_t>(-static_cast<int64_t>(diff));
        }
        std::memcpy(begin + i + 1, &rel2, 4);
        ++n;
        i += 4;
    }
    return n;
}

struct HookResult {
    bool ok = false;
    std::size_t redirected = 0;
    const char* error = "";
};

// Hooks the function at `target_rva` of the mapped image at `base`: one thunk, then every direct call or jump to it
// in the image's executable sections. Runs before any image code (main.cpp), so no thread is executing the bytes.
// Validates `target_rva` as code of the mapped image at `base`, builds the thunk with `make(target)` and points the
// image's direct calls and jumps to the target at it.
template <class MakeThunk>
inline HookResult redirect_image_calls(uint8_t* base, uint32_t target_rva, MakeThunk make) {
    HookResult r;
    if (!base) return r.error = "not a mapped PE image", r;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < static_cast<LONG>(sizeof(IMAGE_DOS_HEADER)))
        return r.error = "not a mapped PE image", r;
    const uint64_t lfanew = static_cast<uint64_t>(dos->e_lfanew);
    if (lfanew > UINT32_MAX - sizeof(IMAGE_NT_HEADERS64)) return r.error = "not a mapped PE image", r;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64) ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return r.error = "not a mapped PE64 image", r;
    const uint64_t image_size = nt->OptionalHeader.SizeOfImage;
    const uint64_t sec_table_end =
        lfanew + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
        static_cast<uint64_t>(nt->FileHeader.SizeOfOptionalHeader) +
        static_cast<uint64_t>(nt->FileHeader.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER);
    if (image_size == 0 || sec_table_end > image_size)
        return r.error = "not a mapped PE64 image", r;
    if (target_rva >= image_size) return r.error = "the RVA is outside the image", r;
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    // Match pe_loader.cpp's effective section span (VirtualSize, falling back to SizeOfRawData when 0), clamped to SizeOfImage.
    auto exec_section_span = [&](const IMAGE_SECTION_HEADER& s, uint64_t& va, uint64_t& len) -> bool {
        if (!(s.Characteristics & IMAGE_SCN_MEM_EXECUTE)) return false;
        va = s.VirtualAddress;
        const uint64_t raw = s.Misc.VirtualSize ? static_cast<uint64_t>(s.Misc.VirtualSize)
                                                : static_cast<uint64_t>(s.SizeOfRawData);
        if (raw == 0 || va >= image_size) return false;
        len = raw > image_size - va ? image_size - va : raw;
        return len > 0;
    };
    bool in_code = false;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        uint64_t va = 0, len = 0;
        if (exec_section_span(sec[i], va, len) && target_rva >= va && target_rva < va + len)
            in_code = true;
    }
    if (!in_code) return r.error = "the RVA is not in an executable section", r;
    uint8_t* target = base + target_rva;
    uint8_t* thunk = make(target);
    if (!thunk) return r.error = "no memory within 2 GiB of the image for the thunk", r;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        uint64_t va = 0, len = 0;
        if (!exec_section_span(sec[i], va, len)) continue;
        uint8_t* begin = base + va;
        const std::size_t size = static_cast<std::size_t>(len);
        DWORD old = 0;
        if (!VirtualProtect(begin, size, PAGE_EXECUTE_READWRITE, &old)) {
            if (r.redirected == 0) free_thunk(thunk);
            return r.error = "VirtualProtect failed", r;
        }
        r.redirected += redirect_direct_branches(begin, size, target, thunk);
        DWORD ignored = 0;
        VirtualProtect(begin, size, old, &ignored);
        FlushInstructionCache(GetCurrentProcess(), begin, size);
    }
    if (r.redirected == 0) free_thunk(thunk);
    r.ok = true;
    return r;
}

// The argument-remapping hook (archetype lookup): argument `arg` of every direct call goes through `fn`.
inline HookResult hook_image_calls(uint8_t* base, uint32_t target_rva, RemapFn fn, int arg) {
    if (!fn) return HookResult{false, 0, "hook callback is null"};
    if (arg != 0 && arg != 1) return HookResult{false, 0, "only argument 0 or 1 can be remapped"};
    return redirect_image_calls(base, target_rva,
                                [&](uint8_t* target) { return make_remap_thunk(target, arg, fn, target); });
}

// The result-override hook (view-mode getter): every direct call's result goes through `fn` with the first argument.
inline HookResult hook_image_results(uint8_t* base, uint32_t target_rva, ResultFn fn) {
    if (!fn) return HookResult{false, 0, "hook callback is null"};
    return redirect_image_calls(base, target_rva,
                                [&](uint8_t* target) { return make_result_thunk(target, fn, target); });
}

}  // namespace leonida::hooks
