// TODO: Implement SMTC media controls with C++/WinRT. Owner: @kerelosgayed
//
// Until then this is a stub so the daemon links: open() fails with "not
// supported", so nothing can call the other functions. Replace the whole file
// with the real thing (src/pal/interfaces/media_controls.hpp, issue #9).

#include "media_controls.hpp"

#include <utility>

namespace am::pal {

struct MediaControls::Impl {};

std::expected<MediaControls, std::error_code> MediaControls::open(Reactor&, Callback,
                                                                  std::string_view) {
    return std::unexpected(std::make_error_code(std::errc::function_not_supported));
}

MediaControls::MediaControls(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
MediaControls::~MediaControls() = default;
MediaControls::MediaControls(MediaControls&&) noexcept = default;
MediaControls& MediaControls::operator=(MediaControls&&) noexcept = default;

void MediaControls::set_track(const TrackInfo&) {}
void MediaControls::clear_track() {}
void MediaControls::set_playback_state(common::PlaybackState) {}
void MediaControls::set_position(std::chrono::microseconds) {}
void MediaControls::seeked(std::chrono::microseconds) {}
void MediaControls::set_volume(std::uint8_t) {}

} // namespace am::pal
