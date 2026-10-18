# Getting a Widevine license (#21)

`scratterd --test-film` plays Google's Widevine-encrypted test film, but on
robbie's machine the license server says no to our very first license request,
so only the film's unencrypted first seconds play. Helium, on the same machine
with the same Widevine, plays Widevine films fine. This page explains what a
license request is, what we think goes wrong in ours, and what the page now
does about it.

## The conversation, step by step

Nothing in our code ever sees a key. The browser, the Widevine CDM (Google's
closed library that does the decrypting) and the license server talk, and the
page only carries the messages between them:

```
1. the page loads the film         Shaka sees the film is encrypted
2. Widevine writes a request       "I'm this device, I want the key for this film"
3. the page posts it to the server proxy.uat.widevine.com, an ordinary HTTPS POST
4. the server checks the device    is this a real, trusted Widevine device?
5. the server sends the license    the key, locked so only that device can open it
6. Widevine decrypts the film      the decoded sound reaches CefAudioHandler
```

On our side, step 4 fails: the server answers step 3 with an error (400 from
Widevine's server, 500 ACCESS_DENIED from the one Angel One used), so step 5
never happens.

## The device's identity and "privacy mode"

The request in step 2 carries the device's **identity**: a certificate built
into Widevine, plus a few name=value details such as the CDM version and the
operating system. That identity is the same every time, so anyone watching the
network could follow one computer around the web with it.

So since Chrome 58, Widevine hides it. Before the first license request,
Chrome's Widevine sends a tiny 2-byte message instead, "send me your
certificate". The server answers with its **service certificate**, and Widevine
then encrypts its identity with it, so only the license server can read it.
That's called privacy mode.

In our logs, Widevine's first and only message was a 1722-byte license
request. No 2-byte certificate request came before it. That's the suspect:
our CEF's Widevine sends its identity in the clear, and Google's servers now
refuse that from a desktop browser. Why our CEF skips the step while Helium
doesn't is still unknown.

## What the server really said

With the page printing the server's answer as text, the next run on robbie's
machine (2026-10-18) gave the real reason:

```
status=ACCESS_DENIED, internal_status=DRM_DEVICE_CERTIFICATE_REVOKED
```

Google has revoked the device certificate our Widevine sends. Privacy mode
wasn't the cause: the server read the identity fine, and Widevine still sent
it in the clear even with the service certificate in place. A fresh browser
profile changed nothing, so it isn't old data the daemon kept.

Helium runs the same Widevine version and gets licenses, so the next question
is what Helium sends that we don't. `tools/test_page/serve.py` serves
the daemon's own page on `http://localhost:8000`, so Helium can run exactly
the same page against the same server and print the same log lines in its
console, including which device certificate it sends (its "system ID" and
when it was made). The rest of this page explains how we got here.

## What the page does now

The page (`src/daemon/cef_host/test_film_page.hpp`) asks for the service
certificate itself, before loading the film, and gives it to Shaka:

```js
const response = await fetch(license, { method: 'POST', body: new Uint8Array([0x08, 0x04]) });
...
drm: { advanced: { 'com.widevine.alpha': { serverCertificate: certificate } } }
```

`0x08 0x04` is the whole "send me your certificate" message. The server's
answer wraps the certificate in a message of its own (type 5), and
`setServerCertificate()` wants the certificate alone, so the page unwraps it
first. The first try on robbie's machine passed the whole answer and Widevine
refused it (Shaka error 6004, "invalid server certificate"), which stopped the
film from loading at all. Now the page tries the certificate alone, then the
whole answer, then no certificate, so the film always starts. Shaka passes the
answer to the browser with `MediaKeys.setServerCertificate()`, the standard
web way of doing what Chrome's Widevine does by itself. This is ordinary
use of the browser's DRM API: nothing is bypassed and no key is touched.

It also logs what each message is, so one run tells us which case we're in:

| Log line | Meaning |
| --- | --- |
| `got the license server's service certificate, N bytes` | The certificate arrived. |
| `Widevine refused the N-byte certificate (Shaka error 6004)` | Widevine didn't accept that form; the page tries the next. |
| `loaded the film with a N-byte certificate` | Widevine accepted it; `no certificate` means it took none. |
| `no service certificate: the license server answered HTTP 400: ...` | Even this tiny request is refused, so the problem is the HTTP request itself, not Widevine. |
| `license request: ..., identity encrypted (privacy mode)` | Widevine used the certificate. |
| `license request: ..., identity NOT encrypted: name=value, ...; device certificate: system ID N, made DATE` | It didn't. The details are what the server sees about the device. |
| `license answer: N bytes` | The server sent a license. It worked. |
| `the license server answered HTTP 400: ...` | The server's own words, without the HTML. |

## Reading a protobuf by hand

Widevine's messages are **protobufs**, Google's compact binary format. A
protobuf is a list of fields, and each field starts with one number that packs
two things: the field's number and its "wire type" (`key = number * 8 + type`).
Type 0 is a number, type 2 is a run of bytes (text or a message inside the
message). Numbers are written 7 bits per byte, with the top bit meaning "more
bytes follow".

So `08 04` reads as: key `0x08` = field 1, type 0; value `4`. Field 1 of a
Widevine `SignedMessage` is its type, and type 4 is
`SERVICE_CERTIFICATE_REQUEST`. That's the whole message.

`protobufFields()` in the page does exactly this for one level, and
`describeWidevine()` walks the few fields we care about, using the field
numbers from Widevine's public `license_protocol.proto`:

- `SignedMessage`: field 1 the type (1 is a license request, 4 a certificate
  request), field 2 the message inside.
- `LicenseRequest`: field 1 the identity in the clear, field 8 the identity
  encrypted.
- the identity: field 2 the device certificate, each field 3 one name=value
  pair.
- the device certificate (a `SignedDrmCertificate`): field 1 the certificate
  itself, in which field 3 is when it was made (seconds since 1970) and
  field 5 the system ID, the number Google gives each kind of device. A
  revocation names system IDs or certificates, so this is what to compare.

## What you learn

- How DRM in a browser really works: the page only carries messages, and the
  key never leaves Widevine.
- Why a device identity is a privacy problem, and how a server certificate
  solves it.
- Reading a binary format byte by byte, and what a protobuf looks like on the
  wire.
- Debugging something you can't run yourself: make one run print enough to
  tell every likely cause apart.
