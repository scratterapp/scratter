// MediaControls on Linux: MPRIS v2 over D-Bus, with sd-bus (spec section
// 3.4.1, issue #56).
//
// D-Bus is the desktop's message bus. Programs connect to it and publish
// objects; each object has interfaces, and each interface has methods (calls
// other programs can make), properties (values they can read) and signals
// (notices we send out). MPRIS is a standard set of those for music players:
//
//   bus name   org.mpris.MediaPlayer2.<name>    how the desktop finds us
//   object     /org/mpris/MediaPlayer2
//     org.mpris.MediaPlayer2          who we are: Identity, CanQuit, ...
//     org.mpris.MediaPlayer2.Player   PlayPause(), Seek(), PlaybackStatus,
//                                     Metadata, Volume, Position, Seeked, ...
//
// The KDE and GNOME media widgets, playerctl and the media keys all look for
// names starting with org.mpris.MediaPlayer2. and talk to whatever is there.
//
// sd-bus (part of libsystemd) is the library. We describe each interface with
// a table (a "vtable") of method and property handlers below, and sd-bus calls
// them when a message comes in. When a property changes we call
// sd_bus_emit_properties_changed(), and sd-bus asks the handler for the new
// value and sends a PropertiesChanged signal, which is how the widget learns
// a song started or got paused without asking every second.
//
// The connection is one socket. Its fd goes to Reactor::watch(), and when it's
// readable pump() lets sd-bus read and handle everything waiting. So every
// handler, and the callback it calls, runs on the reactor thread, as the
// header promises. There's no extra thread.

#include "media_controls.hpp"

#include "reactor.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <poll.h>
#include <systemd/sd-bus.h>

namespace am::pal {
namespace {

using std::chrono::microseconds;

constexpr const char* kObjectPath = "/org/mpris/MediaPlayer2";
constexpr const char* kRootInterface = "org.mpris.MediaPlayer2";
constexpr const char* kPlayerInterface = "org.mpris.MediaPlayer2.Player";

// MPRIS wants every track to have an id, an object path. There's only ever
// one current track and nothing else refers to tracks by id, so one fixed path
// is enough. NoTrack is the value MPRIS reserves for "nothing loaded".
constexpr const char* kTrackId = "/CurrentTrack";
constexpr const char* kNoTrack = "/org/mpris/MediaPlayer2/TrackList/NoTrack";

std::error_code make_error(std::errc code) noexcept { return std::make_error_code(code); }

// sd-bus returns errors as negative errno values.
std::error_code from_sd_bus(int result) noexcept { return {-result, std::system_category()}; }

// MPRIS has no "buffering", so that shows as playing: it's about to be.
const char* status_name(common::PlaybackState state) noexcept {
    switch (state) {
    case common::PlaybackState::Playing:
    case common::PlaybackState::Buffering:
        return "Playing";
    case common::PlaybackState::Paused:
        return "Paused";
    case common::PlaybackState::Stopped:
        return "Stopped";
    }
    return "Stopped";
}

// MPRIS volume is 0.0 to 1.0 (clients may send more; we stop at 1.0).
std::uint8_t to_percent(double volume) noexcept {
    if (!(volume > 0.0)) { // also catches NaN
        return 0;
    }
    return static_cast<std::uint8_t>(std::lround(std::min(volume, 1.0) * 100.0));
}

// Everything behind one MediaControls: the connection and the values we show
// the desktop. MediaControls::Impl (at the bottom) just holds one.
struct Mpris {
    Mpris(Reactor& r, MediaControls::Callback callback, std::string name)
        : reactor(r), on_request(std::move(callback)), bus_name("org.mpris.MediaPlayer2." + name),
          identity(std::move(name)) {}

    ~Mpris() { close(); }

    Mpris(const Mpris&) = delete;
    Mpris& operator=(const Mpris&) = delete;

    // Lets sd-bus handle everything that has arrived, then watches for
    // "writable" too if it still has something queued to send.
    void pump() {
        if (bus == nullptr) {
            return;
        }
        for (;;) {
            const int r = sd_bus_process(bus, nullptr);
            if (r < 0) {
                std::fprintf(
                    stderr,
                    "media keys: lost the D-Bus connection (%s), carrying on without them\n",
                    from_sd_bus(r).message().c_str());
                close();
                return;
            }
            if (r == 0) {
                break; // nothing left to do
            }
        }
        update_interest();
    }

