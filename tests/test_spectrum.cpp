// Tests for the spectrum analyzer (src/daemon/audio_engine/spectrum.hpp): the
// FFT on its own, then the 64 bands the window draws, fed known signals.

#include "daemon/audio_engine/spectrum.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <numbers>
#include <vector>

namespace {

using am::audio::SpectrumAnalyzer;

constexpr std::uint32_t kRate = 48000;

std::vector<float> sine(double hz, float amplitude, std::size_t samples) {
    std::vector<float> out(samples);
    for (std::size_t i = 0; i < samples; ++i) {
        out[i] = amplitude * static_cast<float>(std::sin(2.0 * std::numbers::pi * hz *
                                                         static_cast<double>(i) / kRate));
    }
    return out;
}

// A frequency in the middle of `band`, on an FFT bin so it doesn't smear.
double center_of(const SpectrumAnalyzer& analyzer, std::size_t band) {
    const double bin_hz =
        static_cast<double>(kRate) / static_cast<double>(SpectrumAnalyzer::kFftSize);
    const double hz = std::sqrt(analyzer.band_low_hz(band) * analyzer.band_high_hz(band));
    return std::round(hz / bin_hz) * bin_hz;
}

std::size_t loudest_band(const SpectrumAnalyzer& analyzer) {
    const auto& bands = analyzer.bands();
    return static_cast<std::size_t>(std::distance(bands.begin(), std::ranges::max_element(bands)));
}

} // namespace

TEST_CASE("the FFT finds a single cosine in its own bin", "[spectrum]") {
    constexpr std::size_t kN = SpectrumAnalyzer::kFftSize;
    std::vector<std::complex<float>> data(kN);
    for (std::size_t i = 0; i < kN; ++i) {
        data[i] = static_cast<float>(std::cos(2.0 * std::numbers::pi * 10.0 *
                                              static_cast<double>(i) / static_cast<double>(kN)));
    }
    const auto twiddles = am::audio::fft_twiddles<kN>();
    am::audio::fft<kN>(std::span<std::complex<float>, kN>(data.data(), kN), twiddles);

    // A cosine of amplitude 1 puts N/2 in bin 10 and its mirror, nothing elsewhere.
    constexpr auto kHalf = static_cast<float>(kN) / 2.0f;
    CHECK(std::abs(std::abs(data[10]) - kHalf) < 0.02f * kHalf);
    CHECK(std::abs(std::abs(data[kN - 10]) - kHalf) < 0.02f * kHalf);
    float elsewhere = 0.0f;
    for (std::size_t i = 0; i < kN; ++i) {
        if (i != 10 && i != kN - 10) {
            elsewhere = std::max(elsewhere, std::abs(data[i]));
        }
    }
    CHECK(elsewhere < 0.01f);
}

TEST_CASE("the bands run from low to high, each starting where the last ends", "[spectrum]") {
    const auto analyzer = std::make_unique<SpectrumAnalyzer>(kRate);
    CHECK(std::abs(analyzer->band_low_hz(0) - SpectrumAnalyzer::kLowestHz) < 0.01);
    CHECK(std::abs(analyzer->band_high_hz(SpectrumAnalyzer::kBands - 1) -
                   SpectrumAnalyzer::kHighestHz) < 0.1);
    for (std::size_t band = 1; band < SpectrumAnalyzer::kBands; ++band) {
        CHECK(analyzer->band_low_hz(band) > analyzer->band_low_hz(band - 1));
        CHECK(std::abs(analyzer->band_low_hz(band) - analyzer->band_high_hz(band - 1)) < 1e-6);
    }
}

TEST_CASE("silence is the floor in every band", "[spectrum]") {
    auto analyzer = std::make_unique<SpectrumAnalyzer>(kRate);
    const std::vector<float> quiet(SpectrumAnalyzer::kFftSize, 0.0f);
    analyzer->push(quiet);
    analyzer->analyze();
    for (float db : analyzer->bands()) {
        CHECK(db == SpectrumAnalyzer::kFloorDb);
    }
    CHECK(analyzer->at_floor());
}

