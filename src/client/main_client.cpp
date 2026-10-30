// am-ui: the app window. It connects to the daemon in the background
// (daemon_link.hpp) and shows what's playing, with play/pause, stop and volume
// (views/now_playing.cpp), or a waiting screen while the daemon isn't there.
// Closing it only disconnects: the music keeps playing in the daemon. SDL3
// makes the window and reads input, Dear ImGui decides what goes in it, and
// SDL_Renderer draws it with whatever the machine has (OpenGL or Vulkan on
// Linux, Direct3D on Windows), so there is no platform code here.
//
//   am-ui              open the window
//   am-ui --frames N   draw N frames as fast as possible, then quit. The tests
//                      use it to check that start-up and shutdown are clean.

#include "client/daemon_link.hpp"
#include "client/frame_pacer.hpp"
#include "client/theme.hpp"
#include "client/ui_state.hpp"
#include "client/views/now_playing.hpp"
#include "client/views/waiting_view.hpp"
#include "common/app_name.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace {

constexpr int kDefaultWidth = 960;
constexpr int kDefaultHeight = 600;

struct Options {
    // Set by --frames: draw this many frames, then quit.
    std::optional<int> frame_limit;
};

std::optional<Options> parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--frames" && i + 1 < argc) {
            const std::string_view value = argv[++i];
            int frames = 0;
            const auto [end, error] =
                std::from_chars(value.data(), value.data() + value.size(), frames);
            if (error != std::errc{} || end != value.data() + value.size() || frames < 1) {
                std::fprintf(stderr, "am-ui: --frames needs a whole number above 0\n");
                return std::nullopt;
            }
            options.frame_limit = frames;
        } else {
            std::fprintf(stderr, "am-ui: unknown option '%s' (try --frames N)\n", argv[i]);
            return std::nullopt;
        }
    }
    return options;
}

// "scratter" -> "Scratter", for the title bar.
std::string window_title() {
    std::string title{am::common::kAppName};
    if (!title.empty() && title[0] >= 'a' && title[0] <= 'z') {
        title[0] = static_cast<char>(title[0] - 'a' + 'A');
    }
    return title;
}

// Owning pointers that call SDL's destroy functions when they go out of scope,
// so every early return below cleans up what was made before it.
using WindowPtr = std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)>;
using RendererPtr = std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)>;

// SDL_Init and SDL_Quit as a pair: SDL_Quit runs when this goes out of scope,
// after the window and renderer declared below it are gone.
struct SdlSession {
    SdlSession() = default;
    SdlSession(const SdlSession&) = delete;
    SdlSession& operator=(const SdlSession&) = delete;
    ~SdlSession() { SDL_Quit(); }
};

// The same for ImGui: makes its context, and on the way out shuts down
// whichever of its two backends started (they must go before the renderer they
// draw with) and then the context.
struct ImGuiSession {
    ImGuiSession() { ImGui::CreateContext(); }
    ImGuiSession(const ImGuiSession&) = delete;
    ImGuiSession& operator=(const ImGuiSession&) = delete;
    ~ImGuiSession() {
        if (renderer_backend_started) {
            ImGui_ImplSDLRenderer3_Shutdown();
        }
        if (platform_backend_started) {
            ImGui_ImplSDL3_Shutdown();
        }
        ImGui::DestroyContext();
    }

    bool platform_backend_started = false;
    bool renderer_backend_started = false;
};

