// Plays a 440 Hz sine through the ring buffer and the platform AudioSink, and
// counts late buffers (underruns). This is the Phase 2 "does it actually make
// sound" check (issue #19). It's not run by ctest because it needs speakers and ears.
//
//   ./build/linux-debug/tests/manual/play_sine [seconds]
//
// To check the audio thread makes no syscalls once it's running:
//
//   strace -f -tt -o trace.txt ./build/linux-debug/tests/manual/play_sine 5
//
// then find PipeWire's data thread in trace.txt (its id shows up once the
// stream starts) and check that nothing from it appears while the sine plays.
//
// The main thread plays CEF's part: it keeps the ring buffer topped up
// to the spec's 4096-frame target, 512 frames at a time.

#include "audio_sink.hpp"
#include "common/spsc_ring_buffer.hpp"
#include "realtime.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <numbers>
#include <span>
#include <thread>

namespace {

constexpr std::size_t kCapacityFrames = 16384; // spec 3.1.4: ~341 ms at 48 kHz
constexpr std::size_t kTargetFrames = 4096;    // ~85 ms
constexpr std::size_t kPacketFrames = 512;     // what CEF delivers per packet
constexpr std::uint32_t kSampleRate = 48000;
constexpr float kFrequency = 440.0f;
constexpr float kVolume = 0.2f; // quiet: this goes straight to your speakers

using RingBuffer = am::common::LockFreeSpscAudioBuffer<float, kCapacityFrames, 2>;

// Writes the next stretch of the sine into `out` (interleaved stereo).
struct SineGenerator {
    float phase = 0.0f;
    float step = 2.0f * std::numbers::pi_v<float> * kFrequency / static_cast<float>(kSampleRate);

    void fill(std::span<float> out) {
        for (std::size_t i = 0; i + 1 < out.size(); i += 2) {
            const float sample = kVolume * std::sin(phase);
            out[i] = sample;     // left
            out[i + 1] = sample; // right
            phase += step;
            if (phase >= 2.0f * std::numbers::pi_v<float>) {
                phase -= 2.0f * std::numbers::pi_v<float>;
            }
        }
    }
};

// Tops the buffer up to the target. Returns how many frames it wrote.
std::size_t top_up(RingBuffer& ring, SineGenerator& sine) {
    // reserve_write(capacity) reports all the free space; we then commit only
    // what's needed to reach the target.
    auto space = ring.reserve_write(kCapacityFrames);
    const std::size_t filled = kCapacityFrames - space.frames_reserved;
    if (filled >= kTargetFrames) {
        return 0;
    }
    const std::size_t frames = std::min(kTargetFrames - filled, kPacketFrames);
    const std::size_t first = std::min(frames * 2, space.first.size());
    sine.fill(space.first.first(first));
    sine.fill(space.second.first(frames * 2 - first));
    ring.commit_write(frames);
    return frames;
}

} // namespace

int main(int argc, char** argv) {
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 10;

    auto ring = std::make_unique<RingBuffer>();
    const std::span<const std::byte> ring_bytes(reinterpret_cast<const std::byte*>(ring.get()),
                                                sizeof(RingBuffer));
    if (auto locked = am::pal::lock_memory(ring_bytes); !locked) {
        std::printf("warning: couldn't lock the ring buffer in RAM: %s\n",
                    locked.error().message().c_str());
    }

    // Pre-fill to the target before any audio is pulled.
    SineGenerator sine;
    while (top_up(*ring, sine) > 0) {
    }

    // "buffers" counts every time the sink asked us for audio; "late" counts the
    // times the ring buffer didn't have enough ready (an underrun).
    std::atomic<std::size_t> late{0};
    std::atomic<std::size_t> buffers{0};

    auto sink = am::pal::AudioSink::open(
        {.sample_rate_hz = kSampleRate, .channels = 2}, [&](std::span<float> out) noexcept {
            const std::size_t wanted = out.size() / 2;
            auto in = ring->reserve_read(wanted);
            std::ranges::copy(in.first, out.begin());
            std::ranges::copy(in.second,
                              out.begin() + static_cast<std::ptrdiff_t>(in.first.size()));
            ring->commit_read(in.frames_reserved);
            if (in.frames_reserved < wanted) {
                // Ran dry: pad with silence and count it.
                std::ranges::fill(out.subspan(in.frames_reserved * 2), 0.0f);
                late.fetch_add(1, std::memory_order_relaxed);
            }
            buffers.fetch_add(1, std::memory_order_relaxed);
        });
    if (!sink) {
        std::printf("couldn't open the audio sink: %s\n", sink.error().message().c_str());
        return 1;
    }
    std::printf("playing %g Hz at %u Hz for %d s\n", static_cast<double>(kFrequency),
                sink->format().sample_rate_hz, seconds);

    if (auto started = sink->start(); !started) {
        std::printf("couldn't start: %s\n", started.error().message().c_str());
        return 1;
    }

    const auto started_at = std::chrono::steady_clock::now();
    const auto end = started_at + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < end) {
        while (top_up(*ring, sine) > 0) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (auto stopped = sink->stop(); !stopped) {
        std::printf("couldn't stop: %s\n", stopped.error().message().c_str());
    }
    const std::chrono::duration<double> played = std::chrono::steady_clock::now() - started_at;
    (void)am::pal::unlock_memory(ring_bytes);

    std::printf("%.1f s played, %zu buffers, %zu late\n", played.count(), buffers.load(),
                late.load());
    return late.load() == 0 ? 0 : 2;
}
