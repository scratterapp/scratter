#pragma once

// The spectrum the window draws (issue #40): 64 bars, one per band of
// frequencies, each as loud as the loudest part of its band.
//
//   render() (audio thread)          the tick (reactor thread), ~33 times a second
//   copies what it plays, in mono -> push(): the newest kFftSize samples
//   into AudioPath's tap ring        analyze(): Hann window, FFT, 64 bands in dB
//                                    bands() -> the telemetry page
//
// The bands are spaced evenly on a log scale from 30 Hz to 16 kHz, the way we
// hear pitch: each is about 10% wider than the one below. An FFT of 2048
// samples at 48 kHz has bins 23.4 Hz apart, so below about 450 Hz a band is
// narrower than a bin. Such a band takes the bin nearest its middle, and
// neighbours there share one, so the low end moves in small steps of two or
// three bars.
//
// A bar jumps up as soon as its band gets louder, then falls back kFallDb
// per update, so it doesn't flicker with every frame of a drum hit.
//
// These samples are only ever turned into band levels: they are never kept
// past the next analysis or sent anywhere (the spec's non-goals: no
// recording).
//
// It holds about 40 KiB of tables and buffers in place, so make it with
// std::make_unique rather than on the stack. Nothing in it allocates. None of
// it runs on the audio thread, which only fills AudioPath's tap ring.

#include "common/telemetry_shm.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <utility>

namespace am::audio {

// Reverses the lowest `bits` bits of `value`: the order a radix-2 FFT wants
// its input in.
[[nodiscard]] constexpr std::size_t bit_reverse(std::size_t value, unsigned bits) noexcept {
    std::size_t reversed = 0;
    for (unsigned i = 0; i < bits; ++i) {
        reversed = (reversed << 1) | ((value >> i) & 1);
    }
    return reversed;
}

static_assert(bit_reverse(0b0001, 4) == 0b1000);
static_assert(bit_reverse(0b0110, 4) == 0b0110);
static_assert(bit_reverse(0b1011, 4) == 0b1101);
static_assert(bit_reverse(1, 11) == 1024);

// e^(-2πik/N) for k below N/2: the factors fft() multiplies by.
template <std::size_t N>
[[nodiscard]] std::array<std::complex<float>, N / 2> fft_twiddles() noexcept {
    std::array<std::complex<float>, N / 2> twiddles{};
    for (std::size_t k = 0; k < N / 2; ++k) {
        const double angle =
            -2.0 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(N);
        twiddles[k] = {static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))};
    }
    return twiddles;
}

// An in-place radix-2 FFT (Cooley-Tukey, decimation in time): `data` goes in
// as N samples and comes out as N frequency bins. Plain and small; at 2048
// points it takes tens of microseconds, which is plenty at 33 times a second.
template <std::size_t N>
    requires(N >= 2 && std::has_single_bit(N))
void fft(std::span<std::complex<float>, N> data,
         const std::array<std::complex<float>, N / 2>& twiddles) noexcept {
    constexpr auto kBits = static_cast<unsigned>(std::countr_zero(N));
    for (std::size_t i = 0; i < N; ++i) {
        const std::size_t j = bit_reverse(i, kBits);
        if (i < j) {
            std::swap(data[i], data[j]);
        }
    }
    // Each pass joins pairs of FFTs of `half` points into ones of twice that.
    for (std::size_t half = 1; half < N; half *= 2) {
        const std::size_t stride = N / (2 * half); // twiddle step at this size
        for (std::size_t start = 0; start < N; start += 2 * half) {
            for (std::size_t k = 0; k < half; ++k) {
                const std::complex<float> odd = twiddles[k * stride] * data[start + k + half];
                data[start + k + half] = data[start + k] - odd;
                data[start + k] += odd;
            }
        }
    }
}

class SpectrumAnalyzer {
  public:
    static constexpr std::size_t kFftSize = 2048; // ~43 ms at 48 kHz
    static constexpr std::size_t kBands = common::kFftBandCount;
    static constexpr double kLowestHz = 30.0;
    static constexpr double kHighestHz = 16000.0;
    static constexpr float kFloorDb = -96.0f; // the telemetry page's range is -96 to 0
    static constexpr float kFallDb = 3.0f;    // per update: about 100 dB a second
    // How often the daemon calls analyze() or fall() while a client is connected.
    static constexpr std::uint32_t kUpdateMillis = 30;

    static_assert(std::has_single_bit(kFftSize));
    static_assert(kBands == 64, "the telemetry page has 64 bands");

