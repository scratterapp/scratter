#pragma once

// How am-ui looks: its colors, spacing and fonts, in one place so the views
// don't each pick their own. The idea is a late-night listening room: a warm
// near-black background, paper-colored text, and one loud color (vermilion,
// like a record label) kept for the thing that matters on screen. Body text is
// Atkinson Hyperlegible, made for readability; titles use Instrument Serif.
// docs/learning/app-window.md has the reasoning.

#include <imgui.h>

namespace am::client {

namespace palette {

inline constexpr ImVec4 kBackground{0.078f, 0.067f, 0.059f, 1.0f}; // #14110F
inline constexpr ImVec4 kSurface{0.118f, 0.102f, 0.090f, 1.0f};    // #1E1A17
inline constexpr ImVec4 kRaised{0.165f, 0.141f, 0.125f, 1.0f};     // #2A2420
inline constexpr ImVec4 kText{0.937f, 0.906f, 0.863f, 1.0f};       // #EFE7DC
inline constexpr ImVec4 kTextDim{0.604f, 0.561f, 0.518f, 1.0f};    // #9A8F84
inline constexpr ImVec4 kAccent{1.0f, 0.357f, 0.180f, 1.0f};       // #FF5B2E
inline constexpr ImVec4 kAccentHover{1.0f, 0.478f, 0.333f, 1.0f};  // #FF7A55

} // namespace palette

// The fonts, added to ImGui by load_fonts(). Sizes are picked when a font is
// pushed (ImGui::PushFont(font, size)); kBodySize is the default.
struct Fonts {
    ImFont* body = nullptr;
    ImFont* body_bold = nullptr;
    ImFont* display = nullptr;
    ImFont* display_italic = nullptr;
};

inline constexpr float kBodySize = 17.0f;

// Adds the embedded fonts to the current ImGui context. Call once, after
// ImGui::CreateContext(). A font that fails to load is left as ImGui's own
// built-in font, so the window still opens.
Fonts load_fonts();

// Sets ImGui's colors and spacing. `scale` is the screen's content scale
// (1 on a normal screen, 2 on a high-DPI one).
void apply_style(float scale);

} // namespace am::client
