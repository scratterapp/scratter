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

Two more looked optional:

- **The GPU process.** We pass `--disable-gpu`, but Chromium still starts a GPU
  process to run its compositor (the part that would put the page on screen)
  in software. `--in-process-gpu` runs that on a thread of the daemon instead.
- **The audio service**, which carries the page's sound to CEF's audio
  handler. `--disable-features=AudioServiceOutOfProcess` runs it in the daemon.

Together they save about 75 MB. The first try made Ctrl-C end in a crash, so
they were dropped at first; the next section is why that crash doesn't count,
and both now run in the daemon.

Two more switches trim work rather than processes: `--disable-extensions`
skips Chromium's built-in extensions (the PDF viewer and so on), and
`--disable-background-networking` stops the requests Chromium makes on its own
(field-trial settings, safe-browsing lists). The page's own requests, the
license request included, are not "background" and still go out.

All of these are in `HostApp::OnBeforeCommandLineProcessing` in
`src/daemon/cef_host/cef_host.cpp`.

## The shutdown crash was a developer check

With the GPU and audio in the daemon, Ctrl-C ended in this, from inside
Chromium's own shutdown:

```
A freed allocation is still referenced by a dangling pointer at exit ...
FATAL: Check failed: !errors.
```

A **dangling pointer** points at memory that has already been freed. Using one
is a classic security bug, so Chromium has a detector for them: while it runs,
every free is checked against the pointers still pointing at it, and at exit
it aborts if it found any. Here it found one in Chromium's code while the
whole process was quitting, when nothing could use it any more.

The detector only exists in **unofficial** builds. Chromium's build has a
switch, `is_official_build`, that Google and CEF turn on for what they ship:
it turns on the strongest optimizations and leaves out the checks meant for
Chromium's own developers (`DCHECK`s, "debug checks", and this detector among
them). Our CEF build (`build_cef_with_aac.sh`) left it off, so it has them.
CEF's official builds would never have crashed.

So the daemon turns the detector off with
`--disable-features=PartitionAllocDanglingPtr`. That hides nothing of ours:
the pointer is Chromium's, and a release build would never check it.

