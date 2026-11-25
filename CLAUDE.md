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
5. [`docs/COLLABORATION.md`](docs/COLLABORATION.md) covers how the two platform owners work in
   parallel, and the steps for working an issue. The pinned roadmap issue on GitHub tracks
   what's done and what's next.

For anything touching CEF, [`docs/CEF.md`](docs/CEF.md) explains how the hidden browser
runs inside the daemon and how to build our CEF with AAC.

For performance work, [`docs/BENCHMARKS.md`](docs/BENCHMARKS.md) has the measured
baseline, the goals and how to measure; add a line to its log for every change
that moves a number.

## Quick reference

| What | Here |
| --- | --- |
| Language | C++23 (`CMAKE_CXX_STANDARD 23`). The code already uses `std::expected`, concepts and `std::span`; don't rewrite it down to C++20. Exceptions and RTTI are off. |
| Compilers | GCC or Clang on Linux (CI uses `clang-20`), MSVC 2022 on Windows |
| Build | `cmake --preset linux-debug` then `cmake --build --preset linux-debug` (Windows: `windows-msvc-debug`) |
| Test | `ctest --preset linux-debug` (Windows: `ctest --preset windows-msvc-debug`) |
| Sanitizers | `linux-asan` (AddressSanitizer + UBSan), `linux-tsan` (ThreadSanitizer), `linux-fuzz` (libFuzzer) |
| Format | `clang-format -i <files you changed>` (`.clang-format`; CI doesn't check it) |
| Lint | `run-clang-tidy -p build/linux-debug -quiet "$PWD/(src\|tests\|tools\|bench)/"` (`.clang-tidy`; CI fails on any finding) |
| Coverage | `linux-coverage` preset, read with `gcovr` (see Build and test) |
| Task list | GitHub issues; the pinned roadmap is issue #6. There is no `TODO.md`. |

## Current state

Phase 1, Linux phase 2 and the daemon skeleton are done. `scratterd` (built as
`<AM_APP_NAME>d`, see `src/daemon/main_daemon.cpp`) runs: it plays a 440 Hz tone
through ring buffer, drift controller and PipeWire, answers Hello, Ping, Play, Pause,
Stop, SetVolume, LoadTrack and Seek over the IPC socket, publishes telemetry in shared memory, and
quits cleanly on Ctrl-C or SIGTERM. `amctl` (`tools/amctl/`) is a command-line
remote for it: `amctl pause`, `amctl volume 30`, `amctl load clear-film`, `amctl seek 1:23`, `amctl watch`,
`amctl status --json`.
It can send every request the daemon handles, and `tests/test_daemon.cpp` fails if
the daemon learns one amctl lacks (`coverage()` in `tools/amctl/amctl_args.hpp`).
On Linux the daemon also shows up in the desktop's media widget over MPRIS, so the
media keys and `playerctl play-pause` pause it too, and clicking the player there
brings the window forward or starts it (#41). `cmake --install` puts in a launcher
entry and icon (`packaging/linux/`), or on Windows a Start Menu shortcut (`packaging/windows/`, #97). Built with CEF (the `linux-cef`
preset, off by default because CEF only exists where someone built it),
`scratterd --test-film` plays Google's Widevine test film through a headless
CEF instead of the tone (`src/daemon/cef_host/`, issue #76), and `amctl load` switches
between the tone and the two films while it runs, starting CEF the first time (#38). The films
can be seeked in from amctl, the window's position bar and MPRIS (#39); the tone has no
length, except with `--tone-length`, which the tests use. `tests/test_daemon.cpp` starts the daemon
and drives it, directly, through amctl and over MPRIS. `tools/relaunch/relaunch.py` is the
phase 5 gate: it opens and closes `am-ui` 50 times while the daemon plays and fails
if the sound skipped (#43), or if the window's own start-up or its connect to the
daemon went over budget (#89).

What's real so far: everything in `src/common/`, every
interface in `src/pal/interfaces/`, the Linux reactor, IPC
socket, shared memory, PipeWire sink, keyring vault, MPRIS media controls and signal handling in
`src/pal/linux/`, their Windows counterparts in `src/pal/win32/` (IOCP reactor, named
pipe, file mapping, WASAPI sink (shared, and exclusive with `scratterd --exclusive`), DPAPI vault, SMTC media controls, console Ctrl-C) plus the Windows CEF host
(a CEF built with `third_party/cef-patches/widevine-cdm-dir.patch`, issue #8), and the
drift controller and resampler in `src/daemon/audio_engine/`.
Still stubs: Apple Music itself (#4), the ALSA sink and most of `am-ui`. It opens its window (SDL3 + Dear ImGui, drawn through
`SDL_Renderer`, so no platform code; issue #35), and its connection to the daemon is
real: `am_client_core` (`src/client/daemon_link.hpp`, `ui_state.hpp`) connects on its own
thread, sends commands, follows the state, reconnects, and reads the telemetry page. The
window shows it: a now-playing view with play/pause, stop and volume (#37), a spectrum strip the daemon fills from what it plays (#40), and a library view of playlists that shows made-up sample data until the MusicKit key exists (`src/client/sample_library.hpp`). It configures,
builds and passes its tests on Linux and Windows. `src/net/http_client.hpp` is the HTTP
client for the Apple Music API (#104): libcurl on a thread of its own, tested against a
local server. `src/net/catalog_parser.hpp` (#105) turns Apple Music catalog
responses (songs, albums, playlists, search) into plain structs in the arena
(`src/common/arena_allocator.hpp`) with simdjson; it's tested and fuzzed against
hand-written samples in `tests/data/catalog/`, since there's no Apple key yet, and
nothing calls it until the library views. Catch2, SDL3, Dear ImGui and simdjson (via `FetchContent`, pinned by commit), libcurl
(`pkg-config` on Linux, `FetchContent` pinned to a release with Schannel on Windows) and,
on Linux, PipeWire, libsecret and libsystemd's sd-bus (system packages via `pkg-config`), plus FreeType
for am-ui's text when `pkg-config` finds it, are the only
third-party dependencies wired into CMake so far. SDL3 needs the X11 and Wayland
development files to build its Linux video drivers (`docs/SETUP.md`).

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
  `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror` (MSVC: `/W4 /WX /permissive-`).
  Clang's `-Wconversion` also turns on `-Wsign-conversion` and GCC's doesn't, so
  code that builds clean with GCC can still fail in CI; build with Clang too (see
  Build and test). Keep every check on, clang-tidy's included. If a warning,
  sanitizer or clang-tidy finding blocks you and the fix isn't clear, stop and say
  so instead of silencing it. A `// NOLINT(check-name)` is only for code that is
  right as written, with a comment that says why.
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
- Follow `.clang-format` and `.editorconfig`. (`.clang-format` says
  `Standard: c++20` because clang-format has no C++23 setting; the code is still
  C++23.)
- **No AI attribution anywhere in the repo's history or on GitHub.** Don't add
  `Co-Authored-By` trailers for Claude or any other AI, AI session links, or
  "Generated with/by Claude Code" lines to commits, PR descriptions, PR or issue
  comments, or reviews, and don't commit under an AI identity. Commits are authored
  by the human you are working for.

## Code standards

The goal is code a newcomer, human or AI, can read and change. Plain and direct
beats clever; don't add an abstraction until a second user needs it.

- Use RAII for every resource (file descriptors, handles, mappings, CEF objects).
  `std::unique_ptr` owns; references and `std::span` borrow.
- Report errors with `std::expected` or an error code, and mark functions that
  return one `[[nodiscard]]`. Mark other functions `[[nodiscard]]` and `constexpr`
  where they qualify; `src/common/` is full of examples.
- `const` by default. Initialize every variable where it's declared.
- Use `enum class`, `std::array`, `std::string_view`, `std::optional` and
  `std::span` where they fit. Prefer standard algorithms and ranges over a raw loop
  when they read better; a plain loop is fine in the audio path and in other places
  where it's clearer.
- Every new feature gets tests. `constexpr` code also gets `static_assert` checks.
- Match the style of the code and tests around you: names, layout and how the
  test file is organized.
- Delete dead code outright instead of commenting it out. Git keeps the history.

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
- `linux-coverage` builds with `--coverage` so you can see which lines the tests
  run (details below).
- `linux-fuzz` is `linux-asan` built with Clang plus the libFuzzer targets
  (`tests/fuzz_*.cpp`); its ctest run includes a short smoke run of each fuzzer.
- `linux-cef` is `linux-debug` with `AM_WITH_CEF=ON`: the daemon links our CEF
  build from `third_party/cef/linux64` (see
  [`docs/CEF.md`](docs/CEF.md)). Without it, CEF code
  isn't compiled and `--test-film` says so.
- `linux-cef-release` is `linux-cef` with our code optimized (a Release build).
  Use it for listening and for measuring performance (`docs/BENCHMARKS.md`): the
  debug build's audio code alone takes about 5% of a CPU core, the optimized one
  0.3%. Keep `linux-cef` for debugging.

```sh
cmake --preset linux-tsan
cmake --build --preset linux-tsan
ctest --preset linux-tsan
```

`linux-debug` uses the system compiler (GCC on most Linux setups). To build the same
thing with Clang, as CI does, give it its own build folder:

```sh
cmake --preset linux-debug -B build/linux-clang -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build/linux-clang
ctest --test-dir build/linux-clang --output-on-failure
```

clang-tidy reads the build's `compile_commands.json`, so build first. The command
checks only our code. It takes a couple of minutes, so while you work, run plain
`clang-tidy -p build/linux-debug <file>` on the files you changed:

```sh
run-clang-tidy -p build/linux-debug -quiet "$PWD/(src|tests|tools|bench)/"
```

`.clang-tidy` turns on the bugprone, performance and concurrency checks, plus a short
list of others that match the code standards below. It turns off a few, each with a
reason, and `tests/.clang-tidy` relaxes two more for test code. Windows-only and
CEF files aren't in the Linux build, so clang-tidy doesn't check them yet.

Coverage (needs `gcovr`: `pacman -S gcovr` on Arch, `apt install gcovr` on Ubuntu):

```sh
cmake --preset linux-coverage
cmake --build --preset linux-coverage
ctest --preset linux-coverage
gcovr --root . --filter src/ --filter tools/ --gcov-exclude-directories '.*_deps.*' --print-summary --html-details build/linux-coverage/coverage.html build/linux-coverage
```

On 2026-11-22 the tests ran 86% of the lines and 69% of the branches in the code
the Linux build compiles, and 93% of the lines in `src/pal/linux/`. Code that
only runs with CEF, or only on Windows, isn't in that number. The Linux numbers
need a session bus, an unlocked keyring and a PipeWire sink: without them, the
tests for those skip and the number drops.

Windows, from a Developer PowerShell for VS 2022:

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug
```

CI (`.github/workflows/ci.yml`) path-filters: changes only under `src/pal/linux/`
skip the Windows job, changes only under `src/pal/win32/` skip the Linux job, and
everything else runs both. Linux CI runs on Ubuntu, not Arch, and builds and tests
`linux-debug`, `linux-asan` and `linux-tsan`, runs clang-tidy, and builds `linux-cef` and `linux-cef-release`
against Google's prebuilt CEF of the same version (it can't decode AAC, but it
compiles and links the same). A separate `ipc-fuzz` job runs the IPC
frame fuzzer for 10 million inputs, the last part of the spec's Phase 1 gate.

Tests use [Catch2 v3](https://github.com/catchorg/Catch2), which CMake downloads with
`FetchContent` on first configure (so that first configure needs internet). Each
`tests/test_*.cpp` file is its own executable, and every `TEST_CASE` in it shows up
as a separate ctest test. Because the project builds without exceptions, a failing
`REQUIRE` aborts its test case instead of throwing; use `CHECK` to keep going after
a failure. `tests/test_sanity.cpp` is the example to copy. When a file gets its
first `TEST_CASE`, add its name to `AM_TESTS` in `tests/CMakeLists.txt`.
Sanitizer flags apply to every target, Catch2 included.

## Working a task

`docs/COLLABORATION.md` has the short version for both developers. This is the full
checklist, written so an AI agent can follow it as is.

1. Take one issue at a time from the roadmap (#6). Before building it, check the
   open PRs and recent branches: the other developer may already be on it.
2. Ask about anything ambiguous before starting, including which platform you're
   on if the request doesn't say.
3. Branch from `main` with a plain name (`feature/…`, `fix/…`, `docs/…`).
4. Write or update the tests first, and start from a failing test where you can.
   Before changing code that has thin tests, add tests that pin down what it does
   today. If today's behavior looks wrong, keep the test matching it, open an issue
   that describes intended versus actual behavior, and let robbie and kero decide.
5. Make the smallest change that finishes the issue. Put unrelated improvements
   you notice in a new issue rather than in this PR.
6. If an approach fails twice, stop and write up the problem and the alternatives
   instead of trying a third variation.
7. Before calling it done, check that all of these ran clean, and say so in the PR:
   - the build, with GCC and with Clang on Linux (MSVC on Windows);
   - the full test suite, plus `linux-asan` for anything that parses bytes or does
     pointer math and `linux-tsan` for anything with threads;
   - clang-tidy (CI runs it too, but it's faster to catch findings locally);
   - `clang-format` on the files you changed;
   - coverage of the code you touched is the same or higher (`linux-coverage`).

   Windows-only changes can't be built in a Linux container; say so and let the
   Windows CI job check them.
8. Open the PR with `Closes #N` so the issue closes on merge, and list any new
   issues you found or opened.

## Commits

- Small commits, each a short imperative description of the change ("Add seek to
  amctl").
- A commit message holds only the description of the change. No `Co-Authored-By`
  for an AI, no "Generated with" line, no session link (see the last item under Rules).

## Where to start

The pinned roadmap issue (#6) on GitHub is the list of what's next; pick an issue
from it as `docs/COLLABORATION.md` describes. For orientation, the spec's bootstrap
order and where each step stands:

1. Ring buffer and telemetry page in `src/common/`. *(shared, done)*
2. IPC frame, CRC32C and `ipc_channel.hpp`, then the Unix socket and named pipe
   transports. *(shared, then per platform, done)*
3. Audio sinks playing a 440 Hz tone from the ring buffer: PipeWire and WASAPI.
   *(per platform, done; the ALSA fallback is parked)*
4. Headless CEF and `CefAudioHandler` in `src/daemon/cef_host/`. *(shared, plays
   Google's test films; Apple Music through MusicKit JS is #4)*
5. `src/net/` (libcurl + simdjson) and the ImGui client in `src/client/`. *(shared;
   the window, its daemon link, the HTTP client and the catalog parser are in)*

Shared memory, the credential vault and media controls are done on both platforms.

## Spec vs repo layout

`docs/SPECIFICATION.md` section 5 now matches this tree, including files that are
still empty stubs. If you add, move or rename a file, update that section in the
same PR. If the two ever disagree, follow the repo.

## Open decisions

These aren't settled. Raise them with robbie and kero rather than picking one silently.

- How later third-party dependencies are pulled in. Settled so far (roadmap, #6):
  system libraries through `pkg-config`, small source libraries through
  `FetchContent` pinned by commit (Catch2, SDL3, ImGui, simdjson),
  and libcurl from `pkg-config` on Linux and `FetchContent` with Schannel on
  Windows. Anything new still gets agreed first.
- How we build and ship our own CEF. The prebuilt downloads can't decode AAC, so we
  build CEF with AAC turned on (spec 3.1.1). Still open: where the build runs for
  releases (it takes hours, and it needs redoing for CEF security updates) and how
  the AAC patent question is handled before a public release.
- How far to take clang-tidy and coverage. clang-tidy starts with a short list of
  checks, and Windows and CEF code isn't checked yet. Coverage is measured but has no
  minimum, and code that only runs on Windows or with CEF isn't measured.
- Where the daemon's Apple Music developer token comes from: a token signed with
  your own Apple Developer account (ties users' activity to it, and the key must stay
  private), or the token Apple's web player uses (a clearer terms problem).
