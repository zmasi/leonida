// RT-PIPELINE-PREWARM-ORDER, phase 1: a passive record of which pipelines the title asks for first. Off unless
// LEONIDA_PIPELINE_ORDER_LOG=1; it changes nothing the title or the replay does.
//
// For each pipeline state and variant (the unbound-attachment mask of a graphics pipeline, 0 for compute) that a
// dispatch or draw names for the first time in a run, one line is appended to pipeline_order.log beside
// pipeline_cache.session.bin: the description key of the PSO, the variant, when the PSO was created and when it was
// first used (milliseconds since the process started) and how long its build took. At exit one line of counts says how
// many were asked for, how many were ready already, and the median lead between creation and first use.
//
// The hot path (claim, publish, note_build) takes no lock, makes no system call and allocates nothing: a fixed table
// of 64-bit identities decides "first time" with one compare-and-swap and a preallocated slot takes the record. A
// thread of its own appends published slots to the file every few hundred milliseconds, so a process that is killed
// loses at most the last interval. The file is text so that it can be read with any tool; each line ends with a
// checksum, a truncated or damaged line is skipped when read back, never fatal.
//
//   # leonida pipeline order 1        first line of a new file
//   S 1 <vendor hex> <device hex>     a run starts here
//   P <c|g|m> <key> <variant hex> <created ms> <first ms> <build ms or -> <ready 0|1> *<checksum>
//   B <key> <variant hex> <build ms> *<checksum>   the build of a P line whose time was not known yet
//   E <key> <variant hex> <name hash hex> *<checksum>   which library entry a compute P line is (right after it)
//
// c = compute, g = graphics, m = mesh. The checksum is FNV-1a over the line before " *". No game data is in the file:
// the key is a SHA-256 of the title's own description of the pipeline (cached_pso.h), the rest are numbers. The name
// hash of an E line is 16 hex digits of FNV-1a 64 over the name of the library entry the pipeline was made from (the
// name itself is not written). A reader from before E lines skips them (counted as lines not read);
// parse() below reads them.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#ifdef _WIN32
#include <share.h>
#endif
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace leonida::pipeline_order {

inline constexpr std::uint32_t unknown_ms = 0xffffffffu;  // a build time nobody measured
inline constexpr std::size_t key_chars = 64;               // a SHA-256 in hex; longer keys are cut
inline constexpr std::size_t default_capacity = 8192;      // records per run
inline constexpr std::size_t max_capacity = 16000;         // the table packs the slot into 14 bits
inline constexpr std::uintmax_t default_max_file_bytes = 8u << 20;  // a larger file is set aside as .prev at open

// LEONIDA_PIPELINE_ORDER_LOG: "1" turns the record on. Unset, empty, "0" and anything else leave it off.
inline bool parse_enabled(const char* text) { return text && text[0] == '1' && text[1] == '\0'; }

struct Record {
    char kind = 'c';  // c compute, g graphics, m mesh
    std::string key;
    std::uint32_t variant = 0;
    std::uint32_t created_ms = 0;
    std::uint32_t first_ms = 0;
    std::uint32_t build_ms = unknown_ms;
    bool ready = false;  // its handle existed when it was first asked for
    bool operator==(const Record&) const = default;
};

// An E line: the library entry (by the hash of its name) behind a pipeline and variant.
struct EntryName {
    std::string key;
    std::uint32_t variant = 0;
    std::uint64_t name_hash = 0;
    bool operator==(const EntryName&) const = default;
};

struct Session {
    std::uint32_t vendor = 0;
    std::uint32_t device = 0;
    std::vector<Record> records;  // in the order the title asked for them
};

struct ReadResult {
    std::vector<Session> sessions;
    std::vector<EntryName> entries;  // the E lines of the whole file, in file order
    std::size_t bad_lines = 0;  // complete lines that could not be read: skipped
    bool cut_tail = false;      // the last line had no newline: a write that did not finish, skipped
};

inline std::uint32_t checksum(std::string_view text) {
    std::uint32_t h = 0x811c9dc5u;
    for (const char c : text) h = (h ^ std::uint8_t(c)) * 0x01000193u;
    return h;
}

inline std::uint64_t key_hash(std::string_view key) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (const char c : key) h = (h ^ std::uint8_t(c)) * 0x100000001b3ull;
    return h ? h : 1;
}

