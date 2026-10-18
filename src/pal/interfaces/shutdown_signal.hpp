#pragma once

// "Please quit" from the OS: Ctrl-C in the terminal, or `kill` / systemd
// stopping the daemon (SIGINT and SIGTERM on Linux; Ctrl-C, Ctrl-Break and
// closing the console on Windows).
//
// The default reaction to those is to kill the process on the spot, which
// would skip the cleanup the daemon wants to do: stop the audio stream, remove
// the socket file and the shared-memory name. ShutdownSignal swaps that for a
// callback on the reactor thread, where the daemon can stop the reactor and
// let everything shut down in order:
//
//   auto shutdown = am::pal::ShutdownSignal::watch(reactor, [&] { reactor.stop(); });
//
// Create it before starting any other thread (before AudioSink::open(), which
// starts the audio thread). On Linux this works by blocking the signals and
// reading them from a file descriptor (signalfd), and a thread only inherits
// "blocked" from the thread that created it. A thread started earlier would
// still get the default reaction, which kills the whole process.
//
// On Windows the handler runs on a thread of its own and reaches the reactor
// thread through Reactor::wake(), so a ShutdownSignal takes the reactor's wake
// callback. Don't set another one while it exists.
//
// Each platform defines this class once: src/pal/linux/signalfd_shutdown.cpp
// and src/pal/win32/console_shutdown.cpp.

#include <expected>
#include <functional>
#include <memory>
#include <system_error>

namespace am::pal {

class Reactor;

class ShutdownSignal {
  public:
    using Callback = std::move_only_function<void()>;

    // Starts catching the quit signals. `on_shutdown` runs on the reactor
    // thread each time one arrives (a second Ctrl-C runs it again). Only one
    // ShutdownSignal should exist at a time.
    [[nodiscard]] static std::expected<ShutdownSignal, std::error_code> watch(Reactor& reactor,
                                                                              Callback on_shutdown);

    // Puts the default reaction back. Destroy it before the reactor, and not
    // from inside its own callback.
    ~ShutdownSignal();
    ShutdownSignal(ShutdownSignal&&) noexcept;
    ShutdownSignal& operator=(ShutdownSignal&&) noexcept;
    ShutdownSignal(const ShutdownSignal&) = delete;
    ShutdownSignal& operator=(const ShutdownSignal&) = delete;

  private:
    struct Impl;
    explicit ShutdownSignal(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace am::pal
