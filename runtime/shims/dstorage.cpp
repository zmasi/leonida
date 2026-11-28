// Xbox DirectStorage for a mapped title: DStorageGetFactory and the IDStorageFactoryX2 / IDStorageQueueX2 /
// IDStorageFileX / IDStorageStatusArrayX objects of the console API (dstorage_xs_abi.h).
//
// Served as dstorage_xs.dll, xcrdapi.dll and dstorage.dll!DStorageGetFactory; docs/directstorage.md has the evidence
// for those names, the console ABI and what each method does here. In short:
//   - OpenFile resolves kernel32.dll!CreateFileW through the shim registry, so a GAME: path goes through the
//     runtime's console path mapping and write overlay (file_system.cpp) exactly as the title's own CreateFileW.
//   - Each queue publishes its submitted entries in order: a request reads a file range or
//     copies a memory range into the destination, a status entry, fence signal or event runs when every entry before
//     it has. Completion is therefore in queue order, as the API guarantees.
//   - Compressed requests stage RFC 1950 inflate, BCPack BC block reconstruction and DirectStorage
//     macro-unshuffle before publishing the destination. Physical page numbers resolve in xmem at execution.
//   - A queue half full submits itself, but a read into physical pages and everything enqueued behind it wait for
//     the title's own Submit: the title may fill the page list only then (hold_pages below).
// Nothing here goes online or touches the GPU.
#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <map>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "../src/shader_library.h"  // leonida::zlib_inflate
#include "../src/title_page_watch.h"  // title_gpu_views, title_texel_writes, kernel_writes
#include "../src/bcpack.h"
#include "../src/guarded_copy.h"
#include "../src/shader_directory.h"
#include "../src/dstorage_swizzle.h"
#include "../src/dstorage_trace.h"
#include "../src/dstorage_hold.h"
#include "dstorage_xs_abi.h"
#include "dstorage_staging.h"
#include "startup_trace.h"
#include "xmem.h"
#include "shims.h"

using namespace dsx;

namespace {

// ---- Logging ------------------------------------------------------------------------------------------------------

// Process-lifetime state is allocated once and never freed: a worker thread may still run while statics are destroyed
// at exit.
std::mutex& log_mutex() {
    static auto* m = new std::mutex;
    return *m;
}

void log_once(const std::string& key, const std::string& line) {
    static auto* seen = new std::set<std::string>;
    std::lock_guard lock(log_mutex());
    if (!seen->insert(key).second) return;
    std::fprintf(stderr, "[rt] dstorage: %s\n", line.c_str());
}

std::string guid_text(const GUID& g) {
    char b[48];
    std::snprintf(b, sizeof b, "{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", g.Data1, g.Data2, g.Data3,
                  g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return b;
}

bool same_guid(const GUID& a, const GUID& b) { return std::memcmp(&a, &b, sizeof(GUID)) == 0; }

std::string hr_text(HRESULT hr) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08lx", static_cast<unsigned long>(hr));
    return b;
}

// ---- Interface ids ------------------------------------------------------------------------------------------------
//
// The X interfaces' ids are not public. A creating call (DStorageGetFactory, CreateQueue*, OpenFile,
// CreateStatusArray) accepts an id it has not seen for the kind of object it hands out and logs it the first time, so
// the first boot records the console's ids. It refuses an id seen for another kind, a PC DirectStorage id of another
// shape, and the system COM interfaces (IMarshal, IAgileObject, ...), which a DirectStorage object never implements.
// QueryInterface is stricter (docs/d3d12_vulkan.md: an unknown id is E_NOINTERFACE): it accepts IUnknown, the
// declared ids (dstorage_xs_abi.h) and the ids a creating call accepted for that kind, and logs anything else. This is
// a policy, not a fact; it lives here so a measured id list can replace it.

enum class Kind { Factory, Queue, File, StatusArray };

const char* kind_name(Kind k) {
    switch (k) {
        case Kind::Factory: return "factory";
        case Kind::Queue: return "queue";
        case Kind::File: return "file";
        case Kind::StatusArray: return "status array";
    }
    return "?";
}

// System interfaces: the COM base range {xxxxxxxx-0000-0000-C000-000000000046} (IClassFactory, IMarshal,
// IStdMarshalInfo, IExternalConnection, IMultiQI, IClientSecurity, IServerSecurity, IRpcOptions, IMarshal2,
// IWeakReferenceSource, IDispatch, IPersist, ...) and these others.
constexpr GUID kSystemInterfaceIds[] = {
    {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}},  // IAgileObject
    {0xecc8691b, 0xc1db, 0x4dc0, {0x85, 0x5e, 0x65, 0xf6, 0xc5, 0x51, 0xaf, 0x49}},  // INoMarshal
    {0x1c733a30, 0x2a1c, 0x11ce, {0xad, 0xe5, 0x00, 0xaa, 0x00, 0x44, 0x77, 0x3d}},  // ICallFactory
    {0xaf86e2e0, 0xb12d, 0x4c6a, {0x9c, 0x5a, 0xd7, 0xaa, 0x65, 0x10, 0x1e, 0x90}},  // IInspectable
    {0xdf0b3d60, 0x548f, 0x101b, {0x8e, 0x65, 0x08, 0x00, 0x2b, 0x2b, 0xd1, 0x19}},  // ISupportErrorInfo
    {0xb196b283, 0xbab4, 0x101a, {0xb6, 0x9c, 0x00, 0xaa, 0x00, 0x34, 0x1d, 0x07}},  // IProvideClassInfo
    {0xb196b284, 0xbab4, 0x101a, {0xb6, 0x9c, 0x00, 0xaa, 0x00, 0x34, 0x1d, 0x07}},  // IConnectionPointContainer
    {0xfc4801a3, 0x2ba9, 0x11cf, {0xa2, 0x29, 0x00, 0xaa, 0x00, 0x3d, 0x73, 0x52}},  // IObjectWithSite
};

bool system_interface(const GUID& g) {
    static const uint8_t kComBase[8] = {0xc0, 0, 0, 0, 0, 0, 0, 0x46};
    if (g.Data2 == 0 && g.Data3 == 0 && std::memcmp(g.Data4, kComBase, 8) == 0) return true;
    for (const GUID& s : kSystemInterfaceIds)
        if (same_guid(g, s)) return true;
    return false;
}

// The ids declared for a kind: the PC IDStorageStatusArray id, whose interface has the console one's shape (console
// D3D12 binaries reuse the PC SDK's ids). No other X interface id is known.
bool declared(const GUID& g, Kind kind) {
    for (const GUID& id : kStatusArrayIds)
        if (same_guid(g, id)) return kind == Kind::StatusArray;
    return false;
}

bool declared_for_any(const GUID& g) {
    for (const GUID& id : kStatusArrayIds)
        if (same_guid(g, id)) return true;
    return false;
}

class IidPolicy {
public:
    bool accept_creating(REFIID riid, Kind kind, const char* caller) {
        if (same_guid(riid, __uuidof(IUnknown)) || declared(riid, kind)) return true;
        if (system_interface(riid)) return refuse(riid, caller, "a system COM interface");
        if (declared_for_any(riid)) return refuse(riid, caller, "an id declared for another kind of object");
        for (const GUID& pc : kPcInterfaceIds)
            if (same_guid(riid, pc))
                return refuse(riid, caller, "a PC DirectStorage interface; only the console ABI is served");
        std::lock_guard lock(mutex_);
        for (const auto& [id, k] : learned_)
            if (same_guid(id, riid)) {
                if (k == kind) return true;
                return refuse(riid, caller, std::string("seen before as a ") + kind_name(k) + " interface");
            }
        learned_.push_back({riid, kind});
        log_once("learn" + guid_text(riid), std::string(caller) + " asked for " + guid_text(riid) + ": serving the " +
                                                kind_name(kind) + " interface");
        return true;
    }

    bool accept_query(REFIID riid, Kind kind, const char* caller) {
        if (same_guid(riid, __uuidof(IUnknown)) || declared(riid, kind)) return true;
        {
            std::lock_guard lock(mutex_);
            for (const auto& [id, k] : learned_)
                if (same_guid(id, riid) && k == kind) return true;
        }
        return refuse(riid, caller, std::string("not an id a ") + kind_name(kind) + " was created with");
    }

private:
    static bool refuse(REFIID riid, const char* caller, const std::string& why) {
        log_once(std::string("refuse") + caller + guid_text(riid),
                 std::string(caller) + " asked for " + guid_text(riid) + ": " + why + " (E_NOINTERFACE)");
        return false;
    }

    std::mutex mutex_;
    std::vector<std::pair<GUID, Kind>> learned_;
};

IidPolicy& iid_policy() {
    static auto* p = new IidPolicy;
    return *p;
}

// A bad argument to a factory or queue method, logged once per method and kind: a wrong vtable guess for the console
// interfaces would show up here first, as arguments that make no sense for the method.
HRESULT bad_call(const char* method, HRESULT hr, const char* what) {
    log_once(std::string("bad") + method + what,
             std::string(method) + ": " + what + " (" + hr_text(hr) +
                 "; logged once; a wrong vtable slot guess would show up like this)");
    return hr;
}

// ---- Statistics (IDStorageFactoryX1::GetStats) --------------------------------------------------------------------

struct Stats {
    std::mutex mutex;
    DSTORAGE_STATS s{};
    uint32_t live_queues = 0;  // all priorities; SetStagingBufferSize is refused while queues or files exist
};

Stats& stats() {
    static auto* s = new Stats;
    return *s;
}

DSTORAGE_PRIORITY_STATS& priority_stats(DSTORAGE_STATS& s, DSTORAGE_PRIORITY p) {
    switch (p) {
        case DSTORAGE_PRIORITY_LOW: return s.LowPriorityStats;
        case DSTORAGE_PRIORITY_HIGH: return s.HighPriorityStats;
        case DSTORAGE_PRIORITY_REALTIME: return s.RealtimePriorityStats;
        default: return s.NormalPriorityStats;
    }
}

void count_error() {
    std::lock_guard lock(stats().mutex);
    ++stats().s.ErrorCount;
}

