// The daemon: plays audio and answers commands until it's told to quit.
//
//   scratterd [--name NAME] [--no-audio]
//             [--test-film | --clear-film] [--widevine-dir DIR] [--no-sandbox]
//
//   --name NAME   listen on endpoint NAME and publish telemetry as
//                 NAME_telemetry_v2 (default: the daemon's own name, which is
//                 what am-ui and amctl look for). The tests use it to run a
//                 daemon of their own next to a real one. It also replaces
//                 the product name in the media-keys bus name
//                 (org.mpris.MediaPlayer2.NAME).
//   --no-audio    don't open the sound card. Everything else works; the play
//                 position just doesn't move. For CI machines with no audio.
//   --test-film   play Google's Widevine test film through CEF instead of the
//                 tone (cef_host/test_film_page.hpp). Needs a daemon built with
//                 CEF (the linux-cef preset) and an installed Widevine CDM.
//   --clear-film  the same film without encryption: CEF and the audio path as
//                 with --test-film, but no Widevine license needed.
//   --widevine-dir DIR
//                 the installed Widevine CDM folder, if it isn't in one of the
//                 usual places (src/pal/linux/browser_files.cpp lists them).
//   --no-sandbox  run CEF's subprocesses without Chromium's sandbox, for
//                 machines where it won't start. Only use it to find out
//                 whether the sandbox is the problem.
//
// Exit codes: 0 after a clean shutdown (Ctrl-C or SIGTERM), 1 if setting
// something up failed, 2 for bad arguments, 3 if a daemon is already running.
//
// Until Apple Music arrives (issue #4) the "music" is a 440 Hz tone, or with
// --test-film a test film played by CEF, and it starts playing as soon as the
// daemon does. amctl, the media keys and the desktop's media widget (MPRIS on
// Linux) can pause it and change its volume; it has no next track and can't
// seek.
//
// Startup, in order:
//
//   1. the reactor, the event loop everything else hangs off
//   2. Ctrl-C/SIGTERM handling, before any other thread exists (see
//      shutdown_signal.hpp for why)
//   3. the IPC listener, which doubles as the "already running?" check
//   4. the audio sink and the audio path between it and the tone, or with
//      --test-film, CEF and the page that feeds the audio path instead
//   5. the telemetry page in shared memory, and the control server
//   6. media keys, which carry on without if the desktop has none
//   7. a timer that keeps the ring buffer topped up and publishes telemetry,
//      every 10 ms while playing and every 250 ms while paused or stopped (a
//      command that changes something publishes it at once), and stops the
//      audio output once a pause has faded out, then the reactor runs until
//      a quit signal stops it
//
// Shutdown is the same list backwards, mostly done by destructors.

#include "audio_sink.hpp"
#include "common/app_name.hpp"
#include "common/telemetry_shm.hpp"
#include "daemon/audio_engine/audio_path.hpp"
#include "daemon/audio_engine/sine_generator.hpp"
#include "daemon/cef_host/cef_host.hpp"
#include "daemon/control_server.hpp"
#include "ipc_channel.hpp"
#include "media_controls.hpp"
#include "reactor.hpp"
#include "realtime.hpp"
#include "shm_mapping.hpp"
#include "shutdown_signal.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace {

using namespace std::chrono_literals;
using am::audio::AudioPath;
using am::common::PlaybackState;
using am::common::TelemetryPage;
using am::common::TelemetrySnapshot;

constexpr std::uint32_t kSampleRate = 48000;
// 100 ticks a second: the ring drains 480 frames per tick at 48 kHz, far
// below the 4096 it holds, and the UI gets telemetry about as often as it
// draws a frame.
constexpr auto kTickInterval = 10ms;
// Paused or stopped, nothing moves, so the daemon wakes 4 times a second
// instead of 100. Every wake-up costs a little power, and a daemon that sits
// paused in the background all day shouldn't keep the CPU busy
// (docs/learning/performance.md). A command that starts playback runs the
// tick at once, so play doesn't wait for the slow timer.
constexpr auto kIdleTickInterval = 250ms;

