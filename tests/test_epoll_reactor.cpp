// Linux-only tests for watching file descriptors with the epoll reactor
// (src/pal/linux/epoll_reactor.cpp). They use a pipe as the watched fd: the
// IPC socket from #15 will behave the same way.

#include "reactor.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <memory>
#include <system_error>

namespace {

using am::pal::Interest;
using am::pal::IoEvent;
using am::pal::NativeHandle;
using am::pal::Reactor;
using namespace std::chrono_literals;

// A non-blocking pipe that closes itself.
struct Pipe {
    int read_fd = -1;
    int write_fd = -1;

    Pipe() {
        std::array<int, 2> fds{};
        REQUIRE(::pipe2(fds.data(), O_NONBLOCK | O_CLOEXEC) == 0);
        read_fd = fds[0];
        write_fd = fds[1];
    }
    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;
    ~Pipe() {
        close_read();
        close_write();
    }

    void write_byte() const {
        const char byte = 'x';
        REQUIRE(::write(write_fd, &byte, 1) == 1);
    }
    void close_read() {
        if (read_fd >= 0) {
            ::close(read_fd);
            read_fd = -1;
        }
    }
    void close_write() {
        if (write_fd >= 0) {
            ::close(write_fd);
            write_fd = -1;
        }
    }
};

void drain(int fd) {
    std::array<char, 64> buf{};
    while (::read(fd, buf.data(), buf.size()) > 0) {
    }
}

} // namespace

TEST_CASE("a watched fd reports readable once data arrives", "[reactor][epoll]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);
    Pipe pipe;

    int calls = 0;
    IoEvent seen;
    REQUIRE(reactor->watch(NativeHandle{pipe.read_fd}, Interest{.readable = true},
                           [&](const IoEvent& event) {
                               ++calls;
                               seen = event;
                               drain(pipe.read_fd);
                           }));

    // Nothing written yet, so nothing to report.
    REQUIRE(reactor->run_once(10ms));
    CHECK(calls == 0);

    pipe.write_byte();
    REQUIRE(reactor->run_once(100ms));
    CHECK(calls == 1);
    CHECK(seen.readable);
    CHECK_FALSE(seen.writable);
    CHECK(seen.overlapped == nullptr); // the IOCP half stays empty on Linux

    REQUIRE(reactor->unwatch(NativeHandle{pipe.read_fd}));
}

TEST_CASE("level-triggered: undrained data is reported again", "[reactor][epoll]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);
    Pipe pipe;

    int calls = 0;
    REQUIRE(reactor->watch(NativeHandle{pipe.read_fd}, Interest{.readable = true},
                           [&](const IoEvent&) { ++calls; })); // never reads
    pipe.write_byte();

    REQUIRE(reactor->run_once(100ms));
    REQUIRE(reactor->run_once(100ms));
    CHECK(calls == 2);
}

TEST_CASE("modify switches what a watched fd reports", "[reactor][epoll]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);
    Pipe pipe;

    // An empty pipe's write end is always writable.
    int writable_calls = 0;
    REQUIRE(reactor->watch(NativeHandle{pipe.write_fd}, Interest{.writable = true},
                           [&](const IoEvent& event) {
                               if (event.writable) {
                                   ++writable_calls;
                               }
                           }));
    REQUIRE(reactor->run_once(100ms));
    CHECK(writable_calls == 1);

    // With no interest left, it goes quiet.
    REQUIRE(reactor->modify(NativeHandle{pipe.write_fd}, Interest{}));
    REQUIRE(reactor->run_once(10ms));
    CHECK(writable_calls == 1);
}

TEST_CASE("a callback can unwatch its own fd", "[reactor][epoll]") {
    // Run under linux-asan: a reactor that destroys the running callback
    // shows up here as a use-after-free.
    auto reactor = Reactor::create();
    REQUIRE(reactor);
    Pipe pipe;

    int calls = 0;
    REQUIRE(
        reactor->watch(NativeHandle{pipe.read_fd}, Interest{.readable = true}, [&](const IoEvent&) {
            ++calls;
            REQUIRE(reactor->unwatch(NativeHandle{pipe.read_fd}));
        }));
    pipe.write_byte();

    REQUIRE(reactor->run_once(100ms));
    REQUIRE(reactor->run_once(10ms)); // data is still there, but nobody's watching
    CHECK(calls == 1);
}

