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
// servers refuse our very first license request (Angel One's with HTTP 500
// ACCESS_DENIED, this one with 400), so only the film's unencrypted first
// seconds play; why is still open (docs/learning/cef-host.md). kClearFilmUrl
// plays Google's unencrypted copy of the same film in the meantime.
//
// CEF serves this from memory at kTestFilmUrl. It has to be an https address:
// Chromium refuses DRM on file:// pages ("EME use is not allowed on unique
// origins"), and https counts as a secure origin even though nothing is
// fetched over the network for it.
//
// Before the first license, the page asks the license server for its "service
// certificate" and hands it to Widevine, so Widevine encrypts the device's
// identity in its license requests ("privacy mode"). A real Chrome asks for
// this certificate itself; issue #21 suspects ours doesn't, and that the server
// refuses requests that show the identity in the clear.
// docs/learning/widevine-license.md explains the exchange.
//
// The page logs every license request it makes: its kind, its size, and what
// kind of Widevine message it is (a certificate request, or a license request
// with the identity encrypted or not; if not, the device details Widevine put in
// it, which the server sees anyway). It never logs keys or the license itself,
// only its size. When a server says no, the page prints its answer as plain
// text. If a license is refused (Shaka error 6007) or
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
// The same page asked to play the film's unencrypted copy (--clear-film).
inline constexpr std::string_view kClearFilmUrl = "https://player.invalid/test-film.html?clear";

inline constexpr std::string_view kTestFilmPage = R"html(<!doctype html>
<meta charset="utf-8">
<title>Widevine test film</title>
<video id="film" loop></video>
<script src="https://cdn.jsdelivr.net/npm/shaka-player@4.11.2/dist/shaka-player.compiled.js"></script>
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
// the identity's field 3 holds name=value pairs describing the device.
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
  return `identity NOT encrypted: ${details.join(', ')}`;
}

// The words of an HTML page, without its tags and styles.
function pageText(html) {
  return html.replace(/<style[\s\S]*?<\/style>/g, ' ').replace(/<[^>]*>/g, ' ')
    .replace(/\s+/g, ' ').trim().slice(0, 300);
}

// Asks the license server for its service certificate by sending the 2-byte
// Widevine message that means "send me your certificate" (a SignedMessage of
// type 4). Returns null if it couldn't, and the film then starts without one.
async function serviceCertificate() {
  try {
    const response = await fetch(license, { method: 'POST', body: new Uint8Array([0x08, 0x04]) });
    if (!response.ok) {
      console.log(`no service certificate: the license server answered HTTP ${response.status}: ` +
                  pageText(await response.text()));
      return null;
    }
    const certificate = new Uint8Array(await response.arrayBuffer());
    console.log(`got the license server's service certificate, ${certificate.byteLength} bytes`);
    return certificate;
  } catch (e) {
    console.log(`no service certificate: ${e}`);
    return null;
  }
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
  const certificate = clear ? null : await serviceCertificate();
  player.configure({
    manifest: { disableVideo: true, disableText: true },
    preferredAudioCodecs: ['mp4a.40.2'],
    drm: {
      servers: clear ? {} : { 'com.widevine.alpha': license },
      advanced: certificate ? { 'com.widevine.alpha': { serverCertificate: certificate } } : {},
    },
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
