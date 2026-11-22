// Tests for lock_memory() / unlock_memory() (src/pal/interfaces/realtime.hpp).

#include "realtime.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

TEST_CASE("a ring-buffer-sized block can be locked and unlocked", "[realtime]") {
    // 16384 stereo float frames, the size spec 3.1.4 uses: 128 KiB. That fits
    // in the default locked-memory limit on current Linux and Windows.
    constexpr std::size_t kBytes = 16384 * 2 * sizeof(float);
    auto block = std::make_unique<std::byte[]>(kBytes);
    const std::span<const std::byte> memory(block.get(), kBytes);

    REQUIRE(am::pal::lock_memory(memory));
    REQUIRE(am::pal::unlock_memory(memory));
}

TEST_CASE("locking an empty span does nothing and succeeds", "[realtime]") {
    CHECK(am::pal::lock_memory({}));
    CHECK(am::pal::unlock_memory({}));
}

TEST_CASE("memory that isn't mapped can't be locked or unlocked", "[realtime]") {
    // The first 64 KiB of the address space is never mapped, on Linux
    // (vm.mmap_min_addr) or on Windows, so the OS has nothing to lock there.
    // The pointer only goes to the OS and is never read.
    const auto* nowhere =
        reinterpret_cast<const std::byte*>(std::uintptr_t{4096}); // NOLINT(performance-no-int-to-ptr)
    const std::span<const std::byte> memory(nowhere, 4096);
    auto locked = am::pal::lock_memory(memory);
    REQUIRE_FALSE(locked);
    CHECK(locked.error());
    auto unlocked = am::pal::unlock_memory(memory);
    REQUIRE_FALSE(unlocked);
    CHECK(unlocked.error());
}
