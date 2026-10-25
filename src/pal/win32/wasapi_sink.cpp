// Windows implementation of am::pal::AudioSink (src/pal/interfaces/audio_sink.hpp)
// on WASAPI shared mode.
//
// The sink runs one thread of its own, the audio thread, and every WASAPI call
// happens on it: setup during open(), Start and Stop when start() and stop()
// ask for them, and the render loop. Keeping COM on one thread means the
// caller's thread doesn't matter (it may have no COM at all, or be a
// single-threaded apartment because of CEF). It also means stop() needs no
// handshake with the render callback, unlike pipewire_sink.cpp: the thread
// handles the stop request between two renders, so once stop() has its answer
// the callback can't be running.
//
// The stream is event-driven. WASAPI sets an event once per engine period, and
// the thread fills whatever part of the buffer has been played since. There are
// two ways to set the stream up, best first:
//
//   - IAudioClient3::InitializeSharedAudioStream at the engine's minimum
//     period, often 3 ms instead of the default 10 ms. It needs our format to
//     match the mix format's rate and channel count, so this path runs at the
//     mix rate and format() reports it (the daemon then asks CEF for that
//     rate).
//   - Otherwise (a 5.1 mix, say, or no IAudioClient3), IAudioClient::Initialize
//     with AUTOCONVERTPCM, which has Windows convert our format at the
//     requested rate to whatever the device runs, at the default period.
//
// The thread registers with MMCSS as "Pro Audio", which is how Windows gives an
// audio thread higher priority without admin rights. If MMCSS is unavailable
// the sink still plays, at normal priority.
//
// TODO(@kerelosgayed): follow the default device. When it changes or goes away,
// WASAPI invalidates the stream (AUDCLNT_E_DEVICE_INVALIDATED) and the sink
// goes quiet; PipeWire moves its stream to the new device by itself.

#include "audio_sink.hpp"

#include <windows.h>

#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <ksmedia.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>

namespace am::pal {

namespace {

using Microsoft::WRL::ComPtr;

// PipeWire's limit, so both sinks turn away the same requests.
constexpr std::uint32_t kMaxChannels = 64;

std::error_code os_error(DWORD err) noexcept {
    return {static_cast<int>(err), std::system_category()};
}

std::unexpected<std::error_code> last_error() {
    return std::unexpected(os_error(::GetLastError()));
}

// WASAPI reports HRESULTs. One that wraps a Win32 error becomes that error.
// The rest (AUDCLNT_E_*) keep their HRESULT value, which still identifies them,
// though Windows has no message text for most.
std::error_code hr_error(HRESULT hr) noexcept {
    if (HRESULT_FACILITY(hr) == FACILITY_WIN32) {
        return os_error(static_cast<DWORD>(HRESULT_CODE(hr)));
    }
    return {static_cast<int>(hr), std::system_category()};
}

// Speaker positions for a channel count when there's no mix format to copy
// them from. Other counts stay unpositioned.
DWORD default_channel_mask(std::uint32_t channels) noexcept {
    switch (channels) {
    case 1:
        return SPEAKER_FRONT_CENTER;
    case 2:
        return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    default:
        return 0;
    }
}

// 32-bit float interleaved, which is what the render callback writes.
WAVEFORMATEXTENSIBLE float_format(std::uint32_t rate, std::uint32_t channels, DWORD mask) {
    WAVEFORMATEXTENSIBLE f{};
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = static_cast<WORD>(channels);
    f.Format.nSamplesPerSec = rate;
    f.Format.wBitsPerSample = 32;
    f.Format.nBlockAlign = static_cast<WORD>(channels * sizeof(float));
    f.Format.nAvgBytesPerSec = rate * f.Format.nBlockAlign;
    f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f.Samples.wValidBitsPerSample = 32;
    f.dwChannelMask = mask;
    f.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return f;
}

} // namespace

struct AudioSink::Impl {
    enum class Request { None, Start, Stop, Quit };

    AudioFormat requested;
    AudioFormat format;
    RenderCallback render;

    // Only the audio thread touches these.
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render_client;
    UINT32 buffer_frames = 0;
    bool playing = false;

    HANDLE thread = nullptr;
    HANDLE buffer_event = nullptr;  // WASAPI: part of the buffer has been played
    HANDLE request_event = nullptr; // start(), stop() or the destructor wants something
    HANDLE done_event = nullptr;    // the audio thread finished setup or a request

