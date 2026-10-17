// amctl: a small command-line remote for the daemon, like mpc for mpd.
// It lets you drive the daemon by hand (and from tests) without am-ui, and
// it can do everything am-ui can ask the daemon for (see coverage() in
// amctl_args.hpp).
//
//   amctl pause          sends PauseReq and prints the daemon's answer
//   amctl volume 30      sends SetVolumeReq
//   amctl ping           sends Ping and prints how long the Pong took
//   amctl watch          prints each PlaybackStateEvent as it arrives
//   amctl status         reads the telemetry page; no IPC at all
//
// Run it with no arguments for the full list. Exit codes: 0 when the daemon
// did what was asked, 1 when it said no or couldn't be reached, 2 for bad
// arguments.
//
// One command per run: connect, say hello, send the request, wait for the
// message that answers it, print it, disconnect.

#include "amctl/amctl_args.hpp"
#include "common/ipc_messages.hpp"
#include "common/telemetry_shm.hpp"
#include "ipc_channel.hpp"
#include "reactor.hpp"
#include "shm_mapping.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using namespace std::chrono_literals;
using am::amctl::Arguments;
using am::amctl::Command;
using namespace am::common;
using am::pal::IoStatus;
using am::pal::IpcChannel;
using am::pal::Reactor;
using Clock = std::chrono::steady_clock;

// The daemon answers in well under a millisecond. Two seconds means it's stuck.
constexpr auto kReplyTimeout = 2s;

enum ExitCode : int {
    kExitOk = 0,
    kExitFailed = 1,
    kExitBadArguments = 2,
};

std::string_view state_name(PlaybackState state) {
    switch (state) {
    case PlaybackState::Stopped:
        return "stopped";
    case PlaybackState::Playing:
        return "playing";
    case PlaybackState::Paused:
        return "paused";
    case PlaybackState::Buffering:
        return "buffering";
    }
    return "unknown";
}

std::string_view status_name(CommandStatus status) {
    switch (status) {
    case CommandStatus::Ok:
        return "ok";
    case CommandStatus::BadRequest:
        return "the daemon didn't understand the request";
    case CommandStatus::NotSupported:
        return "the daemon can't do that yet";
    case CommandStatus::Failed:
        return "the daemon tried and failed";
    }
    return "unknown answer";
}

bool is_no_daemon(std::error_code error) {
    return error == std::errc::connection_refused || error == std::errc::no_such_file_or_directory;
}

// "m:ss", e.g. 3:07.
std::string minutes_seconds(std::uint64_t micros) {
    const std::uint64_t seconds = micros / 1'000'000;
    char text[32];
    std::snprintf(text, sizeof text, "%llu:%02llu", static_cast<unsigned long long>(seconds / 60),
                  static_cast<unsigned long long>(seconds % 60));
    return text;
}

// ---- amctl status ------------------------------------------------------------

int show_status(const Arguments& args) {
    const std::string shm_name = args.name + "_telemetry_v2";
    auto shm = am::pal::ShmMapping::open_read_only(shm_name, kTelemetryPageSize);
    if (!shm) {
        if (shm.error() == std::errc::no_such_file_or_directory) {
            std::fprintf(stderr, "no daemon running as %s\n", args.name.c_str());
        } else {
            std::fprintf(stderr, "couldn't open the telemetry page %s: %s\n", shm_name.c_str(),
                         shm.error().message().c_str());
        }
        return kExitFailed;
    }
    // The daemon built a TelemetryPage at the start of this region. read()
    // retries until it gets a snapshot that wasn't being written at the time.
    const auto& page = *reinterpret_cast<const TelemetryPage*>(shm->bytes().data());
    const TelemetrySnapshot s = page.read();

    if (args.json) {
        // Every value is a number or one of the fixed state names, so nothing
        // needs escaping. sample_rate_hz is 0 when there's no audio output.
        std::printf(
            "{\"state\":\"%s\",\"position_micros\":%llu,\"volume_percent\":%u,"
            "\"sample_rate_hz\":%u,\"ring_buffer_fill\":%.3f,\"clock_drift_ppm\":%.3f}\n",
            state_name(s.playback_state).data(), static_cast<unsigned long long>(s.pts_micros),
            static_cast<unsigned>(s.volume_percent), s.sample_rate_hz,
            static_cast<double>(s.ring_buffer_fill_pct), static_cast<double>(s.clock_drift_ppm));
        return kExitOk;
    }

    std::printf("state:    %s\n", state_name(s.playback_state).data());
    std::printf("position: %s\n", minutes_seconds(s.pts_micros).c_str());
    std::printf("volume:   %u%%\n", static_cast<unsigned>(s.volume_percent));
    if (s.sample_rate_hz == 0) {
        std::printf("output:   none (daemon started with --no-audio)\n");
    } else {
        std::printf("output:   %u Hz, ring buffer %.0f%% full, clock drift %+.1f ppm\n",
                    s.sample_rate_hz, static_cast<double>(s.ring_buffer_fill_pct) * 100.0,
                    static_cast<double>(s.clock_drift_ppm));
    }
    return kExitOk;
}

