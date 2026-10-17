// Linux implementation of am::pal::ShutdownSignal (src/pal/interfaces/shutdown_signal.hpp).
//
// A normal signal handler runs in the middle of whatever the thread was doing,
// and may only call a short list of "async-signal-safe" functions, which rules
// out nearly everything useful. signalfd avoids handlers altogether: block
// SIGINT and SIGTERM so the kernel holds them as pending instead of acting on
// them, and read them from a file descriptor that the reactor watches like any
// other. The callback then runs as an ordinary reactor callback.

#include "shutdown_signal.hpp"

#include "reactor.hpp"

#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <cerrno>
#include <utility>

namespace am::pal {

struct ShutdownSignal::Impl {
    Reactor* reactor = nullptr;
    int fd = -1;
    sigset_t previous_mask{}; // what was blocked before, restored on destroy
    Callback on_shutdown;
};

std::expected<ShutdownSignal, std::error_code> ShutdownSignal::watch(Reactor& reactor,
                                                                     Callback on_shutdown) {
    auto impl = std::make_unique<Impl>();
    impl->reactor = &reactor;
    impl->on_shutdown = std::move(on_shutdown);

    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);

    // pthread_sigmask returns the error instead of setting errno.
    if (const int error = ::pthread_sigmask(SIG_BLOCK, &signals, &impl->previous_mask);
        error != 0) {
        return std::unexpected(std::error_code(error, std::system_category()));
    }

    impl->fd = ::signalfd(-1, &signals, SFD_NONBLOCK | SFD_CLOEXEC);
    if (impl->fd < 0) {
        const int error = errno;
        ::pthread_sigmask(SIG_SETMASK, &impl->previous_mask, nullptr);
        return std::unexpected(std::error_code(error, std::system_category()));
    }

    // The Impl lives on the heap and never moves, so the callback can keep a
    // plain pointer to it. The destructor unwatches before freeing it.
    Impl* self = impl.get();
    auto watched = reactor.watch({self->fd}, {.readable = true}, [self](const IoEvent&) {
        // Read every pending signal so the fd stops being readable, then
        // tell the daemon once.
        signalfd_siginfo info;
        bool got_one = false;
        while (::read(self->fd, &info, sizeof(info)) == static_cast<ssize_t>(sizeof(info))) {
            got_one = true;
        }
        if (got_one && self->on_shutdown) {
            self->on_shutdown();
        }
    });
    if (!watched) {
        ::close(impl->fd);
        ::pthread_sigmask(SIG_SETMASK, &impl->previous_mask, nullptr);
        return std::unexpected(watched.error());
    }

    return ShutdownSignal(std::move(impl));
}

ShutdownSignal::ShutdownSignal(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

ShutdownSignal::~ShutdownSignal() {
    if (!impl_) {
        return; // moved from
    }
    (void)impl_->reactor->unwatch({impl_->fd});
    ::close(impl_->fd);
    // Any SIGINT or SIGTERM still pending is delivered as soon as this
    // unblocks it, with the default "kill the process" reaction. That's what
    // a second Ctrl-C during the last steps of shutdown should do anyway.
    ::pthread_sigmask(SIG_SETMASK, &impl_->previous_mask, nullptr);
}

ShutdownSignal::ShutdownSignal(ShutdownSignal&&) noexcept = default;

ShutdownSignal& ShutdownSignal::operator=(ShutdownSignal&& other) noexcept {
    if (this != &other) {
        ShutdownSignal old(std::move(*this)); // cleans up what this held, at the end of the block
        impl_ = std::move(other.impl_);
    }
    return *this;
}

} // namespace am::pal