    // Written before request_event or done_event is set and read after waiting
    // for it, so the event orders the two threads' accesses. Setup also fills
    // in `format` before done_event.
    Request request = Request::None;
    std::error_code result;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        if (thread != nullptr) {
            // A thread whose setup failed is already on its way out and never
            // reads this.
            request = Request::Quit;
            ::SetEvent(request_event);
            ::WaitForSingleObject(thread, INFINITE);
            ::CloseHandle(thread);
        }
        for (HANDLE event : {buffer_event, request_event, done_event}) {
            if (event != nullptr) {
                ::CloseHandle(event);
            }
        }
    }

    // Hands a request to the audio thread and waits for its answer.
    std::expected<void, std::error_code> ask(Request what) {
        request = what;
        ::SetEvent(request_event);
        // The thread handle is there so a thread that died can't leave us
        // waiting forever.
        const HANDLE waits[] = {done_event, thread};
        if (::WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0) {
            return std::unexpected(std::make_error_code(std::errc::io_error));
        }
        if (result) {
            return std::unexpected(result);
        }
        return {};
    }

    static DWORD WINAPI thread_main(void* param) {
        auto* self = static_cast<Impl*>(param);

        const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(com)) {
            self->result = hr_error(com);
            ::SetEvent(self->done_event);
            return 0;
        }
        DWORD task_index = 0;
        HANDLE mmcss = ::AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);

        self->result = self->set_up();
        const bool ready = !self->result;
        // open() may destroy *self as soon as it sees a failure, but the
        // destructor waits for this thread to exit first, so the cleanup
        // below is still safe.
        ::SetEvent(self->done_event);
        if (ready) {
            self->run();
        }

        self->render_client.Reset();
        self->client.Reset();
        if (mmcss != nullptr) {
            ::AvRevertMmThreadCharacteristics(mmcss);
        }
        ::CoUninitialize();
        return 0;
    }

    std::error_code set_up() {
        ComPtr<IMMDeviceEnumerator> devices;
        HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                        IID_PPV_ARGS(&devices));
        if (FAILED(hr)) {
            return hr_error(hr);
        }
        // eMultimedia is the role for music; it's the default device unless
        // the user picked another one for it.
        ComPtr<IMMDevice> device;
        hr = devices->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
        if (FAILED(hr)) {
            return hr_error(hr); // no output device: "Element not found"
        }

        if (!initialize_low_latency(device.Get())) {
            if (const std::error_code error = initialize_converting(device.Get())) {
                return error;
            }
        }

        hr = client->GetBufferSize(&buffer_frames);
        if (SUCCEEDED(hr)) {
            hr = client->SetEventHandle(buffer_event);
        }
        if (SUCCEEDED(hr)) {
            hr = client->GetService(IID_PPV_ARGS(&render_client));
        }
        return FAILED(hr) ? hr_error(hr) : std::error_code{};
    }

    // The IAudioClient3 path. Returns false, leaving `client` empty, if it
    // doesn't apply or doesn't work here, and the caller falls back.
    bool initialize_low_latency(IMMDevice* device) {
        ComPtr<IAudioClient3> client3;
        if (FAILED(device->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, &client3))) {
            return false;
        }
        WAVEFORMATEX* mix = nullptr;
        if (FAILED(client3->GetMixFormat(&mix))) {
            return false;
        }
        const std::uint32_t mix_rate = mix->nSamplesPerSec;
        const std::uint32_t mix_channels = mix->nChannels;
        DWORD mask = default_channel_mask(mix_channels);
        if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            mask = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix)->dwChannelMask;
        }
        ::CoTaskMemFree(mix);
        if (mix_channels != requested.channels) {
            return false;
        }

        WAVEFORMATEXTENSIBLE wanted = float_format(mix_rate, mix_channels, mask);
        UINT32 default_period = 0;
        UINT32 fundamental_period = 0;
        UINT32 min_period = 0;
        UINT32 max_period = 0;
        if (FAILED(client3->GetSharedModeEnginePeriod(
                &wanted.Format, &default_period, &fundamental_period, &min_period, &max_period)) ||
            FAILED(client3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                                        min_period, &wanted.Format, nullptr))) {
            return false;
        }
        client = std::move(client3);
        format = {.sample_rate_hz = mix_rate, .channels = mix_channels};
        return true;
    }

    // The fallback: Windows converts the requested format to the device's.
    std::error_code initialize_converting(IMMDevice* device) {
        ComPtr<IAudioClient> fresh;
        HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &fresh);
        if (FAILED(hr)) {
            return hr_error(hr);
        }
        WAVEFORMATEXTENSIBLE wanted = float_format(requested.sample_rate_hz, requested.channels,
                                                   default_channel_mask(requested.channels));
        hr = fresh->Initialize(AUDCLNT_SHAREMODE_SHARED,
                               AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                   AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                   AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                               0, 0, &wanted.Format, nullptr);
        if (FAILED(hr)) {
            return hr_error(hr);
        }
        client = std::move(fresh);
        format = requested;
        return {};
    }

    // The audio thread's loop. Requests come first in the wait list, so a
    // stop() is answered even while the buffer event keeps firing.
    void run() {
        const HANDLE waits[] = {request_event, buffer_event};
        for (;;) {
            const DWORD woke = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (woke == WAIT_OBJECT_0) {
                if (request == Request::Quit) {
                    if (playing) {
                        client->Stop();
                    }
                    return;
                }
                result = request == Request::Start ? start_stream() : stop_stream();
                ::SetEvent(done_event);
            } else if (woke == WAIT_OBJECT_0 + 1) {
                if (playing) {
                    fill(/*silence=*/false);
                }
            } else {
                return; // the wait itself failed; ask() notices the thread is gone
            }
        }
    }

    std::error_code start_stream() {
        if (playing) {
            return {};
        }
        // Queue a buffer of silence first, so the first period doesn't start
        // with an empty buffer.
        fill(/*silence=*/true);
        if (const HRESULT hr = client->Start(); FAILED(hr)) {
            return hr_error(hr);
        }
        playing = true;
        return {};
    }

    std::error_code stop_stream() {
        if (!playing) {
            return {};
        }
        playing = false;
        HRESULT hr = client->Stop();
        if (SUCCEEDED(hr)) {
            // Drop what's still queued, so starting again doesn't first play
            // the old tail.
            hr = client->Reset();
        }
        return FAILED(hr) ? hr_error(hr) : std::error_code{};
    }

    // Fills the part of the buffer that has been played. Runs once per period.
    // The no-allocation, no-lock, no-syscall rule covers the render callback;
    // the WASAPI calls around it are Windows' side of the cycle, like
    // PipeWire's on Linux.
    void fill(bool silence) {
        UINT32 queued = 0;
        if (FAILED(client->GetCurrentPadding(&queued))) {
            return; // the device went away (see the TODO at the top)
        }
        const UINT32 frames = buffer_frames - queued;
        if (frames == 0) {
            return;
        }
        BYTE* data = nullptr;
        if (FAILED(render_client->GetBuffer(frames, &data))) {
            return;
        }
        if (silence) {
            render_client->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT);
            return;
        }
        render(std::span<float>(reinterpret_cast<float*>(data),
                                static_cast<std::size_t>(frames) * format.channels));
        render_client->ReleaseBuffer(frames, 0);
    }
};

