// COM lifecycle and helper shims for api-ms-win-core-com-l1-1-0..3.dll.
//
// game_scarlett_master.exe imports CoInitializeEx, CoUninitialize, CoCreateInstance, CoCreateGuid, CoTaskMemFree,
// PropVariantClear, and StringFromGUID2 through api-ms-win-core-com-l1-1-0.dll. CoCreateInstance of the
// MMDeviceEnumerator goes to silent_audio.cpp, which serves a silent output endpoint when the host has none. When the host (or a host library) has
// already initialized a thread as STA, a guest call to CoInitializeEx(nullptr, COINIT_MULTITHREADED) fails with
// RPC_E_CHANGED_MODE (0x80010106) and an unbalanced CoUninitialize can tear down host COM early.
//
// Per-thread guest COM depth and host init ownership are tracked via a TlsAlloc slot (constant-initialized via
// InitOnceExecuteOnce with zero _Init_thread_header TLS guards or C++ thread_local destructors).
#include <windows.h>
#include <objbase.h>
#include <propidl.h>

#include <cstdint>
#include <new>

#include "shims.h"
#include "silent_audio.h"

namespace rt::com_shim {
namespace {

struct ThreadComState {
    uint32_t guest_depth = 0;
    uint32_t host_inits_owned = 0;
    DWORD requested_coinit = COINIT_MULTITHREADED;
    DWORD host_coinit = COINIT_MULTITHREADED;
    bool mismatch_absorbed = false;
};

INIT_ONCE g_tls_once = INIT_ONCE_STATIC_INIT;
DWORD g_tls_index = TLS_OUT_OF_INDEXES;

BOOL CALLBACK init_tls_once(PINIT_ONCE, PVOID, PVOID*) {
    g_tls_index = TlsAlloc();
    return TRUE;
}

DWORD tls_slot() {
    InitOnceExecuteOnce(&g_tls_once, init_tls_once, nullptr, nullptr);
    return g_tls_index;
}

bool apartment_differs(DWORD a, DWORD b) {
    return (a & COINIT_APARTMENTTHREADED) != (b & COINIT_APARTMENTTHREADED);
}

DWORD detect_existing_host_coinit(DWORD attempted_coinit) {
    APTTYPE apt = APTTYPE_CURRENT;
    APTTYPEQUALIFIER qual = APTTYPEQUALIFIER_NONE;
    DWORD base = (attempted_coinit & COINIT_APARTMENTTHREADED) ? COINIT_MULTITHREADED : COINIT_APARTMENTTHREADED;
    if (SUCCEEDED(::CoGetApartmentType(&apt, &qual))) {
        if (apt == APTTYPE_STA || apt == APTTYPE_MAINSTA) base = COINIT_APARTMENTTHREADED;
        else if (apt == APTTYPE_MTA) base = COINIT_MULTITHREADED;
    }
    return base | (attempted_coinit & (COINIT_DISABLE_OLE1DDE | COINIT_SPEED_OVER_MEMORY));
}

HRESULT WINAPI co_initialize_ex(LPVOID reserved, DWORD coinit) {
    if (reserved != nullptr) return E_INVALIDARG;

    const DWORD slot = tls_slot();
    if (slot == TLS_OUT_OF_INDEXES) return ::CoInitializeEx(nullptr, coinit);

    auto* state = static_cast<ThreadComState*>(TlsGetValue(slot));
    if (!state) {
        const HRESULT hr = ::CoInitializeEx(nullptr, coinit);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return hr;

        DWORD owned_mode = coinit;
        bool absorbed = false;
        uint32_t owned_count = 1;
        if (hr == RPC_E_CHANGED_MODE) {
            // Retain an actual host COM reference in the thread's existing apartment so guest COM stays alive even if
            // the host calls CoUninitialize() while the guest still holds an active initialization.
            owned_mode = detect_existing_host_coinit(coinit);
            const HRESULT hold_hr = ::CoInitializeEx(nullptr, owned_mode);
            if (FAILED(hold_hr)) return hold_hr;
            absorbed = true;
        }

        auto* created = new (std::nothrow) ThreadComState{};
        if (!created) {
            if (owned_count) ::CoUninitialize();
            return E_OUTOFMEMORY;
        }
        created->guest_depth = 1;
        created->host_inits_owned = owned_count;
        created->requested_coinit = coinit;
        created->host_coinit = owned_mode;
        created->mismatch_absorbed = absorbed;
        if (!TlsSetValue(slot, created)) {
            // Not tracked, so no CoUninitialize would ever give the host reference back: undo it and fail.
            const DWORD error = GetLastError();
            delete created;
            ::CoUninitialize();
            return HRESULT_FROM_WIN32(error ? error : ERROR_NOT_ENOUGH_MEMORY);
        }
        return S_OK;
    }

    const bool matches_host = !apartment_differs(state->host_coinit, coinit);
    const bool matches_absorbed_guest = state->mismatch_absorbed && !apartment_differs(state->requested_coinit, coinit);
    if (!matches_host && !matches_absorbed_guest) return RPC_E_CHANGED_MODE;

    const DWORD forward_mode =
        matches_host ? coinit : (state->host_coinit | (coinit & (COINIT_DISABLE_OLE1DDE | COINIT_SPEED_OVER_MEMORY)));
    const HRESULT hr = ::CoInitializeEx(nullptr, forward_mode);
    if (FAILED(hr)) return hr;

    ++state->host_inits_owned;
    ++state->guest_depth;
    return S_FALSE;
}

void WINAPI co_uninitialize() {
    const DWORD slot = tls_slot();
    if (slot == TLS_OUT_OF_INDEXES) {
        ::CoUninitialize();
        return;
    }
    auto* state = static_cast<ThreadComState*>(TlsGetValue(slot));
    if (!state || state->guest_depth == 0) return;

    --state->guest_depth;
    if (state->guest_depth != 0) {
        // A nested init took its own host reference: give it back now, not at the outermost CoUninitialize, so balanced
        // inner pairs do not pile up host references. The first init's reference stays until the guest balances.
        if (state->host_inits_owned > 1) {
            ::CoUninitialize();
            --state->host_inits_owned;
        }
        return;
    }
    for (uint32_t i = 0; i < state->host_inits_owned; ++i) {
        ::CoUninitialize();
    }
    TlsSetValue(slot, nullptr);
    delete state;
}

HRESULT WINAPI co_create_instance(REFCLSID rclsid, LPUNKNOWN outer, DWORD cls_context, REFIID riid, LPVOID* ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    // The audio device enumerator: the host's when it has an output, else (or with LEONIDA_AUDIO=silent) one that
    // also answers with the silent output endpoint (silent_audio.h).
    if (!outer && IsEqualCLSID(rclsid, rt::silent_audio::kClsidMMDeviceEnumerator))
        return rt::silent_audio::create_enumerator(cls_context, riid, ppv);
    return ::CoCreateInstance(rclsid, outer, cls_context, riid, ppv);
}

HRESULT WINAPI co_create_guid(GUID* guid) {
    if (!guid) return E_INVALIDARG;
    return ::CoCreateGuid(guid);
}

void WINAPI co_task_mem_free(LPVOID pv) { ::CoTaskMemFree(pv); }

HRESULT WINAPI prop_variant_clear(PROPVARIANT* pvar) { return ::PropVariantClear(pvar); }

int WINAPI string_from_guid2(REFGUID rguid, LPOLESTR lpsz, int cch_max) {
    if (!lpsz || cch_max <= 0) return 0;
    return ::StringFromGUID2(rguid, lpsz, cch_max);
}

}  // namespace
}  // namespace rt::com_shim

