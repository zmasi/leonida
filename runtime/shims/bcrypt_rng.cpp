// BCrypt RNG and algorithm provider shims (bcrypt.dll).
//
// game_scarlett_master.exe directly imports BCryptOpenAlgorithmProvider, BCryptGenRandom, and
// BCryptCloseAlgorithmProvider (all 3 imports of bcrypt.dll). This shim:
//   - tracks live BCRYPT_ALG_HANDLEs so double-close, null, or foreign handles fail safely with
//     STATUS_INVALID_HANDLE (0xC0000008) instead of corrupting or faulting in host bcryptprimitives.dll;
//   - delegates hardware CSPRNG generation to dynamically loaded host bcrypt.dll (with SystemFunction036 fallback);
//   - supports an optional deterministic LEONIDA_RNG_SEED=<uint64> mode (decimal or 0x hex) for reproducible
//     testing/debugging runs.
#include <windows.h>

#ifndef _NTDEF_
using NTSTATUS = LONG;
#endif
#include <bcrypt.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <new>

#include "shims.h"

namespace rt::bcrypt_rng {
namespace {

constexpr NTSTATUS kStatusSuccess = 0x00000000L;
constexpr NTSTATUS kStatusUnsuccessful = static_cast<NTSTATUS>(0xC0000001L);
constexpr NTSTATUS kStatusInvalidHandle = static_cast<NTSTATUS>(0xC0000008L);
constexpr NTSTATUS kStatusInvalidParameter = static_cast<NTSTATUS>(0xC000000DL);
constexpr NTSTATUS kStatusNoMemory = static_cast<NTSTATUS>(0xC0000017L);
constexpr NTSTATUS kStatusNotFound = static_cast<NTSTATUS>(0xC0000225L);

using OpenAlgFn = NTSTATUS(WINAPI*)(BCRYPT_ALG_HANDLE*, LPCWSTR, LPCWSTR, ULONG);
using CloseAlgFn = NTSTATUS(WINAPI*)(BCRYPT_ALG_HANDLE, ULONG);
using GenRandomFn = NTSTATUS(WINAPI*)(BCRYPT_ALG_HANDLE, PUCHAR, ULONG, ULONG);
using RtlGenRandomFn = BOOLEAN(WINAPI*)(PVOID, ULONG);

struct AlgHandle {
    BCRYPT_ALG_HANDLE host_handle;
    bool is_rng;
    AlgHandle* next;
};

INIT_ONCE g_init_once = INIT_ONCE_STATIC_INIT;
OpenAlgFn g_host_open = nullptr;
CloseAlgFn g_host_close = nullptr;
GenRandomFn g_host_gen = nullptr;
RtlGenRandomFn g_rtl_gen_random = nullptr;
bool g_seeded_mode = false;
uint64_t g_prng_state = 0;
std::atomic<bool> g_disable_host_rng_for_test{false};

SRWLOCK g_lock = SRWLOCK_INIT;
AlgHandle* g_live_handles = nullptr;

bool parse_u64(const char* text, uint64_t& out) {
    if (!text || !*text) return false;
    char* end = nullptr;
    const unsigned long long val = std::strtoull(text, &end, 0);
    if (!end || *end != '\0') return false;
    out = static_cast<uint64_t>(val);
    return true;
}

BOOL CALLBACK init_bcrypt_once(PINIT_ONCE, PVOID, PVOID*) {
    if (const HMODULE m = LoadLibraryExW(L"bcrypt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        g_host_open = reinterpret_cast<OpenAlgFn>(GetProcAddress(m, "BCryptOpenAlgorithmProvider"));
        g_host_close = reinterpret_cast<CloseAlgFn>(GetProcAddress(m, "BCryptCloseAlgorithmProvider"));
        g_host_gen = reinterpret_cast<GenRandomFn>(GetProcAddress(m, "BCryptGenRandom"));
    }
    if (const HMODULE adv = LoadLibraryExW(L"advapi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        g_rtl_gen_random = reinterpret_cast<RtlGenRandomFn>(GetProcAddress(adv, "SystemFunction036"));
    }

    char seed_buf[64]{};
    const DWORD len = GetEnvironmentVariableA("LEONIDA_RNG_SEED", seed_buf, static_cast<DWORD>(sizeof(seed_buf)));
    uint64_t seed = 0;
    if (len > 0 && len < sizeof(seed_buf) && parse_u64(seed_buf, seed)) {
        g_seeded_mode = true;
        g_prng_state = seed ? seed : 0x9E3779B97F4A7C15ULL;
        std::fprintf(stderr, "[rt] bcrypt: deterministic LEONIDA_RNG_SEED enabled (0x%llx)\n",
                     static_cast<unsigned long long>(seed));
    }
    return TRUE;
}

void ensure_initialized() { InitOnceExecuteOnce(&g_init_once, init_bcrypt_once, nullptr, nullptr); }

uint64_t next_splitmix64() {
    uint64_t z = (g_prng_state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

void fill_seeded(PUCHAR buffer, ULONG count) {
    AcquireSRWLockExclusive(&g_lock);
    ULONG offset = 0;
    while (offset < count) {
        const uint64_t word = next_splitmix64();
        const ULONG chunk = (count - offset) < sizeof(word) ? (count - offset) : static_cast<ULONG>(sizeof(word));
        std::memcpy(buffer + offset, &word, chunk);
        offset += chunk;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

bool is_rng_algorithm(LPCWSTR alg_id) {
    return alg_id && _wcsicmp(alg_id, BCRYPT_RNG_ALGORITHM) == 0;
}

// g_lock held (shared or exclusive).
bool lookup_live_handle(BCRYPT_ALG_HANDLE h, BCRYPT_ALG_HANDLE& out_host, bool& out_is_rng) {
    for (const AlgHandle* cur = g_live_handles; cur; cur = cur->next) {
        if (cur == h) {
            out_host = cur->host_handle;
            out_is_rng = cur->is_rng;
            return true;
        }
    }
    return false;
}

// Holds g_lock shared for as long as it lives: a handle found under it stays open (close_algorithm_provider unlinks
// and closes under the exclusive lock), and calls on other handles or other threads still run side by side.
struct SharedLock {
    SharedLock() { AcquireSRWLockShared(&g_lock); }
    ~SharedLock() { ReleaseSRWLockShared(&g_lock); }
    SharedLock(const SharedLock&) = delete;
    SharedLock& operator=(const SharedLock&) = delete;
};

// The host RNG on a handle (null: the system-preferred one), else SystemFunction036.
NTSTATUS host_random(BCRYPT_ALG_HANDLE host_handle, PUCHAR pbBuffer, ULONG cbBuffer, ULONG dwFlags) {
    if (!g_disable_host_rng_for_test.load(std::memory_order_acquire)) {
        if (g_host_gen) {
            const NTSTATUS st = g_host_gen(host_handle, pbBuffer, cbBuffer, dwFlags);
            if (st >= 0) return st;
        }
        if (g_rtl_gen_random && g_rtl_gen_random(pbBuffer, cbBuffer)) return kStatusSuccess;
    }
    return kStatusUnsuccessful;
}

NTSTATUS WINAPI open_algorithm_provider(BCRYPT_ALG_HANDLE* phAlgorithm, LPCWSTR pszAlgId, LPCWSTR pszImplementation,
                                        ULONG dwFlags) {
    ensure_initialized();
    if (!phAlgorithm || !pszAlgId) return kStatusInvalidParameter;
    *phAlgorithm = nullptr;

    const bool is_rng = is_rng_algorithm(pszAlgId);
    BCRYPT_ALG_HANDLE host_alg = nullptr;
    if (!(g_seeded_mode && is_rng)) {
        if (!g_host_open) {
            if (!is_rng) return kStatusNotFound;
        } else {
            const NTSTATUS st = g_host_open(&host_alg, pszAlgId, pszImplementation, dwFlags);
            if (st < 0) return st;
        }
    }

    auto* wrapper = new (std::nothrow) AlgHandle{host_alg, is_rng, nullptr};
    if (!wrapper) {
        if (host_alg && g_host_close) g_host_close(host_alg, 0);
        return kStatusNoMemory;
    }

    AcquireSRWLockExclusive(&g_lock);
    wrapper->next = g_live_handles;
    g_live_handles = wrapper;
    ReleaseSRWLockExclusive(&g_lock);

    *phAlgorithm = wrapper;
    return kStatusSuccess;
}

NTSTATUS WINAPI close_algorithm_provider(BCRYPT_ALG_HANDLE hAlgorithm, ULONG dwFlags) {
    ensure_initialized();
    if (!hAlgorithm) return kStatusInvalidHandle;

    // Unlinked and closed under the exclusive lock: it waits for a BCryptGenRandom still using the host handle (which
    // holds the lock shared for its whole call), and no later call can find the handle once it goes.
    AlgHandle* removed = nullptr;
    NTSTATUS st = kStatusSuccess;
    AcquireSRWLockExclusive(&g_lock);
    AlgHandle** cur = &g_live_handles;
    while (*cur) {
        if (*cur == hAlgorithm) {
            removed = *cur;
            *cur = removed->next;
            break;
        }
        cur = &((*cur)->next);
    }
    if (removed && removed->host_handle && g_host_close) st = g_host_close(removed->host_handle, dwFlags);
    ReleaseSRWLockExclusive(&g_lock);
    if (!removed) return kStatusInvalidHandle;
    delete removed;
    return st;
}

NTSTATUS WINAPI gen_random(BCRYPT_ALG_HANDLE hAlgorithm, PUCHAR pbBuffer, ULONG cbBuffer, ULONG dwFlags) {
    ensure_initialized();
    if (!pbBuffer) return kStatusInvalidParameter;

    const bool use_system = (dwFlags & BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0;
    if (use_system) {
        if (hAlgorithm != nullptr) return kStatusInvalidParameter;
        if (cbBuffer == 0) return kStatusSuccess;
        if (g_seeded_mode) {
            fill_seeded(pbBuffer, cbBuffer);
            return kStatusSuccess;
        }
        return host_random(nullptr, pbBuffer, cbBuffer, dwFlags);
    }

    if (!hAlgorithm) return kStatusInvalidHandle;
    {
        // The host handle is used under the shared lock it was found under, so a concurrent
        // BCryptCloseAlgorithmProvider cannot close it mid-call.
        SharedLock lock;
        BCRYPT_ALG_HANDLE host_handle = nullptr;
        bool is_rng = false;
        if (!lookup_live_handle(hAlgorithm, host_handle, is_rng) || !is_rng) return kStatusInvalidHandle;
        if (cbBuffer == 0) return kStatusSuccess;
        if (!g_seeded_mode) return host_random(host_handle, pbBuffer, cbBuffer, dwFlags);
    }
    fill_seeded(pbBuffer, cbBuffer);  // takes g_lock exclusive: not under the shared hold
    return kStatusSuccess;
}

}  // namespace

void set_host_rng_enabled_for_test(bool enabled) {
    g_disable_host_rng_for_test.store(!enabled, std::memory_order_release);
}

// Replaces the host bcrypt.dll functions (tests), and returns the previous ones through the same arguments. Call with
// no handle open.
void swap_host_functions_for_test(void** open, void** close, void** gen) {
    ensure_initialized();
    AcquireSRWLockExclusive(&g_lock);
    auto* o = reinterpret_cast<void*>(g_host_open);
    auto* c = reinterpret_cast<void*>(g_host_close);
    auto* g = reinterpret_cast<void*>(g_host_gen);
    g_host_open = reinterpret_cast<OpenAlgFn>(*open);
    g_host_close = reinterpret_cast<CloseAlgFn>(*close);
    g_host_gen = reinterpret_cast<GenRandomFn>(*gen);
    *open = o;
    *close = c;
    *gen = g;
    ReleaseSRWLockExclusive(&g_lock);
}
}  // namespace rt::bcrypt_rng

namespace {
using namespace rt::bcrypt_rng;
}  // namespace

SHIM_TABLE(bcrypt_rng,
    {"bcrypt.dll", "BCryptOpenAlgorithmProvider", reinterpret_cast<void*>(&open_algorithm_provider)},
    {"bcrypt.dll", "BCryptGenRandom", reinterpret_cast<void*>(&gen_random)},
    {"bcrypt.dll", "BCryptCloseAlgorithmProvider", reinterpret_cast<void*>(&close_algorithm_provider)},
);