It also means our CEF build pays for those developer checks everywhere, in
every process. Rebuilding CEF with `is_official_build=true` is the way to find
out how much; the [ideas list](../BENCHMARKS.md#ideas-to-try-next) has it.

## What else could go

Where the rest of the memory goes, roughly, and whether it can go:

| Part | About | Can it go? |
| --- | --- | --- |
| Our own code | 4 MB | Already small. |
| The daemon process (CEF's browser side) | 175 MB | No. It's the browser itself. |
| The renderer (the page, its JavaScript) | 167 MB | Some. A smaller JavaScript heap or fewer JavaScript compilers (`--js-flags`) might save tens of MB, but the real page with MusicKit JS doesn't exist yet, so tuning the test page now could be tuning the wrong thing. |
| Widevine (the CDM process) | 100 MB | No. Google's closed library has to run in its own sandboxed process. |
| The network service | 50 MB | Yes, with `--enable-features=NetworkServiceInProcess2`, but it's the code that reads everything arriving from the internet, and in its own process it's sandboxed. Inside the daemon it would sit next to the Apple Music login. |
| The storage service | 18 MB | No switch for it that we found. |
| The zygotes | small | Only without the sandbox (`--no-sandbox`), which isn't worth it. |

The daemon process and the renderer count some of `libcef.so` that's shared
with the others, so the rows don't add up exactly.

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

## Letting the sound output sleep too

The tick wasn't the only thing awake. PipeWire runs its audio thread in our
process, and it kept asking for a buffer every cycle while paused. With
PipeWire's usual cycle of 1,024 frames, 48,000 frames a second is about 47
wake-ups a second, each one writing silence. The sound card behind it stayed busy too, playing that
silence.

So now the daemon stops the sink while nothing plays (`sink->stop()`, which is
`pw_stream_set_active(false)` on Linux) and starts it again on play. PipeWire's
thread then sleeps, and after a few seconds of nobody playing anything,
PipeWire's session manager suspends the sound card as well. The tone daemon in
a cloud container went from 50 wake-ups a second while paused to 4, the
daemon's own tick. On robbie's machine with the Widevine film, PipeWire's
thread in the daemon (`data-loop.0`) woke 47 times a second while playing and
was gone from the list once paused, and all the processes together woke 36
times a second while paused.

Two things had to be right for that:

- **No click.** Pausing fades out over 10 ms ([fade.md](fade.md)). Stopping
  the sink in the middle of that would cut the sound off mid-wave, which is
  the click the fade exists to avoid. So the tick waits for
  `AudioPath::quiet()`, which turns true once `render()` has seen the pause
  and finished fading out. The tick runs every 250 ms while paused, so the sink
  stops within a quarter of a second of pausing. Play runs the tick at once,
  so the sink starts again straight away.
- **Nothing stale on resume.** While paused, `render()` used to throw away
  whatever the page still sent (Chromium keeps sending for a second or two
  after a pause). With the sink stopped, `render()` isn't called at all, so
  the ring buffer would fill with that old audio, and play would start with
  it. Now `write_planar()` drops the page's audio itself while paused.

`quiet()` compares two counters instead of reading a true/false flag.
`set_state()` counts every change of state, and `render()` writes down the
count it saw whenever it finishes a buffer quiet. The sink only stops when the
count `render()` wrote down is the current one. That matters because
`render()` runs on PipeWire's thread, and a buffer can be half done when you
press play and pause again: that buffer saw the older count, so it can't
trick the daemon into stopping the sink while it's still making sound. It also
means a Stop after a long pause starts the sink again for a moment, so that
`render()` sees the Stop and rewinds the position to 0.

## What the page does while paused

The first guess for the network process's wake-ups while paused (about 90 a
second on robbie's machine, against 3 while playing) was that Shaka Player
keeps downloading ahead. A check in a cloud Chromium says no: with Shaka
4.11.2 playing an Opus copy of a Shaka demo film (that Chromium can't decode
the test film's AAC), Shaka downloaded a piece every 4 seconds while playing,
keeping about 10 seconds ahead, and nothing at all once paused. Its network
process woke about once a second either way.

So whatever wakes CEF's network process while paused, it isn't the page
asking for more film. `footprint.py --threads` names the threads that wake up,
which is the next clue.

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

Last, it lists the daemon process's busiest threads, from each thread's CPU
time in `/proc/PID/task/TID/stat`. With CEF, the daemon process is also
Chromium's browser process, so most of its threads are Chromium's. Two are
ours: the main thread (the reactor, named `scratterd`) and PipeWire's audio
thread (`pw-data-loop` or `data-loop.0`, depending on PipeWire's version),
where `render()` runs. That's how to tell our CPU from Chromium's.

That list is by CPU and only for the daemon. `--threads 5` adds every
process's five threads that woke most often, under its line in the table, by
name (`/proc/PID/task/TID/comm`):

```
daemon                              2916      9.4      5.2      1.9    4.3        146  no
  scratterd                                                                        99
  pw-data-loop                                                                     47
```

That's the tone daemon playing through PipeWire. Chromium names its threads
too, like `Chrome_IOThread` or `ThreadPoolForeg` (Linux keeps only the first
15 letters).

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

## Debug and optimized builds

The `linux-cef` preset builds our code as a **debug build**: the compiler
turns each line into machine code as written, and puts every value back in
memory after each step so a debugger can show it. That makes a crash easy to
follow in `gdb`, and the code slow. Every `std::span` index and every
`coefficient()` call in the resampler stays a real function call.

An **optimized build** (CMake's Release type, `-O3`) lets the compiler rewrite
the code into something faster that does exactly the same: small functions are
pasted into their callers, values stay in the CPU's registers, loops are
unrolled. The source is the same, only the compiler settings differ.
`linux-cef-release` is `linux-cef` with that one change. `libcef.so` doesn't
change at all; it's Chromium's own optimized build either way.

Most of our code runs a few times a second, so how it's built hardly matters.
The audio path is the exception. `render()` runs on PipeWire's audio thread
for every frame of sound, 48,000 a second, and for each one the resampler does
64 multiply-adds (16 taps, two rows of its table, two channels). In a debug
build that makes it the biggest CPU user we own.

`bench/audio_render` measures just that: it pushes a simulated minute through
`write_planar()` and `render()` as fast as it can, then says what share of one
core that would take in real time. On a 4-core cloud machine with GCC 13:

| Build | CPU per second of audio |
| --- | --- |
| debug (`linux-debug`) | 3.4% of one core |
| optimized (`linux-release`) | 0.3% |

And the daemon playing its tone into PipeWire on the same machine, with
`footprint.py`:

| | debug | optimized |
| --- | --- | --- |
| the whole daemon | 3.9% | 1.0% |
| PipeWire's audio thread, running `render()` | 3.4% | 0.6% |
| the reactor (the tone and telemetry) | 0.5% | 0.4% |

The audio thread costs a little more than the benchmark because PipeWire does
its own work on it too.

On robbie's machine, with CEF playing the Widevine film, the two builds one
after the other:

| | `linux-cef` (debug) | `linux-cef-release` |
| --- | --- | --- |
| PipeWire's audio thread, running `render()` | 5.1% | 0.3% |
| the whole daemon process | 7.6% | 3.7% |
| all processes | 14.4% | 12.3% |

All processes together moved less than the daemon, because Chromium's network
process happened to be busier during the second run (2.6% against 4.3%).
[`../BENCHMARKS.md`](../BENCHMARKS.md) has the full log.

So: build with `linux-cef-release` to listen and to measure (every number in
BENCHMARKS.md from here on uses it), and with `linux-cef` when something
crashes and you want `gdb` to show the line.

## What's left

The 115 MB and 0.5% goals were written before we knew what a hidden Chromium
costs. Chromium is the price of Widevine, and there's no Widevine without it, so
those goals can't be met as written. [`../BENCHMARKS.md`](../BENCHMARKS.md)
keeps the dated baseline, nearer goals, a log of each change that moved a
number, and the ideas to try next.

## What you learn

- RSS, PSS and anonymous memory, and why adding up RSS overcounts.
- Why each extra process costs memory, and which of Chromium's are optional.
- What an official build of Chromium leaves out, and why a crash that only
  an unofficial build has can be a developer check rather than a real bug.
- What a wake-up is and why an idle program should have few.
- Reading CPU time, context switches and open files from `/proc`.
- What a debug build costs, and why it shows in the audio path first.
- Stopping a sound stream without a click, and why a counter beats a flag
  when another thread may be halfway through its work.
