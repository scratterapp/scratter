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
  itself is an official build ([learning/cef-host.md](learning/cef-host.md#building-cef-with-aac));
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

[`learning/performance.md`](learning/performance.md) explains what each number
means.

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
| 2026-10-20 | CEF rebuilt the official way (`is_official_build=true`, [learning/cef-host.md](learning/cef-host.md#building-cef-with-aac)): no developer checks, Chromium's own allocator settings for releases. Every process got smaller; the memory they own went from 344 to 256 MB | 422 MB | 2.7% | | |

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
`/proc/<pid>/stat` over 30 s, memory from `/proc/<pid>/smaps_rollup`.

| Date | Change | CPU idle | PSS | Where |
| --- | --- | --- | --- | --- |
| 2026-10-27 | #35: the first window, redrawn only after an event (debug build) | 0.0% (no frames drawn) | 102 MB | Cloud container: Xvfb and Mesa's software renderer, no GPU. Most of the memory is Mesa's LLVM; expect less with a real GPU driver |

While writing #35, a version that also redrew once a second used 4.8% idle on
that software renderer. The clock tick now only runs while something plays
(`FramePacer::set_ticking`).

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
  ahead: in a cloud Chromium, Shaka stops downloading once paused
  ([learning/performance.md](learning/performance.md#what-the-page-does-while-paused)).
- **A smaller JavaScript heap** for the page, which runs in the renderer
  (167 MB).
- **A faster resampler.** Optimized, our audio code takes about 0.3% of a
  core (`bench/audio_render`). SIMD (working on 4 or 8 samples per
  instruction) could cut that further, but it's a small share now.
- **Chromium's network service in the daemon**
  (`--enable-features=NetworkServiceInProcess2`), up to about 50 MB, at the
  cost of the sandbox around the code that reads data from the internet
  ([learning/performance.md](learning/performance.md#what-else-could-go)).
