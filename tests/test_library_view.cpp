// The library view (src/client/views/library_view.cpp) drawn without a
// window: Dear ImGui runs headless with its built-in font, the mouse is
// moved and clicked by hand, and nothing is rendered. A layout mistake that
// trips one of ImGui's own checks (an unbalanced push and pop, say) aborts the
// test, and the clicks check the view moves where it should. The link points
// at a daemon that isn't there, so the player bar's commands go nowhere.

#include "client/daemon_link.hpp"
#include "client/sample_library.hpp"
#include "client/ui_state.hpp"
#include "client/views/library_view.hpp"

#include <catch2/catch_test_macros.hpp>

#include <imgui.h>

#include <cstddef>
#include <memory>
#include <set>

using am::client::LibraryView;
using am::client::Screen;

namespace {

// An ImGui context for one test, at a window size of `size`, drawing
// nothing. ImGui 1.92 makes font textures on demand; claiming the renderer
// handles that is enough, since nothing is ever rendered.
class Headless {
  public:
    explicit Headless(ImVec2 size) {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = size;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.Fonts->AddFontDefault();
    }
    Headless(const Headless&) = delete;
    Headless& operator=(const Headless&) = delete;
    ~Headless() { ImGui::DestroyContext(); }
};

// What the view needs around it: a link to no daemon, a telemetry reader
// that never maps, and a daemon state as if one played the tone.
struct Scene {
    Scene() {
        state.status = am::client::LinkStatus::Connected;
        state.has_playback = true;
        state.playback = am::common::PlaybackState::Playing;
        state.volume_percent = 40;
        state.has_track = true;
        state.track_title = "440 Hz test tone";
        state.track_duration_micros = 263'000'000;
        state.position_micros = 83'000'000;
        am::client::DaemonLinkOptions options;
        options.name = "no_daemon_lv";
        auto started = am::client::DaemonLink::start(std::move(options), [] {});
        REQUIRE(started);
        link = std::move(*started);
    }

    void frame() {
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        view.draw(fonts, state, telemetry, *link, {}, screen);
        ImGui::Render();
    }

    void click(ImVec2 at) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(at.x, at.y);
        frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        frame();
    }

    am::client::Fonts fonts; // all null: the built-in font at each size
    am::client::DaemonState state;
    am::client::TelemetryReader telemetry{"no_daemon_lv"};
    std::unique_ptr<am::client::DaemonLink> link;
    LibraryView view{am::client::sample::kPlaylists};
    Screen screen = Screen::Library;
};

} // namespace

TEST_CASE("the library view draws every sample playlist at the usual and smallest sizes") {
    for (const ImVec2 size : {ImVec2{960.0f, 600.0f}, ImVec2{640.0f, 400.0f}}) {
        const Headless imgui{size};
        Scene scene;
        // The mouse over every part of the window, so hovers and tooltips
        // are drawn too, on each playlist in turn (picked from the side bar).
        for (float y = 4.0f; y < size.y; y += 23.0f) {
            for (float x = 4.0f; x < size.x; x += 61.0f) {
                ImGui::GetIO().AddMousePosEvent(x, y);
                scene.frame();
            }
            scene.click({40.0f, y});
        }
        CHECK(scene.screen != Screen::Library); // "Now playing" was clicked on the way
    }
}

TEST_CASE("the side bar opens each playlist, and Now playing goes back") {
    const Headless imgui{{960.0f, 600.0f}};
    Scene scene;
    scene.frame();
    std::set<std::size_t> opened;
    bool went_back = false;
    for (float y = 0.0f; y < 500.0f; y += 6.0f) {
        scene.click({80.0f, y});
        opened.insert(scene.view.open_playlist());
        if (scene.screen == Screen::NowPlaying) {
            went_back = true;
            scene.screen = Screen::Library;
        }
    }
    CHECK(went_back);
    CHECK(opened.size() == am::client::sample::kPlaylists.size());
}

TEST_CASE("clicking a song doesn't leave the library") {
    const Headless imgui{{960.0f, 600.0f}};
    Scene scene;
    scene.frame();
    scene.click({600.0f, 300.0f});
    CHECK(scene.screen == Screen::Library);
}

TEST_CASE("the player bar's title goes back to now playing") {
    const Headless imgui{{960.0f, 600.0f}};
    Scene scene;
    scene.frame();
    // The bar's bottom-left corner, over the small cover and the title.
    scene.click({60.0f, 560.0f});
    CHECK(scene.screen == Screen::NowPlaying);
}
