// Child process for the two-process test in test_shm_mapping.cpp.
//
//   shm_reader_child <region name>
//
// Opens the region read-only, treats it as a TelemetryPage, and reads it in a
// loop while the parent process writes. Exit codes:
//   0  saw kWantDistinct different snapshots, none of them torn
//   1  bad arguments
//   2  couldn't open the region
//   3  read a torn snapshot
//   4  timed out before seeing enough different snapshots

#include "common/telemetry_shm.hpp"
#include "shm_mapping.hpp"
#include "telemetry_stamp.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: shm_reader_child <region name>\n");
        return 1;
    }

    auto mapping = am::pal::ShmMapping::open_read_only(argv[1], am::common::kTelemetryPageSize);
    if (!mapping) {
        std::fprintf(stderr, "open_read_only failed: %s\n", mapping.error().message().c_str());
        return 2;
    }

    // The parent constructed a TelemetryPage at the start of the region. This
    // process only ever reads it, through a const pointer.
    const auto* page = reinterpret_cast<const am::common::TelemetryPage*>(mapping->bytes().data());

    constexpr std::uint64_t kWantDistinct = 1'000;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);

    std::uint64_t distinct = 0;
    std::uint64_t previous = 0;
    while (distinct < kWantDistinct) {
        const am::common::TelemetrySnapshot s = page->read();
        if (!am::test::is_consistent(s)) {
            std::fprintf(stderr, "torn snapshot near write %llu\n",
                         static_cast<unsigned long long>(s.track_catalog_id));
            return 3;
        }
        if (s.track_catalog_id != previous) {
            ++distinct;
            previous = s.track_catalog_id;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            std::fprintf(stderr, "only saw %llu different snapshots\n",
                         static_cast<unsigned long long>(distinct));
            return 4;
        }
    }
    return 0;
}
