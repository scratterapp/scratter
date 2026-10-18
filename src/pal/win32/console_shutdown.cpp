// Windows implementation of am::pal::ShutdownSignal (src/pal/interfaces/shutdown_signal.hpp).
//
// SetConsoleCtrlHandler gives Windows a function to call on Ctrl-C, Ctrl-Break
// and when the console window is closed. Windows calls it on a new thread of its
// own, so the handler doesn't run on_shutdown itself. It sets a flag and calls
// Reactor::wake(), the one Reactor call that's safe from any thread, and the
// reactor's wake callback runs on_shutdown on the reactor thread.
//
// That takes the reactor's wake callback, and there's only one. Nothing else
// sets it yet. SMTC (#9) needs a way onto the reactor thread too, and then the
// two have to share it (issue #22).
//
// Closing the console window is the odd one out: Windows ends the process as
// soon as the handler returns, or after about 5 seconds if it doesn't. So for
// that one the handler waits until the ShutdownSignal is destroyed, which in
// the daemon is near the end of its cleanup.

#include "shutdown_signal.hpp"

#include "reactor.hpp"

#include <windows.h>

#include <atomic>
#include <expected>
#include <memory>
#include <system_error>
#include <utility>

namespace am::pal {

namespace {

std::unexpected<std::error_code> last_error() {
    return std::unexpected(
        std::error_code(static_cast<int>(::GetLastError()), std::system_category()));
}

// Windows calls the handler without a context pointer, and only one
// ShutdownSignal exists at a time, so what the handler needs is global.
// g_lock guards g_reactor: the handler runs on its own thread and mustn't wake
// a reactor that ~ShutdownSignal has already let go of.
SRWLOCK g_lock = SRWLOCK_INIT;
Reactor* g_reactor = nullptr; // null while no ShutdownSignal exists

// A quit event arrived and on_shutdown hasn't run for it yet.
std::atomic<bool> g_requested{false};

// Set by ~ShutdownSignal, for a handler holding off a console close. Created
// once and never closed, because a handler may still be waiting on it.
HANDLE g_released = nullptr;

class Locked {
  public:
    Locked() noexcept { ::AcquireSRWLockExclusive(&g_lock); }
    ~Locked() { ::ReleaseSRWLockExclusive(&g_lock); }
    Locked(const Locked&) = delete;
    Locked& operator=(const Locked&) = delete;
};

BOOL WINAPI on_console_event(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT && event != CTRL_CLOSE_EVENT) {
        // Logoff and shutdown only reach services, not console programs.
        return FALSE;
    }
    {
        Locked locked;
        if (g_reactor == nullptr) {
            // No ShutdownSignal any more: Windows' default, ending the process.
            return FALSE;
        }
        g_requested.store(true, std::memory_order_release);
        g_reactor->wake();
    }
    if (event == CTRL_CLOSE_EVENT) {
        // Windows ends the process once this returns, so hold it off until the
        // daemon has shut down. If that takes too long, Windows ends it anyway.
        ::WaitForSingleObject(g_released, INFINITE);
    }
    return TRUE;
}

} // namespace

struct ShutdownSignal::Impl {
    Reactor* reactor = nullptr;
    Callback on_shutdown;
};

std::expected<ShutdownSignal, std::error_code> ShutdownSignal::watch(Reactor& reactor,
                                                                     Callback on_shutdown) {
    auto impl = std::make_unique<Impl>();
    impl->reactor = &reactor;
    impl->on_shutdown = std::move(on_shutdown);

    {
        Locked locked;
        if (g_reactor != nullptr) {
            return std::unexpected(std::make_error_code(std::errc::device_or_resource_busy));
        }
        if (g_released == nullptr) {
            // Manual reset, so it stays set for every handler waiting on it.
            g_released = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (g_released == nullptr) {
                return last_error();
            }
        }
        ::ResetEvent(g_released);
        g_requested.store(false, std::memory_order_relaxed);
        g_reactor = &reactor;
    }

    // The Impl lives on the heap and never moves, so the callback can keep a
    // plain pointer to it. The destructor clears the callback before freeing it.
    Impl* self = impl.get();
    reactor.set_wake_callback([self] {
        // stop() wakes the reactor too, so only a quit event runs on_shutdown.
        if (g_requested.exchange(false, std::memory_order_acq_rel) && self->on_shutdown) {
            self->on_shutdown();
        }
    });

    if (::SetConsoleCtrlHandler(on_console_event, TRUE) == 0) {
        auto error = last_error();
        reactor.set_wake_callback(nullptr);
        Locked locked;
        g_reactor = nullptr;
        return error;
    }
    return ShutdownSignal(std::move(impl));
}

ShutdownSignal::ShutdownSignal(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

ShutdownSignal::~ShutdownSignal() {
    if (!impl_) {
        return; // moved from
    }
    {
        // From here on the handler returns FALSE, so a Ctrl-C ends the process
        // the default way. That's what a second Ctrl-C during the last steps of
        // shutdown should do anyway.
        Locked locked;
        g_reactor = nullptr;
    }
    // Lets a handler that's holding off a console close return. Done before
    // removing the handler, in case Windows waits for a running one first.
    ::SetEvent(g_released);
    ::SetConsoleCtrlHandler(on_console_event, FALSE);
    impl_->reactor->set_wake_callback(nullptr);
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
