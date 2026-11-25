#pragma once

// What the daemon says about its own test sources (issue #106): the title,
// artist, album and cover art that the desktop's media widget (MPRIS, SMTC),
// amctl and the window show.
//
// Apple Music tracks won't come from here. With MusicKit (#4) the page knows
// what plays and tells the daemon, which hands it on the same two ways: to
// MediaControls::set_track() and in a TrackChangedEvent. Until then this table
// stands in for the page, so the whole path can be tried, and tested, without
// CEF.

#include "common/app_name.hpp"
#include "common/ipc_messages.hpp"
#include "media_controls.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace am::daemon {

// The film's poster, from Wikimedia Commons (CC BY 3.0, Blender Foundation).
// The desktop fetches it itself; the daemon has no HTTP.
inline constexpr std::string_view kTestFilmPoster =
    "https://upload.wikimedia.org/wikipedia/commons/thumb/7/70/Tos-poster.png/330px-Tos-poster.png";

struct SourceInfo {
    std::string_view title;
    std::string_view artist;
    std::string_view album; // for the films, which of the two it is
    std::string_view artwork_url;
};

[[nodiscard]] constexpr SourceInfo source_info(common::TrackSource source) noexcept {
    switch (source) {
    case common::TrackSource::Tone:
        return {.title = "440 Hz test tone",
                .artist = common::kAppName,
                .album = {},
                .artwork_url = {}};
    case common::TrackSource::ClearFilm:
        return {.title = "Tears of Steel",
                .artist = "Blender Foundation",
                .album = "Unencrypted test film",
                .artwork_url = kTestFilmPoster};
    case common::TrackSource::WidevineFilm:
        return {.title = "Tears of Steel",
                .artist = "Blender Foundation",
                .album = "Widevine test film",
                .artwork_url = kTestFilmPoster};
    }
    return {};
}

// For the media widget. `length` is 0 for the tone, which has no end.
[[nodiscard]] inline pal::TrackInfo track_info(common::TrackSource source,
                                               std::chrono::microseconds length) {
    const SourceInfo info = source_info(source);
    return {.title = std::string(info.title),
            .artist = std::string(info.artist),
            .album = std::string(info.album),
            .artwork_url = std::string(info.artwork_url),
            .length = length};
}

// For the TrackChangedEvent that tells am-ui and amctl.
[[nodiscard]] constexpr common::TrackChangedBody
track_changed(common::TrackSource source, std::chrono::microseconds length) noexcept {
    const SourceInfo info = source_info(source);
    return {
        .catalog_id = 0,
        .duration_micros = static_cast<std::uint64_t>(length.count()),
        .source = source,
        .reserved = {},
        .title = common::make_text_field<common::kTrackTitleBytes>(info.title),
        .artist = common::make_text_field<common::kTrackArtistBytes>(info.artist),
        .album = common::make_text_field<common::kTrackAlbumBytes>(info.album),
        .artwork_url = common::make_url_field(info.artwork_url),
    };
}

// The link has to fit whole, or make_url_field() would leave it out.
static_assert(kTestFilmPoster.size() <= common::kTrackArtworkUrlBytes);

} // namespace am::daemon