std::expected<AudioSink, std::error_code> AudioSink::open(const AudioFormat& requested,
                                                          RenderCallback render) {
    if (requested.channels == 0 || requested.channels > kMaxChannels ||
        requested.sample_rate_hz == 0) {
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }

    auto impl = std::make_unique<Impl>();
    impl->requested = requested;
    impl->format = requested;
    impl->render = std::move(render);

    for (HANDLE* event : {&impl->buffer_event, &impl->request_event, &impl->done_event}) {
        *event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr); // auto-reset
        if (*event == nullptr) {
            return last_error();
        }
    }

    impl->thread = ::CreateThread(nullptr, 0, &Impl::thread_main, impl.get(), 0, nullptr);
    if (impl->thread == nullptr) {
        return last_error();
    }
    ::WaitForSingleObject(impl->done_event, INFINITE);
    if (impl->result) {
        return std::unexpected(impl->result); // ~Impl waits for the thread to exit
    }
    return AudioSink(std::move(impl));
}

AudioSink::AudioSink(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
AudioSink::~AudioSink() = default;
AudioSink::AudioSink(AudioSink&&) noexcept = default;
AudioSink& AudioSink::operator=(AudioSink&&) noexcept = default;

std::expected<void, std::error_code> AudioSink::start() { return impl_->ask(Impl::Request::Start); }

std::expected<void, std::error_code> AudioSink::stop() { return impl_->ask(Impl::Request::Stop); }

AudioFormat AudioSink::format() const noexcept { return impl_->format; }

} // namespace am::pal
