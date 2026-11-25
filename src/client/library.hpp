#pragma once

// What the library view (views/library_view.cpp) draws: songs and playlists,
// and the words it shows about them. Kept apart from the drawing so tests can
// check them without a window (tests/test_library.cpp).
//
// Until the MusicKit key exists the songs come from sample_library.hpp. The
// field names and units follow the catalog parser's structs (#105, am::net),
// strings borrowed from its arena included, so feeding this view real
// playlists later is a copy of a few fields, or a swap of these two structs
// for those.

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace am::client {

struct Song {
    std::string_view title;
    std::string_view artist;
    std::string_view album;
    std::uint64_t duration_micros = 0; // as in TrackChangedBody
    std::uint32_t track_number = 0;    // on its album; 0 when unknown
};

struct Playlist {
    std::string_view name;
    std::string_view curator; // who made it; empty for the user's own
    std::span<const Song> tracks;
};

// "10 songs, 44 minutes", "1 song, 3 minutes", "2 hours 5 minutes" for a long
// one. The length is rounded to the nearest minute, and a playlist shorter
// than half a minute says "under a minute".
inline std::string playlist_summary(std::span<const Song> tracks) {
    if (tracks.empty()) {
        return "No songs yet";
    }
    std::uint64_t micros = 0;
    for (const Song& song : tracks) {
        micros += song.duration_micros;
    }
    std::string text =
        std::to_string(tracks.size()) + (tracks.size() == 1 ? " song, " : " songs, ");
    const std::uint64_t minutes = (micros + 30'000'000) / 60'000'000;
    if (minutes == 0) {
        return text + "under a minute";
    }
    const std::uint64_t hours = minutes / 60;
    const std::uint64_t rest = minutes % 60;
    if (hours > 0) {
        text += std::to_string(hours) + (hours == 1 ? " hour" : " hours");
        if (rest == 0) {
            return text;
        }
        text += ' ';
    }
    return text + std::to_string(rest) + (rest == 1 ? " minute" : " minutes");
}

// `text` cut short with "…" so it fits in `max_width`, or as it is when it
// already fits. `measure` gives a string's width (in the window, ImGui's
// CalcTextSize for the font in use). Cuts only between whole UTF-8 letters,
// drops spaces left before the "…", and returns "" when not even the "…"
// fits.
template <std::invocable<std::string_view> Measure>
[[nodiscard]] std::string elide(std::string_view text, float max_width, Measure measure) {
    if (measure(text) <= max_width) {
        return std::string{text};
    }
    constexpr std::string_view kEllipsis = "\xE2\x80\xA6"; // …
    auto start_of_letter = [&](std::size_t at) {
        // UTF-8 continuation bytes look like 10xxxxxx: step back over them.
        while (at > 0 && (static_cast<unsigned char>(text[at]) & 0xC0U) == 0x80U) {
            --at;
        }
        return at;
    };
    auto fits = [&](std::size_t length) {
        std::string candidate{text.substr(0, length)};
        candidate += kEllipsis;
        return measure(candidate) <= max_width;
    };
    // The longest cut that fits, by halving: widths only grow with length.
    std::size_t low = 0; // fits (or is 0)
    std::size_t high = text.size();
    while (high - low > 1) {
        const std::size_t middle = low + (high - low) / 2;
        if (fits(middle)) {
            low = middle;
        } else {
            high = middle;
        }
    }
    std::size_t cut = start_of_letter(low);
    while (cut > 0 && text[cut - 1] == ' ') {
        --cut;
    }
    if (cut == 0 && !fits(0)) {
        return {};
    }
    std::string result{text.substr(0, cut)};
    result += kEllipsis;
    return result;
}

} // namespace am::client
