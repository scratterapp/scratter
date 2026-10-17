// Tests for the clock-drift controller and resampler
// (src/daemon/audio_engine/drift_controller.hpp and polyphase_resampler.hpp).
//
// The drift tests are the issue #21 gate: a simulated producer runs 150 ppm
// fast (or slow) against a simulated sound card for 60 simulated minutes, and
// the controller has to hold the ring buffer within 3% of its 4096-frame
// target with no underruns or overruns. No audio is copied, only frame counts,
// so an hour of audio simulates in well under a second.
//
// The resampler tests check the actual signal: a constant stays constant, a
// sine comes out as the same sine at the new rate, and frames are consumed at
// the requested ratio.

#include "common/spsc_ring_buffer.hpp"
#include "daemon/audio_engine/drift_controller.hpp"
#include "daemon/audio_engine/polyphase_resampler.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>
#include <vector>

namespace {

using am::audio::DriftController;
using am::audio::PolyphaseResampler;

constexpr double kSampleRate = 48'000.0;

struct DriftResult {
    double worst_error_pct = 0.0; // largest smoothed fill error after settling
    std::uint64_t underruns = 0;  // callbacks that wanted more frames than there were
    std::uint64_t overruns = 0;   // packets that didn't fit in the ring buffer
    double final_ppm = 0.0;       // the controller's correction, averaged over the last minute
};

// Runs a producer whose clock is off by `skew_ppm` against a consumer pulling
// `quantum` frames per callback, for `minutes` of simulated sound-card time.
//
// The producer delivers 480-frame packets (10 ms, like CEF). The consumer
// checks the fill at the start of each callback, asks the controller for a
// ratio, and consumes quantum * ratio frames, carrying the fraction over.
//
// The first `settle_seconds` aren't held to the 3% limit. Because the fill is
// measured at one point in a sawtooth (packets in, callbacks out), its average
// starts up to half a packet plus half a callback away from the target, and the
// controller needs some seconds to drain that at its 1000 ppm limit.
DriftResult simulate(double skew_ppm, std::size_t quantum, double minutes,
                     double settle_seconds = 60.0) {
    constexpr std::size_t kPacket = 480;
    constexpr double kTarget = 4096.0;

    auto ring = std::make_unique<am::common::LockFreeSpscAudioBuffer<float, 16384>>();
    DriftController controller;
    DriftResult result;

    // Pre-buffer to the target before playback starts (spec 3.1.4).
    for (std::size_t filled = 0; filled < static_cast<std::size_t>(kTarget); filled += kPacket) {
        const auto w = ring->reserve_write(kPacket);
        ring->commit_write(w.frames_reserved);
    }

    // Time is measured on the sound card's clock, in seconds. The producer's
    // packets are 10 ms apart on its own clock, which runs fast by skew_ppm.
    const double packet_interval =
        static_cast<double>(kPacket) / kSampleRate / (1.0 + skew_ppm * 1e-6);
    const double callback_interval = static_cast<double>(quantum) / kSampleRate;
    const auto callbacks = static_cast<std::uint64_t>(minutes * 60.0 / callback_interval);
    const auto last_minute_start = callbacks - static_cast<std::uint64_t>(60.0 / callback_interval);

    double next_packet = packet_interval;
    double fraction = 0.0;
    double ppm_sum = 0.0;

    for (std::uint64_t i = 1; i <= callbacks; ++i) {
        const double now = static_cast<double>(i) * callback_interval;

        while (next_packet <= now) {
            const auto w = ring->reserve_write(kPacket);
            if (w.frames_reserved < kPacket) {
                ++result.overruns;
            }
            ring->commit_write(w.frames_reserved);
            next_packet += packet_interval;
        }

        const std::size_t fill = ring->available_read_frames();
        const double ratio = controller.update(fill);

        fraction += static_cast<double>(quantum) * ratio;
        const auto wanted = static_cast<std::size_t>(fraction);
        fraction -= static_cast<double>(wanted);
        if (wanted > fill) {
            ++result.underruns;
        }
        ring->commit_read(wanted < fill ? wanted : fill);

        if (now > settle_seconds) {
            const double pct = std::abs(controller.smoothed_error_frames()) / kTarget * 100.0;
            result.worst_error_pct = pct > result.worst_error_pct ? pct : result.worst_error_pct;
        }
        if (i > last_minute_start) {
            ppm_sum += controller.ppm();
        }
    }
    result.final_ppm = ppm_sum / static_cast<double>(callbacks - last_minute_start);
    return result;
}

void check_drift(const DriftResult& r, double skew_ppm) {
    CHECK(r.underruns == 0);
    CHECK(r.overruns == 0);
    CHECK(r.worst_error_pct < 3.0);
    // The integral term ends up holding the clock mismatch.
    CHECK(std::abs(r.final_ppm - skew_ppm) < 15.0);
}

// The signal a sine at `hz` has at (fractional) input frame `pos`.
float sine_at(double hz, double pos) {
    return static_cast<float>(std::sin(2.0 * std::numbers::pi * hz * pos / kSampleRate));
}

} // namespace

TEST_CASE("controller sits at 1.0 when the fill is on target", "[drift]") {
    DriftController c;
    for (int i = 0; i < 1000; ++i) {
        CHECK(c.update(4096) == 1.0);
    }
}

