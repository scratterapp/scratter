// Tests for the catalog parser (src/net/catalog_parser.hpp, #105): the saved
// sample responses in tests/data/catalog/, then everything a response can do
// wrong: missing fields, wrong types, cut short, not UTF-8, too big for the
// arena.

#include "net/catalog_parser.hpp"

#include "catalog_samples.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace {

using am::common::LinearArena;
using am::net::CatalogParser;
using am::net::ParseError;
using am::net::Song;
using am::test::make_playlist_json;
using am::test::PaddedBody;
using am::test::read_sample;

constexpr std::size_t kArenaBytes = std::size_t{1} << 20;

// One parser and one arena, as a view would have.
struct Fixture {
    CatalogParser parser = *CatalogParser::create();
    LinearArena arena = *LinearArena::create(kArenaBytes);
};

std::expected<std::span<const Song>, ParseError> songs(Fixture& f, std::string_view json) {
    const PaddedBody body(json);
    return f.parser.parse_songs(body.span(), f.arena);
}

// The error parsing `json` as a list of songs gives, or nullopt if it parsed.
std::optional<ParseError> songs_error(std::string_view json) {
    Fixture f;
    const auto parsed = songs(f, json);
    if (parsed) {
        return std::nullopt;
    }
    return parsed.error();
}

} // namespace

TEST_CASE("the samples are there", "[catalog]") {
    for (const char* name : {"album.json", "playlist.json", "songs.json", "search.json",
                             "search-empty.json", "not-found.json"}) {
        INFO(name);
        CHECK_FALSE(read_sample(name).empty());
    }
}

TEST_CASE("an album comes out with its tracks", "[catalog]") {
    Fixture f;
    const PaddedBody body(read_sample("album.json"));
    const auto album = f.parser.parse_album(body.span(), f.arena);
    REQUIRE(album.has_value());

    CHECK(album->id == "1440857781");
    CHECK(album->catalog_id == 1440857781);
    CHECK(album->title == "Rooms Without Doors");
    CHECK(album->artist == "The Sample Quartet");
    CHECK(album->release_date == "2019-05-24");
    CHECK(album->artwork.url_template ==
          "https://is1-ssl.mzstatic.com/image/thumb/Music/v4/00/00/00/sample/source/{w}x{h}bb.jpg");
    CHECK(album->artwork.width == 3000);
    CHECK(album->artwork.height == 3000);
    CHECK(album->tracks_next.empty());

    REQUIRE(album->tracks.size() == 3);
    const Song& first = album->tracks[0];
    CHECK(first.id == "1440857786");
    CHECK(first.catalog_id == 1440857786);
    CHECK(first.title == "Hallway Light");
    CHECK(first.artist == "The Sample Quartet");
    CHECK(first.album == "Rooms Without Doors");
    CHECK(first.duration_micros == 215'347'000);
    CHECK(first.track_number == 1);
    CHECK(first.disc_number == 1);
    CHECK(first.artwork.width == 3000);

    // Escapes come out as the characters they stand for.
    CHECK(album->tracks[1].title == "\"Quoted\" Stairs");
    CHECK(album->tracks[1].artist == "The Sample Quartet feat. José Niño");

    // A track with no artwork, on the second disc, with a title in UTF-8.
    const Song& last = album->tracks[2];
    CHECK(last.title == "階段 (Stairs)");
    CHECK(last.disc_number == 2);
    CHECK(last.track_number == 1);
    CHECK(last.artwork.url_template.empty());
    CHECK(last.artwork.width == 0);
}

TEST_CASE("a playlist leaves out its music videos and keeps the next page", "[catalog]") {
    Fixture f;
    const PaddedBody body(read_sample("playlist.json"));
    const auto playlist = f.parser.parse_playlist(body.span(), f.arena);
    REQUIRE(playlist.has_value());

    CHECK(playlist->id == "pl.5ee8333dbe944d9f9151e97d81d8f6d2");
    CHECK(playlist->name == "Late Night Sample");
    CHECK(playlist->curator == "Apple Music");
    CHECK(playlist->description == "Quiet songs for late nights, updated every Friday.");
    CHECK(playlist->artwork.width == 1080);
    CHECK(playlist->tracks_next ==
          "/v1/catalog/us/playlists/pl.5ee8333dbe944d9f9151e97d81d8f6d2/tracks?offset=100");

    REQUIRE(playlist->tracks.size() == 2);
    CHECK(playlist->tracks[0].title == "Song One");
    CHECK(playlist->tracks[0].track_number == 4);
    CHECK(playlist->tracks[1].title == "Song Three");
    CHECK(playlist->tracks[1].duration_micros == 199'500'000);
    CHECK(playlist->tracks[1].artwork.url_template.empty());
}

