// What the library view says about playlists and how it cuts long names short
// (src/client/library.hpp), plus the sample playlists it shows until the
// MusicKit key exists (src/client/sample_library.hpp).

#include "client/library.hpp"
#include "client/sample_library.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

using am::client::elide;
using am::client::playlist_summary;
using am::client::Song;

namespace {

constexpr std::uint64_t kMinute = 60'000'000;

Song song_of(std::uint64_t micros) { return Song{"t", "a", "b", micros, 1}; }

// One unit of width per letter (not per byte), like a fixed-width font.
float letters(std::string_view text) {
    float width = 0.0f;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0U) != 0x80U) {
            width += 1.0f;
        }
    }
    return width;
}

} // namespace

static_assert(am::client::sample::length(4, 12) == 252'000'000);

TEST_CASE("playlist_summary counts songs and minutes") {
    const std::array ten{song_of(4 * kMinute), song_of(5 * kMinute), song_of(4 * kMinute),
                         song_of(4 * kMinute), song_of(5 * kMinute), song_of(4 * kMinute),
                         song_of(4 * kMinute), song_of(5 * kMinute), song_of(4 * kMinute),
                         song_of(5 * kMinute)};
    CHECK(playlist_summary(ten) == "10 songs, 44 minutes");
}

TEST_CASE("playlist_summary says one song and one minute in the singular") {
    const std::array one{song_of(kMinute)};
    CHECK(playlist_summary(one) == "1 song, 1 minute");
}

TEST_CASE("playlist_summary rounds to the nearest minute") {
    const std::array just_under{song_of(3 * kMinute + 29'000'000)};
    CHECK(playlist_summary(just_under) == "1 song, 3 minutes");
    const std::array half_up{song_of(3 * kMinute + 30'000'000)};
    CHECK(playlist_summary(half_up) == "1 song, 4 minutes");
}

TEST_CASE("playlist_summary gives hours for a long playlist") {
    const std::array exactly_two{song_of(60 * kMinute), song_of(60 * kMinute)};
    CHECK(playlist_summary(exactly_two) == "2 songs, 2 hours");
    const std::array hour_and_one{song_of(61 * kMinute)};
    CHECK(playlist_summary(hour_and_one) == "1 song, 1 hour 1 minute");
    const std::array hour_and_more{song_of(65 * kMinute), song_of(60 * kMinute)};
    CHECK(playlist_summary(hour_and_more) == "2 songs, 2 hours 5 minutes");
}

TEST_CASE("playlist_summary for an empty or very short playlist") {
    CHECK(playlist_summary({}) == "No songs yet");
    const std::array short_one{song_of(20'000'000)};
    CHECK(playlist_summary(short_one) == "1 song, under a minute");
}

TEST_CASE("elide leaves text that fits alone") {
    CHECK(elide("Northbound", 10.0f, letters) == "Northbound");
    CHECK(elide("Northbound", 50.0f, letters) == "Northbound");
    CHECK(elide("", 0.0f, letters).empty());
}

TEST_CASE("elide cuts text that doesn't fit and ends it with an ellipsis") {
    // Nine letters of room: eight of the title and the "…".
    CHECK(elide("Last Gas Before Dawn", 9.0f, letters) == "Last Gas\xE2\x80\xA6");
    CHECK(letters(elide("Last Gas Before Dawn", 9.0f, letters)) <= 9.0f);
}

TEST_CASE("elide drops the spaces it would leave before the ellipsis") {
    // Six letters of room would be "Last " and "…"; the space goes.
    CHECK(elide("Last Gas Before Dawn", 6.0f, letters) == "Last\xE2\x80\xA6");
}

TEST_CASE("elide never cuts a letter in half") {
    // Each Greek letter is two bytes. Any cut must land between letters, so
    // the result is whole letters plus the "…", never a stray byte.
    const std::string_view greek = "Νυχτερινό τρένο";
    for (float width = 1.0f; width < letters(greek); width += 1.0f) {
        const std::string cut = elide(greek, width, letters);
        INFO("width " << width << ": " << cut);
        CHECK(letters(cut) <= width);
        REQUIRE(cut.size() >= 3);
        CHECK(cut.ends_with("\xE2\x80\xA6"));
        const std::string_view kept{cut.data(), cut.size() - 3};
        CHECK(greek.starts_with(kept));
        CHECK((kept.size() == greek.size() ||
               (static_cast<unsigned char>(greek[kept.size()]) & 0xC0U) != 0x80U));
    }
}

TEST_CASE("elide gives nothing when not even the ellipsis fits") {
    CHECK(elide("Northbound", 0.5f, letters).empty());
    CHECK(elide("Northbound", 1.0f, letters) == "\xE2\x80\xA6");
}

TEST_CASE("the sample playlists are all filled in") {
    using am::client::sample::kPlaylists;
    REQUIRE_FALSE(kPlaylists.empty());
    for (const am::client::Playlist& playlist : kPlaylists) {
        INFO(playlist.name);
        CHECK_FALSE(playlist.name.empty());
        CHECK_FALSE(playlist.tracks.empty());
        for (const Song& song : playlist.tracks) {
            INFO(song.title);
            CHECK_FALSE(song.title.empty());
            CHECK_FALSE(song.artist.empty());
            CHECK_FALSE(song.album.empty());
            CHECK(song.duration_micros > 0);
            CHECK(song.track_number > 0);
        }
    }
}

TEST_CASE("the first sample playlist is the one in the design mockup") {
    const am::client::Playlist& first = am::client::sample::kPlaylists.front();
    CHECK(first.name == "Late Night Drive");
    CHECK(playlist_summary(first.tracks) == "10 songs, 44 minutes");
}
