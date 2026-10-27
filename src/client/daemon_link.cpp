#include "client/daemon_link.hpp"

#include "common/ipc_messages.hpp"
#include "reactor.hpp"

#include <algorithm>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace am::client {

namespace {

using namespace am::common;
using Clock = std::chrono::steady_clock;

bool is_no_daemon(std::error_code error) {
    return error == std::errc::connection_refused || error == std::errc::no_such_file_or_directory;
}

CommandOutcome outcome_of(CommandStatus status) {
    switch (status) {
    case CommandStatus::Ok:
        return CommandOutcome::Ok;
    case CommandStatus::BadRequest:
        return CommandOutcome::BadRequest;
    case CommandStatus::NotSupported:
        return CommandOutcome::NotSupported;
    case CommandStatus::Failed:
        return CommandOutcome::Failed;
    }
    return CommandOutcome::Failed; // is_valid() already refused anything else
}

FrameHeader make_header(Opcode opcode, std::uint64_t seq_id) {
    FrameHeader header{};
    header.opcode = opcode;
    header.seq_id = seq_id;
    return header;
}

// A command the window asked for, waiting in the queue.
struct Request {
    RequestId id;
    Command command;
    std::uint8_t volume_percent; // SetVolume only
};

// A command that went out, waiting for its CommandAck or CommandNak.
struct Pending {
    RequestId id;
    Command command;
    Clock::time_point deadline;
};

} // namespace

struct DaemonLink::Impl {
    Impl(DaemonLinkOptions o, WakeCallback w, pal::Reactor r)
        : options(std::move(o)), on_change(std::move(w)), reactor(std::move(r)) {}

    const DaemonLinkOptions options;
    const WakeCallback on_change;
    pal::Reactor reactor; // used only on `thread`, apart from wake() and stop()
    std::thread thread;

    // ---- Shared by both threads, under `mutex` ----
    mutable std::mutex mutex;
    DaemonState state;
    std::vector<Request> queue;
    std::uint64_t next_seq_id = 1;

    // ---- The connection thread's own ----
    std::optional<pal::IpcChannel> channel;
    std::vector<std::byte> buffer = std::vector<std::byte>(kMaxFrameBytes);
    std::optional<Clock::time_point> hello_deadline; // set until the HelloAck arrives
    std::vector<Pending> pending;
    Clock::time_point next_connect_at{};

    // The connection thread's whole life.
    void run() {
        reactor.set_wake_callback([this] { send_queued(); });
        try_connect();
        if (auto ran = reactor.run(); !ran) {
            set_status(LinkStatus::Failed, ran.error());
        }
        channel.reset(); // closed on the thread that owns it
    }

    // Changes the shared state with `change`, then tells the window if
    // anything is different. The wake callback runs outside the lock, so the
    // window can call state() from it without deadlocking.
    template <typename Change> void publish(Change&& change) {
        bool changed = false;
        {
            std::lock_guard lock(mutex);
            const DaemonState before = state;
            change(state);
            changed = !(before == state);
        }
        if (changed && on_change) {
            on_change();
        }
    }

    void set_status(LinkStatus status, std::error_code error = {}) {
        publish([&](DaemonState& s) {
            s.status = status;
            s.error = error;
        });
    }

    void finish(RequestId id, Command command, CommandOutcome outcome) {
        publish([&](DaemonState& s) { s.last_result = CommandResult{id, command, outcome}; });
    }

    std::uint64_t take_seq_id() {
        std::lock_guard lock(mutex);
        return next_seq_id++;
    }

    // ---- Connecting and hanging up ----

    void try_connect() {
        auto connected = pal::IpcChannel::connect(reactor, options.name);
        if (!connected) {
            if (is_no_daemon(connected.error())) {
                set_status(LinkStatus::NotRunning);
            } else {
                // permission_denied, for one: the socket checks from PR #59
                // turned the daemon down. The error says why.
                set_status(LinkStatus::Failed, connected.error());
            }
            next_connect_at = Clock::now() + options.retry_interval;
            arm_timer();
            return;
        }
        channel.emplace(std::move(*connected));
        channel->set_readable_callback([this] { read_messages(); });

        const auto hello = encode_message(make_header(Opcode::HelloReq, take_seq_id()),
                                          HelloBody{.name = make_peer_name("am-ui")});
        if (!send_frame(hello)) {
            return; // send_frame() already hung up
        }
        hello_deadline = Clock::now() + options.reply_timeout;
        set_status(LinkStatus::Connecting);
        arm_timer();
    }

