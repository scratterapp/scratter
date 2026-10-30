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
// One" demo through cwip-shaka-proxy.appspot.com. kClearFilmUrl plays Google's
// unencrypted copy of the same film, for testing everything but Widevine.
//
// CEF serves this from memory at kTestFilmUrl. It has to be an https address:
// Chromium refuses DRM on file:// pages ("EME use is not allowed on unique
// origins"), and https counts as a secure origin even though nothing is
// fetched over the network for it.
//
// The page logs every license request it makes: its kind, its size, and what
// Widevine put in it about the device (its Widevine version and device
// certificate, which the server sees anyway), and the size of each license
// that comes back. It never logs keys or the license itself. When a server
// says no, the page prints its answer as plain text: that's how issue #21
// found a revoked Widevine. If a license
// is refused (Shaka error 6007) or
// anything else goes wrong, it checks 3 seconds later whether the film is
// still moving. Only if it stopped does the page wait 5 more seconds and load
// the film again from where it was, with a fresh license. It logs "reloading
// the film" so the soak test (tools/soak/soak.py) can tell a refused license
// from a fault of ours.
//
// It also keeps where the film is in sessionStorage. If the page's renderer
// process dies, the daemon loads the page again (cef_host.cpp, #60), and the
// page starts the film from there and logs "carrying on from".
//
// Shaka Player comes from jsdelivr with an integrity hash: Chromium refuses to
// run the file unless its SHA-384 matches, so a broken-into CDN can't put its
// own script in our page. A new Shaka version needs a new hash. Take it from
// the npm package, not from the CDN, after checking the package against npm's
// own record:
//
//   curl -fsSL https://registry.npmjs.org/shaka-player/VERSION | jq -r .dist.integrity
//   curl -fsSLO https://registry.npmjs.org/shaka-player/-/shaka-player-VERSION.tgz
//   echo "sha512-$(openssl dgst -sha512 -binary shaka-player-VERSION.tgz | base64 -w0)"
//   tar -xzf shaka-player-VERSION.tgz package/dist/shaka-player.compiled.js
//   echo "sha384-$(openssl dgst -sha384 -binary package/dist/shaka-player.compiled.js | base64
//   -w0)"
//
// The first and third lines must print the same thing; the last one is the hash.
//
// The daemon drives the page through three functions, amPlay(), amPause() and
// amStop(). The page prints what it's doing to its console, and the daemon
// copies those lines to its own output with a "page:" in front.

#include <string_view>

