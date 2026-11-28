// End-to-end checks for module TLS index 0 and a mapped executable, against locally built fixtures.
//
// Windows gives the process executable module TLS index 0, and MSVC code built with /GA relies on it: it reads
// TEB->ThreadLocalStoragePointer[0] without loading _tls_index. tiny_tls_ga.exe is such an image (a thread_local int
// with the initializer 42, built /GA). rt::reserve_tls_index0 moves this test EXE's own TLS (index 0 at start) to the
// home clone's index, whose TLS template is a copy of this EXE's; the Loader then gives the executable index 0.
//
//   loader_tls_index0_test runs <tiny_tls_ga.exe> <tiny_tls.dll> <tiny_thread_hook.dll>
//       Reserve at start, then: this EXE keeps its TLS on the reserving thread and every later thread finds a fresh
//       copy at the home index; the executable reads 42 on the loading thread, a thread older than load() and new
//       threads; its getter is /GA code; tiny_tls.dll keeps a helper index alongside; two runs in a row both get
//       index 0, and a second executable mapped meanwhile gets a helper index.
//   loader_tls_index0_test threads <same>
//       Threads are created all through the reservation, one waits suspended with no TLS array yet, and
//       tiny_thread_hook.dll (loaded before the helper) runs this EXE's code in each new thread's DLL_THREAD_ATTACH,
//       before the helper's: all of them find this EXE's TLS at the home index, the hook included.
//   loader_tls_index0_test noreserve <same>
//       Without the reservation the executable maps with a helper index and this EXE keeps index 0.
//
// gta6rt_tls_host.dll must sit next to this EXE, as for loader_tls_test. No game files are needed. Every function
// that runs before the reservation, or calls it, touches no thread_local data: MSVC may keep a TLS slot address in a
// frame across calls, and the reservation has to come before any such frame exists (pe_loader.h).
#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>

#include "pe_loader.h"

extern "C" unsigned long _tls_index;  // this EXE's module TLS index, read through its TLS directory's AddressOfIndex

