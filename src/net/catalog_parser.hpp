#pragma once

// Apple Music catalog responses into plain structs (spec section 3.3.2, issue
// #105).
//
// The HTTP client (#104) hands over a response body; the parser reads it with
// simdjson's On-Demand API and copies what the window shows (ids, titles,
// artists, durations, artwork) into a LinearArena (#103):
//
//   auto parser = CatalogParser::create();          // once, at start-up
//   auto arena = LinearArena::create(1024 * 1024);  // one per view
//   ...
//   // In the HttpClient callback, on the HTTP thread:
//   const auto playlist = parser->parse_playlist(response.body, *arena);
//   if (!playlist) { log to_string(playlist.error()); }
//   for (const Song& song : playlist->tracks) { ... }
//   ...
//   arena->reset(); // the user moved to another view: everything above is gone
//
// What comes out points into the arena, never into the body, so the HTTP
// client can reuse its buffer for the next response straight away.
//
// The body must be followed in memory by kBodyPadding readable bytes, as
// HttpResponse::body always is: simdjson reads up to 64 bytes past the end
// with wide vector loads. A test or fuzzer that makes its own body has to add
// them (tests/catalog_samples.hpp shows how).
//
// What counts as an error:
//   - Anything that isn't one whole, valid JSON document in UTF-8, including
//     a body cut short and trailing bytes after it.
//   - A field we read that has the wrong type (a name that's a number, a
//     duration that's negative or a string) or a number that doesn't fit.
//   - A response with no resource in it (`data` missing or empty), or a
//     resource with no `id`.
//   - A full arena. What was copied in before then stays in it, so reset()
//     the arena and try a bigger one.
// A missing optional field is not an error: it's left empty or 0. Fields we
// don't read are skipped without looking at them.
//
// The parser makes all of its own memory in create() and nothing after: a
// parse allocates only in the arena (bench/catalog_parse.cpp checks it). One
// thread at a time; for parsing on two threads, make two parsers.

#include "common/arena_allocator.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string_view>

namespace am::net {

// A picture at a URL with {w} and {h} in it, for the width and height wanted
// (no bigger than width x height, the original's size).
struct Artwork {
    std::string_view url_template;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// A song, from a song, album, playlist or search response ("type": "songs").
struct Song {
    std::string_view id; // Apple's id, as sent ("1440857786", or "i.xyz" in a library)
    std::uint64_t catalog_id =
        0; // the id as a number, the way LoadTrackBody carries it; 0 if it isn't one
    std::string_view title;
    std::string_view artist;
    std::string_view album;
    std::uint64_t duration_micros = 0; // the same unit as TrackChangedBody
    std::uint32_t track_number = 0;
    std::uint32_t disc_number = 0;
    Artwork artwork;
};

struct Album {
    std::string_view id;
    std::uint64_t catalog_id = 0;
    std::string_view title;
    std::string_view artist;
    std::string_view release_date; // "2019-05-24", or just a year for some old records
    Artwork artwork;
    std::span<const Song> tracks; // empty in search results, which don't include them
    std::string_view tracks_next; // where the next page of tracks is, or empty
};

struct Playlist {
    std::string_view id; // "pl.<letters and digits>": never a number
    std::string_view name;
    std::string_view curator;     // who made it: Apple Music, or a person
    std::string_view description; // the standard one, or else the short one
    Artwork artwork;
    std::span<const Song> tracks;
    std::string_view tracks_next;
};

// A search: up to three kinds, each in Apple's order of relevance.
struct SearchResults {
    std::span<const Song> songs;
    std::span<const Album> albums;
    std::span<const Playlist> playlists;
};

enum class ParseError : std::uint8_t {
    NotJson,     // not one whole JSON document: malformed, cut short, or more after it
    NotUtf8,     // a byte sequence that isn't UTF-8
    WrongType,   // a field we read has another type, or a number that doesn't fit
    NoResource,  // no `data` (or `results`), an empty one, or a resource without an id
    TooLarge,    // the body is bigger than create()'s max_body_bytes
    ArenaFull,   // the arena ran out of room
    OutOfMemory, // create() couldn't get its memory
};

[[nodiscard]] std::string_view to_string(ParseError error) noexcept;

class CatalogParser {
  public:
    // 4 MiB: a 500-track playlist is about 470 KB, and Apple pages bigger
    // lists anyway (tracks_next).
    static constexpr std::size_t kDefaultMaxBodyBytes = std::size_t{4} * 1024 * 1024;

    // Makes simdjson's buffers for bodies up to `max_body_bytes`, once. 0
    // is refused with TooLarge.
    [[nodiscard]] static std::expected<CatalogParser, ParseError>
    create(std::size_t max_body_bytes = kDefaultMaxBodyBytes) noexcept;

    CatalogParser(CatalogParser&&) noexcept;
    CatalogParser& operator=(CatalogParser&&) noexcept;
    CatalogParser(const CatalogParser&) = delete;
    CatalogParser& operator=(const CatalogParser&) = delete;
    ~CatalogParser();

    // GET /v1/catalog/{storefront}/songs/{id}, or /songs?ids=...: every song
    // in `data`.
    [[nodiscard]] std::expected<std::span<const Song>, ParseError>
    parse_songs(std::span<const std::byte> body, common::LinearArena& arena) noexcept;

    // GET /v1/catalog/{storefront}/albums/{id}: the first album in `data`,
    // with its tracks.
    [[nodiscard]] std::expected<Album, ParseError> parse_album(std::span<const std::byte> body,
                                                               common::LinearArena& arena) noexcept;

    // GET /v1/catalog/{storefront}/playlists/{id}: the first playlist in
    // `data`, with its tracks. Music videos in a playlist are left out.
    [[nodiscard]] std::expected<Playlist, ParseError>
    parse_playlist(std::span<const std::byte> body, common::LinearArena& arena) noexcept;

    // GET /v1/catalog/{storefront}/search?types=songs,albums,playlists: any
    // kind Apple left out (no matches, or not asked for) is empty.
    [[nodiscard]] std::expected<SearchResults, ParseError>
    parse_search(std::span<const std::byte> body, common::LinearArena& arena) noexcept;

  private:
    struct Impl;
    explicit CatalogParser(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace am::net
