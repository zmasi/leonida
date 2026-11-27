// Out-of-process compute pipeline compiles: the compile helper (gta6rt --compile-server).
//
// A first build of a large compute module keeps the driver busy for tens of seconds to minutes, and on NVIDIA 630.20 a
// vkQueueSubmit of the same process waits for the lock that build holds (the compile gate makes it wait in our code
// instead, which still stops the title's frames). The driver's locks are per process. So with LEONIDA_COMPILE_HELPER=1
// a build whose pipeline is in no journal file asks a helper process to make that file, and creates the pipeline from
// it once it is there: a cache hit of a few milliseconds. With asynchronous builds (LEONIDA_ASYNC_PIPELINES=1, the
// intended mode) nothing waits: the dispatch is skipped until the file is there. Without them a warm-up still only
// asks (so the helper's threads work ahead), and a dispatch waits for the file, holding nothing of the driver's.
//
// The helper is the same executable with the same environment, so it creates the same device and computes the same
// journal name; the file it writes is the one the game then reads. The protocol is files beside the journal:
//
//   <journal>/requests/<stem>.req       asked: "<library entry name>\t<priority>" (0: a dispatch waits for it; a
//                                       warm-up asks with its module size), written whole
//   <journal>/requests/<stem>.failed    the helper could not: why, one line
//   <journal>/requests/<stem>.building  a lock file a process holds for the whole build (ownership is the open handle,
//                                       which the OS closes when the process ends, killed or not; its contents name
//                                       the process): precompile and helper runs skip an entry its holder is building
//   <journal>/<stem>.bin                done: the journal file itself
//
// The game starts the helper at its device's first pipeline (`--compile-server --parent <pid>`), and the helper ends
// with the game. With nothing asked it fills the rest of the library at idle priority. A request whose
// file appears another way (a precompile run) is answered the same way, so nothing has to agree on who builds what.
#pragma once

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "../src/cached_pso.h"
#include "../src/pipeline_cache_journal.h"

namespace rt::d3d12::compile_helper {

inline std::filesystem::path requests_dir(const std::filesystem::path& journal_dir) { return journal_dir / "requests"; }

inline std::filesystem::path sidecar(const std::filesystem::path& journal_file, const char* extension) {
    return requests_dir(journal_file.parent_path()) / (journal_file.stem().string() + extension);
}
inline std::filesystem::path request_path(const std::filesystem::path& journal_file) {
    return sidecar(journal_file, ".req");
}
inline std::filesystem::path failed_path(const std::filesystem::path& journal_file) {
    return sidecar(journal_file, ".failed");
}
inline std::filesystem::path building_path(const std::filesystem::path& journal_file) {
    return sidecar(journal_file, ".building");
}

// Whether another process (or thread) holds the entry's lock file: a build of it is running now. An open that asks to
// read conflicts with the holder's share mode 0; a leftover no one holds opens and is closed again at once.
inline bool building_held(const std::filesystem::path& journal_file) {
    const std::filesystem::path marker = building_path(journal_file);
    const HANDLE file = CreateFileW(marker.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
        return false;
    }
    return GetLastError() == ERROR_SHARING_VIOLATION;
}

// Share delete access so a directory scan reading `<stem>.req` never blocks a worker unlinking or replacing it.
inline std::string read_line(const std::filesystem::path& path) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    char buf[4096];
    DWORD read = 0;
    const BOOL ok = ReadFile(file, buf, sizeof(buf), &read, nullptr);
    CloseHandle(file);
    if (!ok || !read) return {};
    std::size_t end = 0;
    while (end < read && buf[end] != '\r' && buf[end] != '\n') ++end;
    return std::string(buf, end);
}

inline std::error_code write_line(const std::filesystem::path& path, const std::string& text) {
    const std::string line = text + "\n";
    return leonida::pipeline_journal::write_entry(path, line.data(), line.size());
}

