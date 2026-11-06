// Tests for the IPC message bodies (src/common/ipc_messages.hpp).
//
// Sizes and field offsets are checked at compile time in the header. These
// tests check that every body survives encode_message() and decode_body(),
// that the bytes land where spec 3.2.1 says, and that wrong sizes and
// out-of-range values are refused. fuzz_ipc_framing.cpp throws random
// payloads at the same decoders.

#include "common/ipc_messages.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace am::common;

FrameHeader header_for(Opcode opcode) {
    FrameHeader header{};
    header.opcode = opcode;
    header.seq_id = 42;
    return header;
}

// Encodes `body` into a frame, validates the frame like a receiver would, and
// decodes the payload again.
template <WireBody Body>
std::expected<Body, BodyError> round_trip(Opcode opcode, const Body& body) {
    const auto frame = encode_message(header_for(opcode), body);
    const auto header = validate_frame(frame);
    REQUIRE(header.has_value());
    CHECK(header->opcode == opcode);
    CHECK(header->payload_bytes == sizeof(Body));
    return decode_body<Body>(std::span(frame).subspan(kFrameHeaderBytes));
}

// Bodies are compared byte for byte; none of them has an operator==.
template <WireBody Body> bool same_bytes(const Body& a, const Body& b) {
    return std::bit_cast<std::array<std::byte, sizeof(Body)>>(a) ==
           std::bit_cast<std::array<std::byte, sizeof(Body)>>(b);
}

// A TrackChangedBody that only sets the title.
TrackChangedBody titled(std::string_view title) {
    return {
        .catalog_id = 0,
        .duration_micros = 0,
        .source = TrackSource::Tone,
        .reserved = {},
        .title = make_track_title(title),
    };
}

} // namespace

TEST_CASE("HelloBody round-trips for both hello opcodes", "[ipc][messages]") {
    const HelloBody sent{.name = make_peer_name("am-ui")};
    for (const Opcode opcode : {Opcode::HelloReq, Opcode::HelloAck}) {
        const auto received = round_trip(opcode, sent);
        REQUIRE(received.has_value());
        CHECK(peer_name(*received) == "am-ui");
    }
}

TEST_CASE("peer names are zero-padded, and cut at 32 bytes", "[ipc][messages]") {
    const auto short_name = make_peer_name("amctl");
    CHECK(short_name[5] == '\0');
    CHECK(short_name[31] == '\0');

    // A name that fills the whole field has no terminating zero.
    const std::string_view long_name = "a-client-name-that-is-longer-than-32-bytes";
    const HelloBody body{.name = make_peer_name(long_name)};
    CHECK(peer_name(body) == long_name.substr(0, kPeerNameBytes));
    CHECK(peer_name(body).size() == kPeerNameBytes);

    CHECK(peer_name(HelloBody{.name = make_peer_name("")}).empty());
}

TEST_CASE("SetVolumeBody round-trips 0 to 100 and refuses more", "[ipc][messages]") {
    for (int percent = 0; percent <= kMaxVolumePercent; ++percent) {
        const auto received =
            round_trip(Opcode::SetVolumeReq,
                       SetVolumeBody{.volume_percent = static_cast<std::uint8_t>(percent)});
        REQUIRE(received.has_value());
        CHECK(received->volume_percent == percent);
    }

    const std::array payload{std::byte{101}};
    const auto too_loud = decode_body<SetVolumeBody>(payload);
    REQUIRE_FALSE(too_loud.has_value());
    CHECK(too_loud.error() == BodyError::BadValue);
}

TEST_CASE("CommandResultBody round-trips and lands at the spec offsets", "[ipc][messages]") {
    const CommandResultBody sent{
        .request_seq_id = 0x0102030405060708,
        .request_opcode = Opcode::SetVolumeReq,
        .status = CommandStatus::NotSupported,
        .reserved = {},
    };
    const auto received = round_trip(Opcode::CommandNak, sent);
    REQUIRE(received.has_value());
    CHECK(same_bytes(*received, sent));

    const auto bytes = std::bit_cast<std::array<std::byte, 16>>(sent);
    CHECK(bytes[0] == std::byte{0x08}); // seq_id, little-endian
    CHECK(bytes[7] == std::byte{0x01});
    CHECK(bytes[8] == std::byte{0x15}); // SetVolumeReq
    CHECK(bytes[9] == std::byte{0x02}); // NotSupported
}

TEST_CASE("CommandResultBody refuses an unknown status", "[ipc][messages]") {
    std::array<std::byte, sizeof(CommandResultBody)> payload{};
    for (int status = 0; status <= 0xFF; ++status) {
        payload[9] = static_cast<std::byte>(status);
        const bool known = status <= static_cast<int>(CommandStatus::Failed);
        CHECK(decode_body<CommandResultBody>(payload).has_value() == known);
    }
}

TEST_CASE("PlaybackStateBody round-trips every state", "[ipc][messages]") {
    for (const PlaybackState state : {PlaybackState::Stopped, PlaybackState::Playing,
                                      PlaybackState::Paused, PlaybackState::Buffering}) {
        const PlaybackStateBody sent{
            .position_micros = 83'500'000,
            .state = state,
            .volume_percent = 70,
            .reserved = {},
        };
        const auto received = round_trip(Opcode::PlaybackStateEvent, sent);
        REQUIRE(received.has_value());
        CHECK(same_bytes(*received, sent));
    }
}

