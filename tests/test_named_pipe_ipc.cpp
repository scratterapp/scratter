// Tests for the Windows IPC channel (src/pal/win32/named_pipe_ipc.cpp).
//
// The same contract as test_unix_seqpacket_ipc.cpp, plus what's particular to
// a named pipe: the SID in its name, its owner and DACL, a busy pipe, and a
// message bigger than any frame from a client that isn't ours. Both ends run in
// this one process on one reactor. The pipe namespace is machine-wide, so each
// run uses an endpoint with its process id in it.

#include "ipc_channel.hpp"
#include "reactor.hpp"

#include "common/ipc_protocol.hpp"

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <aclapi.h>
#include <sddl.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
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

const std::string& endpoint() {
    static const std::string name = "am-test-" + std::to_string(::GetCurrentProcessId());
    return name;
}

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

// The current user's SID, as text and as a SID.
struct CurrentUser {
    alignas(TOKEN_USER) unsigned char buf[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]{};
    std::wstring text;

    CurrentUser() {
        DWORD got = 0;
        REQUIRE(::GetTokenInformation(::GetCurrentProcessToken(), TokenUser, buf, sizeof(buf),
                                      &got) != 0);
        LPWSTR sid_text = nullptr;
        REQUIRE(::ConvertSidToStringSidW(sid(), &sid_text) != 0);
        text = sid_text;
        ::LocalFree(sid_text);
    }
    [[nodiscard]] PSID sid() { return reinterpret_cast<TOKEN_USER*>(buf)->User.Sid; }
};

std::wstring full_pipe_name() {
    const CurrentUser user;
    return L"\\\\.\\pipe\\" + std::wstring(endpoint().begin(), endpoint().end()) + L"." + user.text;
}

// Opens the pipe without IpcChannel, as some other program would.
HANDLE open_raw_client() {
    const HANDLE pipe = ::CreateFileW(full_pipe_name().c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                      nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (pipe != INVALID_HANDLE_VALUE) {
        DWORD mode = PIPE_READMODE_MESSAGE;
        REQUIRE(::SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr) != 0);
    }
    return pipe;
}

// A connected pair: the listener, the UI's channel and the daemon's channel.
struct Connection {
    Reactor reactor = std::move(*Reactor::create());
    IpcListener listener = std::move(*IpcListener::listen(reactor, endpoint()));
    IpcChannel client = std::move(*IpcChannel::connect(reactor, endpoint()));
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
    auto reactor = Reactor::create();
    REQUIRE(reactor.has_value());

    const auto channel = IpcChannel::connect(*reactor, endpoint());
    REQUIRE_FALSE(channel.has_value());
    CHECK(channel.error() == std::errc::no_such_file_or_directory);
}

TEST_CASE("bad endpoint names are rejected", "[ipc]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor.has_value());

    for (const char* name : {"", "../escape", "with space", "a\\b", "a/b"}) {
        INFO("endpoint '" << name << "'");
        const auto listener = IpcListener::listen(*reactor, name);
        REQUIRE_FALSE(listener.has_value());
        CHECK(listener.error() == std::errc::invalid_argument);
        const auto channel = IpcChannel::connect(*reactor, name);
        REQUIRE_FALSE(channel.has_value());
        CHECK(channel.error() == std::errc::invalid_argument);
    }

    // A pipe name, \\.\pipe\ and the SID included, is at most 256 characters.
    const std::string too_long(256, 'x');
    const auto long_listener = IpcListener::listen(*reactor, too_long);
    REQUIRE_FALSE(long_listener.has_value());
    CHECK(long_listener.error() == std::errc::filename_too_long);
}

TEST_CASE("accept() returns nullopt when nobody is waiting", "[ipc]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor.has_value());
    auto listener = IpcListener::listen(*reactor, endpoint());
    REQUIRE(listener.has_value());

    const auto accepted = listener->accept();
    REQUIRE(accepted.has_value());
    CHECK_FALSE(accepted->has_value());
}

TEST_CASE("the accept callback runs when a client connects", "[ipc]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor.has_value());
    auto listener = IpcListener::listen(*reactor, endpoint());
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

    auto first = IpcChannel::connect(*reactor, endpoint());
    auto second = IpcChannel::connect(*reactor, endpoint());
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(run_until(*reactor, [&] { return clients.size() == 2; }));
}

