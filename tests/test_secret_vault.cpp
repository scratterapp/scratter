// Tests for what the Linux keyring vault (src/pal/linux/secret_vault.cpp) does
// when there's no keyring, or the keyring answers with an error.
//
// test_credential_vault.cpp checks the vault against a real keyring, but a real
// keyring can't safely be made to fail: locking the login keyring from a test
// would lock it for the person at the desktop too. So each test here starts its
// own private D-Bus bus (a dbus-daemon that can't start any services), points
// this process at it, and plays a broken keyring on it with sd-bus: every call
// gets the same error back.
//
// GLib connects to the session bus once and keeps that connection for the life
// of the process, so only the first of these tests in a process gets the bus it
// asked for. ctest runs each test case in a process of its own, which is how
// they're meant to run. Running the program by hand, the second and later ones
// print AM_TEST_SKIPPED and pass; give it one test's name to run that one.
//
// They also need dbus-daemon (the `dbus` package). Without it they skip the
// same way.

#include "credential_vault.hpp"

#include <catch2/catch_test_macros.hpp>

#include <libsecret/secret.h>
#include <systemd/sd-bus.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

using am::pal::CredentialVault;
using am::pal::kMusicUserTokenSecret;
using namespace std::chrono_literals;

namespace {

namespace fs = std::filesystem;

// Sets an environment variable for as long as it lives, then puts the old
// value back.
class ScopedEnv {
  public:
    ScopedEnv(const char* name, const std::string& value) : name_(name) {
        if (const char* old = std::getenv(name)) {
            old_ = old;
        }
        setenv(name, value.c_str(), 1);
    }
    ~ScopedEnv() {
        if (old_) {
            setenv(name_, old_->c_str(), 1);
        } else {
            unsetenv(name_);
        }
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

  private:
    const char* name_;
    std::optional<std::string> old_;
};

// A folder of its own under the system temp folder, deleted at the end.
class TempDir {
  public:
    explicit TempDir(std::string_view test)
        : path_(fs::temp_directory_path() /
                ("am_vault." + std::string(test) + "." + std::to_string(getpid()))) {
        std::error_code error;
        fs::remove_all(path_, error);
        fs::create_directories(path_, error);
    }
    ~TempDir() {
        std::error_code error;
        fs::remove_all(path_, error);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const fs::path& path() const { return path_; }

  private:
    fs::path path_;
};

// False, after saying so, if an earlier test in this process already picked
// the session bus (see the top of the file).
bool first_bus_test() {
    static bool used = false;
    if (used) {
        WARN("AM_TEST_SKIPPED: an earlier test already connected this process to a bus; "
             "run this one on its own (ctest does)");
        return false;
    }
    used = true;
    return true;
}

// A private session bus with nothing on it, which this process uses as its
// session bus while the object lives. It has no service files, so nothing
// starts when a client asks for org.freedesktop.secrets.
class PrivateBus {
  public:
    explicit PrivateBus(std::string_view test) : dir_(test) {
        const fs::path socket = dir_.path() / "bus";
        address_ = "unix:path=" + socket.string();
        const fs::path config = dir_.path() / "bus.conf";
        {
            std::ofstream out(config);
            out << "<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-Bus Bus Configuration "
                   "1.0//EN\" \"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd\">\n"
                   "<busconfig>\n"
                   "  <type>session</type>\n"
                   "  <listen>"
                << address_
                << "</listen>\n"
                   "  <auth>EXTERNAL</auth>\n"
                   "  <policy context=\"default\">\n"
                   "    <allow send_destination=\"*\" eavesdrop=\"true\"/>\n"
                   "    <allow eavesdrop=\"true\"/>\n"
                   "    <allow own=\"*\"/>\n"
                   "  </policy>\n"
                   "</busconfig>\n";
        }

        const std::string config_arg = "--config-file=" + config.string();
        char program[] = "dbus-daemon";
        char nofork[] = "--nofork";
        char* argv[] = {program, const_cast<char*>(config_arg.c_str()), nofork, nullptr};
        if (posix_spawnp(&pid_, program, nullptr, nullptr, argv, environ) != 0) {
            pid_ = -1;
            return;
        }
        // It's ready once its socket shows up.
        for (int i = 0; i < 500 && !fs::exists(socket); ++i) {
            std::this_thread::sleep_for(10ms);
        }
        if (!fs::exists(socket)) {
            stop_daemon();
            return;
        }
        env_.emplace("DBUS_SESSION_BUS_ADDRESS", address_);
    }

    ~PrivateBus() {
        // Let go of the bus before it goes away. GLib ends the process when
        // the session bus closes a connection it still uses.
        secret_service_disconnect();
        env_.reset();
        stop_daemon();
    }
    PrivateBus(const PrivateBus&) = delete;
    PrivateBus& operator=(const PrivateBus&) = delete;

    [[nodiscard]] bool running() const { return pid_ > 0; }
    [[nodiscard]] const std::string& address() const { return address_; }

  private:
    void stop_daemon() {
        if (pid_ > 0) {
            kill(pid_, SIGTERM);
            int status = 0;
            waitpid(pid_, &status, 0);
            pid_ = -1;
        }
    }