enum ExitCode : int {
    kExitOk = 0,
    kExitSetupFailed = 1,
    kExitBadArguments = 2,
    kExitAlreadyRunning = 3,
};

struct Options {
    std::string name{am::common::kDaemonName};
    std::string media_name{am::common::kAppName};
    bool audio = true;
    bool test_film = false;
    am::daemon::CefHostOptions cef;
};

std::optional<Options> parse_arguments(std::span<char*> args) {
    Options options;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "--name" && i + 1 < args.size()) {
            options.name = args[++i];
            options.media_name = options.name;
        } else if (arg == "--no-audio") {
            options.audio = false;
        } else if (arg == "--test-film") {
            options.test_film = true;
        } else if (arg == "--clear-film") {
            options.test_film = true;
            options.cef.clear_film = true;
        } else if (arg == "--widevine-dir" && i + 1 < args.size()) {
            options.cef.widevine_dir = args[++i];
        } else if (arg == "--no-sandbox") {
            options.cef.sandbox = false;
        } else {
            return std::nullopt;
        }
    }
    return options;
}

TelemetrySnapshot make_snapshot(const AudioPath& audio, bool audio_open) {
    TelemetrySnapshot s{};
    s.pts_micros = audio.position_micros();
    s.ring_buffer_fill_pct = audio.ring_fill();
    s.clock_drift_ppm = audio.drift_ppm();
    s.sample_rate_hz = audio_open ? audio.sample_rate_hz() : 0;
    s.playback_state = audio.state();
    s.volume_percent = audio.volume();
    return s;
}

} // namespace

