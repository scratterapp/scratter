#pragma once

#include <cstdint>

namespace am::client {

// Which view fills the window while the daemon is connected. A view changes
// it to move to another one; main_client.cpp draws the one it names.
enum class Screen : std::uint8_t { NowPlaying, Library };

} // namespace am::client
