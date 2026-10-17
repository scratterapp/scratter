// TODO: Implement MMCSS "Pro Audio" thread priority and VirtualLock pinning. Owner: @kerelosgayed
//
// Until then lock_memory() is a stub that fails with "not supported" (the
// daemon only warns about that), so the daemon links on Windows. Replace with
// VirtualLock (src/pal/interfaces/realtime.hpp, issue #20).

#include "realtime.hpp"

namespace am::pal {

std::expected<void, std::error_code> lock_memory(std::span<const std::byte>) {
    return std::unexpected(std::make_error_code(std::errc::function_not_supported));
}

std::expected<void, std::error_code> unlock_memory(std::span<const std::byte>) { return {}; }

} // namespace am::pal
