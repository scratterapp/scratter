#pragma once

// am-ui's connection to the daemon (issue #36, design doc section 3.1).
//
// DaemonLink runs its own thread with its own Reactor and IpcChannel, the same
// pieces amctl uses, but it keeps going instead of exiting after one command:
//
//   auto link = DaemonLink::start({}, [] { /* SDL_PushEvent(wake) */ });
//   link->pause();                        // returns at once, never blocks
//   DaemonState s = link->state();        // once per frame: a copy
//
// The window thread never touches the socket, so it can't freeze waiting on
// the daemon. The two threads share two things, both behind one mutex:
//
//   - commands, window -> connection: play() and friends push one onto a
//     queue and wake the reactor (Reactor::wake() is safe from any thread).
//   - state, connection -> window: a DaemonState. After every change the
//     connection thread calls the wake callback, so a pause from the media
//     keys shows up at once, even while the window sleeps waiting for events.
//
// A mutex is fine here: the "no locks" rule is for the audio callback, and
// neither thread is that. The lock is held only to copy a small struct.
//
// The daemon not being there is normal, not an error. The link reports
// NotRunning and tries again every retry_interval, forever. It also drops the
// connection, and starts over, when:
//
//   - the daemon hangs up (it quit or crashed),
//   - the daemon sends a frame that fails validate_frame(), or a body that
//     doesn't decode (status Failed, error bad_message). Never trust what
//     arrives, the same rule the daemon follows for its clients,
//   - the daemon doesn't answer the hello or a command within reply_timeout
//     (status Failed, error timed_out), which means it's stuck.

#include "client/ui_state.hpp"
#include "ipc_channel.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <system_error>

namespace am::client {

struct DaemonLinkOptions {
    // The endpoint to connect to. Tests start a daemon with --name and pass
    // the same name here.
    std::string name{pal::kDaemonEndpoint};
    std::chrono::milliseconds retry_interval{1000};
    std::chrono::milliseconds reply_timeout{2000};
};

class DaemonLink {
  public:
    // Called on the connection thread after the state changed. Keep it short
    // and don't call back into the link from it; posting an event to the
    // window's own loop is the idea.
    using WakeCallback = std::function<void()>;

    // Starts the connection thread. It connects right away and keeps trying
    // until the daemon is there. Fails only if the reactor can't be created.
    [[nodiscard]] static std::expected<std::unique_ptr<DaemonLink>, std::error_code>
    start(DaemonLinkOptions options, WakeCallback on_change);

    // Stops the thread and closes the connection. The daemon carries on.
    ~DaemonLink();

    DaemonLink(const DaemonLink&) = delete;
    DaemonLink& operator=(const DaemonLink&) = delete;

    // A copy of what the connection thread knows right now.
    [[nodiscard]] DaemonState state() const;

    // Commands. Each returns at once with the id its answer will carry in
    // DaemonState::last_result. Not connected means a NotConnected result
    // straight away, never a command saved up for later: a pause pressed while
    // the daemon was down shouldn't suddenly happen a minute after it's back.
    RequestId play();
    RequestId pause();
    RequestId stop();
    RequestId toggle();
    RequestId set_volume(std::uint8_t percent); // 0 to 100; more gets BadRequest

  private:
    struct Impl;
    explicit DaemonLink(std::unique_ptr<Impl> impl);
    RequestId ask(Command command, std::uint8_t volume_percent);
    std::unique_ptr<Impl> impl_;
};

} // namespace am::client
