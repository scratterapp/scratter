# The CEF host: a hidden browser inside the daemon (#76)

The #49 experiment proved that a CEF we build with AAC turned on can play
Widevine-protected AAC audio and hand us the decoded sound. This is the real
version of that experiment, inside the daemon:

```
scratterd --test-film
  └─ CEF (an invisible Chromium)
       └─ the built-in page plays Google's Widevine test film
            └─ CefAudioHandler gets the decoded audio
                 └─ ring buffer → drift controller → PipeWire → your speakers
```

`amctl pause`, the media keys and `playerctl` pause the film the same way they
pause the tone. Apple Music itself comes in #77, once there's a MusicKit key.

Files:

| File | What it is |
| --- | --- |
| `src/daemon/cef_host/cef_host.hpp` | The `CefHost` class `main_daemon.cpp` uses. No CEF types in it. |
| `src/daemon/cef_host/cef_host.cpp` | The real host: settings, the page, the audio handler. Built only with CEF on. |
| `src/daemon/cef_host/cef_host_off.cpp` | Stands in when CEF is off; `start()` says how to turn it on. |
| `src/daemon/cef_host/cef_helper_main.cpp` | The helper program CEF runs for its subprocesses. |
| `src/daemon/cef_host/test_film_page.hpp` | The built-in page, as a string. |
| `src/pal/interfaces/browser_files.hpp` | Where the profile folder and the installed Widevine are, per OS. |
| `cmake/FindCEF.cmake` | Finds our CEF build and builds CEF's C++ wrapper. |

## Turning it on

CEF only exists on a machine that built it (hours of compiling, see the
`experiment/cef-widevine` branch's `build_cef_with_aac.sh`), so the daemon
builds without it unless you ask. The `AM_WITH_CEF` CMake option does that,
and the `linux-cef` preset turns the option on:

```sh
cmake --preset linux-cef
cmake --build --preset linux-cef
./build/linux-cef/scratterd --test-film
```

`cmake/FindCEF.cmake` looks in `third_party/cef/linux64`, the link the build
script leaves. The build copies `libcef.so` and CEF's resource files (`.pak`
files, ICU data, `locales/`) into `build/linux-cef/`, because CEF looks for them
next to the program. That's a few hundred MB, copied once.

Without CEF, `--test-film` stops with "this daemon was built without CEF" and
everything else works as before. CI builds both ways: the usual jobs without
CEF, and a `linux-cef` job that downloads Google's prebuilt CEF of the same
version. That one can't decode AAC, but it compiles and links the same, so the
CEF code can't quietly stop building.

## CEF is several programs, not one

Chromium splits a browser into processes so a crash or an attack in one part
can't take the rest down. CEF keeps that:

| Process | What it does here |
| --- | --- |
| browser | The daemon itself. Runs CEF's UI thread and our `PageClient`. |
| renderer | Runs the page: HTML, JavaScript, Shaka Player. |
| GPU | Drawing. We turn the GPU off, but Chromium still keeps a small one. |
| utility: audio | Decodes and mixes the page's sound, and sends it back to the browser process. |
| utility: CDM | Runs Google's Widevine library to decrypt the film. |
| zygote | A template process the others are copied from (Linux only, see below). |

Every one of these is started by running a program with `--type=renderer`,
`--type=utility` and so on. By default that program is the browser's own
executable, which would mean the daemon's `main()` has to check "am I really
a CEF subprocess?" before reading its own arguments. Instead we point
`CefSettings::browser_subprocess_path` at a separate, tiny program,
`scratterd-cef-helper`, whose `main()` does nothing but hand control to CEF.

## The "stack smashing detected" crash, and why the helper fixes it

In the #49 experiment two helper processes printed
`*** stack smashing detected ***` when playback stopped. It wasn't our code
writing past an array. It was this:

