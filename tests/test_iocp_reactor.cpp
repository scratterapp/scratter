// Windows-only tests for the IOCP reactor (issue #14). test_reactor.cpp covers what
// every platform shares (run loop, wake, stop, timer). This file covers the other
// half: watching handles, which means real overlapped I/O on message-mode named
// pipes, and the Windows specifics of the wakeup and the timer.
//
// Three rules keep the tests safe. Anything the kernel is still writing to (an
// OVERLAPPED, a buffer) is declared before the pipe it reads from, so closing the
// pipe finishes the operation before that memory goes away. Callbacks use CHECK,
// never REQUIRE, because this project builds Catch2 without exceptions and a
// failed REQUIRE ends the process. And timing is only ever checked from below
// ("not before"), with seconds to spare for things that must happen.

#include "reactor.hpp"

#include <windows.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

// A failed REQUIRE ends the process (this project builds Catch2 without
// exceptions), and the debug CRT answers with a modal "abort() has been called"
// dialog that holds up ctest until somebody clicks it. Keep the abort, drop the dialog.
[[maybe_unused]] const bool kQuietAbort = [] {
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    return true;
}();

using am::pal::Interest;
using am::pal::IoEvent;
using am::pal::NativeHandle;
using am::pal::Reactor;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

constexpr std::uint32_t kErrorAborted = ERROR_OPERATION_ABORTED;
constexpr std::uint32_t kErrorBrokenPipe = ERROR_BROKEN_PIPE;
constexpr std::uint32_t kErrorMoreData = ERROR_MORE_DATA;
constexpr std::uint32_t kErrorPipeConnected = ERROR_PIPE_CONNECTED;
constexpr DWORD kIoPending = ERROR_IO_PENDING;
constexpr DWORD kPipeConnected = ERROR_PIPE_CONNECTED;
constexpr DWORD kWaitObject0 = WAIT_OBJECT_0;

NativeHandle native(HANDLE h) { return NativeHandle{reinterpret_cast<std::intptr_t>(h)}; }

// Calls run_once() until `done` returns true or `limit` passes.
template <typename Done>
bool run_until(Reactor& reactor, Done done, std::chrono::milliseconds limit = 2000ms) {
    const auto deadline = Clock::now() + limit;
    while (!done()) {
        if (Clock::now() > deadline) {
            return false;
        }
        if (!reactor.run_once(10ms)) {
            return false;
        }
    }
    return true;
}

// A connected pair of named pipe ends in message mode.
//   server: overlapped, the end the reactor watches
//   client: synchronous, the end the tests write to
struct PipePair {
    HANDLE server = INVALID_HANDLE_VALUE;
    HANDLE client = INVALID_HANDLE_VALUE;

    PipePair() = default;
    PipePair(const PipePair&) = delete;
    PipePair& operator=(const PipePair&) = delete;
    ~PipePair() {
        close_server();
        close_client();
    }

    void close_server() {
        if (server != INVALID_HANDLE_VALUE) {
            ::CloseHandle(server);
            server = INVALID_HANDLE_VALUE;
        }
    }
    void close_client() {
        if (client != INVALID_HANDLE_VALUE) {
            ::CloseHandle(client);
            client = INVALID_HANDLE_VALUE;
        }
    }
};

std::wstring unique_pipe_name() {
    std::random_device rd;
    return L"\\\\.\\pipe\\am_iocp_test_" + std::to_wstring(rd()) + L"_" + std::to_wstring(rd());
}

// Creates both ends. The client has connected, but the server end hasn't called
// ConnectNamedPipe yet.
void open_pair(PipePair& pair) {
    const std::wstring name = unique_pipe_name();
    pair.server = ::CreateNamedPipeW(
        name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096,
        4096, 0, nullptr);
    REQUIRE(pair.server != INVALID_HANDLE_VALUE);
    pair.client = ::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                OPEN_EXISTING, 0, nullptr);
    REQUIRE(pair.client != INVALID_HANDLE_VALUE);
}

void connect_pair(PipePair& pair) {
    open_pair(pair);

    // The client got in first, so this reports ERROR_PIPE_CONNECTED and queues
    // no completion packet.
    OVERLAPPED ov{};
    ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    REQUIRE(ov.hEvent != nullptr);
    const BOOL connected = ::ConnectNamedPipe(pair.server, &ov);
    const DWORD err = ::GetLastError();
    ::CloseHandle(ov.hEvent);
    REQUIRE(connected == 0);
    REQUIRE(err == kPipeConnected);
}
// Writes one message of `length` bytes to the client end.
void send_message(HANDLE client, std::size_t length) {
    const std::string message(length, 'x');
    DWORD written = 0;
    REQUIRE(::WriteFile(client, message.data(), static_cast<DWORD>(message.size()), &written,
                        nullptr) != 0);
    REQUIRE(written == message.size());
}

