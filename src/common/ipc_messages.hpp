#pragma once

// What goes inside an IPC frame's payload (spec section 3.2.1, "Message bodies").
//
// ipc_protocol.hpp defines the 32-byte header. This file defines the bodies
// that follow it, for the messages the daemon and a client need first:
//
//   Opcode               Body                 Sent by
//   HelloReq             HelloBody            client, first thing after connecting
//   HelloAck             HelloBody            daemon, in reply
//   Ping                 (none)               client, to check the daemon is alive
//   Pong                 (none)               daemon, in reply; its header's seq_id
//                                             is the Ping's, not a new one
//   PlayReq              (none)               client
//   PauseReq             (none)               client
//   StopReq              (none)               client
//   SetVolumeReq         SetVolumeBody        client
//   SeekReq              SeekBody             client, to jump to a point in the track
//   CommandAck           CommandResultBody    daemon, when a request worked
//   CommandNak           CommandResultBody    daemon, when it didn't
//   PlaybackStateEvent   PlaybackStateBody    daemon, whenever play state or volume changes
//   LoadTrackReq         LoadTrackBody        client, to switch what plays
//   TrackChangedEvent    TrackChangedBody     daemon, after each switch and right after
//                                             HelloAck, so a new client knows what plays
//   RaiseWindowEvent     (none)               daemon, to every connected window, when the
//                                             desktop asks to raise the player (MPRIS Raise)
//
// "(none)" means payload_bytes is 0 and the frame is just the header. That is
// still a 32-byte message, so the IPC channel's "no empty messages" rule
// doesn't get in the way.
//
// Sending and receiving:
//
//   Sender:                                   Receiver, after validate_frame():
//     FrameHeader h{};                          auto payload = frame.subspan(kFrameHeaderBytes);
//     h.opcode = Opcode::SetVolumeReq;          auto body = decode_body<SetVolumeBody>(payload);
//     h.seq_id = next_id++;                     if (!body) { send a CommandNak; }
//     auto bytes = encode_message(h, SetVolumeBody{.volume_percent = 50});
//
// Every body is a plain fixed-size struct sent as its raw bytes, the same way
// as the header and for the same reasons (see ipc_protocol.hpp): no padding,
// little-endian on both platforms, converted with std::bit_cast and memcpy.
//
// Other opcodes (the queue messages...) get bodies when
// the code that needs them is written.

#include "common/ipc_protocol.hpp"
#include "common/telemetry_shm.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <string_view>
#include <type_traits>

