#pragma once

// The daemon's audio path, from the producer to the sound card:
//
//   producer             ring buffer        render()  (audio thread)
//   (the sine, or CEF) -> 16384 frames  ->   drift controller picks a ratio
//                                            resampler plays at that ratio
//                                            volume, then into the sink's buffer
//
// Three kinds of thread touch an AudioPath, and each member function says which:
//
//   - the producer writes into the ring buffer. That's the reactor thread
//     with top_up() when the daemon plays the test tone, or CEF's audio
//     thread with write_planar() when it plays a page. Never both: the ring
//     buffer has room for one producer only.
//   - the reactor thread (the daemon's main thread) changes the play state
//     and volume, and reads the status for telemetry.
//   - the audio thread (PipeWire's or WASAPI's) calls render() for every
//     buffer the sound card wants.
//
// They share only the ring buffer, which is built for exactly this, and a
// handful of atomics, each written by one thread only. render() and
// write_planar() never allocate, lock or make a syscall (spec section 6,
// phase 2 gate).
//
// render() waits until the ring buffer holds kTargetFrames before it starts
// playing, and again after it runs dry. That gives the drift controller its
// cushion from the start. The tone fills the ring before playback starts so
// it never waits; CEF's audio arrives in real time, so a page's first
// ~85 ms are spent filling up.
//
// That wait doesn't add a gap between songs. render() has no idea of tracks:
// while the page keeps sending, one song runs into the next untouched, and a
// break shorter than the cushion is never heard. A longer break empties the
// ring, and the 85 ms spent refilling are the 85 ms the cushion covered at
// the start of the break, so the silence lasts as long as the break did.
//
// While paused or stopped, the page's audio is thrown away: write_planar()
// drops it, and render() empties the ring buffer once it has faded out, then
// starts filling up again on play. A page doesn't stop the moment it's told
// to pause, and Chromium keeps its audio stream running for a while after, so
// keeping that audio for later would replay stale sound on resume, run dry
// and stutter. Resuming skips about the last 0.1 s before the pause.
//
// Starting and stopping the sound fades it over 10 ms instead of jumping
// between silence and full volume in one sample, which you'd hear as a click.
// Play fades in once the ring buffer has filled up (after a run-dry too), and
// Pause or Stop keeps playing for those 10 ms while it fades out, then goes
// silent. Pressing play again during a fade-out just fades back in.
//
// Once the fade-out is over, quiet() says so, and the daemon stops the sink
// so the sound system can sleep too. Play starts it again. While the sink is
// stopped nothing calls render(), which is why write_planar() has to drop the
// page's audio itself.
//
// Construct it on the heap (std::make_unique): the ring buffer alone is 128 KiB.

#include "common/spsc_ring_buffer.hpp"
#include "common/telemetry_shm.hpp"
#include "daemon/audio_engine/drift_controller.hpp"
#include "daemon/audio_engine/polyphase_resampler.hpp"
#include "daemon/audio_engine/sine_generator.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace am::audio {

using common::PlaybackState;

class AudioPath {
  public:
    static constexpr std::size_t kChannels = 2;
    static constexpr std::size_t kCapacityFrames = 16384; // spec 3.1.4: ~341 ms at 48 kHz
    static constexpr std::size_t kTargetFrames = 4096;    // ~85 ms, the drift controller's target
    static constexpr std::size_t kPacketFrames = 512;     // what CEF delivers per packet
    static constexpr std::uint32_t kFadeMillis = 10;      // fade in on play, out on pause

    using RingBuffer = common::LockFreeSpscAudioBuffer<float, kCapacityFrames, kChannels>;

    static_assert(kChannels == PolyphaseResampler::kChannels);

    // Allocates the resampler's table, so do it at setup, not on the audio thread.
    explicit AudioPath(std::uint32_t sample_rate_hz)
        : sample_rate_hz_(sample_rate_hz),
          fade_frames_(std::max<std::size_t>(1, sample_rate_hz * kFadeMillis / 1000)),
          fade_step_(1.0f / static_cast<float>(fade_frames_)) {}

    // --- Producer: the reactor thread (tone) --------------------------------

    // Writes the tone until the ring buffer holds kTargetFrames, kPacketFrames
    // at a time like CEF would. Returns how many frames it wrote.
    std::size_t top_up(SineGenerator& sine) noexcept {
        std::size_t written = 0;
        for (;;) {
            // Reserving the whole capacity reports all the free space; only
            // what's needed to reach the target gets committed.
            auto space = ring_.reserve_write(kCapacityFrames);
            const std::size_t filled = kCapacityFrames - space.frames_reserved;
            if (filled >= kTargetFrames) {
                return written;
            }
            const std::size_t frames = std::min(kTargetFrames - filled, kPacketFrames);
            const std::size_t first = std::min(frames * kChannels, space.first.size());
            sine.fill(space.first.first(first));
            sine.fill(space.second.first(frames * kChannels - first));
            ring_.commit_write(frames);
            written += frames;
        }
    }

