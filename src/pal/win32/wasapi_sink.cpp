// Windows implementation of am::pal::AudioSink (src/pal/interfaces/audio_sink.hpp)
// on WASAPI, shared or exclusive.
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
// the thread fills whatever part of the buffer has been played since. A shared
// stream can be set up two ways, best first:
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
// An exclusive stream (OutputMode::Exclusive, issue #13) skips the Windows
// mixer: the device runs at exactly the requested rate and channel count, or
// open() fails, and nothing else can play on it until the sink is destroyed.
// Exclusive mode takes only formats the driver itself accepts, often integer
// PCM only, so open() tries 32-bit float, then 32-bit, 24-bit (in 32 bits or
// packed in 3 bytes) and 16-bit integers, and uses the first one the device
// takes. For an integer format the callback renders into a float buffer
// allocated at setup, and fill() converts it into WASAPI's buffer.
//
// Event-driven exclusive streams work in whole periods: the buffer is one
// period long, and each event asks for all of it. The period is the device's
// default (usually 10 ms). Some drivers want the buffer to be a multiple of
// 128 bytes and say AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED; then the period is
// worked out again from the buffer size they suggest and the stream set up
// once more, as Microsoft's documentation for IAudioClient::Initialize says.
//
// The thread registers with MMCSS as "Pro Audio", which is how Windows gives an
// audio thread higher priority without admin rights. If MMCSS is unavailable
// the sink still plays, at normal priority.
//
// The sink follows the default device (issue #94), the way PipeWire moves its
// streams by itself. Two things send the thread to open the default device
// again:
//
//   - Windows says the default changed (IMMNotificationClient). Its callback
//     runs on a thread of Windows' own and only sets an event; the reopen
//     happens here, and only if the default is a different device.
//   - The stream is lost (AUDCLNT_E_DEVICE_INVALIDATED): the device was
//     unplugged, its format was changed in Sound settings, or the audio
//     service restarted. A lost stream may simply stop setting its event, so
//     while the sink plays, a buffer event that's late by far more than a
//     period makes the thread check whether the stream is still there.
//
// The new stream keeps the format open() reported, because the daemon set up
// its audio path and CEF for it. So the IAudioClient3 path is only taken when
// the new mix runs at that rate, and otherwise AUTOCONVERTPCM converts; an
// exclusive sink asks the new device for the same format. If no device takes
// it (none left, or another app holds it in exclusive mode), the sink stays
// quiet and tries again every couple of seconds and on the next change.
// Meanwhile start() and stop() still succeed: they record what was asked, and
// the next stream starts playing if it should.

#include "audio_sink.hpp"
#include "pal/win32/wasapi_sink_testing.hpp"

#include <windows.h>

#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <ksmedia.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

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

// How one sample sits in WASAPI's buffer. Shared mode is always Float32.
enum class SampleType : std::uint8_t {
    Float32,     // what the render callback writes, copied as is
    Int32,       // 32-bit integer, all 32 bits used
    Int24In32,   // 24-bit integer in the top 3 bytes of a 32-bit slot
    Int24Packed, // 24-bit integer in 3 bytes
    Int16,       // 16-bit integer
};

// Best first: the order open() tries them in exclusive mode.
constexpr SampleType kExclusiveTypes[] = {SampleType::Float32, SampleType::Int32,
                                          SampleType::Int24In32, SampleType::Int24Packed,
                                          SampleType::Int16};

struct SampleLayout {
    WORD container_bits; // wBitsPerSample: how much room a sample takes
    WORD valid_bits;     // wValidBitsPerSample: how many of those bits carry it
};

constexpr SampleLayout layout_of(SampleType type) noexcept {
    switch (type) {
    case SampleType::Float32:
    case SampleType::Int32:
        return {32, 32};
    case SampleType::Int24In32:
        return {32, 24};
    case SampleType::Int24Packed:
        return {24, 24};
    case SampleType::Int16:
        return {16, 16};
    }
    return {32, 32};
}

