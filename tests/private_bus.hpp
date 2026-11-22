#pragma once

// A private D-Bus session bus for one test: a dbus-daemon of its own, with no
// services it can start, that this process uses as its session bus while the
// PrivateBus lives. test_secret_vault.cpp plays a broken keyring on it, and
// test_mpris_controls.cpp takes it away under the media controls.
//
// Needs dbus-daemon (the `dbus` package). running() is false without it.

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

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h> // environ

namespace am::test {

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
                ("am_bus." + std::string(test) + "." + std::to_string(getpid()))) {
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

// A private session bus with nothing on it, which this process uses as its
// session bus while the object lives. It has no service files, so nothing
// starts when a client asks for a name nobody has, such as
// org.freedesktop.secrets.
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
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!fs::exists(socket)) {
            stop_daemon();
            return;
        }
        env_.emplace("DBUS_SESSION_BUS_ADDRESS", address_);
    }

    ~PrivateBus() { stop(); }
    PrivateBus(const PrivateBus&) = delete;
    PrivateBus& operator=(const PrivateBus&) = delete;

    [[nodiscard]] bool running() const { return pid_ > 0; }
    [[nodiscard]] const std::string& address() const { return address_; }

    // Ends the bus now, which closes every connection to it, and points
    // DBUS_SESSION_BUS_ADDRESS back where it was.
    void stop() {
        env_.reset();
        stop_daemon();
    }

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

} // namespace am::test
