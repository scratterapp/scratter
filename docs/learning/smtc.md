# Media keys on Windows (SMTC), explained

This covers `src/pal/win32/smtc_controls.cpp` (issue #9): the Windows side of
the `MediaControls` interface from [media-controls.md](media-controls.md). It
puts the daemon in the Windows 11 media flyout (the box that pops up when you
press a volume or media key) and on the lock screen, and makes the keyboard's
play/pause key reach it. The Linux side is [mpris.md](mpris.md).

## What SMTC is

`SystemMediaTransportControls` (SMTC) is the Windows API a player uses to say
"I'm playing this" and to hear "the user pressed pause". Every player you see
in the flyout (Spotify, a browser tab, Apple Music) has one. It's a WinRT API,
the object model Windows' newer APIs use: COM underneath, with classes looked
up by name (`RoGetActivationFactory`) and strings as `HSTRING`s.

Two directions, like MPRIS:

- **Out:** a `DisplayUpdater` for the title, artist, album and cover art, a
  `PlaybackStatus` (playing, paused, stopped), and the timeline (length and
  position) for the flyout's progress bar.
- **In:** a `ButtonPressed` event (play, pause, stop, next, previous) and a
  `PlaybackPositionChangeRequested` event (dragging the progress bar).

The keyboard has one play/pause key, but SMTC has no "toggle" button: Windows
looks at the status we last set and sends Play or Pause. So the daemon never
gets `MediaRequest::Kind::PlayPause` on Windows, and it matters that the
status is kept up to date.

## Why there's a hidden window

A store app gets SMTC from its view. A desktop program like ours gets it for a
window: `ISystemMediaTransportControlsInterop::GetForWindow(hwnd)`. The daemon
has no window, so it makes one that's never shown.

The plan was a *message-only* window (`HWND_MESSAGE`), which is invisible to
everything else on the desktop. `GetForWindow` refuses one: it returns
`E_INVALIDARG`. So it's an ordinary top-level window with no `WS_VISIBLE`, plus
`WS_EX_TOOLWINDOW` to keep it out of Alt-Tab.

A top-level window gets **broadcast messages**: "a setting changed", "the
session is ending". A program that broadcasts with `SendMessage` waits for
every top-level window to answer. A window whose thread never reads its
messages makes those programs wait. The daemon's main thread sits in the IOCP
reactor, which doesn't read window messages, so the window gets its own
thread, the **window thread**, running a normal `GetMessage` loop.

The window also ignores `WM_CLOSE`. `taskkill` without `/F` sends it to every
window a program has, and closing this one would only lose the media keys.

## Three threads, and getting back to the reactor

| Thread | What it does |
| --- | --- |
| reactor thread | The daemon's own. Calls `open()`, the setters, and runs the request callback. |
| window thread | Owns the hidden window and every SMTC object. Shows what the setters asked for. |
| WinRT pool threads | SMTC raises its events on these. |

**Setters → window thread.** A setter writes what it wants shown into a
`Wanted` struct under a mutex, and posts the window one message (two setters in
a row still post one). The window thread copies `Wanted`, compares version
numbers with what it showed last, and updates only what changed.
`set_position()` runs on every tick, so it only stores an atomic; the window
thread reads it when it next sends the timeline, which it does on every change
and every 5 seconds while playing.

**Events → reactor thread.** The interface promises the callback runs on the
reactor thread, and the reactor has exactly one door for other threads,
`wake()`, which the Ctrl-C handler already uses (`console_shutdown.cpp`). So
the controls bring their own door: a pipe.

```
WinRT thread                               reactor thread
ButtonPressed(Play)                        
  -> Inbox.push(Play)                      
     first one since the last drain?       
     -> WriteFile(pipe, 1 byte) ---------> ReadFile completes (IOCP)
                                           -> on_pipe(): take everything in the Inbox,
                                              start the next ReadFile,
                                              call on_request(Play)
```

The reactor watches the pipe's read end with one overlapped `ReadFile` always
waiting, the same way the IPC channel watches its named pipe. That's the old
*self-pipe trick* from Unix, done with a completion port. The pipe has a
unique name, only this user can open it, and it allows one instance, which
our own write end takes at once.

The `Inbox` (the queue, its mutex and the write end) is held by a
`shared_ptr` that the event handlers also hold. When the controls close, an
event that's already running on a pool thread can still finish safely: it
writes into an `Inbox` that's still alive, even though nobody will read it.

## Why not C++/WinRT

The interface header and the spec said C++/WinRT, Microsoft's modern C++
wrapper for WinRT. But C++/WinRT reports every failure by **throwing**
`winrt::hresult_error`, and this project builds with exceptions off. So
`smtc_controls.cpp` calls the raw ABI interfaces (`ABI::Windows::Media::...`)
through **WRL**, the older template library: `ComPtr` for references,
`HString` for strings, `Callback<>` to wrap a lambda as an event handler.
Every call returns an `HRESULT`, which we check.

It's more typing (activating a `Uri` takes a factory, a `CreateUri` call and two
`ComPtr`s), but it's the same API underneath, and it compiles with
`/EHs-c- /GR- /W4 /WX` without complaint.

## What the daemon side looks like

| Daemon calls | SMTC |
| --- | --- |
| `set_track` | `DisplayUpdater`: music type, title, artist, album; thumbnail from the artwork URL through `RandomAccessStreamReference::CreateFromUri`; then `Update()` |
| `clear_track` | `DisplayUpdater.ClearAll()` |
| `set_playback_state` | `PlaybackStatus` (buffering shows as playing, as on Linux) |
| `set_position`, `seeked` | the timeline: 0 to the track's length, seekable over all of it. No length means all zeros, which hides the bar |
| `set_volume` | nothing: SMTC has no volume |

Next and previous are switched off, as on Linux, until there's a queue.

Windows fetches the cover art from the link itself, and only shows a thumbnail
once the image has loaded: a link to a real PNG shows up, while one that 404s
is left out. So the test passes a link but doesn't check for the thumbnail,
which would need the network.

## Testing it

`tests/test_media_controls.cpp` is the shared contract (the setters, moving,
reopening). `tests/test_smtc_controls.cpp` plays the desktop's side, like
`test_mpris_controls.cpp` does with a second D-Bus connection on Linux. It uses
`GlobalSystemMediaTransportControlsSessionManager`, the API the flyout itself
reads, to:

1. find its own session by a title nobody else has,
2. check the status and the timeline length the flyout would show,
3. call `TryPlayAsync`, `TryPauseAsync` and `TryChangePlaybackPositionAsync` on
   that one session, which is what clicking the flyout does, and check that
   Play, Pause and a seek to 90 s arrive on the reactor thread.

It only ever talks to its own session, so a music player running at the same
time is left alone. On a machine with no desktop session it prints
`AM_TEST_SKIPPED`, and ctest reports it as skipped.

By hand: run `scratterd` and press the keyboard's play/pause key, or open the
flyout. It shows "440 Hz test tone" by "scratter" under `scratterd.exe`.

## Read next

- Microsoft's "Manual control of the System Media Transport Controls" page,
  for the store-app version of the same API.
- [mpris.md](mpris.md), for the same interface over D-Bus.