// Interleaved PCM of `type`, in the extensible form exclusive mode needs to
// see the valid bits and the speaker positions.
WAVEFORMATEXTENSIBLE pcm_format(SampleType type, std::uint32_t rate, std::uint32_t channels,
                                DWORD mask) {
    const SampleLayout layout = layout_of(type);
    WAVEFORMATEXTENSIBLE f{};
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = static_cast<WORD>(channels);
    f.Format.nSamplesPerSec = rate;
    f.Format.wBitsPerSample = layout.container_bits;
    f.Format.nBlockAlign = static_cast<WORD>(channels * layout.container_bits / 8);
    f.Format.nAvgBytesPerSec = rate * f.Format.nBlockAlign;
    f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f.Samples.wValidBitsPerSample = layout.valid_bits;
    f.dwChannelMask = mask;
    f.SubFormat =
        type == SampleType::Float32 ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
    return f;
}

// 32-bit float interleaved, which is what the render callback writes.
WAVEFORMATEXTENSIBLE float_format(std::uint32_t rate, std::uint32_t channels, DWORD mask) {
    return pcm_format(SampleType::Float32, rate, channels, mask);
}

// A float sample in [-1, 1] as a signed integer of `bits` bits, rounded to
// the nearest step. Out-of-range input is clipped rather than wrapped around,
// which would be a loud click. Double, because a float can't hold 2^31 - 1
// and 1.0f * 2147483647.0f rounds up past INT32_MAX.
std::int32_t to_int(float sample, int bits) noexcept {
    const double full_scale = static_cast<double>((std::int64_t{1} << (bits - 1)) - 1);
    const double clipped = std::clamp(static_cast<double>(sample), -1.0, 1.0);
    return static_cast<std::int32_t>(std::lround(clipped * full_scale));
}

// Writes `samples` into `out` as `type`. No allocation, no locks, no calls
// into the OS: this runs on the audio thread with the render callback.
void convert(std::span<const float> samples, SampleType type, BYTE* out) noexcept {
    switch (type) {
    case SampleType::Float32:
        std::memcpy(out, samples.data(), samples.size_bytes());
        return;
    case SampleType::Int32:
        for (const float s : samples) {
            const std::int32_t v = to_int(s, 32);
            std::memcpy(out, &v, 4);
            out += 4;
        }
        return;
    case SampleType::Int24In32:
        for (const float s : samples) {
            // Shifted up so the 24 bits sit at the top of the slot, the way
            // the format says, with the bottom byte zero.
            const auto v =
                static_cast<std::int32_t>(static_cast<std::uint32_t>(to_int(s, 24)) << 8);
            std::memcpy(out, &v, 4);
            out += 4;
        }
        return;
    case SampleType::Int24Packed:
        for (const float s : samples) {
            const auto v = static_cast<std::uint32_t>(to_int(s, 24));
            out[0] = static_cast<BYTE>(v & 0xFF);
            out[1] = static_cast<BYTE>((v >> 8) & 0xFF);
            out[2] = static_cast<BYTE>((v >> 16) & 0xFF);
            out += 3;
        }
        return;
    case SampleType::Int16:
        for (const float s : samples) {
            const auto v = static_cast<std::int16_t>(to_int(s, 16));
            std::memcpy(out, &v, 2);
            out += 2;
        }
        return;
    }
}

// 100-nanosecond units (REFERENCE_TIME) for `frames` at `rate`, rounded.
REFERENCE_TIME duration_of(UINT32 frames, std::uint32_t rate) noexcept {
    return static_cast<REFERENCE_TIME>(10'000'000.0 * frames / rate + 0.5);
}

// While the sink plays, the buffer event comes every period, 20 ms at most.
// This long without one, the stream is checked for having been lost.
constexpr DWORD kStalledMs = 500;
// How often a sink with no stream tries to open the default device again.
constexpr DWORD kRetryMs = 2000;

// The errors that mean the stream is gone and only a new one will play.
bool stream_lost(HRESULT hr) noexcept {
    return hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING;
}

