// Synthetic GPU test shaders are packaged under generated/ in runtime-windows artifacts.
// Run downloaded tests from the artifact directory, as CTest does for a local build.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace vktest {
inline std::filesystem::path shader_fixture_path(const std::filesystem::path& compiled_path) {
    if (std::filesystem::is_regular_file(compiled_path)) return compiled_path;
    const auto packaged = std::filesystem::path("generated") / compiled_path.filename();
    if (std::filesystem::is_regular_file(packaged)) return packaged;
    // Preserve the original path in the caller's missing-file diagnostic.
    return compiled_path;
}

// The narrow paths supplied by CMake's fixture definitions are UTF-8.
inline std::filesystem::path shader_fixture_path(const char* compiled_utf8) {
    const std::string_view bytes(compiled_utf8);
    return shader_fixture_path(std::filesystem::path(std::u8string(bytes.begin(), bytes.end())));
}
}  // namespace vktest