namespace {

using namespace rt::com_shim;

#define COM_L1_API(dll)                                                                  \
    {dll, "CoInitializeEx", reinterpret_cast<void*>(&co_initialize_ex)},                 \
        {dll, "CoUninitialize", reinterpret_cast<void*>(&co_uninitialize)},              \
        {dll, "CoCreateInstance", reinterpret_cast<void*>(&co_create_instance)},         \
        {dll, "CoCreateGuid", reinterpret_cast<void*>(&co_create_guid)},                 \
        {dll, "CoTaskMemFree", reinterpret_cast<void*>(&co_task_mem_free)},              \
        {dll, "PropVariantClear", reinterpret_cast<void*>(&prop_variant_clear)},         \
        {dll, "StringFromGUID2", reinterpret_cast<void*>(&string_from_guid2)}

}  // namespace

// Registered strictly on the api-ms-win-core-com API sets (not ole32.dll/combase.dll) so rt::shims::owns_dll keeps
// host COM DLLs unbound to pseudo-handles while answering all mapped api-ms-win-core-com-* imports.
SHIM_TABLE(com_shim,
    COM_L1_API("api-ms-win-core-com-l1-1-0.dll"),
    COM_L1_API("api-ms-win-core-com-l1-1-1.dll"),
    COM_L1_API("api-ms-win-core-com-l1-1-2.dll"),
    COM_L1_API("api-ms-win-core-com-l1-1-3.dll"),
);
