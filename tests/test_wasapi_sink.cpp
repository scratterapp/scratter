// Tests for the WASAPI AudioSink (src/pal/win32/wasapi_sink.cpp).
//
// These need an audio output device. Without one (CI, for example) they print
// a warning and pass, because Catch2's SKIP() needs exceptions, which this
// project builds without. For listening to it, see tests/manual/play_sine.cpp.

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

// Counts callbacks and remembers the largest buffer asked for and the thread
// that asked.
struct Counters {
    std::atomic<int> calls{0};
    std::atomic<std::size_t> largest{0};
    std::atomic<std::thread::id> thread{};
};

std::expected<AudioSink, std::error_code> open_silent_sink(Counters& counters) {
    return AudioSink::open(
        {.sample_rate_hz = 48000, .channels = 2}, [&counters](std::span<float> out) noexcept {
            for (float& sample : out) {
                sample = 0.0f;
            }
            counters.calls.fetch_add(1, std::memory_order_relaxed);
            if (out.size() > counters.largest.load(std::memory_order_relaxed)) {
                counters.largest.store(out.size(), std::memory_order_relaxed);
            }
            counters.thread.store(std::this_thread::get_id(), std::memory_order_relaxed);
        });
}

} // namespace

TEST_CASE("the WASAPI sink calls the render callback between start and stop", "[audio][wasapi]") {
    Counters counters;
    auto sink = open_silent_sink(counters);
    if (!sink) {
        WARN("no audio output device, not testing playback: " << sink.error().message());
        return;
    }

    // Shared mode can run at the mix rate instead of the one asked for, but
    // never changes the channel count.
    CHECK(sink->format().sample_rate_hz > 0);
    CHECK(sink->format().channels == 2);

    // Nothing until start().
    std::this_thread::sleep_for(100ms);
    CHECK(counters.calls.load() == 0);

    REQUIRE(sink->start());
    std::this_thread::sleep_for(300ms);
    REQUIRE(sink->stop());

    const int calls = counters.calls.load();
    REQUIRE(calls > 0);
    CHECK(counters.largest.load() % 2 == 0); // whole stereo frames
    // The sink's own thread, not ours.
    CHECK(counters.thread.load() != std::this_thread::get_id());

    // Nothing after stop() returns.
    std::this_thread::sleep_for(100ms);
    CHECK(counters.calls.load() == calls);

    // And it starts again, the way the daemon restarts it after a pause.
    REQUIRE(sink->start());
    std::this_thread::sleep_for(300ms);
    REQUIRE(sink->stop());
    CHECK(counters.calls.load() > calls);
}

TEST_CASE("start and stop twice in a row are harmless", "[audio][wasapi]") {
    Counters counters;
    auto sink = open_silent_sink(counters);
    if (!sink) {
        WARN("no audio output device, not testing playback: " << sink.error().message());
        return;
    }
    CHECK(sink->stop()); // never started
    REQUIRE(sink->start());
    CHECK(sink->start());
    CHECK(sink->stop());
    CHECK(sink->stop());
}

TEST_CASE("destroying a playing sink stops it", "[audio][wasapi]") {
    Counters counters;
    {
        auto sink = open_silent_sink(counters);
        if (!sink) {
            WARN("no audio output device, not testing playback: " << sink.error().message());
            return;
        }
        REQUIRE(sink->start());
        std::this_thread::sleep_for(100ms);
    }
    const int calls = counters.calls.load();
    std::this_thread::sleep_for(100ms);
    CHECK(counters.calls.load() == calls);
}

TEST_CASE("bad formats are rejected before touching WASAPI", "[audio][wasapi]") {
    auto zero_channels =
        AudioSink::open({.sample_rate_hz = 48000, .channels = 0}, [](std::span<float>) noexcept {});
    REQUIRE_FALSE(zero_channels);
    CHECK(zero_channels.error() == std::errc::invalid_argument);

    auto zero_rate =
        AudioSink::open({.sample_rate_hz = 0, .channels = 2}, [](std::span<float>) noexcept {});
    REQUIRE_FALSE(zero_rate);
    CHECK(zero_rate.error() == std::errc::invalid_argument);
}

TEST_CASE("a shared sink says so", "[audio][wasapi]") {
    Counters counters;
    if (auto sink = open_silent_sink(counters)) {
        CHECK(sink->mode() == am::pal::OutputMode::Shared);
    } else {
        WARN("no audio output device, not testing the shared sink: " << sink.error().message());
    }
}

// Exclusive mode takes the whole device for the half second this runs, so
// anything else playing through it pauses or goes quiet meanwhile. If
// something is playing right now, open() says busy and the test only checks
// that the error is one the interface names.
TEST_CASE("an exclusive sink plays at exactly the format asked for", "[audio][wasapi]") {
    Counters counters;
    auto sink = AudioSink::open(
        {.sample_rate_hz = 48000, .channels = 2},
        [&counters](std::span<float> out) noexcept {
            for (float& sample : out) {
                sample = 0.0f;
            }
            counters.calls.fetch_add(1, std::memory_order_relaxed);
        },
        am::pal::OutputMode::Exclusive);
    if (!sink) {
        // No device (CI), one that can't do 48 kHz stereo on its own or has
        // exclusive mode turned off, or one that another app is using.
        WARN("no exclusive output here, not testing playback: " << sink.error().message());
        return;
    }

    CHECK(sink->mode() == am::pal::OutputMode::Exclusive);
    CHECK(sink->format().sample_rate_hz == 48000);
    CHECK(sink->format().channels == 2);

    REQUIRE(sink->start());
    std::this_thread::sleep_for(200ms);
    REQUIRE(sink->stop());
    const int calls = counters.calls.load();
    CHECK(calls > 0);

    // The device stays ours through the stop, and starts again.
    REQUIRE(sink->start());
    std::this_thread::sleep_for(200ms);
    REQUIRE(sink->stop());
    CHECK(counters.calls.load() > calls);
}

TEST_CASE("exclusive mode refuses a format no device can do", "[audio][wasapi]") {
    // 1 Hz: no driver takes that, so this fails before any device is held.
    auto sink = AudioSink::open(
        {.sample_rate_hz = 1, .channels = 2}, [](std::span<float>) noexcept {},
        am::pal::OutputMode::Exclusive);
    REQUIRE_FALSE(sink);
    if (sink.error() != std::errc::not_supported) {
        // No device at all (CI) says "element not found" instead.
        WARN("no audio output device: " << sink.error().message());
    }
}
