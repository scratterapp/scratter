// Tests for the Linux MediaControls (src/pal/linux/mpris_controls.cpp), issue
// #56, from the desktop's side: a second D-Bus connection plays the part of
// playerctl or the media widget, calls MPRIS methods and reads properties,
// and the test checks what reaches the callback and what the client sees.
//
// test_media_controls.cpp covers the daemon's side, on both platforms.
//
// The client runs on its own thread with blocking sd-bus calls, because each
// call waits for an answer that only comes once the reactor (on the test's
// thread) has handled it. Each thread only touches its own connection. Catch2's
// CHECK isn't safe to call from two threads, so the client only records what
// it saw, and the checks run after it's done.
//
// These need a D-Bus session bus. CI runs ctest under `dbus-run-session`,
// which starts a private one; with no bus, they skip.

#include "media_controls.hpp"
#include "reactor.hpp"

#include <catch2/catch_test_macros.hpp>

#include <systemd/sd-bus.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using am::common::PlaybackState;
using am::pal::MediaControls;
using am::pal::MediaRequest;
using am::pal::Reactor;

namespace {

using Kind = MediaRequest::Kind;

constexpr const char* kPath = "/org/mpris/MediaPlayer2";
constexpr const char* kPlayer = "org.mpris.MediaPlayer2.Player";

std::string test_name(std::string_view test) {
    return std::string(am::common::kAppName) + "_mpris_test_" + std::string(test);
}

// The client's end: its own connection to the session bus.
struct Client {
    explicit Client(std::string name) : bus_name("org.mpris.MediaPlayer2." + std::move(name)) {
        if (sd_bus_open_user(&bus) < 0) {
            bus = nullptr;
        }
    }
    ~Client() { sd_bus_flush_close_unref(bus); }
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Calls a Player method with no arguments. True if it answered without
    // an error.
    bool call(const char* method) const {
        return sd_bus_call_method(bus, bus_name.c_str(), kPath, kPlayer, method, nullptr, nullptr,
                                  "") >= 0;
    }

    std::string get_string(const char* property) const {
        char* value = nullptr;
        if (sd_bus_get_property_string(bus, bus_name.c_str(), kPath, kPlayer, property, nullptr,
                                       &value) < 0) {
            return "(error)";
        }
        std::string result = value;
        free(value); // NOLINT: sd-bus hands back malloc'd memory
        return result;
    }

    template <typename T> std::optional<T> get(const char* property, char type) const {
        T value{};
        if (sd_bus_get_property_trivial(bus, bus_name.c_str(), kPath, kPlayer, property, nullptr,
                                        type, &value) < 0) {
            return std::nullopt;
        }
        return value;
    }

    // The xesam:title entry of Metadata, or "(none)".
    std::string title() const {
        sd_bus_message* reply = nullptr;
        if (sd_bus_get_property(bus, bus_name.c_str(), kPath, kPlayer, "Metadata", nullptr, &reply,
                                "a{sv}") < 0) {
            return "(error)";
        }
        std::string title = "(none)";
        sd_bus_message_enter_container(reply, 'a', "{sv}");
        while (sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
            const char* key = nullptr;
            sd_bus_message_read(reply, "s", &key);
            if (std::string_view(key) == "xesam:title") {
                const char* value = nullptr;
                sd_bus_message_read(reply, "v", "s", &value);
                title = value;
            } else {
                sd_bus_message_skip(reply, "v");
            }
            sd_bus_message_exit_container(reply);
        }
        sd_bus_message_unref(reply);
        return title;
    }

    sd_bus* bus = nullptr;
    std::string bus_name;
};

// Opens media controls for `test` and runs `client_side` on a thread of its
// own while the reactor runs here, until it's done. The callback records
// every request; the daemon-like `on_request` (optional) runs on each one too.
struct Harness {
    explicit Harness(std::string_view test) : name(test_name(test)) {
        auto created = Reactor::create();
        REQUIRE(created.has_value());
        reactor.emplace(std::move(*created));
        auto opened = MediaControls::open(
            *reactor,
            [this](const MediaRequest& request) {
                requests.push_back(request);
                if (on_request) {
                    on_request(request);
                }
            },
            name);
        if (!opened) {
            WARN("no D-Bus session bus here (" << opened.error().message() << "), skipping");
            return;
        }
        controls.emplace(std::move(*opened));
    }

