// Tests for the Windows media controls (src/pal/win32/smtc_controls.cpp) from
// the desktop's side, the direction test_media_controls.cpp can't check.
//
// Windows lists every program's media controls through
// GlobalSystemMediaTransportControlsSessionManager, the API behind the media
// flyout. The test finds its own session there by the title it set, checks
// what the flyout would show, and then asks that one session to play, pause
// and seek, which is what clicking the flyout's buttons does. It never touches
// another program's session, so a music player running on the same machine is
// left alone.
//
// Where there's no session manager or the session never shows up (a CI runner
// with no desktop, for example), the test prints AM_TEST_SKIPPED and ctest
// counts it as skipped, because Catch2's SKIP() needs exceptions.

#include "media_controls.hpp"
#include "reactor.hpp"

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <roapi.h>
#include <windows.foundation.collections.h>
#include <windows.foundation.h>
#include <windows.media.control.h>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>

#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace foundation = ABI::Windows::Foundation;
namespace control = ABI::Windows::Media::Control;
using am::common::PlaybackState;
using am::pal::MediaControls;
using am::pal::MediaRequest;
using am::pal::Reactor;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HStringReference;
using namespace std::chrono_literals;

constexpr auto kPaused = control::GlobalSystemMediaTransportControlsSessionPlaybackStatus_Paused;
constexpr auto kPlaying = control::GlobalSystemMediaTransportControlsSessionPlaybackStatus_Playing;

// WinRT for this thread, ended after everything declared below it is gone.
struct WinRt {
    WinRt() : ok(SUCCEEDED(::RoInitialize(RO_INIT_MULTITHREADED))) {}
    ~WinRt() {
        if (ok) {
            ::RoUninitialize();
        }
    }
    WinRt(const WinRt&) = delete;
    WinRt& operator=(const WinRt&) = delete;
    bool ok;
};

void skip(const char* why) {
    std::printf("AM_TEST_SKIPPED: %s\n", why);
    WARN(why);
}

