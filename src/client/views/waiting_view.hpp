#pragma once

// What the window shows before there is anything to play: a title, a line
// about the daemon, and a record drawn in the background. The now-playing
// view (#37) takes over once the daemon connection (#36) exists.

#include "client/theme.hpp"

namespace am::client {

// Fills the whole SDL window. Call between ImGui::NewFrame() and ImGui::Render().
void draw_waiting_view(const Fonts& fonts);

} // namespace am::client
