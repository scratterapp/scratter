# How heavy is the daemon? Measuring memory, CPU and the GPU

The spec (section 1) and `docs/RATIONALE.md` promise a light player: under
115 MB of memory with the window closed, no video memory at all, under 0.5%
CPU in the background, and commands answered in under 25 µs. This page is
about checking those promises, and about the first changes made to get closer.

## Where we stood before this change

(The [results](#results) further down have the numbers after it.)

| Goal | Measured | Verdict |
| --- | --- | --- |
| IPC round trip under 25 µs | 7.52 µs median (`bench/ipc_ping_pong`, 2026-10-14) | meets |
| No dropouts | 1 late buffer in 4 hours, at the start, 0 dropped frames (soak test, 2026-10-18) | meets |
| Memory under 115 MB, window closed | about 385 MB owned, about 2 GB if you add up every process's RSS | well over |
| 0 MB of video memory | a GPU process ran (148 MB RSS); whether it held video memory was never checked | unknown |
| Under 0.5% CPU in the background | never measured | unknown |

The 2 GB is misleading, and the next section is why. The 385 MB isn't, and it's
almost all Chromium: the daemon playing the plain tone, without CEF, uses about
4 MB.

## Three ways to count memory

Every CEF process maps the same `libcef.so`, a library of over a gigabyte, and
the parts of it in use sit in RAM **once**, shared by all of them. Linux reports
memory three ways, and they disagree because of exactly that:

- **RSS** (resident set size) is everything of one process that's in RAM,
  shared pages included. Add up the RSS of 13 CEF processes and you count the
  shared part of `libcef.so` 13 times. That's where "2 GB" comes from.
- **PSS** (proportional set size) splits each shared page between the processes
  sharing it: a page used by 4 processes counts a quarter in each. Add up PSS
  and you get what the whole thing really costs the machine. This is the number
  to hold against the 115 MB goal.
- **Own memory** (anonymous plus shared memory, what the soak test watches) is
  what the processes asked for themselves: JavaScript heaps, buffers, caches.
  It leaves out code loaded from files entirely, so it's a floor.

PSS is the fair one, but the kernel only shows it to whoever could attach a
debugger to the process. Chromium's sandboxed processes (renderer, zygote,
Widevine) refuse that, so reading their PSS takes `sudo`.

## Why fewer processes means less memory

A process costs memory even when it does nothing: its own copy of Chromium's
start-up state, its own memory allocator with its own free lists, its own
stacks, its own private pages of the libraries it uses. Chromium spends that on
purpose, because separate processes contain crashes and attacks. Some of them
earn it here (the renderer runs a web page and the Widevine process runs
Google's closed library, so both stay sandboxed).

**Spare renderers** don't. The first measurement on a real machine showed
three renderer processes, two of them idle (no wake-ups) at about 45 MB each.
Chromium keeps spare renderers warm so the next page opens faster, and we
never open a next page. `--renderer-process-limit=1` caps it at the one
renderer our page needs.

Two more looked optional and weren't, in practice:

- **The GPU process.** We pass `--disable-gpu`, but Chromium still starts a GPU
  process to run its compositor (the part that would put the page on screen)
  in software. `--in-process-gpu` runs that on a thread of the daemon instead.
- **The audio service**, which carries the page's sound to CEF's audio
  handler. `--disable-features=AudioServiceOutOfProcess` runs it in the daemon.

Together they saved about 75 MB, but then Ctrl-C ended in a crash: during
shutdown, Chromium checks that nothing still points at memory it has freed,
and with these parts in the daemon something did. Those are code paths few
Chromium users run, and a daemon that can't quit cleanly is worse than one
that's 75 MB bigger, so both keep their own processes.

Two more switches trim work rather than processes: `--disable-extensions`
skips Chromium's built-in extensions (the PDF viewer and so on), and
`--disable-background-networking` stops the requests Chromium makes on its own
(field-trial settings, safe-browsing lists). The page's own requests, the
license request included, are not "background" and still go out.

All three are in `HostApp::OnBeforeCommandLineProcessing` in
`src/daemon/cef_host/cef_host.cpp`.

## Fewer wake-ups while paused

Every time a thread wakes up, the CPU has to leave its low-power state for a
moment. One wake-up is nothing; hundreds a second, all day, show up on a laptop
battery. The daemon's tick (keep the ring buffer full, publish telemetry) ran
every 10 ms, 100 times a second, even while paused, when nothing moves.

Now it runs every 10 ms while playing and every 250 ms while paused or stopped.
`publish()` in `src/daemon/main_daemon.cpp`, which runs after every command,
calls the tick straight away, and the tick picks its next delay from the new
state. So pressing play doesn't wait for the slow timer, and pausing slows it
down at once. The reactor has one timer, and setting it again replaces the old
deadline, which is what makes that work.

## Measuring it: `tools/perf/footprint.py`

It watches a daemon that's already running and prints a line per process:

```
process                              pid   RSS MB   PSS MB   own MB  CPU %  wakeups/s  GPU
------------------------------------------------------------------------------------------
daemon                              1163      5.9      4.2      0.7    0.0         99  no
```

That's the plain tone with `--no-audio`, playing; paused, the same daemon
wakes 4 times a second. With CEF there's a line for each of Chromium's
processes too. The columns:

- **RSS, PSS, own**: the three counts above.
- **CPU %**: how much of one core the process used while the script watched,
  from the CPU time in `/proc/PID/stat` before and after.
- **wakeups/s**: how often the process's threads were switched onto a CPU each
  second (`voluntary_ctxt_switches` plus `nonvoluntary_ctxt_switches`, added
  over every thread in `/proc/PID/task`).
- **GPU**: whether the process has the graphics card open (a link to
  `/dev/dri/...` in `/proc/PID/fd`), and how much video memory it holds if the
  driver reports it (`drm-total-vram` in `/proc/PID/fdinfo`, which amdgpu and
  Intel's drivers have; NVIDIA's closed driver may not).

Under the table it holds the totals against the goals. Without `sudo` the PSS
and GPU columns say `?` for sandboxed processes. `sudo` is fine here: the
script only reads `/proc`, it doesn't start or change anything.

## Results

`footprint.py` on robbie's machine, 2026-10-19, `linux-cef` debug build,
`--test-film` (the Widevine film), before and after this change:

| | before (main) | after |
| --- | --- | --- |
| Processes | 14 | 12 |
| Renderers | 3 | 1 |
| Memory really used (PSS) | 693 MB | 664 MB |
| Memory the processes own | 498 MB | 424 MB |
| CPU while playing, all processes | 9.8% of a core | 10.3% |
| CPU while paused | not measured | 2.4% |
| Wake-ups per second while paused | not measured | 317 (the daemon's own tick: 4) |
| Video memory | 0 MB | 0 MB |

Where that leaves the goals:

- **IPC and dropouts: met**, as before.
- **Video memory: met.** The daemon and Chromium's GPU process open the
  graphics card (`/dev/dri`) but hold no video memory, because `--disable-gpu`
  means nothing is drawn on it.
- **Memory: about 6 times the goal.** 664 MB against 115 MB. Our own code is a
  few MB of that; the rest is Chromium. The biggest parts are the daemon
  process (CEF's browser side, 175 MB), the renderer running the page and
  Shaka Player (167 MB), and Widevine (101 MB).
- **CPU: about 20 times the goal while playing, 5 times while paused.**
  Decoding and moving audio through Chromium's processes costs a few percent
  of a core; nearly all of it is in Chromium, not in our code.

## What's left

The 115 MB and 0.5% goals were written before we knew what a hidden Chromium
costs. Chromium is the price of Widevine, and there's no Widevine without it, so
those goals can't be met as written. The honest options are to set new goals
from these measurements, or to look for bigger savings: a smaller JavaScript
heap for the page, pausing PipeWire's stream while paused so its thread sleeps
too, or stopping the page's network fetches while paused (the network process
still wakes about 90 times a second then). Running Chromium's GPU and audio
parts inside the daemon is off the table unless a later CEF fixes the shutdown
crash.

## What you learn

- RSS, PSS and anonymous memory, and why adding up RSS overcounts.
- Why each extra process costs memory, and which of Chromium's are optional.
- What a wake-up is and why an idle program should have few.
- Reading CPU time, context switches and open files from `/proc`.
