// Tests for the Linux side of ShmMapping (src/pal/linux/posix_shm.cpp): the
// ways the OS can refuse a mapping, which test_shm_mapping.cpp (the contract
// both platforms meet) doesn't go into.
//
// The objects live in /dev/shm as "/<name>.<uid>". A test that leaves one
// behind on purpose removes it with shm_unlink at the end.

#include "shm_mapping.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

using am::pal::ShmMapping;

namespace {

std::string unique_name() {
    std::random_device rd;
    return "am_test_" + std::to_string(rd()) + "_" + std::to_string(rd());
}

// The name posix_shm.cpp gives the object for `name`.
std::string posix_name(const std::string& name) {
    return "/" + name + "." + std::to_string(geteuid());
}

// More than a 64-bit Linux process can map: user space is 128 TiB (47 bits),
// or 64 PiB with 5-level page tables, which is still less than this.
constexpr std::size_t kTooBigToMap = std::size_t{1} << 57;

} // namespace

TEST_CASE("a name too long for the OS is reported", "[shm]") {
    const std::string name(300, 'a'); // NAME_MAX is 255
    auto m = ShmMapping::create(name, 4096);
    REQUIRE_FALSE(m);
    CHECK(m.error() == std::errc::filename_too_long);
}

TEST_CASE("a size the OS can't grow the object to is reported", "[shm]") {
    const std::string name = unique_name();
    // SIZE_MAX turns into an off_t of -1, which ftruncate() refuses.
    auto m = ShmMapping::create(name, SIZE_MAX);
    REQUIRE_FALSE(m);
    CHECK(m.error() == std::errc::invalid_argument);
    shm_unlink(posix_name(name).c_str());
}

TEST_CASE("a size too big to map is reported, writing and reading", "[shm]") {
    const std::string name = unique_name();
    // tmpfs grows the object that far without using any memory (it's all
    // holes), but no process has that much address space to map it into.
    auto writer = ShmMapping::create(name, kTooBigToMap);
    REQUIRE_FALSE(writer);
    CHECK(writer.error() == std::errc::not_enough_memory);

    // The object is still there at that size, so a reader gets as far as
    // mapping it, and fails the same way.
    auto reader = ShmMapping::open_read_only(name, kTooBigToMap);
    REQUIRE_FALSE(reader);
    CHECK(reader.error() == std::errc::not_enough_memory);
    shm_unlink(posix_name(name).c_str());
}

TEST_CASE("an object another user owns is refused", "[shm]") {
    // Only root can give an object away, so this checks the owner test
    // (another user's daemon, or someone planting a page) only when run as root.
    if (geteuid() != 0) {
        WARN("AM_TEST_SKIPPED: needs root to make an object another user owns");
        return;
    }
    const std::string name = unique_name();
    const std::string path = posix_name(name);
    const int fd = shm_open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    REQUIRE(fd >= 0);
    REQUIRE(ftruncate(fd, 4096) == 0);
    REQUIRE(fchown(fd, 65534, 65534) == 0); // nobody
    close(fd);

    auto writer = ShmMapping::create(name, 4096);
    REQUIRE_FALSE(writer);
    CHECK(writer.error() == std::errc::permission_denied);

    auto reader = ShmMapping::open_read_only(name, 4096);
    REQUIRE_FALSE(reader);
    CHECK(reader.error() == std::errc::permission_denied);
    shm_unlink(path.c_str());
}