namespace {

namespace fs = std::filesystem;

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

// This EXE's own TLS: thread_local reads here go through _tls_index, so they follow the move.
thread_local int t_host = 1234;
thread_local bool t_hooked = false;
thread_local int t_seen_in_hook = -1;

__declspec(noinline) int host_value() { return t_host; }
__declspec(noinline) void set_host_value(int value) { t_host = value; }
__declspec(noinline) const uint8_t* host_address() { return reinterpret_cast<const uint8_t*>(&t_host); }

void** module_tls_slots() { return reinterpret_cast<void**>(__readgsqword(0x58)); }

const IMAGE_TLS_DIRECTORY64* tls_of(HMODULE module) {
    auto* base = reinterpret_cast<const uint8_t*>(module);
    auto* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    return dir.Size ? reinterpret_cast<const IMAGE_TLS_DIRECTORY64*>(base + dir.VirtualAddress) : nullptr;
}

// This EXE's TLS template: what every fresh block of it starts as.
struct Template {
    const uint8_t* data = nullptr;
    size_t size = 0;
};
Template own_template() {
    const auto* tls = tls_of(GetModuleHandleW(nullptr));
    return {reinterpret_cast<const uint8_t*>(tls->StartAddressOfRawData),
            static_cast<size_t>(tls->EndAddressOfRawData - tls->StartAddressOfRawData + tls->SizeOfZeroFill)};
}

// The home clone, found by its file name, and the index the OS gave it (from its own AddressOfIndex, not from
// this EXE's _tls_index).
HMODULE home_module() {
    const std::wstring name = L"leonida_tls_host_" + std::to_wstring(GetCurrentProcessId()) + L"_home.dll";
    return GetModuleHandleW(name.c_str());
}
DWORD home_index() {
    const HMODULE home = home_module();
    return home ? *reinterpret_cast<const DWORD*>(tls_of(home)->AddressOfIndex) : rt::kNoTlsIndex;
}

bool inside(const uint8_t* p, const void* block, size_t size) {
    const auto* b = static_cast<const uint8_t*>(block);
    return b && p >= b && p + sizeof(int) <= b + size;
}

// Where this thread finds t_host: in the block at the home index, and not in the one at slot 0.
bool host_tls_at_home() {
    const DWORD home = home_index();
    const size_t size = own_template().size;
    return home != rt::kNoTlsIndex && inside(host_address(), module_tls_slots()[home], size) &&
           !inside(host_address(), module_tls_slots()[0], size);
}

template <class Fn>
Fn find_export(const rt::Module& m, const char* name) {
    const auto it = m.exports.find(name);
    if (it == m.exports.end()) throw std::runtime_error(std::string("missing export ") + name);
    return reinterpret_cast<Fn>(it->second);
}

struct Ga {
    int(__cdecl* get)();
    void(__cdecl* set)(int);
    int*(__cdecl* address)();
    unsigned long(__cdecl* tls_index)();
    unsigned long*(__cdecl* tls_index_address)();
};

Ga ga_exports(const rt::Module& m) {
    return {find_export<int(__cdecl*)()>(m, "tiny_ga_get"), find_export<void(__cdecl*)(int)>(m, "tiny_ga_set"),
            find_export<int*(__cdecl*)()>(m, "tiny_ga_address"),
            find_export<unsigned long(__cdecl*)()>(m, "tiny_ga_tls_index"),
            find_export<unsigned long*(__cdecl*)()>(m, "tiny_ga_tls_index_address")};
}

// Code bytes of a function, past an incremental-linking jump stub (Debug builds export the stub).
const uint8_t* code_of(const void* fn) {
    const auto* p = static_cast<const uint8_t*>(fn);
    if (p[0] == 0xE9) {
        int32_t rel;
        std::memcpy(&rel, p + 1, 4);
        p += 5 + rel;
    }
    return p;
}

// The /GA shape of the getter, up to its ret: a load of gs:[58h] (65 REX.W 8B /r with a SIB of disp32 0x58) and no
// rip-relative operand that lands on the image's _tls_index.
bool reads_slot0_without_index(const void* fn, const void* tls_index) {
    const uint8_t* p = code_of(fn);
    bool gs58 = false, index = false;
    for (size_t i = 0; i < 64 && p[i] != 0xC3; ++i) {
        if (p[i] == 0x65 && (p[i + 1] & 0xF8) == 0x48 && p[i + 2] == 0x8B && (p[i + 3] & 0xC7) == 0x04 &&
            p[i + 4] == 0x25 && p[i + 5] == 0x58 && !p[i + 6] && !p[i + 7] && !p[i + 8])
            gs58 = true;
        int32_t disp;
        std::memcpy(&disp, p + i, 4);
        for (int tail = 4; tail <= 5; ++tail)  // the displacement ends the instruction or is followed by an imm8
            index |= p + i + tail + disp == tls_index;
    }
    return gs58 && !index;
}

// Calls the getter with stand-in blocks in slot 0 (77 at the variable's offset) and in slot `other` (88), and the
// executable's _tls_index pointing at `other`: /GA code reads 77, code that loads _tls_index reads 88. Slot `other`
// is this EXE's own TLS meanwhile, so nothing here touches thread_local data.
__declspec(noinline) int read_through_stand_ins(const Ga& ga, size_t offset, DWORD other) {
    alignas(64) static uint8_t zero_slot[4096], other_slot[4096];
    const int seventy_seven = 77, eighty_eight = 88;
    std::memcpy(zero_slot + offset, &seventy_seven, sizeof(int));
    std::memcpy(other_slot + offset, &eighty_eight, sizeof(int));
    void** slots = module_tls_slots();
    unsigned long* index = ga.tls_index_address();
    void* const saved0 = slots[0];
    void* const saved_other = slots[other];
    const unsigned long saved_index = *index;
    slots[0] = zero_slot;
    slots[other] = other_slot;
    *index = other;
    const int seen = ga.get();
    *index = saved_index;
    slots[other] = saved_other;
    slots[0] = saved0;
    return seen;
}

// What a thread started after the reservation sees, without any install call.
struct NewThreadView {
    bool from_template = false;  // its TLS at the home index starts from this EXE's template, not from zeros
    int ga = -1, ga_after_set = -1, host = -1, dll = -1;
    bool host_at_home = false;
    void* slot0 = nullptr;
};

// ga may be null: then only this EXE's own TLS is looked at.
NewThreadView run_new_thread(const Ga* ga, int(__cdecl* dll_get)() = nullptr) {
    NewThreadView v;
    std::thread t([&] {
        // Not the whole block: the helper's DLL_THREAD_ATTACH has already written the loader's own thread_local
        // state there. -1 is a template value that a zero-filled block would not have.
        v.from_template = t_seen_in_hook == -1;
        v.slot0 = module_tls_slots()[0];
        if (ga) {
            v.ga = ga->get();
            ga->set(9);
            v.ga_after_set = ga->get();
        }
        v.host = host_value();
        set_host_value(4321);
        v.host_at_home = host_tls_at_home();
        if (dll_get) v.dll = dll_get();
    });
    t.join();
    return v;
}

__declspec(noinline) bool call_reserve(std::string* why) { return rt::reserve_tls_index0(why); }

// ---- runs ----

// A thread started before the reservation that runs no TLS code before it. After the move it installs the
// executable's copy itself (install_tls_for_thread, as for any module and a thread older than load()).
struct EarlyThread {
    HANDLE go = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE quit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE handle = nullptr;
    rt::Loader* loader = nullptr;
    rt::Module* module = nullptr;
    Ga ga{};
    int host = -1, ga_installed = -1, ga_after_set = -1;
    bool host_at_home = false, block_in_slot0 = false, slot0_restored = false;

