// launch_ui() on Linux (src/pal/interfaces/process_launch.hpp, issue #41).
//
// posix_spawn starts the window. Three things it would otherwise inherit from
// the daemon are reset on the way:
//
//   - The blocked signals. The daemon blocks SIGINT and SIGTERM so it can read
//     them from a signalfd (signalfd_shutdown.cpp), and a new process starts
//     with its parent's blocked set. Without the reset, the window would
//     ignore Ctrl-C and `kill`.
//   - The session. POSIX_SPAWN_SETSID gives the window a session (and process
//     group) of its own, so a Ctrl-C meant for a daemon running in a terminal
//     doesn't close the window too.
//   - Standard input, which reads /dev/null. The window never reads it, and a
//     process in the background that reads the terminal gets stopped.
//
// When a process exits, the kernel keeps a small entry for it (a "zombie",
// OSTEP chapter 5) until its parent collects the exit status with wait(). The
// daemon would only do that when it quits, so every closed window would sit
// in `ps` as "am-ui <defunct>" until then. A pidfd is a file descriptor for
// one process that becomes readable when the process exits, so the reactor
// can watch it like a socket and collect the window as soon as it's gone.

#include "process_launch.hpp"

#include "reactor.hpp"

#include <cerrno>
#include <cstdio>
#include <string>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace am::pal {
namespace {

// Closes the pidfd when the reactor drops the callback that owns it: after
// the window was collected, or when the reactor itself goes away first.
class PidFd {
  public:
    explicit PidFd(int fd) noexcept : fd_(fd) {}
    PidFd(PidFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    PidFd& operator=(PidFd&&) = delete;
    PidFd(const PidFd&) = delete;
    PidFd& operator=(const PidFd&) = delete;
    ~PidFd() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    [[nodiscard]] int get() const noexcept { return fd_; }

  private:
    int fd_;
};

// posix_spawn's two option objects, destroyed on every way out.
struct SpawnOptions {
    posix_spawn_file_actions_t actions{};
    posix_spawnattr_t attributes{};
    SpawnOptions() {
        ::posix_spawn_file_actions_init(&actions);
        ::posix_spawnattr_init(&attributes);
    }
    ~SpawnOptions() {
        ::posix_spawnattr_destroy(&attributes);
        ::posix_spawn_file_actions_destroy(&actions);
    }
    SpawnOptions(const SpawnOptions&) = delete;
    SpawnOptions& operator=(const SpawnOptions&) = delete;
};

std::error_code from_errno(int value) noexcept { return {value, std::system_category()}; }

} // namespace

std::expected<void, std::error_code> launch_ui(Reactor& reactor,
                                               const std::filesystem::path& program) {
    // posix_spawn would say so too, but only through an exit status we'd
    // collect later. Checking first gives the daemon a clear log line.
    if (::access(program.c_str(), X_OK) != 0) {
        return std::unexpected(from_errno(errno));
    }

    SpawnOptions options;
    sigset_t none;
    sigemptyset(&none);
    sigset_t all;
    sigfillset(&all);
    // Unblock every signal, and put every one back to its default action
    // (SETSIGDEF only touches the ones in `all` that can be changed).
    ::posix_spawnattr_setsigmask(&options.attributes, &none);
    ::posix_spawnattr_setsigdefault(&options.attributes, &all);
    ::posix_spawnattr_setflags(&options.attributes,
                               POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSID);
    ::posix_spawn_file_actions_addopen(&options.actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);

    std::string path = program.string();
    char* argv[] = {path.data(), nullptr};
    pid_t pid = -1;
    if (const int r =
            ::posix_spawn(&pid, path.c_str(), &options.actions, &options.attributes, argv, environ);
        r != 0) {
        return std::unexpected(from_errno(r));
    }

    // From here on the window is running, so this returns success whatever
    // happens with the pidfd. Without one (a kernel older than 5.3) the window
    // stays a zombie after it closes, until the daemon quits. Harmless.
    PidFd pidfd(static_cast<int>(::syscall(SYS_pidfd_open, pid, 0)));
    if (pidfd.get() < 0) {
        std::fprintf(stderr, "window: no pidfd (%s); it will show as <defunct> once closed\n",
                     from_errno(errno).message().c_str());
        return {};
    }
    const NativeHandle handle{pidfd.get()};
    auto watched = reactor.watch(handle, {.readable = true},
                                 [&reactor, handle, pid, fd = std::move(pidfd)](const IoEvent&) {
                                     // Readable means it exited, so this
                                     // doesn't wait.
                                     (void)::waitpid(pid, nullptr, WNOHANG);
                                     (void)reactor.unwatch(handle);
                                     // `fd` closes once the reactor lets go
                                     // of this callback.
                                 });
    if (!watched) {
        std::fprintf(stderr, "window: can't watch its pidfd (%s)\n",
                     watched.error().message().c_str());
    }
    return {};
}

} // namespace am::pal
