// Tests for the daemon's audio path (src/daemon/audio_engine/audio_path.hpp):
// the render callback's play, pause, stop, fade and volume behavior, called
// directly the way the audio thread would, without a sound card.

#include "daemon/audio_engine/audio_path.hpp"
#include "daemon/audio_engine/sine_generator.hpp"
#include "daemon/audio_engine/tone_pacer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <memory>
#include <span>
#include <thread>
#include <vector>

namespace {

using am::audio::AudioPath;
using am::audio::PlaybackState;
using am::audio::SineGenerator;

constexpr std::uint32_t kRate = 48000;
constexpr std::size_t kBufferFrames = 256;

float loudest(const std::vector<float>& samples) {
    float peak = 0.0f;
    for (float s : samples) {
        peak = std::max(peak, std::abs(s));
    }
    return peak;
}

// The biggest change from one left-channel sample to the next. A click is a
// big jump; a fade over 480 frames of a constant 0.5 moves about 0.001 a frame.
float biggest_jump(const std::vector<float>& samples) {
    float jump = 0.0f;
    for (std::size_t i = AudioPath::kChannels; i < samples.size(); i += AudioPath::kChannels) {
        jump = std::max(jump, std::abs(samples[i] - samples[i - AudioPath::kChannels]));
    }
    return jump;
}

// Buffers until a fade has finished, rounded up.
constexpr int kFadeBuffers =
    static_cast<int>((kRate * AudioPath::kFadeMillis / 1000 + kBufferFrames - 1) / kBufferFrames);

// One AudioPath with a full ring buffer, and a buffer for render() to fill.
struct Fixture {
    std::unique_ptr<AudioPath> audio = std::make_unique<AudioPath>(kRate);
    SineGenerator sine{kRate};
    std::vector<float> out = std::vector<float>(kBufferFrames * AudioPath::kChannels);

    Fixture() { audio->top_up(sine); }

    // Renders `buffers` buffers, topping the ring up in between like the
    // daemon's tick does.
    void render(int buffers = 1) {
        for (int i = 0; i < buffers; ++i) {
            audio->render(out);
            audio->top_up(sine);
        }
    }
};

} // namespace

TEST_CASE("top_up fills the ring buffer to the target and no further", "[audio_path]") {
    auto audio = std::make_unique<AudioPath>(kRate);
    SineGenerator sine(kRate);
    CHECK(audio->top_up(sine) == AudioPath::kTargetFrames);
    CHECK(audio->top_up(sine) == 0);
}

TEST_CASE("write_tone writes what it's asked for, up to a full ring", "[audio_path]") {
    auto audio = std::make_unique<AudioPath>(kRate);
    SineGenerator sine(kRate);
    CHECK(audio->write_tone(sine, 1000) == 1000); // more than one packet, not a multiple
    CHECK(audio->write_tone(sine, 0) == 0);
    CHECK(audio->dropped_frames() == 0);
    // Past the target, unlike top_up(), and no further than the capacity.
    // What doesn't fit is dropped and counted, like write_planar()'s.
    CHECK(audio->write_tone(sine, AudioPath::kCapacityFrames) == AudioPath::kCapacityFrames - 1000);
    CHECK(audio->dropped_frames() == 1000);
    CHECK(audio->write_tone(sine, 10) == 0);
    CHECK(audio->dropped_frames() == 1010);
}

TEST_CASE("the tone pacer counts frames by the clock, without rounding drift", "[audio_path]") {
    using am::audio::TonePacer;
    using namespace std::chrono_literals;
    TonePacer pacer(kRate);
    const TonePacer::Clock::time_point t0{};

    CHECK_FALSE(pacer.running());
    CHECK(pacer.due(t0 + 1s) == 0); // not started

    pacer.start(t0);
    CHECK(pacer.due(t0) == 0);
    CHECK(pacer.due(t0 + 10ms) == 480);
    CHECK(pacer.due(t0 + 10ms) == 0); // asked twice for the same moment
    // 1/3 ms is 16 frames exactly at 48 kHz; 7 µs steps are 0.336 frames
    // each. Whatever the steps, the total is the elapsed time times the rate.
    std::size_t total = 480;
    auto now = t0 + 10ms;
    for (int i = 0; i < 1000; ++i) {
        now += 7us;
        total += pacer.due(now);
    }
    CHECK(total == 480 + 336); // 7 ms
    CHECK(pacer.due(t0 + 3600s) == 3600u * kRate - total);

    pacer.stop();
    CHECK(pacer.due(t0 + 3601s) == 0);
}