    TempDir dir_;
    std::string address_;
    pid_t pid_ = -1;
    std::optional<ScopedEnv> env_;
};

// Plays a keyring on `bus` that answers every call with the D-Bus error
// `error_name`, on its own thread.
class BrokenKeyring {
  public:
    BrokenKeyring(const PrivateBus& bus, std::string error_name)
        : error_name_(std::move(error_name)) {
        if (sd_bus_new(&bus_) < 0 || sd_bus_set_address(bus_, bus.address().c_str()) < 0 ||
            sd_bus_set_bus_client(bus_, 1) < 0 || sd_bus_start(bus_) < 0 ||
            sd_bus_add_filter(bus_, nullptr, &BrokenKeyring::answer, this) < 0 ||
            sd_bus_request_name(bus_, "org.freedesktop.secrets", 0) < 0) {
            bus_ = sd_bus_flush_close_unref(bus_);
            return;
        }
        thread_ = std::thread([this] {
            while (!stop_.load()) {
                if (sd_bus_process(bus_, nullptr) > 0) {
                    continue;
                }
                sd_bus_wait(bus_, 50'000); // microseconds, so stop_ is seen soon
            }
        });
    }

    ~BrokenKeyring() {
        stop_.store(true);
        if (thread_.joinable()) {
            thread_.join();
        }
        sd_bus_flush_close_unref(bus_);
    }
    BrokenKeyring(const BrokenKeyring&) = delete;
    BrokenKeyring& operator=(const BrokenKeyring&) = delete;

    [[nodiscard]] bool running() const { return bus_ != nullptr; }

  private:
    static int answer(sd_bus_message* m, void* data, sd_bus_error* /*error*/) {
        auto* self = static_cast<BrokenKeyring*>(data);
        if (!sd_bus_message_is_method_call(m, nullptr, nullptr)) {
            return 0; // signals from the bus itself, such as NameAcquired
        }
        sd_bus_reply_method_errorf(m, self->error_name_.c_str(), "the test's broken keyring");
        return 1;
    }

    std::string error_name_;
    sd_bus* bus_ = nullptr;
    std::thread thread_;
    std::atomic<bool> stop_{false};
};

// Whether the private bus started, or says why this test can't run here.
bool bus_or_skip(const PrivateBus& bus) {
    if (!bus.running()) {
        WARN("AM_TEST_SKIPPED: couldn't start dbus-daemon for a private bus");
        return false;
    }
    return true;
}

// Opens a vault against a keyring that answers every call with `error_name`,
// and checks that store, load and remove all report `expected`.
void check_every_call_fails_with(std::string_view test, const char* error_name,
                                 std::errc expected) {
    if (!first_bus_test()) {
        return;
    }
    const PrivateBus bus(test);
    if (!bus_or_skip(bus)) {
        return;
    }
    const BrokenKeyring keyring(bus, error_name);
    REQUIRE(keyring.running());

    auto vault = CredentialVault::open("am-vault-test");
    REQUIRE(vault.has_value()); // something owns the name, so open() is happy

    auto stored = vault->store(kMusicUserTokenSecret, "token");
    REQUIRE_FALSE(stored.has_value());
    CHECK(stored.error() == expected);

    auto loaded = vault->load(kMusicUserTokenSecret);
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error() == expected);

    auto removed = vault->remove(kMusicUserTokenSecret);
    REQUIRE_FALSE(removed.has_value());
    CHECK(removed.error() == expected);
}

} // namespace

TEST_CASE("an empty or cut-short service name is rejected", "[vault]") {
    using namespace std::string_view_literals;
    auto empty = CredentialVault::open("");
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error() == std::errc::invalid_argument);

    auto cut_short = CredentialVault::open("am\0vault"sv);
    REQUIRE_FALSE(cut_short.has_value());
    CHECK(cut_short.error() == std::errc::invalid_argument);
}

TEST_CASE("there's no vault without a session bus", "[vault]") {
    if (!first_bus_test()) {
        return;
    }
    const TempDir dir("no-bus");
    const ScopedEnv env("DBUS_SESSION_BUS_ADDRESS", "unix:path=" + (dir.path() / "none").string());

    auto vault = CredentialVault::open("am-vault-test");
    REQUIRE_FALSE(vault.has_value());
    CHECK(vault.error() == std::errc::function_not_supported);
}

TEST_CASE("there's no vault when nothing on the bus keeps secrets", "[vault]") {
    if (!first_bus_test()) {
        return;
    }
    const PrivateBus bus("no-keyring");
    if (!bus_or_skip(bus)) {
        return;
    }
    auto vault = CredentialVault::open("am-vault-test");
    REQUIRE_FALSE(vault.has_value());
    CHECK(vault.error() == std::errc::function_not_supported);
}

TEST_CASE("a locked keyring is permission denied", "[vault]") {
    check_every_call_fails_with("locked", "org.freedesktop.Secret.Error.IsLocked",
                                std::errc::permission_denied);
}

TEST_CASE("a keyring that went away is reported as no vault", "[vault]") {
    check_every_call_fails_with("gone", "org.freedesktop.DBus.Error.ServiceUnknown",
                                std::errc::function_not_supported);
}

TEST_CASE("a keyring that left the bus is reported as no vault", "[vault]") {
    check_every_call_fails_with("left", "org.freedesktop.DBus.Error.NameHasNoOwner",
                                std::errc::function_not_supported);
}

TEST_CASE("a cancelled call is reported as cancelled", "[vault]") {
    // GDBus sends a GError with no D-Bus name of its own under a name built
    // from its domain and code, and turns that name back into the same GError
    // on arrival. Code 19 is G_IO_ERROR_CANCELLED.
    static_assert(G_IO_ERROR_CANCELLED == 19);
    check_every_call_fails_with("cancelled",
                                "org.gtk.GDBus.UnmappedGError.Quark._g_2dio_2derror_2dquark.Code19",
                                std::errc::operation_canceled);
}

TEST_CASE("any other keyring error is an I/O error", "[vault]") {
    check_every_call_fails_with("other", "org.example.Broken", std::errc::io_error);
}
