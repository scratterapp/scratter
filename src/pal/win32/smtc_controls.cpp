// MediaControls on Windows: SystemMediaTransportControls (SMTC), the API behind
// the Windows 11 media flyout, the lock screen's player and the keyboard's media
// keys (spec section 3.4.1, issue #9).
//
// SMTC belongs to a window: a desktop program gets it with
// ISystemMediaTransportControlsInterop::GetForWindow(hwnd). The daemon has no
// window, so it makes a hidden one. Not a message-only window (HWND_MESSAGE),
// which GetForWindow refuses with E_INVALIDARG, but a plain top-level window
// that's never shown. A top-level window gets broadcast messages (a setting
// changed, the session is ending), and a program that sends one waits for every
// window to answer, so this one needs a thread pumping its messages. That's
// the window thread below, and it owns everything SMTC.
//
// Three kinds of thread meet here:
//
//   reactor thread   calls open(), the setters and the callback. The setters
//                    write what they want shown into `wanted` and post the
//                    window a message; they never touch SMTC themselves.
//   window thread    owns the hidden window and every SMTC object. On that
//                    message it shows what `wanted` says.
//   WinRT threads    SMTC raises ButtonPressed and
//                    PlaybackPositionChangeRequested on threads from its own
//                    pool. The handlers turn them into MediaRequests and drop
//                    them in the Inbox.
//
// The Inbox has to reach the reactor thread, and the reactor's one cross-thread
// door, wake(), already belongs to the Ctrl-C handler (console_shutdown.cpp).
// So the Inbox has a pipe of its own: the reactor watches the read end with
// one overlapped ReadFile always pending, and the first request since the last
// drain writes a byte into the other end. The read finishes, the reactor runs
// our callback on its thread, and that takes every request waiting and passes
// them on. This is the self-pipe trick, done with a completion port.
//
// The WinRT calls go through the raw ABI with WRL (ComPtr, HString) rather than
// C++/WinRT. C++/WinRT reports failures by throwing, and the project builds
// with exceptions off; every ABI call returns an HRESULT instead. (WRL's
// Callback() is spelled out in full below because MediaControls::Callback, our
// request callback type, hides it inside the member functions.)

#include "media_controls.hpp"
#include "reactor.hpp"

#include "common/app_name.hpp"

#include <windows.h>

#include <roapi.h>
#include <sddl.h>
#include <systemmediatransportcontrolsinterop.h>
#include <windows.foundation.h>
#include <windows.media.h>
#include <windows.storage.streams.h>
#include <wrl/client.h>
#include <wrl/event.h>
#include <wrl/wrappers/corewrappers.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace am::pal {
namespace {

namespace foundation = ABI::Windows::Foundation;
namespace media = ABI::Windows::Media;
namespace streams = ABI::Windows::Storage::Streams;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HString;
using Microsoft::WRL::Wrappers::HStringReference;

using ButtonHandler =
    foundation::ITypedEventHandler<media::SystemMediaTransportControls*,
                                   media::SystemMediaTransportControlsButtonPressedEventArgs*>;
using SeekHandler =
    foundation::ITypedEventHandler<media::SystemMediaTransportControls*,
                                   media::PlaybackPositionChangeRequestedEventArgs*>;

// Messages the reactor thread posts to the window thread.
constexpr UINT kShowWanted = WM_APP + 1; // `wanted` changed
constexpr UINT kQuit = WM_APP + 2;       // tear down and end the thread

// While playing, the window thread sends the position this often, so the
// flyout's progress bar doesn't drift far from where playback is.
constexpr UINT kTimelineTimerMs = 5000;
constexpr UINT_PTR kTimelineTimer = 1;

std::error_code make_error(std::errc code) noexcept { return std::make_error_code(code); }

std::error_code os_error(DWORD code) noexcept {
    return {static_cast<int>(code), std::system_category()};
}

// A failed HRESULT as an error_code: a Win32 error carries its own code, the
// way GetLastError() would; anything else keeps the whole HRESULT.
std::error_code hr_error(HRESULT hr) noexcept {
    if (HRESULT_FACILITY(hr) == FACILITY_WIN32) {
        return os_error(static_cast<DWORD>(HRESULT_CODE(hr)));
    }
    return {static_cast<int>(hr), std::system_category()};
}

NativeHandle handle_of(HANDLE h) noexcept {
    return NativeHandle{reinterpret_cast<std::intptr_t>(h)};
}

// UTF-8 to UTF-16. Bad bytes become U+FFFD instead of failing: a garbled
// title is better than none.
std::wstring widen(std::string_view text) {
    if (text.empty() || text.size() > 0x7fff'ffff) {
        return {};
    }
    const int length = static_cast<int>(text.size());
    const int wide_length = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), length, nullptr, 0);
    if (wide_length <= 0) {
        return {};
    }
    std::wstring wide(static_cast<size_t>(wide_length), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), length, wide.data(), wide_length);
    return wide;
}

