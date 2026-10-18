// Tests for the daemon's audio path (src/daemon/audio_engine/audio_path.hpp):
// the render callback's play, pause, stop, fade and volume behavior, called
// directly the way the audio thread would, without a sound card.

#include "daemon/audio_engine/audio_path.hpp"
#include "daemon/audio_engine/sine_generator.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <span>
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

    // The page keeps sending for a while after the pause; none of it is kept
    // once the fade-out is over.
    audio->set_state(PlaybackState::Paused);
    for (int i = 0; i <= kFadeBuffers; ++i) {
        audio->render(out);
    }
    audio->write_planar(channels, page.size());
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