// ---- Live objects ---------------------------------------------------------------------------------------------------
//
// A request names its file and a status entry its array by pointer. Only objects this shim made and that are still
// alive are used; anything else is an error in the request, never a call through a stranger's vtable.

std::mutex& live_mutex() {
    static auto* m = new std::mutex;
    return *m;
}
// One set per kind, so a status array passed as a file (or the other way round) is not found.
std::unordered_set<const void*>& live_files() {
    static auto* s = new std::unordered_set<const void*>;
    return *s;
}
std::unordered_set<const void*>& live_status_arrays() {
    static auto* s = new std::unordered_set<const void*>;
    return *s;
}

// AddRef only if the count has not reached zero (an object being released is not revived).
bool add_ref_if_live(std::atomic<ULONG>& refs) {
    ULONG r = refs.load();
    while (r != 0)
        if (refs.compare_exchange_weak(r, r + 1)) return true;
    return false;
}

// ---- Status arrays ------------------------------------------------------------------------------------------------

class StatusArray final : public IDStorageStatusArrayX {
public:
    StatusArray(uint32_t capacity, std::unique_ptr<std::atomic<HRESULT>[]> entries)
        : capacity_(capacity), entries_(std::move(entries)) {
        for (uint32_t i = 0; i < capacity_; ++i) entries_[i].store(S_OK, std::memory_order_relaxed);
        std::lock_guard lock(live_mutex());
        live_status_arrays().insert(static_cast<IDStorageStatusArrayX*>(this));
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (!iid_policy().accept_query(riid, Kind::StatusArray, "IDStorageStatusArrayX::QueryInterface"))
            return E_NOINTERFACE;
        AddRef();
        *ppv = static_cast<IDStorageStatusArrayX*>(this);
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r;
        {
            std::lock_guard lock(live_mutex());
            r = --refs_;
            if (r == 0) live_status_arrays().erase(static_cast<IDStorageStatusArrayX*>(this));
        }
        if (r == 0) delete this;
        return r;
    }

    // IsComplete is GetHResult(index) != E_PENDING (the reference says so); an index past the array is not pending.
    bool STDMETHODCALLTYPE IsComplete(UINT32 index) override { return GetHResult(index) != E_PENDING; }
    HRESULT STDMETHODCALLTYPE GetHResult(UINT32 index) override {
        if (index >= capacity_) return E_DSTORAGE_INDEX_BOUND;
        return entries_[index].load(std::memory_order_acquire);
    }

    uint32_t capacity() const { return capacity_; }
    void set_pending(uint32_t index) { entries_[index].store(E_PENDING, std::memory_order_release); }
    void complete(uint32_t index, HRESULT hr) { entries_[index].store(hr, std::memory_order_release); }
    std::atomic<ULONG>& refs() { return refs_; }

private:
    std::atomic<ULONG> refs_{1};
    const uint32_t capacity_;
    std::unique_ptr<std::atomic<HRESULT>[]> entries_;
};

// ---- Files --------------------------------------------------------------------------------------------------------

class File final : public IDStorageFileX {
public:
    File(HANDLE handle, std::wstring path) : handle_(handle), path_(std::move(path)) {
        {
            std::lock_guard lock(live_mutex());
            live_files().insert(static_cast<IDStorageFileX*>(this));
        }
        std::lock_guard lock(stats().mutex);
        auto& s = stats().s;
        ++s.CurrentOpenFileCount;
        ++s.CumulativeOpenFileCount;
        s.MaxOpenFileCount = std::max(s.MaxOpenFileCount, s.CurrentOpenFileCount);
    }
    ~File() {
        Close();
        std::lock_guard lock(stats().mutex);
        --stats().s.CurrentOpenFileCount;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (!iid_policy().accept_query(riid, Kind::File, "IDStorageFileX::QueryInterface")) return E_NOINTERFACE;
        AddRef();
        *ppv = static_cast<IDStorageFileX*>(this);
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r;
        {
            std::lock_guard lock(live_mutex());
            r = --refs_;
            if (r == 0) live_files().erase(static_cast<IDStorageFileX*>(this));
        }
        if (r == 0) delete this;
        return r;
    }