// The default render device for music, or null if there's none.
ComPtr<IMMDevice> default_device(IMMDeviceEnumerator* devices, HRESULT& hr) {
    ComPtr<IMMDevice> device;
    // eMultimedia is the role for music; it's the default device unless the
    // user picked another one for it.
    hr = devices->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
    return device;
}

// The device's ID, which stays the same for as long as the device exists.
// Empty if Windows won't say.
std::wstring id_of(IMMDevice* device) {
    LPWSTR id = nullptr;
    if (FAILED(device->GetId(&id))) {
        return {};
    }
    std::wstring copy(id);
    ::CoTaskMemFree(id);
    return copy;
}

// Sets an event when the default device for music changes. Windows calls it
// on a thread of its own, where it must not call back into the device
// enumerator, so it does nothing else. It keeps its own copy of the event,
// since Windows may hold on to it a little after it's unregistered.
class DeviceWatcher final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IMMNotificationClient> {
  public:
    explicit DeviceWatcher(HANDLE changed) noexcept : changed_(changed) {}
    DeviceWatcher(const DeviceWatcher&) = delete;
    DeviceWatcher& operator=(const DeviceWatcher&) = delete;
    ~DeviceWatcher() override { ::CloseHandle(changed_); }

    STDMETHODIMP OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eMultimedia) {
            ::SetEvent(changed_);
        }
        return S_OK;
    }
    STDMETHODIMP OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    STDMETHODIMP OnDeviceAdded(LPCWSTR) override { return S_OK; }
    STDMETHODIMP OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    STDMETHODIMP OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

  private:
    HANDLE changed_;
};

} // namespace

namespace {

// What wasapi_testing::reopen_all() needs from one sink: the event the
// device watcher sets, and a flag saying to reopen even on the same device.
struct ReopenHook {
    HANDLE device_event = nullptr;
    std::atomic<bool> force{false};
};

// The open sinks, for wasapi_testing. Only open() and the destructor change
// the list, never the audio thread.
SRWLOCK g_sinks_lock = SRWLOCK_INIT;
std::vector<ReopenHook*> g_sinks;
std::atomic<std::uint64_t> g_streams_opened{0};

} // namespace

struct AudioSink::Impl {
    enum class Request { None, Start, Stop, Quit };

    AudioFormat requested;
    // What open() reported. A stream opened later, on another device, keeps it.
    AudioFormat format;
    OutputMode mode = OutputMode::Shared;
    RenderCallback render;

    // Only the audio thread touches these.
    ComPtr<IMMDeviceEnumerator> devices;
    ComPtr<DeviceWatcher> watcher; // registered with `devices` while not null
    ComPtr<IAudioClient> client;   // null while there's no stream
    ComPtr<IAudioRenderClient> render_client;
    std::wstring device_id; // the device `client` plays on
    UINT32 buffer_frames = 0;
    SampleType sample_type = SampleType::Float32;
    // Where the callback renders when the device takes integers (exclusive
    // mode only): one buffer's worth, allocated when a stream is set up.
    std::unique_ptr<float[]> scratch;
    std::size_t scratch_samples = 0;
    bool playing = false; // what start() and stop() last asked for
    bool running = false; // the stream is started; implies `client`

    HANDLE thread = nullptr;
    HANDLE buffer_event = nullptr;  // WASAPI: part of the buffer has been played
    HANDLE request_event = nullptr; // start(), stop() or the destructor wants something
    HANDLE done_event = nullptr;    // the audio thread finished setup or a request
    HANDLE device_event = nullptr;  // the default device changed, or a test says so

    ReopenHook hook;
    bool registered = false; // `hook` is in g_sinks