1. **Stack canaries.** GCC (with Arch's default flags) protects functions by
   putting a secret random number, the "canary", on the stack when a function
   starts, and checking it's still there when the function returns. If a buffer
   overflow had overwritten it, the check fails and glibc kills the program
   with "stack smashing detected".
2. **The zygote.** Starting a fresh Chromium process is slow, so on Linux
   Chromium starts one "zygote" process early and later makes renderers and
   utility processes by `fork()`ing it, which copies the process in an instant.
3. **The catch.** Copies would all share one canary, which makes guessing it
   easier for an attacker. So Chromium gives each forked child a new canary.
   But `main()` was already running when the zygote forked. It saved the old
   canary on its stack. When the child finishes and `main()` returns, the check
   compares the old value with the new one, they differ, and glibc reports
   stack smashing.

The fix is to tell the compiler not to put a canary in `main()`, which is what
CEF's own samples do: `NO_STACK_PROTECTOR int main(...)`. In the experiment,
`main()` was the probe's whole program (it had strings on the stack, which is
what makes GCC add a canary). Now the helper's `main()` is three lines and
carries the attribute. The daemon's own `main()` is never forked by the zygote,
so it doesn't need it.

## The sandbox

The renderer and the CDM run untrusted code (a web page, a closed-source
library), so Chromium locks them in a sandbox: they can't open files, start
programs or make most system calls. Widevine expects to run sandboxed too
(spec 3.1.1), so the daemon turns it on. The experiment ran with it off.

On Linux the sandbox uses **user namespaces**, a kernel feature that lets a
normal user give a process its own empty view of the system. Arch and CachyOS
allow it. If a machine doesn't, Chromium falls back to `chrome-sandbox`, a
helper that must be owned by root with the setuid bit, which we don't set up.
If CEF fails to start its processes and the log mentions the sandbox,
`--no-sandbox` turns it off to check whether that's the cause.

## Pointing CEF at Widevine without copying it

We never ship, copy or download the Widevine CDM. On Linux, Chromium looks for
a CDM it didn't come with through a small "hint" file in its profile folder,
`WidevineCdm/latest-component-updated-widevine-cdm`, normally written by its
own updater after downloading Widevine. The daemon writes that file itself:

```json
{"Path": "/usr/lib/chromium/WidevineCdm"}
```

The folder comes from `--widevine-dir`, or else from the first of a short list
of places browsers install it (`src/pal/linux/browser_files.cpp`). Chromium
reads the hint at startup and loads the CDM into the zygote before the sandbox
closes, which is why this has to happen before `CefInitialize()`.

Chromium's updater would also download Widevine and other components on its
own, so the daemon passes `--disable-component-update`.

The profile folder is `~/.cache/scratter/browser` (from the product
name). It holds cookies, the browser cache, the hint file, and `cef.log`,
Chromium's own log, which is the first place to look when CEF misbehaves.

## Serving the page without a web server

DRM only works on a "secure origin". A `file://` page isn't one ("EME use is
not allowed on unique origins", the experiment found), so the experiment ran a
little Python web server on localhost. The daemon does it in-process:
`CefRegisterSchemeHandlerFactory("https", "player.invalid", ...)` tells CEF
"when anyone asks for `https://player.invalid/...`, ask me, not the network".
`PageSchemeHandlerFactory` answers with the page from `test_film_page.hpp`.
`.invalid` is a name reserved so it can never be a real website.

## Threads

Before CEF, the daemon had two threads: the reactor (main) thread and
PipeWire's audio thread. CEF adds its own, and two of them matter here:

- **CEF's UI thread.** CEF normally wants the program's main thread for its
  message loop, but that's the reactor's. `multi_threaded_message_loop = true`
  makes CEF run its loop on a thread of its own. Everything in `PageClient`
  except the audio packets runs there. The reactor thread talks to it with
  `CefPostTask(TID_UI, ...)`: run this function over there.
- **CEF's audio thread.** `OnAudioStreamPacket()` runs about every 10 ms with
  512 frames, and copies them into the ring buffer with
  `AudioPath::write_planar()`. Same rule as the PipeWire callback: no
  allocating, locking or system calls.

