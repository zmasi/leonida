// DirectStorage shim (runtime/shims/dstorage.cpp): DStorageGetFactory resolved through the shim registry under its
// three module names, driven the way a console title calls it, over synthetic files in %TEMP%. No game data, no GPU.
#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <span>
#include "../src/bcpack.h"
#include "../src/bcpack_stream.h"
#include "../src/dstorage_hold.h"
#include "../src/title_geometry_watch.h"
#include "../src/title_page_watch.h"
#include "xmem.h"
#include "dstorage_xs_abi.h"
#include "dstorage_staging.h"
#include "file_system.h"
#include "shims.h"

using namespace dsx;
namespace fs = std::filesystem;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

void check_hr(HRESULT got, HRESULT want, const char* what) {
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::printf("FAIL: %s (got 0x%08lx, want 0x%08lx)\n", what, static_cast<unsigned long>(got),
                    static_cast<unsigned long>(want));
    }
}

// Interface ids a title might pass. The console's own ids are not public, so these are made up; the shim accepts any
// id per kind of object.
constexpr GUID kFactoryId = {0x0d5e0001, 0x1111, 0x4111, {0x81, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x01}};
constexpr GUID kQueueId = {0x0d5e0002, 0x1111, 0x4111, {0x81, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x02}};
constexpr GUID kFileId = {0x0d5e0003, 0x1111, 0x4111, {0x81, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x03}};
constexpr GUID kStatusId = {0x0d5e0004, 0x1111, 0x4111, {0x81, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x04}};
// IDStorageFactory of the PC API (dstorage.h): refused, its object has another shape.
constexpr GUID kPcFactoryId = {0x6924ea0c, 0xc3cd, 0x4826, {0xb1, 0x0a, 0xf6, 0x4f, 0x4e, 0xd9, 0x27, 0xc1}};

// A CPU fence with the PC ID3D12Fence vtable (the one the d3d12_xs fence proxies forward). It records the values it is
// signalled with and runs a check at each signal.
class TestFence final : public ID3D12Fence {
public:
    std::function<void(UINT64)> on_signal;
    std::atomic<UINT64> value{0};
    std::atomic<ULONG> refs{1};

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid != __uuidof(IUnknown) && riid != __uuidof(ID3D12Fence)) return E_NOINTERFACE;
        AddRef();
        *ppv = this;
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }  // lives on the test's stack
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void** ppv) override {
        if (ppv) *ppv = nullptr;
        return E_NOTIMPL;
    }
    UINT64 STDMETHODCALLTYPE GetCompletedValue() override { return value.load(); }
    HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64, HANDLE) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Signal(UINT64 v) override {
        if (on_signal) on_signal(v);
        value.store(v);
        return S_OK;
    }
};

std::vector<uint8_t> pattern(size_t n, uint32_t seed) {
    std::vector<uint8_t> v(n);
    uint32_t x = seed * 2654435761u + 1;
    for (auto& b : v) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        b = static_cast<uint8_t>(x);
    }
    return v;
}

void write_file(const fs::path& p, const std::vector<uint8_t>& bytes) {
    fs::create_directories(p.parent_path());
    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"wb") != 0 || !f) {
        std::printf("FAIL: cannot write %ls\n", p.c_str());
        std::exit(1);
    }
    if (!bytes.empty()) std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
}

bool wait_complete(IDStorageStatusArrayX* s, UINT32 index, int ms = 10000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!s->IsComplete(index)) {
        if (std::chrono::steady_clock::now() > end) return false;
        Sleep(1);
    }
    return true;
}

bool wait_until(const std::function<bool()>& done, int ms = 10000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!done()) {
        if (std::chrono::steady_clock::now() > end) return false;
        Sleep(1);
    }
    return true;
}

DSTORAGE_REQUEST file_request(IDStorageFileX* f, uint64_t offset, void* dst, uint32_t n, uint64_t tag = 0,
                              const char* name = nullptr) {
    DSTORAGE_REQUEST r{};
    r.File = f;
    r.FileOffset = offset;
    r.Destination = dst;
    r.DestinationSize = n;
    r.SourceSize = n;
    r.CancellationTag = tag;
    r.Name = name;
    return r;
}

struct Env {
    fs::path root, game, overlay;
    IDStorageFactoryX2* factory = nullptr;
    std::vector<uint8_t> a_package, a_overlay, b, big;
};

IDStorageQueueX2* make_queue(Env& env, uint16_t capacity = 0x80, bool auto_submit = true,
                             DSTORAGE_REQUEST_SOURCE_TYPE type = DSTORAGE_REQUEST_SOURCE_FILE,
                             DSTORAGE_PRIORITY priority = DSTORAGE_PRIORITY_NORMAL, const char* name = "test queue") {
    DSTORAGE_QUEUE_DESC1 d{};
    d.SourceType = type;
    d.Capacity = capacity;
    d.Priority = priority;
    d.Name = name;
    d.Options.AutoSubmitDisabled = auto_submit ? 0 : 1;
    void* q = nullptr;
    const HRESULT hr = env.factory->CreateQueue1(&d, kQueueId, &q);
    check_hr(hr, S_OK, "CreateQueue1");
    return static_cast<IDStorageQueueX2*>(q);
}

IDStorageFileX* open(Env& env, const wchar_t* path) {
    void* f = nullptr;
    check_hr(env.factory->OpenFile(path, kFileId, &f), S_OK, "OpenFile");
    return static_cast<IDStorageFileX*>(f);
}

IDStorageStatusArrayX* make_status(Env& env, UINT32 capacity) {
    void* s = nullptr;
    check_hr(env.factory->CreateStatusArray(capacity, "test status", kStatusId, &s), S_OK, "CreateStatusArray");
    return static_cast<IDStorageStatusArrayX*>(s);
}

// ---- tests ----------------------------------------------------------------------------------------------------------

void test_staging_budget() {
    using leonida::dstorage::StagingPool;
    using leonida::dstorage::staging_bytes;
    DSTORAGE_REQUEST r{};
    r.SourceSize = 64;
    r.DestinationSize = 64;
    check(staging_bytes(r) == 0, "raw buffer copy needs no staging");
    r.Options.AsUINT64 = options::kSourceMemory | options::kSourcePages;
    r.SourcePageOffset = 65535;
    check(staging_bytes(r) == 80, "two source page numbers are charged with the payload");
    r.Options.AsUINT64 = options::kZlib | (7ull << options::kBcpackShift);
    r.IntermediateSize = 512;
    r.DestinationSize = 256;
    check(staging_bytes(r) == 768, "combined transform charges largest simultaneously live pair");
    r.Options.AsUINT64 |= 1ull << options::kSwizzleShift;
    r.DestinationSize = 1024;
    check(staging_bytes(r) == 2048, "unshuffle holds both complete block images");

    StagingPool pool;
    constexpr uint64_t limit = StagingPool::limit;
    {
        StagingPool::Lease publisher(pool), helper(pool), refused(pool);
        publisher.acquire(limit - 100);
        check(!refused.try_pair(101), "helper falls back when either member of pair cannot fit");
        check(helper.try_pair(100), "helper and publisher reserve the exact remaining budget");
        check(!refused.try_pair(1), "full pool prevents another helper allocation");
    }
    {
        StagingPool::Lease a(pool), b(pool), c(pool);
        check(a.try_pair(1) && b.try_pair(1) && !c.try_pair(1), "at most two helpers hold buffers");
    }
    std::atomic<bool> waiting{}, entered{};
    std::thread other;
    {
        StagingPool::Lease large(pool), concurrent(pool);
        large.acquire(limit + 1);
        check(!concurrent.try_pair(1), "oversize request holds the pool exclusively");
        other = std::thread([&] {
            StagingPool::Lease next(pool);
            waiting = true;
            next.acquire(limit);
            entered = true;
        });
        while (!waiting) std::this_thread::yield();
        check(!entered, "ordinary publisher waits while exclusive request holds buffers");
    }
    other.join();
    check(entered, "waiting publisher wakes when exclusive buffers are released");
    StagingPool::Lease reclaimed(pool);
    check(reclaimed.try_pair(limit), "pool is entirely reusable after the last publisher");
    check(!reclaimed.try_pair(1), "a held lease refuses duplicate try_pair without leaking helper count");
    reclaimed.acquire(0);  // releases prior try_pair cleanly
    check(reclaimed.try_pair(limit), "acquire(0) released prior reservation back to pool");
    reclaimed.acquire(limit / 2);  // re-acquire replaces prior reservation without leaking used_
    {
        StagingPool::Lease companion(pool);
        check(companion.try_pair(limit / 2), "re-acquired lease kept exact remaining pool budget");
    }
    reclaimed.release();
    check(reclaimed.try_pair(limit), "explicit release returns full pool budget");

    // Overflow-safe staging_bytes saturation for huge page-backed or combined transform sizes.
    DSTORAGE_REQUEST big_pages{};
    big_pages.Options.AsUINT64 = options::kSourceMemory | options::kSourcePages;
    big_pages.SourceSize = UINT32_MAX;
    big_pages.SourcePageOffset = 65535;
    check(staging_bytes(big_pages) > big_pages.SourceSize, "large page-backed source does not wrap");

    DSTORAGE_REQUEST big_pair{};
    big_pair.Options.AsUINT64 = options::kZlib;
    big_pair.SourceSize = UINT32_MAX;
    big_pair.DestinationSize = UINT32_MAX;
    check(staging_bytes(big_pair) == uint64_t(UINT32_MAX) * 2, "large transform sum stays in 64-bit range");
}

void test_layout_and_options() {
    DSTORAGE_REQUEST_OPTIONS o{};
    o.ZlibDecompress = 1;
    check(o.AsUINT64 == options::kZlib, "ZlibDecompress is bit 0");
    o.AsUINT64 = 0;
    o.BcpackMode = DSTORAGE_BCPACK_MODE_BC7;
    check(options::bcpack(o.AsUINT64) == 7 && o.AsUINT64 == (7ull << options::kBcpackShift), "BcpackMode is bits 1-4");
    o.AsUINT64 = 0;
    o.SwizzleMode = DSTORAGE_SWIZZLE_MODE_4K_12K_4K_12K;
    check(options::swizzle(o.AsUINT64) == 4 && o.AsUINT64 == (4ull << options::kSwizzleShift),
          "SwizzleMode is bits 5-20");
    o.AsUINT64 = 0;
    o.DestinationIsPhysicalPages = 1;
    check(o.AsUINT64 == options::kDestinationPages, "DestinationIsPhysicalPages is bit 21");
    o.AsUINT64 = 0;
    o.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
    check(o.AsUINT64 == options::kSourceMemory, "SourceType is bit 22");
    o.AsUINT64 = 0;
    o.SourceIsPhysicalPages = 1;
    check(o.AsUINT64 == options::kSourcePages, "SourceIsPhysicalPages is bit 23");
    o.AsUINT64 = 0;
    o.Reserved = 1;
    check(o.AsUINT64 == (1ull << 24) && (o.AsUINT64 & options::kReservedMask), "Reserved starts at bit 24");
    DSTORAGE_QUEUE_OPTIONS q{};
    q.AutoSubmitDisabled = 1;
    check(q.AsUINT64 == 1, "AutoSubmitDisabled is bit 0");
}

void test_registry(Env& env) {
    const char* names[] = {"dstorage_xs.dll", "xcrdapi.dll", "dstorage.dll"};
    void* first = rt::shims::find("dstorage_xs.dll", "DStorageGetFactory");
    check(first != nullptr, "dstorage_xs.dll!DStorageGetFactory is registered");
    for (const char* n : names) {
        check(rt::shims::find(n, "DStorageGetFactory") == first, "every module name serves the same function");
        check(rt::shims::owns_dll(n), "the shims own the module (LoadLibrary gets a pseudo-handle)");
    }
    auto get = reinterpret_cast<DStorageGetFactoryFn>(first);
    if (!get) std::exit(1);

    check_hr(get(kFactoryId, nullptr), E_POINTER, "DStorageGetFactory without ppv");
    void* pc = reinterpret_cast<void*>(1);
    check_hr(get(kPcFactoryId, &pc), E_NOINTERFACE, "the PC IDStorageFactory id is refused");
    check(pc == nullptr, "a refused factory clears ppv");

    void* f1 = nullptr;
    void* f2 = nullptr;
    check_hr(get(kFactoryId, &f1), S_OK, "DStorageGetFactory");
    check_hr(reinterpret_cast<DStorageGetFactoryFn>(rt::shims::find("xcrdapi.dll", "DStorageGetFactory"))(
                 __uuidof(IUnknown), &f2),
             S_OK, "DStorageGetFactory(IUnknown) under xcrdapi.dll");
    check(f1 && f1 == f2, "the factory is one object");
    env.factory = static_cast<IDStorageFactoryX2*>(f1);
    static_cast<IUnknown*>(f2)->Release();
    check(env.factory->Release() >= 1 && env.factory->Release() >= 1, "the factory's count does not drop below one");
    env.factory->AddRef();

    void* again = nullptr;
    check_hr(env.factory->QueryInterface(kFactoryId, &again), S_OK, "factory QueryInterface for its own id");
    if (again) static_cast<IUnknown*>(again)->Release();
}

