# The daemon skeleton (#50)

Until this issue every piece worked on its own: the ring buffer, the drift
controller, the PipeWire sink, the reactor, the IPC socket, the telemetry page.
`main_daemon.cpp` returned 0. Now it's a real program you can start, that plays
a tone, answers commands and quits cleanly. This page is about how those pieces
are put together, which is most of what "writing a daemon" means.

Files:

- `src/daemon/main_daemon.cpp`: startup, the main loop, shutdown.
- `src/daemon/control_server.hpp/.cpp`: the IPC side, clients and commands.
- `src/daemon/audio_engine/audio_path.hpp`: ring buffer, drift controller,
  resampler and volume, between the tone and the sound card.
- `src/daemon/audio_engine/sine_generator.hpp`: the 440 Hz tone, standing in for CEF.
- `src/pal/interfaces/shutdown_signal.hpp` and `src/pal/linux/signalfd_shutdown.cpp`:
  Ctrl-C and SIGTERM.
- `tests/test_daemon.cpp`: starts the real binary and talks to it.
- `tests/test_audio_path.cpp`: play, pause, stop and volume without a sound card.

## Two threads, and what each one owns

The daemon has exactly two threads that run our code:

```
  reactor thread (main)                         audio thread (PipeWire's)
  ----------------------                        -------------------------
  IPC: accept, read, answer                     render(): every few ms, PipeWire
  Ctrl-C / SIGTERM                              asks for N frames, right now
  every 10 ms:
    top the ring buffer up with the tone  --->  ring buffer  --->  drift + resample
    write the telemetry page                                        volume
                                                                    into the sound card
  sets: state, volume (atomics)  ------------------------------->   reads them
  reads: position, fill, drift   <-------------------------------   writes them
```

Everything that isn't audio happens on the reactor thread, one callback at a
time. That's the whole point of an event loop: one thread waits on many things
(sockets, a timer, signals) and handles whichever is ready, so there's never a
second thread to race with and nothing to lock. A command from a client, a
timer tick and a Ctrl-C can arrive at the same moment; the reactor still runs
their callbacks one after another.

The audio thread is the one place we can't do that. PipeWire owns it, it runs
at real-time priority, and it must never wait. So the two threads share only
things that never block: the SPSC ring buffer (built for exactly one writer
and one reader) and a few atomics. Each atomic has a single writer, noted in
`audio_path.hpp`, which is what keeps it simple. The reactor thread writes the
play state and volume, the audio thread writes the position, buffer fill and
drift. Nobody writes the same thing.

