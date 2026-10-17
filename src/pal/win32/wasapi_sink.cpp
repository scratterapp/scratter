// TODO: Implement the WASAPI audio sink. Owner: @kerelosgayed
//
// Until then this is a stub so the daemon links on Windows: open() fails with
// "not supported". Replace the whole file with the real sink
// (src/pal/interfaces/audio_sink.hpp, issue #20).

#include "audio_sink.hpp"

#include <utility>

namespace am::pal {

struct AudioSink::Impl {};

std::expected<AudioSink, std::error_code> AudioSink::open(const AudioFormat&, RenderCallback) {
    return std::unexpected(std::make_error_code(std::errc::function_not_supported));
}

AudioSink::AudioSink(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
AudioSink::~AudioSink() = default;
AudioSink::AudioSink(AudioSink&&) noexcept = default;
AudioSink& AudioSink::operator=(AudioSink&&) noexcept = default;

std::expected<void, std::error_code> AudioSink::start() {
    return std::unexpected(std::make_error_code(std::errc::function_not_supported));
}
std::expected<void, std::error_code> AudioSink::stop() { return {}; }
AudioFormat AudioSink::format() const noexcept { return {}; }

} // namespace am::pal
