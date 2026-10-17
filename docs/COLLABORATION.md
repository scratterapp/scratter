# Collaboration guide

`@cocoa55` and `@kerelosgayed` work in parallel behind `src/pal/`. The daemon
and client consume platform-neutral interfaces and never contain platform
conditionals.

## Rules

- Do not add `#ifdef _WIN32` or `#ifdef __linux__` to `src/daemon/` or `src/client/`.
- Shared contracts live in `src/common/` and `src/pal/interfaces/`; changes require
  review from both developers.
- Put Linux implementations in `src/pal/linux/` and Windows implementations in
  `src/pal/win32/`.
- Keep platform dependencies out of shared headers unless the interface explicitly
  requires an opaque handle.

## Platform mapping

| Linux | Windows |
| --- | --- |
| `epoll` | IOCP |
| `AF_UNIX SOCK_SEQPACKET` | Overlapped Named Pipes |
| `shm_open` | `CreateFileMappingW` |
| PipeWire | WASAPI |
| sd-bus MPRIS v2 | WinRT SMTC |
| libsecret | DPAPI |
| Vulkan | DirectX 11 |

## Module roadmap

1. Agree on the interface and message/telemetry contract in a shared-contract PR.
2. Add Linux and Windows interface-preserving stubs; both developers review the
   contract before platform work begins.
3. Implement one platform module at a time: IPC, shared memory, credential vault,
   media controls, then audio sink.
4. Test each implementation in isolation with a platform-local harness and the
   smallest relevant CMake target.
5. Connect the module through `am_pal`, then run daemon/client smoke tests on both
   platforms.
6. Integrate cross-platform behavior only after both implementations pass their
   local tests; keep UI and daemon code free of platform branches.

Platform-only changes should use the matching CI path filter. Changes to contracts,
top-level CMake, or `cmake/` intentionally run both platform builds.
