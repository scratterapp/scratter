// Tests for the Linux IPC channel (src/pal/linux/unix_seqpacket_ipc.cpp).
//
// Both ends run in this one process on one reactor: the listener, the client
// channel and the accepted server channel. Each test points $XDG_RUNTIME_DIR at
// a fresh temporary directory, so tests don't see each other's socket files or
// a real daemon.

#include "ipc_channel.hpp"
#include "reactor.hpp"

#include "common/ipc_protocol.hpp"

#include <catch2/catch_test_macros.hpp>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace {

using am::pal::IoStatus;
using am::pal::IpcChannel;
using am::pal::IpcListener;
using am::pal::Reactor;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

constexpr const char* kEndpoint = "am-test";

// Makes a temporary directory and points $XDG_RUNTIME_DIR at it.
struct TempRuntimeDir {
    std::string path;

    TempRuntimeDir() {
        std::string pattern = "/tmp/am-ipc-test-XXXXXX";
        REQUIRE(::mkdtemp(pattern.data()) != nullptr);
        path = pattern;
        REQUIRE(::setenv("XDG_RUNTIME_DIR", path.c_str(), 1) == 0);
    }
    TempRuntimeDir(const TempRuntimeDir&) = delete;
    TempRuntimeDir& operator=(const TempRuntimeDir&) = delete;
    ~TempRuntimeDir() {
        ::unlink(socket_file().c_str());
        ::rmdir(path.c_str());
    }

    [[nodiscard]] std::string socket_file() const { return path + "/" + kEndpoint + ".sock"; }
};

template <typename Done>
bool run_until(Reactor& reactor, Done done, std::chrono::milliseconds limit = 2000ms) {
    const auto deadline = Clock::now() + limit;
    while (!done()) {
        if (Clock::now() > deadline || !reactor.run_once(10ms)) {
            return false;
        }
    }
    return true;
}

std::vector<std::byte> message_of(std::size_t size, std::uint8_t fill) {
    return std::vector<std::byte>(size, static_cast<std::byte>(fill));
}

// A connected pair: the listener, the UI's channel and the daemon's channel.
struct Connection {
    TempRuntimeDir dir;
    Reactor reactor = std::move(*Reactor::create());
    IpcListener listener = std::move(*IpcListener::listen(reactor, kEndpoint));
    IpcChannel client = std::move(*IpcChannel::connect(reactor, kEndpoint));
    IpcChannel server = accept_one();

    IpcChannel accept_one() {
        auto accepted = listener.accept();
        REQUIRE(accepted.has_value());
        REQUIRE(accepted->has_value());
        return std::move(**accepted);
    }
};

} // namespace

TEST_CASE("connect fails when nobody is listening", "[ipc]") {
    TempRuntimeDir dir;
    auto reactor = Reactor::create();
    REQUIRE(reactor.has_value());

    const auto channel = IpcChannel::connect(*reactor, kEndpoint);
    REQUIRE_FALSE(channel.has_value());
    CHECK(channel.error() == std::errc::no_such_file_or_directory);
}

TEST_CASE("bad endpoint names and a missing runtime dir are rejected", "[ipc]") {
    TempRuntimeDir dir;
    auto reactor = Reactor::create();
    REQUIRE(reactor.has_value());

    for (const char* name : {"", "../escape", "with space", "a/b"}) {
        INFO("endpoint '" << name << "'");
        const auto listener = IpcListener::listen(*reactor, name);
        REQUIRE_FALSE(listener.has_value());
        CHECK(listener.error() == std::errc::invalid_argument);
    }

    const std::string too_long(200, 'x');
    const auto long_listener = IpcListener::listen(*reactor, too_long);
    REQUIRE_FALSE(long_listener.has_value());
    CHECK(long_listener.error() == std::errc::filename_too_long);

    REQUIRE(::unsetenv("XDG_RUNTIME_DIR") == 0);
    const auto no_dir = IpcListener::listen(*reactor, kEndpoint);
    REQUIRE_FALSE(no_dir.has_value());
    CHECK(no_dir.error() == std::errc::no_such_file_or_directory);
}

TEST_CASE("accept() returns nullopt when nobody is waiting", "[ipc]") {
    TempRuntimeDir dir;
    auto reactor = Reactor::create();
    REQUIRE(reactor.has_value());
    auto listener = IpcListener::listen(*reactor, kEndpoint);
    REQUIRE(listener.has_value());

    const auto accepted = listener->accept();
    REQUIRE(accepted.has_value());
    CHECK_FALSE(accepted->has_value());
}

