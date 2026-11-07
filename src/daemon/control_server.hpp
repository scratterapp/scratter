#pragma once

// The daemon's side of the control IPC: accepts clients (am-ui, amctl, the
// tests), answers their requests, and tells all of them when the play state
// or volume changes.
//
//   Client sends          Daemon answers
//   HelloReq              HelloAck with its name, then a PlaybackStateEvent and
//                         a TrackChangedEvent
//   Ping                  Pong with the Ping's seq_id
//   PlayReq/PauseReq/     CommandAck, then a PlaybackStateEvent to every
//   StopReq/SetVolumeReq  client (this one too) if something changed
//   LoadTrackReq          CommandAck once the switch has started, and a
//                         TrackChangedEvent to every client once the new
//                         source is in place (main's broadcast_track()); a
//                         CommandNak if the daemon can't play that source
//   SeekReq               CommandAck once the seek has started, and a
//                         PlaybackStateEvent with the new position to every
//                         client once it's done; a CommandNak with
//                         NotSupported if what plays has no length (the tone)
//   a bad body            CommandNak with BadRequest
//   any other request     CommandNak with NotSupported
//   a broken frame        nothing; the client is disconnected
//
// When a request changes the play state or volume, the server calls
// `on_change` before it sends the CommandAck. main() uses it to republish the
// telemetry page, so a client that reads the page as soon as its ack arrives
// (`amctl pause` then `amctl status`) already sees the new state.
//
// Everything runs on the reactor thread. Requests are handled in the order
// they arrive, one at a time, so there's nothing to lock.
//
// A client that stops reading its messages is disconnected rather than
// queued for: the socket holds hundreds of these small frames, so a full one
// means the client is stuck.

#include "common/ipc_messages.hpp"
#include "daemon/audio_engine/audio_path.hpp"
#include "ipc_channel.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace am::daemon {

// What the server asks main() about tracks: loading one, and what plays now.
struct TrackHooks {
    // LoadTrackReq. Ok means the switch has started; NotSupported or Failed
    // means nothing changed.
    std::function<common::CommandStatus(common::TrackSource)> load;
    // What plays now, for the TrackChangedEvent after each HelloAck.
    std::function<common::TrackChangedBody()> current;
    // SeekReq. Ok means the seek has started; NotSupported means what plays
    // can't seek.
    std::function<common::CommandStatus(std::chrono::microseconds)> seek;
};

class ControlServer {
  public:
    // Takes over a listener that's already open. main() opens it first,
    // because a listener that fails with address_in_use is how the daemon
    // finds out another copy is already running.
    ControlServer(pal::IpcListener listener, audio::AudioPath& audio,
                  std::function<void()> on_change, TrackHooks tracks);

    // Its callbacks point back at this object, so it can't be copied or moved.
    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    // Sends every client a PlaybackStateEvent with the current state and
    // volume. The server does this itself after its own commands; main()
    // calls it when something else (a media key) changed them.
    void broadcast_state();

    // Sends every client a TrackChangedEvent with what plays now. main()
    // calls it when a switch is done.
    void broadcast_track();

  private:
    struct Client {
        pal::IpcChannel channel;
        std::string name = "a client (no hello yet)";
        bool closed = false; // set on errors; removed once the current callback is done
    };

    void accept_clients();
    void read_from(Client& client);
    void handle(Client& client, const common::FrameHeader& header,
                std::span<const std::byte> payload);

    void reply(Client& client, const common::FrameHeader& request, common::CommandStatus status);
    void send_state(Client& client);
    void send_track(Client& client);
    void send(Client& client, std::span<const std::byte> frame);
    void remove_closed_clients();

    // A header for the next frame this daemon sends.
    common::FrameHeader next_header(common::Opcode opcode);

    pal::IpcListener listener_;
    audio::AudioPath& audio_;
    std::function<void()> on_change_;
    TrackHooks tracks_;
    // unique_ptr so each Client stays at one address: its readable callback
    // holds a pointer to it.
    std::vector<std::unique_ptr<Client>> clients_;
    std::vector<std::byte> receive_buffer_;
    std::uint64_t next_seq_id_ = 1;
};

} // namespace am::daemon
