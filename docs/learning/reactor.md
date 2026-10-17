# The event reactor, explained

This covers `src/pal/interfaces/reactor.hpp` (#12). The Linux version is #13
and the Windows version is #14. It assumes you know some C++ but haven't written
an event loop before.

## The problem it solves

The daemon has to keep an eye on several things at once:

- The IPC socket to `am-ui`: a command like "pause" can arrive at any moment.
- Other threads: CEF runs on its own threads and sometimes needs the daemon's
  main thread to do something.
- Time: "save the playback position every few seconds", or later, "curl wants
  to be called back in 200 ms".

The naive way is one thread per thing, each blocked in its own `recv()` or
`sleep()`. That means locks everywhere, because all those threads touch the
same player state. The other naive way is a loop that checks everything over
and over, which burns a CPU core doing nothing.

A **reactor** is the standard answer. One thread asks the OS: "here's a list of
things I care about; put me to sleep until any of them happens". When the OS
wakes it up, it runs the matching callback, then goes back to sleep. Everything
happens on that one thread, so the player state needs no locks, and the thread
uses no CPU while nothing is happening.

```
            +--------------------------------------+
            |  run():                              |
            |    sleep until something happens  <--+--- IPC socket has data
            |    run its callback                  |--- another thread called wake()
            |    repeat                            |--- the timer fired
            +--------------------------------------+
```

nginx, Redis, Node.js (through libuv) and most game servers are built around
this same loop.

## What our interface offers

Three kinds of things to wait on, plus the loop itself:

| Function | What it's for |
| --- | --- |
| `watch(handle, interest, callback)` | Tell me when something happens on this socket or pipe. |
| `wake()` | Another thread wants the reactor thread's attention. |
| `set_timer(delay, callback)` | Call me back once, `delay` from now. |
| `run()` / `run_once(timeout)` / `stop()` | Drive the loop. |

The rule that keeps it simple: **everything runs on one thread**. Only `wake()`
and `stop()` may be called from other threads. That's how CEF's thread hands
work over: push the work onto a queue, call `wake()`, and the wake callback
(on the reactor thread) drains the queue.

`wake()` calls can merge. If three threads call it at almost the same time,
the callback might run only once. That's why the callback drains the whole
queue rather than taking one item.

## The hard part: readiness vs completion

Linux and Windows answer "tell me when something happens" in opposite ways.

**epoll (Linux) is readiness-based.** You say "wake me when this socket is
readable". epoll wakes you, and then *you* call `recv()`, which now won't
block. epoll never touches your data.

**IOCP (Windows) is completion-based.** You call `ReadFile()` up front, handing
Windows a buffer and an `OVERLAPPED` struct. It returns right away. Later the
completion port wakes you to say "that read finished, 64 bytes are in your
buffer". The OS did the read for you while you were asleep.

A restaurant comparison: epoll is a pager that buzzes when your table is
ready, and then you walk over and sit down. IOCP is a waiter who brings the
food to wherever you are and tells you it's there.

An interface that covers both has three options:

1. **Make Windows look like epoll.** Windows named pipes have no "is this
   readable?" call. You'd fake it with tricks like zero-byte reads, which is
   fragile and costs an extra round trip per message.
2. **Make Linux look like IOCP.** This is what Boost.Asio does: on Linux,
   wait for readiness, then do the read inside the reactor and report it as a
   completion. It works, but the reactor then owns buffers and in-flight
   operations, and that's a lot more code.
3. **Don't hide it.** The reactor does no I/O. It just reports what the OS
   said, and the code that registered the handle deals with it.

We picked **option 3**, because of who actually calls `watch()`. Only the IPC
channel implementations do, and those are already platform code:
`unix_seqpacket_ipc.cpp` knows it's talking to epoll, and `named_pipe_ipc.cpp`
knows it's talking to IOCP. So the event struct has two halves:

```cpp
struct IoEvent {
    bool readable, writable, hangup;   // filled in by epoll
    void* overlapped;                  // filled in by IOCP: which operation finished
    std::uint32_t bytes, error;        // filled in by IOCP: how it went
};
```

The Linux channel reads the top half and calls `recv()` until `EAGAIN`. The
Windows channel reads the bottom half and starts the next `ReadFile()`. The
daemon code above them only sees "a frame arrived" from `IpcChannel` and never
learns which OS it's on. The wakeup and the timer work the same everywhere, so
shared code can use those directly.

The price: `Interest` only means something on Linux, and the `IoEvent` fields
only mean something on one side each. That's a bit untidy, and it's written
down in the header so nobody has to guess.

## Other choices worth knowing about

**Level-triggered.** epoll can be level-triggered (keeps telling you a socket
is readable until you've read everything) or edge-triggered (tells you once,
when it *becomes* readable). Edge-triggered is slightly faster, but if you
forget to read until `EAGAIN` you'll never hear about that socket again.
Level-triggered forgives that mistake, so we start there.

**One timer.** curl only ever wants one ("call me in N ms"), and so far the
daemon does too. If we need more later, they can be kept in a sorted list
behind this one OS timer.

**Errors without exceptions.** The project builds with exceptions off, so
anything that can fail returns `std::expected<void, std::error_code>`.
`std::error_code` already knows how to hold an `errno` value on Linux and a
`GetLastError()` value on Windows, so we didn't need our own error type. A
constructor can't return an error, so the only way to get a `Reactor` is the
`Reactor::create()` factory.

**One class, two `.cpp` files.** There's no virtual base class and no
`#ifdef`. `reactor.hpp` declares `class Reactor` once.
`src/pal/linux/epoll_reactor.cpp` defines its functions for Linux and
`src/pal/win32/iocp_reactor.cpp` defines them for Windows. CMake only compiles
the one for the platform you're on, so there's never a clash. The platform's
file descriptors and handles live in a private `Impl` struct (the "pimpl"
pattern) so the header doesn't have to include `<sys/epoll.h>` or
`<windows.h>`.

Spec section 4 suggests C++20 concepts here instead. With exactly one reactor
per build there's nothing to choose between at compile time, so a concept
would add a template layer without buying anything.

## Things that bite

- **Removing a watch from inside its own callback.** It's the most natural
  thing to do ("the peer hung up, stop watching it"), and a naive reactor
  destroys the callback while it's still running. The interface promises this
  is safe, so both implementations have to handle it.
- **A zero timer on Linux.** `timerfd_settime` with a zero delay *disarms* the
  timer instead of firing it right away. The epoll reactor has to turn zero
  into the smallest non-zero delay.
- **Windows has no documented way to un-associate a handle** from a completion
  port. `unwatch()` forgets the watch at once, and a packet that still arrives
  for it is dropped without looking at its `OVERLAPPED`. It also asks the kernel
  to detach the handle, so it can be watched again, but the kernel only agrees
  once every operation started without an event has been collected from the
  port. Cancel pending I/O and let its completions come back before unwatching,
  and keep each `OVERLAPPED` alive until its packet has been collected. Skip
  that and the handle stays on the port until it's closed, so a later `watch()`
  on it fails.
- **The Windows timer isn't a Windows timer.** A threadpool timer fires on a
  pool thread, so its callback would have to be posted back to the reactor
  thread, and a timer that fires just before `cancel_timer()` leaves a stale
  packet to recognise. With only one timer there is a simpler way: keep the
  deadline, pass the time left to `GetQueuedCompletionStatusEx` as its timeout,
  and run the callback on the reactor thread when the wait ends and the deadline
  has passed. Nothing else touches the timer, so there is nothing to race with.
- **Windows waits are coarse.** They follow the system timer tick, 15.625 ms by
  default, so a timeout or timer can come later than asked. A wait can also
  return a little early, so the loop looks at `steady_clock` before it believes
  a deadline has passed. "Never early" is the promise, "exactly on time" isn't.
  Shortening the tick with `timeBeginPeriod` changes the whole system's timer
  behavior, so the reactor leaves it alone.

## The Linux side (#13)

`src/pal/linux/epoll_reactor.cpp` is about 300 lines, most of it comments. One epoll instance
watches three kinds of file descriptor:

| fd | Made with | Becomes readable when |
| --- | --- | --- |
| `wake_fd` | `eventfd(0, EFD_NONBLOCK \| EFD_CLOEXEC)` | another thread calls `wake()`, which writes 1 to it |
| `timer_fd` | `timerfd_create(CLOCK_MONOTONIC, ...)` | the timer expires |
| your fds | whatever `watch()` was given | the OS has data (or room) for you |

Everything goes through one `epoll_wait()`. For each fd it returns, the loop
checks which kind it is and runs the right callback.

Some details worth reading the code for:

- **Why an eventfd merges wakeups.** An eventfd is a 64-bit counter.
  `write(1)` adds one, and `read()` returns the total and resets it to zero.
  Ten `wake()` calls before the loop gets round to it become one read and one
  callback.
- **Why `CLOCK_MONOTONIC`.** The wall clock can jump (NTP, the user changing
  the time). The monotonic clock only moves forward, so "in 200 ms" always
  means 200 ms.
- **How a callback can unwatch itself safely.** Each callback is stored behind
  a `std::shared_ptr`. Before calling it, the loop takes its own copy of the
  pointer. If the callback calls `unwatch()`, the map drops its copy, but the
  loop's copy keeps the function alive until it returns.
- **fd numbers get reused.** The kernel always hands out the lowest free fd
  number. If one callback closes fd 7 and a later callback in the same batch
  opens something new, that's fd 7 again, while the batch still holds an
  event for the *old* fd 7. Each `watch()` gets a generation number stored
  next to the fd in `epoll_event.data`, and an event whose generation doesn't
  match the current watch is dropped.
- **The stale timer read.** If one callback re-arms or cancels the timer in the
  same batch where the timer had already fired, reading `timer_fd` fails with
  `EAGAIN`. The loop treats that as "this expiry no longer counts" and skips
  it.
- **`EINTR`.** A signal can interrupt `epoll_wait()`. That's not a failure,
  just a short wait with nothing to report.

## The Windows side (#14)

`src/pal/win32/iocp_reactor.cpp` is one I/O completion port, and two kinds of
packet arrive on it:

| Packet | Posted by | Means |
| --- | --- | --- |
| a key that `watch()` handed out | the OS, when an overlapped operation on that handle finishes | the `ReadFile` or `WriteFile` is done: here are the bytes and the error |
| the reserved wake key | `wake()`, from any thread | run the wake callback |

Some details worth reading the code for:

- **Keys instead of handle numbers.** Each `watch()` associates the handle with
  the port under a new key, and keys are never reused. A packet whose key no
  longer belongs to a watch is stale and is dropped without reading its
  `OVERLAPPED`. This does the job of the generation number on Linux: handle
  numbers get reused on Windows too.
- **Where `error` comes from.** `IoEvent::error` is what `GetOverlappedResult`
  says about the finished operation: 0, or for example `ERROR_MORE_DATA` (the
  message was bigger than the buffer, and `bytes` says how much arrived),
  `ERROR_OPERATION_ABORTED` (after `CancelIoEx`) or `ERROR_BROKEN_PIPE` (the
  other end closed). A failed operation is still a completion, so the callback
  runs.
- **Completions you didn't wait for.** While a handle is on the port, an
  operation that finishes at once, with `ReadFile` returning `TRUE`, still
  produces a packet. The callback is the only place to handle a result.
- **Ten `wake()` calls, one packet.** A `wake_pending` flag stays set from the
  moment `wake()` posts a packet until the reactor thread takes it. Calls in
  between see the flag and post nothing. It is the counterpart of the eventfd's
  counter on Linux.
- **The timer.** `run_once()` works out the time to the earlier of its own
  deadline and the timer, waits that long, then checks the clock before
  believing it. A timer cancelled or replaced after its deadline can't run,
  because the state is only checked when the wait ends.

## Tests

The tests are split in three. `tests/test_reactor.cpp` covers what both
platforms share (timer, wakeup, stop) and runs on Linux and Windows.
`tests/test_epoll_reactor.cpp` uses pipes to test watching fds on Linux, and
`tests/test_iocp_reactor.cpp` tests watching handles on Windows with real
overlapped, message-mode named pipes. Run the epoll one under `linux-asan`:
the "callback unwatches itself" test is exactly the kind of bug that shows up as
a use-after-free.

## Read more

- `man 7 epoll`, `man 2 eventfd`, `man 2 timerfd_create`
- Microsoft's "I/O Completion Ports" page
- The libuv design overview (docs.libuv.org, "Design overview"). libuv is the
  library behind Node.js and solves the same epoll-vs-IOCP problem.