namespace {

// Plays the tone the way the daemon does, for `seconds` simulated seconds, and
// returns the drift the controller settled on. Each 10 ms tick the producer
// writes, then the sound card asks `callbacks` times for 480 / callbacks
// frames (one 10 ms exclusive period, or a shared stream's shorter ones).
// `producer_ppm` is how far the tone's clock runs from the sound card's: the
// producer writes that much more (or less) than 480 frames a tick. `paced`
// writes the tone by the clock; otherwise it's topped up to the target each
// tick, the old way.
float settled_drift_ppm(bool paced, double producer_ppm, int callbacks, int seconds) {
    auto audio = std::make_unique<AudioPath>(kRate);
    SineGenerator sine(kRate);
    std::vector<float> out(static_cast<std::size_t>(480 / callbacks) * AudioPath::kChannels);
    audio->top_up(sine);
    double owed = 0.0;
    for (int tick = 0; tick < seconds * 100; ++tick) {
        if (paced) {
            owed += 480.0 * (1.0 + producer_ppm * 1e-6);
            const auto frames = static_cast<std::size_t>(owed);
            owed -= static_cast<double>(audio->write_tone(sine, frames));
        } else {
            audio->top_up(sine);
        }
        for (int i = 0; i < callbacks; ++i) {
            audio->render(out);
        }
    }
    return audio->drift_ppm();
}

} // namespace

TEST_CASE("a tone written by the clock lets the drift controller settle (#85)", "[audio_path]") {
    for (const int callbacks : {1, 4}) {
        CAPTURE(callbacks);
        // Same clocks: nothing to correct.
        CHECK(std::abs(settled_drift_ppm(true, 0.0, callbacks, 120)) < 20.0f);
        // The tone's clock 100 ppm slow: the sound card has to play 100 ppm
        // slower.
        CHECK(std::abs(settled_drift_ppm(true, -100.0, callbacks, 120) + 100.0f) < 20.0f);
    }
    // Topping up each tick doesn't let it settle: the fill render() sees is
    // under the target for every callback after the first, and the integral
    // runs toward the -1000 ppm clamp. This is what amctl showed (#85).
    CHECK(settled_drift_ppm(false, 0.0, 4, 120) < -500.0f);
}

TEST_CASE("playing renders the tone and moves the position", "[audio_path]") {
    Fixture f;
    REQUIRE(f.audio->state() == PlaybackState::Playing); // the daemon starts playing
    f.render(10);

    // The resampler's 8-frame delay means the first buffer starts with a few
    // zeros, so look at a later one.
    CHECK(loudest(f.out) > 0.9f * SineGenerator::kLevel);
    CHECK(loudest(f.out) <= 1.01f * SineGenerator::kLevel);
    CHECK(f.audio->position_frames() == 10 * kBufferFrames);
    CHECK(f.audio->late_buffers() == 0);
    CHECK(f.audio->ring_fill() > 0.0f);
}

TEST_CASE("pause renders silence and keeps the position", "[audio_path]") {
    Fixture f;
    f.render(4);
    const auto position = f.audio->position_frames();

    // It fades out first, playing on for those few buffers, then stays put.
    f.audio->set_state(PlaybackState::Paused);
    f.render(kFadeBuffers);
    CHECK(f.audio->position_frames() == position + kFadeBuffers * kBufferFrames);
    const auto paused_at = f.audio->position_frames();
    f.render(4);
    CHECK(loudest(f.out) == 0.0f);
    CHECK(f.audio->position_frames() == paused_at);

    f.audio->set_state(PlaybackState::Playing);
    f.render(2);
    CHECK(loudest(f.out) > 0.9f * SineGenerator::kLevel);
    CHECK(f.audio->position_frames() == paused_at + 2 * kBufferFrames);
}

