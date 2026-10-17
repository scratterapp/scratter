# The audio sink, explained

This covers `src/pal/interfaces/audio_sink.hpp` and `realtime.hpp` (#18). The
Linux sink is #19 and the Windows sink is #20.

## Push vs pull

There are two ways to get sound out of a program:

- **Push:** you call `write(samples)` whenever you have audio, and the OS
  queues it. It's simple, but you never know exactly when the sound card will
  play what you wrote, and if you're late the queue runs dry.
- **Pull:** the OS calls *you*, on its own clock, and says "I need 256 frames,
  right now". You fill its buffer and return.

Every low-latency audio API is pull-based: PipeWire, JACK, WASAPI in event
mode, CoreAudio. Our sink is too. The sound card's crystal decides when the
callback runs, and our job is to always have samples ready.

## Where the sink sits

```
CEF thread                         OS audio thread (real-time)
-----------                        ---------------------------
OnAudioStreamPacket                render callback
   |                                  ^
   |  reserve_write / commit_write    |  reserve_read / commit_read
   v                                  |
   +------------- SPSC ring buffer ---+
```

CEF pushes decoded audio into the ring buffer whenever it has some. The sink's
render callback pulls from the other end whenever the sound card needs some.
The ring buffer absorbs the difference in timing.

## The interface

```cpp
auto sink = am::pal::AudioSink::open({.sample_rate_hz = 48000, .channels = 2},
                                     [&](std::span<float> out) noexcept {
                                         fill_from_ring_buffer(out);
                                     });
sink->start();
```

- **`open`** connects to PipeWire or WASAPI and asks for a format. The OS can
  say no and pick something else, so `format()` reports what you actually got.
- **The render callback** gets a span of interleaved floats (`L R L R ...`) to
  fill completely. How many frames it asks for can change between calls.
- **`start` / `stop`** begin and end the callbacks. After `stop()` returns the
  callback is guaranteed not to be running, so it's safe to tear down whatever
  it reads from.

The callback sees a plain `std::span<float>`, not the ring buffer. That's on
purpose: the clock-drift resampler (#21) will sit between the ring buffer and
the sink, and this way neither side has to change when it arrives.

## Real-time rules

The callback runs on a real-time thread with a deadline of a few
milliseconds. Anything that can wait is banned inside it:

| Banned | Why |
| --- | --- |
| `new`, `malloc`, `push_back` | the allocator can take a lock |
| `std::mutex` | the other thread might be holding it |
| syscalls (`write`, `printf`, logging) | the kernel can put the thread to sleep |
| touching memory that might be swapped out | a page fault is a disk read |

Note the callback type: `std::move_only_function<void(std::span<float>) noexcept>`.
Calling a `move_only_function` doesn't allocate (only creating one can), so
it's safe to use here. The `noexcept` is there because nothing on that thread
can report an error anyway.

## Why memory pinning is separate

The last row of that table is what `realtime.hpp` is for.
`lock_memory(span)` calls `mlock` on Linux or `VirtualLock` on Windows, which
tells the OS to keep those pages in RAM. It's a separate function rather than
part of the sink because the ring buffer belongs to the daemon, not the sink.

Raising the audio thread's priority isn't in the shared interface at all.
PipeWire already runs its data thread with real-time priority (through rtkit),
and the WASAPI sink creates its own thread and registers it with MMCSS. Each
sink handles it internally.

## The Linux side (#19)

`src/pal/linux/pipewire_sink.cpp` creates a `pw_stream` and asks for 32-bit
float stereo at 48 kHz. Some things worth knowing when you read it:

- **Two threads, neither of them ours.** `pw_thread_loop` runs setup and
  control messages. PipeWire's data thread, which already has real-time
  priority, calls `on_process()` because of `PW_STREAM_FLAG_RT_PROCESS`.
  Without that flag `on_process()` would run on the normal-priority thread
  loop instead.
- **`on_process()`** dequeues a buffer, works out how many frames the graph
  wants this cycle (`buffer->requested`), calls the render callback on that
  span, fills in the chunk size and queues the buffer back.
- **`open()` waits for the stream** to either be created on the PipeWire side
  or fail, so a missing daemon is an error from `open()` and not silence
  later.
- **`stop()` must guarantee the callback isn't running** once it returns.
  Instead of trusting PipeWire's internals for that, the sink uses two
  atomics. The callback sets `in_callback`, *then* checks `running`.
  `stop()` clears `running`, *then* waits for `in_callback` to drop. With
  sequentially consistent atomics there's no gap where both miss each other.
- **The format always "wins".** A PipeWire playback stream goes through an
  adapter that converts and resamples to the graph's format, so our request
  is never refused and `format()` just reports it back.

### What strace shows

Running `play_sine` under `strace -f` against a null sink, PipeWire's data
thread made exactly three kinds of syscall, about once per cycle each:
`epoll_wait` (sleeping until the next cycle), `read` (clearing its wakeup
eventfd) and `write` (waking the next node in the graph). That's PipeWire's
own scheduling. Our callback adds no syscalls of its own: no `futex`, no
`mmap`, nothing from `printf` or `malloc`. So the spec's "zero syscalls on the
audio thread" holds for our code, but not literally for the thread, because
PipeWire itself has to sleep and wake somehow. The spec's phase 2 gate now says
exactly that: the rule covers our callback code, not PipeWire's thread.

### Listening to it

```sh
cmake --build --preset linux-debug
./build/linux-debug/tests/manual/play_sine 10
```

The main thread plays CEF's part and keeps the ring buffer at about 85 ms.
At the end it prints one line, like `10.0 s played, 465 buffers, 0 late`.
"Buffers" is how many times PipeWire asked for audio. "Late" is how many of
those asks found the ring buffer short, so part of the buffer had to be filled
with silence (an underrun). Anything above zero is a click you could hear.

## Read more

- Ross Bencina, "Real-time audio programming 101: time waits for nothing"
- `man 2 mlock`