TEST_CASE("a sine peaks in the band that holds its frequency", "[spectrum]") {
    auto analyzer = std::make_unique<SpectrumAnalyzer>(kRate);
    // Low, middle and high, so the band edges are right all along. Below
    // about band 28 (450 Hz) a band is narrower than an FFT bin, and
    // neighbours share one, so a sine there lights a few bands alike.
    for (const std::size_t band : {std::size_t{30}, std::size_t{45}, std::size_t{60}}) {
        const double hz = center_of(*analyzer, band);
        INFO("band " << band << " at " << hz << " Hz");
        analyzer->push(sine(hz, 0.5f, SpectrumAnalyzer::kFftSize));
        analyzer->analyze();
        CHECK(loudest_band(*analyzer) == band);
        // Bands well away from it stay near the floor.
        CHECK(analyzer->bands()[band > 40 ? band - 20 : band + 20] < -60.0f);
    }
}

TEST_CASE("a full-scale sine reads about 0 dB, and half of it about -6 dB", "[spectrum]") {
    auto analyzer = std::make_unique<SpectrumAnalyzer>(kRate);
    const double hz = center_of(*analyzer, 40);
    analyzer->push(sine(hz, 1.0f, SpectrumAnalyzer::kFftSize));
    analyzer->analyze();
    CHECK(std::abs(analyzer->bands()[40]) < 0.5f);

    analyzer = std::make_unique<SpectrumAnalyzer>(kRate); // no fall-back from the first
    analyzer->push(sine(hz, 0.5f, SpectrumAnalyzer::kFftSize));
    analyzer->analyze();
    CHECK(std::abs(analyzer->bands()[40] + 6.02f) < 0.5f);
}

TEST_CASE("only the newest kFftSize samples count", "[spectrum]") {
    auto analyzer = std::make_unique<SpectrumAnalyzer>(kRate);
    const double hz = center_of(*analyzer, 30);
    analyzer->push(sine(hz, 0.5f, SpectrumAnalyzer::kFftSize)); // old, pushed out below
    analyzer->push(std::vector<float>(SpectrumAnalyzer::kFftSize, 0.0f));
    // Let the bands fall to the floor first, so what analyze() finds next is
    // only what's in the history now: silence.
    for (int i = 0; i < 100; ++i) {
        analyzer->fall();
    }
    analyzer->analyze();
    CHECK(analyzer->at_floor());
}

TEST_CASE("a band jumps up at once and falls back slowly", "[spectrum]") {
    auto analyzer = std::make_unique<SpectrumAnalyzer>(kRate);
    const double hz = center_of(*analyzer, 30);
    analyzer->push(sine(hz, 0.5f, SpectrumAnalyzer::kFftSize));
    analyzer->analyze();
    const float loud = analyzer->bands()[30];
    CHECK(loud > -10.0f);

    // Silence: the band falls by kFallDb per update, not straight to the floor.
    analyzer->push(std::vector<float>(SpectrumAnalyzer::kFftSize, 0.0f));
    analyzer->analyze();
    CHECK(std::abs(analyzer->bands()[30] - (loud - SpectrumAnalyzer::kFallDb)) < 1e-4f);
    CHECK_FALSE(analyzer->at_floor());

    // fall() is the same step, for when no audio arrived at all.
    analyzer->fall();
    CHECK(std::abs(analyzer->bands()[30] - (loud - 2 * SpectrumAnalyzer::kFallDb)) < 1e-4f);

    for (int i = 0; i < 100; ++i) {
        analyzer->fall();
    }
    CHECK(analyzer->at_floor());
    CHECK(analyzer->bands()[30] == SpectrumAnalyzer::kFloorDb);
}

TEST_CASE("push() takes more than kFftSize at once, and in small pieces", "[spectrum]") {
    const auto whole = std::make_unique<SpectrumAnalyzer>(kRate);
    const auto pieces = std::make_unique<SpectrumAnalyzer>(kRate);
    const std::vector<float> signal = sine(1000.0, 0.5f, SpectrumAnalyzer::kFftSize * 3 + 77);
    whole->push(signal);
    for (std::size_t at = 0; at < signal.size(); at += 100) {
        pieces->push(std::span(signal).subspan(at, std::min<std::size_t>(100, signal.size() - at)));
    }
    whole->analyze();
    pieces->analyze();
    CHECK(whole->bands() == pieces->bands());
}
