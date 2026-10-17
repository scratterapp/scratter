// Windows implementation of am::pal::Reactor (src/pal/interfaces/reactor.hpp).
//
// The reactor is one I/O completion port. Two kinds of packet arrive on it:
//
//   - A finished overlapped operation (ReadFile, WriteFile, ...) on a handle
//     that was given to watch(). The handle is associated with the port under a
//     key that belongs to that one watch, and the packet carries the key. A key
//     is never reused, so a packet from an unwatched handle is recognised and
//     dropped even if Windows has since handed the same handle number to
//     something else.
//   - A wakeup, posted by wake() from any thread under a reserved key.
//
// post() never touches the port. Some operations finish without the OS queuing
// anything (ConnectNamedPipe returning ERROR_PIPE_CONNECTED, a ReadFile that fails
// at once), so the reactor keeps its own queue of events and delivers them after
// each batch of packets, through the same watch lookup, so a stale one is dropped
// the same way.
//
// The timer is not a packet. There is only one timer, so the reactor thread
// keeps its deadline and folds the time left into the timeout it gives
// GetQueuedCompletionStatusEx, then runs the callback itself once the wait ends
// and the deadline has passed (libuv does the same). A threadpool timer would
// fire on a pool thread and need a packet posted back to the port, and a packet
// already in flight when set_timer() or cancel_timer() runs would still have to
// be recognised as stale. Here the timer state is only ever touched on the
// reactor thread, so there is nothing to race with.
//
// A Windows wait can end later than asked, by up to the system timer tick
// (15.625 ms by default), and can also return a little early, so the loop checks
// steady_clock before it believes a deadline has passed. The tick itself is left
// alone: shortening it with timeBeginPeriod affects the whole system.

#include "reactor.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <optional>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace am::pal {

namespace {

using Clock = std::chrono::steady_clock;

// GetQueuedCompletionStatusEx takes a DWORD of milliseconds, where 0xFFFFFFFF
// means "forever". This is the longest finite wait.
constexpr std::chrono::milliseconds::rep kMaxWaitMs = 0xFFFF'FFFEll;

// Completion keys. 1 is the wakeup; each watch() gets the next number up.
constexpr ULONG_PTR kWakeKey = 1;
constexpr ULONG_PTR kFirstWatchKey = 2;

// How many packets one wait can take.
constexpr ULONG kBatchSize = 64;

// Removing a handle's association with its completion port is not in the
// documented Win32 API. NtSetInformationFile with this information class
// (FileReplaceCompletionInformation, Windows 8.1 and later) and a null port does
// it, but only when no operation started without an event is pending or waiting
// to be collected. See Impl::detach().
constexpr ULONG kFileReplaceCompletionInformation = 61;

struct IoStatusBlock {
    union {
        LONG status;
        void* pointer;
    };
    ULONG_PTR information;
};

struct FileCompletionInformation {
    HANDLE port;
    void* key;
};

using NtSetInformationFileFn = LONG(NTAPI*)(HANDLE, IoStatusBlock*, void*, ULONG, ULONG);

std::error_code os_error(DWORD err) noexcept {
    return {static_cast<int>(err), std::system_category()};
}

std::unexpected<std::error_code> last_error() {
    return std::unexpected(os_error(::GetLastError()));
}

std::unexpected<std::error_code> generic_error(std::errc e) {
    return std::unexpected(std::make_error_code(e));
}

HANDLE as_handle(std::intptr_t value) noexcept { return reinterpret_cast<HANDLE>(value); }

// Whole milliseconds from `now` until `target`, rounded up so the wait never ends
// before the target, and capped at the longest finite wait.
DWORD ms_until(Clock::time_point target, Clock::time_point now) noexcept {
    if (target <= now) {
        return 0;
    }
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(target - now);
    return static_cast<DWORD>(std::min(remaining.count(), kMaxWaitMs));
}

} // namespace

