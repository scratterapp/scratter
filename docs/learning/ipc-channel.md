# The IPC channel, explained

This covers `src/pal/interfaces/ipc_channel.hpp` (#11). The Linux version is #15
(`unix_seqpacket_ipc.cpp`) and the Windows version is #12 (`named_pipe_ipc.cpp`).
Read [ipc-frame.md](ipc-frame.md) and [reactor.md](reactor.md) first: a channel
carries frames, and it's driven by the reactor.

## The problem it solves

The frame header says what a message looks like. Something still has to carry
it from `am-ui` to `scratterd` and back. That's the channel: a connection
between the two processes, with one side (the daemon) **listening** and the
other (the UI) **connecting**.

Two classes:

- `IpcListener`, which only the daemon has. It sits on a well-known name
  (`kDaemonEndpoint`, the daemon's name from `src/common/app_name.hpp`, written
  `scratterd` below) and hands out one `IpcChannel` per UI that connects.
- `IpcChannel`, one end of a connection. Both sides have one. `send()` a
  message, `receive()` a message.

## Messages vs streams

This is the main idea in the issue.

A TCP socket or a plain pipe is a **byte stream**. If you send 32 bytes and then
another 40, the other side might receive 72 at once, or 10 then 62, or any
other split. The bytes arrive in order, but the boundaries between your sends
are lost. So every stream protocol needs code that buffers what arrived, works
out where one message ends, and keeps the leftover for next time. That code is
easy to get subtly wrong, and it's exactly where parsers get fuzzed into
crashing.

A **message** transport keeps the boundaries. One `send()` of 72 bytes is one
`receive()` of 72 bytes, whole, or nothing. Both of our OSes have one:

| | Linux | Windows |
|---|---|---|
| What | `AF_UNIX` socket, `SOCK_SEQPACKET` | named pipe, `PIPE_TYPE_MESSAGE` |
| Address | `$XDG_RUNTIME_DIR/scratterd.sock` | `\\.\pipe\scratterd.<SID>` |
| Who can connect | file mode `0600` | the pipe's DACL |

So in our code, one message is one frame, and the receiver hands each one
straight to `validate_frame()`. No reassembly anywhere.

## Never blocking

The daemon's thread runs the reactor. If it ever sat inside a `recv()` waiting
for the UI, it would stop answering media keys, timers, everything. So nothing
in the channel waits.

When the OS can't do it right now, the call returns `IoStatus::WouldBlock`:

- `receive()` with nothing waiting: WouldBlock.
- `send()` when the OS's buffer for this connection is full (the other side
  isn't reading fast enough): WouldBlock, and none of the message was sent.

WouldBlock isn't an error. It's the normal answer to "anything for me?" when
there isn't. Errors (`std::error_code`) are kept for things that are actually
broken, so the calling code can treat them differently.

Then the channel tells you when to try again, by calling a callback on the
reactor thread:

```cpp
channel.set_readable_callback([&] {
    std::array<std::byte, am::common::kMaxFrameBytes> buffer;
    for (;;) {
        auto result = channel.receive(buffer);
        if (!result || result->status != IoStatus::Done) {
            break; // error, nothing left (WouldBlock), or PeerClosed
        }
        handle(std::span(buffer).first(result->bytes));
    }
});
```

The loop matters: one callback can mean several messages are waiting.

The other side hanging up (the UI window closed) is also a normal event, not
an error: `IoStatus::PeerClosed`. The usual response is to destroy the channel,
and the interface allows doing that from inside the callback.

## Why the interface looks like this on both platforms

The [reactor](reactor.md) explains that Linux is readiness-based ("you can read
now") and Windows is completion-based ("the read you started has finished").
The channel interface is where that difference gets hidden from the rest of the
code. Each platform does it differently underneath:

- **Linux**: the channel watches its socket with the reactor. "Readable" from
  epoll calls the readable callback. `receive()` is a plain non-blocking
  `recv()`, which returns `EAGAIN` (WouldBlock) when nothing is there.
- **Windows**: the channel keeps one overlapped `ReadFile` going into its own
  buffer. When IOCP says it finished, the channel calls the readable callback,
  and `receive()` copies the message out and starts the next `ReadFile`. With no
  finished read, `receive()` returns WouldBlock.

Both end up behaving the same from the outside, which is the point.

## Daemon already running?

`IpcListener::listen()` fails with `address_in_use` if a daemon is already
listening on the name. That's how a second `scratterd` finds out it isn't
needed.

On Linux there's a catch: a socket is a file, and a daemon that crashed leaves
the file behind. Binding to it fails even though nobody is listening. So the
Linux version tries to connect first. If that's refused, the file is stale and
it's safe to delete it and bind again. Windows pipes vanish when their last
handle closes, so they don't have this problem.

## Security, briefly

Anyone who can connect can tell the daemon what to play, and later, the daemon
holds your Apple Music login. So only the same user may connect. On Linux the
socket file is mode `0600` inside `$XDG_RUNTIME_DIR`, which is already private
to you.

Windows has one pipe namespace for the whole machine, so the pipe's name ends
in your SID (your user's security identifier) and the pipe gets a security
descriptor that lets only you in. That protects the daemon, but not the UI: any
user can create a pipe name first and pretend to be the daemon.
`FILE_FLAG_FIRST_PIPE_INSTANCE` only makes the real daemon's `listen()` fail
with `address_in_use` instead of sharing that pipe. So `connect()` checks that
the process serving the pipe runs as you, and fails with `permission_denied` if
it doesn't.

## The Linux version, line by line

`src/pal/linux/unix_seqpacket_ipc.cpp` (#15). The pieces, in the order a
connection uses them:

**Finding the socket file.** `socket_path()` checks the endpoint name (only
letters, digits, `-`, `_`, `.`, so `../` tricks are impossible), reads
`$XDG_RUNTIME_DIR`, and builds `<dir>/<endpoint>.sock`. A socket address keeps
the path in `sun_path`, a fixed 108-byte array, so a longer path is refused
with `filename_too_long` instead of being cut short. With no `$XDG_RUNTIME_DIR`
it refuses outright rather than falling back to `/tmp`, which other users can
see into.

**Listening.** `socket()` with `SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC`
(`CLOEXEC` stops the fd leaking into CEF's child processes). Then `bind()`
creates the file. If that fails with `EADDRINUSE`, the file already exists:
`is_stale_socket_file()` tries to connect to it. A running daemon accepts, so
the answer is `address_in_use`. A dead one refuses (`ECONNREFUSED`), so the
file is deleted and `bind()` tried again. Then `chmod(0600)` and `listen()`. The
order matters: nobody can connect until `listen()`, so there's no moment where
the file has looser permissions and someone can get in.

**Accepting.** `accept4()` with the same non-blocking flags, in a loop until
`EAGAIN` ("nobody waiting", returned as `std::nullopt`). Each new fd becomes an
`IpcChannel` via `IpcChannel::Impl::open()`, which registers it with the reactor.

**Sending.** One `send()` call. On a `SOCK_SEQPACKET` socket the kernel takes
all of the message or none of it, so there's no "sent half, send the rest
later" bookkeeping. `MSG_NOSIGNAL` matters: without it, sending to a closed
socket kills the whole process with `SIGPIPE`. On `EAGAIN` (the kernel's
buffer for this socket is full) the channel asks epoll to report "writable"
too, and turns that back off once the writable callback has run. It doesn't
ask all the time, because a socket is writable nearly always and epoll would
wake the reactor constantly.

**Receiving.** `recvmsg()`, not `recv()`, only because it returns `msg_flags`.
If the message didn't fit in the buffer, the kernel sets `MSG_TRUNC` and drops
the rest; we report `message_size`. A return of 0 means the other side closed,
which is why `send()` refuses empty messages: on this socket type an empty
message would look exactly like a hang-up.

**Hang-ups.** After `PeerClosed` the channel unwatches its fd. epoll is
level-triggered and a closed socket stays "readable" forever, so otherwise the
reactor would spin calling the callback.

A hang-up doesn't mean there's nothing left to read, though. If the other side
sends a message and closes straight away, both arrive together: the message is
still queued in our socket, and epoll reports "readable" and "hung up" in the
same loop. `recvmsg()` hands out the queued messages first and only returns 0
once they're gone, so a hang-up is only final when `receive()` says
`PeerClosed`. A channel with no readable callback (amctl, which calls
`receive()` itself in a loop) used to mark itself closed as soon as epoll
reported the hang-up, throwing that last message away. That's what made the
"amctl drives the daemon" test fail now and then on a busy machine: the daemon
sent `amctl watch` its "playing" event, the test stopped the daemon right
after, and if the watcher hadn't woken up in between, the event was lost. Now
the channel only stops watching at that point and lets `receive()` find the
end of the queue. The test "messages sent just before a hang-up are still
received" sets up exactly that order, so it doesn't depend on luck.

**Destroying a channel from its own callback.** The usual reaction to
`PeerClosed` is to destroy the channel, often from inside the readable
callback. Two things make that safe. The callbacks are stored behind a
`shared_ptr`, and the code takes its own copy before calling one, so the
function being run isn't destroyed under itself (the epoll reactor does the
same with its watch callbacks). And `on_event()` runs at most one callback per
event and touches nothing afterwards, because `this` may be gone. There's a
test that does exactly this under ASan.

## Measuring it

`bench/ipc_ping_pong.cpp` is the "under 25 µs" check from the spec. One thread
plays the daemon and answers `Ping` with `Pong`. The other sends a `Ping`,
waits for the `Pong`, and times it, 100,000 times. Each side has its own
reactor, so every round trip goes through what the real thing does: encode,
send, epoll wake-up, receive, validate, and back.

```sh
cmake --preset linux-release
cmake --build --preset linux-release
./build/linux-release/bench/ipc_ping_pong
```

It prints min, median, p99, p99.9 and max. The median is the honest headline:
the average gets dragged around by a few very slow round trips (the OS
scheduling something else on that core), and the max is mostly noise. p99 tells
you how bad the slow ones are. Use a release build; debug and sanitizer builds
measure the sanitizer.

The fastest round trips are about 3 µs. Most of the median is the time the
kernel takes to wake a sleeping thread, which depends a lot on the machine. On
a shared cloud VM the median came out around 32 µs. Run it on real hardware
before deciding anything.

## Things to try

- Send two frames back to back and check they arrive as two `receive()`s. Then
  read about what would happen with `SOCK_STREAM`.
- Fill the socket with sends until one returns WouldBlock, and count how many
  frames fit. That number is the kernel's send buffer divided by the frame size.

## Read more

- `man 7 unix` (Linux), the `SOCK_SEQPACKET` parts
- Microsoft docs: "Named Pipe Type, Read, and Wait Modes"
