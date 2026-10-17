// TODO: Find the executable's folder, the browser profile folder and the
// installed Widevine CDM on Windows. Owner: @kerelosgayed (issue #80)
//
// Until then these are stubs that fail with "not supported", so am_pal links.
// Only a daemon built with AM_WITH_CEF calls them.

#include "browser_files.hpp"

namespace am::pal {

std::expected<std::filesystem::path, std::error_code> executable_dir() {
    return std::unexpected(std::make_error_code(std::errc::function_not_supported));
}

std::expected<std::filesystem::path, std::error_code> browser_profile_dir() {
    return std::unexpected(std::make_error_code(std::errc::function_not_supported));
}

std::expected<std::filesystem::path, std::error_code> find_widevine_cdm() {
    return std::unexpected(std::make_error_code(std::errc::function_not_supported));
}

} // namespace am::pal