HString hstring(std::string_view text) {
    const std::wstring wide = widen(text);
    HString result;
    if (!wide.empty()) {
        result.Set(wide.c_str(), static_cast<unsigned>(wide.size()));
    }
    return result;
}

// SMTC counts time in 100 ns ticks.
foundation::TimeSpan ticks(std::chrono::microseconds time) noexcept {
    return foundation::TimeSpan{time.count() * 10};
}

media::MediaPlaybackStatus status_of(common::PlaybackState state) noexcept {
    switch (state) {
    case common::PlaybackState::Playing:
    case common::PlaybackState::Buffering: // the same to the desktop, as on Linux
        return media::MediaPlaybackStatus_Playing;
    case common::PlaybackState::Paused:
        return media::MediaPlaybackStatus_Paused;
    case common::PlaybackState::Stopped:
        break;
    }
    return media::MediaPlaybackStatus_Stopped;
}

template <typename Interface>
HRESULT activation_factory(PCWSTR class_name, ComPtr<Interface>& factory) {
    return ::RoGetActivationFactory(HStringReference(class_name).Get(), IID_PPV_ARGS(&factory));
}

// Requests from the WinRT threads, waiting for the reactor thread. Shared with
// the event handlers, so a handler still running while the controls close
// can't write into freed memory.
class Inbox {
  public:
    explicit Inbox(HANDLE write_end) noexcept : write_end_(write_end) {}
    ~Inbox() { ::CloseHandle(write_end_); }
    Inbox(const Inbox&) = delete;
    Inbox& operator=(const Inbox&) = delete;

    // Any thread. Only the first request since the last take() writes to the
    // pipe; the reactor drains everything at once.
    void push(const MediaRequest& request) {
        bool signal = false;
        {
            std::lock_guard guard(lock_);
            requests_.push_back(request);
            signal = !signalled_;
            signalled_ = true;
        }
        if (signal) {
            DWORD written = 0;
            ::WriteFile(write_end_, "!", 1, &written, nullptr);
        }
    }

    // Reactor thread.
    std::vector<MediaRequest> take() {
        std::lock_guard guard(lock_);
        signalled_ = false;
        return std::exchange(requests_, {});
    }

  private:
    HANDLE write_end_;
    std::mutex lock_;
    std::vector<MediaRequest> requests_;
    bool signalled_ = false;
};

// A pipe only this user can open, both ends in this process. The read end is
// overlapped so the reactor can watch it; the write end is a plain blocking
// handle (a one-byte write into an empty pipe never waits).
struct PipePair {
    HANDLE read = INVALID_HANDLE_VALUE;
    HANDLE write = INVALID_HANDLE_VALUE;
};

std::expected<PipePair, std::error_code> make_pipe() {
    static std::atomic<unsigned> counter{0};
    const std::wstring name = L"\\\\.\\pipe\\" + widen(common::kAppName) + L"-media-" +
                              std::to_wstring(::GetCurrentProcessId()) + L"-" +
                              std::to_wstring(counter.fetch_add(1));

    // Owner only, nobody else, and no inherited entries.
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;OW)", SDDL_REVISION_1,
                                                                &descriptor, nullptr)) {
        return std::unexpected(os_error(::GetLastError()));
    }
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};

    PipePair pipe;
    // FILE_FLAG_FIRST_PIPE_INSTANCE and one instance: if anyone else had the
    // name, this fails, and once our write end connects nobody else can.
    pipe.read = ::CreateNamedPipeW(
        name.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 0, 64, 0,
        &attributes);
    const DWORD create_error = ::GetLastError();
    ::LocalFree(descriptor);
    if (pipe.read == INVALID_HANDLE_VALUE) {
        return std::unexpected(os_error(create_error));
    }
    pipe.write = ::CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe.write == INVALID_HANDLE_VALUE) {
        const DWORD error = ::GetLastError();
        ::CloseHandle(pipe.read);
        return std::unexpected(os_error(error));
    }
    return pipe;
}