void test_factory_checks(Env& env) {
    // Before any queue or file exists.
    check_hr(env.factory->SetStagingBufferSize(17u << 20), E_DSTORAGE_INVALID_STAGING_BUFFER_SIZE, "staging 17 MiB");
    check_hr(env.factory->SetStagingBufferSize(16u << 20), S_OK, "staging 16 MiB");
    check_hr(env.factory->SetStagingBufferSize(0), S_OK, "staging 0");
    check_hr(env.factory->SetCpuAffinity(0x3c), S_OK, "SetCpuAffinity");
    env.factory->SetDebugFlags(DSTORAGE_DEBUG_SHOW_ERRORS);

    auto create = [&](DSTORAGE_REQUEST_SOURCE_TYPE type, uint16_t cap, int prio, uint64_t opts = 0) {
        DSTORAGE_QUEUE_DESC1 d{};
        d.SourceType = type;
        d.Capacity = cap;
        d.Priority = static_cast<DSTORAGE_PRIORITY>(prio);
        d.Options.AsUINT64 = opts;
        void* q = reinterpret_cast<void*>(1);
        const HRESULT hr = env.factory->CreateQueue1(&d, kQueueId, &q);
        if (SUCCEEDED(hr)) static_cast<IUnknown*>(q)->Release();
        else check(q == nullptr, "a failed CreateQueue1 clears ppv");
        return hr;
    };
    check_hr(create(DSTORAGE_REQUEST_SOURCE_FILE, 0x7f, 0), E_DSTORAGE_INVALID_QUEUE_CAPACITY, "capacity 0x7f");
    check_hr(create(DSTORAGE_REQUEST_SOURCE_FILE, 0x2001, 0), E_DSTORAGE_INVALID_QUEUE_CAPACITY, "capacity 0x2001");
    check_hr(create(DSTORAGE_REQUEST_SOURCE_FILE, 0x80, 3), E_DSTORAGE_INVALID_QUEUE_PRIORITY, "priority 3");
    check_hr(create(DSTORAGE_REQUEST_SOURCE_FILE, 0x80, -2), E_DSTORAGE_INVALID_QUEUE_PRIORITY, "priority -2");
    check_hr(create(DSTORAGE_REQUEST_SOURCE_MEMORY, 0x80, 0), E_DSTORAGE_INVALID_MEMORY_QUEUE_PRIORITY,
             "a memory queue must be realtime");
    check_hr(create(static_cast<DSTORAGE_REQUEST_SOURCE_TYPE>(2), 0x80, 0), E_DSTORAGE_INVALID_SOURCE_TYPE,
             "source type 2");
    check_hr(create(DSTORAGE_REQUEST_SOURCE_FILE, 0x80, 0, 2), E_DSTORAGE_RESERVED_FIELDS, "queue option bit 1");
    check_hr(create(DSTORAGE_REQUEST_SOURCE_FILE, 0x2000, -1), S_OK, "capacity 0x2000, low priority");
    check_hr(create(DSTORAGE_REQUEST_SOURCE_MEMORY, 0x80, 2), S_OK, "a realtime memory queue");
    check_hr(env.factory->CreateQueue1(nullptr, kQueueId, nullptr), E_POINTER, "CreateQueue1 without ppv");
    void* q = nullptr;
    check_hr(env.factory->CreateQueue(nullptr, kQueueId, &q), E_INVALIDARG, "CreateQueue without a desc");
    check_hr(env.factory->CreateQueue1(nullptr, kQueueId, &q), E_INVALIDARG, "CreateQueue1 without a desc");
    void* s = nullptr;
    check_hr(env.factory->CreateStatusArray(0, nullptr, kStatusId, &s), E_INVALIDARG, "a status array of 0");

    // The id policy: an id seen for a queue is not a factory id.
    void* wrong = nullptr;
    check_hr(env.factory->QueryInterface(kQueueId, &wrong), E_NOINTERFACE, "a queue id is not a factory id");
    check_hr(env.factory->CreateStatusArray(4, nullptr, kFactoryId, &s), E_NOINTERFACE,
             "a factory id is not a status array id");

    // CreateQueue (DSTORAGE_QUEUE_DESC) and Query reflect the desc.
    DSTORAGE_QUEUE_DESC d{};
    d.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    d.Capacity = 0x100;
    d.Priority = DSTORAGE_PRIORITY_HIGH;
    d.Name = "named";
    check_hr(env.factory->CreateQueue(&d, kQueueId, &q), S_OK, "CreateQueue");
    auto* queue = static_cast<IDStorageQueueX2*>(q);
    DSTORAGE_QUEUE_INFO info{};
    queue->Query(&info);
    check(info.Desc.Capacity == 0x100 && info.Desc.Priority == DSTORAGE_PRIORITY_HIGH && info.Desc.Name == d.Name &&
              info.Desc.SourceType == DSTORAGE_REQUEST_SOURCE_FILE,
          "Query returns the creation desc");
    check(info.EmptySlotCount == 0xff, "an empty queue has capacity - 1 empty slots");
    check(info.RequestCountUntilAutoSubmit == 0x80, "auto-submit at half the capacity");
    DSTORAGE_QUEUE_INFO1 info1{};
    queue->Query1(&info1);
    check(info1.Desc.Options.AsUINT64 == 0 && info1.EmptySlotCount == 0xff, "Query1");
    check_hr(env.factory->SetStagingBufferSize(32u << 20), E_DSTORAGE_STAGING_BUFFER_LOCKED,
             "staging size is locked while a queue exists");
    void* qi = nullptr;
    check_hr(queue->QueryInterface(kQueueId, &qi), S_OK, "queue QueryInterface");
    if (qi) static_cast<IUnknown*>(qi)->Release();
    check(queue->Release() == 0, "the last queue reference frees it");
    check_hr(env.factory->SetStagingBufferSize(32u << 20), S_OK, "staging size unlocked again");
}

void test_open_paths(Env& env) {
    void* f = reinterpret_cast<void*>(1);
    check_hr(env.factory->OpenFile(L"GAME:\\data\\missing.bin", kFileId, &f), HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND),
             "a missing GAME: file");
    check(f == nullptr, "a failed OpenFile clears ppv");
    check_hr(env.factory->OpenFile(L"GAME:\\..\\outside.bin", kFileId, &f), HRESULT_FROM_WIN32(ERROR_INVALID_NAME),
             "a GAME: path that climbs out is refused");
    check_hr(env.factory->OpenFile(nullptr, kFileId, &f), E_INVALIDARG, "OpenFile without a path");
    check_hr(env.factory->OpenFile(L"GAME:\\data\\a.bin", kFileId, nullptr), E_POINTER, "OpenFile without ppv");

    // The overlay copy wins over the package; a host path is passed through.
    IDStorageFileX* a = open(env, L"GAME:\\data\\a.bin");
    IDStorageFileX* b = open(env, L"game:/data/b.bin");
    const std::wstring host = (env.game / L"data" / L"b.bin").wstring();
    IDStorageFileX* h = open(env, host.c_str());
    if (!a || !b || !h) std::exit(1);

    IDStorageQueueX2* q = make_queue(env);
    IDStorageStatusArrayX* s = make_status(env, 4);
    std::vector<uint8_t> da(4096), db(1000), dh(1000);
    DSTORAGE_REQUEST r = file_request(a, 0, da.data(), 4096);
    q->EnqueueRequest(&r);
    r = file_request(b, 500, db.data(), 1000);
    q->EnqueueRequest(&r);
    r = file_request(h, 500, dh.data(), 1000);
    q->EnqueueRequest(&r);
    q->EnqueueStatus(s, 0);
    check_hr(s->GetHResult(0), E_PENDING, "a status entry is pending once enqueued");
    q->Submit();
    check(wait_complete(s, 0), "the reads complete");
    check_hr(s->GetHResult(0), S_OK, "the reads succeed");
    check(std::memcmp(da.data(), env.a_overlay.data(), 4096) == 0 && da != env.a_package,
          "GAME: reads the overlay copy, not the package");

    // GetHandle: a handle of its own (ReOpenFile), so its file position does not follow the shim's reads, which
    // just ended at byte 4096 of this file.
    HANDLE own = a->GetHandle();
    LARGE_INTEGER size{};
    check(own != INVALID_HANDLE_VALUE && GetFileSizeEx(own, &size) && size.QuadPart == 5000,
          "GetHandle names the overlay copy (5000 bytes, the package has 4096)");
    std::vector<uint8_t> via_handle(6000);
    DWORD got = 0;
    check(own != INVALID_HANDLE_VALUE && ReadFile(own, via_handle.data(), 6000, &got, nullptr) && got == 5000 &&
              std::memcmp(via_handle.data(), env.a_overlay.data(), 5000) == 0,
          "a ReadFile through GetHandle's handle starts at byte 0 and reads the overlay copy");
    if (own != INVALID_HANDLE_VALUE) CloseHandle(own);
    r = file_request(a, 100, da.data(), 50);
    q->EnqueueRequest(&r);
    q->EnqueueStatus(s, 1);
    q->Submit();
    check(wait_complete(s, 1) && std::memcmp(da.data(), env.a_overlay.data() + 100, 50) == 0,
          "the shim's reads do not follow a GetHandle handle's position either");
    check(std::memcmp(db.data(), env.b.data() + 500, 1000) == 0,
          "a GAME: read at an offset (forward slashes, any case)");
    check(db == dh, "a host path reads the same file");
    a->Release();
    b->Release();
    h->Release();
    s->Release();
    q->Release();
}

// Simulate a committed page made read-only by the runtime's D3D shadow tracker.
void test_shadowed_file_destination(Env& env) {
    constexpr DWORD size = 4096;
    void* page = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READONLY);
    check(page != nullptr, "allocate a watched file destination");
    if (!page) return;
    auto* file = open(env, L"game:/data/b.bin");
    auto* queue = make_queue(env);
    auto* status = make_status(env, 1);
    leonida::kernel_writes::set_opener([](uintptr_t begin, uintptr_t end) {
        DWORD old = 0;
        VirtualProtect(reinterpret_cast<void*>(begin), end - begin, PAGE_READWRITE, &old);
    });
    auto request = file_request(file, 129, page, size);
    queue->EnqueueRequest(&request);
    queue->EnqueueStatus(status, 0);
    queue->Submit();
    queue->Close();
    leonida::kernel_writes::set_opener(nullptr);
    check_hr(status->GetHResult(0), S_OK, "file read to a watched destination");
    check(std::memcmp(page, env.b.data() + 129, size) == 0, "watched destination received the file range");
    status->Release();
    queue->Release();
    file->Release();
    VirtualFree(page, 0, MEM_RELEASE);
}

void test_reads_and_errors(Env& env) {
    IDStorageFileX* b = open(env, L"GAME:\\data\\b.bin");  // 10,000 bytes
    IDStorageFileX* big = open(env, L"GAME:\\data\\big.bin");
    IDStorageQueueX2* q = make_queue(env, 0x80, false);
    IDStorageStatusArrayX* s = make_status(env, 32);
    HANDLE error_event = q->GetErrorEvent();
    check(error_event != nullptr, "GetErrorEvent returns a handle");

    std::vector<uint8_t> big_out(env.big.size());
    std::vector<uint8_t> few(64, 0xAB);
    std::vector<uint8_t> zero(16, 0xCD);
    struct Case {
        DSTORAGE_REQUEST r;
        HRESULT want;
        const char* what;
    };
    std::vector<Case> cases;
    cases.push_back({file_request(big, 0, big_out.data(), static_cast<uint32_t>(big_out.size())), S_OK,
                     "a 3 MiB read"});
    cases.push_back({file_request(b, 10000, zero.data(), 0), S_OK, "a zero-size read at the end of the file"});
    cases.push_back({file_request(b, 9990, few.data(), 11, 0x77, "past-the-end"), E_DSTORAGE_END_OF_FILE,
                     "a read one byte past the end"});
    cases.push_back({file_request(b, ~0ull - 4, few.data(), 8), E_DSTORAGE_END_OF_FILE, "an offset near 2^64"});
    cases.push_back({file_request(b, 0, few.data(), 8), S_OK, "a good read after failures"});
    {
        DSTORAGE_REQUEST r = file_request(b, 0, few.data(), 8);
        r.DestinationSize = 16;
        cases.push_back({r, E_DSTORAGE_INVALID_DESTINATION_SIZE, "DestinationSize differs from SourceSize"});
        r = file_request(b, 0, few.data(), 8);
        r.Options.ZlibDecompress = 1;
        cases.push_back({r, E_DSTORAGE_INVALID_DESTINATION_SIZE, "zlib with DestinationSize equal to SourceSize"});
        r = file_request(b, 0, few.data(), 8);
        r.Options.BcpackMode = DSTORAGE_BCPACK_MODE_BC1;
        cases.push_back({r, E_DSTORAGE_INVALID_DESTINATION_SIZE, "BCPack needs a larger output"});
        r = file_request(b, 0, few.data(), 8);
        r.Options.SwizzleMode = DSTORAGE_SWIZZLE_MODE_4K_4K_8K;
        cases.push_back({r, E_DSTORAGE_INVALID_DESTINATION_SIZE, "partial swizzle group"});
        r = file_request(b, 0, few.data(), 8);
        r.Options.AsUINT64 = 8ull << options::kBcpackShift;
        cases.push_back({r, E_DSTORAGE_INVALID_BCPACK_MODE, "BCPack mode 8"});
        r = file_request(b, 0, few.data(), 8);
        r.Options.AsUINT64 = 5ull << options::kSwizzleShift;
        cases.push_back({r, E_DSTORAGE_INVALID_SWIZZLE_MODE, "swizzle mode 5"});
        r = file_request(b, 0, few.data(), 8);
        r.Options.Reserved = 1;
        cases.push_back({r, E_DSTORAGE_RESERVED_FIELDS, "a reserved option bit"});
        r = file_request(b, 0, few.data(), 8);
        r.Options.DestinationIsPhysicalPages = 1;
        cases.push_back({r, E_INVALIDARG, "unknown physical page"});
        r = file_request(b, 0, few.data(), 8);
        r.IntermediateSize = 4;
        cases.push_back({r, E_DSTORAGE_INVALID_INTERMEDIATE_SIZE, "IntermediateSize without zlib and BCPack"});
        r = file_request(b, 0, nullptr, 8);
        cases.push_back({r, E_INVALIDARG, "a null destination"});
        r = file_request(nullptr, 0, few.data(), 8);
        cases.push_back({r, E_INVALIDARG, "a null file"});
        r = file_request(reinterpret_cast<IDStorageFileX*>(s), 0, few.data(), 8);
        cases.push_back({r, E_DSTORAGE_INVALID_FILE_HANDLE, "a status array passed as a file"});
        r = file_request(b, 0, few.data(), 8);
        r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        cases.push_back({r, E_DSTORAGE_INVALID_SOURCE_TYPE, "a memory request on a file queue"});
    }
    cases.push_back({file_request(b, 0, nullptr, 0), E_INVALIDARG, "a zero-size read still needs a destination"});
    cases.push_back({file_request(nullptr, 0, zero.data(), 0), E_INVALIDARG, "a zero-size read still needs a file"});
    void* reserved = VirtualAlloc(nullptr, 1 << 16, MEM_RESERVE, PAGE_READWRITE);
    cases.push_back({file_request(b, 0, reserved, 4096), E_DSTORAGE_ACCESS_VIOLATION,
                     "a destination the process cannot write"});
    check(cases.size() <= 31, "the cases fit the status array");

    for (UINT32 i = 0; i < cases.size(); ++i) {
        q->EnqueueRequest(&cases[i].r);
        q->EnqueueStatus(s, i);
    }
    Sleep(30);
    check(!s->IsComplete(0), "nothing runs before Submit when auto-submit is disabled");
    DSTORAGE_QUEUE_INFO1 info{};
    q->Query1(&info);
    check(info.RequestCountUntilAutoSubmit == 0, "no auto-submit count when it is disabled");
    check(info.EmptySlotCount == 0x7f - 2 * cases.size(), "EmptySlotCount counts every entry");
    q->Submit();
    const UINT32 last = static_cast<UINT32>(cases.size() - 1);
    check(wait_complete(s, last), "all cases complete");
    for (UINT32 i = 0; i < cases.size(); ++i) check_hr(s->GetHResult(i), cases[i].want, cases[i].what);
    check(big_out == env.big, "the 3 MiB read has the file's bytes");
    check(zero == std::vector<uint8_t>(16, 0xCD), "a zero-size read writes nothing");
    check(std::memcmp(few.data(), env.b.data(), 8) == 0, "the good read after failures");

    check(WaitForSingleObject(error_event, 0) == WAIT_OBJECT_0, "the error event is set");
    DSTORAGE_ERROR_RECORD rec{};
    q->RetrieveErrorRecord(&rec);
    size_t failures = 0;
    for (const auto& c : cases) failures += FAILED(c.want) ? 1 : 0;
    check(rec.FailureCount == failures, "FailureCount counts every failed request");
    check_hr(rec.FirstFailure.HResult, E_DSTORAGE_END_OF_FILE, "the first failure is recorded");
    check(rec.FirstFailure.CommandType == DSTORAGE_COMMAND_TYPE_REQUEST, "its command type is REQUEST");
    check(rec.FirstFailure.Request.FileOffset == 9990 && rec.FirstFailure.Request.SourceSize == 11 &&
              rec.FirstFailure.Request.CancellationTag == 0x77,
          "the record has the request's offset, size and tag");
    check(std::wcscmp(rec.FirstFailure.Request.Filename, L"GAME:\\data\\b.bin") == 0,
          "the record names the file as the title opened it");
    check(std::strcmp(rec.FirstFailure.Request.Name, "past-the-end") == 0, "the record has the request's name");
    check(WaitForSingleObject(error_event, 0) == WAIT_TIMEOUT, "RetrieveErrorRecord resets the event");
    q->RetrieveErrorRecord(&rec);
    check(rec.FailureCount == 0 && rec.FirstFailure.CommandType == DSTORAGE_COMMAND_TYPE_NONE,
          "a second RetrieveErrorRecord has nothing");

    // Status and fence entries that fail.
    q->EnqueueStatus(s, 1000);
    q->EnqueueStatus(nullptr, 0);
    q->EnqueueSignal(nullptr, 1);
    q->EnqueueStatus(s, 31);
    q->Submit();
    check(wait_complete(s, 31), "the entries after the failed notifications complete");
    check_hr(s->GetHResult(31), S_OK, "notification failures do not fail the next status entry");
    check_hr(s->GetHResult(1000), E_DSTORAGE_INDEX_BOUND, "GetHResult past the array");
    check(s->IsComplete(1000), "IsComplete past the array is not pending");
    q->RetrieveErrorRecord(&rec);
    check(rec.FailureCount == 3 && rec.FirstFailure.CommandType == DSTORAGE_COMMAND_TYPE_STATUS &&
              rec.FirstFailure.HResult == E_DSTORAGE_INDEX_BOUND && rec.FirstFailure.Status.Index == 1000 &&
              rec.FirstFailure.Status.StatusArray == s,
          "an index past the array is recorded as a STATUS failure");

    VirtualFree(reserved, 0, MEM_RELEASE);
    CloseHandle(error_event);
    s->Release();
    q->Release();
    b->Release();
    big->Release();
}

