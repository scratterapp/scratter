#pragma once

// Files and folders the daemon's embedded browser (CEF) needs from the OS.
//
// CEF itself is cross-platform, but where things live isn't:
//
//   executable_dir()       Linux: from /proc/self/exe
//                          Windows: GetModuleFileNameW (not written yet)
//   browser_profile_dir()  Linux: $XDG_CACHE_HOME/<app>/browser, or
//                          ~/.cache/<app>/browser
//                          Windows: under %LOCALAPPDATA% (not written yet)
//   find_widevine_cdm()    Linux: the WidevineCdm folder Chromium or Google
//                          Chrome installed (see browser_files.cpp for the list)
//                          Windows: not written yet
//
// The Widevine CDM is Google's DRM library. We never ship, download or copy
// it (spec 3.1.1, LEGAL.md); the daemon only points CEF at the copy a browser
// already installed.

#include <expected>
#include <filesystem>
#include <system_error>

namespace am::pal {

// The folder this program's executable is in. CEF's helper program and
// libcef's resource files sit next to it.
std::expected<std::filesystem::path, std::error_code> executable_dir();

// Where the browser keeps its profile: cookies, local storage, its cache, and
// the file that tells Chromium where the Widevine CDM is. Created if missing.
std::expected<std::filesystem::path, std::error_code> browser_profile_dir();

// The installed Widevine CDM folder (the one with manifest.json in it), from a
// list of the places browsers install it. Fails with no_such_file_or_directory
// when none has it.
std::expected<std::filesystem::path, std::error_code> find_widevine_cdm();

} // namespace am::pal