    // sd-bus sends what it can straight away and queues the rest. Ask the
    // reactor for "writable" only while something is queued, or it would wake
    // us up all the time.
    void update_interest() {
        if (bus == nullptr) {
            return;
        }
        const int events = sd_bus_get_events(bus);
        const bool writable = events > 0 && (events & POLLOUT) != 0;
        if (writable != want_writable) {
            want_writable = writable;
            (void)reactor.modify(NativeHandle{fd}, {.readable = true, .writable = writable});
        }
    }

    // Gives up the name (so it's free for the next open() at once), then
    // closes the connection. Safe to call twice.
    void close() {
        if (bus == nullptr) {
            return;
        }
        if (watching) {
            (void)reactor.unwatch(NativeHandle{fd});
            watching = false;
        }
        if (named) {
            (void)sd_bus_release_name(bus, bus_name.c_str());
            named = false;
        }
        bus = sd_bus_flush_close_unref(bus);
    }

    // Tells clients these Player properties have new values. sd-bus reads the
    // values back through the property handlers below.
    template <typename... Names> void properties_changed(Names... names) {
        if (bus == nullptr) {
            return;
        }
        (void)sd_bus_emit_properties_changed(bus, kObjectPath, kPlayerInterface, names..., nullptr);
        update_interest();
    }

    void request(const MediaRequest& r) { on_request(r); }

    Reactor& reactor;
    MediaControls::Callback on_request;
    std::string bus_name;
    std::string identity;

    sd_bus* bus = nullptr;
    int fd = -1;
    bool watching = false;
    bool named = false;
    bool want_writable = false;

