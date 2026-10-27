// Tests for am-ui's connection to the daemon (src/client/daemon_link.hpp) and
// its telemetry reader (src/client/ui_state.hpp), issue #36.
//
// Starts the real daemon, as test_daemon.cpp does, and drives it the way the
// window will: through DaemonLink, which runs its own thread. Two threads
// share the link's state, so this file is also what linux-tsan checks for
// data races in it.
//
// The last test plays a broken daemon with a bare IpcListener, to check the
// link gives up on one that never answers or sends garbage.
//
// Linux-only for now, like test_daemon.cpp: it starts and kills the daemon
// with posix_spawn and kill (daemon_process.hpp).

#include "client/daemon_link.hpp"
#include "client/ui_state.hpp"
#include "daemon_process.hpp"
#include "common/ipc_protocol.hpp"
#include "ipc_channel.hpp"
#include "reactor.hpp"

#include <catch2/catch_test_macros.hpp>

#include <signal.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace am::client;
using am::common::PlaybackState;
using am::pal::IoStatus;
using am::pal::IpcChannel;
using am::pal::IpcListener;
using am::pal::Reactor;
using am::test::Daemon;
using am::test::RuntimeDir;
using am::test::unique_name;

// Stands in for the window's event loop: the link's wake callback pokes it,
// and the test sleeps on it the way the window sleeps in SDL_WaitEvent.
struct Window {
    std::mutex mutex;
    std::condition_variable woken;
    int wakes = 0;

    DaemonLink::WakeCallback callback() {
        return [this] {
            {
                std::lock_guard lock(mutex);
                ++wakes;
            }
            woken.notify_all();
        };
    }

    // Waits until `done` is true for the link's state, checking after each
    // wake. Also checks every 50 ms, so a missed wake shows up as a failed
    // CHECK on `wakes` rather than a hang.
    bool wait_for(const DaemonLink& link, const std::function<bool(const DaemonState&)>& done,
                  std::chrono::milliseconds timeout = 5s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::unique_lock lock(mutex);
        for (;;) {
            lock.unlock();
            const bool ok = done(link.state());
            lock.lock();
            if (ok) {
                return true;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            woken.wait_for(lock, 50ms);
        }
    }
};

DaemonLinkOptions options_for(const std::string& name) {
    // Faster retries than the window's 1 s, so the test doesn't crawl.
    return {.name = name, .retry_interval = 100ms, .reply_timeout = 2s};
}

bool connected(const DaemonState& s) { return s.status == LinkStatus::Connected && s.has_playback; }

// Waits for the answer to command `id` and returns how it ended.
std::optional<CommandOutcome> outcome_of(Window& window, const DaemonLink& link, RequestId id) {
    std::optional<CommandOutcome> outcome;
    window.wait_for(link, [&](const DaemonState& s) {
        if (s.last_result && s.last_result->id == id) {
            outcome = s.last_result->outcome;
            return true;
        }
        return false;
    });
    return outcome;
}

// Calls update() once per "frame" until the snapshot passes `done`.
bool telemetry_shows(TelemetryReader& reader, const DaemonLink& link,
                     const std::function<bool(const am::common::TelemetrySnapshot&)>& done) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (done(reader.update(link.state())) && reader.fresh()) {
            return true;
        }
        std::this_thread::sleep_for(16ms); // about one frame
    }
    return false;
}

} // namespace

