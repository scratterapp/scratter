// Tests for the daemon's audio path (src/daemon/audio_engine/audio_path.hpp):
// the render callback's play, pause, stop and volume behavior, called directly
// the way the audio thread would, without a sound card.

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

    f.audio->set_state(PlaybackState::Paused);
    f.render(4);
    CHECK(loudest(f.out) == 0.0f);
    CHECK(f.audio->position_frames() == position);

    f.audio->set_state(PlaybackState::Playing);
    f.render(2);
    CHECK(loudest(f.out) > 0.9f * SineGenerator::kLevel);
    CHECK(f.audio->position_frames() == position + 2 * kBufferFrames);
}

TEST_CASE("stop renders silence and rewinds the position", "[audio_path]") {
    Fixture f;
    f.render(4);
    f.audio->set_state(PlaybackState::Stopped);
    f.render();
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
    audio->render(out);
    audio->render(out); // past the resampler's start-up delay
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
    audio->set_state(PlaybackState::Paused);
    audio->render(out);
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
