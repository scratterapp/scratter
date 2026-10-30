> **Repo note:** section 5 ("Repository Layout") has been updated to match the actual
> tree, including files that are still empty stubs. If the tree and this document ever
> disagree again, the tree wins; fix this document in the same PR that moves files.

> **Naming note:** the project is called Scratter. The name lives in `AM_APP_NAME` in the
> root `CMakeLists.txt` and reaches code as `am::common::kAppName` / `kDaemonName` in
> `src/common/app_name.hpp`; it must never contain "Apple". The daemon binary is
> `scratterd`. Runtime names such as the socket, the shared-memory page and the MPRIS
> bus name are built from those constants.

# System Architecture and Technical Specification: `scratterd` & `am-ui`

- **Document ID:** SYS-SPEC-AM-001
    
- **Architecture Revision:** 2.0 (Consolidated Production Baseline)
    
- **Language & Standard:** ISO C++20 / C++23 (`-std=c++23`, `-fno-exceptions` on real-time paths, `-fno-rtti`)
    
- **Target Platforms:** Linux (`x86_64` / `aarch64`, Kernel 6.1+) and Windows 10/11 (`x86_64`, Build 19041+)
    
- **Architecture Paradigm:** Decoupled Resident Media Daemon (`scratterd`) + Detachable Native Immediate-Mode Client (`am-ui`)
    

## 1. Abstract & System Problem Statement

Existing desktop clients for Apple Music suffer from severe architectural inefficiencies caused by monolithic WebView or Electron packaging. Wrapping both the User Interface and the Encrypted Media Extensions (EME) playback engine inside a persistent, multi-process Chromium visual tree conflates media transport with presentation rendering. On Linux, open-source Electron wrappers exhibit high idle memory footprints, compositor frame-pacing stutter, and fragile build dependencies; on Windows, the official WinUI/WebView2 hybrid client introduces significant cold-start latency, excessive background thread wakeups, and opaque audio routing through the Windows Audio Session API (WASAPI) shared mixer.

This specification defines a decoupled, dual-binary native architecture modeled after the classic Unix daemon/client split (`mpd` and `ncmpcpp`):

1. **`scratterd` (Resident Media & DRM Daemon):** A headless background service that encapsulates an off-screen Chromium Embedded Framework (CEF) runtime strictly for Widevine L3 EME license negotiation and stream decryption, tapping decoded 32-bit float Pulse-Code Modulation (PCM) frames directly into a lock-free Single-Producer Single-Consumer (SPSC) ring buffer that feeds the native OS audio subsystem (`PipeWire` on Linux; Low-Latency `WASAPI` on Windows) and exposes OS media transport controls (`MPRIS v2` / `SMTC`).
    
2. **`am-ui` (Detachable Native Frontend):** A stateless, hardware-accelerated Dear ImGui client (drawn through SDL3's renderer: `OpenGL`/`Vulkan` on Linux, `Direct3D` on Windows) coupled with an asynchronous `libcurl` + `simdjson` metadata engine. `am-ui` communicates with `scratterd` over a zero-copy-aligned local Control Plane and reads real-time playback telemetry from a lock-free shared-memory page, allowing the GUI process to terminate and relaunch in less than 5 milliseconds without interrupting active audio playback.
    

### Quantitative Target Benchmarks vs. Monolithic Electron Baselines

|**Metric**|**Monolithic Electron Client (Baseline)**|**scratterd + am-ui (Target Specification)**|**Architectural Mechanism**|
|---|---|---|---|
|**Resident Set Size (RSS) — Active GUI**|650 MB – 1,200 MB|Less than 160 MB combined (`scratterd` + `am-ui`)|Headless windowless CEF (GPU rasterizer disabled during steady state) + immediate-mode vertex buffer UI.|
|**Resident Set Size (RSS) — Headless Playback**|N/A (Closing window kills playback or hides full DOM)|Less than 115 MB (`scratterd` only)|Complete termination of `am-ui` process, freeing all VRAM textures, font atlases, and catalog metadata caches.|
|**UI Cold-Attach Latency**|1,800 ms – 4,200 ms|Less than 5 ms|Pre-allocated swapchain + instant `AF_UNIX` / Named Pipe handshake and `mmap` telemetry attach.|
|**Control IPC Round-Trip Latency**|2 ms – 15 ms (JSON over IPC / WebSockets)|Less than 25 microseconds (p99)|32-byte naturally aligned binary frames over kernel `SOCK_SEQPACKET` / `IOCP` pipes.|
|**Catalog JSON Deserialization**|12 ms – 45 ms (`V8 JSON.parse` + GC pauses)|Less than 0.8 ms (zero GC pauses)|`simdjson` On-Demand SIMD vector parsing directly over reusable `libcurl` ring buffers.|
|**Audio Callback Jitter**|Unbounded (Subject to JS/IPC congestion)|Wait-free ($O(1)$ bounded, zero syscalls)|Cache-line isolated SPSC ring buffer in `mlock`ed physical RAM with adaptive PI clock-drift resampling.|

These are design targets, written before anything was measured. With Chromium inside the daemon for Widevine, the measured baseline on 2026-10-19 was 693 MB and about 10% of one CPU core while playing. [`BENCHMARKS.md`](BENCHMARKS.md) has the measurements, the nearer goals and the progress towards them.

## 2. High-Level Process & Data Flow Topology

Plaintext

```
+===================================================================================================+
|                                    USER SPACE: DETACHABLE CLIENT                                  |
|  Process: am-ui (Stateless Native Frontend & Catalog Engine)                                      |
|                                                                                                   |
|  +----------------------------------------+          +-----------------------------------------+  |
|  | Dear ImGui Presentation Engine         |          | Asynchronous Metadata Engine            |  |
|  | - SDL3 + SDL_Renderer:                 |          | - libcurl multi_socket API              |  |
|  |   OpenGL/Vulkan, Direct3D (Windows)    |          | - simdjson On-Demand (AVX2 / NEON)      |  |
|  | - Event-driven frame throttling        |          | - Arena-allocated catalog models        |  |
|  +-------------------+--------------------+          +--------------------+--------------------+  |
|                      |                                                    |                       |
|                      +-------------------------+--------------------------+                       |
|                                                |                                                  |
|  +---------------------------------------------v-----------------------------------------------+  |
|  | Platform Event Reactor (Linux: epoll + timerfd / Windows: IOCP + wait timeout)              |  |
|  +-------------------+----------------------------------------------------+--------------------+  |
+======================|====================================================|=======================+
                       |                                                    |
        [1] Control Plane IPC (Bidirectional)                [2] Telemetry Plane (Read-Only Attach)
        - Linux: AF_UNIX (SOCK_SEQPACKET)                    - Linux: POSIX shm_open + mmap
        - Win32: Named Pipes (FILE_FLAG_OVERLAPPED)          - Win32: CreateFileMappingW + MapViewOfFile
        - Payload: 32B Aligned Header + TLV Binary           - Synchronization: Lock-Free SeqLock (120Hz)
                       |                                                    ^
+======================v====================================================|=======================+
|                                    USER SPACE: RESIDENT DAEMON                                    |
|  Process: scratterd (Session State, Headless EME Decryption, Real-Time Audio Sink)              |
|                                                                                                   |
|  +---------------------------------------------------------------------------------------------+  |
|  | Daemon Control Reactor & Session Coordinator                                                |  |
|  | - Command Dispatcher (Play, Pause, Seek, Queue Mutation, Auth Escalation)                   |  |
|  | - OS Media Bridge: MPRIS v2 (Linux sd-bus) / SMTC (Windows WinRT)                           |  |
|  | - Credential Vault Manager: libsecret (Linux) / DPAPI + CredMan (Windows)                   |  |
|  +-------------------+----------------------------------------------------+--------------------+  |
|                      | (CEF Task Posting)                                 | (SeqLock Publisher)   |
|  +-------------------v--------------------+                               |                       |
|  | Headless CEF Runtime (Windowless)      |                               |                       |
|  | - MusicKit JS Core + EME Bridge        |                               |                       |
|  | - Widevine L3 CDM (com.widevine.alpha) |                               |                       |
|  | - Stream Ceiling: 256 kbps AAC-LC      |                               |                       |
|  | - CefAudioHandler::OnAudioStreamPacket |                               |                       |
|  +-------------------+--------------------+                               |                       |
|                      |                                                    |                       |
|                      | [3] In-Process Audio Data Plane (32-bit Float PCM) |                       |
|                      | - mlock() / VirtualLock() pinned pages             |                       |
|                      | - 64B cache-line isolated SPSC Ring Buffer         |                       |
|                      v                                                    |                       |
|  +------------------------------------------------------------------------+--------------------+  |
|  | Real-Time Audio Consumer & Clock-Drift Resampler (RT Priority: rtkit / MMCSS "Pro Audio")   |  |
|  | - Delay-Locked Loop (DLL) + PI Controller monitoring SPSC fill watermark                    |  |
|  | - SIMD Polyphase Sinc Resampler (compensates CPU monotonic vs. DAC crystal skew)            |  |
|  | - Fast Radix-2 FFT Tap (writes 64-band spectrum + PTS to Shared Memory Telemetry Page)      |  |
|  +---------------------------------------------+-----------------------------------------------+  |
+================================================|==================================================+
                                                 |
                                  [4] Native OS Audio Subsystem
                                  - Linux: PipeWire (pw_stream) / ALSA fallback (later)
                                  - Win32: WASAPI Shared Low-Latency (IAudioClient3)
                                           + Optional WASAPI Exclusive Bypass
                                                 |
                                                 v
                                    [ Physical Hardware DAC ]
```

## 3. Subsystem Deep Dives

### 3.1 Daemon Runtime & Headless DRM Playback Core (`scratterd`)

#### 3.1.1 Off-Screen CEF Configuration & EME Attestation

`scratterd` hosts a minimal Chromium Embedded Framework (CEF) instance configured strictly for headless audio execution:

- **Initialization Flags:** `CefSettings.windowless_rendering_enabled = true`, `CefSettings.no_sandbox = false` (maintaining CDM process sandboxing required by Widevine), and command-line flags `--disable-gpu`, `--disable-gpu-compositing`, `--renderer-process-limit=1` (no spare renderers, see `docs/learning/performance.md`), `--disable-extensions`, `--disable-background-networking`, `--autoplay-policy=no-user-gesture-required`, `--disable-component-update` (only the Widevine CDM already installed on the machine is used) and `--password-store=basic`.
    
- **Render Throttling:** Because `windowless_rendering_enabled` requires a `CefRenderHandler`, the daemon implements a stub handler that provides a `1x1` dummy view rectangle and sets `CefBrowserHost::SetWindowlessFrameRate(1)`, eliminating Chromium compositor CPU overhead during steady-state playback.
    
- **Widevine CDM (never distributed):** The project does not ship, bundle, download on the user's behalf, or commit the Widevine CDM (`libwidevinecdm.so` / `widevinecdm.dll`). Google licenses it, and only licensees may redistribute it. The daemon loads a CDM already on the user's machine, such as the copy Chrome or Chromium installs, or a path the user sets, and reports a clear error if it finds none.
    
- **CEF build with AAC (built by us):** Apple Music streams AAC, and the prebuilt CEF downloads can't decode it (Chromium leaves patented codecs out unless asked). The #5 experiment confirmed this with CEF 150.0.22. So the daemon uses a CEF we build from source with `proprietary_codecs=true` and `ffmpeg_branding="Chrome"`, the same CEF branch as the prebuilt one otherwise. `docs/learning/cef-host.md` has the build command and settings (an official build, `is_official_build=true`); it takes several hours and about 100 GB of disk, so users never run it: releases would bundle the built `libcef` (how we ship is undecided until the AAC patent question below is settled). A Windows build is a separate run of the same steps. This plan stands only if that build turns out to play AAC (being checked on robbie's machine). Shipping a binary with an AAC decoder raises patent-licensing questions that must be settled before any public release.
    
