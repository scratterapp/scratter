#include "daemon/control_server.hpp"

#include "common/app_name.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <utility>

namespace am::daemon {

using common::CommandResultBody;
using common::CommandStatus;
using common::FrameHeader;
using common::Opcode;
using common::PlaybackState;

ControlServer::ControlServer(pal::IpcListener listener, audio::AudioPath& audio,
                             std::function<void()> on_change)
    : listener_(std::move(listener)), audio_(audio), on_change_(std::move(on_change)),
      receive_buffer_(common::kMaxFrameBytes) {
    listener_.set_accept_callback([this] { accept_clients(); });
}

void ControlServer::accept_clients() {
    for (;;) {
        auto accepted = listener_.accept();
        if (!accepted) {
            if (accepted.error() == std::errc::permission_denied) {
                std::fprintf(stderr, "turned away a client running as another user\n");
                continue; // others may be waiting
            }
            std::fprintf(stderr, "accepting a client failed: %s\n",
                         accepted.error().message().c_str());
            return;
        }
        if (!*accepted) {
            return; // nobody else waiting
        }
        auto client = std::make_unique<Client>(std::move(**accepted));
        Client* raw = client.get();
        raw->channel.set_readable_callback([this, raw] {
            read_from(*raw);
            remove_closed_clients(); // may destroy *raw; nothing touches it after this
        });
        clients_.push_back(std::move(client));
    }
}

void ControlServer::read_from(Client& client) {
    // Several messages can be waiting; take them all.
    while (!client.closed) {
        auto received = client.channel.receive(receive_buffer_);
        if (!received) {
            std::fprintf(stderr, "%s: receive failed: %s\n", client.name.c_str(),
                         received.error().message().c_str());
            client.closed = true;
            return;
        }
        if (received->status == pal::IoStatus::WouldBlock) {
            return;
        }
        if (received->status == pal::IoStatus::PeerClosed) {
            std::fprintf(stderr, "%s disconnected\n", client.name.c_str());
            client.closed = true;
            return;
        }

        const std::span<const std::byte> frame(receive_buffer_.data(), received->bytes);
        auto header = common::validate_frame(frame);
        if (!header) {
            // Not our protocol, a different version, or damaged. There's no
            // sensible answer, so hang up.
            std::fprintf(stderr, "%s sent a broken frame (error %d), disconnecting\n",
                         client.name.c_str(), static_cast<int>(header.error()));
            client.closed = true;
            return;
        }
        handle(client, *header, frame.subspan(common::kFrameHeaderBytes));
    }
}

void ControlServer::handle(Client& client, const FrameHeader& header,
                           std::span<const std::byte> payload) {
    switch (header.opcode) {
    case Opcode::HelloReq: {
        const auto hello = common::decode_body<common::HelloBody>(payload);
        if (!hello) {
            reply(client, header, CommandStatus::BadRequest);
            return;
        }
        client.name = common::peer_name(*hello);
        std::fprintf(stderr, "%s connected\n", client.name.c_str());
        const auto ack = common::encode_message(
            next_header(Opcode::HelloAck),
            common::HelloBody{.name = common::make_peer_name(common::kDaemonName)});
        send(client, ack);
        send_state(client);
        return;
    }
    case Opcode::Ping: {
        // A liveness check: the Pong carries the Ping's own seq_id, so the
        // client knows which ping it answers. It changes nothing.
        if (!common::expect_no_body(payload)) {
            reply(client, header, CommandStatus::BadRequest);
            return;
        }
        FrameHeader pong = next_header(Opcode::Pong);
        pong.seq_id = header.seq_id;
        send(client, common::encode_header(pong));
        return;
    }
    case Opcode::PlayReq:
    case Opcode::PauseReq:
    case Opcode::StopReq: {
        if (!common::expect_no_body(payload)) {
            reply(client, header, CommandStatus::BadRequest);
            return;
        }
        const PlaybackState wanted = header.opcode == Opcode::PlayReq    ? PlaybackState::Playing
                                     : header.opcode == Opcode::PauseReq ? PlaybackState::Paused
                                                                         : PlaybackState::Stopped;
        const bool changed = audio_.state() != wanted;
        audio_.set_state(wanted);
        if (changed) {
            on_change_(); // before the ack, so the telemetry page is current when it arrives
        }
        // Answer the request first, then tell everyone (this client too).
        reply(client, header, CommandStatus::Ok);
        if (changed) {
            broadcast_state();
        }
        return;
    }
    case Opcode::SetVolumeReq: {
        const auto body = common::decode_body<common::SetVolumeBody>(payload);
        if (!body) {
            reply(client, header, CommandStatus::BadRequest); // wrong size, or over 100
            return;
        }
        const bool changed = audio_.volume() != body->volume_percent;
        audio_.set_volume(body->volume_percent);
        if (changed) {
            on_change_();
        }
        reply(client, header, CommandStatus::Ok);
        if (changed) {
            broadcast_state();
        }
        return;
    }
    default:
        // A real opcode, just not one this daemon handles yet (LoadTrackReq,
        // SeekReq...), or one only the daemon is meant to send.
        reply(client, header, CommandStatus::NotSupported);
        return;
    }
}

void ControlServer::reply(Client& client, const FrameHeader& request, CommandStatus status) {
    const auto frame = common::encode_message(
        next_header(status == CommandStatus::Ok ? Opcode::CommandAck : Opcode::CommandNak),
        CommandResultBody{.request_seq_id = request.seq_id,
                          .request_opcode = request.opcode,
                          .status = status,
                          .reserved = {}});
    send(client, frame);
}

void ControlServer::send_state(Client& client) {
    const auto frame = common::encode_message(next_header(Opcode::PlaybackStateEvent),
                                              common::PlaybackStateBody{
                                                  .position_micros = audio_.position_micros(),
                                                  .state = audio_.state(),
                                                  .volume_percent = audio_.volume(),
                                                  .reserved = {},
                                              });
    send(client, frame);
}

void ControlServer::broadcast_state() {
    for (auto& client : clients_) {
        if (!client->closed) {
            send_state(*client);
        }
    }
}

void ControlServer::send(Client& client, std::span<const std::byte> frame) {
    if (client.closed) {
        return;
    }
    auto sent = client.channel.send(frame);
    if (!sent) {
        std::fprintf(stderr, "%s: send failed: %s\n", client.name.c_str(),
                     sent.error().message().c_str());
        client.closed = true;
    } else if (*sent == pal::IoStatus::WouldBlock) {
        std::fprintf(stderr, "%s isn't reading its messages, disconnecting\n", client.name.c_str());
        client.closed = true;
    } else if (*sent == pal::IoStatus::PeerClosed) {
        client.closed = true;
    }
}

void ControlServer::remove_closed_clients() {
    std::erase_if(clients_, [](const auto& client) { return client->closed; });
}

FrameHeader ControlServer::next_header(Opcode opcode) {
    FrameHeader header{};
    header.opcode = opcode;
    header.seq_id = next_seq_id_++;
    header.timestamp_ns =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now().time_since_epoch())
                                       .count());
    return header;
}

} // namespace am::daemon
