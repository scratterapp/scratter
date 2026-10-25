// End-to-end test of the daemon (src/daemon/main_daemon.cpp), issue #50, and
// of amctl (tools/amctl/), issue #51, driving it. Also checks amctl's
// coverage() list against what the daemon really handles, so every request
// the daemon learns gets an amctl command.
//
// Starts the real daemon binary as a child process, with --no-audio so it
// works on CI machines without a sound server and stays quiet on yours, and
// with a name of its own (--name) and a private $XDG_RUNTIME_DIR so it can't
// collide with a daemon you're running. Then it talks to it the way am-ui
// will: over the IPC socket, and by reading the telemetry page.
//
// The media keys part plays the desktop's side of MPRIS over D-Bus, and skips
// with no session bus (CI runs ctest under dbus-run-session, which has one).
//
// Linux-only for now: it starts and signals the child with posix_spawn and
// kill, and the daemon needs the Linux reactor, socket and shared memory.

#include "amctl/amctl_args.hpp"
#include "common/ipc_messages.hpp"
#include "common/telemetry_shm.hpp"
#include "ipc_channel.hpp"
#include "reactor.hpp"
#include "shm_mapping.hpp"

#include <catch2/catch_test_macros.hpp>

#include <systemd/sd-bus.h>

#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fcntl.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

extern char** environ;

namespace {

using namespace std::chrono_literals;
using namespace am::common;
using am::pal::IoStatus;
using am::pal::IpcChannel;
using am::pal::Reactor;
using am::pal::ShmMapping;

// A temporary $XDG_RUNTIME_DIR for this test process and the daemons it starts.
struct RuntimeDir {
    std::filesystem::path path;

    RuntimeDir() {
        std::string templ = (std::filesystem::temp_directory_path() / "am-daemon-XXXXXX").string();
        REQUIRE(::mkdtemp(templ.data()) != nullptr);
        path = templ;
        REQUIRE(::setenv("XDG_RUNTIME_DIR", path.c_str(), 1) == 0);
    }
    ~RuntimeDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

// A failed REQUIRE aborts the test process (no exceptions, so no destructors
// run). Without this, the daemon child would keep running, and ctest would
// wait forever for it to close the output it inherited.
volatile pid_t g_daemon_to_kill = -1;

extern "C" void kill_daemon_on_abort(int) {
    if (g_daemon_to_kill > 0) {
        ::kill(g_daemon_to_kill, SIGKILL);
    }
    ::signal(SIGABRT, SIG_DFL);
    ::raise(SIGABRT);
}

// A daemon child process. The destructor kills it if the test didn't stop it.
struct Daemon {
    pid_t pid = -1;

    explicit Daemon(const std::string& name) {
        std::vector<std::string> args = {AM_DAEMON_PATH, "--name", name, "--no-audio"};
        std::vector<char*> argv;
        for (auto& arg : args) {
            argv.push_back(arg.data());
        }
        argv.push_back(nullptr);
        REQUIRE(::posix_spawn(&pid, AM_DAEMON_PATH, nullptr, nullptr, argv.data(), environ) == 0);
        if (g_daemon_to_kill <= 0) { // the first one; a second exits on its own
            g_daemon_to_kill = pid;
            ::signal(SIGABRT, kill_daemon_on_abort);
        }
    }
    ~Daemon() {
        if (pid > 0) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, nullptr, 0);
            exited();
        }
    }
    void exited() {
        if (g_daemon_to_kill == pid) {
            g_daemon_to_kill = -1;
        }
        pid = -1;
    }

    // Waits for the process to exit and returns its exit code, or nothing if
    // it didn't exit within `timeout` or was killed by a signal.
    std::optional<int> wait_for_exit(std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            int status = 0;
            if (::waitpid(pid, &status, WNOHANG) == pid) {
                exited();
                if (WIFEXITED(status)) {
                    return WEXITSTATUS(status);
                }
                return std::nullopt;
            }
            std::this_thread::sleep_for(10ms);
        }
        return std::nullopt;
    }
};

