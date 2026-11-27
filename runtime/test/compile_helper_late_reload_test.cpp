// Late compute-journal reload and `.building` fallback coordination (shims/d3d12_compile_helper.h). No Vulkan, no GPU.
#include "../shims/d3d12_compile_helper.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace helper = rt::d3d12::compile_helper;
namespace journal = leonida::pipeline_journal;
namespace store = leonida::pipeline_cache_store;

#define CHECK(condition) do {                                        \
    if (!(condition)) {                                              \
        std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
        return 1;                                                    \
    }                                                                \
} while (0)

namespace {

void put32(std::uint8_t* p, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(value >> (8 * i));
}

leonida::cached_pso::Identity test_identity() {
    leonida::cached_pso::Identity id;
    id.vendor = 0x10de;
    id.device = 0x2204;
    for (int i = 0; i < 16; ++i) id.cache_uuid[i] = static_cast<std::uint8_t>(0xb0 + i);
    return id;
}

std::vector<char> valid_blob(const leonida::cached_pso::Identity& id, std::size_t payload = 256) {
    std::vector<char> out(store::header_bytes + payload, '\x3c');
    auto* p = reinterpret_cast<std::uint8_t*>(out.data());
    put32(p, store::header_bytes);
    put32(p + 4, store::header_version_one);
    put32(p + 8, id.vendor);
    put32(p + 12, id.device);
    std::memcpy(p + 16, id.cache_uuid, sizeof(id.cache_uuid));
    return out;
}

struct FakeJournal {
    fs::path path;
    leonida::cached_pso::Identity id;
    std::vector<char> data;

    bool has_file_data() const { return !data.empty(); }
    bool reload() {
        if (!data.empty()) return true;
        data = journal::read_entry(path, id);
        return !data.empty();
    }
};

// 1. Journal published while waiting on `big_lock`: reloaded immediately, `big_lock` released, `ask_shared` false,
//    and no `.building` claimed.
int test_reload_after_big_lock_wait(const fs::path& root) {
    const auto id = test_identity();
    const auto path = journal::entry_path(root / "case_big_lock", "cs", 0x101);
    const auto blob = valid_blob(id, 512);
    FakeJournal j{path, id, {}};
    CHECK(!j.reload());

    std::mutex big_compile;
    std::atomic<bool> holder_locked{false};
    std::thread holder([&] {
        std::lock_guard guard(big_compile);
        holder_locked = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        journal::write_entry(path, blob.data(), blob.size());
    });
    while (!holder_locked.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));

    std::unique_lock<std::mutex> big_lock(big_compile, std::defer_lock);
    fs::path claimed;
    const bool hit = helper::coordinate_fallback_compile(
        path, id, /*serial_big=*/true, /*use_building=*/true, big_lock,
        [&] { return j.reload(); }, [] {}, &claimed);
    holder.join();

    CHECK(hit);
    CHECK(j.has_file_data() && j.data == blob);
    CHECK(!big_lock.owns_lock());  // fast journal hit must never retain `big_lock`
    CHECK(claimed.empty() && !helper::building_held(path));
    const bool ask_shared = !j.has_file_data();
    CHECK(!ask_shared);
    return 0;
}

// 2. Journal published during compile-gate drain before `big_lock`: reloaded without ever acquiring `big_lock`.
int test_reload_after_compile_gate_drain(const fs::path& root) {
    const auto id = test_identity();
    const auto path = journal::entry_path(root / "case_gate_drain", "cs", 0x102);
    const auto blob = valid_blob(id, 384);
    FakeJournal j{path, id, {}};

    std::mutex big_compile;
    std::unique_lock<std::mutex> preheld(big_compile);  // held throughout: `big_lock.lock()` would deadlock if called
    std::unique_lock<std::mutex> big_lock(big_compile, std::defer_lock);
    fs::path claimed;
    bool drained = false;
    const bool hit = helper::coordinate_fallback_compile(
        path, id, /*serial_big=*/true, /*use_building=*/true, big_lock,
        [&] { return j.reload(); },
        [&] {
            drained = true;
            journal::write_entry(path, blob.data(), blob.size());
        },
        &claimed);

    CHECK(drained && hit);
    CHECK(j.has_file_data() && j.data == blob);
    CHECK(!big_lock.owns_lock());
    CHECK(claimed.empty());
    return 0;
}

