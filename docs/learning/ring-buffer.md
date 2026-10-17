# The audio ring buffer, explained

This covers `src/common/spsc_ring_buffer.hpp` and its tests in
`tests/test_spsc_concurrency.cpp`, from PR #27. It assumes you know some
C++ but have never written lock-free code.

## The problem it solves

Two threads in `scratterd` have to hand audio to each other:

- **The producer** is CEF's audio callback. Chromium decrypts the song and calls us
  with a chunk of decoded samples, whenever it feels like it.
- **The consumer** is the PipeWire (or WASAPI) callback. The sound card calls it on
  a strict clock, about every 5 to 10 ms, and asks for exactly N frames, right now.

The consumer is a **real-time thread**. If it ever waits, even for a millisecond
too long, the sound card runs dry and you hear a click or dropout. That rules out
the normal tools for sharing data between threads:

- A **mutex** can make the audio thread sleep while the other thread holds it.
- **Allocating memory** (`new`, `std::vector::push_back`) can take a lock inside
  malloc.
- **Syscalls** can block in the kernel.

So we need a way to pass audio across that never waits, never allocates, and never
calls the kernel. That's the ring buffer.

## What a ring buffer is

Picture a fixed array bent into a circle, with two pointers walking around it:

```
          read                write
           v                    v
  [ . . . A B C D E F G H . . . . . . ]
           \_____________/
           audio waiting to be played
```

- The producer writes new audio at `write` and moves it forward.
- The consumer plays audio from `read` and moves it forward.
- When a pointer hits the end of the array, it wraps back to the start. That's the
  "ring".
- Everything between `read` and `write` is audio waiting to be played. Everything
  else is free space.
- If `write` catches up to `read` from behind, the buffer is **full** and the
  producer has to wait its turn. If `read` catches up to `write`, it's **empty**
  and the consumer has nothing to play (an underrun).

The array is allocated once, up front, so no memory is ever allocated while music
plays.

## Why no lock is needed

"SPSC" means **single producer, single consumer**: exactly one thread writes and
exactly one thread reads. That's what makes it work without a lock:

- Only the producer ever changes `write`.
- Only the consumer ever changes `read`.

Each thread just *reads* the other's index to find out how much room or data
there is. No two threads ever write the same variable, so they never need to take
turns.

The indices are `std::atomic<std::size_t>` so that each thread sees the other's
updates at all. The one subtle part is **ordering**:

- The producer fills in samples, *then* moves `write` forward with
  `memory_order_release`. That means "everything I wrote before this is done".
- The consumer reads `write` with `memory_order_acquire`. That means "show me
  everything that was done before that release".

Together they guarantee the consumer never sees the new `write` value before the
samples behind it have landed. Without them, the CPU or compiler could reorder
things and the consumer could play half-written garbage. The TSan build checks
exactly this (see below).

## How this one works

### Two steps: reserve, then commit

Neither side copies through a temporary buffer. Each side gets a pointer straight
into the ring, fills or drains it, then says "done":

```cpp
// Producer (CEF thread)
auto w = buf.reserve_write(frames);     // "how much room is there, and where?"
// ...write samples into w.first, then w.second...
buf.commit_write(w.frames_reserved);    // "it's ready, consumer can have it"

// Consumer (PipeWire thread)
auto r = buf.reserve_read(frames);      // "how much audio is there, and where?"
// ...copy r.first, then r.second, to the sound card...
buf.commit_read(r.frames_reserved);     // "I'm done, producer can reuse that space"
```

Until `commit_write` runs, the consumer can't see the new audio at all, so it never
reads a half-filled chunk.

### Why there are two spans (`first` and `second`)

If the free space runs past the end of the array, it is really two pieces: the bit
up to the end, and the bit that continues from the start. So a reservation gives
you two `std::span`s. Usually `second` is empty. When the region wraps, you fill
`first` and then `second`. The test "a reservation that crosses the end is split
into two spans" walks through this with an 8-frame buffer.

### Frames and channels

A **frame** is one moment in time: for stereo, one left sample plus one right
sample. Sizes in the API are in frames, and the spans hold samples, so a 5-frame
stereo reservation has 10 floats in it. In the daemon's buffer, capacity is 16,384
frames (about 341 ms at 48 kHz).