// One overlapped ReadFile: its OVERLAPPED, the buffer it fills, and an event that
// is signalled when it finishes, whether or not a completion port hears about it.
struct PendingRead {
    OVERLAPPED ov{};
    std::array<char, 256> buffer{};
    HANDLE event = nullptr;

    PendingRead() : event(::CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    PendingRead(const PendingRead&) = delete;
    PendingRead& operator=(const PendingRead&) = delete;
    ~PendingRead() {
        if (event != nullptr) {
            ::CloseHandle(event);
        }
    }
};

// Starts an overlapped read of up to `length` bytes. It may finish before this
// returns or stay pending; a completion follows either way.
void start_read(HANDLE h, PendingRead& pending, DWORD length) {
    pending.ov = OVERLAPPED{};
    pending.ov.hEvent = pending.event;
    if (::ReadFile(h, pending.buffer.data(), length, nullptr, &pending.ov) == 0) {
        REQUIRE(::GetLastError() == kIoPending);
    }
}

void wait_for_completion(PendingRead& pending) {
    REQUIRE(::WaitForSingleObject(pending.event, 2000) == kWaitObject0);
}

// Watches `handle` and requires that to work.
void watch_or_fail(Reactor& reactor, HANDLE handle, Reactor::IoCallback callback) {
    auto result = reactor.watch(native(handle), Interest{}, std::move(callback));
    REQUIRE(result.has_value());
}

struct Seen {
    int count = 0;
    IoEvent last{};
};

// A callback that records the latest event.
auto recorder(Seen& seen) {
    return [&seen](const IoEvent& event) {
        ++seen.count;
        seen.last = event;
    };
}

} // namespace

TEST_CASE("a finished read is reported with its OVERLAPPED, byte count and no error",
          "[iocp][watch]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);
    PendingRead pending;
    PipePair pipe;
    connect_pair(pipe);

    Seen seen;
    watch_or_fail(*reactor, pipe.server, recorder(seen));

    start_read(pipe.server, pending, 256);
    send_message(pipe.client, 5);

    REQUIRE(run_until(*reactor, [&] { return seen.count > 0; }));
    CHECK(seen.count == 1);
    CHECK(seen.last.overlapped == &pending.ov);
    CHECK(seen.last.bytes == 5u);
    CHECK(seen.last.error == 0u);
    CHECK_FALSE(seen.last.readable);
    CHECK_FALSE(seen.last.writable);
    CHECK_FALSE(seen.last.hangup);
}

TEST_CASE("a read that finishes at once is still reported", "[iocp][watch]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);
    PendingRead pending;
    PipePair pipe;
    connect_pair(pipe);

    Seen seen;
    // Interest means nothing to a completion port, so asking for everything must
    // behave the same as asking for nothing.
    auto watched = reactor->watch(native(pipe.server), Interest{true, true}, recorder(seen));
    REQUIRE(watched.has_value());

    send_message(pipe.client, 7);          // the message is already waiting,
    start_read(pipe.server, pending, 256); // so this read finishes before it returns

    REQUIRE(run_until(*reactor, [&] { return seen.count > 0; }));
    CHECK(seen.count == 1);
    CHECK(seen.last.overlapped == &pending.ov);
    CHECK(seen.last.bytes == 7u);
    CHECK(seen.last.error == 0u);
}

TEST_CASE("a write on a watched handle is reported too", "[iocp][watch]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);
    PipePair pipe;
    connect_pair(pipe);

    Seen seen;
    watch_or_fail(*reactor, pipe.server, recorder(seen));

    OVERLAPPED ov{};
    const std::string message(9, 'y');
    if (::WriteFile(pipe.server, message.data(), static_cast<DWORD>(message.size()), nullptr,
                    &ov) == 0) {
        REQUIRE(::GetLastError() == kIoPending);
    }

    REQUIRE(run_until(*reactor, [&] { return seen.count > 0; }));
    CHECK(seen.last.overlapped == &ov);
    CHECK(seen.last.bytes == 9u);
    CHECK(seen.last.error == 0u);
}

TEST_CASE("modify succeeds for a watched handle and fails for an unknown one", "[iocp][watch]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);
    PipePair pipe;
    connect_pair(pipe);

    auto unknown = reactor->modify(native(pipe.server), Interest{true, false});
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error() == std::errc::no_such_file_or_directory);

    watch_or_fail(*reactor, pipe.server, [](const IoEvent&) {});
    CHECK(reactor->modify(native(pipe.server), Interest{true, true}).has_value());
}

TEST_CASE("watching a handle twice fails and keeps the first watch", "[iocp][watch]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);
    PendingRead pending;
    PipePair pipe;
    connect_pair(pipe);

    Seen first;
    Seen second;
    watch_or_fail(*reactor, pipe.server, recorder(first));
    auto again = reactor->watch(native(pipe.server), Interest{}, recorder(second));
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error() == std::errc::file_exists);

    start_read(pipe.server, pending, 256);
    send_message(pipe.client, 2);
    REQUIRE(run_until(*reactor, [&] { return first.count > 0; }));
    CHECK(first.count == 1);
    CHECK(second.count == 0);
}

