#pragma once

// The words and numbers the now-playing view (views/now_playing.cpp) shows,
// kept apart from the drawing so tests can check them without a window
// (tests/test_now_playing_text.cpp).

#include "client/ui_state.hpp"
#include "common/telemetry_shm.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

namespace am::client {

// How much one press of up or down changes the volume.
inline constexpr int kVolumeStep = 5;

// 83'000'000 microseconds -> "1:23". An hour or more gets hours too: "1:02:03".
inline std::string format_clock(std::uint64_t micros) {
    const std::uint64_t total = micros / 1'000'000;
    const auto hours = static_cast<unsigned long long>(total / 3600);
    const auto minutes = static_cast<unsigned long long>(total / 60 % 60);
    const auto seconds = static_cast<unsigned long long>(total % 60);
    char text[32];
    if (hours > 0) {
        std::snprintf(text, sizeof text, "%llu:%02llu:%02llu", hours, minutes, seconds);
    } else {
        std::snprintf(text, sizeof text, "%llu:%02llu", minutes, seconds);
    }
    return text;
}

// The volume after `steps` presses of up (positive) or down (negative),
// kept between 0 and 100.
inline std::uint8_t step_volume(std::uint8_t current, int steps) {
    return static_cast<std::uint8_t>(std::clamp(current + steps * kVolumeStep, 0, 100));
}

inline const char* playback_word(common::PlaybackState state) {
    switch (state) {
    case common::PlaybackState::Playing:
        return "Playing";
    case common::PlaybackState::Paused:
        return "Paused";
    case common::PlaybackState::Buffering:
        return "Buffering";
    case common::PlaybackState::Stopped:
        break;
    }
    return "Stopped";
}

// "Couldn't pause: the daemon can't do that yet." for a command that didn't
// work, or an empty string for one that did.
inline std::string command_failure_text(const CommandResult& result) {
    const char* verb = "do that";
    switch (result.command) {
    case Command::Play:
        verb = "play";
        break;
    case Command::Pause:
        verb = "pause";
        break;
    case Command::Stop:
        verb = "stop";
        break;
    case Command::Toggle:
        verb = "play or pause";
        break;
    case Command::SetVolume:
        verb = "change the volume";
        break;
    }
    const char* why = nullptr;
    switch (result.outcome) {
    case CommandOutcome::Ok:
        return {};
    case CommandOutcome::BadRequest:
        why = "the daemon didn't understand the request";
        break;
    case CommandOutcome::NotSupported:
        why = "the daemon can't do that yet";
        break;
    case CommandOutcome::Failed:
        why = "the daemon tried and it went wrong";
        break;
    case CommandOutcome::NotConnected:
        why = "the daemon isn't running";
        break;
    case CommandOutcome::ConnectionLost:
        why = "the connection dropped before the daemon answered";
        break;
    case CommandOutcome::TimedOut:
        why = "the daemon didn't answer in time";
        break;
    }
    return std::string{"Couldn't "} + verb + ": " + why + ".";
}

// The fold-away line for hand testing: "48000 Hz · buffer 52% · drift +3.1 ppm".
inline std::string telemetry_line(const common::TelemetrySnapshot& snapshot) {
    char text[96];
    std::snprintf(text, sizeof text, "%u Hz  \xC2\xB7  buffer %.0f%%  \xC2\xB7  drift %+.1f ppm",
                  snapshot.sample_rate_hz,
                  static_cast<double>(snapshot.ring_buffer_fill_pct) * 100.0,
                  static_cast<double>(snapshot.clock_drift_ppm));
    return text;
}

} // namespace am::client
