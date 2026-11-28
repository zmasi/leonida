#include "shader_fixture_path.h"
#include <cstdio>
#include <chrono>
#include <fstream>
#include <string>

int main() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("leonida_fixture_path_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root / "generated");
    const auto previous = fs::current_path();
    int result = 0;
    try {
        fs::current_path(root);
        std::ofstream("generated/test.spv") << "packaged";
        const auto original = root / "build" / "test.spv";
        if (vktest::shader_fixture_path(original) != fs::path("generated/test.spv")) result = 1;
        fs::create_directory("build");
        std::ofstream(original) << "local";
        if (vktest::shader_fixture_path(original) != original) result = 1;
        const auto missing = root / "build" / "missing.spv";
        if (vktest::shader_fixture_path(missing) != missing) result = 1;
        fs::remove(original);
        fs::remove("generated/test.spv");
        fs::create_directory("generated/test.spv");
        if (vktest::shader_fixture_path(original) != original) result = 1;

        // The CMake fixture macros pass narrow UTF-8 strings.
        const auto unicode = root / u8"\u6771\u4eac" / u8"\u96ea.spv";
        const auto encoded = unicode.u8string();
        const std::string compiled_utf8(encoded.begin(), encoded.end());
        fs::create_directory(unicode.parent_path());
        std::ofstream(unicode) << "local unicode";
        if (vktest::shader_fixture_path(compiled_utf8.c_str()) != unicode) result = 1;
        fs::remove(unicode);
        const auto packaged_unicode = fs::path("generated") / unicode.filename();
        std::ofstream(packaged_unicode) << "packaged unicode";
        if (vktest::shader_fixture_path(compiled_utf8.c_str()) != packaged_unicode) result = 1;
    } catch (...) { result = 1; }
    fs::current_path(previous);
    fs::remove_all(root);
    std::puts(result ? "shader fixture lookup failed" : "shader fixture lookup passed");
    return result;
}