    // Written before request_event or done_event is set and read after waiting
    // for it, so the event orders the two threads' accesses. Setup also fills
    // in `format` before done_event.
    Request request = Request::None;
    std::error_code result;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        if (registered) {
            ::AcquireSRWLockExclusive(&g_sinks_lock);
            std::erase(g_sinks, &hook);
            ::ReleaseSRWLockExclusive(&g_sinks_lock);
        }
        if (thread != nullptr) {
            // A thread whose setup failed is already on its way out and never
            // reads this.
            request = Request::Quit;
            ::SetEvent(request_event);
            ::WaitForSingleObject(thread, INFINITE);
            ::CloseHandle(thread);
        }
        for (HANDLE event : {buffer_event, request_event, done_event, device_event}) {
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

        self->close_stream();
        if (self->watcher) {
            self->devices->UnregisterEndpointNotificationCallback(self->watcher.Get());
            self->watcher.Reset();
        }
        self->devices.Reset();
        if (mmcss != nullptr) {
            ::AvRevertMmThreadCharacteristics(mmcss);
        }
        ::CoUninitialize();
        return 0;
    }

    // The first stream, on the default device. Unlike later ones, it has to
    // work: with no device at all, open() fails.
    std::error_code set_up() {
        HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                        IID_PPV_ARGS(&devices));
        if (FAILED(hr)) {
            return hr_error(hr);
        }
        const ComPtr<IMMDevice> device = default_device(devices.Get(), hr);
        if (FAILED(hr)) {
            return hr_error(hr); // no output device: "Element not found"
        }
        if (const std::error_code error = open_stream(device.Get(), /*first=*/true)) {
            return error;
        }
        watch_devices();
        return {};
    }

    // Asks Windows to tell us when the default device changes. If it won't,
    // the sink still plays, and still recovers a lost stream on the default
    // device; it just doesn't move when a new default is picked.
    void watch_devices() {
        HANDLE copy = nullptr;
        if (!::DuplicateHandle(::GetCurrentProcess(), device_event, ::GetCurrentProcess(), &copy, 0,
                               FALSE, DUPLICATE_SAME_ACCESS)) {
            return;
        }
        watcher = Microsoft::WRL::Make<DeviceWatcher>(copy);
        if (!watcher) {
            ::CloseHandle(copy);
            return;
        }
        if (FAILED(devices->RegisterEndpointNotificationCallback(watcher.Get()))) {
            watcher.Reset();
        }
    }

    // Sets up a stream on `device` in `format`, or leaves `client` null. The
    // first one may pick the rate (the mix rate, in shared mode); later ones
    // keep it.
    std::error_code open_stream(IMMDevice* device, bool first) {
        if (mode == OutputMode::Exclusive) {
            if (const std::error_code error = initialize_exclusive(device)) {
                return error;
            }
        } else if (!initialize_low_latency(device, first)) {
            if (const std::error_code error = initialize_converting(device)) {
                return error;
            }
        }

        HRESULT hr = client->GetBufferSize(&buffer_frames);
        if (SUCCEEDED(hr)) {
            hr = client->SetEventHandle(buffer_event);
        }
        if (SUCCEEDED(hr)) {
            hr = client->GetService(IID_PPV_ARGS(&render_client));
        }
        if (FAILED(hr)) {
            render_client.Reset();
            client.Reset();
            return hr_error(hr);
        }
        // A new device can have a larger buffer than the last. This is stream
        // setup, so the allocation is outside the render callback's rule.
        const std::size_t samples = static_cast<std::size_t>(buffer_frames) * format.channels;
        if (sample_type != SampleType::Float32 && samples > scratch_samples) {
            scratch = std::make_unique<float[]>(samples);
            scratch_samples = samples;
        }
        device_id = id_of(device);
        g_streams_opened.fetch_add(1, std::memory_order_relaxed);
        return {};
    }

    // Exclusive mode: the requested rate and channel count in the best sample
    // type the device takes, at its default period.
    //
    // IsFormatSupported() isn't the last word. Some drivers (Bluetooth
    // headphones, for one) say yes to every format and then refuse each one
    // in Initialize(), so a type only counts once Initialize() takes it, and
    // an "unsupported format" from either moves on to the next type.
    std::error_code initialize_exclusive(IMMDevice* device) {
        const DWORD mask = default_channel_mask(requested.channels);
        // What the last refusal said. Usually "unsupported format", but it
        // can also be that exclusive mode is turned off for the device, or
        // that another app is using it.
        HRESULT refused = AUDCLNT_E_UNSUPPORTED_FORMAT;
        for (const SampleType type : kExclusiveTypes) {
            WAVEFORMATEXTENSIBLE wanted =
                pcm_format(type, requested.sample_rate_hz, requested.channels, mask);
            ComPtr<IAudioClient> fresh;
            HRESULT hr = try_exclusive(device, wanted, fresh);
            if (SUCCEEDED(hr)) {
                client = std::move(fresh);
                sample_type = type; // `format` is already `requested`, from open()
                return {};
            }
            refused = hr;
            if (hr != AUDCLNT_E_UNSUPPORTED_FORMAT) {
                break; // not about the format, so another type won't help
            }
        }
        return exclusive_error(refused);
    }

    // One exclusive Initialize() for `wanted` into `out`, with the retry for
    // a driver that wants an aligned buffer. A client that failed
    // Initialize() can't be initialized again, so each try gets a new one.
    static HRESULT try_exclusive(IMMDevice* device, WAVEFORMATEXTENSIBLE& wanted,
                                 ComPtr<IAudioClient>& out) {
        ComPtr<IAudioClient> fresh;
        HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &fresh);
        if (FAILED(hr)) {
            return hr;
        }
        // In exclusive mode there's no "closest match" to hand back; the
        // answer is just yes (S_OK) or no.
        hr = fresh->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &wanted.Format, nullptr);
        if (hr != S_OK) {
            return FAILED(hr) ? hr : AUDCLNT_E_UNSUPPORTED_FORMAT;
        }
        REFERENCE_TIME default_period = 0;
        REFERENCE_TIME min_period = 0;
        hr = fresh->GetDevicePeriod(&default_period, &min_period);
        if (FAILED(hr)) {
            return hr;
        }
        // Event-driven exclusive streams pass the period twice: as the
        // buffer's length and as how often the event fires.
        hr = fresh->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                               default_period, default_period, &wanted.Format, nullptr);
        if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
            // The size the driver can do, as a period.
            UINT32 aligned_frames = 0;
            hr = fresh->GetBufferSize(&aligned_frames);
            if (FAILED(hr)) {
                return hr;
            }
            const REFERENCE_TIME aligned =
                duration_of(aligned_frames, wanted.Format.nSamplesPerSec);
            fresh.Reset();
            hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &fresh);
            if (SUCCEEDED(hr)) {
                hr = fresh->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                       AUDCLNT_STREAMFLAGS_EVENTCALLBACK, aligned, aligned,
                                       &wanted.Format, nullptr);
            }
        }
        if (SUCCEEDED(hr)) {
            out = std::move(fresh);
        }
        return hr;
    }

    // The errors the AudioSink interface names, from what Initialize said.
    static std::error_code exclusive_error(HRESULT hr) noexcept {
        switch (hr) {
        case AUDCLNT_E_DEVICE_IN_USE:
            // Another app has it in exclusive mode, or is playing through it
            // in shared mode right now.
            return std::make_error_code(std::errc::device_or_resource_busy);
        case AUDCLNT_E_UNSUPPORTED_FORMAT:
        case AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED:
            // The second: "Allow applications to take exclusive control of
            // this device" is off in the device's Sound settings.
            return std::make_error_code(std::errc::not_supported);
        default:
            return hr_error(hr);
        }
    }

    // The IAudioClient3 path. Returns false, leaving `client` empty, if it
    // doesn't apply or doesn't work here, and the caller falls back. Only the
    // first stream may take the mix rate; a later one needs the mix to run at
    // the rate the first one reported.
    bool initialize_low_latency(IMMDevice* device, bool any_rate) {
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
        if (mix_channels != requested.channels ||
            (!any_rate && mix_rate != format.sample_rate_hz)) {
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
        if (any_rate) {
            // Only during open(): later, format() may be read on another
            // thread at any time, and it's already this.
            format = {.sample_rate_hz = mix_rate, .channels = mix_channels};
        }
        return true;
    }

    // The fallback: Windows converts `format` to the device's. For the first
    // stream that's the requested format; for a later one, the first one's.
    std::error_code initialize_converting(IMMDevice* device) {
        ComPtr<IAudioClient> fresh;
        HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &fresh);
        if (FAILED(hr)) {
            return hr_error(hr);
        }
        WAVEFORMATEXTENSIBLE wanted = float_format(format.sample_rate_hz, format.channels,
                                                   default_channel_mask(format.channels));
        hr = fresh->Initialize(AUDCLNT_SHAREMODE_SHARED,
                               AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                   AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                   AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                               0, 0, &wanted.Format, nullptr);
        if (FAILED(hr)) {
            return hr_error(hr);
        }
        client = std::move(fresh);
        return {};
    }

    // The audio thread's loop. Requests come first in the wait list, so a
    // stop() is answered even while the buffer event keeps firing.
    void run() {
        const HANDLE waits[] = {request_event, device_event, buffer_event};
        for (;;) {
            // No stream: try for one now and then. Playing: notice a stream
            // that stopped asking for audio. Paused: nothing to do until asked.
            DWORD timeout = INFINITE;
            if (!client) {
                timeout = kRetryMs;
            } else if (running) {
                timeout = kStalledMs;
            }
            const DWORD woke = ::WaitForMultipleObjects(3, waits, FALSE, timeout);
            if (woke == WAIT_OBJECT_0) {
                if (request == Request::Quit) {
                    return;
                }
                result = request == Request::Start ? start_stream() : stop_stream();
                ::SetEvent(done_event);
            } else if (woke == WAIT_OBJECT_0 + 1) {
                default_changed();
            } else if (woke == WAIT_OBJECT_0 + 2) {
                if (running && !fill(/*silence=*/false)) {
                    reopen();
                }
            } else if (woke == WAIT_TIMEOUT) {
                if (!client) {
                    reopen();
                } else if (UINT32 queued = 0; stream_lost(client->GetCurrentPadding(&queued))) {
                    reopen();
                }
            } else {
                return; // the wait itself failed; ask() notices the thread is gone
            }
        }
    }

    // Windows says the default device changed, which it also says when it
    // only changed for another role or flow, so check that ours did.
    void default_changed() {
        HRESULT hr = S_OK;
        const ComPtr<IMMDevice> device = default_device(devices.Get(), hr);
        if (!device) {
            close_stream(); // no output device left; try again later
            return;
        }
        if (hook.force.exchange(false) || !client || id_of(device.Get()) != device_id) {
            reopen();
        }
    }

    // Drops the stream, if any, and sets one up on the default device, playing
    // if it should be. If that doesn't work, `client` stays null and run()
    // tries again later.
    void reopen() {
        close_stream();
        HRESULT hr = S_OK;
        const ComPtr<IMMDevice> device = default_device(devices.Get(), hr);
        if (!device || open_stream(device.Get(), /*first=*/false)) {
            return;
        }
        if (playing && start_client()) {
            close_stream();
        }
    }

    void close_stream() {
        if (running) {
            client->Stop();
            running = false;
        }
        render_client.Reset();
        client.Reset();
        device_id.clear();
    }

    // Starts `client`. A stream lost in the meantime counts as started: the
    // next one starts instead.
    std::error_code start_client() {
        // Queue a buffer of silence first, so the first period doesn't start
        // with an empty buffer.
        (void)fill(/*silence=*/true);
        if (const HRESULT hr = client->Start(); FAILED(hr)) {
            if (stream_lost(hr)) {
                close_stream();
                return {};
            }
            return hr_error(hr);
        }
        running = true;
        return {};
    }

    std::error_code start_stream() {
        const bool was_playing = playing;
        playing = true;
        if (!client || running) {
            return {}; // with no stream, the next one starts playing
        }
        const std::error_code error = start_client();
        if (error) {
            playing = was_playing;
        }
        return error;
    }

    std::error_code stop_stream() {
        playing = false;
        if (!running) {
            return {};
        }
        running = false;
        HRESULT hr = client->Stop();
        if (SUCCEEDED(hr)) {
            // Drop what's still queued, so starting again doesn't first play
            // the old tail.
            hr = client->Reset();
        }
        if (stream_lost(hr)) {
            close_stream(); // stopped either way
            return {};
        }
        return FAILED(hr) ? hr_error(hr) : std::error_code{};
    }

    // Fills the part of the buffer that has been played. Runs once per period.
    // Returns false if the stream is lost.
    //
    // The no-allocation, no-lock, no-syscall rule covers the render callback;
    // the WASAPI calls around it are Windows' side of the cycle, like
    // PipeWire's on Linux.
    [[nodiscard]] bool fill(bool silence) {
        // Shared: whatever has been played since the last event. Exclusive and
        // event-driven: always the whole buffer, which is one period; WASAPI
        // keeps a second one of its own playing meanwhile.
        UINT32 frames = buffer_frames;
        if (mode == OutputMode::Shared) {
            UINT32 queued = 0;
            if (const HRESULT hr = client->GetCurrentPadding(&queued); FAILED(hr)) {
                return !stream_lost(hr);
            }
            frames = buffer_frames - queued;
        }
        if (frames == 0) {
            return true;
        }
        BYTE* data = nullptr;
        if (const HRESULT hr = render_client->GetBuffer(frames, &data); FAILED(hr)) {
            return !stream_lost(hr);
        }
        if (silence) {
            return !stream_lost(render_client->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT));
        }
        const std::size_t samples = static_cast<std::size_t>(frames) * format.channels;
        if (sample_type == SampleType::Float32) {
            render(std::span<float>(reinterpret_cast<float*>(data), samples));
        } else {
            const std::span<float> rendered(scratch.get(), samples);
            render(rendered);
            convert(rendered, sample_type, data);
        }
        return !stream_lost(render_client->ReleaseBuffer(frames, 0));
    }
};

