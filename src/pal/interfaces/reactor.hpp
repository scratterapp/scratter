#pragma once

// The event reactor: one thread that waits on many things at once and runs a
// callback for whichever one happens (spec section 4, "Event Reactor").
//
// It waits on three kinds of things:
//
//   1. Watched handles: an IPC socket or pipe that has something to report.
//   2. A wakeup: another thread calls wake() to get the reactor thread's
//      attention, for example after pushing work onto a queue.
//   3. One timer: "call me back in N milliseconds".
//
//   auto reactor = am::pal::Reactor::create();   // std::expected<Reactor, ...>
//   reactor->set_wake_callback([&] { drain_queue(); });
//   reactor->set_timer(std::chrono::milliseconds{250}, [&] { tick(); });
//   reactor->run();                              // until stop()
//
// Readiness vs completion
// -----------------------
// Linux epoll and Windows IOCP report I/O in opposite ways:
//
//   - epoll is readiness-based: "this socket is readable now, go read it".
//   - IOCP is completion-based: "the ReadFile you started earlier finished,
//     here's how many bytes arrived".
//
// This interface doesn't hide that difference. It doesn't do any I/O itself.
// watch() only says "tell me when something happens on this handle", and the
// IoEvent passed to the callback carries what the OS reported, in its own
// terms. The only code that watches handles is platform code (the IpcChannel
// implementations in src/pal/linux and src/pal/win32), and each knows which
// half of IoEvent its own reactor fills in. Shared code above IpcChannel never
// sees the difference. Only the wakeup and the timer are used from shared code,
// and those behave the same on both platforms.
//
// Threading
// ---------
// Everything runs on the thread that calls run() or run_once(). All callbacks
// run there too. wake() and stop() are the only functions that may be called
// from another thread. Every other function must be called from the reactor
// thread, including from inside a callback.
//
// Don't move a Reactor once anything is watched or the timer is set: the IPC
// channels keep a Reactor& to it. Create it where it will live.
//
// Each platform defines this class once: src/pal/linux/epoll_reactor.cpp and
// src/pal/win32/iocp_reactor.cpp. CMake only compiles one of them, so the
// linker picks the right one. There's no virtual dispatch and no #ifdef here.

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <system_error>

namespace am::pal {

// An OS handle, wide enough for both: a Linux file descriptor (int) or a
// Windows HANDLE/SOCKET (pointer-sized).
struct NativeHandle {
    std::intptr_t value = -1;
};

// What a Linux watcher wants to hear about. IOCP has no equivalent (it reports
// whichever operations the caller started), so the Windows reactor ignores it.
struct Interest {
    bool readable = false;
    bool writable = false;
};

// What happened on a watched handle. Each reactor fills in only its own half.
struct IoEvent {
    // epoll: the handle can be read or written without blocking, or the other
    // end hung up or the handle is in an error state. Level-triggered: these
    // keep being reported until the condition goes away (read until EAGAIN, or
    // unwatch a handle that hung up).
    bool readable = false;
    bool writable = false;
    bool hangup = false;

    // IOCP: one overlapped operation finished. `overlapped` is the OVERLAPPED*
    // the caller passed to ReadFile/WriteFile, so it can tell which one.
    // `bytes` is how many bytes moved, `error` is 0 or the Win32 error code.
    void* overlapped = nullptr;
    std::uint32_t bytes = 0;
    std::uint32_t error = 0;
};

class Reactor {
  public:
    using IoCallback = std::move_only_function<void(const IoEvent&)>;
    using Callback = std::move_only_function<void()>;

    // Opens the OS objects behind the reactor (epoll + eventfd + timerfd, or a
    // completion port + threadpool timer). Constructors can't report failure
    // without exceptions, so this is the only way to get a Reactor.
    [[nodiscard]] static std::expected<Reactor, std::error_code> create();

    ~Reactor();
    Reactor(Reactor&&) noexcept;
    Reactor& operator=(Reactor&&) noexcept;
    Reactor(const Reactor&) = delete;
    Reactor& operator=(const Reactor&) = delete;

    // --- Running ---------------------------------------------------------

    // Waits up to `timeout` (zero means don't wait), runs the callbacks for
    // whatever happened, and returns. Returns early if wake() or stop() is
    // called. Timing out with nothing to do is not an error.
    std::expected<void, std::error_code> run_once(std::chrono::milliseconds timeout);

    // Calls run_once() with no timeout until stop() is called or the OS
    // reports an error.
    std::expected<void, std::error_code> run();

    // Makes run() return after the callbacks it's currently running. Safe to
    // call from any thread, and from inside a callback. It uses the same
    // wakeup as wake(), so the wake callback may run once more.
    void stop() noexcept;

    // --- Watched handles -------------------------------------------------

    // Starts reporting events on `handle` to `callback`. The handle must stay
    // open until unwatch(). Watching the same handle twice is an error.
    //
    // On Windows the handle is associated with the completion port for the
    // rest of its life (IOCP can't undo that), and `interest` is ignored.
    std::expected<void, std::error_code> watch(NativeHandle handle, Interest interest,
                                               IoCallback callback);

    // Changes what an already watched handle reports. Linux uses this to ask
    // for `writable` only while a send is waiting on a full socket. A no-op on
    // Windows.
    std::expected<void, std::error_code> modify(NativeHandle handle, Interest interest);

    // Stops reporting events on `handle`. Safe to call from inside that
    // handle's own callback. After it returns, the callback won't run again,
    // even for an event that was already collected in the same batch, and
    // even if the OS reuses the handle's number for something watched later.
    //
    // On Windows, cancel outstanding I/O first (CancelIoEx) and let it finish:
    // completions that arrive after unwatch() are dropped, and the OVERLAPPED
    // they point at must stay alive until they do.
    std::expected<void, std::error_code> unwatch(NativeHandle handle);

    // --- Cross-thread wakeup ---------------------------------------------

    // Sets what runs on the reactor thread after wake(). Not from inside the
    // wake callback itself.
    void set_wake_callback(Callback callback);

    // Makes the reactor thread run the wake callback soon. Safe to call from
    // any thread. Several wake() calls close together may run the callback
    // only once, so the callback should handle everything that's pending (for
    // example, drain a whole queue), not one item per wake.
    //
    // Everything the calling thread wrote before wake() is visible to the
    // wake callback that runs after it. That's what makes "push onto a queue,
    // then wake()" safe. The same goes for stop() and the code after run()
    // returns.
    void wake() noexcept;

    // --- Timer -----------------------------------------------------------

    // Runs `callback` once, `delay` from now (measured on a monotonic clock).
    // There is one timer: setting it again replaces the previous deadline and
    // callback. It's fine to set it again from inside its own callback.
    std::expected<void, std::error_code> set_timer(std::chrono::milliseconds delay,
                                                   Callback callback);

    // Cancels the timer if it's set. Its callback won't run.
    void cancel_timer() noexcept;

  private:
    // Platform state (file descriptors, the completion port, the callback
    // tables) lives in Impl, defined in each platform's .cpp. That keeps
    // <sys/epoll.h> and <windows.h> out of this header.
    struct Impl;
    explicit Reactor(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace am::pal
