#pragma once

// Shared-memory telemetry page (spec section 3.2.2).
//
// The daemon publishes playback status (position, buffer fill, FFT spectrum)
// up to 120 times a second. am-ui maps the same page read-only and reads it
// once per frame. A sequence lock (SeqLock) keeps the reader from ever seeing a
// half-written snapshot without the writer ever waiting on the reader.
//
//   Writer (one thread only):     Reader (any number, read-only), once per frame:
//     page.write(snapshot);         if (auto s = page.try_read()) { last = *s; }
//
// How it works: `seq_` is even while the payload is stable and odd while a
// write is in progress. The reader notes `seq_`, copies the payload, and checks
// `seq_` again. If it was odd, or changed, the copy may be torn and is retried.
//
// The payload is stored as atomic 64-bit words, not as a plain struct.
// Copying a plain struct while another thread writes it is a data race
// (undefined behavior) even if the reader throws the torn copy away, and
// ThreadSanitizer reports it. Atomic words make every access well-defined,
// and on x86-64 they compile to the same plain loads and stores.
//
// Calling write() from two threads at once is a data race. The class does not
// detect it.

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

namespace am::common {

inline constexpr std::size_t kTelemetryPageSize = 4096;
inline constexpr std::size_t kFftBandCount = 64;

enum class PlaybackState : std::uint8_t {
    Stopped = 0,
    Playing = 1,
    Paused = 2,
    Buffering = 3,
};

struct alignas(64) TelemetrySnapshot {
    std::uint64_t track_catalog_id{0}; // Active Apple Music catalog ID
    std::uint64_t pts_micros{0};       // Playback cursor in microseconds
    std::uint64_t duration_micros{0};  // Track duration in microseconds
    float ring_buffer_fill_pct{0.0f};  // SPSC fill ratio [0.0, 1.0]
    float clock_drift_ppm{0.0f};       // PI resampler compensation in ppm
    std::uint32_t sample_rate_hz{0};   // Active sink rate, e.g. 48000
    PlaybackState playback_state{PlaybackState::Stopped};
    std::uint8_t exclusive_mode_active{0}; // 1 if WASAPI exclusive / direct ALSA
    std::uint8_t volume_percent{0};        // Output volume, 0 to 100
    std::uint8_t reserved_pad{0};
    std::array<float, kFftBandCount> fft_magnitude_db{}; // [-96.0, 0.0] dB per band
    // The fields above take 296 bytes. Spelling out the rest of the cache line
    // means the struct has no compiler-inserted padding, so every byte that
    // crosses the page has a defined value.
    std::array<std::uint8_t, 24> reserved_tail{};
};

static_assert(sizeof(TelemetrySnapshot) == 320, "TelemetrySnapshot layout changed");
static_assert(offsetof(TelemetrySnapshot, fft_magnitude_db) == 40,
              "unexpected padding before fft_magnitude_db");
static_assert(offsetof(TelemetrySnapshot, reserved_tail) == 296,
              "unexpected padding before reserved_tail");
static_assert(std::is_trivially_copyable_v<TelemetrySnapshot>);

// The shared page itself: a sequence counter on its own cache line, then the
// snapshot stored as 40 atomic 64-bit words.
class alignas(64) TelemetryPage {
  public:
    static constexpr std::size_t kWordCount = sizeof(TelemetrySnapshot) / sizeof(std::uint64_t);
    using Words = std::array<std::uint64_t, kWordCount>;

    // Lock-free atomics are also address-free, which is what lets two processes
    // share them through a memory mapping.
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                  "the telemetry page needs lock-free 64-bit atomics");

    // --- Writer side (one thread) ------------------------------------------

    // Publishes `snapshot`. Never blocks, allocates, or makes a syscall, so it
    // is safe to call from the real-time audio thread.
    void write(const TelemetrySnapshot& snapshot) noexcept {
        const std::uint64_t seq = seq_.load(std::memory_order_relaxed);
        seq_.store(seq + 1, std::memory_order_relaxed); // odd: write in progress

        // Release on each word keeps the odd store above from sinking below any
        // of them. A reader that sees one of these words is then guaranteed to
        // see the odd (or a later) counter on its second check.
        const auto words = std::bit_cast<Words>(snapshot);
        for (std::size_t i = 0; i < kWordCount; ++i) {
            payload_[i].store(words[i], std::memory_order_release);
        }

        seq_.store(seq + 2, std::memory_order_release); // even: stable again
    }

    // --- Reader side (any thread, read-only memory is fine) ----------------

    // One attempt. Returns nothing if a write was in progress or happened
    // during the copy.
    [[nodiscard]] std::optional<TelemetrySnapshot> try_read() const noexcept {
        const std::uint64_t before = seq_.load(std::memory_order_acquire);
        if ((before & 1) != 0) {
            return std::nullopt;
        }

        // Acquire on each word keeps the second counter load below from
        // floating up above any of them.
        Words words;
        for (std::size_t i = 0; i < kWordCount; ++i) {
            words[i] = payload_[i].load(std::memory_order_acquire);
        }

        const std::uint64_t after = seq_.load(std::memory_order_relaxed);
        if (after != before) {
            return std::nullopt;
        }
        return std::bit_cast<TelemetrySnapshot>(words);
    }

    // Retries until it gets a consistent copy. Only for tests: if the daemon
    // dies mid-write the counter stays odd and this never returns. am-ui should
    // call try_read() once per frame and keep the last good snapshot.
    [[nodiscard]] TelemetrySnapshot read() const noexcept {
        for (;;) {
            if (auto snapshot = try_read()) {
                return *snapshot;
            }
        }
    }

  private:
    // Readers poll the counter, so it gets its own cache line.
    std::atomic<std::uint64_t> seq_{0};
    alignas(64) std::array<std::atomic<std::uint64_t>, kWordCount> payload_{};
};

static_assert(sizeof(TelemetryPage) == 384, "counter line + 320-byte payload");
static_assert(sizeof(TelemetryPage) <= kTelemetryPageSize, "telemetry must fit in one page");

} // namespace am::common
