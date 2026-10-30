# Windows side: start here

For kero (`@kerelosgayed`) and any agent working on `src/pal/win32/`.

robbie is merging the shared interface PRs below without waiting for a Windows
review, so the Linux side doesn't stall. That makes this page the catch-up. Every
interface here was written with Windows in mind, but none of it has been tried on
Windows yet. If something doesn't fit, say so before Windows code builds on it:
changing an interface now costs one PR, changing it later costs a rewrite on both
sides.

Read [`CLAUDE.md`](../../CLAUDE.md) first for the house rules (no exceptions, no
RTTI, warnings are errors, no OS `#ifdef` outside `src/pal/`). The roadmap issue
#6 tracks what's done.

> **About issue numbers:** the project moved to a fresh repository on 2026-10-17. Open
> issues came along with new numbers (#1 to #14), and this page uses those. Finished
> issues and PRs (like #4, #53 or #81) keep their old numbers from the first tracker,
> which is archived and private, so they won't open on GitHub.

## Interface decisions your code has to fit

All of them share one shape: one class declared in `src/pal/interfaces/`, a private
`Impl` (pimpl) so `<windows.h>` stays out of the header, no virtuals, no `#ifdef`.
Each platform's `.cpp` defines the members and CMake compiles only one. Errors are
`std::expected<T, std::error_code>`; `system_category` carries `GetLastError()`.

| Interface | PR | What it decides for Windows |
| --- | --- | --- |
| Reactor (`reactor.hpp`) | #32 | The reactor does no I/O. epoll reports readiness, IOCP reports completions, and `IoEvent` carries both halves. Windows fills `overlapped`, `bytes` and `error`. `wake()` and `stop()` are the only cross-thread calls. One timer. |
| IpcChannel (`ipc_channel.hpp`) | #37 | One whole message per `send()` / `receive()`, never blocking. Results are `Done`, `WouldBlock` or `PeerClosed`. The expected Windows shape: one overlapped `ReadFile` always pending, the readable callback fires on its completion, and `send()` returns `WouldBlock` while a `WriteFile` is in flight. The header has an error table for `connect()` that both platforms must match (busy daemon maps to `resource_unavailable_try_again`). |
| ShmMapping (`shm_mapping.hpp`) | #34 | `create(name, size)` for the daemon, `open_read_only(name, size)` for the UI. The base name is `scratterd_telemetry_v2` and Windows adds `Local\`. The creator keeps its section handle open in `native_handle_` for the mapping's lifetime, because a region's name lasts only while a handle to it is open; a reader closes its handle before `open_read_only()` returns, since the mapped view alone keeps the memory readable. On Windows `create()` also returns `result_out_of_range` for an existing region smaller than `size` (a region can't grow), comparing whole pages. |
| AudioSink (`audio_sink.hpp`, `realtime.hpp`) | #38 | Pull model: the OS audio thread calls a `noexcept` render callback with a `std::span<float>` to fill. Ask for 48 kHz and report what you got in `format()`. Memory pinning is its own call (`lock_memory`, which is `VirtualLock` on Windows). The WASAPI sink sets MMCSS priority on its own event thread. `win32/wasapi_sink.cpp` (#14) does all of it on one thread of its own: `IAudioClient3` low-latency shared mode at the mix rate when the mix is stereo (reported in `format()`), otherwise `AUTOCONVERTPCM` at the requested 48 kHz; start/stop are requests to that thread; MMCSS "Pro Audio". `mmcss_priority.cpp` is `VirtualLock`, raising the working set when the quota is too low. Following a default-device change and exclusive mode (#13) are still to do. |
| CPU checks in shared code | #31 | CPU and compiler `#if`s (`__x86_64__`, `_MSC_VER`, `__ARM_FEATURE_*`) are allowed in `src/common/`; OS checks still aren't. CRC32C uses `__cpuid` to detect SSE4.2 on MSVC. This changes a CLAUDE.md rule. |
| SeqLock (`telemetry_shm.hpp`) | #30 | The payload is 40 atomic words with release stores and acquire loads instead of fences. The reader doesn't call `_mm_pause()`, since am-ui calls `try_read()` once per frame and there's no spin loop. With #31's rule it could come back behind a CPU check. |
| ShutdownSignal (`shutdown_signal.hpp`) | #50 | Ctrl-C/SIGTERM become a callback on the reactor thread. Linux blocks the signals and reads a signalfd. On Windows, `SetConsoleCtrlHandler` runs its handler on a thread of its own, so `console_shutdown.cpp` (#22) sets a flag and calls `wake()`, and the reactor's wake callback runs the callback. That takes the reactor's only wake callback. |
| CredentialVault (`credential_vault.hpp`) | #62 | Login storage for the two tokens. `open(service)`, then `store` / `load` / `remove` by name. `load()` returns an empty `optional` when nothing is saved (not an error), `remove()` of a missing secret succeeds, and an empty name or a secret that's empty or contains `'\0'` is `invalid_argument`. Service is `kVaultService` (the product name). Calls may block. `tests/test_credential_vault.cpp` is the contract. `win32/dpapi_vault.cpp` (#10) passes it: a generic credential with target `"<service>/<name>"`, kept on this machine only, its blob encrypted with `CryptProtectData` (the target name as entropy). Names can't contain `/` on Windows, and a secret over about 2300 bytes is `value_too_large` (Credential Manager's 2560-byte cap after DPAPI's overhead); the comments in `win32/dpapi_vault.cpp` have the numbers. |
| MediaControls (`media_controls.hpp`) | #65 | Media keys and the Windows 11 flyout through SMTC. `open(reactor, callback, name)`; every desktop request (play, pause, play/pause, stop, next, previous, seek to a position) reaches the daemon as one `MediaRequest` callback **on the reactor thread**, so SMTC's `ButtonPressed` and `PlaybackPositionChangeRequested` handlers, which run on a WinRT thread, have to hand over to the reactor first. Out: `set_track` (title, artist, album, artwork URL, length), `set_playback_state`, `set_position` (called every tick, so only store it), `seeked` (push a timeline update), `set_volume` (ignore it: SMTC has no volume, and Windows never sends `SetVolume`). Setters return nothing and never wait. `tests/test_media_controls.cpp` is the contract #9 must pass. The daemon already calls it (`main_daemon.cpp`, step 6; Linux's MPRIS version is #56), so #9 only replaces `smtc_controls.cpp`. |
| Browser files (`browser_files.hpp`) | #76 | Three lookups for the CEF host: `executable_dir()` (CEF's helper program sits next to the daemon), `browser_profile_dir()` (CEF's profile, created if missing; suggested `%LOCALAPPDATA%\<app>\browser`) and `find_widevine_cdm()` (a folder with `manifest.json` a browser installed). `win32/browser_files.cpp` has all three: the exe path from `GetModuleFileNameW`, the profile at `%LOCALAPPDATA%\<app>\browser`, and the newest CDM from Chrome's component folder, Chrome's own program folder (Chrome bundles one there), Chromium or Edge. Finding the CDM isn't enough on Windows, though: Chromium there ignores the Linux hint file and only registers Widevine through the component updater (`RegisterWidevineCdmComponent` in `chrome/browser/component_updater/registration.cc`), and the host's `--disable-component-update` switch turns that whole path off (`chrome_browser_main.cc`), so with a stock CEF `--test-film` fails with Shaka error 6001. Our CEF build fixes that with `third_party/cef-patches/widevine-cdm-dir.patch`: it registers the folder the host passes as `--widevine-cdm-dir`, and the updater stays off (how to apply it: `docs/CEF.md`). The host and its helper get `CefMainArgs` from `cef_main_args()` (`cef_main_args.hpp`), which on Windows is `GetModuleHandleW(nullptr)`; CEF then reads Chromium's switches from the real command line, the daemon's own arguments included. |
| Product name, legal limits | #43 | The product name lives only in `AM_APP_NAME` (CMake) and `am::common::kAppName` / `kDaemonName`, so never hard-code "scratterd" in pipe, mapping or SMTC names. Never ship or download the Widevine CDM; load the user's own copy. Never add a way to record or export the decoded audio. |

**Telemetry change from #61:** `TelemetrySnapshot` gained `uint8_t volume_percent`
in what used to be the first byte of `reserved_pad` (now a `uint8_t`). It's still 320
bytes and no other offset moved, so nothing on your side changes unless you read
the volume.

## Questions waiting on you

From the PR bodies. Answer on the PR, even after it's merged.

**#32 Reactor**

1. Does the IOCP half of `IoEvent` have everything `named_pipe_ipc.cpp` needs?
2. Threadpool timers fire on a pool thread. The plan is for `iocp_reactor.cpp` to
   post a packet to the port so the timer callback still runs on the reactor
   thread. Does that sound right?
3. IOCP can't un-associate a handle, so the header says to `CancelIoEx` and drain
   before `unwatch()`. Is that what you'd do?
4. Later (phase 4): curl needs socket readiness on Windows too. The guess is
   `WSAEventSelect` or AFD polling inside the IOCP reactor, behind the same
   `watch()`. No answer needed yet.

**#37 IpcChannel**

1. Does the pending-`ReadFile` sketch above fit how you'd write the pipe?
2. Is it OK that there's no peer-credential check (`GetNamedPipeClientProcessId`),
   with pipe permissions doing access control instead?

**#34 ShmMapping**

1. Anything awkward about the contract for #8?

**#38 AudioSink**

1. Is "ask for 48 kHz, report what you got" OK, given WASAPI shared mode runs at
   the mix format's rate unless you pass `AUTOCONVERTPCM`?
2. HRESULTs: is `system_category` (which covers the `HRESULT_FROM_WIN32` ones)
   enough, or do we want a small HRESULT error category later?
3. Exclusive mode (#13) isn't in the interface yet. The plan is an option on
   `open()` when we get there. OK?

**#30 / #31**

1. Are you OK with per-word release/acquire in the SeqLock?
2. Are you OK with allowing CPU and compiler checks in shared code?

## Open Windows-side decisions

- **Pipe name (settled in [#16](https://github.com/scratterapp/scratter/pull/16)).**
  One name on both platforms from `kDaemonEndpoint`, without `_v2` (the frame
  header already carries the version): Linux uses `scratterd.sock`, Windows
  `\\.\pipe\scratterd.<SID>`, so each user on the machine gets their own pipe.
  "Security" in `ipc_channel.hpp` has the rest.
- **Windows sanitizer preset.** Linux has `linux-asan` and `linux-tsan`; Windows
  only has `windows-msvc-debug` and `windows-msvc-release`. Do we add a preset with
  MSVC's `/fsanitize=address`, and does it run in CI? MSVC has no ThreadSanitizer,
  so the race tests only run under `linux-tsan`.

**#52 CredentialVault**

1. Does "target `<service>/<name>`, `CRED_TYPE_GENERIC`, DPAPI blob" fit how you'd
   write #10? The header leaves the naming to each platform, so it's your call.
2. Credential Manager caps a credential's blob at `CRED_MAX_CREDENTIAL_BLOB_SIZE`
   (2560 bytes). A DPAPI-wrapped token should fit, but if one doesn't, say so and
   we'll add an error for it to the header.
3. `dpapi_vault.cpp` is now a "not supported" stub like the others. Replace the
   whole file.

**#55 MediaControls**

1. SMTC raises its events on a WinRT thread, and the callback has to run on the
   reactor thread. The only cross-thread door into the reactor is `wake()`, which
   has a single callback, and the Ctrl-C handler (`console_shutdown.cpp`, #22)
   already uses it. Do you want a general "run this on the reactor thread" call on
   `Reactor`, or will you post your own completion packets inside
   `iocp_reactor.cpp`?
2. The spec puts SMTC on a hidden message-only window
   (`ISystemMediaTransportControlsInterop::GetForWindow`). Does that work with
   `HWND_MESSAGE`, and does the window need a message loop the IOCP reactor
   doesn't run?
3. Artwork: the header passes an `https://` URL. `RandomAccessStreamReference::CreateFromUri`
   should take it as-is. Say so if it doesn't.

**#50 Daemon skeleton**

1. To keep the Windows build linking, #50 filled your stub files
   (`iocp_reactor.cpp`, `named_pipe_ipc.cpp`, `win32_shm.cpp`, `wasapi_sink.cpp`,
   `mmcss_priority.cpp`, and a new `console_shutdown.cpp`) with stubs that return
   `errc::function_not_supported`. Replace each whole file when you implement it.
   OK with that?
2. Does `ShutdownSignal` fit `SetConsoleCtrlHandler` (handler on its own thread,
   waking the reactor)?

## Your issues, in order

| Issue | What | Blocked by |
| --- | --- | --- |
| #8 | File-mapping shared memory, `win32_shm.cpp` | #6 (ShmMapping, PR #34) |
| #14 | IOCP reactor, `iocp_reactor.cpp` | #12 (Reactor, PR #32) |
| #22 | Ctrl-C and console close, `console_shutdown.cpp` (the daemon's first stop on Windows) | nothing |
| #12 | Message-mode named pipe, `named_pipe_ipc.cpp` | #11 (IpcChannel, PR #37) and #14 |
| #14 | WASAPI shared low-latency sink, `wasapi_sink.cpp` | #18 (AudioSink, PR #38) |
| #13 | WASAPI exclusive mode | #14 |
| #10 | DPAPI + Credential Manager vault, `dpapi_vault.cpp` | #52 (CredentialVault) |
| #9 | SMTC media controls, `smtc_controls.cpp` | #55 (MediaControls) and #14 |
| #8 | CEF built with AAC on Windows, `browser_files.cpp`, the CEF host on Windows | #76 (CEF host), and #14 to hear anything |

#8 and #14 can start as soon as their interface PR merges. The Linux versions are
up as working references: POSIX shm in #35, epoll in #33, the SEQPACKET channel in
#39 and the PipeWire sink in #42. Each issue's "done when" line names the test
Windows has to pass, usually the same one Linux uses.

## How to push back

- Comment on the PR, merged or not. That's the quickest.
- For anything bigger, open an issue labelled `shared` and tag `@cocoa55`.
- Interface changes go in their own PR to `src/pal/interfaces/` or `src/common/`,
  before the Windows code that needs them.