// The hash an E line carries for the name of a library entry.
inline std::uint64_t entry_name_hash(std::string_view name) { return key_hash(name); }

namespace detail {

inline std::uint64_t mix(std::uint64_t key, std::uint32_t variant) {
    std::uint64_t z = key + 0x9e3779b97f4a7c15ull * (std::uint64_t(variant) + 1);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

inline void append_line(std::string& out, std::string body) {
    char tail[16];
    std::snprintf(tail, sizeof(tail), " *%08x\n", checksum(body));
    out += body;
    out += tail;
}

inline std::string number_or_dash(std::uint32_t v) { return v == unknown_ms ? "-" : std::to_string(v); }

inline bool parse_u32(std::string_view s, int base, std::uint32_t& out) {
    if (s.empty() || s.size() > 10) return false;
    std::uint64_t v = 0;
    for (const char c : s) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else return false;
        if (d >= base) return false;
        v = v * std::uint64_t(base) + std::uint64_t(d);
        if (v > 0xffffffffull) return false;
    }
    out = std::uint32_t(v);
    return true;
}

// Exactly 16 lowercase hex digits.
inline bool parse_hash64(std::string_view s, std::uint64_t& out) {
    if (s.size() != 16) return false;
    std::uint64_t v = 0;
    for (const char c : s) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else return false;
        v = (v << 4) | std::uint64_t(d);
    }
    out = v;
    return true;
}

inline std::vector<std::string_view> split(std::string_view s) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < s.size()) {
        const std::size_t j = s.find(' ', i);
        if (j == std::string_view::npos) {
            out.push_back(s.substr(i));
            break;
        }
        out.push_back(s.substr(i, j - i));
        i = j + 1;
    }
    return out;
}

}  // namespace detail

inline std::string format_header() { return "# leonida pipeline order 1\n"; }

inline std::string format_session(std::uint32_t vendor, std::uint32_t device) {
    char text[64];
    std::snprintf(text, sizeof(text), "S 1 %x %x", vendor, device);
    std::string out;
    detail::append_line(out, text);
    return out;
}

inline std::string format_record(const Record& r) {
    std::string body = "P ";
    body += r.kind;
    body += ' ';
    body += r.key;
    char numbers[32];
    std::snprintf(numbers, sizeof(numbers), " %x ", r.variant);
    body += numbers;
    body += std::to_string(r.created_ms) + ' ' + std::to_string(r.first_ms) + ' ' + detail::number_or_dash(r.build_ms) +
            (r.ready ? " 1" : " 0");
    std::string out;
    detail::append_line(out, std::move(body));
    return out;
}

inline std::string format_build(std::string_view key, std::uint32_t variant, std::uint32_t build_ms) {
    char numbers[32];
    std::snprintf(numbers, sizeof(numbers), " %x ", variant);
    std::string body = "B ";
    body += key;
    body += numbers;
    body += std::to_string(build_ms);
    std::string out;
    detail::append_line(out, std::move(body));
    return out;
}

inline std::string format_entry(std::string_view key, std::uint32_t variant, std::uint64_t name_hash) {
    char numbers[48];
    std::snprintf(numbers, sizeof(numbers), " %x %016llx", variant, static_cast<unsigned long long>(name_hash));
    std::string body = "E ";
    body += key;
    body += numbers;
    std::string out;
    detail::append_line(out, std::move(body));
    return out;
}

