// Tests for ShmMapping (src/pal/interfaces/shm_mapping.hpp).
//
// The single-process tests check create, open, move and cleanup. The last test
// is the issue #7 gate: this process writes through the SeqLock while a second
// process (shm_reader_child) maps the page read-only and checks every read.

#include "common/telemetry_shm.hpp"
#include "shm_mapping.hpp"
#include "telemetry_stamp.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace {

using am::common::kTelemetryPageSize;
using am::common::TelemetryPage;
using am::pal::ShmMapping;

// Shared-memory names are global to the machine, so each test uses its own
// random one. That keeps parallel ctest runs (and a stale name from a crashed
// run) from interfering.
std::string unique_name() {
    std::random_device rd;
    return "am_test_" + std::to_string(rd()) + "_" + std::to_string(rd());
}

} // namespace

TEST_CASE("create maps a zeroed, writable region", "[shm]") {
    const std::string name = unique_name();
    auto m = ShmMapping::create(name, kTelemetryPageSize);
    REQUIRE(m.has_value());

    CHECK(m->size() == kTelemetryPageSize);
    CHECK(m->writable());
    REQUIRE(m->writable_bytes().size() == kTelemetryPageSize);
    for (std::byte b : m->bytes()) {
        CHECK(b == std::byte{0});
    }
}

TEST_CASE("a read-only open sees what the creator wrote", "[shm]") {
    const std::string name = unique_name();
    auto writer = ShmMapping::create(name, kTelemetryPageSize);
    REQUIRE(writer.has_value());
    auto reader = ShmMapping::open_read_only(name, kTelemetryPageSize);
    REQUIRE(reader.has_value());

    CHECK_FALSE(reader->writable());
    CHECK(reader->writable_bytes().empty());

    // Two mappings of the same region are two different addresses for the
    // same memory.
    CHECK(reader->bytes().data() != writer->bytes().data());
    writer->writable_bytes()[100] = std::byte{0x5A};
    CHECK(reader->bytes()[100] == std::byte{0x5A});
}

TEST_CASE("open_read_only reports a missing region", "[shm]") {
    auto m = ShmMapping::open_read_only(unique_name(), kTelemetryPageSize);
    REQUIRE_FALSE(m.has_value());
    CHECK(m.error() == std::errc::no_such_file_or_directory);
}

TEST_CASE("open_read_only refuses a region smaller than asked for", "[shm]") {
    const std::string name = unique_name();
    auto writer = ShmMapping::create(name, kTelemetryPageSize);
    REQUIRE(writer.has_value());

    auto reader = ShmMapping::open_read_only(name, 2 * kTelemetryPageSize);
    REQUIRE_FALSE(reader.has_value());
    CHECK(reader.error() == std::errc::result_out_of_range);
}

TEST_CASE("bad names and sizes are rejected", "[shm]") {
    CHECK(ShmMapping::create("", kTelemetryPageSize).error() == std::errc::invalid_argument);
    CHECK(ShmMapping::create("has/slash", kTelemetryPageSize).error() ==
          std::errc::invalid_argument);
    CHECK(ShmMapping::create(unique_name(), 0).error() == std::errc::invalid_argument);
    CHECK(ShmMapping::open_read_only("", kTelemetryPageSize).error() ==
          std::errc::invalid_argument);
}

TEST_CASE("destroying the creator removes the name", "[shm]") {
    const std::string name = unique_name();
    auto reader = [&] {
        auto writer = ShmMapping::create(name, kTelemetryPageSize);
        REQUIRE(writer.has_value());
        writer->writable_bytes()[0] = std::byte{7};
        return ShmMapping::open_read_only(name, kTelemetryPageSize);
    }(); // writer is destroyed here

    // A reader that was already open keeps its memory...
    REQUIRE(reader.has_value());
    CHECK(reader->bytes()[0] == std::byte{7});

    // ...but nobody new can find it.
    auto late = ShmMapping::open_read_only(name, kTelemetryPageSize);
    REQUIRE_FALSE(late.has_value());
    CHECK(late.error() == std::errc::no_such_file_or_directory);
}

TEST_CASE("moving a mapping leaves the source empty", "[shm]") {
    const std::string name = unique_name();
    auto m = ShmMapping::create(name, kTelemetryPageSize);
    REQUIRE(m.has_value());
    const std::byte* data = m->bytes().data();

    ShmMapping moved = std::move(*m);
    CHECK(moved.bytes().data() == data);
    CHECK(moved.writable());
    CHECK(m->size() == 0);
    CHECK(m->bytes().empty());

    // Move assignment releases what the target held. Afterwards the name is
    // gone, because the mapping that owned it was replaced.
    moved = ShmMapping{};
    CHECK(moved.size() == 0);
    CHECK(ShmMapping::open_read_only(name, kTelemetryPageSize).error() ==
          std::errc::no_such_file_or_directory);
}

TEST_CASE("another process reads the SeqLock page tear-free", "[shm][stress]") {
    const std::string name = unique_name();
    auto mapping = ShmMapping::create(name, kTelemetryPageSize);
    REQUIRE(mapping.has_value());

    // Build the page inside the shared memory. The daemon will do the same.
    TelemetryPage* page =
        std::construct_at(reinterpret_cast<TelemetryPage*>(mapping->writable_bytes().data()));

    std::atomic<bool> stop{false};
    // The writer yields after each write. Writing back to back with no gap
    // keeps the counter odd nearly all the time, and on a slow CI machine the
    // other process then almost never gets a clean read. The in-process test
    // in test_telemetry_seqlock.cpp already covers that worst case.
    std::thread writer([page, &stop] {
        std::uint64_t n = 1;
        while (!stop.load(std::memory_order_relaxed)) {
            page->write(am::test::stamped(n++));
            std::this_thread::yield();
        }
    });

    // The child exits 0 once it has read a thousand different snapshots and
    // none of them was torn. See shm_reader_child.cpp for the other codes.
    const std::string command = std::string("\"") + AM_SHM_READER_CHILD + "\" " + name;
    const int status = std::system(command.c_str());

    stop.store(true, std::memory_order_relaxed);
    writer.join();
    std::destroy_at(page);

    CHECK(status == 0);
}
