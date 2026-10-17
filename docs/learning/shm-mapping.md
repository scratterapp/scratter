# The ShmMapping interface, explained

This covers `src/pal/interfaces/shm_mapping.hpp` (issue #6). It is the contract
that both `posix_shm.cpp` (#7, Linux) and `win32_shm.cpp` (#8, Windows) fill in.
The thing it maps is the telemetry page from [seqlock.md](seqlock.md).

## What it has to do

Two jobs, one per process:

- The daemon **creates** a named 4096-byte region and maps it read-write, so it
  can put a `TelemetryPage` in it.
- The UI **opens** the same region by name and maps it read-only, so a bug in
  the UI can't scribble on the daemon's data.

Both sides must unmap the region when they're done, including on early returns.
That's what RAII is for.

## RAII in one paragraph

RAII ("resource acquisition is initialization") means an object owns a resource
and its destructor gives it back. `std::unique_ptr` does it for heap memory;
`ShmMapping` does it for a mapping. You never call "unmap" yourself: when the
`ShmMapping` goes out of scope, `~ShmMapping()` calls `reset()`, which unmaps.

## Why move-only

If you could copy a `ShmMapping`, both copies would unmap the same region in
their destructors, and the second unmap would hit memory that's already gone.
So the copy constructor and copy assignment are `= delete`d.

Moving is fine, and needed: `create()` builds the mapping inside the function and
hands it back to the caller. The move constructor copies the pointer and size
across, then sets the source back to empty (`nullptr`, size 0). The empty
source's destructor then has nothing to unmap. The move assignment does the same,
but first calls `reset()` to let go of whatever it held before.

## Why `std::expected` and a private constructor

The project builds with `-fno-exceptions`, so a constructor has no way to say
"`shm_open` failed". Instead the two ways of getting a mapping are static
functions:

```cpp
auto m = am::pal::ShmMapping::open_read_only(am::pal::kTelemetryShmName, 4096);
if (!m) {
    // m.error() == std::errc::no_such_file_or_directory if the daemon
    // isn't running yet.
    return;
}
auto bytes = m->bytes(); // std::span<const std::byte>, 4096 long
```

`std::expected<ShmMapping, std::error_code>` holds either a working mapping or an
error, never both. `if (!m)` checks which; `*m` or `m->` gets the mapping;
`m.error()` gets the error. The constructor that takes a raw pointer is private,
so the only way to get a non-empty `ShmMapping` is through those two functions.

The error is a `std::error_code`, the same type the Reactor and IpcChannel
interfaces use. When the OS fails, it holds the raw `errno` (Linux) or
`GetLastError()` (Windows) number, and `m.error().message()` turns that into
text like "No such file or directory" for the log. To decide what to do, compare
it with a portable `std::errc` value: `m.error() == std::errc::permission_denied`
works on both platforms, because the standard library knows which `errno` and
which Windows error codes mean the same thing.

## One class, two `.cpp` files

The spec asks for platform code picked at build time, not virtual functions.
The usual way to do that is a C++20 concept plus a different class per
platform. Here both platforms only need to remember the same five things
(pointer, size, writable, the name if they created it, and a native handle the
creator keeps open), so there is one class, declared once in the header. Linux
leaves that handle empty; on Windows the creator stores its section handle
there, because a region's name lasts only while a handle to it is open.
`posix_shm.cpp` defines `create`,
`open_read_only` and `reset` on Linux; `win32_shm.cpp` defines them on Windows.
`src/pal/CMakeLists.txt` compiles only one of the two, so the linker sees one
definition of each.

The header includes nothing platform-specific (no `<sys/mman.h>`, no
`<windows.h>`), which is a project rule for shared headers.

## Read-only means read-only

`bytes()` gives a `std::span<const std::byte>` on any mapping.
`writable_bytes()` gives a writable span only on a mapping from `create()`, and an
empty span otherwise, so code that tries to write through the UI's mapping gets
nothing to write into instead of a crash.

## Who removes the name

The mapping returned by `create()` remembers the name. When the daemon's mapping
is destroyed, the name is removed, so a UI started afterwards gets `no_such_file_or_directory`
instead of a page that nobody updates any more. A UI that already had the page
open keeps its mapping. On Linux that's an explicit `shm_unlink`; on Windows it's
automatic: the creator holds the only lasting handle, so the name disappears when
that handle closes.

On Linux, if the daemon crashes the name is left behind, and the next `create()`
reuses it rather than failing, which is why `create()` doesn't insist the region
be new. On Windows a crash closes the daemon's handle, so the name goes away on
its own and the next `create()` makes a fresh region.
