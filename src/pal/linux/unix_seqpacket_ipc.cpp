// Linux implementation of am::pal::IpcChannel and IpcListener
// (src/pal/interfaces/ipc_channel.hpp).
//
// Both are an AF_UNIX socket of type SOCK_SEQPACKET: like UDP, the kernel keeps
// each send() as one separate message, but like TCP it's a reliable, ordered
// connection with a listen/accept/connect handshake. The socket file lives at
// $XDG_RUNTIME_DIR/<endpoint>.sock with mode 0600.
//
// Only the same user gets in, checked three ways (see "Security" in the
// interface header):
//
//   - listen() and connect() refuse a $XDG_RUNTIME_DIR that isn't a directory
//     we own with no access for anyone else (check_runtime_dir).
//   - accept() asks the kernel which user is on the other end (SO_PEERCRED)
//     and turns away anyone who isn't us.
//   - connect() asks the same about the listener, so a socket another user
//     created can't pose as the daemon.
//
// Every socket is non-blocking. The reactor watches it (level-triggered epoll)
// and this file turns what epoll reports into the channel's callbacks:
//
//   readable or hung up  ->  readable callback (receive() then sees the data,
//                            or PeerClosed)
//   writable             ->  writable callback, but only after a send() hit a
//                            full socket. Otherwise the socket is writable
//                            nearly all the time and we don't ask epoll about it.

#include "ipc_channel.hpp"
#include "reactor.hpp"

#include "common/ipc_protocol.hpp"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

namespace am::pal {

namespace {

std::error_code errno_code() { return {errno, std::system_category()}; }

std::unexpected<std::error_code> fail(std::errc code) {
    return std::unexpected(std::make_error_code(code));
}

// A file descriptor that closes itself, for the setup steps that can fail
// halfway. release() hands it over once setup has succeeded.
class UniqueFd {
  public:
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
    UniqueFd& operator=(UniqueFd&&) = delete;
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    ~UniqueFd() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }

  private:
    int fd_;
};

UniqueFd open_socket() {
    return UniqueFd(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
}

bool is_valid_endpoint(std::string_view endpoint) {
    if (endpoint.empty()) {
        return false;
    }
    for (const char c : endpoint) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (!ok) {
            return false;
        }
    }
    return true;
}

// $XDG_RUNTIME_DIR has to be a directory that we own and that nobody else can
// read, write or enter (logind makes it 0700). Anyone with write access to it
// could delete our socket file and put their own in its place, and the socket
// file's own 0600 wouldn't help. A misconfigured system (say XDG_RUNTIME_DIR
// pointing at /tmp) is refused rather than quietly used.
//
// stat() follows a symlink, so a link to a private directory is fine: what
// matters is the directory the socket ends up in.
std::error_code check_runtime_dir(const char* dir) {
    struct stat info {};
    if (::stat(dir, &info) < 0) {
        return errno_code();
    }
    if (!S_ISDIR(info.st_mode)) {
        return std::make_error_code(std::errc::not_a_directory);
    }
    const bool ours = info.st_uid == ::geteuid();
    const bool private_to_us = (info.st_mode & (S_IRWXG | S_IRWXO)) == 0;
    if (!ours || !private_to_us) {
        return std::make_error_code(std::errc::permission_denied);
    }
    return {};
}

// Which user is on the other end of a connected socket. The kernel wrote it
// down when that process called connect() (for a client) or listen() (for the
// daemon), so the other side can't lie about it.
bool peer_is_same_user(int fd) {
    ucred peer{};
    socklen_t size = sizeof(peer);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) < 0) {
        return false;
    }
    return peer.uid == ::geteuid();
}

// Where the socket file for `endpoint` lives, as a socket address.
struct SocketPath {
    sockaddr_un address;
    std::string path;
};

