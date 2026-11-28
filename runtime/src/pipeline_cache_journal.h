// One Vulkan pipeline cache file per compiled compute pipeline: the "pipeline journal".
//
// The session cache (pipeline_cache_store.h) is one blob written at device teardown or at an idle checkpoint. A boot
// that ends by a crash, a device loss or `timeout` never tears the device down, and while the compute warm-up thread
// has builds queued the idle checkpoint never runs, so the hundreds of large compute pipelines a boot compiles were
// compiled again on every boot. The journal keeps each one in its own small cache file the moment it is built:
//
//   <cache dir>/pipeline_cache.d/cs-<16 hex digits>.bin
//
// The name is a hash of what the pipeline is made of (the SPIR-V words, the stage's subgroup request and the layout).
// Each file is a complete VkPipelineCache blob holding one pipeline. A build reads the file, if any, into a
// VkPipelineCache of its own and creates the pipeline against it: the driver finds the pipeline there (a hit, a few
// milliseconds where the compile took seconds) or compiles it and the file is written again. Files are published
// whole (write_pipeline_cache_file: a closed staging file renamed over the target), so a process killed mid-write
// leaves the previous file or none, and a boot that dies keeps everything it built before.
//
// The hash only picks the file. Whether the pipeline in it may be used is the driver's decision: it checks the blob
// header (vendor, device, pipeline-cache UUID; checked here first too, so a driver update starts over) and its own key.
//
// Upkeep (tidy, at the first pipeline of a device): staging files a killed writer left behind are removed once they are
// ten minutes old, and past LEONIDA_PIPELINE_JOURNAL_BYTES the least recently used files go until three quarters of
// it is left. A file's modification time is its last use: written, or touched when a build found its pipeline there.
//
// Pure stdlib apart from pipeline_cache_file.h's Windows rename retry; tested by pipeline_cache_journal_test.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

#include "cached_pso.h"
#include "pipeline_cache_store.h"  // before pipeline_cache_file.h, whose <windows.h> may define max
#include "pipeline_cache_file.h"

namespace leonida::pipeline_journal {

// Bumped when what goes into a key changes, so files named under the old scheme are no longer looked up.
inline constexpr std::uint32_t key_version = 1;

// A file larger than this is not read (a corrupt or foreign file; one pipeline's cache is a few MiB at most).
inline constexpr std::size_t entry_read_limit = std::size_t{256} << 20;

// 64-bit FNV-1a. Not cryptographic: it names a file, and a collision costs one compile, not a wrong pipeline.
class Hasher {
public:
    Hasher() { value(key_version); }

    void bytes(const void* data, std::size_t size) {
        if (!data || size == 0) return;
        const auto* p = static_cast<const std::uint8_t*>(data);
        std::uint64_t h = hash_;
        for (std::size_t i = 0; i < size; ++i) {
            h ^= p[i];
            h *= 0x100000001b3ull;
        }
        hash_ = h;
    }

    template<class T> void value(const T& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        bytes(&v, sizeof(v));
    }

    void text(const char* s) {
        const std::string_view view(s ? s : "");
        value(std::uint64_t(view.size()));
        bytes(view.data(), view.size());
    }