// What the reactor thread wants the desktop to show. The window thread copies
// it under `lock` and shows the parts whose version moved.
struct Wanted {
    std::optional<TrackInfo> track;
    std::uint64_t track_version = 0;
    common::PlaybackState state = common::PlaybackState::Stopped;
    std::uint64_t seek_version = 0;
};

} // namespace

struct MediaControls::Impl {
    Impl(Reactor& r, Callback callback) : reactor(r), on_request(std::move(callback)) {}
    ~Impl();
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    std::expected<void, std::error_code> start();
    void post_wanted();

    // Reactor thread: the pipe and the requests that come through it.
    void read_next();
    void on_pipe(const IoEvent& event);

    // Window thread.
    static DWORD WINAPI thread_main(void* self) noexcept;
    HRESULT set_up_smtc();
    void run_window();
    void show_wanted();
    void show_track(const std::optional<TrackInfo>& track);
    void show_timeline();
    void tear_down_smtc();

    Reactor& reactor;
    Callback on_request;

    std::shared_ptr<Inbox> inbox;
    HANDLE read_end = INVALID_HANDLE_VALUE;
    OVERLAPPED read_ov{};
    char read_buffer[64]{};
    bool reading = false;
    bool watching = false;

    // The window thread, and what it says once it's ready (or failed).
    HANDLE thread = nullptr;
    HANDLE ready = nullptr;
    HWND window = nullptr;
    HRESULT setup_result = E_FAIL;

    std::mutex lock; // guards wanted and show_posted
    Wanted wanted;
    bool show_posted = false;
    // set_position() runs every tick, so it skips the lock and the message.
    std::atomic<std::int64_t> position_us{0};

    // Window thread only.
    ComPtr<media::ISystemMediaTransportControls> smtc;
    ComPtr<media::ISystemMediaTransportControls2> smtc2;
    ComPtr<media::ISystemMediaTransportControlsDisplayUpdater> display;
    EventRegistrationToken button_token{};
    EventRegistrationToken seek_token{};
    Wanted shown;
    bool shown_once = false;
    bool timer_running = false;
};

std::expected<void, std::error_code> MediaControls::Impl::start() {
    auto pipe = make_pipe();
    if (!pipe) {
        return std::unexpected(pipe.error());
    }
    read_end = pipe->read;
    inbox = std::make_shared<Inbox>(pipe->write);

    read_ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ready = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (read_ov.hEvent == nullptr || ready == nullptr) {
        return std::unexpected(os_error(::GetLastError()));
    }
    auto watched = reactor.watch(handle_of(read_end), Interest{.readable = true},
                                 [this](const IoEvent& event) { on_pipe(event); });
    if (!watched) {
        return std::unexpected(watched.error());
    }
    watching = true;
    read_next();
    if (!reading) {
        return std::unexpected(os_error(::GetLastError()));
    }

    thread = ::CreateThread(nullptr, 0, &Impl::thread_main, this, 0, nullptr);
    if (thread == nullptr) {
        return std::unexpected(os_error(::GetLastError()));
    }
    const HANDLE waits[] = {ready, thread};
    ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
    if (FAILED(setup_result)) {
        // The thread cleaned up after itself and is ending.
        ::WaitForSingleObject(thread, INFINITE);
        ::CloseHandle(thread);
        thread = nullptr;
        // Anything that kept us from getting SMTC for the window (no WinRT
        // media on this edition, no desktop to make a window on) means there
        // are no media controls here.
        return std::unexpected(make_error(std::errc::function_not_supported));
    }
    return {};
}

MediaControls::Impl::~Impl() {
    if (thread != nullptr) {
        ::PostMessageW(window, kQuit, 0, 0);
        ::WaitForSingleObject(thread, INFINITE);
        ::CloseHandle(thread);
    }
    if (watching) {
        // A packet still on its way carries a key nobody owns now and is
        // dropped without touching read_ov.
        [[maybe_unused]] auto ignored = reactor.unwatch(handle_of(read_end));
    }
    if (reading) {
        ::CancelIoEx(read_end, &read_ov);
        ::WaitForSingleObject(read_ov.hEvent, INFINITE);
    }
    if (read_end != INVALID_HANDLE_VALUE) {
        ::CloseHandle(read_end);
    }
    for (HANDLE event : {read_ov.hEvent, ready}) {
        if (event != nullptr) {
            ::CloseHandle(event);
        }
    }
}