    // Waits for the submitted requests that read this file, then closes the handle. A request submitted later fails
    // with E_DSTORAGE_FILE_NOT_OPEN. Waiting only for submitted requests keeps a Close from blocking forever on a
    // request that sits unsubmitted in a queue with auto-submit disabled.
    void STDMETHODCALLTYPE Close() override {
        std::unique_lock lock(mutex_);
        closed_ = true;
        idle_.wait(lock, [&] { return pending_ == 0; });
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

    // A new handle the caller closes (GENERIC_READ, FILE_SHARE_READ, as the reference describes it). ReOpenFile,
    // not DuplicateHandle: a duplicate shares the file position with the shim's own reads, so the title's ReadFile
    // through it would start wherever the worker's last read ended.
    HANDLE STDMETHODCALLTYPE GetHandle() override {
        std::lock_guard lock(mutex_);
        if (closed_ || handle_ == INVALID_HANDLE_VALUE) {
            SetLastError(ERROR_INVALID_HANDLE);
            return INVALID_HANDLE_VALUE;
        }
        return ReOpenFile(handle_, GENERIC_READ, FILE_SHARE_READ, 0);
    }

    // Called when a request naming this file is submitted. False once the file is closed.
    bool add_pending() {
        std::lock_guard lock(mutex_);
        if (closed_) return false;
        ++pending_;
        return true;
    }
    void finish_pending() {
        std::lock_guard lock(mutex_);
        if (--pending_ == 0) idle_.notify_all();
    }
    // Valid while the caller holds a pending count: Close does not close the handle before it drops to zero.
    HANDLE handle() const { return handle_; }
    const std::wstring& path() const { return path_; }
    std::atomic<ULONG>& refs() { return refs_; }

private:
    std::atomic<ULONG> refs_{1};
    std::mutex mutex_;
    std::condition_variable idle_;
    HANDLE handle_;
    const std::wstring path_;
    uint32_t pending_ = 0;
    bool closed_ = false;
};

StatusArray* live_status_array(IDStorageStatusArrayX* p) {
    std::lock_guard lock(live_mutex());
    if (!p || !live_status_arrays().count(p)) return nullptr;
    auto* a = static_cast<StatusArray*>(p);
    return add_ref_if_live(a->refs()) ? a : nullptr;
}

File* live_file(IDStorageFileX* p) {
    std::lock_guard lock(live_mutex());
    if (!p || !live_files().count(p)) return nullptr;
    auto* f = static_cast<File*>(p);
    return add_ref_if_live(f->refs()) ? f : nullptr;
}

// ---- Reads --------------------------------------------------------------------------------------------------------

// memmove that turns an access violation into an error instead of ending the process. No C++ objects here: a
// function with __try cannot have any that need unwinding. The bytes move in leonida::guarded::move, not the CRT's
// memmove: under Wine a fault inside ucrtbase never reaches this __except (src/guarded_copy.h).
bool guarded_move(void* destination, const void* source, size_t size) {
    __try {
        leonida::guarded::move(destination, source, size);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                  : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

// Reads one byte of every 4 KiB page of the range from user mode. Title memory that xmem maps from physical pages is
// committed by a vectored handler on first touch; a kernel write (ReadFile) raises no user-mode fault and fails with
// ERROR_NOACCESS instead. No C++ objects here: the function has __try.
void touch_pages(void* buffer, size_t size) {
    const uintptr_t start = reinterpret_cast<uintptr_t>(buffer);
    for (uintptr_t page = start & ~uintptr_t{0xfff}; page < start + size; page += 0x1000) {
        __try {
            (void)*reinterpret_cast<volatile const unsigned char*>(page < start ? start : page);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
}

// A copy into a request's destination, title memory: through kernel_writes like the file reads into it, so a page the
// D3D12 layer keeps read-only (a slot-133 shadow's guard, a geometry tile's) is opened and pinned first, and the GPU
// copies of it take the bytes at the next submission. A copy that faults anyway is retried once after the range is
// reopened (title_page_watch.h); false only when it still faults.
bool title_write(void* destination, const void* source, size_t size) {
    return leonida::kernel_writes::write(destination, source, size);
}

HRESULT read_error(DWORD error) {
    switch (error) {
        case ERROR_NOACCESS:
        case ERROR_INVALID_USER_BUFFER: return E_DSTORAGE_ACCESS_VIOLATION;
        case ERROR_HANDLE_EOF: return E_DSTORAGE_END_OF_FILE;
        default: return HRESULT_FROM_WIN32(error);
    }
}

bool extended_enabled() {
    static const bool enabled = [] {
        char value[2]{};
        return GetEnvironmentVariableA("LEONIDA_DSTORAGE_EXTENDED", value, sizeof value) != 1 || value[0] != '0';
    }();
    return enabled;
}

// Calibrated predictor values stay in a local sidecar. Absent or invalid files retain the built-in codec policy.
const leonida::bcpack::Calibration* bcpack_history() {
    static const auto profile = []() -> std::unique_ptr<leonida::bcpack::Calibration> {
        std::wstring explicit_path(32768, L'\0');
        const DWORD n = GetEnvironmentVariableW(L"LEONIDA_DSTORAGE_HISTORY", explicit_path.data(), DWORD(explicit_path.size()));
        if (n >= explicit_path.size()) return {};
        std::filesystem::path path;
        if (n) {
            explicit_path.resize(n);
            if (explicit_path == L"0") return {};
            path = explicit_path;
        } else {
            auto shaders = leonida::shader_paths::configured();
            if (shaders.empty()) return {};
            if (!shaders.has_filename()) shaders = shaders.parent_path();
            path = shaders.parent_path() / "bcpack" / "history.bcph";
        }
        FILE* raw = nullptr;
        if (_wfopen_s(&raw, path.c_str(), L"rb") || !raw) {
            log_once("history-open", "could not open the local BCPack predictor profile");
            return {};
        }
        std::unique_ptr<FILE, decltype(&std::fclose)> file(raw, &std::fclose);
        auto table = std::make_unique<leonida::bcpack::Calibration>();
        char magic[8];
        if (std::fread(magic, 1, sizeof magic, raw) != sizeof magic || std::memcmp(magic, "BCPHIST1", 8) ||
            std::fread(table.get(), 1, sizeof *table, raw) != sizeof *table || std::fgetc(raw) != EOF || std::ferror(raw)) {
            log_once("history-format", "invalid local BCPack predictor profile");
            return {};
        }
        unsigned bc6_ep = 0, bc6_ix = 0, bc7_ep = 0, bc7_ix = 0;
        for (const auto& row : table->bc6) {
            bc6_ep += row.ep_class != 0;
            bc6_ix += row.ix_class != 0;
        }
        for (const auto& row : table->bc7) {
            bc7_ep += unsigned(std::count_if(row.ep_bits.begin(), row.ep_bits.end(), [](auto v) {return v != 0;}));
            bc7_ix += unsigned(std::count_if(row.ix_bits.begin(), row.ix_bits.end(), [](auto v) {return v != 0;}));
        }
        log_once("history-ok", std::string("loaded ") + (n ? "explicit" : "default") + " BCPack profile; BC6H endpoint/index rows " +
                 std::to_string(bc6_ep) + "/256 " + std::to_string(bc6_ix) + "/256; BC7 endpoint/index fields " +
                 std::to_string(bc7_ep) + "/6144 " + std::to_string(bc7_ix) + "/8192");
        return table;
    }();
    return profile.get();
}

// Reads exactly "size" bytes at "offset" of a synchronous handle into "destination". Through kernel_writes, as the
// title's own reads: the destination is title memory, which a kernel write cannot fault in (pages the D3D12 layer
// keeps read-only, xmem pages that wait for their first touch), title_page_watch.h.
enum class ReadTarget { Title, Staging };
HRESULT read_file_range(HANDLE handle, uint64_t offset, void* destination, uint32_t size, ReadTarget target) {
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(handle, &length)) return HRESULT_FROM_WIN32(GetLastError());
    const uint64_t file_size = static_cast<uint64_t>(length.QuadPart);
    if (offset > file_size || size > file_size - offset) return E_DSTORAGE_END_OF_FILE;
    auto* out = static_cast<uint8_t*>(destination);
    uint32_t done = 0;
    while (done < size) {
        OVERLAPPED at{};
        const uint64_t position = offset + done;
        at.Offset = static_cast<DWORD>(position);
        at.OffsetHigh = static_cast<DWORD>(position >> 32);
        DWORD got = 0;
        // A private vector cannot be mapped or write-protected by the title.
        const auto read = [&] {
            return target == ReadTarget::Title
                ? leonida::kernel_writes::read_file(handle, out + done, size - done, &got, &at)
                : ::ReadFile(handle, out + done, size - done, &got, &at);
        };
        if (!read()) {
            const DWORD error = GetLastError();
            if (error != ERROR_NOACCESS) return read_error(error);
            // Destination pages not committed yet (see touch_pages): commit them from user mode and read again once.
            touch_pages(out + done, size - done);
            at = OVERLAPPED{};
            at.Offset = static_cast<DWORD>(position);
            at.OffsetHigh = static_cast<DWORD>(position >> 32);
            if (!read()) return read_error(GetLastError());
        }
        if (got == 0) return E_DSTORAGE_END_OF_FILE;  // the file shrank since the size check
        done += got;
    }
    return S_OK;
}

// ---- Reads into physical pages wait for the title's Submit -------------------------------------------------------
//
// A request into physical pages names them through a pointer into the title's own page-number table, and a title may
// fill that table for a batch only in its submit path, right before Submit. A queue that submits itself at half its
// capacity then starts such a read early: it finds page 0 and fails, or finds the number the slot held before and
// writes the data into a page that now belongs to something else, with no error at all. So a self-submit stops at
// the first read into physical pages: that read and every entry behind it (the queue completes in order) wait for
// the title's Submit. A full queue still submits everything, since the title cannot call Submit while it waits in an
// enqueue, and Close does as before. The idea is from VIctory (Marvisak/VIctory eddde73, carried in
// fxceless/victory-fork 79cfdd9); the code here is written anew for this queue.
//
// LEONIDA_DSTORAGE_HOLD_PAGES=0 restores the old self-submit of everything. A hold has a time limit, so that a
// title path that never calls Submit and counted on the self-submit cannot stall for good: held entries that have
// waited LEONIDA_DSTORAGE_HOLD_MS milliseconds (2000 unless set) are submitted, page lists as they are, and counted
// as a timed release. 0 is no limit: they wait for Submit, however long. Both are read when a queue is created.
constexpr uint32_t kDefaultHoldMs = 2000;

uint32_t environment_number(const wchar_t* name, uint32_t fallback) {
    wchar_t value[16]{};
    const DWORD n = GetEnvironmentVariableW(name, value, 16);
    if (n == 0 || n >= 16) return fallback;
    wchar_t* end = nullptr;
    const unsigned long parsed = std::wcstoul(value, &end, 10);
    return end && *end == 0 ? static_cast<uint32_t>(parsed) : fallback;
}

// One line with every counter, at each power of two of the counter that moved: countable in a truncated log
// without a line per event.
void log_hold(const char* event, uint64_t n, unsigned capacity, size_t held_now) {
    if (n & (n - 1)) return;
    const auto& h = leonida::dstorage::hold_stats();
    std::lock_guard lock(log_mutex());
    std::fprintf(stderr, "[rt] ds-hold event=%s self_submits=%llu held_reads=%llu full_releases=%llu "
                         "timed_releases=%llu page0_entries=%llu capacity=%u held_now=%zu\n", event,
                 static_cast<unsigned long long>(h.self_submits.load()),
                 static_cast<unsigned long long>(h.held_reads.load()),
                 static_cast<unsigned long long>(h.full_releases.load()),
                 static_cast<unsigned long long>(h.timed_releases.load()),
                 static_cast<unsigned long long>(h.page0_entries.load()), capacity, held_now);
}

// ---- Queues -------------------------------------------------------------------------------------------------------

// Only sampled when request tracing is enabled. Times include work done by the file/decode worker.
struct Timing {
    using Clock = std::chrono::steady_clock;
    long long read{}, inflate{}, bcpack{}, copy{}, unshuffle{};
    static Clock::time_point start() {
        return leonida::dstorage::trace_enabled() ? Clock::now() : Clock::time_point{};
    }
    static long long elapsed(Clock::time_point begin) {
        return leonida::dstorage::trace_enabled()
            ? std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - begin).count() : 0;
    }
};
struct Entry {
    Timing timing;
    const char* reason = "none";
    enum class Type { Request, Status, Signal, Event } type = Type::Request;
    DSTORAGE_REQUEST request{};
    File* file = nullptr;          // a reference, for a file request naming a live file
    bool file_counted = false;     // the file's pending count was raised at submit
    bool cancelled = false;        // CancelRequestsWithTag matched it before it started
    bool counted = false;          // a request the title passed: counted in the statistics when it is submitted
    HRESULT enqueue_error = S_OK;  // found at enqueue: reported when the entry is reached, in order
    StatusArray* status = nullptr;  // a reference
    IDStorageStatusArrayX* status_as_given = nullptr;
    uint32_t index = 0;
    ID3D12Fence* fence = nullptr;  // a reference
    uint64_t value = 0;
    HANDLE event = nullptr;  // a duplicate the entry owns
};

class Queue final : public IDStorageQueueX2 {
public:
    explicit Queue(const DSTORAGE_QUEUE_DESC1& desc)
        : desc_(desc),
          hold_pages_(environment_number(L"LEONIDA_DSTORAGE_HOLD_PAGES", 1) != 0),
          hold_release_ms_(environment_number(L"LEONIDA_DSTORAGE_HOLD_MS", kDefaultHoldMs)) {
        error_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        clear_record_locked();
        std::lock_guard lock(stats().mutex);
        auto& p = priority_stats(stats().s, desc_.Priority);
        ++p.CurrentQueueCount;
        ++p.CumulativeQueueCount;
        p.MaxQueueCount = std::max(p.MaxQueueCount, p.CurrentQueueCount);
        ++stats().live_queues;
    }
    // Separate from the constructor so a failure to start the thread can be reported.
    bool start() {
        if (!error_event_) return false;
        wchar_t workers[4]{};
        const DWORD n = GetEnvironmentVariableW(L"LEONIDA_DSTORAGE_WORKERS", workers, 4);
        if (!(n == 1 && (workers[0] == L'1' || workers[0] == L'0')))
            preparation_work_ = CreateThreadpoolWork(prepare_in_background, this, nullptr);
        try {
            worker_ = std::thread([this] { run(); });
        } catch (...) {
            return false;
        }
        return true;
    }
    ~Queue() {
        Close();
        if (preparation_work_) CloseThreadpoolWork(preparation_work_);
        if (error_event_) CloseHandle(error_event_);
        std::lock_guard lock(stats().mutex);
        --priority_stats(stats().s, desc_.Priority).CurrentQueueCount;
        --stats().live_queues;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (!iid_policy().accept_query(riid, Kind::Queue, "IDStorageQueueX::QueryInterface")) return E_NOINTERFACE;
        AddRef();
        *ppv = static_cast<IDStorageQueueX2*>(this);
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    // The last Release closes the queue, which runs everything enqueued first.
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --refs_;
        if (r == 0) delete this;
        return r;
    }

    void STDMETHODCALLTYPE EnqueueRequest(const DSTORAGE_REQUEST* request) override {
        Entry e;
        e.type = Entry::Type::Request;
        if (!request) {
            e.enqueue_error = bad_call("IDStorageQueueX::EnqueueRequest", E_INVALIDARG, "null request");
        } else {
            e.request = *request;  // the title may reuse its structure as soon as this returns
            const uint64_t o = e.request.Options.AsUINT64;
            if (options::source_type(o) == DSTORAGE_REQUEST_SOURCE_FILE &&
                desc_.SourceType == DSTORAGE_REQUEST_SOURCE_FILE && e.request.File) {
                e.file = live_file(e.request.File);
                if (!e.file) e.enqueue_error = E_DSTORAGE_INVALID_FILE_HANDLE;
            }
            e.counted = true;  // TotalRequestCount is "submitted requests" (DSTORAGE_PRIORITY_STATS): see submit_locked
        }
        enqueue(std::move(e));
    }

    void STDMETHODCALLTYPE EnqueueStatus(IDStorageStatusArrayX* status_array, UINT32 index) override {
        Entry e;
        e.type = Entry::Type::Status;
        e.status_as_given = status_array;
        e.index = index;
        e.status = live_status_array(status_array);
        if (!e.status) {
            e.enqueue_error = E_DSTORAGE_INVALID_STATUS_ARRAY;
        } else if (index >= e.status->capacity()) {
            e.enqueue_error = E_DSTORAGE_INDEX_BOUND;
        } else {
            e.status->set_pending(index);  // polling after this call sees E_PENDING until the entry is reached
        }
        {
            std::lock_guard lock(stats().mutex);
            ++priority_stats(stats().s, desc_.Priority).TotalStatusCount;
        }
        enqueue(std::move(e));
    }

    void STDMETHODCALLTYPE EnqueueSignal(ID3D12Fence* fence, UINT64 value) override {
        Entry e;
        e.type = Entry::Type::Signal;
        e.fence = fence;
        e.value = value;
        if (fence) {
            fence->AddRef();
        } else {
            e.enqueue_error = E_DSTORAGE_INVALID_FENCE;
        }
        {
            std::lock_guard lock(stats().mutex);
            ++priority_stats(stats().s, desc_.Priority).TotalFenceCount;
        }
        enqueue(std::move(e));
    }

    // The handle is duplicated here and the duplicate closed once it has been set (or dropped), so an event the
    // title closes, and whose handle value is then reused for another object, is never the one set.
    void STDMETHODCALLTYPE EnqueueSetEvent(HANDLE handle) override {
        Entry e;
        e.type = Entry::Type::Event;
        if (!handle) {
            e.enqueue_error = bad_call("IDStorageQueueX1::EnqueueSetEvent", HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE),
                                       "null event handle");
        } else if (!DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &e.event, 0, FALSE,
                                    DUPLICATE_SAME_ACCESS)) {
            e.event = nullptr;
            e.enqueue_error = HRESULT_FROM_WIN32(GetLastError());
        }
        enqueue(std::move(e));
    }

    void STDMETHODCALLTYPE Submit() override {
        std::lock_guard lock(mutex_);
        submit_locked();
    }

    // Marks the requests enqueued before this call that have not started. A cancelled request completes as a success
    // without being read (the overview: the title may no longer use its result).
    void STDMETHODCALLTYPE CancelRequestsWithTag(UINT64 mask, UINT64 value) override {
        std::lock_guard lock(mutex_);
        for (Entry& e : entries_)
            if (e.type == Entry::Type::Request && (e.request.CancellationTag & mask) == value) e.cancelled = true;
    }

    // Submits what is enqueued, waits until the worker has run all of it and stops the worker. Later enqueues are
    // dropped and recorded as E_DSTORAGE_QUEUE_CLOSED.
    void STDMETHODCALLTYPE Close() override {
        std::lock_guard close_lock(close_mutex_);
        {
            std::unique_lock lock(mutex_);
            if (closed_) return;
            closing_ = true;
            submit_locked();
            space_.notify_all();  // an enqueue waiting for space gives up (the queue is closing)
            idle_.wait(lock, [&] { return entries_.empty() && !in_flight_; });
            closed_ = true;
            work_.notify_all();
        }
        if (worker_.joinable()) worker_.join();
    }

    HANDLE STDMETHODCALLTYPE GetErrorEvent() override {
        HANDLE dup = nullptr;
        if (!error_event_ || !DuplicateHandle(GetCurrentProcess(), error_event_, GetCurrentProcess(), &dup, 0, FALSE,
                                              DUPLICATE_SAME_ACCESS))
            return nullptr;
        return dup;  // the reference says the title closes it
    }

    void STDMETHODCALLTYPE RetrieveErrorRecord(DSTORAGE_ERROR_RECORD* record) override {
        if (!record) {
            bad_call("IDStorageQueueX::RetrieveErrorRecord", E_POINTER, "null record");
            return;
        }
        std::lock_guard lock(error_mutex_);
        *record = record_;
        clear_record_locked();
        if (error_event_) ResetEvent(error_event_);
    }

    void STDMETHODCALLTYPE Query(DSTORAGE_QUEUE_INFO* info) override {
        if (!info) {
            bad_call("IDStorageQueueX::Query", E_POINTER, "null info");
            return;
        }
        DSTORAGE_QUEUE_INFO1 full{};
        Query1(&full);
        info->Desc.SourceType = full.Desc.SourceType;
        info->Desc.Capacity = full.Desc.Capacity;
        info->Desc.Priority = full.Desc.Priority;
        info->Desc.Name = full.Desc.Name;
        info->EmptySlotCount = full.EmptySlotCount;
        info->RequestCountUntilAutoSubmit = full.RequestCountUntilAutoSubmit;
    }

    // An empty queue has capacity - 1 empty slots (one slot tells empty from full). The automatic submit happens when
    // the entries enqueued since the last submit reach half the capacity; with it disabled the count is 0.
    void STDMETHODCALLTYPE Query1(DSTORAGE_QUEUE_INFO1* info) override {
        if (!info) {
            bad_call("IDStorageQueueX2::Query1", E_POINTER, "null info");
            return;
        }
        std::lock_guard lock(mutex_);
        info->Desc = desc_;
        info->EmptySlotCount = static_cast<UINT16>(usable() - used_locked());
        const size_t unsubmitted = entries_.size() - submitted_;
        info->RequestCountUntilAutoSubmit =
            auto_submit() ? static_cast<UINT16>(half() > unsubmitted ? half() - unsubmitted : 0) : 0;
    }

private:
    size_t usable() const { return desc_.Capacity - 1u; }
    size_t half() const { return desc_.Capacity / 2u; }
    bool auto_submit() const { return !desc_.Options.AutoSubmitDisabled; }
    size_t used_locked() const { return entries_.size() + in_flight_; }

    // A read into physical pages: its page list may not be filled before the title's Submit (hold_pages above).
    static bool into_pages(const Entry& e) {
        return e.type == Entry::Type::Request && (e.request.Options.AsUINT64 & options::kDestinationPages) != 0;
    }

    // The queue has reached half its capacity and holds nothing yet. Submits up to the first read into physical
    // pages, which waits with everything behind it for the title's Submit.
    void self_submit_locked() {
        auto& h = leonida::dstorage::hold_stats();
        if (!hold_pages_) {
            log_hold("self-submit", ++h.self_submits, desc_.Capacity, 0);
            submit_locked();
            return;
        }
        size_t end = submitted_;
        while (end < entries_.size() && !into_pages(entries_[end])) ++end;
        if (end < entries_.size()) {
            uint64_t reads = 0;
            for (size_t i = end; i < entries_.size(); ++i) reads += into_pages(entries_[i]);
            h.held_reads += reads;
            holding_ = true;
            hold_warned_ = false;
            held_since_ = std::chrono::steady_clock::now();
        }
        const size_t held_now = entries_.size() - end;
        const bool hold = holding_;  // submit_locked ends a hold when it submits everything
        submit_locked(end);
        holding_ = hold;
        if (holding_) work_.notify_one();  // the worker starts timing the hold
        log_hold("self-submit", ++h.self_submits, desc_.Capacity, held_now);
    }

    void enqueue(Entry&& e) {
        {
            std::unique_lock lock(mutex_);
            // A full queue waits for the worker to free a slot. With auto-submit on, half the queue was submitted
            // by then, except what is held for the title's Submit: the title cannot make that call while it waits
            // here, so a full queue submits the held entries too, page lists as they are. With auto-submit off,
            // only a Submit from another thread frees a slot (as the reference warns).
            space_.wait(lock, [&] {
                if (closing_ || used_locked() < usable()) return true;
                if (holding_) {
                    const size_t held_now = entries_.size() - submitted_;
                    submit_locked();
                    log_hold("full-queue", ++leonida::dstorage::hold_stats().full_releases, desc_.Capacity, held_now);
                    log_once("full-hold", "a full queue submitted reads into physical pages before the title's "
                                          "Submit (their page lists may not be filled yet)");
                }
                return false;
            });
            if (!closing_) {
                entries_.push_back(std::move(e));
                if (holding_) {
                    // Behind a held read: it waits with it, whatever it is. No second self-submit until the hold ends.
                    if (into_pages(entries_.back())) ++leonida::dstorage::hold_stats().held_reads;
                } else if (auto_submit() && entries_.size() - submitted_ >= half()) {
                    self_submit_locked();
                }
                return;
            }
        }
        DSTORAGE_ERROR_FIRST_FAILURE f{};
        f.HResult = E_DSTORAGE_QUEUE_CLOSED;
        describe(e, f);
        record_failure(f);
        log_once("closed", "an entry was enqueued on a closed queue (E_DSTORAGE_QUEUE_CLOSED); it never completes");
        release_references(e);
    }

    // Submits the entries before "end". Without one it submits all of them and ends a hold: the title's Submit, a
    // full queue, Close, a hold that ran out of time.
    void submit_locked(size_t end = SIZE_MAX) {
        if (end >= entries_.size()) {
            end = entries_.size();
            holding_ = false;
        }
        uint32_t requests = 0;
        uint64_t source_bytes = 0, destination_bytes = 0;
        for (size_t i = submitted_; i < end; ++i) {
            Entry& e = entries_[i];
            if (e.file) e.file_counted = e.file->add_pending();
            if (e.counted) {
                ++requests;
                source_bytes += e.request.SourceSize;
                destination_bytes += e.request.DestinationSize;
            }
        }
        if (requests) {
            std::lock_guard stats_lock(stats().mutex);
            auto& p = priority_stats(stats().s, desc_.Priority);
            p.TotalRequestCount += requests;
            p.TotalRequestSourceByteCount += source_bytes;
            p.TotalRequestDestinationByteCount += destination_bytes;
        }
        if (end > submitted_) {
            submitted_ = end;
            work_.notify_one();
        }
    }

    // How long the worker sleeps before it looks at a hold again: until the time limit runs out, a quarter of a
    // second at most (and exactly that without a limit, for the five-second line).
    std::chrono::milliseconds hold_wait_locked() const {
        long long ms = 250;
        if (hold_release_ms_) {
            const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - held_since_).count();
            ms = std::clamp<long long>(static_cast<long long>(hold_release_ms_) - waited, 1, 250);
        }
        return std::chrono::milliseconds(ms);
    }

