// xgameruntime: XGameSaveImpl, the class xgameruntime.lib's XGameSave* functions reach through QueryApiImpl, offline.
//
// A GDK title opens its save provider during boot (XGameSaveInitializeProvider), then reads and writes containers
// of named blobs. Each XGameSave* function in xgameruntime.lib asks QueryApiImpl for XGameSaveImpl and calls one slot
// with its own arguments after `this`. The class and interface ids and the slot order are
// xodus-gaming/xgameruntime-docs' "Layout" lists (GPL-3.0; facts only, nothing copied). Signatures, limits and error
// codes follow Microsoft's public GDK reference (learn.microsoft.com/gaming/gdk: the XGameSave function pages and
// "Error codes"). No GDK header or library code is used.
//
// Offline the saves are files, in the layout tools/saves.py imports into (docs/saves.md):
//   <root>\local-user\<container>\<blob>      root = LEONIDA_SAVE_DIR, else %LOCALAPPDATA%\Leonida\saves
// Container and blob names are %-encoded: [A-Za-z0-9_-] and '.' (except as the first character) stay, every other
// UTF-8 byte becomes %XX. A container's display name is kept in "<container>\.display", which no encoded blob name
// can collide with (an encoded name never starts with '.'). Names ending in '~' are temporary files of an update in
// progress; an encoded name never ends in '~' either.
//
// An update buffers its writes and deletes; SubmitUpdate writes every new blob to "<blob>~", then renames each over
// its blob and removes the deleted ones, so a failed write leaves the container as it was.
//
// LEONIDA_TRACE_XGAMESAVE=1 logs one "[rt] xgamesave:" line per call (see "Tracing" below).
//
// ASSUMPTION (no executable yet): the quota is a fixed 1 GiB (the GDK says the per-user quota "is not a fixed size");
// a blob name is at most 256 bytes (GS_MAX_BLOB_NAME_SIZE is not on the public pages); GetContainerInfo of a
// container that does not exist returns S_OK without calling the callback; ReadBlobData with blobNames = nullptr
// reads every blob of the container. syncOnDemand is accepted and ignored: nothing syncs offline.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <XAsync.h>
#include <XAsyncProvider.h>
#include <XTaskQueue.h>

namespace {

namespace fs = std::filesystem;

// ---- Public GDK types (XGameSave.h, as the GDK reference documents them) ----

using XUserHandle = void*;
struct XGameSaveProvider;
struct XGameSaveContainer;
struct XGameSaveUpdate;
using XGameSaveProviderHandle = XGameSaveProvider*;
using XGameSaveContainerHandle = XGameSaveContainer*;
using XGameSaveUpdateHandle = XGameSaveUpdate*;

struct XGameSaveContainerInfo {
    const char* name;
    const char* displayName;
    uint32_t blobCount;
    uint64_t totalSize;
    time_t lastModifiedTime;
    bool needsSync;
};
struct XGameSaveBlobInfo {
    const char* name;
    uint32_t size;
};
struct XGameSaveBlob {
    XGameSaveBlobInfo info;
    uint8_t* data;
};
using ContainerInfoCallback = bool(CALLBACK*)(const XGameSaveContainerInfo* info, void* context);
using BlobInfoCallback = bool(CALLBACK*)(const XGameSaveBlobInfo* info, void* context);

// XGameErr.h values, from the GDK "Error codes" page (Game save).
constexpr HRESULT E_GS_INVALID_CONTAINER_NAME = static_cast<HRESULT>(0x80830001);
constexpr HRESULT E_GS_OUT_OF_LOCAL_STORAGE = static_cast<HRESULT>(0x80830003);
constexpr HRESULT E_GS_UPDATE_TOO_BIG = static_cast<HRESULT>(0x80830005);
constexpr HRESULT E_GS_QUOTA_EXCEEDED = static_cast<HRESULT>(0x80830006);
constexpr HRESULT E_GS_PROVIDED_BUFFER_TOO_SMALL = static_cast<HRESULT>(0x80830007);
constexpr HRESULT E_GS_BLOB_NOT_FOUND = static_cast<HRESULT>(0x80830008);
constexpr HRESULT E_GS_HANDLE_EXPIRED = static_cast<HRESULT>(0x8083000D);

constexpr uint64_t kMaxUpdateBytes = 16ull * 1024 * 1024;   // GS_MAX_BLOB_SIZE, 16 MB: one blob and one update
constexpr uint64_t kQuotaBytes = 1ull << 30;                 // ASSUMPTION, see the header
constexpr size_t kMaxContainerName = 256;
constexpr size_t kMaxBlobName = 256;                         // ASSUMPTION, see the header

// ---- Names ----

std::string encode_name(std::string_view name) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (const char ch : name) {
        const auto c = static_cast<unsigned char>(ch);
        const bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
                          c == '-' || (c == '.' && !out.empty());
        if (keep) {
            out += ch;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

// The inverse; false for a file name encode_name could not have produced (temporary files, .display, strays).
bool decode_name(std::string_view encoded, std::string& out) {
    out.clear();
    if (encoded.empty() || encoded.front() == '.' || encoded.back() == '~') return false;
    auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    for (size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] != '%') {
            out += encoded[i];
            continue;
        }
        if (i + 2 >= encoded.size()) return false;
        const int hi = digit(encoded[i + 1]), lo = digit(encoded[i + 2]);
        if (hi < 0 || lo < 0) return false;
        out += static_cast<char>(hi << 4 | lo);
        i += 2;
    }
    return encode_name(out) == encoded;   // one spelling per name
}

// XGameSaveCreateContainer's rule: at most 256 characters; a path part (up to the last '/') of [A-Za-z0-9_/], then a
// non-empty file part of [A-Za-z0-9_.-] that does not end in '.' nor hold "..".
bool valid_container_name(const char* name) {
    if (!name) return false;
    const std::string_view n(name);
    if (n.empty() || n.size() > kMaxContainerName) return false;
    const size_t slash = n.rfind('/');
    const std::string_view path = slash == std::string_view::npos ? std::string_view{} : n.substr(0, slash + 1);
    const std::string_view file = slash == std::string_view::npos ? n : n.substr(slash + 1);
    auto alnum = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
    for (const char c : path)
        if (!alnum(c) && c != '_' && c != '/') return false;
    if (file.empty() || file.back() == '.' || file.find("..") != std::string_view::npos) return false;
    for (const char c : file)
        if (!alnum(c) && c != '_' && c != '.' && c != '-') return false;
    return true;
}

bool valid_blob_name(const char* name) { return name && name[0] && std::strlen(name) <= kMaxBlobName; }

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

fs::path save_root() {
    wchar_t buffer[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(L"LEONIDA_SAVE_DIR", buffer, static_cast<DWORD>(std::size(buffer)));
    if (n > 0 && n < std::size(buffer)) return fs::path(buffer);
    n = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, static_cast<DWORD>(std::size(buffer)));
    if (n > 0 && n < std::size(buffer)) return fs::path(buffer) / L"Leonida" / L"saves";
    return {};
}

// ---- Handles ----

// One mutex for the handle sets and the files: saves are small and rare, so nothing is gained by finer locking.
std::recursive_mutex g_mutex;

struct XGameSaveProvider {
    fs::path user_dir;   // <root>\local-user
    std::string config_id;
    uint64_t serial = 0;
};
struct XGameSaveContainer {
    fs::path user_dir;   // the provider's, copied: a container outlives a closed provider safely
    std::string name;
    uint64_t serial = 0;
    fs::path dir() const { return user_dir / widen(encode_name(name)); }
};
struct Op {
    bool write;
    std::vector<uint8_t> data;
};
struct XGameSaveUpdate {
    XGameSaveContainer* container;
    uint64_t container_serial;       // the container's serial when the update was made
    std::string display_name;
    bool has_display_name;
    std::map<std::string, Op> ops;   // blob name -> its one write or delete
    uint64_t bytes;
    uint64_t serial = 0;
};

std::unordered_set<const void*> g_providers, g_containers, g_updates;

bool live(const std::unordered_set<const void*>& set, const void* h) { return h && set.count(h) != 0; }

// Every handle gets a serial when it is made, and no two get the same one. The heap may hand a closed handle's address
// to the next handle of its kind, so whatever keeps a handle past the call that gave it (an update its container, an
// async job its provider, container or update) keeps the serial as well, and the handle counts as the same one only
// while both match. A reused address is then refused as expired instead of acting on another save. Under g_mutex.
uint64_t g_next_serial = 1;

template <class T> bool live_as(const std::unordered_set<const void*>& set, const T* h, uint64_t serial) {
    return live(set, h) && h->serial == serial;
}

// The serial of a live handle; 0, which no handle has, for one that isn't, so a job made from it fails as expired.
template <class T> uint64_t serial_of(const std::unordered_set<const void*>& set, const T* h) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return live(set, h) ? h->serial : 0;
}

