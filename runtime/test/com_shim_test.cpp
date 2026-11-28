// Tests for runtime/shims/com_shim.cpp through rt::shims::find.
#include <windows.h>
#include <objbase.h>
#include <propidl.h>

#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "shims.h"

namespace {

using CoInitExFn = HRESULT(WINAPI*)(LPVOID, DWORD);
using CoUninitFn = void(WINAPI*)();
using CoCreateInstFn = HRESULT(WINAPI*)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
using CoCreateGuidFn = HRESULT(WINAPI*)(GUID*);
using CoTaskMemFreeFn = void(WINAPI*)(LPVOID);
using PropVarClearFn = HRESULT(WINAPI*)(PROPVARIANT*);
using StringFromGuid2Fn = int(WINAPI*)(REFGUID, LPOLESTR, int);

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

template <typename Fn>
Fn resolve(const char* name) {
    return reinterpret_cast<Fn>(rt::shims::find("api-ms-win-core-com-l1-1-0.dll", name));
}

void test_registry_coverage() {
    for (const char* dll : {"api-ms-win-core-com-l1-1-0.dll", "api-ms-win-core-com-l1-1-1.dll",
                            "api-ms-win-core-com-l1-1-2.dll", "api-ms-win-core-com-l1-1-3.dll"}) {
        check(rt::shims::find(dll, "CoInitializeEx") != nullptr, "CoInitializeEx resolves");
        check(rt::shims::find(dll, "CoUninitialize") != nullptr, "CoUninitialize resolves");
        check(rt::shims::find(dll, "CoCreateInstance") != nullptr, "CoCreateInstance resolves");
        check(rt::shims::find(dll, "CoCreateGuid") != nullptr, "CoCreateGuid resolves");
        check(rt::shims::find(dll, "CoTaskMemFree") != nullptr, "CoTaskMemFree resolves");
        check(rt::shims::find(dll, "PropVariantClear") != nullptr, "PropVariantClear resolves");
        check(rt::shims::find(dll, "StringFromGUID2") != nullptr, "StringFromGUID2 resolves");
    }
    check(!rt::shims::owns_dll("ole32.dll"), "ole32.dll stays host-owned");
    check(!rt::shims::owns_dll("api-ms-win-core-com-l1-1-0.dll"), "api-ms-win-core-com-l1-1-0.dll is not pseudo-owned");
}

void test_com_lifecycle_and_sta_isolation() {
    auto co_init = resolve<CoInitExFn>("CoInitializeEx");
    auto co_uninit = resolve<CoUninitFn>("CoUninitialize");

    // 1. Fresh thread: MTA init + nested MTA + intra-guest mismatch -> RPC_E_CHANGED_MODE + clean unwind.
    bool fresh_ok = false;
    std::thread([&]() {
        int dummy = 1;
        const HRESULT bad_res = co_init(&dummy, COINIT_MULTITHREADED);
        const HRESULT r1 = co_init(nullptr, COINIT_MULTITHREADED);
        const HRESULT r2 = co_init(nullptr, COINIT_MULTITHREADED);
        const HRESULT r_mix = co_init(nullptr, COINIT_APARTMENTTHREADED);
        co_uninit();
        co_uninit();
        co_uninit();  // extra unbalanced uninit must be ignored safely
        // Host COM should now be uninitialized again on this thread; CoInitializeEx(STA) must return S_OK.
        const HRESULT host_after = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(host_after)) ::CoUninitialize();
        fresh_ok = (bad_res == E_INVALIDARG && r1 == S_OK && r2 == S_FALSE && r_mix == RPC_E_CHANGED_MODE &&
                    host_after == S_OK);
    }).join();
    check(fresh_ok, "fresh thread balances guest COM without leaking or corrupting host apartment");