So the ring buffer now has a different producer. With the tone, the reactor's
10 ms tick writes into it (`top_up()`). With the film, CEF's audio thread does,
and the tick leaves it alone. The ring buffer has room for exactly one
producer, so the daemon never runs both.

Signals: Chromium normally unblocks Ctrl-C and SIGTERM and installs its own
handlers. That would steal them from the daemon's `signalfd`
(`shutdown_signal.hpp`), so `disable_signal_handlers = true` tells it not to.

## Audio: planar to interleaved, at the sink's rate

CEF hands over audio **planar**: one array per channel, `data[0]` all left
samples, `data[1]` all right. The ring buffer and PipeWire use **interleaved**:
L R L R. `write_planar()` interleaves as it copies (a plain loop; the spec's
SIMD version can come later if a benchmark says it matters).

`GetAudioParameters()` asks Chromium for stereo at the sink's sample rate
(48 kHz on most machines) in 512-frame packets, so Chromium resamples the film
once and our resampler only does the small drift corrections it was built for.

One change to `AudioPath::render()` came with this: it now waits until the ring
buffer holds 4096 frames (about 85 ms) before it starts playing, and again
after it runs dry. The tone filled the ring before playback started, so it
never noticed. CEF's audio arrives in real time, about as fast as PipeWire
takes it, so without the wait the ring would sit near empty and every buffer
would be late. Frames that arrive when the ring is full are dropped and counted
in `dropped_frames()`.

Pausing needed one more rule. A page doesn't stop the moment it's told to
pause, and Chromium keeps its audio stream running for a while after, so
sound keeps arriving. At first the daemon kept it for later: on the first hand
test that overflowed the ring buffer (45056 frames dropped), and pressing play
replayed that stale sound, ran dry, cut out, and came back. Now `render()`
throws away whatever reaches the ring buffer while paused or stopped, and on
play waits for ~85 ms of fresh sound before it starts. The cost is skipping
about the last 0.1 s before the pause, which you don't notice.

## Play, pause, stop

amctl and the media keys change `AudioPath`'s state as before. With a page
playing, the daemon also calls `CefHost::set_state()`, which runs `amPlay()`,
`amPause()` or `amStop()` in the page, so the film stops producing audio
instead of overflowing the ring buffer.

## Shutting down

`~CefHost()` asks CEF's UI thread to close the browser, waits up to 5 seconds
for `OnBeforeClose()`, then calls `CefShutdown()` from the reactor thread (CEF
must be shut down from the thread that started it). The daemon does this right
after the media keys go away and before the audio sink stops, so nothing
writes into the ring buffer once it's being freed.

## Hand test

On the machine with the AAC build, open a terminal:

```sh
git fetch origin
git switch feature/cef-host
cmake --preset linux-cef
cmake --build --preset linux-cef
./build/linux-cef/scratterd --test-film
```

Expect "using the Widevine CDM in ...", "CEF ... started (sandbox on)", then
"page: playing the Widevine test film, audio mp4a.40.2", and the film's
soundtrack from your speakers about a second later. Then, in a second terminal
window:

```sh
./build/linux-cef/amctl pause
./build/linux-cef/amctl play
./build/linux-cef/amctl volume 30
./build/linux-cef/amctl status
```

Back in the first window, Ctrl-C should end with "stopped cleanly" and no
"stack smashing detected" anywhere above it.

Known: Google's test license server sometimes refuses a second license request
("ACCESS_DENIED") after a few seconds of play, and the film stops. That's
Google's test server, not us; it happened in the experiment too.

## What you learn

- How a Chromium-based program is really several processes, and why.
- Stack canaries, `fork()`, and why a crash message can point at the wrong
  culprit.
- What a sandbox is and how Linux user namespaces make one.
- Running a library's event loop on its own thread next to yours, and handing
  work between threads with posted tasks.
- A second producer for the same lock-free ring buffer, and why only one may
  run at a time.
- Keeping an optional, heavy dependency behind a build option so everyone else
  can still build.
