#pragma once

#include <string_view>

// The project's name comes from AM_APP_NAME in the root CMakeLists.txt. Build
// runtime names (socket path, shared-memory page, MPRIS bus name, keyring service)
// from these constants instead of writing "scratterd" by hand.
namespace am::common {

inline constexpr std::string_view kAppName = AM_APP_NAME;
inline constexpr std::string_view kDaemonName = AM_APP_NAME "d";

} // namespace am::common