// Reads what a file holds: whole sessions in file order, a record per P line (its B line merged in), every damaged line
// skipped. Never throws on any input.
inline ReadResult parse(std::string_view text) {
    ReadResult result;
    if (!text.empty() && text.back() != '\n') {
        result.cut_tail = true;
        const std::size_t last = text.rfind('\n');
        text = last == std::string_view::npos ? std::string_view{} : text.substr(0, last + 1);
    }
    const auto session = [&]() -> Session& {
        if (result.sessions.empty()) result.sessions.emplace_back();
        return result.sessions.back();
    };
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t end = text.find('\n', at);
        if (end == std::string_view::npos) break;  // not reached: the unfinished last line was cut off above
        std::string_view line = text.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty() || line[0] == '#') continue;
        const std::size_t star = line.rfind(" *");
        std::uint32_t sum = 0;
        if (star == std::string_view::npos || star + 2 + 8 != line.size() ||
            !detail::parse_u32(line.substr(star + 2), 16, sum) || sum != checksum(line.substr(0, star))) {
            ++result.bad_lines;
            continue;
        }
        const auto f = detail::split(line.substr(0, star));
        bool ok = false;
        if (f.size() == 4 && f[0] == "S") {
            Session s;
            ok = f[1] == "1" && detail::parse_u32(f[2], 16, s.vendor) && detail::parse_u32(f[3], 16, s.device);
            if (ok) result.sessions.push_back(std::move(s));
        } else if (f.size() == 8 && f[0] == "P") {
            Record r;
            ok = f[1].size() == 1 && (f[1][0] == 'c' || f[1][0] == 'g' || f[1][0] == 'm') && !f[2].empty() &&
                 f[2].size() <= key_chars && detail::parse_u32(f[3], 16, r.variant) &&
                 detail::parse_u32(f[4], 10, r.created_ms) && detail::parse_u32(f[5], 10, r.first_ms) &&
                 (f[6] == "-" || detail::parse_u32(f[6], 10, r.build_ms)) && (f[7] == "0" || f[7] == "1");
            if (ok) {
                r.kind = f[1][0];
                r.key = std::string(f[2]);
                r.ready = f[7] == "1";
                session().records.push_back(std::move(r));
            }
        } else if (f.size() == 4 && f[0] == "B") {
            std::uint32_t variant = 0, ms = 0;
            ok = !f[1].empty() && f[1].size() <= key_chars && detail::parse_u32(f[2], 16, variant) &&
                 detail::parse_u32(f[3], 10, ms);
            if (ok) {
                // The newest record of the run with that key and variant that still has no build time.
                auto& records = session().records;
                for (auto it = records.rbegin(); it != records.rend(); ++it)
                    if (it->variant == variant && it->key == f[1] && it->build_ms == unknown_ms) {
                        it->build_ms = ms;
                        break;
                    }
            }
        } else if (f.size() == 4 && f[0] == "E") {
            EntryName e;
            ok = !f[1].empty() && f[1].size() <= key_chars && detail::parse_u32(f[2], 16, e.variant) &&
                 detail::parse_hash64(f[3], e.name_hash);
            if (ok) {
                e.key = std::string(f[1]);
                result.entries.push_back(std::move(e));
            }
        }
        if (!ok) ++result.bad_lines;
    }
    return result;
}

struct Counts {
    std::size_t requested = 0;  // distinct pipelines and variants asked for, recorded
    std::size_t ready = 0;      // of those, ready when first asked for
    std::size_t overflow = 0;   // requests that found the record full and were not kept (counted per request)
    std::uint32_t median_lead_ms = 0;  // first use minus creation, of the recorded ones
};

// The recorder. Thread-safe: claim, publish and note_build from any number of threads at once, flush from one
// thread at a time (it takes its own lock).
class Recorder {
public:
    struct Config {
        std::filesystem::path path;
        std::size_t capacity = default_capacity;
        std::uint32_t vendor = 0;
        std::uint32_t device = 0;
        std::uint32_t (*now_ms)() = nullptr;  // milliseconds since the process started
        std::uintmax_t max_file_bytes = default_max_file_bytes;
    };

    // A reserved record: valid for the one thread that was told it is the first to ask for this pipeline.
    struct Claim {
        std::uint32_t index = 0;
        bool first = false;
        explicit operator bool() const { return first; }
    };

    explicit Recorder(Config config)
        : config_(std::move(config)),
          capacity_(std::min<std::size_t>(std::max<std::size_t>(config_.capacity, 1), max_capacity)),
          mask_(table_size(capacity_) - 1),
          table_(new std::atomic<std::uint64_t>[mask_ + 1]()),
          slots_(new Slot[capacity_]) {}
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;
    ~Recorder() { stop(); }