TEST_CASE("a callback can unwatch another fd from the same batch", "[reactor][epoll]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);
    Pipe a;
    Pipe b;

    // Both become readable at once, so both land in one epoll_wait. Whichever
    // runs first unwatches the other, which must then not run.
    int calls = 0;
    REQUIRE(
        reactor->watch(NativeHandle{a.read_fd}, Interest{.readable = true}, [&](const IoEvent&) {
            ++calls;
            REQUIRE(reactor->unwatch(NativeHandle{b.read_fd}));
            REQUIRE(reactor->unwatch(NativeHandle{a.read_fd}));
        }));
    REQUIRE(
        reactor->watch(NativeHandle{b.read_fd}, Interest{.readable = true}, [&](const IoEvent&) {
            ++calls;
            REQUIRE(reactor->unwatch(NativeHandle{a.read_fd}));
            REQUIRE(reactor->unwatch(NativeHandle{b.read_fd}));
        }));
    a.write_byte();
    b.write_byte();

    REQUIRE(reactor->run_once(100ms));
    CHECK(calls == 1);
}

TEST_CASE("a stale event for a reused fd number is not delivered", "[reactor][epoll]") {
    // Two pipes become readable in the same batch. Whichever callback runs
    // first closes the other pipe and opens a new one, which the kernel gives
    // the lowest free fd: the number just closed. The batch still holds an
    // event for the old fd with that number. It must not reach the new
    // watcher, whose pipe is empty.
    auto reactor = Reactor::create();
    REQUIRE(reactor);
    auto a = std::make_unique<Pipe>();
    auto b = std::make_unique<Pipe>();
    std::unique_ptr<Pipe> replacement;
    int replacement_calls = 0;
    bool swapped = false;

    auto replace = [&](std::unique_ptr<Pipe>& victim) {
        if (swapped) {
            return;
        }
        swapped = true;
        const int old_fd = victim->read_fd;
        REQUIRE(reactor->unwatch(NativeHandle{old_fd}));
        victim.reset();
        replacement = std::make_unique<Pipe>();
        CHECK(replacement->read_fd == old_fd); // same number, new pipe
        REQUIRE(reactor->watch(NativeHandle{replacement->read_fd}, Interest{.readable = true},
                               [&](const IoEvent&) { ++replacement_calls; }));
    };

    REQUIRE(reactor->watch(NativeHandle{a->read_fd}, Interest{.readable = true},
                           [&](const IoEvent&) { replace(b); }));
    REQUIRE(reactor->watch(NativeHandle{b->read_fd}, Interest{.readable = true},
                           [&](const IoEvent&) { replace(a); }));
    a->write_byte();
    b->write_byte();

    REQUIRE(reactor->run_once(100ms));
    CHECK(swapped);
    CHECK(replacement_calls == 0);
}

TEST_CASE("closing the other end reports hangup", "[reactor][epoll]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);
    Pipe pipe;

    IoEvent seen;
    REQUIRE(reactor->watch(NativeHandle{pipe.read_fd}, Interest{.readable = true},
                           [&](const IoEvent& event) { seen = event; }));
    pipe.close_write();

    REQUIRE(reactor->run_once(100ms));
    CHECK(seen.hangup);
    REQUIRE(reactor->unwatch(NativeHandle{pipe.read_fd}));
}

TEST_CASE("watching an fd twice or unwatching an unknown fd fails", "[reactor][epoll]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);
    Pipe pipe;

    const NativeHandle fd{pipe.read_fd};
    REQUIRE(reactor->watch(fd, Interest{.readable = true}, [](const IoEvent&) {}));

    const auto twice = reactor->watch(fd, Interest{.readable = true}, [](const IoEvent&) {});
    REQUIRE_FALSE(twice);
    CHECK(twice.error() == std::errc::file_exists);

    REQUIRE(reactor->unwatch(fd));
    const auto again = reactor->unwatch(fd);
    REQUIRE_FALSE(again);
    CHECK(again.error() == std::errc::no_such_file_or_directory);
}
