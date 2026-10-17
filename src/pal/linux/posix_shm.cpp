// POSIX shared memory for ShmMapping (spec section 3.2.2). Linux only.
//
// A POSIX shared-memory object is a file in /dev/shm (a RAM-backed tmpfs):
// shm_open() opens it by name, ftruncate() sets its size, and mmap() maps it
// into this process. Two processes that map the same object see the same
// physical pages, at different virtual addresses.
//
// /dev/shm is shared by every user on the machine, so two things keep the page
// private to one user: the name ends in the user ID, so two people logged in at
// once each get their own page, and both functions refuse an object that some
// other user created under our name (they could otherwise read our telemetry,
// or feed fake telemetry to our UI).

#include "shm_mapping.hpp"

#include <cerrno>
#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace am::pal {
namespace {

std::error_code os_error(int err) noexcept { return {err, std::system_category()}; }

std::error_code invalid_argument() noexcept {
    return std::make_error_code(std::errc::invalid_argument);
}

// POSIX names must start with one slash and contain no others.
bool is_valid_name(std::string_view name) noexcept {
    return !name.empty() && name.find('/') == std::string_view::npos &&
           name.find('\\') == std::string_view::npos;
}

// "<daemon>_telemetry_v2" -> "/<daemon>_telemetry_v2.1000" for user 1000.
std::string posix_name(std::string_view name) {
    std::string path = "/";
    path += name;
    path += '.';
    path += std::to_string(::geteuid());
    return path;
}

// Fails unless the object behind `fd` belongs to this user. Also hands back
// its size, which open_read_only() needs.
std::expected<std::size_t, std::error_code> check_owner(int fd) noexcept {
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        return std::unexpected(os_error(errno));
    }
    if (st.st_uid != ::geteuid()) {
        return std::unexpected(std::make_error_code(std::errc::permission_denied));
    }
    return static_cast<std::size_t>(st.st_size);
}

} // namespace

std::expected<ShmMapping, std::error_code> ShmMapping::create(std::string_view name,
                                                              std::size_t size) noexcept {
    if (!is_valid_name(name) || size == 0) {
        return std::unexpected(invalid_argument());
    }
    std::string path = posix_name(name);

    // O_CREAT without O_EXCL: reuse an object a crashed daemon left behind.
    // O_CLOEXEC: don't leak the descriptor into child processes (CEF spawns
    // several). 0600: only this user can open it.
    const int fd = ::shm_open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) {
        return std::unexpected(os_error(errno));
    }
    if (auto owner = check_owner(fd); !owner) {
        ::close(fd);
        return std::unexpected(owner.error());
    }

    // A new object has size 0; this grows it, and the new bytes read as zero.
    if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
        const int err = errno;
        ::close(fd);
        return std::unexpected(os_error(err));
    }

    void* data = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    const int err = errno;
    // The mapping keeps the object alive on its own, so the descriptor can go.
    ::close(fd);
    if (data == MAP_FAILED) {
        return std::unexpected(os_error(err));
    }

    return ShmMapping(data, size, true, std::move(path));
}

std::expected<ShmMapping, std::error_code> ShmMapping::open_read_only(std::string_view name,
                                                                      std::size_t size) noexcept {
    if (!is_valid_name(name) || size == 0) {
        return std::unexpected(invalid_argument());
    }
    const std::string path = posix_name(name);

    const int fd = ::shm_open(path.c_str(), O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0) {
        return std::unexpected(os_error(errno));
    }

    const auto object_size = check_owner(fd);
    if (!object_size) {
        ::close(fd);
        return std::unexpected(object_size.error());
    }
    // Mapping past the end of the object would map pages that crash the
    // process (SIGBUS) on first read, so check the size first.
    if (*object_size < size) {
        ::close(fd);
        return std::unexpected(std::make_error_code(std::errc::result_out_of_range));
    }

    void* data = ::mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
    const int err = errno;
    ::close(fd);
    if (data == MAP_FAILED) {
        return std::unexpected(os_error(err));
    }

    return ShmMapping(data, size, false, std::string{});
}

void ShmMapping::reset() noexcept {
    if (data_ != nullptr) {
        ::munmap(data_, size_);
    }
    // Only the creator removes the name. Anyone who still has it mapped keeps
    // their mapping; the memory is freed when the last one unmaps.
    if (!owned_name_.empty()) {
        ::shm_unlink(owned_name_.c_str());
    }
    data_ = nullptr;
    size_ = 0;
    writable_ = false;
    owned_name_.clear();
}

} // namespace am::pal
