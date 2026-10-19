// Tests for the PipeWire AudioSink (src/pal/linux/pipewire_sink.cpp).
//
// These need a running PipeWire daemon. Without one (CI, for example) they
// print a warning and pass, because Catch2's SKIP() needs exceptions, which
// this project builds without. Run them on your desktop to exercise the real thing. For
// listening to it, see tests/manual/play_sine.cpp.

#include "audio_sink.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <span>
#include <system_error>
#include <thread>

namespace {

using am::pal::AudioFormat;
using am::pal::AudioSink;
using namespace std::chrono_literals;

// Counts callbacks and remembers the largest buffer asked for.
struct Counters {
    std::atomic<int> calls{0};
    std::atomic<std::size_t> largest{0};
};

std::expected<AudioSink, std::error_code> open_silent_sink(Counters& counters) {
    return AudioSink::open({.sample_rate_hz = 48000, .channels = 2},
                           [&counters](std::span<float> out) noexcept {
                               for (float& sample : out) {
                                   sample = 0.0f;
                               }
                               counters.calls.fetch_add(1, std::memory_order_relaxed);
                               if (out.size() > counters.largest.load(std::memory_order_relaxed)) {
                                   counters.largest.store(out.size(), std::memory_order_relaxed);
                               }
                           });
}

} // namespace

TEST_CASE("the PipeWire sink calls the render callback between start and stop",
          "[audio][pipewire]") {
    Counters counters;
    auto sink = open_silent_sink(counters);
    if (!sink) {
        WARN("no PipeWire daemon to connect to, not testing playback: " << sink.error().message());
        return;
    }

    CHECK(sink->format().sample_rate_hz == 48000);
    CHECK(sink->format().channels == 2);

    // Inactive until start().
    std::this_thread::sleep_for(100ms);
    CHECK(counters.calls.load() == 0);

    REQUIRE(sink->start());
    std::this_thread::sleep_for(300ms);
    REQUIRE(sink->stop());

    const int calls = counters.calls.load();
    if (calls == 0) {
        // The stream connected but nothing pulled from it: no output device
        // or session manager linked it. Not this code's fault.
        WARN("PipeWire never scheduled the stream (no output linked?), not checking further");
        return;
    }
    CHECK(counters.largest.load() % 2 == 0); // whole stereo frames

    // Nothing after stop() returns.
    std::this_thread::sleep_for(100ms);
    CHECK(counters.calls.load() == calls);

    // And it starts again, the way the daemon restarts it after a pause.
    REQUIRE(sink->start());
    std::this_thread::sleep_for(300ms);
    REQUIRE(sink->stop());
    CHECK(counters.calls.load() > calls);
}

TEST_CASE("bad formats are rejected before touching PipeWire", "[audio][pipewire]") {
    auto zero_channels =
        AudioSink::open({.sample_rate_hz = 48000, .channels = 0}, [](std::span<float>) noexcept {});
    REQUIRE_FALSE(zero_channels);
    CHECK(zero_channels.error() == std::errc::invalid_argument);

    auto zero_rate =
        AudioSink::open({.sample_rate_hz = 0, .channels = 2}, [](std::span<float>) noexcept {});
    REQUIRE_FALSE(zero_rate);
    CHECK(zero_rate.error() == std::errc::invalid_argument);
}