    // Returns false if the client couldn't connect.
    bool run_client(const std::function<void(const Client&)>& client_side) {
        std::atomic<bool> done{false};
        bool connected = false;
        std::thread client([&] {
            const Client c(name);
            connected = c.bus != nullptr;
            if (connected) {
                client_side(c);
            }
            done.store(true);
            reactor->wake();
        });
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (!done.load() && std::chrono::steady_clock::now() < deadline) {
            CHECK(reactor->run_once(100ms).has_value());
        }
        client.join();
        return connected;
    }

    std::string name;
    std::optional<Reactor> reactor;
    std::optional<MediaControls> controls;
    std::vector<MediaRequest> requests;
    std::function<void(const MediaRequest&)> on_request;
};

} // namespace

TEST_CASE("MPRIS method calls reach the callback", "[mpris]") {
    Harness h("methods");
    if (!h.controls) {
        return;
    }
    std::vector<bool> answered;
    REQUIRE(h.run_client([&](const Client& c) {
        for (const char* method : {"Play", "Pause", "PlayPause", "Stop", "Next", "Previous"}) {
            answered.push_back(c.call(method));
        }
    }));
    CHECK(answered == std::vector<bool>(6, true));
    const std::vector<Kind> expected = {Kind::Play, Kind::Pause, Kind::PlayPause,
                                        Kind::Stop, Kind::Next,  Kind::Previous};
    REQUIRE(h.requests.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CHECK(h.requests[i].kind == expected[i]);
    }
}

TEST_CASE("MPRIS clients see what the daemon sets", "[mpris]") {
    Harness h("properties");
    if (!h.controls) {
        return;
    }

    // Nothing loaded yet.
    std::string status;
    std::string title;
    std::optional<int> can_seek;
    std::optional<int> can_go_next;
    REQUIRE(h.run_client([&](const Client& c) {
        status = c.get_string("PlaybackStatus");
        title = c.title();
        can_seek = c.get<int>("CanSeek", 'b');
        can_go_next = c.get<int>("CanGoNext", 'b');
    }));
    CHECK(status == "Stopped");
    CHECK(title == "(none)");
    CHECK(can_seek == 0);
    CHECK(can_go_next == 0); // no play queue yet

    h.controls->set_track({.title = "Test Track",
                           .artist = "Test Artist",
                           .album = "",
                           .artwork_url = "",
                           .length = 3min});
    h.controls->set_playback_state(PlaybackState::Playing);
    h.controls->set_volume(40);
    h.controls->set_position(12s);

    std::optional<double> volume;
    std::optional<std::int64_t> position;
    REQUIRE(h.run_client([&](const Client& c) {
        status = c.get_string("PlaybackStatus");
        title = c.title();
        volume = c.get<double>("Volume", 'd');
        position = c.get<std::int64_t>("Position", 'x');
        can_seek = c.get<int>("CanSeek", 'b');
    }));
    CHECK(status == "Playing");
    CHECK(title == "Test Track");
    CHECK(volume == 0.4);
    CHECK(position == 12'000'000); // microseconds
    CHECK(can_seek == 1);

    // MPRIS has no "buffering"; it shows as playing.
    h.controls->set_playback_state(PlaybackState::Buffering);
    REQUIRE(h.run_client([&](const Client& c) { status = c.get_string("PlaybackStatus"); }));
    CHECK(status == "Playing");

    h.controls->set_playback_state(PlaybackState::Paused);
    h.controls->clear_track();
    REQUIRE(h.run_client([&](const Client& c) {
        status = c.get_string("PlaybackStatus");
        title = c.title();
    }));
    CHECK(status == "Paused");
    CHECK(title == "(none)");

    CHECK(h.requests.empty());
}

