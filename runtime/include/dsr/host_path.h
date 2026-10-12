#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <limits>
#include <stdexcept>
#include <system_error>
#endif

namespace dsr::host {

// SDL, Godot and the session C ABI exchange UTF-8 rather than the Windows ACP.
inline std::filesystem::path path_from_utf8(std::string_view text) {
#if defined(_WIN32)
    return std::filesystem::u8path(text.begin(), text.end());
#else
    return std::filesystem::path(text);
#endif
}

#if defined(_WIN32)
inline std::string utf8_from_wide(std::wstring_view text) {
    if (text.empty()) return {};
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::length_error("UTF-16 path is too long");
    const int count = static_cast<int>(text.size());
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), count,
                                         nullptr, 0, nullptr, nullptr);
    if (bytes == 0)
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "convert UTF-16 path to UTF-8");
    std::string result(static_cast<std::size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), count,
                            result.data(), bytes, nullptr, nullptr) == 0)
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "convert UTF-16 path to UTF-8");
    return result;
}
#endif

inline std::string path_to_utf8(const std::filesystem::path& path) {
#if defined(_WIN32)
    return utf8_from_wide(path.native());
#else
    return path.string();
#endif
}

inline std::filesystem::path path_with_suffix(std::filesystem::path path, const char* suffix) {
    path += suffix;
    return path;
}

} // namespace dsr::host
