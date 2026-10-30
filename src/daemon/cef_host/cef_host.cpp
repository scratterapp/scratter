// The real CefHost, built when the AM_WITH_CEF CMake option is on. See
// cef_host.hpp for what it does and docs/CEF.md for how CEF's
// pieces fit together.
//
// Which thread runs what:
//
//   reactor thread   start(), set_state(), ~CefHost(). Posts work to CEF's
//                    UI thread and, on shutdown, waits for it.
//   CEF UI thread    everything in PageClient and HostApp except the audio
//                    callbacks below. It's CEF's own thread because the
//                    daemon sets multi_threaded_message_loop.
//   CEF audio thread OnAudioStreamPacket(), which copies the page's audio
//                    into the ring buffer. No allocations, locks or
//                    syscalls there, the same rule as the sink's callback.

#include "daemon/cef_host/cef_host.hpp"

#include "browser_files.hpp"
#include "cef_main_args.hpp"
#include "common/app_name.hpp"
#include "daemon/cef_host/test_film_page.hpp"

#include "include/base/cef_callback.h"
#include "include/cef_app.h"
#include "include/cef_audio_handler.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_display_handler.h"
#include "include/cef_life_span_handler.h"
#include "include/cef_load_handler.h"
#include "include/cef_render_handler.h"
#include "include/cef_request_handler.h"
#include "include/cef_scheme.h"
#include "include/cef_version.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_stream_resource_handler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <span>
#include <string_view>
#include <utility>

namespace am::daemon {

namespace {

using namespace std::chrono_literals;
using common::PlaybackState;

// How long ~CefHost waits for the page to close before shutting CEF down
// anyway. Closing normally takes well under a second.
constexpr auto kCloseTimeout = 5s;

// When the page's renderer dies, the page is loaded again after a wait that
// starts at kFirstReloadDelay and doubles each time it dies again within
// kStableAfter of loading, up to kMaxReloadDelay.
constexpr auto kFirstReloadDelay = 1s;
constexpr auto kMaxReloadDelay = 60s;
constexpr auto kStableAfter = 60s;

using Clock = std::chrono::steady_clock;

const char* termination_name(cef_termination_status_t status) {
    switch (status) {
    case TS_ABNORMAL_TERMINATION:
        return "exited with an error";
    case TS_PROCESS_WAS_KILLED:
        return "was killed";
    case TS_PROCESS_CRASHED:
        return "crashed";
    case TS_PROCESS_OOM:
        return "ran out of memory";
    case TS_LAUNCH_FAILED:
        return "couldn't start";
    case TS_INTEGRITY_FAILURE:
        return "failed a code integrity check";
    default:
        return "ended";
    }
}

// Chromium on Linux finds a Widevine CDM it didn't bundle through a small
// "hint" file in its profile folder, the one its own updater writes after
// downloading Widevine. Writing it ourselves points CEF at the copy a browser
// already installed; nothing is copied or downloaded. Chromium loads the CDM
// at startup, before the sandbox closes, so this has to happen before
// CefInitialize(). Windows Chromium ignores the file; there the host passes
// the folder as a switch instead (HostApp below).
std::expected<void, std::string> point_chromium_at_widevine(const std::filesystem::path& profile,
                                                            const std::filesystem::path& cdm) {
    std::string escaped; // the folder as a JSON string
    for (char c : cdm.string()) {
        if (c == '"' || c == '\\') {
            escaped += '\\';
        }
        escaped += c;
    }

    const auto dir = profile / "WidevineCdm";
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    std::ofstream hint(dir / "latest-component-updated-widevine-cdm", std::ios::trunc);
    hint << "{\"Path\": \"" << escaped << "\"}\n";
    hint.close();
    if (error || !hint) {
        return std::unexpected("couldn't write the Widevine hint file in " + dir.string());
    }
    return {};
}

// Hands CEF the built-in page from memory when it asks for kTestFilmUrl.
class PageSchemeHandlerFactory final : public CefSchemeHandlerFactory {
  public:
    CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                                         const CefString&, CefRefPtr<CefRequest> request) override {
        const std::string url = request->GetURL().ToString();
        if (url != kTestFilmUrl && url != kClearFilmUrl) {
            return nullptr; // anything else on this host doesn't exist
        }
        // CreateForData wants a non-const pointer but only reads from it.
        auto stream = CefStreamReader::CreateForData(const_cast<char*>(kTestFilmPage.data()),
                                                     kTestFilmPage.size());
        return new CefStreamResourceHandler("text/html", stream);
    }