// Space and up/down belong to the player (space plays or pauses, up and down
// change the volume), except while the keyboard is moving between buttons:
// after Tab, ImGui shows a highlight on the focused item and these keys work
// it (space presses the button, arrows move or nudge the slider) like in any
// other app. A mouse click or Escape hides the highlight again. Returns true
// when the key was the player's, so ImGui doesn't see it too.
bool take_player_key(const SDL_KeyboardEvent& key, am::client::PlayerKeys& keys) {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.NavVisible || io.WantTextInput || (key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT)) != 0) {
        return false;
    }
    switch (key.key) {
    case SDLK_SPACE:
        // Holding space down shouldn't flicker between play and pause.
        keys.toggle = keys.toggle || !key.repeat;
        return true;
    case SDLK_UP:
        ++keys.volume_steps;
        return true;
    case SDLK_DOWN:
        --keys.volume_steps;
        return true;
    default:
        return false;
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::optional<Options> options = parse_options(argc, argv);
    if (!options) {
        return 2;
    }

    const std::string title = window_title();
    const std::string app_id{am::common::kAppName};
    // The app id is how Wayland compositors and taskbars group the window and
    // find its .desktop file (#41 adds scratter.desktop). Set before SDL_Init.
    SDL_SetAppMetadata(title.c_str(), nullptr, app_id.c_str());
    SDL_SetHint(SDL_HINT_APP_ID, app_id.c_str());

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "am-ui: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    const SdlSession sdl;

    // On a high-DPI screen the content scale is above 1 (1.5 or 2), and both
    // the window and ImGui's sizes are multiplied by it so text isn't tiny.
    float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (scale <= 0.0f) {
        scale = 1.0f;
    }
    constexpr SDL_WindowFlags kWindowFlags =
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
    const WindowPtr window{
        SDL_CreateWindow(title.c_str(), static_cast<int>(static_cast<float>(kDefaultWidth) * scale),
                         static_cast<int>(static_cast<float>(kDefaultHeight) * scale),
                         kWindowFlags),
        &SDL_DestroyWindow};
    if (!window) {
        std::fprintf(stderr, "am-ui: SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }

    // nullptr lets SDL pick the best renderer this machine has.
    const RendererPtr renderer{SDL_CreateRenderer(window.get(), nullptr), &SDL_DestroyRenderer};
    if (!renderer) {
        std::fprintf(stderr, "am-ui: SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return 1;
    }
    // Present at the screen's refresh rate at most. Not every renderer can
    // (the software one in the tests can't), and that's fine.
    SDL_SetRenderVSync(renderer.get(), 1);
    SDL_SetWindowMinimumSize(window.get(), static_cast<int>(640.0f * scale),
                             static_cast<int>(400.0f * scale));
    SDL_ShowWindow(window.get());

    IMGUI_CHECKVERSION();
    ImGuiSession imgui;
    ImGuiIO& io = ImGui::GetIO();
    // ImGui would otherwise write imgui.ini into whatever folder am-ui was
    // started from. Where settings live is decided later.
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    am::client::apply_style(scale);
    const am::client::Fonts fonts = am::client::load_fonts();
    imgui.platform_backend_started =
        ImGui_ImplSDL3_InitForSDLRenderer(window.get(), renderer.get());
    imgui.renderer_backend_started =
        imgui.platform_backend_started && ImGui_ImplSDLRenderer3_Init(renderer.get());
    if (!imgui.renderer_backend_started) {
        std::fprintf(stderr, "am-ui: starting Dear ImGui's SDL backends failed\n");
        return 1;
    }

    // The connection to the daemon runs on its own thread. When what it knows
    // changes (a pause from the media keys, the daemon going away) it posts
    // this event, which wakes the loop below from SDL_WaitEventTimeout, so
    // the window can sleep while nothing happens and still follow at once.
    // SDL_PushEvent is safe from any thread. The link is declared after `sdl`,
    // so it stops before SDL_Quit runs.
    const Uint32 wake_event = SDL_RegisterEvents(1);
    auto link = am::client::DaemonLink::start({}, [wake_event] {
        if (wake_event != 0) {
            SDL_Event event{};
            event.type = wake_event;
            SDL_PushEvent(&event);
        }
    });
    if (!link) {
        std::fprintf(stderr, "am-ui: can't start the daemon connection: %s\n",
                     link.error().message().c_str());
        return 1;
    }
    am::client::TelemetryReader telemetry{std::string{am::pal::kDaemonEndpoint}};
    am::client::NowPlayingView now_playing;

    am::client::FramePacer pacer;
    pacer.set_animating(options->frame_limit.has_value());
    int frames_drawn = 0;
    bool quit = false;

    while (!quit) {
        // Sleep in SDL_WaitEventTimeout until there is an event or the pacer
        // wants a frame. This wait is why an idle window costs almost no CPU.
        const std::int32_t wait = pacer.wait_ms(SDL_GetTicks());
        SDL_Event event;
        bool have_event = wait == 0 ? SDL_PollEvent(&event) : SDL_WaitEventTimeout(&event, wait);
        am::client::PlayerKeys keys;
        while (have_event) {
            const bool players_key =
                event.type == SDL_EVENT_KEY_DOWN && take_player_key(event.key, keys);
            if (!players_key) {
                ImGui_ImplSDL3_ProcessEvent(&event);
            }
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                quit = true;
            }
            pacer.on_event();
            have_event = SDL_PollEvent(&event);
        }
        if (quit) {
            break;
        }

        // A copy, so the connection thread can carry on while this frame is
        // drawn from a state that doesn't change under it.
        const am::client::DaemonState state = (*link)->state();
        // Connected and the daemon has said what it's doing. Between the two
        // the waiting view stays up, so the window never shows a made-up
        // "stopped, volume 0" for a frame.
        const bool connected =
            state.status == am::client::LinkStatus::Connected && state.has_playback;
        telemetry.update(state);
        // The clock on screen moves once a second while music plays.
        pacer.set_ticking(connected && state.playback == am::common::PlaybackState::Playing);

        constexpr SDL_WindowFlags kNotVisible =
            SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN | SDL_WINDOW_OCCLUDED;
        pacer.set_hidden((SDL_GetWindowFlags(window.get()) & kNotVisible) != 0);
        if (!pacer.should_draw(SDL_GetTicks())) {
            continue;
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        if (connected) {
            now_playing.draw(fonts, state, telemetry, **link, keys);
        } else {
            am::client::draw_waiting_view(fonts, state);
        }
        ImGui::Render();

        SDL_SetRenderScale(renderer.get(), io.DisplayFramebufferScale.x,
                           io.DisplayFramebufferScale.y);
        constexpr ImVec4 kClear = am::client::palette::kBackground;
        SDL_SetRenderDrawColorFloat(renderer.get(), kClear.x, kClear.y, kClear.z, kClear.w);
        SDL_RenderClear(renderer.get());
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer.get());
        SDL_RenderPresent(renderer.get());
        pacer.on_frame_drawn(SDL_GetTicks());

        ++frames_drawn;
        if (options->frame_limit && frames_drawn >= *options->frame_limit) {
            quit = true;
        }
    }
    return 0;
}
