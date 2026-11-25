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

bool round_button(const char* id, const char* tooltip, Icon icon, float radius, bool primary,
                  bool enabled) {
    using namespace palette;
    ImGui::BeginDisabled(!enabled);
    const ImVec2 corner = ImGui::GetCursorScreenPos();
    const bool pressed =
        ImGui::InvisibleButton(id, {radius * 2.0f, radius * 2.0f}, ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered();
    ImGui::SetItemTooltip("%s", tooltip);
    ImGui::EndDisabled();

    const float alpha = enabled ? 1.0f : 0.35f;
    ImVec4 fill = primary ? kAccent : kRaised;
    if (hovered && enabled) {
        fill = primary ? kAccentHover : kSurface;
    }
    const ImU32 ink = to_u32(primary ? kBackground : kText, alpha);
    const ImVec2 c{corner.x + radius, corner.y + radius};
    const float r = radius;
    ImDrawList& draw = *ImGui::GetWindowDrawList();
    draw.AddCircleFilled(c, r, to_u32(fill, alpha), 48);
    switch (icon) {
    case Icon::Play:
        // Nudged right of center: a triangle's weight sits toward its flat side.
        draw.AddTriangleFilled({c.x - r * 0.26f, c.y - r * 0.38f},
                               {c.x - r * 0.26f, c.y + r * 0.38f}, {c.x + r * 0.40f, c.y}, ink);
        break;
    case Icon::Pause:
        draw.AddRectFilled({c.x - r * 0.32f, c.y - r * 0.38f}, {c.x - r * 0.10f, c.y + r * 0.38f},
                           ink, r * 0.04f);
        draw.AddRectFilled({c.x + r * 0.10f, c.y - r * 0.38f}, {c.x + r * 0.32f, c.y + r * 0.38f},
                           ink, r * 0.04f);
        break;
    case Icon::Stop:
        draw.AddRectFilled({c.x - r * 0.30f, c.y - r * 0.30f}, {c.x + r * 0.30f, c.y + r * 0.30f},
                           ink, r * 0.06f);
        break;
    }
    return pressed && enabled;
}

bool chip(const char* label, bool lit) {
    using namespace palette;
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ImGui::GetFrameHeight() * 0.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        {ImGui::GetFontSize() * 0.9f, ImGui::GetStyle().FramePadding.y});
    ImGui::PushStyleColor(ImGuiCol_Button, lit ? kAccent : kRaised);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, lit ? kAccentHover : kSurface);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, lit ? kAccentHover : kSurface);
    ImGui::PushStyleColor(ImGuiCol_Text, lit ? kBackground : kText);
    const bool pressed = ImGui::Button(label);
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);
    return pressed;
}

void small_caps(const Fonts& fonts, const char* text, ImVec4 color) {
    ImGui::PushFont(fonts.body_bold, kBodySize * 0.72f);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

} // namespace am::client
