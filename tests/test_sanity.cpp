// Proves the test framework and sanitizer builds are wired up. Real tests live
// in the other test_*.cpp files.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <numeric>
#include <thread>

TEST_CASE("test framework runs", "[sanity]") {
    std::array<int, 4> values{1, 2, 3, 4};
    REQUIRE(std::accumulate(values.begin(), values.end(), 0) == 10);
}

TEST_CASE("two threads can share an atomic counter", "[sanity]") {
    // Exercises the thread runtime so linux-tsan has something to watch.
    std::atomic<int> counter{0};
    std::thread worker([&counter] {
        for (int i = 0; i < 1000; ++i) {
            counter.fetch_add(1, std::memory_order_relaxed);
        }
    });
    for (int i = 0; i < 1000; ++i) {
        counter.fetch_add(1, std::memory_order_relaxed);
    }
    worker.join();
    REQUIRE(counter.load() == 2000);
}
