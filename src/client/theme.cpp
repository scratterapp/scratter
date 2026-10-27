#include "client/theme.hpp"

#include "client/fonts/embedded_fonts.hpp"

#include <span>

namespace am::client {

namespace {

ImFont* add_font(std::span<const unsigned char> bytes) {
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig config;
    // The bytes live in the program for its whole run, so ImGui must not
    // free them (it would by default).
    config.FontDataOwnedByAtlas = false;
    // ImGui wants a non-const pointer because by default it owns the data,
    // but with the line above it only reads it.
    ImFont* font =
        io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bytes.data()),
                                       static_cast<int>(bytes.size()), kBodySize, &config);
    return font != nullptr ? font : io.Fonts->AddFontDefault();
}

} // namespace

Fonts load_fonts() {
    Fonts fonts;
    // The first font added is ImGui's default, so body text comes first.
    fonts.body = add_font(embedded::atkinson_regular());
    fonts.body_bold = add_font(embedded::atkinson_bold());
    fonts.display = add_font(embedded::instrument_serif_regular());
    fonts.display_italic = add_font(embedded::instrument_serif_italic());
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