void test_fence_order_and_events(Env& env) {
    IDStorageFileX* b = open(env, L"GAME:\\data\\b.bin");
    IDStorageQueueX2* q = make_queue(env, 0x200);
    TestFence fence;
    constexpr int kReads = 100;
    std::vector<std::vector<uint8_t>> outs(kReads, std::vector<uint8_t>(100));
    std::atomic<bool> in_place{false};
    fence.on_signal = [&](UINT64 v) {
        bool ok = v == 7;
        for (int i = 0; i < kReads; ++i) ok = ok && std::memcmp(outs[i].data(), env.b.data() + i * 100, 100) == 0;
        in_place.store(ok);
    };
    for (int i = 0; i < kReads; ++i) {
        DSTORAGE_REQUEST r = file_request(b, static_cast<uint64_t>(i) * 100, outs[i].data(), 100);
        q->EnqueueRequest(&r);
    }
    q->EnqueueSignal(&fence, 7);
    check(fence.refs.load() == 2, "an enqueued signal holds a fence reference");
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    q->EnqueueSetEvent(ev);
    q->Submit();
    check(WaitForSingleObject(ev, 10000) == WAIT_OBJECT_0, "EnqueueSetEvent sets the event");
    check(fence.GetCompletedValue() == 7, "EnqueueSignal signals the fence with its value");
    check(in_place.load(), "the fence is signalled after every read before it is in place");
    check(wait_until([&] { return fence.refs.load() == 1; }), "the fence reference is released after the signal");

    // A failed request before a fence: the fence still signals, and the status after the fence is clean.
    IDStorageStatusArrayX* s = make_status(env, 2);
    std::vector<uint8_t> tmp(8);
    DSTORAGE_REQUEST bad = file_request(b, 20000, tmp.data(), 8);
    q->EnqueueRequest(&bad);
    q->EnqueueSignal(&fence, 8);
    q->EnqueueStatus(s, 0);
    q->Submit();
    check(wait_complete(s, 0), "the status after a fence completes");
    check_hr(s->GetHResult(0), S_OK, "a failure before a fence is not reported by a status after it");
    check(fence.GetCompletedValue() == 8, "a fence after a failed read still signals");
    CloseHandle(ev);
    s->Release();
    q->Release();
    b->Release();
}

void test_cancel(Env& env) {
    IDStorageFileX* b = open(env, L"GAME:\\data\\b.bin");
    IDStorageQueueX2* q = make_queue(env, 0x80, false);
    IDStorageStatusArrayX* s = make_status(env, 1);
    std::vector<std::vector<uint8_t>> outs(6, std::vector<uint8_t>(16, 0));
    const uint64_t tags[] = {0x11, 0x12, 0x21, 0x22, 0x31};
    for (int i = 0; i < 5; ++i) {
        DSTORAGE_REQUEST r = file_request(b, 0, outs[i].data(), 16, tags[i]);
        q->EnqueueRequest(&r);
    }
    q->CancelRequestsWithTag(0xF0, 0x10);  // 0x11 and 0x12
    q->CancelRequestsWithTag(0xFF, 0x22);  // 0x22
    DSTORAGE_REQUEST later = file_request(b, 0, outs[5].data(), 16, 0x11);  // enqueued after: not cancelled
    q->EnqueueRequest(&later);
    q->EnqueueStatus(s, 0);
    q->Submit();
    check(wait_complete(s, 0), "cancelled requests complete");
    check_hr(s->GetHResult(0), S_OK, "a cancelled request is a success");
    const std::vector<uint8_t> zeros(16, 0);
    check(outs[0] == zeros && outs[1] == zeros && outs[3] == zeros, "cancelled requests read nothing");
    check(std::memcmp(outs[2].data(), env.b.data(), 16) == 0 && std::memcmp(outs[4].data(), env.b.data(), 16) == 0,
          "requests the mask did not match are read");
    check(std::memcmp(outs[5].data(), env.b.data(), 16) == 0, "a request enqueued after the cancel is read");
    s->Release();
    q->Release();
    b->Release();
}

void test_auto_submit_and_full_queue(Env& env) {
    IDStorageFileX* b = open(env, L"GAME:\\data\\b.bin");
    IDStorageQueueX2* q = make_queue(env, 0x80, true);
    std::vector<std::vector<uint8_t>> outs(64, std::vector<uint8_t>(8, 0));
    for (int i = 0; i < 63; ++i) {
        DSTORAGE_REQUEST r = file_request(b, i, outs[i].data(), 8);
        q->EnqueueRequest(&r);
    }
    DSTORAGE_QUEUE_INFO info{};
    q->Query(&info);
    check(info.RequestCountUntilAutoSubmit == 1, "one entry before the automatic submit");
    Sleep(20);
    check(outs[0] == std::vector<uint8_t>(8, 0), "below half the capacity nothing is submitted");
    DSTORAGE_REQUEST r = file_request(b, 63, outs[63].data(), 8);
    q->EnqueueRequest(&r);  // the 64th: half of 0x80
    check(wait_until([&] { return std::memcmp(outs[63].data(), env.b.data() + 63, 8) == 0; }),
          "reaching half the capacity submits without Submit");
    bool all = true;
    for (int i = 0; i < 64; ++i) all = all && std::memcmp(outs[i].data(), env.b.data() + i, 8) == 0;
    check(all, "every auto-submitted read has its bytes");
    q->Release();

    // Auto-submit disabled and full: an enqueue waits until another thread submits.
    q = make_queue(env, 0x80, false);
    IDStorageStatusArrayX* s = make_status(env, 1);
    constexpr int kMany = 400;
    std::vector<uint8_t> sink(kMany * 4);
    std::atomic<bool> done{false};
    std::thread producer([&] {
        for (int i = 0; i < kMany; ++i) {
            DSTORAGE_REQUEST rr = file_request(b, i, sink.data() + i * 4, 4);
            q->EnqueueRequest(&rr);
        }
        q->EnqueueStatus(s, 0);
        done.store(true);
    });
    check(wait_until([&] {
              DSTORAGE_QUEUE_INFO i{};
              q->Query(&i);
              return i.EmptySlotCount == 0;
          }),
          "the queue fills to capacity - 1");
    Sleep(20);
    check(!done.load(), "an enqueue into a full queue waits");
    while (!done.load()) {
        q->Submit();
        Sleep(1);
    }
    producer.join();
    q->Submit();
    check(wait_complete(s, 0), "the producer's reads complete");
    bool same = true;
    for (int i = 0; i < kMany; ++i) same = same && std::memcmp(sink.data() + i * 4, env.b.data() + i, 4) == 0;
    check(same, "every read of the full-queue run has its bytes");
    s->Release();
    q->Release();
    b->Release();
}

void test_memory_queue(Env& env) {
    IDStorageQueueX2* q = make_queue(env, 0x80, true, DSTORAGE_REQUEST_SOURCE_MEMORY, DSTORAGE_PRIORITY_REALTIME);
    IDStorageStatusArrayX* s = make_status(env, 6);
    std::vector<uint8_t> buf = pattern(8192, 9);
    std::vector<uint8_t> want = buf;
    std::memmove(want.data() + 1024, want.data(), 4096);
    DSTORAGE_REQUEST r{};
    r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
    r.Source = buf.data();
    r.Destination = buf.data() + 1024;
    r.SourceSize = r.DestinationSize = 4096;
    q->EnqueueRequest(&r);
    q->EnqueueStatus(s, 0);
    void* reserved = VirtualAlloc(nullptr, 1 << 16, MEM_RESERVE, PAGE_READWRITE);
    DSTORAGE_REQUEST bad = r;
    bad.Destination = reserved;
    q->EnqueueRequest(&bad);
    q->EnqueueStatus(s, 1);
    DSTORAGE_REQUEST file_on_memory{};  // a file request on a memory queue
    file_on_memory.Destination = buf.data();
    file_on_memory.SourceSize = file_on_memory.DestinationSize = 4;
    q->EnqueueRequest(&file_on_memory);
    q->EnqueueStatus(s, 2);
    DSTORAGE_REQUEST huge = r;
    huge.SourceSize = huge.DestinationSize = (1u << 29) + 1;  // more than 1 GiB together
    q->EnqueueRequest(&huge);
    q->EnqueueStatus(s, 3);
    // Buffers over 32 MiB need 64 KiB or 2 MiB pages (DSTORAGE_REQUEST). Reserved memory: a copy that is not refused
    // faults and reports an access violation instead.
    constexpr uint32_t k32MiB = 32u << 20;
    void* wide = VirtualAlloc(nullptr, 2 * (k32MiB + 4096), MEM_RESERVE, PAGE_READWRITE);
    check(wide != nullptr, "reserve 64 MiB for the over-limit copy");
    DSTORAGE_REQUEST over = r;
    over.Source = wide;
    over.Destination = static_cast<uint8_t*>(wide) + k32MiB + 4096;
    over.SourceSize = over.DestinationSize = k32MiB + 1;
    q->EnqueueRequest(&over);
    q->EnqueueStatus(s, 4);
    std::vector<uint8_t> at_limit_source = pattern(k32MiB, 10), at_limit(k32MiB, 0);
    DSTORAGE_REQUEST limit = r;
    limit.Source = at_limit_source.data();
    limit.Destination = at_limit.data();
    limit.SourceSize = limit.DestinationSize = k32MiB;
    q->EnqueueRequest(&limit);
    q->EnqueueStatus(s, 5);
    q->Submit();
    check(wait_complete(s, 5), "memory requests complete");
    check_hr(s->GetHResult(4), E_DSTORAGE_REQUEST_TOO_LARGE, "an unpaged buffer over 32 MiB is refused untouched");
    check_hr(s->GetHResult(5), S_OK, "a 32 MiB unpaged copy");
    check(at_limit == at_limit_source, "the 32 MiB copy moved every byte");
    VirtualFree(wide, 0, MEM_RELEASE);
    check_hr(s->GetHResult(0), S_OK, "a memory-to-memory copy");
    check(buf == want, "an overlapping memory copy moves like memmove");
    check_hr(s->GetHResult(1), E_DSTORAGE_ACCESS_VIOLATION, "a memory copy to memory the process cannot write");
    check_hr(s->GetHResult(2), E_DSTORAGE_INVALID_SOURCE_TYPE, "a file request on a memory queue");
    check_hr(s->GetHResult(3), E_DSTORAGE_REQUEST_TOO_LARGE, "source plus destination over 1 GiB");
    VirtualFree(reserved, 0, MEM_RELEASE);
    s->Release();
    q->Release();
}

