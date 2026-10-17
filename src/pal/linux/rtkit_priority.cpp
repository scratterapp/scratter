// Linux implementation of src/pal/interfaces/realtime.hpp.
//
// Only memory locking lives here for now. The audio thread's real-time
// priority comes from PipeWire, which asks rtkit for it on its own data
// thread, so the sink doesn't need to call rtkit itself.

#include "realtime.hpp"

#include <sys/mman.h>

#include <cerrno>

namespace am::pal {

std::expected<void, std::error_code> lock_memory(std::span<const std::byte> memory) {
    if (memory.empty()) {
        return {};
    }
    // Plain mlock(), not mlock2(MLOCK_ONFAULT): it faults every page in now,
    // so none can fault later on the audio thread.
    if (::mlock(memory.data(), memory.size()) != 0) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }
    return {};
}

std::expected<void, std::error_code> unlock_memory(std::span<const std::byte> memory) {
    if (memory.empty()) {
        return {};
    }
    if (::munlock(memory.data(), memory.size()) != 0) {
        return std::unexpected(std::error_code(errno, std::system_category()));
    }
    return {};
}

} // namespace am::pal