    // On the worker while entries are held: a hold with a time limit ends when it runs out (a timed release), and
    // one without says once that it has lasted five seconds (a title that never calls Submit would show up as this
    // line and nothing after it).
    void hold_tick_locked() {
        if (!holding_) return;
        const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - held_since_).count();
        const size_t held_now = entries_.size() - submitted_;
        if (hold_release_ms_ && waited >= static_cast<long long>(hold_release_ms_)) {
            submit_locked();
            log_hold("timed", ++leonida::dstorage::hold_stats().timed_releases, desc_.Capacity, held_now);
            log_once("timed-hold", "timed release: reads into physical pages held for the title's Submit were "
                                   "submitted after LEONIDA_DSTORAGE_HOLD_MS without one (their page lists may "
                                   "not be filled; the ds-hold line counts these)");
        } else if (waited >= 5000 && !hold_warned_) {
            hold_warned_ = true;
            std::lock_guard lock(log_mutex());
            std::fprintf(stderr, "[rt] dstorage: %zu entries behind a read into physical pages have waited %lld ms "
                                 "for the title's Submit (queue %p, capacity %u; LEONIDA_DSTORAGE_HOLD_MS=<n> "
                                 "submits them after n ms, LEONIDA_DSTORAGE_HOLD_PAGES=0 never holds)\n",
                         held_now, static_cast<long long>(waited), static_cast<void*>(this),
                         unsigned(desc_.Capacity));
        }
    }

    struct Prepared {
        Entry& entry;
        HRESULT result = S_OK;
        std::vector<uint8_t> bytes;
    };
    // Each queue publishes in order. Only an adjacent pair of file transforms can
    // overlap. Memory inputs, direct reads, page resolution and notifications remain barriers.
    static bool can_prepare(const Entry& e) {
        const auto& r = e.request;
        const auto o = r.Options.AsUINT64;
        constexpr uint64_t budget = 16ull << 20;
        return e.type == Entry::Type::Request && !e.cancelled && SUCCEEDED(e.enqueue_error) && e.file &&
               (options::zlib(o) || options::bcpack(o) || options::swizzle(o)) &&
               leonida::dstorage::staging_bytes(r) <= budget;
    }

    static void CALLBACK prepare_in_background(PTP_CALLBACK_INSTANCE, void* context, PTP_WORK) {
        auto& q = *static_cast<Queue*>(context);
        q.set_pending(+1);
        auto& job = *q.helper_job_;
        job.result = q.validate_request(job.entry);
        if (SUCCEEDED(job.result)) job.result = q.prepare(job.entry, job.bytes);
        else job.entry.reason = "request-validation";
    }

    void run() {
        for (;;) {
            Entry head, next;
            leonida::dstorage::StagingPool::Lease reservation(leonida::dstorage::staging_pool());
            Prepared prepared{next, S_OK, {}};
            bool paired = false;
            {
                std::unique_lock lock(mutex_);
                while (!(submitted_ > 0 || closed_)) {
                    if (!holding_) work_.wait(lock);
                    else if (work_.wait_for(lock, hold_wait_locked()) == std::cv_status::timeout)
                        hold_tick_locked();
                }
                if (submitted_ == 0) return;
                head = std::move(entries_.front());
                entries_.pop_front();
                --submitted_;
                paired = preparation_work_ && submitted_ && can_prepare(head) && can_prepare(entries_.front()) &&
                         reservation.try_pair(leonida::dstorage::staging_bytes(head.request) +
                                              leonida::dstorage::staging_bytes(entries_.front().request));
                if (paired) {
                    next = std::move(entries_.front());
                    entries_.pop_front();
                    --submitted_;
                    helper_job_ = &prepared;
                    SubmitThreadpoolWork(preparation_work_);
                }
                in_flight_ = paired ? 2 : 1;
            }
            process(head, nullptr, paired);
            release_references(head);
            if (paired) {
                WaitForThreadpoolWorkCallbacks(preparation_work_, FALSE);
                process(next, &prepared);
                release_references(next);
                helper_job_ = nullptr;
            }
            {
                std::lock_guard lock(mutex_);
                in_flight_ = 0;
                space_.notify_all();
                if (entries_.empty()) idle_.notify_all();
            }
        }
    }

    // Runs on the worker. since_notify_ is the first failure since the last status entry or fence signal: the
    // HRESULT a status entry reports.
    void process(Entry& e, Prepared* prepared, bool pair_reserved = false) {
        HRESULT hr = e.enqueue_error;
        if (FAILED(hr)) e.reason = "enqueue";
        const auto begin = Timing::start();
        long long notify_us = 0;
        switch (e.type) {
            case Entry::Type::Request:
                // A cancelled request is a success whatever else is wrong with it (the GDK: not counted as a failure).
                if (e.cancelled) {
                    hr = S_OK;
                } else if (SUCCEEDED(hr)) {
                    // Pending: processing has started and not completed (DSTORAGE_PRIORITY_STATS).
                    if (prepared) {
                        hr = prepared->result;
                        if (SUCCEEDED(hr)) hr = publish(e, prepared->bytes);
                    } else {
                        set_pending(+1);
                        hr = execute_request(e, pair_reserved);
                    }
                    set_pending(-1);
                }
                if (FAILED(hr) || leonida::dstorage::trace_enabled()) {
                    const auto& r = e.request;
                    const auto o = r.Options.AsUINT64;
                    const bool pages = (o & options::kDestinationPages) != 0;
                    std::fprintf(stderr, "[rt] ds-request queue=%p options=%llx zlib=%d bc=%llu sw=%llu "
                                 "dest=%s address=%p file=%ls offset=%llu src=%u mid=%u dst=%u cancelled=%d hr=%08lx "
                                 "tick_ms=%llu page_offset=%u read_us=%lld inflate_us=%lld bc_us=%lld copy_us=%lld sw_us=%lld paired=%d reason=%s priority=%d\n",
                                 static_cast<void*>(this), o, int(options::zlib(o)), options::bcpack(o),
                                 options::swizzle(o), pages ? "pages" : "buffer", r.Destination,
                                 e.file ? e.file->path().c_str() : L"-", e.file ? r.FileOffset : 0ull,
                                 r.SourceSize, r.IntermediateSize, r.DestinationSize, int(e.cancelled),
                                 static_cast<unsigned long>(hr), GetTickCount64(),
                                 pages ? unsigned(r.DestinationPageOffset) : 0u, e.timing.read, e.timing.inflate,
                                 e.timing.bcpack, e.timing.copy, e.timing.unshuffle, int(prepared != nullptr), e.reason, int(desc_.Priority));
                    if (hr == E_DSTORAGE_ACCESS_VIOLATION) {
                        const void* cpu = pages ? r.DestinationPageArray : cpu_destination(r);
                        MEMORY_BASIC_INFORMATION info{};
                        VirtualQuery(cpu, &info, sizeof(info));
                        std::fprintf(stderr, "[rt] ds-access queue=%p dest=%p cpu=%p paired=%llx base=%p "
                                     "region=%zu state=%lx protect=%lx type=%lx\n", static_cast<void*>(this),
                                     r.Destination, cpu, static_cast<unsigned long long>(pages ? 0 :
                                     leonida::title_gpu_views::other_view(reinterpret_cast<uintptr_t>(cpu))),
                                     info.BaseAddress, info.RegionSize, info.State, info.Protect, info.Type);
                    }
                    // Put failures on the same clock as title faults, even if another queue is active.
                    if (FAILED(hr) && rt::startup_trace::enabled()) {
                        rt::startup_trace::event("dstorage_failure",
                            "\"queue\":" + rt::startup_trace::quote(rt::startup_trace::hex(reinterpret_cast<uintptr_t>(this))) +
                            ",\"hr\":" + rt::startup_trace::quote(hr_text(hr)) +
                            ",\"options\":" + rt::startup_trace::quote(rt::startup_trace::hex(o)) +
                            ",\"offset\":" + std::to_string(e.file ? r.FileOffset : 0ull) +
                            ",\"source_size\":" + std::to_string(r.SourceSize) +
                            ",\"destination_size\":" + std::to_string(r.DestinationSize));
                    }
                }
                if (e.file && e.file_counted) e.file->finish_pending();
                if (FAILED(hr) && SUCCEEDED(since_notify_)) since_notify_ = hr;
                break;
            case Entry::Type::Status: {
                const auto result = FAILED(hr) ? hr : since_notify_;
                if (SUCCEEDED(hr)) {
                    e.status->complete(e.index, result);
                    since_notify_ = S_OK;
                }
                notify_us = Timing::elapsed(begin);
                if (leonida::dstorage::trace_enabled())
                    std::fprintf(stderr, "[rt] ds-status queue=%p array=%p slot=%u hr=%08lx\n",
                                 static_cast<void*>(this), static_cast<void*>(e.status), e.index,
                                 static_cast<unsigned long>(result));
                break;
            }
            case Entry::Type::Signal:
                if (SUCCEEDED(hr)) hr = e.fence->Signal(e.value);
                notify_us = Timing::elapsed(begin);
                if (leonida::dstorage::trace_enabled())
                    std::fprintf(stderr, "[rt] ds-signal queue=%p value=%llu hr=%08lx\n", static_cast<void*>(this),
                                 e.value, static_cast<unsigned long>(hr));
                since_notify_ = S_OK;
                break;
            case Entry::Type::Event:
                if (SUCCEEDED(hr) && !SetEvent(e.event)) hr = HRESULT_FROM_WIN32(GetLastError());
                notify_us = Timing::elapsed(begin);
                if (leonida::dstorage::trace_enabled())
                    std::fprintf(stderr, "[rt] ds-event queue=%p hr=%08lx\n", static_cast<void*>(this),
                                 static_cast<unsigned long>(hr));
                break;
        }
        if (e.type != Entry::Type::Request && leonida::dstorage::trace_enabled())
            std::fprintf(stderr, "[rt] ds-notify queue=%p type=%u us=%lld\n", static_cast<void*>(this),
                         static_cast<unsigned>(e.type), notify_us);
        if (FAILED(hr)) {
            DSTORAGE_ERROR_FIRST_FAILURE f{};
            f.HResult = hr;
            describe(e, f);
            record_failure(f);
        }
    }

    HRESULT validate_request(const Entry& e) const {
        const DSTORAGE_REQUEST& r = e.request;
        const uint64_t o = r.Options.AsUINT64;
        if (o & options::kReservedMask) return E_DSTORAGE_RESERVED_FIELDS;
        if (options::source_type(o) != desc_.SourceType) return E_DSTORAGE_INVALID_SOURCE_TYPE;
        if (options::bcpack(o) > DSTORAGE_BCPACK_MODE_BC7) return E_DSTORAGE_INVALID_BCPACK_MODE;
        if (options::swizzle(o) > DSTORAGE_SWIZZLE_MODE_4K_12K_4K_12K) return E_DSTORAGE_INVALID_SWIZZLE_MODE;
        const unsigned bc = unsigned(options::bcpack(o)), sw = unsigned(options::swizzle(o));
        const bool pages = o & (options::kDestinationPages | options::kSourcePages);
        if ((bc || sw || pages) && !extended_enabled()) return E_NOTIMPL;
        if (bc == DSTORAGE_BCPACK_MODE_BC2) {
            log_once("bc2", "BC2 has no known BCPack block decoder (E_NOTIMPL)");
            return E_NOTIMPL;
        }
        const bool zlib = options::zlib(o);
        if ((zlib && bc) != (r.IntermediateSize != 0) || r.IntermediateSize > kMaxRequestBytes)
            return E_DSTORAGE_INVALID_INTERMEDIATE_SIZE;
        if (zlib || bc) {
            if (r.DestinationSize <= r.SourceSize) return E_DSTORAGE_INVALID_DESTINATION_SIZE;
        } else if (r.DestinationSize != r.SourceSize) return E_DSTORAGE_INVALID_DESTINATION_SIZE;
        if (bc && r.DestinationSize % ((bc == 1 || bc == 4) ? 8 : 16)) return E_DSTORAGE_INVALID_DESTINATION_SIZE;
        if (sw && r.DestinationSize % leonida::dstorage::group_size(sw)) return E_DSTORAGE_INVALID_DESTINATION_SIZE;
        const bool memory = desc_.SourceType == DSTORAGE_REQUEST_SOURCE_MEMORY;
        if (!memory && (o & options::kSourcePages)) return E_DSTORAGE_INVALID_SOURCE_TYPE;
        const uint64_t total = static_cast<uint64_t>(r.DestinationSize) + (memory ? r.SourceSize : 0u);
        if (total > kMaxRequestBytes ||
            (r.DestinationSize > kMax4kPageDestinationSize && !(o & options::kDestinationPages)) ||
            (memory && r.SourceSize > kMax4kPageDestinationSize && !(o & options::kSourcePages)))
            return E_DSTORAGE_REQUEST_TOO_LARGE;
        if ((zlib || bc) && !memory && r.FileOffset % 16 != 0) return E_DSTORAGE_INVALID_FILE_OFFSET;
        if (!r.Destination) return E_INVALIDARG;
        if (memory) {
            if (!r.Source) return E_INVALIDARG;
        } else {
            if (!r.File) return E_INVALIDARG;
            if (!e.file_counted) return E_DSTORAGE_FILE_NOT_OPEN;
        }
        return S_OK;
    }

    HRESULT execute_request(Entry& e, bool pair_reserved) {
        const HRESULT valid = validate_request(e);
        if (FAILED(valid)) { e.reason = "request-validation"; return valid; }
        const auto& r = e.request;
        const auto o = r.Options.AsUINT64;
        const bool memory = desc_.SourceType == DSTORAGE_REQUEST_SOURCE_MEMORY;
        if ((o & (options::kDestinationPages | options::kSourcePages)) || options::zlib(o) ||
            options::bcpack(o) || options::swizzle(o)) {
            leonida::dstorage::StagingPool::Lease reservation(leonida::dstorage::staging_pool());
            if (!pair_reserved) reservation.acquire(leonida::dstorage::staging_bytes(r));
            std::vector<uint8_t> data;
            const auto hr = prepare(e, data);
            return FAILED(hr) ? hr : publish(e, data);
        }
        if (r.SourceSize == 0) return S_OK;  // nothing to move; the reference does not say this fails
        void* const destination = cpu_destination(r);
        if (memory) {
            const auto begin = Timing::start();
            const auto hr = written(r, title_write(destination, r.Source, r.SourceSize) ? S_OK : E_DSTORAGE_ACCESS_VIOLATION);
            e.timing.copy = Timing::elapsed(begin);
            if (FAILED(hr)) e.reason = "copy";
            return hr;
        }
        if (extended_enabled() && !rt::xmem::commit_physical_range(destination, r.DestinationSize)) {
            e.reason = "destination-commit";
            return E_DSTORAGE_ACCESS_VIOLATION;
        }
        const auto begin = Timing::start();
        const auto hr = read_file_range(e.file->handle(), r.FileOffset, destination, r.SourceSize, ReadTarget::Title);
        e.timing.read = Timing::elapsed(begin);
        if (FAILED(hr)) e.reason = "file-read";
        return written(r, hr);
    }

    // Where the CPU writes a request's destination: the title hands its stream pool's GPU-only alias range as the
    // destination of streamed texture data (gfxgeom notes, section 2), which only the GPU maps; its CPU view holds the
    // same pages (src/title_page_watch.h, title_gpu_views).
    static void* cpu_destination(const DSTORAGE_REQUEST& r) {
        return reinterpret_cast<void*>(leonida::title_gpu_views::to_cpu(reinterpret_cast<uintptr_t>(r.Destination)));
    }
    // A request that wrote its destination tells the D3D12 layer, whose images of textures in that memory are filled
    // from it (title_texel_writes).
    static HRESULT written(const DSTORAGE_REQUEST& r, HRESULT hr) {
        if (SUCCEEDED(hr)) leonida::title_texel_writes::note(cpu_destination(r), r.DestinationSize);
        return hr;
    }

    // The caller may populate or remap the physical page list between enqueue and Submit.
    // Snapshot page numbers on the worker before taking xmem's allocation/map lock.
    static HRESULT page_transfer(const ULONG_PTR* pages, uint16_t offset, std::span<uint8_t> buffer, bool write) {
        if (buffer.empty()) return S_OK;
        std::vector<uint64_t> numbers(1 + (buffer.size() - 1 + offset) / 65536);
        if (!guarded_move(numbers.data(), pages, numbers.size() * sizeof(numbers[0])))
            return E_DSTORAGE_ACCESS_VIOLATION;
        // Page 0 is never a title page: a destination list that names it was not filled when its request ran, and
        // the request fails below. Counted, since each one is data the title asked for and did not get.
        if (write && std::find(numbers.begin(), numbers.end(), uint64_t{0}) != numbers.end())
            log_hold("page0", ++leonida::dstorage::hold_stats().page0_entries, 0, 0);
        if (rt::xmem::transfer_physical(numbers, offset, buffer, write)) return S_OK;
        return HRESULT_FROM_WIN32(GetLastError());
    }

    static HRESULT inflate(const std::vector<uint8_t>& src, std::vector<uint8_t>& out, size_t size) {
        leonida::ZlibError why{};
        static const bool bulk = [] {
            wchar_t value[2]{};
            return GetEnvironmentVariableW(L"LEONIDA_DSTORAGE_FAST_INFLATE", value, 2) != 1 || value[0] != L'0';
        }();
        out.reserve(size);
        if (leonida::zlib_inflate(src, out, size, &why, bulk))
            return out.size() == size ? S_OK : E_DSTORAGE_DECOMPRESSION_ERROR;
        switch (why) {
            case leonida::ZlibError::header: return E_DSTORAGE_ZLIB_BAD_HEADER;
            case leonida::ZlibError::checksum: return E_DSTORAGE_ZLIB_PARITY_FAIL;
            case leonida::ZlibError::limit: return E_DSTORAGE_DECOMPRESSION_ERROR;
            default: return E_DSTORAGE_ZLIB_BAD_DATA;
        }
    }

    static HRESULT decode_stages(Entry& e, unsigned bc, unsigned sw, std::vector<uint8_t>& data) {
        const auto& r = e.request;
        if (options::zlib(r.Options.AsUINT64)) {
            std::vector<uint8_t> scratch;
            static const bool reported = (log_once("zlib", "zlib requests use CPU inflate (RFC 1950)"), true);
            (void)reported;
            const auto begin = Timing::start();
            const auto hr = inflate(data, scratch, bc ? r.IntermediateSize : r.DestinationSize);
            e.timing.inflate = Timing::elapsed(begin);
            if (FAILED(hr)) { e.reason = "inflate"; return hr; }
            data = std::move(scratch);
        }
        if (bc) {
            std::vector<uint8_t> scratch(r.DestinationSize);
            static const bool allow_fitted = [] {
                char value[2]{};
                return GetEnvironmentVariableA("LEONIDA_DSTORAGE_FITTED", value, sizeof value) != 1 || value[0] != '0';
            }();
            const auto begin = Timing::start();
            static const bool fast_entropy = [] {
                char value[2]{};
                return GetEnvironmentVariableA("LEONIDA_DSTORAGE_FAST_BCPACK", value, sizeof value) != 1 || value[0] != '0';
            }();
            // LEONIDA_DSTORAGE_LOST_DIVISOR=N: a fitted BC6H/BC7 request may rebuild up to 1 in N of its blocks from
            // a predictor the profile lacks (default 32; 1024 is the reference decoder's limit). Past it the request
            // fails, as before. A few approximate blocks cost a few wrong 4x4 texels; a failed request leaves the
            // whole destination untouched.
            static const std::size_t lost_divisor = [] {
                char value[16]{};
                const DWORD n = GetEnvironmentVariableA("LEONIDA_DSTORAGE_LOST_DIVISOR", value, sizeof value);
                if (n == 0 || n >= sizeof value) return std::size_t(32);
                char* end = nullptr;
                const unsigned long v = std::strtoul(value, &end, 10);
                if (end == value || *end || v < 1) {
                    log_once("lost-divisor", std::string("LEONIDA_DSTORAGE_LOST_DIVISOR=") + value +
                                                 " is not a positive number: using 32");
                    return std::size_t(32);
                }
                return std::size_t(v);
            }();
            const char* failure = nullptr;
            leonida::bcpack::Report report;
            const auto quality = leonida::bcpack::decode(bc, data, scratch, allow_fitted, bc >= 6 ? bcpack_history() : nullptr,
                                                         &failure, fast_entropy, lost_divisor, &report);
            e.timing.bcpack = Timing::elapsed(begin);
            if (quality == leonida::bcpack::Quality::failed) { e.reason = failure; return E_DSTORAGE_DECOMPRESSION_ERROR; }
            if (quality == leonida::bcpack::Quality::fitted) {
                static const bool reported = (log_once("fitted", "using fitted BCPack predictors"), true);
                (void)reported;
                if (report.lost) {
                    static std::atomic<std::uint64_t> requests{0}, blocks{0};
                    const auto n = requests.fetch_add(1) + 1;
                    const auto total = blocks.fetch_add(report.lost) + report.lost;
                    if (n <= 8 || (n & (n - 1)) == 0)   // the first eight, then at powers of two
                        std::fprintf(stderr, "[rt] dstorage: BC%u request approximated %zu of %zu blocks from predictors the "
                                     "profile lacks (%llu requests, %llu blocks so far; LEONIDA_DSTORAGE_LOST_DIVISOR=%zu)\n",
                                     bc, report.lost, report.blocks, static_cast<unsigned long long>(n),
                                     static_cast<unsigned long long>(total), lost_divisor);
                }
                if (leonida::dstorage::trace_enabled())
                    std::fprintf(stderr, "[rt] ds-fitted bc=%u bytes=%u\n", bc, r.DestinationSize);
            }
            data = std::move(scratch);
        }
        if (sw) {
            std::vector<uint8_t> scratch(r.DestinationSize);
            const auto begin = Timing::start();
            if (!leonida::dstorage::unshuffle(sw, data, scratch)) return E_DSTORAGE_INVALID_DESTINATION_SIZE;
            e.timing.unshuffle = Timing::elapsed(begin);
            data = std::move(scratch);
        }
        return S_OK;
    }

    HRESULT prepare(Entry& e, std::vector<uint8_t>& data) try {
        const auto& r = e.request;
        const auto o = r.Options.AsUINT64;
        const bool memory = desc_.SourceType == DSTORAGE_REQUEST_SOURCE_MEMORY;
        const auto bc = unsigned(options::bcpack(o)), sw = unsigned(options::swizzle(o));
        data.resize(r.SourceSize);
        HRESULT hr = S_OK;
        const auto read_begin = Timing::start();
        if (!memory) hr = read_file_range(e.file->handle(), r.FileOffset, data.data(), r.SourceSize, ReadTarget::Staging);
        else if (o & options::kSourcePages) hr = page_transfer(r.SourcePageArray, r.SourcePageOffset, data, false);
        else if (!data.empty() && !guarded_move(data.data(), r.Source, data.size())) hr = E_DSTORAGE_ACCESS_VIOLATION;
        e.timing.read = Timing::elapsed(read_begin);
        if (FAILED(hr)) { e.reason = "read"; return hr; }
        if (bc || sw || options::zlib(o)) {
            set_decompression_pending(+1);
            try {
                hr = decode_stages(e, bc, sw, data);
            } catch (const std::bad_alloc&) {
                hr = E_OUTOFMEMORY;
            }
            set_decompression_pending(-1);
            if (FAILED(hr)) {
                std::lock_guard lock(stats().mutex);
                ++stats().s.DecompressionErrors;
                return hr;
            }
        }
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }

    HRESULT publish(Entry& e, std::vector<uint8_t>& data) try {
        const auto& r = e.request;
        const auto o = r.Options.AsUINT64;
        const auto bc = options::bcpack(o), sw = options::swizzle(o);
        HRESULT hr = S_OK;
        const auto copy_begin = Timing::start();
        if (o & options::kDestinationPages) hr = page_transfer(r.DestinationPageArray, r.DestinationPageOffset, data, true);
        else if (!data.empty()) {
            void* const dest = cpu_destination(r);
            if (extended_enabled() && !rt::xmem::commit_physical_range(dest, data.size())) {
                e.reason = "destination-commit";
                return E_DSTORAGE_ACCESS_VIOLATION;
            }
            hr = written(r, title_write(dest, data.data(), data.size()) ? S_OK : E_DSTORAGE_ACCESS_VIOLATION);
        }
        e.timing.copy = Timing::elapsed(copy_begin);
        if (FAILED(hr)) e.reason = "publish";
        if (leonida::dstorage::trace_enabled() && SUCCEEDED(hr) && (bc || sw || (o & options::kDestinationPages) ||
            leonida::title_gpu_views::other_view(reinterpret_cast<uintptr_t>(r.Destination)))) {
            std::fprintf(stderr, "[rt] ds-write queue=%p dest=%p cpu=%p options=%llx bytes=%zu nonzero=%d\n",
                         static_cast<void*>(this), r.Destination, (o & options::kDestinationPages) ? nullptr : cpu_destination(r),
                         o, data.size(),
                         int(std::any_of(data.begin(), data.end(), [](uint8_t byte) { return byte != 0; })));
        }
        return hr;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }

    void set_pending(int delta) {
        std::lock_guard lock(stats().mutex);
        priority_stats(stats().s, desc_.Priority).PendingRequestCount += static_cast<uint32_t>(delta);
    }

    // PendingDecompressionRequestCount covers the current decode pipeline; TotalDecompressionRequestCount counts each
    // one that reached its inflate (its source was read), DecompressionErrors those whose stream then failed.
    static void set_decompression_pending(int delta) {
        std::lock_guard lock(stats().mutex);
        auto& s = stats().s;
        s.PendingDecompressionRequestCount += static_cast<uint32_t>(delta);
        if (delta > 0) ++s.TotalDecompressionRequestCount;
    }

    void describe(const Entry& e, DSTORAGE_ERROR_FIRST_FAILURE& f) const {
        switch (e.type) {
            case Entry::Type::Request: {
                f.CommandType = DSTORAGE_COMMAND_TYPE_REQUEST;
                auto& q = f.Request;
                const DSTORAGE_REQUEST& r = e.request;
                q.Options = r.Options;
                q.Destination = r.Destination;
                q.DestinationSize = r.DestinationSize;
                q.SourceSize = r.SourceSize;
                q.CancellationTag = r.CancellationTag;
                if (options::source_type(r.Options.AsUINT64) == DSTORAGE_REQUEST_SOURCE_MEMORY) {
                    q.Source = r.Source;
                } else {
                    q.FileOffset = r.FileOffset;
                    if (e.file) wcsncpy_s(q.Filename, e.file->path().c_str(), _TRUNCATE);
                }
                // The reference says the name stays readable until the request completes.
                if (r.Name)
                    for (size_t i = 0; i + 1 < kRequestMaxName && r.Name[i]; ++i) q.Name[i] = r.Name[i];
                break;
            }
            case Entry::Type::Status:
                f.CommandType = DSTORAGE_COMMAND_TYPE_STATUS;
                f.Status.StatusArray = e.status_as_given;
                f.Status.Index = e.index;
                break;
            case Entry::Type::Signal:
                f.CommandType = DSTORAGE_COMMAND_TYPE_SIGNAL;
                f.Signal.Fence = e.fence;
                f.Signal.Value = e.value;
                break;
            case Entry::Type::Event:
                f.CommandType = kCommandTypeEvent;
                break;
        }
    }

    void record_failure(const DSTORAGE_ERROR_FIRST_FAILURE& f) {
        count_error();
        {
            std::lock_guard lock(error_mutex_);
            if (record_.FailureCount++ == 0) record_.FirstFailure = f;
            if (error_event_) SetEvent(error_event_);
        }
        static auto* counts = new std::map<HRESULT, uint64_t>;
        std::lock_guard lock(log_mutex());
        const auto count = ++(*counts)[f.HResult];
        // The boot digest groups identical lines. Keep one invariant line per HRESULT and
        // report the cumulative value separately so a truncated run is still countable.
        std::fprintf(stderr, "[rt] dstorage: an entry failed with 0x%08lx (facility=%lx code=%lx)\n"
                             "[rt] ds-error-count hr=%08lx count=%llu command=%u\n",
                     static_cast<unsigned long>(f.HResult), static_cast<unsigned long>(HRESULT_FACILITY(f.HResult)),
                     static_cast<unsigned long>(HRESULT_CODE(f.HResult)), static_cast<unsigned long>(f.HResult),
                     static_cast<unsigned long long>(count), unsigned(f.CommandType));
    }

    void clear_record_locked() {
        record_ = DSTORAGE_ERROR_RECORD{};
        record_.FirstFailure.CommandType = DSTORAGE_COMMAND_TYPE_NONE;
    }

    static void release_references(Entry& e) {
        if (e.file) e.file->Release();
        if (e.status) e.status->Release();
        if (e.fence) e.fence->Release();
        if (e.event) CloseHandle(e.event);
        e.event = nullptr;
        e.file = nullptr;
        e.status = nullptr;
        e.fence = nullptr;
    }

    std::atomic<ULONG> refs_{1};
    const DSTORAGE_QUEUE_DESC1 desc_;
    std::mutex close_mutex_;  // one Close at a time
    std::mutex mutex_;        // entries_, submitted_, in_flight_, closing_, closed_
    std::condition_variable work_, space_, idle_;
    std::deque<Entry> entries_;
    size_t submitted_ = 0;  // the first submitted_ entries are submitted
    const bool hold_pages_;           // LEONIDA_DSTORAGE_HOLD_PAGES
    const uint32_t hold_release_ms_;  // LEONIDA_DSTORAGE_HOLD_MS (kDefaultHoldMs unless set); 0: no time limit
    bool holding_ = false;            // under mutex_: a self-submit left entries for the title's Submit
    bool hold_warned_ = false;        // under mutex_: this hold's five-second line was written
    std::chrono::steady_clock::time_point held_since_{};  // under mutex_
    size_t in_flight_ = 0;
    bool closing_ = false;
    bool closed_ = false;
    std::thread worker_;
    PTP_WORK preparation_work_ = nullptr;
    Prepared* helper_job_ = nullptr; // Lives on the publisher stack through the callback and ordered write.
    HRESULT since_notify_ = S_OK;  // worker only
    std::mutex error_mutex_;
    DSTORAGE_ERROR_RECORD record_{};
    HANDLE error_event_ = nullptr;
};

