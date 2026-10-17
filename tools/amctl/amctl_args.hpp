#pragma once

// amctl's command line, turned into an Arguments struct. Kept apart from
// main_amctl.cpp so tests/test_amctl_args.cpp can check it without a daemon.
//
//   amctl [--name NAME] COMMAND   (kUsage below lists the commands)
//
// Also here: which amctl command sends each IPC request (coverage()). The rule
// is that anything am-ui can ask the daemon to do, amctl can too, so tests and
// scripts never need a window. The tests use coverage() to keep that true.

#include "common/app_name.hpp"
#include "common/ipc_messages.hpp"

#include <charconv>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace am::amctl {

enum class Command : std::uint8_t {
    Play,
    Pause,
    Stop,
    Toggle, // play if paused or stopped, pause if playing
    Volume, // needs volume_percent
    Ping,   // checks the daemon is alive and answering
    Status, // reads the telemetry page, no IPC
    Watch,  // prints every state change until the daemon quits
};

struct Arguments {
    Command command = Command::Status;
    std::uint8_t volume_percent = 0;           // only for Command::Volume
    bool json = false;                         // only for Status and Watch
    std::string name{am::common::kDaemonName}; // the daemon's --name
};

inline constexpr std::string_view kUsage =
    "usage: amctl [--name NAME] COMMAND\n"
    "\n"
    "commands:\n"
    "  play             start or resume playback\n"
    "  pause            pause playback\n"
    "  stop             stop playback\n"
    "  toggle           pause if playing, otherwise play\n"
    "  volume PERCENT   set the volume, 0 to 100\n"
    "  ping             check the daemon is answering, and how fast\n"
    "  status [--json]  show the play state, position and volume\n"
    "  watch [--json]   print each change of play state or volume until the\n"
    "                   daemon quits (Ctrl-C stops watching)\n"
    "\n"
    "--name NAME talks to a daemon started with the same --name.\n"
    "--json prints JSON instead of text, one object per line.\n";

// `args` is argv without the program name. On failure the error is a one-line
// message saying what was wrong; print it with kUsage.
[[nodiscard]] inline std::expected<Arguments, std::string>
parse_arguments(std::span<const std::string_view> args) {
    Arguments parsed;
    std::size_t i = 0;

    if (i < args.size() && args[i] == "--name") {
        if (i + 1 >= args.size() || args[i + 1].empty()) {
            return std::unexpected("--name needs a name after it");
        }
        parsed.name = args[i + 1];
        i += 2;
    }

    if (i >= args.size()) {
        return std::unexpected("no command given");
    }
    const std::string_view command = args[i++];
    std::size_t operands = 0; // how many words the command takes after itself

    if (command == "play") {
        parsed.command = Command::Play;
    } else if (command == "pause") {
        parsed.command = Command::Pause;
    } else if (command == "stop") {
        parsed.command = Command::Stop;
    } else if (command == "toggle") {
        parsed.command = Command::Toggle;
    } else if (command == "ping") {
        parsed.command = Command::Ping;
    } else if (command == "status" || command == "watch") {
        parsed.command = command == "status" ? Command::Status : Command::Watch;
        if (i < args.size() && args[i] == "--json") {
            parsed.json = true;
            operands = 1;
        }
    } else if (command == "volume") {
        parsed.command = Command::Volume;
        operands = 1;
        if (i >= args.size()) {
            return std::unexpected("volume needs a number from 0 to 100");
        }
        // from_chars takes plain digits only: no sign, no spaces, no "%".
        const std::string_view text = args[i];
        unsigned value = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size() ||
            value > am::common::kMaxVolumePercent) {
            return std::unexpected("volume needs a number from 0 to 100, not \"" +
                                   std::string(text) + "\"");
        }
        parsed.volume_percent = static_cast<std::uint8_t>(value);
    } else {
        return std::unexpected("unknown command \"" + std::string(command) + "\"");
    }

    if (args.size() - i != operands) {
        return std::unexpected("too many arguments after " + std::string(command));
    }
    return parsed;
}

// ---- Which command sends which request -------------------------------------

struct Coverage {
    enum class Kind : std::uint8_t {
        Command,    // `command` sends it
        Automatic,  // every command sends it (the HelloReq)
        FromDaemon, // only the daemon sends this one; nothing for amctl to do
        NotYet,     // the daemon can't do it yet; arrives with the CEF player
    };
    Kind kind;
    std::string_view command; // the amctl word, for Kind::Command only
};

// One line per opcode. There's no `default:`, so adding an opcode to
// ipc_protocol.hpp without deciding its line here is a compile error with
// GCC and Clang (-Wswitch, and warnings are errors). test_daemon.cpp then checks the lines
// against a running daemon: a NotYet request must still get NotSupported, so
// teaching the daemon a new request fails the test until amctl learns it too.
[[nodiscard]] constexpr Coverage coverage(am::common::Opcode opcode) noexcept {
    using am::common::Opcode;
    using Kind = Coverage::Kind;
    switch (opcode) {
    case Opcode::HelloReq:
        return {Kind::Automatic, {}};
    case Opcode::Ping:
        return {Kind::Command, "ping"};
    case Opcode::PlayReq:
        return {Kind::Command, "play"};
    case Opcode::PauseReq:
        return {Kind::Command, "pause"};
    case Opcode::StopReq:
        return {Kind::Command, "stop"};
    case Opcode::SetVolumeReq:
        return {Kind::Command, "volume"};

    // Login, loading tracks, seeking, the queue: all need the real player.
    case Opcode::AuthEscalationReq:
    case Opcode::LoadTrackReq:
    case Opcode::SeekReq:
    case Opcode::SetAudioModeReq:
    case Opcode::QueueReplaceReq:
    case Opcode::QueueAppendReq:
    case Opcode::QueueRemoveReq:
    case Opcode::QueueShuffleReq:
        return {Kind::NotYet, {}};

    case Opcode::HelloAck:
    case Opcode::Pong:
    case Opcode::AuthStatusEvent:
    case Opcode::CommandAck:
    case Opcode::CommandNak:
    case Opcode::TrackChangedEvent:
    case Opcode::EmeErrorEvent:
    case Opcode::PlaybackStateEvent:
        return {Kind::FromDaemon, {}};
    }
    return {Kind::FromDaemon, {}}; // not a listed opcode at all
}

} // namespace am::amctl