// ---- Talking to the daemon ---------------------------------------------------

FrameHeader make_header(Opcode opcode, std::uint64_t seq_id) {
    FrameHeader header{};
    header.opcode = opcode;
    header.seq_id = seq_id;
    return header;
}

// Sends one whole frame. A brand-new connection has plenty of room, so
// WouldBlock here means something is badly wrong; treat it like an error.
bool send_frame(IpcChannel& channel, std::span<const std::byte> frame) {
    auto sent = channel.send(frame);
    if (!sent) {
        std::fprintf(stderr, "sending to the daemon failed: %s\n", sent.error().message().c_str());
        return false;
    }
    if (*sent != IoStatus::Done) {
        std::fprintf(stderr, "the daemon isn't taking messages\n");
        return false;
    }
    return true;
}

// One connection to the daemon. The channel keeps a reference to the reactor
// it was opened with, so a Connection is made once, on the heap, and never
// moved (see connect()).
struct Connection {
    Reactor reactor;
    std::optional<IpcChannel> channel; // set by connect()
    std::vector<std::byte> buffer = std::vector<std::byte>(kMaxFrameBytes);

    explicit Connection(Reactor r) : reactor(std::move(r)) {}
};

struct Frame {
    FrameHeader header;
    std::span<const std::byte> payload; // points into Connection::buffer
};

// Why next_frame() came back without a frame. It prints its own message for
// Failed; the caller decides what a hang-up or a timeout means.
enum class ReadError : std::uint8_t { HungUp, TimedOut, Failed };

// Waits for the next message, until `deadline` (or forever, for watch).
std::expected<Frame, ReadError> next_frame(Connection& c,
                                           std::optional<Clock::time_point> deadline) {
    for (;;) {
        auto received = c.channel->receive(c.buffer);
        if (!received) {
            std::fprintf(stderr, "reading from the daemon failed: %s\n",
                         received.error().message().c_str());
            return std::unexpected(ReadError::Failed);
        }
        if (received->status == IoStatus::PeerClosed) {
            return std::unexpected(ReadError::HungUp);
        }
        if (received->status == IoStatus::WouldBlock) {
            // Nothing yet. Sleep in the reactor until the socket is readable
            // or the time is up. Without a deadline, wake once a second just
            // to check again; the socket wakes it sooner.
            auto left = std::chrono::milliseconds{1000};
            if (deadline) {
                left = std::chrono::ceil<std::chrono::milliseconds>(*deadline - Clock::now());
                if (left <= 0ms) {
                    return std::unexpected(ReadError::TimedOut);
                }
            }
            if (auto ran = c.reactor.run_once(left); !ran) {
                std::fprintf(stderr, "waiting for the daemon failed: %s\n",
                             ran.error().message().c_str());
                return std::unexpected(ReadError::Failed);
            }
            continue;
        }

        const std::span<const std::byte> frame(c.buffer.data(), received->bytes);
        auto header = validate_frame(frame);
        if (!header) {
            std::fprintf(stderr, "the daemon sent a broken frame (error %d)\n",
                         static_cast<int>(header.error()));
            return std::unexpected(ReadError::Failed);
        }
        return Frame{*header, frame.subspan(kFrameHeaderBytes)};
    }
}