  private:
    IMPLEMENT_REFCOUNTING(PageSchemeHandlerFactory);
};

// The invisible browser: its audio, its console, its lifetime, bringing it
// back when its renderer dies, and the do-nothing drawing that windowless
// rendering asks for.
class PageClient final : public CefClient,
                         public CefAudioHandler,
                         public CefDisplayHandler,
                         public CefLifeSpanHandler,
                         public CefLoadHandler,
                         public CefRenderHandler,
                         public CefRequestHandler {
  public:
    PageClient(audio::AudioPath& audio, std::uint32_t sample_rate_hz)
        : audio_(audio), sample_rate_hz_(sample_rate_hz) {}

    CefRefPtr<CefAudioHandler> GetAudioHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
    CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }

    // --- Called on CEF's UI thread by HostApp and CefHost ---------------

    void open(const std::string& url) {
        CefWindowInfo window;
        window.SetAsWindowless(kNullWindowHandle); // no window at all
        CefBrowserSettings settings;
        // Redraw once a second at most (spec 3.1.1): nothing is shown, and a
        // lower rate saves CPU in Chromium's compositor.
        settings.windowless_frame_rate = 1;
        creating_ = CefBrowserHost::CreateBrowser(window, this, url, settings, nullptr, nullptr);
        if (!creating_) {
            std::fprintf(stderr, "CEF couldn't create the browser\n");
            mark_closed();
        }
    }

    // Runs amPlay(), amPause() or amStop() in the page. Remembered, so a
    // pause that arrives before the page has loaded still happens once it has
    // (OnLoadEnd below).
    void run_script(const std::string& script) {
        last_script_ = script;
        if (browser_ && loaded_) {
            browser_->GetMainFrame()->ExecuteJavaScript(script, "", 0);
        }
    }

    void close() {
        close_requested_ = true;
        if (browser_) {
            browser_->GetHost()->CloseBrowser(true);
        } else if (!creating_) {
            mark_closed(); // there was never a browser to close
        }
        // Otherwise OnAfterCreated() closes it as soon as it exists.
    }

    // --- Called on the reactor thread ------------------------------------

    bool wait_until_closed() {
        std::unique_lock lock(closed_mutex_);
        return closed_cv_.wait_for(lock, kCloseTimeout, [this] { return closed_; });
    }

    // --- CefAudioHandler ---------------------------------------------------

    // UI thread. Asks Chromium for stereo at the sink's rate, in packets the
    // size the ring buffer was planned around, so we never resample twice.
    bool GetAudioParameters(CefRefPtr<CefBrowser>, CefAudioParameters& params) override {
        params.channel_layout = CEF_CHANNEL_LAYOUT_STEREO;
        params.sample_rate = static_cast<int>(sample_rate_hz_);
        params.frames_per_buffer = static_cast<int>(audio::AudioPath::kPacketFrames);
        return true;
    }

    // Audio thread.
    void OnAudioStreamStarted(CefRefPtr<CefBrowser>, const CefAudioParameters& params,
                              int channels) override {
        channels_.store(channels, std::memory_order_relaxed);
        std::fprintf(stderr, "page audio started: %d Hz, %d channels, %d frames per packet\n",
                     params.sample_rate, channels, params.frames_per_buffer);
    }

    // Audio thread, about every 10 ms while the page plays. `data` holds one
    // array per channel ("planar"), floats from -1 to 1.
    void OnAudioStreamPacket(CefRefPtr<CefBrowser>, const float** data, int frames,
                             int64_t) override {
        const int channels = std::clamp(channels_.load(std::memory_order_relaxed), 0, 2);
        if (frames <= 0) {
            return;
        }
        audio_.write_planar(std::span<const float* const>(data, static_cast<std::size_t>(channels)),
                            static_cast<std::size_t>(frames));
    }

