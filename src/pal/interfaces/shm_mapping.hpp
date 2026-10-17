#pragma once

// Named shared-memory region (spec sections 3.2.2 and 4).
//
// The daemon creates the telemetry page read-write and the UI opens the same
// page read-only. Both get back a ShmMapping that unmaps the region when it is
// destroyed.
//
//   Daemon:                                    UI:
//     auto m = ShmMapping::create(              auto m = ShmMapping::open_read_only(
//         kTelemetryShmName, 4096);                 kTelemetryShmName, 4096);
//     if (!m) { log m.error().message(); }      if (!m) { daemon not running yet? }
//     m->writable_bytes()                       m->bytes()
//
// The build has no exceptions, so a constructor can't report failure. The two
// static functions return std::expected instead, and the constructor that
// fills in a mapping is private.
//
// Errors are std::error_code, like the Reactor and IpcChannel interfaces. OS
// failures carry errno or GetLastError() in std::system_category(), so
// .message() reads well in a log. Compare against std::errc to branch:
//
//   std::errc::no_such_file_or_directory  open_read_only(): nobody created it
//   std::errc::permission_denied          it exists but belongs to another user
//   std::errc::result_out_of_range        smaller than `size` (open_read_only();
//                                         also create() on Windows, which can't
//                                         grow a region, comparing whole pages)
//   std::errc::invalid_argument           empty or badly formed name, or size 0
//
// There is one class and one set of member functions. src/pal/linux/posix_shm.cpp
// defines them on Linux and src/pal/win32/win32_shm.cpp on Windows, and CMake
// compiles only one of the two. Nothing here may include a platform header.

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace am::pal {

// Plain name, no slash or backslash: the daemon's name plus "_telemetry_v2"
// (AM_APP_NAME comes from the root CMakeLists.txt; see common/app_name.hpp).
// Each platform turns it into a name only the current user can see: Linux adds
// a leading slash and the user ID ("/<daemon>_telemetry_v2.1000"), Windows adds
// "Local\" (per session).
inline constexpr std::string_view kTelemetryShmName = AM_APP_NAME "d_telemetry_v2";

class ShmMapping {
  public:
    // Creates the region if it doesn't exist, or reuses it if it does (on
    // Linux, for example, one left over from a daemon that crashed), and maps
    // it read-write. Fails with permission_denied if the existing region
    // belongs to another user.
    // The region is `size` bytes; new regions start zeroed.
    //
    // A Windows region can't grow and its size is rounded up to whole pages:
    // reusing an existing region smaller than `size` fails with
    // result_out_of_range, and `size` is compared against that page-rounded
    // size.
    //
    // The mapping returned by create() owns the name. When it is destroyed the
    // name goes away, so later open_read_only() calls fail with
    // no_such_file_or_directory until the daemon creates it again. Readers that already have it
    // mapped keep working.
    //
    // On Windows a region's name is reachable only while some handle to it is
    // open; a mapped view keeps the memory, not the name. So create() holds its
    // section handle in native_handle_ for the mapping's lifetime, while
    // open_read_only() closes its handle before returning (the view is enough).
    // The OS closes a crashed daemon's handles, so no name is left behind.
    [[nodiscard]] static std::expected<ShmMapping, std::error_code>
    create(std::string_view name, std::size_t size) noexcept;

    // Maps an existing region read-only. Fails with no_such_file_or_directory
    // if nobody has created it, and result_out_of_range if it is smaller than
    // `size`. Writing through
    // the mapping crashes the process, so only bytes() is useful.
    [[nodiscard]] static std::expected<ShmMapping, std::error_code>
    open_read_only(std::string_view name, std::size_t size) noexcept;

    // An empty mapping: no region, size 0. A moved-from mapping looks the same.
    ShmMapping() noexcept = default;

    ~ShmMapping() { reset(); }

    // Move-only: two objects unmapping the same region would be a double free.
    ShmMapping(const ShmMapping&) = delete;
    ShmMapping& operator=(const ShmMapping&) = delete;

    ShmMapping(ShmMapping&& other) noexcept
        : data_(other.data_), size_(other.size_), writable_(other.writable_),
          owned_name_(std::move(other.owned_name_)), native_handle_(other.native_handle_) {
        other.data_ = nullptr;
        other.size_ = 0;
        other.writable_ = false;
        other.owned_name_.clear();
        other.native_handle_ = nullptr;
    }

    ShmMapping& operator=(ShmMapping&& other) noexcept {
        if (this != &other) {
            reset();
            data_ = other.data_;
            size_ = other.size_;
            writable_ = other.writable_;
            owned_name_ = std::move(other.owned_name_);
            native_handle_ = other.native_handle_;
            other.data_ = nullptr;
            other.size_ = 0;
            other.writable_ = false;
            other.owned_name_.clear();
            other.native_handle_ = nullptr;
        }
        return *this;
    }

    // The whole region. Empty for an empty mapping.
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return {static_cast<const std::byte*>(data_), size_};
    }

    // The whole region, writable. Empty unless the mapping came from create().
    [[nodiscard]] std::span<std::byte> writable_bytes() noexcept {
        if (!writable_) {
            return {};
        }
        return {static_cast<std::byte*>(data_), size_};
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool writable() const noexcept { return writable_; }

  private:
    ShmMapping(void* data, std::size_t size, bool writable, std::string owned_name,
               void* native_handle = nullptr) noexcept
        : data_(data), size_(size), writable_(writable), owned_name_(std::move(owned_name)),
          native_handle_(native_handle) {}

    // Unmaps the region (and, for the creator, removes the name), then leaves
    // this object empty. Defined per platform.
    void reset() noexcept;

    void* data_ = nullptr;
    std::size_t size_ = 0;
    bool writable_ = false;
    std::string owned_name_; // set only on the mapping that create() returned
    // An OS handle the platform keeps for the mapping's lifetime. Linux leaves
    // it null. On Windows the creator stores its section handle here, because a
    // region's name lasts only while a handle to it is open.
    void* native_handle_ = nullptr;
};

} // namespace am::pal