TEST_CASE("stop renders silence and rewinds the position", "[audio_path]") {
    Fixture f;
    f.render(4);
    f.audio->set_state(PlaybackState::Stopped);
    f.render(kFadeBuffers + 1); // fade out, then one silent buffer
    CHECK(loudest(f.out) == 0.0f);
    CHECK(f.audio->position_frames() == 0);
}

TEST_CASE("a seek moves the position, and render() carries on from there", "[audio_path]") {
    Fixture f;
    f.render(4);
    f.audio->set_state(PlaybackState::Paused);
    f.render(kFadeBuffers + 1);
    REQUIRE(f.audio->quiet());

    // While quiet, render() leaves the position alone, so the seek sticks.
    f.audio->set_position_frames(10 * kRate);
    f.render(2);
    CHECK(f.audio->position_frames() == 10 * kRate);

    f.audio->set_state(PlaybackState::Playing);
    f.render(2);
    CHECK(f.audio->position_frames() == 10 * kRate + 2 * kBufferFrames);
}

TEST_CASE("a source with a length goes round to 0 at its end", "[audio_path]") {
    Fixture f;
    CHECK(f.audio->length_frames() == 0);
    f.audio->set_length_frames(3 * kBufferFrames);
    f.render(2);
    CHECK(f.audio->position_frames() == 2 * kBufferFrames);
    f.render(2); // 4 buffers into a 3-buffer source
    CHECK(f.audio->position_frames() == kBufferFrames);

    // A seek to the very end is the start again.
    f.audio->set_position_frames(3 * kBufferFrames);
    CHECK(f.audio->position_frames() == 0);

    // No length: no end.
    f.audio->set_length_frames(0);
    CHECK(f.audio->position_frames() == 3 * kBufferFrames);
}

TEST_CASE("volume scales the output", "[audio_path]") {
    Fixture f;
    f.render(4);
    const float full = loudest(f.out);

    f.audio->set_volume(50);
    f.render(4);
    CHECK(std::abs(loudest(f.out) - full / 2) < 0.01f * full);

    f.audio->set_volume(0);
    f.render();
    CHECK(loudest(f.out) == 0.0f);
}

TEST_CASE("render waits for the ring buffer to fill before playing", "[audio_path]") {
    auto audio = std::make_unique<AudioPath>(kRate); // never topped up
    std::vector<float> out(kBufferFrames * AudioPath::kChannels, 1.0f);
    audio->render(out);
    CHECK(loudest(out) == 0.0f);
    CHECK(audio->late_buffers() == 0); // filling up isn't late
    CHECK(audio->position_frames() == 0);

    // Just short of the target: still waiting.
    SineGenerator sine(kRate);
    std::vector<float> tone((AudioPath::kTargetFrames - 1) * AudioPath::kChannels);
    sine.fill(tone);
    const float* left = tone.data();
    audio->write_planar(std::span(&left, 1), AudioPath::kTargetFrames - 1);
    audio->render(out);
    CHECK(audio->position_frames() == 0);

    // At the target it starts.
    audio->top_up(sine);
    audio->render(out);
    CHECK(audio->position_frames() == kBufferFrames);
}

TEST_CASE("running dry counts a late buffer, then waits to fill again", "[audio_path]") {
    Fixture f;
    f.render(4); // playing normally
    std::vector<float> big(2 * AudioPath::kTargetFrames * AudioPath::kChannels);
    f.audio->render(big); // more than the ring holds
    CHECK(f.audio->late_buffers() == 1);

    // A little audio arrives, but not enough to start again.
    std::vector<float> some(kBufferFrames * AudioPath::kChannels);
    SineGenerator sine(kRate);
    sine.fill(some);
    const float* channels[] = {some.data(), some.data()};
    f.audio->write_planar(channels, kBufferFrames / 2);
    const auto position = f.audio->position_frames();
    f.audio->render(f.out);
    CHECK(loudest(f.out) == 0.0f);
    CHECK(f.audio->position_frames() == position);
    CHECK(f.audio->late_buffers() == 1);
}

