// Tests for the Linux ShutdownSignal (src/pal/linux/signalfd_shutdown.cpp).
//
// The signals are sent with raise(), which sends them to this thread. While a
// ShutdownSignal exists they're blocked, so they wait in the signalfd until the
// reactor reads them instead of ending the test. Every test reads all it sent
// before the ShutdownSignal goes away: one still pending then would end the
// test program, the default reaction coming back with the old signal mask.
//
// test_daemon.cpp checks the same from outside, with `kill` on the daemon.

#include "reactor.hpp"
#include "shutdown_signal.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <csignal>
#include <utility>

#include <pthread.h>

using am::pal::Reactor;
using am::pal::ShutdownSignal;
using namespace std::chrono_literals;

namespace {

// Whether `signal` is blocked on this thread right now.
bool blocked(int signal) {
    sigset_t current;
    sigemptyset(&current);
    pthread_sigmask(SIG_BLOCK, nullptr, &current);
    return sigismember(&current, signal) == 1;
}

// Moves `from` into `to`, through references so the compiler doesn't see a
// self-move when the test means one.
void move_into(ShutdownSignal& to, ShutdownSignal& from) { to = std::move(from); }

} // namespace

TEST_CASE("SIGINT and SIGTERM reach the callback on the reactor thread", "[shutdown]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);
    int calls = 0;
    {
        auto shutdown = ShutdownSignal::watch(*reactor, [&calls] { ++calls; });
        REQUIRE(shutdown);
        CHECK(blocked(SIGINT));
        CHECK(blocked(SIGTERM));

        REQUIRE(raise(SIGTERM) == 0);
        REQUIRE(reactor->run_once(1s));
        CHECK(calls == 1);

        // Two at once are one call: the daemon only needs to hear "quit" once
        // per wakeup.
        REQUIRE(raise(SIGINT) == 0);
        REQUIRE(raise(SIGTERM) == 0);
        REQUIRE(reactor->run_once(1s));
        CHECK(calls == 2);

        // Nothing left over.
        REQUIRE(reactor->run_once(10ms));
        CHECK(calls == 2);
    }
    // The old mask is back once it's gone.
    CHECK_FALSE(blocked(SIGINT));
    CHECK_FALSE(blocked(SIGTERM));
}

TEST_CASE("reclaim has nothing to do on Linux", "[shutdown]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);
    auto shutdown = ShutdownSignal::watch(*reactor, [] {});
    REQUIRE(shutdown);
    CHECK(shutdown->reclaim());
    CHECK(blocked(SIGTERM));
}

TEST_CASE("a moved ShutdownSignal keeps watching, and the moved-from one does nothing",
          "[shutdown]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);
    int calls = 0;
    {
        auto first = ShutdownSignal::watch(*reactor, [&calls] { ++calls; });
        REQUIRE(first);

        ShutdownSignal second(std::move(*first));
        REQUIRE(raise(SIGTERM) == 0);
        REQUIRE(reactor->run_once(1s));
        CHECK(calls == 1);

        // Back into the moved-from one, then onto itself, which changes nothing.
        move_into(*first, second);
        move_into(*first, *first);
        REQUIRE(raise(SIGTERM) == 0);
        REQUIRE(reactor->run_once(1s));
        CHECK(calls == 2);
        CHECK(blocked(SIGTERM));
        // `second` is moved from now; destroying it first must leave the
        // signals with `first`.
    }
    CHECK_FALSE(blocked(SIGTERM));
}

TEST_CASE("a callback that's empty is never called", "[shutdown]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);
    auto shutdown = ShutdownSignal::watch(*reactor, ShutdownSignal::Callback{});
    REQUIRE(shutdown);
    REQUIRE(raise(SIGTERM) == 0);
    REQUIRE(reactor->run_once(1s)); // reads the signal and doesn't crash
    REQUIRE(reactor->run_once(10ms));
}
