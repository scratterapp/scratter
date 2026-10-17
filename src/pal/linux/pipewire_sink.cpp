// Linux implementation of am::pal::AudioSink (src/pal/interfaces/audio_sink.hpp)
// on PipeWire.
//
// PipeWire runs two threads for us:
//
//   - The thread loop ("am-audio"), started here. Setup, start and stop talk
//     to the PipeWire daemon through it, holding its lock.
//   - PipeWire's own data thread, which already runs with real-time priority
//     (PipeWire gets that from rtkit). PW_STREAM_FLAG_RT_PROCESS makes it call
//     on_process() directly, so the render callback runs there.
//
// The stream asks for 32-bit float interleaved at the requested rate. A
// PipeWire playback stream sits behind an adapter that converts and resamples
// to whatever the graph runs at, so it always accepts our format and format()
// can just report what was asked for.

#include "audio_sink.hpp"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>

namespace am::pal {

namespace {

std::unexpected<std::error_code> error_from(int err) {
    return std::unexpected(std::error_code(err, std::system_category()));
}

// PipeWire functions return a negative errno on failure.
std::expected<void, std::error_code> check(int result) {
    if (result < 0) {
        return error_from(-result);
    }
    return {};
}

// RAII for pw_thread_loop_lock / unlock.
class LoopLock {
  public:
    explicit LoopLock(pw_thread_loop* loop) : loop_(loop) { pw_thread_loop_lock(loop_); }
    ~LoopLock() { pw_thread_loop_unlock(loop_); }
    LoopLock(const LoopLock&) = delete;
    LoopLock& operator=(const LoopLock&) = delete;

  private:
    pw_thread_loop* loop_;
};

} // namespace

struct AudioSink::Impl {
    AudioFormat format;
    RenderCallback render;

    pw_thread_loop* loop = nullptr;
    pw_stream* stream = nullptr;
    spa_hook stream_listener{};
    pw_stream_events stream_events{};

    // Handshake between stop() and the audio thread, so that once stop()
    // returns the render callback can't be running. See on_process().
    std::atomic<bool> running{false};
    std::atomic<bool> in_callback{false};

    Impl() { pw_init(nullptr, nullptr); }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        // Stopping the thread loop first means nothing else touches the stream
        // while it's destroyed.
        if (loop != nullptr) {
            pw_thread_loop_stop(loop);
        }
        if (stream != nullptr) {
            pw_stream_destroy(stream);
        }
        if (loop != nullptr) {
            pw_thread_loop_destroy(loop);
        }
        pw_deinit();
    }

    // Runs on the thread loop. open() waits for the first state change to
    // find out whether connecting to the PipeWire daemon worked.
    static void on_state_changed(void* data, pw_stream_state /*old*/, pw_stream_state /*state*/,
                                 const char* /*error*/) {
        auto* self = static_cast<Impl*>(data);
        pw_thread_loop_signal(self->loop, false);
    }

    // Runs on PipeWire's real-time data thread. No allocation, locks or
    // syscalls from here on, in this function or in render().
    static void on_process(void* data) {
        auto* self = static_cast<Impl*>(data);

        pw_buffer* buffer = pw_stream_dequeue_buffer(self->stream);
        if (buffer == nullptr) {
            return; // PipeWire has no free buffer for us this cycle
        }
        spa_data& plane = buffer->buffer->datas[0];
        auto* samples = static_cast<float*>(plane.data);
        if (samples == nullptr) {
            pw_stream_queue_buffer(self->stream, buffer);
            return;
        }

        const std::uint32_t channels = self->format.channels;
        const std::uint32_t stride = static_cast<std::uint32_t>(sizeof(float)) * channels;
        std::uint64_t frames = plane.maxsize / stride;
        if (buffer->requested != 0) {
            // How much the graph wants this cycle. Writing more only adds latency.
            frames = std::min(frames, buffer->requested);
        }
        const std::span<float> out(samples, static_cast<std::size_t>(frames) * channels);

        // in_callback goes up before running is checked. stop() clears running
        // and then waits for in_callback to drop, so either it waits for this
        // call, or this call sees running == false. Both use seq_cst ordering,
        // which is what makes that "either/or" hold.
        self->in_callback.store(true);
        if (self->running.load()) {
            self->render(out);
        } else {
            std::ranges::fill(out, 0.0f);
        }
        self->in_callback.store(false);

        plane.chunk->offset = 0;
        plane.chunk->stride = static_cast<std::int32_t>(stride);
        plane.chunk->size = static_cast<std::uint32_t>(frames) * stride;
        pw_stream_queue_buffer(self->stream, buffer);
    }
};

