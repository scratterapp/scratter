#pragma once

// The page the daemon plays until Apple Music arrives (issue #77): Google's
// public Widevine test film "Angel One", audio only, through Shaka Player.
// Google encrypts it with Widevine and runs a test license server that gives
// the key to anyone, so it exercises the same path as an Apple Music track
// (AAC audio, a Widevine license, protected audio reaching CefAudioHandler)
// without an Apple account. The #49 experiment used the same film.
//
// CEF serves this from memory at kTestFilmUrl. It has to be an https address:
// Chromium refuses DRM on file:// pages ("EME use is not allowed on unique
// origins"), and https counts as a secure origin even though nothing is
// fetched over the network for it.
//
// The daemon drives the page through three functions, amPlay(), amPause() and
// amStop(). The page prints what it's doing to its console, and the daemon
// copies those lines to its own output with a "page:" in front.

#include <string_view>

namespace am::daemon {

// A made-up host under .invalid, a name reserved so it can never be a real site.
inline constexpr std::string_view kTestFilmOrigin = "https://player.invalid";
inline constexpr std::string_view kTestFilmUrl = "https://player.invalid/test-film.html";

inline constexpr std::string_view kTestFilmPage = R"html(<!doctype html>
<meta charset="utf-8">
<title>Widevine test film</title>
<video id="film" loop></video>
<script src="https://cdn.jsdelivr.net/npm/shaka-player@4.11.2/dist/shaka-player.compiled.js"></script>
<script>
const manifest = 'https://storage.googleapis.com/shaka-demo-assets/angel-one-widevine/dash.mpd';
const license = 'https://cwip-shaka-proxy.appspot.com/no_auth';
const film = document.getElementById('film');
let ready = false;
let wanted = 'play';

// The daemon calls these. Before the film has loaded they only remember what
// was asked for.
window.amPlay = () => { wanted = 'play'; if (ready) film.play(); };
window.amPause = () => { wanted = 'pause'; if (ready) film.pause(); };
window.amStop = () => {
  wanted = 'pause';
  if (ready) { film.pause(); film.currentTime = 0; }
};

async function main() {
  const aac = 'audio/mp4; codecs="mp4a.40.2"';
  if (!MediaSource.isTypeSupported(aac)) {
    console.log('this CEF build cannot decode AAC; it needs proprietary_codecs=true');
    return;
  }
  if (typeof shaka === 'undefined') {
    console.log('Shaka Player did not load; is the internet reachable?');
    return;
  }
  shaka.polyfill.installAll();
  const player = new shaka.Player();
  await player.attach(film);
  player.configure({
    manifest: { disableVideo: true, disableText: true },
    preferredAudioCodecs: ['mp4a.40.2'],
    drm: { servers: { 'com.widevine.alpha': license } },
  });
  player.addEventListener('error', (e) =>
    console.log(`Shaka error ${e.detail.code} (see shaka.util.Error in Shaka's docs)`));
  film.addEventListener('playing', () => {
    const track = player.getVariantTracks().find((t) => t.active);
    console.log(`playing the Widevine test film, audio ${track ? track.audioCodec : 'unknown'}`);
  });
  film.addEventListener('pause', () => console.log('paused'));

  try {
    await player.load(manifest);
  } catch (e) {
    console.log(`couldn't load the film: Shaka error ${e.code}`);
    return;
  }
  ready = true;
  if (wanted === 'play') film.play();
}

main();
</script>
)html";

} // namespace am::daemon
