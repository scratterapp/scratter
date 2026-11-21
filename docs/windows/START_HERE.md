# Windows side: start here

For kero (`@kerelosgayed`) and any agent working on `src/pal/win32/`.

Every shared interface below now has a working Windows implementation, and the
daemon, `amctl` and `am-ui` all run on Windows. This page records what each
interface decided for Windows and how the Windows code answered the questions
the interface PRs asked. If a new interface doesn't fit Windows, say so before
Windows code builds on it: changing an interface early costs one PR, changing
it later costs a rewrite on both sides.

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
| AudioSink (`audio_sink.hpp`, `realtime.hpp`) | #38 | Pull model: the OS audio thread calls a `noexcept` render callback with a `std::span<float>` to fill. Ask for 48 kHz and report what you got in `format()`. Memory pinning is its own call (`lock_memory`, which is `VirtualLock` on Windows). The WASAPI sink sets MMCSS priority on its own event thread. `win32/wasapi_sink.cpp` (#14) does all of it on one thread of its own: `IAudioClient3` low-latency shared mode at the mix rate when the mix is stereo (reported in `format()`), otherwise `AUTOCONVERTPCM` at the requested 48 kHz; start/stop are requests to that thread; MMCSS "Pro Audio". `mmcss_priority.cpp` is `VirtualLock`, raising the working set when the quota is too low. Exclusive mode (#13) is in too: `OutputMode::Exclusive` tries float, then 32-, 24- and 16-bit integer PCM at exactly the requested format, converting from a float buffer allocated at setup, and the daemon asks for it with `--exclusive`. It follows the default device too (#94): `IMMNotificationClient` and lost streams (`AUDCLNT_E_DEVICE_INVALIDATED`) both make its thread open the default device again, keeping the format `open()` reported. |
| CPU checks in shared code | #31 | CPU and compiler `#if`s (`__x86_64__`, `_MSC_VER`, `__ARM_FEATURE_*`) are allowed in `src/common/`; OS checks still aren't. CRC32C uses `__cpuid` to detect SSE4.2 on MSVC. This changes a CLAUDE.md rule. |
| SeqLock (`telemetry_shm.hpp`) | #30 | The payload is 40 atomic words with release stores and acquire loads instead of fences. The reader doesn't call `_mm_pause()`, since am-ui calls `try_read()` once per frame and there's no spin loop. With #31's rule it could come back behind a CPU check. |
| ShutdownSignal (`shutdown_signal.hpp`) | #50 | Ctrl-C/SIGTERM become a callback on the reactor thread. Linux blocks the signals and reads a signalfd. On Windows, `SetConsoleCtrlHandler` runs its handler on a thread of its own, so `console_shutdown.cpp` (#22) sets a flag and calls `wake()`, and the reactor's wake callback runs the callback. That takes the reactor's only wake callback. |
| CredentialVault (`credential_vault.hpp`) | #62 | Login storage for the two tokens. `open(service)`, then `store` / `load` / `remove` by name. `load()` returns an empty `optional` when nothing is saved (not an error), `remove()` of a missing secret succeeds, and an empty name or a secret that's empty or contains `'\0'` is `invalid_argument`. Service is `kVaultService` (the product name). Calls may block. `tests/test_credential_vault.cpp` is the contract. `win32/dpapi_vault.cpp` (#10) passes it: a generic credential with target `"<service>/<name>"`, kept on this machine only, its blob encrypted with `CryptProtectData` (the target name as entropy). Names can't contain `/` on Windows, and a secret over about 2300 bytes is `value_too_large` (Credential Manager's 2560-byte cap after DPAPI's overhead); the comments in `win32/dpapi_vault.cpp` have the numbers. |
| MediaControls (`media_controls.hpp`) | #65 | Media keys and the Windows 11 flyout through SMTC. `open(reactor, callback, name)`; every desktop request (play, pause, play/pause, stop, next, previous, seek to a position) reaches the daemon as one `MediaRequest` callback **on the reactor thread**, so SMTC's `ButtonPressed` and `PlaybackPositionChangeRequested` handlers, which run on a WinRT thread, have to hand over to the reactor first. Out: `set_track` (title, artist, album, artwork URL, length), `set_playback_state`, `set_position` (called every tick, so only store it), `seeked` (push a timeline update), `set_volume` (ignore it: SMTC has no volume, and Windows never sends `SetVolume`). Setters return nothing and never wait. `tests/test_media_controls.cpp` is the contract #9 must pass. The daemon already calls it (`main_daemon.cpp`, step 6; Linux's MPRIS version is #56), so #9 only replaced `smtc_controls.cpp`. It has: SMTC on a hidden top-level window with its own message-loop thread, raw WinRT through WRL (C++/WinRT throws), and button and seek events handed to the reactor through a pipe it watches. `tests/test_smtc_controls.cpp` presses Play, Pause and Seek through Windows' session manager. |
| Browser files (`browser_files.hpp`) | #76 | Three lookups for the CEF host: `executable_dir()` (CEF's helper program sits next to the daemon), `browser_profile_dir()` (CEF's profile, created if missing; suggested `%LOCALAPPDATA%\<app>\browser`) and `find_widevine_cdm()` (a folder with `manifest.json` a browser installed). `win32/browser_files.cpp` has all three: the exe path from `GetModuleFileNameW`, the profile at `%LOCALAPPDATA%\<app>\browser`, and the newest CDM from Chrome's component folder, Chrome's own program folder (Chrome bundles one there), Chromium or Edge. Finding the CDM isn't enough on Windows, though: Chromium there ignores the Linux hint file and only registers Widevine through the component updater (`RegisterWidevineCdmComponent` in `chrome/browser/component_updater/registration.cc`), and the host's `--disable-component-update` switch turns that whole path off (`chrome_browser_main.cc`), so with a stock CEF `--test-film` fails with Shaka error 6001. Our CEF build fixes that with `third_party/cef-patches/widevine-cdm-dir.patch`: it registers the folder the host passes as `--widevine-cdm-dir`, and the updater stays off (how to apply it: `docs/CEF.md`). The host and its helper get `CefMainArgs` from `cef_main_args()` (`cef_main_args.hpp`), which on Windows is `GetModuleHandleW(nullptr)`; CEF then reads Chromium's switches from the real command line, the daemon's own arguments included. |
| Product name, legal limits | #43 | The product name lives only in `AM_APP_NAME` (CMake) and `am::common::kAppName` / `kDaemonName`, so never hard-code "scratterd" in pipe, mapping or SMTC names. Never ship or download the Widevine CDM; load the user's own copy. Never add a way to record or export the decoded audio. |