// ---- Tracing (LEONIDA_TRACE_XGAMESAVE=1) ----
//
// One line per XGameSave* and XGameSaveFiles* call: the call, its handles, container and blob names, sizes and
// HRESULT, after "t=<ms since the process started> tid=<thread>". An async call also logs when begin() schedules its
// job (XAsyncSchedule), when the work starts on the queue (with how long it waited), when it completes, and when the
// title collects the result with the ...Result call. Blob contents are never logged. Off, a call pays one relaxed
// load; the names are only looked up while tracing.

using TraceSink = void (*)(const char* line);
std::atomic<int> g_trace{-1};                    // -1: LEONIDA_TRACE_XGAMESAVE not read yet
std::atomic<TraceSink> g_trace_sink{nullptr};    // tests: the lines go here instead of stderr

bool tracing() {
    int on = g_trace.load(std::memory_order_relaxed);
    if (on < 0) {
        char value[8] = {};
        const DWORD n = GetEnvironmentVariableA("LEONIDA_TRACE_XGAMESAVE", value, static_cast<DWORD>(sizeof(value)));
        int expected = -1;
        g_trace.compare_exchange_strong(expected, n == 1 && value[0] == '1' ? 1 : 0, std::memory_order_relaxed);
        on = g_trace.load(std::memory_order_relaxed);
    }
    return on > 0;
}

uint64_t filetime_ticks(const FILETIME& t) { return (static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime; }

// Milliseconds since the process was created, the clock the other "t=" figures of a run are read against.
uint64_t trace_ms() {
    static const uint64_t start = [] {
        FILETIME created{}, exited{}, kernel{}, user{};
        if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return filetime_ticks(created);
        FILETIME now{};
        GetSystemTimePreciseAsFileTime(&now);
        return filetime_ticks(now);
    }();
    FILETIME now{};
    GetSystemTimePreciseAsFileTime(&now);
    const uint64_t ticks = filetime_ticks(now);
    return ticks > start ? (ticks - start) / 10000 : 0;
}

void trace(const char* format, ...) {
    char body[3072];
    va_list args;
    va_start(args, format);
    std::vsnprintf(body, sizeof(body), format, args);
    va_end(args);
    char line[3200];
    std::snprintf(line, sizeof(line), "[rt] xgamesave: t=%llums tid=%lu %s\n", static_cast<unsigned long long>(trace_ms()),
                  static_cast<unsigned long>(GetCurrentThreadId()), body);
    if (const TraceSink sink = g_trace_sink.load(std::memory_order_acquire)) {
        sink(line);
    } else {
        std::fputs(line, stderr);
    }
}

#define TRACE(...)                        \
    do {                                  \
        if (tracing()) trace(__VA_ARGS__); \
    } while (0)

const char* or_null(const char* s) { return s ? s : "(null)"; }
unsigned long hr_bits(HRESULT hr) { return static_cast<unsigned long>(hr); }

// "a","b",... for the first eight names, then "+n".
std::string quoted_names(const std::vector<std::string>& names) {
    std::string out;
    for (size_t i = 0; i < names.size() && i < 8; ++i) out += (i ? ",\"" : "\"") + names[i] + "\"";
    if (names.size() > 8) out += ",+" + std::to_string(names.size() - 8);
    return out.empty() ? std::string("(none)") : out;
}

// The name of a live container, "?" for a handle that is not one (it is never read through then).
std::string container_label(const XGameSaveContainer* c) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return live(g_containers, c) ? "\"" + c->name + "\"" : std::string("?");
}
std::string update_label(const XGameSaveUpdate* u) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (!live(g_updates, u) || !live_as(g_containers, u->container, u->container_serial)) return "?";
    return "\"" + u->container->name + "\" ops=" + std::to_string(u->ops.size()) + " bytes=" + std::to_string(u->bytes);
}

// ---- Files ----

struct BlobFile {
    std::string name;
    uint64_t size;
    fs::path path;
};

std::vector<BlobFile> list_blobs(const fs::path& dir) {
    std::vector<BlobFile> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec)) continue;
        std::string name;
        if (!decode_name(narrow(e.path().filename().wstring()), name)) continue;
        out.push_back({name, static_cast<uint64_t>(e.file_size(ec)), e.path()});
    }
    std::sort(out.begin(), out.end(), [](const BlobFile& a, const BlobFile& b) { return a.name < b.name; });
    return out;
}

time_t modified_time(const fs::path& p) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &data)) return 0;
    const uint64_t t = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
    return t < 116444736000000000ull ? 0 : static_cast<time_t>((t - 116444736000000000ull) / 10000000ull);
}

std::string read_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return f ? std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()) : std::string{};
}

bool write_file(const fs::path& p, const void* data, size_t size) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    f.close();
    return !f.fail();
}

struct ContainerRecord {
    std::string name, display_name;
    uint32_t blob_count = 0;
    uint64_t total_size = 0;
    time_t modified = 0;
};

ContainerRecord describe(const std::string& name, const fs::path& dir) {
    ContainerRecord r;
    r.name = name;
    r.display_name = read_text(dir / L".display");
    for (const BlobFile& b : list_blobs(dir)) {
        ++r.blob_count;
        r.total_size += b.size;
        const time_t t = modified_time(b.path);
        if (t > r.modified) r.modified = t;
    }
    return r;
}

std::vector<ContainerRecord> list_containers(const fs::path& user_dir, std::string_view prefix) {
    std::vector<ContainerRecord> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(user_dir, ec)) {
        if (!e.is_directory(ec)) continue;
        std::string name;
        if (!decode_name(narrow(e.path().filename().wstring()), name)) continue;
        if (name.compare(0, prefix.size(), prefix) != 0) continue;
        out.push_back(describe(name, e.path()));
    }
    std::sort(out.begin(), out.end(), [](const ContainerRecord& a, const ContainerRecord& b) { return a.name < b.name; });
    return out;
}

uint64_t used_bytes(const fs::path& user_dir) {
    uint64_t used = 0;
    for (const ContainerRecord& c : list_containers(user_dir, {})) used += c.total_size;
    return used;
}

void enumerate_containers(const fs::path& user_dir, std::string_view prefix, void* context, ContainerInfoCallback cb) {
    for (const ContainerRecord& c : list_containers(user_dir, prefix)) {
        const XGameSaveContainerInfo info{c.name.c_str(), c.display_name.c_str(), c.blob_count, c.total_size,
                                          c.modified, false};
        if (!cb(&info, context)) break;
    }
}

// ---- Reading blobs into the title's buffer ----
//
// The buffer is laid out as the GDK samples read it: an array of XGameSaveBlob, one per blob, whose info.name and
// data point further into the same buffer (each blob's bytes 8-aligned, then its name).

struct BlobRead {
    std::string name;
    std::vector<uint8_t> data;
};

size_t align8(size_t n) { return (n + 7) & ~size_t{7}; }

// The layout the blobs are packed in: 8-byte aligned data and names when the buffer has room for that, packed back
// to back otherwise. A title sizes the buffer as the sum of sizeof(XGameSaveBlob), the data and the name with its NUL
// for each blob, so the minimum is the unaligned size.
size_t packed_size(const std::vector<BlobRead>& blobs, bool aligned) {
    const auto a = [aligned](size_t n) { return aligned ? align8(n) : n; };
    size_t n = a(blobs.size() * sizeof(XGameSaveBlob));
    for (const BlobRead& b : blobs) n += a(b.data.size()) + a(b.name.size() + 1);
    return n;
}
size_t packed_size(const std::vector<BlobRead>& blobs) { return packed_size(blobs, false); }

