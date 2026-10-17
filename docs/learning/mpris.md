# MPRIS media keys on Linux, explained

This covers `src/pal/linux/mpris_controls.cpp` (issue #56), the Linux side of
the [MediaControls interface](media-controls.md), and how the daemon uses it.
After this, the play/pause key on your keyboard, the media widget in the KDE or
GNOME panel and `playerctl` all control the daemon's tone.

## D-Bus in five words: objects, interfaces, methods, properties, signals

D-Bus is a message bus: one background program (`dbus-daemon` or
`dbus-broker`) that every desktop program connects to, so they can talk without
knowing about each other. There's a *session bus* per logged-in user, and that's
the one MPRIS uses.

Things on the bus are organised a bit like a C++ program:

| D-Bus | Like | Ours |
| --- | --- | --- |
| bus name | a program's address | `org.mpris.MediaPlayer2.scratter` |
| object path | an object | `/org/mpris/MediaPlayer2` |
| interface | a class the object implements | `org.mpris.MediaPlayer2` and `org.mpris.MediaPlayer2.Player` |
| method | a member function others call | `PlayPause()`, `Seek(offset)` |
| property | a member variable others read | `PlaybackStatus`, `Metadata`, `Volume` |
| signal | a notice we send to whoever's listening | `PropertiesChanged`, `Seeked` |

MPRIS is just an agreement on those names. Anything that wants to control
music players asks the bus "which names start with `org.mpris.MediaPlayer2.`?"
and calls the same methods on each. You can poke at it yourself:

```sh
busctl --user list | grep mpris
busctl --user introspect org.mpris.MediaPlayer2.scratter /org/mpris/MediaPlayer2
```

## sd-bus and the vtable

We use sd-bus, the D-Bus library in libsystemd. It's already on every systemd
Linux, it's plain C with no event loop or threads of its own, and the spec
names it. (libsecret, which the vault uses, brings GLib's GDBus along, but
GDBus wants its own GLib main loop running, which we don't have.)

With sd-bus you describe an interface as a table, a "vtable":

```cpp
const sd_bus_vtable kPlayerVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("PlayPause", "", "", simple_request<Kind::PlayPause>, 0),
    SD_BUS_METHOD("Seek", "x", "", seek, 0),
    SD_BUS_PROPERTY("PlaybackStatus", "s", get_playback_status, 0, kEmits),
    ...
    SD_BUS_VTABLE_END,
};
```

Each row is a name, the D-Bus types of its arguments (`"x"` is a 64-bit
integer, `"s"` a string, `"b"` a bool, `"d"` a double, `"a{sv}"` a dictionary of
strings to anything), and the C++ function sd-bus calls for it. When a call to
`PlayPause` arrives, sd-bus finds the row and calls `simple_request`, which
passes a `MediaRequest` to the daemon and replies.

## Properties and PropertiesChanged

A property is read by calling the getter in its row. That's how
`playerctl status` works: it asks for `PlaybackStatus`, and `get_playback_status`
answers "Playing", "Paused" or "Stopped".

But the media widget doesn't want to ask every second. So when something
changes, we send a `PropertiesChanged` signal:

```cpp
sd_bus_emit_properties_changed(bus, path, interface, "PlaybackStatus", nullptr);
```

sd-bus calls the getter itself to fill in the new value, so the value only
lives in one place. The flags in the vtable say which properties do this:
`kEmits` means "we send PropertiesChanged", `kConst` means "never changes,
cache it". `Position` has neither: it changes 100 times a second, so clients
read it when they want it, and we only send the `Seeked` signal when it jumps.
That's why `set_position()` is free to call on every tick.

## Fitting into the reactor

sd-bus's connection is one Unix socket. The daemon already has a loop waiting
on sockets (the [reactor](reactor.md)), so we hand it the socket's fd:

```cpp
reactor.watch(NativeHandle{fd}, {.readable = true}, [raw] { raw->pump(); });
```

When a message arrives, the reactor calls `pump()`, which calls
`sd_bus_process()` until it says there's nothing left. Every handler runs inside
that call, so on the reactor thread, together with IPC commands and the tick.
No thread, no lock.

Sending works the other way: sd-bus writes straight away when it can, and
queues the rest. If anything's queued, `sd_bus_get_events()` says it wants to
write, and only then do we ask the reactor to wake us on "writable".

## What the daemon does with a key press

`main_daemon.cpp` turns each `MediaRequest` into the same change an amctl
command makes: set the audio's state or volume, republish telemetry, and send
every connected client a `PlaybackStateEvent`. Going the other way, whenever
an IPC command changes something, the same `publish()` updates the media
widget. So `amctl pause` greys out the widget's pause button, and the pause key
shows up in `amctl status`.

The tone is the "track" for now, with the title "440 Hz test tone". It has no
next track and no length, so `CanGoNext`, `CanGoPrevious` and `CanSeek` are
false, and the widget greys out or hides those buttons. `CanGoNext` and `CanGoPrevious`
stay false in the MPRIS code until there's a play queue; then the interface
gets a setter for them.

A few MPRIS details that are easy to miss:

- MPRIS has no "buffering" status, so buffering shows as "Playing".
- Volume is 0.0 to 1.0 on D-Bus and 0 to 100 everywhere else, so the code
  converts, and anything above 1.0 counts as 100.
- `SetPosition` carries the track's id, and MPRIS says to ignore it if that's
  not the current track: the song may have changed while you were dragging the
  slider. Our one id is `/CurrentTrack`.
- Closing gives up the bus name before disconnecting, so a test (or a
  restarted daemon) can claim it again straight away.

## Testing

Two levels:

- `tests/test_mpris_controls.cpp` opens a second connection on another
  thread and plays the desktop: calls `PlayPause`, reads `Metadata`, sets
  `Volume`, listens for `PropertiesChanged`. Catch2's checks aren't
  thread-safe, so the client thread only records what it saw and the checks
  run after it's finished.
- `tests/test_daemon.cpp` starts the real daemon and calls `PlayPause` on it
  over D-Bus, then checks the telemetry page says Paused.

Both need a session bus. CI has none, so it runs `ctest` under
`dbus-run-session`, which starts a private bus for that one command. On a
machine with no bus at all, `open()` returns "not supported" and the tests
skip.

## Read next

- The [MPRIS spec](https://specifications.freedesktop.org/mpris-spec/latest/)
  (short; the Player interface page is the useful one).
- `man sd-bus`, and `man sd_bus_add_object_vtable` for the vtable macros.
