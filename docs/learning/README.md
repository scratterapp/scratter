# Learning guide

This goes with the roadmap issue on GitHub. The roadmap says **what** is done and
what is next. This guide says **how** to do each piece and **what you learn** by
doing it, with pointers to read before you start.

It assumes you know some C++ but haven't done much systems, lock-free or audio
work. Each section is short on purpose: read it, read the spec section it points
at, then open the issue.

> **About issue numbers:** the project moved to a fresh repository on 2026-10-17. Open
> issues came along with new numbers (#1 to #14), and this guide uses those. Finished
> issues and PRs (like #4, #53 or #81) keep their old numbers from the first tracker,
> which is archived and private, so they won't open on GitHub.

- [How to work an issue](#how-to-work-an-issue)
- [Phase 1: primitives, shared memory and IPC](#phase-1-primitives-shared-memory-and-ipc)
- [Phase 2: real-time audio](#phase-2-real-time-audio)
- [Phases 3 to 5: CEF, metadata, media keys and the UI](#phases-3-to-5-cef-metadata-media-keys-and-the-ui)
- [Concepts index](#concepts-index)

Deeper write-ups for finished pieces:

- [ring-buffer.md](ring-buffer.md): the SPSC audio ring buffer (#4), line by line.
- [seqlock.md](seqlock.md): the SeqLock telemetry page (#5) and the data-race trap it avoids.
- [shm-mapping.md](shm-mapping.md): the `ShmMapping` interface (#6), RAII and `std::expected`.
- [posix-shm.md](posix-shm.md): POSIX shared memory on Linux (#7) and the two-process test.
- [reactor.md](reactor.md): the event reactor interface (#12) and the epoll vs
  IOCP decision.
- [crc32c.md](crc32c.md): the CRC32C checksum that guards IPC headers (#9).
- [ipc-frame.md](ipc-frame.md): the 32-byte IPC frame header and its fuzzer (#10).
- [ipc-messages.md](ipc-messages.md): what goes inside a frame (Hello, Play,
  SetVolume, CommandAck...) and how wire formats get versioned (#48).
- [ipc-channel.md](ipc-channel.md): the IPC channel interface (#11), messages vs
  streams.
- [ipc-socket-checks.md](ipc-socket-checks.md): who may talk to the daemon,
  checking the runtime folder's owner and mode, and asking the kernel who is on
  the other end of a socket with `SO_PEERCRED`.
- [audio-sink.md](audio-sink.md): the audio sink and memory pinning
  interfaces (#18), and the PipeWire sink (#19).
- [drift-controller.md](drift-controller.md): the clock-drift controller and resampler (#21).
- [daemon.md](daemon.md): the runnable daemon (#50), how the pieces join up
  around one event loop, and catching Ctrl-C with `signalfd`.
- [amctl.md](amctl.md): `amctl`, the command-line remote (#51), a client's side
  of the IPC conversation and parsing a command line without exceptions.
- [amctl-parity.md](amctl-parity.md): amctl doing everything am-ui will (`ping`,
  `toggle`, `watch`, `--json`), and a test that keeps it that way.
- [credential-vault.md](credential-vault.md): the login storage interface (#52),
  and using `std::expected<std::optional<T>>` for "worked, worked but empty, failed".
- [keyring-vault.md](keyring-vault.md): the Linux vault (#53), D-Bus and how
  desktop keyrings work.
- [media-controls.md](media-controls.md): the media keys interface (#55), and
  designing an interface where events come in as well as go out.
- [cef-host.md](cef-host.md): CEF inside the daemon (#76), Chromium's processes,
  the zygote and the "stack smashing" crash, the sandbox, and a second producer
  for the ring buffer.
- [fade.md](fade.md): fading in and out on play and pause (#1), and why a
  sudden jump in the samples sounds like a click.
- [gapless.md](gapless.md): whether a song change can leave a gap in the
  audio path, and why waiting for the ring buffer to refill doesn't make one.
- [soak-test.md](soak-test.md): the 4-hour soak test (#2), reading a process's
  memory from `/proc`, and telling a leak from a cache filling up.
- [performance.md](performance.md): how heavy the daemon is, RSS versus PSS,
  why each Chromium process costs memory, wake-ups, and debug versus optimized
  builds; `tools/perf/footprint.py`.
- [license-request.md](license-request.md): how a Widevine license is asked
  for, how we found Google had revoked our Widevine (#21), and reading a
  protobuf by hand.
- [hardening.md](hardening.md): the compiler and linker flags that turn many
  memory bugs into a crash instead of a takeover, and how to check a binary has
  them.

## How to work an issue

1. Pick an issue from the roadmap whose "blocked by" items are all closed. Shared
   items (`src/common/`, `src/pal/interfaces/`) come before the platform items that
   build on them.
2. Branch from `main` with a plain name: `feature/crc32c`, `fix/ipc-frame`.
3. Write the test first when you can. Every `tests/test_*.cpp` is its own
   executable; add its name to `AM_TESTS` in `tests/CMakeLists.txt` once it has a
   `TEST_CASE`.
4. Build and test with `linux-debug`, then run the sanitizer that matters for the
   change: `linux-tsan` for anything with threads, `linux-asan` for anything that
   parses bytes or does pointer math.
5. Open a PR. CODEOWNERS asks kero to review only when it touches Windows-only
   files; for a shared-contract change, tag both robbie and kero yourself.

House rules that shape every answer below (full list in [CLAUDE.md](../../CLAUDE.md)):
no exceptions and no RTTI, so errors come back as `std::expected` or error codes;
warnings are errors; no `#ifdef _WIN32` outside `src/pal/`; our audio callback code
never allocates, locks or makes a syscall once the stream is running (the PipeWire
or WASAPI thread that calls it still makes its own to sleep and wake; those aren't
ours).

---

## Phase 1: primitives, shared memory and IPC

Goal: the two processes can talk (IPC) and the daemon can publish live status to
the UI (shared memory), all proven tear-free and race-free under sanitizers.
Spec: section 3.1.3, 3.2 and the Phase 1 gate in section 6.

### #3 Test framework and sanitizer presets (done)

- **What was done:** Catch2 v3 via `FetchContent`, built without exceptions, plus
  the `linux-asan` and `linux-tsan` presets.
- **You learn:** what sanitizers are. They are compiler switches that add checks
  around memory accesses and fail the moment something illegal happens.
  ThreadSanitizer finds data races, AddressSanitizer finds out-of-bounds and
  use-after-free, UBSan finds undefined behavior like signed overflow.
- **Read:** the "What the sanitizer builds check" table in
  [ring-buffer.md](ring-buffer.md).

### #4 Lock-free SPSC ring buffer (done)

- **What was done:** `src/common/spsc_ring_buffer.hpp`, plus a 100-million-frame
  two-thread stress test.
- **You learn:** `std::atomic` with acquire/release ordering, why a single
  producer and single consumer need no lock, false sharing and cache lines,
  power-of-two masking, `std::span`, and C++20 `requires` clauses on templates.
- **Read:** [ring-buffer.md](ring-buffer.md).

### #5 SeqLock and TelemetrySnapshot (done)

- **What was done:** `src/common/telemetry_shm.hpp`, plus a two-reader stress
  test. Write-up in [seqlock.md](seqlock.md).
- **How:** a struct of playback status (position, buffer fill, 64 FFT bands)
  guarded by one atomic counter. The writer bumps it to odd, writes, bumps it to
  even. The reader reads the counter, copies the struct, reads the counter again,
  and retries if it changed or was odd. Spec 3.2.2 has the layout; it must be
  exactly 320 bytes.
- **Test:** one writer thread writing snapshots whose fields all hold the same
  number, one reader checking it never sees a mix. Run it under `linux-tsan`.
- **You learn:** the difference between lock-free (someone always makes progress)
  and wait-free (the writer never waits at all). Also a real C++ trap: copying a
  plain struct while another thread writes it is a data race even if you throw the
  result away, and TSan will say so. The fix is to copy field by field through
  `std::atomic_ref`, or to store the payload as atomic words (what we did).
  `static_assert` on `sizeof`, `std::is_trivially_copyable_v` and
  `is_always_lock_free` pins the layout down at compile time.
- **Read:** Hans Boehm, "Can Seqlocks Get Along With Programming Language Memory
  Models?" (2012). It is short and explains exactly this trap.

### #6 ShmMapping interface (shared)

- **How:** a header in `src/pal/interfaces/shm_mapping.hpp` describing "create a
  named 4096-byte region read-write" (daemon) and "open it read-only" (client).
  Move-only RAII type that unmaps in its destructor. A factory returns
  `std::expected<ShmMapping, Error>` because constructors can't report failure
  without exceptions.
- **You learn:** interface design across two OSes. Spec section 4 wants static
  polymorphism (one implementation picked at build time) rather than virtual
  functions; here that is one class whose member functions each platform's
  `.cpp` defines. RAII and move-only types (`= delete` the copy constructor,
  write a move constructor that leaves the source empty).
- **Done:** [shm-mapping.md](shm-mapping.md) walks through the header.

### #7 Linux: POSIX shared memory (robbie)

- **How:** `shm_open("/scratterd_telemetry_v2", O_CREAT | O_RDWR | O_CLOEXEC, 0600)`,
  `ftruncate` to 4096, `mmap(PROT_READ | PROT_WRITE, MAP_SHARED)`. The client
  opens with `O_RDONLY` and maps `PROT_READ`. Decide who calls `shm_unlink` and when.
- **Test:** fork, or spawn a second process, and run the SeqLock test across
  processes instead of threads.
- **You learn:** file descriptors, `/dev/shm`, why `O_CLOEXEC` matters, and how
  two processes can see the same physical RAM through different virtual addresses.
- **Read:** `man 7 shm_overview`, `man 2 mmap`.
- **Done:** [posix-shm.md](posix-shm.md) walks through `posix_shm.cpp` and the test.

### #8 Windows: file-mapping shared memory (kero)

- **How:** `CreateFileMappingW(INVALID_HANDLE_VALUE, ..., PAGE_READWRITE, 0, 4096, name)`
  then `MapViewOfFile`. The client uses `OpenFileMappingW` and `FILE_MAP_READ`.
- **You learn:** Windows named kernel objects, handle lifetime (the mapping lives
  until the last handle closes, there is no unlink), and session namespaces
  (`Local\` vs `Global\`).

### #9 CRC32C (done)

- **What was done:** `src/common/crc32c.hpp`: a 256-entry lookup table built at
  compile time from the reflected polynomial `0x82F63B78`, plus a hardware path
  (`_mm_crc32_u64` on x86-64 with a runtime SSE4.2 check, `__crc32cd` on ARM64
  builds that enable CRC). `crc32c()` picks the hardware one when it can.
- **Test:** the check value (`123456789` gives `0xE3069283`) and the RFC 3720
  appendix B.4 vectors through every code path, plus hardware vs table on every
  length and alignment.
- **You learn:** what a checksum catches and what it doesn't, bit-reflection,
  compiler intrinsics, and runtime CPU feature detection. It is also a contained
  first PR in a shared file.
- **Read:** [crc32c.md](crc32c.md).

### #10 FrameHeader and opcodes (shared, in review)

- **What was done:** `src/common/ipc_protocol.hpp`: the struct, opcodes and
  `static_assert`s from spec 3.2.1, plus `encode_header()` (fills in magic,
  version and the CRC) and `validate_frame()` (checks length, magic, CRC, version,
  opcode and payload size before any field is trusted). Bytes and struct are
  converted with `std::bit_cast`, never by casting a `char*` to `FrameHeader*`.
- **Test:** `tests/test_ipc_framing.cpp` pins the exact wire bytes, round-trips
  every opcode, rejects every other opcode byte and each kind of bad header, and
  flips every bit of a good header. `tests/fuzz_ipc_framing.cpp` is the libFuzzer
  target (`linux-fuzz` preset).
- **You learn:** alignment and padding (`has_unique_object_representations_v`
  proves there are no hidden padding bytes), endianness, strict aliasing, and
  fuzzing: letting the computer search for inputs that crash your parser.
- **Read:** [ipc-frame.md](ipc-frame.md).

### #11 IpcChannel interface (shared, in review)

- **How:** send one whole frame, receive one whole frame, never block. Server role
  (daemon) and client role. Return "would block" as a normal result, not an error.
- **Where it stands:** a proposed `ipc_channel.hpp` is up for review:
  `IpcListener` (daemon) hands out an `IpcChannel` per UI, `send()`/`receive()`
  return `Done`, `WouldBlock` or `PeerClosed`, and the reactor calls back when
  it's worth trying again.
- **You learn:** message vs stream sockets. TCP and plain pipes are byte streams,
  so you have to find frame boundaries yourself. `SOCK_SEQPACKET` and message-mode
  pipes keep each send as one unit, which is why the spec picked them.
- **Read:** [ipc-channel.md](ipc-channel.md).

### #12 Reactor interface (shared)

- **How:** one thread waits on many sources at once: "this socket is readable",
  "another thread woke me", "this timer fired". The interface registers sources
  and dispatches callbacks.
- **You learn:** the core design tension of the whole PAL. Linux epoll is
  **readiness**-based ("you can read now, go do it"). Windows IOCP is
  **completion**-based ("I already did the read you asked for, here's the
  result"). An interface that fits both has to be designed around that difference.
  Talk it through before writing code.
- **Where it stands:** a proposed `reactor.hpp` is up for review. The reactor
  does no I/O itself; it reports what the OS said (readiness on Linux,
  completions on Windows) and the platform IPC channel handles it.
- **Read:** [reactor.md](reactor.md), then the libuv design overview.

### #13 Linux: epoll reactor (robbie)

- **How:** `epoll_create1(EPOLL_CLOEXEC)`, an `eventfd` for cross-thread wakeups,
  a `timerfd_create(CLOCK_MONOTONIC)` for the timer. One `epoll_wait` loop.
- **You learn:** level-triggered vs edge-triggered (`EPOLLET`), and why
  edge-triggered means you must drain until `EAGAIN`.
- **Read:** `man 7 epoll`, `man 2 eventfd`, `man 2 timerfd_create`.

### #14 Windows: IOCP reactor (kero)

- **How:** `CreateIoCompletionPort`, `GetQueuedCompletionStatusEx` in the loop,
  `PostQueuedCompletionStatus` for wakeups, and the timer's deadline folded into
  the wait timeout instead of a threadpool timer.
- **You learn:** overlapped I/O and why the `OVERLAPPED` struct must outlive the
  operation.

### #15 Linux: SOCK_SEQPACKET channel (robbie, in review)

- **How:** `socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)`,
  bind to `$XDG_RUNTIME_DIR/scratterd.sock` with mode `0600`, register the fd
  with the epoll reactor. Remove a stale socket file left by a crashed daemon.
- **Where it stands:** `src/pal/linux/unix_seqpacket_ipc.cpp` is written, with
  tests in `tests/test_unix_seqpacket_ipc.cpp`. The walkthrough is the "Linux
  version, line by line" part of [ipc-channel.md](ipc-channel.md).
- **Test:** a ping-pong benchmark (`bench/ipc_ping_pong.cpp`): send `Ping`, wait
  for `Pong`, many times. Time it with `std::chrono::steady_clock` and report the
  median and 99th percentile, not just the average. Target is under 25 µs per
  round trip.
- **You learn:** Unix domain sockets, `sun_path`'s 108-byte limit, checking the
  peer with `SO_PEERCRED` ([ipc-socket-checks.md](ipc-socket-checks.md)), and
  how to benchmark honestly.
- **Read:** `man 7 unix`.

### #12 Windows: message-mode named pipe (kero)

- **How:** `CreateNamedPipeW(L"\\\\.\\pipe\\scratterd.<SID>", ...)` with
  `PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE`, `FILE_FLAG_OVERLAPPED` and
  `FILE_FLAG_FIRST_PIPE_INSTANCE`, wired into the IOCP reactor. `<SID>` is the
  current user's security identifier. Use `Reactor::post()` for the completions
  Windows doesn't queue, like `ConnectNamedPipe` returning `ERROR_PIPE_CONNECTED`.
- **Where it stands:** `src/pal/win32/named_pipe_ipc.cpp` is written, with
  tests in `tests/test_named_pipe_ipc.cpp`; `amctl` drives the daemon on
  Windows. The walkthrough is the "Windows version" part of
  [ipc-channel.md](ipc-channel.md).
- **You learn:** pipe security descriptors, and why neither the DACL nor
  `FIRST_PIPE_INSTANCE` stops another user from squatting on the pipe name, so
  the UI has to check who is serving the pipe.

### #17 Phase 1 gate in CI (shared, in review)

- **How:** CI already builds `linux-asan` and `linux-tsan`. Add the fuzzer from
  #10 as a time-boxed job (libFuzzer needs Clang and `-fsanitize=fuzzer`).
- **Where it stands:** the `ipc-fuzz` job in `.github/workflows/ci.yml` runs
  `fuzz_ipc_framing` for 10 million inputs (about 30 seconds) and uploads the
  crashing input if it finds one. The SPSC and SeqLock stress tests already run
  in the `linux-tsan` matrix job.
- **You learn:** GitHub Actions jobs and matrices, and turning a spec gate into
  something that runs on every PR.

---

## Phase 2: real-time audio

Goal: play a synthetic 440 Hz sine from the ring buffer with zero dropouts, and
keep the buffer level steady even when the producer's clock runs slightly fast or
slow. Spec: 3.1.4, section 4 and the Phase 2 gate.

### #18 AudioSink interface (shared)

- **How:** open with a requested format, start, stop, report the sample rate the
  OS actually gave you. The OS calls you ("pull") on its own real-time thread;
  that callback reads from the ring buffer.
- **You learn:** push vs pull audio, and what "real-time thread" really means:
  the deadline is a few milliseconds and missing it is an audible click.
- **Where it stands:** proposed `audio_sink.hpp` and `realtime.hpp` are up for
  review. Read [audio-sink.md](audio-sink.md).

### #19 Linux: PipeWire sink (robbie)

- **How:** a `pw_stream` running on a `pw_thread_loop`. In the `process`
  callback: `pw_stream_dequeue_buffer`, `reserve_read` from the ring buffer, copy,
  `commit_read`, `pw_stream_queue_buffer`. Pin the ring buffer with `mlock` and
  ask rtkit for real-time priority. For the test signal, a sine generator keeps a
  phase accumulator: `phase += 2π · 440 / sample_rate`, wrapping at 2π.
- **Test:** listen for clicks, then prove it: run under
  `strace -f` and confirm our callback code adds no syscalls after setup. The
  PipeWire thread that calls it still makes its own (`epoll_wait`, `read`,
  `write`) to sleep and wake each cycle; those aren't ours.
- **Where it stands:** a first version is up for review on top of #18, with
  `tests/manual/play_sine.cpp` for listening.
  [audio-sink.md](audio-sink.md) has a section on how it works.
- **You learn:** the PipeWire graph model (nodes, ports, quantum), real-time
  scheduling (`SCHED_FIFO`, rtkit), page faults and why `mlock` prevents them, and
  how to spot hidden allocations.
- **Read:** the PipeWire tutorial (`tutorial4.c`, "playing a tone"), and Ross
  Bencina's "Real-time audio programming 101: time waits for nothing".

### #14 Windows: WASAPI shared low-latency sink (kero)

- **How:** `IAudioClient3::InitializeSharedAudioStream` at the minimum period,
  event-driven callback, MMCSS "Pro Audio" priority, `VirtualLock` on the buffer.
- **You learn:** COM basics without exceptions (check every `HRESULT`), the
  Windows audio engine's period model, MMCSS.

### #21 PI clock-drift controller and resampler (shared, no OS code)

- **How:** every callback, measure fill error (fill minus 4096 frames), smooth it
  with an exponential moving average (α = 0.005), feed it to a PI controller that
  outputs a playback ratio clamped to ±1000 ppm, and resample by that ratio with a
  16-tap windowed-sinc filter.
- **Test:** pure simulation, no sound card: a fake producer running 150 ppm fast,
  a fake consumer, 60 simulated minutes; fill must stay within ±3% of the target.
  This is `tests/test_resampler_drift.cpp`.
- **You learn:** why two clocks always drift, feedback control (P reacts to error
  now, I removes steady-state error, and integral windup if you forget to clamp
  it), low-pass filtering, and the basics of resampling and aliasing. Anyone can
  take this one; it needs no platform knowledge.
- **Read:** Fons Adriaensen, "Using a DLL to filter time" (the paper behind
  zita-ajbridge), and Julius O. Smith's online "Digital Audio Resampling Home Page".
- **Done:** [drift-controller.md](drift-controller.md) walks through both headers and the test.

### #7 Linux: ALSA fallback and bit-perfect `hw:` sink (robbie, parked until after phase 3)

- **How:** `snd_pcm_open` on `hw:X,Y`, negotiate the exact format the device
  supports, no software mixing.
- **You learn:** what "bit-perfect" means and why it rules out mixing, resampling
  and volume in software.

### #13 Windows: WASAPI exclusive mode (kero, later)

- **How:** `AUDCLNT_SHAREMODE_EXCLUSIVE` with event callbacks.
- **You learn:** the Windows counterpart of #7.

---

## Phases 3 to 5: CEF, metadata, media keys and the UI

Phase 3 now has issues (listed on the roadmap issue). The ones
started so far get a section here; the rest are summarized after them.

### #48 IPC message bodies (shared)

- **How:** one plain fixed-size struct per opcode in `src/common/ipc_messages.hpp`
  (`HelloBody`, `SetVolumeBody`, `CommandResultBody`, `PlaybackStateBody`; Play,
  Pause and Stop have none), a `WireBody` concept plus `static_assert`s on size and
  offsets, `encode_message()` to build a whole frame, and `decode_body<T>()` that
  refuses a wrong length or an out-of-range field. The frame fuzzer also decodes
  payloads.
- **You learn:** wire formats, why lengths must match exactly, and how a binary
  protocol gets versioned.
- **Read:** [ipc-messages.md](ipc-messages.md).

### #50 Daemon skeleton (shared)

- **How:** `main_daemon.cpp` sets up the reactor, Ctrl-C handling, the IPC
  listener (a second daemon gets `address_in_use` and exits), the PipeWire sink
  and the telemetry page, then runs the reactor. A 10 ms timer tops the ring
  buffer up with a 440 Hz tone and writes telemetry. `ControlServer` answers
  Hello, Play, Pause, Stop and SetVolume. `AudioPath` holds what the audio
  thread runs: ring buffer, drift controller, resampler, volume. Ctrl-C and
  SIGTERM go through a new `ShutdownSignal` interface (signalfd on Linux).
  Windows builds link against stubs that report "not supported".
- **You learn:** how a daemon is put together around one event loop, what may
  cross into the real-time thread, startup and shutdown order, and testing a
  whole program as a child process.
- **Read:** [daemon.md](daemon.md).

### #51 amctl, a command-line remote (shared)

- **How:** `tools/amctl/main_amctl.cpp` connects with `IpcChannel`, sends a Hello
  and one request, and waits (in `Reactor::run_once`, with a 2 s limit) for the
  `CommandAck` or `CommandNak` whose `request_seq_id` matches. `amctl status`
  skips IPC and maps the telemetry page read-only instead, which now carries the
  volume too. Argument parsing is a pure function in `amctl_args.hpp`, tested
  without a daemon; `test_daemon` runs the real binary against a real daemon.
- **You learn:** the client half of request/answer over IPC (matching answers by
  sequence number, skipping events you didn't ask for, timing out), and why
  splitting "parse the input" from "do the work" makes code easy to test.
- **Read:** [amctl.md](amctl.md).

### #52 Login storage interface (shared)

- **How:** `src/pal/interfaces/credential_vault.hpp` declares `CredentialVault`
  with `open()`, `store()`, `load()` and `remove()`, filed under a service named
  after the product. `load()` returns an empty `optional` for "nothing saved",
  so that case isn't an error. Both platform files are "not supported" stubs;
  `tests/test_credential_vault.cpp` is the contract #53 and #10 must pass. The
  same PR made the daemon republish telemetry the moment a command changes
  something, before it sends the ack.
- **You learn:** designing a small platform interface: picking the verbs, saying
  exactly what happens at the edges (missing secret, deleting twice, bad
  input), and writing the tests before either implementation exists.
- **Read:** [credential-vault.md](credential-vault.md).

### #53 Linux: login keyring vault (robbie)

- **How:** `src/pal/linux/secret_vault.cpp` stores each secret as one item in the
  desktop's login keyring through libsecret, tagged with two attributes (service
  and name) under the app's own schema. `open()` makes one real D-Bus call so a
  machine with no keyring fails there with "not supported", which is how CI skips
  the tests. The contract tests now use a fixed throwaway service per test and
  clean up at the start too, so a crashed run can't leave items behind.
- **You learn:** D-Bus (the message bus Linux desktop programs talk over), the
  Secret Service API every Linux keyring speaks, and how a C library's `GError`
  maps onto `std::error_code`.
- **Read:** [keyring-vault.md](keyring-vault.md).

### #55 Media keys interface (shared)

- **How:** `src/pal/interfaces/media_controls.hpp` declares `MediaControls`.
  `open(reactor, callback)` registers the daemon with the desktop; key presses
  and clicks in the media widget come back as one callback on the reactor
  thread, with a `MediaRequest` saying which (play, pause, next, seek to a
  position, set the volume...). Going the other way, plain setters
  (`set_track`, `set_playback_state`, `set_position`, `seeked`, `set_volume`)
  tell the desktop what's playing. Both platform files are "not supported"
  stubs; `tests/test_media_controls.cpp` is the daemon-side contract #56 and
  #9 must pass.
- **You learn:** designing an interface with traffic in both directions, why
  every event gets delivered on one thread, and why "how often may I call
  this?" belongs in the header.
- **Read:** [media-controls.md](media-controls.md).

### #56 Linux: MPRIS media keys (robbie)

- **How:** `src/pal/linux/mpris_controls.cpp` connects to the D-Bus session bus
  with sd-bus, claims `org.mpris.MediaPlayer2.<name>`, and publishes the two MPRIS
  interfaces as tables of method and property handlers. The connection's fd is
  watched by the reactor, so key presses reach the daemon on its one thread.
  The daemon handles them like the matching amctl command. Next, previous and
  seeking are switched off (`CanGoNext` and friends are false) until there's a
  real queue. `tests/test_mpris_controls.cpp` plays the desktop's side over a
  second connection, and CI runs every test under `dbus-run-session`.
- **You learn:** D-Bus objects, interfaces, methods, properties and signals, and
  how a C library's socket fits into an event loop.
- **Read:** [mpris.md](mpris.md).

### amctl does everything the UI can (shared)

- **How:** amctl gains `ping` (the daemon now answers `Ping` with a `Pong`
  carrying the same `seq_id`), `toggle`, `watch` (prints each
  `PlaybackStateEvent`) and `--json` for `status` and `watch`. `coverage()` in
  `amctl_args.hpp` names, for every opcode, the amctl command that sends it or
  why there isn't one. Its switch has no `default:`, so a new opcode doesn't
  compile until it's listed, and `test_daemon` checks the list against a real
  daemon. Load, seek, the queue and login get commands when the daemon can do
  them.
- **You learn:** enforcing a design rule with the compiler and a test, events
  versus polling, and why references break when the object they point at moves.
- **Read:** [amctl-parity.md](amctl-parity.md).

### #76 CEF in the daemon (shared)

- **How:** behind the `AM_WITH_CEF` CMake option (the `linux-cef` preset),
  `src/daemon/cef_host/` starts an invisible CEF on its own UI thread, serves a
  built-in page from memory at an `https://` address, and plays Google's
  Widevine test film with the Widevine CDM a browser already installed (found
  through `src/pal/interfaces/browser_files.hpp`). `CefAudioHandler` copies the
  decoded audio into the ring buffer with `AudioPath::write_planar()`, so it
  plays through the same drift controller and PipeWire sink as the tone. CEF's
  subprocesses run a separate helper program whose `main()` has no stack
  protector, which fixes the experiment's "stack smashing detected" crash.
  Hand test: `scratterd --test-film`, then pause it with amctl.
- **You learn:** how Chromium splits into processes, stack canaries and the
  zygote, sandboxes, running a library's event loop next to yours, and keeping
  a heavy dependency optional.
- **Read:** [cef-host.md](cef-host.md).

### #1 Fade in and out on play and pause (shared)

- **How:** `AudioPath::render()` keeps one gain between 0 and 1 and moves it a
  step per frame, up while playing and down while paused or stopped, so play
  ramps up over 10 ms and pause plays on for 10 ms while it ramps down. That
  removes the click right after play in the CEF hand test.
- **You learn:** why a jump in the samples is audible, and how to test for one.
- **Read:** [fade.md](fade.md).

### #2 The 4-hour soak test (shared)

- **How:** `tools/soak/soak.py` starts `scratterd --test-film`, and once a
  minute adds up the memory of the daemon and every CEF process under it
  (from `/proc/PID/status`) and asks `amctl status --json` how far playback
  got. After 4 hours it stops the daemon, reads its late-buffer and
  dropped-frame counts, and says whether memory grew after a 15-minute
  warm-up, by how much per hour, and in which process.
- **You learn:** what RSS and anonymous memory are, why a leak shows as a
  slope and not as a big number, and fitting a straight line to noisy data.
- **Read:** [soak-test.md](soak-test.md).

### Performance against the spec's goals (shared)

- **How:** `tools/perf/footprint.py` watches a running daemon and prints each
  process's memory (RSS, PSS, own), CPU, wake-ups and GPU use, then the totals
  against the goals. The daemon keeps one renderer instead of three, and
  ticks 4 times a second instead of 100 while paused. The `linux-cef-release`
  preset builds our code optimized, which cuts the audio path's CPU by more
  than ten times (`bench/audio_render`).
- **You learn:** why adding up RSS overcounts, what a process costs, why
  an idle program should rarely wake up, and what a debug build costs.
- **Read:** [performance.md](performance.md).

### Quiet while paused (shared)

- **How:** once a pause has faded out (`AudioPath::quiet()`), the daemon's
  tick stops the sink, so PipeWire's audio thread and the sound card can
  sleep; play starts it again at once. `write_planar()` drops the page's audio
  while paused, since nothing empties the ring buffer then.
  `footprint.py --threads` names the threads that still wake up.
- **You learn:** stopping a sound stream without a click, and telling which
  state another thread has seen by comparing counters.
- **Read:** [performance.md](performance.md#letting-the-sound-output-sleep-too).

### Still to come in phase 3

- **#4 Apple Music through MusicKit JS** (waits on the MusicKit key), **#3 the
  login popup** that stores the user token in the vault, and **#8** the
  Windows build of CEF. You learn: EME/Widevine against
  a real service, and OS secret storage in use. Spec 3.1.1, 3.1.2, 3.3.3.
- **Phase 4, metadata and media keys.** libcurl's multi interface driven by our
  reactor, `simdjson` On-Demand parsing into an arena allocator, MPRIS over sd-bus
  (Linux) and SMTC (Windows). You learn: async HTTP without threads, SIMD
  parsing, arena allocation, D-Bus. Spec 3.3.1, 3.3.2, 3.4.1.
- **Phase 5, the UI.** Dear ImGui on Vulkan (Linux) or DirectX 11 (Windows),
  reading the SeqLock page every frame, idling when nothing changes. You learn:
  immediate-mode UI and GPU swapchains. Spec 3.4.2.

---

## Concepts index

Where each idea first shows up, so you can jump to the issue that teaches it.

| Concept | Issue |
| --- | --- |
| Sanitizers (TSan, ASan, UBSan) | #3 |
| Atomics, acquire/release, false sharing | #4 |
| `std::span`, `requires` clauses | #4 |
| SeqLock, `std::atomic_ref`, wait-free writers | #5 |
| RAII, move-only types, `std::expected` | #6 |
| Static polymorphism with concepts | #6, #12 |
| Shared memory, `mmap` | #7, #8 |
| Checksums, intrinsics, CPU feature detection | #9 |
| Alignment, padding, endianness, `std::bit_cast` | #10 |
| Fuzzing | #10, #17 |
| Message vs stream transports | #11, #15, #12 |
| Readiness (epoll) vs completion (IOCP) I/O | #12, #13, #14 |
| Honest latency benchmarks | #15 |
| Real-time threads, `mlock`, zero-syscall audio | #18, #19, #14 |
| Clock drift, PI control, resampling | #21 |
| Message bodies, wire-format versioning | #48 |
| Event loops, signal handling, startup/shutdown order | #50 |
| Request/answer over IPC, testable command-line parsing | #51 |
| Designing a small interface, OS secret storage | #52, #53, #10 |
| D-Bus, the Secret Service, wrapping a C library's errors | #53 |
| Two-way interfaces, events on one thread, desktop media keys | #55, #56, #9 |
| Browser processes, stack canaries, sandboxes, optional dependencies | #76 |
| Clicks, fades | #1 |