void pack(const std::vector<BlobRead>& blobs, void* buffer, size_t buffer_size) {
    const bool aligned = buffer_size >= packed_size(blobs, true);
    const auto a = [aligned](size_t n) { return aligned ? align8(n) : n; };
    auto* base = static_cast<uint8_t*>(buffer);
    auto* entries = static_cast<XGameSaveBlob*>(buffer);
    size_t at = a(blobs.size() * sizeof(XGameSaveBlob));
    for (size_t i = 0; i < blobs.size(); ++i) {
        uint8_t* data = base + at;
        if (!blobs[i].data.empty()) std::memcpy(data, blobs[i].data.data(), blobs[i].data.size());
        at += a(blobs[i].data.size());
        char* name = reinterpret_cast<char*>(base + at);
        std::memcpy(name, blobs[i].name.c_str(), blobs[i].name.size() + 1);
        at += a(blobs[i].name.size() + 1);
        entries[i].info.name = name;
        entries[i].info.size = static_cast<uint32_t>(blobs[i].data.size());
        entries[i].data = data;
    }
}

// Reads the named blobs (every blob when names is null) of a live container.
HRESULT read_blobs(const XGameSaveContainer* c, const char* const* names, uint32_t count, std::vector<BlobRead>& out) {
    out.clear();
    const fs::path dir = c->dir();
    std::vector<std::string> wanted;
    if (names) {
        for (uint32_t i = 0; i < count; ++i) {
            if (!valid_blob_name(names[i])) return E_INVALIDARG;
            wanted.emplace_back(names[i]);
        }
    } else {
        for (const BlobFile& b : list_blobs(dir)) wanted.push_back(b.name);
    }
    for (const std::string& name : wanted) {
        std::ifstream f(dir / widen(encode_name(name)), std::ios::binary);
        if (!f) return E_GS_BLOB_NOT_FOUND;
        out.push_back({name, std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>())});
    }
    return S_OK;
}

// ---- Committing an update ----
//
// Every write goes to "<blob>~" first. Then each op, in name order, applies its change: the blob's file is copied to
// its backup "<blob>~b~", then a write renames its temporary over the blob or a delete removes the blob. A blob never
// leaves its name, so a process that dies midway leaves each blob under its name, old or new, as before. When an op
// fails, the ops already applied are undone newest first (a blob that existed is copied back from its backup, one
// that did not is removed), the scratch files are deleted and the update fails with E_GS_OUT_OF_LOCAL_STORAGE. A
// backup whose restore failed is kept, so the old contents survive. Every scratch name ends in '~', which encode_name
// never produces and decode_name rejects, so enumeration and the quota never count one. A commit first deletes the
// temporaries of a commit that died midway. It leaves backups alone: one may be the only copy of a blob's old
// contents. The next successful update of that blob replaces and then removes its backup.

struct CommitStep {
    fs::path final_path, temp, backup;
    bool write = false;
    bool existed = false;      // the blob had a file before the update; the backup is a copy of it
    bool backed_up = false;    // this update wrote the backup
    bool applied = false;      // the op took effect
    bool keep_backup = false;  // its undo failed: the backup holds the only copy of the old contents
};

bool copy_blob(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    return !ec;
}

// Undoes the applied steps among steps[0, upto), newest first. A step whose restore fails keeps its backup.
void undo_steps(std::vector<CommitStep>& steps, size_t upto) {
    for (size_t i = upto; i-- > 0;) {
        CommitStep& s = steps[i];
        if (!s.applied) continue;
        std::error_code ec;
        if (s.existed) {
            if (copy_blob(s.backup, s.final_path)) {
                s.applied = false;
            } else {
                s.keep_backup = true;
            }
        } else {
            fs::remove(s.final_path, ec);
            if (!ec) s.applied = false;
        }
    }
}

// Deletes what the update left behind: the temporaries, and the backups it wrote itself that are not the only copy of
// a blob's old contents.
void remove_scratch(const std::vector<CommitStep>& steps) {
    for (const CommitStep& s : steps) {
        std::error_code ec;
        fs::remove(s.temp, ec);
        if (s.backed_up && !s.keep_backup) fs::remove(s.backup, ec);
    }
}

HRESULT commit(XGameSaveUpdate* u) {
    XGameSaveContainer* c = u->container;
    const fs::path dir = c->dir();
    // The quota: what the container holds after the update, plus every other container.
    uint64_t after = used_bytes(c->user_dir);
    for (const auto& [name, op] : u->ops) {
        std::error_code ec;
        const fs::path p = dir / widen(encode_name(name));
        const uint64_t old = fs::is_regular_file(p, ec) ? static_cast<uint64_t>(fs::file_size(p, ec)) : 0;
        after = after - old + (op.write ? op.data.size() : 0);
    }
    if (after > kQuotaBytes) return E_GS_QUOTA_EXCEEDED;

    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return E_GS_OUT_OF_LOCAL_STORAGE;
    // Temporaries of a commit that died midway: never blobs, and nothing else uses these names. Backups are kept.
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::error_code entry_ec;
        const std::wstring n = e.path().filename().wstring();
        if (n.ends_with(L"~") && !n.ends_with(L"~b~") && e.is_regular_file(entry_ec)) fs::remove(e.path(), entry_ec);
    }

    std::vector<CommitStep> steps;
    steps.reserve(u->ops.size());
    for (const auto& [name, op] : u->ops) {
        CommitStep s;
        s.final_path = dir / widen(encode_name(name));
        s.temp = s.final_path;
        s.temp += L"~";
        s.backup = s.final_path;
        s.backup += L"~b~";
        s.write = op.write;
        std::error_code exists_ec;
        s.existed = fs::is_regular_file(s.final_path, exists_ec);
        if (op.write && !write_file(s.temp, op.data.data(), op.data.size())) {
            std::error_code rm_ec;
            for (const CommitStep& done : steps) fs::remove(done.temp, rm_ec);
            fs::remove(s.temp, rm_ec);
            return E_GS_OUT_OF_LOCAL_STORAGE;
        }
        steps.push_back(std::move(s));
    }

    for (size_t i = 0; i < steps.size(); ++i) {
        CommitStep& s = steps[i];
        bool ok = true;
        if (s.existed) ok = s.backed_up = copy_blob(s.final_path, s.backup);
        if (ok && s.write) {
            ok = MoveFileExW(s.temp.c_str(), s.final_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
        } else if (ok && s.existed) {   // a delete
            std::error_code rm_ec;
            fs::remove(s.final_path, rm_ec);   // already gone is a delete that has nothing left to do
            ok = !rm_ec;
        }
        if (!ok) {
            undo_steps(steps, i);   // the failed op changed nothing: only the ops before it were applied
            remove_scratch(steps);
            return E_GS_OUT_OF_LOCAL_STORAGE;
        }
        s.applied = true;
    }
    remove_scratch(steps);
    if (u->has_display_name) write_file(dir / L".display", u->display_name.data(), u->display_name.size());
    return S_OK;
}


// ---- Operations shared by the sync slots and the async work ----

HRESULT open_provider(XUserHandle user, const char* config_id, XGameSaveProviderHandle* out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!user || !config_id) return E_INVALIDARG;
    const fs::path root = save_root();
    if (root.empty()) return E_GS_OUT_OF_LOCAL_STORAGE;
    const fs::path user_dir = root / L"local-user";
    std::error_code ec;
    fs::create_directories(user_dir, ec);
    if (ec) return E_GS_OUT_OF_LOCAL_STORAGE;
    auto* p = new XGameSaveProvider{user_dir, config_id};
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    p->serial = g_next_serial++;
    g_providers.insert(p);
    *out = p;
    static std::atomic<int> s_logged{0};
    if (s_logged.fetch_add(1) == 0)
        std::fprintf(stderr, "[rt] xgamesave: offline provider for \"%s\" at %ls\n", config_id, user_dir.c_str());
    return S_OK;
}

HRESULT remaining_quota(XGameSaveProviderHandle p, int64_t* remaining) {
    if (!remaining) return E_POINTER;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (!live(g_providers, p)) return E_GS_HANDLE_EXPIRED;
    const uint64_t used = used_bytes(p->user_dir);
    *remaining = used >= kQuotaBytes ? 0 : static_cast<int64_t>(kQuotaBytes - used);
    return S_OK;
}