    // UI thread.
    void OnAudioStreamStopped(CefRefPtr<CefBrowser>) override {
        std::fprintf(stderr, "page audio stopped\n");
    }

    // UI or audio thread. The stream is over after this.
    void OnAudioStreamError(CefRefPtr<CefBrowser>, const CefString& message) override {
        std::fprintf(stderr, "page audio error: %s\n", message.ToString().c_str());
    }

    // --- CefDisplayHandler: the page's console.log lines ------------------

    bool OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t, const CefString& message,
                          const CefString&, int) override {
        std::fprintf(stderr, "page: %s\n", message.ToString().c_str());
        return true; // printed here, so CEF needn't log it too
    }

    // --- CefLifeSpanHandler ----------------------------------------------

    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
        browser_ = browser;
        creating_ = false;
        if (close_requested_) {
            browser_->GetHost()->CloseBrowser(true); // the daemon quit while it started
        }
    }

    void OnBeforeClose(CefRefPtr<CefBrowser>) override {
        browser_ = nullptr;
        mark_closed();
    }

    // --- CefLoadHandler ---------------------------------------------------

    void OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int) override {
        if (!frame->IsMain()) {
            return;
        }
        loaded_ = true;
        loaded_at_ = Clock::now();
        if (!last_script_.empty()) {
            browser->GetMainFrame()->ExecuteJavaScript(last_script_, "", 0);
        }
    }

    // --- CefRequestHandler: the renderer died ------------------------------

    // UI thread. The page's renderer process ended: it crashed, ran out of
    // memory or was killed. The browser is still there but empty, so the audio
    // would stay off for good (#60). Load the page again; it remembers where
    // the film was (test_film_page.hpp) and carries on from there. A page
    // that keeps dying soon after it loads waits longer each time, so a
    // renderer that crashes on load isn't restarted in a tight loop.
    void OnRenderProcessTerminated(CefRefPtr<CefBrowser>, TerminationStatus status, int error_code,
                                   const CefString& error_string) override {
        loaded_ = false;
        if (close_requested_) {
            return;
        }
        deaths_in_a_row_ = Clock::now() - loaded_at_ < kStableAfter ? deaths_in_a_row_ + 1 : 1;
        const auto delay =
            std::min(kFirstReloadDelay * (1 << std::min(deaths_in_a_row_ - 1, 6)), kMaxReloadDelay);
        const std::string why = error_string.ToString();
        std::fprintf(stderr,
                     "the page's renderer %s (code %d%s%s); loading the page again in %lld s\n",
                     termination_name(status), error_code, why.empty() ? "" : ", ", why.c_str(),
                     static_cast<long long>(delay.count()));
        CefPostDelayedTask(TID_UI, base::BindOnce(&PageClient::reload, CefRefPtr<PageClient>(this)),
                           std::chrono::duration_cast<std::chrono::milliseconds>(delay).count());
    }

    // --- CefRenderHandler: windowless rendering draws into nothing --------

    void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override { rect = CefRect(0, 0, 1, 1); }
    void OnPaint(CefRefPtr<CefBrowser>, PaintElementType, const RectList&, const void*, int,
                 int) override {}

  private:
    // UI thread, kFirstReloadDelay or more after the renderer died. Reloading
    // starts a new renderer.
    void reload() {
        if (browser_ && !close_requested_) {
            browser_->Reload();
        }
    }

    void mark_closed() {
        {
            std::lock_guard lock(closed_mutex_);
            closed_ = true;
        }
        closed_cv_.notify_all();
    }

    audio::AudioPath& audio_;
    const std::uint32_t sample_rate_hz_;
    std::atomic<int> channels_{2}; // written on the audio thread

    // UI thread only.
    CefRefPtr<CefBrowser> browser_;
    bool creating_ = false;
    bool close_requested_ = false;
    bool loaded_ = false;
    std::string last_script_; // the last play/pause/stop asked for
    Clock::time_point loaded_at_{};
    int deaths_in_a_row_ = 0; // renderer deaths, each soon after the page loaded

    // Set on the UI thread, waited for on the reactor thread.
    std::mutex closed_mutex_;
    std::condition_variable closed_cv_;
    bool closed_ = false;

    IMPLEMENT_REFCOUNTING(PageClient);
};

