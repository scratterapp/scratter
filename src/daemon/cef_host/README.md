# cef_host

Headless CEF for the daemon: `CefApp`, `CefClient`, `CefAudioHandler` (copies the
page's decoded audio into the ring buffer) and the helper program for CEF's
subprocesses. See `docs/SPECIFICATION.md` sections 3.1.1 and 3.1.2, and
[`docs/CEF.md`](../../../docs/CEF.md) for how it fits
together.

Built only with the `AM_WITH_CEF` CMake option (the `linux-cef` preset). Without
it, `cef_host_off.cpp` stands in. Still to come: Apple Music through MusicKit JS
(#4), the login window (#3) and the VMP signature check.

Owners: @cocoa55 @kerelosgayed