// Waits for a WinRT async operation to finish and returns its result. Polls
// instead of registering a completion handler, which is plenty for a test.
template <typename Operation, typename Result> bool wait_for(Operation* operation, Result* result) {
    ComPtr<foundation::IAsyncInfo> info;
    if (FAILED(operation->QueryInterface(IID_PPV_ARGS(&info)))) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    AsyncStatus status = AsyncStatus::Started;
    while (SUCCEEDED(info->get_Status(&status)) && status == AsyncStatus::Started &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    return status == AsyncStatus::Completed && SUCCEEDED(operation->GetResults(result));
}

std::wstring text_of(HSTRING text) {
    UINT32 length = 0;
    const wchar_t* raw = ::WindowsGetStringRawBuffer(text, &length);
    return std::wstring(raw, length);
}

// Our session in the session manager: the one whose title is `title`.
ComPtr<control::IGlobalSystemMediaTransportControlsSession>
find_session(control::IGlobalSystemMediaTransportControlsSessionManager* manager,
             const std::wstring& title) {
    using Sessions =
        foundation::Collections::IVectorView<control::GlobalSystemMediaTransportControlsSession*>;
    ComPtr<Sessions> sessions;
    UINT32 count = 0;
    if (FAILED(manager->GetSessions(&sessions)) || FAILED(sessions->get_Size(&count))) {
        return nullptr;
    }
    for (UINT32 i = 0; i < count; ++i) {
        ComPtr<control::IGlobalSystemMediaTransportControlsSession> session;
        ComPtr<foundation::IAsyncOperation<
            control::GlobalSystemMediaTransportControlsSessionMediaProperties*>>
            fetching;
        ComPtr<control::IGlobalSystemMediaTransportControlsSessionMediaProperties> properties;
        HSTRING found = nullptr;
        if (SUCCEEDED(sessions->GetAt(i, &session)) &&
            SUCCEEDED(session->TryGetMediaPropertiesAsync(&fetching)) &&
            wait_for(fetching.Get(), properties.GetAddressOf()) && properties &&
            SUCCEEDED(properties->get_Title(&found))) {
            const bool ours = text_of(found) == title;
            ::WindowsDeleteString(found);
            if (ours) {
                return session;
            }
        }
    }
    return nullptr;
}

// The status the desktop sees for `session`, once it's `wanted` or after 3
// seconds. Changes reach it a moment after set_playback_state().
control::GlobalSystemMediaTransportControlsSessionPlaybackStatus
wait_for_status(control::IGlobalSystemMediaTransportControlsSession* session,
                control::GlobalSystemMediaTransportControlsSessionPlaybackStatus wanted) {
    control::GlobalSystemMediaTransportControlsSessionPlaybackStatus status{};
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    do {
        ComPtr<control::IGlobalSystemMediaTransportControlsSessionPlaybackInfo> playback;
        if (SUCCEEDED(session->GetPlaybackInfo(&playback))) {
            playback->get_PlaybackStatus(&status);
        }
        if (status == wanted) {
            break;
        }
        std::this_thread::sleep_for(20ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return status;
}

// Runs the reactor until a request arrives, for up to 3 seconds.
std::optional<MediaRequest> next_request(Reactor& reactor, std::vector<MediaRequest>& requests) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (requests.empty() && std::chrono::steady_clock::now() < deadline) {
        CHECK(reactor.run_once(50ms).has_value());
    }
    if (requests.empty()) {
        return std::nullopt;
    }
    MediaRequest request = requests.front();
    requests.erase(requests.begin());
    return request;
}

} // namespace

TEST_CASE("the desktop sees the track and its buttons reach the reactor thread", "[media]") {
    const WinRt winrt;
    REQUIRE(winrt.ok);

    auto reactor = Reactor::create();
    REQUIRE(reactor.has_value());
    const std::thread::id reactor_thread = std::this_thread::get_id();
    std::vector<MediaRequest> requests;
    bool on_reactor_thread = true;
    auto controls = MediaControls::open(
        *reactor,
        [&](const MediaRequest& request) {
            on_reactor_thread = on_reactor_thread && std::this_thread::get_id() == reactor_thread;
            requests.push_back(request);
        },
        "scratter_test_smtc");
    if (!controls) {
        skip("no media controls here");
        return;
    }

    // A title nobody else has, to find our session by.
    const std::string title = "SMTC test " + std::to_string(::GetCurrentProcessId());
    controls->set_track({.title = title,
                         .artist = "Test Artist",
                         .album = "Test Album",
                         // Windows fetches the image itself and leaves the thumbnail
                         // out if it won't load, so this only runs the code path.
                         .artwork_url = "https://example.com/cover.jpg",
                         .length = 3min});
    controls->set_position(30s);
    controls->set_playback_state(PlaybackState::Paused);

    ComPtr<control::IGlobalSystemMediaTransportControlsSessionManagerStatics> statics;
    ComPtr<foundation::IAsyncOperation<control::GlobalSystemMediaTransportControlsSessionManager*>>
        requesting;
    ComPtr<control::IGlobalSystemMediaTransportControlsSessionManager> manager;
    if (FAILED(::RoGetActivationFactory(
            HStringReference(
                RuntimeClass_Windows_Media_Control_GlobalSystemMediaTransportControlsSessionManager)
                .Get(),
            IID_PPV_ARGS(&statics))) ||
        FAILED(statics->RequestAsync(&requesting)) ||
        !wait_for(requesting.Get(), manager.GetAddressOf()) || !manager) {
        skip("no session manager here");
        return;
    }

    // The window thread shows the track a moment after set_track().
    const std::wstring wide_title(title.begin(), title.end());
    ComPtr<control::IGlobalSystemMediaTransportControlsSession> session;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!session && std::chrono::steady_clock::now() < deadline) {
        CHECK(reactor->run_once(100ms).has_value());
        session = find_session(manager.Get(), wide_title);
    }
    if (!session) {
        skip("our session never showed up in the session manager");
        return;
    }

    CHECK(wait_for_status(session.Get(), kPaused) == kPaused);
    {
        ComPtr<control::IGlobalSystemMediaTransportControlsSessionTimelineProperties> timeline;
        foundation::TimeSpan end{};
        REQUIRE(SUCCEEDED(session->GetTimelineProperties(&timeline)));
        REQUIRE(SUCCEEDED(timeline->get_EndTime(&end)));
        CHECK(end.Duration ==
              std::chrono::duration_cast<std::chrono::nanoseconds>(3min).count() / 100);
    }

    // Each Try*Async call is a click on the flyout's button.
    boolean accepted = false;
    ComPtr<foundation::IAsyncOperation<bool>> asking;

    REQUIRE(SUCCEEDED(session->TryPlayAsync(&asking)));
    CHECK(wait_for(asking.Get(), &accepted));
    auto request = next_request(*reactor, requests);
    REQUIRE(request.has_value());
    CHECK(request->kind == MediaRequest::Kind::Play);
    // What the daemon does next. Windows won't pass on a pause to a session
    // that says it's paused already.
    controls->set_playback_state(PlaybackState::Playing);
    CHECK(wait_for_status(session.Get(), kPlaying) == kPlaying);

    REQUIRE(SUCCEEDED(session->TryPauseAsync(&asking)));
    CHECK(wait_for(asking.Get(), &accepted));
    request = next_request(*reactor, requests);
    REQUIRE(request.has_value());
    CHECK(request->kind == MediaRequest::Kind::Pause);

    // 90 s, in SMTC's 100 ns ticks.
    REQUIRE(SUCCEEDED(session->TryChangePlaybackPositionAsync(900'000'000, &asking)));
    CHECK(wait_for(asking.Get(), &accepted));
    request = next_request(*reactor, requests);
    REQUIRE(request.has_value());
    CHECK(request->kind == MediaRequest::Kind::Seek);
    CHECK(request->position == 90s);

    CHECK(on_reactor_thread);
    CHECK(requests.empty());
}