TEST_CASE("write_planar interleaves the channels", "[audio_path]") {
    auto audio = std::make_unique<AudioPath>(kRate);
    // Left is a constant 0.25 and right a constant -0.5, enough of it to start
    // playing. The resampler passes a constant through unchanged.
    std::vector<float> left(AudioPath::kTargetFrames, 0.25f);
    std::vector<float> right(AudioPath::kTargetFrames, -0.5f);
    const float* channels[] = {left.data(), right.data()};
    CHECK(audio->write_planar(channels, AudioPath::kTargetFrames) == AudioPath::kTargetFrames);

    std::vector<float> out(kBufferFrames * AudioPath::kChannels);
    for (int i = 0; i <= kFadeBuffers; ++i) {
        audio->render(out); // past the resampler's start-up delay and the fade-in
    }
    for (std::size_t i = 0; i < out.size(); i += 2) {
        CHECK(std::abs(out[i] - 0.25f) < 1e-4f);
        CHECK(std::abs(out[i + 1] + 0.5f) < 1e-4f);
    }
}

TEST_CASE("write_planar drops what doesn't fit and counts it", "[audio_path]") {
    auto audio = std::make_unique<AudioPath>(kRate);
    std::vector<float> silence(AudioPath::kCapacityFrames + 100);
    const float* channels[] = {silence.data(), silence.data()};
    CHECK(audio->write_planar(channels, silence.size()) == AudioPath::kCapacityFrames);
    CHECK(audio->dropped_frames() == 100);
    CHECK(audio->write_planar(channels, 10) == 0);
    CHECK(audio->dropped_frames() == 110);
}

TEST_CASE("pause throws away what the page sends and refills on play", "[audio_path]") {
    auto audio = std::make_unique<AudioPath>(kRate);
    std::vector<float> page(AudioPath::kTargetFrames, 0.5f);
    const float* channels[] = {page.data(), page.data()};
    audio->write_planar(channels, page.size());
    std::vector<float> out(kBufferFrames * AudioPath::kChannels);
    audio->render(out);
    audio->render(out);
    CHECK(loudest(out) > 0.0f);

    // The page keeps sending for a while after the pause; none of it is kept.
    // write_planar() drops it without counting it as dropped, because while
    // the daemon has the sink stopped, nothing else would empty the ring.
    audio->set_state(PlaybackState::Paused);
    for (int i = 0; i <= kFadeBuffers; ++i) {
        audio->render(out);
    }
    CHECK(audio->write_planar(channels, page.size()) == 0);
    audio->render(out);
    CHECK(loudest(out) == 0.0f);

    // On play, nothing stale is left: it waits for the ring to fill again,
    // then plays without running dry.
    audio->set_state(PlaybackState::Playing);
    audio->render(out);
    CHECK(loudest(out) == 0.0f);
    audio->write_planar(channels, page.size());
    audio->render(out);
    audio->render(out);
    CHECK(loudest(out) > 0.0f);
    CHECK(audio->late_buffers() == 0);
    CHECK(audio->dropped_frames() == 0);
}

TEST_CASE("quiet() turns true once a pause has faded out, and false on play", "[audio_path]") {
    Fixture f;
    f.render(4);
    CHECK_FALSE(f.audio->quiet()); // playing

    f.audio->set_state(PlaybackState::Paused);
    CHECK_FALSE(f.audio->quiet()); // render() hasn't seen the pause yet
    f.render(kFadeBuffers + 1);    // fade out, then one silent buffer
    CHECK(f.audio->quiet());
    f.audio->set_state(PlaybackState::Paused); // the same again changes nothing
    CHECK(f.audio->quiet());

    // The daemon starts the sink again as soon as this turns false.
    f.audio->set_state(PlaybackState::Playing);
    CHECK_FALSE(f.audio->quiet());
    f.render();
    CHECK_FALSE(f.audio->quiet());
}