TEST_CASE("the window's link connects, sends commands and follows the state", "[daemon_link]") {
    RuntimeDir dir;
    const std::string name = unique_name() + "-link";
    Daemon daemon(name);

    Window window;
    auto link = DaemonLink::start(options_for(name), window.callback());
    REQUIRE(link.has_value());
    DaemonLink& l = **link;

    // Hello, then the daemon's current state: playing at full volume.
    REQUIRE(window.wait_for(l, connected, 10s));
    DaemonState s = l.state();
    CHECK(s.connection_count == 1);
    CHECK(s.daemon_name == am::common::kDaemonName);
    CHECK(s.playback == PlaybackState::Playing);
    CHECK(s.volume_percent == 100);
    CHECK(window.wakes > 0);

    TelemetryReader reader(name);
    CHECK(telemetry_shows(
        reader, l, [](const auto& t) { return t.playback_state == PlaybackState::Playing; }));

    // Pause: an Ack for that request, then the new state arrives by itself.
    CHECK(outcome_of(window, l, l.pause()) == CommandOutcome::Ok);
    CHECK(window.wait_for(
        l, [](const DaemonState& st) { return st.playback == PlaybackState::Paused; }));
    CHECK(telemetry_shows(reader, l,
                          [](const auto& t) { return t.playback_state == PlaybackState::Paused; }));

    CHECK(outcome_of(window, l, l.set_volume(30)) == CommandOutcome::Ok);
    CHECK(window.wait_for(l, [](const DaemonState& st) { return st.volume_percent == 30; }));
    CHECK(telemetry_shows(reader, l, [](const auto& t) { return t.volume_percent == 30; }));

    // The daemon says no, and the link passes that on.
    CHECK(outcome_of(window, l, l.set_volume(101)) == CommandOutcome::BadRequest);

    // Toggle from paused plays.
    CHECK(outcome_of(window, l, l.toggle()) == CommandOutcome::Ok);
    CHECK(window.wait_for(
        l, [](const DaemonState& st) { return st.playback == PlaybackState::Playing; }));

    CHECK(outcome_of(window, l, l.stop()) == CommandOutcome::Ok);
    CHECK(window.wait_for(
        l, [](const DaemonState& st) { return st.playback == PlaybackState::Stopped; }));
    CHECK(outcome_of(window, l, l.play()) == CommandOutcome::Ok);
}

TEST_CASE("the window's link says when the daemon isn't running and reconnects", "[daemon_link]") {
    RuntimeDir dir;
    const std::string name = unique_name() + "-link-restart";

    Window window;
    auto link = DaemonLink::start(options_for(name), window.callback());
    REQUIRE(link.has_value());
    DaemonLink& l = **link;
    TelemetryReader reader(name);

    // No daemon yet. Commands fail at once instead of waiting for one.
    std::this_thread::sleep_for(300ms); // a few retries
    CHECK(l.state().status == LinkStatus::NotRunning);
    CHECK(l.state().connection_count == 0);
    CHECK(outcome_of(window, l, l.play()) == CommandOutcome::NotConnected);
    reader.update(l.state());
    CHECK_FALSE(reader.mapped());
    CHECK_FALSE(reader.fresh());

    std::optional<Daemon> daemon(std::in_place, name);
    REQUIRE(window.wait_for(l, connected, 10s));
    CHECK(l.state().connection_count == 1);
    CHECK(telemetry_shows(reader, l, [](const auto&) { return true; }));
    CHECK(outcome_of(window, l, l.pause()) == CommandOutcome::Ok);
    CHECK(window.wait_for(
        l, [](const DaemonState& st) { return st.playback == PlaybackState::Paused; }));

    // Kill it the hard way, as a crash would. The link notices the hang-up
    // and the reader lets go of the dead daemon's page.
    daemon.reset(); // SIGKILL and wait
    REQUIRE(window.wait_for(
        l, [](const DaemonState& st) { return st.status == LinkStatus::NotRunning; }));
    {
        const DaemonState st = l.state();
        CHECK(st.has_playback); // the last state stays, for the window to grey out
        CHECK(st.playback == PlaybackState::Paused);
    }
    reader.update(l.state());
    CHECK_FALSE(reader.mapped());
    CHECK_FALSE(reader.fresh());

    // A new daemon starts playing again. The link finds it by itself, and the
    // reader maps the new daemon's page, not the old one.
    daemon.emplace(name);
    REQUIRE(window.wait_for(
        l, [](const DaemonState& st) { return connected(st) && st.connection_count == 2; }, 10s));
    CHECK(window.wait_for(
        l, [](const DaemonState& st) { return st.playback == PlaybackState::Playing; }));
    CHECK(telemetry_shows(
        reader, l, [](const auto& t) { return t.playback_state == PlaybackState::Playing; }));
    CHECK(reader.mapped());
}

