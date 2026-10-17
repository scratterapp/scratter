// Tests for LockFreeSpscAudioBuffer (src/common/spsc_ring_buffer.hpp).
//
// The single-threaded tests pin down the edge cases: empty, full, partial
// reservations and wrap-around. The stress test runs a real producer thread and
// consumer thread and checks every sample arrives bit-for-bit in order. Run it
// under linux-tsan to prove there are no data races (spec Phase 1 gate).

#include "common/spsc_ring_buffer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <span>
#include <thread>

namespace {

using am::common::LockFreeSpscAudioBuffer;

// 8 stereo frames: small enough to reason about by hand.
using SmallBuffer = LockFreeSpscAudioBuffer<float, 8, 2>;

// Writes `frames` frames whose samples are base, base+1, base+2, ...
std::size_t write_sequence(SmallBuffer& buf, std::size_t frames, float base) {
    auto w = buf.reserve_write(frames);
    float value = base;
    for (float& s : w.first) {
        s = value++;
    }
    for (float& s : w.second) {
        s = value++;
    }
    buf.commit_write(w.frames_reserved);
    return w.frames_reserved;
}

// The sample that frame `frame`, channel `channel` should hold in the stress test.
// Samples are built from the bit pattern so the check is bitwise, not approximate.
float stress_sample(std::uint64_t frame, std::size_t channel) {
    const auto bits = static_cast<std::uint32_t>(frame * 2 + channel);
    return std::bit_cast<float>(bits);
}

} // namespace

TEST_CASE("a new buffer is empty", "[spsc]") {
    auto buf = std::make_unique<SmallBuffer>();
    CHECK(buf->available_read_frames() == 0);
    CHECK(buf->reserve_read(1).frames_reserved == 0);
}

TEST_CASE("reservations are limited by free space", "[spsc]") {
    auto buf = std::make_unique<SmallBuffer>();

    SECTION("asking for more than the capacity gets the capacity") {
        auto w = buf->reserve_write(100);
        CHECK(w.frames_reserved == 8);
        CHECK(w.first.size() == 16); // 8 frames x 2 channels
        CHECK(w.second.empty());
    }

    SECTION("a full buffer has no room to write") {
        REQUIRE(write_sequence(*buf, 8, 0.0f) == 8);
        CHECK(buf->available_read_frames() == 8);
        CHECK(buf->reserve_write(1).frames_reserved == 0);
    }

    SECTION("reading frees space for the producer") {
        REQUIRE(write_sequence(*buf, 8, 0.0f) == 8);
        auto r = buf->reserve_read(3);
        REQUIRE(r.frames_reserved == 3);
        buf->commit_read(3);
        CHECK(buf->reserve_write(100).frames_reserved == 3);
    }
}

TEST_CASE("nothing is visible before commit_write", "[spsc]") {
    auto buf = std::make_unique<SmallBuffer>();
    auto w = buf->reserve_write(4);
    REQUIRE(w.frames_reserved == 4);
    CHECK(buf->available_read_frames() == 0);

    buf->commit_write(2); // committing less than reserved is allowed
    CHECK(buf->available_read_frames() == 2);
}

TEST_CASE("reads return frames in the order they were written", "[spsc]") {
    auto buf = std::make_unique<SmallBuffer>();
    REQUIRE(write_sequence(*buf, 5, 0.0f) == 5);

    auto r = buf->reserve_read(5);
    REQUIRE(r.frames_reserved == 5);
    REQUIRE(r.first.size() == 10);
    for (std::size_t i = 0; i < r.first.size(); ++i) {
        CHECK(r.first[i] == static_cast<float>(i));
    }
    buf->commit_read(5);
    CHECK(buf->available_read_frames() == 0);
}

