// Tests for TelemetryPage and TelemetrySnapshot (src/common/telemetry_shm.hpp).
//
// The single-threaded tests check the round trip. The stress test runs a writer
// thread flat out against reader threads and checks that no reader ever sees a
// snapshot mixing two writes. Run it under linux-tsan to prove there are no data
// races (spec Phase 1 gate).

#include "common/telemetry_shm.hpp"
#include "telemetry_stamp.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace {

using am::common::PlaybackState;
using am::common::TelemetryPage;
using am::common::TelemetrySnapshot;

using am::test::is_consistent;
using am::test::stamped;

} // namespace

TEST_CASE("a new page reads as all zeros", "[seqlock]") {
    auto page = std::make_unique<TelemetryPage>();

    auto s = page->try_read();
    REQUIRE(s.has_value());
    CHECK(s->track_catalog_id == 0);
    CHECK(s->playback_state == PlaybackState::Stopped);
    CHECK(is_consistent(*s));
}

TEST_CASE("a written snapshot reads back unchanged", "[seqlock]") {
    auto page = std::make_unique<TelemetryPage>();

    TelemetrySnapshot in;
    in.track_catalog_id = 1'440'857'781;
    in.pts_micros = 83'250'000;
    in.duration_micros = 241'000'000;
    in.ring_buffer_fill_pct = 0.5f;
    in.clock_drift_ppm = -12.25f;
    in.sample_rate_hz = 48'000;
    in.playback_state = PlaybackState::Playing;
    in.exclusive_mode_active = 1;
    in.volume_percent = 65;
    for (std::size_t i = 0; i < in.fft_magnitude_db.size(); ++i) {
        in.fft_magnitude_db[i] = -static_cast<float>(i);
    }
    page->write(in);

    const TelemetrySnapshot out = page->read();
    CHECK(out.track_catalog_id == in.track_catalog_id);
    CHECK(out.pts_micros == in.pts_micros);
    CHECK(out.duration_micros == in.duration_micros);
    CHECK(out.ring_buffer_fill_pct == in.ring_buffer_fill_pct);
    CHECK(out.clock_drift_ppm == in.clock_drift_ppm);
    CHECK(out.sample_rate_hz == in.sample_rate_hz);
    CHECK(out.playback_state == in.playback_state);
    CHECK(out.exclusive_mode_active == in.exclusive_mode_active);
    CHECK(out.volume_percent == in.volume_percent);
    CHECK(out.fft_magnitude_db == in.fft_magnitude_db);
}

TEST_CASE("readers work through a const reference", "[seqlock]") {
    // am-ui maps the page read-only, so the whole reader API has to be const.
    auto page = std::make_unique<TelemetryPage>();
    page->write(stamped(7));
    const TelemetryPage& view = *page;
    CHECK(view.read().track_catalog_id == 7);
    CHECK(view.try_read().has_value());
}

TEST_CASE("readers never see a torn snapshot while the writer runs flat out", "[seqlock][stress]") {
    // The real writer updates at 120 Hz. Writing back to back with no pause is
    // the worst case for readers: the counter is odd as often as possible.
    constexpr std::uint64_t kWrites = 1'000'000;
    constexpr int kReaders = 2;

    auto page = std::make_unique<TelemetryPage>();
    std::atomic<bool> done{false};

    // Catch2 assertions aren't thread-safe, so the threads only count, and the
    // checks happen on the test thread after join().
    struct ReaderStats {
        std::uint64_t reads = 0;
        std::uint64_t torn = 0;
        std::uint64_t went_backwards = 0;
        std::uint64_t last_seen = 0;
    };
    std::vector<ReaderStats> stats(kReaders);
    std::vector<std::thread> readers;
    readers.reserve(kReaders);

    for (int r = 0; r < kReaders; ++r) {
        readers.emplace_back([&page, &done, &st = stats[static_cast<std::size_t>(r)]] {
            const TelemetryPage& view = *page;
            std::uint64_t previous = 0;
            // Read at least once after the writer finishes so the final value
            // is always checked.
            bool last_pass = false;
            while (!last_pass) {
                last_pass = done.load(std::memory_order_acquire);
                const TelemetrySnapshot s = view.read();
                ++st.reads;
                if (!is_consistent(s)) {
                    ++st.torn;
                }
                if (s.track_catalog_id < previous) {
                    ++st.went_backwards;
                }
                previous = s.track_catalog_id;
            }
            st.last_seen = previous;
        });
    }

    std::thread writer([&page, &done] {
        for (std::uint64_t n = 1; n <= kWrites; ++n) {
            page->write(stamped(n));
        }
        done.store(true, std::memory_order_release);
    });

    writer.join();
    for (auto& t : readers) {
        t.join();
    }

    for (const ReaderStats& st : stats) {
        CHECK(st.reads > 0);
        CHECK(st.torn == 0);
        CHECK(st.went_backwards == 0);
        CHECK(st.last_seen == kWrites);
    }
}