TEST_CASE("handle values that can't be handles are rejected", "[iocp][watch]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);

    auto invalid = reactor->watch(NativeHandle{-1}, Interest{}, [](const IoEvent&) {});
    REQUIRE_FALSE(invalid.has_value());
    CHECK(invalid.error() == std::errc::invalid_argument);

    auto null = reactor->watch(NativeHandle{0}, Interest{}, [](const IoEvent&) {});
    REQUIRE_FALSE(null.has_value());
    CHECK(null.error() == std::errc::invalid_argument);
}

TEST_CASE("a handle that another reactor already watches is refused", "[iocp][watch]") {
    auto first = Reactor::create();
    auto second = Reactor::create();
    REQUIRE(first);
    REQUIRE(second);
    PendingRead pending;
    PipePair pipe;
    connect_pair(pipe);

    Seen first_seen;
    Seen second_seen;
    watch_or_fail(*first, pipe.server, recorder(first_seen));

    auto refused = second->watch(native(pipe.server), Interest{}, recorder(second_seen));
    CHECK_FALSE(refused.has_value());

    // It was not left half-watched.
    auto unwatched = second->unwatch(native(pipe.server));
    REQUIRE_FALSE(unwatched.has_value());
    CHECK(unwatched.error() == std::errc::no_such_file_or_directory);

    // The reactor that owns it still hears from it.
    start_read(pipe.server, pending, 256);
    send_message(pipe.client, 2);
    REQUIRE(run_until(*first, [&] { return first_seen.count > 0; }));
    CHECK(first_seen.count == 1);
    CHECK(second_seen.count == 0);
}

TEST_CASE("a handle that isn't overlapped is refused and left unwatched", "[iocp][watch]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);
    PipePair pipe;
    connect_pair(pipe);

    // The client end was opened without FILE_FLAG_OVERLAPPED.
    auto refused = reactor->watch(native(pipe.client), Interest{}, [](const IoEvent&) {});
    CHECK_FALSE(refused.has_value());

    auto unwatched = reactor->unwatch(native(pipe.client));
    REQUIRE_FALSE(unwatched.has_value());
    CHECK(unwatched.error() == std::errc::no_such_file_or_directory);
}

TEST_CASE("unwatching a handle that isn't watched fails", "[iocp][watch]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);
    PipePair pipe;
    connect_pair(pipe);

    auto unwatched = reactor->unwatch(native(pipe.server));
    REQUIRE_FALSE(unwatched.has_value());
    CHECK(unwatched.error() == std::errc::no_such_file_or_directory);
}

TEST_CASE("a handle can be watched again after it is unwatched", "[iocp][watch]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);
    PendingRead pending;
    PipePair pipe;
    connect_pair(pipe);

    Seen first;
    Seen second;
    watch_or_fail(*reactor, pipe.server, recorder(first));
    REQUIRE(reactor->unwatch(native(pipe.server)).has_value());
    watch_or_fail(*reactor, pipe.server, recorder(second));

    start_read(pipe.server, pending, 256);
    send_message(pipe.client, 4);
    REQUIRE(run_until(*reactor, [&] { return second.count > 0; }));
    CHECK(second.count == 1);
    CHECK(second.last.bytes == 4u);
    CHECK(first.count == 0);
}

// Windows only lets a handle leave its completion port once no operation started
// without an event is pending or waiting to be collected, so this is the order
// the pipe code has to follow: cancel, collect the completion, then unwatch.
TEST_CASE("a handle whose cancelled read was collected can be watched again", "[iocp][watch]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);
    std::array<char, 64> buffer{};
    OVERLAPPED cancelled{}; // no event, like the pipe code's reads
    OVERLAPPED later{};
    PipePair pipe;
    connect_pair(pipe);

    Seen first;
    watch_or_fail(*reactor, pipe.server, recorder(first));
    if (::ReadFile(pipe.server, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr,
                   &cancelled) == 0) {
        REQUIRE(::GetLastError() == kIoPending);
    }
    REQUIRE(::CancelIoEx(pipe.server, &cancelled) != 0);
    REQUIRE(run_until(*reactor, [&] { return first.count > 0; }));
    CHECK(first.last.error == kErrorAborted);

    REQUIRE(reactor->unwatch(native(pipe.server)).has_value());

    Seen second;
    watch_or_fail(*reactor, pipe.server, recorder(second));
    if (::ReadFile(pipe.server, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr,
                   &later) == 0) {
        REQUIRE(::GetLastError() == kIoPending);
    }
    send_message(pipe.client, 3);
    REQUIRE(run_until(*reactor, [&] { return second.count > 0; }));
    CHECK(second.last.overlapped == &later);
    CHECK(second.last.bytes == 3u);
    CHECK(first.count == 1);
}

