#pragma once

// Test hooks for the WASAPI sink (wasapi_sink.cpp). Windows only, and only for
// tests/test_wasapi_sink.cpp: the daemon never calls these.
//
// A test can't change the user's default output device, or unplug it, without
// moving everything else they're listening to. reopen_all() gets the sink to
// do what it does then instead: drop its stream and set up a new one on the
// default device, which here is the same device.

#include <cstdint>

namespace am::pal::wasapi_testing {

// Asks every open sink to reopen its stream, as if the default device had
// changed. Returns at once; each sink's own thread does the work.
void reopen_all() noexcept;

// How many streams the sinks in this process have set up so far, open()'s
// included. Goes up by one for each reopen that worked.
[[nodiscard]] std::uint64_t streams_opened() noexcept;

} // namespace am::pal::wasapi_testing
