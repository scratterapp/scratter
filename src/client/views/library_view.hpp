#pragma once

// The library view: playlists down the side, the open one's songs in the
// middle, and a slim player bar along the bottom with what the daemon plays
// now. Its songs are the sample ones (sample_library.hpp) until the MusicKit
// key exists; Play and Shuffle stay greyed out until then, because the daemon
// can only play the tone and the test films (#4).
//
// Like the now-playing view, it never waits on the daemon: play/pause and the
// volume hand a command to the DaemonLink and the bar follows the daemon's own
// state.

#include "client/daemon_link.hpp"
#include "client/library.hpp"
#include "client/theme.hpp"
#include "client/ui_state.hpp"
#include "client/views/now_playing.hpp"
#include "client/views/screen.hpp"

#include <cstddef>
#include <optional>
#include <span>

namespace am::client {

class LibraryView {
  public:
    // `playlists` must outlive the view.
    explicit LibraryView(std::span<const Playlist> playlists) : playlists_(playlists) {}

    // Fills the whole SDL window. Call between ImGui::NewFrame() and
    // ImGui::Render(), only while `state.status` is Connected. "Now playing"
    // in the side bar, or a click on the player bar's title, sets `screen` to
    // Screen::NowPlaying.
    void draw(const Fonts& fonts, const DaemonState& state, const TelemetryReader& telemetry,
              DaemonLink& link, PlayerKeys keys, Screen& screen);

    // Which of `playlists` is shown, for the tests.
    [[nodiscard]] std::size_t open_playlist() const { return open_; }

  private:
    void draw_side_bar(const Fonts& fonts, Screen& screen);
    void draw_playlist(const Fonts& fonts);
    void draw_player_bar(const Fonts& fonts, const DaemonState& state,
                         const TelemetryReader& telemetry, DaemonLink& link, Screen& screen);

    std::span<const Playlist> playlists_;
    std::size_t open_ = 0;              // the playlist shown
    std::optional<std::size_t> picked_; // the song row clicked last, in that playlist
    bool scroll_to_top_ = false;        // a new playlist was opened

    // The volume slider's value while it's dragged, as in NowPlayingView.
    int slider_volume_ = 0;
    bool slider_held_ = false;
};

} // namespace am::client