    static __declspec(noinline) void checks(EarlyThread* t) {
        t->host = host_value();
        t->host_at_home = host_tls_at_home();
        void* before = module_tls_slots()[0];
        t->loader->install_tls_for_thread(*t->module);
        t->block_in_slot0 = module_tls_slots()[0] != before;
        t->ga_installed = t->ga.get();
        t->ga.set(5);
        t->ga_after_set = t->ga.get();
        t->loader->uninstall_tls_for_thread(*t->module);
        t->slot0_restored = module_tls_slots()[0] == before;
    }
    static DWORD WINAPI body(void* p) {  // no TLS here: it runs from before the reservation
        auto* t = static_cast<EarlyThread*>(p);
        WaitForSingleObject(t->go, INFINITE);
        if (t->loader) checks(t);
        SetEvent(t->done);
        WaitForSingleObject(t->quit, INFINITE);  // ends after both Loaders are gone
        return 0;
    }
    __declspec(noinline) void start() { handle = CreateThread(nullptr, 0, &body, this, 0, nullptr); }
    void finish() {
        SetEvent(go);
        SetEvent(quit);
        if (handle) {
            WaitForSingleObject(handle, INFINITE);
            CloseHandle(handle);
        }
        for (HANDLE h : {go, done, quit}) CloseHandle(h);
    }
};

void test_first_run(const fs::path& exe, const fs::path& dll, EarlyThread& early, void* own_block) {
    std::printf("\n-- first run: %s --\n", exe.string().c_str());
    const DWORD home = home_index();
    void* block = nullptr;
    void* saved = nullptr;
    {
        rt::Loader loader(exe.parent_path());
        rt::Module& m = loader.load(exe);
        const Ga ga = ga_exports(m);
        std::printf("     this EXE's _tls_index is now %lu; the executable's block=%p\n", _tls_index, m.tls_block);

        check(m.tls_index == 0, "the mapped executable gets module TLS index 0");
        check(ga.tls_index() == 0, "its AddressOfIndex (_tls_index) holds 0");
        check(m.tls_block && module_tls_slots()[0] == m.tls_block, "slot 0 of this thread holds its template copy");
        check(ga.get() == 42, "its /GA getter reads the template value 42 on the loading thread");
        check(module_tls_slots()[home] == own_block && host_value() == 555,
              "this EXE's TLS on the loading thread is still the same block, now at the home index");
        check(m.tls_saved_block && m.tls_saved_block != own_block, "the block saved from slot 0 is not this EXE's");

        const auto offset = reinterpret_cast<const uint8_t*>(ga.address()) - static_cast<const uint8_t*>(m.tls_block);
        const bool offset_ok = offset >= 0 && static_cast<uint64_t>(offset) + sizeof(int) <= m.tls_block_size &&
                               static_cast<size_t>(offset) + sizeof(int) <= 4096;
        check(offset_ok, "the executable's thread_local lies inside its slot-0 block");
        if (offset_ok)
            check(read_through_stand_ins(ga, static_cast<size_t>(offset), home) == 77,
                  "the getter reads slot 0 even when its _tls_index names another slot (/GA code)");
#if LEONIDA_GA_FIXTURE_IS_GA
        check(reads_slot0_without_index(reinterpret_cast<const void*>(ga.get), ga.tls_index_address()),
              "the fixture's getter has the /GA shape: gs:[58h] and no load of _tls_index");
#else
        std::printf("SKIP the fixture's /GA code shape (this compiler may ignore /GA)\n");
#endif

        ga.set(43);
        const NewThreadView fresh = run_new_thread(&ga);
        check(fresh.from_template, "a new thread's TLS at the home index starts from this EXE's template");
        check(fresh.ga == 42, "a thread started after load() reads 42 without an install call");
        check(fresh.ga_after_set == 9 && ga.get() == 43, "its write stays in its own copy");
        check(fresh.slot0 != m.tls_block, "its slot 0 is its own block, not the loading thread's");
        check(fresh.host == 1234 && fresh.host_at_home,
              "this EXE's thread_local starts at 1234 there, at the home index");
        check(host_value() == 555, "this EXE's value on the loading thread is unchanged");

        early.loader = &loader;
        early.module = &m;
        early.ga = ga;
        SetEvent(early.go);
        WaitForSingleObject(early.done, INFINITE);
        check(early.host == 1234 && early.host_at_home,
              "a thread that existed before the reservation finds a fresh copy of this EXE's TLS at the home index");
        // With main's install_on_existing_threads, load() itself gave this already-running thread its copy in slot 0
        // (the executable's slot included), so install_tls_for_thread is a no-op there and slot 0 does not change
        // across the call. Without that, install_tls_for_thread puts the copy in. Either way the thread reads its own.
        check(early.ga_installed == 42 && early.ga_after_set == 5,
              "that thread has its own copy in slot 0 (from load(), or from install_tls_for_thread)");
        check(early.slot0_restored, "uninstall_tls_for_thread puts that thread's slot 0 back");

        loader.initialise();
        check(m.initialised, "initialise() runs the executable's TLS callbacks (it has none)");
        check(run_new_thread(&ga).ga == 42, "after initialise() a new thread still reads 42");

        rt::Module& d = loader.load(dll);
        const auto dll_get = find_export<int(__cdecl*)()>(d, "tiny_tls_get");
        check(d.tls_index != rt::kNoTlsIndex && d.tls_index != 0 && d.tls_index != home,
              "tiny_tls.dll gets a helper index, neither 0 nor the home index");
        check(dll_get() == 7 && ga.get() == 43 && host_value() == 555,
              "the DLL, the executable and this EXE each read their own value on the loading thread");
        const NewThreadView both = run_new_thread(&ga, dll_get);
        check(both.dll == 7 && both.ga == 42 && both.host == 1234,
              "a new thread reads the DLL's 7, the executable's 42 and this EXE's 1234");

        block = m.tls_block;
        saved = m.tls_saved_block;
    }
    check(module_tls_slots()[0] == saved, "~Loader puts slot 0's earlier block back");
    MEMORY_BASIC_INFORMATION info{};
    check(VirtualQuery(block, &info, sizeof(info)) == 0 || info.State == MEM_FREE, "~Loader frees the copy");
    check(_tls_index == home && host_value() == 555, "this EXE keeps the home index and its value after the run");
}

void test_second_run(const fs::path& exe) {
    std::printf("\n-- second run, and a second executable at once --\n");
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    const fs::path copy =
        fs::path(temp) / ("leonida_tls_index0_copy_" + std::to_string(GetCurrentProcessId()) + ".exe");
    // CopyFileW, not fs::copy_file: MSVC's copy_file calls CopyFile2, which Wine 9 doesn't implement.
    check(CopyFileW(exe.c_str(), copy.c_str(), FALSE) != 0, "copied the executable under a second name");
    {
        rt::Loader loader(exe.parent_path());
        rt::Module& m = loader.load(exe);
        const Ga ga = ga_exports(m);
        loader.initialise();
        check(m.tls_index == 0 && ga.get() == 42, "a second Loader maps the executable at index 0 again, value 42");
        check(run_new_thread(&ga).ga == 42 && host_value() == 555, "new threads and this EXE's value still check out");

        rt::Module& other = loader.load(copy);
        const Ga other_ga = ga_exports(other);
        check(other.tls_index != 0 && other.tls_index != rt::kNoTlsIndex && other_ga.tls_index() == other.tls_index,
              "a second executable mapped meanwhile gets a helper index in its _tls_index");
        check(m.tls_index == 0 && ga.get() == 42, "the first keeps index 0 and its value");
    }
    std::error_code ec;
    fs::remove(copy, ec);
}

void test_home_template() {
    std::printf("\n-- the home clone --\n");
    const HMODULE home = home_module();
    check(home != nullptr, "the home clone is loaded");
    if (!home) return;
    const auto* tls = tls_of(home);
    const Template tmpl = own_template();
    std::printf("     this EXE's TLS is %zu bytes; the home clone's template is %llu bytes\n", tmpl.size,
                static_cast<unsigned long long>(tls->EndAddressOfRawData - tls->StartAddressOfRawData));
    check(tls->EndAddressOfRawData - tls->StartAddressOfRawData == tmpl.size,
          "the home template is exactly as large as this EXE's TLS (template plus zero fill)");
    check(std::memcmp(reinterpret_cast<const void*>(tls->StartAddressOfRawData), tmpl.data, tmpl.size) == 0,
          "the home template holds this EXE's template");
}

// Runs before the reservation: TLS here is fine, as no frame outlives the call.
__declspec(noinline) void* before_reservation() {
    check(_tls_index == 0, "before the reservation this test EXE holds module TLS index 0 (Windows gave it)");
    set_host_value(555);
    check(inside(host_address(), module_tls_slots()[0], own_template().size), "its TLS is the block in slot 0");
    return module_tls_slots()[0];
}

__declspec(noinline) void after_reservation(bool reserved, const std::string& why, void* own_block, EarlyThread& early,
                                           const fs::path& exe, const fs::path& dll) {
    if (!why.empty()) std::printf("     reserve_tls_index0: %s\n", why.c_str());
    check(reserved && why.empty(), "reserve_tls_index0 succeeds");
    const DWORD home = home_index();
    check(home != rt::kNoTlsIndex && home != 0 && _tls_index == home,
          "this EXE's _tls_index is the home clone's own index");
    check(module_tls_slots()[home] == own_block && module_tls_slots()[0] != own_block && host_value() == 555,
          "on the reserving thread this EXE's block moved, with its value, from slot 0 to the home index");
    std::string again;
    check(rt::reserve_tls_index0(&again) && again.empty(), "a second call returns the first result");
    test_home_template();
    test_first_run(exe, dll, early, own_block);
    test_second_run(exe);
}

__declspec(noinline) int scenario_runs(const fs::path& exe, const fs::path& dll) {
    void* own_block = before_reservation();
    EarlyThread early;
    early.start();
    std::string why;
    const bool reserved = call_reserve(&why);
    after_reservation(reserved, why, own_block, early, exe, dll);
    early.finish();
    return 0;
}

// ---- threads ----

constexpr int kChurn = 64;
struct ThreadResult {
    int host = -1, seen_in_hook = -1;
    bool hooked = false, at_home = false, fresh = false;
};
ThreadResult g_results[kChurn + 1];
HANDLE g_handles[kChurn + 1];
HANDLE g_go = nullptr;
std::atomic<bool> g_hook_active{false};
std::atomic<int> g_created{0};
std::atomic<bool> g_stop_churn{false};

// Runs in tiny_thread_hook.dll's DllMain(DLL_THREAD_ATTACH), before the TLS helper's own DLL_THREAD_ATTACH.
void __cdecl hook_body() {
    if (!g_hook_active.load()) return;  // TLS only once the reservation is done
    t_seen_in_hook = host_value();
    t_hooked = host_tls_at_home();
    set_host_value(4321);
}

__declspec(noinline) void record(int i) {
    g_results[i].hooked = t_hooked;
    g_results[i].seen_in_hook = t_seen_in_hook;
    g_results[i].host = host_value();
    g_results[i].at_home = host_tls_at_home();
}

DWORD WINAPI churn_body(void* p) {  // no TLS before the wait: it may start before the reservation
    WaitForSingleObject(g_go, INFINITE);
    record(static_cast<int>(reinterpret_cast<intptr_t>(p)));
    return 0;
}

DWORD WINAPI churn_creator(void*) {  // runs through the reservation and touches no TLS
    for (int i = 1; i <= kChurn; ++i) {
        void* index = reinterpret_cast<void*>(static_cast<intptr_t>(i));
        g_handles[i] = CreateThread(nullptr, 0, &churn_body, index, 0, nullptr);
        g_created.store(i);
        if (i >= kChurn / 2) while (!g_stop_churn.load()) Sleep(0);  // the rest after the reservation
        Sleep(0);
    }
    return 0;
}

using NtQueryInformationThreadFn = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);

