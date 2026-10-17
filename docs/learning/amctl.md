# amctl, a command-line remote (#51)

`amctl` is to our daemon what `mpc` is to `mpd`: a tiny program that sends one
command and exits. It exists so we can drive the daemon by hand, and from tests,
long before the ImGui window does the same job.

```fish
amctl pause
amctl play
amctl stop
amctl volume 30
amctl status
```

Later it learned `ping`, `toggle`, `watch` and `--json`, and a test now makes
sure it keeps up with the daemon: see [amctl-parity.md](amctl-parity.md).

Files:

- `tools/amctl/main_amctl.cpp`: connecting, sending, waiting for the answer, and `status`.
- `tools/amctl/amctl_args.hpp`: turning the command line into a struct.
- `tests/test_amctl_args.cpp`: the parser on its own, no daemon needed.
- `tests/test_daemon.cpp`: the real `amctl` binary against a real daemon.

It lives in `tools/` rather than `src/client/` because it isn't part of am-ui.
Like the daemon, it links only `am_common` and `am_pal`, and has no `#ifdef`:
the IPC channel, reactor and shared memory hide the platform.

## Two ways to talk to the daemon

The daemon offers two channels, and amctl uses both:

| | IPC socket | Telemetry page |
| --- | --- | --- |
| Direction | both ways | daemon to readers only |
| Used for | commands: play, pause, volume | reading state: position, volume, buffer |
| Cost | a round trip through the kernel | reading memory, no syscalls once mapped |
| amctl command | `play`, `pause`, `stop`, `volume` | `status` |

`amctl status` never touches the socket. It maps the telemetry page read-only
(`ShmMapping::open_read_only`) and calls `TelemetryPage::read()`, exactly what
am-ui will do once per frame. That's why the volume moved into the telemetry
snapshot in this issue: before, it was only in the IPC `PlaybackStateEvent`, so
a reader of the page couldn't see it. It took one byte of the old two-byte
`reserved_pad`, so the snapshot is still 320 bytes and nothing else moved.

The daemon rewrites the page every 10 ms, and also right after any command that
changes the state or volume, before it sends the ack. So by the time
`amctl volume 30` prints "ok", `amctl status` already shows 30. (Before #52 it
only had the 10 ms tick, and a `status` straight after a command could show the
old value.)

## One command, start to finish

```
amctl                                    daemon
-----                                    ------
connect to $XDG_RUNTIME_DIR/scratterd.sock
HelloReq  "amctl"          seq 1  --->
PauseReq                   seq 2  --->
                                  <---   HelloAck "scratterd"
                                  <---   PlaybackStateEvent (current state)
                                  <---   CommandAck for seq 2, status Ok
                                  <---   PlaybackStateEvent (now paused)
print "pause: ok", disconnect
```

A few things in there are worth noticing.

**Matching answers by sequence number.** Every frame header carries a `seq_id`
the sender picks. The daemon's `CommandAck` repeats the request's number in
`request_seq_id`. amctl sends only one request, so this looks like overkill
here, but am-ui will have several in flight (a volume drag sends many), and the
number is how it knows which answer belongs to which request.

**Skipping what you didn't ask for.** The daemon also sends a `HelloAck` and
`PlaybackStateEvent`s. amctl reads past anything that isn't a `CommandAck` or
`CommandNak` for its request. A client can't assume the next message is the
answer.

**The Hello is optional.** The daemon would answer without it. amctl sends one
so the daemon's log says "amctl connected" rather than "a client (no hello
yet)", which helps when you're watching both terminals.

**Waiting without spinning.** `receive()` never blocks; it says `WouldBlock`
when nothing has arrived yet. Calling it in a tight loop would burn a CPU core.
Instead amctl calls `reactor.run_once(time_left)`, which sleeps in `epoll_wait`
until the socket is readable or the time runs out, then tries `receive()`
again. After 2 seconds with no answer it gives up and says so. The daemon
answers in microseconds, so 2 seconds means something is stuck, and a test
tool that hangs forever is worse than one that fails.

## Parsing the command line

`parse_arguments()` takes the words after the program name and returns
`std::expected<Arguments, std::string>`: either a filled-in struct or a
one-line reason ("volume needs a number from 0 to 100, not \"50%\""). The
project has exceptions turned off, so this is how a function says "that didn't
work, and here's why", the same way `ShmMapping::create` does.

The number goes through `std::from_chars`, not `std::stoi` or `atoi`.
`std::stoi` throws on garbage (and we have no exceptions), and `atoi` quietly
returns 0 for "loud", which would mute you. `from_chars` reports what it
parsed and where it stopped, so the code can insist the whole word was digits
and the value is at most 100.

Keeping the parser in its own header, apart from all the socket code, is what
makes it testable. `test_amctl_args.cpp` feeds it word lists and checks the
result, with no daemon, no socket, nothing to start or clean up. The end-to-end
test in `test_daemon.cpp` then only has to prove the pieces are wired together.

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | the daemon did it (or `status` printed) |
| 1 | no daemon running, it said no (`CommandNak`), or it didn't answer |
| 2 | bad command line; nothing was sent |

Scripts and tests can rely on these, the same way the daemon's own exit codes
(3 means "already running") are part of how `test_daemon` checks it.

## Trying it

Build, then start the daemon in one terminal:

```fish
./build/linux-debug/scratterd
```

In a second terminal:

```fish
./build/linux-debug/amctl status
./build/linux-debug/amctl pause
./build/linux-debug/amctl play
./build/linux-debug/amctl volume 20
./build/linux-debug/amctl volume 100
```

The tone should stop on pause, come back on play, and get quieter at 20. The
daemon's terminal logs each "amctl connected" and "amctl disconnected".

## What you learn

- The client side of request and answer over IPC: sequence numbers, skipping
  messages you didn't ask for, and a timeout.
- When to use the socket (commands) and when the shared page (reading state).
- Waiting for I/O with the reactor instead of spinning.
- `std::expected` and `std::from_chars` for error handling without exceptions.
- Splitting "understand the input" from "do the work" so each can be tested
  on its own.