// The client side: one connection to the daemon, plus helpers to send a
// request and wait for a particular message back.
struct Client {
    Reactor reactor;
    std::optional<IpcChannel> channel;
    std::vector<std::byte> buffer = std::vector<std::byte>(kMaxFrameBytes);
    std::uint64_t next_seq_id = 1;

    explicit Client(Reactor r) : reactor(std::move(r)) {}

    // Keeps trying until the daemon has created its socket.
    bool connect(std::string_view name) {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (auto c = IpcChannel::connect(reactor, name)) {
                channel.emplace(std::move(*c));
                return true;
            }
            std::this_thread::sleep_for(20ms);
        }
        return false;
    }

    // Sends a frame with no body (Play, Pause, Stop) or with one. Returns the
    // seq_id used.
    std::uint64_t send(Opcode opcode) {
        FrameHeader h{};
        h.opcode = opcode;
        h.seq_id = next_seq_id++;
        const auto frame = encode_header(h);
        REQUIRE(channel->send(frame) == IoStatus::Done);
        return h.seq_id;
    }
    template <WireBody Body> std::uint64_t send(Opcode opcode, const Body& body) {
        FrameHeader h{};
        h.opcode = opcode;
        h.seq_id = next_seq_id++;
        const auto frame = encode_message(h, body);
        REQUIRE(channel->send(frame) == IoStatus::Done);
        return h.seq_id;
    }

    // Reads messages until one with `opcode` arrives, skipping others (the
    // PlaybackStateEvents that come with every change, for example). Returns
    // its payload, or nothing on timeout or hang-up.
    std::optional<std::vector<std::byte>> wait_for(Opcode opcode) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            auto r = channel->receive(buffer);
            REQUIRE(r.has_value());
            if (r->status == IoStatus::PeerClosed) {
                return std::nullopt;
            }
            if (r->status == IoStatus::WouldBlock) {
                (void)reactor.run_once(10ms);
                continue;
            }
            const std::span<const std::byte> frame(buffer.data(), r->bytes);
            auto header = validate_frame(frame);
            REQUIRE(header.has_value());
            if (header->opcode == opcode) {
                auto payload = frame.subspan(kFrameHeaderBytes);
                return std::vector<std::byte>(payload.begin(), payload.end());
            }
        }
        return std::nullopt;
    }

    // Waits for the CommandAck or CommandNak answering `seq_id`.
    std::optional<CommandResultBody> wait_for_result(Opcode expected, std::uint64_t seq_id) {
        auto payload = wait_for(expected);
        if (!payload) {
            return std::nullopt;
        }
        auto body = decode_body<CommandResultBody>(*payload);
        REQUIRE(body.has_value());
        CHECK(body->request_seq_id == seq_id);
        return *body;
    }
};

// Reads the telemetry page until the play state is `wanted`, or 2 s pass.
bool telemetry_shows(const TelemetryPage& page, PlaybackState wanted) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto s = page.try_read(); s && s->playback_state == wanted) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return false;
}

// Starts amctl with `args` and returns its pid, or -1. Its standard output
// goes to `output_file` if one is given, else to the test's output, which
// ctest shows on failure.
pid_t start_amctl(std::vector<std::string> args, const std::filesystem::path& output_file = {}) {
    args.insert(args.begin(), AM_AMCTL_PATH);
    std::vector<char*> argv;
    for (auto& arg : args) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    ::posix_spawn_file_actions_init(&actions);
    if (!output_file.empty()) {
        ::posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, output_file.c_str(),
                                           O_WRONLY | O_CREAT | O_TRUNC, 0600);
    }
    pid_t pid = -1;
    const int spawned = ::posix_spawn(&pid, AM_AMCTL_PATH, &actions, nullptr, argv.data(), environ);
    ::posix_spawn_file_actions_destroy(&actions);
    return spawned == 0 ? pid : -1;
}