// TEB->ThreadLocalStoragePointer of another thread.
void* tls_array_of(HANDLE thread) {
    struct {
        LONG exit_status;
        PVOID teb;
        PVOID ids[2];
        ULONG_PTR affinity;
        LONG priority, base_priority;
    } info{};
    const auto query = reinterpret_cast<NtQueryInformationThreadFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    if (!query || query(thread, 0, &info, sizeof(info), nullptr) < 0 || !info.teb) return reinterpret_cast<void*>(1);
    return *reinterpret_cast<void**>(static_cast<uint8_t*>(info.teb) + 0x58);
}

__declspec(noinline) void threads_after(bool reserved, HANDLE suspended, bool suspended_had_no_array) {
    check(reserved, "reserve_tls_index0 succeeds while threads are being created");
    check(suspended_had_no_array, "the thread created suspended has no TLS array yet when the reservation starts");
    std::printf("     %d threads created before the reservation returned\n", g_created.load());
    g_hook_active = true;
    SetEvent(g_go);
    g_stop_churn = true;
    ResumeThread(suspended);
    HANDLE creator = g_handles[0];
    WaitForSingleObject(creator, INFINITE);
    CloseHandle(creator);
    WaitForSingleObject(suspended, INFINITE);
    CloseHandle(suspended);
    int hooked = 0, ok = 0, window_ok = 0;
    for (int i = 1; i <= kChurn; ++i) {
        WaitForSingleObject(g_handles[i], INFINITE);
        CloseHandle(g_handles[i]);
    }
    for (int i = 0; i <= kChurn; ++i) {
        const ThreadResult& r = g_results[i];
        ok += r.at_home && r.host == (r.hooked ? 4321 : 1234);
        hooked += r.hooked;
        window_ok += r.hooked && r.seen_in_hook == 1234;
    }
    std::printf("     %d of %d threads ran the hook in DLL_THREAD_ATTACH\n", hooked, kChurn + 1);
    check(ok == kChurn + 1, "every thread created before, during or after the reservation finds this EXE's TLS at the "
                            "home index, with the value it should have");
    check(g_results[0].hooked && g_results[0].at_home && g_results[0].host == 4321,
          "the suspended thread, resumed afterwards, gets a fresh copy and keeps the hook's write");
    check(hooked > 0 && window_ok == hooked,
          "code of this EXE run before the helper's DLL_THREAD_ATTACH reads a fresh copy of its TLS (1234) there");
}