    // The first thread to name this (key hash, variant) gets a claim and must publish() it; everyone else, and every
    // request after the record is full, gets none.
    Claim claim(std::uint64_t hash, std::uint32_t variant) noexcept {
        const std::uint64_t id = detail::mix(hash, variant);
        const std::uint64_t tag = id >> slot_bits;
        std::size_t at = std::size_t(id) & mask_;
        for (std::size_t probes = 0; probes <= mask_; ++probes, at = (at + 1) & mask_) {
            std::uint64_t entry = table_[at].load(std::memory_order_acquire);
            if (entry == 0) {
                const std::uint32_t slot = next_.load(std::memory_order_relaxed);
                if (slot >= capacity_) {
                    overflow_.fetch_add(1, std::memory_order_relaxed);
                    return {};
                }
                const std::uint32_t mine = next_.fetch_add(1, std::memory_order_relaxed);
                if (mine >= capacity_) {
                    overflow_.fetch_add(1, std::memory_order_relaxed);
                    return {};
                }
                Slot& s = slots_[mine];
                s.state.store(claimed, std::memory_order_relaxed);
                const std::uint64_t packed = (tag << slot_bits) | (mine + 1);
                if (table_[at].compare_exchange_strong(entry, packed, std::memory_order_acq_rel)) {
                    s.first_ms = now();
                    s.variant = variant;
                    return {mine, true};
                }
                s.state.store(void_slot, std::memory_order_release);  // lost the race: the entry is somebody's now
            }
            if ((entry >> slot_bits) == tag) return {};
        }
        return {};
    }

    // Fills the claimed record and makes it visible to the writer. `key` is cut at key_chars. `build_ms` is what is
    // known of the build now, or unknown_ms. `name_hash` is entry_name_hash() of the library entry of a compute
    // pipeline (an E line follows its P line), 0 when there is none.
    void publish(Claim claim, std::string_view key, char kind, bool ready, std::uint32_t created_ms,
                 std::uint32_t build_ms, std::uint64_t name_hash = 0) noexcept {
        if (!claim.first) return;
        Slot& s = slots_[claim.index];
        s.name_hash = name_hash;
        s.key_len = std::uint8_t(std::min(key.size(), key_chars));
        for (std::size_t i = 0; i < s.key_len; ++i) {  // a key is one word of the line: no space, no newline
            const char c = key[i];
            s.key[i] = (c > ' ' && c < 0x7f && c != '*') ? c : '_';
        }
        s.kind = kind;
        s.ready = ready;
        s.created_ms = created_ms;
        if (build_ms != unknown_ms) {
            std::uint32_t none = unknown_ms;
            s.build_ms.compare_exchange_strong(none, build_ms, std::memory_order_relaxed);
        }
        s.state.store(published, std::memory_order_release);
    }

    // claim() and publish() in one: what the title's hook does. `ready` and `build_ms` are called, in that order, only by
    // the first request, after its claim: so a build that ends meanwhile is seen by one of them or by note_build().
    // Returns true when this call made the record.
    template <class Ready, class Build>
    bool request(std::uint64_t hash, std::string_view key, std::uint32_t variant, char kind, std::uint32_t created_ms,
                 Ready&& ready, Build&& build_ms, std::uint64_t name_hash = 0) noexcept {
        const Claim c = claim(hash, variant);
        if (!c) return false;
        const bool is_ready = ready();
        publish(c, key, kind, is_ready, created_ms, build_ms(), name_hash);
        return true;
    }

    // The build of a pipeline that was asked for finished. Ignored for one that was not (its time is passed to
    // publish() when it is) and when a time is already there.
    void note_build(std::uint64_t hash, std::uint32_t variant, std::uint32_t ms) noexcept {
        const std::uint64_t id = detail::mix(hash, variant);
        const std::uint64_t tag = id >> slot_bits;
        std::size_t at = std::size_t(id) & mask_;
        for (std::size_t probes = 0; probes <= mask_; ++probes, at = (at + 1) & mask_) {
            const std::uint64_t entry = table_[at].load(std::memory_order_acquire);
            if (entry == 0) return;
            if ((entry >> slot_bits) != tag) continue;
            std::uint32_t none = unknown_ms;
            slots_[(entry & slot_mask) - 1].build_ms.compare_exchange_strong(none, ms, std::memory_order_relaxed);
            return;
        }
    }

