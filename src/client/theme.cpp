#include "client/theme.hpp"

#include "client/fonts/embedded_fonts.hpp"

#include <span>

namespace am::client {

namespace {

// Adds one font. With `merge`, its letters join the font added just before
// (ImFontConfig::MergeMode) instead of making a new one: ImGui looks a letter
// up in the first font, and only if that doesn't have it, in the next.
ImFont* add_font(std::span<const unsigned char> bytes, bool merge = false) {
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig config;
    config.MergeMode = merge;
    // The bytes live in the program for its whole run, so ImGui must not
    // free them (it would by default).
    config.FontDataOwnedByAtlas = false;
    // ImGui wants a non-const pointer because by default it owns the data,
    // but with the line above it only reads it.
    ImFont* font =
        io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bytes.data()),
                                       static_cast<int>(bytes.size()), kBodySize, &config);
    if (merge) {
        return font; // a missing fallback just means fewer letters, not a missing font
    }
    return font != nullptr ? font : io.Fonts->AddFontDefault();
}

// Atkinson Hyperlegible and Instrument Serif only have Latin letters. Track
// titles come in any language, so each font gets Noto behind it: Greek,
// Cyrillic, Vietnamese and the rest of Latin. ImGui (since 1.92) only draws a
// letter the first time it's shown, so the extra fonts cost the program's
// size (about 1.1 MB), not memory for letters nobody uses. Chinese, Japanese
// and Korean aren't in these; those fonts are 15 MB or more each.
ImFont* add_font_with_fallback(std::span<const unsigned char> bytes,
                               std::span<const unsigned char> fallback) {
    ImFont* font = add_font(bytes);
    add_font(fallback, true);
    return font;
}

} // namespace

Fonts load_fonts() {
    Fonts fonts;
    // The first font added is ImGui's default, so body text comes first.
    // A bold or italic letter that falls through to Noto comes out upright
    // and regular; carrying a Noto for every style would double the size.
    fonts.body = add_font_with_fallback(embedded::atkinson_regular(), embedded::noto_sans());
    fonts.body_bold = add_font_with_fallback(embedded::atkinson_bold(), embedded::noto_sans());
    fonts.display =
        add_font_with_fallback(embedded::instrument_serif_regular(), embedded::noto_serif());
    fonts.display_italic =
        add_font_with_fallback(embedded::instrument_serif_italic(), embedded::noto_serif());
    return fonts;
}

void apply_style(float scale) {
    using namespace palette;
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::StyleColorsDark(&style);

    // Roomy and soft: generous padding, rounded corners, no outlines.
    style.WindowPadding = {24.0f, 24.0f};
    style.FramePadding = {14.0f, 8.0f};
    style.ItemSpacing = {12.0f, 10.0f};
    style.ItemInnerSpacing = {8.0f, 6.0f};
    style.WindowRounding = 12.0f;
    style.ChildRounding = 10.0f;
    style.FrameRounding = 8.0f;
    style.PopupRounding = 10.0f;
    style.GrabRounding = 8.0f;
    style.ScrollbarRounding = 8.0f;
    style.ScrollbarSize = 10.0f;
    style.WindowBorderSize = 0.0f;
    style.FrameBorderSize = 0.0f;
    style.PopupBorderSize = 0.0f;
    style.FontSizeBase = kBodySize;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = kText;
    colors[ImGuiCol_TextDisabled] = kTextDim;
    colors[ImGuiCol_WindowBg] = kBackground;
    colors[ImGuiCol_ChildBg] = kSurface;
    colors[ImGuiCol_PopupBg] = kRaised;
    colors[ImGuiCol_FrameBg] = kSurface;
    colors[ImGuiCol_FrameBgHovered] = kRaised;
    colors[ImGuiCol_FrameBgActive] = kRaised;
    colors[ImGuiCol_Button] = kRaised;
    colors[ImGuiCol_ButtonHovered] = kAccentHover;
    colors[ImGuiCol_ButtonActive] = kAccent;
    colors[ImGuiCol_Header] = kRaised;
    colors[ImGuiCol_HeaderHovered] = kRaised;
    colors[ImGuiCol_HeaderActive] = kAccent;
    colors[ImGuiCol_SliderGrab] = kAccent;
    colors[ImGuiCol_SliderGrabActive] = kAccentHover;
    colors[ImGuiCol_CheckMark] = kAccent;
    colors[ImGuiCol_PlotHistogram] = kAccent;
    colors[ImGuiCol_PlotHistogramHovered] = kAccentHover;
    colors[ImGuiCol_ScrollbarBg] = kBackground;
    colors[ImGuiCol_ScrollbarGrab] = kRaised;
    colors[ImGuiCol_Separator] = kRaised;
    colors[ImGuiCol_NavCursor] = kAccent;
    colors[ImGuiCol_TextSelectedBg] = {kAccent.x, kAccent.y, kAccent.z, 0.35f};

    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
}

} // namespace am::client
