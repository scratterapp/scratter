// Tests for MediaControls (src/pal/interfaces/media_controls.hpp).
//
// These are the contract both platforms have to meet: Linux's MPRIS version
// (#56) and Windows' SMTC version (#57). They can only check the daemon's side:
// that open() and the setters behave, and that nothing arrives at the callback
// unasked. Pressing a media key needs something outside the process (playerctl
// on Linux), so #56 and #57 add their own tests for that direction.
//
// Each test registers under a throwaway name, "<product>_test_<test>", so it
// never clashes with a daemon that's running on the same machine.
//
// Until a platform's version is written, open() says "not supported" and the
// tests stop there. The same happens on a machine with no media control
// service, like a CI runner with no D-Bus session bus.

#include "media_controls.hpp"
#include "reactor.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

using namespace std::chrono_literals;
using am::common::PlaybackState;
using am::pal::MediaControls;
using am::pal::MediaRequest;
using am::pal::Reactor;
using am::pal::TrackInfo;

namespace {

std::string test_name(std::string_view test) {
    return std::string(am::common::kAppName) + "_test_" + std::string(test);
}

// A reactor, or nullopt on a platform that doesn't have one yet.
std::optional<Reactor> create_or_skip_reactor() {
    auto reactor = Reactor::create();
    if (!reactor) {
        WARN("no reactor on this platform yet (" << reactor.error().message() << "), skipping");
        return std::nullopt;
    }
    return std::move(*reactor);
}

// Media controls under this test's own name, counting the requests that
// arrive in `requests`. nullopt if this machine has no media control service.
std::optional<MediaControls> open_or_skip(Reactor& reactor, std::string_view test, int& requests) {
    auto controls = MediaControls::open(
        reactor, [&requests](const MediaRequest&) { ++requests; }, test_name(test));
    if (!controls) {
        // Anything else means the service is there but broken.
        CHECK(controls.error() == std::errc::function_not_supported);
        WARN("no media controls here (" << controls.error().message() << "), skipping");
        return std::nullopt;
    }
    return std::move(*controls);
}

// Lets the reactor deliver whatever the desktop sent, without waiting.
void run_pending(Reactor& reactor) {
    for (int i = 0; i < 10; ++i) {
        CHECK(reactor.run_once(0ms).has_value());
    }
}

} // namespace

TEST_CASE("an empty name is rejected", "[media]") {
    auto reactor = create_or_skip_reactor();
    if (!reactor) {
        return;
    }
    auto controls = MediaControls::open(*reactor, [](const MediaRequest&) {}, "");
    REQUIRE_FALSE(controls.has_value());
    // A platform that isn't written yet can't check anything, so it may say
    // "not supported" instead.
    CHECK((controls.error() == std::errc::invalid_argument ||
           controls.error() == std::errc::function_not_supported));
}

TEST_CASE("the setters take a whole track's life without sending requests", "[media]") {
    auto reactor = create_or_skip_reactor();
    if (!reactor) {
        return;
    }
    int requests = 0;
    auto controls = open_or_skip(*reactor, "track", requests);
    if (!controls) {
        return;
    }

    controls->set_volume(80);
    controls->set_track({
        .title = "Test Track",
        .artist = "Test Artist",
        .album = "Test Album",
        .artwork_url = "https://example.com/cover.jpg",
        .length = 3min,
    });
    controls->set_playback_state(PlaybackState::Buffering);
    controls->set_playback_state(PlaybackState::Playing);
    run_pending(*reactor);

    controls->set_position(1s);
    controls->set_position(2s);
    controls->seeked(90s);
    controls->set_playback_state(PlaybackState::Paused);
    controls->set_volume(0);
    controls->set_volume(100);
    run_pending(*reactor);

    controls->set_playback_state(PlaybackState::Stopped);
    controls->clear_track();
    run_pending(*reactor);

    // Nobody pressed anything, so nothing should have arrived.
    CHECK(requests == 0);
}

TEST_CASE("odd track info is accepted", "[media]") {
    auto reactor = create_or_skip_reactor();
    if (!reactor) {
        return;
    }
    int requests = 0;
    auto controls = open_or_skip(*reactor, "odd_track", requests);
    if (!controls) {
        return;
    }

    // Nothing known at all.
    controls->set_track({});
    run_pending(*reactor);

    // Non-ASCII text, a long title, no artwork, no length.
    controls->set_track({
        .title = std::string(1000, 'x'),
        .artist = "Sigur R\xc3\xb3s",
        .album = "\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88",
        .artwork_url = "",
        .length = 0s,
    });
    run_pending(*reactor);

    // A position past the end, and clearing twice.
    controls->set_position(10min);
    controls->clear_track();
    controls->clear_track();
    run_pending(*reactor);

    CHECK(requests == 0);
}

TEST_CASE("media controls can be moved, closed and opened again", "[media]") {
    auto reactor = create_or_skip_reactor();
    if (!reactor) {
        return;
    }
    int requests = 0;
    {
        auto controls = open_or_skip(*reactor, "reopen", requests);
        if (!controls) {
            return;
        }
        MediaControls moved = std::move(*controls);
        moved.set_playback_state(PlaybackState::Playing);
        run_pending(*reactor);
    }
    run_pending(*reactor);

    // The same name is free again once the first one is gone.
    auto again = open_or_skip(*reactor, "reopen", requests);
    REQUIRE(again.has_value());
    again->set_playback_state(PlaybackState::Paused);
    run_pending(*reactor);

    CHECK(requests == 0);
}