TEST_CASE("unwatch leaves the handle open and usable", "[iocp][watch]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);
    PendingRead pending;
    PipePair pipe;
    connect_pair(pipe);

    watch_or_fail(*reactor, pipe.server, [](const IoEvent&) {});
    REQUIRE(reactor->unwatch(native(pipe.server)).has_value());

    // Plain overlapped I/O with an event still works on the handle.
    start_read(pipe.server, pending, 256);
    send_message(pipe.client, 3);
    wait_for_completion(pending);
    DWORD transferred = 0;
    REQUIRE(::GetOverlappedResult(pipe.server, &pending.ov, &transferred, FALSE) != 0);
    CHECK(transferred == 3u);
}

TEST_CASE("destroying the reactor runs nothing and leaves watched handles open", "[iocp][watch]") {
    PendingRead pending;
    PipePair pipe;
    connect_pair(pipe);

    int io_calls = 0;
    int wake_calls = 0;
    int timer_calls = 0;
    {
        auto reactor = Reactor::create();
        REQUIRE(reactor);
        reactor->set_wake_callback([&] { ++wake_calls; });
        watch_or_fail(*reactor, pipe.server, [&](const IoEvent&) { ++io_calls; });
        start_read(pipe.server, pending, 256); // still pending when the reactor goes
        REQUIRE(reactor->set_timer(1ms, [&] { ++timer_calls; }));
        reactor->wake(); // a wakeup nobody collected
    }
    CHECK(io_calls == 0);
    CHECK(wake_calls == 0);
    CHECK(timer_calls == 0);

    // The handle is still open and the read still completes.
    send_message(pipe.client, 4);
    wait_for_completion(pending);
    DWORD transferred = 0;
    REQUIRE(::GetOverlappedResult(pipe.server, &pending.ov, &transferred, FALSE) != 0);
    CHECK(transferred == 4u);
}

TEST_CASE("a cancelled read reports ERROR_OPERATION_ABORTED", "[iocp][errors]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);
    PendingRead pending;
    PipePair pipe;
    connect_pair(pipe);

    Seen seen;
    watch_or_fail(*reactor, pipe.server, recorder(seen));

    start_read(pipe.server, pending, 256);
    REQUIRE(::CancelIoEx(pipe.server, &pending.ov) != 0);

    REQUIRE(run_until(*reactor, [&] { return seen.count > 0; }));
    CHECK(seen.last.overlapped == &pending.ov);
    CHECK(seen.last.error == kErrorAborted);
    CHECK(seen.last.bytes == 0u);
}

TEST_CASE("a read on a pipe whose other end closed reports ERROR_BROKEN_PIPE", "[iocp][errors]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);
    PendingRead pending;
    PipePair pipe;
    connect_pair(pipe);

    Seen seen;
    watch_or_fail(*reactor, pipe.server, recorder(seen));

    start_read(pipe.server, pending, 256);
    pipe.close_client();

    REQUIRE(run_until(*reactor, [&] { return seen.count > 0; }));
    CHECK(seen.last.overlapped == &pending.ov);
    CHECK(seen.last.error == kErrorBrokenPipe);
}

TEST_CASE("a message bigger than the buffer reports ERROR_MORE_DATA and the bytes read",
          "[iocp][errors]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);
    PendingRead pending;
    PipePair pipe;
    connect_pair(pipe);

    Seen seen;
    watch_or_fail(*reactor, pipe.server, recorder(seen));

    start_read(pipe.server, pending, 16);
    send_message(pipe.client, 100);

    REQUIRE(run_until(*reactor, [&] { return seen.count > 0; }));
    CHECK(seen.last.overlapped == &pending.ov);
    CHECK(seen.last.error == kErrorMoreData);
    CHECK(seen.last.bytes == 16u);
}

TEST_CASE("a callback can unwatch its own handle", "[iocp][unwatch]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);
    PendingRead first_read;
    PendingRead second_read;
    PipePair pipe;
    connect_pair(pipe);

    int calls = 0;
    watch_or_fail(*reactor, pipe.server, [&](const IoEvent&) {
        ++calls;
        CHECK(reactor->unwatch(native(pipe.server)).has_value());
    });

    start_read(pipe.server, first_read, 256);
    send_message(pipe.client, 3);
    REQUIRE(run_until(*reactor, [&] { return calls > 0; }));

    // Nothing is reported once it is unwatched.
    start_read(pipe.server, second_read, 256);
    send_message(pipe.client, 3);
    wait_for_completion(second_read);
    REQUIRE(reactor->run_once(50ms).has_value());
    CHECK(calls == 1);
}

