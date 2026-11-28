// Portable C++20 tests for runtime/shims/d3d12_pipeline_checkpoint.h
#include "../shims/d3d12_pipeline_checkpoint.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#define CHECK(condition) do {                                        \
    if (!(condition)) {                                              \
        std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
        return 1;                                                    \
    }                                                                \
} while (0)

int main() {
    using namespace rt::d3d12;
    namespace fs = std::filesystem;

    // 1. CompileActivity RAII counter
    {
        std::atomic<unsigned> active{0};
        {
            CompileActivity a1(active);
            CHECK(active.load() == 1);
            {
                CompileActivity a2(active);
                CHECK(active.load() == 2);
            }
            CHECK(active.load() == 1);
        }
        CHECK(active.load() == 0);

        // Concurrency stress test
        std::vector<std::thread> threads;
        for (int i = 0; i < 8; ++i) {
            threads.emplace_back([&active] {
                for (int j = 0; j < 500; ++j) {
                    CompileActivity act(active);
                }
            });
        }
        for (auto& t : threads) t.join();
        CHECK(active.load() == 0);
    }

    // 2. is_direct_session_cache_enabled helper
    CHECK(is_direct_session_cache_enabled(nullptr, 0));
    CHECK(is_direct_session_cache_enabled(L"", 0));
    CHECK(is_direct_session_cache_enabled(L"1", 1));
    CHECK(!is_direct_session_cache_enabled(L"0", 1));
    CHECK(is_direct_session_cache_enabled(L"0", 2));
    CHECK(is_direct_session_cache_enabled(L"false", 5));
    CHECK(is_direct_session_cache_enabled(L"off", 3));

    // 3. cache_temp_suffix predicate
    CHECK(cache_temp_suffix(L"0.0"));
    CHECK(cache_temp_suffix(L"123.456"));
    CHECK(cache_temp_suffix(L"999999999.1"));
    CHECK(cache_temp_suffix(L"1.0"));

    CHECK(!cache_temp_suffix(L""));
    CHECK(!cache_temp_suffix(L"."));
    CHECK(!cache_temp_suffix(L".1"));
    CHECK(!cache_temp_suffix(L"1."));
    CHECK(!cache_temp_suffix(L"1.2.3"));
    CHECK(!cache_temp_suffix(L"a.b"));
    CHECK(!cache_temp_suffix(L"1.a"));
    CHECK(!cache_temp_suffix(L"a.1"));
    CHECK(!cache_temp_suffix(L"-1.0"));
    CHECK(!cache_temp_suffix(L"1.-0"));

    // 4. clean_stale_cache_files edge cases
    clean_stale_cache_files(fs::path{});
    clean_stale_cache_files(fs::path{L"non_existent_dir_12345/dummy.cache"});

    // 5. CacheStagingFile invalid arguments
    const auto test_dir = fs::temp_directory_path() / L"leonida_checkpoint_test";
    std::error_code ec;
    fs::create_directories(test_dir, ec);

    const auto target_file = test_dir / L"session.cache";

    {
        CacheStagingFile empty_dest(fs::path{}, 1024);
        CHECK(!empty_dest.is_valid());
        CHECK(empty_dest.data == nullptr);

        CacheStagingFile zero_bytes(target_file, 0);
        CHECK(!zero_bytes.is_valid());
        CHECK(zero_bytes.data == nullptr);
    }

    // 6. CacheStagingFile publish workflow
    {
        if (fs::exists(target_file)) fs::remove(target_file, ec);

        constexpr std::size_t kMapBytes = 512;
        constexpr std::size_t kWriteBytes = 128;
        CacheStagingFile staging(target_file, kMapBytes);
        CHECK(staging.is_valid());
        CHECK(staging.data != nullptr);

        auto* bytes = static_cast<std::uint8_t*>(staging.data);
        for (std::size_t i = 0; i < kWriteBytes; ++i) {
            bytes[i] = static_cast<std::uint8_t>(i ^ 0x5A);
        }

        const bool ok = staging.publish(target_file, kWriteBytes);
        CHECK(ok);
        CHECK(!staging.is_valid());
        CHECK(staging.data == nullptr);

        CHECK(fs::exists(target_file));
        CHECK(fs::file_size(target_file) == kWriteBytes);

        std::ifstream in(target_file, std::ios::binary);
        std::vector<std::uint8_t> read_bytes(kWriteBytes);
        in.read(reinterpret_cast<char*>(read_bytes.data()), kWriteBytes);
        CHECK(in.gcount() == static_cast<std::streamsize>(kWriteBytes));
        for (std::size_t i = 0; i < kWriteBytes; ++i) {
            CHECK(read_bytes[i] == static_cast<std::uint8_t>(i ^ 0x5A));
        }
    }

    // 7. CacheStagingFile abort cleanup
    {
        fs::path orphaned_temp;
        {
            CacheStagingFile aborted(target_file, 256);
            CHECK(aborted.is_valid());
            orphaned_temp = aborted.temp_path();
            CHECK(fs::exists(orphaned_temp));
        }
        // Destructor must delete the unmapped staging file
        CHECK(!fs::exists(orphaned_temp));
    }

    // Clean up test directory
    fs::remove_all(test_dir, ec);

    std::printf("d3d12_pipeline_checkpoint: activity counter, cache predicates, staging and publish tests passed\n");
    return 0;
}