std::expected<SocketPath, std::error_code> socket_path(std::string_view endpoint) {
    if (!is_valid_endpoint(endpoint)) {
        return fail(std::errc::invalid_argument);
    }

    // $XDG_RUNTIME_DIR is a per-user directory (usually /run/user/<uid>, mode
    // 0700) that systemd-logind creates at login. Without it there's no safe
    // place to put the socket, so refuse rather than fall back to /tmp.
    const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR");
    if (runtime_dir == nullptr || *runtime_dir == '\0') {
        return fail(std::errc::no_such_file_or_directory);
    }
    if (const std::error_code problem = check_runtime_dir(runtime_dir)) {
        return std::unexpected(problem);
    }

    SocketPath result{};
    result.path = std::string(runtime_dir) + "/" + std::string(endpoint) + ".sock";

    // sun_path is a fixed 108-byte array that needs room for the final '\0'.
    if (result.path.size() >= sizeof(result.address.sun_path)) {
        return fail(std::errc::filename_too_long);
    }
    result.address.sun_family = AF_UNIX;
    std::memcpy(result.address.sun_path, result.path.c_str(), result.path.size() + 1);
    return result;
}

int connect_to(int fd, const sockaddr_un& address) {
    return ::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
}

int bind_to(int fd, const sockaddr_un& address) {
    return ::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
}

NativeHandle handle_of(int fd) { return NativeHandle{fd}; }

} // namespace

// --- IpcChannel ------------------------------------------------------------

struct IpcChannel::Impl {
    Reactor* reactor;
    int fd;
    bool watching = false;
    bool peer_closed = false;
    bool waiting_for_writable = false;

    // Callbacks sit behind a shared_ptr so on_event() can keep the one it's
    // calling alive, even if that callback destroys the channel (and this Impl).
    std::shared_ptr<Callback> on_readable;
    std::shared_ptr<Callback> on_writable;

    Impl(Reactor& r, int socket_fd) noexcept : reactor(&r), fd(socket_fd) {}
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        stop_watching();
        ::close(fd);
    }

    // Wraps an already connected socket. Used by connect() and accept().
    static std::expected<IpcChannel, std::error_code> open(Reactor& reactor, UniqueFd fd) {
        auto impl = std::make_unique<Impl>(reactor, fd.release());
        Impl* self = impl.get(); // stays put: the Impl never moves, only the unique_ptr does
        auto watched = reactor.watch(handle_of(self->fd), Interest{.readable = true},
                                     [self](const IoEvent& event) { self->on_event(event); });
        if (!watched) {
            return std::unexpected(watched.error());
        }
        self->watching = true;
        return IpcChannel(std::move(impl));
    }

    // Runs at most one user callback and then returns without touching `this`
    // again, because the callback may have destroyed the channel. epoll is
    // level-triggered, so anything not handled now is reported again next loop.
    void on_event(const IoEvent& event) {
        if (waiting_for_writable && event.writable) {
            waiting_for_writable = false;
            [[maybe_unused]] auto ignored =
                reactor->modify(handle_of(fd), Interest{.readable = true});
            call(on_writable);
            return;
        }
        if (event.hangup && !(on_readable && *on_readable)) {
            // No callback to tell, and epoll would keep reporting the hang-up
            // every loop, so stop watching. Don't mark the peer closed yet:
            // messages it sent just before closing are still queued, and a
            // caller that polls receive() itself (like amctl) must get them
            // first. receive() says PeerClosed once the queue is empty.
            stop_watching();
            return;
        }
        if (event.readable || event.hangup) {
            call(on_readable);
        }
    }

    static void call(const std::shared_ptr<Callback>& stored) {
        const std::shared_ptr<Callback> callback = stored; // our own reference
        if (callback && *callback) {
            (*callback)();
        }
    }

    // Once the peer is gone, epoll would report the hang-up on every loop
    // forever, so stop watching. send() and receive() keep saying PeerClosed.
    IoStatus mark_peer_closed() {
        peer_closed = true;
        stop_watching();
        return IoStatus::PeerClosed;
    }

    void stop_watching() {
        if (watching) {
            watching = false;
            [[maybe_unused]] auto ignored = reactor->unwatch(handle_of(fd));
        }
    }
};

