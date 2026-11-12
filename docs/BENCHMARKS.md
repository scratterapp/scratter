# Benchmarks

How heavy and how fast `scratterd` is, measured the same way every time, so each
improvement can be held against the day we started measuring. The table at the
top of [`SPECIFICATION.md`](SPECIFICATION.md) and [`RATIONALE.md`](RATIONALE.md)
are the design targets; this page is what we actually measured.

## How we measure

Always the same way, so the numbers compare:

- **Machine:** robbie's PC: CachyOS, 6 cores, 15 GB RAM, PipeWire 1.6.9.
- **Build:** the `linux-cef-release` preset (our code optimized; `libcef.so`
  is a release build either way). The first two lines of the log used the
  `linux-cef` debug build, before that preset existed. From 2026-10-20, CEF
  itself is an official build ([CEF.md](CEF.md#building-cef-with-aac));
  the lines before that used an unofficial one.
- **What plays:** `./build/linux-cef-release/scratterd --test-film`, the
  Widevine test film, left playing for a minute first.
- **Memory, CPU, wake-ups, GPU:** `sudo python3 tools/perf/footprint.py` in a
  second terminal (30 seconds), then `./build/linux-cef-release/amctl pause`
  and the same again for the paused numbers. Adding `--threads 5` names each
  process's threads behind the wake-ups without changing the numbers.
- **IPC:** `./build/linux-release/bench/ipc_ping_pong` (release build). On
  Windows, `build\windows-msvc-release\bench\ipc_ping_pong.exe`; the first
  run there (kero's PC, 2026-10-22, with a Chromium build compiling alongside)
  gave a median of 9.7 to 13.4 µs. Windows doesn't have a baseline of its own
  yet.
- **Our audio code alone:** `./build/linux-release/bench/audio_render`.

What the memory columns mean. Every CEF process maps the same `libcef.so`, and
the parts of it in use sit in RAM once, shared by all of them, so Linux's counts
disagree:

- **RSS** is everything of one process that's in RAM, shared pages included.
  Adding up RSS counts the shared part of `libcef.so` once per process.
- **PSS** splits each shared page between the processes sharing it. Added up, it's
  what the whole thing really costs the machine: the number to hold against the
  goal. The kernel only shows it to whoever could attach a debugger, so the
  sandboxed processes need `sudo` (the script only reads `/proc`).
- **Own memory** (anonymous plus shared memory, what the soak test watches) is
  what the processes asked for themselves: heaps, buffers, caches. It leaves out
  code loaded from files, so it's a floor.

**wakeups/s** is how often a process's threads were switched onto a CPU each
second, from `/proc/PID/task/*/status`. In the daemon's thread list, two threads
are ours: the reactor (named `scratterd`) and PipeWire's audio thread
(`pw-data-loop` or `data-loop.0`); the rest are Chromium's.

## Baseline: 2026-10-19

`main` at 5928323, before any performance work.

| Measure | Baseline |
| --- | --- |
| Memory really used (PSS, all processes, playing) | 693 MB |
| Memory the processes own (anonymous + shared) | 498 MB |
| Processes | 14 (3 renderers) |
| CPU while playing, all processes | 9.8% of one core |
| Wake-ups per second while playing | 809 |
| Video memory | 0 MB |
| IPC round trip, median | 7.52 µs (2026-10-14) |
| Dropouts in 4 hours | 1 late buffer at the start, 0 dropped frames (2026-10-18) |

The paused numbers weren't taken that day; the first ones are in the log
below. Our own code is a few MB of the memory: the daemon playing a plain
tone without CEF uses about 4 MB. The rest is Chromium, which the daemon needs
for Widevine.

## Goals

Set 2026-10-19, from the baseline. They're meant to be hard but reachable with
Chromium inside the daemon. The spec's 115 MB and 0.5% stay as the long-term
design targets.

| Measure | Baseline | Goal | Now |
| --- | --- | --- | --- |
| Memory (PSS, playing) | 693 MB | under 500 MB | 422 MB |
| CPU while playing | 9.8% | under 5% | 2.7% |
| CPU while paused | not measured | under 0.5% | 0.6% |
| Wake-ups per second while paused | not measured | under 50 | 36 |
| Video memory | 0 MB | stays 0 MB | 0 MB |
| IPC round trip, median | 7.52 µs | stays under 25 µs | 7.52 µs |
| Dropouts | 1 late buffer | stays at 0 after start-up | 0 late, 0 dropped |

"Now" is the latest line of the log. Chromium's chatter (see below the log)
can add several points to both CPU columns in a given run.

## Log

One line per change that moved a number, newest last.

| Date | Change | PSS | CPU playing | CPU paused | Wake-ups paused |
| --- | --- | --- | --- | --- | --- |
| 2026-10-19 | Baseline (`main` at 5928323, debug build) | 693 MB | 9.8% | | |
| 2026-10-19 | #26: one renderer instead of three, no extensions or background requests, the daemon's tick slows from 100 to 4 a second while paused (debug build) | 664 MB | 10.3% | 2.4% | 317 |
| 2026-10-19 | #26 measured again, just before the next line (debug build) | 667 MB | 14.4% | 8.5% | 1094 |
| 2026-10-19 | #29: our code built optimized (`linux-cef-release`). The daemon process went from 7.6% to 3.7% playing, its audio thread from 5.1% to 0.3% | 609 MB | 12.3% | 8.7% | 1156 |
| 2026-10-19 | #30: the sound output sleeps once a pause has faded out. The daemon's PipeWire thread goes from 47 wake-ups a second to none while paused | 676 MB | 4.7% | 0.6% | 36 |
| 2026-10-20 | `main` (with #30) measured again, just before the next line | 638 MB | 4.7% | | |
| 2026-10-20 | #31: Chromium's GPU process and audio service run inside the daemon. 12 processes to 9; the memory they own goes from 421 to 344 MB | 599 MB | 4.8% | | |
| 2026-10-20 | CEF rebuilt the official way (`is_official_build=true`, [CEF.md](CEF.md#building-cef-with-aac)): no developer checks, Chromium's own allocator settings for releases. Every process got smaller; the memory they own went from 344 to 256 MB | 422 MB | 2.7% | | |

Two runs of the same code can differ by a few points, mostly in Chromium's
network process: it used 2.6% in the first #29 run and 4.3% in the second.
That's why #26 was measured again right before #29, and why the daemon
process shows the change better than the total. Paused, both of those runs
were much busier than the first #26 run (8.5% against 2.4%), in the network
process and in Chromium's threads inside the daemon; the network process is
one of the ideas below. PSS came out 58 MB lower with #29, but the memory the
processes own didn't change (428 MB, then 431 MB). The difference is in pages
of shared files like `libcef.so`, which how we build our code shouldn't
affect. The #30 run was back at 676 MB with 424 MB owned, so it was noise.

Chromium's chatter didn't show up at all in the #30 run: the network process
woke twice a second and CEF's threads in the daemon fewer than 20 times,
against about 300 and 500 in the busy runs earlier that day. That, not #30,
is most of the drop in every column; #30 only changes the paused numbers. Its
own part is the daemon's PipeWire thread, which woke 47 times a second while
playing and now stops while paused instead of playing silence. Without #30
that run would have shown about 83 wake-ups a second paused instead of 36
(worked out from the thread list, not measured separately).

## The app window (am-ui)

Its own table, since the window comes and goes and its numbers don't add to the
daemon's. Measured with the window open and the mouse off it, CPU from
`/proc/<pid>/stat` over 30 s, memory from `/proc/<pid>/smaps_rollup` on Linux,
and from `Get-Process am-ui` (`TotalProcessorTime`, `WorkingSet64`,
`PrivateMemorySize64`) on Windows, which has no PSS.

| Date | Change | CPU idle | PSS | Where |
| --- | --- | --- | --- | --- |
| 2026-10-27 | #35: the first window, redrawn only after an event (debug build) | 0.0% (no frames drawn) | 102 MB | Cloud container: Xvfb and Mesa's software renderer, no GPU. Most of the memory is Mesa's LLVM; expect less with a real GPU driver |
| 2026-11-04 | #42: the same window on Windows, Release build, drawn with direct3d11, connected to `scratterd --clear-film` | 0.16% playing (one redraw a second), 0.10% paused | 63 MB working set, 72 MB private | kero's Windows 11 machine, 150% display scale. CPU from the process's processor time over 30 s. The debug build uses 1.7% playing |

While writing #35, a version that also redrew once a second used 4.8% idle on
that software renderer. The clock tick now only runs while something plays
(`FramePacer::set_ticking`).

### Start-up, and closing it while music plays

The spec's phase 5 gate: open and close the window 50 times while the daemon
plays, with no glitch in the sound, and see how fast it starts. Measured with

```sh
cmake --preset linux-cef-release
cmake --build --preset linux-cef-release
python3 tools/relaunch/relaunch.py
```

which plays `--clear-film`, opens the window 50 times (every other one closed
cleanly, the rest with `kill -9`) and writes `relaunch-results/<date-time>/summary.txt`.

On Windows, from a Developer PowerShell:

```powershell
cmake --preset windows-msvc-release
cmake --build --preset windows-msvc-release
py tools/relaunch/relaunch.py -- --tone-length 600
```

A clean close there is `WM_CLOSE`, what the window's close button sends, and
`kill -9` is `TerminateProcess`. The release preset has no CEF, so this plays
the tone. For the films, point `--build` at a CEF build and leave out the
`--`; `build/windows-cef` is a debug build, so its start-up times are slower
than a release build's would be.
"Launch" is just before the script starts `am-ui`, so the times include loading
the program; "after main()" leaves that out. Times are medians, with the
slowest in brackets. "Glitches" are the daemon's late audio buffers and dropped
frames while the windows came and went.

| Date | Change | Launch to first frame | Launch to the daemon's state | Glitches | Where |
| --- | --- | --- | --- | --- | --- |
| 2026-11-07 | #43: the first measurement, Release build, 50 of 50 windows | 89 ms (127) | 128 ms (178) | 0 late, 0 dropped | Cloud container: SDL's offscreen driver and software renderer, no GPU, the tone (`--tone-length 600`) on a PipeWire null sink instead of the film. About 40 ms is creating the software renderer and about 40 ms is drawing the first frame, most of it rasterizing the fonts' glyphs. Not robbie's PC: a real GPU should be quicker at the drawing but slower to create the context |
| 2026-11-07 | #43 on robbie's PC, `linux-cef-release`, `--clear-film`, 50 of 50 windows | 94 ms (121) | 99 ms (127) | 0 late, 0 dropped | robbie's PC, Hyprland (Wayland). 90 ms of the first frame is after main(): making the window, its GPU renderer and the first frame with its fonts. The daemon's state follows about 5 ms later |

The target was 5 ms in the spec's first draft and is now a median under
100 ms from launch to the daemon's state, met on robbie's PC
([SPECIFICATION.md](SPECIFICATION.md), Phase 5). The part the 5 ms was meant
for, connecting to the daemon and reading its telemetry, is the roughly 5 ms
between the first frame and the one with the daemon's state; the window, its
GPU renderer and the fonts are the other 90 ms. That's the place to look if
start-up ever needs to get quicker: SDL_CreateRenderer alone took about 40 ms
in the cloud.

## Ideas to try next

Guesses, not measurements, roughly cheapest first:

- **Find what makes Chromium chatter in some runs.** In some runs the network
  process wakes about 300 times a second and three of CEF's threads in the
  daemon about 500 between them, playing and paused alike, which costs
  several points of CPU (`footprint.py --threads 5` shows it). In others,
  like the #30 run, they're nearly silent. A 3-second `strace` of the network
  process during a quiet run found nothing to explain it, so the next step is
  one during a busy run. One unchecked guess is the system reporting network
  changes over and over (`ip monitor` would show that). It isn't Shaka loading
  ahead: in a cloud Chromium (Shaka 4.11.2), Shaka downloaded a piece every
  4 seconds while playing and nothing at all once paused.
- **A smaller JavaScript heap** for the page, which runs in the renderer
  (167 MB).
- **A faster resampler.** Optimized, our audio code takes about 0.3% of a
  core (`bench/audio_render`). SIMD (working on 4 or 8 samples per
  instruction) could cut that further, but it's a small share now.
- **Chromium's network service in the daemon**
  (`--enable-features=NetworkServiceInProcess2`), up to about 50 MB, at the
  cost of the sandbox around the code that reads data from the internet,
  which would then sit next to the Apple Music login.