    // Closes the connection and schedules the next try. `status` is
    // NotRunning when the daemon hung up, Failed when we gave up on it.
    void hang_up(LinkStatus status, std::error_code error = {}) {
        channel.reset(); // allowed from inside the channel's own callback
        hello_deadline.reset();
        const CommandOutcome lost = error == std::errc::timed_out ? CommandOutcome::TimedOut
                                                                  : CommandOutcome::ConnectionLost;
        for (const Pending& p : pending) {
            finish(p.id, p.command, lost);
        }
        pending.clear();
        set_status(status, error);
        next_connect_at = Clock::now() + options.retry_interval;
        arm_timer();
    }

    // Sends one frame. A full socket means the daemon has stopped reading
    // (it reads everything as it arrives), so that counts as stuck.
    bool send_frame(std::span<const std::byte> frame) {
        auto sent = channel->send(frame);
        if (!sent) {
            hang_up(LinkStatus::Failed, sent.error());
            return false;
        }
        if (*sent == pal::IoStatus::PeerClosed) {
            hang_up(LinkStatus::NotRunning);
            return false;
        }
        if (*sent == pal::IoStatus::WouldBlock) {
            hang_up(LinkStatus::Failed, std::make_error_code(std::errc::timed_out));
            return false;
        }
        return true;
    }

    // ---- The one reactor timer ----
    //
    // The reactor has one timer, and the link needs three kinds of deadline:
    // the next connection attempt, the hello's answer, and each command's
    // answer. So the timer is always set for the earliest of whichever apply.

    void arm_timer() {
        std::optional<Clock::time_point> next;
        if (!channel) {
            next = next_connect_at;
        } else {
            next = hello_deadline;
            for (const Pending& p : pending) {
                if (!next || p.deadline < *next) {
                    next = p.deadline;
                }
            }
        }
        if (!next) {
            reactor.cancel_timer();
            return;
        }
        const auto delay =
            std::max(std::chrono::milliseconds{0},
                     std::chrono::ceil<std::chrono::milliseconds>(*next - Clock::now()));
        if (auto set = reactor.set_timer(delay, [this] { on_timer(); }); !set) {
            set_status(LinkStatus::Failed, set.error());
        }
    }

    void on_timer() {
        const auto now = Clock::now();
        if (!channel) {
            if (now >= next_connect_at) {
                try_connect();
            } else {
                arm_timer();
            }
            return;
        }
        const bool hello_late = hello_deadline && now >= *hello_deadline;
        const bool answer_late =
            std::ranges::any_of(pending, [&](const Pending& p) { return now >= p.deadline; });
        if (hello_late || answer_late) {
            hang_up(LinkStatus::Failed, std::make_error_code(std::errc::timed_out));
            return;
        }
        arm_timer();
    }

    // ---- Messages from the daemon ----

    // The readable callback: takes every waiting message.
    void read_messages() {
        while (channel) {
            auto received = channel->receive(buffer);
            if (!received) {
                hang_up(LinkStatus::Failed, received.error());
                return;
            }
            if (received->status == pal::IoStatus::PeerClosed) {
                hang_up(LinkStatus::NotRunning);
                return;
            }
            if (received->status == pal::IoStatus::WouldBlock) {
                return;
            }
            const std::span<const std::byte> frame(buffer.data(), received->bytes);
            auto header = validate_frame(frame);
            if (!header || !handle(*header, frame.subspan(kFrameHeaderBytes))) {
                hang_up(LinkStatus::Failed, std::make_error_code(std::errc::bad_message));
                return;
            }
        }
    }

