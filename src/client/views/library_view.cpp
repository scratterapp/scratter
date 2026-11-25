#include "client/views/library_view.hpp"

#include "client/now_playing_text.hpp"
#include "client/views/drawing.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace am::client {

namespace {

using common::PlaybackState;

// `text` at `pos` in the font pushed now, cut short with "…" to fit `width`.
// Returns true when it had to be cut, so the caller can offer the whole of it
// in a tooltip.
bool draw_fitted(ImDrawList& draw, ImVec2 pos, std::string_view text, float width, ImU32 color) {
    const std::string shown = elide(text, width, [](std::string_view t) {
        return ImGui::CalcTextSize(t.data(), t.data() + t.size()).x;
    });
    draw.AddText(ImGui::GetFont(), ImGui::GetFontSize(), pos, color, shown.data(),
                 shown.data() + shown.size());
    return shown.size() != text.size();
}

// Stand-in artwork until real covers come with the catalog: a square in
// warm gradients with a few record grooves, a different pair of colors for
// each playlist so they're told apart at a glance.
void draw_cover(ImDrawList& draw, ImVec2 corner, float side, std::size_t which) {
    struct Colors {
        ImVec4 light;
        ImVec4 dark;
    };
    constexpr std::array kColors{
        Colors{palette::kAccent, {0.12f, 0.05f, 0.04f, 1.0f}},
        Colors{{0.85f, 0.62f, 0.32f, 1.0f}, {0.18f, 0.10f, 0.05f, 1.0f}},
        Colors{{0.42f, 0.56f, 0.48f, 1.0f}, {0.06f, 0.10f, 0.08f, 1.0f}},
        Colors{{0.28f, 0.42f, 0.56f, 1.0f}, {0.04f, 0.06f, 0.09f, 1.0f}},
        Colors{{0.62f, 0.38f, 0.58f, 1.0f}, {0.09f, 0.05f, 0.09f, 1.0f}},
    };
    const Colors& c = kColors[which % kColors.size()];
    const ImVec2 far{corner.x + side, corner.y + side};
    draw.AddRectFilledMultiColor(corner, far, to_u32(c.light), to_u32(c.dark, 0.9f), to_u32(c.dark),
                                 to_u32(c.light, 0.85f));
    const ImVec2 center{corner.x + side * 0.68f, corner.y + side * 0.62f};
    for (float r = side * 0.12f; r < side * 0.36f; r += std::max(2.0f, side * 0.025f)) {
        draw.AddCircle(center, r, to_u32(palette::kText, 0.10f), 48, 1.0f);
    }
    draw.AddCircleFilled(center, side * 0.08f, to_u32(palette::kBackground, 0.8f), 32);
}

// One line in the side bar, lit with the accent edge when `open`. Returns
// true when clicked.
bool side_bar_item(std::string_view label, bool open) {
    using namespace palette;
    const float em = ImGui::GetFontSize();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const ImVec2 size{ImGui::GetContentRegionAvail().x, em * 1.7f};
    const bool pressed = ImGui::InvisibleButton("##item", size, ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList& draw = *ImGui::GetWindowDrawList();
    const ImVec2 far{at.x + size.x, at.y + size.y};
    if (open || hovered) {
        draw.AddRectFilled(at, far, to_u32(kRaised, open ? 1.0f : 0.5f), em * 0.4f);
    }
    if (open) {
        draw.AddRectFilled(at, {at.x + 3.0f, far.y}, to_u32(kAccent));
    }
    const float pad = em * 0.6f;
    if (draw_fitted(draw, {at.x + pad, at.y + (size.y - em) * 0.5f}, label, size.x - pad * 2.0f,
                    to_u32(open || hovered ? kText : kTextDim))) {
        ImGui::SetItemTooltip("%.*s", static_cast<int>(label.size()), label.data());
    }
    return pressed;
}

} // namespace

void LibraryView::draw(const Fonts& fonts, const DaemonState& state,
                       const TelemetryReader& telemetry, DaemonLink& link, PlayerKeys keys,
                       Screen& screen) {
    // The player's keys work here too: space plays or pauses, up and down
    // change the volume.
    if (keys.toggle) {
        link.toggle();
    }
    if (keys.volume_steps != 0) {
        link.set_volume(step_volume(state.volume_percent, keys.volume_steps));
    }
    if (playlists_.empty()) {
        open_ = 0;
    } else {
        open_ = std::min(open_, playlists_.size() - 1);
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::Begin("library", nullptr, kFlags);
    ImGui::PopStyleVar();

    const float em = ImGui::GetFontSize();
    const ImVec2 size = ImGui::GetWindowSize();
    const float bar_height = em * 4.6f;
    const float side_width = std::clamp(size.x * 0.24f, em * 10.0f, em * 15.0f);
    const float upper_height = std::max(0.0f, size.y - bar_height);

    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {em * 0.6f, em * 0.25f});

    // Side bar, in the raised surface color (ImGuiCol_ChildBg).
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {em * 0.8f, em * 1.5f});
    ImGui::BeginChild("##side", {side_width, upper_height}, ImGuiChildFlags_AlwaysUseWindowPadding);
    draw_side_bar(fonts, screen);
    ImGui::EndChild();
    ImGui::PopStyleVar();

