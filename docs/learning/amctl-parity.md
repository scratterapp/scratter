# amctl can do everything am-ui can

The rule: anything the window will be able to ask the daemon for, `amctl` can
ask for too. Then tests, scripts and either of us (or our AI helpers) can drive
the daemon with no screen, and no feature ever works only by clicking.

This round filled the gaps amctl had and added a test that keeps them filled.

```fish
amctl play
amctl pause
amctl stop
amctl toggle          # pause if playing, otherwise play
amctl volume 30
amctl ping            # is the daemon alive? prints "pong in 76 us"
amctl status
amctl status --json   # the same, as one line of JSON
amctl watch           # prints each state change until the daemon quits
amctl watch --json    # the same, one JSON object per line
```

## What's new and why

**`ping`.** The protocol has had `Ping` and `Pong` opcodes since the start, but
the daemon answered `Ping` with "not supported". Now it answers with a `Pong`
whose header `seq_id` is the `Ping`'s own number, the same trick the
`ipc_ping_pong` benchmark uses, so a client with several pings in flight knows
which one came back. am-ui will use this to notice a daemon that's stuck
rather than dead. amctl times the round trip.

**`toggle`.** The play/pause button in the UI and the play/pause media key both
mean "pause if playing, otherwise play". There's no opcode for it; the client
decides. amctl reads the current state from the `PlaybackStateEvent` the daemon
sends straight after every hello, then sends `PlayReq` or `PauseReq`. That's
the same decision the daemon makes for the media key in `main_daemon.cpp`.

**`watch`.** am-ui won't poll for changes. It listens for `PlaybackStateEvent`s,
which the daemon sends to every connected client when the state or volume
changes, whoever changed it (amctl, a media key, another client). `amctl watch`
does exactly that and prints each one. It's handy in a second terminal while
you press media keys or run other amctl commands. It stops with "the daemon
quit" when the daemon goes away.

**`--json`.** Text is for people. A script or an AI agent reading "volume: 30%"
has to guess the format; `{"volume_percent":30}` it can parse. `status --json`
prints the telemetry snapshot as one object; `watch --json` prints one object
per line (a format called JSON Lines), so a reader can handle each line as it
arrives. amctl writes the JSON with `printf`: every value is a number or one of
four fixed state names, so nothing needs escaping and no JSON library is
needed.

## Keeping it true: coverage()

A rule that lives only in a doc gets forgotten. `coverage()` in
`tools/amctl/amctl_args.hpp` gives every opcode one line saying what amctl
does about it:

| Kind | Meaning | Opcodes |
| --- | --- | --- |
| `Command` | an amctl command sends it | `Ping`, `PlayReq`, `PauseReq`, `StopReq`, `SetVolumeReq` |
| `Automatic` | every command sends it | `HelloReq` |
| `NotYet` | the daemon can't do it yet | login, `LoadTrackReq`, `SeekReq`, `SetAudioModeReq`, the four queue requests |
| `FromDaemon` | only the daemon sends it | the acks, `Pong` and the events |

Two checks hold it in place:

1. **The compiler.** The `switch` in `coverage()` has no `default:`. GCC and
   Clang warn when a switch over an enum misses one of its values (`-Wswitch`,
   part of `-Wall`), and our warnings are errors. So adding an opcode to
   `ipc_protocol.hpp` breaks the build until someone decides its line here.
   (MSVC doesn't warn about this by default, so the Linux build is the one that
   catches it.)
2. **A test against the real daemon.** `test_daemon.cpp`'s "amctl has a
   command for every request the daemon handles" sends every request opcode to
   a running daemon. A `NotYet` one must still get `NotSupported`; the rest must
   not. The day someone teaches the daemon `SeekReq`, that test fails with a
   comment saying what to do: add `amctl seek` and change the line to
   `Command`. `test_amctl_args.cpp` checks the other half, that every command
   word `coverage()` names really parses.

## What arrives with the real player

These requests exist in the protocol but the daemon only plays a test tone, so
there's nothing for them to do yet. Each gets an amctl command when the daemon
learns it (the test above makes sure of that):

- `LoadTrackReq` (play a song by its Apple Music id): `amctl load ID`
- `SeekReq`: `amctl seek 1:30`
- the queue requests (`QueueReplaceReq`, `QueueAppendReq`, `QueueRemoveReq`,
  `QueueShuffleReq`), plus next and previous, which the media keys already ask
  for: `amctl queue ...`, `amctl next`, `amctl previous`
- `SetAudioModeReq` (shared vs exclusive output)
- `AuthEscalationReq`, the login flow

The command names are suggestions; whoever adds each one picks.

## A bug worth knowing about

The first version of this code crashed `amctl watch` when the daemon quit. The
cause: `IpcChannel::connect(reactor, ...)` keeps a reference to the `Reactor`
it was given. amctl made the reactor, connected, and then moved both into a
`Connection` struct. The moved-to reactor lived at a new address, so the
channel's reference pointed at the old, emptied one. Commands that finished
quickly never touched it; `watch` did, when the hang-up came in.

The fix keeps the `Connection` at one address: it's built first, with the
reactor inside it, on the heap (`std::make_unique`), and the channel is
connected to that reactor afterwards. The test's `Client` struct in
`test_daemon.cpp` already did it this way. The general lesson: if an object
hands out references to itself, moving it breaks them, so either make it
non-movable or make sure nobody moves it after the references exist.

## Trying it

Build, then start the daemon in one terminal:

```fish
./build/linux-debug/scratterd
```

Open a second terminal window and leave this running in it:

```fish
./build/linux-debug/amctl watch
```

Open a third and try:

```fish
./build/linux-debug/amctl ping
./build/linux-debug/amctl toggle
./build/linux-debug/amctl toggle
./build/linux-debug/amctl volume 40
./build/linux-debug/amctl status --json
```

The watch window prints a line for each toggle and the volume change. Press
the play/pause key on your keyboard and it prints one for that too.

## What you learn

- Turning a design rule into something the compiler and the tests enforce.
- Events versus polling: why the UI listens for changes instead of asking.
- Output for machines (`--json`) next to output for people.
- References into an object don't survive moving it.