### Small tricks for speed

- **The capacity must be a power of two.** Then turning an ever-growing index into
  an array position is `index & (capacity - 1)`, a single instruction, instead of
  a `%` division. The indices just keep counting up and never reset. Unsigned
  subtraction gives the right fill level even when they eventually overflow.
- **Each index sits on its own 64-byte cache line.** CPU cores share memory in
  64-byte chunks. If both indices were in the same chunk, every write by one core
  would invalidate the other core's copy ("false sharing"), and they would keep
  stealing the line back and forth.
- **Each side caches the other side's index.** The producer remembers the last
  `read` value it saw and only looks again when that stale value says the buffer
  is full. Most of the time it never touches the consumer's cache line at all. The
  consumer does the same in reverse.

### One thing I added beyond the spec

The spec's sketch gave the consumer `available_read_frames()` (how much is there)
but no way to actually reach the samples. I added `reserve_read()`, which mirrors
`reserve_write()`. It was noted in the PR for kero to review.

## What the tests check

All of it is in `tests/test_spsc_concurrency.cpp`. Each `TEST_CASE` shows up as its
own line in `ctest`.

| Test | What it proves |
| --- | --- |
| a new buffer is empty | Nothing to read before anything is written. |
| reservations are limited by free space | You can't reserve more than fits, a full buffer refuses writes, and reading frees room. |
| nothing is visible before commit_write | The consumer can't see a reservation until it's committed, and committing fewer frames than reserved is fine. |
| reads return frames in the order they were written | Basic first-in, first-out order. |
| a reservation that crosses the end is split into two spans | The wrap-around case: data split at the end of the array comes back in the right order. |
| the buffer keeps working after many laps | 1,000 rounds of uneven sizes, so the split lands everywhere. |
| 100M frames cross two threads bit-for-bit | The real thing (below). |

The stress test runs a producer thread and a consumer thread at full speed for
100 million stereo frames. Each sample's value is built from its position number,
so the consumer knows exactly what bits it should see next, and it compares them
bit for bit (not "close enough" as floats). It uses a tiny 1,024-frame buffer and
odd chunk sizes, so the two threads constantly hit full, empty and the wrap point.
This is the spec's Phase 1 gate.

## What the sanitizer builds check

The same tests are compiled three ways. Sanitizers are compiler options that add
checking code around every memory access, then fail the test the moment something
illegal happens.

| Preset | Sanitizer | Catches | Why it matters here |
| --- | --- | --- | --- |
| `linux-debug` | none | Plain test failures | Fast everyday run (~2 s for the stress test). |
| `linux-tsan` | ThreadSanitizer | **Data races**: two threads touching the same memory without proper syncing | The most important one for this file. If the acquire/release ordering were wrong, TSan would flag it. I checked by weakening it to `relaxed`, and TSan failed the test immediately. About 15 s. |
| `linux-asan` | AddressSanitizer + UBSan | Reading or writing outside an array, use-after-free, leaks, signed overflow and other undefined behavior | Catches an off-by-one in the wrap-around math that would read past the end of storage. |

A normal build can pass all of these by luck. Races and out-of-bounds reads often
"work" until they don't. The sanitizers turn "might break someday" into a test
failure now.

## Running it on Arch

```sh
cmake --preset linux-tsan            # first time downloads Catch2
cmake --build --preset linux-tsan
ctest --preset linux-tsan                  # every test
ctest --preset linux-tsan -R 100M          # only tests whose name contains "100M"
./build/linux-tsan/tests/test_spsc_concurrency "[spsc]"   # run the binary directly
```

Swap `linux-tsan` for `linux-debug` or `linux-asan`. Running the binary directly
with `--list-tests` shows every test name, and `-s` prints each check as it runs.

## Where it's used next

Step 3 of the spec's bootstrap order (the PipeWire sink on Linux, the WASAPI sink on
Windows) reads a
440 Hz sine wave out of this buffer on the PipeWire callback. Each sink's
process callback becomes the consumer: `reserve_read`, copy into PipeWire's
buffer, `commit_read`. Later the CEF audio handler becomes the producer.