TEST_CASE("a reservation that crosses the end is split into two spans", "[spsc]") {
    auto buf = std::make_unique<SmallBuffer>();

    // Move both indices to frame 6, two frames before the end of storage.
    REQUIRE(write_sequence(*buf, 6, 0.0f) == 6);
    buf->commit_read(buf->reserve_read(6).frames_reserved);

    // 5 frames starting at frame 6: frames 6-7 at the end, then 0-2 at the start.
    auto w = buf->reserve_write(5);
    REQUIRE(w.frames_reserved == 5);
    CHECK(w.first.size() == 4);  // 2 frames
    CHECK(w.second.size() == 6); // 3 frames
    CHECK(w.first.data() != w.second.data());

    float value = 100.0f;
    for (float& s : w.first) {
        s = value++;
    }
    for (float& s : w.second) {
        s = value++;
    }
    buf->commit_write(5);

    auto r = buf->reserve_read(5);
    REQUIRE(r.frames_reserved == 5);
    CHECK(r.first.size() == 4);
    CHECK(r.second.size() == 6);

    float expected = 100.0f;
    for (float s : r.first) {
        CHECK(s == expected++);
    }
    for (float s : r.second) {
        CHECK(s == expected++);
    }
}

TEST_CASE("the buffer keeps working after many laps", "[spsc]") {
    auto buf = std::make_unique<SmallBuffer>();
    float next_write = 0.0f;
    float next_read = 0.0f;

    // Uneven chunk sizes so the split point lands somewhere different each lap.
    for (int lap = 0; lap < 1000; ++lap) {
        const std::size_t chunk = static_cast<std::size_t>(lap % 7) + 1;
        const std::size_t written = write_sequence(*buf, chunk, next_write);
        next_write += static_cast<float>(written * 2);

        auto r = buf->reserve_read(chunk);
        for (float s : r.first) {
            REQUIRE(s == next_read++);
        }
        for (float s : r.second) {
            REQUIRE(s == next_read++);
        }
        buf->commit_read(r.frames_reserved);
    }
    CHECK(buf->available_read_frames() == 0);
}

TEST_CASE("100M frames cross two threads bit-for-bit", "[spsc][stress]") {
    // Small capacity so the indices wrap around storage hundreds of thousands
    // of times and the two threads keep bumping into full and empty.
    using StressBuffer = LockFreeSpscAudioBuffer<float, 1024, 2>;
    constexpr std::uint64_t kTotalFrames = 100'000'000;

    auto buf = std::make_unique<StressBuffer>();

    std::thread producer([&buf] {
        std::uint64_t frame = 0;
        std::size_t chunk = 1;
        while (frame < kTotalFrames) {
            const auto remaining = static_cast<std::size_t>(kTotalFrames - frame);
            auto w = buf->reserve_write(std::min(chunk, remaining));
            if (w.frames_reserved == 0) {
                std::this_thread::yield();
                continue;
            }
            for (auto span : {w.first, w.second}) {
                for (std::size_t i = 0; i < span.size(); i += 2) {
                    span[i] = stress_sample(frame, 0);
                    span[i + 1] = stress_sample(frame, 1);
                    ++frame;
                }
            }
            buf->commit_write(w.frames_reserved);
            chunk = chunk % 509 + 1; // varies 1..509, never lines up with 1024
        }
    });

    // Catch2 assertions aren't thread-safe, so the consumer runs here on the
    // test thread and only counts mismatches.
    std::uint64_t frame = 0;
    std::uint64_t mismatches = 0;
    std::size_t chunk = 1;
    while (frame < kTotalFrames) {
        auto r = buf->reserve_read(chunk);
        if (r.frames_reserved == 0) {
            std::this_thread::yield();
            continue;
        }
        for (auto span : {r.first, r.second}) {
            for (std::size_t i = 0; i < span.size(); i += 2) {
                const float left = stress_sample(frame, 0);
                const float right = stress_sample(frame, 1);
                if (std::bit_cast<std::uint32_t>(span[i]) != std::bit_cast<std::uint32_t>(left) ||
                    std::bit_cast<std::uint32_t>(span[i + 1]) !=
                        std::bit_cast<std::uint32_t>(right)) {
                    ++mismatches;
                }
                ++frame;
            }
        }
        buf->commit_read(r.frames_reserved);
        chunk = chunk % 251 + 1;
    }

    producer.join();
    CHECK(frame == kTotalFrames);
    CHECK(mismatches == 0);
    CHECK(buf->available_read_frames() == 0);
}
