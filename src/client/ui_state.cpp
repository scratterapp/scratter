#include "client/ui_state.hpp"

#include <utility>

namespace am::client {

namespace {

using namespace std::chrono_literals;

// How old the last good read may be before the numbers count as stale, and
// how long to wait before trying to open a page that wasn't there.
constexpr auto kStaleAfter = 1s;
constexpr auto kReopenAfter = 1s;

} // namespace

TelemetryReader::TelemetryReader(std::string daemon_name)
    : shm_name_(std::move(daemon_name) + "_telemetry_v2") {}

const common::TelemetrySnapshot& TelemetryReader::update(const DaemonState& state) {
    if (state.status != LinkStatus::Connected) {
        // The daemon is gone (or not ready). Its page may be too, or about to
        // be replaced by a new daemon's, so let go of it.
        mapping_ = {};
        open_failed_at_.reset();
        return last_;
    }
    if (mapped() && mapped_for_ != state.connection_count) {
        mapping_ = {}; // a new connection, so maybe a new daemon with a new page
    }
    if (!mapped()) {
        const auto now = Clock::now();
        if (open_failed_at_ && now - *open_failed_at_ < kReopenAfter) {
            return last_;
        }
        auto opened = pal::ShmMapping::open_read_only(shm_name_, common::kTelemetryPageSize);
        if (!opened) {
            open_failed_at_ = now;
            return last_;
        }
        mapping_ = std::move(*opened);
        mapped_for_ = state.connection_count;
        open_failed_at_.reset();
    }

    // The daemon built a TelemetryPage at the start of the region, as amctl
    // status reads it.
    const auto& page = *reinterpret_cast<const common::TelemetryPage*>(mapping_.bytes().data());
    if (auto snapshot = page.try_read()) {
        last_ = *snapshot;
        last_good_at_ = Clock::now();
    }
    return last_;
}

bool TelemetryReader::fresh() const {
    return mapped() && last_good_at_ && Clock::now() - *last_good_at_ < kStaleAfter;
}

} // namespace am::client
