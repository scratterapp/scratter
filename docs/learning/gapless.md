# Gaps between tracks, explained

This covers a question rather than a new feature: when one song ends and the
next begins, can the daemon's audio path (`src/daemon/audio_engine/audio_path.hpp`)
add a gap or a click of its own? The answer is no, as long as Chromium keeps
sending audio, and the three track-change tests at the end of
`tests/test_audio_path.cpp` check it. The question came from reading how
librespot, an open-source Spotify player, plays albums without gaps.

## Why there might have been one

The ring buffer from [ring-buffer.md](ring-buffer.md) sits between Chromium and
the sound card. Chromium writes into it, the sound card's callback
(`AudioPath::render()`) reads out of it. It's the producer/consumer problem
with a bounded buffer, which OSTEP gets to in the chapter on condition
variables (ch. 30), except ours never waits on a lock because the audio thread
isn't allowed to.

`render()` keeps about 85 ms (4096 frames) of sound in the ring ahead of the
sound card. That's the cushion the drift controller steers around
([drift-controller.md](drift-controller.md)). It has one rule that looked
risky: when the ring runs dry, `render()` plays silence until the ring holds
85 ms again, and only then plays on.

So the worry was: if Chromium's audio stopped for a moment at a track change,
the ring would run dry and then we'd sit through another 85 ms of silence on
top. That would be a gap even if Apple Music's player is gapless.

## What actually happens

There's no "track" anywhere in the audio path. `render()` doesn't know a song
ended; it sees a stream of samples and how full the ring is. So the only thing
that matters is whether Chromium keeps sending packets. Three cases:

1. **Chromium keeps sending.** The last sample of one song is followed by the
   first of the next, and the ring never notices. No gap.
2. **Chromium goes quiet for a short moment** (less than about 60 ms). The
   sound card keeps playing what's in the cushion while the page catches up.
   No gap either; you'd never hear it.
3. **Chromium goes quiet for longer.** The ring runs dry, `render()` counts a
   late buffer, and waits for 85 ms of the next song before playing again.
   But that wait doesn't add to the gap. Think of a 200 ms break:

   ```
   time (ms)     0          85              200           285
   page          |--- sends nothing --------|--- sends again --->
   sound card    |-- plays cushion --|-- silence ---------|-- plays --->
   ```

   The sound card plays the 85 ms cushion at the start of the break, and the
   85 ms wait at the end is the cushion being rebuilt. They cancel: you hear
   200 ms of silence, the same as the break itself. Measured, it's 202 ms with
   256-frame sound card buffers and 213 ms with 1024-frame ones; the extra few
   ms come from the page's packets and the sound card's buffers running on
   their own schedules.

Why not skip the wait and play the moment audio comes back? Then the break
would sound 85 ms shorter, but the ring would be empty, and every little
hiccup after it would run dry again. The test proves it: with the wait taken
out, a 200 ms break gives 24 late buffers instead of one, each a tiny cut-out.
A clean gap is better than a stutter.

## Does Chromium go quiet between songs?

Probably not in a way that matters. As far as I know CEF keeps the page's audio
stream open until the page has been silent for about two seconds, then calls
`OnAudioStreamStopped`. The daemon already sees the same thing on pause: sound
keeps arriving for a while after the page pauses (see
[cef-host.md](cef-host.md)). So if Apple Music's player goes straight into the
next song, the stream never stops and we're in case 1. If the stream does stop,
the page was already silent for two seconds, so whatever gap you hear is Apple
Music's, not ours.

That's still to be confirmed with real songs once #4 lands. The hand test:

- Play an album where the songs run into each other (a live album, or one like
  Pink Floyd's *The Dark Side of the Moon*).
- Watch the daemon's terminal for `page audio stopped` / `page audio started`
  at the song change. Neither should show up.
- Run `amctl status` before and after the song change: the late-buffer count
  shouldn't go up.
- Check the `page audio started` line says the same rate and channel count for
  every song.

## Two things to know

- **A break in the middle of loud sound would click.** If Chromium stopped
  sending mid-song, the ring would run dry partway through a buffer and the
  sound would drop to zero in one sample, with no fade-out
  ([fade.md](fade.md) explains why that clicks). That needs Chromium's audio
  thread to stall while sound is playing, which we haven't seen; a song change
  doesn't cause it.
- **A short break spends the cushion for a while.** After a 50 ms break in
  case 2, the ring holds about 35 ms instead of 85. The drift controller
  refills it, but only by playing at most 0.1% slower, which takes close to a
  minute. A second break in that minute has less cushion to hide behind.

## The tests

`play_track_change()` plays two "songs", a constant 0.5 for a second and then a
constant 0.3, the way CEF would: a 512-frame packet every 512 frames of time,
while `render()` is called every 256 (or 1024) frames like a sound card would.
Neither song ever touches zero, so any run of zeros in the output after the
music starts is a gap. It runs a whole simulated three seconds in a few
milliseconds, because nothing waits on a real clock.

- "a page that keeps sending plays into the next track without a gap": case 1.
- "a short break between tracks is hidden by the ring buffer": a 50 ms break,
  case 2.
- "a long break between tracks is heard as long as it lasts, no longer": a
  200 ms break, case 3, with one late buffer and a gap within one packet and
  one sound card buffer of 200 ms.

To see the last one fail, delete `filling_up_ = true;` from the run-dry branch
of `render()` and run `test_audio_path`.
