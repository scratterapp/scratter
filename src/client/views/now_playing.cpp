#include "client/views/now_playing.hpp"

#include "client/now_playing_text.hpp"
#include "client/views/drawing.hpp"
#include "common/app_name.hpp"

#include <algorithm>
#include <string>

namespace am::client {

namespace {

using common::PlaybackState;

enum class Icon : std::uint8_t { Play, Pause, Stop };

// A round button with an icon drawn on it. ImGui's own buttons are text in a
// box; a player wants big round ones. It's an InvisibleButton underneath, so
// clicks, keyboard focus (Tab) and the tooltip all work like any other item.
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

// A word in small spaced capitals, like the "VOL" label in the mockup.
void small_caps(const Fonts& fonts, const char* text, ImVec4 color) {
    ImGui::PushFont(fonts.body_bold, kBodySize * 0.72f);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

} // namespace

void NowPlayingView::draw(const Fonts& fonts, const DaemonState& state,
                          const TelemetryReader& telemetry, DaemonLink& link, PlayerKeys keys) {
    using namespace palette;
    const std::string daemon =
        state.daemon_name.empty() ? std::string{am::common::kDaemonName} : state.daemon_name;
    const bool playing =
        state.playback == PlaybackState::Playing || state.playback == PlaybackState::Buffering;
    const common::TelemetrySnapshot& snapshot = telemetry.snapshot();

    // A new connection: answers from before it (say "couldn't pause: the
    // daemon isn't running" from while it was down) no longer apply.
    if (state.connection_count != seen_connection_) {
        seen_connection_ = state.connection_count;
        results_after_ = state.last_result ? state.last_result->id : 0;
    }
    const bool fresh_result = state.last_result && state.last_result->id > results_after_;

    // The volume to show: what this window asked for, until the daemon's state
    // shows it. The daemon acks a request before it sends the new state, so
    // letting go at the ack would flick the slider back for a frame. Also let
    // go when that request failed (the daemon won't change), or when a newer
    // answer arrived (request ids only go up, and the state event for ours
    // came before it).
    if (asked_volume_) {
        const bool answered = fresh_result && state.last_result->id >= asked_volume_id_;
        const bool failed = answered && state.last_result->id == asked_volume_id_ &&
                            state.last_result->outcome != CommandOutcome::Ok;
        const bool newer = answered && state.last_result->id > asked_volume_id_;
        if (state.volume_percent == *asked_volume_ || failed || newer) {
            asked_volume_.reset();
        }
    }
    const std::uint8_t volume = asked_volume_.value_or(state.volume_percent);
    auto ask_volume = [&](std::uint8_t percent) {
        asked_volume_ = percent;
        asked_volume_id_ = link.set_volume(percent);
    };

    if (keys.toggle) {
        link.toggle();
    }
    if (keys.volume_steps != 0) {
        ask_volume(step_volume(volume, keys.volume_steps));
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("now playing", nullptr, kFlags);

    const ImVec2 size = ImGui::GetWindowSize();
    // The label is lit while music plays and dims when it doesn't.
    draw_record(*ImGui::GetWindowDrawList(), ImGui::GetWindowPos(), size, playing ? 0.92f : 0.35f);

    const float em = ImGui::GetFontSize();
    const float margin = em * 3.0f;
    ImGui::SetCursorPos({margin, size.y * 0.16f});
    draw_app_label(fonts);

    // What's playing. The daemon doesn't say yet (there's no message for it
    // until Apple Music arrives, #38), and for now it's always a test sound.
    ImGui::SetCursorPosX(margin);
    ImGui::PushFont(fonts.display, kBodySize * 3.6f);
    ImGui::TextUnformatted("Test sound");
    ImGui::PopFont();
    ImGui::SetCursorPosX(margin);
    ImGui::PushFont(fonts.display_italic, kBodySize * 1.4f);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
    const std::string from = "the tone or the test film, from " + daemon;
    ImGui::TextUnformatted(from.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();

    // Where playback is, from the telemetry page, which the daemon updates
    // many times a second. The window only redraws once a second while
    // playing (FramePacer), which is all an m:ss clock needs. Greyed out when
    // the page is stale.
    ImGui::Dummy({0.0f, em * 0.8f});
    ImGui::SetCursorPosX(margin);
    const std::uint64_t position = telemetry.mapped() ? snapshot.pts_micros : state.position_micros;
    ImGui::PushFont(fonts.display, kBodySize * 2.6f);
    ImGui::PushStyleColor(ImGuiCol_Text, telemetry.fresh() ? kText : kTextDim);
    const std::string clock = format_clock(position);
    ImGui::TextUnformatted(clock.c_str());
    ImGui::PopStyleColor();
    const float clock_bottom = ImGui::GetItemRectMax().y;
    ImGui::PopFont();
    ImGui::SameLine(0.0f, em * 0.8f);
    // Sit the word on the clock's baseline rather than its top.
    ImGui::SetCursorScreenPos(
        {ImGui::GetCursorScreenPos().x, clock_bottom - ImGui::GetTextLineHeight() * 1.45f});
    small_caps(fonts, playback_word(state.playback), playing ? kAccent : kTextDim);

    if (snapshot.duration_micros > 0) {
        // A track with a known length gets a bar. The tone and the test film
        // don't report one, so this waits for real tracks.
        const float width = em * 20.0f;
        const float fraction =
            std::clamp(static_cast<float>(position) / static_cast<float>(snapshot.duration_micros),
                       0.0f, 1.0f);
        ImGui::SetCursorPosX(margin);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImDrawList& draw = *ImGui::GetWindowDrawList();
        draw.AddRectFilled({at.x, at.y + em * 0.4f}, {at.x + width, at.y + em * 0.55f},
                           to_u32(kRaised), 2.0f);
        draw.AddRectFilled({at.x, at.y + em * 0.4f}, {at.x + width * fraction, at.y + em * 0.55f},
                           to_u32(kAccent), 2.0f);
        ImGui::Dummy({width, em});
    }

    // The controls: play/pause, stop, then the volume.
    ImGui::Dummy({0.0f, em * 0.6f});
    ImGui::SetCursorPosX(margin);
    const float big = em * 1.7f;
    const float small = em * 1.2f;
    const float row_top = ImGui::GetCursorPosY();
    if (round_button("##playpause", playing ? "Pause (Space)" : "Play (Space)",
                     playing ? Icon::Pause : Icon::Play, big, true, true)) {
        if (playing) {
            link.pause();
        } else {
            link.play();
        }
    }
    ImGui::SameLine(0.0f, em * 0.9f);
    ImGui::SetCursorPosY(row_top + big - small);
    if (round_button("##stop", "Stop", Icon::Stop, small, false,
                     state.playback != PlaybackState::Stopped)) {
        link.stop();
    }

    ImGui::SameLine(0.0f, em * 2.2f);
    ImGui::SetCursorPosY(row_top + big - em * 0.45f);
    small_caps(fonts, "VOL", kTextDim);
    ImGui::SameLine(0.0f, em * 0.7f);
    ImGui::SetCursorPosY(row_top + big - ImGui::GetFrameHeight() * 0.5f);
    if (!slider_held_) {
        slider_volume_ = volume;
    }
    ImGui::SetNextItemWidth(em * 9.0f);
    ImGui::SliderInt("##volume", &slider_volume_, 0, 100, "%d%%",
                     ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput);
    slider_held_ = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        ask_volume(static_cast<std::uint8_t>(slider_volume_));
    }
    ImGui::SetItemTooltip("Volume (Up and Down)");
    ImGui::SetCursorPosY(row_top + big * 2.0f + em * 0.6f);

    // The newest answer, when it's a no. A later command that works replaces
    // it, so it doesn't stay around once things are fine again.
    if (fresh_result) {
        const std::string failure = command_failure_text(*state.last_result);
        if (!failure.empty()) {
            ImGui::SetCursorPosX(margin);
            ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
            ImGui::TextUnformatted(failure.c_str());
            ImGui::PopStyleColor();
        }
    }

    // The fold-away status line, for hand testing.
    ImGui::Dummy({0.0f, em * 0.4f});
    ImGui::SetCursorPosX(margin);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
    if (ImGui::TreeNodeEx("Details", ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                         ImGuiTreeNodeFlags_SpanLabelWidth)) {
        ImGui::SetCursorPosX(margin + em);
        const std::string connected = "Connected to " + daemon;
        ImGui::TextUnformatted(connected.c_str());
        ImGui::SetCursorPosX(margin + em);
        if (telemetry.mapped()) {
            const std::string line = telemetry_line(snapshot);
            ImGui::TextUnformatted(line.c_str());
        } else {
            ImGui::TextUnformatted("No telemetry page yet");
        }
    }
    ImGui::PopStyleColor();

    ImGui::End();
}

} // namespace am::client