TEST_CASE("controller speeds up when too full and slows down when too empty", "[drift]") {
    DriftController full;
    DriftController empty;
    for (int i = 0; i < 100; ++i) {
        full.update(5000);
        empty.update(3000);
    }
    CHECK(full.ratio() > 1.0);
    CHECK(empty.ratio() < 1.0);
}

TEST_CASE("controller never goes past 1000 ppm, and unwinds after a stall", "[drift]") {
    DriftController c;
    // Hugely overfull for a simulated minute at 256-frame callbacks.
    for (int i = 0; i < 11'250; ++i) {
        c.update(16'000);
        CHECK(c.ppm() <= 1000.0 + 1e-6);
    }
    CHECK(c.ppm() > 999.0);

    // Now a little too empty for another minute. Only negative error can
    // unwind the integral. With anti-windup it only got as far as the limit,
    // so it unwinds and the ratio drops below 1.0 within the minute. Without
    // anti-windup it would have grown about 40 times bigger, and the ratio
    // would still be pinned at +1000 ppm here.
    for (int i = 0; i < 11'250; ++i) {
        c.update(3500);
    }
    CHECK(c.ppm() < 0.0);
}

TEST_CASE("a producer 150 ppm fast stays within 3% for 60 minutes (256-frame callbacks)",
          "[drift]") {
    check_drift(simulate(150.0, 256, 60.0), 150.0);
}

TEST_CASE("a producer 150 ppm fast stays within 3% for 60 minutes (1024-frame callbacks)",
          "[drift]") {
    check_drift(simulate(150.0, 1024, 60.0), 150.0);
}

TEST_CASE("a producer 150 ppm slow stays within 3% for 60 minutes", "[drift]") {
    check_drift(simulate(-150.0, 256, 60.0), -150.0);
}

TEST_CASE("resampler passes a constant through unchanged", "[resampler]") {
    auto r = std::make_unique<PolyphaseResampler>();
    std::vector<float> in(2 * 1000, 0.5f);
    std::vector<float> out(2 * 900);

    const auto res = r->process(in, out, 1.0007);
    REQUIRE(res.frames_produced == 900);
    // Skip the first kTaps frames, while the history still holds zeros.
    for (std::size_t i = 2 * PolyphaseResampler::kTaps; i < out.size(); ++i) {
        CHECK(std::abs(out[i] - 0.5f) < 1e-5f);
    }
}

TEST_CASE("resampler consumes input at the requested ratio", "[resampler]") {
    auto r = std::make_unique<PolyphaseResampler>();
    std::vector<float> in(2 * 1100);
    std::vector<float> out(2 * 1024);

    for (const double ratio : {0.999, 1.0, 1.00015, 1.001}) {
        r->reset();
        std::size_t consumed = 0;
        std::size_t produced = 0;
        for (int call = 0; call < 1000; ++call) {
            const auto res = r->process(in, out, ratio);
            REQUIRE(res.frames_produced == 1024);
            consumed += res.frames_consumed;
            produced += res.frames_produced;
        }
        const double expected = static_cast<double>(produced) * ratio;
        CHECK(std::abs(static_cast<double>(consumed) - expected) < 2.0);
    }
}

TEST_CASE("resampler reproduces a sine at the new rate", "[resampler]") {
    constexpr double kRatio = 1.0005;
    constexpr std::size_t kFrames = 4800;

    for (const double hz : {440.0, 1000.0, 10'000.0}) {
        auto r = std::make_unique<PolyphaseResampler>();
        std::vector<float> in(2 * kFrames);
        for (std::size_t n = 0; n < kFrames; ++n) {
            in[2 * n] = sine_at(hz, static_cast<double>(n));
            in[2 * n + 1] = -in[2 * n];
        }
        std::vector<float> out(2 * 4000);
        const auto res = r->process(in, out, kRatio);
        REQUIRE(res.frames_produced == 4000);

        // Output frame n lands at input position n * ratio, minus the filter's
        // 8-frame delay.
        float worst = 0.0f;
        for (std::size_t n = PolyphaseResampler::kTaps; n < res.frames_produced; ++n) {
            const double pos = static_cast<double>(n) * kRatio - 8.0;
            const float want = sine_at(hz, pos);
            worst = std::max(worst, std::abs(out[2 * n] - want));
            worst = std::max(worst, std::abs(out[2 * n + 1] + want));
        }
        INFO("hz = " << hz << ", worst error = " << worst);
        CHECK(worst < 2e-3f);
    }
}

TEST_CASE("resampler gives the same output whether input comes in one piece or two",
          "[resampler]") {
    std::vector<float> in(2 * 600);
    for (std::size_t i = 0; i < in.size(); ++i) {
        in[i] = sine_at(1000.0, static_cast<double>(i / 2));
    }
    std::vector<float> whole(2 * 500);
    std::vector<float> split(2 * 500);

    auto a = std::make_unique<PolyphaseResampler>();
    REQUIRE(a->process(in, whole, 1.0003).frames_produced == 500);

    // Like a ring buffer reservation that wraps: first part, then the rest.
    auto b = std::make_unique<PolyphaseResampler>();
    const std::span<const float> all(in);
    const auto first = b->process(all.first(2 * 250), split, 1.0003);
    const auto second =
        b->process(all.subspan(2 * first.frames_consumed),
                   std::span<float>(split).subspan(2 * first.frames_produced), 1.0003);
    REQUIRE(first.frames_produced + second.frames_produced == 500);
    CHECK(whole == split);
}