HRESULT delete_container(XGameSaveProviderHandle p, const char* name) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (!live(g_providers, p)) return E_GS_HANDLE_EXPIRED;
    if (!valid_container_name(name)) return E_GS_INVALID_CONTAINER_NAME;
    std::error_code ec;
    fs::remove_all(p->user_dir / widen(encode_name(name)), ec);
    return ec ? E_FAIL : S_OK;
}

HRESULT submit_update(XGameSaveUpdateHandle u) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (!live(g_updates, u) || !live_as(g_containers, u->container, u->container_serial)) return E_GS_HANDLE_EXPIRED;
    return commit(u);
}

// ---- Async: each *Async slot runs its work on the title's queue ----

struct Job {
    const void* identity;
    XUserHandle user = nullptr;
    std::string text;   // configId
    XGameSaveProviderHandle provider = nullptr;
    XGameSaveContainerHandle container = nullptr;
    XGameSaveUpdateHandle update = nullptr;
    uint64_t serial = 0;   // the serial of the provider, container or update above, when the job was made
    std::vector<std::string> names;
    bool all_blobs = false;
    // results
    int64_t quota = 0;
    std::vector<BlobRead> blobs;
    std::string folder;
    // tracing only
    const char* name = "";   // the *Async function
    std::string label;       // its arguments, as the trace lines show them
    const XAsyncBlock* title_async = nullptr;   // the title's block: XAsync hands the provider a copy of its own
    uint64_t scheduled_ms = 0;
};

const char kInitIdentity = 0, kQuotaIdentity = 0, kDeleteIdentity = 0, kReadIdentity = 0, kSubmitIdentity = 0,
           kFolderIdentity = 0;

// The blob count of the last ReadBlobDataAsync result collected on this thread (GetResult runs on the caller's).
thread_local uint32_t t_read_count = 0;

HRESULT do_work(Job& job, size_t& size) {
    size = 0;
    if (job.identity == &kInitIdentity) {
        // The handle is made when the title collects the result, so an uncollected result leaks nothing; here only
        // the folder is checked.
        if (!job.user) return E_INVALIDARG;
        const fs::path root = save_root();
        std::error_code ec;
        if (root.empty() || (fs::create_directories(root / L"local-user", ec), ec)) return E_GS_OUT_OF_LOCAL_STORAGE;
        size = sizeof(XGameSaveProviderHandle);
        return S_OK;
    }
    if (job.identity == &kQuotaIdentity) {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        if (!live_as(g_providers, job.provider, job.serial)) return E_GS_HANDLE_EXPIRED;
        const HRESULT hr = remaining_quota(job.provider, &job.quota);
        if (SUCCEEDED(hr)) size = sizeof(int64_t);
        return hr;
    }
    if (job.identity == &kDeleteIdentity) {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        if (!live_as(g_providers, job.provider, job.serial)) return E_GS_HANDLE_EXPIRED;
        return delete_container(job.provider, job.text.c_str());
    }
    if (job.identity == &kSubmitIdentity) {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        if (!live_as(g_updates, job.update, job.serial)) return E_GS_HANDLE_EXPIRED;
        return submit_update(job.update);
    }
    if (job.identity == &kReadIdentity) {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        if (!live_as(g_containers, job.container, job.serial)) return E_GS_HANDLE_EXPIRED;
        std::vector<const char*> names;
        for (const std::string& n : job.names) names.push_back(n.c_str());
        const HRESULT hr = read_blobs(job.container, job.all_blobs ? nullptr : names.data(),
                                      static_cast<uint32_t>(names.size()), job.blobs);
        if (SUCCEEDED(hr)) size = packed_size(job.blobs);
        return hr;
    }
    if (job.identity == &kFolderIdentity) {
        const fs::path root = save_root();
        if (root.empty()) return E_GS_OUT_OF_LOCAL_STORAGE;
        const fs::path dir = root / L"files" / widen(encode_name(job.text));
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) return E_GS_OUT_OF_LOCAL_STORAGE;
        job.folder = narrow(dir.wstring());
        size = job.folder.size() + 1;
        return S_OK;
    }
    return E_UNEXPECTED;
}

HRESULT CALLBACK job_provider(XAsyncOp op, const XAsyncProviderData* data) {
    auto* job = static_cast<Job*>(data->context);
    switch (op) {
    case XAsyncOp::Begin: {
        if (!tracing()) return XAsyncSchedule(data->async, 0);
        // Logged before the call, so it comes before the "run" line; the work may run, complete and free the job
        // before XAsyncSchedule returns, so a failure is logged from copies. `async` is the title's block (the one
        // its *Async and ...Result calls pass), so the lines of one job can be matched up.
        const char* name = job->name;
        const void* async = job->title_async;
        job->scheduled_ms = trace_ms();
        trace("%s schedule async=%p %s", name, async, job->label.c_str());
        const HRESULT hr = XAsyncSchedule(data->async, 0);
        if (FAILED(hr)) trace("%s schedule async=%p failed hr=0x%08lX", name, async, hr_bits(hr));
        return hr;
    }
    case XAsyncOp::DoWork: {
        const bool traced = tracing();
        const uint64_t started = traced ? trace_ms() : 0;
        if (traced)
            trace("%s run async=%p %s waited=%llums", job->name, static_cast<const void*>(job->title_async),
                  job->label.c_str(), static_cast<unsigned long long>(started - std::min(started, job->scheduled_ms)));
        size_t size = 0;
        const HRESULT hr = do_work(*job, size);
        if (traced)   // before XAsyncComplete, which may free the job
            trace("%s complete async=%p %s hr=0x%08lX size=%zu work=%llums", job->name,
                  static_cast<const void*>(job->title_async), job->label.c_str(), hr_bits(hr), SUCCEEDED(hr) ? size : 0,
                  static_cast<unsigned long long>(trace_ms() - started));
        XAsyncComplete(data->async, hr, SUCCEEDED(hr) ? size : 0);
        return S_OK;
    }
    case XAsyncOp::GetResult:
        if (!data->buffer) return E_INVALIDARG;
        if (job->identity == &kInitIdentity) {
            if (data->bufferSize < sizeof(XGameSaveProviderHandle)) return E_INVALIDARG;
            return open_provider(job->user, job->text.c_str(), static_cast<XGameSaveProviderHandle*>(data->buffer));
        }
        if (job->identity == &kQuotaIdentity) {
            if (data->bufferSize < sizeof(int64_t)) return E_INVALIDARG;
            *static_cast<int64_t*>(data->buffer) = job->quota;
            return S_OK;
        }
        if (job->identity == &kReadIdentity) {
            if (data->bufferSize < packed_size(job->blobs)) return E_GS_PROVIDED_BUFFER_TOO_SMALL;
            pack(job->blobs, data->buffer, data->bufferSize);
            t_read_count = static_cast<uint32_t>(job->blobs.size());
            return S_OK;
        }
        if (job->identity == &kFolderIdentity) {
            if (data->bufferSize < job->folder.size() + 1) return E_GS_PROVIDED_BUFFER_TOO_SMALL;
            std::memcpy(data->buffer, job->folder.c_str(), job->folder.size() + 1);
            return S_OK;
        }
        return S_OK;
    case XAsyncOp::Cleanup:
        delete job;
        return S_OK;
    default:
        return S_OK;
    }
}

// `label`: the job's arguments for the trace lines, made only while tracing.
HRESULT begin(XAsyncBlock* async, Job* job, const char* name, const std::string& label = {}) {
    job->name = name;
    job->label = label;
    job->title_async = async;
    HRESULT hr = E_POINTER;
    if (async) {
        hr = XAsyncBegin(async, job, job->identity, name, job_provider);
        if (FAILED(hr)) delete job;
    } else {
        delete job;
    }
    TRACE("%s async=%p %s hr=0x%08lX", name, static_cast<void*>(async), label.c_str(), hr_bits(hr));
    return hr;
}

// ---- IXGameSaveImpl (slots 3-29), IXGameSaveImpl2 (30-32) ----

// 3: HRESULT XGameSaveInitializeProvider(XUserHandle requestingUser, const char* configurationId, bool syncOnDemand,
//                                         XGameSaveProviderHandle* provider)
HRESULT STDMETHODCALLTYPE save_init(void*, XUserHandle user, const char* config_id, bool sync,
                                    XGameSaveProviderHandle* out) {
    const HRESULT hr = open_provider(user, config_id, out);
    TRACE("XGameSaveInitializeProvider user=%p config=\"%s\" syncOnDemand=%d provider=%p hr=0x%08lX", user,
          or_null(config_id), sync ? 1 : 0, out ? static_cast<void*>(*out) : nullptr, hr_bits(hr));
    return hr;
}