void MediaControls::Impl::read_next() {
    ::ResetEvent(read_ov.hEvent);
    // Without FILE_SKIP_COMPLETION_PORT_ON_SUCCESS, a read that finishes at
    // once still queues a packet, so both outcomes end up in on_pipe().
    const BOOL done = ::ReadFile(read_end, read_buffer, sizeof(read_buffer), nullptr, &read_ov);
    reading = done != 0 || ::GetLastError() == ERROR_IO_PENDING;
}

void MediaControls::Impl::on_pipe(const IoEvent& event) {
    if (event.overlapped != &read_ov) {
        return;
    }
    reading = false;
    if (event.error != 0) {
        return; // cancelled, or the pipe broke: no more requests
    }
    read_next();
    // Runs the callback last: it may do anything the reactor allows, but not
    // destroy these controls (the header forbids that).
    for (const MediaRequest& request : inbox->take()) {
        on_request(request);
    }
}

void MediaControls::Impl::post_wanted() {
    // Under the lock, so two setters in a row post one message.
    if (!show_posted) {
        show_posted = true;
        ::PostMessageW(window, kShowWanted, 0, 0);
    }
}

DWORD WINAPI MediaControls::Impl::thread_main(void* raw) noexcept {
    auto* self = static_cast<Impl*>(raw);
    const HRESULT init = ::RoInitialize(RO_INIT_MULTITHREADED);
    self->setup_result = FAILED(init) ? init : self->set_up_smtc();
    const bool ok = SUCCEEDED(self->setup_result);
    ::SetEvent(self->ready); // after this, open() may return
    if (ok) {
        self->run_window();
    }
    self->tear_down_smtc();
    if (SUCCEEDED(init)) {
        ::RoUninitialize();
    }
    return 0;
}

namespace {

// The hidden window's messages. Closing it isn't ours to allow: `taskkill`
// without /F sends WM_CLOSE to a program's windows, and losing the media keys
// is all that would come of it.
LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_CLOSE) {
        return 0;
    }
    return ::DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

HRESULT MediaControls::Impl::set_up_smtc() {
    static const std::wstring class_name = widen(common::kAppName) + L" media controls";
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = ::GetModuleHandleW(nullptr);
    window_class.lpszClassName = class_name.c_str();
    if (::RegisterClassExW(&window_class) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return HRESULT_FROM_WIN32(::GetLastError());
    }
    const std::wstring title = widen(common::kAppName);
    window =
        ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, class_name.c_str(), title.c_str(),
                          WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, window_class.hInstance, nullptr);
    if (window == nullptr) {
        return HRESULT_FROM_WIN32(::GetLastError());
    }

    ComPtr<ISystemMediaTransportControlsInterop> interop;
    HRESULT hr =
        activation_factory(RuntimeClass_Windows_Media_SystemMediaTransportControls, interop);
    if (SUCCEEDED(hr)) {
        hr = interop->GetForWindow(window, IID_PPV_ARGS(&smtc));
    }
    if (SUCCEEDED(hr)) {
        hr = smtc.As(&smtc2);
    }
    if (SUCCEEDED(hr)) {
        hr = smtc->get_DisplayUpdater(&display);
    }
    if (FAILED(hr)) {
        return hr;
    }

    // Next and previous stay off until there's a queue to move through, as on
    // Linux. Seeking is allowed whenever the track has a length (show_timeline).
    smtc->put_IsPlayEnabled(true);
    smtc->put_IsPauseEnabled(true);
    smtc->put_IsStopEnabled(true);
    smtc->put_IsNextEnabled(false);
    smtc->put_IsPreviousEnabled(false);
    smtc->put_PlaybackStatus(media::MediaPlaybackStatus_Stopped);
    display->put_Type(media::MediaPlaybackType_Music);
    display->Update();

    // The keyboard's single play/pause key arrives as Play or Pause, whichever
    // fits the status we last set, so PlayPause never comes from Windows.
    std::shared_ptr<Inbox> box = inbox;
    hr = smtc->add_ButtonPressed(
        Microsoft::WRL::Callback<ButtonHandler>(
            [box](media::ISystemMediaTransportControls*,
                  media::ISystemMediaTransportControlsButtonPressedEventArgs* args) -> HRESULT {
                media::SystemMediaTransportControlsButton button{};
                if (FAILED(args->get_Button(&button))) {
                    return S_OK;
                }
                using Kind = MediaRequest::Kind;
                switch (button) {
                case media::SystemMediaTransportControlsButton_Play:
                    box->push({.kind = Kind::Play});
                    break;
                case media::SystemMediaTransportControlsButton_Pause:
                    box->push({.kind = Kind::Pause});
                    break;
                case media::SystemMediaTransportControlsButton_Stop:
                    box->push({.kind = Kind::Stop});
                    break;
                case media::SystemMediaTransportControlsButton_Next:
                    box->push({.kind = Kind::Next});
                    break;
                case media::SystemMediaTransportControlsButton_Previous:
                    box->push({.kind = Kind::Previous});
                    break;
                default:
                    break; // record, fast forward, rewind, channel up/down
                }
                return S_OK;
            })
            .Get(),
        &button_token);
    if (FAILED(hr)) {
        return hr;
    }
    hr = smtc2->add_PlaybackPositionChangeRequested(
        Microsoft::WRL::Callback<SeekHandler>(
            [box](media::ISystemMediaTransportControls*,
                  media::IPlaybackPositionChangeRequestedEventArgs* args) -> HRESULT {
                foundation::TimeSpan position{};
                if (SUCCEEDED(args->get_RequestedPlaybackPosition(&position))) {
                    box->push({.kind = MediaRequest::Kind::Seek,
                               .position = std::chrono::microseconds{position.Duration / 10}});
                }
                return S_OK;
            })
            .Get(),
        &seek_token);
    if (FAILED(hr)) {
        return hr;
    }
    return smtc->put_IsEnabled(true);
}