IpcChannel::IpcChannel(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
IpcChannel::~IpcChannel() = default;
IpcChannel::IpcChannel(IpcChannel&&) noexcept = default;
IpcChannel& IpcChannel::operator=(IpcChannel&&) noexcept = default;

std::expected<IpcChannel, std::error_code> IpcChannel::connect(Reactor& reactor,
                                                               std::string_view endpoint) {
    auto where = socket_path(endpoint);
    if (!where) {
        return std::unexpected(where.error());
    }

    UniqueFd fd = open_socket();
    if (fd.get() < 0) {
        return std::unexpected(errno_code());
    }
    // On a Unix socket connect() completes straight away, even non-blocking:
    // the listener's kernel queue either takes us or it doesn't. ENOENT means
    // no socket file, ECONNREFUSED means a file but no daemon behind it.
    if (connect_to(fd.get(), where->address) < 0) {
        return std::unexpected(errno_code());
    }
    // The directory is ours, so only we (or root) could have made the socket.
    // Checking who is listening costs one call and doesn't lean on that.
    if (!peer_is_same_user(fd.get())) {
        return fail(std::errc::permission_denied);
    }
    return Impl::open(reactor, std::move(fd));
}

std::expected<IoStatus, std::error_code> IpcChannel::send(std::span<const std::byte> message) {
    if (message.empty()) {
        return fail(std::errc::invalid_argument);
    }
    if (message.size() > common::kMaxFrameBytes) {
        return fail(std::errc::message_size);
    }
    if (impl_->peer_closed) {
        return IoStatus::PeerClosed;
    }

    // MSG_NOSIGNAL: without it, sending to a closed socket kills the whole
    // process with SIGPIPE instead of returning EPIPE.
    ssize_t sent;
    do {
        sent = ::send(impl_->fd, message.data(), message.size(), MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);

    if (sent >= 0) {
        // SOCK_SEQPACKET sends all of a message or none of it.
        return IoStatus::Done;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // The socket's send buffer is full. Ask epoll to tell us when it isn't.
        if (!impl_->waiting_for_writable) {
            auto modified = impl_->reactor->modify(handle_of(impl_->fd),
                                                   Interest{.readable = true, .writable = true});
            if (!modified) {
                return std::unexpected(modified.error());
            }
            impl_->waiting_for_writable = true;
        }
        return IoStatus::WouldBlock;
    }
    if (errno == EPIPE || errno == ECONNRESET) {
        return impl_->mark_peer_closed();
    }
    return std::unexpected(errno_code());
}

std::expected<ReceiveResult, std::error_code> IpcChannel::receive(std::span<std::byte> buffer) {
    if (impl_->peer_closed) {
        return ReceiveResult{IoStatus::PeerClosed, 0};
    }

    // recvmsg() instead of recv() only to get msg_flags back, which says
    // whether the message was too big for `buffer` (MSG_TRUNC).
    iovec chunk{buffer.data(), buffer.size()};
    msghdr header{};
    header.msg_iov = &chunk;
    header.msg_iovlen = 1;

    ssize_t received;
    do {
        received = ::recvmsg(impl_->fd, &header, 0);
    } while (received < 0 && errno == EINTR);

    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return ReceiveResult{IoStatus::WouldBlock, 0};
        }
        if (errno == ECONNRESET) {
            return ReceiveResult{impl_->mark_peer_closed(), 0};
        }
        return std::unexpected(errno_code());
    }
    if (received == 0) {
        // The peer closed its end. (It can't be an empty message: send()
        // refuses those on both sides.)
        return ReceiveResult{impl_->mark_peer_closed(), 0};
    }
    if ((header.msg_flags & MSG_TRUNC) != 0) {
        // The kernel already dropped the rest of the message; the next
        // receive() gets the next message.
        return fail(std::errc::message_size);
    }
    return ReceiveResult{IoStatus::Done, static_cast<std::size_t>(received)};
}

void IpcChannel::set_readable_callback(Callback callback) {
    impl_->on_readable = std::make_shared<Callback>(std::move(callback));
}

void IpcChannel::set_writable_callback(Callback callback) {
    impl_->on_writable = std::make_shared<Callback>(std::move(callback));
}

// --- IpcListener -----------------------------------------------------------

struct IpcListener::Impl {
    Reactor* reactor;
    int fd;
    std::string path;
    std::shared_ptr<Callback> on_accept;

