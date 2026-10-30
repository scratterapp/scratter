#pragma once

// The now-playing view (issue #37): what's playing, where playback is, and
// play/pause, stop and volume. Everything amctl can do, with buttons.
//
// It never waits on the daemon. Buttons hand a command to the DaemonLink,
// which returns at once, and the screen follows the daemon's own state the
// next frame the link wakes the window for. So a pause from the media keys
// or playerctl shows up here the same way as one from this window.

#include "client/daemon_link.hpp"
#include "client/theme.hpp"
#include "client/ui_state.hpp"

#include <cstdint>
#include <optional>

namespace am::client {

// The keys main_client.cpp caught this frame for the player (space and up/down).
struct PlayerKeys {
    bool toggle = false;
    int volume_steps = 0; // +1 per press of up, -1 per press of down
};

class NowPlayingView {
  public:
    // Fills the whole SDL window. Call between ImGui::NewFrame() and
    // ImGui::Render(), only while `state.status` is Connected.
    void draw(const Fonts& fonts, const DaemonState& state, const TelemetryReader& telemetry,
              DaemonLink& link, PlayerKeys keys);

  private:
    // The volume this window last asked for, until the daemon answers. Up
    // pressed three times quickly should go up three steps, not three times
    // one step from the old value the daemon hasn't changed yet.
    std::optional<std::uint8_t> asked_volume_;
    RequestId asked_volume_id_ = 0;

    // The slider's own value while it's being dragged. The volume is only
    // sent when it's let go, not on every pixel of the drag.
    int slider_volume_ = 0;
    bool slider_held_ = false;
};

} // namespace am::client