- **VMP (open question):** Verified Media Path signing applies on Windows and macOS. CEF builds, ours included, aren't VMP-signed, and the only public signing route found so far is castLabs (EVS for their Electron fork, commercial certification for custom Chromium builds). On Linux an unsigned CEF got a Widevine license and protected audio with the system CDM in the #5 experiment, but only from Google's test license server so far; whether Apple's license server accepts it is part 2 of that experiment. On Windows, Apple's license server may refuse an unsigned build. Never fake, strip or bypass a VMP signature.
    
- **License negotiation:** During startup, `scratterd` serves its player page from memory at `https://player.invalid/` via a `CefSchemeHandlerFactory` registered for that https host (`src/daemon/cef_host/`). `.invalid` is a reserved name that can never be a real site, and https makes it a secure origin without any network request: Chromium refuses EME on `file://` and other opaque origins ("EME use is not allowed on unique origins"). This page loads MusicKit JS v3 and negotiates `com.widevine.alpha` EME sessions at Security Level L3, streaming **256 kbps AAC-LC** (44.1 kHz or 48 kHz stereo).
    

#### 3.1.2 `CefAudioHandler` Producer Stage

Instead of allowing Chromium's internal Audio Service to open an OS audio stream, `scratterd` overrides `CefClient::GetAudioHandler()`:

1. **`GetAudioParameters`:** Configures Chromium to deliver audio in standard native device rates (48,000 Hz, 2-channel stereo, 512 frames per packet = ~10.67 ms).
    
2. **`OnAudioStreamPacket`:** Invoked directly on Chromium's real-time audio worker thread with `const float** data` (planar 32-bit IEEE-754 float in range `[-1.0f, 1.0f]`), `int frames`, and `int64_t pts`.
    
3. **Planar-to-Interleaved Vectorization:** The callback interleaves `data[0][i]` (Left) and `data[1][i]` (Right) using AVX2 (`_mm256_unpacklo_ps` / `_mm256_unpackhi_ps`) or ARM NEON (`vzipq_f32`) intrinsics directly into the contiguous write span exposed by the in-process SPSC ring buffer.
    

#### 3.1.3 Cache-Aligned Lock-Free SPSC Ring Buffer

The ring buffer bridges the CEF audio producer thread and the OS audio consumer thread without mutexes, condition variables, or kernel syscalls.

C++

