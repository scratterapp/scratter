#pragma once

// Starting the real daemon from a test: a private $XDG_RUNTIME_DIR and a
// daemon child process that's killed when the test ends. Shared by
// test_daemon.cpp and test_daemon_link.cpp.
//
// Linux-only: it uses mkdtemp, posix_spawn and kill. AM_DAEMON_PATH comes from
// tests/CMakeLists.txt.

#include <catch2/catch_test_macros.hpp>

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

extern char** environ;

namespace am::test {

using namespace std::chrono_literals;

// A temporary $XDG_RUNTIME_DIR for this test process and the daemons it starts.
struct RuntimeDir {
    std::filesystem::path path;

    RuntimeDir() {
        std::string templ = (std::filesystem::temp_directory_path() / "am-daemon-XXXXXX").string();
        REQUIRE(::mkdtemp(templ.data()) != nullptr);
        path = templ;
        REQUIRE(::setenv("XDG_RUNTIME_DIR", path.c_str(), 1) == 0);
    }
    ~RuntimeDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

// A failed REQUIRE aborts the test process (no exceptions, so no destructors
// run). Without this, the daemon child would keep running, and ctest would
// wait forever for it to close the output it inherited.
inline volatile pid_t g_daemon_to_kill = -1;

extern "C" inline void kill_daemon_on_abort(int) {
    if (g_daemon_to_kill > 0) {
        ::kill(g_daemon_to_kill, SIGKILL);
    }
    ::signal(SIGABRT, SIG_DFL);
    ::raise(SIGABRT);
}

// A daemon child process. The destructor kills it if the test didn't stop it.
struct Daemon {
    pid_t pid = -1;

    explicit Daemon(const std::string& name) {
        std::vector<std::string> args = {AM_DAEMON_PATH, "--name", name, "--no-audio"};
        std::vector<char*> argv;
        for (auto& arg : args) {
            argv.push_back(arg.data());
        }
        argv.push_back(nullptr);
        REQUIRE(::posix_spawn(&pid, AM_DAEMON_PATH, nullptr, nullptr, argv.data(), environ) == 0);
        if (g_daemon_to_kill <= 0) { // the first one; a second exits on its own
            g_daemon_to_kill = pid;
            ::signal(SIGABRT, kill_daemon_on_abort);
        }
    }
    ~Daemon() {
        if (pid > 0) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, nullptr, 0);
            exited();
        }
    }
    void exited() {
        if (g_daemon_to_kill == pid) {
            g_daemon_to_kill = -1;
        }
        pid = -1;
    }

    // Waits for the process to exit and returns its exit code, or nothing if
    // it didn't exit within `timeout` or was killed by a signal.
    std::optional<int> wait_for_exit(std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            int status = 0;
            if (::waitpid(pid, &status, WNOHANG) == pid) {
                exited();
                if (WIFEXITED(status)) {
                    return WEXITSTATUS(status);
                }
                return std::nullopt;
            }
            std::this_thread::sleep_for(10ms);
        }
        return std::nullopt;
    }
};

// A name no other test process uses at the same time.
inline std::string unique_name() { return "am-test-daemon-" + std::to_string(::getpid()); }

} // namespace am::test