TEST_CASE("a stop after a pause isn't quiet until render() has rewound", "[audio_path]") {
    Fixture f;
    f.render(4);
    f.audio->set_state(PlaybackState::Paused);
    f.render(kFadeBuffers + 1);
    REQUIRE(f.audio->quiet());
    REQUIRE(f.audio->position_frames() > 0);

    // By now the daemon has stopped the sink. The stop has to start it again,
    // so that render() sees it and rewinds the position.
    f.audio->set_state(PlaybackState::Stopped);
    CHECK_FALSE(f.audio->quiet());
    f.audio->render(f.out);
    CHECK(f.audio->quiet());
    CHECK(f.audio->position_frames() == 0);
}

TEST_CASE("a fade lasts 10 ms", "[audio_path]") {
    CHECK(std::make_unique<AudioPath>(48000)->fade_frames() == 480);
    CHECK(std::make_unique<AudioPath>(44100)->fade_frames() == 441);
}

// A page that sends a constant 0.5 on both channels. Without the fade, play
// would jump from 0 to 0.5 and pause from 0.5 to 0 in one sample.
struct ConstantPage {
    std::unique_ptr<AudioPath> audio = std::make_unique<AudioPath>(kRate);
    std::vector<float> page = std::vector<float>(AudioPath::kTargetFrames, 0.5f);
    std::vector<float> out = std::vector<float>(kBufferFrames * AudioPath::kChannels);
    std::vector<float> heard; // everything render() wrote, in order

    ConstantPage() { send(AudioPath::kTargetFrames); }

    void send(std::size_t frames) {
        const float* channels[] = {page.data(), page.data()};
        audio->write_planar(channels, frames);
    }

    // Renders `buffers` buffers, sending as much as each one plays the way a
    // page in real time would, and keeps what they played.
    void render(int buffers) {
        for (int i = 0; i < buffers; ++i) {
            send(kBufferFrames);
            audio->render(out);
            heard.insert(heard.end(), out.begin(), out.end());
        }
    }
};

TEST_CASE("play fades in instead of jumping to full volume", "[audio_path]") {
    ConstantPage page;
    page.render(kFadeBuffers + 2);
    CHECK(std::abs(page.heard[0]) < 0.01f); // starts near silence
    CHECK(biggest_jump(page.heard) < 0.01f);
    CHECK(std::abs(page.out[0] - 0.5f) < 1e-4f); // and ends at full volume
}

TEST_CASE("pause fades out, and play fades back in", "[audio_path]") {
    ConstantPage page;
    page.render(kFadeBuffers + 2);
    page.audio->set_state(PlaybackState::Paused);
    page.render(kFadeBuffers + 2);
    CHECK(loudest(page.out) == 0.0f); // silent once the fade is done

    page.audio->set_state(PlaybackState::Playing);
    page.send(AudioPath::kTargetFrames); // the page starts sending again
    page.render(kFadeBuffers + 2);
    CHECK(std::abs(page.out[0] - 0.5f) < 1e-4f);
    CHECK(biggest_jump(page.heard) < 0.01f); // no click anywhere along the way
    CHECK(page.audio->late_buffers() == 0);
}

TEST_CASE("play during a fade-out fades straight back in", "[audio_path]") {
    ConstantPage page;
    page.render(kFadeBuffers + 2);
    page.audio->set_state(PlaybackState::Paused);
    page.render(1); // part way down
    CHECK(loudest(page.out) > 0.0f);
    CHECK(loudest(page.out) < 0.5f);

    page.audio->set_state(PlaybackState::Playing);
    page.render(kFadeBuffers + 1);
    CHECK(std::abs(page.out[0] - 0.5f) < 1e-4f);
    CHECK(biggest_jump(page.heard) < 0.01f);
}