// A request's line: "<name>\t<priority>"; a bare name (an older writer) is priority 0.
struct RequestLine {
    std::string name;
    std::uint64_t priority = 0;
};
inline RequestLine parse_request(const std::string& line) {
    RequestLine out;
    const auto tab = line.rfind('\t');
    if (tab == std::string::npos) {
        out.name = line;
        return out;
    }
    out.name = line.substr(0, tab);
    out.priority = std::strtoull(line.c_str() + tab + 1, nullptr, 10);
    return out;
}
inline std::string format_request(const std::string& name, std::uint64_t priority) {
    return name + "\t" + std::to_string(priority);
}

// This process builds for others (--compile-server, --precompile-pipelines): it never asks a helper or starts one,
// and its device keeps no session cache.
inline std::atomic<bool> build_host{false};

// LEONIDA_COMPILE_HELPER=1: modules of at least LEONIDA_COMPILE_HELPER_MIN_BYTES of SPIR-V (default 128 KiB).
inline std::size_t min_bytes() {
    static const std::size_t bytes = [] {
        char text[32]{};
        const DWORD n = GetEnvironmentVariableA("LEONIDA_COMPILE_HELPER", text, sizeof(text));
        if (!(n == 1 && text[0] == '1')) return SIZE_MAX;
        const DWORD m = GetEnvironmentVariableA("LEONIDA_COMPILE_HELPER_MIN_BYTES", text, sizeof(text));
        const unsigned long long v = (m > 0 && m < sizeof(text)) ? std::strtoull(text, nullptr, 10) : 0;
        return v ? static_cast<std::size_t>(v) : std::size_t{128} << 10;
    }();
    return bytes;
}
inline bool enabled() { return min_bytes() != SIZE_MAX && !build_host.load(); }
inline bool wanted(std::size_t spirv_bytes) { return enabled() && spirv_bytes >= min_bytes(); }

// LOCAL FIX (compile helper: a killed helper's marker outlived a reused process id): the marker is a lock file, not a
// note naming its owner. It is created with no sharing and DELETE_ON_CLOSE, and the handle is held for the whole build:
// the OS closes it when the process ends, even on TerminateProcess, so a marker no process holds is one to take over
// and a leftover can never block a later run that happens to be given the same process id. The stems this process
// holds are kept here instead, which is what "another thread of this process has it" means.
inline std::mutex& markers_mutex() {
    static std::mutex mutex;
    return mutex;
}
inline std::map<std::string, HANDLE>& held_markers() {  // stem -> the handle that keeps its lock file
    static std::map<std::string, HANDLE> handles;
    return handles;
}

// Claims the entry for this process's build: false when this process or another one holds its lock file.
inline bool mark_building(const std::filesystem::path& journal_file) {
    const auto marker = building_path(journal_file);
    const std::string stem = journal_file.stem().string();
    std::error_code ec;
    std::filesystem::create_directories(marker.parent_path(), ec);
    {
        std::lock_guard lock(markers_mutex());
        if (held_markers().count(stem)) return false;  // another thread of this process is building it
    }
    // CREATE_NEW cannot say whether an existing file is held, so a create that finds it is followed by an open with
    // no sharing: that succeeds on a leftover and fails with ERROR_SHARING_VIOLATION on a live build.
    for (int attempt = 0; attempt < 2; ++attempt) {
        HANDLE file = CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            if (GetLastError() != ERROR_FILE_EXISTS) return true;  // no marker can be kept: build anyway
            file = CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
            if (file == INVALID_HANDLE_VALUE) {
                if (GetLastError() == ERROR_SHARING_VIOLATION) return false;  // a live build has it
                continue;  // it went away between the two calls: try the create once more
            }
        }
        // Its contents are for a human reading the folder; nothing here reads them.
        const std::string pid = std::to_string(GetCurrentProcessId()) + "\n";
        DWORD written = 0;
        WriteFile(file, pid.data(), DWORD(pid.size()), &written, nullptr);
        std::lock_guard lock(markers_mutex());
        held_markers()[stem] = file;
        return true;
    }
    return true;  // a marker that cannot be opened at all: build anyway
}

