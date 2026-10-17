#pragma once

// Desktop media controls: the play/pause keys on the keyboard, the media
// widget in the KDE or GNOME panel, the Windows 11 media flyout, a phone's
// controls over Bluetooth (spec section 3.4.1).
//
//   Linux:    MPRIS v2 over D-Bus, with sd-bus, #56
//   Windows:  SystemMediaTransportControls (SMTC), with C++/WinRT, #57
//
// They live in the daemon, not in am-ui, so the keys keep working with the UI
// closed. Traffic goes both ways:
//
//   in  (desktop -> daemon): play, pause, play/pause, stop, next, previous,
//                            seek, set volume. One callback, on the reactor
//                            thread, with a MediaRequest saying which.
//   out (daemon -> desktop): what's playing (title, artist, album, artwork,
//                            length), whether it's playing, the position and
//                            the volume. One setter each.
//
//   auto controls = MediaControls::open(reactor, [&](const MediaRequest& request) {
//       switch (request.kind) {
//       case MediaRequest::Kind::PlayPause: toggle(); break;
//       case MediaRequest::Kind::Seek:      seek_to(request.position); break;
//       ...
//       }
//   });
//   if (!controls) { log controls.error().message(); carry on without media keys }
//
//   TrackInfo track;
//   track.title = "Song";
//   track.artist = "Band";
//   track.length = 3min;
//   controls->set_track(track);
//   controls->set_playback_state(PlaybackState::Playing);
//   controls->set_position(where_we_are);   // every tick is fine
//
// Media keys are a nice extra, not something playback depends on: if open()
// fails, the daemon logs it and plays on without them.
//
// Threading
// ---------
// Like the Reactor: open() and every member function run on the reactor
// thread, and the callback runs there too. On Linux the D-Bus connection's fd
// is watched by the reactor, so there's no extra thread. On Windows, SMTC
// raises its events on a thread of its own; the Windows version has to hand
// them over to the reactor thread before calling the callback.
//
// None of the setters wait for the desktop. They store the new value and send
// a change notice if one is due, so they're cheap enough to call every tick.
//
// Errors are std::error_code, like the other interfaces. Compare against
// std::errc to branch:
//
//   std::errc::function_not_supported  no media control service here (no
//                                      D-Bus session bus, for example), or
//                                      this platform's version isn't written
//                                      yet
//   std::errc::invalid_argument        an empty name
//
// The setters don't return errors. If a notice can't be sent, the desktop
// shows something stale until the next change, which isn't worth stopping
// for.
//
// There is one class and one set of member functions. src/pal/linux/mpris_controls.cpp
// defines them on Linux and src/pal/win32/smtc_controls.cpp on Windows, and CMake
// compiles only one of the two. Nothing here may include a platform header.

#include "common/app_name.hpp"
#include "common/telemetry_shm.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

namespace am::pal {

class Reactor;

// Something the desktop asked the daemon to do.
struct MediaRequest {
    enum class Kind : std::uint8_t {
        Play,
        Pause,
        PlayPause, // the single play/pause key: pause if playing, play otherwise
        Stop,
        Next,
        Previous,
        Seek,      // jump to `position`
        SetVolume, // set the volume to `volume_percent`
    };

    Kind kind = Kind::Play;

    // Seek only: where to jump to, from the start of the track. MPRIS can also
    // ask for a jump relative to where we are ("10 s forward"); the Linux
    // version turns that into a position using the last set_position(), and
    // stops at 0. It can be past the end of the track: the daemon decides what
    // that means (MPRIS says: go to the next track).
    std::chrono::microseconds position{0};

    // SetVolume only: 0 to 100, the same scale as the IPC SetVolume message.
    // MPRIS uses 0.0 to 1.0, so the Linux version converts. SMTC has no
    // volume control, so Windows never sends this.
    std::uint8_t volume_percent = 0;
};

// What's playing, as the desktop shows it. Empty strings are fine: the desktop
// leaves that line blank.
struct TrackInfo {
    std::string title;
    std::string artist;
    std::string album;
    std::string artwork_url; // https:// link to the cover art; the desktop fetches it
    std::chrono::microseconds length{0};
};

class MediaControls {
  public:
    using Callback = std::move_only_function<void(const MediaRequest&)>;

    // Registers with the desktop under `name` and starts passing its requests
    // to `on_request`. Linux claims the bus name org.mpris.MediaPlayer2.<name>;
    // Windows doesn't need a name and only checks it isn't empty. The tests
    // pass a throwaway name so they never clash with a running daemon.
    //
    // Until set_track() is called, the desktop sees a player that is stopped
    // with nothing loaded.
    [[nodiscard]] static std::expected<MediaControls, std::error_code>
    open(Reactor& reactor, Callback on_request, std::string_view name = common::kAppName);

    // Unregisters, and the player disappears from the desktop. Destroy it
    // before the reactor, and not from inside its own callback. Moving it is
    // fine.
    ~MediaControls();
    MediaControls(MediaControls&&) noexcept;
    MediaControls& operator=(MediaControls&&) noexcept;
    MediaControls(const MediaControls&) = delete;
    MediaControls& operator=(const MediaControls&) = delete;

    // A new track started, or what's known about this one changed (the cover
    // art link arrived late, say).
    void set_track(const TrackInfo& track);

    // Nothing loaded any more: the desktop shows an empty player.
    void clear_track();

    void set_playback_state(common::PlaybackState state);

    // Where playback is in the current track. Call it as often as you like;
    // it only stores the value, for the desktop to read when it wants it.
    void set_position(std::chrono::microseconds position);

    // The position jumped (a seek, from the desktop or from am-ui). Stores it
    // like set_position() and tells the desktop, so its progress bar jumps too.
    void seeked(std::chrono::microseconds position);

    // 0 to 100. Windows ignores it (SMTC has no volume).
    void set_volume(std::uint8_t volume_percent);

  private:
    // Platform state (the D-Bus connection, or the SMTC object and its hidden
    // window) lives in Impl, defined in each platform's .cpp. That keeps
    // <systemd/sd-bus.h> and the WinRT headers out of this one.
    struct Impl;
    explicit MediaControls(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace am::pal