TEST_CASE("the accept callback runs when a client connects", "[ipc]") {
    TempRuntimeDir dir;
    auto reactor = Reactor::create();
    REQUIRE(reactor.has_value());
    auto listener = IpcListener::listen(*reactor, kEndpoint);
    REQUIRE(listener.has_value());

    std::vector<IpcChannel> clients;
    listener->set_accept_callback([&] {
        for (;;) {
            auto accepted = listener->accept();
            if (!accepted || !*accepted) {
                break;
            }
            clients.push_back(std::move(**accepted));
        }
    });

    auto first = IpcChannel::connect(*reactor, kEndpoint);
    auto second = IpcChannel::connect(*reactor, kEndpoint);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(run_until(*reactor, [&] { return clients.size() == 2; }));
}

TEST_CASE("the socket file is private and removed by the listener", "[ipc]") {
    TempRuntimeDir dir;
    auto reactor = Reactor::create();
    REQUIRE(reactor.has_value());

    {
        auto listener = IpcListener::listen(*reactor, kEndpoint);
        REQUIRE(listener.has_value());

        struct stat info {};
        REQUIRE(::stat(dir.socket_file().c_str(), &info) == 0);
        CHECK(S_ISSOCK(info.st_mode));
        CHECK((info.st_mode & 0777) == 0600);
    }

    CHECK(::access(dir.socket_file().c_str(), F_OK) != 0);
}

TEST_CASE("a second listener on the same endpoint gets address_in_use", "[ipc]") {
    TempRuntimeDir dir;
    auto reactor = Reactor::create();
    REQUIRE(reactor.has_value());

    auto first = IpcListener::listen(*reactor, kEndpoint);
    REQUIRE(first.has_value());

    const auto second = IpcListener::listen(*reactor, kEndpoint);
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error() == std::errc::address_in_use);

    // The failed attempt must not have removed the first listener's file.
    CHECK(IpcChannel::connect(*reactor, kEndpoint).has_value());
}