    // --- Producer: CEF's audio thread (a page) ------------------------------

    // Copies `frames` frames of planar audio into the ring buffer, interleaving
    // them as it goes. `channels` holds one array per channel, the way
    // CefAudioHandler delivers them; the first two are used, and a single
    // channel goes to both sides. Frames that don't fit are dropped and
    // counted. While paused or stopped everything is dropped, and not counted
    // (see the top). Returns how many frames were written.
    std::size_t write_planar(std::span<const float* const> channels, std::size_t frames) noexcept {
        if (channels.empty() || state_.load(std::memory_order_acquire) != PlaybackState::Playing) {
            return 0;
        }
        const float* left = channels[0];
        const float* right = channels.size() > 1 ? channels[1] : channels[0];

        auto space = ring_.reserve_write(frames);
        std::size_t frame = 0;
        for (std::span<float> part : {space.first, space.second}) {
            for (std::size_t i = 0; i + 1 < part.size(); i += 2) {
                part[i] = left[frame];
                part[i + 1] = right[frame];
                ++frame;
            }
        }
        ring_.commit_write(space.frames_reserved);

        if (space.frames_reserved < frames) {
            dropped_frames_.fetch_add(frames - space.frames_reserved, std::memory_order_relaxed);
        }
        return space.frames_reserved;
    }

    // --- Reactor thread --------------------------------------------------

    void set_state(PlaybackState state) noexcept {
        if (state == state_.load(std::memory_order_relaxed)) {
            return;
        }
        state_.store(state, std::memory_order_release);
        // After the state, so a render() that reads the new count also reads
        // the new state (see quiet()).
        state_changes_.fetch_add(1, std::memory_order_release);
    }
    void set_volume(std::uint8_t percent) noexcept {
        volume_percent_.store(percent, std::memory_order_relaxed);
    }

    // True once render() has seen the latest set_state(), that state isn't
    // Playing, and the fade-out is over: from then on render() only writes
    // silence, so the daemon can stop the sink. Turns false again with the
    // next set_state(), so the daemon starts the sink to play, or to let
    // render() see a Stop and rewind.
    //
    // It compares counts rather than reading a flag, because render() may be
    // halfway through a buffer when the state changes. That buffer still
    // plays the old state, and it can't make quiet() true: it saw the old
    // count, and quiet() only accepts the new one.
    [[nodiscard]] bool quiet() const noexcept {
        return quiet_after_.load(std::memory_order_acquire) ==
               state_changes_.load(std::memory_order_relaxed);
    }

    // --- Any thread ------------------------------------------------------

