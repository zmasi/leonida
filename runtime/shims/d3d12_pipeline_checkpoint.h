// Win32 staging for a Vulkan session cache. Fill the mapped file directly so a
// checkpoint does not need a second, cache-sized private heap allocation.
#pragma once

#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string_view>

namespace rt::d3d12 {
struct CompileActivity {
    std::atomic<unsigned>& count;
    explicit CompileActivity(std::atomic<unsigned>& active) : count(active) { ++count; }
    ~CompileActivity() { --count; }
};

inline bool is_direct_session_cache_enabled(const wchar_t* val, DWORD len) noexcept {
    return !(len == 1 && val && val[0] == L'0');
}

inline bool direct_session_cache() {
    static const bool enabled = [] {
        wchar_t value[2]{};
        const DWORD len = GetEnvironmentVariableW(L"LEONIDA_DIRECT_SESSION_CACHE", value, 2);
        return is_direct_session_cache_enabled(value, len);
    }();
    return enabled;
}

// Both generations of staging filenames use two unsigned decimal components.
inline bool cache_temp_suffix(std::wstring_view suffix) {
    const auto dot = suffix.find(L'.');
    return dot != suffix.npos && dot > 0 && dot + 1 < suffix.size() &&
           suffix.substr(0, dot).find_first_not_of(L"0123456789") == suffix.npos &&
           suffix.substr(dot + 1).find_first_not_of(L"0123456789") == suffix.npos;
}

inline void clean_stale_cache_files(const std::filesystem::path& target) {
    namespace fs = std::filesystem;
    if (target.empty()) return;
    const auto prefix = target.filename().wstring() + L".tmp.";
    const auto cutoff = fs::file_time_type::clock::now() - std::chrono::minutes(10);
    std::error_code ec;
    for (fs::directory_iterator it(target.parent_path(), ec), end; !ec && it != end; it.increment(ec)) {
        const auto name = it->path().filename().wstring();
        if (!name.starts_with(prefix) || !cache_temp_suffix(std::wstring_view(name).substr(prefix.size()))) continue;
        std::error_code stat;
        if (it->symlink_status(stat).type() != fs::file_type::regular || stat ||
            it->last_write_time(stat) >= cutoff || stat) continue;
        // No sharing: an open writer/reader prevents cleanup, even after the age threshold.
        HANDLE orphan = CreateFileW(it->path().c_str(), DELETE, 0, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (orphan != INVALID_HANDLE_VALUE) CloseHandle(orphan);
    }
}

class CacheStagingFile {
public:
    void* data = nullptr;

    CacheStagingFile(const std::filesystem::path& destination, std::size_t bytes) {
        if (destination.empty() || bytes == 0) return;
        static std::atomic<unsigned> sequence{0};
        temp_ = destination;
        temp_ += L".tmp." + std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()) + L"." + std::to_wstring(sequence++);
        file_ = CreateFileW(temp_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) return;
        const auto length = static_cast<unsigned long long>(bytes);
        HANDLE mapping = CreateFileMappingW(file_, nullptr, PAGE_READWRITE, DWORD(length >> 32), DWORD(length), nullptr);
        if (!mapping) return;
        data = MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, bytes);
        CloseHandle(mapping);
    }
    CacheStagingFile(const CacheStagingFile&) = delete;
    CacheStagingFile& operator=(const CacheStagingFile&) = delete;
    ~CacheStagingFile() {
        if (data) UnmapViewOfFile(data);
        if (file_ != INVALID_HANDLE_VALUE) {
            CloseHandle(file_);
            DeleteFileW(temp_.c_str());
        }
    }

    bool is_valid() const noexcept { return data != nullptr; }
    const std::filesystem::path& temp_path() const noexcept { return temp_; }

    bool publish(const std::filesystem::path& destination, std::size_t written) {
        if (!data || destination.empty()) return false;
        if (!UnmapViewOfFile(data)) return false;
        data = nullptr;
        LARGE_INTEGER length{};
        length.QuadPart = static_cast<LONGLONG>(written);
        if (!SetFilePointerEx(file_, length, nullptr, FILE_BEGIN) || !SetEndOfFile(file_)) return false;
        CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
        if (MoveFileExW(temp_.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
        DeleteFileW(temp_.c_str());
        return false;
    }
private:
    std::filesystem::path temp_;
    HANDLE file_ = INVALID_HANDLE_VALUE;
};
}  // namespace rt::d3d12
