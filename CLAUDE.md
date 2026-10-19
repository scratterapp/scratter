# Working in Scratter

This file is the starting point for anyone picking up this repo, human or AI.
`AGENTS.md` points here so every coding agent reads the same rules.

## What this is

An unofficial, native client for Apple Music on Arch Linux and Windows, written in C++23 and split
into two processes, like `mpd` + `ncmpcpp`:

- **`scratterd`** is the headless daemon. It runs an invisible CEF instance only for
  Widevine L3 decryption, pushes decoded PCM through a lock-free SPSC ring buffer to
  PipeWire or WASAPI, and owns MPRIS/SMTC media controls and the credential vault.
- **`am-ui`** is a Dear ImGui window that can be closed and reopened without stopping
  playback. It talks to the daemon over binary IPC and reads a SeqLock shared-memory
  telemetry page.

The project is called Scratter. The name lives in `AM_APP_NAME` in the root
`CMakeLists.txt` and reaches code as `am::common::kAppName` (`src/common/app_name.hpp`),
so the daemon builds as `scratterd`. Don't hard-code it in new code; build socket, shm,
MPRIS and keyring names from the constants.

Read these before writing code, in this order:

1. [`docs/OVERVIEW.md`](docs/OVERVIEW.md) explains what the system does, in plain terms.
2. [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) shows the process and data-plane boundaries.
3. [`docs/SPECIFICATION.md`](docs/SPECIFICATION.md) is the full technical spec, including
   the bootstrap order (section 5) and the phase roadmap with verification gates (section 6).
4. [`docs/RATIONALE.md`](docs/RATIONALE.md) covers why this exists versus Cider and the official app.
5. [`docs/COLLABORATION.md`](docs/COLLABORATION.md) covers how the two platform owners work in parallel.
6. [`docs/learning/README.md`](docs/learning/README.md) walks through how to do each roadmap
   issue and what it teaches. The pinned roadmap issue on GitHub tracks what's done.

For performance work, [`docs/BENCHMARKS.md`](docs/BENCHMARKS.md) has the measured
baseline, the goals and how to measure; add a line to its log for every change
that moves a number.

## Current state

