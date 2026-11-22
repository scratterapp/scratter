#pragma once

// Decides when am-ui draws a frame and how long it may sleep in between (spec
// section 3.4.2). A music player's window spends
// most of its life showing the same picture, so drawing 60 times a second
// would burn CPU and battery for nothing. The rules:
//
//   hidden or minimized   draw nothing, wake at least every 250 ms
//   animating             draw every frame (vsync caps the rate), or at
//                         most once per frame interval if it has one
//   otherwise             draw after an event, plus once each whole second
//                         while ticking (playing, so the progress bar's
//                         clock moves; main_client.cpp turns it on)
//
// Dear ImGui sometimes needs a second frame to settle after input (a hover
// highlight, a layout that changed size), so an event buys two frames.
//
// It knows nothing about SDL: main_client.cpp feeds it the time and what
// happened, which keeps the rules testable without a window
// (tests/test_frame_pacer.cpp).

#include <algorithm>
#include <cstdint>

namespace am::client {

class FramePacer {
  public:
    static constexpr std::int32_t kHiddenWaitMs = 250;
    static constexpr std::int32_t kFramesPerEvent = 2;
    static constexpr std::uint64_t kTickMs = 1000;
    // The spectrum's frame interval: about 33 a second, as often as the
    // daemon changes the bars (SpectrumAnalyzer::kUpdateMillis). Drawing
    // faster, at a 60 or 240 Hz screen's rate, only draws the same bars
    // again, and each frame costs about 1.5 ms of CPU.
    static constexpr std::uint64_t kSpectrumFrameMs = 30;
    // What wait_ms() returns when only an event can make a frame due. SDL's
    // wait functions take -1 as "no time limit".
    static constexpr std::int32_t kWaitForever = -1;

    // Something happened that may change the picture: input, a resize, the
    // window being uncovered, and later a message from the daemon.
    void on_event() { frames_owed_ = kFramesPerEvent; }

    void set_hidden(bool hidden) { hidden_ = hidden; }
    // On while something moves on its own, like the spectrum (#40). With a
    // frame interval, at most one frame per that many milliseconds; without
    // (0), every frame vsync allows.
    // Turning it off draws one more frame, so the screen shows where the
    // animation stopped.
    void set_animating(bool animating, std::uint64_t frame_interval_ms = 0) {
        if (animating_ && !animating) {
            frames_owed_ = std::max(frames_owed_, 1);
        }
        animating_ = animating;
        frame_interval_ms_ = frame_interval_ms;
    }
    // On while the clock on screen should move once a second.
    void set_ticking(bool ticking) { ticking_ = ticking; }

    // How long the loop may wait for the next event before calling
    // should_draw() again. 0 means don't wait at all, kWaitForever means
    // sleep until an event comes.
    [[nodiscard]] std::int32_t wait_ms(std::uint64_t now_ms) const {
        if (hidden_) {
            return kHiddenWaitMs;
        }
        if (should_draw(now_ms)) {
            return 0;
        }
        if (animating_) {
            return static_cast<std::int32_t>(next_animation_ms() - now_ms);
        }
        if (!ticking_) {
            return kWaitForever;
        }
        return static_cast<std::int32_t>(next_tick_ms_ - now_ms);
    }

    [[nodiscard]] bool should_draw(std::uint64_t now_ms) const {
        return !hidden_ && (animation_due(now_ms) || frames_owed_ > 0 || tick_due(now_ms));
    }

    void on_frame_drawn(std::uint64_t now_ms) {
        if (frames_owed_ > 0) {
            --frames_owed_;
        }
        // The next whole second, so the clock ticks on the second rather
        // than drifting by however long each frame took.
        next_tick_ms_ = (now_ms / kTickMs + 1) * kTickMs;
        last_frame_ms_ = now_ms;
    }

  private:
    [[nodiscard]] std::uint64_t next_animation_ms() const {
        return last_frame_ms_ + frame_interval_ms_;
    }
    [[nodiscard]] bool animation_due(std::uint64_t now_ms) const {
        return animating_ && now_ms >= next_animation_ms();
    }
    [[nodiscard]] bool tick_due(std::uint64_t now_ms) const {
        return ticking_ && now_ms >= next_tick_ms_;
    }

    bool hidden_ = false;
    bool animating_ = false;
    bool ticking_ = false;
    // The first frame is owed too, so the window isn't blank until the
    // first event.
    std::int32_t frames_owed_ = kFramesPerEvent;
    std::uint64_t next_tick_ms_ = 0;
    std::uint64_t frame_interval_ms_ = 0; // while animating; 0 for every frame
    std::uint64_t last_frame_ms_ = 0;
};

} // namespace am::client