TEST_CASE("the pipe is named after the user and only they can open it", "[ipc]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor.has_value());
    auto listener = IpcListener::listen(*reactor, endpoint());
    REQUIRE(listener.has_value());

    // \\.\pipe\<endpoint>.<SID> exists.
    const HANDLE pipe = open_raw_client();
    REQUIRE(pipe != INVALID_HANDLE_VALUE);

    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    REQUIRE(::GetSecurityInfo(pipe, SE_KERNEL_OBJECT,
                              OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner,
                              nullptr, &dacl, nullptr, &sd) == ERROR_SUCCESS);

    CurrentUser user;
    CHECK(::EqualSid(owner, user.sid()) != 0);
    REQUIRE(dacl != nullptr);
    REQUIRE(dacl->AceCount == 1);
    void* ace = nullptr;
    REQUIRE(::GetAce(dacl, 0, &ace) != 0);
    auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
    CHECK(allowed->Header.AceType == ACCESS_ALLOWED_ACE_TYPE);
    CHECK(::EqualSid(&allowed->SidStart, user.sid()) != 0);

    ::LocalFree(sd);
    ::CloseHandle(pipe);
}

TEST_CASE("a second listener on the same endpoint gets address_in_use", "[ipc]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor.has_value());

    auto first = IpcListener::listen(*reactor, endpoint());
    REQUIRE(first.has_value());

    const auto second = IpcListener::listen(*reactor, endpoint());
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error() == std::errc::address_in_use);

    // The failed attempt must not have disturbed the first listener.
    CHECK(IpcChannel::connect(*reactor, endpoint()).has_value());
}

TEST_CASE("the pipe goes away with the listener", "[ipc]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor.has_value());

    {
        auto listener = IpcListener::listen(*reactor, endpoint());
        REQUIRE(listener.has_value());
    }

    const auto channel = IpcChannel::connect(*reactor, endpoint());
    REQUIRE_FALSE(channel.has_value());
    CHECK(channel.error() == std::errc::no_such_file_or_directory);

    // And the name is free for the next daemon.
    CHECK(IpcListener::listen(*reactor, endpoint()).has_value());
}

TEST_CASE("a busy pipe gives resource_unavailable_try_again until accept()", "[ipc]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor.has_value());
    auto listener = IpcListener::listen(*reactor, endpoint());
    REQUIRE(listener.has_value());

    // Connect without accepting or running the reactor until every waiting
    // instance is taken.
    std::vector<IpcChannel> clients;
    std::optional<std::error_code> refused;
    for (int i = 0; i < 64 && !refused; ++i) {
        auto channel = IpcChannel::connect(*reactor, endpoint());
        if (channel) {
            clients.push_back(std::move(*channel));
        } else {
            refused = channel.error();
        }
    }
    REQUIRE(refused.has_value());
    CHECK(*refused == std::errc::resource_unavailable_try_again);
    CHECK_FALSE(clients.empty());

    // Accepting makes room again.
    std::vector<IpcChannel> servers;
    for (;;) {
        auto accepted = listener->accept();
        REQUIRE(accepted.has_value());
        if (!*accepted) {
            break;
        }
        servers.push_back(std::move(**accepted));
    }
    CHECK(servers.size() == clients.size());
    CHECK(IpcChannel::connect(*reactor, endpoint()).has_value());
}

TEST_CASE("messages keep their boundaries in both directions", "[ipc]") {
    Connection c;
    std::array<std::byte, am::common::kMaxFrameBytes> buffer{};

    // Three sends of different sizes must come out as three receives.
    for (const std::size_t size : {std::size_t{32}, std::size_t{1}, am::common::kMaxFrameBytes}) {
        const auto message = message_of(size, static_cast<std::uint8_t>(size));
        const auto sent = c.client.send(message);
        REQUIRE(sent.has_value());
        CHECK(*sent == IoStatus::Done);
    }
    for (const std::size_t size : {std::size_t{32}, std::size_t{1}, am::common::kMaxFrameBytes}) {
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

    // And again, after the first ones were read from inside the callback.
    REQUIRE(c.client.send(message_of(32, 3)).value() == IoStatus::Done);
    CHECK(run_until(c.reactor, [&] { return messages == 3; }));
}

TEST_CASE("a readable callback set after the message arrived still runs", "[ipc]") {
    Connection c;
    REQUIRE(c.client.send(message_of(32, 1)).value() == IoStatus::Done);
    REQUIRE(c.reactor.run_once(20ms).has_value()); // the read finishes with nobody to tell

    bool readable = false;
    c.server.set_readable_callback([&] { readable = true; });
    CHECK(run_until(c.reactor, [&] { return readable; }));
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

        std::array<std::byte, 50> tiny{};
        const auto too_big = c.server.receive(tiny);
        REQUIRE_FALSE(too_big.has_value());
        CHECK(too_big.error() == std::errc::message_size);

        const auto next = c.server.receive(tiny);
        REQUIRE(next.has_value());
        CHECK(next->status == IoStatus::Done);
        CHECK(next->bytes == 10);
        CHECK(tiny[0] == std::byte{2});
    }
}

