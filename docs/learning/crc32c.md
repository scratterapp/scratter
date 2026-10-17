# CRC32C, explained

This covers `src/common/crc32c.hpp` and its tests in `tests/test_crc32c.cpp`
(issue #9). It assumes you know some C++ but have never written a checksum.

## The problem it solves

The daemon and the UI talk by sending each other small binary messages over a
socket (Linux) or a named pipe (Windows). Every message starts with a 32-byte
`FrameHeader` (spec 3.2.1) that says what the message is and how long it is.

Before the receiver trusts any of those fields, it wants to know the 32 bytes
are the bytes the sender meant to send. Things that can go wrong:

- a bug on our side writes a half-built header,
- an old `am-ui` talks to a newer daemon with a different layout,
- some other program connects and sends junk.

The last 4 bytes of the header hold a **checksum** of the first 28. The sender
computes it and stores it. The receiver computes it again over the 28 bytes it
got and compares. If they differ, the frame is thrown away before anything reads
`payload_bytes` and goes off the end of a buffer.

## What a checksum is, and what a CRC is

A checksum squashes any number of bytes down to a small fingerprint (here, 32
bits) so that a small change in the input almost always changes the fingerprint.

The simplest checksum is "add up all the bytes". It's weak: swap two bytes and
the sum doesn't change. A **CRC** (cyclic redundancy check) is a smarter recipe
that is still cheap. The idea:

1. Treat the whole message as one huge binary number.
2. Divide it by a fixed number called the **polynomial**, using a kind of
   division where you XOR instead of subtract (no carries, no borrows).
3. The remainder is the CRC.

Because it is division, every bit of input affects the remainder. A good
polynomial guarantees things like: any single flipped bit is caught, any two
flipped bits are caught, and any burst of errors up to 32 bits long is caught.

What a CRC is **not**: security. Anyone can compute a matching CRC for a
malicious message. It protects against accidents and bugs, not attackers. Our
protection against other programs is the socket's `0600` permissions on Linux
and the pipe's access list (DACL) on Windows, not this.

## Why CRC32C and not plain CRC32

"CRC32" names a family. The one in zip files and Ethernet uses polynomial
`0x04C11DB7`. **CRC32C** (the C is for Castagnoli, who found it) uses
`0x1EDC6F41`, which catches more error patterns on short messages. It's what
iSCSI, ext4, Btrfs and SCTP use.

The practical reason the spec picks it: Intel added an instruction that computes
CRC32C directly (SSE4.2, 2008), and ARMv8 has one too. Plain CRC32 has no such
instruction. So on almost any machine this runs on, checking a header costs a
handful of CPU cycles.

## How the code works

There are two versions that must always give the same answer, plus one function
that picks between them.

### The portable version: a lookup table

The textbook way processes one **bit** at a time:

```cpp
crc ^= byte;
for (int bit = 0; bit < 8; ++bit) {
    crc = (crc & 1) ? (crc >> 1) ^ kCrc32cPolynomial : crc >> 1;
}
```

"If the low bit is 1, shift right and XOR in the polynomial; otherwise just
shift right." That is long division, one bit per step.

Eight steps per byte is slow, but notice the result of those 8 steps depends only
on the low 8 bits of `crc` at the start. So there are only 256 possible
outcomes. We work them all out once, into `kCrc32cTable`, and then each byte is
one lookup:

```cpp
crc = kCrc32cTable[(crc ^ byte) & 0xFF] ^ (crc >> 8);
```

The table is built by a lambda that runs at **compile time** (`constexpr`), so
it is baked into the program and costs nothing when it starts.

### Why the polynomial is written `0x82F63B78`

That's `0x1EDC6F41` with its 32 bits written in reverse order. CRC32C is
defined to read each byte starting from its lowest bit. Reversing the polynomial
lets the code shift right and use the low bit, which is simpler than reversing
every input byte. You'll see this called the "reflected" form.

### Start at all ones, finish by flipping

```cpp
std::uint32_t crc = 0xFFFFFFFF;
... process every byte ...
return crc ^ 0xFFFFFFFF;
```

Both are part of the CRC32C definition. Starting at zero would mean leading zero
bytes don't change the result, so a message and the same message with extra
zeros in front would look identical. Starting at all ones fixes that. The final
flip is just part of the standard so everyone gets the same numbers.

### The hardware version

On x86-64 the CPU instruction `crc32` does the table step for 8 bytes at once.
The compiler exposes it as `_mm_crc32_u64(crc, eight_bytes)`. So the loop is:

1. eat 8 bytes at a time with `_mm_crc32_u64`,
2. eat the 0 to 7 leftover bytes one at a time with `_mm_crc32_u8`.

Each 8-byte chunk is copied out with `std::memcpy` instead of casting the pointer
to `uint64_t*`. The data may not start on an 8-byte boundary, and reading through
a cast pointer would be undefined behavior. The compiler turns that `memcpy` into
a single load anyway.

ARM64 is the same shape with `__crc32cd` and `__crc32cb`.

### Is the instruction there? Runtime feature detection

Not every x86-64 CPU has SSE4.2 (very old ones don't), and running an
instruction the CPU doesn't have crashes the program. So we ask the CPU once:

- GCC and Clang: `__builtin_cpu_supports("sse4.2")`.
- MSVC: the `cpuid` instruction, leaf 1, bit 20 of ECX.

The answer goes in a function-local `static`, so it's asked on the first call
only. `crc32c()` uses the hardware version when the answer is yes and the table
when it's no.

GCC and Clang also refuse to emit SSE4.2 instructions in a normal function,
because the project doesn't compile for SSE4.2 everywhere. The
`[[gnu::target("sse4.2")]]` attribute allows it in just `crc32c_hardware`. That's
safe because we only call it after the check.

ARM64 is simpler: we only compile the hardware path when the compiler was told
the CPU has CRC (`__ARM_FEATURE_CRC32`, for example `-march=armv8-a+crc`), so
there is nothing to check at runtime. On a plain ARM64 build you get the table
version. Checking at runtime on ARM needs an OS call (`getauxval` on Linux), and
`src/common/` can't make OS calls.

### "No platform `#ifdef`s outside `src/pal/`"

This header does have `#if`s, but they ask "which **CPU** is this?" and "which
**compiler**?", never "which OS?". The x86 code is identical on Linux and
Windows. That's why it lives in `src/common/` alongside the other shared code.

## What the tests check

- **Known answers.** CRC32C of the text `123456789` is `0xE3069283`. That's the
  check value every CRC32C implementation publishes. RFC 3720 (iSCSI) appendix
  B.4 adds five more: 32 zero bytes, 32 `0xFF` bytes, 32 counting up, 32
  counting down, and a real 48-byte iSCSI command. Every one runs through the
  table version, the hardware version (when the CPU has it) and `crc32c()`.
- **Any single flipped bit is caught.** Flip each of the 224 bits of a 28-byte
  message, one at a time, and check the CRC changes every time.
- **The two versions agree everywhere.** Every length from 0 to 64 bytes, at
  every starting offset from 0 to 7. That covers every number of leftover tail
  bytes and every misalignment of the 8-byte loads. If the tail loop were wrong,
  the known-answer tests could miss it (they're mostly multiples of 8); this one
  wouldn't.

The first test case prints a warning if the hardware path didn't run, so you can
tell from the output which versions your machine actually tested.

## Running it on Arch

```sh
cmake --preset linux-debug
cmake --build --preset linux-debug
./build/linux-debug/tests/test_crc32c -s   # -s shows every passing check too
```

`linux-asan` is the sanitizer that matters here: it would catch the hardware loop
reading past the end of the buffer.

## Going further (not needed now)

The hardware loop does one `crc32` at a time, and each waits for the previous
result. Fast libraries run three independent streams at once and stitch the
results together, which is about three times faster on big buffers. Our input is
28 bytes, so it would make no difference. If a later issue needs to checksum
large payloads, look at how Abseil's `crc32c` or Mark Adler's `crc32c.c` do it.

## Where it's used next

#10 writes `encode_header` and `validate_header` in `src/common/ipc_protocol.hpp`.
Encoding fills `header_crc32c` with `crc32c()` over the first 28 bytes of the
header; validating recomputes it and rejects the frame if it doesn't match,
before trusting any other field.
