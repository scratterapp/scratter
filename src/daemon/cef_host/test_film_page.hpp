#pragma once

// The page the daemon plays until Apple Music arrives (issue #4): Google's
// public Widevine test film "Tears of Steel" (12 minutes, looped), audio
// only, through Shaka Player. Google encrypts it with Widevine and runs a test
// license server that gives the key to anyone, so it exercises the same path
// as an Apple Music track (AAC audio, a Widevine license, protected audio
// reaching CefAudioHandler) without an Apple account. Android's ExoPlayer and
// dash.js test against the same stream and server.
//
// The #5 experiment and the first version of this page used Shaka's "Angel
// One" demo through cwip-shaka-proxy.appspot.com. On robbie's machine both
// servers answer the first license request, then refuse a second one a few
// seconds in (Angel One's with HTTP 500 ACCESS_DENIED, this one with 400).
//
// CEF serves this from memory at kTestFilmUrl. It has to be an https address:
// Chromium refuses DRM on file:// pages ("EME use is not allowed on unique
// origins"), and https counts as a secure origin even though nothing is
// fetched over the network for it.
//
// The page logs every license request it makes, with its kind, to find out
// what that second request is. If one is refused (Shaka error 6007) or
// anything else goes wrong, it checks 3 seconds later whether the film is
// still moving. Only if it stopped does the page wait 5 more seconds and load
// the film again from where it was, with a fresh license. It logs "reloading
// the film" so the soak test (tools/soak/soak.py) can tell a refused license
// from a fault of ours.
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
const manifest = 'https://storage.googleapis.com/wvmedia/cenc/h264/tears/tears.mpd';
const license = 'https://proxy.uat.widevine.com/proxy?provider=widevine_test';
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
  // Log every license request and what kind it is (the first license, a
  // renewal, ...), to see which one a server refuses.
  player.getNetworkingEngine().registerRequestFilter((type, request) => {
    if (type !== shaka.net.NetworkingEngine.RequestType.LICENSE) return;
    const kind = request.licenseRequestType || 'unknown kind';
    const bytes = request.body ? request.body.byteLength : 0;
    console.log(`license request: ${kind}, ${bytes} bytes`);
  });
  // Load the film again from the same place, which asks for a fresh license.
  let reloading = false;
  async function reload(code) {
    if (reloading) return; // errors during the reload itself land here too
    reloading = true;
    const at = film.currentTime;
    console.log(`reloading the film at ${at.toFixed(1)} s after Shaka error ${code}`);
    // Not at once: a server that just said no shouldn't be asked again and again.
    await new Promise((resolve) => setTimeout(resolve, 5000));
    for (;;) {
      try {
        await player.load(manifest, at);
        break;
      } catch (e) {
        console.log(`couldn't reload the film (Shaka error ${e.code}), trying again in 10 s`);
        await new Promise((resolve) => setTimeout(resolve, 10000));
      }
    }
    reloading = false;
    if (wanted === 'play') film.play();
  }
  player.addEventListener('error', (e) => {
    const error = e.detail;
    // error.data says more, such as the HTTP status the license server sent.
    let data = '';
    try { data = JSON.stringify(error.data).slice(0, 300); } catch { /* not plain data */ }
    console.log(`Shaka error ${error.code} (see shaka.util.Error in Shaka's docs): ${data}`);
    if (error.severity !== shaka.util.Error.Severity.CRITICAL) return;
    // Not every critical error stops the film: a refused license renewal
    // doesn't, while the license from before is still good. So look again in
    // 3 seconds, and reload only if the film stopped moving.
    const before = film.currentTime;
    setTimeout(() => {
      if (wanted === 'play' && film.currentTime === before) {
        reload(error.code);
      } else {
        console.log(`the film plays on after Shaka error ${error.code}`);
      }
    }, 3000);
  });
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