struct Reactor::Impl {
    HANDLE port = nullptr;

    std::atomic<bool> stop_requested{false};
    // True from the moment wake() posts a packet until the reactor thread takes
    // it, so wake() calls in between don't post more.
    std::atomic<bool> wake_pending{false};

    // The callback sits behind a shared_ptr so the loop can hold its own
    // reference while calling it. That way a callback can unwatch its own handle
    // (erasing the map's copy) without destroying the function that is still
    // running.
    struct Watch {
        std::intptr_t handle = 0;
        std::shared_ptr<IoCallback> callback;
    };
    std::unordered_map<std::intptr_t, ULONG_PTR> key_by_handle;
    std::unordered_map<ULONG_PTR, Watch> watch_by_key;
    // Never reused, so a key identifies one watch for the life of the reactor.
    ULONG_PTR next_key = kFirstWatchKey;

    Callback wake_callback;

    // Events from post(), waiting for the next pass. They name the watch by key,
    // so one for an unwatched handle is dropped when it comes up.
    struct Posted {
        ULONG_PTR key = 0;
        IoEvent event;
    };
    std::deque<Posted> posted;

    struct Timer {
        bool armed = false;
        Clock::time_point deadline{};
        Callback callback;
    };
    Timer timer;

    NtSetInformationFileFn nt_set_information_file = nullptr;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        if (port != nullptr) {
            ::CloseHandle(port);
        }
    }

    // Waits until a batch of packets arrives, the timer is due, or `deadline`
    // passes (never, if there is none), then runs what happened.
    std::expected<void, std::error_code>
    wait_and_dispatch(std::optional<Clock::time_point> deadline) {
        std::array<OVERLAPPED_ENTRY, kBatchSize> entries{};
        for (;;) {
            const Clock::time_point now = Clock::now();

            // The earliest thing there is to wake up for: the caller's deadline or the timer.
            std::optional<Clock::time_point> wake_at = deadline;
            if (timer.armed && (!wake_at || timer.deadline < *wake_at)) {
                wake_at = timer.deadline;
            }
            // Posted events are ready now, so don't wait for the OS to report anything.
            DWORD wait_ms = wake_at ? ms_until(*wake_at, now) : INFINITE;
            if (!posted.empty()) {
                wait_ms = 0;
            }

            ULONG count = 0;
            if (::GetQueuedCompletionStatusEx(port, entries.data(), kBatchSize, &count, wait_ms,
                                              FALSE) != 0) {
                for (ULONG i = 0; i < count; ++i) {
                    dispatch(entries[i]);
                }
                deliver_posted();
                fire_timer_if_due();
                return {};
            }

            const DWORD err = ::GetLastError();
            if (err != WAIT_TIMEOUT) {
                return std::unexpected(os_error(err));
            }

            // The wait ran out, but it can end a little early, so look at the clock
            // rather than trusting it.
            if (deliver_posted()) {
                fire_timer_if_due();
                return {};
            }
            if (fire_timer_if_due()) {
                return {};
            }
            if (deadline && Clock::now() >= *deadline) {
                return {};
            }
        }
    }

    void dispatch(const OVERLAPPED_ENTRY& entry) {
        if (entry.lpCompletionKey == kWakeKey) {
            dispatch_wake();
        } else {
            dispatch_watch(entry);
        }
    }

    void dispatch_wake() {
        // Clear the flag before running the callback, so a wake() made while it
        // runs posts a new packet instead of being lost.
        wake_pending.exchange(false, std::memory_order_acq_rel);
        if (wake_callback) {
            wake_callback();
        }
    }

    void dispatch_watch(const OVERLAPPED_ENTRY& entry) {
        // An earlier callback in this batch, or an earlier call to unwatch(), may
        // have removed this watch. Its keys are never reused, so a miss means the
        // packet is stale. Drop it without touching entry.lpOverlapped: whoever
        // owned that OVERLAPPED is free to have released it.
        const auto it = watch_by_key.find(entry.lpCompletionKey);
        if (it == watch_by_key.end()) {
            return;
        }
        const std::shared_ptr<IoCallback> callback = it->second.callback;

        IoEvent event;
        event.overlapped = entry.lpOverlapped;
        event.bytes = static_cast<std::uint32_t>(entry.dwNumberOfBytesTransferred);
        if (entry.lpOverlapped != nullptr) {
            DWORD transferred = 0;
            if (::GetOverlappedResult(as_handle(it->second.handle), entry.lpOverlapped,
                                      &transferred, FALSE) == 0) {
                event.error = ::GetLastError();
            }
        }
        if (*callback) {
            (*callback)(event);
        }
    }

    // Delivers the events post() queued before this call. One that a callback
    // posts meanwhile waits for the next pass, so a callback that keeps posting
    // can't keep the loop from ever returning. Returns whether there were any.
    bool deliver_posted() {
        if (posted.empty()) {
            return false;
        }
        std::deque<Posted> batch;
        batch.swap(posted);
        for (const Posted& entry : batch) {
            const auto it = watch_by_key.find(entry.key);
            if (it == watch_by_key.end()) {
                continue; // unwatched since it was posted
            }
            const std::shared_ptr<IoCallback> callback = it->second.callback;
            if (*callback) {
                (*callback)(entry.event);
            }
        }
        return true;
    }

    // Runs the timer's callback if it is armed and due. Returns whether it ran.
    bool fire_timer_if_due() {
        if (!timer.armed || Clock::now() < timer.deadline) {
            return false;
        }
        // Move the callback out before calling it, so it can set a new timer
        // (replacing timer.callback) without destroying itself mid-call.
        Callback callback = std::move(timer.callback);
        timer.callback = nullptr;
        timer.armed = false;
        if (callback) {
            callback();
        }
        return true;
    }

    // Tries to remove the handle's association with the completion port, so the
    // handle can be watched again.
    //
    // This is best effort. The kernel refuses (STATUS_UNSUCCESSFUL) while an
    // operation that was started without an event is still pending, or has
    // finished but its packet hasn't been collected yet. That is fine: the watch
    // is already gone, any packet still to come carries a key nobody owns and is
    // dropped, and the handle simply stays associated until it is closed. To watch
    // a handle again, cancel its I/O and let the completions arrive before
    // unwatching it.
    void detach(std::intptr_t handle) const noexcept {
        if (nt_set_information_file == nullptr) {
            return;
        }
        FileCompletionInformation info{nullptr, nullptr};
        IoStatusBlock status_block{};
        [[maybe_unused]] const LONG status = nt_set_information_file(
            as_handle(handle), &status_block, &info, static_cast<ULONG>(sizeof(info)),
            kFileReplaceCompletionInformation);
    }
};

