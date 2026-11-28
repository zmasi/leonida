// Exercise checkpoint replacement and real filesystem errors without a Vulkan device.
#include "../src/pipeline_cache_file.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include "../shims/d3d12_pipeline_checkpoint.h"
#else
#include <csignal>
#include <sys/resource.h>
#endif

namespace fs = std::filesystem;

#define CHECK(condition) do {                                        \
    if (!(condition)) {                                              \
        std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
        return 1;                                                    \
    }                                                                \
} while (0)

static std::string contents(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}

static int test_checkpoint(const fs::path& dir) {
    const auto cache = dir / "cache.bin";
    const auto partial = dir / "cache.bin.partial";
    std::ofstream(partial, std::ios::binary) << "unrelated";
    const std::string saved("new\0data\r\n", 10);
    CHECK(!write_pipeline_cache_file(cache, "older checkpoint", 16));
    CHECK(contents(cache) == "older checkpoint");
    CHECK(!write_pipeline_cache_file(cache, saved.data(), saved.size()));
    CHECK(contents(cache) == saved);

    const auto directory = dir / "empty";
    CHECK(fs::create_directory(directory));
    CHECK(write_pipeline_cache_file(directory, "next", 4));
    CHECK(fs::is_directory(directory));

    // Guard against empty path and null buffer with positive size
    CHECK(write_pipeline_cache_file("", "data", 4) == std::make_error_code(std::errc::invalid_argument));
    CHECK(write_pipeline_cache_file(cache, nullptr, 16) == std::make_error_code(std::errc::invalid_argument));
    // Zero size with null pointer safely succeeds and writes empty file
    const auto empty_file = dir / "empty.bin";
    CHECK(!write_pipeline_cache_file(empty_file, nullptr, 0));
    CHECK(contents(empty_file).empty());
    fs::remove(empty_file);

#ifdef _WIN32
    {
        using namespace rt::d3d12;
        CHECK(cache_temp_suffix(L"123.0") && !cache_temp_suffix(L".2") && !cache_temp_suffix(L"1.2.3"));
        CacheStagingFile next(cache, 128);
        CHECK(next.data);
        std::memcpy(next.data, "short", 5);
        CHECK(next.publish(cache, 5) && contents(cache) == "short");
        { CacheStagingFile aborted(cache, 16); CHECK(aborted.data); }
        CHECK(contents(cache) == "short");
        CHECK(!write_pipeline_cache_file(cache, saved.data(), saved.size()));
        const auto orphan = dir / "cache.bin.tmp.140.0", fresh = dir / "cache.bin.tmp.141.0";
        const auto held = dir / "cache.bin.tmp.142.0";
        for (const auto& path : {orphan, fresh, held}) std::ofstream(path) << "partial";
        auto old = fs::file_time_type::clock::now() - std::chrono::hours(1);
        fs::last_write_time(orphan, old);
        fs::last_write_time(held, old);
        HANDLE live = CreateFileW(held.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        CHECK(live != INVALID_HANDLE_VALUE);
        clean_stale_cache_files(cache);
        CHECK(!fs::exists(orphan) && fs::exists(fresh) && fs::exists(held) && contents(cache) == saved);
        CloseHandle(live);
        fs::remove(fresh);
        fs::remove(held);
    }
    // A reader that did not share DELETE access prevents replacement.
    HANDLE held = CreateFileW(cache.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(held != INVALID_HANDLE_VALUE);
    const auto failed = write_pipeline_cache_file(cache, "next", 4);
    CloseHandle(held);
    CHECK(failed);
#else
    // A buffered write may first report the size limit when it flushes or closes.
    struct rlimit original;
    CHECK(getrlimit(RLIMIT_FSIZE, &original) == 0);
    struct rlimit limited = original;
    limited.rlim_cur = 2;
    CHECK(std::signal(SIGXFSZ, SIG_IGN) != SIG_ERR);
    CHECK(setrlimit(RLIMIT_FSIZE, &limited) == 0);
    const auto failed = write_pipeline_cache_file(cache, "next", 4);
    const auto absent = dir / "absent";
    const auto failed_create = write_pipeline_cache_file(absent, "next", 4);
    CHECK(setrlimit(RLIMIT_FSIZE, &original) == 0);
    CHECK(failed && failed_create);
    CHECK(!fs::exists(absent));
#endif
    CHECK(contents(cache) == saved);
    CHECK(contents(partial) == "unrelated");
    CHECK(std::distance(fs::directory_iterator(dir), fs::directory_iterator{}) == 3);
    return 0;
}

int main() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto dir = fs::temp_directory_path() / ("leonida_pipeline_cache_" + std::to_string(stamp));
    CHECK(fs::create_directory(dir));
    const int result = test_checkpoint(dir);
    fs::remove_all(dir);
    return result;
}
