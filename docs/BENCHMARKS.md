# Benchmarks

How heavy and how fast `scratterd` is, measured the same way every time, so each
improvement can be held against the day we started measuring. The table at the
top of [`SPECIFICATION.md`](SPECIFICATION.md) and [`RATIONALE.md`](RATIONALE.md)
are the design targets; this page is what we actually measured.

## How we measure

Always the same way, so the numbers compare:

- **Machine:** robbie's PC: CachyOS, 6 cores, 15 GB RAM, PipeWire 1.6.9.
- **Build:** the `linux-cef` preset (our code in a debug build; `libcef.so` is
  a release build either way).
- **What plays:** `./build/linux-cef/scratterd --test-film`, the Widevine
  test film, left playing for a minute first.
- **Memory, CPU, wake-ups, GPU:** `sudo python3 tools/perf/footprint.py` in a
  second terminal (30 seconds), then `./build/linux-cef/amctl pause` and the
  same again for the paused numbers. Adding `--threads 5` names the threads
  behind the wake-ups without changing the numbers.
- **IPC:** `./build/linux-release/bench/ipc_ping_pong` (release build).

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
| Memory (PSS, playing) | 693 MB | under 500 MB | 664 MB |
| CPU while playing | 9.8% | under 5% | 10.3% |
| CPU while paused | not measured | under 0.5% | 2.4% |
| Wake-ups per second while paused | not measured | under 50 | 317 |
| Video memory | 0 MB | stays 0 MB | 0 MB |
| IPC round trip, median | 7.52 µs | stays under 25 µs | 7.52 µs |
| Dropouts | 1 late buffer | stays at 0 after start-up | 0 late, 0 dropped |

## Log

One line per change that moved a number, newest last.

| Date | Change | PSS | CPU playing | CPU paused | Wake-ups paused |
| --- | --- | --- | --- | --- | --- |
| 2026-10-19 | Baseline (`main` at 5928323) | 693 MB | 9.8% | | |
| 2026-10-19 | #26: one renderer instead of three, no extensions or background requests, the daemon's tick slows from 100 to 4 a second while paused | 664 MB | 10.3% | 2.4% | 317 |

## Ideas to try next

Guesses, not measurements, roughly cheapest first:

- **Build our code with optimizations.** The resampler runs inside the
  daemon, in PipeWire's audio thread, for every sample. In a debug build it
  runs unoptimized, so some of the daemon's 5.6% while playing may be ours.
  A release build of the `linux-cef` preset would show how much.
- **Find what wakes the network process while paused.** It wakes about 90
  times a second paused and 3 playing. It isn't Shaka loading ahead: in a
  cloud Chromium, Shaka stops downloading once paused
  ([learning/performance.md](learning/performance.md#what-the-page-does-while-paused)).
  `footprint.py --threads` shows which threads it is.
- **A smaller JavaScript heap** for the page, which runs in the renderer
  (167 MB).
- **Ask CEF's forum about the shutdown crash.** Running Chromium's GPU and
  audio parts inside the daemon saved about 75 MB, but made CEF abort on exit
  ([learning/performance.md](learning/performance.md)). A newer CEF may not.
