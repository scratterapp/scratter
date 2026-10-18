# How heavy is the daemon? Measuring memory, CPU and the GPU

The spec (section 1) and `docs/RATIONALE.md` promise a light player: under
115 MB of memory with the window closed, no video memory at all, under 0.5%
CPU in the background, and commands answered in under 25 µs. This page is
about checking those promises, and about the first changes made to get closer.

## Where we stood before this change

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
Google's closed library, so both stay sandboxed). Two don't:

- **The GPU process.** We pass `--disable-gpu`, but Chromium still starts a GPU
  process to run its compositor, the part that would put the page on screen,
  in software instead. Our page is never shown. `--in-process-gpu` runs that on
  a thread of the daemon instead.
- **The audio service.** It's how the page's sound reaches CEF's audio handler,
  and so us. It plays nothing itself (CEF hands the sound to us instead of the
  speakers). `--disable-features=AudioServiceOutOfProcess` runs it in the
  daemon too.

Two more switches trim work rather than processes: `--disable-extensions`
skips Chromium's built-in extensions (the PDF viewer and so on), and
`--disable-background-networking` stops the requests Chromium makes on its own
(field-trial settings, safe-browsing lists). The page's own requests, the
license request included, are not "background" and still go out.

All four are in `HostApp::OnBeforeCommandLineProcessing` in
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

## What's left

Even with every process trimmed, a headless Chromium playing a stream costs
far more than 115 MB; the 115 MB was written before we knew how much of the
budget Chromium would take. We'll have the real PSS numbers from robbie's
machine first, then decide whether to aim for a new target or for bigger
changes (one renderer instead of a spare, a smaller JavaScript heap, pausing
PipeWire's stream while paused so its thread sleeps too).

## What you learn

- RSS, PSS and anonymous memory, and why adding up RSS overcounts.
- Why each extra process costs memory, and which of Chromium's are optional.
- What a wake-up is and why an idle program should have few.
- Reading CPU time, context switches and open files from `/proc`.
