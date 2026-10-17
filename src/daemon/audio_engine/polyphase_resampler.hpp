#pragma once

// Fractional-ratio resampler for the drift controller (spec section 3.1.4).
//
// The drift controller asks for ratios like 1.00015: consume 1.00015 input
// frames per output frame. That means most output frames fall *between* two
// input frames, so their value has to be interpolated.
//
// Straight-line interpolation between neighbours dulls the high end and adds
// distortion. The textbook answer is to rebuild the signal with a sinc filter
// (the ideal band-limited interpolator), cut down to a few taps and smoothed
// with a window so it doesn't ring. This one uses 16 taps: each output frame is
// a weighted sum of the 16 input frames around it.
//
// The weights depend on where between two input frames the output lands (the
// "phase", 0.0 to 1.0). Computing a windowed sinc per output frame would be
// slow, so the constructor precomputes weights for 256 evenly spaced phases.
// Each output frame blends the two nearest rows. That precomputed table is what
// "polyphase" refers to.
//
//   PolyphaseResampler resampler;                  // at stream setup: allocates
//   auto r = resampler.process(input, output, ratio); // per callback: does not
//   ring.commit_read(r.frames_consumed);
//
// Samples are interleaved stereo floats, the same layout as the ring buffer.
// The filter delays the audio by kTaps / 2 frames (8 frames, 0.17 ms at
// 48 kHz). process() never allocates, locks or makes a syscall. This is plain
// scalar code; a SIMD version can replace the inner loop later.

#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>
#include <vector>

namespace am::audio {

class PolyphaseResampler {
  public:
    static constexpr std::size_t kChannels = 2;
    static constexpr std::size_t kTaps = 16;
    static constexpr std::size_t kPhases = 256;

    // Builds the coefficient table. This allocates, so do it during setup, not
    // on the audio thread.
    PolyphaseResampler() : table_((kPhases + 1) * kTaps) {
        // Pass everything below 90% of the Nyquist frequency (about 21.6 kHz at
        // 48 kHz). With ratios this close to 1.0 there's nothing to
        // anti-alias, so the cutoff only has to leave room for the short
        // filter's transition band.
        constexpr double kCutoff = 0.9;
        constexpr double kPi = std::numbers::pi;
        constexpr double kCenter = static_cast<double>(kTaps / 2 - 1);

        // Row p is for an output frame `p / kPhases` of the way from history
        // frame kTaps/2 - 1 to the next one. There is one extra row (p ==
        // kPhases) so blending between row p and p + 1 never runs off the end.
        for (std::size_t p = 0; p <= kPhases; ++p) {
            const double phase = static_cast<double>(p) / static_cast<double>(kPhases);
            double sum = 0.0;
            for (std::size_t k = 0; k < kTaps; ++k) {
                // Distance in input frames from tap k to the output position.
                const double x = static_cast<double>(k) - kCenter - phase;
                const double sinc =
                    (x == 0.0) ? 1.0 : std::sin(kPi * kCutoff * x) / (kPi * kCutoff * x);
                // Blackman window over the filter's span, centered on the
                // output position.
                const double w =
                    (x + static_cast<double>(kTaps) / 2.0) / static_cast<double>(kTaps);
                const double window =
                    0.42 - 0.5 * std::cos(2.0 * kPi * w) + 0.08 * std::cos(4.0 * kPi * w);
                const double c = sinc * window;
                coefficient(p, k) = static_cast<float>(c);
                sum += c;
            }
            // Scale each row to add up to 1, so a constant input comes out at
            // exactly the same level whatever the phase.
            for (std::size_t k = 0; k < kTaps; ++k) {
                coefficient(p, k) = static_cast<float>(coefficient(p, k) / sum);
            }
        }
    }

    struct Result {
        std::size_t frames_consumed; // input frames used; commit these to the ring
        std::size_t frames_produced; // output frames written
    };

    // Fills `output` with resampled audio, consuming input frames at `ratio`
    // input frames per output frame. Stops when `output` is full or `input`
    // runs out, whichever comes first. Both spans hold interleaved frames.
    //
    // State carries over between calls, so input can arrive in pieces (for
    // example, the two halves of a ring buffer reservation that wraps).
    Result process(std::span<const float> input, std::span<float> output, double ratio) noexcept {
        const std::size_t in_frames = input.size() / kChannels;
        const std::size_t out_frames = output.size() / kChannels;
        Result result{0, 0};

        while (result.frames_produced < out_frames) {
            // Move forward through the input until the output position is
            // between history frame kTaps/2 - 1 and the one after it.
            while (phase_ >= 1.0) {
                if (result.frames_consumed == in_frames) {
                    return result; // out of input; keep phase_ for next call
                }
                push(input.subspan(result.frames_consumed * kChannels, kChannels));
                ++result.frames_consumed;
                phase_ -= 1.0;
            }

            // Blend the two table rows on either side of the exact phase.
            const double scaled = phase_ * static_cast<double>(kPhases);
            const auto row = static_cast<std::size_t>(scaled);
            const auto blend = static_cast<float>(scaled - static_cast<double>(row));

            std::span<float> frame = output.subspan(result.frames_produced * kChannels, kChannels);
            for (std::size_t ch = 0; ch < kChannels; ++ch) {
                float a = 0.0f;
                float b = 0.0f;
                for (std::size_t k = 0; k < kTaps; ++k) {
                    a += coefficient(row, k) * history_[ch][k];
                    b += coefficient(row + 1, k) * history_[ch][k];
                }
                frame[ch] = a + blend * (b - a);
            }

            ++result.frames_produced;
            phase_ += ratio;
        }
        return result;
    }

    // Clears the filter history, for example after a seek.
    void reset() noexcept {
        for (auto& channel : history_) {
            channel.fill(0.0f);
        }
        phase_ = 1.0;
    }

  private:
    float& coefficient(std::size_t phase_row, std::size_t tap) noexcept {
        return table_[phase_row * kTaps + tap];
    }

    // Shifts one input frame into the history, dropping the oldest.
    void push(std::span<const float> frame) noexcept {
        for (std::size_t ch = 0; ch < kChannels; ++ch) {
            auto& h = history_[ch];
            for (std::size_t k = 0; k + 1 < kTaps; ++k) {
                h[k] = h[k + 1];
            }
            h[kTaps - 1] = frame[ch];
        }
    }

    std::vector<float> table_; // (kPhases + 1) rows of kTaps coefficients
    std::array<std::array<float, kTaps>, kChannels> history_{};
    // Output position past history frame kTaps/2 - 1, in input frames. Starts
    // at 1.0 so the first call pulls in a frame before producing anything.
    double phase_ = 1.0;
};

} // namespace am::audio