// For commands waiting on an answer, where a hang-up or silence is a failure.
void report(ReadError error) {
    if (error == ReadError::HungUp) {
        std::fprintf(stderr, "the daemon hung up\n");
    } else if (error == ReadError::TimedOut) {
        std::fprintf(stderr, "the daemon didn't answer within %lld s\n",
                     static_cast<long long>(kReplyTimeout.count()));
    }
}

// Reads messages until one with `opcode` arrives, skipping everything else
// (the HelloAck, PlaybackStateEvents...), for up to kReplyTimeout.
std::optional<Frame> wait_for(Connection& c, Opcode opcode) {
    const auto deadline = Clock::now() + kReplyTimeout;
    for (;;) {
        auto frame = next_frame(c, deadline);
        if (!frame) {
            report(frame.error());
            return std::nullopt;
        }
        if (frame->header.opcode == opcode) {
            return *frame;
        }
    }
}

// Waits for the CommandAck or CommandNak answering request `seq_id`.
std::optional<CommandResultBody> wait_for_answer(Connection& c, std::uint64_t seq_id) {
    const auto deadline = Clock::now() + kReplyTimeout;
    for (;;) {
        auto frame = next_frame(c, deadline);
        if (!frame) {
            report(frame.error());
            return std::nullopt;
        }
        if (frame->header.opcode != Opcode::CommandAck &&
            frame->header.opcode != Opcode::CommandNak) {
            continue;
        }
        auto body = decode_body<CommandResultBody>(frame->payload);
        if (!body) {
            std::fprintf(stderr, "the daemon sent an answer amctl can't read\n");
            return std::nullopt;
        }
        if (body->request_seq_id == seq_id) {
            return *body;
        }
    }
}

std::optional<PlaybackStateBody> read_state(const Frame& frame) {
    auto body = decode_body<PlaybackStateBody>(frame.payload);
    if (!body) {
        std::fprintf(stderr, "the daemon sent a state amctl can't read\n");
        return std::nullopt;
    }
    return *body;
}

// Connects and says hello. The daemon answers a hello with a HelloAck and then
// a PlaybackStateEvent with the current state, which toggle and watch use.
std::unique_ptr<Connection> connect(const Arguments& args) {
    auto reactor = Reactor::create();
    if (!reactor) {
        std::fprintf(stderr, "couldn't start the event loop: %s\n",
                     reactor.error().message().c_str());
        return nullptr;
    }
    auto c = std::make_unique<Connection>(std::move(*reactor));
    auto channel = IpcChannel::connect(c->reactor, args.name);
    if (!channel) {
        if (is_no_daemon(channel.error())) {
            std::fprintf(stderr, "no daemon running as %s\n", args.name.c_str());
        } else {
            std::fprintf(stderr, "couldn't connect to %s: %s\n", args.name.c_str(),
                         channel.error().message().c_str());
        }
        return nullptr;
    }
    c->channel.emplace(std::move(*channel));

    // Also so the daemon's log says "amctl connected" instead of "a client
    // (no hello yet)".
    const auto hello = encode_message(make_header(Opcode::HelloReq, 1),
                                      HelloBody{.name = make_peer_name("amctl")});
    if (!send_frame(*c->channel, hello)) {
        return nullptr;
    }
    return c;
}

constexpr std::uint64_t kRequestSeqId = 2; // the hello was 1

// ---- amctl ping --------------------------------------------------------------

int ping(Connection& c) {
    // The hello's answer comes first, so wait for it before starting the clock.
    if (!wait_for(c, Opcode::HelloAck)) {
        return kExitFailed;
    }
    const auto sent_at = Clock::now();
    if (!send_frame(*c.channel, encode_header(make_header(Opcode::Ping, kRequestSeqId)))) {
        return kExitFailed;
    }
    // The daemon's Pong carries the Ping's seq_id, like ipc_ping_pong does.
    for (;;) {
        auto pong = wait_for(c, Opcode::Pong);
        if (!pong) {
            return kExitFailed;
        }
        if (pong->header.seq_id == kRequestSeqId) {
            const auto took =
                std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - sent_at);
            std::printf("pong in %lld us\n", static_cast<long long>(took.count()));
            return kExitOk;
        }
    }
}

