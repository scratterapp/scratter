#include "client/views/waiting_view.hpp"

#include "common/app_name.hpp"

#include <algorithm>
#include <string>

namespace am::client {

namespace {

ImU32 color(ImVec4 c, float alpha = 1.0f) {
    return ImGui::GetColorU32(ImVec4{c.x, c.y, c.z, c.w * alpha});
}

// A record half off the right edge of the window, in quiet colors so text can
// sit on top of it. It's drawn with circles only: about 50 shapes, redrawn only
// when the frame is (see FramePacer), so it costs nothing while idle.
void draw_record(ImDrawList& draw, ImVec2 window_pos, ImVec2 window_size) {
    using namespace palette;
    const float radius = window_size.y * 0.62f;
    const ImVec2 center{window_pos.x + window_size.x * 0.80f, window_pos.y + window_size.y * 0.56f};

    // The vinyl, a shade darker than the background.
    draw.AddCircleFilled(center, radius, color({0.043f, 0.035f, 0.031f, 1.0f}), 128);

    // Grooves: thin rings, every other one a touch brighter. Where light
    // would catch them (two short arcs on opposite sides), each ring gets a
    // brighter stretch on top.
    constexpr float kPi = 3.14159265f;
    const float groove_gap = std::max(4.0f, radius / 60.0f);
    int ring = 0;
    for (float r = radius * 0.40f; r < radius * 0.97f; r += groove_gap, ++ring) {
        const float alpha = (ring % 2 == 0) ? 0.035f : 0.06f;
        draw.AddCircle(center, r, color(kText, alpha), 128, 1.0f);
        for (const float start : {-0.78f * kPi, 0.22f * kPi}) {
            draw.PathArcTo(center, r, start, start + 0.16f * kPi, 24);
            draw.PathStroke(color(kText, alpha * 1.6f), 1.0f);
        }
    }

    // The label, in the accent color, and the spindle hole.
    draw.AddCircleFilled(center, radius * 0.34f, color(kAccent, 0.92f), 96);
    draw.AddCircle(center, radius * 0.25f, color(kBackground, 0.25f), 96, 1.5f);
    draw.AddCircleFilled(center, std::max(3.0f, radius * 0.022f), color(kBackground), 32);
}

} // namespace

void draw_waiting_view(const Fonts& fonts) {
    using namespace palette;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("waiting", nullptr, kFlags);

    const ImVec2 pos = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    draw_record(*ImGui::GetWindowDrawList(), pos, size);

    // Text sits on the left, a little above the middle. Sizes are in units of
    // the body font, so they grow with it on a high-DPI screen.
    const float em = ImGui::GetFontSize();
    const float margin = em * 3.0f;
    ImGui::SetCursorPos({margin, size.y * 0.30f});

    std::string label{am::common::kAppName};
    std::ranges::transform(label, label.begin(), [](char c) {
        return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
    });
    ImGui::PushFont(fonts.body_bold, kBodySize * 0.82f);
    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    ImGui::TextUnformatted(label.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();

    ImGui::SetCursorPosX(margin);
    ImGui::PushFont(fonts.display, kBodySize * 4.2f);
    ImGui::TextUnformatted("Nothing playing");
    ImGui::PopFont();

    // A hollow dot: not connected. #36 fills it in.
    ImGui::SetCursorPosX(margin);
    ImGui::PushFont(fonts.display_italic, kBodySize * 1.6f);
    const ImVec2 line = ImGui::GetCursorScreenPos();
    const float line_height = ImGui::GetTextLineHeight();
    const float dot = line_height * 0.16f;
    ImGui::GetWindowDrawList()->AddCircle({line.x + dot, line.y + line_height * 0.55f}, dot,
                                          color(kTextDim), 24, 1.5f);
    ImGui::SetCursorPosX(margin + dot * 2.0f + em * 0.6f);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
    const std::string waiting = "Waiting for " + std::string{am::common::kDaemonName};
    ImGui::TextUnformatted(waiting.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();

    ImGui::Dummy({0.0f, em * 0.6f});
    ImGui::SetCursorPosX(margin);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
    ImGui::PushTextWrapPos(margin + em * 22.0f);
    ImGui::TextUnformatted("The music plays in the daemon, so this window can close and open "
                           "again without stopping it.");
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();

    ImGui::End();
}

} // namespace am::client