    // One valid frame. False if its body doesn't decode.
    bool handle(const FrameHeader& header, std::span<const std::byte> payload) {
        switch (header.opcode) {
        case Opcode::HelloAck: {
            auto body = decode_body<HelloBody>(payload);
            if (!body) {
                return false;
            }
            hello_deadline.reset();
            publish([&](DaemonState& s) {
                s.status = LinkStatus::Connected;
                s.error = {};
                ++s.connection_count;
                s.daemon_name = peer_name(*body);
            });
            arm_timer();
            return true;
        }
        case Opcode::PlaybackStateEvent: {
            auto body = decode_body<PlaybackStateBody>(payload);
            if (!body) {
                return false;
            }
            publish([&](DaemonState& s) {
                s.has_playback = true;
                s.playback = body->state;
                s.volume_percent = body->volume_percent;
                s.position_micros = body->position_micros;
            });
            return true;
        }
        case Opcode::CommandAck:
        case Opcode::CommandNak: {
            auto body = decode_body<CommandResultBody>(payload);
            if (!body) {
                return false;
            }
            // Match the answer to its request by seq_id. One we're not
            // waiting for (it already timed out) is ignored.
            auto it = std::ranges::find(pending, body->request_seq_id, &Pending::id);
            if (it != pending.end()) {
                const Pending done = *it;
                pending.erase(it);
                finish(done.id, done.command, outcome_of(body->status));
                arm_timer();
            }
            return true;
        }
        default:
            return true; // nothing the window needs yet (a Pong, say)
        }
    }

    // ---- Commands from the window ----

    // The wake callback: sends everything the window queued since last time.
    void send_queued() {
        std::vector<Request> requests;
        bool connected = false;
        PlaybackState playback = PlaybackState::Stopped;
        {
            std::lock_guard lock(mutex);
            requests.swap(queue);
            connected = state.status == LinkStatus::Connected;
            playback = state.playback;
        }
        for (const Request& r : requests) {
            if (!connected || !channel) {
                finish(r.id, r.command, CommandOutcome::NotConnected);
                continue;
            }
            if (!send_request(r, playback)) {
                finish(r.id, r.command, CommandOutcome::ConnectionLost);
                connected = false; // the rest get NotConnected
                continue;
            }
            pending.push_back({r.id, r.command, Clock::now() + options.reply_timeout});
        }
        arm_timer();
    }

    bool send_request(const Request& r, PlaybackState playback) {
        const FrameHeader play = make_header(Opcode::PlayReq, r.id);
        const FrameHeader pause = make_header(Opcode::PauseReq, r.id);
        switch (r.command) {
        case Command::Play:
            return send_frame(encode_header(play));
        case Command::Pause:
            return send_frame(encode_header(pause));
        case Command::Stop:
            return send_frame(encode_header(make_header(Opcode::StopReq, r.id)));
        case Command::Toggle:
            // The play/pause media key's rule: pause if playing, else play.
            return send_frame(encode_header(playback == PlaybackState::Playing ? pause : play));
        case Command::SetVolume:
            return send_frame(encode_message(make_header(Opcode::SetVolumeReq, r.id),
                                             SetVolumeBody{.volume_percent = r.volume_percent}));
        }
        return false;
    }
};

std::expected<std::unique_ptr<DaemonLink>, std::error_code>
DaemonLink::start(DaemonLinkOptions options, WakeCallback on_change) {
    auto reactor = pal::Reactor::create();
    if (!reactor) {
        return std::unexpected(reactor.error());
    }
    // The Impl lives on the heap and never moves, so the reactor stays put
    // for the channels that keep a reference to it.
    auto impl =
        std::make_unique<Impl>(std::move(options), std::move(on_change), std::move(*reactor));
    Impl* raw = impl.get();
    impl->thread = std::thread([raw] { raw->run(); });
    return std::unique_ptr<DaemonLink>(new DaemonLink(std::move(impl)));
}

DaemonLink::DaemonLink(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

DaemonLink::~DaemonLink() {
    impl_->reactor.stop();
    impl_->thread.join();
}

DaemonState DaemonLink::state() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->state;
}

RequestId DaemonLink::ask(Command command, std::uint8_t volume_percent) {
    RequestId id = 0;
    {
        std::lock_guard lock(impl_->mutex);
        id = impl_->next_seq_id++;
        impl_->queue.push_back({id, command, volume_percent});
    }
    impl_->reactor.wake();
    return id;
}

RequestId DaemonLink::play() { return ask(Command::Play, 0); }
RequestId DaemonLink::pause() { return ask(Command::Pause, 0); }
RequestId DaemonLink::stop() { return ask(Command::Stop, 0); }
RequestId DaemonLink::toggle() { return ask(Command::Toggle, 0); }
RequestId DaemonLink::set_volume(std::uint8_t percent) { return ask(Command::SetVolume, percent); }

} // namespace am::client
