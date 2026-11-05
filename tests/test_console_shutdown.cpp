// Windows-only: ShutdownSignal against a second console handler, the way CEF
// adds one when it starts (console_shutdown.cpp explains why that matters).
//
// A real Ctrl-C goes to every process on the console, ctest included, so the
// test runs itself again as a child with a console of its own and no window
// (CREATE_NO_WINDOW). The child sends itself Ctrl-C and checks who got it; the
// parent only checks the child passed.

#include "reactor.hpp"
#include "shutdown_signal.hpp"

#include <windows.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <expected>
#include <string>
#include <system_error>

namespace {

constexpr const wchar_t* kChildVariable = L"AM_CONSOLE_SHUTDOWN_CHILD";

// Stands in for Chromium's handler: takes every event and says it's handled.
std::atomic<int> g_foreign_calls{0};

BOOL WINAPI foreign_handler(DWORD) {
    g_foreign_calls.fetch_add(1);
    return TRUE;
}

bool is_child() {
    wchar_t value[2] = {};
    return ::GetEnvironmentVariableW(kChildVariable, value, 2) == 1 && value[0] == L'1';
}

// Runs this test executable again with only `test_name`, in a console of its
// own, and returns its exit code (-1 if it couldn't start or didn't finish).
int run_child(const wchar_t* test_name) {
    wchar_t exe[MAX_PATH] = {};
    if (::GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0) {
        return -1;
    }
    std::wstring command = L"\"" + std::wstring{exe} + L"\" \"" + test_name + L"\"";

    // Shares this process's output, so the child's failures show in the log.
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = ::GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);

    ::SetEnvironmentVariableW(kChildVariable, L"1");
    PROCESS_INFORMATION process{};
    const BOOL started = ::CreateProcessW(exe, command.data(), nullptr, nullptr, TRUE,
                                          CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    ::SetEnvironmentVariableW(kChildVariable, nullptr);
    if (started == 0) {
        return -1;
    }
    ::CloseHandle(process.hThread);
    DWORD code = static_cast<DWORD>(-1);
    if (::WaitForSingleObject(process.hProcess, 30'000) == WAIT_OBJECT_0) {
        ::GetExitCodeProcess(process.hProcess, &code);
    } else {
        ::TerminateProcess(process.hProcess, 1);
    }
    ::CloseHandle(process.hProcess);
    return static_cast<int>(code);
}

template <typename Done> bool wait_for(Done done, am::pal::Reactor& reactor) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        (void)reactor.run_once(std::chrono::milliseconds(20));
    }
    return true;
}

} // namespace

TEST_CASE("reclaim puts the daemon's Ctrl-C handler back in front") {
    if (!is_child()) {
        CHECK(run_child(L"reclaim puts the daemon's Ctrl-C handler back in front") == 0);
        return;
    }

    // In the child, with a console nobody else is on. A parent that ignores
    // Ctrl-C passes that on, so turn it back on.
    REQUIRE(::SetConsoleCtrlHandler(nullptr, FALSE) != 0);

    auto reactor = am::pal::Reactor::create();
    REQUIRE(reactor);
    std::atomic<int> ours{0};
    auto shutdown = am::pal::ShutdownSignal::watch(*reactor, [&] { ours.fetch_add(1); });
    REQUIRE(shutdown);

    // Like CEF starting: a newer handler that takes everything.
    REQUIRE(::SetConsoleCtrlHandler(foreign_handler, TRUE) != 0);

    // Without reclaim(), Ctrl-C never reaches the daemon.
    REQUIRE(::GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0) != 0);
    CHECK(wait_for([] { return g_foreign_calls.load() == 1; }, *reactor));
    (void)reactor->run_once(std::chrono::milliseconds(200));
    CHECK(ours.load() == 0);

    // After it, the daemon's handler is asked first, and the other one not at all.
    REQUIRE(shutdown->reclaim());
    REQUIRE(::GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0) != 0);
    CHECK(wait_for([&] { return ours.load() == 1; }, *reactor));
    CHECK(g_foreign_calls.load() == 1);

    // Once the ShutdownSignal is gone it takes nothing, the reclaimed copy
    // included: Ctrl-C reaches the other handler again.
    shutdown = std::unexpected(std::error_code{});
    REQUIRE(::GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0) != 0);
    CHECK(wait_for([] { return g_foreign_calls.load() == 2; }, *reactor));
    CHECK(ours.load() == 1);

    ::SetConsoleCtrlHandler(foreign_handler, FALSE);
}
