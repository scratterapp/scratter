# The MediaControls interface, explained

This covers `src/pal/interfaces/media_controls.hpp` (issue #55). It is the
contract that `mpris_controls.cpp` (#56, Linux) and `smtc_controls.cpp` (#57,
Windows) fill in. The Linux one is done ([mpris.md](mpris.md)); the Windows one
is still a stub that says "not supported".

## What it's for

Every desktop has a standard way for a music player to say "I'm playing this"
and to hear "the user pressed pause":

- **Linux:** MPRIS, a set of D-Bus methods and properties that players publish.
  The KDE and GNOME media widgets, `playerctl`, the play/pause key and Bluetooth
  headphone buttons all talk to it. Try `playerctl metadata` while Spotify or a
  browser tab is playing.
- **Windows:** SMTC (System Media Transport Controls), the thing behind the
  media flyout that pops up when you press a volume or media key.

The daemon owns this, not am-ui, so the keys keep working when the window is
closed. That's the whole point of splitting the app into two processes.

## Traffic in both directions

The vault (#52) only had calls going out: the daemon asks, the keyring answers.
Media controls have traffic both ways:

```
            set_track, set_playback_state,
            set_position, seeked, set_volume
  daemon  ------------------------------------>  desktop
          <------------------------------------
            MediaRequest callback: play, pause,
            next, seek, set volume...
```

Going out is easy: plain functions the daemon calls when something changes.

Coming in is the interesting part. A key press can happen at any moment, while
the daemon is busy doing something else. The interface answers "when and where
does the daemon hear about it?" in one sentence: **as a callback, on the
reactor thread.**

That matters because everything else in the daemon also runs on the reactor
thread: IPC commands from am-ui and amctl, the 10 ms tick, Ctrl-C. A pause
from the keyboard then works exactly like `amctl pause`, with no locks and no
way for the two to step on each other. It's the same promise
[`ShutdownSignal`](daemon.md) makes for Ctrl-C.

How each platform keeps that promise is its own business:

- On Linux, the D-Bus connection is a socket. The MPRIS code hands its file
  descriptor to `Reactor::watch()`, and the reactor wakes up when a message
  arrives. No extra thread at all.
- On Windows, SMTC calls its event handlers on a thread of its own. The
  Windows code has to pass the request over to the reactor thread before
  calling our callback. Exactly how is one of the open questions for kero in
  `docs/windows/START_HERE.md`.

## One callback, not eight

The desktop can ask for eight different things. There are two obvious shapes:

```cpp
// A: one callback per request
struct Callbacks { std::function<void()> play, pause, play_pause, stop, next,
                   previous; std::function<void(microseconds)> seek; ... };

// B: one callback, one small struct saying which
void on_request(const MediaRequest& request);   // request.kind, .position, .volume_percent
```

The header uses B. The daemon handles it with one `switch`, the compiler warns
if a new `Kind` is added and the `switch` forgets it (`-Wswitch` is part of
`-Wall`), and adding a ninth request later doesn't change `open()`'s
signature. The cost is that `position` and `volume_percent` sit there unused
for most kinds; the comments say which kind uses which.

## Seeking: one meaning, not two

MPRIS has two ways to seek: `Seek(offset)` ("10 seconds forward") and
`SetPosition(position)` ("go to 1:30"). SMTC only has the second. Rather than
make the daemon handle both, the interface only has "go to this position", and
the Linux code turns an offset into a position using the last value the daemon
gave `set_position()`. The daemon writes one seek handler, and Windows has
nothing to convert.

## "How often may I call this?"

`set_position()` is meant to be called every tick (100 times a second). That's
only OK because the header promises it **only stores the value**. Desktops ask
for the position when they want it (MPRIS clients read the `Position`
property), so there's nothing to send.

When the position *jumps*, the desktop does need telling, or its progress bar
keeps sliding from the old spot. That's what `seeked()` is for: store, and send
a notice (MPRIS's `Seeked` signal, SMTC's timeline update).

Writing down "cheap, call it every tick" vs "sends something" in the header is
what lets the daemon code stay simple. Without it, whoever writes the daemon
side has to read both platform implementations to find out.

## Why the setters don't return errors

Every other interface returns `std::expected`. The setters here return `void`.
If a "now playing" notice fails to send, the worst case is the desktop shows
stale text until the next change. The daemon couldn't do anything useful with
the error anyway, so making it check one on every tick would just be noise.
`open()` still returns `std::expected`, because the daemon does want to know
(and log) when media keys aren't available at all.

## What the tests can and can't check

`tests/test_media_controls.cpp` is the contract both platforms run. It can
check the daemon's side: `open()` works or says "not supported", every setter
accepts a whole track's life (and odd input like an empty track or non-ASCII
text), nothing arrives at the callback unasked, and closing frees the name for
the next `open()`.

It can't press a media key. That needs something outside the process
(`playerctl` on Linux, or a D-Bus call made by the test itself), which is
platform-specific, so #56 and #57 add those tests next to their code. On a CI
machine with no D-Bus session bus, `open()` fails with "not supported" and the
tests skip, the same way the vault tests skip with no keyring.

## Read next

- Spec section 3.4.1 (OS media bridges) and the Phase 4 gate in section 6
  (`playerctl play-pause` must work with no UI running).
- The [MPRIS spec](https://specifications.freedesktop.org/mpris-spec/latest/),
  for #56.
- [reactor.md](reactor.md), for what "on the reactor thread" means.