TEST_CASE("a songs response gives every song in it", "[catalog]") {
    Fixture f;
    const PaddedBody body(read_sample("songs.json"));
    const auto list = f.parser.parse_songs(body.span(), f.arena);
    REQUIRE(list.has_value());
    REQUIRE(list->size() == 2);
    CHECK((*list)[0].title == "Hallway Light");
    CHECK((*list)[1].title == "Song Three");
    CHECK((*list)[1].disc_number == 0); // not in the response
}

TEST_CASE("a search gives songs, albums and playlists", "[catalog]") {
    Fixture f;
    const PaddedBody body(read_sample("search.json"));
    const auto results = f.parser.parse_search(body.span(), f.arena);
    REQUIRE(results.has_value());

    REQUIRE(results->songs.size() == 1);
    CHECK(results->songs[0].title == "Hallway Light");

    REQUIRE(results->albums.size() == 2);
    CHECK(results->albums[0].title == "Rooms Without Doors");
    CHECK(results->albums[0].tracks.empty()); // a search doesn't include them
    CHECK(results->albums[1].release_date == "1968");
    CHECK(results->albums[1].artwork.url_template.empty());

    REQUIRE(results->playlists.size() == 1);
    CHECK(results->playlists[0].name == "Late Night Sample");
    CHECK(results->playlists[0].description == "Quiet songs for late nights."); // only a short one
}

TEST_CASE("a search with no matches is empty, not an error", "[catalog]") {
    Fixture f;
    const PaddedBody body(read_sample("search-empty.json"));
    const auto results = f.parser.parse_search(body.span(), f.arena);
    REQUIRE(results.has_value());
    CHECK(results->songs.empty());
    CHECK(results->albums.empty());
    CHECK(results->playlists.empty());
}

TEST_CASE("Apple's 404 answer has no resource in it", "[catalog]") {
    Fixture f;
    const PaddedBody body(read_sample("not-found.json"));
    CHECK(f.parser.parse_album(body.span(), f.arena).error() == ParseError::NoResource);
    CHECK(f.parser.parse_playlist(body.span(), f.arena).error() == ParseError::NoResource);
    CHECK(f.parser.parse_songs(body.span(), f.arena).error() == ParseError::NoResource);
    CHECK(f.parser.parse_search(body.span(), f.arena).error() == ParseError::NoResource);
    CHECK(songs_error(R"({"data":[]})") == ParseError::NoResource);
}

TEST_CASE("what comes out lives in the arena, not in the body", "[catalog]") {
    Fixture f;
    std::string json = read_sample("songs.json");
    const auto list = songs(f, json); // the body is gone after this line
    REQUIRE(list.has_value());
    json.assign(json.size(), 'x');
    CHECK((*list)[0].title == "Hallway Light");
    CHECK(f.arena.used() > 0);
}

TEST_CASE("one parser reads one response after another", "[catalog]") {
    Fixture f;
    for (int round = 0; round < 3; ++round) {
        const PaddedBody album(read_sample("album.json"));
        const PaddedBody search(read_sample("search.json"));
        REQUIRE(f.parser.parse_album(album.span(), f.arena).has_value());
        REQUIRE(f.parser.parse_search(search.span(), f.arena).has_value());
        f.arena.reset();
    }
}

TEST_CASE("missing optional fields are left empty", "[catalog]") {
    Fixture f;
    const auto list = songs(f, R"({"data":[{"id":"1"},{"id":"2","attributes":{}}]})");
    REQUIRE(list.has_value());
    REQUIRE(list->size() == 2);
    const Song& song = (*list)[0];
    CHECK(song.id == "1");
    CHECK(song.catalog_id == 1);
    CHECK(song.title.empty());
    CHECK(song.artist.empty());
    CHECK(song.album.empty());
    CHECK(song.duration_micros == 0);
    CHECK(song.track_number == 0);
    CHECK(song.artwork.url_template.empty());
}

TEST_CASE("null counts as missing", "[catalog]") {
    Fixture f;
    const auto list = songs(f, R"({"data":[{"id":"7","type":null,"attributes":{
        "name":null,"durationInMillis":null,"trackNumber":null,"artwork":null}}]})");
    REQUIRE(list.has_value());
    REQUIRE(list->size() == 1);
    CHECK((*list)[0].title.empty());
    CHECK((*list)[0].duration_micros == 0);
    CHECK(songs_error(R"({"data":null})") == ParseError::NoResource);
}

