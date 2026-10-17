// Tests for CRC32C (src/common/crc32c.hpp).
//
// Each known-answer vector runs through every implementation this build has:
// the portable table version always, and the hardware version when the CPU has
// it. The last test checks the two agree on every length and alignment, which is
// what catches mistakes in the hardware version's 8-byte steps and leftover bytes.

#include "common/crc32c.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>
#include <vector>

namespace {

using am::common::crc32c;
using am::common::crc32c_portable;

std::vector<std::byte> bytes_of(std::string_view text) {
    std::vector<std::byte> out;
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

std::vector<std::byte> bytes_of(std::initializer_list<int> values) {
    std::vector<std::byte> out;
    for (const int v : values) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

// Runs `data` through every implementation and checks each gives `expected`.
void check_all_paths(std::span<const std::byte> data, std::uint32_t expected) {
    CHECK(crc32c_portable(data) == expected);
    CHECK(crc32c(data) == expected);
#if defined(AM_CRC32C_HAS_HARDWARE_PATH)
    if (am::common::has_hardware_crc32c()) {
        CHECK(am::common::crc32c_hardware(data) == expected);
    }
#endif
}

} // namespace

TEST_CASE("reports which implementation this build uses", "[crc32c]") {
#if defined(AM_CRC32C_HAS_HARDWARE_PATH)
    if (am::common::has_hardware_crc32c()) {
        SUCCEED("hardware CRC32C available and tested");
    } else {
        WARN("hardware CRC32C compiled in but this CPU lacks it; only the portable path ran");
    }
#else
    CHECK_FALSE(am::common::has_hardware_crc32c());
    WARN("no hardware CRC32C on this architecture; only the portable path ran");
#endif
}

TEST_CASE("empty input gives zero", "[crc32c]") { check_all_paths({}, 0x00000000); }

TEST_CASE("standard check value: \"123456789\"", "[crc32c]") {
    check_all_paths(bytes_of("123456789"), 0xE3069283);
}

// RFC 3720 (iSCSI) appendix B.4.
TEST_CASE("RFC 3720 test vectors", "[crc32c]") {
    std::array<std::byte, 32> data{};

    SECTION("32 bytes of zeros") {
        data.fill(std::byte{0x00});
        check_all_paths(data, 0x8A9136AA);
    }

    SECTION("32 bytes of 0xFF") {
        data.fill(std::byte{0xFF});
        check_all_paths(data, 0x62A8AB43);
    }

    SECTION("32 incrementing bytes 0x00..0x1F") {
        for (std::size_t i = 0; i < data.size(); ++i) {
            data[i] = static_cast<std::byte>(i);
        }
        check_all_paths(data, 0x46DD794E);
    }

    SECTION("32 decrementing bytes 0x1F..0x00") {
        for (std::size_t i = 0; i < data.size(); ++i) {
            data[i] = static_cast<std::byte>(31 - i);
        }
        check_all_paths(data, 0x113FDB5C);
    }

    SECTION("an iSCSI read command") {
        const auto pdu = bytes_of({
            0x01, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00,
            0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x18, 0x28, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        });
        check_all_paths(pdu, 0xD9963A56);
    }
}

TEST_CASE("flipping any single bit changes the checksum", "[crc32c]") {
    auto data = bytes_of("a 28-byte frame header here!");
    const std::uint32_t original = crc32c(data);

    for (auto& b : data) {
        for (int bit = 0; bit < 8; ++bit) {
            b ^= static_cast<std::byte>(1 << bit);
            CHECK(crc32c(data) != original);
            b ^= static_cast<std::byte>(1 << bit);
        }
    }
    CHECK(crc32c(data) == original);
}

TEST_CASE("hardware and portable agree on every length and alignment", "[crc32c]") {
#if defined(AM_CRC32C_HAS_HARDWARE_PATH)
    if (!am::common::has_hardware_crc32c()) {
        return;
    }

    std::array<std::byte, 128> buffer{};
    for (std::size_t i = 0; i < buffer.size(); ++i) {
        buffer[i] = static_cast<std::byte>(i * 37 + 11);
    }

    // Starting at offsets 0..7 means the 8-byte loads are misaligned in every
    // possible way; lengths 0..64 cover every count of leftover tail bytes.
    for (std::size_t offset = 0; offset < 8; ++offset) {
        for (std::size_t length = 0; length <= 64; ++length) {
            const auto slice = std::span<const std::byte>(buffer).subspan(offset, length);
            CHECK(am::common::crc32c_hardware(slice) == crc32c_portable(slice));
        }
    }
#endif
}
