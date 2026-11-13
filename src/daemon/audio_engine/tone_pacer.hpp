#pragma once

// How much of the test tone is due, by the clock (issue #85).
//
// A real source delivers audio at its own sample rate, by its own clock: CEF
// sends a page's audio in real time. The sound card plays at its rate, by
// its clock, and the two drift apart by a few dozen parts per million. The
// drift controller (drift_controller.hpp) makes up the difference, and it can
// only do that when the fill responds to the ratio it picks.
//
// The tone used to be topped up to the controller's target on every tick
// instead, refilling exactly what render() had taken. The fill then had
// nothing to do with the ratio, the controller always saw it a little below
// target (render() reads after part of each tick has played), and its
// integral ran down to the -1000 ppm clamp. TonePacer makes the tone behave
// like a real source: it says how many frames are due since start() at the
// sample rate, by the steady clock, and the daemon writes that many
// (AudioPath::write_tone()). The controller then settles on the real
// difference between the sound card's clock and the system clock, which is
// what the telemetry page's clock_drift_ppm should show.
//
// Only the reactor thread uses it.

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace am::audio {

class TonePacer {
  public:
    using Clock = std::chrono::steady_clock;

    explicit TonePacer(std::uint32_t sample_rate_hz) noexcept : sample_rate_hz_(sample_rate_hz) {}

    // Starts counting from `now`. The daemon starts it when the tone starts
    // playing, with the ring buffer already filled to the target.
    void start(Clock::time_point now) noexcept {
        start_ = now;
        counted_ = 0;
        running_ = true;
    }

    // Pause, stop, a seek or a switch: nothing is due until start() again.
    void stop() noexcept { running_ = false; }

    [[nodiscard]] bool running() const noexcept { return running_; }

    // The frames due between the last call (or start()) and `now`. Counted
    // from start() each time, not added up call by call, so rounding never
    // builds up: after any number of calls the total is the elapsed time
    // times the rate, rounded down.
    [[nodiscard]] std::size_t due(Clock::time_point now) noexcept {
        if (!running_ || now <= start_) {
            return 0;
        }
        const auto elapsed = now - start_;
        // Whole seconds and the rest apart, so the product can't overflow:
        // nanoseconds times 48000 would after about two days.
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(elapsed);
        const auto rest = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed - seconds);
        const std::uint64_t total =
            static_cast<std::uint64_t>(seconds.count()) * sample_rate_hz_ +
            static_cast<std::uint64_t>(rest.count()) * sample_rate_hz_ / 1'000'000'000u;
        const std::uint64_t now_due = total - counted_;
        counted_ = total;
        return static_cast<std::size_t>(now_due);
    }

  private:
    std::uint32_t sample_rate_hz_;
    Clock::time_point start_{};
    std::uint64_t counted_ = 0;
    bool running_ = false;
};

} // namespace am::audio