The telemetry page has the same one-writer rule (it's a SeqLock), so the
reactor thread writes it, from numbers the audio thread left in its atomics.
The audio thread could write it directly, but then the page would only update
while audio plays, and a pause wouldn't show until the next buffer.

## Startup order matters

`main()` sets things up in a particular order, and each step is there for a reason:

1. **The reactor.** Everything else registers with it.
2. **Ctrl-C handling, before any other thread exists.** On Linux the signals
   are *blocked* and read from a file descriptor instead (more below). Threads
   inherit the blocked set from whoever creates them, so if PipeWire's thread
   were started first it would still have the default reaction, and a Ctrl-C
   landing on it would kill the process without any cleanup.
3. **The IPC listener**, which doubles as the "is a daemon already running?"
   check. Only one process can listen on a socket name. A second daemon gets
   `address_in_use`, prints "already running", and exits with code 3 before it
   has opened the sound card or touched the shared memory, so it can't disturb
   the first one.
4. **The audio sink, then the audio path.** The sink comes first because the OS
   may not give us the 48 kHz we asked for, and the tone generator and position
   math need the real rate. The ring buffer is filled before `start()`, so the
   first callback finds audio waiting.
5. **The telemetry page.**
6. **The tick timer**, then `reactor->run()` until a signal stops it.

Shutdown is the list backwards. Most of it is destructors running in reverse
declaration order, which is why `audio` is declared before `sink`: the sink's
destructor stops the audio thread, and only then is it safe to free the ring
buffer the thread was reading. The listener's destructor removes the socket
file, and the shared-memory mapping's destructor removes its name, so nothing
is left behind that looks like a crashed daemon.

## Catching Ctrl-C without a signal handler

The classic way is `signal(SIGINT, handler)`. The problem is that the handler
runs in the middle of whatever the thread was doing (maybe halfway through
`malloc`), so it may only call a short list of "async-signal-safe" functions.
No `printf`, no allocating, no touching most of your own data.

Linux has a cleaner way, `signalfd`:

1. Block SIGINT and SIGTERM, so the kernel just marks them pending.
2. Open a file descriptor that becomes readable when one is pending.
3. Watch it with the reactor like any socket.

The "signal" then arrives as an ordinary reactor callback, where anything is
allowed; ours calls `reactor->stop()`. Windows will do the same job with
`SetConsoleCtrlHandler`, whose handler runs on a separate thread and can wake
the reactor. Both hide behind `am::pal::ShutdownSignal`, so `src/daemon/` has
no `#ifdef`.

## Answering commands

`ControlServer` is a small loop per client: receive every waiting message,
check the frame (`validate_frame`), decode the body (`decode_body`), act, reply.

- Play, Pause, Stop and SetVolume change an atomic in the audio path, answer
  with `CommandAck`, then send a `PlaybackStateEvent` to **every** client. The
  answer goes first so a client waiting for its ack sees it before the event.
  Broadcasting means am-ui shows a pause that amctl caused, without polling.
- A body that doesn't decode gets `CommandNak` with `BadRequest`. A real
  opcode the daemon can't handle yet (Seek, LoadTrack...) gets `NotSupported`.
- A frame that fails `validate_frame` (wrong magic, bad checksum, another
  protocol version) gets no answer at all. There's nothing sensible to say to
  something not speaking our protocol, so it's disconnected.

Disconnecting is also the answer to a client that stops reading. A `send()`
that returns `WouldBlock` means the socket's buffer is full, which takes
hundreds of these small frames. Queueing more for a stuck client would just
grow memory, so it's dropped instead.

Each client lives in a `std::unique_ptr` because its readable callback holds a
pointer to it. A `std::vector<Client>` would move clients around in memory when
it grows, and those pointers would dangle.

## Pause, stop and the tone

`render()` checks the play state first:

- **Playing:** ask the drift controller for a ratio, resample out of the ring
  buffer, apply the volume, add to the position.
- **Paused:** write silence and leave the ring buffer alone. Playing again
  continues from exactly the same sample.
- **Stopped:** silence too, and the position goes back to 0.

For a tone, stop and pause sound the same. With real music, stop will also
throw away what's buffered, but that only means something once there's a
track to throw away.

Volume is plain linear multiplication (50% halves each sample). Most players
use a curve, because hearing isn't linear; that can come with the UI's slider.

## The test

`tests/test_daemon.cpp` starts the actual `scratterd` binary as a child
process, with two flags added for tests:

- `--name` gives it its own socket and telemetry names, so it can run next to
  a daemon you already have going.
- `--no-audio` skips the sound card. CI machines have no PipeWire, and you
  don't want a tone every time you run `ctest`.

Then it connects like am-ui will, says hello, pauses, plays, stops, sets the
volume, checks every answer and reads the play state back from shared memory.
It also starts a second daemon with the same name (it must exit with code 3),
and finally sends SIGTERM and checks the daemon exits with 0 and cleans up its
shared memory and socket file.

One detail worth knowing: this project builds without exceptions, so a failed
`REQUIRE` aborts the test process on the spot and no destructors run. The
daemon child would be left running, and ctest would wait forever for it to
close the output it inherited. The test installs a SIGABRT handler that kills
the child first.

## Trying it

On your machine, from the build directory:

```fish
./build/linux-debug/scratterd
```

You should hear the tone. Ctrl-C stops it with "stopped cleanly". Run it twice
in two terminals and the second one says it's already running. To pause it and
change the volume, use `amctl` from another terminal (#51, see
[amctl.md](amctl.md)).

## What you learn

- How a daemon is put together around one event loop, and why that removes
  most locking.
- What may and may not cross into the real-time audio thread.
- Why startup and shutdown order matter, and how C++ destructor order does
  most of shutdown for you.
- `signalfd`: turning signals into ordinary events.
- Testing a whole program from the outside, as a child process.
