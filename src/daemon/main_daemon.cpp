// The daemon: plays audio and answers commands until it's told to quit.
//
//   scratterd [--name NAME] [--no-audio] [--tone-length SECONDS]
//             [--test-film | --clear-film] [--widevine-dir DIR] [--no-sandbox]
//             [--ui PROGRAM]
//
//   --name NAME   listen on endpoint NAME and publish telemetry as
//                 NAME_telemetry_v2 (default: the daemon's own name, which is
//                 what am-ui and amctl look for). The tests use it to run a
//                 daemon of their own next to a real one. It also replaces
//                 the product name in the media-keys bus name
//                 (org.mpris.MediaPlayer2.NAME).
//   --no-audio    don't open the sound card. Everything else works; the play
//                 position just doesn't move. For CI machines with no audio.
//   --tone-length SECONDS
//                 give the tone a length, so it can be seeked in and goes
//                 round to 0 at the end like the films. The tests use it to
//                 check seeking without CEF. Without it the tone has no end,
//                 and a seek on it is refused.
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
//   --ui PROGRAM  what to start when the desktop asks to see the player (MPRIS
//                 Raise) and no window is open. Default: the am-ui next to
//                 this daemon. The tests point it at a stand-in.
//
// Exit codes: 0 after a clean shutdown (Ctrl-C or SIGTERM), 1 if setting
// something up failed, 2 for bad arguments, 3 if a daemon is already running.
//
// Until Apple Music arrives (issue #4) the "music" is a 440 Hz tone, or with
// --test-film a test film played by CEF, and it starts playing as soon as the
// daemon does. amctl, the media keys and the desktop's media widget (MPRIS on
// Linux) can pause it and change its volume, and a client can switch between
// the tone and the two films while it runs (LoadTrackReq, `amctl load`). A
// daemon built with CEF starts CEF the first time a film is asked for. The
// films can be seeked in (SeekReq, `amctl seek`, the window's bar, MPRIS);
// the tone only with --tone-length. There is no next track yet. Clicking the
// player in the media widget brings the window to the front, or starts it
// if none is open (issue #41).
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
#include "daemon/audio_engine/tone_pacer.hpp"
#include "daemon/cef_host/cef_host.hpp"
#include "daemon/cef_host/test_film_page.hpp"
#include "daemon/control_server.hpp"
#include "ipc_channel.hpp"
#include "browser_files.hpp"
#include "media_controls.hpp"
#include "process_launch.hpp"
#include "reactor.hpp"
#include "realtime.hpp"
#include "shm_mapping.hpp"
#include "shutdown_signal.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace std::chrono_literals;
using am::audio::AudioPath;
using am::common::PlaybackState;
using am::common::TelemetryPage;
using am::common::TelemetrySnapshot;
using am::common::TrackSource;
using std::chrono::microseconds;

constexpr std::uint32_t kSampleRate = 48000;
// 100 ticks a second: the ring drains 480 frames per tick at 48 kHz, far
// below the 4096 it holds, and the UI gets telemetry about as often as it
// draws a frame.
constexpr auto kTickInterval = 10ms;
// Paused or stopped, nothing moves, so the daemon wakes 4 times a second
// instead of 100. Every wake-up costs a little power, and a daemon that sits
// paused in the background all day shouldn't keep the CPU busy
// (docs/BENCHMARKS.md). A command that starts playback runs the
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
    TrackSource source = TrackSource::Tone; // what plays first
    microseconds tone_length{0};            // 0: the tone has no end
    std::string ui_program;                 // --ui; empty: am-ui next to the daemon
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
        } else if (arg == "--tone-length" && i + 1 < args.size()) {
            const std::string_view text = args[++i];
            unsigned seconds = 0;
            const auto [end, error] =
                std::from_chars(text.data(), text.data() + text.size(), seconds);
            if (error != std::errc{} || end != text.data() + text.size() || seconds == 0) {
                return std::nullopt;
            }
            options.tone_length = std::chrono::seconds{seconds};
        } else if (arg == "--test-film") {
            options.source = TrackSource::WidevineFilm;
        } else if (arg == "--clear-film") {
            options.source = TrackSource::ClearFilm;
        } else if (arg == "--widevine-dir" && i + 1 < args.size()) {
            options.cef.widevine_dir = args[++i];
        } else if (arg == "--no-sandbox") {
            options.cef.sandbox = false;
        } else if (arg == "--ui" && i + 1 < args.size()) {
            options.ui_program = args[++i];
        } else {
            return std::nullopt;
        }
    }
    return options;
}