void test_close_and_release(Env& env) {
    IDStorageFileX* big = open(env, L"GAME:\\data\\big.bin");
    // Close with requests in flight: it returns after they all ran.
    IDStorageQueueX2* q = make_queue(env, 0x100, true);
    constexpr int kReads = 300;
    constexpr uint32_t kChunk = 8192;
    std::vector<uint8_t> out(kReads * kChunk);
    for (int i = 0; i < kReads; ++i) {
        DSTORAGE_REQUEST r = file_request(big, static_cast<uint64_t>(i) * kChunk, out.data() + i * kChunk, kChunk);
        q->EnqueueRequest(&r);
    }
    q->Close();
    check(std::memcmp(out.data(), env.big.data(), out.size()) == 0, "Close returns after every enqueued read ran");
    IDStorageStatusArrayX* s = make_status(env, 1);
    std::vector<uint8_t> tmp(8, 0);
    DSTORAGE_REQUEST r = file_request(big, 0, tmp.data(), 8);
    q->EnqueueRequest(&r);
    q->EnqueueStatus(s, 0);
    q->Submit();
    q->Close();  // a second Close is harmless
    Sleep(20);
    check(tmp == std::vector<uint8_t>(8, 0), "a request enqueued after Close is not run");
    DSTORAGE_ERROR_RECORD rec{};
    q->RetrieveErrorRecord(&rec);
    check(rec.FailureCount == 2 && rec.FirstFailure.HResult == E_DSTORAGE_QUEUE_CLOSED,
          "enqueues after Close are recorded");
    check(q->Release() == 0, "a closed queue is freed by its last Release");

    // The last Release with requests in flight finishes them first.
    q = make_queue(env, 0x100, true);
    std::fill(out.begin(), out.end(), uint8_t{0});
    for (int i = 0; i < kReads; ++i) {
        DSTORAGE_REQUEST rr = file_request(big, static_cast<uint64_t>(i) * kChunk, out.data() + i * kChunk, kChunk);
        q->EnqueueRequest(&rr);
    }
    q->Submit();
    big->Release();  // the requests hold the file
    check(q->Release() == 0, "the last Release of a busy queue");
    check(std::memcmp(out.data(), env.big.data(), out.size()) == 0, "the last Release ran the reads in flight");

    // IDStorageFileX::Close: submitted reads finish, a later one fails with E_DSTORAGE_FILE_NOT_OPEN.
    IDStorageFileX* b = open(env, L"GAME:\\data\\b.bin");
    q = make_queue(env, 0x80, false);
    IDStorageStatusArrayX* s2 = make_status(env, 3);
    std::vector<uint8_t> first(100, 0), second(100, 0);
    DSTORAGE_REQUEST r1 = file_request(b, 0, first.data(), 100);
    q->EnqueueRequest(&r1);
    q->EnqueueStatus(s2, 0);
    q->Submit();
    DSTORAGE_REQUEST r2 = file_request(b, 0, second.data(), 100);
    q->EnqueueRequest(&r2);  // not submitted yet
    b->Close();
    check(std::memcmp(first.data(), env.b.data(), 100) == 0,
          "IDStorageFileX::Close waits for the file's submitted reads");
    check(wait_complete(s2, 0) && s2->GetHResult(0) == S_OK, "the submitted read succeeded");
    check(b->GetHandle() == INVALID_HANDLE_VALUE, "GetHandle on a closed file");
    q->EnqueueStatus(s2, 1);
    DSTORAGE_REQUEST r3 = file_request(b, 0, second.data(), 0);
    q->EnqueueRequest(&r3);
    q->EnqueueStatus(s2, 2);
    q->Submit();
    check(wait_complete(s2, 2), "the reads after the close complete");
    check_hr(s2->GetHResult(1), E_DSTORAGE_FILE_NOT_OPEN, "a read of a closed file");
    check_hr(s2->GetHResult(2), E_DSTORAGE_FILE_NOT_OPEN, "a zero-size read of a closed file");
    check(second == std::vector<uint8_t>(100, 0), "a read of a closed file writes nothing");
    q->Release();
    b->Release();
    s->Release();
    s2->Release();
}

void test_threads(Env& env) {
    IDStorageFileX* big = open(env, L"GAME:\\data\\big.bin");
    IDStorageQueueX2* q = make_queue(env, 0x80, true);
    constexpr int kThreads = 4, kPer = 250;
    constexpr uint32_t kChunk = 512;
    std::vector<std::vector<uint8_t>> outs(kThreads, std::vector<uint8_t>(kPer * kChunk));
    std::vector<IDStorageStatusArrayX*> arrays;
    for (int t = 0; t < kThreads; ++t) arrays.push_back(make_status(env, 1));
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([&, t] {
            for (int i = 0; i < kPer; ++i) {
                const uint64_t offset = (static_cast<uint64_t>(t) * kPer + i) * kChunk % (env.big.size() - kChunk);
                DSTORAGE_REQUEST r = file_request(big, offset, outs[t].data() + i * kChunk, kChunk, t);
                q->EnqueueRequest(&r);
            }
            q->EnqueueStatus(arrays[t], 0);
            q->Submit();
        });
    for (auto& th : threads) th.join();
    bool ok = true;
    for (int t = 0; t < kThreads; ++t) {
        ok = ok && wait_complete(arrays[t], 0) && arrays[t]->GetHResult(0) == S_OK;
        for (int i = 0; i < kPer; ++i) {
            const uint64_t offset = (static_cast<uint64_t>(t) * kPer + i) * kChunk % (env.big.size() - kChunk);
            ok = ok && std::memcmp(outs[t].data() + i * kChunk, env.big.data() + offset, kChunk) == 0;
        }
    }
    check(ok, "four threads enqueueing at once all read their bytes");
    for (auto* a : arrays) a->Release();
    q->Release();
    big->Release();
}

void test_iid_policy_events_and_cancel(Env& env) {
    // Well-known system interfaces and made-up ids: QueryInterface refuses all of them.
    const GUID kRandom = {0x5eed0001, 0x2222, 0x4222, {0x82, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x01}};
    const GUID kIMarshal = {0x00000003, 0x0000, 0x0000, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
    const GUID kIAgileObject = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};
    const GUID kINoMarshal = {0xecc8691b, 0xc1db, 0x4dc0, {0x85, 0x5e, 0x65, 0xf6, 0xc5, 0x51, 0xaf, 0x49}};
    const GUID kIClientSecurity = {0x0000013d, 0x0000, 0x0000, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
    const GUID kPcStatusArrayId = {0x82397587, 0x7cd5, 0x453b, {0xa0, 0x2e, 0x31, 0x37, 0x9b, 0xd6, 0x46, 0x56}};
    const GUID kPcQueueId = {0xcfdbd83f, 0x9e06, 0x4fda, {0x8e, 0xa5, 0x69, 0x04, 0x21, 0x37, 0xf4, 0x9b}};
    const GUID kQueueId2 = {0x0d5e0012, 0x1111, 0x4111, {0x81, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x12}};
    const GUID refused[] = {kRandom, kIMarshal, kIAgileObject, kINoMarshal, kIClientSecurity};

    IDStorageQueueX2* q = make_queue(env, 0x80, false);
    IDStorageFileX* b = open(env, L"GAME:\\data\\b.bin");
    IDStorageStatusArrayX* s = make_status(env, 8);
    IUnknown* objects[] = {env.factory, q, b, s};
    for (IUnknown* o : objects)
        for (const GUID& g : refused) {
            void* out = reinterpret_cast<void*>(1);
            check_hr(o->QueryInterface(g, &out), E_NOINTERFACE, "QueryInterface refuses an unknown or system id");
            check(out == nullptr, "a refused QueryInterface clears ppv");
        }
    void* out = nullptr;
    check_hr(q->QueryInterface(kFileId, &out), E_NOINTERFACE, "a queue refuses a file id");
    check_hr(b->QueryInterface(kQueueId, &out), E_NOINTERFACE, "a file refuses a queue id");
    check_hr(s->QueryInterface(kFactoryId, &out), E_NOINTERFACE, "a status array refuses a factory id");
    check_hr(q->QueryInterface(__uuidof(IUnknown), &out), S_OK, "QueryInterface(IUnknown)");
    if (out) static_cast<IUnknown*>(out)->Release();

    // Creating calls learn a new id but refuse system ones.
    auto get = reinterpret_cast<DStorageGetFactoryFn>(rt::shims::find("dstorage_xs.dll", "DStorageGetFactory"));
    check_hr(get(kIMarshal, &out), E_NOINTERFACE, "DStorageGetFactory refuses IMarshal");
    check_hr(get(kIAgileObject, &out), E_NOINTERFACE, "DStorageGetFactory refuses IAgileObject");
    DSTORAGE_QUEUE_DESC1 d{};
    d.Capacity = 0x80;
    check_hr(env.factory->CreateQueue1(&d, kINoMarshal, &out), E_NOINTERFACE, "CreateQueue1 refuses INoMarshal");
    check_hr(env.factory->OpenFile(L"GAME:\\data\\b.bin", kIClientSecurity, &out), E_NOINTERFACE,
             "OpenFile refuses IClientSecurity");
    check_hr(env.factory->CreateQueue1(&d, kPcQueueId, &out), E_NOINTERFACE, "CreateQueue1 refuses the PC queue id");
    check_hr(env.factory->CreateQueue1(&d, kPcStatusArrayId, &out), E_NOINTERFACE,
             "the status array id is not a queue id");
    check_hr(env.factory->CreateQueue1(&d, kQueueId2, &out), S_OK, "a creating call learns a new queue id");
    if (out) static_cast<IUnknown*>(out)->Release();
    check_hr(q->QueryInterface(kQueueId2, &out), S_OK, "QueryInterface accepts an id a queue was created with");
    if (out) static_cast<IUnknown*>(out)->Release();

    // The PC IDStorageStatusArray id has the console interface's shape and is accepted.
    check_hr(env.factory->CreateStatusArray(2, nullptr, kPcStatusArrayId, &out), S_OK,
             "CreateStatusArray accepts the PC IDStorageStatusArray id");
    if (out) static_cast<IUnknown*>(out)->Release();
    check_hr(s->QueryInterface(kPcStatusArrayId, &out), S_OK, "a status array answers the PC IDStorageStatusArray id");
    if (out) static_cast<IUnknown*>(out)->Release();

    // EnqueueSetEvent duplicates the handle: closing the title's handle, even with its value reused, changes nothing.
    HANDLE first = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    q->EnqueueSetEvent(first);
    CloseHandle(first);
    HANDLE second = CreateEventW(nullptr, TRUE, FALSE, nullptr);  // may get the same handle value
    q->EnqueueSetEvent(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x7ffffff0)));  // not a handle
    q->EnqueueSetEvent(nullptr);
    q->EnqueueStatus(s, 0);
    q->Submit();
    check(wait_complete(s, 0), "the event entries complete");
    check(WaitForSingleObject(second, 0) == WAIT_TIMEOUT, "a closed event's reused handle value is not set");
    DSTORAGE_ERROR_RECORD rec{};
    q->RetrieveErrorRecord(&rec);
    check(rec.FailureCount == 2 && rec.FirstFailure.CommandType == kCommandTypeEvent &&
              FAILED(rec.FirstFailure.HResult),
          "an invalid and a null event handle are recorded as EVENT failures; the closed one is not");
    CloseHandle(second);

    // A cancelled request that also had an enqueue error is still a success, and not counted as a failure.
    std::vector<uint8_t> tmp(8, 0);
    DSTORAGE_REQUEST foreign = file_request(reinterpret_cast<IDStorageFileX*>(s), 0, tmp.data(), 8, 0x99);
    q->EnqueueRequest(&foreign);
    DSTORAGE_REQUEST null_file = file_request(nullptr, 0, tmp.data(), 8, 0x99);
    q->EnqueueRequest(&null_file);
    q->CancelRequestsWithTag(~0ull, 0x99);
    q->EnqueueStatus(s, 1);
    q->Submit();
    check(wait_complete(s, 1), "cancelled bad requests complete");
    check_hr(s->GetHResult(1), S_OK, "a cancelled request with an enqueue error is a success");
    q->RetrieveErrorRecord(&rec);
    check(rec.FailureCount == 0, "a cancelled request is not counted as a failure");

    s->Release();
    b->Release();
    q->Release();
}

// Hold the worker at its first read. PAGE_NOACCESS gives a catchable access violation on Windows and Proton.
// The handler restores read access before retrying exactly that instruction.
struct GuardHold {
    uint8_t* page = nullptr;
    HANDLE entered = nullptr;
    HANDLE release = nullptr;
};
GuardHold* g_hold = nullptr;

