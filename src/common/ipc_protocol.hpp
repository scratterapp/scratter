#pragma once

// The control-plane wire format (spec section 3.2.1).
//
// Every message between am-ui and scratterd is one frame: a fixed 32-byte
// FrameHeader followed by `payload_bytes` bytes of payload (0 to 64 KiB).
// What goes in each opcode's payload is defined in ipc_messages.hpp.
//
//   Sender:                                Receiver:
//     FrameHeader h{};                       auto header = validate_frame(bytes);
//     h.opcode = Opcode::Ping;               if (!header) { drop it; }
//     h.seq_id = next_id++;                  switch (header->opcode) { ... }
//     auto bytes = encode_header(h);
//
// encode_header() fills in the three fields the protocol owns (magic, version
// and the checksum), so callers only set the rest. validate_frame() checks
// everything about a received frame before any field in it is trusted.
//
// The header goes on the wire as its raw in-memory bytes. That only works
// because the struct has no padding (checked below) and both platforms are
// little-endian (also checked below), so the bytes mean the same thing on
// either end. Bytes and struct are converted with std::bit_cast, never by
// casting a byte pointer to FrameHeader*.

#include "common/crc32c.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <type_traits>

namespace am::common {

// Multi-byte fields are sent in the CPU's own byte order. x86-64 and ARM64 are
// both little-endian, so that's the wire order too. A big-endian port would need
// byte swaps in encode_header() and validate_header().
static_assert(std::endian::native == std::endian::little,
              "the wire format assumes a little-endian CPU");

// Stored little-endian, this is the bytes 41 4D 50 32: "AMP2" in a hex dump.
inline constexpr std::uint32_t kProtocolMagic = 0x32504D41;
inline constexpr std::uint16_t kProtocolVersion = 0x0002;

inline constexpr std::size_t kFrameHeaderBytes = 32;
inline constexpr std::uint32_t kMaxPayloadBytes = 65536;
inline constexpr std::size_t kMaxFrameBytes = kFrameHeaderBytes + kMaxPayloadBytes;

// Bits in FrameHeader::flags. The other six bits are reserved and sent as zero.
inline constexpr std::uint8_t kFlagAckRequired = 1 << 0;
inline constexpr std::uint8_t kFlagError = 1 << 1;

enum class Opcode : std::uint8_t {
    // Lifecycle and handshake (0x01 - 0x0F)
    HelloReq = 0x01,
    HelloAck = 0x02,
    Ping = 0x03,
    Pong = 0x04,
    AuthEscalationReq = 0x05,
    AuthStatusEvent = 0x06,

    // Playback transport control (0x10 - 0x1F)
    LoadTrackReq = 0x10,
    PlayReq = 0x11,
    PauseReq = 0x12,
    StopReq = 0x13,
    SeekReq = 0x14,
    SetVolumeReq = 0x15,
    SetAudioModeReq = 0x16,

    // Queue mutation (0x20 - 0x2F)
    QueueReplaceReq = 0x20,
    QueueAppendReq = 0x21,
    QueueRemoveReq = 0x22,
    QueueShuffleReq = 0x23,

