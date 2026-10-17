#pragma once

// Control-plane IPC: the connection between am-ui and scratterd (spec
// section 3.2.1, and the "Control IPC Transport" row of section 4).
//
// The daemon listens, the UI connects:
//
//   // Daemon
//   auto listener = IpcListener::listen(reactor, kDaemonEndpoint);
//   listener->set_accept_callback([&] {
//       for (;;) {
//           auto client = listener->accept();
//           if (!client || !*client) {
//               break; // an error, or nobody else waiting
//           }
//           clients.push_back(std::move(**client));
//       }
//   });
//
//   // UI
//   auto channel = IpcChannel::connect(reactor, kDaemonEndpoint);
//   channel->send(frame_bytes);
//
// Messages, not a byte stream
// ---------------------------
// Each send() arrives at the other end as exactly one receive(), whole, or not
// at all. A message is one IPC frame (src/common/ipc_protocol.hpp), so the
// receiver never has to find where one frame ends and the next begins. Linux
// gets this from AF_UNIX SOCK_SEQPACKET, Windows from a PIPE_TYPE_MESSAGE named
// pipe. The channel doesn't look inside messages; run validate_frame() on each
// one received.
//
// Never blocking
// --------------
// No call here waits. When the OS can't take or hand over a message right
// now, the call returns IoStatus::WouldBlock, which is a normal answer, not an
// error. The channel then calls a callback on the reactor thread when it's
// worth trying again:
//
//   - the readable callback when receive() may have something. Call receive()
//     until it returns WouldBlock; there can be several messages waiting.
//   - the writable callback when a send() that returned WouldBlock can be
//     retried. It only runs after a WouldBlock.
//
// The other end hanging up isn't an error either: send() and receive() return
// IoStatus::PeerClosed, after which the channel is finished and should be
// destroyed. Errors (std::error_code) are for things that really went wrong.
//
// Threading and lifetime
// ----------------------
// Channels and listeners belong to the reactor they were created with. Every
// call must happen on that reactor's thread, which is also where the
// callbacks run. Destroy them before the reactor, and don't move the Reactor
// while any exist: they keep a reference to it. Destroying a channel or
// listener from inside one of its own callbacks is allowed (that's the usual
// response to PeerClosed).
//
// For implementers, that last promise needs care: the destroy frees the Impl
// that holds the callback while the callback is still running. Hold callbacks
// by shared_ptr and call through a local copy (or move the callback out
// first), and touch nothing in Impl after it returns. The epoll reactor does
// the same for unwatch() from inside a watch callback.
//
// Security
// --------
// Only processes running as the same user may connect. Linux creates the
// socket file with mode 0600 in $XDG_RUNTIME_DIR; Windows gives the pipe a
// DACL for the current user and FILE_FLAG_FIRST_PIPE_INSTANCE, so another
// program can't create the pipe first and pretend to be the daemon.
//
// Each platform defines these classes once: src/pal/linux/unix_seqpacket_ipc.cpp
// and src/pal/win32/named_pipe_ipc.cpp. Like the Reactor, there's no virtual
// dispatch and no #ifdef here; CMake compiles one of the two.

#include "common/app_name.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>

namespace am::pal {

class Reactor;

// The endpoint the daemon listens on and the UI connects to: the daemon's name
// from src/common/app_name.hpp, so renaming the project renames the socket too.
inline constexpr std::string_view kDaemonEndpoint = am::common::kDaemonName;

// What a send() or receive() did, when nothing went wrong.
enum class IoStatus : std::uint8_t {
    Done,       // one whole message was sent or received
    WouldBlock, // nothing happened; try again after the matching callback
    PeerClosed, // the other end is gone; the channel is finished
};

struct ReceiveResult {
    IoStatus status;
    std::size_t bytes; // size of the received message; 0 unless status is Done
};

// One end of a connection. Move-only; closing happens in the destructor.
class IpcChannel {
  public:
    using Callback = std::move_only_function<void()>;

    // UI side: connects to the endpoint a daemon is listening on. Doesn't wait.
    // Both platforms report the same errors:
    //
    //   no daemon there                  connection_refused or no_such_file_or_directory
    //   daemon too busy to take us now   resource_unavailable_try_again (Linux: the
    //                                    listen backlog is full; Windows: ERROR_PIPE_BUSY)
    //   bad endpoint name                invalid_argument
    //
    // Linux also fails with no_such_file_or_directory when $XDG_RUNTIME_DIR is
    // unset (listen() too), rather than falling back to a shared directory.
    //
    // `endpoint` is a short name such as kDaemonEndpoint, made of letters, digits,
    // '-', '_' and '.'. Each platform turns it into a real address: Linux uses
    // $XDG_RUNTIME_DIR/<endpoint>.sock, Windows uses \\.\pipe\<endpoint>.
    [[nodiscard]] static std::expected<IpcChannel, std::error_code>
    connect(Reactor& reactor, std::string_view endpoint);

    ~IpcChannel();
    IpcChannel(IpcChannel&&) noexcept;
    IpcChannel& operator=(IpcChannel&&) noexcept;
    IpcChannel(const IpcChannel&) = delete;
    IpcChannel& operator=(const IpcChannel&) = delete;

    // Sends `message` as one unit. Either all of it is handed to the OS (Done)
    // or none of it is (WouldBlock, PeerClosed). Messages longer than
    // am::common::kMaxFrameBytes fail with errc::message_size, and empty ones
    // with errc::invalid_argument: a zero-length message can't be told apart
    // from a hang-up on a Linux socket, and no frame is shorter than 32 bytes.
    [[nodiscard]] std::expected<IoStatus, std::error_code> send(std::span<const std::byte> message);

    // Takes the next message, if there is one, and copies it into `buffer`.
    // Give it room for am::common::kMaxFrameBytes. A message that doesn't fit
    // is thrown away and reported as errc::message_size; the channel is still
    // usable afterwards.
    [[nodiscard]] std::expected<ReceiveResult, std::error_code>
    receive(std::span<std::byte> buffer);

    // See "Never blocking" above. Setting one replaces the previous one.
    void set_readable_callback(Callback callback);
    void set_writable_callback(Callback callback);

  private:
    // Platform state (the socket or pipe handle, overlapped I/O buffers) lives
    // in Impl, defined in each platform's .cpp, which keeps <sys/socket.h> and
    // <windows.h> out of this header.
    friend class IpcListener;
    struct Impl;
    explicit IpcChannel(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

// The daemon's side: waits for UIs to connect. Move-only.
class IpcListener {
  public:
    using Callback = std::move_only_function<void()>;

    // Starts listening on `endpoint` (same naming rules as IpcChannel::connect).
    // Fails with errc::address_in_use if something is already listening there,
    // which is how a second daemon finds out it isn't needed. A leftover socket
    // file from a daemon that crashed is not "in use" and gets replaced.
    [[nodiscard]] static std::expected<IpcListener, std::error_code>
    listen(Reactor& reactor, std::string_view endpoint);

    ~IpcListener();
    IpcListener(IpcListener&&) noexcept;
    IpcListener& operator=(IpcListener&&) noexcept;
    IpcListener(const IpcListener&) = delete;
    IpcListener& operator=(const IpcListener&) = delete;

    // Hands over one waiting connection, or std::nullopt when nobody is
    // waiting (the "would block" case). Call it until it returns nullopt.
    [[nodiscard]] std::expected<std::optional<IpcChannel>, std::error_code> accept();

    // Called on the reactor thread when accept() may have a connection.
    void set_accept_callback(Callback callback);

  private:
    struct Impl;
    explicit IpcListener(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace am::pal