// Drops the claim: closing the handle removes the lock file (DELETE_ON_CLOSE).
inline void unmark_building(const std::filesystem::path& journal_file) {
    HANDLE handle = INVALID_HANDLE_VALUE;
    {
        std::lock_guard lock(markers_mutex());
        auto& handles = held_markers();
        const auto it = handles.find(journal_file.stem().string());
        if (it == handles.end()) return;
        handle = it->second;
        handles.erase(it);
    }
    CloseHandle(handle);
}

// The game's side: one helper per process, started at the first request.
class Client {
public:
    enum class State { ready, pending, failed, gone };

    static Client& instance() {
        static auto* client = new Client;  // never destroyed: builds may still be asking at exit
        return *client;
    }

    // Asks for `journal_file` (the entry `name`) at `priority` without waiting; true when asked (or already there).
    // A request is written again only for a more urgent priority.
    bool request(const std::filesystem::path& journal_file, const std::string& name, std::uint64_t priority,
                 const leonida::cached_pso::Identity& identity, std::string* why) {
        if (leonida::pipeline_journal::entry_usable(journal_file, identity)) return true;
        if (!start(why)) return false;
        const std::string stem = journal_file.stem().string();
        bool fresh = false;
        {
            std::lock_guard lock(mutex_);
            const auto it = asked_.find(stem);
            if (it != asked_.end() && it->second <= priority) return true;
            fresh = it == asked_.end();
            asked_[stem] = priority;
        }
        // LOCAL FIX (compile helper: a failure of a request this session already wrote): only a fresh request drops an
        // older failure, so asking again can retry. Bumping the priority of a request keeps it, since .failed is the
        // answer to that same request and dropping it would put the caller back to waiting for a file nobody builds.
        if (fresh) {
            std::error_code ec;
            std::filesystem::remove(failed_path(journal_file), ec);  // an older failure: ask again
        }
        if (const std::error_code error = write_line(request_path(journal_file), format_request(name, priority))) {
            if (why) *why = "could not write the request: " + error.message();
            std::lock_guard lock(mutex_);
            asked_.erase(stem);
            return false;
        }
        return true;
    }

    // Where a request stands: its file is there, the helper gave up (*why), the helper is gone, or still pending.
    State state(const std::filesystem::path& journal_file, const leonida::cached_pso::Identity& identity,
                std::string* why) {
        if (leonida::pipeline_journal::entry_usable(journal_file, identity)) return State::ready;
        std::error_code ec;
        const auto failed = failed_path(journal_file);
        if (std::filesystem::exists(failed, ec)) {
            if (why) *why = "the helper could not build it: " + read_line(failed);
            forget(journal_file);
            return State::failed;
        }
        const std::string stem = journal_file.stem().string();
        std::lock_guard lock(mutex_);
        if (!process_ || WaitForSingleObject(process_, 0) == WAIT_OBJECT_0) {
            drop(stem);
            if (why) *why = "the helper exited";
            return State::gone;
        }
        // The helper takes .req away when it starts a build and holds the .building lock file until the journal file
        // is written: an entry being built is pending however long the build takes (the title's big programs take
        // minutes), and that time does not count against the pending bound below.
        const auto now = std::chrono::steady_clock::now();
        if (building_held(journal_file)) {
            pending_since_[stem] = now;
            return State::pending;
        }
        // LOCAL FIX (compile helper: nothing bounded pending): the helper removes .req before it writes .failed and
        // before the build that writes the journal file, so a request of ours that is neither listed nor being built
        // has been answered another way or lost. Waiting would last as long as the helper lives, so answer failed here
        // and the game builds the pipeline itself. (The journal file is checked once more: the build may have ended
        // between the first check and the lock file's.)
        if (!std::filesystem::exists(request_path(journal_file), ec)) {
            if (leonida::pipeline_journal::entry_usable(journal_file, identity)) {
                drop(stem);
                return State::ready;
            }
            drop(stem);
            if (why) *why = "its request is gone and its journal file is not usable";
            return State::failed;
        }
        // The helper is alive but may never build it (a module it cannot, a request it lost): the same bound as a
        // failed request, so a skipped dispatch becomes a build here rather than a title that never dispatches.
        const auto first = pending_since_.emplace(stem, now).first;
        const unsigned long long limit = pending_ms();
        if (limit && now - first->second >= std::chrono::milliseconds(limit)) {
            drop(stem);
            if (why) *why = "the helper did not build it within " + std::to_string(limit) + " ms";
            return State::failed;
        }
        return State::pending;
    }