    std::uint64_t digest() const { return hash_; }

private:
    std::uint64_t hash_ = 0xcbf29ce484222325ull;
};

// <dir>/<kind>-<key as 16 lowercase hex digits>.bin
inline std::filesystem::path entry_path(const std::filesystem::path& dir, const char* kind, std::uint64_t key) {
    char suffix[24];
    std::snprintf(suffix, sizeof(suffix), "-%016llx.bin", static_cast<unsigned long long>(key));
    std::string name(kind && *kind ? kind : "entry");
    name += suffix;
    return dir / name;
}

// Resolve the two roots once, not on the compile path. With no seed keep the original single-root behavior.
// An explicit bad/overlapping pair disables the journal rather than accidentally publishing into the seed.
// This is a configuration check, not a substitute for a read-only mount/ACL and stable parent directories.
struct Roots {
    std::filesystem::path writable;
    std::filesystem::path seed;
};

inline Roots resolve_roots(const std::filesystem::path& local, const std::filesystem::path& seed) {
    namespace fs = std::filesystem;
    if (seed.empty() || local.empty()) return {local, {}};
    std::error_code ec;
    if (!seed.is_absolute() || !fs::is_directory(seed, ec) || ec) return {};
    const auto source = fs::canonical(seed, ec);
    if (ec) return {};
    const auto abs_local = fs::absolute(local, ec);
    if (ec) return {};
    const auto dest = fs::weakly_canonical(abs_local, ec); // The writable leaf need not exist yet.
    if (ec) return {};
    const bool dest_exists = fs::exists(dest, ec);
    if (ec || (dest_exists && !fs::is_directory(dest, ec))) return {};
    // Use directory identity, not string prefixes: NTFS aliases and case differences still identify one directory.
    auto overlaps = [&](const fs::path& existing, fs::path path) {
        for (;;) {
            if (fs::exists(path, ec) && fs::equivalent(existing, path, ec)) return true;
            if (ec) return true;
            const auto parent = path.parent_path();
            if (parent == path || parent.empty()) return false;
            path = parent;
        }
    };
    if (overlaps(source, dest) || (dest_exists && overlaps(dest, source))) return {};
    return {dest, source};
}

// The selected compatible bytes plus metadata for the *writable* leaf as the build found it.
struct EntryFile {
    std::vector<char> data;   // compatible local bytes, else compatible seed bytes; empty when neither is usable
    bool from_seed = false;   // an external hit must not refresh either the immutable seed or a stale local file
    std::uint64_t bytes = 0;  // its size on disk, whatever its contents (0: none): what a rewrite replaces
    std::filesystem::file_time_type used{};  // its modification time: its last use
};

inline EntryFile read_entry_file(const std::filesystem::path& path, const cached_pso::Identity& identity,
                                 std::size_t limit = entry_read_limit) {
    EntryFile entry;
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) return entry;
    entry.bytes = size;
    entry.used = std::filesystem::last_write_time(path, ec);
    if (ec || size < pipeline_cache_store::header_bytes || size > limit) return entry;
    std::ifstream file(path, std::ios::binary);
    if (!file) return entry;
    entry.data.resize(static_cast<std::size_t>(size));
    if (!file.read(entry.data.data(), static_cast<std::streamsize>(entry.data.size())) ||
        !pipeline_cache_store::header_matches(entry.data.data(), entry.data.size(), identity))
        entry.data.clear();
    return entry;
}

// The file's bytes, or empty when it is missing, unreadable, over `limit`, or its header does not name `identity`.
inline std::vector<char> read_entry(const std::filesystem::path& path, const cached_pso::Identity& identity,
                                    std::size_t limit = entry_read_limit) {
    return read_entry_file(path, identity, limit).data;
}

// Whether the file is there with a header naming this device and driver: only the header is read (gta6rt
// --precompile-pipelines asks this of thousands of files of a few MiB).
inline bool entry_usable(const std::filesystem::path& path, const cached_pso::Identity& identity,
                         std::size_t limit = entry_read_limit) {
    // A helper-ready file must also fit the consumer's bounded read. Check metadata, not the full payload.
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size < pipeline_cache_store::header_bytes || size > limit) return false;
    std::ifstream file(path, std::ios::binary);
    char header[pipeline_cache_store::header_bytes];
    return file && file.read(header, sizeof(header)) &&
           pipeline_cache_store::header_matches(header, sizeof(header), identity);
}

enum class ReadRole { runtime, materialize_local };