TEST_CASE("handles that unwatch each other in one batch run only one callback", "[iocp][unwatch]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);
    PendingRead read_a;
    PendingRead read_b;
    PipePair a;
    PipePair b;
    connect_pair(a);
    connect_pair(b);

    int a_calls = 0;
    int b_calls = 0;
    watch_or_fail(*reactor, a.server, [&](const IoEvent&) {
        ++a_calls;
        (void)reactor->unwatch(native(b.server));
        (void)reactor->unwatch(native(a.server));
    });
    watch_or_fail(*reactor, b.server, [&](const IoEvent&) {
        ++b_calls;
        (void)reactor->unwatch(native(a.server));
        (void)reactor->unwatch(native(b.server));
    });

    start_read(a.server, read_a, 256);
    start_read(b.server, read_b, 256);
    send_message(a.client, 4);
    send_message(b.client, 4); // both packets are queued before the reactor looks

    REQUIRE(reactor->run_once(1000ms).has_value());
    REQUIRE(reactor->run_once(50ms).has_value());
    CHECK(a_calls + b_calls == 1);
}

TEST_CASE("a packet queued before unwatch is dropped without touching its OVERLAPPED",
          "[iocp][unwatch]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);
    std::array<char, 64> buffer{};
    PipePair pipe;
    connect_pair(pipe);

    int calls = 0;
    watch_or_fail(*reactor, pipe.server, [&](const IoEvent&) { ++calls; });

    // The OVERLAPPED gets a page to itself, which is made inaccessible below. A
    // reactor that looked at it after unwatch() would fault.
    void* page = ::VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    REQUIRE(page != nullptr);
    auto* ov = static_cast<OVERLAPPED*>(page);
    REQUIRE(::ReadFile(pipe.server, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr,
                       ov) == 0);
    REQUIRE(::GetLastError() == kIoPending);
    send_message(pipe.client, 6); // finishes the read: a packet for this watch is now queued

    REQUIRE(reactor->unwatch(native(pipe.server)).has_value());

    DWORD old_protection = 0;
    REQUIRE(::VirtualProtect(page, 4096, PAGE_NOACCESS, &old_protection) != 0);
    REQUIRE(reactor->run_once(100ms).has_value());
    CHECK(calls == 0);

    ::VirtualFree(page, 0, MEM_RELEASE);
}

TEST_CASE("a completion that is still in flight when its handle is unwatched is dropped",
          "[iocp][unwatch]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);
    PendingRead pending;
    PipePair pipe;
    connect_pair(pipe);

    int calls = 0;
    watch_or_fail(*reactor, pipe.server, [&](const IoEvent&) { ++calls; });

    start_read(pipe.server, pending, 256);
    REQUIRE(reactor->unwatch(native(pipe.server)).has_value());
    send_message(pipe.client, 6); // the read finishes after the unwatch
    wait_for_completion(pending);

    REQUIRE(reactor->run_once(50ms).has_value());
    CHECK(calls == 0);
}

TEST_CASE("a completion for a handle that stays associated after unwatch is dropped by its key",
          "[iocp][unwatch]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);
    std::array<char, 64> buffer{};
    OVERLAPPED ov{}; // no event: the kernel keeps such a handle on the port while it's pending
    PipePair pipe;
    connect_pair(pipe);

    int calls = 0;
    watch_or_fail(*reactor, pipe.server, [&](const IoEvent&) { ++calls; });

    if (::ReadFile(pipe.server, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr, &ov) ==
        0) {
        REQUIRE(::GetLastError() == kIoPending);
    }
    REQUIRE(reactor->unwatch(native(pipe.server)).has_value());
    send_message(pipe.client, 6);

    // The completion packet is still posted under the old key. It is dropped.
    REQUIRE(reactor->run_once(100ms).has_value());
    CHECK(calls == 0);
}

TEST_CASE("a handle number that gets reused doesn't inherit the old watch's packets",
          "[iocp][unwatch]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);
    PendingRead first_read;
    PendingRead second_read;
    PendingRead fresh_read;
    PipePair first;
    auto second = std::make_unique<PipePair>();
    std::unique_ptr<PipePair> fresh;
    connect_pair(first);
    connect_pair(*second);

    int first_calls = 0;
    int second_calls = 0;
    int fresh_calls = 0;

    watch_or_fail(*reactor, first.server, [&](const IoEvent&) {
        ++first_calls;
        // Throw `second` away (its packet is queued right behind this one) and
        // open a new pipe, which usually gets the handle number that was just freed.
        (void)reactor->unwatch(native(second->server));
        second.reset();
        fresh = std::make_unique<PipePair>();
        connect_pair(*fresh);
        watch_or_fail(*reactor, fresh->server, [&](const IoEvent&) { ++fresh_calls; });
    });
    watch_or_fail(*reactor, second->server, [&](const IoEvent&) { ++second_calls; });

    start_read(first.server, first_read, 256);
    start_read(second->server, second_read, 256);
    send_message(first.client, 3);
    send_message(second->client, 3);

    REQUIRE(reactor->run_once(1000ms).has_value());
    REQUIRE(reactor->run_once(50ms).has_value());
    CHECK(first_calls == 1);
    CHECK(second_calls == 0);
    CHECK(fresh_calls == 0);

    // The new watch works.
    REQUIRE(fresh != nullptr);
    start_read(fresh->server, fresh_read, 256);
    send_message(fresh->client, 3);
    REQUIRE(run_until(*reactor, [&] { return fresh_calls > 0; }));
    CHECK(fresh_calls == 1);
}