// Waits for amctl to exit and returns its exit code, or -1 if it didn't exit
// normally.
int wait_for_amctl(pid_t pid) {
    if (pid < 0) {
        return -1;
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
}

// Runs amctl with `args` and returns its exit code.
int run_amctl(std::vector<std::string> args) {
    return wait_for_amctl(start_amctl(std::move(args)));
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string unique_name() { return "am-test-daemon-" + std::to_string(::getpid()); }

} // namespace

TEST_CASE("the daemon answers commands and publishes its state", "[daemon]") {
    RuntimeDir dir;
    const std::string name = unique_name();
    Daemon daemon(name);

    auto reactor = Reactor::create();
    REQUIRE(reactor.has_value());
    Client client(std::move(*reactor));
    REQUIRE(client.connect(name));

    // Hello: the daemon says who it is, then sends the current state.
    client.send(Opcode::HelloReq, HelloBody{.name = make_peer_name("test_daemon")});
    auto hello = client.wait_for(Opcode::HelloAck);
    REQUIRE(hello.has_value());
    auto hello_body = decode_body<HelloBody>(*hello);
    REQUIRE(hello_body.has_value());
    CHECK(peer_name(*hello_body) == kDaemonName);

    auto state = client.wait_for(Opcode::PlaybackStateEvent);
    REQUIRE(state.has_value());
    auto state_body = decode_body<PlaybackStateBody>(*state);
    REQUIRE(state_body.has_value());
    CHECK(state_body->state == PlaybackState::Playing);
    CHECK(state_body->volume_percent == 100);

    // The telemetry page exists and says the same.
    auto shm = ShmMapping::open_read_only(name + "_telemetry_v2", kTelemetryPageSize);
    REQUIRE(shm.has_value());
    const auto& page = *reinterpret_cast<const TelemetryPage*>(shm->bytes().data());
    CHECK(telemetry_shows(page, PlaybackState::Playing));

    SECTION("pause and play") {
        auto seq = client.send(Opcode::PauseReq);
        auto result = client.wait_for_result(Opcode::CommandAck, seq);
        REQUIRE(result.has_value());
        CHECK(result->request_opcode == Opcode::PauseReq);
        CHECK(result->status == CommandStatus::Ok);
        CHECK(telemetry_shows(page, PlaybackState::Paused));

        seq = client.send(Opcode::PlayReq);
        REQUIRE(client.wait_for_result(Opcode::CommandAck, seq).has_value());
        CHECK(telemetry_shows(page, PlaybackState::Playing));

        seq = client.send(Opcode::StopReq);
        REQUIRE(client.wait_for_result(Opcode::CommandAck, seq).has_value());
        CHECK(telemetry_shows(page, PlaybackState::Stopped));
    }

    SECTION("volume changes are announced, bad ones refused") {
        auto seq = client.send(Opcode::SetVolumeReq, SetVolumeBody{.volume_percent = 40});
        REQUIRE(client.wait_for_result(Opcode::CommandAck, seq).has_value());
        auto event = client.wait_for(Opcode::PlaybackStateEvent);
        REQUIRE(event.has_value());
        CHECK(decode_body<PlaybackStateBody>(*event)->volume_percent == 40);

        seq = client.send(Opcode::SetVolumeReq, SetVolumeBody{.volume_percent = 101});
        auto nak = client.wait_for_result(Opcode::CommandNak, seq);
        REQUIRE(nak.has_value());
        CHECK(nak->status == CommandStatus::BadRequest);
    }

    SECTION("requests it can't handle yet get NotSupported") {
        auto seq = client.send(Opcode::SeekReq);
        auto nak = client.wait_for_result(Opcode::CommandNak, seq);
        REQUIRE(nak.has_value());
        CHECK(nak->status == CommandStatus::NotSupported);
    }

    SECTION("media keys pause and resume it") {
        // The same call playerctl play-pause makes.
        sd_bus* bus = nullptr;
        if (sd_bus_open_user(&bus) < 0) {
            WARN("no D-Bus session bus here, skipping");
        } else {
            const std::string bus_name = "org.mpris.MediaPlayer2." + name;
            auto play_pause = [&] {
                return sd_bus_call_method(bus, bus_name.c_str(), "/org/mpris/MediaPlayer2",
                                          "org.mpris.MediaPlayer2.Player", "PlayPause", nullptr,
                                          nullptr, "");
            };
            CHECK(play_pause() >= 0);
            CHECK(telemetry_shows(page, PlaybackState::Paused));
            // Connected clients hear about it, as if amctl had paused it.
            auto event = client.wait_for(Opcode::PlaybackStateEvent);
            REQUIRE(event.has_value());
            CHECK(decode_body<PlaybackStateBody>(*event)->state == PlaybackState::Paused);

            CHECK(play_pause() >= 0);
            CHECK(telemetry_shows(page, PlaybackState::Playing));
            sd_bus_flush_close_unref(bus);
        }
    }

    SECTION("a second daemon with the same name refuses to start") {
        Daemon second(name);
        CHECK(second.wait_for_exit(10s) == 3);
    }

    // SIGTERM (what `kill` and systemd send) shuts it down cleanly: exit code
    // 0, and the shared-memory name is gone.
    REQUIRE(::kill(daemon.pid, SIGTERM) == 0);
    CHECK(daemon.wait_for_exit(10s) == 0);
    auto gone = ShmMapping::open_read_only(name + "_telemetry_v2", kTelemetryPageSize);
    REQUIRE_FALSE(gone.has_value());
    CHECK(gone.error() == std::errc::no_such_file_or_directory);
    CHECK(!std::filesystem::exists(dir.path / (name + ".sock")));
}

// $XDG_RUNTIME_DIR has to be private: anyone who can write to it could swap
// the socket for their own. The daemon must refuse to start rather than use it.
TEST_CASE("the daemon refuses a runtime dir other users can open", "[daemon]") {
    RuntimeDir dir;
    REQUIRE(::chmod(dir.path.c_str(), 0755) == 0);

    Daemon daemon(unique_name() + "-open-dir");
    CHECK(daemon.wait_for_exit(10s) == 1); // kExitSetupFailed
}

TEST_CASE("amctl drives the daemon", "[daemon][amctl]") {
    RuntimeDir dir;
    const std::string name = unique_name() + "-amctl";

    // No daemon yet: every command fails with exit code 1.
    CHECK(run_amctl({"--name", name, "status"}) == 1);
    CHECK(run_amctl({"--name", name, "pause"}) == 1);

    Daemon daemon(name);
    // Wait until it's listening; status works once the telemetry page exists,
    // which the daemon creates after its socket.
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (run_amctl({"--name", name, "status"}) != 0) {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(20ms);
    }

    auto shm = ShmMapping::open_read_only(name + "_telemetry_v2", kTelemetryPageSize);
    REQUIRE(shm.has_value());
    const auto& page = *reinterpret_cast<const TelemetryPage*>(shm->bytes().data());

    // The daemon updates the page before it acks a command, so the new state
    // is there the moment amctl returns, with no waiting for the next tick.
    CHECK(run_amctl({"--name", name, "pause"}) == 0);
    CHECK(page.read().playback_state == PlaybackState::Paused);
    CHECK(run_amctl({"--name", name, "play"}) == 0);
    CHECK(page.read().playback_state == PlaybackState::Playing);
    CHECK(run_amctl({"--name", name, "stop"}) == 0);
    CHECK(page.read().playback_state == PlaybackState::Stopped);
    CHECK(run_amctl({"--name", name, "volume", "30"}) == 0);
    CHECK(page.read().volume_percent == 30);

    // toggle plays when stopped or paused, and pauses when playing.
    CHECK(run_amctl({"--name", name, "toggle"}) == 0);
    CHECK(page.read().playback_state == PlaybackState::Playing);
    CHECK(run_amctl({"--name", name, "toggle"}) == 0);
    CHECK(page.read().playback_state == PlaybackState::Paused);

    CHECK(run_amctl({"--name", name, "ping"}) == 0);

    const auto json_file = dir.path / "status.json";
    CHECK(wait_for_amctl(start_amctl({"--name", name, "status", "--json"}, json_file)) == 0);
    const std::string json = read_file(json_file);
    CHECK(json.starts_with("{\"state\":\"paused\","));
    CHECK(json.find("\"volume_percent\":30,") != std::string::npos);

    // Bad arguments never reach the daemon.
    CHECK(run_amctl({"--name", name, "volume", "101"}) == 2);
    CHECK(run_amctl({"--name", name, "rewind"}) == 2);

    // watch prints the current state, then each change, and stops cleanly
    // when the daemon quits.
    const auto watch_file = dir.path / "watch.txt";
    const pid_t watcher = start_amctl({"--name", name, "watch", "--json"}, watch_file);
    REQUIRE(watcher > 0);
    const auto first_line = std::chrono::steady_clock::now() + 5s;
    while (read_file(watch_file).find('\n') == std::string::npos) {
        REQUIRE(std::chrono::steady_clock::now() < first_line);
        std::this_thread::sleep_for(10ms);
    }
    CHECK(run_amctl({"--name", name, "play"}) == 0);

    REQUIRE(::kill(daemon.pid, SIGTERM) == 0);
    CHECK(daemon.wait_for_exit(10s) == 0);
    CHECK(wait_for_amctl(watcher) == 0);
    const std::string watched = read_file(watch_file);
    CHECK(watched.starts_with("{\"state\":\"paused\","));
    CHECK(watched.find("{\"state\":\"playing\",") != std::string::npos);
}

TEST_CASE("amctl has a command for every request the daemon handles", "[daemon][amctl]") {
    using am::amctl::Coverage;
    RuntimeDir dir;
    const std::string name = unique_name() + "-coverage";
    Daemon daemon(name);

    auto reactor = Reactor::create();
    REQUIRE(reactor.has_value());
    Client client(std::move(*reactor));
    REQUIRE(client.connect(name));

    // Send each request header-only. Some need a body and get BadRequest,
    // which is fine: the point is whether the daemon knows the request at all.
    for (unsigned byte = 0; byte <= 0xFF; ++byte) {
        const auto opcode = static_cast<Opcode>(byte);
        if (!is_known_opcode(opcode)) {
            continue;
        }
        const Coverage c = am::amctl::coverage(opcode);
        if (c.kind == Coverage::Kind::FromDaemon) {
            continue;
        }
        INFO("opcode 0x" << std::hex << byte);
        const auto seq = client.send(opcode);
        if (opcode == Opcode::Ping) {
            CHECK(client.wait_for(Opcode::Pong).has_value());
            continue;
        }
        // Wait for whichever answer comes, Ack or Nak.
        std::optional<CommandResultBody> answer;
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!answer && std::chrono::steady_clock::now() < deadline) {
            auto r = client.channel->receive(client.buffer);
            REQUIRE(r.has_value());
            REQUIRE(r->status != IoStatus::PeerClosed);
            if (r->status == IoStatus::WouldBlock) {
                (void)client.reactor.run_once(10ms);
                continue;
            }
            const std::span<const std::byte> frame(client.buffer.data(), r->bytes);
            auto header = validate_frame(frame);
            REQUIRE(header.has_value());
            if (header->opcode == Opcode::CommandAck || header->opcode == Opcode::CommandNak) {
                auto body = decode_body<CommandResultBody>(frame.subspan(kFrameHeaderBytes));
                REQUIRE(body.has_value());
                answer = *body;
            }
        }
        REQUIRE(answer.has_value());
        CHECK(answer->request_seq_id == seq);
        if (c.kind == Coverage::Kind::NotYet) {
            // If this fails, the daemon has learned this request: give amctl a
            // command for it and change its line in coverage() to Command.
            CHECK(answer->status == CommandStatus::NotSupported);
        } else {
            CHECK(answer->status != CommandStatus::NotSupported);
        }
    }

    REQUIRE(::kill(daemon.pid, SIGTERM) == 0);
    CHECK(daemon.wait_for_exit(10s) == 0);
}