    // 2. Host pre-initialized as STA: guest requests MTA -> shim absorbs RPC_E_CHANGED_MODE, retains host apartment
    //    reference so guest outlives host teardown cleanly, and accepts matching STA on the real STA thread.
    bool mismatch_ok = false;
    std::thread([&]() {
        const HRESULT host_sta = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const HRESULT g1 = co_init(nullptr, COINIT_MULTITHREADED);
        const HRESULT g_sta = co_init(nullptr, COINIT_APARTMENTTHREADED);
        const HRESULT g2 = co_init(nullptr, COINIT_MULTITHREADED);
        co_uninit();
        co_uninit();
        co_uninit();
        // Host STA must STILL be active on this thread (second host STA init must return S_FALSE, not S_OK!).
        const HRESULT host_still_sta = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(host_still_sta)) ::CoUninitialize();
        if (SUCCEEDED(host_sta)) ::CoUninitialize();
        mismatch_ok =
            (host_sta == S_OK && g1 == S_OK && g_sta == S_FALSE && g2 == S_FALSE && host_still_sta == S_FALSE);
    }).join();
    check(mismatch_ok, "guest MTA under host STA avoids RPC_E_CHANGED_MODE and keeps host STA alive");

    // 3. Host-first teardown while guest COM initialization remains active (regression for #9516 review).
    auto create_inst = resolve<CoCreateInstFn>("CoCreateInstance");
    bool host_first_teardown_ok = false;
    std::thread([&]() {
        const HRESULT host_sta = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const HRESULT g1 = co_init(nullptr, COINIT_MULTITHREADED);
        // Host balances its own initialization while guest depth is still 1.
        if (SUCCEEDED(host_sta)) ::CoUninitialize();

        APTTYPE apt = APTTYPE_CURRENT;
        APTTYPEQUALIFIER qual = APTTYPEQUALIFIER_NONE;
        const HRESULT apt_hr = ::CoGetApartmentType(&apt, &qual);
        void* out = reinterpret_cast<void*>(1);
        const HRESULT cls_hr = create_inst(GUID_NULL, nullptr, CLSCTX_INPROC_SERVER, IID_IUnknown, &out);

        // Now guest balances; COM should become uninitialized afterwards.
        co_uninit();
        const HRESULT apt_after_guest = ::CoGetApartmentType(&apt, &qual);
        host_first_teardown_ok =
            (host_sta == S_OK && g1 == S_OK && SUCCEEDED(apt_hr) && cls_hr == REGDB_E_CLASSNOTREG && out == nullptr &&
             apt_after_guest == CO_E_NOTINITIALIZED);
    }).join();
    check(host_first_teardown_ok, "guest COM stays live after host CoUninitialize and unwinds cleanly on guest balance");

    // 4. Balanced nested pairs under an outer init give their host references back as they go (kevincommits' finding
    //    in #1608, Finding 19 item 1): only the outer init's reference is left, so one host CoUninitialize ends COM
    //    on the thread. They used to stay until the outermost CoUninitialize, one per pair.
    int host_refs_left = -1;
    bool nested_ok = false;
    std::thread([&]() {
        const HRESULT outer = co_init(nullptr, COINIT_MULTITHREADED);
        bool inner_ok = true;
        for (int i = 0; i < 100; ++i) {
            inner_ok = inner_ok && co_init(nullptr, COINIT_MULTITHREADED) == S_FALSE;
            co_uninit();
        }
        APTTYPE apt = APTTYPE_CURRENT;
        APTTYPEQUALIFIER qual = APTTYPEQUALIFIER_NONE;
        const HRESULT still_live = ::CoGetApartmentType(&apt, &qual);
        // Count the host references by giving them back one at a time (this thread ends right after).
        int refs = 0;
        while (refs < 1000 && SUCCEEDED(::CoGetApartmentType(&apt, &qual))) {
            ::CoUninitialize();
            ++refs;
        }
        host_refs_left = refs;
        co_uninit();  // the guest's outer balance: the shim's state goes
        nested_ok = outer == S_OK && inner_ok && SUCCEEDED(still_live);
    }).join();
    check(nested_ok, "an outer init stays live across 100 balanced nested pairs");
    check(host_refs_left == 1, "balanced nested pairs leave only the outer init's host reference");
}

void test_com_helpers() {
    auto co_init = resolve<CoInitExFn>("CoInitializeEx");
    auto co_uninit = resolve<CoUninitFn>("CoUninitialize");
    auto create_inst = resolve<CoCreateInstFn>("CoCreateInstance");
    auto create_guid = resolve<CoCreateGuidFn>("CoCreateGuid");
    auto task_free = resolve<CoTaskMemFreeFn>("CoTaskMemFree");
    auto prop_clear = resolve<PropVarClearFn>("PropVariantClear");
    auto guid_str = resolve<StringFromGuid2Fn>("StringFromGUID2");

    check(co_init(nullptr, COINIT_MULTITHREADED) == S_OK, "CoInitializeEx succeeds for helper checks");

    void* null_out = reinterpret_cast<void*>(1);
    check(create_inst(GUID_NULL, nullptr, CLSCTX_INPROC_SERVER, IID_IUnknown, nullptr) == E_POINTER,
          "CoCreateInstance rejects null out pointer");
    check(create_inst(GUID_NULL, nullptr, CLSCTX_INPROC_SERVER, IID_IUnknown, &null_out) != S_OK && null_out == nullptr,
          "CoCreateInstance zeroes out pointer on unregistered CLSID");

    check(create_guid(nullptr) == E_INVALIDARG, "CoCreateGuid rejects nullptr");
    GUID g1{}, g2{};
    check(create_guid(&g1) == S_OK && create_guid(&g2) == S_OK &&
              std::memcmp(&g1, &g2, sizeof(GUID)) != 0,
          "CoCreateGuid produces distinct GUIDs");

    wchar_t buf[64]{};
    check(guid_str(g1, nullptr, 64) == 0 && guid_str(g1, buf, 0) == 0,
          "StringFromGUID2 rejects invalid output buffer");
    check(guid_str(g1, buf, 64) == 39 && buf[0] == L'{' && buf[37] == L'}',
          "StringFromGUID2 formats 39-char braced GUID");

    PROPVARIANT pv{};
    PropVariantInit(&pv);
    pv.vt = VT_LPWSTR;
    pv.pwszVal = static_cast<wchar_t*>(CoTaskMemAlloc(8 * sizeof(wchar_t)));
    if (pv.pwszVal) std::wmemcpy(pv.pwszVal, L"abc", 4);
    check(prop_clear(&pv) == S_OK && pv.vt == VT_EMPTY, "PropVariantClear resets PROPVARIANT");

    void* mem = CoTaskMemAlloc(32);
    task_free(mem);
    task_free(nullptr);
    check(true, "CoTaskMemFree accepts valid and null pointers");

    co_uninit();
}

}  // namespace

int main() {
    test_registry_coverage();
    test_com_lifecycle_and_sta_isolation();
    test_com_helpers();
    std::printf("%d check(s), %d failure(s)\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
