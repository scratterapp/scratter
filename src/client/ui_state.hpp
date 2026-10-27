#pragma once

// What am-ui knows about the daemon (issue #36, design doc section 3).
//
// Two kinds of information reach the window, by two roads:
//
//   DaemonState         the connection thread (daemon_link.hpp) keeps it up to
//                       date from IPC messages: connected or not, playing or
//                       paused, the volume, the answer to the last command.
//                       The window copies it with DaemonLink::state().
//
//   TelemetryReader     reads the telemetry page in shared memory once per
//                       frame: position, buffer fill, drift, later the
//                       spectrum. Only the newest value matters there.
//
// The rule from the design doc: if missing it would be a bug, it's a message.
// If only the newest value matters, it goes in the page.
//
// Nothing here draws anything, so tests can use all of it without a window.

#include "common/ipc_messages.hpp"
#include "common/telemetry_shm.hpp"
#include "shm_mapping.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <system_error>

namespace am::client {

// Where the connection to the daemon stands.
enum class LinkStatus : std::uint8_t {
    NotRunning, // nothing is listening: scratterd isn't running (yet). Retrying.
    Connecting, // connected, waiting for the daemon's HelloAck
    Connected,  // said hello; commands go through and the state is live
    Failed,     // something else went wrong; DaemonState::error says what. Retrying.
};

// The commands the window can send. Toggle is play/pause, decided on the
// connection thread from the newest state, like the play/pause media key.
enum class Command : std::uint8_t { Play, Pause, Stop, Toggle, SetVolume };

// How a command ended. The first four are the daemon's own answer
// (CommandStatus); the rest are about the connection.
enum class CommandOutcome : std::uint8_t {
    Ok,             // CommandAck
    BadRequest,     // CommandNak: the daemon couldn't read the request
    NotSupported,   // CommandNak: the daemon can't do that yet
    Failed,         // CommandNak: the daemon tried and it went wrong
    NotConnected,   // never sent, there was no daemon to send it to
    ConnectionLost, // sent, but the connection dropped before the answer came
    TimedOut,       // sent, and no answer within the reply timeout
};

// Each command gets an id when it's asked for. It's also the seq_id the
// request goes out with, so it matches the daemon's answer.
using RequestId = std::uint64_t;

struct CommandResult {
    RequestId id = 0;
    Command command = Command::Play;
    CommandOutcome outcome = CommandOutcome::Ok;

    bool operator==(const CommandResult&) const = default;
};

// A copy of everything the connection thread knows. Plain data, cheap to copy.
struct DaemonState {
    LinkStatus status = LinkStatus::NotRunning;
    std::error_code error; // set while status is Failed, otherwise empty

    // Goes up by one at every HelloAck, so the window can tell "the daemon
    // came back" (a new page to map) from "still the same connection".
    std::uint64_t connection_count = 0;
    std::string daemon_name; // from the HelloAck

    // From the newest PlaybackStateEvent. Kept after the daemon goes away, so
    // the window can show the last state greyed out. has_playback is false
    // until the first event arrives.
    bool has_playback = false;
    common::PlaybackState playback = common::PlaybackState::Stopped;
    std::uint8_t volume_percent = 0;
    std::uint64_t position_micros = 0; // where playback was when the event was sent

    // The newest answer to a command, for "couldn't pause: ..." under the
    // controls. Empty until the first command finishes.
    std::optional<CommandResult> last_result;

    // The connection thread only wakes the window when something changed.
    bool operator==(const DaemonState&) const = default;
};

// Reads the telemetry page, once per frame, from the window's thread.
//
// The page belongs to one run of the daemon. When the daemon quits, its page
// goes away; a restarted daemon makes a new one under the same name. So the
// reader maps the page after each HelloAck (a new connection_count) and drops
// it as soon as the connection is gone. Without that, the window would be
// reading a dead daemon's last numbers forever.
//
// One try_read() per update(), never a retry loop: a daemon that died in the
// middle of a write leaves the page looking busy forever (spec 3.2.2). When a
// read fails, the last good snapshot stays.
class TelemetryReader {
  public:
    // `daemon_name` is the endpoint name (kDaemonEndpoint unless the daemon
    // runs with --name); the page is called <daemon_name>_telemetry_v2.
    explicit TelemetryReader(std::string daemon_name);

    // Call once per frame with this frame's copy of the daemon state. Returns
    // the newest good snapshot, or the last one if this read didn't work.
    const common::TelemetrySnapshot& update(const DaemonState& state);

    // The snapshot update() returned last time.
    [[nodiscard]] const common::TelemetrySnapshot& snapshot() const { return last_; }

    // True while the page is mapped and a read worked within the last second.
    // The window greys the numbers out otherwise.
    [[nodiscard]] bool fresh() const;

    [[nodiscard]] bool mapped() const { return mapping_.size() != 0; }

  private:
    using Clock = std::chrono::steady_clock;

    std::string shm_name_;
    pal::ShmMapping mapping_;
    std::uint64_t mapped_for_ = 0; // the connection_count the mapping belongs to
    std::optional<Clock::time_point> open_failed_at_;
    std::optional<Clock::time_point> last_good_at_;
    common::TelemetrySnapshot last_{};
};

} // namespace am::client