// ---- The factory ----------------------------------------------------------------------------------------------------

HRESULT validate_queue_desc(const DSTORAGE_QUEUE_DESC1& d) {
    if (d.SourceType != DSTORAGE_REQUEST_SOURCE_FILE && d.SourceType != DSTORAGE_REQUEST_SOURCE_MEMORY)
        return E_DSTORAGE_INVALID_SOURCE_TYPE;
    if (d.Capacity < kMinQueueCapacity || d.Capacity > kMaxQueueCapacity) return E_DSTORAGE_INVALID_QUEUE_CAPACITY;
    if (d.Priority < DSTORAGE_PRIORITY_FIRST || d.Priority > DSTORAGE_PRIORITY_LAST)
        return E_DSTORAGE_INVALID_QUEUE_PRIORITY;
    if (d.SourceType == DSTORAGE_REQUEST_SOURCE_MEMORY && d.Priority != DSTORAGE_PRIORITY_REALTIME)
        return E_DSTORAGE_INVALID_MEMORY_QUEUE_PRIORITY;
    if (d.Options.Reserved) return E_DSTORAGE_RESERVED_FIELDS;
    return S_OK;
}

using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

// The title's own CreateFileW: the GAME: mapping and overlay when file_system.cpp is linked, else the host's.
CreateFileWFn create_file_w() {
    if (void* fn = rt::shims::find("kernel32.dll", "CreateFileW")) return reinterpret_cast<CreateFileWFn>(fn);
    return &::CreateFileW;
}