    // Starts the helper now, so its device is up before the title's load needs it: made while the title fills video
    // memory, the helper's device took 110 s in one boot. Called at the device's first pipeline.
    void prestart() {
        std::string why;
        if (!start(&why)) std::fprintf(stderr, "[rt] d3d12: compile helper: %s\n", why.c_str());
    }

    // Asks and waits until the file is usable, the helper gives up or is gone, or LEONIDA_COMPILE_HELPER_WAIT_MS
    // passes (default 120 s; 0: no limit). True when the file is there.
    bool build(const std::filesystem::path& journal_file, const std::string& name,
               const leonida::cached_pso::Identity& identity, std::string* why) {
        if (!request(journal_file, name, 0, identity, why)) return false;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms());
        for (;;) {
            switch (state(journal_file, identity, why)) {
            case State::ready: return true;
            case State::failed:
            case State::gone: return false;
            case State::pending: break;
            }
            // A build in progress is waited for to its end: giving up would only start the same build here.
            if (building_held(journal_file))
                deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms());
            if (wait_ms() && std::chrono::steady_clock::now() >= deadline) {
                if (why) *why = "timed out waiting for the helper";
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    // The helper is running for a test: state() and request() then answer for it without starting one (the test's own
    // process handle stays alive, so the helper reads as running).
    void pretend_helper_running_for_test() {
        std::lock_guard lock(mutex_);
        process_ = GetCurrentProcess();
        tried_ = true;
    }

private:
    // Under mutex_ (or before it is taken): this process's bookkeeping for a request that is answered.
    void drop(const std::string& stem) {
        asked_.erase(stem);
        pending_since_.erase(stem);
    }

    void forget(const std::filesystem::path& journal_file) {
        std::lock_guard lock(mutex_);
        drop(journal_file.stem().string());
    }

    // How long a dispatch waits for the helper's file: LEONIDA_COMPILE_HELPER_WAIT_MS, default 120 s so a helper that
    // cannot build a module never holds a dispatch for the whole boot; 0 keeps the old no-limit wait.
    static unsigned long long wait_ms() {
        static const unsigned long long ms = [] {
            char text[32]{};
            const DWORD n = GetEnvironmentVariableA("LEONIDA_COMPILE_HELPER_WAIT_MS", text, sizeof(text));
            if (n == 0 || n >= sizeof(text)) return 120000ull;
            return std::strtoull(text, nullptr, 10);
        }();
        return ms;
    }

    // How long a pending request may stay pending with the helper alive (LEONIDA_COMPILE_HELPER_PENDING_MS, default
    // 120 s; 0: no limit). Read on each call so a test can set a short one.
    static unsigned long long pending_ms() {
        char text[32]{};
        const DWORD n = GetEnvironmentVariableA("LEONIDA_COMPILE_HELPER_PENDING_MS", text, sizeof(text));
        if (n == 0 || n >= sizeof(text)) return 120000ull;
        return std::strtoull(text, nullptr, 10);
    }

    // The helper's process handle, started once; null (and *why) when it could not be or has exited.
    HANDLE start(std::string* why) {
        std::lock_guard lock(mutex_);
        if (process_ && WaitForSingleObject(process_, 0) != WAIT_OBJECT_0) return process_;
        if (tried_) {
            if (why) *why = process_ ? "the helper exited" : "the helper could not be started";
            return nullptr;
        }
        tried_ = true;
        std::wstring exe(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, exe.data(), DWORD(exe.size()));
        if (!length || length >= exe.size()) {
            if (why) *why = "no executable path";
            return nullptr;
        }
        exe.resize(length);
        // LEONIDA_COMPILE_HELPER_THREADS (default 2): requests built at once, ~3.5 GB of host memory each for a module
        // over 3 MB. LEONIDA_COMPILE_HELPER_IDLE_THREADS (default 1, at most THREADS - 1 so one is always free for a
        // request): of those, how many fill the rest of the library at idle priority when nothing is asked.
        const auto number = [](const char* variable, unsigned fallback) {
            char text[16]{};
            const DWORD n = GetEnvironmentVariableA(variable, text, sizeof(text));
            return (n > 0 && n < sizeof(text)) ? unsigned(std::strtoul(text, nullptr, 10)) : fallback;
        };
        const unsigned threads = (std::max)(1u, number("LEONIDA_COMPILE_HELPER_THREADS", 2));
        const unsigned idle = number("LEONIDA_COMPILE_HELPER_IDLE_THREADS", 1);
        std::wstring command = L"\"" + exe + L"\" --compile-server --parent " + std::to_wstring(GetCurrentProcessId()) +
                               L" --threads " + std::to_wstring(threads) + L" --idle-threads " + std::to_wstring(idle);
        STARTUPINFOW startup{sizeof(startup)};
        startup.dwFlags = STARTF_USESTDHANDLES;  // its log lines go where the game's do
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = GetStdHandle(STD_ERROR_HANDLE);
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION info{};
        // Normal priority: its requests are what the title's skipped dispatches wait for (at below normal a busy title
        // starved its start for minutes). Only its library filling runs at idle priority.
        if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                            &startup, &info)) {
            if (why) *why = "CreateProcess failed (" + std::to_string(GetLastError()) + ")";
            return nullptr;
        }
        CloseHandle(info.hThread);
        process_ = info.hProcess;
        std::fprintf(stderr, "[rt] d3d12: compile helper started (process %lu, %u thread(s), %u filling the library "
                             "when idle)\n", info.dwProcessId, threads, idle);
        return process_;
    }

    std::mutex mutex_;
    HANDLE process_ = nullptr;
    bool tried_ = false;
    std::map<std::string, std::uint64_t> asked_;  // stem -> priority of the request written
    std::map<std::string, std::chrono::steady_clock::time_point> pending_since_;  // stem -> first seen pending
};