namespace am::daemon {

// A made-up host under .invalid, a name reserved so it can never be a real site.
inline constexpr std::string_view kTestFilmOrigin = "https://player.invalid";
inline constexpr std::string_view kTestFilmUrl = "https://player.invalid/test-film.html";
// The same page asked to play the film's unencrypted copy (--clear-film).
inline constexpr std::string_view kClearFilmUrl = "https://player.invalid/test-film.html?clear";

inline constexpr std::string_view kTestFilmPage = R"html(<!doctype html>
<meta charset="utf-8">
<title>Widevine test film</title>
<video id="film" loop></video>
<script src="https://cdn.jsdelivr.net/npm/shaka-player@4.11.2/dist/shaka-player.compiled.js"
        integrity="sha384-9Ggzd10pFFpeKcDGCcHkCH+qpmRonKnNcMzC5lCtdQ9zJkocFw/o4Yv0Np8QcfdH"
        crossorigin="anonymous"></script>
<script>
// ?clear asks for Google's unencrypted copy of the same film.
const clear = location.search === '?clear';
const manifest = clear
  ? 'https://storage.googleapis.com/wvmedia/clear/h264/tears/tears.mpd'
  : 'https://storage.googleapis.com/wvmedia/cenc/h264/tears/tears.mpd';
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

// Widevine's messages are protobufs, Google's binary format: a list of
// fields, each a field number and a value. This splits one level of a message
// into its fields: numbers become { number, value }, nested messages and text
// become { number, bytes }.
function protobufFields(bytes) {
  const fields = [];
  let i = 0;
  const varint = () => { // a number written 7 bits per byte
    let value = 0;
    for (let shift = 0; i < bytes.length; shift += 7) {
      const b = bytes[i++];
      value += (b & 0x7f) * 2 ** shift;
      if (!(b & 0x80)) break;
    }
    return value;
  };
  while (i < bytes.length) {
    const key = varint();
    const number = Math.floor(key / 8);
    const wire = key % 8;
    if (wire === 0) {
      fields.push({ number, value: varint() });
    } else if (wire === 2) {
      const length = varint();
      fields.push({ number, bytes: bytes.subarray(i, i + length) });
      i += length;
    } else if (wire === 1 || wire === 5) {
      i += wire === 1 ? 8 : 4; // fixed-size numbers we don't need
    } else {
      break; // not a protobuf after all
    }
  }
  return fields;
}

// Says what kind of Widevine message the CDM asked the page to send. The field
// numbers are from Widevine's public license_protocol.proto: SignedMessage
// field 1 is its type and field 2 the message; in a LicenseRequest, field 1 is
// the device's identity in the clear and field 8 the same identity encrypted;
// in the identity, field 2 is the device certificate (a SignedDrmCertificate
// whose field 1 is the DrmCertificate: field 3 when it was made, field 5 the
// "system ID" Google gave this kind of device) and each field 3 is a
// name=value pair describing the device.
function describeWidevine(bytes) {
  const signed = protobufFields(bytes);
  const type = signed.find((f) => f.number === 1)?.value;
  if (type === 4) return 'a service certificate request';
  if (type !== 1) return `not a license request (message type ${type})`;
  const request = protobufFields(signed.find((f) => f.number === 2)?.bytes ?? new Uint8Array());
  if (request.some((f) => f.number === 8)) return 'identity encrypted (privacy mode)';
  const identity = request.find((f) => f.number === 1);
  if (!identity) return 'no identity in it';
  const text = (fields, n) => new TextDecoder().decode(fields.find((f) => f.number === n)?.bytes);
  const details = protobufFields(identity.bytes)
    .filter((f) => f.number === 3)
    .map((f) => protobufFields(f.bytes))
    .map((pair) => `${text(pair, 1)}=${text(pair, 2)}`);
  const signedCertificate = protobufFields(protobufFields(identity.bytes)
    .find((f) => f.number === 2)?.bytes ?? new Uint8Array());
  const certificate = protobufFields(signedCertificate.find((f) => f.number === 1)?.bytes ??
                                     new Uint8Array());
  const made = certificate.find((f) => f.number === 3)?.value;
  const systemId = certificate.find((f) => f.number === 5)?.value;
  const date = made ? new Date(made * 1000).toISOString().slice(0, 10) : 'unknown';
  return `identity NOT encrypted: ${details.join(', ')}; ` +
         `device certificate: system ID ${systemId}, made ${date}`;
}

// The words of an HTML page, without its tags and styles.
function pageText(html) {
  return html.replace(/<style[\s\S]*?<\/style>/g, ' ').replace(/<[^>]*>/g, ' ')
    .replace(/\s+/g, ' ').trim().slice(0, 300);
}

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
    drm: { servers: clear ? {} : { 'com.widevine.alpha': license } },
  });
  // Log every license request and what kind it is (the first license, a
  // renewal, ...), to see which one a server refuses, and the size of each
  // answer that came back.
  const LICENSE = shaka.net.NetworkingEngine.RequestType.LICENSE;
  player.getNetworkingEngine().registerRequestFilter((type, request) => {
    if (type !== LICENSE) return;
    const kind = request.licenseRequestType || 'unknown kind';
    const body = request.body ? shaka.util.BufferUtils.toUint8(request.body) : new Uint8Array();
    console.log(`license request: ${kind}, ${body.byteLength} bytes, ${describeWidevine(body)}`);
  });
  player.getNetworkingEngine().registerResponseFilter((type, response) => {
    if (type !== LICENSE) return;
    console.log(`license answer: ${response.data.byteLength} bytes`);
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
    // A refused license (6007) carries the HTTP error (1001), whose third
    // item is what the server sent back. Google's servers send an HTML page,
    // so print just its words.
    const http = error.code === 6007 && error.data && error.data[0];
    if (http && http.code === 1001 && typeof http.data[2] === 'string') {
      console.log(`the license server answered HTTP ${http.data[1]}: ${pageText(http.data[2])}`);
    }
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
    const kind = clear ? 'unencrypted' : 'Widevine';
    console.log(`playing the ${kind} test film, audio ${track ? track.audioCodec : 'unknown'}`);
  });
  film.addEventListener('pause', () => console.log('paused'));
  // Where the film is, once a second. sessionStorage lives in the browser
  // process, so it outlives a renderer that dies, and the daemon's reload
  // picks up from here (#60). A fresh daemon starts a fresh session.
  let saved = -1;
  film.addEventListener('timeupdate', () => {
    const second = Math.floor(film.currentTime);
    if (second !== saved) {
      saved = second;
      sessionStorage.setItem('film-position', String(film.currentTime));
    }
  });
  const resumeAt = Number(sessionStorage.getItem('film-position')) || 0;

  try {
    await player.load(manifest, resumeAt > 0 ? resumeAt : undefined);
  } catch (e) {
    console.log(`couldn't load the film: Shaka error ${e.code}`);
    return;
  }
  if (resumeAt > 0) {
    console.log(`carrying on from ${resumeAt.toFixed(1)} s after the page was loaded again`);
  }
  ready = true;
  if (wanted === 'play') film.play();
}

main();
</script>
)html";

} // namespace am::daemon
