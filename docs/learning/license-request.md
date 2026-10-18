# Getting a Widevine license (#21)

For two days `scratterd --test-film` played the Widevine test film's first few
seconds and then went silent. Google's license server was refusing every
license we asked for. The cause turned out to be outside our code: the
Widevine CDM installed on robbie's machine (4.10.2934.0) was old enough that
Google had revoked it. Updating the system package (`chromium-widevine`, to
4.10.3050.0) fixed it. This page explains what a license request is, how we
found the cause, and what the page now logs so the next refusal explains
itself.

## The conversation, step by step

Nothing in our code ever sees a key. The browser, the Widevine CDM (Google's
closed library that does the decrypting) and the license server talk, and the
page only carries the messages between them:

```
1. the page loads the film         Shaka sees the film is encrypted
2. Widevine writes a request       "I'm this device, I want the key for this film"
3. the page posts it to the server proxy.uat.widevine.com, an ordinary HTTPS POST
4. the server checks the device    is this a real Widevine that isn't revoked?
5. the server sends the license    the key, locked so only that device can open it
6. Widevine decrypts the film      the decoded sound reaches CefAudioHandler
```

Our requests failed at step 4.

## Device certificates and revocation

The request in step 2 carries the device's identity: a **device certificate**
built into that version of Widevine, plus a few name=value details such as the
Widevine version and the operating system. A certificate here is like an ID
card signed by Google.

Every few months Google ships a new Widevine with a new certificate, and some
months later revokes the old ones. After that, the license server refuses any
request carrying an old certificate, whatever the site or browser. Desktop
Chrome never notices, because its component updater keeps Widevine current.
Our daemon turns that updater off (`--disable-component-update`, because we
don't download anything ourselves), so Widevine is only as new as the system
package. On robbie's machine that package hadn't been updated in a while.

## How we found it

The trail, in order, because the wrong turns are part of the lesson:

1. **The symptom.** Sound for 3 to 10 seconds, then silence. Test films start
   with a few unencrypted seconds, so that was all we ever heard. The page's
   log showed one license request, refused with HTTP 400.
2. **Ruled out:** the sandbox (same with `--no-sandbox`) and the copy of
   Widevine (Helium had the same version, and pointing the daemon at Helium's
   copy failed the same way). Helium seemed to play a Widevine demo, so we
   blamed our CEF build. That was the wrong conclusion: we didn't check that
   Helium got past the unencrypted part.
3. **A wrong guess.** Chrome normally hides the device's identity by first
   asking the server for a "service certificate" and encrypting with it
   ("privacy mode"). Ours sent the identity in the clear, so we tried fetching
   that certificate ourselves. Widevine still sent the identity in the clear,
   and the server read it fine either way. Not the cause; that code is gone.
4. **Reading the error properly.** Google's server sends its errors as an HTML
   page, and we had only been printing the first 300 characters of the HTML,
   which held nothing but the page header. Printing the page's words instead
   showed:

   ```
   status=ACCESS_DENIED, internal_status=DRM_DEVICE_CERTIFICATE_REVOKED
   ```

5. **Which certificate?** The page now decodes the request and prints the
   certificate's system ID and the date it was made: `system ID 39208, made
   2025-08-13`. Google revoked every Widevine older than its October 2025
   update in January 2026, so a certificate from August 2025 is one of them.
6. **Checking Helium properly.** `tools/test_page/serve.py` serves the
   daemon's own page on `http://localhost:8000`, so a normal browser can run
   it. Helium couldn't start Widevine on it at all (Shaka error 6001). So its
   earlier "pass" had been the unencrypted lead-in too.
7. **The fix.** Updating `chromium-widevine` from the AUR installed
   4.10.3050.0, with a certificate made 2026-03-12. The next run printed
   `license answer: 1655 bytes`, and the film played on.

The lesson: when a server refuses you, read its whole answer before you guess.
And when you compare against something that "works", check that it really
does.

## What the page logs

| Log line | Meaning |
| --- | --- |
| `license request: license-request, N bytes, identity NOT encrypted: ...; device certificate: system ID N, made DATE` | What Widevine told the server about this device. |
| `license request: ..., identity encrypted (privacy mode)` | The same, hidden from us (Widevine encrypted it for the server). |
| `license answer: N bytes` | The server sent a license. It worked. |
| `the license server answered HTTP 400: ...` | The server's own words, without the HTML. `DRM_DEVICE_CERTIFICATE_REVOKED` means: update Widevine. |

It never logs keys or the license itself, only sizes. Everything it prints
about the device is what the server sees anyway.

If you see the revoked error, check the installed version and update the
package that provides it:

```sh
grep '"version"' /usr/lib/chromium/WidevineCdm/manifest.json
pacman -Qo /usr/lib/chromium/WidevineCdm/manifest.json
```

## Reading a protobuf by hand

Widevine's messages are **protobufs**, Google's compact binary format. A
protobuf is a list of fields, and each field starts with one number that packs
two things: the field's number and its "wire type" (`key = number * 8 + type`).
Type 0 is a number, type 2 is a run of bytes (text, or a message inside the
message). Numbers are written 7 bits per byte, with the top bit meaning "more
bytes follow".

For example, `08 04` reads as: key `0x08` = field 1, type 0; value `4`. Field 1
of a Widevine `SignedMessage` is its type, and type 4 is
`SERVICE_CERTIFICATE_REQUEST`. That's the whole message.

`protobufFields()` in the page does exactly this for one level, and
`describeWidevine()` walks the few fields we care about, using the field
numbers from Widevine's public `license_protocol.proto`:

- `SignedMessage`: field 1 the type (1 is a license request), field 2 the
  message inside.
- `LicenseRequest`: field 1 the identity in the clear, field 8 the identity
  encrypted.
- the identity: field 2 the device certificate, each field 3 one name=value
  pair.
- the device certificate (a `SignedDrmCertificate`): field 1 the certificate
  itself, in which field 3 is when it was made (seconds since 1970) and field
  5 the system ID, the number Google gives each Widevine build.

## What you learn

- How DRM in a browser really works: the page only carries messages, and the
  key never leaves Widevine.
- Why a DRM library expires: certificates, revocation, and what a component
  updater quietly does for a normal browser.
- Reading a binary format byte by byte, and what a protobuf looks like on the
  wire.
- Debugging something you can't run yourself: make one run print enough to
  tell the likely causes apart, and read the whole error first.
