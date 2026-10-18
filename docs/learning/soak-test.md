# The soak test: does memory stay flat? (#2)

Half of the phase 3 gate (spec section 6) is "4 hours of playback with flat
memory". A program that leaks loses a little memory every time it does
something, say every audio packet or every license renewal. A short test never
notices; after a few hours, or a few days of a daemon that's always on, the
machine does. A **soak test** is the cure: leave the thing running for a long
time and watch.

`tools/soak/soak.py` does the watching:

```
soak.py
  ├─ starts build/linux-cef/scratterd --test-film   (output -> daemon.log)
  ├─ once a minute:
  │    ├─ memory of the daemon and every CEF process under it   (/proc)
  │    ├─ amctl status --json: how far did playback get?
  │    └─ one line in memory.csv
  └─ after 4 hours: stops the daemon like Ctrl-C, writes summary.txt
```

Everything lands in `soak-results/<date-time>/`, which git ignores.

## Which memory, of which processes

The daemon isn't one process once CEF is on. Chromium runs the page, the audio
decoding, the GPU stub and Widevine in processes of their own, all started by
the daemon ([cef-host.md](cef-host.md) has the table). A leak can be in any of
them, so the script finds every process whose parent, or parent's parent, is
the daemon, by reading the parent pid out of `/proc/PID/stat` for every
process on the machine. It sorts them by the `--type=` argument Chromium
starts them with (`renderer`, `gpu-process`, `utility`, `zygote`), so the
CSV has a column per kind.

For each process it reads two numbers from `/proc/PID/status`:

- **`VmRSS`**, the resident set size: all of the process's memory that sits in
  RAM right now. It includes shared libraries such as `libcef.so`, which every
  CEF process maps, so adding up RSS counts those more than once. That's fine
  here: we care about change over time, not the exact total.
- **`RssAnon`**, the anonymous part: memory the program asked for itself
  (`malloc`, `new`, a JavaScript heap) rather than memory that mirrors a file
  on disk. A leak is memory asked for and never given back, so it shows up
  here.
- **`RssShmem`**, shared memory: memory set up on purpose to be shared
  between processes. Chromium hands audio and other data from one of its
  processes to another this way, so a leak can sit here instead. Each process
  that maps a piece counts it, so the total overcounts, but a rising line
  still shows.

The pass/fail verdict goes by anonymous plus shared memory, added up over
all the processes.

`/proc/PID/smaps_rollup` has a fairer number (PSS, which splits shared pages
between the processes sharing them), but reading it needs the same permission
as attaching a debugger, which a sandboxed Chromium process may refuse.
`status` is readable for any process, and is enough to see a trend.

## Is it playing?

Memory staying flat means nothing if the film stopped after a minute. Each
line also records the play position from `amctl status --json`. The daemon
only moves the position for audio it actually sent to the sound card, so the
difference between two lines is how many seconds of sound played in that
minute. Below 90% of the minute counts as "the sound stopped or cut out" in
that line (column `stalled`).

When the daemon quits it prints `stopped cleanly, N late audio buffers,
M frames dropped`. The script stops it with SIGTERM, the same signal Ctrl-C
handling listens for, and copies that line into the summary. A late buffer is
a moment PipeWire asked for sound and the ring buffer had run dry (a dropout
you can hear). A dropped frame is sound that arrived when the ring buffer was
full and had to be thrown away. A handful of late buffers in the first second,
while the film loads, is normal; a steady climb is not.

## Leak or cache?

Memory nearly always rises at the start: Chromium fills its caches, the page
loads Shaka Player and the film, the JavaScript engine grows its heap. That's
not a leak. A leak is memory that **keeps** rising at a steady rate for as long
as the program runs.

So the summary ignores the first 15 minutes, then fits a straight line through
the rest of the points and reports its slope in MB per hour. "Least squares"
picks the line that sits closest to all the points at once, so one spike at
the start or the end can't swing the answer the way "last minus first" would.
More than 10 MB an hour of anonymous plus shared memory (`--max-growth` to change it)
fails the run. 10 MB is a judgment call, not a rule: it allows for Chromium's
own housekeeping, and over 4 hours it would still be a 40 MB climb you'd see
in the numbers. Under half an hour after the warm-up, the summary says "too
short to judge" instead of guessing, and it ends in NOT JUDGED rather
than PASSED, so a run cut short can't pass for a good one.