namespace am::common {

// Why a payload was rejected.
enum class BodyError : std::uint8_t {
    WrongSize, // payload isn't exactly the size of the body this opcode carries
    BadValue,  // right size, but a field holds a value this build doesn't accept
};

// ---- The bodies --------------------------------------------------------------

inline constexpr std::size_t kPeerNameBytes = 32;

// The name am-ui says in its HelloReq. It's how the daemon tells a window
// from amctl or a test: RaiseWindowEvent goes only to clients that gave this
// name, and if none did, the daemon starts a window instead.
inline constexpr std::string_view kWindowPeerName = "am-ui";

// HelloReq and HelloAck. Each side says who it is, mostly for log lines
// ("am-ui connected"). There is no version number in here: the frame header
// already carries one, and validate_frame() drops any frame whose version
// doesn't match before its body is ever read.
struct HelloBody {
    // UTF-8, padded with zero bytes. A name that fills all 32 bytes has no
    // terminating zero, so read it with peer_name(), never as a C string.
    std::array<char, kPeerNameBytes> name;
};

// SetVolumeReq. The daemon's output volume, 0 (silent) to 100 (full).
struct SetVolumeBody {
    std::uint8_t volume_percent;
};

inline constexpr std::uint8_t kMaxVolumePercent = 100;

// SeekReq. Jump to `position_micros` from the start of the track. Always an
// absolute position: a relative jump ("+10 s") is worked out by the sender
// from the position it last heard, the way MPRIS's Seek(offset) already is.
// A position past the end is clamped to the end by the daemon. The tone has
// no length, so a seek on it gets CommandNak / NotSupported.
struct SeekBody {
    std::uint64_t position_micros;
};

// The biggest position a SeekBody may carry. The daemon keeps positions in
// std::chrono::microseconds, which is signed 64-bit, so anything above this
// would turn negative on the way in.
inline constexpr std::uint64_t kMaxSeekMicros = 0x7FFF'FFFF'FFFF'FFFF;

// The answer to a request, carried by CommandAck (status Ok) and CommandNak
// (any other status). request_seq_id is the seq_id from the request's header,
// which is how the client knows which request this answers.
enum class CommandStatus : std::uint8_t {
    Ok = 0,
    BadRequest = 1,   // the request's body didn't decode (wrong size or value)
    NotSupported = 2, // a known opcode this daemon can't handle yet
    Failed = 3,       // understood, but doing it went wrong
};

struct CommandResultBody {
    std::uint64_t request_seq_id;
    Opcode request_opcode;
    CommandStatus status;
    std::array<std::uint8_t, 6> reserved; // sent as zero; makes the size 16 with no padding
};

// PlaybackStateEvent. Sent when the play state or volume changes, so a client
// doesn't have to poll. The smoothly moving position is in the telemetry page;
// the one here is just where playback was when the event was sent.
struct PlaybackStateBody {
    std::uint64_t position_micros;
    PlaybackState state; // shared with the telemetry page (telemetry_shm.hpp)
    std::uint8_t volume_percent;
    std::array<std::uint8_t, 6> reserved; // sent as zero; makes the size 16 with no padding
};

// Where the audio comes from. Until the MusicKit key arrives (#4) these are
// the daemon's own test sources; Apple Music tracks will be a fourth kind,
// Catalog, that uses the catalog id. A daemon built without CEF can only play
// the tone and refuses the films with CommandNak / NotSupported.
enum class TrackSource : std::uint8_t {
    Tone = 0,         // the daemon's 440 Hz test tone
    ClearFilm = 1,    // Google's unencrypted test film, through CEF
    WidevineFilm = 2, // Google's Widevine test film, through CEF
};

// LoadTrackReq. Stops whatever plays and switches to `source`. The play state
// carries over: if it was playing, the new source plays.
struct LoadTrackBody {
    std::uint64_t catalog_id; // Apple Music catalog id; 0 for the test sources
    TrackSource source;
    std::array<std::uint8_t, 7> reserved; // sent as zero; makes the size 16 with no padding
};

// Room for what the desktop and the window show about a track. Apple Music
// titles run long (a classical track can name the work, the movement and the
// performers), so 256 bytes each; artwork links are around 150 bytes today.
inline constexpr std::size_t kTrackTitleBytes = 256;
inline constexpr std::size_t kTrackArtistBytes = 256;
inline constexpr std::size_t kTrackAlbumBytes = 256;
inline constexpr std::size_t kTrackArtworkUrlBytes = 512;

// TrackChangedEvent. What plays now: sent after each LoadTrackReq that worked,
// and right after HelloAck, so a client that just connected knows too. The
// same things the daemon hands the desktop's media controls (TrackInfo in
// src/pal/interfaces/media_controls.hpp).
//
// The four text fields are UTF-8, padded with zero bytes. Like HelloBody::name
// they may be completely full with no terminating zero, so read them with
// track_title() and the others below, never as C strings. Fill them with
// make_text_field() (cut at a character boundary) and make_url_field() (a link
// that doesn't fit is left out, since a cut one points at nothing). Any of
// them can be empty: the receiver leaves that line blank.
struct TrackChangedBody {
    std::uint64_t catalog_id;      // as in LoadTrackBody
    std::uint64_t duration_micros; // 0 when it has no end (the tone) or isn't known yet
    TrackSource source;
    std::array<std::uint8_t, 7> reserved; // sent as zero; keeps the text 8-byte aligned
    std::array<char, kTrackTitleBytes> title;
    std::array<char, kTrackArtistBytes> artist;
    std::array<char, kTrackAlbumBytes> album;
    std::array<char, kTrackArtworkUrlBytes> artwork_url; // https:// link to the cover art
};

// What every body has to be for raw-byte sending to be safe: copyable as
// plain bytes, and every byte belonging to a field (no padding, whose bytes
// would be garbage on the wire). Floats fail the second test, which is why
// volume is a whole-number percent.
template <typename T>
concept WireBody = std::is_trivially_copyable_v<T> && std::has_unique_object_representations_v<T>;

static_assert(WireBody<HelloBody> && sizeof(HelloBody) == 32);
static_assert(WireBody<SetVolumeBody> && sizeof(SetVolumeBody) == 1);
static_assert(WireBody<SeekBody> && sizeof(SeekBody) == 8);
static_assert(WireBody<CommandResultBody> && sizeof(CommandResultBody) == 16);
static_assert(offsetof(CommandResultBody, request_opcode) == 8);
static_assert(offsetof(CommandResultBody, status) == 9);
static_assert(WireBody<PlaybackStateBody> && sizeof(PlaybackStateBody) == 16);
static_assert(offsetof(PlaybackStateBody, state) == 8);
static_assert(offsetof(PlaybackStateBody, volume_percent) == 9);
static_assert(WireBody<LoadTrackBody> && sizeof(LoadTrackBody) == 16);
static_assert(offsetof(LoadTrackBody, source) == 8);
static_assert(WireBody<TrackChangedBody> && sizeof(TrackChangedBody) == 1304);
static_assert(offsetof(TrackChangedBody, duration_micros) == 8);
static_assert(offsetof(TrackChangedBody, source) == 16);
static_assert(offsetof(TrackChangedBody, title) == 24);
static_assert(offsetof(TrackChangedBody, artist) == 280);
static_assert(offsetof(TrackChangedBody, album) == 536);
static_assert(offsetof(TrackChangedBody, artwork_url) == 792);

// ---- Field checks ------------------------------------------------------------
//
// decode_body() calls the matching is_valid() after the size check. Enum
// fields can hold any byte off the wire, so each one is checked against the
// values this build knows, the same way validate_header() checks the opcode.

[[nodiscard]] constexpr bool is_valid(const HelloBody&) noexcept { return true; }

[[nodiscard]] constexpr bool is_valid(const SetVolumeBody& body) noexcept {
    return body.volume_percent <= kMaxVolumePercent;
}

[[nodiscard]] constexpr bool is_valid(const SeekBody& body) noexcept {
    return body.position_micros <= kMaxSeekMicros;
}

[[nodiscard]] constexpr bool is_valid(const CommandResultBody& body) noexcept {
    switch (body.status) {
    case CommandStatus::Ok:
    case CommandStatus::BadRequest:
    case CommandStatus::NotSupported:
    case CommandStatus::Failed:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool is_valid(const PlaybackStateBody& body) noexcept {
    if (body.volume_percent > kMaxVolumePercent) {
        return false;
    }
    switch (body.state) {
    case PlaybackState::Stopped:
    case PlaybackState::Playing:
    case PlaybackState::Paused:
    case PlaybackState::Buffering:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool is_known(TrackSource source) noexcept {
    switch (source) {
    case TrackSource::Tone:
    case TrackSource::ClearFilm:
    case TrackSource::WidevineFilm:
        return true;
    }
    return false;
}

// The test sources have no catalog id, so a nonzero one means the sender
// meant something this build doesn't know.
[[nodiscard]] constexpr bool is_valid(const LoadTrackBody& body) noexcept {
    return is_known(body.source) && body.catalog_id == 0;
}

[[nodiscard]] constexpr bool is_valid(const TrackChangedBody& body) noexcept {
    return is_known(body.source) && body.catalog_id == 0;
}

// ---- Encode and decode -------------------------------------------------------

// A whole frame (header then body) as one block of bytes, ready for
// IpcChannel::send(). Sets header.payload_bytes to the body's size.
template <WireBody Body>
[[nodiscard]] std::array<std::byte, kFrameHeaderBytes + sizeof(Body)>
encode_message(FrameHeader header, const Body& body) noexcept {
    header.payload_bytes = sizeof(Body);
    std::array<std::byte, kFrameHeaderBytes + sizeof(Body)> frame{};
    const FrameHeaderBytes head = encode_header(header);
    const auto tail = std::bit_cast<std::array<std::byte, sizeof(Body)>>(body);
    std::ranges::copy(head, frame.begin());
    std::ranges::copy(tail, frame.begin() + kFrameHeaderBytes);
    return frame;
}

// Reads a body out of a received payload (the bytes after the header).
// Refuses a payload that isn't exactly sizeof(Body) bytes, then checks the
// fields with is_valid().
template <WireBody Body>
[[nodiscard]] std::expected<Body, BodyError>
decode_body(std::span<const std::byte> payload) noexcept {
    if (payload.size() != sizeof(Body)) {
        return std::unexpected(BodyError::WrongSize);
    }
    std::array<std::byte, sizeof(Body)> raw{};
    std::memcpy(raw.data(), payload.data(), sizeof(Body));
    const auto body = std::bit_cast<Body>(raw);
    if (!is_valid(body)) {
        return std::unexpected(BodyError::BadValue);
    }
    return body;
}

// For PlayReq, PauseReq and StopReq, which carry no body: any payload at all
// is a malformed request.
[[nodiscard]] constexpr std::expected<void, BodyError>
expect_no_body(std::span<const std::byte> payload) noexcept {
    if (!payload.empty()) {
        return std::unexpected(BodyError::WrongSize);
    }
    return {};
}

// ---- Names -------------------------------------------------------------------

// Packs `name` into HelloBody's fixed field, padded with zero bytes. Anything
// past 32 bytes is cut off (which can split a multi-byte UTF-8 character;
// fine for a log line).
[[nodiscard]] constexpr std::array<char, kPeerNameBytes>
make_peer_name(std::string_view name) noexcept {
    std::array<char, kPeerNameBytes> field{};
    std::ranges::copy(name.substr(0, kPeerNameBytes), field.begin());
    return field;
}

// The name in a received HelloBody: everything up to the first zero byte, or
// all 32 bytes if there isn't one.
[[nodiscard]] constexpr std::string_view peer_name(const HelloBody& body) noexcept {
    const std::string_view all(body.name.data(), body.name.size());
    return all.substr(0, all.find('\0'));
}

// ---- Track text --------------------------------------------------------------

// Packs `text` into one of TrackChangedBody's N-byte text fields, padded with
// zero bytes. Unlike a peer name it's shown to the user, so a long one is cut
// before the character that doesn't fit whole, never in the middle of it.
//
//   .title = make_text_field<kTrackTitleBytes>(title),
template <std::size_t N>
[[nodiscard]] constexpr std::array<char, N> make_text_field(std::string_view text) noexcept {
    std::size_t length = text.size();
    if (length > N) {
        length = N;
        // UTF-8 continuation bytes look like 10xxxxxx. Back up while the
        // first byte that doesn't fit is one, so the cut lands on the start
        // of a character.
        while (length > 0 && (static_cast<unsigned char>(text[length]) & 0xC0U) == 0x80U) {
            --length;
        }
    }
    std::array<char, N> field{};
    std::ranges::copy(text.substr(0, length), field.begin());
    return field;
}

// Packs a link into the artwork field. One that doesn't fit is left out
// (an empty field) rather than cut, because a cut link points at nothing.
[[nodiscard]] constexpr std::array<char, kTrackArtworkUrlBytes>
make_url_field(std::string_view url) noexcept {
    if (url.size() > kTrackArtworkUrlBytes) {
        return {};
    }
    return make_text_field<kTrackArtworkUrlBytes>(url);
}

// The text in a received field: everything up to the first zero byte, or the
// whole field if there isn't one.
template <std::size_t N>
[[nodiscard]] constexpr std::string_view text_field(const std::array<char, N>& field) noexcept {
    const std::string_view all(field.data(), field.size());
    return all.substr(0, all.find('\0'));
}

[[nodiscard]] constexpr std::string_view track_title(const TrackChangedBody& body) noexcept {
    return text_field(body.title);
}
[[nodiscard]] constexpr std::string_view track_artist(const TrackChangedBody& body) noexcept {
    return text_field(body.artist);
}
[[nodiscard]] constexpr std::string_view track_album(const TrackChangedBody& body) noexcept {
    return text_field(body.album);
}
[[nodiscard]] constexpr std::string_view track_artwork_url(const TrackChangedBody& body) noexcept {
    return text_field(body.artwork_url);
}

static_assert(text_field(make_text_field<4>("abc")) == "abc");
static_assert(text_field(make_text_field<4>("abcdef")) == "abcd");
static_assert(text_field(make_text_field<4>("abc\xC3\xA9")) == "abc"); // 'é' doesn't fit whole
static_assert(text_field(make_url_field("https://example.com/a.jpg")) ==
              "https://example.com/a.jpg");

} // namespace am::common