// A write always replaces the local file, not the seed. Keep its size and timestamp even if its
// data is unusable. A seed miss adds no writable bytes; the seed directory is never scanned.
inline EntryFile read_with_seed(const std::filesystem::path& local_entry, const std::filesystem::path& seed,
                                const cached_pso::Identity& identity, ReadRole role, std::size_t limit = entry_read_limit) {
    auto local = read_entry_file(local_entry, identity, limit);
    // Build hosts promise a file in the writable journal. External data must not satisfy such a request.
    if (role == ReadRole::runtime && local.data.empty() && !seed.empty()) {
        local.data = read_entry_file(seed / local_entry.filename(), identity, limit).data;
        local.from_seed = !local.data.empty();
    }
    return local;
}

// A create this long, with no feedback to say, was a compile.
inline constexpr std::uint64_t unreported_compile_ms = 1000;
// NVIDIA 630.20 answers FAIL_ON_PIPELINE_COMPILE_REQUIRED with a compiled pipeline (seconds) instead of
// VK_PIPELINE_COMPILE_REQUIRED. A pipeline from its own disk cache comes back fast and without the application-cache
// hit bit too, and is no sign of that.
inline constexpr std::uint64_t probe_compile_ms = 250;

// Whether a build writes its journal file. `seeded`: its cache was made from the file; `hit`: the creation feedback
// (1 found in that cache, 0 not, -1 not reported); `driver_ms`: the vkCreateComputePipelines call alone.
inline bool should_write(bool seeded, int hit, std::uint64_t driver_ms) {
    if (!seeded) return true;  // the pipeline is in no file yet
    if (hit >= 0) return hit == 0;
    return driver_ms >= unreported_compile_ms;
}

// Whether a pipeline the shared cache gave for FAIL_ON_PIPELINE_COMPILE_REQUIRED was compiled after all.
inline bool probe_compiled(int hit, std::uint64_t driver_ms) { return hit == 0 && driver_ms >= probe_compile_ms; }

// The journal's size after a file of `old_bytes` (0: none) is replaced by one of `new_bytes`.
inline std::uint64_t replace_bytes(std::uint64_t total, std::uint64_t old_bytes, std::uint64_t new_bytes) {
    return total - (std::min)(total, old_bytes) + new_bytes;
}

// A file's last use is refreshed when it is more than this old, not at every hit.
inline constexpr std::chrono::hours touch_after{1};

inline void touch(const std::filesystem::path& path, std::filesystem::file_time_type now) {
    std::error_code ec;
    std::filesystem::last_write_time(path, now, ec);
}

// Called only after a journal cache hit. Only the local entry that supplied the hit is eligible for LRU.
inline void touch_local_hit(const std::filesystem::path& local_entry, const EntryFile& file,
                            std::filesystem::file_time_type now) {
    if (!file.from_seed && file.bytes && now - file.used >= touch_after)
        touch(local_entry, now);
}

namespace detail {
inline bool digits(std::string_view s) {
    return !s.empty() && s.find_first_not_of("0123456789") == std::string_view::npos;
}
inline bool hex(std::string_view s) {
    return !s.empty() && s.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}
}  // namespace detail

// <kind>-<16 hex digits>.bin, as entry_path names them.
inline bool entry_name(std::string_view name) {
    constexpr std::string_view ext = ".bin";
    const auto dash = name.find('-');
    if (dash == std::string_view::npos || dash == 0 || name.size() != dash + 1 + 16 + ext.size() ||
        name.substr(name.size() - ext.size()) != ext)
        return false;
    return detail::hex(name.substr(dash + 1, 16));
}

// What write_pipeline_cache_file stages an entry in: <entry>.tmp.<digits>.<digits>.
inline bool staging_name(std::string_view name) {
    constexpr std::string_view mark = ".tmp.";
    const auto at = name.rfind(mark);
    if (at == std::string_view::npos || !entry_name(name.substr(0, at))) return false;
    const auto suffix = name.substr(at + mark.size());
    const auto dot = suffix.find('.');
    return dot != std::string_view::npos && detail::digits(suffix.substr(0, dot)) &&
           detail::digits(suffix.substr(dot + 1));
}