## Running it

It needs the CEF build (the `linux-cef` preset) and Python 3, nothing else.

```sh
git fetch origin
git switch feature/cef-soak-test
cmake --build --preset linux-cef
python3 tools/soak/soak.py
```

It prints a line a minute. Leave it running for the 4 hours, with the PC
awake (a locked screen is fine; sleep or suspend isn't). The film plays out
loud the whole time; turn the speakers down, or from a second terminal window
run `./build/linux-cef/amctl volume 5` (volume doesn't change what's measured).
Ctrl-C ends the run early and still writes the summary.

Shorter or different runs:

```sh
python3 tools/soak/soak.py --hours 1
python3 tools/soak/soak.py -- --clear-film
python3 tools/soak/soak.py --build build/linux-debug -- --no-audio
```

Anything after `--` goes to the daemon in place of `--test-film`. The second
line plays the same film without encryption: everything in CEF and the audio
path runs, but no Widevine and no license server. The third soaks the plain
tone without a sound card.

If Google's test license server refuses a license ("Shaka error 6007" in
`daemon.log`), the film stops. The page then waits 5 seconds and loads the film
again from the same spot with a fresh license and prints `page: reloading the
film`. The script counts those lines each minute (column `reloads`), and a
minute where the sound stopped but the page reloaded the film doesn't fail the
run: that's Google's test server, not us. The summary says how often it
happened. A reload is also a decent extra test, since it builds and tears down
the player and a Widevine session every time.

A few seconds of the same script run in ctest (`soak_smoke`, against the
tone with `--no-audio`) so it can't quietly break when the daemon changes.

## Results

**2026-10-18, robbie's machine: PASSED.** 4 hours of `--clear-film` (the
unencrypted copy of the film, because Widevine licensing doesn't work in our
CEF yet, issue #21), `linux-cef` debug build, CachyOS, 13 processes:

```
ran for 4.00 hours, 240 lines in memory.csv
memory after the first 15 minutes (start -> end, growth):
  total         2006.8 ->   1979.1 MB,  -11.15 MB/hour
  total_anon     381.5 ->    385.4 MB,   +0.44 MB/hour
  total_shmem      3.3 ->      3.7 MB,   +0.03 MB/hour
  daemon         341.7 ->    336.7 MB,   -1.97 MB/hour
  renderer       688.8 ->    682.3 MB,   -2.79 MB/hour
  gpu            156.7 ->    148.0 MB,   -2.59 MB/hour
  utility        456.6 ->    453.5 MB,   -2.51 MB/hour
  zygote         289.4 ->    284.9 MB,   -1.27 MB/hour
  other           73.7 ->     73.7 MB,   +0.00 MB/hour
memory: flat (+0.47 MB/hour, limit 10)
playback: played the whole time
stopped cleanly: 1 late audio buffers, 0 frames dropped
PASSED
```

What it says:

- **No leak.** The memory the processes own (anonymous plus shared) grew
  half a megabyte an hour, about 2 MB over the whole run: noise. Every process
  kind went slightly *down* in resident memory, which is the kernel trimming
  pages nobody touched, not the player shrinking.
- **No dropouts.** Every one of the 240 minutes played its full 60 seconds.
  The one late buffer is the start, while the film loaded.
- **The size.** About 385 MB the processes own, around 2 GB if you add up
  every process's resident memory (shared libraries such as `libcef.so`
  counted once per process). The renderer and the audio utility process are
  the biggest. That's normal for a hidden Chromium, and heavier than a music
  player needs; making it smaller is separate work.

Still to do: the same run on the Widevine film once #21 is fixed, since the
CDM process and license renewals only run there, and again with real Apple
Music (#4).

## What you learn

- What RSS, anonymous and shared memory are, and why the numbers from different
  processes overlap.
- How Linux describes processes in `/proc`, and walking a process tree from it.
- Why a leak is a slope, not a big number, and fitting a line to noisy data.
- Why long tests catch what short ones can't.
