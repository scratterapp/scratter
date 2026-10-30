#pragma once

// Drawing shared by the waiting and now-playing views: the record in the
// background and the small app name over the title.

#include "client/theme.hpp"

#include <imgui.h>

namespace am::client {

// A theme color as ImGui's packed form, with its alpha multiplied by `alpha`.
ImU32 to_u32(ImVec4 c, float alpha = 1.0f);

// A record half off the right edge of the window, in quiet colors so text can
// sit on top of it. `label_alpha` dims the label (the now-playing view dims it
// while nothing plays).
void draw_record(ImDrawList& draw, ImVec2 window_pos, ImVec2 window_size,
                 float label_alpha = 0.92f);

// "SCRATTER" in small bold capitals and the accent color, at the cursor.
void draw_app_label(const Fonts& fonts);

} // namespace am::client
