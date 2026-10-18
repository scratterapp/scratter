# Fading in and out on play and pause, explained

This covers the fade in `AudioPath::render()`
(`src/daemon/audio_engine/audio_path.hpp`) and its tests in
`tests/test_audio_path.cpp` (issue #1). It's about 20 lines of code; the
interesting part is why a sudden start or stop is audible at all.

## Why you hear a click

A speaker cone sits wherever the current sample tells it to. Music moves it
smoothly, a little each sample. If the samples jump, say from 0.5 straight to
0 because you pressed pause, the cone has to get from one place to the other
in 1/48000 of a second. A jump that fast is a burst of every frequency at once,
and your ear hears it as a click or pop.

The CEF hand test showed two of these:

- **Resuming.** While paused, `render()` throws away the page's audio (see
  [cef-host.md](cef-host.md) for why), so playing again starts from silence and
  jumps straight to wherever the music is, at full volume.
- **Pausing.** The sound cut from full volume to silence in one sample.

Nothing is wrong with the audio itself. The fix is to never jump.

## The fade

Each `AudioPath` keeps one number, `fade_`, between 0 (silent) and 1 (full
volume). `render()` multiplies every frame by it, along with the volume, and
moves it a small step each frame:

- while playing, up towards 1;
- while paused or stopped, down towards 0.

The step is `1 / 480` at 48 kHz, so a full fade takes 480 frames, which is
10 ms (`kFadeMillis`). That's long enough to smooth the jump away and short
enough that a pause still feels instant.

So pressing play now goes: wait for the ring buffer to fill up (as before),
then ramp from silence to full volume over 10 ms. Pressing pause goes: keep
playing for 10 more milliseconds while ramping down, and only then go silent
and start throwing away what the page sends.

That last part is the only change to how pause works. Before, `render()`
checked "paused?" and went silent right away. Now it checks "paused, and done
fading out?". Until the fade reaches 0 it plays on as if nothing happened,
just quieter each frame.

Two edge cases fall out without extra code:

- **Play during a fade-out.** `fade_` just turns around and climbs back to 1
  from wherever it was. No restart, no gap.
- **Running dry.** If the ring buffer runs out mid-song (an underrun), there's
  nothing to fade, so it still cuts. But `render()` sets `fade_` back to 0
  then, so when the audio comes back it fades in instead of jumping.

## Why it's safe on the audio thread

The audio thread can't allocate, lock or make a syscall (spec section 6,
phase 2 gate). The fade is one `float` that only the audio thread touches, plus
a multiply and a `std::min`/`std::max` per frame, so it's fine. The play state
it reads was already an atomic.

Why linear and not a curve? Over 10 ms the shape doesn't matter much to the
ear; what matters is that there's no jump. Some players use a curved ramp for
longer fades (a few hundred milliseconds when you skip tracks), and that can
come later if we want one.

## The tests

The new tests feed the audio path a page that sends a constant 0.5 on both
channels. With that input, the output without a fade would be a flat line that
jumps between 0 and 0.5. The tests record everything `render()` writes and
check the biggest change from one sample to the next, `biggest_jump()`:

- with the fade, the biggest step is about 0.5 / 480 ≈ 0.001;
- without it, it's 0.5.

They check play, pause then play, play halfway through a fade-out, and stop.
To make sure the tests can actually fail, set the fade step to 1 (a fade one
frame long) and run them: five checks fail.

A few older tests changed too, because pause and stop now take a couple of
buffers (`kFadeBuffers`) before they're silent.

## Hand test

Run the test film (see [cef-host.md](cef-host.md#hand-test)), then pause and
play it a few times with `amctl pause` and `amctl play`. The tiny cut right
after play should be gone. It's worth checking again with real Apple Music
tracks once #4 lands, since they may behave differently from the test film.