TEST_CASE("stop fades out too", "[audio_path]") {
    ConstantPage page;
    page.render(kFadeBuffers + 2);
    page.audio->set_state(PlaybackState::Stopped);
    page.render(kFadeBuffers + 1);
    CHECK(loudest(page.out) == 0.0f);
    CHECK(biggest_jump(page.heard) < 0.01f);
    CHECK(page.audio->position_frames() == 0);
}

// A page playing two tracks back to back in real time, the way CEF sends
// audio: a 512-frame packet every 512 frames of time, while the sound card
// asks for `sink_frames` at a time on its own schedule. Track one is a
// constant 0.5 for a second, track two a constant 0.3. Between them the page
// can go quiet for `break_frames` and send nothing at all, the way Chromium
// would if its audio stream stopped and started again at the track change.
//
// Returns the longest stretch of silence render() wrote once the music had
// started, in frames. The tracks never touch zero, so any silence is a gap.
struct TrackChange {
    std::size_t longest_silence = 0;
    std::uint64_t late_buffers = 0;
    float last_sample = 0.0f;
};

TrackChange play_track_change(std::size_t sink_frames, std::size_t break_frames) {
    auto audio = std::make_unique<AudioPath>(kRate);
    std::vector<float> packet(AudioPath::kPacketFrames);
    std::vector<float> out(sink_frames * AudioPath::kChannels);
    const std::size_t track_frames = kRate; // one second
    std::size_t sent = 0;
    bool started = false;
    std::size_t silence = 0;
    TrackChange result;

    for (std::size_t now = 0; now < 3 * kRate; ++now) {
        const bool on_break = now >= track_frames && now < track_frames + break_frames;
        if (now % AudioPath::kPacketFrames == 0 && !on_break) {
            std::ranges::fill(packet, sent < track_frames ? 0.5f : 0.3f);
            const float* channels[] = {packet.data(), packet.data()};
            sent += audio->write_planar(channels, packet.size());
        }
        if (now % sink_frames == sink_frames - 1) {
            audio->render(out);
            for (std::size_t i = 0; i < out.size(); i += AudioPath::kChannels) {
                const bool quiet = std::abs(out[i]) < 1e-3f;
                started = started || !quiet;
                silence = started && quiet ? silence + 1 : 0;
                result.longest_silence = std::max(result.longest_silence, silence);
            }
        }
    }
    result.late_buffers = audio->late_buffers();
    result.last_sample = out[out.size() - AudioPath::kChannels];
    return result;
}

TEST_CASE("a page that keeps sending plays into the next track without a gap", "[audio_path]") {
    for (std::size_t sink_frames : {256u, 1024u}) {
        INFO("sound card buffer " << sink_frames << " frames");
        const auto heard = play_track_change(sink_frames, 0);
        CHECK(heard.longest_silence == 0);
        CHECK(heard.late_buffers == 0);
        CHECK(std::abs(heard.last_sample - 0.3f) < 1e-4f); // track two is playing
    }
}

TEST_CASE("a short break between tracks is hidden by the ring buffer", "[audio_path]") {
    // The ring holds about 85 ms ahead of the sound card, so a page that goes
    // quiet for 50 ms at the track change is never heard: the sound card plays
    // what's left of track one while the page catches up.
    for (std::size_t sink_frames : {256u, 1024u}) {
        INFO("sound card buffer " << sink_frames << " frames");
        const auto heard = play_track_change(sink_frames, kRate * 50 / 1000);
        CHECK(heard.longest_silence == 0);
        CHECK(heard.late_buffers == 0);
        CHECK(std::abs(heard.last_sample - 0.3f) < 1e-4f);
    }
}

