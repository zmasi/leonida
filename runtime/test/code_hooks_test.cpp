// code_hooks.h: a remapping thunk and direct-call redirection, on small x64 functions written into memory. No game
// files: the archetype lookup they stand in for takes its model hash in ecx (docs/modding.md).
#include "../src/code_hooks.h"
#include "../src/mods.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {

int g_checks = 0, g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s\n", what);
    }
}

uint32_t remap_lucia(uint32_t hash) {
    return hash == leonida::mods::joaat("player_story000_00") ? leonida::mods::joaat("cs_mandy") : hash;
}

void put(uint8_t* at, std::initializer_list<uint8_t> bytes) {
    for (uint8_t b : bytes) *at++ = b;
}

// `call rel32` (or `jmp` with e9) at `at` towards `to`.
void branch(uint8_t* at, uint8_t opcode, const uint8_t* to) {
    at[0] = opcode;
    const int32_t rel = static_cast<int32_t>(to - (at + 5));
    std::memcpy(at + 1, &rel, 4);
}

// sub rsp,0x28 / call to / add rsp,0x28 / ret
void caller(uint8_t* at, const uint8_t* to) {
    put(at, {0x48, 0x83, 0xEC, 0x28});
    branch(at + 4, 0xE8, to);
    put(at + 9, {0x48, 0x83, 0xC4, 0x28, 0xC3});
}

void init_pe64(uint8_t* img, uint32_t size_of_image, const std::vector<IMAGE_SECTION_HEADER>& sections) {
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(img);
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x80;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(img + dos->e_lfanew);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt->FileHeader.NumberOfSections = static_cast<WORD>(sections.size());
    nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt->OptionalHeader.SizeOfImage = size_of_image;
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (std::size_t i = 0; i < sections.size(); ++i) sec[i] = sections[i];
}

// Real functions for the unwinding checks: an exception leaves the hooked function, or the callback.
uint32_t throw_on_seven(uint32_t value) {
    if (value == 7) throw std::runtime_error("seven");
    return value + 1;
}

uint32_t throw_on_seven_result(uint32_t result, uint32_t argument) {
    if (argument == 8) throw std::runtime_error("eight");
    return result * 100 + argument;
}

// Leaves no return address of an earlier call where an unwinder that reads the wrong slot could find one.
void dirty_stack() {
    volatile unsigned char bytes[8192];
    for (auto& b : bytes) b = 0xAB;
}

bool caught_through(const uint8_t* thunk, uint32_t argument) {
    dirty_stack();
    try {
        reinterpret_cast<uint32_t (*)(uint32_t)>(const_cast<uint8_t*>(thunk))(argument);
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

// Unwinds one frame from inside `thunk`, where its call returns to (`return_offset`, after a prolog that moved rsp by
// `frame` bytes), and says whether the caller's rip and rsp come out.
bool unwinds_to_caller(uint8_t* thunk, std::size_t return_offset, std::size_t frame) {
    alignas(16) uint64_t stack[64] = {};
    const uint64_t caller_rip = 0x1234567890ull;
    uint64_t* at_entry = stack + 40;  // rsp when the thunk is entered: the return address
    *at_entry = caller_rip;
    CONTEXT context{};
    context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    context.Rip = reinterpret_cast<DWORD64>(thunk + return_offset);
    context.Rsp = reinterpret_cast<DWORD64>(at_entry) - frame;
    DWORD64 base = 0, establisher = 0;
    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &base, nullptr);
    if (!function || base != reinterpret_cast<DWORD64>(thunk)) return false;
    void* handler_data = nullptr;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, context.Rip, function, &context, &handler_data, &establisher, nullptr);
    return context.Rip == caller_rip && context.Rsp == reinterpret_cast<DWORD64>(at_entry + 1);
}

// The offset just after the first `call rax` (FF D0) of a thunk.
std::size_t after_first_call(const uint8_t* thunk) {
    for (std::size_t i = 0; i + 1 < 256; ++i)
        if (thunk[i] == 0xFF && thunk[i + 1] == 0xD0) return i + 2;
    return 0;
}

}  // namespace

