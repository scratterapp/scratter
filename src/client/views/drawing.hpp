#pragma once

// Drawing the views share: the record in the background, the small app name
// over the title, and the round buttons, chips and small capitals.

#include "client/theme.hpp"

#include <imgui.h>

#include <cstdint>

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

enum class Icon : std::uint8_t { Play, Pause, Stop };

// A round button with an icon drawn on it. ImGui's own buttons are text in a
// box; a player wants big round ones. It's an InvisibleButton underneath, so
// clicks, keyboard focus (Tab) and the tooltip all work like any other item.
// `primary` fills it with the accent color. Returns true when clicked.
bool round_button(const char* id, const char* tooltip, Icon icon, float radius, bool primary,
                  bool enabled);

// A pill-shaped button, filled with the accent color when `lit`.
bool chip(const char* label, bool lit);

// A word in small bold capitals, like the "VOL" label beside the volume.
void small_caps(const Fonts& fonts, const char* text, ImVec4 color);

} // namespace am::client
