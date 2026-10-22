// Windows implementation of am::pal::IpcChannel and IpcListener
// (src/pal/interfaces/ipc_channel.hpp).
//
// Both are an overlapped, message-mode named pipe, \\.\pipe\<endpoint>.<SID>.
// PIPE_TYPE_MESSAGE keeps each WriteFile as one message, the way SOCK_SEQPACKET
// does on Linux.
//
// Each pipe handle is a PipeEnd, watched by the IOCP reactor from creation to
// close. The reactor ties a callback to a handle once, when it is watched, so a
// server-side PipeEnd keeps that one callback when it changes role: it starts
// out listening (ConnectNamedPipe pending, owned by the listener) and becomes
// the accepted channel when a client connects.
//
// I/O, per PipeEnd:
//
//   - Reading: one ReadFile is always pending into a kMaxFrameBytes buffer. Its
//     completion fires the readable callback; receive() copies the message out
//     and starts the next ReadFile.
//   - Writing: send() copies the message and starts a WriteFile. One may be in
//     flight; a send() while it is returns WouldBlock, and the writable
//     callback runs when it finishes.
//   - The handle has FILE_SKIP_COMPLETION_PORT_ON_SUCCESS set, so an operation
//     that finishes at once is handled where it was started and queues no
//     packet. Only ERROR_IO_PENDING ones are reported by the reactor.
//
// receive(), send() and accept() don't wait for the reactor to report a pending
// operation that has finished: they check the OVERLAPPED (HasOverlappedIoCompleted)
// and take the result straight away, as the Linux calls do with the kernel's
// queue. That operation's packet still arrives later, so each operation keeps a
// count of results taken early, and that many of its packets are dropped.
// Packets for one OVERLAPPED arrive in the order the operations finished, which
// is what makes counting enough.
//
// Some things happen without any packet (ConnectNamedPipe finding the client
// already there, a ReadFile that finished at once while nobody is in receive()),
// so a PipeEnd can post itself a "kick" through Reactor::post() to get its
// callbacks run from the loop.
//
// Closing cancels whatever is in flight and waits for it to finish before
// freeing the OVERLAPPEDs and buffers (a cancelled pipe operation finishes at
// once). A message still pending in a WriteFile at that point is lost; one that
// WriteFile finished is in the pipe and the other end still reads it.

#include "ipc_channel.hpp"
#include "reactor.hpp"

#include "common/ipc_protocol.hpp"

#include <windows.h>

#include <aclapi.h>
#include <sddl.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <expected>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace am::pal {