std::expected<AudioSink, std::error_code> AudioSink::open(const AudioFormat& requested,
                                                          RenderCallback render, OutputMode mode) {
    if (requested.channels == 0 || requested.channels > kMaxChannels ||
        requested.sample_rate_hz == 0) {
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }
    auto impl = std::make_unique<Impl>();
    impl->requested = requested;
    impl->format = requested;
    impl->mode = mode;
    impl->render = std::move(render);

    for (HANDLE* event :
         {&impl->buffer_event, &impl->request_event, &impl->done_event, &impl->device_event}) {
        *event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr); // auto-reset
        if (*event == nullptr) {
            return last_error();
        }
    }
    impl->hook.device_event = impl->device_event;

    impl->thread = ::CreateThread(nullptr, 0, &Impl::thread_main, impl.get(), 0, nullptr);
    if (impl->thread == nullptr) {
        return last_error();
    }
    ::WaitForSingleObject(impl->done_event, INFINITE);
    if (impl->result) {
        return std::unexpected(impl->result); // ~Impl waits for the thread to exit
    }

    ::AcquireSRWLockExclusive(&g_sinks_lock);
    g_sinks.push_back(&impl->hook);
    ::ReleaseSRWLockExclusive(&g_sinks_lock);
    impl->registered = true;
    return AudioSink(std::move(impl));
}

AudioSink::AudioSink(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
AudioSink::~AudioSink() = default;
AudioSink::AudioSink(AudioSink&&) noexcept = default;
AudioSink& AudioSink::operator=(AudioSink&&) noexcept = default;

std::expected<void, std::error_code> AudioSink::start() { return impl_->ask(Impl::Request::Start); }

std::expected<void, std::error_code> AudioSink::stop() { return impl_->ask(Impl::Request::Stop); }

AudioFormat AudioSink::format() const noexcept { return impl_->format; }

OutputMode AudioSink::mode() const noexcept { return impl_->mode; }

namespace wasapi_testing {

void reopen_all() noexcept {
    ::AcquireSRWLockShared(&g_sinks_lock);
    for (ReopenHook* sink : g_sinks) {
        sink->force.store(true);
        ::SetEvent(sink->device_event);
    }
    ::ReleaseSRWLockShared(&g_sinks_lock);
}

std::uint64_t streams_opened() noexcept { return g_streams_opened.load(); }

} // namespace wasapi_testing

} // namespace am::pal