    // What we tell the desktop. The setters change these; the property
    // handlers read them.
    std::optional<TrackInfo> track;
    common::PlaybackState state = common::PlaybackState::Stopped;
    microseconds position{0};
    std::uint8_t volume_percent = 100;
};

Mpris& mpris_of(void* userdata) noexcept { return *static_cast<Mpris*>(userdata); }

// --- Methods -------------------------------------------------------------
//
// Each one gets the incoming call, and replies to it. The reply goes out after
// the daemon's callback has run, so a client that reads PlaybackStatus right
// after PlayPause() returns already sees the new state.

// Play, Pause, PlayPause, Stop, Next, Previous: no arguments, one request.
template <MediaRequest::Kind kind>
int simple_request(sd_bus_message* call, void* userdata, sd_bus_error*) {
    mpris_of(userdata).request({.kind = kind});
    return sd_bus_reply_method_return(call, nullptr);
}

// Seek(offset): jump this far from where we are, forward or back. The header
// only has "go to this position", so work it out from the last set_position().
int seek(sd_bus_message* call, void* userdata, sd_bus_error*) {
    Mpris& mpris = mpris_of(userdata);
    std::int64_t offset = 0;
    if (const int r = sd_bus_message_read(call, "x", &offset); r < 0) {
        return r;
    }
    if (mpris.track) {
        // Any program on the bus can send any 64-bit offset, and a sum past
        // the largest or smallest int64 is undefined behavior in C++. With
        // `from` 0 or more, only a forward seek can go over, so stop at the
        // end of the range instead.
        const std::int64_t from = std::max<std::int64_t>(mpris.position.count(), 0);
        const std::int64_t max = std::numeric_limits<std::int64_t>::max();
        const std::int64_t to = offset > 0 && from > max - offset ? max : from + offset;
        const microseconds target{std::max<std::int64_t>(to, 0)};
        mpris.request({.kind = MediaRequest::Kind::Seek, .position = target});
    }
    return sd_bus_reply_method_return(call, nullptr);
}

// SetPosition(track_id, position): go to `position`. MPRIS says to ignore it
// if the track id isn't the current one (the track changed while the user was
// dragging the slider) or the position is outside the track.
int set_position(sd_bus_message* call, void* userdata, sd_bus_error*) {
    Mpris& mpris = mpris_of(userdata);
    const char* track_id = nullptr;
    std::int64_t position = 0;
    if (const int r = sd_bus_message_read(call, "ox", &track_id, &position); r < 0) {
        return r;
    }
    if (!mpris.track || std::string_view(track_id) != kTrackId) {
        return sd_bus_reply_method_return(call, nullptr);
    }
    const auto length = mpris.track->length.count();
    if (position >= 0 && (length == 0 || position <= length)) {
        mpris.request({.kind = MediaRequest::Kind::Seek, .position = microseconds{position}});
    }
    return sd_bus_reply_method_return(call, nullptr);
}

// We don't open links or files given to us from outside.
int open_uri(sd_bus_message*, void*, sd_bus_error* error) {
    return sd_bus_error_set(error, SD_BUS_ERROR_NOT_SUPPORTED, "OpenUri isn't supported");
}

// Raise() (bring the window up) and Quit(): CanRaise and CanQuit say we
// don't, so these do nothing. MPRIS still requires them to exist.
int do_nothing(sd_bus_message* call, void*, sd_bus_error*) {
    return sd_bus_reply_method_return(call, nullptr);
}

// --- Properties ----------------------------------------------------------
//
// sd-bus calls a getter when a client reads a property and when we emit
// PropertiesChanged. It appends the value to `reply`.

#define AM_GETTER(name)                                                                            \
    int name(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply,                \
             [[maybe_unused]] void* userdata, sd_bus_error*)

AM_GETTER(get_true) { return sd_bus_message_append(reply, "b", 1); }
AM_GETTER(get_false) { return sd_bus_message_append(reply, "b", 0); }
AM_GETTER(get_rate) { return sd_bus_message_append(reply, "d", 1.0); }
AM_GETTER(get_no_strings) { return sd_bus_message_append(reply, "as", 0); }

AM_GETTER(get_identity) {
    return sd_bus_message_append(reply, "s", mpris_of(userdata).identity.c_str());
}

AM_GETTER(get_playback_status) {
    return sd_bus_message_append(reply, "s", status_name(mpris_of(userdata).state));
}

AM_GETTER(get_volume) {
    return sd_bus_message_append(reply, "d", mpris_of(userdata).volume_percent / 100.0);
}

AM_GETTER(get_position) {
    return sd_bus_message_append(reply, "x",
                                 static_cast<std::int64_t>(mpris_of(userdata).position.count()));
}

// Seeking needs a track whose length we know.
AM_GETTER(get_can_seek) {
    const auto& track = mpris_of(userdata).track;
    return sd_bus_message_append(reply, "b", track && track->length.count() > 0 ? 1 : 0);
}

#undef AM_GETTER

// One "key: value" entry of the Metadata dictionary. The value is a variant,
// so it carries its own type: "s" for text, "x" for a number, and so on.
template <typename... Values>
int append_entry(sd_bus_message* reply, const char* key, const char* type, Values... values) {
    int r = sd_bus_message_open_container(reply, 'e', "sv");
    if (r >= 0) {
        r = sd_bus_message_append(reply, "s", key);
    }
    if (r >= 0) {
        r = sd_bus_message_open_container(reply, 'v', type);
    }
    if (r >= 0) {
        r = sd_bus_message_append(reply, type, values...);
    }
    if (r >= 0) {
        r = sd_bus_message_close_container(reply);
    }
    if (r >= 0) {
        r = sd_bus_message_close_container(reply);
    }
    return r;
}

// Metadata is a dictionary (D-Bus type a{sv}) with standard keys: xesam: ones
// come from an old desktop search project, mpris: ones from MPRIS itself.
// Artist is a list because songs can have several.
int get_metadata(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply,
                 void* userdata, sd_bus_error*) {
    const auto& track = mpris_of(userdata).track;
    int r = sd_bus_message_open_container(reply, 'a', "{sv}");
    if (r < 0) {
        return r;
    }
    if (!track) {
        r = append_entry(reply, "mpris:trackid", "o", kNoTrack);
    } else {
        r = append_entry(reply, "mpris:trackid", "o", kTrackId);
        if (r >= 0) {
            r = append_entry(reply, "xesam:title", "s", track->title.c_str());
        }
        if (r >= 0) {
            r = append_entry(reply, "xesam:artist", "as", 1, track->artist.c_str());
        }
        if (r >= 0) {
            r = append_entry(reply, "xesam:album", "s", track->album.c_str());
        }
        if (r >= 0 && track->length.count() > 0) {
            r = append_entry(reply, "mpris:length", "x",
                             static_cast<std::int64_t>(track->length.count()));
        }
        if (r >= 0 && !track->artwork_url.empty()) {
            r = append_entry(reply, "mpris:artUrl", "s", track->artwork_url.c_str());
        }
    }
    if (r < 0) {
        return r;
    }
    return sd_bus_message_close_container(reply);
}

// A client set Volume. Pass it on; the daemon calls set_volume() back, which
// stores it and tells everyone.
int set_volume_property(sd_bus*, const char*, const char*, const char*, sd_bus_message* value,
                        void* userdata, sd_bus_error*) {
    double volume = 0.0;
    if (const int r = sd_bus_message_read(value, "d", &volume); r < 0) {
        return r;
    }
    mpris_of(userdata).request(
        {.kind = MediaRequest::Kind::SetVolume, .volume_percent = to_percent(volume)});
    return 0;
}

// --- The two interfaces ---------------------------------------------------
//
// CONST: never changes, clients may cache it. EMITS_CHANGE: we send
// PropertiesChanged when it does. Position has neither: it changes all the
// time, so clients read it when they want it, and Seeked tells them about
// jumps.

using Kind = MediaRequest::Kind;
constexpr auto kConst = SD_BUS_VTABLE_PROPERTY_CONST;
constexpr auto kEmits = SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE;

const sd_bus_vtable kRootVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Raise", "", "", do_nothing, 0),
    SD_BUS_METHOD("Quit", "", "", do_nothing, 0),
    SD_BUS_PROPERTY("CanQuit", "b", get_false, 0, kConst),
    SD_BUS_PROPERTY("CanRaise", "b", get_false, 0, kConst),
    SD_BUS_PROPERTY("HasTrackList", "b", get_false, 0, kConst),
    SD_BUS_PROPERTY("Identity", "s", get_identity, 0, kConst),
    SD_BUS_PROPERTY("SupportedUriSchemes", "as", get_no_strings, 0, kConst),
    SD_BUS_PROPERTY("SupportedMimeTypes", "as", get_no_strings, 0, kConst),
    SD_BUS_VTABLE_END,
};

// CanGoNext and CanGoPrevious stay false until there's a play queue to move
// through; the interface gets a setter for them then.
const sd_bus_vtable kPlayerVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Next", "", "", simple_request<Kind::Next>, 0),
    SD_BUS_METHOD("Previous", "", "", simple_request<Kind::Previous>, 0),
    SD_BUS_METHOD("Pause", "", "", simple_request<Kind::Pause>, 0),
    SD_BUS_METHOD("PlayPause", "", "", simple_request<Kind::PlayPause>, 0),
    SD_BUS_METHOD("Stop", "", "", simple_request<Kind::Stop>, 0),
    SD_BUS_METHOD("Play", "", "", simple_request<Kind::Play>, 0),
    SD_BUS_METHOD("Seek", "x", "", seek, 0),
    SD_BUS_METHOD("SetPosition", "ox", "", set_position, 0),
    SD_BUS_METHOD("OpenUri", "s", "", open_uri, 0),
    SD_BUS_SIGNAL("Seeked", "x", 0),
    SD_BUS_PROPERTY("PlaybackStatus", "s", get_playback_status, 0, kEmits),
    SD_BUS_PROPERTY("Rate", "d", get_rate, 0, kConst),
    SD_BUS_PROPERTY("Metadata", "a{sv}", get_metadata, 0, kEmits),
    SD_BUS_WRITABLE_PROPERTY("Volume", "d", get_volume, set_volume_property, 0, kEmits),
    SD_BUS_PROPERTY("Position", "x", get_position, 0, 0),
    SD_BUS_PROPERTY("MinimumRate", "d", get_rate, 0, kConst),
    SD_BUS_PROPERTY("MaximumRate", "d", get_rate, 0, kConst),
    SD_BUS_PROPERTY("CanGoNext", "b", get_false, 0, kConst),
    SD_BUS_PROPERTY("CanGoPrevious", "b", get_false, 0, kConst),
    SD_BUS_PROPERTY("CanPlay", "b", get_true, 0, kConst),
    SD_BUS_PROPERTY("CanPause", "b", get_true, 0, kConst),
    SD_BUS_PROPERTY("CanSeek", "b", get_can_seek, 0, kEmits),
    SD_BUS_PROPERTY("CanControl", "b", get_true, 0, kConst),
    SD_BUS_VTABLE_END,
};

} // namespace