__declspec(noinline) int scenario_threads(const fs::path& hook_dll) {
    HMODULE hook = LoadLibraryW(hook_dll.c_str());  // before the helper, so its DLL_THREAD_ATTACH comes first
    using SetHook = void(__cdecl*)(void(__cdecl*)());
    auto set_hook = hook ? reinterpret_cast<SetHook>(GetProcAddress(hook, "tiny_thread_hook_set")) : nullptr;
    check(set_hook != nullptr, "tiny_thread_hook.dll is loaded");
    if (!set_hook) return 1;
    set_hook(&hook_body);
    g_go = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE suspended = CreateThread(nullptr, 0, &churn_body, nullptr, CREATE_SUSPENDED, nullptr);
    g_handles[0] = CreateThread(nullptr, 0, &churn_creator, nullptr, 0, nullptr);
    while (g_created.load() < 4) Sleep(0);
    const bool no_array = tls_array_of(suspended) == nullptr;
    const bool reserved = call_reserve(nullptr);
    threads_after(reserved, suspended, no_array);
    CloseHandle(g_go);
    return 0;
}

// ---- noreserve ----

__declspec(noinline) int scenario_noreserve(const fs::path& exe) {
    rt::Loader loader(exe.parent_path());
    rt::Module& m = loader.load(exe);
    check(m.tls_index != 0 && m.tls_index != rt::kNoTlsIndex,
          "without reserve_tls_index0 the executable gets a helper index");
    check(_tls_index == 0, "and this EXE keeps module TLS index 0");
    check(ga_exports(m).tls_index() == m.tls_index, "the executable's _tls_index holds that helper index");
    // Set in initialize_tls before the slot owner is published (RT-LOADER-RUNTIME-SCOPE-AND-TLS-SLOT).
    check(m.tls_ga_slot != rt::kNoTlsIndex && m.tls_ga_sites > 0,
          "its index-0 (/GA) reads are rewritten to a TlsAlloc slot, as it does not hold index 0");
    return 0;
}

