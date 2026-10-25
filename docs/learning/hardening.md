# Build hardening

`cmake/Hardening.cmake` adds a handful of compiler and linker flags to every
program we build. None of them fixes a bug. What they do is change what happens
*after* a memory bug: instead of an attacker quietly taking over the program,
it crashes. A crash is a bug report; a takeover is someone reading your Apple
Music login.

Read with: OSTEP chapters 13 to 15 (address spaces and how a program's memory
is laid out), and the stack section of chapter 13 in particular. Most of these
flags protect one part of that layout.

## The attack they get in the way of

A typical memory bug is writing past the end of a buffer. On the stack, the
bytes after a local array include the **return address**: where the CPU jumps
when the function finishes. Overwrite that, and the function "returns" into
whatever code the attacker chose. Each flag below blocks one step of that.

| Flag | What it does | Protects |
| --- | --- | --- |
| `-fstack-protector-strong` | Puts a secret value (the *canary*) between local arrays and the return address, and checks it before returning. A write that runs over the array changes the canary, and the program aborts with "stack smashing detected". | The return address |
| `-D_FORTIFY_SOURCE=3` | Swaps `memcpy`, `strcpy`, `snprintf` and friends for versions that know the size of the buffer they write to (`__memcpy_chk`) and abort if it's too small. Only in optimized builds (Release, RelWithDebInfo, MinSizeRel): it needs the optimizer to work out sizes. | Buffers passed to C functions |
| `-D_GLIBCXX_ASSERTIONS` | `std::vector`, `std::span`, `std::array` and `std::string` check the index in `operator[]`, and `front()` and `back()` check for empty. Out of range aborts instead of reading or writing past the end. | Our own C++ indexing |
| `-fstack-clash-protection` | A function that needs a huge stack area touches it one page at a time, so it can't jump past the guard page below the stack into other memory. | The stack's edge |
| `-fcf-protection=full` | Marks the program ready for Intel CET: every place an indirect jump may land is marked, so the CPU can refuse others, and the CPU can keep a second, hidden copy of return addresses (the *shadow stack*). x86-64 only. On Linux this is mostly groundwork for now: the kernel doesn't enforce the jump check for programs, and glibc leaves the shadow stack off unless it's told to turn it on. | Jumps and returns, once enforced |
| PIE (`-pie`) | The program loads at a random address each run (ASLR), so an attacker doesn't know where its code is. | Guessing addresses |
| `-z relro -z now` | The table of addresses of library functions (the GOT) is filled in at startup and then made read-only, so a write can't redirect `free()` to something else. | Library calls |
| `-z noexecstack` | The stack can hold data but not runnable code. | Code injected on the stack |

On Windows, MSVC already does the PIE, non-executable stack and canary parts by
default (`/DYNAMICBASE`, `/NXCOMPAT`, `/GS`). We add `/guard:cf` (Control Flow
Guard, like CET's jump check) and, on x64, `/CETCOMPAT` (lets Windows turn on the shadow
stack).

## What it costs

Almost nothing. `bench/audio_render` measured 0.11 to 0.12% of one core with and
without the flags (clang 20, Release). `_GLIBCXX_ASSERTIONS` adds one compare
per index, and the optimizer drops most of them because it can see the index is
in range.

## Why the CEF helper opts out

`cef_helper_main.cpp` marks `main()` with `NO_STACK_PROTECTOR`. Chromium forks
its helper processes from a "zygote" and gives each child a new canary, but
`main()` was already running before the fork, so its check would fail on
return. The attribute turns the canary off for that one function only.
[cef-host.md](cef-host.md) has the whole story.

## Checking a binary

These commands show the marks the flags leave. CI runs them on the Release
daemon (`Check hardening` in `.github/workflows/ci.yml`).

```sh
readelf -hW build/linux-release/scratterd | grep Type     # DYN (Position-Independent Executable file)
readelf -dW build/linux-release/scratterd | grep FLAGS    # BIND_NOW, and NOW PIE
nm -D build/linux-release/scratterd | grep _chk           # __stack_chk_fail, __memcpy_chk
readelf -nW build/linux-release/scratterd | grep x86      # IBT, SHSTK: the CET-ready marks
```

`checksec --file=build/linux-release/scratterd` (from the `checksec` package)
shows the same in one table.

Configuring with `-DAM_HARDENING=OFF` builds without the flags, to compare.
Without them, a clang build on Ubuntu still gets PIE (the default there) but no
canary, no fortified calls and no `BIND_NOW`, which is why setting them in CMake
matters: what a compiler turns on by itself differs between distros.