TEST_CASE("ten wake() calls before the loop looks run the callback once", "[iocp][wake]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);

    int calls = 0;
    reactor->set_wake_callback([&] { ++calls; });
    for (int i = 0; i < 10; ++i) {
        reactor->wake();
    }

    REQUIRE(reactor->run_once(0ms).has_value());
    CHECK(calls == 1);
    REQUIRE(reactor->run_once(30ms).has_value());
    CHECK(calls == 1);

    // And the next wake() still gets through.
    reactor->wake();
    REQUIRE(reactor->run_once(1000ms).has_value());
    CHECK(calls == 2);
}

TEST_CASE("a wake ends run_once before its timeout", "[iocp][wake]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);

    reactor->wake();
    const auto start = Clock::now();
    REQUIRE(reactor->run_once(10s).has_value());
    CHECK(Clock::now() - start < 5s);
}

TEST_CASE("a timer cancelled after its deadline passed never runs", "[iocp][timer]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);

    bool fired = false;
    REQUIRE(reactor->set_timer(1ms, [&] { fired = true; }));
    std::this_thread::sleep_for(40ms); // the deadline passes with nobody looking
    reactor->cancel_timer();

    REQUIRE(reactor->run_once(50ms).has_value());
    CHECK_FALSE(fired);
}

TEST_CASE("a timer replaced after its deadline passed never runs", "[iocp][timer]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);

    bool old_timer = false;
    bool new_timer = false;
    REQUIRE(reactor->set_timer(1ms, [&] { old_timer = true; }));
    std::this_thread::sleep_for(40ms);
    REQUIRE(reactor->set_timer(10s, [&] { new_timer = true; }));

    REQUIRE(reactor->run_once(30ms).has_value());
    CHECK_FALSE(old_timer);
    CHECK_FALSE(new_timer);
    reactor->cancel_timer();
}

TEST_CASE("a timer replaced by one that is due runs only the new one", "[iocp][timer]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);

    bool old_timer = false;
    bool new_timer = false;
    REQUIRE(reactor->set_timer(10s, [&] { old_timer = true; }));
    REQUIRE(reactor->set_timer(0ms, [&] { new_timer = true; }));

    REQUIRE(reactor->run_once(0ms).has_value());
    CHECK(new_timer);
    CHECK_FALSE(old_timer);
}

TEST_CASE("run_once returns when the timer fires, not when its timeout runs out", "[iocp][timer]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);

    bool fired = false;
    const auto start = Clock::now();
    REQUIRE(reactor->set_timer(30ms, [&] { fired = true; }));
    REQUIRE(reactor->run_once(10s).has_value());

    CHECK(fired);
    CHECK(Clock::now() - start >= 30ms);
    CHECK(Clock::now() - start < 5s);
}

TEST_CASE("a delay too long for the clock is kept instead of overflowing", "[iocp][timer]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);

    bool fired = false;
    REQUIRE(reactor->set_timer(std::chrono::milliseconds::max(), [&] { fired = true; }));
    REQUIRE(reactor->run_once(20ms).has_value());
    CHECK_FALSE(fired);
    reactor->cancel_timer();
}

TEST_CASE("a timer whose callback calls stop() ends run() no earlier than its delay",
          "[iocp][timer]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);

    const auto start = Clock::now();
    REQUIRE(reactor->set_timer(40ms, [&] { reactor->stop(); }));
    REQUIRE(reactor->run().has_value());
    CHECK(Clock::now() - start >= 40ms);
}

TEST_CASE("run_once with the longest and the shortest timeouts", "[iocp][run]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);

    // The shortest is negative, which means don't wait.
    const auto start = Clock::now();
    REQUIRE(reactor->run_once(std::chrono::milliseconds::min()).has_value());
    CHECK(Clock::now() - start < 1s);

    // The longest is capped at what Windows accepts, and a wake still ends it.
    bool woken = false;
    reactor->set_wake_callback([&] { woken = true; });
    std::thread other([&] {
        std::this_thread::sleep_for(30ms);
        reactor->wake();
    });
    REQUIRE(reactor->run_once(std::chrono::milliseconds::max()).has_value());
    other.join();
    CHECK(woken);
}