TEST_CASE("a long break between tracks is heard as long as it lasts, no longer", "[audio_path]") {
    // A 200 ms break empties the ring, and render() waits for ~85 ms of track
    // two before it plays again. That wait doesn't make the gap longer: it
    // starts when the page does, and the 85 ms it waits are the 85 ms the
    // ring had covered at the start of the break. What's left over is at most
    // one sound card buffer and one packet, from the two running on their own
    // schedules.
    constexpr std::size_t kBreak = kRate * 200 / 1000;
    for (std::size_t sink_frames : {256u, 1024u}) {
        INFO("sound card buffer " << sink_frames << " frames");
        const auto heard = play_track_change(sink_frames, kBreak);
        CHECK(heard.late_buffers == 1);
        CHECK(heard.longest_silence >= kBreak - AudioPath::kPacketFrames);
        CHECK(heard.longest_silence <= kBreak + sink_frames + AudioPath::kPacketFrames);
        CHECK(std::abs(heard.last_sample - 0.3f) < 1e-4f);
    }
}

TEST_CASE("a page shut out by let_page_write(false) writes nothing", "[audio_path]") {
    auto audio = std::make_unique<AudioPath>(kRate);
    std::vector<float> page(AudioPath::kPacketFrames, 0.5f);
    const float* channels[] = {page.data(), page.data()};

    audio->let_page_write(false);
    CHECK(audio->write_planar(channels, page.size()) == 0);
    CHECK(audio->dropped_frames() == 0); // shut out isn't dropped for lack of room

    audio->let_page_write(true);
    CHECK(audio->write_planar(channels, page.size()) == page.size());
}

// The page's thread keeps sending while the test thread takes the ring buffer
// away from it, writes the tone, and hands it back, over and over, the way
// the daemon switches sources: shut the page out, stop and let render() empty
// the ring, top up the tone, play and let the page back in. Under linux-tsan
// a write_planar() and a top_up() touching the producer's side at the same
// time is reported as a data race.
TEST_CASE("the ring buffer changes hands between the page and the tone", "[audio_path]") {
    auto audio = std::make_unique<AudioPath>(kRate);
    SineGenerator sine(kRate);
    std::vector<float> packet(AudioPath::kPacketFrames, 0.25f);
    const float* channel = packet.data();

    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> page_frames{0};
    std::thread page([&] {
        while (!done.load(std::memory_order_relaxed)) {
            page_frames.fetch_add(audio->write_planar(std::span(&channel, 1), packet.size()),
                                  std::memory_order_relaxed);
        }
    });

    std::vector<float> out(kBufferFrames * AudioPath::kChannels);
    // render() is the consumer's side. It may run on any thread as long as
    // it's always the same one, so this thread plays the audio thread too.
    std::uint64_t tone_frames = 0;
    for (int round = 0; round < 500; ++round) {
        audio->let_page_write(false);
        audio->set_state(PlaybackState::Stopped);
        audio->discard();
        while (!audio->quiet()) {
            audio->render(out); // fades out, then empties the ring
        }
        tone_frames += audio->top_up(sine); // this thread is the only producer now
        audio->set_state(PlaybackState::Playing);
        audio->let_page_write(true);
        for (int i = 0; i < 4; ++i) {
            audio->render(out);
        }
    }

    // Make sure the page got in at least once, however the threads were
    // scheduled: the gate is open and it's playing, so it will.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (page_frames.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    done.store(true, std::memory_order_relaxed);
    page.join();

    // Each round emptied the ring, so each top-up wrote the whole target.
    CHECK(tone_frames == 500 * AudioPath::kTargetFrames);
    CHECK(page_frames.load() > 0);
}

TEST_CASE("discard() throws away what the tone left in the ring while stopped", "[audio_path]") {
    Fixture f;
    f.audio->set_state(PlaybackState::Stopped);
    f.render(kFadeBuffers + 1); // faded out; the top-ups filled the ring again
    REQUIRE(f.audio->quiet());

    f.audio->discard();
    CHECK_FALSE(f.audio->quiet()); // the daemon waits for render() to empty it
    f.audio->render(f.out);        // no top-up this time
    CHECK(f.audio->quiet());

    // Empty now, so playing waits to fill up instead of playing old tone.
    f.audio->set_state(PlaybackState::Playing);
    f.audio->render(f.out);
    CHECK(loudest(f.out) == 0.0f);
}
