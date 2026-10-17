// Tests for the parts of am::pal::Reactor that behave the same on every
// platform: the run loop, wake(), stop() and the timer. Watching handles is
// platform-specific, so those tests live in test_epoll_reactor.cpp (and later
// a Windows equivalent).
//
// Run under linux-tsan: wake() and stop() are the only calls allowed from
// another thread, and these tests make them from one.

#include "reactor.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

namespace {

using am::pal::Reactor;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

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

} // namespace

TEST_CASE("create() gives a working reactor", "[reactor]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor.has_value());
}

TEST_CASE("run_once with nothing to do waits out its timeout", "[reactor]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);

    const auto start = Clock::now();
    REQUIRE(reactor->run_once(20ms));
    CHECK(Clock::now() - start >= 15ms);
}

TEST_CASE("the timer fires once, after its delay", "[reactor][timer]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);

    int fired = 0;
    const auto start = Clock::now();
    REQUIRE(reactor->set_timer(30ms, [&] { ++fired; }));

    REQUIRE(run_until(*reactor, [&] { return fired > 0; }));
    CHECK(Clock::now() - start >= 30ms);

    // One-shot: nothing more after it has fired.
    REQUIRE(reactor->run_once(60ms));
    CHECK(fired == 1);
}

TEST_CASE("a zero delay still fires the timer", "[reactor][timer]") {
    // On Linux an all-zero timerfd setting disarms the timer, so this checks
    // the reactor doesn't pass zero straight through.
    auto reactor = Reactor::create();
    REQUIRE(reactor);

    bool fired = false;
    REQUIRE(reactor->set_timer(0ms, [&] { fired = true; }));
    CHECK(run_until(*reactor, [&] { return fired; }));
}

TEST_CASE("cancel_timer stops a pending timer", "[reactor][timer]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);

    bool fired = false;
    REQUIRE(reactor->set_timer(10ms, [&] { fired = true; }));
    reactor->cancel_timer();

    REQUIRE(reactor->run_once(50ms));
    CHECK_FALSE(fired);
}

TEST_CASE("setting the timer again replaces the old one", "[reactor][timer]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);

    bool first = false;
    bool second = false;
    REQUIRE(reactor->set_timer(10ms, [&] { first = true; }));
    REQUIRE(reactor->set_timer(20ms, [&] { second = true; }));

    REQUIRE(run_until(*reactor, [&] { return second; }));
    CHECK_FALSE(first);
}

TEST_CASE("the timer can re-arm itself from its own callback", "[reactor][timer]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);

    int ticks = 0;
    std::move_only_function<void()> tick;
    tick = [&] {
        if (++ticks < 3) {
            REQUIRE(reactor->set_timer(5ms, [&] { tick(); }));
        }
    };
    REQUIRE(reactor->set_timer(5ms, [&] { tick(); }));

    CHECK(run_until(*reactor, [&] { return ticks == 3; }));
}

TEST_CASE("wake() from another thread runs the wake callback", "[reactor][wake]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);

    // A plain int, not an atomic: the reactor promises that what the other
    // thread wrote before wake() is visible in the wake callback. Under
    // linux-tsan this is a data race unless wake() really provides that.
    int handed_over = 0;
    bool woken = false;
    reactor->set_wake_callback([&] {
        CHECK(handed_over == 42);
        woken = true;
        reactor->stop();
    });

    std::thread other([&] {
        std::this_thread::sleep_for(10ms);
        handed_over = 42;
        reactor->wake();
    });

    REQUIRE(reactor->run());
    other.join();
    CHECK(woken);
}

TEST_CASE("many wake() calls before the loop runs can merge into one", "[reactor][wake]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);

    int callbacks = 0;
    reactor->set_wake_callback([&] { ++callbacks; });
    for (int i = 0; i < 10; ++i) {
        reactor->wake();
    }

    REQUIRE(reactor->run_once(0ms));
    CHECK(callbacks >= 1);
    CHECK(callbacks <= 10);
}

TEST_CASE("stop() from another thread ends run()", "[reactor]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);

    std::thread other([&] {
        std::this_thread::sleep_for(20ms);
        reactor->stop();
    });

    const auto start = Clock::now();
    REQUIRE(reactor->run());
    other.join();
    CHECK(Clock::now() - start >= 15ms);
}

TEST_CASE("stop() before run() makes run() return right away", "[reactor]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor);

    reactor->stop();
    CHECK(reactor->run());
}