TEST_CASE("PlaybackStateBody refuses an unknown state or volume over 100", "[ipc][messages]") {
    std::array<std::byte, sizeof(PlaybackStateBody)> payload{};
    payload[8] = std::byte{4}; // one past Buffering
    CHECK(decode_body<PlaybackStateBody>(payload).error() == BodyError::BadValue);

    payload[8] = std::byte{1}; // Playing
    payload[9] = std::byte{101};
    CHECK(decode_body<PlaybackStateBody>(payload).error() == BodyError::BadValue);

    payload[9] = std::byte{100};
    CHECK(decode_body<PlaybackStateBody>(payload).has_value());
}

TEST_CASE("LoadTrackBody round-trips every source", "[ipc][messages]") {
    for (const TrackSource source :
         {TrackSource::Tone, TrackSource::ClearFilm, TrackSource::WidevineFilm}) {
        const LoadTrackBody sent{.catalog_id = 0, .source = source, .reserved = {}};
        const auto received = round_trip(Opcode::LoadTrackReq, sent);
        REQUIRE(received.has_value());
        CHECK(same_bytes(*received, sent));
    }

    const auto bytes = std::bit_cast<std::array<std::byte, 16>>(
        LoadTrackBody{.catalog_id = 0, .source = TrackSource::WidevineFilm, .reserved = {}});
    CHECK(bytes[8] == std::byte{0x02});
}

TEST_CASE("LoadTrackBody refuses an unknown source or a catalog id", "[ipc][messages]") {
    std::array<std::byte, sizeof(LoadTrackBody)> payload{};
    for (int source = 0; source <= 0xFF; ++source) {
        payload[8] = static_cast<std::byte>(source);
        const bool known = source <= static_cast<int>(TrackSource::WidevineFilm);
        CHECK(decode_body<LoadTrackBody>(payload).has_value() == known);
    }

    // A catalog id means Apple Music, which this build can't play yet.
    payload[8] = std::byte{0}; // Tone
    payload[0] = std::byte{1};
    CHECK(decode_body<LoadTrackBody>(payload).error() == BodyError::BadValue);
}

TEST_CASE("TrackChangedBody round-trips and lands at the spec offsets", "[ipc][messages]") {
    const TrackChangedBody sent{
        .catalog_id = 0,
        .duration_micros = 0x0102030405060708,
        .source = TrackSource::ClearFilm,
        .reserved = {},
        .title = make_track_title("Sintel"),
    };
    const auto received = round_trip(Opcode::TrackChangedEvent, sent);
    REQUIRE(received.has_value());
    CHECK(same_bytes(*received, sent));
    CHECK(track_title(*received) == "Sintel");

    const auto bytes = std::bit_cast<std::array<std::byte, 88>>(sent);
    CHECK(bytes[8] == std::byte{0x08}); // duration, little-endian
    CHECK(bytes[15] == std::byte{0x01});
    CHECK(bytes[16] == std::byte{0x01}); // ClearFilm
    CHECK(bytes[24] == std::byte{'S'});

    std::array<std::byte, sizeof(TrackChangedBody)> payload{};
    payload[16] = std::byte{3}; // one past WidevineFilm
    CHECK(decode_body<TrackChangedBody>(payload).error() == BodyError::BadValue);
}

TEST_CASE("track titles are zero-padded, and cut whole characters at 64 bytes", "[ipc][messages]") {
    const auto short_title = make_track_title("Tone");
    CHECK(short_title[4] == '\0');
    CHECK(short_title[63] == '\0');

    // Exactly 64 bytes fits, with no terminating zero.
    const std::string exact(kTrackTitleBytes, 'a');
    const TrackChangedBody full = titled(exact);
    CHECK(track_title(full) == exact);

    // 63 ASCII bytes, then a 2-byte character: only its first byte would fit,
    // so the cut drops the whole character.
    const std::string split = std::string(63, 'a') + "\xC3\xA9"; // e with an acute accent
    const TrackChangedBody cut = titled(split);
    CHECK(track_title(cut) == std::string(63, 'a'));

    // A 4-byte character starting at byte 62: the cut backs up to byte 62.
    const std::string note = std::string(62, 'a') + "\xF0\x9F\x8E\xB5"; // a musical note emoji
    const TrackChangedBody backed = titled(note);
    CHECK(track_title(backed) == std::string(62, 'a'));

    CHECK(track_title(titled("")).empty());
}

TEST_CASE("every body refuses a payload one byte short or long", "[ipc][messages]") {
    const std::vector<std::byte> bytes(128); // more than the biggest body plus one
    const auto check_sizes = [&]<WireBody Body>(Body) {
        INFO("body size " << sizeof(Body));
        CHECK(decode_body<Body>(std::span(bytes).first(sizeof(Body))).has_value());
        CHECK(decode_body<Body>(std::span(bytes).first(sizeof(Body) - 1)).error() ==
              BodyError::WrongSize);
        CHECK(decode_body<Body>(std::span(bytes).first(sizeof(Body) + 1)).error() ==
              BodyError::WrongSize);
        CHECK(decode_body<Body>({}).error() == BodyError::WrongSize);
    };
    check_sizes(HelloBody{});
    check_sizes(SetVolumeBody{});
    check_sizes(CommandResultBody{});
    check_sizes(PlaybackStateBody{});
    check_sizes(LoadTrackBody{});
    check_sizes(TrackChangedBody{});
}

TEST_CASE("Play, Pause and Stop are header-only frames", "[ipc][messages]") {
    for (const Opcode opcode : {Opcode::PlayReq, Opcode::PauseReq, Opcode::StopReq}) {
        const auto frame = encode_header(header_for(opcode));
        const auto header = validate_frame(frame);
        REQUIRE(header.has_value());
        CHECK(header->payload_bytes == 0);
        CHECK(expect_no_body(std::span(frame).subspan(kFrameHeaderBytes)).has_value());
    }

    const std::array stray{std::byte{0}};
    CHECK(expect_no_body(stray).error() == BodyError::WrongSize);
}
