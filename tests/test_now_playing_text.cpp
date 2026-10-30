// The words and numbers on the now-playing screen
// (src/client/now_playing_text.hpp). The screen itself is drawn by the
// am_ui_opens_and_closes test and checked by hand (issue #37).

#include "client/now_playing_text.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace am::client;

TEST_CASE("the clock reads minutes and seconds, and hours after an hour", "[now_playing]") {
    CHECK(format_clock(0) == "0:00");
    CHECK(format_clock(999'999) == "0:00"); // whole seconds only, never rounded up
    CHECK(format_clock(83'000'000) == "1:23");
    CHECK(format_clock(59 * 60 * 1'000'000ULL + 59'000'000) == "59:59");
    CHECK(format_clock(3600 * 1'000'000ULL) == "1:00:00");
    CHECK(format_clock((3600 + 2 * 60 + 3) * 1'000'000ULL) == "1:02:03");
}

TEST_CASE("up and down move the volume in steps and stay between 0 and 100", "[now_playing]") {
    CHECK(step_volume(50, 1) == 55);
    CHECK(step_volume(50, -1) == 45);
    CHECK(step_volume(50, 3) == 65);
    CHECK(step_volume(98, 1) == 100);
    CHECK(step_volume(2, -1) == 0);
    CHECK(step_volume(0, -4) == 0);
    CHECK(step_volume(100, 30) == 100);
}

TEST_CASE("a command that worked shows nothing, one that didn't says why", "[now_playing]") {
    CHECK(command_failure_text({.id = 1, .command = Command::Pause, .outcome = CommandOutcome::Ok})
              .empty());
    CHECK(command_failure_text(
              {.id = 2, .command = Command::Pause, .outcome = CommandOutcome::NotSupported}) ==
          "Couldn't pause: the daemon can't do that yet.");
    CHECK(command_failure_text(
              {.id = 3, .command = Command::SetVolume, .outcome = CommandOutcome::TimedOut}) ==
          "Couldn't change the volume: the daemon didn't answer in time.");
}

TEST_CASE("each playback state has a word", "[now_playing]") {
    using am::common::PlaybackState;
    CHECK(std::string{playback_word(PlaybackState::Playing)} == "Playing");
    CHECK(std::string{playback_word(PlaybackState::Paused)} == "Paused");
    CHECK(std::string{playback_word(PlaybackState::Stopped)} == "Stopped");
    CHECK(std::string{playback_word(PlaybackState::Buffering)} == "Buffering");
}

TEST_CASE("the details line shows rate, buffer fill and drift", "[now_playing]") {
    am::common::TelemetrySnapshot snapshot{};
    snapshot.sample_rate_hz = 48000;
    snapshot.ring_buffer_fill_pct = 0.52f;
    snapshot.clock_drift_ppm = 3.14f;
    CHECK(telemetry_line(snapshot) == "48000 Hz  \xC2\xB7  buffer 52%  \xC2\xB7  drift +3.1 ppm");
}