    // Builds the window, the FFT's factors and which bins go in which band.
    explicit SpectrumAnalyzer(std::uint32_t sample_rate_hz) noexcept
        : twiddles_(fft_twiddles<kFftSize>()) {
        for (std::size_t i = 0; i < kFftSize; ++i) {
            // Hann: 0 at both ends, 1 in the middle, so the block's edges
            // don't add a click of their own to the spectrum.
            window_[i] = static_cast<float>(
                0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) /
                                     static_cast<double>(kFftSize)));
        }
        const double ratio = kHighestHz / kLowestHz;
        for (std::size_t b = 0; b <= kBands; ++b) {
            edges_hz_[b] =
                kLowestHz * std::pow(ratio, static_cast<double>(b) / static_cast<double>(kBands));
        }
        const double bin_hz = static_cast<double>(sample_rate_hz) / static_cast<double>(kFftSize);
        for (std::size_t b = 0; b < kBands; ++b) {
            // Bins whose middle lies in [low, high); none for a band that's
            // narrower than a bin, which then takes the bin nearest its middle.
            const auto bin_at = [&](double hz) {
                return std::min(kFftSize / 2, static_cast<std::size_t>(std::ceil(hz / bin_hz)));
            };
            std::size_t first = bin_at(edges_hz_[b]);
            std::size_t last = bin_at(edges_hz_[b + 1]);
            if (first >= last) {
                const double middle = std::sqrt(edges_hz_[b] * edges_hz_[b + 1]);
                first = std::min(kFftSize / 2 - 1,
                                 static_cast<std::size_t>(std::lround(middle / bin_hz)));
                last = first + 1;
            }
            bins_[b] = {first, last};
        }
        bands_.fill(kFloorDb);
    }

    // Adds the newest samples (mono) to the history analyze() looks at. Only
    // the last kFftSize are kept.
    void push(std::span<const float> samples) noexcept {
        if (samples.size() > kFftSize) {
            samples = samples.last(kFftSize);
        }
        for (const float s : samples) {
            history_[next_] = s;
            next_ = (next_ + 1) % kFftSize;
        }
    }

    // Works out the bands from the last kFftSize samples pushed. A band that
    // got louder shows it at once; one that got quieter falls by kFallDb.
    void analyze() noexcept {
        // Oldest first: the history is a ring, and next_ is where it starts.
        for (std::size_t i = 0; i < kFftSize; ++i) {
            work_[i] = history_[(next_ + i) % kFftSize] * window_[i];
        }
        fft<kFftSize>(work_, twiddles_);

        // A sine of amplitude A peaks at A * N / 4 through a Hann window, so
        // this scale makes a full-scale sine 0 dB.
        constexpr float kScale = 4.0f / static_cast<float>(kFftSize);
        for (std::size_t b = 0; b < kBands; ++b) {
            float peak = 0.0f;
            for (std::size_t bin = bins_[b].first; bin < bins_[b].last; ++bin) {
                peak = std::max(peak, std::abs(work_[bin]));
            }
            const float amplitude = peak * kScale;
            // Anything below the floor is the floor (log10 of 0 isn't a number).
            constexpr float kFloorAmplitude = 1.5849e-5f; // 10^(-96 / 20)
            const float db = amplitude > kFloorAmplitude ? 20.0f * std::log10(amplitude) : kFloorDb;
            bands_[b] = std::clamp(std::max(db, bands_[b] - kFallDb), kFloorDb, 0.0f);
        }
    }

    // No new audio this time (paused, stopped, filling up): every bar falls.
    void fall() noexcept {
        for (float& db : bands_) {
            db = std::max(kFloorDb, db - kFallDb);
        }
    }

    // Back to silence: every bar at the floor and nothing in the history.
    void reset() noexcept {
        history_.fill(0.0f);
        bands_.fill(kFloorDb);
    }

    [[nodiscard]] const std::array<float, kBands>& bands() const noexcept { return bands_; }
    [[nodiscard]] bool at_floor() const noexcept {
        return std::ranges::all_of(bands_, [](float db) { return db <= kFloorDb; });
    }

    // Where each band starts and ends, in Hz.
    [[nodiscard]] double band_low_hz(std::size_t band) const noexcept { return edges_hz_[band]; }
    [[nodiscard]] double band_high_hz(std::size_t band) const noexcept {
        return edges_hz_[band + 1];
    }

  private:
    struct Bins {
        std::size_t first = 0; // the band is bins first up to, but not including, last
        std::size_t last = 0;
    };

    std::array<std::complex<float>, kFftSize / 2> twiddles_;
    std::array<float, kFftSize> window_{};
    std::array<double, kBands + 1> edges_hz_{};
    std::array<Bins, kBands> bins_{};

    std::array<float, kFftSize> history_{}; // a ring; next_ is the oldest sample
    std::size_t next_ = 0;
    std::array<std::complex<float>, kFftSize> work_{};
    std::array<float, kBands> bands_{};
};

} // namespace am::audio