void print_result() {
    if (g_failures == 0)
        std::printf("\nloader_tls_index0_test: OK (%d checks)\n", g_checks);
    else
        std::printf("\nloader_tls_index0_test: FAILED (%d checks, %d failure%s)\n", g_checks, g_failures,
                    g_failures == 1 ? "" : "s");
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc != 5) {
        std::fprintf(stderr,
                     "usage: loader_tls_index0_test runs|threads|noreserve <tiny_tls_ga.exe> <tiny_tls.dll> "
                     "<tiny_thread_hook.dll>\n");
        return 2;
    }
    std::printf("loader_tls_index0_test %s: module TLS index 0 for a mapped executable\n", argv[1]);
    const std::string scenario = argv[1];
    const fs::path exe = argv[2], dll = argv[3], hook = argv[4];
    try {
        if (!fs::exists(exe) || !fs::exists(dll) || !fs::exists(hook)) throw std::runtime_error("a fixture is missing");
        if (scenario == "runs") scenario_runs(exe, dll);
        else if (scenario == "threads") scenario_threads(hook);
        else if (scenario == "noreserve") scenario_noreserve(exe);
        else throw std::runtime_error("unknown scenario " + scenario);
    } catch (const std::exception& e) {
        ++g_checks;
        ++g_failures;
        std::printf("FAIL unexpected exception: %s\n", e.what());
    }
    print_result();
    return g_failures ? 1 : 0;
}