// Browser-process settings and the first page.
class HostApp final : public CefApp, public CefBrowserProcessHandler {
  public:
    HostApp(CefRefPtr<PageClient> client, std::string_view url, std::filesystem::path cdm)
        : client_(std::move(client)), url_(url), cdm_(std::move(cdm)) {}

    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }

    // Chromium command-line switches, added before CEF starts. Only for the
    // browser process (an empty process_type); Chromium passes what the
    // others need along to them.
    void OnBeforeCommandLineProcessing(const CefString& process_type,
                                       CefRefPtr<CefCommandLine> command_line) override {
        if (!process_type.empty()) {
            return;
        }
        // Nobody clicks on a headless page, so let it play without a click.
        command_line->AppendSwitchWithValue("autoplay-policy", "no-user-gesture-required");
        // Nothing is drawn, so skip the GPU.
        command_line->AppendSwitch("disable-gpu");
        command_line->AppendSwitch("disable-gpu-compositing");
        // Chromium would still start a GPU process (to composite the page in
        // software) and an audio service process. Running both on threads of
        // the daemon instead saves about 75 MB (docs/BENCHMARKS.md).
        command_line->AppendSwitch("in-process-gpu");
        // One switch for both features: a second --disable-features would
        // replace the first. PartitionAllocDanglingPtr is a check for
        // Chromium's own developers that aborts at exit when Chromium leaves a
        // dangling pointer behind during shutdown, which it does with the GPU
        // and audio in-process. Only unofficial builds of Chromium, like our
        // own CEF build, have that check; CEF's official builds don't.
        command_line->AppendSwitchWithValue("disable-features",
                                            "AudioServiceOutOfProcess,PartitionAllocDanglingPtr");
        // No built-in extensions (the PDF viewer and the like), and none of
        // Chromium's own background requests (field trials, safe-browsing
        // lists). The page's own requests aren't affected.
        command_line->AppendSwitch("disable-extensions");
        command_line->AppendSwitch("disable-background-networking");
        // One renderer process. Chromium keeps a spare one ready for the next
        // page you open, but we only ever have the one page, and an idle
        // renderer still costs about 45 MB.
        command_line->AppendSwitchWithValue("renderer-process-limit", "1");
        // Chromium's updater would download Widevine and other components on
        // its own. We only use what's already installed (spec 3.1.1).
        command_line->AppendSwitch("disable-component-update");
        // On Windows, where Chromium otherwise only registers Widevine through
        // that updater, our CEF build registers the installed CDM named here
        // (third_party/cef-patches/widevine-cdm-dir.patch). Linux uses the
        // hint file instead and ignores the switch.
        command_line->AppendSwitchWithValue("widevine-cdm-dir", CefString(cdm_.native()));
        // Don't ask the desktop keyring for a password to encrypt cookies.
        command_line->AppendSwitchWithValue("password-store", "basic");
    }

    // UI thread, once CEF is up: serve the built-in page and open it.
    void OnContextInitialized() override {
        CefRegisterSchemeHandlerFactory("https", "player.invalid", new PageSchemeHandlerFactory);
        client_->open(url_);
    }

  private:
    CefRefPtr<PageClient> client_;
    std::string url_;
    std::filesystem::path cdm_;

    IMPLEMENT_REFCOUNTING(HostApp);
};

} // namespace

struct CefHost::Impl {
    CefRefPtr<PageClient> client;
    PlaybackState state = PlaybackState::Playing; // the page starts playing by itself
};