// 4: HRESULT XGameSaveInitializeProviderAsync(XUserHandle, const char* configurationId, bool syncOnDemand, XAsyncBlock*)
HRESULT STDMETHODCALLTYPE save_init_async(void*, XUserHandle user, const char* config_id, bool, XAsyncBlock* async) {
    if (!user || !config_id) {
        TRACE("XGameSaveInitializeProviderAsync async=%p user=%p config=\"%s\" hr=0x%08lX", static_cast<void*>(async),
              user, or_null(config_id), hr_bits(E_INVALIDARG));
        return E_INVALIDARG;
    }
    auto* job = new Job{&kInitIdentity};
    job->user = user;
    job->text = config_id;
    return begin(async, job, "XGameSaveInitializeProviderAsync",
                 tracing() ? "config=\"" + job->text + "\"" : std::string());
}

// 5: HRESULT XGameSaveInitializeProviderResult(XAsyncBlock* async, XGameSaveProviderHandle* provider)
HRESULT STDMETHODCALLTYPE save_init_result(void*, XAsyncBlock* async, XGameSaveProviderHandle* out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    const HRESULT hr = XAsyncGetResult(async, &kInitIdentity, sizeof(*out), out, nullptr);
    TRACE("XGameSaveInitializeProviderResult async=%p provider=%p hr=0x%08lX", static_cast<void*>(async),
          static_cast<void*>(*out), hr_bits(hr));
    return hr;
}

// 6: void XGameSaveCloseProvider(XGameSaveProviderHandle provider)
void STDMETHODCALLTYPE save_close_provider(void*, XGameSaveProviderHandle p) {
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        const bool open = p && g_providers.erase(p) != 0;
        TRACE("XGameSaveCloseProvider provider=%p%s", static_cast<void*>(p), open ? "" : " (not open)");
        if (!open) return;
    }
    delete p;
}

// 7: HRESULT XGameSaveGetRemainingQuota(XGameSaveProviderHandle provider, int64_t* remainingQuota)
HRESULT STDMETHODCALLTYPE save_quota(void*, XGameSaveProviderHandle p, int64_t* remaining) {
    const HRESULT hr = remaining_quota(p, remaining);
    TRACE("XGameSaveGetRemainingQuota provider=%p remaining=%lld hr=0x%08lX", static_cast<void*>(p),
          SUCCEEDED(hr) ? static_cast<long long>(*remaining) : 0ll, hr_bits(hr));
    return hr;
}

// 8: HRESULT XGameSaveGetRemainingQuotaAsync(XGameSaveProviderHandle provider, XAsyncBlock* async)
HRESULT STDMETHODCALLTYPE save_quota_async(void*, XGameSaveProviderHandle p, XAsyncBlock* async) {
    auto* job = new Job{&kQuotaIdentity};
    job->provider = p;
    job->serial = serial_of(g_providers, p);
    char label[64] = {};
    if (tracing()) std::snprintf(label, sizeof(label), "provider=%p", static_cast<void*>(p));
    return begin(async, job, "XGameSaveGetRemainingQuotaAsync", label);
}

// 9: HRESULT XGameSaveGetRemainingQuotaResult(XAsyncBlock* async, int64_t* remainingQuota)
HRESULT STDMETHODCALLTYPE save_quota_result(void*, XAsyncBlock* async, int64_t* remaining) {
    if (!remaining) return E_POINTER;
    const HRESULT hr = XAsyncGetResult(async, &kQuotaIdentity, sizeof(*remaining), remaining, nullptr);
    TRACE("XGameSaveGetRemainingQuotaResult async=%p remaining=%lld hr=0x%08lX", static_cast<void*>(async),
          SUCCEEDED(hr) ? static_cast<long long>(*remaining) : 0ll, hr_bits(hr));
    return hr;
}

// 10: HRESULT XGameSaveDeleteContainer(XGameSaveProviderHandle provider, const char* containerName)
HRESULT STDMETHODCALLTYPE save_delete(void*, XGameSaveProviderHandle p, const char* name) {
    const HRESULT hr = delete_container(p, name);
    TRACE("XGameSaveDeleteContainer provider=%p container=\"%s\" hr=0x%08lX", static_cast<void*>(p), or_null(name),
          hr_bits(hr));
    return hr;
}

// 11: HRESULT XGameSaveDeleteContainerAsync(XGameSaveProviderHandle provider, const char* containerName, XAsyncBlock*)
HRESULT STDMETHODCALLTYPE save_delete_async(void*, XGameSaveProviderHandle p, const char* name, XAsyncBlock* async) {
    if (!valid_container_name(name)) {
        TRACE("XGameSaveDeleteContainerAsync async=%p provider=%p container=\"%s\" hr=0x%08lX",
              static_cast<void*>(async), static_cast<void*>(p), or_null(name), hr_bits(E_GS_INVALID_CONTAINER_NAME));
        return E_GS_INVALID_CONTAINER_NAME;
    }
    auto* job = new Job{&kDeleteIdentity};
    job->provider = p;
    job->serial = serial_of(g_providers, p);
    job->text = name;
    char label[64] = {};
    if (tracing()) std::snprintf(label, sizeof(label), "provider=%p", static_cast<void*>(p));
    return begin(async, job, "XGameSaveDeleteContainerAsync",
                 tracing() ? std::string(label) + " container=\"" + job->text + "\"" : std::string());
}

// 12: HRESULT XGameSaveDeleteContainerResult(XAsyncBlock* async)
HRESULT STDMETHODCALLTYPE save_delete_result(void*, XAsyncBlock* async) {
    const HRESULT hr = XAsyncGetResult(async, &kDeleteIdentity, 0, nullptr, nullptr);
    TRACE("XGameSaveDeleteContainerResult async=%p hr=0x%08lX", static_cast<void*>(async), hr_bits(hr));
    return hr;
}

// The enumerations count what they hand the title, for the trace.
struct ContainerCounter {
    ContainerInfoCallback cb;
    void* context;
    uint32_t calls;
    uint64_t bytes;
};
bool CALLBACK count_container(const XGameSaveContainerInfo* info, void* context) {
    auto* c = static_cast<ContainerCounter*>(context);
    ++c->calls;
    c->bytes += info->totalSize;
    return c->cb(info, c->context);
}
struct BlobCounter {
    BlobInfoCallback cb;
    void* context;
    uint32_t calls;
    uint64_t bytes;
};
bool CALLBACK count_blob(const XGameSaveBlobInfo* info, void* context) {
    auto* c = static_cast<BlobCounter*>(context);
    ++c->calls;
    c->bytes += info->size;
    return c->cb(info, c->context);
}

