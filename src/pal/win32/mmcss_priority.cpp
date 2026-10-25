// Windows implementation of src/pal/interfaces/realtime.hpp.
//
// Only memory locking lives here, like rtkit_priority.cpp on Linux. The audio
// thread's priority is the sink's job: wasapi_sink.cpp registers its own
// thread with MMCSS.
//
// VirtualLock can only lock as much as the process's minimum working set
// allows, and the default minimum is small (about 200 KB, part of it already
// used). When Windows says the quota is too low, lock_memory() raises the
// minimum and maximum by the size of the block and tries again. A process can
// raise its own working set without any privilege.

#include "realtime.hpp"

#include <windows.h>

namespace am::pal {

namespace {

std::unexpected<std::error_code> last_error() {
    return std::unexpected(
        std::error_code(static_cast<int>(::GetLastError()), std::system_category()));
}

} // namespace

std::expected<void, std::error_code> lock_memory(std::span<const std::byte> memory) {
    if (memory.empty()) {
        return {};
    }
    // VirtualLock takes a non-const pointer but doesn't write through it. Like
    // mlock(), it makes every page resident now, so none can fault later on the
    // audio thread.
    void* const start = const_cast<std::byte*>(memory.data());
    if (::VirtualLock(start, memory.size())) {
        return {};
    }
    if (::GetLastError() != ERROR_WORKING_SET_QUOTA) {
        return last_error();
    }

    const HANDLE process = ::GetCurrentProcess();
    SIZE_T minimum = 0;
    SIZE_T maximum = 0;
    DWORD flags = 0;
    if (!::GetProcessWorkingSetSizeEx(process, &minimum, &maximum, &flags)) {
        return last_error();
    }
    // The block, plus a page at each end for a start and an end that aren't
    // page-aligned.
    SYSTEM_INFO info{};
    ::GetSystemInfo(&info);
    const SIZE_T grow = memory.size() + 2 * static_cast<SIZE_T>(info.dwPageSize);
    if (!::SetProcessWorkingSetSizeEx(process, minimum + grow, maximum + grow, flags)) {
        return last_error();
    }
    if (!::VirtualLock(start, memory.size())) {
        return last_error();
    }
    return {};
}

std::expected<void, std::error_code> unlock_memory(std::span<const std::byte> memory) {
    if (memory.empty()) {
        return {};
    }
    // The working set stays raised. It's only a quota, and the daemon locks
    // one block for its whole life.
    if (!::VirtualUnlock(const_cast<std::byte*>(memory.data()), memory.size())) {
        return last_error();
    }
    return {};
}

} // namespace am::pal