Phase 1, Linux phase 2 and the daemon skeleton are done. `scratterd` (built as
`<AM_APP_NAME>d`, see `src/daemon/main_daemon.cpp`) runs: it plays a 440 Hz tone
through ring buffer, drift controller and PipeWire, answers Hello, Ping, Play, Pause,
Stop and SetVolume over the IPC socket, publishes telemetry in shared memory, and
quits cleanly on Ctrl-C or SIGTERM. `amctl` (`tools/amctl/`) is a command-line
remote for it: `amctl pause`, `amctl volume 30`, `amctl watch`, `amctl status --json`.
It can send every request the daemon handles, and `tests/test_daemon.cpp` fails if
the daemon learns one amctl lacks (`coverage()` in `tools/amctl/amctl_args.hpp`).
On Linux the daemon also shows up in the desktop's media widget over MPRIS, so the
media keys and `playerctl play-pause` pause it too. Built with CEF (the `linux-cef`
preset, off by default because CEF only exists where someone built it),
`scratterd --test-film` plays Google's Widevine test film through a headless
CEF instead of the tone (`src/daemon/cef_host/`, issue #76). `tests/test_daemon.cpp` starts the daemon
and drives it, directly, through amctl and over MPRIS.

What's real so far: everything in `src/common/` except `arena_allocator.hpp`, every
interface in `src/pal/interfaces/`, the Linux reactor, IPC
socket, shared memory, PipeWire sink, keyring vault, MPRIS media controls and signal handling in
`src/pal/linux/`, and the drift controller and resampler in `src/daemon/audio_engine/`.
Still stubs: CEF on Windows, Apple Music itself (#4), the Windows vault, the Windows media controls (SMTC), `src/net/`, the
ALSA sink and `am-ui` (whose `main()` returns 0). On Windows, `src/pal/win32/` holds stubs that fail with
"not supported" so the daemon links; kero's issues replace them. It configures,
builds and passes its tests on Linux and Windows. Catch2 (via `FetchContent`) and,
on Linux, PipeWire, libsecret and libsystemd's sd-bus (system packages via `pkg-config`) are the only
third-party dependencies wired into CMake so far.

## Who owns what

Two developers work in parallel, one per platform. Stay inside your area unless the
change is a shared contract that both have agreed to.

| Area | Owner | Paths |
| --- | --- | --- |
| Linux platform layer | robbie (`@cocoa55`) | `src/pal/linux/` |
| Windows platform layer | kero (`@kerelosgayed`) | `src/pal/win32/` |
| Shared contracts | robbie and kero both review | `src/common/`, `src/pal/interfaces/`, root `CMakeLists.txt`, `cmake/` |
| Daemon, client and tools | shared | `src/daemon/`, `src/client/`, `src/net/`, `tools/` |

`.github/CODEOWNERS` only auto-requests kero on Windows-only paths (`src/pal/win32/`,
`docs/windows/`); for a shared-contract PR, tag him by hand. Each TODO comment names its owner;
TODOs in shared files name both.

If you are an AI agent, ask which platform you are working on if it isn't clear
from the request, then keep your changes to that platform's directory plus any
shared code the task explicitly needs.

**Working on Windows?** Read [`docs/windows/START_HERE.md`](docs/windows/START_HERE.md)
next. It lists the shared interfaces your code has to fit, the questions waiting
on the Windows side, and the Windows issues in order.

## Rules

- **No platform conditionals outside `src/pal/`.** `src/daemon/`, `src/client/`,
  `src/common/`, `src/net/` and `tools/` must not contain `#ifdef _WIN32`, `#ifdef __linux__`
  or similar. Platform behavior goes behind an interface in `src/pal/interfaces/`.
  CPU-architecture and compiler checks (`__x86_64__`, `_MSC_VER`, `__ARM_FEATURE_*`)
  are fine in shared code, as in `src/common/crc32c.hpp`; OS checks are not.
- **Shared contracts change in their own PR**, reviewed by both robbie and kero, before either
  platform implements against them. Keep platform headers (`<windows.h>`,
  `<sys/socket.h>`, PipeWire, etc.) out of shared headers.
- **Warnings are errors.** `cmake/CompilerWarnings.cmake` builds with
  `-Wall -Wextra -Wpedantic -Wconversion -Werror` (MSVC: `/W4 /WX /permissive-`).
- **No exceptions, no RTTI.** Everything builds with `-fno-exceptions -fno-rtti`
  (MSVC: `/EHs-c- /GR-`). Report errors with return values (`std::expected`, error
  codes), not `throw`, and don't use `dynamic_cast` or `typeid`.
- **Our audio callback code must not allocate, lock, or make syscalls** after stream
  setup (spec section 6, phase 2 gate). The rule is about the code we write that
  runs in the callback. PipeWire's own thread still makes syscalls every cycle to
  sleep and wake, and that's outside our control.
- **Keep it a player.** No recording, saving or exporting decrypted audio, no DRM
  bypass, and never ship the Widevine CDM. The spec's Non-goals section has the list.
- **Never commit** credentials, tokens, CEF binaries, or Widevine blobs. `.gitignore`
  already excludes `third_party/cef/*`, `*widevine*`, `*.token`, `*.key` and `.env*`.
- Follow `.clang-format` and `.editorconfig`.
- **No AI attribution anywhere in the repo's history or on GitHub.** Don't add
  `Co-Authored-By` trailers for Claude or any other AI, AI session links, or
  "Generated with/by Claude Code" lines to commits, PR descriptions, PR or issue
  comments, or reviews, and don't commit under an AI identity. Commits are authored
  by the human you are working for.

## Build and test

Install dependencies first: see [`docs/SETUP.md`](docs/SETUP.md).

Linux:

```sh
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug
```

Sanitizer builds use the same three commands with a different preset:

- `linux-asan` runs AddressSanitizer plus UndefinedBehaviorSanitizer (memory errors,
  leaks, signed overflow and other undefined behavior).
- `linux-tsan` runs ThreadSanitizer (data races between threads).
- `linux-fuzz` is `linux-asan` built with Clang plus the libFuzzer targets
  (`tests/fuzz_*.cpp`); its ctest run includes a short smoke run of each fuzzer.
- `linux-cef` is `linux-debug` with `AM_WITH_CEF=ON`: the daemon links our CEF
  build from `third_party/cef/linux64` (see
  [`docs/learning/cef-host.md`](docs/learning/cef-host.md)). Without it, CEF code
  isn't compiled and `--test-film` says so.

```sh
cmake --preset linux-tsan
cmake --build --preset linux-tsan
ctest --preset linux-tsan
```

Windows, from a Developer PowerShell for VS 2022:

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug
```

CI (`.github/workflows/ci.yml`) path-filters: changes only under `src/pal/linux/`
skip the Windows job, changes only under `src/pal/win32/` skip the Linux job, and
everything else runs both. Linux CI runs on Ubuntu, not Arch, and builds and tests
`linux-debug`, `linux-asan` and `linux-tsan`, plus `linux-cef` against Google's
prebuilt CEF of the same version (it can't decode AAC, but it compiles and links the
same). A separate `ipc-fuzz` job runs the IPC
frame fuzzer for 10 million inputs, the last part of the spec's Phase 1 gate.

Tests use [Catch2 v3](https://github.com/catchorg/Catch2), which CMake downloads with
`FetchContent` on first configure (so that first configure needs internet). Each
`tests/test_*.cpp` file is its own executable, and every `TEST_CASE` in it shows up
as a separate ctest test. Because the project builds without exceptions, a failing
`REQUIRE` aborts its test case instead of throwing; use `CHECK` to keep going after
a failure. `tests/test_sanity.cpp` is the example to copy. When a file gets its
first `TEST_CASE`, add its name to `AM_TESTS` in `tests/CMakeLists.txt`.
Sanitizer flags apply to every target, Catch2 included.

## Where to start

Follow the spec's bootstrap order. Steps 1 and 2 are shared contracts and block
both platforms, so they come first.

1. `src/common/spsc_ring_buffer.hpp` and `src/common/telemetry_shm.hpp` (both done), with
   multi-threaded stress tests under `linux-tsan`. *(shared)*
2. `src/common/ipc_protocol.hpp` (32-byte `FrameHeader`, done; CRC32C in `crc32c.hpp`, done) and
   `src/pal/interfaces/ipc_channel.hpp`. *(shared)* Then the transports:
   `src/pal/linux/unix_seqpacket_ipc.cpp` *(Linux)* and
   `src/pal/win32/named_pipe_ipc.cpp` *(Windows)*, with a ping-pong latency test.
3. Audio sinks playing a synthetic 440 Hz sine from the ring buffer:
   `src/pal/linux/pipewire_sink.cpp` *(Linux)* and `src/pal/win32/wasapi_sink.cpp`
   *(Windows)*.
4. Headless CEF and `CefAudioHandler` in `src/daemon/`. *(shared)*
5. `src/net/` (libcurl + simdjson) and the ImGui client in `src/client/`. *(shared)*

Shared memory (`posix_shm.cpp` / `win32_shm.cpp`), the credential vault
(`secret_vault.cpp` / `dpapi_vault.cpp`) and media controls (`mpris_controls.cpp` /
`smtc_controls.cpp`) are per-platform work that can proceed once their interface in
`src/pal/interfaces/` is agreed.

## Spec vs repo layout

`docs/SPECIFICATION.md` section 5 now matches this tree, including files that are
still empty stubs. If you add, move or rename a file, update that section in the
same PR. If the two ever disagree, follow the repo.

## Open decisions

These aren't settled. Raise them with robbie and kero rather than picking one silently.

- How third-party dependencies are pulled in (system packages on Arch, vcpkg on
  Windows, CMake `FetchContent`, or a mix). Catch2 uses `FetchContent` so it builds
  with the project's sanitizer and no-exceptions flags; that doesn't decide it for
  the other libraries.
- GLFW or SDL3 for the Linux window.
- How we build and ship our own CEF. The prebuilt downloads can't decode AAC, so we
  build CEF with AAC turned on (spec 3.1.1). Still open: where the build runs for
  releases (it takes hours, and it needs redoing for CEF security updates) and how
  the AAC patent question is handled before a public release.
- Where the daemon's Apple Music developer token comes from: a token signed with
  your own Apple Developer account (ties users' activity to it, and the key must stay
  private), or the token Apple's web player uses (a clearer terms problem).
- Where the `am-ui` graphics backend (Vulkan/OpenGL vs DirectX 11) lives. It is
  platform code but client-only, so it can't go in `am_pal`, which the daemon links.
