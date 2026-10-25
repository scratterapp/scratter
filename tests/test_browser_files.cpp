// Tests for the Linux side of browser_files.hpp.
//
// browser_profile_dir() follows $XDG_CACHE_HOME, so each test points it at a
// fresh folder under the system temp folder and puts the old value back after.

#include "browser_files.hpp"

#include "common/app_name.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

#include <unistd.h>

namespace {

namespace fs = std::filesystem;

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