TEST_CASE("a socket file left by a crashed daemon is replaced", "[ipc]") {
    TempRuntimeDir dir;

    // Bind a socket and close it without unlinking, like a crash would.
    {
        const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET, 0);
        REQUIRE(fd >= 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        const std::string path = dir.socket_file();
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        REQUIRE(::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
        ::close(fd);
    }
    REQUIRE(::access(dir.socket_file().c_str(), F_OK) == 0);

    auto reactor = Reactor::create();
    REQUIRE(reactor.has_value());
    auto listener = IpcListener::listen(*reactor, kEndpoint);
    REQUIRE(listener.has_value());
    CHECK(IpcChannel::connect(*reactor, kEndpoint).has_value());
}

TEST_CASE("messages keep their boundaries in both directions", "[ipc]") {
    Connection c;
    std::array<std::byte, am::common::kMaxFrameBytes> buffer{};

    // Three sends of different sizes must come out as three receives.
    for (const std::size_t size : {32UL, 1UL, am::common::kMaxFrameBytes}) {
        const auto message = message_of(size, static_cast<std::uint8_t>(size));
        const auto sent = c.client.send(message);
        REQUIRE(sent.has_value());
        CHECK(*sent == IoStatus::Done);
    }
    for (const std::size_t size : {32UL, 1UL, am::common::kMaxFrameBytes}) {
        const auto received = c.server.receive(buffer);
        REQUIRE(received.has_value());
        REQUIRE(received->status == IoStatus::Done);
        CHECK(received->bytes == size);
        CHECK(buffer[size - 1] == static_cast<std::byte>(static_cast<std::uint8_t>(size)));
    }

    // And back the other way.
    REQUIRE(c.server.send(message_of(40, 7)).value() == IoStatus::Done);
    const auto reply = c.client.receive(buffer);
    REQUIRE(reply.has_value());
    CHECK(reply->status == IoStatus::Done);
    CHECK(reply->bytes == 40);
}

TEST_CASE("receive() with nothing waiting is WouldBlock", "[ipc]") {
    Connection c;
    std::array<std::byte, 64> buffer{};
    const auto received = c.server.receive(buffer);
    REQUIRE(received.has_value());
    CHECK(received->status == IoStatus::WouldBlock);
    CHECK(received->bytes == 0);
}

TEST_CASE("the readable callback runs when a message arrives", "[ipc]") {
    Connection c;
    std::array<std::byte, am::common::kMaxFrameBytes> buffer{};
    int messages = 0;

    c.server.set_readable_callback([&] {
        for (;;) {
            const auto received = c.server.receive(buffer);
            if (!received || received->status != IoStatus::Done) {
                break;
            }
            ++messages;
        }
    });

    REQUIRE(c.client.send(message_of(32, 1)).value() == IoStatus::Done);
    REQUIRE(c.client.send(message_of(32, 2)).value() == IoStatus::Done);
    CHECK(run_until(c.reactor, [&] { return messages == 2; }));
}

TEST_CASE("message size limits", "[ipc]") {
    Connection c;

    SECTION("empty messages are refused") {
        const auto sent = c.client.send({});
        REQUIRE_FALSE(sent.has_value());
        CHECK(sent.error() == std::errc::invalid_argument);
    }

    SECTION("messages over kMaxFrameBytes are refused") {
        const auto sent = c.client.send(message_of(am::common::kMaxFrameBytes + 1, 0));
        REQUIRE_FALSE(sent.has_value());
        CHECK(sent.error() == std::errc::message_size);
    }

    SECTION("a message too big for the buffer is dropped, and the next one is fine") {
        REQUIRE(c.client.send(message_of(100, 1)).value() == IoStatus::Done);
        REQUIRE(c.client.send(message_of(10, 2)).value() == IoStatus::Done);

        std::array<std::byte, 50> small{};
        const auto too_big = c.server.receive(small);
        REQUIRE_FALSE(too_big.has_value());
        CHECK(too_big.error() == std::errc::message_size);

        const auto next = c.server.receive(small);
        REQUIRE(next.has_value());
        CHECK(next->status == IoStatus::Done);
        CHECK(next->bytes == 10);
        CHECK(small[0] == std::byte{2});
    }
}

TEST_CASE("a full socket gives WouldBlock, then the writable callback", "[ipc]") {
    Connection c;
    const auto message = message_of(am::common::kMaxFrameBytes, 9);

    int sent = 0;
    for (;;) {
        const auto result = c.client.send(message);
        REQUIRE(result.has_value());
        if (*result == IoStatus::WouldBlock) {
            break;
        }
        ++sent;
        REQUIRE(sent < 10000); // the kernel buffer is finite
    }
    INFO(sent << " full-size frames fit before WouldBlock");
    CHECK(sent > 0);

    bool writable = false;
    c.client.set_writable_callback([&] { writable = true; });

    // Nobody is reading yet, so the callback mustn't run.
    REQUIRE(c.reactor.run_once(20ms).has_value());
    CHECK_FALSE(writable);

    std::array<std::byte, am::common::kMaxFrameBytes> buffer{};
    for (int i = 0; i < sent; ++i) {
        REQUIRE(c.server.receive(buffer).value().status == IoStatus::Done);
    }
    CHECK(run_until(c.reactor, [&] { return writable; }));
    CHECK(c.client.send(message).value() == IoStatus::Done);
}

TEST_CASE("the other end closing gives PeerClosed", "[ipc]") {
    Connection c;
    std::array<std::byte, 64> buffer{};

    bool readable = false;
    c.server.set_readable_callback([&] { readable = true; });

    { [[maybe_unused]] IpcChannel gone = std::move(c.client); }

    CHECK(run_until(c.reactor, [&] { return readable; }));
    const auto received = c.server.receive(buffer);
    REQUIRE(received.has_value());
    CHECK(received->status == IoStatus::PeerClosed);

    const auto sent = c.server.send(message_of(32, 0));
    REQUIRE(sent.has_value());
    CHECK(*sent == IoStatus::PeerClosed);

    // The channel stops watching its socket after PeerClosed, so the reactor
    // doesn't keep reporting the hang-up.
    readable = false;
    REQUIRE(c.reactor.run_once(20ms).has_value());
    CHECK_FALSE(readable);
}

TEST_CASE("a hang-up with no readable callback doesn't spin the reactor", "[ipc]") {
    Connection c;
    { [[maybe_unused]] IpcChannel gone = std::move(c.client); }

    // The first loop sees the hang-up and the channel stops watching; after
    // that, run_once() has nothing to report and waits out its timeout.
    REQUIRE(c.reactor.run_once(20ms).has_value());
    const auto start = Clock::now();
    REQUIRE(c.reactor.run_once(20ms).has_value());
    CHECK(Clock::now() - start >= 15ms);

    std::array<std::byte, 64> buffer{};
    const auto received = c.server.receive(buffer);
    REQUIRE(received.has_value());
    CHECK(received->status == IoStatus::PeerClosed);
}

TEST_CASE("a channel can be destroyed from inside its own callback", "[ipc]") {
    Connection c;
    std::optional<IpcChannel> server = std::move(c.server);
    std::array<std::byte, 64> buffer{};

    server->set_readable_callback([&] {
        [[maybe_unused]] auto received = server->receive(buffer);
        server.reset(); // ASan would catch any use of the channel after this
    });

    REQUIRE(c.client.send(message_of(32, 1)).value() == IoStatus::Done);
    CHECK(run_until(c.reactor, [&] { return !server.has_value(); }));

    // The reactor must keep working with the channel gone.
    REQUIRE(c.reactor.run_once(10ms).has_value());
}
