// Tests for the IPC frame header (src/common/ipc_protocol.hpp).
//
// The struct layout itself is checked at compile time by the static_asserts in
// the header. These tests check the bytes: that encode_header() writes exactly
// the layout in spec 3.2.1, that every known opcode survives a round trip, and
// that validate_frame() rejects each kind of bad frame. The fuzzer in
// fuzz_ipc_framing.cpp then throws random bytes at the same functions.

#include "common/ipc_protocol.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ios>
#include <span>
#include <vector>

namespace {

using am::common::encode_header;
using am::common::FrameError;
using am::common::FrameHeader;
using am::common::FrameHeaderBytes;
using am::common::kFrameHeaderBytes;
using am::common::kMaxPayloadBytes;
using am::common::Opcode;
using am::common::validate_frame;
using am::common::validate_header;

FrameHeader sample_header(Opcode opcode = Opcode::Ping, std::uint32_t payload_bytes = 0) {
    FrameHeader header{};
    header.payload_bytes = payload_bytes;
    header.seq_id = 0x0102030405060708;
    header.timestamp_ns = 0x1122334455667788;
    header.opcode = opcode;
    header.flags = am::common::kFlagAckRequired;
    return header;
}

// A whole frame: the encoded header plus `payload_bytes` zero bytes.
std::vector<std::byte> frame_of(const FrameHeader& header) {
    const FrameHeaderBytes encoded = encode_header(header);
    std::vector<std::byte> frame(encoded.begin(), encoded.end());
    frame.resize(kFrameHeaderBytes + header.payload_bytes);
    return frame;
}

// encode_header() always writes the right magic and version, so to test that
// validate rejects wrong ones we have to edit the bytes ourselves and then fix
// up the checksum, or the CRC check would catch the edit first.
FrameHeaderBytes with_fixed_checksum(FrameHeader header) {
    header.header_crc32c = am::common::header_checksum(std::bit_cast<FrameHeaderBytes>(header));
    return std::bit_cast<FrameHeaderBytes>(header);
}

} // namespace

TEST_CASE("encode_header writes the spec 3.2.1 layout", "[ipc]") {
    const FrameHeaderBytes bytes = encode_header(sample_header(Opcode::Ping, 4));

    // clang-format off
    const std::array<std::uint8_t, 32> expected{
        0x41, 0x4D, 0x50, 0x32,                         // 0x00 magic, "AMP2"
        0x04, 0x00, 0x00, 0x00,                         // 0x04 payload_bytes = 4
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, // 0x08 seq_id
        0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, // 0x10 timestamp_ns
        0x02, 0x00,                                     // 0x18 version 2
        0x03,                                           // 0x1A opcode Ping
        0x01,                                           // 0x1B flags: ACK_REQUIRED
        0xB2, 0x46, 0x1F, 0xB3,                         // 0x1C CRC32C of the 28 bytes above
    };
    // clang-format on

    for (std::size_t i = 0; i < expected.size(); ++i) {
        INFO("byte " << i);
        CHECK(static_cast<std::uint8_t>(bytes[i]) == expected[i]);
    }
}

TEST_CASE("encode_header ignores the caller's magic, version and checksum", "[ipc]") {
    FrameHeader header = sample_header();
    const FrameHeaderBytes clean = encode_header(header);

    header.magic = 0xDEADBEEF;
    header.version = 99;
    header.header_crc32c = 12345;
    CHECK(encode_header(header) == clean);
}

TEST_CASE("every known opcode round-trips and every other byte is rejected", "[ipc]") {
    int known = 0;
    for (int value = 0; value <= 0xFF; ++value) {
        const auto opcode = static_cast<Opcode>(value);
        INFO("opcode 0x" << std::hex << value);

        if (am::common::is_known_opcode(opcode)) {
            ++known;
            const FrameHeader sent = sample_header(opcode);
            const auto received = validate_frame(frame_of(sent));
            REQUIRE(received.has_value());
            CHECK(received->opcode == opcode);
            CHECK(received->seq_id == sent.seq_id);
            CHECK(received->timestamp_ns == sent.timestamp_ns);
            CHECK(received->flags == sent.flags);
            CHECK(received->payload_bytes == 0);
        } else {
            FrameHeader header = sample_header();
            header.opcode = opcode;
            const auto result = validate_header(encode_header(header));
            REQUIRE_FALSE(result.has_value());
            CHECK(result.error() == FrameError::UnknownOpcode);
        }
    }
    CHECK(known == 22); // the number of opcodes in spec 3.2.1
}

