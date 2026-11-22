// Tests for the Linux side of browser_files.hpp.
//
// browser_profile_dir() follows $XDG_CACHE_HOME, or $HOME without it, so each
// test points them at a fresh folder under the system temp folder and puts the
// old values back after.

#include "browser_files.hpp"

#include "common/app_name.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

#include <unistd.h>

namespace {

namespace fs = std::filesystem;

// Sets (or, given nullopt, unsets) an environment variable for as long as it
// lives, then puts the old value back.
class ScopedEnv {
  public:
    ScopedEnv(const char* name, const std::optional<std::string>& value) : name_(name) {
        if (const char* old = std::getenv(name)) {
            old_ = old;
        }
        if (value) {
            setenv(name, value->c_str(), 1);
        } else {
            unsetenv(name);
        }
    }
    ~ScopedEnv() {
        if (old_) {
            setenv(name_, old_->c_str(), 1);
        } else {
            unsetenv(name_);
        }
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

  private:
    const char* name_;
    std::optional<std::string> old_;
};

// Sets XDG_CACHE_HOME to an empty temporary folder for as long as it lives,
// then deletes the folder and restores the variable.
class TempCacheHome {
  public:
    TempCacheHome()
        : root_(fs::temp_directory_path() / ("am_test_browser_files." + std::to_string(getpid()))) {
        std::error_code error;
        fs::remove_all(root_, error);
        fs::create_directories(root_, error);
        if (const char* old = std::getenv("XDG_CACHE_HOME")) {
            old_ = old;
        }
        setenv("XDG_CACHE_HOME", root_.c_str(), 1);
    }
    ~TempCacheHome() {
        if (old_) {
            setenv("XDG_CACHE_HOME", old_->c_str(), 1);
        } else {
            unsetenv("XDG_CACHE_HOME");
        }
        std::error_code error;
        fs::remove_all(root_, error);
    }
    TempCacheHome(const TempCacheHome&) = delete;
    TempCacheHome& operator=(const TempCacheHome&) = delete;

    [[nodiscard]] const fs::path& root() const { return root_; }

  private:
    fs::path root_;
    std::optional<std::string> old_;
};

// The permission bits of `path`, as in `ls -l` (0700 is rwx------).
fs::perms mode_of(const fs::path& path) { return fs::status(path).permissions() & fs::perms::mask; }

} // namespace

TEST_CASE("the browser profile goes under XDG_CACHE_HOME") {
    TempCacheHome home;
    auto dir = am::pal::browser_profile_dir();
    REQUIRE(dir.has_value());
    CHECK(*dir == home.root() / am::common::kAppName / "browser");
    CHECK(fs::is_directory(*dir));
}

TEST_CASE("only its owner can open the browser profile") {
    TempCacheHome home;
    auto dir = am::pal::browser_profile_dir();
    REQUIRE(dir.has_value());
    CHECK(mode_of(*dir) == fs::perms::owner_all);
}

TEST_CASE("a profile left readable by an older build is made private") {
    TempCacheHome home;
    const auto old_dir = home.root() / am::common::kAppName / "browser";
    fs::create_directories(old_dir);
    fs::permissions(old_dir, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                                 fs::perms::others_read | fs::perms::others_exec);
    REQUIRE(mode_of(old_dir) != fs::perms::owner_all);

    auto dir = am::pal::browser_profile_dir();
    REQUIRE(dir.has_value());
    CHECK(mode_of(*dir) == fs::perms::owner_all);
}

TEST_CASE("without XDG_CACHE_HOME the browser profile goes under ~/.cache") {
    TempCacheHome home; // only for its folder, and to restore XDG_CACHE_HOME
    const ScopedEnv no_xdg("XDG_CACHE_HOME", std::nullopt);
    const ScopedEnv fake_home("HOME", home.root().string());

    auto dir = am::pal::browser_profile_dir();
    REQUIRE(dir.has_value());
    CHECK(*dir == home.root() / ".cache" / am::common::kAppName / "browser");
    CHECK(fs::is_directory(*dir));
}

TEST_CASE("a relative XDG_CACHE_HOME is ignored, as the XDG spec says") {
    TempCacheHome home;
    const ScopedEnv relative("XDG_CACHE_HOME", "relative/cache");
    const ScopedEnv fake_home("HOME", home.root().string());

    auto dir = am::pal::browser_profile_dir();
    REQUIRE(dir.has_value());
    CHECK(*dir == home.root() / ".cache" / am::common::kAppName / "browser");
}

TEST_CASE("with neither XDG_CACHE_HOME nor HOME there's no browser profile") {
    TempCacheHome home;
    const ScopedEnv no_xdg("XDG_CACHE_HOME", std::nullopt);
    const ScopedEnv no_home("HOME", std::nullopt);

    auto dir = am::pal::browser_profile_dir();
    REQUIRE_FALSE(dir.has_value());
    CHECK(dir.error() == std::errc::no_such_file_or_directory);
}

TEST_CASE("a cache folder that's really a file is reported") {
    TempCacheHome home;
    const fs::path file = home.root() / "not-a-folder";
    std::ofstream(file) << "x";
    const ScopedEnv xdg("XDG_CACHE_HOME", file.string());

    auto dir = am::pal::browser_profile_dir();
    REQUIRE_FALSE(dir.has_value());
    CHECK(dir.error());
}

TEST_CASE("the executable's folder is the one this test program is in") {
    auto dir = am::pal::executable_dir();
    REQUIRE(dir.has_value());
    CHECK(*dir == fs::canonical("/proc/self/exe").parent_path());
    CHECK(fs::is_regular_file(*dir / "test_browser_files"));
}

TEST_CASE("the Widevine CDM is found where a browser put it, or reported missing") {
    // Which one depends on this machine: a desktop with Chromium or Chrome has
    // it, a CI runner usually doesn't.
    auto cdm = am::pal::find_widevine_cdm();
    if (cdm) {
        CHECK(fs::is_regular_file(*cdm / "manifest.json"));
    } else {
        CHECK(cdm.error() == std::errc::no_such_file_or_directory);
    }
}