std::expected<AudioSink, std::error_code> AudioSink::open(const AudioFormat& requested,
                                                          RenderCallback render) {
    if (requested.channels == 0 || requested.channels > SPA_AUDIO_MAX_CHANNELS ||
        requested.sample_rate_hz == 0) {
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }

    auto impl = std::make_unique<Impl>();
    impl->format = requested;
    impl->render = std::move(render);

    impl->loop = pw_thread_loop_new("am-audio", nullptr);
    if (impl->loop == nullptr) {
        return error_from(errno);
    }

    pw_properties* props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",        //
                                             PW_KEY_MEDIA_CATEGORY, "Playback", //
                                             PW_KEY_MEDIA_ROLE, "Music",        //
                                             PW_KEY_APP_NAME, AM_APP_NAME "d",  //
                                             PW_KEY_NODE_NAME, AM_APP_NAME "d", //
                                             nullptr);

    impl->stream_events.version = PW_VERSION_STREAM_EVENTS;
    impl->stream_events.state_changed = &Impl::on_state_changed;
    impl->stream_events.process = &Impl::on_process;

    // pw_stream_new_simple takes ownership of props, even on failure.
    impl->stream = pw_stream_new_simple(pw_thread_loop_get_loop(impl->loop), AM_APP_NAME, props,
                                        &impl->stream_events, impl.get());
    if (impl->stream == nullptr) {
        return error_from(errno);
    }

    // Describe the format as a SPA "pod", PipeWire's binary parameter format.
    std::array<std::uint8_t, 1024> pod_storage{};
    spa_pod_builder builder{};
    spa_pod_builder_init(&builder, pod_storage.data(),
                         static_cast<std::uint32_t>(pod_storage.size()));
    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.rate = requested.sample_rate_hz;
    info.channels = requested.channels;
    if (requested.channels == 2) {
        info.position[0] = SPA_AUDIO_CHANNEL_FL;
        info.position[1] = SPA_AUDIO_CHANNEL_FR;
    } else {
        info.flags = SPA_AUDIO_FLAG_UNPOSITIONED;
    }
    const spa_pod* params[] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info)};

    // INACTIVE: connect now, but don't call on_process until start().
    const auto flags =
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                     PW_STREAM_FLAG_RT_PROCESS | PW_STREAM_FLAG_INACTIVE);
    if (auto connected = check(
            pw_stream_connect(impl->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params, 1));
        !connected) {
        return std::unexpected(connected.error());
    }

    if (auto started = check(pw_thread_loop_start(impl->loop)); !started) {
        return std::unexpected(started.error());
    }

    // Connecting happens on the thread loop. Wait until the stream has either
    // been created on the PipeWire side (PAUSED, since it starts inactive) or
    // failed, so a missing PipeWire daemon is reported here and not ignored.
    {
        const LoopLock lock(impl->loop);
        constexpr int kConnectTimeoutSeconds = 5;
        pw_stream_state state = pw_stream_get_state(impl->stream, nullptr);
        while (state == PW_STREAM_STATE_CONNECTING || state == PW_STREAM_STATE_UNCONNECTED) {
            if (pw_thread_loop_timed_wait(impl->loop, kConnectTimeoutSeconds) != 0) {
                return std::unexpected(std::make_error_code(std::errc::timed_out));
            }
            state = pw_stream_get_state(impl->stream, nullptr);
        }
        if (state == PW_STREAM_STATE_ERROR) {
            return std::unexpected(std::make_error_code(std::errc::connection_refused));
        }
    }

    return AudioSink(std::move(impl));
}

AudioSink::AudioSink(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
AudioSink::~AudioSink() = default;
AudioSink::AudioSink(AudioSink&&) noexcept = default;
AudioSink& AudioSink::operator=(AudioSink&&) noexcept = default;

std::expected<void, std::error_code> AudioSink::start() {
    impl_->running.store(true);
    const LoopLock lock(impl_->loop);
    return check(pw_stream_set_active(impl_->stream, true));
}

std::expected<void, std::error_code> AudioSink::stop() {
    impl_->running.store(false);
    // A callback that started before the store above may still be inside
    // render(). Wait it out; it's a few microseconds at most.
    while (impl_->in_callback.load()) {
        std::this_thread::yield();
    }
    const LoopLock lock(impl_->loop);
    return check(pw_stream_set_active(impl_->stream, false));
}

AudioFormat AudioSink::format() const noexcept { return impl_->format; }

} // namespace am::pal
