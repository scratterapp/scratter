#pragma once

// Lock-free single-producer, single-consumer ring buffer for interleaved PCM
// audio (spec section 3.1.3).
//
// One thread writes (the CEF audio callback), one thread reads (the PipeWire or
// WASAPI callback). Neither side ever blocks, allocates, or makes a syscall, so
// both are safe to call from a real-time audio thread.
//
// Both sides work in two steps: reserve a region, fill or drain it in place,
// then commit. Because the buffer wraps around, a region can be split in two:
// `first` runs to the end of storage and `second` continues from the start.
//
//   Producer:                              Consumer:
//     auto w = buf.reserve_write(n);         auto r = buf.reserve_read(n);
//     copy into w.first, then w.second       copy out of r.first, then r.second
//     buf.commit_write(w.frames_reserved);   buf.commit_read(r.frames_reserved);
//
// Calling a producer method from two threads at once (or a consumer method from
// two threads at once) is a data race. The class does not detect it.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <span>

namespace am::common {

inline constexpr std::size_t kCacheLineSize = 64;

template <typename SampleT, std::size_t FrameCapacity, std::size_t Channels = 2>
    requires((FrameCapacity != 0) && ((FrameCapacity & (FrameCapacity - 1)) == 0) &&
             (Channels != 0))
class alignas(kCacheLineSize) LockFreeSpscAudioBuffer {
  public:
    static constexpr std::size_t kFrameCapacity = FrameCapacity;
    static constexpr std::size_t kChannels = Channels;
    static constexpr std::size_t kSampleCapacity = FrameCapacity * Channels;
    static constexpr std::size_t kIndexMask = FrameCapacity - 1;

    static_assert(std::atomic<std::size_t>::is_always_lock_free,
                  "frame indices must be lock-free atomics");

    struct WriteReservation {
        std::span<SampleT> first;
        std::span<SampleT> second;
        std::size_t frames_reserved;
    };

    struct ReadReservation {
        std::span<const SampleT> first;
        std::span<const SampleT> second;
        std::size_t frames_reserved;
    };

    // --- Producer side -----------------------------------------------------

    // Reserves up to `requested_frames` of free space. May return fewer frames
    // (or zero) when the buffer is nearly full. Nothing becomes visible to the
    // consumer until commit_write().
    [[nodiscard]] WriteReservation reserve_write(std::size_t requested_frames) noexcept {
        const std::size_t write = write_frame_idx_.load(std::memory_order_relaxed);
        std::size_t free_frames = FrameCapacity - (write - cached_read_frame_idx_);

        // Only touch the consumer's cache line when the stale copy says we're short.
        if (free_frames < requested_frames) {
            cached_read_frame_idx_ = read_frame_idx_.load(std::memory_order_acquire);
            free_frames = FrameCapacity - (write - cached_read_frame_idx_);
        }

        const std::size_t to_write = std::min(requested_frames, free_frames);
        if (to_write == 0) {
            return {{}, {}, 0};
        }

        const std::size_t start_frame = write & kIndexMask;
        const std::size_t first_frames = std::min(to_write, FrameCapacity - start_frame);
        const std::size_t second_frames = to_write - first_frames;

        return {
            std::span<SampleT>(&storage_[start_frame * Channels], first_frames * Channels),
            std::span<SampleT>(&storage_[0], second_frames * Channels),
            to_write,
        };
    }

    // Publishes `written_frames` to the consumer. Must not exceed the
    // frames_reserved of the last reserve_write().
    void commit_write(std::size_t written_frames) noexcept {
        const std::size_t next = write_frame_idx_.load(std::memory_order_relaxed) + written_frames;
        write_frame_idx_.store(next, std::memory_order_release);
    }

    // --- Consumer side -----------------------------------------------------

    // Number of committed frames waiting to be read.
    [[nodiscard]] std::size_t available_read_frames() noexcept {
        const std::size_t read = read_frame_idx_.load(std::memory_order_relaxed);
        cached_write_frame_idx_ = write_frame_idx_.load(std::memory_order_acquire);
        return cached_write_frame_idx_ - read;
    }

    // Reserves up to `requested_frames` of committed audio for reading in place.
    // May return fewer frames (or zero) when the buffer is nearly empty.
    [[nodiscard]] ReadReservation reserve_read(std::size_t requested_frames) noexcept {
        const std::size_t read = read_frame_idx_.load(std::memory_order_relaxed);
        std::size_t ready_frames = cached_write_frame_idx_ - read;

        // Only touch the producer's cache line when the stale copy says we're short.
        if (ready_frames < requested_frames) {
            cached_write_frame_idx_ = write_frame_idx_.load(std::memory_order_acquire);
            ready_frames = cached_write_frame_idx_ - read;
        }

        const std::size_t to_read = std::min(requested_frames, ready_frames);
        if (to_read == 0) {
            return {{}, {}, 0};
        }

        const std::size_t start_frame = read & kIndexMask;
        const std::size_t first_frames = std::min(to_read, FrameCapacity - start_frame);
        const std::size_t second_frames = to_read - first_frames;

        return {
            std::span<const SampleT>(&storage_[start_frame * Channels], first_frames * Channels),
            std::span<const SampleT>(&storage_[0], second_frames * Channels),
            to_read,
        };
    }

    // Hands `consumed_frames` back to the producer. Must not exceed the
    // frames_reserved of the last reserve_read() (or available_read_frames()).
    void commit_read(std::size_t consumed_frames) noexcept {
        const std::size_t next = read_frame_idx_.load(std::memory_order_relaxed) + consumed_frames;
        read_frame_idx_.store(next, std::memory_order_release);
    }

  private:
    // Indices count frames since construction and are never masked; they wrap
    // at SIZE_MAX, and because FrameCapacity is a power of two the unsigned
    // subtraction `write - read` stays correct across that wrap.

    // Producer's cache line. The consumer only reads write_frame_idx_ when it
    // has run out of data, so the line rarely bounces between cores.
    alignas(kCacheLineSize) std::atomic<std::size_t> write_frame_idx_{0};
    std::size_t cached_read_frame_idx_{0};

    // Consumer's cache line, mirrored.
    alignas(kCacheLineSize) std::atomic<std::size_t> read_frame_idx_{0};
    std::size_t cached_write_frame_idx_{0};

    // The daemon pins this with mlock()/VirtualLock() through the PAL; this
    // header stays OS-agnostic.
    alignas(kCacheLineSize) SampleT storage_[kSampleCapacity]{};
};

} // namespace am::common
