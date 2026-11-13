// launch_ui() on Windows (src/pal/win32/process_launch.cpp, issue #41).
//
// Linux's version is tested through the daemon and MPRIS in test_daemon.cpp.
// Windows' media flyout has no button that opens the app, so here launch_ui()
// starts launch_child (tests/launch_child.cpp) directly, and the child reports
// what it got.

#include "process_launch.hpp"
#include "reactor.hpp"

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

const std::filesystem::path kChild = AM_LAUNCH_CHILD;

// A report file of its own for each test, passed to the child as
// AM_LAUNCH_CHILD_OUT (it inherits the environment).
struct Report {
    std::filesystem::path path;
    explicit Report(const char* tag) {
        path = std::filesystem::temp_directory_path() /
               ("am-launch-" + std::to_string(GetCurrentProcessId()) + "-" + tag + ".txt");
        std::filesystem::remove(path);
        SetEnvironmentVariableW(L"AM_LAUNCH_CHILD_OUT", path.c_str());
    }
    ~Report() { std::filesystem::remove(path); }
    Report(const Report&) = delete;
    Report& operator=(const Report&) = delete;

    // The child's report once it's there, or "" after five seconds.
    [[nodiscard]] std::string wait() const {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!std::filesystem::exists(path)) {
            if (std::chrono::steady_clock::now() > deadline) {
                return {};
            }
            std::this_thread::sleep_for(10ms);
        }
        std::ifstream in(path, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
    }
};

std::string hex(const std::string& bytes) {
    std::string out;
    char pair[3];
    for (unsigned char c : bytes) {
        std::snprintf(pair, sizeof(pair), "%02x", c);
        out += pair;
    }
    return out;
}

} // namespace

TEST_CASE("launch_ui passes every argument through unchanged", "[process_launch]") {
    auto reactor = am::pal::Reactor::create();
    REQUIRE(reactor.has_value());
    Report report("args");

    // What the command line's quoting has to get right: spaces, quotes,
    // backslashes inside, before a quote and at the end, an empty argument,
    // and text that isn't ASCII.
    const std::vector<std::string> args = {
        "--name",
        "plain",
        "two words",
        "",
        "say \"hi\"",
        "back\\slash",
        "ends in\\",
        "ends in quote\\\"",
        "C:\\dir with space\\",
        "\xce\xba\xce\xad\xcf\x81\xce\xbf", // Greek, four letters
    };
    REQUIRE(am::pal::launch_ui(*reactor, kChild, args).has_value());

    std::string expected;
    for (std::size_t i = 0; i < args.size(); ++i) {
        expected += "arg " + std::to_string(i + 1) + " " + hex(args[i]) + "\n";
    }
    // Standard input reads NUL: nothing.
    expected += "stdin 0\ndone\n";
    CHECK(report.wait() == expected);
}

TEST_CASE("launch_ui with no arguments", "[process_launch]") {
    auto reactor = am::pal::Reactor::create();
    REQUIRE(reactor.has_value());
    Report report("none");
    REQUIRE(am::pal::launch_ui(*reactor, kChild, {}).has_value());
    CHECK(report.wait() == "stdin 0\ndone\n");
}

TEST_CASE("launch_ui says when the program isn't there", "[process_launch]") {
    auto reactor = am::pal::Reactor::create();
    REQUIRE(reactor.has_value());
    auto started = am::pal::launch_ui(*reactor, kChild.parent_path() / "no-such-window.exe", {});
    REQUIRE_FALSE(started.has_value());
    CHECK(started.error() == std::errc::no_such_file_or_directory);

    started = am::pal::launch_ui(*reactor, kChild.parent_path() / "no-such-dir" / "am-ui.exe", {});
    REQUIRE_FALSE(started.has_value());
    CHECK(started.error() == std::errc::no_such_file_or_directory);
}

TEST_CASE("launch_ui refuses an argument that isn't UTF-8", "[process_launch]") {
    auto reactor = am::pal::Reactor::create();
    REQUIRE(reactor.has_value());
    const std::vector<std::string> args = {"\xff\xfe"};
    auto started = am::pal::launch_ui(*reactor, kChild, args);
    REQUIRE_FALSE(started.has_value());
    CHECK(started.error() == std::errc::invalid_argument);
}

// Every launch has to close the two handles CreateProcessW hands back and the
// copies of the standard handles it passed on. A leak shows as the daemon's
// handle count going up with every window.
TEST_CASE("launch_ui keeps no handles", "[process_launch]") {
    auto reactor = am::pal::Reactor::create();
    REQUIRE(reactor.has_value());
    Report report("handles");
    // One launch first, so whatever Windows sets up once and keeps is already
    // counted.
    REQUIRE(am::pal::launch_ui(*reactor, kChild, {}).has_value());
    REQUIRE_FALSE(report.wait().empty());

    DWORD before = 0;
    REQUIRE(GetProcessHandleCount(GetCurrentProcess(), &before));
    for (int i = 0; i < 20; ++i) {
        std::filesystem::remove(report.path);
        REQUIRE(am::pal::launch_ui(*reactor, kChild, {}).has_value());
        REQUIRE_FALSE(report.wait().empty());
    }
    DWORD after = 0;
    REQUIRE(GetProcessHandleCount(GetCurrentProcess(), &after));
    // Leaking even one handle per launch would be 20 more. A few either way
    // are Windows' own bookkeeping.
    CHECK(after <= before + 3);
}