```
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <span>

namespace am::audio {

inline constexpr std::size_t kCacheLineSize = 64;

template <typename SampleT, std::size_t FrameCapacity, std::size_t Channels = 2>
    requires ((FrameCapacity != 0) && ((FrameCapacity & (FrameCapacity - 1)) == 0))
class alignas(kCacheLineSize) LockFreeSpscAudioBuffer {
public:
    static constexpr std::size_t kSampleCapacity = FrameCapacity * Channels;
    static constexpr std::size_t kIndexMask = FrameCapacity - 1;

    struct WriteReservation {
        std::span<SampleT> first;
        std::span<SampleT> second;
        std::size_t frames_reserved;
    };

    // Called exclusively by CefAudioHandler producer thread
    [[nodiscard]] WriteReservation reserve_write(std::size_t requested_frames) noexcept {
        const std::size_t write = write_frame_idx_.load(std::memory_order_relaxed);
        std::size_t avail = FrameCapacity - (write - cached_read_frame_idx_);

        if (avail < requested_frames) {
            cached_read_frame_idx_ = read_frame_idx_.load(std::memory_order_acquire);
            avail = FrameCapacity - (write - cached_read_frame_idx_);
        }

        const std::size_t to_write = (requested_frames < avail) ? requested_frames : avail;
        if (to_write == 0) {
            return {{}, {}, 0};
        }

        const std::size_t start_frame = write & kIndexMask;
        const std::size_t first_frames = std::min(to_write, FrameCapacity - start_frame);
        const std::size_t second_frames = to_write - first_frames;

        return {
            std::span<SampleT>(&storage_[start_frame * Channels], first_frames * Channels),
            std::span<SampleT>(&storage_[0], second_frames * Channels),
            to_write
        };
    }

    void commit_write(std::size_t written_frames) noexcept {
        const std::size_t next = write_frame_idx_.load(std::memory_order_relaxed) + written_frames;
        write_frame_idx_.store(next, std::memory_order_release);
    }

    // Called exclusively by PipeWire / WASAPI real-time consumer callback
    [[nodiscard]] std::size_t available_read_frames() noexcept {
        const std::size_t read = read_frame_idx_.load(std::memory_order_relaxed);
        cached_write_frame_idx_ = write_frame_idx_.load(std::memory_order_acquire);
        return cached_write_frame_idx_ - read;
    }

    void commit_read(std::size_t consumed_frames) noexcept {
        const std::size_t next = read_frame_idx_.load(std::memory_order_relaxed) + consumed_frames;
        read_frame_idx_.store(next, std::memory_order_release);
    }

private:
    // Producer-exclusive cache line: prevents MESI invalidation on consumer core reads
    alignas(kCacheLineSize) std::atomic<std::size_t> write_frame_idx_{0};
    std::size_t cached_read_frame_idx_{0};

    // Consumer-exclusive cache line: prevents MESI invalidation on producer core writes
    alignas(kCacheLineSize) std::atomic<std::size_t> read_frame_idx_{0};
    std::size_t cached_write_frame_idx_{0};

    // Pinned via mlock() / VirtualLock() at initialization
    alignas(kCacheLineSize) SampleT storage_[kSampleCapacity];
};

} // namespace am::audio
```

#### 3.1.4 Asynchronous Clock-Drift Compensation (DLL + PI Resampler)

Because Chromium pushes packets on the system monotonic clock (`CLOCK_MONOTONIC` / `QueryPerformanceCounter`) while the OS audio callback pulls samples timed by the physical audio interface's crystal oscillator, `scratterd` implements a closed-loop adaptive resampler on the consumer side:

1. **Target Watermark:** With a capacity of $2^{14} = 16,384$ frames (~341 ms at 48 kHz), the consumer pre-buffers until fill reaches $W_{\text{target}} = 4,096$ frames (~85.3 ms).
    
2. **Error Signal Filtering:** On every hardware audio callback, the instantaneous fill error $e[n] = \text{fill}[n] - W_{\text{target}}$ is passed through a single-pole Exponential Moving Average (EMA) low-pass filter ($\alpha = 0.005$) to strip packet arrival jitter while isolating secular clock drift.
    
3. **Proportional-Integral (PI) Ratio Control:** The smoothed error $\bar{e}[n]$ drives a PI controller computing the resampling ratio $\rho[n]$:
    
    $$\rho[n] = 1.0 + K_p \cdot \bar{e}[n] + K_i \sum_{k=0}^{n} \bar{e}[k]$$
    
    where $\rho[n]$ is clamped strictly to $[0.9990, 1.0010]$ ($\pm 1000\text{ ppm}$, completely inaudible pitch deviation) and feeds a 16-tap polyphase windowed-sinc SIMD interpolator.
    

4. **Implementation notes** (`src/daemon/audio_engine/`): the gains are $K_p = 3 \times 10^{-6}$ and $K_i = 3 \times 10^{-10}$ per callback, chosen by simulation to settle without oscillating for callbacks of 128 to 1024 frames. The integral stops growing once $K_i \sum \bar{e}$ alone reaches the clamp (anti-windup). The resampler blends the two nearest rows of a 256-phase table of Blackman-windowed sinc coefficients (cutoff 0.9 × Nyquist), and delays the audio by 8 frames. Fill is measured at one point in a sawtooth (packets in, callbacks out), so its average starts up to half a packet plus half a callback off target; the test gives the controller 60 simulated seconds to drain that before applying the ±3% limit.

### 3.2 Zero-Copy Control Plane IPC & Telemetry Shared Memory

#### 3.2.1 Wire Framing & Alignment Invariants

All Control Plane communication between `am-ui` and `scratterd` uses a fixed 32-byte naturally aligned binary header followed by a deterministic payload. Compiler-specific packed attributes (`__attribute__((packed))` or `#pragma pack(1)`) are strictly prohibited to prevent unaligned memory access undefined behavior (UB) and split-cache-line loads.

C++

```
#include <cstdint>
#include <type_traits>

namespace am::ipc {

inline constexpr uint32_t kProtocolMagic = 0x32504D41; // Bytes 41 4D 50 32 ("AMP2") on the little-endian wire
inline constexpr uint16_t kProtocolVersion = 0x0002;

enum class Opcode : uint8_t {
    // Lifecycle & Handshake (0x01 - 0x0F)
    HelloReq          = 0x01,
    HelloAck          = 0x02,
    Ping              = 0x03,
    Pong              = 0x04,
    AuthEscalationReq = 0x05,
    AuthStatusEvent   = 0x06,

    // Playback Transport Control (0x10 - 0x1F)
    LoadTrackReq      = 0x10,
    PlayReq           = 0x11,
    PauseReq          = 0x12,
    StopReq           = 0x13,
    SeekReq           = 0x14,
    SetVolumeReq      = 0x15,
    SetAudioModeReq   = 0x16,

    // Queue Mutation (0x20 - 0x2F)
    QueueReplaceReq   = 0x20,
    QueueAppendReq    = 0x21,
    QueueRemoveReq    = 0x22,
    QueueShuffleReq   = 0x23,

    // Asynchronous Daemon Events (0x80 - 0xFF)
    CommandAck        = 0x80,
    CommandNak        = 0x81,
    TrackChangedEvent = 0x82,
    EmeErrorEvent     = 0x83,
    PlaybackStateEvent = 0x84
};

struct alignas(8) FrameHeader {
    uint32_t magic;            // Offset 0x00 (4B): Must equal kProtocolMagic
    uint32_t payload_bytes;    // Offset 0x04 (4B): Length of trailing payload (0 .. 65536)
    uint64_t seq_id;           // Offset 0x08 (8B): Monotonic request/correlation ID
    uint64_t timestamp_ns;     // Offset 0x10 (8B): CLOCK_MONOTONIC / QPC nanoseconds
    uint16_t version;          // Offset 0x18 (2B): Protocol version (kProtocolVersion)
    Opcode   opcode;           // Offset 0x1A (1B): Frame operation code
    uint8_t  flags;            // Offset 0x1B (1B): Bit 0: ACK_REQUIRED, Bit 1: ERROR
    uint32_t header_crc32c;    // Offset 0x1C (4B): Hardware CRC32C of bytes [0x00 .. 0x1B]
};

static_assert(sizeof(FrameHeader) == 32, "FrameHeader must occupy exactly 32 bytes");
static_assert(alignof(FrameHeader) == 8, "FrameHeader must be 8-byte aligned");
static_assert(std::is_trivially_copyable_v<FrameHeader>, "FrameHeader must be trivially copyable");
static_assert(std::has_unique_object_representations_v<FrameHeader>, "Implicit padding is forbidden");

} // namespace am::ipc
```

In the repo this lives in `src/common/ipc_protocol.hpp` under `am::common`, next to `encode_header()` and `validate_frame()`.

**Message bodies.** The payload after the header is a fixed-size, trivially copyable struct per opcode, sent as its raw little-endian bytes with no padding (checked by `static_assert`s). They live in `src/common/ipc_messages.hpp`, with `encode_message()` (header plus body in one buffer, `payload_bytes` filled in) and `decode_body<T>()`, which refuses a payload whose length isn't exactly `sizeof(T)` and any enum or range field this build doesn't accept. Bodies defined so far:

