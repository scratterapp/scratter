#pragma once

// A linear arena for catalog data (spec section 3.3.2, issue #103).
//
// The catalog parser (#105) copies what it pulls out of an Apple Music
// response (titles, ids, durations, artwork URLs) into one of these, and the
// window draws a view from it. When the user moves to another album or
// playlist, the whole view goes at once with reset(): one store, no matter how
// many tracks it held, and no heap traffic while a view is being built.
//
//   auto arena = LinearArena::create(256 * 1024);       // std::expected<LinearArena, std::errc>
//   auto tracks = arena->make_array<Track>(n);           // std::optional<std::span<Track>>
//   auto title = arena->copy_string(json_title);         // std::optional<std::string_view>
//   ...
//   arena->reset();                                      // everything above is gone
//
// How it works: one block, allocated once by create(), and a count of how
// much of it is used. Each allocation rounds the count up to the alignment
// asked for and moves it past the new slice. reset() sets the count back to 0.
// Nothing is freed one at a time, and no destructors run, which is why only
// trivially destructible types go in (a std::string or std::vector would leak
// its own heap memory). Strings go in as std::string_view over bytes copied
// into the arena.
//
// A full arena is a normal answer, not a crash: allocate(), make_array() and
// copy_string() return nothing, make() returns nullptr, and the arena is
// unchanged. The caller decides what to do (show what fits, or start over in
// a bigger arena).
//
// Everything handed out points into the block, so it's valid until the next
// reset() or until the arena is destroyed. Moving the arena keeps the block
// (and so every pointer) where it is.
//
// One thread at a time: it's for building a view on the window's thread, and
// has no locks or atomics.

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

namespace am::common {

// Rounds `offset` up to a multiple of `align`, a power of two. Returns
// nothing if that would go past the largest std::size_t.
[[nodiscard]] constexpr std::optional<std::size_t> align_up(std::size_t offset,
                                                            std::size_t align) noexcept {
    const std::size_t mask = align - 1;
    if (offset > SIZE_MAX - mask) {
        return std::nullopt;
    }
    return (offset + mask) & ~mask;
}

static_assert(align_up(0, 8) == 0);
static_assert(align_up(1, 8) == 8);
static_assert(align_up(8, 8) == 8);
static_assert(align_up(9, 1) == 9);
static_assert(align_up(65, 64) == 128);
static_assert(!align_up(SIZE_MAX, 2).has_value());

class LinearArena {
  public:
    // Allocates the block: `capacity` bytes, zeroed. invalid_argument for 0,
    // not_enough_memory if the system says no.
    [[nodiscard]] static std::expected<LinearArena, std::errc>
    create(std::size_t capacity) noexcept {
        if (capacity == 0) {
            return std::unexpected(std::errc::invalid_argument);
        }
        // new (std::nothrow) returns nullptr instead of throwing; the
        // project builds without exceptions.
        std::unique_ptr<std::byte[]> block(new (std::nothrow) std::byte[capacity]());
        if (!block) {
            return std::unexpected(std::errc::not_enough_memory);
        }
        return LinearArena(std::move(block), capacity);
    }

    // Moved from, it holds nothing and refuses every allocation.
    LinearArena(LinearArena&& other) noexcept
        : block_(std::move(other.block_)), capacity_(std::exchange(other.capacity_, 0)),
          used_(std::exchange(other.used_, 0)) {}
    LinearArena& operator=(LinearArena&& other) noexcept {
        if (this != &other) {
            block_ = std::move(other.block_);
            capacity_ = std::exchange(other.capacity_, 0);
            used_ = std::exchange(other.used_, 0);
        }
        return *this;
    }
    LinearArena(const LinearArena&) = delete;
    LinearArena& operator=(const LinearArena&) = delete;
    ~LinearArena() = default;

    // `bytes` bytes at an address that's a multiple of `align`, a power of
    // two (64 for a cache line or AVX-512 is fine: it's the address that's
    // rounded, not the offset, so it doesn't depend on how the block itself
    // is aligned). Nothing if it doesn't fit, or if `align` isn't a power of two.
    [[nodiscard]] std::optional<std::span<std::byte>> allocate(std::size_t bytes,
                                                               std::size_t align) noexcept {
        if (capacity_ == 0 || !std::has_single_bit(align)) {
            return std::nullopt;
        }
        const auto base = reinterpret_cast<std::uintptr_t>(block_.get());
        const std::optional<std::size_t> aligned = align_up(base + used_, align);
        if (!aligned) {
            return std::nullopt;
        }
        const std::size_t start = *aligned - base;
        if (start > capacity_ || bytes > capacity_ - start) {
            return std::nullopt;
        }
        used_ = start + bytes;
        return std::span<std::byte>(block_.get() + start, bytes);
    }

    // `count` value-initialized T (zeros for plain structs), side by side.
    template <typename T>
        requires std::is_trivially_destructible_v<T> && std::is_default_constructible_v<T>
    [[nodiscard]] std::optional<std::span<T>> make_array(std::size_t count) noexcept {
        if (count > SIZE_MAX / sizeof(T)) {
            return std::nullopt;
        }
        const auto bytes = allocate(count * sizeof(T), alignof(T));
        if (!bytes) {
            return std::nullopt;
        }
        T* first = reinterpret_cast<T*>(bytes->data());
        std::uninitialized_value_construct_n(first, count);
        return std::span<T>(first, count);
    }

    // One T built from `args`, or nullptr if there's no room.
    template <typename T, typename... Args>
        requires std::is_trivially_destructible_v<T>
    [[nodiscard]] T* make(Args&&... args) noexcept {
        const auto bytes = allocate(sizeof(T), alignof(T));
        if (!bytes) {
            return nullptr;
        }
        return std::construct_at(reinterpret_cast<T*>(bytes->data()), std::forward<Args>(args)...);
    }

    // A copy of `text` in the arena, so it outlives the buffer it came from
    // (the parser's network buffer is reused for the next response).
    [[nodiscard]] std::optional<std::string_view> copy_string(std::string_view text) noexcept {
        const auto bytes = allocate(text.size(), 1);
        if (!bytes) {
            return std::nullopt;
        }
        if (!text.empty()) {
            std::memcpy(bytes->data(), text.data(), text.size());
        }
        return std::string_view(reinterpret_cast<const char*>(bytes->data()), text.size());
    }

    // Everything handed out so far is gone; the next allocation starts at the
    // beginning of the block again.
    void reset() noexcept { used_ = 0; }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t used() const noexcept { return used_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return capacity_ - used_; }

  private:
    LinearArena(std::unique_ptr<std::byte[]> block, std::size_t capacity) noexcept
        : block_(std::move(block)), capacity_(capacity) {}

    std::unique_ptr<std::byte[]> block_;
    std::size_t capacity_ = 0;
    std::size_t used_ = 0; // bytes handed out, padding included
};

} // namespace am::common
