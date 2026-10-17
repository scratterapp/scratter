# Scratter

Scratter is an unofficial, cross-platform, native client for Apple Music, targeting
Arch Linux and Windows. It is intentionally split into two processes:

- **`scratterd`** is a headless daemon for Widevine L3 DRM, playback, native audio
  output, metadata, persistence, and OS media controls.
- **`am-ui`** is a detachable Dear ImGui client, modeled after `mpd` + `ncmpcpp`.
  Closing the UI does not stop background playback.

**Status:** early development, not usable yet. On Linux the daemon plays a test tone
and Google's Widevine test film, answers remote commands (`amctl`) and shows up in the
desktop's media controls. Apple Music itself, the Windows side and the UI are still
to come. Platform behavior belongs behind the Platform Abstraction Layer (`src/pal/`).

**New here, or an AI agent?** Start with [CLAUDE.md](CLAUDE.md): it covers who owns
what, the rules, how to build, and what to work on first.

> **Not affiliated with or endorsed by Apple Inc.** Apple Music is a trademark of
> Apple Inc. You need your own Apple Music subscription.

## Why this exists

| Capability | Scratter | Cider v1 (abandoned) | Cider 2 | Official Windows app |
| --- | --- | --- | --- | --- |
| Open-source path for Arch Linux and Windows | Yes | Partial/abandoned | No, proprietary Electron | No |
| UI attach/detach latency | Target sub-5 ms | N/A | N/A | N/A |
| Playback after UI closes | Yes, daemon remains active | No | No | No |
| Gaming impact | Daemon uses `--disable-gpu` (0 MB VRAM target); UI suspends its swapchain on unfocus/minimize or closes during gaming | N/A | Electron overhead | App remains resident |

## Build

Linux requires CMake 3.25+, Ninja, and Clang. Windows requires CMake 3.25+,
Ninja, and MSVC. [docs/SETUP.md](docs/SETUP.md) has install commands for Arch and
Windows.

```sh
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug
```

Windows developers use `windows-msvc-debug` from a Developer PowerShell.
Sanitizer presets are available as `linux-asan` (AddressSanitizer + UBSan) and
`linux-tsan` (ThreadSanitizer). Tests use Catch2.

## Documentation

- [docs/OVERVIEW.md](docs/OVERVIEW.md): what the system does, end to end
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): process and data-plane boundaries
- [docs/SPECIFICATION.md](docs/SPECIFICATION.md): full technical spec and phase roadmap
- [docs/RATIONALE.md](docs/RATIONALE.md): why this exists
- [docs/COLLABORATION.md](docs/COLLABORATION.md): parallel platform work
- [docs/learning/](docs/learning/README.md): how to do each roadmap item and what it teaches
- [docs/SETUP.md](docs/SETUP.md): installing dependencies
- [LEGAL.md](LEGAL.md): notice