int main() {
    using namespace leonida::hooks;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    uint8_t* mem = alloc_near(reinterpret_cast<const void*>(&remap_lucia), 4096);
    check(mem != nullptr, "executable memory near the module");
    if (!mem) return 1;
    std::memset(mem, 0xCC, 4096);

    uint8_t* target0 = mem;          // returns its first argument:  mov eax, ecx ; ret
    put(target0, {0x89, 0xC8, 0xC3});
    uint8_t* target1 = mem + 16;     // returns its second argument: mov eax, edx ; ret
    put(target1, {0x89, 0xD0, 0xC3});
    uint8_t* target2 = mem + 32;     // returns its third argument:  mov eax, r8d ; ret
    put(target2, {0x44, 0x89, 0xC0, 0xC3});

    uint8_t* call0 = mem + 64;
    caller(call0, target0);
    uint8_t* tail0 = mem + 96;       // jmp target0 (a tail call)
    branch(tail0, 0xE9, target0);
    uint8_t* decoy = mem + 112;      // a call to target0 + 1: not the hooked function, must stay as it is
    caller(decoy, target0 + 1);
    uint8_t* call1 = mem + 144;
    caller(call1, target1);
    uint8_t* call2 = mem + 176;
    caller(call2, target2);
    uint8_t decoy_before[16];
    std::memcpy(decoy_before, decoy, 16);

    uint8_t* thunk0 = make_remap_thunk(target0, 0, &remap_lucia, target0);
    uint8_t* thunk1 = make_remap_thunk(target1, 1, &remap_lucia, target1);
    uint8_t* thunk2 = make_remap_thunk(target2, 0, &remap_lucia, target2);
    check(thunk0 && thunk1 && thunk2, "thunks are allocated near their targets");
    if (!thunk0 || !thunk1 || !thunk2) return 1;

    const std::size_t n0 = redirect_direct_branches(mem + 64, 112, target0, thunk0);
    const std::size_t n1 = redirect_direct_branches(call1, 16, target1, thunk1);
    const std::size_t n2 = redirect_direct_branches(call2, 16, target2, thunk2);
    check(n0 == 2, "the call and the tail jump to the function are redirected, the decoy is not");
    check(n1 == 1 && n2 == 1, "one call each in the other callers");
    check(std::memcmp(decoy, decoy_before, 16) == 0, "a branch to another address is left byte for byte");
    FlushInstructionCache(GetCurrentProcess(), mem, 4096);

    using Fn1 = uint32_t (*)(uint32_t);
    using Fn2 = uint32_t (*)(uint64_t, uint32_t);
    using Fn3 = uint32_t (*)(uint32_t, uint32_t, uint32_t);
    const uint32_t lucia = 0xE472B687u, mandy = 0xD7C0CDFFu;
    check(reinterpret_cast<Fn1>(call0)(lucia) == mandy, "through the call, Lucia's model id becomes cs_mandy's");
    check(reinterpret_cast<Fn1>(call0)(1234) == 1234, "any other id passes through unchanged");
    check(reinterpret_cast<Fn1>(tail0)(lucia) == mandy, "a tail jump is remapped as well");
    check(reinterpret_cast<Fn2>(call1)(0x1111, lucia) == mandy, "argument 1 (edx) is remapped when asked");
    check(reinterpret_cast<Fn3>(call2)(lucia, 2, 777) == 777, "r8 reaches the original unchanged");

    // hook_image_calls refuses what is not a mapped image or not code
    check(!hook_image_calls(mem, 0, &remap_lucia, 0).ok, "memory without a PE header is refused");
    auto* self = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    check(!hook_image_calls(self, 0x7FFFFFF0u, &remap_lucia, 0).ok, "an RVA outside the image is refused");
    check(!hook_image_calls(self, 0, &remap_lucia, 0).ok, "an RVA in the headers (not code) is refused");
    check(!make_remap_thunk(target0, 2, &remap_lucia, target0), "only argument 0 or 1 can be remapped");

    // The result-override thunk (a view-mode getter): the original returns 2 ("third person, far") for every
    // context; the override answers 4 ("first person") for context 0 only and leaves the others as they were.
    uint8_t* getter = mem + 512;
    put(getter, {0xB8, 0x02, 0x00, 0x00, 0x00, 0xC3});   // mov eax, 2 ; ret
    uint8_t* call_getter = mem + 544;
    caller(call_getter, getter);
    static int original_calls = 0;
    auto first_person_on_foot = [](uint32_t result, uint32_t context) -> uint32_t {
        ++original_calls;
        return context == 0 ? 4u : result;
    };
    uint8_t* rthunk = make_result_thunk(getter, first_person_on_foot, getter);
    check(rthunk != nullptr, "a result thunk is allocated");
    if (rthunk) {
        check(redirect_direct_branches(call_getter, 16, getter, rthunk) == 1, "the getter's caller is redirected");
        FlushInstructionCache(GetCurrentProcess(), mem, 4096);
        using Ctx = uint32_t (*)(uint32_t);
        check(reinterpret_cast<Ctx>(call_getter)(0) == 4, "context 0 gets the overridden result");
        check(reinterpret_cast<Ctx>(call_getter)(3) == 2, "another context keeps the original's result");
        check(original_calls == 2, "the override sees every call, after the original ran");
    }
    check(!hook_image_results(mem, 0, first_person_on_foot).ok, "hook_image_results refuses memory without a PE");

    // Test A: PE64 executable section with Misc.VirtualSize == 0 falls back to SizeOfRawData (matching pe_loader.cpp).
    uint8_t* img_a = alloc_near(reinterpret_cast<const void*>(&remap_lucia), 0x2000);
    check(img_a != nullptr, "synthetic PE64 image A allocated near module");
    if (img_a) {
        std::memset(img_a, 0, 0x2000);
        IMAGE_SECTION_HEADER sec_a{};
        std::memcpy(sec_a.Name, ".text", 5);
        sec_a.Misc.VirtualSize = 0;
        sec_a.VirtualAddress = 0x1000;
        sec_a.SizeOfRawData = 0x200;
        sec_a.Characteristics = IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_CODE;
        init_pe64(img_a, 0x2000, {sec_a});
        put(img_a + 0x1000, {0x89, 0xC8, 0xC3});  // mov eax, ecx ; ret
        caller(img_a + 0x1020, img_a + 0x1000);
        DWORD old_a = 0;
        check(VirtualProtect(img_a + 0x1000, 0x1000, PAGE_EXECUTE_READ, &old_a) != FALSE,
              "image A code page marked PAGE_EXECUTE_READ");
        FlushInstructionCache(GetCurrentProcess(), img_a + 0x1000, 0x1000);

        const HookResult ra = hook_image_calls(img_a, 0x1000, &remap_lucia, 0);
        check(ra.ok && ra.redirected == 1,
              "VirtualSize == 0 executable section uses SizeOfRawData and redirects its caller");
        if (ra.ok) {
            check(reinterpret_cast<Fn1>(img_a + 0x1020)(lucia) == mandy,
                  "hooked caller in VirtualSize == 0 section remaps Lucia to Mandy");
            check(reinterpret_cast<Fn1>(img_a + 0x1020)(1234) == 1234,
                  "hooked caller in VirtualSize == 0 section passes other hashes through");
        }
    }

    // Test B: a 0-byte executable section (VirtualSize == 0 && SizeOfRawData == 0) is skipped rather than failing VirtualProtect.
    uint8_t* img_b = alloc_near(reinterpret_cast<const void*>(&remap_lucia), 0x2000);
    check(img_b != nullptr, "synthetic PE64 image B allocated near module");
    if (img_b) {
        std::memset(img_b, 0, 0x2000);
        IMAGE_SECTION_HEADER sec_empty{};
        std::memcpy(sec_empty.Name, ".empty", 6);
        sec_empty.Misc.VirtualSize = 0;
        sec_empty.VirtualAddress = 0x1000;
        sec_empty.SizeOfRawData = 0;
        sec_empty.Characteristics = IMAGE_SCN_MEM_EXECUTE;
        IMAGE_SECTION_HEADER sec_text{};
        std::memcpy(sec_text.Name, ".text", 5);
        sec_text.Misc.VirtualSize = 0x200;
        sec_text.VirtualAddress = 0x1000;
        sec_text.SizeOfRawData = 0x200;
        sec_text.Characteristics = IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_CODE;
        init_pe64(img_b, 0x2000, {sec_empty, sec_text});
        put(img_b + 0x1040, {0xB8, 0x02, 0x00, 0x00, 0x00, 0xC3});  // mov eax, 2 ; ret
        caller(img_b + 0x1060, img_b + 0x1040);
        DWORD old_b = 0;
        check(VirtualProtect(img_b + 0x1000, 0x1000, PAGE_EXECUTE_READ, &old_b) != FALSE,
              "image B code page marked PAGE_EXECUTE_READ");
        FlushInstructionCache(GetCurrentProcess(), img_b + 0x1000, 0x1000);

        const int before_calls = original_calls;
        const HookResult rb = hook_image_results(img_b, 0x1040, first_person_on_foot);
        check(rb.ok && rb.redirected == 1,
              "0-byte executable section is skipped and the non-empty section is hooked");
        if (rb.ok) {
            using Ctx = uint32_t (*)(uint32_t);
            check(reinterpret_cast<Ctx>(img_b + 0x1060)(0) == 4,
                  "result hook in PE64 image overrides context 0");
            check(reinterpret_cast<Ctx>(img_b + 0x1060)(3) == 2,
                  "result hook in PE64 image preserves non-overridden contexts");
            check(original_calls == before_calls + 2,
                  "result hook invokes the original getter on each call");
        }
    }

    // Test C: sections extending past SizeOfImage are clamped and 32-bit VirtualAddress + VirtualSize wraparound is safe.
    uint8_t* img_c = alloc_near(reinterpret_cast<const void*>(&remap_lucia), 0x2000);
    check(img_c != nullptr, "synthetic PE64 image C allocated near module");
    if (img_c) {
        std::memset(img_c, 0, 0x2000);
        IMAGE_SECTION_HEADER sec_wrap{};
        std::memcpy(sec_wrap.Name, ".wrap", 5);
        sec_wrap.Misc.VirtualSize = 0x200u;
        sec_wrap.VirtualAddress = 0xFFFFFF00u;
        sec_wrap.Characteristics = IMAGE_SCN_MEM_EXECUTE;
        IMAGE_SECTION_HEADER sec_over{};
        std::memcpy(sec_over.Name, ".text", 5);
        sec_over.Misc.VirtualSize = 0x1800u;  // spans 0x0800..0x2000, past SizeOfImage = 0x1000
        sec_over.VirtualAddress = 0x0800u;
        sec_over.Characteristics = IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_CODE;
        init_pe64(img_c, 0x1000, {sec_wrap, sec_over});
        put(img_c + 0x0800, {0x89, 0xC8, 0xC3});  // mov eax, ecx ; ret
        caller(img_c + 0x0820, img_c + 0x0800);   // in-image caller
        caller(img_c + 0x1020, img_c + 0x0800);   // out-of-image bytes past SizeOfImage
        uint8_t out_before[16];
        std::memcpy(out_before, img_c + 0x1020, sizeof(out_before));
        FlushInstructionCache(GetCurrentProcess(), img_c, 0x2000);

        const HookResult rc = hook_image_calls(img_c, 0x0800, &remap_lucia, 0);
        check(rc.ok && rc.redirected == 1,
              "section past SizeOfImage is clamped and wraparound section is ignored");
        if (rc.ok) {
            check(reinterpret_cast<Fn1>(img_c + 0x0820)(lucia) == mandy,
                  "in-image caller inside clamped section is remapped");
            check(std::memcmp(img_c + 0x1020, out_before, sizeof(out_before)) == 0,
                  "bytes beyond SizeOfImage are left untouched");
        }
    }

    // Test D: malformed headers, PE32 optional header, out-of-image section table, and null arguments/callbacks.
    uint8_t* bad = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(bad != nullptr, "scratch buffer for malformed PE checks allocated");
    if (bad) {
        std::memset(bad, 0, 0x1000);
        auto* bad_dos = reinterpret_cast<IMAGE_DOS_HEADER*>(bad);
        bad_dos->e_magic = IMAGE_DOS_SIGNATURE;
        bad_dos->e_lfanew = 0;
        check(!hook_image_calls(bad, 0x200, &remap_lucia, 0).ok, "MZ with e_lfanew == 0 is refused");
        bad_dos->e_lfanew = -16;
        check(!hook_image_calls(bad, 0x200, &remap_lucia, 0).ok, "MZ with negative e_lfanew is refused");

        std::memset(bad, 0, 0x1000);
        init_pe64(bad, 0x1000, {});
        auto* bad_nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(bad + 0x80);
        bad_nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
        check(!hook_image_calls(bad, 0x200, &remap_lucia, 0).ok, "PE32 optional header magic is refused");

        std::memset(bad, 0, 0x1000);
        init_pe64(bad, 0x200, {});
        bad_nt->FileHeader.NumberOfSections = 100;
        check(!hook_image_calls(bad, 0x100, &remap_lucia, 0).ok,
              "section table extending past SizeOfImage is refused");
        VirtualFree(bad, 0, MEM_RELEASE);
    }

    if (img_a) {
        check(!hook_image_calls(img_a, 0x1000, nullptr, 0).ok, "hook_image_calls refuses null callback");
        VirtualFree(img_a, 0, MEM_RELEASE);
    }
    if (img_b) {
        check(!hook_image_results(img_b, 0x1040, nullptr).ok, "hook_image_results refuses null callback");
        VirtualFree(img_b, 0, MEM_RELEASE);
    }
    if (img_c) VirtualFree(img_c, 0, MEM_RELEASE);

    check(!hook_image_calls(nullptr, 0x1000, &remap_lucia, 0).ok, "hook_image_calls refuses null base");
    check(!hook_image_results(nullptr, 0x1000, first_person_on_foot).ok, "hook_image_results refuses null base");
    check(make_remap_thunk(target0, 0, nullptr, target0) == nullptr, "make_remap_thunk refuses null callback");
    check(make_remap_thunk(nullptr, 0, &remap_lucia, target0) == nullptr, "make_remap_thunk refuses null close_to");
    check(make_result_thunk(getter, nullptr, getter) == nullptr, "make_result_thunk refuses null callback");
    check(make_result_thunk(getter, first_person_on_foot, nullptr) == nullptr,
          "make_result_thunk refuses null original");
    check(redirect_direct_branches(nullptr, 16, target0, thunk0) == 0,
          "redirect_direct_branches returns 0 for null begin");
    VirtualFree(mem, 0, MEM_RELEASE);

    // Stack walks and exceptions pass through both thunks.
    const auto* throwing = reinterpret_cast<const void*>(&throw_on_seven);
    uint8_t* unwind_result = make_result_thunk(throwing, &throw_on_seven_result, throwing);
    uint8_t* unwind_remap = make_remap_thunk(throwing, 0, &throw_on_seven, throwing);
    check(unwind_result && unwind_remap, "thunks for the unwinding checks");
    if (unwind_result && unwind_remap) {
        check(reinterpret_cast<Fn1>(unwind_result)(1) == 201, "the result thunk still works");
        check(reinterpret_cast<Fn1>(unwind_remap)(1) == 3, "the remap thunk still works");
        check(unwinds_to_caller(unwind_result, after_first_call(unwind_result), 0x28),
              "a stack walk from the hooked function reaches the result thunk's caller");
        check(unwinds_to_caller(unwind_remap, after_first_call(unwind_remap), 0x88),
              "a stack walk from the callback reaches the remap thunk's caller");
        check(caught_through(unwind_result, 7), "an exception leaves the hooked function through the result thunk");
        check(caught_through(unwind_result, 8), "an exception leaves the callback through the result thunk");
        check(caught_through(unwind_remap, 7), "an exception leaves the callback through the remap thunk");
        const DWORD64 freed = reinterpret_cast<DWORD64>(unwind_result) + 1;
        DWORD64 base = 0;
        free_thunk(unwind_result);
        check(RtlLookupFunctionEntry(freed, &base, nullptr) == nullptr, "a freed thunk leaves no function table");
        free_thunk(unwind_remap);
    }
    free_thunk(nullptr);
    check(finish_thunk(nullptr, 16, {0x01, 0, 0, 0}) == nullptr, "finish_thunk refuses null thunk buffer");

    // Zero direct callers: redirect_image_calls succeeds and frees the unused thunk and its function table.
    uint8_t* img_zero = alloc_near(reinterpret_cast<const void*>(&remap_lucia), 0x2000);
    check(img_zero != nullptr, "synthetic PE64 image for zero-caller check allocated");
    if (img_zero) {
        std::memset(img_zero, 0, 0x2000);
        IMAGE_SECTION_HEADER sec_z{};
        std::memcpy(sec_z.Name, ".text", 5);
        sec_z.Misc.VirtualSize = 0x100;
        sec_z.VirtualAddress = 0x1000;
        sec_z.SizeOfRawData = 0x100;
        sec_z.Characteristics = IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_CODE;
        init_pe64(img_zero, 0x2000, {sec_z});
        put(img_zero + 0x1000, {0x89, 0xC8, 0xC3});  // target with no direct E8/E9 callers
        uint8_t* unused_thunk = nullptr;
        const HookResult rz = redirect_image_calls(img_zero, 0x1000, [&](uint8_t* target) {
            unused_thunk = make_remap_thunk(target, 0, &remap_lucia, target);
            return unused_thunk;
        });
        DWORD64 base = 0;
        check(rz.ok && rz.redirected == 0 && unused_thunk != nullptr,
              "redirect_image_calls succeeds with zero redirected branches");
        check(unused_thunk && RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(unused_thunk) + 1, &base, nullptr) == nullptr,
              "unused thunk and its function table are freed when zero branches were redirected");
        VirtualFree(img_zero, 0, MEM_RELEASE);
    }

    std::printf("code_hooks_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