    Impl(Reactor& r, int socket_fd, std::string socket_file) noexcept
        : reactor(&r), fd(socket_fd), path(std::move(socket_file)) {}
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        [[maybe_unused]] auto ignored = reactor->unwatch(handle_of(fd));
        ::close(fd);
        // The file would otherwise stay behind and look like a crashed daemon.
        ::unlink(path.c_str());
    }

    void on_event(const IoEvent& event) {
        if (event.readable) {
            const std::shared_ptr<Callback> callback = on_accept; // see IpcChannel::Impl::call
            if (callback && *callback) {
                (*callback)();
            }
        }
    }
};

namespace {

// bind() failed with EADDRINUSE: the socket file exists. Either a daemon is
// listening on it, or one crashed and left the file behind. Connecting tells
// the two apart: a stale file refuses the connection.
bool is_stale_socket_file(const sockaddr_un& address) {
    const UniqueFd probe = open_socket();
    if (probe.get() < 0) {
        return false;
    }
    return connect_to(probe.get(), address) < 0 && errno == ECONNREFUSED;
}

} // namespace

IpcListener::IpcListener(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
IpcListener::~IpcListener() = default;
IpcListener::IpcListener(IpcListener&&) noexcept = default;
IpcListener& IpcListener::operator=(IpcListener&&) noexcept = default;

std::expected<IpcListener, std::error_code> IpcListener::listen(Reactor& reactor,
                                                                std::string_view endpoint) {
    auto where = socket_path(endpoint);
    if (!where) {
        return std::unexpected(where.error());
    }

    UniqueFd fd = open_socket();
    if (fd.get() < 0) {
        return std::unexpected(errno_code());
    }

    if (bind_to(fd.get(), where->address) < 0) {
        if (errno != EADDRINUSE) {
            return std::unexpected(errno_code());
        }
        // Known gap: two daemons starting at the same instant can both decide
        // the file is stale, and the second unlink() removes the first one's
        // live socket. A flock() on a "<endpoint>.lock" file next to the socket
        // would close it, if that ever turns out to matter.
        if (!is_stale_socket_file(where->address)) {
            return fail(std::errc::address_in_use); // another daemon is running
        }
        ::unlink(where->path.c_str());
        if (bind_to(fd.get(), where->address) < 0) {
            return std::unexpected(errno_code());
        }
    }

    // Owner read/write only. Nobody can connect between bind() and this,
    // because connecting needs listen(), which comes after.
    if (::chmod(where->path.c_str(), S_IRUSR | S_IWUSR) < 0 || ::listen(fd.get(), SOMAXCONN) < 0) {
        const std::error_code error = errno_code();
        ::unlink(where->path.c_str());
        return std::unexpected(error);
    }

    auto impl = std::make_unique<Impl>(reactor, fd.release(), std::move(where->path));
    Impl* self = impl.get();
    auto watched = reactor.watch(handle_of(self->fd), Interest{.readable = true},
                                 [self](const IoEvent& event) { self->on_event(event); });
    if (!watched) {
        // ~Impl would unwatch a handle that was never watched; that just fails
        // quietly, then it closes the socket and removes the file.
        return std::unexpected(watched.error());
    }
    return IpcListener(std::move(impl));
}

std::expected<std::optional<IpcChannel>, std::error_code> IpcListener::accept() {
    for (;;) {
        UniqueFd client(::accept4(impl_->fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
        if (client.get() >= 0) {
            // The socket file's 0600 should already keep other users out;
            // this holds even if someone loosens it. Returning (rather than
            // trying the next client) lets the daemon log it. `client` closes
            // here, so the other side sees a hang-up.
            if (!peer_is_same_user(client.get())) {
                return fail(std::errc::permission_denied);
            }
            auto channel = IpcChannel::Impl::open(*impl_->reactor, std::move(client));
            if (!channel) {
                return std::unexpected(channel.error());
            }
            return std::optional<IpcChannel>(std::move(*channel));
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return std::nullopt;
        }
        // A client that gave up while still in the queue, or a signal. Neither
        // is a problem with the listener, so try the next one.
        if (errno != ECONNABORTED && errno != EINTR) {
            return std::unexpected(errno_code());
        }
    }
}

void IpcListener::set_accept_callback(Callback callback) {
    impl_->on_accept = std::make_shared<Callback>(std::move(callback));
}

} // namespace am::pal
