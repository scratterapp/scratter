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

// The stretch of the page's -96 to 0 dB the strip shows, bottom to top. Music
// doesn't use the whole range: on the clear film the loudest band averages
// about -32 dB, the middle band 20 dB below that, and the top bands -65 to
// -75. Drawn over all 96 dB, those differences came out as a fifth of the
// strip, and every bar looked about as tall as the next. Over these 60 dB a
// typical peak is about half the strip and a quiet band sits at the bottom.
// Loud masters (pop on Apple Music) run about 10 dB hotter than the film,
// which is the room left at the top. To check again with real tracks.
inline constexpr float kSpectrumBottomDb = -66.0f;
inline constexpr float kSpectrumTopDb = -6.0f;

// How much of the strip's height a band at `db` fills, 0 to 1. Linear in dB,
// which is how loudness is heard; anything outside the range is clamped to it.
[[nodiscard]] constexpr float bar_fraction(float db) noexcept {
    return std::clamp((db - kSpectrumBottomDb) / (kSpectrumTopDb - kSpectrumBottomDb), 0.0f, 1.0f);
}

static_assert(bar_fraction(kSpectrumBottomDb) == 0.0f);
static_assert(bar_fraction(-96.0f) == 0.0f); // the page's floor: silence
static_assert(bar_fraction(-36.0f) == 0.5f);
static_assert(bar_fraction(kSpectrumTopDb) == 1.0f);
static_assert(bar_fraction(0.0f) == 1.0f);

// Draws the bars between `min` and `max` (screen coordinates), bottom-aligned.
// `alpha` dims them, for a telemetry page that's gone stale.
void draw_spectrum(ImDrawList& draw, ImVec2 min, ImVec2 max,
                   const std::array<float, common::kFftBandCount>& bands_db, float alpha = 1.0f);

} // namespace am::client
