# Architecture

The daemon and client are separate processes. The client may attach, detach, or
restart without interrupting playback.

```text
+-----------------------------+                 +------------------------------+
|           am-ui             |                 |         scratterd           |
|  Dear ImGui detachable UI   |                 | Headless Widevine L3 + audio |
+-------------+---------------+                 +---------------+--------------+
              | Control Plane IPC (commands/events)              |
              | AF_UNIX SOCK_SEQPACKET / Windows Named Pipes    |
              +-------------------------------------------------+
              | Shared Memory Telemetry Plane (read-mostly)     |
              | mmap / MapViewOfFile + SeqLock                  |
              +-------------------------------------------------+
                                                            +---v------------------+
                                                            | In-process Audio Data |
                                                            | CefAudioHandler       |
                                                            | -> Lock-Free SPSC     |
                                                            | -> PI Clock-Drift     |
                                                            |    Resampler          |
                                                            | -> PipeWire / WASAPI  |
                                                            +-----------------------+
```

## Core subsystems

1. **Headless DRM & Audio Core** - Owns browser-media integration, Widevine L3
   playback, decode handoff, clocking, and audio output while remaining independent
   of the UI process.
2. **Aligned Binary Control IPC** - Uses versioned, aligned messages and CRC32C
   framing over `AF_UNIX SOCK_SEQPACKET` or Named Pipes.
3. **Async Metadata Engine** - Uses async `libcurl` and `simdjson` so artwork,
   playlists, search, and account metadata never block the audio path.
4. **OS Media Controls & Credential Vault** - Integrates MPRIS v2/SMTC and stores
   credentials through libsecret/DPAPI.

The shared-memory telemetry plane uses a SeqLock for consistent snapshots. The
audio data plane stays in-process with the daemon; control and telemetry are the
only cross-process paths.
