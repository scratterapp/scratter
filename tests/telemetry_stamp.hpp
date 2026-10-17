#pragma once

// Test helper: a TelemetrySnapshot where every field is derived from one
// number, so a reader can tell whether all of it came from the same write.
// Floats are built from bit patterns so the check is bitwise, not approximate.

#include "common/telemetry_shm.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace am::test {

inline common::TelemetrySnapshot stamped(std::uint64_t n) {
    const auto n32 = static_cast<std::uint32_t>(n);
    common::TelemetrySnapshot s;
    s.track_catalog_id = n;
    s.pts_micros = n;
    s.duration_micros = n;
    s.ring_buffer_fill_pct = std::bit_cast<float>(n32);
    s.clock_drift_ppm = std::bit_cast<float>(n32);
    s.sample_rate_hz = n32;
    s.playback_state = static_cast<common::PlaybackState>(n & 0xFF);
    s.exclusive_mode_active = static_cast<std::uint8_t>(n);
    s.volume_percent = static_cast<std::uint8_t>(n);
    s.reserved_pad = static_cast<std::uint8_t>(n);
    for (float& band : s.fft_magnitude_db) {
        band = std::bit_cast<float>(n32);
    }
    for (std::uint8_t& b : s.reserved_tail) {
        b = static_cast<std::uint8_t>(n);
    }
    return s;
}

// True if every field of `s` agrees with track_catalog_id.
inline bool is_consistent(const common::TelemetrySnapshot& s) {
    using Bytes = std::array<std::byte, sizeof(common::TelemetrySnapshot)>;
    return std::bit_cast<Bytes>(s) == std::bit_cast<Bytes>(stamped(s.track_catalog_id));
}

} // namespace am::test