bool is_film(TrackSource source) { return source != TrackSource::Tone; }

// What the media widget, amctl watch and the window show.
const char* title_of(TrackSource source) {
    switch (source) {
    case TrackSource::Tone:
        return "440 Hz test tone";
    case TrackSource::ClearFilm:
        return "Unencrypted test film (Tears of Steel)";
    case TrackSource::WidevineFilm:
        return "Widevine test film (Tears of Steel)";
    }
    return "";
}

std::uint64_t frames_in(microseconds time, std::uint32_t sample_rate) {
    return static_cast<std::uint64_t>(time.count()) * sample_rate / 1'000'000;
}

TelemetrySnapshot make_snapshot(const AudioPath& audio, bool audio_open, microseconds length) {
    TelemetrySnapshot s{};
    s.pts_micros = audio.position_micros();
    s.duration_micros = static_cast<std::uint64_t>(length.count());
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
                     "usage: %s [--name NAME] [--no-audio] [--tone-length SECONDS] "
                     "[--test-film | --clear-film] [--widevine-dir DIR] [--no-sandbox] "
                     "[--ui PROGRAM]\n",
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
        if (listener.error() == std::errc::permission_denied ||
            listener.error() == std::errc::not_a_directory) {
            std::fprintf(stderr, "the folder for the socket ($XDG_RUNTIME_DIR on Linux) must "
                                 "be yours and closed to everyone else (mode 0700)\n");
        }
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
    TrackSource source = options->source; // what plays now; LoadTrackReq changes it
    // How long a source is. The films loop, and so does a tone with a length.
    auto length_of = [&](TrackSource of) {
        return is_film(of) ? microseconds{am::daemon::kTestFilmLength} : options->tone_length;
    };
    audio->set_length_frames(frames_in(length_of(source), sample_rate));
    if (!is_film(source)) {
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

    // While a film plays, CEF's audio thread fills the ring buffer instead of
    // the tick. CEF starts with the daemon for --test-film or --clear-film,
    // otherwise the first time a client loads a film, and then stays up until
    // the daemon quits (CEF can't start twice in one process). Declared after
    // `audio` and the sink so it's destroyed before them.
    std::optional<am::daemon::CefHost> cef;
    bool page_has_clear_film = false; // which film the page has loaded
    auto start_cef = [&](TrackSource film) {
        auto options_cef = options->cef;
        options_cef.sample_rate_hz = sample_rate;
        options_cef.clear_film = film == TrackSource::ClearFilm;
        auto started = am::daemon::CefHost::start(options_cef, *audio);
        if (!started) {
            std::fprintf(stderr, "couldn't start CEF: %s\n", started.error().c_str());
            return false;
        }
        cef.emplace(std::move(*started));
        page_has_clear_film = options_cef.clear_film;
        // On Windows, CEF has just put Chromium's own Ctrl-C handler in front
        // of ours, and it would end the process without the cleanup below.
        if (auto reclaimed = shutdown->reclaim(); !reclaimed) {
            std::fprintf(stderr, "couldn't take Ctrl-C back from CEF: %s\n",
                         reclaimed.error().message().c_str());
            return false;
        }
        return true;
    };
    if (is_film(source) && !start_cef(source)) {
        return kExitSetupFailed;
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
    telemetry->write(make_snapshot(*audio, sink.has_value(), length_of(source)));

    // Republish telemetry as soon as a command changes something, not just on
    // the next tick, so `amctl pause; amctl status` shows the new state. The
    // media widget hears about it at the same time.
    std::optional<am::pal::MediaControls> media;
    std::function<void()> tick; // step 7
    // The page and the media widget follow the audio path's state. The page
    // only plays while a film is the source; with the tone it stays stopped.
    auto sync_outputs = [&] {
        if (cef) {
            cef->set_state(is_film(source) ? audio->state() : PlaybackState::Stopped);
        }
        if (media) {
            media->set_playback_state(audio->state());
            media->set_volume(audio->volume());
        }
    };
    auto publish = [&] {
        sync_outputs();
        tick(); // publishes telemetry, and speeds up or slows down to match the new state
    };

    // Switching sources (LoadTrackReq). The old source fades out and the
    // audio path throws its audio away, as for a Stop. Then the ring buffer
    // changes producer, and the new source starts in the play state the old
    // one had. The fade-out takes a tick or two, so the tick finishes the
    // switch (finish_switch below) once the audio path is quiet().
    struct Switch {
        TrackSource to;
        PlaybackState resume;
    };
    std::optional<Switch> switching;
    // A seek under way; see `seek` below.
    struct Seek {
        microseconds to;
        PlaybackState resume;
    };
    std::optional<Seek> seeking;
    auto load_track = [&](TrackSource to) {
        if (is_film(to) && !am::daemon::CefHost::built_in()) {
            return am::common::CommandStatus::NotSupported;
        }
        if (is_film(to) && !cef) {
            std::fprintf(stderr, "starting CEF for the %s\n", title_of(to));
            if (!start_cef(to)) {
                return am::common::CommandStatus::Failed;
            }
        }
        // A second load during a switch keeps the first one's play state, and
        // a load during a seek the state from before the seek. The seek is
        // dropped: the new source starts at 0.
        const PlaybackState resume = switching ? switching->resume
                                     : seeking ? seeking->resume
                                               : audio->state();
        seeking.reset();
        switching = Switch{.to = to, .resume = resume};
        audio->let_page_write(false); // from here the page can't touch the ring
        audio->set_state(PlaybackState::Stopped);
        audio->discard(); // also empties what the tone left in the ring while stopped
        sync_outputs();   // the page stops too
        // The tick takes it from here, starting right after this callback, so
        // the request's ack always goes out before the TrackChangedEvent.
        (void)reactor->set_timer(std::chrono::milliseconds::zero(), [&tick] { tick(); });
        return am::common::CommandStatus::Ok;
    };
    auto current_track = [&] {
        return am::common::TrackChangedBody{
            .catalog_id = 0,
            .duration_micros = static_cast<std::uint64_t>(length_of(source).count()),
            .source = source,
            .reserved = {},
            .title = am::common::make_track_title(title_of(source)),
        };
    };

    // Seeking (SeekReq, the media widget, the window's bar). Like a switch
    // without the change of source: pause, wait for the audio path to fade
    // out and throw away the old audio, then move the position and play on
    // in the state from before. Without that wait, the ring buffer's 85 ms
    // of audio from before the jump would play first. The page is paused
    // first and jumps at once, so it has new audio ready when it plays again.
    std::optional<am::daemon::ControlServer> server; // finish_seek() announces through it
    auto seek = [&](microseconds to) {
        const microseconds length = length_of(source);
        if (length == microseconds::zero()) {
            return am::common::CommandStatus::NotSupported; // the tone has no length
        }
        if (switching) {
            // The position belongs to a source that's on its way out.
            return am::common::CommandStatus::Failed;
        }
        // Past the end is the end. The test sources loop, so for them the
        // end is the start again.
        to = std::clamp(to, microseconds::zero(), length);
        // A stopped player that's seeked stays paused at that point instead of
        // starting over from 0 on play. A second seek before the first is done
        // keeps the first one's play state.
        PlaybackState resume = seeking ? seeking->resume : audio->state();
        if (resume == PlaybackState::Stopped) {
            resume = PlaybackState::Paused;
        }
        seeking = Seek{.to = to, .resume = resume};
        audio->set_state(PlaybackState::Paused);
        audio->discard(); // throw away the ring's audio even if already paused
        sync_outputs();   // pauses the page...
        if (cef && is_film(source)) {
            cef->seek(to); // ...then moves it
        }
        // As for a switch: the ack goes out first, then the tick takes over.
        (void)reactor->set_timer(std::chrono::milliseconds::zero(), [&tick] { tick(); });
        return am::common::CommandStatus::Ok;
    };
    server.emplace(
        std::move(*listener), *audio, publish,
        am::daemon::TrackHooks{.load = load_track, .current = current_track, .seek = seek});

    auto show_track = [&] {
        if (media) {
            media->set_track({.title = title_of(source),
                              .artist = std::string(am::common::kAppName),
                              .album = {},
                              .artwork_url = {},
                              .length = length_of(source)});
        }
    };

    // The tick calls this once the old source has faded out and render() has
    // emptied the ring buffer.
    auto finish_switch = [&] {
        const Switch done = *switching;
        switching.reset();
        if (is_film(done.to)) {
            const bool clear = done.to == TrackSource::ClearFilm;
            if (clear != page_has_clear_film) {
                cef->load_film(clear);
                page_has_clear_film = clear;
            }
            audio->let_page_write(true);
        } else {
            audio->top_up(sine); // full before render() asks, like at startup
        }
        source = done.to;
        // render() rewound when it saw the stop; without a sound card nothing
        // did. Either way the new source starts at 0.
        audio->set_position_frames(0);
        audio->set_length_frames(frames_in(length_of(source), sample_rate));
        audio->set_state(done.resume);
        std::fprintf(stderr, "switched to the %s\n", title_of(source));
        show_track();
        sync_outputs();
        server->broadcast_state();
        server->broadcast_track();
    };

    // The tick calls this once a seek's fade-out is over.
    auto finish_seek = [&] {
        const Seek done = *seeking;
        seeking.reset();
        audio->set_position_frames(frames_in(done.to, sample_rate));
        if (!is_film(source)) {
            audio->top_up(sine);
        }
        audio->set_state(done.resume);
        sync_outputs(); // plays the page again, if it was playing
        if (media) {
            media->seeked(microseconds{static_cast<std::int64_t>(audio->position_micros())});
        }
        // The page first, so a client that reads it as soon as the event
        // arrives (`amctl seek` then `amctl status`) sees the new position.
        telemetry->write(make_snapshot(*audio, sink.has_value(), length_of(source)));
        server->broadcast_state();
    };

    // MPRIS Raise: bring the open window forward, or start one. A second
    // click while the new window is still starting (it takes a moment to
    // connect) shouldn't start a second window, hence the wait.
    std::optional<std::chrono::steady_clock::time_point> last_launch;
    auto raise_window = [&] {
        if (server->raise_windows() > 0) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (last_launch && now - *last_launch < 5s) {
            return;
        }
        std::filesystem::path program = options->ui_program;
        if (program.empty()) {
            auto dir = am::pal::executable_dir();
            if (!dir) {
                std::fprintf(stderr, "can't find the window program: %s\n",
                             dir.error().message().c_str());
                return;
            }
            program = *dir / AM_UI_NAME;
        }
        // A daemon under another name (--name) tells the window which one to
        // connect to; the default daemon is what am-ui looks for anyway.
        std::vector<std::string> args;
        if (options->name != am::common::kDaemonName) {
            args = {"--name", options->name};
        }
        if (auto started = am::pal::launch_ui(*reactor, program, args); !started) {
            std::fprintf(stderr, "couldn't start the window (%s): %s\n", program.string().c_str(),
                         started.error().message().c_str());
            return;
        }
        last_launch = now;
        std::fprintf(stderr, "started the window: %s\n", program.string().c_str());
    };

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
        case Kind::Seek:
            (void)seek(request.position); // finish_seek() tells the widget and the clients
            return;
        case Kind::Next:
        case Kind::Previous:
            return; // the test sources have no next track
        case Kind::Raise:
            raise_window();
            return;
        }
        if (state == audio->state() && volume == audio->volume()) {
            return;
        }
        audio->set_state(state);
        audio->set_volume(volume);
        publish();
        server->broadcast_state();
    };
    if (auto opened = am::pal::MediaControls::open(*reactor, on_media_key, options->media_name)) {
        media.emplace(std::move(*opened));
        media->set_volume(audio->volume());
        show_track();
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
    am::audio::TonePacer tone_pacer(sample_rate);
    // Late buffers and dropped frames are glitches you can hear, so the tick
    // says when there are new ones, at most once a second so a bad minute
    // doesn't flood the log. tools/relaunch/relaunch.py reads these lines to
    // tell whether opening and closing the window made the sound skip.
    std::uint64_t late_reported = 0;
    std::uint64_t dropped_reported = 0;
    auto glitch_reported_at = std::chrono::steady_clock::now();
    tick = [&] {
        if (switching) {
            if (audio->state() != PlaybackState::Stopped) {
                // A play or pause arrived during the switch: carry on in that
                // state once it's done, and keep the old source stopped.
                switching->resume = audio->state();
                audio->set_state(PlaybackState::Stopped);
            }
            // Without a sound card nothing runs render(), so there's nothing
            // to wait for.
            if (!sink || audio->quiet()) {
                finish_switch();
            }
        }
        if (seeking) {
            if (audio->state() != PlaybackState::Paused) {
                // Play, pause or stop during the seek: that's the state to
                // carry on in. The audio stays paused until the seek is done.
                seeking->resume = audio->state();
                audio->set_state(PlaybackState::Paused);
            }
            if (!sink || audio->quiet()) {
                finish_seek();
            }
        }
        // Running and now quiet: stop it. Stopped and no longer quiet: start it.
        if (sink && audio->quiet() == sink_running) {
            sink_running = !sink_running;
            auto changed = sink_running ? sink->start() : sink->stop();
            if (!changed) {
                std::fprintf(stderr, "couldn't %s the audio output: %s\n",
                             sink_running ? "restart" : "pause", changed.error().message().c_str());
            }
        }
        // The tone. While it plays through a sound card it's written at the
        // sample rate by the clock, like a real source (tone_pacer.hpp, #85).
        // Otherwise, and when it starts playing, the ring is filled to the
        // target so play starts at once. Without a sound card nothing reads
        // the ring, so it's just kept at the target.
        if (!is_film(source) && !switching && !seeking) {
            if (sink && audio->state() == PlaybackState::Playing) {
                const auto now = am::audio::TonePacer::Clock::now();
                if (tone_pacer.running()) {
                    audio->write_tone(sine, tone_pacer.due(now));
                } else {
                    audio->top_up(sine);
                    tone_pacer.start(now);
                }
            } else {
                tone_pacer.stop();
                audio->top_up(sine);
            }
        } else {
            tone_pacer.stop();
        }
        telemetry->write(make_snapshot(*audio, sink.has_value(), length_of(source)));
        const std::uint64_t late = audio->late_buffers();
        const std::uint64_t dropped = audio->dropped_frames();
        const auto now = std::chrono::steady_clock::now();
        if ((late != late_reported || dropped != dropped_reported) &&
            now - glitch_reported_at >= 1s) {
            std::fprintf(stderr, "audio: %llu late audio buffers, %llu frames dropped so far\n",
                         static_cast<unsigned long long>(late),
                         static_cast<unsigned long long>(dropped));
            late_reported = late;
            dropped_reported = dropped;
            glitch_reported_at = now;
        }
        if (media) {
            media->set_position(
                std::chrono::microseconds{static_cast<std::int64_t>(audio->position_micros())});
        }
        // A switch or seek waits for render(), so it ticks fast like playing does.
        const bool waiting = switching || seeking;
        const auto next =
            audio->state() == PlaybackState::Playing || waiting ? kTickInterval : kIdleTickInterval;
        (void)reactor->set_timer(next, [&tick] { tick(); });
    };
    tick();

    std::fprintf(stderr, "%s: %s the %s at %u Hz, listening as %s. Ctrl-C quits.\n",
                 am::common::kDaemonName.data(),
                 sink ? "playing" : "pretending to play (--no-audio)", title_of(source),
                 sample_rate, name);

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
