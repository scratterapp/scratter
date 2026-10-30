#include "client/views/waiting_view.hpp"

#include "client/views/drawing.hpp"
#include "common/app_name.hpp"

#include <string>

namespace am::client {

namespace {

struct Message {
    std::string headline;
    std::string detail;
};

// What to say for each state of the connection. The link retries by itself in
// every case, so none of them asks the user to do anything in this window.
Message message_for(const DaemonState& state) {
    const std::string daemon{am::common::kDaemonName};
    const std::string how_to_start = "Start it in a terminal with " + daemon + ", or " + daemon +
                                     " --clear-film for the test film. This window finds it "
                                     "by itself.";
    switch (state.status) {
    case LinkStatus::Connecting:
        return {"Saying hello to " + daemon, "It's there; waiting for its answer."};
    case LinkStatus::Failed:
        return {"Can't reach " + daemon,
                "Trying again every second. The error was: " + state.error.message() + "."};
    case LinkStatus::Connected: // the now-playing view shows this one
    case LinkStatus::NotRunning:
        break;
    }
    if (state.connection_count > 0) {
        return {daemon + " stopped", "The music stopped with it. " + how_to_start};
    }
    return {"Waiting for " + daemon, "The music plays in " + daemon +
                                         ", so this window can close and open again without "
                                         "stopping it. " +
                                         how_to_start};
}

} // namespace

void draw_waiting_view(const Fonts& fonts, const DaemonState& state) {
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
    draw_app_label(fonts);

    ImGui::SetCursorPosX(margin);
    ImGui::PushFont(fonts.display, kBodySize * 4.2f);
    ImGui::TextUnformatted("Nothing playing");
    ImGui::PopFont();

    const Message message = message_for(state);

    // A hollow dot: not connected.
    ImGui::SetCursorPosX(margin);
    ImGui::PushFont(fonts.display_italic, kBodySize * 1.6f);
    const ImVec2 line = ImGui::GetCursorScreenPos();
    const float line_height = ImGui::GetTextLineHeight();
    const float dot = line_height * 0.16f;
    ImGui::GetWindowDrawList()->AddCircle({line.x + dot, line.y + line_height * 0.55f}, dot,
                                          to_u32(kTextDim), 24, 1.5f);
    ImGui::SetCursorPosX(margin + dot * 2.0f + em * 0.6f);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
    ImGui::TextUnformatted(message.headline.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();

    ImGui::Dummy({0.0f, em * 0.6f});
    ImGui::SetCursorPosX(margin);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
    ImGui::PushTextWrapPos(margin + em * 22.0f);
    ImGui::TextUnformatted(message.detail.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();

    ImGui::End();
}

} // namespace am::client