TEST_CASE("an id that isn't a number has catalog_id 0", "[catalog]") {
    Fixture f;
    const auto list = songs(f, R"({"data":[{"id":"i.abcDEF123"},{"id":"99999999999999999999"},
                                           {"id":"-5"},{"id":"12a"}]})");
    REQUIRE(list.has_value());
    REQUIRE(list->size() == 4);
    CHECK((*list)[0].id == "i.abcDEF123"); // a library id
    for (const Song& song : *list) {
        CHECK(song.catalog_id == 0);
    }
}

TEST_CASE("a resource without an id is an error", "[catalog]") {
    CHECK(songs_error(R"({"data":[{"type":"songs","attributes":{"name":"x"}}]})") ==
          ParseError::NoResource);
    CHECK(songs_error(R"({"data":[{"id":""}]})") == ParseError::NoResource);
    CHECK(songs_error(R"({"data":[{"id":"1"},{}]})") == ParseError::NoResource);
}

TEST_CASE("a field with the wrong type is an error", "[catalog]") {
    for (const std::string_view json : {
             R"({"data":[{"id":12}]})",
             R"({"data":{"id":"1"}})",
             R"({"data":["1"]})",
             R"({"data":[{"id":"1","attributes":[]}]})",
             R"({"data":[{"id":"1","attributes":{"name":5}}]})",
             R"({"data":[{"id":"1","attributes":{"artistName":true}}]})",
             R"({"data":[{"id":"1","attributes":{"durationInMillis":"215347"}}]})",
             R"({"data":[{"id":"1","attributes":{"durationInMillis":-1}}]})",
             R"({"data":[{"id":"1","attributes":{"durationInMillis":1.5}}]})",
             R"({"data":[{"id":"1","attributes":{"durationInMillis":18446744073709552}}]})",
             R"({"data":[{"id":"1","attributes":{"trackNumber":4294967296}}]})",
             R"({"data":[{"id":"1","attributes":{"discNumber":18446744073709551616}}]})",
             R"({"data":[{"id":"1","attributes":{"artwork":"https://x/{w}x{h}.jpg"}}]})",
             R"({"data":[{"id":"1","attributes":{"artwork":{"width":-3000}}}]})",
             R"({"data":[{"id":"1","type":4}]})",
             R"(["data"])",
             R"("data")",
         }) {
        INFO(json);
        CHECK(songs_error(json) == ParseError::WrongType);
    }
}

TEST_CASE("the largest numbers that fit are fine", "[catalog]") {
    Fixture f;
    const auto list = songs(f, R"({"data":[{"id":"18446744073709551615","attributes":{
        "durationInMillis":18446744073709551,"trackNumber":4294967295}}]})");
    REQUIRE(list.has_value());
    CHECK((*list)[0].catalog_id == 18446744073709551615u);
    CHECK((*list)[0].duration_micros == 18446744073709551000u);
    CHECK((*list)[0].track_number == 4294967295u);
}

TEST_CASE("something that isn't JSON is an error", "[catalog]") {
    for (const std::string_view json : {
             "",
             " ",
             "<html>Service Unavailable</html>",
             R"({"data":[{"id":"1"}]} {"data":[]})", // two documents
             R"({"data":[{"id":"1"}]}x)",
             R"({"data":[{"id":"1"}],})",
             R"({"data":[{"id":"1"},]})",
             R"({"data":[{"id":'1'}]})",
             R"({"data":[{"id":"1\q"}]})",
         }) {
        INFO(json);
        const auto error = songs_error(json);
        REQUIRE(error.has_value());
        CHECK(*error == ParseError::NotJson);
    }
}

TEST_CASE("every cut-short sample is an error, never a crash", "[catalog]") {
    Fixture f;
    for (const char* name : {"album.json", "playlist.json", "songs.json", "search.json"}) {
        const std::string json = read_sample(name);
        // Trailing whitespace can go without changing the document.
        const std::size_t end = json.find_last_not_of(" \n") + 1;
        for (std::size_t length = 0; length < end; ++length) {
            const PaddedBody body(std::string_view(json).substr(0, length));
            f.arena.reset();
            INFO(name << " cut to " << length << " bytes");
            CHECK_FALSE(f.parser.parse_album(body.span(), f.arena).has_value());
            CHECK_FALSE(f.parser.parse_playlist(body.span(), f.arena).has_value());
            CHECK_FALSE(f.parser.parse_songs(body.span(), f.arena).has_value());
            CHECK_FALSE(f.parser.parse_search(body.span(), f.arena).has_value());
        }
    }
}