int main(int argc, char** argv) {
    const auto options = parse_arguments(std::span(argv, static_cast<std::size_t>(argc)));
    if (!options) {
        std::fprintf(stderr,
                     "usage: %s [--name NAME] [--no-audio] [--test-film | --clear-film] "
                     "[--widevine-dir DIR] [--no-sandbox]\n",
                     argv[0]);
        return kExitBadArguments;
    }
    const char* name = options->name.c_str();

    // 1. The reactor.
    auto reactor = am::pal::Reactor::create();
    if (!reactor) {
        std::fprintf(stderr, "couldn't start the event loop: %s\n",
                     reactor.error().message().c_str());
        return kExitSetupFailed;
    }

    // 2. Ctrl-C and SIGTERM stop the reactor, and everything below unwinds.
    auto shutdown = am::pal::ShutdownSignal::watch(*reactor, [&] {
        std::fprintf(stderr, "quitting\n");
        reactor->stop();
    });
    if (!shutdown) {
        std::fprintf(stderr, "couldn't catch Ctrl-C: %s\n", shutdown.error().message().c_str());
        return kExitSetupFailed;
    }

    // 3. The listener. Only one daemon can listen on a name.
    auto listener = am::pal::IpcListener::listen(*reactor, options->name);
    if (!listener) {
        if (listener.error() == std::errc::address_in_use) {
            std::fprintf(stderr, "%s is already running\n", name);
            return kExitAlreadyRunning;
        }
        std::fprintf(stderr, "couldn't listen on %s: %s\n", name,
                     listener.error().message().c_str());
        return kExitSetupFailed;
    }

    // 4. Audio. The sink's render callback reads `audio` through this
    // pointer, which is filled in before the sink starts. `audio` is declared
    // first so it's destroyed last, after the sink has stopped calling it.
    std::unique_ptr<AudioPath> audio;
    std::optional<am::pal::AudioSink> sink;
    std::uint32_t sample_rate = kSampleRate;
    if (options->audio) {
        auto opened = am::pal::AudioSink::open(
            {.sample_rate_hz = kSampleRate, .channels = AudioPath::kChannels},
            [&audio](std::span<float> out) noexcept { audio->render(out); });
        if (!opened) {
            std::fprintf(stderr,
                         "couldn't open the audio output: %s (--no-audio runs without it)\n",
                         opened.error().message().c_str());
            return kExitSetupFailed;
        }
        if (opened->format().channels != AudioPath::kChannels) {
            std::fprintf(stderr, "the audio output gave us %u channels, and only stereo works\n",
                         opened->format().channels);
            return kExitSetupFailed;
        }
        sample_rate = opened->format().sample_rate_hz;
        sink.emplace(std::move(*opened));
    }
    audio = std::make_unique<AudioPath>(sample_rate);
    am::audio::SineGenerator sine(sample_rate);
    if (!options->test_film) {
        audio->top_up(sine); // full before the first callback asks for anything
    }

    // A swapped-out page of the ring buffer would make the audio thread wait
    // for the disk. Not fatal if the OS says no, just less safe.
    const bool locked = am::pal::lock_memory(audio->ring_bytes()).has_value();
    if (!locked) {
        std::fprintf(stderr, "warning: couldn't lock the ring buffer in RAM\n");
    }

    if (sink) {
        if (auto started = sink->start(); !started) {
            std::fprintf(stderr, "couldn't start the audio output: %s\n",
                         started.error().message().c_str());
            return kExitSetupFailed;
        }
    }

    // With --test-film, CEF's audio thread fills the ring buffer instead of the
    // tick. Declared after `audio` and the sink so it's destroyed before them.
    std::optional<am::daemon::CefHost> cef;
    if (options->test_film) {
        auto options_cef = options->cef;
        options_cef.sample_rate_hz = sample_rate;
        auto started = am::daemon::CefHost::start(options_cef, *audio);
        if (!started) {
            std::fprintf(stderr, "couldn't start CEF: %s\n", started.error().c_str());
            return kExitSetupFailed;
        }
        cef.emplace(std::move(*started));
    }

    // 5. Telemetry. The page is built in place at the start of the shared
    // region; readers map the same bytes read-only.
    const std::string shm_name = options->name + "_telemetry_v2";
    auto shm = am::pal::ShmMapping::create(shm_name, am::common::kTelemetryPageSize);
    if (!shm) {
        std::fprintf(stderr, "couldn't create the telemetry page %s: %s\n", shm_name.c_str(),
                     shm.error().message().c_str());
        return kExitSetupFailed;
    }
    TelemetryPage* telemetry =
        std::construct_at(reinterpret_cast<TelemetryPage*>(shm->writable_bytes().data()));
    telemetry->write(make_snapshot(*audio, sink.has_value()));

    // Republish telemetry as soon as a command changes something, not just on
    // the next tick, so `amctl pause; amctl status` shows the new state. The
    // media widget hears about it at the same time.
    std::optional<am::pal::MediaControls> media;
    std::function<void()> tick; // step 7
    auto publish = [&] {
        if (cef) {
            cef->set_state(audio->state()); // the page plays, pauses or stops to match
        }
        if (media) {
            media->set_playback_state(audio->state());
            media->set_volume(audio->volume());
        }
        tick(); // publishes telemetry, and speeds up or slows down to match the new state
    };
    am::daemon::ControlServer server(std::move(*listener), *audio, publish);

    // 6. Media keys. A key press works like the same amctl command: change
    // the audio, publish, and tell every connected client.
    auto on_media_key = [&](const am::pal::MediaRequest& request) {
        using Kind = am::pal::MediaRequest::Kind;
        PlaybackState state = audio->state();
        std::uint8_t volume = audio->volume();
        switch (request.kind) {
        case Kind::Play:
            state = PlaybackState::Playing;
            break;
        case Kind::Pause:
            state = PlaybackState::Paused;
            break;
        case Kind::PlayPause:
            state =
                state == PlaybackState::Playing ? PlaybackState::Paused : PlaybackState::Playing;
            break;
        case Kind::Stop:
            state = PlaybackState::Stopped;
            break;
        case Kind::SetVolume:
            volume = request.volume_percent;
            break;
        case Kind::Next:
        case Kind::Previous:
        case Kind::Seek:
            return; // a tone has no next track and nowhere to seek to
        }
        if (state == audio->state() && volume == audio->volume()) {
            return;
        }
        audio->set_state(state);
        audio->set_volume(volume);
        publish();
        server.broadcast_state();
    };
    if (auto opened = am::pal::MediaControls::open(*reactor, on_media_key, options->media_name)) {
        media.emplace(std::move(*opened));
        media->set_volume(audio->volume());
        const char* title = !cef                      ? "440 Hz test tone"
                            : options->cef.clear_film ? "Unencrypted test film (Tears of Steel)"
                                                      : "Widevine test film (Tears of Steel)";
        media->set_track({.title = title,
                          .artist = std::string(am::common::kAppName),
                          .album = {},
                          .artwork_url = {},
                          .length = {}});
        media->set_playback_state(audio->state());
    } else {
        std::fprintf(stderr, "no media keys: %s\n", opened.error().message().c_str());
    }

    // 7. The tick. The reactor has one timer, and it fires once, so the tick
    // sets it again each time it runs. Setting it replaces the old deadline,
    // which is how publish() moves the next tick when the state changes.
    //
    // The tick also stops the sink while nothing plays, so PipeWire's audio
    // thread (and the sound card, after a few seconds) can sleep too. It
    // waits until the audio path has faded out (AudioPath::quiet()), so the
    // last thing the sound card got was silence. Play starts it again at
    // once, because publish() runs the tick.
    bool sink_running = sink.has_value();
    tick = [&] {
        // Running and now quiet: stop it. Stopped and no longer quiet: start it.
        if (sink && audio->quiet() == sink_running) {
            sink_running = !sink_running;
            auto changed = sink_running ? sink->start() : sink->stop();
            if (!changed) {
                std::fprintf(stderr, "couldn't %s the audio output: %s\n",
                             sink_running ? "restart" : "pause", changed.error().message().c_str());
            }
        }
        if (!cef) {
            audio->top_up(sine);
        }
        telemetry->write(make_snapshot(*audio, sink.has_value()));
        if (media) {
            media->set_position(
                std::chrono::microseconds{static_cast<std::int64_t>(audio->position_micros())});
        }
        const auto next =
            audio->state() == PlaybackState::Playing ? kTickInterval : kIdleTickInterval;
        (void)reactor->set_timer(next, [&tick] { tick(); });
    };
    tick();

    if (cef) {
        std::fprintf(stderr,
                     "%s: %s the %s test film at %u Hz, listening as %s. Ctrl-C quits.\n",
                     am::common::kDaemonName.data(),
                     sink ? "playing" : "pretending to play (--no-audio)",
                     options->cef.clear_film ? "unencrypted" : "Widevine", sample_rate, name);
    } else {
        std::fprintf(
            stderr, "%s: %s a %g Hz tone at %u Hz, listening as %s. Ctrl-C quits.\n",
            am::common::kDaemonName.data(), sink ? "playing" : "pretending to play (--no-audio)",
            static_cast<double>(am::audio::SineGenerator::kFrequencyHz), sample_rate, name);
    }

    auto ran = reactor->run();

    // Shutdown. Stop the audio first so nothing touches the ring buffer
    // while it's unlocked and freed; the destructors do the rest (the
    // socket file and the shared-memory name are removed).
    reactor->cancel_timer();
    media.reset(); // the player leaves the desktop's media widget
    cef.reset();   // closes the page and shuts CEF down, so nothing writes to the ring
    if (sink) {
        (void)sink->stop();
    }
    if (locked) {
        (void)am::pal::unlock_memory(audio->ring_bytes());
    }
    telemetry->write(TelemetrySnapshot{}); // anyone still mapped sees Stopped

    if (!ran) {
        std::fprintf(stderr, "the event loop failed: %s\n", ran.error().message().c_str());
        return kExitSetupFailed;
    }
    std::fprintf(stderr, "stopped cleanly, %llu late audio buffers, %llu frames dropped\n",
                 static_cast<unsigned long long>(audio->late_buffers()),
                 static_cast<unsigned long long>(audio->dropped_frames()));
    return kExitOk;
}