TEST_CASE("every callback runs on the thread that calls run_once", "[iocp][threads]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);
    PendingRead pending;
    PipePair pipe;
    connect_pair(pipe);

    std::thread::id io_thread;
    std::thread::id wake_thread;
    std::thread::id timer_thread;
    bool io_done = false;
    bool wake_done = false;
    bool timer_done = false;
    watch_or_fail(*reactor, pipe.server, [&](const IoEvent&) {
        io_thread = std::this_thread::get_id();
        io_done = true;
    });
    reactor->set_wake_callback([&] {
        wake_thread = std::this_thread::get_id();
        wake_done = true;
    });
    REQUIRE(reactor->set_timer(5ms, [&] {
        timer_thread = std::this_thread::get_id();
        timer_done = true;
    }));

    start_read(pipe.server, pending, 256);
    send_message(pipe.client, 3);
    std::thread other([&] { reactor->wake(); });

    REQUIRE(run_until(*reactor, [&] { return io_done && wake_done && timer_done; }));
    other.join();

    const std::thread::id me = std::this_thread::get_id();
    CHECK(io_thread == me);
    CHECK(wake_thread == me);
    CHECK(timer_thread == me);
}

// The cases below use a fixed seed, so a failure reproduces exactly.

TEST_CASE("every read is reported once, with the length that was sent", "[iocp][random]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);
    std::mt19937 rng{20240607};

    for (int round = 0; round < 40; ++round) {
        const std::size_t count = 1 + rng() % 8;
        std::vector<PendingRead> reads(count);
        std::vector<std::size_t> lengths(count);
        PipePair pipe;
        connect_pair(pipe);
        std::vector<IoEvent> events;
        watch_or_fail(*reactor, pipe.server, [&](const IoEvent& e) { events.push_back(e); });

        for (PendingRead& pending : reads) {
            start_read(pipe.server, pending, 256);
        }
        for (std::size_t& length : lengths) {
            length = 1 + rng() % 200;
            send_message(pipe.client, length);
        }

        REQUIRE(run_until(*reactor, [&] { return events.size() >= count; }));
        CHECK(events.size() == count);
        for (const IoEvent& e : events) {
            std::size_t index = 0;
            while (index < count && e.overlapped != &reads[index].ov) {
                ++index;
            }
            REQUIRE(index < count);
            CHECK(e.bytes == lengths[index]);
            CHECK(e.error == 0u);
        }
        REQUIRE(reactor->unwatch(native(pipe.server)).has_value());
    }
}

TEST_CASE("a timer never fires before its delay", "[iocp][random]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);
    std::mt19937 rng{20240608};

    for (int i = 0; i < 25; ++i) {
        const std::chrono::milliseconds delay{static_cast<int>(rng() % 40)};
        bool fired = false;
        const auto start = Clock::now();
        REQUIRE(reactor->set_timer(delay, [&] { fired = true; }));
        REQUIRE(run_until(*reactor, [&] { return fired; }));
        CHECK(Clock::now() - start >= delay);
    }
}

TEST_CASE("watching, unwatching and doing I/O in any order is reported correctly",
          "[iocp][random]") {
    constexpr std::size_t kPairs = 3;
    auto reactor = Reactor::create();
    REQUIRE(reactor);
    std::array<PendingRead, kPairs> reads;
    std::array<PipePair, kPairs> pipes;
    std::array<bool, kPairs> watched{};
    std::array<int, kPairs> calls{};
    for (PipePair& pipe : pipes) {
        connect_pair(pipe);
    }

    std::mt19937 rng{20240609};
    for (int step = 0; step < 300; ++step) {
        const std::size_t i = rng() % kPairs;
        if (rng() % 3 == 0) {
            if (watched[i]) {
                REQUIRE(reactor->unwatch(native(pipes[i].server)).has_value());
                watched[i] = false;
            } else {
                watch_or_fail(*reactor, pipes[i].server,
                              [&calls, i](const IoEvent&) { ++calls[i]; });
                watched[i] = true;
            }
        } else {
            const int before = calls[i];
            start_read(pipes[i].server, reads[i], 256);
            send_message(pipes[i].client, 1 + rng() % 100);
            wait_for_completion(reads[i]); // the read is done, whoever hears about it
            if (watched[i]) {
                REQUIRE(run_until(*reactor, [&] { return calls[i] > before; }));
                CHECK(calls[i] == before + 1);
            } else {
                REQUIRE(reactor->run_once(5ms).has_value());
                CHECK(calls[i] == before);
            }
        }
    }
}

// --- post(): events the OS doesn't report ----------------------------------

