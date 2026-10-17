#pragma once

// CRC32C checksum (spec section 3.2.1). Every IPC FrameHeader carries the
// CRC32C of its first 28 bytes, so a receiver can reject a corrupted or garbage
// frame before trusting any field in it.
//
// CRC32C is the "Castagnoli" CRC: polynomial 0x1EDC6F41, written here in its
// bit-reflected form 0x82F63B78. It is the same checksum as iSCSI (RFC 3720),
// ext4 and SCTP, and the one x86 SSE4.2 and ARMv8 can compute in hardware.
//
// There are two implementations that always give the same answer:
//
//   crc32c_portable()  A 256-entry lookup table, one byte per step. Runs anywhere.
//   crc32c_hardware()  The CPU's crc32 instruction, eight bytes per step. Only
//                      compiled on x86-64 and on ARM64 builds that enable the
//                      CRC extension, and only safe to call when
//                      has_hardware_crc32c() is true.
//
// Callers use crc32c(), which picks the hardware one when it can.
//
// These are CPU checks, not OS checks: the same code paths run on Linux and
// Windows, which is why they live here and not in src/pal/.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#if defined(__x86_64__) || defined(_M_X64)
#define AM_CRC32C_HAS_HARDWARE_PATH 1
#define AM_CRC32C_X86 1
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <immintrin.h>
#endif
#elif defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
#define AM_CRC32C_HAS_HARDWARE_PATH 1
#define AM_CRC32C_ARM 1
#include <arm_acle.h>
#endif

namespace am::common {

inline constexpr std::uint32_t kCrc32cPolynomial = 0x82F63B78; // bit-reflected 0x1EDC6F41

// --- Portable version ------------------------------------------------------

// kCrc32cTable[b] is what eight rounds of shift-and-xor do to the low byte `b`.
// Built by the compiler, so it costs nothing at runtime.
inline constexpr std::array<std::uint32_t, 256> kCrc32cTable = [] {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t byte = 0; byte < 256; ++byte) {
        std::uint32_t crc = byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1) ? (crc >> 1) ^ kCrc32cPolynomial : crc >> 1;
        }
        table[byte] = crc;
    }
    return table;
}();

[[nodiscard]] inline std::uint32_t crc32c_portable(std::span<const std::byte> data) noexcept {
    std::uint32_t crc = 0xFFFFFFFF;
    for (const std::byte b : data) {
        const auto index = (crc ^ static_cast<std::uint32_t>(b)) & 0xFF;
        crc = kCrc32cTable[index] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

// --- Hardware version ------------------------------------------------------

#if defined(AM_CRC32C_X86)

// GCC and Clang only emit SSE4.2 instructions inside functions marked for it.
// MSVC doesn't need (or recognise) the attribute.
#if defined(__GNUC__)
#define AM_CRC32C_TARGET [[gnu::target("sse4.2")]]
#else
#define AM_CRC32C_TARGET
#endif

// True when this CPU has SSE4.2. Asked once; the answer can't change.
[[nodiscard]] inline bool has_hardware_crc32c() noexcept {
#if defined(_MSC_VER)
    static const bool supported = [] {
        int regs[4]{}; // eax, ebx, ecx, edx
        __cpuid(regs, 1);
        return (regs[2] & (1 << 20)) != 0; // CPUID leaf 1, ECX bit 20 = SSE4.2
    }();
#else
    static const bool supported = __builtin_cpu_supports("sse4.2");
#endif
    return supported;
}

AM_CRC32C_TARGET [[nodiscard]] inline std::uint32_t
crc32c_hardware(std::span<const std::byte> data) noexcept {
    std::uint64_t crc = 0xFFFFFFFF;
    std::size_t i = 0;
    for (; i + 8 <= data.size(); i += 8) {
        std::uint64_t chunk;
        std::memcpy(&chunk, data.data() + i, 8); // memcpy: data may not be 8-byte aligned
        crc = _mm_crc32_u64(crc, chunk);
    }
    auto crc32 = static_cast<std::uint32_t>(crc);
    for (; i < data.size(); ++i) {
        crc32 = _mm_crc32_u8(crc32, static_cast<unsigned char>(data[i]));
    }
    return crc32 ^ 0xFFFFFFFF;
}

#undef AM_CRC32C_TARGET

#elif defined(AM_CRC32C_ARM)

// The compiler was told the CPU has the CRC extension (__ARM_FEATURE_CRC32), so
// there is nothing to check at runtime.
[[nodiscard]] inline bool has_hardware_crc32c() noexcept { return true; }

[[nodiscard]] inline std::uint32_t crc32c_hardware(std::span<const std::byte> data) noexcept {
    std::uint32_t crc = 0xFFFFFFFF;
    std::size_t i = 0;
    for (; i + 8 <= data.size(); i += 8) {
        std::uint64_t chunk;
        std::memcpy(&chunk, data.data() + i, 8); // memcpy: data may not be 8-byte aligned
        crc = __crc32cd(crc, chunk);
    }
    for (; i < data.size(); ++i) {
        crc = __crc32cb(crc, static_cast<std::uint8_t>(data[i]));
    }
    return crc ^ 0xFFFFFFFF;
}

#else

[[nodiscard]] inline bool has_hardware_crc32c() noexcept { return false; }

#endif

// --- What callers use ------------------------------------------------------

[[nodiscard]] inline std::uint32_t crc32c(std::span<const std::byte> data) noexcept {
#if defined(AM_CRC32C_HAS_HARDWARE_PATH)
    if (has_hardware_crc32c()) {
        return crc32c_hardware(data);
    }
#endif
    return crc32c_portable(data);
}

} // namespace am::common