    // Appends what was published since the last call. Returns the lines written; -1 when the file cannot be opened
    // (the record is then off for the rest of the run).
    long flush() {
        std::lock_guard lock(io_mutex_);
        return flush_locked();
    }

    // A thread that calls flush() every `period`. Idempotent.
    void start(std::chrono::milliseconds period) {
        std::lock_guard lock(thread_mutex_);
        if (thread_.joinable()) return;
        stopping_ = false;
        thread_ = std::thread([this, period] {
            std::unique_lock lock(thread_mutex_);
            while (!stopping_) {
                thread_cv_.wait_for(lock, period, [this] { return stopping_; });
                lock.unlock();
                flush();
                lock.lock();
            }
        });
    }

    void stop() {
        {
            std::lock_guard lock(thread_mutex_);
            stopping_ = true;
        }
        thread_cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    Counts counts() const {
        Counts c;
        std::vector<std::uint32_t> leads;
        const std::uint32_t used = std::min<std::uint32_t>(next_.load(std::memory_order_acquire), std::uint32_t(capacity_));
        for (std::uint32_t i = 0; i < used; ++i) {
            const Slot& s = slots_[i];
            if (s.state.load(std::memory_order_acquire) != published) continue;
            ++c.requested;
            if (s.ready) ++c.ready;
            leads.push_back(s.first_ms > s.created_ms ? s.first_ms - s.created_ms : 0);
        }
        c.overflow = overflow_.load(std::memory_order_relaxed);
        if (!leads.empty()) {
            const std::size_t mid = (leads.size() - 1) / 2;
            std::nth_element(leads.begin(), leads.begin() + std::ptrdiff_t(mid), leads.end());
            c.median_lead_ms = leads[mid];
        }
        return c;
    }

    // Stops the thread, writes what is left and returns the one-line report; empty after the first call.
    std::string close() {
        if (closed_.exchange(true)) return {};
        stop();
        flush();
        return report(counts(), config_.path);
    }

    static std::string report(const Counts& c, const std::filesystem::path& path) {
        std::string line = "[rt] pipeline-order: requested " + std::to_string(c.requested) + ", already ready " +
                           std::to_string(c.ready) + ", median lead " + std::to_string(c.median_lead_ms) + " ms";
        if (c.overflow) line += ", not recorded (record full) " + std::to_string(c.overflow);
        line += " -> " + path.string();
        return line;
    }

    // The file could not be opened or written: nothing more is recorded this run.
    bool failed() {
        std::lock_guard lock(io_mutex_);
        return failed_;
    }
    const std::filesystem::path& path() const { return config_.path; }
    std::size_t capacity() const { return capacity_; }

private:
    static constexpr unsigned slot_bits = 14;
    static constexpr std::uint64_t slot_mask = (1u << slot_bits) - 1;
    enum : std::uint32_t { free_slot = 0, claimed = 1, published = 2, void_slot = 3 };

    struct Slot {
        std::atomic<std::uint32_t> state{free_slot};
        std::atomic<std::uint32_t> build_ms{unknown_ms};
        std::uint32_t variant = 0;
        std::uint32_t created_ms = 0;
        std::uint32_t first_ms = 0;
        std::uint64_t name_hash = 0;  // 0: no E line
        std::uint8_t key_len = 0;
        char kind = 'c';
        bool ready = false;
        char key[key_chars] = {};
    };

    static std::size_t table_size(std::size_t capacity) {
        std::size_t n = 16;
        while (n < capacity * 2) n <<= 1;
        return n;
    }

    std::uint32_t now() const noexcept { return config_.now_ms ? config_.now_ms() : 0; }

    long flush_locked() {
        if (failed_) return -1;
        std::string out;
        long lines = 0;
        // Published slots, in claim order; stop at one still being filled so that the order stays the order of asking.
        const std::uint32_t used = std::min<std::uint32_t>(next_.load(std::memory_order_acquire), std::uint32_t(capacity_));
        while (cursor_ < used) {
            Slot& s = slots_[cursor_];
            const std::uint32_t state = s.state.load(std::memory_order_acquire);
            if (state == void_slot) {
                ++cursor_;
                continue;
            }
            if (state != published) break;
            Record r;
            r.kind = s.kind;
            r.key.assign(s.key, s.key_len);
            r.variant = s.variant;
            r.created_ms = s.created_ms;
            r.first_ms = s.first_ms;
            r.build_ms = s.build_ms.load(std::memory_order_relaxed);
            r.ready = s.ready;
            out += format_record(r);
            ++lines;
            if (s.name_hash) {
                out += format_entry(r.key, r.variant, s.name_hash);
                ++lines;
            }
            if (r.build_ms == unknown_ms) waiting_.push_back(cursor_);
            ++cursor_;
        }
        // The builds that finished after their line was written.
        for (std::size_t i = 0; i < waiting_.size();) {
            Slot& s = slots_[waiting_[i]];
            const std::uint32_t ms = s.build_ms.load(std::memory_order_relaxed);
            if (ms == unknown_ms) {
                ++i;
                continue;
            }
            out += format_build(std::string_view(s.key, s.key_len), s.variant, ms);
            ++lines;
            waiting_[i] = waiting_.back();
            waiting_.pop_back();
        }
        if (out.empty()) return 0;
        if (!file_) {
            if (!open()) {
                failed_ = true;
                return -1;
            }
        }
        std::string head;
        if (!session_written_) {
            if (new_file_) head += format_header();
            head += format_session(config_.vendor, config_.device);
            session_written_ = true;
        }
        head += out;
        const bool ok = std::fwrite(head.data(), 1, head.size(), file_.get()) == head.size() && std::fflush(file_.get()) == 0;
        if (!ok) {
            failed_ = true;
            return -1;
        }
        return lines;
    }

    static bool ends_with_newline(const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in || in.tellg() <= 0) return true;
        in.seekg(-1, std::ios::end);
        char last = '\n';
        in.get(last);
        return last == '\n';
    }

