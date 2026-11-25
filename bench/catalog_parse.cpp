// The catalog parser's gate (spec section 6, phase 4; issue #105): a
// 500-track playlist parses in under 1.5 ms with no heap allocation outside
// the arena.
//
//   ./build/linux-release/bench/catalog_parse [parses]
//
// It makes the playlist (tests/catalog_samples.hpp, about 470 KB, each track
// with the attributes a real response has), then parses it again and again
// into one arena, resetting it between parses the way a view does. It prints
// the median and the slowest parse, and how many times anything called
// operator new during the parses: this file replaces the global operator new
// to count, and simdjson and the arena both allocate through it.
//
// The gate is for an optimized build (linux-release): a debug build is
// several times slower and prints a FAIL that means nothing. It exits with 1
// if the median is over budget or anything allocated.

#include "net/catalog_parser.hpp"

#include "catalog_samples.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

namespace {

// Counts every operator new while `counting` is on. Plain globals, no locks:
// the benchmark has one thread.
std::atomic<bool> counting{false};
std::atomic<long> allocations{0};

void* counted_new(std::size_t size) noexcept {
    if (counting.load(std::memory_order_relaxed)) {
        allocations.fetch_add(1, std::memory_order_relaxed);
    }
    return std::malloc(size == 0 ? 1 : size);
}

using Clock = std::chrono::steady_clock;
constexpr std::size_t kTracks = 500;
constexpr double kBudgetMillis = 1.5;

} // namespace

// The replaced allocation functions. Exceptions are off, so the throwing
// forms can't throw either; they abort if there's no memory, as libstdc++'s
// do with -fno-exceptions.
void* operator new(std::size_t size) {
    void* p = counted_new(size);
    if (p == nullptr) {
        std::abort();
    }
    return p;
}
void* operator new[](std::size_t size) {
    void* p = counted_new(size);
    if (p == nullptr) {
        std::abort();
    }
    return p;
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return counted_new(size); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return counted_new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

int main(int argc, char** argv) {
    const long parses = argc > 1 ? std::atol(argv[1]) : 2000;
    if (parses <= 0) {
        std::fprintf(stderr, "usage: %s [parses]\n", argv[0]);
        return 2;
    }

    const std::string json = am::test::make_playlist_json(kTracks);
    const am::test::PaddedBody body(json);
    auto parser = am::net::CatalogParser::create();
    auto arena = am::common::LinearArena::create(std::size_t{1} << 20);
    if (!parser || !arena) {
        std::fprintf(stderr, "couldn't make the parser or the arena\n");
        return 1;
    }

    // One parse first, so nothing measured below is a first-time cost.
    const auto first = parser->parse_playlist(body.span(), *arena);
    if (!first || first->tracks.size() != kTracks) {
        std::fprintf(stderr, "the playlist didn't parse\n");
        return 1;
    }
    const std::size_t arena_bytes = arena->used();

    // Check the counting itself works, so a 0 below means something.
    counting.store(true);
    ::operator delete(::operator new(16));
    counting.store(false);
    if (allocations.exchange(0) != 1) {
        std::fprintf(stderr, "the allocation counter doesn't count\n");
        return 1;
    }

    std::vector<double> millis(static_cast<std::size_t>(parses));
    std::size_t tracks = 0; // printed, so the parses can't be optimized away
    counting.store(true);
    for (double& took : millis) {
        arena->reset();
        const auto started = Clock::now();
        const auto playlist = parser->parse_playlist(body.span(), *arena);
        took = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        tracks += playlist ? playlist->tracks.size() : 0;
    }
    counting.store(false);

    std::ranges::sort(millis);
    const double median = millis[millis.size() / 2];
    const double slowest = millis.back();
    const double megabytes_per_second = static_cast<double>(json.size()) / (median * 1000.0);
    std::printf("%zu tracks, %zu KB of JSON, %zu KB in the arena\n", kTracks, json.size() / 1024,
                arena_bytes / 1024);
    std::printf("%ld parses: median %.3f ms (%.0f MB/s), slowest %.3f ms\n", parses, median,
                megabytes_per_second, slowest);
    std::printf("allocations during the parses: %ld\n", allocations.load());
    std::printf("tracks read: %zu\n", tracks);

    const bool fast = median < kBudgetMillis;
    const bool no_heap = allocations.load() == 0;
    std::printf("%s: median under %.1f ms %s, no allocation outside the arena %s\n",
                fast && no_heap ? "PASS" : "FAIL", kBudgetMillis, fast ? "yes" : "NO",
                no_heap ? "yes" : "NO");
    return fast && no_heap ? 0 : 1;
}