    [[nodiscard]] PlaybackState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint8_t volume() const noexcept {
        return volume_percent_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint32_t sample_rate_hz() const noexcept { return sample_rate_hz_; }
    // How many frames a fade lasts: 480 at 48 kHz.
    [[nodiscard]] std::size_t fade_frames() const noexcept { return fade_frames_; }

    // Frames played since the last Stop (Pause keeps the place).
    [[nodiscard]] std::uint64_t position_frames() const noexcept {
        return position_frames_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t position_micros() const noexcept {
        return position_frames() * 1'000'000 / sample_rate_hz_;
    }
    // How full render() last found the ring buffer, 0.0 to 1.0.
    [[nodiscard]] float ring_fill() const noexcept {
        return static_cast<float>(fill_frames_.load(std::memory_order_relaxed)) /
               static_cast<float>(kCapacityFrames);
    }
    [[nodiscard]] float drift_ppm() const noexcept {
        return drift_ppm_.load(std::memory_order_relaxed);
    }
    // Buffers render() couldn't fill completely because the ring ran dry.
    [[nodiscard]] std::uint64_t late_buffers() const noexcept {
        return late_buffers_.load(std::memory_order_relaxed);
    }
    // Frames write_planar() threw away because the ring buffer was full.
    [[nodiscard]] std::uint64_t dropped_frames() const noexcept {
        return dropped_frames_.load(std::memory_order_relaxed);
    }

    // The ring buffer's memory, for am::pal::lock_memory().
    [[nodiscard]] std::span<const std::byte> ring_bytes() const noexcept {
        return {reinterpret_cast<const std::byte*>(&ring_), sizeof(ring_)};
    }

    // --- Audio thread ----------------------------------------------------

    // Fills `out` (interleaved stereo) with the next stretch of audio. This
    // is the AudioSink's render callback.
    void render(std::span<float> out) noexcept {
        // The count first: whatever count this reads, the state read after it
        // is at least as new (set_state() writes them the other way round).
        const std::uint64_t changes = state_changes_.load(std::memory_order_acquire);
        const PlaybackState state = state_.load(std::memory_order_acquire);
        const bool playing = state == PlaybackState::Playing;
        if (!playing && fade_ == 0.0f) {
            // Paused or stopped, and done fading out: silence, and throw away
            // what's in the ring buffer (see the top). Playing again fills up
            // afresh. Until the fade-out is done, play on below.
            std::ranges::fill(out, 0.0f);
            ring_.commit_read(ring_.available_read_frames());
            filling_up_ = true;
            if (state == PlaybackState::Stopped) {
                position_frames_.store(0, std::memory_order_relaxed);
            }
            quiet_after_.store(changes, std::memory_order_release); // see quiet()
            return;
        }

        const std::size_t fill = ring_.available_read_frames();
        fill_frames_.store(fill, std::memory_order_relaxed);
        if (filling_up_) {
            if (fill < kTargetFrames) {
                std::ranges::fill(out, 0.0f); // still filling: silence, nothing consumed
                return;
            }
            filling_up_ = false;
        }
        const double ratio = drift_.update(fill);

        // The ring's readable part can be split in two where it wraps; the
        // resampler keeps its place between the two calls.
        auto in = ring_.reserve_read(fill);
        const auto a = resampler_.process(in.first, out, ratio);
        const auto b =
            resampler_.process(in.second, out.subspan(a.frames_produced * kChannels), ratio);
        ring_.commit_read(a.frames_consumed + b.frames_consumed);

        const std::size_t produced = a.frames_produced + b.frames_produced;
        if (produced * kChannels < out.size()) {
            // Ran dry: pad with silence, count it, and wait for the ring
            // buffer to fill up again before playing on.
            std::ranges::fill(out.subspan(produced * kChannels), 0.0f);
            late_buffers_.fetch_add(1, std::memory_order_relaxed);
            filling_up_ = true;
        }

        // Plain linear volume. A curve that sounds more even (most players
        // use one) can come with the UI's volume slider.
        const float gain =
            static_cast<float>(volume_percent_.load(std::memory_order_relaxed)) / 100.0f;

        // The fade moves one step per frame: up towards 1 while playing, down
        // towards 0 while paused or stopped. Both channels of a frame get the
        // same step.
        for (std::size_t i = 0; i + 1 < out.size(); i += kChannels) {
            fade_ =
                playing ? std::min(1.0f, fade_ + fade_step_) : std::max(0.0f, fade_ - fade_step_);
            out[i] *= gain * fade_;
            out[i + 1] *= gain * fade_;
        }
        if (filling_up_) {
            fade_ = 0.0f; // ran dry: fade in again once it's filled up
        }

        position_frames_.fetch_add(produced, std::memory_order_relaxed);
        drift_ppm_.store(static_cast<float>(drift_.ppm()), std::memory_order_relaxed);
    }

  private:
    RingBuffer ring_;
    std::uint32_t sample_rate_hz_;
    std::size_t fade_frames_;
    float fade_step_; // 1 / fade_frames_

    // Written by the reactor thread, read by the audio thread.
    std::atomic<PlaybackState> state_{PlaybackState::Playing};
    std::atomic<std::uint8_t> volume_percent_{100};
    std::atomic<std::uint64_t> state_changes_{1}; // set_state() calls that changed it

    // Written by the audio thread, read by the reactor thread: the value of
    // state_changes_ when render() last went quiet (0: not yet). See quiet().
    std::atomic<std::uint64_t> quiet_after_{0};

    // Only the audio thread touches these.
    DriftController drift_;
    PolyphaseResampler resampler_;
    bool filling_up_ = true; // waiting for kTargetFrames before playing
    float fade_ = 0.0f;      // 0 is silent, 1 is full volume; see render()

    // Written by the audio thread, read by the reactor thread.
    std::atomic<std::uint64_t> position_frames_{0};
    std::atomic<std::size_t> fill_frames_{0};
    std::atomic<float> drift_ppm_{0.0f};
    std::atomic<std::uint64_t> late_buffers_{0};

    // Written by the producer, read by the reactor thread.
    std::atomic<std::uint64_t> dropped_frames_{0};
};

} // namespace am::audio