    bool open() {
        std::error_code ec;
        if (config_.path.empty()) return false;
        if (config_.path.has_parent_path()) std::filesystem::create_directories(config_.path.parent_path(), ec);
        const auto size = std::filesystem::file_size(config_.path, ec);
        if (!ec && size >= config_.max_file_bytes) {
            auto prev = config_.path;
            prev += ".prev";
            std::filesystem::remove(prev, ec);
            std::filesystem::rename(config_.path, prev, ec);
        }
        ec.clear();
        const auto now_size = std::filesystem::file_size(config_.path, ec);
        new_file_ = ec || now_size == 0;
        std::FILE* f = nullptr;
#ifdef _WIN32
        f = _wfsopen(config_.path.c_str(), L"ab", _SH_DENYNO);  // others may read it while the title runs
#else
        f = std::fopen(config_.path.c_str(), "ab");
#endif
        if (!f) return false;
        file_.reset(f);
        // A process killed in the middle of a line leaves no newline: the next session's line would stick to it and
        // the reader would file this run's records under the previous one. The reader skips empty lines.
        if (!new_file_ && !ends_with_newline(config_.path) && std::fputc('\n', f) == EOF) {
            file_.reset();
            return false;
        }
        return true;
    }

    struct Closer {
        void operator()(std::FILE* f) const { if (f) std::fclose(f); }
    };

    Config config_;
    const std::size_t capacity_;
    const std::size_t mask_;
    std::unique_ptr<std::atomic<std::uint64_t>[]> table_;
    std::unique_ptr<Slot[]> slots_;
    std::atomic<std::uint32_t> next_{0};
    std::atomic<std::size_t> overflow_{0};
    std::atomic<bool> closed_{false};

    std::mutex io_mutex_;
    std::unique_ptr<std::FILE, Closer> file_;
    std::uint32_t cursor_ = 0;
    std::vector<std::uint32_t> waiting_;
    bool failed_ = false;
    bool new_file_ = false;
    bool session_written_ = false;

    std::mutex thread_mutex_;
    std::condition_variable thread_cv_;
    std::thread thread_;
    bool stopping_ = false;
};

}  // namespace leonida::pipeline_order
