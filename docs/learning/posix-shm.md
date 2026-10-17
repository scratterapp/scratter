# POSIX shared memory, explained

This covers `src/pal/linux/posix_shm.cpp` and its tests in
`tests/test_shm_mapping.cpp` (issue #7). It fills in the interface from
[shm-mapping.md](shm-mapping.md), so read that first.

## What a shared-memory object is

On Linux, `shm_open("/scratterd_telemetry_v2.1000", ...)` opens a file called
`/dev/shm/scratterd_telemetry_v2.1000`. "scratterd" is the daemon's name
(`AM_APP_NAME` plus "d") and 1000 is the
user ID; more on that below. `/dev/shm` is a tmpfs: a filesystem that
lives in RAM, so nothing is ever written to disk. You can `ls /dev/shm` while the
daemon runs and see it.

`shm_open` gives back a file descriptor, like `open` does. On its own that
isn't shared memory yet; it's just a file. Two more calls turn it into memory:

1. `ftruncate(fd, 4096)` sets the file's size. A brand-new object is 0 bytes;
   growing it fills the new bytes with zeros.
2. `mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)` maps those
   4096 bytes into this process's address space and returns a pointer to them.

`MAP_SHARED` is the important flag. It means the pointer goes straight to the
file's pages in RAM, so every process that maps the same file sees the same
bytes. (`MAP_PRIVATE` would give each process its own copy on first write.)

The two processes get **different virtual addresses** for the **same physical
memory**. The test "a read-only open sees what the creator wrote" shows exactly
that: the two pointers differ, but a byte written through one shows up through
the other.

## Walking through `create()`

```cpp
const int fd = ::shm_open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
```

- `O_CREAT` makes the object if it doesn't exist. There's no `O_EXCL`, so if a
  crashed daemon left one behind we reuse it instead of failing forever.
- `O_CLOEXEC` closes the descriptor automatically if this process ever runs
  another program. The daemon will launch CEF's helper processes; without this
  flag each of them would inherit a descriptor to our telemetry page.
- `0600` means only the same user can open it.

Then `check_owner`, then `ftruncate`, then `mmap`, then **`close(fd)`**. Closing the descriptor
right away surprises people. It's fine: the mapping holds its own reference to
the object, and the descriptor is no longer needed.

Every failure path closes the descriptor before returning. Each OS call reports
its reason in `errno`, which goes back to the caller as
`std::error_code(errno, std::system_category())`. Note the code saves `errno`
into a local *before* calling `close`, because `close` could overwrite it.

## Keeping it to one user

`/dev/shm` is one folder for the whole machine, so a fixed name like
`/scratterd_telemetry_v2` would be shared by everyone logged in. That breaks
two ways:

- **Two users at once.** The first user's daemon creates the object with mode
  `0600`. The second user's daemon tries to open the same name and gets
  `EACCES`, so its telemetry never works.
- **Someone else gets there first.** Another local user creates the name with
  mode `0666` before our daemon starts. Without `O_EXCL`, our `create()` would
  happily open their object and use it. They could then read which track we're
  playing, or write fake telemetry for our UI to show.

So `posix_name()` adds the user ID to the name (`geteuid()`, "effective user
ID"), giving every user their own page. And after `shm_open`, `check_owner()`
runs `fstat` on the descriptor and refuses the object with `permission_denied`
unless its owner (`st_uid`) is us. A planted object then fails loudly instead
of being used. `open_read_only()` does the same check, so the UI never trusts a
page someone else made.

## Walking through `open_read_only()`

Same three steps with less power: `O_RDONLY` instead of `O_RDWR`, and
`PROT_READ` instead of `PROT_READ | PROT_WRITE`. Writing through this pointer
would crash the UI with a segfault, which is the point: the UI can never
corrupt the daemon's page.

There's one extra check. The same `fstat` also reads the object's current size, and if it's
smaller than asked for, the call fails with `result_out_of_range`. Without it, `mmap` would
happily succeed and the first read past the end of the object would kill the
process with `SIGBUS`.

## Cleaning up: `reset()`

`munmap` undoes the mapping. Then, only on the mapping `create()` returned,
`shm_unlink` removes the name from `/dev/shm`.

Unlinking works like deleting an open file: the name is gone at once, but the
memory stays until the last process unmaps it. So a UI that's already showing
the page keeps working, and a UI started afterwards gets `no_such_file_or_directory`, which is
the honest answer when no daemon is running.

## Putting a TelemetryPage in the region

The mapping is just bytes. The daemon turns it into a page with
`std::construct_at`, which runs `TelemetryPage`'s constructor at that address:

```cpp
TelemetryPage* page = std::construct_at(
    reinterpret_cast<TelemetryPage*>(mapping->writable_bytes().data()));
page->write(snapshot);
```

The UI never constructs anything. It casts its read-only bytes to
`const TelemetryPage*` and calls `read()`, which only loads. Loading a lock-free
atomic from read-only memory is fine on x86-64 and ARM64; that's why
`telemetry_shm.hpp` asserts the atomics are lock-free.

`mmap` returns page-aligned memory (4096 bytes), so the page's `alignas(64)`
is satisfied for free.

## The two-process test

Issue #7 is done when one process writes through the SeqLock and another reads
it without ever seeing a torn snapshot. Threads already proved that in
`test_telemetry_seqlock.cpp`; this proves it across a real process boundary,
which is how the daemon and UI will actually run.

- `test_shm_mapping.cpp` creates the region, constructs a `TelemetryPage` in
  it, and starts a thread that writes numbered snapshots as fast as it can.
- It then runs `shm_reader_child` with `std::system`, passing the region's name.
  The child is a tiny separate program (`tests/shm_reader_child.cpp`) that opens
  the region read-only and reads in a loop. Every field of a snapshot is derived
  from its number (`tests/telemetry_stamp.hpp`), so one mismatched field means a
  torn read. It exits 0 after seeing 1,000 different snapshots with no tears.
  The writer yields between writes; flat out, the counter is odd so often that
  on a slow machine the other process hardly ever gets a clean read.
- The parent stops the writer and checks the exit code.

`std::system` keeps the test free of `fork`/`exec` and other Linux-only calls,
so kero can reuse it as-is once `win32_shm.cpp` lands. Until then CMake only
builds the test on Linux.

Each test uses a random region name, because names are global to the machine
and ctest may run several tests at once.

## Read more

- `man 7 shm_overview` for the whole API in two pages.
- `man 2 mmap`, especially `MAP_SHARED` and the note on `SIGBUS`.
- `man 2 open` on `O_CLOEXEC`.
