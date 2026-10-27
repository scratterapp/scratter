// When am-ui draws and how long it sleeps (src/client/frame_pacer.hpp). The
// window itself is checked by the am_ui_opens_and_closes test in
// tests/CMakeLists.txt.

#include "client/frame_pacer.hpp"

#include <catch2/catch_test_macros.hpp>

using am::client::FramePacer;

TEST_CASE("a new window draws two frames and then sleeps until an event", "[frame_pacer]") {
    FramePacer pacer;
    CHECK(pacer.should_draw(0));
    CHECK(pacer.wait_ms(0) == 0);
    pacer.on_frame_drawn(0);
    CHECK(pacer.should_draw(16));
    pacer.on_frame_drawn(16);

    CHECK_FALSE(pacer.should_draw(32));
    CHECK(pacer.wait_ms(32) == FramePacer::kWaitForever);
    // Nothing changes on its own, even much later.
    CHECK_FALSE(pacer.should_draw(60'000));
}

TEST_CASE("an event buys two frames", "[frame_pacer]") {
    FramePacer pacer;
    pacer.on_frame_drawn(0);
    pacer.on_frame_drawn(0);
    REQUIRE_FALSE(pacer.should_draw(100));

    pacer.on_event();
    CHECK(pacer.wait_ms(100) == 0);
    pacer.on_frame_drawn(100);
    CHECK(pacer.should_draw(116));
    pacer.on_frame_drawn(116);
    CHECK_FALSE(pacer.should_draw(132));
}

TEST_CASE("while ticking it draws once on each whole second", "[frame_pacer]") {
    FramePacer pacer;
    pacer.set_ticking(true);
    pacer.on_frame_drawn(1'250);
    pacer.on_frame_drawn(1'266);

    // The next tick is at 2000, not 1266 + 1000.
    CHECK_FALSE(pacer.should_draw(1'300));
    CHECK(pacer.wait_ms(1'300) == 700);
    CHECK(pacer.should_draw(2'000));
    pacer.on_frame_drawn(2'003);
    CHECK(pacer.wait_ms(2'003) == 997);
}

TEST_CASE("a hidden window never draws and wakes every 250 ms", "[frame_pacer]") {
    FramePacer pacer;
    pacer.set_hidden(true);
    pacer.set_ticking(true);
    pacer.on_event();
    CHECK_FALSE(pacer.should_draw(5'000));
    CHECK(pacer.wait_ms(5'000) == FramePacer::kHiddenWaitMs);

    // Shown again: the frames the event owed are still drawn.
    pacer.set_hidden(false);
    CHECK(pacer.should_draw(5'000));
}

TEST_CASE("an animating window draws every frame", "[frame_pacer]") {
    FramePacer pacer;
    pacer.set_animating(true);
    for (std::uint64_t now = 0; now < 1'000; now += 16) {
        CHECK(pacer.should_draw(now));
        CHECK(pacer.wait_ms(now) == 0);
        pacer.on_frame_drawn(now);
    }
}
