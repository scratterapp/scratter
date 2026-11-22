// Tests for what the daemon says about its test sources (src/daemon/test_sources.hpp,
// issue #106): the same title, artist, album and artwork go to the desktop's
// media widget and, in a TrackChangedEvent, to am-ui and amctl.

#include "daemon/test_sources.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <string_view>

namespace {

using namespace std::chrono_literals;
using am::common::TrackSource;
using am::daemon::source_info;
using am::daemon::track_changed;
using am::daemon::track_info;

constexpr TrackSource kAllSources[] = {TrackSource::Tone, TrackSource::ClearFilm,
                                       TrackSource::WidevineFilm};

} // namespace

TEST_CASE("the films carry the film's own metadata", "[track-metadata]") {
    for (const TrackSource film : {TrackSource::ClearFilm, TrackSource::WidevineFilm}) {
        const auto info = source_info(film);
        CHECK(info.title == "Tears of Steel");
        CHECK(info.artist == "Blender Foundation");
        CHECK(info.artwork_url == am::daemon::kTestFilmPoster);
        CHECK(info.artwork_url.starts_with("https://"));
    }
    // The album line tells the two films apart.
    CHECK(source_info(TrackSource::ClearFilm).album !=
          source_info(TrackSource::WidevineFilm).album);
}

TEST_CASE("the tone is by the app and has no artwork", "[track-metadata]") {
    const auto info = source_info(TrackSource::Tone);
    CHECK(info.title == "440 Hz test tone");
    CHECK(info.artist == am::common::kAppName);
    CHECK(info.album.empty());
    CHECK(info.artwork_url.empty());
}

TEST_CASE("the desktop and the TrackChangedEvent hear the same thing", "[track-metadata]") {
    for (const TrackSource source : kAllSources) {
        INFO("source " << static_cast<int>(source));
        const auto length = source == TrackSource::Tone ? 0us : 734s;
        const auto desktop = track_info(source, length);
        const auto event = track_changed(source, length);

        CHECK(event.source == source);
        CHECK(event.catalog_id == 0);
        CHECK(event.duration_micros == static_cast<std::uint64_t>(desktop.length.count()));
        CHECK(am::common::track_title(event) == desktop.title);
        CHECK(am::common::track_artist(event) == desktop.artist);
        CHECK(am::common::track_album(event) == desktop.album);
        CHECK(am::common::track_artwork_url(event) == desktop.artwork_url);
        CHECK(am::common::is_valid(event)); // what decode_body() will check on arrival
    }
}
