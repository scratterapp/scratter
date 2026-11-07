// src/pal/interfaces/process_launch.hpp, not written yet on this platform.
//
// TODO(kero): the real version. Until then the daemon logs that it can't
// start the window and carries on.

#include "process_launch.hpp"

namespace am::pal {

std::expected<void, std::error_code> launch_ui(Reactor& /*reactor*/,
                                               const std::filesystem::path& /*program*/) {
    return std::unexpected(std::make_error_code(std::errc::function_not_supported));
}

} // namespace am::pal