    // The open playlist, on the plain background, scrolling on its own.
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {em * 2.2f, em * 2.0f});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, palette::kBackground);
    if (scroll_to_top_) {
        ImGui::SetNextWindowScroll({0.0f, 0.0f});
        scroll_to_top_ = false;
    }
    ImGui::BeginChild("##playlist", {0.0f, upper_height}, ImGuiChildFlags_AlwaysUseWindowPadding);
    draw_playlist(fonts);
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    ImGui::PopStyleVar(2);

    draw_player_bar(fonts, state, telemetry, link, screen);
    ImGui::End();
}

void LibraryView::draw_side_bar(const Fonts& fonts, Screen& screen) {
    using namespace palette;
    const float em = ImGui::GetFontSize();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + em * 0.6f);
    draw_app_label(fonts);
    ImGui::Dummy({0.0f, em * 0.8f});

    ImGui::PushID("now playing");
    if (side_bar_item("Now playing", false)) {
        screen = Screen::NowPlaying;
    }
    ImGui::PopID();

    ImGui::Dummy({0.0f, em * 0.8f});
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + em * 0.6f);
    small_caps(fonts, "PLAYLISTS", kTextDim);
    ImGui::Dummy({0.0f, em * 0.1f});
    for (std::size_t i = 0; i < playlists_.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (side_bar_item(playlists_[i].name, i == open_) && i != open_) {
            open_ = i;
            picked_.reset();
            scroll_to_top_ = true;
        }
        ImGui::PopID();
    }
}

