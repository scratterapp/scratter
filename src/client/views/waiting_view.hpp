#pragma once

// What the window shows while it isn't connected to the daemon: a title, a
// line saying what the connection is doing and how to start the daemon, and a
// record drawn in the background. The daemon link keeps retrying on its own
// thread meanwhile, and the now-playing view takes over once it's connected.

#include "client/theme.hpp"
#include "client/ui_state.hpp"

namespace am::client {

// Fills the whole SDL window. Call between ImGui::NewFrame() and ImGui::Render().
void draw_waiting_view(const Fonts& fonts, const DaemonState& state);

} // namespace am::client