void MediaControls::Impl::run_window() {
    MSG message{};
    while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.hwnd == window && message.message == kQuit) {
            return;
        }
        if (message.hwnd == window && message.message == kShowWanted) {
            show_wanted();
        } else if (message.hwnd == window && message.message == WM_TIMER &&
                   message.wParam == kTimelineTimer) {
            show_timeline();
        } else {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
    }
}

void MediaControls::Impl::show_wanted() {
    Wanted now;
    {
        std::lock_guard guard(lock);
        show_posted = false;
        now = wanted;
    }
    const bool track_changed = !shown_once || now.track_version != shown.track_version;
    const bool state_changed = !shown_once || now.state != shown.state;
    const bool seeked = !shown_once || now.seek_version != shown.seek_version;
    shown = std::move(now);
    shown_once = true;

    if (track_changed) {
        show_track(shown.track);
    }
    if (state_changed) {
        smtc->put_PlaybackStatus(status_of(shown.state));
    }
    if (track_changed || state_changed || seeked) {
        show_timeline();
    }

    const bool playing = shown.state == common::PlaybackState::Playing ||
                         shown.state == common::PlaybackState::Buffering;
    if (playing != timer_running) {
        if (playing) {
            ::SetTimer(window, kTimelineTimer, kTimelineTimerMs, nullptr);
        } else {
            ::KillTimer(window, kTimelineTimer);
        }
        timer_running = playing;
    }
}

void MediaControls::Impl::show_track(const std::optional<TrackInfo>& track) {
    if (!track) {
        display->ClearAll();
        display->put_Type(media::MediaPlaybackType_Music);
        display->Update();
        return;
    }
    display->put_Type(media::MediaPlaybackType_Music);
    ComPtr<media::IMusicDisplayProperties> music;
    if (SUCCEEDED(display->get_MusicProperties(&music))) {
        music->put_Title(hstring(track->title).Get());
        music->put_Artist(hstring(track->artist).Get());
        ComPtr<media::IMusicDisplayProperties2> music2;
        if (SUCCEEDED(music.As(&music2))) {
            music2->put_AlbumTitle(hstring(track->album).Get());
        }
    }

    // The flyout fetches the cover art from the link itself.
    ComPtr<streams::IRandomAccessStreamReference> artwork;
    if (!track->artwork_url.empty()) {
        ComPtr<foundation::IUriRuntimeClassFactory> uris;
        ComPtr<foundation::IUriRuntimeClass> uri;
        ComPtr<streams::IRandomAccessStreamReferenceStatics> references;
        if (SUCCEEDED(activation_factory(RuntimeClass_Windows_Foundation_Uri, uris)) &&
            SUCCEEDED(uris->CreateUri(hstring(track->artwork_url).Get(), &uri)) &&
            SUCCEEDED(activation_factory(
                RuntimeClass_Windows_Storage_Streams_RandomAccessStreamReference, references))) {
            references->CreateFromUri(uri.Get(), &artwork);
        }
    }
    display->put_Thumbnail(artwork.Get()); // null clears the old one
    display->Update();
}

