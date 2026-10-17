# The clock-drift controller and resampler, explained

This covers `src/daemon/audio_engine/drift_controller.hpp`,
`src/daemon/audio_engine/polyphase_resampler.hpp` and their tests in
`tests/test_resampler_drift.cpp` (issue #21). It sits on the consumer side of
the ring buffer from [ring-buffer.md](ring-buffer.md). There's no OS code in any
of it, just arithmetic.

## The problem: two clocks

Audio has two ends. CEF decodes the stream and writes PCM into the ring buffer,
timed by the computer's clock. The sound card reads it back out, timed by a
crystal on the sound card. Both say "48,000 frames a second", and both are a
little wrong, each in its own way.

A mismatch of 150 parts per million (ppm) is normal. That's 7.2 extra frames
every second, about 26,000 an hour. With a 16,384-frame ring buffer that
started at 4,096 frames, the buffer is full in under half an hour and audio
starts getting dropped. If the mismatch goes the other way, the buffer runs dry
and you hear clicks.

You can't fix the clocks. What you can do is play very slightly faster or
slower so the sound card consumes frames at the rate CEF actually produces
them. 150 ppm is a pitch change of 0.0026 semitones, far below what anyone can
hear.

## The controller: how fast should we play?

The only thing the consumer can observe is how full the ring buffer is. If it's
creeping up, the producer is faster; if it's creeping down, slower. So the
controller is a feedback loop: measure the fill, nudge the playback ratio,
repeat every audio callback.

`DriftController::update(fill)` does three things.

**1. Error.** `error = fill - 4096`. Positive means too full, so play faster.

**2. Smoothing.** CEF doesn't deliver one frame at a time; it delivers 480
frames every 10 ms. The sound card takes 256 or 1024 frames at a time. So the
raw fill is a sawtooth that jumps by hundreds of frames, while the drift we want
to see is a fraction of a frame per callback. An exponential moving average
filters the jumps out:

```cpp
smoothed_error_ += 0.005 * (error - smoothed_error_);
```

Each callback moves the average 0.5% of the way towards the new reading. A
single jump barely moves it; a trend moves it steadily.

**3. PI control.**

```cpp
ratio = 1.0 + kp * smoothed_error + ki * sum_of_smoothed_errors;
```

- The **P** (proportional) term reacts to the error right now. On its own it
  can't finish the job: to keep playing 150 ppm fast it would need a permanent
  error to multiply. The buffer would settle off-target.
- The **I** (integral) term adds up the error over time. As long as there's any
  error at all, the sum keeps growing, so the ratio keeps moving, until the
  error is truly zero. At that point the sum is holding the clock mismatch
  itself. The test checks this: after an hour at +150 ppm, `ppm()` reads about
  150.

Both gains are tiny (3e-6 and 3e-10) because the error is measured in frames
and the ratio in parts per million. They were picked by running the simulation
below with different values: bigger gains settle faster but start to overshoot
and oscillate with 1024-frame callbacks.

Finally the ratio is clamped to ±1000 ppm, so a bug or a stall can never make
the music audibly faster or slower.

### Integral windup

Clamping the output creates a trap. Say the producer stalls for a minute and
the buffer overflows. The error is huge, the ratio is clamped at +1000 ppm, and
meanwhile the integral keeps adding up that huge error, far past anything the
clamp would ever let through. When the stall ends, the integral is so large
that it holds the ratio at the limit for many minutes while the negative error
slowly pays it back.

The fix is called anti-windup: stop adding to the integral once the I term alone
would reach the clamp.

```cpp
integral_ = std::clamp(integral_ + smoothed_error_, -integral_limit, integral_limit);
```

The test "controller never goes past 1000 ppm, and unwinds after a stall" runs
exactly that story and checks the ratio recovers within a minute.

## The resampler: playing at 1.00015x

The controller produces a number like 1.00015: "consume 1.00015 input frames per
output frame". So most output frames fall between two input frames, and their
value has to be made up.

The simplest guess is a straight line between the two neighbours (linear
interpolation). It's cheap, but it acts like a gentle low-pass filter and adds
distortion you can measure.

The correct answer comes from sampling theory: a band-limited signal can be
rebuilt exactly from its samples by adding up a sinc function (`sin(x)/x`)
centered on each one. A real sinc goes on forever, so we take the 16 samples
nearest the output point and fade the sinc out towards the edges with a
window (Blackman's), so cutting it off doesn't cause ringing. Each output frame
is then a weighted sum of 16 input frames.

The weights depend on where exactly the output lands between two input frames:
its **phase**, from 0.0 to 1.0. Working out 16 windowed sincs per output frame
would be slow, so the constructor computes them once, for 256 evenly spaced
phases, into a table. For a phase in between two table rows, `process()`
computes the sum with both rows and blends the two results. A table of
filters indexed by phase is what **polyphase** means.

Some details worth knowing:

- **The table is built in the constructor**, which allocates. That happens once
  at stream setup. `process()` itself never allocates, locks or makes a
  syscall, so it's safe on the audio thread.
- **Each row is scaled to add up to 1.** Otherwise a constant input (or very
  low bass) would come out slightly louder at some phases than others. The test
  "resampler passes a constant through unchanged" checks this.
- **The cutoff is 0.9 of Nyquist** (about 21.6 kHz at 48 kHz). We aren't
  changing the sample rate by much, so there's nothing to anti-alias; the
  margin is just room for a 16-tap filter's gentle slope.
- **Delay.** The filter needs 8 frames on each side of the output point, so the
  output lags the input by 8 frames (0.17 ms).
- **State carries over** between calls: the last 16 input frames and the phase.
  So the input can come in pieces, like the two halves of a ring buffer
  reservation that wraps around. The last test checks one piece and two pieces
  give identical output.

This version is plain scalar code. The spec calls for SIMD; that can replace
the inner loop later without changing the interface.

## The test: an hour in a second

`simulate()` in `tests/test_resampler_drift.cpp` plays both ends:

- A producer whose clock is 150 ppm fast delivers 480-frame packets into a real
  `LockFreeSpscAudioBuffer`.
- A consumer callback every 256 (or 1024) frames of sound-card time asks the
  controller for a ratio, then takes `quantum * ratio` frames out, carrying the
  fraction over to the next callback.

Only frame counts move, no actual audio, so 60 simulated minutes take a fraction
of a second, even under the sanitizers. The test checks no callback ever found
too few frames (underrun), no packet ever failed to fit (overrun), the smoothed
fill stayed within 3% of 4096, and the controller ended up correcting by about
150 ppm. On the current gains the worst error is under 1%.

One honest caveat: the first 60 simulated seconds aren't held to the 3% limit.
The fill is sampled at one point in a sawtooth, so on startup its average sits
up to half a packet plus half a callback away from the target, and the
controller needs some seconds to drain that at its 1000 ppm speed limit. That's
startup behavior, not drift.

## Read more

- Fons Adriaensen, "Using a DLL to filter time" (2005). The paper behind
  zita-ajbridge, which solves exactly this problem for JACK.
- Julius O. Smith, "Digital Audio Resampling Home Page" (online, CCRMA). The
  bandlimited-interpolation section is where the polyphase table comes from.
- Any intro to PID control; the I term and windup sections are the parts used
  here.