TEST_CASE("a posted event reaches the watch callback from the loop, not from post()",
          "[iocp][post]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);
    OVERLAPPED marker{};
    PipePair pipe;
    connect_pair(pipe);

    Seen seen;
    watch_or_fail(*reactor, pipe.server, recorder(seen));

    IoEvent event;
    event.overlapped = &marker;
    event.bytes = 7;
    event.error = kErrorPipeConnected;
    REQUIRE(reactor->post(native(pipe.server), event).has_value());
    CHECK(seen.count == 0); // not inside post()

    REQUIRE(reactor->run_once(0ms).has_value());
    CHECK(seen.count == 1);
    CHECK(seen.last.overlapped == &marker);
    CHECK(seen.last.bytes == 7u);
    CHECK(seen.last.error == kErrorPipeConnected);
}

TEST_CASE("posted events keep their order and run_once doesn't wait for them", "[iocp][post]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);
    PipePair pipe;
    connect_pair(pipe);

    std::vector<std::uint32_t> order;
    watch_or_fail(*reactor, pipe.server, [&](const IoEvent& e) { order.push_back(e.bytes); });
    for (std::uint32_t i = 1; i <= 3; ++i) {
        IoEvent event;
        event.bytes = i;
        REQUIRE(reactor->post(native(pipe.server), event).has_value());
    }

    const auto start = Clock::now();
    REQUIRE(reactor->run_once(10s).has_value());
    CHECK(Clock::now() - start < 5s);
    CHECK(order == std::vector<std::uint32_t>{1u, 2u, 3u});
}

TEST_CASE("an event posted from a callback waits for the next pass", "[iocp][post]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);
    PipePair pipe;
    connect_pair(pipe);

    int calls = 0;
    watch_or_fail(*reactor, pipe.server, [&](const IoEvent& e) {
        ++calls;
        if (e.bytes == 1) {
            IoEvent next;
            next.bytes = 2;
            CHECK(reactor->post(native(pipe.server), next).has_value());
        }
    });
    IoEvent first;
    first.bytes = 1;
    REQUIRE(reactor->post(native(pipe.server), first).has_value());

    REQUIRE(reactor->run_once(0ms).has_value());
    CHECK(calls == 1);
    REQUIRE(reactor->run_once(0ms).has_value());
    CHECK(calls == 2);
}

TEST_CASE("post fails for a handle that isn't watched", "[iocp][post]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);
    PipePair pipe;
    connect_pair(pipe);

    auto result = reactor->post(native(pipe.server), IoEvent{});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == std::errc::no_such_file_or_directory);
}

TEST_CASE("a posted event for a handle unwatched before delivery is dropped", "[iocp][post]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);
    PipePair pipe;
    connect_pair(pipe);

    int calls = 0;
    watch_or_fail(*reactor, pipe.server, [&](const IoEvent&) { ++calls; });
    REQUIRE(reactor->post(native(pipe.server), IoEvent{}).has_value());
    REQUIRE(reactor->unwatch(native(pipe.server)).has_value());

    REQUIRE(reactor->run_once(50ms).has_value());
    CHECK(calls == 0);
}

TEST_CASE("a posted event isn't delivered to a watch that reuses the handle number",
          "[iocp][post]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);
    auto old_pipe = std::make_unique<PipePair>();
    connect_pair(*old_pipe);

    int old_calls = 0;
    int new_calls = 0;
    watch_or_fail(*reactor, old_pipe->server, [&](const IoEvent&) { ++old_calls; });
    REQUIRE(reactor->post(native(old_pipe->server), IoEvent{}).has_value());
    REQUIRE(reactor->unwatch(native(old_pipe->server)).has_value());
    old_pipe.reset(); // closes the handle, whose number Windows tends to hand out again

    PipePair replacement;
    connect_pair(replacement);
    watch_or_fail(*reactor, replacement.server, [&](const IoEvent&) { ++new_calls; });

    REQUIRE(reactor->run_once(50ms).has_value());
    CHECK(old_calls == 0);
    CHECK(new_calls == 0);
}

// The case post() exists for. When the client gets to a pipe before the server
// calls ConnectNamedPipe, the call reports ERROR_PIPE_CONNECTED and Windows
// queues nothing, so the reactor would never hear that the pipe is connected.
TEST_CASE("ERROR_PIPE_CONNECTED queues no packet, and post() reports the connect instead",
          "[iocp][post]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);
    PipePair pipe;
    open_pair(pipe);

    Seen seen;
    watch_or_fail(*reactor, pipe.server, recorder(seen));

    OVERLAPPED connect_ov{};
    REQUIRE(::ConnectNamedPipe(pipe.server, &connect_ov) == 0);
    REQUIRE(::GetLastError() == kPipeConnected);

    REQUIRE(reactor->run_once(100ms).has_value());
    CHECK(seen.count == 0); // Windows said nothing

    IoEvent event;
    event.overlapped = &connect_ov;
    REQUIRE(reactor->post(native(pipe.server), event).has_value());
    REQUIRE(run_until(*reactor, [&] { return seen.count > 0; }));
    CHECK(seen.count == 1);
    CHECK(seen.last.overlapped == &connect_ov);
    CHECK(seen.last.error == 0u);
}