class Factory final : public IDStorageFactoryX2 {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (!iid_policy().accept_query(riid, Kind::Factory, "IDStorageFactoryX::QueryInterface")) return E_NOINTERFACE;
        AddRef();
        *ppv = static_cast<IDStorageFactoryX2*>(this);
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    // One factory for the process; its count never drops below one, so it is never freed.
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = refs_.load();
        while (r > 1 && !refs_.compare_exchange_weak(r, r - 1)) {}
        return r > 1 ? r - 1 : 1;
    }

    HRESULT STDMETHODCALLTYPE CreateQueue(const DSTORAGE_QUEUE_DESC* desc, REFIID riid, void** ppv) override {
        if (!ppv) return bad_call("IDStorageFactoryX::CreateQueue", E_POINTER, "null ppv");
        *ppv = nullptr;
        if (!desc) return bad_call("IDStorageFactoryX::CreateQueue", E_INVALIDARG, "null desc");
        DSTORAGE_QUEUE_DESC1 d{};
        d.SourceType = desc->SourceType;
        d.Capacity = desc->Capacity;
        d.Priority = desc->Priority;
        d.Name = desc->Name;
        return create_queue(d, riid, ppv, "IDStorageFactoryX::CreateQueue");
    }

    HRESULT STDMETHODCALLTYPE CreateQueue1(const DSTORAGE_QUEUE_DESC1* desc, REFIID riid, void** ppv) override {
        if (!ppv) return bad_call("IDStorageFactoryX2::CreateQueue1", E_POINTER, "null ppv");
        *ppv = nullptr;
        if (!desc) return bad_call("IDStorageFactoryX2::CreateQueue1", E_INVALIDARG, "null desc");
        return create_queue(*desc, riid, ppv, "IDStorageFactoryX2::CreateQueue1");
    }

    HRESULT STDMETHODCALLTYPE OpenFile(const WCHAR* path, REFIID riid, void** ppv) override {
        if (!ppv) return bad_call("IDStorageFactoryX::OpenFile", E_POINTER, "null ppv");
        *ppv = nullptr;
        if (!path || !*path) return bad_call("IDStorageFactoryX::OpenFile", E_INVALIDARG, "null or empty path");
        if (!iid_policy().accept_creating(riid, Kind::File, "IDStorageFactoryX::OpenFile")) return E_NOINTERFACE;
        const HANDLE h = create_file_w()(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                         FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            return HRESULT_FROM_WIN32(error ? error : ERROR_FILE_NOT_FOUND);
        }
        File* f = new (std::nothrow) File(h, path);
        if (!f) {
            CloseHandle(h);
            return E_OUTOFMEMORY;
        }
        *ppv = static_cast<IDStorageFileX*>(f);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateStatusArray(UINT32 capacity, PCSTR, REFIID riid, void** ppv) override {
        if (!ppv) return bad_call("IDStorageFactoryX::CreateStatusArray", E_POINTER, "null ppv");
        *ppv = nullptr;
        if (capacity == 0) return bad_call("IDStorageFactoryX::CreateStatusArray", E_INVALIDARG, "capacity 0");
        if (!iid_policy().accept_creating(riid, Kind::StatusArray, "IDStorageFactoryX::CreateStatusArray"))
            return E_NOINTERFACE;
        std::unique_ptr<std::atomic<HRESULT>[]> entries(new (std::nothrow) std::atomic<HRESULT>[capacity]);
        if (!entries) return E_OUTOFMEMORY;
        auto* a = new (std::nothrow) StatusArray(capacity, std::move(entries));
        if (!a) return E_OUTOFMEMORY;
        *ppv = static_cast<IDStorageStatusArrayX*>(a);
        return S_OK;
    }

    // DirectStorage's own worker work would be pinned to these cores on the console. The mask names console cores,
    // so it is stored and not applied to the host's.
    HRESULT STDMETHODCALLTYPE SetCpuAffinity(UINT64 affinity) override {
        cpu_affinity_.store(affinity);
        return S_OK;
    }
    void STDMETHODCALLTYPE SetDebugFlags(UINT32 flags) override { debug_flags_.store(flags); }

    HRESULT STDMETHODCALLTYPE SetStagingBufferSize(UINT32 size) override {
        if (std::find(std::begin(kStagingBufferSizes), std::end(kStagingBufferSizes), size) ==
            std::end(kStagingBufferSizes))
            return bad_call("IDStorageFactoryX::SetStagingBufferSize", E_DSTORAGE_INVALID_STAGING_BUFFER_SIZE,
                            "not one of 0, 16, 20, 24, 28 or 32 MiB");
        std::lock_guard lock(stats().mutex);
        if (stats().live_queues || stats().s.CurrentOpenFileCount) return E_DSTORAGE_STAGING_BUFFER_LOCKED;
        staging_size_ = size;  // there is no staging buffer here; the size is only kept
        return S_OK;
    }

    void STDMETHODCALLTYPE GetStats(DSTORAGE_STATS* out) override {
        if (!out) {
            bad_call("IDStorageFactoryX1::GetStats", E_POINTER, "null stats");
            return;
        }
        std::lock_guard lock(stats().mutex);
        *out = stats().s;
    }

private:
    HRESULT create_queue(const DSTORAGE_QUEUE_DESC1& desc, REFIID riid, void** ppv, const char* caller) {
        if (const HRESULT hr = validate_queue_desc(desc); FAILED(hr)) return bad_call(caller, hr, "invalid queue desc");
        if (!iid_policy().accept_creating(riid, Kind::Queue, caller)) return E_NOINTERFACE;
        auto* q = new (std::nothrow) Queue(desc);
        if (!q) return E_OUTOFMEMORY;
        if (!q->start()) {
            q->Release();
            return E_OUTOFMEMORY;
        }
        *ppv = static_cast<IDStorageQueueX2*>(q);
        return S_OK;
    }

    std::atomic<ULONG> refs_{1};
    std::atomic<uint64_t> cpu_affinity_{~0ull};
    std::atomic<uint32_t> debug_flags_{0};
    uint32_t staging_size_ = 32u << 20;  // under stats().mutex
};

Factory& factory() {
    static auto* f = new Factory;
    return *f;
}

HRESULT WINAPI dstorage_get_factory(REFIID riid, void** ppv) {
    if (!ppv) return bad_call("DStorageGetFactory", E_POINTER, "null ppv");
    *ppv = nullptr;
    if (!iid_policy().accept_creating(riid, Kind::Factory, "DStorageGetFactory")) return E_NOINTERFACE;
    Factory& f = factory();
    f.AddRef();
    *ppv = static_cast<IDStorageFactoryX2*>(&f);
    return S_OK;
}

}  // namespace

// The console module names; docs/directstorage.md has the evidence and the confidence for each.
SHIM_TABLE(dstorage,
    {"dstorage_xs.dll", "DStorageGetFactory", reinterpret_cast<void*>(&dstorage_get_factory)},
    {"xcrdapi.dll", "DStorageGetFactory", reinterpret_cast<void*>(&dstorage_get_factory)},
    {"dstorage.dll", "DStorageGetFactory", reinterpret_cast<void*>(&dstorage_get_factory)},
);