void MediaControls::Impl::show_timeline() {
    ComPtr<IInspectable> instance;
    ComPtr<media::ISystemMediaTransportControlsTimelineProperties> timeline;
    if (FAILED(::RoActivateInstance(
            HStringReference(
                RuntimeClass_Windows_Media_SystemMediaTransportControlsTimelineProperties)
                .Get(),
            &instance)) ||
        FAILED(instance.As(&timeline))) {
        return;
    }
    // No length (the test tone, or nothing loaded): all zeros, which hides the
    // progress bar and leaves nowhere to seek to.
    using std::chrono::microseconds;
    const microseconds length =
        shown.track ? std::max(shown.track->length, microseconds{0}) : microseconds{0};
    const microseconds position = std::clamp(
        microseconds{position_us.load(std::memory_order_relaxed)}, microseconds{0}, length);
    timeline->put_StartTime(ticks(microseconds{0}));
    timeline->put_EndTime(ticks(length));
    timeline->put_MinSeekTime(ticks(microseconds{0}));
    timeline->put_MaxSeekTime(ticks(length));
    timeline->put_Position(ticks(position));
    smtc2->UpdateTimelineProperties(timeline.Get());
}

void MediaControls::Impl::tear_down_smtc() {
    if (timer_running) {
        ::KillTimer(window, kTimelineTimer);
    }
    if (smtc) {
        smtc->remove_ButtonPressed(button_token);
        smtc->put_PlaybackStatus(media::MediaPlaybackStatus_Closed);
        smtc->put_IsEnabled(false);
    }
    if (smtc2) {
        smtc2->remove_PlaybackPositionChangeRequested(seek_token);
    }
    if (display) {
        display->ClearAll();
        display->Update();
    }
    display.Reset();
    smtc2.Reset();
    smtc.Reset();
    if (window != nullptr) {
        ::DestroyWindow(window);
    }
}

std::expected<MediaControls, std::error_code>
MediaControls::open(Reactor& reactor, Callback on_request, std::string_view name) {
    if (name.empty()) {
        return std::unexpected(make_error(std::errc::invalid_argument));
    }
    auto impl = std::make_unique<Impl>(reactor, std::move(on_request));
    auto started = impl->start();
    if (!started) {
        return std::unexpected(started.error());
    }
    return MediaControls(std::move(impl));
}

MediaControls::MediaControls(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
MediaControls::~MediaControls() = default;
MediaControls::MediaControls(MediaControls&&) noexcept = default;
MediaControls& MediaControls::operator=(MediaControls&&) noexcept = default;

void MediaControls::set_track(const TrackInfo& track) {
    std::lock_guard guard(impl_->lock);
    impl_->wanted.track = track;
    ++impl_->wanted.track_version;
    impl_->post_wanted();
}

void MediaControls::clear_track() {
    std::lock_guard guard(impl_->lock);
    impl_->wanted.track.reset();
    ++impl_->wanted.track_version;
    impl_->post_wanted();
}

void MediaControls::set_playback_state(common::PlaybackState state) {
    std::lock_guard guard(impl_->lock);
    if (impl_->wanted.state != state) {
        impl_->wanted.state = state;
        impl_->post_wanted();
    }
}

void MediaControls::set_position(std::chrono::microseconds position) {
    impl_->position_us.store(position.count(), std::memory_order_relaxed);
}

void MediaControls::seeked(std::chrono::microseconds position) {
    impl_->position_us.store(position.count(), std::memory_order_relaxed);
    std::lock_guard guard(impl_->lock);
    ++impl_->wanted.seek_version;
    impl_->post_wanted();
}

void MediaControls::set_volume(std::uint8_t) {
    // SMTC has no volume, and Windows never asks to change it.
}

} // namespace am::pal
