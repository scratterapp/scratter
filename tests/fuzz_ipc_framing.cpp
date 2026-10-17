// libFuzzer target for the IPC frame parser (spec section 6, Phase 1 gate) and
// the message-body decoders in ipc_messages.hpp.
//
// libFuzzer calls LLVMFuzzerTestOneInput() millions of times with byte strings
// it keeps mutating, and keeps the mutations that reach new code. Built with
// ASan and UBSan, any out-of-bounds read, overflow or other undefined behavior
// in the parser stops the run with a report.
//
// Only built with Clang and -DAM_BUILD_FUZZERS=ON (the linux-fuzz preset):
//
//   cmake --preset linux-fuzz
//   cmake --build --preset linux-fuzz
//   ./build/linux-fuzz/tests/fuzz_ipc_framing -runs=10000000
//
// ctest --preset linux-fuzz runs a short smoke version.

#include "common/ipc_messages.hpp"
#include "common/ipc_protocol.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>

namespace {

using namespace am::common;

// A frame that passes validation must encode back to exactly the same header
// bytes. Anything else means encode and validate disagree about the layout.
void check_round_trip(std::span<const std::byte> input, const FrameHeader& header) {
    const FrameHeaderBytes again = encode_header(header);
    if (std::memcmp(again.data(), input.data(), kFrameHeaderBytes) != 0) {
        std::abort();
    }
}

// A body that decodes must encode back to the same bytes, and must pass the
// same checks again.
template <WireBody Body> void check_body(std::span<const std::byte> payload) {
    const auto body = decode_body<Body>(payload);
    if (!body) {
        return;
    }
    const auto again = std::bit_cast<std::array<std::byte, sizeof(Body)>>(*body);
    if (std::memcmp(again.data(), payload.data(), sizeof(Body)) != 0 || !is_valid(*body)) {
        std::abort();
    }
}

// Decodes the payload as the body its opcode carries, the way the daemon and
// client will. Opcodes with no body defined yet are skipped.
void check_payload(Opcode opcode, std::span<const std::byte> payload) {
    switch (opcode) {
    case Opcode::HelloReq:
    case Opcode::HelloAck:
        check_body<HelloBody>(payload);
        if (const auto body = decode_body<HelloBody>(payload)) {
            if (peer_name(*body).size() > kPeerNameBytes) {
                std::abort();
            }
        }
        break;
    case Opcode::SetVolumeReq:
        check_body<SetVolumeBody>(payload);
        break;
    case Opcode::CommandAck:
    case Opcode::CommandNak:
        check_body<CommandResultBody>(payload);
        break;
    case Opcode::PlaybackStateEvent:
        check_body<PlaybackStateBody>(payload);
        break;
    case Opcode::PlayReq:
    case Opcode::PauseReq:
    case Opcode::StopReq:
        if (expect_no_body(payload).has_value() != payload.empty()) {
            std::abort();
        }
        break;
    default:
        break;
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto input = std::as_bytes(std::span(data, size));

    if (const auto header = validate_frame(input)) {
        check_round_trip(input, *header);
        check_payload(header->opcode, input.subspan(kFrameHeaderBytes));
    }

    // Random bytes almost never have a correct CRC, so on its own the fuzzer
    // would rarely get past the checksum test. Also try the input with magic
    // and checksum patched in, so it explores the checks after them too.
    if (size >= kFrameHeaderBytes) {
        FrameHeaderBytes raw;
        std::memcpy(raw.data(), data, kFrameHeaderBytes);
        auto patched = std::bit_cast<FrameHeader>(raw);
        patched.magic = kProtocolMagic;
        patched.header_crc32c = header_checksum(std::bit_cast<FrameHeaderBytes>(patched));
        raw = std::bit_cast<FrameHeaderBytes>(patched);

        if (const auto header = validate_header(raw)) {
            check_round_trip(raw, *header);
        }
    }

    // The same goes for the bodies: a fuzzed frame rarely has a valid header,
    // so also feed the bytes after the first one straight to a decoder, using
    // the first byte as the opcode.
    if (size >= 1) {
        check_payload(static_cast<Opcode>(data[0]), input.subspan(1));
    }
    return 0;
}