TEST_CASE("a PlayPause from the desktop shows up as a PropertiesChanged", "[mpris]") {
    Harness h("signal");
    if (!h.controls) {
        return;
    }
    h.controls->set_playback_state(PlaybackState::Playing);
    // Act like the daemon: play/pause while playing pauses.
    h.on_request = [&h](const MediaRequest& request) {
        if (request.kind == Kind::PlayPause) {
            h.controls->set_playback_state(PlaybackState::Paused);
        }
    };

    int matched = -1;
    bool answered = false;
    bool heard = false;
    std::string status;
    REQUIRE(h.run_client([&](const Client& c) {
        // Listen for PropertiesChanged from the player, the way the desktop
        // widget does, then press play/pause.
        sd_bus_slot* slot = nullptr;
        matched = sd_bus_match_signal(
            c.bus, &slot, nullptr, kPath, "org.freedesktop.DBus.Properties", "PropertiesChanged",
            [](sd_bus_message* m, void* userdata, sd_bus_error*) {
                const char* interface = nullptr;
                if (sd_bus_message_read(m, "s", &interface) >= 0 &&
                    std::string_view(interface) == kPlayer) {
                    *static_cast<bool*>(userdata) = true;
                }
                return 0;
            },
            &heard);
        answered = c.call("PlayPause");
        // The signal went out before the answer, so it has already arrived;
        // this hands it to the match above.
        while (sd_bus_process(c.bus, nullptr) > 0) {
        }
        status = c.get_string("PlaybackStatus");
        sd_bus_slot_unref(slot);
    }));
    CHECK(matched >= 0);
    CHECK(answered);
    CHECK(heard);
    CHECK(status == "Paused");
    CHECK(h.requests.size() == 1);
}

TEST_CASE("Seek, SetPosition and Volume turn into the header's requests", "[mpris]") {
    Harness h("seek");
    if (!h.controls) {
        return;
    }
    h.controls->set_track(
        {.title = "t", .artist = "", .album = "", .artwork_url = "", .length = 3min});
    h.controls->set_position(10s);

    std::vector<bool> answered;
    REQUIRE(h.run_client([&](const Client& c) {
        const char* bus = c.bus_name.c_str();
        auto seek = [&](std::int64_t offset) {
            answered.push_back(sd_bus_call_method(c.bus, bus, kPath, kPlayer, "Seek", nullptr,
                                                  nullptr, "x", offset) >= 0);
        };
        auto set_position = [&](const char* track, std::int64_t position) {
            answered.push_back(sd_bus_call_method(c.bus, bus, kPath, kPlayer, "SetPosition",
                                                  nullptr, nullptr, "ox", track, position) >= 0);
        };
        auto set_volume = [&](double volume) {
            answered.push_back(sd_bus_set_property(c.bus, bus, kPath, kPlayer, "Volume", nullptr,
                                                   "d", volume) >= 0);
        };
        seek(-5'000'000);  // 5 s back from 10 s
        seek(-20'000'000); // 20 s back stops at 0
        // Go to 1:00 on the current track. Then a stale track id and a spot
        // past the end, which MPRIS says to ignore.
        set_position("/CurrentTrack", 60'000'000);
        set_position("/SomeOtherTrack", 60'000'000);
        set_position("/CurrentTrack", 600'000'000);
        // 0.0 to 1.0 on D-Bus, 0 to 100 here. Too loud stops at 100.
        set_volume(0.25);
        set_volume(7.0);
    }));
    CHECK(answered == std::vector<bool>(7, true));

    REQUIRE(h.requests.size() == 5);
    CHECK(h.requests[0].kind == Kind::Seek);
    CHECK(h.requests[0].position == 5s);
    CHECK(h.requests[1].position == 0s);
    CHECK(h.requests[2].kind == Kind::Seek);
    CHECK(h.requests[2].position == 60s);
    CHECK(h.requests[3].kind == Kind::SetVolume);
    CHECK(h.requests[3].volume_percent == 25);
    CHECK(h.requests[4].volume_percent == 100);
}

TEST_CASE("a name that's already taken is refused", "[mpris]") {
    Harness h("taken");
    if (!h.controls) {
        return;
    }
    auto second = MediaControls::open(*h.reactor, [](const MediaRequest&) {}, h.name);
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error() == std::errc::address_in_use);
}