| Opcode | Body | Bytes | Fields |
| --- | --- | --- | --- |
| `HelloReq`, `HelloAck` | `HelloBody` | 32 | `name`: sender's name, UTF-8, zero-padded (not necessarily zero-terminated). No version field: the header's `version` already rejects a mismatched peer. |
| `Ping`, `Pong` | none | 0 | Header-only. The daemon answers each `Ping` with a `Pong` whose header `seq_id` is the `Ping`'s, so the client can match them. |
| `PlayReq`, `PauseReq`, `StopReq` | none | 0 | Header-only frames. Any payload is refused. |
| `SetVolumeReq` | `SetVolumeBody` | 1 | `volume_percent`: 0 to 100 |
| `CommandAck`, `CommandNak` | `CommandResultBody` | 16 | `request_seq_id` (u64, the request's `seq_id`), `request_opcode` (u8), `status` (u8: 0 Ok, 1 BadRequest, 2 NotSupported, 3 Failed; Ack carries Ok, Nak the others), 6 reserved zero bytes |
| `PlaybackStateEvent` | `PlaybackStateBody` | 16 | `position_micros` (u64), `state` (u8, the telemetry page's `PlaybackState`), `volume_percent` (u8), 6 reserved zero bytes |

The other opcodes get bodies when the code that needs them is written. Until the first release, adding opcodes or bodies keeps `kProtocolVersion` at 2; after it, any change to an existing body's layout or meaning bumps it.

- **Integrity Validation:** `header_crc32c` is computed in 4 CPU cycles using SSE4.2 (`_mm_crc32_u64` / `_mm_crc32_u32`) on `x86_64` or ARMv8 CRC32 instructions (`__crc32cd`).
    
- **Message Boundary Guarantees:**
    
    - **Linux:** Uses `socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)` bound to `$XDG_RUNTIME_DIR/scratterd.sock` (permissions `0600`). The daemon refuses to start unless `$XDG_RUNTIME_DIR` is a directory owned by the current user with no group or other access (`0700`), and both ends check the other's uid with `SO_PEERCRED`, dropping a peer that runs as another user. `SOCK_SEQPACKET` enforces atomic record boundaries in the kernel—each `recvmsg()` returns exactly one complete frame, eliminating userspace stream-reassembly state machines.
        
    - **Windows:** Uses `CreateNamedPipeW(L"\\\\.\\pipe\\scratterd.<SID>", PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, ...)`, where `<SID>` is the current user's security identifier: Windows has one pipe namespace for the whole machine, so each user gets a pipe of their own. `PIPE_TYPE_MESSAGE` provides the exact Windows equivalent of `SOCK_SEQPACKET`. The pipe's DACL keeps other users out of the daemon, but any user can create the name first, so the UI checks that the process serving the pipe runs as the current user (see "Security" in `src/pal/interfaces/ipc_channel.hpp`).
        

#### 3.2.2 Shared Memory Telemetry Plane (`SeqLock`)

Polling playback cursor position, buffer health, or FFT audio spectrum over IPC sockets at 120 Hz wastes syscalls and context switches. Instead, `scratterd` exposes a single 4096-byte shared-memory page (`<daemon>_telemetry_v2`, made per-user by each platform; see section 4) mapped read-write in the daemon and read-only (`PROT_READ` / `FILE_MAP_READ`) in `am-ui`.

Synchronization uses a **Sequence Lock (`SeqLock`)**. The writer is wait-free; readers retry on a torn copy. The implementation is `src/common/telemetry_shm.hpp`.

1. The daemon (one writer thread) stores `seq + 1` (odd) to the atomic `seq_counter`, writes the snapshot, then stores `seq + 2` (even) with `memory_order_release`.

2. The `am-ui` render loop loads `seq1 = seq_counter.load(std::memory_order_acquire)`. If `seq1` is odd, a write is in progress and the attempt fails. Otherwise it copies the 320-byte `TelemetrySnapshot`, loads `seq2`, and accepts the copy only if `seq2 == seq1`. A failed attempt is not retried in a loop: if the daemon dies mid-write the counter stays odd forever, so am-ui makes one attempt per frame and keeps drawing the last good snapshot.

3. **The payload is not a plain struct in shared memory.** Copying a plain struct while another thread writes it is a data race, which is undefined behavior in C++ even when the torn copy is discarded, and ThreadSanitizer reports it. The page stores the snapshot as 40 `std::atomic<uint64_t>` words. The writer stores each word with `memory_order_release` and the reader loads each with `memory_order_acquire`. That keeps the odd counter store from sinking below the payload stores, and keeps the reader's second counter load from rising above the payload loads, without standalone fences (which ThreadSanitizer does not model). On x86-64 these compile to plain `mov`s; on ARM64 to `stlr`/`ldar`. See Hans Boehm, "Can Seqlocks Get Along With Programming Language Memory Models?" (2012).

4. Lock-free atomics are address-free, so the same layout works between processes through a read-only mapping. All reader methods are `const`.

C++

```
enum class PlaybackState : uint8_t { Stopped = 0, Playing = 1, Paused = 2, Buffering = 3 };

struct alignas(64) TelemetrySnapshot {
    uint64_t track_catalog_id;      // Active Apple Music catalog ID
    uint64_t pts_micros;            // Current playback cursor in microseconds
    uint64_t duration_micros;       // Total track duration in microseconds
    float    ring_buffer_fill_pct;  // SPSC fill ratio [0.0f .. 1.0f]
    float    clock_drift_ppm;       // Active PI resampler compensation in ppm
    uint32_t sample_rate_hz;        // Active sink sample rate (e.g., 48000)
    PlaybackState playback_state;
    uint8_t  exclusive_mode_active; // 1 if WASAPI Exclusive / Direct ALSA is active
    uint8_t  volume_percent;        // Output volume, 0 to 100 (same scale as SetVolumeBody)
    uint8_t  reserved_pad;          // Explicit padding
    std::array<float, 64> fft_magnitude_db; // 64-band logarithmic spectrum [-96.0f .. 0.0f dB]
    std::array<uint8_t, 24> reserved_tail;  // Fills the 296 bytes of fields out to 320, so there is no implicit padding
};
static_assert(sizeof(TelemetrySnapshot) == 320, "TelemetrySnapshot size mismatch");

// The shared page: the counter on its own cache line, then the payload as atomic words.
class alignas(64) TelemetryPage {                  // 384 bytes, fits in the 4096-byte page
    std::atomic<uint64_t> seq_;
    alignas(64) std::array<std::atomic<uint64_t>, 40> payload_;
};
```

### 3.3 Asynchronous Metadata & Session Engine

#### 3.3.1 Reactor-Driven HTTP/2 Pipeline (`curl_multi_socket_action`)

Catalog searches, album views, and playlist fetches execute inside `am-ui` (and token refresh/playback reporting inside `scratterd`) without blocking threads:

1. A persistent `CURLM*` handle is initialized with HTTP/2 multiplexing (`CURLPIPE_MULTIPLEX`) and TLS session caching to `amp-api.music.apple.com`.
    
2. `CURLMOPT_SOCKETFUNCTION` registers socket file descriptors directly with the platform reactor (`epoll_ctl` on Linux; `CreateIoCompletionPort` / `WSAEventSelect` on Windows).
    
3. `CURLMOPT_TIMERFUNCTION` updates the reactor's single timer (`timerfd_settime` on Linux; on Windows `set_timer()`, whose deadline becomes the `GetQueuedCompletionStatusEx` wait timeout).
    
4. When the reactor wakes on network readiness, it invokes `curl_multi_socket_action()`, streaming response chunks into a pre-allocated, reusable slab buffer with `SIMDJSON_PADDING` (64 bytes) appended at the tail.
    

#### 3.3.2 Zero-Allocation JSON Deserialization (`simdjson` On-Demand)

Apple Music's REST API returns deeply nested JSON-API payloads (`data[].relationships.tracks.data[].attributes`). Using DOM parsers (`nlohmann::json`) causes thousands of small heap allocations per query.

- A single `simdjson::ondemand::parser` instance is allocated once at startup and reused across all HTTP completions.
    
- The parser iterates over the raw network buffer in-place using AVX2 / AVX-512 / ARM NEON instructions, extracting string views (`std::string_view`) that are copied directly into a contiguous **Per-View Linear Arena Allocator**. When the user navigates to a different album view in `am-ui`, the entire arena is reset in $O(1)$ time by resetting a single pointer offset.
    

#### 3.3.3 Ephemeral OAuth / 2FA Escalation & Native Credential Vault

To satisfy Apple’s SRP-6a password exchange, 2FA verification, and anti-bot JS challenges without keeping a visible browser running:

1. **Cold Boot Vault Check:** On startup, `scratterd` queries the OS credential store for the `Developer-Token` and `Music-User-Token`. Where the developer token comes from is not decided yet (see the open decisions in `CLAUDE.md`). Developer keys and tokens are never committed or shipped.
    
    - **Linux:** Communicates over D-Bus with `org.freedesktop.secrets` (`libsecret-1`) storing tokens encrypted in the user's login keyring (GNOME Keyring / KWallet).
        
    - **Windows:** Encrypts tokens using Windows Data Protection API (`CryptProtectData` bound to `CRYPTPROTECT_UI_FORBIDDEN` under the current user SID) and stores the blob in Windows Credential Manager (`CredWriteW`).
        
2. **Ephemeral Popup Escalation:** If the vault is empty or `amp-api.music.apple.com` returns `HTTP 401/403`, `scratterd` emits an `AuthStatusEvent` over IPC and spawns a temporary, non-windowless child `CefBrowser` window pointed at `authorize.music.apple.com`.
    
3. **Token Interception & Teardown:** A `CefRequestHandler` and `CefCookieVisitor` monitor the authentication completion callback. The moment the `media-user-token` is captured, `scratterd` commits it to the OS vault, destroys the visible `CefBrowser` window, and reverts to 100% headless operation.
    

### 3.4 Desktop Media Integration & Native Frontend (`am-ui`)

#### 3.4.1 Native OS Media Transport Bridges (Hosted in `scratterd`)

Because `am-ui` can be closed at any time, OS media integration resides inside `scratterd`:

- **Linux (`MPRIS v2`):** Implemented via `sd-bus` (`libsystemd` C API) claiming bus name `org.mpris.MediaPlayer2.<kAppName>` on the session D-Bus. Implements `org.mpris.MediaPlayer2` and `org.mpris.MediaPlayer2.Player` interfaces, publishing track metadata, artwork URLs, and playback status signals (`PropertiesChanged`), and routing hardware media keys (`PlayPause`, `Next`, `Previous`, `Seek`) directly into the daemon's command dispatcher.
    
- **Windows (`SMTC`):** Implemented via `Windows.Media.SystemMediaTransportControls` through the raw WinRT ABI with WRL (C++/WinRT throws, and the project builds without exceptions), attached to a hidden top-level window owned by `scratterd` on a thread of its own that pumps its messages (`GetForWindow` refuses a message-only `HWND_MESSAGE` window with `E_INVALIDARG`), providing native Windows 11 media flyout integration, thumbnail streaming via `RandomAccessStreamReference`, and hardware media key hooks.
    

#### 3.4.2 Stateless Immediate-Mode Frontend (`am-ui`)

- **Render Loop Discipline:** To prevent typical game-loop GPU waste, `am-ui` operates an **Adaptive Event-Driven Render Loop**:
    
    - **Idle / Minimized:** Unmaps the high-frequency spectrum widget and blocks inside `SDL_WaitEventTimeout(250ms)` (0.1% CPU, 0% GPU).
        
    - **Active Window (No Spectrum):** Renders on user input events (two frames per event, so ImGui can settle hover and layout) or 1 Hz progress bar ticks while playing. With nothing playing it sleeps until the next event. `src/client/frame_pacer.hpp` holds these rules.
        
    - **Active Window (Spectrum/Lyrics Enabled):** Locks to display VSync (60/120/144 Hz), reading the `SeqLock` shared memory telemetry page in less than 50 nanoseconds per frame.
        

## 4. Cross-Platform OS Abstraction Layer (PAL)

All OS-specific syscalls are strictly isolated behind C++20 Concepts and static polymorphism (`std::conditional_t` platform traits) rather than virtual inheritance on hot paths.

Where both platforms can store the same members, there is a simpler form of the same idea: the class is declared once in `src/pal/interfaces/` and each platform's `.cpp` defines its member functions, so the linker picks the implementation. `ShmMapping` (`shm_mapping.hpp`) works this way. The shared-memory name is plain (`<daemon>_telemetry_v2`, built from `AM_APP_NAME`) and each platform makes it per-user: Linux uses `/<daemon>_telemetry_v2.<uid>` and refuses an object some other user owns, Windows uses `Local\<daemon>_telemetry_v2` (per session). Errors are `std::error_code`, as in the Reactor and IpcChannel interfaces.

|**Subsystem Capability**|**Linux Implementation (src/pal/linux/)**|**Windows Implementation (src/pal/win32/)**|
|---|---|---|
|**Event Reactor**|`epoll_create1(EPOLL_CLOEXEC)` + `eventfd` (cross-thread wakeup) + `timerfd_create(CLOCK_MONOTONIC)`|`CreateIoCompletionPort` (IOCP) + `PostQueuedCompletionStatus` (cross-thread wakeup) + the timer deadline folded into the `GetQueuedCompletionStatusEx` timeout|
|**Control IPC Transport**|`AF_UNIX` with `SOCK_SEQPACKET \| SOCK_NONBLOCK` in `$XDG_RUNTIME_DIR`|Named Pipes (`\\.\pipe\scratterd.<SID>`) with `PIPE_TYPE_MESSAGE \| FILE_FLAG_OVERLAPPED`|
|**Shared Memory Telemetry**|`shm_open()` + `ftruncate()` + `mmap(PROT_READ, MAP_SHARED)`|`CreateFileMappingW(INVALID_HANDLE_VALUE, ...)` + `MapViewOfFile(FILE_MAP_READ)`|
|**Primary Audio Sink**|`PipeWire` (`pw_stream` native graph node) with dynamic quantum negotiation; `ALSA` (`snd_pcm_open`) fallback, parked until after phase 3 (issue #7)|`WASAPI Shared Low-Latency` (`IAudioClient3::InitializeSharedAudioStream` at minimum periodicity, typically 2–10 ms)|
|**Bit-Perfect Bypass Sink**|Direct hardware `ALSA` (`hw:X,Y`) bypassing user-space mixing, parked with the fallback (issue #7)|`WASAPI Exclusive Mode` (`AUDCLNT_SHAREMODE_EXCLUSIVE` + event-driven `AUDCLNT_STREAMFLAGS_EVENTCALLBACK`)|
|**Real-Time Thread Priority**|`org.freedesktop.RealtimeKit1` (`MakeThreadRealtime`) or `sched_setscheduler(SCHED_FIFO)`|Multimedia Class Scheduler Service (`AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_idx)`)|
|**Physical RAM Pinning**|`mlock(ptr, len)` (faults pages in up front)|`VirtualLock(ptr, len)` (after adjusting `SetProcessWorkingSetSizeEx`)|
|**Credential Vault**|`libsecret-1` (`org.freedesktop.secrets` over D-Bus)|DPAPI (`CryptProtectData`) + Windows Credential Manager (`wincred.h`)|
|**Desktop Media Controls**|`sd-bus` (`org.mpris.MediaPlayer2.Player`)|WinRT `SystemMediaTransportControls` (raw ABI with WRL) + `ITaskbarList3`|
|**Window, Input & Graphics (`am-ui` only)**|Not in the PAL. `am-ui` uses SDL3, which picks Wayland or X11 and draws through `SDL_Renderer` with OpenGL or Vulkan|Not in the PAL. The same SDL3 code; `SDL_Renderer` draws with Direct3D 11 or 12|

The window and graphics row is the exception to this section: SDL3 already hides the platform, so `am-ui` has no platform code and needs no PAL interface (issue #35). Dear ImGui draws through its `imgui_impl_sdl3` and `imgui_impl_sdlrenderer3` backends. If a measurement ever shows `SDL_Renderer` is too slow, or a view needs custom shaders, a hand-written Vulkan or DirectX backend can replace it; that touches only `src/client/main_client.cpp`.

## 5. Repository Layout & Entry-Point Architecture ("Where to Start")

To maintain strict compilation firewalls (ensuring `am-ui` never compiles or links against `libcef`), the project is structured as a multi-target CMake workspace:

Plaintext

```
scratter/
├── CLAUDE.md / AGENTS.md              # Start here: ownership, rules, build/test (AGENTS.md points to CLAUDE.md)
├── LEGAL.md                           # Notice: unofficial, not affiliated with Apple
├── SECURITY.md                        # How to report a vulnerability, and what counts as one
├── CMakeLists.txt                     # Root build definition, C++23 flags, sanitizer options
├── CMakePresets.json                  # Configure/build/test presets: linux-debug/-asan/-tsan/-fuzz/-release/-cef/-cef-release, windows-msvc-debug/-release
├── cmake/
│   ├── FindCEF.cmake                  # Our AAC-enabled CEF build in third_party/cef/ as CEF::CEF, when AM_WITH_CEF is on (never the Widevine CDM)
│   ├── CompilerWarnings.cmake         # -Wall -Wextra -Wpedantic -Werror -Wconversion, no exceptions/RTTI
│   ├── EmbedFile.cmake                # am_embed_file(): compiles a file (am-ui's fonts) into a program as a byte array
│   ├── Hardening.cmake                # Stack protector, FORTIFY_SOURCE, bounds-checked std containers, PIE, full RELRO (MSVC: /guard:cf, /CETCOMPAT)
│   └── Sanitizers.cmake               # ASan+UBSan / TSan flags applied to every target
├── docs/                              # OVERVIEW, ARCHITECTURE, SPECIFICATION, RATIONALE, COLLABORATION, SETUP, BENCHMARKS, release-checklist; assets/ (logo, banner), learning/, windows/START_HERE.md
├── .github/                           # CODEOWNERS, PR template, CI (path-filtered per platform, actions pinned by commit), Dependabot
├── src/
│   ├── common/                        # Target: am_common (header-only INTERFACE library - zero OS/CEF deps)
│   │   ├── app_name.hpp               # kAppName / kDaemonName from AM_APP_NAME (project name)
│   │   ├── ipc_protocol.hpp           # 32B aligned FrameHeader, Opcodes, static_asserts
│   │   ├── ipc_messages.hpp           # Payload bodies (Hello, SetVolume, CommandAck, PlaybackState)
│   │   ├── telemetry_shm.hpp          # SeqLock and TelemetrySnapshot definitions
│   │   ├── spsc_ring_buffer.hpp       # Cache-aligned LockFreeSpscAudioBuffer<T, N>
│   │   ├── crc32c.hpp                 # CRC32C: SSE4.2 / ARMv8 instructions, table fallback
│   │   └── arena_allocator.hpp        # Linear slab allocator for catalog metadata
│   ├── pal/                           # Target: am_pal (Static Library - OS Primitives)
│   │   ├── interfaces/                # Shared contracts, reviewed by both platform owners
│   │   │   ├── reactor.hpp            # Reactor: handle watches, wakeup, one timer
│   │   │   ├── ipc_channel.hpp        # SeqPacket / NamedPipe framing abstraction
│   │   │   ├── shm_mapping.hpp        # POSIX shm / Win32 FileMapping RAII wrappers
│   │   │   ├── audio_sink.hpp         # Real-time PCM consumer (shared and bit-perfect modes)
│   │   │   ├── realtime.hpp           # Physical RAM pinning (mlock / VirtualLock)
│   │   │   ├── shutdown_signal.hpp    # Ctrl-C / SIGTERM as a reactor callback
│   │   │   ├── credential_vault.hpp   # Keyring / DPAPI token storage interface
│   │   │   ├── media_controls.hpp     # OS media transport (MPRIS / SMTC) interface
│   │   │   ├── browser_files.hpp      # CEF's profile folder, the program's folder, the installed Widevine CDM
│   │   │   └── cef_main_args.hpp      # CefMainArgs per OS; its .cpp files build am_pal_cef (only with AM_WITH_CEF)
│   │   ├── linux/                     # Owner: Linux developer
│   │   │   ├── epoll_reactor.cpp      # epoll + eventfd + timerfd
│   │   │   ├── unix_seqpacket_ipc.cpp # AF_UNIX SOCK_SEQPACKET in $XDG_RUNTIME_DIR
│   │   │   ├── posix_shm.cpp          # shm_open + ftruncate + mmap
│   │   │   ├── pipewire_sink.cpp      # pw_stream primary sink
│   │   │   ├── alsa_sink.cpp          # ALSA fallback and hw:X,Y bit-perfect bypass (parked until after phase 3)
│   │   │   ├── rtkit_priority.cpp     # mlock (PipeWire gives its own thread RT priority)
│   │   │   ├── secret_vault.cpp       # libsecret (org.freedesktop.secrets), login keyring
│   │   │   ├── signalfd_shutdown.cpp  # SIGINT/SIGTERM blocked and read through a signalfd
│   │   │   ├── mpris_controls.cpp     # sd-bus MPRIS v2: org.mpris.MediaPlayer2.<name>, fd watched by the reactor
│   │   │   ├── browser_files.cpp      # ~/.cache/<app>/browser, /proc/self/exe, known WidevineCdm folders
│   │   │   └── cef_main_args.cpp      # CefMainArgs from argc and argv
│   │   └── win32/                     # Owner: Windows developer
│   │       ├── iocp_reactor.cpp       # IOCP + PostQueuedCompletionStatus + timer in the wait timeout
│   │       ├── named_pipe_ipc.cpp     # Overlapped message-mode Named Pipes
│   │       ├── win32_shm.cpp          # CreateFileMappingW + MapViewOfFile
│   │       ├── wasapi_sink.cpp        # WASAPI shared low-latency (IAudioClient3), MMCSS "Pro Audio" thread; exclusive mode to come
│   │       ├── mmcss_priority.cpp     # VirtualLock (the MMCSS call is in wasapi_sink.cpp)
│   │       ├── dpapi_vault.cpp        # DPAPI + Credential Manager
│   │       ├── console_shutdown.cpp   # SetConsoleCtrlHandler, onto the reactor thread through Reactor::wake()
│   │       ├── smtc_controls.cpp      # SystemMediaTransportControls (WinRT ABI with WRL), hidden window on its own thread
│   │       ├── browser_files.cpp      # CEF profile folder and installed Widevine on Windows
│   │       └── cef_main_args.cpp      # CefMainArgs from GetModuleHandleW(nullptr)
│   ├── net/                           # Target: am_net (Static Library - Async HTTP & JSON)
│   │   ├── curl_reactor_bridge.cpp    # Binds curl_multi_socket_action to pal::Reactor
│   │   └── catalog_parser.cpp         # simdjson On-Demand Apple Music API parsers
│   ├── daemon/                        # Target: scratterd (Executable - Links am_common, am_pal, CEF)
│   │   ├── main_daemon.cpp            # Daemon entry point: startup order, tick timer, reactor loop, shutdown
│   │   ├── control_server.hpp/.cpp    # IPC clients: Hello, Play/Pause/Stop, SetVolume, state broadcasts
│   │   ├── cef_host/                  # Headless CEF (built only with AM_WITH_CEF; VMP check still to come)
│   │   │   ├── cef_host.hpp           # CefHost: start CEF, play/pause/stop the page; no CEF types
│   │   │   ├── cef_host.cpp           # CefApp, CefClient, CefAudioHandler -> AudioPath::write_planar
│   │   │   ├── cef_host_off.cpp       # Stand-in when CEF is off: start() says how to turn it on
│   │   │   ├── cef_helper_main.cpp    # <daemon>-cef-helper: CEF's subprocesses (NO_STACK_PROTECTOR main)
│   │   │   └── test_film_page.hpp     # Built-in page at https://player.invalid/: Widevine test film
│   │   └── audio_engine/              # PI clock-drift controller, SIMD resampler, FFT tap
│   │       ├── audio_path.hpp         # Ring buffer (tone or CEF writes it) -> drift -> resampler -> volume; the sink's render callback
│   │       ├── sine_generator.hpp     # 440 Hz test tone, the producer until CEF arrives
│   │       ├── drift_controller.hpp   # EMA + PI controller: ring buffer fill -> playback ratio
│   │       └── polyphase_resampler.hpp # 16-tap windowed-sinc resampler at that ratio (scalar for now)
│   └── client/                        # Target: am-ui (Executable - Links am_client_core, am_common, am_pal, am_net, SDL3, ImGui)
│       ├── CMakeLists.txt             # am-ui, plus SDL3 and Dear ImGui via FetchContent and the embedded fonts
│       ├── main_client.cpp            # SDL3 window, ImGui setup, the render loop and the player's keys
│       ├── frame_pacer.hpp            # When to draw and how long to sleep (adaptive render loop, 3.4.2)
│       ├── theme.hpp / theme.cpp      # Colors, spacing and fonts
│       ├── daemon_link.hpp/.cpp       # Connection thread: hello, commands, state events, reconnects (library am_client_core)
│       ├── ui_state.hpp/.cpp          # DaemonState, plus the once-per-frame SeqLock telemetry reader (am_client_core)
│       ├── now_playing_text.hpp       # The now-playing view's words and numbers (clock, volume steps, errors)
│       ├── fonts/                     # Atkinson Hyperlegible + Instrument Serif (OFL), compiled into am-ui
│       └── views/                     # ImGui views (Library, Search, Queue, Spectrum)
│           ├── waiting_view.cpp       # Shown while the daemon isn't connected, with how to start it
│           ├── now_playing.cpp        # What's playing, position, play/pause, stop, volume (#37)
│           └── drawing.cpp            # The background record and app label both views share
├── tools/                             # Small helper programs (the C++ ones link am_common + am_pal only)
│   ├── amctl/                         # Command-line remote for the daemon, like mpc for mpd
│   │   ├── main_amctl.cpp             # Every request the daemon handles, over IPC; status from telemetry
│   │   └── amctl_args.hpp             # Command-line parsing, and which command sends each opcode
│   ├── perf/
│   │   └── footprint.py               # Linux-only Python script: memory (RSS, PSS), CPU, wake-ups and GPU use of a running daemon, per process, and its busiest threads (by wake-ups for every process with --threads)
│   ├── soak/
│   │   └── soak.py                    # Linux-only Python script: plays for hours, logs memory per process each minute
│   └── test_page/
│       └── serve.py                   # Serves the daemon's built-in test film page on localhost, to run it in a browser
├── bench/                             # Hand-run benchmarks, not in ctest
│   ├── ipc_ping_pong.cpp              # IPC round-trip latency (target: median under 25 µs)
│   └── audio_render.cpp               # CPU our audio code (write_planar, render) takes per second of audio
├── third_party/
│   ├── cef/                           # Our CEF build goes here (linux64/, windows64/); ignored by git
│   └── cef-patches/                   # Our patches to Chromium, applied before building CEF
│       └── widevine-cdm-dir.patch     # Windows: register the installed Widevine CDM named by --widevine-cdm-dir
└── tests/                             # Catch2 v3 (FetchContent); one executable per file
    ├── test_sanity.cpp                # Trivial test proving the framework and sanitizers run
    ├── test_spsc_concurrency.cpp      # TSan multi-threaded ring buffer stress test
    ├── test_telemetry_seqlock.cpp     # TSan one-writer, two-reader SeqLock torn-read test
    ├── test_shm_mapping.cpp           # ShmMapping create/open/move, plus the two-process SeqLock test
    ├── shm_reader_child.cpp           # Second process for that test: maps the page read-only and checks reads
    ├── test_win32_shm.cpp             # Windows-only: file-mapping owner/DACL, page-rounded size, name lifetime
    ├── telemetry_stamp.hpp            # Test helper: snapshots whose fields all derive from one number
    ├── test_reactor.cpp               # Reactor timer, wakeup and stop (both platforms)
    ├── test_epoll_reactor.cpp         # Linux-only: watching fds with the epoll reactor
    ├── test_iocp_reactor.cpp          # Windows-only: watching handles with the IOCP reactor, over overlapped pipes
    ├── test_unix_seqpacket_ipc.cpp    # Linux-only: the SOCK_SEQPACKET IPC channel
    ├── test_named_pipe_ipc.cpp        # Windows-only: the named pipe IPC channel
    ├── test_crc32c.cpp                # CRC32C known-answer vectors on every code path
    ├── test_ipc_framing.cpp           # FrameHeader layout, round-trip and rejection tests
    ├── test_ipc_messages.cpp          # Message bodies: round trips, wrong sizes and bad values
    ├── fuzz_ipc_framing.cpp           # libFuzzer target for the frame parser and body decoders (linux-fuzz preset)
    ├── test_realtime.cpp              # lock_memory / unlock_memory
    ├── test_pipewire_sink.cpp         # Linux-only: PipeWire sink (passes with a warning if no daemon)
    ├── test_wasapi_sink.cpp           # Windows-only: WASAPI sink (passes with a warning if no output device)
    ├── test_audio_path.cpp            # The daemon's render callback: play, pause, stop, fades, volume, underruns, CEF's planar input
    ├── test_daemon.cpp                # Linux-only: starts the daemon binary, drives it directly and with amctl
    ├── test_daemon_link.cpp           # Linux-only: am-ui's DaemonLink against the real daemon (kill, restart) and a broken one
    ├── daemon_process.hpp             # Starting a daemon child with a private $XDG_RUNTIME_DIR, for both daemon tests
    ├── test_amctl_args.cpp            # amctl's argument parsing
    ├── test_frame_pacer.cpp           # am-ui's render loop rules: when to draw, how long to sleep
    ├── test_now_playing_text.cpp      # The now-playing view's clock, volume steps and error lines
    ├── test_credential_vault.cpp      # CredentialVault contract (skips where there's no vault or no keyring running)
    ├── test_media_controls.cpp        # MediaControls contract, daemon side (skips where there's no media control service)
    ├── test_mpris_controls.cpp        # Linux-only: plays the desktop's side of MPRIS over D-Bus (skips with no session bus)
    ├── test_smtc_controls.cpp         # Windows-only: the desktop's side of SMTC through Windows' session manager (skips with no desktop session)
    ├── test_browser_files.cpp         # Linux-only: where the browser profile goes, and that it's private (0700)
    ├── tsan.supp                      # TSan suppressions for GLib (libsecret's D-Bus thread), used by the linux-tsan preset
    ├── manual/play_sine.cpp           # Not in ctest: plays 440 Hz through the sink, counts late buffers
    ├── manual/vault_demo.cpp          # Not in ctest: saves a dummy secret to look at in Seahorse, then deletes it
    └── test_resampler_drift.cpp       # Simulated +/-150 ppm skew over 60 minutes, plus resampler signal checks
```

Layout rules that follow from this tree:

- Platform code lives only under `src/pal/linux/` and `src/pal/win32/`, behind a contract in `src/pal/interfaces/`. `src/daemon/`, `src/client/`, `src/common/`, `src/net/` and `tools/` contain no platform conditionals. The OS media bridges (Section 3.4.1) run inside `scratterd` but are implemented as `media_controls.hpp` plus `mpris_controls.cpp` / `smtc_controls.cpp`, not as a daemon subdirectory.
- `am_common` stays a header-only `INTERFACE` library while every file in it is a header.
- CEF is linked only into `scratterd`; `am-ui` never links it.
- The `am-ui` window and graphics backend live inside SDL3 (Section 4), so `am-ui` has no platform code and nothing of it goes into `am_pal`, which the daemon links.


### Exact Bootstrap Order (Where to Write the First Line of Code)

Do **not** begin with CEF or ImGui. Build from the core invariants outward so every layer is testable in isolation:

1. **Step 1 (`src/common/spsc_ring_buffer.hpp` & `src/common/telemetry_shm.hpp`):** Implement the cache-aligned SPSC ring buffer and the `SeqLock` telemetry structure. Validate them immediately with a standalone multi-threaded test under ThreadSanitizer (`-fsanitize=thread`).
    
2. **Step 2 (`src/common/ipc_protocol.hpp`, `src/pal/interfaces/ipc_channel.hpp`, `src/pal/interfaces/reactor.hpp`, then `src/pal/linux/unix_seqpacket_ipc.cpp` + `epoll_reactor.cpp` and `src/pal/win32/named_pipe_ipc.cpp` + `iocp_reactor.cpp`):** Implement the 32-byte `FrameHeader`, hardware CRC32C check, and the `AF_UNIX` (`SOCK_SEQPACKET`) / Named Pipe reactor loop. Write a CLI ping-pong benchmark verifying sub-25µs round-trip latency.
    
3. **Step 3 (`src/pal/interfaces/audio_sink.hpp`, `src/pal/linux/pipewire_sink.cpp`, `src/pal/win32/wasapi_sink.cpp`):** Implement the `PipeWire` and `WASAPI` real-time audio consumers reading a synthetic 440 Hz sine wave from the `LockFreeSpscAudioBuffer`. Verify zero dropouts and validate the PI clock-drift resampler against synthetic producer jitter.
    
4. **Step 4 (`src/daemon/cef_host/`):** Integrate headless CEF + `CefAudioHandler` to replace the synthetic sine wave producer with decoded MusicKit JS PCM frames.
    
5. **Step 5 (`src/net/` & `src/client/`):** Bring up the `libcurl` + `simdjson` reactor and attach the Dear ImGui frontend.
    

## Non-goals

These are out of scope on purpose, to keep the project a player. Don't add them, and decline PRs that do:

- Recording, saving, exporting or piping decrypted audio anywhere other than the OS audio output. PCM stays in RAM.
- Removing, bypassing or weakening DRM, extracting keys, or faking a VMP signature.
- Shipping or bundling the Widevine CDM.
- Getting around Apple Music account limits, such as the one-stream-per-device rule, or sharing accounts.
- Bulk catalog scraping or ignoring Apple's rate limits (`429`).

## 6. Phased Implementation Roadmap & Verification Plan

### Phase 1: Lock-Free Primitives, Shared Memory & Control IPC

- **Deliverables:** `am_common` and `am_pal` libraries (`Reactor`, `LockFreeSpscAudioBuffer`, `SeqLock` shared memory, and `FrameHeader` IPC transport on Linux and Windows).
    
- **Verification Gate:**
    
    - Zero warnings under `-Wall -Wextra -Wpedantic -Wconversion -Werror`.
        
    - 100 million SPSC frames transferred across two pinned CPU cores under ThreadSanitizer (`TSan`) with zero data races and bitwise-identical output verification.
        
    - `libFuzzer` harness feeding mutated byte streams into the IPC frame parser for 10 million iterations under AddressSanitizer (`ASan`) and UndefinedBehaviorSanitizer (`UBSan`) with zero crashes or out-of-bounds reads.
        

### Phase 2: Native Real-Time Audio Engine & Clock-Drift Loop

- **Deliverables:** `PipeWire` (`Linux`) and `WASAPI Shared/Exclusive` (`Windows`) audio sinks, `mlock`/`VirtualLock` memory pinning, `rtkit`/`MMCSS` real-time thread elevation, SIMD planar-to-interleaved conversion, and the PI-controlled polyphase resampler.
    
- **Verification Gate:**
    
    - Inject a synthetic $+150\text{ ppm}$ producer clock skew into the SPSC ring buffer over a simulated 60-minute run; verify that the PI controller stabilizes the ring buffer fill level within $\pm 3\%$ of the 85 ms target watermark with zero underruns or overruns.
        
    - Verify via `strace -f -e trace=all` (Linux) or ETW (Windows) that **our audio callback code** makes no system calls, takes no locks, allocates no memory and incurs **zero page faults** (`minflt=0, majflt=0`) after stream initialization. The OS audio thread that calls it is not ours and still makes its own syscalls to sleep and wake each cycle (PipeWire's data thread does `epoll_wait`, `read` and `write`); the rule covers only what our callback adds on top.
        

### Phase 3: Headless CEF Runtime, Widevine L3 & OAuth Vault

- **Deliverables:** Windowless `scratterd` daemon hosting MusicKit JS, VMP signature check, ephemeral OAuth/2FA browser escalation, `libsecret`/`DPAPI` credential persistence, and `CefAudioHandler::OnAudioStreamPacket` feeding the SPSC ring buffer.
    
- **Verification Gate:**
    
    - Successful cold-boot authentication capturing `Music-User-Token` into the OS keyring, followed by a daemon restart that authenticates and begins streaming a licensed 256 kbps AAC-LC track completely headlessly.
        
    - Sustained 4-hour playback soak test verifying flat RSS memory usage (zero memory leaks in the CEF-to-SPSC bridge).
        
    - Before building on `CefAudioHandler`, confirm that CEF delivers EME-protected audio to `OnAudioStreamPacket` without patching Chromium. If it only works with a patch that defeats a protection, stop and rethink the design. (#5 part 1: confirmed for protected Opus with stock CEF; turning on AAC is a build setting, not a patch. Part 2 repeats it with a real Apple Music track.)
        

### Phase 4: Reactor Metadata Engine & OS Media Bridges

- **Deliverables:** `curl_multi_socket_action` integrated into `epoll`/`IOCP`, `simdjson` On-Demand catalog parsers with linear arena allocation, plus `MPRIS v2` (`sd-bus`) and `SMTC` (`WinRT`) controllers inside `scratterd`.
    
- **Verification Gate:**
    
    - Parse a 500-track Apple Music playlist JSON payload in less than 1.5 ms with zero individual heap allocations outside the linear arena.
        
    - Confirm `playerctl play-pause`, `playerctl next`, and KDE/GNOME/Windows 11 media applets accurately reflect track metadata and control playback when no UI client is running.
        

### Phase 5: Detachable `am-ui` Frontend & Production Profiling

- **Deliverables:** Hardware-accelerated Dear ImGui client (SDL3 + `SDL_Renderer`), adaptive idle frame throttling, 64-band FFT spectrum visualizer reading the `SeqLock` telemetry page, and sub-5ms attach/detach lifecycle.
    
- **Verification Gate:**
    
    - Kill and relaunch `am-ui` 50 consecutive times during active track playback; verify zero audible glitches in `scratterd` and measure cold-attach time from `execve()` to first rendered frame at less than 5 ms.