// 3. Helper holds `.building` after `big_lock` is acquired: `big_lock` is released while waiting for the helper's
//    active build to finish, and the published journal entry is reloaded cleanly.
int test_waits_for_active_building_without_holding_big_lock(const fs::path& root) {
    const auto id = test_identity();
    const auto path = journal::entry_path(root / "case_active_building", "cs", 0x103);
    const auto blob = valid_blob(id, 640);
    fs::create_directories(helper::requests_dir(path.parent_path()));
    FakeJournal j{path, id, {}};

    std::mutex big_compile;
    std::atomic<bool> gate_entered{false}, building_locked{false}, big_lock_was_free{false};

    std::thread server;
    std::unique_lock<std::mutex> big_lock(big_compile, std::defer_lock);
    fs::path claimed;
    const bool hit = helper::coordinate_fallback_compile(
        path, id, /*serial_big=*/true, /*use_building=*/true, big_lock,
        [&] { return j.reload(); },
        [&] {
            // Simulate the helper grabbing `.building` right after `drain_gate` / during `big_lock` acquisition.
            server = std::thread([&] {
                const HANDLE lock = CreateFileW(helper::building_path(path).c_str(), GENERIC_WRITE, 0, nullptr,
                                                CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
                if (lock == INVALID_HANDLE_VALUE) return;
                building_locked = true;
                // While the caller waits on `lock` (`.building`), `big_compile` must NOT remain locked!
                for (int i = 0; i < 40 && !big_lock_was_free.load(); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    std::unique_lock<std::mutex> probe(big_compile, std::try_to_lock);
                    if (probe.owns_lock()) big_lock_was_free = true;
                }
                journal::write_entry(path, blob.data(), blob.size());
                CloseHandle(lock);
            });
            while (!building_locked.load()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            gate_entered = true;
        },
        &claimed);
    if (server.joinable()) server.join();

    CHECK(gate_entered.load() && big_lock_was_free.load());
    CHECK(hit && j.has_file_data() && j.data == blob);
    CHECK(!big_lock.owns_lock());
    CHECK(claimed.empty() && !helper::building_held(path));
    return 0;
}

// 4. Helper holds `.building` and exits without publishing a usable file: caller recovers, re-acquires `big_lock`,
//    claims `.building` locally, and removes the stale `.req`.
int test_active_building_failure_recovers_and_claims_local(const fs::path& root) {
    const auto id = test_identity();
    const auto path = journal::entry_path(root / "case_dead_helper", "cs", 0x104);
    CHECK(!helper::write_line(helper::request_path(path), helper::format_request("synthetic/cs_dead", 0)));
    CHECK(fs::exists(helper::request_path(path)));
    FakeJournal j{path, id, {}};

    std::atomic<bool> building_locked{false};
    std::thread dead_server([&] {
        const HANDLE lock = CreateFileW(helper::building_path(path).c_str(), GENERIC_WRITE, 0, nullptr,
                                        CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (lock == INVALID_HANDLE_VALUE) return;
        building_locked = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        CloseHandle(lock);  // no journal file written
    });
    while (!building_locked.load()) std::this_thread::sleep_for(std::chrono::milliseconds(2));

    std::mutex big_compile;
    std::unique_lock<std::mutex> big_lock(big_compile, std::defer_lock);
    fs::path claimed;
    const bool hit = helper::coordinate_fallback_compile(
        path, id, /*serial_big=*/true, /*use_building=*/true, big_lock,
        [&] { return j.reload(); }, [] {}, &claimed);
    dead_server.join();

    CHECK(!hit && !j.has_file_data());
    CHECK(big_lock.owns_lock());  // serialized in-process compile keeps `big_lock`
    CHECK(claimed == path && helper::building_held(path));
    CHECK(!fs::exists(helper::request_path(path)));  // `.req` unlinked so helper will not duplicate it
    helper::unmark_building(claimed);
    CHECK(!helper::building_held(path));
    return 0;
}

// 5. Close-the-window check: if the journal file becomes usable right as `await_late_journal_or_claim_local` claims
//    `.building`, `.building` is immediately released and `true` is returned.
int test_publish_race_drops_claimed_building(const fs::path& root) {
    const auto id = test_identity();
    const auto path = journal::entry_path(root / "case_race", "cs", 0x105);
    const auto blob = valid_blob(id, 128);
    CHECK(!journal::write_entry(path, blob.data(), blob.size()));
    fs::path claimed;
    CHECK(helper::await_late_journal_or_claim_local(path, id, &claimed));
    CHECK(claimed.empty() && !helper::building_held(path));
    return 0;
}

}  // namespace

int main() {
    const auto dir = fs::temp_directory_path() /
        ("leonida_helper_late_reload_test_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    fs::create_directories(dir, ec);
    int failed = test_reload_after_big_lock_wait(dir);
    if (!failed) failed = test_reload_after_compile_gate_drain(dir);
    if (!failed) failed = test_waits_for_active_building_without_holding_big_lock(dir);
    if (!failed) failed = test_active_building_failure_recovers_and_claims_local(dir);
    if (!failed) failed = test_publish_race_drops_claimed_building(dir);
    fs::remove_all(dir, ec);
    if (failed) return failed;
    std::printf("compile_helper_late_reload_test: ok\n");
    return 0;
}