void LibraryView::draw_playlist(const Fonts& fonts) {
    using namespace palette;
    if (playlists_.empty()) {
        ImGui::TextUnformatted("No playlists yet");
        return;
    }
    const Playlist& playlist = playlists_[open_];
    const float em = ImGui::GetFontSize();
    ImDrawList& draw = *ImGui::GetWindowDrawList();
    const float left = ImGui::GetCursorScreenPos().x;
    const float right = left + ImGui::GetContentRegionAvail().x;

    // The header: cover, name, who made it and how long it is, Play and Shuffle.
    const float cover = std::clamp((right - left) * 0.22f, em * 5.0f, em * 9.0f);
    const ImVec2 cover_at = ImGui::GetCursorScreenPos();
    draw_cover(draw, cover_at, cover, open_);
    ImGui::Dummy({cover, cover});
    ImGui::SameLine(0.0f, em * 1.6f);
    ImGui::SetCursorScreenPos({ImGui::GetCursorScreenPos().x, cover_at.y + em * 0.4f});
    ImGui::BeginGroup();
    small_caps(fonts, "PLAYLIST", kTextDim);
    const float caps_top = ImGui::GetItemRectMin().y;
    ImGui::SameLine(0.0f, em * 0.8f);
    ImGui::SetCursorScreenPos({ImGui::GetCursorScreenPos().x, caps_top});
    small_caps(fonts, "SAMPLE DATA", kAccent);
    ImGui::SetItemTooltip("Made-up songs, until Scratter can fetch your real library");

    const std::string name{playlist.name};
    const float name_right = right;
    ImGui::PushFont(fonts.display, kBodySize * 3.2f);
    if (ImGui::CalcTextSize(name.c_str()).x > name_right - ImGui::GetCursorScreenPos().x) {
        ImGui::PopFont();
        ImGui::PushFont(fonts.display, kBodySize * 2.2f);
    }
    ImGui::PushTextWrapPos(name_right - ImGui::GetWindowPos().x);
    ImGui::TextUnformatted(name.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopFont();

    std::string about = playlist_summary(playlist.tracks);
    if (!playlist.curator.empty()) {
        about = std::string{playlist.curator} + "  \xC2\xB7  " + about;
    }
    ImGui::PushFont(fonts.display_italic, kBodySize * 1.3f);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
    ImGui::TextUnformatted(about.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();

    // Greyed out until the daemon can play Apple Music (#4).
    ImGui::Dummy({0.0f, em * 0.4f});
    ImGui::BeginDisabled();
    chip("Play", true);
    ImGui::EndDisabled();
    const bool play_hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    ImGui::SameLine(0.0f, em * 0.7f);
    ImGui::BeginDisabled();
    chip("Shuffle", false);
    ImGui::EndDisabled();
    if (play_hovered || ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Playing these needs Apple Music, which isn't connected yet");
    }
    ImGui::EndGroup();
    const float header_bottom = std::max(cover_at.y + cover, ImGui::GetItemRectMax().y);

    // The column heads. A narrow window drops the album column first.
    const bool album_shown = right - left > em * 26.0f;
    const float title_x = left + em * 2.4f;
    const float time_width = em * 3.2f;
    const float album_x = album_shown ? left + (right - left) * 0.58f : right - time_width;
    const float title_width = album_x - title_x - em;
    const float album_width = right - time_width - album_x - em;

    const float heads_y = header_bottom + em * 1.8f;
    auto head = [&](float x, const char* text) {
        ImGui::SetCursorScreenPos({x, heads_y});
        small_caps(fonts, text, kTextDim);
    };
    head(left, "#");
    head(title_x, "TITLE");
    if (album_shown) {
        head(album_x, "ALBUM");
    }
    ImGui::PushFont(fonts.body_bold, kBodySize * 0.72f);
    const float time_head_width = ImGui::CalcTextSize("TIME").x;
    ImGui::PopFont();
    head(right - time_head_width, "TIME");
    const float rule_y = heads_y + em * 1.3f;
    draw.AddLine({left, rule_y}, {right, rule_y}, to_u32(kRaised));
    ImGui::SetCursorScreenPos({left, rule_y + em * 0.5f});

    if (playlist.tracks.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
        ImGui::TextUnformatted("No songs in this playlist yet");
        ImGui::PopStyleColor();
        return;
    }

    // The songs. Only the rows on screen are laid out (ImGuiListClipper), so
    // a playlist of thousands costs the same per frame as one of ten.
    const float row_height = em * 2.7f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0.0f, 0.0f});
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(playlist.tracks.size()), row_height);
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const auto index = static_cast<std::size_t>(row);
            const Song& song = playlist.tracks[index];
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::PushID(row);
            if (ImGui::InvisibleButton("##song", {right - left, row_height},
                                       ImGuiButtonFlags_EnableNav)) {
                picked_ = index;
            }
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            const bool picked = picked_ == index;
            if (picked || hovered) {
                draw.AddRectFilled({left - em * 0.6f, at.y + em * 0.1f},
                                   {right + em * 0.6f, at.y + row_height - em * 0.1f},
                                   to_u32(picked ? kRaised : kSurface), em * 0.4f);
            }

            const float middle_y = at.y + (row_height - em) * 0.5f;
            const std::string number = std::to_string(index + 1);
            draw.AddText({left, middle_y}, to_u32(picked ? kAccent : kTextDim), number.c_str());

            const float title_y = at.y + row_height * 0.5f - em * 1.0f;
            bool cut = draw_fitted(draw, {title_x, title_y}, song.title, title_width,
                                   to_u32(picked ? kAccent : kText));
            ImGui::PushFont(nullptr, kBodySize * 0.82f);
            cut = draw_fitted(draw, {title_x, title_y + em * 1.15f}, song.artist, title_width,
                              to_u32(kTextDim)) ||
                  cut;
            ImGui::PopFont();
            if (album_shown) {
                cut = draw_fitted(draw, {album_x, middle_y}, song.album, album_width,
                                  to_u32(kTextDim)) ||
                      cut;
            }
            if (cut && hovered) {
                ImGui::SetTooltip("%.*s\n%.*s  \xC2\xB7  %.*s", static_cast<int>(song.title.size()),
                                  song.title.data(), static_cast<int>(song.artist.size()),
                                  song.artist.data(), static_cast<int>(song.album.size()),
                                  song.album.data());
            }

            const std::string length = format_clock(song.duration_micros);
            const float length_width = ImGui::CalcTextSize(length.c_str()).x;
            draw.AddText({right - length_width, middle_y}, to_u32(kTextDim), length.c_str());
        }
    }
    ImGui::PopStyleVar();
    ImGui::Dummy({0.0f, em});
}

