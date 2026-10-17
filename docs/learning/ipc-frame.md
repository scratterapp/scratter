# The IPC frame header, explained

This covers `src/common/ipc_protocol.hpp`, its tests in `tests/test_ipc_framing.cpp`
and the fuzzer in `tests/fuzz_ipc_framing.cpp` (issue #10). Read
[crc32c.md](crc32c.md) first; the header's last field is a CRC32C.

## The problem it solves

`am-ui` and `scratterd` are separate programs. When you press Pause, the UI has
to tell the daemon "pause" over a socket (Linux) or a named pipe (Windows). A
socket only moves bytes, so both sides need to agree exactly what those bytes
mean. That agreement is the **wire format**.

Every message is one **frame**: a 32-byte header, then a payload of 0 to 64 KiB.
The header says what the message is (the **opcode**: `PauseReq`, `Ping`,
`TrackChangedEvent`...) and how long the payload is. The payload carries the
details, like which track to load. What each payload holds is in
[ipc-messages.md](ipc-messages.md).

## The header, byte by byte

```
offset  size  field           example
0x00    4     magic           41 4D 50 32   "AMP2", says "this is one of our frames"
0x04    4     payload_bytes   04 00 00 00   4 bytes of payload follow
0x08    8     seq_id          ...           matches replies to requests
0x10    8     timestamp_ns    ...           sender's clock when it sent this
0x18    2     version         02 00         protocol version 2
0x1A    1     opcode          03            Ping
0x1B    1     flags           01            bit 0: please acknowledge
0x1C    4     header_crc32c   B2 46 1F B3   CRC32C of bytes 0x00..0x1B
```

The first test in `test_ipc_framing.cpp` checks exactly these bytes, so if anyone
reorders a field by accident the test fails.

## Why the struct can be sent as raw bytes

The simplest way to turn a struct into bytes is to copy its memory. That only
works if three things are true, and the header checks all three at compile time.

**No padding.** Compilers put each field at an address that's a multiple of its
size (a `uint64_t` at a multiple of 8). If a field wouldn't land there, the
compiler quietly inserts unused **padding** bytes before it. Padding holds
whatever garbage was in memory, which would then go on the wire and into the
checksum. The fields here are ordered so every one already lands in the right
place: 4 + 4, then 8, then 8, then 2 + 1 + 1 + 4. That adds up to 32 with no gaps.

```cpp
static_assert(std::has_unique_object_representations_v<FrameHeader>);
```

This asks the compiler: "does every bit of this struct belong to some field?"
It's only true when there's no padding. If someone adds a field in the wrong
place, the build breaks instead of the protocol.

**No `#pragma pack`.** The other way to get rid of padding is to tell the compiler
to pack fields tightly. That makes fields **unaligned** (a `uint64_t` at address
5), and reading one is slow on some CPUs and undefined behavior in C++. The spec
bans it.

**Same byte order on both ends.** A `uint32_t` like `0x32504D41` is stored in
memory lowest byte first on x86-64 and ARM64 (**little-endian**): `41 4D 50 32`.
A big-endian CPU would store it the other way round. Both our platforms are
little-endian, so we skip byte-swapping and assert it instead:

```cpp
static_assert(std::endian::native == std::endian::little);
```

That's also why the magic constant looks backwards. `0x32504D41` is what you
write in C++ so that the bytes on the wire spell `AMP2` in a hex dump. (An early
draft of the spec had it the other way, `0x414D5032`, which shows up as `2PMA`.
One of the tests uses that value to check bad magic is rejected.)

## Going between bytes and the struct

Never do this:

```cpp
auto* header = reinterpret_cast<const FrameHeader*>(buffer); // don't
```

It breaks C++'s **strict aliasing** rule (you may only read memory through the
type it was written as, with a few exceptions), and `buffer` might not be 8-byte
aligned. Both are undefined behavior: it usually works, until an optimizer
decides it doesn't.

Instead the code **copies** with `std::memcpy` and `std::bit_cast`:

