// Linux implementation of src/pal/interfaces/browser_files.hpp.

#include "browser_files.hpp"

#include "common/app_name.hpp"

#include <array>
#include <cstdlib>
#include <string_view>

namespace am::pal {

namespace {

// Where browsers put the Widevine CDM on Linux. Each folder holds
// manifest.json and _platform_specific/linux_x64/libwidevinecdm.so.
constexpr std::array<std::string_view, 3> kWidevineDirs = {
    "/usr/lib/chromium/WidevineCdm",         // Arch: chromium + chromium-widevine
    "/opt/google/chrome/WidevineCdm",        // Google Chrome
    "/usr/lib/chromium-browser/WidevineCdm", // Debian/Ubuntu-style chromium
};

} // namespace

std::expected<std::filesystem::path, std::error_code> executable_dir() {
    std::error_code error;
    auto exe = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error) {
        return std::unexpected(error);
    }
    return exe.parent_path();
}

std::expected<std::filesystem::path, std::error_code> browser_profile_dir() {
    std::filesystem::path cache;
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg != nullptr && *xdg == '/') {
        cache = xdg;
    } else if (const char* home = std::getenv("HOME"); home != nullptr && *home == '/') {
        cache = std::filesystem::path(home) / ".cache";
    } else {
        return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
    }

    auto dir = cache / common::kAppName / "browser";
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    if (error) {
        return std::unexpected(error);
    }
    return dir;
}

std::expected<std::filesystem::path, std::error_code> find_widevine_cdm() {
    for (std::string_view dir : kWidevineDirs) {
        std::error_code error;
        if (std::filesystem::is_regular_file(std::filesystem::path(dir) / "manifest.json", error)) {
            return std::filesystem::path(dir);
        }
    }
    return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
}

} // namespace am::pal