LONG CALLBACK hold_on_guard(EXCEPTION_POINTERS* x) {
    const EXCEPTION_RECORD* e = x->ExceptionRecord;
    if (!g_hold || e->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || e->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;
    const auto* address = reinterpret_cast<const uint8_t*>(e->ExceptionInformation[1]);
    if (address < g_hold->page || address >= g_hold->page + 4096) return EXCEPTION_CONTINUE_SEARCH;
    DWORD old;
    if (!VirtualProtect(g_hold->page, 4096, PAGE_READWRITE, &old)) return EXCEPTION_CONTINUE_SEARCH;
    SetEvent(g_hold->entered);
    WaitForSingleObject(g_hold->release, 10000);
    return EXCEPTION_CONTINUE_EXECUTION;
}

// DSTORAGE_PRIORITY_STATS: TotalRequestCount and the byte totals count submitted requests; PendingRequestCount counts
// requests whose processing has started and not completed. A memory queue (realtime, the only priority it may have):
// the counts are compared with the ones before.
void test_stats_timing(Env& env) {
    auto realtime = [&] {
        DSTORAGE_STATS st{};
        env.factory->GetStats(&st);
        return st.RealtimePriorityStats;
    };
    const DSTORAGE_PRIORITY_STATS before = realtime();
    IDStorageQueueX2* q = make_queue(env, 0x80, false, DSTORAGE_REQUEST_SOURCE_MEMORY, DSTORAGE_PRIORITY_REALTIME);
    if (!q) return;
    IDStorageStatusArrayX* s = make_status(env, 1);
    GuardHold hold;
    hold.page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    hold.entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    hold.release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!hold.page || !hold.entered || !hold.release) {
        check(false, "allocate the guard page and its events");
        return;
    }
    std::memset(hold.page, 0x5A, 4096);
    DWORD old = 0;
    check(VirtualProtect(hold.page, 4096, PAGE_NOACCESS, &old) != FALSE, "arm the first-read barrier");
    std::vector<uint8_t> out(4096, 0);
    DSTORAGE_REQUEST r{};
    r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
    r.Source = hold.page;
    r.Destination = out.data();
    r.SourceSize = r.DestinationSize = 4096;
    q->EnqueueRequest(&r);
    q->EnqueueStatus(s, 0);
    DSTORAGE_PRIORITY_STATS p = realtime();
    check(p.TotalRequestCount == before.TotalRequestCount &&
              p.TotalRequestSourceByteCount == before.TotalRequestSourceByteCount &&
              p.TotalRequestDestinationByteCount == before.TotalRequestDestinationByteCount,
          "an enqueued request is not counted before Submit");

    g_hold = &hold;
    void* handler = AddVectoredExceptionHandler(1, hold_on_guard);
    q->Submit();
    check(WaitForSingleObject(hold.entered, 10000) == WAIT_OBJECT_0, "the copy reached the guard page");
    p = realtime();
    check(p.TotalRequestCount == before.TotalRequestCount + 1 &&
              p.TotalRequestSourceByteCount == before.TotalRequestSourceByteCount + 4096 &&
              p.TotalRequestDestinationByteCount == before.TotalRequestDestinationByteCount + 4096,
          "a submitted request is counted, with its sizes");
    check(p.PendingRequestCount == before.PendingRequestCount + 1, "a running request is pending");
    SetEvent(hold.release);
    check(wait_complete(s, 0), "the held copy completes");
    check_hr(s->GetHResult(0), S_OK, "the held copy succeeds");
    check(out == std::vector<uint8_t>(4096, 0x5A), "the held copy moved every byte");
    check(realtime().PendingRequestCount == before.PendingRequestCount, "a completed request is no longer pending");
    RemoveVectoredExceptionHandler(handler);
    g_hold = nullptr;
    CloseHandle(hold.entered);
    CloseHandle(hold.release);
    VirtualFree(hold.page, 0, MEM_RELEASE);
    s->Release();
    q->Release();
}

void test_stats(Env& env) {
    DSTORAGE_STATS st{};
    env.factory->GetStats(&st);
    check(st.CurrentOpenFileCount == 0, "every file was released");
    check(st.NormalPriorityStats.CurrentQueueCount == 0 && st.RealtimePriorityStats.CurrentQueueCount == 0,
          "every queue was released");
    check(st.NormalPriorityStats.TotalRequestCount > 1000 && st.NormalPriorityStats.TotalFenceCount >= 2 &&
              st.RealtimePriorityStats.TotalRequestCount == 7,
          "requests and fences are counted per priority");
    check(st.CumulativeOpenFileCount >= 10 && st.MaxOpenFileCount >= 3, "files are counted");
    check(st.ErrorCount > 10, "errors are counted");
}


// ---- zlib requests ------------------------------------------------------------------------------------------------
//
// Synthetic streams only. The two Huffman streams were made with Python's zlib (3.14, zlib-ng 1.3.1 API) from the
// text zlib_text() builds, which in Python is
//   text = lambda n: b"".join(b"line %04d: a zlib request for the queue, value %d\n" % (i, (i * i) % 97)
//                             for i in range(n))
// kZlibDynamic = zlib.compress(text(64), 9): one final dynamic-Huffman block (BTYPE 2), 345 bytes for 3,191.
// kZlibFixed = zlib.compress(text(4), 1): one final fixed-Huffman block (BTYPE 1), 77 bytes for 196.
// Streams with stored blocks (BTYPE 0) are built here by stored_stream(); for a single block that is byte for byte
// what zlib.compress(data, 0) gives.
constexpr uint8_t kZlibDynamic[] = {
    0x78, 0xda, 0x95, 0xd4, 0x4d, 0x4e, 0x03, 0x31, 0x0c, 0x05, 0xe0, 0x3d, 0xa7, 0xc8, 0x01, 0x58, 0x4c, 0xfc,
    0x97, 0x84, 0xdb, 0x14, 0x69, 0x10, 0x95, 0x46, 0x20, 0x4a, 0xcb, 0x82, 0xd3, 0x33, 0x0b, 0xf4, 0xbc, 0x7d,
    0xee, 0x6e, 0xa4, 0x3e, 0x25, 0xce, 0x67, 0xfb, 0xb8, 0x7e, 0xec, 0x6d, 0x3b, 0x7f, 0x2f, 0xed, 0xd2, 0x7e,
    0x8f, 0xeb, 0x6b, 0xbb, 0xed, 0x5f, 0x8f, 0xfd, 0xfb, 0xde, 0xde, 0x3e, 0x6f, 0xed, 0xfe, 0xbe, 0xb7, 0xf3,
    0xeb, 0xb1, 0x3f, 0xb7, 0x9f, 0xcb, 0xf1, 0x38, 0xff, 0xf9, 0x74, 0xfc, 0x07, 0x3a, 0x17, 0xe8, 0x08, 0x08,
    0x17, 0x30, 0x04, 0x94, 0x0b, 0x2c, 0x04, 0x8c, 0xbc, 0x52, 0x20, 0xe1, 0x5c, 0x42, 0x1c, 0x89, 0xe0, 0x12,
    0x9a, 0x67, 0x0c, 0xb2, 0xee, 0xac, 0x63, 0x72, 0x89, 0xc8, 0xa7, 0x5a, 0x5c, 0x62, 0x42, 0xa3, 0x93, 0xde,
    0x8a, 0x00, 0xe9, 0x2d, 0xb8, 0x54, 0x67, 0xc1, 0x07, 0x12, 0xa4, 0xf8, 0x10, 0x24, 0x48, 0xf2, 0x0c, 0x90,
    0xe2, 0x9a, 0x2f, 0x45, 0x8a, 0x47, 0x9e, 0x41, 0x8a, 0x2f, 0x74, 0x55, 0x27, 0xc5, 0x35, 0x39, 0x48, 0xf1,
    0x81, 0x81, 0x15, 0x52, 0xbc, 0xa3, 0x0e, 0x21, 0xc9, 0x1d, 0xb7, 0x12, 0x92, 0x7c, 0x61, 0x3a, 0x84, 0x24,
    0x37, 0xb4, 0x95, 0x90, 0xe4, 0x0b, 0x82, 0x42, 0x9a, 0x5b, 0xd6, 0x41, 0x9a, 0xaf, 0xbc, 0x15, 0x69, 0xee,
    0xe9, 0x41, 0x9a, 0x4f, 0x04, 0x48, 0xf2, 0x40, 0x5b, 0x29, 0x49, 0x2e, 0x98, 0x40, 0x25, 0xc9, 0x27, 0x6e,
    0xa5, 0x24, 0xb9, 0xe3, 0xa9, 0x94, 0x24, 0x17, 0x34, 0xa2, 0x92, 0xe4, 0x13, 0x2b, 0x54, 0x49, 0xf2, 0x40,
    0x93, 0x28, 0xbb, 0xd8, 0xf3, 0x75, 0x49, 0xf2, 0x9e, 0x67, 0xb0, 0xe4, 0x18, 0x0f, 0x65, 0xcd, 0x91, 0x30,
    0xd2, 0xdc, 0x20, 0x68, 0xa4, 0xb9, 0xc2, 0xc3, 0x48, 0xf3, 0x9e, 0x67, 0x90, 0xe6, 0x59, 0x06, 0x3b, 0xe5,
    0x98, 0x59, 0x23, 0xc9, 0x27, 0x00, 0x8d, 0x24, 0x1f, 0x68, 0x2b, 0x23, 0xc9, 0x47, 0x9e, 0x41, 0x92, 0x8f,
    0xac, 0x63, 0x55, 0x13, 0xbe, 0x55, 0x6f, 0xe5, 0xbd, 0x5a, 0xb9, 0x4b, 0xf5, 0x75, 0x5d, 0xab, 0x82, 0x6e,
    0xc5, 0x26, 0x71, 0xaf, 0xf6, 0xa1, 0x47, 0xb5, 0xd7, 0x7d, 0x54, 0xe7, 0xc9, 0x67, 0x75, 0x66, 0x7d, 0x55,
    0xf7, 0x42, 0x6c, 0xd5, 0xdd, 0x13, 0xbd, 0xba, 0xdf, 0x42, 0xaa, 0x3b, 0x34, 0x94, 0xde, 0xd3, 0x7f, 0x97,
    0x8b, 0x11, 0xce,
};
constexpr uint8_t kZlibFixed[] = {
    0x78, 0x01, 0xcb, 0xc9, 0xcc, 0x4b, 0x55, 0x30, 0x30, 0x30, 0x30, 0xb0, 0x52, 0x48, 0x54, 0xa8, 0xca, 0xc9,
    0x4c, 0x52, 0x28, 0x4a, 0x2d, 0x2c, 0x4d, 0x2d, 0x2e, 0x51, 0x48, 0xcb, 0x2f, 0x52, 0x28, 0xc9, 0x48, 0x55,
    0x28, 0x2c, 0x4d, 0x2d, 0x4d, 0xd5, 0x51, 0x28, 0x4b, 0xcc, 0x29, 0x4d, 0x55, 0x30, 0xe0, 0x82, 0x69, 0x30,
    0x24, 0x4e, 0x83, 0x21, 0x5c, 0x83, 0x11, 0x71, 0x1a, 0x4c, 0xe0, 0x1a, 0x8c, 0x89, 0xd3, 0x60, 0xc9, 0x05,
    0x00, 0xb0, 0x6e, 0x40, 0x49,
};

std::vector<uint8_t> zlib_text(int lines) {
    std::string t;
    char line[96];
    for (int i = 0; i < lines; ++i) {
        std::snprintf(line, sizeof line, "line %04d: a zlib request for the queue, value %d\n", i, (i * i) % 97);
        t += line;
    }
    return {t.begin(), t.end()};
}

