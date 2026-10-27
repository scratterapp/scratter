# The window's connection to the daemon (#36)

`am-ui` is the window; `scratterd` is the daemon that actually plays music.
They are two separate programs (two processes, in OSTEP's words), so the
window has to talk to the daemon over the IPC socket and read its telemetry
page, just as `amctl` does. This issue writes that part of the window, without
the window itself (that's #35), so it can be tested on a machine with no screen.

Files:

- `src/client/daemon_link.hpp/.cpp`: `DaemonLink`, the connection. It runs on
  its own thread, says hello, sends commands, follows the daemon's state, and
  reconnects when the daemon comes and goes.
- `src/client/ui_state.hpp/.cpp`: `DaemonState`, the plain struct of what the
  link knows, and `TelemetryReader`, which reads the shared-memory page once
  per frame.
- `tests/test_daemon_link.cpp`: starts the real daemon and drives it through
  the link, kills it, starts it again. Also fakes a broken daemon.
- `tests/daemon_process.hpp`: starting a daemon from a test, moved out of
  `test_daemon.cpp` so both test files share it.

CMake builds the two `src/client` `.cpp` files into a library, `am_client_core`.
It links `am_pal` and `am_common` and no graphics library. `am-ui` links it,
and so does the test.

## Why a second thread

A window spends most of its life asleep, waiting for something to draw. SDL,
the window library, sleeps in `SDL_WaitEvent()` until there's a mouse move, a
key press, a resize... but it can't also wait on our socket at the same time,
not on every platform.

So if the window thread owned the socket, a pause from the keyboard's media
keys (which reaches the daemon over MPRIS, not through the window) would only
show up when the window happened to wake for some other reason. The window
would also have to wake every so often just to check.

Instead, the link has a thread of its own, with its own `Reactor` (the event
loop from [reactor.md](reactor.md)). That thread sleeps in the reactor, which
*can* wait on the socket. When a message arrives it updates the state and calls
a **wake callback** the window gave it. The window's callback will be
`SDL_PushEvent()`, which SDL lets any thread call, so the window wakes up at
once and redraws.

```
 window thread                              link thread
 -------------                              -----------
 SDL_WaitEvent()  ... asleep ...            reactor.run()  ... asleep ...
                                            daemon: PlaybackStateEvent (Paused)
                                            lock, state.playback = Paused, unlock
                  <------ wake callback ---  (SDL_PushEvent)
 wakes up
 link.state()  -> copy, under the lock
 draws "Paused"
```

You'll meet threads properly in OSTEP's concurrency part (chapter 26 onwards).
For now: a thread is a second line of execution inside the same process. Both
threads see the same memory, which is handy and also the danger.

## Sharing memory between the two threads

Two threads touch the same data:

- **The state.** The link thread writes `DaemonState` (playing or paused,
  volume, connected or not). The window thread reads it.
- **The command queue.** The window thread adds "pause" to a list. The link
  thread takes it off and sends it.

If one thread writes a variable while another reads it, with nothing to keep
them apart, that's a *data race*. In C++ it's undefined behavior, not just "you
might read an old value": the compiler is allowed to assume it never happens.
ThreadSanitizer (`linux-tsan`) catches exactly this, which is why the issue
asks for the tests to pass under it.

The fix here is a plain `std::mutex`, a lock. Whoever holds it is the only one
touching the shared data; the other waits until it's released (OSTEP chapter
28). The code locks it, copies the struct or pushes onto the queue, and unlocks:
a few dozen bytes, so the wait is never noticeable.

"But the rules say no locks!" The rule is about the **audio callback**, the
code PipeWire calls every few milliseconds that must never wait (see
[audio-sink.md](audio-sink.md)). Neither of these threads is that one, and
neither is in the daemon at all.

Two details worth spotting in `daemon_link.cpp`:

- **The wake callback runs after unlocking.** `publish()` changes the state
  inside the lock, then calls `on_change` outside it. If it called it while
  still holding the lock, and the window's callback then called `state()`
  (which takes the same lock), the thread would wait for itself forever: a
  *deadlock*.
- **It only wakes the window when something really changed.** `publish()`
  compares the state before and after (`DaemonState` has `operator==` for
  this). While the daemon is down, the link retries every second, and each
  failed try sets "not running" again; without the comparison, the window
  would wake every second to redraw the same thing.

Commands go the other way: `pause()` locks, pushes onto the queue, unlocks, and
calls `Reactor::wake()`, the one reactor function that's safe from another
thread. The reactor then runs `send_queued()` on the link thread, which sends
everything waiting.

## Matching answers to requests

Each command gets an id when it's asked for (`pause()` returns it), and that id
goes out as the request's `seq_id`. The daemon's `CommandAck` or `CommandNak`
echoes it back in `request_seq_id`, so the link knows which request was
answered, even with several on the way. The answer lands in
`DaemonState::last_result`, for the window to show "couldn't do that" under
the buttons.

The ack and the new state are separate messages. `pause()` gets an Ack
("I did it") and, separately, a `PlaybackStateEvent` ("I'm paused now"), which
every connected client receives, whoever asked. The window draws from the
event, not the ack, so it shows the same thing whether you paused in the
window, with `amctl`, or with the media keys.

## The daemon going away is normal

The window can open before the daemon, and the daemon can quit or crash while
the window stays open. Neither is an error. `LinkStatus` says where things
stand:

| Status | Means | What the link does |
| --- | --- | --- |
| `NotRunning` | nothing is listening on the socket | tries again every second |
| `Connecting` | connected, hello sent, waiting for `HelloAck` | gives up after 2 s |
| `Connected` | commands go through, state is live | |
| `Failed` | anything else; `error` says what | tries again every second |

`Failed` covers three cases worth knowing:

- `permission_denied`: the socket checks from
  [ipc-socket-checks.md](ipc-socket-checks.md) refused the daemon (another user
  runs it, or `$XDG_RUNTIME_DIR` isn't private).
- `timed_out`: the daemon took the connection but didn't answer the hello or a
  command within 2 s. It's stuck, so the link hangs up and starts over.
- `bad_message`: the daemon sent something that failed `validate_frame()` or a
  body that didn't decode. The link never trusts what comes in, the same rule
  the daemon follows with its clients. It hangs up rather than guessing.

A command asked for while not connected fails at once (`NotConnected`). It
isn't saved for later: a pause you clicked while the daemon was down shouldn't
suddenly happen a minute after it comes back.

## One timer, three deadlines

The reactor has exactly one timer, but the link has three kinds of deadline:
when to try connecting again, when to give up on the hello, and when to give up
on each command's answer. `arm_timer()` works out which of those is soonest and
sets the timer for that. When it fires, `on_timer()` checks what's due, does
it, and calls `arm_timer()` again. That's a common trick: one alarm clock, set
each time for the next thing on the list.

## Reading telemetry

The moving numbers (position, buffer fill, later the spectrum) don't come over
the socket. They're in the shared-memory page the daemon rewrites constantly
([seqlock.md](seqlock.md)). The window reads it with `TelemetryReader`, on
its own thread, once per frame:

```cpp
TelemetryReader telemetry(name);
// each frame:
const DaemonState state = link->state();
const TelemetrySnapshot& t = telemetry.update(state);
```

Three rules:

1. **One read attempt per frame, never a retry loop.** If the daemon is in the
   middle of writing, `try_read()` says so and the reader keeps last frame's
   numbers. A daemon killed mid-write leaves the page looking busy forever, so
   a loop could hang the window (spec 3.2.2).
2. **Map the page after each `HelloAck`, drop it when the connection goes.**
   A daemon that quits cleanly removes the page's name, and the next one makes
   a brand-new page under the same name. The old mapping still points at the
   dead daemon's memory, which the kernel keeps as long as someone has it
   mapped, so the window would show frozen numbers forever. (A crashed daemon
   leaves the name behind and the next one reuses it, but the reader doesn't
   need to know which happened.) So `connection_count`, which goes up at every
   `HelloAck`, tells the reader "new daemon, map again".
3. **Say when the numbers are stale.** `fresh()` is false when nothing is
   mapped or no read has worked for a second; the window will grey the numbers
   out.

## The tests

`tests/test_daemon_link.cpp` does what the issue asks:

1. Start the real daemon. Connect through the link, pause, see the `Paused`
   state arrive by itself, set the volume, see the telemetry page agree, get a
   `BadRequest` for volume 101, toggle, stop.
2. Start the link with no daemon: `NotRunning`, and commands fail straight
   away. Start a daemon: it connects. Kill the daemon with `SIGKILL` (no chance
   to clean up, like a crash; OSTEP chapter 5's `kill()`): `NotRunning`, the
   last state is kept, the page is let go. Start another daemon: it reconnects
   by itself, `connection_count` is 2, and the reader maps the new page.
3. A fake daemon, a bare `IpcListener` on its own thread, that either never
   answers or answers with 32 bytes of junk. The link must end up `Failed` with
   `timed_out` or `bad_message`.

The tests wait the way the window will: they sleep on a condition variable
that the wake callback pokes, then look at `state()`. That checks the wake
callback really fires.

Run them under ThreadSanitizer, since this is the first shared code where two
of our own threads share data:

```fish
cmake --preset linux-tsan
cmake --build --preset linux-tsan
ctest --preset linux-tsan -R link
```

## What's next

- #35 opens the window and passes `SDL_PushEvent` as the wake callback.
- #37 draws the now-playing view from `DaemonState` and the telemetry snapshot.
- Later, `amctl` can move onto this library too, so the handshake lives in one
  place (design doc, open question 7).
