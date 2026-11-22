// How much CPU the spectrum costs the daemon (#40): one SpectrumAnalyzer
// update, the way the tick does it while a window is connected. That's a push()
// of 30 ms of new audio, then analyze(): the Hann window, the 2048-point FFT
// and the 64 bands. It runs many updates as fast as it can and says how long
// one takes, and what share of one core 33 a second would take.
//
//   ./build/linux-release/bench/spectrum_fft [updates]
//
// As with audio_render, compare a debug build with an optimized one; the
// numbers are in docs/BENCHMARKS.md.

#include "daemon/audio_engine/spectrum.hpp"
#include "daemon/audio_engine/sine_generator.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace {

using am::audio::SpectrumAnalyzer;
using Clock = std::chrono::steady_clock;

constexpr std::uint32_t kSampleRate = 48000;
constexpr std::size_t kNewSamples = kSampleRate * SpectrumAnalyzer::kUpdateMillis / 1000;

} // namespace

int main(int argc, char** argv) {
    const long updates = argc > 1 ? std::atol(argv[1]) : 20000;
    if (updates <= 0) {
        std::fprintf(stderr, "usage: %s [updates]\n", argv[0]);
        return 2;
    }

    // The tone, in mono, so the FFT has something other than zeros to chew on.
    am::audio::SineGenerator sine(kSampleRate);
    std::vector<float> stereo(kNewSamples * 2);
    sine.fill(stereo);
    std::vector<float> mono(kNewSamples);
    for (std::size_t i = 0; i < kNewSamples; ++i) {
        mono[i] = (stereo[2 * i] + stereo[2 * i + 1]) * 0.5f;
    }

    const auto analyzer = std::make_unique<SpectrumAnalyzer>(kSampleRate);
    // The sum of every band, printed at the end, so the compiler can't decide
    // the work is never used and skip it.
    double checksum = 0.0;
    const auto started = Clock::now();
    for (long u = 0; u < updates; ++u) {
        analyzer->push(mono);
        analyzer->analyze();
        checksum += static_cast<double>(analyzer->bands()[static_cast<std::size_t>(u) % 64]);
    }
    const std::chrono::duration<double, std::micro> took = Clock::now() - started;

    const double each = took.count() / static_cast<double>(updates);
    const double per_second = 1000.0 / SpectrumAnalyzer::kUpdateMillis;
    std::printf("%ld updates took %.1f ms: %.2f us each, %.3f%% of one core at %.0f a second\n",
                updates, took.count() / 1000.0, each, each * per_second / 1e4, per_second);
    std::printf("checksum %.3f\n", checksum);
    return 0;
}