// One pending request, as the helper lists them.
struct Request {
    std::string stem;  // the journal file's stem: cs-<16 hex digits>
    std::string name;  // the library entry
    std::uint64_t priority = 0;
    std::filesystem::file_time_type asked{};
};

// The requests in `dir`, most urgent first (priority, then age).
inline std::vector<Request> list_requests(const std::filesystem::path& dir) {
    std::vector<Request> out;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != ".req") continue;
        std::error_code stat;
        const auto asked = it->last_write_time(stat);
        if (stat) continue;
        RequestLine line = parse_request(read_line(it->path()));
        if (line.name.empty()) continue;
        out.push_back({it->path().stem().string(), std::move(line.name), line.priority, asked});
    }
    std::sort(out.begin(), out.end(), [](const Request& a, const Request& b) {
        // The priority first, then the oldest: a stale request left by a killed session (a warm-up, so a high
        // priority) cannot get ahead of this session's dispatches, which ask at 0 (LOCAL FIX, review).
        return a.priority != b.priority ? a.priority < b.priority : a.asked < b.asked;
    });
    return out;
}

// Tracks listed requests across directory scans so a request finished and unlinked by a worker is neither re-claimed
// from the previous scan's `pending` list nor re-inserted by an unlocked `list_requests` scan that read `<stem>.req`
// just before it was unlinked.
class RequestQueue {
public:
    static constexpr auto kElsewhereCooldown = std::chrono::seconds(5);

