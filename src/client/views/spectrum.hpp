#pragma once

// The spectrum strip (issue #40): 64 bars along the bottom of the now-playing
// view, one per band of the daemon's spectrum (scratterd works them out in
// src/daemon/audio_engine/spectrum.hpp and publishes them in the telemetry
// page). The view turns it on and off with its Spectrum chip; while it's on,
// main_client.cpp draws at the screen's refresh rate so the bars move.

#include "common/telemetry_shm.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>

namespace am::client {

// The page's range: -96 dB is silence, 0 dB a full-scale sine.
inline constexpr float kSpectrumFloorDb = -96.0f;

// How much of the strip's height a band at `db` fills, 0 to 1. Linear in dB,
// which is how loudness is heard; anything outside the page's range is
// clamped to it.
[[nodiscard]] constexpr float bar_fraction(float db) noexcept {
    return std::clamp((db - kSpectrumFloorDb) / -kSpectrumFloorDb, 0.0f, 1.0f);
}

static_assert(bar_fraction(kSpectrumFloorDb) == 0.0f);
static_assert(bar_fraction(-200.0f) == 0.0f);
static_assert(bar_fraction(-48.0f) == 0.5f);
static_assert(bar_fraction(0.0f) == 1.0f);
static_assert(bar_fraction(6.0f) == 1.0f);

// Draws the bars between `min` and `max` (screen coordinates), bottom-aligned.
// `alpha` dims them, for a telemetry page that's gone stale.
void draw_spectrum(ImDrawList& draw, ImVec2 min, ImVec2 max,
                   const std::array<float, common::kFftBandCount>& bands_db, float alpha = 1.0f);

} // namespace am::client