// 13: HRESULT XGameSaveGetContainerInfo(XGameSaveProviderHandle, const char* containerName, void* context,
//                                       XGameSaveContainerInfoCallback* callback)
HRESULT get_container_info(XGameSaveProviderHandle p, const char* name, void* context, ContainerInfoCallback cb) {
    if (!cb) return E_INVALIDARG;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (!live(g_providers, p)) return E_GS_HANDLE_EXPIRED;
    if (!valid_container_name(name)) return E_GS_INVALID_CONTAINER_NAME;
    const fs::path dir = p->user_dir / widen(encode_name(name));
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return S_OK;
    const ContainerRecord c = describe(name, dir);
    const XGameSaveContainerInfo info{c.name.c_str(), c.display_name.c_str(), c.blob_count, c.total_size, c.modified,
                                      false};
    cb(&info, context);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE save_container_info(void*, XGameSaveProviderHandle p, const char* name, void* context,
                                              ContainerInfoCallback cb) {
    if (!tracing() || !cb) return get_container_info(p, name, context, cb);
    ContainerCounter counter{cb, context, 0, 0};
    const HRESULT hr = get_container_info(p, name, &counter, count_container);
    trace("XGameSaveGetContainerInfo provider=%p container=\"%s\" found=%u size=%llu hr=0x%08lX",
          static_cast<void*>(p), or_null(name), counter.calls, static_cast<unsigned long long>(counter.bytes),
          hr_bits(hr));
    return hr;
}

HRESULT enumerate_container_info(XGameSaveProviderHandle p, std::string_view prefix, void* context,
                                 ContainerInfoCallback cb) {
    if (!cb) return E_INVALIDARG;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (!live(g_providers, p)) return E_GS_HANDLE_EXPIRED;
    enumerate_containers(p->user_dir, prefix, context, cb);
    return S_OK;
}
HRESULT traced_enumerate_container_info(const char* call, XGameSaveProviderHandle p, const char* prefix, void* context,
                                        ContainerInfoCallback cb) {
    const std::string_view pre = prefix ? std::string_view(prefix) : std::string_view{};
    if (!tracing() || !cb) return enumerate_container_info(p, pre, context, cb);
    ContainerCounter counter{cb, context, 0, 0};
    const HRESULT hr = enumerate_container_info(p, pre, &counter, count_container);
    trace("%s provider=%p prefix=\"%s\" containers=%u size=%llu hr=0x%08lX", call, static_cast<void*>(p),
          prefix ? prefix : "", counter.calls, static_cast<unsigned long long>(counter.bytes), hr_bits(hr));
    return hr;
}

// 14: HRESULT XGameSaveEnumerateContainerInfo(XGameSaveProviderHandle, void* context, XGameSaveContainerInfoCallback*)
HRESULT STDMETHODCALLTYPE save_enum_containers(void*, XGameSaveProviderHandle p, void* context, ContainerInfoCallback cb) {
    return traced_enumerate_container_info("XGameSaveEnumerateContainerInfo", p, nullptr, context, cb);
}

// 15: HRESULT XGameSaveEnumerateContainerInfoByName(XGameSaveProviderHandle, const char* containerNamePrefix,
//                                                   void* context, XGameSaveContainerInfoCallback*)
HRESULT STDMETHODCALLTYPE save_enum_containers_by_name(void*, XGameSaveProviderHandle p, const char* prefix,
                                                       void* context, ContainerInfoCallback cb) {
    return traced_enumerate_container_info("XGameSaveEnumerateContainerInfoByName", p, prefix, context, cb);
}

// 16: HRESULT XGameSaveCreateContainer(XGameSaveProviderHandle, const char* containerName, XGameSaveContainerHandle*).
// The folder appears when the first update is submitted.
HRESULT create_container(XGameSaveProviderHandle p, const char* name, XGameSaveContainerHandle* out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (!live(g_providers, p)) return E_GS_HANDLE_EXPIRED;
    if (!valid_container_name(name)) return E_GS_INVALID_CONTAINER_NAME;
    auto* c = new XGameSaveContainer{p->user_dir, name};
    c->serial = g_next_serial++;
    g_containers.insert(c);
    *out = c;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE save_create_container(void*, XGameSaveProviderHandle p, const char* name,
                                                XGameSaveContainerHandle* out) {
    const HRESULT hr = create_container(p, name, out);
    TRACE("XGameSaveCreateContainer provider=%p container=\"%s\" handle=%p hr=0x%08lX", static_cast<void*>(p),
          or_null(name), out ? static_cast<void*>(*out) : nullptr, hr_bits(hr));
    return hr;
}

// 17: void XGameSaveCloseContainer(XGameSaveContainerHandle context)
void STDMETHODCALLTYPE save_close_container(void*, XGameSaveContainerHandle c) {
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        if (tracing()) {
            trace("XGameSaveCloseContainer handle=%p container=%s", static_cast<void*>(c),
                  live(g_containers, c) ? container_label(c).c_str() : "? (not open)");
        }
        if (!c || g_containers.erase(c) == 0) return;
    }
    delete c;
}

HRESULT enumerate_blobs(XGameSaveContainerHandle c, std::string_view prefix, void* context, BlobInfoCallback cb) {
    if (!cb) return E_INVALIDARG;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (!live(g_containers, c)) return E_GS_HANDLE_EXPIRED;
    for (const BlobFile& b : list_blobs(c->dir())) {
        if (b.name.compare(0, prefix.size(), prefix) != 0) continue;
        const XGameSaveBlobInfo info{b.name.c_str(), static_cast<uint32_t>(b.size)};
        if (!cb(&info, context)) break;
    }
    return S_OK;
}
HRESULT traced_enumerate_blobs(const char* call, XGameSaveContainerHandle c, const char* prefix, void* context,
                               BlobInfoCallback cb) {
    const std::string_view pre = prefix ? std::string_view(prefix) : std::string_view{};
    if (!tracing() || !cb) return enumerate_blobs(c, pre, context, cb);
    BlobCounter counter{cb, context, 0, 0};
    const std::string label = container_label(c);
    const HRESULT hr = enumerate_blobs(c, pre, &counter, count_blob);
    trace("%s handle=%p container=%s prefix=\"%s\" blobs=%u size=%llu hr=0x%08lX", call, static_cast<void*>(c),
          label.c_str(), prefix ? prefix : "", counter.calls, static_cast<unsigned long long>(counter.bytes),
          hr_bits(hr));
    return hr;
}

// 18: HRESULT XGameSaveEnumerateBlobInfo(XGameSaveContainerHandle, void* context, XGameSaveBlobInfoCallback*)
HRESULT STDMETHODCALLTYPE save_enum_blobs(void*, XGameSaveContainerHandle c, void* context, BlobInfoCallback cb) {
    return traced_enumerate_blobs("XGameSaveEnumerateBlobInfo", c, nullptr, context, cb);
}

// 19: HRESULT XGameSaveEnumerateBlobInfoByName(XGameSaveContainerHandle, const char* blobNamePrefix, void* context,
//                                              XGameSaveBlobInfoCallback*)
HRESULT STDMETHODCALLTYPE save_enum_blobs_by_name(void*, XGameSaveContainerHandle c, const char* prefix, void* context,
                                                  BlobInfoCallback cb) {
    return traced_enumerate_blobs("XGameSaveEnumerateBlobInfoByName", c, prefix, context, cb);
}

// "name":size,... of the blobs a read handed the title (names and sizes only).
std::string read_blobs_label(const XGameSaveBlob* blobs, uint32_t count) {
    std::string out;
    for (uint32_t i = 0; i < count && i < 8; ++i)
        out += (i ? ",\"" : "\"") + std::string(or_null(blobs[i].info.name)) + "\":" + std::to_string(blobs[i].info.size);
    if (count > 8) out += ",+" + std::to_string(count - 8);
    return out.empty() ? std::string("(none)") : out;
}

// The names a title asks to read, "(all)" for blobNames = nullptr.
std::string requested_names(const char* const* names, uint32_t count) {
    if (!names) return "(all)";
    std::vector<std::string> list;
    for (uint32_t i = 0; i < count; ++i) list.emplace_back(or_null(names[i]));
    return quoted_names(list);
}

// 20: HRESULT XGameSaveReadBlobData(XGameSaveContainerHandle, const char** blobNames, uint32_t* countOfBlobs,
//                                   size_t blobsSize, XGameSaveBlob* blobData)
HRESULT read_blob_data(XGameSaveContainerHandle c, const char** names, uint32_t* count, size_t size,
                       XGameSaveBlob* out) {
    if (!count || !out) return E_POINTER;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (!live(g_containers, c)) return E_GS_HANDLE_EXPIRED;
    std::vector<BlobRead> blobs;
    const HRESULT hr = read_blobs(c, names, names ? *count : 0, blobs);
    if (FAILED(hr)) return hr;
    if (size < packed_size(blobs)) return E_GS_PROVIDED_BUFFER_TOO_SMALL;
    pack(blobs, out, size);
    *count = static_cast<uint32_t>(blobs.size());
    return S_OK;
}
HRESULT STDMETHODCALLTYPE save_read(void*, XGameSaveContainerHandle c, const char** names, uint32_t* count,
                                    size_t size, XGameSaveBlob* out) {
    if (!tracing()) return read_blob_data(c, names, count, size, out);
    const std::string label = container_label(c), wanted = requested_names(names, names && count ? *count : 0);
    const HRESULT hr = read_blob_data(c, names, count, size, out);
    trace("XGameSaveReadBlobData handle=%p container=%s names=%s buffer=%zu read=%s hr=0x%08lX",
          static_cast<void*>(c), label.c_str(), wanted.c_str(), size,
          SUCCEEDED(hr) ? read_blobs_label(out, *count).c_str() : "(none)", hr_bits(hr));
    return hr;
}

// 21: HRESULT XGameSaveReadBlobDataAsync(XGameSaveContainerHandle, const char** blobNames, uint32_t countOfBlobs,
//                                        XAsyncBlock* async). The names are copied: the title may free them at once.
HRESULT STDMETHODCALLTYPE save_read_async(void*, XGameSaveContainerHandle c, const char** names, uint32_t count,
                                          XAsyncBlock* async) {
    auto* job = new Job{&kReadIdentity};
    job->container = c;
    job->serial = serial_of(g_containers, c);
    job->all_blobs = names == nullptr;
    for (uint32_t i = 0; names && i < count; ++i) {
        if (!valid_blob_name(names[i])) {
            delete job;
            TRACE("XGameSaveReadBlobDataAsync async=%p handle=%p hr=0x%08lX", static_cast<void*>(async),
                  static_cast<void*>(c), hr_bits(E_INVALIDARG));
            return E_INVALIDARG;
        }
        job->names.emplace_back(names[i]);
    }
    std::string label;
    if (tracing()) {
        char handle[48];
        std::snprintf(handle, sizeof(handle), "handle=%p", static_cast<void*>(c));
        label = std::string(handle) + " container=" + container_label(c) +
                " names=" + (job->all_blobs ? std::string("(all)") : quoted_names(job->names));
    }
    return begin(async, job, "XGameSaveReadBlobDataAsync", label);
}

// 22: HRESULT XGameSaveReadBlobDataResult(XAsyncBlock* async, size_t blobsSize, XGameSaveBlob* blobData,
//                                         uint32_t* countOfBlobs). blobsSize: XAsyncGetResultSize's answer.
HRESULT STDMETHODCALLTYPE save_read_result(void*, XAsyncBlock* async, size_t size, XGameSaveBlob* out, uint32_t* count) {
    if (!out || !count) return E_POINTER;
    *count = 0;
    t_read_count = 0;
    HRESULT hr = XAsyncGetResult(async, &kReadIdentity, size, out, nullptr);
    if (hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) || hr == E_NOT_SUFFICIENT_BUFFER)
        hr = E_GS_PROVIDED_BUFFER_TOO_SMALL;
    if (SUCCEEDED(hr)) *count = t_read_count;
    TRACE("XGameSaveReadBlobDataResult async=%p buffer=%zu read=%s hr=0x%08lX", static_cast<void*>(async), size,
          SUCCEEDED(hr) ? read_blobs_label(out, *count).c_str() : "(none)", hr_bits(hr));
    return hr;
}