    void refresh(std::vector<Request> listing,
                 std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
        for (auto it = elsewhere_.begin(); it != elsewhere_.end();) {
            if (now - it->second >= kElsewhereCooldown)
                it = elsewhere_.erase(it);
            else
                ++it;
        }
        std::map<std::string, Stamp> still_listed;
        pending_.clear();
        pending_.reserve(listing.size());
        for (Request& req : listing) {
            const auto ans = answered_.find(req.stem);
            if (ans != answered_.end() && ans->second.priority == req.priority && ans->second.asked == req.asked) {
                still_listed.emplace(req.stem, ans->second);
                continue;
            }
            pending_.push_back(std::move(req));
        }
        answered_ = std::move(still_listed);
    }

    bool take(Request& out, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
        for (const Request& request : pending_) {
            if (busy_.count(request.stem)) continue;
            if (const auto it = elsewhere_.find(request.stem);
                it != elsewhere_.end() && now - it->second < kElsewhereCooldown)
                continue;
            busy_.insert(request.stem);
            out = request;
            return true;
        }
        return false;
    }

    void finish(const Request& request, bool leave,
                std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
        busy_.erase(request.stem);
        if (leave) {
            elsewhere_[request.stem] = now;
            return;
        }
        elsewhere_.erase(request.stem);
        answered_[request.stem] = {request.priority, request.asked};
        std::erase_if(pending_, [&](const Request& queued) { return queued.stem == request.stem; });
    }

    bool active() const { return !pending_.empty() || !busy_.empty(); }
    std::size_t pending_count() const { return pending_.size(); }
    std::size_t answered_count() const { return answered_.size(); }

private:
    struct Stamp {
        std::uint64_t priority = 0;
        std::filesystem::file_time_type asked{};
    };
    std::vector<Request> pending_;
    std::set<std::string> busy_;
    std::map<std::string, std::chrono::steady_clock::time_point> elsewhere_;
    std::map<std::string, Stamp> answered_;
};

struct IdlePriorityScope {
    HANDLE thread = GetCurrentThread();
    int saved = GetThreadPriority(thread);
    IdlePriorityScope() {
        if (saved != THREAD_PRIORITY_ERROR_RETURN) SetThreadPriority(thread, THREAD_PRIORITY_IDLE);
    }
    ~IdlePriorityScope() {
        if (saved != THREAD_PRIORITY_ERROR_RETURN) SetThreadPriority(thread, saved);
    }
    IdlePriorityScope(const IdlePriorityScope&) = delete;
    IdlePriorityScope& operator=(const IdlePriorityScope&) = delete;
};

// Starts `threads` request workers at normal priority, the last `idle_threads` (clamped to `threads - 1`) of which
// also step `fill_one()` at THREAD_PRIORITY_IDLE whenever no takeable request is queued.
template <class ServeOne, class FillOne>
std::vector<std::thread> start_server_workers(unsigned threads, unsigned idle_threads, std::mutex& mutex,
                                              std::condition_variable& wake, RequestQueue& queue, const bool& stop,
                                              ServeOne serve_one, FillOne fill_one) {
    threads = (std::max)(1u, threads);
    idle_threads = (std::min)(idle_threads, threads - 1);
    const unsigned dedicated = threads - idle_threads;
    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (unsigned t = 0; t < threads; ++t) {
        const bool can_fill = t >= dedicated;
        pool.emplace_back([&, can_fill, serve_one, fill_one]() mutable {
            bool fill_done = !can_fill;
            for (;;) {
                Request request;
                bool has_request = false;
                {
                    std::unique_lock lock(mutex);
                    if (fill_done) {
                        wake.wait_for(lock, std::chrono::milliseconds(100),
                                      [&] { return stop || queue.take(request); });
                        if (stop) return;
                        has_request = !request.stem.empty();
                    } else {
                        if (stop) return;
                        has_request = queue.take(request);
                    }
                }
                if (has_request) {
                    serve_one(request);
                    continue;
                }
                IdlePriorityScope idle;
                if (!fill_one()) fill_done = true;
            }
        });
    }
    return pool;
}

}  // namespace rt::d3d12::compile_helper