std::expected<CefHost, std::string> CefHost::start(const CefHostOptions& options,
                                                   audio::AudioPath& audio) {
    // CEF's resource files and our helper program sit next to the daemon.
    auto exe_dir = pal::executable_dir();
    if (!exe_dir) {
        return std::unexpected("couldn't find the daemon's own folder: " +
                               exe_dir.error().message());
    }
    const auto helper = *exe_dir / AM_CEF_HELPER_NAME;
    std::error_code error;
    if (!std::filesystem::is_regular_file(helper, error)) {
        return std::unexpected("CEF's helper program is missing: " + helper.string());
    }

    auto profile = pal::browser_profile_dir();
    if (!profile) {
        return std::unexpected("couldn't create the browser profile folder: " +
                               profile.error().message());
    }

    std::filesystem::path cdm = options.widevine_dir;
    if (cdm.empty()) {
        auto found = pal::find_widevine_cdm();
        if (!found) {
            return std::unexpected("no Widevine CDM found. Install Chromium's or Chrome's "
                                   "Widevine, or point at one with --widevine-dir");
        }
        cdm = *found;
    }
    if (!std::filesystem::is_regular_file(cdm / "manifest.json", error)) {
        return std::unexpected("no Widevine CDM in " + cdm.string() + " (no manifest.json)");
    }
    if (auto pointed = point_chromium_at_widevine(*profile, cdm); !pointed) {
        return std::unexpected(pointed.error());
    }
    std::fprintf(stderr, "using the Widevine CDM in %s\n", cdm.string().c_str());

    CefSettings settings;
    // CEF runs its UI thread itself, so the daemon's reactor keeps the main
    // thread.
    settings.multi_threaded_message_loop = true;
    settings.windowless_rendering_enabled = true;
    settings.no_sandbox = !options.sandbox;
    // The daemon catches Ctrl-C and SIGTERM itself (shutdown_signal.hpp).
    // Without this, Chromium would unblock and take over those signals.
    settings.disable_signal_handlers = true;
    // CEF's subprocesses (page renderer, GPU, audio, the Widevine CDM) run
    // this small program instead of a second copy of the daemon.
    CefString(&settings.browser_subprocess_path) = helper.string();
    CefString(&settings.root_cache_path) = profile->string();
    CefString(&settings.cache_path) = profile->string();
    // CEF's own log, for when something goes wrong inside Chromium.
    CefString(&settings.log_file) = (*profile / "cef.log").string();
    settings.log_severity = LOGSEVERITY_WARNING;

    auto impl = std::make_unique<Impl>();
    impl->client = new PageClient(audio, options.sample_rate_hz);

    // CEF reads Chromium switches from the program's arguments. The daemon's
    // own aren't for it, so on Linux it only gets the program's path. Windows
    // ignores these and reads the real command line (cef_main_args.hpp).
    static std::string program = (*exe_dir / common::kDaemonName).string();
    static char* argv[] = {program.data(), nullptr};
    const CefMainArgs main_args = pal::cef_main_args(1, argv);

    if (!CefInitialize(
            main_args, settings,
            new HostApp(impl->client, options.clear_film ? kClearFilmUrl : kTestFilmUrl, cdm),
            nullptr)) {
        return std::unexpected("CEF didn't start (exit code " + std::to_string(CefGetExitCode()) +
                               "). Its log is " + (*profile / "cef.log").string());
    }
    std::fprintf(stderr, "CEF %s started (sandbox %s)\n", CEF_VERSION,
                 options.sandbox ? "on" : "off");
    return CefHost(std::move(impl));
}

CefHost::CefHost(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
CefHost::CefHost(CefHost&&) noexcept = default;
CefHost& CefHost::operator=(CefHost&&) noexcept = default;

CefHost::~CefHost() {
    if (!impl_) {
        return; // moved from
    }
    // Close the page on CEF's UI thread, wait for it to go, then shut CEF
    // down from this thread, the one that started it.
    CefPostTask(TID_UI, base::BindOnce(&PageClient::close, impl_->client));
    if (!impl_->client->wait_until_closed()) {
        std::fprintf(stderr, "the page didn't close in time; shutting CEF down anyway\n");
    }
    impl_->client = nullptr;
    CefShutdown();
}

void CefHost::set_state(PlaybackState state) {
    if (state == impl_->state) {
        return;
    }
    impl_->state = state;
    const char* script = state == PlaybackState::Playing  ? "amPlay()"
                         : state == PlaybackState::Paused ? "amPause()"
                                                          : "amStop()";
    CefPostTask(TID_UI,
                base::BindOnce(&PageClient::run_script, impl_->client, std::string(script)));
}

} // namespace am::daemon