// 23: HRESULT XGameSaveCreateUpdate(XGameSaveContainerHandle, const char* containerDisplayName, XGameSaveUpdateHandle*)
HRESULT create_update(XGameSaveContainerHandle c, const char* display_name, XGameSaveUpdateHandle* out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (!live(g_containers, c)) return E_GS_HANDLE_EXPIRED;
    auto* u = new XGameSaveUpdate{c, c->serial, display_name ? display_name : "", display_name != nullptr, {}, 0};
    u->serial = g_next_serial++;
    g_updates.insert(u);
    *out = u;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE save_create_update(void*, XGameSaveContainerHandle c, const char* display_name,
                                             XGameSaveUpdateHandle* out) {
    if (!tracing()) return create_update(c, display_name, out);
    const std::string label = container_label(c);
    const HRESULT hr = create_update(c, display_name, out);
    trace("XGameSaveCreateUpdate handle=%p container=%s display=\"%s\" update=%p hr=0x%08lX", static_cast<void*>(c),
          label.c_str(), or_null(display_name), out ? static_cast<void*>(*out) : nullptr, hr_bits(hr));
    return hr;
}

// 24: void XGameSaveCloseUpdate(XGameSaveUpdateHandle context)
void STDMETHODCALLTYPE save_close_update(void*, XGameSaveUpdateHandle u) {
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        const bool open = u && g_updates.erase(u) != 0;
        TRACE("XGameSaveCloseUpdate update=%p%s", static_cast<void*>(u), open ? "" : " (not open)");
        if (!open) return;
    }
    delete u;
}

// 25: HRESULT XGameSaveSubmitBlobWrite(XGameSaveUpdateHandle, const char* blobName, const uint8_t* data, size_t size).
// One write or delete per blob per update; the update (and so each blob) stays within GS_MAX_BLOB_SIZE.
HRESULT submit_blob_write(XGameSaveUpdateHandle u, const char* name, const uint8_t* data, size_t size) {
    if (!valid_blob_name(name) || (!data && size)) return E_INVALIDARG;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (!live(g_updates, u)) return E_GS_HANDLE_EXPIRED;
    if (u->ops.count(name)) return E_INVALIDARG;
    if (size > kMaxUpdateBytes || u->bytes + size > kMaxUpdateBytes) return E_GS_UPDATE_TOO_BIG;
    u->ops.emplace(name, Op{true, std::vector<uint8_t>(data, data + size)});
    u->bytes += size;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE save_write(void*, XGameSaveUpdateHandle u, const char* name, const uint8_t* data, size_t size) {
    const HRESULT hr = submit_blob_write(u, name, data, size);
    TRACE("XGameSaveSubmitBlobWrite update=%p blob=\"%s\" size=%zu hr=0x%08lX", static_cast<void*>(u), or_null(name),
          size, hr_bits(hr));
    return hr;
}

// 26: HRESULT XGameSaveSubmitBlobDelete(XGameSaveUpdateHandle, const char* blobName)
HRESULT submit_blob_delete(XGameSaveUpdateHandle u, const char* name) {
    if (!valid_blob_name(name)) return E_INVALIDARG;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (!live(g_updates, u)) return E_GS_HANDLE_EXPIRED;
    if (u->ops.count(name)) return E_INVALIDARG;
    u->ops.emplace(name, Op{false, {}});
    return S_OK;
}
HRESULT STDMETHODCALLTYPE save_delete_blob(void*, XGameSaveUpdateHandle u, const char* name) {
    const HRESULT hr = submit_blob_delete(u, name);
    TRACE("XGameSaveSubmitBlobDelete update=%p blob=\"%s\" hr=0x%08lX", static_cast<void*>(u), or_null(name),
          hr_bits(hr));
    return hr;
}

// 27: HRESULT XGameSaveSubmitUpdate(XGameSaveUpdateHandle update)
HRESULT STDMETHODCALLTYPE save_submit(void*, XGameSaveUpdateHandle u) {
    if (!tracing()) return submit_update(u);
    const std::string label = update_label(u);
    const uint64_t started = trace_ms();
    const HRESULT hr = submit_update(u);
    trace("XGameSaveSubmitUpdate update=%p container=%s hr=0x%08lX work=%llums", static_cast<void*>(u), label.c_str(),
          hr_bits(hr), static_cast<unsigned long long>(trace_ms() - started));
    return hr;
}

// 28: HRESULT XGameSaveSubmitUpdateAsync(XGameSaveUpdateHandle update, XAsyncBlock* async)
HRESULT STDMETHODCALLTYPE save_submit_async(void*, XGameSaveUpdateHandle u, XAsyncBlock* async) {
    auto* job = new Job{&kSubmitIdentity};
    job->update = u;
    job->serial = serial_of(g_updates, u);
    std::string label;
    if (tracing()) {
        char handle[48];
        std::snprintf(handle, sizeof(handle), "update=%p", static_cast<void*>(u));
        label = std::string(handle) + " container=" + update_label(u);
    }
    return begin(async, job, "XGameSaveSubmitUpdateAsync", label);
}

// 29: HRESULT XGameSaveSubmitUpdateResult(XAsyncBlock* async)
HRESULT STDMETHODCALLTYPE save_submit_result(void*, XAsyncBlock* async) {
    const HRESULT hr = XAsyncGetResult(async, &kSubmitIdentity, 0, nullptr, nullptr);
    TRACE("XGameSaveSubmitUpdateResult async=%p hr=0x%08lX", static_cast<void*>(async), hr_bits(hr));
    return hr;
}

// 30: HRESULT XGameSaveFilesGetFolderWithUiAsync(XUserHandle requestingUser, const char* configurationId,
//                                                XAsyncBlock* async): a folder of its own, <root>\files\<configId>.
HRESULT STDMETHODCALLTYPE files_folder_async(void*, XUserHandle user, const char* config_id, XAsyncBlock* async) {
    if (!user || !config_id) {
        TRACE("XGameSaveFilesGetFolderWithUiAsync async=%p user=%p config=\"%s\" hr=0x%08lX",
              static_cast<void*>(async), user, or_null(config_id), hr_bits(E_INVALIDARG));
        return E_INVALIDARG;
    }
    auto* job = new Job{&kFolderIdentity};
    job->user = user;
    job->text = config_id;
    return begin(async, job, "XGameSaveFilesGetFolderWithUiAsync",
                 tracing() ? "config=\"" + job->text + "\"" : std::string());
}

// 31: HRESULT XGameSaveFilesGetFolderWithUiResult(XAsyncBlock* async, size_t folderSize, char* folderResult)
HRESULT STDMETHODCALLTYPE files_folder_result(void*, XAsyncBlock* async, size_t size, char* folder) {
    if (!folder) return E_POINTER;
    HRESULT hr = XAsyncGetResult(async, &kFolderIdentity, size, folder, nullptr);
    if (hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) || hr == E_NOT_SUFFICIENT_BUFFER)
        hr = E_GS_PROVIDED_BUFFER_TOO_SMALL;
    TRACE("XGameSaveFilesGetFolderWithUiResult async=%p buffer=%zu folder=\"%s\" hr=0x%08lX",
          static_cast<void*>(async), size, SUCCEEDED(hr) ? folder : "", hr_bits(hr));
    return hr;
}