TEST_CASE("payload length limits", "[ipc]") {
    SECTION("an empty payload is fine") {
        CHECK(validate_frame(frame_of(sample_header(Opcode::Ping, 0))).has_value());
    }

    SECTION("exactly kMaxPayloadBytes is fine") {
        const auto result =
            validate_frame(frame_of(sample_header(Opcode::LoadTrackReq, kMaxPayloadBytes)));
        REQUIRE(result.has_value());
        CHECK(result->payload_bytes == kMaxPayloadBytes);
    }

    SECTION("one byte over is rejected") {
        const auto bytes = encode_header(sample_header(Opcode::LoadTrackReq, kMaxPayloadBytes + 1));
        const auto result = validate_header(bytes);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == FrameError::PayloadTooLarge);
    }

    SECTION("a huge length is rejected without reading past the buffer") {
        const auto bytes = encode_header(sample_header(Opcode::LoadTrackReq, 0xFFFFFFFF));
        const auto result = validate_frame(bytes);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == FrameError::PayloadTooLarge);
    }
}

TEST_CASE("validate_frame checks the payload length against what arrived", "[ipc]") {
    auto frame = frame_of(sample_header(Opcode::SeekReq, 8));
    CHECK(validate_frame(frame).has_value());

    SECTION("payload cut short") {
        frame.pop_back();
        const auto result = validate_frame(frame);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == FrameError::PayloadSizeMismatch);
    }

    SECTION("extra bytes after the payload") {
        frame.push_back(std::byte{0});
        const auto result = validate_frame(frame);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == FrameError::PayloadSizeMismatch);
    }

    SECTION("validate_header alone doesn't look at the payload") {
        frame.pop_back();
        CHECK(validate_header(frame).has_value());
    }
}

TEST_CASE("fewer than 32 bytes is rejected", "[ipc]") {
    const FrameHeaderBytes bytes = encode_header(sample_header());
    for (std::size_t size = 0; size < kFrameHeaderBytes; ++size) {
        INFO("size " << size);
        const auto result = validate_frame(std::span(bytes).first(size));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == FrameError::TooShort);
    }
}

TEST_CASE("wrong magic is rejected", "[ipc]") {
    FrameHeader header = sample_header();
    header.magic = 0x414D5032; // the byte-swapped magic: "2PMA" on the wire
    header.version = am::common::kProtocolVersion;

    const auto result = validate_header(with_fixed_checksum(header));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == FrameError::BadMagic);
}

TEST_CASE("wrong version is rejected", "[ipc]") {
    FrameHeader header = sample_header();
    header.magic = am::common::kProtocolMagic;
    header.version = 1;

    const auto result = validate_header(with_fixed_checksum(header));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == FrameError::BadVersion);
}

TEST_CASE("any single flipped bit is rejected", "[ipc]") {
    const FrameHeaderBytes clean = encode_header(sample_header());
    REQUIRE(validate_header(clean).has_value());

    for (std::size_t i = 0; i < clean.size(); ++i) {
        for (int bit = 0; bit < 8; ++bit) {
            FrameHeaderBytes damaged = clean;
            damaged[i] ^= static_cast<std::byte>(1 << bit);

            INFO("byte " << i << " bit " << bit);
            const auto result = validate_header(damaged);
            REQUIRE_FALSE(result.has_value());
            // A flip in the magic is caught as bad magic; anywhere else, including
            // the checksum field itself, the checksum no longer matches.
            CHECK(result.error() == (i < 4 ? FrameError::BadMagic : FrameError::BadChecksum));
        }
    }
}

TEST_CASE("validate_header doesn't need aligned input", "[ipc]") {
    // A receive buffer may hand us the header at any address. validate copies
    // it out with memcpy, so an odd offset must work (ASan/UBSan would catch an
    // unaligned load if it didn't).
    const FrameHeaderBytes encoded = encode_header(sample_header());
    std::array<std::byte, kFrameHeaderBytes + 1> buffer{};
    std::memcpy(buffer.data() + 1, encoded.data(), encoded.size());

    const auto result = validate_header(std::span(buffer).subspan(1));
    REQUIRE(result.has_value());
    CHECK(result->seq_id == 0x0102030405060708);
}
