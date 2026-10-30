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

## How to work an issue

1. Pick an issue from the pinned roadmap whose "blocked by" items are all closed.
   Shared items (`src/common/`, `src/pal/interfaces/`) come before the platform items
   that build on them.
2. Branch from `main` with a plain name: `feature/crc32c`, `fix/ipc-frame`.
3. Write the test first when you can. Every `tests/test_*.cpp` is its own executable;
   add its name to `AM_TESTS` in `tests/CMakeLists.txt` once it has a `TEST_CASE`.
4. Build and test with `linux-debug` (or `windows-msvc-debug`), then run the sanitizer
   that matters for the change: `linux-tsan` for anything with threads, `linux-asan`
   for anything that parses bytes or does pointer math.
5. Open a PR. CODEOWNERS asks kero to review only when it touches Windows-only files;
   for a shared-contract change, tag both robbie and kero yourself.
