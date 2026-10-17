# The telemetry SeqLock, explained

This covers `src/common/telemetry_shm.hpp` and its tests in
`tests/test_telemetry_seqlock.cpp` (issue #5). It builds on
[ring-buffer.md](ring-buffer.md), so read that first if acquire/release is new to
you.

## The problem it solves

The UI wants to draw a progress bar and a 64-band spectrum, up to 144 times a
second. All of that lives in the daemon. Asking over the IPC socket every frame
would cost two syscalls and a context switch per frame, so instead the daemon
writes its status into one 4096-byte page of shared memory and the UI reads it
directly.

That leaves one question: the snapshot is 320 bytes, and no CPU can write 320
bytes in one go. If the UI copies it while the daemon is halfway through an
update, it gets a **torn read**: the new track ID with the old position, or half
the spectrum from one frame and half from the next.

## Why not a mutex, or the ring buffer?

- **A mutex** would make the writer wait whenever the UI holds it. The writer is
  the audio thread, which must never wait. Worse, the UI maps the page read-only
  and lives in another process. If it crashed while holding the lock, the daemon
  would be stuck forever.
- **The ring buffer** is a queue: every item written must be read exactly once,
  and the writer stalls when it's full. Telemetry is the opposite. The UI only
  wants the latest value, it's fine to skip old ones, and when the UI is closed
  nobody reads at all. A queue would fill up and block the daemon.

What we want is "one writer that never waits, any number of readers that only
want the newest value, and readers can't write anything". That is exactly what
a SeqLock is for.

## How a SeqLock works

There is one counter next to the data. It's even when the data is stable and odd
while a write is in progress.

```
Writer                          Reader
  seq = seq + 1   (now odd)       before = seq          (odd? try again)
  write the data                  copy the data
  seq = seq + 1   (even again)    after = seq           (changed? try again)
```

The reader never blocks the writer and never writes anything, so it works
through a read-only mapping. If the reader's copy overlapped a write, either
`before` was odd or `after` is different, and it just tries again. The write
takes a few dozen nanoseconds, so a retry is rare.

The writer is **wait-free**: it always finishes in a fixed number of steps no
matter what readers do. Readers are not. The case that matters is a writer that
dies mid-write: if the daemon is killed between the two counter stores, the
counter stays odd forever and a reader that loops until it succeeds hangs. So
am-ui calls `try_read()` once per frame and keeps drawing the last good
snapshot when it fails. `read()`, which loops, is only for tests.

## The trap: the "obvious" version is undefined behavior

Here is what most SeqLock examples look like:

```cpp
struct BrokenSeqLock {
    std::atomic<std::uint64_t> seq{0};
    TelemetrySnapshot data{};          // a plain struct

    void write(const TelemetrySnapshot& v) {
        seq.fetch_add(1, std::memory_order_release);
        data = v;
        seq.fetch_add(1, std::memory_order_release);
    }
    TelemetrySnapshot read() const {
        for (;;) {
            auto before = seq.load(std::memory_order_acquire);
            if (before & 1) continue;
            TelemetrySnapshot copy = data;   // races with write()
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq.load(std::memory_order_relaxed) == before) return copy;
        }
    }
};
```

It looks fine: a torn copy is thrown away. But in C++, if one thread writes a
plain (non-atomic) variable while another reads it, that's a **data race**, and a
data race is undefined behavior. It doesn't matter that you discard the result.
The compiler is allowed to assume it never happens and optimize on that basis.

ThreadSanitizer knows this rule. I built that exact code with `-fsanitize=thread`
and it failed straight away:

```
WARNING: ThreadSanitizer: data race
  Write of size 8 ... by thread T1:
    #0 BrokenSeqLock::write(...)   broken.cpp:13   (data = v)
  Previous read of size 8 ... by main thread:
    #0 BrokenSeqLock::read() const broken.cpp:20   (copy = data)
```

There is a second, quieter bug in it too. The first `fetch_add` is a release.
Release only stops earlier things from moving *after* it; it does nothing to stop
`data = v` from moving *before* it. So the CPU is allowed to start writing the
data before the counter turns odd, and a reader could copy half-new data with an
even counter on both sides. (The spec's first draft had this same ordering.)

## How ours avoids it

The payload isn't stored as a plain struct. It is stored as 40
`std::atomic<std::uint64_t>` words (320 bytes / 8):

```cpp
std::atomic<std::uint64_t> seq_{0};
alignas(64) std::array<std::atomic<std::uint64_t>, kWordCount> payload_{};
```

Those two lines are all the data `TelemetryPage` holds. `write()` turns the
snapshot into 40 words with `std::bit_cast` and stores each one. `try_read()` loads each word and `bit_cast`s them back. Every access is
atomic, so there is no data race at all, and TSan is happy.

The memory orders are chosen to fix the second bug without standalone fences:

| Step | Order | Why |
| --- | --- | --- |
| writer: counter to odd | relaxed | The next line handles ordering. |
| writer: each payload word | **release** | A release store keeps everything before it (the odd counter) above it. So if a reader sees any new word, it is guaranteed to also see the odd counter. |
| writer: counter to even | release | Readers that see the even value see all the words before it. |
| reader: first counter load | acquire | Pairs with the "even" store above. |
| reader: each payload word | **acquire** | An acquire load keeps everything after it (the second counter load) below it. |
| reader: second counter load | relaxed | The loads above already pin it in place. |

Put together: if the reader picked up even one word from a newer write, its
second counter load must see that write's odd value (or later), which won't match
`before`, so it retries. Hans Boehm's paper "Can Seqlocks Get Along With
Programming Language Memory Models?" (2012) walks through this argument and the
fence-based alternative.

Is this slow? No. On x86-64 every one of those loads and stores compiles to a
plain `mov`; x86 already gives acquire/release for free. On ARM64 they become
`ldar` and `stlr`, which cost a little but are still well under the spec's 50 ns
budget for 40 words. We avoided `std::atomic_thread_fence` partly because TSan
doesn't model standalone fences (GCC even warns about it under `-fsanitize=thread`,
which `-Werror` would turn into a build failure).

## The rest of the header

- **`TelemetrySnapshot` is exactly 320 bytes.** The fields add up to 296; the
  last 24 bytes are an explicit `reserved_tail` array instead of hidden compiler
  padding. That matters because `std::bit_cast` of padding bytes into a
  `uint64_t` is undefined, and because it keeps every byte crossing the process
  boundary well defined. `offsetof` asserts check there's no padding hidden in the
  middle either.
- **`PlaybackState` is an `enum class : std::uint8_t`**, still one byte, but you
  can't accidentally compare it with an unrelated integer.
- **The counter gets its own cache line.** Readers spin on it, so it shouldn't
  share a line with the payload the writer is busy updating. The whole page
  object is 384 bytes, well inside 4096.
- **Readers are all `const`**: `try_read()` and `read()`. am-ui will hold a
  `const TelemetryPage*` into a read-only mapping.
- **It works across processes**, not just threads, because lock-free atomics are
  also "address-free": they work the same when two processes map the same memory
  at different addresses. The `is_always_lock_free` static_assert guards that.

What it doesn't do yet: create the shared memory. That's issue #6 (the interface)
and #7/#8 (Linux and Windows). The daemon will `std::construct_at` a
`TelemetryPage` at the start of the mapping, and the UI will point a
`const TelemetryPage*` at its read-only view.

## What the tests check

| Test | What it proves |
| --- | --- |
| a new page reads as all zeros | A fresh page is valid to read before the first write. |
| a written snapshot reads back unchanged | Every field survives the trip through the 40 words. |
| readers work through a const reference | The reader API compiles against a `const` page, like a read-only mapping. |
| readers never see a torn snapshot while the writer runs flat out | The real thing (below). |

The stress test runs one writer thread doing a million back-to-back writes (much
harsher than 120 Hz) against two reader threads. Every field of write number *n*,
including each FFT band and the padding, is stamped from *n*, so a reader can tell
whether everything it copied came from the same write. Readers also check the
numbers never go backwards and that the last thing they see is the final write.

To make sure the test can actually fail, I disabled the second counter check in
`try_read()` and ran it again. The readers saw 8,809 and 6,735 torn snapshots, and
the test failed. With the check back, zero.

| Preset | Result | Stress test time |
| --- | --- | --- |
| `linux-debug` | pass | ~2 s |
| `linux-asan` (ASan + UBSan) | pass | ~3 s |
| `linux-tsan` | pass, no races | ~15 s |

## Running it

```sh
cmake --preset linux-tsan
cmake --build --preset linux-tsan
ctest --preset linux-tsan -R torn
./build/linux-tsan/tests/test_telemetry_seqlock "[seqlock]"
```

## Where it's used next

- Issues #6, #7, #8 put a `TelemetryPage` in real shared memory. The Linux test
  for #7 can rerun this same torn-read check across two processes.
- The audio engine's FFT tap (phase 2 and 3) becomes the writer.
- am-ui's render loop (phase 5) becomes the reader.