struct MediaControls::Impl {
    Impl(Reactor& reactor, Callback on_request, std::string name)
        : mpris(reactor, std::move(on_request), std::move(name)) {}
    Mpris mpris;
};

std::expected<MediaControls, std::error_code>
MediaControls::open(Reactor& reactor, Callback on_request, std::string_view name) {
    if (name.empty()) {
        return std::unexpected(make_error(std::errc::invalid_argument));
    }
    auto impl = std::make_unique<Impl>(reactor, std::move(on_request), std::string(name));
    Mpris& mpris = impl->mpris;

    // The session bus: one per logged-in user, found through
    // $DBUS_SESSION_BUS_ADDRESS or $XDG_RUNTIME_DIR/bus. A CI runner or an
    // ssh login may not have one.
    if (sd_bus_open_user(&mpris.bus) < 0) {
        mpris.bus = nullptr;
        return std::unexpected(make_error(std::errc::function_not_supported));
    }

    // The handlers get &mpris as their userdata. It stays at one
    // address (it's behind a unique_ptr), so moving MediaControls is fine.
    int r = sd_bus_add_object_vtable(mpris.bus, nullptr, kObjectPath, kRootInterface, kRootVtable,
                                     &mpris);
    if (r >= 0) {
        r = sd_bus_add_object_vtable(mpris.bus, nullptr, kObjectPath, kPlayerInterface,
                                     kPlayerVtable, &mpris);
    }
    if (r < 0) {
        return std::unexpected(from_sd_bus(r));
    }

    // Claim the name last, once everything behind it is ready to answer.
    // Flags 0: fail if someone else has it, rather than wait in line for it.
    r = sd_bus_request_name(mpris.bus, mpris.bus_name.c_str(), 0);
    if (r == -EINVAL) {
        // Bus names allow letters, digits, '_' and '-' only.
        return std::unexpected(make_error(std::errc::invalid_argument));
    }
    if (r == -EEXIST) {
        return std::unexpected(make_error(std::errc::address_in_use));
    }
    if (r < 0) {
        return std::unexpected(from_sd_bus(r));
    }
    mpris.named = true;

    r = sd_bus_get_fd(mpris.bus);
    if (r < 0) {
        return std::unexpected(from_sd_bus(r));
    }
    mpris.fd = r;
    Mpris* raw = &mpris;
    if (auto watched = reactor.watch(NativeHandle{mpris.fd}, {.readable = true},
                                     [raw](const IoEvent&) { raw->pump(); });
        !watched) {
        return std::unexpected(watched.error());
    }
    mpris.watching = true;

    // Requesting the name may have read other messages already; handle them
    // now, since the fd won't say "readable" for those.
    mpris.pump();
    return MediaControls(std::move(impl));
}

