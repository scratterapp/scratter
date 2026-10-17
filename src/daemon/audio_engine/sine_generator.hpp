#pragma once

// A 440 Hz test tone. It stands in for CEF as the thing that fills the ring
// buffer until real decoded music arrives (bootstrap step 4 replaces it).

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>

namespace am::audio {

class SineGenerator {
  public:
    static constexpr float kFrequencyHz = 440.0f;
    static constexpr float kLevel = 0.2f; // quiet: this goes straight to the speakers

    explicit SineGenerator(std::uint32_t sample_rate_hz) noexcept
        : step_(2.0f * std::numbers::pi_v<float> * kFrequencyHz /
                static_cast<float>(sample_rate_hz)) {}

    // Writes the next stretch of the tone into `out`, interleaved stereo
    // (L R L R ...), the same sample in both channels.
    void fill(std::span<float> out) noexcept {
        for (std::size_t i = 0; i + 1 < out.size(); i += 2) {
            const float sample = kLevel * std::sin(phase_);
            out[i] = sample;
            out[i + 1] = sample;
            phase_ += step_;
            if (phase_ >= 2.0f * std::numbers::pi_v<float>) {
                phase_ -= 2.0f * std::numbers::pi_v<float>;
            }
        }
    }

  private:
    float phase_ = 0.0f;
    float step_;
};

} // namespace am::audio
