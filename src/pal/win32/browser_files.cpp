// Windows implementation of src/pal/interfaces/browser_files.hpp.

#include "browser_files.hpp"

#include "common/app_name.hpp"

#include <windows.h>

#include <knownfolders.h>
#include <shlobj.h>

#include <array>
#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace am::pal {

namespace {

// Where browsers keep the Widevine CDM on Windows, below a known folder. Each
// of these holds one folder per version (manifest.json and
// _platform_specific\win_x64\widevinecdm.dll inside). The component updater
// writes the first kind into the browser's profile; Chrome also bundles a copy
// next to its own program, in a folder named after Chrome's version.
struct CdmPlace {
    const KNOWNFOLDERID* base;
    std::wstring_view versions; // the folder with one subfolder per version
    std::wstring_view inside;   // the CDM's folder inside that version folder
};

constexpr std::array<CdmPlace, 5> kCdmPlaces = {{
    {&FOLDERID_LocalAppData, L"Google\\Chrome\\User Data\\WidevineCdm", L""},
    {&FOLDERID_ProgramFiles, L"Google\\Chrome\\Application", L"WidevineCdm"},
    {&FOLDERID_LocalAppData, L"Google\\Chrome\\Application", L"WidevineCdm"}, // per-user Chrome
    {&FOLDERID_LocalAppData, L"Chromium\\User Data\\WidevineCdm", L""},
    {&FOLDERID_LocalAppData, L"Microsoft\\Edge\\User Data\\WidevineCdm", L""},
}};

// GetModuleFileNameW's buffer can't grow past the longest path Windows allows.
constexpr DWORD kMaxPathChars = 32768;

std::error_code os_error(DWORD code) { return {static_cast<int>(code), std::system_category()}; }

std::expected<std::filesystem::path, std::error_code> known_folder(const KNOWNFOLDERID& id) {
    PWSTR raw = nullptr;
    const HRESULT result = SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw);
    if (FAILED(result)) {
        CoTaskMemFree(raw); // the docs say to free it even on failure
        return std::unexpected(os_error(HRESULT_CODE(result)));
    }
    std::filesystem::path folder(raw);
    CoTaskMemFree(raw);
    return folder;
}

// "155.0.8059.85" as numbers, or nothing if the name isn't a version.
std::optional<std::vector<unsigned>> parse_version(std::wstring_view name) {
    std::vector<unsigned> parts;
    std::string digits;
    for (size_t start = 0; start <= name.size();) {
        size_t end = name.find(L'.', start);
        if (end == std::wstring_view::npos) {
            end = name.size();
        }
        digits.clear();
        for (wchar_t c : name.substr(start, end - start)) {
            if (c < L'0' || c > L'9') {
                return std::nullopt;
            }
            digits += static_cast<char>(c);
        }
        unsigned part = 0;
        auto [ptr, error] = std::from_chars(digits.data(), digits.data() + digits.size(), part);
        if (digits.empty() || error != std::errc{} || ptr != digits.data() + digits.size()) {
            return std::nullopt;
        }
        parts.push_back(part);
        start = end + 1;
    }
    return parts;
}

// The newest version folder under `versions` whose `inside` folder has a
// manifest.json in it.
std::optional<std::filesystem::path> newest_cdm(const std::filesystem::path& versions,
                                                std::wstring_view inside) {
    std::error_code error;
    std::filesystem::directory_iterator it(versions, error);
    if (error) {
        return std::nullopt;
    }
    std::optional<std::filesystem::path> best;
    std::vector<unsigned> best_version;
    for (; it != std::filesystem::directory_iterator(); it.increment(error)) {
        if (error) {
            break;
        }
        auto version = parse_version(it->path().filename().native());
        if (!version || (best && *version <= best_version)) {
            continue;
        }
        auto cdm = it->path() / inside;
        if (std::filesystem::is_regular_file(cdm / "manifest.json", error)) {
            best = std::move(cdm);
            best_version = std::move(*version);
        }
    }
    return best;
}

} // namespace

std::expected<std::filesystem::path, std::error_code> executable_dir() {
    std::wstring exe(MAX_PATH, L'\0');
    for (;;) {
        const DWORD size = static_cast<DWORD>(exe.size());
        const DWORD written = GetModuleFileNameW(nullptr, exe.data(), size);
        if (written == 0) {
            return std::unexpected(os_error(GetLastError()));
        }
        if (written < size) {
            exe.resize(written);
            return std::filesystem::path(exe).parent_path();
        }
        // Cut short: the path didn't fit.
        if (size >= kMaxPathChars) {
            return std::unexpected(std::make_error_code(std::errc::filename_too_long));
        }
        exe.resize(size * 2 > kMaxPathChars ? kMaxPathChars : size * 2);
    }
}

std::expected<std::filesystem::path, std::error_code> browser_profile_dir() {
    auto local = known_folder(FOLDERID_LocalAppData);
    if (!local) {
        return std::unexpected(local.error());
    }
    auto dir = *local / common::kAppName / "browser";
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    if (error) {
        return std::unexpected(error);
    }
    return dir;
}

std::expected<std::filesystem::path, std::error_code> find_widevine_cdm() {
    for (const CdmPlace& place : kCdmPlaces) {
        auto base = known_folder(*place.base);
        if (!base) {
            continue;
        }
        if (auto cdm = newest_cdm(*base / place.versions, place.inside)) {
            return *cdm;
        }
    }
    return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
}

} // namespace am::pal
