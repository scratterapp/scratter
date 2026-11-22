// Tests for the linear arena (src/common/arena_allocator.hpp): what the
// catalog parser (#105) copies a view's data into, and throws away at once.

#include "common/arena_allocator.hpp"

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

using am::common::LinearArena;

std::uintptr_t address(const void* p) { return reinterpret_cast<std::uintptr_t>(p); }

// A catalog-like record: plain data only, as the arena requires.
struct Track {
    std::uint64_t id = 0;
    std::uint32_t duration_ms = 0;
    std::string_view title; // points into the same arena
};

} // namespace

TEST_CASE("create() refuses an empty arena", "[arena]") {
    const auto arena = LinearArena::create(0);
    REQUIRE_FALSE(arena.has_value());
    CHECK(arena.error() == std::errc::invalid_argument);
}

TEST_CASE("a new arena is empty and as big as asked", "[arena]") {
    auto arena = LinearArena::create(1024);
    REQUIRE(arena.has_value());
    CHECK(arena->capacity() == 1024);
    CHECK(arena->used() == 0);
    CHECK(arena->remaining() == 1024);
}

TEST_CASE("allocate() hands out aligned, non-overlapping slices", "[arena]") {
    auto arena = LinearArena::create(4096);
    REQUIRE(arena.has_value());
    for (std::size_t align = 1; align <= 64; align *= 2) {
        INFO("alignment " << align);
        // An odd-sized slice first, so the next one has to be padded.
        const auto odd = arena->allocate(3, 1);
        REQUIRE(odd.has_value());
        const auto slice = arena->allocate(10, align);
        REQUIRE(slice.has_value());
        CHECK(slice->size() == 10);
        CHECK(address(slice->data()) % align == 0);
        CHECK(address(slice->data()) >= address(odd->data()) + odd->size());
    }
}

TEST_CASE("allocate() refuses an alignment that isn't a power of two", "[arena]") {
    auto arena = LinearArena::create(256);
    REQUIRE(arena.has_value());
    CHECK_FALSE(arena->allocate(8, 0).has_value());
    CHECK_FALSE(arena->allocate(8, 3).has_value());
    CHECK_FALSE(arena->allocate(8, 24).has_value());
    CHECK(arena->used() == 0); // and takes nothing for it
}

TEST_CASE("the arena fills to exactly full, and not a byte more", "[arena]") {
    auto arena = LinearArena::create(100);
    REQUIRE(arena.has_value());
    REQUIRE(arena->allocate(60, 1).has_value());
    CHECK_FALSE(arena->allocate(41, 1).has_value()); // one over
    CHECK(arena->used() == 60);                      // a refusal takes nothing
    REQUIRE(arena->allocate(40, 1).has_value());     // exactly the rest
    CHECK(arena->remaining() == 0);
    CHECK_FALSE(arena->allocate(1, 1).has_value());
}

TEST_CASE("a size that would overflow is refused, not wrapped", "[arena]") {
    auto arena = LinearArena::create(64);
    REQUIRE(arena.has_value());
    REQUIRE(arena->allocate(1, 1).has_value());
    CHECK_FALSE(arena->allocate(SIZE_MAX, 1).has_value());
    CHECK_FALSE(arena->allocate(SIZE_MAX - 8, 16).has_value());
    CHECK_FALSE(arena->make_array<std::uint64_t>(SIZE_MAX / 4).has_value());
}

TEST_CASE("reset() empties it and hands out the same memory again", "[arena]") {
    auto arena = LinearArena::create(512);
    REQUIRE(arena.has_value());
    const auto first = arena->allocate(100, 16);
    REQUIRE(first.has_value());
    REQUIRE(arena->allocate(200, 8).has_value());

    arena->reset();
    CHECK(arena->used() == 0);
    const auto again = arena->allocate(100, 16);
    REQUIRE(again.has_value());
    CHECK(again->data() == first->data());
}

TEST_CASE("copy_string() copies the text in, and empty text too", "[arena]") {
    auto arena = LinearArena::create(256);
    REQUIRE(arena.has_value());
    std::string source = "Tears of Steel";
    const auto copy = arena->copy_string(source);
    REQUIRE(copy.has_value());
    source = "something else"; // the copy doesn't follow the original
    CHECK(*copy == "Tears of Steel");

    const auto empty = arena->copy_string("");
    REQUIRE(empty.has_value());
    CHECK(empty->empty());

    CHECK_FALSE(arena->copy_string(std::string(300, 'x')).has_value());
}

TEST_CASE("make_array() gives value-initialized, aligned elements", "[arena]") {
    auto arena = LinearArena::create(1024);
    REQUIRE(arena.has_value());
    REQUIRE(arena->allocate(1, 1).has_value()); // knock the next one off alignment
    const auto tracks = arena->make_array<Track>(5);
    REQUIRE(tracks.has_value());
    REQUIRE(tracks->size() == 5);
    CHECK(address(tracks->data()) % alignof(Track) == 0);
    for (const Track& t : *tracks) {
        CHECK(t.id == 0);
        CHECK(t.duration_ms == 0);
        CHECK(t.title.empty());
    }

    // A view built in place: the records and their strings in one arena.
    const auto title = arena->copy_string("Crossing the Line");
    REQUIRE(title.has_value());
    (*tracks)[2] = Track{.id = 1440, .duration_ms = 215000, .title = *title};
    CHECK((*tracks)[2].title == "Crossing the Line");

    const auto none = arena->make_array<Track>(0);
    REQUIRE(none.has_value());
    CHECK(none->empty());
}

TEST_CASE("make() builds one object, or says there's no room", "[arena]") {
    auto arena = LinearArena::create(sizeof(Track) + alignof(Track));
    REQUIRE(arena.has_value());
    Track* t = arena->make<Track>(Track{.id = 7, .duration_ms = 1000, .title = {}});
    REQUIRE(t != nullptr);
    CHECK(t->id == 7);
    CHECK(arena->make<Track>() == nullptr);
}

TEST_CASE("an arena can be moved, and the moved-from one is empty", "[arena]") {
    auto arena = LinearArena::create(128);
    REQUIRE(arena.has_value());
    const auto slice = arena->allocate(16, 8);
    REQUIRE(slice.has_value());

    LinearArena moved = std::move(*arena);
    CHECK(moved.capacity() == 128);
    CHECK(moved.used() == slice->size());
    // Using it after the move is the point here: checking what a move leaves behind.
    // NOLINTBEGIN(bugprone-use-after-move)
    CHECK(arena->capacity() == 0);
    CHECK_FALSE(arena->allocate(1, 1).has_value());
    // NOLINTEND(bugprone-use-after-move)

    LinearArena other = std::move(moved);
    moved = std::move(other); // move-assign back
    CHECK(moved.capacity() == 128);
}