    // Asynchronous daemon events (0x80 - 0xFF)
    CommandAck = 0x80,
    CommandNak = 0x81,
    TrackChangedEvent = 0x82,
    EmeErrorEvent = 0x83,
    PlaybackStateEvent = 0x84,
};

// An Opcode can hold any byte, including ones that aren't listed above (a byte
// off the wire, or a newer peer). True only for the listed ones.
[[nodiscard]] constexpr bool is_known_opcode(Opcode opcode) noexcept {
    switch (opcode) {
    case Opcode::HelloReq:
    case Opcode::HelloAck:
    case Opcode::Ping:
    case Opcode::Pong:
    case Opcode::AuthEscalationReq:
    case Opcode::AuthStatusEvent:
    case Opcode::LoadTrackReq:
    case Opcode::PlayReq:
    case Opcode::PauseReq:
    case Opcode::StopReq:
    case Opcode::SeekReq:
    case Opcode::SetVolumeReq:
    case Opcode::SetAudioModeReq:
    case Opcode::QueueReplaceReq:
    case Opcode::QueueAppendReq:
    case Opcode::QueueRemoveReq:
    case Opcode::QueueShuffleReq:
    case Opcode::CommandAck:
    case Opcode::CommandNak:
    case Opcode::TrackChangedEvent:
    case Opcode::EmeErrorEvent:
    case Opcode::PlaybackStateEvent:
        return true;
    }
    return false;
}

// Every field starts at a multiple of its own size, so the compiler has no
// reason to add padding. No #pragma pack: packed structs cause unaligned
// loads, which are slow on some CPUs and undefined behavior in C++.
struct alignas(8) FrameHeader {
    std::uint32_t magic;         // 0x00: kProtocolMagic
    std::uint32_t payload_bytes; // 0x04: length of the payload after the header, 0..65536
    std::uint64_t seq_id;        // 0x08: request/correlation ID, increases per sender
    std::uint64_t timestamp_ns;  // 0x10: sender's monotonic clock, nanoseconds
    std::uint16_t version;       // 0x18: kProtocolVersion
    Opcode opcode;               // 0x1A: what this frame is
    std::uint8_t flags;          // 0x1B: kFlagAckRequired, kFlagError
    std::uint32_t header_crc32c; // 0x1C: CRC32C of bytes 0x00..0x1B
};

static_assert(sizeof(FrameHeader) == kFrameHeaderBytes, "FrameHeader must be 32 bytes");
static_assert(alignof(FrameHeader) == 8, "FrameHeader must be 8-byte aligned");
static_assert(std::is_trivially_copyable_v<FrameHeader>, "FrameHeader must be trivially copyable");
// True only when every bit of the struct belongs to a field, i.e. no padding.
// Padding bytes would hold garbage that ends up on the wire and in the CRC.
static_assert(std::has_unique_object_representations_v<FrameHeader>,
              "FrameHeader must have no padding");
static_assert(offsetof(FrameHeader, header_crc32c) == 0x1C, "the CRC must be the last 4 bytes");

// The header as it appears on the wire.
using FrameHeaderBytes = std::array<std::byte, kFrameHeaderBytes>;

// Why a received frame was rejected, in the order validate_frame() checks.
enum class FrameError : std::uint8_t {
    TooShort,            // fewer than 32 bytes
    BadMagic,            // not one of our frames at all
    BadChecksum,         // header bytes were damaged or half-written
    BadVersion,          // a peer speaking a different protocol version
    UnknownOpcode,       // an opcode this build doesn't know
    PayloadTooLarge,     // payload_bytes over kMaxPayloadBytes
    PayloadSizeMismatch, // payload_bytes doesn't match the bytes that arrived
};

// CRC32C over everything before the checksum field (bytes 0x00..0x1B).
[[nodiscard]] inline std::uint32_t header_checksum(const FrameHeaderBytes& bytes) noexcept {
    return crc32c(std::span(bytes).first<offsetof(FrameHeader, header_crc32c)>());
}

// Turns `header` into wire bytes. Sets magic, version and the checksum; any
// values the caller put in those three fields are ignored.
[[nodiscard]] inline FrameHeaderBytes encode_header(FrameHeader header) noexcept {
    header.magic = kProtocolMagic;
    header.version = kProtocolVersion;
    header.header_crc32c = 0;
    header.header_crc32c = header_checksum(std::bit_cast<FrameHeaderBytes>(header));
    return std::bit_cast<FrameHeaderBytes>(header);
}

// Checks the first 32 bytes of `bytes` and returns them as a FrameHeader.
// Bytes after the header are ignored. The checksum is checked right after the
// magic, so the later checks only ever look at undamaged fields.
[[nodiscard]] inline std::expected<FrameHeader, FrameError>
validate_header(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() < kFrameHeaderBytes) {
        return std::unexpected(FrameError::TooShort);
    }

    FrameHeaderBytes raw;
    std::memcpy(raw.data(), bytes.data(), kFrameHeaderBytes);
    const auto header = std::bit_cast<FrameHeader>(raw);

    if (header.magic != kProtocolMagic) {
        return std::unexpected(FrameError::BadMagic);
    }
    if (header.header_crc32c != header_checksum(raw)) {
        return std::unexpected(FrameError::BadChecksum);
    }
    if (header.version != kProtocolVersion) {
        return std::unexpected(FrameError::BadVersion);
    }
    if (!is_known_opcode(header.opcode)) {
        return std::unexpected(FrameError::UnknownOpcode);
    }
    if (header.payload_bytes > kMaxPayloadBytes) {
        return std::unexpected(FrameError::PayloadTooLarge);
    }
    return header;
}

// Checks one whole received frame: a valid header followed by exactly
// `payload_bytes` of payload. This is what a receiver calls on each message
// its IpcChannel hands it. The payload is bytes.subspan(kFrameHeaderBytes).
[[nodiscard]] inline std::expected<FrameHeader, FrameError>
validate_frame(std::span<const std::byte> bytes) noexcept {
    auto header = validate_header(bytes);
    if (header && bytes.size() != kFrameHeaderBytes + header->payload_bytes) {
        return std::unexpected(FrameError::PayloadSizeMismatch);
    }
    return header;
}

} // namespace am::common
