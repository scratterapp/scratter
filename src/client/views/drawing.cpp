#include "client/views/drawing.hpp"

#include "common/app_name.hpp"

#include <algorithm>
#include <string>

namespace am::client {

ImU32 to_u32(ImVec4 c, float alpha) {
    return ImGui::GetColorU32(ImVec4{c.x, c.y, c.z, c.w * alpha});
}

// It's drawn with circles only: about 50 shapes, redrawn only when the frame
// is (see FramePacer), so it costs nothing while idle.
void draw_record(ImDrawList& draw, ImVec2 window_pos, ImVec2 window_size, float label_alpha) {
    using namespace palette;
    const float radius = window_size.y * 0.62f;
    const ImVec2 center{window_pos.x + window_size.x * 0.80f, window_pos.y + window_size.y * 0.56f};

    // The vinyl, a shade darker than the background.
    draw.AddCircleFilled(center, radius, to_u32({0.043f, 0.035f, 0.031f, 1.0f}), 128);

    // Grooves: thin rings, every other one a touch brighter. Where light
    // would catch them (two short arcs on opposite sides), each ring gets a
    // brighter stretch on top.
    constexpr float kPi = 3.14159265f;
    const float groove_gap = std::max(4.0f, radius / 60.0f);
    int ring = 0;
    for (float r = radius * 0.40f; r < radius * 0.97f; r += groove_gap, ++ring) {
        const float alpha = (ring % 2 == 0) ? 0.035f : 0.06f;
        draw.AddCircle(center, r, to_u32(kText, alpha), 128, 1.0f);
        for (const float start : {-0.78f * kPi, 0.22f * kPi}) {
            draw.PathArcTo(center, r, start, start + 0.16f * kPi, 24);
            draw.PathStroke(to_u32(kText, alpha * 1.6f), 1.0f);
        }
    }

    // The label, in the accent color, and the spindle hole.
    draw.AddCircleFilled(center, radius * 0.34f, to_u32(kAccent, label_alpha), 96);
    draw.AddCircle(center, radius * 0.25f, to_u32(kBackground, 0.25f), 96, 1.5f);
    draw.AddCircleFilled(center, std::max(3.0f, radius * 0.022f), to_u32(kBackground), 32);
}

void draw_app_label(const Fonts& fonts) {
    std::string label{am::common::kAppName};
    std::ranges::transform(label, label.begin(), [](char c) {
        return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
    });
    ImGui::PushFont(fonts.body_bold, kBodySize * 0.82f);
    ImGui::PushStyleColor(ImGuiCol_Text, palette::kAccent);
    ImGui::TextUnformatted(label.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

} // namespace am::client