namespace {

// Listens like the daemon but doesn't behave like it: either says nothing at
// all, or answers the hello with 32 bytes of junk. Runs its own reactor
// thread. Made on the heap so the callbacks' `this` stays put.
struct BrokenDaemon {
    enum class Mode { Silent, Garbage };

    Reactor reactor;
    Mode mode;
    std::optional<IpcListener> listener;
    std::vector<std::unique_ptr<IpcChannel>> clients;
    std::vector<std::byte> buffer = std::vector<std::byte>(am::common::kMaxFrameBytes);
    std::thread thread;

    BrokenDaemon(Reactor r, Mode m) : reactor(std::move(r)), mode(m) {}

    static std::unique_ptr<BrokenDaemon> start(const std::string& name, Mode mode) {
        auto reactor = Reactor::create();
        REQUIRE(reactor.has_value());
        auto d = std::make_unique<BrokenDaemon>(std::move(*reactor), mode);
        auto listener = IpcListener::listen(d->reactor, name);
        REQUIRE(listener.has_value());
        d->listener.emplace(std::move(*listener));
        BrokenDaemon* self = d.get();
        d->listener->set_accept_callback([self] { self->accept_all(); });
        d->thread = std::thread([self] { (void)self->reactor.run(); });
        return d;
    }

    ~BrokenDaemon() {
        reactor.stop();
        thread.join();
    }

    void accept_all() {
        for (;;) {
            auto client = listener->accept();
            if (!client || !*client) {
                return;
            }
            auto channel = std::make_unique<IpcChannel>(std::move(**client));
            IpcChannel* raw = channel.get();
            raw->set_readable_callback([this, raw] { read(raw); });
            clients.push_back(std::move(channel));
        }
    }

    void read(IpcChannel* channel) {
        for (;;) {
            auto r = channel->receive(buffer);
            if (!r || r->status == IoStatus::PeerClosed) {
                std::erase_if(clients, [&](const auto& c) { return c.get() == channel; });
                return; // the channel is gone now; touch nothing of it
            }
            if (r->status == IoStatus::WouldBlock) {
                return;
            }
            if (mode == Mode::Garbage) {
                std::vector<std::byte> junk(am::common::kFrameHeaderBytes, std::byte{0xAB});
                (void)channel->send(junk);
            }
        }
    }
};

} // namespace

TEST_CASE("the window's link gives up on a daemon that misbehaves", "[daemon_link]") {
    RuntimeDir dir;
    const std::string name = unique_name() + "-link-broken";
    Window window;
    DaemonLinkOptions options = options_for(name);
    options.reply_timeout = 300ms;

    SECTION("no answer to the hello") {
        auto daemon = BrokenDaemon::start(name, BrokenDaemon::Mode::Silent);
        auto link = DaemonLink::start(options, window.callback());
        REQUIRE(link.has_value());
        CHECK(window.wait_for(**link, [](const DaemonState& s) {
            return s.status == LinkStatus::Failed && s.error == std::errc::timed_out;
        }));
        CHECK((*link)->state().connection_count == 0);
    }

    SECTION("a broken frame") {
        auto daemon = BrokenDaemon::start(name, BrokenDaemon::Mode::Garbage);
        auto link = DaemonLink::start(options, window.callback());
        REQUIRE(link.has_value());
        CHECK(window.wait_for(**link, [](const DaemonState& s) {
            return s.status == LinkStatus::Failed && s.error == std::errc::bad_message;
        }));
        CHECK((*link)->state().connection_count == 0);
    }
}