TEST_CASE("bytes that aren't UTF-8 are an error", "[catalog]") {
    for (const std::string_view json : {
             "{\"data\":[{\"id\":\"1\",\"attributes\":{\"name\":\"\xff\"}}]}",
             "{\"data\":[{\"id\":\"1\",\"attributes\":{\"name\":\"\xc3\"}}]}",         // cut short
             "{\"data\":[{\"id\":\"1\",\"attributes\":{\"name\":\"\xed\xa0\x80\"}}]}", // a
                                                                                       // surrogate
             "{\"data\":[{\"id\":\"1\"}],\"meta\":\"\xc0\xaf\"}", // overlong, in a field we skip
         }) {
        INFO(json);
        CHECK(songs_error(json) == ParseError::NotUtf8);
    }
}

TEST_CASE("a body bigger than create() allowed is refused", "[catalog]") {
    auto parser = CatalogParser::create(24);
    REQUIRE(parser.has_value());
    auto arena = LinearArena::create(1024);
    REQUIRE(arena.has_value());
    const PaddedBody fits(R"({"data":[{"id":"1"}]})");
    const PaddedBody too_big(R"({"data":[{"id":"1"},{"id":"2"}]})");
    CHECK(parser->parse_songs(fits.span(), *arena).has_value());
    CHECK(parser->parse_songs(too_big.span(), *arena).error() == ParseError::TooLarge);
    CHECK(CatalogParser::create(0).error() == ParseError::TooLarge);
}

TEST_CASE("a full arena is an error at any point", "[catalog]") {
    const PaddedBody body(read_sample("album.json"));
    Fixture f;
    REQUIRE(f.parser.parse_album(body.span(), f.arena).has_value());
    const std::size_t needed = f.arena.used();

    // Every size short of what the album takes, so the arena fills up at each
    // point in the parse: the track array, a title, an artwork URL...
    for (std::size_t size = 1; size < needed; ++size) {
        auto small = LinearArena::create(size);
        REQUIRE(small.has_value());
        INFO("arena of " << size << " bytes");
        CHECK(f.parser.parse_album(body.span(), *small).error() == ParseError::ArenaFull);
    }
    auto exact = LinearArena::create(needed);
    REQUIRE(exact.has_value());
    CHECK(f.parser.parse_album(body.span(), *exact).has_value());
}

TEST_CASE("a 500-track playlist comes out whole", "[catalog]") {
    Fixture f;
    const std::string json = make_playlist_json(500);
    const PaddedBody body(json);
    const auto playlist = f.parser.parse_playlist(body.span(), f.arena);
    REQUIRE(playlist.has_value());
    CHECK(playlist->name == "Five Hundred");
    REQUIRE(playlist->tracks.size() == 500);
    for (std::size_t i = 0; i < playlist->tracks.size(); ++i) {
        const Song& song = playlist->tracks[i];
        if (song.catalog_id != 1700000000 + i || song.title != "Song " + std::to_string(i) ||
            song.duration_micros != (180000 + i) * 1000 || song.track_number != i % 20 + 1) {
            FAIL("track " << i << " is wrong");
        }
    }
    // About 470 KB, the size the benchmark's comments quote.
    CHECK(json.size() > 450 * 1000);
    CHECK(json.size() < 500 * 1000);
}

TEST_CASE("every error has words for a log line", "[catalog]") {
    for (const ParseError error :
         {ParseError::NotJson, ParseError::NotUtf8, ParseError::WrongType, ParseError::NoResource,
          ParseError::TooLarge, ParseError::ArenaFull, ParseError::OutOfMemory}) {
        CHECK_FALSE(am::net::to_string(error).empty());
    }
}

TEST_CASE("an album or playlist without an id is an error", "[catalog]") {
    Fixture f;
    const PaddedBody album(R"({"data":[{"attributes":{"name":"x"}}]})");
    const PaddedBody playlist(R"({"data":[{"type":"playlists","relationships":{}}]})");
    CHECK(f.parser.parse_album(album.span(), f.arena).error() == ParseError::NoResource);
    CHECK(f.parser.parse_playlist(playlist.span(), f.arena).error() == ParseError::NoResource);
}

TEST_CASE("a parser can be moved", "[catalog]") {
    auto first = CatalogParser::create();
    auto second = CatalogParser::create();
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    *second = std::move(*first);
    auto arena = LinearArena::create(kArenaBytes);
    REQUIRE(arena.has_value());
    const PaddedBody body(read_sample("songs.json"));
    CHECK(second->parse_songs(body.span(), *arena).has_value());
}
