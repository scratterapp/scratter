// IPC round-trip benchmark (spec section 5, bootstrap step 2: "sub-25 µs
// round-trip latency").
//
// A "daemon" thread listens and answers every Ping with a Pong. The main
// thread connects, then sends Pings one at a time and times how long each
// takes to come back. Each thread runs its own Reactor, exactly as the two
// processes will, so a round trip includes everything the real thing does:
// encode, send, epoll wake-up, receive, validate, and the same back again.
// Threads instead of two processes only saves the setup; the kernel path for
// a Unix socket is the same either way.
//
//   ./build/linux-release/bench/ipc_ping_pong [round_trips]
//
// Build with linux-release for real numbers; debug and sanitizer builds are
// several times slower. Exits with status 1 if the median is over 25 µs.

#include "ipc_channel.hpp"
#include "reactor.hpp"

#include "common/ipc_protocol.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace am;
using Clock = std::chrono::steady_clock;

constexpr const char* kEndpoint = "am-ping-pong";
constexpr double kTargetMicros = 25.0;
constexpr int kWarmupRoundTrips = 1000;

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
            .count());
}

common::FrameHeaderBytes make_frame(common::Opcode opcode, std::uint64_t seq_id) {
    common::FrameHeader header{};
    header.opcode = opcode;
    header.seq_id = seq_id;
    header.timestamp_ns = now_ns();
    return common::encode_header(header);
}

[[noreturn]] void die(const char* what) {
    std::fprintf(stderr, "ipc_ping_pong: %s\n", what);
    std::exit(2);
}

// The daemon side: accept one client and answer each Ping until it hangs up.
void run_server(std::atomic<bool>& listening) {
    auto reactor = pal::Reactor::create();
    if (!reactor) {
        die("server: Reactor::create failed");
    }
    auto listener = pal::IpcListener::listen(*reactor, kEndpoint);
    if (!listener) {
        die("server: listen failed (is XDG_RUNTIME_DIR set?)");
    }

    std::optional<pal::IpcChannel> client;
    std::array<std::byte, common::kMaxFrameBytes> buffer{};

    listener->set_accept_callback([&] {
        auto accepted = listener->accept();
        if (!accepted || !*accepted) {
            return;
        }
        client = std::move(**accepted);
        client->set_readable_callback([&] {
            for (;;) {
                const auto received = client->receive(buffer);
                if (!received || received->status == pal::IoStatus::PeerClosed) {
                    reactor->stop();
                    return;
                }
                if (received->status == pal::IoStatus::WouldBlock) {
                    return;
                }
                const auto ping = common::validate_frame(std::span(buffer).first(received->bytes));
                if (!ping || ping->opcode != common::Opcode::Ping) {
                    die("server: bad frame");
                }
                const auto pong = make_frame(common::Opcode::Pong, ping->seq_id);
                if (client->send(pong).value_or(pal::IoStatus::PeerClosed) != pal::IoStatus::Done) {
                    die("server: send failed");
                }
            }
        });
    });

    listening.store(true, std::memory_order_release);
    if (!reactor->run()) {
        die("server: reactor failed");
    }
}

} // namespace

int main(int argc, char** argv) {
    const int round_trips = argc > 1 ? std::atoi(argv[1]) : 100000;
    if (round_trips <= 0) {
        die("round_trips must be a positive number");
    }

    std::atomic<bool> listening{false};
    std::thread server(run_server, std::ref(listening));
    while (!listening.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    auto reactor = pal::Reactor::create();
    if (!reactor) {
        die("client: Reactor::create failed");
    }
    auto connected = pal::IpcChannel::connect(*reactor, kEndpoint);
    if (!connected) {
        die("client: connect failed");
    }
    std::optional<pal::IpcChannel> channel = std::move(*connected);

    std::array<std::byte, common::kMaxFrameBytes> buffer{};
    bool got_pong = false;
    std::uint64_t expected_seq = 0;
    channel->set_readable_callback([&] {
        const auto received = channel->receive(buffer);
        if (!received || received->status != pal::IoStatus::Done) {
            return;
        }
        const auto pong = common::validate_frame(std::span(buffer).first(received->bytes));
        if (!pong || pong->opcode != common::Opcode::Pong || pong->seq_id != expected_seq) {
            die("client: bad frame");
        }
        got_pong = true;
    });

    std::vector<double> micros;
    micros.reserve(static_cast<std::size_t>(round_trips));

    for (int i = 0; i < kWarmupRoundTrips + round_trips; ++i) {
        expected_seq = static_cast<std::uint64_t>(i);
        got_pong = false;
        const auto start = Clock::now();

        if (channel->send(make_frame(common::Opcode::Ping, expected_seq))
                .value_or(pal::IoStatus::PeerClosed) != pal::IoStatus::Done) {
            die("client: send failed");
        }
        while (!got_pong) {
            if (!reactor->run_once(std::chrono::milliseconds{1000})) {
                die("client: reactor failed");
            }
        }

        const std::chrono::duration<double, std::micro> elapsed = Clock::now() - start;
        if (i >= kWarmupRoundTrips) {
            micros.push_back(elapsed.count());
        }
    }

    channel.reset(); // the server sees PeerClosed and stops
    server.join();

    std::sort(micros.begin(), micros.end());
    const auto at = [&](double fraction) {
        return micros[static_cast<std::size_t>(fraction * static_cast<double>(micros.size() - 1))];
    };
    const double median = at(0.50);

    std::printf("%d round trips of a 32-byte frame\n", round_trips);
    std::printf("  min    %7.2f us\n", micros.front());
    std::printf("  median %7.2f us\n", median);
    std::printf("  p99    %7.2f us\n", at(0.99));
    std::printf("  p99.9  %7.2f us\n", at(0.999));
    std::printf("  max    %7.2f us\n", micros.back());
    std::printf("target: median under %.0f us: %s\n", kTargetMicros,
                median < kTargetMicros ? "PASS" : "FAIL");
    return median < kTargetMicros ? 0 : 1;
}