uint32_t adler32(const std::vector<uint8_t>& data) {
    uint32_t a = 1, b = 0;
    for (const uint8_t x : data) {
        a = (a + x) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

// A zlib stream (header 78 01) whose first block stores `stored` (a final block when there is no tail), followed by
// the deflate blocks of the zlib stream `tail` (its header and Adler-32 dropped: a stored block ends on a byte
// boundary, so they follow as they are) and the Adler-32 of the whole result, `stored` then `tail_plain`.
std::vector<uint8_t> stored_stream(const std::vector<uint8_t>& stored, const uint8_t* tail = nullptr,
                                   size_t tail_size = 0, const std::vector<uint8_t>& tail_plain = {}) {
    const auto n = static_cast<uint16_t>(stored.size());
    std::vector<uint8_t> z = {0x78, 0x01, static_cast<uint8_t>(tail ? 0 : 1), static_cast<uint8_t>(n),
                              static_cast<uint8_t>(n >> 8), static_cast<uint8_t>(~n), static_cast<uint8_t>(~n >> 8)};
    z.insert(z.end(), stored.begin(), stored.end());
    if (tail) z.insert(z.end(), tail + 2, tail + tail_size - 4);
    std::vector<uint8_t> plain = stored;
    plain.insert(plain.end(), tail_plain.begin(), tail_plain.end());
    const uint32_t check = adler32(plain);
    for (int shift = 24; shift >= 0; shift -= 8) z.push_back(static_cast<uint8_t>(check >> shift));
    return z;
}

void test_zlib(Env& env) {
    const std::vector<uint8_t> dynamic(std::begin(kZlibDynamic), std::end(kZlibDynamic));
    const std::vector<uint8_t> fixed(std::begin(kZlibFixed), std::end(kZlibFixed));
    const std::vector<uint8_t> dynamic_plain = zlib_text(64), fixed_plain = zlib_text(4);
    // The first block header is the low three bits after the two header bytes: BFINAL, then BTYPE (RFC 1951 3.2.3).
    check(dynamic.size() == 345 && (dynamic[2] & 7) == (1 | 2 << 1), "kZlibDynamic is one final dynamic block");
    check(fixed.size() == 77 && (fixed[2] & 7) == (1 | 1 << 1), "kZlibFixed is one final fixed-Huffman block");
    check(dynamic_plain.size() == 3191 && fixed_plain.size() == 196, "the synthetic text has the generator's sizes");

    const std::vector<uint8_t> stored_part = pattern(300, 21);
    const std::vector<uint8_t> mixed = stored_stream(stored_part, kZlibDynamic, sizeof kZlibDynamic, dynamic_plain);
    std::vector<uint8_t> mixed_plain = stored_part;
    mixed_plain.insert(mixed_plain.end(), dynamic_plain.begin(), dynamic_plain.end());
    check((mixed[2] & 7) == 0, "the mixed stream starts with a stored block that is not the last");
    const std::vector<uint8_t> stored_only = stored_stream(pattern(40, 22));  // larger than what it inflates to
    std::vector<uint8_t> bad_method = dynamic, bad_type = fixed, bad_check = dynamic, preset = dynamic;
    bad_method[0] ^= 1;          // CM 9: not deflate
    preset[1] = 0xbb;            // 78 BB: a valid check value with FDICT set (a preset dictionary)
    bad_type[2] |= 0x06;         // BTYPE 3, reserved
    bad_check.back() ^= 1;       // the Adler-32

    // One file, every stream at a 16-byte aligned offset and followed by 32 zero bytes.
    std::vector<uint8_t> file;
    auto put = [&](const std::vector<uint8_t>& z) {
        const uint64_t at = file.size();
        file.insert(file.end(), z.begin(), z.end());
        file.resize((file.size() + 32 + 15) & ~size_t{15}, 0);
        return at;
    };
    const uint64_t at_dynamic = put(dynamic), at_fixed = put(fixed), at_mixed = put(mixed),
                   at_stored = put(stored_only), at_method = put(bad_method), at_preset = put(preset),
                   at_type = put(bad_type), at_check = put(bad_check);
    write_file(env.game / L"data" / L"zlib.bin", file);

    struct Case {
        uint64_t offset;
        uint32_t source, destination;
        HRESULT want;
        const std::vector<uint8_t>* result;  // the expected bytes; null when the destination stays untouched
        const char* what;
        uint64_t options = 0;
        uint32_t intermediate = 0;
    };
    const auto z = static_cast<uint32_t>(dynamic.size());
    std::vector<Case> cases = {
        {at_dynamic, z, 3191, S_OK, &dynamic_plain, "a zlib file request: dynamic Huffman"},
        {at_fixed, 77, 196, S_OK, &fixed_plain, "a zlib file request: fixed Huffman"},
        {at_mixed, static_cast<uint32_t>(mixed.size()), 3491, S_OK, &mixed_plain,
         "a zlib file request: a stored block, then dynamic Huffman"},
        {at_dynamic, z + 16, 3191, S_OK, &dynamic_plain, "bytes after the Adler-32 are not read"},
        {at_stored, static_cast<uint32_t>(stored_only.size()), 40, E_DSTORAGE_INVALID_DESTINATION_SIZE, nullptr,
         "a zlib DestinationSize not larger than SourceSize"},
        {at_method, z, 3191, E_DSTORAGE_ZLIB_BAD_HEADER, nullptr, "a header that is not deflate"},
        {at_preset, z, 3191, E_DSTORAGE_ZLIB_BAD_HEADER, nullptr, "a header with a preset dictionary"},
        {at_type, 77, 196, E_DSTORAGE_ZLIB_BAD_DATA, nullptr, "a reserved block type"},
        {at_dynamic, z - 10, 3191, E_DSTORAGE_ZLIB_BAD_DATA, nullptr, "a truncated stream"},
        {at_check, z, 3191, E_DSTORAGE_ZLIB_PARITY_FAIL, nullptr, "an Adler-32 mismatch"},
        {at_dynamic, z, 3192, E_DSTORAGE_DECOMPRESSION_ERROR, nullptr, "a stream shorter than DestinationSize"},
        {at_dynamic, z, 3190, E_DSTORAGE_DECOMPRESSION_ERROR, nullptr, "a stream longer than DestinationSize"},
        {at_dynamic + 8, z, 3191, E_DSTORAGE_INVALID_FILE_OFFSET, nullptr, "a zlib offset not 16-byte aligned"},
        {file.size() - 16, 32, 64, E_DSTORAGE_END_OF_FILE, nullptr, "a zlib source past the end of the file"},
        {at_dynamic, z, 3191, E_DSTORAGE_INVALID_INTERMEDIATE_SIZE, nullptr, "zlib with an IntermediateSize", 0,
         16},
        {at_dynamic, z, 3191, E_DSTORAGE_INVALID_DESTINATION_SIZE, nullptr, "zlib plus BCPack needs whole blocks",
         static_cast<uint64_t>(DSTORAGE_BCPACK_MODE_BC1) << options::kBcpackShift, 64},
        {at_dynamic, z, 3191, E_DSTORAGE_INVALID_DESTINATION_SIZE, nullptr, "zlib plus swizzle needs complete groups",
         static_cast<uint64_t>(DSTORAGE_SWIZZLE_MODE_4K_4K_8K) << options::kSwizzleShift},
    };

    DSTORAGE_STATS before{};
    env.factory->GetStats(&before);
    IDStorageFileX* f = open(env, L"GAME:\\data\\zlib.bin");
    IDStorageQueueX2* q = make_queue(env, 0x80, false);
    IDStorageStatusArrayX* s = make_status(env, static_cast<UINT32>(cases.size()));
    constexpr size_t kSlack = 64;  // past DestinationSize: must stay untouched
    std::vector<std::vector<uint8_t>> out(cases.size());
    for (size_t i = 0; i < cases.size(); ++i) {
        const Case& c = cases[i];
        out[i].assign(c.destination + kSlack, 0xEE);
        DSTORAGE_REQUEST r = file_request(f, c.offset, out[i].data(), c.destination);
        r.SourceSize = c.source;
        r.IntermediateSize = c.intermediate;
        r.Options.AsUINT64 = options::kZlib | c.options;
        q->EnqueueRequest(&r);
        q->EnqueueStatus(s, static_cast<UINT32>(i));
    }
    q->Submit();
    check(wait_complete(s, static_cast<UINT32>(cases.size() - 1)), "the zlib file requests complete");
    for (size_t i = 0; i < cases.size(); ++i) {
        const Case& c = cases[i];
        check_hr(s->GetHResult(static_cast<UINT32>(i)), c.want, c.what);
        const std::vector<uint8_t> untouched(c.destination, 0xEE);
        const bool head = c.result ? std::equal(c.result->begin(), c.result->end(), out[i].begin())
                                   : std::equal(untouched.begin(), untouched.end(), out[i].begin());
        const bool tail = std::all_of(out[i].end() - kSlack, out[i].end(), [](uint8_t x) { return x == 0xEE; });
        check(head && tail, c.result ? "the inflated bytes are the synthetic text, and nothing past them"
                                     : "a failed zlib request leaves the destination untouched");
    }
    s->Release();
    q->Release();
    f->Release();

    // A memory source: the same stream, one inflated in place (the compressed bytes sit at the end of their own
    // destination, as a title may load them), a corrupt one, a source and a destination the process cannot touch.
    IDStorageQueueX2* mq = make_queue(env, 0x80, true, DSTORAGE_REQUEST_SOURCE_MEMORY, DSTORAGE_PRIORITY_REALTIME);
    IDStorageStatusArrayX* ms = make_status(env, 5);
    void* reserved = VirtualAlloc(nullptr, 1 << 16, MEM_RESERVE, PAGE_READWRITE);
    std::vector<uint8_t> m0(3191, 0xEE), in_place(3191, 0xEE), m2(3191, 0xEE), m3(3191, 0xEE);
    std::copy(dynamic.begin(), dynamic.end(), in_place.end() - dynamic.size());
    DSTORAGE_REQUEST r{};
    r.Options.AsUINT64 = options::kZlib | options::kSourceMemory;
    r.SourceSize = z;
    r.DestinationSize = 3191;
    r.Source = dynamic.data();
    r.Destination = m0.data();
    mq->EnqueueRequest(&r);
    mq->EnqueueStatus(ms, 0);
    r.Source = in_place.data() + in_place.size() - dynamic.size();
    r.Destination = in_place.data();
    mq->EnqueueRequest(&r);
    mq->EnqueueStatus(ms, 1);
    r.Source = bad_check.data();
    r.Destination = m2.data();
    mq->EnqueueRequest(&r);
    mq->EnqueueStatus(ms, 2);
    r.Source = reserved;
    r.Destination = m3.data();
    mq->EnqueueRequest(&r);
    mq->EnqueueStatus(ms, 3);
    r.Source = dynamic.data();
    r.Destination = reserved;
    mq->EnqueueRequest(&r);
    mq->EnqueueStatus(ms, 4);
    mq->Submit();
    check(wait_complete(ms, 4), "the zlib memory requests complete");
    check_hr(ms->GetHResult(0), S_OK, "a zlib memory request");
    check(m0 == dynamic_plain, "the zlib memory request inflates to the synthetic text");
    check_hr(ms->GetHResult(1), S_OK, "a zlib memory request whose source lies inside its destination");
    check(in_place == dynamic_plain, "inflating in place gives the synthetic text");
    check_hr(ms->GetHResult(2), E_DSTORAGE_ZLIB_PARITY_FAIL, "a zlib memory request with an Adler-32 mismatch");
    check(m2 == std::vector<uint8_t>(3191, 0xEE), "the failed zlib memory request leaves the destination untouched");
    check_hr(ms->GetHResult(3), E_DSTORAGE_ACCESS_VIOLATION, "a zlib source the process cannot read");
    check_hr(ms->GetHResult(4), E_DSTORAGE_ACCESS_VIOLATION, "a zlib destination the process cannot write");
    VirtualFree(reserved, 0, MEM_RELEASE);
    ms->Release();
    mq->Release();

    // Counted when the inflate starts: the 11 file requests (4 good, 7 bad streams) and 4 memory requests (all but the
    // unreadable source) whose checks passed and whose source was read. Failed: the 7 bad file streams and the memory
    // one with an Adler-32 mismatch; a destination the process cannot write is not a decompression error.
    DSTORAGE_STATS after{};
    env.factory->GetStats(&after);
    check(after.TotalDecompressionRequestCount - before.TotalDecompressionRequestCount == 15,
          "TotalDecompressionRequestCount counts the zlib requests that reached the inflate");
    check(after.DecompressionErrors - before.DecompressionErrors == 8, "DecompressionErrors counts the bad streams");
    check(after.PendingDecompressionRequestCount == 0, "no decompression is pending afterwards");

    // Both file transforms may prepare concurrently, but publish to the same buffer
    // in enqueue order. The status after an invalid pair must retain its first error.
    f = open(env, L"GAME:\\data\\zlib.bin");
    q = make_queue(env, kMinQueueCapacity, false);
    s = make_status(env, 2);
    std::vector<uint8_t> overlap(dynamic_plain.size() + 1, 0xee);
    auto enqueue_zlib = [&](uint64_t at, uint32_t source, uint32_t dest) {
        auto request = file_request(f, at, overlap.data(), source);
        request.Options.AsUINT64 = options::kZlib;
        request.DestinationSize = dest;
        q->EnqueueRequest(&request);
    };
    enqueue_zlib(at_dynamic, z, 3191);
    enqueue_zlib(at_fixed, 77, 196);
    q->EnqueueStatus(s, 0);
    enqueue_zlib(at_check, z, 3191);
    enqueue_zlib(at_type, 77, 196);
    q->EnqueueStatus(s, 1);
    q->Submit();
    f->Close();
    check(wait_complete(s, 1), "adjacent file transforms retire before the final status");
    check_hr(s->GetHResult(0), S_OK, "overlapping file outputs complete in queue order");
    check_hr(s->GetHResult(1), E_DSTORAGE_ZLIB_PARITY_FAIL, "a pair reports the first failure in enqueue order");
    auto expected = dynamic_plain;
    std::copy(fixed_plain.begin(), fixed_plain.end(), expected.begin());
    expected.push_back(0xee);
    check(overlap == expected, "ordered publication and failed transforms preserve every destination byte");
    s->Release();
    q->Release();
    f->Release();
}

struct Vector {
    unsigned codec;
    std::span<const uint8_t> raw, plain, wrapped;
};
#include "dstorage_vectors.inc"

leonida::bcpack::Calibration test_profile() {
    leonida::bcpack::Calibration history{};
    auto& six = history.bc6[255];
    for (unsigned c = 0; c < 3; ++c) six.ep[8 * c + 1] = 16;
    six.ep_class = six.ix_class = 3;
    six.ep_confirmed.fill(16);
    six.ix_confirmed.fill(4);
    auto& seven = history.bc7[255];
    seven.ep.fill(64);
    seven.ep_bits.fill(8);
    seven.ep_confirmed.fill(8);
    seven.ix_bits.fill(4);
    seven.ix_confirmed.fill(4);
    return history;
}

// Requests whose destination the runtime's own guards keep read-only, written from the queue's worker threads (each
// through kernel_writes: src/title_page_watch.h):
//   1. a geometry tile the geometry watch armed (src/title_geometry_watch.h), with the D3D12 layer's opener as far as
//      that watch goes: a file read, a memory copy and a decompressed (BCPack) publish into it all succeed, and the
//      tile counts as written (its GPU copy is refilled);
//   2. an xmem physical page (the stream pool's CPU view) left read-only with no guard to reopen it, with
//      rt::xmem::reopen_unowned as the unguarder (as gta6rt sets it): the same three requests succeed.
void test_guarded_destinations(Env& env) {
    namespace gw = leonida::geometry_writes;
    namespace kw = leonida::kernel_writes;
    auto* file = open(env, L"game:/data/b.bin");
    auto* files = make_queue(env, 0x80, false);
    auto* memory = make_queue(env, 0x80, false, DSTORAGE_REQUEST_SOURCE_MEMORY, DSTORAGE_PRIORITY_REALTIME);
    auto* status = make_status(env, 3);
    const std::vector<uint8_t> source = pattern(4096, 21);
    const auto& packed = vectors[0];
    // Each request into `at` (file range, memory copy, BCPack), each of the three after `guard` made the pages
    // read-only again; the bytes are checked after each.
    const auto run = [&](uint8_t* at, const std::function<void()>& guard, const char* where) {
        guard();
        auto read = file_request(file, 300, at, 4096);
        files->EnqueueRequest(&read);
        files->EnqueueStatus(status, 0);
        files->Submit();
        check(wait_complete(status, 0), where);
        check_hr(status->GetHResult(0), S_OK, where);
        check(std::memcmp(at, env.b.data() + 300, 4096) == 0, where);
        guard();
        DSTORAGE_REQUEST copy{};
        copy.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        copy.Source = source.data();
        copy.Destination = at + 0x1000;
        copy.SourceSize = copy.DestinationSize = 4096;
        memory->EnqueueRequest(&copy);
        memory->EnqueueStatus(status, 1);
        memory->Submit();
        check(wait_complete(status, 1), where);
        check_hr(status->GetHResult(1), S_OK, where);
        check(std::memcmp(at + 0x1000, source.data(), 4096) == 0, where);
        guard();
        DSTORAGE_REQUEST bc{};
        bc.Options.AsUINT64 = options::kSourceMemory | (packed.codec << options::kBcpackShift);
        bc.Source = packed.raw.data();
        bc.SourceSize = static_cast<uint32_t>(packed.raw.size());
        bc.Destination = at + 0x2000;
        bc.DestinationSize = static_cast<uint32_t>(packed.plain.size());
        memory->EnqueueRequest(&bc);
        memory->EnqueueStatus(status, 2);
        memory->Submit();
        check(wait_complete(status, 2), where);
        check_hr(status->GetHResult(2), S_OK, where);
        check(std::equal(packed.plain.begin(), packed.plain.end(), at + 0x2000), where);
    };

    // 1. A geometry tile.
    auto* tile = static_cast<uint8_t*>(VirtualAlloc(nullptr, gw::tile_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(tile != nullptr && packed.plain.size() <= gw::tile_size - 0x2000, "a geometry tile for the destinations");
    if (tile) {
        const auto va = reinterpret_cast<uintptr_t>(tile);
        gw::Cell* cell = gw::acquire(va);
        kw::set_opener([](uintptr_t from, uintptr_t to) { gw::notify(from, to, gw::Event::open); });
        const auto version = cell->version.load();
        run(tile, [&] { gw::arm(va, *cell); }, "a request into an armed geometry tile");
        check(cell->version.load() != version, "the geometry tile counts as written");
        kw::set_opener(nullptr);
        gw::release(va);
        VirtualFree(tile, 0, MEM_RELEASE);
    }

    // 2. An xmem physical page read-only with no guard to answer for it.
    using Reserve = void*(WINAPI*)(void*, SIZE_T, DWORD, ULONGLONG, DWORD);
    using Allocate = BOOL(WINAPI*)(uint64_t, uint64_t*, uint64_t*);
    using Free = BOOL(WINAPI*)(uint64_t, const uint64_t*);
    using Map = void*(WINAPI*)(void*, uint64_t, const uint64_t*);
    const auto reserve = reinterpret_cast<Reserve>(rt::shims::find("xmem.dll", "XMemVirtualAlloc"));
    const auto allocate = reinterpret_cast<Allocate>(rt::shims::find("xmem.dll", "XMemAllocatePhysicalPages"));
    const auto free_pages = reinterpret_cast<Free>(rt::shims::find("xmem.dll", "XMemFreePhysicalPages"));
    const auto map = reinterpret_cast<Map>(rt::shims::find("xmem.dll", "XMemMapPhysicalPages"));
    auto* window = reserve ? static_cast<uint8_t*>(reserve(nullptr, 0x10000, MEM_RESERVE | 0x20400000, 0x23,
                                                           PAGE_READWRITE)) : nullptr;
    uint64_t page = 0, count = 1;
    check(window && allocate && allocate(0x280, &count, &page) && map && map(window, 1, &page) == window,
          "an xmem physical page for the destinations");
    if (window && count == 1) {
        for (size_t i = 0; i < 0x10000; i += 0x1000) window[i] = 0;  // committed
        kw::set_unguarder(&rt::xmem::reopen_unowned);
        run(window, [&] {
            DWORD old = 0;
            VirtualProtect(window, 0x10000, PAGE_READONLY, &old);
        }, "a request into an xmem page left read-only");
        kw::set_unguarder(nullptr);
        map(window, 1, nullptr);
        free_pages(1, &page);
    }
    status->Release();
    memory->Release();
    files->Release();
    file->Release();
}

void test_extended(Env& env) {
    auto* q = make_queue(env, 0x80, false, DSTORAGE_REQUEST_SOURCE_MEMORY, DSTORAGE_PRIORITY_REALTIME);
    auto* status = make_status(env, 1);
    auto submit = [&](DSTORAGE_REQUEST& r, HRESULT want) {
        q->EnqueueRequest(&r);
        q->EnqueueStatus(status, 0);
        check_hr(status->GetHResult(0), E_PENDING, "extended request is pending before Submit");
        q->Submit();
        check(wait_complete(status, 0), "extended request retires");
        check_hr(status->GetHResult(0), want, "extended request status");
    };
    DSTORAGE_REQUEST r{};
    for (const auto& v : vectors) {
        for (bool zlib : {false, true}) {
            const auto source = zlib ? v.wrapped : v.raw;
            const size_t bytes = v.plain.size();
            std::vector<uint8_t> output(bytes+1, 0xA5);
            r = {};
            r.Options.AsUINT64 = options::kSourceMemory | (v.codec << options::kBcpackShift) | zlib;
            r.Source = source.data();
            r.SourceSize = static_cast<uint32_t>(source.size());
            r.IntermediateSize = zlib ? static_cast<uint32_t>(v.raw.size()) : 0;
            r.Destination = output.data();
            r.DestinationSize = static_cast<uint32_t>(bytes);
            submit(r, S_OK);
            check(std::equal(v.plain.begin(), v.plain.end(), output.begin()) && output.back() == 0xA5,
                  "BC blocks equal the Python decoder, including all mode fields");
            if (!zlib) {
                std::fill(output.begin(), output.end(), 0xA5);
                r.SourceSize /= 2;
                submit(r, E_DSTORAGE_DECOMPRESSION_ERROR);
                check(std::all_of(output.begin(), output.end(), [](uint8_t v){return v == 0xA5;}),
                      "a truncated BCPack stream publishes no bytes");
            }
        }
    }
    {
        auto history = test_profile();
        auto& six = history.bc6[255];
        auto& seven = history.bc7[255];
        for (unsigned codec : {6u, 7u}) {
            const std::span<const uint8_t> raw = codec == 6 ? std::span<const uint8_t>(profile_raw6) : profile_raw7;
            const uint8_t* first = codec == 6 ? profile_first6 : profile_first7;
            std::vector<uint8_t> result(512);
            check(leonida::bcpack::decode(codec, raw, result, false, &history) == leonida::bcpack::Quality::confirmed,
                  "confirmed local BPTC predictor accepts the stream");
            bool exact = true;
            for (size_t j = 0; j < result.size(); ++j) exact &= result[j] == first[j % 16];
            check(exact, "propagated external BPTC history matches all reference blocks");
            DSTORAGE_REQUEST from_profile{};
            from_profile.Options.AsUINT64 = options::kSourceMemory | (uint64_t(codec) << options::kBcpackShift);
            from_profile.Source = raw.data();
            from_profile.SourceSize = static_cast<uint32_t>(raw.size());
            from_profile.Destination = result.data();
            from_profile.DestinationSize = static_cast<uint32_t>(result.size());
            std::fill(result.begin(), result.end(), 0xA5);
            submit(from_profile, S_OK);
            bool default_exact = true;
            for (size_t j = 0; j < result.size(); ++j) default_exact &= result[j] == first[j % 16];
            check(default_exact, "default sidecar next to the shader library supplies exact reference blocks");
            if (codec == 6) six.ep_class = 1;
            else seven.ep_confirmed.fill(0);
            check(leonida::bcpack::decode(codec, raw, result, false, &history) == leonida::bcpack::Quality::failed &&
                  leonida::bcpack::decode(codec, raw, result, true, &history) == leonida::bcpack::Quality::fitted,
                  "partially confirmed BPTC profile requires fitted mode");
            // A history entry the profile does not know (no bits at all): every block whose predictor chain starts
            // there is lost. The reference limit (1 in 1,024, at least 2) refuses the request; a lower divisor
            // rebuilds it and reports how many blocks were approximated. Strict mode still refuses it.
            auto unknown = history;
            if (codec == 6) unknown.bc6[255].ep_class = 0;
            else unknown.bc7[255].ep_bits.fill(0);
            const char* why = nullptr;
            leonida::bcpack::Report report;
            check(leonida::bcpack::decode(codec, raw, result, true, &unknown, &why) == leonida::bcpack::Quality::failed &&
                      why && std::string(why) == "missing-endpoint",
                  "an unknown BPTC predictor past the reference limit fails as missing-endpoint");
            check(leonida::bcpack::decode(codec, raw, result, true, &unknown, &why, true, 1, &report) ==
                          leonida::bcpack::Quality::fitted &&
                      report.blocks == result.size() / 16 && report.lost > 2 && report.lost <= report.blocks,
                  "a lower lost-block divisor rebuilds the request and reports the approximated blocks");
            check(leonida::bcpack::decode(codec, raw, result, false, &unknown, &why, true, 1, &report) ==
                      leonida::bcpack::Quality::failed,
                  "strict decoding still refuses an unknown BPTC predictor");
        }
    }
    std::vector<uint8_t> fitted(512);
    check(leonida::bcpack::decode(1, fitted_raw, fitted) == leonida::bcpack::Quality::failed,
          "strict decoding rejects fitted history");
    check(leonida::bcpack::decode(1, fitted_raw, fitted, true) == leonida::bcpack::Quality::fitted &&
          std::equal(std::begin(fitted_first), std::end(fitted_first), fitted.begin()),
          "opt-in fitted history matches the reference and reports its quality");

    // Three groups make an accidental per-64-KiB permutation distinguishable from macro-unshuffle.
    const std::vector<std::vector<int>> planes = {{1,1,2},{1,1,6},{1,3,1,1,2},{1,3,1,3}};
    for (unsigned m = 1; m <= 4; ++m) {
        std::vector<uint8_t> input, expected;
        for (unsigned c = 0; c < planes[m-1].size(); ++c)
            for (int group = 0; group < 3; ++group)
                input.insert(input.end(), planes[m-1][c]*4096, uint8_t(16*group+c));
        for (int group = 0; group < 3; ++group)
            for (unsigned c = 0; c < planes[m-1].size(); ++c)
                expected.insert(expected.end(), planes[m-1][c]*4096, uint8_t(16*group+c));
        std::vector<uint8_t> output(expected.size());
        r = {};
        r.Options.AsUINT64 = options::kSourceMemory | (m << options::kSwizzleShift);
        r.Source = input.data();
        r.Destination = output.data();
        r.SourceSize = r.DestinationSize = static_cast<uint32_t>(output.size());
        submit(r, S_OK);
        check(output == expected, "all component stripes reach the corresponding group");
    }

    const auto alloc = reinterpret_cast<BOOL(WINAPI*)(uint64_t,uint64_t*,uint64_t*)>(
        rt::shims::find("xmem.dll", "XMemAllocatePhysicalPages"));
    const auto reserve = reinterpret_cast<void*(WINAPI*)(void*,SIZE_T,DWORD,uint64_t,DWORD)>(
        rt::shims::find("xmem.dll", "XMemVirtualAlloc"));
    const auto map = reinterpret_cast<void*(WINAPI*)(void*,uint64_t,const uint64_t*)>(
        rt::shims::find("xmem.dll", "XMemMapPhysicalPages"));
    const auto free_pages = reinterpret_cast<BOOL(WINAPI*)(uint64_t,const uint64_t*)>(
        rt::shims::find("xmem.dll", "XMemFreePhysicalPages"));
    uint64_t pages[2]{}, count = 2;
    constexpr size_t page = 65536, offset = page-7;
    auto* window = static_cast<uint8_t*>(reserve(nullptr, 2*page, MEM_RESERVE | 0x20400000, 0x21, PAGE_READWRITE));
    const bool allocated = window && alloc(0x280, &count, pages);
    const bool mapped = allocated && map(window, 2, pages) == window;
    check(mapped, "two physical pages mapped");
    if (mapped) {
        ULONG_PTR scatter[] = {pages[1], 0};
        const auto& v = vectors[0];
        r = {};
        r.Options.AsUINT64 = options::kSourceMemory | options::kDestinationPages | options::kZlib |
                            (v.codec << options::kBcpackShift);
        r.DestinationPageArray = scatter;
        r.DestinationPageOffset = offset;
        r.DestinationSize = static_cast<uint32_t>(v.plain.size());
        r.Source = v.wrapped.data();
        r.SourceSize = static_cast<uint32_t>(v.wrapped.size());
        r.IntermediateSize = static_cast<uint32_t>(v.raw.size());
        submit(r, E_INVALIDARG);
        check(window[page+offset] == 0, "bad final page leaves the first untouched");
        bool overflow;
        leonida::title_texel_writes::take(overflow);
        q->EnqueueRequest(&r);
        q->EnqueueStatus(status, 0);
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        q->EnqueueSetEvent(event);
        scatter[1] = pages[0]; // resolved when the worker runs
        q->Submit();
        check(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0, "event follows the scatter write");
        CloseHandle(event);
        check_hr(status->GetHResult(0), S_OK, "page list resolved after enqueue");
        check(std::equal(v.plain.begin(), v.plain.begin()+7, window+page+offset) &&
              std::equal(v.plain.begin()+7, v.plain.end(), window), "scatter is visible through the mapped CPU view");
        const auto ranges = leonida::title_texel_writes::take(overflow);
        check(ranges.size() == 2 && ranges[0].first == reinterpret_cast<uintptr_t>(window+page+offset) &&
              ranges[1].first == reinterpret_cast<uintptr_t>(window), "refill sees both mapped ranges");
        std::vector<uint8_t> out(v.plain.size());
        r = {};
        r.Options.AsUINT64 = options::kSourceMemory | options::kSourcePages;
        r.SourcePageArray = scatter;
        r.SourcePageOffset = offset;
        r.SourceSize = r.DestinationSize = static_cast<uint32_t>(out.size());
        r.Destination = out.data();
        submit(r, S_OK);
        check(std::equal(v.plain.begin(), v.plain.end(), out.begin()), "gather follows the page list and offset");
        map(window, 2, nullptr);
    }
    if (allocated) free_pages(count, pages);
    if (window) VirtualFree(window, 0, MEM_RELEASE);
    status->Release();
    q->Release();
}

// A queue half full submits itself, but a read into physical pages and everything behind it wait for Submit: the
// title may fill the page list right before its own Submit. Until then the list names no page (the read would
// fail) or the page its slot named before (the data would land in a page that belongs to something else). The two
// cases of Marvisak/VIctory eddde73, then the ways a hold ends without Submit.
void test_page_reads_wait_for_submit(Env& env) {
    const auto alloc = reinterpret_cast<BOOL(WINAPI*)(uint64_t,uint64_t*,uint64_t*)>(
        rt::shims::find("xmem.dll", "XMemAllocatePhysicalPages"));
    const auto reserve = reinterpret_cast<void*(WINAPI*)(void*,SIZE_T,DWORD,uint64_t,DWORD)>(
        rt::shims::find("xmem.dll", "XMemVirtualAlloc"));
    const auto map = reinterpret_cast<void*(WINAPI*)(void*,uint64_t,const uint64_t*)>(
        rt::shims::find("xmem.dll", "XMemMapPhysicalPages"));
    const auto free_pages = reinterpret_cast<BOOL(WINAPI*)(uint64_t,const uint64_t*)>(
        rt::shims::find("xmem.dll", "XMemFreePhysicalPages"));
    uint64_t pages[2]{}, count = 2;
    constexpr size_t page = 65536, bytes = 4096;
    auto* window = static_cast<uint8_t*>(reserve(nullptr, 2*page, MEM_RESERVE | 0x20400000, 0x21, PAGE_READWRITE));
    const bool allocated = window && alloc(0x280, &count, pages);
    const bool mapped = allocated && map(window, 2, pages) == window;
    check(mapped, "two physical pages mapped for the held reads");
    if (!mapped) {
        if (allocated) free_pages(count, pages);
        if (window) VirtualFree(window, 0, MEM_RELEASE);
        return;
    }
    auto& h = leonida::dstorage::hold_stats();
    const auto plain = pattern(bytes, 9);
    const auto into_pages = [&](ULONG_PTR* list) {
        DSTORAGE_REQUEST r{};
        r.Options.AsUINT64 = options::kSourceMemory | options::kDestinationPages;
        r.DestinationPageArray = list;
        r.DestinationPageOffset = 0;
        r.Source = plain.data();
        r.SourceSize = r.DestinationSize = static_cast<uint32_t>(bytes);
        return r;
    };
    const auto into_buffer = [&](uint8_t* out) {
        DSTORAGE_REQUEST r{};
        r.Options.AsUINT64 = options::kSourceMemory;
        r.Destination = out;
        r.Source = plain.data();
        r.SourceSize = r.DestinationSize = 16;
        return r;
    };
    const auto zero = [&](const uint8_t* at, size_t n) { return std::all_of(at, at + n, [](uint8_t b) { return b == 0; }); };
    const auto holds_plain = [&](const uint8_t* at, size_t n) { return std::memcmp(at, plain.data(), n) == 0; };
    const auto errors = [&] {
        DSTORAGE_STATS stats{};
        env.factory->GetStats(&stats);
        return stats.ErrorCount;
    };
    constexpr int kHalf = 0x40;  // of a capacity of 0x80
    std::vector<uint8_t> outs(kHalf * 16);
    const auto reset = [&] {
        std::memset(window, 0, 2*page);
        std::fill(outs.begin(), outs.end(), uint8_t{0});
    };

    // The list still names the slot's previous page when the queue submits itself, the right one by Submit.
    {
        reset();
        auto* q = make_queue(env, 0x80, true, DSTORAGE_REQUEST_SOURCE_MEMORY, DSTORAGE_PRIORITY_REALTIME);
        auto* status = make_status(env, 1);
        const uint64_t self = h.self_submits.load(), held = h.held_reads.load(), failed = errors();
        ULONG_PTR list[] = {static_cast<ULONG_PTR>(pages[0])};
        DSTORAGE_REQUEST r = into_buffer(outs.data());
        q->EnqueueRequest(&r);
        r = into_pages(list);
        q->EnqueueRequest(&r);
        for (int i = 2; i < kHalf; ++i) {
            r = into_buffer(outs.data() + i * 16);
            q->EnqueueRequest(&r);  // the last one makes half the capacity
        }
        check(wait_until([&] { return holds_plain(outs.data(), 16); }),
              "the entry ahead of a read into pages starts when the queue submits itself");
        Sleep(50);  // time for an early read to land in the old page
        check(zero(window, bytes), "a self-submit does not write the page the list named before the title's Submit");
        check(zero(outs.data() + 2 * 16, 16), "entries behind a held read keep their place");
        check(h.self_submits.load() == self + 1 && h.held_reads.load() == held + 1,
              "one self-submit and one held read are counted");
        DSTORAGE_QUEUE_INFO info{};
        q->Query(&info);
        check(info.EmptySlotCount == 0x7f - (kHalf - 1),
              "held entries still take their slots");
        list[0] = static_cast<ULONG_PTR>(pages[1]);  // the title fills its page table, then submits
        q->EnqueueStatus(status, 0);
        check_hr(status->GetHResult(0), E_PENDING, "a status behind a held read is pending");
        q->Submit();
        check(wait_complete(status, 0), "the held entries run at Submit");
        check_hr(status->GetHResult(0), S_OK, "a read held for Submit succeeds");
        check(holds_plain(window + page, bytes), "the read lands in the page its list names at Submit");
        check(zero(window, bytes), "the page the list named before stays untouched");
        bool all = true;
        for (int i = 2; i < kHalf; ++i) all = all && holds_plain(outs.data() + i * 16, 16);
        check(all, "every entry behind the held read ran");
        check(errors() == failed, "no entry failed");
        status->Release();
        q->Release();
    }

    // The list is still empty when the queue submits itself: no failure, the read waits.
    {
        reset();
        auto* q = make_queue(env, 0x80, true, DSTORAGE_REQUEST_SOURCE_MEMORY, DSTORAGE_PRIORITY_REALTIME);
        auto* status = make_status(env, 1);
        const uint64_t page0 = h.page0_entries.load(), failed = errors();
        ULONG_PTR list[] = {0};
        DSTORAGE_REQUEST r = into_pages(list);
        q->EnqueueRequest(&r);
        for (int i = 1; i < kHalf; ++i) {
            r = into_buffer(outs.data() + i * 16);
            q->EnqueueRequest(&r);
        }
        Sleep(50);
        check(h.page0_entries.load() == page0 && errors() == failed, "an empty page list is not read before Submit");
        list[0] = static_cast<ULONG_PTR>(pages[0]);
        q->EnqueueStatus(status, 0);
        q->Submit();
        check(wait_complete(status, 0), "the read with a late page list completes");
        check_hr(status->GetHResult(0), S_OK, "a page list filled before Submit is written");
        check(holds_plain(window, bytes) && errors() == failed, "its bytes are in the page");
        status->Release();
        q->Release();
    }

    // A full queue: the title waits in an enqueue and cannot call Submit, so the held entries go.
    {
        reset();
        auto* q = make_queue(env, 0x80, true, DSTORAGE_REQUEST_SOURCE_MEMORY, DSTORAGE_PRIORITY_REALTIME);
        auto* status = make_status(env, 1);
        const uint64_t full = h.full_releases.load();
        ULONG_PTR list[] = {static_cast<ULONG_PTR>(pages[1])};
        constexpr int kMany = 200;
        std::vector<uint8_t> sink(kMany * 16);
        std::atomic<bool> done{false};
        std::thread producer([&] {
            DSTORAGE_REQUEST r = into_pages(list);
            q->EnqueueRequest(&r);
            for (int i = 0; i < kMany; ++i) {
                r = into_buffer(sink.data() + i * 16);
                q->EnqueueRequest(&r);
            }
            q->EnqueueStatus(status, 0);
            done.store(true);
        });
        check(wait_until([&] { return done.load(); }), "a full queue with held entries does not wait for Submit");
        producer.join();
        check(h.full_releases.load() == full + 1, "the full queue's release is counted");
        q->Submit();
        check(wait_complete(status, 0), "the full queue's entries complete");
        check_hr(status->GetHResult(0), S_OK, "they succeed");
        bool all = holds_plain(window + page, bytes);
        for (int i = 0; i < kMany; ++i) all = all && holds_plain(sink.data() + i * 16, 16);
        check(all, "every entry of the full queue has its bytes");
        status->Release();
        q->Release();
    }

    // A hold has a time limit: held entries go after LEONIDA_DSTORAGE_HOLD_MS without Submit, 2000 ms unless set.
    for (const wchar_t* limit : {static_cast<const wchar_t*>(nullptr), L"200"}) {
        reset();
        SetEnvironmentVariableW(L"LEONIDA_DSTORAGE_HOLD_MS", limit);
        auto* q = make_queue(env, 0x80, true, DSTORAGE_REQUEST_SOURCE_MEMORY, DSTORAGE_PRIORITY_REALTIME);
        SetEnvironmentVariableW(L"LEONIDA_DSTORAGE_HOLD_MS", nullptr);
        const uint64_t timed = h.timed_releases.load();
        ULONG_PTR list[] = {static_cast<ULONG_PTR>(pages[0])};
        const auto begin = std::chrono::steady_clock::now();
        DSTORAGE_REQUEST r = into_pages(list);
        q->EnqueueRequest(&r);
        for (int i = 1; i < kHalf; ++i) {
            r = into_buffer(outs.data() + i * 16);
            q->EnqueueRequest(&r);
        }
        check(zero(window, bytes), "a hold with a time limit starts as a hold");
        if (!limit) {
            Sleep(1000);
            check(zero(window, bytes), "the default limit is not over after a second");
        }
        check(wait_until([&] { return holds_plain(window, bytes); }),
              limit ? "LEONIDA_DSTORAGE_HOLD_MS ends a hold" : "a hold ends by itself without the variable");
        const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - begin).count();
        check(waited >= (limit ? 150 : 1900) && waited < (limit ? 2000 : 6000),
              limit ? "after about the 200 ms asked for" : "after about the default 2000 ms");
        check(h.timed_releases.load() == timed + 1, "the timed release is counted");
        q->Release();
    }

    // LEONIDA_DSTORAGE_HOLD_MS=0: no time limit, the entries wait for Submit.
    {
        reset();
        SetEnvironmentVariableW(L"LEONIDA_DSTORAGE_HOLD_MS", L"0");
        auto* q = make_queue(env, 0x80, true, DSTORAGE_REQUEST_SOURCE_MEMORY, DSTORAGE_PRIORITY_REALTIME);
        SetEnvironmentVariableW(L"LEONIDA_DSTORAGE_HOLD_MS", nullptr);
        auto* status = make_status(env, 1);
        const uint64_t timed = h.timed_releases.load();
        ULONG_PTR list[] = {static_cast<ULONG_PTR>(pages[0])};
        DSTORAGE_REQUEST r = into_pages(list);
        q->EnqueueRequest(&r);
        for (int i = 1; i < kHalf; ++i) {
            r = into_buffer(outs.data() + i * 16);
            q->EnqueueRequest(&r);
        }
        Sleep(2300);  // past the default limit
        check(zero(window, bytes) && h.timed_releases.load() == timed, "with 0 a hold outlasts the default limit");
        q->EnqueueStatus(status, 0);
        q->Submit();
        check(wait_complete(status, 0) && holds_plain(window, bytes), "and ends at Submit");
        status->Release();
        q->Release();
    }

    // LEONIDA_DSTORAGE_HOLD_PAGES=0: the self-submit takes everything, as before.
    {
        reset();
        SetEnvironmentVariableW(L"LEONIDA_DSTORAGE_HOLD_PAGES", L"0");
        auto* q = make_queue(env, 0x80, true, DSTORAGE_REQUEST_SOURCE_MEMORY, DSTORAGE_PRIORITY_REALTIME);
        SetEnvironmentVariableW(L"LEONIDA_DSTORAGE_HOLD_PAGES", nullptr);
        const uint64_t held = h.held_reads.load();
        ULONG_PTR list[] = {static_cast<ULONG_PTR>(pages[1])};
        DSTORAGE_REQUEST r = into_pages(list);
        q->EnqueueRequest(&r);
        for (int i = 1; i < kHalf; ++i) {
            r = into_buffer(outs.data() + i * 16);
            q->EnqueueRequest(&r);
        }
        check(wait_until([&] { return holds_plain(window + page, bytes); }),
              "LEONIDA_DSTORAGE_HOLD_PAGES=0 submits a read into pages with the rest");
        check(h.held_reads.load() == held, "and holds nothing");
        q->Release();
    }

    // A list that names page 0 when its data is written fails the entry and is counted.
    {
        auto* q = make_queue(env, 0x80, false, DSTORAGE_REQUEST_SOURCE_MEMORY, DSTORAGE_PRIORITY_REALTIME);
        auto* status = make_status(env, 1);
        const uint64_t page0 = h.page0_entries.load();
        ULONG_PTR list[] = {0};
        DSTORAGE_REQUEST r = into_pages(list);
        q->EnqueueRequest(&r);
        q->EnqueueStatus(status, 0);
        q->Submit();
        check(wait_complete(status, 0), "the read with an empty page list completes");
        check_hr(status->GetHResult(0), E_INVALIDARG, "a page list naming page 0 at Submit fails its entry");
        check(h.page0_entries.load() == page0 + 1, "the entry with page 0 in its list is counted");
        status->Release();
        q->Release();
    }

    map(window, 2, nullptr);
    free_pages(count, pages);
    VirtualFree(window, 0, MEM_RELEASE);
}

}  // namespace

int main() {
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    Env env;
    env.root = fs::path(temp) / (L"leonida_dstorage_test_" + std::to_wstring(GetCurrentProcessId()));
    env.game = env.root / L"game";
    env.overlay = env.root / L"overlay";
    std::error_code ec;
    fs::remove_all(env.root, ec);
    env.a_package = pattern(4096, 1);
    env.a_overlay = pattern(5000, 2);  // another size and other bytes than the package's copy
    env.b = pattern(10000, 3);
    env.big = pattern(3u << 20, 4);
    write_file(env.game / L"data" / L"a.bin", env.a_package);
    write_file(env.overlay / L"data" / L"a.bin", env.a_overlay);
    write_file(env.game / L"data" / L"b.bin", env.b);
    write_file(env.game / L"data" / L"big.bin", env.big);
    rt::fs::configure(env.game, env.overlay);
    const auto profile = test_profile();
    std::vector<uint8_t> packed_profile(8 + sizeof profile);
    std::memcpy(packed_profile.data(), "BCPHIST1", 8);
    std::memcpy(packed_profile.data() + 8, &profile, sizeof profile);
    write_file(env.root / L"shaders/bcpack/history.bcph", packed_profile);
    SetEnvironmentVariableW(L"LEONIDA_DSTORAGE_HISTORY", nullptr);
    SetEnvironmentVariableW(L"LEONIDA_SHADERS", (env.root / L"shaders/spirv").c_str());


    bool entropy_exact = true;
    for (int width = 1; width <= 8; ++width) {
        for (int q = 0; q < 16; ++q) {
            const auto& model = leonida::bcpack::detail::model_for(width, q);
            for (uint32_t slot = 0; slot < 65536; ++slot)
                entropy_exact &= model.symbol(slot, true) == model.symbol(slot, false);
        }
    }
    check(entropy_exact, "fast entropy lookup matches scalar division for every slot in all 128 models");
    test_layout_and_options();
    test_staging_budget();
    test_registry(env);
    test_factory_checks(env);
    test_open_paths(env);
    test_reads_and_errors(env);
    test_shadowed_file_destination(env);
    test_fence_order_and_events(env);
    test_cancel(env);
    test_auto_submit_and_full_queue(env);
    test_memory_queue(env);
    test_close_and_release(env);
    test_threads(env);
    test_iid_policy_events_and_cancel(env);
    test_stats_timing(env);
    test_stats(env);
    test_extended(env);
    test_page_reads_wait_for_submit(env);
    test_guarded_destinations(env);
    test_zlib(env);  // after test_stats, whose request totals it would change

    fs::remove_all(env.root, ec);
    std::printf("dstorage: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