std::expected<Reactor, std::error_code> Reactor::create() {
    auto impl = std::make_unique<Impl>();

    // One concurrent thread: only the reactor thread waits on the port.
    impl->port = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    if (impl->port == nullptr) {
        return last_error();
    }

    if (const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll"); ntdll != nullptr) {
        impl->nt_set_information_file = reinterpret_cast<NtSetInformationFileFn>(
            ::GetProcAddress(ntdll, "NtSetInformationFile"));
    }

    return Reactor(std::move(impl));
}

Reactor::Reactor(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Reactor::~Reactor() = default;
Reactor::Reactor(Reactor&&) noexcept = default;
Reactor& Reactor::operator=(Reactor&&) noexcept = default;

std::expected<void, std::error_code> Reactor::run_once(std::chrono::milliseconds timeout) {
    // A negative timeout means don't wait, and a huge one means the longest wait there is.
    const auto ms = std::clamp<std::chrono::milliseconds::rep>(timeout.count(), 0, kMaxWaitMs);
    return impl_->wait_and_dispatch(Clock::now() + std::chrono::milliseconds{ms});
}

std::expected<void, std::error_code> Reactor::run() {
    // exchange() both checks and clears the flag, so run() can be called again
    // after it returns.
    while (!impl_->stop_requested.exchange(false, std::memory_order_acquire)) {
        if (auto result = impl_->wait_and_dispatch(std::nullopt); !result) {
            return result;
        }
    }
    return {};
}

void Reactor::stop() noexcept {
    impl_->stop_requested.store(true, std::memory_order_release);
    wake();
}

std::expected<void, std::error_code> Reactor::watch(NativeHandle handle, Interest,
                                                    IoCallback callback) {
    const std::intptr_t value = handle.value;
    // -1 is INVALID_HANDLE_VALUE, and 0 is no handle at all.
    if (value == -1 || value == 0) {
        return generic_error(std::errc::invalid_argument);
    }
    if (impl_->key_by_handle.contains(value)) {
        return generic_error(std::errc::file_exists);
    }

    // Fails with ERROR_INVALID_PARAMETER if the handle isn't overlapped or already
    // belongs to a completion port.
    const ULONG_PTR key = impl_->next_key++;
    if (::CreateIoCompletionPort(as_handle(value), impl_->port, key, 0) == nullptr) {
        return last_error();
    }

    impl_->key_by_handle.emplace(value, key);
    impl_->watch_by_key.emplace(
        key, Impl::Watch{value, std::make_shared<IoCallback>(std::move(callback))});
    return {};
}

std::expected<void, std::error_code> Reactor::modify(NativeHandle handle, Interest) {
    // IOCP reports whichever operations the caller started, so there is nothing to change.
    if (!impl_->key_by_handle.contains(handle.value)) {
        return generic_error(std::errc::no_such_file_or_directory);
    }
    return {};
}

std::expected<void, std::error_code> Reactor::unwatch(NativeHandle handle) {
    const auto it = impl_->key_by_handle.find(handle.value);
    if (it == impl_->key_by_handle.end()) {
        return generic_error(std::errc::no_such_file_or_directory);
    }
    // Erase both entries first: from here on, any packet carrying this key is stale.
    impl_->watch_by_key.erase(it->second);
    impl_->key_by_handle.erase(it);
    impl_->detach(handle.value);
    return {};
}

std::expected<void, std::error_code> Reactor::post(NativeHandle handle, const IoEvent& event) {
    const auto it = impl_->key_by_handle.find(handle.value);
    if (it == impl_->key_by_handle.end()) {
        return generic_error(std::errc::no_such_file_or_directory);
    }
    impl_->posted.push_back(Impl::Posted{it->second, event});
    return {};
}

void Reactor::set_wake_callback(Callback callback) { impl_->wake_callback = std::move(callback); }

void Reactor::wake() noexcept {
    // Several wake() calls before the reactor thread looks share one packet.
    if (impl_->wake_pending.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (::PostQueuedCompletionStatus(impl_->port, 0, kWakeKey, nullptr) == 0) {
        // The packet wasn't queued; let the next wake() try again.
        impl_->wake_pending.store(false, std::memory_order_release);
    }
}

std::expected<void, std::error_code> Reactor::set_timer(std::chrono::milliseconds delay,
                                                        Callback callback) {
    const Clock::time_point now = Clock::now();
    const std::chrono::milliseconds wait = std::max(delay, std::chrono::milliseconds::zero());
    // A delay too long to add to the clock means "never in practice".
    const auto room =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - now);

    impl_->timer.deadline = wait >= room ? Clock::time_point::max() : now + wait;
    impl_->timer.callback = std::move(callback);
    impl_->timer.armed = true;
    return {};
}

void Reactor::cancel_timer() noexcept {
    impl_->timer.armed = false;
    impl_->timer.callback = nullptr;
}

} // namespace am::pal