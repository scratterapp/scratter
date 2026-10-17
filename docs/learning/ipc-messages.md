# IPC message bodies, explained

This covers `src/common/ipc_messages.hpp` and its tests in
`tests/test_ipc_messages.cpp` (issue #48). Read [ipc-frame.md](ipc-frame.md)
first: a body is what follows the 32-byte header.

## The problem it solves

The header says *what* a message is (`SetVolumeReq`) and *how long* its payload
is. It doesn't say what the payload bytes mean. "Set volume" needs a number, and
both programs have to agree on where that number sits, how big it is, and what
range it has. That agreement, per opcode, is the message body.

## The bodies so far

| Opcode | Body | Bytes | What's in it |
| --- | --- | --- | --- |
| `HelloReq`, `HelloAck` | `HelloBody` | 32 | the sender's name |
| `PlayReq`, `PauseReq`, `StopReq` | none | 0 | nothing, the opcode says it all |
| `SetVolumeReq` | `SetVolumeBody` | 1 | volume, 0 to 100 |
| `CommandAck`, `CommandNak` | `CommandResultBody` | 16 | which request, and ok or why not |
| `PlaybackStateEvent` | `PlaybackStateBody` | 16 | playing/paused/stopped, position, volume |

`PlaybackStateEvent` is a new opcode (0x84). The spec's list had no way for the
daemon to say "I'm paused now", so a client would have had to poll for it.

### A conversation

```
am-ui                                   daemon
HelloReq   seq 1  name "am-ui"    -->
                                  <--   HelloAck            name "scratter"
SetVolumeReq seq 2  volume 150    -->
                                  <--   CommandNak          request 2, BadRequest
PauseReq   seq 3  (no body)       -->
                                  <--   CommandAck          request 3, Ok
                                  <--   PlaybackStateEvent  Paused, 83.5 s, volume 70
```

`CommandAck` and `CommandNak` carry the request's `seq_id`, which is how the
client matches an answer to the request it sent. That's the "correlation ID" job
the header's `seq_id` field was made for.

## How a body goes on the wire

Exactly like the header: the struct's memory *is* the wire format. That only
works if the struct has no padding and both ends are little-endian, so every
body passes the `WireBody` concept:

```cpp
template <typename T>
concept WireBody = std::is_trivially_copyable_v<T> && std::has_unique_object_representations_v<T>;
```

`has_unique_object_representations` is false if any byte of the struct isn't
part of a field (padding). It's also false for `float`, because two different
bit patterns can both mean "not a number". That's one reason volume is a whole
number of percent instead of a `float` from 0.0 to 1.0.

The 16-byte bodies end with `reserved` bytes. `CommandResultBody` is a `uint64_t`
(8 bytes) plus two 1-byte fields. The compiler would round that up to 16 so the
`uint64_t` stays aligned in an array of them, and fill the gap with padding. By
naming those 6 bytes ourselves, every byte has a defined value (zero).

## Sending

```cpp
FrameHeader h{};
h.opcode = Opcode::SetVolumeReq;
h.seq_id = next_id++;
auto bytes = encode_message(h, SetVolumeBody{.volume_percent = 50});
channel.send(bytes);
```

`encode_message()` sets `payload_bytes` for you and returns one array holding
header then body, because a message socket sends one buffer as one message.
Play, Pause and Stop have no body, so they're just `encode_header(h)`.

## Receiving

```cpp
auto header = validate_frame(frame);           // the header first, always
auto payload = frame.subspan(kFrameHeaderBytes);
switch (header->opcode) {
case Opcode::SetVolumeReq:
    if (auto body = decode_body<SetVolumeBody>(payload)) { set_volume(body->volume_percent); }
    else { /* reply CommandNak, BadRequest */ }
    break;
case Opcode::PauseReq:
    if (expect_no_body(payload)) { pause(); }
    break;
...
}
```

`decode_body<T>()` does two checks, in this order:

1. **Size.** The payload must be exactly `sizeof(T)` bytes. Not "at least": a
   payload with extra bytes means the sender has a different idea of the format,
   and guessing which bytes it meant is how parsers get exploited.
2. **Values.** Then `is_valid()` for that body. An `enum class` field can hold any
   byte off the wire, so a `PlaybackState` of 7 is rejected, the same way
   `validate_header()` rejects an unknown opcode. Volume over 100 is rejected
   too.

The bytes are copied out with `memcpy` and `std::bit_cast`, never by casting the
payload pointer to `SetVolumeBody*`. The payload starts 32 bytes into a buffer
nobody promised was aligned, and reading through a cast pointer is undefined
behavior anyway.

## Why Hello has no version number

The issue asked for one. But every frame's header already has a `version`, and
`validate_frame()` throws away any frame with the wrong one before its body is
read. A second version inside the Hello body could never disagree with the
header and still arrive, so it would be dead weight. The name is what's left,
and it's for log lines ("am-ui connected", "amctl connected").

## Versioning: how a format changes later

This is the part that matters once people run different builds of the UI and
daemon. A few rules, all of which this file follows:

- **Fixed sizes, checked at compile time.** Each body has a `static_assert` on its
  size and field offsets. If someone adds a field by accident, the build fails
  instead of the wire format quietly changing.
- **Exact length on receive.** A body that grew is rejected, not half-read.
- **Reserved bytes are sent as zero.** A later version could give them a meaning
  without changing the size. Receivers don't check them, so a future sender that
  uses them doesn't trip an old receiver's checks. The header's `version` field
  is still what tells the two apart.
- **One version for the whole protocol.** `kProtocolVersion` lives in the header.
  Until the first release, it stays at 2 while bodies are being added, because no
  two different builds are out there to disagree. After the first release, any
  change to an existing body's layout or meaning bumps it. Adding a brand-new
  opcode doesn't strictly need a bump (an old peer rejects the unknown opcode
  cleanly), but bump anyway if the newer side depends on getting an answer.

Real protocols handle this in fancier ways: Protocol Buffers tag every field so
old readers skip fields they don't know, and Cap'n Proto lays structs out so
they can grow at the end. We don't need that yet. Two programs from the same
repo, built together, can agree on fixed structs and a version number.

## Fuzzing the bodies

`tests/fuzz_ipc_framing.cpp` now also decodes payloads. For a frame that passes
`validate_frame()`, it decodes the payload as that opcode's body. Since random
bytes rarely make a valid header, it also treats the first input byte as an
opcode and the rest as the payload, so the decoders see millions of inputs. Any
body that decodes must encode back to the same bytes and pass `is_valid()`
again.

## Things to try

- Change `SetVolumeBody::volume_percent` to a `float`. Which `static_assert`
  fires, and why?
- Remove `reserved` from `PlaybackStateBody`. What does the compiler do with
  those 6 bytes, and which check catches it?
- Add `Seeking = 4` to `PlaybackState` without touching `is_valid()`. GCC and
  Clang stop the build with a `-Wswitch` warning, because the `switch` lists every
  value instead of checking `state <= Buffering`. A range check would have
  compiled and quietly rejected the new state.

## Read more

- cppreference: `std::has_unique_object_representations`, designated initializers
- "Protocol Buffers: Updating a message type" (the rules a tagged format follows)