// IpcChannel never sends more than kMaxFrameBytes, but another program on the
// pipe can. The channel reads past it, reports message_size once, and goes on.
TEST_CASE("a message bigger than any frame is dropped whole", "[ipc]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor.has_value());
    auto listener = IpcListener::listen(*reactor, endpoint());
    REQUIRE(listener.has_value());

    const HANDLE raw = open_raw_client();
    REQUIRE(raw != INVALID_HANDLE_VALUE);
    auto accepted = listener->accept();
    REQUIRE(accepted.has_value());
    REQUIRE(accepted->has_value());
    IpcChannel server = std::move(**accepted);

    int readable = 0;
    server.set_readable_callback([&] { ++readable; });

    // Overlapped writes, since nobody reads until they're all queued. Each
    // gets its own OVERLAPPED, waited for at the end.
    const auto huge = message_of(3 * am::common::kMaxFrameBytes + 5, 1);
    const auto tiny = message_of(32, 2);
    const auto lead = message_of(16, 3);
    std::array<OVERLAPPED, 4> writes{};
    std::size_t next_write = 0;
    auto write = [&](const std::vector<std::byte>& message) {
        OVERLAPPED& ov = writes.at(next_write++);
        ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        const BOOL done =
            ::WriteFile(raw, message.data(), static_cast<DWORD>(message.size()), nullptr, &ov);
        CHECK((done != 0 || ::GetLastError() == ERROR_IO_PENDING));
    };

    std::array<std::byte, am::common::kMaxFrameBytes> buffer{};

    SECTION("arriving while a read is pending") {
        write(huge);
        write(tiny);
    }
    SECTION("already waiting when a read starts") {
        // The lead message completes the pending read. The channel's next
        // ReadFile starts only when receive() takes it, by which time the big
        // one is in the pipe, so that ReadFile finishes at once with part of it.
        write(lead);
        write(huge);
        write(tiny);
        REQUIRE(run_until(*reactor, [&] { return readable > 0; }));
        const auto first = server.receive(buffer);
        REQUIRE(first.has_value());
        CHECK(first->status == IoStatus::Done);
        CHECK(first->bytes == lead.size());
    }

    std::optional<std::error_code> first_error;
    CHECK(run_until(*reactor, [&] {
        const auto received = server.receive(buffer);
        if (!received) {
            first_error = received.error();
            return true;
        }
        return false;
    }));
    REQUIRE(first_error.has_value());
    CHECK(*first_error == std::errc::message_size);

    std::optional<std::size_t> next;
    CHECK(run_until(*reactor, [&] {
        const auto received = server.receive(buffer);
        REQUIRE(received.has_value());
        if (received->status == IoStatus::Done) {
            next = received->bytes;
        }
        return next.has_value();
    }));
    CHECK(next == std::size_t{32});
    CHECK(buffer[0] == std::byte{2});

    // The readable callback keeps working after the oversized message: no
    // packet was miscounted on the way.
    readable = 0;
    write(tiny);
    CHECK(run_until(*reactor, [&] { return readable > 0; }));
    const auto last = server.receive(buffer);
    REQUIRE(last.has_value());
    CHECK(last->status == IoStatus::Done);
    CHECK(last->bytes == tiny.size());

    for (std::size_t i = 0; i < next_write; ++i) {
        DWORD ignored = 0;
        ::GetOverlappedResult(raw, &writes.at(i), &ignored, TRUE);
        ::CloseHandle(writes.at(i).hEvent);
    }
    ::CloseHandle(raw);
}

TEST_CASE("a full pipe gives WouldBlock, then the writable callback", "[ipc]") {
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
        REQUIRE(sent < 10000); // the pipe's buffer is finite
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
        const auto received = c.server.receive(buffer);
        REQUIRE(received.has_value());
        REQUIRE(received->status == IoStatus::Done);
        CHECK(received->bytes == message.size());
    }
    CHECK(run_until(c.reactor, [&] { return writable; }));
    CHECK(c.client.send(message).value() == IoStatus::Done);
}

TEST_CASE("the other end closing gives PeerClosed", "[ipc]") {
    Connection c;
    std::array<std::byte, 64> buffer{};

    bool readable = false;
    c.server.set_readable_callback([&] { readable = true; });

    {
        [[maybe_unused]] IpcChannel gone = std::move(c.client);
    }

    CHECK(run_until(c.reactor, [&] { return readable; }));
    const auto received = c.server.receive(buffer);
    REQUIRE(received.has_value());
    CHECK(received->status == IoStatus::PeerClosed);

    const auto sent = c.server.send(message_of(32, 0));
    REQUIRE(sent.has_value());
    CHECK(*sent == IoStatus::PeerClosed);

    // Nothing more is reported after PeerClosed.
    readable = false;
    REQUIRE(c.reactor.run_once(20ms).has_value());
    CHECK_FALSE(readable);
}