MediaControls::MediaControls(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
MediaControls::~MediaControls() = default;
MediaControls::MediaControls(MediaControls&&) noexcept = default;
MediaControls& MediaControls::operator=(MediaControls&&) noexcept = default;

void MediaControls::set_track(const TrackInfo& track) {
    impl_->mpris.track = track;
    impl_->mpris.properties_changed("Metadata", "CanSeek");
}

void MediaControls::clear_track() {
    if (!impl_->mpris.track) {
        return;
    }
    impl_->mpris.track.reset();
    impl_->mpris.properties_changed("Metadata", "CanSeek");
}

void MediaControls::set_playback_state(common::PlaybackState state) {
    // Buffering and Playing look the same to MPRIS; don't send a notice for
    // a change nobody can see.
    const bool visible = std::string_view(status_name(state)) != status_name(impl_->mpris.state);
    impl_->mpris.state = state;
    if (visible) {
        impl_->mpris.properties_changed("PlaybackStatus");
    }
}

void MediaControls::set_position(std::chrono::microseconds position) {
    impl_->mpris.position = position;
}

void MediaControls::seeked(std::chrono::microseconds position) {
    impl_->mpris.position = position;
    if (impl_->mpris.bus == nullptr) {
        return;
    }
    (void)sd_bus_emit_signal(impl_->mpris.bus, kObjectPath, kPlayerInterface, "Seeked", "x",
                             static_cast<std::int64_t>(position.count()));
    impl_->mpris.update_interest();
}

void MediaControls::set_volume(std::uint8_t volume_percent) {
    if (volume_percent == impl_->mpris.volume_percent) {
        return;
    }
    impl_->mpris.volume_percent = volume_percent;
    impl_->mpris.properties_changed("Volume");
}

} // namespace am::pal
