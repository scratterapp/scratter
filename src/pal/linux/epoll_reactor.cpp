// Linux implementation of am::pal::Reactor (src/pal/interfaces/reactor.hpp).
//
// One epoll instance watches three kinds of file descriptor:
//
//   - wake_fd, an eventfd. wake() writes to it from any thread, which makes it
//     readable and wakes epoll_wait().
//   - timer_fd, a timerfd on CLOCK_MONOTONIC. It becomes readable when the
//     timer expires.
//   - whatever fds the IPC code passes to watch().
//
// Everything is level-triggered (no EPOLLET): epoll keeps reporting an fd for
// as long as it's readable, so a callback that doesn't drain it all gets called
// again on the next loop instead of being forgotten.
//
// post() doesn't involve epoll. Its events wait in a queue that the loop drains
// after each epoll_wait(), and epoll_wait() gets a zero timeout while the queue
// isn't empty.

#include "reactor.hpp"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <ctime>
#include <deque>
#include <initializer_list>
#include <memory>
#include <unordered_map>
#include <utility>

namespace am::pal {

namespace {

std::unexpected<std::error_code> last_error() {
    return std::unexpected(std::error_code(errno, std::system_category()));
}

std::uint32_t to_epoll_events(Interest interest) {
    std::uint32_t events = 0;
    if (interest.readable) {
        events |= EPOLLIN;
    }
    if (interest.writable) {
        events |= EPOLLOUT;
    }
    // EPOLLHUP and EPOLLERR are always reported, whether asked for or not.
    return events;
}

IoEvent to_io_event(std::uint32_t events) {
    IoEvent event;
    event.readable = (events & EPOLLIN) != 0;
    event.writable = (events & EPOLLOUT) != 0;
    event.hangup = (events & (EPOLLHUP | EPOLLERR)) != 0;
    return event;
}

// What we store in epoll_event.data for each fd: the fd in the low 32 bits and
// a "generation" number in the high 32. Every watch() gets a new generation.
// If a callback closes fd 7 and a later callback in the same batch opens a new
// fd that the kernel also numbers 7, the batch still holds an event for the
// old fd 7. Its generation won't match the new watch, so it's dropped instead
// of being delivered to the wrong watcher.
std::uint64_t make_tag(int fd, std::uint32_t generation) {
    return (std::uint64_t{generation} << 32) | static_cast<std::uint32_t>(fd);
}
int tag_fd(std::uint64_t tag) { return static_cast<int>(tag & 0xFFFF'FFFFu); }
std::uint32_t tag_generation(std::uint64_t tag) { return static_cast<std::uint32_t>(tag >> 32); }

void close_if_open(int fd) {
    if (fd >= 0) {
        ::close(fd);
    }
}

} // namespace

struct Reactor::Impl {
    int epoll_fd = -1;
    int wake_fd = -1;
    int timer_fd = -1;

    std::atomic<bool> stop_requested{false};

    // The callback sits behind a shared_ptr so the loop can hold its own
    // reference while calling it. That way a callback can unwatch its own fd
    // (destroying the map's copy) without destroying the function that is
    // still running.
    struct Watch {
        std::uint32_t generation = 0;
        std::shared_ptr<IoCallback> callback;
    };
    std::unordered_map<int, Watch> watches;
    // Generation 0 is the wake and timer fds; watches start at 1.
    std::uint32_t next_generation = 1;

    Callback wake_callback;
    Callback timer_callback;

    // Events from post(), waiting for the next dispatch. Like an epoll event they
    // name the watch by fd and generation, so one for an unwatched fd is dropped.
    struct Posted {
        int fd;
        std::uint32_t generation;
        IoEvent event;
    };
    std::deque<Posted> posted;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        close_if_open(timer_fd);
        close_if_open(wake_fd);
        close_if_open(epoll_fd);
    }

    // One epoll_wait() and the callbacks for whatever it returned.
    std::expected<void, std::error_code> wait_and_dispatch(int timeout_ms) {
        std::array<epoll_event, 64> events{};
        // Posted events are ready now, so don't wait for the OS to report anything.
        const int wait_ms = posted.empty() ? timeout_ms : 0;
        const int count =
            ::epoll_wait(epoll_fd, events.data(), static_cast<int>(events.size()), wait_ms);
        if (count < 0) {
            // A signal interrupted the wait. Nothing happened, so it's not an error.
            if (errno == EINTR) {
                return {};
            }
            return last_error();
        }

        for (int i = 0; i < count; ++i) {
            const epoll_event& event = events[static_cast<std::size_t>(i)];
            const int fd = tag_fd(event.data.u64);

            if (fd == wake_fd) {
                dispatch_wake();
            } else if (fd == timer_fd) {
                dispatch_timer();
            } else {
                dispatch_watch(fd, tag_generation(event.data.u64), event.events);
            }
        }
        deliver_posted();
        return {};
    }

    void dispatch_wake() {
        // Reading an eventfd returns the sum of every write since the last read
        // and resets it to zero, so ten wake() calls become one callback.
        std::uint64_t pending = 0;
        [[maybe_unused]] const ssize_t n = ::read(wake_fd, &pending, sizeof(pending));
        if (wake_callback) {
            wake_callback();
        }
    }

    void dispatch_timer() {
        // The read fails with EAGAIN if an earlier callback in this same batch
        // re-armed or cancelled the timer after it expired. Then this expiry is
        // stale and must not run anything.
        std::uint64_t expirations = 0;
        if (::read(timer_fd, &expirations, sizeof(expirations)) < 0) {
            return;
        }
        // Move the callback out before calling it, so it can set a new timer
        // (replacing timer_callback) without destroying itself mid-call.
        Callback callback = std::move(timer_callback);
        timer_callback = nullptr;
        if (callback) {
            callback();
        }
    }

    void dispatch_watch(int fd, std::uint32_t generation, std::uint32_t epoll_events) {
        deliver(fd, generation, to_io_event(epoll_events));
    }

    // Runs the callback of the watch that `fd` and `generation` name, if it still
    // exists. An earlier callback in this batch may have unwatched this fd, or
    // unwatched it and then watched a new fd that got the same number; the
    // generation tells the two apart.
    void deliver(int fd, std::uint32_t generation, const IoEvent& event) {
        const auto it = watches.find(fd);
        if (it == watches.end() || it->second.generation != generation) {
            return;
        }
        const std::shared_ptr<IoCallback> callback = it->second.callback;
        (*callback)(event);
    }

    // Delivers the events post() queued before this call. One that a callback
    // posts meanwhile waits for the next dispatch, so a callback that keeps
    // posting can't keep the loop from ever returning.
    void deliver_posted() {
        if (posted.empty()) {
            return;
        }
        std::deque<Posted> batch;
        batch.swap(posted);
        for (const Posted& entry : batch) {
            deliver(entry.fd, entry.generation, entry.event);
        }
    }
};

std::expected<Reactor, std::error_code> Reactor::create() {
    auto impl = std::make_unique<Impl>();

    impl->epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
    if (impl->epoll_fd < 0) {
        return last_error();
    }

    impl->wake_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (impl->wake_fd < 0) {
        return last_error();
    }

    impl->timer_fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (impl->timer_fd < 0) {
        return last_error();
    }

    for (const int fd : {impl->wake_fd, impl->timer_fd}) {
        epoll_event event{};
        event.events = EPOLLIN;
        event.data.u64 = make_tag(fd, 0);
        if (::epoll_ctl(impl->epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0) {
            return last_error();
        }
    }

    return Reactor(std::move(impl));
}

Reactor::Reactor(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Reactor::~Reactor() = default;
Reactor::Reactor(Reactor&&) noexcept = default;
Reactor& Reactor::operator=(Reactor&&) noexcept = default;

std::expected<void, std::error_code> Reactor::run_once(std::chrono::milliseconds timeout) {
    // epoll_wait takes an int of milliseconds, where -1 means forever.
    const auto ms = std::clamp<std::chrono::milliseconds::rep>(timeout.count(), 0, INT_MAX);
    return impl_->wait_and_dispatch(static_cast<int>(ms));
}

std::expected<void, std::error_code> Reactor::run() {
    // exchange() both checks and clears the flag, so run() can be called again
    // after it returns.
    while (!impl_->stop_requested.exchange(false, std::memory_order_acquire)) {
        if (auto result = impl_->wait_and_dispatch(-1); !result) {
            return result;
        }
    }
    return {};
}

void Reactor::stop() noexcept {
    impl_->stop_requested.store(true, std::memory_order_release);
    wake();
}

std::expected<void, std::error_code> Reactor::watch(NativeHandle handle, Interest interest,
                                                    IoCallback callback) {
    const int fd = static_cast<int>(handle.value);
    const std::uint32_t generation = impl_->next_generation++;
    if (impl_->next_generation == 0) {
        impl_->next_generation = 1; // after 4 billion watches, skip 0 on wraparound
    }
    epoll_event event{};
    event.events = to_epoll_events(interest);
    event.data.u64 = make_tag(fd, generation);
    // Fails with EEXIST if the fd is already watched.
    if (::epoll_ctl(impl_->epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0) {
        return last_error();
    }
    impl_->watches.insert_or_assign(
        fd, Impl::Watch{generation, std::make_shared<IoCallback>(std::move(callback))});
    return {};
}

std::expected<void, std::error_code> Reactor::modify(NativeHandle handle, Interest interest) {
    const int fd = static_cast<int>(handle.value);
    const auto it = impl_->watches.find(fd);
    if (it == impl_->watches.end()) {
        return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
    }
    epoll_event event{};
    event.events = to_epoll_events(interest);
    event.data.u64 = make_tag(fd, it->second.generation);
    if (::epoll_ctl(impl_->epoll_fd, EPOLL_CTL_MOD, fd, &event) < 0) {
        return last_error();
    }
    return {};
}

std::expected<void, std::error_code> Reactor::unwatch(NativeHandle handle) {
    const int fd = static_cast<int>(handle.value);
    const auto it = impl_->watches.find(fd);
    if (it == impl_->watches.end()) {
        return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
    }
    impl_->watches.erase(it);
    if (::epoll_ctl(impl_->epoll_fd, EPOLL_CTL_DEL, fd, nullptr) < 0) {
        return last_error();
    }
    return {};
}

std::expected<void, std::error_code> Reactor::post(NativeHandle handle, const IoEvent& event) {
    const int fd = static_cast<int>(handle.value);
    const auto it = impl_->watches.find(fd);
    if (it == impl_->watches.end()) {
        return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
    }
    impl_->posted.push_back(Impl::Posted{fd, it->second.generation, event});
    return {};
}

void Reactor::set_wake_callback(Callback callback) { impl_->wake_callback = std::move(callback); }

void Reactor::wake() noexcept {
    // Adds 1 to the eventfd's counter. The only failure is EAGAIN when the
    // counter is nearly 2^64, which means a wakeup is already pending anyway.
    const std::uint64_t one = 1;
    [[maybe_unused]] const ssize_t n = ::write(impl_->wake_fd, &one, sizeof(one));
}

std::expected<void, std::error_code> Reactor::set_timer(std::chrono::milliseconds delay,
                                                        Callback callback) {
    using namespace std::chrono;

    itimerspec spec{}; // it_interval stays zero: fire once, don't repeat
    if (delay > milliseconds::zero()) {
        const auto whole_seconds = duration_cast<seconds>(delay);
        spec.it_value.tv_sec = static_cast<std::time_t>(whole_seconds.count());
        spec.it_value.tv_nsec =
            static_cast<long>(duration_cast<nanoseconds>(delay - whole_seconds).count());
    } else {
        // An all-zero it_value disarms a timerfd instead of firing it, so
        // "now" becomes the shortest delay there is.
        spec.it_value.tv_nsec = 1;
    }

    if (::timerfd_settime(impl_->timer_fd, 0, &spec, nullptr) < 0) {
        return last_error();
    }
    impl_->timer_callback = std::move(callback);
    return {};
}

void Reactor::cancel_timer() noexcept {
    const itimerspec disarm{};
    ::timerfd_settime(impl_->timer_fd, 0, &disarm, nullptr);
    impl_->timer_callback = nullptr;
}

} // namespace am::pal