// ---- amctl watch -------------------------------------------------------------

void print_state(const PlaybackStateBody& state, bool json) {
    if (json) {
        std::printf("{\"state\":\"%s\",\"position_micros\":%llu,\"volume_percent\":%u}\n",
                    state_name(state.state).data(),
                    static_cast<unsigned long long>(state.position_micros),
                    static_cast<unsigned>(state.volume_percent));
    } else {
        std::printf("%-9s %s  volume %u%%\n", state_name(state.state).data(),
                    minutes_seconds(state.position_micros).c_str(),
                    static_cast<unsigned>(state.volume_percent));
    }
    std::fflush(stdout); // so a script reading the pipe sees each line at once
}

// The first event is the state at the time of the hello; after that, one per
// change. Ends cleanly when the daemon quits.
int watch(Connection& c, bool json) {
    for (;;) {
        auto frame = next_frame(c, std::nullopt);
        if (!frame) {
            if (frame.error() == ReadError::HungUp) {
                if (!json) {
                    std::printf("the daemon quit\n");
                }
                return kExitOk;
            }
            return kExitFailed;
        }
        if (frame->header.opcode != Opcode::PlaybackStateEvent) {
            continue;
        }
        auto state = read_state(*frame);
        if (!state) {
            return kExitFailed;
        }
        print_state(*state, json);
    }
}

// ---- play, pause, stop, toggle, volume ---------------------------------------

int send_request(Connection& c, const Arguments& args) {
    Command command = args.command;
    if (command == Command::Toggle) {
        // Same rule as the play/pause media key: pause if playing, otherwise
        // play. The current state is in the event that follows the HelloAck.
        auto event = wait_for(c, Opcode::PlaybackStateEvent);
        if (!event) {
            return kExitFailed;
        }
        auto state = read_state(*event);
        if (!state) {
            return kExitFailed;
        }
        command = state->state == PlaybackState::Playing ? Command::Pause : Command::Play;
    }

    bool sent = false;
    std::string what; // for the printed answer, e.g. "volume 30: ok"
    switch (command) {
    case Command::Play:
        what = "play";
        sent = send_frame(*c.channel, encode_header(make_header(Opcode::PlayReq, kRequestSeqId)));
        break;
    case Command::Pause:
        what = "pause";
        sent = send_frame(*c.channel, encode_header(make_header(Opcode::PauseReq, kRequestSeqId)));
        break;
    case Command::Stop:
        what = "stop";
        sent = send_frame(*c.channel, encode_header(make_header(Opcode::StopReq, kRequestSeqId)));
        break;
    case Command::Volume:
        what = "volume " + std::to_string(args.volume_percent);
        sent = send_frame(*c.channel,
                          encode_message(make_header(Opcode::SetVolumeReq, kRequestSeqId),
                                         SetVolumeBody{.volume_percent = args.volume_percent}));
        break;
    case Command::Toggle: // turned into Play or Pause above
    case Command::Ping:   // main() sends these elsewhere; listed to keep the switch complete
    case Command::Status:
    case Command::Watch:
        return kExitFailed;
    }
    if (!sent) {
        return kExitFailed;
    }

    const auto answer = wait_for_answer(c, kRequestSeqId);
    if (!answer) {
        return kExitFailed;
    }
    std::printf("%s: %s\n", what.c_str(), status_name(answer->status).data());
    return answer->status == CommandStatus::Ok ? kExitOk : kExitFailed;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string_view> words;
    for (int i = 1; i < argc; ++i) {
        words.emplace_back(argv[i]);
    }
    const auto args = am::amctl::parse_arguments(words);
    if (!args) {
        std::fprintf(stderr, "amctl: %s\n\n%s", args.error().c_str(), am::amctl::kUsage.data());
        return kExitBadArguments;
    }
    if (args->command == Command::Status) {
        return show_status(*args);
    }

    auto connection = connect(*args);
    if (!connection) {
        return kExitFailed;
    }
    switch (args->command) {
    case Command::Ping:
        return ping(*connection);
    case Command::Watch:
        return watch(*connection, args->json);
    default:
        return send_request(*connection, *args);
    }
}
