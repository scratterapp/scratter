// launch_ui() on Linux (src/pal/linux/process_launch.cpp).
//
// test_daemon.cpp starts the real window through MPRIS Raise. This checks the
// parts it doesn't: a program that isn't there, and that a window which has
// closed is reaped instead of staying a zombie.

#include "process_launch.hpp"
#include "reactor.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <sys/wait.h>

using namespace std::chrono_literals;

TEST_CASE("launch_ui says when the program isn't there", "[process_launch]") {
    auto reactor = am::pal::Reactor::create();
    REQUIRE(reactor);
    const auto missing = std::filesystem::temp_directory_path() / "am-no-such-dir" / "am-ui";
    auto started = am::pal::launch_ui(*reactor, missing, {});
    REQUIRE_FALSE(started);
    CHECK(started.error() == std::errc::no_such_file_or_directory);
}

TEST_CASE("launch_ui says when the program can't be run", "[process_launch]") {
    auto reactor = am::pal::Reactor::create();
    REQUIRE(reactor);
    // A folder exists but isn't a program.
    auto started = am::pal::launch_ui(*reactor, std::filesystem::temp_directory_path(), {});
    REQUIRE_FALSE(started);
    CHECK(started.error() == std::errc::permission_denied);
}

TEST_CASE("a window that closed is reaped by the reactor", "[process_launch]") {
    auto reactor = am::pal::Reactor::create();
    REQUIRE(reactor);
    // `true` stands in for am-ui: it starts and exits at once.
    const std::vector<std::string> args = {"--ignored"};
    REQUIRE(am::pal::launch_ui(*reactor, "/bin/true", args));

    // Its pidfd turns readable when it exits, and the callback collects it.
    // Once it has, this process has no children left, zombie or not. WNOWAIT
    // only looks, so this loop never collects the child itself.
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    bool reaped = false;
    while (!reaped && std::chrono::steady_clock::now() < deadline) {
        REQUIRE(reactor->run_once(100ms));
        siginfo_t info{};
        errno = 0;
        reaped = ::waitid(P_ALL, 0, &info, WEXITED | WNOHANG | WNOWAIT) == -1 && errno == ECHILD;
    }
    CHECK(reaped);
}