void LibraryView::draw_player_bar(const Fonts& fonts, const DaemonState& state,
                                  const TelemetryReader& telemetry, DaemonLink& link,
                                  Screen& screen) {
    using namespace palette;
    const float em = ImGui::GetFontSize();
    const ImVec2 window_at = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    const float bar_height = em * 4.6f;
    const float top = window_at.y + size.y - bar_height;
    ImDrawList& draw = *ImGui::GetWindowDrawList();
    draw.AddRectFilled({window_at.x, top}, {window_at.x + size.x, window_at.y + size.y},
                       to_u32({0.055f, 0.047f, 0.043f, 1.0f}));
    draw.AddLine({window_at.x, top}, {window_at.x + size.x, top}, to_u32(kRaised));

    const bool playing =
        state.playback == PlaybackState::Playing || state.playback == PlaybackState::Buffering;

    // What plays, on the left. A click there goes back to the now-playing view.
    const float mini = bar_height - em * 1.6f;
    const ImVec2 mini_at{window_at.x + em * 1.2f, top + em * 0.8f};
    const float info_width = std::min(size.x * 0.28f, em * 16.0f);
    ImGui::SetCursorScreenPos(mini_at);
    if (ImGui::InvisibleButton("##now playing", {info_width, mini}, ImGuiButtonFlags_EnableNav)) {
        screen = Screen::NowPlaying;
    }
    ImGui::SetItemTooltip("Open the player");
    draw_cover(draw, mini_at, mini, 3);
    const float text_x = mini_at.x + mini + em * 0.9f;
    const float text_width = mini_at.x + info_width - text_x;
    const std::string_view title =
        state.has_track ? std::string_view{state.track_title} : std::string_view{"Test sound"};
    draw_fitted(draw, {text_x, top + bar_height * 0.5f - em * 1.05f}, title, text_width,
                to_u32(kText));
    ImGui::PushFont(nullptr, kBodySize * 0.82f);
    const std::string from = std::string{source_label(state.track_source)} + "  \xC2\xB7  " +
                             playback_word(state.playback);
    draw_fitted(draw, {text_x, top + bar_height * 0.5f + em * 0.15f}, from, text_width,
                to_u32(kTextDim));
    ImGui::PopFont();

    // Play/pause in the middle, with where playback is under it.
    const float center_x = window_at.x + size.x * 0.5f;
    const float radius = em * 1.05f;
    ImGui::SetCursorScreenPos({center_x - radius, top + bar_height * 0.40f - radius});
    if (round_button("##playpause", playing ? "Pause (Space)" : "Play (Space)",
                     playing ? Icon::Pause : Icon::Play, radius, true, true)) {
        if (playing) {
            link.pause();
        } else {
            link.play();
        }
    }
    const std::uint64_t position =
        telemetry.mapped() ? telemetry.snapshot().pts_micros : state.position_micros;
    const std::uint64_t length = state.has_track ? state.track_duration_micros : 0;
    const float line_y = top + bar_height * 0.80f;
    ImGui::PushFont(nullptr, kBodySize * 0.8f);
    const std::string clock = format_clock(position);
    // The bar keeps clear of the title on the left, times included.
    const float info_right = mini_at.x + info_width;
    const float width =
        std::min({size.x * 0.32f, em * 22.0f, (center_x - info_right - em * 3.6f) * 2.0f});
    if (length > 0 && width > em * 4.0f) {
        const float bar_left = center_x - width * 0.5f;
        const float fraction =
            std::clamp(static_cast<float>(position) / static_cast<float>(length), 0.0f, 1.0f);
        draw.AddRectFilled({bar_left, line_y - 1.5f}, {bar_left + width, line_y + 1.5f},
                           to_u32(kRaised), 2.0f);
        draw.AddRectFilled({bar_left, line_y - 1.5f}, {bar_left + width * fraction, line_y + 1.5f},
                           to_u32(kAccent), 2.0f);
        const float text_y = line_y - ImGui::GetFontSize() * 0.5f;
        const float clock_width = ImGui::CalcTextSize(clock.c_str()).x;
        draw.AddText({bar_left - em * 0.8f - clock_width, text_y}, to_u32(kTextDim), clock.c_str());
        const std::string total = format_clock(length);
        draw.AddText({bar_left + width + em * 0.8f, text_y}, to_u32(kTextDim), total.c_str());
    } else {
        const float clock_width = ImGui::CalcTextSize(clock.c_str()).x;
        draw.AddText({center_x - clock_width * 0.5f, line_y - ImGui::GetFontSize() * 0.5f},
                     to_u32(kTextDim), clock.c_str());
    }
    ImGui::PopFont();

    // The volume on the right, when there's room for it beside the middle.
    const float slider_width = em * 6.5f;
    const float slider_x = window_at.x + size.x - em * 1.5f - slider_width;
    if (slider_x - em * 3.0f > center_x + em * 13.0f) {
        const float middle_y = top + bar_height * 0.5f;
        ImGui::SetCursorScreenPos({slider_x - em * 2.6f, middle_y - em * 0.36f});
        small_caps(fonts, "VOL", kTextDim);
        if (!slider_held_) {
            slider_volume_ = state.volume_percent;
        }
        ImGui::SetCursorScreenPos({slider_x, middle_y - ImGui::GetFrameHeight() * 0.5f});
        ImGui::SetNextItemWidth(slider_width);
        if (ImGui::SliderInt("##volume", &slider_volume_, 0, 100, "%d%%",
                             ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput)) {
            link.set_volume(static_cast<std::uint8_t>(slider_volume_));
        }
        slider_held_ = ImGui::IsItemActive();
        ImGui::SetItemTooltip("Volume (Up and Down)");
    }
}

} // namespace am::client