TEST_CASE("the daemon closing gives the UI PeerClosed", "[ipc]") {
    Connection c;
    std::array<std::byte, 64> buffer{};

    bool readable = false;
    c.client.set_readable_callback([&] { readable = true; });
    {
        [[maybe_unused]] IpcChannel gone = std::move(c.server);
    }

    CHECK(run_until(c.reactor, [&] { return readable; }));
    const auto received = c.client.receive(buffer);
    REQUIRE(received.has_value());
    CHECK(received->status == IoStatus::PeerClosed);
}

TEST_CASE("a hang-up with no readable callback doesn't spin the reactor", "[ipc]") {
    Connection c;
    {
        [[maybe_unused]] IpcChannel gone = std::move(c.client);
    }

    REQUIRE(c.reactor.run_once(20ms).has_value());
    const auto start = Clock::now();
    REQUIRE(c.reactor.run_once(20ms).has_value());
    CHECK(Clock::now() - start >= 15ms);

    std::array<std::byte, 64> buffer{};
    const auto received = c.server.receive(buffer);
    REQUIRE(received.has_value());
    CHECK(received->status == IoStatus::PeerClosed);
}

// The peer sends one last message and closes before it is read. The message
// must still come out of receive() before PeerClosed, in both directions
// (amctl watch relies on getting the daemon's last event).
TEST_CASE("messages sent just before a hang-up are still received", "[ipc]") {
    Connection c;
    std::array<std::byte, 64> buffer{};

    SECTION("UI to daemon") {
        REQUIRE(c.client.send(message_of(32, 7)).value() == IoStatus::Done);
        {
            [[maybe_unused]] IpcChannel gone = std::move(c.client);
        }
        REQUIRE(c.reactor.run_once(20ms).has_value());

        const auto last = c.server.receive(buffer);
        REQUIRE(last.has_value());
        CHECK(last->status == IoStatus::Done);
        CHECK(last->bytes == 32);
        CHECK(buffer[0] == std::byte{7});
        const auto after = c.server.receive(buffer);
        REQUIRE(after.has_value());
        CHECK(after->status == IoStatus::PeerClosed);
    }

    SECTION("daemon to UI") {
        REQUIRE(c.server.send(message_of(32, 8)).value() == IoStatus::Done);
        {
            [[maybe_unused]] IpcChannel gone = std::move(c.server);
        }
        REQUIRE(c.reactor.run_once(20ms).has_value());

        const auto last = c.client.receive(buffer);
        REQUIRE(last.has_value());
        CHECK(last->status == IoStatus::Done);
        CHECK(last->bytes == 32);
        CHECK(buffer[0] == std::byte{8});
        const auto after = c.client.receive(buffer);
        REQUIRE(after.has_value());
        CHECK(after->status == IoStatus::PeerClosed);
    }
}

TEST_CASE("a client that leaves before accept() is a channel that's already closed", "[ipc]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor.has_value());
    auto listener = IpcListener::listen(*reactor, endpoint());
    REQUIRE(listener.has_value());

    {
        [[maybe_unused]] auto gone = IpcChannel::connect(*reactor, endpoint());
    }

    auto accepted = listener->accept();
    REQUIRE(accepted.has_value());
    REQUIRE(accepted->has_value());
    std::array<std::byte, 64> buffer{};
    const auto received = (*accepted)->receive(buffer);
    REQUIRE(received.has_value());
    CHECK(received->status == IoStatus::PeerClosed);

    // The listener carries on.
    CHECK(IpcChannel::connect(*reactor, endpoint()).has_value());
}

TEST_CASE("a channel can be destroyed from inside its own callback", "[ipc]") {
    Connection c;
    std::optional<IpcChannel> server = std::move(c.server);
    std::array<std::byte, 64> buffer{};

    server->set_readable_callback([&] {
        [[maybe_unused]] auto received = server->receive(buffer);
        server.reset();
    });

    REQUIRE(c.client.send(message_of(32, 1)).value() == IoStatus::Done);
    CHECK(run_until(c.reactor, [&] { return !server.has_value(); }));

    // The reactor must keep working with the channel gone.
    REQUIRE(c.reactor.run_once(10ms).has_value());
}

TEST_CASE("a listener can be destroyed from inside its accept callback", "[ipc]") {
    auto reactor = Reactor::create();
    REQUIRE(reactor.has_value());
    std::optional<IpcListener> listener = std::move(*IpcListener::listen(*reactor, endpoint()));
    listener->set_accept_callback([&] { listener.reset(); });

    auto client = IpcChannel::connect(*reactor, endpoint());
    REQUIRE(client.has_value());
    CHECK(run_until(*reactor, [&] { return !listener.has_value(); }));
    REQUIRE(reactor->run_once(10ms).has_value());
}