// 32: HRESULT XGameSaveFilesGetRemainingQuota(XUserHandle requestingUser, const char* configurationId,
//                                             int64_t* remainingQuota)
HRESULT files_remaining_quota(XUserHandle user, const char* config_id, int64_t* remaining) {
    if (!remaining) return E_POINTER;
    if (!user || !config_id) return E_INVALIDARG;
    const fs::path root = save_root();
    if (root.empty()) return E_GS_OUT_OF_LOCAL_STORAGE;
    uint64_t used = 0;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(root / L"files" / widen(encode_name(config_id)), ec))
        if (e.is_regular_file(ec)) used += static_cast<uint64_t>(e.file_size(ec));
    *remaining = used >= kQuotaBytes ? 0 : static_cast<int64_t>(kQuotaBytes - used);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE files_quota(void*, XUserHandle user, const char* config_id, int64_t* remaining) {
    const HRESULT hr = files_remaining_quota(user, config_id, remaining);
    TRACE("XGameSaveFilesGetRemainingQuota user=%p config=\"%s\" remaining=%lld hr=0x%08lX", user, or_null(config_id),
          SUCCEEDED(hr) ? static_cast<long long>(*remaining) : 0ll, hr_bits(hr));
    return hr;
}

// ---- The object ----

constexpr GUID kSaveClass = {0x704c3f58, 0xe629, 0x4cc2, {0xb1, 0x97, 0x30, 0x51, 0x1b, 0x99, 0x6f, 0xe2}};
const GUID kSaveInterfaces[] = {
    kSaveClass,                                                                             // IXGameSaveImpl
    {0x704c3f58, 0xe629, 0x4cc2, {0xb1, 0x97, 0x30, 0x51, 0x1b, 0x99, 0x6e, 0xe2}},         // IXGameSaveImpl2
    {0x1bfff3af, 0xf14a, 0x40a3, {0x8e, 0x35, 0x9a, 0xda, 0x90, 0x65, 0x93, 0xf9}},         // IXGameSaveImpl3 (no methods)
};
constexpr GUID kIUnknown = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

struct SaveObject {
    const void* const* vtable;
};
SaveObject* save_object();

HRESULT query(REFIID iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid == kIUnknown) {
        *out = save_object();
        return S_OK;
    }
    for (const GUID& id : kSaveInterfaces)
        if (iid == id) {
            *out = save_object();
            return S_OK;
        }
    return E_NOINTERFACE;
}

// A process-wide singleton, as the other runtime classes: AddRef and Release don't free.
HRESULT STDMETHODCALLTYPE object_query_interface(void*, REFIID iid, void** out) { return query(iid, out); }
ULONG STDMETHODCALLTYPE object_add_ref(void*) { return 2; }
ULONG STDMETHODCALLTYPE object_release(void*) { return 1; }

#define FN(f) reinterpret_cast<const void*>(&f)

const void* const kSaveVtable[] = {
    FN(object_query_interface), FN(object_add_ref), FN(object_release),   // 0-2  IUnknown
    FN(save_init),                      // 3  XGameSaveInitializeProvider
    FN(save_init_async),                // 4  XGameSaveInitializeProviderAsync
    FN(save_init_result),               // 5  XGameSaveInitializeProviderResult
    FN(save_close_provider),            // 6  XGameSaveCloseProvider
    FN(save_quota),                     // 7  XGameSaveGetRemainingQuota
    FN(save_quota_async),               // 8  XGameSaveGetRemainingQuotaAsync
    FN(save_quota_result),              // 9  XGameSaveGetRemainingQuotaResult
    FN(save_delete),                    // 10 XGameSaveDeleteContainer
    FN(save_delete_async),              // 11 XGameSaveDeleteContainerAsync
    FN(save_delete_result),             // 12 XGameSaveDeleteContainerResult
    FN(save_container_info),            // 13 XGameSaveGetContainerInfo
    FN(save_enum_containers),           // 14 XGameSaveEnumerateContainerInfo
    FN(save_enum_containers_by_name),   // 15 XGameSaveEnumerateContainerInfoByName
    FN(save_create_container),          // 16 XGameSaveCreateContainer
    FN(save_close_container),           // 17 XGameSaveCloseContainer
    FN(save_enum_blobs),                // 18 XGameSaveEnumerateBlobInfo
    FN(save_enum_blobs_by_name),        // 19 XGameSaveEnumerateBlobInfoByName
    FN(save_read),                      // 20 XGameSaveReadBlobData
    FN(save_read_async),                // 21 XGameSaveReadBlobDataAsync
    FN(save_read_result),               // 22 XGameSaveReadBlobDataResult
    FN(save_create_update),             // 23 XGameSaveCreateUpdate
    FN(save_close_update),              // 24 XGameSaveCloseUpdate
    FN(save_write),                     // 25 XGameSaveSubmitBlobWrite
    FN(save_delete_blob),               // 26 XGameSaveSubmitBlobDelete
    FN(save_submit),                    // 27 XGameSaveSubmitUpdate
    FN(save_submit_async),              // 28 XGameSaveSubmitUpdateAsync
    FN(save_submit_result),             // 29 XGameSaveSubmitUpdateResult
    FN(files_folder_async),             // 30 XGameSaveFilesGetFolderWithUiAsync (2)
    FN(files_folder_result),            // 31 XGameSaveFilesGetFolderWithUiResult (2)
    FN(files_quota),                    // 32 XGameSaveFilesGetRemainingQuota (2)
};
static_assert(std::size(kSaveVtable) == 33, "IUnknown + IXGameSaveImpl's 27 methods + IXGameSaveImpl2's 3");

#undef FN

SaveObject* save_object() {
    static SaveObject o{kSaveVtable};
    return &o;
}

}  // namespace

namespace rt::shims {

// Called by xgameruntime.cpp's QueryApiImpl for every class: true when the class is XGameSaveImpl, with *hr set.
bool xgameruntime_gamesave_query(const GUID& class_id, REFIID interface_id, void** out, HRESULT* hr) {
    if (class_id != kSaveClass) return false;
    *hr = query(interface_id, out);
    return true;
}

// For tests: turns LEONIDA_TRACE_XGAMESAVE on and sends its lines to `sink` instead of stderr; nullptr turns it off.
void xgameruntime_gamesave_trace_for_test(void (*sink)(const char* line)) {
    g_trace_sink.store(sink, std::memory_order_release);
    g_trace.store(sink ? 1 : 0, std::memory_order_relaxed);
}

// For tests: the provider, container and update handles a title holds open.
long xgameruntime_gamesave_open_handles() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return static_cast<long>(g_providers.size() + g_containers.size() + g_updates.size());
}

}  // namespace rt::shims
