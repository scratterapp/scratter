#pragma once

// Clock-drift controller for the audio path (spec section 3.1.4).
//
// CEF writes audio into the ring buffer on the computer's clock. The sound card
// reads it on its own crystal. The two never run at exactly the same speed: a
// typical mismatch is 10 to 200 parts per million (ppm), which at +150 ppm is
// about 430 extra frames a minute. Left alone, the buffer slowly fills up (adding
// latency until it overflows) or drains (until it underruns and clicks).
//
// The fix is to play slightly faster or slower to match. Every audio callback,
// the consumer tells this controller how full the ring buffer is, and gets back
// a ratio: how many input frames to consume per output frame. 1.0 means normal
// speed, 1.0001 means 100 ppm faster. The resampler (polyphase_resampler.hpp)
// applies it.
//
//   Consumer callback:
//     const double ratio = controller.update(ring.available_read_frames());
//     resampler.process(input, output, ratio);
//
// Inside, three steps:
//   1. error = fill - target (in frames). Positive means too full.
//   2. An exponential moving average smooths the error. Packets from CEF arrive
//      in bursts, so the raw fill jumps around by hundreds of frames; the
//      drift we care about is buried underneath.
//   3. A PI controller turns the smoothed error into a ratio. The P term reacts
//      to the error now. The I term adds up the error over time, so it keeps
//      growing until the error is truly zero; it ends up holding the clock
//      mismatch itself (about 150 ppm in the test). The ratio is clamped to
//      +/-1000 ppm, which nobody can hear as a pitch change.
//
// All of this is a few multiplies per callback, with no allocation, locks or
// syscalls, so it runs on the real-time audio thread.

#include <algorithm>
#include <cstddef>

namespace am::audio {

class DriftController {
  public:
    struct Config {
        double target_fill_frames = 4096.0; // ~85 ms at 48 kHz
        double ema_alpha = 0.005;           // smoothing per callback
        // Gains are per callback, in ratio units per frame of error. Chosen by
        // simulation (tests/test_resampler_drift.cpp) for callbacks of 128 to
        // 1024 frames at 48 kHz. Higher gains settle faster but start to
        // oscillate with the larger callback sizes.
        double kp = 3e-6;
        double ki = 3e-10;
        double max_ppm = 1000.0;
    };

    DriftController() noexcept : DriftController(Config{}) {}
    explicit DriftController(const Config& config) noexcept : config_(config) {}

    // Call once per audio callback with the ring buffer's current fill.
    // Returns the ratio for this callback, within 1 +/- max_ppm / 1e6.
    double update(std::size_t fill_frames) noexcept {
        const double error = static_cast<double>(fill_frames) - config_.target_fill_frames;
        smoothed_error_ += config_.ema_alpha * (error - smoothed_error_);

        // Anti-windup: stop adding to the integral once the I term alone would
        // hit the clamp. Without this, a long stretch of error (say, a stall)
        // would wind it far past the limit, and it would take minutes to unwind
        // after the stall ended.
        const double max_deviation = config_.max_ppm * 1e-6;
        const double integral_limit = max_deviation / config_.ki;
        integral_ = std::clamp(integral_ + smoothed_error_, -integral_limit, integral_limit);

        const double deviation = config_.kp * smoothed_error_ + config_.ki * integral_;
        ratio_ = 1.0 + std::clamp(deviation, -max_deviation, max_deviation);
        return ratio_;
    }

    // Forget everything, for example after a seek or a new track flushes the
    // ring buffer.
    void reset() noexcept {
        smoothed_error_ = 0.0;
        integral_ = 0.0;
        ratio_ = 1.0;
    }

    [[nodiscard]] double ratio() const noexcept { return ratio_; }

    // How far off the consumer is playing, in ppm. Goes in
    // TelemetrySnapshot::clock_drift_ppm.
    [[nodiscard]] double ppm() const noexcept { return (ratio_ - 1.0) * 1e6; }

    // The filtered fill error in frames. The test checks this stays small.
    [[nodiscard]] double smoothed_error_frames() const noexcept { return smoothed_error_; }

  private:
    Config config_;
    double smoothed_error_ = 0.0;
    double integral_ = 0.0;
    double ratio_ = 1.0;
};

} // namespace am::audio