**Telemetry change from #61:** `TelemetrySnapshot` gained `uint8_t volume_percent`
in what used to be the first byte of `reserved_pad` (now a `uint8_t`). It's still 320
bytes and no other offset moved, so nothing on your side changes unless you read
the volume.

## How Windows answered the interface questions

The interface PRs asked the Windows side a list of questions. The Windows code
answers them; this is the record. Only #32's fourth question is still open.

**#32 Reactor** (`iocp_reactor.cpp`, #14)

1. Yes: `IoEvent`'s IOCP half (`overlapped`, `bytes`, `error`) is all
   `named_pipe_ipc.cpp` needs.
2. No threadpool timer, and no packet for it. There's only one timer, so the
   reactor thread checks it when its wait ends and passes the deadline as the
   wait's timeout (libuv does the same). The timer state is only touched on the
   reactor thread, so no timer callback can arrive stale.
3. Yes: cancel and collect the handle's I/O, then `unwatch()`. Handle keys are
   never reused, so a completion that arrives after `unwatch()` is recognised
   and dropped.
4. Still open (phase 4): curl needs socket readiness on Windows, probably
   `WSAEventSelect` or AFD polling inside the IOCP reactor behind the same
   `watch()`. It waits until someone starts `src/net/`.

`Reactor::post()` came out of the pipe work (#12, PR #16): it queues an event
for a handle's callback without touching the port, for operations that finish
at once.

**#37 IpcChannel** (`named_pipe_ipc.cpp`, #12)

1. Yes: one overlapped `ReadFile` is always pending into a whole-frame buffer,
   and its completion starts the next one.
2. Yes, there's no `GetNamedPipeClientProcessId` check. The pipe is
   `\\.\pipe\<endpoint>.<SID>`, its DACL admits only the current user, and the
   client checks that the pipe it opened is owned by its own user before
   talking to it (`permission_denied` otherwise).

**#34 ShmMapping** (`win32_shm.cpp`, #8)

1. Nothing awkward. The only Windows addition is `result_out_of_range` when an
   existing region is smaller than asked for, since a region can't grow.

**#38 AudioSink** (`wasapi_sink.cpp`, #14 and #13)

1. Yes. Shared mode uses `IAudioClient3` at the mix rate when the mix is
   stereo, and reports that rate in `format()`; otherwise `AUTOCONVERTPCM` at
   the requested 48 kHz.
2. `system_category` is enough. An HRESULT that wraps a Win32 error becomes
   that error, and the `AUDCLNT_E_*` ones keep their HRESULT value, which still
   identifies them. The errors the interface names (`not_supported`,
   `device_or_resource_busy`) are mapped explicitly.
3. Settled on #13, as above.

**#30 / #31**

1. Yes to per-word release/acquire in the SeqLock.
2. Yes to CPU and compiler checks in shared code; `crc32c.hpp` uses `__cpuid`
   on MSVC.

**#52 CredentialVault** (`dpapi_vault.cpp`, #10)

1. Yes: target `<service>/<name>`, `CRED_TYPE_GENERIC`, DPAPI blob with the
   target name as entropy. Names can't contain `/` on Windows.
2. A secret over about 2300 bytes doesn't fit after DPAPI's overhead, and the
   header's `value_too_large` covers it.
3. Replaced.

**#50 Daemon skeleton**

1. Yes, and every stub has since been replaced.
2. Yes: `console_shutdown.cpp` (#22) sets a flag and calls `wake()`. CEF takes
   the console handler over when it starts, so `ShutdownSignal::reclaim()`
   (PR #72) takes it back.

**#55 MediaControls** (`smtc_controls.cpp`, #9)

1. SMTC's events reach the reactor thread without a new `Reactor` call or
   packets inside `iocp_reactor.cpp`. The controls bring their own door: a pipe
   whose read end the reactor watches with one `ReadFile` always pending.
   SMTC's handlers queue the request and write one byte, and the completion
   runs the callback on the reactor thread. It only uses `Reactor`'s public
   calls, so the interface didn't change.
2. `HWND_MESSAGE` doesn't work: `GetForWindow` returns `E_INVALIDARG` for it.
   It's a hidden top-level window instead, and since a top-level window gets
   broadcast messages, it lives on a thread of its own running a message loop.
3. The `https://` artwork URL works as-is. Windows fetches the image itself and
   only shows it once it loads.

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
- **clang-tidy on Windows code.** Since #93, CI runs clang-tidy on the Linux
  build only, so nothing in `src/pal/win32/` is checked. Running it on Windows
  would need a clang-cl build (or `compile_commands.json` from the MSVC one) in
  the Windows CI job. Part of CLAUDE.md's open decision on how far to take
  clang-tidy.
- **The phase 5 startup target ([#89](https://github.com/scratterapp/scratter/issues/89)).**
  `am-ui` takes about 210 ms from launch to first frame on Windows, against a
  100 ms target. About 160 ms of it is the GPU driver creating the Direct3D 11
  device, which no SDL renderer does faster. Waiting on whether the gate measures
  first frame to daemon state (about 2 ms) instead.

## What's done, and what's next

Every Windows issue on the roadmap (#6) is closed: the IOCP reactor (#14), the
named pipe (#12), shared memory (#8), Ctrl-C (#22), the WASAPI sink, shared (#14)
and exclusive (#13), the vault (#10), SMTC (#9), CEF with AAC and the CEF host
(#8), `am-ui` (#42), `launch_ui()` (PR #87) and the relaunch gate (#43, PR #88).
The sink following the default device is #94 (PR #95).

Next, roughly in order:

| What | Where | Notes |
| --- | --- | --- |
| Settle the startup target | #89 | A decision, not code. |
| Spectrum visualizer | #40 | Shared code, nobody on it. |
| Socket readiness in the IOCP reactor | #32's fourth question | Groundwork for libcurl in `src/net/`, once phase 4 starts. |
| Apple Music on Windows | #4, #3 | After robbie's MusicKit key: the player page and the login popup have to work in the Windows CEF host too. |

## How to push back

- Comment on the PR, merged or not. That's the quickest.
- For anything bigger, open an issue labelled `shared` and tag `@cocoa55`.
- Interface changes go in their own PR to `src/pal/interfaces/` or `src/common/`,
  before the Windows code that needs them.
