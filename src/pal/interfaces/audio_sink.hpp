#pragma once

// The audio sink: hands PCM to the OS and keeps it coming (spec section 4,
// "Primary Audio Sink"; Linux is PipeWire, Windows is WASAPI shared mode).
//
// The sink pulls. The OS owns a real-time thread that wakes every few
// milliseconds and asks for exactly N frames, right now. On that thread the
// sink calls our render callback, which fills the buffer, normally by reading
// from the SPSC ring buffer (src/common/spsc_ring_buffer.hpp).
//
//   auto sink = am::pal::AudioSink::open({.sample_rate_hz = 48000, .channels = 2},
//                                        [&](std::span<float> out) noexcept {
//                                            fill_from_ring_buffer(out);
//                                        });
//   sink->start();   // the callback starts running on the OS audio thread
//   ...
//   sink->stop();    // and stops; it's not running once this returns
//
// The callback only sees a span of floats, not the ring buffer, so the
// drift-compensating resampler (#21) can sit between the two later without
// changing this interface.
//
// The render callback runs on a real-time thread. It must not allocate, lock,
// make syscalls or block in any way (spec Phase 2 gate). If there's nothing
// to play, write zeros: the OS will play whatever is in the buffer.
//
// Raising the audio thread's priority is the sink's own job (PipeWire's
// data thread already runs real-time; the WASAPI sink registers its thread
// with MMCSS). Pinning the ring buffer in RAM is separate, in realtime.hpp,
// because the daemon owns that memory, not the sink.
//
// Like Reactor, each platform defines this class once: pipewire_sink.cpp and
// wasapi_sink.cpp. CMake compiles only one of them.

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <system_error>

namespace am::pal {

// Samples are always 32-bit float in [-1.0, 1.0], interleaved (L R L R ...
// for stereo), because that's what CEF delivers and the ring buffer holds.
struct AudioFormat {
    std::uint32_t sample_rate_hz = 48000;
    std::uint32_t channels = 2;
};

class AudioSink {
  public:
    // Fills `out` with out.size() / channels frames of interleaved samples.
    // The frame count can change from one call to the next.
    using RenderCallback = std::move_only_function<void(std::span<float> out) noexcept>;

    // Connects to the OS audio system and sets up a stream asking for
    // `requested`. Nothing plays until start(). The OS may pick a different
    // rate (or channel count); format() says what it actually gave us.
    [[nodiscard]] static std::expected<AudioSink, std::error_code>
    open(const AudioFormat& requested, RenderCallback render);

    // Stops the stream if it's running, like stop(): once the destructor
    // returns, the render callback isn't running and never will again.
    ~AudioSink();
    AudioSink(AudioSink&&) noexcept;
    AudioSink& operator=(AudioSink&&) noexcept;
    AudioSink(const AudioSink&) = delete;
    AudioSink& operator=(const AudioSink&) = delete;

    // Starts calling the render callback on the OS audio thread.
    std::expected<void, std::error_code> start();

    // Stops the stream. Once this returns, the render callback is not running
    // and won't be called again until start(). Starting again after a stop is
    // allowed.
    std::expected<void, std::error_code> stop();

    // The format the OS agreed to. Valid right after open().
    [[nodiscard]] AudioFormat format() const noexcept;

  private:
    struct Impl;
    explicit AudioSink(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace am::pal