```cpp
FrameHeaderBytes raw;                                   // std::array<std::byte, 32>
std::memcpy(raw.data(), bytes.data(), kFrameHeaderBytes);
const auto header = std::bit_cast<FrameHeader>(raw);
```

`std::bit_cast` (C++20) says "give me these exact bits as another type". It only
compiles when both types are the same size and trivially copyable, which the
`static_assert`s guarantee. Compilers turn the copy into a couple of register
loads, so it costs nothing.

## Encoding

```cpp
FrameHeader h{};
h.opcode = Opcode::Ping;
h.seq_id = next_id++;
FrameHeaderBytes bytes = encode_header(h);
```

`encode_header()` fills in the three fields the protocol owns: magic, version and
the checksum. Callers can't get them wrong because whatever they put there is
overwritten. The checksum goes last, computed over the first 28 bytes.

## Validating, and why the order matters

A receiver calls `validate_frame()` on every message before touching any field.
It returns either the header or a `FrameError` saying what was wrong
(`std::expected`, since the project builds without exceptions). The checks run in
this order:

1. **At least 32 bytes**, or there's no header to read.
2. **Magic.** Cheap, and catches "this isn't our protocol at all".
3. **Checksum.** If the bytes were damaged, every other field is suspect, so
   check this before believing them.
4. **Version.** An old UI talking to a new daemon.
5. **Opcode.** A C++ `enum class` with `uint8_t` underneath can hold any byte,
   including values that aren't in the list. `is_known_opcode()` rejects those,
   so the code that `switch`es on the opcode later only sees real ones.
6. **Payload limit.** `payload_bytes` comes from the other side. If it says
   4 GB and we believed it, we'd read far past our buffer. Over 64 KiB is rejected.
7. **Payload size matches what arrived.** On a message socket each receive gets
   exactly one frame, so the frame's length must be exactly 32 + `payload_bytes`.

`validate_header()` does steps 1 to 6 and ignores anything after the header.
`validate_frame()` adds step 7.

A test flips each of the 256 bits of a good header one at a time and checks
every single one is rejected. That's the CRC's guarantee in action.

## Fuzzing

Tests check the inputs you thought of. A **fuzzer** checks the ones you didn't.

libFuzzer, built into Clang, calls one function over and over with byte strings:

```cpp
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);
```

It starts with random bytes, watches which branches of your code each input
reaches (Clang instruments the code for this), and keeps mutating the inputs that
reach new branches. Built together with AddressSanitizer and UBSan, any
out-of-bounds read or undefined behavior stops the run with a report and the
input that caused it.

Our target does two things with each input:

- Runs `validate_frame()` on it. If it passes, encodes the header again and checks
  the bytes match. If they don't, `encode_header` and `validate_header` disagree.
- Random bytes almost never have a correct CRC, so the fuzzer would spend all
  its time failing check 3. So it also patches in the right magic and checksum
  and validates that copy, which lets it explore checks 4 to 6.

Run it:

```sh
cmake --preset linux-fuzz
cmake --build --preset linux-fuzz
./build/linux-fuzz/tests/fuzz_ipc_framing -runs=10000000
```

`ctest --preset linux-fuzz` runs a 100,000-input smoke version. The spec's Phase 1
gate wants 10 million runs with no crash; CI's `ipc-fuzz` job does that on every
PR (#17). If it finds a crash, the job's artifacts hold the input that caused
it: download it and run `./tests/fuzz_ipc_framing crash-<hash>` to reproduce.

## Things to try

- Swap the order of `version` and `opcode` in the struct. Which `static_assert`
  fires? Which test fails?
- Add a `uint16_t` field between `magic` and `payload_bytes` and see
  `has_unique_object_representations_v` catch the padding.
- Delete the `payload_bytes > kMaxPayloadBytes` check and run the fuzzer. It
  won't crash (the check only protects code that doesn't exist yet), which is a
  good lesson in what fuzzing can and can't find.

## Read more

- cppreference: `std::bit_cast`, `std::has_unique_object_representations`
- LLVM libFuzzer docs, "Fuzz Target" and "Options" sections
