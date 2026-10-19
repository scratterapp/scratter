// How much CPU our own audio code costs: the drift controller, resampler,
// fades and volume in AudioPath::render(), plus write_planar(), which copies
// CEF's packets into the ring buffer.
//
// It plays a simulated minute of audio as fast as it can, the way the daemon
// does with --test-film: 512-frame packets go in as the ring buffer needs
// them, like CEF's, and render() takes 1024 frames at a time, like a PipeWire
// callback. Then it says what share of one CPU core that would take in real
// time.
//
//   ./build/linux-release/bench/audio_render [seconds_of_audio]
//
// Compare a debug build (linux-debug, linux-cef) with an optimized one
// (linux-release, linux-cef-release): the code is the same, only the compiler
// settings differ. docs/learning/performance.md has the numbers.

#include "daemon/audio_engine/audio_path.hpp"
#include "daemon/audio_engine/sine_generator.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace {

using am::audio::AudioPath;
using Clock = std::chrono::steady_clock;

constexpr std::uint32_t kSampleRate = 48000;
constexpr std::size_t kCallbackFrames = 1024; // PipeWire's usual quantum at 48 kHz

} // namespace

int main(int argc, char** argv) {
    const double seconds = argc > 1 ? std::atof(argv[1]) : 60.0;
    if (seconds <= 0.0) {
        std::fprintf(stderr, "usage: %s [seconds_of_audio]\n", argv[0]);
        return 2;
    }

    // One packet of the tone, split into two channels the way CEF delivers it.
    am::audio::SineGenerator sine(kSampleRate);
    std::vector<float> interleaved(AudioPath::kPacketFrames * AudioPath::kChannels);
    sine.fill(interleaved);
    std::vector<float> left(AudioPath::kPacketFrames);
    std::vector<float> right(AudioPath::kPacketFrames);
    for (std::size_t i = 0; i < AudioPath::kPacketFrames; ++i) {
        left[i] = interleaved[2 * i];
        right[i] = interleaved[2 * i + 1];
    }
    const std::array<const float*, 2> packet{left.data(), right.data()};

    auto audio = std::make_unique<AudioPath>(kSampleRate);
    std::vector<float> out(kCallbackFrames * AudioPath::kChannels);
    const auto callbacks =
        static_cast<std::size_t>(seconds * kSampleRate / static_cast<double>(kCallbackFrames));

    // The sum of everything played, printed at the end, so the compiler
    // can't decide the output is never used and skip the work.
    double checksum = 0.0;
    std::size_t written = 0;
    std::size_t rendered = 0;
    const auto started = Clock::now();
    for (std::size_t c = 0; c < callbacks; ++c) {
        // CEF delivers in real time, so it stays about kTargetFrames ahead.
        while (written < rendered + AudioPath::kTargetFrames) {
            written += audio->write_planar(packet, AudioPath::kPacketFrames);
        }
        audio->render(out);
        rendered += kCallbackFrames;
        checksum += static_cast<double>(out[c % out.size()]);
    }
    const std::chrono::duration<double> took = Clock::now() - started;

    const double played = static_cast<double>(rendered) / kSampleRate;
    std::printf("%.0f s of audio took %.1f ms: %.2f%% of one core in real time\n", played,
                took.count() * 1000.0, 100.0 * took.count() / played);
    std::printf("late buffers: %llu, dropped frames: %llu (both should be 0), checksum %.3f\n",
                static_cast<unsigned long long>(audio->late_buffers()),
                static_cast<unsigned long long>(audio->dropped_frames()), checksum);
    return 0;
}
