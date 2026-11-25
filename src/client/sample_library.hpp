#pragma once

// Made-up playlists for the library view, until the MusicKit key lets am-ui
// fetch real ones (#4, then the catalog parser, #105). Every artist, album and
// song here is invented, so nothing licensed or personal is in the repo. The
// last playlist has Greek, Cyrillic and Vietnamese names and a very long title,
// to show the fallback fonts and the "…" on screen.

#include "client/library.hpp"

#include <array>
#include <cstdint>

namespace am::client::sample {

// 4:12 as microseconds.
consteval std::uint64_t length(std::uint64_t minutes, std::uint64_t seconds) {
    return (minutes * 60 + seconds) * 1'000'000;
}

inline constexpr std::array kLateNightDrive{
    Song{"Sodium Lights", "The Quiet Hours", "Overpass", length(4, 12), 1},
    Song{"Northbound", "Mara Ellison", "Glass Weather", length(3, 47), 2},
    Song{"Low Tide Radio", "Pale Harbor", "Low Tide Radio", length(5, 3), 1},
    Song{"Ghost Lanes", "The Quiet Hours", "Overpass", length(3, 29), 4},
    Song{"Amber Static", "Juno Vale", "Afterhours", length(4, 41), 3},
    Song{"Cassette Moon", "Okin & Rye", "Tape Hiss", length(3, 58), 6},
    Song{"Exit 14", "Mara Ellison", "Glass Weather", length(4, 20), 7},
    Song{"Velvet Signal", "Pale Harbor", "Low Tide Radio", length(6, 2), 5},
    Song{"Rearview", "Juno Vale", "Afterhours", length(3, 36), 9},
    Song{"Last Gas Before Dawn", "Okin & Rye", "Tape Hiss", length(4, 55), 10},
};

inline constexpr std::array kSundayCoffee{
    Song{"Steam on the Window", "Hollis & Grey", "Kitchen Light", length(3, 12), 1},
    Song{"Slow Pour", "Ines Calder", "Morning Pages", length(4, 5), 2},
    Song{"Crossword", "The Lindens", "Small Hours", length(2, 58), 3},
    Song{"Linen", "Hollis & Grey", "Kitchen Light", length(3, 44), 5},
    Song{"Second Cup", "Ines Calder", "Morning Pages", length(3, 21), 4},
    Song{"Paper Round", "The Lindens", "Small Hours", length(4, 30), 8},
};

inline constexpr std::array kFocusMostly{
    Song{"Grid Paper", "Tessellate", "Graph", length(5, 40), 1},
    Song{"Long Division", "Tessellate", "Graph", length(6, 12), 2},
    Song{"Index Cards", "Room Tone", "Quiet Machines", length(4, 48), 1},
    Song{"Fan Hum", "Room Tone", "Quiet Machines", length(7, 3), 2},
    Song{"Margins", "Ada Brook", "Working Title", length(5, 15), 3},
    Song{"Footnote", "Ada Brook", "Working Title", length(3, 59), 4},
    Song{"Pencil Shavings", "Tessellate", "Graph", length(4, 22), 5},
    Song{"Stacks", "Room Tone", "Quiet Machines", length(6, 31), 3},
    Song{"Highlighter", "Ada Brook", "Working Title", length(4, 4), 7},
    Song{"Due Tomorrow", "Tessellate", "Graph", length(5, 50), 8},
    Song{"Reading Room", "Room Tone", "Quiet Machines", length(8, 17), 5},
    Song{"Bookmark", "Ada Brook", "Working Title", length(3, 33), 9},
    Song{"Late Library", "Tessellate", "Graph", length(6, 46), 10},
    Song{"Closing Bell", "Room Tone", "Quiet Machines", length(5, 2), 6},
    Song{"Draft Two", "Ada Brook", "Working Title", length(4, 40), 11},
    Song{"Lamp Off", "Tessellate", "Graph", length(7, 25), 12},
};

inline constexpr std::array kRainyWindow{
    Song{"Gutter Song", "Wren Okafor", "Downpour", length(3, 50), 1},
    Song{"Umbrella Stand", "Wren Okafor", "Downpour", length(4, 18), 2},
    Song{"Puddle Lights", "Sola Marsh", "Overcast", length(5, 1), 3},
};

inline constexpr std::array kDiscovered{
    Song{"Νυχτερινό τρένο", "Ελένη Μαρίνου", "Σταθμοί", length(4, 9), 1},
    Song{"Белые ночи", "Кира Ветрова", "Северный берег", length(3, 52), 2},
    Song{"Mưa đầu mùa", "Nguyễn Thảo", "Phố cũ", length(4, 27), 3},
    Song{"A Song With a Title Long Enough That No Column Could Ever Hold All of It, "
         "Not Even on a Wide Screen",
         "The Band With an Equally Unreasonable Name for a Band", "Live at Somewhere Far Away",
         length(12, 34), 4},
    Song{"Short", "Ö", "Ü", length(0, 45), 5},
};

inline constexpr std::array kPlaylists{
    Playlist{"Late Night Drive", "", kLateNightDrive},
    Playlist{"Sunday Coffee", "", kSundayCoffee},
    Playlist{"Focus, Mostly", "Ada Brook", kFocusMostly},
    Playlist{"Rainy Window", "", kRainyWindow},
    Playlist{"Discovered in 2026", "", kDiscovered},
};

} // namespace am::client::sample