struct EntryInfo {
    std::filesystem::path path;
    std::uint64_t bytes = 0;
    std::filesystem::file_time_type used{};
};

// Which entries go, least recently used first, when they hold more than `budget`: as many as leave at most `target`.
inline std::vector<std::size_t> plan_eviction(const std::vector<EntryInfo>& entries, std::uint64_t budget,
                                              std::uint64_t target) {
    std::uint64_t total = 0;
    for (const EntryInfo& entry : entries) total += entry.bytes;
    std::vector<std::size_t> out;
    if (total <= budget) return out;
    std::vector<std::size_t> order(entries.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        if (entries[a].used != entries[b].used) return entries[a].used < entries[b].used;
        return entries[a].path < entries[b].path;
    });
    for (const std::size_t i : order) {
        if (total <= target) break;
        out.push_back(i);
        total -= entries[i].bytes;
    }
    return out;
}

struct TidyResult {
    std::uint64_t bytes = 0;  // what the entries left take
    std::size_t entries = 0;  // how many are left
    std::size_t evicted = 0;  // entries removed for the budget
    std::size_t swept = 0;    // staging files removed
};

// Removes staging files older than `staging_age` (a writer still holding one keeps it: Windows refuses to delete an
// open file not shared for deletion), then, past `budget`, the least recently used entries until three quarters of
// it is left. Other files in `dir` are left alone and not counted.
inline TidyResult tidy(const std::filesystem::path& dir, std::uint64_t budget,
                       std::chrono::seconds staging_age = std::chrono::minutes(10),
                       std::filesystem::file_time_type now = std::filesystem::file_time_type::clock::now()) {
    TidyResult result;
    std::vector<EntryInfo> entries;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code stat;
        if (!it->is_regular_file(stat) || stat) continue;
        const std::string name = it->path().filename().string();
        const auto used = it->last_write_time(stat);
        if (stat) continue;
        if (staging_name(name)) {
            std::error_code removed;
            if (now - used >= staging_age && std::filesystem::remove(it->path(), removed) && !removed) ++result.swept;
            continue;
        }
        if (!entry_name(name)) continue;
        const auto size = it->file_size(stat);
        if (!stat) entries.push_back({it->path(), size, used});
    }
    std::vector<bool> gone(entries.size());
    for (const std::size_t i : plan_eviction(entries, budget, budget / 4 * 3)) {
        std::error_code removed;
        if (std::filesystem::remove(entries[i].path, removed) && !removed) {
            gone[i] = true;
            ++result.evicted;
        }
    }
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (gone[i]) continue;
        result.bytes += entries[i].bytes;
        ++result.entries;
    }
    return result;
}

// What the journal's files take on disk now: the sizes of the regular files in `dir` (0 when it does not exist).
inline std::uint64_t directory_bytes(const std::filesystem::path& dir) {
    std::uint64_t total = 0;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code stat;
        if (it->is_regular_file(stat) && !stat) {
            const auto size = it->file_size(stat);
            if (!stat) total += size;
        }
    }
    return total;
}

// LEONIDA_PIPELINE_JOURNAL_BYTES: how much the journal may hold. A compute pipeline's file is 2-5 MiB on NVIDIA: the
// ~1700 a boot builds take ~4 GiB, the whole library's ~5400 (gta6rt --precompile-pipelines) about 15. A boot writes
// nothing past it; the next one's tidy makes room.
inline constexpr std::uint64_t default_budget = std::uint64_t{32} << 30;

// Publishes `data` as the file at `path`, creating its directory. The previous file stays until the rename.
inline std::error_code write_entry(const std::filesystem::path& path, const void* data, std::size_t size) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return ec;
    return write_pipeline_cache_file(path, static_cast<const char*>(data), size);
}

}  // namespace leonida::pipeline_journal