namespace {

// Pipe instances kept listening, so clients connecting close together don't
// find the pipe busy before the listener has made the next one.
constexpr std::size_t kBacklog = 4;

// Each direction's buffer, a hint to the pipe driver. A WriteFile that doesn't
// fit stays pending until the reader catches up.
constexpr DWORD kPipeBufferBytes = 128 * 1024;

// The whole name, \\.\pipe\ included, can be up to 256 characters.
constexpr std::size_t kMaxPipeNameChars = 256;

constexpr std::wstring_view kPipePrefix = L"\\\\.\\pipe\\";

std::error_code os_error(DWORD err) noexcept {
    return {static_cast<int>(err), std::system_category()};
}

std::unexpected<std::error_code> fail(std::errc code) {
    return std::unexpected(std::make_error_code(code));
}

std::unexpected<std::error_code> last_error() {
    return std::unexpected(os_error(::GetLastError()));
}

NativeHandle handle_of(HANDLE h) noexcept {
    return NativeHandle{reinterpret_cast<std::intptr_t>(h)};
}

// The other end has closed, or is closing, the pipe.
bool is_hang_up(DWORD err) noexcept {
    return err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED || err == ERROR_NO_DATA;
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

// Closes a HANDLE on scope exit unless released.
class HandleGuard {
  public:
    explicit HandleGuard(HANDLE h) noexcept : h_(h) {}
    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
    ~HandleGuard() {
        if (h_ != nullptr && h_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(h_);
        }
    }
    [[nodiscard]] HANDLE get() const noexcept { return h_; }
    [[nodiscard]] HANDLE release() noexcept { return std::exchange(h_, nullptr); }

  private:
    HANDLE h_;
};

// A TOKEN_USER and the SID it points into, read from a token.
struct UserSid {
    alignas(TOKEN_USER) unsigned char buf[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    [[nodiscard]] PSID sid() noexcept { return reinterpret_cast<TOKEN_USER*>(buf)->User.Sid; }
};

std::expected<void, std::error_code> read_token_user(HANDLE token, UserSid& out) {
    DWORD got = 0;
    if (::GetTokenInformation(token, TokenUser, out.buf, sizeof(out.buf), &got) == 0) {
        return last_error();
    }
    return {};
}

// \\.\pipe\<endpoint>.<SID of the current user>
std::expected<std::wstring, std::error_code> pipe_name(std::string_view endpoint) {
    if (!is_valid_endpoint(endpoint)) {
        return fail(std::errc::invalid_argument);
    }
    UserSid user;
    if (auto read = read_token_user(::GetCurrentProcessToken(), user); !read) {
        return std::unexpected(read.error());
    }
    LPWSTR sid_text = nullptr;
    if (::ConvertSidToStringSidW(user.sid(), &sid_text) == 0) {
        return last_error();
    }
    std::wstring name(kPipePrefix);
    name.append(endpoint.begin(), endpoint.end()); // checked above: plain ASCII
    name += L'.';
    name += sid_text;
    ::LocalFree(sid_text);

    if (name.size() > kMaxPipeNameChars) {
        return fail(std::errc::filename_too_long);
    }
    return name;
}

// Whether `pipe` is owned by `user`. Our listener makes the user the owner
// even when it runs elevated, and nobody can make another user the owner of
// what they create without SeRestorePrivilege, which only administrators have.
bool pipe_owned_by(HANDLE pipe, PSID user) {
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (::GetSecurityInfo(pipe, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr,
                          nullptr, nullptr, &sd) != ERROR_SUCCESS) {
        return false;
    }
    const bool ours = owner != nullptr && ::EqualSid(owner, user) != 0;
    ::LocalFree(sd);
    return ours;
}

// Fails with permission_denied unless the process serving `pipe` runs as
// `user`. When we may not look at that process's token, which is what happens
// when the daemon is elevated and we aren't, the pipe's owner decides instead.
std::expected<void, std::error_code> verify_server_user(HANDLE pipe, PSID user) {
    ULONG pid = 0;
    if (::GetNamedPipeServerProcessId(pipe, &pid) == 0) {
        return last_error();
    }
    const HandleGuard process{::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
    HANDLE raw_token = nullptr;
    if (process.get() == nullptr ||
        ::OpenProcessToken(process.get(), TOKEN_QUERY, &raw_token) == 0) {
        if (pipe_owned_by(pipe, user)) {
            return {};
        }
        return fail(std::errc::permission_denied);
    }
    const HandleGuard token{raw_token};
    UserSid server;
    if (!read_token_user(token.get(), server) || ::EqualSid(server.sid(), user) == 0) {
        return fail(std::errc::permission_denied);
    }
    return {};
}

// A security descriptor that makes the current user the pipe's owner and gives
// only that user access, the same shape as the telemetry page (win32_shm.cpp).
// The explicit owner matters when the daemon runs elevated: objects would
// otherwise belong to BUILTIN\Administrators. Holds pointers into itself, so it
// stays where it was made.
class PipeSecurity {
  public:
    PipeSecurity() = default;
    PipeSecurity(const PipeSecurity&) = delete;
    PipeSecurity& operator=(const PipeSecurity&) = delete;

    std::expected<void, std::error_code> init() {
        if (auto read = read_token_user(::GetCurrentProcessToken(), user_); !read) {
            return read;
        }
        auto* dacl = reinterpret_cast<PACL>(acl_buf_);
        if (::InitializeAcl(dacl, sizeof(acl_buf_), ACL_REVISION) == 0 ||
            ::AddAccessAllowedAce(dacl, ACL_REVISION, FILE_ALL_ACCESS, user_.sid()) == 0 ||
            ::InitializeSecurityDescriptor(&sd_, SECURITY_DESCRIPTOR_REVISION) == 0 ||
            ::SetSecurityDescriptorOwner(&sd_, user_.sid(), FALSE) == 0 ||
            ::SetSecurityDescriptorDacl(&sd_, TRUE, dacl, FALSE) == 0) {
            return last_error();
        }
        // No inheritable ACE from elsewhere can widen a protected DACL.
        ::SetSecurityDescriptorControl(&sd_, SE_DACL_PROTECTED, SE_DACL_PROTECTED);
        attributes_.nLength = sizeof(attributes_);
        attributes_.lpSecurityDescriptor = &sd_;
        attributes_.bInheritHandle = FALSE;
        return {};
    }

    [[nodiscard]] SECURITY_ATTRIBUTES* attributes() noexcept { return &attributes_; }

  private:
    UserSid user_{};
    alignas(DWORD) unsigned char acl_buf_[sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) +
                                          SECURITY_MAX_SID_SIZE]{};
    SECURITY_DESCRIPTOR sd_{};
    SECURITY_ATTRIBUTES attributes_{};
};

// One server-side instance of the pipe. Only the first may pass `first`, so
// that a pipe someone else already created under our name is refused.
std::expected<HANDLE, std::error_code> create_pipe_instance(const std::wstring& name, bool first) {
    PipeSecurity security;
    if (auto ready = security.init(); !ready) {
        return std::unexpected(ready.error());
    }
    const DWORD open_mode =
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
    const DWORD pipe_mode =
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS;
    const HANDLE pipe =
        ::CreateNamedPipeW(name.c_str(), open_mode, pipe_mode, PIPE_UNLIMITED_INSTANCES,
                           kPipeBufferBytes, kPipeBufferBytes, 0, security.attributes());
    if (pipe == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        // FILE_FLAG_FIRST_PIPE_INSTANCE fails this way when the name is taken,
        // by another daemon or by anyone else.
        if (first && err == ERROR_ACCESS_DENIED) {
            return fail(std::errc::address_in_use);
        }
        return std::unexpected(os_error(err));
    }
    return pipe;
}

class PipeServer;

// One pipe handle and everything in flight on it. See the top of the file.
class PipeEnd {
  public:
    using Callback = IpcChannel::Callback;

    // Takes ownership of `pipe`, also when it fails.
    static std::expected<std::unique_ptr<PipeEnd>, std::error_code> open(Reactor& reactor,
                                                                         HANDLE pipe);

    PipeEnd(Reactor& reactor, HANDLE pipe) : reactor_(&reactor), pipe_(pipe) {}
    PipeEnd(const PipeEnd&) = delete;
    PipeEnd& operator=(const PipeEnd&) = delete;
    ~PipeEnd();

    // --- Channel ---------------------------------------------------------

    // Starts the first ReadFile. Called once the pipe is connected.
    void start_channel() {
        server_ = nullptr;
        listening_ = false;
        start_read();
    }

    std::expected<IoStatus, std::error_code> send(std::span<const std::byte> message);
    std::expected<ReceiveResult, std::error_code> receive(std::span<std::byte> buffer);

    void set_readable_callback(Callback callback) {
        on_readable_ = std::make_shared<Callback>(std::move(callback));
        // A message may have arrived before there was anyone to tell.
        if (ready_ != Ready::Nothing || read_finished_early()) {
            post_kick();
        }
    }

    void set_writable_callback(Callback callback) {
        on_writable_ = std::make_shared<Callback>(std::move(callback));
    }

    // --- Listening (server side) -------------------------------------------

    // Waits for a client with ConnectNamedPipe. `server` hears about it.
    std::expected<void, std::error_code> listen(PipeServer& server) {
        server_ = &server;
        listening_ = true;
        return start_connect();
    }

    // Whether a client has connected. A client that connected and left again
    // is forgotten and the end listens again; that only fails if listening
    // again does.
    std::expected<bool, std::error_code> poll_connected();

  private:
    // One kind of overlapped operation.
    struct Op {
        OVERLAPPED ov{};
        bool pending = false;  // started, and its result not taken yet
        int early_results = 0; // results taken before their packet arrived
    };

    // What receive() hands out next.
    enum class Ready : std::uint8_t { Nothing, Message, Oversized, Closed, Failed };

    struct Result {
        DWORD error;
        DWORD bytes;
    };

    void on_event(const IoEvent& event);
    void notify_server();

    std::expected<void, std::error_code> start_connect();

    std::optional<Result> issue_read();
    void start_read() {
        if (const auto result = issue_read()) {
            read_finished(result->error, result->bytes);
        }
    }
    void read_finished(DWORD error, DWORD bytes);
    void take_early_read();
    [[nodiscard]] bool read_finished_early() const noexcept {
        return read_.pending && HasOverlappedIoCompleted(&read_.ov);
    }

    void take_early_write();
    void write_finished(DWORD error) noexcept {
        // The message may or may not have arrived; either way nothing more can
        // be sent. The only error that isn't a hang-up is our own cancel.
        if (error != 0) {
            write_closed_ = true;
        }
    }

    // Takes the result of `op`, which has finished, outside of its packet.
    Result take_early(Op& op) {
        op.pending = false;
        ++op.early_results;
        DWORD bytes = 0;
        const DWORD error = ::GetOverlappedResult(pipe_, &op.ov, &bytes, FALSE) != 0
                                ? ERROR_SUCCESS
                                : ::GetLastError();
        return Result{error, bytes};
    }

    void post_kick() {
        if (kick_posted_ || !watching_) {
            return;
        }
        IoEvent event;
        event.overlapped = &kick_;
        if (reactor_->post(handle_of(pipe_), event)) {
            kick_posted_ = true;
        }
    }

    static void call(const std::shared_ptr<Callback>& stored) {
        const std::shared_ptr<Callback> callback = stored; // our own reference
        if (callback && *callback) {
            (*callback)();
        }
    }

    Reactor* reactor_;
    HANDLE pipe_;
    bool watching_ = false;

    Op connect_;
    Op read_;
    Op write_;
    // Only its address is used, to tell a kick from a real completion.
    OVERLAPPED kick_{};
    bool kick_posted_ = false;

    // Server side, until a client connects.
    PipeServer* server_ = nullptr;
    bool listening_ = false;
    bool connect_done_ = false;
    DWORD connect_error_ = ERROR_SUCCESS;

    std::vector<std::byte> read_buf_ = std::vector<std::byte>(common::kMaxFrameBytes);
    std::vector<std::byte> write_buf_ = std::vector<std::byte>(common::kMaxFrameBytes);
    Ready ready_ = Ready::Nothing;
    DWORD ready_bytes_ = 0;
    DWORD failure_ = ERROR_SUCCESS;
    bool discarding_ = false; // reading the rest of a message too big for read_buf_
    bool write_closed_ = false;
    bool want_writable_ = false;

    // Behind a shared_ptr so on_event() can keep the callback it is running
    // alive, even if that callback destroys the channel and this PipeEnd.
    std::shared_ptr<Callback> on_readable_;
    std::shared_ptr<Callback> on_writable_;
};

// The listener's state: kBacklog instances waiting for clients, and the
// connected ones waiting for accept().
class PipeServer {
  public:
    using Callback = IpcListener::Callback;

    PipeServer(Reactor& reactor, std::wstring name) : reactor_(&reactor), name_(std::move(name)) {}

    std::expected<void, std::error_code> add_instance(bool first) {
        auto pipe = create_pipe_instance(name_, first);
        if (!pipe) {
            return std::unexpected(pipe.error());
        }
        auto end = PipeEnd::open(*reactor_, *pipe);
        if (!end) {
            return std::unexpected(end.error());
        }
        if (auto listening = (*end)->listen(*this); !listening) {
            return listening;
        }
        listening_.push_back(std::move(*end));
        return {};
    }

    // Moves the ends that have a client to ready_ and makes new ones to keep
    // kBacklog listening. Returns the last error, if any.
    std::error_code collect() {
        std::error_code error;
        for (std::size_t i = 0; i < listening_.size();) {
            const auto connected = listening_[i]->poll_connected();
            if (connected && !*connected) {
                ++i;
                continue;
            }
            if (connected) {
                listening_[i]->start_channel();
                ready_.push_back(std::move(listening_[i]));
            } else {
                error = connected.error();
            }
            listening_.erase(listening_.begin() + static_cast<std::ptrdiff_t>(i));
        }
        while (listening_.size() < kBacklog) {
            if (auto added = add_instance(false); !added) {
                error = added.error();
                break;
            }
        }
        return error;
    }

    std::expected<std::unique_ptr<PipeEnd>, std::error_code> accept() {
        const std::error_code error = collect();
        if (ready_.empty()) {
            // Report an error only when it has left nothing listening.
            if (error && listening_.empty()) {
                return std::unexpected(error);
            }
            return std::unique_ptr<PipeEnd>{};
        }
        std::unique_ptr<PipeEnd> end = std::move(ready_.front());
        ready_.pop_front();
        return end;
    }

    // A listening end has news. May destroy any end, including the caller,
    // and the user's callback may destroy this server: touch nothing after.
    void on_instance_event() {
        [[maybe_unused]] const std::error_code error = collect();
        if (!ready_.empty()) {
            const std::shared_ptr<Callback> callback = on_accept_; // our own reference
            if (callback && *callback) {
                (*callback)();
            }
        }
    }

    void set_accept_callback(Callback callback) {
        on_accept_ = std::make_shared<Callback>(std::move(callback));
    }

  private:
    Reactor* reactor_;
    std::wstring name_;
    std::vector<std::unique_ptr<PipeEnd>> listening_;
    std::deque<std::unique_ptr<PipeEnd>> ready_;
    std::shared_ptr<Callback> on_accept_;
};

std::expected<std::unique_ptr<PipeEnd>, std::error_code> PipeEnd::open(Reactor& reactor,
                                                                       HANDLE pipe) {
    auto end = std::make_unique<PipeEnd>(reactor, pipe);
    PipeEnd* self = end.get(); // stays put: the PipeEnd never moves, only the unique_ptr does

    // Manual-reset events, so the destructor can wait for a cancelled
    // operation to finish. Completions still go to the port as well.
    for (Op* op : {&self->connect_, &self->read_, &self->write_}) {
        op->ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (op->ov.hEvent == nullptr) {
            return last_error();
        }
    }
    if (::SetFileCompletionNotificationModes(pipe, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS) == 0) {
        return last_error();
    }
    auto watched = reactor.watch(handle_of(pipe), Interest{},
                                 [self](const IoEvent& event) { self->on_event(event); });
    if (!watched) {
        return std::unexpected(watched.error());
    }
    self->watching_ = true;
    return end;
}

PipeEnd::~PipeEnd() {
    if (watching_) {
        // Packets still on their way carry a key nobody owns now and are
        // dropped without touching our OVERLAPPEDs.
        [[maybe_unused]] auto ignored = reactor_->unwatch(handle_of(pipe_));
    }
    if (connect_.pending || read_.pending || write_.pending) {
        ::CancelIoEx(pipe_, nullptr);
        for (Op* op : {&connect_, &read_, &write_}) {
            if (op->pending) {
                ::WaitForSingleObject(op->ov.hEvent, INFINITE);
            }
        }
    }
    ::CloseHandle(pipe_);
    for (Op* op : {&connect_, &read_, &write_}) {
        if (op->ov.hEvent != nullptr) {
            ::CloseHandle(op->ov.hEvent);
        }
    }
}

// Runs at most one user callback, as its last step, because the callback may
// destroy this PipeEnd.
void PipeEnd::on_event(const IoEvent& event) {
    if (event.overlapped == &kick_) {
        kick_posted_ = false;
        if (listening_) {
            notify_server();
            return;
        }
        take_early_read();
        if (ready_ != Ready::Nothing) {
            call(on_readable_);
        }
        return;
    }

    Op* op = nullptr;
    for (Op* candidate : {&connect_, &read_, &write_}) {
        if (event.overlapped == &candidate->ov) {
            op = candidate;
        }
    }
    if (op == nullptr) {
        return;
    }
    if (op->early_results > 0) {
        --op->early_results; // its result was taken already
        return;
    }
    op->pending = false;

    if (op == &connect_) {
        connect_done_ = true;
        connect_error_ = event.error;
        notify_server();
    } else if (op == &read_) {
        read_finished(event.error, event.bytes);
        if (ready_ != Ready::Nothing) {
            call(on_readable_);
        }
    } else {
        write_finished(event.error);
        if (want_writable_) {
            want_writable_ = false;
            call(on_writable_);
        }
    }
}

void PipeEnd::notify_server() {
    if (listening_ && server_ != nullptr) {
        server_->on_instance_event();
    }
}

std::expected<void, std::error_code> PipeEnd::start_connect() {
    connect_done_ = false;
    connect_error_ = ERROR_SUCCESS;
    for (;;) {
        if (::ConnectNamedPipe(pipe_, &connect_.ov) != 0) {
            connect_done_ = true;
            post_kick();
            return {};
        }
        const DWORD err = ::GetLastError();
        if (err == ERROR_IO_PENDING) {
            connect_.pending = true;
            return {};
        }
        if (err == ERROR_PIPE_CONNECTED) {
            // The client got here between CreateNamedPipe and now. Nothing is
            // queued for that, so post the news to ourselves.
            connect_done_ = true;
            post_kick();
            return {};
        }
        if (err == ERROR_NO_DATA) {
            // A client connected and closed again before we got to it.
            ::DisconnectNamedPipe(pipe_);
            continue;
        }
        return std::unexpected(os_error(err));
    }
}

std::expected<bool, std::error_code> PipeEnd::poll_connected() {
    for (;;) {
        if (connect_.pending && HasOverlappedIoCompleted(&connect_.ov)) {
            connect_error_ = take_early(connect_).error;
            connect_done_ = true;
        }
        if (!connect_done_) {
            return false;
        }
        if (connect_error_ == ERROR_SUCCESS || connect_error_ == ERROR_PIPE_CONNECTED) {
            return true;
        }
        // The client left before the connection was taken. Listen again.
        ::DisconnectNamedPipe(pipe_);
        if (auto restarted = start_connect(); !restarted) {
            return std::unexpected(restarted.error());
        }
    }
}

// Starts a ReadFile into read_buf_. Returns its result if it finished at once,
// or nothing if it is pending and a packet will report it.
std::optional<PipeEnd::Result> PipeEnd::issue_read() {
    if (::ReadFile(pipe_, read_buf_.data(), static_cast<DWORD>(read_buf_.size()), nullptr,
                   &read_.ov) != 0) {
        DWORD bytes = 0;
        ::GetOverlappedResult(pipe_, &read_.ov, &bytes, FALSE);
        return Result{ERROR_SUCCESS, bytes};
    }
    const DWORD err = ::GetLastError();
    if (err == ERROR_IO_PENDING) {
        read_.pending = true;
        return std::nullopt;
    }
    if (err == ERROR_MORE_DATA) {
        // Part of a message too big for the buffer. This is a warning status,
        // not a success, so FILE_SKIP_COMPLETION_PORT_ON_SUCCESS doesn't apply
        // and a packet is queued for it as well: drop that one. (In testing
        // the pipe answered such reads with ERROR_IO_PENDING instead, even
        // with the message already waiting, so the tests don't reach this.)
        ++read_.early_results;
        DWORD bytes = 0;
        ::GetOverlappedResult(pipe_, &read_.ov, &bytes, FALSE);
        return Result{err, bytes};
    }
    return Result{err, 0};
}

void PipeEnd::read_finished(DWORD error, DWORD bytes) {
    // A message longer than kMaxFrameBytes: read and throw away the rest, then
    // report it as one oversized message.
    while (error == ERROR_MORE_DATA) {
        discarding_ = true;
        const auto next = issue_read();
        if (!next) {
            return; // pending; its packet carries on from here
        }
        error = next->error;
        bytes = next->bytes;
    }
    if (error == ERROR_SUCCESS) {
        ready_ = discarding_ ? Ready::Oversized : Ready::Message;
        ready_bytes_ = bytes;
        discarding_ = false;
    } else if (is_hang_up(error)) {
        ready_ = Ready::Closed;
    } else {
        ready_ = Ready::Failed;
        failure_ = error;
    }
}

void PipeEnd::take_early_read() {
    while (ready_ == Ready::Nothing && read_finished_early()) {
        const Result result = take_early(read_);
        read_finished(result.error, result.bytes);
    }
}

void PipeEnd::take_early_write() {
    if (write_.pending && HasOverlappedIoCompleted(&write_.ov)) {
        write_finished(take_early(write_).error);
    }
}

std::expected<IoStatus, std::error_code> PipeEnd::send(std::span<const std::byte> message) {
    if (message.empty()) {
        return fail(std::errc::invalid_argument);
    }
    if (message.size() > common::kMaxFrameBytes) {
        return fail(std::errc::message_size);
    }
    take_early_write();
    if (write_closed_ || ready_ == Ready::Closed) {
        return IoStatus::PeerClosed;
    }
    if (write_.pending) {
        want_writable_ = true;
        return IoStatus::WouldBlock;
    }

    // The buffer must outlive a WriteFile that goes pending.
    std::memcpy(write_buf_.data(), message.data(), message.size());
    want_writable_ = false;
    if (::WriteFile(pipe_, write_buf_.data(), static_cast<DWORD>(message.size()), nullptr,
                    &write_.ov) != 0) {
        return IoStatus::Done; // a message-mode pipe takes the whole message
    }
    const DWORD err = ::GetLastError();
    if (err == ERROR_IO_PENDING) {
        // The pipe's buffer is full, but the message is the OS's now.
        write_.pending = true;
        return IoStatus::Done;
    }
    if (is_hang_up(err)) {
        write_closed_ = true;
        return IoStatus::PeerClosed;
    }
    return std::unexpected(os_error(err));
}

std::expected<ReceiveResult, std::error_code> PipeEnd::receive(std::span<std::byte> buffer) {
    take_early_read();
    switch (ready_) {
    case Ready::Nothing:
        return ReceiveResult{IoStatus::WouldBlock, 0};
    case Ready::Closed:
        // Reads come out in order, so everything sent before the hang-up has
        // been received by now.
        return ReceiveResult{IoStatus::PeerClosed, 0};
    case Ready::Failed:
        return std::unexpected(os_error(failure_));
    case Ready::Oversized:
        ready_ = Ready::Nothing;
        start_read();
        return fail(std::errc::message_size);
    case Ready::Message:
        break;
    }

    const std::size_t bytes = ready_bytes_;
    ready_ = Ready::Nothing;
    if (bytes > buffer.size()) {
        start_read();
        return fail(std::errc::message_size);
    }
    std::memcpy(buffer.data(), read_buf_.data(), bytes);
    start_read();
    return ReceiveResult{IoStatus::Done, bytes};
}

} // namespace

// --- IpcChannel ------------------------------------------------------------

struct IpcChannel::Impl {
    std::unique_ptr<PipeEnd> end;
};

IpcChannel::IpcChannel(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
IpcChannel::~IpcChannel() = default;
IpcChannel::IpcChannel(IpcChannel&&) noexcept = default;
IpcChannel& IpcChannel::operator=(IpcChannel&&) noexcept = default;

std::expected<IpcChannel, std::error_code> IpcChannel::connect(Reactor& reactor,
                                                               std::string_view endpoint) {
    auto name = pipe_name(endpoint);
    if (!name) {
        return std::unexpected(name.error());
    }

    // SECURITY_IDENTIFICATION: the daemon may learn who we are, but can't act
    // as us.
    HandleGuard pipe{::CreateFileW(
        name->c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr)};
    if (pipe.get() == INVALID_HANDLE_VALUE) {
        // Not waiting (WaitNamedPipe) when every instance is taken: connect()
        // never blocks, the caller retries.
        switch (const DWORD err = ::GetLastError()) {
        case ERROR_FILE_NOT_FOUND:
            return fail(std::errc::no_such_file_or_directory);
        case ERROR_PIPE_BUSY:
            return fail(std::errc::resource_unavailable_try_again);
        case ERROR_ACCESS_DENIED:
            return fail(std::errc::permission_denied);
        default:
            return std::unexpected(os_error(err));
        }
    }

    DWORD mode = PIPE_READMODE_MESSAGE;
    if (::SetNamedPipeHandleState(pipe.get(), &mode, nullptr, nullptr) == 0) {
        return last_error();
    }

    UserSid user;
    if (auto read = read_token_user(::GetCurrentProcessToken(), user); !read) {
        return std::unexpected(read.error());
    }
    if (auto ours = verify_server_user(pipe.get(), user.sid()); !ours) {
        return std::unexpected(ours.error());
    }

    auto end = PipeEnd::open(reactor, pipe.release());
    if (!end) {
        return std::unexpected(end.error());
    }
    (*end)->start_channel();
    auto impl = std::make_unique<Impl>();
    impl->end = std::move(*end);
    return IpcChannel(std::move(impl));
}

std::expected<IoStatus, std::error_code> IpcChannel::send(std::span<const std::byte> message) {
    return impl_->end->send(message);
}

std::expected<ReceiveResult, std::error_code> IpcChannel::receive(std::span<std::byte> buffer) {
    return impl_->end->receive(buffer);
}

void IpcChannel::set_readable_callback(Callback callback) {
    impl_->end->set_readable_callback(std::move(callback));
}

void IpcChannel::set_writable_callback(Callback callback) {
    impl_->end->set_writable_callback(std::move(callback));
}

// --- IpcListener -----------------------------------------------------------

struct IpcListener::Impl {
    std::unique_ptr<PipeServer> server;
};

IpcListener::IpcListener(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
IpcListener::~IpcListener() = default;
IpcListener::IpcListener(IpcListener&&) noexcept = default;
IpcListener& IpcListener::operator=(IpcListener&&) noexcept = default;

std::expected<IpcListener, std::error_code> IpcListener::listen(Reactor& reactor,
                                                                std::string_view endpoint) {
    auto name = pipe_name(endpoint);
    if (!name) {
        return std::unexpected(name.error());
    }
    auto server = std::make_unique<PipeServer>(reactor, std::move(*name));
    if (auto first = server->add_instance(true); !first) {
        return std::unexpected(first.error());
    }
    if (const std::error_code error = server->collect()) {
        return std::unexpected(error);
    }
    auto impl = std::make_unique<Impl>();
    impl->server = std::move(server);
    return IpcListener(std::move(impl));
}

std::expected<std::optional<IpcChannel>, std::error_code> IpcListener::accept() {
    auto end = impl_->server->accept();
    if (!end) {
        return std::unexpected(end.error());
    }
    if (!*end) {
        return std::nullopt;
    }
    auto channel = std::make_unique<IpcChannel::Impl>();
    channel->end = std::move(*end);
    return std::optional<IpcChannel>(IpcChannel(std::move(channel)));
}

void IpcListener::set_accept_callback(Callback callback) {
    impl_->server->set_accept_callback(std::move(callback));
}

} // namespace am::pal
